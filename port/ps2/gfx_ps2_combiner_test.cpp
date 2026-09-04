#include "gfx_ps2_combiner.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct CombinerBuilder {
    uint8_t c[2][2][4];
    uint32_t options;
};

static void set_single(struct CombinerBuilder *builder, uint8_t cycle,
    uint8_t channel, uint8_t value)
{
    builder->c[cycle][channel][0] = SHADER_0;
    builder->c[cycle][channel][1] = SHADER_0;
    builder->c[cycle][channel][2] = SHADER_0;
    builder->c[cycle][channel][3] = value;
}

static void set_multiply(struct CombinerBuilder *builder, uint8_t cycle,
    uint8_t channel, uint8_t a, uint8_t c)
{
    builder->c[cycle][channel][0] = a;
    builder->c[cycle][channel][1] = SHADER_0;
    builder->c[cycle][channel][2] = c;
    builder->c[cycle][channel][3] = SHADER_0;
}

static void set_tex01_lerp(struct CombinerBuilder *builder, uint8_t cycle,
    uint8_t channel)
{
    builder->c[cycle][channel][0] = SHADER_TEXEL1;
    builder->c[cycle][channel][1] = SHADER_TEXEL0;
    builder->c[cycle][channel][2] = SHADER_INPUT_1;
    builder->c[cycle][channel][3] = SHADER_TEXEL0;
}

static void set_input1_tex0_lerp(struct CombinerBuilder *builder,
    uint8_t cycle, uint8_t channel)
{
    builder->c[cycle][channel][0] = SHADER_TEXEL0;
    builder->c[cycle][channel][1] = SHADER_INPUT_1;
    builder->c[cycle][channel][2] = SHADER_INPUT_2;
    builder->c[cycle][channel][3] = SHADER_INPUT_1;
}

static void set_tex0_factor_lerp(struct CombinerBuilder *builder,
    uint8_t cycle, uint8_t channel)
{
    builder->c[cycle][channel][0] = SHADER_INPUT_1;
    builder->c[cycle][channel][1] = SHADER_INPUT_2;
    builder->c[cycle][channel][2] = SHADER_TEXEL0;
    builder->c[cycle][channel][3] = SHADER_INPUT_2;
}

static struct CCFeatures decode(const struct CombinerBuilder *builder)
{
    uint64_t shader_id0 = 0;
    for (uint8_t cycle = 0; cycle < 2; ++cycle) {
        for (uint8_t channel = 0; channel < 2; ++channel) {
            for (uint8_t term = 0; term < 4; ++term) {
                const uint8_t shift = (uint8_t)(cycle * 32u +
                    channel * 16u + term * 4u);
                shader_id0 |= (uint64_t)builder->c[cycle][channel][term]
                    << shift;
            }
        }
    }

    struct CCFeatures features{};
    gfx_cc_get_features(shader_id0, builder->options, &features);
    return features;
}

static struct Ps2CombinerPlan plan(const struct CombinerBuilder *builder)
{
    const struct CCFeatures features = decode(builder);
    struct Ps2CombinerPlan result{};
    ps2GfxPlanCombiner(&features, &result);
    return result;
}

static void test_one_cycle_modulate(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_ALPHA;
    set_multiply(&builder, 0, 0, SHADER_TEXEL0, SHADER_INPUT_1);
    set_multiply(&builder, 0, 1, SHADER_TEXEL0, SHADER_INPUT_1);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_cycle == 0);
    assert(result.alpha_cycle == 0);
    assert(result.color_recipe == PS2_COLOR_TEX0_MUL_INPUT1);
    assert(result.alpha_recipe == PS2_ALPHA_TEX0_MUL_INPUT1);
    assert(result.textured);
    assert(result.texture_alpha);
    assert(!result.hardware_validation_required);

    /* The VU1 hardware cube must remain one-pass when fog/alpha cutout are on. */
    builder.options |= SHADER_OPT_ALPHA_THRESHOLD | SHADER_OPT_FOG;
    const struct CCFeatures fog_features = decode(&builder);
    const struct Ps2CombinerPlan fog_result = plan(&builder);
    assert(fog_features.opt_fog);
    assert(fog_result.supported);
    assert(fog_result.pass_graph == PS2_PASS_GRAPH_DIRECT);
    assert(fog_result.textured);
    assert(fog_result.color_recipe == PS2_COLOR_TEX0_MUL_INPUT1);
    assert(fog_result.alpha_recipe == PS2_ALPHA_TEX0_MUL_INPUT1);
    assert(!fog_result.hardware_validation_required);
}

