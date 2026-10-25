/*
 * Native functions of the roller coaster minigame (game mode 11). Shared by
 * ff7_coaster.c and the emulator test (scripts/emu/test_coaster.py, which
 * compiles this header into an ARM blob and runs it next to the recompiled
 * functions). Freestanding; build without -ffast-math and without FP
 * contraction.
 *
 * All thiscall (ECX = this), callee pops:
 *
 *   0x5F2759 (v, m)       ret 8: view depth of the int16 vertex v:
 *                         ((m[2] x + m[6] y) + m[10] z) + m[14], in double,
 *                         left on the x87 stack (stored as float on the
 *                         side, to a dead local)
 *   0x5F2838 (d)          ret 4: distance fade of depth d (float) between
 *                         A = [0xC3F750] and B = [0xC3F754] (int32): 1.0 if
 *                         A > d, else 0.0 if B < d, else 1 - (d - A) / (B - A);
 *                         on the x87 stack
 *   0x5F2897 (c, f)       ret 8: the color [c] faded: each of its 3 low bytes
 *                         times f through _ftol (0x40B470), returned as
 *                         0xFF000000 | b0 << 16 | b1 << 8 | b2
 *   0x5F2639 (v, m, out)  ret 0xC: out = (float x, y, z of v) * m (0x66CE40,
 *                         float); w == 0 becomes 0.1; out = x/w, y/w, z/w
 *                         (products by 1/w), 1/w
 *   0x5F04D7 (tri, t1, g1, t2, g2, h1, h2)  ret 0x1C: one triangle (3 int16
 *                         vertices 8 bytes apart, 3 colors at +0x18, flags
 *                         byte +0x1B): depth (this + 0x80) and fade of each
 *                         vertex; nothing if none is in front (depth > 0);
 *                         if one is closer than -100 the clipping path
 *                         (0x5F26EB, 0x672C61, 0x66C4F0) - left to the
 *                         original; else room from 0x66E272 (1, t) (t1 or
 *                         t2 by the flags' bit 1) and the 3 projected
 *                         vertices (this + 0x84) written to [t + 0x70] as
 *                         x, y, z, 1/w, faded color, 0xFFFFFFFF, 0, 0 - the
 *                         second vertex in slot 2, the third in slot 1
 *
 * As the recompiled code: float operands widened to double, double results
 * (no 80-bit); an unordered compare (NaN) leaves C0/C2/C3; fnstsw writes the
 * whole EAX; a division by 0 keeps the dividend and sets ZE; _ftol is the
 * .so's (ff7_ftol_core.h). One _check_event per function at its exit (and
 * 0x66CE40's, 0x40B470's). The final EAX/ECX/EDX are the ones the x86 code
 * leaves. The callees' stack frames are dead stack after them: not written.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_COASTER_CORE_H
#define FF7_COASTER_CORE_H

#include <stdint.h>

#include "ff7_ftol_core.h"

typedef struct coaster_env {
    uint8_t * (* mk_address)(uint32_t va);
    void (* check_event)(void);
    volatile uint32_t * regs; // x86 registers of the recompiled code
    uint8_t * fpu;            // its x87 state
    void (* f_66e272)(void);  // vertex buffer room (its x86 entry)
    uint8_t * const * memmap; // mk_address's page table, or NULL: call it
    uint32_t * ev_count;      // _check_event's call counter, or NULL: call it
    uint32_t counters[4];     // triangles drawn, triangles not in front, projections, colors
} coaster_env;

enum { CS_EAX = 0, CS_ECX = 1, CS_EDX = 2, CS_ESP = 4, CS_EBP = 5 };

#define CS_C0  0x5c
#define CS_C2  0x5e
#define CS_C3  0x64
#define CS_ZE  0x56
#define CS_TOP 0x60

#define CS_K0   0x7b7810u // 0.0f
#define CS_KN   0x7b7818u // -100.0f
#define CS_K1   0x7b781cu // 1.0f
#define CS_FADE 0xc3f750u // int32 A, B

static inline uint8_t * cs_addr(const coaster_env * e, uint32_t va) {
    uint8_t * p = e->memmap ? e->memmap[va >> 12] : (uint8_t *) 0;
    return p && va ? p + (va & 0xfff) : e->mk_address(va);
}

static inline void cs_event(const coaster_env * e) {
    uint32_t c;
    if (e->ev_count && ((c = *e->ev_count + 1) & 0x7ff)) *e->ev_count = c;
    else e->check_event();
}

static inline uint32_t cs_rd32(const coaster_env * e, uint32_t va) {
    uint32_t v;
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(&v, cs_addr(e, va), 4);
    } else {
        v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t) *cs_addr(e, va + i) << (8 * i);
    }
    return v;
}

static inline void cs_wr32(const coaster_env * e, uint32_t va, uint32_t v) {
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(cs_addr(e, va), &v, 4);
    } else {
        for (int i = 0; i < 4; i++) *cs_addr(e, va + i) = (uint8_t) (v >> (8 * i));
    }
}

static inline int16_t cs_rd16(const coaster_env * e, uint32_t va) {
    return (int16_t) (*cs_addr(e, va) | *cs_addr(e, va + 1) << 8);
}

static inline uint8_t * cs_block(const coaster_env * e, uint32_t va, uint32_t len) {
    return (va & 0xfff) + len <= 0x1000 ? cs_addr(e, va) : (uint8_t *) 0;
}

static inline uint32_t cs_get(const coaster_env * e, const uint8_t * blk, uint32_t va, uint32_t off) {
    uint32_t v;
    if (blk) {
        __builtin_memcpy(&v, blk + off, 4);
        return v;
    }
    return cs_rd32(e, va + off);
}

static inline void cs_put(const coaster_env * e, uint8_t * blk, uint32_t va, uint32_t off, uint32_t v) {
    if (blk) __builtin_memcpy(blk + off, &v, 4);
    else cs_wr32(e, va + off, v);
}

static inline float cs_f(uint32_t b) {
    float f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}

static inline uint32_t cs_bits(float f) {
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    return b;
}

static inline uint32_t cs_top(const coaster_env * e) {
    uint32_t top;
    __builtin_memcpy(&top, e->fpu + CS_TOP, 4);
    return top;
}

static inline void cs_push(coaster_env * e, double v) {
    uint32_t top = (cs_top(e) - 1) & 7;
    __builtin_memcpy(e->fpu + CS_TOP, &top, 4);
    __builtin_memcpy(e->fpu + 8 * top, &v, 8);
}

// fcomp: a against b; unordered leaves the flags
static inline void cs_fcomp(coaster_env * e, double a, double b) {
    if (a != a || b != b) return;
    e->fpu[CS_C0] = a < b;
    e->fpu[CS_C2] = 0;
    e->fpu[CS_C3] = a == b;
}

// fnstsw ax (the whole EAX)
static inline uint32_t cs_fnstsw(const coaster_env * e) {
    const uint8_t * f = e->fpu;
    uint32_t top = cs_top(e);
    uint32_t sw = f[0x54] | f[0x55] << 1 | f[0x56] << 2 | f[0x57] << 3 | f[0x58] << 4 | f[0x59] << 5
                | f[0x5a] << 6 | f[0x5b] << 7 | f[CS_C0] << 8 | f[0x5d] << 9 | f[CS_C2] << 10 | top << 11
                | f[CS_C3] << 14 | f[0x65] << 15;
    return sw & 0xffffu;
}

static inline double cs_div(coaster_env * e, double a, double b) {
    if (b == 0.0) {
        e->fpu[CS_ZE] = 1;
        return a;
    }
    return a / b;
}

// The int16 vertex as floats (fild; fstp float: exact)
static inline void cs_vertex(const coaster_env * e, uint32_t v_va, float in[3]) {
    const uint8_t * b = cs_block(e, v_va, 6);
    for (int i = 0; i < 3; i++) {
        int16_t x;
        if (b) __builtin_memcpy(&x, b + 2 * i, 2);
        else x = cs_rd16(e, v_va + 2 * i);
        in[i] = (float) x;
    }
}

// 0x5F2759's value (no x87 or register effects)
static inline double cs_depth_value(const coaster_env * e, uint32_t v_va, uint32_t m_va) {
    float in[3];
    cs_vertex(e, v_va, in);
    const uint8_t * mb = cs_block(e, m_va, 0x3c);
    double s = (double) cs_f(cs_get(e, mb, m_va, 0x08)) * (double) in[0];
    s = s + (double) cs_f(cs_get(e, mb, m_va, 0x18)) * (double) in[1];
    s = s + (double) cs_f(cs_get(e, mb, m_va, 0x28)) * (double) in[2];
    return s + (double) cs_f(cs_get(e, mb, m_va, 0x38));
}

// 0x5F2759 (v, m): the depth pushed on the x87 stack; EAX = ECX = EDX = m
static inline double cs_depth(coaster_env * e, uint32_t v_va, uint32_t m_va) {
    double s = cs_depth_value(e, v_va, m_va);
    cs_push(e, s);
    volatile uint32_t * regs = e->regs;
    regs[CS_EAX] = m_va;
    regs[CS_ECX] = m_va;
    regs[CS_EDX] = m_va;
    cs_event(e);
    return s;
}

// 0x5F2838 (d): the fade pushed on the x87 stack; sets EAX
static inline double cs_fade(coaster_env * e, uint32_t d_bits) {
    volatile uint32_t * regs = e->regs;
    double d = cs_f(d_bits), r;
    const uint8_t * gb = cs_block(e, CS_FADE, 8);
    int32_t a = (int32_t) cs_get(e, gb, CS_FADE, 0), b = (int32_t) cs_get(e, gb, CS_FADE, 4);
    cs_fcomp(e, (double) a, d);
    regs[CS_EAX] = cs_fnstsw(e);
    if (!(e->fpu[CS_C0] | e->fpu[CS_C3])) { // A > d
        r = cs_f(cs_rd32(e, CS_K1));
    } else {
        cs_fcomp(e, (double) b, d);
        regs[CS_EAX] = cs_fnstsw(e);
        if (e->fpu[CS_C0]) { // B < d
            r = cs_f(cs_rd32(e, CS_K0));
        } else {
            uint32_t range = (uint32_t) b - (uint32_t) a;
            r = d - (double) a;                               // fild A; fsubr d
            r = cs_div(e, r, (double) (int32_t) range);       // fidiv
            r = (double) cs_f(cs_rd32(e, CS_K1)) - r;          // fsubr 1.0
            regs[CS_EAX] = range;
        }
    }
    cs_push(e, r);
    cs_event(e);
    return r;
}

// _ftol of v (0x40B470, ff7_ftol_core.h), as called with v pushed
static inline uint32_t cs_ftol(coaster_env * e, double v) {
    cs_push(e, v);
    uint8_t scratch[16];
    ff7_ftol_core(e->regs, e->fpu, scratch);
    cs_event(e);
    return e->regs[CS_EAX];
}

// 0x5F2897 (c, f): the faded color; EAX = it, ECX = b2, EDX = b1 << 8
static inline uint32_t cs_color(coaster_env * e, uint32_t c_va, uint32_t f_bits) {
    volatile uint32_t * regs = e->regs;
    uint32_t c = cs_rd32(e, c_va);
    double f = cs_f(f_bits);
    uint32_t nb[3];
    for (int i = 0; i < 3; i++) nb[i] = cs_ftol(e, (double) (int32_t) ((c >> (8 * i)) & 0xff) * f) & 0xff;
    uint32_t r = 0xff000000u | nb[0] << 16 | nb[1] << 8 | nb[2];
    regs[CS_EAX] = r;
    regs[CS_ECX] = nb[2];
    regs[CS_EDX] = nb[1] << 8;
    cs_event(e);
    e->counters[3]++;
    return r;
}

// 0x5F2639 (v, m, out): writes out (4 floats); EAX = EDX = out, ECX = 1/w
static inline void cs_project(coaster_env * e, uint32_t v_va, uint32_t m_va, uint32_t o_va) {
    volatile uint32_t * regs = e->regs;
    float in[3], m[16], o[4];
    cs_vertex(e, v_va, in);
    const uint8_t * mb = cs_block(e, m_va, 64);
    for (int i = 0; i < 16; i++) m[i] = cs_f(cs_get(e, mb, m_va, 4 * i));
    for (int j = 0; j < 4; j++) { // 0x66CE40
        float s = m[4 + j] * in[1];
        s = s + m[8 + j] * in[2];
        s = s + m[j] * in[0];
        o[j] = s + m[12 + j];
    }
    cs_event(e); // 0x66CE40's
    uint32_t w_bits = cs_bits(o[3]);
    cs_fcomp(e, (double) o[3], (double) cs_f(cs_rd32(e, CS_K0)));
    regs[CS_EAX] = cs_fnstsw(e);
    if (e->fpu[CS_C3]) w_bits = 0x3dcccccdu; // 0.1f
    float inv = (float) cs_div(e, (double) cs_f(cs_rd32(e, CS_K1)), (double) cs_f(w_bits));
    uint8_t * ob = cs_block(e, o_va, 16);
    for (int j = 0; j < 3; j++) cs_put(e, ob, o_va, 4 * j, cs_bits((float) ((double) o[j] * (double) inv)));
    cs_put(e, ob, o_va, 12, cs_bits(inv));
    regs[CS_EAX] = o_va;
    regs[CS_ECX] = cs_bits(inv);
    regs[CS_EDX] = o_va;
    cs_event(e);
    e->counters[2]++;
}

static inline int cs_top_ok(const coaster_env * e) {
    return cs_top(e) <= 7;
}

// thiscall entries: ESP at the return address, arguments above it
static inline int coaster_depth_run(coaster_env * e) {
    if (!cs_top_ok(e)) return 0;
    uint32_t esp = e->regs[CS_ESP];
    const uint8_t * a = cs_block(e, esp + 4, 8);
    cs_depth(e, cs_get(e, a, esp + 4, 0), cs_get(e, a, esp + 4, 4));
    e->regs[CS_ESP] = esp + 4 + 8;
    return 1;
}

static inline int coaster_fade_run(coaster_env * e) {
    if (!cs_top_ok(e)) return 0;
    uint32_t esp = e->regs[CS_ESP];
    cs_fade(e, cs_rd32(e, esp + 4));
    e->regs[CS_ESP] = esp + 4 + 4;
    return 1;
}

static inline int coaster_color_run(coaster_env * e) {
    if (!cs_top_ok(e)) return 0;
    uint32_t esp = e->regs[CS_ESP];
    const uint8_t * a = cs_block(e, esp + 4, 8);
    cs_color(e, cs_get(e, a, esp + 4, 0), cs_get(e, a, esp + 4, 4));
    e->regs[CS_ESP] = esp + 4 + 8;
    return 1;
}

static inline int coaster_project_run(coaster_env * e) {
    if (!cs_top_ok(e)) return 0;
    uint32_t esp = e->regs[CS_ESP];
    const uint8_t * a = cs_block(e, esp + 4, 12);
    cs_project(e, cs_get(e, a, esp + 4, 0), cs_get(e, a, esp + 4, 4), cs_get(e, a, esp + 4, 8));
    e->regs[CS_ESP] = esp + 4 + 12;
    return 1;
}

/*
 * 0x5F04D7 as called. Returns 0 with nothing done when the original must
 * run: the x87 stack top is not 0..7, a depth is NaN, or the triangle is in
 * front with a vertex closer than -100 (the clipping path).
 */
