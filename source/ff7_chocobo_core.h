/*
 * Native 0x77B9D4 (triangle) and 0x77BCA4 (quad): the chocobo race draws its
 * primitives with them (25% and 12% of the game thread in the v0.6.8
 * profile). Shared by ff7_chocobo.c and the emulator test
 * (scripts/emu/test_chocobo.py, which compiles this header into an ARM blob
 * and runs it next to the recompiled functions). Freestanding; build
 * without -ffast-math and without FP contraction.
 *
 * (c[0..n-1], v[0..n-1], m, t), n = 3 or 4, cdecl:
 *
 *     for each vertex k:  o_k = v_k * m          (0x66CE40, float)
 *                         return if o_k.w <= [0x7B7F50]  (double; fcomp,
 *                                                         test ah, 0x41)
 *     return if 0x66E272 (1, t) == 0             (room in t's vertex buffer)
 *     for each vertex k:  inv = [0x7B7F58] / o_k.w           (double -> float)
 *                         vb = [t + 0x70] + 0x20 k
 *                         vb.x, y, z = o_k.x, y, z * inv     (double -> float)
 *                         vb + 0xc = inv, vb + 0x10 = c_k
 *
 * As the recompiled code: an unordered compare (NaN) leaves C0/C2/C3 and the
 * test sees the previous ones; fnstsw writes the whole EAX; a division by 0
 * keeps the dividend and sets ZE. 0x66E272 is called through its x86 entry
 * with the same stack frame. The final EAX/ECX/EDX are the ones the x86
 * code leaves on each path.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_CHOCOBO_CORE_H
#define FF7_CHOCOBO_CORE_H

#include <stdint.h>

typedef struct chocobo_env {
    uint8_t * (* mk_address)(uint32_t va);
    void (* check_event)(void);
    volatile uint32_t * regs; // x86 registers of the recompiled code
    uint8_t * fpu;            // its x87 state
    void (* f_66e272)(void);  // vertex buffer room (its x86 entry)
    void (* f_770434)(void);  // the race's type 0x21 objects (its x86 entry)
    uint8_t * const * memmap; // mk_address's page table (x86 page -> host page), or NULL: call it
    uint32_t * ev_count;      // _check_event's call counter, or NULL: call it
    uint32_t counters[5];     // triangles, quads, primitives drawn, lists, objects
} chocobo_env;

enum { CH_EAX = 0, CH_ECX = 1, CH_EDX = 2, CH_ESP = 4, CH_EBP = 5 };

#define CH_C0  0x5c
#define CH_C2  0x5e
#define CH_C3  0x64
#define CH_ZE  0x56
#define CH_TOP 0x60

/*
 * mk_address and _check_event inlined: the native code called them ~40 times
 * per primitive, a call into the .so each (in the v0.6.12 profile most of the
 * race's game thread time was spent around them).
 *   mk_address(va): page table entry + offset; va 0 or an unmapped page go
 *     to the real function (its log / default page).
 *   _check_event(): counts its calls and steps the timers every 2048th; the
 *     count is bumped here and only the 2048th call goes to the function.
 */
static inline uint8_t * ch_addr(const chocobo_env * e, uint32_t va) {
    uint8_t * p = e->memmap ? e->memmap[va >> 12] : (uint8_t *) 0;
    return p && va ? p + (va & 0xfff) : e->mk_address(va);
}

static inline void ch_event(const chocobo_env * e) {
    uint32_t c;
    if (e->ev_count && ((c = *e->ev_count + 1) & 0x7ff)) *e->ev_count = c;
    else e->check_event();
}

static inline uint32_t ch_rd32(const chocobo_env * e, uint32_t va) {
    uint32_t v;
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(&v, ch_addr(e, va), 4);
    } else {
        v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t) *ch_addr(e, va + i) << (8 * i);
    }
    return v;
}

static inline void ch_wr32(const chocobo_env * e, uint32_t va, uint32_t v) {
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(ch_addr(e, va), &v, 4);
    } else {
        for (int i = 0; i < 4; i++) *ch_addr(e, va + i) = (uint8_t) (v >> (8 * i));
    }
}

static inline float ch_f(uint32_t b) {
    float f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}

static inline uint32_t ch_bits(float f) {
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    return b;
}

