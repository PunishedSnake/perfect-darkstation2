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

/* Exact runtime proof used to select the one-channel material graph. */
static inline uint32_t gfxPs2MaterialRgbChannelPasses(bool monochrome_rgb)
{
    return monochrome_rgb ? 1u : 3u;
}

/* Union of two normalized coverage values: B + A * (1 - B). */
static inline float gfxPs2CoverageUnion(float a, float b)
{
    return b + a * (1.0f - b);
}

static inline bool gfxPs2Rgba32IsMonochrome(
    const uint8_t *rgba32, uint32_t texel_count)
{
    if (!rgba32 && texel_count != 0u) {
        return false;
    }
    for (uint32_t i = 0u; i < texel_count; ++i) {
        const uint8_t *texel = &rgba32[i * 4u];
        if (texel[0] != texel[1] || texel[0] != texel[2]) {
            return false;
        }
    }
    return true;
}

static inline bool gfxPs2N64Rgba16IsMonochrome(
    const uint8_t *rgba5551_be, uint32_t texel_count)
{
    if (!rgba5551_be && texel_count != 0u) {
        return false;
    }
    for (uint32_t i = 0u; i < texel_count; ++i) {
        const uint16_t texel =
            ((uint16_t)rgba5551_be[i * 2u] << 8u) |
            rgba5551_be[i * 2u + 1u];
        const uint16_t r = (texel >> 11u) & 0x1fu;
        const uint16_t g = (texel >> 6u) & 0x1fu;
        const uint16_t b = (texel >> 1u) & 0x1fu;
        if (r != g || r != b) {
            return false;
        }
    }
    return true;
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

/* Emit the current renderer/VU1 counters, optionally forcing durable storage. */
void gfxPs2LogRendererStats(bool checkpoint);

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
