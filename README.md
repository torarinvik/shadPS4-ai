<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-FileCopyrightText: 2026 fork maintainers
SPDX-License-Identifier: GPL-2.0-or-later
-->

# shadPS4 Experimental AI-Friendly Fork

> [!IMPORTANT]
> This repository is an independent fork derived from
> [shadPS4](https://github.com/shadps4-emu/shadPS4). It is not the official
> shadPS4 repository, is not maintained by the shadPS4 team, and is not endorsed
> by the shadPS4 project.

This fork exists so we can experiment quickly, test compatibility fixes, and
accept AI-generated or AI-assisted submissions in a space where that workflow is
explicitly allowed. It is not meant to undermine, replace, or negate the work of
the official shadPS4 project. Quite the opposite: if you care about PS4
emulation, please support the official project and its maintainers.

Please do not report issues from this repository to the upstream shadPS4 project
unless the same issue is reproducible on an unmodified upstream checkout.

## Parallel Porting Work

A separate port of this work to Elisa is being developed at the same time. This
repository is kept focused on the C++ codebase so emulator debugging and
compatibility work can continue without mixing in porting glue. A link to the
separate Elisa port will be added later.

## Project Status

This repository is a work in progress. It contains C++ code derived from
upstream shadPS4 plus local experiments, diagnostics, compatibility fixes, and
platform-specific investigations. Some behavior may differ from upstream.

## macOS Compatibility Snapshot

Current compatibility testing is focused on macOS, especially Apple Silicon Macs
running the Vulkan renderer through MoltenVK. Unless a row explicitly says
otherwise, the results below have **not** been validated on Linux or Windows.

This is not a polished compatibility list yet; it is a living dogfood log. The
fuller, messier test record lives in [GamesChecklist.md](GamesChecklist.md).
These are the titles that currently stand out as working or meaningfully
playable in local macOS testing:

| Game | Title ID | Current macOS result | What changed here |
|---|---|---|---|
| The Amazing Spider-Man 2 | CUSA00394 | Reaches the title screen and manual playtesting showed it advances and appears to work fine. | Fixed VideoOut handling for `A16R16G16B16Float` by mapping it to `R16G16B16A16Sfloat` and allowing 64-bpp presentation surfaces. |
| Tokyo Twilight Ghost Hunters Daybreak: Special Gigs | CUSA06045 | Boots into visual-novel scenes and appeared to work normally in the initial test. | Improved dogfood coverage and documented keyboard/controller mappings needed to advance prompts. |
| The Witch and the Hundred Knight 2 | CUSA10135 | Appears to work fine during manual play; audio works and the earlier crash was not reproduced. | Hardened NGS2/AT9 audio metadata and waveform decoding/mixing paths that previously crashed during logo/audio loading. |
| Katamari Damacy Reroll | CUSA24361 | Appears playable in manual testing; strict black-screen watchdog stayed nonblack. | Added safer render/watchdog testing and kept noisy metadata-read diagnostics from being mistaken for a fatal blocker. |
| Teenage Mutant Ninja Turtles: Shredder's Revenge | CUSA30991 | Reaches startup, main menu, and gameplay; startup music works. | Fixed the flexible-memory/`sceKernelMunmap(0, ...)` quit path that previously made this title much less stable. |
| Gigantosaurus: Dino Sports | CUSA43402 | Appears playable in manual testing, with occasional flicker. | Render watchdog confirmed it was producing real nonblack frames while Unity-style assets and shaders loaded. |

Several other titles now boot further than they did before but still have known
rendering, audio, filesystem, or HLE issues. We track those openly in
[GamesChecklist.md](GamesChecklist.md), including the useful failures. The aim is
not to pretend the emulator is done; the aim is to make every improvement
visible and reproducible.

If you want the official shadPS4 project, use:

- Upstream repository: [shadps4-emu/shadPS4](https://github.com/shadps4-emu/shadPS4)
- Upstream website: [shadps4.net](https://shadps4.net/)

## Upstream Attribution

This work is derived from [shadPS4](https://github.com/shadps4-emu/shadPS4),
which is licensed under GPL-2.0-or-later. Original shadPS4 copyright notices,
license terms, and source attribution are preserved.

The original shadPS4 authors are not responsible for changes made in this
repository. This fork's maintainers are responsible for modifications,
integration choices, support, and release artifacts produced from this
repository.

See [NOTICE.md](NOTICE.md) for the fuller attribution and non-affiliation notice.

## Building

Build instructions are inherited from the upstream shadPS4 layout:

- [Docker build instructions](documents/building-docker.md)
- [Windows build instructions](documents/building-windows.md)
- [Linux build instructions](documents/building-linux.md)
- [macOS build instructions](documents/building-macos.md)

Some commands still produce an executable named `shadps4` or `shadPS4.exe`.
That inherited name does not mean the artifact is an official shadPS4 build.

## Usage Examples

The command-line interface can be inspected with `--help`.

Common command patterns:

```sh
shadPS4 CUSA00001
shadPS4 --fullscreen true --config-clean CUSA00001
shadPS4 -g CUSA00001 --fullscreen true --config-clean
shadPS4 /path/to/game.elf
shadPS4 CUSA00001 -- -flag1 -flag2
```

## Debugging

For local development and troubleshooting, see
[Debugging and reporting issues](documents/Debugging/Debugging.md).

Issue reports for this fork should go to this repository's maintainers.
Upstream shadPS4 support channels should only be used for issues reproduced on
unmodified upstream shadPS4.

## Firmware Files

The emulator core can load some PlayStation 4 firmware files. Supported modules
must be placed in the `sys_modules` folder expected by the current build.

| Modules                  | Modules                  | Modules                  | Modules                  |
|--------------------------|--------------------------|--------------------------|--------------------------|
| libSceAudiodec.sprx      | libSceCesCs.sprx         | libSceFont.sprx          | libSceFontFt.sprx        |
| libSceFreeTypeOt.sprx    | libSceJpegDec.sprx       | libSceJpegEnc.sprx       | libSceJson.sprx          |
| libSceJson2.sprx         | libSceLibcInternal.sprx  | libSceNgs2.sprx          | libScePngEnc.sprx        |
| libSceRtc.sprx           | libSceSystemGesture.sprx | libSceUlt.sprx           |                          |

> [!CAUTION]
> Firmware files must be dumped from your legally owned PlayStation 4 console.
> They are not provided by this repository.

## Contributing

Anyone can contribute. AI-generated and AI-assisted submissions are welcome when
they work, are understandable, and preserve license attribution. See
[CONTRIBUTING.md](CONTRIBUTING.md).

When contributing code derived from upstream shadPS4, preserve license headers,
copyright notices, and attribution.

## License

This repository is distributed under the
[GPL-2.0-or-later license](LICENSE), consistent with upstream shadPS4.
