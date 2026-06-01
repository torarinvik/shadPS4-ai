# GPU / emulator early-error-handling backlog

Prioritized by ROI for *this* codebase: how directly each catches the failure classes we've
actually hit (device loss / use-after-free / bounds / churn) vs. cost and false-positive risk.
Tier 1 targets the active intermittent device-loss bug.

Severity model for every check: **warn always** (cheap, greppable in normal runs), **hard-assert only
under `SHADPS4_STRICT_RENDER_VALIDATION`**. Checks are pure (log/assert only; never alter a command
or resource) so they cannot regress rendering.

Status legend: `[ ]` todo, `[~]` partial, `[x]` done.

## TIER 1 — Resource lifetime & use-after-free (highest ROI — the live bug)
1. [ ] Tag every buffer/image with a per-scheduler "last-bound tick" when bound into a cmdbuf; on destroy assert it is GPU-complete on ALL schedulers. (Would name the current intermittent race.)
2. [ ] Single chokepoint around vmaDestroyBuffer/vmaDestroyImage; under a flag, validate no in-flight cmdbuf references it before freeing.
3. [ ] Freed-handle poison list kept N frames; assert on bind that a handle isn't poisoned (bind-after-free).
4. [x] Per-scheduler deferred-destroy queue-depth gauge; warns (rate-limited) when pending_ops exceeds 1024 (free-churn/stalled-drain). WarnDeferredQueueDepth, kept out of the hot header.
5. [x] Count image recreates per guest address (NoteImageRecreate at ResolveDepthOverlap); warn once past 64 recreates (churn driver). (todo: also wire ExpandImage.)
6. [x] Warn on a monotonically growing recreate-size ratchet at one address (8 consecutive growths) via NoteImageRecreate.
7. [ ] SlotVector double-free guard: each slot id erased exactly once; add a generation counter validated at submit.
8. [x] Frees recorded into the GPU-op ring with their registration tick (DeleteImage/DeleteBuffer), dumped on device loss; the device-loss failure path now logs target_tick/known_gpu_tick/current_tick to compare against FREE reg_tick entries.
9. [ ] Assert DeleteImage/DeleteBuffer resource is Unregistered before the deferred destroy is queued.
10. [~] MasterSemaphore invariant: known GPU tick never exceeds logical current_tick (warn in Refresh). (todo: NextTick never returns <= KnownGpuTick - hot inline path, deferred.)
11. [ ] In PopPendingOperations, assert a fired op's gpu_tick < current submitted tick.
12. [ ] Assert detile scratch isn't freed before the consuming image.Upload is submitted.
13. [ ] StreamBuffer ring: warn when an allocation wraps onto an offset still referenced by an in-flight tick.
14. [x] Warn when a bound ImageView's parent image lacks the Registered flag (unregistered/pending-destroy) at BindTextures.
15. [ ] Validate the 3 schedulers each registered exactly 2 siblings at first submit.

## TIER 1 — GPU copy / transfer bounds
16. [x] Buffer->image upload bounds. (extend to stencil-only-aspect copies)
17. [x] Image->buffer download bounds. (verify against true dst buffer size)
18. [x] Image->image copyImage: validate src+dst subresource (mip/layer) ranges at Image::CopyImage.
19. [~] Buffer->buffer bounds at buffer_cache copy sites (done: Copy, ExpandOverlap, Download; todo: SynchronizeBuffer - source is a raw handle without a size).
20. [x] fillBuffer bounds (Buffer::Fill: offset+size <= buffer size; %4 already asserted).
21. [ ] blitImage: regions within extents; filter/format compatibility.
22. [~] clearColorImage subresource range in bounds at Image::Clear (CheckSubresourceRange; todo: depth/stencil clear + layout legality).
23. [ ] Validate bufferOffset alignment for copyBufferToImage/copyImageToBuffer (Metal per-format alignment).
24. [ ] Validate copy aspect masks match the image's actual aspect.
25. [ ] Detile/tile descriptor ranges <= actual buffer size.
26. [ ] Validate TilingElementCount/DispatchGroups against the real bound source-buffer size.
27. [~] Centralize CheckImageSubresources at every copy site (done: Upload, Download, CopyImageWithBuffer).

