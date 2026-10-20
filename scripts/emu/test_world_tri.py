#!/usr/bin/env python3
"""
Compares source/ff7_world_tri.h (native 0x75F68C, world map triangles) with
the recompiled function under emulation. The header is compiled into a
freestanding ARM blob (blob_world_tri.c, arm-vita-eabi-gcc) and both run in
the same emulator from the same state, calling the same recompiled 0x68DAE1
and 0x66E272. Compared: all x86 memory but the stack below the call, EAX /
ECX / EDX / ESP / EBP / ESI, x87 C0 / C2 / C3, zero-divide flag and stack top,
and the number of _check_event() calls.

Usage: scripts/emu/test_world_tri.py libjni_ff7.so ff7_en.exe [meshes]
Needs arm-vita-eabi-gcc on PATH (source scripts/env.sh) and `pip install unicorn`.
"""
import math, os, random, struct, subprocess, sys, tempfile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP, UC_ARM_REG_LR
from ff7emu import Emu, B, FPU, MEMMAP, XVA, XH, REGS, END, HSTACK, CHECK_EVENT

FUNC = 0x63d3bc       # _Z15func_0x0075f68cv
MK_ADDRESS = 0x93d714
LIGHT = 0xccc8a8      # _Z15func_0x0068dae1v
ALLOC = 0xc50ab4      # _Z15func_0x0066e272v
BLOB = 0x58000000
ENV = 0x59000000
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'source')

def build_blob():
    d = tempfile.mkdtemp()
    elf, binf = os.path.join(d, 'blob.elf'), os.path.join(d, 'blob.bin')
    subprocess.run(['arm-vita-eabi-gcc', '-mfloat-abi=softfp', '-O3', '-ffreestanding', '-nostdlib', '-nostartfiles',
                    '-static', '-Wl,-Ttext=0x%x' % BLOB, '-Wl,-e,entry', '-Wl,--gc-sections', '-I', SRC,
                    os.path.join(HERE, 'blob_world_tri.c'), '-lgcc', '-o', elf], check=True)
    subprocess.run(['arm-vita-eabi-objcopy', '-O', 'binary', elf, binf], check=True)
    nm = subprocess.run(['arm-vita-eabi-nm', elf], capture_output=True, text=True, check=True).stdout
    entry = [int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' entry')][0]
    return open(binf, 'rb').read(), entry

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

def f32(x):
    try: return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError: return math.copysign(math.inf, x)

