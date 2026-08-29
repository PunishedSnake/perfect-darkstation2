#ifndef PERFECT_DARK_PC_GFX_PS2_COMBINER_H
#define PERFECT_DARK_PC_GFX_PS2_COMBINER_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx_cc.h"

enum Ps2ColorRecipe {
    PS2_COLOR_UNSUPPORTED = 0,
    PS2_COLOR_INPUT1,
    PS2_COLOR_TEX0,
    PS2_COLOR_TEX0_MUL_INPUT1,
    PS2_COLOR_TEX01_LERP_INPUT1,
    PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2,
    PS2_COLOR_INPUT1_TEX0_LERP_INPUT2,
    PS2_COLOR_INPUT1_TEX0_LERP_INPUT2_MUL_INPUT3,
    PS2_COLOR_INPUT2_INPUT1_LERP_TEX0,
};

enum Ps2AlphaRecipe {
    PS2_ALPHA_UNSUPPORTED = 0,
    PS2_ALPHA_OPAQUE,
    PS2_ALPHA_ZERO,
    PS2_ALPHA_ONE,
    PS2_ALPHA_INPUT1,
    PS2_ALPHA_TEX0,
    PS2_ALPHA_TEX0_MUL_INPUT1,
    PS2_ALPHA_INPUT1_MUL_INPUT2,
    PS2_ALPHA_TEX0_MUL_INPUT1_MUL_INPUT2,
    /* TEX_EDGE observes INPUT1 + INPUT2 only through its adjusted threshold. */
    PS2_ALPHA_INPUT1_PLUS_INPUT2_EDGE,
    /* INPUT2 * INPUT1 * (1 - INPUT1), reconstructed in a scalar target. */
    PS2_ALPHA_INPUT1_INV_INPUT1_MUL_INPUT2,
    /* INPUT3 + TEX0 * (INPUT1 - INPUT2), used by signed TEX_EDGE. */
    PS2_ALPHA_TEX0_MUL_INPUT1_MINUS_INPUT2_PLUS_INPUT3_EDGE,
    PS2_ALPHA_INPUT2_INPUT1_LERP_TEX0,
};

enum Ps2PassGraph {
    PS2_PASS_GRAPH_DIRECT = 0,
    PS2_PASS_GRAPH_OPAQUE_TRILERP,
    PS2_PASS_GRAPH_OPAQUE_INPUT1_TEX0_LERP,
    PS2_PASS_GRAPH_TRILERP_INDEPENDENT_ALPHA,
    PS2_PASS_GRAPH_ALPHA_TRILERP_MODULATE,
    PS2_PASS_GRAPH_TEX0_FACTOR_LERP,
};

struct Ps2CombinerPlan {
    bool supported;
    bool textured;
    bool texture_alpha;
    bool hardware_validation_required;
    uint8_t color_cycle;
    uint8_t alpha_cycle;
    enum Ps2ColorRecipe color_recipe;
    enum Ps2AlphaRecipe alpha_recipe;
    enum Ps2PassGraph pass_graph;
};

/*
 * Reduce an upstream Fast3D combiner to an exact GS recipe. Independent final
 * cycles collapse to one pass. Equations that require ordered GS passes carry
 * an explicit pass graph instead of overloading a scalar channel recipe.
 */
bool ps2GfxPlanCombiner(const struct CCFeatures *features,
    struct Ps2CombinerPlan *plan);

#endif
