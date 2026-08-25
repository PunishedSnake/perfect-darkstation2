#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <dmaKit.h>
#include <gsKit.h>

#include "gs_core.h"
#include "gs_native_queue.h"
#include "log_ps2.h"
#include "system.h"

#define PS2_GS_MAX_TEXTURES 64
#define PS2_GS_NATIVE_QUEUE_QW 16384u

/* GS TEST.ATST encodings, cross-checked against current PS2SDK libgs. */
#define PS2_GS_ATST_ALWAYS  1u
#define PS2_GS_ATST_GEQUAL  5u
#define PS2_GS_AFAIL_KEEP   0u

struct Ps2GsTextureSlot {
    bool used;
    bool uploaded;
    GSTEXTURE texture;
};

static GSGLOBAL *s_gs;
static struct Ps2GsTextureSlot s_textures[PS2_GS_MAX_TEXTURES];
static bool s_frame_building;
static bool s_depth_update = true;
static bool s_texture_alpha;
static bool s_native_submit_failed;
static uint8_t s_fog_r;
static uint8_t s_fog_g;
static uint8_t s_fog_b;

static_assert(sizeof(Ps2GsColorVertex) == sizeof(GSPRIMPOINT),
    "packet-ready color vertex must match current gsKit A+D source layout");
static_assert(sizeof(Ps2GsTexturedVertex) == sizeof(GSPRIMSTQPOINT),
    "packet-ready textured vertex must match current gsKit A+D source layout");

static struct Ps2GsTextureSlot *ps2GsCoreTextureSlot(Ps2GsTextureHandle handle)
{
    if (handle == PS2_GS_TEXTURE_INVALID || handle > PS2_GS_MAX_TEXTURES) {
        return NULL;
    }

    struct Ps2GsTextureSlot *slot = &s_textures[handle - 1];
    return slot->used ? slot : NULL;
}

static struct Ps2GsPackedReg *ps2GsCoreReserve(uint32_t register_count)
{
    if (!s_frame_building) {
        return NULL;
    }
    return ps2GsNativeQueueReserveAd(register_count);
}

static void ps2GsCoreWriteReg(struct Ps2GsPackedReg *dst, uint64_t value, uint64_t reg)
{
    dst->value = value;
    dst->reg = reg;
}

static uint64_t ps2GsCoreCurrentTestValue(int ztst_override)
{
    const int ztst = ztst_override >= 0 ? ztst_override : s_gs->Test->ZTST;
    return GS_SETREG_TEST(
        s_gs->Test->ATE,
        s_gs->Test->ATST,
        s_gs->Test->AREF,
        s_gs->Test->AFAIL,
        s_gs->Test->DATE,
        s_gs->Test->DATM,
        s_gs->Test->ZTE,
        ztst);
}

static void ps2GsCoreEmitTest(void)
{
    struct Ps2GsPackedReg *p = ps2GsCoreReserve(1);
    if (p) {
        ps2GsCoreWriteReg(p,
            ps2GsCoreCurrentTestValue(-1),
            GS_TEST_1 + s_gs->PrimContext);
    }
}

static void ps2GsCoreEmitZbufWriteMask(void)
{
    if (!s_gs->ZBuffering) {
        return;
    }

    struct Ps2GsPackedReg *p = ps2GsCoreReserve(1);
    if (p) {
        ps2GsCoreWriteReg(p,
            GS_SETREG_ZBUF(s_gs->ZBuffer / 8192, s_gs->PSMZ, s_depth_update ? 0 : 1),
            GS_ZBUF_1 + s_gs->PrimContext);
    }
}

static void ps2GsCoreEmitAlpha(void)
{
    if (!s_gs->PrimAlphaEnable) {
        return;
    }

    struct Ps2GsPackedReg *p = ps2GsCoreReserve(2);
    if (!p) {
        return;
    }

    ps2GsCoreWriteReg(&p[0], s_gs->PABE, GS_PABE);
    ps2GsCoreWriteReg(&p[1], s_gs->PrimAlpha, GS_ALPHA_1 + s_gs->PrimContext);
}

