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
 * Read-ahead cache for small file-backed reads. Streamed audio is the main
 * client: every active voice pulls a few hundred bytes of ADPCM per audio
 * frame from its own position in sfxtbl, and each uncached read is a
 * synchronous seek + USB 1.1 transfer of roughly 10 ms. The cache must
 * therefore hold one line per concurrent stream (LRU, not round-robin) and
 * a line must last a stream many frames.
 */
#define ROMSOURCE_FILE_CACHE_SLOTS 16u

struct romsourcecacheline {
	u32 offset;
	u32 length;
	u32 last_use;
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
	u8 *read_cache;
	struct romsourcecacheline read_cache_lines[ROMSOURCE_FILE_CACHE_SLOTS];
	u32 read_cache_clock;
	u32 read_cache_reads; /* device reads issued through the cache */
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
