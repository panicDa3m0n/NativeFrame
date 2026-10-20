# Contributing to NativeFrame

Thank you for helping. NativeFrame is a small, experimental fork of [AnyPS5](https://github.com/boykopovar/AnyPS5) focused on frame time. The most useful contributions right now are:

- **measurements on hardware we do not have** (other CPUs, AMD and Intel GPUs, other drivers), taken as described below;
- **architecture work** on the open problems in [docs/research](docs/research/README.md), most of them tracked as issues labeled **help wanted**;
- **reviews** of the design notes: a wrong assumption found early saves weeks.

## Ground rules

These apply to issues, pull requests, discussions and every file in the repository.

- **No game content.** Never attach, upload or link game files, executables, system libraries, firmware, keys, memory dumps, shader dumps (`mem_*.bin`, `shader_*.req` or similar), frame captures (RenderDoc, Nsight, PIX) or screenshots that reproduce game assets in detail. Such content is deleted and the thread may be locked.
- **No circumvention.** Do not ask for, give or link instructions to obtain games, decrypt them or bypass copy protection. NativeFrame assumes you already have your own legally obtained, unpacked executable.
- **No confidential material.** Do not quote, paraphrase or link leaked or confidential SDK documentation or source code. Use public sources: open-source projects, published GPU ISA documents, Vulkan specifications, your own measurements.
- **Be kind and precise.** See the [Code of Conduct](CODE_OF_CONDUCT.md).

## Code

NativeFrame keeps the code rules of AnyPS5, so that changes can move between the two projects:

- [Code conventions](docs/dev/CONVENTIONS.md): naming, no comments in code except technical debt, [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/) for commit subjects and pull request titles.
- Every function either does exactly what it is supposed to do or throws. Unimplemented exports call `NotImplemented_nid_no_patch(__func__)`; never return a made-up success.
- Implement the general behaviour, never a special case for a value, address or path seen in one title. If the general behaviour is unknown, throw and record it in `docs/dev/debt/<library>.md`.
- Third-party code is a git submodule under `3rdparty/`, built from source.
- [AGENTS.md](AGENTS.md) summarizes the same rules for AI coding agents.

NativeFrame adds three rules for performance work:

1. **Behind a switch.** A change that alters how the driver schedules, caches or validates work can be turned off at runtime with an environment variable (`APS5_NO_<NAME>`), documented in [docs/dev/ENVIRONMENT.md](docs/dev/ENVIRONMENT.md). The switch is how a regression is found and how a measurement is made on one binary.
2. **With a test that can fail.** Add a test that exercises the new path and checks the bytes the GPU or the guest sees. Show that it catches a deliberate mistake: break the change on purpose (skip a check, reuse stale state) and confirm the test fails. Say in the pull request which mutation you tried.
3. **With a verification mode where reuse is involved.** When a change skips work because "nothing changed", add a mode (`APS5_VERIFY_<NAME>`) that also does the full work and stops at the first difference, as the existing memos do.

## Measuring performance

Frame times move between sessions on the same machine, sometimes by more than the effect you want to show. The full procedure is in [docs/research/test-bench.md](docs/research/test-bench.md). In short:

- Measure the **real frame time** with `APS5_PRESENT_RATE` and no profiler. `APS5_PROFILE_DRAW` and `APS5_PROFILE_GPU` are for finding *where* time goes; they cost several milliseconds per frame themselves.
- Compare **on the same binary**, with your switch off and on, in the same session, on the same scene, with warm shader and pipeline caches.
- Prefer **A-B-A**: base, change, base again, and compare the change with the mean of the two bases.
- Report the hardware (CPU, GPU, driver, OS), the commit, every `APS5_*` variable set, and the numbers, not just the conclusion. A neutral or negative result is worth reporting too.
- Never attach frame captures. Performance traces (`aps5-trace-*.json.gz`) and summary lines (`aps5-summary.jsonl`) hold timings and counts only, and are welcome.

## Building and testing

Build instructions are in the [README](README.md#building) and in [docs/dev/BUILD.md](docs/dev/BUILD.md). Before opening a pull request:

```powershell
cmake --build build --parallel
cmake --build build --target libs --parallel
ctest --test-dir build --output-on-failure -j 8
python tools/check_conventions.py --base origin/main
```

On Windows with an NVIDIA GPU some tests are skipped by design (VideoOut tests without a Vulkan surface under ctest, Linux-only tests, extensions the driver does not expose). Say in the pull request which tests failed or were skipped, if any.

## Pull requests

- One topic per pull request, based on the current `main` of NativeFrame.
- Describe **what** changed, **why** (with the measurement that motivated it), and **how it was tested** (tests, mutation, measurement).
- If the change is generic and useful to AnyPS5 as well, consider sending it upstream too, following [AnyPS5's contributing rules](https://github.com/boykopovar/AnyPS5/blob/main/CONTRIBUTING.md). Check upstream's open pull requests first, to avoid duplicating someone else's work.
- Say whether the change was written with AI assistance (`AI-assisted: yes` or `no`). It is a declaration, not a judgement: the author of the pull request is responsible for every line either way.
- By submitting a contribution you agree that it is licensed under the GNU General Public License, version 2 only, like the rest of the project, and you confirm that you have the right to submit it.

## Issues

Use the issue forms: **Bug report**, **Title compatibility report**, **Performance report** (with a measured frame) and **Technical proposal**. Search the existing issues first.

### Labels

| Label | Meaning |
|---|---|
| `help wanted` | Open problem where outside help is welcome; the issue describes what is known and how to verify a solution |
| `cpu` | CPU side: packet decoding, draw preparation, commit and recording, memory tracking |
| `gpu` | GPU side: Vulkan work, barriers, memory placement, shader recompiler output |
| `performance` | Frame-time work or a measured slowdown |
| `bug` | Wrong behaviour or a crash |
| `compatibility` | How far a title gets |
| `proposal` | Design proposal for discussion |
| `good first issue` | Small, well-scoped task |

## Questions

Open an issue with the form that fits best. For security problems, follow [SECURITY.md](SECURITY.md) instead of opening a public issue.