static void ps2GsCoreEmitFogColor(void)
{
    if (!s_gs->PrimFogEnable) {
        return;
    }

    struct Ps2GsPackedReg *p = ps2GsCoreReserve(1);
    if (p) {
        ps2GsCoreWriteReg(p,
            (uint64_t)s_fog_r | ((uint64_t)s_fog_g << 8) | ((uint64_t)s_fog_b << 16),
            GS_FOGCOL);
    }
}

static void ps2GsCoreEmitClamp(void)
{
    struct Ps2GsPackedReg *p = ps2GsCoreReserve(1);
    if (p) {
        ps2GsCoreWriteReg(p,
            GS_SETREG_CLAMP(
                s_gs->Clamp->WMS,
                s_gs->Clamp->WMT,
                s_gs->Clamp->MINU,
                s_gs->Clamp->MAXU,
                s_gs->Clamp->MINV,
                s_gs->Clamp->MAXV),
            GS_CLAMP_1 + s_gs->PrimContext);
    }
}

static void ps2GsCoreEmitFullScissor(void)
{
    struct Ps2GsPackedReg *p = ps2GsCoreReserve(1);
    if (p) {
        ps2GsCoreWriteReg(p,
            GS_SETREG_SCISSOR(0, s_gs->Width - 1, 0, s_gs->Height - 1),
            GS_SCISSOR_1 + s_gs->PrimContext);
    }
}

static int ps2GsCoreTextureExponent(uint32_t size)
{
    int exponent = 0;
    uint32_t value = 1;

    while (value < size && exponent < 10) {
        value <<= 1;
        ++exponent;
    }
    return exponent;
}

static uint64_t ps2GsCoreMakeXyz2(int x, int y, uint32_t z)
{
    int fx = x * 16 + s_gs->OffsetX;
    int fy = y * 16 + s_gs->OffsetY;

    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx >= 4096 * 16) fx = 4096 * 16 - 1;
    if (fy >= 4096 * 16) fy = 4096 * 16 - 1;

    return GS_SETREG_XYZ2((uint32_t)fx, (uint32_t)fy, z);
}

extern "C" bool ps2GsCoreInit(const struct Ps2GsCreateInfo *info)
{
    if (s_gs) {
        return true;
    }

    const struct Ps2GsCreateInfo defaults = {
        PS2_GS_PSM_CT16,
        PS2_GS_PSMZ_16,
        true,
        true,
    };
    const struct Ps2GsCreateInfo *config = info ? info : &defaults;

    sysLogPrintf(LOG_NOTE, "GS core: gsKit CRT/VRAM bootstrap");
    ps2LogCheckpoint();

    GSGLOBAL *gs = gsKit_init_global();
    if (!gs) {
        sysLogPrintf(LOG_ERROR, "GS core: gsKit_init_global returned NULL");
        ps2LogCheckpoint();
        return false;
    }

    gs->PSM = config->color_psm;
    gs->PSMZ = config->depth_psm;
    gs->ZBuffering = config->z_buffering ? GS_SETTING_ON : GS_SETTING_OFF;
    gs->Dithering = config->dithering ? GS_SETTING_ON : GS_SETTING_OFF;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
        D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    /*
     * CURRENT IMPLEMENTATION: gsKit still owns CRT setup and initial system
     * VRAM allocation. Frame/state/primitive PATH3 traffic below this point is
     * project-owned and no longer passes through gsKit's FINISH-injecting queue.
     */
    gsKit_vram_clear(gs);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);

    s_gs = gs;
    s_frame_building = false;
    s_depth_update = true;
    s_texture_alpha = false;
    s_native_submit_failed = false;
    s_fog_r = 0;
    s_fog_g = 0;
    s_fog_b = 0;
    memset(s_textures, 0, sizeof(s_textures));

    /* Match the previous gsKit Z-test baseline without queuing a gsKit packet. */
    s_gs->Test->ZTST = config->z_buffering ? 2 : 1;
    s_gs->PrimFogEnable = GS_SETTING_OFF;

    if (!ps2GsNativeQueueInit(PS2_GS_NATIVE_QUEUE_QW)) {
        sysLogPrintf(LOG_ERROR, "GS core: native PATH3 queue initialisation failed");
        ps2LogCheckpoint();
        s_gs = NULL;
        return false;
    }

    sysLogPrintf(LOG_NOTE,
        "GS core: ready %dx%d mode=0x%x PSM=0x%x PSMZ=0x%x z=%d dither=%d",
        gs->Width,
        gs->Height,
        gs->Mode,
        gs->PSM,
        gs->PSMZ,
        gs->ZBuffering == GS_SETTING_ON ? 1 : 0,
        gs->Dithering == GS_SETTING_ON ? 1 : 0);
    sysLogPrintf(LOG_NOTE,
        "GS core: VRAM screen0=%08x screen1=%08x zbuffer=%08x next=%08x",
        gs->ScreenBuffer[0], gs->ScreenBuffer[1], gs->ZBuffer, gs->CurrentPointer);
    sysLogPrintf(LOG_NOTE,
        "GS core: active renderer transport=native GIF PACKED A+D queue=%u QW x2",
        PS2_GS_NATIVE_QUEUE_QW);
    ps2LogCheckpoint();

    return true;
}