## TIER 1 — Dispatch & draw parameter sanity
28. [~] Plumb the real device maxComputeWorkGroupCount into CheckDispatch (raised ceiling to 1<<28; still want the true per-device limit).
29. [x] Validate compute local workgroup size (num_thread x*y*z) <= maxComputeWorkGroupInvocations (1024) via CheckWorkgroupSize at the dispatch site.
30. [x] Index buffer bounds: bound range (offset + num_indices*index_size) <= backing buffer size at BindIndexBuffer.
31. [ ] Vertex/instance counts non-absurd; vertex offset within buffer.
32. [~] Indirect draw: args buffer (base + stride*max_count) and count buffer (count_base + 4) in bounds at DrawIndirect (todo: indirect dispatch site).
33. [~] Viewport/scissor non-negative warn at UpdateViewportScissorState (todo: within-framebuffer / render-area <= min attachment extent).
34. [ ] Pipeline attachment formats match BeginRendering attachments. NOTE: needs the pipeline's real createInfo depth/color format exposed (recomputing via GetSupportedFormat would false-positive on every legitimate format substitution). Defer until GraphicsPipeline exposes its bound attachment formats.
35. [ ] Log which render target failed to resolve when a draw is skipped for "no valid render attachments".
36. [ ] Validate index type (16/32-bit) matches the bound index buffer.
37. [ ] Warn on topologies MoltenVK software-emulates.

