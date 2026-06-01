// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ranges>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "common/assert.h"
#include "common/trace_control.h"
#include "video_core/host_diagnostics.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_gpu_command_diagnostics.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

static bool IsStrictRenderValidationEnabled() {
    static const bool enabled = Common::Trace::EnvEnabled("SHADPS4_STRICT_RENDER_VALIDATION");
    return enabled;
}

static bool ShouldAbortCopyLayerCoercion() {
    static const bool enabled =
        Common::Trace::EnvEnabled("SHADPS4_STRICT_COPY_LAYER_COERCION_ABORT");
    return IsStrictRenderValidationEnabled() && enabled;
}

// Diagnostic: image copy subresources must lie within the image's mip/layer range. An out-of-
// bounds mip or layer makes the GPU access memory past the image and faults the command buffer
// (device loss). Catch it at the recording site rather than as an opaque async GPU error. Warns
// always; asserts under strict render validation. Pure check.
static void ValidateCopySubresources(const char* site, const ImageInfo& info,
                                     std::span<const vk::BufferImageCopy> copies) {
    for (const auto& copy : copies) {
        const auto& sub = copy.imageSubresource;
        const u32 end_layer = sub.baseArrayLayer + sub.layerCount;
        if (sub.mipLevel >= info.resources.levels || end_layer > info.resources.layers ||
            sub.layerCount == 0) {
            LOG_WARNING(Render_Vulkan,
                        "[{}] image copy subresource out of bounds: addr={:#x} mip={} "
                        "image_levels={} base_layer={} layer_count={} image_layers={}",
                        site, info.guest_address, sub.mipLevel, info.resources.levels,
                        sub.baseArrayLayer, sub.layerCount, info.resources.layers);
            ASSERT_MSG(!IsStrictRenderValidationEnabled(),
                       "Strict render validation: [{}] image copy subresource OOB addr={:#x} "
                       "mip={} image_levels={} end_layer={} image_layers={}",
                       site, info.guest_address, sub.mipLevel, info.resources.levels, end_layer,
                       info.resources.layers);
        }
    }
}

static bool CanDirectCopyImageFormats(const ImageInfo& src_info, const ImageInfo& dst_info,
                                      vk::ImageAspectFlags src_aspect,
                                      vk::ImageAspectFlags dst_aspect) {
    if (src_info.num_samples != dst_info.num_samples) {
        return false;
    }
    if (src_aspect != dst_aspect) {
        return false;
    }
    if (src_info.props.is_depth != dst_info.props.is_depth ||
        src_info.props.has_stencil != dst_info.props.has_stencil) {
        return false;
    }
    return src_info.pixel_format == dst_info.pixel_format ||
           IsVulkanFormatCompatible(src_info.pixel_format, dst_info.pixel_format);
}

static vk::ImageUsageFlags ImageUsageFlags(const Vulkan::Instance* instance,
                                           const ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (instance->IsAttachmentFeedbackLoopLayoutSupported()) {
                usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
            }
            // Prefer storage-capable images to avoid re-creation in case of e.g. compute clears,
            // but some color formats are not valid storage images on MoltenVK.
            if (instance->IsFormatSupported(info.pixel_format,
                                            vk::FormatFeatureFlagBits2::eStorageImage)) {
                usage |= vk::ImageUsageFlagBits::eStorage;
            }
        }
    }

    return usage;
}

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageType::e1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE();
    }
}

static vk::FormatFeatureFlags2 FormatFeatureFlags(const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags{};
    if (usage_flags & vk::ImageUsageFlagBits::eTransferSrc) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eTransferDst) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eSampled) {
        feature_flags |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eColorAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    // Note: StorageImage is intentionally ignored for now since it is always set, and can mess up
    // compatibility checks.
    return feature_flags;
}

namespace {

// Freed-image reuse pool. The texture cache aliases one guest address as multiple incompatible
// image formats/sizes (depth<->color reinterprets, slice-expand ratchets) and recreates+frees the
// VkImage at that address hundreds of times per second. Even though every free is correctly
// GPU-deferred, that vmaCreateImage/vmaDestroyImage churn is what drives the intermittent MoltenVK
// kIOGPUCommandBufferCallbackErrorInvalidResource device loss (Metal residency/heap-aliasing
// pressure, not a Vulkan use-after-free — the lifetime gate was proven correct). Recycling the GPU
// image objects keyed by their exact create-info eliminates that churn at the source, for every
// recreate site, instead of patching each site. The pool only holds GPU-idle images (releases run
// inside the deferred-destroy callback, after the lifetime gate), so a reused image is never in
// flight.
struct ImagePoolKey {
    u32 flags;
    u32 type;
    u32 format;
    u32 usage;
    u32 tiling;
    u32 samples;
    u32 mips;
    u32 layers;
    u32 width;
    u32 height;
    u32 depth;
    u32 initial_layout;

