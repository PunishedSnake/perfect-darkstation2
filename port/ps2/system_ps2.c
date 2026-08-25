#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <kernel.h>
#include <timer.h>

#include <PR/ultratypes.h>
#include "system.h"

#define USEC_IN_SEC 1000000ULL
#define LOG_FNAME "pd.log"

static s32 sysArgc;
static const char **sysArgv;
static u64 startUsec;
static char logPath[1024];

static u64 timerUsec(void)
{
    u32 sec = 0;
    u32 usec = 0;

    TimerBusClock2USec(GetTimerSystemTime(), &sec, &usec);
    return (u64)sec * USEC_IN_SEC + (u64)usec;
}

static void pathBaseFromArgv0(char *outPath, u32 outLen)
{
    if (!outPath || outLen == 0) {
        return;
    }

    outPath[0] = '\0';

    if (sysArgc > 0 && sysArgv && sysArgv[0] && sysArgv[0][0]) {
        strncpy(outPath, sysArgv[0], outLen - 1);
        outPath[outLen - 1] = '\0';

        char *lastSlash = strrchr(outPath, '/');
        if (lastSlash) {
            *lastSlash = '\0';
            return;
        }

        /*
         * PS2 device paths can arrive as host:foo.elf, mass:foo.elf, etc.
         * Preserve the device prefix so callers can safely append /data or
         * a log filename. Exact argv[0] behaviour remains loader-dependent.
         */
        char *deviceColon = strchr(outPath, ':');
        if (deviceColon) {
            deviceColon[1] = '\0';
            return;
        }
    }

    if (outLen >= 2) {
        outPath[0] = '.';
        outPath[1] = '\0';
    }
}

static void sysLogSetPath(const char *fname)
{
    char base[768];
    pathBaseFromArgv0(base, sizeof(base));

    snprintf(logPath, sizeof(logPath), "%s/%s", base, fname);

    FILE *f = fopen(logPath, "wb");
    if (f) {
        fclose(f);
    } else {
        logPath[0] = '\0';
    }
}

void sysInitArgs(s32 argc, const char **argv)
{
    sysArgc = argc;
    sysArgv = argv;
}

void sysInit(void)
{
    startUsec = timerUsec();

    if (sysArgCheck("--log")) {
        sysLogSetPath(LOG_FNAME);
    }
}

s32 sysArgCheck(const char *arg)
{
    if (!arg) {
        return 0;
    }

    for (s32 i = 1; i < sysArgc; ++i) {
        if (sysArgv[i] && !strcasecmp(sysArgv[i], arg)) {
            return 1;
        }
    }

    return 0;
}

const char *sysArgGetString(const char *arg)
{
    if (!arg) {
        return NULL;
    }

    for (s32 i = 1; i < sysArgc; ++i) {
        if (sysArgv[i] && !strcasecmp(sysArgv[i], arg)) {
            return (i + 1 < sysArgc) ? sysArgv[i + 1] : NULL;
        }
    }

    return NULL;
}

s32 sysArgGetInt(const char *arg, s32 defval)
{
    const char *value = sysArgGetString(arg);
    return value ? (s32)strtol(value, NULL, 0) : defval;
}

u64 sysGetMicroseconds(void)
{
    const u64 now = timerUsec();
    return now >= startUsec ? now - startUsec : 0;
}

s32 sysLogIsOpen(void)
{
    return logPath[0] != '\0';
}

void sysLogPrintf(s32 level, const char *fmt, ...)
{
    static const char *const prefix[] = { "", "WARNING: ", "ERROR: " };
    const u32 safeLevel =
        (level >= LOG_NOTE && level <= LOG_ERROR) ? (u32)level : LOG_ERROR;
    char msg[1536];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    FILE *out = safeLevel == LOG_NOTE ? stdout : stderr;
    fprintf(out, "%s%s\n", prefix[safeLevel], msg);

    if (logPath[0]) {
        FILE *f = fopen(logPath, "ab");
        if (f) {
            fprintf(f, "%s%s\n", prefix[safeLevel], msg);
            fclose(f);
        }
    }
}

void sysFatalError(const char *fmt, ...)
{
    char msg[1536];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    sysLogPrintf(LOG_ERROR, "FATAL: %s", msg);
    fflush(stdout);
    fflush(stderr);
    exit(1);
}

void sysGetExecutablePath(char *outPath, const u32 outLen)
{
    pathBaseFromArgv0(outPath, outLen);
}

void sysGetHomePath(char *outPath, const u32 outLen)
{
    /*
     * There is no desktop-style HOME policy on PS2. For bootstrap 0 use the
     * ELF's device/directory as the least surprising fallback. Save/config
     * ownership gets its own device policy when the real filesystem layer is
     * brought up.
     */
    pathBaseFromArgv0(outPath, outLen);
}

void *sysMemAlloc(const u32 size)
{
    return malloc(size);
}

void *sysMemZeroAlloc(const u32 size)
{
    return calloc(1, size);
}

void *sysMemRealloc(void *ptr, const u32 newSize)
{
    return realloc(ptr, newSize);
}

void sysMemFree(void *ptr)
{
    free(ptr);
}

void sysSleep(const s64 hns)
{
    if (hns <= 0) {
        return;
    }

    /* hns is 100 ns. DelayThread accepts microseconds. */
    u64 usec = ((u64)hns + 9ULL) / 10ULL;

    while (usec) {
        const u32 chunk = usec > UINT_MAX ? UINT_MAX : (u32)usec;
        DelayThread(chunk);
        usec -= chunk;
    }
}

void sysCpuRelax(void)
{
    /*
     * Keep this a true spin hint for now. Turning it into a scheduler yield
     * would change call-site behaviour and needs workload evidence first.
     */
    __asm__ volatile("nop");
}

void crashInit(void)
{
    /* bootstrap 0: no platform crash handler yet */
}

void crashShutdown(void)
{
}
