# N64 RDP TMEM semantics required by the PS2 renderer

Status: implementation contract / research record for the `ps2` branch.

Purpose: document the original Nintendo 64 texture-memory semantics that Perfect Dark's Fast3D/RDP command stream expects, before translating those semantics into PS2-native GS resources. This file is intentionally about the **logical N64 contract**, not about copying N64 hardware execution literally onto PS2.

## 1. Primary Nintendo sources

Nintendo 64 Programming Manual mirrors used for this work:

- Chapter 12, Texture Engine:
  https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro12/12-04.html
- Chapter 13, Texture Mapping overview:
  https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-01.html
- Chapter 13, Tile Attributes:
  https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-04.html
- Chapter 13, TMEM layout:
  https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-08.html
- Chapter 13, Texture Loading:
  https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-09.html
- Nintendo GBI header/macro reference:
  https://ultra64.ca/files/documentation/online-manuals/man/header/gbi.htm
- N64 Functions Reference, texture loading:
  https://ultra64.ca/files/documentation/online-manuals/functions_reference_manual_2.0i/gdp/gDPLoadTexture.html

These sources are used to establish the compatibility semantics. PS2 implementation choices remain governed by the PS2 Optimization Research Library v2.

## 2. Confirmed logical model

### 2.1 TMEM capacity and access

**CONFIRMED:** RDP contains 4 KiB of on-chip texture memory (TMEM), physically organized into four simultaneously accessible banks.

```text
TMEM bytes: 4096
TMEM 64-bit words: 512
```

The application may place several logical texture regions in TMEM at once, but only eight tile descriptors are active at a time.

### 2.2 Eight tile descriptors

**CONFIRMED:** each tile descriptor includes at least:

```text
format
texel size
line
TMEM address
palette
mirror S/T
mask S/T
shift S/T
SL/TL
SH/TH
clamp S/T
```

`line` is the number of 64-bit TMEM words in one row/stride of the tile.

The load tile and the render tile do not have to be the same descriptor. Nintendo explicitly recommends patterns such as loading through tile 7 while rendering through tile 0 to reduce synchronization pressure.

### 2.3 SetTextureImage is source state, not a load

**CONFIRMED:** `SetTextureImage` establishes the source image used by a later load command. The normal sequence is conceptually:

```text
SetTextureImage      source image state
SetTile              load-tile format / TMEM destination
LoadSync             dependency barrier where required
LoadTile/LoadBlock   actual TMEM mutation
SetTile              render-tile state
SetTileSize          render bounds
```

Therefore a backend model in which `SetTextureImage` or a later texture-cache lookup simply aliases the original source pointer is not equivalent to the RDP contract when display lists reuse or overwrite TMEM.

## 3. LoadTile

**CONFIRMED:** `LoadTile` performs a rectangular load from source DRAM into TMEM.

Important properties:

- each row may cause a separate DRAM transfer;
- each row is automatically padded to a 64-bit TMEM-word boundary;
- tile corner coordinates select the loaded rectangle;
- `LoadTile` updates the load tile's `SL/TL/SH/TH` fields when executed;
- row padding can consume TMEM that is not visible texel data.

This means texture identity cannot be derived only from the source pointer and visible width/height. Destination TMEM placement, row stride and load history matter.

## 4. LoadBlock

**CONFIRMED:** `LoadBlock` treats the source as one long transfer instead of one DRAM transfer per row.

`dxt` controls logical line transitions during that transfer. Nintendo describes `dxt` as effectively the reciprocal of the number of 64-bit words in one line. A counter accumulates `dxt` for each TMEM word transferred; integer rollover increments the logical line number.

The line number matters because **odd lines are word-swapped/interleaved for rendering access**.

Consequences:

- the same linear source bytes can produce a different usable TMEM layout depending on `dxt`;
- `LoadBlock` does not perform the same per-row padding convenience as `LoadTile`;
- source data used by `LoadBlock` must satisfy the required padding/layout rules;
- non-power-of-two `dxt` can accumulate line-counter error; Nintendo documents restrictions/workarounds;
- pre-swapped texture macros use `dxt = 0` because the odd-line interleave is already present in the source;
- the low-level command may load at most 2048 texels in one operation, with macro tricks changing the temporary load size for formats such as 4-bit textures.

