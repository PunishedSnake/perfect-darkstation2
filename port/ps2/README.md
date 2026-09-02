# Perfect DarkStation 2: PS2 bootstrap

This directory contains the PlayStation 2 bring-up path for the `ps2` branch.

Upstream baseline at bootstrap 0:

- repository lineage: `perfect-dark-pc-port/perfect_dark`
- upstream branch: `port`
- baseline commit: `32a1cb9f268dd3ac73016801025c6bbbfa20130f`
- target: retail PlayStation 2 using current PS2DEV/PS2SDK

## Status

Bootstrap 0 is intentionally small. It proves only the platform/toolchain entry point and the existing `system.h` contract.

It does **not** prove that the full game builds, renders, fits in memory, or performs acceptably on PS2.

### Confirmed from current source

- Current PS2SDK provides an EE CMake toolchain at `samples/ps2dev.cmake`.
- That toolchain selects the `mips64r5900el-ps2-elf` C/C++ compilers and defines `PLATFORM_PS2`.
- Current PS2SDK exposes `GetTimerSystemTime()` and `TimerBusClock2USec()` for EE timing.
- The current Perfect Dark port exposes platform memory, timing, logging, sleep and path services through `port/include/system.h`.

### Current implementation in this bootstrap

- `malloc/calloc/realloc/free` are used only to satisfy the existing platform contract during bring-up.
- The PS2 ELF/device directory is used as the temporary home/config fallback.
- `DelayThread()` implements the sleep contract.
- `sysCpuRelax()` remains a spin hint instead of silently changing scheduling semantics.
- Build optimization is forced to `-Og` for this target. The current upstream port already documents unresolved `-O2` issues, while current PS2SDK injects `-O2` by default.
- Project-owned PATH3/GS command submission, native texture residency, indexed
  formats, multipass combiner graphs and native framebuffer presentation are
  active behind the shared Fast3D frontend.
- An opt-in VIF1/VU1 diagnostic now submits complete GS-ready color and
  textured A+D packets through VU1 XGKICK/PATH1 while preserving the PATH3
  renderer as the automatic fallback and physical-console A/B baseline.
- The same diagnostic build routes ordinary one-pass textured Fast3D draws
  through a separate VU1 geometry microprogram: perspective divide, viewport
  and depth mapping, STQ and PACKED XYZ2/XYZF2 output. Fixed NOP-padded state
  slots and disjoint input/output banks are host-tested. CPU fallback vertices
  are still prepared eagerly; physical-console transform A/B remains pending.
- DualShock 2 input implements the portable controller contract through
  ROM-resident PADMAN modules.
- ROM data is read through a bounded file-backed source instead of retaining a
  32 MiB image in EE RAM. Ordinary assets are loaded lazily and released at the
  existing file lifetime boundary.
- The PS2 audio backend embeds `audsrv.irx`, loads ROM `LIBSD`, and streams the
  game's mixed 22050 Hz stereo PCM16 frames through an ownership-explicit,
  bounded IOP queue.

### Inference / not yet proven

Loader-specific `argv[0]` behaviour varies between launch paths. The backend handles common device-prefixed forms such as `host:` and `mass:`, but this must be checked under the loaders we actually support.

## Build

With a current PS2DEV/PS2SDK environment:

```sh
cmake -S port/ps2 -B build-ps2 \
  -DCMAKE_TOOLCHAIN_FILE="$PS2SDK/samples/ps2dev.cmake"
cmake --build build-ps2 -j
```

If `PS2SDK` is exported, the subproject can also locate the current toolchain itself:

```sh
cmake -S port/ps2 -B build-ps2
cmake --build build-ps2 -j
```

Expected output:

```text
build-ps2/pd-ps2-bootstrap.elf
```

For the first VIF1/VU1/PATH1 hardware artifact and its validation procedure,
see [VU1_COLOR_DIAGNOSTIC.md](VU1_COLOR_DIAGNOSTIC.md).

Expected runtime output:

```text
Perfect DarkStation 2 bootstrap
platform: r5900-ps2
system backend: ok
heap smoke: ok
bootstrap completed in ... us
```

Run it in PCSX2 for basic correctness/inspection, then validate it on real hardware. Emulator timing is not accepted as a performance result.

## What to record for the first hardware run

At minimum:

```text
SCPH / hardware revision
PS2SDK commit
toolchain version
build flags
loader / launch path
active IRX modules
ELF size
runtime output
```

No performance claim is attached to bootstrap 0.

## Port order

The project intentionally follows the least-risk path first:

1. platform/toolchain bootstrap;
2. filesystem and ROM access model;
3. controller input through the current PS2SDK pad stack;
4. audio correctness baseline;
5. PS2 `GfxWindowManagerAPI` semantics;
6. PS2 `GfxRenderingAPI` compile stub;
7. Fast3D command translation into a minimal GS backend;
8. first real triangles and title-screen path;
9. profiling on real hardware;
10. only then introduce data-layout, batching, DMA/VIF/VU or hand-tuned kernels where measurements justify them.

## Memory status

The desktop port still retains the complete ROM, but the PS2 path no longer
does. A file-backed `RomSource` performs bounded reads and streaming RZIP
decompression. Permanent segments receive exact-sized allocations; ordinary
assets keep compact extent metadata and become resident only while used.

The game heap now has an explicit fixed-memory policy. On PS2 the platform
measures the current libc tail after persistent subsystem startup, preserves a
4 MiB platform/streaming reserve and bounds the requested `memp` arena instead
of blindly asking `calloc` for the configured size. Diagnostic logs expose the
physical/linker/libc addresses and resulting plan without reserving the arena.
Real hardware at build `aa007d1b72cc` reported a 27,864 KiB free libc tail after
renderer startup and accepted the requested 16 MiB game arena while preserving
the 4 MiB reserve. The remaining memory work is peak lazy-asset measurement in
the real game loop.

`pd_ps2_game` is the full-runtime link frontier. It links the real game entry
point and complete game/core object sets against the PS2 filesystem, ROM,
controller, SPU2 and native renderer services. Its default data and save root is
the ELF directory, and PS2 device prefixes are absolute paths. The two retired
bring-up scenes which bypassed this maintained path have been removed. CI run
33643145244 linked the first `pd-ps2-game.elf` with zero undefined symbols and
3,528,478 bytes of text, data and BSS.

## Performance rules for this port

Before changing a hot path, identify the actual bottleneck. Prefer, in order:

1. remove work;
2. do it less often;
3. move less data;
4. improve representation/locality;
5. batch;
6. remove unnecessary copies and dynamic allocations;
7. overlap independent work;
8. use specialised PS2 hardware where the workload fits;
9. hand-specialise only the measured hot kernel.

For every major data set, track producer, consumer, lifetime, representation, alignment requirement, transport, batch size and ownership. Cache-line alignment, allocator alignment and DMA/device alignment are separate contracts.

Once frame-time measurement starts, report p50, p95, p99, max and deadline misses, not just an average FPS number.
