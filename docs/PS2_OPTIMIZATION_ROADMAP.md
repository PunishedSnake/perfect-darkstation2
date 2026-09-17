# PS2 optimization roadmap

Date: 2026-09-17

This document collects the practical optimization opportunities for the whole
Perfect Dark PS2 port, from instruction-level substitutions to replacement of
major runtime components. It is a roadmap, not permission to weaken rendering
or gameplay correctness. Every optimization must be measured on retail PS2
hardware and compared with an unchanged reference build.

## Ground rules

- Correctness comes before speed. A faster black screen is not progress.
- Change one independently testable contract at a time.
- Use the same ROM, save, scene and camera path for A/B measurements.
- Prefer removal of duplicated work, waits, copies and state changes before
  approximating results.
- Keep risky approximations behind explicit build profiles until hardware
  proves them safe.
- Record CPU time, GS/VU waits, SIF waits, I/O bytes and visible regressions.
- Do not enable global `-ffast-math`, `-O3` or broad LTO without evidence.

## Current baseline

The port already has several PS2-specific foundations:

- native GS rendering with GIF/VIF submission;
- batched VU1/PATH1 transform for eligible geometry;
- native CI4/CI8 and intensity texture paths;
- GS state shadowing and packet batching;
- frame arenas and fixed renderer storage;
- PS2 hardware `sqrt.s` implementation;
- cached texture-coordinate scales;
- four-line ROM read-ahead cache;
- reduced synchronous audio status RPC traffic;
- hot STL renderer caches replaced with fixed storage;
- `libstdc++.a` removed from the PS2 ELF;
- default file logging disabled because synchronous `mass:` writes can stall
  gameplay indefinitely.

The current priority remains renderer correctness, especially texture/tile
semantics, alpha materials and framebuffer effects. The optimizations below
must not obscure those faults.

## Small and local changes

| # | Candidate | Potential gain | Cost | Risk / condition |
|---:|---|---|---|---|
| 1 | Add paired `pd_sincosf` for matrix and camera construction | Medium in math-heavy frames | Low | Preserve the existing approximation and range-reduction contract |
| 2 | Use COP1/VU0 reciprocal square root for proven normalization paths | Medium | Low-Medium | Only finite, nonzero vectors; never globally replace length calculations |
| 3 | Remove accidental `double` constants and 64-bit arithmetic in hot paths | Small-Medium | Low | Inspect generated R5900 assembly and retain required precision |
| 4 | Add aligned 128-bit copy/fill kernels | Small-Medium | Low | Useful only for aligned, sufficiently large blocks |
| 5 | Use R5900 MMI for texture packing, PCM mixing, CRC and byte conversion | Medium | Medium | Introduce only after byte-volume profiling identifies a dominant loop |
| 6 | Add measured `PREF` prefetches in sequential loops | Small | Low | Bad prefetch distance can evict useful 8 KiB D-cache data |
| 7 | Compile proven hot files with selective `-O2` | Medium | Low | Hardware A/B required; do not make the whole program one profile |
| 8 | Compile large cold parsers/UI modules with `-Os` | Small-Medium | Low | Verify that reduced I-cache pressure beats slower local code |
| 9 | Order hot functions and isolate cold error paths in the linker layout | Small-Medium | Medium | Requires stable phase profiling or hardware counters |
| 10 | Replace repeated division with cached reciprocal/scale values | Small-Medium | Low | Only for immutable or explicitly invalidated state |

## C and C++ runtime / libraries

| # | Candidate | Potential gain | Cost | Risk / condition |
|---:|---|---|---|---|
| 11 | Continue replacing dynamic STL use with fixed-capacity PS2 storage | Medium | Medium | Capacity exhaustion must fail visibly and safely |
| 12 | Audit Newlib calls and provide tiny PS2-specific hot implementations | Medium | Medium | Keep full semantics where the game depends on them |
| 13 | Evaluate a smaller Newlib/nano-style configuration | Small-Medium, mostly footprint | Medium | PS2SDK ABI, errno, locale and stdio behavior must remain compatible |
| 14 | Keep lifetime arenas; add fixed pools for repeated same-size objects | Medium | Medium | Do not replace correct stage/permanent arena semantics with general malloc |
| 15 | Evaluate TLSF or segregated lists only for truly dynamic heaps | Medium in allocator-heavy workloads | Medium-High | Requires a captured allocation trace and fragmentation telemetry |
| 16 | Replace zlib only after measuring decompression | Medium-High if dominant | Medium-High | `libdeflate` is whole-buffer oriented; `miniz` is portable but not automatically faster; SIMD-focused zlib-ng paths do not directly target R5900 |
| 17 | Use block-based LZ4 or tuned DEFLATE for new PS2 asset packs | High for loading/streaming | High | Requires an offline repacker and versioned asset format |