extern "C" bool ps2GsCoreIsReady(void)
{
    return s_gs != NULL;
}

extern "C" int ps2GsCoreGetWidth(void)
{
    return s_gs ? s_gs->Width : 0;
}

extern "C" int ps2GsCoreGetHeight(void)
{
    return s_gs ? s_gs->Height : 0;
}

extern "C" int ps2GsCoreGetMode(void)
{
    return s_gs ? s_gs->Mode : 0;
}

extern "C" int ps2GsCoreGetRefreshRate(void)
{
    if (!s_gs) {
        return 60;
    }

    switch (s_gs->Mode) {
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

extern "C" int ps2GsCoreGetOffsetX(void)
{
    return s_gs ? s_gs->OffsetX : 0;
}

extern "C" int ps2GsCoreGetOffsetY(void)
{
    return s_gs ? s_gs->OffsetY : 0;
}

extern "C" void ps2GsCoreBeginFrame(void)
{
    if (!s_gs) {
        return;
    }

    ps2GsNativeQueueBeginFrame();
    s_frame_building = true;

    /* Materialize persistent state into the new command arena. */
    ps2GsCoreEmitFullScissor();
    ps2GsCoreEmitTest();
    ps2GsCoreEmitZbufWriteMask();
    ps2GsCoreEmitClamp();
    ps2GsCoreEmitAlpha();
    ps2GsCoreEmitFogColor();
}

extern "C" void ps2GsCoreSubmit(void)
{
    if (!s_gs || !s_frame_building) {
        return;
    }

    s_frame_building = false;
    if (!ps2GsNativeQueueSubmit()) {
        if (!s_native_submit_failed) {
            sysLogPrintf(LOG_ERROR,
                "GS core: native PATH3 submit failed or command arena overflowed");
            ps2LogCheckpoint();
            s_native_submit_failed = true;
        }
        return;
    }

    /* gsKit_queue_exec used to flip this before gsKit_sync_flip(). */
    s_gs->FirstFrame = GS_SETTING_OFF;
}

extern "C" void ps2GsCorePresent(void)
{
    if (s_gs) {
        /*
         * TRANSITIONAL IMPLEMENTATION: gsKit still owns PCRTC/VSync and buffer
         * selection. It waits for GIF-channel ownership in setactive(), but the
         * renderer no longer injects or waits for GS FINISH every frame.
         */
        gsKit_sync_flip(s_gs);
    }
}

extern "C" void ps2GsCoreClear(bool clear_color, bool clear_depth)
{
    if (!s_gs || !s_frame_building || (!clear_color && !clear_depth)) {
        return;
    }

    /*
     * Preserve the previous gsKit clear contract for the correctness baseline:
     * either requested clear draws the black full-screen sprite sequence while
     * Z comparison is forced ALWAYS, then restores the current TEST register.
     * Fog is explicitly disabled for the clear primitive so persistent material
     * state from the previous frame cannot tint the render target.
     */
    const uint32_t slices = ((uint32_t)s_gs->Width + 63u) / 64u;
    const uint32_t register_count = 4u + slices * 2u;
    struct Ps2GsPackedReg *p = ps2GsCoreReserve(register_count);
    if (!p) {
        return;
    }

    uint32_t out = 0;
    ps2GsCoreWriteReg(&p[out++],
        ps2GsCoreCurrentTestValue(1),
        GS_TEST_1 + s_gs->PrimContext);
    ps2GsCoreWriteReg(&p[out++],
        GS_SETREG_PRIM(
            GS_PRIM_PRIM_SPRITE,
            0,
            0,
            0,
            s_gs->PrimAlphaEnable,
            s_gs->PrimAAEnable,
            0,
            s_gs->PrimContext,
            0),
        GS_PRIM);
    ps2GsCoreWriteReg(&p[out++],
        GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00),
        GS_RGBAQ);

    for (uint32_t slice = 0; slice < slices; ++slice) {
        const int x0 = (int)(slice * 64u);
        int x1 = x0 + 64;
        if (x1 > s_gs->Width) {
            x1 = s_gs->Width;
        }

        ps2GsCoreWriteReg(&p[out++], ps2GsCoreMakeXyz2(x0, 0, 0), GS_XYZ2);
        ps2GsCoreWriteReg(&p[out++], ps2GsCoreMakeXyz2(x1, s_gs->Height, 0), GS_XYZ2);
    }

    ps2GsCoreWriteReg(&p[out++],
        ps2GsCoreCurrentTestValue(-1),
        GS_TEST_1 + s_gs->PrimContext);
}

extern "C" void ps2GsCoreSetScissor(int x, int y, int width, int height)
{
    if (!s_gs || width <= 0 || height <= 0) {
        return;
    }

    int x0 = x;
    int y0 = y;
    int x1 = x + width - 1;
    int y1 = y + height - 1;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= s_gs->Width) x1 = s_gs->Width - 1;
    if (y1 >= s_gs->Height) y1 = s_gs->Height - 1;

    if (!s_frame_building || x0 > x1 || y0 > y1) {
        return;
    }

    struct Ps2GsPackedReg *p = ps2GsCoreReserve(1);
    if (p) {
        ps2GsCoreWriteReg(p,
            GS_SETREG_SCISSOR(x0, x1, y0, y1),
            GS_SCISSOR_1 + s_gs->PrimContext);
    }
}

