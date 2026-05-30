# shadPS4 Games Checklist

`Acquired` here means the game is currently present in the `Games` folder as a title-ID directory.

| Acquired | Game | Title ID | macOS | Windows | Linux |
|---|---|---|---|---|---|
| [x] | EA Sports UFC | CUSA00264 | Tested. Runs, then hits the known render black-screen bug. Strict watchdog shows the black frame starts upstream of final presentation; next step is VideoOut compute input tracing for shader `0xc455a5aa2c447041`. | Not tested | Not tested |
| [x] | DRIVECLUB | CUSA00003 | Tested. Initially crashed when a fixed direct-memory map into the PRT/macOS GPU-reserved hole at `0x1000000000` fell through to `CarveVMA`; fixed by applying Apple fixed-mapping relocation to all fixed mappings, not only `Fixed\|NoOverwrite`. Retest survived until 75s timeout with playtime around 1:12. Current issues: repeated `videoPlayer` audio invalid-port errors, stubbed `sceHttpWaitRequest`, and Facebook dialog status spam; needs manual visual assessment. | Not tested | Not tested |
| [x] | Need for Speed Rivals | CUSA00168 | Retested after macOS fixed-mapping hardening. The old deterministic `SmallBlockAllocator` crash from guest writes to the original `0x1043ffc000` region no longer reproduces: relocated access faults are handled with single-step register restore, pthread mutex/condvar/rwlock HLE slots resolve relocated guest addresses, and the game survived a 45s capped run to timeout. Needs manual visual/gameplay validation next. | Not tested | Not tested |
| [-] | The Amazing Spider-Man 2 | CUSA00394 | Tested. Initially crashed on VideoOut `A16R16G16B16Float` because presentation image format conversion only handled 32-bit formats; fixed by mapping it to `R16G16B16A16Sfloat` and allowing 64-bpp VideoOut surfaces. Retest reached the title screen and manual playtesting showed it advances and appears to work fine. | Not tested | Not tested |
| [x] | Tearaway Unfolded | CUSA00562 | Retested after the game was restored to the local `Games/` folder. It gets much farther than startup: VideoOut flips, shader compilation, AVPlayer/FM‌OD audio, and save-data activity all begin, then it crashes on `shadPS4:GpuCommandProcessor` with `Unhandled access violation ... Read from address 0x0`. | Not tested | Not tested |
| [x] | FINAL FANTASY VII | CUSA01875 | Tested. Short capped run survived until the 75s external timeout rather than crashing, and manual playtest confirms the game works/runs and advances into gameplay/dialogue. Current issue is severe rendering corruption: UI/dialogue boxes render, but 3D geometry is massively misordered/overlapped with black background and broken scene composition. Log breadcrumbs point at the PS1/Windows/OpenGL-style compatibility layer: `GL INIT` progresses, but `GetPixelFormat`, `wglGetProcAddress` for color-table extensions, and `glGetError` are missing/stubbed; NGS2 pan/matrix calls also spam during audio setup. | Not tested | Not tested |
| [x] | UFC 2 | CUSA01968 | Tested. No immediate launch crash; the game maps `EAOSGlobal` at `0x1000000000`, which is now relocated out of the macOS GPU-reserved hole. Short run survived until external timeout with playtime updates around 1:13, but the user observed a black screen with no behavior, matching the existing boots-but-black bucket. | Not tested | Not tested |
| [-] | Hasbro Family Fun Pack | CUSA03312 | Retested. Launcher starts and selecting Monopoly with X successfully hands off into `monopoly/monopoly.self`; direct `monopoly.self` launch also works. The earlier Monopoly crash immediately after selection was tied to `sceUserServiceGetUserName` receiving the system user id `0xff`; user-service name/color lookups now fall back to the default local user for system ids. The handoff is now queued from the guest thread and performed by the emulator main thread, avoiding the old crashy fork/wait or guest-thread `exec` shape. Added small no-op `libSceUlt` coverage and demoted expected empty user-slot probes, so ULT/user-service spam is gone. Remaining low-priority noise is one-shot NP/network stubs such as `sceNpSignalingInitialize` and `sceNpLookupCreateTitleCtx`. | Not tested | Not tested |
| [x] | Earth Defense Force 4.1: The Shadow of New Despair | CUSA03467 | Tested. Boots well: loads modules/PRX, maps memory, hides splash screen, processes a user-service event, opens VideoOut and registers buffers, and begins compiling compute shaders (e.g. cs `0xa0ac792f`). Then crashes in the GPU command processor (`ProcessGraphics`). Diagnosed via a temporary dcb dword dump: the first crash was `Unimplemented PM4 type 0` at a 22-dword run of zeros that the guest left as a hole inside its submitted command buffer (valid content resumed cleanly afterwards) — fixed by skipping a zero/padding dword one at a time, like the existing type-2 NOP handling (`liverpool.cpp` `ProcessGraphics`). After that fix the parser advances further and now hits a separate `Wrong PM4 type 1` desync where the read pointer is off by exactly one dword (real type-3 content resumes at +1), so some earlier packet is under-consumed by 1 dword. That second issue is a genuine packet-size bug that needs a frame dump to pin precisely and was intentionally NOT blind-patched to avoid regressing games that currently render. Current bucket: command-stream parser advances past the zero-hole but still desyncs by one dword on a later packet. | Not tested | Not tested |
| [x] | Prison Architect: PlayStation 4 Edition | CUSA03487 | Tested. Capped run starts the game, compiles shaders, updates playtime briefly, and exits without a captured fatal emulator crash. User observed harsh buzzing audio and no visible image. Log is dominated by many `sceNgs2ParseWaveformData` calls, PadSpk mono audio open, missing `tilesetpadded.gnf`/`allsprites.gnf` fallbacks, and a `libpng error: IDAT: incorrect data check`. Current bucket: boots but no image with broken/buzzing audio, likely NGS2/audio plus missing/failed asset decode rather than a pure launch crash. | Not tested | Not tested |
| [x] | Joe's Diner | CUSA03774 | Tested. Starts and accepts movement input from arrow keys/left stick. After the loading-screen transition it still crashes on `shadPS4:GpuCommandProcessor` with `Unhandled access violation ... Read from address 0x20`; latest macOS crash report points at `MVKBuffer::applyBufferMemoryBarrier` during `vkQueueSubmit`. Log immediately before crash is dominated by `CopyBuffer destination aliases cached image` and ambiguous `FindImageFromRange` warnings, so current bucket is buffer/image aliasing plus invalid Vulkan buffer barrier rather than input mapping. | Not tested | Not tested |
| [x] | WORLD OF FINAL FANTASY | CUSA04647 | Tested. Boots into gameplay, audio works, UI/dialog/minimap/pause/map overlays render correctly, and the game appears to keep playing. Main 3D gameplay/world rendering is mostly black, so this is a partial render-path bug rather than the UFC-style total black-screen/no-render failure. Also shows an in-game `0x80552C0A` error dialog early in the boot flow and repeated sign-in/stat log noise. | Not tested | Not tested |
| [x] | Rise of the Tomb Raider | CUSA05716 | Retested after the game was added locally. Same fixed-mapping leak family as Need for Speed Rivals: `OSHeap.Core` is relocated away from `0x1000000000`, then the guest later writes near the original fixed region (`0x1000b206d8`) and crashes. | Not tested | Not tested |
| [x] | Yooka-Laylee | CUSA05721 | Tested. Boots into scene/menu path, but has severe flicker/distortion/missing geometry and did not advance from the "press X" prompt with keyboard X. | Not tested | Not tested |
| [-] | Tokyo Twilight Ghost Hunters Daybreak: Special Gigs | CUSA06045 | Tested. Boots and reaches in-game visual-novel scenes. Input combo prompt `R1 + Square` maps to keyboard `U + Z`; game appeared to work normally during the initial test. | Not tested | Not tested |
| [x] | UFC 3 | CUSA06534 | Renders the EA Sports UFC 3 title/loading screens correctly (confirmed visually; presents via the flip-event path, so flip-count heuristics misread 0 — earlier "black screen" notes were a false negative). Then hits a recurring MoltenVK device loss `kIOGPUCommandBufferCallbackErrorInvalidResource` (`Lost VkDevice ... Invalid Resource`), after which `vk_presenter.cpp GetRenderFrame` asserts on `ErrorDeviceLost`. Same bug as Madden NFL 24. Root-caused via Metal API validation (`MVK_CONFIG_DEBUG=1`): GPU buffers were being freed while a recorded-but-not-yet-submitted Metal command buffer still referenced them. Three fixes landed and verified: (1) `vk_scheduler` `PopPendingOperations` now requires `gpu_tick < CurrentTick()` so a deferred destroy can't run for the open, un-submitted recording; (2) buffer-cache temp/overlap/GC buffers deferred via `DeferOperationAfterSubmit`; (3) the detile scratch buffer (`TileManager`) is now freed at its true last-use tick (after the caller's `image.Upload`) via `ReleasePendingScratchBuffers`. Effect: device loss pushed from ~15s to ~60s and compute texture binds climb past 1500 (most rendering yet) before a later, separate device loss on a different command buffer. Ruled out (not the cause): freed-image-in-flight (already GPU-safe), and illegal sRGB storage image (`SHADPS4_VIDEOOUT_UNORM=1` did not help). Regression-checked clean: New Super Lucky's Tale (4:30 playtime, no device loss) and Crash Bandicoot's unrelated LegalScreen crash is identical with/without the fixes. NOT fully fixed — a later device loss remains. DEEPER ROOT CAUSE FOUND via custom tick-lifecycle instrumentation (logging alloc/defer/destroy ticks per VkBuffer handle plus every queue submit): the emulator runs THREE independent `Scheduler` instances on the same Metal queue, each with its own timeline tick counter — `draw_scheduler`, `present_scheduler`, `flip_scheduler` (vk_presenter.cpp:147-149). The trace showed two concurrent submit streams with disjoint tick ranges (e.g. ~320 and ~600) interleaved. The VideoOut flip path (`Presenter::PrepareFrame`, flip_scheduler context) calls `texture_cache.UpdateImage` -> `RefreshImage` -> `DetileImage`, which allocates the detile scratch buffer and records its compute on `draw_scheduler` (the only scheduler texture_cache/tile_manager hold), then blits the resulting image into the frame on `flip_scheduler` (fsr_pass). So a resource's lifetime is tracked against ONE scheduler's timeline while a DIFFERENT scheduler's in-flight command buffer still references it (or the image it produced). Per-scheduler tick deferral (all three landed fixes) is therefore structurally insufficient — when draw_scheduler's gpu_tick says the buffer is free, flip_scheduler's command buffer using it may not have completed, and the two timelines have no cross-synchronization. The correct fix is cross-scheduler resource synchronization (e.g. tie the scratch/image lifetime to whichever scheduler records the consuming command, or have UpdateImage during flip run on flip_scheduler, or add an inter-scheduler dependency so a resource isn't freed until all schedulers that referenced it have passed). Current bucket: renders title/loading screens; dominant per-scheduler use-after-frees fixed, but a cross-scheduler (draw vs flip) resource-lifetime device-loss remains (shared with Madden NFL 24). PROGRESS (2026-05-30): two distinct device-loss bugs identified; the FIRST is fixed and the game now advances from ~34-40% loading to 100% loading (user-confirmed visual gain) before hitting the SECOND. (A) CROSS-SCHEDULER RESOURCE-LIFETIME (FIXED, kept): the three Schedulers (draw/present/flip) share one Metal queue but each has an independent timeline, so a deferred GPU-resource destroy gated on only the recording scheduler's tick could fire while another scheduler's in-flight command buffer still referenced the resource. Fix in `vk_scheduler.{h,cpp}` + `vk_presenter.cpp`: each Scheduler registers its two siblings' timeline semaphores (`AddSiblingSemaphore`); every deferred destroy (`DeferOperation`/`DeferOperationAfterSubmit`/`DeferPriorityOperation`) snapshots each sibling's highest already-submitted tick (`CurrentTick()-1`) and only frees once all siblings have GPU-completed it (`SiblingWaitsSatisfied` gate in `PopPendingOperations` + sibling `Wait` in the priority-ops thread). Strictly more conservative — can only DELAY a free, never free earlier — so it cannot corrupt rendering or add a new UAF. Also kept the `TileManager` scratch `DeferOperationAfterSubmit` nudge (harmless, subsumed by the gate). Effect: loading reaches 100%. NOTE the earlier one-tick-only attempt was an intermittent false-positive (one 120s run reached 6244 binds clean, another crashed at 1488); the sibling-gate is the principled version of that idea. (B) DEPTH-STENCIL/COLOR ALIASING STORM (REMAINS — the current crash, at the loading-complete→menu transition): at the 100% moment the game aliases ONE 12 MB region at guest_addr=0x279620000 as BOTH a D16UnormS8Uint depth-stencil image AND an R16Uint compute/color image. `texture_cache.cpp:603 ResolveOverlap` logs "Avoiding incompatible depth-stencil image reuse" for that single address **1653 times** in a burst, `image.cpp:623 CopyImage` skips the incompatible direct copy, and the cache thrashes creating/freeing an overlapping color image against the live depth image until a resource is freed while a compute command buffer (MTLCommandBuffer "Command Buffer 2") still references it → `kIOGPUCommandBufferCallbackErrorInvalidResource` device loss, then `vk_swapchain.cpp AcquireNextImage` ErrorDeviceLost terminate. (The adjacent D16S8→D32S8 `image_view format_substitution usage_storage=false` line is normal MoltenVK behavior, NOT the cause.) This is a SEPARATE bug from (A). CORRECTION (2026-05-30, after testing): the "depth-image accumulation → out-of-memory" theory is REFUTED by direct experiment. A bounded-dedup fix in `FindImage` (reclaim stale same-address aliases, keeping the N most-recently-accessed) reduced the resident depth-image count from ~57 → ~42 → ~27 (the "Avoiding incompatible depth-stencil reuse" warning count fell 1653 → 911 → 381 accordingly), yet the device loss still occurred at the SAME point every time: ~48-49s / ~1615 compute-texture binds. If memory exhaustion from the depth leak were the cause, cutting the image count by half would have delayed or removed the crash; it did not move at all. CONCLUSION: the depth-aliasing/CopyImage-skip storm is a CO-OCCURRING SYMPTOM at the loading→menu transition, NOT the cause of the device loss. The bounded-dedup change was reverted (it only reduced the symptom and freeing images adds risk). The REAL trigger is some specific GPU operation issued deterministically at the loading-complete moment (~1615 binds) that produces an invalid Metal resource on "Command Buffer 2" — still UNIDENTIFIED. A correct next investigation must inspect the exact failing dispatch/draw and its bound resources at the crash instant (not the depth-warning noise): e.g. capture with Metal frame capture / MVK_CONFIG_DEBUG at that moment, or log the precise pipeline+descriptor set on the command buffer that fails. Failed Bug-B attempts (all reverted): (1) FreeImage on every incompatible-depth reject — wedged rendering at startup (freed the live depth target); (2) bounded same-address dedup with absolute-age gate — no effect (burst images all "young"); (3) bounded dedup keeping N newest — reduced symptom, did NOT fix crash. Status: (A) cross-scheduler fix committed (c12052bd) and is a real, verified gain (loading 34-40% → 100%); (B) remains unsolved and MISDIAGNOSED-then-corrected — needs fresh investigation of the actual failing GPU op. Likely shared with Madden NFL 24. | Not tested | Not tested |
| [x] | UFC 3 Patch v1.14 | CUSA06534-patch | Patch folder; not tested independently. | Not tested | Not tested |
| [x] | Tokyo 42 | CUSA07526 | Retested after being restored locally. Capped run reaches deep Unity/Mono startup, loads managed assemblies and `globalgamemanagers`, and continues until the external timeout/termination path rather than a captured emulator crash. Earlier manual test reached in-game visual-novel scenes and appeared to work normally. | Not tested | Not tested |
| [x] | Crash Bandicoot N. Sane Trilogy | CUSA07399 | Retested after image type reuse hardening. The old strict-validation stop where a `Color1D` view reused a cached `Color2D` image is fixed; strict 90s run survives to playtime and manual playtesting reaches gameplay. The game mostly works, but there is occasional flicker and the main character model is not rendered correctly: eyes/head/body pieces appear separated or missing while the environment, crates, shadows, and UI render well. Live log breadcrumbs during gameplay show repeated `Geometry shader stage unsupported, skipping` warnings and some `Skipping draw with no valid render attachments`, so the likely remaining bucket is missing/unsupported geometry-stage or attachment handling rather than the old image-view compatibility crash. | Not tested | Not tested |
| [x] | OKAMI HD | CUSA08364 | Tested. Boots through Vulkan startup and begins loading/rendering real assets, including CRI audio/movie/save data (`prologue_4k.usm`, `okami_hanko_44k.adx`, `idsavecore.idd`, save files). Crashes before a stable playable state with `Unreachable code! Unhandled access violation ... Read from address 0x38`. | Not tested | Not tested |
| [x] | SHADOW OF THE COLOSSUS | CUSA08809 | Retested after the game was restored locally. A longer run previously contributed to a macOS freeze, so it was retested with GPU wait timeout/retry caps. It reaches playtime around 17s, then logs GPU timeline/presenter wait timeouts (`master_semaphore` target tick 21, known tick 20; `get_render_frame_present_done`) and skips VideoOut frames after retry exhaustion. Current bucket: risky GPU wait/presenter stall; avoid uncapped long runs until fail-fast handling is stronger. | Not tested | Not tested |
| [x] | Beast Quest | CUSA09052 | Tested. Starts, plays audio/narrator, then reaches a black screen similar to the UFC black-screen bucket. Also showed startup flicker/distortion. | Not tested | Not tested |
| [x] | RESIDENT EVIL 2 | CUSA09171 | Retested with the clean current x86_64 build after removing the stale `build/` binary trap. The old fixed direct-memory map crash at `0x2000000000` is gone: those mappings now relocate out of the macOS GPU-reserved hole and still report the original guest address. Manual run now reaches a silent black screen with no audio/graphics. Log evidence shows `sceVideoOutOpen` and `sceVideoOutGetResolutionStatus`, but no `sceVideoOutRegisterBuffers`, flips, or audio API activity before timeout; playtime still advances. Current bucket: pre-presentation/pre-audio startup stall, not the old VMA crash and not yet a compositor black-frame case. | Not tested | Not tested |
| [-] | YAKUZA 6: The Song of Life | CUSA09660 | Retested after shader stencil export support. The old `EmitSetAttribute: Unreachable code! Write attribute StencilRef` crash is fixed. Manual playtest reached gameplay and appears to work fine. Remaining notes: occasional texture-cache range ambiguity warnings during copy/image matching. | Not tested | Not tested |
| [x] | SEGA Mega Drive Classics | CUSA09771 | Tested. Reaches the "press button to continue" screen, then enters a black-screen state while audio/game logic continues. No strict render-validation assert was captured in this run; log is dominated by repeated metadata texture-read warnings and net stub spam. | Not tested | Not tested |
| [x] | Biomutant | CUSA09848 | Retested after being restored locally. It survives the short capped run without crashing, but the user observed a plain black screen with no sound or apparent behavior. Log tail is mostly repeated memory-pool commits. Current bucket: boots/runs but no visible/audio progress. | Not tested | Not tested |
| [-] | The Witch and the Hundred Knight 2 | CUSA10135 | Retested after NGS2 hardening and AT9 waveform decoding/mixing. Previously crashed after logo/audio loading in `PhyreEngineWorkerThread` from an uninitialized/bogus audio metadata path. Now appears to work fine during manual play; audio works and the old crash was not reproduced. | Not tested | Not tested |
| [x] | Borderlands: Game of the Year Edition | CUSA10455 | Tested. Boots through Vulkan startup and runs an active VideoOut/render loop with repeated 1920x1080 storage bindings and frame flips. User observed that it appears to work and respond to keypresses, but it starts and continues on a black screen, so this is another game-logic-alive/render-black case. | Not tested | Not tested |
| [-] | YAKUZA KIWAMI 2 | CUSA10634 | Retested after shader stencil export support and buffer/image alias sync hardening. The old `EmitSetAttribute: Unreachable code! Write attribute StencilRef` crash is fixed, and the New Game crash on texel-buffer/image subresource aliasing is fixed by syncing overlapping images back to the aliased buffer. Manual playtest now appears to work fine. Remaining notes: repeated compute SetQueueReg warnings and occasional texture-cache range ambiguity warnings. | Not tested | Not tested |
| [x] | LEFT ALIVE | CUSA11229 | Tested. Gets through many direct-memory archive mappings and opens `masterPS4/GxArchivedFile*.dat`, then the game reports `Stall during rendering at flush syncLabel=7, expectedLabel=8` on `GxRenderThread`; after stubbed `sceGnmDebugHardwareStatus`, it crashes with `Unhandled access violation ... Write to address 0x0`. Current bucket: GNM/render synchronization or missing hardware-status behavior after boot. | Not tested | Not tested |
| [x] | NBA 2K Playgrounds 2 | CUSA13619 | Tested. Loads 80 modules and runs an extended `sceKernelMemoryPoolCommit` ramp, reaching about 25s playtime, but never reaches the renderer: no `sceVideoOutOpen`, no `RegisterBuffers`, no flips, and no shader compilation before it silently stops mid memory-pool-commit with no captured crash/assert. Log noise is mostly `Unimplemented type SCE Module Parameters/Comment/Library Version` loader warnings and `_sceKernelSetThreadAtexit*` stubs. User observed a black screen with no behavior. Current bucket: silent black screen — early startup stall before any VideoOut/render activity (no visible crash). | Not tested | Not tested |
| [x] | The Outer Worlds | CUSA13689 | Tested. User observed a black screen with no behavior, followed by a crash. Log ends in VMM decommit failure: `PoolDecommit: Assertion Failed! Attempted to access invalid address 0x1002000000`. Current bucket: memory-pool/decommit invalid-address bug rather than renderer-only black screen. | Not tested | Not tested |
| [x] | RESIDENT EVIL 3 | CUSA14123 | Tested. Startup performs many fixed direct-memory mappings and relocations out of macOS GPU-reserved ranges, then crashes with `Unhandled access violation ... Write to address 0x0`. Current bucket: early null write after fixed-mapping-heavy startup. | Not tested | Not tested |
| [x] | EA Sports UFC 4 | CUSA14204 | Retested after depth-stencil compatibility hardening. The old Metal/MoltenVK `Depth16Unorm` viewed as `R16Uint` abort is fixed. It reaches the UFC4 loading-tip screen and playtime keeps advancing, but appears stuck during the transition from `loadingScreen_0.mkv` to `homeGenericBackground.mkv`; logs are dominated by repeated DirtySDK `sceNetEpollCreate`/`sceNetEpollControl` activity. Current bucket: menu/movie/network transition stall rather than render-driver crash. | Not tested | Not tested |
| [x] | FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition | CUSA16830 | Tested. Short capped run survived until the 75s external timeout rather than crashing, and manual run reaches the animated `Now Loading` screen. Current issue: loading animation renders and keeps running, playtime advances past five minutes, but it never progresses into gameplay. Log breadcrumbs show Unity/IL2CPP startup with many fallback asset opens from `/archive/mount/point/Media/...` to `/app0/Media/...`, repeated metadata texture reads, repeated signal-30 wakes across `UnityPreload`/save-data/IL2CPP worker threads, repeated `cflatcore.prx` load/start activity, `cflat/us/*.cfd` data reads, and heavy `sceKernelGetModuleInfoForUnwind` activity. Current bucket: Unity/content/plugin loading-progress stall rather than crash or total render failure. | Not tested | Not tested |
| [x] | Blair Witch | CUSA18142 | Tested. Capped launch exited cleanly after startup logs, but manual run produced the same black screen with no audio as the existing silent black-boot bucket. Logs are heavy with NP/toolkit/dialog stubs and direct-memory relocation, with no captured fatal crash in the capped run. | Not tested | Not tested |
| [x] | Crysis Remastered | CUSA18671 | Tested. Boots through Vulkan startup, initializes audio, maps several fixed render/engine memory regions out of the macOS GPU-reserved hole, opens CryEngine-style shader assets, registers 1920x1080 `A2R10G10B10Srgb` VideoOut buffers, then crashes during startup/render initialization with `Unhandled access violation ... Write to address 0x20000800fa7450`. | Not tested | Not tested |
| [x] | Crysis 2 Remastered | CUSA18672 | Tested. Reaches CryEngine startup, opens engine/shader cache paks, initializes audio and render threads, registers 1920x1080 `A2R10G10B10Srgb` VideoOut buffers, then fails remapping a relocated render memory region (`Unable to map 0x4000000 bytes at address 0x264380000`) and crashes with `Unhandled access violation ... Write to address 0x0`. Current bucket: fixed-mapping/remap collision plus null write. | Not tested | Not tested |
| [x] | Crysis 3 Remastered | CUSA18673 | Tested. Similar to Crysis Remastered: boots through Vulkan startup, initializes audio/HLE modules, starts the CryEngine render thread, maps large fixed render memory regions out of the macOS GPU-reserved hole, registers 1920x1080 `A2R10G10B10Srgb` VideoOut buffers, then crashes with `Unhandled access violation ... Write to address 0x2000080117bb60`. | Not tested | Not tested |
| [x] | Zero Strain | CUSA18570 | Retested after macOS fixed-mapping relocation and host sidecar filtering. Previously exited in the fixed-address mapping bucket, then hit a macOS `.DS_Store` directory-iteration crash after relocation. Now gets past both and reaches playtime updates; live manual run exited cleanly. Current state: black screen and no audio, likely the next renderer/audio initialization bucket rather than a launch crash. | Not tested | Not tested |
| [x] | New Super Lucky's Tale | CUSA20302 | Retested after the render-safety hardening batch. The capped run exited cleanly after reaching playtime around `0:01:14` with no crash or GPU wait timeout, and UI/subtitles/audio proceed. The green-frame rendering bug remains: the main scene is still mostly solid green with tiny visible fragments, while overlay text renders correctly. Follow-up run with `SHADPS4_DISABLE_COMPUTE_META_CLEAR_HLE=1` and `SHADPS4_DISABLE_COMPUTE_IMAGE_CLEAR_HLE=1` reached about two minutes of playtime and did not change the bucket. Remaining breadcrumbs: unsupported `B4G4R4A4UnormPack16` image creation plus repeated `Skipping draw with no valid render attachments`, likely useful Unity/render setup clues for the missing scene visuals. | Not tested | Not tested |
| [x] | Race With Ryan Road Trip Deluxe Edition | CUSA23279 | Retested after macOS fixed-mapping relocation hardening. Previously crashed when `sceKernelMapNamedDirectMemory` requested `0x4000000000` inside the Rosetta/Metal reserved hole. Now the Apple relocation path is enabled by default and the precise relocated-pointer fault handler lets the game survive and exit cleanly; live manual run reached playtime updates. Current state: black screen and no audio, likely the next renderer/audio initialization bucket rather than a launch crash. | Not tested | Not tested |
| [-] | Katamari Damacy Reroll | CUSA24361 | Tested. Appears to work/playable during manual play. Strict black-screen watchdog stayed nonblack and no GPU wait timeout or crash was observed. Log is noisy with repeated `Unexpected metadata read by a shader (texture)` warnings, but they do not currently block gameplay. | Not tested | Not tested |
| [x] | Stray | CUSA24899 | Retested after fixing non-Windows `preadv` into guest buffers by reading through a host scratch buffer and copying via emulator memory. The old UE4 `Corrupt pak index detected` blocker is gone: pak footer/index chunks now return full byte counts. It now progresses into asset loading and crashes later in a TaskGraph worker with `Unhandled access violation ... Read from address 0x50676e6f6a614d67` (`gMajongP` bytes), so the next blocker is likely UE4 asset/runtime handling rather than pak index I/O. | Not tested | Not tested |
| [x] | SpongeBob SquarePants: The Cosmic Shake | CUSA30582 | Retested after the guest-buffer `preadv` fix. The old `Corrupt pak index detected` blocker is gone: pak index/content reads now return full byte counts. It now progresses into later UE4 content loading and crashes in a TaskGraph worker with `Unhandled access violation ... Read from address 0x47f551f712e88507`. Same next bucket as Stray: post-pak UE4 asset/runtime handling. | Not tested | Not tested |
| [x] | Teenage Mutant Ninja Turtles: Shredder's Revenge | CUSA30991 | Tested. Works well enough to reach startup, main menu, and gameplay after fixing the flexible-memory/`sceKernelMunmap(0, ...)` quit path. Startup music works; user observed no in-game audio yet. | Not tested | Not tested |
| [x] | Redout 2 | CUSA31411 | Retested after the guest-buffer `preadv` fix. The old UE4 `Corrupt pak index detected` blocker is gone: pak footer/index/content reads from `/app0/redout2/content/paks/pakchunk0-ps4.pak` now return full byte counts. It now progresses into later UE4 content loading and crashes in a TaskGraph worker with `Unhandled access violation ... Read from address 0xfc03d4f0a4b19271`. Same next bucket as Stray/SpongeBob: post-pak UE4 asset/runtime handling. | Not tested | Not tested |
| [x] | Madden NFL 24 | CUSA37089 | Retested after implementing `sceHttpWaitRequest`. Originally hung in an EA online check: the game issues non-blocking HTTPS requests to `rl.data.ea.com` (`/bugsentry/session/`) and `pin-river.data.ea.com` (`/pinEvents`), binds them to a (stubbed) epoll with a null handle, then polled `sceHttpWaitRequest` ~5000 times waiting for completion that the stub never reported. `sceHttpWaitRequest` now scans for non-blocking requests that reached a terminal state (`Sent`/`Aborted`) and reports a completion event (transport-failure bits for the offline path), so the poll loop sees the request finished and proceeds (~2 wait calls instead of ~5000). It now advances much further — past network init into real rendering (compiles shaders, opens VideoOut). New blocker: the GPU dies mid-render. The real error is `[mvk-error] VK_ERROR_OUT_OF_DEVICE_MEMORY: Lost VkDevice after MTLCommandBuffer execution failed (code 9): Invalid Resource (kIOGPUCommandBufferCallbackErrorInvalidResource)`, after which `vk_swapchain.cpp AcquireNextImage` gets `ErrorDeviceLost` and asserts (`Unreachable code`). It happens right as a compute shader (`0x70415071`, `pgm=0xd3524177`) binds and writes the 1920x1080 videoout storage surface, interleaved with `sceKernelMunmap`/remap traffic — so the likely cause is a GPU resource bound to an in-flight Metal command buffer referencing guest memory that was unmapped/remapped while the GPU still used it. Investigated the obvious suspect (`UnmapMemory` -> `FreeImage` -> `DeleteImage`) and ruled it out: image destruction already defers via `scheduler.DeferOperation` until the GPU reaches the current CPU tick, and texture images are device-local (no host-pointer import), so images are torn down GPU-safely. The invalid resource is therefore something subtler (likely a buffer or the videoout compute storage descriptor still pointing at guest pages across an unmap/remap mid-frame). Not blind-patched: pinning the exact rejected resource needs a frame dump + reproduction, and changing the renderer's memory tracking blindly is high regression risk for games that currently render. Current bucket: HTTP online-check stall fixed; now a MoltenVK "Invalid Resource" device-lost crash during the compute-to-videoout present path. | Not tested | Not tested |
| [x] | High on Life | CUSA42391 | Tested. Crashes during startup after resolving many NP/WebApi and NGS2 imports. The immediate blocker is another fixed/no-overwrite direct-memory map at `0x4000000000` for `0x8000000` bytes; `MapMemory` logs that this region is outside usable VMAs, then the game null-writes and aborts. Current bucket: fixed `0x4000000000` mapping relocation gap, same family as several modern/Unity/UE-style titles. | Not tested | Not tested |
| [-] | Gigantosaurus: Dino Sports | CUSA43402 | Tested. Appears to work/playable during manual play, with occasional flicker. No GPU wait timeout, crash, or strict black-screen abort occurred. Watchdog first saw a very dark but nonblack frame (`avg_luma` around 7, `near_black` around 96%, nonblack pixels present), then later bright nonblack frames while Unity-style assets loaded and shaders compiled. Log is noisy with repeated `Unexpected metadata read by a shader (texture)` warnings, but they do not currently block gameplay. | Not tested | Not tested |
| [x] | The Smurfs 2: The Prisoner of the Green Stone | CUSA43623 | Retested after the guest-buffer `preadv` fix. The old UE4 `Corrupt pak index detected` blocker is gone: pak footer/index/content reads from `/app0/sm2/content/paks/sm2-ps4.pak` now return full byte counts. It now progresses into later UE4 content loading, logs `MallocBinned2 Corruption Canary was 0x3941, should be 0x17ea`, then crashes with `Unhandled access violation ... Write to address 0x0`. This points at post-pak UE4 memory/asset-runtime corruption rather than pak index I/O. | Not tested | Not tested |
| [- ] | Another Sight | CUSA15308 | Tested, then removed from folder. Blocked by fixed mapping around `0x4000000000`, which overlaps the macOS x86_64-on-Apple-Silicon reserved address hole. | Not tested | Not tested |
| [ -] | Minecraft Dungeons | CUSA18797 | Tested, then removed from folder. Blocked by the same fixed `0x4000000000` mapping issue. | Not tested | Not tested |
| [ -] | Taxi Chaos | CUSA20527 | Tested, then removed from folder. Blocked by the same fixed `0x4000000000` mapping issue. | Not tested | Not tested |
| [- ] | Severed Steel | CUSA30139 | Tested, then removed from folder. Blocked by the same fixed `0x4000000000` mapping issue. | Not tested | Not tested |

