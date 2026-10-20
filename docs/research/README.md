# Research notes

These notes record what NativeFrame has measured, why the driver is shaped the way it is, and which problems are still open. They are working documents: numbers come with the machine, the commit and the method that produced them, and a hypothesis that a measurement disproved stays in the text with the correction.

| Note | Subject |
|---|---|
| [cpu-chain.md](cpu-chain.md) | Where the CPU time of a frame goes: queue worker, draw preparation threads, committer. Current bottleneck and the steps taken. |
| [memory-mirror.md](memory-mirror.md) | The memory model: how guest memory reaches the GPU today, how other projects solve it, and the move from per-draw proofs to event-driven validity. |
| [architecture-30fps.md](architecture-30fps.md) | Survey of shadPS4, Xenia, Ryujinx/yuzu, DXVK and vkd3d-proton, the model they converge on, and the prototype plan towards 30 FPS. |
| [resident-staging.md](resident-staging.md) | Keeping GPU-written buffers resident in VRAM between uses instead of copying them through host memory each time. |
| [test-bench.md](test-bench.md) | How measurements are taken and compared, and the noise they have to beat. |

## Context for all notes

- **Machine:** Windows 11, Intel Core i7-11700K (8 cores, about 4.6 GHz under load), NVIDIA GeForce RTX 4070 Ti SUPER (driver 616.x), PCIe 4.0.
- **Reference title:** Astro Bot, in one fixed scene of the Crash Site level, at the default resolution. It is the only title measured so far, so every number below describes that title on that machine.
- **Time unit:** "per 10 s" counters come from the driver's periodic profile lines (`APS5_PROFILE_DRAW`, `APS5_PROFILE_GPU`), which print totals every 10 seconds. "Real frame" means the mean frame time printed by `APS5_PRESENT_RATE` without any profiler.
- **Commits** are those of this repository's history. Switches (`APS5_*`) are documented in [docs/dev/ENVIRONMENT.md](../dev/ENVIRONMENT.md).

## Glossary

| Term | Meaning in this driver |
|---|---|
| Guest memory | The game's address space, mapped in the host process. |
| Import | Guest memory made visible to the GPU with `VK_EXT_external_memory_host`. The GPU reads and writes it across PCIe, at a few GB/s. |
| Queue worker | The thread that decodes a submitted PM4 command buffer packet by packet (`Driver::run`); for queue 0 it also prepares draws. |
| Draw preparation | Decoding a draw's state, materializing its shader stages and capturing the memory they read. Can run on extra threads (`APS5_PARALLEL_DRAW_PREPARE`). |
| Committer | The single thread that takes prepared draws in order and records them into Vulkan command buffers (`DrawPipeline::run`). |
| Template | The cached set of resources a draw with the same programs and state used last time. A "template hit" reuses it after proving it is still valid. |
| Proof / revalidation | The per-use check that nothing a template, target or snapshot depends on has been written since (`Revalidate`, `fastRevalidate`). |
| Write tracker | Per-block (64 KiB) generations stamped by CPU writes (found with `GetWriteWatch` or page protection), driver writes and GPU writes. |
| Epoch | The interval between two collections of CPU writes; about 310 per frame in the reference scene. |
| Snapshot | A per-draw copy of read-only guest data (constants, buffers) in device memory, reused while unchanged. |
| Staging | A device-local copy of a guest buffer that a shader writes, copied in before and back after the work. |
| Unit shadow | Device-local slabs that hold storage image data in 64 KiB units, published back to guest memory only when the CPU needs it. |
| Label | A PM4 packet that writes a value to memory when the GPU reaches it (`RELEASE_MEM`, `WRITE_DATA`), used by the game for synchronization. |
| Drain | The worker waiting until every queued draw has been committed, because the next packet must observe their effects. |
