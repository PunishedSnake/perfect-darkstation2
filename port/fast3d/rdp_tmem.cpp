#include "rdp_tmem.h"
#include "rdp_tmem_hash.h"

#include <string.h>

static uint32_t gfxRdpTmemNextGeneration(struct GfxRdpTmem *tmem)
{
    uint32_t next = tmem->generation + 1u;

    /* Keep zero reserved as the reset/unwritten generation after wraparound. */
    if (next == 0u) {
        next = 1u;
        for (uint32_t i = 0; i < GFX_RDP_TMEM_WORDS; ++i) {
            tmem->word_generation[i] = 0u;
        }
        memset(tmem->load_records, 0, sizeof(tmem->load_records));
    }

    tmem->generation = next;
    return next;
}

static bool gfxRdpTmemRangeValid(uint32_t byte_offset, uint32_t size_bytes)
{
    return byte_offset <= GFX_RDP_TMEM_BYTES &&
           size_bytes <= GFX_RDP_TMEM_BYTES - byte_offset;
}

static const uint64_t k_gfx_rdp_tmem_ones = UINT64_C(0x0101010101010101);

static inline uint64_t gfxRdpTmemLoad64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* True when none of the eight bytes of v is zero (exact, branch-free). */
static inline bool gfxRdpTmemAllBytesNonZero(uint64_t v)
{
    return ((v - k_gfx_rdp_tmem_ones) & ~v & UINT64_C(0x8080808080808080)) == 0u;
}

static void gfxRdpTmemRefreshWordValid(struct GfxRdpTmem *tmem, uint32_t word)
{
    const uint32_t first_byte = word * GFX_RDP_TMEM_WORD_BYTES;
    tmem->word_valid[word] = gfxRdpTmemAllBytesNonZero(
        gfxRdpTmemLoad64(&tmem->byte_valid[first_byte])) ? 1u : 0u;
}

static void gfxRdpTmemRecordLoad(struct GfxRdpTmem *tmem,
    uint32_t first_word, uint32_t word_count, uint32_t generation,
    uint64_t hash)
{
    struct GfxRdpTmemLoadRecord *record =
        &tmem->load_records[tmem->load_record_next % GFX_RDP_TMEM_LOAD_RECORDS];
    tmem->load_record_next =
        (tmem->load_record_next + 1u) % GFX_RDP_TMEM_LOAD_RECORDS;
    record->first_word = first_word;
    record->word_count = word_count;
    record->generation = generation;
    record->hash = hash;
}

extern "C" const struct GfxRdpTmemLoadRecord *gfxRdpTmemFindIntactLoad(
    const struct GfxRdpTmem *tmem, uint32_t first_word, uint32_t word_count)
{
    if (!tmem || word_count == 0u || first_word > GFX_RDP_TMEM_WORDS ||
        word_count > GFX_RDP_TMEM_WORDS - first_word) {
        return NULL;
    }
    for (uint32_t i = 0; i < GFX_RDP_TMEM_LOAD_RECORDS; ++i) {
        const struct GfxRdpTmemLoadRecord *record = &tmem->load_records[i];
        if (record->generation == 0u || first_word < record->first_word ||
            first_word + word_count > record->first_word + record->word_count) {
            continue;
        }
        const uint32_t *gen = &tmem->word_generation[first_word];
        uint32_t w = 0;
        while (w < word_count && gen[w] == record->generation) {
            ++w;
        }
        if (w == word_count) {
            return record;
        }
    }
    return NULL;
}

/*
 * Store one complete 64-bit TMEM word. Equivalent to eight
 * gfxRdpTmemWriteMappedByte calls (mapped = swap ? i ^ 4 : i) followed by
 * gfxRdpTmemMarkOneWord, without the per-byte bookkeeping.
 */
static inline void gfxRdpTmemStoreFullWord(struct GfxRdpTmem *tmem,
    uint32_t dst_word, const uint8_t *src, bool swap_halves,
    uint32_t generation)
{
    uint8_t *dst = &tmem->bytes[dst_word * GFX_RDP_TMEM_WORD_BYTES];
    if (swap_halves) {
        memcpy(dst, src + 4, 4);
        memcpy(dst + 4, src, 4);
    } else {
        memcpy(dst, src, 8);
    }
    memcpy(&tmem->byte_valid[dst_word * GFX_RDP_TMEM_WORD_BYTES],
        &k_gfx_rdp_tmem_ones, sizeof(k_gfx_rdp_tmem_ones));
    tmem->word_valid[dst_word] = 1u;
    tmem->word_linear[dst_word] = 0u;
    tmem->word_generation[dst_word] = generation;
}

static void gfxRdpTmemSetByteValidity(struct GfxRdpTmem *tmem,
    uint32_t byte_offset, uint32_t size_bytes, uint8_t valid)
{
    if (size_bytes == 0u) {
        return;
    }

    memset(&tmem->byte_valid[byte_offset], valid ? 1 : 0, size_bytes);

    const uint32_t first_word = byte_offset / GFX_RDP_TMEM_WORD_BYTES;
    const uint32_t last_word =
        (byte_offset + size_bytes - 1u) / GFX_RDP_TMEM_WORD_BYTES;
    for (uint32_t word = first_word; word <= last_word; ++word) {
        gfxRdpTmemRefreshWordValid(tmem, word);
    }
}

static void gfxRdpTmemMarkRange(struct GfxRdpTmem *tmem,
    uint32_t byte_offset, uint32_t size_bytes)
{
    if (size_bytes == 0u) {
        return;
    }

    const uint32_t generation = gfxRdpTmemNextGeneration(tmem);
    const uint32_t first_word = byte_offset / GFX_RDP_TMEM_WORD_BYTES;
    const uint32_t last_word =
        (byte_offset + size_bytes - 1u) / GFX_RDP_TMEM_WORD_BYTES;

    for (uint32_t word = first_word; word <= last_word; ++word) {
        tmem->word_generation[word] = generation;
    }
}

static void gfxRdpTmemMarkOneWord(struct GfxRdpTmem *tmem,
    uint32_t word, uint32_t generation)
{
    tmem->word_generation[word] = generation;
    tmem->word_linear[word] = 0u;
    gfxRdpTmemRefreshWordValid(tmem, word);
}

