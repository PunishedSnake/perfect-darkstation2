#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <gsKit.h>
#include <gsInline.h>

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_ps2.h"
#include "gs_core.h"
#include "system.h"

/*
 * Perfect Dark Fast3D compatibility adapter for PS2.
 *
 * The adapter accepts the clip-space VBO contract produced by current
 * port/fast3d/gfx_pc.cpp and translates the supported material subset into
 * GS-ready vertices and texture selections. Device lifetime, frame ownership
 * and GS register state live below this file in gs_core.
 *
 * Current supported material recipes:
 *   - INPUT1 (untextured vertex colour)
 *   - TEXEL0 * INPUT1 (one resident texture modulated by vertex colour)
 *
 * Unsupported combiners are retained in the shader table so shader_get_info()
 * still reports the exact upstream VBO layout, but draw submission rejects the
 * unsupported recipe rather than silently rendering a wrong approximation.
 */

#define PS2_GFX_MAX_SHADERS 32
#define PS2_GFX_MAX_TEXTURES 64
#define PS2_GFX_TRANSLATE_VERTS 96

struct ShaderProgram {
    bool used;
    bool supported;
    bool textured;
    uint64_t shader_id0;
    uint32_t shader_id1;
    struct CCFeatures features;
};

struct Ps2TextureSlot {
    bool used;
    bool uploaded;
    GSTEXTURE texture;
};

struct Ps2Viewport {
    int x;
    int y;
    int width;
    int height;
};

/*
 * Transitional escape hatch. Texture residency and GS-ready vertex packing are
 * the remaining reasons this Fast3D adapter still needs a GSGLOBAL pointer.
 */
static GSGLOBAL *s_gs;
static struct ShaderProgram s_shaders[PS2_GFX_MAX_SHADERS];
static struct Ps2TextureSlot s_textures[PS2_GFX_MAX_TEXTURES];
static struct ShaderProgram *s_shader;
static uint32_t s_selected_texture[2];
static int s_active_texture_tile;
static struct Ps2Viewport s_viewport;
static float s_depth_near = 0.0f;
static float s_depth_far = 1.0f;
static bool s_depth_test = true;
static bool s_depth_update = true;
static bool s_use_alpha;
static bool s_modulate;
static enum FilteringMode s_filter_mode = FILTER_LINEAR;
static enum MipmapFilteringMode s_mipmap_filter = MIPMAP_DISABLED;
static int s_anisotropy = 1;
static bool s_warned_framebuffer;
static bool s_warned_depth_prim;
static bool s_warned_mipmap;

static GSPRIMSTQPOINT s_stq_vertices[PS2_GFX_TRANSLATE_VERTS];
static GSPRIMPOINT s_color_vertices[PS2_GFX_TRANSLATE_VERTS];

extern "C" void gfxPs2BindGs(GSGLOBAL *gs)
{
    s_gs = gs;
    if (gs) {
        s_viewport.x = 0;
        s_viewport.y = 0;
        s_viewport.width = gs->Width;
        s_viewport.height = gs->Height;
    }
}

static const char *ps2_get_name(void)
{
    return "PlayStation 2 GS";
}

static int ps2_get_max_texture_size(void)
{
    /* GS texture coordinates and TEX0 support up to 1024 texels per axis. */
    return 1024;
}

static struct GfxClipParameters ps2_get_clip_parameters(void)
{
    /*
     * Request the current Fast3D path to remap clip Z from [-W,+W] to [0,W].
     * Y remains in the upstream convention and is converted to GS top-down
     * screen coordinates during clip->viewport translation below.
     */
    struct GfxClipParameters params = { true, false };
    return params;
}

static bool ps2_shader_common_supported(const struct CCFeatures *f)
{
    return !f->opt_fog && !f->opt_noise && !f->opt_2cyc &&
           !f->opt_grayscale && !f->opt_blur && !f->used_textures[1] &&
           f->num_inputs <= 1;
}

