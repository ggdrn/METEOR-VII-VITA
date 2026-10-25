/*
 * Native snowboard minigame functions (game modes 8 and 14). Shared by
 * ff7_snowboard.c and the emulator test (scripts/emu/test_snowboard.py, which
 * compiles this header into an ARM blob and runs it next to the recompiled
 * functions). Freestanding; build without -ffast-math and without FP
 * contraction.
 *
 *   0x7334DA (m, v, out)   out = v * m: out[j] = x m[j] + y m[4+j] + z m[8+j]
 *                          + m[12+j], in double (the recompiled x87), stored
 *                          as float
 *   0x73488D (v, c)        thiscall, ret 8: one triangle (v: 3 x {x, y, z, w}
 *                          floats, c: 3 colors) clipped at z = [0x7B7E14]
 *                          (0.5) by 0x7335FB, then 1 or 2 triangles written to
 *                          the vertex buffer of the texture [this] (room from
 *                          0x66E272) as x/w, y/w, z/w, 1/w, color; [this + 4]
 *                          = 1 when written
 *   0x7335FB (v, c, ov, oc, oinv)  thiscall, ret 0x14, only called by
 *                          0x73488D: counts the vertices with z < 0.5; none:
 *                          the triangle and 1/w; one: a quad (4 vertices, the
 *                          clipped one replaced by two on the plane); two: a
 *                          triangle (the clipped ones moved to the plane) and
 *                          1/w; three: 0. New vertices interpolate x, y, w,
 *                          z = 0.5, colors through 0x73404C (per byte: a +
 *                          (b - a) t, through _ftol)
 *
 *   0x734CAB (p0, p1, p2, c0, c1, c2)  thiscall, ret 0x18: 0x73488D for
 *                          three vertex pointers and three colors
 *
 * As the recompiled code: float operands widened to double, double results
 * (no 80-bit), stored as float; an unordered compare (NaN) leaves C0/C2/C3;
 * a division by 0 keeps the dividend and sets ZE; _ftol is the .so's
 * (ff7_ftol_core.h). 0x7335FB and 0x73404C run inline here (their outputs
 * are 0x73488D's stack locals, dead after it returns: not written), with
 * their _check_event calls; 0x66E272 is called through its x86 entry with the
 * same stack frame. The final EAX/ECX/EDX are the ones the x86 code leaves.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_SNOWBOARD_CORE_H
#define FF7_SNOWBOARD_CORE_H

#include <stdint.h>

#include "ff7_ftol_core.h"

typedef struct snowboard_env {
    uint8_t * (* mk_address)(uint32_t va);
    void (* check_event)(void);
    volatile uint32_t * regs; // x86 registers of the recompiled code
    uint8_t * fpu;            // its x87 state
    void (* f_66e272)(void);  // vertex buffer room (its x86 entry)
    uint8_t * const * memmap; // mk_address's page table (x86 page -> host page), or NULL: call it
    uint32_t * ev_count;      // _check_event's call counter, or NULL: call it
    uint32_t counters[4];     // transforms, triangles in, triangles clipped (1 or 2 vertices), triangles drawn
} snowboard_env;

enum { SB_EAX = 0, SB_ECX = 1, SB_EDX = 2, SB_ESP = 4, SB_EBP = 5 };

#define SB_C0  0x5c
#define SB_C2  0x5e
#define SB_C3  0x64
#define SB_ZE  0x56
#define SB_TOP 0x60

// mk_address and _check_event inlined, as in ff7_chocobo_core.h
static inline uint8_t * sb_addr(const snowboard_env * e, uint32_t va) {
    uint8_t * p = e->memmap ? e->memmap[va >> 12] : (uint8_t *) 0;
    return p && va ? p + (va & 0xfff) : e->mk_address(va);
}

static inline void sb_event(const snowboard_env * e) {
    uint32_t c;
    if (e->ev_count && ((c = *e->ev_count + 1) & 0x7ff)) *e->ev_count = c;
    else e->check_event();
}

static inline uint32_t sb_rd32(const snowboard_env * e, uint32_t va) {
    uint32_t v;
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(&v, sb_addr(e, va), 4);
    } else {
        v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t) *sb_addr(e, va + i) << (8 * i);
    }
    return v;
}

static inline void sb_wr32(const snowboard_env * e, uint32_t va, uint32_t v) {
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(sb_addr(e, va), &v, 4);
    } else {
        for (int i = 0; i < 4; i++) *sb_addr(e, va + i) = (uint8_t) (v >> (8 * i));
    }
}

// [va, va + len) as one host block if it lies in one page, else NULL
static inline uint8_t * sb_block(const snowboard_env * e, uint32_t va, uint32_t len) {
    return (va & 0xfff) + len <= 0x1000 ? sb_addr(e, va) : (uint8_t *) 0;
}

static inline uint32_t sb_get(const snowboard_env * e, const uint8_t * blk, uint32_t va, uint32_t off) {
    uint32_t v;
    if (blk) {
        __builtin_memcpy(&v, blk + off, 4);
        return v;
    }
    return sb_rd32(e, va + off);
}

static inline void sb_put(const snowboard_env * e, uint8_t * blk, uint32_t va, uint32_t off, uint32_t v) {
    if (blk) __builtin_memcpy(blk + off, &v, 4);
    else sb_wr32(e, va + off, v);
}

static inline float sb_f(uint32_t b) {
    float f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}

static inline uint32_t sb_bits(float f) {
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    return b;
}

static inline int sb_top_ok(const snowboard_env * e) {
    uint32_t top;
    __builtin_memcpy(&top, e->fpu + SB_TOP, 4);
    return top <= 7;
}

// fdiv: a / b in double; b == 0 keeps a and sets ZE
static inline double sb_div(snowboard_env * e, double a, double b) {
    if (b == 0.0) {
        e->fpu[SB_ZE] = 1;
        return a;
    }
    return a / b;
}

/*
 * 0x7334DA (m, v, out), cdecl, as called (ESP at the return address).
 * Returns 0, nothing done, if the x87 stack top is not 0..7.
 */
