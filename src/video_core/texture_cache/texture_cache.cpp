// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <xxhash.h>

#include <cstdlib>
#include <cstring>
#include <span>

#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/scope_exit.h"
#include "common/trace_control.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/host_diagnostics.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_gpu_command_diagnostics.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u64 NumFramesBeforeRemoval = 32;

static u64 ImageFreeExtraSubmits() {
    static const u64 extra_submits = [] {
        if (const char* value = std::getenv("SHADPS4_IMAGE_FREE_EXTRA_SUBMITS")) {
            return std::strtoull(value, nullptr, 10);
        }
        return 2ull;
    }();
    return extra_submits;
}

static bool IsStrictRenderValidationEnabled() {
    static const bool enabled = Common::Trace::EnvEnabled("SHADPS4_STRICT_RENDER_VALIDATION");
    return enabled;
}

static bool ShouldAbortAmbiguousImageRangeLookup() {
    static const bool enabled =
        Common::Trace::EnvEnabled("SHADPS4_STRICT_AMBIGUOUS_IMAGE_RANGE_ABORT");
    return IsStrictRenderValidationEnabled() && enabled;
}

static bool ShouldAbortNullGuestImageDescriptor() {
    static const bool enabled =
        Common::Trace::EnvEnabled("SHADPS4_STRICT_NULL_GUEST_IMAGE_DESCRIPTOR_ABORT");
    return IsStrictRenderValidationEnabled() && enabled;
}

static bool IsDepthFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eX8D24UnormPack32:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
        return true;
    default:
        return false;
    }
}

static bool AreImageTypesViewCompatible(AmdGpu::ImageType requested_type,
                                        AmdGpu::ImageType cached_type) {
    switch (requested_type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return cached_type == AmdGpu::ImageType::Color1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DMsaaArray:
        return cached_type == AmdGpu::ImageType::Color2D || cached_type == AmdGpu::ImageType::Color3D;
    case AmdGpu::ImageType::Color3D:
        return cached_type == AmdGpu::ImageType::Color3D;
    default:
        return requested_type == cached_type;
    }
}

static bool HasStencil(vk::Format format) {
    switch (format) {
    case vk::Format::eS8Uint:
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
        return true;
    default:
        return false;
    }
}

static bool CanCreateDepthStencilViewAs(const ImageInfo& image_info, vk::Format view_format) {
    if (!image_info.props.is_depth) {
        return true;
    }

    if (IsDepthFormat(image_info.pixel_format) &&
        Vulkan::LiverpoolToVK::IsFormatDepthCompatible(view_format)) {
        return true;
    }

    return HasStencil(image_info.pixel_format) &&
           Vulkan::LiverpoolToVK::IsFormatStencilCompatible(view_format);
}

static bool IsBlockFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eBc1RgbUnormBlock:
    case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc1RgbaUnormBlock:
    case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc2UnormBlock:
    case vk::Format::eBc2SrgbBlock:
    case vk::Format::eBc3UnormBlock:
    case vk::Format::eBc3SrgbBlock:
    case vk::Format::eBc4UnormBlock:
    case vk::Format::eBc4SnormBlock:
    case vk::Format::eBc5UnormBlock:
    case vk::Format::eBc5SnormBlock:
    case vk::Format::eBc6HUfloatBlock:
    case vk::Format::eBc6HSfloatBlock:
    case vk::Format::eBc7UnormBlock:
    case vk::Format::eBc7SrgbBlock:
        return true;
    default:
        return false;
    }
}

static u32 NullImageBitsPerBlock(vk::Format format) {
    switch (format) {
    case vk::Format::eR8Unorm:
    case vk::Format::eR8Snorm:
    case vk::Format::eR8Uint:
    case vk::Format::eR8Sint:
    case vk::Format::eS8Uint:
        return 8;
    case vk::Format::eR4G4B4A4UnormPack16:
    case vk::Format::eB4G4R4A4UnormPack16:
    case vk::Format::eR5G6B5UnormPack16:
    case vk::Format::eB5G6R5UnormPack16:
    case vk::Format::eA1R5G5B5UnormPack16:
    case vk::Format::eR8G8Unorm:
    case vk::Format::eR8G8Snorm:
    case vk::Format::eR8G8Uint:
    case vk::Format::eR8G8Sint:
    case vk::Format::eR16Unorm:
    case vk::Format::eR16Snorm:
    case vk::Format::eR16Uint:
    case vk::Format::eR16Sint:
    case vk::Format::eR16Sfloat:
    case vk::Format::eD16Unorm:
        return 16;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eA2B10G10R10UnormPack32:
    case vk::Format::eA2R10G10B10UnormPack32:
    case vk::Format::eR16G16Unorm:
    case vk::Format::eR16G16Snorm:
    case vk::Format::eR16G16Uint:
    case vk::Format::eR16G16Sint:
    case vk::Format::eR16G16Sfloat:
    case vk::Format::eR32Uint:
    case vk::Format::eR32Sint:
    case vk::Format::eR32Sfloat:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eX8D24UnormPack32:
        return 32;
    case vk::Format::eR16G16B16A16Unorm:
    case vk::Format::eR16G16B16A16Snorm:
    case vk::Format::eR16G16B16A16Uint:
    case vk::Format::eR16G16B16A16Sint:
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eR32G32Uint:
    case vk::Format::eR32G32Sint:
    case vk::Format::eR32G32Sfloat:
    case vk::Format::eBc1RgbUnormBlock:
    case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc1RgbaUnormBlock:
    case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc4UnormBlock:
    case vk::Format::eBc4SnormBlock:
    case vk::Format::eD32SfloatS8Uint:
        return 64;
    case vk::Format::eR32G32B32A32Uint:
    case vk::Format::eR32G32B32A32Sint:
    case vk::Format::eR32G32B32A32Sfloat:
    case vk::Format::eBc2UnormBlock:
    case vk::Format::eBc2SrgbBlock:
    case vk::Format::eBc3UnormBlock:
    case vk::Format::eBc3SrgbBlock:
    case vk::Format::eBc5UnormBlock:
    case vk::Format::eBc5SnormBlock:
    case vk::Format::eBc6HUfloatBlock:
    case vk::Format::eBc6HSfloatBlock:
    case vk::Format::eBc7UnormBlock:
    case vk::Format::eBc7SrgbBlock:
        return 128;
    default:
        return 32;
    }
}

