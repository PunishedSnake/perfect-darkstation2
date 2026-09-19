# PS2 renderer trace

Updated: 2026-09-18

The retail-hardware renderer can capture one diagnostic frame without enabling
the global `pdps2.log` file logger. The capture is intentionally synchronous
and may stall gameplay while it writes to USB, but ordinary frames do not touch
the filesystem.

## Capture

1. Start the normal `pd-ps2-game.elf` build from USB.
2. Move to the scene and camera angle you want to inspect. You can capture
   from the menu, Carrington Institute or a mission.
3. Press **Select** on controller 1 once. The next complete renderer frame is
   buffered in EE memory and written as `pdps2-gs-trace.bin` beside the ELF.
4. Wait for USB activity to finish before resetting or removing the device.

Release Select before pressing it again. Each new capture replaces the previous
file, so copy a useful trace before recording another. A USB write can briefly
stall the game after that frame; Select is not forwarded as a game action.
The capture contains:

- ordered Fast3D/renderer state changes and draw/clip counts;
- shader IDs, exact combiner recipes and pass graphs;
- texture selections, upload metadata and a 64-bit source hash;
- depth, alpha, viewport, scissor and sampler changes;
- PATH1 submission metadata and the exact raw PATH3 GIF qwords;
- the final GS register shadow, resident texture/CLUT/render-target inventory;
- VRAM allocator and cumulative renderer statistics.

It does not read the four-megabyte GS VRAM back to the EE. The exact submitted
command stream, resource layout and authoritative software register shadow are
captured without introducing a risky local-to-host transfer into gameplay.

## Decode

Copy the binary file to a computer and run:

```sh
python3 tools/ps2_renderer_trace_decode.py pdps2-gs-trace.bin \
  --json pdps2-gs-trace.json
```

The console summary reports event counts, PATH1/PATH3 submission and qword
traffic, draw cost grouped by pass graph, unsupported shader IDs and fully
clipped draws, and the final GS shadow. The JSON
preserves those aggregates together with the complete event stream and decoded
PATH3 A+D register writes for analysis or comparison between two hardware
captures.

## First retail-hardware capture

The stage 38 capture from 2026-09-18 contained 6,093 ordered events with no
dropped event records. It isolated one transport pathology:

- `alpha_trilerp_modulate` consumed 181,513 microseconds of the 246,962
  microseconds covered by the capture;
- 80 input triangles expanded to 327 clipped vertices through the exact tiled
  material graph;
- those draws caused 2,800 PATH1 and 2,800 PATH3 submissions, carrying 47,598
  and 452,492 requested qwords respectively;
- the other 30 draws together caused only 46 PATH1 and 43 PATH3 submissions.

The alpha-trilerp graph was therefore not removed or approximated. Its
three-vertex tile passes are now kept in the already-open PATH3 arena instead
of paying a VIF1 chain, `FLUSHA`, VU1 launch and PATH ownership handoff for
every triangle. Batches of at least four triangles remain eligible for VU1.
This is a transport optimization; texture, combiner, alpha, depth and geometry
semantics are unchanged.

## Second retail-hardware capture

A later stage 38 frame from the Og build containing the batch threshold recorded
513 events, with no dropped event records. `alpha_trilerp_modulate` used zero
PATH1 and 29 PATH3 submits, down from 2,800 submits on each path in the first
capture. The frame took 199,997 microseconds in the instrumented recorder, and
the alpha graph took 131,867 microseconds. The frames were captured at different
moments, with 80 versus 90 alpha input triangles, so these durations do not
prove the exact uninstrumented FPS improvement. The alpha pass graph still
submitted 468,345 requested PATH3 qwords in this frame.

