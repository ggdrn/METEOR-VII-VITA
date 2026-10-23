#!/usr/bin/env python3
"""
Compares the chocobo race's draw list in source/ff7_chocobo_core.h with the
recompiled functions under emulation: 0x770305 (the object lists), 0x770767 /
0x7706D7 (one triangle / quad object) and 0x77BB50 / 0x77BE95 (one
primitive), all down to 0x77B9D4 / 0x77BCA4 and the recompiled 0x66E272.
0x770434 (type 0x21 objects) is replaced, in both runs, by a stub that reads
its arguments and registers and leaves its own results, so the call setup is
checked too. Random lists: the three object types, unknown types, list
separators, flags, the stack left over as the first flag, textures with and
without room, a null texture, vertices behind the camera.
Compared: the 8 x86 registers, x87 state, x86 memory (but the stack below the
call), the race's globals pages and the _check_event() calls.

Usage: scripts/emu/test_chocobo_list.py libjni_ff7.so ff7_en.exe [calls]
Needs arm-vita-eabi-gcc on PATH (source scripts/env.sh) and `pip install unicorn`.
"""
import math, random, struct, sys
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
from ff7emu import Emu, B, FPU, MEMMAP, XVA, XH, REGS, END, HSTACK, CHECK_EVENT, EVCOUNT
from test_world_tri import pe_reader
from test_chocobo import build_blob, f32, ALLOC, MK_ADDRESS

F_LIST, F_OBJ, F_WRAP = 0x6a57a0, {0: 0x6a074c, 1: 0x6a2720}, {3: 0x6a03bc, 4: 0x6a253c}
F_770434 = 0x6a4e84
BLOB, ENV = 0x58000000, 0x59000000
HOSTG = 0x53000000  # x86 pages 0xE71000, 0xE72000
G = 0xe71000

