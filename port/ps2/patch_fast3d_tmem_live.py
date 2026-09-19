#!/usr/bin/env python3
"""Generate the PS2 Fast3D frontend with ordered live-TMEM hooks.

This is a deliberately narrow migration seam. It keeps the shared portable
`port/fast3d/gfx_pc.cpp` source byte-for-byte intact while the PS2 backend moves
texture identity from host source pointers to RDP TMEM semantics. Every edit is
an exact one-shot replacement. If upstream Fast3D changes around a hook point,
the build fails rather than silently compiling a partially instrumented copy.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            f"Fast3D live-TMEM hook '{label}' expected exactly one match, got {count}"
        )
    return text.replace(old, new, 1)


def patch(source: str) -> str:
    source = replace_once(
        source,
        '#include "gfx_screen_config.h"\n',
        '#include "gfx_screen_config.h"\n#include "gfx_ps2.h"\n#include "rdp_tmem_live.h"\n',
        "include",
    )

    source = replace_once(
        source,
        "static inline int gfx_lod_tile_offset(const int i) {\n"
        "    if (gfx_detail_textures_enabled)\n"
        "        return ((rdp.tex_lod && !rdp.tex_detail) ? 0 : i);\n"
        "    return (rdp.tex_lod ? rdp.tex_detail : i);\n"
        "}\n",
        "static inline int gfx_lod_tile_offset(const int i) {\n"
        "    /*\n"
        "     * The PS2 backend does not yet own a complete GS mip chain.\n"
        "     * Keep ordinary LOD on the known-good base tile so TEXEL1 cannot\n"
        "     * sample an absent/stale adjacent mip. Real detail-texture mode\n"
        "     * still exposes tile i and therefore retains its two textures.\n"
        "     */\n"
        "    if (gfx_detail_textures_enabled)\n"
        "        return ((rdp.tex_lod && !rdp.tex_detail) ? 0 : i);\n"
        "    return (rdp.tex_lod ? rdp.tex_detail : i);\n"
        "}\n",
        "native GS safe LOD tile selection",
    )

    source = replace_once(
        source,
        "    if ((rsp.geometry_mode & G_CULL_BOTH) != 0) {\n",
        "    if ((rsp.geometry_mode & G_CULL_BOTH) != 0\n"
        "        /* Screen-space winding is undefined before homogeneous\n"
        "         * clipping when a triangle reaches or crosses the eye plane.\n"
        "         * Let the PS2 backend clip those triangles first instead of\n"
        "         * dividing by a non-positive W and rejecting valid world\n"
        "         * geometry. Fully visible triangles keep the cheap frontend\n"
        "         * cull. */\n"
        "        && v1->w > 0.0f && v2->w > 0.0f && v3->w > 0.0f\n"
        "#if defined(PERFECT_DARK_PS2_DISABLE_CULLING)\n"
        "        && false\n"
        "#endif\n"
        "    ) {\n",
        "PS2 culling diagnostic gate",
    )

    source = replace_once(
        source,
        "static void gfx_run_dl(Gfx* cmd) {\n",
        "static void gfx_run_dl(Gfx* cmd, uint32_t ps2_trace_depth) {\n",
        "display-list recursion depth",
    )

    source = replace_once(
        source,
        "    for (;;) {\n"
        "        uint32_t opcode = cmd->words.w0 >> 24;\n"
        "        // gfx_print_cmd(cmd);\n",
        "    for (;;) {\n"
        "        uint32_t opcode = cmd->words.w0 >> 24;\n"
        "        uint32_t ps2_trace_entries = 1u;\n"
        "        if (opcode == G_TEXRECT || opcode == G_TEXRECTFLIP ||\n"
        "            opcode == G_TEXRECT_WIDE_EXT || opcode == G_IMAGERECT_EXT) {\n"
        "            ps2_trace_entries = 3u;\n"
        "        } else if (opcode == G_FILLRECT_WIDE_EXT) {\n"
        "            ps2_trace_entries = 2u;\n"
        "        }\n"
        "        gfxPs2TraceGfxCommands(cmd, ps2_trace_depth, ps2_trace_entries);\n"
        "        // gfx_print_cmd(cmd);\n",
        "raw display-list command stream",
    )

    source = replace_once(
        source,
        "            case G_MTX: {\n"
        "                gfx_sp_matrix(C0(16, 8), (const int32_t*)seg_addr(cmd->words.w1));\n"
        "                break;\n"
        "            }\n",
        "            case G_MTX: {\n"
        "                const void *ps2_mtx_source = seg_addr(cmd->words.w1);\n"
        "                gfxPs2TraceGfxSource(1u, ps2_mtx_source, 64u, C0(16, 8));\n"
        "                gfx_sp_matrix(C0(16, 8), (const int32_t*)ps2_mtx_source);\n"
        "                break;\n"
        "            }\n",
        "matrix source payload",
    )

    source = replace_once(
        source,
        "            case G_VTX:\n"
        "                gfx_sp_vertex(C0(0, 16) / sizeof(Vtx), C0(16, 4), (const Vtx*)seg_addr(cmd->words.w1));\n"
        "                break;\n",
        "            case G_VTX: {\n"
        "                const uint32_t ps2_vtx_count = C0(16, 4);\n"
        "                const Vtx *ps2_vtx_source = (const Vtx*)seg_addr(cmd->words.w1);\n"
        "                gfxPs2TraceGfxSource(2u, ps2_vtx_source,\n"
        "                    ps2_vtx_count * (uint32_t)sizeof(Vtx),\n"
        "                    (uint64_t)ps2_vtx_count |\n"
        "                        ((uint64_t)(C0(0, 16) / sizeof(Vtx)) << 32u));\n"
        "                gfx_sp_vertex(C0(0, 16) / sizeof(Vtx),\n"
        "                    ps2_vtx_count, ps2_vtx_source);\n"
        "                break;\n"
        "            }\n",
        "vertex source payload",
    )

    source = replace_once(
        source,
        "                        gfx_run_dl(subGFX);\n",
        "                        gfx_run_dl(subGFX, ps2_trace_depth + 1u);\n",
        "nested display-list depth",
    )

    source = replace_once(
        source,
        "            case G_COL:\n"
        "                gfx_sp_set_vertex_colors(C0(0, 16) / 4, (NormalColor *)seg_addr(cmd->words.w1));\n"
        "                break;\n",
        "            case G_COL: {\n"
        "                const uint32_t ps2_color_count = C0(0, 16) / 4u;\n"
        "                const NormalColor *ps2_color_source =\n"
        "                    (const NormalColor *)seg_addr(cmd->words.w1);\n"
        "                gfxPs2TraceGfxSource(3u, ps2_color_source,\n"
        "                    ps2_color_count * (uint32_t)sizeof(NormalColor),\n"
        "                    ps2_color_count);\n"
        "                gfx_sp_set_vertex_colors(\n"
        "                    ps2_color_count, ps2_color_source);\n"
        "                break;\n"
        "            }\n",
        "vertex color source payload",
    )

    source = replace_once(
        source,
        "    gfx_run_dl(commands);\n",
        "    gfx_run_dl(commands, 0u);\n",
        "top-level display-list depth",
    )

    source = replace_once(
        source,
        "static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, uint32_t tex_flags, const void* addr) {\n"
        "    rdp.texture_to_load.addr = (const uint8_t*)addr;\n",
        "static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, uint32_t tex_flags, const void* addr) {\n"
        "    gfxRdpTmemLiveSetTextureImage(format, size, width, addr);\n"
        "    rdp.texture_to_load.addr = (const uint8_t*)addr;\n",
        "SetTextureImage",
    )

    source = replace_once(
        source,
        "static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette,\n"
        "                            uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks,\n"
        "                            uint32_t shifts) {\n"
        "    // OTRTODO:\n",
        "static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette,\n"
        "                            uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks,\n"
        "                            uint32_t shifts) {\n"
        "    // Capture raw RDP fmt/siz before portable compatibility remapping.\n"
        "    gfxRdpTmemLiveSetTile(fmt, siz, line, tmem, tile);\n"
        "    // OTRTODO:\n",
        "SetTile",
    )

    source = replace_once(
        source,
        "static void gfx_dp_load_tlut(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {\n"
        "    // SUPPORT_CHECK(tile == G_TX_LOADTILE);\n",
        "static void gfx_dp_load_tlut(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {\n"
        "    gfxRdpTmemLiveLoadTlut(tile, uls, ult, lrs, lrt);\n"
        "    // SUPPORT_CHECK(tile == G_TX_LOADTILE);\n",
        "LoadTLUT",
    )

    source = replace_once(
        source,
        "static void gfx_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {\n"
        "    // SUPPORT_CHECK(tile == G_TX_LOADTILE);\n",
        "static void gfx_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {\n"
        "    gfxRdpTmemLiveLoadBlock(tile, uls, ult, lrs, dxt);\n"
        "    // SUPPORT_CHECK(tile == G_TX_LOADTILE);\n",
        "LoadBlock",
    )

    source = replace_once(
        source,
        "static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {\n"
        "    SUPPORT_CHECK(tile == G_TX_LOADTILE);\n",
        "static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {\n"
        "    gfxRdpTmemLiveLoadTile(tile, uls, ult, lrs, lrt);\n"
        "    SUPPORT_CHECK(tile == G_TX_LOADTILE);\n",
        "LoadTile",
    )

    source = replace_once(
        source,
        "    TextureCacheKey key;\n"
        "    if (fmt == G_IM_FMT_CI) {\n"
        "        key = { orig_addr, { rdp.palette_addrs[0], rdp.palette_addrs[1] }, fmt, siz, palette_index };\n"
        "    } else {\n"
        "        key = { orig_addr, {}, fmt, siz, palette_index };\n"
        "    }\n\n"
        "    if (gfx_texture_cache_lookup(i, key)) {\n",
        "    TextureCacheKey key;\n"
        "    if (fmt == G_IM_FMT_CI) {\n"
        "        key = { orig_addr, { rdp.palette_addrs[0], rdp.palette_addrs[1] }, fmt, siz, palette_index };\n"
        "    } else {\n"
        "        key = { orig_addr, {}, fmt, siz, palette_index };\n"
        "    }\n\n"
        "    GfxRdpTmemLiveTextureView tmem_view{};\n"
        "    const bool tmem_view_exact = gfxRdpTmemLiveMaterializeTexture(\n"
        "        tile, loaded_texture.line_size_bytes, loaded_texture.size_bytes,\n"
        "        fmt, siz, palette_index, &tmem_view);\n"
        "    if (tmem_view_exact) {\n"
        "        key.texture_addr = nullptr;\n"
        "        key.palette_addrs[0] = nullptr;\n"
        "        key.palette_addrs[1] = nullptr;\n"
        "        key.content_identity = gfxPs2TextureVariantIdentity(\n"
        "            tmem_view.content_identity,\n"
        "            fmt,\n"
        "            rdp.texture_tile[tile].cms,\n"
        "            rdp.texture_tile[tile].cmt,\n"
        "            rdp.palette_fmt);\n"
        "        key.content_identity_valid = true;\n"
        "    }\n\n"
        "    if (gfx_texture_cache_lookup(i, key)) {\n",
        "TextureCacheKey live TMEM view",
    )

    source = replace_once(
        source,
        "    if (gfx_texture_cache_lookup(i, key)) {\n"
        "        return;\n"
        "    }\n\n"
        "    if (fmt == G_IM_FMT_RGBA) {\n",
        "    if (gfx_texture_cache_lookup(i, key)) {\n"
        "        if (tmem_view_exact && rendering_state.textures[i] != nullptr) {\n"
        "            gfxPs2TraceTmemTextureView(\n"
        "                rendering_state.textures[i]->second.texture_id,\n"
        "                &tmem_view, fmt, siz, rdp.palette_fmt);\n"
        "        }\n"
        "        return;\n"
        "    }\n\n"
        "    LoadedTexture tmem_loaded_texture = loaded_texture;\n"
        "    const LoadedTexture* texture_to_import = &loaded_texture;\n"
        "    if (tmem_view_exact) {\n"
        "        tmem_loaded_texture.addr = tmem_view.texels;\n"
        "        tmem_loaded_texture.orig_size_bytes = tmem_view.size_bytes;\n"
        "        tmem_loaded_texture.full_size_bytes = tmem_view.size_bytes;\n"
        "        tmem_loaded_texture.size_bytes = tmem_view.size_bytes;\n"
        "        tmem_loaded_texture.full_image_line_size_bytes =\n"
        "            tmem_view.line_size_bytes;\n"
        "        tmem_loaded_texture.line_size_bytes = tmem_view.line_size_bytes;\n"
        "        if (tmem_view.palette_count != 0) {\n"
        "            memcpy(rdp.palette + tmem_view.palette_first, tmem_view.palette,\n"
        "                (size_t)tmem_view.palette_count * sizeof(rdp.palette[0]));\n"
        "        }\n"
        "        texture_to_import = &tmem_loaded_texture;\n"
        "    }\n\n"
        "    if (fmt == G_IM_FMT_RGBA) {\n",
        "authoritative TMEM importer view",
    )

    source = replace_once(
        source,
        "        texture_to_import = &tmem_loaded_texture;\n"
        "    }\n\n"
        "    if (fmt == G_IM_FMT_RGBA) {\n",
        "        texture_to_import = &tmem_loaded_texture;\n"
        "    }\n\n"
        "    gfxPs2SetTextureUploadMirror(\n"
        "        rdp.texture_tile[tile].cms, rdp.texture_tile[tile].cmt);\n"
        "    if (tmem_view_exact && gfxPs2UploadTmemTexture(\n"
        "            &tmem_view, fmt, siz, rdp.palette_fmt, rdp.tex_lod)) {\n"
        "        return;\n"
        "    }\n\n"
        "    if (fmt == G_IM_FMT_RGBA) {\n",
        "native PS2 TMEM texture upload",
    )

    for importer in (
        "import_texture_rgba16",
        "import_texture_rgba32",
        "import_texture_ia4",
        "import_texture_ia8",
        "import_texture_ia16",
        "import_texture_ci4",
        "import_texture_ci8",
        "import_texture_i4",
        "import_texture_i8",
    ):
        source = replace_once(
            source,
            f"{importer}(tile, loaded_texture, rdp.tex_lod);",
            f"{importer}(tile, *texture_to_import, rdp.tex_lod);",
            f"{importer} live TMEM view",
        )

    source = replace_once(
        source,
        'extern "C" void gfx_init(const GfxInitSettings *settings) {\n'
        "    gfx_wapi = settings->wapi;\n",
        'extern "C" void gfx_init(const GfxInitSettings *settings) {\n'
        "    gfxRdpTmemLiveReset();\n"
        "    gfx_wapi = settings->wapi;\n",
        "gfx_init reset",
    )

    return source


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    original = args.input.read_text(encoding="utf-8")
    generated = patch(original)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
