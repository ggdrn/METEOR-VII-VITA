#!/usr/bin/env python3
"""Validate rate consumers in Android 1.0.29's original ARM code.

Usage: python3 scripts/emu/test_encounter_rate.py build/libjni_ff7.so
Requires Unicorn. The host UI test separately exercises the real C wrappers.
"""
import random
import struct
import sys
from ff7emu import Emu, B, MEMMAP, REGS, XVA
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_PC, UC_ARM_REG_LR

emu = Emu(sys.argv[1])
uc = emu.uc
HOST = 0x51000000
uc.mem_map(HOST, 0x2000)
for index, page in enumerate((0xdbc, 0xcc1)):
    emu.w32(B + MEMMAP + 4 * page, HOST + index * 0x1000)

RATE = HOST + 0xad9
DANGER = HOST + 0x1000 + 0x668
WORLD = 0x65bb6c  # func_0x00767c55
FIELD_COMPARE = 0x20809c
# Start at the field's final probability computation, after walking/script
# checks and danger accumulation. Stop at its comparison, before battle setup.
FIELD_RATE = 0x20805e
captured = []

def compare(uc, addr, size, user):
    captured.append((uc.reg_read(UC_ARM_REG_R2), uc.reg_read(UC_ARM_REG_R1)))
    uc.emu_stop()

handle = uc.hook_add(UC_HOOK_CODE, compare, begin=B + FIELD_COMPARE, end=B + FIELD_COMPARE)
for rate in (0, 1, 8, 16, 24, 32, 64, 128, 255):
    uc.mem_write(RATE, bytes([rate]))
    emu.setreg(5, 0x12345678)
    esp = emu.call_x86(WORLD, [])
    assert emu.reg(0) == rate * (2 if rate > 16 else 1), (rate, emu.reg(0))
    assert emu.reg(4) == esp + 4 and emu.reg(5) == 0x12345678
    assert bytes(uc.mem_read(RATE, 1)) == bytes([rate])

    for danger in (0, 1, 4095, 4096, 12345, 65535):
        uc.mem_write(DANGER, struct.pack('<H', danger))
        ebp = XVA + 0x18000
        emu.setreg(5, ebp)
        emu.wva(ebp - 16, b'\x7f')
        uc.reg_write(UC_ARM_REG_R4, B + REGS)
        uc.reg_write(UC_ARM_REG_R5, 0)
        emu.call_x86(FIELD_RATE, [])
        assert captured[-1] == (danger * rate >> 12, 127), (rate, danger, captured[-1])
assert len(captured) == 54
print('encounter ARM: original field probability and world Enemy Lure/Away rates verified (63 cases)')

# Exercise the original world caller too: rate -> division by terrain ->
# danger accumulation -> random comparison. The earlier test of the helper
# alone cannot establish how many encounters a multiplier produces.
u = uc
u.mem_map(HOST + 0x2000, 0x1000)
emu.w32(B + MEMMAP + 4 * 0xe3a, HOST + 0x2000)
WORLD_DANGER = HOST + 0x2888
factor = 1
rng = random.Random(42)
world_checks = []

def scale_world_return(uc, addr, size, user):
    emu.setreg(0, emu.reg(0) * factor)

def world_random(uc, addr, size, user):
    emu.setreg(0, rng.randrange(256))
    emu.setreg(4, emu.reg(4) + 4)
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

def world_compare(uc, addr, size, user):
    world_checks.append((uc.reg_read(UC_ARM_REG_R1), uc.reg_read(UC_ARM_REG_R3), emu.r32(WORLD_DANGER)))
    uc.emu_stop()

u.hook_add(UC_HOOK_CODE, scale_world_return, begin=B + 0x65c2f2, end=B + 0x65c2f2)
u.hook_add(UC_HOOK_CODE, world_random, begin=B + 0x616b1c, end=B + 0x616b1c)
u.hook_add(UC_HOOK_CODE, world_compare, begin=B + 0x65c3a8, end=B + 0x65c3a8)
u.mem_write(RATE, b'\x10')
counts = {}
for factor in (1, 2, 4):
    emu.w32(WORLD_DANGER, 0)
    rng.seed(42)
    battles = 0
    ebp = XVA + 0x18000
    for tick in range(10000):
        emu.setreg(5, ebp)
        emu.wva(ebp - 16, struct.pack('<I', 16))  # same terrain in all three runs
        u.reg_write(UC_ARM_REG_R4, B + REGS)
        u.reg_write(UC_ARM_REG_R5, 0)
        emu.call_x86(0x65c2e8, [])
        sample, threshold, danger = world_checks[-1]
        if tick == 0:
            assert danger == 1024 * factor and threshold == 4 * factor
        if sample < threshold:
            battles += 1
            emu.w32(WORLD_DANGER, 0)  # model battle return; battle setup is outside this slice
    counts[factor] = battles
assert counts[1] < counts[2] < counts[4]
# This reports measured frequency, deliberately not an assertion of 2x/4x fights.
print('world ARM, 10000 identical checks per factor: ' + ', '.join(
    '%dx = %d encounters (%.2fx normal)' % (f, n, n / counts[1]) for f, n in counts.items()))
