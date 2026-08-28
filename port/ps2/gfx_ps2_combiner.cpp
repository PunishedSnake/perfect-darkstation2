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
    return (!f->opt_texture_edge || f->opt_alpha) &&
           (!f->opt_invisible || f->opt_alpha) && !f->opt_noise &&
           !f->opt_grayscale && !f->opt_blur &&
           (!f->opt_alpha_threshold || f->opt_alpha);
}

static bool ps2_is_tex01_lerp_cycle(const struct CCFeatures *f)
{
    return f->c[0][0][0] == SHADER_TEXEL1 &&
           f->c[0][0][1] == SHADER_TEXEL0 &&
           f->c[0][0][2] == SHADER_INPUT_1 &&
           f->c[0][0][3] == SHADER_TEXEL0;
}

static bool ps2_is_tex01_alpha_lerp_cycle(const struct CCFeatures *f)
{
    const uint8_t a = f->c[0][1][0];
    const uint8_t b = f->c[0][1][1];
    const uint8_t d = f->c[0][1][3];
    return (a == SHADER_TEXEL1 || a == SHADER_TEXEL1A) &&
           (b == SHADER_TEXEL0 || b == SHADER_TEXEL0A) &&
           f->c[0][1][2] == SHADER_INPUT_1 &&
           (d == SHADER_TEXEL0 || d == SHADER_TEXEL0A);
}

static bool ps2_cycle_multiplies_combined_by_input2(
    const struct CCFeatures *f, uint8_t channel)
{
    if (!f->do_multiply[1][channel]) {
        return false;
    }

    const uint8_t a = f->c[1][channel][0];
    const uint8_t c = f->c[1][channel][2];
    return (a == SHADER_COMBINED && c == SHADER_INPUT_2) ||
           (a == SHADER_INPUT_2 && c == SHADER_COMBINED);
}

static bool ps2_plan_opaque_tex01_lerp(const struct CCFeatures *f,
    struct Ps2CombinerPlan *plan)
{
    if (!f->opt_2cyc || f->opt_alpha || f->opt_texture_edge ||
        f->opt_invisible ||
        !ps2_is_tex01_lerp_cycle(f)) {
        return false;
    }

    if (ps2_channel_passes_combined(f, 0)) {
        plan->color_recipe = PS2_COLOR_TEX01_LERP_INPUT1;
    } else if (f->do_multiply[1][0]) {
        const uint8_t a = f->c[1][0][0];
        const uint8_t c = f->c[1][0][2];
        if ((a == SHADER_COMBINED && c == SHADER_INPUT_2) ||
            (a == SHADER_INPUT_2 && c == SHADER_COMBINED)) {
            plan->color_recipe =
                PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2;
        }
    }

    if (plan->color_recipe == PS2_COLOR_UNSUPPORTED) {
        return false;
    }

    plan->alpha_recipe = PS2_ALPHA_OPAQUE;
    plan->color_cycle = 1;
    plan->alpha_cycle = 0;
    plan->textured = true;
    plan->texture_alpha = false;
    plan->pass_graph = PS2_PASS_GRAPH_OPAQUE_TRILERP;
    plan->supported = true;
    return true;
}

static bool ps2_plan_alpha_tex01_lerp_modulate(
    const struct CCFeatures *f, struct Ps2CombinerPlan *plan)
{
    if (!f->opt_2cyc || !f->opt_alpha || f->opt_fog ||
        f->opt_texture_edge || f->opt_invisible ||
        !ps2_is_tex01_lerp_cycle(f) ||
        !ps2_is_tex01_alpha_lerp_cycle(f)) {
        return false;
    }

    for (uint8_t channel = 0; channel < 2u; ++channel) {
        if (!f->do_multiply[1][channel]) {
            return false;
        }
        const uint8_t a = f->c[1][channel][0];
        const uint8_t c = f->c[1][channel][2];
        if (!((a == SHADER_COMBINED && c == SHADER_INPUT_2) ||
              (a == SHADER_INPUT_2 && c == SHADER_COMBINED))) {
            return false;
        }
    }

    plan->color_recipe = PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2;
    plan->alpha_recipe = PS2_ALPHA_UNSUPPORTED;
    plan->color_cycle = 1;
    plan->alpha_cycle = 1;
    plan->textured = true;
    plan->texture_alpha = true;
    plan->pass_graph = PS2_PASS_GRAPH_ALPHA_TRILERP_MODULATE;
    /*
     * The complete tiled pass graph is available to opt-in hardware builds,
     * but the low-byte PSMCT32 -> PSMT8 shuffle remains a real-PS2 image A/B
     * gate. Keep normal builds explicit-unsupported until that proof exists.
     */
    plan->hardware_validation_required = true;
    plan->supported = false;
    return true;
}