static bool ps2_shader_is_untextured_input1(const struct CCFeatures *f)
{
    return ps2_shader_common_supported(f) && !f->used_textures[0] &&
           f->num_inputs == 1 && f->do_single[0][0] &&
           f->c[0][0][3] == SHADER_INPUT_1;
}

static bool ps2_shader_is_tex0_mul_input1(const struct CCFeatures *f)
{
    return ps2_shader_common_supported(f) && f->used_textures[0] &&
           f->num_inputs == 1 && f->do_multiply[0][0] &&
           f->c[0][0][0] == SHADER_TEXEL0 &&
           f->c[0][0][2] == SHADER_INPUT_1;
}

static struct ShaderProgram *ps2_lookup_shader(uint64_t shader_id0, uint32_t shader_id1)
{
    for (int i = 0; i < PS2_GFX_MAX_SHADERS; ++i) {
        if (s_shaders[i].used && s_shaders[i].shader_id0 == shader_id0 &&
            s_shaders[i].shader_id1 == shader_id1) {
            return &s_shaders[i];
        }
    }
    return NULL;
}

static void ps2_unload_shader(struct ShaderProgram *old_prg)
{
    (void)old_prg;
}

static void ps2_load_shader(struct ShaderProgram *new_prg)
{
    s_shader = new_prg;
}

static struct ShaderProgram *ps2_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1)
{
    struct ShaderProgram *existing = ps2_lookup_shader(shader_id0, shader_id1);
    if (existing) {
        s_shader = existing;
        return existing;
    }

    for (int i = 0; i < PS2_GFX_MAX_SHADERS; ++i) {
        if (!s_shaders[i].used) {
            struct ShaderProgram *prg = &s_shaders[i];
            memset(prg, 0, sizeof(*prg));
            prg->used = true;
            prg->shader_id0 = shader_id0;
            prg->shader_id1 = shader_id1;
            gfx_cc_get_features(shader_id0, shader_id1, &prg->features);
            prg->textured = ps2_shader_is_tex0_mul_input1(&prg->features);
            prg->supported = prg->textured || ps2_shader_is_untextured_input1(&prg->features);

            sysLogPrintf(prg->supported ? LOG_NOTE : LOG_WARNING,
                "GfxPS2 shader %02d id=%016llx/%08x inputs=%d tex=%d%d supported=%d",
                i,
                (unsigned long long)shader_id0,
                (unsigned int)shader_id1,
                prg->features.num_inputs,
                prg->features.used_textures[0] ? 1 : 0,
                prg->features.used_textures[1] ? 1 : 0,
                prg->supported ? 1 : 0);

            s_shader = prg;
            return prg;
        }
    }

    sysLogPrintf(LOG_ERROR, "GfxPS2 shader pool exhausted (%d)", PS2_GFX_MAX_SHADERS);
    return NULL;
}

static void ps2_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2])
{
    if (!prg) {
        *num_inputs = 0;
        used_textures[0] = false;
        used_textures[1] = false;
        return;
    }

    *num_inputs = (uint8_t)prg->features.num_inputs;
    used_textures[0] = prg->features.used_textures[0];
    used_textures[1] = prg->features.used_textures[1];
}

static void ps2_clear_shaders(void)
{
    memset(s_shaders, 0, sizeof(s_shaders));
    s_shader = NULL;
}

static uint32_t ps2_new_texture(void)
{
    for (uint32_t i = 0; i < PS2_GFX_MAX_TEXTURES; ++i) {
        if (!s_textures[i].used) {
            memset(&s_textures[i], 0, sizeof(s_textures[i]));
            s_textures[i].used = true;
            return i + 1;
        }
    }

    sysLogPrintf(LOG_ERROR, "GfxPS2 texture table exhausted (%d)", PS2_GFX_MAX_TEXTURES);
    return 0;
}

static struct Ps2TextureSlot *ps2_texture_slot(uint32_t texture_id)
{
    if (texture_id == 0 || texture_id > PS2_GFX_MAX_TEXTURES) {
        return NULL;
    }

