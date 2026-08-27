#ifndef PERFECT_DARK_PS2_GS_VRAM_ALLOCATOR_H
#define PERFECT_DARK_PS2_GS_VRAM_ALLOCATOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_GS_VRAM_BYTES (4u * 1024u * 1024u)
#define PS2_GS_VRAM_BLOCK_BYTES 256u
#define PS2_GS_VRAM_MAX_FREE_RANGES 129u

struct Ps2GsVramRange {
    uint32_t offset;
    uint32_t size;
};

/*
 * Fixed-metadata allocator for the project-owned GS-local resource pool.
 *
 * The system framebuffer/Z prefix is excluded at init. Ordinary allocations
 * are block-rounded to the 256-byte texture base-pointer unit; callers such as
 * render targets can request a stricter power-of-two alignment. Free ranges
 * stay sorted and are coalesced immediately without render-path heap use.
 */
struct Ps2GsVramAllocator {
    uint32_t begin;
    uint32_t end;
    uint16_t free_count;
    struct Ps2GsVramRange free_ranges[PS2_GS_VRAM_MAX_FREE_RANGES];
};

struct Ps2GsVramStats {
    uint32_t pool_bytes;
    uint32_t free_bytes;
    uint32_t largest_free_bytes;
    uint16_t free_ranges;
};

bool ps2GsVramAllocatorInit(struct Ps2GsVramAllocator *allocator,
    uint32_t begin, uint32_t end);
bool ps2GsVramAllocatorAlloc(struct Ps2GsVramAllocator *allocator,
    uint32_t size, uint32_t *offset);
/* Alignment must be a power of two and at least one 256-byte GS block. */
bool ps2GsVramAllocatorAllocAligned(struct Ps2GsVramAllocator *allocator,
    uint32_t size, uint32_t alignment, uint32_t *offset);
bool ps2GsVramAllocatorFree(struct Ps2GsVramAllocator *allocator,
    uint32_t offset, uint32_t size);
void ps2GsVramAllocatorGetStats(const struct Ps2GsVramAllocator *allocator,
    struct Ps2GsVramStats *stats);

#ifdef __cplusplus
}
#endif

#endif
