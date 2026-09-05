# PS2 code and file audit

Audit date: 2026-09-03; startup-chain review updated 2026-09-05. Scope: the
complete repository with emphasis on the `ps2` branch build graph, runtime
startup, renderer frontier, generated inputs and the local uncommitted files
present during the audit.

This audit intentionally distinguishes four different meanings of “unused”:

- not linked into the final PS2 game ELF;
- compiled only as a compatibility gate;
- used only by a host test or generator;
- genuinely incomplete, temporary or recoverable output.

Only the last class is a deletion candidate. No tracked source was removed.

## Inventory and method

The audited tree contains 2,412 tracked files, including 691 C files, 45 C++
files, 390 headers, 2 VU microprogram sources, 11 Python tools and 10 Markdown
documents before this report.

The review used:

1. CMake target and source-list expansion;
2. workflow command and test-source references;
3. include/reference checks for every file under `port/ps2`;
4. startup and first-frame call-chain tracing;
5. exact-content duplicate detection;
6. final-ELF section/symbol inspection;
7. comparison of local modifications with `HEAD`;
8. hardware logs supplied during bring-up.

The previous game ELF was an ELF32 little-endian MIPS N32 R5900 executable with
entry point `0x100880`. Its single loadable image occupied about 3.54 MiB of
text, initialized data and BSS, ending far below the retail EE RAM limit. The
new linker map is retained with every CI ELF so future size and dead-section
claims can be based on the actual link rather than filenames.

## Build graph classification

| Target or input class | Purpose | Linked into `pd-ps2-game.elf` |
| --- | --- | --- |
| `pd_ps2_game` | Actual ROM-backed game runtime and renderer. | Yes. |
| `pd_ps2_game_compile_all` | Compiles the complete `src/game/*.c` frontier to catch EE incompatibilities. | Its object files are supplied to the game link; `--gc-sections` may discard unreferenced sections. |
| `pd_ps2_core_compile_all` | Compiles selected portable libraries and libultra compatibility code. | Its object files are supplied to the game link; section survival is decided by the linker. |
| `pd_ps2_game_bridge` | Fast, narrow compatibility gate used while porting. | No. It is intentionally compile-only. |
| `pd_ps2_bootstrap` | Standalone native GS/PAD/SPU2 diagnostic. | No. It produces a separate ELF. |
| `*_test.c`, `*_test.cpp`, test Python | Backend-independent correctness tests executed on the CI host. | No. They are test programs, not dead runtime code. |
| `.vsm` sources | VU1 microprograms assembled by `dvp-as`. | Yes when the owning renderer configuration needs them. |
| asset JSON and generator scripts | Produce enum/header contracts for game compilation. | Generated headers affect the build; JSON itself is not linked. |
| `src/setups/*.c` | Canonical decompilation source for ROM setup files and rebuild workflows. | No. The current PS2 runtime streams compiled setup payloads from the supplied ROM. |

All C, C++, VSM and Python files under `port/ps2` have an explicit owner in
`port/ps2/CMakeLists.txt` or `.github/workflows/ps2-bootstrap.yml`. The same is
true for PS2 headers through an include from a runtime or test owner. There is
currently no orphan PS2 implementation file that can be safely deleted merely
because it is absent from the final ELF.

## Local files reviewed

These files were present locally but are deliberately excluded from the audit
commit.

| Path/class | Finding | Action |
| --- | --- | --- |
| `src/setups/setupwax.c` | A previous local copy was 971 lines shorter than `HEAD` and ended mid-token at `if_chr_knockedout`. History and conversation review found no intended WAX edit. The file defines the complete Mr. Blonde's Revenge stage setup and maps to `UsetupwaxZ` and `Ump_setupwaxZ`. | Recovered byte-for-byte from the tracked blob on 2026-09-04. Keep it as canonical decompilation source even though the current PS2 runtime streams the compiled setup from ROM. |
| `src/assets/*/tiles/.lee.json.*`, `.lue.json.*`, `.azt.json.*`, `.cave.json.*`, `.pam.json.*` | Large JSON fragments ending mid-record. Their suffixes do not match the generator's `*.json` glob. They look like interrupted atomic-write/editor temporary files. | Safe cleanup candidates after the owner confirms they contain no recoverable edits. Now ignored. |
| `artifacts/` | Downloaded CI ZIP/ELF output, reproducible from GitHub Actions. | Safe to remove locally when no longer needed. Now ignored. |
| `pd.ini`, `eeprom.bin`, `pdps2.log` | Runtime state and diagnostic evidence created beside the ELF. | Keep outside source control. Now ignored by repository-local patterns. |

