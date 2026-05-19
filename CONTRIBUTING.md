<!--
SPDX-FileCopyrightText: 2024 shadPS4 Emulator Project
SPDX-FileCopyrightText: 2026 fork maintainers
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Contributing

> [!IMPORTANT]
> This repository is an independent fork derived from upstream shadPS4. It is
> not the official shadPS4 repository, and it does not try to undermine or
> negate anything in the official project. Please support upstream shadPS4 and
> preserve upstream copyright notices, SPDX headers, and attribution when
> modifying derived files.

## Everyone Can Contribute

There is no gatekeeping here. You do not need to be a CS guru, emulator expert,
graphics expert, or long-time contributor to help. Small fixes, careful test
reports, documentation improvements, compatibility notes, cleanup patches, and
"I found this weird log line" investigations are all useful.

AI-generated and AI-assisted code is completely acceptable in this repository.
This fork exists partly because many repositories disallow or strongly
discourage AI-generated submissions, while this project wants to explore that
workflow openly.

The merge bar is intentionally practical: only merge it if it works. A change
does not need to be perfect. If it works for the slice it claims to handle, does
not obviously break nearby behavior, and keeps license/attribution intact, it is
welcome. If it is experimental, say so. If it only fixes one game, one crash,
one log parser, or one small cleanup, that is still real progress.

For AI-assisted patches, the same rule applies: try it, make sure it works well
enough for what you are submitting, and be honest about anything you did not
verify. We are not here to gatekeep; we are here to make the emulator better one
working improvement at a time. No particular code style is enforced beyond
keeping the code understandable and maintainable.

Beginner questions are welcome. Rough experiments are welcome when clearly
marked as experiments. Compatibility reports are welcome even when the result is
"it crashes" or "it only renders UI." Those reports are how this fork gets
better.

## General Rules

* Don't introduce new external dependencies into Core
* Don't use any platform specific code in Core
