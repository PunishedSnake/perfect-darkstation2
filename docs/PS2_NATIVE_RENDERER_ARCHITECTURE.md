# Perfect DarkStation 2 native renderer architecture

Status: active design record for the `ps2` branch.

This document records the intended PS2 renderer/dataflow architecture so that implementation decisions survive individual development sessions. It is deliberately stricter than a wishlist: every substantial optimization should be tied to an observed bottleneck, a documented hardware/API contract, or a real-hardware experiment.

## 1. Source-of-truth policy

PS2 work follows the project `PS2 Optimization Research Library v2` routing.

Priority on conflicts:

```text
v2 corpus
> more specialized current corpus
> current source/manual for the exact fact
> real-hardware reproduction
> integrator corpus
> emulator/reverse engineering
> historical forum/anecdote
```

Relevant authoritative project corpora:

- `PS2_Optimization_Library_v2_MANIFEST.md`
- `PS2_PERFORMANCE_BIBLE.md`
- `PS2_Graphics_Synthesizer_optimization_research_corpus_v2.md`
- `PS2_VU0_VU1_optimization_research_corpus_v2.md`
- `PS2_VIF_optimization_research_corpus.md`
- `PS2_DMAC_RDRAM_Scratchpad_MainBus_optimization_research_corpus_v2.md`
- `PS2_IPU_optimization_research_corpus.md`
- `PS2_Data_Oriented_Design_optimization_research_corpus_v2.md`
- `PS2_Whole_System_Scheduling_research_corpus_v2.md`
- `PS2_PS2SDK_optimization_research_corpus_v2.md`

Epistemic labels used below:

- **CONFIRMED**: hardware manual, current source, or real-hardware reproduction.
- **CURRENT IMPLEMENTATION**: behavior of the current `perfect-darkstation2` / PS2SDK / gsKit implementation.
- **HISTORICAL**: old toolchains, old stacks, forum measurements.
- **INFERENCE**: architecture conclusion not yet directly measured.
- **TEST HYPOTHESIS**: explicit candidate for real-hardware A/B measurement.

## 2. Core rule: preserve N64 semantics, not N64 execution

The renderer is not intended to emulate an N64 graphics processor inside the EE.

The compatibility layer must preserve the observable semantics needed by Perfect Dark and Fast3D/RDP commands, while executing them using PS2-native units and representations.

Nintendo's N64 Programming Manual is used to understand the original contract:

- Graphics Interface, Chapter 4: CPU builds GBI display lists; RSP performs matrix/geometry work; RDP performs rasterization, texturing, blending, Z and AA.
  - https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro04/04-05.html
- RDP Texture Engine, Chapter 12: TMEM is 4 KiB and is described by eight tile descriptors.
  - https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro12/12-04.html
- Texture Mapping, Chapter 13: textures are loaded into TMEM through `LoadTile`, `LoadBlock`, or `LoadTlut` and then sampled through tile state.
  - https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-01.html
- Texture Loading, Chapter 13: `SetTextureImage` selects the source; the load command mutates TMEM; load and render tiles may intentionally differ.
  - https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-09.html

**CONFIRMED:** the N64 split is conceptually useful: the RSP is geometry-oriented and the RDP is raster/texture-oriented.

**INFERENCE:** on PS2, much of the regular Fast3D/RSP-style geometry workload naturally belongs on VIF1/VU1, while GS is the final raster consumer. This is an execution mapping, not a claim that VU1 is instruction-compatible with RSP.

## 3. Target PS2 execution map

```text
                         Perfect Dark
                              |
                     game / simulation
                              |
                         EE / R5900
                 control + irregular work
                   /        |          \
                 VU0       IPU         IOP
             small math   images     services
                            |
assets / streaming -> DMAC -> VIF1 -> VU1
                            |       |
                         unpack   geometry
                                  lighting
                                  transform
                                  texgen
                                  clipping
                                     |
                                   XGKICK
                                     |
                                    GIF
                                     |
                                    GS
                          texture / raster / Z
                          blend / multipass
```

The desired direction is to minimize unhideable time, copies, transport and synchronization between a producer and its final consumer.

## 4. Hardware workload policy

### 4.1 EE / R5900

Primary responsibilities:

- gameplay, AI, visibility and control flow;
- command/state extraction from the Fast3D compatibility frontend;
- irregular work and pointer-heavy structures that do not map cleanly to VU;
- scheduling and ownership transitions between asynchronous producers/consumers.

Do not keep regular bulk vertex work on EE merely because the first implementation is convenient.

### 4.2 VIF1 + VU1

Target responsibilities:

- packed geometry input;
- VIF unpack/expansion into VU1 local memory;
- matrix transform and projection;
- regular lighting/texgen/fog work where semantics match;
- clipping/rejection where a measured batch-friendly implementation is appropriate;
- generation of GS-ready vertex/GIF output;
- XGKICK directly toward GS when possible.

Preferred long-term path:

```text
PS2-ready packed mesh
 -> DMAC
 -> VIF1 UNPACK
 -> VU1 microprogram
 -> GS-ready records
 -> XGKICK
 -> GIF
 -> GS
```

**CONFIRMED:** VU1 is appropriate for regular batched vector work and VIF is designed to format/unpack streams for VU local memory.

**INFERENCE:** the current CPU Fast3D geometry path is a useful correctness baseline, but it should not be the final high-performance execution path.

### 4.3 VU0

Use only when the workload and ownership justify it. Candidate roles include small recurring math kernels that would otherwise burden EE and do not interfere destructively with VU0 macro-mode use.

No task is moved to VU0 merely because it is idle.

### 4.4 GS

GS is the final graphics consumer. The renderer should prefer GS-ready representations and long native GIF streams over repeated CPU-side repacking.

Priorities:

1. correct Fast3D/RDP semantics needed by Perfect Dark;
2. explicit texture residency in the 4 MiB GS local memory;
3. batching of state and primitives;
4. early texture upload relative to first use;
5. minimal unnecessary `FINISH`/global waits;
6. multipass only when required by the N64 combiner/blender semantics being reproduced.

