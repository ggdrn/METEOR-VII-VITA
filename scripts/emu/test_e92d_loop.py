#!/usr/bin/env python3
"""
Runs the per-vertex lighting loop of 0x68E92D (0xD1D288-0xD1D424) under
emulation and compares it with the model the native version implements
(ff7_light.c): for each of `count` entries,
    0x68DE75(ctx[0x5a8], ebp-0x34, ctx+0xf8, *q, q+4, q+0x14, p+0x10)
then p += 0x20, q += 0x18; at the end count = 0, EAX = 0, ECX = p, EDX = q.

Usage: scripts/emu/test_e92d_loop.py libjni_ff7.so [runs]
"""
import ctypes, random, struct, sys
from unicorn.arm_const import *
from ff7emu import Emu, XVA, B, FPU, REGS, HSTACK, END
from test_light2 import build_core, f32

LOOP, EXIT = 0xd1d288, 0xd1d424

def main():
    lib = build_core()
    emu = Emu(sys.argv[1]); uc = emu.uc
    runs = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    F3, F12, U4 = ctypes.c_float * 3, ctypes.c_float * 12, ctypes.c_uint8 * 4
    random.seed(4); bad = 0
    for it in range(runs):
        EBP, CTX, NORMALS = XVA + 0x8000, XVA + 0x9000 + random.randrange(0, 64) * 4, XVA + 0xc000
        Q, P = XVA + 0x10000, XVA + 0x18000 + random.randrange(0, 16) * 4
        count = random.randrange(1, 60)
        color = random.getrandbits(32)
        m = [f32(random.uniform(-1, 1)) for _ in range(12)]
        for r in range(3): m[4 * r + 3] = f32(random.uniform(0, 0.5))
        scale = [f32(random.uniform(0, 1.5)) for _ in range(3)]
        emu.wva(CTX + 0x5a8, struct.pack('<I', color)); emu.wva(CTX + 0xf8, struct.pack('<12f', *m))
        emu.wva(EBP - 0x34, struct.pack('<3f', *scale)); emu.wva(EBP + 0xc, struct.pack('<I', CTX))
        emu.wva(EBP - 0x24, struct.pack('<i', count)); emu.wva(EBP - 0x20, struct.pack('<I', P))
        emu.wva(EBP - 0x1c, struct.pack('<I', Q))
        entries = []
        for i in range(count):
            n = [f32(random.uniform(-1, 1)) for _ in range(3)]
            na = NORMALS + 12 * random.randrange(0, 200)
            emu.wva(na, struct.pack('<3f', *n))
            rgb = [f32(random.uniform(0, 255)) for _ in range(3)]
            src = bytes(random.randrange(256) for _ in range(4))
            emu.wva(Q + 0x18 * i, struct.pack('<I3fI', na, *rgb, 0) + src)
            entries.append((na, rgb, src))
        emu.wva(P, b'\xee' * (0x20 * count + 0x20))
        emu.setreg(5, EBP); emu.setreg(4, EBP - 0x40)
        for i in (0, 1, 2, 3, 6, 7): emu.setreg(i, 0x1000 + i)
        uc.reg_write(UC_ARM_REG_R4, B + REGS); uc.reg_write(UC_ARM_REG_R5, B + REGS)
        uc.reg_write(UC_ARM_REG_R6, 0); uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000)
        uc.emu_start(B + LOOP | 1, B + EXIT, count=10000000)
        # model
        exp = bytearray(b'\xee' * (0x20 * count + 0x20)); lt = eq = 0
        for i, (na, rgb, src) in enumerate(entries):
            out, last, l_, e_ = U4(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
            n = struct.unpack('<3f', emu.rva(na, 12))
            assert lib.vertex2(ctypes.c_uint32(color), F3(*scale), F12(*m), F3(*n), F3(*rgb), ctypes.c_uint8(src[3]),
                               out, ctypes.byref(last), ctypes.byref(l_), ctypes.byref(e_))
            exp[0x20 * i + 0x10: 0x20 * i + 0x14] = bytes(out); lt, eq = l_.value, e_.value
        got = emu.rva(P, len(exp))
        fb = bytes(uc.mem_read(B + FPU, 0x66))
        state = (struct.unpack('<i', emu.rva(EBP - 0x24, 4))[0], struct.unpack('<I', emu.rva(EBP - 0x20, 4))[0],
                 struct.unpack('<I', emu.rva(EBP - 0x1c, 4))[0], emu.reg(0), emu.reg(1), emu.reg(2), emu.reg(4),
                 fb[0x5c], fb[0x64])
        want = (0, P + 0x20 * count, Q + 0x18 * count, 0, P + 0x20 * count, Q + 0x18 * count, EBP - 0x40, lt, eq)
        if got != bytes(exp) or state != want:
            bad += 1
            if bad <= 3: print('MISMATCH', state, want, got == bytes(exp))
    print('0x68E92D loop: %d mismatches in %d runs' % (bad, runs))
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
