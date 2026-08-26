#include "rdp_tmem.h"

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
    }

    tmem->generation = next;
    return next;
}

static bool gfxRdpTmemRangeValid(uint32_t byte_offset, uint32_t size_bytes)
{
    return byte_offset <= GFX_RDP_TMEM_BYTES &&
           size_bytes <= GFX_RDP_TMEM_BYTES - byte_offset;
}

static void gfxRdpTmemRefreshWordValid(struct GfxRdpTmem *tmem, uint32_t word)
{
    const uint32_t first_byte = word * GFX_RDP_TMEM_WORD_BYTES;
    uint8_t valid = 1u;

    for (uint32_t i = 0; i < GFX_RDP_TMEM_WORD_BYTES; ++i) {
        valid &= tmem->byte_valid[first_byte + i] ? 1u : 0u;
    }

    tmem->word_valid[word] = valid;
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

    const uint32_t generation = gfxRdpTmemNextGeneration(tmem);
    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint16_t value = entries[i];
        const uint8_t hi = (uint8_t)(value >> 8);
        const uint8_t lo = (uint8_t)value;
        const uint32_t dst_byte =
            (first_tmem_word + i) * GFX_RDP_TMEM_WORD_BYTES;

        for (uint32_t copy = 0; copy < 4u; ++copy) {
            gfxRdpTmemWriteMappedByte(tmem, dst_byte + copy * 2u, hi);
            gfxRdpTmemWriteMappedByte(tmem, dst_byte + copy * 2u + 1u, lo);
        }
        gfxRdpTmemMarkOneWord(tmem, first_tmem_word + i, generation);
    }

    return true;
}

extern "C" bool gfxRdpTmemLoadTileLinear(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint32_t line_words,
    const uint8_t *source, uint32_t source_stride_bytes,
    uint32_t row_bytes, uint32_t row_count)
{
    if (!tmem || (!source && row_bytes != 0u && row_count != 0u) ||
        first_tmem_word > GFX_RDP_TMEM_WORDS ||
        (row_count > 1u && line_words == 0u) ||
        source_stride_bytes < row_bytes) {
        return false;
    }

    if (row_bytes == 0u || row_count == 0u) {
        return true;
    }

    const uint32_t row_words =
        (row_bytes + GFX_RDP_TMEM_WORD_BYTES - 1u) / GFX_RDP_TMEM_WORD_BYTES;
    if (row_words > line_words && row_count > 1u) {
        return false;
    }

    const uint32_t last_row_word = first_tmem_word + (row_count - 1u) * line_words;
    if (last_row_word > GFX_RDP_TMEM_WORDS ||
        row_words > GFX_RDP_TMEM_WORDS - last_row_word) {
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

extern "C" bool gfxRdpTmemLoadBlockLinear(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, const uint8_t *source,
    uint32_t size_bytes, uint16_t dxt)
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

    const uint32_t generation = gfxRdpTmemNextGeneration(tmem);
    uint32_t t_accum = 0u;

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

        gfxRdpTmemPreparePartialWord(tmem, dst_word, copy_bytes);
        for (uint32_t i = 0; i < copy_bytes; ++i) {
            const uint32_t mapped = (dxt != 0u && (line & 1u)) ? (i ^ 4u) : i;
            gfxRdpTmemWriteMappedByte(tmem, dst_byte + mapped, src[i]);
        }
        gfxRdpTmemMarkOneWord(tmem, dst_word, generation);
        t_accum += dxt;
    }

    return true;
}

extern "C" bool gfxRdpTmemReadTileLinear(const struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint32_t line_words,
    uint8_t *dest, uint32_t dest_stride_bytes,
    uint32_t row_bytes, uint32_t row_count)
{
    if (!tmem || (!dest && row_bytes != 0u && row_count != 0u) ||
        first_tmem_word > GFX_RDP_TMEM_WORDS ||
        (row_count > 1u && line_words == 0u) ||
        dest_stride_bytes < row_bytes) {
        return false;
    }

    if (row_bytes == 0u || row_count == 0u) {
        return true;
    }

    const uint32_t row_words =
        (row_bytes + GFX_RDP_TMEM_WORD_BYTES - 1u) / GFX_RDP_TMEM_WORD_BYTES;
    if (row_words > line_words && row_count > 1u) {
        return false;
    }

    const uint32_t last_row_word = first_tmem_word + (row_count - 1u) * line_words;
    if (last_row_word > GFX_RDP_TMEM_WORDS ||
        row_words > GFX_RDP_TMEM_WORDS - last_row_word) {
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
            for (uint32_t i = 0; i < copy_bytes; ++i) {
                const uint32_t mapped = (row & 1u) ? (i ^ 4u) : i;
                if (!tmem->byte_valid[src_byte + mapped]) {
                    return false;
                }
                dst_row[dst_offset + i] = tmem->bytes[src_byte + mapped];
            }
        }
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