    struct Ps2TextureSlot *slot = &s_textures[texture_id - 1];
    return slot->used ? slot : NULL;
}

static void ps2_select_texture(int tile, uint32_t texture_id, bool linear_filter)
{
    if (tile < 0 || tile > 1 || !ps2_texture_slot(texture_id)) {
        return;
    }

    s_selected_texture[tile] = texture_id;
    s_active_texture_tile = tile;
    s_textures[texture_id - 1].texture.Filter = linear_filter ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
}

static void ps2_upload_texture(const uint8_t *rgba32_buf, uint32_t width, uint32_t height, bool gen_mipmaps)
{
    if (!s_gs || s_active_texture_tile < 0 || s_active_texture_tile > 1) {
        return;
    }

    struct Ps2TextureSlot *slot = ps2_texture_slot(s_selected_texture[s_active_texture_tile]);
    if (!slot || !rgba32_buf || width == 0 || height == 0 || width > 1024 || height > 1024) {
        return;
    }

    if (gen_mipmaps && !s_warned_mipmap) {
        sysLogPrintf(LOG_WARNING, "GfxPS2 mipmap generation is not implemented in the bring-up backend");
        s_warned_mipmap = true;
    }

    GSTEXTURE *tex = &slot->texture;
    const u32 bytes = gsKit_texture_size((int)width, (int)height, GS_PSM_CT32);

    /*
     * CURRENT IMPLEMENTATION: gsKit's allocator is monotonic. Reallocating an
     * existing texture with a different extent would leak GS-local memory, so
     * reject that transition until gs_core owns residency/eviction explicitly.
     */
    if (slot->uploaded && (tex->Width != width || tex->Height != height)) {
        sysLogPrintf(LOG_WARNING,
            "GfxPS2 texture resize rejected id=%u old=%ux%u new=%ux%u",
            s_selected_texture[s_active_texture_tile], tex->Width, tex->Height, width, height);
        return;
    }

    if (!slot->uploaded) {
        memset(tex, 0, sizeof(*tex));
        tex->Width = width;
        tex->Height = height;
        tex->PSM = GS_PSM_CT32;
        tex->Filter = GS_FILTER_NEAREST;
        tex->Vram = gsKit_vram_alloc(s_gs, bytes, GSKIT_ALLOC_USERBUFFER);
        if (tex->Vram == GSKIT_ALLOC_ERROR) {
            sysLogPrintf(LOG_ERROR,
                "GfxPS2 GS VRAM allocation failed id=%u size=%u",
                s_selected_texture[s_active_texture_tile], bytes);
            return;
        }
        slot->uploaded = true;
    }

    tex->Mem = (u32 *)(uintptr_t)rgba32_buf;
    gsKit_texture_upload(s_gs, tex);
    tex->Mem = NULL;
}

static void ps2_set_sampler_parameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt, bool mipmaps)
{
    if (sampler < 0 || sampler > 1) {
        return;
    }

    struct Ps2TextureSlot *slot = ps2_texture_slot(s_selected_texture[sampler]);
    if (slot) {
        slot->texture.Filter = linear_filter ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
    }

    ps2GsCoreSetTextureClamp(cms, cmt);

    if (mipmaps && !s_warned_mipmap) {
        sysLogPrintf(LOG_WARNING, "GfxPS2 mipmap sampling requested but not implemented");
        s_warned_mipmap = true;
    }
}

static void ps2_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare,
                               bool depth_source_prim, uint16_t zmode)
{
    (void)zmode;

    s_depth_test = depth_test;
    s_depth_update = depth_update;
    ps2GsCoreSetDepthMode(depth_test, depth_update, depth_compare);

    if (depth_source_prim && !s_warned_depth_prim) {
        sysLogPrintf(LOG_WARNING,
            "GfxPS2 primitive depth source requested; bring-up backend still consumes per-vertex clip Z");
        s_warned_depth_prim = true;
    }
}

