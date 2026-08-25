#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <gsKit.h>

#include "gfx_window_ps2.h"
#include "system.h"

/*
 * Console window manager baseline.
 *
 * A PS2 does not have movable desktop windows, HIDPI or an independent swap
 * interval. Keep those operations explicit no-ops instead of emulating a PC
 * abstraction. The meaningful responsibilities here are display dimensions,
 * frame admission, timing and presentation ownership.
 *
 * Whole-system rule: swap_buffers_begin() never waits. Presentation waits only
 * in swap_buffers_end(), after Fast3D has submitted the frame and called its
 * rendering backend finish hook.
 */

static GSGLOBAL *s_window_gs;
static int s_target_fps = 60;
static void (*s_fullscreen_changed_cb)(bool);

void gfxPs2WindowBindGs(GSGLOBAL *gs)
{
    s_window_gs = gs;
}

static int ps2_window_refresh_rate(void)
{
    if (!s_window_gs) {
        return s_target_fps > 0 ? s_target_fps : 60;
    }

    switch (s_window_gs->Mode) {
        case GS_MODE_PAL:
        case GS_MODE_DTV_576P:
        case GS_MODE_DVD_PAL:
            return 50;
        case GS_MODE_VGA_640_72:
        case GS_MODE_VGA_800_72:
            return 72;
        case GS_MODE_VGA_640_75:
        case GS_MODE_VGA_800_75:
        case GS_MODE_VGA_1024_75:
        case GS_MODE_VGA_1280_75:
            return 75;
        case GS_MODE_VGA_640_85:
        case GS_MODE_VGA_800_85:
        case GS_MODE_VGA_1024_85:
            return 85;
        default:
            return 60;
    }
}

static void ps2_window_init(const struct GfxWindowInitSettings *settings)
{
    if (!s_window_gs) {
        sysLogPrintf(LOG_ERROR, "GfxWapiPS2 init: GS is not bound");
        return;
    }

    if (settings &&
        (settings->width != (uint32_t)s_window_gs->Width ||
         settings->height != (uint32_t)s_window_gs->Height)) {
        sysLogPrintf(LOG_WARNING,
            "GfxWapiPS2 init: requested %ux%u but active GS is %dx%d; keeping active display",
            settings->width, settings->height, s_window_gs->Width, s_window_gs->Height);
    }

    s_target_fps = ps2_window_refresh_rate();
    sysLogPrintf(LOG_NOTE,
        "GfxWapiPS2 init: fixed display %dx%d mode=0x%x refresh=%d",
        s_window_gs->Width, s_window_gs->Height, s_window_gs->Mode, s_target_fps);
}

static void ps2_window_close(void)
{
    /* GS lifetime is owned by the PS2 platform layer during bring-up. */
}

static int ps2_get_display_mode(int modenum, int *out_w, int *out_h)
{
    if (modenum != 0 || !s_window_gs) {
        return 0;
    }
    if (out_w) *out_w = s_window_gs->Width;
    if (out_h) *out_h = s_window_gs->Height;
    return 1;
}

static int ps2_get_current_display_mode(int *out_w, int *out_h)
{
    return ps2_get_display_mode(0, out_w, out_h);
}

static int ps2_get_num_display_modes(void)
{
    return s_window_gs ? 1 : 0;
}

static int32_t ps2_get_fullscreen_state(void)
{
    return 1;
}

static void ps2_set_fullscreen_changed_callback(void (*cb)(bool))
{
    s_fullscreen_changed_cb = cb;
}

static void ps2_set_fullscreen(bool enable)
{
    if (enable && s_fullscreen_changed_cb) {
        s_fullscreen_changed_cb(true);
    }
}

static void ps2_set_fullscreen_exclusive(bool exc)
{
    (void)exc;
}

static void ps2_set_fullscreen_flag(int32_t mode)
{
    (void)mode;
}

static int32_t ps2_get_fullscreen_flag_mode(void)
{
    return 1;
}

static int32_t ps2_get_maximized_state(void)
{
    return 1;
}

static void ps2_set_maximize(bool enable)
{
    (void)enable;
}

static void ps2_get_active_window_refresh_rate(uint32_t *refresh_rate)
{
    if (refresh_rate) {
        *refresh_rate = (uint32_t)ps2_window_refresh_rate();
    }
}

static void ps2_set_cursor_visibility(bool visible)
{
    (void)visible;
}

static void ps2_set_closest_resolution(int32_t width, int32_t height, bool should_center)
{
    (void)width;
    (void)height;
    (void)should_center;
}

static void ps2_set_dimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY)
{
    (void)width;
    (void)height;
    (void)posX;
    (void)posY;
}

static void ps2_get_dimensions(uint32_t *width, uint32_t *height, int32_t *posX, int32_t *posY)
{
    if (width) *width = s_window_gs ? (uint32_t)s_window_gs->Width : 0;
    if (height) *height = s_window_gs ? (uint32_t)s_window_gs->Height : 0;
    if (posX) *posX = 0;
    if (posY) *posY = 0;
}

static void ps2_get_centered_positions(int32_t width, int32_t height, int32_t *posX, int32_t *posY)
{
    (void)width;
    (void)height;
    if (posX) *posX = 0;
    if (posY) *posY = 0;
}

static void ps2_handle_events(void)
{
    /* Controller input is a separate platform/input backend. */
}

static bool ps2_start_frame(void)
{
    return s_window_gs != NULL;
}

static void ps2_swap_buffers_begin(void)
{
    /* Submit early, wait late: deliberately no synchronization here. */
}

static void ps2_swap_buffers_end(void)
{
    if (s_window_gs) {
        gsKit_sync_flip(s_window_gs);
    }
}

static double ps2_get_time(void)
{
    return (double)sysGetMicroseconds() / 1000000.0;
}

static int32_t ps2_get_target_fps(void)
{
    return s_target_fps;
}

static void ps2_set_target_fps(int fps)
{
    if (fps > 0) {
        s_target_fps = fps;
    }
}

static bool ps2_can_disable_vsync(void)
{
    return false;
}

static void *ps2_get_window_handle(void)
{
    return NULL;
}

static void ps2_set_window_title(const char *title)
{
    (void)title;
}

static int ps2_get_swap_interval(void)
{
    return 1;
}

static bool ps2_set_swap_interval(int interval)
{
    return interval == 1;
}

struct GfxWindowManagerAPI gfx_window_ps2_api = {
    ps2_window_init,
    ps2_window_close,
    ps2_get_display_mode,
    ps2_get_current_display_mode,
    ps2_get_num_display_modes,
    ps2_get_fullscreen_state,
    ps2_set_fullscreen_changed_callback,
    ps2_set_fullscreen,
    ps2_set_fullscreen_exclusive,
    ps2_set_fullscreen_flag,
    ps2_get_fullscreen_flag_mode,
    ps2_get_maximized_state,
    ps2_set_maximize,
    ps2_get_active_window_refresh_rate,
    ps2_set_cursor_visibility,
    ps2_set_closest_resolution,
    ps2_set_dimensions,
    ps2_get_dimensions,
    ps2_get_centered_positions,
    ps2_handle_events,
    ps2_start_frame,
    ps2_swap_buffers_begin,
    ps2_swap_buffers_end,
    ps2_get_time,
    ps2_get_target_fps,
    ps2_set_target_fps,
    ps2_can_disable_vsync,
    ps2_get_window_handle,
    ps2_set_window_title,
    ps2_get_swap_interval,
    ps2_set_swap_interval,
};
