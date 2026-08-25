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

#endif
