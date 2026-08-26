#include <stdint.h>

#include <PR/gbi.h>

#include "gfx_api.h"
#include "gfx_window_ps2.h"
#include "gs_core.h"

/*
 * The PS2 CMake target renames the real Fast3D gfx_start_frame symbol to this
 * implementation name. This tiny wrapper is the platform seam between a
 * fixed-raster CRT device and Fast3D's desktop-style square-pixel WAPI model.
 */
void gfx_start_frame_ps2_impl(void);

void gfx_start_frame(void)
{
    /*
     * Fast3D computes rsp.aspect_scale/aspect_ofs near the end of start_frame.
     * Let that calculation see a square-pixel logical 4:3 or 16:9 surface.
     */
    gfxPs2WindowBeginLogicalDimensions();
    gfx_start_frame_ps2_impl();
    gfxPs2WindowEndLogicalDimensions();

    if (!ps2GsCoreIsReady()) {
        return;
    }

    /*
     * Restore final-consumer coordinates before any display-list work begins.
     * Keep only the logical aspect calculated above. RATIO_X/RATIO_Y, viewport,
     * scissor and packet generation therefore continue to use the real GS
     * framebuffer dimensions and do not increase raster work or VRAM usage.
     */
    const uint32_t width = (uint32_t)ps2GsCoreGetWidth();
    const uint32_t height = (uint32_t)ps2GsCoreGetHeight();
    const float aspect = gfxPs2WindowGetDisplayAspect();

    gfx_current_window_dimensions.width = width;
    gfx_current_window_dimensions.height = height;
    gfx_current_window_dimensions.aspect_ratio = aspect;

    gfx_current_dimensions.width = width;
    gfx_current_dimensions.height = height;
    gfx_current_dimensions.aspect_ratio = aspect;

    gfx_current_game_window_viewport.width = width;
    gfx_current_game_window_viewport.height = height;
}
