// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/assert.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/trace_control.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "core/libraries/system/systemservice.h"
#include "core/memory.h"
#include "imgui/notifications_layer.h"
#include "imgui/renderer/imgui_core.h"
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <imgui.h>
#include <png.h>
#include <vk_mem_alloc.h>

namespace Vulkan {

static bool IsStrictRenderValidationEnabled() {
    static const bool enabled = Common::Trace::EnvEnabled("SHADPS4_STRICT_RENDER_VALIDATION");
    return enabled;
}

static bool IsStrictBlackScreenWatchdogEnabled() {
    static const bool enabled =
        Common::Trace::EnvEnabled("SHADPS4_STRICT_BLACK_SCREEN_WATCHDOG");
    return enabled;
}

static double GetEnvDouble(const char* name, const double fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char* end{};
    const double parsed = std::strtod(value, &end);
    return end != value ? parsed : fallback;
}

static u32 GetEnvU32(const char* name, const u32 fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char* end{};
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || parsed > std::numeric_limits<u32>::max()) {
        return fallback;
    }
    return static_cast<u32>(parsed);
}

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

enum class ScreenshotKind : u8 {
    GameOnly,
    FrameImage,
    WithOverlays,
};

static const char* ScreenshotKindName(const ScreenshotKind kind) {
    switch (kind) {
    case ScreenshotKind::GameOnly:
        return "GameOnly";
    case ScreenshotKind::FrameImage:
        return "FrameImage";
    case ScreenshotKind::WithOverlays:
        return "WithOverlays";
    default:
        return "Unknown";
    }
}

struct LumaStats {
    double avg_luma{};
    double variance{};
    u8 max_luma{};
    double near_black_pct{};
    u64 near_black_pixels{};
    u64 nonblack_pixels{};
    u64 pixel_count{};
};

struct WatchdogReadbackContext {
    u64 frame_index{};
    VAddr videoout_addr{};
    u32 image_id{};
    u64 guest_size{};
    u32 flags{};
    u32 usage_texture{};
    u32 usage_storage{};
    u32 usage_render_target{};
    u32 usage_depth_target{};
    u32 image_samples{};
    u32 backing_samples{};
    VAddr cmask_addr{};
    VAddr fmask_addr{};
    VAddr htile_addr{};
    vk::ImageLayout layout{};
    LumaStats guest_stats{};
    const char* last_write_op{};
    u64 last_write_address{};
    u64 last_write_size{};
    u64 last_write_detail0{};
    u64 last_write_detail1{};
    u64 last_write_sequence{};
    uintptr_t frame_image{};
    uintptr_t frame_view{};
    uintptr_t frame_texture{};
};

struct BlackFrameWatchdogConfig {
    double near_black_pct{99.5};
    u8 max_luma{8};
    double avg_luma{2.0};
    u32 consecutive_frames{3};
};

struct BlackFrameWatchdog {
    explicit BlackFrameWatchdog()
        : config{
              .near_black_pct =
                  GetEnvDouble("SHADPS4_BLACK_WATCHDOG_NEAR_BLACK_PCT", 99.5),
              .max_luma = static_cast<u8>(
                  std::clamp(GetEnvU32("SHADPS4_BLACK_WATCHDOG_MAX_LUMA", 8), 0u, 255u)),
              .avg_luma = GetEnvDouble("SHADPS4_BLACK_WATCHDOG_AVG_LUMA", 2.0),
              .consecutive_frames =
                  std::max<u32>(GetEnvU32("SHADPS4_BLACK_WATCHDOG_CONSECUTIVE_FRAMES", 3), 1),
          } {}

    bool IsNearBlack(const LumaStats& stats) const {
        return stats.pixel_count > 0 && stats.near_black_pct >= config.near_black_pct &&
               stats.max_luma <= config.max_luma && stats.avg_luma <= config.avg_luma;
    }

    BlackFrameWatchdogConfig config{};
    std::mutex mutex;
    bool saw_nonblack_game_frame{};
    bool last_armed{};
    std::array<u32, 3> consecutive_black{};
    std::array<LumaStats, 3> last_stats{};
};

static bool IsTraceRenderEnabled() {
    return Common::Trace::IsAggressiveLoggingEnabled();
}

static u64 GetTraceVideoOutInterval() {
    static const u64 interval = [] {
        const char* value = std::getenv("SHADPS4_TRACE_VIDEO_OUT_EVERY");
        if (value == nullptr || value[0] == '\0') {
            return 30ULL;
        }

        char* end{};
        const auto parsed = std::strtoull(value, &end, 10);
        return end != value && parsed > 0 ? parsed : 30ULL;
    }();
    return interval;
}

static bool IsTracePresentLumaEnabled() {
    static const bool enabled = Common::Trace::EnvEnabled("SHADPS4_TRACE_PRESENT_LUMA");
    return enabled;
}

static u64 GetTracePresentLumaInterval() {
    static const u64 interval = [] {
        const char* value = std::getenv("SHADPS4_TRACE_PRESENT_LUMA_EVERY");
        if (value == nullptr || value[0] == '\0') {
            return 1ULL;
        }

        char* end{};
        const auto parsed = std::strtoull(value, &end, 10);
        return end != value && parsed > 0 ? parsed : 1ULL;
    }();
    return interval;
}

struct ScreenshotReadback {
    ScreenshotKind kind{};
    std::vector<std::filesystem::path> paths{};
    WatchdogReadbackContext watchdog_context{};
    bool watchdog{};
    VideoCore::Buffer buffer;
    u32 width{};
    u32 height{};
    vk::Format format{};
    bool hdr_encoded{};

    ScreenshotReadback(const Instance& instance, Scheduler& scheduler, ScreenshotKind kind_,
                       std::vector<std::filesystem::path> paths_, const u32 width_,
                       const u32 height_, const vk::Format format_, const bool hdr_encoded_,
                       WatchdogReadbackContext watchdog_context_ = {},
                       const bool watchdog_ = false)
        : kind{kind_}, paths{std::move(paths_)},
          watchdog_context{watchdog_context_}, watchdog{watchdog_},
          buffer{instance,
                 scheduler,
                 VideoCore::MemoryUsage::Download,
                 0,
                 vk::BufferUsageFlagBits::eTransferDst,
                 static_cast<u64>(width_) * static_cast<u64>(height_) * 4},
          width{width_}, height{height_}, format{format_}, hdr_encoded{hdr_encoded_} {}
};

static std::string SanitizeFilenameComponent(std::string value) {
    for (char& c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
            c = '_';
        }
    }
    if (value.empty()) {
        return "UNKNOWN";
    }
    return value;
}

