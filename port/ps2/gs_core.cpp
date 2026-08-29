#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <dmaKit.h>
#include <gsKit.h>

#include "gs_command_budget.h"
#include "gs_core.h"
#include "gs_native_queue.h"
#include "gs_render_target_layout.h"
#include "gs_state_shadow.h"
#include "gs_texture_convert.h"
#include "gs_vram_allocator.h"
#include "log_ps2.h"
#include "system.h"

#define PS2_GS_MAX_TEXTURES 64
#define PS2_GS_MAX_RENDER_TARGETS 4
#define PS2_GS_MAX_RETIRED_BLOCKS (PS2_GS_MAX_TEXTURES * 2)
#define PS2_GS_NATIVE_QUEUE_QW 16384u
#define PS2_GS_SHARED_CLUT_COUNT 5u
#define PS2_GS_ALPHA_IDENTITY_CLUT_INDEX 4u

/* GS TEST.ATST encodings, cross-checked against current PS2SDK libgs. */
#define PS2_GS_ATST_ALWAYS  1u
#define PS2_GS_ATST_LEQUAL  3u
#define PS2_GS_ATST_GEQUAL  5u
#define PS2_GS_AFAIL_KEEP   0u

struct Ps2GsTextureSlot {
    bool used;
    bool resident;
    bool uploaded;
    uint32_t vram_bytes;
    uint32_t clut_vram_bytes;
    GSTEXTURE texture;
};

struct Ps2GsRetiredVramBlock {
    uint32_t offset;
    uint32_t size;
};

struct Ps2GsSharedClut {
    bool resident;
    uint32_t vram;
    uint32_t bytes;
};

struct Ps2GsRenderTargetSlot {
    bool used;
    bool has_contents;
    bool texture_cache_dirty;
    uint32_t vram;
    struct Ps2GsRenderTargetLayout layout;
};

static GSGLOBAL *s_gs;
static struct Ps2GsTextureSlot s_textures[PS2_GS_MAX_TEXTURES];
static struct Ps2GsRenderTargetSlot
    s_render_targets[PS2_GS_MAX_RENDER_TARGETS];
static struct Ps2GsVramAllocator s_vram_allocator;
static struct Ps2GsRetiredVramBlock
    s_retired_vram[PS2_GS_MAX_RETIRED_BLOCKS];
static struct Ps2GsSharedClut s_shared_cluts[PS2_GS_SHARED_CLUT_COUNT];
static uint32_t s_shared_clut_staging[256] __attribute__((aligned(64)));
static uint32_t s_retired_vram_count;
static bool s_frame_building;
static bool s_depth_update = true;
static bool s_color_write = true;
static bool s_alpha_write = true;
static bool s_framebuffer_alpha_force;
static bool s_texture_alpha;
static bool s_native_submit_failed;
static bool s_native_finish_failed;
static bool s_native_present_failed;
static bool s_logged_command_arena_spill;
static bool s_logged_oversized_command;
static Ps2GsRenderTargetHandle s_active_render_target;
static uint32_t s_loaded_clut_vram;
static int s_loaded_clut_psm;
static uint8_t s_fog_r;
static uint8_t s_fog_g;
static uint8_t s_fog_b;
static uint32_t s_scissor_x0;
static uint32_t s_scissor_x1;
static uint32_t s_scissor_y0;
static uint32_t s_scissor_y1;
static struct Ps2GsStateShadow s_state_shadow;

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

static struct Ps2GsRenderTargetSlot *ps2GsCoreRenderTargetSlot(
    Ps2GsRenderTargetHandle handle)
{
    if (handle == PS2_GS_RENDER_TARGET_DEFAULT ||
        handle > PS2_GS_MAX_RENDER_TARGETS) {
        return NULL;
    }

    struct Ps2GsRenderTargetSlot *slot = &s_render_targets[handle - 1u];
    return slot->used ? slot : NULL;
}

static uint32_t ps2GsCoreTargetWidth(void)
{
    struct Ps2GsRenderTargetSlot *slot =
        ps2GsCoreRenderTargetSlot(s_active_render_target);
    return slot ? slot->layout.width : (uint32_t)s_gs->Width;
}

static uint32_t ps2GsCoreTargetHeight(void)
{
    struct Ps2GsRenderTargetSlot *slot =
        ps2GsCoreRenderTargetSlot(s_active_render_target);
    return slot ? slot->layout.height : (uint32_t)s_gs->Height;
}

static uint32_t ps2GsCoreTargetVram(void)
{
    struct Ps2GsRenderTargetSlot *slot =
        ps2GsCoreRenderTargetSlot(s_active_render_target);
    return slot ? slot->vram :
        s_gs->ScreenBuffer[s_gs->ActiveBuffer & 1u];
}

static uint32_t ps2GsCoreTargetFbw(void)
{
    struct Ps2GsRenderTargetSlot *slot =
        ps2GsCoreRenderTargetSlot(s_active_render_target);
    return slot ? slot->layout.fbw : (uint32_t)s_gs->Width / 64u;
}

static int ps2GsCoreTargetPsm(void)
{
    return s_active_render_target == PS2_GS_RENDER_TARGET_DEFAULT
        ? s_gs->PSM : GS_PSM_CT32;
}

static bool ps2GsCoreReclaimRetiredVram(void)
{
    uint32_t failed = 0;
    for (uint32_t i = 0; i < s_retired_vram_count; ++i) {
        const struct Ps2GsRetiredVramBlock block = s_retired_vram[i];
        if (!ps2GsVramAllocatorFree(
                &s_vram_allocator, block.offset, block.size)) {
            s_retired_vram[failed++] = block;
        }
    }
    s_retired_vram_count = failed;

    if (failed != 0) {
        sysLogPrintf(LOG_ERROR,
            "GS core: failed to reclaim %u retired VRAM block(s)", failed);
        ps2LogCheckpoint();
        return false;
    }
    return true;
}

static bool ps2GsCoreDrainAndFence(void)
{
    if (s_frame_building) {
        /*
         * Submit the current draw arena before the fence. The replacement
         * arena starts empty; persistent GS registers remain live, so no state
         * replay is required at this mid-frame ownership boundary.
         */
        if (!ps2GsNativeQueueSubmit()) {
            return false;
        }
        ps2GsNativeQueueBeginFrame();
    }

    if (!ps2GsNativeQueueWaitGs()) {
        return false;
    }
    return ps2GsCoreReclaimRetiredVram();
}

static bool ps2GsCoreEnsureRetireCapacity(uint32_t required)
{
    if (required <= PS2_GS_MAX_RETIRED_BLOCKS - s_retired_vram_count) {
        return true;
    }
    return ps2GsCoreDrainAndFence() &&
        required <= PS2_GS_MAX_RETIRED_BLOCKS - s_retired_vram_count;
}

static void ps2GsCoreRetireVram(uint32_t offset, uint32_t size)
{
    s_retired_vram[s_retired_vram_count].offset = offset;
    s_retired_vram[s_retired_vram_count].size = size;
    ++s_retired_vram_count;
}

static uint32_t ps2GsCoreResidentBlockCount(
    const struct Ps2GsTextureSlot *slot)
{
    return slot && slot->resident ?
        1u + (slot->clut_vram_bytes != 0u ? 1u : 0u) : 0u;
}

static void ps2GsCoreRetireTextureBlocks(
    const struct Ps2GsTextureSlot *slot)
{
    ps2GsCoreRetireVram(slot->texture.Vram, slot->vram_bytes);
    if (slot->clut_vram_bytes != 0u) {
        ps2GsCoreRetireVram(slot->texture.VramClut,
            slot->clut_vram_bytes);
    }
}

static void ps2GsCoreLogVramAllocationFailure(Ps2GsTextureHandle handle,
    uint32_t bytes, uint32_t width, uint32_t height)
{
    struct Ps2GsVramStats stats;
    ps2GsVramAllocatorGetStats(&s_vram_allocator, &stats);
    sysLogPrintf(LOG_ERROR,
        "GS core: VRAM allocation failed id=%u size=%u (%ux%u) "
        "free=%u largest=%u ranges=%u",
        (unsigned int)handle, bytes, width, height,
        stats.free_bytes, stats.largest_free_bytes,
        (unsigned int)stats.free_ranges);
    ps2LogCheckpoint();
}

static void ps2GsCoreInvalidateClutCache(void)
{
    s_loaded_clut_vram = UINT32_MAX;
    s_loaded_clut_psm = -1;
    ps2GsStateShadowInvalidate(&s_state_shadow, PS2_GS_STATE_TEX0);
}

static struct Ps2GsPackedReg *ps2GsCoreReserve(uint32_t register_count)
{
    if (!s_frame_building || register_count == 0u) {
        return NULL;
    }

    const uint32_t used_qw = ps2GsNativeQueueUsedQwords();
    const uint32_t capacity_qw = ps2GsNativeQueueCapacityQwords();
    const enum Ps2GsCommandBudgetDecision decision =
        ps2GsClassifyCommandReservation(
            used_qw, capacity_qw, register_count);
    if (decision == PS2_GS_COMMAND_TOO_LARGE) {
        if (!s_logged_oversized_command) {
            sysLogPrintf(LOG_ERROR,
                "GS core: single command packet exceeds arena registers=%u capacity=%u QW",
                register_count, capacity_qw);
            ps2LogCheckpoint();
            s_logged_oversized_command = true;
        }
        return NULL;
    }
    if (decision == PS2_GS_COMMAND_SPILL) {
        /*
         * This is a GIF-channel ownership boundary, not a GS dependency.
         * Register state persists across ordered PATH3 DMA submissions, so the
         * next arena continues the same frame without replay or FINISH.
         */
        if (!ps2GsNativeQueueSubmit()) {
            return NULL;
        }
        ps2GsNativeQueueBeginFrame();
        if (!s_logged_command_arena_spill) {
            sysLogPrintf(LOG_NOTE,
                "GS core: command arena spill enabled used=%u request=%u capacity=%u QW",
                used_qw, register_count + 1u, capacity_qw);
            ps2LogCheckpoint();
            s_logged_command_arena_spill = true;
        }
    }
    return ps2GsNativeQueueReserveAd(register_count);
}

