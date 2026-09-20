#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifdef PLATFORM_PS2
#include <malloc.h>
#endif

#include "romsource.h"

#if defined(PLATFORM_PS2) && defined(PD_PS2_FMCB_STARTUP_DIAGNOSTIC)
static void romSourceStartupMarker(uint64_t color)
{
	*(volatile uint64_t *)0x120000e0 = color;

	uint32_t spins = 40000000u;
	__asm__ __volatile__(
		".set noreorder\n\t"
		"1:\n\t"
		"addiu %0, %0, -1\n\t"
		"bnez %0, 1b\n\t"
		"nop\n\t"
		".set reorder\n\t"
		: "+r"(spins)
		:
		: "memory");
}
#define ROMSOURCE_STARTUP_MARKER(color) romSourceStartupMarker((uint64_t)(color))
#else
#define ROMSOURCE_STARTUP_MARKER(color) ((void)0)
#endif

#define ROMSOURCE_DIAG_FOPEN_READY   UINT64_C(0x0000ff80) /* lime */
#define ROMSOURCE_DIAG_SEEK_END      UINT64_C(0x0000ff00) /* green */
#define ROMSOURCE_DIAG_SIZE_READY    UINT64_C(0x00ffff00) /* cyan */
#define ROMSOURCE_DIAG_REWIND_READY  UINT64_C(0x00ff0000) /* blue */
#define ROMSOURCE_DIAG_CACHE_READY   UINT64_C(0x00ff00ff) /* magenta */

#define ROMSOURCE_FILE_CACHE_LINE_SIZE (4u * 1024u)
#define ROMSOURCE_FILE_CACHE_SIZE \
	(ROMSOURCE_FILE_CACHE_SLOTS * ROMSOURCE_FILE_CACHE_LINE_SIZE)
#define ROMSOURCE_FILE_CACHE_ALIGNMENT 512u

static u8 *romSourceAllocCache(void)
{
#ifdef PLATFORM_PS2
	return memalign(64u, ROMSOURCE_FILE_CACHE_SIZE);
#else
	return malloc(ROMSOURCE_FILE_CACHE_SIZE);
#endif
}

static void romSourceResetCache(struct romsource *source)
{
	if (!source) {
		return;
	}

	for (u32 i = 0; i < ROMSOURCE_FILE_CACHE_SLOTS; ++i) {
		source->read_cache_lines[i].offset = 0;
		source->read_cache_lines[i].length = 0;
	}

	source->read_cache_next_slot = 0;
}

static bool romSourceRangeValid(const struct romsource *source, u32 offset, u32 length)
{
	if (!source || source->kind == ROMSOURCE_NONE) {
		return false;
	}

	if (offset > source->size) {
		return false;
	}

	return length <= source->size - offset;
}

void romSourceInitMemory(struct romsource *source, const void *data, u32 size)
{
	if (!source) {
		return;
	}

	source->kind = data || size == 0 ? ROMSOURCE_MEMORY : ROMSOURCE_NONE;
	source->memory = data;
	source->handle = NULL;
	source->size = size;
	source->read_cache = NULL;
	romSourceResetCache(source);
}

bool romSourceOpenFile(struct romsource *source, const char *path)
{
	if (!source || !path || !path[0]) {
		return false;
	}

	FILE *file = fopen(path, "rb");
	if (!file) {
		return false;
	}
	ROMSOURCE_STARTUP_MARKER(ROMSOURCE_DIAG_FOPEN_READY);

	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return false;
	}
	ROMSOURCE_STARTUP_MARKER(ROMSOURCE_DIAG_SEEK_END);

	const long fileSize = ftell(file);
	if (fileSize < 0 || (unsigned long)fileSize > 0xfffffffful) {
		fclose(file);
		return false;
	}
	ROMSOURCE_STARTUP_MARKER(ROMSOURCE_DIAG_SIZE_READY);

	if (fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return false;
	}
	ROMSOURCE_STARTUP_MARKER(ROMSOURCE_DIAG_REWIND_READY);

	source->kind = ROMSOURCE_FILE;
	source->memory = NULL;
	source->handle = file;
	source->size = (u32)fileSize;
	source->read_cache = romSourceAllocCache();
	romSourceResetCache(source);
	ROMSOURCE_STARTUP_MARKER(ROMSOURCE_DIAG_CACHE_READY);

	return true;
}

void romSourceClose(struct romsource *source)
{
	if (!source) {
		return;
	}

	if (source->kind == ROMSOURCE_FILE && source->handle) {
		fclose((FILE *)source->handle);
	}
	free(source->read_cache);

	source->kind = ROMSOURCE_NONE;
	source->memory = NULL;
	source->handle = NULL;
	source->size = 0;
	source->read_cache = NULL;
	romSourceResetCache(source);
}

u32 romSourceGetSize(const struct romsource *source)
{
	return source ? source->size : 0;
}

const u8 *romSourceView(const struct romsource *source, u32 offset, u32 length)
{
	if (!romSourceRangeValid(source, offset, length)) {
		return NULL;
	}

	if (source->kind != ROMSOURCE_MEMORY) {
		return NULL;
	}

	return source->memory + offset;
}