static std::vector<std::filesystem::path> BuildScreenshotPaths(const ScreenshotKind kind,
                                                               const u32 count) {
    static std::atomic<u64> screenshot_sequence{0};
    std::vector<std::filesystem::path> paths{};
    if (count == 0) {
        return paths;
    }

    const char* trace_screenshot_dir = std::getenv("SHADPS4_TRACE_SCREENSHOT_DIR");
    const auto screenshots_dir =
        trace_screenshot_dir != nullptr && trace_screenshot_dir[0] != '\0'
            ? std::filesystem::path{trace_screenshot_dir}
            : Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir);
    std::filesystem::create_directories(screenshots_dir);

    const auto game_id =
        SanitizeFilenameComponent(std::string(Common::ElfInfo::Instance().GameSerial()));
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;

    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif

    std::ostringstream stamp;
    stamp << std::put_time(&local_tm, "%Y%m%d_%H%M%S") << '_' << std::setw(3) << std::setfill('0')
          << ms;

    const char* suffix = kind == ScreenshotKind::GameOnly     ? "game"
                         : kind == ScreenshotKind::FrameImage ? "frame"
                                                              : "hud";
    const auto first_sequence = screenshot_sequence.fetch_add(count, std::memory_order_relaxed);

    paths.reserve(count);
    const auto stamp_str = stamp.str();
    for (u32 i = 0; i < count; ++i) {
        paths.emplace_back(screenshots_dir / fmt::format("{}_{}_{}_{:06}.png", game_id, stamp_str,
                                                         suffix, first_sequence + i));
    }

    return paths;
}

static bool IsTraceScreenshotStatsOnly() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_TRACE_SCREENSHOT_STATS_ONLY");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static float PqToNits(const float encoded) {
    // ST.2084 inverse EOTF
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;

    const float v = std::clamp(encoded, 0.0f, 1.0f);
    const float vp = std::pow(v, 1.0f / m2);
    const float num = std::max(vp - c1, 0.0f);
    const float den = std::max(c2 - c3 * vp, 1e-6f);
    return 10000.0f * std::pow(num / den, 1.0f / m1);
}

static float ToneMapToSdrLinear(const float nits) {
    // Map absolute HDR luminance into SDR [0,1], preserving 100-nit white.
    constexpr float sdr_white_nits = 100.0f;
    const float x = std::max(nits, 0.0f) / sdr_white_nits;
    const float mapped = (2.0f * x) / (1.0f + x);
    return std::clamp(mapped, 0.0f, 1.0f);
}

static float LinearToSrgb(const float linear) {
    const float x = std::clamp(linear, 0.0f, 1.0f);
    if (x <= 0.0031308f) {
        return 12.92f * x;
    }
    return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

static const std::array<float, 1024>& GetPqDecodeNitsLut() {
    static const std::array<float, 1024> lut = [] {
        std::array<float, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = PqToNits(static_cast<float>(i) / 1023.0f);
        }
        return values;
    }();
    return lut;
}

static const std::array<u8, 1024>& GetUnorm10ToU8Lut() {
    static const std::array<u8, 1024> lut = [] {
        std::array<u8, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<u8>((i * 255u + 511u) / 1023u);
        }
        return values;
    }();
    return lut;
}

static void CopyImageToReadback(const vk::CommandBuffer& cmdbuf, const vk::Image image,
                                const vk::ImageLayout layout, ScreenshotReadback& readback) {
    const vk::BufferImageCopy copy_region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {readback.width, readback.height, 1},
    };
    cmdbuf.copyImageToBuffer(image, layout, readback.buffer.Handle(), copy_region);
}

static bool ConvertReadbackToRgba8(const ScreenshotReadback& readback, std::vector<u8>& out_rgba) {
    const u64 pixel_count = static_cast<u64>(readback.width) * static_cast<u64>(readback.height);
    const u64 byte_size = pixel_count * 4;
    if (readback.buffer.mapped_data.size() < byte_size) {
        LOG_ERROR(Render_Vulkan, "Screenshot readback buffer size mismatch (have {}, need {})",
                  readback.buffer.mapped_data.size(), byte_size);
        return false;
    }

    readback.buffer.Invalidate(0, byte_size);
    const auto src =
        std::span<const u8>{readback.buffer.mapped_data.data(), static_cast<size_t>(byte_size)};
    out_rgba.resize(static_cast<size_t>(byte_size));

    switch (readback.format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        std::memcpy(out_rgba.data(), src.data(), out_rgba.size());
        for (u64 i = 0; i < pixel_count; ++i) {
            out_rgba[static_cast<size_t>(i) * 4 + 3] = 255;
        }
        return true;
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            out_rgba[o + 0] = src[o + 2];
            out_rgba[o + 1] = src[o + 1];
            out_rgba[o + 2] = src[o + 0];
            out_rgba[o + 3] = 255;
        }
        return true;
    case vk::Format::eA2R10G10B10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 b = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 r = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    case vk::Format::eA2B10G10R10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 r = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 b = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // HDR swapchain path is Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    default:
        LOG_WARNING(Render_Vulkan, "Unsupported screenshot format: {}",
                    vk::to_string(readback.format));
        return false;
    }
}

static LumaStats ComputeLumaStats(const std::span<const u8> rgba, const u32 width,
                                  const u32 height) {
    LumaStats stats{};
    if (rgba.empty()) {
        return stats;
    }

    u64 luma_sum = 0;
    u64 luma_sum_sq = 0;
    stats.pixel_count = static_cast<u64>(width) * static_cast<u64>(height);
    for (u64 i = 0; i < stats.pixel_count; ++i) {
        const size_t offset = static_cast<size_t>(i) * 4;
        const u8 r = rgba[offset + 0];
        const u8 g = rgba[offset + 1];
        const u8 b = rgba[offset + 2];
        const u8 luma = static_cast<u8>((static_cast<u32>(r) * 54 + static_cast<u32>(g) * 183 +
                                         static_cast<u32>(b) * 19) >>
                                        8);
        luma_sum += luma;
        luma_sum_sq += static_cast<u64>(luma) * luma;
        stats.max_luma = std::max(stats.max_luma, luma);
        if (luma <= 4) {
            stats.near_black_pixels++;
        }
    }

    stats.nonblack_pixels = stats.pixel_count - stats.near_black_pixels;
    stats.avg_luma =
        stats.pixel_count == 0 ? 0.0 : static_cast<double>(luma_sum) / stats.pixel_count;
    if (stats.pixel_count != 0) {
        const double mean_sq = static_cast<double>(luma_sum_sq) / stats.pixel_count;
        stats.variance = std::max(0.0, mean_sq - stats.avg_luma * stats.avg_luma);
    }
    stats.near_black_pct = stats.pixel_count == 0
                               ? 0.0
                               : (100.0 * static_cast<double>(stats.near_black_pixels)) /
                                     stats.pixel_count;
    return stats;
}