## Tested Issue Categories

Games that appear to work fine are intentionally omitted from this triage section.

### Total or Silent Black Screen

| Game | Title ID | State |
|---|---|---|
| UFC 2 | CUSA01968 | Boots/runs to timeout, but user observed black screen with no behavior. |
| Prison Architect: PlayStation 4 Edition | CUSA03487 | Starts and updates playtime briefly, but user observed no visible image and harsh buzzing audio. |
| RESIDENT EVIL 2 | CUSA09171 | Old fixed-mapping crash is fixed, but current run reaches silent black screen with no audio/graphics; log opens VideoOut and queries resolution but does not register buffers, flip, or hit audio APIs before timeout. |
| Biomutant | CUSA09848 | Boots/runs without short-run crash, but shows plain black screen with no sound or apparent behavior. |
| Blair Witch | CUSA18142 | Manual run produced black screen with no audio. |
| Zero Strain | CUSA18570 | Gets past fixed-mapping and directory issues, then reaches black screen with no audio. |
| Race With Ryan Road Trip Deluxe Edition | CUSA23279 | Gets past fixed-mapping crash and reaches playtime, then black screen with no audio. |
| NBA 2K Playgrounds 2 | CUSA13619 | Reaches ~25s playtime but never opens VideoOut or compiles shaders; silent black screen with no behavior. |
| Madden NFL 24 | CUSA37089 | HTTP online-check stall fixed (implemented `sceHttpWaitRequest`); now advances into rendering and crashes on a MoltenVK `ErrorDeviceLost` during swapchain present. |

