#ifndef PERFECT_DARK_FAST3D_RDP_TMEM_LIVE_H
#define PERFECT_DARK_FAST3D_RDP_TMEM_LIVE_H

#include <stdint.h>

#include "rdp_tmem_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GfxRdpTmemLiveStats {
    uint32_t set_texture_image;
    uint32_t set_tile;
    uint32_t load_tlut_exact;
    uint32_t load_tile_exact;
    uint32_t load_tile_conservative;
    uint32_t load_block_exact;
    uint32_t load_block_conservative;
    uint32_t malformed_or_unsupported;
};

/*
 * Ordered live TMEM owner for the Fast3D compatibility frontend.
 *
 * These hooks intentionally mirror gfx_pc.cpp's existing gfx_dp_* parameter
 * convention so the final integration is a handful of calls at the exact RDP
 * command execution points. The old source-pointer texture path remains the
 * visible renderer until cache/decode migration is separately validated.
 */
void gfxRdpTmemLiveReset(void);

void gfxRdpTmemLiveSetTextureImage(uint32_t format, uint32_t size,
    uint32_t width_minus_one, const void *resolved_addr);

void gfxRdpTmemLiveSetTile(uint8_t fmt, uint32_t siz, uint32_t line,
    uint32_t tmem, uint8_t tile);

void gfxRdpTmemLiveLoadTlut(uint8_t tile,
    uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);

void gfxRdpTmemLiveLoadTile(uint8_t tile,
    uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);

void gfxRdpTmemLiveLoadBlock(uint8_t tile,
    uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt);

const struct GfxRdpTmem *gfxRdpTmemLiveState(void);
void gfxRdpTmemLiveGetStats(struct GfxRdpTmemLiveStats *stats);

#ifdef __cplusplus
}
#endif

#endif
