#!/usr/bin/env python3
"""Decode a one-frame Perfect DarkStation 2 renderer/GS capture."""

from __future__ import annotations

import argparse
import collections
import json
import struct
import sys
from pathlib import Path

HEADER = struct.Struct("<8s6I2Q14I")
EVENT = struct.Struct("<IHHQQQQQ")
QWORD = struct.Struct("<QQ")

EVENT_NAMES = {
    1: "frame_begin", 2: "frame_end", 3: "shader",
    4: "texture_select", 5: "texture_upload", 6: "sampler",
    7: "depth", 8: "viewport", 9: "scissor", 10: "alpha",
    11: "draw_input", 12: "draw_clipped", 13: "path1_submit",
    14: "path3_submit", 15: "core_summary", 16: "gs_register",
    17: "texture_resource", 18: "texture_clut",
    19: "render_target", 20: "vram", 21: "renderer_stats",
    22: "frontend_state", 23: "warning", 24: "pass_graph_draw",
    25: "independent_alpha_draw", 26: "clipped_triangle_bounds",
    27: "texture_coord_range", 28: "draw_state",
    29: "draw_payload", 30: "texture_detail",
    31: "build_config", 32: "queue_wait", 33: "gs_draw",
    34: "resource_op",
}

GS_STATE_NAMES = [
    "TEST", "ZBUF", "FRAME", "FBA", "PABE", "ALPHA", "FOGCOL",
    "CLAMP", "TEXA", "SCISSOR", "TEX0", "TEX1", "PRIM",
]

GS_REGISTER_NAMES = {
    0x00: "PRIM", 0x01: "RGBAQ", 0x02: "ST", 0x03: "UV",
    0x04: "XYZF2", 0x05: "XYZ2", 0x06: "TEX0_1", 0x07: "TEX0_2",
    0x08: "CLAMP_1", 0x09: "CLAMP_2", 0x0A: "FOG", 0x14: "TEX1_1",
    0x15: "TEX1_2", 0x18: "XYOFFSET_1", 0x19: "XYOFFSET_2",
    0x3B: "TEXA", 0x3D: "FOGCOL", 0x40: "SCISSOR_1",
    0x41: "SCISSOR_2", 0x42: "ALPHA_1", 0x43: "ALPHA_2",
    0x45: "DTHE", 0x46: "COLCLAMP", 0x47: "TEST_1", 0x48: "TEST_2",
    0x4A: "PABE", 0x4C: "FRAME_1", 0x4D: "FRAME_2",
    0x4E: "ZBUF_1", 0x4F: "ZBUF_2", 0x50: "BITBLTBUF",
    0x51: "TRXPOS", 0x52: "TRXREG", 0x53: "TRXDIR", 0x54: "HWREG",
}

PASS_GRAPH_NAMES = {
    0: "direct",
    1: "opaque_trilerp",
    2: "opaque_input1_tex0_lerp",
    3: "independent_tex0_alpha",
    4: "trilerp_independent_alpha",
    5: "alpha_trilerp_modulate",
    6: "tex0_factor_lerp",
    7: "tex1_alpha_factor_lerp",
    8: "interference",
}


def _hex(value: int) -> str:
    return f"0x{value:016x}"


def _event_value(event: dict, key: str) -> int:
    return int(event[key], 16)


def _u32_pair(value: int) -> tuple[int, int]:
    return value & 0xffffffff, value >> 32


def _float_pair(value: int) -> tuple[float, float]:
    return struct.unpack("<ff", value.to_bytes(8, "little"))


def _s32(value: int) -> int:
    value &= 0xffffffff
    return value - 0x100000000 if value & 0x80000000 else value


