// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/std.h>
#include <spdlog/sinks/base_sink.h>

#include "common/assert.h"
#include "common/config.h"
#include "common/logging/log.h"
#include "common/logging/thread_name_formatter.h"
#include "common/thread.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#ifdef _WIN32
#include <Windows.h>
#endif

// return codes above 'standard'
// https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes
enum class ShadPs4ReturnCode : u32 {
    TERMINATE_WITHOUT_EXCEPTION = 20'000,
    TERMINATE_WITH_EXCEPTION = 20'001,
    TERMINATE_WITH_UNKNOWN_EXCEPTION = 20'002,
};

namespace Common::Log {
bool g_should_append = false;

static std::shared_ptr<spdlog_stdout> g_console_sink;
static std::shared_ptr<spdlog::sinks::basic_file_sink_mt> g_shad_file_sink;

static const char* InferTerminateContext(std::string_view thread_name) {
    if (thread_name.find("Ajm") != std::string_view::npos) {
        return "Ajm";
    }
    if (thread_name.find("AvPlayer") != std::string_view::npos ||
        thread_name.find("AvVideo") != std::string_view::npos ||
        thread_name.find("AvAudio") != std::string_view::npos ||
        thread_name.find("AvDemuxer") != std::string_view::npos) {
        return "AvPlayer";
    }
    if (thread_name.find("MoviePlayer") != std::string_view::npos ||
        thread_name.find("MoviePlayer2") != std::string_view::npos) {
        return "MoviePlayer";
    }
    if (thread_name.find("Present") != std::string_view::npos ||
        thread_name.find("draw") != std::string_view::npos ||
        thread_name.find("GPU") != std::string_view::npos) {
        return "GPU";
    }
    return "unknown";
}

std::unordered_map<std::string_view, std::shared_ptr<spdlog::logger>> ALL_LOGGERS{
    {Class::Common, nullptr},
    {Class::Common_Filesystem, nullptr},
    {Class::Common_Memory, nullptr},
    {Class::Config, nullptr},
    {Class::Core, nullptr},
    {Class::Core_Devices, nullptr},
    {Class::Core_Linker, nullptr},
    {Class::Debug, nullptr},
    {Class::Frontend, nullptr},
    {Class::IPC, nullptr},
    {Class::ImGui, nullptr},
    {Class::Input, nullptr},
    {Class::Kernel, nullptr},
    {Class::Kernel_Event, nullptr},
    {Class::Kernel_Fs, nullptr},
    {Class::Kernel_Pthread, nullptr},
    {Class::Kernel_Sce, nullptr},
    {Class::Kernel_Vmm, nullptr},
    {Class::KeyManager, nullptr},
    {Class::Lib, nullptr},
    {Class::Lib_Ajm, nullptr},
    {Class::Lib_AppContent, nullptr},
    {Class::Lib_Audio3d, nullptr},
    {Class::Lib_AudioIn, nullptr},
    {Class::Lib_AudioOut, nullptr},
    {Class::Lib_AvPlayer, nullptr},
    {Class::Lib_Camera, nullptr},
    {Class::Lib_CommonDlg, nullptr},
    {Class::Lib_CompanionHttpd, nullptr},
    {Class::Lib_CompanionUtil, nullptr},
    {Class::Lib_ContentExport, nullptr},
    {Class::Lib_DiscMap, nullptr},
    {Class::Lib_ErrorDialog, nullptr},
    {Class::Lib_Fiber, nullptr},
    {Class::Lib_Font, nullptr},
    {Class::Lib_FontFt, nullptr},
    {Class::Lib_GameLiveStreaming, nullptr},
    {Class::Lib_GnmDriver, nullptr},
    {Class::Lib_Hmd, nullptr},
    {Class::Lib_HmdSetupDialog, nullptr},
    {Class::Lib_Http, nullptr},
    {Class::Lib_Http2, nullptr},
    {Class::Lib_Ime, nullptr},
    {Class::Lib_ImeDialog, nullptr},
    {Class::Lib_Jpeg, nullptr},
    {Class::Lib_Kernel, nullptr},
    {Class::Lib_LibcInternal, nullptr},
    {Class::Lib_Mouse, nullptr},
    {Class::Lib_Move, nullptr},
    {Class::Lib_MsgDlg, nullptr},
    {Class::Lib_Net, nullptr},
    {Class::Lib_NetCtl, nullptr},
    {Class::Lib_Ngs2, nullptr},
    {Class::Lib_NpAuth, nullptr},
    {Class::Lib_NpCommerce, nullptr},
    {Class::Lib_NpCommon, nullptr},
    {Class::Lib_NpManager, nullptr},
    {Class::Lib_NpMatching2, nullptr},
    {Class::Lib_NpPartner, nullptr},
    {Class::Lib_NpParty, nullptr},
    {Class::Lib_NpProfileDialog, nullptr},
    {Class::Lib_NpScore, nullptr},
    {Class::Lib_NpSnsFacebookDialog, nullptr},
    {Class::Lib_NpTrophy, nullptr},
    {Class::Lib_NpTus, nullptr},
    {Class::Lib_NpWebApi, nullptr},
    {Class::Lib_NpWebApi2, nullptr},
    {Class::Lib_Pad, nullptr},
    {Class::Lib_PlayGo, nullptr},
    {Class::Lib_PlayGoDialog, nullptr},
    {Class::Lib_Png, nullptr},
    {Class::Lib_Random, nullptr},
    {Class::Lib_RazorCpu, nullptr},
    {Class::Lib_Remoteplay, nullptr},
    {Class::Lib_Rtc, nullptr},
    {Class::Lib_Rudp, nullptr},
    {Class::Lib_SaveData, nullptr},
    {Class::Lib_SaveDataDialog, nullptr},
    {Class::Lib_Screenshot, nullptr},
    {Class::Lib_SharePlay, nullptr},
    {Class::Lib_SigninDialog, nullptr},
    {Class::Lib_Ssl, nullptr},
    {Class::Lib_Ssl2, nullptr},
    {Class::Lib_SysModule, nullptr},
    {Class::Lib_SystemGesture, nullptr},
    {Class::Lib_SystemService, nullptr},
    {Class::Lib_Usbd, nullptr},
    {Class::Lib_UserService, nullptr},
    {Class::Lib_Vdec2, nullptr},
    {Class::Lib_VideoOut, nullptr},
    {Class::Lib_Videodec, nullptr},
    {Class::Lib_VideoRecording, nullptr},
    {Class::Lib_Voice, nullptr},
    {Class::Lib_VrTracker, nullptr},
    {Class::Lib_WebBrowserDialog, nullptr},
    {Class::Lib_Zlib, nullptr},
    {Class::Loader, nullptr},
    {Class::Log, nullptr},
    {Class::Render, nullptr},
    {Class::Render_Recompiler, nullptr},
    {Class::Render_Vulkan, nullptr},
    {Class::Tty, nullptr},
};

template <typename T>
static auto UpdateColorLevels(T sink) {
#ifdef _WIN32
    using LogColor = std::uint16_t;

    const auto Grey = FOREGROUND_INTENSITY;
    const auto Cyan = FOREGROUND_GREEN | FOREGROUND_BLUE;
    const auto Bright_gray = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    const auto Bright_yellow = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    const auto Bright_red = FOREGROUND_RED | FOREGROUND_INTENSITY;
    const auto Bright_magenta = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
#else
    using LogColor = std::string_view;

#define ESC "\x1b"
    const auto Grey = ESC "[1;30m";
    const auto Cyan = ESC "[0;36m";
    const auto Bright_gray = ESC "[0;37m";
    const auto Bright_yellow = ESC "[1;33m";
    const auto Bright_red = ESC "[1;31m";
    const auto Bright_magenta = ESC "[1;35m";
#undef ESC
#endif

    const std::unordered_map<spdlog::level, LogColor> colors{
        {spdlog::level::trace, Grey},       {spdlog::level::debug, Cyan},
        {spdlog::level::info, Bright_gray}, {spdlog::level::warn, Bright_yellow},
        {spdlog::level::err, Bright_red},   {spdlog::level::critical, Bright_magenta}};

    for (const auto& [level, color] : colors) {
        sink->set_color(level, color);
    }

    return sink;
}

// A sink that suppresses a message once its exact formatted payload has already been emitted, so a
// warning/info that repeats — even when interleaved with other lines (which the strictly-consecutive
// dup_filter_sink misses) — is logged once instead of flooding the log. A periodic heartbeat
// re-emits a persistently-repeating line every kHeartbeat occurrences so the condition stays
// visible, and errors/critical are always passed through so important repeats are never hidden. The
// seen-set is bounded and cleared when large.
class ContentDedupSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit ContentDedupSink(std::vector<spdlog::sink_ptr> downstream)
        : downstream_sinks(std::move(downstream)) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        // Dedup only info/warn (the floods); always forward errors and above.
        if (msg.log_level < spdlog::level::err) {
            const std::string_view payload(msg.payload.data(), msg.payload.size());
            const std::size_t key = std::hash<std::string_view>{}(payload);
            auto [it, inserted] = counts.try_emplace(key, 0);
            const u64 n = ++it->second;
            if (!inserted && (n % kHeartbeat) != 0) {
                return; // already seen this message; suppress until the next heartbeat
            }
            if (counts.size() > kMaxEntries) {
                counts.clear();
            }
        }
        for (auto& sink : downstream_sinks) {
            if (sink->should_log(msg.log_level)) {
                sink->log(msg);
            }
        }
    }

    void flush_() override {
        for (auto& sink : downstream_sinks) {
            sink->flush();
        }
    }

