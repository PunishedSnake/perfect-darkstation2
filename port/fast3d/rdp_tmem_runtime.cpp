#include "rdp_tmem_runtime.h"

#include <stddef.h>
#include <string.h>

#ifndef G_TEXTURE_IMAGE_FRAC
#define G_TEXTURE_IMAGE_FRAC 2
#endif

#ifndef G_IM_FMT_RGBA
#define G_IM_FMT_RGBA 0
#define G_IM_FMT_YUV  1
#endif

#ifndef G_IM_SIZ_16b
#define G_IM_SIZ_16b 2
#define G_IM_SIZ_32b 3
#endif

static uint32_t gfxRdpTmemRuntimeBitsPerTexel(uint32_t siz)
{
    return siz <= G_IM_SIZ_32b ? (4u << siz) : 0u;
}

static void gfxRdpTmemRuntimeInvalidateAll(struct GfxRdpTmemRuntime *runtime)
{
    if (runtime) {
        (void)gfxRdpTmemInvalidatePhysical(&runtime->tmem,
            0u, GFX_RDP_TMEM_BYTES);
    }
}

static enum GfxRdpTmemLoadResult gfxRdpTmemRuntimeMalformed(
    struct GfxRdpTmemRuntime *runtime)
{
    gfxRdpTmemRuntimeInvalidateAll(runtime);
    return GFX_RDP_TMEM_LOAD_MALFORMED;
}

static enum GfxRdpTmemLoadResult gfxRdpTmemRuntimeConservative(
    struct GfxRdpTmemRuntime *runtime)
{
    gfxRdpTmemRuntimeInvalidateAll(runtime);
    return GFX_RDP_TMEM_LOAD_CONSERVATIVE;
}

