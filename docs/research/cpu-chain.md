# The CPU chain of a frame

October 2026. Branch state: this repository at `e09fa3091`. Context, machine and glossary: [README](README.md).

## The chain

A frame of the reference scene is about 2,050 draws. Each one passes through three stages on the CPU before the GPU sees it:

1. **Queue worker** (`Driver::run`): decodes the PM4 packets of the game's command buffer in order, resolves each draw's registers and programs, and either prepares the draw itself or hands it to the preparation threads.
2. **Draw preparation** (`DrawPrepare`, optional threads): materializes the shader stages, captures the guest memory the shaders read, builds the resource keys.
3. **Committer** (`DrawPipeline::run`, one thread): in packet order, proves the draw's cached template still valid, resolves its resources and snapshots, and records the Vulkan commands.

The game's submit thread waits for the worker (`sceAgcDriverSubmitDcb` holds the flip behind it), so the frame time is set by whichever stage of the chain is slowest, plus the bubbles where one stage waits for another.

## Measurement method

Runs of 150-170 s in the reference scene, real frame time with `APS5_PRESENT_RATE` (no profiler), every thread sampled every 2 ms with `APS5_SAMPLE_THREADS`, stacks folded into call trees.

## Default configuration: the worker is the bottleneck

| Thread | Busy | Where the time goes |
|---|---|---|
| Queue-0 worker | 79 % | `Driver::draw` 56 %: `materializeDrawStage` 33.5 % (`Materialize` 15.5, `ShaderMemory::capture` 13, `InvocationFor` 3.2); queuing labels 11 % (mostly spinning in `DrawPipeline::Enqueue`); `RenderingWait::Wait` 7.9 %; dispatches run on the worker about 10 %; heap about 11 % spread out |
| Committer | 63.5 % committing | `recordDraw` 19.5, `resolveDrawResources` 15.2, `prepareDrawInputs` 7.1, `commitLabel` 7.5; **idle 34 %** (17 waiting, 17 spinning on the clock) |
| Game submit thread | — | waits for the worker 70 % of the time (`Driver::holdFlipBehindWorker`) |
| Game logic thread | — | spins in `scePthreadYield` → `SleepEx(0)`, outside the chain |

With an 84-89 ms frame the worker works about 66 ms and the committer about 53 ms. The committer is idle a third of the time and the game waits for the worker.

### Why parallel preparation looked useless

`Draw.cpp` sends a draw to the preparation threads only when draw relocation is off (`!drawRelocationEnabled()`), and relocation is on by default. With `APS5_PARALLEL_DRAW_PREPARE=3` alone the worker does not change: `materializeDrawStage` is still 32.7 % of it. An earlier measurement had tried the right combination, but at that time the committer was the bottleneck (28 µs per commit), so it showed nothing.

| Configuration (same session) | Real frame |
|---|---|
| default | 88.8 ms |
| `APS5_NO_DRAW_RELOCATION=1` | 87.9 ms |
| relocation off + `APS5_PARALLEL_DRAW_PREPARE=3` | **74.4 ms (−16 %)** |
| relocation off + 5 threads | 78.2 ms |

Relocation is now worth less than 1 ms; parallel preparation, when it gets the draws, is worth about 14 ms.

| Preparation threads (relocation off) | Real frame |
|---|---|
| 2 | 73.7 ms |
| 3 | 74.0 ms |
| 4 | 75.2 ms |
| 5 | 78.2 ms |

Two threads are the best on an 8-core CPU. The code default stays off (the existing tests check that without the variable no thread is used); the recommended configuration is a launch profile with `APS5_NO_DRAW_RELOCATION=1` and `APS5_PARALLEL_DRAW_PREPARE=2`. With that profile the full test suite passes except three known failures and the two relocation tests, which pass without the profile; a recorded-input capture showed no visual defects.

### How parallel preparation keeps order

- **Cut point.** After `resolveDrawDecode`, the rest of `Driver::draw` depends only on data already copied per draw: the decoded draw, the programs with their user-data patches, the draw parameters and the memory regions of the programs, plus one live register (`PA_CL_VS_OUT_CNTL`, 0x207). The per-draw snapshot is small; no register file copy is needed.
- **Stages.** The worker decodes in order and hands out numbered jobs. Preparation threads materialize stages, capture memory and build keys. The worker itself sequences finished jobs in number order into the draw pipeline.
- **Barriers.** Before any packet that writes guest memory from the CPU or must be ordered with draws (CPU labels and stores, CPU-path DMA and `WRITE_DATA`, dispatches, flips, end of submission) and before every drain of the draw pipeline, the worker waits for every open job. Register-only packets (`SET_*_REG`, the vast majority) pass without a barrier.
- **Coherence.** A job's capture reads guest memory after its own decode and before any CPU write of later packets, thanks to the barriers. GPU writes of later packets are only recorded and change nothing before their commit. Captures that overlap queued items, or the writes of an earlier open job, are redone synchronously on the worker. Indirect register loads (`SET_*_REG_INDIRECT`: 0x63, 0x64, 0x9F) pass without a drain only when the table they read does not touch the known writes of open jobs.
- **Excluded** from the parallel path, prepared synchronously as before: indirect draws, draws with deferred labels in the queue, dumps and verification modes, draws without a register key.

## With the launch profile: the committer sets the pace

