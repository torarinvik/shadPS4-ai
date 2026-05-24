// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <limits>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "libc_internal_str.h"

namespace Libraries::LibcInternal {

static size_t BoundedStringLength(const char* str, size_t max_len) {
    size_t len = 0;
    while (len < max_len && str[len] != '\0') {
        ++len;
    }
    return len;
}

static s32 CopyStringChecked(char* dest, size_t dest_size, const char* src, size_t count) {
    if (dest == nullptr || dest_size == 0 || src == nullptr) {
        return ORBIS_FAIL;
    }

    const size_t src_len = std::strlen(src);
    const size_t copy_len = std::min(src_len, count);
    if (copy_len >= dest_size) {
        dest[0] = '\0';
        return ORBIS_FAIL;
    }

    std::memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI internal_strcpy_s(char* dest, size_t dest_size, const char* src) {
#ifdef _WIN64
    return strcpy_s(dest, dest_size, src);
#else
    return CopyStringChecked(dest, dest_size, src, std::numeric_limits<size_t>::max());
#endif
}

s32 PS4_SYSV_ABI internal_strcat_s(char* dest, size_t dest_size, const char* src) {
#ifdef _WIN64
    return strcat_s(dest, dest_size, src);
#else
    if (dest == nullptr || dest_size == 0 || src == nullptr) {
        return ORBIS_FAIL;
    }

    const size_t dest_len = BoundedStringLength(dest, dest_size);
    if (dest_len == dest_size) {
        dest[0] = '\0';
        return ORBIS_FAIL;
    }

    const size_t src_len = std::strlen(src);
    if (src_len >= dest_size - dest_len) {
        dest[0] = '\0';
        return ORBIS_FAIL;
    }

    std::memcpy(dest + dest_len, src, src_len + 1);
    return ORBIS_OK;
#endif
}

s32 PS4_SYSV_ABI internal_strcmp(const char* str1, const char* str2) {
    return std::strcmp(str1, str2);
}

s32 PS4_SYSV_ABI internal_strncmp(const char* str1, const char* str2, size_t num) {
    return std::strncmp(str1, str2, num);
}

size_t PS4_SYSV_ABI internal_strlen(const char* str) {
    return std::strlen(str);
}

char* PS4_SYSV_ABI internal_strncpy(char* dest, const char* src, std::size_t count) {
    return std::strncpy(dest, src, count);
}

s32 PS4_SYSV_ABI internal_strncpy_s(char* dest, size_t destsz, const char* src, size_t count) {
#ifdef _WIN64
    return strncpy_s(dest, destsz, src, count);
#else
    return CopyStringChecked(dest, destsz, src, count);
#endif
}

char* PS4_SYSV_ABI internal_strcat(char* dest, const char* src) {
    return std::strcat(dest, src);
}

const char* PS4_SYSV_ABI internal_strchr(const char* str, int c) {
    return std::strchr(str, c);
}

void RegisterlibSceLibcInternalStr(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("5Xa2ACNECdo", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcpy_s);
    LIB_FUNCTION("K+gcnFFJKVc", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcat_s);
    LIB_FUNCTION("Ovb2dSJOAuE", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcmp);
    LIB_FUNCTION("aesyjrHVWy4", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strncmp);
    LIB_FUNCTION("j4ViWNHEgww", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strlen);
    LIB_FUNCTION("6sJWiWSRuqk", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strncpy);
    LIB_FUNCTION("YNzNkJzYqEg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strncpy_s);
    LIB_FUNCTION("Ls4tzzhimqQ", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcat);
    LIB_FUNCTION("ob5xAW4ln-0", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strchr);
}

} // namespace Libraries::LibcInternal
