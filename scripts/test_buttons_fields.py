#!/usr/bin/env python3
"""Exercise the actual field patch with paged x86 memory, optionally on an OBB.
Usage: python3 scripts/test_buttons_fields.py [original.obb ...]
No game data is stored in the repository.
"""
import ctypes
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import zipfile

root = Path(__file__).resolve().parents[1]
source = (root / 'source/ff7_buttons.c').read_text()

def function(name):
    match = re.search(r'^(?:static (?:inline )?)?[^\n]+\b' + name + r'\([^\n]*\) \{', source, re.M)
    start, pos, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]

code = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ff7_buttons_core.h"
#define l_info(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define l_warn(...) l_info(__VA_ARGS__)
#define MAX_STRING 4096
#define FIELD_SECTION0 0x2a
#define FIELD_SECTIONS 9
static int text_enabled = 1, font_ready = 1;
static unsigned lzss_seen, fields_patched, labels_patched;
static uint8_t *pages[4096];
static uint8_t *mk_address(uint32_t a) {
    assert(a < 0x1000000 && pages[a >> 12]);
    return pages[a >> 12] + (a & 4095);
}
typedef struct { uint32_t page; uint8_t *base; } xcache;
static void patch_widths(void) {}
'''
code += '\n'.join(function(n) for n in (
    'xptr', 'rd16', 'rd32', 'patch_string', 'patch_table', 'ff7_buttons_lzss'))
code += r'''
unsigned patch(uint8_t *data, unsigned size) {
    const unsigned base = 0x10ffb; /* header and strings can cross pages */
    for (unsigned a = base & ~4095u; a < base + size + 4096; a += 4096) {
        pages[a >> 12] = calloc(1, 4096);
        assert(pages[a >> 12]);
    }
    for (unsigned i = 0; i < size; i++) *mk_address(base + i) = data[i];
    labels_patched = 0;
    ff7_buttons_lzss(base, size);
    for (unsigned i = 0; i < size; i++) data[i] = *mk_address(base + i);
    for (unsigned a = base & ~4095u; a < base + size + 4096; a += 4096) {
        free(pages[a >> 12]); pages[a >> 12] = NULL;
    }
    return labels_patched;
}
'''

def decompress(b):
    ring, cursor, out, i = bytearray(4096), 4078, bytearray(), 4
    while i < len(b):
        flags = b[i]; i += 1
        for k in range(8):
            if i >= len(b):
                break
            if flags & (1 << k):
                values = [b[i]]; i += 1
                for v in values:
                    out.append(v); ring[cursor] = v; cursor = (cursor + 1) & 4095
            else:
                a, c = b[i:i+2]; i += 2
                p = a | ((c & 240) << 4)
                for j in range((c & 15) + 3):
                    v = ring[(p+j) & 4095]
                    out.append(v); ring[cursor] = v; cursor = (cursor + 1) & 4095
    return out

with tempfile.TemporaryDirectory() as tmp:
    c, lib = Path(tmp)/'test.c', Path(tmp)/'test.dylib'
    c.write_text(code)
    subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'source'), str(c), '-lm', '-o', str(lib)], check=True)
    dll = ctypes.CDLL(str(lib))
    dll.patch.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint]
    dll.patch.restype = ctypes.c_uint
    def patch(data):
        return dll.patch((ctypes.c_uint8 * len(data)).from_buffer(data), len(data))

    # Reproduce the log: strings offset == stored script length, but dialogs
    # still precede section 1. Also cover mods that include them in the length.
    for embedded in (False, True):
        d = bytearray(128)
        struct.pack_into('<HIII', d, 0, 0, 9, 42, 100)
        struct.pack_into('<I', d, 42, 54 if embedded else 8)
        struct.pack_into('<H', d, 46, 0x0502)
        struct.pack_into('<H', d, 50, 8)
        struct.pack_into('<HH', d, 54, 1, 4)
        d[58:62] = bytes([0x3b, 0x21, 0x3d, 0xff])
        before = bytes(d)
        assert patch(d) == 1
        assert d[58:62] == bytes([0xfe, 0xd3, 0x96, 0xff])
        assert d[:58] == before[:58] and d[62:] == before[62:]
        assert patch(d) == 0
        for boundary in (0, 50, 129, 0xffffffff):
            bad = bytearray(before)
            struct.pack_into('<I', bad, 10, boundary)
            snapshot = bytes(bad)
            assert patch(bad) == 0 and bad == snapshot
    print('Field regression: stock appended dialogs, mod embedded dialogs, invalid boundaries passed')

    for obb in sys.argv[1:]:
        fields = labels = 0
        with zipfile.ZipFile(obb) as archive:
            lgp = archive.read('assets/ff7_1.02/data/field/flevel.lgp')
        for i in range(struct.unpack_from('<I', lgp, 12)[0]):
            entry = 16 + i*27
            name = lgp[entry:entry+20].split(b'\0')[0]
            if b'.' in name:
                continue
            pos = struct.unpack_from('<I', lgp, entry+20)[0]
            size = struct.unpack_from('<I', lgp, pos+20)[0]
            packed = lgp[pos+24:pos+24+size]
            if len(packed) < 4 or struct.unpack_from('<I', packed)[0] != len(packed) - 4:
                continue  # non-field entries such as maplist
            d = decompress(packed)
            if len(d) < 54 or d[:10] != bytes.fromhex('0000090000002a000000'):
                continue
            before = bytes(d)
            count = patch(d)
            if count:
                table = 46 + struct.unpack_from('<H', before, 50)[0]
                limit = struct.unpack_from('<I', before, 10)[0]
                assert d[:table] == before[:table] and d[limit:] == before[limit:], name
                assert patch(d) == 0, name
            fields += 1; labels += count
        assert labels > 0
        print(f'{obb}: {fields} fields, {labels} labels replaced; writes confined to dialog tables')