static void test_one_cycle_texture_edge(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_ALPHA | SHADER_OPT_TEXTURE_EDGE;
    set_multiply(&builder, 0, 0, SHADER_TEXEL0, SHADER_INPUT_1);
    set_multiply(&builder, 0, 1, SHADER_TEXEL0, SHADER_INPUT_1);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.pass_graph == PS2_PASS_GRAPH_DIRECT);
    assert(result.texture_alpha);
}

static void test_texture_edge_requires_alpha(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_TEXTURE_EDGE;
    set_multiply(&builder, 0, 0, SHADER_TEXEL0, SHADER_INPUT_1);

    assert(!plan(&builder).supported);
}

static void test_one_cycle_invisible_depth_only(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_ALPHA | SHADER_OPT_INVISIBLE;
    set_multiply(&builder, 0, 0, SHADER_TEXEL0, SHADER_INPUT_1);
    set_multiply(&builder, 0, 1, SHADER_TEXEL0, SHADER_INPUT_1);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.pass_graph == PS2_PASS_GRAPH_DIRECT);
}

static void test_invisible_requires_alpha_contract(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_INVISIBLE;
    set_multiply(&builder, 0, 0, SHADER_TEXEL0, SHADER_INPUT_1);

    assert(!plan(&builder).supported);
}

static void test_two_cycle_independent_final_cycle(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_ALPHA | SHADER_OPT_2CYC;

    /* Dead first-cycle sources must not constrain the exact final result. */
    set_multiply(&builder, 0, 0, SHADER_TEXEL1, SHADER_INPUT_2);
    set_single(&builder, 0, 1, SHADER_TEXEL1);
    set_multiply(&builder, 1, 0, SHADER_TEXEL0, SHADER_INPUT_1);
    set_multiply(&builder, 1, 1, SHADER_TEXEL0, SHADER_INPUT_1);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_cycle == 1);
    assert(result.alpha_cycle == 1);
    assert(result.color_recipe == PS2_COLOR_TEX0_MUL_INPUT1);
    assert(result.alpha_recipe == PS2_ALPHA_TEX0_MUL_INPUT1);
}

static void test_two_cycle_pass2_uses_first_cycle(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_ALPHA | SHADER_OPT_2CYC;
    set_multiply(&builder, 0, 0, SHADER_TEXEL0, SHADER_INPUT_1);
    set_multiply(&builder, 0, 1, SHADER_TEXEL0, SHADER_INPUT_1);
    set_single(&builder, 1, 0, SHADER_COMBINED);
    set_single(&builder, 1, 1, SHADER_COMBINED);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_cycle == 0);
    assert(result.alpha_cycle == 0);
    assert(result.color_recipe == PS2_COLOR_TEX0_MUL_INPUT1);
    assert(result.alpha_recipe == PS2_ALPHA_TEX0_MUL_INPUT1);
}

static void test_multiply_by_one_is_pass2(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC;
    set_single(&builder, 0, 0, SHADER_INPUT_1);
    set_multiply(&builder, 1, 0, SHADER_COMBINED, SHADER_1);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_cycle == 0);
    assert(result.color_recipe == PS2_COLOR_INPUT1);
    assert(result.alpha_recipe == PS2_ALPHA_OPAQUE);
}

static void test_color_and_alpha_reduce_independently(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_ALPHA | SHADER_OPT_2CYC;
    set_multiply(&builder, 0, 0, SHADER_TEXEL0, SHADER_INPUT_1);
    set_single(&builder, 0, 1, SHADER_TEXEL0);
    set_single(&builder, 1, 0, SHADER_COMBINED);
    set_single(&builder, 1, 1, SHADER_INPUT_1);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_cycle == 0);
    assert(result.alpha_cycle == 1);
    assert(result.color_recipe == PS2_COLOR_TEX0_MUL_INPUT1);
    assert(result.alpha_recipe == PS2_ALPHA_INPUT1);
    assert(!result.texture_alpha);
}

