#!/usr/bin/env python3
"""
Compares source/ff7_submarine_core.h (native submarine sea step 0x79F903,
bit count 0x7A2EF8, splashes 0x79A61E and surface 0x7A0190) with the recompiled functions under
emulation. The header is compiled into a freestanding ARM blob
(blob_submarine.c); both run in the same emulator from the same state and
0x79A61E's native calls the same recompiled 0x6617E9, 0x67D2BF (the view
matrix) and 0x66E272 (vertex buffer room; its common path, or 0 for a null
texture).

The sea grid (x86 pages 0xEC7-0xED7) is mapped either as one host block (the
native's fast path) or with its pages scattered (byte by byte). Compared: the
8 x86 registers, x87 bytes, all mapped x86 memory but the stack below the
call, and the _check_event() calls (the counter, and the native code calling
the real function only on every 2048th when inlining is enabled). Directed
visible scenes cover all four grid corners and the interior; random scenes
cover culling, fade, null textures and NaNs. Every fifth call disables the
page-table and event-counter shortcuts to exercise their fallback paths.

Usage: scripts/emu/test_submarine.py libjni_ff7.so ff7_en.exe [calls]
Needs arm-vita-eabi-gcc on PATH (source scripts/env.sh) and `pip install unicorn`.
"""
import math, os, random, struct, subprocess, sys, tempfile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
from ff7emu import Emu, B, FPU, MEMMAP, XVA, XH, REGS, END, HSTACK, CHECK_EVENT, EVCOUNT
from test_world_tri import pe_reader

FUNCS = {1: 0x6d5148, 2: 0x6e1364, 3: 0x6e6564, 4: 0x6d7a04, 5: 0x6d5908}
NAMES = {1: '0x79F903 (sea step)', 2: '0x7A2EF8 (bit count)', 3: '0x79A61E (splashes)', 4: '0x7A0190 (surface)',
         5: '0x79FB20 (models)'}
# 0x79FB20's set-up: 0x6625AB, 0x6627CD, 0x6626BC, 0x663673, 0x662035, 0x663707, 0x662ECC
F_MODEL = [0xc8aa28, 0xc8bc64, 0xc8b348, 0xc402a0, 0xc3bd38, 0xc40504, 0xc3d8a4]
GET_TICK_NOW = 0x951998
F_6617E9, F_67D2BF = 0xc39614, 0xc95fec
ALLOC = 0xc50ab4
MK_ADDRESS = 0x93d714
BLOB, ENV = 0x58000000, 0x59000000
GRID_PAGES = list(range(0xec7, 0xed8))
GRIDH, GRIDS = 0x54000000, 0x55000000  # the grid as one block / scattered (every other page)
TBLH = 0x53000000                      # pages 0xE73, 0xE74 (bit tables)
EXTRAH = 0x56000000                    # the other x86 pages 0x79A61E uses
EXTRA_PAGES = [0x7b6, 0x7b7, 0x7b8, 0x7b9, 0x90a, 0xdb2, 0xd8d, 0x987, 0x988, 0x989, 0xec0, 0xec1] + \
              list(range(0xeda, 0xeec)) + [0xeed, 0xe99]
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'source')
STEPS = 60000000

def build_blob():
    d = tempfile.mkdtemp()
    elf, binf = os.path.join(d, 'blob.elf'), os.path.join(d, 'blob.bin')
    subprocess.run(['arm-vita-eabi-gcc', '-mfloat-abi=softfp', '-O3', '-ffreestanding', '-nostdlib', '-nostartfiles',
                    '-static', '-Wl,-Ttext=0x%x' % BLOB, '-Wl,-e,entry', '-Wl,--gc-sections', '-I', SRC,
                    os.path.join(HERE, 'blob_submarine.c'), '-lgcc', '-o', elf], check=True)
    subprocess.run(['arm-vita-eabi-objcopy', '-O', 'binary', elf, binf], check=True)
    nm = subprocess.run(['arm-vita-eabi-nm', elf], capture_output=True, text=True, check=True).stdout
    return open(binf, 'rb').read(), [int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' entry')][0]

