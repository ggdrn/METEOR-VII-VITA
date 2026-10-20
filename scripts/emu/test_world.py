#!/usr/bin/env python3
"""
Compares source/ff7_world_core.h with the recompiled code under emulation:
 - world_core_project / 0x75F263 (world map: transform, project and
   clip-flag 126 vertices): every word it writes in x86 memory, EAX/ECX/EDX,
   ESP/EBP, the x87 stack top and the zero-divide flag;
 - world_core_light / 0x68DAE1 (vertex color): output bytes, EAX/ECX/EDX,
   x87 C0/C2/C3 and stack top;
 - world_core_mesh / 0x75F0AD (mesh vertices, world curvature): the whole
   function against its four setup calls run under emulation followed by
   the native loop: x86 memory, EAX/ECX/EDX, ESP/EBP, x87 top and
   zero-divide flag.

Usage: scripts/emu/test_world.py libjni_ff7.so [calls] [vertices] [ff7_en.exe]
ff7_en.exe (from the OBB) provides the .rdata constants the setup calls use;
without it the mesh test is skipped.
Needs a C compiler (cc) and `pip install unicorn`.
"""
import ctypes, math, os, random, struct, subprocess, sys, tempfile
from ff7emu import Emu, B, FPU, MEMMAP, XVA

FUNC = 0x639134  # _Z15func_0x0075f263v
LIGHT = 0xccc8a8  # _Z15func_0x0068dae1v
MESH = 0x63876c   # _Z15func_0x0075f0adv
MESH_SETUP = (0xc35d1c, 0xc39614, 0xd26c8c, 0xc95fec)  # 0x66100d, 0x6617e9, 0x69c69f, 0x67d2bf
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'source')

# x86 pages the function touches, backed by extra emulator memory
PAGES = (0xde6, 0xe2c, 0xe38, 0xe39, 0xe3a)
HOST = 0x51000000
IN, OUT, PROJ, M, FLAGS = 0xe38338, 0xe38f38, 0xe38938, 0xe39738, 0xe397b8
K_VA = 0x7b7ef8
A_VA, B_VA, CW_VA, DH_VA = 0xe2c424, 0xe2c428, 0xde68f8, 0xde67e8

def f32(x):
    try: return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError: return math.copysign(math.inf, x)

def build_core():
    d = tempfile.mkdtemp()
    c = os.path.join(d, 'core.c')
    open(c, 'w').write('#include "ff7_world_core.h"\n'
                       'int project(const float * m, const float * in, float * out, float * proj, int32_t * flags,'
                       ' float k, int32_t a, int32_t b, int32_t c, int32_t d)'
                       ' { return world_core_project(m, (const float (*)[3]) in, (float (*)[4]) out,'
                       ' (float (*)[3]) proj, flags, k, a, b, c, d); }\n'
                       'int light(const float * m, const float * n, const float * rgb, uint8_t * out, int * path,'
                       ' int * lt, int * eq) { int r = world_core_light(m, n, rgb, 0.0f, 255.0f, out, path, lt, eq);'
                       ' return r ? 1 + world_core_light_edx(path) : 0; }\n'
                       'int mesh(const int16_t * src, int32_t step, const float * m1, const float * m2, float * in,'
                       ' float * proj, int32_t c, int32_t v0, float k1, float k2, int32_t * eax, int32_t * ecx)'
                       ' { return world_core_mesh(src, step, m1, m2, (float (*)[3]) in, (float (*)[3]) proj, c, v0,'
                       ' k1, k2, eax, ecx); }\n')
    lib = os.path.join(d, 'libcore.so')
    subprocess.run(['cc', '-O2', '-fno-fast-math', '-ffp-contract=off', '-shared', '-fPIC', '-I', SRC, c, '-o', lib],
                   check=True)
    return ctypes.CDLL(lib)

