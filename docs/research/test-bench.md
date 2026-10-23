# Test bench: how performance is measured

The rules NativeFrame uses to decide whether a change makes a frame faster. They exist because the noise between runs is often larger than the effect being measured. Context and machine: [README](README.md).

## A run

- One fixed scene of the reference title, loaded the same way every time (a command-line argument selects the level, so there is no menu navigation).
- Warm caches: the scene has run at least once since the last shader or pipeline change, and the title was restarted.
- Length: 150-240 s, the same for every run being compared.
- Normal exit of the game at the end, so the driver flushes its counters and caches.
- Nothing else heavy on the GPU during the run. Overlays (for example the NVIDIA overlay) stay as they are for every run being compared, or off for all of them.

### What each run records

| What | How |
|---|---|
| Real frame time | `APS5_PRESENT_RATE`: presented frames and mean frame time every 10 s, **without** the draw profile |
| Where CPU time goes | a separate run with `APS5_PROFILE_DRAW` (per-phase driver and draw totals every 10 s) and `APS5_TRACE_SUMMARY_EVERY` |
| Where GPU time goes | a separate run with `APS5_PROFILE_GPU` (GPU time of recorded work by class, from batch timestamps) |
| Which thread is busy, and in what | `APS5_SAMPLE_THREADS`: every thread sampled every 2 ms, hottest stacks written as collapsed stacks |
| GPU clocks, power, temperature | `nvidia-smi` every 10 s alongside the run |

The draw profile costs about 7.5 ms per frame on the reference scene, and the GPU profile adds its own overhead: profiles attribute time, they never decide whether a change is faster.

## Noise

Five runs of the same build in one morning, with the draw profile on:

| Metric | Spread |
|---|---|
| frame p50 | ±4 % (8 % between extremes); 0.2 % between two back-to-back runs |
| committer µs per draw | ±0.4 % |
| `GetWriteWatch` calls per draw | ±0.6 % |
| GPU time per frame | ±4 % |

The GPU never throttled (about 2,940 MHz, 62 °C at most).

**Drift between sessions is larger than the noise inside one.** The same build measured 74.5 ms in the morning and 88-90 ms later the same day, with another desktop application using about half a CPU core and some GPU time. Even 25 minutes apart, one counter moved by 35 % with the same work done, and the frame by 1 ms with the same binary. Absolute numbers are therefore only comparable within one session.

## Rules for saying a change helps

1. **Same binary, switch off and on.** Every change has a runtime switch (`APS5_NO_<NAME>`); the cleanest comparison is the same build with the switch off and on, in close runs.
2. **A-B-A when the effect is near the threshold.** Base, change, base again in the same session; compare the change with the mean of the two adjacent bases.
3. **Thresholds.** Committer µs per draw and `GetWriteWatch` per draw: a difference counts above 1 %. Real frame time: above 5 % in A-B-A, or above 8 % outside it. With the GPU profile on, GPU-side numbers can move by up to 14 %.
4. **Correctness before speed.** Full `ctest` with the switch on, the new test with a deliberate mutation shown to fail, and a capture with recorded input compared visually against the base.

## Method

Runs are there to find out **where and why** the machine loses time, not to prove small gains.

1. **Single, targeted runs.** Each run answers one question written down before it starts, and carries the diagnostic counters that answer it. Only a net effect beyond the noise band counts.
2. **Map the hardware first.** Microbenchmarks of the machine (copy bandwidth per queue and per memory type, GPU read cost of imported memory versus VRAM, cost of a protection fault) tie each change to the PC-versus-console difference it addresses.
3. **A negative result is a result.** It is written down with its reason, and the branch is kept as an archive.

## Launch profile

The recommended runtime configuration for the reference title lives in a launch profile: one `APS5_*` variable per line, read by the launcher and by the measurement scripts. The code defaults stay those of the engine, covered by the existing tests; the profile moves them for the title. Today it holds:

```
APS5_NO_DRAW_RELOCATION=1
APS5_PARALLEL_DRAW_PREPARE=2
```

(real frame 88.8 → 73.7 ms in the same session; see [cpu-chain.md](cpu-chain.md)). Every new change is measured on top of the profile, with its own switch, on the same binary.

Measured again on 23 October 2026 on the line over AnyPS5 main 8499ea52e, with AnyPS5 #9012: four alternating rounds per side, each a 150-second trace window at Crash Site, without and with the profile. Frame p50 87.1 → 78.4 ms (every round: 88.5, 87.4, 86.8, 85.1 against 79.8, 77.1, 78.7, 78.2), p99 91.7 → 82.8 ms, GPU busy unchanged (62.9 and 63.3 ms per present), and no driver reset in either configuration over ten runs.

## Open observation

Libraries built from upstream before AnyPS5 #6763 (draw inputs copied on NVIDIA's Windows driver) got slower run after run in one session (303, 382, 443 ms), while the build with #6763 stayed stable. In those runs the GPU was 94 % busy but drew about 78 W of 285: it was waiting for PCIe reads. The cause of the growth (disk caches shared between runs?) was not investigated, because builds with #6763 do not show it.