static void ps2_set_depth_range(float znear, float zfar)
{
    s_depth_near = znear;
    s_depth_far = zfar;
}

static void ps2_set_viewport(int x, int y, int width, int height)
{
    s_viewport.x = x;
    s_viewport.y = y;
    s_viewport.width = width;
    s_viewport.height = height;
}

static void ps2_set_scissor(int x, int y, int width, int height)
{
    ps2GsCoreSetScissor(x, y, width, height);
}

static void ps2_set_use_alpha(bool use_alpha, bool modulate)
{
    s_use_alpha = use_alpha;
    s_modulate = modulate;
    ps2GsCoreSetAlphaBlend(use_alpha);
}

static float ps2_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint8_t ps2_color_component(float v)
{
    /* GS colour modulation uses 0x80 as unity. */
    int out = (int)(ps2_clampf(v, 0.0f, 1.0f) * 128.0f + 0.5f);
    if (out < 0) out = 0;
    if (out > 128) out = 128;
    return (uint8_t)out;
}

static int ps2_map_clip_depth(float z, float w)
{
    float ndc = (w != 0.0f) ? (z / w) : 1.0f;
    ndc = ps2_clampf(ndc, 0.0f, 1.0f);

    float ranged = s_depth_near + ndc * (s_depth_far - s_depth_near);
    ranged = ps2_clampf(ranged, 0.0f, 1.0f);

    /* Current GS baseline uses GEQUAL: near is large, far is small. */
    return 1 + (int)((1.0f - ranged) * 65534.0f);
}

static size_t ps2_vbo_stride(const struct ShaderProgram *prg)
{
    if (!prg) {
        return 0;
    }

    size_t stride = 4;
    for (int t = 0; t < 2; ++t) {
        if (!prg->features.used_textures[t]) {
            continue;
        }
        stride += 2;
        if (prg->features.clamp[t][0]) ++stride;
        if (prg->features.clamp[t][1]) ++stride;
    }
    if (prg->features.opt_fog) stride += 4;
    if (prg->features.opt_grayscale) stride += 4;
    stride += (size_t)prg->features.num_inputs * (prg->features.opt_alpha ? 4u : 3u);
    return stride;
}

