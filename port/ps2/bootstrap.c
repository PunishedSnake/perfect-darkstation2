#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "romsource.h"
#include "system.h"

#define BOOTSTRAP_HEAP_SMOKE_SIZE 4096u
#define PERFECT_DARK_ROM_SIZE (32u * 1024u * 1024u)

bool ps2VideoDiagRun(int rom_status);

static void bootstrapTestRomSource(void)
{
    static const unsigned char sourceData[] = {
        0x10, 0x11, 0x12, 0x13,
        0x20, 0x21, 0x22, 0x23,
        0x30, 0x31, 0x32, 0x33,
        0x40, 0x41, 0x42, 0x43,
    };

    struct romsource source = { 0 };
    unsigned char copy[4] = { 0 };

    romSourceInitMemory(&source, sourceData, sizeof(sourceData));

    if (romSourceGetSize(&source) != sizeof(sourceData)) {
        sysFatalError("ROM source size contract failed");
    }

    const u8 *view = romSourceView(&source, 8, 4);
    if (!view || memcmp(view, &sourceData[8], 4) != 0) {
        sysFatalError("ROM source resident view contract failed");
    }

    if (!romSourceReadAt(&source, 4, copy, sizeof(copy)) ||
        memcmp(copy, &sourceData[4], sizeof(copy)) != 0) {
        sysFatalError("ROM source read_at contract failed");
    }

    if (romSourceReadAt(&source, 14, copy, sizeof(copy))) {
        sysFatalError("ROM source bounds contract failed");
    }

    romSourceClose(&source);
    puts("ROM source memory smoke: ok");
}

/*
 * Optional real-file smoke test.
 *
 * The prototype never retains the 32 MiB ROM. It reads two bounded 64/16-byte
 * ranges into caller-owned storage, which is the contract the later romdata
 * migration will build on. Pass a ROM path as argv[1] when the launcher can
 * supply arguments; otherwise the visual prototype still runs without it.
 */
static int bootstrapTestRomFile(const char *path)
{
    if (!path || !path[0]) {
        puts("ROM file: not requested (pass path as argv[1] for bounded I/O smoke)");
        return 0;
    }

    struct romsource source = { 0 };
    unsigned char header[64] = { 0 };
    unsigned char tail[16] = { 0 };
    int status = 1;

    if (!romSourceOpenFile(&source, path)) {
        printf("ROM file: open failed: %s\n", path);
        return -1;
    }

    const u32 size = romSourceGetSize(&source);
    printf("ROM file: %s (%u bytes)\n", path, size);

    if (size != PERFECT_DARK_ROM_SIZE) {
        printf("ROM file: unexpected size, expected %u bytes\n", PERFECT_DARK_ROM_SIZE);
        status = -1;
    }

    if (size < sizeof(header) || !romSourceReadAt(&source, 0, header, sizeof(header))) {
        puts("ROM file: bounded header read failed");
        status = -1;
    } else {
        printf("ROM file: first bytes %02x %02x %02x %02x\n",
            header[0], header[1], header[2], header[3]);
    }

    if (size < sizeof(tail) ||
        !romSourceReadAt(&source, size - sizeof(tail), tail, sizeof(tail))) {
        puts("ROM file: bounded tail read failed");
        status = -1;
    }

    if (size >= 8 && romSourceReadAt(&source, size - 8u, tail, sizeof(tail))) {
        puts("ROM file: out-of-bounds read unexpectedly succeeded");
        status = -1;
    }

    romSourceClose(&source);

    if (status > 0) {
        puts("ROM file bounded I/O smoke: ok");
    }

    return status;
}

int main(int argc, char **argv)
{
    sysInitArgs(argc, (const char **)argv);
    sysInit();

    puts("Perfect DarkStation 2 bootstrap");
    puts("platform: r5900-ps2");
    puts("system backend: ok");

    void *p = sysMemZeroAlloc(BOOTSTRAP_HEAP_SMOKE_SIZE);

    if (!p) {
        sysFatalError("bootstrap heap allocation failed");
    }

    for (unsigned int i = 0; i < BOOTSTRAP_HEAP_SMOKE_SIZE; ++i) {
        if (((unsigned char *)p)[i] != 0) {
            sysFatalError("bootstrap calloc contract failed at byte %u", i);
        }
    }

    memset(p, 0x5a, BOOTSTRAP_HEAP_SMOKE_SIZE);
    sysMemFree(p);

    puts("heap smoke: ok");
    bootstrapTestRomSource();

    const int rom_status = bootstrapTestRomFile(argc > 1 ? argv[1] : NULL);

    sysLogPrintf(LOG_NOTE, "bootstrap pre-GS checks completed in %llu us",
        (unsigned long long)sysGetMicroseconds());

    puts("GS diagnostic: starting");
    puts("bars: green=EE/system, blue=GIF/GS, third=ROM status");
    puts("ROM status: green=bounded file I/O ok, amber=not requested, red=failed");

    if (!ps2VideoDiagRun(rom_status)) {
        sysFatalError("GS diagnostic initialisation failed");
    }

    return 0;
}
