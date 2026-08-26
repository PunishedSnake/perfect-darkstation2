#include "rdp_tmem_live.h"

#include <string.h>

static struct {
    bool initialized;
    struct GfxRdpTmemRuntime runtime;
    struct GfxRdpTmemLiveStats stats;
} s_live;

static void gfxRdpTmemLiveEnsureInitialized(void)
{
    if (!s_live.initialized) {
        gfxRdpTmemLiveReset();
    }
}

static void gfxRdpTmemLiveRecordLoad(enum GfxRdpTmemLoadResult result,
    uint32_t *exact, uint32_t *conservative)
{
    switch (result) {
        case GFX_RDP_TMEM_LOAD_EXACT:
            if (exact) {
                ++*exact;
            }
            break;
        case GFX_RDP_TMEM_LOAD_CONSERVATIVE:
            if (conservative) {
                ++*conservative;
            }
            break;
        case GFX_RDP_TMEM_LOAD_MALFORMED:
        default:
            ++s_live.stats.malformed_or_unsupported;
            break;
    }
}

extern "C" void gfxRdpTmemLiveReset(void)
{
    memset(&s_live, 0, sizeof(s_live));
    gfxRdpTmemRuntimeReset(&s_live.runtime);
    s_live.initialized = true;
}

extern "C" void gfxRdpTmemLiveSetTextureImage(uint32_t format, uint32_t size,
    uint32_t width_minus_one, const void *resolved_addr)
{
    gfxRdpTmemLiveEnsureInitialized();
    gfxRdpTmemRuntimeSetTextureImage(&s_live.runtime,
        (uint8_t)format, (uint8_t)size, (uint16_t)width_minus_one,
        resolved_addr);
    ++s_live.stats.set_texture_image;
}

extern "C" void gfxRdpTmemLiveSetTile(uint8_t fmt, uint32_t siz, uint32_t line,
    uint32_t tmem, uint8_t tile)
{
    gfxRdpTmemLiveEnsureInitialized();
    if (!gfxRdpTmemRuntimeSetTile(&s_live.runtime,
            tile, fmt, (uint8_t)siz, (uint16_t)line, (uint16_t)tmem)) {
        ++s_live.stats.malformed_or_unsupported;
        (void)gfxRdpTmemInvalidatePhysical(&s_live.runtime.tmem,
            0u, GFX_RDP_TMEM_BYTES);
    }
    ++s_live.stats.set_tile;
}

extern "C" void gfxRdpTmemLiveLoadTlut(uint8_t tile,
    uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt)
{
    gfxRdpTmemLiveEnsureInitialized();
    const enum GfxRdpTmemLoadResult result = gfxRdpTmemRuntimeLoadTlut(
        &s_live.runtime, tile, uls, ult, lrs, lrt);
    if (result == GFX_RDP_TMEM_LOAD_EXACT) {
        ++s_live.stats.load_tlut_exact;
    } else if (result == GFX_RDP_TMEM_LOAD_MALFORMED) {
        ++s_live.stats.malformed_or_unsupported;
    }
}

extern "C" void gfxRdpTmemLiveLoadTile(uint8_t tile,
    uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt)
{
    gfxRdpTmemLiveEnsureInitialized();
    gfxRdpTmemLiveRecordLoad(gfxRdpTmemRuntimeLoadTile(
        &s_live.runtime, tile, uls, ult, lrs, lrt),
        &s_live.stats.load_tile_exact,
        &s_live.stats.load_tile_conservative);
}

extern "C" void gfxRdpTmemLiveLoadBlock(uint8_t tile,
    uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt)
{
    gfxRdpTmemLiveEnsureInitialized();
    gfxRdpTmemLiveRecordLoad(gfxRdpTmemRuntimeLoadBlock(
        &s_live.runtime, tile, uls, ult, lrs, (uint16_t)dxt),
        &s_live.stats.load_block_exact,
        &s_live.stats.load_block_conservative);
}

extern "C" const struct GfxRdpTmem *gfxRdpTmemLiveState(void)
{
    gfxRdpTmemLiveEnsureInitialized();
    return gfxRdpTmemRuntimeState(&s_live.runtime);
}

extern "C" void gfxRdpTmemLiveGetStats(struct GfxRdpTmemLiveStats *stats)
{
    if (!stats) {
        return;
    }
    gfxRdpTmemLiveEnsureInitialized();
    *stats = s_live.stats;
}
