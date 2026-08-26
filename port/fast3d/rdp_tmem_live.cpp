#include "rdp_tmem_live.h"

#include <stddef.h>
#include <string.h>

#ifndef G_IM_FMT_RGBA
#define G_IM_FMT_RGBA 0
#define G_IM_FMT_YUV  1
#define G_IM_FMT_CI   2
#endif

#ifndef G_IM_SIZ_4b
#define G_IM_SIZ_4b  0
#define G_IM_SIZ_8b  1
#define G_IM_SIZ_16b 2
#define G_IM_SIZ_32b 3
#endif

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

static void gfxRdpTmemLiveHashByte(uint64_t *hash, uint8_t value)
{
    *hash ^= value;
    *hash *= UINT64_C(1099511628211);
}

static bool gfxRdpTmemLiveHashWords(const struct GfxRdpTmem *tmem,
    uint32_t first_word, uint32_t word_count,
    uint64_t *hash, uint32_t *valid_bytes)
{
    if (!tmem || !hash || !valid_bytes ||
        first_word > GFX_RDP_TMEM_WORDS ||
        word_count > GFX_RDP_TMEM_WORDS - first_word) {
        return false;
    }

    for (uint32_t word = 0; word < word_count; ++word) {
        const uint32_t base =
            (first_word + word) * GFX_RDP_TMEM_WORD_BYTES;
        for (uint32_t lane = 0; lane < GFX_RDP_TMEM_WORD_BYTES; ++lane) {
            const uint32_t offset = base + lane;
            const bool valid = tmem->byte_valid[offset] != 0u;

            /*
             * Invalid bytes hash as an explicit marker, never as their stale
             * backing value. Conservative invalidation therefore cannot become
             * an accidental cache hit just because old bytes remain in storage.
             */
            gfxRdpTmemLiveHashByte(hash, valid ? 0xa5u : 0x5au);
            if (valid) {
                gfxRdpTmemLiveHashByte(hash, tmem->bytes[offset]);
                ++*valid_bytes;
            }
        }
    }

    return true;
}

