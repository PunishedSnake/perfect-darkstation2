#ifndef PERFECT_DARK_PS2_GS_CORE_H
#define PERFECT_DARK_PS2_GS_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "gs_alpha_equation.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hardware PSM values used by the current bootstrap. Keep gsKit names/types
 * out of the public device contract so Fast3D and platform clients do not grow
 * a dependency on the temporary transport implementation. */
enum Ps2GsPixelStorageMode {
    PS2_GS_PSM_CT32 = 0x00,
    PS2_GS_PSM_CT24 = 0x01,
    PS2_GS_PSM_CT16 = 0x02,
    PS2_GS_PSM_CT16S = 0x0a,
    PS2_GS_PSM_T8 = 0x13,
    PS2_GS_PSM_T4 = 0x14,
    PS2_GS_PSM_T8H = 0x1b,
};

enum Ps2GsDepthStorageMode {
    PS2_GS_PSMZ_32 = 0x00,
    PS2_GS_PSMZ_24 = 0x01,
    PS2_GS_PSMZ_16 = 0x02,
    PS2_GS_PSMZ_16S = 0x0a,
};

enum Ps2GsN64IntensityEncoding {
    PS2_GS_N64_IA4,
    PS2_GS_N64_IA8,
    PS2_GS_N64_I4,
    PS2_GS_N64_I8,
};

enum Ps2GsN64PaletteEncoding {
    PS2_GS_N64_PALETTE_RGBA16,
    PS2_GS_N64_PALETTE_IA16,
};

enum Ps2GsAlphaTestComparison {
    PS2_GS_ALPHA_TEST_GEQUAL = 0,
    PS2_GS_ALPHA_TEST_LEQUAL,
};

struct Ps2GsCreateInfo {
    int color_psm;
    int depth_psm;
    bool z_buffering;
    bool dithering;
};

typedef uint16_t Ps2GsTextureHandle;
#define PS2_GS_TEXTURE_INVALID ((Ps2GsTextureHandle)0)

typedef uint8_t Ps2GsRenderTargetHandle;
#define PS2_GS_RENDER_TARGET_DEFAULT ((Ps2GsRenderTargetHandle)0)

/*
 * Packet-ready register record.
 *
 * `value` is the 64-bit GS register payload and `reg` is the GIF register id.
 * The vertex records are final A+D records. `xyz2` may carry either XYZ2 or
 * XYZF2 because the register selector is part of the record itself; this keeps
 * fog from expanding the per-vertex transport representation.
 */
struct Ps2GsPackedReg {
    uint64_t value;
    uint64_t reg;
};

struct Ps2GsColorVertex {
    struct Ps2GsPackedReg rgbaq;
    struct Ps2GsPackedReg xyz2;
};

struct Ps2GsTexturedVertex {
    struct Ps2GsPackedReg rgbaq;
    struct Ps2GsPackedReg st;
    struct Ps2GsPackedReg xyz2;
};

bool ps2GsCoreInit(const struct Ps2GsCreateInfo *info);
bool ps2GsCoreIsReady(void);

int ps2GsCoreGetWidth(void);
int ps2GsCoreGetHeight(void);
int ps2GsCoreGetMode(void);
int ps2GsCoreGetRefreshRate(void);
int ps2GsCoreGetOffsetX(void);
int ps2GsCoreGetOffsetY(void);

/* Frame ownership. Submit does not wait; present owns the VSync dependency. */
void ps2GsCoreBeginFrame(void);
void ps2GsCoreSubmit(void);
void ps2GsCorePresent(void);

/* Render-target and GS state owned below the Fast3D compatibility adapter. */
void ps2GsCoreClear(bool clear_color, bool clear_depth);
void ps2GsCoreSetScissor(int x, int y, int width, int height);
void ps2GsCoreSetDepthMode(bool depth_test, bool depth_update, bool depth_compare);
void ps2GsCoreSetAlphaBlend(bool enable);
void ps2GsCoreSetAlphaBlendEquation(
    enum Ps2GsAlphaBlendEquation equation);
/* Mask every framebuffer lane while preserving depth test/write submission. */
void ps2GsCoreSetColorWrite(bool enable);
/* Preserve framebuffer alpha while RGB is accumulated by a multipass draw. */
void ps2GsCoreSetAlphaWrite(bool enable);
void ps2GsCoreSetAlphaTest(bool enable, uint8_t reference);
void ps2GsCoreSetAlphaTestComparison(bool enable, uint8_t reference,
    enum Ps2GsAlphaTestComparison comparison);
