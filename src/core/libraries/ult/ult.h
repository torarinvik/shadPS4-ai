// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/error_codes.h"
#include "core/loader/symbols_resolver.h"

namespace Libraries::Ult {

s32 PS4_SYSV_ABI sceUltInitialize();
s32 PS4_SYSV_ABI sceUltFinalize();
std::size_t PS4_SYSV_ABI sceUltWaitingQueueResourcePoolGetWorkAreaSize();
s32 PS4_SYSV_ABI _sceUltWaitingQueueResourcePoolCreate(void* pool, const char* name, void* work_area,
                                                       std::size_t work_area_size,
                                                       const void* opt_param);
s32 PS4_SYSV_ABI sceUltWaitingQueueResourcePoolDestroy(void* pool);
s32 PS4_SYSV_ABI _sceUltMutexOptParamInitialize(void* opt_param);
s32 PS4_SYSV_ABI _sceUltMutexCreate(void* mutex, const char* name, void* resource_pool,
                                    const void* opt_param);
s32 PS4_SYSV_ABI sceUltMutexLock(void* mutex);
s32 PS4_SYSV_ABI sceUltMutexTryLock(void* mutex);
s32 PS4_SYSV_ABI sceUltMutexUnlock(void* mutex);
s32 PS4_SYSV_ABI sceUltMutexDestroy(void* mutex);

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Ult