static inline int snowboard_xform_run(snowboard_env * e) {
    if (!sb_top_ok(e)) return 0;
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[SB_ESP];
    const uint8_t * args = sb_block(e, esp + 4, 12);
    uint32_t m_va = sb_get(e, args, esp + 4, 0), v_va = sb_get(e, args, esp + 4, 4);
    uint32_t o_va = sb_get(e, args, esp + 4, 8);
    double m[16], v[3];
    const uint8_t * mb = sb_block(e, m_va, 64);
    for (int i = 0; i < 16; i++) m[i] = sb_f(sb_get(e, mb, m_va, 4 * i));
    const uint8_t * vb = sb_block(e, v_va, 12);
    for (int i = 0; i < 3; i++) v[i] = sb_f(sb_get(e, vb, v_va, 4 * i));
    uint8_t * ob = sb_block(e, o_va, 16);
    for (int j = 0; j < 4; j++) { // faddp st(i), st: st(i) + st
        double s = v[0] * m[j];
        s = s + v[1] * m[4 + j];
        s = s + v[2] * m[8 + j];
        s = s + m[12 + j];
        sb_put(e, ob, o_va, 4 * j, sb_bits((float) s));
    }
    regs[SB_EAX] = o_va;
    regs[SB_ECX] = v_va;
    regs[SB_EDX] = m_va;
    sb_event(e);
    regs[SB_ESP] = esp + 4;
    e->counters[0]++;
    return 1;
}

// _ftol of the value pushed on the x87 stack (0x40B470, ff7_ftol_core.h);
// its scratch stack writes go to dead stack and are dropped
static inline uint32_t sb_ftol(snowboard_env * e, double v) {
    uint32_t top;
    __builtin_memcpy(&top, e->fpu + SB_TOP, 4);
    top = (top - 1) & 7;
    __builtin_memcpy(e->fpu + SB_TOP, &top, 4);
    __builtin_memcpy(e->fpu + 8 * top, &v, 8);
    uint8_t scratch[16];
    ff7_ftol_core(e->regs, e->fpu, scratch);
    sb_event(e);
    return e->regs[SB_EAX];
}

// 0x73404C (a, b, t, out): per color byte, a + (b - a) t through _ftol
static inline uint32_t sb_lerp_color(snowboard_env * e, uint32_t a, uint32_t b, uint32_t t_bits) {
    double t = sb_f(t_bits);
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        int32_t ca = (int32_t) ((a >> (8 * i)) & 0xff), cb = (int32_t) ((b >> (8 * i)) & 0xff);
        double x = (double) (cb - ca) * t; // fild; fmul
        x = x + (double) ca;               // fiadd
        out |= (sb_ftol(e, x) & 0xff) << (8 * i);
    }
    sb_event(e); // 0x73404C's own
    return out;
}

