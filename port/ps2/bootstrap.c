#include <stdio.h>
#include <string.h>

#include "romsource.h"
#include "system.h"

#define BOOTSTRAP_HEAP_SMOKE_SIZE 4096u

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
    puts("ROM source smoke: ok");
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

    sysLogPrintf(LOG_NOTE, "bootstrap completed in %llu us",
        (unsigned long long)sysGetMicroseconds());

    return 0;
}
