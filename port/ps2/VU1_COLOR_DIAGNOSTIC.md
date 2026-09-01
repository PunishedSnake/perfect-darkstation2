# VU1 PATH1 geometry diagnostic

This diagnostic connects the native renderer to VIF1/VU1. Ordinary one-pass
textured batches send raw clip-space vertices, STQ and color to a transform
microprogram. Color and multipass draws retain the GS-ready A+D transport.
Both routes use:

```text
EE batch -> DMAC VIF1 -> VIF UNPACK -> VU1 -> XGKICK -> GIF PATH1 -> GS
```

Texture uploads and GS state outside each draw packet remain on the PATH3
baseline. Fast3D matrix transforms, lighting, clipping and combiner selection
still run on EE. VU1 now performs perspective division, viewport/depth mapping,
screen-coordinate saturation, STQ preparation and PACKED vertex emission.
The EE still prepares fallback vertices eagerly. This is correctness bring-up,
not a claim of reduced CPU time or a hardware-validated renderer.

## Build

```sh
cmake -S port/ps2 -B build-ps2-vu1-diag -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/port/ps2/ps2dev-toolchain.cmake" \
  -DPD_PS2_VU1_COLOR_DIAGNOSTIC=ON
cmake --build build-ps2-vu1-diag -j2
```

Output:

```text
build-ps2-vu1-diag/pd-ps2-vu1-color-diag.elf
```

The ordinary `pd-ps2-bootstrap.elf` remains the PATH3 comparison artifact.

## Runtime A/B controls

The VU1 ELF starts on the textured cube with VU1 enabled and fog off.

- `Select`: drain pending PATH1 work and toggle VU1/PATH1 versus EE/PATH3.
  The serial log names the selected mode. A failed switch is logged and does
  not silently change mode. It also checkpoints the current PATH1/PATH3 and
  VIF1-wait counters. The green heartbeat bar means VU1 is selected, not that
  the image has passed validation.
- `Triangle`: toggle depth-varying fog on the same cube, exercising XYZF2 and
  checkpointing the same renderer counters.
- Keep both sticks centered with a connected pad for stationary captures.
  The unattended cube rotates when no pad is connected.
- `Cross`: reset cube position/orientation. Existing stick and rumble tests
  remain available.

Capture all four combinations: VU1/PATH3 with fog off/on. Changing transport
keeps the scene, shader and texture fixed; only the heartbeat status bar
changes deliberately. The PATH3 mode does not increase VU1 submission counts.

## Current ownership contract

- Two 256-QW input banks are selected with VIF1 `BASE=0`, `OFFSET=256` and
  TOP-relative UNPACK. Transform output uses separate banks at QW 512 and 768.
- The raw transform input contains six header QWs, five fixed A+D prefix
  slots, three QWs per vertex and one fixed suffix slot. Unused state slots
  write GS NOP. At 81 vertices the input is 255 QWs and output is 252 QWs.
- Output contains prefix, vertex and suffix GIF tags. The vertex order is
  STQ, RGBAQ, XYZ2/XYZF2 so GIF takes Q from STQ before RGBAQ. Fogged Z uses
  FTOI4 instead of FTOI0: PACKED XYZF2 places Z in bits 4..27. W-lane fog/ADC
  bits are copied with MOVE, never floating arithmetic.
- The A+D passthrough program at instruction 0 supports 96 color or 81
  textured vertices with draw-local state. The raw transform is at address 64.
  The diagnostic Fast3D adapter limits all its batches to 81 vertices.
- Both programs read TOP and submit with XGKICK. Direct core/multipass calls
  without raw clip data retain the A+D route.
- Two 259-QW UCAB EE source-chain slots alternate between submissions. Each
  holds three DMAC tags plus capacity for one 256-QW VU payload.
  The largest transform chain uses 258 QWs of that capacity.
- Every later PATH3 claimant waits for a pending validation batch before it may
  change GS state or upload texture data.

The first hardware version deliberately uses `FLUSHA -> MSCAL -> FLUSH` around
each PATH1 packet. This is a correctness barrier, not a final performance
policy. It prevents path arbitration bugs from being confused with malformed
geometry. After the image A/B passes on a retail console, the next revision can
replace it with dependency-scoped scheduling and overlap VIF input for bank B
with VU1 work on bank A.

Invalid viewport/depth mappings and nonpositive, subnormal or nonfinite W keep
the prepared EE geometry. A rejected transform submission falls back to PATH3.
The standard ELF remains unchanged by this opt-in route.

## Host regression checks