// x, y, z, w floats (bits)
typedef struct { uint32_t c[4]; } sb_vert;

// A vertex on the plane between p (kept) and q: (q - p) t + p for x, y, w; z = 0.5
static inline sb_vert sb_on_plane(const sb_vert * p, const sb_vert * q, uint32_t t_bits) {
    double t = sb_f(t_bits);
    sb_vert r;
    for (int i = 0; i < 4; i++) {
        if (i == 2) {
            r.c[i] = 0x3f000000u;
            continue;
        }
        double x = (double) sb_f(q->c[i]) - (double) sb_f(p->c[i]);
        x = x * t;
        x = x + (double) sb_f(p->c[i]);
        r.c[i] = sb_bits((float) x);
    }
    return r;
}

// fld [k]; fsub [a.z]; fld [b.z]; fsub [a.z]; fdivp: (k - a.z) / (b.z - a.z), as float
static inline uint32_t sb_plane_t(snowboard_env * e, float k, const sb_vert * a, const sb_vert * b) {
    double num = (double) k - (double) sb_f(a->c[2]);
    double den = (double) sb_f(b->c[2]) - (double) sb_f(a->c[2]);
    return sb_bits((float) sb_div(e, num, den));
}

// 1.0f / w (fld [0x7B7E18]; fdiv w; fstp float)
static inline uint32_t sb_inv(snowboard_env * e, float one, uint32_t w_bits) {
    return sb_bits((float) sb_div(e, (double) one, (double) sb_f(w_bits)));
}

/*
 * 0x7335FB's work (see the top): fills ov, oc (and oinv for 3), returns
 * 0, 3 or 4. Sets *ecx_out / *edx_out to the registers it leaves on the "all
 * clipped" path (the only one 0x73488D does not overwrite).
 */
static inline int sb_clip(snowboard_env * e, const sb_vert * v, const uint32_t * c, sb_vert * ov, uint32_t * oc,
                          uint32_t * oinv, float k_plane, float one) {
    uint8_t * fpu = e->fpu;
    int f[3], count = 0;
    for (int i = 0; i < 3; i++) { // fld [z]; fcomp [0x7B7E14]; test ah, 1
        double z = sb_f(v[i].c[2]), k = k_plane;
        if (!(z != z || k != k)) {
            fpu[SB_C0] = z < k;
            fpu[SB_C2] = 0;
            fpu[SB_C3] = z == k;
        }
        f[i] = fpu[SB_C0] != 0;
        count += f[i];
    }
    if (count == 0) {
        for (int i = 0; i < 3; i++) {
            uint32_t inv = sb_inv(e, one, v[i].c[3]);
            ov[i] = v[i];
            oc[i] = c[i];
            oinv[i] = inv;
        }
        sb_event(e);
        return 3;
    }
    if (count == 1 || count == 2) e->counters[2]++;
    if (count == 1) {
        int a = 0, b = 1, cc = 2;
        if (f[0]) { a = 0; b = 1; cc = 2; }
        if (f[1]) { a = 1; b = 2; cc = 0; }
        if (f[2]) { a = 2; b = 0; cc = 1; }
        uint32_t t1 = sb_plane_t(e, k_plane, &v[a], &v[b]);
        uint32_t t2 = sb_plane_t(e, k_plane, &v[a], &v[cc]);
        sb_vert p1 = sb_on_plane(&v[a], &v[b], t1), p2 = sb_on_plane(&v[a], &v[cc], t2);
        uint32_t c1 = sb_lerp_color(e, c[a], c[b], t1);
        uint32_t c2 = sb_lerp_color(e, c[a], c[cc], t2);
        if (a == 0) {
            ov[0] = p1; oc[0] = c1; ov[1] = v[1]; oc[1] = c[1];
            ov[2] = v[2]; oc[2] = c[2]; ov[3] = p2; oc[3] = c2;
        } else if (a == 1) {
            ov[0] = v[0]; oc[0] = c[0]; ov[1] = p2; oc[1] = c2;
            ov[2] = p1; oc[2] = c1; ov[3] = v[2]; oc[3] = c[2];
        } else {
            ov[0] = v[0]; oc[0] = c[0]; ov[1] = v[1]; oc[1] = c[1];
            ov[2] = p2; oc[2] = c2; ov[3] = p1; oc[3] = c1;
        }
        sb_event(e);
        return 4;
    }
    if (count == 2) {
        int a, b, cc; // a: the vertex in front
        if (!f[0]) { a = 0; b = 1; cc = 2; }
        else if (!f[1]) { a = 1; b = 2; cc = 0; }
        else { a = 2; b = 0; cc = 1; }
        uint32_t t1 = sb_plane_t(e, k_plane, &v[b], &v[a]);
        uint32_t t2 = sb_plane_t(e, k_plane, &v[cc], &v[a]);
        sb_vert q1 = sb_on_plane(&v[b], &v[a], t1), q2 = sb_on_plane(&v[cc], &v[a], t2);
        uint32_t c1 = sb_lerp_color(e, c[b], c[a], t1);
        uint32_t c2 = sb_lerp_color(e, c[cc], c[a], t2);
        if (a == 0) {
            uint32_t i0 = sb_inv(e, one, v[0].c[3]), i1 = sb_inv(e, one, q1.c[3]), i2 = sb_inv(e, one, q2.c[3]);
            ov[0] = v[0]; oc[0] = c[0]; ov[1] = q1; oc[1] = c1; ov[2] = q2; oc[2] = c2;
            oinv[0] = i0; oinv[1] = i1; oinv[2] = i2;
        } else if (a == 1) {
            uint32_t i0 = sb_inv(e, one, q2.c[3]), i1 = sb_inv(e, one, v[1].c[3]), i2 = sb_inv(e, one, q1.c[3]);
            ov[0] = q2; oc[0] = c2; ov[1] = v[1]; oc[1] = c[1]; ov[2] = q1; oc[2] = c1;
            oinv[0] = i0; oinv[1] = i1; oinv[2] = i2;
        } else {
            uint32_t i0 = sb_inv(e, one, q1.c[3]), i1 = sb_inv(e, one, q2.c[3]), i2 = sb_inv(e, one, v[2].c[3]);
            ov[0] = q1; oc[0] = c1; ov[1] = q2; oc[1] = c2; ov[2] = v[2]; oc[2] = c[2];
            oinv[0] = i0; oinv[1] = i1; oinv[2] = i2;
        }
        sb_event(e);
        return 3;
    }
    sb_event(e);
    return 0;
}