static struct Ps2GsPackedReg *ps2GsCoreReserveChunk(uint32_t register_count)
{
    if (!s_frame_building || register_count == 0u) {
        return NULL;
    }

    return ps2GsCoreReserve(register_count);
}

static void ps2GsCoreWriteReg(struct Ps2GsPackedReg *dst, uint64_t value, uint64_t reg)
{
    dst->value = value;
    dst->reg = reg;
}

static void ps2GsCoreEmitCachedState(enum Ps2GsStateSlot slot,
    uint64_t value, uint64_t reg)
{
    if (!ps2GsStateShadowNeedsWrite(&s_state_shadow, slot, value)) {
        return;
    }

    struct Ps2GsPackedReg *p = ps2GsCoreReserve(1u);
    if (!p) {
        return;
    }
    ps2GsCoreWriteReg(p, value, reg);
    ps2GsStateShadowCommit(&s_state_shadow, slot, value);
}

static uint64_t ps2GsCoreCurrentTestValue(int ztst_override)
{
    const int ztst = ztst_override >= 0 ? ztst_override : s_gs->Test->ZTST;
    const int zte = s_active_render_target == PS2_GS_RENDER_TARGET_DEFAULT
        ? s_gs->Test->ZTE : 0;
    return GS_SETREG_TEST(
        s_gs->Test->ATE,
        s_gs->Test->ATST,
        s_gs->Test->AREF,
        s_gs->Test->AFAIL,
        s_gs->Test->DATE,
        s_gs->Test->DATM,
        zte,
        ztst);
}

static void ps2GsCoreEmitTest(void)
{
    ps2GsCoreEmitCachedState(
        PS2_GS_STATE_TEST,
        ps2GsCoreCurrentTestValue(-1),
        GS_TEST_1 + s_gs->PrimContext);
}

static void ps2GsCoreEmitZbufWriteMask(void)
{
    if (!s_gs->ZBuffering ||
        s_active_render_target != PS2_GS_RENDER_TARGET_DEFAULT) {
        return;
    }

    ps2GsCoreEmitCachedState(
        PS2_GS_STATE_ZBUF,
        GS_SETREG_ZBUF(
            s_gs->ZBuffer / 8192, s_gs->PSMZ,
            s_depth_update ? 0 : 1),
        GS_ZBUF_1 + s_gs->PrimContext);
}

static uint32_t ps2GsCoreFrameMask(void)
{
    if (!s_gs) {
        return 0;
    }
    if (!s_color_write) {
        return 0xffffffffu;
    }
    if (s_alpha_write) {
        return 0;
    }

    switch (ps2GsCoreTargetPsm()) {
        case PS2_GS_PSM_CT32:
            return 0xff000000u;
        case PS2_GS_PSM_CT16:
        case PS2_GS_PSM_CT16S:
            /* FRAME mask bit 31 maps to the converted CT16 alpha bit. */
            return 0x80000000u;
        default:
            return 0;
    }
}

static void ps2GsCoreEmitFrameMask(void)
{
    if (!s_gs) {
        return;
    }

    ps2GsCoreEmitCachedState(
        PS2_GS_STATE_FRAME,
        GS_SETREG_FRAME_1(
            ps2GsCoreTargetVram() / PS2_GS_FRAMEBUFFER_ALIGNMENT,
            ps2GsCoreTargetFbw(),
            ps2GsCoreTargetPsm(),
            ps2GsCoreFrameMask()),
        GS_FRAME_1 + s_gs->PrimContext);
}

static void ps2GsCoreEmitFramebufferAlphaForce(void)
{
    if (s_gs) {
        ps2GsCoreEmitCachedState(
            PS2_GS_STATE_FBA,
            s_framebuffer_alpha_force ? 1u : 0u,
            GS_FBA_1 + s_gs->PrimContext);
    }
}

static void ps2GsCoreEmitAlpha(void)
{
    if (!s_gs->PrimAlphaEnable) {
        return;
    }

    const uint64_t values[2] = { s_gs->PABE, s_gs->PrimAlpha };
    const uint64_t registers[2] = {
        GS_PABE, GS_ALPHA_1 + s_gs->PrimContext,
    };
    const enum Ps2GsStateSlot slots[2] = {
        PS2_GS_STATE_PABE, PS2_GS_STATE_ALPHA,
    };
    bool emit[2] = {
        ps2GsStateShadowNeedsWrite(
            &s_state_shadow, slots[0], values[0]),
        ps2GsStateShadowNeedsWrite(
            &s_state_shadow, slots[1], values[1]),
    };
    const uint32_t register_count =
        (emit[0] ? 1u : 0u) + (emit[1] ? 1u : 0u);
    if (register_count == 0u) {
        return;
    }

    struct Ps2GsPackedReg *p = ps2GsCoreReserve(register_count);
    if (!p) {
        return;
    }
    uint32_t out = 0u;
    for (uint32_t i = 0u; i < 2u; ++i) {
        if (emit[i]) {
            ps2GsCoreWriteReg(&p[out++], values[i], registers[i]);
            ps2GsStateShadowCommit(
                &s_state_shadow, slots[i], values[i]);
        }
    }
}

static void ps2GsCoreEmitFogColor(void)
{
    if (!s_gs->PrimFogEnable) {
        return;
    }

    ps2GsCoreEmitCachedState(
        PS2_GS_STATE_FOGCOL,
        (uint64_t)s_fog_r |
            ((uint64_t)s_fog_g << 8) |
            ((uint64_t)s_fog_b << 16),
        GS_FOGCOL);
}

static void ps2GsCoreEmitClamp(void)
{
    ps2GsCoreEmitCachedState(
        PS2_GS_STATE_CLAMP,
        GS_SETREG_CLAMP(
            s_gs->Clamp->WMS,
            s_gs->Clamp->WMT,
            s_gs->Clamp->MINU,
            s_gs->Clamp->MAXU,
            s_gs->Clamp->MINV,
            s_gs->Clamp->MAXV),
        GS_CLAMP_1 + s_gs->PrimContext);
}

static void ps2GsCoreEmitTextureAlphaExpansion(void)
{
    /*
     * Expand the one-bit alpha of PSMCT16 to 0xff. This deliberately matches
     * RGBA32 texel alpha, preserving the adapter's 0x40 fragment compensation
     * for GS MODULATE across both resident formats.
     */
    ps2GsCoreEmitCachedState(
        PS2_GS_STATE_TEXA,
        GS_SETREG_TEXA(0x00, 0, 0xff),
        GS_TEXA);
}

static void ps2GsCoreEmitScissor(void)
{
    ps2GsCoreEmitCachedState(
        PS2_GS_STATE_SCISSOR,
        GS_SETREG_SCISSOR(
            s_scissor_x0, s_scissor_x1,
            s_scissor_y0, s_scissor_y1),
        GS_SCISSOR_1 + s_gs->PrimContext);
}

static void ps2GsCoreSetFullScissor(void)
{
    s_scissor_x0 = 0u;
    s_scissor_x1 = ps2GsCoreTargetWidth() - 1u;
    s_scissor_y0 = 0u;
    s_scissor_y1 = ps2GsCoreTargetHeight() - 1u;
    ps2GsCoreEmitScissor();
}

static void ps2GsCoreEmitRenderTargetState(void)
{
    if (!s_frame_building) {
        return;
    }

    ps2GsCoreEmitFrameMask();
    ps2GsCoreSetFullScissor();
    ps2GsCoreEmitTest();
    ps2GsCoreEmitZbufWriteMask();
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

static void ps2GsCoreMarkActiveRenderTargetWritten(void)
{
    struct Ps2GsRenderTargetSlot *slot =
        ps2GsCoreRenderTargetSlot(s_active_render_target);
    if (slot) {
        slot->has_contents = true;
        slot->texture_cache_dirty = true;
    }
}

static uint64_t ps2GsCoreMakeXyz2Fixed(int x16, int y16, uint32_t z)
{
    int fx = x16 + s_gs->OffsetX;
    int fy = y16 + s_gs->OffsetY;

    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx >= 4096 * 16) fx = 4096 * 16 - 1;
    if (fy >= 4096 * 16) fy = 4096 * 16 - 1;

    return GS_SETREG_XYZ2((uint32_t)fx, (uint32_t)fy, z);
}

