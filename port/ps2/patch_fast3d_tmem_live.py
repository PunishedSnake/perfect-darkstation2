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
        '#include "gfx_screen_config.h"\n#include "rdp_tmem_live.h"\n',
        "include",
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
        "        key.content_identity = tmem_view.content_identity;\n"
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
