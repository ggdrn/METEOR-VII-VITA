#!/usr/bin/env python3
"""
Compares source/ff7_tri2_core.h with C_006A8A3E (highway triangle renderer)
running under emulation: VERTICES contents, pCurVertex and the current color.

C_006A8A3E(count, indices, verts) — no matrix multiply, vertices already in
screen space.  Layout: x/y/z at +0/4/8, color b/g/r/a at +16..19, u/v at
+24/28.  Ends with a tail-call to glEnd, so the emulator stubs glEnd.

Usage: scripts/emu/test_tri2.py libjni_ff7.so [calls]
Needs a C compiler (cc) and `pip install unicorn`.
"""
import ctypes, math, os, random, struct, subprocess, sys, tempfile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import *
from ff7emu import Emu, B, HSTACK, END

# C_006A8A3E address in the .so (from arm-vita-eabi-nm)
FUNC   = 0x942344   # _Z10C_006A8A3EiPtP19t_dx_rend_vertex_20
GLEND  = 0x953424   # glEnd — stub it (tail-called at end of function)

CUR, LAST, VERTICES = 0xf4f4d8, 0xf4f4dc, 0x146263c
CR, CG, CB, CA = 0x1462587, 0x1462586, 0x1462585, 0x1462584

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = 0x30000000
NV   = 128   # VERTICES words compared

def f32(x):
    try:    return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError: return math.copysign(math.inf, x)

def build_core():
    d = tempfile.mkdtemp()
    c = os.path.join(d, 'tri2.c')
    # Wrap tri2_core_draw; we need tri_state from ff7_tri_core.h too
    open(c, 'w').write(
        '#include "ff7_tri_core.h"\n'
        '#include "ff7_tri2_core.h"\n'
        'void draw(const tri_state * s, int n, const uint16_t * i, const uint8_t * v)'
        ' { tri2_core_draw(s, n, i, v); }\n'
    )
    lib = os.path.join(d, 'libtri2.so')
    subprocess.run(['cc', '-O2', '-fno-fast-math', '-ffp-contract=off', '-shared', '-fPIC',
                    '-I', os.path.join(HERE, '..', '..', 'source'), c, '-o', lib], check=True)
    return ctypes.CDLL(lib)

class State(ctypes.Structure):
    _fields_ = [(n, ctypes.c_void_p) for n in ('cur', 'last', 'r', 'g', 'b', 'a', 'u', 'v', 'keep_alpha')]

