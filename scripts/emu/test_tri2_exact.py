#!/usr/bin/env python3
"""
Compares source/ff7_tri2_core.h (native C_006A8A3E, the triangle renderer of
the highway and snowboard minigames, and its siblings C_006A8242, C_006A83AE
and C_006A8BFC: flat or smooth color, with or without u/v) with the .so
functions under emulation,
everything included: the whole VERTICES output (x, y, z, color bytes, u, v),
pCurVertex, and the current color and u/v globals left behind (later
glVertex3f calls use them). The .so's GOT entries for its own symbols are
resolved (Emu(resolve_defined=True)), so fake_glColor4ub, glTexCoord2f and
glVertex3f run as on the Vita. glBegin/glEnd are stubbed in both runs (the
native code calls the .so's, unchanged). The core runs as an ARM blob in the
same emulator, on the same globals.

v0.6.19's core took every vertex's color (the .so draws each triangle flat
with its first vertex's color) and left the current u/v alone: this test
fails on it.

Usage: scripts/emu/test_tri2_exact.py libjni_ff7.so [calls]
Needs arm-vita-eabi-gcc on PATH (source scripts/env.sh) and `pip install unicorn`.
"""
import math, os, random, struct, subprocess, sys, tempfile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, \
    UC_ARM_REG_LR, UC_ARM_REG_PC
from ff7emu import Emu, B, HSTACK, END

GLBEGIN, GLEND = 0x95316c, 0x953424
# tri2_core_draw_mode modes: 0 flat, 1 smooth, 2 flat + u/v, 3 smooth + u/v
FUNCS = {0: (0x941f24, 'C_006A8242'), 1: (0x941f8c, 'C_006A83AE'), 2: (0x942344, 'C_006A8A3E'),
         3: (0x9423c4, 'C_006A8BFC')}
CUR, LAST, VERTICES = 0xf4f4d8, 0xf4f4dc, 0x146263c
CR, CG, CB, CA = 0x1462587, 0x1462586, 0x1462585, 0x1462584
CU, CV = 0x146258c, 0x1462588
NV = 200
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'source')
BLOB, STATE, DATA = 0x58000000, 0x59000000, 0x30000000

C = r'''
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#include "ff7_tri_core.h"
#include "ff7_tri2_core.h"
__attribute__((section(".text.entry"), used))
void entry(const tri_state * s, int count, const uint16_t * idx, const uint8_t * verts, int mode) {
    if (mode == 2) tri2_core_draw(s, count, idx, verts);
    else tri2_core_draw_mode(s, mode, count, idx, verts);
}
void * memcpy(void * d, const void * s, unsigned n) {
    unsigned char * dd = d; const unsigned char * ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
'''

def build_blob():
    d = tempfile.mkdtemp()
    src, elf, binf = os.path.join(d, 'b.c'), os.path.join(d, 'b.elf'), os.path.join(d, 'b.bin')
    open(src, 'w').write(C)
    subprocess.run(['arm-vita-eabi-gcc', '-mfloat-abi=softfp', '-O2', '-ffreestanding', '-nostdlib', '-nostartfiles',
                    '-static', '-Wl,-Ttext=0x%x' % BLOB, '-Wl,-e,entry', '-Wl,--gc-sections', '-I', SRC, src, '-lgcc',
                    '-o', elf], check=True)
    subprocess.run(['arm-vita-eabi-objcopy', '-O', 'binary', elf, binf], check=True)
    nm = subprocess.run(['arm-vita-eabi-nm', elf], capture_output=True, text=True, check=True).stdout
    return open(binf, 'rb').read(), [int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' entry')][0]

def f32(x):
    try: return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError: return math.copysign(math.inf, x)

