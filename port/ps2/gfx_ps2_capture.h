#ifndef PD_PS2_GFX_PS2_CAPTURE_H
#define PD_PS2_GFX_PS2_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Arm a one-frame renderer/GS capture after the requested warm-up frames. */
void gfxPs2RequestRendererCapture(uint32_t stage, uint32_t warmup_frames);

#ifdef __cplusplus
}
#endif

#endif
