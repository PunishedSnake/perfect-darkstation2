#include "gfx_ps2_combiner.h"

#include <string.h>

static bool ps2_channel_references_combined(const struct CCFeatures *f,
    uint8_t channel)
{
    for (uint8_t term = 0; term < 4; ++term) {
        if (f->c[1][channel][term] == SHADER_COMBINED) {
            return true;
        }
    }
    return false;
}

static bool ps2_channel_passes_combined(const struct CCFeatures *f,
    uint8_t channel)
{
    if (f->do_single[1][channel] &&
        f->c[1][channel][3] == SHADER_COMBINED) {
        return true;
    }

    if (!f->do_multiply[1][channel]) {
        return false;
    }

    const uint8_t a = f->c[1][channel][0];
    const uint8_t c = f->c[1][channel][2];
    return (a == SHADER_COMBINED && c == SHADER_1) ||
           (a == SHADER_1 && c == SHADER_COMBINED);
}

static int ps2_effective_cycle(const struct CCFeatures *f, uint8_t channel)
{
    if (!f->opt_2cyc) {
        return 0;
    }

    if (!ps2_channel_references_combined(f, channel)) {
        return 1;
    }

    if (ps2_channel_passes_combined(f, channel)) {
        return 0;
    }

    return -1;
}

static bool ps2_shader_item_is_tex0_alpha(uint8_t item)
{
    /*
     * gfx_generate_cc() encodes G_ACMUX_TEXEL0 as SHADER_TEXEL0 in
     * the alpha half. SHADER_TEXEL0A represents the same scalar when texture
     * alpha enters through the RGB combiner.
     */
    return item == SHADER_TEXEL0 || item == SHADER_TEXEL0A;
}

static enum Ps2ColorRecipe ps2_classify_color_recipe(
    const struct CCFeatures *f, uint8_t cycle)
{
    if (f->do_single[cycle][0]) {
        if (f->c[cycle][0][3] == SHADER_INPUT_1) {
            return PS2_COLOR_INPUT1;
        }
        if (f->c[cycle][0][3] == SHADER_TEXEL0) {
            return PS2_COLOR_TEX0;
        }
    }

    if (f->do_multiply[cycle][0]) {
        const uint8_t a = f->c[cycle][0][0];
        const uint8_t c = f->c[cycle][0][2];
        if ((a == SHADER_TEXEL0 && c == SHADER_INPUT_1) ||
            (a == SHADER_INPUT_1 && c == SHADER_TEXEL0)) {
            return PS2_COLOR_TEX0_MUL_INPUT1;
        }
    }

    return PS2_COLOR_UNSUPPORTED;
}

static enum Ps2AlphaRecipe ps2_classify_alpha_recipe(
    const struct CCFeatures *f, uint8_t cycle)
{
    if (!f->opt_alpha) {
        return PS2_ALPHA_OPAQUE;
    }

    if (f->do_single[cycle][1]) {
        const uint8_t d = f->c[cycle][1][3];
        if (d == SHADER_0) {
            return PS2_ALPHA_ZERO;
        }
        if (d == SHADER_1) {
            return PS2_ALPHA_ONE;
        }
        if (d == SHADER_INPUT_1) {
            return PS2_ALPHA_INPUT1;
        }
        if (ps2_shader_item_is_tex0_alpha(d)) {
            return PS2_ALPHA_TEX0;
        }
    }

    if (f->do_multiply[cycle][1]) {
        const uint8_t a = f->c[cycle][1][0];
        const uint8_t c = f->c[cycle][1][2];
        if ((ps2_shader_item_is_tex0_alpha(a) && c == SHADER_INPUT_1) ||
            (a == SHADER_INPUT_1 && ps2_shader_item_is_tex0_alpha(c))) {
            return PS2_ALPHA_TEX0_MUL_INPUT1;
        }
    }

    return PS2_ALPHA_UNSUPPORTED;
}

static bool ps2_color_recipe_textured(enum Ps2ColorRecipe recipe)
{
    return recipe == PS2_COLOR_TEX0 || recipe == PS2_COLOR_TEX0_MUL_INPUT1;
}

static bool ps2_alpha_recipe_textured(enum Ps2AlphaRecipe recipe)
{
    return recipe == PS2_ALPHA_TEX0 || recipe == PS2_ALPHA_TEX0_MUL_INPUT1;
}

static bool ps2_common_options_supported(const struct CCFeatures *f)
{
    /* Alpha threshold and fog already have exact fixed-function mappings. */
    return !f->opt_texture_edge && !f->opt_noise &&
           !f->opt_invisible && !f->opt_grayscale && !f->opt_blur &&
           (!f->opt_alpha_threshold || f->opt_alpha);
}

bool ps2GfxPlanCombiner(const struct CCFeatures *f,
    struct Ps2CombinerPlan *plan)
{
    if (!f || !plan) {
        return false;
    }

    memset(plan, 0, sizeof(*plan));
    plan->color_recipe = PS2_COLOR_UNSUPPORTED;
    plan->alpha_recipe = PS2_ALPHA_UNSUPPORTED;

    if (!ps2_common_options_supported(f)) {
        return false;
    }

    const int color_cycle = ps2_effective_cycle(f, 0);
    const int alpha_cycle = f->opt_alpha ? ps2_effective_cycle(f, 1) : 0;
    if (color_cycle < 0 || alpha_cycle < 0) {
        return false;
    }

    plan->color_cycle = (uint8_t)color_cycle;
    plan->alpha_cycle = (uint8_t)alpha_cycle;
    plan->color_recipe = ps2_classify_color_recipe(f, plan->color_cycle);
    plan->alpha_recipe = ps2_classify_alpha_recipe(f, plan->alpha_cycle);
    if (plan->color_recipe == PS2_COLOR_UNSUPPORTED ||
        plan->alpha_recipe == PS2_ALPHA_UNSUPPORTED) {
        return false;
    }

    const bool color_textured = ps2_color_recipe_textured(plan->color_recipe);
    const bool alpha_textured = ps2_alpha_recipe_textured(plan->alpha_recipe);

    /* GS MODULATE cannot sample alpha without also applying texture RGB. */
    if (alpha_textured && !color_textured) {
        return false;
    }

    plan->textured = color_textured;
    plan->texture_alpha = alpha_textured;
    plan->supported = true;
    return true;
}
