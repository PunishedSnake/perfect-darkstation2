#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <PR/ultratypes.h>
#include <PR/gbi.h>

#include "config.h"
#include "system.h"
#include "video.h"

#include "../fast3d/gfx_api.h"
#include "gfx_ps2.h"
#include "gfx_window_ps2.h"
#include "gs_core.h"

/*
 * Perfect Dark video API for PlayStation 2.
 *
 * This is the console implementation of port/include/video.h. The desktop
 * implementation in port/src/video.c owns SDL/OpenGL policy and is therefore
 * intentionally not pulled into the EE build.
 *
 * CURRENT IMPLEMENTATION:
 *   - gs_core owns CRT/VRAM/GS lifetime.
 *   - gfx_window_ps2 owns presentation/VBlank timing.
 *   - gfx_ps2 translates the shared Fast3D VBO contract into packet-ready GS
 *     vertices.
 *   - offscreen framebuffer effects, MSAA, mipmaps and anisotropy remain
 *     disabled until the corresponding GS contracts are implemented.
 *   - logical 4:3/16:9 aspect is independent from GS framebuffer dimensions;
 *     changing aspect therefore changes projection/layout, not raster workload.
 *
 * The public configuration names which also exist on desktop are preserved so
 * game/menu code does not need a second settings system merely because the
 * output device happens to be a console rather than a movable rectangle.
 */

static bool s_init_done;
static s32 s_framerate_limit;
static s32 s_display_fps;
static s32 s_aspect_mode = VIDEO_ASPECT_4_3;
static f32 s_display_fps_interval = 1.0f;
static f32 s_average_fps;
static u32 s_texture_filter = FILTER_LINEAR;
static s32 s_texture_filter_2d = true;
static f32 s_glare_brightness = 1.0f;
static f32 s_overexposure_scale = 1.0f;

static displaymode s_display_modes[2];
static s32 s_display_mode_count = 1;

static f64 s_last_frame_time;
static f64 s_fps_deadline;
static f64 s_fps_accum;
static s32 s_fps_frames;

void optionsMenuInit(void);

static f32 ps2VideoAspectValue(void)
{
    return s_aspect_mode == VIDEO_ASPECT_16_9 ? (16.0f / 9.0f) : (4.0f / 3.0f);
}

static void ps2VideoApplyAspect(void)
{
    gfxPs2WindowSetDisplayAspect(ps2VideoAspectValue());
}

static void ps2VideoRefreshModeList(void)
{
    s_display_modes[0].width = 0;
    s_display_modes[0].height = 0;
    s_display_mode_count = 1;

    if (!ps2GsCoreIsReady()) {
        return;
    }

    s_display_modes[1].width = ps2GsCoreGetWidth();
    s_display_modes[1].height = ps2GsCoreGetHeight();
    s_display_mode_count = 2;
}