static bool gfxRdpTmemLiveHashTextureRows(
    const struct GfxRdpTmemRuntimeTile *tile,
    const struct GfxRdpTmem *tmem,
    uint32_t row_count, bool split_rgba32,
    uint64_t *hash, uint32_t *valid_bytes)
{
    if (!tile || !tmem || !hash || !valid_bytes ||
        tile->line_words == 0u || row_count == 0u) {
        return false;
    }

    for (uint32_t row = 0; row < row_count; ++row) {
        const uint64_t row_word64 =
            (uint64_t)tile->tmem_word + (uint64_t)row * tile->line_words;

        if (split_rgba32) {
            if (row_word64 >= GFX_RDP_TMEM_HALF_WORDS ||
                tile->line_words >
                    GFX_RDP_TMEM_HALF_WORDS - (uint32_t)row_word64) {
                return false;
            }

            const uint32_t low_word = (uint32_t)row_word64;
            const uint32_t high_word = low_word + GFX_RDP_TMEM_HALF_WORDS;
            if (!gfxRdpTmemLiveHashWords(tmem, low_word,
                    tile->line_words, hash, valid_bytes) ||
                !gfxRdpTmemLiveHashWords(tmem, high_word,
                    tile->line_words, hash, valid_bytes)) {
                return false;
            }
        } else {
            if (row_word64 >= GFX_RDP_TMEM_WORDS ||
                tile->line_words >
                    GFX_RDP_TMEM_WORDS - (uint32_t)row_word64) {
                return false;
            }

            if (!gfxRdpTmemLiveHashWords(tmem, (uint32_t)row_word64,
                    tile->line_words, hash, valid_bytes)) {
                return false;
            }
        }

        /* Keep otherwise identical row concatenations structurally distinct. */
        gfxRdpTmemLiveHashByte(hash, 0x3cu);
    }

    return true;
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

extern "C" bool gfxRdpTmemLiveTextureFingerprint(uint8_t tile_index,
    uint32_t loaded_line_size_bytes, uint32_t loaded_size_bytes,
    uint8_t render_fmt, uint8_t render_siz, uint8_t palette_index,
    uint64_t *fingerprint)
{
    gfxRdpTmemLiveEnsureInitialized();

    if (!fingerprint || tile_index >= GFX_RDP_TMEM_RUNTIME_TILES ||
        loaded_line_size_bytes == 0u || loaded_size_bytes == 0u) {
        ++s_live.stats.fingerprint_fallback;
        return false;
    }

    const struct GfxRdpTmemRuntimeTile *tile =
        &s_live.runtime.tiles[tile_index];
    const struct GfxRdpTmem *tmem = &s_live.runtime.tmem;
    if (!tile->valid || tile->line_words == 0u ||
        tile->fmt == G_IM_FMT_YUV) {
        ++s_live.stats.fingerprint_fallback;
        return false;
    }

    const bool split_rgba32 =
        tile->fmt == G_IM_FMT_RGBA && tile->siz == G_IM_SIZ_32b;
    const uint64_t render_row_bytes64 =
        (uint64_t)tile->line_words * (split_rgba32 ? 16u : 8u);
    if (render_row_bytes64 == 0u || render_row_bytes64 > UINT32_MAX) {
        ++s_live.stats.fingerprint_fallback;
        return false;
    }

    const uint32_t render_row_bytes = (uint32_t)render_row_bytes64;
    const uint32_t rows_from_load =
        (loaded_size_bytes + loaded_line_size_bytes - 1u) /
        loaded_line_size_bytes;
    const uint32_t rows_from_render =
        (loaded_size_bytes + render_row_bytes - 1u) / render_row_bytes;
    const uint32_t row_count =
        rows_from_load > rows_from_render ? rows_from_load : rows_from_render;

    uint64_t hash = UINT64_C(1469598103934665603);
    uint32_t valid_texture_bytes = 0u;
    if (!gfxRdpTmemLiveHashTextureRows(tile, tmem,
            row_count, split_rgba32, &hash, &valid_texture_bytes) ||
        valid_texture_bytes < loaded_size_bytes) {
        ++s_live.stats.fingerprint_fallback;
        return false;
    }

    if (render_fmt == G_IM_FMT_CI) {
        uint32_t first_palette_word;
        uint32_t palette_words;

        if (render_siz == G_IM_SIZ_4b) {
            if (palette_index >= 16u) {
                ++s_live.stats.fingerprint_fallback;
                return false;
            }
            first_palette_word =
                GFX_RDP_TMEM_HALF_WORDS + (uint32_t)palette_index * 16u;
            palette_words = 16u;
        } else if (render_siz == G_IM_SIZ_8b) {
            first_palette_word = GFX_RDP_TMEM_HALF_WORDS;
            palette_words = GFX_RDP_TMEM_HALF_WORDS;
        } else {
            ++s_live.stats.fingerprint_fallback;
            return false;
        }

        uint32_t valid_palette_bytes = 0u;
        if (!gfxRdpTmemLiveHashWords(tmem, first_palette_word,
                palette_words, &hash, &valid_palette_bytes) ||
            valid_palette_bytes < palette_words * GFX_RDP_TMEM_WORD_BYTES) {
            ++s_live.stats.fingerprint_fallback;
            return false;
        }
    }

    /* Fold compatibility layout metadata into the content identity itself. */
    for (uint32_t shift = 0u; shift < 32u; shift += 8u) {
        gfxRdpTmemLiveHashByte(&hash,
            (uint8_t)(loaded_size_bytes >> shift));
        gfxRdpTmemLiveHashByte(&hash,
            (uint8_t)(loaded_line_size_bytes >> shift));
        gfxRdpTmemLiveHashByte(&hash,
            (uint8_t)(render_row_bytes >> shift));
    }
    gfxRdpTmemLiveHashByte(&hash, render_fmt);
    gfxRdpTmemLiveHashByte(&hash, render_siz);
    gfxRdpTmemLiveHashByte(&hash, palette_index);

    *fingerprint = hash;
    ++s_live.stats.fingerprint_exact;
    return true;
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
