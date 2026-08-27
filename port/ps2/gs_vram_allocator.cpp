#include "gs_vram_allocator.h"

#include <stddef.h>
#include <string.h>

static uint32_t ps2GsVramAlignUp(uint32_t value)
{
    return (value + PS2_GS_VRAM_BLOCK_BYTES - 1u) &
           ~(PS2_GS_VRAM_BLOCK_BYTES - 1u);
}

static uint32_t ps2GsVramAlignDown(uint32_t value)
{
    return value & ~(PS2_GS_VRAM_BLOCK_BYTES - 1u);
}

extern "C" bool ps2GsVramAllocatorInit(
    struct Ps2GsVramAllocator *allocator, uint32_t begin, uint32_t end)
{
    if (!allocator || begin > end || end > PS2_GS_VRAM_BYTES) {
        return false;
    }

    const uint32_t aligned_begin = ps2GsVramAlignUp(begin);
    const uint32_t aligned_end = ps2GsVramAlignDown(end);
    if (aligned_begin > aligned_end) {
        return false;
    }

    memset(allocator, 0, sizeof(*allocator));
    allocator->begin = aligned_begin;
    allocator->end = aligned_end;
    if (aligned_begin < aligned_end) {
        allocator->free_count = 1u;
        allocator->free_ranges[0].offset = aligned_begin;
        allocator->free_ranges[0].size = aligned_end - aligned_begin;
    }
    return true;
}

extern "C" bool ps2GsVramAllocatorAlloc(
    struct Ps2GsVramAllocator *allocator, uint32_t size, uint32_t *offset)
{
    return ps2GsVramAllocatorAllocAligned(
        allocator, size, PS2_GS_VRAM_BLOCK_BYTES, offset);
}

extern "C" bool ps2GsVramAllocatorAllocAligned(
    struct Ps2GsVramAllocator *allocator, uint32_t size,
    uint32_t alignment, uint32_t *offset)
{
    if (!allocator || !offset || size == 0u ||
        size > PS2_GS_VRAM_BYTES - (PS2_GS_VRAM_BLOCK_BYTES - 1u) ||
        alignment < PS2_GS_VRAM_BLOCK_BYTES ||
        alignment > PS2_GS_VRAM_BYTES ||
        (alignment & (alignment - 1u)) != 0u) {
        return false;
    }

    const uint32_t aligned_size = ps2GsVramAlignUp(size);
    uint16_t best = UINT16_MAX;
    uint32_t best_size = UINT32_MAX;
    uint32_t best_offset = 0u;
    for (uint16_t i = 0; i < allocator->free_count; ++i) {
        const uint32_t range_offset = allocator->free_ranges[i].offset;
        const uint32_t range_size = allocator->free_ranges[i].size;
        const uint32_t aligned_offset =
            (range_offset + alignment - 1u) & ~(alignment - 1u);
        if (aligned_offset < range_offset) {
            continue;
        }
        const uint32_t prefix = aligned_offset - range_offset;
        if (prefix > range_size || aligned_size > range_size - prefix) {
            continue;
        }
        const uint32_t suffix = range_size - prefix - aligned_size;
        if (prefix != 0u && suffix != 0u &&
            allocator->free_count >= PS2_GS_VRAM_MAX_FREE_RANGES) {
            continue;
        }
        if (range_size < best_size) {
            best = i;
            best_size = range_size;
            best_offset = aligned_offset;
        }
    }

    if (best == UINT16_MAX) {
        return false;
    }

    struct Ps2GsVramRange *range = &allocator->free_ranges[best];
    const uint32_t range_offset = range->offset;
    const uint32_t range_size = range->size;
    const uint32_t prefix = best_offset - range_offset;
    const uint32_t suffix = range_size - prefix - aligned_size;
    *offset = best_offset;

    if (prefix == 0u && suffix == 0u) {
        for (uint16_t i = best + 1u; i < allocator->free_count; ++i) {
            allocator->free_ranges[i - 1u] = allocator->free_ranges[i];
        }
        --allocator->free_count;
    } else if (prefix == 0u) {
        range->offset = best_offset + aligned_size;
        range->size = suffix;
    } else if (suffix == 0u) {
        range->size = prefix;
    } else {
        for (uint16_t i = allocator->free_count; i > best + 1u; --i) {
            allocator->free_ranges[i] = allocator->free_ranges[i - 1u];
        }
        range->size = prefix;
        allocator->free_ranges[best + 1u].offset =
            best_offset + aligned_size;
        allocator->free_ranges[best + 1u].size = suffix;
        ++allocator->free_count;
    }
    return true;
}

extern "C" bool ps2GsVramAllocatorFree(
    struct Ps2GsVramAllocator *allocator, uint32_t offset, uint32_t size)
{
    if (!allocator || size == 0u ||
        (offset & (PS2_GS_VRAM_BLOCK_BYTES - 1u)) != 0u ||
        size > PS2_GS_VRAM_BYTES - (PS2_GS_VRAM_BLOCK_BYTES - 1u)) {
        return false;
    }

    const uint32_t aligned_size = ps2GsVramAlignUp(size);
    if (offset < allocator->begin || offset > allocator->end ||
        aligned_size > allocator->end - offset) {
        return false;
    }

    uint16_t pos = 0u;
    while (pos < allocator->free_count &&
           allocator->free_ranges[pos].offset < offset) {
        ++pos;
    }

    if (pos > 0u) {
        const struct Ps2GsVramRange *previous =
            &allocator->free_ranges[pos - 1u];
        if (offset < previous->offset + previous->size) {
            return false;
        }
    }
    if (pos < allocator->free_count &&
        offset + aligned_size > allocator->free_ranges[pos].offset) {
        return false;
    }

    const bool merge_previous = pos > 0u &&
        allocator->free_ranges[pos - 1u].offset +
            allocator->free_ranges[pos - 1u].size == offset;
    const bool merge_next = pos < allocator->free_count &&
        offset + aligned_size == allocator->free_ranges[pos].offset;

    if (merge_previous && merge_next) {
        allocator->free_ranges[pos - 1u].size +=
            aligned_size + allocator->free_ranges[pos].size;
        for (uint16_t i = pos + 1u; i < allocator->free_count; ++i) {
            allocator->free_ranges[i - 1u] = allocator->free_ranges[i];
        }
        --allocator->free_count;
        return true;
    }
    if (merge_previous) {
        allocator->free_ranges[pos - 1u].size += aligned_size;
        return true;
    }
    if (merge_next) {
        allocator->free_ranges[pos].offset = offset;
        allocator->free_ranges[pos].size += aligned_size;
        return true;
    }

    if (allocator->free_count >= PS2_GS_VRAM_MAX_FREE_RANGES) {
        return false;
    }
    for (uint16_t i = allocator->free_count; i > pos; --i) {
        allocator->free_ranges[i] = allocator->free_ranges[i - 1u];
    }
    allocator->free_ranges[pos].offset = offset;
    allocator->free_ranges[pos].size = aligned_size;
    ++allocator->free_count;
    return true;
}

extern "C" void ps2GsVramAllocatorGetStats(
    const struct Ps2GsVramAllocator *allocator,
    struct Ps2GsVramStats *stats)
{
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    if (!allocator) {
        return;
    }

    stats->pool_bytes = allocator->end - allocator->begin;
    stats->free_ranges = allocator->free_count;
    for (uint16_t i = 0; i < allocator->free_count; ++i) {
        const uint32_t size = allocator->free_ranges[i].size;
        stats->free_bytes += size;
        if (size > stats->largest_free_bytes) {
            stats->largest_free_bytes = size;
        }
    }
}
