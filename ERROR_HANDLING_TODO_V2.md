# GPU / emulator error-handling backlog — v2 (2026-06-01)

Re-prioritized after the UFC 3 device loss was fixed by the **freed-image reuse pool**
(`47e3e36d`). The live blockers are now: (a) depth↔color aliasing **render corruption**
(octagon shows only a clear color — draws skipped/surfaces uninitialized), (b) an **intermittent
black-on-boot / no-audio** early failure, and (c) **reuse-pool stale-content safety**. ROI =
how directly each catches/fixes a *current* failure class vs cost and false-positive risk.

Severity model: **warn always** (cheap, greppable), **assert under `SHADPS4_GPU_VALIDATION=assert`
/ `SHADPS4_STRICT_RENDER_VALIDATION`**. Checks are pure (never alter a command/resource) unless
explicitly a fix. Logging now goes through the content-dedup sink (`bcec708d`), so high-volume
warns no longer flood.

Legend: `[ ]` todo, `[~]` partial, `[x]` done.

## TIER 0 — Image reuse pool correctness & safety (new code, highest risk surface)
1. [ ] Optionally clear/zero a recycled image on acquire (env-gated `SHADPS4_IMAGE_REUSE_POOL_CLEAR`) to test whether stale content causes the octagon/black-boot corruption.
2. [ ] Assert a pooled image is GPU-idle when released (its owning Image's last-use tick is GPU-complete) — catches a release that skipped the deferred gate.
3. [ ] Track + log pool stats (hits, misses, evictions, live bytes) under a flag; warn on pathological miss rates (key churn) or runaway live bytes.
4. [ ] Validate the reused image's create-info byte-exactly matches the request (already keyed, but assert on the full vk::ImageCreateInfo to rule out a key field omission).
5. [ ] Never pool images with external/shared memory or dedicated VideoOut backings; assert such images take the destroy path.
6. [ ] Cap pool entries per-key (not just global) so one churning key can't evict everything else useful.
7. [ ] Drain-on-pressure: when VMA reports budget pressure, drain the pool before failing an allocation.
8. [ ] Verify the pool is fully drained at shutdown before `vmaDestroyAllocator` (assert pool empty in a debug build).
9. [ ] Make the pool reuse deterministic for the same address surface (prefer the most-recently-released entry of a key) to maximize content-stability for persistent surfaces.
10. [ ] Add a poison-on-evict check: assert an evicted VkImage handle is not still referenced anywhere (bind-after-evict tripwire).

## TIER 0 — Depth↔color aliasing render correctness (the octagon corruption)
11. [ ] Count + warn (rate-limited) when a draw is skipped via `HasValidRenderAttachment` returning false, naming which attachment(s) were invalid and the pgm hash — pin which pass produces the clear-color-only frame.
12. [ ] Log the full render-target set (formats, layouts, image ids, valid/invalid) at the first skipped draw of each frame — diagnostic for the octagon.
13. [ ] Validate that the "Unimplemented depth overlap copy" path's resulting image is actually consumed before being written; warn if a sampled surface is read while still in Undefined/uninitialized layout.
14. [ ] Implement the missing MSAA depth→color (and color→depth) reinterpret copy/resolve so the recreated surface has correct contents instead of garbage.
15. [ ] Detect a render target bound with format incompatible with the bound pipeline's color/depth format and warn (the depth-format mismatch class) — needs the pipeline's real createInfo formats exposed.
16. [ ] Warn when a color attachment's image is actually a depth image (or vice-versa) at BeginRendering.
17. [ ] Validate attachment extents are mutually consistent (all >= render area) and warn on mismatch that would clip the scene.
18. [ ] Track per-frame count of draws issued vs skipped; warn when >K% of draws in a frame are skipped (scene not rendering).
19. [ ] Verify the depth target's htile/meta state is consistent after a depth↔color recreate (stale htile → wrong depth test → skipped/black geometry).
20. [ ] Warn when a surface is sampled as a texture in the same pass it is bound as a render/depth target without a feedback-loop layout.

## TIER 0 — Intermittent black-on-boot / no-audio diagnostics
21. [ ] Early-boot watchdog: if N seconds pass after `sceVideoOutOpen` with no successful flip presenting non-blank content, log a structured "boot stalled" dump (last GPU op, last kernel call, audio state).
22. [ ] Log the first N frames' present results + whether the presented image was blank/cleared vs had drawn content (cheap luminance/variance probe).
23. [ ] Detect and warn when audio init (`sceAudioOutOpen`/Ngs2) does not occur within a boot window — distinguishes "no audio" from "audio failed".
24. [ ] Capture and log any early exception/abort on non-GPU threads (Ajm, AvPlayer, MoviePlayer2) with thread + context instead of a bare terminate.
25. [ ] A/B harness: a single env to disable the reuse pool (`SHADPS4_IMAGE_REUSE_POOL=0`) — already present; document + log which mode is active at boot.
26. [ ] Determinism probe: log a hash of the first presented frame's content so black vs non-black boots are machine-distinguishable across runs.
27. [ ] Warn if the first VideoOut buffer registered is never written by the GPU before its first flip (presenting uninitialized memory = black).
28. [ ] Track whether the black-boot correlates with a specific shader compile failure or a skipped first draw (join with #11/#18).

## TIER 1 — Render-attachment & pipeline validity
29. [ ] Pipeline attachment formats match BeginRendering attachments (expose GraphicsPipeline createInfo formats; assert the depth-format mismatch class).
30. [ ] Validate the bound depth image's aspect (depth/stencil) matches what the pipeline + draw expect.
31. [ ] Warn on topologies/primitive types MoltenVK software-emulates (perf + correctness).
32. [ ] Validate index type (16/32-bit) matches the bound index buffer's element size.
33. [ ] Viewport/scissor within framebuffer; render area <= min attachment extent (extend the non-negative check already added).
34. [ ] Validate color blend / write-mask state vs attachment count.
35. [ ] Assert no draw is recorded outside an active render pass (begin/end rendering balance).
36. [ ] Warn when a pipeline is compiled with a depth pixel format but no depth attachment is bound (the Metal setRenderPipelineState assert class).
37. [ ] Validate fetch-shader vertex-attribute offsets/strides within the bound vertex buffers.

## TIER 1 — Descriptor & binding validity
38. [ ] Every UBO/SSBO descriptor range <= backing buffer size (done #39 earlier — re-verify after pool changes).
39. [ ] Sampled images in a shader-read-compatible layout at bind (not Undefined/TransferDst) — warn; high signal for the octagon.
40. [ ] Storage images bound with eStorage usage + storage-compatible format on MoltenVK.
41. [ ] Detect a depth image bound as a storage image (Metal forbids) — warn/skip.
42. [ ] Descriptor counts <= pool/layout limits before pushDescriptorSet.
43. [ ] Push-constant size <= maxPushConstantsSize and the pipeline's declared range.
44. [ ] Assert no descriptor binds a resource queued for deferred destroy this frame (partial via #14/#45 earlier — extend to image views).
45. [ ] Sampler/vertex-buffer bindings reference live resources with consistent stride.
46. [ ] Validate the shader's declared storage-image format matches the bound image format.
47. [ ] Warn on a null bound image view that is NOT a legitimate null-descriptor (refine the existing null-view warn).

## TIER 2 — Image/buffer state, layout & format
48. [ ] Central Image::ValidateState() (layout != Undefined when sampled/attached, backing non-null, samples consistent) called at bind.
49. [ ] Validate layout transitions are legal (source layout matches the image's tracked actual layout).
50. [ ] Format-substitution guard: host != guest format -> assert usage is compatible (the D16->D32 class).
51. [ ] Depth/stencil plane-size correctness for copies (asserted in the diagnostic; assert at source too).
52. [ ] Image extent <= maxImageDimension2D/3D at creation.
53. [ ] num_samples supported for the format+usage (already queried; assert the chosen sample count is supported).
54. [ ] guest_size == sum of mips_layout (catches a malformed ImageInfo).
55. [ ] Color-attachment-and-texture-in-same-pass uses feedback-loop layout (or warn) — see #20.
56. [ ] Buffer usage flags include what the bind requires.
57. [ ] image_view subresource subset of image range (done #57 earlier; keep).
58. [ ] Tiling-mode/array-mode consistency before image reuse (now also before pool reuse).

## TIER 2 — Texture-cache & aliasing invariants
59. [ ] Warn when >K images are registered at one guest address simultaneously (aliasing storm gauge).
60. [ ] Assert ResolveDepthOverlap recreate frees/pools exactly one old image per new.
61. [ ] Count + warn on the "Unimplemented depth overlap copy" path (done — routed through ReportOnce + dedup; keep as a frame-rate gauge).
62. [ ] "Resolved to too-few-resources" path -> always-warn.
63. [ ] Warn on depth<->color reinterpret where bit-widths differ (likely-wrong reinterpret).
64. [ ] GC/pool must not reclaim an image with tick_accessed_last == CurrentTick.
65. [ ] Page-table consistency: each registered image's pages map back to it.
66. [ ] ExpandImage copy succeeded (or warn) — the size-ratchet path.
67. [ ] Warn on per-frame full re-upload of GpuModified surfaces.
68. [ ] Detect depth-as-texture reads early and route through a stable non-churning path (the pool now reduces churn; verify correctness).

## TIER 3 — Memory, allocation & mapping
69. [ ] Check every vmaCreate*/createImage result (done at all sites; re-verify the pool's reuse path can't return a stale/invalid handle).
70. [ ] ObtainBuffer* returns a buffer covering [addr, addr+size); warn on partial coverage (the over-read class).
71. [ ] Validate memory->IsValidMapping before host CopySparseMemory in the staging path.
72. [ ] Track device memory used vs budget (incl. pooled idle images); warn approaching it.
73. [ ] staging_buffer.Map wrap warning.
74. [ ] Assert GDS/null/fault buffers are never freed/pooled.
75. [ ] Descriptor buffer alignment (minUniformBufferOffsetAlignment, etc.).
76. [ ] Make the buffer_cache "destination aliases cached image" warning actionable (count/throttle — now deduped).

## TIER 3 — Pipeline, shader & PM4 stream
77. [ ] Check graphics/compute pipeline creation results; log shader hash on failure.
78. [ ] Shader resource bindings match pipeline-layout binding count/types.
79. [ ] Validate PM4 packet sizes/opcodes in the command processor (the type-0/type-1 desync class seen in other titles).
80. [ ] Register-state coherence checks (depth_enable vs bound depth target) — links to #36.
81. [ ] Fetch-shader/vertex-attribute offsets within the vertex buffer (see #37).
82. [ ] Heuristic warn on shaders with huge push-constant-driven loop bounds.
83. [ ] Validate compute local-size declaration vs dispatch assumptions (done #29; keep).
84. [ ] Validate render-pass attachment count vs pipeline colorAttachmentCount.

## TIER 3 — Kernel / libraries / audio / CPU
85. [ ] AjmWorker: replace the "Unreachable code" abort with a logged, recoverable error dumping codec/instruction (movie-audio crash).
86. [ ] Bounds-check AJM decode in/out buffers; validate codec params.
87. [ ] Catch the Rosetta synchronous-exception path; log x86 RIP + context instead of aborting.
88. [ ] Throttle FS "open() failed" spam to once-per-path (now also deduped globally; keep path-keyed counter).
89. [ ] Second sceVideoOutOpen: explicitly support or cleanly reject.
90. [ ] Validate AvPlayer/Videodec/MoviePlayer2 stub outputs don't feed garbage downstream (the movie path).
91. [ ] Bounds-check kernel mmap/munmap ranges against the guest address space.
92. [ ] Validate equeue/thread handles before use.
93. [ ] SIGSEGV/SIGABRT watchdog dumping the last GPU command + last kernel call (extend the ring dump to signal handlers).

## TIER 4 — Diagnostics infrastructure (force-multipliers)
94. [ ] Plumb all real device limits into host_diagnostics.h (dispatch ceiling raised; full plumbing pending).
95. [ ] GPU-op ring (done, 512 entries, dumps on device loss) — add per-op subsystem tag (RefreshImage/Detile/FSR/Present) and pool hit/miss.
96. [ ] Rate-limit every check (done) + content-dedup logging (done) — extend dedup with an optional periodic "repeated N×" summary line.
97. [ ] One env `SHADPS4_GPU_VALIDATION=off|warn|assert` controlling all checks (done; document).
98. [ ] Tag each GPU command with its originating subsystem in logs (links #95).
99. [ ] Add "ticks since last successful present" + "frames with drawn content" to the device-loss/boot-stall log.
100. [ ] Env-gated one-shot Metal frame-capture trigger at a target bind count or on first skipped-draw — capture the octagon frame for offline analysis.
