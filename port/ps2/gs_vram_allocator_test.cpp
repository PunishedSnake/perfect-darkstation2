#include "gs_vram_allocator.h"

#include <assert.h>
#include <stdio.h>

static void test_alignment_best_fit_and_coalescing(void)
{
    Ps2GsVramAllocator allocator{};
    assert(ps2GsVramAllocatorInit(&allocator, 0x1001u, 0x5000u));
    assert(allocator.begin == 0x1100u);
    assert(allocator.end == 0x5000u);

    uint32_t a = 0u;
    uint32_t b = 0u;
    uint32_t c = 0u;
    assert(ps2GsVramAllocatorAlloc(&allocator, 1u, &a));
    assert(ps2GsVramAllocatorAlloc(&allocator, 0x180u, &b));
    assert(ps2GsVramAllocatorAlloc(&allocator, 0x300u, &c));
    assert(a == 0x1100u);
    assert(b == 0x1200u);
    assert(c == 0x1400u);

    assert(ps2GsVramAllocatorFree(&allocator, b, 0x180u));
    assert(ps2GsVramAllocatorFree(&allocator, a, 1u));
    assert(ps2GsVramAllocatorFree(&allocator, c, 0x300u));

    Ps2GsVramStats stats{};
    ps2GsVramAllocatorGetStats(&allocator, &stats);
    assert(stats.free_ranges == 1u);
    assert(stats.free_bytes == stats.pool_bytes);
    assert(stats.largest_free_bytes == stats.pool_bytes);
}

static void test_fragment_reuse_and_reject_double_free(void)
{
    Ps2GsVramAllocator allocator{};
    assert(ps2GsVramAllocatorInit(&allocator, 0u, 0x1000u));

    uint32_t a = 0u;
    uint32_t b = 0u;
    uint32_t c = 0u;
    assert(ps2GsVramAllocatorAlloc(&allocator, 0x400u, &a));
    assert(ps2GsVramAllocatorAlloc(&allocator, 0x400u, &b));
    assert(ps2GsVramAllocatorAlloc(&allocator, 0x400u, &c));
    assert(ps2GsVramAllocatorFree(&allocator, b, 0x400u));

    uint32_t reused = 0u;
    assert(ps2GsVramAllocatorAlloc(&allocator, 0x300u, &reused));
    assert(reused == b);
    assert(!ps2GsVramAllocatorFree(&allocator, reused, 0x500u));
    assert(ps2GsVramAllocatorFree(&allocator, reused, 0x300u));
    assert(!ps2GsVramAllocatorFree(&allocator, reused, 0x300u));
}

static void test_bounds_and_exhaustion(void)
{
    Ps2GsVramAllocator allocator{};
    assert(ps2GsVramAllocatorInit(&allocator,
        PS2_GS_VRAM_BYTES - 0x400u, PS2_GS_VRAM_BYTES));

    uint32_t block = 0u;
    assert(ps2GsVramAllocatorAlloc(&allocator, 0x400u, &block));
    assert(block == PS2_GS_VRAM_BYTES - 0x400u);
    assert(!ps2GsVramAllocatorAlloc(&allocator, 1u, &block));
    assert(!ps2GsVramAllocatorFree(&allocator,
        PS2_GS_VRAM_BYTES, 0x100u));
}

static void test_maximum_fragmentation_metadata(void)
{
    constexpr uint32_t block_count = 256u;
    Ps2GsVramAllocator allocator{};
    assert(ps2GsVramAllocatorInit(&allocator, 0u,
        block_count * PS2_GS_VRAM_BLOCK_BYTES));

    uint32_t blocks[block_count]{};
    for (uint32_t i = 0; i < block_count; ++i) {
        assert(ps2GsVramAllocatorAlloc(&allocator,
            PS2_GS_VRAM_BLOCK_BYTES, &blocks[i]));
    }
    for (uint32_t i = 0; i < block_count; i += 2u) {
        assert(ps2GsVramAllocatorFree(&allocator,
            blocks[i], PS2_GS_VRAM_BLOCK_BYTES));
    }
    assert(allocator.free_count == block_count / 2u);

    for (uint32_t i = 1u; i < block_count; i += 2u) {
        assert(ps2GsVramAllocatorFree(&allocator,
            blocks[i], PS2_GS_VRAM_BLOCK_BYTES));
    }
    assert(allocator.free_count == 1u);
    assert(allocator.free_ranges[0].size ==
        block_count * PS2_GS_VRAM_BLOCK_BYTES);
}

int main(void)
{
    test_alignment_best_fit_and_coalescing();
    test_fragment_reuse_and_reject_double_free();
    test_bounds_and_exhaustion();
    test_maximum_fragmentation_metadata();
    puts("gs_vram_allocator tests passed");
    return 0;
}
