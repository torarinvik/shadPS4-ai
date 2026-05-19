# shadPS4 fork game compatibility notes

Last updated: 2026-05-15

These notes track the local fork test state on macOS using the current checkout and the
`build/shadps4` binary. They are diagnostic notes, not a general compatibility list.

Use this checkout only:

```sh
cd "/path/to/this/shadPS4-fork"
```

Normal game runs should not use `SHADPS4_DISABLE_GPU_MEMORY_PROTECTION=1`; that option caused
flicker/distorted/stale graphics in multiple games and in UFC.

## Tested games

| Game | Title ID | Current state | Notes |
| --- | --- | --- | --- |
| Teenage Mutant Ninja Turtles: Shredder's Revenge | CUSA30991 | Playable enough to reach main menu and game | Fixed emulator quit caused by oversized flexible-memory request followed by `sceKernelMunmap(0, ...)`. Startup music works. User observed no in-game audio yet; check game settings and NGS2 audio behavior next. |
| EA Sports UFC | CUSA00264 | Runs, then hits render black-screen bug | Strict watchdog proves output goes black upstream of final presentation. Next render task is VideoOut compute input stats for shader `0xc455a5aa2c447041`. Brief flicker only when not using the bad GPU-memory-protection override. |
| Beast Quest | unknown | Starts, audio/narrator plays, then black screen | Also showed flicker/distorted/missing graphics during startup. Similar black-screen bucket to UFC, but not yet diagnosed as deeply. |
| Yooka-Laylee | CUSA05721 | Boots into scene but input/render issues remain | Severe flicker/distortion/missing geometry seen at startup. Reached a "press X to continue" style screen, but keyboard X did not advance. Resizing by user changed the view but was not itself the root cause. |
| Joe's Diner | unknown | Starts and accepts movement input | Arrow keys and left stick worked. Keyboard button mappings such as X/C/Z did not behave as expected. Needs input mapping/config follow-up. |
| Hasbro Family Fun Pack | unknown | Ran after fullscreen quit issue was investigated | Initial quit happened when entering fullscreen; after macOS permission/restart it ran further. Needs retest for current status. |
| Another Sight | CUSA15308 | Blocked on fixed guest address mapping | Hits fixed map around `0x4000000000`, which overlaps the macOS x86_64-on-Apple-Silicon reserved address hole. Not worth chasing under current Rosetta/native-address setup without address translation or different host. |
| Minecraft Dungeons | CUSA18797 | Blocked on fixed guest address mapping | Same `0x4000000000` mapping issue as Another Sight. |
| Taxi Chaos | CUSA20527 | Blocked on fixed guest address mapping | Same `0x4000000000` mapping issue as Another Sight. |
| Severed Steel | CUSA30139 | Blocked on fixed guest address mapping | Same `0x4000000000` mapping issue as Another Sight. |

## Useful run commands

TMNT:

```sh
SHADPS4_STRICT_RENDER_VALIDATION=1 \
SHADPS4_TRACE_RENDER=0 \
SHADPS4_VIDEOOUT_UNORM=1 \
./build/shadps4 -g Games/CUSA30991/eboot.bin
```

UFC trace:

```sh
SHADPS4_BIN=./build/shadps4 \
SHADPS4_STRICT_RENDER_VALIDATION=1 \
SHADPS4_TRACE_RENDER=0 \
SHADPS4_VIDEOOUT_UNORM=1 \
scripts/run_ufc_trace.sh
```

UFC strict black-screen trace:

```sh
SHADPS4_BIN=./build/shadps4 \
SHADPS4_STRICT_RENDER_VALIDATION=1 \
SHADPS4_STRICT_BLACK_SCREEN_WATCHDOG=1 \
SHADPS4_TRACE_IMAGE_VIEW_INVARIANTS=1 \
SHADPS4_TRACE_SCREENSHOTS=0 \
SHADPS4_TRACE_DESKTOP_SCREENSHOTS=0 \
SHADPS4_TRACE_RENDER=0 \
scripts/run_ufc_trace.sh
```

## Current diagnostic buckets

### Fixed guest address mapping

Games in this bucket request fixed mappings around `0x4000000000`. On macOS x86_64 running on
Apple Silicon, that overlaps the reserved GPU address hole `0x1000000000-0x6fffffffff`. Current
diagnostics now report this cleanly instead of leaving confusing output addresses.

Affected so far:

- Another Sight
- Minecraft Dungeons
- Taxi Chaos
- Severed Steel

### Render black screen after successful startup

UFC is the best-instrumented example. The watchdog shows blackness starts before final
presentation/ImGui/swapchain. The VideoOut storage binding and image view range look structurally
sane. Next high-value step is sampling input textures to VideoOut compute shader
`0xc455a5aa2c447041`.

Beast Quest may belong here, but needs focused tracing.

### Flicker/distortion

The biggest discovered cause was running with `SHADPS4_DISABLE_GPU_MEMORY_PROTECTION=1`. Avoid that
env for normal compatibility testing. Yooka-Laylee and Beast Quest still showed graphics problems
without a final diagnosis.

### Input mapping

Joe's Diner and Yooka-Laylee both exposed keyboard/controller mapping questions. Movement worked in
Joe's Diner, but expected button keys did not. Yooka-Laylee did not advance on keyboard X at the
"press X" prompt.

## Recent TMNT fix

TMNT requested a huge flexible heap after reaching the logo/menu path:

```text
sceKernelMapNamedFlexibleMemory len=0x54330000 name='SceLibcHeap'
```

Before the fix, shadPS4 failed the allocation, the guest tried `sceKernelMunmap(0, 0x54330000)`,
and the emulator asserted internally. The current local fix:

- returns `ORBIS_KERNEL_ERROR_ENOMEM` for flexible-memory exhaustion,
- rejects null/misaligned `sceKernelMunmap` ranges with `ORBIS_KERNEL_ERROR_EINVAL`,
- returns an error for invalid unmap ranges instead of asserting,
- adds optional `SHADPS4_TRACE_SIGNAL_SYMBOLS=1` for future host-side signal diagnostics.
