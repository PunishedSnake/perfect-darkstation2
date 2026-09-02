#ifndef PD_PS2_PATH_PS2_H
#define PD_PS2_PATH_PS2_H

#include <stdbool.h>

static inline bool ps2PathHasDevicePrefix(const char *path)
{
    const unsigned char *cursor = (const unsigned char *)path;

    if (!cursor || !*cursor) {
        return false;
    }

    while ((*cursor >= 'a' && *cursor <= 'z') ||
           (*cursor >= 'A' && *cursor <= 'Z') ||
           (*cursor >= '0' && *cursor <= '9')) {
        ++cursor;
    }

    return cursor != (const unsigned char *)path && *cursor == ':';
}

#endif
