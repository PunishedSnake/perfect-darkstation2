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