Both captures contain two occurrences of the same unsupported shader
`0x320d020d818a818a/0x0000000000000513`; the unsupported material appears
in three draw calls containing five input triangles. Three other draws,
containing six input triangles, have no vertices left after clipping.
These are candidates for missing geometry, not proof of the cause of every
missing model. For a useful comparison, press Select when a specific model
vanishes or a dark line appears and retain the corresponding view/photo.
The raw GIF/VIF capture stores at most 65,536 qwords; the second frame dropped
435,716 raw qwords, while retaining every high-level event and requested
submission size. Interpret the stored command stream as incomplete.

## Select captures at two viewing angles

Two further retail captures from stage 38 use the Select trigger. Their frame
numbers are 612 (`pdps2-gs-trace(2).bin`) and 474
(`pdps2-gs-trace(3).bin`); the file suffix does not establish capture order.
The recorder contains no screenshot, camera transform or model identifier, so
neither file can yet be labeled as the visible or missing-model view.

| Recorded frame | Captured duration | Input draws / triangles | Fully clipped draws | Unsupported draws / triangles | Alpha trilerp time | PATH3 requested qwords |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 612 | 241,635 us | 30 / 188 | 0 | 4 / 8 | 178,540 us | 695,735 |
| 474 | 155,956 us | 23 / 157 | 0 | 3 / 5 | 112,601 us | 431,123 |

Both captures retained all high-level events. The raw GIF qword recorder reached
its 65,536-qword capacity, dropping 631,020 and 367,011 requested qwords
respectively. Requested qword counts remain complete in the submit events.
These are distinct frames with different geometry; their duration difference
is not an isolated benchmark of a code change or an estimate of uninstrumented
FPS. The alpha trilerp graph accounts for about 74% and 72% of their measured
durations, respectively. Its PSMCT32-to-PSMT8 channel shuffle emits many
small sprites per tile and still dominates PATH3 traffic after the PATH1
handoff reduction.

The unsupported shader in both views is
`0x320d020d818a818a/0x0000000000000513`. Decode of the two-cycle shader ID
shows the same RGB equation as supported
`0x020d020d818a818a/0x0000000000000011`, but the second alpha cycle is
`COMBINED.a * INPUT2.a + INPUT3.a`, whereas the supported recipe ends after
the multiplication. `gfx_ps2.cpp` rejects unsupported shaders, so those 3 or
4 draws do not reach GS. Neither new view has a fully clipped draw; this rules
out wholesale clipping of a submitted draw in these frames, but does not rule
out an object being culled earlier by the game, individual triangles being
clipped, or a GS material/depth error.

The paired retail observation now identifies frame 474 / trace `(3)` as the
view in which the couch in the first Carrington room is missing. This turns the
previously anonymous comparison into a useful material correlation:

- **POTWIERDZONE:** the renderer drops the shader above before GS submission;
- **POTWIERDZONE:** its cluster uses a 56x54 PSMT8 texture in both views
  (handle 62 in frame 612, handle 47 in frame 474), next to the same 64x64
  PSMT4 alpha-trilerp resource shape (handles 41 and 48 respectively);
- **INFERENCJA:** the stable shader/resource pattern plus the missing-couch
  label makes this material a strong candidate for the couch surface, but the
  trace still does not carry a model identifier.

The PS2 combiner now accepts this exact additive-alpha variant. It reuses the
validated alpha-trilerp graph, keeps its scalar intermediate in the N64 0..255
alpha domain, adds `INPUT3.a` with a GS fixed-factor additive blend, and
converts the final alpha back to the GS blend-factor scale during composite.
This is deliberately restricted to the exact
`COMBINED.a * INPUT2.a + INPUT3.a` equation. **HIPOTEZA DO TESTU:** on real
hardware the unsupported count for this shader should drop to zero and the
couch should remain visible at the frame-474 camera angle. The extra additive
draw is local to this material graph; PATH3 traffic and frame-tail latency must
be re-profiled after correctness is confirmed.


## Additive-alpha follow-up capture

