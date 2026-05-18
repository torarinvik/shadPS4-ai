// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/ult/ult.h"

#include "common/logging/log.h"
#include "core/libraries/libs.h"

namespace Libraries::Ult {

s32 PS4_SYSV_ABI sceUltInitialize() {
    LOG_TRACE(Core, "sceUltInitialize called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceUltFinalize() {
    LOG_TRACE(Core, "sceUltFinalize called");
    return ORBIS_OK;
}

std::size_t PS4_SYSV_ABI sceUltWaitingQueueResourcePoolGetWorkAreaSize() {
    LOG_TRACE(Core, "sceUltWaitingQueueResourcePoolGetWorkAreaSize called");
    return 0;
}

s32 PS4_SYSV_ABI _sceUltWaitingQueueResourcePoolCreate(void* pool, const char* name,
                                                       void* work_area,
                                                       std::size_t work_area_size,
                                                       const void* opt_param) {
    LOG_TRACE(Core,
              "_sceUltWaitingQueueResourcePoolCreate called pool={} name={} work_area={} "
              "work_area_size={} opt_param={}",
              pool, name ? name : "", work_area, work_area_size, opt_param);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceUltWaitingQueueResourcePoolDestroy(void* pool) {
    LOG_TRACE(Core, "sceUltWaitingQueueResourcePoolDestroy called pool={}", pool);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI _sceUltMutexOptParamInitialize(void* opt_param) {
    LOG_TRACE(Core, "_sceUltMutexOptParamInitialize called opt_param={}", opt_param);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI _sceUltMutexCreate(void* mutex, const char* name, void* resource_pool,
                                    const void* opt_param) {
    LOG_TRACE(Core, "_sceUltMutexCreate called mutex={} name={} resource_pool={} opt_param={}",
              mutex, name ? name : "", resource_pool, opt_param);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceUltMutexLock(void* mutex) {
    LOG_TRACE(Core, "sceUltMutexLock called mutex={}", mutex);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceUltMutexTryLock(void* mutex) {
    LOG_TRACE(Core, "sceUltMutexTryLock called mutex={}", mutex);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceUltMutexUnlock(void* mutex) {
    LOG_TRACE(Core, "sceUltMutexUnlock called mutex={}", mutex);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceUltMutexDestroy(void* mutex) {
    LOG_TRACE(Core, "sceUltMutexDestroy called mutex={}", mutex);
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("hZIg1EWGsHM", "libSceUlt", 1, "libSceUlt", sceUltInitialize);
    LIB_FUNCTION("d-kSG2fLrvI", "libSceUlt", 1, "libSceUlt", sceUltFinalize);
    LIB_FUNCTION("WIWV1Qd7PFU", "libSceUlt", 1, "libSceUlt",
                 sceUltWaitingQueueResourcePoolGetWorkAreaSize);
    LIB_FUNCTION("YiHujOG9vXY", "libSceUlt", 1, "libSceUlt",
                 _sceUltWaitingQueueResourcePoolCreate);
    LIB_FUNCTION("or55417wcDk", "libSceUlt", 1, "libSceUlt",
                 sceUltWaitingQueueResourcePoolDestroy);
    LIB_FUNCTION("1+8t9aHLiz8", "libSceUlt", 1, "libSceUlt", _sceUltMutexOptParamInitialize);
    LIB_FUNCTION("mmt8Sa6tL6c", "libSceUlt", 1, "libSceUlt", _sceUltMutexCreate);
    LIB_FUNCTION("8hEGkR1pfr8", "libSceUlt", 1, "libSceUlt", sceUltMutexLock);
    LIB_FUNCTION("jOsUG0BJI-Y", "libSceUlt", 1, "libSceUlt", sceUltMutexTryLock);
    LIB_FUNCTION("h0XebKiMBtk", "libSceUlt", 1, "libSceUlt", sceUltMutexUnlock);
    LIB_FUNCTION("jW+HnafeS3Y", "libSceUlt", 1, "libSceUlt", sceUltMutexDestroy);
}

} // namespace Libraries::Ult