    bool operator==(const ImagePoolKey& o) const noexcept {
        return std::memcmp(this, &o, sizeof(ImagePoolKey)) == 0;
    }
};

ImagePoolKey MakeImagePoolKey(const vk::ImageCreateInfo& ci) {
    return ImagePoolKey{
        .flags = static_cast<u32>(static_cast<VkImageCreateFlags>(ci.flags)),
        .type = static_cast<u32>(ci.imageType),
        .format = static_cast<u32>(ci.format),
        .usage = static_cast<u32>(static_cast<VkImageUsageFlags>(ci.usage)),
        .tiling = static_cast<u32>(ci.tiling),
        .samples = static_cast<u32>(static_cast<VkSampleCountFlags>(ci.samples)),
        .mips = ci.mipLevels,
        .layers = ci.arrayLayers,
        .width = ci.extent.width,
        .height = ci.extent.height,
        .depth = ci.extent.depth,
        .initial_layout = static_cast<u32>(ci.initialLayout),
    };
}

struct ImagePoolKeyHash {
    std::size_t operator()(const ImagePoolKey& k) const noexcept {
        const auto* p = reinterpret_cast<const u8*>(&k);
        std::size_t h = 1469598103934665603ull; // FNV-1a
        for (std::size_t i = 0; i < sizeof(k); ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }
};

class ImageReusePool {
public:
    static ImageReusePool& Get() {
        static ImageReusePool pool;
        return pool;
    }

    // Try to hand back a previously-released image matching `key`. Returns true on a hit.
    bool Acquire(const ImagePoolKey& key, VkImage& out_image, VmaAllocation& out_alloc) {
        if (!enabled) {
            return false;
        }
        std::scoped_lock lk{mutex};
        auto it = entries.find(key);
        if (it == entries.end() || it->second.empty()) {
            ++stat_misses;
            return false;
        }
        const Entry e = it->second.back();
        it->second.pop_back();
        total_bytes -= e.bytes;
        --total_count;
        ++stat_hits;
        out_image = e.image;
        out_alloc = e.allocation;
        return true;
    }

    // Return a GPU-idle image to the pool, or destroy it if pooling is disabled / over budget.
    void Release(VmaAllocator allocator, const ImagePoolKey& key, VkImage image,
                 VmaAllocation allocation, u64 bytes) {
        if (image == VK_NULL_HANDLE) {
            return;
        }
        if (!enabled) {
            vmaDestroyImage(allocator, image, allocation);
            return;
        }
        std::scoped_lock lk{mutex};
        entries[key].push_back(Entry{image, allocation, bytes});
        total_bytes += bytes;
        ++total_count;
        while (total_bytes > max_bytes || total_count > max_count) {
            if (!EvictOne(allocator)) {
                break;
            }
        }
    }

    // Destroy all pooled images and disable further pooling (called before the allocator dies).
    void Drain(VmaAllocator allocator) {
        std::scoped_lock lk{mutex};
        const u64 total_acquire = stat_hits + stat_misses;
        const double hit_rate = total_acquire ? (100.0 * stat_hits / total_acquire) : 0.0;
        LOG_INFO(Render_Vulkan,
                 "ImageReusePool stats: hits={} misses={} hit_rate={:.1f}% evictions={} "
                 "residual_entries={} residual_bytes={}",
                 stat_hits, stat_misses, hit_rate, stat_evictions, total_count, total_bytes);
        enabled = false;
        for (auto& [key, bucket] : entries) {
            for (const Entry& e : bucket) {
                vmaDestroyImage(allocator, e.image, e.allocation);
            }
        }
        entries.clear();
        total_bytes = 0;
        total_count = 0;
    }

private:
    struct Entry {
        VkImage image;
        VmaAllocation allocation;
        u64 bytes;
    };

    ImageReusePool() {
        if (const char* v = std::getenv("SHADPS4_IMAGE_REUSE_POOL")) {
            enabled = std::strcmp(v, "0") != 0;
        }
        if (const char* v = std::getenv("SHADPS4_IMAGE_REUSE_POOL_MAX_MB")) {
            const unsigned long mb = std::strtoul(v, nullptr, 10);
            if (mb > 0) {
                max_bytes = static_cast<u64>(mb) * 1024 * 1024;
            }
        }
    }

    // Evict one entry from any non-empty bucket (caller holds the lock).
    bool EvictOne(VmaAllocator allocator) {
        for (auto& [key, bucket] : entries) {
            if (!bucket.empty()) {
                const Entry e = bucket.back();
                bucket.pop_back();
                vmaDestroyImage(allocator, e.image, e.allocation);
                total_bytes -= e.bytes;
                --total_count;
                ++stat_evictions;
                return true;
            }
        }
        return false;
    }

    bool enabled = true;
    u64 max_bytes = 512ull * 1024 * 1024; // default 512 MB of idle pooled images
    u32 max_count = 512;
    u64 total_bytes = 0;
    u32 total_count = 0;
    u64 stat_hits = 0;
    u64 stat_misses = 0;
    u64 stat_evictions = 0;
    std::unordered_map<ImagePoolKey, std::vector<Entry>, ImagePoolKeyHash> entries;
    std::mutex mutex;
};

// Rough byte estimate for pool budgeting (does not need to be exact).
u64 ApproxImageBytes(const vk::ImageCreateInfo& ci) {
    const u64 texels = static_cast<u64>(ci.extent.width) * std::max<u32>(ci.extent.height, 1) *
                       std::max<u32>(ci.extent.depth, 1) * std::max<u32>(ci.arrayLayers, 1);
    const u64 samples = std::max<u32>(static_cast<u32>(static_cast<VkSampleCountFlags>(ci.samples)), 1);
    return texels * samples * 4ull; // assume <= 4 bytes/texel (sufficient for a cap heuristic)
}

} // namespace

void DrainImageReusePool(VmaAllocator allocator) {
    ImageReusePool::Get().Drain(allocator);
}

UniqueImage::~UniqueImage() {
    if (image) {
        ImageReusePool::Get().Release(allocator, MakeImagePoolKey(image_ci), image, allocation,
                                      ApproxImageBytes(image_ci));
    }
}

void UniqueImage::Destroy() {
    if (image) {
        ImageReusePool::Get().Release(allocator, MakeImagePoolKey(image_ci), image, allocation,
                                      ApproxImageBytes(image_ci));
        image = vk::Image{};
        allocation = {};
    }
}

bool UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    this->image_ci = image_ci;
    ASSERT(!image);

