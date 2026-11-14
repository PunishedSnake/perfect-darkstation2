# Perfect DarkStation 2

[![PS2 CI](https://github.com/PunishedSnake/perfect-darkstation2/actions/workflows/ps2-ci.yml/badge.svg?branch=ps2)](https://github.com/PunishedSnake/perfect-darkstation2/actions/workflows/ps2-ci.yml)

**Perfect DarkStation 2** is an experimental open-source port of *Perfect Dark* to the original Sony PlayStation 2.

The project is based on the [Perfect Dark PC port](https://github.com/fgsfdsfgs/perfect_dark) and the original [Perfect Dark decompilation](https://github.com/n64decomp/perfect_dark), but the `ps2` branch replaces the desktop rendering/platform path with a native PlayStation 2 runtime targeting the Emotion Engine, Graphics Synthesizer, VU1, SPU2 and DualShock 2.

The goal is not emulation. The game is being brought up as a native PS2 ELF and is continuously tested on real retail hardware.

> **Current state (November 2026):** the game boots on retail PlayStation 2 hardware, runs the full title sequence and menus, and loads and runs missions with cutscenes, characters, sound and speech. It is **not yet a release**: some materials still render wrong or through slow fallbacks, and heavy scenes run below the N64's frame pacing. Each development round is tested on a retail (Japanese) PS2 and measured in PCSX2.

## Scope

The aim is Perfect Dark as if Rare had shipped it on the PS2: the original game logic, levels and assets from the player's own ROM, with the rendering, audio and I/O rebuilt for the PS2's hardware instead of emulating the N64's.

In scope:

- a native EE executable that runs from USB or HDD on a retail console;
- a PS2-specific renderer (**Emotion3D**) that uses the GS, VU0 and VU1 directly;
- audio on the SPU2's hardware voices and reverb, driven from the IOP;
- PS2 controls, video modes (480i/480p/1080i), saves and settings;
- rendering that matches the N64 version, then performance on real hardware.

Out of scope:

- shipping the ROM or any extracted game data;
- emulator-only fixes that are not checked against hardware;
- new game content or gameplay changes.

## Development

The authoritative development branch is `ps2`. New PS2 work and pull requests should target it; `port` is retained as the inherited portable/upstream baseline.

Start with:

- [PS2 development guide](docs/PS2_DEVELOPMENT.md)
- [Contributing](CONTRIBUTING.md)
- [Documentation index](docs/README.md)
- [PS2 platform quick start](port/ps2/README.md)

## What works

**Runtime**

- native PS2 EE executable built with the current PS2DEV/PS2SDK toolchain, with the IOP stack owned by the game (USB and HDD/PFS);
- ROM-backed game data: the ROM is validated and streamed at runtime, nothing copyrighted is shipped;
- the portable Perfect Dark runtime: title sequence, menus, missions, cutscenes;
- DualShock 2 input with several control schemes and button remapping;
- `pd.ini` settings and an emulated cartridge EEPROM (`eeprom.bin`) for saves;
- PS2-only options: control schemes, 480i/480p/1080i output.

**Rendering (Emotion3D)**

- a native GS backend (no OpenGL, no gsKit in the frame path) with its own GIF/VIF packet queue;
- Fast3D/RDP translation with N64 TMEM semantics, PS2 texture conversion and VRAM residency;
- combiner recipes planned as one or more GS passes, including N64 trilerp and alpha-trilerp materials;
- VU1 microprograms for vertex transform, projection, clipping support, and GIF packing;
- VU0 macro-mode transforms, lighting and frustum tests on the EE side;
- compiled room geometry and lazy palettes (Emotion3D stages 1-2);
- character models skinned, lit and culled on VU1 (stage 5).

**Audio**

- SPU2 hardware voices for sound effects and music instruments, with SPU2 reverb per area;
- speech clips played as SPU2 voices instead of EE MP3 decoding;
- both need the optional `pd.pssb` sound bank built from your ROM (see [Sound bank](#sound-bank-optional)); without it the original software mixer is used.

**Tooling**

- host regression tests for the renderer, texture conversion, GS state, audio codec and VU1 microprograms (run through a VU simulator);
- GS/renderer trace capture on hardware and an offline decoder;
- GitHub Actions CI that builds the ELFs and publishes the linker map.

## Current limitations

- A few materials still go through slow or approximate paths: the alpha-add window material, the independent-alpha trilerp, and some two-pass blends.
- Framebuffer effects (copies, off-screen targets) are incomplete.
- Heavy scenes such as the Defection intro still take about 19-21 ms per frame in PCSX2. Hardware timing can differ, and hardware results are the ones that count.
- Only the NTSC-final (US v1.1) ROM is supported.

Real-hardware validation is authoritative for DMA ordering, VIF/VU behaviour, GS FIFO behaviour, storage I/O and timing.

## Renderer architecture

```text
Perfect Dark GBI display lists
      |
      v
Emotion3D front end
  - compiled room geometry and model programs (cached per list)
  - Fast3D/RDP state model for everything else
      |
      v
PS2 combiner + pass planner, texture/TMEM model
      |
      +--> VU1 object / skin batches (PATH1): transform, light, cull, pack
      |
      +--> EE direct draws and fallbacks (PATH3)
      |
      v
native GIF/VIF packet queue -> Graphics Synthesizer -> VBlank presentation
```

This backend is native to the PS2. It does not wrap OpenGL and does not depend on an emulator-specific rendering interface.

Design notes:

- [Emotion3D stages and measurements](docs/ps2-emotion3d.md)
- [Vector-unit geometry pipeline](docs/ps2-vu1-pipeline.md)
- [Native PS2 renderer architecture](docs/PS2_NATIVE_RENDERER_ARCHITECTURE.md)
- [N64 RDP/TMEM semantics](docs/N64_RDP_TMEM_SEMANTICS.md)
- [Renderer correctness audit](docs/PS2_RENDERER_CORRECTNESS_AUDIT.md)
- [SPU2 hardware-voice audio](docs/ps2-spu2-audio.md)
- [EE runtime library replacements](docs/ps2-ee-runtime.md)

## Startup chain

On a retail PlayStation 2 the game runs:

```text
ELF startup -> filesystem/config -> ROM validation and streaming
 -> DualShock 2 / SPU2 / GS initialization -> Perfect Dark runtime
 -> LEGAL screen -> Rare / Nintendo 64 / Perfect Dark logos
 -> main menu -> mission loading -> gameplay
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

pd.pssb      # optional SPU2 sound bank built from your ROM (see below)

pd.ini       # generated runtime configuration
eeprom.bin   # generated 2048-byte emulated cartridge EEPROM
```

Diagnostic builds may additionally create `pdps2.log` or `pdps2-gs-trace.bin`.

## Sound bank (optional)

The SPU2 hardware-voice audio needs `pd.pssb`, a PS-ADPCM bank of the game's sounds and speech. It is built from your ROM, so it cannot be distributed with the game.

1. Download `pd_spu2bank` for your system from the [SPU2 bank tool releases](https://github.com/PunishedSnake/perfect-darkstation2/releases?q=tools-spu2bank), or build it with `tools/ps2/build_spu2bank.sh`.
2. Run it on the ROM (`.z64`, `.v64` or `.n64`):

   ```sh
   pd_spu2bank pd.ntsc-final.z64      # writes pd.pssb next to the ROM
   ```

   On Windows, drag the ROM onto `pd_spu2bank-x86_64-windows-gnu.exe`.
3. Copy `pd.pssb` next to `pd-ps2-game.elf`.

It takes a few seconds and produces about 29 MiB. Without the bank, or with `Audio.HardwareVoices=0` in `pd.ini`, the game uses the original software mixer, which costs the EE several milliseconds per frame. Details: [docs/ps2-spu2-audio.md](docs/ps2-spu2-audio.md).

## Building

Install the current [PS2DEV](https://github.com/ps2dev/ps2dev) toolchain and ensure `PS2DEV`, `PS2SDK` and `GSKIT` are exported.

Configure and build the normal correctness-oriented game ELF:

```sh
cmake -S port/ps2 -B build-ps2 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/port/ps2/ps2dev-toolchain.cmake"

cmake --build build-ps2 -j2
```

Outputs:

```text
build-ps2/pd-ps2-game.elf        # normal launcher ELF
build-ps2/pd-ps2-game-fmcb.elf   # same load image, symbols stripped (for FMCB-style launchers)
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

The default build produces the game ELF only. Hardware diagnostics use the
game's trace and checkpoint facilities.

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

## Useful `pd.ini` switches

These are mainly for A/B checks on hardware:

| Key | Default | Purpose |
| --- | --- | --- |
| `Audio.HardwareVoices` | 1 | SPU2 hardware voices when `pd.pssb` is present. |
| `Audio.HardwareReverb` / `Audio.HardwareReverbDepth` | 1 / 100 | SPU2 reverb and its return level (%). |
| `Video.Ps2Emotion3D` | 1 | Compiled room geometry (Emotion3D stages 1-2). |
| `Video.Ps2Emotion3DSkin` | 1 | Character models on the VU1 skin path. |
| `Video.Ps2Emotion3DModels` | 0 | Model display-list replay without the skin path (measured slower). |
| `Video.Ps2ObjectGeometry` | 1 | Object-space geometry transformed on VU1. |

## Controls and video settings

Extended Options (pause menu → Options → Extended) are PS2-specific:

- **Controls**, per player: DualShock 2 port, a control scheme (*Modern*, *Shoulder Shooter*, *N64 Classic* or *Custom*), a *Button Layout* reminder screen and *Customize Buttons* remapping (Select cancels a remap). Custom layouts are stored in `pd.ini` as `Input.PlayerN.Custom.*`.
- **Video Output**: 480i, 480p or 1080i (component cable for the latter two). The 640x448 image is scaled by the GS display unit, so the mode costs no speed. It applies on the next boot; **hold START+TRIANGLE while the game boots** to fall back to 480i if a TV cannot show the chosen mode.

## Renderer trace capture

The normal game build can record a single renderer frame without globally enabling synchronous file logging.

Press **Select** on controller 1 in the scene you want to inspect. The next complete renderer frame is captured to:

```text
pdps2-gs-trace.bin
```

Each press replaces the previous capture after Select has been released. The trace records renderer state, combiner recipes, pass graphs, texture metadata, PATH1 submission information, raw PATH3 GIF qwords, GS register shadow state, VRAM allocation state and renderer statistics. A busy frame may exceed the fixed raw-qword budget; the event stream reports any loss.

Decode it on a host system with:

```sh
python3 tools/ps2_renderer_trace_decode.py pdps2-gs-trace.bin \
  --json pdps2-gs-trace.json
```

See [PS2 renderer trace](docs/PS2_RENDERER_TRACE.md) for the capture format and procedure.

## CI and testing

The `ps2` branch has dedicated GitHub Actions CI using the PS2DEV container.

CI currently:

- builds the normal `Og` PS2 game ELF;
- builds the `O2` comparison ELF only when explicitly requested through a manual workflow run;
- runs backend-independent Fast3D/TMEM tests;
- runs GS state, clipping, allocator, combiner and renderer regression tests;
- runs the VU1 microprograms (object transform, skin) through a functional simulator;
- checks the EE link frontier;
- rejects unresolved symbols;
- publishes the game ELF, linker map, section-size report and build/toolchain metadata used during hardware testing.

A separate *SPU2 bank tool* workflow builds `pd_spu2bank` for Linux, Windows and macOS.

The linker map is treated as part of the diagnostic output so code ownership, section survival and memory use can be checked against the actual final ELF.

## Development status and priorities

Development runs in rounds: a branch per round, local tests and PCSX2 A/B timing, CI, merge to `ps2`, then a test on a retail console. What each round changed and measured is recorded in the [Emotion3D notes](docs/ps2-emotion3d.md) and the pull requests.

Next:

1. cut the EE preparation cost of the VU1 skin path so characters are clearly cheaper than the old path;
2. move the remaining fallback materials (alpha-add windows, independent-alpha trilerp, two-pass blends) onto native GS paths;
3. rewrite other expensive engine features around the GS, VU0 and VU1;
4. framebuffer effects (copies, off-screen targets);
5. confirm every step on retail hardware.

For the larger audit and remaining risks, see:

- [PS2 optimization roadmap](docs/PS2_OPTIMIZATION_ROADMAP.md)
- [Whole-runtime optimization audit](docs/PS2_WHOLE_RUNTIME_OPTIMIZATION_AUDIT.md)
- [PS2 code and file audit](docs/PS2_CODE_AND_FILE_AUDIT.md)
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