private:
    static constexpr u64 kHeartbeat = 1000;      // re-emit a repeating line every 1000 occurrences
    static constexpr std::size_t kMaxEntries = 16384; // bound the seen-set memory
    std::vector<spdlog::sink_ptr> downstream_sinks;
    std::unordered_map<std::size_t, u64> counts;
};

void Setup(std::string_view log_filename) {
    static bool already_registered = false;

    if (!already_registered) {
        already_registered = true;
        std::atexit(Shutdown);
        std::at_quick_exit(Flush);
        std::set_terminate(Terminate);
    }

#ifdef _WIN32
    if (EmulatorSettings.GetLogType() == "wincolor") {
        g_console_sink = std::make_shared<spdlog::sinks::wincolor_stdout_sink_mt>();
    } else {
        g_console_sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
    }

#else
    g_console_sink = UpdateColorLevels(std::make_shared<spdlog_stdout>(spdlog::color_mode::always));
#endif

    g_console_sink->set_formatter(std::make_unique<thread_name_formatter>(UNLIMITED_SIZE));

    g_shad_file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        (GetUserPath(Common::FS::PathType::LogDir) / log_filename).string(), !g_should_append);
    g_shad_file_sink->set_formatter(
        std::make_unique<thread_name_formatter>(EmulatorSettings.GetLogSizeLimit()));

    std::initializer_list<spdlog::sink_ptr> sinks{g_console_sink, g_shad_file_sink};

    std::initializer_list<spdlog::sink_ptr> async_sink{std::make_shared<spdlog::sinks::async_sink>(
        spdlog::sinks::async_sink::config{.sinks = sinks})};

    // Content-based de-duplication: logs each unique formatted message once (with a heartbeat),
    // suppressing repeats even when they are interleaved with other lines. Supersedes the strictly-
    // consecutive dup_filter_sink, which the interleaved GPU warning floods evade.
    std::initializer_list<spdlog::sink_ptr> dup_filter{std::make_shared<ContentDedupSink>(
        std::vector<spdlog::sink_ptr>(EmulatorSettings.IsLogSync() ? sinks : async_sink))};

    spdlog::level default_log_level = spdlog::level::info;
    std::unordered_map<std::string, spdlog::level> log_level_per_class;

    if (EmulatorSettings.IsLogEnable()) {
        for (const auto class_level : std::views::split(EmulatorSettings.GetLogFilter(), ' ')) {
            const auto class_level_pair =
                std::views::split(class_level, ':') | std::ranges::to<std::vector<std::string>>();

            if (class_level_pair.size() != 2) {
                std::cerr << "bad log filter provided" << std::endl;
                continue;
            }

            if (class_level_pair.front()[0] == '*') {
                default_log_level = spdlog::level_from_str(class_level_pair.back() |
                                                           std::ranges::to<std::string>());
            } else {
                log_level_per_class[class_level_pair.front() | std::ranges::to<std::string>()] =
                    spdlog::level_from_str(class_level_pair.back() |
                                           std::ranges::to<std::string>());
            }
        }
    }

    for (auto& [name, logger] : ALL_LOGGERS) {
        logger = std::make_shared<spdlog::logger>(
            std::string(name), EmulatorSettings.IsLogSkipDuplicate()
                                   ? dup_filter
                                   : (EmulatorSettings.IsLogSync() ? sinks : async_sink));

        if (EmulatorSettings.IsLogEnable()) {
            const auto level_it = log_level_per_class.find(std::string(name));

            logger->set_level(level_it != log_level_per_class.end() ? level_it->second
                                                                    : default_log_level);
        } else {
            logger->set_level(spdlog::level::off);
        }
    }
}

