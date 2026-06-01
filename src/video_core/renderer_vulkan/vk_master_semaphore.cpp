// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include "video_core/host_diagnostics.h"
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
}

MasterSemaphore::~MasterSemaphore() = default;

void MasterSemaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
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
        LogGpuWaitFailure("master_semaphore", result);
        break;
    }
    Refresh();
}

} // namespace Vulkan
