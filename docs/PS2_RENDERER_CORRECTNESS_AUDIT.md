# PS2 renderer correctness audit

Date: 2026-09-16

This audit follows the real-hardware results through the `material-alpha`
diagnostic. It separates proven backend-contract defects from visual
hypotheses so another broad diagnostic build cannot accidentally hide the
fault it is meant to isolate.

## Confirmed defects fixed by this pass

### Fast3D area origin versus GS area origin

**POTWIERDZONE:** `GfxRenderingAPI::set_viewport` and `set_scissor` receive the
same bottom-left-origin areas consumed by OpenGL. The GS viewport translation,
native `SCISSOR` register and tiled pass graphs use top-left screen space.

The PS2 backend previously copied the Fast3D Y coordinate verbatim. A
full-screen area happened to survive because both origins produce zero, while
every partial viewport or scissor selected its vertically mirrored strip. The
adapter now converts

```text
top_y = target_height - bottom_y - area_height
```

before storing either area. Host tests cover full-screen and partial regions.

### Disabled depth testing still wrote Z

**POTWIERDZONE:** the portable/OpenGL contract disables both depth comparison
and depth writes when `depth_test` is false. The PS2 backend previously kept
`TEST.ZTE` enabled, selected `ZTST=ALWAYS`, and copied `depth_update` directly
to `ZBUF.ZMSK`. A nominally depth-disabled draw could therefore overwrite Z
across its coverage and reject later world, weapon or UI geometry.

`ps2GsCoreSetDepthMode` now disables `ZTE` and masks Z writes unless a depth
buffer exists and depth testing is enabled. The effective test/write truth
table is host-tested.

### Reversed-Z comparison and decal semantics

**POTWIERDZONE:** the portable backend uses a strict depth comparison for
ordinary opaque/translucent geometry, permits equal depth for primitive-depth
and `ZMODE_INTER`, and applies polygon offset for `ZMODE_DEC`. The PS2 backend
previously selected `GEQUAL` for every compared draw and ignored `zmode`.
Coplanar mask geometry could therefore pass as ordinary world geometry, while
real decals could fight or disappear.

The reversed-Z GS mapping now uses `GREATER` for ordinary OPA/XLU draws and
`GEQUAL` only for primitive-depth, INTER, DEC and subsequent passes of the
same material. DEC receives a saturating +2 depth bias toward the camera. It
falls back from VU1/PATH1 to EE/PATH3 because the shared affine VU mapping
cannot express that saturation safely. Host tests cover the comparison truth
table, mode classification and near-plane saturation. The bias is active only
when both depth testing and depth comparison are enabled, matching the
portable backend's explicit polygon-offset disable on `ZTST=ALWAYS` draws.

The adapter also now starts with depth testing and writing disabled. Fast3D's
cached rendering state is zero-initialised, so its first depth-disabled draw
deliberately emits no `set_depth_mode` call. Starting the GS at the old
depth-enabled `GEQUAL` default made early UI/LEGAL rendering depend on stale
backend state until the first later depth-mode transition.

The same initial-state audit now explicitly disables GS blending and enables
the expected color/alpha write lanes instead of inheriting gsKit bootstrap
values. This matches Fast3D's zero-initialised `alpha_blend=false` cache before
the first material transition.

### Fog state survived only the first pass-graph batch

**POTWIERDZONE:** tiled material graphs temporarily disable GS fog while
reconstructing scalar/color workspaces. Their common restore previously left
fog disabled. The vertex translator caches the fog colour across all batches
of one draw call, so a draw larger than the translation buffer re-enabled fog
for its first batch only. Later batches of the same model or room were emitted
with identical geometry/material data but different visibility.

The common graph restore now reinstates the active shader's fog enable and
last translated fog colour. Temporary workspace stages may still disable fog,
but that state can no longer leak into the next batch.

## Contracts reviewed without a new defect

- **CURRENT IMPLEMENTATION:** NPOT Fast3D coordinates are normalized against
  the logical upload and rescaled to the power-of-two `TEX0.TW/TH` extent.