// Calls 0x66E272 (n, t) through its x86 entry from 0x73488D's frame
static inline uint32_t sb_alloc(snowboard_env * e, uint32_t ebp, uint32_t n, uint32_t t) {
    volatile uint32_t * regs = e->regs;
    sb_wr32(e, ebp - 0xe4, t);
    sb_wr32(e, ebp - 0xe8, n);
    regs[SB_ESP] = ebp - 0xec;
    e->f_66e272();
    regs[SB_ESP] = ebp - 0xe8;
    return regs[SB_EAX];
}

// One projected vertex: x/w, y/w, z/w (float products), 1/w, color
static inline void sb_emit(snowboard_env * e, uint32_t vb, const sb_vert * v, uint32_t inv, uint32_t color) {
    uint8_t * b = sb_block(e, vb, 0x14);
    double w = sb_f(inv);
    sb_put(e, b, vb, 0x10, color);
    for (int j = 0; j < 3; j++) sb_put(e, b, vb, 4 * j, sb_bits((float) ((double) sb_f(v->c[j]) * w)));
    sb_put(e, b, vb, 0xc, inv);
}

/*
 * 0x73488D's work with its vertices and colors read: ESP at its return
 * address, EBP = ebp_in, this = self.
 */
static inline void snowboard_tri_do(snowboard_env * e, uint32_t esp, uint32_t ebp_in, uint32_t self, const sb_vert * v,
                                    const uint32_t * c) {
    volatile uint32_t * regs = e->regs;
    uint32_t ebp = esp - 4;
    sb_vert ov[4];
    uint32_t oc[4], oinv[4];
    const uint8_t * kb = sb_block(e, 0x7b7e14, 8);
    float k_plane = sb_f(sb_get(e, kb, 0x7b7e14, 0)), one = sb_f(sb_get(e, kb, 0x7b7e14, 4));
    e->counters[1]++;

    sb_wr32(e, ebp, ebp_in); // push ebp (0x66E272's frame is below)
    regs[SB_EBP] = ebp;
    int r = sb_clip(e, v, c, ov, oc, oinv, k_plane, one);
    if (r == 3) {
        uint32_t t = sb_rd32(e, self);
        regs[SB_EAX] = t;
        regs[SB_ECX] = t;
        regs[SB_EDX] = self;
        if (sb_alloc(e, ebp, 1, t)) {
            uint32_t p = sb_rd32(e, t + 0x70);
            for (int i = 0; i < 3; i++, p += 0x20) {
                *sb_addr(e, self + 4) = 1;
                sb_emit(e, p, &ov[i], oinv[i], oc[i]);
            }
            regs[SB_EAX] = p;
            regs[SB_ECX] = oinv[2];
            regs[SB_EDX] = 3;
            e->counters[3]++;
        }
    } else if (r == 4) {
        uint32_t t = sb_rd32(e, self);
        regs[SB_EAX] = t;
        regs[SB_ECX] = self;
        regs[SB_EDX] = t;
        if (sb_alloc(e, ebp, 2, t)) {
            static const uint8_t idx[6] = { 0, 1, 2, 0, 2, 3 };
            for (int k = 0; k < 4; k++) oinv[k] = sb_inv(e, one, ov[k].c[3]);
            uint32_t p = sb_rd32(e, t + 0x70);
            for (int i = 0; i < 3; i++, p += 0x20) sb_emit(e, p, &ov[idx[i]], oinv[idx[i]], oc[idx[i]]);
            uint8_t * tb = sb_block(e, t + 0x40, 0x34);
            p = sb_get(e, tb, t + 0x40, 0x30) + sb_get(e, tb, t + 0x40, 0);
            sb_put(e, tb, t + 0x40, 0x30, p);
            for (int i = 3; i < 6; i++, p += 0x20) sb_emit(e, p, &ov[idx[i]], oinv[idx[i]], oc[idx[i]]);
            *sb_addr(e, self + 4) = 1;
            regs[SB_EAX] = 3;
            regs[SB_ECX] = p;
            regs[SB_EDX] = self;
            e->counters[3] += 2;
        }
    } else {
        regs[SB_EAX] = (uint32_t) r;
        regs[SB_ECX] = 1;
        regs[SB_EDX] = 2;
    }
    regs[SB_EBP] = ebp_in;
    sb_event(e);
    regs[SB_ESP] = esp + 4 + 8;
}

