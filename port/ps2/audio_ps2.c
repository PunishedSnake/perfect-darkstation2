#include <stdbool.h>
#include <stdint.h>

#include <audsrv.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>

#include "audio.h"
#include "audio_ps2_queue.h"
#include "system.h"

#define PS2_AUDIO_FREQUENCY 22050
#define PS2_AUDIO_QUEUE_LIMIT_SAMPLES 4096u

extern unsigned char audsrv_irx[] __attribute__((aligned(16)));
extern unsigned int size_audsrv_irx;

static const s16 *s_pending_buffer;
static u32 s_pending_bytes;
static bool s_audio_ready;
static u32 s_drop_count;
static u32 s_submit_error_count;

static int ps2AudioEnsureRomModule(const char *name, const char *path)
{
    int result = SifSearchModuleByName(name);
    if (result >= 0) {
        return result;
    }

    result = SifLoadModule(path, 0, NULL);
    if (result >= 0) {
        return result;
    }

    /* A loader may report a duplicate module as an error. */
    const int resident = SifSearchModuleByName(name);
    return resident >= 0 ? resident : result;
}

static int ps2AudioEnsureServer(void)
{
    int result = SifSearchModuleByName("audsrv");
    if (result >= 0) {
        return result;
    }

    /* Permit LOADFILE to execute the project-owned in-memory IRX image. */
    sbv_patch_enable_lmb();

    int module_result = 0;
    result = SifExecModuleBuffer(
        audsrv_irx, size_audsrv_irx, 0, NULL, &module_result);
    if (result >= 0) {
        return result;
    }

    const int resident = SifSearchModuleByName("audsrv");
    return resident >= 0 ? resident : result;
}

s32 audioInit(void)
{
    if (s_audio_ready) {
        return 0;
    }

    s_pending_buffer = NULL;
    s_pending_bytes = 0;
    s_drop_count = 0;
    s_submit_error_count = 0;

    sceSifInitRpc(0);

    const int libsd_module = ps2AudioEnsureRomModule("libsd", "rom0:LIBSD");
    if (libsd_module < 0) {
        sysLogPrintf(LOG_ERROR,
            "AUDIO: failed to provide ROM LIBSD module (%d)", libsd_module);
        return -1;
    }

    const int audsrv_module = ps2AudioEnsureServer();
    if (audsrv_module < 0) {
        sysLogPrintf(LOG_ERROR,
            "AUDIO: failed to execute embedded audsrv.irx (%d)", audsrv_module);
        return -1;
    }

    int result = audsrv_init();
    if (result != AUDSRV_ERR_NOERROR) {
        sysLogPrintf(LOG_ERROR,
            "AUDIO: audsrv_init failed (%d: %s)",
            result, audsrv_get_error_string());
        return -1;
    }

    struct audsrv_fmt_t format = {
        .freq = PS2_AUDIO_FREQUENCY,
        .bits = 16,
        .channels = 2,
    };
    result = audsrv_set_format(&format);
    if (result != AUDSRV_ERR_NOERROR) {
        sysLogPrintf(LOG_ERROR,
            "AUDIO: 22050 Hz stereo PCM16 format rejected (%d: %s)",
            result, audsrv_get_error_string());
        audsrv_quit();
        return -1;
    }

    result = audsrv_set_volume(MAX_VOLUME);
    if (result != AUDSRV_ERR_NOERROR) {
        sysLogPrintf(LOG_ERROR,
            "AUDIO: volume setup failed (%d: %s)",
            result, audsrv_get_error_string());
        audsrv_quit();
        return -1;
    }

    s_audio_ready = true;
    sysLogPrintf(LOG_NOTE,
        "AUDIO: native SPU2 service ready module=%d format=22050/S16/stereo queue_limit=%u",
        audsrv_module, PS2_AUDIO_QUEUE_LIMIT_SAMPLES);
    return 0;
}

s32 audioGetBytesBuffered(void)
{
    if (!s_audio_ready) {
        return 0;
    }

    const int queued = audsrv_queued();
    return queued > 0 ? queued : 0;
}

s32 audioGetSamplesBuffered(void)
{
    return audioGetBytesBuffered() / (s32)PS2_AUDIO_STEREO_FRAME_BYTES;
}

void audioSetNextBuffer(const s16 *buf, u32 len)
{
    s_pending_buffer = buf;
    s_pending_bytes = len;
}

void audioEndFrame(void)
{
    const s16 *buffer = s_pending_buffer;
    const u32 bytes = s_pending_bytes;

    /* EndFrame consumes ownership regardless of whether the service submits. */
    s_pending_buffer = NULL;
    s_pending_bytes = 0;

    if (!s_audio_ready || !buffer || bytes == 0) {
        return;
    }

    const int queued_result = audsrv_queued();
    const int available_result = audsrv_available();
    if (queued_result < 0 || available_result < 0) {
        if (s_submit_error_count++ < 4) {
            sysLogPrintf(LOG_ERROR,
                "AUDIO: queue observation failed queued=%d available=%d",
                queued_result, available_result);
        }
        return;
    }

    const enum Ps2AudioSubmitPlan plan = ps2AudioPlanSubmit(
        (u32)queued_result,
        (u32)available_result,
        bytes,
        PS2_AUDIO_QUEUE_LIMIT_SAMPLES);

    if (plan >= PS2_AUDIO_DROP_EMPTY) {
        if (s_drop_count++ < 4) {
            sysLogPrintf(LOG_WARNING,
                "AUDIO: dropped PCM frame plan=%d queued=%d available=%d bytes=%u",
                plan, queued_result, available_result, bytes);
        }
        return;
    }

    if (plan == PS2_AUDIO_SUBMIT_AFTER_WAIT) {
        const int wait_result = audsrv_wait_audio((int)bytes);
        if (wait_result != AUDSRV_ERR_NOERROR) {
            if (s_submit_error_count++ < 4) {
                sysLogPrintf(LOG_ERROR,
                    "AUDIO: wait failed (%d: %s) bytes=%u",
                    wait_result, audsrv_get_error_string(), bytes);
            }
            return;
        }
    }

    const int submitted = audsrv_play_audio((const char *)buffer, (int)bytes);
    if (submitted != (int)bytes && s_submit_error_count++ < 4) {
        sysLogPrintf(LOG_ERROR,
            "AUDIO: partial PCM submit bytes=%u submitted=%d error=%s",
            bytes, submitted, audsrv_get_error_string());
    }
}
