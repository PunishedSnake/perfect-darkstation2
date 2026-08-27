#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <malloc.h>

#include <dmaKit.h>
#include <kernel.h>

#include "gs_native_queue.h"
#include "gs_texture_convert.h"
#include "log_ps2.h"
#include "system.h"

#define PS2_GIF_PACKED 0u
#define PS2_GIF_REG_AD 0x0eu
#define PS2_GIF_MAX_NLOOP 0x7fffu

#define PS2_GS_UPLOAD_SLOTS 2u
#define PS2_GS_UPLOAD_MAX_BYTES (1024u * 1024u * 4u)
#define PS2_GS_UPLOAD_MAX_QW ((PS2_GS_UPLOAD_MAX_BYTES + 15u) / 16u)
#define PS2_GS_UPLOAD_MAX_CHUNKS \
    ((PS2_GS_UPLOAD_MAX_QW + GS_GIF_BLOCKSIZE - 1u) / GS_GIF_BLOCKSIZE)
/* Initial CNT packet: 6 QW, each IMAGE chunk: 3 QW, END+TEXFLUSH: 3 QW. */
#define PS2_GS_UPLOAD_CHAIN_QW (9u + PS2_GS_UPLOAD_MAX_CHUNKS * 3u)

struct Ps2GsNativeArena {
    void *canonical;
    uint64_t *ucab;
    uint32_t capacity_qw;
    uint32_t used_qw;
    bool overflowed;
};

struct Ps2GsNativeUploadSlot {
    uint8_t *payload;
    uint32_t capacity_bytes;
    uint64_t *chain;
};

static struct Ps2GsNativeArena s_arenas[2];
static struct Ps2GsNativeUploadSlot s_upload_slots[PS2_GS_UPLOAD_SLOTS];
static void *s_finish_canonical;
static uint64_t *s_finish_ucab;
static void *s_present_canonical;
static uint64_t *s_present_ucab;
static uint32_t s_build_arena;
static uint32_t s_upload_slot;
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

static void ps2GsNativeQueueReleaseInitMemory(void)
{
    for (uint32_t i = 0; i < 2; ++i) {
        free(s_arenas[i].canonical);
        s_arenas[i].canonical = NULL;
        s_arenas[i].ucab = NULL;
    }

    for (uint32_t i = 0; i < PS2_GS_UPLOAD_SLOTS; ++i) {
        free(s_upload_slots[i].payload);
        free(s_upload_slots[i].chain);
        s_upload_slots[i].payload = NULL;
        s_upload_slots[i].chain = NULL;
        s_upload_slots[i].capacity_bytes = 0;
    }

    free(s_finish_canonical);
    s_finish_canonical = NULL;
    s_finish_ucab = NULL;

    free(s_present_canonical);
    s_present_canonical = NULL;
    s_present_ucab = NULL;
}

static bool ps2GsNativeEnsureUploadCapacity(struct Ps2GsNativeUploadSlot *slot,
    uint32_t required_bytes)
{
    if (slot->capacity_bytes >= required_bytes) {
        return true;
    }

    /*
     * Cache-line alignment is intentional only for this EE staging allocation:
     * the payload is explicitly written by the CPU then range-writebacked for
     * GIF DMA. DMA itself only requires qword-granular payload here.
     */
    uint8_t *replacement = (uint8_t *)memalign(64, required_bytes);
    if (!replacement) {
        return false;
    }

    free(slot->payload);
    slot->payload = replacement;
    slot->capacity_bytes = required_bytes;
    return true;
}

