// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_diagnostics.h"
#include "video_core/renderer_vulkan/vk_gpu_command_diagnostics.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"

#include "video_core/host_shaders/tiling_comp.h"

#include "common/div_ceil.h"

#include <magic_enum/magic_enum.hpp>
#include <vk_mem_alloc.h>

namespace VideoCore {

struct TilingInfo {
    u32 bank_swizzle;
    u32 num_slices;
    u32 num_mips;
    u32 total_elements;
    std::array<ImageInfo::MipInfo, 16> mips;
};

static u32 TilingDispatchGroups(u32 byte_size, u32 num_bits) {
    if (byte_size == 0 || num_bits == 0) {
        return 0;
    }
    const u64 elements = Common::DivCeil(static_cast<u64>(byte_size) * 8, static_cast<u64>(num_bits));
    return static_cast<u32>(Common::DivCeil(elements, 64ULL));
}

static u32 TilingElementCount(u32 byte_size, u32 num_bits) {
    if (byte_size == 0 || num_bits == 0) {
        return 0;
    }
    return static_cast<u32>(
        Common::DivCeil(static_cast<u64>(byte_size) * 8, static_cast<u64>(num_bits)));
}

TileManager::TileManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         StreamBuffer& stream_buffer_)
    : instance{instance}, scheduler{scheduler}, stream_buffer{stream_buffer_} {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto desc_layout_result = device.createDescriptorSetLayoutUnique(desc_layout_ci);
    if (desc_layout_result.result != vk::Result::eSuccess) {
        Vulkan::RecordGpuCommandDiagnostic("tile_descriptor_layout_failed result=%s",
                                           vk::to_string(desc_layout_result.result).c_str());
        Vulkan::DumpGpuCommandDiagnostics("tile_descriptor_layout_failed");
    }
    ASSERT_MSG(desc_layout_result.result == vk::Result::eSuccess,
               "Failed to create descriptor set layout: {}",
               vk::to_string(desc_layout_result.result));
    desc_layout = std::move(desc_layout_result.value);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0U,
        .pPushConstantRanges = nullptr,
    };
    auto [layout_result, layout] = device.createPipelineLayoutUnique(layout_info);
    if (layout_result != vk::Result::eSuccess) {
        Vulkan::RecordGpuCommandDiagnostic("tile_pipeline_layout_failed result=%s",
                                           vk::to_string(layout_result).c_str());
        Vulkan::DumpGpuCommandDiagnostics("tile_pipeline_layout_failed");
    }
    ASSERT_MSG(layout_result == vk::Result::eSuccess, "Failed to create pipeline layout: {}",
               vk::to_string(layout_result));
    pl_layout = std::move(layout);
}

TileManager::~TileManager() = default;