static void test_combined_dependency_requires_multipass(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC;
    set_single(&builder, 0, 0, SHADER_TEXEL0);
    set_multiply(&builder, 1, 0, SHADER_COMBINED, SHADER_INPUT_1);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(!result.supported);
}

static void test_opaque_trilerp_pass2(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC;
    set_tex01_lerp(&builder, 0, 0);
    set_single(&builder, 1, 0, SHADER_COMBINED);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe == PS2_COLOR_TEX01_LERP_INPUT1);
    assert(result.alpha_recipe == PS2_ALPHA_OPAQUE);
    assert(result.textured);
    assert(!result.texture_alpha);
    assert(result.pass_graph == PS2_PASS_GRAPH_OPAQUE_TRILERP);
}

static void test_opaque_trilerp_modulate(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_FOG;
    set_tex01_lerp(&builder, 0, 0);
    set_multiply(&builder, 1, 0, SHADER_COMBINED, SHADER_INPUT_2);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2);
    assert(result.alpha_recipe == PS2_ALPHA_OPAQUE);
}

static void test_alpha_trilerp_modulate_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA;
    set_tex01_lerp(&builder, 0, 0);
    set_tex01_lerp(&builder, 0, 1);
    set_multiply(&builder, 1, 0, SHADER_COMBINED, SHADER_INPUT_2);
    set_multiply(&builder, 1, 1, SHADER_COMBINED, SHADER_INPUT_2);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2);
    assert(result.alpha_recipe == PS2_ALPHA_UNSUPPORTED);
    assert(result.textured);
    assert(result.texture_alpha);
    assert(!result.hardware_validation_required);
    assert(result.pass_graph == PS2_PASS_GRAPH_ALPHA_TRILERP_MODULATE);
}

static void test_custom25_independent_alpha_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA | SHADER_OPT_FOG;
    set_tex01_lerp(&builder, 0, 0);
    set_single(&builder, 0, 1, SHADER_INPUT_1);
    set_multiply(&builder, 1, 0, SHADER_COMBINED, SHADER_INPUT_2);
    set_multiply(&builder, 1, 1, SHADER_COMBINED, SHADER_INPUT_2);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2);
    assert(result.alpha_recipe == PS2_ALPHA_INPUT1_MUL_INPUT2);
    assert(result.textured);
    assert(!result.texture_alpha);
    assert(!result.hardware_validation_required);
    assert(result.pass_graph ==
        PS2_PASS_GRAPH_TRILERP_INDEPENDENT_ALPHA);
}

static void test_custom26_tex0_independent_alpha_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA | SHADER_OPT_FOG;
    set_tex01_lerp(&builder, 0, 0);
    set_multiply(&builder, 0, 1, SHADER_TEXEL0, SHADER_INPUT_1);
    set_multiply(&builder, 1, 0, SHADER_COMBINED, SHADER_INPUT_2);
    set_multiply(&builder, 1, 1, SHADER_COMBINED, SHADER_INPUT_2);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2);
    assert(result.alpha_recipe ==
        PS2_ALPHA_TEX0_MUL_INPUT1_MUL_INPUT2);
    assert(result.textured);
    assert(result.texture_alpha);
    assert(!result.hardware_validation_required);
    assert(result.pass_graph ==
        PS2_PASS_GRAPH_TRILERP_INDEPENDENT_ALPHA);
}

static void test_custom08_independent_input_alpha_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA |
        SHADER_OPT_FOG;
    set_tex01_lerp(&builder, 0u, 0u);
    set_multiply(
        &builder, 1u, 0u, SHADER_COMBINED, SHADER_INPUT_2);
    set_single(&builder, 1u, 1u, SHADER_INPUT_1);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2);
    assert(result.alpha_recipe == PS2_ALPHA_INPUT1);
    assert(result.textured);
    assert(!result.texture_alpha);
    assert(result.pass_graph ==
        PS2_PASS_GRAPH_TRILERP_INDEPENDENT_ALPHA);
    assert(result.hardware_validation_required);

    set_single(&builder, 1u, 1u, SHADER_INPUT_2);
    assert(!plan(&builder).supported);
}

