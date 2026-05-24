// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/trace_control.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_gpu_command_diagnostics.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

namespace Vulkan {

static bool IsTraceRenderEnabled() {
    return Common::Trace::IsAggressiveLoggingEnabled();
}

static bool IsStrictRenderValidationEnabled() {
    static const bool enabled = Common::Trace::EnvEnabled("SHADPS4_STRICT_RENDER_VALIDATION");
    return enabled;
}

static bool ShouldAbortMetadataTextureRead() {
    static const bool enabled =
        Common::Trace::EnvEnabled("SHADPS4_STRICT_METADATA_TEXTURE_READ_ABORT");
    return IsStrictRenderValidationEnabled() && enabled;
}

static bool IsFmaskDecompressResolveEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_FMASK_DECOMPRESS_AS_RESOLVE");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static bool IsFmaskDecompressInPlaceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_FMASK_DECOMPRESS_IN_PLACE");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static bool IsNullMetaTextureReadEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_NULL_METADATA_TEXTURE_READS");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static bool IsNullFmaskTextureReadEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_NULL_FMASK_TEXTURE_READS");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static bool IsCompositorNullLayerEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_COMPOSITOR_NULL_LAYER");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static bool IsCompositorZeroLayerEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_COMPOSITOR_ZERO_LAYER");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static bool IsForceVideoOutStorageColorEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_FORCE_VIDEOOUT_STORAGE_COLOR");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static bool IsForceVideoOutSourceColorsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_FORCE_VIDEOOUT_SOURCE_COLORS");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static bool IsComputeMetaClearHleDisabled() {
    static const bool disabled =
        Common::Trace::EnvEnabled("SHADPS4_DISABLE_COMPUTE_META_CLEAR_HLE");
    return disabled;
}

static bool IsComputeImageClearHleDisabled() {
    static const bool disabled =
        Common::Trace::EnvEnabled("SHADPS4_DISABLE_COMPUTE_IMAGE_CLEAR_HLE");
    return disabled;
}

static const char* MetaTypeName(VideoCore::TextureCache::MetaDataInfo::Type type) {
    switch (type) {
    case VideoCore::TextureCache::MetaDataInfo::Type::CMask:
        return "cmask";
    case VideoCore::TextureCache::MetaDataInfo::Type::FMask:
        return "fmask";
    case VideoCore::TextureCache::MetaDataInfo::Type::HTile:
        return "htile";
    }
    return "unknown";
}

static const char* BindingTypeName(VideoCore::TextureCache::BindingType type) {
    switch (type) {
    case VideoCore::TextureCache::BindingType::Texture:
        return "texture";
    case VideoCore::TextureCache::BindingType::Storage:
        return "storage";
    case VideoCore::TextureCache::BindingType::RenderTarget:
        return "render_target";
    case VideoCore::TextureCache::BindingType::DepthTarget:
        return "depth_target";
    case VideoCore::TextureCache::BindingType::VideoOut:
        return "video_out";
    }
    return "unknown";
}

static bool IsLikelyVideoOutStorageImage(const VideoCore::Image& image) {
    return image.info.size.width == 1920 && image.info.size.height == 1080 &&
           image.info.num_samples == 1 && image.info.guest_size >= 8'000'000 &&
           image.info.guest_size <= 9'000'000 && image.info.guest_address != 0;
}

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, page_manager{this},
      buffer_cache{instance, scheduler, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool} {
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);
}

Rasterizer::~Rasterizer() = default;

void Rasterizer::CpSync() {
    scheduler.EndRendering();
    auto cmdbuf = scheduler.CommandBuffer();

    const vk::MemoryBarrier ib_barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eDrawIndirect,
                           vk::DependencyFlagBits::eByRegion, ib_barrier, {}, {});
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->regs;
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        const bool can_resolve =
            IsFmaskDecompressResolveEnabled() && regs.color_buffers[0] && regs.color_buffers[1];
        const bool can_decompress_in_place =
            (IsFmaskDecompressInPlaceEnabled() || IsStrictRenderValidationEnabled()) &&
            regs.color_buffers[0] && !regs.color_buffers[1];
        if (IsTraceRenderEnabled()) {
            static std::atomic<u64> fmask_decompress_count{};
            const u64 count = fmask_decompress_count.fetch_add(1, std::memory_order_relaxed) + 1;
            const char* action =
                can_resolve ? "resolve"
                            : can_decompress_in_place ? "decompress_in_place" : "skip_no_mrt1";
            const bool should_log = count <= 16 || (count % 600) == 0;
            if (should_log) {
                LOG_INFO(Render_Vulkan,
                         "TRACE_RENDER fmask_decompress count={} mrt0={:#x} mrt1={:#x} "
                         "samples0={} samples1={} action={}",
                         count, regs.color_buffers[0].Address(), regs.color_buffers[1].Address(),
                         regs.color_buffers[0].NumSamples(), regs.color_buffers[1].NumSamples(),
                         action);
            }
        }
        if (can_resolve) {
            Resolve();
        } else if (can_decompress_in_place) {
            const auto& mrt0_hint = liverpool->last_cb_extent[0];
            VideoCore::TextureCache::ImageDesc mrt0_desc{regs.color_buffers[0], mrt0_hint};
            auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
            mrt0_image.SetBackingSamples(1);
        } else if (regs.color_buffers[0] && !regs.color_buffers[1]) {
            static std::atomic_bool logged_missing_mrt1{};
            if (!logged_missing_mrt1.exchange(true, std::memory_order_relaxed)) {
                LOG_WARNING(Render_Vulkan,
                            "TRACE_RENDER fmask_decompress_missing_mrt1 mrt0={:#x} "
                            "samples0={} action=skip",
                            regs.color_buffers[0].Address(), regs.color_buffers[0].NumSamples());
            }
        } else {
            LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        }
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->regs;
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = liverpool->last_cb_extent[cb];
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->last_db_extent;
        auto& [image_id, desc] = db_desc;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
}

