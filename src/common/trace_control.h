// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "common/types.h"

namespace Common::Trace {

inline std::atomic_bool aggressive_logging{false};
inline std::once_flag aggressive_logging_init_flag;
inline std::atomic_bool black_screen_watchdog_armed{false};
inline std::once_flag black_screen_watchdog_arm_init_flag;
inline std::atomic_bool audio_initialized{false};
inline std::atomic<const char*> audio_init_source{nullptr};

struct VideoOutWriteTrace {
    const char* op{};
    u64 address{};
    u64 size{};
    u64 detail0{};
    u64 detail1{};
    u64 sequence{};
};

struct VideoOutRange {
    u64 address{};
    u64 size{};
    VideoOutWriteTrace last_write{};
};

inline std::mutex videoout_trace_mutex;
inline std::array<VideoOutRange, 16> videoout_ranges{};
inline std::atomic_uint64_t videoout_write_sequence{0};

inline bool EnvEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

inline void EnsureAggressiveLoggingInitialized() {
    std::call_once(aggressive_logging_init_flag, [] {
        aggressive_logging.store(EnvEnabled("SHADPS4_TRACE_RENDER"), std::memory_order_relaxed);
    });
}

inline bool IsAggressiveLoggingEnabled() {
    EnsureAggressiveLoggingInitialized();
    return aggressive_logging.load(std::memory_order_relaxed);
}

inline bool SetAggressiveLoggingEnabled(bool enabled) {
    EnsureAggressiveLoggingInitialized();
    aggressive_logging.store(enabled, std::memory_order_relaxed);
    return enabled;
}

inline bool ToggleAggressiveLogging() {
    EnsureAggressiveLoggingInitialized();
    const bool enabled = !aggressive_logging.load(std::memory_order_relaxed);
    aggressive_logging.store(enabled, std::memory_order_relaxed);
    return enabled;
}

inline void EnsureBlackScreenWatchdogArmInitialized() {
    std::call_once(black_screen_watchdog_arm_init_flag, [] {
        black_screen_watchdog_armed.store(EnvEnabled("SHADPS4_BLACK_WATCHDOG_ARMED"),
                                          std::memory_order_relaxed);
    });
}

inline bool IsBlackScreenWatchdogArmed() {
    EnsureBlackScreenWatchdogArmInitialized();
    return black_screen_watchdog_armed.load(std::memory_order_relaxed);
}

inline bool SetBlackScreenWatchdogArmed(bool armed) {
    EnsureBlackScreenWatchdogArmInitialized();
    black_screen_watchdog_armed.store(armed, std::memory_order_relaxed);
    return armed;
}

inline void MarkAudioInitialized(const char* source) {
    audio_init_source.store(source, std::memory_order_relaxed);
    audio_initialized.store(true, std::memory_order_relaxed);
}

inline bool IsAudioInitialized() {
    return audio_initialized.load(std::memory_order_relaxed);
}

inline const char* GetAudioInitSource() {
    const char* source = audio_init_source.load(std::memory_order_relaxed);
    return source != nullptr ? source : "none";
}

inline bool RangesOverlap(u64 lhs_address, u64 lhs_size, u64 rhs_address, u64 rhs_size) {
    return lhs_size != 0 && rhs_size != 0 && lhs_address < rhs_address + rhs_size &&
           rhs_address < lhs_address + lhs_size;
}

inline void RegisterVideoOutRange(u64 address, u64 size) {
    if (!EnvEnabled("SHADPS4_STRICT_BLACK_SCREEN_WATCHDOG") || address == 0 || size == 0) {
        return;
    }

    std::scoped_lock lock{videoout_trace_mutex};
    for (auto& range : videoout_ranges) {
        if (range.address == address) {
            range.size = size;
            return;
        }
    }
    for (auto& range : videoout_ranges) {
        if (range.address == 0) {
            range.address = address;
            range.size = size;
            return;
        }
    }
}

inline void RecordVideoOutWrite(const char* op, u64 address, u64 size, u64 detail0 = 0,
                                u64 detail1 = 0) {
    if (!EnvEnabled("SHADPS4_STRICT_BLACK_SCREEN_WATCHDOG") || address == 0 || size == 0) {
        return;
    }

    std::scoped_lock lock{videoout_trace_mutex};
    for (auto& range : videoout_ranges) {
        if (range.address != 0 && RangesOverlap(address, size, range.address, range.size)) {
            range.last_write = VideoOutWriteTrace{
                .op = op,
                .address = address,
                .size = size,
                .detail0 = detail0,
                .detail1 = detail1,
                .sequence = videoout_write_sequence.fetch_add(1, std::memory_order_relaxed) + 1,
            };
        }
    }
}

inline VideoOutWriteTrace GetLastVideoOutWrite(u64 address, u64 size) {
    std::scoped_lock lock{videoout_trace_mutex};
    VideoOutWriteTrace latest{};
    for (const auto& range : videoout_ranges) {
        if (range.address != 0 && RangesOverlap(address, size, range.address, range.size) &&
            range.last_write.sequence > latest.sequence) {
            latest = range.last_write;
        }
    }
    return latest;
}

} // namespace Common::Trace
