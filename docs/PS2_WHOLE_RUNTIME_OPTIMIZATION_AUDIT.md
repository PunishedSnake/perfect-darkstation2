# PS2 whole-runtime optimization audit

Date: 2026-09-17

This pass covers the game outside the GS renderer: EE gameplay code, math,
memory, ROM/USB streaming, EE/IOP communication, SPU2 audio and executable
layout. The rule is the same as for graphics: preserve the original numerical
and ownership contracts, remove duplicated work first, then specialize only a
measured hot path.

## Changes implemented by this pass

### One fewer synchronous audio status RPC per block

The portable audio manager queries the number of queued bytes before mixing.
`audioEndFrame` then queried both queued and available bytes again. In PS2SDK
`audsrv`, these values are complementary views of one fixed-size ring.

Initialization now records the ring capacity from one queued/available pair
before playback starts. Submission reads only the current available byte count
and derives the queued count. If an unexpected server reports an impossible
value, the code falls back to the old two-call observation. Audio format,
latency limit, waiting and ownership are unchanged.

Result: the steady path removes one blocking EE/IOP RPC per submitted audio
block.

### Four-line ROM read-ahead cache

The PS2 keeps the 32 MiB ROM file-backed. Sample DMA and compressed texture
loads previously issued `fseek` plus `fread` for every small range. The source
now owns four 4 KiB cache lines, refilled on 512-byte boundaries.

Four lines are intentional. One 16 KiB line performs well for a single
sequential inflater but can amplify traffic when several audio voices alternate
between distant sample regions. Four independent lines retain several active
localities while matching the useful 4 KiB storage-transfer granularity.
Requests larger than one line still read directly into their final destination
and do not pay an extra copy.

### Cached streamed-segment resolution

Only `sfxtbl`, `seqtbl` and `texturesdata` remain file-backed virtual segments.
Audio and texture DMA arrive in bursts from one segment, but every request used
to scan the complete ROM segment table. `romdataDmaRead` now tests the last
successful segment first and preserves the complete checked fallback scan.

### Optional pool failure cleanup

`utilsInit` requests two legacy buffers from disabled pool 8. Failure is an
accepted original-game path, but the portable code logged it as an error and
then performed pointer arithmetic on `NULL`. Optional pools 7 and 8 now fail
quietly, and the derived pointers remain `NULL`. No active consumer of these
legacy buffers exists in the current port.

## Ranked remaining work

| Priority | Subsystem | Current evidence | Safe next experiment |
|---|---|---|---|
| P0 | Whole-frame profiling | `profile.c` is a stub; current logs only split tick, renderer and present | In-memory phase ring for logic, visibility, animation, audio mix, ROM waits and render translation; export only on explicit request |
| P1 | Audio transport | EE mixes PCM, status and payload use blocking RPC, samples originate on USB | Batch control, keep reusable SFX as PS-ADPCM in SPU2 RAM, move long-stream refill to an IOP worker |
| P1 | Asset streaming | ROM cache removes small-call amplification, but file loads still allocate compressed input and final output in several paths | Count bytes/read calls/copies per stage; stream-decompress directly into stage ownership where preprocess contracts permit |
| P1 | Gameplay locality | `propobj.c` and `chraction.c` are very large mixed-control modules; R5900 has 8 KiB D-cache and 16 KiB I-cache | Hardware phase profile first, then split hot/cold fields or bucket repeated prop/character work by state |
| P2 | Math | Hardware `sqrt.s` is restored; many adjacent `sinf`/`cosf` pairs remain | Explicit `pd_sincosf` at matrix/camera call sites with bit/error tests against the current game approximation |
| P2 | Normalization | Many vector paths compute `1 / sqrt(length2)` | Dedicated finite, nonzero helper using COP1/VU0 reciprocal square root; never replace collision lengths or zero-vector behavior globally |
| P2 | Allocators | The game already uses permanent/stage arenas plus a small free-list heap | Add high-water and longest-free telemetry; replay a real allocation trace before changing `mema` or Newlib malloc |
| P2 | Code layout | Function/data sections and linker GC are active; broad `-O2` is still a hardware A/B profile | Use measured hot functions for selective `-O2`; try `-Os` for large cold UI/parser modules; compare I-cache counters |
| P3 | MMI/VU0 kernels | Texture packing and PCM mixing are regular integer workloads | Byte-exact MMI kernels only after volume counters identify a dominant scalar loop |
| P3 | Scratchpad/DMAC | No non-render kernel yet proves enough regular work to amortize staging | Double-buffer one measured large transform/decode batch; do not use scratchpad as a general heap |

## Decisions from the audit

- Do not replace the existing lifetime arenas with a general allocator. Their
  reset semantics are already the right PS2 design.
- Do not enable global `-ffast-math`, `-O3`, reciprocal transforms or LTO. They
  can change collision, clipping and alpha edge decisions and can enlarge the
  I-cache working set.
- Do not move arbitrary game logic to VU0. VU0 is useful for proven regular
  vector batches, not branch-heavy AI or object state machines.
- Do not stream PCM from the EE one voice at a time as the final audio design.
  The long-term target is data near SPU2 with an IOP-side producer and batched
  control from the EE.
- Do not increase buffering blindly. USB and audio queues need enough headroom
  for measured tail latency, not maximum latency disguised as stability.

## Hardware validation

For the next real-console comparison, use the same ROM and scene and record:

1. time from stage selection to first interactive frame;
2. whether audio clicks, drops or gains latency;
3. menu and gameplay frame time with sound enabled;
4. the same scene with sound disabled;
5. texture pop-in during the first traversal of a room.

The cache and RPC changes are contract-preserving, but only the retail console
can establish their actual benefit under USB, SIF and IOP contention.
