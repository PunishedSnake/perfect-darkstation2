#include "rdp_tmem_live.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

namespace {

enum : uint8_t {
    FMT_RGBA = 0u,
    FMT_YUV = 1u,
    FMT_CI = 2u,
    SIZ_4B = 0u,
    SIZ_16B = 2u,
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

} // namespace

int main(void)
{
    test_identical_reload_keeps_fingerprint();
    test_changed_texel_changes_fingerprint();
    test_unrelated_tmem_load_does_not_change_fingerprint();
    test_ci_fingerprint_tracks_tlut();
    test_conservative_shadow_forces_pointer_fallback();
    puts("rdp_tmem_live tests passed");
    return 0;
}
