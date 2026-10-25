#!/usr/bin/env python3
"""
Compares source/ff7_snowboard_core.h (native 0x7334DA vertex transform,
0x73488D triangle clip + draw and its 0x734CAB wrapper, with 0x7335FB / 0x73404C / _ftol inline) with
the recompiled functions under emulation. The header is compiled into a
freestanding ARM blob (blob_snowboard.c); both run in the same emulator from
the same state and call the same recompiled 0x66E272 (its common path: the
test keeps the buffer current and with room, or a null texture).
Compared: the 8 x86 registers, x87 state (control/status bytes, TOP), all
x86 memory but the stack below the call, and the _check_event() calls (the
counter, and the native code calling the real function only on every 2048th).

Usage: scripts/emu/test_snowboard.py libjni_ff7.so ff7_en.exe [calls]
Needs arm-vita-eabi-gcc on PATH (source scripts/env.sh) and `pip install unicorn`.
"""
import math, os, random, struct, subprocess, sys, tempfile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR
from ff7emu import Emu, B, FPU, MEMMAP, XVA, XH, REGS, END, HSTACK, CHECK_EVENT, EVCOUNT
from test_world_tri import pe_reader

XFORM, TRI, TRI3 = 0x57c9f8, 0x588d7c, 0x589e20  # _Z15func_0x007334dav, 0x0073488dv, 0x00734cabv
ALLOC = 0xc50ab4
MK_ADDRESS = 0x93d714
BLOB, ENV = 0x58000000, 0x59000000
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'source')

def build_blob():
    d = tempfile.mkdtemp()
    elf, binf = os.path.join(d, 'blob.elf'), os.path.join(d, 'blob.bin')
    subprocess.run(['arm-vita-eabi-gcc', '-mfloat-abi=softfp', '-O3', '-ffreestanding', '-nostdlib', '-nostartfiles',
                    '-static', '-Wl,-Ttext=0x%x' % BLOB, '-Wl,-e,entry', '-Wl,--gc-sections', '-I', SRC,
                    os.path.join(HERE, 'blob_snowboard.c'), '-lgcc', '-o', elf], check=True)
    subprocess.run(['arm-vita-eabi-objcopy', '-O', 'binary', elf, binf], check=True)
    nm = subprocess.run(['arm-vita-eabi-nm', elf], capture_output=True, text=True, check=True).stdout
    return open(binf, 'rb').read(), [int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' entry')][0]

def f32(x):
    try: return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError: return math.copysign(math.inf, x)

