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

## Where the time still is

From the EE profile of the same scenes:
* **TMEM emulation and texture identification: ~8-9 %.**
  `gfxRdpTmemWriteTlutBytes`, `LoadBlockLinear`, `HashValidRange`,
  `TextureFingerprint` and `IdentifyTexture`. Room textures are static, so
  most of this re-derives the same GS texture every frame.
* **Display-list decoding and vertex work for models** (props, characters,
  weapons): `gfx_run_dl`, `gfx_sp_vertex`, `gfx_ps2_direct_vertex`, object
  contexts and matrix work, ~20 %.
* **Triangle setup and combiner → GS state mapping: ~4-5 %.**

## Next stages

2. **Room state ops resolved to GS state.** Record what each STATE op
   resolves to the first time it runs: GS texture handle, sampler, combiner
   path. Replay that instead of the TMEM emulation, as long as the inputs
   and the TMEM contents it relies on are unchanged. This is where most of
   the remaining per-frame cost of rooms is.
3. **Model display lists.** Model definitions are static per model, and
   only matrices, colours and a few dynamic fields change. Compile their
   lists the same way and key them by model definition.
4. **Prebuilt GS/VU1 packets.** Once state and geometry are both compiled,
   a leaf's DMA chain can be built once and only patched per frame.
