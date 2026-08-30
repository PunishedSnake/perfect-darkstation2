#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <malloc.h>

#include <dmaKit.h>
#include <kernel.h>
#include <vif_registers.h>

#include "gs_native_queue.h"
#include "gs_vu1_batch.h"
#include "gs_vu1_queue.h"
#include "log_ps2.h"
#include "system.h"

#if defined(PERFECT_DARK_PS2_VU1_COLOR_BATCH)

#define PS2_VIF_CMD_NOP    0x00u
#define PS2_VIF_CMD_BASE   0x03u
#define PS2_VIF_CMD_OFFSET 0x02u
#define PS2_VIF_CMD_FLUSHA 0x13u
#define PS2_VIF_CMD_MPG    0x4au

#define PS2_DMA_TAG_CNT 0x01u
#define PS2_DMA_TAG_END 0x07u

#define PS2_VIF1_STAT_ACTIVE_MASK 0x1f00000fu
#define PS2_VIF1_STAT_ERROR_MASK  0x00003000u

struct Ps2GsVu1QueueSlot {
    void *canonical;
    uint32_t *ucab;
};

extern "C" uint32_t Ps2GsVu1ColorPathCodeStart;
extern "C" uint32_t Ps2GsVu1ColorPathCodeEnd;

static struct Ps2GsVu1QueueSlot s_slots[2];
static uint32_t s_build_slot;
static bool s_initialized;
static bool s_pending;

static_assert(sizeof(struct Ps2GsColorVertex) ==
        2u * sizeof(struct Ps2GsPackedReg),
    "color vertices must remain contiguous A+D records");
static_assert(sizeof(struct Ps2GsTexturedVertex) ==
        3u * sizeof(struct Ps2GsPackedReg),
    "textured vertices must remain contiguous A+D records");

static uint32_t ps2GsVu1QueueVifCode(
    uint32_t immediate, uint32_t num, uint32_t command)
{
    return (immediate & 0xffffu) |
        ((num & 0xffu) << 16) |
        ((command & 0x7fu) << 24);
}

static uint64_t ps2GsVu1QueueDmaTag(uint32_t qwords, uint32_t id)
{
    return ((uint64_t)(qwords & 0xffffu) << 0) |
        ((uint64_t)(id & 0x7u) << 28);
}

static void ps2GsVu1QueueRelease(void)
{
    for (uint32_t i = 0; i < 2u; ++i) {
        free(s_slots[i].canonical);
        s_slots[i].canonical = NULL;
        s_slots[i].ucab = NULL;
    }
    s_initialized = false;
    s_pending = false;
}

static bool ps2GsVu1QueueWaitVifIdle(void)
{
    if (dmaKit_wait(DMA_CHANNEL_VIF1, 0) < 0) {
        return false;
    }
    for (;;) {
        const uint32_t status = VIF1_STAT;
        if ((status & PS2_VIF1_STAT_ERROR_MASK) != 0u) {
            return false;
        }
        if ((status & PS2_VIF1_STAT_ACTIVE_MASK) == 0u) {
            return true;
        }
    }
}

static bool ps2GsVu1QueueSendChainAndWait(uint32_t *ucab)
{
    if (!ucab || !ps2GsVu1QueueWaitVifIdle()) {
        return false;
    }
    dmaKit_send_chain_ucab(DMA_CHANNEL_VIF1, ucab);
    return ps2GsVu1QueueWaitVifIdle();
}

