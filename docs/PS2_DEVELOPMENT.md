# PS2 development guide

Updated: 2026-11-14

This is the canonical developer-facing guide for the Perfect DarkStation 2 port. It describes the current development branch, the shortest build/test loop and the active technical frontier.

For end-user project status, see the repository [README](../README.md). For detailed platform build/runtime notes, see [port/ps2/README.md](../port/ps2/README.md).

## Branch model

### `ps2`

`ps2` is the authoritative development branch for Perfect DarkStation 2.

New PS2 work should branch from `ps2` and pull requests should target `ps2`. The dedicated PS2 CI is also scoped to this branch.

GitHub uses `ps2` as the repository default branch. A lightweight classic
protection rule blocks force-pushes and deletion of `ps2`, but deliberately
does not require a pull request, review or status check for every update. This
keeps direct single-maintainer development possible while protecting the branch
from the two destructive accidents which are hardest to recover from.

### `port`

`port` is retained as the inherited portable/upstream baseline. It is useful for comparing PS2-specific changes with the desktop-oriented port, but new PS2 features should not be developed there.

Do not merge `ps2` back into `port` merely to make the branch graph look tidy. The PS2 branch intentionally owns platform-specific runtime and renderer work.

### Topic branches

Use short-lived branches from `ps2` for risky or independently testable changes, especially:

- renderer correctness changes;
- VIF1/VU1 ownership or packet-layout work;
- framebuffer/render-target work;
- ROM/filesystem changes;
- allocator or display-list bounds changes;
- performance experiments which need an A/B build.

Keep changes small enough that a retail-hardware regression can be bisected.

## Current frontier

Retail PlayStation 2 testing covers the full startup chain, the title
sequence, menus, mission loading and gameplay with cutscenes, characters,
SPU2 audio and speech. Reaching the game is no longer the problem.

Work is now about speed and the remaining materials, under the rule "don't do
unnecessary work": move work to the GS, VU0, VU1 and IOP where it wins on
hardware, and keep the EE for game logic.

Done so far (details in [ps2-emotion3d.md](ps2-emotion3d.md) and the pull
requests):

| Area | State |
| --- | --- |
| GS transport | Native GIF/VIF packet queue, arena self-spill (no dropped frames), packed V4-16 object vertices. |
| Rooms | Compiled room geometry and lazy palettes (Emotion3D stages 1-2). |
| Object geometry | Object-space G_VTX blocks transformed and packed on VU1. |
| Characters | VU1 skin: palette per matrix epoch, Fast3D lighting, texgen, LOD alpha, cull (stage 5, default on). |
| Materials | One-pass and multipass combiner plans; same-sample alpha trilerp on VU1; two-pass opaque trilerp on the skin path; texture clamps on the direct path. |
| Audio | SPU2 hardware voices, per-area reverb and speech from the `pd.pssb` bank via the project IOP module `pdsnd.irx`. |
| EE runtime | EE-specific soft-double, memcpy/memset; VU0 transforms and lighting. |

Open, in order:

1. EE preparation cost of the skin path (matrix snaps, per-load bounds):
   characters should be clearly cheaper, not just at parity.
2. Fallback materials: the alpha-add window material (graph 5),
   independent-alpha trilerp (graph 4), two-pass input lerp (graph 2).
3. Engine features rewritten around the GS and vector units.
4. Framebuffer copies and off-screen effects.
5. Hardware confirmation of every PCSX2 measurement.

The port is not yet a release.

## Build loop

With `PS2DEV`, `PS2SDK` and `GSKIT` exported:

```sh
cmake -S port/ps2 -B build-ps2 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/port/ps2/ps2dev-toolchain.cmake"

cmake --build build-ps2 --target pd_ps2_game -j2
```

Primary outputs:

```text
build-ps2/pd-ps2-game.elf
build-ps2/pd-ps2-game.map
```

Use the normal `Og` build as the correctness baseline. Build `O2` separately for controlled comparisons:

```sh
cmake -S port/ps2 -B build-ps2-o2 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/port/ps2/ps2dev-toolchain.cmake" \
  -DPD_PS2_OPTIMIZATION=O2

cmake --build build-ps2-o2 --target pd_ps2_game -j2
```

A third profile keeps the Og baseline for game logic but compiles only the
renderer and Fast3D hot path (`gfx_ps2.cpp`, `gs_*`, the generated Fast3D
frontend, TMEM model) at `-O2` (roadmap item 7):

```sh
cmake -S port/ps2 -B build-ps2-hot-o2 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/port/ps2/ps2dev-toolchain.cmake" \
  -DPD_PS2_HOT_PATH_O2=ON
```

The build log reports the profile as `Og+hotO2`. CI publishes it only on a
manual run with `build_hot_o2`.

Never compare different optimization profiles, ROMs, logging modes or scenes and call the difference a performance result.

## Runtime test set

A useful hardware test should identify:

- commit SHA / ELF identity;
- ELF SHA-256;
- Og or O2 profile;
- PS2 model/revision and loader;
- launch device;
- ROM identity;
- stage/scene;
- whether sound is enabled;
- whether synchronous file logging is enabled;
- visible corruption;
- whether the title/menu/mission path still progresses;
- renderer trace or diagnostic log when relevant.

For performance evidence, additionally record the PS2SDK/toolchain identity,
active IRX set, video mode, sample count, units, correctness hash and
`p50`/`p95`/`p99`/maximum/deadline misses. If the current instrumentation does
not expose a field, write `not captured` instead of silently inventing it.

