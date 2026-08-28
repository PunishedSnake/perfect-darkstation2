#ifndef _IN_ROMSOURCE_H
#define _IN_ROMSOURCE_H

#include <stdbool.h>
#include <PR/ultratypes.h>

enum romsourcekind {
	ROMSOURCE_NONE = 0,
	ROMSOURCE_MEMORY,
	ROMSOURCE_FILE,
};

/*
 * ROM source contract.
 *
 * source data is addressed by stable 32-bit offsets. A returned memory view is
 * only valid for resident-memory sources; file-backed callers must use readAt
 * into storage they own.
 */
struct romsource {
	enum romsourcekind kind;
	const u8 *memory;
	void *handle;
	u32 size;
};

void romSourceInitMemory(struct romsource *source, const void *data, u32 size);
bool romSourceOpenFile(struct romsource *source, const char *path);
void romSourceClose(struct romsource *source);

u32 romSourceGetSize(const struct romsource *source);
const u8 *romSourceView(const struct romsource *source, u32 offset, u32 length);
bool romSourceReadAt(struct romsource *source, u32 offset, void *dst, u32 length);

/*
 * Bounded RZIP 1173 streaming contract.
 *
 * The caller owns both the final output and the input scratch buffer. This
 * keeps allocation policy outside the source layer and makes file-backed ROM
 * inflation independent of a resident 32 MiB image.
 */
bool romSourceGetRzip1173Size(struct romsource *source, u32 offset, u32 *outSize);
bool romSourceInflate1173(struct romsource *source, u32 offset,
		void *output, u32 outputSize, void *inputScratch, u32 inputScratchSize,
		u32 *outCompressedSize);

#endif