static bool ps2GsVu1QueueUploadProgram(void)
{
    const ptrdiff_t program_words =
        &Ps2GsVu1ColorPathCodeEnd - &Ps2GsVu1ColorPathCodeStart;
    if (program_words <= 0 || (program_words & 1) != 0) {
        return false;
    }

    const uint32_t instruction_count = (uint32_t)program_words / 2u;
    if (instruction_count == 0u || instruction_count > 256u ||
        (instruction_count & 1u) != 0u) {
        return false;
    }

    const uint32_t program_qwords = instruction_count / 2u;
    const uint32_t chain_qwords = program_qwords + 2u;
    const size_t stream_bytes = (size_t)chain_qwords * 16u;
    void *canonical = memalign(64, stream_bytes);
    if (!canonical) {
        return false;
    }

    uint32_t *ucab = (uint32_t *)UCAB_SEG(canonical);
    memset(ucab, 0, stream_bytes);
    const uint64_t cnt_tag = ps2GsVu1QueueDmaTag(
        program_qwords, PS2_DMA_TAG_CNT);
    memcpy(&ucab[0], &cnt_tag, sizeof(cnt_tag));
    ucab[2] = ps2GsVu1QueueVifCode(0u, 0u, PS2_VIF_CMD_NOP);
    ucab[3] = ps2GsVu1QueueVifCode(
        0u, instruction_count == 256u ? 0u : instruction_count,
        PS2_VIF_CMD_MPG);
    memcpy(&ucab[4], &Ps2GsVu1ColorPathCodeStart,
        (size_t)program_words * sizeof(uint32_t));

    const uint32_t end_word = 4u + program_qwords * 4u;
    const uint64_t end_tag = ps2GsVu1QueueDmaTag(0u, PS2_DMA_TAG_END);
    memcpy(&ucab[end_word], &end_tag, sizeof(end_tag));
    ucab[end_word + 2u] =
        ps2GsVu1QueueVifCode(0u, 0u, PS2_VIF_CMD_NOP);
    ucab[end_word + 3u] =
        ps2GsVu1QueueVifCode(0u, 0u, PS2_VIF_CMD_NOP);

    const bool submitted = ps2GsVu1QueueSendChainAndWait(ucab);
    free(canonical);
    return submitted;
}

static bool ps2GsVu1QueueConfigureBuffers(void)
{
    void *canonical = memalign(64, 64u);
    if (!canonical) {
        return false;
    }

    uint32_t *ucab = (uint32_t *)UCAB_SEG(canonical);
    memset(ucab, 0, 64u);
    const uint64_t cnt_tag = ps2GsVu1QueueDmaTag(0u, PS2_DMA_TAG_CNT);
    memcpy(&ucab[0], &cnt_tag, sizeof(cnt_tag));
    ucab[2] = ps2GsVu1QueueVifCode(
        PS2_GS_VU1_BUFFER_BASE_QW, 0u, PS2_VIF_CMD_BASE);
    ucab[3] = ps2GsVu1QueueVifCode(
        PS2_GS_VU1_BUFFER_OFFSET_QW, 0u, PS2_VIF_CMD_OFFSET);
    const uint64_t end_tag = ps2GsVu1QueueDmaTag(0u, PS2_DMA_TAG_END);
    memcpy(&ucab[4], &end_tag, sizeof(end_tag));
    ucab[6] = ps2GsVu1QueueVifCode(0u, 0u, PS2_VIF_CMD_FLUSHA);
    ucab[7] = ps2GsVu1QueueVifCode(0u, 0u, PS2_VIF_CMD_NOP);

    const bool submitted = ps2GsVu1QueueSendChainAndWait(ucab);
    free(canonical);
    return submitted;
}

#endif

extern "C" bool ps2GsVu1QueueInit(void)
{
#if !defined(PERFECT_DARK_PS2_VU1_COLOR_BATCH)
    return true;
#else
    if (s_initialized) {
        return true;
    }

    memset(s_slots, 0, sizeof(s_slots));
    const size_t slot_bytes = (size_t)PS2_GS_VU1_DMA_SLOT_QW * 16u;
    for (uint32_t i = 0; i < 2u; ++i) {
        s_slots[i].canonical = memalign(64, slot_bytes);
        if (!s_slots[i].canonical) {
            ps2GsVu1QueueRelease();
            return false;
        }
        s_slots[i].ucab = (uint32_t *)UCAB_SEG(s_slots[i].canonical);
        memset(s_slots[i].ucab, 0, slot_bytes);
    }

    if (!ps2GsVu1QueueUploadProgram() ||
        !ps2GsVu1QueueConfigureBuffers()) {
        ps2GsVu1QueueRelease();
        return false;
    }

    s_build_slot = 0u;
    s_pending = false;
    s_initialized = true;
    sysLogPrintf(LOG_NOTE,
        "GS VU1 queue: PATH1 A+D transport ready banks=%u+%u QW max_records=%u",
        PS2_GS_VU1_BUFFER_BASE_QW,
        PS2_GS_VU1_BUFFER_OFFSET_QW,
        PS2_GS_VU1_MAX_AD_REGISTERS);
    sysLogPrintf(LOG_NOTE,
        "GS VU1 queue: validation ordering FLUSHA->MSCAL->FLUSH active");
    ps2LogCheckpoint();
    return true;
#endif
}

