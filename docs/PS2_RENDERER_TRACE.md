# PS2 renderer trace

Updated: 2026-09-18

The retail-hardware renderer can capture one diagnostic frame without enabling
the global `pdps2.log` file logger. The capture is intentionally synchronous
and may stall gameplay while it writes to USB, but ordinary frames do not touch
the filesystem.

## Capture

1. Start the normal `pd-ps2-game.elf` build from USB.
2. Reach the main menu.
3. Select **Carrington Institute**. The menu callback arms the recorder.
4. The transition frame is skipped. The following complete scene frame is
   buffered in EE memory and written once as `pdps2-gs-trace.bin` beside the
   running ELF.
5. Wait for USB activity to finish before resetting or removing the device.

Selecting the same item again replaces the previous file with a fresh capture.
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
traffic, draw cost grouped by pass graph, and the final GS shadow. The JSON
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
semantics are unchanged. A second retail-hardware trace is required to measure
the realized frame-time reduction and verify the expected collapse in PATH
handoffs.