The next retail capture, frame 247 (`pdps2-gs-trace(4).bin`), was taken
after commit `357c94c7` made
`0x320d020d818a818a/0x00000513` a supported alpha-trilerp material.
It contains 32 draws / 247 input triangles and **zero unsupported shader
draws**. The formerly rejected shader now appears in four supported draws
containing eight input triangles and 33 clipped output vertices. All four use
the same 56x54 PSMT8 resource shape (handle 30 in this frame).

This falsifies the earlier simple hypothesis that dropping this shader was by
itself the cause of the missing couch. The material now survives planner and
clip submission, while the paired television image still shows severe missing
surface coverage. The next discriminator is therefore inside the tiled
pass-graph/composite path rather than the unsupported-shader gate.

The same capture reinforces the performance diagnosis but does not prove a
runtime packet overflow. It recorded 723,512 requested PATH3 qwords, of which
695,860 belong to `alpha_trilerp_modulate`. That pass graph consumed 184,401
of the 245,670 captured microseconds. The raw trace qword storage again filled
at 65,536 qwords and dropped 659,964 qwords; high-level events and requested
submission counts remained intact.

Starting with the next build, each alpha-trilerp draw records two lightweight
high-level `pass_graph_draw` events only while Select capture is active. The
decoder reports the screen-space bounding box, tile count, LOD-factor range,
shade-alpha range, additive INPUT3 alpha range and pass-graph success bit.
Ordinary gameplay frames do not perform this extra range scan. This should let
a paired screenshot identify whether the couch/computer geometry reaches a
valid tiled composite and whether its effective alpha inputs collapse near
zero.


## Fifth retail capture: remove definitely unused shuffle work

Frame 187 (`pdps2-gs-trace(5).bin`) was captured with the per-draw
pass-graph instrumentation from `7bc0e68e`. It contains 32 draws / 226 input
triangles and zero unsupported shaders. The recorded frame spans 254,431 us.

`alpha_trilerp_modulate` remains the dominant cost: 20 draws / 117 input
triangles account for 191,791 us and 773,975 requested PATH3 qwords, versus
804,516 PATH3 qwords for the complete frame. The instrumented alpha graph
contains 635 workspace-tile invocations after clipping.

The four draws using additive-alpha shader
`0x320d020d818a818a/0x0000000000000513` report `INPUT3.a == 0` at every
captured vertex. **POTWIERDZONE for this frame:** their additive triangle pass
does no useful mathematical work, so the renderer now skips that draw whenever
all three triangle vertices have zero additive alpha. The final normalization
remains unchanged.

The larger source of redundant transport is the CT32-red-to-alpha channel
shuffle. The old path emits every 8x2 shuffle sprite in the rectangular tile
even though the final composite samples only the source triangle. The new
default `PD_PS2_ALPHA_SPARSE_SHUFFLE=ON` path computes a conservative
2-pixel-row span for each triangle/tile and omits only 8x2 blocks that cannot
intersect the triangle. A one-pixel guard is included around the mathematical
coverage to avoid raster-edge false negatives. The original rectangular path
remains available as an A/B baseline with
`-DPD_PS2_ALPHA_SPARSE_SHUFFLE=OFF`.

**HIPOTEZA DO TESTU:** this should reduce PATH3 qwords substantially for large,
diagonal alpha-trilerp triangles without changing the pass equation. The next
real-hardware capture must compare requested PATH3 qwords, alpha-trilerp time,
visible edges and the existing missing-couch/computer symptoms. A lower qword
count is not accepted as a win if coverage changes.


## Sixth retail capture: sparse shuffle result and opaque-texture fast paths

Frame 292 (`pdps2-gs-trace(6).bin`) was captured from commit `45e24f77`.
The picture remained visually unchanged on the real console. The frame is not
the same workload as frame 187, so absolute frame times are not an A/B benchmark:
it contains 126 draws / 1,128 input triangles, versus 32 / 226 in trace (5).