static void gfxRdpTmemWriteMappedByte(struct GfxRdpTmem *tmem,
    uint32_t dst_byte, uint8_t value)
{
    tmem->bytes[dst_byte] = value;
    tmem->byte_valid[dst_byte] = 1u;
}

static void gfxRdpTmemPreparePartialWord(struct GfxRdpTmem *tmem,
    uint32_t dst_word, uint32_t copy_bytes)
{
    if (copy_bytes >= GFX_RDP_TMEM_WORD_BYTES) {
        return;
    }

    const uint32_t dst_byte = dst_word * GFX_RDP_TMEM_WORD_BYTES;
    memset(&tmem->byte_valid[dst_byte], 0, GFX_RDP_TMEM_WORD_BYTES);
    tmem->word_valid[dst_word] = 0u;
}

static bool gfxRdpTmemTileRangeValid(uint32_t first_tmem_word,
    uint32_t line_words, uint32_t row_words, uint32_t row_count,
    uint32_t word_limit)
{
    if (first_tmem_word > word_limit || row_words > word_limit) {
        return false;
    }
    if (row_count == 0u || row_words == 0u) {
        return true;
    }
    if (row_count > 1u && line_words == 0u) {
        return false;
    }
    if (row_words > line_words && row_count > 1u) {
        return false;
    }

    const uint32_t row_advance = (row_count - 1u) * line_words;
    if (row_advance > word_limit - first_tmem_word) {
        return false;
    }

    const uint32_t last_row_word = first_tmem_word + row_advance;
    return row_words <= word_limit - last_row_word;
}

extern "C" void gfxRdpTmemReset(struct GfxRdpTmem *tmem)
{
    if (!tmem) {
        return;
    }

    memset(tmem, 0, sizeof(*tmem));
}

extern "C" bool gfxRdpTmemWritePhysical(struct GfxRdpTmem *tmem,
    uint32_t byte_offset, const void *src, uint32_t size_bytes)
{
    if (!tmem || (!src && size_bytes != 0u) ||
        !gfxRdpTmemRangeValid(byte_offset, size_bytes)) {
        return false;
    }

    if (size_bytes == 0u) {
        return true;
    }

    memcpy(&tmem->bytes[byte_offset], src, size_bytes);
    gfxRdpTmemSetByteValidity(tmem, byte_offset, size_bytes, 1u);
    gfxRdpTmemMarkRange(tmem, byte_offset, size_bytes);
    return true;
}

extern "C" bool gfxRdpTmemInvalidatePhysical(struct GfxRdpTmem *tmem,
    uint32_t byte_offset, uint32_t size_bytes)
{
    if (!tmem || !gfxRdpTmemRangeValid(byte_offset, size_bytes)) {
        return false;
    }

    if (size_bytes == 0u) {
        return true;
    }

    gfxRdpTmemSetByteValidity(tmem, byte_offset, size_bytes, 0u);
    gfxRdpTmemMarkRange(tmem, byte_offset, size_bytes);
    return true;
}

extern "C" bool gfxRdpTmemWriteTlut(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, const uint16_t *entries, uint32_t entry_count)
{
    if (!tmem || (!entries && entry_count != 0u) ||
        first_tmem_word > GFX_RDP_TMEM_WORDS ||
        entry_count > GFX_RDP_TMEM_WORDS - first_tmem_word) {
        return false;
    }

    if (entry_count == 0u) {
        return true;
    }

    /*
     * Reloading the palette TMEM already holds (the common case: the same
     * TLUT before every draw that uses it) changes nothing observable, as
     * for identical LoadBlocks: keep generations and load records intact.
     * Each entry fills its word as four copies of hi,lo; compare against that
     * pattern directly and stop at the first difference.
     */
    uint32_t same = 0u;
    for (; same < entry_count; ++same) {
        const uint32_t word = first_tmem_word + same;
        /* shifts, not a 64-bit multiply: the R5900 has no DMULT and the
         * multiply became a libgcc call per entry */
        const uint32_t half = ((uint32_t)(entries[same] >> 8) & 0xffu) |
            (((uint32_t)entries[same] & 0xffu) << 8);
        const uint32_t pair = half | (half << 16);
        const uint64_t pattern = (uint64_t)pair | ((uint64_t)pair << 32);
        if (tmem->word_valid[word] == 0u || tmem->word_linear[word] != 0u ||
            gfxRdpTmemLoad64(&tmem->bytes[word * GFX_RDP_TMEM_WORD_BYTES]) != pattern) {
            break;
        }
    }
    if (same == entry_count) {
        return true;
    }

    /* One pass: each word is four copies of hi,lo (built in a register,
     * stored straight into TMEM) with full validity and a fresh generation. */
    const uint32_t generation = gfxRdpTmemNextGeneration(tmem);
    uint64_t load_hash = GFX_RDP_TMEM_HASH_SEED;
    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint32_t word = first_tmem_word + i;
        const uint32_t half = ((uint32_t)(entries[i] >> 8) & 0xffu) |
            (((uint32_t)entries[i] & 0xffu) << 8);
        const uint32_t pair = half | (half << 16);
        const uint64_t pattern = (uint64_t)pair | ((uint64_t)pair << 32);
        memcpy(&tmem->bytes[word * GFX_RDP_TMEM_WORD_BYTES], &pattern, 8);
        memcpy(&tmem->byte_valid[word * GFX_RDP_TMEM_WORD_BYTES],
            &k_gfx_rdp_tmem_ones, sizeof(k_gfx_rdp_tmem_ones));
        tmem->word_valid[word] = 1u;
        tmem->word_linear[word] = 0u;
        tmem->word_generation[word] = generation;
        gfxRdpTmemHashMix64(&load_hash, pattern);
    }

    gfxRdpTmemRecordLoad(tmem, first_tmem_word, entry_count,
        generation, load_hash);
    return true;
}