    // Reuse a GPU-idle image of identical create-info if one is pooled, avoiding the
    // vmaCreateImage/vmaDestroyImage churn that drives the MoltenVK device loss.
    {
        VkImage reused{};
        VmaAllocation reused_alloc{};
        if (ImageReusePool::Get().Acquire(MakeImagePoolKey(image_ci), reused, reused_alloc)) {
            image = vk::Image{reused};
            allocation = reused_alloc;
            return true;
        }
    }

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    VkImage unsafe_image{};
    VkResult result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_info, &unsafe_image,
                                     &allocation, nullptr);
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating image with error {}",
               vk::to_string(vk::Result{result}));
    image = vk::Image{unsafe_image};
    return false;
}

Image::Image(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
             BlitHelper& blit_helper_, Common::SlotVector<ImageView>& slot_image_views_,
             const ImageInfo& info_)
    : instance{&instance_}, scheduler{&scheduler_}, blit_helper{&blit_helper_},
      slot_image_views{&slot_image_views_}, info{info_} {
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }
    image_uid = global_image_uid.Next();
    mip_hashes.resize(info.resources.levels);
    // Here we force `eExtendedUsage` as don't know all image usage cases beforehand. In normal case
    // the texture cache should re-create the resource with the usage requested
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance->Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && instance->IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }

    usage_flags = ImageUsageFlags(instance, info);
    format_features = FormatFeatureFlags(usage_flags);
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    constexpr auto tiling = vk::ImageTiling::eOptimal;
    const auto supported_format = instance->GetSupportedFormat(info.pixel_format, format_features);
    const vk::PhysicalDeviceImageFormatInfo2 format_info{
        .format = supported_format,
        .type = ConvertImageType(info.type),
        .tiling = tiling,
        .usage = usage_flags,
        .flags = flags,
    };
    const auto image_format_properties =
        instance->GetPhysicalDevice().getImageFormatProperties2(format_info);
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported) {
        LOG_ERROR(Render_Vulkan, "image format {} type {} is not supported (flags {}, usage {})",
                  vk::to_string(supported_format), vk::to_string(format_info.type),
                  vk::to_string(format_info.flags), vk::to_string(format_info.usage));
        ASSERT_MSG(!IsStrictRenderValidationEnabled(),
                   "Strict render validation: unsupported image format {} type {} flags {} usage {} "
                   "guest_addr={:#x} guest_size={} size={}x{}x{} layers={} levels={} samples={}",
                   vk::to_string(supported_format), vk::to_string(format_info.type),
                   vk::to_string(format_info.flags), vk::to_string(format_info.usage),
                   info.guest_address, info.guest_size, info.size.width, info.size.height,
                   info.size.depth, info.resources.layers, info.resources.levels,
                   info.num_samples);
    }
    supported_samples = image_format_properties.result == vk::Result::eSuccess
                            ? image_format_properties.value.imageFormatProperties.sampleCounts
                            : vk::SampleCountFlagBits::e1;

    const vk::ImageCreateInfo image_ci = {
        .flags = flags,
        .imageType = ConvertImageType(info.type),
        .format = supported_format,
        .extent{
            .width = info.size.width,
            .height = info.size.height,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = LiverpoolToVK::NumSamples(info.num_samples, supported_samples),
        .tiling = tiling,
        .usage = usage_flags,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    backing = &backing_images.emplace_back();
    backing->num_samples = info.num_samples;
    backing->image = UniqueImage{instance->GetDevice(), instance->GetAllocator()};
    const bool reused_from_pool = backing->image.Create(image_ci);
    if (reused_from_pool) {
        RecordGpuCommandDiagnostic(
            "IMAGE_POOL acquire addr=0x%llx size=%u format=%s extent=%ux%u layers=%u samples=%u",
            static_cast<unsigned long long>(info.guest_address), info.guest_size,
            vk::to_string(info.pixel_format).c_str(), info.size.width, info.size.height,
            info.resources.layers, info.num_samples);
    }

    Vulkan::SetObjectName(instance->GetDevice(), GetImage(),
                          "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          vk::to_string(info.pixel_format), info.guest_address, info.guest_size,
                          info.resources.layers, info.resources.levels, info.num_samples);
}

Image::~Image() {
    for (const BackingImage& backing : backing_images) {
        if (backing.image.image) {
            RecordGpuCommandDiagnostic(
                "IMAGE_POOL release addr=0x%llx size=%u format=%s extent=%ux%u layers=%u "
                "samples=%u",
                static_cast<unsigned long long>(info.guest_address), info.guest_size,
                vk::to_string(info.pixel_format).c_str(), info.size.width, info.size.height,
                info.resources.layers, backing.num_samples);
        }
    }
}

ImageView& Image::FindView(const ImageViewInfo& requested_view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        SetBackingSamples(info.num_samples);
    }
    ImageViewInfo view_info = requested_view_info;
    const auto requested_end_level = view_info.range.base.level + view_info.range.extent.levels;
    const auto requested_end_layer = view_info.range.base.layer + view_info.range.extent.layers;
    if (requested_end_level > info.resources.levels ||
        requested_end_layer > info.resources.layers) {
        LOG_WARNING(Render_Vulkan,
                    "TRACE_IMAGE view_subresource_oob addr={:#x} size={:#x} format={} "
                    "image_levels={} image_layers={} base_level={} base_layer={} "
                    "req_levels={} req_layers={} end_level={} end_layer={}",
                    info.guest_address, info.guest_size, vk::to_string(info.pixel_format),
                    info.resources.levels, info.resources.layers, view_info.range.base.level,
                    view_info.range.base.layer, view_info.range.extent.levels,
                    view_info.range.extent.layers, requested_end_level, requested_end_layer);
        ASSERT_MSG(!IsStrictRenderValidationEnabled(),
                   "Strict render validation: image view requests out-of-bounds subresources "
                   "addr={:#x} size={:#x} format={} image_levels={} image_layers={} "
                   "base_level={} base_layer={} req_levels={} req_layers={} end_level={} "
                   "end_layer={}",
                   info.guest_address, info.guest_size, vk::to_string(info.pixel_format),
                   info.resources.levels, info.resources.layers, view_info.range.base.level,
                   view_info.range.base.layer, view_info.range.extent.levels,
                   view_info.range.extent.layers, requested_end_level, requested_end_layer);
        view_info.range.base.level = std::min(view_info.range.base.level, info.resources.levels - 1);
        view_info.range.base.layer = std::min(view_info.range.base.layer, info.resources.layers - 1);
        view_info.range.extent.levels =
            std::min(view_info.range.extent.levels,
                     info.resources.levels - view_info.range.base.level);
        view_info.range.extent.layers =
            std::min(view_info.range.extent.layers,
                     info.resources.layers - view_info.range.base.layer);
    }
    const auto& view_infos = backing->image_view_infos;
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        const auto view_id = backing->image_view_ids[std::distance(view_infos.begin(), it)];
        return (*slot_image_views)[view_id];
    }
    const auto view_id = slot_image_views->insert(*instance, view_info, *this);
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}