The transport result is nevertheless strong. Despite the much larger workload,
the complete frame requested 397,081 PATH3 qwords instead of 804,516. The
alpha-trilerp graph covered 859 recorded workspace tiles; its spill-attributed
PATH3 traffic is 358,426 qwords. The previous capture recorded 635 tiles and
773,975 qwords in that pass. Normalized by recorded tile invocation, the
observed transport falls from roughly 1,219 to 417 qwords/tile. Because PATH3
submit events are arena-level rather than per-command accounting, treat this as
a strong hardware observation rather than an exact instruction-level cost
model.

The remaining alpha graph still consumes 252,168 us of the 428,621 us captured
frame. Current texture inventory shows that most heavy draws use native PSMT4
plus CT16 CLUT resources. The renderer now classifies texture alpha exactly
once at upload, including only palette entries actually referenced by CI texels.

When both trilerp textures are proven fully opaque, the N64 alpha equation
`lerp(TEXEL0.a,TEXEL1.a,LOD) * INPUT2.a` collapses exactly to `INPUT2.a`.
The renderer therefore bypasses the scalar target and channel shuffle. Fully
opaque INPUT2 triangles go straight through the existing two-pass opaque
trilerp path; other triangles reconstruct only RGB in the color workspace and
composite once using exact per-vertex INPUT2 alpha. Additive variants use these
paths only when INPUT3 alpha is exactly zero at all three vertices.

The original scalar/shuffle graph remains the correctness fallback whenever
texture alpha is not proven opaque. Select traces report both fast-path triangle
counts so hardware captures can quantify the removed work.


## Seventh retail capture: fast-path validation and same-sample collapse

Frame 297 (`pdps2-gs-trace(7).bin`) was captured from `8a4768f5`.
The user reported the same visible output but subjectively faster rendering.
The capture confirms a large renderer-side reduction. It contains 32 draws /
248 input triangles, close to frame 187's 32 / 226, while the captured frame
interval falls from 254,431 us to 121,744 us.

PATH3 drops from 804,516 requested qwords in frame 187 to 104,362 in frame 297.
`alpha_trilerp_modulate` falls from 191,791 us / 773,975 qwords to
58,830 us / 81,562 qwords. Its clipped output contains 161 triangles; 134
(83.2%) take the direct opaque fast path and therefore submit no workspace
tiles. The remaining expensive work is concentrated in 27 clipped triangles
and 129 workspace tiles using non-opaque textures, principally handles 45, 46
and 9.

A second exact redundancy is visible in the frontend state: every alpha-trilerp
draw in this capture has the same GS texture handle selected for TEXEL0 and
TEXEL1. Equal handles alone are not sufficient because the two cycles may carry
different coordinates or region clamps. The renderer now has an additional,
strict fast path enabled by `PD_PS2_ALPHA_SAME_SAMPLE_FASTPATH=ON`: it fires
only when texture handle, wrap/clamp state, region-clamp state and all three
vertices' TEXEL0/TEXEL1 coordinates compare exactly equal, and INPUT3 alpha is
zero.

Under those conditions both color and alpha lerps collapse algebraically to a
single texture sample. The renderer submits one ordinary textured triangle with
texture alpha enabled instead of any render-target/shuffle graph. The old path
remains available with `-DPD_PS2_ALPHA_SAME_SAMPLE_FASTPATH=OFF`. Select
captures report `fast_same_sample` so the hardware test will reveal whether
Carrington's remaining 27 expensive triangles satisfy the exact condition.


## Eighth retail capture: alpha-trilerp eliminated from the hot path

Frame 914 (`pdps2-gs-trace(8).bin`) was captured from `69834d08`.
The user reports another visible speed increase. This capture is closely matched
to frame 297: 32 draws / 242 input triangles versus 32 / 248.

The measured frame interval falls from 121,744 us to 78,653 us. Requested PATH3
traffic falls from 104,362 qwords to 18,710 qwords, with no trace-qword drops.
All 152 clipped alpha-trilerp triangles report `fast_same_sample`; their tiled
workspace count is exactly zero. The alpha-trilerp graph therefore falls from
58,830 us in frame 297 to 7,755 us in frame 914.