`gs_vu1_transform_test.cpp` checks the payload layout, NOP padding, DMA/VIF
tags, capacity/alignment rejection and viewport mapping. The additional
`gs_vu1_microprogram_test.py` executes the checked-in VSM with input from the
actual C++ payload builder. It checks both banks, 3/81 vertices, fog/no fog,
state padding/restoration, STQ ordering, coordinate saturation, fog/ADC bits
and conservative lane dependencies. It is a limited functional interpreter,
not a VIF/GIF timing emulator or a model of all VU floating-point behavior.

## Expected boot evidence

Before the VU1 program uploads, the log must pass:

```text
GS core: global allocated; configure DMA
GS core: enter screen init D_PCR=00000004 D_STAT=...
GS core: screen init completed
```

The fast-wait mask is deliberately GIF-only. `dmaKit_wait_fast()` checks
completion flags through CPCOND0, not idle CHCR bits. Selecting VIF1 before
its first submission can stall inside `gsKit_init_screen()` after the GIF
setup transfer. VIF1 retains its separate explicit ownership waits.
Program-upload and bank-configuration begin/end checkpoints identify later
initialization stalls without implying that the microprogram has rendered.

The serial log must contain:

```text
GS VU1 queue: PATH1 A+D transport ready banks=0+256 QW max_records=255
GS VU1 queue: textured transform ready entry=64 output=512+768 QW max_vertices=81
GS VU1 queue: validation ordering FLUSHA->MSCAL->FLUSH active
```

If setup fails, the renderer logs that the VU1 validation transport is
unavailable and preserves the PATH3 fallback. A fallback boot is not a passing
VU1 result.

After textured Fast3D draws, `GfxPS2 VU1: transform_batches=...` and
`transform_vertices=...` must increase. These are subsets of the PATH1 totals,
not extra vertices. An increasing `path1_textured` count alone may only prove
A+D passthrough. Counters report accepted submissions, not GPU completion.
The log reports cumulative counters on frame 1 and every 300 frames.

## Physical PS2 A/B

Hardware report for build `998c397cd366` (CI #218): black screen. ROM probing,
bounded RZIP and PAD initialization completed; the last persisted line was
`GS core: gsKit CRT/VRAM bootstrap`. The GIF+VIF1 CPCOND mask was an invalid
bootstrap dependency consistent with this stop point. The GIF-only correction
had a host regression test; its hardware confirmation is recorded below. The
failed build itself did not validate or invalidate VU1 geometry math.

Hardware report for build `145e556d409b` (CI #219): the corrected GIF-only
bootstrap mask reached `GS core: screen init completed`, uploaded both VU1
programs and configured the two TOPS banks. The rotating textured cube was
visible on a physical PS2, and `Triangle` produced a visible fog change. This
confirms live clip/STQ/RGBA input, VU1 perspective/viewport mapping, XYZ2/XYZF2
emission and XGKICK transport for the diagnostic. It does not by itself prove
pixel equivalence with PATH3 or frame-time improvement.

VIF1 waits are now bounded to 100 ms and inspect both DMAC `CHCR.STR` and VIF1
status. A timeout or VIF error disables new VU1 submissions and persists the
register values in the log. The next idle observation releases the pending
slot so the existing PATH3 fallback can resume; an actually wedged/error-latched
VIF remains unavailable rather than being reset behind the renderer's back.

Hardware report for build `d6f729e1270c` (CI #220): the bounded-wait build
again reached GS/VU1 queue readiness and the VU1 diagnostic remained visible
and responsive. The copied mass-storage log ended at Fast3D scene
initialisation, so it contained no first-frame wait counters. Later builds
therefore persist the live renderer counters whenever `Select` or `Triangle`
is pressed, placing the measurement beside an observed A/B transition.

Run both the baseline and VU1 diagnostic with the same ROM, loader, video mode
and controller setup; use runtime toggles for same-ELF comparisons too. Record:

1. console model and hardware revision;
2. PS2SDK/toolchain revision from the CI run;
3. loader and launch device;
4. complete serial log through the first presented frame and a later counter
   sample showing nonzero transform submissions;
5. lossless captures of the same diagnostic frame;
6. whether any colored or textured geometry is missing, reordered, corrupted
   or uses stale GS state;
7. whether controller input and frame presentation remain responsive.

Include a textured scene with nonconstant W and a fog-enabled draw. Inspect
depth intersections and near/far clipping as well as texture perspective.
The VU math order differs from the EE packer, including when XYOFFSET is
added relative to truncation. Subpixel/depth rounding equivalence is not
assumed from host tests; investigate any differing edge pixels before promotion.

The pass condition is pixel-equivalent color and textured geometry, with no VIF
error, hang or PATH3 fallback. Emulator success is useful for inspection but is
not accepted as hardware validation or performance data.