### Render Black, But Game/UI/Audio Is Alive

| Game | Title ID | State |
|---|---|---|
| EA Sports UFC | CUSA00264 | Known render black-screen bug; watchdog shows black starts upstream of final presentation. |
| WORLD OF FINAL FANTASY | CUSA04647 | Gameplay/audio/UI overlays work, but main 3D world rendering is mostly black. |
| Beast Quest | CUSA09052 | Audio/narrator starts, then reaches UFC-like black-screen bucket with startup flicker. |
| SEGA Mega Drive Classics | CUSA09771 | Reaches prompt, then black screen while audio/game logic continues. |
| Borderlands: Game of the Year Edition | CUSA10455 | Responds and flips frames, but starts/continues on black screen. |

### Severe Rendering Corruption or Missing Geometry

| Game | Title ID | State |
|---|---|---|
| FINAL FANTASY VII | CUSA01875 | Playable/progresses, but 3D geometry is severely misordered/overlapped with broken scene composition. |
| Yooka-Laylee | CUSA05721 | Boots into scene/menu path with severe flicker, distortion, and missing geometry. |
| Crash Bandicoot N. Sane Trilogy | CUSA07399 | Mostly works, but has occasional flicker and broken/missing main character model pieces. |
| New Super Lucky's Tale | CUSA20302 | UI/subtitles/audio progress, but main scene is mostly solid green with tiny visible fragments. |
| Gigantosaurus: Dino Sports | CUSA43402 | Playable, but has occasional flicker. |