The bottleneck has moved. One `independent_tex0_alpha` draw
(shader `0x0000000001081000/0x00000001`, 16 input triangles) consumes
17,428 us and triggers a 16,373-qword PATH3 arena submission. Its equation is
independent INPUT1 RGB with `TEXEL0.a * INPUT1.a` alpha, with ordinary alpha
blending and no texture-edge/alpha-threshold option.

For that exact class the scratch render target is unnecessary. The new default
`PD_PS2_INDEPENDENT_ALPHA_DIRECT=ON` path first writes the primitive's source
alpha directly to framebuffer alpha with RGB masked, preserving depth testing
but not updating Z. It then renders INPUT1 RGB using the GS
`DESTINATION_ALPHA_LERP` equation, so destination alpha is the just-computed
source alpha. The RGB pass owns the normal depth update and leaves alpha
untouched. This preserves primitive order by executing the pair per triangle.
Threshold/texture-edge materials retain the tiled correctness fallback.

The same test also reports two correctness issues that are not explained by
the now-eliminated alpha-trilerp workspace: angle-dependent missing furniture,
and occasional black screen-crossing strips / texture corruption outside the
first room. Select captures now record post-clip screen bounds for every output
triangle. The decoder flags near-zero-W, extremely thin, and screen-spanning
triangles so a capture taken while a black strip is visible can distinguish a
clip/geometry failure from a material/texture failure.


## Ninth retail capture: direct independent-alpha blend-order regression

Frame 542 (`pdps2-gs-trace(9).bin`) validates the performance gain from
`81df9184` but exposes a correctness regression in the new direct
`independent_tex0_alpha` path. The user reports light sprites becoming opaque
white rectangles. The capture contains one direct independent-alpha draw with
18 triangles; that draw completes in 896 us. The full frame requests only
2,465 PATH3 qwords plus 1,291 PATH1 qwords and spans 60,743 us. Alpha-trilerp
still uses the same-sample path for all 152 clipped triangles and submits zero
workspace tiles.

**POTWIERDZONE in current source:** `ps2GsCoreSetAlphaBlend(true)` resets the
GS ALPHA equation to `SOURCE_OVER`. The direct independent-alpha path installed
`DESTINATION_ALPHA_LERP` first and then enabled blending, immediately
overwriting the custom equation. The RGB pass therefore used its own opaque
fragment alpha instead of the alpha captured into framebuffer destination
alpha, exactly matching the observed white opaque quads.

The fix is intentionally minimal: enable blending first, then install
`DESTINATION_ALPHA_LERP`. The direct path and its performance benefit remain
otherwise unchanged. Hardware validation must confirm restored sprite
transparency before this path is treated as correctness-proven.


## Tenth retail capture: bottleneck moved to draw/state overhead

Frame 910 (`pdps2-gs-trace(10).bin`) was captured from `d7ef02fe`
outside the first Carrington room while a texture-rendering problem was being
sought. It contains 135 draws / 1,182 input triangles and spans 235,344 us.
Trace storage is complete: PATH1 requests 8,182 qwords and PATH3 requests
11,673 qwords with zero dropped qwords.

The important performance result is that transport is no longer the dominant
problem in this workload. `alpha_trilerp_modulate` accounts for 81 draws /
638 input triangles and 40,058 us. All 2,211 clipped vertices, i.e. 737 output
triangles, take `fast_same_sample`, and the tiled workspace count is zero.
The current implementation still submits that exact fast path triangle by
triangle, repeatedly paying state/draw setup. The renderer now batches a fully
compatible same-sample draw into one triangle-list submission; mixed draws keep
the existing ordered per-triangle fallback.

