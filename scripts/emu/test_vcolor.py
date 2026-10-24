#!/usr/bin/env python3
"""
Compares source/ff7_vcolor_core.h (native vertex color scaling of battle
models: 0x68F2D6, 0x68F0F8, 0x68EFCE) with the recompiled functions under
emulation. The header is compiled into a freestanding ARM blob
(blob_vcolor.c); both run in the same emulator from the same state.

Random polygon sets: locked or not (index out of range, no vertex buffer),
no colors, vertex counts 0..60, the paletted path (0x6A5BA0, 0x660F12,
0x660F54, the latter two calling a stub through the game object's table),
scales and colors with NaN, infinities, negatives and values beyond int32.
Compared: the 8 x86 registers, x87 bytes, x86 memory (but the stack below
the call) and the _check_event() calls (the counter, and the native code
calling the real function only on every 2048th).

Usage: scripts/emu/test_vcolor.py libjni_ff7.so [calls]
Needs arm-vita-eabi-gcc on PATH (source scripts/env.sh) and `pip install unicorn`.
"""
import os, random, struct, subprocess, sys, tempfile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
from ff7emu import Emu, B, FPU, MEMMAP, XVA, XH, REGS, END, HSTACK, CHECK_EVENT, EVCOUNT

FUNCS = {1: 0xcd3e64, 2: 0xcd34d4, 3: 0xade67c}
NAMES = {1: '0x68F2D6 (sets, paletted)', 2: '0x68F0F8 (sets)', 3: '0x68EFCE (one color)'}
CALLEES = [0xc76640, 0xc7fa7c, 0xc7f88c, 0xc76698, 0xc76828] # 676578 670196 670144 660f12 660f54
MK_ADDRESS = 0x93d714
BLOB, ENV = 0x58000000, 0x59000000
PH = 0x56000000 # page 0xdb2: [0xDB2BB8] = the game object
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'source')
STEPS = 20000000

def build_blob():
    d = tempfile.mkdtemp()
    elf, binf = os.path.join(d, 'blob.elf'), os.path.join(d, 'blob.bin')
    subprocess.run(['arm-vita-eabi-gcc', '-mfloat-abi=softfp', '-mfpu=neon', '-O3', '-ffreestanding', '-nostdlib',
                    '-nostartfiles', '-static', '-Wl,-Ttext=0x%x' % BLOB, '-Wl,-e,entry', '-Wl,--gc-sections', '-I', SRC,
                    os.path.join(HERE, 'blob_vcolor.c'), '-lgcc', '-o', elf], check=True)
    subprocess.run(['arm-vita-eabi-objcopy', '-O', 'binary', elf, binf], check=True)
    nm = subprocess.run(['arm-vita-eabi-nm', elf], capture_output=True, text=True, check=True).stdout
    return open(binf, 'rb').read(), [int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' entry')][0]

def f2u(f): return struct.unpack('<I', struct.pack('<f', f))[0]

def rnd_float(scale_like):
    r = random.random()
    if r < 0.03: return float('nan')
    if r < 0.05: return random.choice([float('inf'), -float('inf')])
    if r < 0.08: return random.choice([3e9, -3e9, 2147483520.0, -2147483648.0, 1e-40, -0.0])
    if r < 0.12: return random.uniform(-300, 0)
    return random.uniform(0, 2) if scale_like else random.choice([random.uniform(0, 255), float(random.randrange(256)),
                                                                     random.uniform(0, 600)])

