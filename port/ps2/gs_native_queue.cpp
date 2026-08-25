#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <malloc.h>

#include <dmaKit.h>
#include <kernel.h>

#include "gs_native_queue.h"
#include "log_ps2.h"
#include "system.h"

#define PS2_GIF_PACKED 0u
#define PS2_GIF_REG_AD 0x0eu
#define PS2_GIF_MAX_NLOOP 0x7fffu

struct Ps2GsNativeArena {
    void *canonical;
    uint64_t *ucab;
    uint32_t capacity_qw;
    uint32_t used_qw;
    bool overflowed;
};

static struct Ps2GsNativeArena s_arenas[2];
static uint32_t s_build_arena;
static bool s_initialized;

static_assert(sizeof(struct Ps2GsPackedReg) == 16,
    "native A+D records must occupy exactly one GIF quadword");

static uint64_t ps2GifPackedAdTag(uint32_t nloop)
{
    return ((uint64_t)nloop << 0) |
           ((uint64_t)1u << 15) |      /* EOP */
           ((uint64_t)0u << 46) |      /* PRE */
           ((uint64_t)0u << 47) |      /* PRIM */
           ((uint64_t)PS2_GIF_PACKED << 58) |
           ((uint64_t)1u << 60);       /* NREG = one A+D descriptor */
}

extern "C" bool ps2GsNativeQueueInit(uint32_t qwords_per_buffer)
{
    if (s_initialized) {
        return true;
    }
    if (qwords_per_buffer < 64) {
        return false;
    }

    const size_t bytes = (size_t)qwords_per_buffer * 16u;
    memset(s_arenas, 0, sizeof(s_arenas));

    for (uint32_t i = 0; i < 2; ++i) {
        void *canonical = memalign(64, bytes);
        if (!canonical) {
            sysLogPrintf(LOG_ERROR,
                "GS native queue: allocation failed arena=%u bytes=%u",
                i, (unsigned int)bytes);
            for (uint32_t j = 0; j < i; ++j) {
                free(s_arenas[j].canonical);
                s_arenas[j].canonical = NULL;
                s_arenas[j].ucab = NULL;
            }
            return false;
        }

        memset(canonical, 0, bytes);
        SyncDCache(canonical, (uint8_t *)canonical + bytes - 1);

        s_arenas[i].canonical = canonical;
        s_arenas[i].ucab = (uint64_t *)UCAB_SEG(canonical);
        s_arenas[i].capacity_qw = qwords_per_buffer;
    }

    s_build_arena = 0;
    s_initialized = true;

    sysLogPrintf(LOG_NOTE,
        "GS native queue: 2x%u QW (%u KiB each) UCAB command arenas",
        qwords_per_buffer, (unsigned int)(bytes / 1024u));
    ps2LogCheckpoint();
    return true;
}

extern "C" void ps2GsNativeQueueBeginFrame(void)
{
    if (!s_initialized) {
        return;
    }

    struct Ps2GsNativeArena *arena = &s_arenas[s_build_arena];
    arena->used_qw = 0;
    arena->overflowed = false;
}

extern "C" struct Ps2GsPackedReg *ps2GsNativeQueueReserveAd(uint32_t reg_count)
{
    if (!s_initialized || reg_count == 0 || reg_count > PS2_GIF_MAX_NLOOP) {
        return NULL;
    }

    struct Ps2GsNativeArena *arena = &s_arenas[s_build_arena];
    const uint32_t packet_qw = reg_count + 1u;
    if (arena->used_qw > arena->capacity_qw ||
        packet_qw > arena->capacity_qw - arena->used_qw) {
        if (!arena->overflowed) {
            sysLogPrintf(LOG_ERROR,
                "GS native queue: overflow used=%u request=%u capacity=%u QW",
                arena->used_qw, packet_qw, arena->capacity_qw);
            arena->overflowed = true;
        }
        return NULL;
    }

    uint64_t *packet = arena->ucab + (size_t)arena->used_qw * 2u;
    packet[0] = ps2GifPackedAdTag(reg_count);
    packet[1] = PS2_GIF_REG_AD;

    arena->used_qw += packet_qw;
    return (struct Ps2GsPackedReg *)(packet + 2);
}

extern "C" bool ps2GsNativeQueueSubmit(void)
{
    if (!s_initialized) {
        return false;
    }

    struct Ps2GsNativeArena *arena = &s_arenas[s_build_arena];
    if (arena->overflowed) {
        return false;
    }
    if (arena->used_qw == 0) {
        return true;
    }

    /*
     * Ownership barrier only for the GIF DMA channel. Unlike current gsKit
     * queue_exec this does not append or wait for GS FINISH. With two arenas,
     * EE packet construction can proceed in the other buffer while PATH3 owns
     * this one. A busy channel is waited only when a new submit actually needs
     * to claim it.
     */
    if (dmaKit_wait(DMA_CHANNEL_GIF, 0) < 0) {
        return false;
    }

    dmaKit_send_ucab(DMA_CHANNEL_GIF, arena->ucab, arena->used_qw);
    s_build_arena ^= 1u;
    return true;
}

extern "C" uint32_t ps2GsNativeQueueUsedQwords(void)
{
    return s_initialized ? s_arenas[s_build_arena].used_qw : 0;
}

extern "C" uint32_t ps2GsNativeQueueCapacityQwords(void)
{
    return s_initialized ? s_arenas[s_build_arena].capacity_qw : 0;
}

extern "C" bool ps2GsNativeQueueOverflowed(void)
{
    return s_initialized && s_arenas[s_build_arena].overflowed;
}
