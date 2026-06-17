// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include "common/assert.h"
#include "common/trace_control.h"
#include "video_core/renderer_vulkan/vk_gpu_command_diagnostics.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/sampler.h"

namespace VideoCore {

static bool IsStrictRenderValidationEnabled() {
    static const bool enabled = Common::Trace::EnvEnabled("SHADPS4_STRICT_RENDER_VALIDATION");
    return enabled;
}

Sampler::Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                 const AmdGpu::BorderColorBuffer border_color_base) {
    using namespace Vulkan;
    const bool anisotropy_enable = instance.IsAnisotropicFilteringSupported() &&
                                   (AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
                                    AmdGpu::IsAnisoFilter(sampler.xy_min_filter));
    const float max_anisotropy =
        anisotropy_enable ? std::clamp(sampler.MaxAniso(), 1.0f, instance.MaxSamplerAnisotropy())
                          : 1.0f;
    const float max_lod_bias = instance.MaxSamplerLodBias();
    const float min_lod = sampler.MinLod();
    const float max_lod = std::max(min_lod, sampler.MaxLod());
    auto border_color = LiverpoolToVK::BorderColor(sampler.border_color_type);
    if (border_color == vk::BorderColor::eFloatCustomEXT &&
        !instance.IsCustomBorderColorSupported()) {
        LOG_WARNING(Render_Vulkan, "Custom border color is not supported, falling back to black");
        ASSERT_MSG(!IsStrictRenderValidationEnabled(),
                   "Strict render validation: custom sampler border color unsupported; would "
                   "fallback to opaque black border_color_type={} border_ptr={:#x}",
                   static_cast<u32>(static_cast<AmdGpu::BorderColor>(sampler.border_color_type)),
                   sampler.border_color_ptr.Value());
        border_color = vk::BorderColor::eFloatOpaqueBlack;
    }

    const auto custom_color = [&]() -> std::optional<vk::SamplerCustomBorderColorCreateInfoEXT> {
        if (border_color == vk::BorderColor::eFloatCustomEXT) {
            const auto border_color_index = sampler.border_color_ptr.Value();
            const auto border_color_buffer = border_color_base.Address<std::array<float, 4>*>();
            const auto custom_border_color_array = border_color_buffer[border_color_index];

            const vk::SamplerCustomBorderColorCreateInfoEXT ret{
                .customBorderColor =
                    vk::ClearColorValue{
                        .float32 = custom_border_color_array,
                    },
                .format = vk::Format::eR32G32B32A32Sfloat,
            };
            return ret;
        } else {
            return std::nullopt;
        }
    }();

    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom_color ? &*custom_color : nullptr,
        .magFilter = LiverpoolToVK::Filter(sampler.xy_mag_filter),
        .minFilter = LiverpoolToVK::Filter(sampler.xy_min_filter),
        .mipmapMode = LiverpoolToVK::MipFilter(sampler.mip_filter),
        .addressModeU = LiverpoolToVK::ClampMode(sampler.clamp_x),
        .addressModeV = LiverpoolToVK::ClampMode(sampler.clamp_y),
        .addressModeW = LiverpoolToVK::ClampMode(sampler.clamp_z),
        .mipLodBias = std::clamp(sampler.LodBias(), -max_lod_bias, max_lod_bias),
        .anisotropyEnable = anisotropy_enable,
        .maxAnisotropy = max_anisotropy,
        .compareEnable = sampler.depth_compare_func != AmdGpu::DepthCompare::Never,
        .compareOp = LiverpoolToVK::DepthCompare(sampler.depth_compare_func),
        .minLod = min_lod,
        .maxLod = max_lod,
        .borderColor = border_color,
        .unnormalizedCoordinates = false, // Handled in shader due to Vulkan limitations.
    };
    auto [sampler_result, smplr] = instance.GetDevice().createSamplerUnique(sampler_ci);
    if (sampler_result != vk::Result::eSuccess) {
        Vulkan::RecordGpuCommandDiagnostic(
            "sampler_create_failed result=%s mag=%u min=%u mip=%u clamp=%u/%u/%u compare=%u "
            "border=%u aniso=%u lod=%f..%f bias=%f",
            vk::to_string(sampler_result).c_str(),
            static_cast<u32>(static_cast<AmdGpu::Filter>(sampler.xy_mag_filter)),
            static_cast<u32>(static_cast<AmdGpu::Filter>(sampler.xy_min_filter)),
            static_cast<u32>(static_cast<AmdGpu::MipFilter>(sampler.mip_filter)),
            static_cast<u32>(static_cast<AmdGpu::ClampMode>(sampler.clamp_x)),
            static_cast<u32>(static_cast<AmdGpu::ClampMode>(sampler.clamp_y)),
            static_cast<u32>(static_cast<AmdGpu::ClampMode>(sampler.clamp_z)),
            static_cast<u32>(static_cast<AmdGpu::DepthCompare>(sampler.depth_compare_func)),
            static_cast<u32>(static_cast<AmdGpu::BorderColor>(sampler.border_color_type)),
            anisotropy_enable ? 1 : 0, min_lod, max_lod, sampler_ci.mipLodBias);
        Vulkan::DumpGpuCommandDiagnostics("sampler_create_failed");
    }
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    handle = std::move(smplr);
}

Sampler::~Sampler() = default;

} // namespace VideoCore
