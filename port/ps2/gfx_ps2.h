#ifndef PD_PS2_GFX_PS2_H
#define PD_PS2_GFX_PS2_H

#include <gsKit.h>

#include "gfx_rendering_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Binds the current GS instance to the Perfect Dark rendering API backend.
 * The full port will move this ownership into the PS2 window/platform layer;
 * the bring-up target keeps it explicit so graphics state has one owner.
 */
void gfxPs2BindGs(GSGLOBAL *gs);

extern struct GfxRenderingAPI gfx_ps2_api;

#ifdef __cplusplus
}
#endif

#endif