extern "C" void ps2GsCoreSetDepthMode(bool depth_test, bool depth_update, bool depth_compare)
{
    if (!s_gs) {
        return;
    }

    s_gs->Test->ZTST = (depth_test && depth_compare) ? 2 : 1;
    s_depth_update = depth_update;

    if (s_frame_building) {
        ps2GsCoreEmitTest();
        ps2GsCoreEmitZbufWriteMask();
    }
}

extern "C" void ps2GsCoreSetAlphaBlend(bool enable)
{
    if (!s_gs) {
        return;
    }

    s_gs->PrimAlphaEnable = enable ? GS_SETTING_ON : GS_SETTING_OFF;
    if (enable) {
        s_gs->PrimAlpha = GS_SETREG_ALPHA(0, 1, 0, 1, 0);
        s_gs->PABE = 0;
        if (s_frame_building) {
            ps2GsCoreEmitAlpha();
        }
    }
}

extern "C" void ps2GsCoreSetAlphaTest(bool enable, uint8_t reference)
{
    if (!s_gs) {
        return;
    }

    s_gs->Test->ATE = enable ? GS_SETTING_ON : GS_SETTING_OFF;
    s_gs->Test->ATST = enable ? PS2_GS_ATST_GEQUAL : PS2_GS_ATST_ALWAYS;
    s_gs->Test->AREF = reference;
    s_gs->Test->AFAIL = PS2_GS_AFAIL_KEEP;

    if (s_frame_building) {
        ps2GsCoreEmitTest();
    }
}

