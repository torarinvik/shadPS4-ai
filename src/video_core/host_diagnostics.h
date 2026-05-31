// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Early-detection diagnostics for GPU command recording.
//
// The goal of this header is to catch GPU resource / bounds / state problems on the CPU, at the
// exact site where the offending command is *recorded*, rather than letting them surface much
// later as an opaque, hard-to-attribute asynchronous device loss (MoltenVK
// kIOGPUCommandBufferCallbackErrorInvalidResource / Timeout). Every check here is PURE: it only
// logs/asserts, it never alters the command or any resource, so it can never change rendering
// behavior or introduce a regression.
//
// Severity model (controlled uniformly by SHADPS4_GPU_VALIDATION=off|warn|assert; default warn):
//   * warn  - a warning is logged (rate-limited to one line per unique finding) and the run
//             continues. Visible in normal runs so issues can be grepped.
//   * assert- additionally aborts (for CI / strict debugging).
//   * off   - no logging or asserting.
// SHADPS4_STRICT_RENDER_VALIDATION=1 is honored as an alias for assert mode (back-compat).
//
// Every check is PURE (log/assert only; never alters a command or resource) so it cannot regress
// rendering. Findings are de-duplicated by a key so a repeating problem (the menu churn produced
// 1653 / 643 identical warnings) logs a single line.
//
// Usage: include this header and call the Check* helpers next to copy/dispatch/bind recording.

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <fmt/format.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/trace_control.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore::Diag {

[[nodiscard]] inline bool StrictValidation() {
    static const bool enabled = Common::Trace::EnvEnabled("SHADPS4_STRICT_RENDER_VALIDATION");
    return enabled;
}

enum class Mode { Off, Warn, Assert };

// Uniform severity control for all diagnostics (#97). SHADPS4_GPU_VALIDATION=off|warn|assert.
[[nodiscard]] inline Mode ValidationMode() {
    static const Mode mode = [] {
        if (const char* v = std::getenv("SHADPS4_GPU_VALIDATION")) {
            if (std::strcmp(v, "off") == 0 || std::strcmp(v, "0") == 0) {
                return Mode::Off;
            }
            if (std::strcmp(v, "assert") == 0) {
                return Mode::Assert;
            }
            return Mode::Warn;
        }
        return StrictValidation() ? Mode::Assert : Mode::Warn;
    }();
    return mode;
}

// Rate limiter (#96): returns true the first time `key` is seen, false afterwards. Thread-safe.
// Inline-function statics have one shared instance across translation units.
[[nodiscard]] inline bool FirstReport(std::string_view key) {
    static std::mutex mutex;
    static std::unordered_set<std::string> seen;
    std::scoped_lock lk(mutex);
    return seen.emplace(key).second;
}

// Central reporting for a diagnostic finding: honors the validation mode and rate-limits each
// unique `key` to one log line. `msg` is the human-readable detail.
inline void ReportOnce(std::string_view key, const std::string& msg) {
    const Mode mode = ValidationMode();
    if (mode == Mode::Off) {
        return;
    }
    if (FirstReport(key)) {
        LOG_WARNING(Render_Vulkan, "{}", msg);
    }
    if (mode == Mode::Assert) {
        ASSERT_MSG(false, "GPU validation: {}", msg);
    }
}

// When SHADPS4_TRACE_FREES=1, GPU-resource destructions are logged at the moment they actually run
// (the deferred-destroy callback firing -> vmaDestroy). Used to correlate a freed resource with a
// subsequent kIOGPU "Invalid Resource" device loss: the last resource freed before the loss, on the
// same Metal queue, is the prime suspect for a use-after-free the static gates didn't cover.
[[nodiscard]] inline bool TraceFrees() {
    static const bool enabled = Common::Trace::EnvEnabled("SHADPS4_TRACE_FREES");
    return enabled;
}

// 4x4 for block-compressed (BCn) formats, 1x1 otherwise. The copy's row length / extent are in
// texels but a block-compressed buffer is addressed in blocks.
[[nodiscard]] inline u32 BlockDim(vk::Format format) {
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
        return 4u;
    default:
        return 1u;
    }
}