static void test_gfxreplace_custom11_custom06_tex1_alpha(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA |
        SHADER_OPT_FOG;

    /* CUSTOM_11 first cycle and CUSTOM_06 second cycle. */
    set_tex01_lerp(&builder, 0u, 0u);
    set_single(&builder, 0u, 1u, SHADER_TEXEL1);
    set_multiply(
        &builder, 1u, 0u, SHADER_COMBINED, SHADER_INPUT_2);
    set_multiply(
        &builder, 1u, 1u, SHADER_COMBINED, SHADER_INPUT_1);

    struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2);
    assert(result.alpha_recipe == PS2_ALPHA_TEX1_MUL_INPUT1);
    assert(result.pass_graph ==
        PS2_PASS_GRAPH_TRILERP_INDEPENDENT_ALPHA);
    assert(result.textured);
    assert(result.texture_alpha);
    assert(result.hardware_validation_required);

    /* The active fog replacement can also be used with TEX_EDGE state. */
    builder.options |= SHADER_OPT_TEXTURE_EDGE;
    result = plan(&builder);
    assert(result.supported);

    builder.c[1][1][2] = SHADER_INPUT_2;
    assert(!plan(&builder).supported);
}

static void test_custom24_nonlinear_alpha_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA | SHADER_OPT_FOG;
    set_tex01_lerp(&builder, 0, 0);
    builder.c[0][1][0] = SHADER_1;
    builder.c[0][1][1] = SHADER_INPUT_1;
    builder.c[0][1][2] = SHADER_INPUT_2;
    builder.c[0][1][3] = SHADER_0;
    set_multiply(&builder, 1, 0, SHADER_COMBINED, SHADER_INPUT_2);
    set_multiply(&builder, 1, 1, SHADER_COMBINED, SHADER_INPUT_1);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2);
    assert(result.alpha_recipe ==
        PS2_ALPHA_INPUT1_INV_INPUT1_MUL_INPUT2);
    assert(result.textured);
    assert(!result.texture_alpha);
    assert(result.pass_graph ==
        PS2_PASS_GRAPH_TRILERP_INDEPENDENT_ALPHA);

    builder.c[0][1][1] = SHADER_INPUT_2;
    assert(!plan(&builder).supported);
}

static void test_custom21_texture_edge_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA |
        SHADER_OPT_FOG | SHADER_OPT_TEXTURE_EDGE;
    set_tex01_lerp(&builder, 0, 0);
    set_multiply(&builder, 1, 0, SHADER_COMBINED, SHADER_INPUT_2);
    builder.c[0][1][0] = SHADER_1;
    builder.c[0][1][1] = SHADER_0;
    builder.c[0][1][2] = SHADER_INPUT_1;
    builder.c[0][1][3] = SHADER_INPUT_2;
    set_single(&builder, 1, 1, SHADER_COMBINED);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2);
    assert(result.alpha_recipe == PS2_ALPHA_INPUT1_PLUS_INPUT2_EDGE);
    assert(result.textured);
    assert(!result.texture_alpha);
    assert(!result.hardware_validation_required);
    assert(result.pass_graph ==
        PS2_PASS_GRAPH_TRILERP_INDEPENDENT_ALPHA);

    builder.options &= ~SHADER_OPT_TEXTURE_EDGE;
    assert(!plan(&builder).supported);
}

