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

/*
 * Backend-independent shadow of the RDP's 4 KiB TMEM.
 *
 * `bytes` is a canonical byte image of logical TMEM storage. Loader-specific
 * format/bank/interleave rules belong in the RDP command implementation, which
 * writes the resulting physical bytes through this model. This avoids baking
 * OpenGL/GS texture identities or source pointers into the memory contract.
 *
 * Generation values support conservative cache invalidation while the faithful
 * loader path is introduced incrementally. A word generation is changed when
 * any byte in the corresponding 64-bit TMEM word is mutated.
 */
struct GfxRdpTmem {
    uint8_t bytes[GFX_RDP_TMEM_BYTES];
    uint32_t generation;
    uint32_t word_generation[GFX_RDP_TMEM_WORDS];
};

void gfxRdpTmemReset(struct GfxRdpTmem *tmem);

/*
 * Store bytes that have already been arranged according to the RDP TMEM layout.
 * This helper deliberately does not pretend to implement LoadTile/LoadBlock.
 */
bool gfxRdpTmemWritePhysical(struct GfxRdpTmem *tmem,
    uint32_t byte_offset, const void *src, uint32_t size_bytes);

/*
 * Model the documented LoadTLUT storage operation for host-order logical
 * 16-bit entries: each entry is replicated four times into one 64-bit TMEM
 * word. The caller remains responsible for source-image addressing and for
 * interpreting RGBA16 versus IA16 TLUT contents.
 */
bool gfxRdpTmemWriteTlut(struct GfxRdpTmem *tmem,
    uint32_t first_tmem_word, const uint16_t *entries, uint32_t entry_count);

const uint8_t *gfxRdpTmemBytes(const struct GfxRdpTmem *tmem);
uint32_t gfxRdpTmemGeneration(const struct GfxRdpTmem *tmem);
uint32_t gfxRdpTmemWordGeneration(const struct GfxRdpTmem *tmem,
    uint32_t tmem_word);

#ifdef __cplusplus
}
#endif

#endif
