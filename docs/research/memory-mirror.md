# Guest memory on the GPU: from per-draw proofs to event-driven validity

October 2026. Context, machine and glossary: [README](README.md).

The question behind this note is about the engine, not about one game: how should the driver's memory model work so that the cost per draw stops growing with the amount of guest memory a draw touches? The reference title is only the measuring stick.

## The problem, in terms of memory

On the console, CPU and GPU share one memory. On a PC the GPU has its own VRAM and reaches system memory across PCIe. Today guest memory reaches the GPU by five different routes, each with its own validity proof:

| Route | How | Measured cost |
|---|---|---|
| In-place reads | guest memory imported into the GPU (`VK_EXT_external_memory_host`); the GPU reads across PCIe at 6-8 GB/s | slow GPU reads, which is why snapshots exist |
| Per-draw snapshots | `PrepareDrawBindings`: for each binding a copy in VRAM (resizable BAR), reused while the tracker sees no writes; fast path only within the same epoch (about 310 epochs per frame) | 10.5 % of the committer, plus a new descriptor set per draw |
| Vertex inputs | `CopyDrawInput`: snapshots of vertices and indices | about 4 % (`prepareDrawInputs` 8.7 %) |
| Buffers written by the GPU | staged in VRAM and copied straight back into the import ("staging-out") | 4.4 ms of GPU per present (31 MB at 7 GB/s) |
| Fills | written into the import | 3.0 ms of GPU per present (15 MB at 4.8 GB/s) |
| Storage images | unit shadow in VRAM (64 KiB units in 8 MiB slabs), published lazily; linear images included since `018c5a841` | 3 ms of GPU per present (was 9-27) |

On top of that comes the **per-draw validity proof**: `Revalidate` and `fastRevalidate` run a `CollectWrites` per texture and per draw (memoized per epoch); `refreshResidentTarget` and `StorageTexture::Refresh` re-prove targets; per-use copies of sampled depth planes are re-proved at every use and made about 1,900 templates per 10 s non-reusable.

In the committer's sampled profile, proofs and snapshots are about 40 % of the samples and collections of CPU writes 7 %. No single function is slow: the cost is the **model**, which recomputes at every draw the coherence between guest memory and the GPU copies.

## How others solve it (sources read directly)

**Xenia** (`src/xenia/gpu/shared_memory.cc`, `render_target_cache.cc`; article "Leaving no pixel behind", 2021).
- One GPU buffer mirrors the whole physical memory (512 MB); validity per page in bitmaps (`valid`, `valid_and_gpu_written`).
- CPU writes: page protection with a callback that clears the bits; invalidation is widened to blocks ("It's a lot cheaper to upload some excess data than to catch access violations").
- `RequestRange` uploads only invalid pages, in batches; GPU writes mark pages and are not reloaded until the CPU touches them.
- Render targets: ownership per range, transferred on the GPU only when the owner changes.

**shadPS4** (`src/video_core/buffer_cache/buffer_cache.cpp`, `memory_tracker.h`, `region_manager.h`).
- The guest address space is covered by **sparse Vulkan buffers ("arenas")**; physical memory is bound on demand (`EnsureResident`, `vkQueueBindSparse` ordered with a timeline semaphore).
- O(1) lookup: `address_space[addr >> ARENA_PAGE_BITS]`; a buffer binding is (arena, offset).
- Coherence: a two-level `MemoryTracker` with CPU and GPU bits per page; the `PageManager` protects cached pages, the fault (`InvalidateRegion`) marks them dirty and unprotects them.
- Upload: `SynchronizeMemory` gathers dirty pages (`ForEachUploadRange`) into a staging buffer and issues **one** copy.
- GPU writes: `MarkRegionAsGpuModified`; read back only when the CPU touches the region, in 512 KB windows ("GPU-modified ranges come as many small scattered islands").
- Small read-only buffers: copied every time into a 128 MB stream buffer so render passes are not split.
- A BDA table per block (`bda_pagetable_buffer`) for shaders that read by address.

**Ryujinx and yuzu** (`MemoryTracking`, `RegionHandle`; yuzu commit 82c2601555): the same model, regions with a dirty flag and page protection; in yuzu CPU writes to pages in use by the GPU are applied at the end of the command buffer.

