# Vector-unit geometry pipeline (plan)

## Where it stands

* **VU0, macro mode:**
  * vertex transform (one MULA/MADDA/MADDA/MADD chain per vertex)
  * matrix multiplies
  * vertex lighting: four lights' intensities in one chain, accumulation
    identical to the scalar code (checked on 1.77 M vertices)
  * lookat dot products for texture generation
* **VU1, micro mode:** projection, perspective divide, ST/Q and GIF packing
  for the shared-vertex batches. These are plain-textured, same-tap trilerp
  and untextured shaders, about three quarters of gameplay geometry.
* **EE:** the rest of the work on each vertex and triangle. Measured in EE
  cycles per gameplay frame:
  * vertex processing: ~0.43 M. Unlit vertices cost ~155 cycles each;
    lit + texgen vertices ~330 each.
  * triangle setup, culling and record copies: ~0.62 M
  * the remaining old-path shaders (alpha trilerp and similar): ~0.73 M

## Target

The EE only walks the display list and uploads raw data. For each G_VTX it
sends the s16 positions, the colour/normal bytes and S/T to VU1 as they are.
Matrices, lights, fog and texture scale go up once per change. A VU1
microprogram then does all of the following for every vertex:

* transform
* lighting
* texgen
* fog
* clip codes

Triangles are sent as index lists (G_TRI1/G_TRI4 already are). VU1 then:

* rejects them by outcode AND
* culls them by the sign of det[x y w]
* clips near-plane crossings
* emits GIF packets

The EE stops touching per-vertex data entirely.

## Steps

1. A VU1 vertex cache microprogram (transform, lighting, fog, outcodes) whose
   output is read back by the EE. This replaces gfx_sp_vertex. It can be
   verified bit for bit against the current macro-mode path, the same way
   lighting was.
2. Triangle assembly on VU1 for the shared-vertex shaders: index lists in,
   GIF packets out. Rejected triangles never leave VU1.
3. Near-plane clipping on VU1. Only clipped triangles still fall back to the
   EE.
4. Multi-pass shaders, alpha trilerp first, as several output passes from one
   vertex upload.

Each step keeps the existing path as a verified fallback, so it can land on
its own.

## Object-space geometry (round 14)

The first piece of this that ships is `gs_vu1_object_transform.vsm`, which takes
a different route to step 1. It does not read VU1 output back. Instead it skips
the EE transform for every block that can't need clipping:

* **G_VTX** (`gfx_obj_try_defer`, `gfx_pc.cpp`) computes the block's object-space
  bounding box with MMI `PMINH`/`PMAXH`. VU0 tests the box against the block's
  clip matrix as twelve planes: the guard band, near, far, `w >= 1e-3`, and the
  exact x/y planes. The matrix has aspect, Y flip and depth remap folded in.
  * Inside the guard band: the slots keep pointers to the source `Vtx` and
    colour array. Nothing is transformed.
  * Outside one exact plane: the slots are kept with that plane mask, and
    triangles over them are rejected as Fast3D would.
  * Straddling: the usual EE path.
  * Deferral only happens while the last triangle setup could take object
    batches. That way, states that end up on the VBO path don't pay a replay.
* **Triangles** over deferred slots of one block context are culled in object
  space. Fast3D's `-det[x y w]` equals a dot product of the triangle's plane
  with a per-matrix vector (Cauchy-Binet; `gfxPs2ObjectCullCross`). Each kept
  triangle appends three raw vertices to the batch: int32 position, G_TEXTURE
  scaled `u, v`, and colour bytes.
* **VU1** builds the whole record: clip matrix, `s, t` scale/bias, and the
  colour channel program. The program is linear because every direct colour
  table is `round(i / 255 * K)`. VU1 also computes fog, then does the same
  divide/viewport/pack as the textured program.
* **Fallback.** Any other use of a deferred slot replays the original G_VTX
  for its block with the captured transform inputs (`gfx_obj_replay`). This
  covers a mixed triangle, a non-direct state, `G_MODIFYVTX`, and the end of
  the frame. Every other path still sees exactly what it used to.

`Video.Ps2ObjectGeometry=0` in `pd.ini` turns it off for A/B checks. Host
tests:
* `gs_vu1_microprogram_test.py` runs the microprogram against the payload
  builder.
* `gfx_ps2_object_test.cpp` checks the object-space cull sign against the
  transformed test. It also checks that box classification is conservative,
  and the EE conversion.