Image::Barriers Image::GetBarriers(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                                   vk::PipelineStageFlags2 dst_stage,
                                   std::optional<SubresourceRange> subres_range) {
    auto& last_state = backing->state;
    auto& subresource_states = backing->subresource_states;

    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != info.resources);
    const bool partially_transited = !subresource_states.empty();

    Barriers barriers;
    if (needs_partial_transition || partially_transited) {
        if (!partially_transited) {
            subresource_states.resize(info.resources.levels * info.resources.layers);
            std::fill(subresource_states.begin(), subresource_states.end(), last_state);
        }

        // In case of partial transition, we need to change the specified subresources only.
        // Otherwise all subresources need to be set to the same state so we can use a full
        // resource transition for the next time.
        const auto mips =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.level,
                                           subres_range->base.level + subres_range->extent.levels)
                : std::views::iota(0u, info.resources.levels);
        const auto layers =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.layer,
                                           subres_range->base.layer + subres_range->extent.layers)
                : std::views::iota(0u, info.resources.layers);

        for (u32 mip : mips) {
            for (u32 layer : layers) {
                // NOTE: these loops may produce a lot of small barriers.
                // If this becomes a problem, we can optimize it by merging adjacent barriers.
                const auto subres_idx = mip * info.resources.layers + layer;
                ASSERT(subres_idx < subresource_states.size());
                auto& state = subresource_states[subres_idx];

                constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                             vk::AccessFlagBits2::eShaderWrite |
                                             vk::AccessFlagBits2::eMemoryWrite;
                const bool is_write = static_cast<bool>(state.access_mask & write_flags);
                if (state.layout != dst_layout || state.access_mask != dst_mask || is_write) {
                    barriers.emplace_back(vk::ImageMemoryBarrier2{
                        .srcStageMask = state.pl_stage,
                        .srcAccessMask = state.access_mask,
                        .dstStageMask = dst_stage,
                        .dstAccessMask = dst_mask,
                        .oldLayout = state.layout,
                        .newLayout = dst_layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = GetImage(),
                        .subresourceRange{
                            .aspectMask = aspect_mask,
                            .baseMipLevel = mip,
                            .levelCount = 1,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                        },
                    });
                    state.layout = dst_layout;
                    state.access_mask = dst_mask;
                    state.pl_stage = dst_stage;
                }
            }
        }

        if (!needs_partial_transition) {
            subresource_states.clear();
        }
    } else { // Full resource transition
        constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
        const bool is_write = static_cast<bool>(last_state.access_mask & write_flags);
        if (last_state.layout == dst_layout && last_state.access_mask == dst_mask && !is_write) {
            return {};
        }

        barriers.emplace_back(vk::ImageMemoryBarrier2{
            .srcStageMask = last_state.pl_stage,
            .srcAccessMask = last_state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = last_state.layout,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = GetImage(),
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        });
    }

    last_state.layout = dst_layout;
    last_state.access_mask = dst_mask;
    last_state.pl_stage = dst_stage;

    return barriers;
}

void Image::Transit(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                    std::optional<SubresourceRange> range, vk::CommandBuffer cmdbuf /*= {}*/) {
    // Adjust pipieline stage
    const vk::PipelineStageFlags2 dst_pl_stage =
        (dst_mask == vk::AccessFlagBits2::eTransferRead ||
         dst_mask == vk::AccessFlagBits2::eTransferWrite)
            ? vk::PipelineStageFlagBits2::eTransfer
            : vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eComputeShader;

    const auto barriers = GetBarriers(dst_layout, dst_mask, dst_pl_stage, range);
    if (barriers.empty()) {
        return;
    }

    if (!cmdbuf) {
        // When using external cmdbuf you are responsible for ending rp.
        scheduler->EndRendering();
        cmdbuf = scheduler->CommandBuffer();
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    });
}

