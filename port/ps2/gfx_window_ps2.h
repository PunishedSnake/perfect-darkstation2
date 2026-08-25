#ifndef PD_PS2_GFX_WINDOW_PS2_H
#define PD_PS2_GFX_WINDOW_PS2_H

#include <gsKit.h>

#include "gfx_window_manager_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bind the already-initialised GS instance owned by the PS2 platform layer.
 * The bring-up harness owns GS creation for now; this makes the ownership
 * boundary explicit while Fast3D adopts the normal GfxWindowManagerAPI path.
 */
void gfxPs2WindowBindGs(GSGLOBAL *gs);

extern struct GfxWindowManagerAPI gfx_window_ps2_api;

#ifdef __cplusplus
}
#endif

#endif
