#ifndef PERFECT_DARK_PS2_GS_CORE_H
#define PERFECT_DARK_PS2_GS_CORE_H

#include <stdbool.h>
#include <stdint.h>

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
};

enum Ps2GsDepthStorageMode {
    PS2_GS_PSMZ_32 = 0x00,
    PS2_GS_PSMZ_24 = 0x01,
    PS2_GS_PSMZ_16 = 0x02,
    PS2_GS_PSMZ_16S = 0x0a,
};

struct Ps2GsCreateInfo {
    int color_psm;
    int depth_psm;
    bool z_buffering;
    bool dithering;
};

typedef uint16_t Ps2GsTextureHandle;
#define PS2_GS_TEXTURE_INVALID ((Ps2GsTextureHandle)0)

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
void ps2GsCoreSetAlphaTest(bool enable, uint8_t reference);
void ps2GsCoreSetFog(bool enable, uint8_t r, uint8_t g, uint8_t b);
/* TEX0.TCC is texture-function state, independent of primitive alpha blending. */
void ps2GsCoreSetTextureAlpha(bool enable);
void ps2GsCoreSetTextureClamp(uint32_t cms, uint32_t cmt);

/*
 * Texture residency baseline.
 *
 * Handles are logical GS resources. The current implementation still uses
 * gsKit's monotonic VRAM allocation metadata, but pixel transport is submitted
 * through the project-owned asynchronous GIF IMAGE path. VRAM allocation and
 * successful upload are separate states, and upload failure is propagated to
 * the caller. The allocator/metadata dependency is transitional and may later
 * be replaced by an explicit residency cache without changing this public API.
 */
Ps2GsTextureHandle ps2GsCoreCreateTexture(void);
bool ps2GsCoreTextureExists(Ps2GsTextureHandle handle);
bool ps2GsCoreTextureReady(Ps2GsTextureHandle handle);
bool ps2GsCoreUploadTextureRgba32(Ps2GsTextureHandle handle,
    const uint8_t *rgba32, uint32_t width, uint32_t height);
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
