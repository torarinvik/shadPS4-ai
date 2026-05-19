// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "launch_pipeline.h"

#include <cstdint>
#include <iostream>
#include <memory>

#include <core/emulator_settings.h>
#include <core/emulator_state.h>
#include <core/user_settings.h>

#include "common/config.h"
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "core/debugger.h"
#include "core/file_sys/fs.h"
#include "core/ipc/ipc.h"
#include "emulator.h"
#include "imgui/big_picture/big_picture.h"

#ifdef SHADPS4_ENABLE_ELISA_PORTS
#include "elisa/native/shadps4_elisa_debugger.h"
#endif

namespace LaunchPipeline {

namespace {

void WaitForPidExit(int pid) {
#ifdef SHADPS4_ENABLE_ELISA_PORTS
    constexpr std::uint32_t poll_ms = 500;
    constexpr std::int64_t max_polls = 7200; // One hour keeps restart waits finite and visible.
    std::cerr << "Waiting for process " << pid << " to exit..." << std::endl;
    if (shadps4_elisa_wait_for_pid_exit(pid, poll_ms, max_polls)) {
        return;
    }
    std::cerr << "Timed out waiting for process " << pid
              << " to exit; continuing launch anyway." << std::endl;
#else
    Core::Debugger::WaitForPid(pid);
#endif
}

} // namespace

void InitializeRuntimeSettings() {
    // Start default log before user settings can redirect or append to the configured log.
    Common::Log::Setup("shad_log.txt");

    IPC::Instance().Init();

    auto emu_state = std::make_shared<EmulatorState>();
    EmulatorState::SetInstance(emu_state);
    UserSettings.Load();

    const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    Config::load(user_dir / "config.toml");

    auto key_manager = KeyManager::GetInstance();
    key_manager->LoadFromFile();
    if (key_manager->GetAllKeys().TrophyKeySet.ReleaseTrophyKey.empty() &&
        !Config::getTrophyKey().empty()) {
        auto keys = key_manager->GetAllKeys();
        if (keys.TrophyKeySet.ReleaseTrophyKey.empty() && !Config::getTrophyKey().empty()) {
            keys.TrophyKeySet.ReleaseTrophyKey =
                KeyManager::HexStringToBytes(Config::getTrophyKey());
            key_manager->SetAllKeys(keys);
            key_manager->SaveToFile();
        }
    }

    std::shared_ptr<EmulatorSettingsImpl> emu_settings = std::make_shared<EmulatorSettingsImpl>();
    EmulatorSettingsImpl::SetInstance(emu_settings);
    emu_settings->Load();

    Common::Log::Shutdown();
    Common::Log::g_should_append |= EmulatorSettings.IsLogAppend();
    Common::Log::Setup("shad_log.txt");
}

bool HandleUtilityCommand(bool big_picture, char* executable_name,
                          const std::optional<std::filesystem::path>& add_game_folder,
                          const std::optional<std::filesystem::path>& set_addon_folder) {
    if (big_picture) {
        BigPictureMode::Launch(executable_name);
        return true;
    }

    if (add_game_folder) {
        EmulatorSettings.AddGameInstallDir(*add_game_folder);
        EmulatorSettings.Save();
        std::cout << "Game folder successfully saved.\n";
        return true;
    }

    if (set_addon_folder) {
        EmulatorSettings.SetAddonInstallDir(*set_addon_folder);
        EmulatorSettings.Save();
        std::cout << "Addon folder successfully saved.\n";
        return true;
    }

    return false;
}

bool NormalizeGamePathAndArgs(std::optional<std::string>& game_path,
                              std::vector<std::string>& game_args) {
    if (!game_path.has_value()) {
        if (!game_args.empty()) {
            game_path = game_args.front();
            game_args.erase(game_args.begin());
        } else {
            std::cerr << "Error: Please provide a game path or ID.\n";
            return false;
        }
    }

    if (!game_args.empty()) {
        if (game_args.front() == "--") {
            game_args.erase(game_args.begin());
        } else {
            std::cerr << "Error: unhandled flags\n";
            return false;
        }
    }

    return true;
}

bool ApplyLaunchFlags(const std::optional<std::string>& patch_file, bool ignore_game_patch,
                      const std::optional<std::string>& fullscreen, bool show_fps,
                      bool config_clean, bool config_global) {
    if (patch_file) {
        MemoryPatcher::patch_file = *patch_file;
    }

    if (ignore_game_patch) {
        Core::FileSys::MntPoints::ignore_game_patches = true;
    }

    if (fullscreen) {
        if (*fullscreen == "true") {
            EmulatorSettings.SetFullScreen(true);
        } else if (*fullscreen == "false") {
            EmulatorSettings.SetFullScreen(false);
        } else {
            std::cerr << "Error: Invalid argument for --fullscreen (use true|false)\n";
            return false;
        }
    }

    if (show_fps) {
        EmulatorSettings.SetShowFpsCounter(true);
    }

    if (config_clean) {
        EmulatorSettings.SetConfigMode(ConfigMode::Clean);
    }

    if (config_global) {
        EmulatorSettings.SetConfigMode(ConfigMode::Global);
    }

    return true;
}

std::optional<std::filesystem::path> ResolveGamePathOrId(const std::string& game_path) {
    std::filesystem::path eboot_path(game_path);
    if (std::filesystem::exists(eboot_path)) {
        return eboot_path;
    }

    constexpr int max_depth = 5;
    for (const auto& install_dir : EmulatorSettings.GetGameInstallDirs()) {
        if (auto found_path = Common::FS::FindGameByID(install_dir, game_path, max_depth)) {
            return found_path;
        }
    }

    std::cerr << "Error: Game ID or file path not found: " << game_path << "\n";
    return std::nullopt;
}

void RunEmulator(const char* executable_name, bool wait_for_debugger,
                 const std::filesystem::path& eboot_path,
                 const std::vector<std::string>& game_args,
                 const std::optional<std::filesystem::path>& override_root) {
    auto* emulator = Common::Singleton<Core::Emulator>::Instance();
    emulator->executableName = executable_name;
    emulator->waitForDebuggerBeforeRun = wait_for_debugger;
    emulator->Run(eboot_path, game_args, override_root);
}

int RunParsedLaunch(const char* executable_name, LaunchIntent::CliState state) {
    if (state.wait_pid) {
        WaitForPidExit(*state.wait_pid);
    }

    InitializeRuntimeSettings();

    if (HandleUtilityCommand(state.big_picture, const_cast<char*>(executable_name),
                             state.add_game_folder, state.set_addon_folder)) {
        return 0;
    }

    if (!NormalizeGamePathAndArgs(state.game_path, state.game_args)) {
        return 1;
    }
    if (!state.game_path) {
        return 1;
    }

    if (!ApplyLaunchFlags(state.patch_file, state.ignore_game_patch, state.fullscreen,
                          state.show_fps, state.config_clean, state.config_global)) {
        return 1;
    }

    const auto eboot_path = ResolveGamePathOrId(*state.game_path);
    if (!eboot_path) {
        return 1;
    }

    RunEmulator(executable_name, state.wait_for_debugger, *eboot_path, state.game_args,
                state.override_root);
    return 0;
}

} // namespace LaunchPipeline
