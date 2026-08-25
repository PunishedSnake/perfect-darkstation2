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

## Memory warning: the current ROM model is temporary by definition

The desktop port currently loads the entire 32 MiB Perfect Dark ROM into host memory. A retail PS2 has 32 MiB of EE main RAM total, so that model cannot be the final PS2 data path.

The eventual PS2 path should keep only the active working set in EE RAM and move toward storage-backed reads and/or offline prepared consumer-ready assets. The exact representation will be chosen from real access patterns rather than by blindly converting everything up front.

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
