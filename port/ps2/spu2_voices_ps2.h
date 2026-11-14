#ifndef PD_PS2_SPU2_VOICES_PS2_H
#define PD_PS2_SPU2_VOICES_PS2_H

#include <PR/ultratypes.h>
#include <stdbool.h>

#include "pdsnd_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True once pdsnd.irx runs with a valid bank: synth voices go to SPU2. */
extern bool g_Ps2HwVoices;

void ps2HwVoicesInit(s32 output_rate);
/* Bank entry for a wavetable (ALWaveTable base pointer and length). */
s32 ps2HwVoicesEntryFor(const u8 *base, s32 len);
u16 ps2HwVoicesPitch(f32 ratio);
void ps2HwVoicesPush(const PdSndVoiceCmd *cmd);
/* Send this audio frame's commands (non-blocking). */
void ps2HwVoicesFlush(void);

#ifdef __cplusplus
}
#endif

#endif
