// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace LaunchCli {

struct CliState {
    std::optional<std::string> game_path;
    std::optional<std::string> patch_file;
    std::optional<std::filesystem::path> override_root;
    std::optional<std::filesystem::path> add_game_folder;
    std::optional<std::filesystem::path> set_addon_folder;
    std::optional<std::string> fullscreen;
    std::optional<int> wait_pid;
    std::vector<std::string> game_args;
    bool ignore_game_patch = false;
    bool big_picture = false;
    bool show_fps = false;
    bool config_clean = false;
    bool config_global = false;
    bool log_append = false;
    bool wait_for_debugger = false;
};

struct ParseResult {
    CliState state;
    bool should_exit = false;
    int exit_code = 0;
};

ParseResult Parse(int argc, char* argv[]);

} // namespace LaunchCli
