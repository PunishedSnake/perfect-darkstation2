#ifndef PERFECT_DARK_PS2_GS_STATE_SHADOW_H
#define PERFECT_DARK_PS2_GS_STATE_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Logical register slots whose values survive across ordered PATH3 packets.
 * Geometry payload registers are intentionally excluded: only material/state
 * values which are safe to suppress when unchanged belong in this shadow.
 */
enum Ps2GsStateSlot {
    PS2_GS_STATE_TEST = 0,
    PS2_GS_STATE_ZBUF,
    PS2_GS_STATE_FRAME,
    PS2_GS_STATE_FBA,
    PS2_GS_STATE_PABE,
    PS2_GS_STATE_ALPHA,
    PS2_GS_STATE_FOGCOL,
    PS2_GS_STATE_CLAMP,
    PS2_GS_STATE_TEXA,
    PS2_GS_STATE_SCISSOR,
    PS2_GS_STATE_TEX0,
    PS2_GS_STATE_TEX1,
    PS2_GS_STATE_PRIM,
    PS2_GS_STATE_COUNT,
};

struct Ps2GsStateShadow {
    uint64_t value[PS2_GS_STATE_COUNT];
    uint32_t valid_mask;
    uint32_t emitted_writes;
    uint32_t suppressed_writes;
};

void ps2GsStateShadowReset(struct Ps2GsStateShadow *shadow);

/* Returns true when the register must be written. Does not mutate its value. */
bool ps2GsStateShadowNeedsWrite(struct Ps2GsStateShadow *shadow,
    enum Ps2GsStateSlot slot, uint64_t value);

/* Commit only after the corresponding A+D write has been reserved and built. */
void ps2GsStateShadowCommit(struct Ps2GsStateShadow *shadow,
    enum Ps2GsStateSlot slot, uint64_t value);

void ps2GsStateShadowInvalidate(struct Ps2GsStateShadow *shadow,
    enum Ps2GsStateSlot slot);
void ps2GsStateShadowInvalidateAll(struct Ps2GsStateShadow *shadow);

#ifdef __cplusplus
}
#endif

#endif