static void ps2_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris)
{
    if (!s_gs || !s_shader || !s_shader->supported || !buf_vbo || buf_vbo_num_tris == 0) {
        return;
    }

    const size_t stride = ps2_vbo_stride(s_shader);
    const size_t vertex_count = buf_vbo_num_tris * 3;
    if (stride == 0 || buf_vbo_len < vertex_count * stride) {
        return;
    }

    struct Ps2TextureSlot *texture_slot = NULL;
    if (s_shader->textured) {
        texture_slot = ps2_texture_slot(s_selected_texture[0]);
        if (!texture_slot || !texture_slot->uploaded) {
            return;
        }
    }

    size_t base_vertex = 0;
    while (base_vertex < vertex_count) {
        size_t batch_vertices = vertex_count - base_vertex;
        if (batch_vertices > PS2_GFX_TRANSLATE_VERTS) {
            batch_vertices = PS2_GFX_TRANSLATE_VERTS;
            batch_vertices -= batch_vertices % 3;
        }

        for (size_t i = 0; i < batch_vertices; ++i) {
            const float *src = &buf_vbo[(base_vertex + i) * stride];
            size_t pos = 0;
            const float clip_x = src[pos++];
            const float clip_y = src[pos++];
            const float clip_z = src[pos++];
            const float clip_w = src[pos++];
            const float inv_w = clip_w != 0.0f ? 1.0f / clip_w : 0.0f;
            const float ndc_x = clip_x * inv_w;
            const float ndc_y = clip_y * inv_w;

            float tex_u[2] = { 0.0f, 0.0f };
            float tex_v[2] = { 0.0f, 0.0f };
            for (int t = 0; t < 2; ++t) {
                if (!s_shader->features.used_textures[t]) {
                    continue;
                }
                tex_u[t] = src[pos++];
                tex_v[t] = src[pos++];
                if (s_shader->features.clamp[t][0]) ++pos;
                if (s_shader->features.clamp[t][1]) ++pos;
            }
            if (s_shader->features.opt_fog) pos += 4;
            if (s_shader->features.opt_grayscale) pos += 4;

            float r = 1.0f;
            float g = 1.0f;
            float b = 1.0f;
            float a = 1.0f;
            if (s_shader->features.num_inputs > 0) {
                r = src[pos++];
                g = src[pos++];
                b = src[pos++];
                if (s_shader->features.opt_alpha) {
                    a = src[pos++];
                }
            }

            const float sx = (float)s_viewport.x + (ndc_x * 0.5f + 0.5f) * (float)s_viewport.width;
            const float sy = (float)s_viewport.y + (0.5f - ndc_y * 0.5f) * (float)s_viewport.height;
            const int iz = ps2_map_clip_depth(clip_z, clip_w);
            const uint8_t cr = ps2_color_component(r);
            const uint8_t cg = ps2_color_component(g);
            const uint8_t cb = ps2_color_component(b);
            const uint8_t ca = s_use_alpha ? ps2_color_component(a) : 0x80;

            if (s_shader->textured) {
                s_stq_vertices[i].rgbaq = color_to_RGBAQ(cr, cg, cb, ca, inv_w);
                s_stq_vertices[i].stq = vertex_to_STQ(tex_u[0] * inv_w, tex_v[0] * inv_w);
                s_stq_vertices[i].xyz2 = vertex_to_XYZ2(s_gs, sx, sy, iz);
            } else {
                s_color_vertices[i].rgbaq = color_to_RGBAQ(cr, cg, cb, ca, 0.0f);
                s_color_vertices[i].xyz2 = vertex_to_XYZ2(s_gs, sx, sy, iz);
            }
        }

        if (s_shader->textured) {
            GSTEXTURE *tex = &texture_slot->texture;
            gsKit_set_texfilter(s_gs, tex->Filter);
            gsKit_prim_list_triangle_goraud_texture_stq_3d(
                s_gs, tex, (int)batch_vertices, s_stq_vertices);
        } else {
            gsKit_prim_list_triangle_gouraud_3d(
                s_gs, (int)batch_vertices, s_color_vertices);
        }

        base_vertex += batch_vertices;
    }
}

static void ps2_init(void)
{
    ps2_clear_shaders();
    memset(s_textures, 0, sizeof(s_textures));
    s_selected_texture[0] = 0;
    s_selected_texture[1] = 0;
    s_active_texture_tile = 0;
    s_depth_near = 0.0f;
    s_depth_far = 1.0f;
    s_depth_test = true;
    s_depth_update = true;
    s_use_alpha = false;
    s_modulate = false;
    s_filter_mode = FILTER_LINEAR;
    s_mipmap_filter = MIPMAP_DISABLED;
    s_anisotropy = 1;

    if (s_gs) {
        s_viewport.x = 0;
        s_viewport.y = 0;
        s_viewport.width = s_gs->Width;
        s_viewport.height = s_gs->Height;
    }

    sysLogPrintf(LOG_NOTE,
        "GfxPS2 init: fixed shader=%d texture=%d translate_batch=%d",
        PS2_GFX_MAX_SHADERS, PS2_GFX_MAX_TEXTURES, PS2_GFX_TRANSLATE_VERTS);
}

static void ps2_on_resize(void)
{
    if (s_gs) {
        s_viewport.x = 0;
        s_viewport.y = 0;
        s_viewport.width = s_gs->Width;
        s_viewport.height = s_gs->Height;
    }
}

static void ps2_start_frame(void)
{
    ps2GsCoreBeginFrame();
}

static void ps2_end_frame(void)
{
    ps2GsCoreSubmit();
}

