#!/usr/bin/env python3
"""
Compares source/ff7_highway_core.h (native 0x651A6D, highway vertex color)
with the recompiled function under emulation. The header is compiled into a
freestanding ARM blob (blob_highway.c, arm-vita-eabi-gcc) and both run in the
same emulator from the same state; the original calls the recompiled
0x651A2B, 0x6519D3 and _ftol (0x40B470). Compared: the 8 x86 registers, the
x87 control/status state and stack top (not the popped slots), all x86
memory but the stack below the call, and the _check_event() calls.

Usage: scripts/emu/test_highway.py libjni_ff7.so ff7_en.exe [vertices]
Needs arm-vita-eabi-gcc on PATH (source scripts/env.sh) and `pip install unicorn`.
"""
import math, os, random, struct, subprocess, sys, tempfile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP, UC_ARM_REG_LR
from ff7emu import Emu, B, FPU, MEMMAP, XVA, XH, REGS, END, HSTACK, CHECK_EVENT
from test_world_tri import pe_reader

FUNC = 0x2b4374       # _Z15func_0x00651a6dv
MK_ADDRESS = 0x93d714
BLOB = 0x58000000
ENV = 0x59000000
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'source')

def build_blob():
    d = tempfile.mkdtemp()
    elf, binf = os.path.join(d, 'blob.elf'), os.path.join(d, 'blob.bin')
    subprocess.run(['arm-vita-eabi-gcc', '-mfloat-abi=softfp', '-O3', '-ffreestanding', '-nostdlib', '-nostartfiles',
                    '-static', '-Wl,-Ttext=0x%x' % BLOB, '-Wl,-e,entry', '-Wl,--gc-sections', '-I', SRC,
                    os.path.join(HERE, 'blob_highway.c'), '-lgcc', '-o', elf], check=True)
    subprocess.run(['arm-vita-eabi-objcopy', '-O', 'binary', elf, binf], check=True)
    nm = subprocess.run(['arm-vita-eabi-nm', elf], capture_output=True, text=True, check=True).stdout
    entry = [int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' entry')][0]
    return open(binf, 'rb').read(), entry

def f32(x):
    try: return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError: return math.copysign(math.inf, x)

