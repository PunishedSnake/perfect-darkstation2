#include <stdio.h>
#include <string.h>

#include "system.h"

#define BOOTSTRAP_HEAP_SMOKE_SIZE 4096u

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
    sysLogPrintf(LOG_NOTE, "bootstrap completed in %llu us",
        (unsigned long long)sysGetMicroseconds());

    return 0;
}
