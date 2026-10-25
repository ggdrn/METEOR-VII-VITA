#!/usr/bin/env python3
"""
Compares source/ff7_fieldbg_core.h (native field background tile lists:
0x64085C, a layer's tiles, and 0x6464BA, append a tile to a draw list) with
the recompiled functions under emulation. The header is compiled into a
freestanding ARM blob (blob_fieldbg.c); both run in the same emulator from
the same state.

Random layers: tiles inside / outside each edge of the window, flag bytes
that allow or not, the hidden layer, both list fields, the row shift, scales
and positions that overflow int16. Compared: the 8 x86 registers, x87 bytes,
x86 memory (but the stack below the call) and the _check_event() calls (the
counter, and the native code calling the real function only on every 2048th).

Usage: scripts/emu/test_fieldbg.py libjni_ff7.so [calls]
Needs arm-vita-eabi-gcc on PATH (source scripts/env.sh) and `pip install unicorn`.
"""
import os, random, struct, subprocess, sys, tempfile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
from ff7emu import Emu, B, FPU, MEMMAP, XVA, XH, REGS, END, HSTACK, CHECK_EVENT, EVCOUNT

FUNCS = {1: 0x274a98, 2: 0x274574}
NAMES = {1: '0x64085C (layer)', 2: '0x6464BA (append)'}
MK_ADDRESS = 0x93d714
BLOB, ENV = 0x58000000, 0x59000000
PAGES = [0xcc0, 0xcff, 0xd00]
PH = 0x56000000
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'source')
STEPS = 20000000

def build_blob():
    d = tempfile.mkdtemp()
    elf, binf = os.path.join(d, 'blob.elf'), os.path.join(d, 'blob.bin')
    subprocess.run(['arm-vita-eabi-gcc', '-mfloat-abi=softfp', '-O3', '-ffreestanding', '-nostdlib', '-nostartfiles',
                    '-static', '-Wl,-Ttext=0x%x' % BLOB, '-Wl,-e,entry', '-Wl,--gc-sections', '-I', SRC,
                    os.path.join(HERE, 'blob_fieldbg.c'), '-lgcc', '-o', elf], check=True)
    subprocess.run(['arm-vita-eabi-objcopy', '-O', 'binary', elf, binf], check=True)
    nm = subprocess.run(['arm-vita-eabi-nm', elf], capture_output=True, text=True, check=True).stdout
    return open(binf, 'rb').read(), [int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' entry')][0]

