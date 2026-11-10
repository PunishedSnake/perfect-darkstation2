#include "rdp_tmem_live.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

enum : uint8_t {
    FMT_RGBA = 0u,
    FMT_YUV = 1u,
    FMT_CI = 2u,
    SIZ_4B = 0u,
    SIZ_16B = 2u,
    SIZ_32B = 3u,
};

static void test_identical_reload_keeps_fingerprint(void)
{
    uint8_t source[32];
    for (uint32_t i = 0; i < sizeof(source); ++i) {
        source[i] = (uint8_t)(0x20u + i);
    }

    gfxRdpTmemLiveReset();
    gfxRdpTmemLiveSetTextureImage(FMT_RGBA, SIZ_16B, 15u, source);
    gfxRdpTmemLiveSetTile(FMT_RGBA, SIZ_16B, 2u, 0u, 0u);
    gfxRdpTmemLiveLoadBlock(0u, 0u, 0u, 15u, 0x400u);

    uint64_t first = 0u;
    assert(gfxRdpTmemLiveTextureFingerprint(
        0u, sizeof(source), sizeof(source),
        FMT_RGBA, SIZ_16B, 0u, &first));

    gfxRdpTmemLiveLoadBlock(0u, 0u, 0u, 15u, 0x400u);
    uint64_t second = 0u;
    assert(gfxRdpTmemLiveTextureFingerprint(
        0u, sizeof(source), sizeof(source),
        FMT_RGBA, SIZ_16B, 0u, &second));
    assert(first == second);
}

static void test_changed_texel_changes_fingerprint(void)
{
    uint8_t source[32];
    for (uint32_t i = 0; i < sizeof(source); ++i) {
        source[i] = (uint8_t)(0x40u + i);
    }

    gfxRdpTmemLiveReset();
    gfxRdpTmemLiveSetTextureImage(FMT_RGBA, SIZ_16B, 15u, source);
    gfxRdpTmemLiveSetTile(FMT_RGBA, SIZ_16B, 2u, 0u, 0u);
    gfxRdpTmemLiveLoadBlock(0u, 0u, 0u, 15u, 0x400u);

    uint64_t before = 0u;
    assert(gfxRdpTmemLiveTextureFingerprint(
        0u, sizeof(source), sizeof(source),
        FMT_RGBA, SIZ_16B, 0u, &before));

    source[11] ^= 0x5au;
    gfxRdpTmemLiveLoadBlock(0u, 0u, 0u, 15u, 0x400u);

    uint64_t after = 0u;
    assert(gfxRdpTmemLiveTextureFingerprint(
        0u, sizeof(source), sizeof(source),
        FMT_RGBA, SIZ_16B, 0u, &after));
    assert(before != after);
}

static void test_unrelated_tmem_load_does_not_change_fingerprint(void)
{
    uint8_t source[32];
    uint8_t unrelated[16];
    for (uint32_t i = 0; i < sizeof(source); ++i) {
        source[i] = (uint8_t)(0x60u + i);
    }
    for (uint32_t i = 0; i < sizeof(unrelated); ++i) {
        unrelated[i] = (uint8_t)(0xa0u + i);
    }

    gfxRdpTmemLiveReset();
    gfxRdpTmemLiveSetTextureImage(FMT_RGBA, SIZ_16B, 15u, source);
    gfxRdpTmemLiveSetTile(FMT_RGBA, SIZ_16B, 2u, 0u, 0u);
    gfxRdpTmemLiveLoadBlock(0u, 0u, 0u, 15u, 0x400u);

    uint64_t before = 0u;
    assert(gfxRdpTmemLiveTextureFingerprint(
        0u, sizeof(source), sizeof(source),
        FMT_RGBA, SIZ_16B, 0u, &before));

    gfxRdpTmemLiveSetTextureImage(FMT_RGBA, SIZ_16B, 7u, unrelated);
    gfxRdpTmemLiveSetTile(FMT_RGBA, SIZ_16B, 1u, 32u, 1u);
    gfxRdpTmemLiveLoadBlock(1u, 0u, 0u, 7u, 0u);

    uint64_t after = 0u;
    assert(gfxRdpTmemLiveTextureFingerprint(
        0u, sizeof(source), sizeof(source),
        FMT_RGBA, SIZ_16B, 0u, &after));
    assert(before == after);
}