extern "C" bool gfxRdpTmemLoadTileLinear(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint32_t line_words,
    const uint8_t *source, uint32_t source_stride_bytes,
    uint32_t row_bytes, uint32_t row_count)
{
    if (!tmem || (!source && row_bytes != 0u && row_count != 0u) ||
        source_stride_bytes < row_bytes) {
        return false;
    }

    if (row_bytes == 0u || row_count == 0u) {
        return true;
    }

    const uint32_t row_words =
        (row_bytes + GFX_RDP_TMEM_WORD_BYTES - 1u) / GFX_RDP_TMEM_WORD_BYTES;
    if (!gfxRdpTmemTileRangeValid(first_tmem_word, line_words,
            row_words, row_count, GFX_RDP_TMEM_WORDS)) {
        return false;
    }

    const uint32_t generation = gfxRdpTmemNextGeneration(tmem);

    for (uint32_t row = 0; row < row_count; ++row) {
        const uint8_t *src_row = source + (size_t)row * source_stride_bytes;
        const uint32_t dst_row_word = first_tmem_word + row * line_words;

        for (uint32_t word = 0; word < row_words; ++word) {
            const uint32_t src_offset = word * GFX_RDP_TMEM_WORD_BYTES;
            uint32_t copy_bytes = row_bytes - src_offset;
            if (copy_bytes > GFX_RDP_TMEM_WORD_BYTES) {
                copy_bytes = GFX_RDP_TMEM_WORD_BYTES;
            }

            const uint32_t dst_word = dst_row_word + word;
            const uint32_t dst_byte = dst_word * GFX_RDP_TMEM_WORD_BYTES;
            const uint8_t *src = src_row + src_offset;

            if (copy_bytes == GFX_RDP_TMEM_WORD_BYTES) {
                gfxRdpTmemStoreFullWord(tmem, dst_word, src,
                    (row & 1u) != 0u, generation);
                continue;
            }

            gfxRdpTmemPreparePartialWord(tmem, dst_word, copy_bytes);
            for (uint32_t i = 0; i < copy_bytes; ++i) {
                const uint32_t mapped = (row & 1u) ? (i ^ 4u) : i;
                gfxRdpTmemWriteMappedByte(tmem, dst_byte + mapped, src[i]);
            }
            gfxRdpTmemMarkOneWord(tmem, dst_word, generation);
        }
    }

    return true;
}

extern "C" bool gfxRdpTmemLoadTileRgba32(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint32_t line_words,
    const uint8_t *source, uint32_t source_stride_bytes,
    uint32_t texels_per_row, uint32_t row_count)
{
    const uint32_t source_row_bytes = texels_per_row * 4u;
    if (!tmem || (!source && texels_per_row != 0u && row_count != 0u) ||
        source_stride_bytes < source_row_bytes) {
        return false;
    }

    if (texels_per_row == 0u || row_count == 0u) {
        return true;
    }

    const uint32_t row_half_bytes = texels_per_row * 2u;
    const uint32_t row_words =
        (row_half_bytes + GFX_RDP_TMEM_WORD_BYTES - 1u) / GFX_RDP_TMEM_WORD_BYTES;
    if (!gfxRdpTmemTileRangeValid(first_tmem_word, line_words,
            row_words, row_count, GFX_RDP_TMEM_HALF_WORDS)) {
        return false;
    }

    const uint32_t generation = gfxRdpTmemNextGeneration(tmem);

    for (uint32_t row = 0; row < row_count; ++row) {
        const uint8_t *src_row = source + (size_t)row * source_stride_bytes;
        const uint32_t low_row_word = first_tmem_word + row * line_words;

        for (uint32_t group = 0; group < row_words; ++group) {
            const uint32_t first_texel = group * 4u;
            uint32_t group_texels = texels_per_row - first_texel;
            if (group_texels > 4u) {
                group_texels = 4u;
            }

            const uint32_t low_word = low_row_word + group;
            const uint32_t high_word = low_word + GFX_RDP_TMEM_HALF_WORDS;
            const uint32_t exact_half_bytes = group_texels * 2u;
            gfxRdpTmemPreparePartialWord(tmem, low_word, exact_half_bytes);
            gfxRdpTmemPreparePartialWord(tmem, high_word, exact_half_bytes);

            for (uint32_t lane = 0; lane < group_texels; ++lane) {
                const uint32_t texel = first_texel + lane;
                const uint8_t *src = src_row + texel * 4u;
                const uint32_t mapped_lane = (row & 1u) ? (lane ^ 2u) : lane;
                const uint32_t low_byte =
                    low_word * GFX_RDP_TMEM_WORD_BYTES + mapped_lane * 2u;
                const uint32_t high_byte =
                    high_word * GFX_RDP_TMEM_WORD_BYTES + mapped_lane * 2u;

                gfxRdpTmemWriteMappedByte(tmem, low_byte, src[0]);
                gfxRdpTmemWriteMappedByte(tmem, low_byte + 1u, src[1]);
                gfxRdpTmemWriteMappedByte(tmem, high_byte, src[2]);
                gfxRdpTmemWriteMappedByte(tmem, high_byte + 1u, src[3]);
            }

            gfxRdpTmemMarkOneWord(tmem, low_word, generation);
            gfxRdpTmemMarkOneWord(tmem, high_word, generation);
        }
    }

    return true;
}

static uint64_t gfxRdpTmemHashBlockSourceUncached(const uint8_t *source,
    uint32_t first_word, uint32_t word_count, uint16_t dxt);

/*
 * Memo for gfxRdpTmemHashBlockSource. The same static textures are loaded and
 * fingerprinted many times per frame; the serial hash chain costs far more
 * than an exact compare against a retained copy of the bytes. A hit requires
 * byte-identical source content (or a compare already made in the current
 * source epoch), so results are exactly those of the uncached hash.
 *
 * Entries live in a small hash table (two probes) and keep their byte copy in
 * a ring arena sized to the texture, so the per-frame working set (a few
 * hundred block sub-ranges) fits without a linear search.
 */
enum {
    GFX_RDP_TMEM_SOURCE_MEMO_SLOTS = 512,
    GFX_RDP_TMEM_SOURCE_MEMO_ARENA_WORDS = 32768, /* 256 KiB */
};