### Implementation constraint

Do **not** implement `LoadBlock` as a plain `memcpy(source, tmem)` and call it faithful.

The exact word-swap mapping and format-dependent bank addressing must be encoded from documentation/reference behavior before it replaces the current source-pointer compatibility path.

## 5. TLUT / CI semantics

**CONFIRMED:** color-indexed textures partition TMEM usage:

- CI texel/index data resides in the lower half of TMEM;
- TLUT entries reside in the upper half;
- 8-bit CI uses a 256-entry lookup table;
- 4-bit CI uses a tile palette selector and may address one of sixteen 16-entry palettes.

`LoadTLUT` expands each 16-bit color entry into a 64-bit TMEM word by replicating the entry four times across the banks. Nintendo describes this as "quadrication".

Supported TLUT interpretations include 16-bit RGBA and 16-bit IA according to RDP state.

### Compatibility consequence

A separate host array such as `uint16_t palette[256]` can be a useful decoded cache, but it is not by itself the authoritative RDP memory model. Correct invalidation and identity must follow the TMEM/TLUT mutation that produced it.

## 6. 32-bit RGBA and physical layout

**CONFIRMED:** 32-bit RGBA occupies up to 1K texels in the 4 KiB TMEM and has a banked layout distinct from simple packed host RGBA32 memory.

The Nintendo TMEM-layout figures show the format-specific organization.

### Implementation constraint

The PS2 frontend must not assume that a faithful TMEM shadow for RGBA32 can always be interpreted as one simple contiguous host RGBA32 array.

The exact bank mapping is a required implementation detail before full replacement of the old pointer-alias path.

## 7. Current Perfect DarkStation 2 compatibility behavior

**CURRENT IMPLEMENTATION:** `port/fast3d/gfx_pc.cpp` currently stores:

```text
texture_to_load.addr
texture_to_load.siz
texture_to_load.width
...
texture_tile[8]
loaded_texture[512]
palette[256]
```

`gfx_dp_set_texture_image()` correctly behaves as source-state setup.

However, the actual load functions are approximations:

### `gfx_dp_load_block()`

It computes sizes and associates the current source pointer with:

```text
rdp.loaded_texture[rdp.texture_tile[tile].tmem]
```

but does not materialize the 4 KiB TMEM contents, `dxt` line transitions or odd-line swapping.

### `gfx_dp_load_tile()`

It computes a source offset/stride and associates a pointer to the selected source region with the TMEM-word slot, but does not materialize physical TMEM row padding/layout.

### `gfx_dp_load_tlut()`

It copies converted 16-bit palette entries into a host palette array and tracks source addresses, but does not make a 4 KiB TMEM image the authoritative storage/invalidation source.

This pointer-alias model is a useful portable-renderer shortcut, but it is not the documented RDP TMEM contract.

## 8. Required replacement architecture

The backend-independent Fast3D/RDP frontend should converge on:

```text
RDP source image descriptor
        |
        +-> LoadTile
        +-> LoadBlock
        +-> LoadTLUT
                |
                v
        authoritative 4096-byte TMEM image
                |
        +-------+-------+
        |               |
  8 tile descriptors   TMEM generation/range generations
        |               |
        +-------+-------+
                |
        logical render texture view
                |
        backend translation/cache
                |
        PS2 GS residency / desktop GPU texture
```

The N64-compatible TMEM layer must not know about GS VRAM, `GSTEXTURE`, OpenGL texture IDs or PS2 upload queues.

## 9. Texture cache identity

The current source-address-oriented identity is insufficient once TMEM behavior is modeled.

A replacement cache key should include the information that makes the **rendered TMEM view** distinct, for example:

```text
TMEM generation or generation of the touched word range
tile TMEM base
format
size
line stride
palette selector / TLUT state
SL/TL/SH/TH
mask / mirror / clamp / shift where they alter decoded texel view
raw-texture metadata required by the port's asset extension path
```

This list is an implementation design, not a claim that every field must necessarily be hashed independently. The final key should be reduced only after proving which state can safely be excluded.

