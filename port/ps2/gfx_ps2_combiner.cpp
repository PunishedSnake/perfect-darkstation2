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

static bool ps2_shader_item_is_tex1_alpha(uint8_t item)
{
    return item == SHADER_TEXEL1 || item == SHADER_TEXEL1A;
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
    if (!f->opt_2cyc || !f->opt_alpha ||
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
     * The complete tiled graph, including its low-byte PSMCT32 -> PSMT8
     * shuffle, passed the deterministic A/B scene on physical PS2 hardware.
     * Keep the diagnostic scene optional, but expose the recipe to ordinary
     * gameplay builds.
     */
    plan->hardware_validation_required = false;
    plan->supported = true;
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

static bool ps2_plan_tex01_lerp_tex1_alpha(
    const struct CCFeatures *f, struct Ps2CombinerPlan *plan)
{
    if (!f->opt_2cyc || !f->opt_alpha || f->opt_invisible ||
        !ps2_is_tex01_lerp_cycle(f) ||
        !ps2_cycle_multiplies_combined_by_input2(f, 0u) ||
        !f->do_single[0][1] ||
        !ps2_shader_item_is_tex1_alpha(f->c[0][1][3]) ||
        !f->do_multiply[1][1]) {
        return false;
    }

    const uint8_t alpha_a = f->c[1][1][0];
    const uint8_t alpha_c = f->c[1][1][2];
    if (!((alpha_a == SHADER_COMBINED &&
           alpha_c == SHADER_INPUT_1) ||
          (alpha_a == SHADER_INPUT_1 &&
           alpha_c == SHADER_COMBINED))) {
        return false;
    }

    /*
     * Active room-fog replacement CUSTOM_11/CUSTOM_06:
     * RGB = lerp(TEXEL0, TEXEL1, LOD_FRACTION) * SHADE
     * A   = TEXEL1.a * ENVIRONMENT.a
     *
     * RGB remains the regular two-texture graph. The renderer captures the
     * independent TEXEL1 alpha with a third alpha-only GS draw.
     */
    plan->color_recipe = PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2;
    plan->alpha_recipe = PS2_ALPHA_TEX1_MUL_INPUT1;
    plan->color_cycle = 1u;
    plan->alpha_cycle = 1u;
    plan->textured = true;
    plan->texture_alpha = true;
    plan->pass_graph = PS2_PASS_GRAPH_TRILERP_INDEPENDENT_ALPHA;
    plan->hardware_validation_required = true;
    plan->supported = true;
    return true;
}

static bool ps2_plan_tex01_lerp_independent_input_alpha(
    const struct CCFeatures *f, struct Ps2CombinerPlan *plan)
{
    if (!f->opt_2cyc || !f->opt_alpha || f->opt_invisible ||
        !ps2_is_tex01_lerp_cycle(f) ||
        !ps2_cycle_multiplies_combined_by_input2(f, 0u) ||
        !f->do_single[1][1] ||
        f->c[1][1][3] != SHADER_INPUT_1) {
        return false;
    }

    plan->color_recipe = PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2;
    plan->alpha_recipe = PS2_ALPHA_INPUT1;
    plan->color_cycle = 1u;
    plan->alpha_cycle = 1u;
    plan->textured = true;
    plan->texture_alpha = false;
    plan->pass_graph = PS2_PASS_GRAPH_TRILERP_INDEPENDENT_ALPHA;
    plan->hardware_validation_required = true;
    plan->supported = true;
    return true;
}

static bool ps2_plan_custom24_nonlinear_alpha(
    const struct CCFeatures *f, struct Ps2CombinerPlan *plan)
{
    if (!f->opt_2cyc || !f->opt_alpha ||
        f->opt_texture_edge || f->opt_invisible ||
        !ps2_is_tex01_lerp_cycle(f) ||
        !ps2_cycle_multiplies_combined_by_input2(f, 0u) ||
        !f->do_multiply[1][1]) {
        return false;
    }

    /* CUSTOM_24 alpha: ((1 - INPUT1) * INPUT2) * INPUT1. */
    if (f->c[0][1][0] != SHADER_1 ||
        f->c[0][1][1] != SHADER_INPUT_1 ||
        f->c[0][1][2] != SHADER_INPUT_2 ||
        f->c[0][1][3] != SHADER_0) {
        return false;
    }
    const uint8_t alpha_a = f->c[1][1][0];
    const uint8_t alpha_c = f->c[1][1][2];
    if (!((alpha_a == SHADER_COMBINED &&
           alpha_c == SHADER_INPUT_1) ||
          (alpha_a == SHADER_INPUT_1 &&
           alpha_c == SHADER_COMBINED))) {
        return false;
    }

    plan->color_recipe = PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2;
    plan->alpha_recipe = PS2_ALPHA_INPUT1_INV_INPUT1_MUL_INPUT2;
    plan->color_cycle = 1;
    plan->alpha_cycle = 1;
    plan->textured = true;
    plan->texture_alpha = false;
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

static bool ps2_plan_custom22_23_texture_edge(
    const struct CCFeatures *f, struct Ps2CombinerPlan *plan)
{
    if (!f->opt_2cyc || !f->opt_alpha || !f->opt_texture_edge ||
        f->opt_invisible || !ps2_is_tex01_lerp_cycle(f) ||
        !ps2_cycle_multiplies_combined_by_input2(f, 0u)) {
        return false;
    }

    /* CUSTOM_22 alpha: (INPUT1 - INPUT2) * TEXEL0. */
    if (f->c[0][1][0] != SHADER_INPUT_1 ||
        f->c[0][1][1] != SHADER_INPUT_2 ||
        !ps2_shader_item_is_tex0_alpha(f->c[0][1][2]) ||
        f->c[0][1][3] != SHADER_0) {
        return false;
    }

    /* CUSTOM_23 alpha: INPUT3 + COMBINED. */
    if (f->c[1][1][0] != SHADER_1 ||
        f->c[1][1][1] != SHADER_0 ||
        f->c[1][1][2] != SHADER_INPUT_3 ||
        f->c[1][1][3] != SHADER_COMBINED) {
        return false;
    }

    plan->color_recipe = PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2;
    plan->alpha_recipe =
        PS2_ALPHA_TEX0_MUL_INPUT1_MINUS_INPUT2_PLUS_INPUT3_EDGE;
    plan->color_cycle = 1;
    plan->alpha_cycle = 1;
    plan->textured = true;
    plan->texture_alpha = true;
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

static bool ps2_is_tex0_factor_lerp_cycle(
    const struct CCFeatures *f, uint8_t cycle, uint8_t channel)
{
    return f->c[cycle][channel][0] == SHADER_INPUT_1 &&
           f->c[cycle][channel][1] == SHADER_INPUT_2 &&
           ps2_shader_item_is_tex0_alpha(
               f->c[cycle][channel][2]) &&
           f->c[cycle][channel][3] == SHADER_INPUT_2;
}

static bool ps2_cycle_multiplies_tex0_by_input1(
    const struct CCFeatures *f, uint8_t cycle, uint8_t channel)
{
    if (!f->do_multiply[cycle][channel]) {
        return false;
    }

    const uint8_t a = f->c[cycle][channel][0];
    const uint8_t c = f->c[cycle][channel][2];
    if (channel == 0u) {
        return (a == SHADER_TEXEL0 && c == SHADER_INPUT_1) ||
               (a == SHADER_INPUT_1 && c == SHADER_TEXEL0);
    }
    return (ps2_shader_item_is_tex0_alpha(a) &&
            c == SHADER_INPUT_1) ||
           (a == SHADER_INPUT_1 &&
            ps2_shader_item_is_tex0_alpha(c));
}

static bool ps2_plan_custom00_01_shield(
    const struct CCFeatures *f, struct Ps2CombinerPlan *plan)
{
    if (!f->opt_2cyc || !f->opt_alpha || f->opt_texture_edge ||
        f->opt_invisible ||
        !ps2_cycle_multiplies_tex0_by_input1(f, 0u, 0u) ||
        !ps2_cycle_multiplies_tex0_by_input1(f, 0u, 1u) ||
        !ps2_channel_passes_combined(f, 0u)) {
        return false;
    }

    /* CUSTOM_01 alpha: (1 - COMBINED) * INPUT2 + COMBINED. */
    if (f->c[1][1][0] != SHADER_1 ||
        f->c[1][1][1] != SHADER_COMBINED ||
        f->c[1][1][2] != SHADER_INPUT_2 ||
        f->c[1][1][3] != SHADER_COMBINED) {
        return false;
    }

    /*
     * RGB = lerp(0, INPUT1, TEX0).
     * A = lerp(INPUT2, INPUT2 + INPUT1 * (1 - INPUT2), TEX0).
     * Both fit the existing channel-wise TEXEL0-factor graph once the alpha
     * endpoints are prepared by the vertex translator.
     */
    plan->color_recipe = PS2_COLOR_INPUT2_INPUT1_LERP_TEX0;
    plan->alpha_recipe =
        PS2_ALPHA_INPUT2_INPUT1_COVERAGE_LERP_TEX0;
    plan->color_cycle = 1u;
    plan->alpha_cycle = 1u;
    plan->textured = true;
    plan->texture_alpha = true;
    plan->pass_graph = PS2_PASS_GRAPH_TEX0_FACTOR_LERP;
    plan->hardware_validation_required = true;
    plan->supported = true;
    return true;
}

static bool ps2_plan_text_blend(
    const struct CCFeatures *f, struct Ps2CombinerPlan *plan)
{
    if (!f->opt_2cyc || !f->opt_alpha || f->opt_texture_edge ||
        f->opt_invisible ||
        f->c[0][0][0] != SHADER_INPUT_1 ||
        f->c[0][0][1] != SHADER_INPUT_2 ||
        f->c[0][0][2] != SHADER_TEXEL1A ||
        f->c[0][0][3] != SHADER_INPUT_2 ||
        !f->do_single[0][1] ||
        !ps2_shader_item_is_tex0_alpha(f->c[0][1][3]) ||
        !ps2_channel_passes_combined(f, 0u) ||
        !f->do_multiply[1][1]) {
        return false;
    }

    const uint8_t alpha_a = f->c[1][1][0];
    const uint8_t alpha_c = f->c[1][1][2];
    if (!((alpha_a == SHADER_COMBINED &&
           alpha_c == SHADER_INPUT_1) ||
          (alpha_a == SHADER_INPUT_1 &&
           alpha_c == SHADER_COMBINED))) {
        return false;
    }

    plan->color_recipe =
        PS2_COLOR_INPUT2_INPUT1_LERP_TEX1_ALPHA;
    plan->alpha_recipe = PS2_ALPHA_TEX0_MUL_INPUT1;
    plan->color_cycle = 1u;
    plan->alpha_cycle = 1u;
    plan->textured = true;
    plan->texture_alpha = true;
    plan->pass_graph = PS2_PASS_GRAPH_TEX1_ALPHA_FACTOR_LERP;
    plan->hardware_validation_required = true;
    plan->supported = true;
    return true;
}

static bool ps2_plan_tex0_factor_lerp(
    const struct CCFeatures *f, struct Ps2CombinerPlan *plan)
{
    if (f->opt_texture_edge || f->opt_invisible) {
        return false;
    }

    const int color_cycle = ps2_effective_cycle(f, 0u);
    const int alpha_cycle = f->opt_alpha
        ? ps2_effective_cycle(f, 1u) : 0;
    if (color_cycle < 0 || alpha_cycle < 0 ||
        !ps2_is_tex0_factor_lerp_cycle(
            f, (uint8_t)color_cycle, 0u)) {
        return false;
    }

    enum Ps2AlphaRecipe alpha_recipe = f->opt_alpha
        ? PS2_ALPHA_UNSUPPORTED : PS2_ALPHA_OPAQUE;
    if (f->opt_alpha && ps2_is_tex0_factor_lerp_cycle(
            f, (uint8_t)alpha_cycle, 1u)) {
        alpha_recipe = PS2_ALPHA_INPUT2_INPUT1_LERP_TEX0;
    } else if (f->opt_alpha && f->do_single[alpha_cycle][1] &&
        f->c[alpha_cycle][1][3] == SHADER_INPUT_1) {
        alpha_recipe = PS2_ALPHA_INPUT1;
    } else if (f->opt_alpha && f->do_multiply[alpha_cycle][1]) {
        const uint8_t a = f->c[alpha_cycle][1][0];
        const uint8_t c = f->c[alpha_cycle][1][2];
        if ((ps2_shader_item_is_tex0_alpha(a) &&
             c == SHADER_INPUT_1) ||
            (a == SHADER_INPUT_1 &&
             ps2_shader_item_is_tex0_alpha(c))) {
            alpha_recipe = PS2_ALPHA_TEX0_MUL_INPUT1;
        }
    }
    if (alpha_recipe == PS2_ALPHA_UNSUPPORTED) {
        return false;
    }

    plan->color_recipe = PS2_COLOR_INPUT2_INPUT1_LERP_TEX0;
    plan->alpha_recipe = alpha_recipe;
    plan->color_cycle = (uint8_t)color_cycle;
    plan->alpha_cycle = (uint8_t)alpha_cycle;
    plan->textured = true;
    plan->texture_alpha = alpha_recipe ==
            PS2_ALPHA_INPUT2_INPUT1_LERP_TEX0 ||
        alpha_recipe == PS2_ALPHA_TEX0_MUL_INPUT1;
    plan->pass_graph = PS2_PASS_GRAPH_TEX0_FACTOR_LERP;
    plan->hardware_validation_required = true;
    plan->supported = true;
    return true;
}

static bool ps2_cycle_multiplies_tex0_tex1(
    const struct CCFeatures *f, uint8_t channel)
{
    if (!f->do_multiply[0][channel]) {
        return false;
    }

    const uint8_t a = f->c[0][channel][0];
    const uint8_t c = f->c[0][channel][2];
    if (channel == 0u) {
        return (a == SHADER_TEXEL0 && c == SHADER_TEXEL1) ||
               (a == SHADER_TEXEL1 && c == SHADER_TEXEL0);
    }
    return (ps2_shader_item_is_tex0_alpha(a) &&
            ps2_shader_item_is_tex1_alpha(c)) ||
           (ps2_shader_item_is_tex1_alpha(a) &&
            ps2_shader_item_is_tex0_alpha(c));
}

static bool ps2_cycle_multiplies_combined_by_input1(
    const struct CCFeatures *f, uint8_t channel)
{
    if (!f->do_multiply[1][channel]) {
        return false;
    }
    const uint8_t a = f->c[1][channel][0];
    const uint8_t c = f->c[1][channel][2];
    return (a == SHADER_COMBINED && c == SHADER_INPUT_1) ||
           (a == SHADER_INPUT_1 && c == SHADER_COMBINED);
}

static bool ps2_plan_interference(
    const struct CCFeatures *f, struct Ps2CombinerPlan *plan)
{
    if (!f->opt_2cyc || !f->opt_alpha || f->opt_texture_edge ||
        f->opt_invisible ||
        !ps2_cycle_multiplies_tex0_tex1(f, 0u) ||
        !ps2_cycle_multiplies_tex0_tex1(f, 1u) ||
        !ps2_cycle_multiplies_combined_by_input1(f, 0u) ||
        !ps2_cycle_multiplies_combined_by_input1(f, 1u)) {
        return false;
    }

    plan->color_recipe = PS2_COLOR_TEX0_MUL_TEX1_MUL_INPUT1;
    plan->alpha_recipe = PS2_ALPHA_TEX0_MUL_TEX1_MUL_INPUT1;
    plan->color_cycle = 1u;
    plan->alpha_cycle = 1u;
    plan->textured = true;
    plan->texture_alpha = true;
    plan->pass_graph = PS2_PASS_GRAPH_INTERFERENCE;
    plan->hardware_validation_required = true;
    plan->supported = true;
    return true;
}

static bool ps2_plan_independent_tex0_alpha(
    const struct CCFeatures *f, struct Ps2CombinerPlan *plan)
{
    if (!f->opt_alpha || f->opt_invisible) {
        return false;
    }

    const int color_cycle = ps2_effective_cycle(f, 0u);
    const int alpha_cycle = ps2_effective_cycle(f, 1u);
    if (color_cycle < 0 || alpha_cycle < 0) {
        return false;
    }

    const enum Ps2ColorRecipe color_recipe =
        ps2_classify_color_recipe(f, (uint8_t)color_cycle);
    const enum Ps2AlphaRecipe alpha_recipe =
        ps2_classify_alpha_recipe(f, (uint8_t)alpha_cycle);
    if (color_recipe != PS2_COLOR_INPUT1 ||
        (alpha_recipe != PS2_ALPHA_TEX0 &&
         alpha_recipe != PS2_ALPHA_TEX0_MUL_INPUT1)) {
        return false;
    }

    /*
     * GS MODULATE cannot consume TEXEL0.a without applying TEXEL0.rgb. Keep
     * the channels independent in one tiled CT32 target and composite once.
     */
    plan->color_cycle = (uint8_t)color_cycle;
    plan->alpha_cycle = (uint8_t)alpha_cycle;
    plan->color_recipe = color_recipe;
    plan->alpha_recipe = alpha_recipe;
    plan->textured = true;
    plan->texture_alpha = true;
    plan->pass_graph = PS2_PASS_GRAPH_INDEPENDENT_TEX0_ALPHA;
    plan->hardware_validation_required = true;
    plan->supported = true;
    return true;
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

    if (ps2_plan_custom22_23_texture_edge(f, plan)) {
        return true;
    }

    if (ps2_plan_custom24_nonlinear_alpha(f, plan)) {
        return true;
    }

    if (ps2_plan_tex01_lerp_independent_alpha(f, plan)) {
        return true;
    }

    if (ps2_plan_tex01_lerp_tex1_alpha(f, plan)) {
        return true;
    }

    if (ps2_plan_tex01_lerp_independent_input_alpha(f, plan)) {
        return true;
    }

    if (ps2_plan_alpha_tex01_lerp_modulate(f, plan)) {
        return true;
    }

    if (ps2_plan_opaque_input1_tex0_lerp(f, plan)) {
        return true;
    }

    if (ps2_plan_custom00_01_shield(f, plan)) {
        return true;
    }

    if (ps2_plan_text_blend(f, plan)) {
        return true;
    }

    if (ps2_plan_tex0_factor_lerp(f, plan)) {
        return true;
    }

    if (ps2_plan_interference(f, plan)) {
        return true;
    }

    if (ps2_plan_independent_tex0_alpha(f, plan)) {
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
