// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/assert.h"
#include "common/decoder.h"
#include "common/signal_context.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/signals.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#ifdef ARCH_X86_64
#include <Zydis/Formatter.h>
#endif
#endif

#ifndef _WIN32
namespace Libraries::Kernel {
void SigactionHandler(int native_signum, siginfo_t* inf, ucontext_t* raw_context);
extern std::array<OrbisKernelExceptionHandler, 130> Handlers;
} // namespace Libraries::Kernel
#endif

namespace Core {

#if defined(_WIN32)

static LONG WINAPI SignalHandler(EXCEPTION_POINTERS* pExp) noexcept {
    const auto* signals = Signals::Instance();
    DWORD code = 0;
    PVOID address = nullptr;

    if (pExp != nullptr && pExp->ExceptionRecord != nullptr) {
        code = pExp->ExceptionRecord->ExceptionCode;
        address = pExp->ExceptionRecord->ExceptionAddress;
    }

    bool handled = false;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        handled = signals->DispatchAccessViolation(
            pExp, reinterpret_cast<void*>(pExp->ExceptionRecord->ExceptionInformation[1]));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        handled = signals->DispatchIllegalInstruction(pExp);
        break;
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        // Used by OutputDebugString functions.
        return EXCEPTION_CONTINUE_EXECUTION;
    default:
        break;
    }

    if (handled) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    LOG_CRITICAL(Debug, "Unhandled Exception code {:#x} at {}", code, address);
    Common::Log::Flush();

    return EXCEPTION_CONTINUE_SEARCH;
}

#else

static std::string DisassembleInstruction(void* code_address) {
    char buffer[256] = "<unable to decode>";

#ifdef ARCH_X86_64
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status =
        Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
    if (ZYAN_SUCCESS(status)) {
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible, buffer, sizeof(buffer),
                                        reinterpret_cast<u64>(code_address), ZYAN_NULL);
    }
#endif

    return buffer;
}

static bool TraceSignalFaults() {
    static const bool enabled = std::getenv("SHADPS4_TRACE_SIGNAL_FAULTS") != nullptr;
    return enabled;
}

static bool TraceSignalSymbols() {
    static const bool enabled = std::getenv("SHADPS4_TRACE_SIGNAL_SYMBOLS") != nullptr;
    return enabled;
}

static void TraceSignalFault(int sig, void* raw_context, void* fault_address) {
    if (!TraceSignalFaults()) {
        return;
    }
    auto* code_address = Common::GetRip(raw_context);
    const auto instruction = DisassembleInstruction(code_address);
#ifdef ARCH_X86_64
    const auto& state = reinterpret_cast<ucontext_t*>(raw_context)->uc_mcontext->__ss;
    char buffer[768];
    const auto len = std::snprintf(
        buffer, sizeof(buffer),
        "TRACE_SIGNAL_FAULT sig=%d rip=%p fault=%p is_write=%d inst=\"%s\" "
        "rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx "
        "r13=%#llx r14=%#llx\n",
        sig, code_address, fault_address, Common::IsWriteError(raw_context) ? 1 : 0,
        instruction.c_str(), static_cast<unsigned long long>(state.__rax),
        static_cast<unsigned long long>(state.__rbx),
        static_cast<unsigned long long>(state.__rcx),
        static_cast<unsigned long long>(state.__rdx),
        static_cast<unsigned long long>(state.__rsi),
        static_cast<unsigned long long>(state.__rdi),
        static_cast<unsigned long long>(state.__r13),
        static_cast<unsigned long long>(state.__r14));
#else
    char buffer[256];
    const auto len = std::snprintf(
        buffer, sizeof(buffer),
        "TRACE_SIGNAL_FAULT sig=%d rip=%p fault=%p is_write=%d inst=\"%s\"\n", sig, code_address,
        fault_address, Common::IsWriteError(raw_context) ? 1 : 0, instruction.c_str());
#endif
    if (len > 0) {
        const auto size = static_cast<size_t>(std::min(len, static_cast<int>(sizeof(buffer) - 1)));
        write(STDERR_FILENO, buffer, size);
    }
}

