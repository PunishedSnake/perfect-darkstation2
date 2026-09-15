# PS2 startup and first-frame chain

This document describes the `ps2` branch as audited on 2026-09-03. It follows
the retail-console path from the PS2SDK entry point to the first normal 3D
title frame. It is intended both as a maintenance map and as a checklist for
hardware logs.

Hardware testing on 2026-09-05 reached the Rare, Nintendo 64 and Perfect Dark
logos after LEGAL. Controller detection and EEPROM creation have also been
confirmed. Title rendering remains incorrect: fragmented logos and roughly one
frame per four seconds were reported. Reaching these states does not establish
renderer correctness or playable performance.

The next build bounds scratch channel copies to each tile's used rectangle. An
experimental direct draw for constant alpha-trilerp endpoints was removed after
the first hardware build containing it no longer showed the post-LEGAL logos.
All factors therefore retain the known tiled graph until a controlled A/B build
can validate a cheaper path independently. The cause of the fragmented logos is
not yet proven.

Title tracing now flushes buffered text without closing and reopening `mass:`
inside `lvTick` or model rendering. The first-frame completion checkpoint remains
outside `mainTick`, after presentation. This follows the logger's own rule that
durable USB filesystem checkpoints do not belong in frame-critical code and
prevents tracing from becoming part of the renderer bottleneck.

Diagnostic lines `PS2 frame video` measure full `gfx_run` and `gfx_end_frame`
durations. `PS2 frame runtime` measures scheduler-start plus mainTick, scheduler-end,
and their total, excluding the outer frame gate. Rendering happens within mainTick,
so these timings overlap and must not be added together. Each reports one sampled
frame at roughly five-second intervals, not an average. Trilerp counters are
cumulative and identify tiled triangles and submitted scratch tiles. Existing `ee_us`
only measures vertex translation and cannot explain total frame time.

## Execution overview

```mermaid
flowchart TD
    A["PS2SDK CRT and constructors"] --> B["Platform and file services"]
    B --> C["ROM stream and permanent data"]
    C --> D["GS, input and audio backends"]
    D --> E["Perfect Dark runtime init"]
    E --> F["Title state machine"]
    F --> G["Fast3D command stream"]
    G --> H["VU1 or PATH3 to GS"]
```

The port deliberately initializes PAD and SPU2 before graphics DMA ownership,
then finishes ROM streaming before entering the game. This ordering has already
survived on retail hardware and should not be changed casually.

## Phase 1: process and platform bootstrap

| Order | Owner | Work | Failure policy |
| --- | --- | --- | --- |
| 1 | PS2SDK CRT/linker | Enters the ELF at `_start`, establishes the C runtime and invokes constructors. | Toolchain/ABI failure, normally no application log. |
| 2 | `port/src/main.c` | Parses arguments and starts the crash/logger layer. | Continue only when logging is optional. |
| 3 | `port/ps2/system_ps2.c` | Derives the launch-device base path, initializes PS2 platform services and opens `pdps2.log`. | Fatal hold for required services. |
| 4 | `port/src/fs.c` | Selects base/save directories and validates filesystem access. | Fatal hold. |
| 5 | `port/src/config.c` | Registers defaults, loads `pd.ini`, or creates a default file when none exists. | Invalid individual values fall back to registered defaults; required I/O errors are logged. |
| 6 | `port/ps2/input_ps2.c` | Initializes SIF/RPC, PADMAN and DualShock 2 state. | Warning and continued boot so controller failure is diagnosable. |
| 7 | `port/ps2/audio_ps2.c` | Loads embedded `audsrv.irx` and initializes SPU2 output. | Warning; the game audio heap is disabled for this run. |

Device-qualified paths such as `mass:`, `host:`, `mc0:` and `pfs0:` are
absolute. Relative runtime files are rooted beside the ELF unless `--basedir`
or `--savedir` overrides that behavior.

## Phase 2: ROM materialization

`port/src/romdata.c` opens the legally supplied NTSC-final ROM and validates its
header. The PS2 path keeps the ROM file-backed instead of copying the complete
32 MiB image into EE RAM.

The materialization sequence is:

