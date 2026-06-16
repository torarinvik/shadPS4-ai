// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <thread>
#include <queue>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

class MasterSemaphore {
public:
    explicit MasterSemaphore(const Instance& instance_);
    ~MasterSemaphore();

    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current_tick.load(std::memory_order_acquire);
    }

    [[nodiscard]] u64 KnownGpuTick() const noexcept {
        return gpu_tick.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return KnownGpuTick() >= tick;
    }

    [[nodiscard]] u64 NextTick() noexcept {
        return current_tick.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] vk::Semaphore Handle() const noexcept {
        return semaphore.get();
    }

    /// Refresh the known GPU tick
    void Refresh();

    /// Waits for a tick to be hit on the GPU
    void Wait(u64 tick);

protected:
    const Instance& instance;
    vk::UniqueSemaphore semaphore;    ///< Timeline semaphore.
    std::atomic<u64> gpu_tick{0};     ///< Current known GPU tick.
    std::atomic<u64> current_tick{1}; ///< Current logical tick.

private:
    // GPU-progress watchdog: on this machine a wedged Metal command buffer can hang the AGX
    // firmware without ever reporting a device loss, escalating into a whole-macOS freeze that
    // needs a hard reboot. The watchdog polls the timeline semaphore directly and, if ticks are
    // pending but the counter makes no progress for SHADPS4_GPU_HANG_TIMEOUT seconds (default 10,
    // 0 disables), dumps the op ring and hard-exits (code 72) while the hang is young enough for
    // macOS to recover the GPU by reaping the process.
    void WatchdogLoop(std::stop_token stop);
    std::jthread watchdog;
};

} // namespace Vulkan