### Loading, Menu, or Transition Stalls

| Game | Title ID | State |
|---|---|---|
| EA Sports UFC 4 | CUSA14204 | Reaches loading-tip screen and playtime advances, but appears stuck during movie/menu transition. |
| FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition | CUSA16830 | Animated loading screen keeps running for minutes, but never progresses into gameplay. |

### Crashes After Boot or During Loading

| Game | Title ID | State |
|---|---|---|
| Tearaway Unfolded | CUSA00562 | Gets into rendering/audio/save activity, then crashes on GPU command processor null read. |
| Joe's Diner | CUSA03774 | Accepts movement input, then crashes after loading-screen transition in MoltenVK buffer barrier path. |
| Rise of the Tomb Raider | CUSA05716 | Fixed mapping is relocated, then guest writes near original fixed region and crashes. |
| OKAMI HD | CUSA08364 | Begins loading/rendering real assets, then crashes on read from `0x38`. |
| LEFT ALIVE | CUSA11229 | Crashes on `GxRenderThread` after a render flush sync-label stall and stubbed `sceGnmDebugHardwareStatus`. |
| The Outer Worlds | CUSA13689 | Black screen followed by VMM decommit invalid-address crash. |
| RESIDENT EVIL 3 | CUSA14123 | Fixed-mapping-heavy startup, then crashes with write to `0x0`. |
| Crysis Remastered | CUSA18671 | Crashes during startup/render initialization after VideoOut registration. |
| Crysis 2 Remastered | CUSA18672 | Fails remapping relocated render memory, then crashes with write to `0x0`. |
| Crysis 3 Remastered | CUSA18673 | Crashes during startup/render initialization after large fixed-memory mappings. |
| Stray | CUSA24899 | UE4 pak index blocker fixed; now crashes later during post-pak asset/runtime loading. |
| SpongeBob SquarePants: The Cosmic Shake | CUSA30582 | UE4 pak index blocker fixed; now crashes later during post-pak asset/runtime loading. |
| Redout 2 | CUSA31411 | UE4 pak index blocker fixed; now crashes later during post-pak asset/runtime loading. |
| High on Life | CUSA42391 | Crashes during startup on fixed/no-overwrite direct-memory map at `0x4000000000`, followed by a null write. |
| The Smurfs 2: The Prisoner of the Green Stone | CUSA43623 | UE4 pak index blocker fixed; now hits post-pak memory/asset-runtime corruption and crashes. |