void Image::Upload(std::span<const vk::BufferImageCopy> upload_copies, vk::Buffer buffer,
                   u64 offset) {
    ValidateCopySubresources("Image::Upload", info, upload_copies);
    SetBackingSamples(info.num_samples, false);
    scheduler->EndRendering();

    const vk::BufferMemoryBarrier2 pre_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer,
        .offset = offset,
        .size = info.guest_size,
    };
    const vk::BufferMemoryBarrier2 post_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer,
        .offset = offset,
        .size = info.guest_size,
    };
    const auto image_barriers =
        GetBarriers(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                    vk::PipelineStageFlagBits2::eCopy, {});
    const auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
        .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
        .pImageMemoryBarriers = image_barriers.data(),
    });
    RecordGpuCommandDiagnostic("upload_buffer_to_image dst_addr=0x%llx copies=%zu",
                               static_cast<unsigned long long>(info.guest_address),
                               upload_copies.size());
    cmdbuf.copyBufferToImage(buffer, GetImage(), vk::ImageLayout::eTransferDstOptimal,
                             upload_copies);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
    flags &= ~ImageFlagBits::Dirty;
}

void Image::Download(std::span<const vk::BufferImageCopy> download_copies, vk::Buffer buffer,
                     u64 offset, u64 download_size) {
    ValidateCopySubresources("Image::Download", info, download_copies);
    // The download writes image data into `buffer`; verify the regions fit the destination so we
    // don't over-write past it (an out-of-bounds write the GPU would fault on).
    VideoCore::Diag::CheckBufferImageCopies("Image::Download", info.guest_address, info.pixel_format,
                                            download_copies, offset + download_size);
    SetBackingSamples(info.num_samples);
    scheduler->EndRendering();

    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = download_size,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .buffer = buffer,
        .offset = offset,
        .size = download_size,
    };
    const auto image_barriers =
        GetBarriers(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                    vk::PipelineStageFlagBits2::eCopy, {});
    auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
        .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
        .pImageMemoryBarriers = image_barriers.data(),
    });
    RecordGpuCommandDiagnostic("download_image_to_buffer src_addr=0x%llx copies=%zu",
                               static_cast<unsigned long long>(info.guest_address),
                               download_copies.size());
    cmdbuf.copyImageToBuffer(GetImage(), vk::ImageLayout::eTransferSrcOptimal, buffer,
                             download_copies);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
}

static std::pair<u32, u32> SanitizeCopyLayers(const ImageInfo& src_info, const ImageInfo& dst_info,
                                              const u32 depth, const u32 src_base_layer = 0,
                                              const u32 dst_base_layer = 0) {
    const auto vk_src_type = ConvertImageType(src_info.type);
    const auto vk_dst_type = ConvertImageType(dst_info.type);

    const auto remaining_layers = [](u32 layers, u32 base_layer) {
        return base_layer < layers ? layers - base_layer : 0;
    };
    u32 src_layers = remaining_layers(src_info.resources.layers, src_base_layer);
    u32 dst_layers = remaining_layers(dst_info.resources.layers, dst_base_layer);

    // 3D images can only use 1 layer.
    if (vk_src_type == vk::ImageType::e3D && src_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D source layers {} to 1.", src_layers);
        ASSERT_MSG(!ShouldAbortCopyLayerCoercion(),
                   "Strict render validation: coercing 3D source copy layers from {} to 1 "
                   "src_addr={:#x} src_size={} dst_addr={:#x} dst_size={}",
                   src_layers, src_info.guest_address, src_info.guest_size, dst_info.guest_address,
                   dst_info.guest_size);
        src_layers = 1;
    }
    if (vk_dst_type == vk::ImageType::e3D && dst_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D destination layers {} to 1.", dst_layers);
        ASSERT_MSG(!ShouldAbortCopyLayerCoercion(),
                   "Strict render validation: coercing 3D destination copy layers from {} to 1 "
                   "src_addr={:#x} src_size={} dst_addr={:#x} dst_size={}",
                   dst_layers, src_info.guest_address, src_info.guest_size,
                   dst_info.guest_address, dst_info.guest_size);
        dst_layers = 1;
    }

    // If the image type is equal, layer count must match. Take the minimum of both.
    if (vk_src_type == vk_dst_type) {
        if (src_layers != dst_layers) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy source layers {} and destination layers {} to minimum.",
                        src_layers, dst_layers);
            ASSERT_MSG(!ShouldAbortCopyLayerCoercion(),
                       "Strict render validation: coercing copy layers from src={} dst={} "
                       "src_addr={:#x} src_size={} dst_addr={:#x} dst_size={}",
                       src_layers, dst_layers, src_info.guest_address, src_info.guest_size,
                       dst_info.guest_address, dst_info.guest_size);
            src_layers = dst_layers = std::min(src_layers, dst_layers);
        }
    } else {
        // For 2D <-> 3D copies, 2D layer count must equal 3D depth.
        if (vk_src_type == vk::ImageType::e2D && vk_dst_type == vk::ImageType::e3D &&
            src_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D source layers {} to 3D destination depth {}", src_layers,
                        depth);
            ASSERT_MSG(!ShouldAbortCopyLayerCoercion(),
                       "Strict render validation: coercing 2D source copy layers {} to 3D depth {} "
                       "src_addr={:#x} src_size={} dst_addr={:#x} dst_size={}",
                       src_layers, depth, src_info.guest_address, src_info.guest_size,
                       dst_info.guest_address, dst_info.guest_size);
            src_layers = std::min(src_layers, depth);
        }
        if (vk_src_type == vk::ImageType::e3D && vk_dst_type == vk::ImageType::e2D &&
            dst_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D destination layers {} to 3D source depth {}", dst_layers,
                        depth);
            ASSERT_MSG(!ShouldAbortCopyLayerCoercion(),
                       "Strict render validation: coercing 2D destination copy layers {} to 3D "
                       "depth {} src_addr={:#x} src_size={} dst_addr={:#x} dst_size={}",
                       dst_layers, depth, src_info.guest_address, src_info.guest_size,
                       dst_info.guest_address, dst_info.guest_size);
            dst_layers = std::min(dst_layers, depth);
        }
    }

    return std::make_pair(src_layers, dst_layers);
}