Real frame 73.5 ms. Thread samples:

| Thread | Busy | Where |
|---|---|---|
| Committer | about 70 % committing, 17 % idle | `recordDraw` 23.2 (`PrepareDrawBindings` 13.2), `resolveDrawResources` 13.3 (`Revalidate` 6.2), `prepareDrawInputs` 8.7, `commitLabel` 9 |
| Worker | 59 % | `DrawPrepare::sequence` 36 (20 waiting for the preparation threads, 18 queuing into a full pipeline), labels 13, `RenderingWait` 6.8, dispatches 11.6, `Flush` 5.3 |
| 2 preparation threads | about 40 % each | `materializeDrawStage` 26-28, capture 9-10 |

Draw profile (`APS5_PROFILE_DRAW`, per 10 s):

- 255,783 draws; 99 % take the template-hit path at about 25 µs each: lookup and proof 5.3, recording 9.6, vertex inputs 3.0, keep 1.6, pipeline 1.5, target 1.4.
- Draw recipes (`DrawWithRecipe`, attached to the worker's draw cache) hit 367 draws of 255,783 and cost 90-103 µs each: that fast path does not serve this title, because the constants move to a new address every frame.
- Parallel preparation: 5,720 drains and 75,426 waits for the oldest draw. Drains by opcode: `RELEASE_MEM` (0x49) 2,278, `WRITE_DATA` (0x37) 2,061, 0x58 694, `EVENT_WRITE` (0x46) 563, 0x15 108.

No thread is saturated: the frame is the committer's work (about 51 ms) plus the bubbles where one stage waits for another.

## Label ordering: tried, neutral, not applied

Hypothesis: `RELEASE_MEM` and `WRITE_DATA` labels queued into the draw pipeline drain the parallel preparation, because the worker waits for every draw in preparation before queuing the label. Queuing them in preparation order as ready items, with the range they write among the item's writes, should remove 76 % of the drains.

Three steps were built and tested (labels as ready items with their writes; packets with the predication bit transparent when predication is off; `WAIT_REG_MEM` resolved from labels still in order), with a new test (`DrawPrepareLabels`) that catches a deliberate mutation.

| Step | Drains per 10 s (top opcodes) | Waits for the oldest | Real frame, switch off / on |
|---|---|---|---|
| before | 5,720 (0x49 2,278, 0x37 2,061) | 75,426 | — |
| a | 5,461 (0x79 3,222) | 78,455 | 73.9 / 72.3 ms |
| b | 5,434 (0x3c 2,840) | 82,960 | 74.1 / 72.6 ms |
| c | 5,237 (0x58 3,611) | 79,035 | 73.1 / 73.0 ms |

The game synchronizes in groups (label, predicate register, wait, `ACQUIRE_MEM`): removing one drain moves it to the next packet of the group. The worker's waits do not drop because the committer sets the pace (about 25 µs per draw, 51 ms per frame): the draw pipeline is full and the preparation threads stay ahead. Mean −1.4 %, inside the noise, so the steps were not merged. What matters is the committer's cost per draw. The idea is still valid once the committer is faster; see the open problems below.

## The committer's cost per draw

The committer spends about 25 µs per template-hit draw. The largest single item is the **template proof** (`Revalidate`): about 280,000 calls and about 1,150 ms per 10 s, 4.1 µs each, 98.5 % of them on the fast path, roughly 8 ms of committer time per frame. Timers per phase (ms per 10 s, out of 1,130):

| Phase | ms per 10 s | Note |
|---|---|---|
| fast proof (`fastRevalidate`) | 565 | about 2 µs per call: `CollectWrites` per texture, DCC key checks, tracker queries |
| full proof | 243 | 4,153 proofs of about 58 µs |
| pending CPU writes | 144 | |
| staging copies of reused builds | 98 | |
| imports | 48 | |
| queue | 23 | |

Ceiling for a per-template token: 75 % of the proofs (about 208,000 of 278,000 per 10 s) find the tracker generation, the collection epoch, the pending-writes serial and the storage-cache serial unchanged since the template's last successful proof, and all of those then succeed on the fast path. A prototype that reuses the last proof under those stamps (with unchanged imports and no depth surfaces, and a verification mode that redoes the proof and stops if it would have failed) is being measured; it is not in this branch yet.

## What is not the bottleneck

- CPU frequency: 4.57 GHz effective, no active limit.
- GPU: about 60 ms busy per frame, below the frame time.
- The VideoOut vblank counter advances with real time; the slow motion of the game at low frame rates comes from its own fixed-step logic.

## Open problems

- **Committer cost per draw.** About 25 µs per draw, about 51 ms per frame. The pieces: template proof (above), recording and bindings (`PrepareDrawBindings` about 6 µs per draw, see [memory-mirror.md](memory-mirror.md#bindings-measured)), vertex inputs. Target: 10-12 µs per draw.
- **Recording on more than one thread.** Today one committer records every draw under the GPU lock (`GuestMemory::GpuMutex`), which protects the recorder state (open batch, pending write and read notes, label table, hazard tracking), pending imports and storage images, and the order of commands on the single Vulkan queue. Splitting recording per render pass needs those pieces separated first.
- **Labels and drains of parallel preparation.** Once the committer is faster, the 5,000+ drains per 10 s caused by grouped label/wait/acquire sequences will matter again.
