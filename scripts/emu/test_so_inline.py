#!/usr/bin/env python3
"""
Runs source/ff7_so_inline.h's lookup (compiled into an ARM blob, with a
stand-in so_module) on the loaded libjni_ff7.so and checks it finds
mk_address's page table and _check_event's counter. The v0.6.13 lookup got
the PC of `add r3, pc` wrong: every native that inlines mk_address turned
itself off on the Vita while the other emulator tests (which hand the
pointers in) passed.

Usage: scripts/emu/test_so_inline.py libjni_ff7.so
Needs arm-vita-eabi-gcc on PATH (source scripts/env.sh) and `pip install unicorn`.
"""
import os, struct, subprocess, sys, tempfile
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR
from ff7emu import Emu, B, END, HSTACK, MEMMAP, EVCOUNT, CHECK_EVENT

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'source')
BLOB, OUT = 0x58000000, 0x59000000
C = r'''
#include "ff7_so_inline.h"
__attribute__((section(".text.entry"), used)) int entry(uint32_t base, uint32_t * out) {
    so_module m = { base };
    uint8_t * const * mm = 0;
    uint32_t * ev = 0;
    int r = ff7_so_inline_lookup(&m, base + 0x93d714 + 1, base + %d + 1, &mm, &ev);
    out[0] = (uint32_t) mm;
    out[1] = (uint32_t) ev;
    return r;
}
'''

def main():
    d = tempfile.mkdtemp()
    os.makedirs(os.path.join(d, 'so_util'))
    open(os.path.join(d, 'so_util', 'so_util.h'), 'w').write(
        '#include <stdint.h>\ntypedef struct { uintptr_t text_base; } so_module;\n')
    open(os.path.join(d, 'blob.c'), 'w').write(C % CHECK_EVENT)
    elf, binf = os.path.join(d, 'blob.elf'), os.path.join(d, 'blob.bin')
    subprocess.run(['arm-vita-eabi-gcc', '-mthumb', '-O2', '-ffreestanding', '-nostdlib', '-nostartfiles', '-static',
                    '-Wl,-Ttext=0x%x' % BLOB, '-Wl,-e,entry', '-I', d, '-I', SRC, os.path.join(d, 'blob.c'), '-o', elf],
                   check=True)
    subprocess.run(['arm-vita-eabi-objcopy', '-O', 'binary', elf, binf], check=True)
    emu = Emu(sys.argv[1])
    uc = emu.uc
    uc.mem_map(BLOB, 0x10000); uc.mem_write(BLOB, open(binf, 'rb').read()); uc.mem_map(OUT, 0x1000)
    uc.reg_write(UC_ARM_REG_R0, B); uc.reg_write(UC_ARM_REG_R1, OUT)
    uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000); uc.reg_write(UC_ARM_REG_LR, END | 1)
    uc.emu_start(BLOB | 1, END, count=10000)
    r = uc.reg_read(UC_ARM_REG_R0)
    mm, ev = struct.unpack('<2I', bytes(uc.mem_read(OUT, 8)))
    ok = r == 1 and mm == B + MEMMAP and ev == B + EVCOUNT
    print('lookup %s: returned %d, page table 0x%x (expected 0x%x), counter 0x%x (expected 0x%x)'
          % ('ok' if ok else 'WRONG', r, mm, B + MEMMAP, ev, B + EVCOUNT))
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