def main():
    so, exe = sys.argv[1], sys.argv[2]
    total = int(sys.argv[3]) if len(sys.argv) > 3 else 1500
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
    uc.mem_map(HOSTG, 0x2000)
    emu.w32(B + MEMMAP + 4 * 0xe71, HOSTG)
    emu.w32(B + MEMMAP + 4 * 0xe72, HOSTG + 0x1000)
    def gw(va, data): uc.mem_write(HOSTG + va - G, bytes(data))
    uc.mem_map(BLOB, 0x10000); uc.mem_write(BLOB, blob); uc.mem_map(ENV, 0x1000)
    events = [0]
    def count(uc_, addr, size, user): events[0] += 1
    uc.hook_add(UC_HOOK_CODE, count, begin=B + CHECK_EVENT, end=B + CHECK_EVENT + 1)
    stub_calls = [0]
    def stub(uc_, addr, size, user):
        # 0x770434 (m, i, count): results from its arguments and registers
        esp = emu.reg(4)
        m, i, cnt = struct.unpack('<3I', emu.rva(esp + 4, 12))
        r = [emu.reg(k) for k in range(8)]
        slot = G + 0xd00 + 4 * (i & 0x3f)
        old = struct.unpack('<I', bytes(uc.mem_read(HOSTG + slot - G, 4)))[0]
        gw(slot, struct.pack('<I', (old * 31 + m + cnt + r[0] + r[1] + r[2] + r[5] + esp) & 0xffffffff))
        emu.setreg(0, i & 1)
        emu.setreg(1, (r[1] * 3 + 1) & 0xffffffff)
        emu.setreg(2, r[2] ^ 0x5a5a5a5a)
        emu.setreg(4, esp + 4)
        stub_calls[0] += 1
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, stub, begin=B + F_770434, end=B + F_770434 + 1)

    BASE, TENT, TP, QENT, QP, WTAB, PTRS, LTAB = (XVA + 0x20000, XVA + 0x20100, XVA + 0x21000, XVA + 0x22000,
                                                 XVA + 0x23000, XVA + 0x24000, XVA + 0x24100, XVA + 0x25000)
    TEX = [XVA + 0x26000 + 0x400 * k for k in range(4)]
    VB = XVA + 0x28000
    NOBJ = 24
    random.seed(11)
    specials = [0.0, -0.0, 1.0, -1.0, 1e-30, 1e30, math.inf, -math.inf, math.nan]
    bad = {}
    calls = {}
    tot = [0] * 6
    for it in range(total):
        kind = ('list', 'list', 'list', 'obj', 'wrap')[it % 5]
        sp = it % 7 == 6
        def rf(lo, hi):
            return f32(random.choice(specials)) if sp and random.random() < 0.1 else f32(random.uniform(lo, hi))
        # globals page, the race's model
        uc.mem_write(HOSTG, bytes(random.getrandbits(8) for _ in range(0x2000)))
        gw(0xe7112c, struct.pack('<I', BASE))
        gw(0xe715fc, struct.pack('<I', LTAB))
        gw(0xe71108, struct.pack('<I', random.randrange(100)))
        m = [rf(-2, 2) for _ in range(16)]
        if it % 4: m[15] = f32(random.uniform(10, 5000))
        gw(0xe72e60, struct.pack('<16f', *m))
        for k, off in enumerate((0xc28, 0xc44, 0xbf0, 0xc0c)):
            gw(G + off, struct.pack('<I', 0 if random.random() < 0.05 else TEX[k]))
        for t in TEX:
            emu.wva(t, bytes(0x100))
            emu.wva(t + 0x30, struct.pack('<I', t + 0x200)); emu.wva(t + 0x200, struct.pack('<I', 3))
            emu.wva(t + 0x54, struct.pack('<I', 5))
            emu.wva(t + 0x50, struct.pack('<i', 400))
            emu.wva(t + 0x8c, struct.pack('<I', t + 0x300))
            emu.wva(t + 0x80, struct.pack('<I', VB + 0x40 * random.randrange(4)))
            emu.wva(t + 0x40, struct.pack('<I', 0x20))
        emu.wva(VB, bytes(0x2000))
        emu.wva(BASE, bytes(random.getrandbits(8) for _ in range(0x60)))
        for off, v in ((0x18, TENT), (0x4c, TP), (0x10, QENT), (0x50, QP), (0x1c, WTAB), (0x20, PTRS)):
            emu.wva(BASE + off, struct.pack('<I', v))
        for i in range(NOBJ):
            e = bytearray(random.getrandbits(8) for _ in range(0x28))
            if random.random() < 0.5: e[0x1b] = 0x30
            if random.random() < 0.5: e[0x27] = 0x38
            emu.wva(TENT + 0x1c * i, e[:0x1c]) if i < NOBJ - 1 else None
            emu.wva(QENT + 0x28 * i, e)
            emu.wva(TP + 0x30 * i, struct.pack('<9f', *[rf(-3000, 3000) for _ in range(9)])
                    + struct.pack('<3I', *[random.getrandbits(32) for _ in range(3)]))
            emu.wva(QP + 0x40 * i, struct.pack('<12f', *[rf(-3000, 3000) for _ in range(12)])
                    + struct.pack('<4I', *[random.getrandbits(32) for _ in range(4)]))
        emu.wva(TENT + 0x1c * (NOBJ - 1), bytes(random.getrandbits(8) for _ in range(0x1c)))
        nl = 6
        for l in range(nl):
            emu.wva(LTAB + 0x18 * l, bytes(random.getrandbits(8) for _ in range(0x18)))
        # the entries: lists separated by negative entries, ended by -1
        ents = []
        for l in range(nl):
            if l: ents.append(random.choice([0x80000000, 0xfffffffe, 0x80123456, 0x80210003, 0x80010002, 0xff000000 | random.getrandbits(24)]))
            for _ in range(random.randrange(0, 8)):
                ty = random.choice([0, 0, 0, 1, 1, 0x21, 0x21, 5, 0x20])
                ents.append(ty << 16 | random.randrange(NOBJ) | random.getrandbits(8) << 24 & 0x7f000000)
        ents.append(0xffffffff)
        start = random.randrange(4)
        emu.wva(PTRS, b''.join(struct.pack('<I', random.getrandbits(32) & 0x7fffffff) for _ in range(start)))
        emu.wva(PTRS + 4 * start, b''.join(struct.pack('<I', v) for v in ents))
        first = random.randrange(3)
        emu.wva(WTAB, struct.pack('<8h', *([start] * 8)))
        last = first + random.randrange(-1, nl)

        fstate = bytearray(uc.mem_read(B + FPU, 0x68))
        for i in (0x5c, 0x5e, 0x64): fstate[i] = random.randrange(2)
        fstate[0x56] = 0
        fstate[0x60:0x64] = struct.pack('<I', random.randrange(8))
        uc.mem_write(B + FPU, bytes(fstate))
        esp0 = XVA + 0x10000
        # stack left over: the first list's flag, sometimes negative
        emu.wva(esp0 - 0x100, bytes(random.getrandbits(8) for _ in range(0x100)))
        if kind == 'list':
            func, mode = F_LIST, 30
            args = [first & 0xffffffff, last & 0xffffffff]
            # its frame: [ebp - 4] = esp0 - 4 (args) - 4 (ret) - 4 (ebp) - 4
            emu.wva(esp0 - 20, struct.pack('<I', random.choice([0, 0x80, 0xffffff00, 0x80000000])))
        elif kind == 'obj':
            q = random.randrange(2)
            func, mode = F_OBJ[q], 23 + q
            args = [0xe72e60, random.randrange(NOBJ - 1), random.getrandbits(32),
                    random.choice([0, 0x80, 0x80000000, 0xffffffff])]
        else:
            n = random.choice([3, 4])
            func, mode = F_WRAP[n], 10 + n
            tex = 0 if random.random() < 0.05 else TEX[random.randrange(4)]
            args = [0xe72e60, tex, (TP + 0x30 * random.randrange(NOBJ)) if n == 3 else (QP + 0x40 * random.randrange(NOBJ))]
        regs0 = [random.getrandbits(32) for _ in range(8)]
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        state_x = bytes(uc.mem_read(XH, 0x200000)); state_f = bytes(uc.mem_read(B + FPU, 0x68))
        state_g = bytes(uc.mem_read(HOSTG, 0x2000))

        ev_o = random.getrandbits(32); emu.w32(B + EVCOUNT, ev_o)
        events[0] = 0; stub_calls[0] = 0
        esp = emu.call_x86(func, args, esp=esp0)
        o_x = bytes(uc.mem_read(XH, 0x200000)); o_f = bytes(uc.mem_read(B + FPU, 0x68))
        o_g = bytes(uc.mem_read(HOSTG, 0x2000))
        o_r = [emu.reg(i) for i in range(8)]; o_ev = events[0]
        assert (emu.r32(B + EVCOUNT) - ev_o) & 0xffffffff == o_ev; o_st = stub_calls[0]

        uc.mem_write(XH, state_x); uc.mem_write(B + FPU, state_f); uc.mem_write(HOSTG, state_g)
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        sp_ = esp0 - 4 * len(args)
        for i, a in enumerate(args): emu.wva(sp_ + 4 * i, struct.pack('<I', a))
        emu.wva(sp_ - 4, struct.pack('<I', 0xdeadbeef))
        emu.setreg(4, sp_ - 4)
        uc.mem_write(ENV, struct.pack('<8I5I', B + MK_ADDRESS | 1, B + CHECK_EVENT | 1, B + REGS, B + FPU,
                                      B + ALLOC | 1, B + F_770434 | 1, B + MEMMAP, B + EVCOUNT, 0, 0, 0, 0, 0))
        ev0 = random.choice([random.getrandbits(32), 0x7fe, 0x3fe, 0xfffff7f0 | random.randrange(16)])
        emu.w32(B + EVCOUNT, ev0); emu.ev_at = []
        events[0] = 0; stub_calls[0] = 0
        uc.reg_write(UC_ARM_REG_R0, ENV); uc.reg_write(UC_ARM_REG_R1, mode)
        uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000); uc.reg_write(UC_ARM_REG_LR, END | 1)
        uc.emu_start(entry | 1, END, count=5000000)
        ret = uc.reg_read(UC_ARM_REG_R0)
        n_x = bytes(uc.mem_read(XH, 0x200000)); n_f = bytes(uc.mem_read(B + FPU, 0x68))
        n_g = bytes(uc.mem_read(HOSTG, 0x2000))
        n_r = [emu.reg(i) for i in range(8)]; n_ev = (emu.r32(B + EVCOUNT) - ev0) & 0xffffffff
        n_st = stub_calls[0]
        # the real _check_event runs on every 2048th count, and only then from the native code
        if any(v & 0x7ff for v, lr in emu.ev_at if BLOB <= lr < BLOB + 0x10000) or \
                {v for v, lr in emu.ev_at if not v & 0x7ff} != {(ev0 + k) & 0xffffffff for k in range(1, n_ev + 1)
                                                                if not (ev0 + k) & 0x7ff}:
            n_ev = -1
        for k, v in enumerate(struct.unpack('<5I', bytes(uc.mem_read(ENV + 32, 20))) + (n_st,)): tot[k] += v

        problems = []
        if ret != 1: problems.append('native returned %d' % ret)
        lo, hi = esp0 - 0x1000 - XVA, esp - XVA
        if n_x[:lo] != o_x[:lo] or n_x[hi:] != o_x[hi:]:
            diffs = [o for o in range(0, len(n_x), 4) if n_x[o:o + 4] != o_x[o:o + 4] and not lo <= o < hi]
            problems.append('memory: %d words differ, first at %x (native %s, original %s)'
                            % (len(diffs), XVA + diffs[0], n_x[diffs[0]:diffs[0] + 4].hex(), o_x[diffs[0]:diffs[0] + 4].hex()))
        if kind == 'list':
            # the frame the original leaves (locals and saved EBP)
            f0 = esp0 - 16 - 0x20 - XVA
            if n_x[f0:f0 + 0x24] != o_x[f0:f0 + 0x24]:
                problems.append('frame: native %s, original %s' % (n_x[f0:f0 + 0x24].hex(), o_x[f0:f0 + 0x24].hex()))
        if n_g != o_g:
            d = [o for o in range(0, 0x2000, 4) if n_g[o:o + 4] != o_g[o:o + 4]]
            problems.append('globals: %d words differ, first at %x' % (len(d), G + d[0]))
        if n_r != o_r:
            problems.append('regs %s' % ', '.join('%d: %08x/%08x' % (i, n_r[i], o_r[i]) for i in range(8) if n_r[i] != o_r[i]))
        fd = [hex(i) for i in range(0x40, 0x68) if n_f[i] != o_f[i]]
        if fd: problems.append('x87 bytes differ at %s' % fd)
        if n_ev != o_ev: problems.append('_check_event %d, original %d' % (n_ev, o_ev))
        if n_st != o_st: problems.append('0x770434 calls %d, original %d' % (n_st, o_st))
        calls[kind] = calls.get(kind, 0) + 1
        if problems:
            bad[kind] = bad.get(kind, 0) + 1
            if sum(bad.values()) <= 8: print('MISMATCH %s (call %d): %s' % (kind, it, '; '.join(problems)))
    names = {'list': '0x770305 (lists)', 'obj': '0x770767/0x7706D7 (objects)', 'wrap': '0x77BB50/0x77BE95 (primitives)'}
    for k in ('list', 'obj', 'wrap'):
        print('%s: %d mismatches in %d calls' % (names[k], bad.get(k, 0), calls.get(k, 0)))
    print('native: %d triangles, %d quads (%d drawn), %d lists, %d objects, %d type 0x21 calls' % tuple(tot))
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