## Renderer and GS submission

| # | Candidate | Potential gain | Cost | Risk / condition |
|---:|---|---|---|---|
| 18 | Preconvert textures offline to final GS formats and swizzles | High | Medium-High | First fix TMEM, tile, TLUT, NPOT, clamp and mirror correctness |
| 19 | Add persistent VRAM residency with explicit eviction | High | High | Needs deterministic ownership, CLUT tracking and safe invalidation |
| 20 | Suppress redundant `TEX0`, `CLAMP`, `TEST`, `ALPHA`, `ZBUF` and `TEXFLUSH` changes | Medium-High | Medium | Shadow state must include every value that changes the result |
| 21 | Sort or bucket draws by compatible material/state within safe ordering boundaries | High | High | Transparency, framebuffer effects and depth-only passes constrain reordering |
| 22 | Compile known combiner equations into cached GS pass recipes | High | High | Every shortcut needs a proof of equivalence, including alpha |
| 23 | Cache translated display-list segments and invalidate by source/state key | High | High | Dynamic matrices, lights, textures and segment bases must be part of the key |
| 24 | Build larger GIF/VIF packets and reduce DMA submissions | High | Medium | Preserve PATH ownership, packet limits and synchronization |
| 25 | Remove duplicate EE transform work when VU1 submission succeeds | High | Medium | Prevalidate VU1 batches; run the exact EE fallback only after failure |
| 26 | Pack VIF input and let VIF/VU1 expand attributes | High | High | Changes the vertex ABI and VU1 microprogram contract |
| 27 | Move full transform, lighting and clipping to VU1 | Very high | Very high | Requires robust homogeneous clipping and tested fallbacks |
| 28 | Specialize VU1 microprograms by common material class | High | Very high | Manage micro-memory and switching costs explicitly |
| 29 | Reduce or dynamically scale the 3D render resolution | High when GS-bound | Medium | Keep UI readable and preserve aspect/viewport contracts |
| 30 | Re-evaluate framebuffer and Z formats | Medium-High | Medium-High | Precision, masks, alpha and copy effects must remain correct |
| 31 | Alias transient render targets by non-overlapping lifetime | Medium, mostly VRAM capacity | High | Needs a real frame graph and explicit synchronization |
| 32 | Replace per-draw framebuffer copies with explicit effect passes | High | High | Requires complete knowledge of N64 framebuffer semantics used by the game |

## Game logic, animation and data layout

| # | Candidate | Potential gain | Cost | Risk / condition |
|---:|---|---|---|---|
| 33 | Split hot and cold fields of props, characters and effects | Medium-High | High | Requires profiling and careful save/runtime ABI handling |
| 34 | Use SoA/AoSoA for repeated transforms, visibility and animation data | High | High | Best for regular batches; poor fit for branch-heavy object ownership |
| 35 | Bucket objects by update state instead of scanning every object uniformly | High | Medium-High | Preserve update order where gameplay depends on it |
| 36 | Batch skeletal animation and skinning | High | High | Strong VU0/VU1 candidate after correctness and workload profiling |
| 37 | Improve collision broadphase and room/portal filtering | High in busy stages | High | Must preserve exact collision and visibility edge cases |
| 38 | Time-slice noncritical AI, effects and housekeeping | Medium-High | Medium | Never time-slice decisions whose latency changes gameplay |
| 39 | Move cold debug, parser and setup code out of the hot executable layout | Small-Medium | Medium | Preserve diagnostics in explicit development profiles |

## Storage, IOP, SIF and loading

