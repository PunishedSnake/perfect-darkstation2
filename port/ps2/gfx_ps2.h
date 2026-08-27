#ifndef PD_PS2_GFX_PS2_H
#define PD_PS2_GFX_PS2_H

#include "gfx_rendering_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fast3D backend consumes the already-initialised project GS core. */
extern struct GfxRenderingAPI gfx_ps2_api;

struct GfxRdpTmemLiveTextureView;

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