1. validate the ROM type and expected file size;
2. inflate the compressed permanent data segment with bounded input and output;
3. expose named permanent segments used by the portable game;
4. initialize the ROM file table and file-name table;
5. retain file offsets so later asset reads can stream only the requested
   range;
6. validate every PS2 runtime patch offset before applying it.

Short reads, malformed RZIP headers, decompression that does not reach
`Z_STREAM_END`, output overflow, and invalid patch offsets now fail explicitly.
They are no longer allowed to masquerade as a successful asset load.

## Phase 3: video and game heap

`port/ps2/video_ps2.c::videoInit` initializes the native GS core and the patched
Fast3D frontend. `port/src/main.c` then calls `gameInit`, creates the portable
scheduler facade and allocates the single game heap.

The reported game-heap size becomes both `g_OsMemSize` and `osMemSize`, keeping
the original game heuristics consistent with memory that is actually owned by
the process. Failure to obtain the heap is fatal before the game can write
through a null pointer.

The selected stage defaults to `STAGE_TITLE` (`0x5a`). `--skip-intro` redirects
it to CI Training and `--boot-stage` can select another numeric stage for
bring-up.

## Phase 4: portable runtime initialization

`port/src/pdmain.c::mainProc` emits durable checkpoints around the three major
steps:

| Checkpoint | Important work behind it |
| --- | --- |
| `mainInit begin` | Entry into the reconstructed Perfect Dark runtime. |
| `mainInit complete; rdpInit begin` | Fault/DMA/audio-manager shims, variables, memory pools, controller facade, VI state, file table, permanent game systems and `titleInit`. |
| `rdpInit complete; sndInit begin` | RDP output buffers and scheduler-facing graphics state. |
| `sndInit complete; mainLoop begin` | Sound tables and buffers, or the deliberately disabled sound path. |

Critical permanent allocations in RDP, sound, MEMA, VI and per-frame Gfx/Vtx
setup are checked. An allocation failure now names the subsystem and enters the
fatal hold instead of continuing into address zero.

The two-bank `memp` allocator probes the onboard bank before the expansion
bank. Exhausting only the first bank is normal fallback, so it is no longer
reported as an error. A PS2 `mempAlloc` error now means that neither bank could
satisfy the request and reports the remaining capacity in both banks.

## Phase 5: title sequence

The title state machine is owned by `src/game/title.c`. The relevant early path
is:

```text
legal / product-identification screen with Expansion Pak status
  -> controller check
  -> Rare logo model load
  -> Rare logo model instantiation
  -> first Rare logo modelRender
  -> Nintendo logo / PD logo / title menu
```

The controller warning is conditional. Its absence on hardware proves that the
portable controller facade sees PAD port 0; it does not by itself prove that
every game binding and rumble path works.

The Rare-logo boundary has dedicated durable messages:

```text
title: Rare logo load begin ...
title: Rare logo model loaded ...
title: Rare logo model instantiated ...
title: Rare logo init complete
title: Rare logo first render begin ...
title: Rare logo relations ready ...
title: Rare logo first model pass complete
title: Rare logo first render complete
```

The model path is now fail-fast:

1. `modeldefLoad` asks `fileLoad` for the ROM-backed model;
2. `fileLoad` validates the table entry, destination capacity and file read;
3. compressed input remains in its separate `romdata` buffer;
4. `rzipInflateSized` writes into the full bounded destination and must reach
   the end of the stream;
5. preprocessing checks its temporary allocation;
6. model definitions used by the title sequence must have a root node and at
   least one matrix;
7. title-arena, model-slot and model RW-data allocations are checked before
   pointer walking or instance initialization;
8. only a successful model definition and instance reach relation updates and
   rendering.

This removes the previous unsigned tail-placement underflow and the case where
a failed file read still returned a destination pointer.

## Phase 6: one rendered frame

Every game frame follows this ownership chain:

