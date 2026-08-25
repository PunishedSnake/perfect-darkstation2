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

## Active logger

Bring-up logging is enabled by default. The bootstrap tries to create:

```text
pdps2.log
```

next to the launched ELF. Console output remains enabled in parallel for loaders such as ps2link.

The text logger records:

- monotonic EE timestamp in microseconds,
- level (`INFO`, `WARN`, `ERROR`),
- source git commit baked into the build,
- compiler version,
- launch arguments,
- resolved ROM path and source size,
- ROM/header/RZIP/file-table milestones,
- compressed and decompressed byte counts,
- data-segment correctness hash,
- total ROM-probe duration,
- GS bring-up milestones and resolved display dimensions.

Normal INFO lines are staged in a fixed 8 KiB buffer and written at coarse checkpoints instead of opening and closing the file for every log line. Warnings and errors force a flush. A fatal error also flushes before exit. This logger is intended for bring-up and correctness diagnostics; the later frame profiler will use a separate preallocated binary trace ring so formatted text does not enter hot paths.

For a timing experiment where even this diagnostic I/O is unwanted, disable the file sink explicitly:

```text
--no-log
```

stdout/stderr remain available.

If the filesystem/device does not permit creation of `pdps2.log`, the bootstrap falls back to console-only logging rather than failing the prototype.

## Expected visible result

The diagnostic frame contains three horizontal status bars and a Gouraud triangle.

- first bar green: EE/system bootstrap reached the renderer,
- second bar blue: GIF/GS diagnostic path is active,
- third bar green: ROM header, bounded RZIP streaming and file-table sanity all passed,
- third bar red: ROM data path failed.

The active log additionally reports the decompressed data-segment size, exact compressed bytes consumed by zlib, first file extent and an FNV-1a hash of the decompressed data segment.

## Real-hardware record

For every timing or stability result, record at least:

- console SCPH model and hardware revision if known,
- launch method and device (`host:`, `mass:`, HDD, etc.),
- git commit,
- PS2SDK / PS2DEV image or commit where known,
- active IRX modules where known,
- ROM path/device,
- whether all three bars reached the expected state,
- `pdps2.log` when the file sink is available,
- repeated-run count,
- p50 / p95 / p99 / max for measured loading work once timing collection is added,
- any failure or deadline miss count,
- correctness hash printed by the prototype.

PCSX2 is useful for correctness and inspection, but timing, cache, DMA, FIFO and device-performance claims must be confirmed on a real PlayStation 2.

## Current architecture boundary

This prototype intentionally does not use VU, VIF, custom DMA, manual R5900 assembly or broad alignment changes. The current bottleneck is still data residency and runtime platform bring-up, so the smallest correct next step is to migrate the real `romdata` path onto the bounded `RomSource` contract before specialising hot kernels.
