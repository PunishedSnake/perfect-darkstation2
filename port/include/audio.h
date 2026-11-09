#ifndef _IN_AUDIO_H
#define _IN_AUDIO_H

#include <PR/ultratypes.h>

s32 audioInit(void);
s32 audioGetBytesBuffered(void);
s32 audioGetSamplesBuffered(void);
void audioSetNextBuffer(const s16 *buf, u32 len);
void audioEndFrame(void);
#ifdef PLATFORM_PS2
// how many of the requested 60 Hz audio ticks to synthesize this game frame
s32 audioFrameBudget(s32 requested);
#endif

#endif
