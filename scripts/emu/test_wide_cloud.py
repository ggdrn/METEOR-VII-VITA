#!/usr/bin/env python3
"""Validate the cloud hook's TL buffer layout against the original APK routine.
Requires Unicorn and build/libjni_ff7.so. Stubs only allocation and the world
renderer capability check; coordinate, UV and vertex conversion run unmodified.
Run from the repository root: python3 scripts/emu/test_wide_cloud.py
"""
import sys, struct
from pathlib import Path
from ff7emu import Emu, B, MEMMAP, REGS
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC
emu=Emu('build/libjni_ff7.so', resolve_defined=True)
uc=emu.uc
prim,buf,verts=0x110000,0x111000,0x112000
uc.mem_map(0x52000000,0x1000)
emu.w32(B+MEMMAP+4*0xde6,0x52000000)
uc.mem_write(0x52000000+0x9d8,struct.pack('<I',2))
def w(a,v): emu.wva(a,struct.pack('<I',v))
w(prim+0x28,buf)
for i,(x,y) in enumerate(((0,0),(320,0),(0,64),(320,64))):
    emu.wva(prim+8+8*i,struct.pack('<hhBBBB',x,y,0,0,0,0))
w(buf+0x24,0x3b800000)
w(buf+0x28,0x3b800000)
def stub(uc,addr,size,data):
    esp=emu.reg(4)
    if addr==B+0xc50ab4:
        argbuf=struct.unpack('<I',emu.rva(esp+8,4))[0]
        assert argbuf==buf
        w(buf+0x70,verts)
        w(buf+0x80,verts+128)
        w(buf+0x40,128)
        w(buf+0x4c,1)
    emu.setreg(0,1)
    emu.setreg(4,esp+4)
    uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
for off in (0xc50ab4,0x608990): uc.hook_add(UC_HOOK_CODE,stub,begin=B+off,end=B+off+1)
emu.call_x86(0x619b30,[prim])
assert struct.unpack('<I',emu.rva(buf+0x70,4))[0]==verts
assert sorted(struct.unpack('<f',emu.rva(verts+32*i,4))[0] for i in range(4))==[0,0,640,640]
print('Cloud converter: actual .so confirms four TL vertices at buffer+0x70, stride 32')
