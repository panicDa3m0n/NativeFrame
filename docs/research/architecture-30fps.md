# Architecture for playable frame rates (30 FPS, then 60)

Study of October 2026. Research done on code and primary documentation; every claim names its source. Measurement base: commit `e66f03949`, reference scene, test machine ([README](README.md)).

## Starting point (measured)

- Frame 107-110 ms. CPU chain: the queue-0 worker (frame sensitivity 0.85) and the committer (0.4-0.8) wait for each other.
- Committer: about 2,900-3,200 commits per frame, template hit 31 µs per draw, of which 3-4 % Vulkan recording.
- Floor with every shortcut that was valid at the time (trusted templates and targets, timestamps, submits, parallel preparation, lighter GPU): 88 ms, committer 24.5 µs per commit. Remaining per draw: recording 9.9 µs (constant snapshots and descriptor sets about 5.7), lookup 4.4, vertices and indices 3.8.
- GPU 70-75 ms: draws 35, storage images 12.7 (up to 30 in the slow regime), compute 11, staging 4.5.
- The AnyPS5 model at that point: guest memory imported into the GPU (`external_memory_host`), CPU writes discovered with `GetWriteWatch` per range and per epoch, per-draw snapshots of constants in VRAM through the BAR with a RAM shadow, resource templates revalidated at every draw, a new descriptor set per draw.

## How others solve it

### shadPS4 (PS4, GCN on Vulkan): the closest case

Sources: `src/video_core/page_manager.cpp`, `src/video_core/buffer_cache/buffer_cache.cpp`, `src/video_core/renderer_vulkan/vk_rasterizer.cpp` (main branch, October 2026, github.com/shadps4-emu/shadPS4).

- **Sparse mirror of guest memory on the GPU.** `GetArena()` maps the pages of the guest virtual address space onto sparse Vulkan buffers; `EnsureResident()` allocates device memory for missing blocks and binds it with `bindSparse` (`SubmitPendingArenaBinds()` at each submit).
- **Page table for BDA.** `bda_pagetable_buffer` holds a `VkDeviceAddress` per block: shaders reach any resident guest address by indexing the table. A `fault_manager` collects GPU accesses to non-resident blocks and makes them resident the next frame (`TickFrame()`, `ProcessFaultBuffer()`).
- **Event-driven tracking.** `PageManager` write-protects (and read-protects if needed) pages the GPU uses, with a watcher count per page and protections merged into contiguous runs. A write fault calls `rasterizer->InvalidateMemory()`, which marks the memory tracker dirty; the upload happens at the next GPU access (`SynchronizeMemory()`, only dirty ranges, via staging).
- **VRAM authoritative, lazy read-back.** GPU writes go into `gpu_modified_ranges`; they are copied back only on a CPU read fault (`ReadMemory()`), widened to a window because "GPU-modified ranges come as many small scattered islands".
- **Per draw.** `BindResources()` builds descriptor writes with a null `dstSet` (push-descriptor style, no set to allocate); small read-only data and user data (`flatbuf`) go to a 128 MB stream buffer; V# buffers come from `ObtainBuffer()`, which on the fast path only checks the dirty bits.

**Difference with us.** We have every draw prove that nothing changed (revalidation, collections, compared snapshots); in shadPS4 a draw asks "is it dirty?" of a bitmap updated by faults. Our constants are per-draw snapshots with a new descriptor set; in shadPS4 constants live in the mirror or the stream buffer and descriptors are pushed.

### Xenia (Xbox 360 on D3D12 and Vulkan)

Source: `src/xenia/gpu/shared_memory.cc` (master branch, github.com/xenia-project/xenia).