static void test_custom22_23_signed_texture_edge_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA |
        SHADER_OPT_FOG | SHADER_OPT_TEXTURE_EDGE;
    set_tex01_lerp(&builder, 0, 0);
    builder.c[0][1][0] = SHADER_INPUT_1;
    builder.c[0][1][1] = SHADER_INPUT_2;
    builder.c[0][1][2] = SHADER_TEXEL0;
    builder.c[0][1][3] = SHADER_0;
    set_multiply(&builder, 1, 0, SHADER_COMBINED, SHADER_INPUT_2);
    builder.c[1][1][0] = SHADER_1;
    builder.c[1][1][1] = SHADER_0;
    builder.c[1][1][2] = SHADER_INPUT_3;
    builder.c[1][1][3] = SHADER_COMBINED;

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2);
    assert(result.alpha_recipe ==
        PS2_ALPHA_TEX0_MUL_INPUT1_MINUS_INPUT2_PLUS_INPUT3_EDGE);
    assert(result.textured);
    assert(result.texture_alpha);
    assert(!result.hardware_validation_required);
    assert(result.pass_graph ==
        PS2_PASS_GRAPH_TRILERP_INDEPENDENT_ALPHA);

    builder.c[1][1][2] = SHADER_INPUT_2;
    assert(!plan(&builder).supported);
}

static void test_alpha_trilerp_rejects_mismatched_final_channels(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA;
    set_tex01_lerp(&builder, 0, 0);
    set_tex01_lerp(&builder, 0, 1);
    set_multiply(&builder, 1, 0, SHADER_COMBINED, SHADER_INPUT_2);
    set_single(&builder, 1, 1, SHADER_COMBINED);

    assert(!plan(&builder).supported);
}

static void test_alpha_trilerp_supports_fogged_final_composite(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA | SHADER_OPT_FOG;
    set_tex01_lerp(&builder, 0, 0);
    set_tex01_lerp(&builder, 0, 1);
    set_multiply(&builder, 1, 0, SHADER_COMBINED, SHADER_INPUT_2);
    set_multiply(&builder, 1, 1, SHADER_COMBINED, SHADER_INPUT_2);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.pass_graph == PS2_PASS_GRAPH_ALPHA_TRILERP_MODULATE);
    assert(result.color_recipe ==
        PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2);
    assert(!result.hardware_validation_required);
}

static void test_opaque_env_tex0_lerp_modulate(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_FOG;
    set_input1_tex0_lerp(&builder, 0, 0);
    set_multiply(&builder, 1, 0, SHADER_COMBINED, SHADER_INPUT_3);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_INPUT1_TEX0_LERP_INPUT2_MUL_INPUT3);
    assert(result.alpha_recipe == PS2_ALPHA_OPAQUE);
    assert(result.textured);
    assert(!result.texture_alpha);
}

static void test_opaque_env_tex0_lerp_pass2(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC;
    set_input1_tex0_lerp(&builder, 0, 0);
    set_single(&builder, 1, 0, SHADER_COMBINED);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_INPUT1_TEX0_LERP_INPUT2);
    assert(result.alpha_recipe == PS2_ALPHA_OPAQUE);
}

static void test_alpha_env_tex0_lerp_remains_unsupported(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA;
    set_input1_tex0_lerp(&builder, 0, 0);
    set_multiply(&builder, 1, 0, SHADER_COMBINED, SHADER_INPUT_3);
    set_single(&builder, 0, 1, SHADER_INPUT_1);
    set_single(&builder, 1, 1, SHADER_COMBINED);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(!result.supported);
}

static void test_blendia_tex0_factor_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA |
        SHADER_OPT_FOG;
    set_tex0_factor_lerp(&builder, 1u, 0u);
    set_multiply(
        &builder, 1u, 1u, SHADER_TEXEL0, SHADER_INPUT_1);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_INPUT2_INPUT1_LERP_TEX0);
    assert(result.alpha_recipe == PS2_ALPHA_TEX0_MUL_INPUT1);
    assert(result.pass_graph == PS2_PASS_GRAPH_TEX0_FACTOR_LERP);
    assert(result.texture_alpha);
}

static void test_custom27_tex0_factor_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_ALPHA;
    set_tex0_factor_lerp(&builder, 0u, 0u);
    set_tex0_factor_lerp(&builder, 0u, 1u);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_INPUT2_INPUT1_LERP_TEX0);
    assert(result.alpha_recipe ==
        PS2_ALPHA_INPUT2_INPUT1_LERP_TEX0);
    assert(result.pass_graph == PS2_PASS_GRAPH_TEX0_FACTOR_LERP);

    builder.c[0][1][3] = SHADER_INPUT_1;
    assert(!plan(&builder).supported);
}

