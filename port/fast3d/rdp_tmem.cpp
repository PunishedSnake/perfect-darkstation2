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

static void gfxRdpTmemMarkRange(struct GfxRdpTmem *tmem,
    uint32_t byte_offset, uint32_t size_bytes, bool invalidate)
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
        if (invalidate) {
            tmem->word_valid[word] = 0u;
        }
    }
}

static void gfxRdpTmemMarkWrittenRange(struct GfxRdpTmem *tmem,
    uint32_t byte_offset, uint32_t size_bytes)
{
    if (size_bytes == 0u) {
        return;
    }

    const uint32_t generation = gfxRdpTmemNextGeneration(tmem);
    const uint32_t first_word = byte_offset / GFX_RDP_TMEM_WORD_BYTES;
    const uint32_t last_word =
        (byte_offset + size_bytes - 1u) / GFX_RDP_TMEM_WORD_BYTES;
    const uint32_t write_end = byte_offset + size_bytes;

    for (uint32_t word = first_word; word <= last_word; ++word) {
        const uint32_t word_start = word * GFX_RDP_TMEM_WORD_BYTES;
        const uint32_t word_end = word_start + GFX_RDP_TMEM_WORD_BYTES;

        tmem->word_generation[word] = generation;
        if (byte_offset <= word_start && write_end >= word_end) {
            tmem->word_valid[word] = 1u;
        }
    }
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
    gfxRdpTmemMarkWrittenRange(tmem, byte_offset, size_bytes);
    return true;
}

extern "C" bool gfxRdpTmemInvalidatePhysical(struct GfxRdpTmem *tmem,
    uint32_t byte_offset, uint32_t size_bytes)
{
    if (!tmem || !gfxRdpTmemRangeValid(byte_offset, size_bytes)) {
        return false;
    }

    gfxRdpTmemMarkRange(tmem, byte_offset, size_bytes, true);
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

    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint16_t value = entries[i];
        const uint8_t hi = (uint8_t)(value >> 8);
        const uint8_t lo = (uint8_t)value;
        uint8_t *dst = &tmem->bytes[(first_tmem_word + i) * GFX_RDP_TMEM_WORD_BYTES];

        dst[0] = hi;
        dst[1] = lo;
        dst[2] = hi;
        dst[3] = lo;
        dst[4] = hi;
        dst[5] = lo;
        dst[6] = hi;
        dst[7] = lo;
        tmem->word_valid[first_tmem_word + i] = 1u;
    }

    gfxRdpTmemMarkRange(tmem,
        first_tmem_word * GFX_RDP_TMEM_WORD_BYTES,
        entry_count * GFX_RDP_TMEM_WORD_BYTES,
        false);
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
            uint8_t *dst = &tmem->bytes[dst_word * GFX_RDP_TMEM_WORD_BYTES];
            const uint8_t *src = src_row + src_offset;

            if ((row & 1u) == 0u) {
                memcpy(dst, src, copy_bytes);
            } else {
                /* Odd rows swap the two 32-bit halves of each 64-bit word. */
                const uint32_t low_bytes = copy_bytes > 4u ? 4u : copy_bytes;
                if (low_bytes != 0u) {
                    memcpy(dst + 4u, src, low_bytes);
                }
                if (copy_bytes > 4u) {
                    memcpy(dst, src + 4u, copy_bytes - 4u);
                }
            }

            tmem->word_generation[dst_word] = generation;
            if (copy_bytes == GFX_RDP_TMEM_WORD_BYTES) {
                tmem->word_valid[dst_word] = 1u;
            } else {
                /* Padding bytes are not claimed byte-exact yet. */
                tmem->word_valid[dst_word] = 0u;
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

extern "C" bool gfxRdpTmemWordValid(const struct GfxRdpTmem *tmem,
    uint32_t tmem_word)
{
    return tmem && tmem_word < GFX_RDP_TMEM_WORDS &&
           tmem->word_valid[tmem_word] != 0u;
}
