# Emotion3D

Emotion3D is the PS2 port's replacement for Fast3D, built for this one game
on this one machine. Fast3D is a general N64 display-list emulator: every
frame it re-decodes every display list, re-runs the N64's vertex pipeline,
re-emulates TMEM and re-derives GS state from RDP state. Perfect Dark's data
is far more static than that, and the PS2 has VU0, VU1 and the GS to do the
work directly.

Emotion3D replaces Fast3D piece by piece, starting with the most static
data. Whatever it does not handle yet still goes through Fast3D, so every
stage can be shipped, measured and switched off on its own
(`Video.Ps2Emotion3D=0` in `pd.ini`).

## Stage 1: room geometry (round 18)

Level geometry is drawn as rooms:
* A room is a tree of blocks. Each leaf block is a display list over the
  room's vertex buffer (segment 14) and colour table (segment 13).
* While a room is loaded, its display lists and vertex positions do not
  change. Only the colours change, through room lighting, shot-out lights
  and so on.

### Hook

`bgRenderRoomPass` sets the two segments as before. Instead of `G_DL`, it
then emits `G_E3D_ROOMLEAF_EXT` (0x46): the room number in w0, the leaf's
display list in w1. Exceptions:
* Rooms with animated vertex texture coordinates (`ROOMFLAG_HASDYNTEX`) keep
  `G_DL`.
* `bgLoadRoom` and `bgUnloadRoom` bump the room's generation, which
  invalidates its compiled leaves. Those are the only places a room's lists
  are written: the texture command rewrite and the GBI find/replace pass
  both happen at load.

### Compile (first use of a leaf)

`port/fast3d/e3d_geom_ps2.inc` walks the list once and splits it into ops:
* **STATE**: runs of anything except geometry commands (texture loads,
  combiner, render modes). They are copied into a compact list that Fast3D
  still interprets, so textures and render state behave exactly as before.
* **GEOM**: runs of `G_COL` / `G_VTX` / `G_TRI1` / `G_TRI4`. These are split
  into **chunks**, the triangles between two vertex loads. Each chunk keeps:
  * its bounding box;
  * the vertex loads it depends on;
  * per triangle corner, the vertex index, the colour index and the
    original vertex slot.

  A vertex's colour comes from the colour table that was current when its
  `G_VTX` ran, so the colour index is resolved per corner at compile time.

Leaves that do not fit are left as plain display lists: nested lists,
unexpected segments, or more than 2048 commands or triangles. Programs live
in a 256 KiB arena that is cleared when full (Defection peaks around 240 KiB; a reset only recompiles the leaves in view).

### Replay

Each chunk's box is classified against the object context's planes on VU0
(the round 14 classifier), before any triangle setup:
* **Outside one exact plane**: skipped. Fast3D would reject every triangle,
  and no texture import is triggered.
* **Inside the guard band, with a state the object path can draw**:
  triangles are culled in object space and packed straight into the VU1
  object batch. There is no `G_VTX` decoding, no per-vertex deferral
  bookkeeping, and no per-triangle slot checks.
* **Straddling, or a state the object path cannot draw**: the chunk's vertex
  loads and triangles go to `gfx_sp_vertex` / `gfx_sp_tri1` directly. That
  is what the display list did, minus the decoding.

The triangle setup is built only at the first visible chunk of a GEOM op.
An earlier version built it for every GEOM op, including off-screen ones.
That triggered texture imports and TMEM fingerprinting Fast3D would never
have done, and made the cutscene slower overall.

### Results (PCSX2, Defection: opening cutscene, then rooftop gameplay)

Counters per frame (`PD_PS2_OBJECT_STATS`):
* Cutscene: 2-9 room leaves. 50-75 % of room triangles take the fast path,
  and 60-300 chunks are skipped as off screen.
* Rooftop: about 1,200-1,300 fast-path triangles against 400-450 through
  Fast3D's routines.

