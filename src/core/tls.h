// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include "common/types.h"
#ifdef _WIN32
#include <malloc.h>
#endif

namespace Xbyak {
class CodeGenerator;
}

namespace Libraries::Fiber {
struct OrbisFiberContext;
}

namespace Core {

union DtvEntry {
    std::size_t counter;
    u8* pointer;
};

struct Tcb {
    Tcb* tcb_self;
    DtvEntry* tcb_dtv;
    void* tcb_thread;
    ::Libraries::Fiber::OrbisFiberContext* tcb_fiber;
};

#ifdef _WIN32
/// Gets the thread local storage key for the TCB block.
u32 GetTcbKey();
#endif

/// Sets the data pointer to the TCB block.
void SetTcbBase(void* image_address);

/// Retrieves Tcb structure for the calling thread.
Tcb* GetTcbBase();

/// Makes sure TLS is initialized for the thread before entering guest.
void InitializeTLS();

void RegisterHostCallName(u64 wrapper, std::string_view name, std::string_view nid = {},
                          std::string_view library = {}, std::string_view module = {});
void TraceHostCall(u64 wrapper, const std::array<u64, 3>& args = {}, u64 return_address = 0);
std::vector<std::string> GetRecentHostCalls();

template <class T>
static inline u64 TraceHostCallArg(T value) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_pointer_v<U>) {
        return reinterpret_cast<u64>(value);
    } else if constexpr (std::is_integral_v<U> || std::is_enum_v<U>) {
        return static_cast<u64>(value);
    } else {
        return 0;
    }
}

template <class... Args>
static inline std::array<u64, 3> TraceHostCallArgs(Args... args) {
    std::array<u64, 3> out{};
    std::size_t index = 0;
    ((index < out.size() ? (out[index++] = TraceHostCallArg(args), 0) : 0), ...);
    return out;
}

static inline u64 TraceHostCallReturnAddress() {
#if defined(__GNUC__) || defined(__clang__)
    return reinterpret_cast<u64>(__builtin_return_address(0));
#else
    return 0;
#endif
}

template <class F, F f>
struct HostCallWrapperImpl;

template <class ReturnType, class... Args, PS4_SYSV_ABI ReturnType (*func)(Args...)>
struct HostCallWrapperImpl<PS4_SYSV_ABI ReturnType (*)(Args...), func> {
    static ReturnType PS4_SYSV_ABI wrap(Args... args) {
        TraceHostCall(reinterpret_cast<u64>(&wrap), TraceHostCallArgs(args...),
                      TraceHostCallReturnAddress());
        return func(args...);
    }
};

#define HOST_CALL(func) (Core::HostCallWrapperImpl<decltype(&(func)), func>::wrap)

} // namespace Core
