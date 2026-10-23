#!/usr/bin/env python3
"""
Prints every thread of a PS Vita core dump (psp2core-*.psp2dmp, gzip'd ELF):
name, uid, and pc / lr / sp / cpsr from THREAD_REG_INFO. Map an eboot pc with
`arm-vita-eabi-addr2line -f -i -e build/ff7_vita <pc>` (same build only).

Usage: scripts/parse_core.py build/VPK/psp2core-....psp2dmp
"""
import gzip,struct,sys,re
d=gzip.open(sys.argv[1]).read()
e_phoff=struct.unpack_from('<I',d,0x1c)[0]; e_phnum=struct.unpack_from('<H',d,0x2c)[0]
notes={}
for i in range(e_phnum):
    t,off,va,pa,fs,ms,fl,al=struct.unpack_from('<8I',d,e_phoff+32*i)
    if t==4:
        p=off
        while p<off+fs:
            nsz,dsz,typ=struct.unpack_from('<3I',d,p); name=d[p+12:p+12+nsz].rstrip(b'\0').decode()
            notes[name]=d[p+12+((nsz+3)&~3):p+12+((nsz+3)&~3)+dsz]
            p+=12+((nsz+3)&~3)+((dsz+3)&~3)
ti=notes['THREAD_INFO']
unk,num=struct.unpack_from('<II',ti,0); p=8
threads={}
for i in range(num):
    size=struct.unpack_from('<I',ti,p)[0]; e=ti[p:p+size]
    uid=struct.unpack_from('<I',e,4)[0]; name=e[8:40].split(b'\0')[0].decode('latin1')
    words=struct.unpack_from('<%dI'%(size//4),e)
    threads[uid]=name
    stop=[ (k,hex(w)) for k,w in enumerate(words) if k in range(10,40)]
    print('%08x %-28s' % (uid,name), ' '.join('%x'%w for w in words[10:40]))
    p+=size
tr=notes['THREAD_REG_INFO']
unk,num=struct.unpack_from('<II',tr,0); p=8
print('--- regs')
for i in range(num):
    size=struct.unpack_from('<I',tr,p)[0]; e=tr[p:p+size]
    uid=struct.unpack_from('<I',e,4)[0]
    regs=struct.unpack_from('<17I',e,8)
    print('%08x %-28s pc %08x lr %08x sp %08x cpsr %08x' % (uid, threads.get(uid,'?'), regs[15], regs[14], regs[13], regs[16]))
    p+=size