static struct {
    const uint8_t *source;
    uint32_t word_count;
    uint16_t dxt;
    uint16_t t_phase;
    bool valid;
    uint32_t verified_epoch; /* source bytes compared equal in this epoch */
    uint32_t arena_word;     /* start of the byte copy in the arena ring */
    uint64_t arena_written;  /* arena write counter when the copy was made */
    uint64_t hash;
} s_source_memo[GFX_RDP_TMEM_SOURCE_MEMO_SLOTS];
static uint64_t s_source_memo_arena[GFX_RDP_TMEM_SOURCE_MEMO_ARENA_WORDS]
    __attribute__((aligned(16)));
static uint64_t s_source_memo_arena_written; /* total words ever allocated */
static uint32_t s_source_memo_flip;

/*
 * Source epochs: the owner of the display-list run (gfx_run on PS2) starts a
 * new epoch whenever source memory may have changed. Within one epoch the
 * game does not run, so a source verified once stays verified; this is the
 * same assumption the lazy LoadBlock deferral already makes when it reads
 * source memory at the end of the run instead of at LoadBlock time.
 */
static uint32_t s_source_epoch = 1u;

extern "C" void gfxRdpTmemSourceMemoNewEpoch(void)
{
    if (++s_source_epoch == 0u) {
        s_source_epoch = 1u;
        for (uint32_t i = 0u; i < GFX_RDP_TMEM_SOURCE_MEMO_SLOTS; ++i) {
            s_source_memo[i].verified_epoch = 0u;
        }
    }
}

static bool gfxRdpTmemSourceEqual(const uint8_t *a, const uint64_t *b,
    uint32_t words)
{
#if defined(_EE)
    /* Both sides 16-byte aligned (the arena keeps copies quadword aligned):
     * compare two TMEM words per quadword load. */
    if ((((uintptr_t)a | (uintptr_t)b) & 15u) == 0u && words >= 2u) {
        uint32_t n = words >> 1;
        const uint8_t *pa = a;
        const uint64_t *pb = b;
        uint32_t differ;
        __asm__ __volatile__(
            ".set    push\n\t"
            ".set    noreorder\n\t"
            "por     $10, $0, $0\n\t"
            "1:\n\t"
            "lq      $8, 0(%1)\n\t"
            "lq      $9, 0(%2)\n\t"
            "addiu   %3, %3, -1\n\t"
            "addiu   %1, %1, 16\n\t"
            "pxor    $8, $8, $9\n\t"
            "addiu   %2, %2, 16\n\t"
            "bgtz    %3, 1b\n\t"
            "por     $10, $10, $8\n\t"   /* delay slot */
            "pcpyud  $8, $10, $0\n\t"
            "or      $10, $10, $8\n\t"
            "sltu    %0, $0, $10\n\t"
            ".set    pop\n\t"
            : "=r"(differ), "+r"(pa), "+r"(pb), "+r"(n)
            :
            : "$8", "$9", "$10", "memory");
        if (differ) {
            return false;
        }
        if ((words & 1u) == 0u) {
            return true;
        }
        return gfxRdpTmemLoad64(pa) == *pb;
    }
#endif
    uint64_t diff = 0u;
    for (uint32_t i = 0u; i < words; ++i) {
        diff |= gfxRdpTmemLoad64(a + i * GFX_RDP_TMEM_WORD_BYTES) ^ b[i];
        if ((i & 15u) == 15u && diff != 0u) {
            return false;
        }
    }
    return diff == 0u;
}

/* The arena copy of an entry is intact until the ring laps it. */
static const uint64_t *gfxRdpTmemSourceMemoBytes(uint32_t slot)
{
    const uint64_t age = s_source_memo_arena_written -
        s_source_memo[slot].arena_written;
    if (age + s_source_memo[slot].word_count >
            GFX_RDP_TMEM_SOURCE_MEMO_ARENA_WORDS) {
        return NULL;
    }
    return &s_source_memo_arena[s_source_memo[slot].arena_word];
}

static void gfxRdpTmemSourceMemoStore(uint32_t slot, const uint8_t *start,
    uint32_t word_count)
{
    /* Keep copies quadword aligned for the MMI compare. */
    s_source_memo_arena_written = (s_source_memo_arena_written + 1u) & ~(uint64_t)1u;
    uint32_t at = (uint32_t)(s_source_memo_arena_written %
        GFX_RDP_TMEM_SOURCE_MEMO_ARENA_WORDS);
    if (at + word_count > GFX_RDP_TMEM_SOURCE_MEMO_ARENA_WORDS) {
        /* Never split a copy: skip to the start of the ring. */
        s_source_memo_arena_written += GFX_RDP_TMEM_SOURCE_MEMO_ARENA_WORDS - at;
        at = 0u;
    }
    memcpy(&s_source_memo_arena[at], start,
        word_count * GFX_RDP_TMEM_WORD_BYTES);
    s_source_memo[slot].arena_word = at;
    s_source_memo[slot].arena_written = s_source_memo_arena_written;
    s_source_memo_arena_written += word_count;
}