s32 videoInit(void)
{
    if (!ps2GsCoreIsReady()) {
        const struct Ps2GsCreateInfo gs_info = {
            PS2_GS_PSM_CT16,
            PS2_GS_PSMZ_16,
            true,
            true,
        };

        if (!ps2GsCoreInit(&gs_info)) {
            sysLogPrintf(LOG_ERROR, "VideoPS2: GS core initialisation failed");
            return -1;
        }
    }

    const s32 width = ps2GsCoreGetWidth();
    const s32 height = ps2GsCoreGetHeight();
    if (width <= 0 || height <= 0) {
        sysLogPrintf(LOG_ERROR, "VideoPS2: invalid active GS dimensions %dx%d", width, height);
        return -1;
    }

    gfx_current_native_viewport.width = 320;
    gfx_current_native_viewport.height = 220;
    gfx_current_native_aspect = 320.0f / 220.0f;

    /*
     * Set physical display aspect before gfx_init/start_frame. The WAPI frame
     * bridge exposes it to Fast3D without changing the actual GS raster size.
     */
    ps2VideoApplyAspect();

    /* Do not advertise effects that the active GS backend intentionally rejects. */
    gfx_framebuffers_enabled = false;
    gfx_detail_textures_enabled = false;
    gfx_msaa_level = 1;

    struct GfxInitSettings settings = {
        .wapi = &gfx_window_ps2_api,
        .rapi = &gfx_ps2_api,
        .window_settings = {
            .title = "Perfect DarkStation 2",
            .width = (uint32_t)width,
            .height = (uint32_t)height,
            .x = 0,
            .y = 0,
            .fullscreen = true,
            .fullscreen_is_exclusive = true,
            .maximized = true,
            .centered = true,
            .allow_hidpi = false,
        },
    };

    gfx_init(&settings);
    gfx_set_texture_filter((enum FilteringMode)s_texture_filter);
    gfx_set_mipmap_filter(MIPMAP_DISABLED);
    gfx_ps2_api.set_anisotropy_level(1);

    ps2VideoRefreshModeList();

    /* PS2 presentation currently owns one VBlank wait per displayed frame. */
    gfx_window_ps2_api.set_swap_interval(1);
    gfx_window_ps2_api.set_target_fps(s_framerate_limit);

    s_last_frame_time = gfx_window_ps2_api.get_time();
    s_fps_deadline = s_last_frame_time + s_display_fps_interval;
    s_fps_accum = 0.0;
    s_fps_frames = 0;
    s_average_fps = 0.0f;

    optionsMenuInit();
    s_init_done = true;

    sysLogPrintf(LOG_NOTE,
        "VideoPS2: Fast3D->GS game backend active %dx%d refresh=%d aspect=%s(%.4f) framebuffers=0 msaa=0 mipmaps=0 aniso=1",
        width,
        height,
        ps2GsCoreGetRefreshRate(),
        s_aspect_mode == VIDEO_ASPECT_16_9 ? "16:9" : "4:3",
        ps2VideoAspectValue());
    return 0;
}

void videoStartFrame(void)
{
    if (s_init_done) {
        gfx_start_frame();
    }
}

void videoSubmitCommands(Gfx *cmds)
{
    if (s_init_done && cmds) {
        gfx_run(cmds);
    }
}

void videoEndFrame(void)
{
    if (!s_init_done) {
        return;
    }

    gfx_end_frame();

    const f64 now = gfx_window_ps2_api.get_time();
    const f64 delta = now - s_last_frame_time;
    s_last_frame_time = now;

    if (delta >= 0.0) {
        s_fps_accum += delta;
        ++s_fps_frames;
    }

    if (now >= s_fps_deadline) {
        s_average_fps = (s_fps_accum > 0.0 && s_fps_frames > 0)
            ? (f32)((f64)s_fps_frames / s_fps_accum)
            : 0.0f;
        s_fps_accum = 0.0;
        s_fps_frames = 0;
        s_fps_deadline = now + s_display_fps_interval;
    }
}

void videoClearScreen(void)
{
    if (!s_init_done) {
        return;
    }

    gfx_start_frame();
    gfx_ps2_api.clear_framebuffer(true, true);
    gfx_end_frame();
}

void *videoGetWindowHandle(void)
{
    return NULL;
}

void videoUpdateNativeResolution(s32 w, s32 h)
{
    if (w <= 0 || h <= 0) {
        return;
    }

    gfx_current_native_viewport.width = (uint32_t)w;
    gfx_current_native_viewport.height = (uint32_t)h;
    gfx_current_native_aspect = (f32)w / (f32)h;
}

s32 videoGetNativeWidth(void)
{
    return (s32)gfx_current_native_viewport.width;
}

s32 videoGetNativeHeight(void)
{
    return (s32)gfx_current_native_viewport.height;
}

s32 videoGetWidth(void)
{
    return (s32)gfx_current_dimensions.width;
}