// Host address of [va, va + len) if it lies in one page, else NULL. One
// mk_address call per block instead of one per word: the native code ran
// ~5 million mk_address calls per second in the busiest race scenes (v0.6.10).
static inline uint8_t * ch_block(const chocobo_env * e, uint32_t va, uint32_t len) {
    return (va & 0xfff) + len <= 0x1000 ? ch_addr(e, va) : (uint8_t *) 0;
}

static inline uint32_t ch_get(const chocobo_env * e, const uint8_t * blk, uint32_t va, uint32_t off) {
    uint32_t v;
    if (blk) {
        __builtin_memcpy(&v, blk + off, 4);
        return v;
    }
    return ch_rd32(e, va + off);
}

static inline void ch_put(const chocobo_env * e, uint8_t * blk, uint32_t va, uint32_t off, uint32_t v) {
    if (blk) __builtin_memcpy(blk + off, &v, 4);
    else ch_wr32(e, va + off, v);
}

// fnstsw ax as the recompiled code builds it (the whole EAX)
static inline uint32_t ch_fnstsw(const chocobo_env * e) {
    const uint8_t * f = e->fpu;
    uint32_t top;
    __builtin_memcpy(&top, f + CH_TOP, 4);
    uint32_t sw = f[0x54] | f[0x55] << 1 | f[0x56] << 2 | f[0x57] << 3 | f[0x58] << 4 | f[0x59] << 5
                | f[0x5a] << 6 | f[0x5b] << 7 | f[CH_C0] << 8 | f[0x5d] << 9 | f[CH_C2] << 10 | top << 11
                | f[CH_C3] << 14 | f[0x65] << 15;
    return sw & 0xffffu;
}

static inline int ch_top_ok(const chocobo_env * e) {
    uint32_t top;
    __builtin_memcpy(&top, e->fpu + CH_TOP, 4);
    return top <= 7;
}

/*
 * 0x77B9D4 (n = 3) or 0x77BCA4 (n = 4) with its arguments, called with the
 * x86 ESP at esp (its return address) and EBP = ebp_in.
 */
static inline void chocobo_prim_do(chocobo_env * e, int n, uint32_t esp, uint32_t ebp_in, const uint32_t * c,
                                   const uint32_t * v, uint32_t m_va, uint32_t t) {
    volatile uint32_t * regs = e->regs;
    uint8_t * fpu = e->fpu;
    uint32_t ebp = esp - 4;
    uint32_t frame = ebp - (n == 3 ? 0x48 : 0x60); // after sub esp
    if (n == 3) e->counters[0]++;
    else e->counters[1]++;

    // [0x7B7F50] (double, w limit) and [0x7B7F58] (float 1.0)
    const uint8_t * kb = ch_block(e, 0x7b7f50, 12);
    double kmin;
    {
        uint32_t lo = ch_get(e, kb, 0x7b7f50, 0), hi = ch_get(e, kb, 0x7b7f50, 4);
        uint64_t b = (uint64_t) hi << 32 | lo;
        __builtin_memcpy(&kmin, &b, 8);
    }
    float m[16];
    const uint8_t * mb = ch_block(e, m_va, 64);
    for (int i = 0; i < 16; i++) m[i] = ch_f(ch_get(e, mb, m_va, 4 * i));

    float o[4][4];
    for (int k = 0; k < n; k++) {
        float in[3];
        const uint8_t * vbk = ch_block(e, v[k], 12);
        for (int i = 0; i < 3; i++) in[i] = ch_f(ch_get(e, vbk, v[k], 4 * i));
        for (int j = 0; j < 4; j++) { // 0x66CE40
            float s = m[4 + j] * in[1];
            s = s + m[8 + j] * in[2];
            s = s + m[j] * in[0];
            o[k][j] = s + m[12 + j];
        }
        ch_event(e);
        double w = o[k][3];
        if (!(w != w || kmin != kmin)) { // fcomp; unordered leaves the flags
            fpu[CH_C0] = w < kmin;
            fpu[CH_C2] = 0;
            fpu[CH_C3] = w == kmin;
        }
        if (fpu[CH_C0] | fpu[CH_C3]) { // test ah, 0x41: o.w <= kmin
            regs[CH_EAX] = ch_fnstsw(e);
            regs[CH_ECX] = v[k];
            regs[CH_EDX] = m_va;
            ch_event(e);
            regs[CH_ESP] = esp + 4;
            return;
        }
    }
    // the last pushes before 0x66E272: ECX = v[n-1], EDX = m (from the transform call)
    regs[CH_EAX] = ch_fnstsw(e);
    regs[CH_ECX] = v[n - 1];
    regs[CH_EDX] = m_va;
    ch_wr32(e, ebp, ebp_in); // push ebp
    regs[CH_EBP] = ebp;
    ch_wr32(e, frame - 4, t);
    ch_wr32(e, frame - 8, 1);
    regs[CH_ESP] = frame - 12;
    e->f_66e272();
    regs[CH_ESP] = esp;
    regs[CH_EBP] = ebp_in;
    if (!regs[CH_EAX]) {
        ch_event(e);
        regs[CH_ESP] = esp + 4;
        return;
    }

    float one = ch_f(ch_get(e, kb, 0x7b7f50, 8));
    uint32_t inv_bits = 0, vb = 0;
    for (int k = 0; k < n; k++) {
        float inv;
        if (o[k][3] == 0.0f) {
            fpu[CH_ZE] = 1;
            inv = one;
        } else {
            inv = (float) ((double) one / (double) o[k][3]);
        }
        vb = ch_rd32(e, t + 0x70) + 0x20 * (uint32_t) k;
        uint8_t * vbb = ch_block(e, vb, 0x14);
        for (int j = 0; j < 3; j++) ch_put(e, vbb, vb, 4 * j, ch_bits((float) ((double) o[k][j] * (double) inv)));
        inv_bits = ch_bits(inv);
        ch_put(e, vbb, vb, 0xc, inv_bits);
        ch_put(e, vbb, vb, 0x10, c[k]);
    }
    regs[CH_EAX] = c[n - 1];
    regs[CH_ECX] = inv_bits;
    regs[CH_EDX] = vb;
    e->counters[2]++;
    ch_event(e);
    regs[CH_ESP] = esp + 4;
}