static std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Regs& regs, const Shader::Info& info,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        const auto valid_sgpr = [](s8 sgpr) {
            return sgpr >= 0 && static_cast<size_t>(sgpr) < Shader::NUM_USER_DATA_REGS;
        };
        if (vertex_offset == 0 && valid_sgpr(fetch_shader->vertex_offset_sgpr)) {
            vertex_offset = info.user_data[fetch_shader->vertex_offset_sgpr];
        }
        if (valid_sgpr(fetch_shader->instance_offset_sgpr)) {
            instance_offset = info.user_data[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->regs.color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->last_cb_extent[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    image.Clear(clear_value, desc.view_info.range);
    ScopeMarkerEnd();
}

static bool HasValidRenderAttachment(const RenderState& state) {
    for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
        if (state.color_attachments[cb].image_view) {
            return true;
        }
    }
    return state.depth_stencil_attachment.has_depth || state.depth_stencil_attachment.has_stencil;
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        return;
    }

    const auto& regs = liverpool->regs;
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);
    if (!HasValidRenderAttachment(state)) {
        LOG_WARNING(Render_Vulkan, "Skipping draw with no valid render attachments");
        return;
    }

    buffer_cache.BindVertexBuffers(*pipeline);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(index_offset);
    }

    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    const auto& vs_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        RecordGpuCommandDiagnostic(
            "draw_indexed num_indices=%u instances=%u vertex_offset=%u instance_offset=%u",
            regs.num_indices, regs.num_instances.NumInstances(), vertex_offset, instance_offset);
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        RecordGpuCommandDiagnostic("draw num_vertices=%u instances=%u vertex_offset=%u "
                                   "instance_offset=%u",
                                   regs.num_indices, regs.num_instances.NumInstances(),
                                   vertex_offset, instance_offset);
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }

    ResetBindings();
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        return;
    }

    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);
    if (!HasValidRenderAttachment(state)) {
        LOG_WARNING(Render_Vulkan, "Skipping indirect draw with no valid render attachments");
        return;
    }

    buffer_cache.BindVertexBuffers(*pipeline);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(0);
    }

    const auto& [buffer, base] =
        buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);

    VideoCore::Buffer* count_buffer{};
    u32 count_base{};
    if (count_address != 0) {
        std::tie(count_buffer, count_base) = buffer_cache.ObtainBuffer(count_address, 4, false);
    }

    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    // We can safely ignore both SGPR UD indices and results of fetch shader parsing, as vertex and
    // instance offsets will be automatically applied by Vulkan from indirect args buffer.

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            RecordGpuCommandDiagnostic("draw_indexed_indirect_count arg=0x%llx offset=%u stride=%u "
                                       "max_count=%u count=0x%llx",
                                       static_cast<unsigned long long>(arg_address), offset, stride,
                                       max_count, static_cast<unsigned long long>(count_address));
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_base, max_count, stride);
        } else {
            RecordGpuCommandDiagnostic("draw_indexed_indirect arg=0x%llx offset=%u stride=%u "
                                       "max_count=%u",
                                       static_cast<unsigned long long>(arg_address), offset, stride,
                                       max_count);
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            RecordGpuCommandDiagnostic("draw_indirect_count arg=0x%llx offset=%u stride=%u "
                                       "max_count=%u count=0x%llx",
                                       static_cast<unsigned long long>(arg_address), offset, stride,
                                       max_count, static_cast<unsigned long long>(count_address));
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_base,
                                     max_count, stride);
        } else {
            RecordGpuCommandDiagnostic("draw_indirect arg=0x%llx offset=%u stride=%u max_count=%u",
                                       static_cast<unsigned long long>(arg_address), offset, stride,
                                       max_count);
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
    }

    ResetBindings();
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (ExecuteShaderHLE(cs, liverpool->regs, cs_program, *this)) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    if (cs_program.dim_x == 0 || cs_program.dim_y == 0 || cs_program.dim_z == 0) {
        LOG_WARNING(Render_Vulkan, "Skipping zero-sized compute dispatch x={} y={} z={}",
                    cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
        ResetBindings();
        return;
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    ForceVideoOutSourceColorsIfRequested(cmdbuf, cs);
    RecordGpuCommandDiagnostic("dispatch x=%u y=%u z=%u", cs_program.dim_x, cs_program.dim_y,
                               cs_program.dim_z);
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    ForceVideoOutStorageColorIfRequested(cmdbuf, cs, cs_program.dim_x, cs_program.dim_y,
                                         cs_program.dim_z);

    ResetBindings();
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
    ForceVideoOutSourceColorsIfRequested(cmdbuf, cs);
    cmdbuf.dispatchIndirect(buffer->Handle(), base);
    ForceVideoOutStorageColorIfRequested(cmdbuf, cs, 0, 0, 0);

    ResetBindings();
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    if (fault_process_pending) {
        fault_process_pending = false;
        buffer_cache.ProcessFaultBuffer();
    }
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    buffer_cache.RunGarbageCollector();
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_barriers.clear();
    buffer_infos.clear();
    image_infos.clear();
    videoout_storage_probe.reset();
    videoout_source_probes.clear();

    bool uses_dma = false;

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(liverpool->regs);
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          stage->samplers.size());
        stage->PushUd(binding, push_data);
        BindBuffers(*stage, binding, push_data);
        BindTextures(*stage, binding);
        uses_dma |= stage->uses_dma;
    }

    if (uses_dma) {
        // We only use fault buffer for DMA right now.
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (auto& range : mapped_ranges) {
            buffer_cache.SynchronizeBuffersInRange(range.lower(), range.upper() - range.lower());
        }
        fault_process_pending = true;
    }

    return true;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (IsComputeMetaClearHleDisabled()) {
        return false;
    }
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_address != buf0.base_address ||
        image1.info.guest_address != buf1.base_address ||
        image0.info.guest_size != buf0.GetSize() || image1.info.guest_size != buf1.GetSize() ||
        image0.info.guest_size != image1.info.guest_size || image0.info.pitch != image1.info.pitch ||
        image0.info.num_bits != image1.info.num_bits ||
        image0.info.pixel_format != image1.info.pixel_format ||
        image0.info.num_samples != image1.info.num_samples || image0.info.type != image1.info.type ||
        image0.info.resources.levels != image1.info.resources.levels ||
        image0.info.resources.layers != image1.info.resources.layers ||
        image0.info.props.is_depth != image1.info.props.is_depth ||
        image0.aspect_mask != image1.aspect_mask) {
        LOG_TRACE(Render_Vulkan,
                  "Compute image-copy HLE rejected non-exact alias src_addr={:#x} dst_addr={:#x}",
                  buf0.base_address, buf1.base_address);
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    if (instance.IsMaintenance8Supported() ||
        src_image.info.props.is_depth == dst_image.info.props.is_depth) {
        dst_image.CopyImage(src_image);
    } else {
        const auto& copy_buffer =
            buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::DeviceLocal);
        dst_image.CopyImageWithBuffer(src_image, copy_buffer.Handle(), 0);
    }
    dst_image.flags |= VideoCore::ImageFlagBits::GpuModified;
    dst_image.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (IsComputeImageClearHleDisabled()) {
        return false;
    }
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_address != buf1.base_address || image1.info.guest_size != buf1.GetSize() ||
        image1.info.num_bits != buf1_bpp || image1.info.props.is_depth ||
        !memory->IsValidMapping(buf0.base_address, 16)) {
        return false;
    }

    // Perform image clear
    std::array<float, 4> values{};
    std::memcpy(values.data(), reinterpret_cast<const void*>(buf0.base_address), sizeof(values));
    const vk::ClearValue clear = {
        .color = {.float32 = values},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    image1.Clear(clear, range);
    image1.flags |= VideoCore::ImageFlagBits::GpuModified;
    image1.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    buffer_bindings.clear();

    for (const auto& desc : stage.buffers) {
        const auto vsharp = desc.GetSharp(stage);
        if (!desc.IsSpecial() && vsharp.base_address != 0 && vsharp.GetSize() > 0) {
            const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
            const auto buffer_id = buffer_cache.FindBuffer(vsharp.base_address, size);
            buffer_bindings.emplace_back(buffer_id, vsharp, size);
        } else {
            buffer_bindings.emplace_back(VideoCore::BufferId{}, vsharp, 0);
        }
    }

    // Second pass to re-bind buffers that were updated after binding
    for (u32 i = 0; i < buffer_bindings.size(); i++) {
        const auto& [buffer_id, vsharp, size] = buffer_bindings[i];
        const auto& desc = stage.buffers[i];
        const bool is_storage = desc.IsStorage(vsharp);
        const u32 alignment =
            is_storage ? instance.StorageMinAlignment() : instance.UniformMinAlignment();
        // Buffer is not from the cache, either a special buffer or unbound.
        if (!buffer_id) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const u32 ubo_size = stage.flattened_ud_buf.size() * sizeof(u32);
                const u64 offset =
                    vk_buffer.Copy(stage.flattened_ud_buf.data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const auto& cs_program = liverpool->GetCsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                if (data != nullptr) {
                    std::memset(data, 0, lds_size);
                    lds_buffer.Commit();
                    buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
                } else if (instance.IsNullDescriptorSupported()) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& null_buffer = buffer_cache.GetBuffer(VideoCore::NULL_BUFFER_ID);
                    buffer_infos.emplace_back(null_buffer.Handle(), 0, VK_WHOLE_SIZE);
                }
            } else if (instance.IsNullDescriptorSupported()) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                auto& null_buffer = buffer_cache.GetBuffer(VideoCore::NULL_BUFFER_ID);
                buffer_infos.emplace_back(null_buffer.Handle(), 0, VK_WHOLE_SIZE);
            }
        } else {
            const auto [vk_buffer, offset] = buffer_cache.ObtainBuffer(
                vsharp.base_address, size, desc.is_written, desc.is_formatted, buffer_id);
            const u32 offset_aligned = Common::AlignDown(offset, alignment);
            const u32 adjust = offset - offset_aligned;
            ASSERT(adjust % 4 == 0);
            push_data.AddOffset(binding.buffer, adjust);
            buffer_infos.emplace_back(vk_buffer->Handle(), offset_aligned, size + adjust);
            if (auto barrier =
                    vk_buffer->GetBarrier(desc.is_written ? vk::AccessFlagBits2::eShaderWrite
                                                          : vk::AccessFlagBits2::eShaderRead,
                                          vk::PipelineStageFlagBits2::eAllCommands)) {
                buffer_barriers.emplace_back(*barrier);
            }
            if (desc.is_written && desc.is_formatted) {
                texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
            }
        }

        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageBuffer : vk::DescriptorType::eUniformBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    image_bindings.clear();
    const u32 first_image_idx = image_infos.size();
    // For loading/storing to explicit mip levels, when no native instruction support, bind an array
    // of descriptors consecutively, 1 for each mip level. The shader can index this with LOD
    // operand.
    // This array holds the size of each consecutive array with the number of bindings consumed.
    // This is currently always 1 for anything other than mip fallback arrays.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;

    for (const auto& image_desc : stage.images) {
        const auto tsharp = image_desc.GetSharp(stage);
        if (texture_cache.IsMeta(tsharp.Address())) {
            const auto* meta = texture_cache.FindMetaData(tsharp.Address());
            const bool null_fmask_read =
                (IsNullFmaskTextureReadEnabled() || IsStrictRenderValidationEnabled()) && meta &&
                meta->type == VideoCore::TextureCache::MetaDataInfo::Type::FMask &&
                !image_desc.is_written;
            const bool null_meta_read =
                !null_fmask_read &&
                (IsNullMetaTextureReadEnabled() || IsStrictRenderValidationEnabled()) &&
                !image_desc.is_written;
            const char* action =
                null_fmask_read ? "null_fmask_texture"
                                : null_meta_read ? "null_metadata_texture" : "sample_metadata";
            if (IsTraceRenderEnabled()) {
                static std::atomic<u64> metadata_texture_read_count{};
                const u64 count =
                    metadata_texture_read_count.fetch_add(1, std::memory_order_relaxed) + 1;
                const bool should_log = count <= 32 || (count % 300) == 0;
                if (meta && should_log) {
                    LOG_WARNING(Render_Vulkan,
                                "TRACE_RENDER metadata_texture_read count={} addr={:#x} kind={} "
                                "action={} "
                                "owner_image={} owner_binding={} owner_guest_addr={:#x} "
                                "owner_guest_size={} owner_size={}x{}x{} owner_pitch={} "
                                "owner_vk_format={} owner_tile_mode={} owner_array_mode={} "
                                "owner_bits={} owner_samples={} clear_mask={:#x} data_fmt={} "
                                "num_fmt={} type={} width={} height={} depth={} pitch={} mips={} "
                                "is_written={}",
                                count, tsharp.Address(), MetaTypeName(meta->type), action,
                                meta->owner_image_id.index, BindingTypeName(meta->owner_binding),
                                meta->owner_guest_address, meta->owner_guest_size,
                                meta->owner_size.width, meta->owner_size.height,
                                meta->owner_size.depth, meta->owner_pitch,
                                static_cast<u32>(meta->owner_format),
                                static_cast<u32>(meta->owner_tile_mode),
                                static_cast<u32>(meta->owner_array_mode), meta->owner_num_bits,
                                meta->owner_num_samples, static_cast<u32>(meta->clear_mask),
                                static_cast<u32>(tsharp.GetDataFmt()),
                                static_cast<u32>(tsharp.GetNumberFmt()),
                                static_cast<u32>(tsharp.GetType()),
                                static_cast<u32>(tsharp.width + 1),
                                static_cast<u32>(tsharp.height + 1),
                                static_cast<u32>(tsharp.depth + 1), tsharp.Pitch(),
                                tsharp.NumLevels(), image_desc.is_written);
                } else if (should_log) {
                    LOG_WARNING(Render_Vulkan,
                                "TRACE_RENDER metadata_texture_read count={} addr={:#x} "
                                "kind=unknown action={} data_fmt={} num_fmt={} type={} width={} "
                                "height={} depth={} pitch={} mips={} is_written={}",
                                count, tsharp.Address(), action,
                                static_cast<u32>(tsharp.GetDataFmt()),
                                static_cast<u32>(tsharp.GetNumberFmt()),
                                static_cast<u32>(tsharp.GetType()),
                                static_cast<u32>(tsharp.width + 1),
                                static_cast<u32>(tsharp.height + 1),
                                static_cast<u32>(tsharp.depth + 1), tsharp.Pitch(),
                                tsharp.NumLevels(), image_desc.is_written);
                }
            } else {
                LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
            }
            ASSERT_MSG(!ShouldAbortMetadataTextureRead() || null_fmask_read || null_meta_read,
                       "Strict render validation: shader samples metadata texture addr={:#x} "
                       "kind={} action={} stage={} pgm={:#x} data_fmt={} num_fmt={} type={} "
                       "width={} height={} depth={} pitch={} mips={} is_written={}",
                       tsharp.Address(), meta ? MetaTypeName(meta->type) : "unknown", action,
                       stage.stage, stage.pgm_hash, static_cast<u32>(tsharp.GetDataFmt()),
                       static_cast<u32>(tsharp.GetNumberFmt()), static_cast<u32>(tsharp.GetType()),
                       static_cast<u32>(tsharp.width + 1), static_cast<u32>(tsharp.height + 1),
                       static_cast<u32>(tsharp.depth + 1), tsharp.Pitch(), tsharp.NumLevels(),
                       image_desc.is_written);
            if (null_fmask_read) {
                VideoCore::TextureCache::ImageDesc desc{tsharp, image_desc};
                desc.view_info.range.base.level = 0;
                desc.view_info.range.base.layer = 0;
                desc.view_info.range.extent.levels = 1;
                desc.view_info.range.extent.layers = 1;
                image_bindings.emplace_back(texture_cache.GetNullImage(desc.info.pixel_format), desc);
                image_descriptor_array_sizes.push_back(1);
                continue;
            }
            if (null_meta_read) {
                VideoCore::TextureCache::ImageDesc desc{tsharp, image_desc};
                desc.view_info.range.base.level = 0;
                desc.view_info.range.base.layer = 0;
                desc.view_info.range.extent.levels = 1;
                desc.view_info.range.extent.layers = 1;
                image_bindings.emplace_back(texture_cache.GetNullImage(desc.info.pixel_format), desc);
                image_descriptor_array_sizes.push_back(1);
                continue;
            }
        }

        if (tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid) {
            ASSERT_MSG(!IsStrictRenderValidationEnabled(),
                       "Strict render validation: invalid texture descriptor format stage={} "
                       "pgm={:#x} addr={:#x} num_fmt={} type={} width={} height={} depth={} "
                       "pitch={} mips={} is_written={}",
                       stage.stage, stage.pgm_hash, tsharp.Address(),
                       static_cast<u32>(tsharp.GetNumberFmt()), static_cast<u32>(tsharp.GetType()),
                       static_cast<u32>(tsharp.width + 1), static_cast<u32>(tsharp.height + 1),
                       static_cast<u32>(tsharp.depth + 1), tsharp.Pitch(), tsharp.NumLevels(),
                       image_desc.is_written);
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = std::min<u32>(image_desc.NumBindings(stage), Shader::NUM_IMAGES);

        for (auto i = 0; i < num_bindings; i++) {
            auto& [image_id, desc] = image_bindings.emplace_back(
                std::piecewise_construct, std::tuple{}, std::tuple{tsharp, image_desc});

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            image_id = texture_cache.FindImage(desc);
            if (IsTraceRenderEnabled() && IsCompositorZeroLayerEnabled() &&
                stage.stage == Shader::Stage::Compute &&
                stage.pgm_hash == 0xc455a5aa2c447041ULL && image_bindings.size() == 1 &&
                !image_desc.is_written) {
                desc.view_info.mapping.r = vk::ComponentSwizzle::eZero;
                desc.view_info.mapping.g = vk::ComponentSwizzle::eZero;
                desc.view_info.mapping.b = vk::ComponentSwizzle::eZero;
                desc.view_info.mapping.a = vk::ComponentSwizzle::eZero;
            }
            auto* image = &texture_cache.GetImage(image_id);
            if (image->depth_id) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = image->depth_id;
                image = &texture_cache.GetImage(image_id);
            }
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    const bool trace_image_bindings = IsTraceRenderEnabled();
    bool trace_stage_has_videoout_storage = false;
    u64 trace_videoout_stage_index = 0;
    if (trace_image_bindings) {
        for (const auto& [image_id, desc] : image_bindings) {
            if (!image_id || desc.type != VideoCore::TextureCache::BindingType::Storage) {
                continue;
            }
            if (IsLikelyVideoOutStorageImage(texture_cache.GetImage(image_id))) {
                trace_stage_has_videoout_storage = true;
                break;
            }
        }
        if (trace_stage_has_videoout_storage) {
            static std::atomic<u64> videoout_storage_stage_count{};
            trace_videoout_stage_index =
                videoout_storage_stage_count.fetch_add(1, std::memory_order_relaxed) + 1;
        }
    }

    // Second pass to re-bind images that were updated after binding
    u32 trace_image_binding_idx = 0;
    for (auto& [image_id, desc] : image_bindings) {
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (IsTraceRenderEnabled() && IsCompositorNullLayerEnabled() &&
            stage.stage == Shader::Stage::Compute &&
            stage.pgm_hash == 0xc455a5aa2c447041ULL && trace_image_binding_idx == 0 &&
            !is_storage) {
            LOG_WARNING(Render_Vulkan,
                        "TRACE_RENDER compositor_null_layer binding_index={} old_image_id={} "
                        "desc_guest_addr={:#x} desc_size={}x{}x{} desc_format={}",
                        trace_image_binding_idx, image_id.index, desc.info.guest_address,
                        desc.info.size.width, desc.info.size.height, desc.info.size.depth,
                        vk::to_string(desc.info.pixel_format));
            image_id = texture_cache.GetNullImage(desc.info.pixel_format);
        }
        if (!image_id) {
            ASSERT_MSG(!IsStrictRenderValidationEnabled(),
                       "Strict render validation: null image binding stage={} pgm={:#x} "
                       "binding_index={} descriptor={} desc_guest_addr={:#x} desc_guest_size={} "
                       "desc_size={}x{}x{} desc_pitch={} desc_format={} desc_samples={}",
                       stage.stage, stage.pgm_hash, trace_image_binding_idx,
                       BindingTypeName(desc.type), desc.info.guest_address, desc.info.guest_size,
                       desc.info.size.width, desc.info.size.height, desc.info.size.depth,
                       desc.info.pitch, vk::to_string(desc.info.pixel_format),
                       desc.info.num_samples);
            if (trace_image_bindings && (trace_stage_has_videoout_storage || is_storage)) {
                static std::atomic<u64> null_image_binding_count{};
                const u64 count =
                    null_image_binding_count.fetch_add(1, std::memory_order_relaxed) + 1;
                const bool should_log =
                    trace_stage_has_videoout_storage || count <= 64 || (count % 300) == 0;
                if (should_log) {
                    LOG_WARNING(Render_Vulkan,
                                "TRACE_RENDER image_binding_null count={} videoout_stage={} "
                                "binding_index={} stage={} pgm={:#x} descriptor={} "
                                "desc_guest_addr={:#x} desc_guest_size={} desc_size={}x{}x{} "
                                "desc_pitch={} desc_vk_format={} desc_tile_mode={} desc_array_mode={} "
                                "desc_bits={} desc_samples={}",
                                count, trace_videoout_stage_index, trace_image_binding_idx,
                                stage.stage, stage.pgm_hash, BindingTypeName(desc.type),
                                desc.info.guest_address, desc.info.guest_size,
                                desc.info.size.width, desc.info.size.height, desc.info.size.depth,
                                desc.info.pitch, vk::to_string(desc.info.pixel_format),
                                static_cast<u32>(desc.info.tile_mode),
                                static_cast<u32>(desc.info.array_mode), desc.info.num_bits,
                                desc.info.num_samples);
                }
            }
            if (instance.IsNullDescriptorSupported()) {
                image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            } else {
                auto& null_image_view = texture_cache.FindTexture(VideoCore::NULL_IMAGE_ID, desc);
                image_infos.emplace_back(VK_NULL_HANDLE, *null_image_view.image_view,
                                         vk::ImageLayout::eGeneral);
            }
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                image_id = texture_cache.FindImage(desc);
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(image_id, desc);
            const auto view_base_level = desc.view_info.range.base.level;
            const auto view_levels = desc.view_info.range.extent.levels;
            const auto view_end_level = view_base_level + view_levels;
            const auto view_base_layer = desc.view_info.range.base.layer;
            const auto view_layers = desc.view_info.range.extent.layers;
            const auto view_end_layer = view_base_layer + view_layers;
            const bool empty_view_range = view_levels == 0 || view_layers == 0;
            const bool oob_view_range = view_end_level > image.info.resources.levels ||
                                        view_end_layer > image.info.resources.layers;

            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            if ((image.binding.force_general || image.binding.is_target) &&
                !image.info.props.is_depth) {
                vk::AccessFlags2 access_mask = vk::AccessFlagBits2::eShaderRead;
                if (image.binding.force_general) {
                    access_mask |= vk::AccessFlagBits2::eShaderWrite;
                }
                if (image.binding.is_target) {
                    access_mask |= vk::AccessFlagBits2::eColorAttachmentWrite |
                                   vk::AccessFlagBits2::eColorAttachmentRead;
                }
                image.Transit(instance.IsAttachmentFeedbackLoopLayoutSupported() &&
                                      image.binding.is_target
                                  ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                  : vk::ImageLayout::eGeneral,
                              access_mask, {});
            } else {
                if (is_storage) {
                    image.Transit(vk::ImageLayout::eGeneral,
                                  vk::AccessFlagBits2::eShaderRead |
                                      vk::AccessFlagBits2::eShaderWrite,
                                  image_view.info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    image.Transit(new_layout, vk::AccessFlagBits2::eShaderRead,
                                  image_view.info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view,
                                     image.backing->state.layout);

            const bool is_videoout_storage = is_storage && IsLikelyVideoOutStorageImage(image);
            const bool trace_invariant =
                is_videoout_storage || trace_stage_has_videoout_storage || empty_view_range ||
                oob_view_range ||
                Common::Trace::EnvEnabled("SHADPS4_TRACE_IMAGE_VIEW_INVARIANTS");
            if (trace_invariant) {
                LOG_INFO(Render_Vulkan,
                         "TRACE_RENDER image_binding_invariant videoout_stage={} binding_index={} "
                         "stage={} pgm={:#x} descriptor={} is_videoout_storage={} image_id={} "
                         "guest_addr={:#x} guest_size={} image_size={}x{}x{} image_levels={} "
                         "image_layers={} image_type={} image_format={} view_type={} "
                         "view_format={} backing_samples={} layout={} desc_addr={:#x} "
                         "desc_size={}x{}x{} desc_pitch={} desc_base_layer={} desc_mips={} "
                         "view_base_level={} view_levels={} view_end_level={} "
                         "view_base_layer={} view_layers={} view_end_layer={} swizzle={}{}{}{} "
                         "empty_range={} oob_range={}",
                         trace_videoout_stage_index, trace_image_binding_idx, stage.stage,
                         stage.pgm_hash, BindingTypeName(desc.type), is_videoout_storage,
                         image_id.index, image.info.guest_address, image.info.guest_size,
                         image.info.size.width, image.info.size.height, image.info.size.depth,
                         image.info.resources.levels, image.info.resources.layers,
                         static_cast<u32>(image.info.type), vk::to_string(image.info.pixel_format),
                         static_cast<u32>(desc.view_info.type), vk::to_string(desc.view_info.format),
                         image.backing ? image.backing->num_samples : 0,
                         image.backing ? vk::to_string(image.backing->state.layout) : "Undefined",
                         desc.info.guest_address, desc.info.size.width, desc.info.size.height,
                         desc.info.size.depth, desc.info.pitch, view_base_layer, view_levels,
                         view_base_level, view_levels, view_end_level,
                         view_base_layer, view_layers, view_end_layer,
                         static_cast<u32>(desc.view_info.mapping.r),
                         static_cast<u32>(desc.view_info.mapping.g),
                         static_cast<u32>(desc.view_info.mapping.b),
                         static_cast<u32>(desc.view_info.mapping.a), empty_view_range,
                         oob_view_range);
            }
            ASSERT_MSG(!IsStrictRenderValidationEnabled() || !empty_view_range,
                       "Strict render validation: image binding has empty view range stage={} "
                       "pgm={:#x} binding={} descriptor={} image_id={} guest_addr={:#x} "
                       "base_level={} levels={} base_layer={} layers={}",
                       stage.stage, stage.pgm_hash, trace_image_binding_idx,
                       BindingTypeName(desc.type), image_id.index, image.info.guest_address,
                       view_base_level, view_levels, view_base_layer, view_layers);
            ASSERT_MSG(!IsStrictRenderValidationEnabled() || !oob_view_range,
                       "Strict render validation: image binding view range exceeds image "
                       "subresources stage={} pgm={:#x} binding={} descriptor={} image_id={} "
                       "guest_addr={:#x} image_levels={} image_layers={} base_level={} levels={} "
                       "end_level={} base_layer={} layers={} end_layer={}",
                       stage.stage, stage.pgm_hash, trace_image_binding_idx,
                       BindingTypeName(desc.type), image_id.index, image.info.guest_address,
                       image.info.resources.levels, image.info.resources.layers, view_base_level,
                       view_levels, view_end_level, view_base_layer, view_layers, view_end_layer);
            if (is_videoout_storage) {
                videoout_storage_probe = VideoOutStorageProbe{
                    .image = image.GetImage(),
                    .guest_address = image.info.guest_address,
                    .guest_size = image.info.guest_size,
                    .image_id = image_id.index,
                    .width = image.info.size.width,
                    .height = image.info.size.height,
                    .format = image.info.pixel_format,
                };
                Common::Trace::RecordVideoOutWrite(
                    "BindStorageVideoOut", image.info.guest_address, image.info.guest_size,
                    stage.pgm_hash,
                    (static_cast<u64>(static_cast<u32>(stage.stage)) << 32) |
                        trace_image_binding_idx);
            }
            if (trace_stage_has_videoout_storage && !is_storage && !image.info.props.is_depth &&
                videoout_source_probes.size() < videoout_source_probes.capacity()) {
                videoout_source_probes.emplace_back(VideoOutSourceProbe{
                    .image = image.GetImage(),
                    .guest_address = image.info.guest_address,
                    .guest_size = image.info.guest_size,
                    .image_id = image_id.index,
                    .binding_index = trace_image_binding_idx,
                    .width = image.info.size.width,
                    .height = image.info.size.height,
                    .format = image.info.pixel_format,
                });
            }

            if (trace_image_bindings && (trace_stage_has_videoout_storage || is_storage)) {
                static std::atomic<u64> image_binding_count{};
                const u64 count =
                    image_binding_count.fetch_add(1, std::memory_order_relaxed) + 1;
                const bool should_log = trace_stage_has_videoout_storage ||
                                        is_videoout_storage || count <= 128 ||
                                        (count % 300) == 0;
                if (should_log) {
                    const u32 backing_samples = image.backing ? image.backing->num_samples : 0;
                    const auto backing_layout =
                        image.backing ? image.backing->state.layout : vk::ImageLayout::eUndefined;
                    const u32 usage_texture = image.usage.texture;
                    const u32 usage_storage = image.usage.storage;
                    const u32 usage_rt = image.usage.render_target;
                    const u32 usage_dt = image.usage.depth_target;
                    const u32 usage_vo = image.usage.vo_surface;
                    LOG_INFO(Render_Vulkan,
                             "TRACE_RENDER image_binding count={} videoout_stage={} "
                             "binding_index={} stage={} pgm={:#x} descriptor={} "
                             "is_videoout_storage={} image_id={} guest_addr={:#x} "
                             "guest_size={} size={}x{}x{} pitch={} vk_format={} "
                             "tile_mode={} array_mode={} bits={} info_samples={} "
                             "backing_samples={} layout={} flags={:#x} usage=t{}s{}rt{}dt{}vo{} "
                             "desc_guest_addr={:#x} desc_size={}x{}x{} desc_pitch={} "
                             "desc_vk_format={} desc_samples={} image_levels={} image_layers={} "
                             "view_type={} range_level={} range_levels={} range_end_level={} "
                             "range_slice={} range_slices={} range_end_slice={} swizzle={}{}{}{}",
                             count, trace_videoout_stage_index, trace_image_binding_idx,
                             stage.stage, stage.pgm_hash, BindingTypeName(desc.type),
                             is_videoout_storage, image_id.index, image.info.guest_address,
                             image.info.guest_size, image.info.size.width, image.info.size.height,
                             image.info.size.depth, image.info.pitch,
                             vk::to_string(image.info.pixel_format),
                             static_cast<u32>(image.info.tile_mode),
                             static_cast<u32>(image.info.array_mode), image.info.num_bits,
                             image.info.num_samples, backing_samples, vk::to_string(backing_layout),
                             static_cast<u32>(image.flags), usage_texture, usage_storage, usage_rt,
                             usage_dt, usage_vo, desc.info.guest_address, desc.info.size.width,
                             desc.info.size.height, desc.info.size.depth, desc.info.pitch,
                             vk::to_string(desc.info.pixel_format), desc.info.num_samples,
                             image.info.resources.levels, image.info.resources.layers,
                             static_cast<u32>(desc.view_info.type), view_base_level, view_levels,
                             view_end_level, view_base_layer, view_layers, view_end_layer,
                             static_cast<u32>(desc.view_info.mapping.r),
                             static_cast<u32>(desc.view_info.mapping.g),
                             static_cast<u32>(desc.view_info.mapping.b),
                             static_cast<u32>(desc.view_info.mapping.a));
                }
            }
        }
        ++trace_image_binding_idx;
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        ASSERT_MSG(!IsStrictRenderValidationEnabled() || image_binding_idx < image_bindings.size(),
                   "Strict render validation: image descriptor binding index overflow "
                   "binding_idx={} bindings={} image_info_idx={} image_infos={} array_size={} "
                   "stage={} pgm={:#x}",
                   image_binding_idx, image_bindings.size(), image_info_idx, image_infos.size(),
                   array_size, stage.stage, stage.pgm_hash);
        ASSERT_MSG(!IsStrictRenderValidationEnabled() ||
                       image_info_idx + array_size <= image_infos.size(),
                   "Strict render validation: image descriptor info range overflow "
                   "binding_idx={} image_info_idx={} array_size={} image_infos={} stage={} "
                   "pgm={:#x}",
                   image_binding_idx, image_info_idx, array_size, image_infos.size(), stage.stage,
                   stage.pgm_hash);
        const auto& [_, desc] = image_bindings[image_binding_idx];
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (sampler.disable_aniso) {
            ASSERT_MSG(!IsStrictRenderValidationEnabled() ||
                           sampler.associated_image < stage.images.size(),
                       "Strict render validation: sampler associated image index out of range "
                       "associated={} images={} stage={} pgm={:#x}",
                       sampler.associated_image, stage.images.size(), stage.stage, stage.pgm_hash);
            if (sampler.associated_image >= stage.images.size()) {
                ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            } else {
                const auto& tsharp = stage.images[sampler.associated_image].GetSharp(stage);
                if (tsharp.base_level == 0 && tsharp.last_level == 0) {
                    ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
                }
            }
        }
        const auto vk_sampler = texture_cache.GetSampler(ssharp, liverpool->regs.ta_bc_base);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

void Rasterizer::ForceVideoOutSourceColorsIfRequested(vk::CommandBuffer cmdbuf,
                                                      const Shader::Info& stage) {
    if (!IsForceVideoOutSourceColorsEnabled() || videoout_source_probes.empty()) {
        return;
    }

    static std::atomic_bool logged{};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        LOG_WARNING(Render_Vulkan,
                    "SHADPS4_FORCE_VIDEOOUT_SOURCE_COLORS=1: clearing VideoOut compute source "
                    "textures before dispatch");
    }

    constexpr vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    for (const auto& probe : videoout_source_probes) {
        const vk::ImageMemoryBarrier read_to_clear{
            .srcAccessMask = vk::AccessFlagBits::eShaderRead,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = probe.image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, read_to_clear);

        const vk::ClearColorValue color =
            probe.binding_index == 0
                ? vk::ClearColorValue{std::array<float, 4>{1.0f, 0.0f, 0.0f, 1.0f}}
                : probe.binding_index == 1
                      ? vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f}}
                      : vk::ClearColorValue{std::array<float, 4>{1.0f, 1.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(probe.image, vk::ImageLayout::eTransferDstOptimal, color, range);

        const vk::ImageMemoryBarrier clear_to_read{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = probe.image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eComputeShader,
                               vk::DependencyFlagBits::eByRegion, {}, {}, clear_to_read);
        LOG_INFO(Render_Vulkan,
                 "TRACE_RENDER videoout_source_force_color pgm={:#x} stage={} binding={} "
                 "image_id={} guest_addr={:#x} guest_size={} size={}x{} format={}",
                 stage.pgm_hash, stage.stage, probe.binding_index, probe.image_id,
                 probe.guest_address, probe.guest_size, probe.width, probe.height,
                 vk::to_string(probe.format));
    }
}

void Rasterizer::ForceVideoOutStorageColorIfRequested(vk::CommandBuffer cmdbuf,
                                                      const Shader::Info& stage, u32 dim_x,
                                                      u32 dim_y, u32 dim_z) {
    if (!IsForceVideoOutStorageColorEnabled() || !videoout_storage_probe.has_value()) {
        return;
    }

    const auto probe = *videoout_storage_probe;
    static std::atomic_bool logged{};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        LOG_WARNING(Render_Vulkan,
                    "SHADPS4_FORCE_VIDEOOUT_STORAGE_COLOR=1: clearing VideoOut storage image "
                    "after compute dispatch to diagnostic green");
    }
    LOG_INFO(Render_Vulkan,
             "TRACE_RENDER videoout_storage_force_color pgm={:#x} stage={} dispatch={}x{}x{} "
             "image_id={} guest_addr={:#x} guest_size={} size={}x{} format={}",
             stage.pgm_hash, stage.stage, dim_x, dim_y, dim_z, probe.image_id,
             probe.guest_address, probe.guest_size, probe.width, probe.height,
             vk::to_string(probe.format));

    constexpr vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    const vk::ImageMemoryBarrier shader_to_clear{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = probe.image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eTransfer,
                           vk::DependencyFlagBits::eByRegion, {}, {}, shader_to_clear);

    const vk::ClearColorValue color{std::array<float, 4>{0.0f, 1.0f, 0.0f, 1.0f}};
    cmdbuf.clearColorImage(probe.image, vk::ImageLayout::eGeneral, color, range);

    const vk::ImageMemoryBarrier clear_to_shader{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = probe.image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                           vk::PipelineStageFlagBits::eAllCommands,
                           vk::DependencyFlagBits::eByRegion, {}, {}, clear_to_shader);
    Common::Trace::RecordVideoOutWrite("ForceVideoOutStorageColor", probe.guest_address,
                                       probe.guest_size, stage.pgm_hash,
                                       (static_cast<u64>(dim_x) << 32) | dim_y);
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    const auto& regs = liverpool->regs;
    const auto& key = pipeline->GetGraphicsKey();
    static std::atomic<u64> render_pass_count{};
    const bool trace_render_pass = IsTraceRenderEnabled();
    const u64 render_pass_index =
        trace_render_pass ? render_pass_count.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    const bool log_render_pass =
        trace_render_pass && (render_pass_index <= 96 || (render_pass_index % 180) == 0);
    RenderState state{};
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        image->SetBackingSamples(key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            image->Transit(instance.IsAttachmentFeedbackLoopLayoutSupported()
                               ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                               : vk::ImageLayout::eGeneral,
                           vk::AccessFlagBits2::eColorAttachmentWrite, {});
            attachment_feedback_loop = true;
        } else {
            image->Transit(vk::ImageLayout::eColorAttachmentOptimal,
                           vk::AccessFlagBits2::eColorAttachmentWrite |
                               vk::AccessFlagBits2::eColorAttachmentRead,
                           image_view.info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        if (log_render_pass) {
            const auto active_cmask_addr = col_buf.info.fast_clear ? col_buf.CmaskAddress() : 0;
            const auto active_fmask_addr = col_buf.info.compression ? col_buf.FmaskAddress() : 0;
            LOG_INFO(Render_Vulkan,
                     "TRACE_RENDER render_target pass={} cb={} mrt_mask={:#x} color_mode={} "
                     "prim={} cb_addr={:#x} cb_samples={} image_id={} guest_addr={:#x} "
                     "guest_size={} size={}x{}x{} pitch={} vk_format={} tile_mode={} "
                     "array_mode={} bits={} info_samples={} backing_samples={} layout={} "
                     "flags={:#x} clear={} clear_value0={:#x} cmask={:#x} fmask={:#x} "
                     "fast_clear={} compression={} cmask_base={:#x} fmask_base={:#x} "
                     "cmask_slice_tile_max={} fmask_slice_tile_max={} fmask_tile_max={}",
                     render_pass_index, cb, key.mrt_mask, static_cast<u32>(regs.color_control.mode),
                     static_cast<u32>(regs.primitive_type), col_buf.Address(), col_buf.NumSamples(),
                     image_id.index, image->info.guest_address, image->info.guest_size,
                     image->info.size.width, image->info.size.height, image->info.size.depth,
                     image->info.pitch, vk::to_string(image->info.pixel_format),
                     static_cast<u32>(image->info.tile_mode),
                     static_cast<u32>(image->info.array_mode), image->info.num_bits,
                     image->info.num_samples, image->backing->num_samples,
                     vk::to_string(image->backing->state.layout), static_cast<u32>(image->flags),
                     is_clear, clear_value.color.uint32[0], active_cmask_addr, active_fmask_addr,
                     col_buf.info.fast_clear, col_buf.info.compression,
                     col_buf.cmask_base_address, col_buf.fmask_base_address,
                     col_buf.cmask_slice.tile_max, col_buf.fmask_slice.tile_max,
                     col_buf.pitch.fmask_tile_max);
        }

        image->usage.render_target = 1u;
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear = regs.depth_render_control.depth_clear_enable ||
                                    texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        const bool depth_write = regs.depth_control.depth_enable &&
                                 regs.depth_control.depth_write_enable &&
                                 !regs.depth_view.z_read_only;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !regs.depth_view.stencil_read_only;
        const auto new_layout =
            depth_write ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                       : vk::ImageLayout::eDepthAttachmentOptimal
            : stencil_write ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
            : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                          : vk::ImageLayout::eDepthReadOnlyOptimal;
        image.Transit(new_layout,
                      vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                          vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                      image_view.info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment = {};
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        if (log_render_pass) {
            LOG_INFO(Render_Vulkan,
                     "TRACE_RENDER depth_target pass={} image_id={} depth_addr={:#x} "
                     "stencil_addr={:#x} htile={:#x} guest_addr={:#x} guest_size={} "
                     "size={}x{}x{} pitch={} vk_format={} tile_mode={} array_mode={} "
                     "bits={} samples={} backing_samples={} layout={} flags={:#x} "
                     "depth_clear={} stencil_clear={} clear_depth={:#x} clear_stencil={:#x}",
                     render_pass_index, image_id.index, regs.depth_buffer.DepthAddress(),
                     regs.depth_buffer.StencilAddress(), htile_address, image.info.guest_address,
                     image.info.guest_size, image.info.size.width, image.info.size.height,
                     image.info.size.depth, image.info.pitch, vk::to_string(image.info.pixel_format),
                     static_cast<u32>(image.info.tile_mode),
                     static_cast<u32>(image.info.array_mode), image.info.num_bits,
                     image.info.num_samples, image.backing->num_samples,
                     vk::to_string(image.backing->state.layout), static_cast<u32>(image.flags),
                     is_depth_clear, is_stencil_clear, attachment.clear_value[0],
                     attachment.clear_value[1]);
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->last_cb_extent[0];
    const auto& mrt1_hint = liverpool->last_cb_extent[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->regs.color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->regs.color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 liverpool->regs.color_buffers[0].Address(),
                                 liverpool->regs.color_buffers[1].Address()));
    mrt1_image.Resolve(mrt0_image, mrt0_desc.view_info.range, mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->regs;

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->regs.depth_view.slice_start;
    sub_range.extent.layers = liverpool->regs.depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    read_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                       sub_range);
    write_image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                        sub_range);

    auto aspect_mask = vk::ImageAspectFlags(0);
    if (is_depth) {
        aspect_mask |= vk::ImageAspectFlagBits::eDepth;
    }
    if (is_stencil) {
        aspect_mask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageCopy region = {
        .srcSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .srcOffset = {0, 0, 0},
        .dstSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .dstOffset = {0, 0, 0},
        .extent = {write_image.info.size.width, write_image.info.size.height, 1},
    };
    scheduler.CommandBuffer().copyImage(read_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                        write_image.GetImage(),
                                        vk::ImageLayout::eTransferDstOptimal, region);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    buffer_cache.FillBuffer(address, num_bytes, value, is_gds);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    buffer_cache.CopyBuffer(dst, src, num_bytes, dst_gds, src_gds);
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size);
    return true;
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    page_manager.OnGpuUnmap(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = liverpool->regs;

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
    }

    if (viewports.empty()) {
        static std::atomic_uint dummy_viewport_warn_count{0};
        const u32 warn_count = dummy_viewport_warn_count.fetch_add(1, std::memory_order_relaxed);
        if (warn_count < 16 || IsStrictRenderValidationEnabled()) {
            LOG_WARNING(Render_Vulkan,
                        "No valid guest viewport; using dummy 1x1 viewport. Draw output will "
                        "likely be invisible. count={}",
                        warn_count + 1);
        }
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        dynamic_state.SetStencilReferences(front.stencil_test_val, back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency ||
               topology == vk::PrimitiveTopology::ePatchList;
    };

    auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type));
    if (is_indexed && prim_restart && regs.primitive_restart_index != 0xFFFF &&
        regs.primitive_restart_index != 0xFFFFFFFF) {
        LOG_WARNING(Render_Vulkan,
                    "Disabling unsupported primitive restart index {:#x} for topology {}",
                    regs.primitive_restart_index, static_cast<u32>(regs.primitive_type));
        prim_restart = false;
    }

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

} // namespace Vulkan