**Convergence.** A persistent mirror in VRAM addressed like guest memory; validity driven by events (CPU write faults, recorded GPU writes); batched uploads of dirty pages only; VRAM authoritative for what the GPU writes, with lazy read-back; bindings as (buffer, offset), stable across draws.

## What NativeFrame already has, to be unified

- **Write tracker** in 64 KiB blocks with generations (`UnchangedSince`, `UnchangedSinceAll`); CPU writes found by collection (`GetWriteWatch`, upstream's armed resident runs, `cleanRanges`), GPU writes stamped.
- **Unit shadow** (`UnitShadow`): 8 MiB VRAM slabs per import, freshness derived from the tracker, lazy publication into the import (hooks, regions, scan-out). It is already a partial mirror, for storage images only.
- **VRAM staging** of buffers written by the GPU, copied back to the import right away (see [resident-staging.md](resident-staging.md)).
- **Sparse binding** (`createSparseDmaBufImport`, `context.sparseQueue`), used today for dma-buf imports on Linux.
- GPU limits on the test machine (`vulkaninfo`, driver 616.64): `sparseBinding` and `sparseResidencyBuffer` supported, `sparseAddressSpaceSize` 1 TB, `maxStorageBufferRange` 4 GB, `minStorageBufferOffsetAlignment` 16.

## First target model, and the render-pass constraint

The first plan mirrored each import in a sparse device-local buffer of the same size, bound in 64 KiB blocks on demand; a GPU read of a range would queue import→mirror copies for blocks dirtied by the CPU, then bind (mirror, offset); GPU writes would stay in the mirror; CPU reads of GPU-owned blocks would publish them back, as the unit shadow does.

Working through how the mirror would be updated during recording showed a hard constraint:

- a copy from import to mirror in the command stream (`vkCmdCopyBuffer`) is not allowed inside a render pass: updating a block in the middle of a pass means closing and reopening it, and in the reference scene 194,000 of 215,000 draws continue an open pass;
- writing the mirror from the CPU at record time would change the bytes read by draws already recorded but not yet executed (one copy per address, no versions);
- this is why shadPS4 copies small read-only buffers into a stream buffer every time ("For read-only buffers use device local stream buffer to reduce renderpass breaks"): the same model as our snapshots, with a ring and push descriptors instead of objects and sets.

Consequence: a mirror with GPU copies pays off for **cold** data (blocks the CPU does not rewrite between frames), whose copies are rare and fall outside passes; **hot** data stays on a per-draw ring, copied without bookkeeping. What we measure is not the copy, it is the **per-draw bookkeeping**: snapshot lookups, `UnchangedSince` under the tracker lock, byte comparisons (`CompareSpans` and `memcmp`, about 5.7 %), write collections (about 7 %). The underlying lever is **event-driven validity**: a per-block state updated when the write happens (CPU through page protection or one collection per epoch, GPU through its stamp), read by draws without locks and without collections.

## Event-driven design

Xenia uses one-shot watches attached to page buckets (`WatchMemoryRange`, `FireWatches`), fired by the CPU write-fault callback (`MemoryInvalidationCallback`, which widens the invalidation up to 256 KB, stopping at GPU-written pages) and by GPU writes (`RangeWrittenByGpu`); the per-draw check (`RequestRange`) is a lock and a few bitmap operations per 64 pages. NativeFrame does the opposite: whoever uses a range **asks** (collection of CPU writes per epoch, then `UnchangedSince` under the tracker lock), at every draw, for every texture, target and snapshot.

Pieces of a notification model:

- **E1, write-fault observer** in libc (`WindowsMappings::HandleWrite`, `resumeResidentWrites`, following the existing observers in `GuestArena` and `GuestAllocations`): the reopened range reaches the driver when the fault happens.
- **E2, immediate stamp**: the driver stamps the tracker's CPU blocks in the callback, without waiting for a collection.
- **E3, watch registry** per 64 KiB block (one-shot, like Xenia): templates, textures, targets and depth copies subscribe to their ranges; a CPU or GPU stamp fires them.
- **E4, per-epoch collection of subscribed hot blocks only** (memory that is not armed, where faults do not arrive): once per epoch and per block, not per draw and per texture.
- **E5, consumers**: the template proof becomes "no watch fired" (plus the real work of `Revalidate`: import flushes, staging copies); depth copies refresh only when their watch fired, and the template becomes reusable again.

Correctness check: a verification mode that at every use compares the event-driven answer with the current proof and stops at the first difference; tests with mutations (CPU write to an armed block, to a hot block, GPU write, aliasing).

### E1-E2, measured

- **Root cause.** libc armed (read-only protection, collections skipped as clean) only resident runs collected in full, and the first fault reopened the run for good: the cold parts of large heaps and of written heaps all fell back to `GetWriteWatch`.
- **Theory.** libc's "clean and armed" state is already an event source: as long as no clean range is removed, no CPU write can have happened there. It is enough to extend it to more memory (E1) and make it readable without a lock through a serial (E2).
- **Sources read:** Xenia `shared_memory.cc`; shadPS4 `memory_tracker.h`; RPCS3 progress report of September 2018 (read-only locks on cached regions, no-access locks when the GPU holds the newest copy, faults as lazy read-back). The threshold of 64 collections and the two-fault rule of E1 are NativeFrame's own, motivated by the upstream test that requires a single fault for large writes.
- **Result** (one diagnostic run): collections reaching libc from 332,000 to 119,000 per 10 s, 3.27 million answers from the clean memo, resident walks from 84 GB to 4.9 GB, written pages found 66,200 against 70,800, 4-5 faults per 10 s, serial unchanged; frame from 99.7 to 94.8 ms, committer from 25.8 to 24.4 µs per commit. Commits `8ce2e4d63` and `f6ff658f8`.

## Validity tokens

Instead of adding event pieces on top of the polling model, the model is replaced: the question "did anything this draw reads change?" must cost a few atomic reads, and only a "yes" leads to the full proof.

- **CN-1, lock-free validity:** per-block generations readable with atomic loads (today `UnchangedSince` takes the tracker lock), plus the clean serial (E2) for CPU writes to armed memory. This is Xenia's `RequestRange`.
- **CN-2, validity token per consumer:** templates, textures, targets and snapshots keep a token (clean serial, blocks with the generations seen, pending serial, allocation registry generation); the per-draw check is one function on the token; when the token expires, full proof and new token.
- **CN-3, draw repetition:** with a valid token the draw reuses the whole bound state (pipeline, descriptor set with dynamic offsets for moved constants, vertices, targets); the new constant goes to a ring like shadPS4's stream buffer.
- **CN-4, fault notifications** from libc to the driver for armed blocks, only if the global serial is not enough (today 4-5 faults per 10 s).

The profile confirms the root: the committer recomputes the whole bound state from guest memory at every draw, even when the template is the same (98.7 % of draws) and nothing it reads changed. The cost is spread (no function above 3 %) because it is the model.

| Component | Today, per draw | With a token | Share of committer |
|---|---|---|---|
| template proof | `Revalidate` / `fastRevalidate` | clean serial, lock-free block generations, pending serial | 5.5 % |
| targets | `refreshResidentTarget`, `StorageTexture::Refresh` | image token (generation, keys, pending) | 5.8 % |
| vertex inputs | `prepareDrawInputs` (layout, copies, push constants) | template layout and buffers reused while their ranges' token holds; only new offsets | 9.3 % |
| bindings | `PrepareDrawBindings` (snapshots, new set, writes) | template set reused, moved constants in a ring with dynamic offsets | 11.7 % |

Order: targets and proof first (least risk, clean measurement), then inputs, then bindings. Every piece: a switch, a verification mode, a test with a mutation, and one run with its question written down first.

### Depth-copy reuse (`7d1ad9106`)

Templates that sample per-use depth plane copies become reusable. The mechanism works: builds per 10 s go from 3,258 at 197 µs to 1,211 at 33 µs, the resources phase from 192 to 32 ms. The frame does not change (92.9 against 93.2 ms with the switch off on the same binary): refreshing the copy inside the fast proof adds about 105 ms of lookups per 10 s. Builds were about 7 % of the committer; the real cost is the work of every hit. It stays on.

### Closed hypothesis: 4 KiB pages for large snapshot refreshes

shadPS4 (`region_definitions.h`: `BYTES_PER_PAGE` 4 KiB, `ForEachUploadRange` without comparison) and Ryujinx (`GranularBufferThreshold` 4096, `MultiRegionHandle`) track by page. A diagnostic showed that of the 9.9 GB compared per 10 s, 99.6 % falls in pages that really were written. The game rewrites whole buffers of about 4.7 MB with 95 % of the bytes unchanged, so finer granularity does not help; the comparison already runs at about 25 GB/s.

### Resident target token (`00c8bcc2c`)

98.3 % of target lookups repeat the target of the same slot of the previous draw; in 85.2 % of those, tracker generation, collection epoch and pending serial are unchanged. Those lookups cost 1.67 µs each, about 475 ms of committer time per 10 s.

In `refreshResidentTarget`, on the committer and under the GPU lock, a memo per color slot reuses the object if all of these are unchanged:
- the fields of the `ColorTarget` and the object itself (`weak_ptr`);
- recorder and device;
- tracker generation (writes already stamped: CPU, driver, GPU);
- collection epoch (CPU writes not yet collected);
- serial of pending images;
- serial of the storage image cache (inserts and evictions);
- no copied writer or completion label waiting (`PendingCpuWritesPossible`, the exit condition of `CompletePendingCpuWrites`).

`APS5_NO_TARGET_MEMO` turns it off. `APS5_VERIFY_TARGET_MEMO` also runs the full lookup and stops if it returns a different object or if `Refresh` had to work. The test `ResidentTargetMemo` draws three times on the same target (the third must reuse), then writes back, lets the CPU write the first page, advances the epoch and has a draw that inverts the destination: the page must become 0xEE. With a token missing generation, epoch and pending serial the mutation leaves 0x11 and the test fails. A trap found while writing it: a `ColorTarget` that only keeps the destination is not enough, because at write-back blocks written by the CPU win over GPU results anyway, so a stale image is invisible in memory; the test needs a draw whose result depends on the destination.

Result in game (same binary, close runs): memo off 90.6 ms, 24.4 µs per commit, target reads 750 ms per 10 s; memo on 89.0 ms, 23.5 µs per commit, 288 ms, 84 % hits. The verifier compared 2.1 million reuses with the full lookup without a difference.

## Bindings, measured

Record phase per draw (µs):

| PrepareDrawBindings | pass and pipeline | bind and push | commands | start | leave | access | flush |
|---|---|---|---|---|---|---|---|
| 5.98 | 1.92 | 0.50 | 0.48 | 0.24 | 0.22 | 0.20 | 0.10 |

Inside `PrepareDrawBindings` (µs per call, 7 allocations per draw):
- snapshot loop 5.85, then update 0.51, keep 0.41, writes 0.26, set allocation 0.20, counts 0.14;
- of the 7 allocations: 2.9 found in the memo of unwritten data, 2.8 copied into the arena (about 174 bytes each), 0.08 on the slow path of large buffers.

Snapshot loop (µs per call): slow path plus small copies 3.63; loop overhead (vectors, `shared_ptr`) 1.28 plus 0.70; lookup in the unwritten memo 0.46; in-place decision 0.24, stamp 0.21, note 0.23.

Slow path, large buffers of 2.8 MiB on average: collect 2.5 µs per call (walks in 19 % of cases), `ReusableDrawSnapshot` 22.9 µs (in 12.6 % of cases it refreshes by comparing about 4.7 MB, about 190 µs), new ones 0.8 µs. It weighs about 1.8 µs per draw.

shadPS4 (`vk_rasterizer.cpp` `BindBuffers`, `vk_pipeline_common.cpp` `BindResources`) rewrites descriptors at every draw but pushes them with `vkCmdPushDescriptorSetKHR` (no set allocated, no update, no keep); V# buffers come from `ObtainBuffer` on the persistent mirror, without a per-draw snapshot; the stream buffer is only for flattened user data, clip planes and LDS; unaligned offsets go in push constants. The descriptor API itself costs about 0.6 µs per draw on NVIDIA; the rest is our bookkeeping.

Proposed, in two steps:
1. **Push descriptors for the set rewritten at every draw.** Set layouts with `PUSH_DESCRIPTOR_BIT` (`maxPushDescriptors` 32), template writes plus snapshots pushed into the command buffer. Removes allocation, update and keep of the set (about 1.1 µs). Fallback to the rewritten set above 32 descriptors or with variable counts.
2. **Small constants always copied into a ring**, without the unwritten memo or notes: copying 174 bytes costs less than the maps. Removes lookup, note and most of the overhead (about 1.5-2 µs).

Expected: from about 6 to about 2.5-3 µs per draw, −12/−14 % of the committer. Constraint: pipeline layouts change, so the pipeline journal and cache are rebuilt on first start.

## The GPU side

- **Real frame.** With `APS5_PRESENT_RATE` and no profiler the scene ran at about 80 ms per frame at that point; the draw profile itself costs about 7.5 ms per frame. Gains are measured without the profile; the profile only attributes.
- **Committer synchronization.** 3.6 % of the committer was in `notify_all`, waking the worker at every commit. Waking at half depth was worse (+1.7 ms: the worker refills in bursts and the committer runs dry; now opt-in as `APS5_COMMIT_HYSTERESIS`). A producer that spins for the hand-off window before sleeping gained 2.1 ms (`98cf38fcd`, `APS5_NO_PRODUCER_SPIN` turns it off).
- **Resolution.** The output buffers are 3840×2160, but the main scene is drawn at 1920×1080: the game uses dynamic resolution and, with our GPU times, stays at its minimum. Resolution scaling is therefore not a lever here, and an earlier estimate of 22 ms of 4K pixel work was wrong.

GPU time per frame:

| Item | ms |
|---|---|
| draws | 35 |
| of which depth passes | 14.6 |
| of which G-buffer | 12.3 |
| of which shadows | 5.1 |
| compute | about 10 |
| staging-out | 4.5 |
| fill | 3.1 |
| storage images | 3.6 |

A single draw costs 11.7 ms: a per-pixel light list built with GDS appends (`ds_append`), an `image_atomic_swap` on a head image and two `buffer_store`s per fragment into buffers marked written. It is not the barrier after the atomics: without it the cost is the same. Staging in VRAM of written buffers existed only for dispatches (`AllowDeviceStaging`), so draw writes went through PCIe into the import.

**Draw staging** (`29363d1f0`, `APS5_NO_DRAW_STAGING`): recordable draw builds may stage their written buffers; a draw that ends up outside the recorded path rebuilds its resources without staging. Test `DrawStagedWrites` (on and off).

| Run (150 s, GPU profile) | GPU per present | draws | staging-out |
|---|---|---|---|
| draw staging off | 62.1 ms | 35.5 | 4.54 |
| on (2 MiB window) | 60.0 ms | 32.6 | 5.19 |
| 16 MiB window for everything | 75.1 ms | 23.8 | 25.4 (185 MiB) |
| 16 MiB window for draws only | 73.2 ms | 24.1 | 20.7 (149 MiB) |

The real frame stays at about 90 ms in every variant (that session): the frame is CPU-bound. The light-list buffers are 8 and 12 MiB, above the 2 MiB window. In VRAM the draw gets 8.7 ms faster, but copying the whole buffer back at every use costs more. The way out is **VRAM as the authority for GPU-written data**: no copy back until a reader outside VRAM (the CPU, an in-place read) needs it. This is an open problem.

## Expected budget towards 30 FPS

At about 97 ms per frame: committer 25.4 µs per commit for 3,180 commits, about 81 ms per frame at 83 % busy. 30 FPS needs about 10 µs per commit, fewer commits, or recording on more threads. Trusting templates and targets (an upper bound for E1-E5) takes it from 29 to 23 µs; snapshots, descriptors and inputs (ring and stable sets) about 5-7 µs more; labels and submits about 2-3 µs. GPU busy 58-60 ms per present. 30 FPS needs, together: committer towards 10-12 µs, GPU under 30 ms (GPU-written data resident in VRAM, fewer PCIe round trips). No single step is enough: each CPU lever moves the frame by 5-10 %.

## Risks and checks

- Coherence: every step behind a switch, tests with mutations (CPU write after upload, GPU write then CPU read, aliases of different sizes), a recorded-input capture compared visually.
- Memory: a mirror binds only blocks read or written by the GPU; configurable cap, eviction with publication as for the shadow.
- Sparse binding on Windows with NVIDIA: to be validated first with a minimal test (64 KiB block binding, copy, shader read).
- Upstream pieces compatible with this direction: AnyPS5 #6947 (global descriptor heap) and #4286 (image aliasing).
