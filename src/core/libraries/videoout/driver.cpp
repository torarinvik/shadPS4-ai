// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>

#include "common/assert.h"
#include "common/debug.h"
#include "common/trace_control.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/videoout_error.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Libraries::VideoOut {

constexpr static bool Is32BppPixelFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return true;
    default:
        return false;
    }
}

constexpr static bool IsSupportedPixelFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
    case PixelFormat::A16R16G16B16Float:
        return true;
    default:
        return false;
    }
}

constexpr u32 PixelFormatBpp(PixelFormat pixel_format) {
    switch (pixel_format) {
    case PixelFormat::A16R16G16B16Float:
        return 8;
    default:
        return 4;
    }
}

static u32 GetBootWatchdogSeconds() {
    static const u32 seconds = [] {
        const char* value = std::getenv("SHADPS4_BOOT_WATCHDOG_SECONDS");
        if (value == nullptr || value[0] == '\0') {
            return 10u;
        }
        char* end{};
        const unsigned long parsed = std::strtoul(value, &end, 10);
        return end != value ? static_cast<u32>(std::min<unsigned long>(parsed, 300)) : 10u;
    }();
    return seconds;
}

static int ValidateBufferAttribute(const BufferAttribute* attribute) {
    if (!IsSupportedPixelFormat(attribute->pixel_format)) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PIXEL_FORMAT;
    }
    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }
    return ORBIS_OK;
}

VideoOutDriver::VideoOutDriver(u32 width, u32 height) {
    main_port.resolution.full_width = width;
    main_port.resolution.full_height = height;
    main_port.resolution.pane_width = width;
    main_port.resolution.pane_height = height;
    present_thread = std::jthread([&](std::stop_token token) { PresentThread(token); });
}

VideoOutDriver::~VideoOutDriver() = default;

int VideoOutDriver::Open(const ServiceThreadParams* params) {
    if (main_port.is_open) {
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }
    main_port.is_open = true;
    main_port.saw_nonblank_flip = false;
    main_port.boot_watchdog_reported = false;
    main_port.open_time = std::chrono::steady_clock::now();
    liverpool->SetVoPort(&main_port);
    return 1;
}

void VideoOutDriver::Close(s32 handle) {
    std::scoped_lock lock{mutex};

    if (presenter) {
        for (const auto& buffer : main_port.buffer_slots) {
            if (buffer.group_index != -1) {
                presenter->UnregisterVideoOutSurface(buffer.address_left);
            }
        }
    }

    // Mark as closed
    main_port.is_open = false;
    main_port.flip_rate = 0;
    main_port.prev_index = -1;
    main_port.saw_nonblank_flip = false;
    main_port.boot_watchdog_reported = false;
    main_port.open_time = {};

    // Clear port information
    std::memset(main_port.buffer_labels.data(), 0, sizeof(main_port.buffer_labels));
    std::memset(main_port.groups.data(), 0, sizeof(main_port.groups));
    std::memset(&main_port.vblank_status, 0, sizeof(main_port.vblank_status));
    main_port.flip_status = FlipStatus{};

    // Re-initialize buffers
    std::memset(main_port.buffer_slots.data(), 0, sizeof(main_port.buffer_slots));
    for (auto& buffer : main_port.buffer_slots) {
        buffer.group_index = -1;
    }

    {
        std::scoped_lock port_lock{main_port.port_mutex};
        for (auto event : main_port.flip_events) {
            auto* equeue = Kernel::GetEqueue(event);
            if (equeue != nullptr) {
                equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                                    Kernel::OrbisKernelEvent::Filter::VideoOut);
            }
        }
        main_port.flip_events.clear();

        for (auto event : main_port.vblank_events) {
            auto* equeue = Kernel::GetEqueue(event);
            if (equeue != nullptr) {
                equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                    Kernel::OrbisKernelEvent::Filter::VideoOut);
            }
        }
        main_port.vblank_events.clear();
    }
}

VideoOutPort* VideoOutDriver::GetPort(int handle) {
    if (handle != 1) [[unlikely]] {
        return nullptr;
    }
    return &main_port;
}