| Step | Function or component | Result |
| --- | --- | --- |
| Frame begin | `schedStartFrame` -> `videoStartFrame` | Opens the Fast3D/GS frame. |
| Game production | `mainTick` -> `lvTick` / `lvRender` | Builds N64-style `Gfx` commands and transient matrices/vertices. |
| Task submission | `rdpCreateTask` -> `port/src/pdsched.c::schedSubmitTask` | Routes a graphics task directly to the portable video bridge. |
| Frontend | `videoSubmitCommands` -> patched `gfx_run` | Interprets GBI commands and live TMEM operations. |
| Planning | `port/ps2/gfx_ps2.cpp` | Maps state and combiner recipes to one or more GS passes. |
| Native transport | `gs_core`, `gs_native_queue`, `gs_vu1_*` | Uses VIF1/VU1 PATH1 where supported and PATH3 fallback otherwise. |
| Presentation | `gfx_end_frame` -> `ps2GsCorePresent` | Waits for the appropriate synchronization and flips at VBlank. |
| Frame end | `schedEndFrame` | Polls PAD/audio, applies VI blanking to the active frame and advances the delayed-unblank state. |

The portable scheduler does not launch a separate RSP. It preserves the game
task boundary but executes the graphics task synchronously through the PS2
backend.

`osViBlack` is implemented as persistent output state on PS2. While blanking is
active, `videoEndFrame` appends a black colour/depth clear to the already-open
frame before its single submit and presentation. It must not call
`videoClearScreen` from `viHandleRetrace`: that would open a nested Fast3D/GS
frame, reset the native command arena after game submission and perform an
extra VBlank presentation from inside the scheduler.

`viBlack(false)` is intentionally delayed rather than immediate. It initializes
a countdown equal to the emulated framebuffer count so stale buffers cannot
flash on screen. The portable scheduler already decrements that value in
`__scUpdateViMode`, once per presented frame after `viHandleRetrace` applies
the current state. Do not also decrement it in `schedEndFrame`: this would
halve the intended delay. A host regression test covers the countdown and
the permanent-black sentinel. The missing countdown hypothesis for the
2026-09-05 hardware log was rejected on inspection of the full scheduler.

### LEGAL-screen diagnostic build

The `pdps2(20260905-094506).log` capture ends at `stage reset complete; frame
loop begin`. It does not establish which call stopped progress afterwards,
or prove that VI blanking caused the black screen. The added first-frame
checkpoints distinguish the frame gate, native frame begin, timing, `lvTick`,
`lvRender`, synchronous graphics-task execution and presentation. They run
only for the first frame of each stage. A one-shot warning reports a frame
gate that still has not opened after five seconds, provided the system clock
continues advancing. This is observation only, not an automatic reset,
stage skip or timeout recovery.

Title-mode application is logged separately, including controller detection
and Rare-logo entry. Existing file-backed logging can itself block or fail;
the last durable line is a boundary for investigation, not proof that the
next source line crashed. Retest this build on hardware before attributing
the original failure to any single subsystem.

### Correct-logo performance baseline

The `aaad5659` hardware capture on 2026-09-06 confirms that the Rare, Nintendo
64 and Perfect Dark logos render correctly. The first frame reports 527
translated batches and 3162 vertices in 17.154 ms of EE translation, while
the complete frame takes 217.705 ms. A later logo frame spends 650.209 ms in
`schedEndFrame`, which contains the final GS completion fence and VBlank wait.
This rules out vertex translation as the dominant title bottleneck and points
at queued GS pass work.

The exact alpha-bearing trilerp graph is especially expensive: each 128x64
tile uses two CT32 scratch targets, several texture/composite passes and up to
512 independent 8x2 channel-shuffle sprites. Constant LOD endpoints are
theoretically reducible to one source texture, but the default correctness
baseline deliberately retains the tiled graph for every factor after the
endpoint A/B build failed the post-LEGAL hardware test.

The `d1556ac4` Og/O2 hardware run exposed a regression in the later broad
complex-material fallbacks: LEGAL text looked displaced and neither run
reached the Rare, Nintendo 64 or Perfect Dark logos. Both logs stop after
`stage reset complete; frame loop begin`, so they contain no completed-frame
telemetry and do not distinguish a long first GS fence from a failed file-sink
reopen. The matching visual regression nevertheless invalidates the unproved
RGB substitutions.