extern "C" void ps2GsCoreSetFog(bool enable, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_gs) {
        return;
    }

    s_gs->PrimFogEnable = enable ? GS_SETTING_ON : GS_SETTING_OFF;
    s_fog_r = r;
    s_fog_g = g;
    s_fog_b = b;

    if (enable && s_frame_building) {
        ps2GsCoreEmitFogColor();
    }
}

extern "C" void ps2GsCoreSetTextureAlpha(bool enable)
{
    s_texture_alpha = enable;
}

extern "C" void ps2GsCoreSetTextureClamp(uint32_t cms, uint32_t cmt)
{
    if (!s_gs) {
        return;
    }

    /* N64 G_TX_CLAMP is bit 1; mirror semantics remain a Fast3D TODO. */
    s_gs->Clamp->WMS = (cms & 2u) ? GS_CMODE_CLAMP : GS_CMODE_REPEAT;
    s_gs->Clamp->WMT = (cmt & 2u) ? GS_CMODE_CLAMP : GS_CMODE_REPEAT;
    if (s_frame_building) {
        ps2GsCoreEmitClamp();
    }
}

extern "C" Ps2GsTextureHandle ps2GsCoreCreateTexture(void)
{
    for (uint32_t i = 0; i < PS2_GS_MAX_TEXTURES; ++i) {
        if (!s_textures[i].used) {
            memset(&s_textures[i], 0, sizeof(s_textures[i]));
            s_textures[i].used = true;
            return (Ps2GsTextureHandle)(i + 1);
        }
    }

    sysLogPrintf(LOG_ERROR, "GS core: texture table exhausted (%d)", PS2_GS_MAX_TEXTURES);
    return PS2_GS_TEXTURE_INVALID;
}

extern "C" bool ps2GsCoreTextureExists(Ps2GsTextureHandle handle)
{
    return ps2GsCoreTextureSlot(handle) != NULL;
}

extern "C" bool ps2GsCoreTextureReady(Ps2GsTextureHandle handle)
{
    struct Ps2GsTextureSlot *slot = ps2GsCoreTextureSlot(handle);
    return slot && slot->uploaded;
}

extern "C" bool ps2GsCoreUploadTextureRgba32(Ps2GsTextureHandle handle,
    const uint8_t *rgba32, uint32_t width, uint32_t height)
{
    if (!s_gs || !rgba32 || width == 0 || height == 0 || width > 1024 || height > 1024) {
        return false;
    }

    struct Ps2GsTextureSlot *slot = ps2GsCoreTextureSlot(handle);
    if (!slot) {
        return false;
    }

    GSTEXTURE *tex = &slot->texture;
    const u32 bytes = gsKit_texture_size((int)width, (int)height, GS_PSM_CT32);

    /*
     * CURRENT IMPLEMENTATION: gsKit's VRAM allocator is monotonic. Preserve the
     * existing correctness rule and reject resizing an already resident handle
     * rather than leak another allocation behind the caller's back.
     */
    if (slot->uploaded && (tex->Width != width || tex->Height != height)) {
        sysLogPrintf(LOG_WARNING,
            "GS core: texture resize rejected id=%u old=%ux%u new=%ux%u",
            (unsigned int)handle, tex->Width, tex->Height, width, height);
        return false;
    }

    if (!slot->uploaded) {
        memset(tex, 0, sizeof(*tex));
        tex->Width = width;
        tex->Height = height;
        tex->PSM = GS_PSM_CT32;
        tex->Filter = GS_FILTER_NEAREST;
        tex->Vram = gsKit_vram_alloc(s_gs, bytes, GSKIT_ALLOC_USERBUFFER);
        if (tex->Vram == GSKIT_ALLOC_ERROR) {
            sysLogPrintf(LOG_ERROR,
                "GS core: VRAM allocation failed id=%u size=%u",
                (unsigned int)handle, bytes);
            return false;
        }
        slot->uploaded = true;
    }

    /*
     * CURRENT IMPLEMENTATION: texture transport is the last synchronous gsKit
     * datapath retained by the active renderer. It lives outside frame command
     * building and will be replaced by native IMAGE-mode upload batching next.
     */
    tex->Mem = (u32 *)(uintptr_t)rgba32;
    gsKit_texture_upload(s_gs, tex);
    tex->Mem = NULL;
    return true;
}

