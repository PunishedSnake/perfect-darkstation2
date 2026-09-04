# Perfect DarkStation 2

This directory owns the PlayStation 2 port on the `ps2` branch. The target is a
retail PS2 using the current PS2DEV/PS2SDK toolchain. No ROM or extracted game
asset is included in source control or CI artifacts.

## Current status

`pd_ps2_game` is a complete EE link frontier, not a renderer-only demo. It
contains the portable Perfect Dark runtime, ROM-backed assets, DualShock 2
input, SPU2 output, Fast3D command translation, the native GS backend and VU1
microprograms.

Real hardware has confirmed:

- system, filesystem and logger startup;
- bounded loading of the NTSC-final ROM data segment;
- GS presentation and the diagnostic renderer;
- DualShock 2 discovery and corrected stick extrema;
- the Perfect Dark legal/product-identification screen, including the
  "N64 Expansion Pak detected" status;
- EEPROM creation through the portable libultra interface.

The first normal 3D title frame after that legal screen has not yet been
confirmed. The current audit hardened the title-model load path and added
checkpoints around the Rare-logo model, but those changes still require a real
hardware run. This is not a playable release.

## Required files

Put the following in one writable directory on the launch device:

```text
pd-ps2-game.elf
pd.ntsc-final.z64
```

The ROM must be a legally obtained NTSC-final / US v1.1 big-endian `.z64`
image. Its MD5 is `e03b088b6ac9e0080440efed07c1e40f`.

On first start the game also creates these files beside the ELF:

```text
pd.ini       runtime configuration
eeprom.bin   emulated 16 Kbit cartridge EEPROM, exactly 2048 bytes
pdps2.log    durable bring-up log
```

The ELF directory is the default base and save directory because PS2 launchers
do not provide a reliable desktop-style working directory. Common PS2 device
prefixes such as `mass:`, `host:`, `mc0:` and `pfs0:` are treated as absolute.

## Build

With `PS2DEV`, `PS2SDK` and `GSKIT` exported:

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

The map file is a required build artifact. It records actual archive members,
section contributions and discarded sections after `--gc-sections`; source
presence in CMake alone is not proof that code survives the final link.

The default target builds the standalone diagnostic:

```sh
cmake --build build-ps2 -j2
# build-ps2/pd-ps2-bootstrap.elf
```

Optional hardware diagnostics use separate build directories:

```sh
cmake -S port/ps2 -B build-ps2-alpha-diag -G Ninja \
  -DPD_PS2_ALPHA_TRILERP_DIAGNOSTIC=ON
cmake --build build-ps2-alpha-diag -j2

cmake -S port/ps2 -B build-ps2-vu1-diag -G Ninja \
  -DPD_PS2_VU1_COLOR_DIAGNOSTIC=ON
cmake --build build-ps2-vu1-diag -j2
```

CI builds and inspects all three configurations, runs backend-independent host
tests, rejects undefined symbols in the game ELF, and publishes the game ELF
together with its linker map.

## Runtime options useful during bring-up

| Option | Effect |
| --- | --- |
| `--rom-file <path>` | Override the ROM path. |
| `--eeprom-file <path>` | Override the EEPROM path. |
| `--basedir <path>` | Override the data root. |
| `--savedir <path>` | Override config/save output. |
| `--boot-stage <number>` | Start at a selected stage number. |
| `--skip-intro` | Start at CI training instead of the title sequence. |
| `--no-sound` | Disable the game audio heap and output. |
| `--no-log` | Disable the file log for timing experiments. |
| `--profile <number>` | Select a player profile where supported. |

Input or SPU2 startup failure is non-fatal. A failed SPU2 backend automatically
disables the game audio heap. ROM, video, required asset, and required-memory
failures are fatal.

On PS2 a fatal error writes and closes `pdps2.log`, then deliberately holds the
EE instead of returning to the OSD. Reset the console manually after copying the
log. This makes a controlled failure distinguishable from an uncontrolled
exception or hardware reset.

## Renderer boundary

The active game path is:

```text
Perfect Dark GBI -> portable Fast3D -> PS2 combiner/pass planner
                 -> VIF1/VU1 PATH1 where supported
                 -> PATH3 CPU fallback otherwise
                 -> GS framebuffer -> VBlank presentation
```

Implemented now:

- color and textured triangles;
- depth, scissor, viewport, fog, alpha test and texture alpha state;
- N64 TMEM load semantics and texture conversion;
- native VRAM residency and eviction;
- one-pass and selected multipass combiner plans;
- VU1 textured transform and GS-ready packet transport;
- renderer counters and controller-triggered durable snapshots.

Known incomplete areas:

- unsupported combiner recipes are logged and dropped;
- offscreen framebuffer effects and framebuffer copies are not implemented;
- mipmap generation and sampling are not implemented;
- runtime display-mode changes are not implemented;
- centralized Vtx/Mtx/colour arena allocations are bounds checked, and the
  master display list is checked at PS2 frame phase boundaries; individual
  display-list writers do not yet reserve their worst-case command count;
- visual fidelity of text, blend equations and all title/game effects needs
  systematic real-hardware validation.

See [the native renderer architecture](../../docs/PS2_NATIVE_RENDERER_ARCHITECTURE.md)
for design details and [the startup chain](../../docs/PS2_STARTUP_CHAIN.md) for
the complete execution path.

## Hardware test handoff

For each run record:

- ELF SHA-256 and embedded commit shown in `pdps2.log`;
- console model, launch device and loader;
- exact last durable `runtime:` or `title:` checkpoint;
- whether the console held, returned to OSD, or reset;
- a photograph for visual corruption;
- `pdps2.log`, `pd.ini`, and `eeprom.bin` when created;
- Triangle/Select renderer snapshots when the frame loop is alive.

Emulators are useful for functional inspection. Timing, DMA ordering, VIF/VU
hazards, GS FIFO behavior and device I/O must be accepted only after testing on
real hardware.

## More documentation

- [Startup and first-frame chain](../../docs/PS2_STARTUP_CHAIN.md)
- [Code and file audit](../../docs/PS2_CODE_AND_FILE_AUDIT.md)
- [Native renderer architecture](../../docs/PS2_NATIVE_RENDERER_ARCHITECTURE.md)
- [N64 RDP/TMEM semantics](../../docs/N64_RDP_TMEM_SEMANTICS.md)
- [Diagnostic test procedure](PROTOTYPE_TEST.md)
- [VU1 diagnostic](VU1_COLOR_DIAGNOSTIC.md)
