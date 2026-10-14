#!/usr/bin/env python3
"""
Compares source/ff7_tri_core.h with the porters' C_006A8002 triangle renderer
running under emulation: VERTICES contents, pCurVertex and the current color.

Usage: scripts/emu/test_tri.py libjni_ff7.so [calls]
Needs a C compiler (cc) and `pip install unicorn`.
"""
import ctypes, math, os, random, struct, subprocess, sys, tempfile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import *
from ff7emu import Emu, B, HSTACK, END

FUNC, GLEND = 0x941d4c, 0x953424
CUR, LAST, VERTICES = 0xf4f4d8, 0xf4f4dc, 0x146263c
CR, CG, CB, CA, CU, CV = 0x1462587, 0x1462586, 0x1462585, 0x1462584, 0x146258c, 0x1462588
FLAG = 0x141fad8
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = 0x30000000  # indices, vertices, matrix
NV = 128           # VERTICES compared (count < 3 * 30)

def f32(x):
    try: return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError: return math.copysign(math.inf, x)

def build_core():
    d = tempfile.mkdtemp()
    c = os.path.join(d, 'tri.c')
    open(c, 'w').write('#include "ff7_tri_core.h"\n'
                       'void draw(const tri_state * s, int n, const uint16_t * i, const uint8_t * v, const float * m)'
                       ' { tri_core_draw(s, n, i, v, m); }\n')
    lib = os.path.join(d, 'libtri.so')
    subprocess.run(['cc', '-O2', '-fno-fast-math', '-ffp-contract=off', '-shared', '-fPIC',
                    '-I', os.path.join(HERE, '..', '..', 'source'), c, '-o', lib], check=True)
    return ctypes.CDLL(lib)

class State(ctypes.Structure):
    _fields_ = [(n, ctypes.c_void_p) for n in ('cur', 'last', 'r', 'g', 'b', 'a', 'u', 'v', 'keep_alpha')]

def main():
    lib = build_core()
    emu = Emu(sys.argv[1]); uc = emu.uc
    uc.mem_map(DATA, 0x100000)
    uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, u: uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR)),
                begin=B + GLEND, end=B + GLEND + 1)
    calls = int(sys.argv[2]) if len(sys.argv) > 2 else 500
    random.seed(2); bad = 0
    specials = [0.0, -0.0, 1e-30, -1e-30, 1e30, math.inf, -math.inf, math.nan]
    for it in range(calls):
        nverts = random.randrange(3, 40)
        count = random.choice([0, -3, 1, 2]) if it % 25 == 0 else 3 * random.randrange(1, 30) + random.choice([0, 0, 0, 1, 2])
        ntri = max(0, (count + 2) // 3)
        idx = [random.randrange(nverts) for _ in range(3 * ntri)]
        def rv(lo, hi): return f32(random.choice(specials)) if random.random() < 0.03 else f32(random.uniform(lo, hi))
        verts = b''.join(struct.pack('<4fI3f', rv(-100, 100), rv(-100, 100), rv(-100, 100), rv(-1, 1),
                                     random.getrandbits(32), 0, 0, 0) for _ in range(nverts))
        m = [rv(-2, 2) for _ in range(16)]
        if random.random() < 0.7: m[15] = f32(random.uniform(50, 300))  # mostly w > 0
        keep = random.choice([0, 1, 0x100])
        u, v = random.getrandbits(32), random.getrandbits(32)
        room = random.choice([4096, 4096, 7, 0])  # vertices of room left in VERTICES
        IDX, VTX, MAT = DATA, DATA + 0x10000, DATA + 0x20000
        uc.mem_write(IDX, struct.pack('<%dH' % len(idx), *idx) if idx else b'')
        uc.mem_write(VTX, verts); uc.mem_write(MAT, struct.pack('<16f', *m))
        uc.mem_write(B + VERTICES, b'\xcc' * (0x18 * NV))
        emu.w32(B + LAST, B + VERTICES + 0x18 * room)
        for a, val in ((CR, 1), (CG, 2), (CB, 3), (CA, 4)): uc.mem_write(B + a, bytes([val]))
        emu.w32(B + CU, u); emu.w32(B + CV, v); emu.w32(B + FLAG, keep)
        uc.reg_write(UC_ARM_REG_R0, count & 0xffffffff); uc.reg_write(UC_ARM_REG_R1, IDX)
        uc.reg_write(UC_ARM_REG_R2, VTX); uc.reg_write(UC_ARM_REG_R3, MAT)
        uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000); uc.reg_write(UC_ARM_REG_LR, END | 1)
        uc.emu_start(B + FUNC | 1, END, count=5000000)
        e_cur = emu.r32(B + CUR) - (B + VERTICES)
        e_buf = bytes(uc.mem_read(B + VERTICES, 0x18 * NV))
        e_col = bytes(uc.mem_read(B + CA, 4))
        # host
        buf = ctypes.create_string_buffer(b'\xcc' * (0x18 * NV), 0x18 * NV)
        cur = ctypes.c_void_p(ctypes.addressof(buf)); last = ctypes.c_void_p(ctypes.addressof(buf) + 0x18 * room)
        col = (ctypes.c_uint8 * 4)(4, 3, 2, 1)  # a, b, g, r at increasing addresses
        uu, vv, kk = ctypes.c_uint32(u), ctypes.c_uint32(v), ctypes.c_uint32(keep)
        st = State(ctypes.addressof(cur), ctypes.addressof(last), ctypes.addressof(col) + 3, ctypes.addressof(col) + 2,
                   ctypes.addressof(col) + 1, ctypes.addressof(col), ctypes.addressof(uu), ctypes.addressof(vv),
                   ctypes.addressof(kk))
        ib = (ctypes.c_uint16 * max(1, len(idx)))(*idx) if idx else (ctypes.c_uint16 * 1)()
        vb = ctypes.create_string_buffer(verts, len(verts)); mb = (ctypes.c_float * 16)(*m)
        lib.draw(ctypes.byref(st), ctypes.c_int(count), ib, vb, mb)
        h_cur = cur.value - ctypes.addressof(buf)
        # NaN payloads/signs: the PC's default NaN differs from ARM's (the Vita runs
        # the same vdiv as the original), so any NaN matches any NaN here
        def canon(b):
            out = bytearray(b)
            for k in range(0, len(b), 4):
                if (struct.unpack_from('<I', b, k)[0] & 0x7f800000) == 0x7f800000 and struct.unpack_from('<I', b, k)[0] & 0x7fffff:
                    out[k:k+4] = b'NaN!'
            return bytes(out)
        if (e_cur, canon(e_buf), e_col) != (h_cur, canon(buf.raw), bytes(col)):
            bad += 1
            if bad <= 3:
                print('MISMATCH count %d cur %d/%d col %s/%s' % (count, e_cur, h_cur, e_col.hex(), bytes(col).hex()))
                for k in range(0, 0x18 * NV, 4):
                    if e_buf[k:k+4] != buf.raw[k:k+4]:
                        print('  first diff at +%#x: %s vs %s' % (k, e_buf[k:k+4].hex(), buf.raw[k:k+4].hex())); break
    print('C_006A8002: %d mismatches in %d calls' % (bad, calls))
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