static LumaStats ComputeRawGuestLumaStats(const VAddr address, const u32 width, const u32 height,
                                          const u32 pitch, const u32 bits_per_pixel,
                                          const u64 guest_size) {
    const u32 bytes_per_pixel = std::max<u32>(bits_per_pixel / 8, 1);
    const u64 row_bytes = static_cast<u64>(std::max(width, pitch)) * bytes_per_pixel;
    const u64 read_size = std::min<u64>(guest_size, row_bytes * height);
    LumaStats stats{};
    if (address == 0 || width == 0 || height == 0 || read_size == 0) {
        return stats;
    }

    std::vector<u8> data(read_size);
    Core::Memory::Instance()->CopySparseMemory(address, data.data(), read_size);
    stats.pixel_count = static_cast<u64>(width) * height;

    u64 luma_sum = 0;
    u64 luma_sum_sq = 0;
    for (u32 y = 0; y < height; ++y) {
        const u64 row_offset = static_cast<u64>(y) * row_bytes;
        if (row_offset >= data.size()) {
            break;
        }
        for (u32 x = 0; x < width; ++x) {
            const u64 pixel_offset = row_offset + static_cast<u64>(x) * bytes_per_pixel;
            if (pixel_offset >= data.size()) {
                break;
            }

            const u8 r = data[static_cast<size_t>(pixel_offset + std::min<u32>(2, bytes_per_pixel - 1))];
            const u8 g = data[static_cast<size_t>(pixel_offset + std::min<u32>(1, bytes_per_pixel - 1))];
            const u8 b = data[static_cast<size_t>(pixel_offset)];
            const u8 luma = static_cast<u8>((static_cast<u32>(r) * 54 + static_cast<u32>(g) * 183 +
                                             static_cast<u32>(b) * 19) >>
                                            8);
            luma_sum += luma;
            luma_sum_sq += static_cast<u64>(luma) * luma;
            stats.max_luma = std::max(stats.max_luma, luma);
            if (luma <= 4) {
                stats.near_black_pixels++;
            }
        }
    }

    stats.nonblack_pixels = stats.pixel_count - stats.near_black_pixels;
    stats.avg_luma =
        stats.pixel_count == 0 ? 0.0 : static_cast<double>(luma_sum) / stats.pixel_count;
    if (stats.pixel_count != 0) {
        const double mean_sq = static_cast<double>(luma_sum_sq) / stats.pixel_count;
        stats.variance = std::max(0.0, mean_sq - stats.avg_luma * stats.avg_luma);
    }
    stats.near_black_pct = stats.pixel_count == 0
                               ? 0.0
                               : (100.0 * static_cast<double>(stats.near_black_pixels)) /
                                     stats.pixel_count;
    return stats;
}

static u64 ComputeRawGuestFrameHash(const VAddr address, const u32 width, const u32 height,
                                    const u32 pitch, const u32 bits_per_pixel,
                                    const u64 guest_size) {
    const u32 bytes_per_pixel = std::max<u32>(bits_per_pixel / 8, 1);
    const u64 row_bytes = static_cast<u64>(std::max(width, pitch)) * bytes_per_pixel;
    const u64 read_size = std::min<u64>(guest_size, row_bytes * height);
    if (address == 0 || width == 0 || height == 0 || read_size == 0) {
        return 0;
    }

    std::vector<u8> data(read_size);
    Core::Memory::Instance()->CopySparseMemory(address, data.data(), read_size);

    u64 hash = 1469598103934665603ull;
    for (const u8 byte : data) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

static void InvalidateReadbackBuffer(const VideoCore::Buffer& buffer) {
    if (buffer.is_coherent || buffer.SizeBytes() == 0) {
        return;
    }
    vmaInvalidateAllocation(buffer.instance->GetAllocator(), buffer.buffer.allocation, 0,
                            buffer.SizeBytes());
}

static bool WritePng(const std::filesystem::path& path, const std::span<const u8> rgba,
                     const u32 width, const u32 height) {
    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Create);
    if (!file.IsOpen()) {
        return false;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png_ptr == nullptr) {
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == nullptr) {
        png_destroy_write_struct(&png_ptr, nullptr);
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr)) != 0) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return false;
    }

    png_init_io(png_ptr, file.file);
    png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    thread_local std::vector<png_bytep> rows;
    rows.resize(height);
    for (u32 y = 0; y < height; ++y) {
        rows[y] = const_cast<png_bytep>(rgba.data() + static_cast<size_t>(y) * width * 4);
    }

    png_write_image(png_ptr, rows.data());
    png_write_end(png_ptr, info_ptr);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return true;
}

static void LogScreenshotStats(const std::filesystem::path& path, const std::span<const u8> rgba,
                               const u32 width, const u32 height) {
    const auto stats = ComputeLumaStats(rgba, width, height);
    LOG_INFO(Render_Vulkan,
             "TRACE_SCREENSHOT path={} size={}x{} avg_luma={:.2f} variance={:.2f} max_luma={} "
             "near_black={:.2f}%",
             path.string(), width, height, stats.avg_luma, stats.variance, stats.max_luma,
             stats.near_black_pct);
}