### GPU Wait or Presenter Stall Risk

| Game | Title ID | State |
|---|---|---|
| SHADOW OF THE COLOSSUS | CUSA08809 | Reaches playtime, then hits GPU timeline/presenter wait timeouts; avoid uncapped long runs. |

### Needs Manual Validation

| Game | Title ID | State |
|---|---|---|
| DRIVECLUB | CUSA00003 | Survived capped run after fixed-mapping fix; needs visual/gameplay assessment. |
| Need for Speed Rivals | CUSA00168 | Old mapping crash no longer reproduces; needs manual visual/gameplay validation. |
| UFC 3 | CUSA06534 | Renders title/loading screens; GPU buffer use-after-frees fixed (scheduler gate + buffer-cache + detile scratch buffer) pushed the MoltenVK device loss from ~15s to ~60s, but a later device loss remains — same bug as Madden NFL 24. |

### Removed or Not Currently Local, But Previously Tested

| Game | Title ID | State |
|---|---|---|
| Another Sight | CUSA15308 | Removed from folder; previously blocked by fixed `0x4000000000` mapping issue. |
| Minecraft Dungeons | CUSA18797 | Removed from folder; previously blocked by fixed `0x4000000000` mapping issue. |
| Taxi Chaos | CUSA20527 | Removed from folder; previously blocked by fixed `0x4000000000` mapping issue. |
| Severed Steel | CUSA30139 | Removed from folder; previously blocked by fixed `0x4000000000` mapping issue. |