The normal build should keep file logging disabled. Synchronous writes to `mass:` have already been proven capable of stalling frame progress on tested hardware.

## Debugging hierarchy

Use the least invasive tool that can answer the question.

### 1. Host regression tests

Use host-side tests for deterministic contracts such as:

- TMEM decode;
- texture conversion;
- combiner/pass planning;
- clipping;
- GS register equations;
- VRAM allocation;
- state shadowing;
- command-budget rules;
- input mapping;
- bounded memory helpers.

If a hardware problem can be represented as a pure contract, add a host regression test before or with the fix.

### 2. Normal console logging

Console output is appropriate for sparse startup and state-transition information. Avoid per-vertex or per-command logging in the frame loop.

### 3. Renderer counters and snapshots

Use renderer counters to determine whether geometry is going through PATH1/PATH3, being clipped, or being dropped by an unsupported recipe before adding more invasive tracing.

### 4. Binary GS/renderer trace

Use [PS2_RENDERER_TRACE.md](PS2_RENDERER_TRACE.md) when a complete scene frame needs to be reconstructed offline.

The capture is intentionally synchronous only for the selected diagnostic frame. Ordinary gameplay frames do not write the trace.

### 5. File logging

Enable `--file-log` only for controlled startup/fatal-boundary diagnostics. Do not use it as a normal performance or gameplay logging mode.

### 6. PCSX2 timing

For A/B timing, run the same ELF twice with different `pd.ini` switches
(for example `Video.Ps2Emotion3DSkin=0/1`) over the same scene and compare
per-frame `gfx`/`logic` times. Emulator timing ranks changes; only hardware
confirms them. See [PS2_PCSX2_DEBUGGING.md](PS2_PCSX2_DEBUGGING.md).

### 7. Hardware test ELF

Use the owned-IOP game ELF from the current PS2 CI run. Earlier bootstrap and
VU1 color test results are historical records:

- [Prototype test](../port/ps2/PROTOTYPE_TEST.md)
- [VU1 colour diagnostic](../port/ps2/VU1_COLOR_DIAGNOSTIC.md)

## Renderer ownership

The active path is:

```text
Perfect Dark GBI
 -> Emotion3D front end (compiled rooms, model programs) / Fast3D for the rest
 -> RDP/TMEM model
 -> PS2 combiner/pass planner
 -> VU1 object and skin batches (PATH1) when supported
 -> EE direct draws / fallbacks (PATH3) otherwise
 -> native GIF/VIF queue -> GS
 -> VBlank presentation
```

The most important rule is to keep ownership explicit. A packet, VU bank, render target or arena must not be reused until the previous owner is proven idle.

See [PS2_NATIVE_RENDERER_ARCHITECTURE.md](PS2_NATIVE_RENDERER_ARCHITECTURE.md) before changing transport or GS-state lifetime.

## Correctness labels

When documenting or reviewing renderer work, use these meanings consistently:

- **CONFIRMED**: proven by code contract, host test or controlled hardware evidence.
- **CURRENT IMPLEMENTATION**: describes what the code does now, without claiming visual correctness.
- **HARDWARE-VALIDATED**: directly observed on retail PS2 hardware.
- **HYPOTHESIS TO TEST**: plausible explanation which must not be treated as a fix until tested.

This distinction matters because a visually improved diagnostic build can still be technically wrong.

## Performance rules

Do not optimize an incorrect renderer by hiding work.

Prioritize:

1. remove duplicate state changes, copies and waits;
2. batch work without changing results;
3. move regular measured work to VU/MMI only when it wins end-to-end;
4. use selective compiler optimization after hardware measurement;
5. approximate behaviour only behind an explicit experiment and only when the compatibility cost is understood.

The performance backlog is maintained in:

- [PS2_OPTIMIZATION_ROADMAP.md](PS2_OPTIMIZATION_ROADMAP.md)
- [PS2_WHOLE_RUNTIME_OPTIMIZATION_AUDIT.md](PS2_WHOLE_RUNTIME_OPTIMIZATION_AUDIT.md)

## Before committing renderer changes

At minimum:

1. run the relevant host tests;
2. build the normal PS2 game ELF;
3. inspect for new unresolved symbols or build warnings;
4. preserve the linker map;
5. if packet/GS/VU ownership changed, run the narrowest relevant hardware test;
6. if visual output changed, capture the exact scene and compare against the previous known build;
7. update the relevant living document if a contract or project frontier changed.

## Where to record new findings

- Project/development frontier -> this file.
- Renderer ownership/design -> [PS2_NATIVE_RENDERER_ARCHITECTURE.md](PS2_NATIVE_RENDERER_ARCHITECTURE.md).
- Confirmed visual/state contract -> [PS2_RENDERER_CORRECTNESS_AUDIT.md](PS2_RENDERER_CORRECTNESS_AUDIT.md).
- Performance priority -> [PS2_OPTIMIZATION_ROADMAP.md](PS2_OPTIMIZATION_ROADMAP.md).
- Runtime call-chain/fatal boundary -> [PS2_STARTUP_CHAIN.md](PS2_STARTUP_CHAIN.md).
- One-off dated audit -> create or update the relevant audit without presenting old observations as the current frontier.
- 2026-09-18 whole-port snapshot -> [PS2_PORT_AUDIT_2026-09-18.md](PS2_PORT_AUDIT_2026-09-18.md).

The documentation index is [docs/README.md](README.md).
