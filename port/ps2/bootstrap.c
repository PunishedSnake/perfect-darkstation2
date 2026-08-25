#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "romsource.h"
#include "system.h"
#include "log_ps2.h"

#define BOOTSTRAP_HEAP_SMOKE_SIZE 4096u
#define DEFAULT_ROM_NAME "pd.ntsc-final.z64"

bool ps2VideoDiagRun(int rom_status);
int ps2RomProbe(const char *path);

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
    sysLogPrintf(LOG_NOTE, "ROM source memory smoke: ok");
}

static const char *bootstrapResolveRomPath(char *path, u32 pathLen)
{
    const char *explicitPath = sysArgGetString("--rom-file");

    if (explicitPath && explicitPath[0]) {
        return explicitPath;
    }

    char base[768];
    sysGetExecutablePath(base, sizeof(base));
    snprintf(path, pathLen, "%s/%s", base, DEFAULT_ROM_NAME);
    return path;
}

int main(int argc, char **argv)
{
    sysInitArgs(argc, (const char **)argv);
    sysInit();

    sysLogPrintf(LOG_NOTE, "Perfect DarkStation 2 bootstrap");
    sysLogPrintf(LOG_NOTE, "platform: r5900-ps2");
    sysLogPrintf(LOG_NOTE, "system backend: ok");

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

    sysLogPrintf(LOG_NOTE, "heap smoke: ok");
    bootstrapTestRomSource();
    ps2LogFlush();

    char autoRomPath[1024];
    const char *romPath = bootstrapResolveRomPath(autoRomPath, sizeof(autoRomPath));
    sysLogPrintf(LOG_NOTE, "ROM probe path: %s", romPath);

    const u64 romProbeStart = sysGetMicroseconds();
    const int rom_status = ps2RomProbe(romPath);
    const u64 romProbeEnd = sysGetMicroseconds();

    sysLogPrintf(LOG_NOTE, "ROM probe status=%d duration=%llu us",
        rom_status, (unsigned long long)(romProbeEnd - romProbeStart));
    ps2LogFlush();

    sysLogPrintf(LOG_NOTE, "bootstrap pre-GS checks completed in %llu us",
        (unsigned long long)sysGetMicroseconds());

    sysLogPrintf(LOG_NOTE, "GS diagnostic: starting");
    sysLogPrintf(LOG_NOTE, "bars: green=EE/system, blue=GIF/GS, third=ROM data path");
    sysLogPrintf(LOG_NOTE, "ROM status: green=header+streamed RZIP+file table ok, red=failed");
    ps2LogFlush();

    if (!ps2VideoDiagRun(rom_status)) {
        sysFatalError("GS diagnostic initialisation failed");
    }

    return 0;
}
