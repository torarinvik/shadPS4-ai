// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ctime>

#include "common/logging/log.h"
#include "core/libraries/libs.h"
#include "libc_internal_time.h"

namespace Libraries::LibcInternal {

static thread_local std::tm g_local_time{};
static thread_local std::tm g_gm_time{};
static int g_need_sce_libc_internal{};

static PS4_SYSV_ABI time_t internal_time(time_t* out) {
    const time_t now = std::time(nullptr);
    if (out) {
        *out = now;
    }
    return now;
}

static PS4_SYSV_ABI time_t internal_mktime(std::tm* timeptr) {
    if (!timeptr) {
        return static_cast<time_t>(-1);
    }
    return std::mktime(timeptr);
}

static bool CopyLocalTime(const time_t* timer, std::tm* out) {
    if (!timer || !out) {
        return false;
    }
#ifdef _WIN32
    return localtime_s(out, timer) == 0;
#else
    return localtime_r(timer, out) != nullptr;
#endif
}

static bool CopyGmTime(const time_t* timer, std::tm* out) {
    if (!timer || !out) {
        return false;
    }
#ifdef _WIN32
    return gmtime_s(out, timer) == 0;
#else
    return gmtime_r(timer, out) != nullptr;
#endif
}

static PS4_SYSV_ABI std::tm* internal_localtime(const time_t* timer) {
    if (!CopyLocalTime(timer, &g_local_time)) {
        return nullptr;
    }
    return &g_local_time;
}

static PS4_SYSV_ABI std::tm* internal_gmtime(const time_t* timer) {
    if (!CopyGmTime(timer, &g_gm_time)) {
        return nullptr;
    }
    return &g_gm_time;
}

static PS4_SYSV_ABI std::tm* internal_gmtime_s(const time_t* timer, std::tm* result) {
    if (!CopyGmTime(timer, result)) {
        return nullptr;
    }
    return result;
}

static PS4_SYSV_ABI void internal___cxa_finalize(void* dso_handle) {
    LOG_TRACE(Lib_LibcInternal, "__cxa_finalize({}) ignored", fmt::ptr(dso_handle));
}

static PS4_SYSV_ABI int internal_need_sce_libc_internal() {
    return 0;
}

void RegisterlibSceLibcInternalTime(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("wLlFkwG9UcQ", "libSceLibcInternal", 1, "libSceLibcInternal", internal_time);
    LIB_FUNCTION("n7AepwR0s34", "libSceLibcInternal", 1, "libSceLibcInternal", internal_mktime);
    LIB_FUNCTION("efhK-YSUYYQ", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_localtime);
    LIB_FUNCTION("1mecP7RgI2A", "libSceLibcInternal", 1, "libSceLibcInternal", internal_gmtime);
    LIB_FUNCTION("5bBacGLyLOs", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_gmtime_s);
    LIB_FUNCTION("H2e8t5ScQGc", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal___cxa_finalize);
    LIB_FUNCTION("ZT4ODD2Ts9o", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_need_sce_libc_internal);
    LIB_OBJ("ZT4ODD2Ts9o", "libSceLibcInternal", 1, "libSceLibcInternal",
            &g_need_sce_libc_internal);
}

} // namespace Libraries::LibcInternal
