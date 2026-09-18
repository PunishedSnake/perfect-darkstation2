# Perfect DarkStation 2

[![PS2 Bootstrap CI](https://github.com/PunishedSnake/perfect-darkstation2/actions/workflows/ps2-bootstrap.yml/badge.svg?branch=ps2)](https://github.com/PunishedSnake/perfect-darkstation2/actions/workflows/ps2-bootstrap.yml)

**Perfect DarkStation 2** is an experimental open-source port of *Perfect Dark* to the original Sony PlayStation 2.

The project is based on the [Perfect Dark PC port](https://github.com/fgsfdsfgs/perfect_dark) and the original [Perfect Dark decompilation](https://github.com/n64decomp/perfect_dark), but the `ps2` branch replaces the desktop rendering/platform path with a native PlayStation 2 runtime targeting the Emotion Engine, Graphics Synthesizer, VU1, SPU2 and DualShock 2.

The goal is not emulation. The game is being brought up as a native PS2 ELF and is continuously tested on real retail hardware.

> **Current state:** the port boots on real PlayStation 2 hardware, passes the legal screen and the Rare, Nintendo 64 and Perfect Dark logo sequence, reaches the main menu and can begin mission loading. It is **not yet a playable release**. Rendering correctness and performance remain the main blockers.

## What works

The PS2 branch is a full game-runtime bring-up rather than a renderer-only prototype.

Confirmed or implemented so far:

- native PS2 EE executable built with the current PS2DEV/PS2SDK toolchain;
- ROM-backed game data without shipping copyrighted ROM or extracted game assets;
- bounded streaming of the NTSC-final ROM data and runtime asset loading;
- portable Perfect Dark runtime linked into the PS2 ELF;
- native Graphics Synthesizer backend;
- Fast3D/RDP command translation for the PS2 renderer;
- N64 TMEM load semantics and PS2 texture conversion;
- colour and textured triangle rendering;
- depth, viewport, scissor, fog, alpha-test and texture-alpha state;
- VRAM allocation, texture residency and eviction;
- one-pass and selected multipass combiner plans;
- VIF1/VU1 PATH1 rendering where supported, with CPU/PATH3 fallback;
- VU1 transform/microprogram path;
- DualShock 2 discovery and analog input;
- SPU2 audio backend and portable sound integration;
- PS2 filesystem, configuration and save paths;
- `pd.ini` creation;
- emulated cartridge EEPROM through `eeprom.bin`;
- title sequence progression through all startup logos;
- main-menu entry and mission-loading path on retail hardware;
- renderer counters, durable checkpoints and hardware-oriented diagnostics;
- binary renderer/GS trace capture for offline analysis;
- host-side regression tests for renderer components;
- dedicated PS2 GitHub Actions CI and linker-map artifacts.

## Current limitations

The runtime is substantially further along than the graphics currently make it look. Humanity has once again discovered that reaching a menu is easier than reproducing an N64 renderer correctly on completely unrelated hardware.

Major remaining work includes:

- corrupted or inaccurate textures, materials and effects in title/menu/game scenes;
- unsupported Fast3D combiner recipes which are currently recorded and dropped;
- incomplete blending and text fidelity;
- off-screen render targets, framebuffer copies and framebuffer-based effects;
- mipmap generation and sampling;
- further VIF1/VU1 and PATH3 synchronization validation;
- remaining display-list command-budget hardening;
- major performance work before the game can be considered playable.

Real-hardware validation is authoritative for DMA ordering, VIF/VU behaviour, GS FIFO behaviour, storage I/O and timing.

## Renderer architecture

The active graphics path is:

```text
Perfect Dark GBI
      |
      v
portable Fast3D frontend
      |
      v
N64 RDP/TMEM state model
      |
      v
PS2 combiner + pass planner
      |
      +--> VIF1 / VU1 / PATH1 where supported
      |
      +--> CPU / PATH3 fallback
      |
      v
Graphics Synthesizer
      |
      v
VBlank presentation
```

This backend is intentionally native to the PS2. It does not wrap OpenGL and it does not depend on an emulator-specific rendering interface.

Design and implementation details are documented in:

- [Native PS2 renderer architecture](docs/PS2_NATIVE_RENDERER_ARCHITECTURE.md)
- [N64 RDP/TMEM semantics](docs/N64_RDP_TMEM_SEMANTICS.md)
- [Renderer correctness audit](docs/PS2_RENDERER_CORRECTNESS_AUDIT.md)
- [PS2 optimization roadmap](docs/PS2_OPTIMIZATION_ROADMAP.md)
- [Whole-runtime optimization audit](docs/PS2_WHOLE_RUNTIME_OPTIMIZATION_AUDIT.md)

## Retail-hardware milestone

The current tested startup chain on a retail PlayStation 2 is:

```text
ELF startup
 -> filesystem/config
 -> ROM validation and data materialization
 -> DualShock 2 / SPU2 / GS initialization
 -> Perfect Dark runtime initialization
 -> LEGAL / Expansion Pak screen
 -> Rare logo
 -> Nintendo 64 logo
 -> Perfect Dark logo
 -> main menu
 -> mission loading
```

See [PS2 startup and first-frame chain](docs/PS2_STARTUP_CHAIN.md) for the detailed execution path and failure boundaries.

## Requirements

The repository does **not** contain a Perfect Dark ROM or extracted copyrighted game assets.

The PS2 port currently targets a legally obtained:

- Perfect Dark NTSC-final / US v1.1 / US Rev 1 ROM
- big-endian `.z64` image
- MD5: `e03b088b6ac9e0080440efed07c1e40f`

Place the ROM beside the ELF as:

```text
pd.ntsc-final.z64
```

The normal runtime directory contains:

```text
pd-ps2-game.elf
pd.ntsc-final.z64

pd.ini       # generated runtime configuration
eeprom.bin   # generated 2048-byte emulated cartridge EEPROM
```

Diagnostic builds may additionally create `pdps2.log` or `pdps2-gs-trace.bin`.

## Building

Install the current [PS2DEV](https://github.com/ps2dev/ps2dev) toolchain and ensure `PS2DEV`, `PS2SDK` and `GSKIT` are exported.

Configure and build the normal correctness-oriented game ELF:

```sh
cmake -S port/ps2 -B build-ps2 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/port/ps2/ps2dev-toolchain.cmake"

cmake --build build-ps2 --target pd_ps2_game -j2
```

Outputs:

```text
build-ps2/pd-ps2-game.elf
build-ps2/pd-ps2-game.map
```

The default build uses the current `Og` correctness baseline.

For a controlled optimized comparison build:

```sh
cmake -S port/ps2 -B build-ps2-o2 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/port/ps2/ps2dev-toolchain.cmake" \
  -DPD_PS2_OPTIMIZATION=O2

cmake --build build-ps2-o2 --target pd_ps2_game -j2
```

A separate standalone hardware diagnostic ELF can be built with:

```sh
cmake --build build-ps2 -j2
```

which produces `pd-ps2-bootstrap.elf`.

More detailed build, launch and diagnostic instructions live in [port/ps2/README.md](port/ps2/README.md).

## Useful runtime options

| Option | Purpose |
| --- | --- |
| `--rom-file <path>` | Override the ROM path. |
| `--eeprom-file <path>` | Override the EEPROM path. |
| `--basedir <path>` | Override the runtime data root. |
| `--savedir <path>` | Override config/save output. |
| `--boot-stage <number>` | Start at a selected stage. |
| `--skip-intro` | Start at CI Training instead of the title sequence. |
| `--no-sound` | Disable game audio/output for diagnostics. |
| `--file-log` | Enable synchronous durable file logging. |
| `--no-log` | Force the filesystem log sink off. |
| `--profile <number>` | Select a player profile where supported. |

## Renderer trace capture

The normal game build can record a single renderer frame without globally enabling synchronous file logging.

From the main menu, selecting **Carrington Institute** arms the recorder. After the transition, a complete scene frame is captured to:

```text
pdps2-gs-trace.bin
```

The trace records renderer state, combiner recipes, pass graphs, texture metadata, PATH1 submission information, raw PATH3 GIF qwords, GS register shadow state, VRAM allocation state and renderer statistics.

Decode it on a host system with:

```sh
python3 tools/ps2_renderer_trace_decode.py pdps2-gs-trace.bin \
  --json pdps2-gs-trace.json
```

See [PS2 renderer trace](docs/PS2_RENDERER_TRACE.md) for the capture format and procedure.

## CI and testing

The `ps2` branch has dedicated GitHub Actions CI using the PS2DEV container.

CI currently:

- builds the PS2 game and diagnostic targets;
- builds correctness and optimized variants where requested;
- runs backend-independent Fast3D/TMEM tests;
- runs GS state, clipping, allocator, combiner and renderer regression tests;
- checks the EE link frontier;
- rejects unresolved symbols;
- publishes ELF and linker-map artifacts used during hardware testing.

The linker map is treated as part of the diagnostic output so code ownership, section survival and memory use can be checked against the actual final ELF.

## Development status and priorities

The project has moved beyond platform bootstrap. Current development is concentrated on renderer correctness and performance.

The immediate priorities are:

1. use retail-hardware GS traces to identify the remaining incorrect or unsupported renderer states;
2. close high-frequency combiner and texture-path gaps;
3. validate synchronization and packet ordering across PATH1/VU1 and PATH3;
4. implement framebuffer effects required by menu and gameplay scenes;
5. profile and remove the largest CPU, packet and synchronization bottlenecks;
6. reach stable, visually correct and playable gameplay on original PS2 hardware.

For the larger audit and remaining risks, see:

- [PS2 code and file audit](docs/PS2_CODE_AND_FILE_AUDIT.md)
- [PS2 modern optimization audit](docs/PS2_MODERN_OPTIMIZATION_AUDIT.md)
- [PS2 optimization roadmap](docs/PS2_OPTIMIZATION_ROADMAP.md)
- [SM64 PS2 comparison](docs/PS2_SM64_PORT_COMPARISON.md)

## Project lineage and credits

Perfect DarkStation 2 exists because of the work done by several earlier projects and contributors.

In particular:

- the [Perfect Dark decompilation](https://github.com/n64decomp/perfect_dark) project and its contributors;
- [fgsfdsfgs/perfect_dark](https://github.com/fgsfdsfgs/perfect_dark), which provides the portable runtime this PS2 effort is based on;
- Ryan Dwyer for the original decompilation work, tooling and `pd-extract`;
- doomhack's earlier [Perfect Dark porting effort](https://github.com/doomhack/perfect_dark);
- the [sm64-port](https://github.com/sm64-port/sm64-port) contributors;
- the Ship of Harkinian / libultraship Fast3D work and its contributors;
- PS2DEV, PS2SDK and gsKit contributors;
- everyone testing the PS2 ELF on real hardware and contributing reports, logs and traces.

See the repository history for individual code contributions.

## License

Source code in this repository is distributed under the [MIT License](LICENSE).

*Perfect Dark*, Nintendo 64, PlayStation and PlayStation 2 are trademarks of their respective owners. This project is an unofficial fan-made port and is not affiliated with or endorsed by Nintendo, Rare, Microsoft or Sony.
