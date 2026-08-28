#ifndef PD_PS2_GFX_PS2_H
#define PD_PS2_GFX_PS2_H

#include "gfx_rendering_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fast3D backend consumes the already-initialised project GS core. */
extern struct GfxRenderingAPI gfx_ps2_api;

struct GfxRdpTmemLiveTextureView;

/* Backend-private cache variant for a physically expanded mirror period. */
static inline uint8_t gfxPs2TextureMirrorVariant(uint8_t cms, uint8_t cmt)
{
    const uint8_t mirror_s = (cms & 1u) != 0u ? 1u : 0u;
    const uint8_t mirror_t = (cmt & 1u) != 0u ? 2u : 0u;
    return mirror_s | mirror_t;
}

/* Decode Fast3D's normalized last-texel-centre clamp into GS texel bounds. */
static inline bool gfxPs2TextureRegionClampMax(
    float normalized_bound, uint32_t logical_extent, uint16_t *maximum)
{
    if (!maximum || logical_extent == 0u ||
        !(normalized_bound >= 0.0f)) {
        return false;
    }

    const float last_center = normalized_bound * (float)logical_extent;
    if (!(last_center >= 0.5f) || last_center > 1023.5f) {
        return false;
    }

    const uint32_t extent = (uint32_t)(last_center + 0.5f);
    if (extent == 0u || extent > 1024u) {
        return false;
    }
    *maximum = (uint16_t)(extent - 1u);
    return true;
}

/* (SHADE + ENV) >= reference becomes SHADE >= reference - ENV. */
static inline uint8_t gfxPs2TextureEdgeAdjustedReference(
    uint8_t reference, uint8_t environment)
{
    return environment >= reference
        ? 0u : (uint8_t)(reference - environment);
}

static inline uint64_t gfxPs2TextureVariantIdentity(
    uint64_t identity, uint8_t format, uint8_t cms, uint8_t cmt,
    uint32_t palette_format)
{
    const uint64_t palette_variant = format == 2u
        ? (uint64_t)((palette_format >> 14u) & 3u) : 0u;
    const uint64_t variant = gfxPs2TextureMirrorVariant(cms, cmt) |
        (palette_variant << 2u);
    identity ^= variant + UINT64_C(0x9e3779b97f4a7c15) +
        (identity << 6u) + (identity >> 2u);
    return identity;
}

/* Set the physical upload variant before the native or RGBA32 fallback path. */
void gfxPs2SetTextureUploadMirror(uint8_t cms, uint8_t cmt);

/*
 * PS2-only import seam used by the generated live-TMEM frontend. Returns true
 * only when the exact logical view was consumed in a native GS format.
 */
bool gfxPs2UploadTmemTexture(const struct GfxRdpTmemLiveTextureView *view,
    uint8_t format, uint8_t size, uint32_t palette_format,
    bool gen_mipmaps);

#ifdef __cplusplus
}
#endif

#endif