### 4.5 IPU

The IPU is not reserved only for FMV.

The authoritative IPU corpus and SCE EE documentation support:

- MPEG-1/2 macroblock decode paths;
- standalone YCbCr -> RGB conversion (`CSC`);
- RGB32 -> RGB16 / INDX4 conversion (`PACK`);
- ordered dithering in the supported conversion path;
- 16-entry vector-quantization table loading (`SETVQ`);
- threshold-driven alpha generation (`SETTH` in the supported CSC path);
- fixed-length bitstream extraction (`FDEC`);
- DMA input/output and use of decoded images as textures.

Candidate uses are classified below.

#### A. Compressed texture / large image streaming

```text
compressed or intra image
 -> toIPU DMA
 -> IPU decode / conversion
 -> fromIPU DMA
 -> GIF IMAGE upload
 -> GS VRAM
```

**CONFIRMED:** SCE documentation describes decoded image data being usable as texture/display-list data.

**TEST HYPOTHESIS:** for sufficiently large streamed imagery, IPU decode plus direct GS consumption can reduce EE work and/or storage bandwidth despite setup cost and Main Bus contention.

Do not encode every small Perfect Dark texture as MPEG. Small CI/IA/RGBA assets should first be evaluated for direct offline conversion into GS-native formats.

#### B. RGB32 -> RGB16 packing

**CONFIRMED:** IPU `PACK` supports this conversion, with hardware dithering in the supported path.

**TEST HYPOTHESIS:** when a producer already emits RGB32 and the final GS representation can be RGB16, IPU packing may reduce both EE conversion work and subsequent transfer/VRAM footprint.

Do not first create RGB32 only to justify using IPU.

#### C. RGB32 -> INDX4 / 16-entry VQ

**CONFIRMED:** `SETVQ` loads a 16-entry codebook and `PACK` can produce 4-bit indexed output using the fixed nearest-centroid mechanism described by the hardware.

**TEST HYPOTHESIS:** suitable effect/HUD/decal/data textures may benefit if the codebook is prepared offline and reused across large batches.

The IPU does not build an optimal palette for us. Codebook generation remains an offline/CPU responsibility.

#### D. Standalone CSC and threshold alpha

Useful for future video textures, streamed backgrounds, camera-like inputs, or other content that naturally exists as YCbCr.

Do not perform YCbCr conversion if the actual consumer only needs luminance.

#### E. FDEC/custom parser use

**CONFIRMED:** fixed-length bit extraction exists.

**TEST HYPOTHESIS:** it is only interesting when bitstream work is large enough, DMA-fed, and overlapped with useful EE work. Ordinary R5900 shifts/masks are otherwise too cheap to replace with a ceremonial coprocessor trip.

#### IPU non-goals

Do not assume undocumented programmability. In particular the current research corpus does not support treating the IPU as:

- a general programmable SIMD processor;
- an arbitrary IDCT kernel engine;
- an arbitrary Huffman/JPEG/PNG decoder;
- a programmable 3x3 CSC matrix unit;
- a shader processor;
- a free background worker with no bus/synchronization cost.

### 4.6 IOP / SIF

IOP remains a service CPU, not spare game compute.

Use it for device-local services such as storage, input, audio/network support and streaming orchestration. Bulk EE/IOP traffic should be grouped and asynchronous where the service contract allows it.

### 4.7 SPU2

Audio data should converge toward SPU2-ready representation, preferably prepared offline when possible. Audio has real deadlines and is not a resource to starve because graphics found a new DMA hobby.

## 5. Asset representation policy

Prefer a representation already suitable for the final consumer.

### Geometry

Long-term target:

```text
quantized / packed vertex data
 -> VIF-ready stream
 -> VU1 expansion/transform
 -> GIF/GS-ready output
```

Avoid runtime float-heavy repacking when equivalent static asset transformation can be performed offline.

### Textures

Current/candidate policy by class, subject to actual Perfect Dark content profiling:

```text
CI4 / CI8 RGBA TLUT -> PSMT4 / PSMT8 + PSMCT16 CSM1 CLUT active
CI4 / CI8 IA16 TLUT -> PSMT4 / PSMT8 + PSMCT32 CSM1 CLUT active
IA4 / IA8 / I4 / I8 -> PSMT4 / PSMT8 + shared PSMCT32 CSM1 CLUT active
RGBA16         -> exact live-TMEM view -> PSMCT16 active path
IA16           -> exact live-TMEM IA16 -> PSMCT32 staging active path
large imagery  -> IPU experiment candidate
runtime RGBA32 -> native GIF IMAGE baseline, then remove copies if measured worthwhile
```

CI index data and its CLUT are allocated, uploaded and published as one logical residency. CI4 upload reverses the two indices in each source byte for GS low-nibble-first PSMT4 addressing. CI8 index bytes are copied directly. Indexed TBW uses the GS 128-pixel buffer-width alignment, while direct-color formats retain 64-pixel alignment. The RGBA5551 palette is converted to PSMCT16 and the 256-entry CI8 case is permuted to CSM1 order.

IA4/IA8/I4/I8 preserve the portable Fast3D importer's exact RGBA expansion through four immutable PSMCT32 palettes. Each palette is materialized lazily once and shared by every texture of that encoding; only the compact PSMT4/PSMT8 index plane belongs to the texture slot. This retains I-format intensity-as-alpha and IA-format independent alpha without a four-byte-per-texel expansion. The core keys the GS CLUT cache by actual VRAM address and pixel format, so switching between handles sharing a palette does not reload it, while any mutable CI palette upload invalidates the key. IA16 texels now expand directly from authoritative TMEM bytes into PSMCT32 IMAGE staging, preserving independent eight-bit intensity and alpha without traversing the generic Fast3D RGBA buffer. CI4/CI8 with IA16 TLUT keeps its compact PSMT4/PSMT8 index plane and uploads an exact PSMCT32 CSM1 palette, published transactionally with the index block. The cache identity includes TLUT interpretation so identical words cannot alias between RGBA5551 and IA16 residency.

