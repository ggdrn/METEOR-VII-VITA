#!/usr/bin/env python3
"""
Generates source/ff7_memmap_sites.bin: the mk_address() call sites of
libjni_ff7.so (v1.0.29) whose address is a constant (see source/ff7_memmap.c).

The recompiled ff7.exe reaches every x86 global through

    movw r0, #lo16(va)
    ...                     (instructions that neither read nor write r0)
    movt r0, #hi16(va)
    ...
    bl   mk_address

A site is listed only when this is provable from the disassembly:
  - it is inside a recompiled function (_Z15func_0x... / _Z10C_00...): only
    the game thread runs those, so patching them at startup cannot race;
  - movw/movt/bl are unconditional (no IT block) and at most 16 instructions
    apart, with no branch, no IT and no use of r0 in between;
  - no direct branch lands on any instruction after the movw up to the bl;
  - the function has no computed jump (tbb/tbh, bx to a register other
    than lr, writes to pc).

Usage: scripts/gen-memmap-sites.py libjni_ff7.so source/ff7_memmap_sites.bin
(needs llvm-objdump on PATH).

Format (little endian): "MKS1", u32 count, then per site, in address order:
varint((bl - previous bl) / 2), u8((bl - movw) / 2), u8((bl - movt) / 2).
Offsets are relative to the .so text base; the first "previous bl" is 0.
"""
import bisect
import re
import struct
import subprocess
import sys

MK_ADDRESS = 0x93d714
WINDOW = 16

INS = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{4} ?){1,2})\s+(\S+)\s*(.*)$')
FUNC = re.compile(r'^([0-9a-f]{8}) <(.*)>:$')
BRANCH = re.compile(r'^(b|bl|blx|bx|cbz|cbnz|tbb|tbh)(\.w|\.n)?$'
                    r'|^b(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)(\.w|\.n)?$')
R0 = re.compile(r'\br0\b')
RECOMPILED = re.compile(r'^_Z15func_0x|^_Z10C_00')


def varint(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def main(so_path, out_path):
    dis = subprocess.run(['llvm-objdump', '-d', '--triple=thumbv7-none-eabi', so_path],
                         capture_output=True, text=True, check=True).stdout
    insns, funcs, names = [], [], []
    for line in dis.split('\n'):
        m = FUNC.match(line)
        if m:
            funcs.append(int(m.group(1), 16))
            names.append(m.group(2))
            continue
        m = INS.match(line)
        if m:
            insns.append((int(m.group(1), 16), m.group(3), m.group(4)))

    def func_of(a):
        return funcs[bisect.bisect_right(funcs, a) - 1]

    targets, computed = set(), set()
    for a, op, args in insns:
        if BRANCH.match(op) and op not in ('bx', 'tbb', 'tbh'):
            m = re.search(r'0x([0-9a-f]+)', args)
            if m:
                targets.add(int(m.group(1), 16))
        if (op in ('tbb', 'tbh') or (op == 'bx' and args != 'lr')
                or (re.match(r'(ldr|mov|add)(\.w)?$', op) and args.startswith('pc'))):
            computed.add(func_of(a))
    recompiled = {f for f, n in zip(funcs, names) if RECOMPILED.match(n)}

    sites, calls = [], 0
    for i, (a, op, args) in enumerate(insns):
        if not (op == 'bl' and args.startswith('0x%x ' % MK_ADDRESS)):
            continue
        calls += 1
        f = func_of(a)
        if f not in recompiled or f in computed:
            continue
        movt = movw = None
        j = i - 1
        while j >= 0 and i - j <= WINDOW:
            pa, pop, pargs = insns[j]
            if insns[j + 1][0] in targets or pa < f:
                break
            if pop == 'movt' and pargs.startswith('r0,') and movt is None:
                movt = pa
            elif pop == 'movw' and pargs.startswith('r0,'):
                movw = pa
                break
            elif (BRANCH.match(pop) or pop.startswith('it') or R0.search(pargs)
                  or pop in ('udf', 'svc', 'bkpt') or '<unknown>' in pop):
                break
            j -= 1
        if movw is not None and movt is not None and movw < movt < a:
            sites.append((a, movw, movt))

    out = bytearray(b'MKS1' + struct.pack('<I', len(sites)))
    prev = 0
    for bl, movw, movt in sites:
        out += varint((bl - prev) // 2) + bytes([(bl - movw) // 2, (bl - movt) // 2])
        prev = bl
    with open(out_path, 'wb') as f:
        f.write(out)
    print('%d mk_address calls, %d constant sites, %d bytes' % (calls, len(sites), len(out)))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