### Minor Audio or Gameplay Caveats

| Game | Title ID | State |
|---|---|---|
| Teenage Mutant Ninja Turtles: Shredder's Revenge | CUSA30991 | Reaches startup, menu, and gameplay; startup music works, but in-game audio was not yet heard. |

## List of broken titles on MacOS

Sorted by approximate PS4 storage size, largest to smallest. Sizes are rough and can vary by region, patch, language packs, DLC, and disc vs. digital install.

| Acquired | Approx GB | Game |
|---|---:|---|
| [ ] | 175 | Call of Duty®: Modern Warfare® |
| [ ] | 110 | Gran Turismo® 7 |
| [ ] | 105 | Red Dead Redemption 2 |
| [x] | 100 | Gran Turismo®SPORT |
| [ ] | 100 | Call of Duty®: Black Ops III |
| [ ] | 100 | The Last of Us™ Part II |
| [ ] | 95 | Call of Duty®: Black Ops 4 |
| [ ] | 95 | Call of Duty®: Vanguard |
| [ ] | 90 | FINAL FANTASY VII REMAKE |
| [ ] | 90 | NBA 2K19 |
| [ ] | 90 | Battlefield™ V |
| [ ] | 86 | Grand Theft Auto V |
| [ ] | 80 | Call of Duty®: WWII |
| [ ] | 80 | FINAL FANTASY XV |
| [ ] | 75 | Borderlands® 3 |
| [ ] | 70 | Marvel's Guardians of the Galaxy |
| [ ] | 70 | OUTRIDERS |
| [ ] | 65 | Marvel's Spider-Man |
| [Unavailable ] | ? | The Last of Us™ Remastered |
| [ X] | 44.4 | Star Wars Jedi: Fallen Order™ |
| [ X] | 44.7 | DEATH STRANDING |
| [X ] | 24.1 | Tiny Tina's Wonderlands |
| [Unavailable ] | ? | Destiny |
| [ X] | 65.4 | Call of Duty®: Advanced Warfare |
| [Unavailable ] | ? | Marvel's Spider-Man: Miles Morales |
| [ X] | 38.9 | Battlefield™ 1 |
| [X ] | 39.4 | Battlefield™ Hardline |
| [ Unavailable] | ? | Ghost of Tsushima |
| [ Unavailable] | ? | Uncharted™ 4: A Thief’s End |
| [X ] | 30.5 | The Quarry |
| [ Unavailable] | ? | The Witcher 3: Wild Hunt – Game of the Year Edition |
| [ Unavailable] | ? | MLB® The Show™ 19 |
| [ Unavailable] | ? | God of War |
| [ Unavailable] | ? | Horizon Zero Dawn™ |
| [ Unavailable] | ? | Uncharted: The Lost Legacy™ |
| [ X] | 40.5 | Uncharted: The Nathan Drake Collection™ |
| [ X] | 39.4 | Detroit: Become Human™ |
| [ Unavailable] | ? | KINGDOM HEARTS III |
| [ Unavailable] | ? | NBA 2K14 |
| [X] | 48.0 | NBA 2K26 |
| [X] | 47.0 | NBA 2K25 |
| [x] | 41.2 | SWORD ART ONLINE Alicization Lycoris |
| [ ] | 40 | Call of Duty®: Modern Warfare® Remastered |
| [X ] | 36.2 | Dishonored 2 |
| [X ] | 35.9 | Prey |
| [ Unavailable ] | ? | RESIDENT EVIL RESISTANCE |
| [ X] | 39.3 | KILLZONE™ SHADOW FALL |
| [x] | 37.2 | KNACK 2 |
| [Unavailable] | 36.5 | Fallout 4 |
| [ Unavailable ] | ? | Until Dawn™ |
| [ X] | 35 | South Park™: The Fractured But Whole™ |
| [ X] | 35 | HITMAN™ 2 |
| [ X] | 32 | Crash Bandicoot™ 4: It’s About Time |
| [x] | 30.1 | Sniper Elite 4 |
| [ Unavailable ] | ? | Madden NFL 16 |
| [ X] | 30 | Just Cause 3 |
| [Unavailable ] | 11.3 | Sniper Ghost Warrior 3 |
| [ X] | 30 | SCARLET NEXUS |
| [ X] | 30 | Spyro Reignited Trilogy |
| [X ] | 28 | MediEvil |
| [? ] | 28 | Concrete Genie |
| [X ] | 25.7 | Diablo III: Reaper of Souls – Ultimate Evil Edition |
| [ Unavailable ] | ? | Ratchet & Clank™ |
| [ Unavailable] | ? | RESIDENT EVIL 7 biohazard |
| [x] | 25 | Need for Speed™ |
| [Unavailable] | 23.78 | Tony Hawk's™ Pro Skater™ 1 + 2 |
| [Unavailable] | 18 | Chicken Police |
| [Unavailable] | 16 | ASTRO BOT Rescue Mission |
| [Unavailable] | 15 | Moss |
| [x] | 13 | Tony Hawk's™ Pro Skater™ 3 + 4 |
| [x] | 12 | FINAL FANTASY XII THE ZODIAC AGE |
| [Unavailable] | 12 | Project Highrise: Architect's Edition |
| [x] | 12 | Resident Evil 4 |
| [x] | 11 | The Witch and the Hundred Knight: Revival Edition |
| [x] | 10 | SEGA Genesis Classics |
| [x] | 6 | Galak-Z |
| [x] | ? | Madden NFL 25 |
| [ Unavailable] | ? | Madden NFL 19 |
| [ Unavailable] | ? | Madden NFL 20 |
| [x] | ? | Final Fantasy X/X-2 HD Remaster |
| [Unavailable] | ? | Overwatch: Origins Edition |
| [Unavailable] | ? | HORROR TALES: The Wine |
