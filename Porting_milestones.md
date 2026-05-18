# shadPS4 Elisa Porting Milestones

This file tracks which shadPS4 C++ responsibilities have been ported, shadowed,
or prepared for incremental Elisa ownership. The goal is not a big-bang rewrite;
it is to replace small, inspectable pieces of C++ with safer Elisa code while
keeping the emulator runnable after every slice.

## Status Legend

- `ported`: Elisa owns the behavior in normal Elisa-enabled builds.
- `shadowed`: Elisa computes/checks the behavior, while C++ remains authoritative.
- `wrapped`: Elisa controls or calls into the C++ behavior through a narrow ABI.
- `planned`: Good next candidate, not implemented yet.

## Ported

| Area | C++ Source | Elisa Source | Status | Notes |
| --- | --- | --- | --- | --- |
| Launch intent parsing | `src/launch_cli.cpp` | `elisa/src/launch_intent.elisa` | ported | Elisa is the default parser when `SHADPS4_ENABLE_ELISA_PORTS` is enabled; `SHADPS4_DISABLE_ELISA_LAUNCH_INTENT=1` keeps the C++ escape hatch. |
| Launch directory validation | `src/launch_cli.cpp` | `elisa/src/launch_intent.elisa` | ported | Elisa validates `--override-root`, `--add-game-folder`, and `--set-addon-folder` through a small filesystem C ABI. |
| Root main dispatch | `src/main.cpp` | `elisa/src/shadps4_main.elisa` | ported | C++ `main.cpp` is now a platform trampoline in Elisa-enabled builds. Elisa decides no-args, help, parse-error, and normal-run flow. |
| macOS debugger wait helper | `core/debugger.*` path usage | `elisa/src/debugger.elisa` | ported | Elisa-backed wait path is used by the launch pipeline in Elisa-enabled builds, with platform-specific native helpers. |
| Launch pipeline orchestration | `src/launch_pipeline.cpp` | `elisa/src/launch_pipeline.elisa`, `src/launch_elisa_main_bridge.cpp` | ported | Elisa-enabled builds now own wait/init/utility/flag/resolve/run sequencing; C++ exposes narrow host operations and remains as the non-Elisa fallback. |

## Wrapped Or Shadowed

| Area | C++ Source | Elisa/C ABI Surface | Status | Notes |
| --- | --- | --- | --- | --- |
| C ABI packaging | `CMakeLists.txt`, `cmake/Elisa.cmake` | `-emit c-archive` targets | ported | Elisa modules build as static archives with checked-in ABI headers and sidecar manifests. |
| Launch intent smoke/shadow tests | `elisa/native/launch_intent_smoke.cpp` | `elisa/src/launch_intent.elisa` | shadowed | Smoke compares normalized launch intent behavior without changing non-Elisa runtime behavior. |
| Safe range and VideoOut validation policy | `core/libraries/videoout/*`, address/range helper call sites | `elisa/src/safe_range.elisa`, `elisa/src/videoout_validation.elisa` | shadowed | Pure Elisa helpers now cover overflow-safe exclusive ranges plus VideoOut shape/attribute/index checks; C++ remains authoritative until ABI wiring lands. |

## Compiler Porting Support

| Feature | Status | Notes |
| --- | --- | --- |
| Static C ABI archives | ported | `project.json` targets can emit `.a` archives for C/C++ linking. |
| Checked-in ABI headers | ported | Generated headers are audit artifacts; checked-in headers remain the build contract. |
| Unsafe permission fencing | ported | Trusted low-level regions are visible and auditable. |
| Bounds/provenance/alias safety slices | ported | These safety checks are compiler-enforced enough to dogfood low-level ports. |
| Progress safety | ported | Budgeted loops/recursion/blocking checks give us pressure tests against runaway code. |
| C++-style module paths | ported | `module Foo::Bar:` and `Foo::Bar::baz()` parse to existing dotted qualified names internally. |
| Reusable porting guardrails | shadowed | `safe_range.elisa` and `videoout_validation.elisa` provide tested policy helpers for replacing scattered C++ guard code incrementally. |

## Planned Next Ports

| Area | C++ Source | Proposed Elisa Module | Status | Why It Is A Good Candidate |
| --- | --- | --- | --- | --- |
| Runtime settings internals | `src/launch_pipeline.cpp`, settings classes | `LaunchPipeline` host externs | planned | The orchestration is ported; settings storage/loading still lives in C++ and should stay wrapped until Elisa has enough filesystem/config support. |
| VideoOut validation ABI | `core/libraries/videoout/driver.cpp`, `video_out.cpp` | `elisa/src/videoout_validation.elisa` | planned | The pure Elisa policy exists; next step is exposing narrow C ABI wrappers and replacing one validation cluster at a time. |
| Utility command side effects | `src/launch_pipeline.cpp`, BigPicture/settings | `LaunchPipeline` host externs | planned | Routing is ported; Big Picture launch and settings persistence remain native side effects. |
| Game path normalization | `src/launch_pipeline.cpp` | `LaunchPipeline::normalize_game_path_and_args` | planned | Mostly pure argument policy with clear error cases. |
| Game checklist/update harness | manual testing workflow | `elisa/src/game_checklist.elisa` or fixtures | planned | Keeps compatibility discoveries structured as we dogfood. |

## Current Principle

Prefer porting policy, validation, orchestration, and trace/checklist code before
renderer/GPU execution. Effectful functions are welcome, but they should enter
Elisa through narrow permissions and small native ABI surfaces first.
