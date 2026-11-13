#include "rdp_tmem_live.h"
#include "rdp_tmem.h"
#include "rdp_tmem_hash.h"

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

/* Pending lazy block loads; active entries are pairwise disjoint. */
enum { GFX_RDP_TMEM_LIVE_DEFERRED = 4 };
struct GfxRdpTmemLiveDeferredLoad {
    bool active;
    uint32_t first_word;
    uint32_t word_count;
    const uint8_t *source;  /* start of the original block */
    uint32_t skip_words;    /* first_word holds block word skip_words */
    uint16_t dxt;
    /* memo of the last source hash, as block-relative word range */
    bool hash_valid;
    uint32_t hash_skip;
    uint32_t hash_count;
    uint64_t hash;
};

static struct {
    bool initialized;
    struct GfxRdpTmemRuntime runtime;
    struct GfxRdpTmemLiveStats stats;
    struct GfxRdpTmemLiveDeferredLoad deferred[GFX_RDP_TMEM_LIVE_DEFERRED];
    uint32_t deferred_next;
    uint8_t materialized_texels[GFX_RDP_TMEM_BYTES];
    uint16_t materialized_palette[256];
} s_live;

static void gfxRdpTmemLiveEnsureInitialized(void)
{
    if (!s_live.initialized) {
        gfxRdpTmemLiveReset();
    }
}

static void gfxRdpTmemLiveFlushOne(struct GfxRdpTmemLiveDeferredLoad *load)
{
    if (!load->active) {
        return;
    }
    load->active = false;
    /* Cannot fail: every precondition was checked when it was deferred. */
    (void)gfxRdpTmemLoadBlockLinearPart(&s_live.runtime.tmem,
        load->first_word, load->source, load->skip_words,
        load->word_count, load->dxt);
}

extern "C" void gfxRdpTmemLiveFlush(void)
{
    if (!s_live.initialized) {
        return;
    }
    for (uint32_t i = 0; i < GFX_RDP_TMEM_LIVE_DEFERRED; ++i) {
        gfxRdpTmemLiveFlushOne(&s_live.deferred[i]);
    }
}

static bool gfxRdpTmemLiveDeferredOverlap(
    const struct GfxRdpTmemLiveDeferredLoad *load,
    uint32_t first_word, uint32_t word_count)
{
    return load->active &&
        first_word < load->first_word + load->word_count &&
        load->first_word < first_word + word_count;
}

/* Settle every pending load that touches [first_word, +word_count). */
static void gfxRdpTmemLiveFlushOverlapping(uint32_t first_word,
    uint32_t word_count)
{
    for (uint32_t i = 0; i < GFX_RDP_TMEM_LIVE_DEFERRED; ++i) {
        if (gfxRdpTmemLiveDeferredOverlap(&s_live.deferred[i],
                first_word, word_count)) {
            gfxRdpTmemLiveFlushOne(&s_live.deferred[i]);
        }
    }
}

/*
 * A command that overwrites [first_word, +word_count) in full replaces the
 * pending words it covers; only the uncovered head/tail of an older block
 * stays pending. Words nobody reads before the next overwrite are never
 * copied.
 */
static void gfxRdpTmemLiveDropCovered(uint32_t first_word, uint32_t word_count)
{
    const uint32_t new_end = first_word + word_count;
    for (uint32_t i = 0; i < GFX_RDP_TMEM_LIVE_DEFERRED; ++i) {
        struct GfxRdpTmemLiveDeferredLoad *old = &s_live.deferred[i];
        if (!gfxRdpTmemLiveDeferredOverlap(old, first_word, word_count)) {
            continue;
        }
        const uint32_t old_end = old->first_word + old->word_count;
        const bool head = old->first_word < first_word;
        const bool tail = old_end > new_end;
        if (!head && !tail) {
            old->active = false;
            ++s_live.stats.load_block_elided;
        } else if (head && tail) {
            /* Split would need a second slot; settle the tail. */
            struct GfxRdpTmemLiveDeferredLoad tail_part = *old;
            tail_part.skip_words += new_end - old->first_word;
            tail_part.first_word = new_end;
            tail_part.word_count = old_end - new_end;
            gfxRdpTmemLiveFlushOne(&tail_part);
            old->word_count = first_word - old->first_word;
        } else if (head) {
            old->word_count = first_word - old->first_word;
        } else {
            old->skip_words += new_end - old->first_word;
            old->word_count = old_end - new_end;
            old->first_word = new_end;
        }
    }
}