## TIER 2 — Descriptor & binding validity
38. [~] BindTextures: warn when a bound image view handle is null (CheckDescriptor; todo: pending-destroy check needs lifetime tracking from #1/#45).
39. [x] Every UBO/SSBO descriptor range <= backing buffer size (BindUBO/BindSSBO at the ObtainBuffer bind path).
40. [ ] Sampled images in a shader-read-compatible layout at bind (not Undefined/TransferDst).
41. [ ] Storage images bound with eStorage usage + storage-compatible format on MoltenVK.
42. [ ] Detect a depth image bound as a storage image (Metal forbids) - warn/skip.
43. [ ] Descriptor counts <= pool/layout limits before pushDescriptorSet.
44. [ ] Push-constant size <= maxPushConstantsSize and the pipeline's declared range.
45. [~] Warn when a descriptor binds a buffer with is_deleted set (BindBuffer); image side covered by #14. (todo: image-view descriptor pending-destroy beyond the Registered flag.)
46. [ ] Sampler/vertex-buffer bindings reference live resources with consistent stride.
47. [ ] Validate the shader's declared storage-image format matches the bound image format.

## TIER 2 — Image/buffer state, layout & format
48. [ ] Central Image::ValidateState() (layout != Undefined when used, backing non-null, samples consistent).
49. [ ] Validate layout transitions are legal (source layout matches the image's actual layout).
50. [ ] Format-substitution guard: host != guest format -> assert usage is compatible.
51. [ ] Depth/stencil plane-size correctness for copies (asserted in the diagnostic; assert at source too).
52. [ ] Image extent <= maxImageDimension2D/3D at creation.
53. [ ] num_samples supported for the format+usage.
54. [ ] guest_size == sum of mips_layout.
55. [ ] Color-attachment-and-texture-in-same-pass uses feedback-loop layout (or warn).
56. [ ] Buffer usage flags include what the bind requires.
57. [x] image_view subresource subset of image range - oob now always-warns via ReportOnce (rate-limited; strict still asserts).
58. [ ] Tiling-mode/array-mode consistency before image reuse.

## TIER 2 — Texture-cache & aliasing invariants
59. [ ] Warn when >K images are registered at one guest address simultaneously.
60. [ ] Assert ResolveDepthOverlap recreate frees exactly one old image per new.
61. [x] "Unimplemented depth overlap copy" path (leaves image uninitialized) now routes through ReportOnce - rate-limited + attributable per address (was a 643x flood).
62. [ ] "Resolved to too-few-resources" path -> always-warn (currently strict-only).
63. [ ] Warn on depth<->color reinterpret where bit-widths differ.
64. [ ] GC must not free an image with tick_accessed_last == CurrentTick.
65. [ ] Page-table consistency: each registered image's pages map back to it.
66. [ ] ExpandImage copy succeeded (or warn).
67. [ ] Warn on per-frame full re-upload of GpuModified surfaces.
68. [ ] Detect depth-as-texture reads early and route through a stable non-churning path.

## TIER 3 — Memory, allocation & mapping
69. [x] Check every vmaCreate*/createImage result (already asserted at all 4 sites: buffer.cpp, image.cpp, tile_manager.cpp, vk_presenter.cpp).
70. [ ] ObtainBuffer* returns a buffer covering [addr, addr+size); warn on partial coverage.
71. [ ] Validate memory->IsValidMapping before host CopySparseMemory in the staging path.
72. [ ] Track device memory used vs budget; warn approaching it.
73. [ ] staging_buffer.Map wrap warning.
74. [ ] Assert GDS/null/fault buffers are never freed.
75. [ ] Descriptor buffer alignment (minUniformBufferOffsetAlignment, etc.).
76. [ ] Make the buffer_cache "destination aliases cached image" warning actionable (count/throttle).

## TIER 3 — Pipeline, shader & PM4 stream
77. [ ] Check graphics/compute pipeline creation results; log shader hash on failure.
78. [ ] Shader resource bindings match pipeline-layout binding count/types.
79. [ ] Validate PM4 packet sizes/opcodes in the command processor.
80. [ ] Register-state coherence checks (depth_enable vs bound depth target).
81. [ ] Fetch-shader/vertex-attribute offsets within the vertex buffer.
82. [ ] Heuristic warn on shaders with huge push-constant-driven loop bounds.
83. [ ] Validate compute local-size declaration vs dispatch assumptions (detile 64-wide).
84. [ ] Validate render-pass attachment count vs pipeline colorAttachmentCount.

## TIER 3 — Kernel / libraries / audio / CPU
85. [ ] AjmWorker: replace the "Unreachable code" abort with a logged, recoverable error dumping codec/instruction (current movie-audio crash).
86. [ ] Bounds-check AJM decode in/out buffers; validate codec params.
87. [ ] Catch the Rosetta synchronous-exception path; log x86 RIP + context instead of aborting.
88. [ ] Throttle FS "open() failed" spam to once-per-path; keep non-fatal.
89. [ ] Second sceVideoOutOpen: explicitly support or cleanly reject.
90. [ ] Validate AvPlayer/Videodec stub outputs don't feed garbage downstream.
91. [ ] Bounds-check kernel mmap/munmap ranges against the guest address space.
92. [ ] Validate equeue/thread handles before use.
93. [ ] SIGSEGV/SIGABRT watchdog dumping the last GPU command + last kernel call.

## TIER 4 — Diagnostics infrastructure (force-multipliers)
94. [~] Plumb all device limits into host_diagnostics.h so checks use real limits (dispatch ceiling raised; full plumbing pending).
95. [x] Global ring of last 64 GPU ops dumped on device loss. Records by default whenever GPU validation is active (was gated behind a separate env, so default dumps were empty). Now covers draws, dispatches, swapchain, image<->image copies, buffer<->image upload/download, buffer<->buffer copies, and frees (with tick).
96. [x] Rate-limit every check (log each unique signature once) - avoids the 1653/643 log floods.
97. [x] One env SHADPS4_GPU_VALIDATION=off|warn|assert controlling all checks uniformly.
98. [ ] Tag each GPU command with its originating subsystem (RefreshImage/Detile/FSR/Present) in logs.
99. [ ] Add "ticks since last successful present" to the device-loss log.
100. [ ] Env-gated one-shot Metal frame-capture trigger at a target bind count.
