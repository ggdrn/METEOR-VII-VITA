#!/usr/bin/env python3
"""
Compares source/ff7_coaster_core.h (native roller coaster triangle 0x5F04D7
and its helpers 0x5F2639 projection, 0x5F2759 depth, 0x5F2838 fade, 0x5F2897
color fade with _ftol) with the recompiled functions under emulation. The
header is compiled into a freestanding ARM blob (blob_coaster.c); both run in
the same emulator from the same state and call the same recompiled 0x66E272
(its common path; 0x5F04D7 writes to [t + 0x70] whatever 0x66E272 returns,
so the texture is never null).

Compared: the 8 x86 registers, x87 state (control/status bytes, TOP, and the
value a helper leaves on the x87 stack), all x86 memory but the stack below
the call, and the _check_event() calls (the counter, and the native code
calling the real function only on every 2048th). When the native triangle
declines (a vertex closer than -100 in front of the camera, or a NaN depth:
the original's clipping path) it must have changed nothing at all.

Usage: scripts/emu/test_coaster.py libjni_ff7.so ff7_en.exe [calls]
Needs arm-vita-eabi-gcc on PATH (source scripts/env.sh) and `pip install unicorn`.
"""
import math, os, random, struct, subprocess, sys, tempfile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR
from ff7emu import Emu, B, FPU, MEMMAP, XVA, XH, REGS, END, HSTACK, CHECK_EVENT, EVCOUNT
from test_world_tri import pe_reader

FUNCS = {1: 0x1a6af0, 2: 0x1a28c0, 3: 0x1a37f0, 4: 0x1a3e10, 5: 0x1a6864}
NAMES = {1: '0x5F04D7 (triangle)', 2: '0x5F2639 (projection)', 3: '0x5F2759 (depth)', 4: '0x5F2838 (fade)',
         5: '0x5F2897 (color)'}
ALLOC = 0xc50ab4
MK_ADDRESS = 0x93d714
BLOB, ENV = 0x58000000, 0x59000000
GPAGE = 0x53000000  # x86 page 0xC3F000 (the fade range)
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'source')

def build_blob():
    d = tempfile.mkdtemp()
    elf, binf = os.path.join(d, 'blob.elf'), os.path.join(d, 'blob.bin')
    subprocess.run(['arm-vita-eabi-gcc', '-mfloat-abi=softfp', '-O3', '-ffreestanding', '-nostdlib', '-nostartfiles',
                    '-static', '-Wl,-Ttext=0x%x' % BLOB, '-Wl,-e,entry', '-Wl,--gc-sections', '-I', SRC,
                    os.path.join(HERE, 'blob_coaster.c'), '-lgcc', '-o', elf], check=True)
    subprocess.run(['arm-vita-eabi-objcopy', '-O', 'binary', elf, binf], check=True)
    nm = subprocess.run(['arm-vita-eabi-nm', elf], capture_output=True, text=True, check=True).stdout
    return open(binf, 'rb').read(), [int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' entry')][0]

def f32(x):
    try: return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError: return math.copysign(math.inf, x)

