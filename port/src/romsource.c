#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "romsource.h"

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

	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return false;
	}

	const long fileSize = ftell(file);
	if (fileSize < 0 || (unsigned long)fileSize > 0xfffffffful) {
		fclose(file);
		return false;
	}

	if (fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return false;
	}

	source->kind = ROMSOURCE_FILE;
	source->memory = NULL;
	source->handle = file;
	source->size = (u32)fileSize;

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

	source->kind = ROMSOURCE_NONE;
	source->memory = NULL;
	source->handle = NULL;
	source->size = 0;
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
