#!/usr/bin/env python3
"""Compare the compiled native conversions/_ftol with the original Android ARM.

Usage: source scripts/env.sh && python3 scripts/emu/test_ftol.py build/libjni_ff7.so [cases]
Requires Unicorn. Tests the C header compiled for Cortex-A9, not a Python
reimplementation. Checks the whole x86 stack region, register/flag block,
x87 state, and state at _check_event (including an event that changes ESP).
"""
import pathlib
import random
import struct
import subprocess
import sys
import tempfile

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR
from ff7emu import Emu, B, FPU, REGS, XVA, END, HSTACK, CHECK_EVENT

BLOB = 0x58000000
FTOL, UNSIGNED, SIGNED = 0xafcd74, 0xf1f53c, 0xf1f4f8
ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 12000
    rng = random.Random(40_470)
    with tempfile.TemporaryDirectory(prefix='ff7-ftol-') as temp:
        elf, binary = pathlib.Path(temp) / 'blob.elf', pathlib.Path(temp) / 'blob.bin'
        subprocess.run(['arm-vita-eabi-gcc', '-mcpu=cortex-a9', '-mthumb', '-mfpu=neon',
                        '-mfloat-abi=softfp', '-O3', '-ffast-math', '-ffreestanding',
                        '-fno-tree-loop-distribute-patterns', '-nostdlib', '-nostartfiles',
                        '-static', f'-Wl,-Ttext={BLOB:#x}', '-Wl,-e,entry',
                        '-I', str(ROOT / 'source'), str(ROOT / 'scripts/emu/blob_ftol.c'),
                        '-lgcc', '-o', str(elf)], check=True)
        subprocess.run(['arm-vita-eabi-objcopy', '-O', 'binary', str(elf), str(binary)], check=True)
        nm = subprocess.check_output(['arm-vita-eabi-nm', str(elf)], text=True)
        symbols = {p[2]: int(p[0], 16) | 1 for line in nm.splitlines()
                   if len(p := line.split()) == 3 and p[1] in ('t', 'T')}
        emu = Emu(sys.argv[1])
        uc = emu.uc
        uc.mem_map(BLOB, 0x10000)
        uc.mem_write(BLOB, binary.read_bytes())

    def run(address, args=()):
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            uc.reg_write(reg, value)
        uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000)
        uc.reg_write(UC_ARM_REG_LR, END | 1)
        uc.emu_start(address, END, count=20000)
        return uc.reg_read(UC_ARM_REG_R0) | uc.reg_read(UC_ARM_REG_R1) << 32

    # Both sides of every binary exponent transition, plus arbitrary payloads
    # (including signaling/quiet NaNs) and typical game coordinate magnitudes.
    inputs = []
    for exp in range(2048):
        for frac in (0, 1, (1 << 52) - 1):
            inputs.extend((exp << 52 | frac, 1 << 63 | exp << 52 | frac))
    inputs += [rng.getrandbits(64) for _ in range(n)]
    inputs += [struct.unpack('<Q', struct.pack('<d', rng.uniform(-1e7, 1e7)))[0] for _ in range(n // 4)]
    for i, bits in enumerate(inputs):
        args = (bits & 0xffffffff, bits >> 32)
        for original, native in ((UNSIGNED, 'unsigned_entry'), (SIGNED, 'signed_entry')):
            want = run(B + original, args)  # original helpers are ARM, not Thumb
            got = run(symbols[native], args)
            if got != want:
                raise AssertionError(f'{native} case {i}: input {bits:016x}: {got:016x} != {want:016x}')
    print(f'conversion helpers: {len(inputs)} inputs each, signed and unsigned identical', flush=True)

    stack_va = XVA + 0xf000
    events, event_delta = [], [0]

    def snapshot():
        return (bytes(uc.mem_read(B + REGS, 0x30)), bytes(uc.mem_read(B + FPU, 0x68)),
                emu.rva(stack_va, 0x3000))

    def event(uc_, address, size, _):
        events.append(snapshot())
        # Original reads ESP again after the event. Guard against hoisting
        # the final return increment above the callback in the replacement.
        emu.setreg(4, (emu.reg(4) + event_delta[0]) & 0xffffffff)

    uc.hook_add(UC_HOOK_CODE, event, begin=B + CHECK_EVENT, end=B + CHECK_EVENT + 1)
    for i in range(n):
        regs = bytearray(rng.randbytes(0x30))
        fpu = bytearray(rng.randbytes(0x68))
        # Cover aligned/unaligned frames and frames spanning page boundaries.
        esp = XVA + 0x10000 + (i % 33 - 16)
        top = i % 8
        struct.pack_into('<I', regs, 16, esp)
        struct.pack_into('<I', fpu, 0x60, top)
        bits = inputs[(i * 137) % len(inputs)]
        struct.pack_into('<Q', fpu, top * 8, bits)
        stack = rng.randbytes(0x3000)
        event_delta[0] = 8 if i % 17 == 0 else 0

        def reset():
            uc.mem_write(B + REGS, bytes(regs))
            uc.mem_write(B + FPU, bytes(fpu))
            emu.wva(stack_va, stack)
            events.clear()

        reset()
        run(B + FTOL | 1)
        want, want_events = snapshot(), list(events)
        reset()
        run(symbols['entry'], (B + REGS, B + FPU, emu.host(esp - 16), B + CHECK_EVENT | 1))
        got = snapshot()
        if got != want or events != want_events or len(events) != 1:
            for label, a, b in zip(('registers/flags', 'x87', 'stack'), got, want):
                if a != b:
                    off = next(j for j in range(len(a)) if a[j] != b[j])
                    print(f'{label}: first difference at +{off:#x}: {a[off:off+8].hex()} != {b[off:off+8].hex()}')
            raise AssertionError(f'_ftol case {i}, bits {bits:016x}, top {top}, event counts {len(events)}/{len(want_events)}')
    if emu.unmapped:
        raise AssertionError(f'unmapped x86 addresses: {emu.unmapped}')
    print(f'_ftol: {n} states identical (stack, registers/flags, full x87, event state and return order)')


if __name__ == '__main__':
    main()
