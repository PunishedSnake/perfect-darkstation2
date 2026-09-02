# Perfect DarkStation 2 prototype test

This is a bring-up prototype, not a playable game build yet.

## What this prototype proves

The current prototype exercises one bounded producer-to-consumer path on the EE:

1. resolve a user-supplied Perfect Dark NTSC-final `.z64` ROM,
2. validate the ROM header without retaining the whole 32 MiB image,
3. read the RZIP 1173 data stream in 8 KiB chunks,
4. inflate directly into the final data-segment allocation,
5. sanity-check the first file-table extents,
6. initialise GIF/GS through current gsKit/dmaKit,
7. present a deterministic diagnostic frame.

No copyrighted ROM or extracted game assets are included in the repository or CI artifacts.

## Files

Put these two files in the same directory when testing from a filesystem-backed launcher:

- `pd-ps2-bootstrap.elf`
- `pd.ntsc-final.z64`

The ROM must be a legally obtained NTSC-final / v1.1 z64 image matching the current port baseline.

The bootstrap derives the ROM path from the launched ELF path. A different path can be supplied explicitly:

```text
--rom-file <device:path/to/pd.ntsc-final.z64>
```

## Expected visible result

The diagnostic frame contains three horizontal status bars and a Gouraud triangle.

- first bar green: EE/system bootstrap reached the renderer,
- second bar blue: GIF/GS diagnostic path is active,
- third bar green: ROM header, bounded RZIP streaming and file-table sanity all passed,
- third bar red: ROM data path failed.

The console/file log additionally reports the decompressed data-segment size, exact compressed bytes consumed by zlib, first file extent and an FNV-1a hash of the decompressed data segment.

## Active bring-up logger

Bring-up logging is enabled by default. The prototype mirrors human-readable logs to stdout/stderr and to `pdps2.log` beside the ELF when a writable filesystem sink is available.

Current PS2SDK does not implement `fsync()`. On filesystem-backed launchers such as `mass:`, flushing a still-open stdio stream is therefore not a sufficient durability contract for an ongoing logger. The PS2 backend uses coarse durable checkpoints that:

1. flush the static 8 KiB staging buffer,
2. close the log file so the filesystem can publish file-size/directory metadata,
3. reopen it in append mode for subsequent entries.

Every checkpoint still flushes immediately, but ordinary close/reopen cycles are
limited to at most 10 Hz. Real-hardware `mass:` logs stopped after a deterministic
sequence containing many dense reopen operations. Explicit
controller-triggered renderer snapshots and fatal errors bypass the throttle so
their final counters receive a durable close. Checkpoints must not be moved into
frame or other hot paths. `--no-log` disables the file sink for timing-sensitive
experiments while console output remains available.

## Real-hardware observations

### 2026-08-25: first visual bring-up PASS

A real PlayStation 2 produced the expected diagnostic frame with:

- first status bar green,
- second status bar blue,
- third status bar green,
- visible Gouraud triangle and white marker.

This is direct hardware evidence that the current EE/system bootstrap, bounded ROM source, NTSC-final header validation, streamed RZIP 1173 data-segment decode, file-table sanity check, GIF path and GS diagnostic renderer all reached their expected state in one execution.

The first logger implementation created `pdps2.log` but left the file at 0 bytes while the diagnostic remained in its infinite GS loop. This reproduced the known PS2SDK/filesystem issue where an ongoing file may not publish its final size while it remains open and `fsync()` is unavailable. Explicit close/reopen checkpoints fixed early durability, but later hardware logs repeatedly stopped after checkpointing Fast3D scene entry. The throttled policy above preserves early diagnostics while reserving reliable reopen progress for runtime measurements.

Metadata not recorded for this first run and therefore intentionally not inferred:

- SCPH model / hardware revision,
- exact launcher and device stack,
- active IRX set,
- measured ROM-probe timing distribution.

## Real-hardware record

For every timing or stability result, record at least:

- console SCPH model and hardware revision if known,
- launch method and device (`host:`, `mass:`, HDD, etc.),
- git commit,
- PS2SDK / PS2DEV image or commit where known,
- active IRX modules where known,
- ROM path/device,
- whether all three bars reached the expected state,
- relevant console/file log,
- repeated-run count,
- p50 / p95 / p99 / max for measured loading work once timing collection is added,
- any failure or deadline miss count,
- correctness hash printed by the prototype.

PCSX2 is useful for correctness and inspection, but timing, cache, DMA, FIFO and device-performance claims must be confirmed on a real PlayStation 2.

## Current architecture boundary

The real `romdata` path uses the same bounded `RomSource` contract as this
probe: PS2 keeps the ROM file-backed, inflates through a caller-owned 8 KiB
window, materializes permanent segments at exact size and loads ordinary files
on demand. Native audio, input and video now have PS2 owners. Build
`aa007d1b72cc` confirmed the complete 16 MiB game arena with the 4 MiB reserve
intact. `pd_ps2_game` now links `port/src/main.c`, all game/core objects and the
same hardware-tested VU1/GS backend. Its forced startup checkpoints make the
first full-runtime hardware attempt diagnosable even if it stops before the
first rendered game frame. CI run 33643145244 linked that ELF with zero
undefined symbols; its text, data and BSS total 3,528,478 bytes.
