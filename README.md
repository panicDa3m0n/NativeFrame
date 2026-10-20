<h1 align="center">NativeFrame</h1>

<p align="center"><b>Every frame, native.</b></p>

<p align="center">Native PC ports of console games, open source.</p>

<p align="center">
  <a href="LICENSE"><img alt="License: GPL-2.0-only" src="https://img.shields.io/badge/license-GPL--2.0--only-FF6A00?style=flat-square&labelColor=1A1A1A"></a>
  <img alt="Language: C++20" src="https://img.shields.io/badge/C%2B%2B-20-F2F2F2?style=flat-square&labelColor=1A1A1A">
  <img alt="Build: CMake + Ninja" src="https://img.shields.io/badge/build-CMake%20%2B%20Ninja-6B6B6B?style=flat-square&labelColor=1A1A1A">
  <img alt="Status: experimental" src="https://img.shields.io/badge/status-experimental-FF6A00?style=flat-square&labelColor=1A1A1A">
</p>

NativeFrame turns console games you own into native PC executables — static conversion of the executable, rebuilt system libraries and Vulkan rendering — built for smooth, high-framerate play. Born from [AnyPS5](https://github.com/boykopovar/AnyPS5).

NativeFrame is a performance-focused fork of AnyPS5. AnyPS5 makes PS5 titles start and run on PC; NativeFrame takes that work and goes after the frame time, with the goal of playable frame rates (30 FPS first, then 60) on Windows.

> **Not affiliated with Sony.** NativeFrame contains no Sony code, firmware, keys or game data. You need your own, legally obtained copy of a game. See [Legal notice](#legal-notice).

## How it works

| Pillar | What it means |
|---|---|
| **Native execution** | The console's CPU is x86-64, so game code can run on a PC CPU directly. The game's executable and its bundled modules are converted ahead of time ([relinker](core/relinker)) into a native Windows PE or Linux ELF. There is no CPU emulator and no separate runtime process. |
| **Rebuilt system libraries** | The system libraries a game links against are reimplemented from scratch as native libraries ([core/libs/prx](core/libs/prx)): kernel, libc, audio, input, video output, graphics driver and more. |
| **Vulkan rendering** | The graphics driver library (AGC) decodes the PM4 command buffers the game builds and records them as Vulkan work, managing the game's memory, render targets and synchronization on a PC GPU with separate VRAM. |
| **Shader recompilation** | The game's RDNA GPU shaders are recompiled to SPIR-V ([recompiler](core/shader/recompiler)) and cached as Vulkan pipelines. |
| **High performance** | NativeFrame's own work: measuring where each frame goes and changing the architecture of the driver so the CPU and GPU spend less time per draw. See [docs/research](docs/research/README.md). |

## Project status

**Experimental.** NativeFrame is a research codebase, not a product. There are no releases and no graphical interface yet.

- Development and all measurements so far happen on one Windows 11 machine (Intel Core i7-11700K, NVIDIA RTX 4070 Ti SUPER) with one reference title, Astro Bot, in one fixed scene.
- In that scene the frame time went from about 200 ms (about 5 FPS) at the start of the fork to about 74 ms (about 13.5 FPS) with the recommended runtime switches. The first figure was measured with the draw profiler on, which adds a few milliseconds per frame. The limit today is the CPU: the chain that turns each draw call into Vulkan commands.
- The next steps, the measurements behind them and the open problems are in [docs/research](docs/research/README.md). Help is welcome: see the issues labeled **help wanted**.
- Compatibility of a title with AnyPS5 does not mean it is playable in NativeFrame, and vice versa. NativeFrame does not keep a compatibility list yet.
- Linux builds are inherited from AnyPS5 and should still work, but NativeFrame does not test them.

## Requirements

To run a converted title:

- Windows 10 or 11, 64-bit (primary target).
- An x86-64 CPU with the instruction set extensions that console code uses without checking, among them AVX2, FMA, F16C, BMI1, BMI2, LZCNT and MOVBE. A converted title checks this at start-up and names any missing extension.
- A recent desktop GPU and driver with Vulkan (1.1 at least, plus the extensions the driver checks at start-up). Development uses NVIDIA; other vendors are untested in NativeFrame.
- Your own, legally obtained copy of the game, as an unpacked and unencrypted executable with its files.

To build:

- Git, CMake 3.22.1 or newer, Ninja.
- Windows: **WinLibs MinGW-w64 GCC 15.2.0** (`x86_64-ucrt-posix-seh`, release [`15.2.0posix-14.0.0-ucrt-r7`](https://github.com/brechtsanders/winlibs_mingw/releases/tag/15.2.0posix-14.0.0-ucrt-r7)). It is the only Windows toolchain supported, as in upstream.
- Python 3.9 or newer (optional, enables the Python tests).

## Building

On Windows, from PowerShell, with the WinLibs `mingw64\bin` directory first on `PATH` for this session only:

```powershell
$env:Path = "C:\path\to\winlibs\mingw64\bin;$env:Path"
$env:SSL_CERT_FILE = "C:\Program Files\Git\ucrt64\etc\ssl\certs\ca-bundle.crt"

git clone https://github.com/panicDa3m0n/NativeFrame.git
cd NativeFrame
git -c core.longpaths=true submodule update --init --recursive

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DBUILD_TESTING=ON
cmake --build build --parallel
cmake --build build --target libs --parallel
ctest --test-dir build --output-on-failure -j 8
```

- `SSL_CERT_FILE` lets the configure step download the prebuilt FFmpeg libraries; any CA bundle works.
- The system libraries are written to `build/core/libs/libs/*.prx`. After changing a library, rebuild the `libs` target, otherwise a title keeps loading the old binaries.
- To build only the relinker, without submodules, add `-DANYPS5_RELINKER_ONLY=ON` (works on Linux, Windows and macOS).
- All CMake flags, the Linux requirements and antivirus notes are in [docs/dev/BUILD.md](docs/dev/BUILD.md). Converting and running a title is described in [docs/user/USAGE.md](docs/user/USAGE.md); runtime switches (`APS5_*` environment variables) are listed in [docs/dev/ENVIRONMENT.md](docs/dev/ENVIRONMENT.md).

The `ANYPS5_*` and `APS5_*` names of flags and switches come from upstream and are kept on purpose, so that changes can move between the two projects.

## Documentation

| Topic | Where |
|---|---|
| Research notes: CPU chain, memory model, roadmap to 30 FPS | [docs/research](docs/research/README.md) |
| How performance is measured | [docs/research/test-bench.md](docs/research/test-bench.md) |
| Architecture (upstream) | [docs/dev/ARCHITECTURE.md](docs/dev/ARCHITECTURE.md) |
| Build, flags, tracing | [docs/dev/BUILD.md](docs/dev/BUILD.md), [docs/dev/TRACING.md](docs/dev/TRACING.md) |
| Input mapping | [docs/user/INPUT_MAPPING.md](docs/user/INPUT_MAPPING.md) |
| Code conventions | [docs/dev/CONVENTIONS.md](docs/dev/CONVENTIONS.md) |

## Contributing

Contributions are welcome, from measurements on other hardware to architecture work. Read [CONTRIBUTING.md](CONTRIBUTING.md) first, and the [Code of Conduct](CODE_OF_CONDUCT.md). Security issues: [SECURITY.md](SECURITY.md).

Never attach game files, executables, dumps of memory or shaders, or frame captures to an issue or pull request.

## AI-assisted development

NativeFrame is developed with the help of AI coding assistants (Claude, by Anthropic). The maintainer directs the work, reviews and decides every change, and is the author of the commits; measurements are run on real hardware. AI assistance is declared openly here and, as upstream asks, on each pull request sent to AnyPS5. Contributors are free to use or not use such tools, and are responsible for what they submit either way.

## Legal notice

- NativeFrame is an independent project for interoperability, research, preservation and performance study. It is **not affiliated with, endorsed by or sponsored by Sony Interactive Entertainment** or any game publisher. "PlayStation" and "PS5" are trademarks of their owners and are used only to describe compatibility; they are not part of NativeFrame's name or logo. Game titles named in this repository are trademarks of their owners and are named only to describe tests and measurements.
- This repository contains **no Sony material**: no firmware, no system libraries, no cryptographic keys, no SDK files, no games, no game data. The system libraries are clean reimplementations.
- You must use your own, legally obtained games and are responsible for complying with the laws and licenses that apply to you. NativeFrame does not provide, and its issue tracker does not accept, instructions for obtaining games or for circumventing copy protection or encryption.
- Do not share game files, executables, memory or shader dumps or frame captures through this project. Such content is removed.

## License and credits

NativeFrame is licensed under the **GNU General Public License, version 2 only** ([LICENSE](LICENSE)), the same license as AnyPS5.

NativeFrame exists because of [AnyPS5](https://github.com/boykopovar/AnyPS5), created by boykopovar and built by hundreds of contributors; its full history is preserved here. Credits for the upstream project, the upstream pull requests integrated into NativeFrame and the third-party libraries are in [NOTICE](NOTICE).
