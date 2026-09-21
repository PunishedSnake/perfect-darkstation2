#ifndef PD_PS2_PATH_PS2_H
#define PD_PS2_PATH_PS2_H

#include <stdbool.h>
#include <string.h>

/*
 * wLaunchELF R3Z normalises USB execution paths to massN:/... before handing
 * argv[0] to the target. The direct FMCB hotkey path can preserve the legacy
 * mass:/... alias instead. Keep this transform explicit and narrow so we can
 * A/B the launcher contract without rewriting arbitrary device paths.
 */
static inline bool ps2PathCanonicalizeMass0ToLegacy(char *path)
{
    if (!path || strncmp(path, "mass0:", 6) != 0) {
        return false;
    }

    path[4] = ':';
    memmove(path + 5, path + 6, strlen(path + 6) + 1);
    return true;
}

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