extern "C" uint64_t gfxRdpTmemHashBlockSource(const uint8_t *source,
    uint32_t first_word, uint32_t word_count, uint16_t dxt)
{
    if (!source || word_count == 0u || word_count > GFX_RDP_TMEM_WORDS ||
        first_word > 0xffffu) {
        return gfxRdpTmemHashBlockSourceUncached(
            source, first_word, word_count, dxt);
    }
    /* Only the odd-line phase of t at the first word matters per word. */
    const uint8_t *start = source + first_word * GFX_RDP_TMEM_WORD_BYTES;
    const uint16_t t_phase = (uint16_t)((first_word * (uint32_t)dxt) & 0xfffu);
    uint32_t key = (uint32_t)(uintptr_t)start * 0x9e3779b1u;
    key ^= (word_count * 0x85ebca6bu) ^ ((uint32_t)dxt << 16) ^ t_phase;
    key ^= key >> 15;
    const uint32_t base = (key * 0xc2b2ae35u) >> 23; /* 0..511 */
    uint32_t victim = base;
    for (uint32_t probe = 0u; probe < 2u; ++probe) {
        const uint32_t i = (base + probe) & (GFX_RDP_TMEM_SOURCE_MEMO_SLOTS - 1u);
        if (s_source_memo[i].valid && s_source_memo[i].source == start &&
            s_source_memo[i].word_count == word_count &&
            s_source_memo[i].dxt == dxt &&
            s_source_memo[i].t_phase == t_phase) {
            if (s_source_memo[i].verified_epoch == s_source_epoch) {
                return s_source_memo[i].hash;
            }
            const uint64_t *copy = gfxRdpTmemSourceMemoBytes(i);
            if (copy && gfxRdpTmemSourceEqual(start, copy, word_count)) {
                s_source_memo[i].verified_epoch = s_source_epoch;
                return s_source_memo[i].hash;
            }
            victim = i;
            goto refresh;
        }
        if (!s_source_memo[i].valid) {
            victim = i;
        }
    }
    if (s_source_memo[victim].valid) {
        /* Both probes taken: alternate which one is replaced. */
        victim = (base + (s_source_memo_flip++ & 1u)) &
            (GFX_RDP_TMEM_SOURCE_MEMO_SLOTS - 1u);
    }
refresh:
    {
        const uint64_t hash = gfxRdpTmemHashBlockSourceUncached(
            source, first_word, word_count, dxt);
        gfxRdpTmemSourceMemoStore(victim, start, word_count);
        s_source_memo[victim].source = start;
        s_source_memo[victim].word_count = word_count;
        s_source_memo[victim].dxt = dxt;
        s_source_memo[victim].t_phase = t_phase;
        s_source_memo[victim].hash = hash;
        s_source_memo[victim].valid = true;
        s_source_memo[victim].verified_epoch = s_source_epoch;
        return hash;
    }
}

static uint64_t gfxRdpTmemHashBlockSourceUncached(const uint8_t *source,
    uint32_t first_word, uint32_t word_count, uint16_t dxt)
{
    uint64_t hash = GFX_RDP_TMEM_HASH_SEED;
    uint32_t t_accum = first_word * (uint32_t)dxt;
    const uint8_t *src = source + first_word * GFX_RDP_TMEM_WORD_BYTES;
    for (uint32_t word = 0u; word < word_count;
         ++word, t_accum += dxt, src += GFX_RDP_TMEM_WORD_BYTES) {
        if (dxt != 0u && ((t_accum >> 11) & 1u)) {
            /* odd line: TMEM holds src[4..7] then src[0..3] */
            uint8_t swapped[8];
            memcpy(swapped, src + 4, 4);
            memcpy(swapped + 4, src, 4);
            gfxRdpTmemHashMix64(&hash, gfxRdpTmemLoad64(swapped));
        } else {
            gfxRdpTmemHashMix64(&hash, gfxRdpTmemLoad64(src));
        }
    }
    return hash;
}

static bool gfxRdpTmemLoadBlockLinearImpl(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, const uint8_t *source,
    uint32_t size_bytes, uint16_t dxt, uint32_t t_start);

extern "C" bool gfxRdpTmemLoadBlockLinear(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, const uint8_t *source,
    uint32_t size_bytes, uint16_t dxt)
{
    return gfxRdpTmemLoadBlockLinearImpl(tmem, first_tmem_word, source,
        size_bytes, dxt, 0u);
}

extern "C" bool gfxRdpTmemLoadBlockLinearPart(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, const uint8_t *block_source,
    uint32_t skip_words, uint32_t word_count, uint16_t dxt)
{
    if (!block_source || skip_words > GFX_RDP_TMEM_WORDS ||
        word_count > GFX_RDP_TMEM_WORDS) {
        return false;
    }
    return gfxRdpTmemLoadBlockLinearImpl(tmem, first_tmem_word,
        block_source + skip_words * GFX_RDP_TMEM_WORD_BYTES,
        word_count * GFX_RDP_TMEM_WORD_BYTES, dxt,
        skip_words * (uint32_t)dxt);
}

static bool gfxRdpTmemLoadBlockLinearImpl(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, const uint8_t *source,
    uint32_t size_bytes, uint16_t dxt, uint32_t t_start)
{
    if (!tmem || (!source && size_bytes != 0u) || dxt > 0x0fffu ||
        first_tmem_word > GFX_RDP_TMEM_WORDS) {
        return false;
    }

    if (size_bytes == 0u) {
        return true;
    }

    const uint32_t word_count =
        (size_bytes + GFX_RDP_TMEM_WORD_BYTES - 1u) / GFX_RDP_TMEM_WORD_BYTES;
    if (word_count > GFX_RDP_TMEM_WORDS - first_tmem_word) {
        return false;
    }

    /*
     * Re-loading identical bytes into fully valid words is a no-op for the
     * TMEM contents. Detect it with a read-only compare and leave generations
     * (and therefore any intact load record and downstream identity reuse)
     * untouched. The compare stops at the first difference.
     */
    if ((size_bytes % GFX_RDP_TMEM_WORD_BYTES) == 0u) {
        uint32_t t_check = t_start;
        uint32_t word = 0u;
        for (; word < word_count; ++word, t_check += dxt) {
            const uint32_t dst_byte =
                (first_tmem_word + word) * GFX_RDP_TMEM_WORD_BYTES;
            const uint8_t *src = source + word * GFX_RDP_TMEM_WORD_BYTES;
            uint64_t want;
            if (dxt != 0u && ((t_check >> 11) & 1u)) {
                uint32_t lo, hi;
                memcpy(&lo, src + 4, 4);
                memcpy(&hi, src, 4);
                want = (uint64_t)lo | ((uint64_t)hi << 32);
            } else {
                want = gfxRdpTmemLoad64(src);
            }
            if (gfxRdpTmemLoad64(&tmem->bytes[dst_byte]) != want ||
                gfxRdpTmemLoad64(&tmem->byte_valid[dst_byte]) !=
                    k_gfx_rdp_tmem_ones ||
                tmem->word_linear[first_tmem_word + word] !=
                    (dxt == 0u ? 1u : 0u)) {
                break;
            }
        }
        if (word == word_count) {
            return true;
        }
    }

    const uint32_t generation = gfxRdpTmemNextGeneration(tmem);
    uint32_t t_accum = t_start;
    uint64_t load_hash = GFX_RDP_TMEM_HASH_SEED;

    for (uint32_t word = 0; word < word_count; ++word) {
        const uint32_t src_offset = word * GFX_RDP_TMEM_WORD_BYTES;
        uint32_t copy_bytes = size_bytes - src_offset;
        if (copy_bytes > GFX_RDP_TMEM_WORD_BYTES) {
            copy_bytes = GFX_RDP_TMEM_WORD_BYTES;
        }

        const uint32_t line = t_accum >> 11;
        const uint32_t dst_word = first_tmem_word + word;
        const uint32_t dst_byte = dst_word * GFX_RDP_TMEM_WORD_BYTES;
        const uint8_t *src = source + src_offset;

        if (copy_bytes == GFX_RDP_TMEM_WORD_BYTES) {
            gfxRdpTmemStoreFullWord(tmem, dst_word, src,
                dxt != 0u && (line & 1u), generation);
            gfxRdpTmemHashMix64(&load_hash,
                gfxRdpTmemLoad64(&tmem->bytes[dst_byte]));
            t_accum += dxt;
            continue;
        }

        gfxRdpTmemPreparePartialWord(tmem, dst_word, copy_bytes);
        for (uint32_t i = 0; i < copy_bytes; ++i) {
            const uint32_t mapped = (dxt != 0u && (line & 1u)) ? (i ^ 4u) : i;
            gfxRdpTmemWriteMappedByte(tmem, dst_byte + mapped, src[i]);
        }
        gfxRdpTmemMarkOneWord(tmem, dst_word, generation);
        t_accum += dxt;
    }

    if (dxt == 0u) {
        memset(&tmem->word_linear[first_tmem_word], 1, word_count);
    }
    if ((size_bytes % GFX_RDP_TMEM_WORD_BYTES) == 0u) {
        gfxRdpTmemRecordLoad(tmem, first_tmem_word, word_count,
            generation, load_hash);
    }
    return true;
}