/*
 * Runs 0x77B9D4 (n = 3) or 0x77BCA4 (n = 4) as called (x86 ESP at the return
 * address). Returns 0, nothing done, if the x86 stack top is not 0..7.
 */
static inline int chocobo_prim_run(chocobo_env * e, int n) {
    if (!ch_top_ok(e)) return 0;
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[CH_ESP];
    uint32_t c[4], v[4];
    const uint8_t * args = ch_block(e, esp + 4, 8 * n + 8);
    for (int k = 0; k < n; k++) {
        c[k] = ch_get(e, args, esp + 4, 4 * k);
        v[k] = ch_get(e, args, esp + 4, 4 * n + 4 * k);
    }
    chocobo_prim_do(e, n, esp, regs[CH_EBP], c, v, ch_get(e, args, esp + 4, 8 * n),
                    ch_get(e, args, esp + 4, 8 * n + 4));
    return 1;
}

/*
 * 0x77BB50 (n = 3) / 0x77BE95 (n = 4) (m, t, p): the primitive p (n vertices
 * of 3 floats at p + 0xc k, then n colors) through 0x77B9D4 / 0x77BCA4
 * (c, p, p + 0xc, .., m, t). ESP at its return address, EBP = ebp_in. The
 * arguments it pushes are not written (dead stack): only 0x66E272's are.
 */
static inline void chocobo_wrap_do(chocobo_env * e, int n, uint32_t esp, uint32_t ebp_in, uint32_t m, uint32_t t,
                                   uint32_t p) {
    uint32_t c[4], v[4];
    const uint8_t * pb = ch_block(e, p + 0xc * n, 4 * n);
    for (int k = 0; k < n; k++) {
        c[k] = ch_get(e, pb, p + 0xc * n, 4 * k);
        v[k] = p + 0xc * k;
    }
    chocobo_prim_do(e, n, esp - 4 - 4 * (2 * n + 2) - 4, esp - 4, c, v, m, t);
    ch_event(e);
    e->regs[CH_EBP] = ebp_in;
    e->regs[CH_ESP] = esp + 4;
}

