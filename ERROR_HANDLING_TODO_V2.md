# GPU / emulator error-handling backlog — v2 (2026-06-01, refreshed)

Re-prioritized after the **image-churn device loss was fixed by the freed-image reuse pool**
(`47e3e36d`) and a diagnostic run pinned two *new, confirmed* live blockers:

- **(B) Residual intermittent device loss, NEW character.** The GPU-op ring dump on the latest loss
  shows **zero image frees** in the last 512 ops (image churn is gone) — instead **474
  `copy_buffer_sync` of 16 KB buffers**, a few draws/dispatches, **one `FREE buffer`**
  (`0x284628000`, 16 KB), loss on **"Command Buffer 4"** detected at `swapchain_acquire`. So the same
  churn class likely shifted to the **buffer** side (or a distinct cmd-buffer issue). Still
  intermittent (this run crashed ~25 s; another reached the intro movie).
- **(A) Octagon/menu render corruption.** Color **render-target lookup returns null** for
  `0x280320000` (1920×1080 R8G8B8A8) and `0x281140000` (1600×900) → draws skipped → scene shows only
  its clear color. NOT an overlap-resolve failure; the descriptor `image_id` is simply null at
  `BeginRendering`.

ROI = how directly each catches/fixes a *current* failure class vs cost and false-positive risk.
Severity: **warn always**, **assert under `SHADPS4_GPU_VALIDATION=assert`/`SHADPS4_STRICT_RENDER_VALIDATION`**.
Checks are pure unless explicitly a fix. Logging goes through the content-dedup sink (`bcec708d`).
Legend: `[ ]` todo, `[~]` partial, `[x]` done.

## TIER 0A — Residual device loss B (buffer-side churn / command buffer)
1. [x] Apply the reuse-pool idea to BUFFERS: recycle UniqueBuffer (VkBuffer+alloc) by exact create-info, same as images, to kill small-buffer vmaCreate/Destroy churn.
2. [x] Count + warn on per-frame buffer create/destroy churn per size-class (16 KB stream/util buffers dominate the ring); gauge whether buffer churn drives the loss.
3. [x] Tag each `copy_buffer_sync`/`copy_buffer_download` ring entry with the source subsystem (stream/util/uniform/GDS) so the 474/frame flood is attributable.
4. [x] On device loss, log WHICH command buffer index failed + map it to the scheduler (draw/present/flip) and its in-flight tick range.
5. [x] Record buffer FREEs in the ring already (done) — also record buffer CREATEs so a create/free pair at the same addr within N ops is flagged (churn signature).
6. [x] Validate `BufferCache::SynchronizeBuffer`'s src buffer size (the one copy site still unchecked — src is a raw handle) by threading the size through.
7. [x] Warn when the deferred-destroy queue for buffers exceeds a threshold (mirror the image gauge) — buffer free-churn detector.
8. [x] Add "ticks since last successful present" + failing-tick to the swapchain-acquire device-loss dump (present path has no tick log yet, unlike master_semaphore).
9. [x] Quarantine experiment (env-gated): defer buffer frees an extra K submits; if the loss recedes, it is buffer-free timing; if not, it is elsewhere.
10. [x] Check whether the loss correlates with a specific dispatch (e.g. `dispatch x=240 y=135` = 1080p detile/compute) referencing a just-freed/recycled buffer.

## TIER 0B — Octagon/menu render corruption A (null color render target)
11. [x] Name which color attachment is null + addr/format at BeginRendering, and which pipeline skips (mrt_mask) — done (`280befc1`); drove the diagnosis.
12. [x] Trace WHY `cb_descs[cb].image_id` is null: instrument `PrepareRenderState` / `FindRenderTarget` for `0x280320000`/`0x281140000` to log the lookup path that returns null.
13. [x] Warn when `FindImage`/`FindRenderTarget` returns null for a color buffer with a valid address (distinguish "no RT bound" from "RT lookup failed").
14. [x] Validate the color buffer's ImageInfo (format/extent/tiling) is well-formed before lookup; warn on a degenerate desc that can't resolve.
15. [x] Check if the null RT addr was recently freed-to-pool and not re-found (pool interaction with RT lookup) — rule the pool in/out for A.
16. [x] Log the full render-target set (all cb formats/ids/layouts + depth) at the first skipped draw of each frame.
17. [x] Per-frame skipped-vs-issued draw ratio; warn when >K% skipped (scene not rendering).
18. [x] Detect a color attachment whose image exists but whose view creation failed (null view vs null image — different bug).
19. [x] Verify htile/meta state after a depth↔color recreate (stale htile → wrong depth → skipped geometry).
20. [x] Env-gated one-shot capture request on first skipped-draw to grab the corrupt frame offline.