static void test_skydome_tex0_factor_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_ALPHA;
    set_tex0_factor_lerp(&builder, 0u, 0u);
    set_single(&builder, 0u, 1u, SHADER_INPUT_1);

    struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_INPUT2_INPUT1_LERP_TEX0);
    assert(result.alpha_recipe == PS2_ALPHA_INPUT1);
    assert(result.pass_graph == PS2_PASS_GRAPH_TEX0_FACTOR_LERP);
    assert(result.textured);
    assert(!result.texture_alpha);
    assert(result.hardware_validation_required);

    builder.options = 0u;
    result = plan(&builder);
    assert(result.supported);
    assert(result.alpha_recipe == PS2_ALPHA_OPAQUE);
    assert(result.pass_graph == PS2_PASS_GRAPH_TEX0_FACTOR_LERP);
    assert(!result.texture_alpha);

    builder.options = SHADER_OPT_ALPHA;
    set_single(&builder, 0u, 1u, SHADER_INPUT_2);
    assert(!plan(&builder).supported);
}

static void test_custom00_01_shield_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA;
    set_multiply(
        &builder, 0u, 0u, SHADER_TEXEL0, SHADER_INPUT_1);
    set_multiply(
        &builder, 0u, 1u, SHADER_TEXEL0, SHADER_INPUT_1);
    set_single(&builder, 1u, 0u, SHADER_COMBINED);
    builder.c[1][1][0] = SHADER_1;
    builder.c[1][1][1] = SHADER_COMBINED;
    builder.c[1][1][2] = SHADER_INPUT_2;
    builder.c[1][1][3] = SHADER_COMBINED;

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_INPUT2_INPUT1_LERP_TEX0);
    assert(result.alpha_recipe ==
        PS2_ALPHA_INPUT2_INPUT1_COVERAGE_LERP_TEX0);
    assert(result.pass_graph == PS2_PASS_GRAPH_TEX0_FACTOR_LERP);
    assert(result.textured);
    assert(result.texture_alpha);
    assert(result.hardware_validation_required);

    builder.c[1][1][2] = SHADER_INPUT_1;
    assert(!plan(&builder).supported);
}

static void test_text_blend_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA;
    builder.c[0][0][0] = SHADER_INPUT_1;
    builder.c[0][0][1] = SHADER_INPUT_2;
    builder.c[0][0][2] = SHADER_TEXEL1A;
    builder.c[0][0][3] = SHADER_INPUT_2;
    set_single(&builder, 0u, 1u, SHADER_TEXEL0);
    set_single(&builder, 1u, 0u, SHADER_COMBINED);
    set_multiply(
        &builder, 1u, 1u, SHADER_COMBINED, SHADER_INPUT_1);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_INPUT2_INPUT1_LERP_TEX1_ALPHA);
    assert(result.alpha_recipe == PS2_ALPHA_TEX0_MUL_INPUT1);
    assert(result.pass_graph ==
        PS2_PASS_GRAPH_TEX1_ALPHA_FACTOR_LERP);
    assert(result.textured);
    assert(result.texture_alpha);
    assert(result.hardware_validation_required);

    builder.c[0][0][2] = SHADER_TEXEL1;
    assert(!plan(&builder).supported);
}

static void test_interference_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC | SHADER_OPT_ALPHA |
        SHADER_OPT_FOG;
    set_multiply(&builder, 0u, 0u, SHADER_TEXEL0, SHADER_TEXEL1);
    set_multiply(&builder, 0u, 1u, SHADER_TEXEL0, SHADER_TEXEL1);
    set_multiply(&builder, 1u, 0u, SHADER_COMBINED, SHADER_INPUT_1);
    set_multiply(&builder, 1u, 1u, SHADER_COMBINED, SHADER_INPUT_1);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe ==
        PS2_COLOR_TEX0_MUL_TEX1_MUL_INPUT1);
    assert(result.alpha_recipe ==
        PS2_ALPHA_TEX0_MUL_TEX1_MUL_INPUT1);
    assert(result.pass_graph == PS2_PASS_GRAPH_INTERFERENCE);
    assert(result.textured);
    assert(result.texture_alpha);
    assert(result.hardware_validation_required);

    builder.c[1][1][2] = SHADER_INPUT_2;
    assert(!plan(&builder).supported);
}

