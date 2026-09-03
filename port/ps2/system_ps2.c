#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <kernel.h>
#include <delaythread.h>
#include <timer.h>

#include <PR/ultratypes.h>
#include "log_checkpoint_policy.h"
#include "memory_budget.h"
#include "system.h"
#include "log_ps2.h"

#define USEC_IN_SEC 1000000ULL
#define LOG_FNAME "pdps2.log"
#define LOG_STAGE_BUFFER_SIZE 8192u
#define LOG_FLUSH_THRESHOLD 6144u
#define PS2_GAME_HEAP_RESERVE_BYTES (4u * 1024u * 1024u)
#define PS2_GAME_HEAP_MINIMUM_BYTES (8u * 1024u * 1024u)
#define PS2_GAME_HEAP_ALIGNMENT 64u

#ifndef PD_PS2_GIT_COMMIT
#define PD_PS2_GIT_COMMIT "unknown"
#endif

static s32 sysArgc;
static const char **sysArgv;
static u64 startUsec;
static char logPath[1024];
static FILE *logFile;
static char logStageBuffer[LOG_STAGE_BUFFER_SIZE];
static u32 logStageUsed;
static u64 logLastDurableUsec;
static bool logHasDurableCheckpoint;

extern char _end;
/* PS2SDK's libc glue exports the allocator break through _sbrk directly. */
extern void *_sbrk(size_t increment);

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
         * Preserve the device prefix so callers can safely append a filename.
         * Exact argv[0] behaviour remains loader-dependent.
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

static void sysLogDisableFileSink(const char *reason)
{
    if (logFile) {
        FILE *closing = logFile;
        logFile = NULL;
        fclose(closing);
    }

    if (reason && reason[0]) {
        fprintf(stderr, "LOGGER: file sink disabled: %s\n", reason);
    }

    logPath[0] = '\0';
    logStageUsed = 0;
}

static void sysLogFlushInternal(void)
{
    if (!logFile || logStageUsed == 0) {
        return;
    }

    const size_t written = fwrite(logStageBuffer, 1, logStageUsed, logFile);
    if (written != logStageUsed || fflush(logFile) != 0) {
        sysLogDisableFileSink("write/flush failed");
        return;
    }

    logStageUsed = 0;
}

void ps2LogFlush(void)
{
    sysLogFlushInternal();
    fflush(stdout);
    fflush(stderr);
}

static void ps2LogCheckpointInternal(bool force)
{
    char reopenPath[sizeof(logPath)];

    /*
     * CURRENT PS2SDK: fsync() returns ENOSYS. More importantly for mass:, a
     * stdio flush may not publish the final file size/directory metadata while
     * the underlying descriptor stays open. A bring-up checkpoint therefore
     * deliberately closes the descriptor and reopens it in append mode.
     *
     * mass: implementations have also shown finite progress across dense
     * close/reopen cycles. Throttle ordinary checkpoints while allowing an
     * explicit runtime snapshot or fatal path to force durability.
     */
    ps2LogFlush();

    if (!logFile || !logPath[0]) {
        return;
    }

    const u64 now = sysGetMicroseconds();
    if (!ps2LogCheckpointShouldClose(
            now, logLastDurableUsec, logHasDurableCheckpoint, force)) {
        return;
    }

    strncpy(reopenPath, logPath, sizeof(reopenPath) - 1);
    reopenPath[sizeof(reopenPath) - 1] = '\0';

    FILE *closing = logFile;
    logFile = NULL;

    if (fclose(closing) != 0) {
        fprintf(stderr, "LOGGER: durable checkpoint close failed: %s\n", reopenPath);
        logPath[0] = '\0';
        logStageUsed = 0;
        return;
    }

    logFile = fopen(reopenPath, "ab");
    if (!logFile) {
        fprintf(stderr, "LOGGER: checkpoint persisted but reopen failed: %s\n", reopenPath);
        logPath[0] = '\0';
        logStageUsed = 0;
        return;
    }

    logLastDurableUsec = now;
    logHasDurableCheckpoint = true;
}

void ps2LogCheckpoint(void)
{
    ps2LogCheckpointInternal(false);
}

void ps2LogCheckpointForce(void)
{
    ps2LogCheckpointInternal(true);
}

static void sysLogSetPath(const char *fname)
{
    char base[768];
    pathBaseFromArgv0(base, sizeof(base));

    snprintf(logPath, sizeof(logPath), "%s/%s", base, fname);

    /* Start each run with a fresh log. The first checkpoint reopens as append. */
    logLastDurableUsec = 0;
    logHasDurableCheckpoint = false;
    logFile = fopen(logPath, "wb");
    if (!logFile) {
        logPath[0] = '\0';
    }
}