void Image::CopyImage(Image& src_image) {
    const auto& src_info = src_image.info;

    const u32 num_mips = std::min(src_info.resources.levels, info.resources.levels);

    // Format mismatch warning (safe but useful)
    if (src_info.pixel_format != info.pixel_format) {
        LOG_DEBUG(Render_Vulkan,
                  "Copy between different formats: src={}, dst={}. "
                  "Result may be undefined.",
                  vk::to_string(src_info.pixel_format), vk::to_string(info.pixel_format));
    }

    const u32 base_width = src_info.size.width;
    const u32 base_height = src_info.size.height;
    const u32 base_depth =
        info.type == AmdGpu::ImageType::Color3D ? info.size.depth : src_info.size.depth;

    // Match sample count before copying
    SetBackingSamples(info.num_samples, false);
    src_image.SetBackingSamples(src_info.num_samples);

    boost::container::small_vector<vk::ImageCopy, 8> regions;

    const vk::ImageAspectFlags src_aspect =
        src_image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    const vk::ImageAspectFlags dst_aspect = aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    if (!CanDirectCopyImageFormats(src_info, info, src_aspect, dst_aspect)) {
        LOG_WARNING(Render_Vulkan,
                    "Skipping incompatible direct image copy src_addr={:#x} src_size={} "
                    "src_format={} src_samples={} src_aspect={} dst_addr={:#x} dst_size={} "
                    "dst_format={} dst_samples={} dst_aspect={}",
                    src_info.guest_address, src_info.guest_size,
                    vk::to_string(src_info.pixel_format), src_info.num_samples,
                    vk::to_string(src_aspect), info.guest_address, info.guest_size,
                    vk::to_string(info.pixel_format), info.num_samples, vk::to_string(dst_aspect));
        return;
    }

    const bool src_is_2d = ConvertImageType(src_info.type) == vk::ImageType::e2D;
    const bool src_is_3d = ConvertImageType(src_info.type) == vk::ImageType::e3D;

    const bool dst_is_2d = ConvertImageType(info.type) == vk::ImageType::e2D;
    const bool dst_is_3d = ConvertImageType(info.type) == vk::ImageType::e3D;

    const bool is_2d_to_3d = src_is_2d && dst_is_3d;
    const bool is_3d_to_2d = src_is_3d && dst_is_2d;
    const bool is_same_type = !is_2d_to_3d && !is_3d_to_2d;

    for (u32 mip = 0; mip < num_mips; ++mip) {
        const u32 mip_w = std::max(base_width >> mip, 1u);
        const u32 mip_h = std::max(base_height >> mip, 1u);
        const u32 mip_d = std::max(base_depth >> mip, 1u);

        auto [src_layers, dst_layers] = SanitizeCopyLayers(src_info, info, mip_d);
        if (src_layers == 0 || dst_layers == 0) {
            continue;
        }

        vk::ImageCopy region{};

        region.srcSubresource.aspectMask = src_aspect;
        region.srcSubresource.mipLevel = mip;
        region.srcSubresource.baseArrayLayer = 0;

        region.dstSubresource.aspectMask = dst_aspect;
        region.dstSubresource.mipLevel = mip;
        region.dstSubresource.baseArrayLayer = 0;

        if (is_same_type) {
            // 2D->2D OR 3D->3D
            if (src_is_3d) {
                // 3D images must use layerCount=1
                region.srcSubresource.layerCount = 1;
                region.dstSubresource.layerCount = 1;
                region.extent = vk::Extent3D(mip_w, mip_h, mip_d);
            } else {
                // Array images
                const u32 copy_layers = std::min(src_layers, dst_layers);
                region.srcSubresource.layerCount = copy_layers;
                region.dstSubresource.layerCount = copy_layers;
                region.extent = vk::Extent3D(mip_w, mip_h, 1);
            }
        } else if (is_2d_to_3d) {
            // 2D array -> 3D volume
            region.srcSubresource.layerCount = src_layers;
            region.dstSubresource.layerCount = 1;
            region.extent = vk::Extent3D(mip_w, mip_h, src_layers);
        } else if (is_3d_to_2d) {
            // 3D volume -> 2D array
            region.srcSubresource.layerCount = 1;
            region.dstSubresource.layerCount = dst_layers;
            region.extent = vk::Extent3D(mip_w, mip_h, dst_layers);
        }

        regions.push_back(region);
    }

    scheduler->EndRendering();

    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});

    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});

    auto cmdbuf = scheduler->CommandBuffer();

    if (!regions.empty()) {
        VideoCore::Diag::CheckImageCopy("Image::CopyImage", src_image.info.guest_address,
                                        src_image.info.resources.levels,
                                        src_image.info.resources.layers, info.guest_address,
                                        info.resources.levels, info.resources.layers, regions);
        RecordGpuCommandDiagnostic("copy_image src_addr=0x%llx dst_addr=0x%llx regions=%zu",
                                   static_cast<unsigned long long>(src_image.info.guest_address),
                                   static_cast<unsigned long long>(info.guest_address),
                                   regions.size());
        cmdbuf.copyImage(src_image.GetImage(), src_image.backing->state.layout, GetImage(),
                         backing->state.layout, regions);
    }

    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}