static void test_independent_tex0_alpha_pass_graph(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_ALPHA | SHADER_OPT_FOG;
    set_single(&builder, 0u, 0u, SHADER_INPUT_1);
    set_multiply(
        &builder, 0u, 1u, SHADER_TEXEL0, SHADER_INPUT_1);

    struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_recipe == PS2_COLOR_INPUT1);
    assert(result.alpha_recipe == PS2_ALPHA_TEX0_MUL_INPUT1);
    assert(result.textured);
    assert(result.texture_alpha);
    assert(result.pass_graph ==
        PS2_PASS_GRAPH_INDEPENDENT_TEX0_ALPHA);
    assert(result.hardware_validation_required);

    set_single(&builder, 0u, 1u, SHADER_TEXEL0);
    result = plan(&builder);
    assert(result.supported);
    assert(result.alpha_recipe == PS2_ALPHA_TEX0);
    assert(result.pass_graph ==
        PS2_PASS_GRAPH_INDEPENDENT_TEX0_ALPHA);

    builder.options |= SHADER_OPT_INVISIBLE;
    assert(!plan(&builder).supported);
}

static void test_opaque_output_ignores_alpha_dependency(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_2CYC;
    set_single(&builder, 1, 0, SHADER_INPUT_1);
    set_multiply(&builder, 1, 1, SHADER_COMBINED, SHADER_INPUT_1);

    const struct Ps2CombinerPlan result = plan(&builder);
    assert(result.supported);
    assert(result.color_cycle == 1);
    assert(result.alpha_recipe == PS2_ALPHA_OPAQUE);
}

static void test_rejects_unmapped_state(void)
{
    struct CombinerBuilder builder{};
    builder.options = SHADER_OPT_ALPHA | SHADER_OPT_NOISE;
    set_single(&builder, 0, 0, SHADER_INPUT_1);
    set_single(&builder, 0, 1, SHADER_INPUT_1);
    assert(!plan(&builder).supported);
}

int main(void)
{
    test_one_cycle_modulate();
    test_one_cycle_texture_edge();
    test_texture_edge_requires_alpha();
    test_one_cycle_invisible_depth_only();
    test_invisible_requires_alpha_contract();
    test_two_cycle_independent_final_cycle();
    test_two_cycle_pass2_uses_first_cycle();
    test_multiply_by_one_is_pass2();
    test_color_and_alpha_reduce_independently();
    test_combined_dependency_requires_multipass();
    test_opaque_trilerp_pass2();
    test_opaque_trilerp_modulate();
    test_alpha_trilerp_modulate_pass_graph();
    test_custom25_independent_alpha_pass_graph();
    test_custom26_tex0_independent_alpha_pass_graph();
    test_custom08_independent_input_alpha_pass_graph();
    test_gfxreplace_custom11_custom06_tex1_alpha();
    test_custom24_nonlinear_alpha_pass_graph();
    test_custom21_texture_edge_pass_graph();
    test_custom22_23_signed_texture_edge_pass_graph();
    test_alpha_trilerp_rejects_mismatched_final_channels();
    test_alpha_trilerp_supports_fogged_final_composite();
    test_opaque_env_tex0_lerp_modulate();
    test_opaque_env_tex0_lerp_pass2();
    test_alpha_env_tex0_lerp_remains_unsupported();
    test_blendia_tex0_factor_pass_graph();
    test_custom27_tex0_factor_pass_graph();
    test_skydome_tex0_factor_pass_graph();
    test_custom00_01_shield_pass_graph();
    test_text_blend_pass_graph();
    test_interference_pass_graph();
    test_independent_tex0_alpha_pass_graph();
    test_opaque_output_ignores_alpha_dependency();
    test_rejects_unmapped_state();
    puts("gfx_ps2_combiner tests passed");
    return 0;
}
