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
    }

    gfxRdpTmemMarkRange(tmem,
        first_tmem_word * GFX_RDP_TMEM_WORD_BYTES,
        entry_count * GFX_RDP_TMEM_WORD_BYTES);
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