// Diagnostic: verify buffer->image copy regions won't read past the source buffer. Mirrors
// Metal's copyBufferToImage source-size requirement on the CPU, so an over-read (e.g. when a guest
// format is substituted for a larger host one, like D16UnormS8Uint -> D32SfloatS8Uint) is caught
// at the recording site with full context, instead of surfacing later as an asynchronous,
// hard-to-attribute GPU device loss (kIOGPUCommandBufferCallbackErrorInvalidResource). Pure check:
// it never alters the copy. Always logs a warning; asserts only under strict render validation.
// Returns true if any region would over-read.
static bool ValidateBufferToImageBounds(const char* site, const ImageInfo& info,
                                        vk::Format host_format,
                                        std::span<const vk::BufferImageCopy> copies,
                                        u64 src_buffer_size) {
    // Bytes a single texel/block contributes for the copy's aspect under the HOST format. Depth/
    // stencil formats are PLANAR: a depth-aspect copy moves only the depth plane (D16=2, D24/D32=4
    // bytes), a stencil-aspect copy only the 1-byte stencil plane. Using the combined depth+stencil
    // size here would double-count and false-positive (e.g. D32SfloatS8Uint depth copies). Color/
    // block formats use their full block size.
    const auto aspect_bytes = [](vk::Format fmt, vk::ImageAspectFlags aspect) -> u64 {
        const bool depth = bool(aspect & vk::ImageAspectFlagBits::eDepth);
        const bool stencil = bool(aspect & vk::ImageAspectFlagBits::eStencil);
        if (stencil && !depth) {
            return 1; // stencil plane only
        }
        if (depth) {
            switch (fmt) {
            case vk::Format::eD16Unorm:
            case vk::Format::eD16UnormS8Uint:
                return 2;
            case vk::Format::eD32Sfloat:
            case vk::Format::eD32SfloatS8Uint:
            case vk::Format::eD24UnormS8Uint:
            case vk::Format::eX8D24UnormPack32:
                return 4;
            default:
                break;
            }
        }
        return std::max<u64>(NullImageBitsPerBlock(fmt) / 8u, 1u);
    };
    // For block-compressed formats the copy's row length / extent are in TEXELS but the buffer is
    // addressed in BLOCKS (a 4x4 texel block), so divide by the block dimensions. Non-block and
    // depth/stencil formats use a 1x1 block.
    const u32 block_dim = IsBlockFormat(host_format) ? 4u : 1u;
    bool over_read = false;
    for (const auto& copy : copies) {
        const u64 texel_bytes =
            aspect_bytes(host_format, copy.imageSubresource.aspectMask);
        const u64 row_texels =
            copy.bufferRowLength ? copy.bufferRowLength : copy.imageExtent.width;
        const u64 rows = copy.bufferImageHeight ? copy.bufferImageHeight : copy.imageExtent.height;
        const u64 slices = std::max<u32>(copy.imageExtent.depth, 1u) *
                           std::max<u32>(copy.imageSubresource.layerCount, 1u);
        const u64 blocks_per_row = (row_texels + block_dim - 1) / block_dim;
        const u64 block_rows = (rows + block_dim - 1) / block_dim;
        const u64 required = static_cast<u64>(copy.bufferOffset) +
                             texel_bytes * blocks_per_row * block_rows * slices;
        if (required > src_buffer_size) {
            over_read = true;
            LOG_WARNING(Render_Vulkan,
                        "[{}] buffer->image copy over-reads source buffer: addr={:#x} "
                        "guest_format={} host_format={} required={} src_buffer_size={} "
                        "bufferOffset={} row_texels={} rows={} slices={} texel_bytes={} aspect={} "
                        "is_tiled={} is_depth={}",
                        site, info.guest_address, vk::to_string(info.pixel_format),
                        vk::to_string(host_format), required, src_buffer_size,
                        static_cast<u64>(copy.bufferOffset), row_texels, rows, slices, texel_bytes,
                        vk::to_string(copy.imageSubresource.aspectMask), info.props.is_tiled,
                        info.props.is_depth);
            ASSERT_MSG(!IsStrictRenderValidationEnabled(),
                       "Strict render validation: [{}] buffer->image copy over-reads source buffer "
                       "addr={:#x} required={} src_buffer_size={} host_format={}",
                       site, info.guest_address, required, src_buffer_size,
                       vk::to_string(host_format));
        }
    }
    return over_read;
}

static bool IsTraceMetaDataRegisterEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_TRACE_METADATA_REGISTER");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static const char* MetaTypeName(TextureCache::MetaDataInfo::Type type) {
    switch (type) {
    case TextureCache::MetaDataInfo::Type::CMask:
        return "cmask";
    case TextureCache::MetaDataInfo::Type::FMask:
        return "fmask";
    case TextureCache::MetaDataInfo::Type::HTile:
        return "htile";
    }
    return "unknown";
}

static const char* BindingTypeName(TextureCache::BindingType type) {
    switch (type) {
    case TextureCache::BindingType::Texture:
        return "texture";
    case TextureCache::BindingType::Storage:
        return "storage";
    case TextureCache::BindingType::RenderTarget:
        return "render_target";
    case TextureCache::BindingType::DepthTarget:
        return "depth_target";
    case TextureCache::BindingType::VideoOut:
        return "video_out";
    }
    return "unknown";
}

static TextureCache::MetaDataInfo MakeMetaDataInfo(TextureCache::MetaDataInfo::Type type,
                                                   const Image& image, ImageId image_id,
                                                   TextureCache::BindingType binding,
                                                   s32 clear_mask = -1) {
    return TextureCache::MetaDataInfo{
        .type = type,
        .clear_mask = clear_mask,
        .owner_image_id = image_id,
        .owner_binding = binding,
        .owner_guest_address = image.info.guest_address,
        .owner_guest_size = image.info.guest_size,
        .owner_size = image.info.size,
        .owner_pitch = image.info.pitch,
        .owner_format = image.info.pixel_format,
        .owner_num_bits = image.info.num_bits,
        .owner_num_samples = image.info.num_samples,
        .owner_tile_mode = image.info.tile_mode,
        .owner_array_mode = image.info.array_mode,
    };
}

static void LogMetaDataRegistration(VAddr address, const TextureCache::MetaDataInfo& meta,
                                    const char* action) {
    if (!IsTraceMetaDataRegisterEnabled()) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "TRACE_RENDER metadata_register addr={:#x} kind={} action={} owner_image={} "
             "owner_binding={} guest_addr={:#x} guest_size={} size={}x{}x{} pitch={} "
             "vk_format={} tile_mode={} array_mode={} bits={} samples={} clear_mask={:#x}",
             address, MetaTypeName(meta.type), action, meta.owner_image_id.index,
             BindingTypeName(meta.owner_binding), meta.owner_guest_address, meta.owner_guest_size,
             meta.owner_size.width,
             meta.owner_size.height, meta.owner_size.depth, meta.owner_pitch,
             static_cast<u32>(meta.owner_format), static_cast<u32>(meta.owner_tile_mode),
             static_cast<u32>(meta.owner_array_mode), meta.owner_num_bits, meta.owner_num_samples,
             static_cast<u32>(meta.clear_mask));
}

static void RegisterSurfaceMeta(tsl::robin_map<VAddr, TextureCache::MetaDataInfo>& surface_metas,
                                VAddr address, TextureCache::MetaDataInfo meta) {
    auto it = surface_metas.find(address);
    if (it == surface_metas.end()) {
        surface_metas.emplace(address, meta);
        LogMetaDataRegistration(address, meta, "insert");
        return;
    }

    if (it.value().owner_image_id.index == meta.owner_image_id.index && it.value().type == meta.type) {
        meta.clear_mask = it.value().clear_mask;
    }
    it.value() = meta;
    LogMetaDataRegistration(address, meta, "update");
}

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           AmdGpu::Liverpool* liverpool_, BufferCache& buffer_cache_,
                           PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, buffer_cache.GetUtilityBuffer(MemoryUsage::Stream)},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {
    // Create basic null image at fixed image ID.
    const auto null_id = GetNullImage(vk::Format::eR8G8B8A8Unorm);
    ASSERT(null_id.index == NULL_IMAGE_ID.index);

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    pressure_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_PRESSURE_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

TextureCache::~TextureCache() = default;

ImageId TextureCache::GetNullImage(const vk::Format format) {
    const auto existing_image = null_images.find(format);
    if (existing_image != null_images.end()) {
        return existing_image->second;
    }

    ImageInfo info{};
    info.pixel_format = format;
    info.type = AmdGpu::ImageType::Color2D;
    info.tile_mode = AmdGpu::TileMode::Thin1DThin;
    info.props.is_depth = IsDepthFormat(format);
    info.props.has_stencil = HasStencil(format);
    info.props.is_block = IsBlockFormat(format);
    info.num_bits = NullImageBitsPerBlock(format);
    info.UpdateSize();

    const ImageId null_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
    auto& image = slot_images[null_id];
    Vulkan::SetObjectName(instance.GetDevice(), image.GetImage(),
                          fmt::format("Null Image ({})", vk::to_string(format)));

    image.flags = ImageFlagBits::Empty;
    image.track_addr = image.info.guest_address;
    image.track_addr_end = image.info.guest_address + image.info.guest_size;

    null_images.emplace(format, null_id);
    return null_id;
}

void TextureCache::ProcessDownloadImages() {
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id);
    }
    download_images.clear();
}

