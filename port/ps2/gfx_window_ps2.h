#ifndef PD_PS2_GFX_WINDOW_PS2_H
#define PD_PS2_GFX_WINDOW_PS2_H

#include "gfx_window_manager_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-display PS2 window backend consumes dimensions/timing from gs_core. */
extern struct GfxWindowManagerAPI gfx_window_ps2_api;

#ifdef __cplusplus
}
#endif

#endif