int VideoOutDriver::RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses,
                                    s32 bufferNum, const BufferAttribute* attribute) {
    const int register_shape_result =
        (attribute == nullptr || addresses == nullptr || startIndex < 0 || bufferNum <= 0 ||
                 startIndex >= MaxDisplayBuffers || bufferNum > MaxDisplayBuffers ||
                 startIndex > MaxDisplayBuffers - bufferNum
             ? ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE
             : ORBIS_OK);

    if (port == nullptr || register_shape_result != ORBIS_OK) {
        LOG_ERROR(Lib_VideoOut, "Invalid register buffers request startIndex={}, bufferNum={}",
                  startIndex, bufferNum);
        return register_shape_result != ORBIS_OK ? register_shape_result
                                                 : ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const s32 group_index = port->FindFreeGroup();
    if (group_index >= MaxDisplayBufferGroups) {
        return ORBIS_VIDEO_OUT_ERROR_NO_EMPTY_SLOT;
    }

    const s32 end_index = startIndex + bufferNum;
    if (std::any_of(port->buffer_slots.begin() + startIndex, port->buffer_slots.begin() + end_index,
                    [](auto& buffer) { return buffer.group_index != -1; })) {
        return ORBIS_VIDEO_OUT_ERROR_SLOT_OCCUPIED;
    }

    const int attribute_result = ValidateBufferAttribute(attribute);
    if (attribute_result == ORBIS_VIDEO_OUT_ERROR_INVALID_PIXEL_FORMAT) {
        LOG_ERROR(Lib_VideoOut, "Invalid pixel format = {:#x}",
                  static_cast<u32>(attribute->pixel_format));
        return attribute_result;
    }
    if (attribute_result == ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return attribute_result;
    }
    if (attribute_result == ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return attribute_result;
    }
    if (attribute_result == ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return attribute_result;
    }
    if (attribute_result == ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return attribute_result;
    }
    if (attribute_result != ORBIS_OK) {
        return attribute_result;
    }
    for (u32 i = 0; i < bufferNum; i++) {
        if (addresses[i] == nullptr) {
            LOG_ERROR(Lib_VideoOut, "Invalid null buffer address at index {}", i + startIndex);
            return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
        }
    }

    LOG_INFO(Lib_VideoOut,
             "startIndex = {}, bufferNum = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             startIndex, bufferNum, GetPixelFormatString(attribute->pixel_format),
             attribute->aspect_ratio, static_cast<u32>(attribute->tiling_mode), attribute->width,
             attribute->height, attribute->pitch_in_pixel, attribute->option);

    auto& group = port->groups[group_index];
    std::memcpy(&group.attrib, attribute, sizeof(BufferAttribute));
    group.is_occupied = true;

    for (u32 i = 0; i < bufferNum; i++) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        port->buffer_slots[startIndex + i] = VideoOutBuffer{
            .group_index = group_index,
            .address_left = address,
            .address_right = 0,
        };

        // Reset flip label also when registering buffer
        port->buffer_labels[startIndex + i] = 0;
        port->SignalVoLabel();

        presenter->RegisterVideoOutSurface(group, address);
        LOG_INFO(Lib_VideoOut, "buffers[{}] = {:#x}", i + startIndex, address);
    }

    return group_index;
}