## TIER 0C — Image reuse pool correctness & safety (new code)
21. [x] Env-gated clear/zero of a recycled image on acquire to test stale-content as a cause of A / black-boot.
22. [ ] Assert a pooled image is GPU-idle on release (owning tick GPU-complete).
23. [x] Pool stats (hits/misses/evictions/live bytes) under a flag; warn on pathological miss rate or runaway bytes.
24. [x] Never pool external/shared-memory or dedicated VideoOut backings; assert they take the destroy path.
25. [x] Per-key entry cap so one churning key can't evict everything useful.
26. [x] Drain-on-budget-pressure before failing an allocation.
27. [x] Assert pool empty at shutdown (debug) before `vmaDestroyAllocator`.
28. [x] Prefer most-recently-released entry of a key (content stability for persistent surfaces).
29. [x] Poison-on-evict tripwire: evicted handle must not still be referenced.
30. [ ] Verify reused image's full create-info matches request byte-exactly (rule out a key field omission).

## TIER 1 — Intermittent black-on-boot / no-audio
31. [ ] Early-boot watchdog: N s after `sceVideoOutOpen` with no non-blank flip → structured "boot stalled" dump.
32. [ ] Cheap luminance/variance probe per presented frame; log blank vs drawn.
33. [ ] Warn if audio init (`sceAudioOutOpen`/Ngs2) doesn't occur within the boot window.
34. [ ] Catch early non-GPU-thread aborts (Ajm/AvPlayer/MoviePlayer2) with thread+context, not bare terminate.
35. [ ] Log which reuse-pool mode is active at boot (A/B clarity).
36. [ ] Hash the first presented frame so black vs non-black boots are machine-distinguishable.
37. [ ] Warn if the first VideoOut buffer is flipped before the GPU writes it (presenting uninitialized = black).
38. [ ] Join black-boot with first skipped-draw / first shader-compile failure.

## TIER 1 — Render-attachment & pipeline validity
39. [ ] Pipeline attachment formats match BeginRendering attachments (expose GraphicsPipeline createInfo formats; the depth-format mismatch class).
40. [ ] Validate bound depth image aspect matches pipeline/draw expectation.
41. [ ] Warn when a pipeline declares a depth pixel format but no depth attachment is bound (Metal setRenderPipelineState assert class).
42. [ ] Index type (16/32-bit) matches bound index buffer element size.
43. [ ] Viewport/scissor within framebuffer; render area <= min attachment extent (extend the non-negative check).
44. [ ] Assert begin/end-rendering balance; no draw outside an active pass.
45. [ ] Validate fetch-shader vertex-attribute offsets/strides within bound vertex buffers.
46. [ ] Warn on topologies MoltenVK software-emulates.
47. [ ] Color blend / write-mask state vs attachment count.