def _fnv1a64(data: bytes) -> int:
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def _analyze(events: list[dict]) -> dict:
    paths = {
        "path1": {"submits": 0, "requested_qwords": 0,
                  "stored_qwords": 0, "uncaptured_submits": 0},
        "path3": {"submits": 0, "requested_qwords": 0,
                  "stored_qwords": 0, "uncaptured_submits": 0},
    }
    draw_totals = {
        "input_draws": 0,
        "input_triangles": 0,
        "input_vertices": 0,
        "clipped_vertices": 0,
        "fully_clipped_draws": 0,
        "fully_clipped_triangles": 0,
    }
    by_pass = collections.defaultdict(lambda: {
        "draws": 0, "input_triangles": 0, "clipped_vertices": 0,
        "duration_microseconds": 0,
        "path1_submits": 0, "path1_requested_qwords": 0,
        "path3_submits": 0, "path3_requested_qwords": 0,
    })
    current_pass = 0
    current_shader = None
    unsupported_shaders = collections.defaultdict(lambda: {
        "draws": 0, "input_triangles": 0, "clipped_vertices": 0,
    })
    active_draw = None
    draw_records = []
    pass_graph_draws = []
    pending_pass_graph_draw = None
    independent_alpha_draws = []
    clipped_triangle_bounds = []
    texture_coord_ranges = []
    draw_states = collections.defaultdict(dict)
    draw_payloads = []
    texture_details = []
    build_config = {}
    queue_waits = []
    gs_draws = []
    resource_ops = []
    gaps = []

    for previous, current in zip(events, events[1:]):
        delta = current["microseconds"] - previous["microseconds"]
        gaps.append({
            "microseconds": delta,
            "after_sequence": previous["sequence"],
            "after_type": previous["type"],
            "before_type": current["type"],
        })

    for event in events:
        event_type = event["type"]
        if event_type == "shader":
            current_pass = _event_value(event, "c") >> 32
            current_shader = {
                "id0": event["a"],
                "id1": event["b"],
                "supported": bool(event["flags"] & 8),
            }
        elif event_type == "draw_input":
            active_draw = {
                "pass_graph": current_pass,
                "shader": current_shader,
                "start": event["microseconds"],
                "start_sequence": event["sequence"],
                "triangles": _event_value(event, "a"),
                "path1_submits": 0,
                "path1_requested_qwords": 0,
                "path3_submits": 0,
                "path3_requested_qwords": 0,
            }
            draw_totals["input_draws"] += 1
            draw_totals["input_triangles"] += active_draw["triangles"]
            draw_totals["input_vertices"] += _event_value(event, "b")
        elif event_type in ("path1_submit", "path3_submit"):
            path_name = "path1" if event_type == "path1_submit" else "path3"
            requested = _event_value(event, "c")
            stored = _event_value(event, "b")
            paths[path_name]["submits"] += 1
            paths[path_name]["requested_qwords"] += requested
            paths[path_name]["stored_qwords"] += stored
            if stored == 0:
                paths[path_name]["uncaptured_submits"] += 1
            if active_draw is not None:
                active_draw[f"{path_name}_submits"] += 1
                active_draw[f"{path_name}_requested_qwords"] += requested
        elif event_type == "pass_graph_draw":
            if event["flags"] & 0x0100:
                if pending_pass_graph_draw is not None:
                    ranges = _event_value(event, "c")
                    pending_pass_graph_draw["shader"] = {
                        "id0": event["a"],
                        "id1": event["b"],
                    }
                    pending_pass_graph_draw["lod_min"] = ranges & 0xff
                    pending_pass_graph_draw["lod_max"] = (ranges >> 8) & 0xff
                    pending_pass_graph_draw["shade_alpha_min"] = (
                        ranges >> 16) & 0xff
                    pending_pass_graph_draw["shade_alpha_max"] = (
                        ranges >> 24) & 0xff
                    pending_pass_graph_draw["add_alpha_min"] = (
                        ranges >> 32) & 0xff
                    pending_pass_graph_draw["add_alpha_max"] = (
                        ranges >> 40) & 0xff
                    pending_pass_graph_draw["success"] = bool(
                        (ranges >> 48) & 1)
                    pending_pass_graph_draw["additive_alpha"] = bool(
                        (ranges >> 49) & 1)
                    pending_pass_graph_draw["direct_opaque_triangles"] = (
                        ranges >> 50) & 0x7f
                    pending_pass_graph_draw["vertex_alpha_triangles"] = (
                        ranges >> 57) & 0x7f
                    pending_pass_graph_draw["same_sample_triangles"] = (
                        _event_value(event, "d") & 0xffffffff)
                    pass_graph_draws.append(pending_pass_graph_draw)
                    pending_pass_graph_draw = None
            else:
                bbox_min = _event_value(event, "c")
                bbox_max = _event_value(event, "d")
                def signed_fixed_16_4(value):
                    value &= 0xffffffff
                    if value & 0x80000000:
                        value -= 0x100000000
                    return value / 16.0
                pending_pass_graph_draw = {
                    "sequence": event["sequence"],
                    "vertices": _event_value(event, "a"),
                    "tiles": _event_value(event, "b"),
                    "bbox": {
                        "min_x": signed_fixed_16_4(bbox_min),
                        "min_y": signed_fixed_16_4(bbox_min >> 32),
                        "max_x": signed_fixed_16_4(bbox_max),
                        "max_y": signed_fixed_16_4(bbox_max >> 32),
                    },
                }
        elif event_type == "independent_alpha_draw":
            mode = _event_value(event, "c")
            independent_alpha_draws.append({
                "sequence": event["sequence"],
                "vertices": _event_value(event, "a"),
                "triangles": _event_value(event, "b"),
                "mode": mode,
                "direct": mode == 1,
                "alpha_mask": mode == 2,
            })
        elif event_type == "texture_coord_range":
            def float_pair(value):
                raw = value.to_bytes(8, "little")
                return struct.unpack("<ff", raw)
            slot, handle = (
                _event_value(event, "a") & 0xffffffff,
                _event_value(event, "a") >> 32,
            )
            min_u, max_u = float_pair(_event_value(event, "b"))
            min_v, max_v = float_pair(_event_value(event, "c"))
            width = _event_value(event, "d") & 0xffffffff
            height = _event_value(event, "d") >> 32
            texture_coord_ranges.append({
                "sequence": event["sequence"],
                "draw_id": active_draw.get("draw_id") if active_draw else None,
                "slot": slot,
                "handle": handle,
                "min_u": min_u,
                "max_u": max_u,
                "min_v": min_v,
                "max_v": max_v,
                "width": width,
                "height": height,
                "linear": bool(event["flags"] & 0x0100),
                "region_s": bool(event["flags"] & 0x0200),
                "region_t": bool(event["flags"] & 0x0400),
                "filter_mode": (event["flags"] >> 12) & 0x3,
            })
        elif event_type == "clipped_triangle_bounds":
            def signed_fixed_16_4(value):
                value &= 0xffffffff
                if value & 0x80000000:
                    value -= 0x100000000
                return value / 16.0
            bbox_min = _event_value(event, "c")
            bbox_max = _event_value(event, "d")
            clipped_triangle_bounds.append({
                "sequence": event["sequence"],
                "draw_id": active_draw.get("draw_id") if active_draw else None,
                "source_triangle": _event_value(event, "a"),
                "output_triangle": _event_value(event, "b"),
                "bbox": {
                    "min_x": signed_fixed_16_4(bbox_min),
                    "min_y": signed_fixed_16_4(bbox_min >> 32),
                    "max_x": signed_fixed_16_4(bbox_max),
                    "max_y": signed_fixed_16_4(bbox_max >> 32),
                },
                "screen_wide": bool(event["flags"] & 0x0100),
                "screen_tall": bool(event["flags"] & 0x0200),
                "thin": bool(event["flags"] & 0x0400),
                "near_zero_w": bool(event["flags"] & 0x0800),
            })
        elif event_type == "draw_state":
            subtype = event["flags"] & 0xff
            draw_id = _event_value(event, "a") & 0xffffffff
            state = draw_states[draw_id]
            state["draw_id"] = draw_id
            if active_draw is not None and "draw_id" not in active_draw:
                active_draw["draw_id"] = draw_id
            b = _event_value(event, "b")
            c = _event_value(event, "c")
            value_d = _event_value(event, "d")
            if subtype == 0:
                tex0, tex1 = _u32_pair(b)
                state["textures"] = [tex0, tex1]
                state["filter_mode"] = c & 0xff
                state["mipmap_filter"] = (c >> 8) & 0xff
                state["anisotropy"] = (c >> 16) & 0xffff
                state["active_texture_tile"] = (c >> 32) & 0xffffffff
                state["flags"] = {
                    "depth_test": bool(value_d & (1 << 0)),
                    "depth_update": bool(value_d & (1 << 1)),
                    "depth_compare": bool(value_d & (1 << 2)),
                    "depth_equal": bool(value_d & (1 << 3)),
                    "depth_decal": bool(value_d & (1 << 4)),
                    "alpha_blend": bool(value_d & (1 << 5)),
                    "modulate": bool(value_d & (1 << 6)),
                    "alpha_threshold": bool(value_d & (1 << 7)),
                    "texture_edge": bool(value_d & (1 << 8)),
                    "fog": bool(value_d & (1 << 9)),
                    "invisible": bool(value_d & (1 << 10)),
                    "two_cycle": bool(value_d & (1 << 11)),
                }
                state["fog_rgb"] = [
                    (value_d >> 16) & 0xff,
                    (value_d >> 24) & 0xff,
                    (value_d >> 32) & 0xff,
                ]
            elif subtype == 1:
                vx, vy = _u32_pair(b)
                vw, vh = _u32_pair(c)
                state["viewport"] = {
                    "x": _s32(vx), "y": _s32(vy),
                    "width": _s32(vw), "height": _s32(vh),
                }
                state["depth_range"] = list(_float_pair(value_d))
            elif subtype == 2:
                sx, sy = _u32_pair(b)
                sw, sh = _u32_pair(c)
                state["scissor"] = {
                    "x": _s32(sx), "y": _s32(sy),
                    "width": _s32(sw), "height": _s32(sh),
                }
            elif subtype in (3, 4):
                handle, logical_width = _u32_pair(b)
                logical_height, slot = _u32_pair(c)
                state[f"sampler{slot}"] = {
                    "handle": handle,
                    "logical_width": logical_width,
                    "logical_height": logical_height,
                    "cms": value_d & 0xff,
                    "cmt": (value_d >> 8) & 0xff,
                    "linear": bool((value_d >> 16) & 1),
                    "region_s": bool((value_d >> 17) & 1),
                    "region_t": bool((value_d >> 18) & 1),
                    "max_u": (value_d >> 24) & 0xffff,
                    "max_v": (value_d >> 40) & 0xffff,
                    "mirror_s": bool((value_d >> 56) & 1),
                    "mirror_t": bool((value_d >> 57) & 1),
                    "monochrome_rgb": bool((value_d >> 58) & 1),
                }
            elif subtype in (5, 6):
                slot = subtype - 5
                state.setdefault(f"sampler{slot}", {})[
                    "coordinate_scale"] = list(_float_pair(b))
            elif subtype in (7, 8):
                slot = subtype - 7
                handle, upload_serial = _u32_pair(b)
                provenance = state.setdefault(
                    f"sampler{slot}", {}).setdefault("provenance", {})
                provenance.update({
                    "handle": handle,
                    "upload_serial": upload_serial,
                    "format": c & 0xff,
                    "size": (c >> 8) & 0xff,
                    "palette_format": (c >> 16) & 0xffff,
                    "palette_count": (c >> 32) & 0xffff,
                    "source_hash": event["d"],
                })
            elif subtype in (9, 10):
                slot = subtype - 9
                handle, upload_serial = _u32_pair(b)
                provenance = state.setdefault(
                    f"sampler{slot}", {}).setdefault("provenance", {})
                provenance.update({
                    "handle": handle,
                    "upload_serial": upload_serial,
                    "palette_hash": event["c"],
                })
            else:
                state[f"subtype_{subtype}"] = {
                    "b": event["b"], "c": event["c"], "d": event["d"],
                }
        elif event_type == "draw_payload":
            packed_id = _event_value(event, "a")
            packed_storage = _event_value(event, "b")
            packed_shape = _event_value(event, "c")
            kind_id = event["flags"] & 0xff
            draw_payloads.append({
                "draw_id": packed_id & 0xffffffff,
                "chunk": packed_id >> 32,
                "kind_id": kind_id,
                "kind": {1: "input", 2: "clipped", 3: "clip_map"}.get(
                    kind_id, f"unknown_{kind_id}"),
                "stored": not bool(event["flags"] & 0x8000),
                "offset": packed_storage & 0xffffffff,
                "size": packed_storage >> 32,
                "vertex_count": packed_shape & 0xffffffff,
                "stride_floats": packed_shape >> 32,
                "hash": event["d"],
            })
        elif event_type == "texture_detail":
            subtype = event["flags"] & 0x7fff
            item = {
                "sequence": event["sequence"],
                "subtype": subtype,
                "dropped": bool(event["flags"] & 0x8000),
                "handle": _event_value(event, "a"),
            }
            b = _event_value(event, "b")
            c = _event_value(event, "c")
            if subtype == 0x0100:
                item.update({
                    "kind": "palette_hash",
                    "palette_count": b,
                    "palette_format": c,
                    "hash": event["d"],
                })
            elif subtype in (0x0200, 0x0300):
                offset, size = _u32_pair(b)
                item.update({
                    "kind": "source_payload" if subtype == 0x0200
                            else "palette_payload",
                    "stored": not bool(event["flags"] & 0x8000),
                    "offset": offset,
                    "size": size,
                    "metadata": event["c"],
                    "hash": event["d"],
                })
                if subtype == 0x0200:
                    first, second = _u32_pair(c)
                    item["source_stride_or_width"] = first
                    item["source_height"] = second
                else:
                    count, palette_format = _u32_pair(c)
                    item["palette_count"] = count
                    item["palette_format"] = palette_format
            elif subtype == 0x1000:
                item.update({
                    "kind": "texture_snapshot_extra",
                    "alpha_mask_clut_vram": b,
                    "resident": bool(c & 0xffffffff),
                    "uploaded": bool(c >> 32),
                    "alpha_opaque": bool(_event_value(event, "d")),
                })
            elif subtype == 0x1100:
                item.update({
                    "kind": "shared_clut_snapshot",
                    "vram": b,
                    "bytes": c,
                    "resident": bool(_event_value(event, "d")),
                })
            else:
                item.update({
                    "kind": f"unknown_{subtype:x}",
                    "b": event["b"], "c": event["c"], "d": event["d"],
                })
            texture_details.append(item)
        elif event_type == "build_config":
            flags = _event_value(event, "a")
            translate, slots = _u32_pair(_event_value(event, "b"))
            alpha_ref, edge_ref = _u32_pair(_event_value(event, "c"))
            runtime = _event_value(event, "d")
            build_config = {
                "flags_raw": _hex(flags),
                "native_indexed_textures": bool(flags & (1 << 0)),
                "vu1_color_batch": bool(flags & (1 << 1)),
                "alpha_sparse_shuffle": bool(flags & (1 << 2)),
                "alpha_same_sample_fastpath": bool(flags & (1 << 3)),
                "independent_alpha_mask": bool(flags & (1 << 4)),
                "independent_alpha_direct": bool(flags & (1 << 5)),
                "geometry_baseline": bool(flags & (1 << 6)),
                "material_baseline": bool(flags & (1 << 7)),
                "translate_vertices": translate,
                "texture_state_slots": slots,
                "alpha_threshold": alpha_ref,
                "texture_edge_threshold": edge_ref,
                "filter_mode": runtime & 0xffff,
                "mipmap_filter": (runtime >> 16) & 0xffff,
                "anisotropy": (runtime >> 32) & 0xffffffff,
            }
        elif event_type == "queue_wait":
            subtype = event["flags"] & 0xff
            item = {
                "sequence": event["sequence"],
                "draw_id": active_draw.get("draw_id") if active_draw else None,
                "subtype": subtype,
                "failed": bool(event["flags"] & 0x8000),
                "a": _event_value(event, "a"),
                "b": _event_value(event, "b"),
                "c": _event_value(event, "c"),
                "d": _event_value(event, "d"),
            }
            if subtype == 0:
                item.update({
                    "name": "path3_ownership",
                    "wait_microseconds": item["a"],
                    "used_qwords": item["b"],
                    "arena": item["c"],
                    "path1_handoff": bool(item["d"]),
                })
            elif subtype == 1:
                item.update({
                    "name": "texture_upload_ownership",
                    "wait_microseconds": item["a"],
                    "payload_bytes": item["b"],
                    "chain_qwords": item["c"],
                    "encoding": item["d"],
                })
            elif subtype == 2:
                item.update({
                    "name": "gs_finish",
                    "ownership_microseconds": item["a"],
                    "finish_dma_microseconds": item["b"],
                    "gs_poll_microseconds": item["c"],
                    "wait_microseconds": item["d"],
                })
            elif subtype == 4:
                encoded = item["d"]
                item.update({
                    "name": "texture_staging",
                    "wait_microseconds": item["a"],
                    "payload_bytes": item["b"],
                    "source_bytes": item["c"],
                    "encoding": encoded & 0xffffffff,
                    "mirror_s": bool((encoded >> 32) & 1),
                    "mirror_t": bool((encoded >> 33) & 1),
                })
            else:
                item["name"] = f"subtype_{subtype}"
            queue_waits.append(item)
        elif event_type == "gs_draw":
            packed = _event_value(event, "a")
            texture_vram, clut_vram = _u32_pair(_event_value(event, "d"))
            gs_draws.append({
                "sequence": event["sequence"],
                "draw_id": active_draw.get("draw_id") if active_draw else None,
                "vertices": packed & 0xffffffff,
                "register_count": packed >> 32,
                "path": "path1" if event["flags"] & 1 else "path3",
                "textured": bool(event["flags"] & 4),
                "indexed": bool(event["flags"] & 0x0100),
                "clut_load": bool(event["flags"] & 0x0200),
                "texflush": bool(event["flags"] & 0x0400),
                "render_target_view": bool(event["flags"] & 0x0800),
                "texture_alpha": bool(event["flags"] & 0x1000),
                "alpha_blend": bool(event["flags"] & 0x2000),
                "primary_state": event["b"],
                "secondary_state": event["c"],
                "texture_vram": texture_vram,
                "clut_vram": clut_vram,
            })
        elif event_type == "resource_op":
            subtype = event["flags"] & 0xff
            names = {
                1: "texture_create", 2: "texture_release",
                3: "texture_upload", 4: "indexed_upload",
                5: "intensity_upload", 10: "rt_create",
                11: "rt_bind", 12: "rt_release", 13: "clear",
                20: "vram_retire", 21: "vram_reclaim",
            }
            resource_ops.append({
                "sequence": event["sequence"],
                "subtype": subtype,
                "name": names.get(subtype, f"subtype_{subtype}"),
                "a": event["a"], "b": event["b"],
                "c": event["c"], "d": event["d"],
            })
        elif event_type == "draw_clipped" and active_draw is not None:
            clipped = _event_value(event, "c")
            draw_totals["clipped_vertices"] += clipped
            if clipped == 0:
                draw_totals["fully_clipped_draws"] += 1
                draw_totals["fully_clipped_triangles"] += active_draw[
                    "triangles"]
            shader = active_draw["shader"]
            if shader is not None and not shader["supported"]:
                shader_name = f"{shader['id0']}/{shader['id1']}"
                unsupported = unsupported_shaders[shader_name]
                unsupported["draws"] += 1
                unsupported["input_triangles"] += active_draw["triangles"]
                unsupported["clipped_vertices"] += clipped
            pass_name = PASS_GRAPH_NAMES.get(
                active_draw["pass_graph"],
                f"unknown_{active_draw['pass_graph']}")
            aggregate = by_pass[pass_name]
            aggregate["draws"] += 1
            aggregate["input_triangles"] += active_draw["triangles"]
            aggregate["clipped_vertices"] += clipped
            aggregate["duration_microseconds"] += max(
                0, event["microseconds"] - active_draw["start"])
            for path_name in ("path1", "path3"):
                aggregate[f"{path_name}_submits"] += active_draw[
                    f"{path_name}_submits"]
                aggregate[f"{path_name}_requested_qwords"] += active_draw[
                    f"{path_name}_requested_qwords"]
            draw_records.append({
                "draw_id": active_draw.get("draw_id"),
                "pass_graph": pass_name,
                "shader": shader,
                "input_triangles": active_draw["triangles"],
                "clipped_vertices": clipped,
                "duration_microseconds": max(
                    0, event["microseconds"] - active_draw["start"]),
                "path1_submits": active_draw["path1_submits"],
                "path1_requested_qwords": active_draw[
                    "path1_requested_qwords"],
                "path3_submits": active_draw["path3_submits"],
                "path3_requested_qwords": active_draw[
                    "path3_requested_qwords"],
                "start_sequence": active_draw.get("start_sequence"),
                "end_sequence": event["sequence"],
            })
            active_draw = None

    return {
        "paths": paths,
        "draws": draw_totals,
        "draw_records": draw_records,
        "unsupported_shaders": dict(sorted(unsupported_shaders.items())),
        "by_pass_graph": dict(sorted(
            by_pass.items(),
            key=lambda item: item[1]["duration_microseconds"],
            reverse=True)),
        "pass_graph_draws": pass_graph_draws,
        "independent_alpha_draws": independent_alpha_draws,
        "clipped_triangle_bounds": clipped_triangle_bounds,
        "texture_coord_ranges": texture_coord_ranges,
        "draw_states": dict(sorted(draw_states.items())),
        "draw_payloads": draw_payloads,
        "texture_details": texture_details,
        "build_config": build_config,
        "queue_waits": queue_waits,
        "gs_draws": gs_draws,
        "resource_ops": resource_ops,
        "largest_event_gaps": sorted(
            gaps, key=lambda gap: gap["microseconds"], reverse=True)[:10],
    }


