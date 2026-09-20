#!/usr/bin/env python3
"""Audit a PS2 EE ELF and prove a stripped launch copy has the same load image."""

from __future__ import annotations

import argparse
import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path

EHDR = struct.Struct("<16sHHIIIIIHHHHHH")
PHDR = struct.Struct("<IIIIIIII")
PT_LOAD = 1
EM_MIPS = 8
ET_EXEC = 2

@dataclass(frozen=True)
class Segment:
    offset: int
    vaddr: int
    paddr: int
    filesz: int
    memsz: int
    flags: int
    align: int
    digest: str

@dataclass(frozen=True)
class Image:
    path: Path
    file_size: int
    entry: int
    flags: int
    loads: tuple[Segment, ...]

def parse(path: Path) -> Image:
    data = path.read_bytes()
    if len(data) < EHDR.size:
        raise ValueError(f"{path}: truncated ELF header")
    fields = EHDR.unpack_from(data)
    ident = fields[0]
    (etype, machine, _version, entry, phoff, _shoff, flags,
     ehsize, phentsize, phnum, _shentsize, _shnum, _shstrndx) = fields[1:]
    if ident[:4] != b"\x7fELF" or ident[4] != 1 or ident[5] != 1:
        raise ValueError(f"{path}: expected little-endian ELF32")
    if etype != ET_EXEC or machine != EM_MIPS:
        raise ValueError(f"{path}: expected ET_EXEC/EM_MIPS")
    if ehsize != EHDR.size or phentsize != PHDR.size:
        raise ValueError(f"{path}: unexpected ELF/program-header size")
    if phoff + phnum * phentsize > len(data):
        raise ValueError(f"{path}: program-header table exceeds file")

    loads = []
    for index in range(phnum):
        values = PHDR.unpack_from(data, phoff + index * phentsize)
        p_type, off, va, pa, filesz, memsz, pflags, align = values
        if p_type != PT_LOAD or filesz == 0:
            continue
        if memsz < filesz or off + filesz > len(data):
            raise ValueError(f"{path}: invalid PT_LOAD {index}")
        digest = hashlib.sha256(data[off:off + filesz]).hexdigest()
        loads.append(Segment(off, va, pa, filesz, memsz, pflags, align, digest))

    if not loads:
        raise ValueError(f"{path}: no non-empty PT_LOAD")
    if not (0x00080000 <= entry <= 32 * 1024 * 1024):
        raise ValueError(f"{path}: entry point outside current PS2SDK loader range")
    if not any(s.vaddr <= entry < s.vaddr + s.filesz for s in loads):
        raise ValueError(f"{path}: entry point is not file-backed by PT_LOAD")
    return Image(path, len(data), entry, flags, tuple(loads))

def describe(image: Image) -> None:
    print(f"{image.path}: {image.file_size} bytes")
    print(f"  entry=0x{image.entry:08x} flags=0x{image.flags:08x} PT_LOAD={len(image.loads)}")
    for i, s in enumerate(image.loads):
        print(
            f"  load[{i}] off=0x{s.offset:x} vaddr=0x{s.vaddr:08x} "
            f"filesz=0x{s.filesz:x} memsz=0x{s.memsz:x} align=0x{s.align:x} "
            f"sha256={s.digest}"
        )

def compare(a: Image, b: Image) -> None:
    if a.entry != b.entry or a.flags != b.flags or len(a.loads) != len(b.loads):
        raise ValueError("compatibility ELF changed entry/flags/PT_LOAD count")
    for i, (x, y) in enumerate(zip(a.loads, b.loads)):
        layout_x = (x.offset, x.vaddr, x.paddr, x.filesz, x.memsz, x.flags, x.align)
        layout_y = (y.offset, y.vaddr, y.paddr, y.filesz, y.memsz, y.flags, y.align)
        if layout_x != layout_y:
            raise ValueError(f"PT_LOAD {i} layout changed")
        if x.digest != y.digest:
            raise ValueError(f"PT_LOAD {i} bytes changed")
    if b.file_size >= a.file_size:
        raise ValueError("compatibility ELF did not shrink")
    print(
        f"  compare=PASS saved={a.file_size - b.file_size} bytes; "
        "entry, PT_LOAD layout and PT_LOAD bytes identical"
    )

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", type=Path)
    ap.add_argument("--compare", type=Path)
    args = ap.parse_args()
    original = parse(args.elf)
    describe(original)
    if args.compare:
        candidate = parse(args.compare)
        describe(candidate)
        compare(original, candidate)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
