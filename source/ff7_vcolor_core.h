/*
 * Native vertex color scaling of ff7.exe (battle models): every frame the
 * colors of a model's polygon sets are multiplied by a brightness. Shared by
 * ff7_vcolor.c and the emulator test (scripts/emu/test_vcolor.py, which
 * compiles this header into an ARM blob and runs it next to the recompiled
 * functions). Freestanding; build without -ffast-math and without FP
 * contraction.
 *
 *   0x68EFCE (scale, c, out)  cdecl: out[k] = (int) (scale * c[k]), k < 3
 *            (the porters rewrote it as float code: f32 products, vcvt
 *            truncation, low byte stored). Leaves the registers and the x87
 *            state alone.
 *   0x68F2D6 (scale, n, i, obj)  cdecl: game = 0x676578() ([0xDB2BB8]);
 *            r = 0x670196(i, obj, game) (polygon set i of obj locked: obj
 *            +0x5c + 0x24 i); r == 0: returns. If [r + 0x20] and its +0x8c
 *            (t): the paletted path, 0x6A5BA0(scale, n, t), then 0x660F12
 *            and 0x660F54 (palette upload, left to the .so); otherwise for
 *            the [r + 4] vertices of [r + 0x18] (colors at +4, 0x18 apart)
 *            and [r + 8] (outputs at +0x10, 0x20 apart): 0x68EFCE. Then
 *            0x670144(i, obj, game) unlocks.
 *   0x68F0F8 (scale, i, obj)  the same without the paletted path.
 *   0x6A5BA0 (scale, n, t)  if t, n < [t + 0xc] and [t + 0x24]: for the
 *            [t + 0x10] palette entries of palette n, 0x68EFCE from
 *            [t + 0x24] + 16 n [t + 0x10] (16 apart) to [t + 0x18] +
 *            4 n [t + 0x10] (4 apart).
 *
 * The other .so functions are called as the x86 code calls them (arguments
 * on the x86 stack, EAX/ECX/EDX as the x86 code leaves them before each
 * call); 0x68EFCE and 0x6A5BA0 are inlined. One _check_event per function
 * at its exit.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_VCOLOR_CORE_H
#define FF7_VCOLOR_CORE_H

#include <stdint.h>

typedef struct vcolor_env {
    uint8_t * (* mk_address)(uint32_t va);
    void (* check_event)(void);
    volatile uint32_t * regs; // x86 registers of the recompiled code
    uint8_t * const * memmap; // mk_address's page table, or NULL: call it
    uint32_t * ev_count;      // _check_event's call counter, or NULL: call it
    void (* f_676578)(void);  // the lock argument
    void (* f_670196)(void);  // lock (i, obj, game)
    void (* f_670144)(void);  // unlock (i, obj, game)
    void (* f_660f12)(void);
    void (* f_660f54)(void);
    uint32_t counters[3];     // sets scaled, colors scaled, paletted sets
} vcolor_env;

enum { VC_EAX = 0, VC_ECX = 1, VC_EDX = 2, VC_ESP = 4, VC_EBP = 5 };

static inline uint8_t * vc_addr(const vcolor_env * e, uint32_t va) {
    uint8_t * p = e->memmap ? e->memmap[va >> 12] : (uint8_t *) 0;
    return p && va ? p + (va & 0xfff) : e->mk_address(va);
}

static inline void vc_event(const vcolor_env * e) {
    uint32_t c;
    if (e->ev_count && ((c = *e->ev_count + 1) & 0x7ff)) *e->ev_count = c;
    else e->check_event();
}

static inline uint32_t vc_rd32(const vcolor_env * e, uint32_t va) {
    uint32_t v;
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(&v, vc_addr(e, va), 4);
    } else {
        v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t) *vc_addr(e, va + i) << (8 * i);
    }
    return v;
}

static inline void vc_wr32(const vcolor_env * e, uint32_t va, uint32_t v) {
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(vc_addr(e, va), &v, 4);
    } else {
        for (int i = 0; i < 4; i++) *vc_addr(e, va + i) = (uint8_t) (v >> (8 * i));
    }
}

static inline uint8_t * vc_block(const vcolor_env * e, uint32_t va, uint32_t len) {
    return (va & 0xfff) + len <= 0x1000 ? vc_addr(e, va) : (uint8_t *) 0;
}

static inline float vc_f(uint32_t b) {
    float f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}

// vcvt.s32.f32: truncates, saturates, NaN -> 0
static inline uint8_t vc_byte(float v) {
    int32_t i;
    if (v != v) i = 0;
    else if (v >= 2147483648.0f) i = 2147483647;
    else if (v <= -2147483648.0f) i = (int32_t) 0x80000000u;
    else i = (int32_t) v;
    return (uint8_t) i;
}

// cdecl call of a recompiled function: ESP is the caller's after its prologue
static inline void vc_call(const vcolor_env * e, void (* fn)(void), uint32_t esp, int n, const uint32_t * args,
                           uint32_t eax, uint32_t ecx, uint32_t edx) {
    volatile uint32_t * regs = e->regs;
    uint32_t sp = esp - 4 * (uint32_t) n;
    uint8_t * b = vc_block(e, sp, 4 * (uint32_t) n);
    if (b) __builtin_memcpy(b, args, 4 * (uint32_t) n);
    else for (int i = 0; i < n; i++) vc_wr32(e, sp + 4 * (uint32_t) i, args[i]);
    regs[VC_EAX] = eax;
    regs[VC_ECX] = ecx;
    regs[VC_EDX] = edx;
    regs[VC_ESP] = sp - 4; // the return address (not stored)
    fn();
    regs[VC_ESP] = esp;    // add esp, 4 * n
}

// 0x68EFCE's work: out[k] = (int) (scale * c[k])
static inline void vc_scale3(const vcolor_env * e, float scale, uint32_t c, uint32_t out) {
    const uint8_t * cb = vc_block(e, c, 12);
    float v[3];
    if (cb) __builtin_memcpy(v, cb, 12);
    else for (int k = 0; k < 3; k++) v[k] = vc_f(vc_rd32(e, c + 4 * k));
    uint8_t * ob = vc_block(e, out, 3);
    for (int k = 0; k < 3; k++) {
        uint8_t b = vc_byte(scale * v[k]);
        if (ob) ob[k] = b;
        else *vc_addr(e, out + k) = b;
    }
    vc_event(e);
}

// The vertex loop: [r + 4] vertices, colors q + 4 (0x18 apart), outputs p + 0x10 (0x20 apart)
static inline void vc_loop(vcolor_env * e, float scale, uint32_t r, uint32_t q, uint32_t p) {
    const uint8_t * rb = vc_block(e, r + 4, 4); // reread every vertex, as the x86 code does
    for (int32_t k = 0;; k++) {
        uint32_t n;
        if (rb) __builtin_memcpy(&n, rb, 4);
        else n = vc_rd32(e, r + 4);
        if (k >= (int32_t) n) break;
        vc_scale3(e, scale, q + 4, p + 0x10);
        q += 0x18;
        p += 0x20;
        e->counters[1]++;
    }
}

// 0x6A5BA0's work
static inline void vc_palette(vcolor_env * e, float scale, uint32_t n, uint32_t t) {
    if (t && (int32_t) n < (int32_t) vc_rd32(e, t + 0xc) && vc_rd32(e, t + 0x24)) {
        uint32_t m = n * vc_rd32(e, t + 0x10);
        uint32_t src = vc_rd32(e, t + 0x24) + (m << 4), out = vc_rd32(e, t + 0x18) + 4 * m;
        for (int32_t k = 0; k < (int32_t) vc_rd32(e, t + 0x10); k++) {
            vc_scale3(e, scale, src, out);
            src += 0x10;
            out += 4;
            e->counters[1]++;
        }
    }
    vc_event(e);
}

// cdecl entries: ESP at the return address, arguments above it
static inline int vcolor_scale_run(vcolor_env * e) { // 0x68EFCE
    uint32_t esp = e->regs[VC_ESP];
    uint32_t a[3];
    const uint8_t * ab = vc_block(e, esp + 4, 12);
    if (ab) __builtin_memcpy(a, ab, 12);
    else for (int k = 0; k < 3; k++) a[k] = vc_rd32(e, esp + 4 + 4 * k);
    vc_scale3(e, vc_f(a[0]), a[1], a[2]);
    e->regs[VC_ESP] = esp + 4;
    return 1;
}

// 0x68F2D6 (palette = 1: scale, n, i, obj) and 0x68F0F8 (palette = 0: scale, i, obj)
static inline int vcolor_set_run(vcolor_env * e, int palette) {
    volatile uint32_t * regs = e->regs;
    uint32_t esp0 = regs[VC_ESP], ebp_in = regs[VC_EBP];
    uint32_t ebp = esp0 - 4;
    uint32_t frame = ebp - (palette ? 0x1c : 0x14); // ESP after the prologue
    uint32_t a[4];
    const uint8_t * ab = vc_block(e, esp0 + 4, 16);
    if (ab) __builtin_memcpy(a, ab, 16);
    else for (int k = 0; k < 4; k++) a[k] = vc_rd32(e, esp0 + 4 + 4 * k);
    // lock/unlock take (i, obj, game): 0x68F2D6's arguments 3 and 4, 0x68F0F8's 2 and 3
    const uint32_t i = palette ? a[2] : a[1], obj = palette ? a[3] : a[2];
    vc_wr32(e, ebp, ebp_in); // push ebp
    regs[VC_EBP] = ebp;
    regs[VC_ESP] = frame - 4;
    e->f_676578();
    regs[VC_ESP] = frame;
    uint32_t game = regs[VC_EAX];
    {
        uint32_t args[3] = { i, obj, game };
        vc_call(e, e->f_670196, frame, 3, args, game, obj, i);
    }
    uint32_t r = regs[VC_EAX];
    if (r) {
        uint32_t r20 = palette ? vc_rd32(e, r + 0x20) : 0;
        uint32_t t = r20 ? vc_rd32(e, r20 + 0x8c) : 0;
        if (t) {
            uint32_t n = a[1];
            uint32_t c = n * vc_rd32(e, t + 0x10);
            // 0x6A5BA0(scale, n, t): its arguments and registers are dead after it
            uint32_t args1[3] = { a[0], n, t };
            uint8_t * b = vc_block(e, frame - 12, 12);
            if (b) __builtin_memcpy(b, args1, 12);
            else for (int k = 0; k < 3; k++) vc_wr32(e, frame - 12 + 4 * k, args1[k]);
            vc_palette(e, vc_f(a[0]), n, t);
            r20 = vc_rd32(e, r + 0x20);
            uint32_t t10 = vc_rd32(e, t + 0x10), t18 = vc_rd32(e, t + 0x18);
            uint32_t args2[6] = { c, t10, t18, c, t, r20 };
            vc_call(e, e->f_660f12, frame, 6, args2, c, t, t10);
            r20 = vc_rd32(e, r + 0x20);
            uint32_t args3[2] = { n, r20 };
            vc_call(e, e->f_660f54, frame, 2, args3, n, r, r20);
            e->counters[2]++;
        } else {
            uint32_t q = vc_rd32(e, r + 0x18);
            if (q) vc_loop(e, vc_f(a[0]), r, q, vc_rd32(e, r + 8));
        }
        uint32_t args[3] = { i, obj, game };
        if (palette) vc_call(e, e->f_670144, frame, 3, args, i, game, obj);
        else vc_call(e, e->f_670144, frame, 3, args, game, obj, i);
        e->counters[0]++;
    }
    regs[VC_EBP] = ebp_in; // mov esp, ebp; pop ebp
    vc_event(e);
    regs[VC_ESP] = esp0 + 4; // ret
    return 1;
}

#endif // FF7_VCOLOR_CORE_H
