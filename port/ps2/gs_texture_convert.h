#ifndef PERFECT_DARK_PS2_GS_TEXTURE_CONVERT_H
#define PERFECT_DARK_PS2_GS_TEXTURE_CONVERT_H

#include <stdbool.h>
#include <stdint.h>

#include "gs_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert N64 big-endian RGBA5551 texels to the GS PSMCT16 A1B5G5R5 bit
 * layout. Source and destination may alias exactly.
 */
bool ps2GsConvertN64Rgba16ToGsCt16(const uint8_t *source,
    uint8_t *destination, uint32_t texel_count);

/* GS PSMT4 consumes the low nibble first; N64 CI4 stores the high nibble first. */
bool ps2GsConvertN64Ci4ToGsT4(const uint8_t *source,
    uint8_t *destination, uint32_t byte_count);

/*
 * Convert logical N64 RGBA5551 TLUT entries to a GS PSMCT16 CSM1 CLUT.
 * CI4 uses 16 entries unchanged in index order. The 256-entry CI8 palette is
 * permuted to the GS CSM1 block order while preserving every RGBA5551 bit.
 */
bool ps2GsConvertN64Rgba16PaletteToGsCt16(const uint16_t *source,
    uint8_t *destination, uint32_t entry_count);

/*
 * Build an exact RGBA32 CSM1 CLUT for a native N64 intensity texture.
 * Four-bit formats require 16 entries; eight-bit formats require 256.
 */
bool ps2GsBuildN64IntensityClut(enum Ps2GsN64IntensityEncoding encoding,
    uint32_t *destination, uint32_t entry_count);

/*
 * GS TBW is expressed in 64-pixel units. PSMT4/PSMT8 buffers additionally
 * require a 128-pixel buffer-width alignment, unlike direct-color formats.
 */
uint32_t ps2GsTextureBufferWidth(uint32_t width, bool indexed);

#ifdef __cplusplus
}
#endif

#endif