## 6. TMEM semantic requirement

This correctness milestone is now active in the PS2 frontend.

Nintendo documents TMEM as 4 KiB of on-chip texture memory with eight tile descriptors. `SetTextureImage` identifies source image state, while `LoadTile`, `LoadBlock`, and `LoadTlut` populate TMEM. Render tile state can differ from the load tile state.

**CURRENT IMPLEMENTATION:** the portable renderer still contains its historical source-pointer compatibility records, but the generated PS2 Fast3D frontend also executes every relevant command against a backend-independent 4096-byte live TMEM model with eight load-tile descriptors. `SetTextureImage` only updates source state; `LoadTile`, `LoadBlock` and `LoadTLUT` mutate the live model.

The PS2 cache/import boundary now:

1. requests a checked logical view from live TMEM;
2. reads ordinary layouts byte-exactly, including partial final rows;
3. reconstructs split-bank RGBA32 texels;
4. derives CI4/CI8 palettes from the authoritative replicated TLUT words;
5. keys cache identity from materialized TMEM content and relevant layout state;
6. imports the materialized view rather than rereading the original RDRAM pointer.

YUV, malformed commands and layouts whose exactness cannot yet be proved are explicit compatibility fallbacks. Their counters remain visible and they do not masquerade as exact TMEM materialization.

This semantic layer should remain backend-independent where possible. PS2-specific residency starts after the compatibility frontend has determined the logical texture content/state.

## 7. Current native GS status

**CURRENT IMPLEMENTATION:**

- frame/state/primitive PATH3 commands use a project-owned native GIF PACKED A+D queue;
- **CURRENT IMPLEMENTATION:** an opt-in VU1 validation artifact can route
  ordinary color and textured batches through `DMAC -> VIF1 UNPACK -> VU1 ->
  XGKICK -> PATH1`. It uses two 256-QW VU1 banks and two EE staging slots,
  emits the same final A+D records as the PATH3 baseline, and falls back to
  PATH3 if setup or submission fails. The first physical A/B intentionally
  serializes each batch with `FLUSHA -> MSCAL -> FLUSH`; overlap is not claimed
  until that ordering scaffold passes on retail hardware;
- **CURRENT IMPLEMENTATION:** cumulative renderer counters record EE vertex
  translation batches, vertices and microseconds, PATH1/PATH3 color/textured
  batches, final A+D records and VU1 rejections. The serial snapshot is emitted
  on frame one and every 300 frames, never inside a primitive submission;