static void ProcessBlackWatchdogReadbacks(
    const std::shared_ptr<BlackFrameWatchdog>& watchdog,
    const std::vector<ScreenshotReadback>& readbacks) {
    if (!watchdog || !IsStrictBlackScreenWatchdogEnabled()) {
        return;
    }
    if (!Common::Trace::IsBlackScreenWatchdogArmed()) {
        return;
    }

    for (const auto& readback : readbacks) {
        if (!readback.watchdog) {
            continue;
        }
        InvalidateReadbackBuffer(readback.buffer);

        std::vector<u8> rgba;
        const bool converted = ConvertReadbackToRgba8(readback, rgba);
        ASSERT_MSG(converted,
                   "Strict black-screen watchdog: failed to convert {} readback frame={} "
                   "size={}x{} format={}",
                   ScreenshotKindName(readback.kind), readback.watchdog_context.frame_index,
                   readback.width, readback.height, vk::to_string(readback.format));

        const auto stats = ComputeLumaStats(rgba, readback.width, readback.height);
        const size_t stage_index = static_cast<size_t>(readback.kind);
        ASSERT_MSG(stage_index < watchdog->consecutive_black.size(),
                   "Strict black-screen watchdog: invalid stage index {}", stage_index);

        std::scoped_lock lock{watchdog->mutex};
        const bool armed = Common::Trace::IsBlackScreenWatchdogArmed();
        if (armed && !watchdog->last_armed) {
            watchdog->saw_nonblack_game_frame = false;
            watchdog->consecutive_black.fill(0);
            LOG_WARNING(Render_Vulkan,
                        "TRACE_BLACK_WATCHDOG armed; waiting for first nonblack GameOnly frame");
        } else if (!armed && watchdog->last_armed) {
            watchdog->saw_nonblack_game_frame = false;
            watchdog->consecutive_black.fill(0);
            LOG_WARNING(Render_Vulkan, "TRACE_BLACK_WATCHDOG disarmed");
        }
        watchdog->last_armed = armed;

        const bool is_black = watchdog->IsNearBlack(stats);
        if (readback.kind == ScreenshotKind::GameOnly && !is_black) {
            watchdog->saw_nonblack_game_frame = true;
        }

        if (watchdog->saw_nonblack_game_frame && is_black) {
            watchdog->consecutive_black[stage_index]++;
        } else {
            watchdog->consecutive_black[stage_index] = 0;
        }
        watchdog->last_stats[stage_index] = stats;

        const auto& ctx = readback.watchdog_context;
        LOG_INFO(Render_Vulkan,
                 "TRACE_BLACK_WATCHDOG stage={} frame={} active={} black={} consecutive={} "
                 "size={}x{} format={} avg_luma={:.2f} variance={:.2f} max_luma={} "
                 "near_black={:.2f}% nonblack={} guest_avg_luma={:.2f} "
                 "guest_variance={:.2f} guest_max_luma={} "
                 "guest_near_black={:.2f}% guest_nonblack={} last_writer={} "
                 "last_writer_seq={} last_writer_addr={:#x} last_writer_size={} "
                 "last_writer_detail0={:#x} last_writer_detail1={:#x} "
                 "videoout_addr={:#x} image_id={} guest_size={} flags={:#x} "
                 "usage=t{}s{}rt{}dt{} samples={} backing_samples={} layout={} cmask={:#x} "
                 "fmask={:#x} htile={:#x} frame_image={:#x} frame_view={:#x} "
                 "frame_texture={:#x}",
                 ScreenshotKindName(readback.kind), ctx.frame_index,
                 watchdog->saw_nonblack_game_frame, is_black,
                 watchdog->consecutive_black[stage_index], readback.width, readback.height,
                 vk::to_string(readback.format), stats.avg_luma, stats.variance, stats.max_luma,
                 stats.near_black_pct, stats.nonblack_pixels, ctx.guest_stats.avg_luma,
                 ctx.guest_stats.variance, ctx.guest_stats.max_luma,
                 ctx.guest_stats.near_black_pct, ctx.guest_stats.nonblack_pixels,
                 ctx.last_write_op ? ctx.last_write_op : "none",
                 ctx.last_write_sequence, ctx.last_write_address, ctx.last_write_size,
                 ctx.last_write_detail0, ctx.last_write_detail1, ctx.videoout_addr, ctx.image_id,
                 ctx.guest_size, ctx.flags, ctx.usage_texture, ctx.usage_storage,
                 ctx.usage_render_target, ctx.usage_depth_target, ctx.image_samples,
                 ctx.backing_samples, vk::to_string(ctx.layout), ctx.cmask_addr, ctx.fmask_addr,
                 ctx.htile_addr, ctx.frame_image, ctx.frame_view, ctx.frame_texture);

        const bool persistent_unexpected_black =
            watchdog->saw_nonblack_game_frame && is_black &&
            watchdog->consecutive_black[stage_index] >= watchdog->config.consecutive_frames;
        const bool has_writer_breadcrumb = ctx.last_write_sequence != 0;
        const bool abort_without_writer =
            Common::Trace::EnvEnabled("SHADPS4_BLACK_WATCHDOG_ABORT_WITHOUT_WRITER");
        if (persistent_unexpected_black && !has_writer_breadcrumb && !abort_without_writer) {
            LOG_WARNING(Render_Vulkan,
                        "TRACE_BLACK_WATCHDOG persistent_black_no_writer stage={} frame={} "
                        "consecutive={} threshold={} videoout_addr={:#x} image_id={} "
                        "usage=t{}s{}rt{}dt{} layout={} avg_luma={:.2f} variance={:.2f} max_luma={} "
                        "near_black={:.2f}% nonblack={}",
                        ScreenshotKindName(readback.kind), ctx.frame_index,
                        watchdog->consecutive_black[stage_index],
                        watchdog->config.consecutive_frames, ctx.videoout_addr, ctx.image_id,
                        ctx.usage_texture, ctx.usage_storage, ctx.usage_render_target,
                        ctx.usage_depth_target, vk::to_string(ctx.layout), stats.avg_luma,
                        stats.variance, stats.max_luma, stats.near_black_pct,
                        stats.nonblack_pixels);
        }

        ASSERT_MSG(!persistent_unexpected_black ||
                       (!has_writer_breadcrumb && !abort_without_writer),
                   "Strict black-screen watchdog: persistent unexpected black at stage={} "
                   "frame={} consecutive={} threshold={} size={}x{} format={} avg_luma={:.2f} "
                   "variance={:.2f} max_luma={} near_black={:.2f}% nonblack={} "
                   "guest_avg_luma={:.2f} guest_variance={:.2f} guest_max_luma={} "
                   "guest_near_black={:.2f}% guest_nonblack={} "
                   "last_writer={} last_writer_seq={} last_writer_addr={:#x} "
                   "last_writer_size={} last_writer_detail0={:#x} last_writer_detail1={:#x} "
                   "videoout_addr={:#x} image_id={} "
                   "guest_size={} flags={:#x} usage=t{}s{}rt{}dt{} samples={} "
                   "backing_samples={} layout={} cmask={:#x} fmask={:#x} htile={:#x} "
                   "frame_image={:#x} frame_view={:#x} frame_texture={:#x}",
                   ScreenshotKindName(readback.kind), ctx.frame_index,
                   watchdog->consecutive_black[stage_index],
                   watchdog->config.consecutive_frames, readback.width, readback.height,
                   vk::to_string(readback.format), stats.avg_luma, stats.variance,
                   stats.max_luma, stats.near_black_pct, stats.nonblack_pixels,
                   ctx.guest_stats.avg_luma, ctx.guest_stats.variance, ctx.guest_stats.max_luma,
                   ctx.guest_stats.near_black_pct, ctx.guest_stats.nonblack_pixels,
                   ctx.last_write_op ? ctx.last_write_op : "none",
                   ctx.last_write_sequence, ctx.last_write_address, ctx.last_write_size,
                   ctx.last_write_detail0, ctx.last_write_detail1, ctx.videoout_addr,
                   ctx.image_id, ctx.guest_size, ctx.flags, ctx.usage_texture, ctx.usage_storage,
                   ctx.usage_render_target, ctx.usage_depth_target, ctx.image_samples,
                   ctx.backing_samples, vk::to_string(ctx.layout), ctx.cmask_addr, ctx.fmask_addr,
                   ctx.htile_addr, ctx.frame_image, ctx.frame_view, ctx.frame_texture);
    }
}

