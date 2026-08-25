#include <limits.h>
#include <stdio.h>
#include <string.h>

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