- **CURRENT IMPLEMENTATION:** ordinary state and primitive reservations may spill across any number of the two alternating PATH3 command arenas in one logical frame. A spill submits the full arena, begins the other one and continues in-order with persistent GS register state; it waits only for GIF-channel ownership and never inserts `FINISH`. Single packets larger than an arena remain a hard error, with host-tested boundary arithmetic preventing unsigned-capacity wraparound;
- texture uploads use project-owned GIF IMAGE source chains with two persistent staging slots;
- upload staging is prepared before claiming the GIF channel, following `submit early, wait late` as far as dependency allows;
- upload chains end with `TEXFLUSH` and do not insert GS `FINISH`;
- draw/state submission serializes on GIF channel ownership, preserving upload -> TEXFLUSH -> dependent draw order;
- a fixed-metadata project allocator owns the post-framebuffer texture region of the 4 MiB GS VRAM and uses 256-byte block alignment;
- **POTWIERDZONE:** `FRAME.FBP` addresses local memory in 8192-byte units; current gsKit likewise rounds system-buffer allocations to 8192 bytes. The project allocator now supports explicit power-of-two alignment instead of assuming every GS resource is a texture;
- **CURRENT IMPLEMENTATION:** transient PSMCT32 render targets use complete 64x32 GS pages, an 8192-byte-aligned base and the same reclaimable VRAM pool as textures. Four fixed metadata slots encode producer/consumer lifetime without an EE heap allocation;
- binding a transient target emits native `FRAME`, full-target `SCISSOR` and `TEST` state. Z is disabled while offscreen because the screen-sized system Z buffer does not share the target's arbitrary `FBW`; rebinding the default target restores persistent Z state;
- **POTWIERDZONE:** after local-memory texture data changes, `TEXFLUSH` is a required texture-cache transition rather than a general-purpose GS completion fence;
- **CURRENT IMPLEMENTATION:** a successful clear or primitive draw marks the active transient target initialized and texture-dirty. Uninitialized targets cannot be sampled. The first later sample emits `TEXFLUSH` in the ordered native A+D stream, then clears the dependency bit. A failed command-arena reservation leaves the bit set, so the barrier cannot be silently lost;
- transient targets can be sampled directly as PSMCT32 without an EE-memory copy. Sampling the active draw target is rejected as an uncontrolled feedback hazard, while NPOT targets use temporary `REGION_CLAMP` bounds and restore the persistent material clamp after the draw;
- **POTWIERDZONE:** `PSMT8H` reinterprets the high byte of each 32-bit local-memory word as an indexed texel, so the alpha lane of a PSMCT32 target does not require the spatial 32-to-8-bit shuffle used for low RGB lanes;
- **CURRENT IMPLEMENTATION:** a fifth immutable shared PSMCT32 CSM1 palette maps every byte `i` to `RGBA(i,i,i,i)`. The render-target alpha-view API combines it with `PSMT8H`, the same dirty-tracked `TEXFLUSH` dependency and exact NPOT clamp, exposing framebuffer alpha as texture intensity without an EE copy;
- **CURRENT IMPLEMENTATION:** the low-lane mapper models every byte coordinate in a 64x32 PSMCT32 page and its 128x64 PSMT8 reinterpretation. An exhaustive host test inverts all 8192 pixel/channel mappings. The native channel-to-alpha blit uses page-local sprite segments, the identity CLUT and `FBMASK=0x00ffffff` to copy red, green, blue or alpha into a distinct active CT32 target's alpha byte while preserving RGB;
- the channel blit streams only the requested upper-left dirty rectangle through bounded command-arena chunks, temporarily owns its `SCISSOR`, disables alpha/depth tests for the copy, then restores persistent `FRAME`, `SCISSOR`, `TEST` and `CLAMP` state. Source and destination must be initialized, equal-sized, distinct transient targets, so uncontrolled local-memory feedback is not admitted;
- **POTWIERDZONE:** the page-local PSMCT32-to-PSMT8 coordinate model and pixel-corner sprite placement passed the deterministic 128x64 A/B image on a physical PS2 with the CI #193 artifact. The console model and video mode were not recorded, so they remain useful reproduction metadata rather than a correctness gate;
- render-target release uses the existing fence-delayed retired-block queue. The block cannot return to the allocator until earlier GS consumers are complete, and an active target cannot be released;
- texture replacement is transactional: a new residency is uploaded before the old baked `TEX0` address is retired;
- retired blocks are reclaimed only after an explicit native GS `FINISH` fence proves that previous consumers are done;
- normal draw and upload submission does not wait for `FINISH`; fences occur at PCRTC publication and under genuine texture-allocation pressure;
- the PS2 Fast3D cache is capped at the backend's 64 texture handles so eviction can recycle handles instead of exhausting the backend table;
- exact live-TMEM N64 RGBA5551 textures are converted directly into GS A1B5G5R5 staging and reside as PSMCT16, halving their IMAGE payload and local-memory footprint relative to the RGBA32 baseline;
- exact split-bank live-TMEM N64 RGBA32 is reconstructed into canonical RGBA bytes and uploaded directly as PSMCT32, bypassing the generic Fast3D importer buffer while retaining native mirror expansion and transactional residency;
- exact IA4/IA8/I4/I8 textures remain compact PSMT4/PSMT8 index planes and select one of four lazy immutable PSMCT32 CSM1 palettes shared across all texture handles;
- TEXA expands the PSMCT16 alpha bit to the same 0..255 texture-alpha convention used by the existing combiner adapter;
- the project owns the per-frame VSync wait, `DISPFB2` publication, draw-buffer selection and native `FRAME`/`SCISSOR` packet;
- a host-tested combiner planner reduces exact two-cycle modes to one GS pass when the final channel is independent of `COMBINED` or merely passes the first-cycle result through;
- **CURRENT IMPLEMENTATION:** direct `TEXTURE_EDGE` recipes follow the portable Fast3D contract without shader emulation: GS `TEST` rejects quantized fragment alpha below 25/128, source-alpha blending is disabled for accepted fragments, and `FBA` forces the stored framebuffer alpha MSB to the native opaque value. The state is scoped to the draw and restored immediately, so frame clears and later materials cannot inherit it. Multipass texture-edge combinations remain explicit unless their final composite owns the same transition;
- **CURRENT IMPLEMENTATION:** direct `INVISIBLE` recipes retain primitive submission and the caller's Z test/write mode while setting `FRAME.FBMSK=0xffffffff`, so depth-only occluders cannot alter any framebuffer lane. The write mask is restored after the draw; invisible multipass graphs remain rejected because their offscreen ownership would need a separate semantic plan;
- opaque `TRILERP -> PASS2` and `TRILERP -> MODULATEI2` are reconstructed from distinct live-TMEM `TEXEL0`/`TEXEL1` tiles as two ordered GS passes: the first writes `TEXEL0`, and the second uses the GS `ALPHA` equation to blend `TEXEL1` by the per-vertex Fast3D LOD fraction;
- opaque `CUSTOM_17/19 -> CUSTOM_18` color is reconstructed as `ENV * SHADE` followed by `TEXEL0 * SHADE`, blended by per-vertex `SHADE_ALPHA`;
- the interpolation pass disables Z writes, preserves framebuffer alpha through `FRAME.FBMSK`, and restores depth/blend/clamp state after submission;
- PS2 detail textures default on and the PS2-only Fast3D seam exposes the explicit tile pair because the GS path does not own OpenGL-style generated mip chains;
- **CURRENT IMPLEMENTATION:** `CUSTOM_25/26 -> MODULATEIA2` use a default-enabled one-target tiled graph. RGB is reconstructed as `(lerp(TEXEL0,TEXEL1,LOD) * SHADE)`, while the first pass writes the independent final alpha `ENV.a * SHADE.a` or `TEXEL0.a * ENV.a * SHADE.a`. The TEXEL1 interpolation masks alpha writes, so the completed PSMCT32 tile can be composited once through the caller's original alpha/depth state without the unvalidated low-byte channel shuffle;
- **CURRENT IMPLEMENTATION:** `CUSTOM_21 -> CUSTOM_18` with `TEX_EDGE` reuses the one-target graph for RGB and stores interpolated `SHADE.a` in target alpha. Because `ENV.a` is draw-constant and this mode observes `SHADE.a + ENV.a` only through the edge predicate, the final GS alpha reference is reduced by quantized `ENV.a`. The composite performs the sole alpha test and FBA promotion, avoiding both a second scalar target and incorrect per-pass rejection;
- **CURRENT IMPLEMENTATION:** `CUSTOM_22 -> CUSTOM_23` preserves the signed alpha equation `PRIMITIVE.a + TEXEL0.a * (SHADE.a - ENV.a)` without storing a negative framebuffer value. The EE clips each triangle at the screen-linear `SHADE.a = ENV.a` boundary while carrying STQ and Z planes into at most two triangles per half. The positive half tests `TEXEL0.a * (SHADE.a - ENV.a)` with `GEQUAL`; the negative half tests `TEXEL0.a * (ENV.a - SHADE.a)` with `LEQUAL`. Quantized `PRIMITIVE.a` moves into the respective GS alpha references, RGB is reconstructed only once, and both halves share the sole final depth/FBA composite;
- **CURRENT IMPLEMENTATION:** `CUSTOM_24 -> MODULATEIA2` uses the two-target tiled graph for its nonlinear final alpha `ENV.a * SHADE.a * (1 - SHADE.a)`. A solid scalar pass places the linear `ENV.a * SHADE.a` term in source RGB and `SHADE.a` in source alpha, then programs GS `ALPHA` as `(0-Cs)*As+Cs`. The hardware-validated red-to-alpha shuffle assembles that scalar lane with the independently reconstructed trilerp RGB before the sole depth/blend composite;
- the GS `ALPHA` factor selection is now a small host-tested device contract instead of a raw register literal hidden in the renderer. Ordinary source-over and the `CUSTOM_24` inverse-source-alpha equation are explicit states, and enabling normal alpha blending always restores source-over;
- native fog remains exact for this graph because the same per-vertex fog coefficient and fog colour are applied to both texture endpoints before their LOD interpolation. Destination-colour modulation remains rejected because it would require framebuffer feedback outside this graph's ownership contract;
- **CURRENT IMPLEMENTATION:** alpha-bearing `TRILERP/MODULATEIA2` now has an explicit tiled pass graph and VBO contract. Two fixed 128x64 PSMCT32 targets consume 64 KiB total instead of reserving two screen-sized surfaces. Each triangle is partitioned against the active scissor, reconstructed per tile, and composited with its original screen-space Z so overlapping batches keep primitive ownership;
- **CURRENT IMPLEMENTATION:** fogged alpha-bearing `TRILERP/MODULATEIA2` applies the RDP fog equation once, on the final screen-space composite. Intermediate color/alpha reconstruction remains unfogged, preventing every GS pass from accumulating the same fog contribution while preserving the original per-vertex fog plane;
- **CURRENT IMPLEMENTATION:** project-owned GS register shadowing suppresses unchanged material writes by comparing the final 64-bit register values, not high-level setter calls. `TEST`, `ZBUF`, `FRAME`, `FBA`, `PABE`, `ALPHA`, `FOGCOL`, `CLAMP`, `TEXA`, `SCISSOR`, `TEX0`, `TEX1` and `PRIM` survive ordered PATH3 submissions and are emitted only after a real value transition. Temporary clear/channel-shuffle packets either restore and recommit their terminal state or invalidate the affected slots; native presentation invalidates `FRAME/SCISSOR` because PCRTC switches the draw buffer outside the ordinary core emitter;
- **CURRENT IMPLEMENTATION:** logical RGB and alpha write enables now produce independent `FRAME.FBMSK` lanes. The previous color-disabled case accidentally masked all 32 framebuffer bits even when a graph requested alpha-only output; the host-tested mask contract now emits `0x00ffffff` for CT32 alpha-only writes and exposes individual R/G/B masks for channel reconstruction;
- **CURRENT IMPLEMENTATION:** `G_CC_BLENDIA` and `G_CC_CUSTOM_27` share an exact `lerp(INPUT2,INPUT1,TEXEL0)` graph. TEXEL0 is captured once at half scale so its bytes are native GS `ALPHA` factors; each RGB channel is assembled with `(Cs-Cd)*Ad+Cd`, while BLENDIA alpha samples `TEXEL0.a*INPUT1.a` and CUSTOM_27 alpha performs the same texel-factor lerp in a scalar lane. IA/I and upload-proven grayscale RGBA textures collapse RGB to one channel pass; genuinely coloured RGBA retains three masked channel passes. Shuffle work is bounded to the tile's dirty rectangle;
- **CURRENT IMPLEMENTATION:** the first-person shield's `CUSTOM_00 -> CUSTOM_01` equation reuses the same TEXEL0-factor graph without another render target. Its dependent cycles reduce algebraically to `RGB=lerp(0,ENV,TEXEL0)` and `A=lerp(PRIM,PRIM+ENV*(1-PRIM),TEXEL0)`. The vertex translator prepares those distinct channel endpoints, the graph preserves the texture's independent RGBA factors, and the sole final composite retains the original cloud-surface blend, depth and fog ownership. This new topology remains marked for physical-hardware validation;
- **CURRENT IMPLEMENTATION:** the skydome's `RGB=lerp(ENV,SHADE,TEXEL0)` material also reuses the channel-wise factor graph. Its alpha lane is now selected explicitly as either independent interpolated `SHADE.a` or native opaque coverage, instead of incorrectly inheriting sampled TEXEL0 alpha. This covers both Fast3D option variants without another target or an additional texture capture, and remains marked for physical-hardware validation;
- **CURRENT IMPLEMENTATION:** the main font blend `RGB=lerp(PRIM,ENV,TEXEL1.a)`, `A=TEXEL0.a*ENV.a` has an explicit two-texture factor graph. TEXEL1 alpha is captured once and drives all RGB lanes in one masked blend because its factor is scalar; TEXEL0 then writes glyph alpha directly into the same CT32 result. The graph reuses the existing two 128x64 targets, adds no VRAM allocation or EE readback, and keeps the sole final composite responsible for depth, fog and framebuffer blending. This topology remains marked for physical-hardware validation;
- **CURRENT IMPLEMENTATION:** `G_CC_INTERFERENCE -> G_CC_MODULATEIA2` reconstructs `TEXEL0*TEXEL1*SHADE` without another VRAM target. The color target first receives `TEXEL1*SHADE`; TEXEL0 RGB is captured as GS alpha factors and multiplies destination RGB through `(Cd-0)*Ad`. Partial `TEXEL1.a*SHADE.a` is preserved in the scalar target while color-target alpha is scratch, then multiplied by `TEXEL0.a` through `(Cd-0)*As` and copied back before the single final depth/blend/fog composite. The actual IA8 TEXEL0 path is format-proven monochrome and therefore uses one RGB factor pass;
- **CURRENT IMPLEMENTATION:** independent-color texture-alpha recipes used by `CUSTOM_02`, `CUSTOM_04` and `SHADEDECALA` no longer receive a wrong direct `MODULATE` approximation or get dropped. One tiled CT32 target captures `TEXEL0.a` while `FRAME.FBMSK` blocks texture RGB, then receives the independent `INPUT1` RGB while alpha is masked. A single screen-space composite owns the original Z, blend, fog and alpha/texture-edge test. The graph reuses the existing 128x64 target and performs no EE readback; its new channel sequence remains marked for physical-hardware validation;
- **CURRENT IMPLEMENTATION:** `TRILERP -> CUSTOM_08` and the equivalent `CUSTOM_15` topology reuse the one-target trilerp graph when final RGB is `COMBINED*INPUT2` but alpha is an independent `INPUT1`. The independent alpha is stored with the TEXEL0 base, TEXEL1 interpolation masks alpha writes, and the final composite applies fog/blend/Z once. This covers fog replacements whose RGB and alpha inputs intentionally name different RDP sources while keeping their shared compact VBO indices;
- the graph first captures `TEXEL0.a * SHADE.a` and `TEXEL1.a * SHADE.a`, exposes each high byte through the PSMT8H identity view, and interpolates the scalar result by vertex LOD into the red lane of the scalar target. It separately reconstructs RGB as the existing opaque two-pass trilerp, copies scalar red into result alpha, then samples the completed RGBA target once through the caller's alpha/depth state;
- workspace sampling converts integer-translated screen coordinates to normalized GS `STQ` by the fixed 128x64 target extent. A host test locks origin, center and far-edge mappings so the diagnostic path cannot silently confuse texel-space `UV` with normalized `STQ`;
- **CURRENT IMPLEMENTATION:** ordinary builds classify the hardware-validated alpha-bearing `TRILERP/MODULATEIA2` graph as supported. Its gameplay availability no longer depends on a diagnostic compile definition;
- **CURRENT IMPLEMENTATION:** the opt-in bootstrap scene binds two deterministic RGBA8 inputs plus a CPU-precomputed reference. The diagnostic binary starts directly on the test, with the tiled GS graph on the left and the reference on the right; `Select` toggles back to the baseline cube. The reference models GS `>>7` modulation, vertex LOD and the adapter's 0..0x80 alpha convention, making channel swaps, clamp errors and gross quantization drift visible without reaching a game-level material;
- CI retains `PD_PS2_ALPHA_TRILERP_DIAGNOSTIC=ON` as a scene-selection option and publishes `pd-ps2-alpha-trilerp-diag.elf` beside the ordinary gameplay-enabled `pd-ps2-bootstrap.elf`;
- **REAL HARDWARE, VALIDATION FAILURE:** commit `0ab8239e` reached and displayed both panels on a physical console, proving that the pass graph submits and completes, but the live GS result did not match the CPU reference. The oversized two-triangle panels also generated enough repeated channel-shuffle work per frame to look stalled and could miss short controller presses. The follow-up diagnostic isolates one aligned 128x64 tile and one triangle, with a visible frame-loop heartbeat, so channel-shuffle correctness is tested independently of multi-tile composition and shared-edge rasterisation;
- **REAL HARDWARE, PAD PASS:** commit `6680f684` displayed a cyan PAD status bar on a physical console and raw `Select` reliably toggled between the isolated A/B scene and the baseline cube. This confirms launcher-resident `sio2man`/`padman` discovery, `PAD_STATE_STABLE`/`PAD_STATE_FINDCTP1` readiness, `padRead`, edge detection and game-action mapping independently of GS completion. The diagnostic keeps the PAD ladder available for future launcher regressions: red=backend unavailable, orange=RPC initialised, yellow=port open, blue=transport ready, cyan=`padRead` succeeded, magenta=raw button held and white=raw `Select` held;
- **REAL HARDWARE, VALIDATION PASS:** CI #193 tested the corrected CT32-red to PSMT8 transform on a physical PS2. The live GS panel displayed successfully, the PAD bar reached cyan and raw `Select` returned to the rotating cube. This promotes the graph into normal renderer coverage while retaining the diagnostic binary for regression tests;
- **EMULATOR COMPATIBILITY REPORT:** the same development line displays on physical PS2 but no longer starts in PCSX2. Treat this as a separate launcher/emulator compatibility regression; it does not roll back GS behavior reproduced on hardware;
- **CURRENT IMPLEMENTATION:** Fast3D shader-clamp metadata now drives native GS `REGION_CLAMP` independently on S/T. `MIRROR|CLAMP` reuses the reflected two-period residency and clamps the final coordinate per fragment, including distinct sampler state for both textures in M4 multipass graphs. The physical cache variant depends on mirror residency only, while the edge bound remains per-draw state;
- remaining dependent second-cycle equations remain explicit unsupported recipes, rather than receiving a visual approximation;
- the fixed shader table holds 128 mode/option combinations so real level traversal does not exhaust the original 32-slot bring-up pool;
- gsKit remains only in one-time CRT/screen bootstrap, system framebuffer/Z setup and block-rounded texture-size calculation;
- RGBA32/PSMCT32 remains the compatibility fallback for other formats and any TMEM view whose exactness is not proved;
- the hot frame loop is native in command, texture and presentation transport, but one-time initialization is not yet gsKit-independent.

