#!/usr/bin/env python3
"""
Compares source/ff7_engine_core.h with the recompiled 0x661465 (PSX matrix
to float), 0x6611DA (fixed point division, result on the x87 stack),
0x661098 (int16 vector to float) and the common path of 0x66E272 (vertex
buffer room) under emulation. For 0x66E272 cases that need the original's
slow path (it allocates through imports the emulator cannot run), the test
checks that the native code refuses them (-1) without touching anything. The header is compiled
into a freestanding ARM blob (blob_engine.c, arm-vita-eabi-gcc); both run in
the same emulator from the same state. Compared: the 8 x86 registers, the
x87 state (control/status bytes, TOP and the slot 0x6611DA pushes), all x86
memory but the stack below the call, and the _check_event() calls.

Usage: scripts/emu/test_engine.py libjni_ff7.so ff7_en.exe [calls]
Needs arm-vita-eabi-gcc on PATH (source scripts/env.sh) and `pip install unicorn`.
"""
import math, os, random, struct, subprocess, sys, tempfile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR
from ff7emu import Emu, B, FPU, MEMMAP, XVA, XH, REGS, END, HSTACK, CHECK_EVENT, EVCOUNT
from test_world_tri import pe_reader

FUNCS = [(0xc37e60, 2, '0x661465'), (0xc36bd8, 1, '0x6611DA'), (0xc36154, 2, '0x661098'), (0xc50ab4, 2, '0x66E272')]
MK_ADDRESS = 0x93d714
BLOB, ENV = 0x58000000, 0x59000000
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'source')

def build_blob():
    d = tempfile.mkdtemp()
    elf, binf = os.path.join(d, 'blob.elf'), os.path.join(d, 'blob.bin')
    subprocess.run(['arm-vita-eabi-gcc', '-mfloat-abi=softfp', '-O3', '-ffreestanding', '-nostdlib', '-nostartfiles',
                    '-static', '-Wl,-Ttext=0x%x' % BLOB, '-Wl,-e,entry', '-Wl,--gc-sections', '-I', SRC,
                    os.path.join(HERE, 'blob_engine.c'), '-lgcc', '-o', elf], check=True)
    subprocess.run(['arm-vita-eabi-objcopy', '-O', 'binary', elf, binf], check=True)
    nm = subprocess.run(['arm-vita-eabi-nm', elf], capture_output=True, text=True, check=True).stdout
    return open(binf, 'rb').read(), [int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' entry')][0]