extern "C" bool gfxRdpTmemLoadBlockRgba32(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint32_t line_words,
    const uint8_t *source, uint32_t texel_count, uint16_t dxt)
{
    if (!tmem || (!source && texel_count != 0u) || dxt > 0x0fffu ||
        first_tmem_word >= GFX_RDP_TMEM_HALF_WORDS ||
        line_words >= GFX_RDP_TMEM_HALF_WORDS) {
        return false;
    }

    if (texel_count == 0u) {
        return true;
    }

    const uint32_t source_word_count = (texel_count + 1u) / 2u;
    const uint32_t generation = gfxRdpTmemNextGeneration(tmem);
    const uint32_t low_half_mask = (GFX_RDP_TMEM_BYTES / 2u) - 1u;
    uint32_t tmem_byte = first_tmem_word * GFX_RDP_TMEM_WORD_BYTES;
    uint32_t tmem_xor = 0u;
    uint32_t dxt_counter = 0u;
    uint32_t texel = 0u;

    for (uint32_t source_word = 0; source_word < source_word_count; ++source_word) {
        const uint32_t mapped_low = (tmem_byte ^ tmem_xor) & low_half_mask;
        const uint32_t mapped_high = mapped_low + GFX_RDP_TMEM_BYTES / 2u;
        const uint32_t low_word = mapped_low / GFX_RDP_TMEM_WORD_BYTES;
        const uint32_t high_word = mapped_high / GFX_RDP_TMEM_WORD_BYTES;
        const uint32_t low_lane = mapped_low & (GFX_RDP_TMEM_WORD_BYTES - 1u);
        const uint32_t high_lane = mapped_high & (GFX_RDP_TMEM_WORD_BYTES - 1u);

        if (low_lane > GFX_RDP_TMEM_WORD_BYTES - 4u ||
            high_lane > GFX_RDP_TMEM_WORD_BYTES - 4u) {
            return false;
        }

        /* A source 64-bit load owns four bytes in each TMEM half. */
        memset(&tmem->byte_valid[low_word * GFX_RDP_TMEM_WORD_BYTES + low_lane],
            0, 4u);
        memset(&tmem->byte_valid[high_word * GFX_RDP_TMEM_WORD_BYTES + high_lane],
            0, 4u);

        for (uint32_t pair = 0; pair < 2u && texel < texel_count;
             ++pair, ++texel) {
            const uint8_t *src = source + (size_t)texel * 4u;
            const uint32_t low_byte =
                low_word * GFX_RDP_TMEM_WORD_BYTES + low_lane + pair * 2u;
            const uint32_t high_byte =
                high_word * GFX_RDP_TMEM_WORD_BYTES + high_lane + pair * 2u;

            gfxRdpTmemWriteMappedByte(tmem, low_byte, src[0]);
            gfxRdpTmemWriteMappedByte(tmem, low_byte + 1u, src[1]);
            gfxRdpTmemWriteMappedByte(tmem, high_byte, src[2]);
            gfxRdpTmemWriteMappedByte(tmem, high_byte + 1u, src[3]);
        }

        gfxRdpTmemMarkOneWord(tmem, low_word, generation);
        gfxRdpTmemMarkOneWord(tmem, high_word, generation);

        dxt_counter += dxt;
        while (dxt_counter >= 0x800u) {
            tmem_byte = (tmem_byte + line_words * GFX_RDP_TMEM_WORD_BYTES) &
                        low_half_mask;
            dxt_counter -= 0x800u;
            tmem_xor ^= 4u;
        }
        tmem_byte = (tmem_byte + 4u) & low_half_mask;
    }

    return true;
}