The ignore rules prevent accidental commits but do not delete any existing
file.

## Duplicate-file findings

Exact duplicates are common in `src/assets/<rom-version>` because each ROM
revision owns an independent extraction contract. Removing one version's copy
would break that version's generator inputs even when the current bytes match.

The following setup-source pairs also have identical content in the audited
revision:

```text
mp_setupsevb.c / setupsevb.c
mp_setupsilo.c / setupsilo.c
mp_setupcat.c  / setupcat.c
mp_setupash.c  / setupash.c
mp_setuplam.c  / setuplam.c
```

They represent different ROM entries and names. They are semantic duplicates,
not safe filesystem duplicates. Do not replace them with symlinks or remove one
without changing the upstream asset build model.

No tracked zero-byte file was found.

## Confirmed issues corrected by this audit

| Area | Previous failure mode | Correction |
| --- | --- | --- |
| ROM file load | Short/failed reads could be reported as success. | Exact read validation and explicit failure propagation. |
| Compressed asset placement | Unsigned subtraction could place compressed input before the allocation or overlap output. | Keep PS2 input in its separate ROM-data buffer and bounded-inflate into the complete destination. |
| RZIP | No source/destination bounds; incomplete/error streams could be accepted. | `rzipInflateSized` enforces both bounds and `Z_STREAM_END`; host boundary tests added. |
| Model load | `modeldefLoad` could dereference a failed/stale file buffer. | Required model failure is fatal with file and destination context. |
| Inflate-size probe | Read a fixed 64-byte header without proving the file was that large. | Validate the minimum RZIP header size before reading. |
| Memory pools | Pool index, alignment and arithmetic overflow could corrupt allocation state. | Validate pool and range arithmetic; fail without advancing the pool. |
| Filesystem | Unsafe path copies plus ignored seek/read/write/close failures. | Bounded paths and exact I/O result handling. |
| Configuration | Unsafe string copies, possible zero-setting dereference and ignored I/O. | Bounded strings, empty-table handling and checked persistence. |
| ROM patching | Runtime patch offsets trusted without segment bounds. | Validate every patch range before writing. |
| Named segments | Unknown names could dereference the sentinel entry. | Return failure for missing names. |
| Startup | Required filesystem, ROM and video initialization return values ignored. | Fail at the owning phase with a durable message. |
| Audio startup | Failed backend could still allow sound heap/output initialization. | Disable game sound automatically for that run. |
| Critical heaps | RDP, sound, MEMA, VI and Gfx/Vtx allocations used without null checks. | Subsystem-specific fail-fast checks. |
| Model runtime | Slot-table, title-arena and model RW-data allocation failures could flow into pointer arithmetic or `modelInitRwData`. | Validate the owning allocations and fail with the exact byte/model context. |
| VI blanking | `osViBlack` opened and presented a nested frame from `schedEndFrame`, resetting the PS2 native command arena after game submission. | Keep persistent blanking state and append the black clear to the one active GS frame. |
| EEPROM | Unterminated path, unchecked partial I/O and address/length overflow. | Bounded path and 2048-byte range enforcement. |
| Fatal exit | Controlled failure returned to OSD and resembled a reset. | Close the durable log and park the EE until manual reset. |
| Desktop CMake | Recursive `port/*.c` globs accidentally included PS2 sources and test `main()` functions. | Exclude `port/ps2` and `port/fast3d/tests` from desktop sources. |
| Link visibility | ELF artifact alone could not prove object/section ownership. | Generate and publish `pd-ps2-game.map` beside the ELF. |

## Files that are intentional but easy to mistake for dead code

