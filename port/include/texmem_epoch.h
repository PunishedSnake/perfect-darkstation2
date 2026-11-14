#ifndef PD_TEXMEM_EPOCH_H
#define PD_TEXMEM_EPOCH_H

/*
 * Texture-memory write epoch (PS2).
 *
 * The renderer identifies textures by their bytes. Verifying that a texture
 * in RAM is unchanged costs a full read every time, so instead every game
 * path that can write memory a texture may live in (allocators that hand
 * memory out again, decompression and loading, DMA from ROM, room loads)
 * bumps this counter. While it does not move, texture identities verified
 * earlier are still valid and the renderer skips re-reading them.
 *
 * Build with PD_PS2_TEXMEM_VERIFY to keep re-reading anyway and log any
 * texture that changed without a bump.
 */
#ifdef __cplusplus
extern "C" {
#endif

extern unsigned int g_TexMemWriteEpoch;

static inline void texmemNoteWrite(void)
{
	g_TexMemWriteEpoch++;
}

#ifdef __cplusplus
}
#endif

#endif