The direct destination-alpha optimization is no longer enabled by default.
Although `d7ef02fe` corrected its ALPHA-register ordering and light sprites,
the user subsequently observed a menu regression where only the highlighted
text remained visible. Trace (10) does not capture the menu, so the exact
failure is not yet proven. Correctness takes precedence: the older tiled
`independent_tex0_alpha` path is restored as the default while the direct path
remains available for controlled A/B builds.

Texture filtering is also now an explicit correctness item. In trace (10), all
229 texture selections use GS linear filtering. **CURRENT IMPLEMENTATION:** the
Fast3D frontend reduces the N64 texture-filter state to a boolean
`linear_filter`, and the PS2 backend maps that boolean directly to GS nearest
or bilinear filtering. This is not evidence that GS bilinear reproduces the
N64's filtering footprint. Subsequent Select traces record per-draw UV ranges,
logical texture extents, region-clamp state and the active nearest/linear choice
so the reported texture corruption can be separated from a filtering-model
difference, bad UV range, or clamp/residency problem.

The post-clip geometry diagnostics in frame 910 report no near-zero-W output.
There are several thin or screen-spanning triangles, but the user reports that
the intermittent black lines were absent in this run, so those triangles are
not treated as causal evidence.


### Trace-overhead correction after frame 910

Frame 910 contains 1,295 `clipped_triangle_bounds` events because the first
version of the geometry diagnostic recorded every clipped output triangle.
Those events are useful for correctness but they make the captured frame a poor
absolute timing benchmark. The trace path now emits this event only for
screen-wide, screen-tall, very thin or near-zero-W output triangles. The same
frame would have retained only 36 such events. Normal `draw_clipped` totals
remain available, so future Select captures should perturb the workload much
less while preserving the evidence needed for the intermittent black-strip
investigation.


## Wombo-combo capture: frame 1887

Frame 1887 (`pdps2-gs-trace(20260919-110938).bin`) was captured from
`4334791d` while the user simultaneously observed an intermittent black strip,
a missing texture, visibly wrong filtering and clipping trouble.

The trace is complete: 101 draws / 834 input triangles, 10,964 requested PATH1
qwords, 6,089 PATH3 qwords and zero dropped events/qwords. No unsupported
shader is present. No recorded suspicious triangle has near-zero W. The clip
diagnostic retains 37 suspicious outputs: 29 thin, six screen-tall and two
full-screen triangles. Because an earlier no-strip capture contained comparable
thin/screen-spanning geometry, these bounds are not sufficient evidence that
the strip is a near-plane explosion.

The strongest black-strip correlation is instead the independent-alpha
execution choice. With the destination-alpha direct path enabled in frames 542
and 910, the user reported the strips absent. Restoring the tiled
`independent_tex0_alpha` correctness baseline restored the strips. In frame
1887 that graph is active again for shader
`0x0000000001081000/0x00000001`. **INFERENCE:** the per-triangle scratch
reconstruction/composite is the leading strip suspect because it re-rasterizes
triangle coverage through an intermediate target. This remains a correlation,
not framebuffer-level proof.

The previous destination-alpha shortcut stays disabled because it also caused a
menu-text regression. The replacement is representation-driven: native
IA4/IA8/I4/I8 textures now receive a small shared alternate CLUT whose RGB is
fixed to GS MODULATE unity (0x80) while alpha exactly preserves the source
intensity-format semantics. For the independent INPUT1-RGB / TEXEL0-alpha
material this makes one ordinary textured GS pass exact:

`RGB = 0x80 * INPUT1 -> INPUT1`
`A   = TEXEL0.a * INPUT1.a`

No scratch render target, framebuffer-alpha detour or second rasterization is
required. `PD_PS2_INDEPENDENT_ALPHA_MASK=ON` is the default; unsupported
texture representations keep the older tiled baseline. The destination-alpha
experiment remains OFF.

