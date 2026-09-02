#ifndef _IN_ROMDATA_H
#define _IN_ROMDATA_H

#include <PR/ultratypes.h>

#ifdef PLATFORM_PS2
#include <stdint.h>
#endif

extern u8 *g_RomFile;
extern u32 g_RomFileSize;

s32 romdataInit(void);

#ifdef PLATFORM_PS2
enum romdata_dma_result {
	ROMDATA_DMA_ERROR = -1,
	ROMDATA_DMA_UNMAPPED = 0,
	ROMDATA_DMA_OK = 1,
};

/* Resolve a pointer-shaped virtual ROM address for the synchronous EE DMA shim. */
s32 romdataDmaRead(void *dst, uintptr_t address, u32 length);
#endif

u8 *romdataFileLoad(s32 fileNum, u32 *outSize);
void romdataFilePreprocess(s32 fileNum, s32 loadType, u8 *data, u32 size, u32 *outSize);
void romdataFileFree(s32 fileNum);
const char *romdataFileGetName(s32 fileNum);

u8 *romdataFileGetData(s32 fileNum);
s32 romdataFileGetSize(s32 fileNum);

s32 romdataFileGetNumForName(const char *name);

u8 *romdataSegGetData(const char *segName);
u8 *romdataSegGetDataEnd(const char *segName);
u32 romdataSegGetSize(const char *segName);
u32 romdataFileGetEstimatedSize(const u32 size, const u32 loadtype);

s32 romdataCheckGbcRom(void);

#endif