def decode(path: Path) -> dict:
    blob = path.read_bytes()
    if len(blob) < HEADER.size:
        raise ValueError("file is smaller than the trace header")
    fields = HEADER.unpack_from(blob)
    magic = fields[0]
    if magic != b"PDGSTRC\0":
        raise ValueError(f"bad magic {magic!r}")
    (version, header_size, event_size, qword_size, stage, frame_number) = fields[1:7]
    start_us, end_us = fields[7:9]
    ints = fields[9:]
    (event_count, event_capacity, dropped_events, qword_count,
     qword_capacity, dropped_qwords, event_offset, qword_offset, flags,
     *reserved) = ints
    if version not in (1, 2) or header_size != HEADER.size or event_size != EVENT.size:
        raise ValueError("unsupported trace layout")
    if qword_size != QWORD.size:
        raise ValueError("unsupported qword layout")
    event_end = event_offset + event_count * event_size
    qword_end = qword_offset + qword_count * qword_size
    blob_size = reserved[0] if version >= 2 else 0
    blob_capacity = reserved[1] if version >= 2 else 0
    dropped_blob = reserved[2] if version >= 2 else 0
    blob_offset = reserved[3] if version >= 2 else qword_end
    capture_profile = reserved[4] if version >= 2 else 0
    forensic_microseconds = reserved[5] if version >= 2 else 0
    blob_end = blob_offset + blob_size
    if (event_offset < header_size or event_end > len(blob) or
        qword_end > len(blob) or blob_offset < qword_end or
        blob_end > len(blob)):
        raise ValueError("section offsets exceed the file")

    events = []
    for index in range(event_count):
        values = EVENT.unpack_from(blob, event_offset + index * event_size)
        sequence, event_type, event_flags, timestamp, a, b, c, d = values
        event = {
            "sequence": sequence,
            "type": EVENT_NAMES.get(event_type, f"unknown_{event_type}"),
            "type_id": event_type,
            "flags": event_flags,
            "microseconds": timestamp,
            "a": _hex(a), "b": _hex(b), "c": _hex(c), "d": _hex(d),
        }
        if event_type == 16 and a < len(GS_STATE_NAMES):
            event["slot"] = GS_STATE_NAMES[a]
        events.append(event)

    qwords = [QWORD.unpack_from(blob, qword_offset + i * QWORD.size)
              for i in range(qword_count)]
    submissions = []
    path1_submissions = []
    for event in events:
        if event["type_id"] not in (13, 14):
            continue
        offset = int(event["a"], 16)
        stored = int(event["b"], 16)
        if not stored or offset + stored > len(qwords):
            continue
        if event["type_id"] == 13:
            packed = int(event["d"], 16)
            path1_submissions.append({
                "qword_offset": offset,
                "stored_qwords": stored,
                "requested_qwords": int(event["c"], 16),
                "register_count": packed & 0xFFFFFFFF,
                "vertex_count": packed >> 32,
                "textured": bool(event["flags"] & 4),
                "vif_chain": [
                    {"lo": _hex(lo), "hi": _hex(hi)}
                    for lo, hi in qwords[offset:offset + stored]
                ],
            })
            continue

        cursor = offset
        end = offset + stored
        packets = []
        while cursor < end:
            tag_lo, tag_hi = qwords[cursor]
            nloop = tag_lo & 0x7FFF
            gif_flag = (tag_lo >> 58) & 0x3
            descriptor = tag_hi & 0xF
            nreg = (tag_lo >> 60) & 0xF
            nreg = nreg or 16
            payload_qwords = nloop * nreg
            packet_end = min(end, cursor + 1 + payload_qwords)
            records = []
            if gif_flag == 0 and nreg == 1 and descriptor == 0xE:
                for value, register in qwords[cursor + 1:packet_end]:
                    records.append({
                        "register": GS_REGISTER_NAMES.get(
                            register, f"REG_{register:02x}"),
                        "register_id": register,
                        "value": _hex(value),
                    })
            packets.append({
                "qword_offset": cursor,
                "gif_nloop": nloop,
                "gif_flag": gif_flag,
                "gif_nreg": nreg,
                "gif_register_descriptor": descriptor,
                "ad_records": records,
            })
            if packet_end <= cursor:
                break
            cursor = packet_end
        submissions.append({
            "qword_offset": offset,
            "stored_qwords": stored,
            "packets": packets,
        })

    analysis = _analyze(events)
    for payload in analysis["draw_payloads"]:
        payload["hash_ok"] = None
        if not payload["stored"]:
            continue
        start = blob_offset + payload["offset"]
        end = start + payload["size"]
        if start < blob_offset or end > blob_end:
            payload["stored"] = False
            payload["error"] = "payload range exceeds blob section"
            continue
        payload["hash_ok"] = (
            _fnv1a64(blob[start:end]) == int(payload["hash"], 16))

    for payload in analysis["texture_details"]:
        if payload.get("kind") not in ("source_payload", "palette_payload"):
            continue
        payload["hash_ok"] = None
        if not payload["stored"]:
            continue
        start = blob_offset + payload["offset"]
        end = start + payload["size"]
        if start < blob_offset or end > blob_end:
            payload["stored"] = False
            payload["error"] = "payload range exceeds blob section"
            continue
        payload["hash_ok"] = (
            _fnv1a64(blob[start:end]) == int(payload["hash"], 16))

    return {
        "source": str(path),
        "version": version,
        "stage": stage,
        "frame_number": frame_number,
        "start_microseconds": start_us,
        "end_microseconds": end_us,
        "duration_microseconds": max(0, end_us - start_us),
        "flags": flags,
        "events": {"count": event_count, "capacity": event_capacity,
                   "dropped": dropped_events},
        "qwords": {"count": qword_count, "capacity": qword_capacity,
                   "dropped": dropped_qwords},
        "blob": {"size": blob_size, "capacity": blob_capacity,
                 "dropped": dropped_blob, "offset": blob_offset,
                 "capture_profile": capture_profile,
                 "post_frame_forensic_microseconds": forensic_microseconds},
        "event_stream": events,
        "path3_submissions": submissions,
        "path1_submissions": path1_submissions,
        "analysis": analysis,
        "reserved": reserved,
    }