- **CURRENT IMPLEMENTATION:** mirrored power-of-two residency uses a reflected
  two-period upload while ST retains the original logical period.
- **CURRENT IMPLEMENTATION:** shader clamp metadata is decoded from the
  last-texel centre into integer GS `REGION_CLAMP` maxima without truncating
  values outside the 10-bit register contract.
- **CURRENT IMPLEMENTATION:** texture upload `TBW` uses 64-pixel units and
  rounds PSMT4/PSMT8 buffers to an even unit count, matching their 128-pixel
  page width.
- **CURRENT IMPLEMENTATION:** RGBA16, RGBA32, IA16, CI4/CI8 and I/IA indexed
  conversions have byte-level host tests, including T4 nibble order and CSM1
  palette permutation.
- **CURRENT IMPLEMENTATION:** clear packets override and restore TEST, FRAME,
  ZBUF and SCISSOR instead of inheriting the previous material.
- **CURRENT IMPLEMENTATION:** CT32 RGB/alpha write lanes and CT16 aggregate
  masks are host-tested. Alpha-only multipass writes no longer mask all lanes.
- **CURRENT IMPLEMENTATION:** alpha threshold 8/256 is converted to 4/128, and
  texture-edge `> 0.19` is quantized to `GEQUAL 25/128`.
- **CURRENT IMPLEMENTATION:** pass-graph wrappers restore the default render
  target and persistent framebuffer/depth/scissor state after success or an
  internal failure.

## Remaining correctness gaps

1. **POTWIERDZONE:** `G_MODULATE_EXT` means `source * destination` in the
   portable backend. Complex PS2 pass graphs reject it explicitly, but the
   direct path currently enables ordinary source-over blending instead. An
   exact GS implementation needs owned framebuffer feedback or a material-
   specific channel graph. It must not be replaced with another approximate
   blend equation.
2. **POTWIERDZONE:** unsupported combiner recipes are still dropped. Renderer
   visibility cannot be complete until the hardware log inventories the
   remaining recipe IDs and their triangle counts.
3. **POTWIERDZONE:** mip generation/sampling and the portable framebuffer-copy
   API are not implemented. Ordinary mip LOD therefore currently feeds the
   known-good base tile to both combiner texture inputs. Real detail-texture
   mode still exposes two tiles. This deliberately removes incorrect adjacent
   mip sampling until a complete GS mip chain is implemented and validated.
4. **HIPOTEZA DO TESTU:** the slight striping reported on recognizable
   textures is filter fidelity rather than row pitch. The PS2 path maps N64
   filtered draws to GS bilinear sampling; it does not implement the portable
   three-point reconstruction. A point-versus-linear A/B should precede any
   coordinate bias.
5. **HIPOTEZA DO TESTU:** remaining LEGAL glyph corruption may have been caused
   by the partial viewport/scissor origin defect. If it survives this fix,
   isolate texture-rectangle/copy-cycle coordinates separately from ordinary
   triangle text.
6. **POTWIERDZONE W KODZIE, SPRZĘT DO TESTU:** Fast3D performs face culling
   before the backend homogeneous clipper. The old path divided by every
   vertex `W` and tried to correct mixed-sign triangles by negating the
   screen-space winding. That is not equivalent to clipping the polygon and
   can reject valid room geometry crossing the eye plane. The PS2-generated
   frontend now performs the cheap pre-cull only when all three `W` values are
   positive. Eye-plane-crossing triangles proceed to the six-plane backend
   clipper. Fully textured no-cull and no-depth artifacts isolate the two
   remaining state contracts without replacing materials with debug colors.

## Next hardware acceptance test

Use the ordinary Og artifact first. Record only these deltas:

- whether LEGAL glyphs changed;
- whether partial-screen effects appear in the correct vertical region;
- whether camera-angle-dependent world/weapon/UI occlusion remains;
- whether Carrington and the first mission retain more geometry;
- whether texture striping changes at all.

An unchanged stripe pattern alongside fixed visibility would confirm that
filter fidelity is independent from the viewport/depth defects.