int VideoOutDriver::UnregisterBuffers(VideoOutPort* port, s32 attributeIndex) {
    if (port == nullptr || attributeIndex < 0 || attributeIndex >= MaxDisplayBufferGroups ||
        !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto& group = port->groups[attributeIndex];
    group.is_occupied = false;

    for (auto& buffer : port->buffer_slots) {
        if (buffer.group_index != attributeIndex) {
            continue;
        }
        if (presenter) {
            presenter->UnregisterVideoOutSurface(buffer.address_left);
        }
        buffer.group_index = -1;
    }

    return ORBIS_OK;
}

int VideoOutDriver::ChangeBufferAttribute(VideoOutPort* port, s32 attributeIndex,
                                          const BufferAttribute* attribute) {
    if (port == nullptr || attribute == nullptr || attributeIndex < 0 ||
        attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const int attribute_result = ValidateBufferAttribute(attribute);
    if (attribute_result == ORBIS_VIDEO_OUT_ERROR_INVALID_PIXEL_FORMAT) {
        LOG_ERROR(Lib_VideoOut, "Invalid pixel format = {:#x}",
                  static_cast<u32>(attribute->pixel_format));
        return attribute_result;
    }
    if (attribute_result == ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return attribute_result;
    }
    if (attribute_result == ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return attribute_result;
    }
    if (attribute_result == ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return attribute_result;
    }
    if (attribute_result == ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return attribute_result;
    }
    if (attribute_result != ORBIS_OK) {
        return attribute_result;
    }

    LOG_INFO(Lib_VideoOut,
             "attributeIndex = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             attributeIndex, GetPixelFormatString(attribute->pixel_format), attribute->aspect_ratio,
             static_cast<u32>(attribute->tiling_mode), attribute->width, attribute->height,
             attribute->pitch_in_pixel, attribute->option);

    std::vector<uintptr_t> active_addresses;
    {
        std::unique_lock lock{port->port_mutex};
        std::memcpy(&port->groups[attributeIndex].attrib, attribute, sizeof(BufferAttribute));
        for (const auto& buffer : port->buffer_slots) {
            if (buffer.group_index == attributeIndex) {
                active_addresses.push_back(buffer.address_left);
            }
        }
    }
    if (presenter) {
        const auto& group = port->groups[attributeIndex];
        for (const auto address : active_addresses) {
            presenter->RegisterVideoOutSurface(group, address);
        }
    }
    return 0;
}

void VideoOutDriver::Flip(const Request& req) {
    // Update HDR status before presenting.
    presenter->SetHDR(req.port->is_hdr);

    // Present the frame.
    presenter->Present(req.frame);

    // Update flip status.
    auto* port = req.port;
    {
        std::unique_lock lock{port->port_mutex};
        auto& flip_status = port->flip_status;
        flip_status.count++;
        flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        flip_status.flip_arg = req.flip_arg;
        flip_status.current_buffer = req.index;
        if (req.eop) {
            --flip_status.gc_queue_num;
        }
        --flip_status.flip_pending_num;
    }

    // Trigger flip events for the port.
    std::vector<Kernel::OrbisKernelEqueue> flip_events;
    {
        std::scoped_lock lock{port->port_mutex};
        flip_events = port->flip_events;
    }
    for (auto event : flip_events) {
        auto* equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (req.flip_arg << 16)));
        }
    }

    // Reset prev flip label
    if (port->prev_index != -1) {
        port->buffer_labels[port->prev_index] = 0;
        port->SignalVoLabel();
    }
    // save to prev buf index
    port->prev_index = req.index;
}

void VideoOutDriver::DrawBlankFrame() {
    const auto empty_frame = presenter->PrepareBlankFrame(false);
    if (empty_frame != nullptr) {
        presenter->Present(empty_frame);
    }
}

void VideoOutDriver::DrawLastFrame() {
    const auto frame = presenter->PrepareLastFrame();
    if (frame != nullptr) {
        presenter->Present(frame, true);
    }
}

void VideoOutDriver::CheckBootWatchdog() {
    const u32 threshold_seconds = GetBootWatchdogSeconds();
    if (threshold_seconds == 0 || !main_port.is_open || main_port.saw_nonblank_flip ||
        main_port.boot_watchdog_reported || main_port.open_time == decltype(main_port.open_time){}) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - main_port.open_time);
    if (elapsed.count() < threshold_seconds) {
        return;
    }

    std::scoped_lock queue_lock{mutex};
    std::scoped_lock port_lock{main_port.port_mutex};
    main_port.boot_watchdog_reported = true;
    LOG_WARNING(
        Lib_VideoOut,
        "BOOT_WATCHDOG stalled_after_videoout_open elapsed_s={} threshold_s={} "
        "registered_buffers={} queued_flips={} flip_pending={} gc_queue={} prev_index={} "
        "flip_rate={} vblank_count={} submit_tsc={} aggressive_logging={} "
        "black_watchdog_armed={}",
        elapsed.count(), threshold_seconds, main_port.NumRegisteredBuffers(), requests.size(),
        main_port.flip_status.flip_pending_num, main_port.flip_status.gc_queue_num,
        main_port.prev_index, main_port.flip_rate, main_port.vblank_status.count,
        main_port.flip_status.submit_tsc, Common::Trace::IsAggressiveLoggingEnabled(),
        Common::Trace::IsBlackScreenWatchdogArmed());
}

