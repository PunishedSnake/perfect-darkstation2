#ifndef PD_PS2_PATH_PS2_H
#define PD_PS2_PATH_PS2_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/*
 * wLaunchELF R3Z normalises USB execution paths to massN:/... before handing
 * argv[0] to the target. The direct FMCB hotkey path can preserve the legacy
 * mass:/... alias instead. Keep this transform explicit and narrow so we can
 * A/B the launcher contract without rewriting arbitrary device paths.
 */
static inline bool ps2PathCanonicalizeUsbMassToLegacy(
    char *path, size_t capacity)
{
    if (!path || capacity == 0) {
        return false;
    }

    if (strncmp(path, "mass0:", 6) == 0) {
        path[4] = ':';
        memmove(path + 5, path + 6, strlen(path + 6) + 1);
        return true;
    }

    if (strncmp(path, "usb0:", 5) == 0) {
        memcpy(path, "mass:", 5);
        return true;
    }

    if (strncmp(path, "usb:", 4) == 0) {
        const size_t length = strlen(path);
        if (length + 2 > capacity) {
            return false;
        }

        memmove(path + 5, path + 4, length - 4 + 1);
        memcpy(path, "mass:", 5);
        return true;
    }

    return false;
}

enum Ps2BootMedium {
    PS2_BOOT_MEDIUM_UNKNOWN = 0,
    PS2_BOOT_MEDIUM_USB,
    PS2_BOOT_MEDIUM_HDD,
    PS2_BOOT_MEDIUM_MC,
    PS2_BOOT_MEDIUM_CDVD,
    PS2_BOOT_MEDIUM_HOST,
    PS2_BOOT_MEDIUM_NETWORK,
    PS2_BOOT_MEDIUM_ROM,
};

static inline enum Ps2BootMedium ps2PathClassifyBootMedium(const char *path)
{
    if (!path || !path[0]) {
        return PS2_BOOT_MEDIUM_UNKNOWN;
    }

    if (!strncmp(path, "mass:", 5) || !strncmp(path, "mass0:", 6) ||
        !strncmp(path, "mass1:", 6) || !strncmp(path, "usb:", 4) ||
        !strncmp(path, "usb0:", 5) || !strncmp(path, "usb1:", 5)) {
        return PS2_BOOT_MEDIUM_USB;
    }

    /*
     * PS2SDK's ELF loader can preserve an APA partition in argv[0] as
     * hdd0:<partition>:pfs:/path. Other launchers may expose hdd0:/... or an
     * already-mounted pfs0:/... path.
     */
    if (!strncmp(path, "hdd0:", 5) || !strncmp(path, "hdd1:", 5) ||
        !strncmp(path, "pfs0:", 5) || !strncmp(path, "pfs1:", 5)) {
        return PS2_BOOT_MEDIUM_HDD;
    }

    if (!strncmp(path, "mc0:", 4) || !strncmp(path, "mc1:", 4)) {
        return PS2_BOOT_MEDIUM_MC;
    }

    if (!strncmp(path, "cdfs:", 5) || !strncmp(path, "cdrom0:", 7) ||
        !strncmp(path, "cdrom1:", 7)) {
        return PS2_BOOT_MEDIUM_CDVD;
    }

    if (!strncmp(path, "host:", 5) || !strncmp(path, "host0:", 6)) {
        return PS2_BOOT_MEDIUM_HOST;
    }

    if (!strncmp(path, "smb:", 4) || !strncmp(path, "smb0:", 5)) {
        return PS2_BOOT_MEDIUM_NETWORK;
    }

    if (!strncmp(path, "rom0:", 5) || !strncmp(path, "rom1:", 5)) {
        return PS2_BOOT_MEDIUM_ROM;
    }

    return PS2_BOOT_MEDIUM_UNKNOWN;
}

