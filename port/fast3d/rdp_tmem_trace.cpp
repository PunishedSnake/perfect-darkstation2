#include "rdp_tmem_trace.h"
#include "rdp_tmem_runtime.h"

#include <stddef.h>
#include <string.h>

#define TMEM_TRACE_MAX_DEPTH 32u
#define TMEM_TRACE_MAX_COMMANDS (1u << 20)

#define TC0(cmd_, pos_, width_) \
    ((uint32_t)(((cmd_)->words.w0 >> (pos_)) & ((1u << (width_)) - 1u)))
#define TC1(cmd_, pos_, width_) \
    ((uint32_t)(((cmd_)->words.w1 >> (pos_)) & ((1u << (width_)) - 1u)))

static struct {
    bool initialized;
    struct GfxRdpTmemRuntime runtime;
    struct GfxRdpTmemTraceStats stats;
    uintptr_t segments[16];
    uint32_t commands_this_run;
} s_trace;

static const void *gfxRdpTmemTraceResolve(uintptr_t address)
{
    /* Match current gfx_pc.cpp segmented-address convention exactly. */
    if (address & 1u) {
        const uint32_t segment = (uint32_t)((address >> 24) & 0x0fu);
        if (segment != 0u && s_trace.segments[segment] != 0u) {
            const uintptr_t offset = address & 0x00fffffeu;
            return (const void *)(s_trace.segments[segment] + offset);
        }
    }

    return (const void *)address;
}

static void gfxRdpTmemTraceInvalidateAll(void)
{
    (void)gfxRdpTmemInvalidatePhysical(&s_trace.runtime.tmem,
        0u, GFX_RDP_TMEM_BYTES);
}

static void gfxRdpTmemTraceMalformed(void)
{
    ++s_trace.stats.malformed_or_unsupported;
}

static void gfxRdpTmemTraceRecordLoad(enum GfxRdpTmemLoadResult result,
    uint32_t *exact, uint32_t *conservative)
{
    switch (result) {
        case GFX_RDP_TMEM_LOAD_EXACT:
            if (exact) {
                ++*exact;
            }
            break;
        case GFX_RDP_TMEM_LOAD_CONSERVATIVE:
            if (conservative) {
                ++*conservative;
            }
            break;
        case GFX_RDP_TMEM_LOAD_MALFORMED:
        default:
            gfxRdpTmemTraceMalformed();
            break;
    }
}

static void gfxRdpTmemTraceSetTextureImage(const Gfx *cmd)
{
    gfxRdpTmemRuntimeSetTextureImage(&s_trace.runtime,
        (uint8_t)TC0(cmd, 21, 3),
        (uint8_t)TC0(cmd, 19, 2),
        (uint16_t)TC0(cmd, 0, 10),
        gfxRdpTmemTraceResolve(cmd->words.w1));
    ++s_trace.stats.set_texture_image;
}

static void gfxRdpTmemTraceSetTile(const Gfx *cmd)
{
    if (!gfxRdpTmemRuntimeSetTile(&s_trace.runtime,
            (uint8_t)TC1(cmd, 24, 3),
            (uint8_t)TC0(cmd, 21, 3),
            (uint8_t)TC0(cmd, 19, 2),
            (uint16_t)TC0(cmd, 9, 9),
            (uint16_t)TC0(cmd, 0, 9))) {
        gfxRdpTmemTraceMalformed();
        gfxRdpTmemTraceInvalidateAll();
    }
    ++s_trace.stats.set_tile;
}

static void gfxRdpTmemTraceLoadTlut(const Gfx *cmd)
{
    const enum GfxRdpTmemLoadResult result = gfxRdpTmemRuntimeLoadTlut(
        &s_trace.runtime,
        (uint8_t)TC1(cmd, 24, 3),
        TC0(cmd, 14, 10), TC0(cmd, 2, 10),
        TC1(cmd, 14, 10), TC1(cmd, 2, 10));

    if (result == GFX_RDP_TMEM_LOAD_EXACT) {
        ++s_trace.stats.load_tlut_exact;
    } else if (result == GFX_RDP_TMEM_LOAD_MALFORMED) {
        gfxRdpTmemTraceMalformed();
    }
}

static void gfxRdpTmemTraceLoadTile(const Gfx *cmd)
{
    const enum GfxRdpTmemLoadResult result = gfxRdpTmemRuntimeLoadTile(
        &s_trace.runtime,
        (uint8_t)TC1(cmd, 24, 3),
        TC0(cmd, 12, 12), TC0(cmd, 0, 12),
        TC1(cmd, 12, 12), TC1(cmd, 0, 12));

    gfxRdpTmemTraceRecordLoad(result,
        &s_trace.stats.load_tile_exact,
        &s_trace.stats.load_tile_conservative);
}

