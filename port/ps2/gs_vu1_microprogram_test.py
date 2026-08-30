"""Functional test of the checked-in VSM against the real C++ payload builder.

This deliberately small interpreter accepts only instructions used by this
program. It checks lane dependencies conservatively, but is NOT a PS2 emulator:
VIF/GIF arbitration, VU special FP rounding and hardware timing still need A/B.
"""

import ctypes as c
import math
from pathlib import Path
import re
import struct
import sys


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def number(value):
    # VU floating operations flush denormals; MOVE/load/store must preserve bits.
    if value & 0x7f800000 == 0:
        return 0.0
    return struct.unpack('<f', struct.pack('<I', value))[0]


def parse_program(path):
    program, labels = [], {}
    for line in path.read_text().splitlines():
        line = line.split(';', 1)[0].strip()
        if not line or line.startswith('.'):
            continue
        if line.endswith(':'):
            labels[line[:-1]] = len(program)
            continue
        fields = line.split()
        upper = fields.pop(0).lower()
        args = '' if upper.startswith('nop') else fields.pop(0)
        program.append((upper, args, ' '.join(fields)))
    return program, labels


def execute(program, labels, memory, top):
    vf = [[0] * 4 for _ in range(32)]
    vf[0][3] = bits(1.0)
    ready = [[0] * 4 for _ in vf]
    vi = [0] * 16
    acc = [0.0] * 4
    q, q_ready = 0.0, 0
    pc, cycle, pending, stop = 0, 0, None, None
    writes, kicks = [], []

    def read(reg, lane, raw=False):
        index = int(reg[2:4])
        assert cycle >= ready[index][lane], (cycle, pc, reg, lane, 'hazard')
        return vf[index][lane] if raw else number(vf[index][lane])

    def write(reg, lane, value):
        index = int(reg[2:4])
        assert index != 0
        vf[index][lane] = value
        ready[index][lane] = cycle + 4

    def integer(reg):
        return int(reg[2:])

    while pc < len(program):
        assert cycle < 20000, 'microprogram did not terminate'
        upper, operands, lower = program[pc]
        branch = None
        if '[e]' in upper:
            stop = cycle + 1
        if not upper.startswith('nop'):
            op, mask = upper.split('.')
            args = operands.split(',')
            lanes = ['xyzw'.index(lane) for lane in mask]
            # Sources are read before writes, as with simultaneous lane ops.
            values = []
            for lane in lanes:
                if op.startswith('ftoi'):
                    value = math.trunc(read(args[1], lane) *
                                       (16 if op == 'ftoi4' else 1))
                    values.append(value & 0xffffffff)
                    continue
                if op == 'mulq':
                    assert cycle >= q_ready, 'DIV Q dependency'
                    value = read(args[1], lane) * q
                elif op in ('maxz', 'maxw', 'miniw'):
                    broadcast = 'xyzw'.index(args[2][-1].lower())
                    fn = min if op.startswith('min') else max
                    value = fn(read(args[1], lane), read(args[2], broadcast))
                elif op == 'mulaw':
                    value = read(args[1], lane) * read(args[2], 3)
                elif op == 'madd':
                    value = read(args[1], lane) * read(args[2], lane) + acc[lane]
                else:
                    raise AssertionError(('unknown upper', upper))
                values.append(bits(value))
            for lane, value in zip(lanes, values):
                if op == 'mulaw':
                    acc[lane] = number(value)  # ACC forwarding to next MADD.
                else:
                    write(args[0], lane, value)

        fields = lower.split()
        op = fields[0].lower()
        args = fields[1].split(',') if len(fields) == 2 else []
        if op == 'nop':
            pass
        elif op == 'xtop':
            vi[integer(args[0])] = top
        elif op in ('iaddiu', 'isubiu'):
            amount = int(args[2], 0) * (-1 if op == 'isubiu' else 1)
            vi[integer(args[0])] = (vi[integer(args[1])] + amount) & 0xffff
        elif op in ('lq', 'lqi', 'sqi', 'ilw.x', 'ilw.w'):
            match = re.fullmatch(r'(\d*)\((VI\d+)(\+\+)?\)', args[1])
            assert match, lower
            offset, reg, increment = match.groups()
            address = vi[integer(reg)] + int(offset or 0)
            assert 0 <= address < 1024, ('VU memory overflow', address)
            if op == 'sqi':
                assert top + 512 <= address < top + 768, ('output bank', address)
                memory[address] = [read(args[0], i, raw=True) for i in range(4)]
                writes.append(address)
            elif op.startswith('ilw'):
                vi[integer(args[0])] = memory[address][3 if op.endswith('w') else 0] & 0xffff
            else:
                for lane, value in enumerate(memory[address]):
                    write(args[0], lane, value)
            if increment:
                vi[integer(reg)] = (vi[integer(reg)] + 1) & 0xffff
        elif op == 'div':
            q = number(bits(read(args[1], 3) / read(args[2], 3)))
            q_ready = cycle + 7
        elif op == 'move.w':
            write(args[0], 3, read(args[1], 3, raw=True))
        elif op in ('ibeq', 'ibne'):
            equal = vi[integer(args[0])] == vi[integer(args[1])]
            if equal == (op == 'ibeq'):
                branch = labels[args[2]]
        elif op == 'b':
            branch = labels[args[0]]
        elif op == 'xgkick':
            kicks.append(vi[integer(args[0])])
        else:
            raise AssertionError(('unknown lower', lower))

        pc = pending if pending is not None else pc + 1
        pending = branch
        if stop == cycle:
            break
        cycle += 1
    assert stop is not None, 'missing E bit'
    assert kicks == [top + 512]
    return writes


