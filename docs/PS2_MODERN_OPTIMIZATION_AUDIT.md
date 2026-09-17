# PS2 modern optimization audit

Date: 2026-09-17

This audit asks which modern low-level techniques can accelerate the PS2 port
without weakening the renderer correctness contract.  It uses the current
R5900, fast-math, data-oriented, DMAC and GS research corpora together with the
actual game and renderer hot paths.

## Already present

The port is not starting from an unoptimized desktop abstraction:

- Perfect Dark supplies its own single-precision `sinf` polynomial and
  `cosf` wrapper in `modelasm_c.c`.
- `acosf`, `asinf` and `atan2f` already use the original game's bounded
  fixed-point/table-driven contracts instead of general-purpose libm.
- the ordinary PS2 build enables batched VU1/PATH1 perspective transform for
  eligible textured geometry;
- GIF state shadowing suppresses redundant GS register writes;
- native CI4/CI8 and intensity residency reduces texture payload and VRAM
  footprint;
- frame arenas, static translation buffers and the GS packet queue avoid
  per-triangle heap allocation;
- PATH1 submission is double-buffered and waits are deferred until ownership
  requires them.

These are the same broad ideas used by modern N64 optimization projects:
specialize the numerical contract, remove duplicated work, batch the data and
use the machine's coprocessors instead of treating it like a small PC.

## Confirmed missed optimization fixed here

### Hardware `sqrtf`

The original N64 source defines `sqrtf` as one `SQRT.S` instruction in
`src/lib/ultra/gu/sqrtf.s`.  The PS2 build compiles the portable C source set
but does not assemble that N64 file, allowing Newlib to satisfy 263 source
calls.  Current Newlib's full `sqrtf` contract can reach a bit-by-bit software
square-root routine when the compiler leaves a real call.

The PS2 runtime now supplies `sqrtf` with one R5900 COP1 `sqrt.s`.  This
restores the numerical model expected by the original game without enabling
global unsafe-math transformations.  CI disassembles the final ELF and fails
unless the symbol contains `sqrt.s`.

## Highest-value next candidates

### 1. Stop doing the transform twice for VU1 draws

The current direct textured path prepares complete EE/PATH3 fallback vertices:
reciprocal W, NDC, viewport mapping, depth mapping and packed STQ/XYZ.  It then
sends the original clip-space values to VU1, which performs those operations
again.  The fallback preserves correctness if VIF/PATH1 submission fails, but
it makes every successful VU1 batch pay most of the EE cost too.

The safe redesign is a two-stage submit:

1. prevalidate the batch and attempt VU1/PATH1;
2. only if submission fails, run the existing EE translator and submit PATH3.

This keeps the recovery path exact while removing one scalar divide and the
screen/depth packing work per successfully offloaded vertex.

### 2. Paired sine/cosine API for matrix construction

There are many adjacent `sinf(angle)` and `cosf(angle)` calls.  Current `cosf`
calls `sinf(angle + pi/2)`, so a pair repeats classification, range reduction
and polynomial work.  A PS2-specific `pd_sincosf` can share one reduction and
evaluate sine/cosine polynomials together.

This must be introduced at explicit matrix/camera call sites and tested against
the existing game functions.  Replacing the global symbols would silently
change the game's established approximation and is not acceptable.

### 3. Reciprocal-square-root contracts for normalization

Vector normalization usually needs `1 / sqrt(length2)`, not a rounded square
root.  R5900 has `RSQRT.S`; PS2SDK also demonstrates VU0 `VRSQRT` for this
case.  Dedicated nonzero-finite normalization helpers are good candidates for
camera, lighting and model vectors after call-site preconditions are proven.

Do not rewrite collision distances that actually consume the length or depend
on zero-vector behaviour.

### 4. VIF-packed vertex input and larger VU1 batches

The present transform payload sends three 128-bit quadwords per vertex and the
EE expands all attributes first.  A later asset/runtime boundary can keep
colour and fixed-point texture data packed, use VIF UNPACK conversion and let
VU1 consume an AoSoA batch.  This reduces EE stores, RDRAM traffic and VIF
payload together.  It is a larger ABI change and belongs after visual
correctness is stable.

### 5. MMI conversion kernels

Texture conversion and audio mixing contain regular 8/16-bit packing work.
R5900 MMI can process multiple texels or samples per instruction.  These
kernels should be selected by measured byte volume, kept out of scalar control
code and compared against compiler output.  CI byte-equivalence tests already
provide a suitable correctness oracle for texture conversion.

### 6. Scratchpad staging only for proven streaming kernels

The 16 KiB scratchpad can isolate translation or conversion blocks from the
8 KiB D-cache.  It is not a general heap.  A useful experiment needs a large,
regular block, double-buffered DMA and enough independent work to hide the
transfer.  Copying small draw packets synchronously would be slower.

## Compiler experiments

Run these as separate hardware A/B profiles, never as unlabelled defaults:

- `-O2` versus the current `-Og` correctness build;
- per-file `-fno-math-errno` for a proven finite/nonnegative math kernel;
- `-fsingle-precision-constant` after assembly and image comparison;
- selective `-freciprocal-math` only inside an explicit approximate kernel;
- function ordering/hot-section layout once hardware counters or stable frame
  phase timings identify an I-cache problem.

Global `-O3`, global `-ffast-math` and broad LTO are not modernization by
themselves.  On a 16 KiB I-cache they can grow the working set, and unsafe
reassociation can alter clipping, depth and alpha edge decisions.

## Measurement order

1. Confirm the current correctness build on hardware.
2. Compare Og and O2 using the same scene and ROM.
3. Record frame runtime, renderer translation microseconds, PATH1/PATH3 vertex
   counts and VU1 wait time.
4. Optimize the dominant measured phase.
5. Keep numerical and byte-equivalence tests beside every specialized kernel.

The immediate rule is simple: remove duplicated work before making the same
work less accurate.