extern "C" bool gfxRdpTmemReadTileLinear(const struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint32_t line_words,
    uint8_t *dest, uint32_t dest_stride_bytes,
    uint32_t row_bytes, uint32_t row_count)
{
    if (!tmem || (!dest && row_bytes != 0u && row_count != 0u) ||
        dest_stride_bytes < row_bytes) {
        return false;
    }

    if (row_bytes == 0u || row_count == 0u) {
        return true;
    }

    const uint32_t row_words =
        (row_bytes + GFX_RDP_TMEM_WORD_BYTES - 1u) / GFX_RDP_TMEM_WORD_BYTES;
    if (!gfxRdpTmemTileRangeValid(first_tmem_word, line_words,
            row_words, row_count, GFX_RDP_TMEM_WORDS)) {
        return false;
    }

    for (uint32_t row = 0; row < row_count; ++row) {
        uint8_t *dst_row = dest + (size_t)row * dest_stride_bytes;
        const uint32_t src_row_word = first_tmem_word + row * line_words;

        for (uint32_t word = 0; word < row_words; ++word) {
            const uint32_t dst_offset = word * GFX_RDP_TMEM_WORD_BYTES;
            uint32_t copy_bytes = row_bytes - dst_offset;
            if (copy_bytes > GFX_RDP_TMEM_WORD_BYTES) {
                copy_bytes = GFX_RDP_TMEM_WORD_BYTES;
            }

            const uint32_t src_byte =
                (src_row_word + word) * GFX_RDP_TMEM_WORD_BYTES;
            const bool swap_row = (row & 1u) != 0u &&
                tmem->word_linear[src_row_word + word] == 0u;
            if (copy_bytes == GFX_RDP_TMEM_WORD_BYTES) {
                if (!gfxRdpTmemAllBytesNonZero(
                        gfxRdpTmemLoad64(&tmem->byte_valid[src_byte]))) {
                    return false;
                }
                if (swap_row) {
                    memcpy(&dst_row[dst_offset], &tmem->bytes[src_byte + 4u], 4);
                    memcpy(&dst_row[dst_offset + 4u], &tmem->bytes[src_byte], 4);
                } else {
                    memcpy(&dst_row[dst_offset], &tmem->bytes[src_byte], 8);
                }
                continue;
            }
            for (uint32_t i = 0; i < copy_bytes; ++i) {
                const uint32_t mapped = swap_row ? (i ^ 4u) : i;
                if (!tmem->byte_valid[src_byte + mapped]) {
                    return false;
                }
                dst_row[dst_offset + i] = tmem->bytes[src_byte + mapped];
            }
        }
    }

    return true;
}

extern "C" bool gfxRdpTmemReadTileLinearBytes(
    const struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint32_t line_words,
    uint8_t *dest, uint32_t row_bytes, uint32_t size_bytes)
{
    if (!tmem || (!dest && size_bytes != 0u) ||
        (size_bytes != 0u && row_bytes == 0u) ||
        row_bytes > GFX_RDP_TMEM_BYTES ||
        size_bytes > GFX_RDP_TMEM_BYTES) {
        return false;
    }

    if (size_bytes == 0u) {
        return true;
    }

    const uint32_t row_count =
        (size_bytes + row_bytes - 1u) / row_bytes;
    const uint32_t full_row_words =
        (row_bytes + GFX_RDP_TMEM_WORD_BYTES - 1u) /
        GFX_RDP_TMEM_WORD_BYTES;
    if (!gfxRdpTmemTileRangeValid(first_tmem_word, line_words,
            full_row_words, row_count, GFX_RDP_TMEM_WORDS)) {
        return false;
    }

    uint32_t remaining = size_bytes;
    uint8_t *dst = dest;
    for (uint32_t row = 0; row < row_count; ++row) {
        uint32_t current_row_bytes = remaining;
        if (current_row_bytes > row_bytes) {
            current_row_bytes = row_bytes;
        }

        const uint32_t row_words =
            (current_row_bytes + GFX_RDP_TMEM_WORD_BYTES - 1u) /
            GFX_RDP_TMEM_WORD_BYTES;
        const uint32_t src_row_word = first_tmem_word + row * line_words;

        for (uint32_t word = 0; word < row_words; ++word) {
            const uint32_t dst_offset = word * GFX_RDP_TMEM_WORD_BYTES;
            uint32_t copy_bytes = current_row_bytes - dst_offset;
            if (copy_bytes > GFX_RDP_TMEM_WORD_BYTES) {
                copy_bytes = GFX_RDP_TMEM_WORD_BYTES;
            }

            const uint32_t src_byte =
                (src_row_word + word) * GFX_RDP_TMEM_WORD_BYTES;
            const bool swap_row = (row & 1u) != 0u &&
                tmem->word_linear[src_row_word + word] == 0u;
            if (copy_bytes == GFX_RDP_TMEM_WORD_BYTES) {
                if (!gfxRdpTmemAllBytesNonZero(
                        gfxRdpTmemLoad64(&tmem->byte_valid[src_byte]))) {
                    return false;
                }
                if (swap_row) {
                    memcpy(&dst[dst_offset], &tmem->bytes[src_byte + 4u], 4);
                    memcpy(&dst[dst_offset + 4u], &tmem->bytes[src_byte], 4);
                } else {
                    memcpy(&dst[dst_offset], &tmem->bytes[src_byte], 8);
                }
                continue;
            }
            for (uint32_t i = 0; i < copy_bytes; ++i) {
                const uint32_t mapped = swap_row ? (i ^ 4u) : i;
                if (!tmem->byte_valid[src_byte + mapped]) {
                    return false;
                }
                dst[dst_offset + i] = tmem->bytes[src_byte + mapped];
            }
        }

        dst += current_row_bytes;
        remaining -= current_row_bytes;
    }

    return true;
}

extern "C" bool gfxRdpTmemReadTileRgba32(const struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint32_t line_words,
    uint8_t *dest, uint32_t dest_stride_bytes,
    uint32_t texels_per_row, uint32_t row_count)
{
    const uint32_t dest_row_bytes = texels_per_row * 4u;
    if (!tmem || (!dest && texels_per_row != 0u && row_count != 0u) ||
        dest_stride_bytes < dest_row_bytes) {
        return false;
    }

    if (texels_per_row == 0u || row_count == 0u) {
        return true;
    }

    const uint32_t row_half_bytes = texels_per_row * 2u;
    const uint32_t row_words =
        (row_half_bytes + GFX_RDP_TMEM_WORD_BYTES - 1u) / GFX_RDP_TMEM_WORD_BYTES;
    if (!gfxRdpTmemTileRangeValid(first_tmem_word, line_words,
            row_words, row_count, GFX_RDP_TMEM_HALF_WORDS)) {
        return false;
    }

    for (uint32_t row = 0; row < row_count; ++row) {
        uint8_t *dst_row = dest + (size_t)row * dest_stride_bytes;
        const uint32_t low_row_word = first_tmem_word + row * line_words;

        for (uint32_t texel = 0; texel < texels_per_row; ++texel) {
            const uint32_t group = texel / 4u;
            const uint32_t lane = texel & 3u;
            const uint32_t mapped_lane = (row & 1u) ? (lane ^ 2u) : lane;
            const uint32_t low_word = low_row_word + group;
            const uint32_t high_word = low_word + GFX_RDP_TMEM_HALF_WORDS;
            const uint32_t low_byte =
                low_word * GFX_RDP_TMEM_WORD_BYTES + mapped_lane * 2u;
            const uint32_t high_byte =
                high_word * GFX_RDP_TMEM_WORD_BYTES + mapped_lane * 2u;

            if (!tmem->byte_valid[low_byte] || !tmem->byte_valid[low_byte + 1u] ||
                !tmem->byte_valid[high_byte] || !tmem->byte_valid[high_byte + 1u]) {
                return false;
            }

            uint8_t *dst = dst_row + texel * 4u;
            dst[0] = tmem->bytes[low_byte];
            dst[1] = tmem->bytes[low_byte + 1u];
            dst[2] = tmem->bytes[high_byte];
            dst[3] = tmem->bytes[high_byte + 1u];
        }
    }

    return true;
}

