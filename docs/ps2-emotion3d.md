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

`port/fast3d/e3d_room_ps2.inc` walks the list once and splits it into ops:
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

## Next stages (revised)

3. **Models: matrix-palette skinning on VU1.**
   * Compile a model's display lists once per model definition. Each
     `G_VTX` becomes a vertex run tagged with the matrix it was loaded
     under, and each triangle refers to vertices across runs.
   * VU1 receives the node matrices of a model instance as a palette and
     transforms each vertex by its own matrix. This is the classic PS2
     skinning layout, and it removes both the per-`G_VTX` EE transform
     (`gfx_sp_vertex`, `gfx_ps2_direct_vertex`: ~8 % in gameplay) and the
     deferral and replay overhead.
4. **GS-side work**, aimed by hardware measurements: the alpha-trilerp
   passes and fill rate rather than packet building, which round 15
   already templated.