The `a56bacda` test build retained exact tiled graphs for non-endpoint
trilerp, while allowing endpoint collapse and a direct independent-alpha draw
only for textures whose RGB lanes were proven white during upload. It also
recorded endpoint, tile and GS FINISH counters outside the measured frame
interval. Those remaining specializations still failed the hardware title
test described below, so they are no longer active in the default build.

The `a56bacda` Og hardware run on 2026-09-15 still displayed LEGAL but did not
display the Rare, Nintendo 64 or Perfect Dark logos. Its durable log again
ends at `stage reset complete; frame loop begin`, without a completed-frame
snapshot. This rejects the proof-gated partial rollback as sufficient.

The active renderer has therefore been returned to the `aaad5659`
hardware-confirmed behavior. The endpoint trilerp collapse, white-texture
direct-alpha path, GS FINISH timing wrapper and their extra hot-path counters
are removed together. The C-safe integer checkpoint ABI and build-profile
labelling remain because they do not alter draw or synchronization behavior.
This is a correctness baseline, not a performance improvement.

The subsequent `46bc51e7` Og hardware run on 2026-09-15 still displayed LEGAL
but did not display the Rare, Nintendo 64 or Perfect Dark logos. Its durable
log again ends at `stage reset complete; frame loop begin`. The original
`aaad5659` CI #245 game artifact remains the binary control because the current
and historical ELFs differ only in the build-profile log/ABI changes and their
resulting eight-byte text layout shift. Test that exact historical ELF from a
clean directory before assigning the failure to renderer source.

`pd-ps2-game-nolog` is the orthogonal filesystem control. It is the same `Og`
source as the default game but does not open `pdps2.log`; console output remains
enabled. If the historical ELF also fails while the no-log build progresses,
the `mass:` file sink or its device state, rather than title rendering, is the
active blocker.

## Fatal and hang interpretation

On PS2, `sysFatalError` writes the final error, forces a log checkpoint, closes
the file and parks the EE in `DelayThread`. The last image should remain and the
console should require a manual reset.

Use this distinction during bring-up:

| Observed result | Likely class |
| --- | --- |
| Last log says `FATAL:` and console remains running | Controlled validation, I/O or allocation failure. |
| Log stops between two documented checkpoints and console remains running | Infinite wait, deadlock or uninstrumented fatal path. |
| Console returns to OSD or resets without `FATAL:` | EE exception, memory corruption, DMA/VIF fault, explicit process return, or external reset. |
| Frame loop lives and Triangle/Select snapshots appear | Runtime and presentation survive; investigate renderer state/fidelity. |

## Remaining high-risk boundaries

1. The restored title sequence matches the last renderer state confirmed on
   retail hardware, but requires a new hardware test. That known-good state was
   too slow to reach the menu in practical time. Non-endpoint alpha-trilerp
   tiles and their channel shuffle remain the leading measured target.
2. Central Vtx/Mtx/colour allocations now fail before crossing their active
   frame arena, and the PS2 master display list is checked at frame phase
   boundaries. Direct display-list writers still need per-writer reservations
   or a protected trailing region to prevent damage before a post-phase check.
3. Unsupported combiner recipes are counted and dropped, so a healthy frame
   loop can still produce an empty image. The first dropped draw schedules an
   end-of-frame durable checkpoint containing its shader ID; periodic and
   controller snapshots report dropped batch/triangle totals as
   `unsupported=B/T`.
4. Offscreen framebuffer effects, framebuffer copies and mipmaps are not yet
   implemented.
5. Some preprocessors estimate output space and validate after conversion;
   they need writer-side bounds for hostile or corrupt input.
6. `bootAllocateStack` is a shared static compatibility stub. It is safe only
   while the portable scheduler remains single-threaded on this path.
7. Text, alpha/blend fidelity and synchronization hazards require retail GS/VU
   validation even when host tests pass.

## Hardware log checklist

For the next title test, preserve the complete `pdps2.log`. The first missing
line in the Rare-logo sequence identifies whether the remaining failure is in
file materialization, model preprocessing/instantiation, relation building,
the first model display list, or submission to GS. Also record whether the
machine held, reset or returned to OSD.