void TextureCache::DownloadImageMemory(ImageId image_id) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    auto& download_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
    const u32 download_size = image.info.pitch * image.info.size.height *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    const auto [download, offset] = download_buffer.Map(download_size);
    download_buffer.Commit();
    const vk::BufferImageCopy image_download = {
        .bufferOffset = offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, 1},
    };
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    cmdbuf.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                             download_buffer.Handle(), image_download);

    scheduler.DeferPriorityOperation(
        [this, device_addr = image.info.guest_address, download, download_size] {
            Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(device_addr), download,
                                                      download_size);
        });
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        // Initialize hash
        const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
        image.hash = XXH3_64bits(addr, image.info.guest_size);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    Common::Trace::RecordVideoOutWrite("InvalidateMemoryFromGPU", address, max_size);
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(address, max_size, [&](ImageId, Image& image) {
        // Ensure image is reuploaded when accessed again.
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        // The reverse: the guest samples a depth surface as a non-depth (e.g. R16) texture. A
        // non-depth view of a depth image is impossible, so recreate the surface in the requested
        // format (a 1-for-1 swap that frees the old depth image) instead of rejecting reuse, which
        // would leave the depth image registered and accumulate one image per bind (the churn that
        // lost the device in UFC 3 at the menu/movie transition).
        recreate |= cache_image.info.props.is_depth && !requested_info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        const bool depth_color_reinterpret =
            cache_image.info.props.is_depth != requested_info.props.is_depth;
        if (depth_color_reinterpret) {
            ImageId alias_image_id{};
            u64 smallest_alias_size = std::numeric_limits<u64>::max();
            ForEachImageInRegion(requested_info.guest_address, requested_info.guest_size,
                                 [&](ImageId image_id, Image& image) {
                                     if (image_id == cache_image_id ||
                                         image.info.guest_address != requested_info.guest_address ||
                                         image.info.guest_size < requested_info.guest_size ||
                                         image.info.size != requested_info.size ||
                                         image.info.resources < requested_info.resources ||
                                         image.info.pixel_format != requested_info.pixel_format ||
                                         image.info.type != requested_info.type ||
                                         image.info.props.is_depth != requested_info.props.is_depth) {
                                         return;
                                     }

                                     if (image.info.guest_size < smallest_alias_size) {
                                         alias_image_id = image_id;
                                         smallest_alias_size = image.info.guest_size;
                                     }
                                 });

            if (alias_image_id) {
                auto& alias_image = slot_images[alias_image_id];
                alias_image.tick_accessed_last = scheduler.CurrentTick();
                TouchImage(alias_image);
                VideoCore::Diag::ReportOnce(
                    fmt::format("depth_color_reuse_alias:{:#x}:{}",
                                requested_info.guest_address, alias_image_id.index),
                    fmt::format("Reusing depth/color alias image: addr={:#x} old_id={} "
                                "alias_id={} old_format={} alias_format={} guest_size={} "
                                "requested_size={}",
                                requested_info.guest_address, cache_image_id.index,
                                alias_image_id.index, vk::to_string(cache_image.info.pixel_format),
                                vk::to_string(alias_image.info.pixel_format),
                                alias_image.info.guest_size, requested_info.guest_size));
                return alias_image_id;
            }
        }

        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        VideoCore::Diag::NoteImageRecreate("ResolveDepthOverlap", new_info.guest_address,
                                           new_info.guest_size);
        const auto new_image_id =
            slot_images.insert(instance, scheduler, blit_helper, slot_image_views, new_info);
        RegisterImage(new_image_id);

        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        new_image.usage = cache_image.usage;
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        if (depth_color_reinterpret) {
            const auto old_htile = cache_image.info.meta_info.htile_addr;
            const auto new_htile = new_image.info.meta_info.htile_addr;
            const auto* old_meta = old_htile != 0 ? FindMetaData(old_htile) : nullptr;
            const auto* new_meta = new_htile != 0 ? FindMetaData(new_htile) : nullptr;
            VideoCore::Diag::ReportOnce(
                fmt::format("depth_color_recreate_meta:{:#x}:{}",
                            new_image.info.guest_address, new_image_id.index),
                fmt::format("Depth/color overlap recreate metadata state: addr={:#x} "
                            "old_id={} new_id={} old_depth={} new_depth={} old_format={} "
                            "new_format={} old_htile={:#x} new_htile={:#x} old_clear_mask={:#x} "
                            "new_clear_mask={:#x} old_meta_owner={} old_meta_mask={:#x} "
                            "new_meta_owner={} new_meta_mask={:#x}",
                            new_image.info.guest_address, cache_image_id.index, new_image_id.index,
                            static_cast<bool>(cache_image.info.props.is_depth),
                            static_cast<bool>(new_image.info.props.is_depth),
                            vk::to_string(cache_image.info.pixel_format),
                            vk::to_string(new_image.info.pixel_format), old_htile, new_htile,
                            cache_image.info.meta_info.htile_clear_mask,
                            new_image.info.meta_info.htile_clear_mask,
                            old_meta != nullptr ? old_meta->owner_image_id.index : 0,
                            old_meta != nullptr ? old_meta->clear_mask : -1,
                            new_meta != nullptr ? new_meta->owner_image_id.index : 0,
                            new_meta != nullptr ? new_meta->clear_mask : -1));
            if (new_meta != nullptr && new_meta->owner_image_id != cache_image_id &&
                new_meta->owner_image_id != new_image_id) {
                LOG_WARNING(Render_Vulkan,
                            "Depth/color overlap recreate found stale HTile owner: addr={:#x} "
                            "htile={:#x} owner={} old_id={} new_id={}",
                            new_image.info.guest_address, new_htile,
                            new_meta->owner_image_id.index, cache_image_id.index,
                            new_image_id.index);
            }
        }

        if (cache_image.info.num_samples == 1 && new_info.num_samples == 1) {
            // Perform depth<->color copy using the intermediate copy buffer.
            if (instance.IsMaintenance8Supported()) {
                new_image.CopyImage(cache_image);
            } else {
                const auto& copy_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
                new_image.CopyImageWithBuffer(cache_image, copy_buffer.Handle(), 0);
            }
        } else if (cache_image.info.num_samples == 1 && new_info.props.is_depth &&
                   new_info.num_samples > 1) {
            // Perform a rendering pass to transfer the channels of source as samples in dest.
            cache_image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
                                vk::AccessFlagBits2::eShaderRead, {});
            new_image.Transit(vk::ImageLayout::eDepthAttachmentOptimal,
                              vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {});
            blit_helper.ReinterpretColorAsMsDepth(
                new_info.size.width, new_info.size.height, new_info.num_samples,
                cache_image.info.pixel_format, new_info.pixel_format, cache_image.GetImage(),
                new_image.GetImage());
        } else if (cache_image.info.props.is_depth && !new_info.props.is_depth &&
                   cache_image.info.num_samples == new_info.num_samples &&
                   new_info.num_samples > 1) {
            // Reinterpret a multisampled depth surface as color by writing the depth plane into
            // the matching color attachment sample-by-sample.
            cache_image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
                                vk::AccessFlagBits2::eShaderRead, {});
            new_image.Transit(vk::ImageLayout::eColorAttachmentOptimal,
                              vk::AccessFlagBits2::eColorAttachmentWrite, {});
            blit_helper.ReinterpretMsDepthAsColor(
                new_info.size.width, new_info.size.height, new_info.num_samples,
                cache_image.info.pixel_format, new_info.pixel_format, cache_image.GetImage(),
                new_image.GetImage());
        } else {
            // #61: this path leaves new_image's contents uninitialized (no copy performed). It
            // fired 643x in one UFC 3 run; route through ReportOnce so it is rate-limited and
            // attributable per address rather than flooding.
            VideoCore::Diag::ReportOnce(
                fmt::format("depth_overlap_uninit:{:#x}", new_info.guest_address),
                fmt::format("Unimplemented depth overlap copy leaves image uninitialized: "
                            "addr={:#x} old_format={} new_format={} old_samples={} new_samples={}",
                            new_info.guest_address, vk::to_string(cache_image.info.pixel_format),
                            vk::to_string(new_info.pixel_format), cache_image.info.num_samples,
                            new_info.num_samples));
            LOG_WARNING(Render_Vulkan, "Unimplemented depth overlap copy");
            ASSERT_MSG(!IsStrictRenderValidationEnabled(),
                       "Strict render validation: unimplemented depth overlap copy requested "
                       "old_addr={:#x} old_size={} old_format={} old_samples={} "
                       "new_addr={:#x} new_size={} new_format={} new_samples={} binding={}",
                       cache_image.info.guest_address, cache_image.info.guest_size,
                       vk::to_string(cache_image.info.pixel_format), cache_image.info.num_samples,
                       new_info.guest_address, new_info.guest_size,
                       vk::to_string(new_info.pixel_format), new_info.num_samples,
                       BindingTypeName(binding));
        }

        if (depth_color_reinterpret) {
            VideoCore::Diag::ReportOnce(
                fmt::format("depth_color_keep_alias:{:#x}", new_info.guest_address),
                fmt::format("Keeping depth/color alias pair registered after recreate: "
                            "addr={:#x} old_id={} new_id={} old_format={} new_format={}",
                            new_info.guest_address, cache_image_id.index, new_image_id.index,
                            vk::to_string(cache_image.info.pixel_format),
                            vk::to_string(new_image.info.pixel_format)));
        } else {
            FreeImage(cache_image_id);
        }
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NumFramesBeforeRemoval;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // A non-depth Texture/Storage binding over a cached depth surface (a depth-as-texture
        // read) used to be rejected here, which left the old depth image registered and
        // accumulated a new image per bind — the aliasing/GC churn that lost the device in UFC 3
        // at the menu/movie transition (~1500 images at one address in a single frame). Instead,
        // fall through to ResolveDepthOverlap below: it RECREATES the surface in the requested
        // (non-depth) format and FREES the old depth image — a stable 1-for-1 swap (handled for
        // both Texture and Storage by the recreate conditions in the binding switch above). The
        // reinterpreted contents may be uninitialized (a possible visual artifact) but it neither
        // churns nor loses the device.

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (!AreImageTypesViewCompatible(image_info.type, cache_image.info.type)) {
            LOG_WARNING(Render_Vulkan,
                        "Avoiding incompatible image type reuse requested_addr={:#x} "
                        "requested_size={} requested_type={} cached_id={} cached_addr={:#x} "
                        "cached_size={} cached_type={} binding={}",
                        image_info.guest_address, image_info.guest_size,
                        magic_enum::enum_name(image_info.type), cache_image_id.index,
                        cache_image.info.guest_address, cache_image.info.guest_size,
                        magic_enum::enum_name(cache_image.info.type), BindingTypeName(binding));
            return {merged_image_id, -1, -1};
        }

        // Same base address, but the later descriptor exposes more mip/layer resources than the
        // render target image originally allocated. Expand now so existing contents are preserved
        // instead of falling through to the generic too-small-resource recovery path below.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources &&
            image_info.tile_mode == cache_image.info.tile_mode &&
            IsVulkanFormatCompatible(cache_image.info.pixel_format, image_info.pixel_format)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // Enhanced debug logging for unreachable case
        // Calculate expected size based on format and dimensions
        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  // Old image details
                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, +cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,

                  // New image details
                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,

                  // Comparison
                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  // Size ratios
                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  // Difference between actual and expected sizes with percentages
                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        UNREACHABLE_MSG("Encountered unresolvable image overlap with equal memory address.");
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // We need to have a larger, already allocated image to copy this one into
                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    merged_image.CopyMip(cache_image, mip, slice);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_image_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    new_image.CopyImage(src_image);

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);

    TrackImage(new_image_id);
    new_image.flags &= ~ImageFlagBits::Dirty;
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;

    if (info.guest_address == 0) [[unlikely]] {
        LOG_WARNING(Render_Vulkan,
                    "Strict render validation: image descriptor has null guest address type={} "
                    "format={} size={}x{}x{} pitch={} layers={} levels={} samples={} using "
                    "null image fallback",
                    BindingTypeName(desc.type), vk::to_string(info.pixel_format), info.size.width,
                    info.size.height, info.size.depth, info.pitch, info.resources.layers,
                    info.resources.levels, info.num_samples);
        ASSERT_MSG(!ShouldAbortNullGuestImageDescriptor(),
                   "Strict render validation: image descriptor has null guest address");
        return GetNullImage(info.pixel_format);
    }

    std::scoped_lock lock{mutex};
    ImageIds image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    ImageId image_id{};

    // Check for a perfect match first
    for (const auto& cache_id : image_ids) {
        auto& cache_image = slot_images[cache_id];
        if (cache_image.info.guest_address != info.guest_address) {
            continue;
        }
        if (cache_image.info.guest_size != info.guest_size) {
            continue;
        }
        if (cache_image.info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
            (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
            continue;
        }
        if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
            continue;
        }
        image_id = cache_id;
    }

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            // The image was clearly picked up wrong.
            ASSERT_MSG(!IsStrictRenderValidationEnabled(),
                       "Strict render validation: image overlap resolved to image with too few "
                       "resources requested_addr={:#x} requested_size={} requested_format={} "
                       "requested_levels={} requested_layers={} resolved_id={} resolved_addr={:#x} "
                       "resolved_size={} resolved_format={} resolved_levels={} resolved_layers={}",
                       info.guest_address, info.guest_size, vk::to_string(info.pixel_format),
                       info.resources.levels, info.resources.layers, image_id.index,
                       image_resolved.info.guest_address, image_resolved.info.guest_size,
                       vk::to_string(image_resolved.info.pixel_format),
                       image_resolved.info.resources.levels, image_resolved.info.resources.layers);
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        image_id = slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (const ImageId image_id : image_ids) {
            Image& image = slot_images[image_id];
            if (image.info.guest_address == address && image.info.guest_size == size) {
                return image_id;
            }
        }
        for (const ImageId image_id : image_ids) {
            Image& image = slot_images[image_id];
            if (image.info.guest_address == address) {
                return image_id;
            }
        }

        ImageId smallest_containing{};
        u64 smallest_size = std::numeric_limits<u64>::max();
        for (const ImageId image_id : image_ids) {
            Image& image = slot_images[image_id];
            if (address < image.info.guest_address) {
                continue;
            }
            const u64 offset = address - image.info.guest_address;
            if (offset <= image.info.guest_size && size <= image.info.guest_size - offset &&
                image.info.guest_size < smallest_size) {
                smallest_containing = image_id;
                smallest_size = image.info.guest_size;
            }
        }
        if (smallest_containing) {
            return smallest_containing;
        }

        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
        for (const auto image_id : image_ids) {
            const Image& image = slot_images[image_id];
            LOG_WARNING(Render_Vulkan,
                        "Ambiguous image range candidate image_id={} guest_addr={:#x} "
                        "guest_size={:#x} size={}x{}x{} levels={} layers={}",
                        image_id.index, image.info.guest_address, image.info.guest_size,
                        image.info.size.width, image.info.size.height, image.info.size.depth,
                        image.info.resources.levels, image.info.resources.layers);
        }
        ASSERT_MSG(!ShouldAbortAmbiguousImageRangeLookup(),
                   "Strict render validation: ambiguous image range lookup addr={:#x} size={:#x} "
                   "candidates={}",
                   address, size, image_ids.size());
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && !image.info.props.is_tiled && image.info.guest_address != 0) {
            download_images.emplace(image_id);
        }
    }
    UpdateImage(image_id);
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    if (readback_linear_images && !image.info.props.is_tiled) {
        download_images.emplace(image_id);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this color buffer
    if (desc.info.meta_info.cmask_addr) {
        auto meta = MakeMetaDataInfo(MetaDataInfo::Type::CMask, image, image_id, desc.type);
        RegisterSurfaceMeta(surface_metas, desc.info.meta_info.cmask_addr, meta);
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        auto meta = MakeMetaDataInfo(MetaDataInfo::Type::FMask, image, image_id, desc.type);
        RegisterSurfaceMeta(surface_metas, desc.info.meta_info.fmask_addr, meta);
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    if (desc.info.meta_info.htile_addr) {
        auto meta = MakeMetaDataInfo(MetaDataInfo::Type::HTile, image, image_id, desc.type,
                                     image.info.meta_info.htile_clear_mask);
        RegisterSurfaceMeta(surface_metas, desc.info.meta_info.htile_addr, meta);
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id =
                slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
            RegisterImage(stencil_id);
        }
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        stencil_image.AssociateDepth(image_id, image.image_uid);
    }

    return image.FindView(desc.view_info, false);
}

