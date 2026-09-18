# PS2 renderer trace

The retail-hardware renderer can capture one diagnostic frame without enabling
the global `pdps2.log` file logger. The capture is intentionally synchronous
and may stall gameplay while it writes to USB, but ordinary frames do not touch
the filesystem.

## Capture

1. Start the normal `pd-ps2-game.elf` build from USB.
2. Reach the main menu in Carrington Institute.
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

The console summary is useful for a quick validity check. The JSON preserves
the complete event stream and decoded PATH3 A+D register writes for analysis or
comparison between two hardware captures.