Remaining renderer milestones include measured GS state batching and combiner coverage, VIF1/VU1 geometry batches, and replacement of the remaining one-time gsKit CRT/system-buffer bootstrap where doing so has a concrete ownership or performance benefit.

## 8. Bottleneck hypothesis ordering

Do not jump straight to VU1 because it is the glamorous bit of silicon.

Current likely sequence:

1. **Correctness:** TMEM/tile/TLUT semantics and unsupported RDP state.
2. **Transport/residency:** texture conversion, duplicate copies and GS VRAM lifetime.
3. **State/combiner:** classify the actual Perfect Dark RDP modes and implement only the required GS mappings/multipass paths first.
4. **Geometry:** profile CPU Fast3D transform/light/clip cost and batch structure.
5. **VU1 migration:** move regular geometry batches only after the input/output representation is stable.
6. **IPU experiments:** add async image jobs where the source representation naturally matches IPU strengths.
7. **Final low-level cleanup:** remove remaining one-time gsKit bootstrap dependencies if profiling or ownership requires it.

After every major optimization, re-profile the whole system because the bottleneck may move to GIF, GS VRAM, Main Bus, SIF or another producer.

## 9. Buffer ownership rules

Double/triple buffering is an ownership protocol, not decoration.

Every asynchronous buffer should have explicit states comparable to:

```text
FREE
 -> PRODUCER_WRITING
 -> READY
 -> DMA_OWNED / DEVICE_OWNED
 -> CONSUMER_DONE
 -> FREE
```

Every major dataset should document:

```yaml
producer:
consumer:
lifetime:
representation:
alignment:
transport:
batch_size:
deadline:
ownership_states:
```

Alignment must distinguish allocator ABI, CPU cache-line considerations, DMA qword requirements, device format requirements and packet alignment. There is no project-wide `ALIGN(64)` rule.

## 10. Synchronization rules

Default scheduling policy:

```text
submit early
perform independent work
wait at the latest proven dependency
```

Do not add global `wait_for_everything`, broad FLUSH/FINISH barriers or synchronous RPC for convenience.

Do not remove a barrier merely because PCSX2 appears happy. Race/timing claims must be confirmed on real hardware.

## 11. Benchmark protocol

For renderer/IPU/VU experiments report more than average FPS.

Required metadata:

```yaml
console_scp:
hardware_revision:
ps2sdk_commit:
toolchain:
build_flags:
active_irx:
video_mode:
workload:
direction:
alignment:
buffering:
batch_size:
sample_count:
units:
correctness_hash:
```

Required real-time statistics:

```text
p50
p95
p99
max
deadline misses
```