static void gfxRdpTmemTraceLoadBlock(const Gfx *cmd)
{
    const enum GfxRdpTmemLoadResult result = gfxRdpTmemRuntimeLoadBlock(
        &s_trace.runtime,
        (uint8_t)TC1(cmd, 24, 3),
        TC0(cmd, 12, 12), TC0(cmd, 0, 12),
        TC1(cmd, 12, 12), (uint16_t)TC1(cmd, 0, 12));

    gfxRdpTmemTraceRecordLoad(result,
        &s_trace.stats.load_block_exact,
        &s_trace.stats.load_block_conservative);
}

static void gfxRdpTmemTraceRun(const Gfx *commands, uint32_t depth)
{
    if (!commands || depth >= TMEM_TRACE_MAX_DEPTH) {
        gfxRdpTmemTraceMalformed();
        return;
    }

    ++s_trace.stats.display_lists;
    const Gfx *cmd = commands;

    for (;;) {
        if (s_trace.commands_this_run >= TMEM_TRACE_MAX_COMMANDS) {
            gfxRdpTmemTraceMalformed();
            return;
        }
        ++s_trace.commands_this_run;
        ++s_trace.stats.commands_total;

        const uint8_t opcode = (uint8_t)(cmd->words.w0 >> 24);
        switch (opcode) {
            case G_SETTIMG:
                gfxRdpTmemTraceSetTextureImage(cmd);
                break;
            case G_SETTILE:
                gfxRdpTmemTraceSetTile(cmd);
                break;
            case G_LOADTLUT:
                gfxRdpTmemTraceLoadTlut(cmd);
                break;
            case G_LOADTILE:
                gfxRdpTmemTraceLoadTile(cmd);
                break;
            case G_LOADBLOCK:
                gfxRdpTmemTraceLoadBlock(cmd);
                break;
            case (uint8_t)G_MOVEWORD:
                if (TC0(cmd, 0, 8) == G_MW_SEGMENT) {
                    const uint32_t segment = (TC0(cmd, 8, 16) >> 2) & 0xffu;
                    if (segment < 16u) {
                        s_trace.segments[segment] = cmd->words.w1;
                    } else {
                        gfxRdpTmemTraceMalformed();
                    }
                }
                break;
            case G_DL: {
                const Gfx *target =
                    (const Gfx *)gfxRdpTmemTraceResolve(cmd->words.w1);
                if (!target) {
                    gfxRdpTmemTraceMalformed();
                    break;
                }
                if (TC0(cmd, 16, 1) == 0u) {
                    gfxRdpTmemTraceRun(target, depth + 1u);
                } else {
                    cmd = target;
                    continue;
                }
                break;
            }
            case (uint8_t)G_ENDDL:
                return;
            case G_TEXRECT:
            case G_TEXRECTFLIP:
                cmd += 2;
                break;
#ifdef G_FILLRECT_WIDE_EXT
            case G_FILLRECT_WIDE_EXT:
                cmd += 1;
                break;
#endif
#ifdef G_TEXRECT_WIDE_EXT
            case G_TEXRECT_WIDE_EXT:
                cmd += 2;
                break;
#endif
#ifdef G_IMAGERECT_EXT
            case G_IMAGERECT_EXT:
                cmd += 2;
                break;
#endif
            default:
                break;
        }

        ++cmd;
    }
}

extern "C" void gfxRdpTmemTraceReset(void)
{
    memset(&s_trace, 0, sizeof(s_trace));
    gfxRdpTmemRuntimeReset(&s_trace.runtime);
    s_trace.initialized = true;
}

extern "C" void gfxRdpTmemTraceDisplayList(const Gfx *commands)
{
    if (!s_trace.initialized) {
        gfxRdpTmemTraceReset();
    }

    /*
     * The guard is per top-level submission and is shared by nested DL calls.
     * Lifetime counters remain monotonic so telemetry can describe a full run.
     */
    s_trace.commands_this_run = 0u;
    gfxRdpTmemTraceRun(commands, 0u);
    s_trace.stats.commands_last_run = s_trace.commands_this_run;
    if (s_trace.commands_this_run > s_trace.stats.commands_max_per_run) {
        s_trace.stats.commands_max_per_run = s_trace.commands_this_run;
    }
}

extern "C" const struct GfxRdpTmem *gfxRdpTmemTraceState(void)
{
    if (!s_trace.initialized) {
        gfxRdpTmemTraceReset();
    }
    return gfxRdpTmemRuntimeState(&s_trace.runtime);
}

extern "C" void gfxRdpTmemTraceGetStats(struct GfxRdpTmemTraceStats *stats)
{
    if (!stats) {
        return;
    }
    if (!s_trace.initialized) {
        gfxRdpTmemTraceReset();
    }
    *stats = s_trace.stats;
}