extern "C" void gfxRdpTmemRuntimeReset(struct GfxRdpTmemRuntime *runtime)
{
    if (!runtime) {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
    gfxRdpTmemReset(&runtime->tmem);
}

extern "C" void gfxRdpTmemRuntimeSetTextureImage(
    struct GfxRdpTmemRuntime *runtime,
    uint8_t fmt, uint8_t siz, uint16_t width_minus_one,
    const void *resolved_addr)
{
    if (!runtime) {
        return;
    }

    runtime->image.addr = (const uint8_t *)resolved_addr;
    runtime->image.fmt = fmt;
    runtime->image.siz = siz;
    runtime->image.width_minus_one = width_minus_one;
    runtime->image.valid = resolved_addr != NULL;
}

extern "C" bool gfxRdpTmemRuntimeSetTile(struct GfxRdpTmemRuntime *runtime,
    uint8_t tile, uint8_t fmt, uint8_t siz,
    uint16_t line_words, uint16_t tmem_word)
{
    if (!runtime || tile >= GFX_RDP_TMEM_RUNTIME_TILES ||
        tmem_word >= GFX_RDP_TMEM_WORDS) {
        return false;
    }

    struct GfxRdpTmemRuntimeTile *dst = &runtime->tiles[tile];
    dst->fmt = fmt;
    dst->siz = siz;
    dst->line_words = line_words;
    dst->tmem_word = tmem_word;
    dst->valid = true;
    return true;
}

extern "C" enum GfxRdpTmemLoadResult gfxRdpTmemRuntimeLoadTlut(
    struct GfxRdpTmemRuntime *runtime, uint8_t tile_index,
    uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt)
{
    if (!runtime || tile_index >= GFX_RDP_TMEM_RUNTIME_TILES) {
        return gfxRdpTmemRuntimeMalformed(runtime);
    }

    const struct GfxRdpTmemRuntimeTile *tile = &runtime->tiles[tile_index];
    if (!runtime->image.valid || !tile->valid ||
        runtime->image.siz != G_IM_SIZ_16b ||
        lrs < uls || lrt < ult) {
        return gfxRdpTmemRuntimeMalformed(runtime);
    }

    const uint32_t width = lrs - uls + 1u;
    const uint32_t height = lrt - ult + 1u;
    const uint64_t count64 = (uint64_t)width * height;
    if (count64 == 0u || count64 > 256u ||
        tile->tmem_word < GFX_RDP_TMEM_HALF_WORDS ||
        count64 > GFX_RDP_TMEM_WORDS - tile->tmem_word) {
        return gfxRdpTmemRuntimeMalformed(runtime);
    }

    const uint32_t pitch = (uint32_t)runtime->image.width_minus_one + 1u;
    const uint64_t first_index = (uint64_t)pitch * ult + uls;
    const uint8_t *src = runtime->image.addr + first_index * 2u;
    uint16_t entries[256];
    const uint32_t count = (uint32_t)count64;

    for (uint32_t i = 0; i < count; ++i) {
        entries[i] = (uint16_t)((uint16_t)src[i * 2u] << 8) |
                     (uint16_t)src[i * 2u + 1u];
    }

    if (!gfxRdpTmemWriteTlut(&runtime->tmem,
            tile->tmem_word, entries, count)) {
        return gfxRdpTmemRuntimeMalformed(runtime);
    }

    return GFX_RDP_TMEM_LOAD_EXACT;
}

extern "C" enum GfxRdpTmemLoadResult gfxRdpTmemRuntimeLoadTile(
    struct GfxRdpTmemRuntime *runtime, uint8_t tile_index,
    uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt)
{
    if (!runtime || tile_index >= GFX_RDP_TMEM_RUNTIME_TILES) {
        return gfxRdpTmemRuntimeMalformed(runtime);
    }

    const struct GfxRdpTmemRuntimeTile *tile = &runtime->tiles[tile_index];
    if (!runtime->image.valid || !tile->valid || lrs < uls || lrt < ult) {
        return gfxRdpTmemRuntimeMalformed(runtime);
    }

    const uint32_t bits = gfxRdpTmemRuntimeBitsPerTexel(runtime->image.siz);
    if (bits == 0u) {
        return gfxRdpTmemRuntimeConservative(runtime);
    }

    const uint32_t offset_x = uls >> G_TEXTURE_IMAGE_FRAC;
    const uint32_t offset_y = ult >> G_TEXTURE_IMAGE_FRAC;
    const uint32_t width = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1u;
    const uint32_t height = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1u;
    const uint32_t full_width = (uint32_t)runtime->image.width_minus_one + 1u;
    const uint64_t source_stride_bits = (uint64_t)full_width * bits;
    const uint64_t source_x_bits = (uint64_t)offset_x * bits;

    /* The current exact writers consume whole bytes, not half-byte starts. */
    if ((source_stride_bits & 7u) != 0u || (source_x_bits & 7u) != 0u ||
        source_stride_bits / 8u > UINT32_MAX) {
        return gfxRdpTmemRuntimeConservative(runtime);
    }

    const uint32_t source_stride = (uint32_t)(source_stride_bits / 8u);
    const uint64_t start_offset =
        (uint64_t)offset_y * source_stride + source_x_bits / 8u;
    const uint8_t *source = runtime->image.addr + start_offset;

    if (tile->fmt == G_IM_FMT_YUV || runtime->image.fmt == G_IM_FMT_YUV) {
        return gfxRdpTmemRuntimeConservative(runtime);
    }

    if (tile->fmt == G_IM_FMT_RGBA &&
        runtime->image.fmt == G_IM_FMT_RGBA &&
        tile->siz == G_IM_SIZ_32b &&
        runtime->image.siz == G_IM_SIZ_32b) {
        if (!gfxRdpTmemLoadTileRgba32(&runtime->tmem,
                tile->tmem_word, tile->line_words,
                source, source_stride, width, height)) {
            return gfxRdpTmemRuntimeConservative(runtime);
        }
        return GFX_RDP_TMEM_LOAD_EXACT;
    }

    const uint32_t row_bytes =
        (uint32_t)(((uint64_t)width * bits + 7u) / 8u);
    if (!gfxRdpTmemLoadTileLinear(&runtime->tmem,
            tile->tmem_word, tile->line_words,
            source, source_stride, row_bytes, height)) {
        return gfxRdpTmemRuntimeConservative(runtime);
    }

    return GFX_RDP_TMEM_LOAD_EXACT;
}

extern "C" enum GfxRdpTmemLoadResult gfxRdpTmemRuntimeLoadBlock(
    struct GfxRdpTmemRuntime *runtime, uint8_t tile_index,
    uint32_t uls, uint32_t ult, uint32_t lrs, uint16_t dxt)
{
    if (!runtime || tile_index >= GFX_RDP_TMEM_RUNTIME_TILES) {
        return gfxRdpTmemRuntimeMalformed(runtime);
    }

    const struct GfxRdpTmemRuntimeTile *tile = &runtime->tiles[tile_index];
    if (!runtime->image.valid || !tile->valid || uls != 0u || ult != 0u) {
        return gfxRdpTmemRuntimeMalformed(runtime);
    }

    const uint64_t size64 = ((uint64_t)lrs + 1u) << runtime->image.siz >> 1u;
    if (size64 == 0u || size64 > UINT32_MAX) {
        return gfxRdpTmemRuntimeMalformed(runtime);
    }

    if (tile->fmt == G_IM_FMT_YUV || runtime->image.fmt == G_IM_FMT_YUV) {
        return gfxRdpTmemRuntimeConservative(runtime);
    }

    if (tile->fmt == G_IM_FMT_RGBA &&
        runtime->image.fmt == G_IM_FMT_RGBA &&
        tile->siz == G_IM_SIZ_32b &&
        runtime->image.siz == G_IM_SIZ_32b) {
        if (!gfxRdpTmemLoadBlockRgba32(&runtime->tmem,
                tile->tmem_word, tile->line_words,
                runtime->image.addr, lrs + 1u, dxt)) {
            return gfxRdpTmemRuntimeConservative(runtime);
        }
        return GFX_RDP_TMEM_LOAD_EXACT;
    }

    if (!gfxRdpTmemLoadBlockLinear(&runtime->tmem,
            tile->tmem_word, runtime->image.addr, (uint32_t)size64, dxt)) {
        return gfxRdpTmemRuntimeConservative(runtime);
    }

    return GFX_RDP_TMEM_LOAD_EXACT;
}

extern "C" const struct GfxRdpTmem *gfxRdpTmemRuntimeState(
    const struct GfxRdpTmemRuntime *runtime)
{
    return runtime ? &runtime->tmem : NULL;
}