def main():
    so, exe = sys.argv[1], sys.argv[2]
    total = int(sys.argv[3]) if len(sys.argv) > 3 else 20000
    blob, entry = build_blob()
    page = pe_reader(exe)
    emu = Emu(so)
    uc = emu.uc
    pages = list(range(0x7b6, 0x7ba)) + [0xd85]
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
    k = struct.unpack('<3f', bytes(uc.mem_read(host(0x7b79a4), 12)))
    print('constants [0x7b79a4..ac]:', k)

    MAT, VTX, SRCC, OUT = XVA + 0x20000, XVA + 0x21000, XVA + 0x22000, XVA + 0x23000
    random.seed(5)
    specials = [0.0, -0.0, 1.0, -1.0, 255.0, 1e30, -1e30, math.inf, -math.inf, math.nan]
    bad = 0
    nan_total = 0
    for it in range(total):
        sp = it % 10 == 9
        def rf(lo, hi):
            return f32(random.choice(specials)) if sp and random.random() < 0.2 else f32(random.uniform(lo, hi))
        near = random.choice([random.randrange(-100, 3000), 0, 1000])
        far = random.choice([near + random.randrange(1, 20000), near, near - 5, near + 1])
        w32(0xd85958, near)
        w32(0xd858fc, far)
        w32(0xd858e0, XVA + 0x1f000)
        emu.wva(XVA + 0x1f000 + 0x14, struct.pack('<I', MAT))
        m = [rf(-2, 2) for _ in range(16)]
        if it % 3 == 0: m[14] = rf(-500, 25000)
        vv = [rf(-5000, 5000) for _ in range(3)]
        if it % 7 == 0:  # z exactly at near / far
            m = [0.0] * 16; m[14] = float(random.choice([near, far]))
        v_va = VTX + 4 * random.randrange(0, 64)
        s_va = SRCC + random.randrange(0, 64)
        o_va = OUT + random.randrange(0, 64)
        emu.wva(MAT, struct.pack('<16f', *m))
        emu.wva(v_va, struct.pack('<3f', *vv))
        # 255 * 1.0 (z before near) saturates: the 0xFF path
        emu.wva(s_va, bytes(random.choice([255, 255, random.getrandbits(8)]) for _ in range(4)))
        if it % 4 == 1:
            m = [0.0] * 16; m[14] = float(near - random.randrange(0, 50))
            emu.wva(MAT, struct.pack('<16f', *m))
        emu.wva(o_va, b'\xaa' * 4)
        fstate = bytearray(uc.mem_read(B + FPU, 0x68))
        for i in (0x5c, 0x5e, 0x64): fstate[i] = random.randrange(2)
        fstate[0x56] = 0
        fstate[0x60:0x64] = struct.pack('<I', random.randrange(8))
        if it % 5 == 0:  # control word fields out of range: _ftol normalizes them
            for i in range(0x40, 0x46): fstate[i] = random.getrandbits(8)
            fstate[0x48:0x4c] = struct.pack('<I', random.getrandbits(32))
            fstate[0x4c:0x50] = struct.pack('<I', random.getrandbits(32))
            fstate[0x50] = random.getrandbits(8)
        uc.mem_write(B + FPU, bytes(fstate))
        args = [s_va, v_va, o_va]
        esp0 = XVA + 0x10000
        regs0 = [random.getrandbits(32) for _ in range(8)]
        regs0[5] = 0x12345678
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        state_x = bytes(uc.mem_read(XH, 0x200000))
        state_h = bytes(uc.mem_read(HOSTM, 0x1000 * len(pages)))
        state_f = bytes(uc.mem_read(B + FPU, 0x68))

        events[0] = 0
        esp = emu.call_x86(FUNC, args, esp=esp0)
        o_x = bytes(uc.mem_read(XH, 0x200000)); o_f = bytes(uc.mem_read(B + FPU, 0x68))
        o_r = [emu.reg(i) for i in range(8)]; o_ev = events[0]

        uc.mem_write(XH, state_x); uc.mem_write(HOSTM, state_h); uc.mem_write(B + FPU, state_f)
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        sp_ = esp0 - 4 * len(args)
        for i, a in enumerate(args): emu.wva(sp_ + 4 * i, struct.pack('<I', a))
        emu.wva(sp_ - 4, struct.pack('<I', 0xdeadbeef))
        emu.setreg(4, sp_ - 4)
        uc.mem_write(ENV, struct.pack('<4I2I', B + MK_ADDRESS | 1, B + CHECK_EVENT | 1, B + REGS, B + FPU, 0, 0))
        events[0] = 0
        uc.reg_write(UC_ARM_REG_R0, ENV)
        uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000)
        uc.reg_write(UC_ARM_REG_LR, END | 1)
        uc.emu_start(entry | 1, END, count=1000000)
        ret = uc.reg_read(UC_ARM_REG_R0)
        n_x = bytes(uc.mem_read(XH, 0x200000)); n_f = bytes(uc.mem_read(B + FPU, 0x68))
        n_r = [emu.reg(i) for i in range(8)]; n_ev = events[0]
        nan_total += struct.unpack('<I', bytes(uc.mem_read(ENV + 20, 4)))[0]

        problems = []
        if ret != 1: problems.append('native returned %d' % ret)
        lo, hi = esp0 - 0x1000 - XVA, esp - XVA
        if n_x[:lo] != o_x[:lo] or n_x[hi:] != o_x[hi:]:  # word by word only on a difference
            diffs = [o for o in range(0, len(n_x), 4) if n_x[o:o + 4] != o_x[o:o + 4] and not lo <= o < hi]
            problems.append('memory: %d words differ, first at %x' % (len(diffs), XVA + diffs[0]))
        if n_r != o_r:
            problems.append('regs %s' % ', '.join('%d: %08x/%08x' % (i, n_r[i], o_r[i]) for i in range(8) if n_r[i] != o_r[i]))
        fd = [hex(i) for i in range(0x40, 0x68) if n_f[i] != o_f[i]]
        if fd: problems.append('x87 bytes differ at %s' % fd)
        if n_ev != o_ev: problems.append('_check_event %d, original %d' % (n_ev, o_ev))
        if problems:
            bad += 1
            if bad <= 8: print('MISMATCH (vertex %d, near %d far %d): %s' % (it, near, far, '; '.join(problems)))
    print('0x651A6D: %d mismatches in %d vertices (%d NaN compares)' % (bad, total, nan_total))
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
