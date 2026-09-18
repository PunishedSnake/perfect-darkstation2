#include <stdint.h>

#include <delaythread.h>
#include <PR/ultratypes.h>

#include "audio.h"
#include "system.h"

#define AUDIO_DIAG_SAMPLE_RATE 22050u
#define AUDIO_DIAG_FRAMES_PER_CHUNK 368u
#define AUDIO_DIAG_TONE_HZ 440u
#define AUDIO_DIAG_AMPLITUDE 6000

static int16_t s_tone[AUDIO_DIAG_FRAMES_PER_CHUNK * 2u]
    __attribute__((aligned(64)));

static void audioDiagFillTone(uint32_t *phase)
{
    const uint32_t step = (uint32_t)(((uint64_t)AUDIO_DIAG_TONE_HZ << 32) /
        AUDIO_DIAG_SAMPLE_RATE);

    for (uint32_t frame = 0u; frame < AUDIO_DIAG_FRAMES_PER_CHUNK; ++frame) {
        const int16_t sample = (*phase & 0x80000000u)
            ? AUDIO_DIAG_AMPLITUDE
            : -AUDIO_DIAG_AMPLITUDE;
        s_tone[frame * 2u] = sample;
        s_tone[frame * 2u + 1u] = sample;
        *phase += step;
    }
}

int main(int argc, char **argv)
{
    sysInitArgs(argc, (const char **)argv);
    sysInit();

    if (audioInit() < 0) {
        sysFatalError("PS2 audio diagnostic could not initialise audsrv/SPU2");
    }

    sysLogPrintf(LOG_NOTE,
        "AUDIO DIAG: continuous 440 Hz square wave; SPU2 path is alive if audible");

    uint32_t phase = 0u;

    for (;;) {
        audioDiagFillTone(&phase);
        audioSetNextBuffer(s_tone, sizeof(s_tone));
        audioEndFrame();

        /* One chunk is about 16.7 ms. Keep the producer independent from GS
         * while leaving audsrv enough headroom to absorb scheduler jitter. */
        DelayThread(16000);
    }
}
