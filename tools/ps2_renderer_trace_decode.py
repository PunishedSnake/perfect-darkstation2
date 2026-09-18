#!/usr/bin/env python3
"""Decode a one-frame Perfect DarkStation 2 renderer/GS capture."""

from __future__ import annotations

import argparse
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
    22: "frontend_state", 23: "warning",
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


def _hex(value: int) -> str:
    return f"0x{value:016x}"


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
    if version != 1 or header_size != HEADER.size or event_size != EVENT.size:
        raise ValueError("unsupported trace layout")
    if qword_size != QWORD.size:
        raise ValueError("unsupported qword layout")
    event_end = event_offset + event_count * event_size
    qword_end = qword_offset + qword_count * qword_size
    if event_offset < header_size or event_end > len(blob) or qword_end > len(blob):
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
        "event_stream": events,
        "path3_submissions": submissions,
        "path1_submissions": path1_submissions,
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
          f"PATH3_qwords={trace['qwords']['count']} "
          f"dropped={trace['qwords']['dropped']}")
    print("event counts:")
    for name in sorted(counts):
        print(f"  {name}: {counts[name]}")
    print("GS shadow:")
    for event in trace["event_stream"]:
        if event["type"] == "gs_register":
            validity = "valid" if event["flags"] & 1 else "invalid"
            print(f"  {event.get('slot', event['a'])}: {event['b']} ({validity})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--json", type=Path, dest="json_path")
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
