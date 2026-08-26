#include "rdp_tmem_trace.h"

#include <stddef.h>
#include <string.h>

#define TMEM_TRACE_MAX_DEPTH 32u
#define TMEM_TRACE_MAX_COMMANDS (1u << 20)

#define TC0(cmd_, pos_, width_) \
    ((uint32_t)(((cmd_)->words.w0 >> (pos_)) & ((1u << (width_)) - 1u)))
#define TC1(cmd_, pos_, width_) \
    ((uint32_t)(((cmd_)->words.w1 >> (pos_)) & ((1u << (width_)) - 1u)))

struct GfxRdpTmemTraceImage {
    const uint8_t *addr;
    uint8_t fmt;
    uint8_t siz;
    uint16_t width_minus_one;
    bool valid;
};

struct GfxRdpTmemTraceTile {
    uint8_t fmt;
    uint8_t siz;
    uint16_t line_words;
    uint16_t tmem_word;
    bool valid;
};

static struct {
    bool initialized;
    struct GfxRdpTmem tmem;
    struct GfxRdpTmemTraceStats stats;
    struct GfxRdpTmemTraceImage image;
    struct GfxRdpTmemTraceTile tiles[8];
    uintptr_t segments[16];
} s_trace;

static uint32_t gfxRdpTmemTraceBitsPerTexel(uint32_t siz)
{
    return siz <= G_IM_SIZ_32b ? (4u << siz) : 0u;
}

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
    (void)gfxRdpTmemInvalidatePhysical(&s_trace.tmem, 0u, GFX_RDP_TMEM_BYTES);
}

static void gfxRdpTmemTraceMalformed(void)
{
    ++s_trace.stats.malformed_or_unsupported;
}

static bool gfxRdpTmemTraceInvalidateLinearTile(
    const struct GfxRdpTmemTraceTile *tile,
    uint32_t row_words, uint32_t row_count)
{
    if (!tile || row_words == 0u || row_count == 0u) {
        return true;
    }

    for (uint32_t row = 0; row < row_count; ++row) {
        const uint64_t first_word =
            (uint64_t)tile->tmem_word + (uint64_t)row * tile->line_words;
        if (first_word >= GFX_RDP_TMEM_WORDS ||
            row_words > GFX_RDP_TMEM_WORDS - (uint32_t)first_word) {
            gfxRdpTmemTraceInvalidateAll();
            return false;
        }

        (void)gfxRdpTmemInvalidatePhysical(&s_trace.tmem,
            (uint32_t)first_word * GFX_RDP_TMEM_WORD_BYTES,
            row_words * GFX_RDP_TMEM_WORD_BYTES);
    }
    return true;
}

static bool gfxRdpTmemTraceInvalidateSplitTile(
    const struct GfxRdpTmemTraceTile *tile,
    uint32_t row_words, uint32_t row_count)
{
    if (!tile || row_words == 0u || row_count == 0u) {
        return true;
    }

    for (uint32_t row = 0; row < row_count; ++row) {
        const uint64_t low_word =
            (uint64_t)tile->tmem_word + (uint64_t)row * tile->line_words;
        if (low_word >= GFX_RDP_TMEM_HALF_WORDS ||
            row_words > GFX_RDP_TMEM_HALF_WORDS - (uint32_t)low_word) {
            gfxRdpTmemTraceInvalidateAll();
            return false;
        }

        const uint32_t low = (uint32_t)low_word;
        const uint32_t high = low + GFX_RDP_TMEM_HALF_WORDS;
        (void)gfxRdpTmemInvalidatePhysical(&s_trace.tmem,
            low * GFX_RDP_TMEM_WORD_BYTES,
            row_words * GFX_RDP_TMEM_WORD_BYTES);
        (void)gfxRdpTmemInvalidatePhysical(&s_trace.tmem,
            high * GFX_RDP_TMEM_WORD_BYTES,
            row_words * GFX_RDP_TMEM_WORD_BYTES);
    }
    return true;
}

static void gfxRdpTmemTraceSetTextureImage(const Gfx *cmd)
{
    s_trace.image.fmt = (uint8_t)TC0(cmd, 21, 3);
    s_trace.image.siz = (uint8_t)TC0(cmd, 19, 2);
    s_trace.image.width_minus_one = (uint16_t)TC0(cmd, 0, 10);
    s_trace.image.addr =
        (const uint8_t *)gfxRdpTmemTraceResolve(cmd->words.w1);
    s_trace.image.valid = s_trace.image.addr != NULL;
    ++s_trace.stats.set_texture_image;
}