static void TraceSignalSymbol(void* code_address) {
    if (!TraceSignalSymbols()) {
        return;
    }
    Dl_info info{};
    const int ok = dladdr(code_address, &info);
    char buffer[1024];
    const auto len = std::snprintf(
        buffer, sizeof(buffer),
        "TRACE_SIGNAL_SYMBOL rip=%p image=%s image_base=%p symbol=%s symbol_addr=%p\n",
        code_address, ok && info.dli_fname ? info.dli_fname : "<unknown>",
        ok ? info.dli_fbase : nullptr, ok && info.dli_sname ? info.dli_sname : "<unknown>",
        ok ? info.dli_saddr : nullptr);
    if (len > 0) {
        const auto size = static_cast<size_t>(std::min(len, static_cast<int>(sizeof(buffer) - 1)));
        write(STDERR_FILENO, buffer, size);
    }
}

void SignalHandler(int sig, siginfo_t* info, void* raw_context) {
    const auto* signals = Signals::Instance();

    auto* code_address = Common::GetRip(raw_context);

    switch (sig) {
    case SIGSEGV:
    case SIGBUS: {
        TraceSignalFault(sig, raw_context, info->si_addr);
        const bool is_write = Common::IsWriteError(raw_context);
        if (!signals->DispatchAccessViolation(raw_context, info->si_addr)) {
            TraceSignalSymbol(code_address);
            // If the guest has installed a custom signal handler, and the access violation didn't
            // come from HLE memory tracking, pass the signal on
            if (Libraries::Kernel::Handlers[Libraries::Kernel::NativeToOrbisSignal(sig)]) {
                Libraries::Kernel::SigactionHandler(sig, info,
                                                    reinterpret_cast<ucontext_t*>(raw_context));
                return;
            }
            UNREACHABLE_MSG("Unhandled access violation at code address {}: {} address {}",
                            fmt::ptr(code_address), is_write ? "Write to" : "Read from",
                            fmt::ptr(info->si_addr));
        }
        break;
    }
    case SIGILL:
        if (!signals->DispatchIllegalInstruction(raw_context)) {
            if (Libraries::Kernel::Handlers[Libraries::Kernel::NativeToOrbisSignal(sig)]) {
                Libraries::Kernel::SigactionHandler(sig, info,
                                                    reinterpret_cast<ucontext_t*>(raw_context));
                return;
            }
            UNREACHABLE_MSG("Unhandled illegal instruction at code address {}: {}",
                            fmt::ptr(code_address), DisassembleInstruction(code_address));
        }
        break;
    default:
        if (sig == SIGSLEEP) {
            // Sleep thread until signal is received again
            sigset_t sigset;
            sigemptyset(&sigset);
            sigaddset(&sigset, SIGSLEEP);
            sigwait(&sigset, &sig);
        }
        break;
    }
}

#endif

SignalDispatch::SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(handle = AddVectoredExceptionHandler(0, SignalHandler),
               "Failed to register exception handler.");
#else
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(sigaction(SIGSEGV, &action, nullptr) == 0 &&
                   sigaction(SIGBUS, &action, nullptr) == 0,
               "Failed to register access violation signal handler.");
    ASSERT_MSG(sigaction(SIGILL, &action, nullptr) == 0,
               "Failed to register illegal instruction signal handler.");
    ASSERT_MSG(sigaction(SIGSLEEP, &action, nullptr) == 0,
               "Failed to register sleep signal handler.");
#endif
}

SignalDispatch::~SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(RemoveVectoredExceptionHandler(handle), "Failed to remove exception handler.");
#else
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(sigaction(SIGSEGV, &action, nullptr) == 0 &&
                   sigaction(SIGBUS, &action, nullptr) == 0,
               "Failed to remove access violation signal handler.");
    ASSERT_MSG(sigaction(SIGILL, &action, nullptr) == 0,
               "Failed to remove illegal instruction signal handler.");
#endif
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, _] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, _] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
