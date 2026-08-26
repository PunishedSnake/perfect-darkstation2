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
};

enum Ps2AlphaRecipe {
    PS2_ALPHA_UNSUPPORTED = 0,
    PS2_ALPHA_OPAQUE,
    PS2_ALPHA_ZERO,
    PS2_ALPHA_ONE,
    PS2_ALPHA_INPUT1,
    PS2_ALPHA_TEX0,
    PS2_ALPHA_TEX0_MUL_INPUT1,
};

struct Ps2CombinerPlan {
    bool supported;
    bool textured;
    bool texture_alpha;
    uint8_t color_cycle;
    uint8_t alpha_cycle;
    enum Ps2ColorRecipe color_recipe;
    enum Ps2AlphaRecipe alpha_recipe;
};

/*
 * Reduce an upstream Fast3D combiner to an exact GS recipe. Independent final
 * cycles collapse to one pass; the explicitly classified opaque TEXEL0/TEXEL1
 * interpolation recipes carry a two-pass execution plan in color_recipe.
 */
bool ps2GfxPlanCombiner(const struct CCFeatures *features,
    struct Ps2CombinerPlan *plan);

#endif