s32 videoGetHeight(void)
{
    return (s32)gfx_current_dimensions.height;
}

f32 videoGetAspect(void)
{
    return ps2VideoAspectValue();
}

s32 videoGetAspectMode(void)
{
    return s_aspect_mode;
}

s32 videoGetFullscreen(void)
{
    return true;
}

s32 videoGetFullscreenMode(void)
{
    return true;
}

s32 videoGetMaximizeWindow(void)
{
    return true;
}

void videoSetMaximizeWindow(s32 fs)
{
    (void)fs;
}

s32 videoGetCenterWindow(void)
{
    return true;
}

void videoSetCenterWindow(s32 center)
{
    (void)center;
}

u32 videoGetTextureFilter(void)
{
    return s_texture_filter;
}

s32 videoGetTextureFilter2D(void)
{
    return s_texture_filter_2d;
}

u32 videoGetAnisotropicFilter(void)
{
    return 1;
}

u32 videoGetMaxAnisotropyLevel(void)
{
    return 1;
}

s32 videoGetDetailTextures(void)
{
    return false;
}

s32 videoGetDisplayModeIndex(void)
{
    return s_display_mode_count > 1 ? 1 : 0;
}

s32 videoGetDisplayMode(displaymode *out, const s32 index)
{
    if (!out || index < 0 || index >= s_display_mode_count) {
        return false;
    }

    *out = s_display_modes[index];
    return true;
}

s32 videoGetNumDisplayModes(void)
{
    return s_display_mode_count;
}

s32 videoGetVsync(void)
{
    return 1;
}

s32 videoGetFramerateLimit(void)
{
    return s_framerate_limit;
}

s32 videoGetDisplayFPS(void)
{
    return s_display_fps;
}

s32 videoGetMSAA(void)
{
    return 1;
}

f32 videoGetGlareBrightness(void)
{
    return s_glare_brightness;
}

f32 videoGetOverexposureScale(void)
{
    return s_overexposure_scale;
}

f32 videoGetAverageFPS(void)
{
    return s_average_fps;
}

void videoSetWindowOffset(s32 x, s32 y)
{
    gfx_current_game_window_viewport.x = (s16)x;
    gfx_current_game_window_viewport.y = (s16)y;
}

void videoSetFullscreen(s32 fs)
{
    (void)fs;
}

void videoSetFullscreenMode(s32 mode)
{
    (void)mode;
}

void videoSetAspectMode(s32 mode)
{
    if (mode != VIDEO_ASPECT_4_3 && mode != VIDEO_ASPECT_16_9) {
        mode = VIDEO_ASPECT_4_3;
    }

    if (s_aspect_mode == mode) {
        return;
    }

    s_aspect_mode = mode;
    ps2VideoApplyAspect();

    if (s_init_done) {
        sysLogPrintf(LOG_NOTE,
            "VideoPS2: display aspect changed to %s (%.4f), GS raster unchanged %dx%d",
            s_aspect_mode == VIDEO_ASPECT_16_9 ? "16:9" : "4:3",
            ps2VideoAspectValue(),
            ps2GsCoreGetWidth(),
            ps2GsCoreGetHeight());
    }
}

void videoSetTextureFilter(u32 filter)
{
    if (filter > FILTER_THREE_POINT) {
        filter = FILTER_THREE_POINT;
    }

    s_texture_filter = filter;
    if (s_init_done) {
        gfx_set_texture_filter((enum FilteringMode)filter);
    }
}

void videoSetTextureFilter2D(s32 filter)
{
    s_texture_filter_2d = !!filter;
}

void videoSetAnisotropicFilter(u32 filter)
{
    (void)filter;
    if (s_init_done) {
        gfx_ps2_api.set_anisotropy_level(1);
    }
}

void videoSetDetailTextures(s32 detail)
{
    (void)detail;
    gfx_detail_textures_enabled = false;
}

