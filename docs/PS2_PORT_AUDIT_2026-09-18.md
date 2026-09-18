# Perfect DarkStation 2 port audit, 2026-09-18

This is a dated repository and implementation snapshot. It is not the
canonical current frontier. Use [PS2_DEVELOPMENT.md](PS2_DEVELOPMENT.md) for
ongoing work and update this file only to correct the audit record itself.

The review followed the routing in `PS2 Optimization Research Library v2`:
manifest first, then the Performance Bible, GS, VU, VIF, whole-system, DOD and
PS2SDK corpora. Claims below use the repository's epistemic labels.

## Repository and branch result

At audited commit `62bdecfa5f17`:

| Branch | Relationship to `ps2` | Role |
| --- | --- | --- |
| `ps2` | active/default | Perfect DarkStation 2 development |
| `port` | 0 commits ahead, 305 behind | inherited portable/upstream baseline |
| `master` | 0 commits ahead, 1454 behind | older decompilation lineage |
| `port-debugger` | 79 ahead, 438 behind | retained historical topic line |
| `port-net` | 202 ahead, 437 behind | retained historical topic line |

No branch was deleted. GitHub had no open issue or pull request at audit time.
The default branch was already `ps2`. A classic protection rule now prevents
force-push and deletion of `ps2` without requiring PRs, reviews or status
checks for ordinary updates.

## Dataflow and ownership findings

| Area | Finding | Evidence class |
| --- | --- | --- |
| PATH3 commands | Two fixed-size UCAB command arenas alternate ownership. Arena spills submit in order and continue in the other arena without a GS `FINISH`. | **CURRENT IMPLEMENTATION** |
| Texture upload | Two persistent upload slots stage conversion before claiming GIF DMA. The next GIF owner provides ordering; upload submission does not immediately wait for GS completion. | **CURRENT IMPLEMENTATION** |
| PATH1/VU1 | Two EE staging slots and two VU1 TOPS banks exist. Pending ownership is guarded by bounded DMAC/VIF polling and error telemetry. | **CURRENT IMPLEMENTATION** |
| VIF barriers | The active chain deliberately retains `FLUSHA -> MSCAL -> FLUSH` validation ordering. This is correctness scaffolding, not proof of optimal overlap. | **CURRENT IMPLEMENTATION** |
| GS VRAM | A fixed-metadata allocator owns post-system VRAM. Texture, CLUT and four transient render-target slots use explicit retirement records and a GS dependency fence before reuse. | **CURRENT IMPLEMENTATION** |
| Game arenas | Vtx/Mtx/colour allocations are checked against the active frame arena. Master display-list use is checked at phase boundaries, but individual writers do not reserve their worst-case Gfx command count before writing. | **CURRENT IMPLEMENTATION** |
| TMEM | The PS2 frontend maintains a checked 4096-byte live TMEM model and eight tile descriptors; unsupported/unproved layouts retain explicit compatibility fallbacks. | **CURRENT IMPLEMENTATION** |
| Combiner coverage | Unsupported recipes are counted, traced and dropped instead of being silently approximated. This preserves diagnosis but still produces missing geometry/materials. | **CURRENT IMPLEMENTATION** |

## Cost and performance findings

### CONFIRMED

- Current gsKit still places broad waits and GS `FINISH` handling in its normal
  queue path. The project-owned hot submission path intentionally bypasses
  those helpers and uses explicit ownership points. Reviewed gsKit commit:
  [`8ef73d05f022`](https://github.com/ps2dev/gsKit/commit/8ef73d05f022d14b3ded7c70cba670fe333ed61a).
- The current PS2SDK `audsrv` interface exposes separate availability/queued,
  wait and play calls. The port's bounded audio planner therefore remains a
  service-layer integration, not a zero-cost local ring. Reviewed PS2SDK
  commit: [`d317f8f0a2a4`](https://github.com/ps2dev/ps2sdk/commit/d317f8f0a2a413db38c5ef2b46ba927996983e0a).
- The Optimization Library PS2SDK snapshot is
  `a13b5971ec0e39c7ba8b8559b80a4e81c8425352`. Current master differs, but the
  inspected packet2, audsrv, kernel/libcglue and relevant common interfaces do
  not contradict the contracts used by this port. The library provenance
  should eventually be refreshed to the newer SHA rather than silently calling
  the older snapshot current.

### CURRENT IMPLEMENTATION

- No general allocator is used for ordinary PATH1/PATH3 command submission.
  Persistent queues allocate during initialization. Texture upload staging may
  grow on first demand, so a new largest texture can still allocate and free in
  a rendering/loading path.
- The trace recorder allocates about 1.4 MiB when armed, timestamps every event,
  copies submitted PATH1/PATH3 qwords, and performs a synchronous file write at
  the end of the selected frame. It is a correctness diagnostic, not a frame
  performance benchmark. Ordinary unarmed frames avoid those buffers and file
  writes.
- Eligible textured batches currently prepare both GS-ready PATH3 fallback
  vertices and raw VU1 transform input before submission. A successful PATH1
  draw therefore does not yet remove all EE transform/repack work.
- Texture conversion is fused into upload staging, but uploads still copy into
  an EE-owned DMA slot. Static content is not yet generally stored as final
  offline GS-ready upload blobs.
- The state shadow suppresses many redundant GS register writes. Material pass
  graphs and framebuffer effects can still multiply draw and pixel cost, and
  the exact cost is not inferable from source review alone.

### INFERENCE

- The current few-FPS frontier is unlikely to have one universal cause. The
  strongest source-level candidates are duplicated fallback preparation,
  texture conversion/upload churn, exact multipass material graphs, deliberate
  VIF serialization, unsupported-state fallout and synchronous dependency
  fences required by current residency lifetime.
- Removing only compiler overhead is unlikely to close the gap while the same
  scene performs duplicated representation work and multiple GS passes.

### HYPOTHESES TO TEST

1. A trace-ranked combiner inventory will remove more wasted/missing work than
   a global optimization-flag change.
2. Prevalidating VU1 eligibility before PATH3 vertex preparation will reduce EE
   translation time for successful PATH1 batches without weakening fallback.
3. Texture residency telemetry will show a small set of repeatedly converted
   or uploaded textures suitable for consumer-near cached/offline formats.
4. After correctness is stable, narrowing the deliberate VIF barriers may
   improve overlap, but only a real-PS2 PATH1/PATH3 A/B can prove safety.

## Safety and correctness blockers

- Add producer-side command reservation to display-list writers or a protected
  trailing region before treating phase-boundary checks as complete safety.
- Keep unsupported combiner recipes explicit until trace evidence ranks their
  frequency and visible effect.
- Complete framebuffer-copy/effect ownership and VRAM budgeting before
  approximating destination-colour behavior.
- Validate PATH1/PATH3 handoffs under long mixed workloads on retail hardware.
- Preserve the existing error and fallback paths while reducing duplicate
  work. A faster dropped material is still a dropped material.

## Recommended next milestone

Capture and decode one normal `Og` Carrington Institute frame from the current
`ps2` head, then produce a deterministic report grouped by:

- unsupported combiner recipe and triangle count;
- PATH1/PATH3 batch and vertex count;
- texture source hash, format, upload count and resident VRAM identity;
- pass-graph draw count;
- command-arena and VIF wait telemetry.

That single evidence set distinguishes correctness gaps from duplicate work and
chooses the next code change without pretending that PCSX2 timing is retail
hardware timing.