extern "C" bool gfxRdpTmemReadTileRgba32Texels(
    const struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint32_t line_words,
    uint8_t *dest, uint32_t texels_per_row, uint32_t texel_count)
{
    if (!tmem || (!dest && texel_count != 0u) ||
        (texel_count != 0u && texels_per_row == 0u) ||
        texels_per_row > GFX_RDP_TMEM_BYTES / 4u ||
        texel_count > GFX_RDP_TMEM_BYTES / 4u) {
        return false;
    }

    if (texel_count == 0u) {
        return true;
    }

    const uint32_t row_count =
        (texel_count + texels_per_row - 1u) / texels_per_row;
    const uint32_t full_row_half_bytes = texels_per_row * 2u;
    const uint32_t full_row_words =
        (full_row_half_bytes + GFX_RDP_TMEM_WORD_BYTES - 1u) /
        GFX_RDP_TMEM_WORD_BYTES;
    if (!gfxRdpTmemTileRangeValid(first_tmem_word, line_words,
            full_row_words, row_count, GFX_RDP_TMEM_HALF_WORDS)) {
        return false;
    }

    uint32_t remaining = texel_count;
    uint8_t *dst = dest;
    for (uint32_t row = 0; row < row_count; ++row) {
        uint32_t current_row_texels = remaining;
        if (current_row_texels > texels_per_row) {
            current_row_texels = texels_per_row;
        }

        const uint32_t low_row_word = first_tmem_word + row * line_words;
        for (uint32_t texel = 0; texel < current_row_texels; ++texel) {
            const uint32_t group = texel / 4u;
            const uint32_t lane = texel & 3u;
            const uint32_t mapped_lane = (row & 1u) ? (lane ^ 2u) : lane;
            const uint32_t low_word = low_row_word + group;
            const uint32_t high_word = low_word + GFX_RDP_TMEM_HALF_WORDS;
            const uint32_t low_byte =
                low_word * GFX_RDP_TMEM_WORD_BYTES + mapped_lane * 2u;
            const uint32_t high_byte =
                high_word * GFX_RDP_TMEM_WORD_BYTES + mapped_lane * 2u;

            if (!tmem->byte_valid[low_byte] ||
                !tmem->byte_valid[low_byte + 1u] ||
                !tmem->byte_valid[high_byte] ||
                !tmem->byte_valid[high_byte + 1u]) {
                return false;
            }

            dst[0] = tmem->bytes[low_byte];
            dst[1] = tmem->bytes[low_byte + 1u];
            dst[2] = tmem->bytes[high_byte];
            dst[3] = tmem->bytes[high_byte + 1u];
            dst += 4;
        }

        remaining -= current_row_texels;
    }

    return true;
}

extern "C" bool gfxRdpTmemReadTlut(const struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint16_t *entries, uint32_t entry_count)
{
    if (!tmem || (!entries && entry_count != 0u) ||
        first_tmem_word > GFX_RDP_TMEM_WORDS ||
        entry_count > GFX_RDP_TMEM_WORDS - first_tmem_word) {
        return false;
    }

    for (uint32_t entry = 0; entry < entry_count; ++entry) {
        const uint32_t byte_offset =
            (first_tmem_word + entry) * GFX_RDP_TMEM_WORD_BYTES;
        const uint8_t hi = tmem->bytes[byte_offset];
        const uint8_t lo = tmem->bytes[byte_offset + 1u];

        for (uint32_t lane = 0; lane < 4u; ++lane) {
            const uint32_t lane_offset = byte_offset + lane * 2u;
            if (!tmem->byte_valid[lane_offset] ||
                !tmem->byte_valid[lane_offset + 1u] ||
                tmem->bytes[lane_offset] != hi ||
                tmem->bytes[lane_offset + 1u] != lo) {
                return false;
            }
        }

        entries[entry] = (uint16_t)((uint16_t)hi << 8) | lo;
    }

    return true;
}

extern "C" const uint8_t *gfxRdpTmemBytes(const struct GfxRdpTmem *tmem)
{
    return tmem ? tmem->bytes : NULL;
}

extern "C" uint32_t gfxRdpTmemGeneration(const struct GfxRdpTmem *tmem)
{
    return tmem ? tmem->generation : 0u;
}

extern "C" uint32_t gfxRdpTmemWordGeneration(const struct GfxRdpTmem *tmem,
    uint32_t tmem_word)
{
    if (!tmem || tmem_word >= GFX_RDP_TMEM_WORDS) {
        return 0u;
    }

    return tmem->word_generation[tmem_word];
}

extern "C" bool gfxRdpTmemByteValid(const struct GfxRdpTmem *tmem,
    uint32_t byte_offset)
{
    return tmem && byte_offset < GFX_RDP_TMEM_BYTES &&
           tmem->byte_valid[byte_offset] != 0u;
}

extern "C" bool gfxRdpTmemWordValid(const struct GfxRdpTmem *tmem,
    uint32_t tmem_word)
{
    return tmem && tmem_word < GFX_RDP_TMEM_WORDS &&
           tmem->word_valid[tmem_word] != 0u;
}