bool romSourceReadAt(struct romsource *source, u32 offset, void *dst, u32 length)
{
	if (!dst && length != 0) {
		return false;
	}

	if (!romSourceRangeValid(source, offset, length)) {
		return false;
	}

	if (length == 0) {
		return true;
	}

	if (source->kind == ROMSOURCE_MEMORY) {
		memcpy(dst, source->memory + offset, length);
		return true;
	}

	if (source->kind == ROMSOURCE_FILE) {
		if (!source->handle || offset > (u32)LONG_MAX) {
			return false;
		}

		FILE *file = (FILE *)source->handle;
		if (source->read_cache && length <= ROMSOURCE_FILE_CACHE_LINE_SIZE) {
			for (u32 i = 0; i < ROMSOURCE_FILE_CACHE_SLOTS; ++i) {
				const struct romsourcecacheline *line = &source->read_cache_lines[i];
				if (offset >= line->offset) {
					const u32 cacheDelta = offset - line->offset;
					if (cacheDelta <= line->length &&
							length <= line->length - cacheDelta) {
						memcpy(dst,
							source->read_cache + i * ROMSOURCE_FILE_CACHE_LINE_SIZE + cacheDelta,
							length);
						return true;
					}
				}
			}

			u32 cacheOffset = offset & ~(ROMSOURCE_FILE_CACHE_ALIGNMENT - 1u);
			u32 cacheDelta = offset - cacheOffset;
			if (length > ROMSOURCE_FILE_CACHE_LINE_SIZE - cacheDelta) {
				cacheOffset = offset;
				cacheDelta = 0;
			}

			const u32 remaining = source->size - cacheOffset;
			const u32 cacheLength = remaining < ROMSOURCE_FILE_CACHE_LINE_SIZE
				? remaining : ROMSOURCE_FILE_CACHE_LINE_SIZE;
			const u32 slot = source->read_cache_next_slot;
			u8 *cache = source->read_cache + slot * ROMSOURCE_FILE_CACHE_LINE_SIZE;

			if (cacheOffset <= (u32)LONG_MAX &&
					fseek(file, (long)cacheOffset, SEEK_SET) == 0 &&
					fread(cache, 1, cacheLength, file) == cacheLength) {
				source->read_cache_lines[slot].offset = cacheOffset;
				source->read_cache_lines[slot].length = cacheLength;
				source->read_cache_next_slot =
					(slot + 1u) % ROMSOURCE_FILE_CACHE_SLOTS;
				memcpy(dst, cache + cacheDelta, length);
				return true;
			}

			source->read_cache_lines[slot].length = 0;
			return false;
		}

		if (fseek(file, (long)offset, SEEK_SET) != 0) {
			return false;
		}

		return fread(dst, 1, length, file) == length;
	}

	return false;
}

bool romSourceGetRzip1173Size(struct romsource *source, u32 offset, u32 *outSize)
{
	u8 header[5];

	if (!outSize || !romSourceReadAt(source, offset, header, sizeof(header))) {
		return false;
	}

	if (header[0] != 0x11 || header[1] != 0x73) {
		return false;
	}

	*outSize = ((u32)header[2] << 16) | ((u32)header[3] << 8) | (u32)header[4];
	return *outSize != 0;
}

bool romSourceInflate1173(struct romsource *source, u32 offset,
		void *output, u32 outputSize, void *inputScratch, u32 inputScratchSize,
		u32 *outCompressedSize)
{
	z_stream stream;
	u32 expectedSize;
	u32 nextOffset;
	bool initialized = false;
	bool success = false;

	if (!output || !inputScratch || inputScratchSize == 0 ||
		inputScratchSize > UINT_MAX || outputSize > UINT_MAX ||
		!romSourceGetRzip1173Size(source, offset, &expectedSize) ||
		expectedSize != outputSize || offset > UINT32_MAX - 5u) {
		return false;
	}

	memset(&stream, 0, sizeof(stream));
	stream.next_out = output;
	stream.avail_out = outputSize;

	if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
		return false;
	}

	initialized = true;
	nextOffset = offset + 5u;

	for (;;) {
		if (stream.avail_in == 0) {
			const u32 sourceSize = romSourceGetSize(source);
			const u32 remaining = nextOffset < sourceSize ? sourceSize - nextOffset : 0;
			const u32 amount = remaining < inputScratchSize ? remaining : inputScratchSize;

			if (amount == 0 || !romSourceReadAt(source, nextOffset, inputScratch, amount)) {
				break;
			}

			nextOffset += amount;
			stream.next_in = inputScratch;
			stream.avail_in = amount;
		}

		const uLong beforeIn = stream.total_in;
		const uLong beforeOut = stream.total_out;
		const int result = inflate(&stream, Z_NO_FLUSH);

		if (result == Z_STREAM_END) {
			success = stream.total_out == expectedSize;
			break;
		}

		if (result != Z_OK || stream.avail_out == 0 ||
			(stream.total_in == beforeIn && stream.total_out == beforeOut)) {
			break;
		}
	}

	if (initialized && inflateEnd(&stream) != Z_OK) {
		success = false;
	}

	if (success && outCompressedSize) {
		*outCompressedSize = (u32)stream.total_in;
	}

	return success;
}
