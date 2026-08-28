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
    const uint8_t mirror_s = (cms & 3u) == 1u ? 1u : 0u;
    const uint8_t mirror_t = (cmt & 3u) == 1u ? 2u : 0u;
    return mirror_s | mirror_t;
}

static inline uint64_t gfxPs2TextureVariantIdentity(
    uint64_t identity, uint8_t cms, uint8_t cmt)
{
    const uint64_t variant = gfxPs2TextureMirrorVariant(cms, cmt);
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
