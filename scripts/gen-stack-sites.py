#!/usr/bin/env python3
"""
Generates source/ff7_stack_sites.bin: the mk_address() call sites of
libjni_ff7.so (v1.0.29) whose address is on the x86 stack (see
source/ff7_stack.c).

The recompiled ff7.exe keeps the x86 registers in g_cpu_regs (ESP at +16,
EBP at +20) and reaches stack memory through, for example,

    ldr   r0, [r4, #16]      ; r4 = g_cpu_regs, loaded from its GOT slot
    subs  r0, #4
    bl    mk_address

A site is listed when, inside one basic block, r0 at the `bl` is provably
ESP + c or EBP + c (|c| <= 0x8000), through ldr from the register slot,
add/sub of immediates and register moves. Rules:
  - only recompiled functions (_Z15func_0x... / _Z10C_00...), without
    computed jumps (tbb/tbh, bx to a register other than lr);
  - the register holding g_cpu_regs is only ever written by the
    literal / add pc / ldr sequence that loads it, so it keeps that value
    across branch targets; every other tracked value is dropped at branch
    targets and after calls (r0-r3, r12, lr);
  - sites inside IT blocks are skipped;
  - ESP sites are always on the stack;
  - EBP sites are listed only for functions where EBP is a frame pointer:
    the first write to EBP is ESP/EBP + c in the entry block, before any
    branch, branch target or EBP site, and every other write to EBP (the
    `pop ebp` / `leave` of an epilogue) is followed by the function exit with
    no EBP site, branch or branch target in between. Callees preserve EBP
    (x86 calling convention).

Usage: scripts/gen-stack-sites.py libjni_ff7.so source/ff7_stack_sites.bin
(uses $OBJDUMP, default arm-vita-eabi-objdump, GNU syntax).

Format (little endian): "STK1", u32 count, then per site, in address order:
varint(((bl - previous bl) / 2) << 1 | kind), kind 0 = ESP, 1 = EBP.
Offsets are relative to the .so text base; the first "previous bl" is 0.
"""
import collections
import os
import re
import struct
import subprocess
import sys

MK_ADDRESS = 0x93d714
REGS_GOT = 0xf4c828      # GOT slot holding &g_cpu_regs
ESP_OFF, EBP_OFF = 16, 20
LIMIT = 0x8000

INS = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{4} ?){1,2})\s+(\S+)\s*(.*)$')
FUNC = re.compile(r'^([0-9a-f]{8}) <(.*)>:$')
RECOMPILED = re.compile(r'^_Z15func_0x|^_Z10C_00')
REG = re.compile(r'\b(r\d+|sl|fp|ip|lr|sb)\b')
ALIAS = {'sb': 'r9', 'sl': 'r10', 'fp': 'r11', 'ip': 'r12'}
STORE = re.compile(r'^(str|stm|push|vst|vpush)')
NOWRITE = re.compile(r'^(cmp|cmn|tst|teq|vcmp|nop|bx|cbz|cbnz|tbb|tbh|udf|dmb|dsb|isb|pld|pli)')
COND = 'eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le'
BRANCH = re.compile(r'^(b|b(%s))(\.w|\.n)?$|^cbn?z$' % COND)
MULTI = ('ldm', 'pop', 'ldrd', 'ldrexd', 'umull', 'smull', 'umlal', 'smlal', 'vmov', 'mrrc')
CALLER_SAVED = ('r0', 'r1', 'r2', 'r3', 'r12', 'lr')
SPLIT = re.compile(r',(?![^\[]*\])(?![^{]*})')


def reg(name):
    return ALIAS.get(name, name)