static void SavePendingScreenshots(const std::vector<ScreenshotReadback>& readbacks) {
    const bool stats_only = IsTraceScreenshotStatsOnly();
    for (const auto& readback : readbacks) {
        if (readback.paths.empty()) {
            continue;
        }

        InvalidateReadbackBuffer(readback.buffer);

        std::vector<u8> rgba;
        if (!ConvertReadbackToRgba8(readback, rgba)) {
            continue;
        }

        const auto& primary_path = readback.paths.front();
        if (stats_only) {
            LogScreenshotStats(primary_path, rgba, readback.width, readback.height);
            continue;
        }

        if (!WritePng(primary_path, rgba, readback.width, readback.height)) {
            LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", primary_path.string());
            continue;
        }

        LOG_INFO(Render_Vulkan, "Saved screenshot: {}", primary_path.string());
        LogScreenshotStats(primary_path, rgba, readback.width, readback.height);

        std::ifstream file(primary_path, std::ios::binary);
        std::vector<u8> imgdata;
        if (file) {
            imgdata = std::vector<u8>(std::istreambuf_iterator<char>(file),
                                      std::istreambuf_iterator<char>());
        }
        shadNotifications::QueueNotification("Saved screenshot:\n" + primary_path.string(), 3.0f,
                                             shadNotifications::position::BottomRight, imgdata);

        for (size_t i = 1; i < readback.paths.size(); ++i) {
            const auto& path = readback.paths[i];
            std::error_code ec{};
            std::filesystem::copy_file(primary_path, path, std::filesystem::copy_options::none, ec);
            if (ec) {
                // Fallback for platforms/filesystems where copy_file can fail for transient
                // reasons.
                if (!WritePng(path, rgba, readback.width, readback.height)) {
                    LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", path.string());
                    continue;
                }
            }

            LOG_INFO(Render_Vulkan, "Saved screenshot: {}", path.string());
            LogScreenshotStats(path, rgba, readback.width, readback.height);
            std::ifstream file(path, std::ios::binary);
            std::vector<u8> imgdata;
            if (file) {
                imgdata = std::vector<u8>(std::istreambuf_iterator<char>(file),
                                          std::istreambuf_iterator<char>());
            }
            shadNotifications::QueueNotification("Saved screenshot:\n" + path.string(), 3.0f,
                                                 shadNotifications::position::BottomRight, imgdata);
        }
    }
}

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, "draw"}, present_scheduler{instance, "present"},
      flip_scheduler{instance, "flip"},
      swapchain{instance, window},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    // The three schedulers share one Metal queue but each has an independent timeline. Register
    // them as siblings so any deferred GPU-resource destruction waits for the other two
    // schedulers' in-flight command buffers to complete before freeing — closing the
    // cross-scheduler use-after-free that MoltenVK reports as kIOGPU Invalid Resource (device
    // loss). This is strictly more conservative than the previous per-scheduler tick gate: it can
    // only ever delay a free, never free earlier, so it cannot corrupt rendering.
    draw_scheduler.AddSibling(&present_scheduler);
    draw_scheduler.AddSibling(&flip_scheduler);
    present_scheduler.AddSibling(&draw_scheduler);
    present_scheduler.AddSibling(&flip_scheduler);
    flip_scheduler.AddSibling(&draw_scheduler);
    flip_scheduler.AddSibling(&present_scheduler);

    if (IsStrictBlackScreenWatchdogEnabled()) {
        black_frame_watchdog = std::make_shared<BlackFrameWatchdog>();
        const auto& cfg = black_frame_watchdog->config;
        LOG_WARNING(Render_Vulkan,
                    "Strict black-screen watchdog enabled: near_black_pct={:.2f} max_luma={} "
                    "avg_luma={:.2f} consecutive_frames={} armed={}",
                    cfg.near_black_pct, cfg.max_luma, cfg.avg_luma, cfg.consecutive_frames,
                    Common::Trace::IsBlackScreenWatchdogArmed());
    }

    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);

    ImGui::Layer::AddLayer(Common::Singleton<Core::Devtools::Layer>::Instance());
}