static void test_fingerprint_independent_of_load_record_eviction(void)
{
    uint8_t source[32];
    uint8_t unrelated[16];
    for (uint32_t i = 0; i < sizeof(source); ++i) {
        source[i] = (uint8_t)(0x13u * i + 7u);
    }
    for (uint32_t i = 0; i < sizeof(unrelated); ++i) {
        unrelated[i] = (uint8_t)(0xc0u + i);
    }

    gfxRdpTmemLiveReset();
    gfxRdpTmemLiveSetTextureImage(FMT_RGBA, SIZ_16B, 15u, source);
    gfxRdpTmemLiveSetTile(FMT_RGBA, SIZ_16B, 2u, 0u, 0u);
    gfxRdpTmemLiveLoadBlock(0u, 0u, 0u, 15u, 0x400u);

    uint64_t via_record = 0u;
    assert(gfxRdpTmemLiveTextureFingerprint(
        0u, sizeof(source), sizeof(source),
        FMT_RGBA, SIZ_16B, 0u, &via_record));

    /* Push the texture's load record out of the ring with unrelated loads. */
    gfxRdpTmemLiveSetTextureImage(FMT_RGBA, SIZ_16B, 7u, unrelated);
    gfxRdpTmemLiveSetTile(FMT_RGBA, SIZ_16B, 1u, 64u, 1u);
    for (uint32_t i = 0; i < 2u * GFX_RDP_TMEM_LOAD_RECORDS; ++i) {
        gfxRdpTmemLiveLoadBlock(1u, 0u, 0u, 7u, 0u);
    }

    uint64_t via_read = 0u;
    assert(gfxRdpTmemLiveTextureFingerprint(
        0u, sizeof(source), sizeof(source),
        FMT_RGBA, SIZ_16B, 0u, &via_read));
    assert(via_record == via_read);

    /* And a rewrite with different bytes must still change the identity. */
    source[3] ^= 1u;
    gfxRdpTmemLiveSetTextureImage(FMT_RGBA, SIZ_16B, 15u, source);
    gfxRdpTmemLiveSetTile(FMT_RGBA, SIZ_16B, 2u, 0u, 0u);
    gfxRdpTmemLiveLoadBlock(0u, 0u, 0u, 15u, 0x400u);
    uint64_t changed = 0u;
    assert(gfxRdpTmemLiveTextureFingerprint(
        0u, sizeof(source), sizeof(source),
        FMT_RGBA, SIZ_16B, 0u, &changed));
    assert(changed != via_record);
}

/*
 * Lazy LoadBlock must be unobservable: run the same randomized command
 * stream eagerly (flush after every command) and lazily, and require
 * identical fingerprints, materialized texels and final TMEM contents.
 */
static uint32_t g_lazy_rng;
static uint32_t lazy_rand(void)
{
    g_lazy_rng ^= g_lazy_rng << 13;
    g_lazy_rng ^= g_lazy_rng >> 17;
    g_lazy_rng ^= g_lazy_rng << 5;
    return g_lazy_rng;
}