Same build, `Video.Ps2Emotion3D` 0 → 1:
* Cutscene windows (20-70 s): frame time 19.71 → 19.27 ms on average,
  with the best windows 0.9-1.5 ms faster.
* Rooftop gameplay: GFX time is unchanged (12.1 ms). There, room geometry
  already went through the round 14 object path, so stage 1 only removes
  the decoding.

Screenshots show no missing or miscoloured geometry. On/off frames differ
only along edges. The same edge pattern appears on the weapon and the star
streaks, which this stage does not touch, so the cause is a camera phase
offset from different frame pacing, not rendering.

## Stage 2: lazy palettes (round 19)

PD loads a different palette before nearly every colour-indexed draw. The
TMEM model wrote each one in full, then hashed the region whenever a
texture was identified:
* four copies per entry, plus validity bytes, generations and a hash mix;
* `gfxRdpTmemWriteTlutBytes` alone was about 3.3 % of EE time, and 88 % of
  its samples were in the write loop itself. These are real changes of
  palette, not redundant reloads.

A draw whose texture is already on the GS never needs those bytes, only
the palette's identity. So, in `rdp_tmem_live.cpp`:
* **Record, don't write.** A well-formed TLUT load is recorded (source,
  TMEM range) instead of written. A newer palette that covers it, or a
  block load that covers it, simply replaces it.
* **Identity from the source.** A colour-indexed fingerprint whose palette
  range lies inside the pending load takes the palette hash from the
  source entries. The value is bit-identical to hashing the written TMEM
  words, so texture cache keys do not change.
* **Memo with a content copy.** Source hashes are memoized with a copy of
  the entries and compared on every use. Palettes the game rewrites in
  place are never stale, and no write epoch is trusted.
* **Write only when needed.** The palette is written into TMEM only when
  something reads or overlaps those words: texel materialization, a texture
  whose rows reach the palette half, a load that may invalidate TMEM, or
  the end of the frame.

The randomized eager-vs-lazy host test (`rdp_tmem_live_test.cpp`, in CI)
must give identical fingerprints, texels and final TMEM. It now also
asserts that the lazy-palette path runs. It caught one ordering bug during
development: a malformed TLUT load invalidates all of TMEM, so a pending
palette must be settled before it.

In Defection, 85-90 % of palette loads are never written, and most
colour-indexed fingerprints come from the source.

Also in this round:
* An object context that is recycled while it owns no deferred vertex slots
  now skips the 128-slot replay scan. In gameplay that is 52 of 78 recycles
  per frame.
* `Video.Ps2ObjectDefer=0` turns off round 14's per-`G_VTX` object
  deferral outside room leaves, for hardware A/B checks. It stays on by
  default (see below).

Against round 18 in the cutscene, the heavy windows are 0.6-1.2 ms faster,
about 4 %. Rooftop gameplay stays at the 60 fps cap.

## What the data says about the next stages

**Gameplay is GS/VU1-bound in PCSX2, not EE-bound.** On the Defection
rooftop:
* the EE spends 23 % of its time waiting for VIF1/GS to drain the previous
  packet chain (`ps2GsNativeQueueSubmitInternal`), plus 4 % idle;
* every frame there makes the 16.7 ms vsync deadline.

PCSX2 does not model GS fill rate faithfully, so where that wait really
comes from has to be measured on hardware before a GS-side stage is
designed.

**PD's models are skinned at vertex-load time.** A character's display list
loads one vertex block under the thigh matrix and the next under the shin
matrix, then draws triangles that join them. Counters on the rooftop, per
frame:
* 42-60 triangles join blocks the frustum test classified differently;
* 6-7 join blocks loaded under different matrices;
* about 25 deferred blocks are replayed, against 1-35 triangles that
  actually drew through the object path.

Turning per-block deferral off for everything except rooms is a wash: the
early cutscene is 0.5-1.4 ms slower, the 60-65 s windows about 1 ms faster.
Rigid props gain from it, skinned characters lose.

## Stage 3: model display lists (round 20)

