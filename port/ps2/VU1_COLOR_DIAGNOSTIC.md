# VU1 PATH1 color diagnostic

This diagnostic is the first executable VIF1/VU1 stage of the native renderer.
It keeps the existing Fast3D translation and final GS register representation,
then routes untextured color-triangle batches through:

```text
EE batch -> DMAC VIF1 -> VIF UNPACK -> VU1 -> XGKICK -> GIF PATH1 -> GS
```

Textured triangles, texture uploads and ordinary GS state remain on the proven
PATH3 baseline. This makes the test sensitive to PATH1/PATH3 ordering without
mixing transport bring-up with a new transform, clip or combiner implementation.

## Build

```sh
cmake -S port/ps2 -B build-ps2-vu1-diag -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PS2SDK/samples/ps2dev.cmake" \
  -DPD_PS2_VU1_COLOR_DIAGNOSTIC=ON
cmake --build build-ps2-vu1-diag -j2
```

Output:

```text
build-ps2-vu1-diag/pd-ps2-vu1-color-diag.elf
```

The ordinary `pd-ps2-bootstrap.elf` remains the PATH3 comparison artifact.

## Current ownership contract

- Two 256-QW VU1 banks are selected with VIF1 `BASE=0`, `OFFSET=256` and
  TOP-relative UNPACK.
- Each bank receives one complete GIF PACKED A+D packet.
- The current 96-vertex translation batch occupies at most 194 QW, including
  its GIF tag and optional PRIM record.
- VU1 reads the active TOP address and sends the packet with XGKICK.
- Two 259-QW UCAB EE source-chain slots alternate between submissions. Each
  holds three DMAC tags plus capacity for one 256-QW VU payload.
- Every later PATH3 claimant waits for a pending validation batch before it may
  change GS state or upload texture data.

The first hardware version deliberately uses `FLUSHA -> MSCAL -> FLUSH` around
each PATH1 packet. This is a correctness barrier, not a final performance
policy. It prevents path arbitration bugs from being confused with malformed
geometry. After the image A/B passes on a retail console, the next revision can
replace it with dependency-scoped scheduling and overlap VIF input for bank B
with VU1 work on bank A.

## Expected boot evidence

The serial log must contain:

```text
GS VU1 queue: PATH1 color transport ready banks=0+256 QW max_vertices=96
GS VU1 queue: validation ordering FLUSHA->MSCAL->FLUSH active
```

If setup fails, the renderer logs that the VU1 validation transport is
unavailable and preserves the PATH3 fallback. A fallback boot is not a passing
VU1 result.

## Physical PS2 A/B

Run both the baseline and VU1 diagnostic with the same ROM, loader, video mode
and controller setup. Record:

1. console model and hardware revision;
2. PS2SDK/toolchain revision from the CI run;
3. loader and launch device;
4. complete serial log through the first presented frame;
5. lossless captures of the same diagnostic frame;
6. whether any colored status triangle is missing, reordered or uses stale GS
   state;
7. whether controller input and frame presentation remain responsive.

The pass condition is pixel-equivalent color geometry and unchanged textured
geometry, with no VIF error, hang or PATH3 fallback. Emulator success is useful
for inspection but is not accepted as hardware validation or performance data.
