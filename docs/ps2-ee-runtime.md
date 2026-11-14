# EE runtime library replacements (round 17)

The question was whether the port spends real time in general-purpose
libraries that a PS2-specific implementation could beat. An EE sampling
profile of the intro, attributed per library, answered it:

| Library code | Share of EE samples |
|---|---|
| libgcc soft-double (fp-bit) | ~1.9 % |
| libkernel `memcpy` / `memset` | ~1.5 % |
| newlib trig (`sinf`, `cosf`, `atan2f`, ...) | ~0.4 % |
| zlib, other libc | < 0.5 % |
| gsKit / dmaKit | ~0 (already replaced by the native queue) |

The largest "general library" left is Fast3D itself, the generic N64
display-list translator, at roughly a third of EE time. It is not a drop-in
replacement and is discussed at the end.

## Soft-double: `port/ps2/rt/ee_softdouble.c`

The EE FPU has no double precision. Rare's code uses doubles on purpose in a
few places (object collision hulls in `objCalculateGeoBlockVertices`,
`cdBlockExcludesBlockLaterally`, ground height). Those coordinates are large
enough that single precision could change collision results, so the doubles
stay. What changed is the library behind them.

libgcc on this toolchain uses the portable fp-bit code: unpack both operands
into a struct, operate, pack. The replacement works on the IEEE bit patterns
in the EE's 64-bit registers:
* 32 x 32 multiplies build the 106-bit product.
* `PLZCW` replaces the missing `CLZ`.
* One shared round-and-pack does round to nearest even, subnormals and
  overflow.

It defines every function of each libgcc object it replaces, so fp-bit is no
longer linked at all.

Cycles per operation in PCSX2 (COP0 count):

| Operation | fp-bit | new |
|---|---|---|
| add | 210 | 106 |
| mul | 271 | 146 |
| div | 671 | 474 |
| compare | 122 | 36 |
| float → double | 118 | 30 |
| double → float | 120 | 64 |

Correctness:
* `ee_softdouble_test.c` (in CI) checks bit-exact results against the
  host's IEEE hardware on edge values and 6 million random operand pairs.
  It covers raw bits, cancellation, subnormals and game-range magnitudes.
* The same test was run on the EE against fp-bit. The only difference is a
  subnormal product that fp-bit rounds wrongly; the host hardware agrees
  with the new code.

In the intro profile, soft-double time fell from about 1.9 % to 0.18 % of
samples.

## `memcpy` / `memset`: `port/ps2/rt/ee_string.c`

libkernel's versions use LQ/SQ only when every pointer is 16-byte aligned.
Otherwise they copy or fill one byte per iteration. The replacements:
* Align the destination with one unaligned doubleword (`SDL`/`SDR`) plus
  bytes.
* Move 32 bytes per iteration with SQ.
* Read a misaligned source with `LDL`/`LDR` pairs joined by `PCPYLD`.
* Copy short tails with one unaligned doubleword.

Cycles in PCSX2, libkernel → new:

| Size, alignment | memcpy | memset |
|---|---|---|
| 12 B, any | 125 → 64 | 75-118 → 57 |
| 256 B, aligned | 144 → 118 | 108 → 101 |
| 256 B, dst +8 | 2324 → 142 | 2070 → 115 |
| 2048 B, aligned | 984 → 734 | 612 → 549 |
| 2048 B, dst +1 src +3 | 18452 → 1707 | 16406 → 606 |

The profile also showed where most `memset` calls came from. The Fast3D
frontend clears short per-vertex tag arrays on every G_VTX with a store
loop, and GCC's loop-idiom pass had turned each loop back into a `memset`
call. The generated frontend is now compiled with
`-fno-tree-loop-distribute-patterns`, so those stay a few inline stores.
`memcpy` + `memset` fell from about 1.5 % to 0.44 % of samples.

## Math flags

`-fno-math-errno` lets `sqrtf` and similar calls compile to their R5900
instruction instead of a call. Nothing in the game reads `errno` after a
math call. Trig (`sinf`/`cosf`, 0.3 %) stays on newlib: a faster polynomial
would change results slightly everywhere, for little gain.

## Result

In the intro, frame time is 0.1-1.3 ms lower in 7 of 9 five-second windows
and level in the other two. EE idle time (waiting on GS/vsync) rose from
5.7 % to 11.6 % of samples, and the native queue's wait for the GS fell from
9.3 % to 4.5 %.

## Next layer: Fast3D

What remains on the EE is mostly the generic display-list translator:
* `gfx_run_dl`, `gfx_sp_tri1`, `gfx_sp_vertex` and object deferral: ~25 %
* TMEM emulation: ~6 %
* Triangle setup: ~2 %

It re-interprets every display list every frame. That includes level
geometry, which is static per room. A PD-specific path that compiles each
room display list once into VU1-ready batches would replace that
interpretation for most of the scene.