TileManager::ScratchBuffer TileManager::GetScratchBuffer(u32 size) {
    constexpr auto usage =
        vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;

    const vk::BufferCreateInfo buffer_ci = {
        .size = size,
        .usage = usage,
    };

    const VmaAllocationCreateInfo alloc_info{
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkBuffer buffer;
    VmaAllocation allocation;
    const auto buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
    const auto result = vmaCreateBuffer(instance.GetAllocator(), &buffer_ci_unsafe, &alloc_info,
                                        &buffer, &allocation, nullptr);
    if (result != VK_SUCCESS) {
        Vulkan::RecordGpuCommandDiagnostic("tile_scratch_buffer_alloc_failed result=%s size=%llu",
                                           vk::to_string(vk::Result{result}).c_str(),
                                           static_cast<unsigned long long>(size));
        Vulkan::DumpGpuCommandDiagnostics("tile_scratch_buffer_alloc_failed");
    }
    ASSERT(result == VK_SUCCESS);
    return {buffer, allocation};
}

vk::Pipeline TileManager::GetTilingPipeline(const ImageInfo& info, bool is_tiler) {
    const TilingPipelineKey key{
        .tile_mode = info.tile_mode,
        .array_mode = info.array_mode,
        .num_bits = info.num_bits,
        .num_samples = info.num_samples,
        .is_tiler = is_tiler,
    };
    auto [it, inserted] = tiling_pipelines.try_emplace(key);
    if (!inserted && *it->second != VK_NULL_HANDLE) {
        return *it->second;
    }
    auto& pipeline_slot = it->second;
    if (auto pipeline = *pipeline_slot; pipeline != VK_NULL_HANDLE) {
        return pipeline;
    }

    const auto device = instance.GetDevice();
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    std::vector<std::string> defines = {
        fmt::format("BITS_PER_PIXEL={}", info.num_bits),
        fmt::format("NUM_SAMPLES={}", info.num_samples),
        fmt::format("ARRAY_MODE={}", u32(info.array_mode)),
        fmt::format("MICRO_TILE_MODE={}", u32(micro_tile_mode)),
        fmt::format("MICRO_TILE_THICKNESS={}", AmdGpu::GetMicroTileThickness(info.array_mode)),
    };
    if (AmdGpu::IsMacroTiled(info.array_mode)) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        const u32 num_banks = AmdGpu::GetNumBanks(macro_tile_mode);
        defines.emplace_back(
            fmt::format("PIPE_CONFIG={}", u32(AmdGpu::GetPipeConfig(info.tile_mode))));
        defines.emplace_back(fmt::format("BANK_WIDTH={}", AmdGpu::GetBankWidth(macro_tile_mode)));
        defines.emplace_back(fmt::format("BANK_HEIGHT={}", AmdGpu::GetBankHeight(macro_tile_mode)));
        defines.emplace_back(fmt::format("NUM_BANKS={}", num_banks));
        defines.emplace_back(fmt::format("NUM_BANK_BITS={}", std::bit_width(num_banks) - 1));
        defines.emplace_back(fmt::format(
            "TILE_SPLIT_BYTES={}", AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode,
                                                              micro_tile_mode, info.num_bits)));
        defines.emplace_back(
            fmt::format("MACRO_TILE_ASPECT={}", AmdGpu::GetMacrotileAspect(macro_tile_mode)));
    }
    if (is_tiler) {
        defines.emplace_back(fmt::format("IS_TILER=1"));
    }

    const auto& module = Vulkan::Compile(HostShaders::TILING_COMP,
                                         vk::ShaderStageFlagBits::eCompute, device, defines);
    const auto module_name = fmt::format("{}_{} {}", magic_enum::enum_name(info.tile_mode),
                                         info.num_bits, is_tiler ? "tiler" : "detiler");
    LOG_INFO(Render_Vulkan, "Compiling shader {}", module_name);
    for (const auto& def : defines) {
        LOG_INFO(Render_Vulkan, "#define {}", def);
    }
    Vulkan::SetObjectName(device, module, module_name);
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pl_layout,
    };
    auto [result, pipeline] =
        device.createComputePipelineUnique(VK_NULL_HANDLE, compute_pipeline_ci);
    if (result != vk::Result::eSuccess) {
        Vulkan::RecordGpuCommandDiagnostic(
            "tile_pipeline_create_failed result=%s tile_mode=%u array_mode=%u bpp=%u samples=%u "
            "is_tiler=%u",
            vk::to_string(result).c_str(), static_cast<u32>(info.tile_mode),
            static_cast<u32>(info.array_mode), info.num_bits, info.num_samples, is_tiler ? 1 : 0);
        Vulkan::DumpGpuCommandDiagnostics("tile_pipeline_create_failed");
    }
    ASSERT_MSG(result == vk::Result::eSuccess, "Detiler pipeline creation failed {}",
               vk::to_string(result));
    pipeline_slot = std::move(pipeline);
    device.destroyShaderModule(module);
    return *pipeline_slot;
}

TileManager::Result TileManager::DetileImage(vk::Buffer in_buffer, u32 in_offset,
                                             const ImageInfo& info) {
    if (!info.props.is_tiled) {
        return {in_buffer, in_offset};
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = std::min<u32>(info.resources.levels, params.mips.size());
    params.total_elements = TilingElementCount(info.guest_size, info.num_bits);
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto [out_buffer, out_allocation] = GetScratchBuffer(info.guest_size);
    // The detiled scratch buffer is returned to the caller and consumed by a later
    // image.Upload (which ends rendering and records into a *new* command buffer at a
    // later tick). Deferring its destruction here, at the detile tick, would free it
    // while that later Upload command buffer still references it -> MoltenVK kIOGPU
    // Invalid Resource / device loss. Stash it instead; ReleasePendingScratchBuffers()
    // is called by the caller after Upload to defer-destroy it at the correct tick.
    pending_scratch_buffers.emplace_back(out_buffer, out_allocation);

    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, GetTilingPipeline(info, false));

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = in_buffer,
        .offset = in_offset,
        .range = info.guest_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = out_buffer,
        .offset = 0,
        .range = info.guest_size,
    };

    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &linear_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pl_layout, 0, set_writes);

    const auto dim_x = TilingDispatchGroups(info.guest_size, info.num_bits);
    if (dim_x == 0) {
        return {out_buffer, 0};
    }
    VideoCore::Diag::CheckDispatch("TileManager.Detile", dim_x, 1, 1);
    cmdbuf.dispatch(dim_x, 1, 1);
    return {out_buffer, 0};
}

