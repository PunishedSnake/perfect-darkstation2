#include "gfx_ps2.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    assert(gfxPs2TextureMirrorVariant(0u, 0u) == 0u);
    assert(gfxPs2TextureMirrorVariant(1u, 0u) == 1u);
    assert(gfxPs2TextureMirrorVariant(0u, 1u) == 2u);
    assert(gfxPs2TextureMirrorVariant(1u, 1u) == 3u);

    /* Mirror+clamp shares the reflected residency; clamp is draw state. */
    assert(gfxPs2TextureMirrorVariant(3u, 0u) == 1u);
    assert(gfxPs2TextureMirrorVariant(0u, 3u) == 2u);

    uint16_t clamp_max = UINT16_MAX;
    assert(gfxPs2TextureRegionClampMax(7.5f / 8.0f, 8u, &clamp_max));
    assert(clamp_max == 7u);
    assert(gfxPs2TextureRegionClampMax(63.5f / 32.0f, 32u, &clamp_max));
    assert(clamp_max == 63u);
    assert(gfxPs2TextureRegionClampMax(1023.5f / 64.0f, 64u, &clamp_max));
    assert(clamp_max == 1023u);
    assert(!gfxPs2TextureRegionClampMax(1024.5f / 64.0f, 64u, &clamp_max));
    assert(!gfxPs2TextureRegionClampMax(0.0f, 0u, &clamp_max));
    assert(!gfxPs2TextureRegionClampMax(-0.5f, 8u, &clamp_max));
    assert(!gfxPs2TextureRegionClampMax(0.5f, 8u, NULL));

    const uint64_t base = UINT64_C(0x0123456789abcdef);
    const uint64_t ordinary = gfxPs2TextureVariantIdentity(
        base, 0u, 0u, 0u, 0u);
    const uint64_t mirror_s = gfxPs2TextureVariantIdentity(
        base, 0u, 1u, 0u, 0u);
    const uint64_t mirror_t = gfxPs2TextureVariantIdentity(
        base, 0u, 0u, 1u, 0u);
    const uint64_t mirror_st = gfxPs2TextureVariantIdentity(
        base, 0u, 1u, 1u, 0u);
    const uint64_t mirror_clamp_s = gfxPs2TextureVariantIdentity(
        base, 0u, 3u, 0u, 0u);
    assert(ordinary != mirror_s && ordinary != mirror_t &&
           ordinary != mirror_st && mirror_s != mirror_t &&
           mirror_s != mirror_st && mirror_t != mirror_st);
    assert(mirror_clamp_s == mirror_s);

    const uint64_t ci_rgba16 = gfxPs2TextureVariantIdentity(
        base, 2u, 0u, 0u, 2u << 14u);
    const uint64_t ci_ia16 = gfxPs2TextureVariantIdentity(
        base, 2u, 0u, 0u, 3u << 14u);
    const uint64_t non_ci_ignores_tlut = gfxPs2TextureVariantIdentity(
        base, 3u, 0u, 0u, 3u << 14u);
    assert(ci_rgba16 != ci_ia16);
    assert(non_ci_ignores_tlut == ordinary);

    puts("gfx_ps2 texture variant tests passed");
    return 0;
}