def varint(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def parse(dis):
    funcs, cur = [], None
    for line in dis.splitlines():
        m = FUNC.match(line)
        if m:
            cur = (int(m.group(1), 16), m.group(2).split('@')[0], [])
            funcs.append(cur)
            continue
        m = INS.match(line)
        if m and cur is not None:
            op = m.group(4).split('@')[0].strip()
            ops = [o.strip() for o in SPLIT.split(op)] if op else []
            cur[2].append((int(m.group(1), 16), m.group(3), op, ops))
    return funcs


def writes(mn, op, ops):
    """Core registers an instruction writes (conservative)."""
    wb = '!' in op or re.search(r'\],\s*#', op)
    base = REG.search(op.split('[', 1)[1]) if '[' in op else None
    if STORE.match(mn):
        if mn.startswith(('stm', 'push')):
            return {reg(ops[0].rstrip('!'))} if ops and ops[0].endswith('!') else set()
        return {reg(base.group(1))} if wb and base else set()
    if mn.startswith(MULTI):
        out = {reg(x) for x in REG.findall(op.split('[')[0])}
        if mn.startswith(('ldm', 'pop')) and ops and not ops[0].endswith('!') and mn.startswith('ldm'):
            out.discard(reg(ops[0]))
        if wb and base:
            out.add(reg(base.group(1)))
        return out
    if mn.startswith('vmrs'):
        return {reg(ops[0])} if ops and REG.fullmatch(ops[0]) else set()
    if mn.startswith('v'):
        return set()
    out = {reg(ops[0])} if ops and REG.fullmatch(ops[0]) else set()
    if wb and base:
        out.add(reg(base.group(1)))
    return out


ALL_REGS = frozenset('r%d' % i for i in range(13)) | {'lr'}


def analyse(so, ins, entry, contrib):
    """One pass over a function. `entry` maps each branch target to the
    registers holding g_cpu_regs there (must-analysis); `contrib` collects, per
    target, the holder sets of every edge reaching it. Returns (sites, events)."""
    addrs = {a for a, *_ in ins}
    vals, sites, events = {}, [], []
    in_it = 0
    falls = True   # the previous instruction can fall through to this one

    def holders():
        return frozenset(r for r, v in vals.items() if v == ('REGS',))

    for a, mn, op, ops in ins:
        if a in entry:
            if falls:
                contrib[a].append(holders())
            events.append(('target', a))
            vals = {r: ('REGS',) for r in entry[a]}
        falls = True
        cond = in_it > 0
        if in_it:
            in_it -= 1
        if re.fullmatch(r'it[te]{0,3}', mn):
            in_it = len(mn) - 1
            continue
        base_mn = mn.split('.')[0]
        if mn == 'bl':
            if op.startswith('%x' % MK_ADDRESS) or '<mk_address' in op:
                v = vals.get('r0')
                if not cond and v and v[0] in ('ESP', 'EBP') and -LIMIT <= v[1] <= LIMIT:
                    sites.append((a, v[0]))
                    if v[0] == 'EBP':
                        events.append(('ebp_site', a))
            for r in CALLER_SAVED:
                vals.pop(r, None)
            continue
        if mn == 'blx':
            for r in CALLER_SAVED:
                vals.pop(r, None)
            continue
        if mn == 'bx' or ((mn == 'pop' or mn.startswith('ldm')) and 'pc' in op):
            events.append(('branch' if cond else 'exit', a))
            falls = cond
            if mn == 'bx':
                continue
        elif BRANCH.match(mn):
            m = re.match(r'(?:\w+,\s*)?([0-9a-f]+)\b', op)
            t = int(m.group(1), 16) if m else None
            uncond = not cond and re.fullmatch(r'b(\.w|\.n)?', mn)
            if t in addrs:
                contrib[t].append(holders())
                events.append(('branch', a))
            else:
                events.append(('exit' if uncond else 'branch', a))
            falls = not uncond
            continue
        if NOWRITE.match(mn):
            continue
        # Writes to EBP: `str rX, [regs, #20]` is classified by its value; any
        # other store into the regs block that covers EBP counts as unknown
        m = re.fullmatch(r'(\w+), \[(\w+), #%d\]' % EBP_OFF, op)
        mb = re.search(r'\[(\w+)(?:, #(-?\d+))?\]', op)
        if base_mn == 'str' and m and vals.get(reg(m.group(2))) == ('REGS',):
            src = vals.get(reg(m.group(1)))
            frame = not cond and src and src[0] in ('ESP', 'EBP')
            events.append(('frame_store' if frame else 'other_store', a))
        elif base_mn.startswith('str') and mb and vals.get(reg(mb.group(1))) == ('REGS',):
            off = int(mb.group(2) or 0)
            width = {'strd': 8, 'strh': 2, 'strb': 1}.get(base_mn, 4)
            if off <= EBP_OFF + 3 and EBP_OFF < off + width:
                events.append(('other_store', a))
        elif base_mn.startswith('stm') and ops and vals.get(reg(ops[0].rstrip('!'))) == ('REGS',):
            # stm/stmia store from offset 0 up; stmdb below the base
            count = len(re.findall(r'\b(?:r\d+|sl|fp|ip|lr|sb)\b', op.split('{', 1)[1])) if '{' in op else 99
            if not base_mn.startswith('stmdb') and 4 * count > EBP_OFF:
                events.append(('other_store', a))
        dests = writes(mn, op, ops)
        new = None
        if not cond and len(dests) == 1:
            d = next(iter(dests))
            m = re.fullmatch(r'(r\d+), \[pc, #(\d+)\]', op)
            if base_mn == 'ldr' and m:
                new = ('lit', struct.unpack_from('<I', so, ((a + 4) & ~3) + int(m.group(2)))[0])
            elif base_mn == 'add' and ops == [d, 'pc'] and vals.get(d, (None,))[0] == 'lit':
                new = ('got', (vals[d][1] + a + 4) & 0xffffffff)
            elif base_mn == 'ldr' and re.fullmatch(r'\w+, \[(\w+)(, #0)?\]', op) \
                    and vals.get(reg(re.match(r'\w+, \[(\w+)', op).group(1))) == ('got', REGS_GOT):
                new = ('REGS',)
            else:
                m = re.fullmatch(r'\w+, \[(\w+), #(\d+)\]', op)
                if base_mn == 'ldr' and m and vals.get(reg(m.group(1))) == ('REGS',):
                    off = int(m.group(2))
                    new = ('ESP', 0) if off == ESP_OFF else ('EBP', 0) if off == EBP_OFF else None
                elif base_mn in ('add', 'adds', 'addw', 'sub', 'subs', 'subw'):
                    sign = 1 if base_mn.startswith('add') else -1
                    src = imm = None
                    if len(ops) == 2 and ops[1].startswith('#'):
                        src, imm = d, int(ops[1][1:], 0)
                    elif len(ops) == 3 and ops[2].startswith('#') and REG.fullmatch(ops[1]):
                        src, imm = reg(ops[1]), int(ops[2][1:], 0)
                    if src and vals.get(src, (None,))[0] in ('ESP', 'EBP'):
                        new = (vals[src][0], vals[src][1] + sign * imm)
                elif base_mn in ('mov', 'movs') and len(ops) == 2 and REG.fullmatch(ops[1]):
                    sv = vals.get(reg(ops[1]))
                    if sv and sv[0] in ('ESP', 'EBP', 'REGS'):
                        new = sv
        for d in dests:
            vals.pop(d, None)
        if new is not None:
            vals[next(iter(dests))] = new
    return sites, events


def function_sites(so, ins):
    """Iterates analyse() to the greatest fixed point of the holder sets."""
    addrs = {a for a, *_ in ins}
    targets = set()
    for a, mn, op, ops in ins:
        if BRANCH.match(mn):
            m = re.match(r'(?:\w+,\s*)?([0-9a-f]+)\b', op)
            if m and int(m.group(1), 16) in addrs:
                targets.add(int(m.group(1), 16))
    entry = {t: ALL_REGS for t in targets}
    for _ in range(100000):
        contrib = collections.defaultdict(list)
        sites, events = analyse(so, ins, entry, contrib)
        new = {t: frozenset.intersection(*contrib[t]) if contrib[t] else frozenset() for t in targets}
        if new == entry:
            return sites, events
        entry = new
    raise RuntimeError('holder analysis did not converge')


def ebp_is_frame(events):
    for kind, _ in events:
        if kind == 'frame_store':
            break
        if kind in ('target', 'branch', 'ebp_site', 'other_store', 'exit'):
            return False
    else:
        return False
    for k, (kind, _) in enumerate(events):
        if kind != 'other_store':
            continue
        for kind2, _ in events[k + 1:]:
            if kind2 == 'exit':
                break
            if kind2 in ('target', 'branch', 'ebp_site'):
                return False
        else:
            return False
    return True


def main(so_path, out_path):
    so = open(so_path, 'rb').read()
    objdump = os.environ.get('OBJDUMP', 'arm-vita-eabi-objdump')
    dis = subprocess.run([objdump, '-d', so_path], capture_output=True, text=True, check=True).stdout
    stats = collections.Counter()
    sites = []
    for start, name, ins in parse(dis):
        if not RECOMPILED.match(name) or not ins:
            continue
        if any(mn in ('tbb', 'tbh') or (mn == 'bx' and op != 'lr') for _, mn, op, _ in ins):
            stats['functions skipped (computed jump)'] += 1
            continue
        fsites, events = function_sites(so, ins)
        frame = ebp_is_frame(events)
        for a, kind in fsites:
            if kind == 'ESP' or frame:
                sites.append((a, 0 if kind == 'ESP' else 1))
                stats[kind + ' sites'] += 1
            else:
                stats['EBP sites skipped (EBP not a frame pointer)'] += 1
    sites.sort()
    out = bytearray(b'STK1' + struct.pack('<I', len(sites)))
    prev = 0
    for a, kind in sites:
        out += varint(((a - prev) // 2) << 1 | kind)
        prev = a
    open(out_path, 'wb').write(out)
    for k, v in sorted(stats.items()):
        print('%-48s %d' % (k, v))
    print('%-48s %d (%d bytes)' % ('sites written', len(sites), len(out)))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