// Bytes a single texel/block contributes to a copy for the given aspect under `format`. Depth and
// stencil planes are addressed separately: a depth-aspect copy moves only the depth plane (D16=2,
// D24/D32=4 bytes), a stencil-aspect copy only the 1-byte stencil plane.
[[nodiscard]] inline u64 AspectTexelBytes(vk::Format format, vk::ImageAspectFlags aspect) {
    const bool depth = bool(aspect & vk::ImageAspectFlagBits::eDepth);
    const bool stencil = bool(aspect & vk::ImageAspectFlagBits::eStencil);
    if (stencil && !depth) {
        return 1;
    }
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eD16UnormS8Uint:
        return depth ? 2 : 2;
    case vk::Format::eD32Sfloat:
    case vk::Format::eD32SfloatS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eX8D24UnormPack32:
        return depth ? 4 : 4;
    case vk::Format::eR8Unorm:
    case vk::Format::eR8Snorm:
    case vk::Format::eR8Uint:
    case vk::Format::eR8Sint:
    case vk::Format::eS8Uint:
        return 1;
    case vk::Format::eR8G8Unorm:
    case vk::Format::eR8G8Uint:
    case vk::Format::eR16Unorm:
    case vk::Format::eR16Uint:
    case vk::Format::eR16Sint:
    case vk::Format::eR16Sfloat:
    case vk::Format::eR5G6B5UnormPack16:
    case vk::Format::eA1R5G5B5UnormPack16:
    case vk::Format::eR4G4B4A4UnormPack16:
        return 2;
    case vk::Format::eBc1RgbUnormBlock:
    case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc1RgbaUnormBlock:
    case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc4UnormBlock:
    case vk::Format::eBc4SnormBlock:
        return 8; // bytes per 4x4 block
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
        return 16; // bytes per 4x4 block
    case vk::Format::eR16G16B16A16Unorm:
    case vk::Format::eR16G16B16A16Uint:
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eR32G32Uint:
    case vk::Format::eR32G32Sfloat:
        return 8;
    case vk::Format::eR32G32B32A32Uint:
    case vk::Format::eR32G32B32A32Sfloat:
        return 16;
    default:
        return 4; // common 32-bit color
    }
}

// Verify a set of buffer<->image copy regions stays within `buffer_size` bytes. `host_format` is
// the format of the HOST image (after any MoltenVK substitution), since Metal computes the copy's
// buffer-side size from the destination/source texture's format. Returns true if any region is OOB.
inline bool CheckBufferImageCopies(const char* site, u64 guest_addr, vk::Format host_format,
                                   std::span<const vk::BufferImageCopy> copies, u64 buffer_size) {
    const u32 block = BlockDim(host_format);
    bool bad = false;
    for (const auto& copy : copies) {
        const u64 texel_bytes = AspectTexelBytes(host_format, copy.imageSubresource.aspectMask);
        const u64 row_texels = copy.bufferRowLength ? copy.bufferRowLength : copy.imageExtent.width;
        const u64 rows = copy.bufferImageHeight ? copy.bufferImageHeight : copy.imageExtent.height;
        const u64 slices = std::max<u32>(copy.imageExtent.depth, 1u) *
                           std::max<u32>(copy.imageSubresource.layerCount, 1u);
        const u64 required = static_cast<u64>(copy.bufferOffset) +
                             texel_bytes * ((row_texels + block - 1) / block) *
                                 ((rows + block - 1) / block) * slices;
        if (required > buffer_size) {
            bad = true;
            ReportOnce(
                fmt::format("bufimg:{}:{}", site, vk::to_string(host_format)),
                fmt::format("[{}] buffer<->image copy out of bounds: addr={:#x} host_format={} "
                            "required={} buffer_size={} bufferOffset={} texel_bytes={}",
                            site, guest_addr, vk::to_string(host_format), required, buffer_size,
                            static_cast<u64>(copy.bufferOffset), texel_bytes));
        }
    }
    return bad;
}