static void gfxRdpTmemTraceSetTile(const Gfx *cmd)
{
    const uint32_t index = TC1(cmd, 24, 3);
    struct GfxRdpTmemTraceTile *tile = &s_trace.tiles[index];

    tile->fmt = (uint8_t)TC0(cmd, 21, 3);
    tile->siz = (uint8_t)TC0(cmd, 19, 2);
    tile->line_words = (uint16_t)TC0(cmd, 9, 9);
    tile->tmem_word = (uint16_t)TC0(cmd, 0, 9);
    tile->valid = true;
    ++s_trace.stats.set_tile;
}

static void gfxRdpTmemTraceLoadTlut(const Gfx *cmd)
{
    const uint32_t tile_index = TC1(cmd, 24, 3);
    const struct GfxRdpTmemTraceTile *tile = &s_trace.tiles[tile_index];
    const uint32_t uls = TC0(cmd, 14, 10);
    const uint32_t ult = TC0(cmd, 2, 10);
    const uint32_t lrs = TC1(cmd, 14, 10);
    const uint32_t lrt = TC1(cmd, 2, 10);

    if (!s_trace.image.valid || !tile->valid ||
        s_trace.image.siz != G_IM_SIZ_16b ||
        lrs < uls || lrt < ult) {
        gfxRdpTmemTraceMalformed();
        gfxRdpTmemTraceInvalidateAll();
        return;
    }

    const uint32_t width = lrs - uls + 1u;
    const uint32_t height = lrt - ult + 1u;
    const uint64_t count64 = (uint64_t)width * height;
    if (count64 == 0u || count64 > 256u ||
        tile->tmem_word < GFX_RDP_TMEM_HALF_WORDS ||
        count64 > GFX_RDP_TMEM_WORDS - tile->tmem_word) {
        gfxRdpTmemTraceMalformed();
        gfxRdpTmemTraceInvalidateAll();
        return;
    }

    const uint32_t pitch = (uint32_t)s_trace.image.width_minus_one + 1u;
    const uint64_t first_index = (uint64_t)pitch * ult + uls;
    const uint8_t *src = s_trace.image.addr + first_index * 2u;
    uint16_t entries[256];
    const uint32_t count = (uint32_t)count64;

    for (uint32_t i = 0; i < count; ++i) {
        entries[i] = (uint16_t)((uint16_t)src[i * 2u] << 8) |
                     (uint16_t)src[i * 2u + 1u];
    }

    if (!gfxRdpTmemWriteTlut(&s_trace.tmem,
            tile->tmem_word, entries, count)) {
        gfxRdpTmemTraceMalformed();
        gfxRdpTmemTraceInvalidateAll();
        return;
    }

    ++s_trace.stats.load_tlut_exact;
}

static void gfxRdpTmemTraceLoadTile(const Gfx *cmd)
{
    const uint32_t tile_index = TC1(cmd, 24, 3);
    const struct GfxRdpTmemTraceTile *tile = &s_trace.tiles[tile_index];
    const uint32_t uls = TC0(cmd, 12, 12);
    const uint32_t ult = TC0(cmd, 0, 12);
    const uint32_t lrs = TC1(cmd, 12, 12);
    const uint32_t lrt = TC1(cmd, 0, 12);

    if (!s_trace.image.valid || !tile->valid || lrs < uls || lrt < ult) {
        gfxRdpTmemTraceMalformed();
        gfxRdpTmemTraceInvalidateAll();
        return;
    }

    const uint32_t bits = gfxRdpTmemTraceBitsPerTexel(s_trace.image.siz);
    if (bits == 0u) {
        ++s_trace.stats.load_tile_conservative;
        gfxRdpTmemTraceInvalidateAll();
        return;
    }

    const uint32_t offset_x = uls >> G_TEXTURE_IMAGE_FRAC;
    const uint32_t offset_y = ult >> G_TEXTURE_IMAGE_FRAC;
    const uint32_t width = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1u;
    const uint32_t height = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1u;
    const uint32_t full_width = (uint32_t)s_trace.image.width_minus_one + 1u;
    const uint64_t source_stride_bits = (uint64_t)full_width * bits;
    const uint64_t source_x_bits = (uint64_t)offset_x * bits;

    if ((source_stride_bits & 7u) != 0u || (source_x_bits & 7u) != 0u ||
        source_stride_bits / 8u > UINT32_MAX) {
        const uint32_t row_bytes = (uint32_t)(((uint64_t)width * bits + 7u) / 8u);
        const uint32_t row_words =
            (row_bytes + GFX_RDP_TMEM_WORD_BYTES - 1u) / GFX_RDP_TMEM_WORD_BYTES;
        (void)gfxRdpTmemTraceInvalidateLinearTile(tile, row_words, height);
        ++s_trace.stats.load_tile_conservative;
        return;
    }

    const uint32_t source_stride = (uint32_t)(source_stride_bits / 8u);
    const uint64_t start_offset =
        (uint64_t)offset_y * source_stride + source_x_bits / 8u;
    const uint8_t *source = s_trace.image.addr + start_offset;

    if (tile->fmt == G_IM_FMT_RGBA &&
        s_trace.image.siz == G_IM_SIZ_32b &&
        tile->siz == G_IM_SIZ_32b) {
        if (gfxRdpTmemLoadTileRgba32(&s_trace.tmem,
                tile->tmem_word, tile->line_words,
                source, source_stride, width, height)) {
            ++s_trace.stats.load_tile_exact;
            return;
        }

        const uint32_t row_words = (width + 3u) / 4u;
        (void)gfxRdpTmemTraceInvalidateSplitTile(tile, row_words, height);
        ++s_trace.stats.load_tile_conservative;
        return;
    }

    if (tile->fmt == G_IM_FMT_YUV || s_trace.image.fmt == G_IM_FMT_YUV) {
        const uint32_t row_words = (width + 3u) / 4u;
        (void)gfxRdpTmemTraceInvalidateSplitTile(tile, row_words, height);
        ++s_trace.stats.load_tile_conservative;
        return;
    }

    const uint32_t row_bytes =
        (uint32_t)(((uint64_t)width * bits + 7u) / 8u);
    if (gfxRdpTmemLoadTileLinear(&s_trace.tmem,
            tile->tmem_word, tile->line_words,
            source, source_stride, row_bytes, height)) {
        ++s_trace.stats.load_tile_exact;
        return;
    }

    const uint32_t row_words =
        (row_bytes + GFX_RDP_TMEM_WORD_BYTES - 1u) / GFX_RDP_TMEM_WORD_BYTES;
    (void)gfxRdpTmemTraceInvalidateLinearTile(tile, row_words, height);
    ++s_trace.stats.load_tile_conservative;
}