Renderer-specific counters should include where practical:

- texture uploads/frame;
- texture bytes/frame and maximum burst;
- cache hits/misses for translated textures;
- GIF channel wait time;
- frame command QW count;
- GS VRAM resident bytes and churn;
- VIF/VU batch count when introduced;
- XGKICK count/stall evidence when introduced;
- IPU input/output bytes, command count and wait time for IPU experiments.

PCSX2 is useful for correctness, packet/state inspection and fast iteration. It is not the final timing/cache/DMA/FIFO arbiter.

## 12. Immediate implementation roadmap

### M3a: native texture transport cleanup

- direct call from GS core to project-owned IMAGE uploader;
- correct allocation/upload state separation;
- explicit error propagation;
- keep current RGBA32 behavior as the A/B baseline.

### M3b: faithful TMEM frontend

- explicit 4 KiB TMEM runtime;
- documented LoadTile/LoadBlock/TLUT behavior;
- generation/content-based texture identity;
- regression comparison with existing desktop output.

### M3c: GS texture residency

- project-owned 256-byte-block allocator for post-system GS VRAM;
- transactional reupload/resize with fence-delayed retirement;
- exact RGBA16 -> PSMCT16 direct residency, with RGBA32 fallback;
- exact split-bank RGBA32 -> PSMCT32 direct residency from the canonical
  live-TMEM view, without traversing the generic importer;
- exact CI4/CI8 plus RGBA16 TLUT residency as paired texture/CLUT blocks;
- exact CI4/CI8 plus IA16 TLUT residency as compact indexed planes paired
  transactionally with PSMCT32 CSM1 palettes;
- exact IA4/IA8/I4/I8 residency through shared immutable CT32 palettes;
- exact direct IA16-to-PSMCT32 IMAGE staging, including mirror-wrap expansion,
  without the generic RGBA32 importer buffer;
- exact `G_TX_MIRROR | G_TX_WRAP` residency for power-of-two TMEM images by
  materializing a reflected two-period CT32/CT16/T8/T4 image directly in IMAGE
  staging and scaling ST by one half on each expanded axis;
- exact `G_TX_MIRROR | G_TX_CLAMP` sampling on the same reflected residency:
  the PS2 adapter consumes Fast3D's per-draw last-texel-centre metadata and
  programs `REGION_CLAMP` independently for S/T. Bounds are applied by the GS
  per fragment, preserving bilinear edge behavior and each multipass tile's
  distinct sampler state;

### M4: RDP state coverage

- consume `G_SETPRIMDEPTH` in the shared Fast3D interpreter and replace clip Z
  with the RDP's 15-bit primitive depth whenever `G_ZS_PRIM` is active; the GS
  then receives the same constant depth through the normal reversed-Z mapping;
- inventory actual combiner/render/blender recipes, with the model path's `TRILERP`, `CUSTOM_17..26`, `MODULATEI/IA` and `PASS2` families as the first concrete set;
- map supported recipes to GS state, including exact final-cycle and `PASS2` collapse before adding any extra draw;
- reconstruct opaque `TRILERP -> PASS2/MODULATEI2` through the implemented `TEXEL0` base pass plus `TEXEL1` source-alpha interpolation pass;
- reconstruct opaque `CUSTOM_17/19 -> CUSTOM_18` through the implemented solid `ENV` base plus `TEXEL0` source-alpha interpolation pass;
- use the implemented page-aligned CT32 transient-target core as the ownership layer for exact alpha-bearing work;
- use the implemented dirty-tracked `TEXFLUSH`, feedback rejection and region-clamped PSMCT32 view for the render-target-to-texture transition;
- use the implemented PSMT8H plus identity-CLUT view when a pass consumes the high alpha byte of a CT32 target;
- use the implemented page-local PSMT8 red-to-alpha blit to assemble a CT32 color result with the separately reconstructed alpha lane;
- retain the hardware-validated 128x64 tiled `MODULATEIA2` graph in ordinary builds and keep `PD_PS2_ALPHA_TRILERP_DIAGNOSTIC=ON` only as the deterministic A/B scene selector;
- preserve the `pd-ps2-alpha-trilerp-diag.elf` regression screen: left is the live GS graph and right is the CPU reference, `Select` returns to the cube, and the lower bars expose ROM, PAD and heartbeat. Future reports should add console model and video mode to CI commit and PAD-colour metadata;
- retain the existing opaque `CUSTOM_20` collapse, the implemented
  `CUSTOM_21 -> CUSTOM_18` additive texture-edge graph, the signed
  `CUSTOM_22 -> CUSTOM_23` texture-edge graph and the nonlinear
  `CUSTOM_24 -> MODULATEIA2` scalar graph;
