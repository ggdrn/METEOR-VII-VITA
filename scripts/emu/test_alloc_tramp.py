#!/usr/bin/env python3
"""
Checks ff7_engine.c's trampoline for 0x66E272's original (used instead of
SO_CONTINUE): the trampoline code and its setup are compiled into an ARM blob,
the setup reads the function's first instructions, then the entry is
overwritten as the hook does and the function is called through the
trampoline. Compared with the untouched function from the same state: x86
registers, x86 memory, the result. Covers the common path and a null texture
(the slow path calls imports the emulator does not have).

Usage: scripts/emu/test_alloc_tramp.py libjni_ff7.so
Needs arm-vita-eabi-gcc on PATH (source scripts/env.sh) and `pip install unicorn`.
"""
import os, random, re, struct, subprocess, sys, tempfile
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R4
from ff7emu import Emu, B, XVA, XH, END, HSTACK

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'source', 'ff7_engine.c')
ALLOC = 0xc50ab4
BLOB = 0x58000000

def build_blob():
    s = open(SRC).read()
    # the trampoline and its setup, verbatim from ff7_engine.c
    tramp = s[s.index('uintptr_t ff7_engine_alloc_r4'):s.index('static void original(')]
    c = ('#include <stdint.h>\n' + tramp +
         '__attribute__((section(".text.entry"), used)) int entry(int mode, uintptr_t fn) {\n'
         '    if (mode == 0) return alloc_tramp_setup(fn);\n'
         '    alloc_original();\n    return 0;\n}\n')
    d = tempfile.mkdtemp()
    src, elf, binf = os.path.join(d, 'b.c'), os.path.join(d, 'b.elf'), os.path.join(d, 'b.bin')
    open(src, 'w').write(c)
    subprocess.run(['arm-vita-eabi-gcc', '-mthumb', '-O2', '-ffreestanding', '-nostdlib', '-nostartfiles', '-static',
                    '-Wl,-Ttext=0x%x' % BLOB, '-Wl,-e,entry', src, '-o', elf], check=True)
    subprocess.run(['arm-vita-eabi-objcopy', '-O', 'binary', elf, binf], check=True)
    nm = subprocess.run(['arm-vita-eabi-nm', elf], capture_output=True, text=True, check=True).stdout
    return open(binf, 'rb').read(), [int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' entry')][0]

def run_blob(emu, entry, mode, fn=0):
    uc = emu.uc
    uc.reg_write(UC_ARM_REG_R0, mode); uc.reg_write(UC_ARM_REG_R1, fn)
    uc.reg_write(UC_ARM_REG_R4, 0x4444)
    uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000); uc.reg_write(UC_ARM_REG_LR, END | 1)
    uc.emu_start(entry | 1, END, count=2000000)
    return uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R4)

def main():
    emu = Emu(sys.argv[1])
    uc = emu.uc
    blob, entry = build_blob()
    uc.mem_map(BLOB, 0x30000); uc.mem_write(BLOB, blob)  # .bss lands at +0x20000
    ok, r4 = run_blob(emu, entry, 0, B + ALLOC | 1)
    if ok != 1:
        print('setup refused the function'); sys.exit(1)
    orig_code = bytes(uc.mem_read(B + ALLOC, 8))
    T, PT, P, VB = XVA + 0x22000, XVA + 0x22800, XVA + 0x22400, XVA + 0x24000
    random.seed(3)
    bad = 0
    total = 400
    for it in range(total):
        emu.wva(T, bytes(0x100))
        emu.wva(T + 0x30, struct.pack('<I', PT)); emu.wva(PT, struct.pack('<I', 3))
        emu.wva(T + 0x54, struct.pack('<I', 5)); emu.wva(T + 0x50, struct.pack('<i', random.randrange(4, 100)))
        emu.wva(T + 0x8c, struct.pack('<I', P)); emu.wva(T + 0x80, struct.pack('<I', VB))
        for o in (0x38, 0x3c, 0x40, 0x44, 0x4c, 0x84, 0x88): emu.wva(T + o, struct.pack('<I', random.getrandbits(16)))
        args = [random.randrange(1, 4), 0 if it % 5 == 4 else T]
        regs0 = [random.getrandbits(32) for _ in range(8)]
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        state = bytes(uc.mem_read(XH, 0x200000))
        uc.mem_write(B + ALLOC, orig_code)
        esp = emu.call_x86(ALLOC, args)
        o_x = bytes(uc.mem_read(XH, 0x200000)); o_r = [emu.reg(i) for i in range(8)]
        # as hooked: the first 8 bytes are not the function's any more
        uc.mem_write(XH, state)
        for i in (0, 1, 2, 3, 5, 6, 7): emu.setreg(i, regs0[i])
        uc.mem_write(B + ALLOC, b'\x00\xde' * 4)  # udf
        sp = XVA + 0x10000 - 4 * len(args)
        for i, a in enumerate(args): emu.wva(sp + 4 * i, struct.pack('<I', a))
        emu.wva(sp - 4, struct.pack('<I', 0xdeadbeef)); emu.setreg(4, sp - 4)
        _, r4_after = run_blob(emu, entry, 1)
        n_x = bytes(uc.mem_read(XH, 0x200000)); n_r = [emu.reg(i) for i in range(8)]
        problems = []
        if n_x != o_x: problems.append('memory differs')
        if n_r != o_r: problems.append('regs %s / %s' % (n_r, o_r))
        if r4_after != 0x4444: problems.append('r4 not preserved (%x)' % r4_after)
        if problems:
            bad += 1
            if bad <= 5: print('MISMATCH call %d: %s' % (it, '; '.join(problems)))
    print('0x66E272 through the trampoline: %d mismatches in %d calls' % (bad, total))
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