def main():
    so = sys.argv[1]
    total = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    blob, entry = build_blob()
    emu = Emu(so)
    uc = emu.uc
    uc.mem_map(PH, 0x1000 * len(PAGES))
    for i, p in enumerate(PAGES): emu.w32(B + MEMMAP + 4 * p, PH + 0x1000 * i)
    def pw(va, data): uc.mem_write(PH + 0x1000 * PAGES.index(va >> 12) + (va & 0xfff), bytes(data))
    uc.mem_map(BLOB, 0x30000); uc.mem_write(BLOB, blob); uc.mem_map(ENV, 0x1000)
    events = [0]
    def count(uc_, addr, size, user): events[0] += 1
    uc.hook_add(UC_HOOK_CODE, count, begin=B + CHECK_EVENT, end=B + CHECK_EVENT + 1)
    TILES, INDEX, LISTS = XVA + 0x40000, XVA + 0x30000, XVA + 0x80000
    random.seed(23)
    bad = {m: 0 for m in FUNCS}
    calls = {m: 0 for m in FUNCS}
    stats = [0, 0, 0]
    for it in range(total):
        mode = 2 if it % 5 == 4 else 1
        uc.mem_write(PH, bytes(random.getrandbits(8) for _ in range(0x1000 * len(PAGES))))
        scale = random.choice([1, 1, 2, 3, random.randrange(-4, 300)])
        pw(0xcff1f0, struct.pack('<i', scale))
        pw(0xcff1d8, struct.pack('<i', random.choice([0, 0, 1])))
        pw(0xcff24c, struct.pack('<i', random.choice([0, 0, -3, random.randrange(1, 40)])))
        pw(0xcff204, struct.pack('<ii', random.randrange(-500, 500), random.randrange(-500, 500)))
        layer = random.randrange(6)
        pw(0xd00088, struct.pack('<H', layer))
        nt = random.choice([0, 1, random.randrange(2, 40), random.randrange(2, 40)])
        ntiles = 48
        pw(0xcffdfc, struct.pack('<H', nt))
        pw(0xcff3fc, struct.pack('<I', INDEX))
        pw(0xcffe00, struct.pack('<I', TILES))
        pw(0xd00050, struct.pack('<I', LISTS))
        emu.wva(INDEX, b''.join(struct.pack('<HH', random.randrange(ntiles), random.getrandbits(16)) for _ in range(40)))
        x = random.choice([random.randrange(-200, 700), 0x140, 0x150])
        y = random.choice([random.randrange(-200, 500), 8, 9, 0xe0])
        tiles = bytearray(random.getrandbits(8) for _ in range(0x105c * ntiles))
        for k in range(ntiles):
            o = 0x105c * k
            tx = random.choice([x - 1, x, x - 0x150, x - 0x14f, random.randrange(x - 0x160, x + 10)])
            ty = random.choice([y - 1, y, y - 0x100, y - 0xff, random.randrange(y - 0x110, y + 10)])
            struct.pack_into('<hh', tiles, o, max(-32768, min(32767, tx)), max(-32768, min(32767, ty)))
            tiles[o + 0x34] = random.choice([0, 0, random.getrandbits(8)])
            struct.pack_into('<H', tiles, o + 0x30, random.choice([layer + 1, layer, random.randrange(8)]))
            struct.pack_into('<I', tiles, o + 0x103c, random.choice([0, 0, 1]))
            struct.pack_into('<HH', tiles, o + 0x1056, random.randrange(4), random.randrange(4))
        emu.wva(TILES, tiles)
        lists = bytearray(random.getrandbits(8) for _ in range(0x1804 * 4))
        for l in range(4): struct.pack_into('<I', lists, 0x1804 * l, random.randrange(0, 100))
        emu.wva(LISTS, lists)
        if mode == 1:
            args = [(x & 0xffff) | random.getrandbits(16) << 16, (y & 0xffff) | random.getrandbits(16) << 16]
        else:
            fx = random.choice([random.uniform(-400, 400), float(random.randrange(-300, 300)), 1e30, -0.0])
            args = [struct.unpack('<I', struct.pack('<f', fx))[0], struct.unpack('<I', struct.pack('<f', random.uniform(-400, 400)))[0],
                    random.getrandbits(32), random.getrandbits(32), random.getrandbits(32), random.getrandbits(16),
                    random.randrange(4)]
        fstate = bytearray(uc.mem_read(B + FPU, 0x68))
        fstate[0x60:0x64] = struct.pack('<I', random.randrange(8))
        uc.mem_write(B + FPU, bytes(fstate))
        esp0 = XVA + 0x10000
        regs0 = [random.getrandbits(32) for _ in range(8)]
        regs0[5] = 0x12345678
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        snap = lambda: (bytes(uc.mem_read(XH, 0x200000)), bytes(uc.mem_read(PH, 0x1000 * len(PAGES))),
                        bytes(uc.mem_read(B + FPU, 0x68)))
        state = snap()
        def run(fn_addr, native):
            sp_ = esp0 - 4 * len(args)
            for i, a in enumerate(args): emu.wva(sp_ + 4 * i, struct.pack('<I', a))
            emu.wva(sp_ - 4, struct.pack('<I', 0xdeadbeef)); emu.setreg(4, sp_ - 4)
            uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000); uc.reg_write(UC_ARM_REG_LR, END | 1)
            if native:
                inline = it % 7 != 0
                uc.mem_write(ENV, struct.pack('<5I3I', B + MK_ADDRESS | 1, B + CHECK_EVENT | 1, B + REGS,
                                              B + MEMMAP if inline else 0, B + EVCOUNT if inline else 0, 0, 0, 0))
                uc.reg_write(UC_ARM_REG_R0, ENV); uc.reg_write(UC_ARM_REG_R1, mode)
            uc.emu_start(fn_addr | 1, END, count=STEPS)
            if uc.reg_read(UC_ARM_REG_PC) != END: raise RuntimeError('budget exceeded at call %d' % it)
            return sp_ - 4
        ev0 = random.choice([random.getrandbits(32), 0x7fe, 0xfffff7f0 | random.randrange(16)])
        emu.w32(B + EVCOUNT, ev0); emu.ev_at = []
        run(entry, True)
        n_s = snap(); n_r = [emu.reg(i) for i in range(8)]
        n_ev = (emu.r32(B + EVCOUNT) - ev0) & 0xffffffff
        inline = it % 7 != 0
        if inline and (any(v & 0x7ff for v, lr in emu.ev_at if BLOB <= lr < BLOB + 0x30000) or
                       {v for v, lr in emu.ev_at if not v & 0x7ff} != {(ev0 + k) & 0xffffffff for k in range(1, n_ev + 1)
                                                                       if not (ev0 + k) & 0x7ff}):
            n_ev = -1
        st = struct.unpack('<3I', bytes(uc.mem_read(ENV + 20, 12)))
        stats = [a + b for a, b in zip(stats, st)]
        uc.mem_write(XH, state[0]); uc.mem_write(PH, state[1]); uc.mem_write(B + FPU, state[2])
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        events[0] = 0; emu.w32(B + EVCOUNT, random.getrandbits(32))
        esp = run(B + FUNCS[mode], False)
        o_s = snap(); o_r = [emu.reg(i) for i in range(8)]; o_ev = events[0]
        calls[mode] += 1
        problems = []
        lo, hi = esp0 - 0x2000 - XVA, esp - XVA
        if n_s[0][:lo] != o_s[0][:lo] or n_s[0][hi:] != o_s[0][hi:]:
            d = [o for o in range(0, 0x200000, 4) if n_s[0][o:o + 4] != o_s[0][o:o + 4] and not lo <= o < hi]
            problems.append('memory: %d words differ, first at %x (native %s, original %s)'
                            % (len(d), XVA + d[0], n_s[0][d[0]:d[0] + 4].hex(), o_s[0][d[0]:d[0] + 4].hex()))
        if n_s[1] != o_s[1]: problems.append('globals differ')
        if n_r != o_r:
            problems.append('regs %s' % ', '.join('%d: %08x/%08x' % (i, n_r[i], o_r[i]) for i in range(8) if n_r[i] != o_r[i]))
        fd = [hex(i) for i in range(0x40, 0x68) if n_s[2][i] != o_s[2][i]]
        if fd: problems.append('x87 bytes differ at %s' % fd)
        if n_ev != o_ev: problems.append('_check_event %d, original %d' % (n_ev, o_ev))
        if problems:
            bad[mode] += 1
            if sum(bad.values()) <= 10: print('MISMATCH %s (call %d): %s' % (NAMES[mode], it, '; '.join(problems)))
    for m in FUNCS: print('%s: %d mismatches in %d calls' % (NAMES[m], bad[m], calls[m]))
    print('native: %d layers, %d tiles looked at, %d appended' % tuple(stats))
    sys.exit(1 if sum(bad.values()) else 0)

if __name__ == '__main__':
    main()