- retain the shared channel-wise `BLENDIA`/`CUSTOM_27` graph, including its
  one-channel path for format-proven or upload-proven grayscale textures and
  three-channel fallback for arbitrary RGBA;
- retain the algebraically reduced `CUSTOM_00 -> CUSTOM_01` shield topology
  in the shared TEXEL0-factor graph, including its independently prepared
  RGB and coverage-alpha interpolation endpoints;
- retain the skydome `lerp(ENV,SHADE,TEXEL0)` path in that graph for both
  independent `SHADE.a` and opaque Fast3D variants;
- retain the two-texture font blend which uses `TEXEL1.a` as the shared RGB
  interpolation factor and `TEXEL0.a*ENV.a` as independent glyph coverage;
- retain the one-target `CUSTOM_02`/`CUSTOM_04`/`SHADEDECALA` graph for
  independent `INPUT1` RGB with `TEXEL0`-derived alpha; preserve its single
  final depth/blend/fog/alpha-test composite and hardware-validation marker;
- retain the one-target `TRILERP -> CUSTOM_08`/`CUSTOM_15` topology for
  trilerped, modulated RGB with independent `INPUT1` alpha;
- log unsupported recipes instead of silently approximating them.
- retain explicit rejection for region bounds outside the GS 10-bit
  `CLAMP.MIN/MAX` contract instead of silently truncating them.

### Full-game runtime frontier

- **CURRENT IMPLEMENTATION:** the physical PADMAN/libpad transport now feeds a
  native implementation of the complete portable `input.h` contract. Logical
  players have explicit physical-port ownership, configurable bindings,
  deadzones and axis scales, dual-analog or C-button right-stick modes, and
  deadline-owned DualShock 2 rumble. Keyboard, mouse, clipboard and text-input
  calls are deterministic console no-ops rather than SDL dependencies;
- **CURRENT IMPLEMENTATION:** PS2 `romdata` no longer allocates a resident
  32 MiB ROM image. A file-backed `RomSource` validates the ROM through bounded
  reads, stream-inflates the RZIP data segment through an 8 KiB input window,
  and keeps stable 32-bit ROM offsets as the resource identity. The temporary
  decompressed table producer is released once compact extent/name metadata has
  been materialized;
- permanent ROM segments own exact-sized buffers because existing game/audio
  consumers retain their pointers. Ordinary asset files retain only
  offset/size/name metadata until first use, then own a bounded allocation that
  `romdataFileFree` releases at the existing `fileRemove` lifetime boundary.
  External replacements and ROM fallback use the same explicit ownership
  state, while desktop builds retain their resident-ROM views;
- `pd_ps2_game_bridge` now compiles the real filesystem, ROM table and bounded
  source modules under the EE ABI. The host gate exercises identical memory-
  and file-backed RZIP paths, including truncated ranges and caller-owned
  scratch/output buffers;
- **CURRENT IMPLEMENTATION:** the PS2 `audio.h` backend now owns the complete
  EE-to-IOP streaming boundary. It embeds the current PS2SDK `audsrv.irx` in the
  ELF, loads ROM `LIBSD`, configures the exact supported 22050 Hz stereo PCM16
  path, and submits each mixed game buffer only at `audioEndFrame`. The game
  retains ownership until `audsrv_play_audio` has copied the bytes into its RPC
  buffer. A host-tested planner rejects malformed or service-oversized chunks,
  caps queued latency and waits only when the bounded IOP ring must release
  enough space. There is no duplicate EE audio ring;
- keep `pd_ps2_game_bridge` as the EE ABI frontier while platform services are
  replaced. Native input, ROM streaming and audio output now cross that
  frontier. The next promotion blocker is a measured EE heap/resident-segment
  budget plus the remaining platform/link closure, after which the real
  `port/src/main.c` entry point can replace the diagnostic bootstrap;
- do not promote the game ELF by ignoring unresolved symbols. Every service
  crossing the link frontier must have an explicit PS2 owner and failure
  contract.

### M5: VIF1/VU1 geometry path

- **CURRENT IMPLEMENTATION:** establish CPU baseline counters for EE
  translation time, translated vertices and final transport volume;
- **IN PROGRESS:** define packed VIF-ready vertex batches. The transport
  supports 96 color vertices or 81 textured vertices plus draw-local GS state;
- **CURRENT IMPLEMENTATION:** the next geometry stage has a host-tested raw
  input/output memory contract. Each 256-QW TOPS input bank contains a six-QW
  control/tag header, draw-local A+D state and up to 81 clip/STQ/RGBA vertices.
  Its disjoint output bank at QW 512 or 768 has room for the corresponding
  multi-tag GIF PACKED stream, including optional post-draw state restoration;
- VU1 transform/light/texgen/fog candidate;
- **IN PROGRESS:** direct GS-ready output and XGKICK. The transport diagnostic
  executes this route, but initially passes through already packed A+D records;
- **IN PROGRESS:** double-buffer VU input/output ownership. BASE/OFFSET banks
  and alternating EE slots exist, while execution remains serialized for the
  first hardware correctness test;
- real-hardware A/B against CPU path.

### M6: IPU image jobs

Start with isolated experiments, not a renderer-wide dependency:

1. large image / intra-compressed texture -> IPU -> RGB16 -> native GIF -> GS;
2. RGB32 -> RGB16 `PACK` A/B against EE conversion;
3. RGB32 -> INDX4 with offline codebook A/B against CPU/MMI and direct preconverted assets;
4. only retain a path if end-to-end frame/streaming cost improves.

The goal is not maximum utilization percentage of every chip. The goal is less unhideable work on the critical path.
