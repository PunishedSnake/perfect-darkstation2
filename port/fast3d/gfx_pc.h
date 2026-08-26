#ifndef GFX_PC_H
#define GFX_PC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unordered_map>
#include <list>
#include <cstddef>

#include <PR/gbi.h>

#include "system.h"

#define SCREEN_WIDTH ((int32_t)gfx_current_native_viewport.width)
#define SCREEN_HEIGHT ((int32_t)gfx_current_native_viewport.height)

extern uintptr_t gfxFramebuffer;

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;

struct TextureCacheKey {
    const uint8_t* texture_addr;
    const uint8_t* palette_addrs[2];
    uint8_t fmt, siz;
    uint8_t palette_index;

    /*
     * Optional backend-independent content identity. Portable renderers keep
     * this disabled and retain the historical source-pointer key. PS2 enables
     * it only when the live RDP TMEM shadow can prove the consumed texture and
     * TLUT footprint exactly. Pointer invalidation deliberately does not match
     * such keys: changing source RAM without a later RDP Load* must not mutate
     * already-loaded TMEM semantics.
     */
    uint64_t content_identity = 0;
    bool content_identity_valid = false;

    bool operator==(const TextureCacheKey& other) const noexcept {
        if (content_identity_valid != other.content_identity_valid ||
            fmt != other.fmt || siz != other.siz ||
            palette_index != other.palette_index) {
            return false;
        }

        if (content_identity_valid) {
            return content_identity == other.content_identity;
        }

        return texture_addr == other.texture_addr &&
               palette_addrs[0] == other.palette_addrs[0] &&
               palette_addrs[1] == other.palette_addrs[1];
    }

    struct Hasher {
        size_t operator()(const TextureCacheKey& key) const noexcept {
            if (key.content_identity_valid) {
                uint64_t x = key.content_identity;
                x ^= x >> 33;
                x *= UINT64_C(0xff51afd7ed558ccd);
                x ^= x >> 33;
                x *= UINT64_C(0xc4ceb9fe1a85ec53);
                x ^= x >> 33;
                return (size_t)x;
            }

            uintptr_t addr = (uintptr_t)key.texture_addr;
            return (size_t)(addr ^ (addr >> 5));
        }
    };
};

typedef std::unordered_map<TextureCacheKey, struct TextureCacheValue, TextureCacheKey::Hasher> TextureCacheMap;
typedef std::pair<const TextureCacheKey, struct TextureCacheValue> TextureCacheNode;

struct TextureCacheValue {
    uint32_t texture_id;
    uint8_t cms, cmt;
    bool linear_filter;

    std::list<struct TextureCacheMapIter>::iterator lru_location;
};

struct TextureCacheMapIter {
    TextureCacheMap::iterator it;
};

extern "C" {

#include "gfx_api.h"

}

#endif