static void sysLogStageLine(const char *line, u32 length)
{
    if (!logFile || !line || length == 0) {
        return;
    }

    if (length > LOG_STAGE_BUFFER_SIZE) {
        sysLogFlushInternal();
        if (!logFile) {
            return;
        }
        if (fwrite(line, 1, length, logFile) != length || fflush(logFile) != 0) {
            sysLogDisableFileSink("oversized write failed");
        }
        return;
    }

    if (logStageUsed + length > LOG_STAGE_BUFFER_SIZE) {
        sysLogFlushInternal();
        if (!logFile) {
            return;
        }
    }

    memcpy(logStageBuffer + logStageUsed, line, length);
    logStageUsed += length;

    if (logStageUsed >= LOG_FLUSH_THRESHOLD) {
        sysLogFlushInternal();
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

    /*
     * Bring-up logging is active by default. --no-log is the explicit escape
     * hatch for timing-sensitive experiments. Console output remains active.
     */
    if (!sysArgCheck("--no-log")) {
        sysLogSetPath(LOG_FNAME);
    }

    sysLogPrintf(LOG_NOTE, "Perfect DarkStation 2 logger online");
    sysLogPrintf(LOG_NOTE, "build commit: %s", PD_PS2_GIT_COMMIT);
    sysLogPrintf(LOG_NOTE, "compiler: %s", __VERSION__);
    sysLogPrintf(LOG_NOTE, "file sink: %s", logFile ? logPath : "unavailable/disabled; console only");
    sysLogPrintf(LOG_NOTE,
        "file checkpoint policy: ordinary close/reopen interval=%llu us; runtime snapshots forced",
        (unsigned long long)PS2_LOG_DURABLE_INTERVAL_USEC);

    for (s32 i = 0; i < sysArgc; ++i) {
        sysLogPrintf(LOG_NOTE, "argv[%d]: %s", i, sysArgv[i] ? sysArgv[i] : "(null)");
    }

    /* Publish the initial file length immediately on filesystem-backed runs. */
    ps2LogCheckpointForce();
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
    return logFile != NULL;
}

void sysLogPrintf(s32 level, const char *fmt, ...)
{
    static const char *const label[] = { "INFO", "WARN", "ERROR" };
    const u32 safeLevel =
        (level >= LOG_NOTE && level <= LOG_ERROR) ? (u32)level : LOG_ERROR;
    char msg[1536];
    char line[1728];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    const int lineLength = snprintf(line, sizeof(line), "[%10llu us] %-5s %s\n",
        (unsigned long long)sysGetMicroseconds(), label[safeLevel], msg);
    const u32 safeLength = lineLength > 0
        ? (u32)((lineLength < (int)sizeof(line)) ? lineLength : (int)sizeof(line) - 1)
        : 0;

    FILE *out = safeLevel == LOG_NOTE ? stdout : stderr;
    if (safeLength) {
        fwrite(line, 1, safeLength, out);
        sysLogStageLine(line, safeLength);
    }

    /* Warnings/errors are rare; flush now and let explicit checkpoints close. */
    if (safeLevel >= LOG_WARNING) {
        ps2LogFlush();
    }
}

void sysFatalError(const char *fmt, ...)
{
    static bool fatalActive;
    char msg[1536];

    if (fatalActive) {
        for (;;) {
            DelayThread(1000000);
        }
    }

    fatalActive = true;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    sysLogPrintf(LOG_ERROR, "FATAL: %s", msg);
    ps2LogCheckpointForce();

    if (logFile) {
        FILE *closing = logFile;
        logFile = NULL;
        fclose(closing);
    }

    /*
     * Returning from the ELF drops back to the browser/OSD and used to make a
     * controlled fatal error look like a console reset. Keep the EE alive so
     * the last displayed frame and the durable log survive until manual reset.
     */
    fprintf(stderr, "Perfect DarkStation 2 stopped after a fatal error.\n");
    fflush(stderr);
    for (;;) {
        DelayThread(1000000);
    }
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

u32 sysMemGetGameHeapSize(u32 requestedSize)
{
    void *const heapCursor = _sbrk(0u);
    void *const heapEnd = EndOfHeap();
    const u32 minimum = requestedSize < PS2_GAME_HEAP_MINIMUM_BYTES
        ? requestedSize : PS2_GAME_HEAP_MINIMUM_BYTES;
    struct Ps2GameHeapPlan plan;

    if (heapCursor == (void *)-1 ||
        !ps2PlanGameHeap(
            (uintptr_t)heapCursor, (uintptr_t)heapEnd,
            requestedSize, PS2_GAME_HEAP_RESERVE_BYTES,
            minimum, PS2_GAME_HEAP_ALIGNMENT, &plan)) {
        sysLogPrintf(LOG_ERROR,
            "MEM: cannot plan game heap physical=%d KiB image_end=%p "
            "libc_break=%p heap_end=%p requested=%u KiB reserve=%u KiB",
            GetMemorySize() / 1024, &_end, heapCursor, heapEnd,
            requestedSize / 1024u,
            PS2_GAME_HEAP_RESERVE_BYTES / 1024u);
        return 0u;
    }

    sysLogPrintf(LOG_NOTE,
        "MEM: physical=%d KiB image_end=%p libc_break=%p heap_end=%p "
        "tail=%u KiB reserve=%u KiB game_requested=%u KiB game_planned=%u KiB",
        GetMemorySize() / 1024, &_end, heapCursor, heapEnd,
        plan.tail_bytes / 1024u, plan.reserve_bytes / 1024u,
        plan.requested_bytes / 1024u, plan.planned_bytes / 1024u);
    return plan.planned_bytes;
}

void sysSleep(const s64 hns)
{
    if (hns <= 0) {
        return;
    }

    /* hns is 100 ns. Current PS2SDK DelayThread accepts signed microseconds. */
    u64 usec = ((u64)hns + 9ULL) / 10ULL;

    while (usec) {
        const s32 chunk = usec > (u64)INT_MAX ? INT_MAX : (s32)usec;
        DelayThread(chunk);
        usec -= (u32)chunk;
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

bool g_CrashEnabled = false;

void crashInit(void)
{
    /* Fatal paths remain log-owned until an EE exception handler is installed. */
    g_CrashEnabled = false;
}

void crashCreateThread(void)
{
    /* The portable crash monitor is a desktop service, not an EE thread. */
}

void crashSetMessage(char *string)
{
    (void)string;
}

void crashReset(void)
{
}

void crashAppendChar(char c)
{
    (void)c;
}

void crashShutdown(void)
{
    ps2LogCheckpointForce();

    if (logFile) {
        FILE *closing = logFile;
        logFile = NULL;
        fclose(closing);
    }
}
