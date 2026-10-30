#!/usr/bin/env python3
"""Check the second tile layer against the original ARM routine.
Expanded culling must add edge tiles without moving the original tiles.
Run from repo root; requires Unicorn and build/libjni_ff7.so.
"""
import struct
from ff7emu import Emu, B, MEMMAP


def run(wide):
    e=Emu('build/libjni_ff7.so',resolve_defined=True)
    uc=e.uc
    pages=(0xcc0,0xcff,0xd00)
    uc.mem_map(0x52000000,0x1000*len(pages))
    for i,page in enumerate(pages): e.w32(B+MEMMAP+4*page,0x52000000+4096*i)
    def host(va):
        if va>>12 in pages: return 0x52000000+4096*pages.index(va>>12)+(va&4095)
        return e.host(va)
    def w(va,data): uc.mem_write(host(va),data)
    def w32(va,value): w(va,struct.pack('<I',value))
    tiles,index,lists=0x140000,0x130000,0x180000
    xs=(-60,-40,-16,0,160,319,320,340,358,380)
    w32(0xcff1f0,2)
    w32(0xcff3f8,index)
    w32(0xcffe54,tiles)
    w(0xcffe5c,struct.pack('<H',len(xs)))
    w32(0xd00050,lists)
    for i,x in enumerate(xs):
        w(index+4*i,struct.pack('<I',i))
        w(tiles+0x105c*i,struct.pack('<hh',x,32))
    if wide:
        uc.mem_write(B+0x2756a2,struct.pack('<HH',0xf5c3,0x72b3))
        uc.mem_write(B+0x275942,struct.pack('<HH',0xf5a0,0x70ce))
    e.call_x86(0x275650,[320+(38 if wide else 0),224])
    n=struct.unpack('<I',e.rva(lists,4))[0]
    return [struct.unpack('<ff',e.rva(lists+4+24*i,8)) for i in range(n)]

old=run(False)
new=run(True)
assert old==[(0,64),(320,64),(638,64)],old
assert new==[(-80,64),(-32,64),(0,64),(320,64),(638,64),(640,64),(680,64)],new
assert all(p in new for p in old)
print('second field layer: original ARM adds both edge regions and preserves every central tile position')