def main():
    lib = build_core()
    lib.project.argtypes = [ctypes.c_void_p] * 5 + [ctypes.c_float] + [ctypes.c_int32] * 4
    emu = Emu(sys.argv[1])
    total = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    uc = emu.uc
    uc.mem_map(HOST, 0x1000 * len(PAGES))
    for i, p in enumerate(PAGES):
        emu.w32(B + MEMMAP + 4 * p, HOST + 0x1000 * i)
    def host(va): return HOST + 0x1000 * PAGES.index(va >> 12) + (va & 0xfff)
    def w(va, data): uc.mem_write(host(va), bytes(data))
    def r(va, n): return bytes(uc.mem_read(host(va), n))
    span = (FLAGS + 4 * 130) - IN  # everything from in[] to past the flags

    specials = [f32(x) for x in (0.0, -0.0, 1.0, -1.0, 1e-40, 1e30, -1e30, 2147483648.0, -2147483904.0,
                                 math.inf, -math.inf, math.nan)]
    random.seed(1)
    bad = 0
    for it in range(total):
        sp = it % 4 == 3  # some calls with special values
        def rv(lo, hi): return random.choice(specials) if sp and random.random() < 0.05 else f32(random.uniform(lo, hi))
        m = [rv(-2, 2) for _ in range(16)]
        if it % 4 == 0:  # world-map-like camera: w around 100-10000
            m[3], m[7], m[11], m[15] = f32(random.uniform(-0.5, 0.5)), f32(random.uniform(-0.5, 0.5)), \
                f32(random.uniform(0.5, 1)), f32(random.uniform(100, 5000))
        vin = [rv(-20000, 20000) for _ in range(3 * 130)]
        if it % 7 == 0:  # w == 0 for some vertices
            m[3] = m[7] = m[11] = m[15] = 0.0
        k = 1.0 if it % 5 else rv(0.5, 400)
        a, b = random.randrange(-50, 50), random.randrange(-50, 50)
        cw, dh = random.randrange(100, 700), random.randrange(100, 500)
        # junk everywhere, then the inputs
        w(IN, bytes(random.getrandbits(8) for _ in range(span)))
        w(IN, struct.pack('<%df' % (3 * 126), *vin[:3 * 126]))
        w(M, struct.pack('<16f', *m))
        w(A_VA, struct.pack('<ii', a, b))
        w(CW_VA, struct.pack('<i', cw))
        w(DH_VA, struct.pack('<i', dh))
        uc.mem_write(0x50000000 + (K_VA & 0xfff), struct.pack('<f', k))
        before = r(IN, span)
        fpu_before = bytes(uc.mem_read(B + FPU, 0x66))
        uc.mem_write(B + FPU + 0x56, b'\x00')
        ebp = 0x12345678
        emu.setreg(5, ebp)
        for i in (0, 1, 2): emu.setreg(i, 0xdead0000 + i)
        esp = emu.call_x86(FUNC, [])
        after = r(IN, span)
        fpu_after = bytes(uc.mem_read(B + FPU, 0x66))

        # Native
        F = ctypes.c_float
        cm = (F * 16)(*m); cin = (F * (3 * 126))(*vin[:3 * 126])
        mem = bytearray(before)
        out = (F * (4 * 126)).from_buffer(mem, OUT - IN)
        proj = (F * (3 * 123)).from_buffer(mem, PROJ - IN)
        flags = (ctypes.c_int32 * 123).from_buffer(mem, FLAGS - IN)
        zd = lib.project(cm, cin, out, proj, flags, F(k), a, b, a + cw, b + dh)
        del out, proj, flags
        mine = bytes(mem)

        problems = []
        if mine != after:
            diffs = [o for o in range(0, span, 4) if mine[o:o + 4] != after[o:o + 4]]
            problems.append('memory: %d words differ, first at VA %x (native %s, original %s)'
                            % (len(diffs), IN + diffs[0], mine[diffs[0]:diffs[0] + 4].hex(),
                               after[diffs[0]:diffs[0] + 4].hex()))
        regs = (emu.reg(0), emu.reg(1), emu.reg(2), emu.reg(4), emu.reg(5))
        if regs != (123, 0, 126, esp + 4, ebp):
            problems.append('regs %s' % [hex(x) for x in regs])
        if fpu_after[0x60] != fpu_before[0x60]: problems.append('x87 top moved')
        if fpu_after[0x56] != zd: problems.append('zero-divide flag %d, native %d' % (fpu_after[0x56], zd))
        if problems:
            bad += 1
            if bad <= 5: print('MISMATCH (call %d): %s' % (it, '; '.join(problems)))
    print('0x75F263: %d mismatches in %d calls (%d vertices)' % (bad, total, total * 126))
    bad_light = test_light(emu, lib, int(sys.argv[3]) if len(sys.argv) > 3 else 20000, specials)
    bad_mesh = test_mesh(sys.argv[1], lib, sys.argv[4], total) if len(sys.argv) > 4 else 0
    sys.exit(1 if bad or bad_light or bad_mesh else 0)