bool VideoOutDriver::SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg,
                                bool is_eop /*= false*/) {
    {
        std::unique_lock lock{port->port_mutex};
        if (index != -1 && port->flip_status.flip_pending_num > 16) {
            LOG_ERROR(Lib_VideoOut, "Flip queue is full");
            return false;
        }

        if (is_eop) {
            ++port->flip_status.gc_queue_num;
        }
        ++port->flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    }

    if (!is_eop) {
        // Non EOP flips can arrive from any thread so ask GPU thread to perform them
        liverpool->SendCommand([=, this]() { SubmitFlipInternal(port, index, flip_arg, is_eop); });
    } else {
        SubmitFlipInternal(port, index, flip_arg, is_eop);
    }

    return true;
}

void VideoOutDriver::SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop) {
    Vulkan::Frame* frame;
    if (index == -1) {
        frame = presenter->PrepareBlankFrame(false);
    } else {
        const auto& buffer = port->buffer_slots[index];
        ASSERT_MSG(buffer.group_index >= 0, "Trying to flip an unregistered buffer!");
        const auto& group = port->groups[buffer.group_index];
        frame = presenter->PrepareFrame(group, buffer.address_left);
    }

    if (frame == nullptr) {
        std::unique_lock lock{port->port_mutex};
        if (is_eop && port->flip_status.gc_queue_num > 0) {
            --port->flip_status.gc_queue_num;
        }
        if (port->flip_status.flip_pending_num > 0) {
            --port->flip_status.flip_pending_num;
        }
        return;
    }

    std::scoped_lock lock{mutex};
    if (index != -1) {
        port->saw_nonblank_flip = true;
    }
    requests.push({
        .frame = frame,
        .port = port,
        .flip_arg = flip_arg,
        .index = index,
        .eop = is_eop,
    });
}

void VideoOutDriver::PresentThread(std::stop_token token) {
    const std::chrono::nanoseconds vblank_period(1000000000 /
                                                 EmulatorSettings.GetVblankFrequency());

    Common::SetCurrentThreadName("shadPS4:PresentThread");
    Common::SetCurrentThreadRealtime(vblank_period);

    Common::AccurateTimer timer{vblank_period};

    const auto receive_request = [this] -> Request {
        std::scoped_lock lk{mutex};
        if (!requests.empty()) {
            const auto request = requests.front();
            requests.pop();
            return request;
        }
        return {};
    };

    while (!token.stop_requested()) {
        timer.Start();
        CheckBootWatchdog();

        if (DebugState.IsGuestThreadsPaused()) {
            DrawLastFrame();
            timer.End();
            continue;
        }

        // Check if it's time to take a request.
        auto& vblank_status = main_port.vblank_status;
        if (vblank_status.count % (main_port.flip_rate + 1) == 0) {
            const auto request = receive_request();
            if (!request) {
                if (timer.GetTotalWait().count() < 0) { // Dont draw too fast
                    if (!main_port.is_open) {
                        DrawBlankFrame();
                    } else if (ImGui::Core::MustKeepDrawing()) {
                        DrawLastFrame();
                    }
                }
            } else {
                Flip(request);
                FRAME_END;
            }
        }

        {
            std::vector<Kernel::OrbisKernelEqueue> vblank_events;
            {
                std::scoped_lock lock{main_port.port_mutex};
                vblank_events = main_port.vblank_events;
            }

            // Needs lock here as can be concurrently read by `sceVideoOutGetVblankStatus`
            std::scoped_lock lock{main_port.vo_mutex};

            // Trigger flip events for the port
            for (auto event : vblank_events) {
                auto* equeue = Kernel::GetEqueue(event);
                if (equeue != nullptr) {
                    equeue->TriggerEvent(
                        static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                        Kernel::OrbisKernelEvent::Filter::VideoOut,
                        reinterpret_cast<void*>(
                            static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) |
                            (vblank_status.count << 16)));
                }
            }

            // Update vblank status
            vblank_status.count++;
            vblank_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
            vblank_status.tsc = Libraries::Kernel::sceKernelReadTsc();
            main_port.vblank_cv.notify_all();
        }

        timer.End();
    }
}

} // namespace Libraries::VideoOut
