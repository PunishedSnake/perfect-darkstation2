#include <stdbool.h>
#include <stdint.h>

#include <gsKit.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "gfx_api.h"
#include "gfx_ps2.h"
#include "gfx_window_ps2.h"
#include "log_ps2.h"
#include "system.h"

#define FAST3D_NATIVE_WIDTH 320u
#define FAST3D_NATIVE_HEIGHT 240u
#define FAST3D_TEST_DL_CAPACITY 16u

/*
 * gfx_pc's fill-rectangle compatibility path tracks the logical N64 color and
 * depth image addresses. It never dereferences the color address for the PS2
 * default framebuffer, but the two addresses must differ or an N64 Z-clear is
 * intentionally discarded. Keep a real object here instead of inventing a
 * magic address.
 */
static uint32_t sFast3dColorImageTag;
static Gfx sFast3dTestDl[FAST3D_TEST_DL_CAPACITY];

static uint16_t fast3dStatusColor(int romStatus)
{
    if (romStatus > 0) {
        return GPACK_RGBA5551(0x28, 0xe0, 0x68, 1);
    }
    if (romStatus < 0) {
        return GPACK_RGBA5551(0xf0, 0x38, 0x38, 1);
    }
    return GPACK_RGBA5551(0xf0, 0xb0, 0x30, 1);
}

static bool buildFast3dTestDl(int romStatus)
{
    Gfx *cmd = sFast3dTestDl;
    const Gfx *const end = sFast3dTestDl + FAST3D_TEST_DL_CAPACITY;

#define FAST3D_EMIT(expr)        \
    do {                         \
        if (cmd >= end) {        \
            return false;        \
        }                        \
        expr;                    \
        ++cmd;                   \
    } while (0)

    /* Establish a non-Z logical color target for the translator. */
    FAST3D_EMIT(gDPSetColorImage(cmd, G_IM_FMT_RGBA, G_IM_SIZ_16b,
        FAST3D_NATIVE_WIDTH, &sFast3dColorImageTag));

    FAST3D_EMIT(gDPSetCycleType(cmd, G_CYC_FILL));

    /* Dark frame establishes that the display list itself owns the image. */
    FAST3D_EMIT(gDPSetFillColor(cmd, GPACK_RGBA5551(0x18, 0x18, 0x20, 1)));
    FAST3D_EMIT(gDPFillRectangle(cmd, 0, 0,
        FAST3D_NATIVE_WIDTH - 1, FAST3D_NATIVE_HEIGHT - 1));

    /* RGB bars prove repeated GBI state changes and rectangle translation. */
    FAST3D_EMIT(gDPSetFillColor(cmd, GPACK_RGBA5551(0xe0, 0x38, 0x38, 1)));
    FAST3D_EMIT(gDPFillRectangle(cmd, 24, 42, 104, 104));

    FAST3D_EMIT(gDPSetFillColor(cmd, GPACK_RGBA5551(0x38, 0xd8, 0x58, 1)));
    FAST3D_EMIT(gDPFillRectangle(cmd, 120, 42, 200, 104));

    FAST3D_EMIT(gDPSetFillColor(cmd, GPACK_RGBA5551(0x48, 0x70, 0xe8, 1)));
    FAST3D_EMIT(gDPFillRectangle(cmd, 216, 42, 296, 104));

    /* ROM/data-path status remains visible after moving behind Fast3D. */
    FAST3D_EMIT(gDPSetFillColor(cmd, fast3dStatusColor(romStatus)));
    FAST3D_EMIT(gDPFillRectangle(cmd, 24, 178, 296, 194));

    /* Small white marker makes obvious whether the final command executed. */
    FAST3D_EMIT(gDPSetFillColor(cmd, GPACK_RGBA5551(0xf8, 0xf8, 0xf8, 1)));
    FAST3D_EMIT(gDPFillRectangle(cmd, 152, 126, 168, 142));

    FAST3D_EMIT(gSPEndDisplayList(cmd));

#undef FAST3D_EMIT

    return true;
}

bool ps2Fast3dSceneRun(GSGLOBAL *gs, int romStatus)
{
    if (!gs) {
        return false;
    }

    if (!buildFast3dTestDl(romStatus)) {
        sysLogPrintf(LOG_ERROR, "Fast3D scene: display-list builder overflow");
        ps2LogCheckpoint();
        return false;
    }

    /*
     * Perfect Dark's GBI uses the original 320x240 coordinate space. The
     * translator scales that native viewport to the fixed PS2 scanout size.
     */
    gfx_current_native_viewport.x = 0;
    gfx_current_native_viewport.y = 0;
    gfx_current_native_viewport.width = FAST3D_NATIVE_WIDTH;
    gfx_current_native_viewport.height = FAST3D_NATIVE_HEIGHT;
    gfx_current_native_aspect = 4.0f / 3.0f;
    gfx_msaa_level = 1;

    gfxPs2BindGs(gs);
    gfxPs2WindowBindGs(gs);

    const struct GfxInitSettings settings = {
        .wapi = &gfx_window_ps2_api,
        .rapi = &gfx_ps2_api,
        .window_settings = {
            .title = "Perfect DarkStation 2 Fast3D bring-up",
            .width = (uint32_t)gs->Width,
            .height = (uint32_t)gs->Height,
            .x = 0,
            .y = 0,
            .fullscreen = true,
            .fullscreen_is_exclusive = true,
            .maximized = true,
            .centered = false,
            .allow_hidpi = false,
        },
    };

    sysLogPrintf(LOG_NOTE,
        "Fast3D scene: init native=%ux%u scanout=%dx%d rom_status=%d",
        FAST3D_NATIVE_WIDTH, FAST3D_NATIVE_HEIGHT, gs->Width, gs->Height, romStatus);
    sysLogPrintf(LOG_NOTE,
        "Fast3D scene: GBI fill rectangles -> gfx_pc -> GfxRenderingAPI -> GS");
    ps2LogCheckpoint();

    /*
     * CURRENT IMPLEMENTATION NOTE:
     * gfx_init() still reserves a desktop-style texture-conversion scratch
     * buffer sized from get_max_texture_size(). The current PS2 backend reports
     * 1024, so this correctness milestone may reserve 4 MiB even though this
     * untextured display list never consumes it. That waste is deliberately
     * isolated as the next memory-policy patch rather than hidden here.
     */
    gfx_init(&settings);

    sysLogPrintf(LOG_NOTE, "Fast3D scene: gfx_init complete; entering display-list loop");
    ps2LogCheckpoint();

    for (;;) {
        gfx_start_frame();
        gfx_run(sFast3dTestDl);
        gfx_end_frame();
    }
}