static void ps2_finish_render(void)
{
    /*
     * Intentionally no unconditional GS FINISH here. Whole-system scheduling
     * treats FINISH as a dependency/profiling tool, not a per-frame ritual.
     * Buffer presentation belongs to the PS2 window-manager backend.
     */
}

static int ps2_create_framebuffer(void)
{
    if (!s_warned_framebuffer) {
        sysLogPrintf(LOG_WARNING,
            "GfxPS2 offscreen framebuffer API is not implemented; default scanout only");
        s_warned_framebuffer = true;
    }
    return 0;
}

static void ps2_update_framebuffer_parameters(int fb_id, uint32_t width, uint32_t height,
                                               uint32_t msaa_level, bool opengl_invert_y,
                                               bool render_target, bool has_depth_buffer,
                                               bool can_extract_depth)
{
    (void)fb_id; (void)width; (void)height; (void)msaa_level;
    (void)opengl_invert_y; (void)render_target; (void)has_depth_buffer; (void)can_extract_depth;
}

static bool ps2_start_draw_to_framebuffer(int fb_id, float noise_scale)
{
    (void)noise_scale;
    return fb_id == 0;
}

static void ps2_copy_framebuffer(int fb_dst, int fb_src, int left, int top, bool flip_y, bool use_back)
{
    (void)fb_dst; (void)fb_src; (void)left; (void)top; (void)flip_y; (void)use_back;
}

static void ps2_clear_framebuffer(bool clear_color, bool clear_depth)
{
    ps2GsCoreClear(clear_color, clear_depth);
}

static void ps2_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source)
{
    (void)fb_id_target; (void)fb_id_source;
}

static void *ps2_get_framebuffer_texture_id(int fb_id)
{
    (void)fb_id;
    return NULL;
}

static void ps2_select_texture_fb(int fb_id)
{
    (void)fb_id;
}

static void ps2_delete_texture(uint32_t tex_id)
{
    struct Ps2TextureSlot *slot = ps2_texture_slot(tex_id);
    if (slot) {
        /* VRAM is monotonic in this baseline; only retire the logical ID. */
        slot->used = false;
    }
}

static void ps2_set_texture_filter(enum FilteringMode mode)
{
    s_filter_mode = mode;
}

static enum FilteringMode ps2_get_texture_filter(void)
{
    return s_filter_mode;
}

static void ps2_set_mipmap_filter(enum MipmapFilteringMode mode)
{
    s_mipmap_filter = mode;
}

static void ps2_set_anisotropy_level(int level)
{
    s_anisotropy = level;
}

static int ps2_get_max_anisotropy_level(void)
{
    return 1;
}

extern "C" struct GfxRenderingAPI gfx_ps2_api = {
    ps2_get_name,
    ps2_get_max_texture_size,
    ps2_get_clip_parameters,
    ps2_unload_shader,
    ps2_load_shader,
    ps2_create_and_load_new_shader,
    ps2_lookup_shader,
    ps2_shader_get_info,
    ps2_clear_shaders,
    ps2_new_texture,
    ps2_select_texture,
    ps2_upload_texture,
    ps2_set_sampler_parameters,
    ps2_set_depth_mode,
    ps2_set_depth_range,
    ps2_set_viewport,
    ps2_set_scissor,
    ps2_set_use_alpha,
    ps2_draw_triangles,
    ps2_init,
    ps2_on_resize,
    ps2_start_frame,
    ps2_end_frame,
    ps2_finish_render,
    ps2_create_framebuffer,
    ps2_update_framebuffer_parameters,
    ps2_start_draw_to_framebuffer,
    ps2_copy_framebuffer,
    ps2_clear_framebuffer,
    ps2_resolve_msaa_color_buffer,
    ps2_get_framebuffer_texture_id,
    ps2_select_texture_fb,
    ps2_delete_texture,
    ps2_set_texture_filter,
    ps2_get_texture_filter,
    ps2_set_mipmap_filter,
    ps2_set_anisotropy_level,
    ps2_get_max_anisotropy_level,
};