def main():
    blob, entry = build_blob()
    emu = Emu(sys.argv[1], resolve_defined=True)
    uc = emu.uc
    uc.mem_map(BLOB, 0x30000); uc.mem_write(BLOB, blob)
    uc.mem_map(STATE, 0x1000); uc.mem_map(DATA, 0x100000)
    ret = lambda u, a, s, ud: u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, ret, begin=B + GLBEGIN, end=B + GLBEGIN + 1)
    uc.hook_add(UC_HOOK_CODE, ret, begin=B + GLEND, end=B + GLEND + 1)
    # tri_state: cur, last, r, g, b, a, u, v, keep_alpha (all .so globals)
    uc.mem_write(STATE, struct.pack('<9I', B + CUR, B + LAST, B + CR, B + CG, B + CB, B + CA, B + CU, B + CV,
                                    B + 0x141fad8))

    calls = int(sys.argv[2]) if len(sys.argv) > 2 else 600
    random.seed(9)
    specials = [0.0, -0.0, 1e-30, 1e30, math.inf, -math.inf, math.nan]
    bad = {m: 0 for m in FUNCS}
    for it in range(calls):
        mode = it % 4
        nverts = random.randrange(3, 60)
        count = random.choice([0, -1, 1, 2, 4]) if it % 20 == 0 else 3 * random.randrange(1, 40)
        ntri = -(-count // 3) if count > 0 else 0
        idx = [random.randrange(nverts) for _ in range(3 * ntri)] or [0]

        def rv(lo, hi):
            return f32(random.choice(specials)) if random.random() < 0.03 else f32(random.uniform(lo, hi))
        verts = b''.join(struct.pack('<fffI4B4xff', rv(-300, 300), rv(-300, 300), rv(-100, 1000),
                                     random.getrandbits(32), *[random.randrange(256) for _ in range(4)],
                                     rv(0, 1), rv(0, 1)) for _ in range(nverts))
        room = random.choice([NV, NV, 7, 0, 30])
        IDX, VTX = DATA, DATA + 0x10000
        uc.mem_write(IDX, struct.pack('<%dH' % len(idx), *idx))
        uc.mem_write(VTX, verts)
        init = bytes(random.getrandbits(8) for _ in range(12))  # current color and u/v before the call

        outs = []
        for run in (0, 1):
            uc.mem_write(B + VERTICES, b'\xcc' * (0x18 * NV))
            emu.w32(B + CUR, B + VERTICES)
            emu.w32(B + LAST, B + VERTICES + 0x18 * room)
            uc.mem_write(B + CA, init[:4]); uc.mem_write(B + CV, init[4:8]); uc.mem_write(B + CU, init[8:12])
            uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000); uc.reg_write(UC_ARM_REG_LR, END | 1)
            if run == 0:
                uc.reg_write(UC_ARM_REG_R0, count & 0xffffffff)
                uc.reg_write(UC_ARM_REG_R1, IDX); uc.reg_write(UC_ARM_REG_R2, VTX)
                uc.emu_start(B + FUNCS[mode][0] | 1, END, count=5000000)
            else:
                uc.reg_write(UC_ARM_REG_R0, STATE); uc.reg_write(UC_ARM_REG_R1, count & 0xffffffff)
                uc.reg_write(UC_ARM_REG_R2, IDX); uc.reg_write(UC_ARM_REG_R3, VTX)
                sp = HSTACK + 0xff000 - 8
                uc.mem_write(sp, struct.pack('<I', mode)); uc.reg_write(UC_ARM_REG_SP, sp)
                uc.emu_start(entry | 1, END, count=5000000)
            outs.append((emu.r32(B + CUR) - (B + VERTICES), bytes(uc.mem_read(B + VERTICES, 0x18 * NV)),
                         bytes(uc.mem_read(B + CA, 4)), bytes(uc.mem_read(B + CV, 8))))
        (oc, ob, ocol, ouv), (nc, nb, ncol, nuv) = outs
        problems = []
        if oc != nc: problems.append('pCurVertex %d / %d' % (nc, oc))
        if ob != nb:
            k = next(i for i in range(len(ob)) if ob[i] != nb[i])
            problems.append('VERTICES differ from byte %d (vertex %d, +%d)' % (k, k // 0x18, k % 0x18))
        if ocol != ncol: problems.append('current color %s / %s' % (ncol.hex(), ocol.hex()))
        if ouv != nuv: problems.append('current u/v %s / %s' % (nuv.hex(), ouv.hex()))
        if problems:
            bad[mode] += 1
            if sum(bad.values()) <= 5:
                print('MISMATCH %s call %d (count %d, room %d): %s' % (FUNCS[mode][1], it, count, room, '; '.join(problems)))
    for m in FUNCS:
        print('%s: %d mismatches in %d calls (vertices with colors, pCurVertex, current color and u/v)'
              % (FUNCS[m][1], bad[m], len(range(m, calls, 4))))
    sys.exit(1 if sum(bad.values()) else 0)

if __name__ == '__main__':
    main()