void Shutdown() {
    for (auto& logger : ALL_LOGGERS | std::views::values) {
        logger.reset();
    }

    g_shad_file_sink.reset();
    g_console_sink.reset();
}

void Flush() {
    if (g_shad_file_sink != nullptr) {
        g_shad_file_sink->flush();
    }

    if (g_console_sink != nullptr) {
        g_console_sink->flush();
    }
}

void Terminate() {
    const std::string thread_name = Common::GetCurrentThreadName();
    const char* context = InferTerminateContext(thread_name);
    try {
        if (std::exception_ptr eptr{std::current_exception()}) {
            std::rethrow_exception(eptr);
        }

        LOG_CRITICAL(Debug, "Exiting without exception thread={} context={}", thread_name, context);

        std::quick_exit(std::to_underlying(ShadPs4ReturnCode::TERMINATE_WITHOUT_EXCEPTION));
    } catch (const std::exception& exception) {
        LOG_CRITICAL(Debug, "Exception thread={} context={}: {}", thread_name, context, exception);

        std::quick_exit(std::to_underlying(ShadPs4ReturnCode::TERMINATE_WITH_EXCEPTION));
    } catch (...) {
        LOG_CRITICAL(Debug, "Unknown exception caught thread={} context={}", thread_name, context);

        std::quick_exit(std::to_underlying(ShadPs4ReturnCode::TERMINATE_WITH_UNKNOWN_EXCEPTION));
    }
}
} // namespace Common::Log