void Image::CopyImageWithBuffer(Image& src_image, vk::Buffer buffer, u64 offset) {
    const auto& src_info = src_image.info;
    const u32 num_mips = std::min(src_info.resources.levels, info.resources.levels);
    const u32 num_layers = std::min(src_info.resources.layers, info.resources.layers);
    ASSERT(src_info.resources.layers == info.resources.layers || num_mips == 1);

    for (u32 mip = 0; mip < num_mips; ++mip) {
        const auto& src_mip = src_info.mips_layout[mip];
        const auto& dst_mip = info.mips_layout[mip];
        if (src_mip.offset != dst_mip.offset || src_mip.pitch != dst_mip.pitch ||
            src_mip.height != dst_mip.height) {
            LOG_WARNING(Render_Vulkan,
                        "Skipping buffer-assisted image copy with incompatible mip layout "
                        "src_addr={:#x} dst_addr={:#x} mip={} src_offset={} dst_offset={} "
                        "src_pitch={} dst_pitch={} src_height={} dst_height={}",
                        src_info.guest_address, info.guest_address, mip, src_mip.offset,
                        dst_mip.offset, src_mip.pitch, dst_mip.pitch, src_mip.height,
                        dst_mip.height);
            return;
        }
    }

    SetBackingSamples(info.num_samples, false);
    src_image.SetBackingSamples(src_info.num_samples);

    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < num_mips; ++mip) {
        const auto mip_w = std::max(src_info.size.width >> mip, 1u);
        const auto mip_h = std::max(src_info.size.height >> mip, 1u);
        const auto mip_d = std::max(src_info.size.depth >> mip, 1u);

        const auto& mip_info = src_info.mips_layout[mip];
        buffer_copies.emplace_back(vk::BufferImageCopy{
            .bufferOffset = offset + mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = src_image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {mip_w, mip_h, mip_d},
        });
    }

    const vk::BufferMemoryBarrier2 pre_copy_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = VK_WHOLE_SIZE,
    };

    const vk::BufferMemoryBarrier2 post_copy_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer,
        .offset = offset,
        .size = VK_WHOLE_SIZE,
    };

    scheduler->EndRendering();
    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});

    auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_copy_barrier,
    });

    VideoCore::Diag::CheckImageSubresources("Image::CopyImageWithBuffer.src",
                                            src_image.info.guest_address,
                                            src_image.info.resources.levels,
                                            src_image.info.resources.layers, buffer_copies);
    cmdbuf.copyImageToBuffer(src_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, buffer,
                             buffer_copies);

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_copy_barrier,
    });

    for (auto& copy : buffer_copies) {
        copy.imageSubresource.aspectMask = aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    }

    VideoCore::Diag::CheckImageSubresources("Image::CopyImageWithBuffer.dst", info.guest_address,
                                            info.resources.levels, info.resources.layers,
                                            buffer_copies);
    cmdbuf.copyBufferToImage(buffer, GetImage(), vk::ImageLayout::eTransferDstOptimal,
                             buffer_copies);
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}

void Image::CopyMip(Image& src_image, u32 mip, u32 slice) {
    const auto& src_info = src_image.info;

    const auto mip_w = std::max(info.size.width >> mip, 1u);
    const auto mip_h = std::max(info.size.height >> mip, 1u);
    const auto mip_d = std::max(info.size.depth >> mip, 1u);
    const auto [src_layers, dst_layers] = SanitizeCopyLayers(src_info, info, mip_d, 0, slice);
    if (src_layers == 0 || dst_layers == 0) {
        return;
    }

    if (!CanDirectCopyImageFormats(src_info, info, src_image.aspect_mask, aspect_mask)) {
        LOG_WARNING(Render_Vulkan,
                    "Skipping incompatible direct mip copy src_addr={:#x} src_size={} "
                    "src_format={} src_samples={} src_aspect={} dst_addr={:#x} dst_size={} "
                    "dst_format={} dst_samples={} dst_aspect={} mip={} slice={}",
                    src_info.guest_address, src_info.guest_size,
                    vk::to_string(src_info.pixel_format), src_info.num_samples,
                    vk::to_string(src_image.aspect_mask), info.guest_address, info.guest_size,
                    vk::to_string(info.pixel_format), info.num_samples, vk::to_string(aspect_mask),
                    mip, slice);
        return;
    }

    ASSERT(mip_w == src_info.size.width);
    ASSERT(mip_h == src_info.size.height);

    const vk::ImageCopy image_copy{
        .srcSubresource{
            .aspectMask = src_image.aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = src_layers,
        },
        .dstSubresource{
            .aspectMask = aspect_mask,
            .mipLevel = mip,
            .baseArrayLayer = slice,
            .layerCount = dst_layers,
        },
        .extent = {mip_w, mip_h, mip_d},
    };

    SetBackingSamples(info.num_samples);
    src_image.SetBackingSamples(src_info.num_samples);

    scheduler->EndRendering();
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});

    const auto cmdbuf = scheduler->CommandBuffer();
    RecordGpuCommandDiagnostic("copy_image src_addr=0x%llx dst_addr=0x%llx",
                               static_cast<unsigned long long>(src_info.guest_address),
                               static_cast<unsigned long long>(info.guest_address));
    cmdbuf.copyImage(src_image.GetImage(), src_image.backing->state.layout, GetImage(),
                     backing->state.layout, image_copy);
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}

