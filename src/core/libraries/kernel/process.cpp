// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/libs.h"
#include "core/linker.h"

#include <cstring>

namespace Libraries::Kernel {

struct OrbisTimeval {
    s64 tv_sec;
    s64 tv_usec;
};
static_assert(sizeof(OrbisTimeval) == 0x10);

struct OrbisRusage {
    OrbisTimeval ru_utime;
    OrbisTimeval ru_stime;
    s64 ru_maxrss;
    s64 ru_ixrss;
    s64 ru_idrss;
    s64 ru_isrss;
    s64 ru_minflt;
    s64 ru_majflt;
    s64 ru_nswap;
    s64 ru_inblock;
    s64 ru_oublock;
    s64 ru_msgsnd;
    s64 ru_msgrcv;
    s64 ru_nsignals;
    s64 ru_nvcsw;
    s64 ru_nivcsw;
};
static_assert(sizeof(OrbisRusage) == 0x90);

s32 PS4_SYSV_ABI sceKernelIsInSandbox() {
    return 1;
}

s32 PS4_SYSV_ABI sceKernelIsNeoMode() {
    static s32 IsNeoMode = -1;
    if (IsNeoMode == -1) {
        IsNeoMode = EmulatorSettings.IsNeo() &&
                    Common::ElfInfo::Instance().GetPSFAttributes().support_neo_mode;
    }
    return IsNeoMode;
}

s32 PS4_SYSV_ABI sceKernelHasNeoMode() {
    return EmulatorSettings.IsNeo();
}

s32 PS4_SYSV_ABI sceKernelGetMainSocId() {
    // These hardcoded values are based on hardware observations.
    // Different models of PS4/PS4 Pro likely return slightly different values.
    LOG_DEBUG(Lib_Kernel, "called");
    if (EmulatorSettings.IsNeo()) {
        return 0x740f30;
    }
    return 0x710f10;
}

s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    *ver = Common::ElfInfo::Instance().CompiledSdkVer();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetCpumode() {
    LOG_DEBUG(Lib_Kernel, "called");
    auto& attrs = Common::ElfInfo::Instance().GetPSFAttributes();
    u32 is_cpu6 = attrs.six_cpu_mode.Value();
    u32 is_cpu7 = attrs.seven_cpu_mode.Value();
    if (is_cpu6 == 1 && is_cpu7 == 1) {
        return 2;
    }
    if (is_cpu7 == 1) {
        return 5;
    }
    return 0;
}

s32 PS4_SYSV_ABI sceKernelGetCurrentCpu() {
    LOG_DEBUG(Lib_Kernel, "called");
    return 0;
}

void* PS4_SYSV_ABI sceKernelGetProcParam() {
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    return linker->GetProcParam();
}

s32 PS4_SYSV_ABI sceKernelLoadStartModule(const char* moduleFileName, u64 args, const void* argp,
                                          u32 flags, const void* pOpt, s32* pRes) {
    if (moduleFileName == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    LOG_INFO(Lib_Kernel, "called filename = {}, args = {}", moduleFileName, args);
    if (flags != 0) {
        LOG_WARNING(Lib_Kernel, "Unsupported sceKernelLoadStartModule flags {:#x}", flags);
    }

    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    auto* linker = Common::Singleton<Core::Linker>::Instance();

    std::filesystem::path path;
    std::string guest_path(moduleFileName);
    if (guest_path.empty()) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    s32 handle = -1;

    if (guest_path[0] == '/') {
        // try load /system/common/lib/ +path
        // try load /system/priv/lib/   +path
        path = mnt->GetHostPath(guest_path);
        handle = linker->LoadAndStartModule(path, args, argp, pRes);
        if (handle != -1)
            return handle;
    } else {
        if (!guest_path.contains('/')) {
            path = mnt->GetHostPath("/app0/" + guest_path);
            handle = linker->LoadAndStartModule(path, args, argp, pRes);
            if (handle != -1)
                return handle;
            // if ((flags & 0x10000) != 0)
            //  try load /system/priv/lib/   +basename
            //  try load /system/common/lib/ +basename
        } else {
            path = mnt->GetHostPath(guest_path);
            handle = linker->LoadAndStartModule(path, args, argp, pRes);
            if (handle != -1)
                return handle;
        }
    }

    return ORBIS_KERNEL_ERROR_ENOENT;
}

s32 PS4_SYSV_ABI sceKernelStopUnloadModule(s32 handle, u64 args, const void* argp, u32 flags,
                                           const void* pOpt, s32* pRes) {
    LOG_INFO(Lib_Kernel, "(STUBBED) handle = {}, args = {}, flags = {:#x}", handle, args, flags);
    if (pRes != nullptr) {
        *pRes = ORBIS_OK;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelDlsym(s32 handle, const char* symbol, void** addrp) {
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* module = linker->GetModule(handle);
    if (module == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    *addrp = module->FindByName(symbol);
    if (*addrp == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetModuleInfoForUnwind(VAddr addr, s32 flags,
                                                 OrbisModuleInfoForUnwind* info) {
    if (flags >= 3) {
        std::memset(info, 0, sizeof(OrbisModuleInfoForUnwind));
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (!info) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (info->st_size < sizeof(OrbisModuleInfoForUnwind)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    // Find module that contains specified address.
    LOG_INFO(Lib_Kernel, "called addr = {:#x}, flags = {:#x}", addr, flags);
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* module = linker->FindByAddress(addr);
    if (!module) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    const auto mod_info = module->GetModuleInfoEx();

    // Fill in module info.
    std::memset(info, 0, sizeof(OrbisModuleInfoForUnwind));
    info->name = mod_info.name;
    info->eh_frame_hdr_addr = mod_info.eh_frame_hdr_addr;
    info->eh_frame_addr = mod_info.eh_frame_addr;
    info->eh_frame_size = mod_info.eh_frame_size;
    info->seg0_addr = mod_info.segments[0].address;
    info->seg0_size = mod_info.segments[0].size;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetModuleInfoFromAddr(VAddr addr, s32 flags,
                                                Core::OrbisKernelModuleInfoEx* info) {
    if (flags >= 3) {
        std::memset(info, 0, sizeof(Core::OrbisKernelModuleInfoEx));
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (info == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    LOG_INFO(Lib_Kernel, "called addr = {:#x}, flags = {:#x}", addr, flags);
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* module = linker->FindByAddress(addr);
    if (!module) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }

    *info = module->GetModuleInfoEx();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetModuleInfo(s32 handle, Core::OrbisKernelModuleInfo* info) {
    if (info == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (info->st_size != sizeof(Core::OrbisKernelModuleInfo)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* module = linker->GetModule(handle);
    if (module == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    *info = module->GetModuleInfo();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetModuleInfo2(s32 handle, Core::OrbisKernelModuleInfo* info) {
    if (info == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (info->st_size != sizeof(Core::OrbisKernelModuleInfo)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* module = linker->GetModule(handle);
    if (module == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    if (module->IsSystemLib()) {
        return ORBIS_KERNEL_ERROR_EPERM;
    }
    *info = module->GetModuleInfo();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetModuleInfoInternal(s32 handle, Core::OrbisKernelModuleInfoEx* info) {
    if (info == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (info->st_size != sizeof(Core::OrbisKernelModuleInfoEx)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* module = linker->GetModule(handle);
    if (module == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    *info = module->GetModuleInfoEx();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetModuleList(s32* handles, u64 num_array, u64* out_count) {
    if (handles == nullptr || out_count == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    auto* linker = Common::Singleton<Core::Linker>::Instance();
    u64 count = 0;
    auto* module = linker->GetModule(count);
    while (module != nullptr && count < num_array) {
        handles[count] = count;
        count++;
        module = linker->GetModule(count);
    }

    if (count == num_array && module != nullptr) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }

    *out_count = count;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetModuleList2(s32* handles, u64 num_array, u64* out_count) {
    if (handles == nullptr || out_count == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    auto* linker = Common::Singleton<Core::Linker>::Instance();
    u64 id = 0;
    u64 index = 0;
    auto* module = linker->GetModule(id);
    while (module != nullptr && index < num_array) {
        if (!module->IsSystemLib()) {
            handles[index++] = id;
        }
        id++;
        module = linker->GetModule(id);
    }

    if (index == num_array && module != nullptr) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }

    *out_count = index;
    return ORBIS_OK;
}

u32 PS4_SYSV_ABI posix_getuid() {
    return 1;
}

s32 PS4_SYSV_ABI posix_getrusage(s32 who, void* usage) {
    LOG_DEBUG(Lib_Kernel, "(PARTIAL) who = {}", who);
    if (usage == nullptr) {
        return -1;
    }
    // PS4 uses the FreeBSD-style rusage layout. Writing a larger host/debug buffer here can
    // overwrite guest stack canaries when titles pass a stack-allocated rusage.
    std::memset(usage, 0, sizeof(OrbisRusage));
    return 0;
}

using SignalHandler = void (*)(s32);

SignalHandler PS4_SYSV_ABI posix_signal(s32 signum, SignalHandler handler) {
    LOG_DEBUG(Lib_Kernel, "(PARTIAL) signum = {}, handler = {}", signum, fmt::ptr(handler));
    return nullptr;
}

s32 PS4_SYSV_ABI posix_sysctl(const s32* name, u32 namelen, void* oldp, u64* oldlenp,
                              const void* newp, u64 newlen) {
    LOG_DEBUG(Lib_Kernel, "(STUBBED) namelen = {}, oldp = {}, newp = {}, newlen = {}", namelen,
              fmt::ptr(oldp), fmt::ptr(newp), newlen);
    if (oldlenp != nullptr) {
        *oldlenp = 0;
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_sigreturn(void* context) {
    LOG_DEBUG(Lib_Kernel, "(STUBBED) context = {}", fmt::ptr(context));
    return 0;
}

s32 PS4_SYSV_ABI elf_phdr_match_addr(void* info, void* addr) {
    LOG_DEBUG(Lib_Kernel, "(STUBBED) info = {}, addr = {}", fmt::ptr(info), fmt::ptr(addr));
    return 0;
}

u64 PS4_SYSV_ABI sceKernelPrintBacktraceWithModuleInfo(u64, u64, u64, u64, u64, u64) {
    LOG_DEBUG(Lib_Kernel, "(STUBBED)");
    return 0;
}

s32 PS4_SYSV_ABI is_signal_return(u64, u64, u64, u64, u64, u64) {
    LOG_DEBUG(Lib_Kernel, "(STUBBED)");
    return 0;
}

void PS4_SYSV_ABI pthread_cxa_finalize(void* dso_handle) {
    LOG_TRACE(Lib_Kernel, "__pthread_cxa_finalize({}) ignored", fmt::ptr(dso_handle));
}

s32 PS4_SYSV_ABI scePthreadSetcancelstate(s32 state, s32* oldstate) {
    LOG_DEBUG(Lib_Kernel, "(STUBBED) state = {}", state);
    if (oldstate != nullptr) {
        *oldstate = 0;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI exit(s32 status) {
    UNREACHABLE_MSG("Exiting with status code {}", status);
    return 0;
}

void RegisterProcess(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("xeu-pV8wkKs", "libkernel", 1, "libkernel", sceKernelIsInSandbox);
    LIB_FUNCTION("WB66evu8bsU", "libkernel", 1, "libkernel", sceKernelGetCompiledSdkVersion);
    LIB_FUNCTION("WslcK1FQcGI", "libkernel", 1, "libkernel", sceKernelIsNeoMode);
    LIB_FUNCTION("rNRtm1uioyY", "libkernel", 1, "libkernel", sceKernelHasNeoMode);
    LIB_FUNCTION("0vTn5IDMU9A", "libkernel", 1, "libkernel", sceKernelGetMainSocId);
    LIB_FUNCTION("VOx8NGmHXTs", "libkernel", 1, "libkernel", sceKernelGetCpumode);
    LIB_FUNCTION("g0VTBxfJyu0", "libkernel", 1, "libkernel", sceKernelGetCurrentCpu);
    LIB_FUNCTION("959qrazPIrg", "libkernel", 1, "libkernel", sceKernelGetProcParam);
    LIB_FUNCTION("wzvqT4UqKX8", "libkernel", 1, "libkernel", sceKernelLoadStartModule);
    LIB_FUNCTION("QKd0qM58Qes", "libkernel", 1, "libkernel", sceKernelStopUnloadModule);
    LIB_FUNCTION("LwG8g3niqwA", "libkernel", 1, "libkernel", sceKernelDlsym);
    LIB_FUNCTION("RpQJJVKTiFM", "libkernel", 1, "libkernel", sceKernelGetModuleInfoForUnwind);
    LIB_FUNCTION("f7KBOafysXo", "libkernel", 1, "libkernel", sceKernelGetModuleInfoFromAddr);
    LIB_FUNCTION("kUpgrXIrz7Q", "libkernel", 1, "libkernel", sceKernelGetModuleInfo);
    LIB_FUNCTION("QgsKEUfkqMA", "libkernel", 1, "libkernel", sceKernelGetModuleInfo2);
    LIB_FUNCTION("QgsKEUfkqMA", "libkernel_module_info", 1, "libkernel", sceKernelGetModuleInfo2);
    LIB_FUNCTION("HZO7xOos4xc", "libkernel", 1, "libkernel", sceKernelGetModuleInfoInternal);
    LIB_FUNCTION("IuxnUuXk6Bg", "libkernel", 1, "libkernel", sceKernelGetModuleList);
    LIB_FUNCTION("ZzzC3ZGVAkc", "libkernel", 1, "libkernel", sceKernelGetModuleList2);
    LIB_FUNCTION("kg4x8Prhfxw", "libkernel", 1, "libkernel", posix_getuid);
    LIB_FUNCTION("hHlZQUnlxSM", "libkernel", 1, "libkernel", posix_getrusage);
    LIB_FUNCTION("VADc3MNQ3cM", "libkernel", 1, "libkernel", posix_signal);
    LIB_FUNCTION("DFmMT80xcNI", "libkernel", 1, "libkernel", posix_sysctl);
    LIB_FUNCTION("mo0bFmWppIw", "libkernel", 1, "libkernel", posix_sigreturn);
    LIB_FUNCTION("Fjc4-n1+y2g", "libkernel", 1, "libkernel", elf_phdr_match_addr);
    LIB_FUNCTION("Wl2o5hOVZdw", "libkernel", 1, "libkernel",
                 sceKernelPrintBacktraceWithModuleInfo);
    LIB_FUNCTION("crb5j7mkk1c", "libkernel", 1, "libkernel", is_signal_return);
    LIB_FUNCTION("kbw4UHHSYy0", "libkernel", 1, "libkernel", pthread_cxa_finalize);
    LIB_FUNCTION("OAmWq+OHSjw", "libkernel", 1, "libkernel", scePthreadSetcancelstate);
    LIB_FUNCTION("6Z83sYWFlA8", "libkernel", 1, "libkernel", exit);
}

} // namespace Libraries::Kernel