def pe_reader(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    n, opt = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    secs = []
    for i in range(n):
        o = pe + 24 + opt + 40 * i
        vs, va, rs, ro = struct.unpack_from('<IIII', d, o + 8)
        secs.append((base + va, vs, ro, rs))
    def page(p):
        va = p << 12
        for v, vs, ro, rs in secs:
            if v <= va < v + vs:
                off = va - v
                return d[ro + off:ro + min(off + 0x1000, rs)].ljust(0x1000, b'\0')
        return b'\0' * 0x1000
    return page

def test_mesh(so, lib, exe, total):
    page = pe_reader(exe)
    emu = Emu(so)
    uc = emu.uc
    # x86 pages: the exe's .rdata and .data as in the file, plus the world map globals
    pages = list(range(0x7b6, 0x7ba)) + list(range(0x900, 0x920)) + [0xdb2] + list(range(0xde0, 0xe40))
    HOSTM = 0x52000000
    uc.mem_map(HOSTM, 0x1000 * len(pages))
    for i, p in enumerate(pages):
        emu.w32(B + MEMMAP + 4 * p, HOSTM + 0x1000 * i)
        uc.mem_write(HOSTM + 0x1000 * i, page(p))
    def host(va): return HOSTM + 0x1000 * pages.index(va >> 12) + (va & 0xfff)
    def w(va, data): uc.mem_write(host(va), bytes(data))
    def r(va, n): return bytes(uc.mem_read(host(va), n))
    k1, k2 = struct.unpack('<ff', r(0x7b7ef0, 8))
    lo, hi = 0xe38000, 0xe3a000  # in, proj, out, matrices, flags
    SRC = XVA + 0x4000
    F = ctypes.c_float
    random.seed(3)
    bad = 0
    for it in range(total):
        # camera state the setup calls read, random world map globals
        w(0xe39738, struct.pack('<32f', *[f32(random.uniform(-2, 2)) for _ in range(32)]))
        w(0xe360f4, struct.pack('<I', random.choice([random.getrandbits(32), random.randrange(0, 4096)])))
        w(0x90aaf0, struct.pack('<I', random.choice([0x90ab00, 0xe39400])))
        w(0x90ab00, struct.pack('<16f', *[f32(random.uniform(-1, 1)) for _ in range(16)]))
        w(0xe39400, struct.pack('<16f', *[f32(random.uniform(-1, 1)) for _ in range(16)]))
        # 0x67d2bf: m1 = m2 * ([0xdb2bb8] + 0x8d0)
        w(0xdb2bb8, struct.pack('<I', 0x905000 - 0x8d0))
        w(0x905000, struct.pack('<16f', *[f32(random.uniform(-1, 1)) if it % 5 else random.choice([0.0, 1.0])
                                          for _ in range(16)]))
        w(0xe2c424, struct.pack('<i', random.randrange(-50, 50)))
        w(0xde68f8, struct.pack('<I', random.choice([640, 320, random.getrandbits(32)])))
        verts = [random.randrange(-32768, 32768) if it % 3 else random.randrange(-3000, 3000) for _ in range(4 * 122)]
        emu.wva(SRC, struct.pack('<%dh' % len(verts), *verts))
        step = random.choice([random.randrange(-4096, 4096), random.randrange(1, 64), -random.randrange(1, 64),
                              random.getrandbits(32) - (1 << 31)])
        esp0 = XVA + 0x10000
        v0 = random.getrandbits(32)
        V0 = esp0 - 8 - 4 - 4 - 4  # [ebp - 4]: below the 2 arguments, the return address and the saved EBP
        emu.wva(V0, struct.pack('<I', v0))
        state = bytes(uc.mem_read(HOSTM, 0x1000 * len(pages)))
        stack = emu.rva(esp0 - 0x100, 0x100)
        # Original
        uc.mem_write(B + FPU + 0x56, b'\x00')
        emu.setreg(5, 0x12345678)
        esp = emu.call_x86(MESH, [SRC, step & 0xffffffff], esp=esp0)
        orig = r(lo, hi - lo)
        oregs = [emu.reg(i) for i in (0, 1, 2, 4, 5)]
        ze = bytes(uc.mem_read(B + FPU + 0x56, 1))[0]
        # Native: the setup calls under emulation, then the loop in C
        uc.mem_write(HOSTM, state)
        emu.wva(esp0 - 0x100, stack)
        # As 0x75F0AD makes them: EBP = its frame, the callee's ESP = EBP - 0x3c
        # (0x6617e9 also sees 0xe39778, pushed for 0x66100d, as a second argument)
        ebp = esp0 - 12 - 4
        emu.setreg(5, ebp)
        for fn, args in zip(MESH_SETUP, ([0xe39778], None, None, [0xe39778, 0xe39738])):
            if fn == MESH_SETUP[1]: args = [emu.reg(0), 0xe39778]
            if fn == MESH_SETUP[2]: args = [0xe39778, struct.unpack('<I', r(0xe360f4, 4))[0]]
            emu.call_x86(fn, args, esp=ebp - 0x3c + 4 + 4 * len(args))
        v0_now = struct.unpack('<i', emu.rva(V0, 4))[0]
        a = struct.unpack('<i', r(0xe2c424, 4))[0]
        c = (struct.unpack('<I', r(0xde68f8, 4))[0] >> 1) + a
        c = ((c + (1 << 31)) & 0xffffffff) - (1 << 31)
        mem = bytearray(r(lo, hi - lo))
        IN, PROJ = 0xe38338, 0xe38938
        cin = (F * (3 * 123)).from_buffer(mem, IN - 12 - lo)
        cproj = (F * (3 * 122)).from_buffer(mem, PROJ - lo)
        m1 = (F * 16).from_buffer_copy(mem, 0xe39738 - lo)
        m2 = (F * 16).from_buffer_copy(mem, 0xe39778 - lo)
        eax, ecx = ctypes.c_int32(), ctypes.c_int32()
        src = (ctypes.c_int16 * len(verts))(*verts)
        zd = lib.mesh(src, ctypes.c_int32(step if step < (1 << 31) else step - (1 << 32)), m1, m2,
                      ctypes.cast(ctypes.addressof(cin) + 12, ctypes.c_void_p), cproj, ctypes.c_int32(c),
                      ctypes.c_int32(v0_now), F(k1), F(k2), ctypes.byref(eax), ctypes.byref(ecx))
        del cin, cproj
        mine = bytes(mem)
        problems = []
        if mine != orig:
            diffs = [o for o in range(0, len(mine), 4) if mine[o:o + 4] != orig[o:o + 4]]
            problems.append('memory: %d words differ, first at VA %x (native %s, original %s)'
                            % (len(diffs), lo + diffs[0], mine[diffs[0]:diffs[0] + 4].hex(),
                               orig[diffs[0]:diffs[0] + 4].hex()))
        want = [eax.value & 0xffffffff, ecx.value & 0xffffffff, 121 * 12, esp + 4, 0x12345678]
        if oregs != want:
            problems.append('regs %s, native %s' % ([hex(x) for x in oregs], [hex(x) for x in want]))
        if ze != zd: problems.append('zero-divide flag %d, native %d' % (ze, zd))
        if problems:
            bad += 1
            if bad <= 5: print('MISMATCH 0x75F0AD (call %d, step %d): %s' % (it, step, '; '.join(problems)))
    print('0x75F0AD: %d mismatches in %d calls (%d vertices)' % (bad, total, total * 122))
    return bad

def test_light(emu, lib, total, specials):
    uc = emu.uc
    F = ctypes.c_float
    random.seed(2)
    bad = fallback = 0
    for it in range(total):
        def rv(lo, hi): return random.choice(specials) if random.random() < 0.05 else f32(random.uniform(lo, hi))
        base = XVA + 0x2000 + random.randrange(0, 0x400) * 4
        M, N, RGB, S, OUT = [base + 0x100 * i + random.randrange(0, 0x40) for i in range(5)]
        if it % 3 == 0:  # world-map-like: normalized light matrix, colors 0-255
            m = [f32(random.uniform(-1, 1)) for _ in range(12)]
            for r in range(3): m[4 * r + 3] = f32(random.uniform(-0.2, 0.5))
            n = [f32(random.uniform(-1, 1)) for _ in range(3)]
            rgb = [f32(random.uniform(0, 300)) for _ in range(3)]
        else:
            m = [rv(-2, 2) for _ in range(12)]; n = [rv(-1, 1) for _ in range(3)]; rgb = [rv(-50, 400) for _ in range(3)]
        if it % 11 == 0:  # l == 0 exactly / t == 255 exactly
            n = [0.0, 0.0, 0.0]
            for r in range(3): m[4 * r + 3] = random.choice([0.0, -0.0, 1.0])
            rgb = [random.choice([255.0, 254.99998, 255.00002, 0.0]) for _ in range(3)]
        src = bytes(random.randrange(256) for _ in range(4))
        emu.wva(M, struct.pack('<12f', *m)); emu.wva(N, struct.pack('<3f', *n))
        emu.wva(RGB, struct.pack('<3f', *rgb)); emu.wva(S, src); emu.wva(OUT, b'\xaa' * 4)
        for i in (0x5c, 0x5e, 0x64): uc.mem_write(B + FPU + i, b'\x07')
        edx_in = random.getrandbits(32)
        emu.setreg(2, edx_in)
        top = bytes(uc.mem_read(B + FPU + 0x60, 4))
        emu.call_x86(LIGHT, [M, N, RGB, S, OUT])
        out, path, lt, eq = (ctypes.c_uint8 * 3)(), (ctypes.c_int * 3)(), ctypes.c_int(), ctypes.c_int()
        r = lib.light((F * 12)(*m), (F * 3)(*n), (F * 3)(*rgb), out, path, ctypes.byref(lt), ctypes.byref(eq))
        if not r:
            fallback += 1
            continue
        mine = bytes(out) + src[3:4]
        edx = ((M, OUT, RGB)[r - 1] & ~0xff) | src[3]
        fb = bytes(uc.mem_read(B + FPU, 0x66))
        problems = []
        if emu.rva(OUT, 4) != mine: problems.append('out %s native %s' % (emu.rva(OUT, 4).hex(), mine.hex()))
        if (emu.reg(0), emu.reg(1), emu.reg(2)) != (OUT, S, edx):
            problems.append('eax/ecx/edx %x %x %x, native %x %x %x' % (emu.reg(0), emu.reg(1), emu.reg(2), OUT, S, edx))
        if (fb[0x5c], fb[0x5e], fb[0x64]) != (lt.value, 0, eq.value):
            problems.append('c0/c2/c3 %d %d %d, native %d 0 %d' % (fb[0x5c], fb[0x5e], fb[0x64], lt.value, eq.value))
        if fb[0x60:0x64] != top: problems.append('x87 top moved')
        if problems:
            bad += 1
            if bad <= 5: print('MISMATCH 0x68DAE1 (vertex %d, paths %s): %s' % (it, list(path), '; '.join(problems)))
    print('0x68DAE1: %d mismatches in %d vertices (%d NaN cases left to the original)'
          % (bad, total - fallback, fallback))
    return bad

if __name__ == '__main__':
    main()
