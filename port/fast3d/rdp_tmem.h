#ifndef PERFECT_DARK_FAST3D_RDP_TMEM_H
#define PERFECT_DARK_FAST3D_RDP_TMEM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_RDP_TMEM_BYTES 4096u
#define GFX_RDP_TMEM_WORD_BYTES 8u
#define GFX_RDP_TMEM_WORDS (GFX_RDP_TMEM_BYTES / GFX_RDP_TMEM_WORD_BYTES)
#define GFX_RDP_TMEM_HALF_WORDS (GFX_RDP_TMEM_WORDS / 2u)

/*
 * Backend-independent shadow of the RDP's 4 KiB TMEM.
 *
 * `bytes` uses canonical RDP byte order rather than host-endian integer layout.
 * Loader-specific format/bank/interleave rules belong in this module or in the
 * RDP command implementation, never in an OpenGL/GS texture cache key.
 *
 * Validity is tracked per byte while generations stay per 64-bit word. During
 * the incremental migration a command may be known to mutate a TMEM word before
 * every byte of its result is represented exactly (notably YUV and row padding
 * cases). Known texel bytes remain usable while unknown padding cannot
 * accidentally become an authoritative cache input.
 */
struct GfxRdpTmem {
    uint8_t bytes[GFX_RDP_TMEM_BYTES];
    uint8_t byte_valid[GFX_RDP_TMEM_BYTES];
    uint32_t generation;
    uint32_t word_generation[GFX_RDP_TMEM_WORDS];
    uint8_t word_valid[GFX_RDP_TMEM_WORDS];
};

void gfxRdpTmemReset(struct GfxRdpTmem *tmem);

/*
 * Store bytes that have already been arranged according to the RDP TMEM layout.
 * This helper deliberately does not pretend to implement LoadTile/LoadBlock.
 */
bool gfxRdpTmemWritePhysical(struct GfxRdpTmem *tmem,
    uint32_t byte_offset, const void *src, uint32_t size_bytes);

/*
 * Conservatively record a TMEM mutation whose byte-exact result is not yet
 * represented. Affected word generations advance and their byte images become
 * invalid. This is preferable to letting a future cache treat stale bytes as
 * authoritative during incremental loader bring-up.
 */
bool gfxRdpTmemInvalidatePhysical(struct GfxRdpTmem *tmem,
    uint32_t byte_offset, uint32_t size_bytes);

/*
 * Model the documented LoadTLUT storage operation for host-order logical
 * 16-bit entries: each entry is replicated four times into one 64-bit TMEM
 * word. The caller remains responsible for source-image addressing and for
 * interpreting RGBA16 versus IA16 TLUT contents.
 */
bool gfxRdpTmemWriteTlut(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, const uint16_t *entries, uint32_t entry_count);

/*
 * Byte-exact LoadTile row writer for the non-planar 4/8/16-bit TMEM layouts.
 * Rows are placed `line_words` 64-bit words apart. Odd T rows swap the two
 * 32-bit halves inside each complete 64-bit word, matching the documented RDP
 * interleave. Only source texel bytes become valid; unknown row padding is
 * explicitly invalidated.
 *
 * RGBA32 and YUV use planar/split-bank layouts and have separate contracts.
 */
bool gfxRdpTmemLoadTileLinear(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint32_t line_words,
    const uint8_t *source, uint32_t source_stride_bytes,
    uint32_t row_bytes, uint32_t row_count);

/*
 * Nintendo's documented 32-bit RGBA layout stores R/G 16-bit pairs in low TMEM
 * and B/A 16-bit pairs at the matching address in high TMEM. `line_words` is
 * the SetTile line value, so a row consumes ceil(texels * 2 / 8) words in each
 * half (SDK G_IM_SIZ_32b_LINE_BYTES is 2). Odd T rows swap pairs of 16-bit
 * entries within each 64-bit word.
 *
 * Source texels are canonical big-endian RGBA byte quadruples R,G,B,A.
 */
bool gfxRdpTmemLoadTileRgba32(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint32_t line_words,
    const uint8_t *source, uint32_t source_stride_bytes,
    uint32_t texels_per_row, uint32_t row_count);

/*
 * Byte-exact LoadBlock writer for a non-planar stream whose source bytes are
 * already padded to 64-bit row boundaries. `dxt` is the RDP unsigned 1.11
 * lines-per-word increment. The line number is derived from the accumulated
 * dxt value before each 64-bit write; odd lines swap 32-bit halves. dxt=0
 * therefore preserves a pre-interleaved source exactly.
 *
 * RGBA32/YUV planar formatting is intentionally outside this helper.
 */
bool gfxRdpTmemLoadBlockLinear(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, const uint8_t *source,
    uint32_t size_bytes, uint16_t dxt);

/*
 * Reconstruct logical non-planar tile rows from physical TMEM. This reverses
 * the odd-row 32-bit-half interleave used by the matching LoadTile writer and
 * fails if any requested source byte is not known byte-exact.
 */
bool gfxRdpTmemReadTileLinear(const struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint32_t line_words,
    uint8_t *dest, uint32_t dest_stride_bytes,
    uint32_t row_bytes, uint32_t row_count);

/* Reconstruct canonical R,G,B,A texels from the split RGBA32 TMEM layout. */
bool gfxRdpTmemReadTileRgba32(const struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, uint32_t line_words,
    uint8_t *dest, uint32_t dest_stride_bytes,
    uint32_t texels_per_row, uint32_t row_count);

const uint8_t *gfxRdpTmemBytes(const struct GfxRdpTmem *tmem);
uint32_t gfxRdpTmemGeneration(const struct GfxRdpTmem *tmem);
uint32_t gfxRdpTmemWordGeneration(const struct GfxRdpTmem *tmem,
    uint32_t tmem_word);
bool gfxRdpTmemByteValid(const struct GfxRdpTmem *tmem, uint32_t byte_offset);
bool gfxRdpTmemWordValid(const struct GfxRdpTmem *tmem, uint32_t tmem_word);

#ifdef __cplusplus
}
#endif

#endif