class Reg(c.Structure):
    _fields_ = [('value', c.c_uint64), ('reg', c.c_uint64)]


class Vertex(c.Structure):
    _fields_ = [('clip', c.c_float * 4), ('stq', c.c_float * 3),
                ('control', c.c_uint32), ('rgba', c.c_uint32 * 4)]


def run_tests(library):
    program, labels = parse_program(Path(__file__).with_name('gs_vu1_textured_transform.vsm'))
    build = c.CDLL(library).ps2GsVu1BuildTexturedTransformPayload
    build.restype = c.c_bool
    build.argtypes = [c.POINTER(c.c_uint32), c.c_uint32,
                      c.POINTER(c.c_float), c.POINTER(c.c_float), c.c_uint32,
                      c.POINTER(Reg), c.c_uint32, c.POINTER(Vertex), c.c_uint32,
                      c.POINTER(Reg), c.c_uint32, c.c_void_p]
    scale = (c.c_float * 4)(160, -120, -65534, 0)
    offset = (c.c_float * 4)(2048, 2048, 65535, 4095.9375)
    cases = 0
    for top in (0, 256):
        for fog in (0, 1):
            for count in (3, 81):
                for prefix_count, suffix_count in ((0, 0), (2, 1), (5, 1)):
                    vertices = (Vertex * count)()
                    for i, vertex in enumerate(vertices):
                        # Include perspective, both depth clamps and XY saturation.
                        w = (1, 2, 4)[i % 3]
                        x, y = ((-32, 32), (0, 0), (32, -32))[i % 3]
                        z = (-0.5, 0.5, 1.5)[i % 3]
                        vertex.clip[:] = (x * w, y * w, z * w, w)
                        vertex.stq[:] = (0.25, 0.75, 1)
                        vertex.control = ((0, 127, 255)[i % 3] << 4 if fog else 0) | (0x8000 if i % 3 == 0 else 0)
                        vertex.rgba[:] = (17, 65, 129, 128)
                    prefix = (Reg * 5)(*[Reg(0x1234 + i, i) for i in range(5)])
                    suffix = Reg(0x5678, 8)
                    payload = (c.c_uint32 * 1024)()
                    assert build(payload, 256, scale, offset, fog,
                                 prefix, prefix_count, vertices, count,
                                 c.byref(suffix), suffix_count, None)
                    memory = [[0xdeadbeef] * 4 for _ in range(1024)]
                    input_qw, output_qw = 12 + count * 3, 9 + count * 3
                    for i in range(input_qw):
                        memory[top + i] = list(payload[i * 4:i * 4 + 4])
                    before = [row[:] for row in memory]
                    writes = execute(program, labels, memory, top)
                    assert writes == list(range(top + 512, top + 512 + output_qw))
                    for i in range(1024):
                        if i not in writes:
                            assert memory[i] == before[i], ('unexpected write', i)
                    output = memory[top + 512:top + 512 + output_qw]
                    assert output[0] == before[top + 3]
                    assert output[1:6] == before[top + 6:top + 11]
                    assert output[6] == before[top + 4]
                    assert output[-2] == before[top + 5]
                    assert output[-1] == before[top + input_qw - 1]
                    for i, vertex in enumerate(vertices):
                        stq, rgba, xyz = output[7 + i * 3:10 + i * 3]
                        w = vertex.clip[3]
                        assert stq[:3] == [bits(v / w) for v in vertex.stq]
                        assert rgba == list(vertex.rgba)
                        x = min(4095.9375, max(0, vertex.clip[0] / w * 160 + 2048))
                        y = min(4095.9375, max(0, vertex.clip[1] / w * -120 + 2048))
                        z = 65535 - min(1, max(0, vertex.clip[2] / w)) * 65534
                        assert xyz == [int(x * 16), int(y * 16),
                                       int(z * (16 if fog else 1)), vertex.control]
                    cases += 1
    print(f'VU1 source functional tests: {cases} cases passed (not hardware validation)')


if __name__ == '__main__':
    run_tests(sys.argv[1])
