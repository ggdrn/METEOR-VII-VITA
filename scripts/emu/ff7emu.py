"""
Runs recompiled ff7.exe functions of libjni_ff7.so (v1.0.29) under Unicorn,
to check native replacements against the original code on a PC.

The .so is loaded with its relocations applied; x86 memory is a host buffer
mapped through the game's own page table (MEMMAP), so mk_address and the
recompiled code run unmodified. _check_event() is stubbed out. Calls to
imported functions are not supported (they jump to an unmapped trap).

Requires: pip install unicorn
"""
import struct
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_ARM, UC_HOOK_CODE, UcError
from unicorn.arm_const import *

B = 0x10000000          # .so base
IMG = 0x1500000
XH = 0x40000000         # host memory backing x86 VAs
XVA = 0x00100000        # first x86 VA we map
XSIZE = 0x200000
HSTACK = 0x60000000     # ARM stack
END = 0x70000000        # return address sentinel
MEMMAP = 0x101de5c
REGS = 0x1492c48
FPU = 0x1492bc0
CHECK_EVENT = 0xadb9f4

class Emu:
    def __init__(self, so_path):
        d = open(so_path, 'rb').read()
        self.d = d
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        uc.mem_map(B, IMG)
        # PT_LOADs: text [0, 0xf49604) file 0; data vaddr 0xf4afa0 file 0xf49fa0 filesz 0x48f3c
        uc.mem_write(B, d[:0xf49604])
        uc.mem_write(B + 0xf4afa0, d[0xf49fa0:0xf49fa0 + 0x48f3c])
        self.uc = uc
        self._relocate()
        uc.mem_map(XH, XSIZE)
        uc.mem_map(HSTACK, 0x100000)
        uc.mem_map(END, 0x1000)
        # VFP on
        uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xf << 20))
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        for i in range(XSIZE // 0x1000):
            self.w32(B + MEMMAP + 4 * ((XVA >> 12) + i), XH + i * 0x1000)
        uc.hook_add(UC_HOOK_CODE, self._stub, begin=B + CHECK_EVENT, end=B + CHECK_EVENT + 1)
        uc.hook_add(UC_HOOK_CODE, self._mk, begin=B + 0x93d714, end=B + 0x93d715)
        self.calls = 0
        self.unmapped = set()
        # ff7.exe constants used by the lighting code (page 0x7b7000): 255.0f at 0x7b7b44
        uc.mem_map(0x50000000, 0x1000)
        self.w32(B + MEMMAP + 4 * 0x7b7, 0x50000000)
        self.uc.mem_write(0x50000000 + 0xb44, struct.pack('<f', 255.0))

    def _relocate(self):
        d = self.d
        # .rel.dyn at 0x95994 size 0x18f98, .rel.plt 0xae92c size 0x560
        for off, size in ((0x95994, 0x18f98), (0xae92c, 0x560)):
            for i in range(0, size, 8):
                r_off, r_info = struct.unpack_from('<II', d, off + i)
                typ = r_info & 0xff
                if typ == 0x17:  # RELATIVE
                    v = struct.unpack_from('<I', self.uc.mem_read(B + r_off, 4))[0]
                    self.w32(B + r_off, (v + B) & 0xffffffff)
                elif typ in (0x15, 0x16, 0x02):  # GLOB_DAT, JUMP_SLOT, ABS32 -> trap
                    self.w32(B + r_off, END + 0x100)

    def _mk(self, uc, addr, size, user):
        va = uc.reg_read(UC_ARM_REG_R0)
        if not self.r32(B + MEMMAP + 4 * (va >> 12)):
            self.unmapped.add(va)

    def _stub(self, uc, addr, size, user):
        # _check_event(): return
        lr = uc.reg_read(UC_ARM_REG_LR)
        uc.reg_write(UC_ARM_REG_PC, lr)

    def w32(self, a, v): self.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def r32(self, a): return struct.unpack('<I', self.uc.mem_read(a, 4))[0]
    def host(self, va): return XH + (va - XVA)
    def wva(self, va, data): self.uc.mem_write(self.host(va), bytes(data))
    def rva(self, va, n): return bytes(self.uc.mem_read(self.host(va), n))
    def reg(self, i): return self.r32(B + REGS + 4 * i)
    def setreg(self, i, v): self.w32(B + REGS + 4 * i, v)

    def call_x86(self, func_off, args, esp=XVA + 0x10000):
        """cdecl call: pushes args and a fake return address, runs the function."""
        esp -= 4 * len(args)
        for i, a in enumerate(args):
            self.wva(esp + 4 * i, struct.pack('<I', a))
        esp -= 4
        self.wva(esp, struct.pack('<I', 0xdeadbeef))
        self.setreg(4, esp)
        uc = self.uc
        uc.reg_write(UC_ARM_REG_SP, HSTACK + 0xff000)
        uc.reg_write(UC_ARM_REG_LR, END | 1)
        uc.emu_start(B + func_off | 1, END, count=2000000)
        return esp
