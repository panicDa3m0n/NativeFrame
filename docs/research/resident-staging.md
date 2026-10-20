# Resident staging in VRAM

October 2026. Commit `34c9f6f60`, switch `APS5_NO_RESIDENT_STAGING`. Context, machine and glossary: [README](README.md).

## Problem, measured

Buffers written by the game's compute shaders (written elements of 16 KiB to 2 MiB inside imported memory) go through a *staging* copy in VRAM:

1. in: copy from the imported memory to VRAM;
2. the shader's work;
3. out: copy from VRAM back to the imported memory.

In the reference scene there were about 4,700 regions and 2.85 GB in each direction every 10 s: staging-in 5.7 ms and staging-out 6.9 ms of GPU per frame, at about 5 GB/s.

**Who writes these regions between one use and the next.** A diagnostic attributed every write that reached the tracker to its caller's return address:

| Writer | Amount per 10 s |
|---|---|
| CPU | 0 |
| copies back of other staged regions | 1,340 MiB |
| in-place writes into the import | 6.9 MiB |
| labels | 65 MiB |
| DMA copies | 34 MiB |
| nobody (unchanged blocks) | 46 % of the bytes |

So the flow is GPU→GPU through PCIe: VRAM → import (5 GB/s) → VRAM.

**Hardware.** A copy microbenchmark on the test machine: copies to and from imported memory run at 6-9 GB/s on every queue, VRAM→VRAM copies at 317 GB/s.

## Change

It applies to reused builds (`RecordStagingCopies`, 99.5 % of the copies in). The copy in is planned per 4 KiB page:

- **kept pages**: no write since this same region's copy back, and no CPU write since its copy in. The VRAM buffer still holds those bytes, and nothing is copied;
- **pages from VRAM**: the last write is the copy back of another staged region, recorded in `stagedCopies` (guest range → buffer, base, generation) with no later writes. They are copied VRAM→VRAM from that buffer;
- **the rest** is copied from the imported memory, as before.

Validity per page: `GuestMemory::StoredPagesSince`, the per-page twin of `StoredOver`:
- for the CPU it uses 4 KiB pages;
- for the driver it uses the last 4 pieces per block;
- a block with the mark but no known pieces, or with lost pieces, counts as fully written.

The registry:
- fills after the marks of the copies back (`NoteStagedCopies` in `MarkGpuWrites`);
- a registration cuts the entries it overlaps;
- a region forgets the entries of its own buffer when it copies into it again;
- a destroyed build forgets the entries of its buffers;
- holds at most 16,384 entries.

`APS5_NO_RESIDENT_STAGING=1` restores the previous behaviour.

## Result (GPU profile, reference scene, fast regime)

| Item, per 10 s | before | per-page registry |
|---|---|---|
| copied in from the import | 2.85 GB | **21 MB** |
| kept pages | 0 | 1.36 GB |
| copied VRAM→VRAM | 0 | 1.47 GB |
| staging-in, GPU ms per frame | 5.70 | **0.21** |
| staging-out, GPU ms per frame | 6.87 | 7.13 (unchanged) |

A first version with 64 KiB blocks still left 854 MB coming from the import: regions sharing a block invalidated each other.

## Impact

**What stays the same.**
- The copy back: guest memory receives the same bytes at the same point of the command stream.
- Pending write notes, tracker marks and barriers after the copy.

**What changes.**
- The barrier before the copies in adds `TRANSFER_WRITE` to the destination access: the staged buffer may have been written by the shader of the previous use.
- The pending read note is taken only for the pieces read from the import. A CPU write to kept or VRAM-copied pages no longer waits for that copy, which does not read the import.

**Order on the GPU.**
- A region reads another build's buffer after that build's copy back, recorded earlier in the stream.
- The `ALL_COMMANDS → TRANSFER` barrier covers the shader's writes.
- The source build can copy into its own buffer again only in a later use, which first forgets its entries and is recorded later in the stream.
- Regions of the same build do not use each other as a source, because their copies in sit in the same copy group, with no barriers between them.

**Accepted window.** A CPU write to a kept page that arrives after the plan is collected and before the GPU executes it does not enter this use. The tracker sees it at the next use, which copies the page again.
- Before, the GPU copy might or might not have caught it, depending on timing.
- It is the same category as the trade-off already documented: the copy back overwrites CPU writes made during the work.
- In the reference title the CPU never writes these regions (0 bytes measured).

**Costs.**
- On the committer: one `CollectWrites` and two tracker queries per staged region at every use, plus the registry lookups, under its own mutex.
- Memory: no new buffer. The registry keeps `shared_ptr`s to buffers of live builds, because the build's destructor forgets them.

**Who benefits.** Any title whose compute shaders write buffers within the staging window inside imported memory. The gain depends on how much data passes from one build to another.

## Verification

- New test `tests/execution/StagingResidency.cpp`: two real compute builds on different parts of the same memory, with the CPU writing in the middle (round 3) and at the tail (round 4), and two repetitions. Checks the bytes at every round and the statistics (kept, from VRAM, from the import).
- `agc_driver_staging_residency_off` (switch off): everything from the import, correct bytes.
- Two mutations caught:
  - without the source check, the tail written by the CPU is overwritten in round 4;
  - with everything kept, the test fails in round 1.
- The change was also checked with the full ctest, an A-B-A run against the previous build and a capture with recorded input.

## Open problem

Staging-out did not change (about 7 ms of GPU per frame), and draws now stage too (see [memory-mirror.md](memory-mirror.md#the-gpu-side)). The next step is **VRAM as the authority for GPU-written data**: skip the copy back until a reader outside VRAM (the CPU, an in-place GPU read, a DMA from the import) actually needs the bytes, and publish lazily, as the unit shadow already does for storage images.