// Verify image copy subresources lie within the image's mip/layer range.
inline bool CheckImageSubresources(const char* site, u64 guest_addr, u32 image_levels,
                                   u32 image_layers,
                                   std::span<const vk::BufferImageCopy> copies) {
    bool bad = false;
    for (const auto& copy : copies) {
        const auto& sub = copy.imageSubresource;
        if (sub.mipLevel >= image_levels || sub.baseArrayLayer + sub.layerCount > image_layers ||
            sub.layerCount == 0) {
            bad = true;
            ReportOnce(
                fmt::format("subres:{}", site),
                fmt::format("[{}] image copy subresource out of bounds: addr={:#x} mip={} levels={} "
                            "base_layer={} layer_count={} layers={}",
                            site, guest_addr, sub.mipLevel, image_levels, sub.baseArrayLayer,
                            sub.layerCount, image_layers));
        }
    }
    return bad;
}

// Verify a buffer->buffer copy stays within both buffers.
inline bool CheckBufferCopy(const char* site, u64 src_size, u64 dst_size, u64 src_offset,
                            u64 dst_offset, u64 size) {
    bool bad = false;
    if (src_offset + size > src_size || dst_offset + size > dst_size || size == 0) {
        bad = true;
        ReportOnce(
            fmt::format("bufcopy:{}", site),
            fmt::format("[{}] buffer copy out of bounds: src_off={} dst_off={} size={} src_size={} "
                        "dst_size={}",
                        site, src_offset, dst_offset, size, src_size, dst_size));
    }
    return bad;
}

// Verify a set of buffer->buffer copy regions stay within both buffers.
inline bool CheckBufferCopyRegions(const char* site, u64 src_size, u64 dst_size,
                                   std::span<const vk::BufferCopy> regions) {
    bool bad = false;
    for (const auto& r : regions) {
        if (static_cast<u64>(r.srcOffset) + r.size > src_size ||
            static_cast<u64>(r.dstOffset) + r.size > dst_size || r.size == 0) {
            bad = true;
            ReportOnce(fmt::format("bufcopyregions:{}", site),
                       fmt::format("[{}] buffer copy region out of bounds: src_off={} dst_off={} "
                                   "size={} src_size={} dst_size={}",
                                   site, static_cast<u64>(r.srcOffset), static_cast<u64>(r.dstOffset),
                                   static_cast<u64>(r.size), src_size, dst_size));
        }
    }
    return bad;
}

// Verify a buffer descriptor range stays within the buffer. Catches a shader being handed a UBO/SSBO
// range that reads past its backing buffer.
inline bool CheckBufferRange(const char* site, u64 buffer_size, u64 offset, u64 range) {
    bool bad = false;
    if (range != VK_WHOLE_SIZE && offset + range > buffer_size) {
        bad = true;
        ReportOnce(
            fmt::format("bufrange:{}", site),
            fmt::format("[{}] buffer descriptor range out of bounds: offset={} range={} "
                        "buffer_size={}",
                        site, offset, range, buffer_size));
    }
    return bad;
}

// Sanity-check a compute dispatch. A zero dimension wastes a submit; an absurdly large one can hang
// the GPU. NOTE: the default ceiling is intentionally far above Vulkan's guaranteed-minimum 65535:
// MoltenVK on Apple GPUs reports maxComputeWorkGroupCount ~1.07e9 per dim, so large detile
// dispatches (tens of thousands of groups) are valid and must NOT be flagged. This only catches
// truly runaway counts.
inline bool CheckDispatch(const char* site, u32 x, u32 y, u32 z, u32 max_groups = (1u << 28)) {
    bool bad = false;
    if (x == 0 || y == 0 || z == 0) {
        bad = true;
        ReportOnce(fmt::format("dispatch0:{}", site),
                   fmt::format("[{}] compute dispatch has a zero dimension: {}x{}x{}", site, x, y, z));
    }
    if (x > max_groups || y > max_groups || z > max_groups) {
        bad = true;
        ReportOnce(fmt::format("dispatchmax:{}", site),
                   fmt::format("[{}] compute dispatch exceeds {} groups per dim: {}x{}x{} (may "
                               "GPU-timeout)",
                               site, max_groups, x, y, z));
    }
    return bad;
}

} // namespace VideoCore::Diag
