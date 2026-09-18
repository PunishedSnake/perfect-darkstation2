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