static void gfxRdpTmemTraceLoadBlock(const Gfx *cmd)
{
    const uint32_t tile_index = TC1(cmd, 24, 3);
    const struct GfxRdpTmemTraceTile *tile = &s_trace.tiles[tile_index];
    const uint32_t uls = TC0(cmd, 12, 12);
    const uint32_t ult = TC0(cmd, 0, 12);
    const uint32_t lrs = TC1(cmd, 12, 12);
    const uint16_t dxt = (uint16_t)TC1(cmd, 0, 12);

    if (!s_trace.image.valid || !tile->valid || uls != 0u || ult != 0u) {
        gfxRdpTmemTraceMalformed();
        gfxRdpTmemTraceInvalidateAll();
        return;
    }

    const uint64_t size64 = ((uint64_t)lrs + 1u) << s_trace.image.siz >> 1u;
    if (size64 == 0u || size64 > UINT32_MAX) {
        gfxRdpTmemTraceMalformed();
        gfxRdpTmemTraceInvalidateAll();
        return;
    }

    /*
     * RGBA32 and YUV need split-bank formatting during LoadBlock. The exact
     * row/tmem-index interaction is intentionally not guessed here; invalidate
     * conservatively until the dedicated loader is verified against Nintendo's
     * layout plus real-N64 behavior.
     */
    if ((tile->fmt == G_IM_FMT_RGBA && s_trace.image.siz == G_IM_SIZ_32b) ||
        tile->fmt == G_IM_FMT_YUV || s_trace.image.fmt == G_IM_FMT_YUV) {
        ++s_trace.stats.load_block_conservative;
        gfxRdpTmemTraceInvalidateAll();
        return;
    }

    if (gfxRdpTmemLoadBlockLinear(&s_trace.tmem,
            tile->tmem_word, s_trace.image.addr, (uint32_t)size64, dxt)) {
        ++s_trace.stats.load_block_exact;
        return;
    }

    ++s_trace.stats.load_block_conservative;
    gfxRdpTmemTraceInvalidateAll();
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
        if (++s_trace.stats.commands > TMEM_TRACE_MAX_COMMANDS) {
            gfxRdpTmemTraceMalformed();
            return;
        }

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
    gfxRdpTmemReset(&s_trace.tmem);
    s_trace.initialized = true;
}

extern "C" void gfxRdpTmemTraceDisplayList(const Gfx *commands)
{
    if (!s_trace.initialized) {
        gfxRdpTmemTraceReset();
    }

    /* Per-call command guard; lifetime TMEM/segment state intentionally persists. */
    s_trace.stats.commands = 0u;
    gfxRdpTmemTraceRun(commands, 0u);
}

extern "C" const struct GfxRdpTmem *gfxRdpTmemTraceState(void)
{
    if (!s_trace.initialized) {
        gfxRdpTmemTraceReset();
    }
    return &s_trace.tmem;
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