static uint64_t run_lazy_stream(bool eager, uint8_t *final_bytes,
    uint8_t *final_valid, uint32_t *elided)
{
    static uint8_t sources[6][512];
    static uint16_t tluts[3][256];
    g_lazy_rng = 0x1234abcdu;
    for (uint32_t s = 0; s < 6u; ++s) {
        for (uint32_t i = 0; i < 512u; ++i) {
            sources[s][i] = (uint8_t)lazy_rand();
        }
    }
    for (uint32_t s = 0; s < 3u; ++s) {
        for (uint32_t i = 0; i < 256u; ++i) {
            tluts[s][i] = (uint16_t)lazy_rand();
        }
    }

    uint64_t digest = 0u;
    gfxRdpTmemLiveReset();
    for (uint32_t step = 0; step < 6000u; ++step) {
        const uint32_t op = lazy_rand() % 10u;
        if (op < 5u) {
            /* LoadBlock of 4b/8b/16b data at a few TMEM addresses. */
            const uint32_t siz = lazy_rand() % 3u; /* 4b, 8b, 16b */
            const uint32_t words = 1u + lazy_rand() % 48u;
            const uint32_t tmem = (lazy_rand() % 4u) * 16u;
            const uint32_t texels = (words * 8u * 8u) / (4u << siz);
            const uint8_t fmt = siz == 2u ? FMT_RGBA : FMT_CI;
            gfxRdpTmemLiveSetTextureImage(fmt, siz, 31u,
                sources[lazy_rand() % 6u] + (lazy_rand() % 8u) * 8u);
            gfxRdpTmemLiveSetTile(fmt, siz, 1u + lazy_rand() % 4u, tmem, 7u);
            gfxRdpTmemLiveLoadBlock(7u, 0u, 0u, texels - 1u,
                (lazy_rand() % 3u) ? (lazy_rand() & 0xfffu) : 0u);
        } else if (op == 5u) {
            gfxRdpTmemLiveSetTextureImage(FMT_RGBA, SIZ_16B, 0u,
                tluts[lazy_rand() % 3u]);
            gfxRdpTmemLiveSetTile(FMT_RGBA, SIZ_4B, 0u,
                256u + (lazy_rand() % 16u) * 16u, 6u);
            gfxRdpTmemLiveLoadTlut(6u, 0u, 0u, 15u << 2, 0u);
        } else if (op == 6u) {
            /* Occasionally mutate source RAM between "frames". */
            gfxRdpTmemLiveFlush();
            sources[lazy_rand() % 6u][lazy_rand() % 512u] ^= 0x41u;
        } else {
            /* Render through some tile: fingerprint, sometimes texels. */
            const uint32_t siz = lazy_rand() % 3u;
            const uint32_t line = 1u + lazy_rand() % 4u;
            const uint32_t rows = 1u + lazy_rand() % 12u;
            const uint8_t fmt = siz == 2u ? FMT_RGBA :
                ((lazy_rand() & 1u) ? FMT_CI : FMT_RGBA);
            gfxRdpTmemLiveSetTile(fmt, siz, line,
                (lazy_rand() % 4u) * 16u, 0u);
            const uint32_t size_bytes = line * 8u * rows;
            if (lazy_rand() & 1u) {
                uint64_t fp = 0u;
                const bool ok = gfxRdpTmemLiveTextureFingerprint(0u,
                    line * 8u, size_bytes, fmt, (uint8_t)siz,
                    (uint8_t)(lazy_rand() % 16u), &fp);
                digest = digest * UINT64_C(0x100000001b3) ^ (ok ? fp : 0x5a5au);
            } else {
                struct GfxRdpTmemLiveTextureView view;
                const bool ok = gfxRdpTmemLiveMaterializeTexture(0u,
                    line * 8u, size_bytes, fmt, (uint8_t)siz,
                    (uint8_t)(lazy_rand() % 16u), &view);
                digest = digest * UINT64_C(0x100000001b3) ^
                    (ok ? view.content_identity : 0xa5a5u);
                if (ok) {
                    for (uint32_t i = 0; i < view.size_bytes; ++i) {
                        digest = digest * 131u + view.texels[i];
                    }
                }
            }
        }
        if (eager) {
            gfxRdpTmemLiveFlush();
        }
    }

    struct GfxRdpTmemLiveStats stats;
    gfxRdpTmemLiveGetStats(&stats);
    *elided = stats.load_block_elided;
    if (!eager) {
        /* both the pending-block identity and the copy-free path ran */
        assert(stats.fingerprint_source_hash > 100u);
    }
    const struct GfxRdpTmem *tmem = gfxRdpTmemLiveState();
    memcpy(final_bytes, tmem->bytes, GFX_RDP_TMEM_BYTES);
    memcpy(final_valid, tmem->byte_valid, GFX_RDP_TMEM_BYTES);
    return digest;
}

