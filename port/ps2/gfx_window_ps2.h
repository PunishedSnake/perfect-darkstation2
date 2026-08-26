#ifndef PD_PS2_GFX_WINDOW_PS2_H
#define PD_PS2_GFX_WINDOW_PS2_H

#include <stdbool.h>

#include "gfx_window_manager_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-display PS2 window backend consumes dimensions/timing from gs_core. */
extern struct GfxWindowManagerAPI gfx_window_ps2_api;

/*
 * PS2 framebuffer dimensions are raster dimensions, not a reliable physical
 * display aspect. The Fast3D compatibility frontend needs a square-pixel
 * logical aspect while GS/PCRTC continue using the real framebuffer size.
 */
void gfxPs2WindowSetDisplayAspect(float aspect);
float gfxPs2WindowGetDisplayAspect(void);

/*
 * Used only by the PS2 Fast3D frame bridge while gfx_start_frame() computes
 * aspect-dependent state. Normal WAPI queries continue to expose physical GS
 * dimensions.
 */
void gfxPs2WindowBeginLogicalDimensions(void);
void gfxPs2WindowEndLogicalDimensions(void);

#ifdef __cplusplus
}
#endif

#endif
