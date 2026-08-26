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
CI4 / CI8      -> GS indexed texture + CLUT candidate
RGBA16         -> exact live-TMEM view -> PSMCT16 active path
IA / I         -> preserve compact semantics where practical
large imagery  -> IPU experiment candidate
runtime RGBA32 -> native GIF IMAGE baseline, then remove copies if measured worthwhile
```

This is not yet a final format table. Actual N64 texture usage in Perfect Dark must be inventoried before choosing permanent GS formats.

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
- texture uploads use project-owned GIF IMAGE source chains with two persistent staging slots;
- upload staging is prepared before claiming the GIF channel, following `submit early, wait late` as far as dependency allows;
- upload chains end with `TEXFLUSH` and do not insert GS `FINISH`;
- draw/state submission serializes on GIF channel ownership, preserving upload -> TEXFLUSH -> dependent draw order;
- a fixed-metadata project allocator owns the post-framebuffer texture region of the 4 MiB GS VRAM and uses 256-byte block alignment;
- texture replacement is transactional: a new residency is uploaded before the old baked `TEX0` address is retired;
- retired blocks are reclaimed only after an explicit native GS `FINISH` fence proves that previous consumers are done;
- normal draw and upload submission does not wait for `FINISH`; fences occur at PCRTC publication and under genuine texture-allocation pressure;
- the PS2 Fast3D cache is capped at the backend's 64 texture handles so eviction can recycle handles instead of exhausting the backend table;
- exact live-TMEM N64 RGBA5551 textures are converted directly into GS A1B5G5R5 staging and reside as PSMCT16, halving their IMAGE payload and local-memory footprint relative to the RGBA32 baseline;
- TEXA expands the PSMCT16 alpha bit to the same 0..255 texture-alpha convention used by the existing combiner adapter;
- the project owns the per-frame VSync wait, `DISPFB2` publication, draw-buffer selection and native `FRAME`/`SCISSOR` packet;
- a host-tested combiner planner reduces exact two-cycle modes to one GS pass when the final channel is independent of `COMBINED` or merely passes the first-cycle result through;
- genuinely dependent second cycles remain explicit unsupported recipes for the forthcoming GS multipass path, rather than receiving a visual approximation;
- the fixed shader table holds 128 mode/option combinations so real level traversal does not exhaust the original 32-slot bring-up pool;
- gsKit remains only in one-time CRT/screen bootstrap, system framebuffer/Z setup and block-rounded texture-size calculation;
- RGBA32/PSMCT32 remains the compatibility fallback for other formats and any TMEM view whose exactness is not proved;
- the hot frame loop is native in command, texture and presentation transport, but one-time initialization is not yet gsKit-independent.

Remaining renderer milestones include native indexed texture residency, measured GS state batching and combiner coverage, VIF1/VU1 geometry batches, and replacement of the remaining one-time gsKit CRT/system-buffer bootstrap where doing so has a concrete ownership or performance benefit.

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
- continue the format inventory before adding CI4/CI8 CLUT residency.

### M4: RDP state coverage

- inventory actual combiner/render/blender recipes, with the model path's `TRILERP`, `CUSTOM_17..26`, `MODULATEI/IA` and `PASS2` families as the first concrete set;
- map supported recipes to GS state, including exact final-cycle and `PASS2` collapse before adding any extra draw;
- use multipass only for recipes that require it;
- log unsupported recipes instead of silently approximating them.

### M5: VIF1/VU1 geometry path

- establish CPU baseline counters;
- define packed VIF-ready vertex batches;
- VU1 transform/light/texgen/fog candidate;
- direct GS-ready output and XGKICK;
- double-buffer VU input/output ownership;
- real-hardware A/B against CPU path.

### M6: IPU image jobs

Start with isolated experiments, not a renderer-wide dependency:

1. large image / intra-compressed texture -> IPU -> RGB16 -> native GIF -> GS;
2. RGB32 -> RGB16 `PACK` A/B against EE conversion;
3. RGB32 -> INDX4 with offline codebook A/B against CPU/MMI and direct preconverted assets;
4. only retain a path if end-to-end frame/streaming cost improves.

The goal is not maximum utilization percentage of every chip. The goal is less unhideable work on the critical path.