def main():
    so, exe = sys.argv[1], sys.argv[2]
    total = int(sys.argv[3]) if len(sys.argv) > 3 else 3000
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
    def host(va): return HOSTM + 0x1000 * pages.index(va >> 12) + (va & 0xfff)
    k0 = bytes(uc.mem_read(host(0x7b7a50), 8))
    uc.mem_map(BLOB, 0x10000); uc.mem_write(BLOB, blob); uc.mem_map(ENV, 0x1000)
    events = [0]
    def count(uc_, addr, size, user): events[0] += 1
    uc.hook_add(UC_HOOK_CODE, count, begin=B + CHECK_EVENT, end=B + CHECK_EVENT + 1)
    random.seed(6)
    bad = [0, 0, 0, 0]
    refused = 0
    for it in range(total):
        which = it % 4
        func, nargs, name = FUNCS[which]
        # constants: the real ones, sometimes 0 (division by zero) or others
        r = random.random()
        kk = k0 if r < 0.85 else struct.pack('<2f', 0.0, 0.0) if r < 0.92 else struct.pack('<2f', random.uniform(-9, 9), 3.0)
        uc.mem_write(host(0x7b7a50), kk)
        src = XVA + 0x20000 + random.randrange(0, 0x1100)  # sometimes across a page
        out = XVA + 0x22000 + random.randrange(0, 0x1100) * 4 // 4
        data = bytes(random.getrandbits(8) for _ in range(0x40))
        if random.random() < 0.3:  # typical PSX values
            data = b''.join(struct.pack('<h', random.choice([4096, -4096, 0, random.randrange(-4096, 4097)])) for _ in range(9)) \
                + b''.join(struct.pack('<i', random.randrange(-100000, 100000)) for _ in range(3)) + data[30:]
        emu.wva(src, data)
        emu.wva(out, bytes(random.getrandbits(8) for _ in range(0x40)))
        arg = [src, out] if nargs == 2 else [struct.unpack('<I', data[:4])[0]]
        slow = False
        if which == 3:  # a texture: buffer state, counters, pointers
            T, P, PT = XVA + 0x30000, XVA + 0x31000, XVA + 0x31800
            emu.wva(T, bytes(random.getrandbits(8) for _ in range(0x100)))
            emu.wva(P, bytes(random.getrandbits(8) for _ in range(0x40)))
            n = random.choice([1, 1, 1, 2, 3, 7, 0, -1])
            cur = random.getrandbits(32)
            room = random.choice([random.randrange(0, 2000), 0, 1, n, random.getrandbits(32)])
            emu.wva(T + 0x30, struct.pack('<I', PT)); emu.wva(PT, struct.pack('<I', cur))
            emu.wva(T + 0x54, struct.pack('<I', random.choice([cur, cur + 1, cur - 1 if cur else 0, 0xffffffff])))
            emu.wva(T + 0x50, struct.pack('<I', room & 0xffffffff))
            emu.wva(T + 0x8c, struct.pack('<I', random.choice([P, T + 0x40])))  # p may alias t's fields
            t_arg = 0 if random.random() < 0.05 else T
            arg = [n & 0xffffffff, t_arg]
            if t_arg:
                tf = bytes(emu.rva(T, 0x100))
                p54 = struct.unpack_from('<I', tf, 0x54)[0]; p50 = struct.unpack_from('<i', tf, 0x50)[0]
                slow = p54 < cur or n < 1 or n > p50
        fstate = bytearray(uc.mem_read(B + FPU, 0x68))
        fstate[0x56] = 0
        fstate[0x60:0x64] = struct.pack('<I', random.randrange(8))
        uc.mem_write(B + FPU, bytes(fstate))
        esp0 = XVA + 0x10000
        regs0 = [random.getrandbits(32) for _ in range(8)]
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        state_x = bytes(uc.mem_read(XH, 0x200000)); state_f = bytes(uc.mem_read(B + FPU, 0x68))

        events[0] = 0
        ev_o = random.getrandbits(32); emu.w32(B + EVCOUNT, ev_o)
        if slow:  # the original would allocate: the native code must refuse and change nothing
            esp = esp0 - 4 * len(arg) - 4
            o_x, o_f = state_x, state_f
            o_r = [emu.reg(i) for i in range(8)]; o_r[4] = esp; o_ev = 0
        else:
            esp = emu.call_x86(func, arg, esp=esp0)
            o_x = bytes(uc.mem_read(XH, 0x200000)); o_f = bytes(uc.mem_read(B + FPU, 0x68))
            o_r = [emu.reg(i) for i in range(8)]; o_ev = events[0]
            assert (emu.r32(B + EVCOUNT) - ev_o) & 0xffffffff == o_ev

        uc.mem_write(XH, state_x); uc.mem_write(B + FPU, state_f)
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        sp_ = esp0 - 4 * len(arg)
        for i, a in enumerate(arg): emu.wva(sp_ + 4 * i, struct.pack('<I', a))
        emu.wva(sp_ - 4, struct.pack('<I', 0xdeadbeef))
        emu.setreg(4, sp_ - 4)
        uc.mem_write(ENV, struct.pack('<6I4I', B + MK_ADDRESS | 1, B + CHECK_EVENT | 1, B + REGS, B + FPU, B + MEMMAP,
                                      B + EVCOUNT, 0, 0, 0, 0))
        ev0 = random.choice([random.getrandbits(32), 0x7fe, 0x3fe, 0xfffff7f0 | random.randrange(16)])
        emu.w32(B + EVCOUNT, ev0); emu.ev_at = []
        events[0] = 0
        uc.reg_write(UC_ARM_REG_R0, ENV); uc.reg_write(UC_ARM_REG_R1, which)
        uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000); uc.reg_write(UC_ARM_REG_LR, END | 1)
        uc.emu_start(entry | 1, END, count=200000)
        ret = uc.reg_read(UC_ARM_REG_R0)
        n_x = bytes(uc.mem_read(XH, 0x200000)); n_f = bytes(uc.mem_read(B + FPU, 0x68))
        n_r = [emu.reg(i) for i in range(8)]; n_ev = (emu.r32(B + EVCOUNT) - ev0) & 0xffffffff
        # the real _check_event runs on every 2048th count, and only then from the native code
        if any(v & 0x7ff for v, lr in emu.ev_at if BLOB <= lr < BLOB + 0x10000) or \
                {v for v, lr in emu.ev_at if not v & 0x7ff} != {(ev0 + k) & 0xffffffff for k in range(1, n_ev + 1)
                                                                if not (ev0 + k) & 0x7ff}:
            n_ev = -1

        problems = []
        ret = ret - (1 << 32) if ret >= 1 << 31 else ret
        if ret != (-1 if slow else 1): problems.append('native returned %d (slow path %s)' % (ret, slow))
        if slow: refused += 1
        lo, hi = esp0 - 0x1000 - XVA, (esp if not slow else esp0) - XVA  # slow: the arguments written for the native run
        if n_x[:lo] != o_x[:lo] or n_x[hi:] != o_x[hi:]:
            diffs = [o for o in range(0, len(n_x), 4) if n_x[o:o + 4] != o_x[o:o + 4] and not lo <= o < hi]
            problems.append('memory: %d words differ, first at %x (native %s, original %s)'
                            % (len(diffs), XVA + diffs[0], n_x[diffs[0]:diffs[0] + 4].hex(), o_x[diffs[0]:diffs[0] + 4].hex()))
        if n_r != o_r:
            problems.append('regs %s' % ', '.join('%d: %08x/%08x' % (i, n_r[i], o_r[i]) for i in range(8) if n_r[i] != o_r[i]))
        fd = [hex(i) for i in range(0x40, 0x68) if n_f[i] != o_f[i]]
        if fd: problems.append('x87 bytes differ at %s' % fd)
        if which == 1:
            top = struct.unpack('<I', o_f[0x60:0x64])[0] & 7
            if n_f[8 * top:8 * top + 8] != o_f[8 * top:8 * top + 8]:
                problems.append('pushed value %s, original %s' % (n_f[8 * top:8 * top + 8].hex(), o_f[8 * top:8 * top + 8].hex()))
        if n_ev != o_ev: problems.append('_check_event %d, original %d' % (n_ev, o_ev))
        if problems:
            bad[which] += 1
            if sum(bad) <= 8: print('MISMATCH %s (call %d): %s' % (name, it, '; '.join(problems)))
    for i, (f, n, name) in enumerate(FUNCS):
        print('%s: %d mismatches in %d calls' % (name, bad[i], (total + 3 - i) // 4))
    print('0x66E272: %d slow-path cases refused as expected' % refused)
    sys.exit(1 if sum(bad) else 0)

if __name__ == '__main__':
    main()