static void test_lazy_load_block_is_unobservable(void)
{
    static uint8_t eager_bytes[GFX_RDP_TMEM_BYTES], lazy_bytes[GFX_RDP_TMEM_BYTES];
    static uint8_t eager_valid[GFX_RDP_TMEM_BYTES], lazy_valid[GFX_RDP_TMEM_BYTES];
    uint32_t eager_elided = 0u, lazy_elided = 0u;
    const uint64_t eager = run_lazy_stream(true, eager_bytes, eager_valid,
        &eager_elided);
    const uint64_t lazy = run_lazy_stream(false, lazy_bytes, lazy_valid,
        &lazy_elided);
    assert(eager == lazy);
    assert(memcmp(eager_bytes, lazy_bytes, sizeof(eager_bytes)) == 0);
    assert(memcmp(eager_valid, lazy_valid, sizeof(eager_valid)) == 0);
    assert(eager_elided == 0u);
    assert(lazy_elided != 0u); /* the stream really exercised elision */
}

static void test_ci_fingerprint_tracks_tlut(void)
{
    uint8_t palette[32];
    uint8_t texture[8];
    for (uint32_t i = 0; i < sizeof(palette); ++i) {
        palette[i] = (uint8_t)(0x10u + i);
    }
    for (uint32_t i = 0; i < sizeof(texture); ++i) {
        texture[i] = (uint8_t)(0x80u + i);
    }

    gfxRdpTmemLiveReset();
    gfxRdpTmemLiveSetTextureImage(FMT_RGBA, SIZ_16B, 15u, palette);
    gfxRdpTmemLiveSetTile(FMT_RGBA, SIZ_16B, 0u, 256u, 7u);
    gfxRdpTmemLiveLoadTlut(7u, 0u, 0u, 15u, 0u);

    gfxRdpTmemLiveSetTextureImage(FMT_CI, SIZ_4B, 15u, texture);
    gfxRdpTmemLiveSetTile(FMT_CI, SIZ_4B, 1u, 0u, 0u);
    gfxRdpTmemLiveLoadBlock(0u, 0u, 0u, 15u, 0u);

    uint64_t before = 0u;
    assert(gfxRdpTmemLiveTextureFingerprint(
        0u, sizeof(texture), sizeof(texture),
        FMT_CI, SIZ_4B, 0u, &before));

    palette[0] ^= 0x01u;
    gfxRdpTmemLiveSetTextureImage(FMT_RGBA, SIZ_16B, 15u, palette);
    gfxRdpTmemLiveLoadTlut(7u, 0u, 0u, 15u, 0u);

    uint64_t after = 0u;
    assert(gfxRdpTmemLiveTextureFingerprint(
        0u, sizeof(texture), sizeof(texture),
        FMT_CI, SIZ_4B, 0u, &after));
    assert(before != after);
}

static void test_conservative_shadow_forces_pointer_fallback(void)
{
    uint8_t source[16] = {};

    gfxRdpTmemLiveReset();
    gfxRdpTmemLiveSetTextureImage(FMT_YUV, SIZ_16B, 7u, source);
    gfxRdpTmemLiveSetTile(FMT_YUV, SIZ_16B, 1u, 0u, 0u);
    gfxRdpTmemLiveLoadBlock(0u, 0u, 0u, 7u, 0u);

    uint64_t fingerprint = 0u;
    assert(!gfxRdpTmemLiveTextureFingerprint(
        0u, sizeof(source), sizeof(source),
        FMT_YUV, SIZ_16B, 0u, &fingerprint));

    GfxRdpTmemLiveStats stats{};
    gfxRdpTmemLiveGetStats(&stats);
    assert(stats.load_block_conservative == 1u);
    assert(stats.fingerprint_fallback == 1u);
}

static void test_materialized_linear_view_owns_loaded_bytes(void)
{
    uint8_t source[32];
    uint8_t expected[sizeof(source)];
    for (uint32_t i = 0; i < sizeof(source); ++i) {
        source[i] = (uint8_t)(0x20u + i);
    }
    memcpy(expected, source, sizeof(expected));

    gfxRdpTmemLiveReset();
    gfxRdpTmemLiveSetTextureImage(FMT_RGBA, SIZ_16B, 15u, source);
    gfxRdpTmemLiveSetTile(FMT_RGBA, SIZ_16B, 2u, 0u, 0u);
    gfxRdpTmemLiveLoadBlock(0u, 0u, 0u, 15u, 0x400u);

    /*
     * Block loads are lazy while a display list executes; the end-of-list
     * flush is the point after which TMEM owns the bytes and source RAM may
     * change freely.
     */
    gfxRdpTmemLiveFlush();
    memset(source, 0xee, sizeof(source));
    GfxRdpTmemLiveTextureView view{};
    assert(gfxRdpTmemLiveMaterializeTexture(
        0u, sizeof(source), sizeof(source),
        FMT_RGBA, SIZ_16B, 0u, &view));
    assert(view.size_bytes == sizeof(expected));
    assert(view.line_size_bytes == 16u);
    assert(memcmp(view.texels, expected, sizeof(expected)) == 0);
}

