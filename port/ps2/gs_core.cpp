#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <dmaKit.h>
#include <gsKit.h>
#include <gsInline.h>

#include "gs_core.h"
#include "log_ps2.h"
#include "system.h"

#define PS2_GS_MAX_TEXTURES 64

struct Ps2GsTextureSlot {
    bool used;
    bool uploaded;
    GSTEXTURE texture;
};

static GSGLOBAL *s_gs;
static struct Ps2GsTextureSlot s_textures[PS2_GS_MAX_TEXTURES];

static struct Ps2GsTextureSlot *ps2GsCoreTextureSlot(Ps2GsTextureHandle handle)
{
    if (handle == PS2_GS_TEXTURE_INVALID || handle > PS2_GS_MAX_TEXTURES) {
        return NULL;
    }

    struct Ps2GsTextureSlot *slot = &s_textures[handle - 1];
    return slot->used ? slot : NULL;
}

extern "C" bool ps2GsCoreInit(const struct Ps2GsCreateInfo *info)
{
    if (s_gs) {
        return true;
    }

    const struct Ps2GsCreateInfo defaults = {
        GS_PSM_CT16,
        GS_PSMZ_16,
        true,
        true,
    };
    const struct Ps2GsCreateInfo *config = info ? info : &defaults;

    sysLogPrintf(LOG_NOTE, "GS core: gsKit_init_global");
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

    /*
     * CURRENT IMPLEMENTATION: dmaKit + gsKit own the PATH3 queue transport.
     * Keeping this in one device module makes the later direct GIF packet
     * backend a transport replacement instead of a Fast3D rewrite.
     */
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
        D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    /* gsKit's VRAM allocator owns the bootstrap framebuffer/Z allocations. */
    gsKit_vram_clear(gs);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_set_test(gs, config->z_buffering ? GS_ZTEST_ON : GS_ZTEST_OFF);

    memset(s_textures, 0, sizeof(s_textures));
    s_gs = gs;

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
    ps2LogCheckpoint();

    return true;
}

extern "C" GSGLOBAL *ps2GsCoreGetLegacyGlobal(void)
{
    return s_gs;
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

extern "C" void ps2GsCoreBeginFrame(void)
{
    if (s_gs) {
        gsKit_set_scissor(s_gs, GS_SCISSOR_RESET);
    }
}

extern "C" void ps2GsCoreSubmit(void)
{
    if (s_gs) {
        /* Submit only. No GS FINISH and no VSync wait on this boundary. */
        gsKit_queue_exec(s_gs);
    }
}

extern "C" void ps2GsCorePresent(void)
{
    if (s_gs) {
        /* Final dependency for the scanout buffer remains as late as possible. */
        gsKit_sync_flip(s_gs);
    }
}

extern "C" void ps2GsCoreClear(bool clear_color, bool clear_depth)
{
    if (!s_gs || (!clear_color && !clear_depth)) {
        return;
    }

    /*
     * Bring-up contract: current gsKit clear updates color and Z together.
     * Split clears will become explicit GS packets when a real Perfect Dark
     * pass requires them. Do not silently fake one half of the operation.
     */
    gsKit_clear(s_gs, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));
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

    if (x0 <= x1 && y0 <= y1) {
        gsKit_set_scissor(s_gs, GS_SETREG_SCISSOR(x0, x1, y0, y1));
    }
}

static void ps2GsCoreEmitZbufWriteMask(bool depth_update)
{
    if (!s_gs || !s_gs->ZBuffering) {
        return;
    }

    u64 *p = (u64 *)gsKit_heap_alloc(s_gs, 1, 16, GIF_AD);
    *p++ = GIF_TAG_AD(1);
    *p++ = GIF_AD;
    *p++ = GS_SETREG_ZBUF(s_gs->ZBuffer / 8192, s_gs->PSMZ, depth_update ? 0 : 1);
    *p++ = GS_ZBUF_1 + s_gs->PrimContext;
}

extern "C" void ps2GsCoreSetDepthMode(bool depth_test, bool depth_update, bool depth_compare)
{
    if (!s_gs) {
        return;
    }

    if (depth_test && depth_compare) {
        gsKit_set_test(s_gs, GS_ZTEST_ON);
    } else {
        gsKit_set_test(s_gs, GS_ZTEST_OFF);
    }

    ps2GsCoreEmitZbufWriteMask(depth_update);
}

extern "C" void ps2GsCoreSetAlphaBlend(bool enable)
{
    if (!s_gs) {
        return;
    }

    s_gs->PrimAlphaEnable = enable ? GS_SETTING_ON : GS_SETTING_OFF;
    if (enable) {
        /* Standard source-alpha over destination baseline. */
        gsKit_set_primalpha(s_gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
    }
}

extern "C" void ps2GsCoreSetTextureClamp(uint32_t cms, uint32_t cmt)
{
    if (!s_gs) {
        return;
    }

    /* N64 G_TX_CLAMP is bit 1; mirror semantics remain a Fast3D TODO. */
    s_gs->Clamp->WMS = (cms & 2u) ? GS_CMODE_CLAMP : GS_CMODE_REPEAT;
    s_gs->Clamp->WMT = (cmt & 2u) ? GS_CMODE_CLAMP : GS_CMODE_REPEAT;
    gsKit_set_clamp(s_gs, GS_CMODE_RESET);
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
     * CURRENT IMPLEMENTATION: current gsKit_texture_upload() ultimately uses
     * the synchronous texture-send path, including DMA waits before and after
     * the transfer. Keeping that policy here makes it removable without
     * changing Fast3D when native upload batching is introduced.
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

extern "C" void ps2GsCoreDrawColorTriangles(const GSPRIMPOINT *vertices, uint32_t vertex_count)
{
    if (!s_gs || !vertices || vertex_count == 0) {
        return;
    }

    gsKit_prim_list_triangle_gouraud_3d(s_gs, (int)vertex_count, vertices);
}

extern "C" void ps2GsCoreDrawTexturedTriangles(Ps2GsTextureHandle handle,
    const GSPRIMSTQPOINT *vertices, uint32_t vertex_count)
{
    if (!s_gs || !vertices || vertex_count == 0) {
        return;
    }

    struct Ps2GsTextureSlot *slot = ps2GsCoreTextureSlot(handle);
    if (!slot || !slot->uploaded) {
        return;
    }

    gsKit_set_texfilter(s_gs, slot->texture.Filter);
    gsKit_prim_list_triangle_goraud_texture_stq_3d(
        s_gs, &slot->texture, (int)vertex_count, vertices);
}
