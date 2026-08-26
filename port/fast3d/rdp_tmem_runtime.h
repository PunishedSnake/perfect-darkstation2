#ifndef PERFECT_DARK_FAST3D_RDP_TMEM_RUNTIME_H
#define PERFECT_DARK_FAST3D_RDP_TMEM_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "rdp_tmem.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_RDP_TMEM_RUNTIME_TILES 8u

enum GfxRdpTmemLoadResult {
    GFX_RDP_TMEM_LOAD_EXACT = 0,
    GFX_RDP_TMEM_LOAD_CONSERVATIVE,
    GFX_RDP_TMEM_LOAD_MALFORMED,
};

struct GfxRdpTmemRuntimeImage {
    const uint8_t *addr;
    uint8_t fmt;
    uint8_t siz;
    uint16_t width_minus_one;
    bool valid;
};

struct GfxRdpTmemRuntimeTile {
    uint8_t fmt;
    uint8_t siz;
    uint16_t line_words;
    uint16_t tmem_word;
    bool valid;
};

/*
 * Backend-independent runtime state for the RDP texture-load side of Fast3D.
 *
 * The producer is the ordered SetTextureImage/SetTile/Load* command stream.
 * The consumer is a future texture decoder/cache and, on PS2, the GS residency
 * layer. Keeping this state separate from gfx_pc's source-pointer aliases lets
 * the compatibility frontend migrate without changing visible output in the
 * same patch.
 */
struct GfxRdpTmemRuntime {
    struct GfxRdpTmem tmem;
    struct GfxRdpTmemRuntimeImage image;
    struct GfxRdpTmemRuntimeTile tiles[GFX_RDP_TMEM_RUNTIME_TILES];
};

void gfxRdpTmemRuntimeReset(struct GfxRdpTmemRuntime *runtime);

void gfxRdpTmemRuntimeSetTextureImage(struct GfxRdpTmemRuntime *runtime,
    uint8_t fmt, uint8_t siz, uint16_t width_minus_one,
    const void *resolved_addr);

bool gfxRdpTmemRuntimeSetTile(struct GfxRdpTmemRuntime *runtime,
    uint8_t tile, uint8_t fmt, uint8_t siz,
    uint16_t line_words, uint16_t tmem_word);

enum GfxRdpTmemLoadResult gfxRdpTmemRuntimeLoadTlut(
    struct GfxRdpTmemRuntime *runtime, uint8_t tile,
    uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);

enum GfxRdpTmemLoadResult gfxRdpTmemRuntimeLoadTile(
    struct GfxRdpTmemRuntime *runtime, uint8_t tile,
    uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);

enum GfxRdpTmemLoadResult gfxRdpTmemRuntimeLoadBlock(
    struct GfxRdpTmemRuntime *runtime, uint8_t tile,
    uint32_t uls, uint32_t ult, uint32_t lrs, uint16_t dxt);

const struct GfxRdpTmem *gfxRdpTmemRuntimeState(
    const struct GfxRdpTmemRuntime *runtime);

#ifdef __cplusplus
}
#endif

#endif