static inline int chocobo_wrap_run(chocobo_env * e, int n) {
    if (!ch_top_ok(e)) return 0;
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[CH_ESP];
    const uint8_t * args = ch_block(e, esp + 4, 12);
    chocobo_wrap_do(e, n, esp, regs[CH_EBP], ch_get(e, args, esp + 4, 0), ch_get(e, args, esp + 4, 4),
                    ch_get(e, args, esp + 4, 8));
    return 1;
}

#define CH_GLOBALS 0xe71000 // the race's globals page

/*
 * 0x770767 (quad = 0) / 0x7706D7 (quad = 1) (m, i, -, flag): object i of the
 * race's model, b = [0xE7112C]:
 *   triangle: e = [b + 0x18] + 0x1c i, p = [b + 0x4c] + 0x30 i, e[0x1b] == 0x30
 *   quad:     e = [b + 0x10] + 0x28 i, p = [b + 0x50] + 0x40 i, e[0x27] == 0x38
 * flag < 0 returns 0; else 0x77BB50 / 0x77BE95 (m, [g], p) with g picked by
 * the e byte, sets the matching "used" global and returns 1.
 */
static inline uint32_t chocobo_obj_do(chocobo_env * e, int quad, uint32_t esp, uint32_t ebp_in, uint32_t m,
                                      uint32_t i, uint32_t flag) {
    volatile uint32_t * regs = e->regs;
    uint8_t * g = ch_addr(e, CH_GLOBALS);
    uint32_t b = ch_get(e, g, CH_GLOBALS, 0x12c);
    const uint8_t * bb = ch_block(e, b, 0x54);
    uint32_t ent, p;
    if (!quad) {
        ent = ch_get(e, bb, b, 0x18) + 0x1c * i;
        p = ch_get(e, bb, b, 0x4c) + 0x30 * i;
    } else {
        ent = ch_get(e, bb, b, 0x10) + 0x28 * i;
        p = ch_get(e, bb, b, 0x50) + (i << 6);
    }
    uint32_t r = 0;
    if ((int32_t) flag < 0) {
        regs[CH_EAX] = 0;
        regs[CH_ECX] = b;
        regs[CH_EDX] = p;
    } else {
        int sel = quad ? *ch_addr(e, ent + 0x27) == 0x38 : *ch_addr(e, ent + 0x1b) == 0x30;
        static const uint16_t tex[2][2] = { { 0xc44, 0xc28 }, { 0xc0c, 0xbf0 } };
        uint32_t off = tex[quad][sel];
        chocobo_wrap_do(e, 3 + quad, esp - 28, esp - 4, m, ch_get(e, g, CH_GLOBALS, off), p);
        ch_put(e, g, CH_GLOBALS, off + 0x14, 1);
        regs[CH_EAX] = r = 1;
    }
    e->counters[4]++;
    ch_event(e);
    regs[CH_EBP] = ebp_in;
    regs[CH_ESP] = esp + 4;
    return r;
}

static inline int chocobo_obj_run(chocobo_env * e, int quad) {
    if (!ch_top_ok(e)) return 0;
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[CH_ESP];
    const uint8_t * args = ch_block(e, esp + 4, 16);
    chocobo_obj_do(e, quad, esp, regs[CH_EBP], ch_get(e, args, esp + 4, 0), ch_get(e, args, esp + 4, 4),
                   ch_get(e, args, esp + 4, 12));
    return 1;
}

/*
 * 0x770305 (first, last): draws the race's object lists first..last. b =
 * [0xE7112C]; the entries start at [b + 0x20] + 4 [b + 0x1c + 2 first] (int16)
 * and end at -1. A negative entry starts the next list (the count
 * last - first ending first) and its flag becomes
 * [0xE715FC][0x18 list + 0x13] & 0x80 (the first list's flag is the stack
 * left over in the frame, as the original reads it); the entry's byte 2
 * picks 0x770767 (0), 0x7706D7 (1) or 0x770434 (0x21) with its int16 index
 * and the drawn total, kept in [0xE71108].
 */