/*
 * 0x73488D (v, c), thiscall (ECX = this), ret 8, as called. Returns 0,
 * nothing done, if the x87 stack top is not 0..7.
 */
static inline int snowboard_tri_run(snowboard_env * e) {
    if (!sb_top_ok(e)) return 0;
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[SB_ESP];
    const uint8_t * args = sb_block(e, esp + 4, 8);
    uint32_t v_va = sb_get(e, args, esp + 4, 0), c_va = sb_get(e, args, esp + 4, 4);
    sb_vert v[3];
    uint32_t c[3];
    const uint8_t * vb = sb_block(e, v_va, 0x30);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) v[i].c[j] = sb_get(e, vb, v_va, 16 * i + 4 * j);
    const uint8_t * cb = sb_block(e, c_va, 12);
    for (int i = 0; i < 3; i++) c[i] = sb_get(e, cb, c_va, 4 * i);
    snowboard_tri_do(e, esp, regs[SB_EBP], regs[SB_ECX], v, c);
    return 1;
}

/*
 * 0x734CAB (p0, p1, p2, c0, c1, c2), thiscall, ret 0x18: copies the three
 * vertices (16 bytes at each p) and the colors into its frame and calls
 * 0x73488D (frame, frame colors) with the same this. Its locals are dead
 * after it returns: 0x73488D's work runs on the values.
 */
static inline int snowboard_tri3_run(snowboard_env * e) {
    if (!sb_top_ok(e)) return 0;
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[SB_ESP], ebp_in = regs[SB_EBP], ebp = esp - 4;
    const uint8_t * args = sb_block(e, esp + 4, 0x18);
    sb_vert v[3];
    uint32_t c[3];
    for (int i = 0; i < 3; i++) {
        uint32_t p = sb_get(e, args, esp + 4, 4 * i);
        const uint8_t * pb = sb_block(e, p, 16);
        for (int j = 0; j < 4; j++) v[i].c[j] = sb_get(e, pb, p, 4 * j);
        c[i] = sb_get(e, args, esp + 4, 12 + 4 * i);
    }
    sb_wr32(e, ebp, ebp_in); // push ebp
    snowboard_tri_do(e, ebp - 0x4c, ebp, regs[SB_ECX], v, c);
    regs[SB_EBP] = ebp_in;
    sb_event(e);
    regs[SB_ESP] = esp + 4 + 0x18;
    return 1;
}

#endif // FF7_SNOWBOARD_CORE_H