void TileManager::ReleasePendingScratchBuffers() {
    if (pending_scratch_buffers.empty()) {
        return;
    }
    // Registered at the current tick, which (thanks to the caller invoking this after the
    // consuming command was recorded) is the buffer's true last-use tick. The scheduler's
    // PopPendingOperations gate guarantees this destruction does not run until that
    // recording has been submitted and completed.
    for (const auto& [buffer, allocation] : pending_scratch_buffers) {
        // DeferOperationAfterSubmit (CurrentTick()+1), not DeferOperation: this scratch buffer
        // feeds an image that is consumed cross-scheduler (the VideoOut flip path refreshes the
        // image on draw_scheduler but blits it on flip_scheduler). Gating the destroy one extra
        // tick guarantees not only that draw_scheduler's recording is submitted+completed, but
        // that the next submit boundary has passed, closing the cross-scheduler window where a
        // different scheduler's in-flight command buffer still references the resource. This is a
        // pure delay of the free; it never reuses or early-releases the buffer contents.
        scheduler.DeferOperationAfterSubmit([this, buffer, allocation]() {
            vmaDestroyBuffer(instance.GetAllocator(), buffer, allocation);
        });
    }
    pending_scratch_buffers.clear();
}

void TileManager::TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            Buffer& out_buffer, u32 out_offset, u32 copy_size) {
    const auto& info = in_image.info;
    if (!info.props.is_tiled) {
        for (auto& copy : buffer_copies) {
            copy.bufferOffset += out_offset;
        }
        in_image.Download(buffer_copies, out_buffer.Handle(), out_offset, copy_size);
        if (auto barrier =
                out_buffer.GetBarrier(vk::AccessFlagBits2::eMemoryRead |
                                           vk::AccessFlagBits2::eMemoryWrite,
                                       vk::PipelineStageFlagBits2::eAllCommands, out_offset)) {
            scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
                .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                .bufferMemoryBarrierCount = 1,
                .pBufferMemoryBarriers = &*barrier,
            });
        }
        return;
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = std::min<u32>(static_cast<u32>(buffer_copies.size()), params.mips.size());
    const u32 valid_size = std::min(copy_size, info.guest_size);
    params.total_elements = TilingElementCount(valid_size, info.num_bits);
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto [temp_buffer, temp_allocation] = GetScratchBuffer(info.guest_size);
    // DeferOperationAfterSubmit (CurrentTick()+1): the destroy is registered here, before the
    // tiling compute that consumes temp_buffer is recorded below. Deferring to CurrentTick()
    // alone can be reclaimed by PopPendingOperations as soon as a prior frame's GPU tick reaches
    // CurrentTick(), freeing the buffer while this not-yet-submitted recording still references
    // it. The extra tick guarantees this recording is submitted+completed first. Pure delay of
    // the free; contents are never reused or early-released.
    scheduler.DeferOperationAfterSubmit([this, temp_buffer, temp_allocation]() {
        vmaDestroyBuffer(instance.GetAllocator(), temp_buffer, temp_allocation);
    });

    const auto cmdbuf = scheduler.CommandBuffer();
    in_image.Download(buffer_copies, temp_buffer, 0, valid_size);

    if (auto barrier = out_buffer.GetBarrier(vk::AccessFlagBits2::eShaderWrite,
                                             vk::PipelineStageFlagBits2::eComputeShader,
                                             out_offset)) {
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &*barrier,
        });
    }

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, GetTilingPipeline(info, true));

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = out_buffer.Handle(),
        .offset = out_offset,
        .range = valid_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = temp_buffer,
        .offset = 0,
        .range = valid_size,
    };

    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &linear_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pl_layout, 0, set_writes);

    const auto dim_x = TilingDispatchGroups(valid_size, info.num_bits);
    if (dim_x == 0) {
        return;
    }
    VideoCore::Diag::CheckDispatch("TileManager.Tile", dim_x, 1, 1);
    cmdbuf.dispatch(dim_x, 1, 1);

    if (auto barrier =
            out_buffer.GetBarrier(vk::AccessFlagBits2::eMemoryRead |
                                      vk::AccessFlagBits2::eMemoryWrite,
                                  vk::PipelineStageFlagBits2::eAllCommands, out_offset)) {
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &*barrier,
        });
    }
}

} // namespace VideoCore