void Image::Resolve(Image& src_image, const VideoCore::SubresourceRange& mrt0_range,
                    const VideoCore::SubresourceRange& mrt1_range) {
    SetBackingSamples(1, false);
    scheduler->EndRendering();

    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                      mrt0_range);
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, mrt1_range);

    const auto [src_layers, dst_layers] =
        SanitizeCopyLayers(src_image.info, info, 1, mrt0_range.base.layer, mrt1_range.base.layer);
    if (src_layers == 0 || dst_layers == 0) {
        return;
    }
    const vk::Extent3D resolve_extent = {
        std::min(src_image.info.size.width, info.size.width),
        std::min(src_image.info.size.height, info.size.height),
        1,
    };
    if (src_image.backing->num_samples == 1) {
        const vk::ImageCopy region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt0_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt1_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = resolve_extent,
        };
        scheduler->CommandBuffer().copyImage(src_image.GetImage(),
                                             vk::ImageLayout::eTransferSrcOptimal, GetImage(),
                                             vk::ImageLayout::eTransferDstOptimal, region);
    } else {
        const vk::ImageResolve region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt0_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt1_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = resolve_extent,
        };
        scheduler->CommandBuffer().resolveImage(src_image.GetImage(),
                                                vk::ImageLayout::eTransferSrcOptimal, GetImage(),
                                                vk::ImageLayout::eTransferDstOptimal, region);
    }

    flags |= VideoCore::ImageFlagBits::GpuModified;
    flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Image::Clear(const vk::ClearValue& clear_value, const VideoCore::SubresourceRange& range) {
    ASSERT_MSG(!info.props.is_depth, "Use a depth/stencil clear path for depth images");
    const vk::ImageSubresourceRange vk_range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = range.base.level,
        .levelCount = range.extent.levels,
        .baseArrayLayer = range.base.layer,
        .layerCount = range.extent.layers,
    };
    VideoCore::Diag::CheckSubresourceRange("Image::Clear", info.guest_address,
                                           info.resources.levels, info.resources.layers,
                                           range.base.level, range.extent.levels, range.base.layer,
                                           range.extent.layers);
    scheduler->EndRendering();
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
    const auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.clearColorImage(GetImage(), vk::ImageLayout::eTransferDstOptimal, clear_value.color,
                           vk_range);
}

void Image::SetBackingSamples(u32 num_samples, bool copy_backing) {
    if (!backing || backing->num_samples == num_samples) {
        return;
    }
    ASSERT_MSG(!info.props.is_depth, "Swapping samples is only valid for color images");
    BackingImage* new_backing;
    auto it = std::ranges::find(backing_images, num_samples, &BackingImage::num_samples);
    if (it == backing_images.end()) {
        auto new_image_ci = backing->image.image_ci;
        new_image_ci.samples = LiverpoolToVK::NumSamples(num_samples, supported_samples);

        new_backing = &backing_images.emplace_back();
        new_backing->num_samples = num_samples;
        new_backing->image = UniqueImage{instance->GetDevice(), instance->GetAllocator()};
        new_backing->image.Create(new_image_ci);

        Vulkan::SetObjectName(instance->GetDevice(), new_backing->image.image,
                              "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{} (backing)",
                              info.size.width, info.size.height, info.size.depth,
                              AmdGpu::NameOf(info.tile_mode), vk::to_string(info.pixel_format),
                              info.guest_address, info.guest_size, info.resources.layers,
                              info.resources.levels, num_samples);
    } else {
        new_backing = std::addressof(*it);
    }

    if (copy_backing) {
        scheduler->EndRendering();
        if (info.resources.levels != 1 || info.resources.layers != 1) {
            LOG_WARNING(Render_Vulkan,
                        "Skipping sample backing preservation for layered/mipped image "
                        "addr={:#x} levels={} layers={}",
                        info.guest_address, info.resources.levels, info.resources.layers);
            copy_backing = false;
        }
    }

    if (copy_backing) {
        // Transition current backing to shader read layout
        auto barriers =
            GetBarriers(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead,
                        vk::PipelineStageFlagBits2::eFragmentShader, std::nullopt);

        // Transition dest backing to color attachment layout, not caring of previous contents
        constexpr auto dst_stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        constexpr auto dst_access = vk::AccessFlagBits2::eColorAttachmentWrite;
        constexpr auto dst_layout = vk::ImageLayout::eColorAttachmentOptimal;
        barriers.push_back(vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = new_backing->image,
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = info.resources.layers,
            },
        });
        const auto cmdbuf = scheduler->CommandBuffer();
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
            .pImageMemoryBarriers = barriers.data(),
        });

        // Copy between ms and non ms backing images
        blit_helper->CopyBetweenMsImages(
            info.size.width, info.size.height, new_backing->num_samples, info.pixel_format,
            backing->num_samples > 1, backing->image, new_backing->image);

        // Update current layout in tracker to new backings layout
        new_backing->state.layout = dst_layout;
        new_backing->state.access_mask = dst_access;
        new_backing->state.pl_stage = dst_stage;
    }

    backing = new_backing;
}

} // namespace VideoCore
