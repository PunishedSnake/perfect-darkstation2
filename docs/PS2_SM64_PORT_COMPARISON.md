# PS2 renderer comparison: Perfect DarkStation 2 and SM64

Status: source review captured on 2026-09-15.

## Reference

The comparison uses the canonical `ps2` branch of
[`fgsfdsfgs/sm64-port`](https://github.com/fgsfdsfgs/sm64-port/tree/ps2) at
commit [`5b693af`](https://github.com/fgsfdsfgs/sm64-port/commit/5b693af3d1de703697edc91439c8393a54a66763).
The later multiplayer fork
[`Carl-Llewellyn/ps2_sm64_three_player_usbnetx`](https://github.com/Carl-Llewellyn/ps2_sm64_three_player_usbnetx)
has the same PS2 graphics backend; its later changes concern controller and USB
network play rather than renderer architecture.

## What SM64 does

The SM64 port connects the shared Fast3D frontend to a compact gsKit backend in
`src/pc/gfx/gfx_ps2_rapi.c`:

- shader IDs are classified into ten fixed draw functions;
- common combiners become one direct GS pass;
- the two-texture mode becomes two ordered passes;
- RGBA5551 and RGBA8888 upload as CT16 and CT32 respectively;
- a 2 MiB, 128-byte-aligned linear EE texture staging cache backs gsKit's
  texture manager;
- the PS2 build uses `-O3 -fno-tree-builtin-call-dce
  -fno-strict-aliasing`;
- presentation waits for two VBlanks because the game is scheduled at 30 FPS;
- the latest canonical commit reduces the set of IOP modules loaded at startup.

This is effective for SM64's small, known material set. It is not a general
RDP implementation. The source itself labels mirror wrapping and decal depth
as fixes or approximations.

## Direct comparison

| Area | SM64 PS2 | Perfect DarkStation 2 | Decision |
| --- | --- | --- | --- |
| Combiner policy | Small fixed dispatch, including magic shader IDs | Semantic recipe planner plus exact and fast graphs | Keep semantic planner; adopt a bounded one-pass full-game policy for measured expensive recipes |
| Geometry submission | CPU viewport conversion and one gsKit primitive packet per triangle | Batches of up to 81 textured vertices through VIF1/VU1 PATH1, with PATH3 fallback | Keep the Perfect Dark path |
| State traffic | Writes TEST/CLAMP/TEX state around most draw calls | Project-owned 64-bit GS register shadow suppresses unchanged writes | Keep the Perfect Dark path |
| Texture source | Linear EE staging cache | Authoritative TMEM view with native CT16/CT32/T4/T8 formats | Keep the Perfect Dark path |
| VRAM lifetime | gsKit manager; complete VRAM clear on Fast3D flush | Transactional residency and fence-delayed block retirement | Keep the Perfect Dark path |
| Complex materials | One or two approximate passes | Exact tiled graphs can require many passes and channel shuffles | Preserve exact graphs in the diagnostic ELF; use measured approximations in the full game |
| Build optimization | Unconditionally `-O3` for PS2 | `-Og` correctness baseline plus separately published `-O2` A/B | Compare both on identical hardware workload; do not adopt SM64's `-O3` blindly |
| IOP footprint | Loads only required modules | Own startup and embedded `audsrv.irx` | Compare active IRX and IOP memory after graphics reaches its frame target |

## Reusable lessons

1. Classify the real material vocabulary and dispatch once per batch. Perfect
   Dark already does this semantically; the next gains come from removing
   costly graphs for recipes that tolerate a documented full-game fallback.
2. Keep native source formats native. Both ports prove CT16 residency for
   RGBA5551; Perfect Dark extends this to live TMEM, CI and IA/I formats.
3. Maintain separate correctness and playability policies. Perfect Dark's
   alpha-trilerp diagnostic should remain exact, while the normal game build
   may select one-pass approximations that retain depth, alpha-test, fog and
   visibility semantics.
4. Treat compiler optimization as a measured A/B. CI emits separate `Og` and
   `O2` game ELFs and embeds the profile in the runtime log. SM64 demonstrates
   that an optimized decompilation can run on PS2, but it does not prove that
   `-O3` is safe for Perfect Dark or that EE compute is the current bottleneck.
5. Revisit IOP module footprint after the render critical path is usable. It
   can recover service memory, but it does not explain the measured GS FINISH
   time in title frames.

## Patterns not to copy

- wrapping the linear texture cache after overflow without proving consumer
  lifetime;
- clearing all GS VRAM as the normal cache-flush policy;
- emitting one gsKit packet per triangle;
- mutating the frontend VBO in place during viewport conversion;
- globally requiring 128-byte alignment for unrelated allocations;
- hard-coding a two-VBlank wait into a game with a different scheduler;
- copying SM64's `-O3` flags without a Perfect Dark correctness and timing A/B.

## Current measured implication

The `aaad5659` real-hardware log attributes only 17.154 ms of the first title
frame to EE translation, while the whole frame is 217.705 ms and a later
`schedEndFrame` reaches 650.209 ms. Therefore the authoritative next step is
still removal of avoidable GS pass work, not a compiler-flag or VU rewrite.
The ordinary alpha-threshold variant of the independent TEXEL0-alpha material
can use the existing one-draw fallback: the GS tests the final
`TEXEL0.a * INPUT1.a` result directly, so the scratch target adds no alpha
correctness.
