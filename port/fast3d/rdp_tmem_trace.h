#ifndef PERFECT_DARK_FAST3D_RDP_TMEM_TRACE_H
#define PERFECT_DARK_FAST3D_RDP_TMEM_TRACE_H

#include <stdint.h>

#include <PR/gbi.h>

#include "rdp_tmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shadow-only RDP texture-load interpreter.
 *
 * This deliberately does not alter rendering output. It observes the same GBI
 * display list consumed by gfx_pc.cpp and builds a backend-independent TMEM
 * image so loader semantics can be verified before texture-cache identity is
 * switched away from source-pointer aliases.
 */
struct GfxRdpTmemTraceStats {
    uint32_t display_lists;
    uint32_t commands;
    uint32_t set_texture_image;
    uint32_t set_tile;
    uint32_t load_tlut_exact;
    uint32_t load_tile_exact;
    uint32_t load_tile_conservative;
    uint32_t load_block_exact;
    uint32_t load_block_conservative;
    uint32_t malformed_or_unsupported;
};

void gfxRdpTmemTraceReset(void);
void gfxRdpTmemTraceDisplayList(const Gfx *commands);
const struct GfxRdpTmem *gfxRdpTmemTraceState(void);
void gfxRdpTmemTraceGetStats(struct GfxRdpTmemTraceStats *stats);

#ifdef __cplusplus
}
#endif

#endif