def main():
    so = sys.argv[1]
    total = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    blob, entry = build_blob()
    emu = Emu(so)
    uc = emu.uc
    uc.mem_map(PH, 0x1000)
    emu.w32(B + MEMMAP + 4 * 0xdb2, PH)
    uc.mem_map(BLOB, 0x30000); uc.mem_write(BLOB, blob); uc.mem_map(ENV, 0x1000)
    events = [0]
    def count(uc_, addr, size, user): events[0] += 1
    uc.hook_add(UC_HOOK_CODE, count, begin=B + CHECK_EVENT, end=B + CHECK_EVENT + 1)
    # 0x660F12/0x660F54 call the renderer through a table (the emulator has no
    # indirect x86 calls): stubbed in both runs, which must make the same calls
    log = []
    def upload(uc_, addr, size, nargs):
        esp = emu.reg(4)
        a = struct.unpack('<%dI' % nargs, emu.rva(esp + 4, 4 * nargs))
        log.append((addr - B, a, emu.reg(0), emu.reg(1), emu.reg(2), emu.reg(5)))
        emu.setreg(0, sum(a) & 0xffffffff); emu.setreg(4, esp + 4)
        uc_.reg_write(UC_ARM_REG_PC, uc_.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, upload, 6, begin=B + 0xc76698, end=B + 0xc76699)
    uc.hook_add(UC_HOOK_CODE, upload, 2, begin=B + 0xc76828, end=B + 0xc76829)
    GAME, VT, OBJ, RECS, VBS, VBP = XVA + 0x50000, XVA + 0x51000, XVA + 0x20000, XVA + 0x21000, XVA + 0x22000, XVA + 0x23000
    COLORS, OUT, PAL, PALC, PALO = XVA + 0x30000, XVA + 0x40000, XVA + 0x52000, XVA + 0x60000, XVA + 0x70000
    random.seed(29)
    bad = {m: 0 for m in FUNCS}
    calls = {m: 0 for m in FUNCS}
    stats = [0, 0, 0]
    for it in range(total):
        mode = 3 if it % 6 == 5 else 2 if it % 3 == 1 else 1
        uc.mem_write(XH, bytes(random.getrandbits(8) for _ in range(0x80000)))
        uc.mem_write(PH, bytes(random.getrandbits(8) for _ in range(0x1000)))
        uc.mem_write(PH + 0xbb8, struct.pack('<I', GAME))
        emu.wva(GAME + 0x984, struct.pack('<I', random.choice([1, 1, 1, 0x80000000])))
        emu.wva(GAME + 0x934, struct.pack('<I', VT))
        emu.wva(VT + 0x54, struct.pack('<II', 0x676578, 0x676578))
        nsets = random.randrange(1, 6)
        emu.wva(OBJ + 0x10, struct.pack('<i', nsets))
        emu.wva(OBJ + 0x5c, struct.pack('<I', random.choice([RECS, RECS, RECS, 0])))
        emu.wva(OBJ + 0x50, struct.pack('<I', random.choice([VBP, VBP, VBP, 0])))
        for k in range(8):
            emu.wva(VBP + 4 * k, struct.pack('<I', random.choice([VBS + 0x40 * k] * 5 + [0])))
            emu.wva(VBS + 0x40 * k + 0x10, struct.pack('<I', OUT + 0x800 * k + random.choice([0, 0, 0xffe])))
            r = RECS + 0x24 * k
            emu.wva(r + 4, struct.pack('<i', random.choice([0, 1, random.randrange(2, 60), random.randrange(2, 60), -3])))
            emu.wva(r + 0x18, struct.pack('<I', random.choice([COLORS + 0x600 * k + random.choice([0, 0, 0xfee]), 0])))
            pt = XVA + 0x24000 + 0x100 * k
            emu.wva(r + 0x20, struct.pack('<I', random.choice([0, 0, 0, pt])))
            emu.wva(pt + 0x8c, struct.pack('<I', random.choice([0, PAL + 0x40 * k, PAL + 0x40 * k])))
            t = PAL + 0x40 * k
            emu.wva(t + 0xc, struct.pack('<i', random.randrange(-1, 5)))
            emu.wva(t + 0x10, struct.pack('<i', random.choice([0, 1, random.randrange(2, 40), -2])))
            emu.wva(t + 0x24, struct.pack('<I', random.choice([PALC + 0x2000 * (k % 4), PALC + 0x2000 * (k % 4), 0])))
            emu.wva(t + 0x18, struct.pack('<I', PALO + 0x800 * (k % 4)))
        emu.wva(COLORS, b''.join(struct.pack('<f', rnd_float(False)) for _ in range(0x10000 // 4)))
        emu.wva(PALC, b''.join(struct.pack('<f', rnd_float(False)) for _ in range(0x8000 // 4)))
        scale = f2u(rnd_float(True))
        if mode == 3:
            args = [scale, COLORS + random.choice([0, 0x10, 0xffc, 0xff8]), OUT + random.choice([0, 0x10, 0xffe])]
        elif mode == 2:
            args = [scale, random.choice([random.randrange(nsets), random.randrange(nsets), nsets]), OBJ]
        else:
            args = [scale, random.randrange(-1, 5), random.choice([random.randrange(nsets), random.randrange(nsets), nsets]), OBJ]
        fstate = bytearray(uc.mem_read(B + FPU, 0x68))
        fstate[0x60:0x64] = struct.pack('<I', random.randrange(8))
        uc.mem_write(B + FPU, bytes(fstate))
        esp0 = XVA + 0x10000
        regs0 = [random.getrandbits(32) for _ in range(8)]
        regs0[5] = 0x12345678
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        snap = lambda: (bytes(uc.mem_read(XH, 0x200000)), bytes(uc.mem_read(PH, 0x1000)), bytes(uc.mem_read(B + FPU, 0x68)))
        state = snap()
        inline = it % 7 != 0
        def run(fn_addr, native):
            sp_ = esp0 - 4 * len(args)
            for i, a in enumerate(args): emu.wva(sp_ + 4 * i, struct.pack('<I', a & 0xffffffff))
            emu.wva(sp_ - 4, struct.pack('<I', 0xdeadbeef)); emu.setreg(4, sp_ - 4)
            uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000); uc.reg_write(UC_ARM_REG_LR, END | 1)
            if native:
                uc.mem_write(ENV, struct.pack('<5I5I3I', B + MK_ADDRESS | 1, B + CHECK_EVENT | 1, B + REGS,
                                              B + MEMMAP if inline else 0, B + EVCOUNT if inline else 0,
                                              *[B + c | 1 for c in CALLEES], 0, 0, 0))
                uc.reg_write(UC_ARM_REG_R0, ENV); uc.reg_write(UC_ARM_REG_R1, mode)
            try: uc.emu_start(fn_addr | 1, END, count=STEPS)
            except Exception as ex:
                raise RuntimeError('%s at call %d mode %d native %s pc %x lr %x args %s' % (ex, it, mode, native, uc.reg_read(UC_ARM_REG_PC), uc.reg_read(UC_ARM_REG_LR), [hex(a) for a in args]))
            if uc.reg_read(UC_ARM_REG_PC) != END: raise RuntimeError('budget exceeded at call %d mode %d native %s pc %x args %s' % (it, mode, native, uc.reg_read(UC_ARM_REG_PC), [hex(a) for a in args]))
            return sp_ - 4
        ev0 = random.choice([random.getrandbits(32), 0x7fe, 0xfffff7f0 | random.randrange(16)])
        emu.w32(B + EVCOUNT, ev0); emu.ev_at = []
        del log[:]
        run(entry, True)
        n_log = list(log); del log[:]
        n_s = snap(); n_r = [emu.reg(i) for i in range(8)]
        n_ev = (emu.r32(B + EVCOUNT) - ev0) & 0xffffffff
        if inline and (any(v & 0x7ff for v, lr in emu.ev_at if BLOB <= lr < BLOB + 0x30000) or
                       {v for v, lr in emu.ev_at if not v & 0x7ff and BLOB <= lr < BLOB + 0x30000} !=
                       {(ev0 + k) & 0xffffffff for k in range(1, n_ev + 1) if not (ev0 + k) & 0x7ff}
                       - {v for v, lr in emu.ev_at if not BLOB <= lr < BLOB + 0x30000}):
            n_ev = -1
        st = struct.unpack('<3I', bytes(uc.mem_read(ENV + 40, 12)))
        stats = [a + b for a, b in zip(stats, st)]
        uc.mem_write(XH, state[0]); uc.mem_write(PH, state[1]); uc.mem_write(B + FPU, state[2])
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        events[0] = 0; emu.w32(B + EVCOUNT, random.getrandbits(32))
        esp = run(B + FUNCS[mode], False)
        o_s = snap(); o_r = [emu.reg(i) for i in range(8)]; o_ev = events[0]
        if n_log != log: problems_log = 'palette uploads: native %s, original %s' % (n_log, log)
        else: problems_log = None
        calls[mode] += 1
        problems = [problems_log] if problems_log else []
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
    print('native: %d sets scaled, %d colors, %d paletted sets' % tuple(stats))
    sys.exit(1 if sum(bad.values()) else 0)

if __name__ == '__main__':
    main()