## 10. Incremental implementation plan

### Stage A: shadow without changing output

Add a checked 4096-byte TMEM model and mutation-generation tracking. Keep the existing pointer-based import path active while shadow writes are validated.

Purpose:

- establish ownership and range validation;
- log the real Perfect Dark TMEM traffic;
- compare shadow state with expected load commands;
- avoid changing visible output before exact format layouts are verified.

### Stage B: TLUT authority

Move TLUT mutation/identity to the TMEM model while retaining the existing decoded `palette[256]` cache as a derived view.

This is attractive early because Nintendo documents the 16-bit-entry -> replicated 64-bit-word behavior explicitly.

### Stage C: LoadTile

Implement exact row copy/padding and format-specific TMEM placement. Compare decoded results against the existing renderer on known scenes.

### Stage D: LoadBlock

Implement `dxt`, logical line rollover and the documented odd-line word swapping. Include pre-swapped / `dxt=0` behavior.

Do not promote Stage D without targeted tests because a superficially plausible `memcpy` implementation is specifically wrong here.

### Stage E: render from TMEM

Switch the texture importer/cache from source aliases to logical views decoded from authoritative TMEM.

### Stage F: PS2-native formats

Only after the frontend is correct, translate logical N64 texture state into the best GS representation:

```text
CI4/CI8 -> indexed GS formats + CLUT where semantics fit
RGBA16  -> 16-bit GS representation where appropriate
other formats -> measured conversion path
large streamed imagery -> IPU experiment candidate, not default
```

## 11. Instrumentation required before optimization

The shadow stage should collect at least:

```text
SetTextureImage count
SetTile count
LoadTile count
LoadBlock count
LoadTLUT count
bytes requested per load
TMEM destination ranges
TMEM overwrite count/ranges
dxt distribution
format/size distribution
render tile vs load tile distribution
CI4/CI8/TLUT use
maximum simultaneously live TMEM span
```

For Perfect Dark this inventory is more valuable than guessing the "usual N64 texture path" from other games.

## 12. Validation cases

Minimum targeted cases before the new TMEM path becomes authoritative:

1. `LoadTile` with row width already 64-bit aligned.
2. `LoadTile` requiring right-side 64-bit padding.
3. `LoadBlock` with power-of-two `dxt` and multiple lines.
4. pre-swapped `LoadBlock` / `dxt=0` case.
5. CI4 with selected 16-entry palette.
6. CI8 with 256-entry TLUT.
7. TLUT overwrite followed by reuse of the same index texture.
8. same source pointer loaded into different TMEM addresses.
9. different source data overwriting the same TMEM address.
10. load tile different from render tile.
11. partial tile load followed by render using a different stride/bounds descriptor.
12. RGBA32 path covering the documented split/banked storage.

Each case needs an expected decoded-output hash or byte-exact TMEM reference where the documentation permits one.

## 13. PS2 translation boundary

The native PS2 renderer begins **after** this compatibility layer has established the logical texture contents/state.

Correct architecture:

```text
N64 display list semantics
 -> authoritative logical TMEM/tile state
 -> texture/material extraction
 -> PS2 representation decision
 -> residency/upload scheduling
 -> GS
```

Incorrect architecture:

```text
N64 command
 -> guess a GS upload directly
 -> repair visual errors with game-specific hacks
```

The second path is tempting because it renders something quickly. It also turns every later edge case into archaeology.

## 14. Current unknowns that require further source/manual validation

The following should remain explicit research items rather than inferred behavior:

- byte/word mapping for every format across all four physical TMEM banks;
- exact odd-line swap mapping for each relevant texel size in the generic low-level model;
- all RGBA32 bank/split details needed for a canonical 4096-byte software shadow;
- YUV TMEM layout if Perfect Dark actually exercises it;
- whether any Perfect Dark display lists rely on documented `dxt` error-edge cases or pre-shuffled assets;
- interaction of the port's custom raw-texture metadata scaling with a faithful TMEM model.

Until these are resolved, the pointer-based renderer remains the correctness comparison baseline, not an authoritative model of RDP memory.