static bool ps2_plan_tex01_lerp_independent_alpha(
    const struct CCFeatures *f, struct Ps2CombinerPlan *plan)
{
    if (!f->opt_2cyc || !f->opt_alpha ||
        f->opt_texture_edge || f->opt_invisible ||
        !ps2_is_tex01_lerp_cycle(f) ||
        !ps2_cycle_multiplies_combined_by_input2(f, 0u) ||
        !ps2_cycle_multiplies_combined_by_input2(f, 1u)) {
        return false;
    }

    if (f->do_single[0][1] &&
        f->c[0][1][3] == SHADER_INPUT_1) {
        plan->alpha_recipe = PS2_ALPHA_INPUT1_MUL_INPUT2;
    } else if (f->do_multiply[0][1]) {
        const uint8_t a = f->c[0][1][0];
        const uint8_t c = f->c[0][1][2];
        if ((ps2_shader_item_is_tex0_alpha(a) &&
             c == SHADER_INPUT_1) ||
            (a == SHADER_INPUT_1 &&
             ps2_shader_item_is_tex0_alpha(c))) {
            plan->alpha_recipe =
                PS2_ALPHA_TEX0_MUL_INPUT1_MUL_INPUT2;
        }
    }

    if (plan->alpha_recipe == PS2_ALPHA_UNSUPPORTED) {
        return false;
    }

    plan->color_recipe = PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2;
    plan->color_cycle = 1;
    plan->alpha_cycle = 1;
    plan->textured = true;
    plan->texture_alpha =
        plan->alpha_recipe == PS2_ALPHA_TEX0_MUL_INPUT1_MUL_INPUT2;
    plan->pass_graph = PS2_PASS_GRAPH_TRILERP_INDEPENDENT_ALPHA;
    plan->supported = true;
    return true;
}

static bool ps2_plan_custom21_texture_edge(
    const struct CCFeatures *f, struct Ps2CombinerPlan *plan)
{
    if (!f->opt_2cyc || !f->opt_alpha || !f->opt_texture_edge ||
        f->opt_invisible || !ps2_is_tex01_lerp_cycle(f) ||
        !ps2_cycle_multiplies_combined_by_input2(f, 0u) ||
        !ps2_channel_passes_combined(f, 1u)) {
        return false;
    }

    /* CUSTOM_21 alpha is SHADE + ENVIRONMENT in Fast3D input order. */
    if (f->c[0][1][0] != SHADER_1 ||
        f->c[0][1][1] != SHADER_0 ||
        f->c[0][1][2] != SHADER_INPUT_1 ||
        f->c[0][1][3] != SHADER_INPUT_2) {
        return false;
    }

    plan->color_recipe = PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2;
    plan->alpha_recipe = PS2_ALPHA_INPUT1_PLUS_INPUT2_EDGE;
    plan->color_cycle = 1;
    plan->alpha_cycle = 1;
    plan->textured = true;
    plan->texture_alpha = false;
    plan->pass_graph = PS2_PASS_GRAPH_TRILERP_INDEPENDENT_ALPHA;
    plan->supported = true;
    return true;
}

static bool ps2_is_input1_tex0_lerp_cycle(const struct CCFeatures *f)
{
    return f->c[0][0][0] == SHADER_TEXEL0 &&
           f->c[0][0][1] == SHADER_INPUT_1 &&
           f->c[0][0][2] == SHADER_INPUT_2 &&
           f->c[0][0][3] == SHADER_INPUT_1;
}

static bool ps2_plan_opaque_input1_tex0_lerp(const struct CCFeatures *f,
    struct Ps2CombinerPlan *plan)
{
    if (!f->opt_2cyc || f->opt_alpha || f->opt_texture_edge ||
        f->opt_invisible ||
        !ps2_is_input1_tex0_lerp_cycle(f)) {
        return false;
    }

    if (ps2_channel_passes_combined(f, 0)) {
        plan->color_recipe = PS2_COLOR_INPUT1_TEX0_LERP_INPUT2;
    } else if (f->do_multiply[1][0]) {
        const uint8_t a = f->c[1][0][0];
        const uint8_t c = f->c[1][0][2];
        if ((a == SHADER_COMBINED && c == SHADER_INPUT_3) ||
            (a == SHADER_INPUT_3 && c == SHADER_COMBINED)) {
            plan->color_recipe =
                PS2_COLOR_INPUT1_TEX0_LERP_INPUT2_MUL_INPUT3;
        }
    }

    if (plan->color_recipe == PS2_COLOR_UNSUPPORTED) {
        return false;
    }

    plan->alpha_recipe = PS2_ALPHA_OPAQUE;
    plan->color_cycle = 1;
    plan->alpha_cycle = 0;
    plan->textured = true;
    plan->texture_alpha = false;
    plan->pass_graph = PS2_PASS_GRAPH_OPAQUE_INPUT1_TEX0_LERP;
    plan->supported = true;
    return true;
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

    if (ps2_plan_opaque_tex01_lerp(f, plan)) {
        return true;
    }

    if (ps2_plan_custom21_texture_edge(f, plan)) {
        return true;
    }

    if (ps2_plan_tex01_lerp_independent_alpha(f, plan)) {
        return true;
    }

    if (ps2_plan_alpha_tex01_lerp_modulate(f, plan)) {
        return true;
    }

    if (ps2_plan_opaque_input1_tex0_lerp(f, plan)) {
        return true;
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
