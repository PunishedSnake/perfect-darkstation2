#ifndef PERFECT_DARK_PS2_GS_NATIVE_QUEUE_H
#define PERFECT_DARK_PS2_GS_NATIVE_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include <gsKit.h>

#include "gs_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Project-owned PATH3 command arena.
 *
 * PACKED A+D draw/state streams use double-buffered UCAB arenas. Texture IMAGE
 * uploads use a separate two-slot source-chain staging path so the EE may copy
 * and prepare upload N+1 while GIF DMA still owns upload N. Normal submission
 * never emits GS FINISH; the explicit dependency fence below is reserved for
 * PCRTC publication and texture-residency hazards.
 *
 * Buffer sizing is a measured policy knob, not part of the public Fast3D API.
 */
bool ps2GsNativeQueueInit(uint32_t qwords_per_buffer);
void ps2GsNativeQueueBeginFrame(void);

/*
 * Reserve one GIF PACKED A+D packet containing exactly reg_count writes.
 * Returned storage is packet-ready and 16-byte stepped; callers fill the
 * project-owned value/register records directly with no later repack/copy.
 */
struct Ps2GsPackedReg *ps2GsNativeQueueReserveAd(uint32_t reg_count);

/* Submit the active draw/state arena to GIF DMA. Never waits for GS FINISH/VSync. */
bool ps2GsNativeQueueSubmit(void);

/*
 * Submit one native GS FINISH token after all earlier PATH3 traffic and wait
 * for it. This is a full GS dependency fence, not merely GIF DMA completion.
 */
bool ps2GsNativeQueueWaitGs(void);

/*
 * Wait for VSync, publish the completed framebuffer through PCRTC and submit
 * native FRAME/SCISSOR state for the next draw buffer. The state packet remains
 * asynchronous; the next GIF claimant observes channel ownership ordering.
 */
bool ps2GsNativeQueuePresent(GSGLOBAL *gs);

/*
 * Native host->local IMAGE upload used by gs_core's transitional GSTEXTURE
 * metadata. The function copies source pixels into one of two persistent EE
 * staging slots, submits a GIF source chain and returns after DMA submission,
 * not completion. Subsequent PATH3 submissions serialize on the GIF channel,
 * preserving upload -> TEXFLUSH -> dependent draw ordering.
 *
 * Source encoding is explicit because CT16 image texels and logical TLUT
 * entries have different byte order, while CI4 additionally needs nibble
 * reversal before GS PSMT4 upload. Conversion is fused into DMA staging.
 */
enum Ps2GsNativeUploadEncoding {
    PS2_GS_NATIVE_UPLOAD_RGBA32,
    PS2_GS_NATIVE_UPLOAD_N64_RGBA16,
    PS2_GS_NATIVE_UPLOAD_T8,
    PS2_GS_NATIVE_UPLOAD_N64_CI4,
    PS2_GS_NATIVE_UPLOAD_N64_RGBA16_CLUT,
};

bool ps2GsNativeQueueUploadTexture(GSGLOBAL *gs, GSTEXTURE *texture,
    enum Ps2GsNativeUploadEncoding encoding);

uint32_t ps2GsNativeQueueUsedQwords(void);
uint32_t ps2GsNativeQueueCapacityQwords(void);
bool ps2GsNativeQueueOverflowed(void);

#ifdef __cplusplus
}
#endif

#endif