/* GS FBA forces the stored framebuffer alpha MSB for accepted fragments. */
void ps2GsCoreSetFramebufferAlphaForce(bool enable);
void ps2GsCoreSetFog(bool enable, uint8_t r, uint8_t g, uint8_t b);
/* TEX0.TCC is texture-function state, independent of primitive alpha blending. */
void ps2GsCoreSetTextureAlpha(bool enable);
void ps2GsCoreSetTextureClamp(uint32_t cms, uint32_t cmt);
/* Override selected axes with exact GS texel-space REGION_CLAMP bounds. */
void ps2GsCoreSetTextureRegionClamp(uint32_t cms, uint32_t cmt,
    bool region_s, uint16_t max_u, bool region_t, uint16_t max_v);

/*
 * Transient CT32 render targets use page-rounded, 8192-byte-aligned VRAM.
 * Offscreen targets intentionally disable Z until they are rebound to the
 * default draw buffer; they do not alias the screen-sized system Z buffer.
 */
Ps2GsRenderTargetHandle ps2GsCoreCreateRenderTarget(
    uint32_t width, uint32_t height);
bool ps2GsCoreBindRenderTarget(Ps2GsRenderTargetHandle handle);
void ps2GsCoreBindDefaultRenderTarget(void);
bool ps2GsCoreReleaseRenderTarget(Ps2GsRenderTargetHandle handle);
uint32_t ps2GsCoreGetRenderTargetWidth(Ps2GsRenderTargetHandle handle);
uint32_t ps2GsCoreGetRenderTargetHeight(Ps2GsRenderTargetHandle handle);

/*
 * Sample a completed CT32 target without copying it through EE memory. The
 * target must have received a successful clear or draw and may not be sampled
 * recursively while active. The first read after a target write inserts the
 * required GS texture-cache transition.
 */
bool ps2GsCoreDrawRenderTargetTriangles(Ps2GsRenderTargetHandle source,
    const struct Ps2GsTexturedVertex *vertices, uint32_t vertex_count,
    bool linear_filter);

/* Map the high alpha byte through an identity CT32 CLUT as RGBA intensity. */
bool ps2GsCoreDrawRenderTargetAlphaTriangles(
    Ps2GsRenderTargetHandle source,
    const struct Ps2GsTexturedVertex *vertices, uint32_t vertex_count,
    bool linear_filter);

/*
 * Copy the red byte of a completed CT32 source into the alpha byte of the
 * active CT32 transient target. RGB is preserved and source/destination must
 * be distinct, initialized targets with identical dimensions.
 */
bool ps2GsCoreBlitRenderTargetRedToActiveAlpha(
    Ps2GsRenderTargetHandle source);

/*
 * Texture residency. Handles are logical GS resources backed by the native
 * reclaimable VRAM pool. Allocation, upload and retirement are transactional.
 */
Ps2GsTextureHandle ps2GsCoreCreateTexture(void);
bool ps2GsCoreTextureExists(Ps2GsTextureHandle handle);
bool ps2GsCoreTextureReady(Ps2GsTextureHandle handle);
bool ps2GsCoreUploadTextureRgba32(Ps2GsTextureHandle handle,
    const uint8_t *rgba32, uint32_t width, uint32_t height,
    bool mirror_s, bool mirror_t);
bool ps2GsCoreUploadTextureN64Rgba16(Ps2GsTextureHandle handle,
    const uint8_t *rgba5551_be, uint32_t width, uint32_t height,
    bool mirror_s, bool mirror_t);
/* IA16 expands directly into CT32 IMAGE staging without the generic importer. */
bool ps2GsCoreUploadTextureN64Ia16(Ps2GsTextureHandle handle,
    const uint8_t *ia16, uint32_t width, uint32_t height,
    bool mirror_s, bool mirror_t);
/* Indexed texels and their exact CT16/CT32 TLUT become one atomic residency. */
bool ps2GsCoreUploadTextureN64Ci(Ps2GsTextureHandle handle,
    const uint8_t *indices, uint32_t width, uint32_t height,
    uint8_t index_bits, const uint16_t *palette,
    uint32_t palette_count, enum Ps2GsN64PaletteEncoding palette_encoding,
    bool mirror_s, bool mirror_t);
/* IA4/IA8/I4/I8 use immutable shared CT32 CSM1 palettes. */
bool ps2GsCoreUploadTextureN64Intensity(Ps2GsTextureHandle handle,
    const uint8_t *texels, uint32_t width, uint32_t height,
    enum Ps2GsN64IntensityEncoding encoding,
    bool mirror_s, bool mirror_t);
void ps2GsCoreSetTextureFilter(Ps2GsTextureHandle handle, bool linear_filter);
void ps2GsCoreReleaseTexture(Ps2GsTextureHandle handle);

/* Packet-ready primitive submission. No Fast3D or gsKit type crosses here. */
void ps2GsCoreDrawColorTriangles(const struct Ps2GsColorVertex *vertices,
    uint32_t vertex_count);
void ps2GsCoreDrawTexturedTriangles(Ps2GsTextureHandle texture,
    const struct Ps2GsTexturedVertex *vertices, uint32_t vertex_count);

#ifdef __cplusplus
}
#endif

#endif
