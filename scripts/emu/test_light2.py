#!/usr/bin/env python3
"""
Compares source/ff7_light_core.h (light_core_vertex2) with the recompiled
0x68DE75 under emulation: output bytes, EAX/ECX/EDX and x87 C0/C2/C3.

Usage: scripts/emu/test_light2.py libjni_ff7.so [vertices]
Needs a C compiler (cc) and `pip install unicorn`.
"""
import ctypes, math, os, random, struct, subprocess, sys, tempfile
from ff7emu import Emu, XVA, B, FPU

FUNC = 0xccf790  # _Z15func_0x0068de75v
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'source')

def f32(x):
    try: return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError: return math.copysign(math.inf, x)

def build_core():
    d = tempfile.mkdtemp()
    c = os.path.join(d, 'core.c')
    open(c, 'w').write('#include "ff7_light_core.h"\n'
                       'int vertex2(uint32_t c, const float * s, const float * m, const float * n, const float * rgb,'
                       ' uint8_t src3, uint8_t * out, int * last, int * lt, int * eq)'
                       ' { return light_core_vertex2(c, s, m, n, rgb, src3, out, last, lt, eq); }\n')
    lib = os.path.join(d, 'libcore.so')
    subprocess.run(['cc', '-O2', '-fno-fast-math', '-ffp-contract=off', '-shared', '-fPIC', '-I', SRC, c, '-o', lib],
                   check=True)
    return ctypes.CDLL(lib)

def main():
    lib = build_core()
    emu = Emu(sys.argv[1])
    total = int(sys.argv[2]) if len(sys.argv) > 2 else 10000
    F3, F12, U4 = ctypes.c_float * 3, ctypes.c_float * 12, ctypes.c_uint8 * 4
    specials = [f32(x) for x in (0.0, -0.0, 1.0, 255.0, 254.99998, 255.00002, 1e-40, 1e30,
                                 math.inf, -math.inf, math.nan)]
    def rv(lo, hi): return random.choice(specials) if random.random() < 0.1 else f32(random.uniform(lo, hi))
    random.seed(1)
    bad = fallback = 0
    for it in range(total):
        base = XVA + 0x2000 + random.randrange(0, 0x400) * 4
        BV, M, N, RGB, S, OUT = [base + 0x100 * i + random.randrange(0, 0x40) for i in range(6)]
        m = [rv(-2, 2) for _ in range(12)]; n = [rv(-1, 1) for _ in range(3)]
        if it % 3 == 0:  # l == ambient exactly
            n = [0.0, 0.0, 0.0]
            for r in range(3): m[4 * r + 3] = rv(-1, 1)
        if it % 5 == 0:  # field-model ranges
            m = [f32(random.uniform(-1, 1)) for _ in range(12)]
            for r in range(3): m[4 * r + 3] = f32(random.uniform(0, 0.4))
            rgb = [f32(random.uniform(0, 255)) for _ in range(3)]
            bv = [f32(random.uniform(0, 1.5)) for _ in range(3)]
        else:
            rgb = [rv(-50, 300) for _ in range(3)]; bv = [rv(-2, 2) for _ in range(3)]
        src = bytes(random.randrange(256) for _ in range(4)); color = random.getrandbits(32)
        emu.wva(BV, struct.pack('<3f', *bv)); emu.wva(M, struct.pack('<12f', *m)); emu.wva(N, struct.pack('<3f', *n))
        emu.wva(RGB, struct.pack('<3f', *rgb)); emu.wva(S, src); emu.wva(OUT, b'\xaa' * 4)
        for i in (0x5c, 0x5e, 0x64): emu.uc.mem_write(B + FPU + i, b'\x07')
        emu.call_x86(FUNC, [color, BV, M, N, RGB, S, OUT])
        out, last, lt, eq = U4(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
        if not lib.vertex2(ctypes.c_uint32(color), F3(*bv), F12(*m), F3(*n), F3(*rgb), ctypes.c_uint8(src[3]), out,
                           ctypes.byref(last), ctypes.byref(lt), ctypes.byref(eq)):
            fallback += 1  # NaN in a comparison: the native code calls the original
            continue
        mine = bytes(out)
        edx = ((M if last.value == 0 else OUT) & ~0xff) | mine[3]
        fb = bytes(emu.uc.mem_read(B + FPU, 0x66))
        if (emu.rva(OUT, 4) != mine or emu.reg(0) != OUT or emu.reg(1) != S or emu.reg(2) != edx
                or fb[0x5c] != lt.value or fb[0x5e] != 0 or fb[0x64] != eq.value):
            bad += 1
            if bad <= 5:
                print('MISMATCH', emu.rva(OUT, 4).hex(), mine.hex(), hex(emu.reg(2)), hex(edx))
    print('0x68DE75: %d mismatches in %d vertices (%d NaN cases left to the original)'
          % (bad, total - fallback, fallback))
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
