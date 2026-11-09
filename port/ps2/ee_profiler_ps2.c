/*
 * Debug-only statistical EE profiler (PD_PS2_EE_PROFILER).
 *
 * A ps2sdk timer counter fires every PD_PS2_EE_PROFILER_PERIOD_US and records
 * the interrupted EE program counter. Unlike host-side sampling of an
 * emulator, this measures real EE time on hardware or in PCSX2, and is not
 * distorted by emulation cost or frame limiting. The raw PCs are written to
 * PD_PS2_EE_PROFILER_PATH once the buffer fills; symbolize them offline
 * against the matching ELF.
 */
#include <stdint.h>
#include <stdio.h>
#include <tamtypes.h>
#include <timer.h>
#include <kernel.h>

#include "log_ps2.h"
#include "system.h"

#if PD_PS2_EE_PROFILER

#ifndef PD_PS2_EE_PROFILER_SAMPLES
#define PD_PS2_EE_PROFILER_SAMPLES 65536u
#endif
#ifndef PD_PS2_EE_PROFILER_PERIOD_US
#define PD_PS2_EE_PROFILER_PERIOD_US 397u /* prime-ish: avoid locking to vsync */
#endif
#ifndef PD_PS2_EE_PROFILER_PATH
#define PD_PS2_EE_PROFILER_PATH "host:pd_ee_pcs.bin"
#endif

static u32 s_prof_pcs[PD_PS2_EE_PROFILER_SAMPLES];
static volatile u32 s_prof_count;
static s32 s_prof_timer = -1;
static int s_prof_state; /* 0 idle, 1 sampling, 2 dumped */

/* EE timer 1 compare interrupt; the kernel passes the interrupted PC. */
static s32 eeProfilerTimerHandler(s32 cause, void *arg, void *addr)
{
    (void)cause;
    (void)arg;
    const u32 epc = (u32)(uintptr_t)addr;
    const u32 n = s_prof_count;
    if (n < PD_PS2_EE_PROFILER_SAMPLES) {
        s_prof_pcs[n] = epc;
        s_prof_count = n + 1u;
    }
    *T1_MODE = *T1_MODE | (1u << 10); /* EQUF is write-one-to-clear */
    ExitHandler();
    return 0;
}

void ps2EeProfilerStart(void)
{
    if (s_prof_state != 0) {
        return;
    }
    s_prof_count = 0;
    /* BUSCLK/16 = 9.216 MHz */
    const u32 comp = (u32)((9216000ull * PD_PS2_EE_PROFILER_PERIOD_US) / 1000000ull);
    s_prof_timer = AddIntcHandler2(INTC_TIM1, eeProfilerTimerHandler, 0, NULL);
    if (s_prof_timer < 0) {
        sysLogPrintf(LOG_ERROR, "EE profiler: AddIntcHandler failed");
        s_prof_state = 2;
        return;
    }
    *T1_MODE = 0;
    *T1_COUNT = 0;
    *T1_COMP = comp;
    *T1_MODE = Tn_MODE(1, 0, 0, 0, 1, 1, 1, 0, 1, 1);
    EnableIntc(INTC_TIM1);
    s_prof_state = 1;
    sysLogPrintf(LOG_NOTE, "EE profiler: sampling %u PCs every %u us (T1 comp=%u)",
        PD_PS2_EE_PROFILER_SAMPLES, PD_PS2_EE_PROFILER_PERIOD_US, (unsigned)comp);
}

void ps2EeProfilerPoll(void)
{
    static u32 s_logged;
    if (s_prof_state == 1 && s_prof_count / 8192u != s_logged) {
        s_logged = s_prof_count / 8192u;
        sysLogPrintf(LOG_NOTE, "EE profiler: %u samples", (unsigned)s_prof_count);
    }
    if (s_prof_state != 1 || s_prof_count < PD_PS2_EE_PROFILER_SAMPLES) {
        return;
    }
    DisableIntc(INTC_TIM1);
    *T1_MODE = 0;
    RemoveIntcHandler(INTC_TIM1, s_prof_timer);
    s_prof_state = 2;
    FILE *f = fopen(PD_PS2_EE_PROFILER_PATH, "wb");
    if (!f) {
        sysLogPrintf(LOG_ERROR, "EE profiler: cannot open %s", PD_PS2_EE_PROFILER_PATH);
        return;
    }
    fwrite(s_prof_pcs, sizeof(u32), PD_PS2_EE_PROFILER_SAMPLES, f);
    fclose(f);
    sysLogPrintf(LOG_NOTE, "EE profiler: wrote %u samples to %s",
        PD_PS2_EE_PROFILER_SAMPLES, PD_PS2_EE_PROFILER_PATH);
}

#else

void ps2EeProfilerStart(void) {}
void ps2EeProfilerPoll(void) {}

#endif