## TIER 2 — Descriptor & binding validity
48. [x] UBO/SSBO descriptor range <= backing buffer size (done #39).
49. [ ] Sampled images in shader-read-compatible layout at bind (not Undefined/TransferDst) — high signal for A.
50. [ ] Storage images bound with eStorage usage + storage-compatible format on MoltenVK.
51. [ ] Detect a depth image bound as a storage image (Metal forbids) — warn/skip.
52. [ ] Descriptor counts <= pool/layout limits before pushDescriptorSet.
53. [ ] Push-constant size <= maxPushConstantsSize and pipeline range.
54. [~] No descriptor binds a resource queued for deferred destroy (partial via #14/#45; extend to image views).
55. [ ] Sampler/vertex-buffer bindings reference live resources with consistent stride.
56. [ ] Shader's declared storage-image format matches bound image format.
57. [~] Warn on a null bound image view that isn't a legitimate null-descriptor (done; refine).

## TIER 2 — Image/buffer state, layout & format
58. [ ] Central Image::ValidateState() (layout != Undefined when sampled/attached, backing non-null) at bind.
59. [ ] Validate layout transitions legal (source matches tracked actual layout).
60. [ ] Format-substitution guard: host != guest -> usage compatible (D16->D32 class).
61. [ ] Depth/stencil plane-size correctness for copies (assert at source too).
62. [ ] Image extent <= maxImageDimension2D/3D at creation.
63. [ ] guest_size == sum of mips_layout.
64. [ ] Color-attachment-and-texture-in-same-pass uses feedback-loop layout (or warn).
65. [ ] Buffer usage flags include what the bind requires.
66. [x] image_view subresource subset of image range (done #57).
67. [ ] Tiling/array-mode consistency before image AND pool reuse.
68. [ ] num_samples supported for format+usage (assert chosen sample count).

## TIER 3 — Texture-cache & aliasing invariants
69. [x] Warn when >K images registered at one guest address simultaneously (aliasing-storm gauge at RegisterImage, >=16).
70. [ ] Assert ResolveDepthOverlap recreate frees/pools exactly one old per new.
71. [x] "Unimplemented depth overlap copy" routed through ReportOnce+dedup (done; keep as gauge).
72. [ ] "Resolved to too-few-resources" path -> always-warn.
73. [ ] Warn on depth<->color reinterpret where bit-widths differ.
74. [x] GC/pool must not reclaim an image with tick_accessed_last == CurrentTick (pool only releases post-gate).
75. [ ] Page-table consistency: each registered image's pages map back to it.
76. [ ] ExpandImage copy succeeded (or warn) — the size-ratchet path.
77. [ ] Detect depth-as-texture reads early; verify the pool-stabilized path is correct.

## TIER 3 — Memory / pipeline / PM4
78. [x] Check every vmaCreate*/createImage result (done; re-verify the pool reuse path can't return a stale handle).
79. [ ] ObtainBuffer* covers [addr, addr+size); warn on partial coverage (over-read class).
80. [ ] Track device memory used vs budget incl. pooled idle images; warn approaching it.
81. [ ] Assert GDS/null/fault buffers are never freed/pooled.
82. [ ] Check graphics/compute pipeline creation results; log shader hash on failure.
83. [ ] Shader resource bindings match pipeline-layout binding count/types.
84. [ ] Validate PM4 packet sizes/opcodes in the command processor.
85. [ ] Register-state coherence (depth_enable vs bound depth target).

## TIER 4 — Kernel / audio / CPU
86. [ ] AjmWorker: replace "Unreachable code" abort with a logged recoverable error dumping codec/instruction (movie-audio crash).
87. [ ] Bounds-check AJM decode in/out buffers; validate codec params.
88. [ ] Catch the Rosetta synchronous-exception path; log x86 RIP+context instead of aborting.
89. [ ] Validate AvPlayer/Videodec/MoviePlayer2 stub outputs don't feed garbage downstream.
90. [ ] Bounds-check kernel mmap/munmap ranges against guest address space.
91. [ ] Validate equeue/thread handles before use.
92. [ ] SIGSEGV/SIGABRT watchdog dumping last GPU command + last kernel call.
93. [ ] Second sceVideoOutOpen: explicitly support or cleanly reject.

## TIER 5 — Diagnostics infrastructure (force-multipliers)
94. [x] GPU-op ring, dumps on device loss, 512 entries, frees+copies (done) — add subsystem tags + pool hit/miss.
95. [x] Rate-limit every check + content-dedup logging (done) — add optional "repeated N×" summary on the deduped line.
96. [x] One env `SHADPS4_GPU_VALIDATION=off|warn|assert` (done; document).
97. [ ] Plumb all real device limits into host_diagnostics.h.
98. [ ] Tag each GPU command with originating subsystem in logs (RefreshImage/Detile/FSR/Present).
99. [ ] Add "frames with drawn content" + "skipped-draw ratio" to the boot-stall/device-loss log.
100. [ ] Env-gated one-shot Metal frame-capture at a target bind count or on first skipped-draw / device loss.