def main():
    so, exe = sys.argv[1], sys.argv[2]
    total = int(sys.argv[3]) if len(sys.argv) > 3 else 5000
    blob, entry = build_blob()
    page = pe_reader(exe)
    emu = Emu(so)
    uc = emu.uc
    pages = list(range(0x7b6, 0x7ba))
    HOSTM = 0x52000000
    uc.mem_map(HOSTM, 0x1000 * len(pages))
    for i, p in enumerate(pages):
        emu.w32(B + MEMMAP + 4 * p, HOSTM + 0x1000 * i)
        uc.mem_write(HOSTM + 0x1000 * i, page(p))
    uc.mem_map(GPAGE, 0x1000)
    emu.w32(B + MEMMAP + 4 * 0xc3f, GPAGE)
    uc.mem_map(BLOB, 0x30000); uc.mem_write(BLOB, blob); uc.mem_map(ENV, 0x1000)
    events = [0]
    def count(uc_, addr, size, user): events[0] += 1
    uc.hook_add(UC_HOOK_CODE, count, begin=B + CHECK_EVENT, end=B + CHECK_EVENT + 1)
    MD, MP, TRI, OUT, COL, SELF, T1, T2, P, PT, VB = (XVA + 0x20000, XVA + 0x20100, XVA + 0x21000, XVA + 0x21400,
                                                      XVA + 0x21800, XVA + 0x21c00, XVA + 0x22000, XVA + 0x22400,
                                                      XVA + 0x22800, XVA + 0x22c00, XVA + 0x24000)
    random.seed(13)
    specials = [0.0, -0.0, 1.0, -1.0, 1e-30, 1e30, math.inf, -math.inf, math.nan]
    bad = {m: 0 for m in FUNCS}
    calls = {m: 0 for m in FUNCS}
    declined = 0
    drawn = [0, 0]
    for it in range(total):
        mode = random.choice([1, 1, 1, 2, 3, 4, 5])
        sp = it % 9 == 8
        def rf(lo, hi):
            return f32(random.choice(specials)) if sp and random.random() < 0.1 else f32(random.uniform(lo, hi))
        # the fade range A..B (B - A may be 0 or negative)
        a = random.randrange(-3000, 3000)
        b = a + random.choice([0, -50, random.randrange(1, 4000)])
        uc.mem_write(GPAGE + 0x750, struct.pack('<ii', a, b))
        # matrices: depth (m[2], m[6], m[10], m[14]) spread around -100 .. 0; projection with w mostly > 0
        md = [rf(-1, 1) for _ in range(16)]
        md[14] = rf(-400, 300)
        emu.wva(MD, struct.pack('<16f', *md))
        mp = [rf(-2, 2) for _ in range(16)]
        if random.random() < 0.9: mp[15] = f32(random.uniform(1, 4000))
        if random.random() < 0.05: mp[3] = mp[7] = mp[11] = mp[15] = 0.0  # w == 0
        emu.wva(MP, struct.pack('<16f', *mp))
        emu.wva(TRI, b''.join(struct.pack('<3hH', *[random.randrange(-400, 400) for _ in range(3)],
                                          random.getrandbits(16)) for _ in range(3))
                + bytes(random.getrandbits(8) for _ in range(12)))
        emu.wva(SELF, bytes(0x80) + struct.pack('<II', MD, MP))
        for t in (T1, T2):
            emu.wva(t, bytes(0x100))
            emu.wva(t + 0x30, struct.pack('<I', PT)); emu.wva(PT, struct.pack('<I', 3))
            emu.wva(t + 0x54, struct.pack('<I', 5)); emu.wva(t + 0x50, struct.pack('<i', 100))
            emu.wva(t + 0x8c, struct.pack('<I', P))
            emu.wva(t + 0x80, struct.pack('<I', VB + 0x20 * random.randrange(8)))
            emu.wva(t + 0x40, struct.pack('<I', 0x60))
        emu.wva(VB, bytes(random.getrandbits(8) for _ in range(0x400)))
        if mode == 1:
            args = [TRI, T1, random.getrandbits(32), T2, random.getrandbits(32),
                    random.getrandbits(32), random.getrandbits(32)]
        elif mode == 2:
            args = [TRI + 8 * random.randrange(3), MP, OUT + 4 * random.randrange(4)]
        elif mode == 3:
            args = [TRI + 8 * random.randrange(3), MD]
        elif mode == 4:
            d = random.choice([rf(-500, 500), float(a), float(b), f32(a + 0.5)])
            args = [struct.unpack('<I', struct.pack('<f', d))[0]]
        else:
            args = [TRI + 0x18 + 4 * random.randrange(3), struct.unpack('<I', struct.pack('<f', rf(-0.5, 1.5)))[0]]
        fstate = bytearray(uc.mem_read(B + FPU, 0x68))
        for i in (0x5c, 0x5e, 0x64): fstate[i] = random.randrange(2)
        fstate[0x56] = 0
        fstate[0x60:0x64] = struct.pack('<I', random.randrange(8))
        uc.mem_write(B + FPU, bytes(fstate))
        esp0 = XVA + 0x10000
        regs0 = [random.getrandbits(32) for _ in range(8)]
        regs0[5] = 0x12345678
        regs0[1] = SELF  # this
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        state_x = bytes(uc.mem_read(XH, 0x200000)); state_f = bytes(uc.mem_read(B + FPU, 0x68))
        state_g = bytes(uc.mem_read(GPAGE, 0x1000))

        # native first: when it declines, nothing may have changed
        sp_ = esp0 - 4 * len(args)
        for i, x in enumerate(args): emu.wva(sp_ + 4 * i, struct.pack('<I', x))
        emu.wva(sp_ - 4, struct.pack('<I', 0xdeadbeef))
        emu.setreg(4, sp_ - 4)
        uc.mem_write(ENV, struct.pack('<7I4I', B + MK_ADDRESS | 1, B + CHECK_EVENT | 1, B + REGS, B + FPU,
                                      B + ALLOC | 1, B + MEMMAP, B + EVCOUNT, 0, 0, 0, 0))
        ev0 = random.choice([random.getrandbits(32), 0x7fe, 0x3fe, 0xfffff7f0 | random.randrange(16)])
        emu.w32(B + EVCOUNT, ev0); emu.ev_at = []
        start_x = bytes(uc.mem_read(XH, 0x200000)); start_r = [emu.reg(i) for i in range(8)]
        uc.reg_write(UC_ARM_REG_R0, ENV); uc.reg_write(UC_ARM_REG_R1, mode)
        uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000); uc.reg_write(UC_ARM_REG_LR, END | 1)
        uc.emu_start(entry | 1, END, count=2000000)
        ret = uc.reg_read(UC_ARM_REG_R0)
        n_x = bytes(uc.mem_read(XH, 0x200000)); n_f = bytes(uc.mem_read(B + FPU, 0x68))
        n_r = [emu.reg(i) for i in range(8)]
        n_ev = (emu.r32(B + EVCOUNT) - ev0) & 0xffffffff
        if any(v & 0x7ff for v, lr in emu.ev_at if BLOB <= lr < BLOB + 0x10000) or \
                {v for v, lr in emu.ev_at if not v & 0x7ff} != {(ev0 + k) & 0xffffffff for k in range(1, n_ev + 1)
                                                                if not (ev0 + k) & 0x7ff}:
            n_ev = -1
        calls[mode] += 1
        problems = []
        cnt = struct.unpack('<4I', bytes(uc.mem_read(ENV + 28, 16)))
        drawn[0] += cnt[0]; drawn[1] += cnt[1]
        if ret == 0:
            declined += 1
            if mode != 1: problems.append('a helper declined')
            if n_x != start_x or n_f != state_f or n_r != start_r or n_ev != 0:
                problems.append('declined but changed the state')
        else:
            # the original from the same state
            uc.mem_write(XH, state_x); uc.mem_write(B + FPU, state_f); uc.mem_write(GPAGE, state_g)
            for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
            events[0] = 0
            ev_o = random.getrandbits(32); emu.w32(B + EVCOUNT, ev_o)
            esp = emu.call_x86(FUNCS[mode], args, esp=esp0)
            o_x = bytes(uc.mem_read(XH, 0x200000)); o_f = bytes(uc.mem_read(B + FPU, 0x68))
            o_r = [emu.reg(i) for i in range(8)]; o_ev = events[0]
            lo, hi = esp0 - 0x1000 - XVA, esp - XVA
            if n_x[:lo] != o_x[:lo] or n_x[hi:] != o_x[hi:]:
                diffs = [o for o in range(0, len(n_x), 4) if n_x[o:o + 4] != o_x[o:o + 4] and not lo <= o < hi]
                problems.append('memory: %d words differ, first at %x (native %s, original %s)'
                                % (len(diffs), XVA + diffs[0], n_x[diffs[0]:diffs[0] + 4].hex(),
                                   o_x[diffs[0]:diffs[0] + 4].hex()))
            if n_r != o_r:
                problems.append('regs %s' % ', '.join('%d: %08x/%08x' % (i, n_r[i], o_r[i]) for i in range(8)
                                                      if n_r[i] != o_r[i]))
            fd = [hex(i) for i in range(0x40, 0x68) if n_f[i] != o_f[i]]
            if fd: problems.append('x87 bytes differ at %s' % fd)
            top = struct.unpack_from('<I', o_f, 0x60)[0] & 7
            if mode in (3, 4) and n_f[8 * top:8 * top + 8] != o_f[8 * top:8 * top + 8]:
                problems.append('st(0) %s / %s' % (n_f[8 * top:8 * top + 8].hex(), o_f[8 * top:8 * top + 8].hex()))
            if n_ev != o_ev: problems.append('_check_event %d, original %d' % (n_ev, o_ev))
        if problems:
            bad[mode] += 1
            if sum(bad.values()) <= 10: print('MISMATCH %s (call %d): %s' % (NAMES[mode], it, '; '.join(problems)))
    for m in FUNCS:
        print('%s: %d mismatches in %d calls%s' % (NAMES[m], bad[m], calls[m],
                                                   ' (%d declined: clipping path / NaN)' % declined if m == 1 else ''))
    print('native triangles: %d drawn, %d not in front' % tuple(drawn))
    sys.exit(1 if sum(bad.values()) else 0)

if __name__ == '__main__':
    main()
