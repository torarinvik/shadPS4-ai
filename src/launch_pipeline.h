// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "launch_cli.h"

namespace LaunchPipeline {

void InitializeRuntimeSettings();

bool HandleUtilityCommand(bool big_picture, char* executable_name,
                          const std::optional<std::filesystem::path>& add_game_folder,
                          const std::optional<std::filesystem::path>& set_addon_folder);

bool NormalizeGamePathAndArgs(std::optional<std::string>& game_path,
                              std::vector<std::string>& game_args);

bool ApplyLaunchFlags(const std::optional<std::string>& patch_file, bool ignore_game_patch,
                      const std::optional<std::string>& fullscreen, bool show_fps,
                      bool config_clean, bool config_global);

std::optional<std::filesystem::path> ResolveGamePathOrId(const std::string& game_path);

void RunEmulator(const char* executable_name, bool wait_for_debugger,
                 const std::filesystem::path& eboot_path,
                 const std::vector<std::string>& game_args,
                 const std::optional<std::filesystem::path>& override_root);

int RunParsedLaunch(const char* executable_name, LaunchCli::CliState state);

} // namespace LaunchPipeline