extern "C" bool ps2GsVu1QueueEnabled(void)
{
#if defined(PERFECT_DARK_PS2_VU1_COLOR_BATCH)
    return s_initialized;
#else
    return false;
#endif
}

extern "C" bool ps2GsVu1QueueWaitIdle(void)
{
#if !defined(PERFECT_DARK_PS2_VU1_COLOR_BATCH)
    return true;
#else
    if (!s_pending) {
        return true;
    }
    if (!ps2GsVu1QueueWaitVifIdle()) {
        return false;
    }
    s_pending = false;
    return true;
#endif
}

extern "C" bool ps2GsVu1QueueSubmitAd(
    const struct Ps2GsPackedReg *prefix, uint32_t prefix_count,
    const struct Ps2GsPackedReg *records, uint32_t record_count,
    const struct Ps2GsPackedReg *suffix, uint32_t suffix_count)
{
#if !defined(PERFECT_DARK_PS2_VU1_COLOR_BATCH)
    (void)prefix;
    (void)prefix_count;
    (void)records;
    (void)record_count;
    (void)suffix;
    (void)suffix_count;
    return false;
#else
    struct Ps2GsVu1BatchLayout layout;
    if (!s_initialized || !records || record_count == 0u ||
        (prefix_count != 0u && !prefix) ||
        (suffix_count != 0u && !suffix) ||
        prefix_count > UINT32_MAX - record_count ||
        suffix_count > UINT32_MAX - prefix_count - record_count ||
        !ps2GsVu1PlanAdBatch(
            prefix_count + record_count + suffix_count, &layout)) {
        return false;
    }

    /*
     * Publish every earlier PATH3 state write first. FLUSHA in the VIF stream
     * then orders PATH1 after that state without a GS FINISH token.
     */
    if (!ps2GsNativeQueueSubmit()) {
        return false;
    }
    ps2GsNativeQueueBeginFrame();

    struct Ps2GsVu1QueueSlot *slot = &s_slots[s_build_slot];
    const uint32_t payload_dma_address =
        (uint32_t)(uintptr_t)slot->canonical +
        PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW * 16u;
    if (!ps2GsVu1BuildAdBatchStream(
            slot->ucab, PS2_GS_VU1_DMA_SLOT_QW, payload_dma_address,
            prefix, prefix_count, records, record_count,
            suffix, suffix_count, &layout)) {
        return false;
    }

    if (!ps2GsVu1QueueWaitVifIdle()) {
        return false;
    }
    dmaKit_send_chain_ucab(DMA_CHANNEL_VIF1, slot->ucab);
    s_pending = true;
    s_build_slot ^= 1u;
    return true;
#endif
}

extern "C" bool ps2GsVu1QueueSubmitColor(
    const struct Ps2GsPackedReg *prim, bool emit_prim,
    const struct Ps2GsColorVertex *vertices, uint32_t vertex_count)
{
    if (!vertices || vertex_count == 0u ||
        vertex_count > PS2_GS_VU1_MAX_COLOR_VERTICES ||
        vertex_count % 3u != 0u) {
        return false;
    }
    return ps2GsVu1QueueSubmitAd(
        emit_prim ? prim : NULL, emit_prim ? 1u : 0u,
        &vertices[0].rgbaq, vertex_count * 2u,
        NULL, 0u);
}