void videoSetDisplayMode(const s32 index)
{
    /*
     * CURRENT IMPLEMENTATION: only the already-active GS mode is exposed.
     * Runtime CRT/VRAM reconfiguration is deliberately not faked here; a mode
     * selector is added once gs_core owns a transactional mode switch.
     */
    (void)index;
}

void videoSetVsync(const s32 vsync)
{
    (void)vsync;
    /* Presentation is synchronized to VBlank on the current PS2 backend. */
}

void videoSetFramerateLimit(const s32 limit)
{
    s_framerate_limit = limit < 0 ? 0 : (limit > VIDEO_MAX_FPS ? VIDEO_MAX_FPS : limit);

    /*
     * The window backend records the target now, but does not yet insert an
     * extra pre-present wait. Keeping the value here lets the existing menu and
     * config contract survive while pacing is implemented against real-hardware
     * VBlank measurements rather than a desktop sleep assumption.
     */
    if (s_init_done) {
        gfx_window_ps2_api.set_target_fps(s_framerate_limit);
    }
}

void videoSetDisplayFPS(const s32 displayfps)
{
    s_display_fps = !!displayfps;
}

void videoSetMSAA(const s32 msaa)
{
    (void)msaa;
    gfx_msaa_level = 1;
}

void videoSetGlareBrightness(f32 bright)
{
    s_glare_brightness = bright < 0.0f ? 0.0f : (bright > 1.0f ? 1.0f : bright);
}

void videoSetOverexposureScale(f32 scale)
{
    s_overexposure_scale = scale < 0.0f ? 0.0f : (scale > 1.0f ? 1.0f : scale);
}

s32 videoCreateFramebuffer(u32 w, u32 h, s32 upscale, s32 autoresize)
{
    (void)w;
    (void)h;
    (void)upscale;
    (void)autoresize;
    return 0;
}

void videoSetFramebuffer(s32 target)
{
    (void)target;
}

void videoResetFramebuffer(void)
{
}

void videoCopyFramebuffer(s32 dst, s32 src, s32 left, s32 top)
{
    (void)dst;
    (void)src;
    (void)left;
    (void)top;
}

void videoResizeFramebuffer(s32 target, u32 w, u32 h, s32 upscale, s32 autoresize)
{
    (void)target;
    (void)w;
    (void)h;
    (void)upscale;
    (void)autoresize;
}

s32 videoFramebuffersSupported(void)
{
    return false;
}

void videoResetTextureCache(void)
{
    gfx_texture_cache_clear();
}

void videoFreeCachedTexture(const void *texptr)
{
    gfx_texture_cache_delete((const uint8_t *)texptr);
}

void videoFreeCachedTextures(const void *start, const void *end)
{
    gfx_texture_cache_delete_range((const uint8_t *)start, (const uint8_t *)end);
}

void videoShutdown(void)
{
    if (s_init_done) {
        gfx_destroy();
        s_init_done = false;
    }
}

PD_CONSTRUCTOR static void videoPs2ConfigInit(void)
{
    configRegisterInt("Video.AspectRatio", &s_aspect_mode,
        VIDEO_ASPECT_4_3, VIDEO_ASPECT_16_9);
    configRegisterInt("Video.FramerateLimit", &s_framerate_limit, 0, VIDEO_MAX_FPS);
    configRegisterInt("Video.DisplayFPS", &s_display_fps, 0, 1);
    configRegisterFloat("Video.DisplayFPSInterval", &s_display_fps_interval, 0.01f, 32.0f);
    configRegisterInt("Video.TextureFilter", (s32 *)&s_texture_filter, 0, FILTER_THREE_POINT);
    configRegisterInt("Video.TextureFilter2D", &s_texture_filter_2d, 0, 1);
    configRegisterFloat("Video.GlareBrightness", &s_glare_brightness, 0.0f, 1.0f);
    configRegisterFloat("Video.OverexposureScale", &s_overexposure_scale, 0.0f, 1.0f);
}
