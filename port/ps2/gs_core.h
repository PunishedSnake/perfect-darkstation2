#ifndef PERFECT_DARK_PS2_GS_CORE_H
#define PERFECT_DARK_PS2_GS_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include <gsKit.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GS device boundary for the PS2 port.
 *
 * Fast3D is a client of this layer, not its owner. The current implementation
 * still uses gsKit for screen setup, queue storage and presentation while the
 * port is brought up. Callers outside gs_core must not depend on that transport
 * choice. The legacy GSGLOBAL accessor exists only to migrate the existing
 * Fast3D adapter incrementally without changing rendering semantics in one
 * giant patch.
 */
struct Ps2GsCreateInfo {
    int color_psm;
    int depth_psm;
    bool z_buffering;
    bool dithering;
};

bool ps2GsCoreInit(const struct Ps2GsCreateInfo *info);
GSGLOBAL *ps2GsCoreGetLegacyGlobal(void);

int ps2GsCoreGetWidth(void);
int ps2GsCoreGetHeight(void);
int ps2GsCoreGetMode(void);

/* Frame ownership. Submit does not wait; present owns the VSync dependency. */
void ps2GsCoreBeginFrame(void);
void ps2GsCoreSubmit(void);
void ps2GsCorePresent(void);

/* Render-target and GS state owned below the Fast3D compatibility adapter. */
void ps2GsCoreClear(bool clear_color, bool clear_depth);
void ps2GsCoreSetScissor(int x, int y, int width, int height);
void ps2GsCoreSetDepthMode(bool depth_test, bool depth_update, bool depth_compare);
void ps2GsCoreSetAlphaBlend(bool enable);
void ps2GsCoreSetTextureClamp(uint32_t cms, uint32_t cmt);

#ifdef __cplusplus
}
#endif

#endif