static inline const char *ps2PathBootMediumName(enum Ps2BootMedium medium)
{
    switch (medium) {
        case PS2_BOOT_MEDIUM_USB: return "usb";
        case PS2_BOOT_MEDIUM_HDD: return "hdd";
        case PS2_BOOT_MEDIUM_MC: return "memory-card";
        case PS2_BOOT_MEDIUM_CDVD: return "cdvd";
        case PS2_BOOT_MEDIUM_HOST: return "host";
        case PS2_BOOT_MEDIUM_NETWORK: return "network";
        case PS2_BOOT_MEDIUM_ROM: return "rom";
        case PS2_BOOT_MEDIUM_UNKNOWN:
        default: return "unknown";
    }
}

/*
 * Recover the APA partition and path from a boot path that still carries HDD
 * context. Supported forms include:
 *
 *   hdd0:__common:pfs:/APPS/PD/BOOT.ELF
 *   hdd0:/__common/APPS/PD/BOOT.ELF
 *   hdd0:__common:/APPS/PD/BOOT.ELF
 *
 * A bare pfs0:/... path is classified as HDD but intentionally fails here:
 * after a clean IOP reboot its mount number alone is not enough to discover
 * which APA partition must be mounted again.
 */
static inline bool ps2PathParseHddBootContext(
    const char *path,
    char *partition, size_t partition_capacity,
    char *relative, size_t relative_capacity)
{
    if (!path || (!partition && partition_capacity) ||
        (!relative && relative_capacity)) {
        return false;
    }

    size_t device_len;
    if (!strncmp(path, "hdd0:", 5) || !strncmp(path, "hdd1:", 5)) {
        device_len = 5;
    } else {
        return false;
    }

    const char *cursor = path + device_len;
    if (*cursor == '/') {
        ++cursor;
    }

    const char *part_begin = cursor;
    while (*cursor && *cursor != ':' && *cursor != '/') {
        ++cursor;
    }

    const size_t part_len = (size_t)(cursor - part_begin);
    if (part_len == 0) {
        return false;
    }

    if (partition) {
        const size_t needed = device_len + part_len + 1;
        if (needed > partition_capacity) {
            return false;
        }
        memcpy(partition, path, device_len);
        memcpy(partition + device_len, part_begin, part_len);
        partition[device_len + part_len] = '\0';
    }

    const char *path_begin = cursor;

    if (*path_begin == ':') {
        ++path_begin;
        if (!strncmp(path_begin, "pfs:", 4)) {
            path_begin += 4;
        } else if (!strncmp(path_begin, "pfs0:", 5) ||
                   !strncmp(path_begin, "pfs1:", 5)) {
            path_begin += 5;
        }
    }

    if (*path_begin == '\0') {
        path_begin = "/";
    } else if (*path_begin != '/') {
        const char *slash = strchr(path_begin, '/');
        if (!slash) {
            return false;
        }
        path_begin = slash;
    }

    if (relative) {
        const size_t path_len = strlen(path_begin);
        if (path_len + 1 > relative_capacity) {
            return false;
        }
        memcpy(relative, path_begin, path_len + 1);
    }

    return true;
}

static inline bool ps2PathCanonicalizeHddToPfs0(
    char *path, size_t capacity)
{
    char partition[96];
    char relative[768];

    if (!path || capacity == 0 ||
        !ps2PathParseHddBootContext(path,
            partition, sizeof(partition), relative, sizeof(relative))) {
        return false;
    }

    const size_t relative_len = strlen(relative);
    if (5 + relative_len + 1 > capacity) {
        return false;
    }

    memcpy(path, "pfs0:", 5);
    memcpy(path + 5, relative, relative_len + 1);
    return true;
}

static inline bool ps2PathCanonicalizeOwnedBootPath(
    char *path, size_t capacity)
{
    if (ps2PathCanonicalizeUsbMassToLegacy(path, capacity)) {
        return true;
    }

    return ps2PathCanonicalizeHddToPfs0(path, capacity);
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