static void test_materialized_rgba32_split_view(void)
{
    uint8_t source[8u * 4u];
    uint8_t expected[sizeof(source)];
    for (uint32_t i = 0; i < sizeof(source); ++i) {
        source[i] = (uint8_t)(0x60u + i);
    }
    memcpy(expected, source, sizeof(expected));

    gfxRdpTmemLiveReset();
    gfxRdpTmemLiveSetTextureImage(FMT_RGBA, SIZ_32B, 3u, source);
    gfxRdpTmemLiveSetTile(FMT_RGBA, SIZ_32B, 0u, 0u, 0u);
    gfxRdpTmemLiveLoadBlock(0u, 0u, 0u, 7u, 0x400u);
    gfxRdpTmemLiveSetTile(FMT_RGBA, SIZ_32B, 1u, 0u, 0u);

    memset(source, 0x11, sizeof(source));
    GfxRdpTmemLiveTextureView view{};
    assert(gfxRdpTmemLiveMaterializeTexture(
        0u, sizeof(source), sizeof(source),
        FMT_RGBA, SIZ_32B, 0u, &view));
    assert(view.line_size_bytes == 16u);
    assert(memcmp(view.texels, expected, sizeof(expected)) == 0);
}

static void test_materialized_ci_view_owns_tlut(void)
{
    uint8_t palette[32];
    uint8_t texture[8];
    uint8_t expected_texture[sizeof(texture)];
    for (uint32_t i = 0; i < sizeof(palette); ++i) {
        palette[i] = (uint8_t)(0x10u + i);
    }
    for (uint32_t i = 0; i < sizeof(texture); ++i) {
        texture[i] = (uint8_t)(0x80u + i);
    }
    memcpy(expected_texture, texture, sizeof(expected_texture));

    gfxRdpTmemLiveReset();
    gfxRdpTmemLiveSetTextureImage(FMT_RGBA, SIZ_16B, 15u, palette);
    gfxRdpTmemLiveSetTile(FMT_RGBA, SIZ_16B, 0u, 256u, 7u);
    gfxRdpTmemLiveLoadTlut(7u, 0u, 0u, 15u, 0u);
    gfxRdpTmemLiveSetTextureImage(FMT_CI, SIZ_4B, 15u, texture);
    gfxRdpTmemLiveSetTile(FMT_CI, SIZ_4B, 1u, 0u, 0u);
    gfxRdpTmemLiveLoadBlock(0u, 0u, 0u, 15u, 0u);
    gfxRdpTmemLiveFlush(); /* end of display list: TMEM owns the bytes */

    memset(palette, 0, sizeof(palette));
    memset(texture, 0, sizeof(texture));
    GfxRdpTmemLiveTextureView view{};
    assert(gfxRdpTmemLiveMaterializeTexture(
        0u, sizeof(texture), sizeof(texture),
        FMT_CI, SIZ_4B, 0u, &view));
    assert(memcmp(view.texels, expected_texture,
        sizeof(expected_texture)) == 0);
    assert(view.palette_first == 0u);
    assert(view.palette_count == 16u);
    assert(view.palette[0] == 0x1011u);
    assert(view.palette[15] == 0x2e2fu);
}

} // namespace

int main(void)
{
    test_identical_reload_keeps_fingerprint();
    test_changed_texel_changes_fingerprint();
    test_unrelated_tmem_load_does_not_change_fingerprint();
    test_ci_fingerprint_tracks_tlut();
    test_fingerprint_independent_of_load_record_eviction();
    test_lazy_load_block_is_unobservable();
    test_conservative_shadow_forces_pointer_fallback();
    test_materialized_linear_view_owns_loaded_bytes();
    test_materialized_rgba32_split_view();
    test_materialized_ci_view_owns_tlut();
    puts("rdp_tmem_live tests passed");
    return 0;
}
