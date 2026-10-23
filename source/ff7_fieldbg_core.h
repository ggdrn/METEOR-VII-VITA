/*
 * Native field background tile lists (ff7.exe 0x64085C and 0x6464BA). Shared
 * by ff7_fieldbg.c and the emulator test (scripts/emu/test_fieldbg.py, which
 * compiles this header into an ARM blob and runs it next to the recompiled
 * functions). Freestanding; build without -ffast-math and without FP
 * contraction.
 *
 *   0x6464BA (fx, fy, a, b, c, d, list)  cdecl: appends a tile to draw list
 *            `list`: e = [0xD00050] + 0x1804 list, n = [e], the entry at
 *            e + 0x18 n gets x = [0xCFF204] + fx, y = [0xCFF208] + fy (int
 *            plus float in double, stored as float), then a, b, c, d; [e] =
 *            n + 1
 *   0x64085C (x, y)  cdecl: for the u16 [0xCFFDFC] tiles of the layer (u16
 *            indices 4 bytes apart at [0xCFF3FC], tiles of 0x105C bytes at
 *            [0xCFFE00]) whose int16 position lies inside the window left
 *            of / above (x, y), whose flag byte +0x34 allows them, and that
 *            are not the hidden layer ([0xCFF1D8] and u16 +0x30 ==
 *            u16 [0xD00088] + 1): marks +0x1040 = 1 and appends it with its
 *            screen position (position * [0xCFF1F0] plus the int16 offsets of
 *            the window, as int16) to list u16 +0x1056 (or +0x1058 when
 *            +0x103C is set). Returns 1.
 *
 * One _check_event per function at its exit. The final EAX/ECX/EDX are the
 * ones the x86 code leaves (ECX keeps the high half of its last full write
 * under the u16 loop counter); the x87 stack is left as found.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_FIELDBG_CORE_H
#define FF7_FIELDBG_CORE_H

#include <stdint.h>

typedef struct fieldbg_env {
    uint8_t * (* mk_address)(uint32_t va);
    void (* check_event)(void);
    volatile uint32_t * regs; // x86 registers of the recompiled code
    uint8_t * const * memmap; // mk_address's page table, or NULL: call it
    uint32_t * ev_count;      // _check_event's call counter, or NULL: call it
    uint32_t counters[3];     // layers, tiles looked at, tiles appended
} fieldbg_env;

enum { FB_EAX = 0, FB_ECX = 1, FB_EDX = 2, FB_ESP = 4 };

#define FB_SCALE   0xcff1f0u // int32
#define FB_HIDDEN  0xcff1d8u // int32: skip the layer after [0xD00088]
#define FB_SHIFT   0xcff24cu // int32: rows near the top move up by it
#define FB_ORIGIN  0xcff204u // int32 x, y
#define FB_INDEX   0xcff3fcu // -> u16 tile indices, 4 bytes apart
#define FB_COUNT   0xcffdfcu // u16
#define FB_TILES   0xcffe00u // -> tiles of 0x105C bytes
#define FB_LISTS   0xd00050u // -> lists of 0x1804 bytes
#define FB_LAYER   0xd00088u // u16
#define FB_ALLOWED 0xcc0e7au // byte table, indexed by the tile's +0x34
#define FB_TILE    0x105cu
#define FB_LIST    0x1804u

static inline uint8_t * fb_addr(const fieldbg_env * e, uint32_t va) {
    uint8_t * p = e->memmap ? e->memmap[va >> 12] : (uint8_t *) 0;
    return p && va ? p + (va & 0xfff) : e->mk_address(va);
}

static inline void fb_event(const fieldbg_env * e) {
    uint32_t c;
    if (e->ev_count && ((c = *e->ev_count + 1) & 0x7ff)) *e->ev_count = c;
    else e->check_event();
}

static inline uint32_t fb_rd32(const fieldbg_env * e, uint32_t va) {
    uint32_t v;
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(&v, fb_addr(e, va), 4);
    } else {
        v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t) *fb_addr(e, va + i) << (8 * i);
    }
    return v;
}

static inline void fb_wr32(const fieldbg_env * e, uint32_t va, uint32_t v) {
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(fb_addr(e, va), &v, 4);
    } else {
        for (int i = 0; i < 4; i++) *fb_addr(e, va + i) = (uint8_t) (v >> (8 * i));
    }
}

static inline uint32_t fb_rd16(const fieldbg_env * e, uint32_t va) {
    return *fb_addr(e, va) | (uint32_t) *fb_addr(e, va + 1) << 8;
}

static inline uint8_t * fb_block(const fieldbg_env * e, uint32_t va, uint32_t len) {
    return (va & 0xfff) + len <= 0x1000 ? fb_addr(e, va) : (uint8_t *) 0;
}

static inline float fb_f(uint32_t b) {
    float f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}

static inline uint32_t fb_bits(float f) {
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    return b;
}

// 0x6464BA's work, its arguments as values; leaves its EAX/ECX/EDX
static inline void fb_append(fieldbg_env * e, float fx, float fy, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                             uint32_t list) {
    volatile uint32_t * regs = e->regs;
    uint32_t lists = fb_rd32(e, FB_LISTS), off = list * FB_LIST;
    uint32_t n = fb_rd32(e, lists + off);
    uint32_t slot = lists + off + 0x18 * n;
    int32_t ox = (int32_t) fb_rd32(e, FB_ORIGIN), oy = (int32_t) fb_rd32(e, FB_ORIGIN + 4);
    uint32_t v[6] = { fb_bits((float) ((double) ox + (double) fx)), fb_bits((float) ((double) oy + (double) fy)),
                      a, b, c, d };
    uint8_t * sb = fb_block(e, slot + 4, 0x18);
    if (sb) __builtin_memcpy(sb, v, 0x18);
    else for (int k = 0; k < 6; k++) fb_wr32(e, slot + 4 + 4 * k, v[k]);
    n = fb_rd32(e, lists + off) + 1; // reread: the entry is written first
    fb_wr32(e, lists + off, n);
    regs[FB_EAX] = n;
    regs[FB_ECX] = off;
    regs[FB_EDX] = lists;
    fb_event(e);
    e->counters[2]++;
}

// cdecl entries: ESP at the return address, arguments above it
static inline int fieldbg_append_run(fieldbg_env * e) {
    uint32_t esp = e->regs[FB_ESP];
    uint32_t a[7];
    const uint8_t * ab = fb_block(e, esp + 4, 28);
    if (ab) __builtin_memcpy(a, ab, 28);
    else for (int k = 0; k < 7; k++) a[k] = fb_rd32(e, esp + 4 + 4 * k);
    fb_append(e, fb_f(a[0]), fb_f(a[1]), a[2], a[3], a[4], a[5], a[6]);
    e->regs[FB_ESP] = esp + 4;
    return 1;
}

static inline int fieldbg_layer_run(fieldbg_env * e) {
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[FB_ESP];
    int32_t x = (int16_t) fb_rd16(e, esp + 4), y = (int16_t) fb_rd16(e, esp + 8);
    int32_t scale = (int32_t) fb_rd32(e, FB_SCALE);
    int16_t xoff = (int16_t) ((uint32_t) (0x140 - x) * (uint32_t) scale);
    int16_t yoff = (int16_t) ((uint32_t) (0xe0 - y) * (uint32_t) scale);
    uint32_t ecx = (uint32_t) (0x140 - x) * (uint32_t) scale;
    int32_t shift = (int32_t) fb_rd32(e, FB_SHIFT);
    if (shift > 0) {
        ecx = (uint32_t) y;
        if (y <= 8) yoff = (int16_t) (yoff - (int16_t) ((uint32_t) shift * (uint32_t) scale));
    }
    uint32_t count = fb_rd16(e, FB_COUNT), i = 0;
    for (; i < count; i = (i + 1) & 0xffff) {
        uint32_t idx = fb_rd16(e, fb_rd32(e, FB_INDEX) + 4 * i);
        uint32_t tiles = fb_rd32(e, FB_TILES), t = tiles + FB_TILE * idx;
        int32_t tx = (int16_t) fb_rd16(e, t), ty = (int16_t) fb_rd16(e, t + 2);
        e->counters[1]++;
        if (ty <= y - 0x100) ecx = (uint32_t) (y - 0x100);
        else if (ty >= y) ecx = (uint32_t) ty;
        else if (tx <= x - 0x150) ecx = tiles;
        else if (tx >= x) ecx = (uint32_t) x;
        else {
            uint32_t f34 = *fb_addr(e, t + 0x34);
            if (f34 && !(*fb_addr(e, t + 0x35) & *fb_addr(e, FB_ALLOWED + f34))) {
                ecx = 0;
            } else {
                fb_wr32(e, t + 0x1040, 1);
                int16_t sx = (int16_t) ((uint32_t) tx * (uint32_t) scale + (uint32_t) xoff);
                int16_t sy = (int16_t) ((uint32_t) ty * (uint32_t) scale + (uint32_t) yoff);
                uint32_t layer = fb_rd16(e, t + 0x30);
                if (fb_rd32(e, FB_HIDDEN) && layer == fb_rd16(e, FB_LAYER) + 1) {
                    ecx = layer;
                } else {
                    uint32_t list = fb_rd16(e, t + (fb_rd32(e, t + 0x103c) ? 0x1058 : 0x1056));
                    fb_append(e, (float) sx, (float) sy, fb_rd32(e, t + 4), fb_rd32(e, t + 0x10),
                              fb_rd32(e, t + 0x14), layer, list);
                    ecx = regs[FB_ECX];
                }
            }
        }
        ecx = (ecx & 0xffff0000u) | ((i + 1) & 0xffff); // mov cx, [ebp - 0x20]; add cx, 1
    }
    regs[FB_EAX] = 1;
    regs[FB_ECX] = ecx;
    regs[FB_EDX] = i;
    fb_event(e);
    e->counters[0]++;
    regs[FB_ESP] = esp + 4;
    return 1;
}

#endif // FF7_FIELDBG_CORE_H