void TextureCache::RefreshImage(Image& image) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        // The image size should be less than page size to be considered MaybeCpuDirty
        // So this calculation should be very uncommon and reasonably fast
        // For now we'll just check up to 64 first pixels
        const auto addr = std::bit_cast<u8*>(image.info.guest_address);
        const u32 w = std::min(image.info.size.width, u32(8));
        const u32 h = std::min(image.info.size.height, u32(8));

        const u32 s_w = image.info.props.is_block ? Common::DivCeil(w, 4u) : w;
        const u32 s_h = image.info.props.is_block ? Common::DivCeil(h, 4u) : h;
        const u32 size = s_w * s_h * (image.info.num_bits / 8);
        const u64 hash = XXH3_64bits(addr, size);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);

    // Skip CPU re-upload of a format-substituted depth-stencil surface. Metal lacks D16S8 so
    // MoltenVK substitutes a larger host format (e.g. D32SfloatS8Uint); the source buffer is
    // sized for the guest depth format but Metal's copyBufferToImage computes the required source
    // size from the larger host format and over-reads the buffer ("totalBytesUsed must be <=
    // sourceBuffer length"), faulting the command buffer with Invalid Resource (device loss; UFC 3
    // loading->menu transition). Gated to CPU-dirty (non-GPU-modified) depth images, which is the
    // abnormal case caused by depth/color memory aliasing — normal GPU-rendered depth targets are
    // GpuModified and unaffected. The guest tiled depth bytes don't match the host layout anyway,
    // so this upload would be meaningless even without the overflow; keep the GPU content.
    if (image.info.props.is_depth && !is_gpu_modified) {
        const vk::Format host_format =
            instance.GetSupportedFormat(image.info.pixel_format, image.format_features);
        if (host_format != image.info.pixel_format) {
            image.flags &= ~ImageFlagBits::Dirty;
            return;
        }
    }

    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        if (is_gpu_modified && !is_gpu_dirty) {
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
        }

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    if (auto barrier = in_buffer->GetBarrier(vk::AccessFlagBits2::eTransferRead,
                                             vk::PipelineStageFlagBits2::eTransfer)) {
        scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &barrier.value(),
        });
    }

    const auto [buffer, offset] =
        tile_manager.DetileImage(in_buffer->Handle(), in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    // Catch (and skip) a buffer->image over-read here at the recording site, rather than letting
    // it surface as a later asynchronous GPU device loss. For tiled images DetileImage returns its
    // scratch buffer (sized guest_size, offset 0); for linear images it returns the buffer-cache
    // buffer directly. The over-read happens for a format-substituted depth surface whose guest
    // texels are narrower than the host ones (e.g. D16 depth -> host D32, 2x), so the host-format
    // copy reads past the guest-sized source buffer. That upload is meaningless anyway (the guest
    // bytes don't match the host depth layout, and for the aliased surface they actually hold
    // non-depth data), so skipping it loses nothing while keeping the GPU-rendered content and
    // avoiding the device loss. The check is aspect/block accurate (no false positives), so this
    // only triggers on a genuine over-read.
    {
        const vk::Format host_format =
            instance.GetSupportedFormat(image.info.pixel_format, image.format_features);
        const u64 src_buffer_size = image.info.props.is_tiled
                                        ? static_cast<u64>(image.info.guest_size)
                                        : static_cast<u64>(in_buffer->SizeBytes());
        if (ValidateBufferToImageBounds("RefreshImage.Upload", image.info, host_format,
                                        image_copies, src_buffer_size)) {
            image.flags &= ~ImageFlagBits::Dirty;
            tile_manager.ReleasePendingScratchBuffers();
            return;
        }
    }

    image.Upload(image_copies, buffer, offset);
    // The detile scratch buffer is consumed by image.Upload above; release it now so its
    // deferred destruction is registered at this (last-use) tick rather than the earlier
    // detile tick, preventing a use-after-free of the buffer by the Upload command buffer.
    tile_manager.ReleasePendingScratchBuffers();
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base) {
    const u64 hash = XXH3_64bits(&sampler, sizeof(sampler));
    const auto [it, new_sampler] = samplers.try_emplace(hash, instance, sampler, border_color_base);
    return it->second.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    const VAddr guest_addr = image.info.guest_address;
    ForEachPage(guest_addr, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
    // Aliasing-storm gauge: warn when many images overlap one guest page simultaneously (the
    // depth<->color reinterpret / size-ratchet aliasing that drives churn + the render corruption).
    if (const auto* ids = page_table.find(guest_addr >> Traits::PageBits); ids != nullptr &&
        ids->size() >= 16) {
        VideoCore::Diag::ReportOnce(
            fmt::format("alias_storm:{:#x}", guest_addr),
            fmt::format("{}+ images overlap the guest page of {:#x} simultaneously (aliasing storm)",
                        ids->size(), guest_addr));
        // Circuit breaker: an unbounded storm fills unified memory and wedges the whole host
        // (hard-reboot territory, like the device-loss exit in MasterSemaphore::Refresh). Dump
        // the overlapping images + op ring and hard-exit instead of letting macOS freeze.
        // SHADPS4_ALIAS_STORM_LIMIT overrides the threshold; 0 disables the breaker.
        static const u64 storm_limit = [] {
            if (const char* v = std::getenv("SHADPS4_ALIAS_STORM_LIMIT")) {
                return static_cast<u64>(std::atoll(v));
            }
            return u64{48};
        }();
        if (storm_limit != 0 && ids->size() >= storm_limit) {
            LOG_CRITICAL(Render_Vulkan,
                         "Aliasing storm breaker: {} images overlap guest page {:#x}; dumping and "
                         "exiting to avoid wedging the host GPU/memory",
                         ids->size(), guest_addr);
            for (const ImageId id : *ids) {
                const Image& overlap = slot_images[id];
                LOG_CRITICAL(Render_Vulkan,
                             "  storm image: addr={:#x} size={:#x} {}x{}x{} format={} usage={:#x}",
                             overlap.info.guest_address, overlap.info.guest_size,
                             overlap.info.size.width, overlap.info.size.height,
                             overlap.info.size.depth,
                             vk::to_string(overlap.info.pixel_format),
                             static_cast<u32>(overlap.usage_flags));
            }
            Vulkan::DumpGpuCommandDiagnostics("alias_storm_breaker");
            Common::Log::Flush();
            std::_Exit(71);
        }
    }
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << Traits::PageBits);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}",
                       page << Traits::PageBits);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers<1>(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers<1>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers<1>(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers<false>(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(addr, size);
}

void TextureCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](ImageId image_id) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        auto& image = slot_images[image_id];
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();
        if (tiled && download) {
            // This is a workaround for now. We can't handle non-linear image downloads.
            return false;
        }
        if (download && !pressured) {
            return false;
        }
        if (download) {
            DownloadImageMemory(image_id);
        }
        FreeImage(image_id);
        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::TouchImage(const Image& image) {
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    const auto erase_owned_meta = [this, image_id](VAddr address) {
        if (!address) {
            return;
        }
        auto it = surface_metas.find(address);
        if (it != surface_metas.end() && it.value().owner_image_id.index == image_id.index) {
            surface_metas.erase(it);
        }
    };
    erase_owned_meta(meta_info.cmask_addr);
    erase_owned_meta(meta_info.fmask_addr);
    erase_owned_meta(meta_info.htile_addr);

    // Reclaim image and any image views it references. Use DeferOperationAfterSubmit (CurrentTick
    // +1), matching DeleteBuffer: an image recreated by ResolveDepthOverlap's swap is freed here
    // while the command buffer currently being recorded (and any in-flight sibling recording) may
    // still reference the old image; deferring to CurrentTick() alone can reclaim it before that
    // recording is submitted+completed -> kIOGPU Invalid Resource device loss (UFC 3 menu/movie).
    const VAddr trace_addr = image.info.guest_address;
    const u64 trace_size = image.info.guest_size;
    const u64 reg_tick = scheduler.CurrentTick();
    const u64 extra_submits = ImageFreeExtraSubmits();
    if (extra_submits > 1) {
        VideoCore::Diag::ReportOnce(
            "image_free_extra_submits",
            fmt::format("SHADPS4_IMAGE_FREE_EXTRA_SUBMITS={} active: image pool releases are "
                        "quarantined for extra submitted ticks",
                        extra_submits));
    }
    scheduler.DeferOperationAfterSubmit([this, image_id, trace_addr, trace_size, reg_tick,
                                         extra_submits] {
        if (VideoCore::Diag::TraceFrees()) {
            LOG_INFO(Render_Vulkan, "FREE image addr={:#x} size={} extra_submits={}", trace_addr,
                     trace_size, extra_submits);
        }
        Vulkan::RecordGpuCommandDiagnostic(
            "FREE image addr=0x%llx size=%llu reg_tick=%llu extra_submits=%llu",
            static_cast<unsigned long long>(trace_addr),
            static_cast<unsigned long long>(trace_size),
            static_cast<unsigned long long>(reg_tick),
            static_cast<unsigned long long>(extra_submits));
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
        }
        slot_images.erase(image_id);
    }, extra_submits);
}

} // namespace VideoCore