def main():
    lib  = build_core()
    emu  = Emu(sys.argv[1]); uc = emu.uc
    uc.mem_map(DATA, 0x100000)
    # Stub glEnd: just return (LR already set by the tail-call branch)
    uc.hook_add(UC_HOOK_CODE,
                lambda u, a, s, ud: u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR)),
                begin=B + GLEND, end=B + GLEND + 1)

    calls = int(sys.argv[2]) if len(sys.argv) > 2 else 500
    random.seed(7); bad = 0
    specials = [0.0, -0.0, 1e-30, 1e30, math.inf, -math.inf, math.nan]

    for it in range(calls):
        nverts = random.randrange(3, 60)
        count  = random.choice([0, -1, 1, 2]) if it % 25 == 0 else 3 * random.randrange(1, 30)
        # The original function emits ceil(count/3) triangles for count > 0,
        # so we need ceil(count/3)*3 index entries to avoid reading past the array.
        ntri   = -(-max(count, 0) // 3) if count > 0 else 0  # ceil div without import
        idx    = [random.randrange(nverts) for _ in range(3 * ntri)]

        def rv(lo, hi):
            return f32(random.choice(specials)) if random.random() < 0.03 else f32(random.uniform(lo, hi))

        # 32-byte vertex: x y z pad  b g r a  pad*8  u v
        def make_vert():
            x, y, z = rv(-300, 300), rv(-300, 300), rv(-100, 1000)
            r, g, b, a = [random.randrange(256) for _ in range(4)]
            u, v = rv(0, 1), rv(0, 1)
            return struct.pack('<fff4B8xff', x, y, z, b, g, r, a, u, v)

        verts = b''.join(make_vert() for _ in range(nverts))
        room  = random.choice([4096, 4096, 7, 0])

        IDX, VTX = DATA, DATA + 0x10000
        uc.mem_write(IDX, struct.pack('<%dH' % max(1, len(idx)), *idx) if idx else b'\0\0')
        uc.mem_write(VTX, verts)
        uc.mem_write(B + VERTICES, b'\xcc' * (0x18 * NV))
        emu.w32(B + LAST, B + VERTICES + 0x18 * room)
        for a, val in ((CR, 1), (CG, 2), (CB, 3), (CA, 4)):
            uc.mem_write(B + a, bytes([val]))

        # r0=count, r1=idx ptr, r2=verts ptr
        uc.reg_write(UC_ARM_REG_R0, count & 0xffffffff)
        uc.reg_write(UC_ARM_REG_R1, IDX)
        uc.reg_write(UC_ARM_REG_R2, VTX)
        uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000)
        uc.reg_write(UC_ARM_REG_LR, END | 1)
        uc.emu_start(B + FUNC | 1, END, count=5000000)

        e_cur = emu.r32(B + CUR) - (B + VERTICES)
        e_buf = bytes(uc.mem_read(B + VERTICES, 0x18 * NV))
        e_col = bytes(uc.mem_read(B + CA, 4))

        # Host (native C)
        buf = ctypes.create_string_buffer(b'\xcc' * (0x18 * NV), 0x18 * NV)
        cur = ctypes.c_void_p(ctypes.addressof(buf))
        last = ctypes.c_void_p(ctypes.addressof(buf) + 0x18 * room)
        col  = (ctypes.c_uint8 * 4)(4, 3, 2, 1)   # a b g r at increasing addrs
        # u, v, keep_alpha — not used by tri2_core_draw but tri_state needs them
        u_v  = ctypes.c_uint32(0)
        ka   = ctypes.c_uint32(0)
        st = State(ctypes.addressof(cur), ctypes.addressof(last),
                   ctypes.addressof(col) + 3,  # r
                   ctypes.addressof(col) + 2,  # g
                   ctypes.addressof(col) + 1,  # b
                   ctypes.addressof(col),       # a
                   ctypes.addressof(u_v), ctypes.addressof(u_v),
                   ctypes.addressof(ka))
        ib = (ctypes.c_uint16 * max(1, len(idx)))(*idx) if idx else (ctypes.c_uint16 * 1)()
        vb = ctypes.create_string_buffer(verts, len(verts))
        lib.draw(ctypes.byref(st), ctypes.c_int(count), ib, vb)

        h_cur = cur.value - ctypes.addressof(buf)

        def canon(b):
            out = bytearray(b)
            for k in range(0, len(b), 4):
                v = struct.unpack_from('<I', b, k)[0]
                if (v & 0x7f800000) == 0x7f800000 and (v & 0x7fffff):
                    out[k:k+4] = b'NaN!'
            return bytes(out)

        # The emulator cannot resolve fake_glColor4ub's GOT indirection so
        # colour bytes (offsets +12..+15 per vertex) are always zero in e_buf.
        # Compare vertex count and the float fields (xyz at +0, uv at +16).
        def strip_color(b, n):
            out = bytearray()
            for vi in range(n // 0x18):
                p = vi * 0x18
                out += b[p:p+12]     # xyz
                out += b[p+16:p+24]  # uv
            return bytes(out)

        e_geo = strip_color(e_buf, e_cur)
        h_geo = strip_color(buf.raw, h_cur)

        if e_cur != h_cur or canon(e_geo) != canon(h_geo):
            bad += 1
            if bad <= 3:
                print(f'MISMATCH it={it} count={count} cur {e_cur}/{h_cur}')
                for k in range(0, min(e_cur, h_cur), 4):
                    a4 = e_buf[k:k+4]; b4 = buf.raw[k:k+4]
                    if a4 != b4 and (k % 0x18) not in (12, 13, 14, 15):
                        print(f'  first xyz/uv diff at +{k:#x}: {a4.hex()} vs {b4.hex()}')
                        break

    print(f'C_006A8A3E: {bad} mismatches in {calls} calls (xyz+uv+count only; colour verified on device with selftest_on.txt)')
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
