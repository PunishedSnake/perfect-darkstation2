#ifndef PERFECT_DARK_FAST3D_RDP_TMEM_LIVE_H
#define PERFECT_DARK_FAST3D_RDP_TMEM_LIVE_H

#include <stdbool.h>
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
    uint32_t fingerprint_exact;
    uint32_t fingerprint_fallback;
    uint32_t materialize_exact;
    uint32_t materialize_fallback;
};

struct GfxRdpTmemLiveTextureView {
    const uint8_t *texels;
    uint32_t size_bytes;
    uint32_t line_size_bytes;
    const uint16_t *palette;
    uint16_t palette_first;
    uint16_t palette_count;
    uint64_t content_identity;
};

/*
 * Ordered live TMEM owner for the Fast3D compatibility frontend.
 *
 * These hooks intentionally mirror gfx_pc.cpp's existing gfx_dp_* parameter
 * convention so the PS2 integration stays at the exact RDP command execution
 * points. Exact shadow views may replace the old source-pointer decoder only
 * when all consumed texel/TLUT bytes can be reconstructed authoritatively.
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

/*
 * Build a deterministic content fingerprint for the TMEM footprint consumed by
 * the compatibility texture importer.
 *
 * `loaded_line_size_bytes` and `loaded_size_bytes` come from gfx_pc's existing
 * LoadedTexture metadata. The live RDP tile supplies TMEM base/line and raw
 * 32-bit RGBA split-bank semantics. `render_fmt`/`render_siz` are the final
 * compatibility format after gfx_pc's legacy remaps; CI additionally includes
 * the exact TLUT footprint selected by `palette_index`.
 *
 * Returns false when the shadow cannot prove that enough requested bytes are
 * represented exactly. Callers must then keep the legacy source-pointer key.
 */
bool gfxRdpTmemLiveTextureFingerprint(uint8_t tile,
    uint32_t loaded_line_size_bytes, uint32_t loaded_size_bytes,
    uint8_t render_fmt, uint8_t render_siz, uint8_t palette_index,
    uint64_t *fingerprint);

/*
 * Materialize the exact logical texture view consumed by the compatibility
 * importer. Storage is owned by the live TMEM layer and remains valid until
 * the next materialization/reset call. The function fails closed for YUV,
 * invalid shadow bytes, malformed layout, or an unproven TLUT footprint.
 */
bool gfxRdpTmemLiveMaterializeTexture(uint8_t tile,
    uint32_t loaded_line_size_bytes, uint32_t loaded_size_bytes,
    uint8_t render_fmt, uint8_t render_siz, uint8_t palette_index,
    struct GfxRdpTmemLiveTextureView *view);

const struct GfxRdpTmem *gfxRdpTmemLiveState(void);
void gfxRdpTmemLiveGetStats(struct GfxRdpTmemLiveStats *stats);

#ifdef __cplusplus
}
#endif

#endif