- **One GPU buffer mirrors the whole physical memory of the guest.** Addresses are the guest's physical ones; shaders read vertices and data from the shared buffer.
- **Validity per page in bitmaps** (`SystemPageFlagsBlock`, 64 pages per block): `valid` (the GPU copy matches the CPU) and `valid_and_gpu_written` (data produced by the GPU, the CPU is not aligned).
- **Event-driven invalidation.** CPU writes arrive through a physical-memory access callback (page protection); the callback clears the bits and, if the range is not exact, widens the invalidation, stopping at GPU-written pages. Code comment: "It's a lot cheaper to upload some excess data than to catch access violations"; the comment reports 4 KB callbacks as much slower than 256 KB ones.
- **Batched on-demand upload.** `RequestRange` walks the `valid` bits of the requested blocks only, merges invalid pages into runs and uploads them with one `UploadRanges` call.
- **GPU writes.** `RangeWrittenByGpu` fires the watches (for example textures that read a resolve) and marks valid pages as GPU-written: they are not reloaded until the CPU touches them.

**Render target cache and resolution scaling** (source: Triang3l's article on xenia.jp, 27 April 2021, "Leaving no pixel behind").
- Ownership per EDRAM range: copy only when a range is used with a different format, width or sample count, otherwise nothing. Copies go directly from one render target to another on the GPU, with a pixel shader generated per format pair and bit-exact conversions. The article's measurements (GTX 1070, GPU time): from 79.8 to 23.5 ms and from 19.5 to 9.2 ms in two scenes, against the previous cache that downloaded and re-uploaded every target at every change.
- Scaling: every console pixel becomes a 2×2 or 3×3 block; scaled data lives in a sparse GPU virtual space allocated only where something was drawn; the scale also applies to sampling offsets and bilinear weights. What breaks: shaders that hard-code the unscaled size, weaker gather effects (bloom, SSAO), half-pixel offsets that become holes.

**For us.** Our aliased storage images are the same problem as Xenia's EDRAM: the cache rewrote and reloaded at every refresh, while the right model transfers ownership only on use, on the GPU.

### Ryujinx and yuzu (Switch)

Sources: a mirror of Ryujinx `Ryujinx.Memory/Tracking` (`MemoryTracking`, `RegionHandle` with a dirty flag and an action on read or write, `VirtualRegion.Signal`); yuzu commit 82c2601555 "video_core: Reimplement the buffer cache" (ReinUsesLisp, 2021). Ryujinx's official sources are no longer on GitHub.
- Same model: regions with a dirty flag, page protection; the fault signals the regions and updates their protection.
- yuzu: cached bindings and page-level modification tracking; CPU writes stay cached until flush; for games that write pages in use by the GPU, CPU writes are applied to the GPU only at the end of the command buffer while the page state is updated immediately.
- yuzu's 2023 buffer cache rewrite (Project Y.F.C.) is reported at about +50 % on average (Steam Deck HQ, March-April 2023): a third-party measurement, not verified.

### Drivers and layers: NVIDIA, DXVK, vkd3d-proton

- **NVIDIA, "Vulkan Dos and Don'ts"** (developer.nvidia.com): reduce `vkQueueSubmit` calls, each with a significant CPU cost, without leaving the GPU idle; dynamic uniform or storage buffers for per-draw buffer changes; few sets and few descriptors per set; minimal, batched barriers with precise stages; `DEVICE_LOCAL | HOST_VISIBLE` (BAR) memory to write VRAM directly, CPU reads very slow.
- **DXVK 2.7** (release notes, github.com/doitsujin/dxvk): `VK_EXT_descriptor_buffer` by default on recent NVIDIA, "significantly reduces CPU overhead" compared with the legacy model; small GPU cost in some cases; ReBAR recommended.
- **DXVK 3.0 and vkd3d-proton** (Phoronix and GamingOnLinux news, 2026): moving to `VK_EXT_descriptor_heap` where supported (NVIDIA 595.84 or newer). Same CPU benefit as descriptor buffers, less GPU penalty on NVIDIA.
- **`VK_EXT_descriptor_buffer` proposal** (docs.vulkan.org): motivated by the call overhead of tens of thousands of descriptor copies per frame through `vkUpdateDescriptorSets`.

**For us.** A new descriptor set per draw (`AllocateRewritten` plus writes) is exactly the legacy model that DXVK and vkd3d-proton moved away from. AnyPS5 #6947 (global descriptor heap) goes in the same direction.

## The model they converge on

1. **The GPU reads guest memory from a persistent mirror** (Xenia: one buffer for the whole physical memory; shadPS4: sparse arenas plus a BDA page table). No per-draw snapshots.
2. **Validity is event-driven**: per-page bits updated by protection faults (CPU writes) and recorded GPU writes; the draw asks the bitmap instead of proving with queries.
3. **Uploads are batched and ordered in the command stream**: dirty pages are copied by commands recorded before the draw that uses them; invalidation is widened to blocks (Xenia: 256 KB far better than 4 KB).
4. **VRAM is authoritative for GPU-written data**, with lazy read-back on a CPU read fault.
5. **Descriptors are not allocated per draw**: push descriptors (shadPS4), descriptor buffers or heaps (DXVK, vkd3d-proton), a global heap (AnyPS5 #6947).
6. **Image aliases are resolved on the GPU, transferring ownership only on use** (Xenia).
7. **Resolution scaling is an emulator feature**, with its known limits.

On the test machine (`vulkaninfo`, driver 616.64, API 1.4.351) `VK_EXT_descriptor_heap`, `VK_EXT_descriptor_buffer`, `VK_KHR_push_descriptor`, `VK_KHR_buffer_device_address`, `VK_EXT_graphics_pipeline_library` and `VK_EXT_shader_object` are all available.

## Map onto our bottlenecks

| Measured bottleneck | Cost | Piece of the model that removes it |
|---|---|---|
| Template revalidation and target refresh per draw | about 9 µs per draw; ceiling −9 % of the frame | 2: event-driven validity |
| Constant snapshots and new descriptor sets per draw | about 5.7 µs per draw | 1 and 5: persistent mirror with stable identity, cached descriptors or heap |
| Vertex and index copies per draw | about 3.8 µs per draw | 1: persistent mirror |
| Write collections in the worker (capture) | about 5 µs per draw | 2: event-driven validity |
| Aliased storage images | 12.7 ms GPU per frame, up to 30 in the slow regime | 6: ownership transferred on use |
| Profiler labels and submits | about 780 labels and 137 submits per frame; ceiling −4/−5 % | batched submits, grouped labels |

What makes the model practical for this title: the CPU rewrites about 80 MB per frame of memory read by the GPU, but 96 % with the same bytes; the truly new bytes are about 6 MB per frame (143.8 MiB of new snapshots and 406.7 MiB copied in refreshes per 10 s, about 89 frames). With invalidation widened to 64-256 KiB, faults per frame stay in the hundreds or a few thousands (measured cost about 2 µs per fault).

## Prototypes, in order

Every prototype sits behind a switch, and its result is recorded even when negative, with the reason.

1. **Stable snapshots.** Device copies of draw reads (constants, V# buffers, vertices, indices) become a persistent mirror of guest pages, updated by copies recorded before the draw that uses them, never recycled while valid. With a stable identity, descriptor sets can be cached per template and mirror. Measure: committer µs per draw (about −5 µs expected).
2. **Event-driven validity.** Page protection on the mirrored ranges (upstream's armed pages and `HostWrite`, vectored handler), invalidation widened to blocks, instead of per-draw collections and revalidation. Measure: committer and worker µs per draw (−6/−9 µs expected), faults per frame, fault cost on the game threads.
3. **Descriptors without per-draw allocation.** `VK_EXT_descriptor_heap` or push descriptors, starting from AnyPS5 #6947. Measure: binding cost in the committer.
4. **Storage images with ownership on use** (Xenia model): no swap at refresh, transfer on the GPU only when the new owner reads. Needs read information for images from the recompiler. Measure: storage classes in the GPU profile, slow regime gone.
5. **Resolution below 1** with a spatial upscaler, then a temporal one. Later found not to be a lever for this title, which already draws its main scene at 1080p with dynamic resolution (see [memory-mirror.md](memory-mirror.md#the-gpu-side)).
6. **Recording on several threads per pass**, only once the fast path of draws is made of reads (after 1-3): according to AnyPS5 issue #7895, 87.5 % of draws do not depend on the previous 16.

Expected: 1-3 bring the committer from 25-29 to about 12-15 µs per draw; with parallel preparation the CPU chain goes towards 45-55 ms. Thirty FPS also needs 6, or fewer commits per frame; sixty needs 6 and work on the translated shaders.

### What happened to the plan

The render-pass constraint (an import→mirror copy cannot happen inside a render pass, and most draws continue an open pass) changed the order: event-driven validity and validity tokens came first, the mirror only for cold data. The details are in [memory-mirror.md](memory-mirror.md). Item 4 was built as "storage images without block units through the unit shadow" (`018c5a841`, below).

## Upstream work to combine

- Event-driven validity: armed pages, `GuestArena::HostWrite` and the vectored handler already in upstream; AnyPS5 #7473 and #7471 for clean resident zones.
- Descriptors: AnyPS5 #6947 and #7642 (global descriptor heap, constants read by address), to be rebuilt on top of the mirror and measured on NVIDIA.
- Storage images: AnyPS5 #4286 (units passed between images of different shapes) and #6197 (imports of surfaces straddling the window).
- Queue-0 worker: AnyPS5 #5656 (reuse with only user words changed) and #6188 (draws compiled ahead), one or the other: both rewrite `Driver::draw`, together with #6946.
- Micro, last: AnyPS5 #6292 and #6615 (MinGW locks and thread-locals), already merged into NativeFrame.

## Storage images without block units through the shadow (`018c5a841`)

**What alias swaps really cost** (diagnostics by class and by time):
- For B's DCC keys: clears are 10 % of the bytes (1.15 GB of 11.3 per 10 s); B uncompressed on an import 6.85 GB; B without DCC 3.26 GB plus 1.69 GB published. The clear rule of AnyPS5 #4286 covers little here.
- Retile and detile kernels are fast: 103 MB per present in 0.33 ms, about 313 GB/s.
- Almost all of the about 16 ms of GPU per present of storage images is PCIe towards the import: upload from the import 5.9 ms (29 MB), write-back into the import 4.1 ms (19 MB), shadow publish 2.9 ms (18.5 MB), at 4.6-6.4 GB/s.
- They are all linear images (tile mode 0), some with a base not aligned to the 64 KiB block, which have no block units and therefore skip the VRAM shadow: they publish the alias's shadow and read back from RAM at every frame.

**How others do it.**
- Xenia (`render_target_cache.cc`, `ChangeOwnership`): ownership per range, transfer on the GPU only when the owner changes, skipped where the range will be cleared. Never through main memory.
- shadPS4 (`texture_cache.cpp`, `ResolveOverlap`): for aliases from a pool or with a different tiling it frees the old image and reloads from guest memory, treating the old content as dead. A heuristic, not exact; a lever for later if the recompiler exposes `imageRead` (today `ResourceTracker` knows it, `DescriptorBindingBuilder` does not pass it on).
- AnyPS5 #4286 (closed): Xenia's model for the clear and uncompressed-on-import cases only; neutral on Windows by default because there most of the data was already in VRAM thanks to the shadow. Here most of it is outside the shadow.

**Design** (`APS5_NO_WHOLE_SURFACE_SHADOW` turns it off).
- Upload without block units: if the shadow touches the surface, gather into a scratch from the slabs and import with `ShadowSources`, then the existing detile from the scratch. No publication.
- Write-back without block units: the pieces to keep go into the slabs with `ShadowDestinationFor` (split at slab boundaries, seeding partly covered units), into the import only if the shadow refuses (budget); partial publication only for the pieces in the import; `MarkShadowed` at settle as for block images.
- Exact by construction: freshness, seeds and publications are those already proven for block images; shadow units coincide with tracker blocks, so a unit is never half in the import and half in a slab.
- Test `agc_driver_whole_surface_shadow` (and `_off`): a tiled image with pending results and a linear image with an unaligned base straddling its end; B's texels equal the memory bytes after publication, no mid-way publication with the shadow, expected final bytes, tiled image reloaded equal to a fresh one loaded from memory. Mutations caught: gather offset, omitted seeds.