static inline int coaster_tri_run(coaster_env * e) {
    if (!cs_top_ok(e)) return 0;
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[CS_ESP], self = regs[CS_ECX];
    const uint8_t * a = cs_block(e, esp + 4, 0x1c);
    uint32_t arg[7];
    for (int i = 0; i < 7; i++) arg[i] = cs_get(e, a, esp + 4, 4 * i);
    uint32_t tri = arg[0];
    const uint8_t * sb = cs_block(e, self + 0x80, 8);
    uint32_t m_depth = cs_get(e, sb, self + 0x80, 0), m_proj = cs_get(e, sb, self + 0x80, 4);

    // decide first, touching nothing (the original redoes it all)
    float d[3];
    int front = 0, near = 0;
    for (int k = 0; k < 3; k++) {
        d[k] = (float) cs_depth_value(e, tri + 8 * k, m_depth);
        if (d[k] != d[k]) return 0;
        if ((double) d[k] < (double) cs_f(cs_rd32(e, CS_KN))) near = 1;
        if ((double) d[k] > (double) cs_f(cs_rd32(e, CS_K0))) front = 1;
    }
    if (front && near) return 0;

    // the per-vertex loop, with its x87 and register effects
    uint32_t fade[3];
    for (int k = 0; k < 3; k++) {
        cs_depth(e, tri + 8 * k, m_depth);
        uint32_t top = (cs_top(e) + 1) & 7; // fstp d[ebp - 0x70]
        __builtin_memcpy(e->fpu + CS_TOP, &top, 4);
        cs_fcomp(e, (double) d[k], (double) cs_f(cs_rd32(e, CS_KN)));
        cs_fcomp(e, (double) d[k], (double) cs_f(cs_rd32(e, CS_K0)));
        regs[CS_ECX] = self;
        double f = cs_fade(e, cs_bits(d[k]));
        top = (cs_top(e) + 1) & 7; // fstp
        __builtin_memcpy(e->fpu + CS_TOP, &top, 4);
        fade[k] = cs_bits((float) f);
        regs[CS_EDX] = (uint32_t) k;
    }
    regs[CS_ECX] = self;
    regs[CS_EDX] = 3;
    if (!front) {
        e->counters[1]++;
        cs_event(e);
        regs[CS_ESP] = esp + 4 + 0x1c;
        return 1;
    }

    int second = (cs_addr(e, tri + 0x1b)[0] & 2) != 0;
    uint32_t t = second ? arg[3] : arg[1];
    // 0x66E272 (1, t) from this frame: ebp = esp - 4, locals 0xb4, esi pushed
    uint32_t ebp = esp - 4, frame = ebp - 0xb4 - 4;
    uint32_t ebp_in = regs[CS_EBP];
    cs_wr32(e, frame - 4, t);
    cs_wr32(e, frame - 8, 1);
    regs[CS_EAX] = second ? arg[4] : arg[2];
    regs[CS_ECX] = second ? arg[6] : arg[5];
    regs[CS_EDX] = t;
    regs[CS_EBP] = ebp;
    regs[CS_ESP] = frame - 12;
    e->f_66e272();
    regs[CS_ESP] = frame;

    // the projected vertices and faded colors, as locals of this frame
    uint32_t out[3] = { ebp - 0x80, ebp - 0x90, ebp - 0xa0 }, col[3];
    for (int k = 0; k < 3; k++) {
        regs[CS_ECX] = self;
        cs_project(e, tri + 8 * k, m_proj, out[k]);
    }
    for (int k = 0; k < 3; k++) {
        regs[CS_ECX] = self;
        col[k] = cs_color(e, tri + 0x18 + 4 * k, fade[k]);
    }
    static const uint8_t slot[3] = { 0, 2, 1 };
    uint32_t vb = 0;
    for (int k = 0; k < 3; k++) {
        vb = cs_rd32(e, t + 0x70) + 0x20 * slot[k];
        uint8_t * b = cs_block(e, vb, 0x20);
        const uint8_t * ob = cs_block(e, out[k], 16);
        for (int j = 0; j < 4; j++) cs_put(e, b, vb, 4 * j, cs_get(e, ob, out[k], 4 * j));
        cs_put(e, b, vb, 0x10, col[k]);
        cs_put(e, b, vb, 0x14, 0xffffffffu);
        cs_put(e, b, vb, 0x18, 0);
        cs_put(e, b, vb, 0x1c, 0);
    }
    regs[CS_EAX] = 0xffffffffu;
    regs[CS_ECX] = vb;
    regs[CS_EDX] = vb;
    regs[CS_EBP] = ebp_in;
    e->counters[0]++;
    cs_event(e);
    regs[CS_ESP] = esp + 4 + 0x1c;
    return 1;
}

#endif // FF7_COASTER_CORE_H