static uint64_t ps2GsCoreMakeXyz2(int x, int y, uint32_t z)
{
    return ps2GsCoreMakeXyz2Fixed(x * 16, y * 16, z);
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
    s_color_write = true;
    s_alpha_write = true;
    s_framebuffer_alpha_force = false;
    s_texture_alpha = false;
    s_native_submit_failed = false;
    s_native_finish_failed = false;
    s_native_present_failed = false;
    s_logged_command_arena_spill = false;
    s_logged_oversized_command = false;
    s_active_render_target = PS2_GS_RENDER_TARGET_DEFAULT;
    ps2GsCoreInvalidateClutCache();
    s_fog_r = 0;
    s_fog_g = 0;
    s_fog_b = 0;
    s_scissor_x0 = 0u;
    s_scissor_x1 = (uint32_t)gs->Width - 1u;
    s_scissor_y0 = 0u;
    s_scissor_y1 = (uint32_t)gs->Height - 1u;
    ps2GsStateShadowReset(&s_state_shadow);
    memset(s_textures, 0, sizeof(s_textures));
    memset(s_render_targets, 0, sizeof(s_render_targets));
    memset(s_retired_vram, 0, sizeof(s_retired_vram));
    memset(s_shared_cluts, 0, sizeof(s_shared_cluts));
    s_retired_vram_count = 0;

    /* Match the previous gsKit Z-test baseline without queuing a gsKit packet. */
    s_gs->Test->ZTST = config->z_buffering ? 2 : 1;
    s_gs->PrimFogEnable = GS_SETTING_OFF;

    if (!ps2GsNativeQueueInit(PS2_GS_NATIVE_QUEUE_QW)) {
        sysLogPrintf(LOG_ERROR, "GS core: native PATH3 queue initialisation failed");
        ps2LogCheckpoint();
        s_gs = NULL;
        return false;
    }

    if (!ps2GsVramAllocatorInit(
            &s_vram_allocator, gs->CurrentPointer, PS2_GS_VRAM_BYTES)) {
        sysLogPrintf(LOG_ERROR,
            "GS core: texture VRAM pool initialisation failed begin=%08x",
            gs->CurrentPointer);
        ps2LogCheckpoint();
        s_gs = NULL;
        return false;
    }

    struct Ps2GsVramStats vram_stats;
    ps2GsVramAllocatorGetStats(&s_vram_allocator, &vram_stats);

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
        "GS core: reclaimable texture VRAM pool=%u KiB block=%u bytes",
        vram_stats.pool_bytes / 1024u, PS2_GS_VRAM_BLOCK_BYTES);
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
    ps2GsCoreSetFullScissor();
    ps2GsCoreEmitTest();
    ps2GsCoreEmitZbufWriteMask();
    ps2GsCoreEmitFrameMask();
    ps2GsCoreEmitFramebufferAlphaForce();
    ps2GsCoreEmitClamp();
    ps2GsCoreEmitTextureAlphaExpansion();
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
}

extern "C" void ps2GsCorePresent(void)
{
    if (!s_gs) {
        return;
    }

    /*
     * PCRTC is about to expose the completed draw buffer. This is a real GS
     * dependency, so wait for a project-owned FINISH token here, then reclaim
     * texture blocks whose last baked TEX0 references are now consumer-free.
     */
    if (!ps2GsNativeQueueWaitGs()) {
        if (!s_native_finish_failed) {
            sysLogPrintf(LOG_ERROR,
                "GS core: native GS FINISH fence failed; frame not presented");
            ps2LogCheckpoint();
            s_native_finish_failed = true;
        }
        return;
    }
    ps2GsCoreReclaimRetiredVram();

    if (!ps2GsNativeQueuePresent(s_gs)) {
        if (!s_native_present_failed) {
            sysLogPrintf(LOG_ERROR,
                "GS core: native PCRTC present failed");
            ps2LogCheckpoint();
            s_native_present_failed = true;
        }
    } else {
        /* Native present materializes the default FRAME/SCISSOR registers. */
        s_active_render_target = PS2_GS_RENDER_TARGET_DEFAULT;
        s_scissor_x0 = 0u;
        s_scissor_x1 = (uint32_t)s_gs->Width - 1u;
        s_scissor_y0 = 0u;
        s_scissor_y1 = (uint32_t)s_gs->Height - 1u;
        ps2GsStateShadowInvalidate(
            &s_state_shadow, PS2_GS_STATE_FRAME);
        ps2GsStateShadowInvalidate(
            &s_state_shadow, PS2_GS_STATE_SCISSOR);
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
    const uint32_t target_width = ps2GsCoreTargetWidth();
    const uint32_t target_height = ps2GsCoreTargetHeight();
    const uint32_t slices = (target_width + 63u) / 64u;
    const uint32_t register_count = 4u + slices * 2u;
    struct Ps2GsPackedReg *p = ps2GsCoreReserve(register_count);
    if (!p) {
        return;
    }

    uint32_t out = 0;
    const uint64_t restored_test = ps2GsCoreCurrentTestValue(-1);
    const uint64_t clear_prim = GS_SETREG_PRIM(
        GS_PRIM_PRIM_SPRITE,
        0,
        0,
        0,
        s_gs->PrimAlphaEnable,
        s_gs->PrimAAEnable,
        0,
        s_gs->PrimContext,
        0);
    ps2GsCoreWriteReg(&p[out++],
        ps2GsCoreCurrentTestValue(1),
        GS_TEST_1 + s_gs->PrimContext);
    ps2GsCoreWriteReg(&p[out++],
        clear_prim,
        GS_PRIM);
    ps2GsCoreWriteReg(&p[out++],
        GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00),
        GS_RGBAQ);

    for (uint32_t slice = 0; slice < slices; ++slice) {
        const int x0 = (int)(slice * 64u);
        int x1 = x0 + 64;
        if ((uint32_t)x1 > target_width) {
            x1 = (int)target_width;
        }

        ps2GsCoreWriteReg(&p[out++], ps2GsCoreMakeXyz2(x0, 0, 0), GS_XYZ2);
        ps2GsCoreWriteReg(&p[out++],
            ps2GsCoreMakeXyz2(x1, (int)target_height, 0), GS_XYZ2);
    }

    ps2GsCoreWriteReg(&p[out++],
        restored_test,
        GS_TEST_1 + s_gs->PrimContext);
    ps2GsStateShadowCommit(
        &s_state_shadow, PS2_GS_STATE_TEST, restored_test);
    ps2GsStateShadowCommit(
        &s_state_shadow, PS2_GS_STATE_PRIM, clear_prim);
    ps2GsCoreMarkActiveRenderTargetWritten();
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
    const int target_width = (int)ps2GsCoreTargetWidth();
    const int target_height = (int)ps2GsCoreTargetHeight();
    if (x1 >= target_width) x1 = target_width - 1;
    if (y1 >= target_height) y1 = target_height - 1;

    if (!s_frame_building || x0 > x1 || y0 > y1) {
        return;
    }

    s_scissor_x0 = (uint32_t)x0;
    s_scissor_x1 = (uint32_t)x1;
    s_scissor_y0 = (uint32_t)y0;
    s_scissor_y1 = (uint32_t)y1;
    ps2GsCoreEmitScissor();
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
        s_gs->PABE = 0;
        ps2GsCoreSetAlphaBlendEquation(
            PS2_GS_ALPHA_BLEND_SOURCE_OVER);
    }
}

extern "C" void ps2GsCoreSetAlphaBlendEquation(
    enum Ps2GsAlphaBlendEquation equation)
{
    if (!s_gs) {
        return;
    }

    struct Ps2GsAlphaBlendFactors factors;
    if (!ps2GsDescribeAlphaBlendEquation(equation, &factors)) {
        return;
    }

    s_gs->PrimAlpha = GS_SETREG_ALPHA(
        factors.a, factors.b, factors.c, factors.d, factors.fix);
    if (s_frame_building && s_gs->PrimAlphaEnable) {
        ps2GsCoreEmitAlpha();
    }
}

extern "C" void ps2GsCoreSetAlphaWrite(bool enable)
{
    if (!s_gs || s_alpha_write == enable) {
        return;
    }

    s_alpha_write = enable;
    if (s_frame_building) {
        ps2GsCoreEmitFrameMask();
    }
}

extern "C" void ps2GsCoreSetColorWrite(bool enable)
{
    if (!s_gs || s_color_write == enable) {
        return;
    }

    s_color_write = enable;
    if (s_frame_building) {
        ps2GsCoreEmitFrameMask();
    }
}

extern "C" void ps2GsCoreSetAlphaTest(bool enable, uint8_t reference)
{
    ps2GsCoreSetAlphaTestComparison(
        enable, reference, PS2_GS_ALPHA_TEST_GEQUAL);
}

extern "C" void ps2GsCoreSetAlphaTestComparison(
    bool enable, uint8_t reference,
    enum Ps2GsAlphaTestComparison comparison)
{
    if (!s_gs) {
        return;
    }

    s_gs->Test->ATE = enable ? GS_SETTING_ON : GS_SETTING_OFF;
    s_gs->Test->ATST = !enable ? PS2_GS_ATST_ALWAYS :
        (comparison == PS2_GS_ALPHA_TEST_LEQUAL ?
            PS2_GS_ATST_LEQUAL : PS2_GS_ATST_GEQUAL);
    s_gs->Test->AREF = reference;
    s_gs->Test->AFAIL = PS2_GS_AFAIL_KEEP;

    if (s_frame_building) {
        ps2GsCoreEmitTest();
    }
}

