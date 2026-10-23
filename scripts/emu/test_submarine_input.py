#!/usr/bin/env python3
"""Check the actual input/fire trampolines and Square -> Delete -> fire mapping.
Usage: test_submarine_input.py libjni_ff7.so ff7_en.exe original.obb
The trampoline assembly is extracted from ff7_submarine_input.c, compiled
as ARM, and compared with each original prologue under Unicorn.
"""
import os, re, struct, subprocess, sys, tempfile, zipfile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import *
from ff7emu import Emu, B, XVA, HSTACK, END, MEMMAP
from test_world_tri import pe_reader

so, exe, obb = sys.argv[1:4]
e = Emu(so, resolve_defined=True)
u = e.uc
blob, globals_va = 0x58000000, 0x59000000
u.mem_map(blob, 0x10000); u.mem_map(globals_va, 0x1000)
source_dir = os.path.join(os.path.dirname(__file__), '..', '..', 'source')
source = open(os.path.join(source_dir, 'ff7_submarine_input.c')).read()
# Exercise the actual queue functions with a game-thread consumer stub.
def function(name):
    match = re.search(r'(?:static )?(?:uint32_t \* |void |int )' + name + r'\([^)]*\) \{', source)
    start, depth, pos = match.start(), 1, match.end()
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]
queue_code = r"""
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
static uint8_t * const * pages;
static volatile unsigned pending;
static unsigned presses, delivered;
static int installed = 1;
static unsigned seen, shots;
static uint32_t * word(uint32_t va);
static void input_original(void) {
    seen = *word(0x9a85e0);
    if (seen & 0x80) shots++;
}
""" + '\n'.join(function(n) for n in ('word', 'input_native', 'ff7_submarine_input_poll', 'ff7_submarine_input_key'))
queue_code += r"""
int main(void) {
    static uint8_t * mappings[4096];
    static uint32_t mode[1024], input[1024];
    pages = mappings; mappings[0xcbf] = (uint8_t *) mode; mappings[0x9a8] = (uint8_t *) input;
    *word(0xcbf9dc) = 10; *word(0x9a85e0) = 0x10;
    assert(ff7_submarine_input_key(1) == 1); ff7_submarine_input_key(0);
    input_native(); assert(shots == 1 && seen == 0x90 && *word(0x9a85e0) == 0x10);
    for (int i = 0; i < 8; i++) input_native();
    assert(shots == 1); // a short press survives release; holding does not duplicate
    for (int i = 0; i < 3; i++) { ff7_submarine_input_key(1); ff7_submarine_input_key(0); }
    for (int i = 0; i < 3; i++) input_native();
    assert(shots == 4 && pending == 0 && delivered == 4);
    *word(0x9a85e0) = 0x80; input_native(); assert(shots == 5); // virtual input preserved
    ff7_submarine_input_key(1); *word(0xcbf9dc) = 1; ff7_submarine_input_poll();
    assert(pending == 0 && ff7_submarine_input_key(1) == 0);
    installed = 0; assert(ff7_submarine_input_key(1) == 0);
    return 0;
}
"""
with tempfile.TemporaryDirectory() as directory:
    cpath, test = os.path.join(directory, 'queue.c'), os.path.join(directory, 'queue')
    open(cpath, 'w').write(queue_code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', cpath, '-o', test], check=True)
    subprocess.run([test], check=True)
print('actual Square queue: short tap, three rapid taps, no duplicates, virtual input, mode exit: passed')

text = '#include <stdint.h>\nuintptr_t ff7_sm_input_regs, ff7_sm_input_resume, ff7_sm_fire_resume;\n'
for name in ('input_original', 'fire_original'):
    fn = re.search(r'__attribute__\(\(naked\)\) static void ' + name + r'\(void\) \{.*?\n\}', source, re.S)[0]
    text += fn.replace('static void', '__attribute__((used)) void') + '\n'
with tempfile.TemporaryDirectory() as directory:
    path = os.path.join(directory, 'test.c'); open(path, 'w').write(text)
    elf, binary = path + '.elf', path + '.bin'
    subprocess.run(['arm-vita-eabi-gcc', '-mthumb', '-mfloat-abi=softfp', '-O2', '-nostdlib',
                    '-Wl,-Ttext=0x58000000', '-Wl,-Tbss=0x59000000', '-Wl,-e,input_original', path, '-o', elf], check=True)
    subprocess.run(['arm-vita-eabi-objcopy', '-O', 'binary', elf, binary], check=True)
    syms = subprocess.check_output(['arm-vita-eabi-nm', elf], text=True)
    symbols = {line.split()[-1]: int(line.split()[0], 16) for line in syms.splitlines() if len(line.split()) == 3}
    data = open(binary, 'rb').read(); u.mem_write(blob, data)
    regs = B + 0x1492c48
    e.w32(symbols['ff7_sm_input_regs'], regs)
    for name, original in [('input_original', 0x6d2dfc), ('fire_original', 0x6d1f8c)]:
        resume = symbols['ff7_sm_input_resume' if name == 'input_original' else 'ff7_sm_fire_resume']
        e.w32(resume, (B + original + 8) | 1)
        def initial():
            for r in range(UC_ARM_REG_R0, UC_ARM_REG_R12 + 1): u.reg_write(r, 0x12340000 + r)
            u.reg_write(UC_ARM_REG_SP, HSTACK + 0x8000)
            u.reg_write(UC_ARM_REG_LR, END | 1)
        def snapshot():
            return ([u.reg_read(r) for r in range(UC_ARM_REG_R0, UC_ARM_REG_R11 + 1)] +
                    [u.reg_read(UC_ARM_REG_SP), u.reg_read(UC_ARM_REG_LR)],
                    bytes(u.mem_read(HSTACK + 0x7fc0, 0x40)))
        initial(); u.emu_start((B + original) | 1, B + original + 8, count=100)
        expected = snapshot()
        initial(); u.emu_start(symbols[name] | 1, B + original + 8, count=100)
        assert snapshot() == expected, name
        assert u.reg_read(UC_ARM_REG_R4) == regs
        print(name + ': registers and saved ARM stack identical')

# Drive the original Android input routines through their real GOT pointers.
for button in range(14):
    u.reg_write(UC_ARM_REG_R0, button); u.reg_write(UC_ARM_REG_R1, 0)
    u.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000); u.reg_write(UC_ARM_REG_LR, END | 1)
    u.emu_start(B + 0x952c2c | 1, END, count=1000)
u.reg_write(UC_ARM_REG_R0, 6); u.reg_write(UC_ARM_REG_R1, 1)
u.reg_write(UC_ARM_REG_LR, END | 1); u.emu_start(B + 0x952c2c | 1, END, count=1000)
u.reg_write(UC_ARM_REG_LR, END | 1); u.emu_start(B + 0x952d58 | 1, END, count=1000)
keyboard = bytes(u.mem_read(B + 0xf95480, 256))
assert [i for i, v in enumerate(keyboard) if v] == [0x53]
with zipfile.ZipFile(obb) as archive: cfg = archive.read('assets/ff7_1.02/ff7input.cfg')
bindings = struct.unpack_from('<25I', cfg, 1)
mask = sum(1 << i for i, scan in enumerate(bindings) if scan < 256 and keyboard[scan] & 0x80)
assert mask == 0x80, hex(mask)
print('Square (6) -> original Android Delete (0x53) -> configured action 0x80: passed')
# The original submarine tests that exact edge immediately before calling fire.
page = pe_reader(exe)
assert page(0x798)[0x604:0x60e] == bytes.fromhex('6880000000e86625c8ff')
print('original submarine fire-edge check at 0x798604: verified')