static bool gfxRdpTmemLiveAnyDeferred(void)
{
    for (uint32_t i = 0; i < GFX_RDP_TMEM_LIVE_DEFERRED; ++i) {
        if (s_live.deferred[i].active) {
            return true;
        }
    }
    return false;
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

static inline void gfxRdpTmemLiveHashMix(uint64_t *hash,
    uint32_t a, uint32_t b)
{
    gfxRdpTmemHashMix(hash, a, b);
}

static void gfxRdpTmemLiveHashByte(uint64_t *hash, uint8_t value)
{
    gfxRdpTmemLiveHashMix(hash, 0x6b000000u | value, 0x94ffffffu ^ value);
}

static inline uint32_t gfxRdpTmemLiveFmix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static uint64_t gfxRdpTmemLiveHashFinish(uint64_t hash)
{
    uint32_t h1 = (uint32_t)(hash >> 32);
    uint32_t h2 = (uint32_t)hash;
    h1 += h2;
    h2 += h1;
    h1 = gfxRdpTmemLiveFmix32(h1);
    h2 = gfxRdpTmemLiveFmix32(h2);
    h1 += h2;
    h2 += h1;
    return ((uint64_t)h1 << 32) | h2;
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

    const uint8_t *bytes = &tmem->bytes[first_word * GFX_RDP_TMEM_WORD_BYTES];
    const uint8_t *valid = &tmem->byte_valid[first_word * GFX_RDP_TMEM_WORD_BYTES];
    uint32_t counted = 0u;

    for (uint32_t word = 0; word < word_count; ++word,
            bytes += GFX_RDP_TMEM_WORD_BYTES, valid += GFX_RDP_TMEM_WORD_BYTES) {
        uint32_t lo, hi;
        uint64_t valid64;
        memcpy(&valid64, valid, sizeof(valid64));

        /* exact "no zero byte" test: every lane of this word is valid */
        if (((valid64 - UINT64_C(0x0101010101010101)) & ~valid64 &
                UINT64_C(0x8080808080808080)) == 0u) {
            memcpy(&lo, bytes, 4);
            memcpy(&hi, bytes + 4, 4);
            gfxRdpTmemLiveHashMix(hash, lo, hi);
            counted += GFX_RDP_TMEM_WORD_BYTES;
            continue;
        }

        /*
         * Invalid bytes hash as an explicit marker, never as their stale
         * backing value. Conservative invalidation therefore cannot become
         * an accidental cache hit just because old bytes remain in storage.
         */
        uint8_t masked[GFX_RDP_TMEM_WORD_BYTES];
        uint32_t mask = 0u;
        for (uint32_t lane = 0; lane < GFX_RDP_TMEM_WORD_BYTES; ++lane) {
            if (valid[lane] != 0u) {
                masked[lane] = bytes[lane];
                mask |= 1u << lane;
                ++counted;
            } else {
                masked[lane] = 0u;
            }
        }
        memcpy(&lo, masked, 4);
        memcpy(&hi, masked + 4, 4);
        gfxRdpTmemLiveHashMix(hash, lo, hi);
        gfxRdpTmemLiveHashMix(hash, 0x5a5a5a00u | mask, 0xa5a5a5a5u);
    }

    *valid_bytes += counted;
    return true;
}

/*
 * Hash [first_word, +word_count) exactly as gfxRdpTmemLoadBlockLinear /
 * gfxRdpTmemWriteTlut hash the words they store, so an intact load record and
 * a fresh read of the same fully valid words give the same value. Uses the
 * record when one is still intact. Fails if any byte in the range is invalid.
 */
static bool gfxRdpTmemLiveHashValidRange(const struct GfxRdpTmem *tmem,
    uint32_t first_word, uint32_t word_count, uint64_t *range_hash)
{
    const struct GfxRdpTmemLoadRecord *record =
        gfxRdpTmemFindIntactLoad(tmem, first_word, word_count);
    /*
     * Only an exact-range record may stand in for the read: the identity
     * must be a pure function of the range contents, whichever path ran.
     */
    if (record && record->first_word == first_word &&
        record->word_count == word_count) {
        *range_hash = record->hash;
        ++s_live.stats.fingerprint_load_reuse;
        return true;
    }
    if (word_count == 0u || first_word > GFX_RDP_TMEM_WORDS ||
        word_count > GFX_RDP_TMEM_WORDS - first_word) {
        return false;
    }
    uint64_t h = GFX_RDP_TMEM_HASH_SEED;
    const uint8_t *bytes = &tmem->bytes[first_word * GFX_RDP_TMEM_WORD_BYTES];
    const uint8_t *valid = &tmem->byte_valid[first_word * GFX_RDP_TMEM_WORD_BYTES];
    for (uint32_t w = 0; w < word_count; ++w) {
        uint64_t v, b;
        memcpy(&v, valid + w * 8u, 8);
        if (((v - UINT64_C(0x0101010101010101)) & ~v &
                UINT64_C(0x8080808080808080)) != 0u) {
            return false;
        }
        memcpy(&b, bytes + w * 8u, 8);
        gfxRdpTmemHashMix64(&h, b);
    }
    *range_hash = h;
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
    gfxRdpTmemSourceMemoNewEpoch();
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
    /*
     * A well-formed TLUT load overwrites exactly its entry words, so pending
     * block words under it are dead (typically a large texture block that
     * spills into the palette half). Anything else settles what it might
     * touch; a rejected load invalidates all of TMEM below.
     */
    if (tile >= GFX_RDP_TMEM_RUNTIME_TILES) {
        gfxRdpTmemLiveFlush();
    } else {
        const struct GfxRdpTmemRuntimeTile *t = &s_live.runtime.tiles[tile];
        const uint64_t count = (lrs >= uls && lrt >= ult)
            ? (uint64_t)(lrs - uls + 1u) * (lrt - ult + 1u) : 0u;
        if (s_live.runtime.image.valid && s_live.runtime.image.addr &&
                s_live.runtime.image.siz == G_IM_SIZ_16b && t->valid &&
                count != 0u && count <= 256u &&
                t->tmem_word >= GFX_RDP_TMEM_HALF_WORDS &&
                count <= GFX_RDP_TMEM_WORDS - t->tmem_word) {
            gfxRdpTmemLiveDropCovered(t->tmem_word, (uint32_t)count);
        } else {
            gfxRdpTmemLiveFlushOverlapping(t->tmem_word, 256u);
        }
    }
    const enum GfxRdpTmemLoadResult result = gfxRdpTmemRuntimeLoadTlut(
        &s_live.runtime, tile, uls, ult, lrs, lrt);
    if (result != GFX_RDP_TMEM_LOAD_EXACT && gfxRdpTmemLiveAnyDeferred()) {
        /*
         * A rejected TLUT load invalidates all of TMEM. The pending blocks
         * logically preceded it: apply them now, then repeat the invalidation
         * so the observable state matches the eager order.
         */
        gfxRdpTmemLiveFlush();
        (void)gfxRdpTmemInvalidatePhysical(&s_live.runtime.tmem,
            0u, GFX_RDP_TMEM_BYTES);
    }
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
    gfxRdpTmemLiveFlush();
    gfxRdpTmemLiveRecordLoad(gfxRdpTmemRuntimeLoadTile(
        &s_live.runtime, tile, uls, ult, lrs, lrt),
        &s_live.stats.load_tile_exact,
        &s_live.stats.load_tile_conservative);
}

extern "C" void gfxRdpTmemLiveLoadBlock(uint8_t tile,
    uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt)
{
    gfxRdpTmemLiveEnsureInitialized();

    /* Same acceptance rules as gfxRdpTmemRuntimeLoadBlock's linear path. */
    if (tile < GFX_RDP_TMEM_RUNTIME_TILES) {
        const struct GfxRdpTmemRuntimeTile *t = &s_live.runtime.tiles[tile];
        const struct GfxRdpTmemRuntimeImage *image = &s_live.runtime.image;
        const uint64_t size64 = ((uint64_t)lrs + 1u) << image->siz >> 1u;
        const bool rgba32 = t->fmt == G_IM_FMT_RGBA &&
            image->fmt == G_IM_FMT_RGBA &&
            t->siz == G_IM_SIZ_32b && image->siz == G_IM_SIZ_32b;
        if (image->valid && t->valid && uls == 0u && ult == 0u &&
            image->addr != NULL && dxt <= 0x0fffu &&
            t->fmt != G_IM_FMT_YUV && image->fmt != G_IM_FMT_YUV &&
            !rgba32 && size64 != 0u &&
            (size64 % GFX_RDP_TMEM_WORD_BYTES) == 0u &&
            t->tmem_word <= GFX_RDP_TMEM_WORDS &&
            size64 / GFX_RDP_TMEM_WORD_BYTES <=
                GFX_RDP_TMEM_WORDS - t->tmem_word) {
            const uint32_t words =
                (uint32_t)(size64 / GFX_RDP_TMEM_WORD_BYTES);
            gfxRdpTmemLiveDropCovered(t->tmem_word, words);
            struct GfxRdpTmemLiveDeferredLoad *slot = NULL;
            for (uint32_t i = 0; i < GFX_RDP_TMEM_LIVE_DEFERRED; ++i) {
                if (!s_live.deferred[i].active) {
                    slot = &s_live.deferred[i];
                    break;
                }
            }
            if (!slot) {
                slot = &s_live.deferred[
                    s_live.deferred_next++ % GFX_RDP_TMEM_LIVE_DEFERRED];
                gfxRdpTmemLiveFlushOne(slot);
            }
            slot->active = true;
            slot->first_word = t->tmem_word;
            slot->word_count = words;
            slot->source = (const uint8_t *)image->addr;
            slot->skip_words = 0u;
            slot->hash_valid = false;
            slot->dxt = (uint16_t)dxt;
            ++s_live.stats.load_block_deferred;
            ++s_live.stats.load_block_exact;
            return;
        }
    }

    gfxRdpTmemLiveFlush();
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
    bool hashed = false;

    /*
     * Fast path for the common fully valid, non-split case: the rendered rows
     * are one contiguous word range. Its identity is the word-sequence hash
     * that the loader already computed (intact load record), or the same
     * hash recomputed from TMEM. Partially valid ranges keep the masked
     * per-row hash below (domain-separated).
     */
    if (gfxRdpTmemLiveAnyDeferred()) {
        const uint64_t total64 = (uint64_t)row_count * tile->line_words;
        struct GfxRdpTmemLiveDeferredLoad *pending = NULL;
        if (!split_rgba32 && total64 != 0u &&
            total64 * GFX_RDP_TMEM_WORD_BYTES >= loaded_size_bytes) {
            for (uint32_t i = 0; i < GFX_RDP_TMEM_LIVE_DEFERRED; ++i) {
                struct GfxRdpTmemLiveDeferredLoad *load =
                    &s_live.deferred[i];
                if (load->active && tile->tmem_word >= load->first_word &&
                    tile->tmem_word + total64 <=
                        (uint64_t)load->first_word + load->word_count) {
                    pending = load;
                    break;
                }
            }
        }
        if (pending) {
            /*
             * The texture lies inside a pending block: hash those words
             * straight from the source. Same value as hashing them from
             * TMEM after the load, so identity does not depend on timing.
             */
            const uint32_t block_skip = pending->skip_words +
                (tile->tmem_word - pending->first_word);
            if (!pending->hash_valid || pending->hash_skip != block_skip ||
                pending->hash_count != (uint32_t)total64) {
                pending->hash = gfxRdpTmemHashBlockSource(pending->source,
                    block_skip, (uint32_t)total64, pending->dxt);
                pending->hash_skip = block_skip;
                pending->hash_count = (uint32_t)total64;
                pending->hash_valid = true;
            }
            const uint64_t range_hash = pending->hash;
            hash = UINT64_C(0x3c6ef372a54ff53a);
            gfxRdpTmemHashMix64(&hash, range_hash);
            /* dxt == 0 blocks read back without the odd-row swap */
            gfxRdpTmemHashMix(&hash, (uint32_t)total64,
                tile->line_words | (pending->dxt == 0u ? 0x80000000u : 0u));
            hashed = true;
            ++s_live.stats.fingerprint_source_hash;
        } else {
            /* Rows may be scattered; settle everything they could read. */
            gfxRdpTmemLiveFlush();
        }
    }

    if (!hashed && !split_rgba32) {
        const uint64_t total64 = (uint64_t)row_count * tile->line_words;
        uint64_t range_hash;
        if (total64 <= GFX_RDP_TMEM_WORDS &&
            total64 * GFX_RDP_TMEM_WORD_BYTES >= loaded_size_bytes &&
            gfxRdpTmemLiveHashValidRange(tmem, tile->tmem_word,
                (uint32_t)total64, &range_hash)) {
            hash = UINT64_C(0x3c6ef372a54ff53a);
            gfxRdpTmemHashMix64(&hash, range_hash);
            gfxRdpTmemHashMix(&hash, (uint32_t)total64,
                tile->line_words |
                (tmem->word_linear[tile->tmem_word] ? 0x80000000u : 0u));
            hashed = true;
        }
    }

    uint32_t valid_texture_bytes = 0u;
    if (!hashed && (!gfxRdpTmemLiveHashTextureRows(tile, tmem,
            row_count, split_rgba32, &hash, &valid_texture_bytes) ||
        valid_texture_bytes < loaded_size_bytes)) {
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

        gfxRdpTmemLiveFlushOverlapping(first_palette_word, palette_words);
        uint64_t palette_hash;
        if (gfxRdpTmemLiveHashValidRange(tmem, first_palette_word,
                palette_words, &palette_hash)) {
            gfxRdpTmemHashMix(&hash, 0x7a1e7a1eu, palette_words);
            gfxRdpTmemHashMix64(&hash, palette_hash);
        } else {
            uint32_t valid_palette_bytes = 0u;
            if (!gfxRdpTmemLiveHashWords(tmem, first_palette_word,
                    palette_words, &hash, &valid_palette_bytes) ||
                valid_palette_bytes < palette_words * GFX_RDP_TMEM_WORD_BYTES) {
                ++s_live.stats.fingerprint_fallback;
                return false;
            }
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

    *fingerprint = gfxRdpTmemLiveHashFinish(hash);
    ++s_live.stats.fingerprint_exact;
    return true;
}

extern "C" bool gfxRdpTmemLiveIdentifyTexture(uint8_t tile_index,
    uint32_t loaded_line_size_bytes, uint32_t loaded_size_bytes,
    uint8_t render_fmt, uint8_t render_siz, uint8_t palette_index,
    struct GfxRdpTmemLiveTextureView *view)
{
    gfxRdpTmemLiveEnsureInitialized();

    if (!view) {
        ++s_live.stats.materialize_fallback;
        return false;
    }
    memset(view, 0, sizeof(*view));

    uint64_t content_identity = 0u;
    if (tile_index >= GFX_RDP_TMEM_RUNTIME_TILES ||
        loaded_line_size_bytes == 0u || loaded_size_bytes == 0u ||
        loaded_size_bytes > sizeof(s_live.materialized_texels) ||
        !gfxRdpTmemLiveTextureFingerprint(tile_index,
            loaded_line_size_bytes, loaded_size_bytes,
            render_fmt, render_siz, palette_index, &content_identity)) {
        ++s_live.stats.materialize_fallback;
        return false;
    }

    const struct GfxRdpTmemRuntimeTile *tile =
        &s_live.runtime.tiles[tile_index];
    const bool split_rgba32 =
        tile->fmt == G_IM_FMT_RGBA && tile->siz == G_IM_SIZ_32b;
    const uint32_t render_line_size_bytes =
        (uint32_t)tile->line_words * (split_rgba32 ? 16u : 8u);
    const uint32_t logical_line_size_bytes =
        loaded_line_size_bytes < render_line_size_bytes
            ? loaded_line_size_bytes : render_line_size_bytes;

    uint16_t palette_first = 0u;
    uint16_t palette_count = 0u;
    if (render_fmt == G_IM_FMT_CI) {
        if (render_siz == G_IM_SIZ_4b && palette_index < 16u) {
            palette_first = (uint16_t)((uint16_t)palette_index * 16u);
            palette_count = 16u;
        } else if (render_siz == G_IM_SIZ_8b) {
            palette_count = 256u;
        } else {
            ++s_live.stats.materialize_fallback;
            return false;
        }
    }

    view->texels = NULL;
    view->size_bytes = loaded_size_bytes;
    view->line_size_bytes = logical_line_size_bytes;
    view->palette = NULL;
    view->palette_first = palette_first;
    view->palette_count = palette_count;
    view->content_identity = content_identity;
    return true;
}

extern "C" bool gfxRdpTmemLiveFillTextureView(uint8_t tile_index,
    struct GfxRdpTmemLiveTextureView *view)
{
    gfxRdpTmemLiveEnsureInitialized();

    /*
     * Settle only the pending blocks this view reads (its rows and its
     * palette). Pending blocks are pairwise disjoint and disjoint from what
     * is settled here, so the rest can stay lazy: a later block that covers
     * them still elides the copy, and the final TMEM state is unchanged.
     */
    if (gfxRdpTmemLiveAnyDeferred()) {
        bool targeted = false;
        if (view && tile_index < GFX_RDP_TMEM_RUNTIME_TILES &&
                view->line_size_bytes != 0u && view->size_bytes != 0u &&
                view->size_bytes <= GFX_RDP_TMEM_BYTES) {
            const struct GfxRdpTmemRuntimeTile *t =
                &s_live.runtime.tiles[tile_index];
            const bool rgba32 =
                t->fmt == G_IM_FMT_RGBA && t->siz == G_IM_SIZ_32b;
            const uint64_t rows = ((uint64_t)view->size_bytes +
                view->line_size_bytes - 1u) / view->line_size_bytes;
            const uint64_t row_words = ((uint64_t)view->line_size_bytes +
                GFX_RDP_TMEM_WORD_BYTES - 1u) / GFX_RDP_TMEM_WORD_BYTES;
            const uint64_t span = (rows - 1u) * t->line_words + row_words;
            if (!rgba32 && span != 0u &&
                    (uint64_t)t->tmem_word + span <= GFX_RDP_TMEM_WORDS) {
                gfxRdpTmemLiveFlushOverlapping(t->tmem_word, (uint32_t)span);
                if (view->palette_count != 0u) {
                    const uint32_t first_palette_word =
                        view->palette_count == 16u
                        ? GFX_RDP_TMEM_HALF_WORDS + view->palette_first
                        : GFX_RDP_TMEM_HALF_WORDS;
                    gfxRdpTmemLiveFlushOverlapping(first_palette_word,
                        view->palette_count);
                }
                targeted = true;
            }
        }
        if (!targeted) {
            gfxRdpTmemLiveFlush();
        }
    }
    if (!view || tile_index >= GFX_RDP_TMEM_RUNTIME_TILES ||
        view->size_bytes == 0u ||
        view->size_bytes > sizeof(s_live.materialized_texels)) {
        ++s_live.stats.materialize_fallback;
        return false;
    }

    const struct GfxRdpTmemRuntimeTile *tile =
        &s_live.runtime.tiles[tile_index];
    const struct GfxRdpTmem *tmem = &s_live.runtime.tmem;
    const bool split_rgba32 =
        tile->fmt == G_IM_FMT_RGBA && tile->siz == G_IM_SIZ_32b;
    const uint32_t logical_line_size_bytes = view->line_size_bytes;
    const uint32_t loaded_size_bytes = view->size_bytes;

    bool exact;
    if (split_rgba32) {
        exact = (logical_line_size_bytes & 3u) == 0u &&
            (loaded_size_bytes & 3u) == 0u &&
            gfxRdpTmemReadTileRgba32Texels(tmem,
                tile->tmem_word, tile->line_words,
                s_live.materialized_texels,
                logical_line_size_bytes / 4u,
                loaded_size_bytes / 4u);
    } else {
        exact = gfxRdpTmemReadTileLinearBytes(tmem,
            tile->tmem_word, tile->line_words,
            s_live.materialized_texels,
            logical_line_size_bytes, loaded_size_bytes);
    }

    if (!exact) {
        ++s_live.stats.materialize_fallback;
        return false;
    }

    if (view->palette_count != 0u) {
        const uint32_t first_palette_word = view->palette_count == 16u
            ? GFX_RDP_TMEM_HALF_WORDS + view->palette_first
            : GFX_RDP_TMEM_HALF_WORDS;
        if (!gfxRdpTmemReadTlut(tmem, first_palette_word,
                s_live.materialized_palette, view->palette_count)) {
            ++s_live.stats.materialize_fallback;
            return false;
        }
    }

    view->texels = s_live.materialized_texels;
    view->palette = s_live.materialized_palette;
    ++s_live.stats.materialize_exact;
    return true;
}

extern "C" bool gfxRdpTmemLiveMaterializeTexture(uint8_t tile_index,
    uint32_t loaded_line_size_bytes, uint32_t loaded_size_bytes,
    uint8_t render_fmt, uint8_t render_siz, uint8_t palette_index,
    struct GfxRdpTmemLiveTextureView *view)
{
    if (!gfxRdpTmemLiveIdentifyTexture(tile_index,
            loaded_line_size_bytes, loaded_size_bytes,
            render_fmt, render_siz, palette_index, view)) {
        return false;
    }
    if (!gfxRdpTmemLiveFillTextureView(tile_index, view)) {
        memset(view, 0, sizeof(*view));
        return false;
    }
    return true;
}

extern "C" const struct GfxRdpTmem *gfxRdpTmemLiveState(void)
{
    gfxRdpTmemLiveEnsureInitialized();
    gfxRdpTmemLiveFlush();
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