def print_summary(trace: dict) -> None:
    counts = {}
    for event in trace["event_stream"]:
        counts[event["type"]] = counts.get(event["type"], 0) + 1
    print(f"PD PS2 renderer trace v{trace['version']}")
    print(f"stage={trace['stage']} frame={trace['frame_number']} "
          f"duration={trace['duration_microseconds']} us")
    print(f"events={trace['events']['count']} "
          f"dropped={trace['events']['dropped']} "
          f"qwords={trace['qwords']['count']}/{trace['qwords']['capacity']} "
          f"dropped={trace['qwords']['dropped']}")
    if trace["version"] >= 2:
        blob = trace["blob"]
        print(f"blob={blob['size']}/{blob['capacity']} bytes "
              f"dropped={blob['dropped']} profile={blob['capture_profile']} "
              f"post_frame={blob['post_frame_forensic_microseconds']} us")
    print("event counts:")
    for name in sorted(counts):
        print(f"  {name}: {counts[name]}")
    analysis = trace["analysis"]
    print("pipeline traffic:")
    for path_name in ("path1", "path3"):
        path = analysis["paths"][path_name]
        print(f"  {path_name.upper()}: submits={path['submits']} "
              f"requested_qwords={path['requested_qwords']} "
              f"stored_qwords={path['stored_qwords']} "
              f"uncaptured_submits={path['uncaptured_submits']}")
    print("draw cost by pass graph:")
    for name, values in analysis["by_pass_graph"].items():
        print(f"  {name}: draws={values['draws']} "
              f"triangles={values['input_triangles']} "
              f"clipped_vertices={values['clipped_vertices']} "
              f"duration={values['duration_microseconds']} us "
              f"PATH1={values['path1_submits']} "
              f"PATH3={values['path3_submits']}")
    draws = analysis["draws"]
    print(f"fully clipped: draws={draws['fully_clipped_draws']} "
          f"triangles={draws['fully_clipped_triangles']}")
    print("unsupported shaders:")
    for shader, values in analysis["unsupported_shaders"].items():
        print(f"  {shader}: draws={values['draws']} "
              f"triangles={values['input_triangles']} "
              f"clipped_vertices={values['clipped_vertices']}")
    if analysis["pass_graph_draws"]:
        print("alpha-trilerp pass-graph draws:")
        for draw in analysis["pass_graph_draws"]:
            bbox = draw["bbox"]
            shader = draw.get("shader", {})
            print(
                f"  shader={shader.get('id0', '?')}/{shader.get('id1', '?')} "
                f"vertices={draw['vertices']} tiles={draw['tiles']} "
                f"bbox=({bbox['min_x']:.1f},{bbox['min_y']:.1f})-"
                f"({bbox['max_x']:.1f},{bbox['max_y']:.1f}) "
                f"lod={draw.get('lod_min', 0)}..{draw.get('lod_max', 0)} "
                f"shade_a={draw.get('shade_alpha_min', 0)}.."
                f"{draw.get('shade_alpha_max', 0)} "
                f"add_a={draw.get('add_alpha_min', 0)}.."
                f"{draw.get('add_alpha_max', 0)} "
                f"fast_opaque={draw.get('direct_opaque_triangles', 0)} "
                f"fast_vertex_alpha={draw.get('vertex_alpha_triangles', 0)} "
                f"fast_same_sample={draw.get('same_sample_triangles', 0)} "
                f"success={draw.get('success', False)}")
    if analysis["independent_alpha_draws"]:
        direct_triangles = sum(
            item["triangles"] for item in analysis["independent_alpha_draws"]
            if item["direct"])
        mask_triangles = sum(
            item["triangles"] for item in analysis["independent_alpha_draws"]
            if item.get("alpha_mask"))
        print(f"independent alpha direct triangles: {direct_triangles}")
        print(f"independent alpha mask triangles: {mask_triangles}")
    suspicious = [
        item for item in analysis["clipped_triangle_bounds"]
        if item["thin"] or item["near_zero_w"] or
           (item["screen_wide"] and item["screen_tall"])
    ]
    if suspicious:
        print("suspicious clipped triangles:")
        for item in suspicious[:32]:
            bbox = item["bbox"]
            print(
                f"  src={item['source_triangle']} out={item['output_triangle']} "
                f"bbox=({bbox['min_x']:.1f},{bbox['min_y']:.1f})-"
                f"({bbox['max_x']:.1f},{bbox['max_y']:.1f}) "
                f"wide={item['screen_wide']} tall={item['screen_tall']} "
                f"thin={item['thin']} near_w0={item['near_zero_w']}")
    if analysis["texture_coord_ranges"]:
        outliers = []
        for item in analysis["texture_coord_ranges"]:
            if item["width"] and item["height"]:
                if (item["min_u"] < -1.0 or
                    item["max_u"] > item["width"] + 1.0 or
                    item["min_v"] < -1.0 or
                    item["max_v"] > item["height"] + 1.0):
                    outliers.append(item)
        if outliers:
            print("texture coordinate ranges outside logical extent:")
            for item in outliers[:32]:
                print(
                    f"  tex{item['slot']} handle={item['handle']} "
                    f"uv=({item['min_u']:.2f}..{item['max_u']:.2f},"
                    f"{item['min_v']:.2f}..{item['max_v']:.2f}) "
                    f"extent={item['width']}x{item['height']} "
                    f"linear={item['linear']} "
                    f"mode={item.get('filter_mode', 0)} "
                    f"region={item['region_s']}/{item['region_t']}")
    if analysis["build_config"]:
        cfg = analysis["build_config"]
        print("build config:")
        print(
            f"  indexed={cfg.get('native_indexed_textures')} "
            f"vu1={cfg.get('vu1_color_batch')} "
            f"same_sample={cfg.get('alpha_same_sample_fastpath')} "
            f"alpha_mask={cfg.get('independent_alpha_mask')} "
            f"alpha_direct={cfg.get('independent_alpha_direct')} "
            f"filter={cfg.get('filter_mode')} "
            f"mipmap={cfg.get('mipmap_filter')} "
            f"aniso={cfg.get('anisotropy')}")
    if analysis["queue_waits"]:
        print("queue/wait timing:")
        grouped = collections.defaultdict(list)
        for item in analysis["queue_waits"]:
            grouped[item.get("name", f"subtype_{item['subtype']}")].append(item)
        for name, items in sorted(grouped.items()):
            values = [item.get("wait_microseconds", 0) for item in items]
            print(
                f"  {name}: count={len(items)} total={sum(values)} us "
                f"max={max(values) if values else 0} us "
                f"failures={sum(1 for item in items if item['failed'])}")
    if analysis["gs_draws"]:
        draws = analysis["gs_draws"]
        print("GS draws:")
        print(
            f"  total={len(draws)} "
            f"path1={sum(1 for x in draws if x['path'] == 'path1')} "
            f"path3={sum(1 for x in draws if x['path'] == 'path3')} "
            f"indexed={sum(1 for x in draws if x['indexed'])} "
            f"clut_load={sum(1 for x in draws if x['clut_load'])} "
            f"texflush={sum(1 for x in draws if x['texflush'])} "
            f"rt_view={sum(1 for x in draws if x['render_target_view'])}")
    if analysis["draw_payloads"]:
        stored = [x for x in analysis["draw_payloads"] if x["stored"]]
        bad_hash = [x for x in stored if x.get("hash_ok") is False]
        print(
            f"draw payloads: total={len(analysis['draw_payloads'])} "
            f"stored={len(stored)} bad_hash={len(bad_hash)}")
    print("GS shadow:")
    for event in trace["event_stream"]:
        if event["type"] == "gs_register":
            validity = "valid" if event["flags"] & 1 else "invalid"
            print(f"  {event.get('slot', event['a'])}: {event['b']} ({validity})")