| # | Candidate | Potential gain | Cost | Risk / condition |
|---:|---|---|---|---|
| 40 | Build an asynchronous IOP streaming service | Very high | Very high | Requires request queues, cancellation, ownership and bounded latency |
| 41 | Add a priority read queue for current room, audio and background assets | High | High | Avoid starvation and priority inversion |
| 42 | Pack assets into large aligned extents instead of many tiny reads | High | Medium-High | Requires offline packing and versioned lookup tables |
| 43 | Stream-decompress directly into final/stage ownership | Medium-High | High | Decoder output and asset preprocessing contracts must allow it |
| 44 | Use HDD/PFS for development or installations where available | High versus USB 1.1 | Low-Medium | Cannot be the only supported deployment path |
| 45 | Keep all file logging out of the gameplay path | Critical stability gain | Done | Diagnostic file logging remains explicit opt-in only |

## Audio and SPU2

| # | Candidate | Potential gain | Cost | Risk / condition |
|---:|---|---|---|---|
| 46 | Keep reusable sound effects as PS-ADPCM in SPU2 RAM | High | High | Needs residency, voice ownership and offline encoding |
| 47 | Move long-stream refill and scheduling to an IOP worker | High | High | Requires underrun-safe buffering and SIF batching |
| 48 | Replace general `audsrv` use with a game-specific SPU2 service | Very high | Very high | Large maintenance and synchronization cost; do only after profiling |
| 49 | Batch EE-to-IOP audio control messages | Medium-High | Medium | Preserve sample-accurate starts where required |
| 50 | Add MMI/VU0 mixing and format-conversion kernels | Medium | Medium | Only if EE mixing remains a measured bottleneck |

## Major architectural replacements

| # | Candidate | Potential gain | Cost | Risk / condition |
|---:|---|---|---|---|
| 51 | Create a PS2-native offline asset pipeline | Very high | Very high | Foundation for native textures, ADPCM, packs, alignment and precomputed metadata |
| 52 | AOT-compile static Fast3D display lists into PS2 command templates | Very high | Very high | Dynamic state boundaries and invalidation must be explicit |
| 53 | Package stages as room-local streaming bundles | Very high | Very high | Changes asset build, loading and lifetime ownership |
| 54 | Introduce a real PS2 frame graph for render targets and passes | High | Very high | Best route to reliable effects, synchronization and VRAM aliasing |
| 55 | Replace portable platform layers with a thin PS2 runtime | High | Very high | Long-term option after gameplay and renderer correctness stabilize |

## Recommended execution order

1. Finish renderer correctness: texture addressing, tile sizes, TLUT/CI,
   clamp/mirror/mask/shift, alpha materials, depth and framebuffer effects.
2. Add a nonblocking in-memory hardware profiler for full frame phases.
3. Establish stable Og/O2 hardware baselines using identical scenes.
4. Remove duplicated EE transform work and reduce GIF/VIF submission count.
5. Implement native offline textures plus deterministic VRAM residency.
6. Cache translated display lists and compiled combiner/pass recipes.
7. Move complete geometry batches to VU1 with correct clipping.
8. Build asynchronous IOP streaming and large aligned asset packs.
9. Move reusable audio to PS-ADPCM/SPU2 and batch control traffic.
10. Restructure measured gameplay hot data into buckets and SoA/AoSoA.
11. Only then hand-write MMI/COP1/VU0 kernels for the remaining measured
    scalar bottlenecks.

## Immediate renderer investigation

Current hardware evidence shows that models and some textured objects render
correctly while much of the environment does not. This argues against one
global transform or VRAM failure. The next correctness pass should compare
working and broken draws across:

1. source N64 texture format and size;
2. selected tile, line, TMEM address and palette;
3. `SetTileSize` bounds versus uploaded dimensions;
4. mask, shift, clamp and mirror on both axes;
5. CI/TLUT mode and palette bank;
6. NPOT expansion and physical GS pitch;
7. mip level and LOD selection;
8. combiner and alpha/depth pass classification;
9. framebuffer-source textures and render-target copies.

A compact in-memory draw-signature counter should group these states without
touching `mass:`. The first diagnostic build should change one disputed
contract at a time and use the same stage/camera path for comparison.

## Validation gates

Every accepted optimization must satisfy all applicable gates:

- host unit/byte-equivalence tests;
- R5900 cross-build and ELF inspection;
- no forbidden C++ runtime reintroduction;
- deterministic capacity/overflow behavior;
- retail PS2 visual comparison;
- frame and phase timing comparison;
- audio, input and loading smoke test;
- documented rollback point.

The target is not merely a higher frame counter. It is a renderer and runtime
whose speed follows from doing less work, moving work to the correct PS2 unit
and feeding the hardware data in the form it actually wants.