def main():
    so, exe = sys.argv[1], sys.argv[2]
    total = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    blob, entry = build_blob()
    page = pe_reader(exe)
    emu = Emu(so)
    uc = emu.uc
    pages = list(range(0x7b6, 0x7ba)) + [0x969, 0xde6] + list(range(0xe20, 0xe40))
    HOSTM = 0x52000000
    uc.mem_map(HOSTM, 0x1000 * len(pages))
    for i, p in enumerate(pages):
        emu.w32(B + MEMMAP + 4 * p, HOSTM + 0x1000 * i)
        uc.mem_write(HOSTM + 0x1000 * i, page(p))
    uc.mem_map(BLOB, 0x10000)
    uc.mem_write(BLOB, blob)
    uc.mem_map(ENV, 0x1000)
    events = [0]
    def count(uc_, addr, size, user): events[0] += 1
    uc.hook_add(UC_HOOK_CODE, count, begin=B + CHECK_EVENT, end=B + CHECK_EVENT + 1)
    def host(va): return HOSTM + 0x1000 * pages.index(va >> 12) + (va & 0xfff)
    def w(va, data): uc.mem_write(host(va), bytes(data))
    def w32(va, v): w(va, struct.pack('<I', v & 0xffffffff))

    TRI, NORM, TEX, VB, UVB, CAM, PT = (XVA + 0x20000, XVA + 0x30000, XVA + 0x40000, XVA + 0x50000,
                                        XVA + 0x60000, XVA + 0x61000, XVA + 0x62000)
    random.seed(4)
    specials = [0.0, -0.0, 0.5, -0.5, 1.0, -1.0, 0.99999994, -0.99999994, 1e30, math.inf, -math.inf, math.nan]
    bad = 0
    stats = [0, 0, 0, 0]
    for it in range(total):
        sp = it % 5 == 4
        def rf(lo, hi):
            return f32(random.choice(specials)) if sp and random.random() < 0.05 else f32(random.uniform(lo, hi))
        ntri = random.randrange(1, 60)
        nv = random.choice([40, 123, 256])
        # projected vertices, transformed vertices, clip flags
        for i in range(256):
            w(0xe38938 + 12 * i, struct.pack('<3f', rf(-300, 1000), rf(-300, 800), rf(-500, 9000)))
            ww = rf(0.5, 5000) if random.random() > 0.08 else random.choice([0.0, -3.0, 0.75, 1.0])
            w(0xe38f38 + 16 * i, struct.pack('<4f', rf(-1e4, 1e4), rf(-1e4, 1e4), rf(-1e4, 1e4), ww))
            w32(0xe397b8 + 4 * i, random.choice([0, 1, 0x100] + [0x101] * 6 + [random.getrandbits(32)]))
        # 12 bytes: i0 i1 i2 pad u0 v0 u1 v1, then a word: u2 v2 in its low bytes, texture in bits 16-24
        buf = b''
        for t in range(ntri):
            idx = [random.randrange(nv) for _ in range(3)]
            word = (random.randrange(4) << 16) | random.getrandbits(16) | (random.getrandbits(7) << 25)
            buf += bytes(idx) + b'\0' + bytes(random.getrandbits(8) for _ in range(4)) + struct.pack('<I', word)
        emu.wva(TRI, buf)
        emu.wva(NORM, bytes(random.getrandbits(8) for _ in range(8 * 256)))
        # globals
        w32(0xde68f8, random.choice([640, 320, 641]))
        w32(0xe2c424, random.randrange(-20, 20))
        w32(0xe36110, random.choice([0, TRI + 12 * random.randrange(ntri) + random.randrange(-13, 14)]))
        w32(0xe37d28, random.choice([0, 3000, 100000, -100000]))
        w32(0xe37520, 0)
        w32(0xe37524, random.choice([0, 0, 200, 2000, 6000]))
        w(0xe2b758, bytes(random.getrandbits(8) for _ in range(0x800)))
        w(0xe28f40 - 0x80, bytes(random.getrandbits(8) for _ in range(0x1100)))
        w32(0x96994c, random.choice([1, 1, 1, 0]))
        w32(0xe2d168, UVB)
        emu.wva(UVB, bytes(random.getrandbits(8) for _ in range(0x1000)))
        w32(0xe360f4, CAM)
        emu.wva(CAM + 0xf8, struct.pack('<12f', *[f32(random.uniform(-1, 1)) for _ in range(12)]))
        for tex in range(4):
            t = TEX + 0x100 * tex
            w32(0xe2bbd8 + 4 * tex, 0 if (it % 7 == 6 and tex == 3) else t)
            fields = {0x24: f32(random.uniform(0, 0.02)), 0x28: f32(random.uniform(0, 0.02))}
            emu.wva(t, bytes(0x100))
            emu.wva(t + 0x24, struct.pack('<ff', fields[0x24], fields[0x28]))
            emu.wva(t + 0x30, struct.pack('<I', PT))
            emu.wva(t + 0x54, struct.pack('<I', 5))
            # room in the buffer: 0x66E272's slow path allocates (imports), which the emulator cannot run
            emu.wva(t + 0x50, struct.pack('<i', 1000))
            emu.wva(t + 0x8c, struct.pack('<I', PT + 0x100 + 0x40 * tex))
            emu.wva(t + 0x80, struct.pack('<I', VB + 0x4000 * tex))
            emu.wva(t + 0x84, struct.pack('<I', random.getrandbits(32)))
            emu.wva(t + 0x88, struct.pack('<I', random.getrandbits(16)))
            emu.wva(t + 0x40, struct.pack('<I', 0x60))
            emu.wva(t + 0x44, struct.pack('<I', random.getrandbits(8)))
            emu.wva(t + 0x38, struct.pack('<I', random.getrandbits(8)))
            emu.wva(t + 0x3c, struct.pack('<I', random.getrandbits(8)))
        emu.wva(PT, struct.pack('<I', 3))
        emu.wva(VB, bytes(0x10000))
        uc.mem_write(B + FPU + 0x56, b'\x00')
        for i in (0x5c, 0x5e, 0x64): uc.mem_write(B + FPU + i, b'\x07')

        args = [TRI, TRI + 12 * ntri, NORM, 0x1234000]
        esp0 = XVA + 0x10000
        regs0 = [0xa0000000 + i for i in range(8)]
        regs0[5] = 0x12345678
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        state_x = bytes(uc.mem_read(XH, 0x200000))
        state_h = bytes(uc.mem_read(HOSTM, 0x1000 * len(pages)))
        state_f = bytes(uc.mem_read(B + FPU, 0x68))

        events[0] = 0
        esp = emu.call_x86(FUNC, args, esp=esp0)
        o_x = bytes(uc.mem_read(XH, 0x200000))
        o_h = bytes(uc.mem_read(HOSTM, 0x1000 * len(pages)))
        o_f = bytes(uc.mem_read(B + FPU, 0x68))
        o_r = [emu.reg(i) for i in range(8)]
        o_ev = events[0]

        # Native, from the same state
        uc.mem_write(XH, state_x)
        uc.mem_write(HOSTM, state_h)
        uc.mem_write(B + FPU, state_f)
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        sp_ = esp0 - 4 * len(args)
        for i, a in enumerate(args): emu.wva(sp_ + 4 * i, struct.pack('<I', a))
        emu.wva(sp_ - 4, struct.pack('<I', 0xdeadbeef))
        emu.setreg(4, sp_ - 4)
        pages_host = host(0xe38000) if host(0xe39000) == host(0xe38000) + 0x1000 else 0
        env = struct.pack('<7I4I', B + MK_ADDRESS | 1, B + CHECK_EVENT | 1, B + REGS, B + FPU, pages_host,
                          B + LIGHT | 1, B + ALLOC | 1, 0, 0, 0, 0)
        uc.mem_write(ENV, env)
        events[0] = 0
        uc.reg_write(UC_ARM_REG_R0, ENV)
        uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000)
        uc.reg_write(UC_ARM_REG_LR, END | 1)
        uc.emu_start(entry | 1, END, count=5000000)
        ret = uc.reg_read(UC_ARM_REG_R0)
        n_x = bytes(uc.mem_read(XH, 0x200000))
        n_h = bytes(uc.mem_read(HOSTM, 0x1000 * len(pages)))
        n_f = bytes(uc.mem_read(B + FPU, 0x68))
        n_r = [emu.reg(i) for i in range(8)]
        n_ev = events[0]
        cnt = struct.unpack('<4I', bytes(uc.mem_read(ENV + 28, 16)))
        for i in range(4): stats[i] += cnt[i]

        problems = []
        if ret != 1: problems.append('native returned %d' % ret)
        lo = esp0 - 0x1000 - XVA  # stack below the call: the functions' locals
        hi = esp - XVA
        def cmp(a, b, base, name, skip=None):
            diffs = [o for o in range(0, len(a), 4)
                     if a[o:o + 4] != b[o:o + 4] and not (skip and skip[0] <= o < skip[1])]
            if diffs:
                problems.append('%s: %d words differ, first at %x (native %s, original %s)'
                                % (name, len(diffs), base + diffs[0], a[diffs[0]:diffs[0] + 4].hex(),
                                   b[diffs[0]:diffs[0] + 4].hex()))
        cmp(n_x, o_x, XVA, 'x86 memory', (lo, hi))
        cmp(n_h, o_h, 0, 'globals (host pages)')
        if n_r != o_r: problems.append('regs native %s original %s' % ([hex(x) for x in n_r], [hex(x) for x in o_r]))
        for off, name in ((0x5c, 'C0'), (0x5e, 'C2'), (0x64, 'C3'), (0x56, 'ZE')):
            if n_f[off] != o_f[off]: problems.append('%s %d, original %d' % (name, n_f[off], o_f[off]))
        if n_f[0x60:0x64] != o_f[0x60:0x64]: problems.append('x87 top')
        if n_ev != o_ev: problems.append('_check_event %d calls, original %d' % (n_ev, o_ev))
        if problems:
            bad += 1
            if bad <= 6: print('MISMATCH (mesh %d, %d triangles): %s' % (it, ntri, '; '.join(problems)))
    print('0x75F68C: %d mismatches in %d meshes (%d triangles, %d drawn, %d allocation slow paths, %d NaN compares)'
          % (bad, total, stats[0], stats[1], stats[2], stats[3]))
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