extern "C" void ps2GsCoreSetFramebufferAlphaForce(bool enable)
{
    if (!s_gs || s_framebuffer_alpha_force == enable) {
        return;
    }

    s_framebuffer_alpha_force = enable;
    if (s_frame_building) {
        ps2GsCoreEmitFramebufferAlphaForce();
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
    ps2GsCoreSetTextureRegionClamp(
        cms, cmt, false, 0u, false, 0u);
}

extern "C" void ps2GsCoreSetTextureRegionClamp(
    uint32_t cms, uint32_t cmt,
    bool region_s, uint16_t max_u, bool region_t, uint16_t max_v)
{
    if (!s_gs) {
        return;
    }

    /* N64 G_TX_CLAMP is bit 1; mirror uses reflected texture residency. */
    s_gs->Clamp->WMS = region_s ? GS_CMODE_REGION_CLAMP :
        ((cms & 2u) ? GS_CMODE_CLAMP : GS_CMODE_REPEAT);
    s_gs->Clamp->WMT = region_t ? GS_CMODE_REGION_CLAMP :
        ((cmt & 2u) ? GS_CMODE_CLAMP : GS_CMODE_REPEAT);
    s_gs->Clamp->MINU = 0u;
    s_gs->Clamp->MAXU = region_s ? max_u : 0u;
    s_gs->Clamp->MINV = 0u;
    s_gs->Clamp->MAXV = region_t ? max_v : 0u;
    if (s_frame_building) {
        ps2GsCoreEmitClamp();
    }
}

extern "C" Ps2GsRenderTargetHandle ps2GsCoreCreateRenderTarget(
    uint32_t width, uint32_t height)
{
    if (!s_gs) {
        return PS2_GS_RENDER_TARGET_DEFAULT;
    }

    struct Ps2GsRenderTargetLayout layout;
    if (!ps2GsDescribeCt32RenderTarget(width, height, &layout)) {
        return PS2_GS_RENDER_TARGET_DEFAULT;
    }

    uint32_t slot_index = PS2_GS_MAX_RENDER_TARGETS;
    for (uint32_t i = 0; i < PS2_GS_MAX_RENDER_TARGETS; ++i) {
        if (!s_render_targets[i].used) {
            slot_index = i;
            break;
        }
    }
    if (slot_index == PS2_GS_MAX_RENDER_TARGETS) {
        sysLogPrintf(LOG_ERROR,
            "GS core: render-target table exhausted (%u)",
            (unsigned int)PS2_GS_MAX_RENDER_TARGETS);
        ps2LogCheckpoint();
        return PS2_GS_RENDER_TARGET_DEFAULT;
    }

    uint32_t vram = 0u;
    if (!ps2GsVramAllocatorAllocAligned(&s_vram_allocator,
            layout.bytes, PS2_GS_FRAMEBUFFER_ALIGNMENT, &vram)) {
        if (!ps2GsCoreDrainAndFence() ||
            !ps2GsVramAllocatorAllocAligned(&s_vram_allocator,
                layout.bytes, PS2_GS_FRAMEBUFFER_ALIGNMENT, &vram)) {
            struct Ps2GsVramStats stats;
            ps2GsVramAllocatorGetStats(&s_vram_allocator, &stats);
            sysLogPrintf(LOG_ERROR,
                "GS core: CT32 render-target allocation failed size=%u "
                "(%ux%u) free=%u largest=%u",
                layout.bytes, width, height,
                stats.free_bytes, stats.largest_free_bytes);
            ps2LogCheckpoint();
            return PS2_GS_RENDER_TARGET_DEFAULT;
        }
    }

    struct Ps2GsRenderTargetSlot *slot = &s_render_targets[slot_index];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->vram = vram;
    slot->layout = layout;
    sysLogPrintf(LOG_NOTE,
        "GS core: CT32 render target id=%u vram=%08x size=%u (%ux%u FBW=%u)",
        (unsigned int)(slot_index + 1u), vram, layout.bytes,
        width, height, layout.fbw);
    ps2LogCheckpoint();
    return (Ps2GsRenderTargetHandle)(slot_index + 1u);
}

extern "C" bool ps2GsCoreBindRenderTarget(
    Ps2GsRenderTargetHandle handle)
{
    if (!s_gs || !ps2GsCoreRenderTargetSlot(handle)) {
        return false;
    }

    s_active_render_target = handle;
    ps2GsCoreEmitRenderTargetState();
    return true;
}

extern "C" void ps2GsCoreBindDefaultRenderTarget(void)
{
    if (!s_gs) {
        return;
    }

    s_active_render_target = PS2_GS_RENDER_TARGET_DEFAULT;
    ps2GsCoreEmitRenderTargetState();
}

extern "C" bool ps2GsCoreReleaseRenderTarget(
    Ps2GsRenderTargetHandle handle)
{
    struct Ps2GsRenderTargetSlot *slot = ps2GsCoreRenderTargetSlot(handle);
    if (!slot || handle == s_active_render_target) {
        return false;
    }
    if (!ps2GsCoreEnsureRetireCapacity(1u)) {
        sysLogPrintf(LOG_ERROR,
            "GS core: render-target retirement fence failed id=%u",
            (unsigned int)handle);
        ps2LogCheckpoint();
        return false;
    }

    ps2GsCoreRetireVram(slot->vram, slot->layout.bytes);
    memset(slot, 0, sizeof(*slot));
    return true;
}

extern "C" uint32_t ps2GsCoreGetRenderTargetWidth(
    Ps2GsRenderTargetHandle handle)
{
    if (handle == PS2_GS_RENDER_TARGET_DEFAULT) {
        return s_gs ? (uint32_t)s_gs->Width : 0u;
    }
    struct Ps2GsRenderTargetSlot *slot = ps2GsCoreRenderTargetSlot(handle);
    return slot ? slot->layout.width : 0u;
}

extern "C" uint32_t ps2GsCoreGetRenderTargetHeight(
    Ps2GsRenderTargetHandle handle)
{
    if (handle == PS2_GS_RENDER_TARGET_DEFAULT) {
        return s_gs ? (uint32_t)s_gs->Height : 0u;
    }
    struct Ps2GsRenderTargetSlot *slot = ps2GsCoreRenderTargetSlot(handle);
    return slot ? slot->layout.height : 0u;
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

static bool ps2GsCoreUploadTexture(Ps2GsTextureHandle handle,
    const uint8_t *source, uint32_t width, uint32_t height, int psm,
    enum Ps2GsNativeUploadEncoding encoding,
    bool mirror_s, bool mirror_t)
{
    if (!s_gs || !source || width == 0 || height == 0 ||
        width > 1024 || height > 1024 ||
        (mirror_s && width > 512u) || (mirror_t && height > 512u) ||
        (psm != GS_PSM_CT32 && psm != GS_PSM_CT16)) {
        return false;
    }

    const uint32_t physical_width = width << (mirror_s ? 1u : 0u);
    const uint32_t physical_height = height << (mirror_t ? 1u : 0u);

    struct Ps2GsTextureSlot *slot = ps2GsCoreTextureSlot(handle);
    if (!slot) {
        return false;
    }

    const u32 bytes = gsKit_texture_size(
        (int)physical_width, (int)physical_height, psm);

    const uint32_t old_block_count = ps2GsCoreResidentBlockCount(slot);
    if (old_block_count != 0u &&
        !ps2GsCoreEnsureRetireCapacity(old_block_count)) {
        sysLogPrintf(LOG_ERROR,
            "GS core: texture retirement fence failed id=%u",
            (unsigned int)handle);
        ps2LogCheckpoint();
        return false;
    }

    uint32_t new_vram = 0;
    bool old_block_reused = false;
    struct Ps2GsVramAllocator allocator_backup = {};

    if (!ps2GsVramAllocatorAlloc(&s_vram_allocator, bytes, &new_vram)) {
        /* Allocation pressure is the second real GS dependency boundary. */
        if (!ps2GsCoreDrainAndFence()) {
            ps2GsCoreLogVramAllocationFailure(handle, bytes, width, height);
            return false;
        }

        if (!ps2GsVramAllocatorAlloc(&s_vram_allocator, bytes, &new_vram)) {
            if (!slot->resident || slot->clut_vram_bytes != 0u) {
                ps2GsCoreLogVramAllocationFailure(handle, bytes, width, height);
                return false;
            }

            /*
             * All previous GS users are fenced. Transactionally release this
             * handle's old block only as a final retry, retaining an allocator
             * snapshot so a failed upload leaves the old texture untouched.
             */
            allocator_backup = s_vram_allocator;
            if (!ps2GsVramAllocatorFree(&s_vram_allocator,
                    slot->texture.Vram, slot->vram_bytes) ||
                !ps2GsVramAllocatorAlloc(
                    &s_vram_allocator, bytes, &new_vram)) {
                s_vram_allocator = allocator_backup;
                ps2GsCoreLogVramAllocationFailure(handle, bytes, width, height);
                return false;
            }
            old_block_reused = true;
        }
    }

    /*
     * Pixel transport is project-owned GIF IMAGE DMA. GSTEXTURE remains a
     * compact register/size description, but gsKit no longer owns allocation,
     * submission or retirement of texture residency.
     */
    GSTEXTURE candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.Width = physical_width;
    candidate.Height = physical_height;
    candidate.PSM = psm;
    candidate.Filter = slot->texture.Filter;
    candidate.Vram = new_vram;
    candidate.Mem = (u32 *)(uintptr_t)source;
    const bool submitted = ps2GsNativeQueueUploadTextureMirrored(
        s_gs, &candidate, encoding, width, height, mirror_s, mirror_t);
    candidate.Mem = NULL;

    if (!submitted) {
        if (old_block_reused) {
            s_vram_allocator = allocator_backup;
        } else if (!ps2GsVramAllocatorFree(
                       &s_vram_allocator, new_vram, bytes)) {
            sysLogPrintf(LOG_ERROR,
                "GS core: failed to roll back VRAM allocation id=%u",
                (unsigned int)handle);
        }
        sysLogPrintf(LOG_ERROR,
            "GS core: native texture upload failed id=%u size=%u (%ux%u)",
            (unsigned int)handle, bytes, width, height);
        ps2LogCheckpoint();
        return false;
    }

    if (slot->resident && !old_block_reused) {
        ps2GsCoreRetireTextureBlocks(slot);
    }
    slot->texture = candidate;
    slot->resident = true;
    slot->uploaded = true;
    slot->vram_bytes = bytes;
    slot->clut_vram_bytes = 0u;
    return true;
}

extern "C" bool ps2GsCoreUploadTextureRgba32(Ps2GsTextureHandle handle,
    const uint8_t *rgba32, uint32_t width, uint32_t height,
    bool mirror_s, bool mirror_t)
{
    return ps2GsCoreUploadTexture(
        handle, rgba32, width, height, GS_PSM_CT32,
        PS2_GS_NATIVE_UPLOAD_RGBA32, mirror_s, mirror_t);
}

extern "C" bool ps2GsCoreUploadTextureN64Rgba16(Ps2GsTextureHandle handle,
    const uint8_t *rgba5551_be, uint32_t width, uint32_t height,
    bool mirror_s, bool mirror_t)
{
    return ps2GsCoreUploadTexture(
        handle, rgba5551_be, width, height, GS_PSM_CT16,
        PS2_GS_NATIVE_UPLOAD_N64_RGBA16,
        mirror_s, mirror_t);
}

extern "C" bool ps2GsCoreUploadTextureN64Ia16(Ps2GsTextureHandle handle,
    const uint8_t *ia16, uint32_t width, uint32_t height,
    bool mirror_s, bool mirror_t)
{
    return ps2GsCoreUploadTexture(
        handle, ia16, width, height, GS_PSM_CT32,
        PS2_GS_NATIVE_UPLOAD_N64_IA16,
        mirror_s, mirror_t);
}

static bool ps2GsCoreAllocIndexedBlocks(uint32_t texture_bytes,
    uint32_t clut_bytes, uint32_t *texture_vram, uint32_t *clut_vram)
{
    if (!ps2GsVramAllocatorAlloc(
            &s_vram_allocator, texture_bytes, texture_vram)) {
        return false;
    }
    if (!ps2GsVramAllocatorAlloc(
            &s_vram_allocator, clut_bytes, clut_vram)) {
        (void)ps2GsVramAllocatorFree(
            &s_vram_allocator, *texture_vram, texture_bytes);
        return false;
    }
    return true;
}

static bool ps2GsCoreUploadSharedCt32Clut(
    struct Ps2GsSharedClut *shared, uint32_t entry_count,
    const char *purpose, uint32_t purpose_id,
    struct Ps2GsSharedClut **result)
{
    if (!shared || !result || !purpose ||
        (entry_count != 16u && entry_count != 256u)) {
        return false;
    }

    const uint32_t clut_width = entry_count == 16u ? 8u : 16u;
    const uint32_t clut_height = entry_count == 16u ? 2u : 16u;
    const uint32_t clut_bytes = gsKit_texture_size(
        (int)clut_width, (int)clut_height, GS_PSM_CT32);

    uint32_t clut_vram = 0u;
    if (!ps2GsVramAllocatorAlloc(
            &s_vram_allocator, clut_bytes, &clut_vram)) {
        if (!ps2GsCoreDrainAndFence() ||
            !ps2GsVramAllocatorAlloc(
                &s_vram_allocator, clut_bytes, &clut_vram)) {
            ps2GsCoreLogVramAllocationFailure(
                PS2_GS_TEXTURE_INVALID, clut_bytes,
                clut_width, clut_height);
            return false;
        }
    }

    GSTEXTURE clut;
    memset(&clut, 0, sizeof(clut));
    clut.Width = clut_width;
    clut.Height = clut_height;
    clut.PSM = GS_PSM_CT32;
    clut.Vram = clut_vram;
    clut.Mem = (u32 *)(uintptr_t)s_shared_clut_staging;
    if (!ps2GsNativeQueueUploadTexture(
            s_gs, &clut, PS2_GS_NATIVE_UPLOAD_RGBA32)) {
        (void)ps2GsVramAllocatorFree(
            &s_vram_allocator, clut_vram, clut_bytes);
        sysLogPrintf(LOG_ERROR,
            "GS core: shared %s CLUT upload failed id=%u",
            purpose, (unsigned int)purpose_id);
        ps2LogCheckpoint();
        return false;
    }

    shared->resident = true;
    shared->vram = clut_vram;
    shared->bytes = clut_bytes;
    ps2GsCoreInvalidateClutCache();
    *result = shared;
    return true;
}

static bool ps2GsCoreEnsureSharedIntensityClut(
    enum Ps2GsN64IntensityEncoding encoding,
    struct Ps2GsSharedClut **result)
{
    const uint32_t index = (uint32_t)encoding;
    if (!result || index >= PS2_GS_ALPHA_IDENTITY_CLUT_INDEX) {
        return false;
    }

    struct Ps2GsSharedClut *shared = &s_shared_cluts[index];
    if (shared->resident) {
        *result = shared;
        return true;
    }

    const bool four_bit = encoding == PS2_GS_N64_IA4 ||
        encoding == PS2_GS_N64_I4;
    const uint32_t entry_count = four_bit ? 16u : 256u;
    if (!ps2GsBuildN64IntensityClut(
            encoding, s_shared_clut_staging, entry_count)) {
        return false;
    }
    return ps2GsCoreUploadSharedCt32Clut(
        shared, entry_count, "intensity", index, result);
}

static bool ps2GsCoreEnsureSharedAlphaIdentityClut(
    struct Ps2GsSharedClut **result)
{
    if (!result) {
        return false;
    }

    struct Ps2GsSharedClut *shared =
        &s_shared_cluts[PS2_GS_ALPHA_IDENTITY_CLUT_INDEX];
    if (shared->resident) {
        *result = shared;
        return true;
    }
    if (!ps2GsBuildIdentityRgba8Clut(
            s_shared_clut_staging, 256u)) {
        return false;
    }
    return ps2GsCoreUploadSharedCt32Clut(
        shared, 256u, "alpha identity",
        PS2_GS_ALPHA_IDENTITY_CLUT_INDEX, result);
}

extern "C" bool ps2GsCoreUploadTextureN64Ci(Ps2GsTextureHandle handle,
    const uint8_t *indices, uint32_t width, uint32_t height,
    uint8_t index_bits, const uint16_t *palette,
    uint32_t palette_count, enum Ps2GsN64PaletteEncoding palette_encoding,
    bool mirror_s, bool mirror_t)
{
    const bool ci4 = index_bits == 4u && palette_count == 16u;
    const bool ci8 = index_bits == 8u && palette_count == 256u;
    if (!s_gs || !indices || !palette || width == 0u ||
        height == 0u || width > 1024u || height > 1024u ||
        (mirror_s && width > 512u) || (mirror_t && height > 512u) ||
        (!ci4 && !ci8) || (ci4 && (width & 1u) != 0u) ||
        (palette_encoding != PS2_GS_N64_PALETTE_RGBA16 &&
         palette_encoding != PS2_GS_N64_PALETTE_IA16)) {
        return false;
    }

    const uint32_t physical_width = width << (mirror_s ? 1u : 0u);
    const uint32_t physical_height = height << (mirror_t ? 1u : 0u);

    struct Ps2GsTextureSlot *slot = ps2GsCoreTextureSlot(handle);
    if (!slot) {
        return false;
    }

    const int texture_psm = ci4 ? GS_PSM_T4 : GS_PSM_T8;
    const uint32_t clut_width = ci4 ? 8u : 16u;
    const uint32_t clut_height = ci4 ? 2u : 16u;
    const int clut_psm =
        palette_encoding == PS2_GS_N64_PALETTE_RGBA16
            ? GS_PSM_CT16 : GS_PSM_CT32;
    const uint32_t texture_bytes =
        gsKit_texture_size(
            (int)physical_width, (int)physical_height, texture_psm);
    const uint32_t clut_bytes = gsKit_texture_size(
        (int)clut_width, (int)clut_height, clut_psm);
    /*
     * Two retirement records cover either the old residency on success or
     * the new texture/CLUT pair if the second upload cannot be fenced safely.
     */
    if (!ps2GsCoreEnsureRetireCapacity(2u)) {
        sysLogPrintf(LOG_ERROR,
            "GS core: indexed texture retirement fence failed id=%u",
            (unsigned int)handle);
        ps2LogCheckpoint();
        return false;
    }

    uint32_t texture_vram = 0u;
    uint32_t clut_vram = 0u;
    if (!ps2GsCoreAllocIndexedBlocks(texture_bytes, clut_bytes,
            &texture_vram, &clut_vram)) {
        if (!ps2GsCoreDrainAndFence() ||
            !ps2GsCoreAllocIndexedBlocks(texture_bytes, clut_bytes,
                &texture_vram, &clut_vram)) {
            ps2GsCoreLogVramAllocationFailure(
                handle, texture_bytes + clut_bytes, width, height);
            return false;
        }
    }

    GSTEXTURE candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.Width = physical_width;
    candidate.Height = physical_height;
    candidate.PSM = texture_psm;
    candidate.Filter = slot->texture.Filter;
    candidate.Vram = texture_vram;
    candidate.VramClut = clut_vram;
    candidate.ClutPSM = clut_psm;
    candidate.ClutStorageMode = 0; /* CSM1 */
    candidate.Mem = (u32 *)(uintptr_t)indices;

    const enum Ps2GsNativeUploadEncoding texture_encoding = ci4
        ? PS2_GS_NATIVE_UPLOAD_N64_T4
        : PS2_GS_NATIVE_UPLOAD_T8;
    const bool texture_submitted = ps2GsNativeQueueUploadTextureMirrored(
        s_gs, &candidate, texture_encoding,
        width, height, mirror_s, mirror_t);
    candidate.Mem = NULL;

    GSTEXTURE clut;
    memset(&clut, 0, sizeof(clut));
    clut.Width = clut_width;
    clut.Height = clut_height;
    clut.PSM = clut_psm;
    clut.Vram = clut_vram;
    clut.Mem = (u32 *)(uintptr_t)palette;
    const enum Ps2GsNativeUploadEncoding clut_encoding =
        palette_encoding == PS2_GS_N64_PALETTE_RGBA16
            ? PS2_GS_NATIVE_UPLOAD_N64_RGBA16_CLUT
            : PS2_GS_NATIVE_UPLOAD_N64_IA16_CLUT;
    const bool clut_submitted = texture_submitted &&
        ps2GsNativeQueueUploadTexture(s_gs, &clut,
            clut_encoding);

    if (!clut_submitted) {
        if (texture_submitted) {
            /* The first DMA may own the new block; fence before rollback. */
            if (!ps2GsCoreDrainAndFence()) {
                ps2GsCoreRetireVram(texture_vram, texture_bytes);
                ps2GsCoreRetireVram(clut_vram, clut_bytes);
                sysLogPrintf(LOG_ERROR,
                    "GS core: indexed rollback deferred after fence failure id=%u",
                    (unsigned int)handle);
                ps2LogCheckpoint();
                return false;
            }
        }
        (void)ps2GsVramAllocatorFree(
            &s_vram_allocator, texture_vram, texture_bytes);
        (void)ps2GsVramAllocatorFree(
            &s_vram_allocator, clut_vram, clut_bytes);
        sysLogPrintf(LOG_ERROR,
            "GS core: indexed texture upload failed id=%u ci%u (%ux%u)",
            (unsigned int)handle, (unsigned int)index_bits, width, height);
        ps2LogCheckpoint();
        return false;
    }

    if (slot->resident) {
        ps2GsCoreRetireTextureBlocks(slot);
    }
    /* A newly uploaded palette may reuse an address seen by the GS cache. */
    ps2GsCoreInvalidateClutCache();
    slot->texture = candidate;
    slot->resident = true;
    slot->uploaded = true;
    slot->vram_bytes = texture_bytes;
    slot->clut_vram_bytes = clut_bytes;
    return true;
}

extern "C" bool ps2GsCoreUploadTextureN64Intensity(
    Ps2GsTextureHandle handle, const uint8_t *texels,
    uint32_t width, uint32_t height,
    enum Ps2GsN64IntensityEncoding encoding,
    bool mirror_s, bool mirror_t)
{
    const bool four_bit = encoding == PS2_GS_N64_IA4 ||
        encoding == PS2_GS_N64_I4;
    const bool eight_bit = encoding == PS2_GS_N64_IA8 ||
        encoding == PS2_GS_N64_I8;
    if (!s_gs || !texels || width == 0u || height == 0u ||
        width > 1024u || height > 1024u ||
        (mirror_s && width > 512u) || (mirror_t && height > 512u) ||
        (!four_bit && !eight_bit) ||
        (four_bit && (width & 1u) != 0u)) {
        return false;
    }

    const uint32_t physical_width = width << (mirror_s ? 1u : 0u);
    const uint32_t physical_height = height << (mirror_t ? 1u : 0u);

    struct Ps2GsTextureSlot *slot = ps2GsCoreTextureSlot(handle);
    struct Ps2GsSharedClut *shared = NULL;
    if (!slot || !ps2GsCoreEnsureSharedIntensityClut(
            encoding, &shared)) {
        return false;
    }

    const int texture_psm = four_bit ? GS_PSM_T4 : GS_PSM_T8;
    const uint32_t texture_bytes =
        gsKit_texture_size(
            (int)physical_width, (int)physical_height, texture_psm);
    const uint32_t old_block_count = ps2GsCoreResidentBlockCount(slot);
    if (old_block_count != 0u &&
        !ps2GsCoreEnsureRetireCapacity(old_block_count)) {
        sysLogPrintf(LOG_ERROR,
            "GS core: intensity texture retirement fence failed id=%u",
            (unsigned int)handle);
        ps2LogCheckpoint();
        return false;
    }

    uint32_t texture_vram = 0u;
    bool old_block_reused = false;
    struct Ps2GsVramAllocator allocator_backup = {};
    if (!ps2GsVramAllocatorAlloc(
            &s_vram_allocator, texture_bytes, &texture_vram)) {
        if (!ps2GsCoreDrainAndFence()) {
            ps2GsCoreLogVramAllocationFailure(
                handle, texture_bytes, width, height);
            return false;
        }
        if (!ps2GsVramAllocatorAlloc(
                &s_vram_allocator, texture_bytes, &texture_vram)) {
            if (!slot->resident || slot->clut_vram_bytes != 0u) {
                ps2GsCoreLogVramAllocationFailure(
                    handle, texture_bytes, width, height);
                return false;
            }
            allocator_backup = s_vram_allocator;
            if (!ps2GsVramAllocatorFree(&s_vram_allocator,
                    slot->texture.Vram, slot->vram_bytes) ||
                !ps2GsVramAllocatorAlloc(&s_vram_allocator,
                    texture_bytes, &texture_vram)) {
                s_vram_allocator = allocator_backup;
                ps2GsCoreLogVramAllocationFailure(
                    handle, texture_bytes, width, height);
                return false;
            }
            old_block_reused = true;
        }
    }

    GSTEXTURE candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.Width = physical_width;
    candidate.Height = physical_height;
    candidate.PSM = texture_psm;
    candidate.Filter = slot->texture.Filter;
    candidate.Vram = texture_vram;
    candidate.VramClut = shared->vram;
    candidate.ClutPSM = GS_PSM_CT32;
    candidate.ClutStorageMode = 0; /* CSM1 */
    candidate.Mem = (u32 *)(uintptr_t)texels;
    const enum Ps2GsNativeUploadEncoding upload_encoding = four_bit
        ? PS2_GS_NATIVE_UPLOAD_N64_T4
        : PS2_GS_NATIVE_UPLOAD_T8;
    const bool submitted = ps2GsNativeQueueUploadTextureMirrored(
        s_gs, &candidate, upload_encoding,
        width, height, mirror_s, mirror_t);
    candidate.Mem = NULL;
    if (!submitted) {
        if (old_block_reused) {
            s_vram_allocator = allocator_backup;
        } else {
            (void)ps2GsVramAllocatorFree(
                &s_vram_allocator, texture_vram, texture_bytes);
        }
        sysLogPrintf(LOG_ERROR,
            "GS core: native intensity upload failed id=%u format=%u (%ux%u)",
            (unsigned int)handle, (unsigned int)encoding, width, height);
        ps2LogCheckpoint();
        return false;
    }

    if (slot->resident && !old_block_reused) {
        ps2GsCoreRetireTextureBlocks(slot);
    }
    slot->texture = candidate;
    slot->resident = true;
    slot->uploaded = true;
    slot->vram_bytes = texture_bytes;
    slot->clut_vram_bytes = 0u;
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
        if (slot->resident && slot->clut_vram_bytes != 0u &&
            s_loaded_clut_vram == slot->texture.VramClut &&
            s_loaded_clut_psm == slot->texture.ClutPSM) {
            ps2GsCoreInvalidateClutCache();
        }
        if (slot->resident) {
            const uint32_t block_count = ps2GsCoreResidentBlockCount(slot);
            if (!ps2GsCoreEnsureRetireCapacity(block_count)) {
                sysLogPrintf(LOG_ERROR,
                    "GS core: texture release fence failed id=%u",
                    (unsigned int)handle);
                ps2LogCheckpoint();
                return;
            }
            ps2GsCoreRetireTextureBlocks(slot);
        }
        memset(slot, 0, sizeof(*slot));
    }
}

extern "C" void ps2GsCoreDrawColorTriangles(const struct Ps2GsColorVertex *vertices,
    uint32_t vertex_count)
{
    if (!s_gs || !s_frame_building || !vertices || vertex_count == 0) {
        return;
    }

    const uint64_t prim = GS_SETREG_PRIM(
        GS_PRIM_PRIM_TRIANGLE,
        1,
        0,
        s_gs->PrimFogEnable,
        s_gs->PrimAlphaEnable,
        s_gs->PrimAAEnable,
        0,
        s_gs->PrimContext,
        0);
    const bool emit_prim = ps2GsStateShadowNeedsWrite(
        &s_state_shadow, PS2_GS_STATE_PRIM, prim);
    const uint32_t register_count =
        (emit_prim ? 1u : 0u) + vertex_count * 2u;
    struct Ps2GsPackedReg *p = ps2GsCoreReserve(register_count);
    if (!p) {
        return;
    }

    uint32_t out = 0u;
    if (emit_prim) {
        ps2GsCoreWriteReg(&p[out++], prim, GS_PRIM);
        ps2GsStateShadowCommit(
            &s_state_shadow, PS2_GS_STATE_PRIM, prim);
    }
    memcpy(&p[out], vertices, (size_t)vertex_count * sizeof(*vertices));
    ps2GsCoreMarkActiveRenderTargetWritten();
}

static bool ps2GsCoreDrawTexturedTrianglesInternal(GSTEXTURE *tex,
    const struct Ps2GsTexturedVertex *vertices, uint32_t vertex_count,
    bool texture_flush, const struct Ps2GsRenderTargetTextureView *target_view)
{
    if (!tex || !vertices || vertex_count == 0u) {
        return false;
    }

    const bool indexed = tex->PSM == GS_PSM_T4 ||
        tex->PSM == GS_PSM_T8 || tex->PSM == GS_PSM_T8H;
    const bool load_clut = indexed &&
        (s_loaded_clut_vram != tex->VramClut ||
         s_loaded_clut_psm != tex->ClutPSM);
    const int tw = target_view ? target_view->tw :
        ps2GsCoreTextureExponent(tex->Width);
    const int th = target_view ? target_view->th :
        ps2GsCoreTextureExponent(tex->Height);
    const uint64_t tex1 = GS_SETREG_TEX1(
        0, 0, tex->Filter, tex->Filter, 0, 0, 0);
    const uint64_t tex0 = GS_SETREG_TEX0(
        tex->Vram / 256,
        tex->TBW,
        tex->PSM,
        tw,
        th,
        s_texture_alpha ? 1 : 0,
        0,
        indexed ? tex->VramClut / 256u : 0u,
        indexed ? tex->ClutPSM : 0u,
        indexed ? tex->ClutStorageMode : 0u,
        0,
        load_clut ? GS_CLUT_STOREMODE_LOAD : GS_CLUT_STOREMODE_NOLOAD);
    const uint64_t prim = GS_SETREG_PRIM(
        GS_PRIM_PRIM_TRIANGLE,
        1,
        1,
        s_gs->PrimFogEnable,
        s_gs->PrimAlphaEnable,
        s_gs->PrimAAEnable,
        0,
        s_gs->PrimContext,
        0);
    const bool emit_tex1 = ps2GsStateShadowNeedsWrite(
        &s_state_shadow, PS2_GS_STATE_TEX1, tex1);
    const bool emit_tex0 = ps2GsStateShadowNeedsWrite(
        &s_state_shadow, PS2_GS_STATE_TEX0, tex0);
    const bool emit_prim = ps2GsStateShadowNeedsWrite(
        &s_state_shadow, PS2_GS_STATE_PRIM, prim);
    const uint32_t state_register_count =
        (emit_tex1 ? 1u : 0u) +
        (emit_tex0 ? 1u : 0u) +
        (emit_prim ? 1u : 0u);
    const uint32_t register_count = state_register_count + vertex_count * 3u +
        (texture_flush ? 1u : 0u) + (target_view ? 2u : 0u);
    struct Ps2GsPackedReg *p = ps2GsCoreReserve(register_count);
    if (!p) {
        return false;
    }

    uint32_t out = 0u;
    if (texture_flush) {
        ps2GsCoreWriteReg(&p[out++], 0u, GS_TEXFLUSH);
    }
    if (emit_tex1) {
        ps2GsCoreWriteReg(
            &p[out++], tex1, GS_TEX1_1 + s_gs->PrimContext);
        ps2GsStateShadowCommit(
            &s_state_shadow, PS2_GS_STATE_TEX1, tex1);
    }
    if (emit_tex0) {
        ps2GsCoreWriteReg(
            &p[out++], tex0, GS_TEX0_1 + s_gs->PrimContext);
        ps2GsStateShadowCommit(
            &s_state_shadow, PS2_GS_STATE_TEX0, tex0);
    }
    if (target_view) {
        ps2GsCoreWriteReg(&p[out++],
            GS_SETREG_CLAMP(
                GS_CMODE_REGION_CLAMP,
                GS_CMODE_REGION_CLAMP,
                0u,
                target_view->clamp_max_u,
                0u,
                target_view->clamp_max_v),
            GS_CLAMP_1 + s_gs->PrimContext);
    }
    if (emit_prim) {
        ps2GsCoreWriteReg(&p[out++], prim, GS_PRIM);
        ps2GsStateShadowCommit(
            &s_state_shadow, PS2_GS_STATE_PRIM, prim);
    }

    memcpy(&p[out], vertices, (size_t)vertex_count * sizeof(*vertices));
    out += vertex_count * 3u;
    if (target_view) {
        ps2GsCoreWriteReg(&p[out++],
            GS_SETREG_CLAMP(
                s_gs->Clamp->WMS,
                s_gs->Clamp->WMT,
                s_gs->Clamp->MINU,
                s_gs->Clamp->MAXU,
                s_gs->Clamp->MINV,
                s_gs->Clamp->MAXV),
            GS_CLAMP_1 + s_gs->PrimContext);
    }
    if (indexed) {
        s_loaded_clut_vram = tex->VramClut;
        s_loaded_clut_psm = tex->ClutPSM;
    }
    ps2GsCoreMarkActiveRenderTargetWritten();
    return true;
}

extern "C" void ps2GsCoreDrawTexturedTriangles(Ps2GsTextureHandle handle,
    const struct Ps2GsTexturedVertex *vertices, uint32_t vertex_count)
{
    if (!s_gs || !s_frame_building || !vertices || vertex_count == 0u) {
        return;
    }

    struct Ps2GsTextureSlot *slot = ps2GsCoreTextureSlot(handle);
    if (!slot || !slot->uploaded) {
        return;
    }

    ps2GsCoreDrawTexturedTrianglesInternal(
        &slot->texture, vertices, vertex_count, false, NULL);
}

static bool ps2GsCoreDrawRenderTargetView(
    Ps2GsRenderTargetHandle source,
    const struct Ps2GsTexturedVertex *vertices, uint32_t vertex_count,
    bool linear_filter, int psm)
{
    if (!s_gs || !s_frame_building || !vertices || vertex_count == 0u ||
        source == s_active_render_target ||
        (psm != GS_PSM_CT32 && psm != GS_PSM_T8H)) {
        return false;
    }

    struct Ps2GsRenderTargetSlot *slot = ps2GsCoreRenderTargetSlot(source);
    struct Ps2GsRenderTargetTextureView view;
    if (!slot || !slot->has_contents ||
        !ps2GsDescribeCt32RenderTargetTextureView(
            &slot->layout, &view)) {
        return false;
    }

    struct Ps2GsSharedClut *shared = NULL;
    if (psm == GS_PSM_T8H &&
        !ps2GsCoreEnsureSharedAlphaIdentityClut(&shared)) {
        return false;
    }

    GSTEXTURE texture;
    memset(&texture, 0, sizeof(texture));
    texture.Width = (int)slot->layout.width;
    texture.Height = (int)slot->layout.height;
    texture.PSM = psm;
    texture.Vram = slot->vram;
    texture.TBW = (int)view.tbw;
    texture.Filter = linear_filter ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
    if (shared) {
        texture.VramClut = shared->vram;
        texture.ClutPSM = GS_PSM_CT32;
        texture.ClutStorageMode = 0; /* CSM1 */
    }

    if (!ps2GsCoreDrawTexturedTrianglesInternal(
            &texture, vertices, vertex_count,
            slot->texture_cache_dirty, &view)) {
        return false;
    }

    slot->texture_cache_dirty = false;
    return true;
}

extern "C" bool ps2GsCoreDrawRenderTargetTriangles(
    Ps2GsRenderTargetHandle source,
    const struct Ps2GsTexturedVertex *vertices, uint32_t vertex_count,
    bool linear_filter)
{
    return ps2GsCoreDrawRenderTargetView(
        source, vertices, vertex_count, linear_filter, GS_PSM_CT32);
}

extern "C" bool ps2GsCoreDrawRenderTargetAlphaTriangles(
    Ps2GsRenderTargetHandle source,
    const struct Ps2GsTexturedVertex *vertices, uint32_t vertex_count,
    bool linear_filter)
{
    return ps2GsCoreDrawRenderTargetView(
        source, vertices, vertex_count, linear_filter, GS_PSM_T8H);
}

static bool ps2GsCoreRestoreChannelBlitState(
    uint32_t scissor_x0, uint32_t scissor_x1,
    uint32_t scissor_y0, uint32_t scissor_y1)
{
    s_scissor_x0 = scissor_x0;
    s_scissor_x1 = scissor_x1;
    s_scissor_y0 = scissor_y0;
    s_scissor_y1 = scissor_y1;

    const uint64_t frame = GS_SETREG_FRAME_1(
        ps2GsCoreTargetVram() / PS2_GS_FRAMEBUFFER_ALIGNMENT,
        ps2GsCoreTargetFbw(),
        ps2GsCoreTargetPsm(),
        ps2GsCoreFrameMask());
    const uint64_t scissor = GS_SETREG_SCISSOR(
        s_scissor_x0, s_scissor_x1,
        s_scissor_y0, s_scissor_y1);
    const uint64_t test = ps2GsCoreCurrentTestValue(-1);
    const uint64_t clamp = GS_SETREG_CLAMP(
        s_gs->Clamp->WMS,
        s_gs->Clamp->WMT,
        s_gs->Clamp->MINU,
        s_gs->Clamp->MAXU,
        s_gs->Clamp->MINV,
        s_gs->Clamp->MAXV);
    struct Ps2GsPackedReg *p = ps2GsCoreReserveChunk(4u);
    if (!p) {
        ps2GsStateShadowInvalidate(
            &s_state_shadow, PS2_GS_STATE_FRAME);
        ps2GsStateShadowInvalidate(
            &s_state_shadow, PS2_GS_STATE_SCISSOR);
        ps2GsStateShadowInvalidate(
            &s_state_shadow, PS2_GS_STATE_TEST);
        ps2GsStateShadowInvalidate(
            &s_state_shadow, PS2_GS_STATE_CLAMP);
        return false;
    }
    ps2GsCoreWriteReg(
        &p[0], frame, GS_FRAME_1 + s_gs->PrimContext);
    ps2GsCoreWriteReg(
        &p[1], scissor, GS_SCISSOR_1 + s_gs->PrimContext);
    ps2GsCoreWriteReg(
        &p[2], test, GS_TEST_1 + s_gs->PrimContext);
    ps2GsCoreWriteReg(
        &p[3], clamp, GS_CLAMP_1 + s_gs->PrimContext);
    ps2GsStateShadowCommit(
        &s_state_shadow, PS2_GS_STATE_FRAME, frame);
    ps2GsStateShadowCommit(
        &s_state_shadow, PS2_GS_STATE_SCISSOR, scissor);
    ps2GsStateShadowCommit(
        &s_state_shadow, PS2_GS_STATE_TEST, test);
    ps2GsStateShadowCommit(
        &s_state_shadow, PS2_GS_STATE_CLAMP, clamp);
    return true;
}

extern "C" bool ps2GsCoreBlitRenderTargetRedToActiveAlpha(
    Ps2GsRenderTargetHandle source)
{
    if (!s_gs || !s_frame_building ||
        s_active_render_target == PS2_GS_RENDER_TARGET_DEFAULT ||
        source == s_active_render_target) {
        return false;
    }

    struct Ps2GsRenderTargetSlot *source_slot =
        ps2GsCoreRenderTargetSlot(source);
    struct Ps2GsRenderTargetSlot *destination_slot =
        ps2GsCoreRenderTargetSlot(s_active_render_target);
    if (!source_slot || !destination_slot ||
        !source_slot->has_contents || !destination_slot->has_contents ||
        source_slot->layout.width != destination_slot->layout.width ||
        source_slot->layout.height != destination_slot->layout.height ||
        source_slot->layout.fbw != destination_slot->layout.fbw) {
        return false;
    }

    struct Ps2GsSharedClut *shared = NULL;
    if (!ps2GsCoreEnsureSharedAlphaIdentityClut(&shared)) {
        return false;
    }

    const uint32_t saved_scissor_x0 = s_scissor_x0;
    const uint32_t saved_scissor_x1 = s_scissor_x1;
    const uint32_t saved_scissor_y0 = s_scissor_y0;
    const uint32_t saved_scissor_y1 = s_scissor_y1;
    s_scissor_x0 = 0u;
    s_scissor_x1 = destination_slot->layout.width - 1u;
    s_scissor_y0 = 0u;
    s_scissor_y1 = destination_slot->layout.height - 1u;

    const bool texture_flush = source_slot->texture_cache_dirty;
    const uint32_t setup_register_count = texture_flush ? 7u : 6u;
    struct Ps2GsPackedReg *setup =
        ps2GsCoreReserveChunk(setup_register_count);
    if (!setup) {
        s_scissor_x0 = saved_scissor_x0;
        s_scissor_x1 = saved_scissor_x1;
        s_scissor_y0 = saved_scissor_y0;
        s_scissor_y1 = saved_scissor_y1;
        return false;
    }

    uint32_t setup_out = 0u;
    if (texture_flush) {
        ps2GsCoreWriteReg(&setup[setup_out++], 0u, GS_TEXFLUSH);
    }
    ps2GsCoreWriteReg(&setup[setup_out++],
        GS_SETREG_TEX1(0, 0, GS_FILTER_NEAREST, GS_FILTER_NEAREST,
            0, 0, 0),
        GS_TEX1_1 + s_gs->PrimContext);
    ps2GsCoreWriteReg(&setup[setup_out++],
        GS_SETREG_FRAME_1(
            destination_slot->vram / PS2_GS_FRAMEBUFFER_ALIGNMENT,
            destination_slot->layout.fbw,
            GS_PSM_CT32,
            0x00ffffffu),
        GS_FRAME_1 + s_gs->PrimContext);
    ps2GsCoreWriteReg(&setup[setup_out++],
        GS_SETREG_SCISSOR(
            s_scissor_x0, s_scissor_x1,
            s_scissor_y0, s_scissor_y1),
        GS_SCISSOR_1 + s_gs->PrimContext);
    ps2GsCoreWriteReg(&setup[setup_out++],
        GS_SETREG_TEST(
            0, PS2_GS_ATST_ALWAYS, 0, PS2_GS_AFAIL_KEEP,
            0, 0, 0, 1),
        GS_TEST_1 + s_gs->PrimContext);
    ps2GsCoreWriteReg(&setup[setup_out++],
        GS_SETREG_PRIM(
            GS_PRIM_PRIM_SPRITE,
            0,
            1,
            0,
            0,
            0,
            1,
            s_gs->PrimContext,
            0),
        GS_PRIM);
    ps2GsCoreWriteReg(&setup[setup_out++],
        GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0),
        GS_RGBAQ);
    source_slot->texture_cache_dirty = false;

    bool load_clut = s_loaded_clut_vram != shared->vram ||
        s_loaded_clut_psm != GS_PSM_CT32;
    const uint32_t width = source_slot->layout.width;
    const uint32_t height = source_slot->layout.height;
    const uint32_t page_rows = (height + 31u) / 32u;
    bool success = true;

    for (uint32_t page_y = 0u; page_y < page_rows && success; ++page_y) {
        const uint32_t destination_y = page_y * 32u;
        const uint32_t page_height = height - destination_y < 32u ?
            height - destination_y : 32u;

        for (uint32_t page_x = 0u;
             page_x < source_slot->layout.fbw && success; ++page_x) {
            const uint32_t destination_x = page_x * 64u;
            const uint32_t page_width = width - destination_x < 64u ?
                width - destination_x : 64u;
            const uint32_t tile_count =
                ((page_width + 7u) / 8u) *
                ((page_height + 1u) / 2u);
            const uint32_t register_count = 1u + tile_count * 5u;
            struct Ps2GsPackedReg *p =
                tile_count != 0u ?
                ps2GsCoreReserveChunk(register_count) : NULL;
            if (!p) {
                success = false;
                break;
            }

            const uint32_t page_index =
                page_y * source_slot->layout.fbw + page_x;
            uint32_t out = 0u;
            ps2GsCoreWriteReg(&p[out++],
                GS_SETREG_TEX0(
                    (source_slot->vram +
                        page_index * PS2_GS_FRAMEBUFFER_ALIGNMENT) / 256u,
                    2u,
                    GS_PSM_T8,
                    7u,
                    6u,
                    1u,
                    0u,
                    shared->vram / 256u,
                    GS_PSM_CT32,
                    0u,
                    0u,
                    load_clut ? GS_CLUT_STOREMODE_LOAD :
                        GS_CLUT_STOREMODE_NOLOAD),
                GS_TEX0_1 + s_gs->PrimContext);

            for (uint32_t y = 0u; y < page_height; y += 2u) {
                const uint32_t tile_height =
                    page_height - y < 2u ? page_height - y : 2u;
                for (uint32_t tile_x = 0u;
                     tile_x < page_width; tile_x += 8u) {
                    const uint32_t tile_width =
                        page_width - tile_x < 8u ?
                        page_width - tile_x : 8u;
                    struct Ps2GsT8PageCoordinate first = {};
                    (void)ps2GsMapCt32PixelChannelToT8Page(
                        tile_x, y, PS2_GS_CT32_CHANNEL_RED, &first);
                    const uint32_t raw_tile_x = tile_x * 2u;
                    const uint32_t u_xor = first.u - raw_tile_x;
                    const uint32_t x0 = destination_x + tile_x;
                    const uint32_t y0 = destination_y + y;

                    ps2GsCoreWriteReg(&p[out++],
                        GS_SETREG_CLAMP(
                            GS_CMODE_REGION_REPEAT,
                            GS_CMODE_REGION_CLAMP,
                            7u,
                            raw_tile_x,
                            first.v,
                            first.v + tile_height - 1u),
                        GS_CLAMP_1 + s_gs->PrimContext);
                    ps2GsCoreWriteReg(&p[out++],
                        GS_SETREG_UV(u_xor * 16u, first.v * 16u),
                        GS_UV);
                    /*
                     * GS sprite coverage is pixel-perfect when the XY
                     * endpoints name the upper-left pixel corners. Keep UV
                     * on exact texel boundaries and bias only XY by half a
                     * pixel; without this, every independent 8x2 shuffle
                     * sprite samples across its REGION_REPEAT seam.
                     */
                    ps2GsCoreWriteReg(&p[out++],
                        ps2GsCoreMakeXyz2Fixed(
                            (int)x0 * 16 - 8,
                            (int)y0 * 16 - 8, 0u),
                        GS_XYZ2);
                    ps2GsCoreWriteReg(&p[out++],
                        GS_SETREG_UV(
                            (u_xor + tile_width) * 16u,
                            (first.v + tile_height) * 16u),
                        GS_UV);
                    ps2GsCoreWriteReg(&p[out++],
                        ps2GsCoreMakeXyz2Fixed(
                            (int)(x0 + tile_width) * 16 - 8,
                            (int)(y0 + tile_height) * 16 - 8, 0u),
                        GS_XYZ2);
                }
            }

            s_loaded_clut_vram = shared->vram;
            s_loaded_clut_psm = GS_PSM_CT32;
            load_clut = false;
            ps2GsCoreMarkActiveRenderTargetWritten();
        }
    }

    const bool restored = ps2GsCoreRestoreChannelBlitState(
        saved_scissor_x0, saved_scissor_x1,
        saved_scissor_y0, saved_scissor_y1);
    /* The shuffle leaves these material registers owned by its last page. */
    ps2GsStateShadowInvalidate(&s_state_shadow, PS2_GS_STATE_TEX0);
    ps2GsStateShadowInvalidate(&s_state_shadow, PS2_GS_STATE_TEX1);
    ps2GsStateShadowInvalidate(&s_state_shadow, PS2_GS_STATE_PRIM);
    return success && restored;
}