def main():
    so, exe = sys.argv[1], sys.argv[2]
    total = int(sys.argv[3]) if len(sys.argv) > 3 else 600
    page = pe_reader(exe)
    blob, entry = build_blob()
    emu = Emu(so)
    uc = emu.uc
    n = len(GRID_PAGES)
    uc.mem_map(GRIDH, 0x1000 * n)
    uc.mem_map(GRIDS, 0x2000 * n)
    uc.mem_map(TBLH, 0x2000)
    emu.w32(B + MEMMAP + 4 * 0xe73, TBLH); emu.w32(B + MEMMAP + 4 * 0xe74, TBLH + 0x1000)
    uc.mem_map(EXTRAH, 0x1000 * len(EXTRA_PAGES))
    for i, p in enumerate(EXTRA_PAGES):
        emu.w32(B + MEMMAP + 4 * p, EXTRAH + 0x1000 * i)
    exe_pages = b''.join(page(p) for p in EXTRA_PAGES)
    def xw(va, data):
        if va >> 12 in (0xe73, 0xe74):
            uc.mem_write(TBLH + va - 0xe73000, bytes(data))
            return
        uc.mem_write(EXTRAH + 0x1000 * EXTRA_PAGES.index(va >> 12) + (va & 0xfff), bytes(data))
    def xr32(va): return struct.unpack('<I', uc.mem_read(EXTRAH + 0x1000 * EXTRA_PAGES.index(va >> 12) + (va & 0xfff), 4))[0]
    def clz8(b): return 8 - b.bit_length()
    uc.mem_map(BLOB, 0x30000); uc.mem_write(BLOB, blob); uc.mem_map(ENV, 0x1000)
    events = [0]
    def count(uc_, addr, size, user): events[0] += 1
    uc.hook_add(UC_HOOK_CODE, count, begin=B + CHECK_EVENT, end=B + CHECK_EVENT + 1)
    # 0x66E70A's recompiled rep movsd uses this libc memcpy PLT entry.
    def copy_memory(uc_, addr, size, user):
        dst, src, length = [uc_.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)]
        uc_.mem_write(dst, bytes(uc_.mem_read(src, length)))
        uc_.reg_write(UC_ARM_REG_PC, uc_.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, copy_memory, begin=B + 0xaeed0, end=B + 0xaeed0)
    # get_tick_now (rdtsc; 0x66C56E times itself into its own stack frame): a clock that moves
    ticks = [0]
    def tick_now(uc_, addr, size, user):
        ticks[0] += 1234
        uc_.reg_write(UC_ARM_REG_R0, ticks[0]); uc_.reg_write(UC_ARM_REG_R1, 0)
        uc_.reg_write(UC_ARM_REG_PC, uc_.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, tick_now, begin=B + GET_TICK_NOW, end=B + GET_TICK_NOW)
    # libm sin / cos (imports; the rotations' recompiled C runtime calls them): the same on both sides
    def libm(fn):
        def call(uc_, addr, size, user):
            lo, hi = uc_.reg_read(UC_ARM_REG_R0), uc_.reg_read(UC_ARM_REG_R1)
            r = struct.unpack('<II', struct.pack('<d', fn(struct.unpack('<d', struct.pack('<II', lo, hi))[0])))
            uc_.reg_write(UC_ARM_REG_R0, r[0]); uc_.reg_write(UC_ARM_REG_R1, r[1])
            uc_.reg_write(UC_ARM_REG_PC, uc_.reg_read(UC_ARM_REG_LR))
        return call
    for plt, fn in ((0xaef84, math.cos), (0xaef90, math.sin)):
        uc.hook_add(UC_HOOK_CODE, libm(fn), begin=B + plt, end=B + plt)


    def map_grid(scattered):
        for i, p in enumerate(GRID_PAGES):
            emu.w32(B + MEMMAP + 4 * p, GRIDS + 0x2000 * (n - 1 - i) if scattered else GRIDH + 0x1000 * i)
    def grid_host(i): return emu.r32(B + MEMMAP + 4 * GRID_PAGES[i])
    def read_grid(): return b''.join(bytes(uc.mem_read(grid_host(i), 0x1000)) for i in range(n))
    def write_grid(data):
        for i in range(n): uc.mem_write(grid_host(i), bytes(data[0x1000 * i:0x1000 * i + 0x1000]))
    def snapshot():
        return (bytes(uc.mem_read(XH, 0x200000)), read_grid(), bytes(uc.mem_read(TBLH, 0x2000)),
                bytes(uc.mem_read(B + FPU, 0x68)), bytes(uc.mem_read(EXTRAH, 0x1000 * len(EXTRA_PAGES))))

    T1, T2, PT, P1, P2, CAMM, PROJ = (XVA + 0x20000, XVA + 0x20400, XVA + 0x20800, XVA + 0x20c00, XVA + 0x20e00,
                                      XVA + 0x21000, XVA + 0x21400)
    VB = {T1: XVA + 0x24000, T2: XVA + 0x28000}
    drawn = [0, 0]

    def setup_splash(it):
        visible = it % 3 != 0
        uc.mem_write(EXTRAH, exe_pages)
        # the bit tables (leading zeros / ones of a byte) and the square root table (bss in the exe)
        t = bytearray(0x2000)
        for k in range(256):
            t[0xf20 + 4 * k] = clz8(k)
            t[0x1358 + 4 * k] = clz8(~k & 0xff)
        uc.mem_write(TBLH, bytes(t))
        xw(0x989cd8, b''.join(struct.pack('<H', min(0xffff, int((v ** 0.5) * 512) + random.randrange(-3, 4)) & 0xffff)
                              for v in range(256)))
        # the camera: position (<< 12), the PSX matrix [0x90AAF0] + 0x10, the projection [0xDB2BB8] + 0x8D0
        cam = [random.randrange(-3000, 3000) for _ in range(3)]
        if visible: cam = [0, 0, 0]
        xw(0x987338, struct.pack('<3i', *[c << 12 | random.getrandbits(12) for c in cam]))
        rot = [4096 if i % 4 == 0 else 0 for i in range(9)]
        rot = [r + random.randrange(-600, 600) for r in rot]
        emu.wva(CAMM, struct.pack('<9hH3i', *rot, 0, *[random.randrange(-300, 300) for _ in range(3)]))
        xw(0x90aaf0, struct.pack('<I', CAMM - 0x10))
        pm = [random.uniform(-1.5, 1.5) for _ in range(16)]
        pm[3] = pm[7] = 0.0; pm[11] = random.uniform(0.5, 1.5); pm[15] = random.uniform(-300, 1500)
        if random.random() < 0.05: pm[15] = float('nan')
        if visible: pm = [1.0 if k % 5 == 0 else 0.0 for k in range(16)]
        emu.wva(PROJ, struct.pack('<16f', *pm))
        xw(0xdb2bb8, struct.pack('<I', PROJ - 0x8d0))
        # parameters
        dim = random.random() < 0.2
        xw(0x9873a8, struct.pack('<I', random.getrandbits(32) if dim else 0))
        xw(0x9873f8, struct.pack('<ii', random.randrange(100, 1200), random.randrange(-20, 1200)))
        xw(0x987400, struct.pack('<i', random.randrange(0, 60)))
        xw(0xec1820, struct.pack('<IIi', random.randrange(0, 70), random.choice([0, 0, 1, random.getrandbits(32)]),
                                 random.randrange(-50, 1500)))
        if visible:
            xw(0x9873a8, struct.pack('<I', int(it % 17 == 0)))
            xw(0x9873f8, struct.pack('<iii', 1200, 1200, 0))
            xw(0xec1820, struct.pack('<III', it % 70, 1, 600))
        count = 5 if visible else random.choice([0, 1, 2, 3, 4, 5])
        xw(0x988504, struct.pack('<I', count))
        g = bytearray(read_grid())
        for i in range(count):
            row = random.choice([63, random.randrange(64), random.randrange(64)])
            col = random.choice([63, random.randrange(64), random.randrange(64)])
            if visible:
                row, col = [(0, 0), (0, 63), (63, 0), (63, 63), (31, 31)][i]
            cell = row * 64 + col
            pos = [c + random.randrange(-450, 450) for c in cam]
            xw(0xec0880 + 8 * i, struct.pack('<3hh', *[max(-32768, min(32767, v)) for v in pos], cell))
            fl = (0x2000 if random.random() < 0.85 else 0) | (0x8000 if random.random() < 0.1 else 0) | \
                random.getrandbits(13)
            if visible: fl = 0x2000
            struct.pack_into('<H', g, 0xaa8 + 16 * cell + 0xa, fl)
        write_grid(bytes(g))
        # the column grid around the camera (floats; x, z spaced by 64)
        cols = bytearray(random.getrandbits(8) for _ in range(0x12000))
        for r in range(65):
            for c in range(64):
                o = 0x688 + 0x400 * r + 16 * c
                if o + 12 <= len(cols):
                    struct.pack_into('<3f', cols, o, cam[0] + random.uniform(-500, 500), random.uniform(-200, 1100),
                                     cam[2] + random.uniform(-500, 500))
        xw(0xeda000, cols[:0x1000])
        for k in range(1, 0x12):
            xw(0xeda000 + 0x1000 * k, cols[0x1000 * k:0x1000 * k + 0x1000])
        # the textures (0x66E272's common path) and their vertex buffers
        for t, pb in ((T1, P1), (T2, P2)):
            emu.wva(t, bytes(0x100))
            emu.wva(t + 0x30, struct.pack('<I', PT)); emu.wva(PT, struct.pack('<I', 3))
            emu.wva(t + 0x54, struct.pack('<I', 5)); emu.wva(t + 0x50, struct.pack('<i', 2000))
            emu.wva(t + 0x8c, struct.pack('<I', pb)); emu.wva(pb, bytes(0x20))
            emu.wva(t + 0x80, struct.pack('<I', VB[t])); emu.wva(t + 0x40, struct.pack('<I', 0x60))
            emu.wva(VB[t], bytes(random.getrandbits(8) for _ in range(0x3000)))
        xw(0xec0878, struct.pack('<II', T1 if random.random() < 0.95 else 0, T2 if random.random() < 0.95 else 0))

    MESH, MVERTS, MTRIS, MCOLS = XVA + 0x30000, XVA + 0x30100, XVA + 0x30400, XVA + 0x30800
    models = [0, 0]

    def setup_model(it):
        setup_splash(it)
        nv = random.randrange(1, 60)
        nt = random.choice([0, -1, random.randrange(1, 40), random.randrange(1, 40)])
        emu.wva(MESH, struct.pack('<4I', MVERTS, MTRIS, MCOLS, nt & 0xffffffff))
        spread = 400 if it % 4 < 2 else 30000 # wide: corners on both sides of the camera within one model
        emu.wva(MVERTS, b''.join(struct.pack('<4h', *[random.randrange(-spread, spread) for _ in range(3)],
                                             random.randrange(-5, 5)) for _ in range(nv)))
        emu.wva(MTRIS, b''.join(struct.pack('<4B', *[random.randrange(nv) for _ in range(3)], random.getrandbits(8))
                                for _ in range(40)))
        emu.wva(MCOLS, bytes(random.getrandbits(8) for _ in range(40 * 12)))
        xw(0xeda668, struct.pack('<I', 0 if it % 13 == 0 else T1))
        reach = 500 if spread == 400 else 0  # the position goes through a 4096 scale into the translation
        xw(0xeda670, struct.pack('<3i', *[random.randrange(-reach, reach + 1) for _ in range(3)]))
        xw(0xeda678, struct.pack('<3h', *[random.randrange(-4096, 4096) for _ in range(3)]))
        # the matrix stack's top (PSX matrix: int16 rotation, int32 translation): the camera
        rot = [r + random.randrange(-900, 900) for r in (4096, 0, 0, 0, 4096, 0, 0, 0, 4096)]
        xw(0xe996f8, struct.pack('<9hH3i', *rot, 0, *[random.randrange(-300, 300) // (1 if spread == 400 else 100)
                                                      for _ in range(3)]))
        if it % 2:  # w = z + c: corners on both sides of the camera (and a NaN now and then)
            pm = [random.uniform(-1, 1) for k in range(16)]
            pm[15] = random.uniform(-150, 150)
            if it % 15 == 1: pm[15] = float('nan')
            emu.wva(PROJ, struct.pack('<16f', *pm))

    random.seed(17)
    bad = {m: 0 for m in FUNCS}
    calls = {m: 0 for m in FUNCS}
    refused = 0
    surface_drawn = [0, 0, 0, 0]
    for it in range(total):
        mode = 4 if os.environ.get('SUBMARINE_SURFACE_ONLY') else 1 if it % 8 == 0 else 2 if it % 8 == 4 else \
            4 if it % 8 == 2 else 5 if it % 8 in (6, 7) or os.environ.get('SUBMARINE_MODEL_ONLY') else 3
        map_grid(it % 16 == 4 or it % 16 == 9)
        if mode == 1:
            g = bytearray(random.getrandbits(8) for _ in range(0x1000 * n))
            base = 0xaa8
            for i in range(0x1000):
                o = base + 16 * i
                rest = random.randrange(0, 3000)
                tmp = random.choice([rest, rest + random.randrange(-40, 40), random.randrange(0, 0x10000)]) & 0xffff
                h = random.choice([rest, tmp, (rest + random.randrange(-6, 7)) & 0xffff, random.getrandbits(16)])
                flags = random.getrandbits(16) & random.choice([0xffff, 0x9fff, 0x1fff, 0x8003])
                struct.pack_into('<HHHHHHHH', g, o, h, *[random.getrandbits(16) for _ in range(4)], flags, tmp, rest)
            write_grid(g)
            args = []
        elif mode == 5:
            setup_model(it)
            args = [MESH]
        elif mode in (3, 4):
            setup_splash(it)
            if mode == 4:
                xw(0xe7475c, struct.pack('<I', 0x40 if it % 11 == 10 else 0x20 if it % 2 else 0))
                emu.wva(PROJ - 0x8d0 + 0xa60, struct.pack('<I', int(it % 7 == 0)))
                if it % 4 == 0:
                    xw(0x987338, struct.pack('<i', random.choice([-2048, 2047, 0]) << 12))
                    xw(0x987340, struct.pack('<i', random.choice([-2048, 2047, 0]) << 12))
                # Four texture streams: smooth/flat, projected/deferred.
                for k, slot in enumerate([0xeda680, 0xeed688, 0xeed68c, 0xeed690]):
                    tex, pb, vb = XVA + 0x40000 + k * 0x200, XVA + 0x42000 + k * 0x40, XVA + 0x60000 + k * 0x30000
                    emu.wva(tex, bytes(0x100))
                    for off, val in [(0x30, PT), (0x54, 5), (0x50, 10000), (0x8c, pb), (0x80, vb), (0x40, 0x60)]:
                        emu.wva(tex + off, struct.pack('<I', val))
                    emu.wva(pb, bytes(0x20))
                    emu.wva(vb, bytes([0x9b]) * 0x30000)
                    xw(slot, struct.pack('<I', 0 if it % 9 == 0 else tex))
            args = []
        else:
            t = bytearray(random.getrandbits(8) for _ in range(0x2000))
            for k in range(256):
                for tb in (0xf20, 0x1358):
                    t[tb + 4 * k] = 8 if random.random() < 0.6 else random.randrange(9)
            uc.mem_write(TBLH, bytes(t))
            x = random.choice([random.getrandbits(32), random.getrandbits(random.randrange(1, 33)), 0, 0x80000000,
                               (-random.getrandbits(random.randrange(1, 31))) & 0xffffffff, 0x7fffffff])
            args = [x]
        fstate = bytearray(uc.mem_read(B + FPU, 0x68))
        fstate[0x60:0x64] = struct.pack('<I', random.randrange(8))
        uc.mem_write(B + FPU, bytes(fstate))
        esp0 = XVA + 0x10000
        regs0 = [random.getrandbits(32) for _ in range(8)]
        regs0[5] = 0x12345678
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        state = snapshot()

        sp_ = esp0 - 4 * len(args)
        for i, x in enumerate(args): emu.wva(sp_ + 4 * i, struct.pack('<I', x))
        emu.wva(sp_ - 4, struct.pack('<I', 0xdeadbeef))
        emu.setreg(4, sp_ - 4)
        inline = it % 5 != 0
        uc.mem_write(ENV, struct.pack('<7I4I3I4I7I3I', B + MK_ADDRESS | 1, B + CHECK_EVENT | 1, B + REGS, B + FPU,
                                      B + ALLOC | 1, B + MEMMAP if inline else 0, B + EVCOUNT if inline else 0,
                                      0, 0, 0, 0, B + F_6617E9 | 1,
                                      B + F_67D2BF | 1, B + 0xc62518 | 1, 0, 0, 0, 0,
                                      *[B + f | 1 for f in F_MODEL], 0, 0, 0))
        ev0 = random.choice([random.getrandbits(32), 0x7fe, 0x3fe, 0xfffff7f0 | random.randrange(16)])
        emu.w32(B + EVCOUNT, ev0); emu.ev_at = []
        uc.reg_write(UC_ARM_REG_R0, ENV); uc.reg_write(UC_ARM_REG_R1, mode)
        uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000); uc.reg_write(UC_ARM_REG_LR, END | 1)
        before_native = snapshot() if mode == 4 else None
        regs_before = [emu.reg(i) for i in range(8)]
        try:
            uc.emu_start(entry | 1, END, count=STEPS)
        except Exception:
            print('native fault', it, hex(uc.reg_read(UC_ARM_REG_PC)), hex(uc.reg_read(UC_ARM_REG_LR)), emu.unmapped, flush=True)
            raise
        if uc.reg_read(UC_ARM_REG_PC) != END:
            raise RuntimeError('native exceeded instruction budget at call %d' % it)
        n_s = snapshot(); n_r = [emu.reg(i) for i in range(8)]
        if mode == 4 and uc.reg_read(UC_ARM_REG_R0) == 0:
            assert n_s == before_native and n_r == regs_before
            assert emu.r32(B + EVCOUNT) == ev0
            refused += 1
            calls[mode] += 1
            continue
        if mode == 4:
            stats = struct.unpack('<4I', bytes(uc.mem_read(ENV + 56, 16)))
            surface_drawn = [a + b for a, b in zip(surface_drawn, stats)]
        n_ev = (emu.r32(B + EVCOUNT) - ev0) & 0xffffffff
        if inline and (any(v & 0x7ff for v, lr in emu.ev_at if BLOB <= lr < BLOB + 0x30000) or \
                {v for v, lr in emu.ev_at if not v & 0x7ff} != {(ev0 + k) & 0xffffffff for k in range(1, n_ev + 1)
                                                                if not (ev0 + k) & 0x7ff}):
            n_ev = -1
        calls[mode] += 1
        cnt = struct.unpack('<4I', bytes(uc.mem_read(ENV + 28, 16)))
        drawn[0] += cnt[2]; drawn[1] += cnt[3]
        mcnt = struct.unpack('<3I', bytes(uc.mem_read(ENV + 72 + 28, 12)))
        models[0] += mcnt[1]; models[1] += mcnt[2]
        if os.environ.get('SUBMARINE_DEBUG') and mode == 5:
            print('model call', it, 'drawn', mcnt[1], 'behind', mcnt[2],
                  ['%.3g' % f for f in struct.unpack('<16f', emu.rva(0x10fff4 - 0x124, 64))], flush=True)

        # the original from the same state
        uc.mem_write(XH, state[0]); write_grid(state[1]); uc.mem_write(TBLH, state[2])
        uc.mem_write(B + FPU, state[3]); uc.mem_write(EXTRAH, state[4])
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        events[0] = 0
        emu.w32(B + EVCOUNT, random.getrandbits(32))
        esp = esp0 - 4 * len(args)
        for i, x in enumerate(args): emu.wva(esp + 4 * i, struct.pack('<I', x))
        esp -= 4
        emu.wva(esp, struct.pack('<I', 0xdeadbeef)); emu.setreg(4, esp)
        uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000); uc.reg_write(UC_ARM_REG_LR, END | 1)
        uc.emu_start(B + FUNCS[mode] | 1, END, count=STEPS)
        if uc.reg_read(UC_ARM_REG_PC) != END:
            raise RuntimeError('original exceeded instruction budget at call %d' % it)
        o_s = snapshot(); o_r = [emu.reg(i) for i in range(8)]; o_ev = events[0]

        problems = []
        lo, hi = esp0 - 0x4000 - XVA, esp - XVA
        n_x, o_x = n_s[0], o_s[0]
        if n_x[:lo] != o_x[:lo] or n_x[hi:] != o_x[hi:]:
            d = [o for o in range(0, len(n_x), 4) if n_x[o:o + 4] != o_x[o:o + 4] and not lo <= o < hi]
            if os.environ.get('SUBMARINE_DEBUG'):
                print('DIFF', [(hex(XVA + o), n_x[o:o+4].hex(), o_x[o:o+4].hex()) for o in d[:40]], flush=True)
            problems.append('x86 memory: %d words differ, first at %x (native %s, original %s)'
                            % (len(d), XVA + d[0], n_x[d[0]:d[0] + 4].hex(), o_x[d[0]:d[0] + 4].hex()))
        if n_s[4] != o_s[4]:
            d = [o for o in range(0, len(n_s[4]), 4) if n_s[4][o:o + 4] != o_s[4][o:o + 4]]
            problems.append('pages: %d words differ, first at %x (native %s, original %s)'
                            % (len(d), (EXTRA_PAGES[d[0] >> 12] << 12) + (d[0] & 0xfff), n_s[4][d[0]:d[0] + 4].hex(),
                               o_s[4][d[0]:d[0] + 4].hex()))
        if n_s[1] != o_s[1]:
            d = [o for o in range(0, len(n_s[1]), 2) if n_s[1][o:o + 2] != o_s[1][o:o + 2]]
            problems.append('grid: %d halfwords differ, first at %x (native %s, original %s)'
                            % (len(d), 0xec7000 + d[0], n_s[1][d[0]:d[0] + 2].hex(), o_s[1][d[0]:d[0] + 2].hex()))
        if n_s[2] != o_s[2]: problems.append('tables changed')
        if n_r != o_r:
            problems.append('regs %s' % ', '.join('%d: %08x/%08x' % (i, n_r[i], o_r[i]) for i in range(8)
                                                  if n_r[i] != o_r[i]))
        fd = [hex(i) for i in range(0x40, 0x68) if n_s[3][i] != o_s[3][i]]
        if fd: problems.append('x87 bytes differ at %s' % fd)
        if n_ev != o_ev: problems.append('_check_event %d, original %d' % (n_ev, o_ev))
        if problems:
            bad[mode] += 1
            if sum(bad.values()) <= 10: print('MISMATCH %s (call %d): %s' % (NAMES[mode], it, '; '.join(problems)))
    for m in FUNCS:
        print('%s: %d mismatches in %d calls' % (NAMES[m], bad[m], calls[m]))
    print('native splashes: %d triangles, %d water columns drawn' % tuple(drawn))
    print('native models: %d triangles drawn, %d behind the camera' % tuple(models))
    print('surface: %d native frames, %d projected, %d deferred, %d culled quads; %d clean refusals' % (*surface_drawn, refused))
    sys.exit(1 if sum(bad.values()) else 0)

if __name__ == '__main__':
    main()
