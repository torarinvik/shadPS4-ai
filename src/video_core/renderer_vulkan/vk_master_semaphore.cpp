// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <limits>
#include "video_core/host_diagnostics.h"
#include "video_core/renderer_vulkan/vk_gpu_command_diagnostics.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"

#include "common/assert.h"

namespace Vulkan {

MasterSemaphore::MasterSemaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
    watchdog = std::jthread([this](std::stop_token stop) { WatchdogLoop(stop); });
}

MasterSemaphore::~MasterSemaphore() = default;

void MasterSemaphore::WatchdogLoop(std::stop_token stop) {
    const u64 timeout_sec = [] {
        if (const char* v = std::getenv("SHADPS4_GPU_HANG_TIMEOUT")) {
            return static_cast<u64>(std::atoll(v));
        }
        return u64{10};
    }();
    if (timeout_sec == 0) {
        return;
    }
    constexpr auto poll = std::chrono::milliseconds(500);
    u64 last_counter = 0;
    auto last_progress = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(poll);
        auto [result, counter] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        if (result != vk::Result::eSuccess) {
            // Device loss is handled (and dumped) by Refresh on the GPU thread.
            continue;
        }
        const u64 submitted = current_tick.load(std::memory_order_acquire) - 1;
        const auto now = std::chrono::steady_clock::now();
        if (counter != last_counter || counter >= submitted) {
            last_counter = counter;
            last_progress = now;
            continue;
        }
        if (now - last_progress < std::chrono::seconds(timeout_sec)) {
            continue;
        }
        DumpGpuCommandDiagnostics("gpu_progress_watchdog");
        LOG_CRITICAL(Render_Vulkan,
                     "GPU made no progress for {}s (counter={} submitted={}); exiting before the "
                     "hang wedges the host GPU",
                     timeout_sec, counter, submitted);
        Common::Log::Flush();
        std::_Exit(72);
    }
}

void MasterSemaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        if (counter_result == vk::Result::eErrorDeviceLost) {
            // First observation of the loss in our runs lands here, not in the swapchain path;
            // dump the GPU-op ring once so the frees/copies racing the failing command buffer
            // are recorded. Then exit immediately: continuing to submit against a lost Metal
            // device escalates into a system-wide GPU hang/freeze on this machine.
            static std::once_flag dump_once;
            std::call_once(dump_once, [] {
                DumpGpuCommandDiagnostics("master_semaphore_device_loss");
                LOG_CRITICAL(Render_Vulkan, "Device lost; exiting to avoid wedging the host GPU");
                Common::Log::Flush();
                std::_Exit(70);
            });
        } else if (counter_result != vk::Result::eSuccess) {
            RecordGpuCommandDiagnostic(
                "master_semaphore_refresh_failed result=%s known_gpu_tick=%llu current_tick=%llu "
                "last_loaded_gpu_tick=%llu",
                vk::to_string(counter_result).c_str(),
                static_cast<unsigned long long>(KnownGpuTick()),
                static_cast<unsigned long long>(CurrentTick()),
                static_cast<unsigned long long>(this_tick));
            DumpGpuCommandDiagnostics("master_semaphore_refresh_failed");
        }
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));

    // Invariant (#10): the GPU can only complete ticks that were actually handed out, so the known
    // GPU tick must never exceed the logical current tick. A violation means timeline-semaphore
    // accounting corruption (or a stray signal), which precedes a device loss - surface it.
    const u64 logical = current_tick.load(std::memory_order_acquire);
    if (counter > logical) {
        VideoCore::Diag::ReportOnce(
            "mastersem:gpu_ahead",
            fmt::format("[MasterSemaphore] known GPU tick {} exceeds logical current_tick {} "
                        "(timeline accounting corruption)",
                        counter, logical));
        RecordGpuCommandDiagnostic("master_semaphore_gpu_ahead counter=%llu logical=%llu",
                                   static_cast<unsigned long long>(counter),
                                   static_cast<unsigned long long>(logical));
    }
}

void MasterSemaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    const u64 timeout = GpuWaitTimeoutNs();
    u32 timeout_count = 0;
    while (true) {
        const auto result = instance.GetDevice().waitSemaphores(&wait_info, timeout);
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            ++timeout_count;
            RecordGpuCommandDiagnostic(
                "master_semaphore_wait_timeout count=%u target_tick=%llu known_gpu_tick=%llu "
                "current_tick=%llu timeout_ns=%llu",
                timeout_count, static_cast<unsigned long long>(tick),
                static_cast<unsigned long long>(KnownGpuTick()),
                static_cast<unsigned long long>(CurrentTick()),
                static_cast<unsigned long long>(timeout));
            LogGpuWaitTimeout("master_semaphore", timeout);
            LOG_ERROR(Render_Vulkan,
                      "GPU timeline semaphore timeout: target_tick={} known_gpu_tick={} current_tick={}",
                      tick, KnownGpuTick(), CurrentTick());
            if (AbortGpuWaitIfRetryLimitReached("master_semaphore", timeout_count)) {
                return;
            }
            continue;
        }
        LOG_ERROR(Render_Vulkan,
                  "GPU timeline semaphore wait failed: target_tick={} known_gpu_tick={} "
                  "current_tick={} (compare target_tick against FREE reg_tick entries in the dump)",
                  tick, KnownGpuTick(), CurrentTick());
        RecordGpuCommandDiagnostic(
            "master_semaphore_wait_failed result=%s target_tick=%llu known_gpu_tick=%llu "
            "current_tick=%llu",
            vk::to_string(result).c_str(), static_cast<unsigned long long>(tick),
            static_cast<unsigned long long>(KnownGpuTick()),
            static_cast<unsigned long long>(CurrentTick()));
        LogGpuWaitFailure("master_semaphore", result);
        DumpGpuCommandDiagnostics("master_semaphore_wait_failed");
        break;
    }
    Refresh();
}

} // namespace Vulkan
