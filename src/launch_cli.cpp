// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "launch_cli.h"

#include <iostream>

#include <CLI/CLI.hpp>
#include <SDL3/SDL_messagebox.h>

#include "common/logging/log.h"

namespace LaunchCli {

ParseResult Parse(int argc, char* argv[]) {
    CLI::App app{"shadPS4 Emulator CLI"};
    ParseResult result{};

    auto& state = result.state;

    app.add_option("-g,--game", state.game_path, "Game path or ID");
    app.add_option("-p,--patch", state.patch_file, "Patch file to apply");
    app.add_flag("-i,--ignore-game-patch", state.ignore_game_patch,
                 "Disable automatic loading of game patches");
    app.add_flag("-b,--big-picture", state.big_picture, "Start in Big Picture Mode");
    app.add_option("-f,--fullscreen", state.fullscreen, "Fullscreen mode (true|false)");
    app.add_option("--override-root", state.override_root)->check(CLI::ExistingDirectory);
    app.add_flag("--wait-for-debugger", state.wait_for_debugger);
    app.add_option("--wait-for-pid", state.wait_pid);
    app.add_flag("--show-fps", state.show_fps);
    app.add_flag("--config-clean", state.config_clean);
    app.add_flag("--config-global", state.config_global);
    app.add_flag("--log-append", state.log_append);
    app.add_option("--add-game-folder", state.add_game_folder)->check(CLI::ExistingDirectory);
    app.add_option("--set-addon-folder", state.set_addon_folder)->check(CLI::ExistingDirectory);

    app.allow_extras();
    app.parse_complete_callback([&]() {
        const auto& extras = app.remaining();
        if (!extras.empty()) {
            state.game_args = extras;
        }
    });

    if (argc == 1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "shadPS4",
                                 "This is a CLI application. Please use the QTLauncher for a GUI:\n"
                                 "https://github.com/shadps4-emu/shadps4-qtlauncher/releases",
                                 nullptr);
        std::cout << app.help();
        result.should_exit = true;
        result.exit_code = -1;
        return result;
    }

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        result.should_exit = true;
        result.exit_code = app.exit(e);
        return result;
    }

    Common::Log::g_should_append = state.log_append;
    return result;
}

} // namespace LaunchCli