Rooms and models now share one compiler, `port/fast3d/e3d_geom_ps2.inc`
(was `e3d_room_ps2.inc`).

### Hook

`modelRenderNodeDl` emits `G_E3D_MODELDL_EXT` (0x47, w1 = the node's list)
in place of `gSPDisplayList`, for the opaque list and both translucent
ones. Without the direct PS2 renderer, or with the switch off, the command
runs the list as a plain `G_DL`.

### What differs from rooms

* **Keys.** A model list is shared by every instance of a model
  definition. Vertices (segment 4), colours (segments 5 and 6) and
  matrices (segment 3) are per instance, so they are read live. Only the
  list's structure is compiled.
* **Run-time lists.** Some lists are built at run time (monitor screens,
  gun lists from a pointer). Every use checks a 32-bit checksum of the
  list's words, at most once per frame per list. A list is compiled only
  the second time it is seen unchanged, so lists in per-frame memory never
  fill the arena. A list whose content changes three times is marked
  volatile and always runs plain.
* **Matrices.** `G_MTX` and `G_POPMTX` are items in the geometry stream.
  Each starts a new matrix epoch, and each load is tagged with its epoch.
* **Skinning.** A triangle that reads a load from an earlier epoch is a
  skinned join. Both epochs are then linear: their loads run in list order
  under their own matrix, and their triangles go to `gfx_sp_tri1`. A list
  with any linear epoch is marked `skinned`.
* **Bounds.** Chunk bounds are computed per instance, as the union of the
  bounds of the chunk's loads. Each load's bounds are computed once per
  replay, in 16-bit integers.
* **Resident loads.** A load that several chunks read is transformed once
  per geometry run, not once per chunk. This also helps rooms on the slow
  path.

Skinned lists, lit lists, and lists whose last replay drew nothing through
the batches run as plain lists with per-`G_VTX` deferral. The last group
retries after 16 frames.

### Results (PCSX2, Defection)

The counters show that the compiler handles the whole game's model set:
* about 30-100 model lists per frame;
* no fallbacks beyond one or two run-time lists;
* skinned joins found where round 14 replayed blocks (up to 550 linear
  triangles per frame in the cutscene, and no deferred-block replays inside
  Emotion3D).

It is **not faster**. Against the same build with only the model half off,
the cutscene's gfx time is 0.3-0.7 ms per frame higher with models on.
That holds even after:
* skinned and lit lists were sent to the plain path;
* the checksum went to once per frame;
* bounds went per load;
* failed replays got a back-off.

The hook alone, with every list sent plain, measures the same as off, so
the cost is in the replay itself. Rigid models are small, about three
triangles per chunk. At that size, round 14's per-block deferral already
does the same classification with less bookkeeping.

So **`Video.Ps2Emotion3DModels` defaults to 0**. `Video.Ps2Emotion3D=1`
keeps rooms and lazy palettes. The model path stays in the build for
hardware A/B tests, since PCSX2's EE timing is approximate. The stats build
(`PD_PS2_OBJECT_STATS`) logs `E3D leaves/models/plain/...` once a second.

## Stage 4: VU1 microcode (folded into the GS work)

VU1 transforms about 950 vertices per frame in rooftop gameplay, in 32
batches. The object microcode costs about 80 cycles per vertex,
unpipelined, which is about 0.26 ms of VU1 time per frame. Pipelining it
fully would save about 0.13 ms.

The VIF1 wait the EE sees (23 % in `ps2GsNativeQueueSubmitInternal`) is
DMA and GS volume, not VU1 arithmetic. The VU1 microcode therefore gets
rewritten together with the GS-side libraries, where packet layout and
draw volume are decided, rather than on its own. The stats build logs the
VU1 counters as `E3D vu1`.

## GS layer, part 1 (round 21)

### What the GS side actually costs

The stats build (`PD_PS2_OBJECT_STATS`) now logs the native queue once a
second (`E3D gsq ...`): chains submitted and spilled, their size, IMAGE
uploads, how the chain splits between A+D state, VU1 input and DMA-referenced
templates, and how long the EE waited in each place. Defection intro and
rooftop, PCSX2:

| | cutscene | rooftop |
|---|---|---|
| chain per frame | 8-14 K QW (was 12-17 K) | 4-6 K QW |
| VU1 input | object path dominates | EE-transformed path dominates |
| EE wait for VIF1/GS at submit | ~0 us/frame | ~0 us/frame |
| EE wait for IMAGE uploads | ~0 us/frame | 0 |

So the EE never waits for the GS or VU1 in practice. The 7 % that the EE
profile charged to `ps2GsNativeQueueSubmitInternal` is the flip wait for
vsync, which is idle time. The GS receives about 40 draws and 100-250 KB a
frame, which is far below what it can take. What remains expensive is the EE
producing that stream.

### Packed object vertices (VIF1 V4-16)

Object-path vertices (rooms, rigid models, deferred blocks) were three
quadwords of int32 lanes. Every value fits 16 bits: N64 s16 positions,
scaled s16 texture coordinates and colour bytes. They are now 24 bytes:
three 64-bit stores on the EE.

VIF1 expands them with a V4-16 UNPACK, sign-extending each lane into the same
int32 VU memory image the microprogram already read, so the microprogram did
not change. A batch is now three UNPACKs: the V4-32 head (header, prefix
slots, transform), the V4-16 vertices and the V4-32 suffix, then MSCAL
(`ps2GsNativeQueueReserveVu1PackedBatch`).

The result is half the EE stores, half the DMA, and no 48-byte staging copy.
The cutscene chain fell from 12-17 K to 8-14 K QW per frame, and mid-frame
spills there went from about one per frame to none. Gfx time is 0.15-0.65 ms
lower per frame in the cutscene windows; PCSX2 undercharges uncached stores,
so hardware should gain more.

The VU test now also checks that a V4-16 unpack of the packed bytes
reproduces the image the microprogram is tested on.

### No more dropped frames from a full arena

Only gs_core's own A+D reservations could spill to the second arena. A full
arena hit through a VU1 batch, a template reference or a direct A+D
reservation marked the frame overflowed, and the whole frame was not drawn.
That happened at the end of the Defection intro, where frames reach 60-70 K
QW.

Every native-queue reservation now spills: it submits the full arena and
continues the frame in the other one. GS state persists across chains, so
nothing is replayed. Those frames are now drawn.

### Found: the tiled alpha-trilerp material

The 60-70 K QW frames come from alpha trilerp drawn through the tiled
channel-shuffle graph: about 10 GS passes per 64x32 tile, and 1,300-1,800
A+D packets plus 20-30 K QW of shuffle templates per frame.

Logging the triangles that reach it shows a single material: a PSMT8 56x54
window texture. Both taps sample the same texels, so it is not real mip
blending. The only reason it misses the one-pass same-sample path is a
per-vertex alpha add:

    alpha = T.a * 0.5 + add(vertex),   add = 20..138

The GS texture functions give `At*Av`, `At` or `At+Av` (the last also adds
`Av` to the colour). The 16-bit framebuffer has one alpha bit, so no
destination-alpha trick is available either. Making this one cheap means
changing how the material is represented, which belongs with the engine
rewrites rather than the transport layer.

A GS hardware-mipmap path (TEX1 MXL=1 with LCM=1, MIPTBP1) for true
two-level alpha trilerp was written and then removed: no triangle in these
scenes needed it.

## Next: PS2-specific GS libraries

* Measure on hardware where the GS time goes: alpha-trilerp passes, fill,
  texture uploads.
* Replace the gsKit-style per-draw packet path with Emotion3D-owned
  packet templates and a VU1 microcode that builds GS primitives (XGKICK)
  straight from object batches.
* Revisit models on top of that. With a VU1 skinning palette, a model
  would be one upload per instance instead of per-`G_VTX` EE transforms,
  which is where stage 3's structure (epochs, skinned joins, resident
  loads) pays off.