static uint32_t ps2GsNativeBuildUploadChain(struct Ps2GsNativeUploadSlot *slot,
    uint32_t width, uint32_t height, uint32_t vram, uint32_t tbw,
    uint32_t psm, uint32_t payload_qw)
{
    uint64_t *p = slot->chain;
    uint8_t *payload = slot->payload;
    uint32_t remaining_qw = payload_qw;

    *p++ = DMA_TAG(5, 0, DMA_CNT, 0, 0, 0);
    *p++ = 0;

    *p++ = GIF_TAG(4, 0, 0, 0, GSKIT_GIF_FLG_PACKED, 1);
    *p++ = GIF_AD;

    *p++ = GS_SETREG_BITBLTBUF(0, 0, 0, vram / 256u, tbw, psm);
    *p++ = GS_BITBLTBUF;
    *p++ = GS_SETREG_TRXPOS(0, 0, 0, 0, 0);
    *p++ = GS_TRXPOS;
    *p++ = GS_SETREG_TRXREG(width, height);
    *p++ = GS_TRXREG;
    *p++ = GS_SETREG_TRXDIR(0);
    *p++ = GS_TRXDIR;

    while (remaining_qw > 0) {
        const uint32_t chunk_qw = remaining_qw > GS_GIF_BLOCKSIZE
            ? GS_GIF_BLOCKSIZE : remaining_qw;

        *p++ = DMA_TAG(1, 0, DMA_CNT, 0, 0, 0);
        *p++ = 0;
        *p++ = GIF_TAG(chunk_qw, 0, 0, 0, GSKIT_GIF_FLG_IMAGE, 0);
        *p++ = 0;
        *p++ = DMA_TAG(chunk_qw, 1, DMA_REF, 0,
            (uint32_t)(uintptr_t)payload, 0);
        *p++ = 0;

        payload += (size_t)chunk_qw * 16u;
        remaining_qw -= chunk_qw;
    }

    /*
     * TEXFLUSH is ordered after IMAGE data in the same PATH3 chain. A later
     * draw chain therefore does not need a GS FINISH just to observe the newly
     * uploaded texture.
     */
    *p++ = DMA_TAG(2, 0, DMA_END, 0, 0, 0);
    *p++ = 0;
    *p++ = GIF_TAG(1, 1, 0, 0, GSKIT_GIF_FLG_PACKED, 1);
    *p++ = GIF_AD;
    *p++ = 0;
    *p++ = GS_TEXFLUSH;

    return (uint32_t)((p - slot->chain) / 2);
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
    memset(s_upload_slots, 0, sizeof(s_upload_slots));

    for (uint32_t i = 0; i < 2; ++i) {
        void *canonical = memalign(64, bytes);
        if (!canonical) {
            sysLogPrintf(LOG_ERROR,
                "GS native queue: allocation failed arena=%u bytes=%u",
                i, (unsigned int)bytes);
            ps2GsNativeQueueReleaseInitMemory();
            return false;
        }

        memset(canonical, 0, bytes);
        SyncDCache(canonical, (uint8_t *)canonical + bytes - 1);

        s_arenas[i].canonical = canonical;
        s_arenas[i].ucab = (uint64_t *)UCAB_SEG(canonical);
        s_arenas[i].capacity_qw = qwords_per_buffer;
    }

    const size_t upload_chain_bytes = (size_t)PS2_GS_UPLOAD_CHAIN_QW * 16u;
    for (uint32_t i = 0; i < PS2_GS_UPLOAD_SLOTS; ++i) {
        s_upload_slots[i].chain = (uint64_t *)memalign(64, upload_chain_bytes);
        if (!s_upload_slots[i].chain) {
            sysLogPrintf(LOG_ERROR,
                "GS native queue: upload-chain allocation failed slot=%u bytes=%u",
                i, (unsigned int)upload_chain_bytes);
            ps2GsNativeQueueReleaseInitMemory();
            return false;
        }
        memset(s_upload_slots[i].chain, 0, upload_chain_bytes);
    }

    s_finish_canonical = memalign(64, 64);
    if (!s_finish_canonical) {
        sysLogPrintf(LOG_ERROR,
            "GS native queue: FINISH-packet allocation failed");
        ps2GsNativeQueueReleaseInitMemory();
        return false;
    }
    memset(s_finish_canonical, 0, 64);
    SyncDCache(s_finish_canonical,
        (uint8_t *)s_finish_canonical + 63u);
    s_finish_ucab = (uint64_t *)UCAB_SEG(s_finish_canonical);
    s_finish_ucab[0] = ps2GifPackedAdTag(1);
    s_finish_ucab[1] = PS2_GIF_REG_AD;
    s_finish_ucab[2] = 0;
    s_finish_ucab[3] = GS_FINISH;

    s_present_canonical = memalign(64, 128);
    if (!s_present_canonical) {
        sysLogPrintf(LOG_ERROR,
            "GS native queue: present-packet allocation failed");
        ps2GsNativeQueueReleaseInitMemory();
        return false;
    }
    memset(s_present_canonical, 0, 128);
    SyncDCache(s_present_canonical,
        (uint8_t *)s_present_canonical + 127u);
    s_present_ucab = (uint64_t *)UCAB_SEG(s_present_canonical);

    s_build_arena = 0;
    s_upload_slot = 0;
    s_initialized = true;

    sysLogPrintf(LOG_NOTE,
        "GS native queue: 2x%u QW (%u KiB each) UCAB command arenas",
        qwords_per_buffer, (unsigned int)(bytes / 1024u));
    sysLogPrintf(LOG_NOTE,
        "GS native queue: %u-slot staged IMAGE uploader, max=%u KiB/texture",
        PS2_GS_UPLOAD_SLOTS, PS2_GS_UPLOAD_MAX_BYTES / 1024u);
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

extern "C" bool ps2GsNativeQueueUploadTexture(GSGLOBAL *gs,
    GSTEXTURE *texture, enum Ps2GsNativeUploadEncoding encoding)
{
    (void)gs;

    if (!s_initialized || !texture || !texture->Mem || texture->Width == 0 ||
        texture->Height == 0 || texture->Width > 1024 || texture->Height > 1024) {
        return false;
    }

    const bool valid_contract =
        (texture->PSM == GS_PSM_CT32 &&
            encoding == PS2_GS_NATIVE_UPLOAD_RGBA32) ||
        (texture->PSM == GS_PSM_CT16 &&
            (encoding == PS2_GS_NATIVE_UPLOAD_N64_RGBA16 ||
             encoding == PS2_GS_NATIVE_UPLOAD_N64_RGBA16_CLUT)) ||
        (texture->PSM == GS_PSM_T8 &&
            encoding == PS2_GS_NATIVE_UPLOAD_T8) ||
        (texture->PSM == GS_PSM_T4 &&
            encoding == PS2_GS_NATIVE_UPLOAD_N64_T4);
    if (!valid_contract) {
        sysLogPrintf(LOG_ERROR,
            "GS native queue: invalid IMAGE source contract psm=0x%x encoding=%d",
            texture->PSM, (int)encoding);
        return false;
    }
    if ((texture->Vram & 0xffu) != 0) {
        sysLogPrintf(LOG_ERROR,
            "GS native queue: texture VRAM base is not 256-byte aligned: %08x",
            texture->Vram);
        return false;
    }

    const uint64_t texel_count64 =
        (uint64_t)texture->Width * (uint64_t)texture->Height;
    uint64_t source_bytes64;
    if (texture->PSM == GS_PSM_CT32) {
        source_bytes64 = texel_count64 * 4u;
    } else if (texture->PSM == GS_PSM_CT16) {
        source_bytes64 = texel_count64 * 2u;
    } else if (texture->PSM == GS_PSM_T8) {
        source_bytes64 = texel_count64;
    } else {
        source_bytes64 = (texel_count64 + 1u) / 2u;
    }
    if (source_bytes64 == 0u || source_bytes64 > UINT32_MAX) {
        return false;
    }
    const uint32_t source_bytes = (uint32_t)source_bytes64;
    const uint32_t payload_bytes = (source_bytes + 15u) & ~15u;
    const uint32_t payload_qw = payload_bytes / 16u;
    if (payload_bytes == 0 || payload_bytes > PS2_GS_UPLOAD_MAX_BYTES) {
        return false;
    }

    struct Ps2GsNativeUploadSlot *slot = &s_upload_slots[s_upload_slot];
    if (!ps2GsNativeEnsureUploadCapacity(slot, payload_bytes)) {
        sysLogPrintf(LOG_ERROR,
            "GS native queue: upload staging allocation failed slot=%u bytes=%u",
            s_upload_slot, payload_bytes);
        return false;
    }

    /*
     * Stage before claiming GIF ownership. The alternating slot is no longer
     * referenced by DMA: every later GIF submission waits for the immediately
     * preceding one, so a slot reused two uploads later is already consumer-free.
     */
    if (encoding == PS2_GS_NATIVE_UPLOAD_N64_RGBA16) {
        if (!ps2GsConvertN64Rgba16ToGsCt16(
                (const uint8_t *)texture->Mem, slot->payload,
                texture->Width * texture->Height)) {
            return false;
        }
    } else if (encoding == PS2_GS_NATIVE_UPLOAD_N64_RGBA16_CLUT) {
        if (!ps2GsConvertN64Rgba16PaletteToGsCt16(
                (const uint16_t *)texture->Mem, slot->payload,
                texture->Width * texture->Height)) {
            return false;
        }
    } else if (encoding == PS2_GS_NATIVE_UPLOAD_N64_T4) {
        if (!ps2GsConvertN64Ci4ToGsT4(
                (const uint8_t *)texture->Mem, slot->payload,
                source_bytes)) {
            return false;
        }
    } else {
        memcpy(slot->payload, texture->Mem, source_bytes);
    }
    if (payload_bytes > source_bytes) {
        memset(slot->payload + source_bytes, 0, payload_bytes - source_bytes);
    }
    SyncDCache(slot->payload, slot->payload + payload_bytes - 1u);

    const bool indexed = texture->PSM == GS_PSM_T4 ||
        texture->PSM == GS_PSM_T8;
    texture->TBW = ps2GsTextureBufferWidth(texture->Width, indexed);
    const uint32_t chain_qw = ps2GsNativeBuildUploadChain(slot,
        texture->Width, texture->Height, texture->Vram, texture->TBW,
        texture->PSM, payload_qw);
    if (chain_qw == 0 || chain_qw > PS2_GS_UPLOAD_CHAIN_QW) {
        return false;
    }

    /*
     * submit early, wait late: CPU staging and chain construction happen before
     * this ownership wait, allowing them to overlap the previous PATH3 DMA.
     * Return immediately after submission; the next GIF claimant performs the
     * dependency wait instead of forcing completion here.
     */
    if (dmaKit_wait(DMA_CHANNEL_GIF, 0) < 0) {
        return false;
    }
    dmaKit_send_chain(DMA_CHANNEL_GIF, slot->chain, chain_qw);

    s_upload_slot ^= 1u;
    return true;
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
     * to claim it. Texture IMAGE transfers use the same rule, so their TEXFLUSH
     * is ordered before any dependent draw chain submitted here.
     */
    if (dmaKit_wait(DMA_CHANNEL_GIF, 0) < 0) {
        return false;
    }

    dmaKit_send_ucab(DMA_CHANNEL_GIF, arena->ucab, arena->used_qw);
    s_build_arena ^= 1u;
    return true;
}

extern "C" bool ps2GsNativeQueueWaitGs(void)
{
    if (!s_initialized || !s_finish_ucab) {
        return false;
    }

    /*
     * First acquire the GIF channel so FINISH is ordered after every earlier
     * draw/IMAGE chain. CSR.FINISH is write-one-to-clear; clear the prior event
     * before submitting this token, then distinguish GIF completion from actual
     * GS completion by polling the privileged FINISH bit.
     */
    if (dmaKit_wait(DMA_CHANNEL_GIF, 0) < 0) {
        return false;
    }
    GS_SETREG_CSR_FINISH(1);
    dmaKit_send_ucab(DMA_CHANNEL_GIF, s_finish_ucab, 2);
    if (dmaKit_wait(DMA_CHANNEL_GIF, 0) < 0) {
        return false;
    }
    while (!(GS_CSR_FINISH)) {
    }
    return true;
}

extern "C" bool ps2GsNativeQueuePresent(GSGLOBAL *gs)
{
    if (!s_initialized || !s_present_ucab || !gs ||
        gs->Width <= 0 || gs->Height <= 0) {
        return false;
    }

    /* Own the tiny dynamic packet before rewriting its UCAB storage. */
    if (dmaKit_wait(DMA_CHANNEL_GIF, 0) < 0) {
        return false;
    }

    /* CSR.VSINT is write-one-to-clear. Wait for the next scanout boundary. */
    *GS_CSR = *GS_CSR & 8u;
    while (!(*GS_CSR & 8u)) {
    }

    if (gs->DoubleBuffering == GS_SETTING_ON) {
        GS_SET_DISPFB2(
            gs->ScreenBuffer[gs->ActiveBuffer & 1u] / 8192u,
            gs->Width / 64u,
            gs->PSM,
            0,
            0);
        gs->ActiveBuffer ^= 1u;
    }

    uint64_t *p = s_present_ucab;
    *p++ = ps2GifPackedAdTag(4);
    *p++ = PS2_GIF_REG_AD;
    *p++ = GS_SETREG_SCISSOR(0, gs->Width - 1, 0, gs->Height - 1);
    *p++ = GS_SCISSOR_1;
    *p++ = GS_SETREG_FRAME_1(
        gs->ScreenBuffer[gs->ActiveBuffer & 1u] / 8192u,
        gs->Width / 64u,
        gs->PSM,
        0);
    *p++ = GS_FRAME_1;
    *p++ = GS_SETREG_SCISSOR(0, gs->Width - 1, 0, gs->Height - 1);
    *p++ = GS_SCISSOR_2;
    *p++ = GS_SETREG_FRAME_1(
        gs->ScreenBuffer[gs->ActiveBuffer & 1u] / 8192u,
        gs->Width / 64u,
        gs->PSM,
        0);
    *p++ = GS_FRAME_2;

    dmaKit_send_ucab(DMA_CHANNEL_GIF, s_present_ucab, 5);
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