static inline int chocobo_list_run(chocobo_env * e) {
    if (!ch_top_ok(e)) return 0;
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[CH_ESP], ebp_in = regs[CH_EBP], ebp = esp - 4;
    const uint8_t * args = ch_block(e, esp + 4, 8);
    uint32_t first = ch_get(e, args, esp + 4, 0), last = ch_get(e, args, esp + 4, 4);
    uint8_t * g = ch_addr(e, CH_GLOBALS);
    uint8_t * fr = ch_block(e, ebp - 0x20, 0x24); // the frame: locals and the saved EBP
    uint32_t list = first, left = last - first, count = ch_get(e, g, CH_GLOBALS, 0x108);
    uint32_t b = ch_get(e, g, CH_GLOBALS, 0x12c);
    uint32_t wva = ch_rd32(e, b + 0x1c) + 2 * first;
    uint16_t w16 = (uint16_t) (*ch_addr(e, wva) | *ch_addr(e, wva + 1) << 8);
    uint32_t ptr = ch_rd32(e, b + 0x20) + 4 * (uint32_t) (int32_t) (int16_t) w16;
    uint32_t flag = ch_get(e, fr, ebp - 0x20, 0x1c);
    uint32_t ecx = ptr, edx, entry;
    int type_set = 0;
    uint8_t type = 0;
    for (;; ptr += 4) {
        edx = ptr;
        entry = ch_rd32(e, ptr);
        if (entry == 0xffffffffu) break;
        if ((int32_t) entry < 0) {
            ecx = --left;
            if ((int32_t) left < 0) break;
            list++;
            uint32_t tbl = ch_get(e, g, CH_GLOBALS, 0x5fc);
            ecx = tbl;
            edx = flag = (uint32_t) (int32_t) (int8_t) *ch_addr(e, tbl + 0x18 * list + 0x13) & 0x80;
        }
        type = (uint8_t) (entry >> 16);
        type_set = 1;
        uint32_t i = (uint32_t) (int32_t) (int16_t) entry;
        if (type == 0) {
            count += chocobo_obj_do(e, 0, esp - 56, ebp, 0xe72e60, i, flag);
            ecx = count;
            regs[CH_ESP] = esp - 36;
        } else if (type == 1) {
            count += chocobo_obj_do(e, 1, esp - 56, ebp, 0xe72e60, i, flag);
            ecx = regs[CH_ECX];
            regs[CH_ESP] = esp - 36;
        } else if (type == 0x21) {
            uint32_t cx = (ecx & 0xffff0000u) | (entry & 0xffff);
            ch_wr32(e, esp - 40, count);
            ch_wr32(e, esp - 44, cx);
            ch_wr32(e, esp - 48, 0xe72e60);
            regs[CH_EAX] = count;
            regs[CH_ECX] = cx;
            regs[CH_EDX] = edx;
            regs[CH_EBP] = ebp;
            regs[CH_ESP] = esp - 52;
            e->f_770434();
            count += regs[CH_EAX];
            ecx = regs[CH_ECX];
            regs[CH_ESP] = esp - 36;
        }
    }
    ch_put(e, g, CH_GLOBALS, 0x108, count);
    // the frame as the original leaves it (a later call reads its flag slot)
    ch_put(e, fr, ebp - 0x20, 0x20, ebp_in);
    ch_put(e, fr, ebp - 0x20, 0x1c, flag);
    {
        uint8_t * wp = ch_block(e, ebp - 8, 2);
        if (wp) __builtin_memcpy(wp, &w16, 2);
        else {
            *ch_addr(e, ebp - 8) = (uint8_t) w16;
            *ch_addr(e, ebp - 7) = (uint8_t) (w16 >> 8);
        }
    }
    ch_put(e, fr, ebp - 0x20, 0x14, left);
    ch_put(e, fr, ebp - 0x20, 0x10, ptr);
    ch_put(e, fr, ebp - 0x20, 0xc, list);
    ch_put(e, fr, ebp - 0x20, 0x8, count);
    ch_put(e, fr, ebp - 0x20, 0x4, entry);
    if (type_set) *ch_addr(e, ebp - 0x20) = type;
    regs[CH_EAX] = entry;
    regs[CH_ECX] = count;
    regs[CH_EDX] = ptr;
    regs[CH_EBP] = ebp_in;
    e->counters[3]++;
    ch_event(e);
    regs[CH_ESP] = esp + 4;
    (void) ecx;
    return 1;
}

#endif // FF7_CHOCOBO_CORE_H