- `port/ps2/bootstrap.c` and diagnostic renderer sources form the proven native
  hardware harness. They are valuable for isolating GS, VU1, PAD and SPU2 from
  the full game.
- compile-only targets catch headers, ABI assumptions and unsupported EE code
  before those functions become reachable at runtime.
- host-side allocator, command-budget, combiner, texture, TMEM and VU tests
  protect hardware packet construction without requiring a console.
- `patch_fast3d_tmem_live.py` creates the PS2-specific generated frontend. It
  intentionally fails when upstream Fast3D changes invalidate the exact patch.
- `PROTOTYPE_TEST.md` and `VU1_COLOR_DIAGNOSTIC.md` describe diagnostic ELFs,
  not the normal game path. They remain useful recovery procedures.
- generated headers in `src/generated/<romid>` are build outputs with stable
  include paths. Edit their JSON inputs or generator tools, not the output.

## Remaining risks, ordered by impact

| Priority | Risk | Consequence | Next verification |
| --- | --- | --- | --- |
| P0 | First Rare-logo model/render path remains unconfirmed after loader and VI-frame ownership hardening. | Black screen, fatal hold or EE fault immediately after Expansion Pak notice. | Retail run and last durable `title:`/`VideoPS2:` checkpoint. |
| P0 | Direct display-list writers can still overrun between phase checks. Central Vtx/Mtx/colour allocations and PS2 frame boundaries are now guarded. | A single oversized renderer may cross the Gfx boundary before the post-phase check catches it. | Add per-writer reservations or a trailing canary, then stress title and a gameplay stage. |
| P0 | Remaining unsupported combiner recipes are dropped. The renderer now counts dropped batches/triangles and durably checkpoints the first recipe; active room-fog `CUSTOM_11/CUSTOM_06` is implemented exactly. | Valid runtime with invisible geometry/effects. | Use the next title/game hardware trace to rank nonzero recipe IDs, then implement them in frequency order. |
| P1 | Offscreen framebuffer operations and copies are stubs. | Missing blur, surveillance, menu and other framebuffer effects. | Build an explicit render-target/copy path with VRAM budgeting. |
| P1 | Preprocessors validate some sizes after writing. | Corrupt ROM or bad estimate can overrun temporary output. | Convert writers to bounded cursors. |
| P1 | VIF1/VU1 and PATH3 synchronization is only partly proven. | Intermittent corruption or hangs that host tests cannot reproduce. | Long hardware stress run with DMA/VU checkpoints and canaries. |
| P1 | Mipmaps are unsupported. | Incorrect distant texture sampling and visual instability. | Implement only after correctness traces establish real game demand. |
| P2 | Shared 4 KiB `bootAllocateStack` compatibility stub. | Unsafe if the port begins using multiple real EE threads through that API. | Assert single-threaded use or allocate one aligned stack per owner. |
| P2 | Text and blend fidelity is imperfect. | Distorted glyphs and inaccurate translucent effects. | Reference-image comparisons after first-frame stability. |
| P2 | Linked C++ runtime carries exception/unwind sections despite source flags. | Avoidable ELF/RAM footprint. | Attribute the sections from the new linker map before changing libraries. |

## Safe cleanup policy

Before deleting a file, require all applicable evidence:

1. no CMake, workflow, generator, include or documentation reference;
2. no symbol/section contribution in the linker map when it is a runtime input;
3. no role in another ROM version or supported desktop platform;
4. no local diff or recoverable partial write;
5. reproducible generation when it is an output;
6. a separate cleanup commit so a regression is easy to bisect.

Under that policy, the interrupted hidden JSON fragments and downloaded
`artifacts/` directory are cleanup candidates. The recovered `setupwax.c`,
tracked PS2 sources, versioned assets and diagnostic tests are not deletion
candidates.

## Audit conclusion

The repository contained real correctness hazards, but no disposable tracked
PS2 source set. The productive cleanup is therefore structural: prevent local
outputs from entering commits, repair build ownership, retain a linker map and
turn silent startup corruption into bounded failures with durable diagnostics.
The next material milestone is a complete first Rare-logo frame on retail
hardware, followed by Gfx/Vtx arena bounds and unsupported-combiner closure.