def main():
    so, exe = sys.argv[1], sys.argv[2]
    total = int(sys.argv[3]) if len(sys.argv) > 3 else 4000
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
    uc.mem_map(BLOB, 0x30000); uc.mem_write(BLOB, blob); uc.mem_map(ENV, 0x1000)
    events = [0]
    def count(uc_, addr, size, user): events[0] += 1
    uc.hook_add(UC_HOOK_CODE, count, begin=B + CHECK_EVENT, end=B + CHECK_EVENT + 1)
    MAT, VTX, OUT, COL, SELF, T, P, PT, VB = (XVA + 0x20000, XVA + 0x21000, XVA + 0x21400, XVA + 0x21800,
                                             XVA + 0x21c00, XVA + 0x22000, XVA + 0x22400, XVA + 0x22800,
                                             XVA + 0x24000)
    random.seed(9)
    specials = [0.0, -0.0, 1.0, -1.0, 0.5, 1e-30, 1e30, math.inf, -math.inf, math.nan]
    bad = [0, 0, 0]
    calls = [0, 0, 0]
    clipped = [0] * 4
    for it in range(total):
        mode = 1 if it % 3 == 0 else random.choice([2, 3])  # transforms, triangles, wrapper
        sp = it % 7 == 6
        def rf(lo, hi):
            return f32(random.choice(specials)) if sp and random.random() < 0.15 else f32(random.uniform(lo, hi))
        if mode == 1:
            emu.wva(MAT, struct.pack('<16f', *[rf(-2, 2) for _ in range(16)]))
            v = VTX + 4 * random.randrange(0, 8)
            emu.wva(v, struct.pack('<3f', *[rf(-3000, 3000) for _ in range(3)]))
            o = OUT + 4 * random.randrange(0, 8)
            emu.wva(OUT, bytes(random.getrandbits(8) for _ in range(0x40)))
            args = [MAT, v, o]
            func = XFORM
        else:
            # z around the plane: all four clip cases
            verts = []
            nclip = random.randrange(4)
            behind = random.sample(range(3), nclip)
            for i in range(3):
                x, y = rf(-500, 500), rf(-500, 500)
                z = rf(-3, 0.49) if i in behind else rf(0.5, 50)
                if random.random() < 0.05: z = 0.5
                w = rf(0.01, 100) if random.random() < 0.95 else 0.0
                verts += [x, y, z, w]
            v = VTX + 4 * random.randrange(0, 4)
            if mode == 2:
                emu.wva(v, struct.pack('<12f', *verts))
                c = COL + 4 * random.randrange(0, 4)
                emu.wva(c, bytes(random.getrandbits(8) for _ in range(12)))
            else:  # three vertex pointers, three colors
                ps = [VTX + 0x40 * k + 4 * random.randrange(0, 4) for k in range(3)]
                for k in range(3): emu.wva(ps[k], struct.pack('<4f', *verts[4 * k:4 * k + 4]))
            # this: [this] = texture; texture with room (0x66E272's common path), sometimes null
            emu.wva(SELF, struct.pack('<I', 0 if it % 23 == 22 else T) + bytes([random.getrandbits(8)]) * 4)
            emu.wva(T, bytes(0x100))
            emu.wva(T + 0x30, struct.pack('<I', PT)); emu.wva(PT, struct.pack('<I', 3))
            emu.wva(T + 0x54, struct.pack('<I', 5)); emu.wva(T + 0x50, struct.pack('<i', 100))
            emu.wva(T + 0x8c, struct.pack('<I', P)); emu.wva(T + 0x80, struct.pack('<I', VB + 0x20 * random.randrange(8)))
            emu.wva(T + 0x40, struct.pack('<I', random.choice([0x20, 0x40, 0x60])))
            emu.wva(VB, bytes(random.getrandbits(8) for _ in range(0x400)))
            args = [v, c] if mode == 2 else ps + [random.getrandbits(32) for _ in range(3)]
            func = TRI if mode == 2 else TRI3
            clipped[nclip] += 1
        fstate = bytearray(uc.mem_read(B + FPU, 0x68))
        for i in (0x5c, 0x5e, 0x64): fstate[i] = random.randrange(2)
        fstate[0x56] = 0
        fstate[0x60:0x64] = struct.pack('<I', random.randrange(8))
        uc.mem_write(B + FPU, bytes(fstate))
        esp0 = XVA + 0x10000
        regs0 = [random.getrandbits(32) for _ in range(8)]
        regs0[5] = 0x12345678
        if mode != 1: regs0[1] = SELF
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        state_x = bytes(uc.mem_read(XH, 0x200000)); state_f = bytes(uc.mem_read(B + FPU, 0x68))

        events[0] = 0
        ev_o = random.getrandbits(32); emu.w32(B + EVCOUNT, ev_o)
        esp = emu.call_x86(func, args, esp=esp0)
        o_x = bytes(uc.mem_read(XH, 0x200000)); o_f = bytes(uc.mem_read(B + FPU, 0x68))
        o_r = [emu.reg(i) for i in range(8)]; o_ev = events[0]
        assert (emu.r32(B + EVCOUNT) - ev_o) & 0xffffffff == o_ev

        uc.mem_write(XH, state_x); uc.mem_write(B + FPU, state_f)
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        sp_ = esp0 - 4 * len(args)
        for i, a in enumerate(args): emu.wva(sp_ + 4 * i, struct.pack('<I', a))
        emu.wva(sp_ - 4, struct.pack('<I', 0xdeadbeef))
        emu.setreg(4, sp_ - 4)
        uc.mem_write(ENV, struct.pack('<7I4I', B + MK_ADDRESS | 1, B + CHECK_EVENT | 1, B + REGS, B + FPU,
                                      B + ALLOC | 1, B + MEMMAP, B + EVCOUNT, 0, 0, 0, 0))
        ev0 = random.choice([random.getrandbits(32), 0x7fe, 0x3fe, 0xfffff7f0 | random.randrange(16)])
        emu.w32(B + EVCOUNT, ev0); emu.ev_at = []
        events[0] = 0
        uc.reg_write(UC_ARM_REG_R0, ENV); uc.reg_write(UC_ARM_REG_R1, mode)
        uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000); uc.reg_write(UC_ARM_REG_LR, END | 1)
        uc.emu_start(entry | 1, END, count=2000000)
        ret = uc.reg_read(UC_ARM_REG_R0)
        n_x = bytes(uc.mem_read(XH, 0x200000)); n_f = bytes(uc.mem_read(B + FPU, 0x68))
        n_r = [emu.reg(i) for i in range(8)]
        n_ev = (emu.r32(B + EVCOUNT) - ev0) & 0xffffffff
        # the real _check_event runs on every 2048th count, and only then from the native code
        if any(v & 0x7ff for v, lr in emu.ev_at if BLOB <= lr < BLOB + 0x10000) or \
                {v for v, lr in emu.ev_at if not v & 0x7ff} != {(ev0 + k) & 0xffffffff for k in range(1, n_ev + 1)
                                                                if not (ev0 + k) & 0x7ff}:
            n_ev = -1

        problems = []
        if ret != 1: problems.append('native returned %d' % ret)
        lo, hi = esp0 - 0x1000 - XVA, esp - XVA
        if n_x[:lo] != o_x[:lo] or n_x[hi:] != o_x[hi:]:
            diffs = [o for o in range(0, len(n_x), 4) if n_x[o:o + 4] != o_x[o:o + 4] and not lo <= o < hi]
            problems.append('memory: %d words differ, first at %x (native %s, original %s)'
                            % (len(diffs), XVA + diffs[0], n_x[diffs[0]:diffs[0] + 4].hex(), o_x[diffs[0]:diffs[0] + 4].hex()))
        if n_r != o_r:
            problems.append('regs %s' % ', '.join('%d: %08x/%08x' % (i, n_r[i], o_r[i]) for i in range(8) if n_r[i] != o_r[i]))
        fd = [hex(i) for i in range(0x40, 0x68) if n_f[i] != o_f[i]]
        if fd: problems.append('x87 bytes differ at %s' % fd)
        if n_ev != o_ev: problems.append('_check_event %d, original %d' % (n_ev, o_ev))
        calls[mode - 1] += 1
        if problems:
            bad[mode - 1] += 1
            if sum(bad) <= 8: print('MISMATCH mode %d (call %d): %s' % (mode, it, '; '.join(problems)))
    print('0x7334DA (transform): %d mismatches in %d calls' % (bad[0], calls[0]))
    print('0x73488D (clip + draw): %d mismatches in %d calls' % (bad[1], calls[1]))
    print('0x734CAB (wrapper): %d mismatches in %d calls (triangles with 0/1/2/3 vertices behind the plane: %s)'
          % (bad[2], calls[2], '/'.join(map(str, clipped))))
    sys.exit(1 if sum(bad) else 0)

if __name__ == '__main__':
    main()