def extract_payloads(trace: dict, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    source_blob = Path(trace["source"]).read_bytes()
    blob_base = trace["blob"]["offset"]
    manifest = []
    for payload in trace["analysis"]["draw_payloads"]:
        entry = dict(payload)
        if not payload["stored"]:
            manifest.append(entry)
            continue
        start = blob_base + payload["offset"]
        end = start + payload["size"]
        data = source_blob[start:end]
        suffix = payload["kind"]
        filename = (
            f"draw_{payload['draw_id']:05d}_{suffix}_"
            f"{payload['chunk']:03d}_{payload['vertex_count']}v_"
            f"{payload['stride_floats']}f.bin")
        (destination / filename).write_bytes(data)
        entry["file"] = filename
        manifest.append(entry)
    texture_manifest = []
    for payload in trace["analysis"]["texture_details"]:
        if payload.get("kind") not in ("source_payload", "palette_payload"):
            continue
        entry = dict(payload)
        if not payload["stored"]:
            texture_manifest.append(entry)
            continue
        start = blob_base + payload["offset"]
        end = start + payload["size"]
        data = source_blob[start:end]
        filename = (
            f"texture_{payload['handle']:03d}_"
            f"{payload['kind']}_{payload['sequence']:05d}.bin")
        (destination / filename).write_bytes(data)
        entry["file"] = filename
        texture_manifest.append(entry)
    (destination / "manifest.json").write_text(
        json.dumps({
            "draw_payloads": manifest,
            "texture_payloads": texture_manifest,
        }, indent=2) + "\n", encoding="utf-8")

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--json", type=Path, dest="json_path")
    parser.add_argument(
        "--extract-payloads", type=Path, dest="payload_dir",
        help="write captured VBO/clip-map payloads and a manifest")
    args = parser.parse_args()
    try:
        trace = decode(args.trace)
    except (OSError, ValueError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print_summary(trace)
    if args.json_path:
        args.json_path.write_text(json.dumps(trace, indent=2) + "\n",
                                  encoding="utf-8")
        print(f"JSON: {args.json_path}")
    if args.payload_dir:
        extract_payloads(trace, args.payload_dir)
        print(f"payloads: {args.payload_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
