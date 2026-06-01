// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "common/logging/log.h"
#include "common/types.h"
#include "video_core/host_diagnostics.h"

namespace Vulkan {

struct GpuCommandDiagnosticEntry {
    u64 serial{};
    char text[256]{};
};

inline bool IsGpuCommandDiagnosticsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_TRACE_GPU_COMMANDS");
        if (value != nullptr) {
            // Explicit override (set/clear) takes precedence over the validation-mode default.
            return value[0] != '\0' && std::strcmp(value, "0") != 0;
        }
        // Otherwise record the ring whenever GPU validation is active (warn is the default), so a
        // device-loss dump is populated without needing a separate env. SHADPS4_GPU_VALIDATION=off
        // disables both validation and this ring.
        return VideoCore::Diag::ValidationMode() != VideoCore::Diag::Mode::Off;
    }();
    return enabled;
}

inline std::array<GpuCommandDiagnosticEntry, 64>& GpuCommandDiagnosticRing() {
    static std::array<GpuCommandDiagnosticEntry, 64> ring{};
    return ring;
}

inline std::atomic<u64>& GpuCommandDiagnosticSerial() {
    static std::atomic<u64> serial{0};
    return serial;
}

inline std::mutex& GpuCommandDiagnosticMutex() {
    static std::mutex mutex;
    return mutex;
}

inline void RecordGpuCommandDiagnostic(const char* format, ...) {
    if (!IsGpuCommandDiagnosticsEnabled()) {
        return;
    }

    char text[256]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    const u64 serial = GpuCommandDiagnosticSerial().fetch_add(1, std::memory_order_relaxed) + 1;
    auto& ring = GpuCommandDiagnosticRing();
    const auto slot = static_cast<std::size_t>((serial - 1) % ring.size());

    std::scoped_lock lock{GpuCommandDiagnosticMutex()};
    ring[slot].serial = serial;
    std::snprintf(ring[slot].text, sizeof(ring[slot].text), "%s", text);
}

inline void DumpGpuCommandDiagnostics(const char* reason) {
    if (!IsGpuCommandDiagnosticsEnabled()) {
        return;
    }

    std::scoped_lock lock{GpuCommandDiagnosticMutex()};
    const auto& ring = GpuCommandDiagnosticRing();
    u64 latest = 0;
    for (const auto& entry : ring) {
        latest = std::max(latest, entry.serial);
    }
    if (latest == 0) {
        LOG_ERROR(Render_Vulkan, "GPU command diagnostics: reason={} no commands recorded", reason);
        return;
    }

    const u64 first = latest > ring.size() ? latest - ring.size() + 1 : 1;
    LOG_ERROR(Render_Vulkan, "GPU command diagnostics: reason={} showing serials {}..{}", reason,
              first, latest);
    for (u64 serial = first; serial <= latest; ++serial) {
        const auto slot = static_cast<std::size_t>((serial - 1) % ring.size());
        const auto& entry = ring[slot];
        if (entry.serial == serial) {
            LOG_ERROR(Render_Vulkan, "GPU command [{}] {}", serial, entry.text);
        }
    }
}

} // namespace Vulkan