The capture also confirms the filtering mismatch as a separate issue. Every
recorded texture selection requests the backend linear path. The project
rendering API distinguishes `FILTER_LINEAR` and `FILTER_THREE_POINT`, and
the OpenGL backend implements three-point filtering explicitly while selecting
nearest hardware sampling for that mode. **CURRENT IMPLEMENTATION:** the PS2
backend stores the global mode but still maps the per-sampler boolean directly
to GS nearest/bilinear, so N64-style three-point filtering is not implemented.

All texture handles selected in frame 1887 are resident and uploaded in the GS
snapshot. The observed missing texture therefore is not explained by a missing
resource at capture end. UV tracing contains finite coordinates; repeat/mirror
coordinates may legitimately leave the normalized 0..1 interval, so such ranges
must not be treated as an error by themselves. The remaining texture suspects
are sampler/clamp semantics, palette/cache state, or earlier visibility/
geometry rather than a simple failed upload.


## Full forensic capture: frame 814 and indexed-texture correctness baseline

Frame 814 (`pdps2-gs-trace(20260919-155815).bin`) is the first complete
v2 forensic capture from `1c6b45a7`. It contains 3,291 events, 16,396 raw
GIF/VIF qwords and 433,776 bytes of VBO/clip-map payload, with zero dropped
events, qwords or blob bytes. All stored draw payload hashes validate.

The capture rules out several earlier suspects for the observed Carrington
corruption:

- PATH1 requests 12,203 qwords and PATH3 4,193 qwords. The 107 recorded PATH3
  ownership waits total only 880 us, max 11 us.
- The GS VRAM allocator reports 2,211,584 bytes free and a 2,199,552-byte
  largest free range. All 64 texture resources in the snapshot are resident
  and uploaded. No texture/CLUT/shared-CLUT VRAM range overlaps were found.
- The captured frame performs no texture upload or texture retirement/reclaim
  operation. The corruption therefore is not explained by upload churn in this
  frame.
- Every captured clipped VBO is finite and remains inside the configured
  `x/y = [-w,+w], z = [0,w]` clip volume. No suspicious output has near-zero
  W. The two full-screen triangles are the first draw and cover exactly
  640x448. This capture does not support a near-plane geometry explosion as the
  source of the texture corruption.
- The frontend explicitly requests `FILTER_LINEAR` (mode 1), not
  `FILTER_THREE_POINT`. Wrong-looking filtering in this frame therefore
  cannot be attributed simply to a missing three-point-filter implementation.
- The previous alpha-trilerp scratch graph remains absent: all captured
  alpha-trilerp pass-graph records report zero workspace tiles.

The strongest remaining common boundary is native indexed residency. Slot 0 is
CI4 + RGBA16 TLUT in 75 of 108 frontend draws and CI8 + RGBA16 TLUT in another
six. IA8/I4/IA4 account for another 24 draws. In other words, PSMT4/PSMT8 plus
CLUT state dominates the failing scene.

This is correlation, not proof. To isolate it cleanly, the branch now defaults
`PD_PS2_NATIVE_INDEXED_TEXTURES=OFF`. The existing current-source fallback is
the control: exact TMEM materialization stays authoritative, but CI/IA/I 4/8-bit
views are expanded through the portable RGBA32 importer instead of being kept
as native GS PSMT4/PSMT8 resources. RGBA16/RGBA32 direct paths remain unchanged.

**Hardware A/B interpretation:**

- if the shifted texture blocks, dotted black rows, turquoise surfaces and
  disappearing textured details substantially disappear, the fault is below
  the logical TMEM view and inside the native indexed GS translation/residency
  boundary (index packing, CLUT layout/load state, TBW/sampling, or related
  state);
- if the artifacts remain materially identical, native indexed residency is
  exonerated and the next investigation moves to sampler coordinate semantics,
  scissor/visibility and the remaining multipass material graphs.

This is deliberately a correctness baseline rather than a performance change.
RGBA32 expansion may cost more VRAM and bandwidth. The captured native frame
still had over 2.2 MiB free GS local memory, but the fallback's actual
real-hardware footprint and timing must be measured rather than inferred.