Presenter::~Presenter() {
    ImGui::Layer::RemoveLayer(Common::Singleton<Core::Devtools::Layer>::Instance());

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        if (frame.imgui_texture) {
            ImGui::Vulkan::RemoveTexture(frame.imgui_texture);
            frame.imgui_texture = nullptr;
        }
        if (frame.image_view) {
            device.destroyImageView(frame.image_view);
        }
        if (frame.image) {
            vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        }
        if (frame.present_done) {
            device.destroyFence(frame.present_done);
        }
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->imgui_texture) {
        ImGui::Vulkan::RemoveTexture(frame->imgui_texture);
        frame->imgui_texture = nullptr;
    }
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;
    frame->format = format;

    frame->imgui_texture = ImGui::Vulkan::AddTexture(view, vk::ImageLayout::eShaderReadOnlyOptimal);
    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    const u64 timeout = GpuWaitTimeoutNs();
    u32 timeout_count = 0;
    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false, timeout);
        if (result == vk::Result::eSuccess) {
            break;
        }
        ASSERT_MSG(!IsStrictRenderValidationEnabled() || result != vk::Result::eTimeout,
                   "Strict render validation: timed out waiting for last submitted frame fence "
                   "frame={} image={:#x} size={}x{} ready_tick={}",
                   static_cast<const void*>(frame),
                   reinterpret_cast<uintptr_t>(static_cast<VkImage>(frame->image)), frame->width,
                   frame->height, frame->ready_tick);
        if (result == vk::Result::eTimeout) {
            ++timeout_count;
            LogGpuWaitTimeout("prepare_last_frame_present_done", timeout);
            if (AbortGpuWaitIfRetryLimitReached("prepare_last_frame_present_done",
                                                timeout_count)) {
                return nullptr;
            }
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    // The ImGui descriptor for frame images is registered as ShaderReadOnlyOptimal. Reusing the
    // last submitted frame does not write it, so keep the image in the descriptor's layout.
    frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        if (Common::Trace::EnvEnabled("SHADPS4_VIDEOOUT_VIEW_MATCH_IMAGE_FORMAT")) {
            static bool logged = false;
            if (!logged) {
                LOG_WARNING(Render_Vulkan,
                            "SHADPS4_VIDEOOUT_VIEW_MATCH_IMAGE_FORMAT=1: using A2B10G10R10 "
                            "VideoOut view format instead of A2R10G10B10");
                logged = true;
            }
            return vk::Format::eA2B10G10R10UnormPack32;
        }
        return vk::Format::eA2R10G10B10UnormPack32;
    case Libraries::VideoOut::PixelFormat::A16R16G16B16Float:
        return vk::Format::eR16G16B16A16Sfloat;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    static std::atomic<u64> trace_video_out_frame{0};
    static std::atomic<VAddr> trace_last_video_out_address{0};

    auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
    const auto image_id = texture_cache.FindImage(desc);
    ASSERT_MSG(!IsStrictRenderValidationEnabled() || image_id,
               "Strict render validation: VideoOut image lookup returned null cpu_addr={:#x} "
               "attr={}x{} pitch={} format={} tiling={} option={:#x}",
               cpu_address, attribute.attrib.width, attribute.attrib.height,
               attribute.attrib.pitch_in_pixel,
               Libraries::VideoOut::GetPixelFormatString(attribute.attrib.pixel_format),
               attribute.attrib.tiling_mode == Libraries::VideoOut::TilingMode::Tile ? "tile"
                                                                                     : "linear",
               attribute.attrib.option);
    texture_cache.UpdateImage(image_id);

    Frame* frame = GetRenderFrame();
    if (frame == nullptr) {
        LOG_ERROR(Render_Vulkan,
                  "Skipping VideoOut frame because no reusable presentation frame became "
                  "available after the GPU wait retry limit cpu_addr={:#x} attr={}x{}",
                  cpu_address, attribute.attrib.width, attribute.attrib.height);
        return nullptr;
    }
    ASSERT_MSG(!IsStrictRenderValidationEnabled() ||
                   (frame != nullptr && frame->image && frame->image_view && frame->imgui_texture &&
                    frame->width > 0 && frame->height > 0),
               "Strict render validation: invalid render frame frame={} image={:#x} view={:#x} "
               "imgui_texture={} size={}x{}",
               static_cast<const void*>(frame),
               frame ? reinterpret_cast<uintptr_t>(static_cast<VkImage>(frame->image)) : 0,
               frame ? reinterpret_cast<uintptr_t>(static_cast<VkImageView>(frame->image_view)) : 0,
               frame ? reinterpret_cast<uintptr_t>(frame->imgui_texture) : 0,
               frame ? frame->width : 0, frame ? frame->height : 0);

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    auto& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(!IsStrictRenderValidationEnabled() ||
                   (image.info.guest_address == cpu_address && image.info.size.width > 0 &&
                    image.info.size.height > 0 && image.info.guest_size > 0 &&
                    image.info.resources.layers > 0 && image.info.resources.levels > 0),
               "Strict render validation: invalid VideoOut resolved image id={} cpu_addr={:#x} "
               "guest_addr={:#x} guest_size={} size={}x{}x{} layers={} levels={} format={} "
               "flags={:#x}",
               image_id.index, cpu_address, image.info.guest_address, image.info.guest_size,
               image.info.size.width, image.info.size.height, image.info.size.depth,
               image.info.resources.layers, image.info.resources.levels,
               vk::to_string(image.info.pixel_format), static_cast<u32>(image.flags));
    auto image_view = *image.FindView(view_info).image_view;
    const vk::Extent2D image_size = {image.info.size.width, image.info.size.height};
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    const auto frame_number = trace_video_out_frame.fetch_add(1, std::memory_order_relaxed);
    const bool trace_video_out =
        IsTraceRenderEnabled() &&
        (frame_number % std::min<u64>(GetTraceVideoOutInterval(), 15ULL) == 0);
    if (trace_video_out) {
        const auto previous_address =
            trace_last_video_out_address.exchange(cpu_address, std::memory_order_relaxed);
        const u32 usage_texture = image.usage.texture;
        const u32 usage_storage = image.usage.storage;
        const u32 usage_render_target = image.usage.render_target;
        const u32 usage_depth_target = image.usage.depth_target;
        LOG_INFO(Render_Vulkan,
                 "TRACE_VIDEO_OUT frame={} cpu_addr={:#x} changed={} image_id={} guest_addr={:#x} "
                 "guest_size={} attr={}x{} pitch={} format={} tiling={} option={:#x} image={}x{} "
                 "vk_format={} tile_mode={} array_mode={} bits={} samples={} cmask={:#x} "
                 "fmask={:#x} htile={:#x} flags={:#x} backing_samples={} backing_layout={} "
                 "usage=t{}s{}rt{}dt{}",
                 frame_number, cpu_address, previous_address != cpu_address, image_id.index,
                 image.info.guest_address, image.info.guest_size, attribute.attrib.width,
                 attribute.attrib.height, attribute.attrib.pitch_in_pixel,
                 Libraries::VideoOut::GetPixelFormatString(attribute.attrib.pixel_format),
                 attribute.attrib.tiling_mode == Libraries::VideoOut::TilingMode::Tile ? "tile"
                                                                                       : "linear",
                 attribute.attrib.option, image_size.width, image_size.height,
                 vk::to_string(image.info.pixel_format), static_cast<u32>(image.info.tile_mode),
                 static_cast<u32>(image.info.array_mode), image.info.num_bits,
                 image.info.num_samples, image.info.meta_info.cmask_addr,
                 image.info.meta_info.fmask_addr, image.info.meta_info.htile_addr,
                 static_cast<u32>(image.flags), image.backing->num_samples,
                 vk::to_string(image.backing->state.layout), usage_texture, usage_storage,
                 usage_render_target, usage_depth_target);
    }

    static std::atomic_bool first_presented_frame_hashed{false};
    const bool hash_first_frame = !first_presented_frame_hashed.exchange(true);

    const bool black_watchdog_enabled =
        black_frame_watchdog != nullptr && IsStrictBlackScreenWatchdogEnabled() &&
        Common::Trace::IsBlackScreenWatchdogArmed() &&
        !Libraries::SystemService::IsSplashVisible();
    const bool trace_present_luma =
        IsTracePresentLumaEnabled() &&
        frame_number % GetTracePresentLumaInterval() == 0;
    Common::Trace::RegisterVideoOutRange(cpu_address, image.info.guest_size);
    const auto last_write =
        Common::Trace::GetLastVideoOutWrite(cpu_address, image.info.guest_size);
    const auto guest_stats =
        (black_watchdog_enabled || trace_present_luma || hash_first_frame)
            ? ComputeRawGuestLumaStats(cpu_address, image.info.size.width, image.info.size.height,
                                       image.info.pitch, image.info.num_bits,
                                       image.info.guest_size)
            : LumaStats{};
    if (hash_first_frame) {
        const u64 frame_hash =
            ComputeRawGuestFrameHash(cpu_address, image.info.size.width, image.info.size.height,
                                     image.info.pitch, image.info.num_bits, image.info.guest_size);
        const bool blank = guest_stats.pixel_count != 0 && guest_stats.near_black_pct >= 99.5 &&
                           guest_stats.max_luma <= 8 && guest_stats.avg_luma <= 2.0;
        LOG_INFO(Render_Vulkan,
                 "FIRST_PRESENTED_FRAME hash={:#x} class={} cpu_addr={:#x} image_id={} "
                 "size={}x{} guest_size={} avg_luma={:.2f} variance={:.2f} max_luma={} "
                 "near_black={:.2f}% nonblack={}",
                 frame_hash, blank ? "blank" : "drawn", cpu_address, image_id.index,
                 image.info.size.width, image.info.size.height, image.info.guest_size,
                 guest_stats.avg_luma, guest_stats.variance, guest_stats.max_luma,
                 guest_stats.near_black_pct, guest_stats.nonblack_pixels);
    }
    if (trace_present_luma) {
        const bool blank = guest_stats.pixel_count != 0 && guest_stats.near_black_pct >= 99.5 &&
                           guest_stats.max_luma <= 8 && guest_stats.avg_luma <= 2.0;
        LOG_INFO(Render_Vulkan,
                 "TRACE_PRESENT_LUMA frame={} class={} cpu_addr={:#x} image_id={} size={}x{} "
                 "guest_size={} avg_luma={:.2f} variance={:.2f} max_luma={} "
                 "near_black={:.2f}% nonblack={} last_writer={} last_writer_seq={} "
                 "last_writer_addr={:#x} last_writer_size={} usage=t{}s{}rt{}dt{} layout={}",
                 frame_number, blank ? "blank" : "drawn", cpu_address, image_id.index,
                 image.info.size.width, image.info.size.height, image.info.guest_size,
                 guest_stats.avg_luma, guest_stats.variance, guest_stats.max_luma,
                 guest_stats.near_black_pct, guest_stats.nonblack_pixels,
                 last_write.op ? last_write.op : "none", last_write.sequence, last_write.address,
                 last_write.size, static_cast<u32>(image.usage.texture),
                 static_cast<u32>(image.usage.storage),
                 static_cast<u32>(image.usage.render_target),
                 static_cast<u32>(image.usage.depth_target),
                 vk::to_string(image.backing->state.layout));
    }
    const WatchdogReadbackContext watchdog_context{
        .frame_index = frame_number,
        .videoout_addr = cpu_address,
        .image_id = image_id.index,
        .guest_size = image.info.guest_size,
        .flags = static_cast<u32>(image.flags),
        .usage_texture = image.usage.texture,
        .usage_storage = image.usage.storage,
        .usage_render_target = image.usage.render_target,
        .usage_depth_target = image.usage.depth_target,
        .image_samples = image.info.num_samples,
        .backing_samples = image.backing->num_samples,
        .cmask_addr = image.info.meta_info.cmask_addr,
        .fmask_addr = image.info.meta_info.fmask_addr,
        .htile_addr = image.info.meta_info.htile_addr,
        .layout = image.backing->state.layout,
        .guest_stats = guest_stats,
        .last_write_op = last_write.op,
        .last_write_address = last_write.address,
        .last_write_size = last_write.size,
        .last_write_detail0 = last_write.detail0,
        .last_write_detail1 = last_write.detail1,
        .last_write_sequence = last_write.sequence,
        .frame_image = reinterpret_cast<uintptr_t>(static_cast<VkImage>(frame->image)),
        .frame_view = reinterpret_cast<uintptr_t>(static_cast<VkImageView>(frame->image_view)),
        .frame_texture = reinterpret_cast<uintptr_t>(frame->imgui_texture),
    };
    frame->watchdog_frame_index = watchdog_context.frame_index;
    frame->watchdog_videoout_addr = watchdog_context.videoout_addr;
    frame->watchdog_image_id = watchdog_context.image_id;
    frame->watchdog_guest_size = watchdog_context.guest_size;
    frame->watchdog_flags = watchdog_context.flags;
    frame->watchdog_usage_texture = watchdog_context.usage_texture;
    frame->watchdog_usage_storage = watchdog_context.usage_storage;
    frame->watchdog_usage_render_target = watchdog_context.usage_render_target;
    frame->watchdog_usage_depth_target = watchdog_context.usage_depth_target;
    frame->watchdog_image_samples = watchdog_context.image_samples;
    frame->watchdog_backing_samples = watchdog_context.backing_samples;
    frame->watchdog_cmask_addr = watchdog_context.cmask_addr;
    frame->watchdog_fmask_addr = watchdog_context.fmask_addr;
    frame->watchdog_htile_addr = watchdog_context.htile_addr;
    frame->watchdog_layout = watchdog_context.layout;

    const u32 capture_game_only_count = VideoCore::ConsumeGameOnlyScreenshotRequests();
    std::vector<ScreenshotReadback> pending_screenshots;
    if (capture_game_only_count > 0 || black_watchdog_enabled) {
        pending_screenshots.reserve(2);
        const bool hdr_encoded =
            attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq;
        if (capture_game_only_count > 0) {
            pending_screenshots.emplace_back(
                instance, draw_scheduler, ScreenshotKind::GameOnly,
                BuildScreenshotPaths(ScreenshotKind::GameOnly, capture_game_only_count),
                image_size.width, image_size.height, view_info.format, hdr_encoded);
        }
        if (black_watchdog_enabled) {
            pending_screenshots.emplace_back(instance, draw_scheduler, ScreenshotKind::GameOnly,
                                             std::vector<std::filesystem::path>{},
                                             image_size.width, image_size.height, view_info.format,
                                             hdr_encoded, watchdog_context, true);
        }

        // Capture the guest output before any host-side scaling (FSR/PP) is applied.
        image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
                      cmdbuf);
        for (auto& readback : pending_screenshots) {
            CopyImageToReadback(cmdbuf, image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                readback);
        }
    }

    // Continue with host-side passes that draw the displayed (scaled) frame.
    image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {},
                  cmdbuf);

    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);

    DebugState.game_resolution = {image_size.width, image_size.height};
    DebugState.output_resolution = {frame->width, frame->height};

    std::shared_ptr<std::vector<ScreenshotReadback>> deferred_screenshots{};
    if (!pending_screenshots.empty()) {
        deferred_screenshots =
            std::make_shared<std::vector<ScreenshotReadback>>(std::move(pending_screenshots));
        const auto watchdog = black_frame_watchdog;
        draw_scheduler.DeferPriorityOperation(
            [deferred_screenshots, watchdog]() {
                SavePendingScreenshots(*deferred_screenshots);
                ProcessBlackWatchdogReadbacks(watchdog, *deferred_screenshots);
            });
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    if (frame == nullptr) {
        LOG_ERROR(Render_Vulkan,
                  "Skipping blank frame because no reusable presentation frame became available "
                  "after the GPU wait retry limit");
        return nullptr;
    }

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::Present(Frame* frame, bool is_reusing_frame) {
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // Recreate the swapchain if the window was resized.
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    swapchain.NotePendingFrameTick(frame->ready_tick);
    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan,
                        "Skipping frame after swapchain image acquisition failed twice; "
                        "size={}x{} frame={} frame_image={:#x}",
                        window.GetWidth(), window.GetHeight(), static_cast<const void*>(frame),
                        reinterpret_cast<uintptr_t>(static_cast<VkImage>(frame->image)));
            free_frame();
            return;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    ImGuiID dockId = ImGui::Core::NewFrame(is_reusing_frame);

    const vk::Image swapchain_image = swapchain.Image();
    const vk::ImageView swapchain_image_view = swapchain.ImageView();
    ASSERT_MSG(!IsStrictRenderValidationEnabled() ||
                   (swapchain_image && swapchain_image_view && frame && frame->imgui_texture),
               "Strict render validation: invalid present inputs swapchain_image={:#x} "
               "swapchain_view={:#x} frame={} frame_texture={}",
               reinterpret_cast<uintptr_t>(static_cast<VkImage>(swapchain_image)),
               reinterpret_cast<uintptr_t>(static_cast<VkImageView>(swapchain_image_view)),
               static_cast<const void*>(frame),
               frame ? reinterpret_cast<uintptr_t>(frame->imgui_texture) : 0);

    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
    const u32 capture_with_overlays_count = VideoCore::ConsumeWithOverlaysScreenshotRequests();
    const bool black_watchdog_enabled =
        black_frame_watchdog != nullptr && IsStrictBlackScreenWatchdogEnabled() &&
        Common::Trace::IsBlackScreenWatchdogArmed() &&
        !Libraries::SystemService::IsSplashVisible();
    const WatchdogReadbackContext watchdog_context{
        .frame_index = frame->watchdog_frame_index,
        .videoout_addr = frame->watchdog_videoout_addr,
        .image_id = frame->watchdog_image_id,
        .guest_size = frame->watchdog_guest_size,
        .flags = frame->watchdog_flags,
        .usage_texture = frame->watchdog_usage_texture,
        .usage_storage = frame->watchdog_usage_storage,
        .usage_render_target = frame->watchdog_usage_render_target,
        .usage_depth_target = frame->watchdog_usage_depth_target,
        .image_samples = frame->watchdog_image_samples,
        .backing_samples = frame->watchdog_backing_samples,
        .cmask_addr = frame->watchdog_cmask_addr,
        .fmask_addr = frame->watchdog_fmask_addr,
        .htile_addr = frame->watchdog_htile_addr,
        .layout = frame->watchdog_layout,
        .frame_image = reinterpret_cast<uintptr_t>(static_cast<VkImage>(frame->image)),
        .frame_view = reinterpret_cast<uintptr_t>(static_cast<VkImageView>(frame->image_view)),
        .frame_texture = reinterpret_cast<uintptr_t>(frame->imgui_texture),
    };
    std::vector<ScreenshotReadback> pending_screenshots;
    if (capture_with_overlays_count > 0 || black_watchdog_enabled) {
        pending_screenshots.reserve(3);
    }

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        auto* profiler_ctx = instance.GetProfilerContext();
        TracyVkNamedZoneC(profiler_ctx, renderer_gpu_zone, cmdbuf, "Host frame",
                          MarkersPalette::GpuMarkerColor, profiler_ctx != nullptr);

        const vk::Extent2D extent = swapchain.GetExtent();
        if (black_watchdog_enabled) {
            pending_screenshots.emplace_back(instance, scheduler, ScreenshotKind::FrameImage,
                                             std::vector<std::filesystem::path>{}, frame->width,
                                             frame->height, frame->format, frame->is_hdr,
                                             watchdog_context, true);
            auto& readback = pending_screenshots.back();

            const vk::ImageMemoryBarrier frame_to_transfer{
                .srcAccessMask = vk::AccessFlagBits::eShaderRead,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, frame_to_transfer);
            CopyImageToReadback(cmdbuf, frame->image, vk::ImageLayout::eTransferSrcOptimal,
                                readback);

            const vk::ImageMemoryBarrier frame_to_general{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                   vk::PipelineStageFlagBits::eFragmentShader,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, frame_to_general);
        }

        const vk::ImageMemoryBarrier pre_barrier{
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };

        bool swapchain_copied_for_screenshot = false;

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);

        { // Draw the game
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
            ImGui::SetNextWindowDockID(dockId, ImGuiCond_Once);
            if (ImGui::Begin("Display##game_display", nullptr, ImGuiWindowFlags_NoNav)) {
                auto game_texture = frame->imgui_texture;
                auto game_width = frame->width;
                auto game_height = frame->height;

                if (Libraries::SystemService::IsSplashVisible()) { // draw splash
                    if (!splash_img.has_value()) {
                        splash_img.emplace();
                        auto splash_path = Common::ElfInfo::Instance().GetSplashPath();
                        if (!splash_path.empty()) {
                            splash_img = ImGui::RefCountedTexture::DecodePngFile(splash_path);
                        }
                    }
                    if (auto& splash_image = this->splash_img.value()) {
                        auto [im_id, width, height] = splash_image.GetTexture();
                        game_texture = im_id;
                        game_width = width;
                        game_height = height;
                    }
                }

                ImVec2 contentArea = ImGui::GetContentRegionAvail();
                SetExpectedGameSize((s32)contentArea.x, (s32)contentArea.y);

                const auto imgRect =
                    FitImage(game_width, game_height, (s32)contentArea.x, (s32)contentArea.y);
                ImVec2 offset{
                    static_cast<float>(imgRect.offset.x),
                    static_cast<float>(imgRect.offset.y),
                };
                ImVec2 size{
                    static_cast<float>(imgRect.extent.width),
                    static_cast<float>(imgRect.extent.height),
                };

                ImGui::SetCursorPos(ImGui::GetCursorStartPos() + offset);
                ImGui::Image(game_texture, size);

                if (EmulatorSettings.IsNullGPU()) {
                    Core::Devtools::Layer::DrawNullGpuNotice();
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();
        }
        ImGui::Core::Render(cmdbuf, swapchain_image_view, swapchain.GetExtent());

        if (capture_with_overlays_count > 0 || black_watchdog_enabled) {
            if (capture_with_overlays_count > 0) {
                pending_screenshots.emplace_back(
                    instance, scheduler, ScreenshotKind::WithOverlays,
                    BuildScreenshotPaths(ScreenshotKind::WithOverlays, capture_with_overlays_count),
                    extent.width, extent.height, swapchain.GetCurrentImageFormat(),
                    swapchain.GetHDR());
            }
            if (black_watchdog_enabled) {
                pending_screenshots.emplace_back(instance, scheduler, ScreenshotKind::WithOverlays,
                                                 std::vector<std::filesystem::path>{},
                                                 extent.width, extent.height,
                                                 swapchain.GetCurrentImageFormat(),
                                                 swapchain.GetHDR(), watchdog_context, true);
            }

            const vk::ImageMemoryBarrier to_transfer{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_transfer);
            for (auto& readback : pending_screenshots) {
                if (readback.kind == ScreenshotKind::WithOverlays) {
                    CopyImageToReadback(cmdbuf, swapchain_image,
                                        vk::ImageLayout::eTransferSrcOptimal, readback);
                }
            }
            swapchain_copied_for_screenshot = true;
        }

        const vk::AccessFlags post_src_access_mask =
            swapchain_copied_for_screenshot ? vk::AccessFlagBits::eTransferRead
                                            : vk::AccessFlagBits::eColorAttachmentWrite;
        const vk::ImageLayout post_old_layout = swapchain_copied_for_screenshot
                                                    ? vk::ImageLayout::eTransferSrcOptimal
                                                    : vk::ImageLayout::eColorAttachmentOptimal;
        const vk::ImageMemoryBarrier post_barrier{
            .srcAccessMask = post_src_access_mask,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = post_old_layout,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);

        if (profiler_ctx) {
            TracyVkCollect(profiler_ctx, cmdbuf);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.
    std::shared_ptr<std::vector<ScreenshotReadback>> deferred_screenshots{};
    if (!pending_screenshots.empty()) {
        deferred_screenshots =
            std::make_shared<std::vector<ScreenshotReadback>>(std::move(pending_screenshots));
        const auto watchdog = black_frame_watchdog;
        scheduler.DeferPriorityOperation(
            [deferred_screenshots, watchdog]() {
                SavePendingScreenshots(*deferred_screenshots);
                ProcessBlackWatchdogReadbacks(watchdog, *deferred_screenshots);
            });
    }

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);

    // Present to swapchain.
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        if (!swapchain.Present()) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }

    free_frame();
    if (!is_reusing_frame) {
        DebugState.IncFlipFrameNum();
    }
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const u64 timeout = GpuWaitTimeoutNs();
    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, timeout);
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    u32 timeout_count = 0;
    while (wait() != vk::Result::eSuccess) {
        if (result != vk::Result::eTimeout) {
            LogGpuWaitFailure("get_render_frame_present_done", result);
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        ASSERT_MSG(!IsStrictRenderValidationEnabled() || result != vk::Result::eTimeout,
                   "Strict render validation: timed out waiting for reusable frame fence frame={} "
                   "image={:#x} size={}x{} expected={}x{} ready_tick={}",
                   static_cast<const void*>(frame),
                   reinterpret_cast<uintptr_t>(static_cast<VkImage>(frame->image)), frame->width,
                   frame->height, expected_frame_width, expected_frame_height, frame->ready_tick);
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            ++timeout_count;
            LogGpuWaitTimeout("get_render_frame_present_done", timeout);
            if (AbortGpuWaitIfRetryLimitReached("get_render_frame_present_done",
                                                timeout_count)) {
                return nullptr;
            }
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        ASSERT_MSG(!IsStrictRenderValidationEnabled(),
                   "Strict render validation: invalid expected game display size {}x{}", width,
                   height);
        return;
    }

    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