extern "C" void ps2GsCoreSetTextureFilter(Ps2GsTextureHandle handle, bool linear_filter)
{
    struct Ps2GsTextureSlot *slot = ps2GsCoreTextureSlot(handle);
    if (slot) {
        slot->texture.Filter = linear_filter ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
    }
}

extern "C" void ps2GsCoreReleaseTexture(Ps2GsTextureHandle handle)
{
    struct Ps2GsTextureSlot *slot = ps2GsCoreTextureSlot(handle);
    if (slot) {
        /*
         * CURRENT IMPLEMENTATION: logical retirement only. VRAM is not
         * reclaimed until the native residency allocator replaces gsKit's
         * monotonic allocator.
         */
        slot->used = false;
    }
}

extern "C" void ps2GsCoreDrawColorTriangles(const struct Ps2GsColorVertex *vertices,
    uint32_t vertex_count)
{
    if (!s_gs || !s_frame_building || !vertices || vertex_count == 0) {
        return;
    }

    const uint32_t register_count = 1u + vertex_count * 2u;
    struct Ps2GsPackedReg *p = ps2GsCoreReserve(register_count);
    if (!p) {
        return;
    }

    ps2GsCoreWriteReg(&p[0],
        GS_SETREG_PRIM(
            GS_PRIM_PRIM_TRIANGLE,
            1,
            0,
            s_gs->PrimFogEnable,
            s_gs->PrimAlphaEnable,
            s_gs->PrimAAEnable,
            0,
            s_gs->PrimContext,
            0),
        GS_PRIM);

    memcpy(&p[1], vertices, (size_t)vertex_count * sizeof(*vertices));
}

extern "C" void ps2GsCoreDrawTexturedTriangles(Ps2GsTextureHandle handle,
    const struct Ps2GsTexturedVertex *vertices, uint32_t vertex_count)
{
    if (!s_gs || !s_frame_building || !vertices || vertex_count == 0) {
        return;
    }

    struct Ps2GsTextureSlot *slot = ps2GsCoreTextureSlot(handle);
    if (!slot || !slot->uploaded) {
        return;
    }

    GSTEXTURE *tex = &slot->texture;
    const int tw = ps2GsCoreTextureExponent(tex->Width);
    const int th = ps2GsCoreTextureExponent(tex->Height);
    const uint32_t register_count = 3u + vertex_count * 3u;
    struct Ps2GsPackedReg *p = ps2GsCoreReserve(register_count);
    if (!p) {
        return;
    }

    ps2GsCoreWriteReg(&p[0],
        GS_SETREG_TEX1(0, 0, tex->Filter, tex->Filter, 0, 0, 0),
        GS_TEX1_1 + s_gs->PrimContext);
    ps2GsCoreWriteReg(&p[1],
        GS_SETREG_TEX0(
            tex->Vram / 256,
            tex->TBW,
            tex->PSM,
            tw,
            th,
            s_texture_alpha ? 1 : 0,
            0,
            0,
            0,
            0,
            0,
            GS_CLUT_STOREMODE_NOLOAD),
        GS_TEX0_1 + s_gs->PrimContext);
    ps2GsCoreWriteReg(&p[2],
        GS_SETREG_PRIM(
            GS_PRIM_PRIM_TRIANGLE,
            1,
            1,
            s_gs->PrimFogEnable,
            s_gs->PrimAlphaEnable,
            s_gs->PrimAAEnable,
            0,
            s_gs->PrimContext,
            0),
        GS_PRIM);

    memcpy(&p[3], vertices, (size_t)vertex_count * sizeof(*vertices));
}
