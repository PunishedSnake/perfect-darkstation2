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

    /* Mirror+clamp is deliberately not the mirror-wrap physical variant. */
    assert(gfxPs2TextureMirrorVariant(3u, 0u) == 0u);
    assert(gfxPs2TextureMirrorVariant(0u, 3u) == 0u);

    const uint64_t base = UINT64_C(0x0123456789abcdef);
    const uint64_t ordinary = gfxPs2TextureVariantIdentity(base, 0u, 0u);
    const uint64_t mirror_s = gfxPs2TextureVariantIdentity(base, 1u, 0u);
    const uint64_t mirror_t = gfxPs2TextureVariantIdentity(base, 0u, 1u);
    const uint64_t mirror_st = gfxPs2TextureVariantIdentity(base, 1u, 1u);
    assert(ordinary != mirror_s && ordinary != mirror_t &&
           ordinary != mirror_st && mirror_s != mirror_t &&
           mirror_s != mirror_st && mirror_t != mirror_st);

    puts("gfx_ps2 texture variant tests passed");
    return 0;
}
