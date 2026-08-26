#ifndef PERFECT_DARK_PS2_GS_TEXTURE_CONVERT_H
#define PERFECT_DARK_PS2_GS_TEXTURE_CONVERT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert N64 big-endian RGBA5551 texels to the GS PSMCT16 A1B5G5R5 bit
 * layout. Source and destination may alias exactly.
 */
bool ps2GsConvertN64Rgba16ToGsCt16(const uint8_t *source,
    uint8_t *destination, uint32_t texel_count);

#ifdef __cplusplus
}
#endif

#endif
