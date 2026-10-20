/*
 * Native 0x75F68C: draws the triangles of one world map terrain mesh.
 *
 * Shared by ff7_world.c and by the emulator test, which compiles it into a
 * freestanding ARM blob and runs it next to the recompiled function
 * (scripts/emu/test_world_tri.py). It works on x86 memory through the
 * environment (mk_address, the x86 registers and x87 state of the
 * recompiled code) and calls the recompiled functions it does not replace
 * (0x68DAE1, the slow path of 0x66E272) the way the x86 code does, with the
 * same stack frame. Freestanding: no libc. Build without -ffast-math and
 * without FP contraction.
 *
 * Per triangle (12 bytes: vertex indices i0 i1 i2, pad, u0 v0 u1 v1 u2 v2,
 * then a word whose bits 16-24 select the texture), in x86 order:
 *
 *  1. skip unless one vertex is inside the clip area horizontally and one
 *     vertically (flags from 0x75F263); area = 0x760CC4 (P0, P1, P2);
 *     clear the pick slot 0xE36110 if it points at this triangle;
 *  2. back faces ((int) area < 0) are skipped; zmax = the farthest z, chosen
 *     with truncated differences as the x86 code does;
 *  3. near triangles ((int) zmax <= [0xE37D28]) set 0xE37520 if they
 *     straddle the screen center on both axes (the x86 code compares y with
 *     the horizontal center too);
 *  4. skip if a vertex has (int) w <= 0;
 *  5. fog (if [0xE37524]): 0x760D1B sorts the vertices by z; colors and
 *     specular words come from tables indexed by the depth band;
 *  6. if [0x96994C] (0x74C969) and 0x66E272 (1, texture) gets room: uv from
 *     the triangle bytes, normals from the mesh, 0x68DAE1 colors each
 *     vertex, and the three vertices (x/w, y/w, z/w, 1/w, color, u, v) are
 *     written to the texture's vertex buffer.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_WORLD_TRI_H
#define FF7_WORLD_TRI_H

#include <stdint.h>

typedef struct world_env {
    uint8_t * (* mk_address)(uint32_t va);
    void (* check_event)(void);
    volatile uint32_t * regs; // x86 registers of the recompiled code
    uint8_t * fpu;            // its x87 state
    uint8_t * pages;          // host address of VA 0xE38000 if 0xE38000-0xE39FFF is contiguous, else 0
    void (* f_68dae1)(void);  // vertex color (the native hook in game)
    void (* f_66e272)(void);  // vertex buffer allocation (its slow path)
    uint32_t counters[4];     // triangles: seen, drawn, allocation slow paths, x87 NaN compares
} world_env;

enum { WT_EAX = 0, WT_ECX = 1, WT_EDX = 2, WT_ESP = 4, WT_EBP = 5 };

#define WT_FPU_C0  0x5c
#define WT_FPU_C2  0x5e
#define WT_FPU_C3  0x64
#define WT_FPU_ZE  0x56

static inline uint8_t * wt_host(const world_env * e, uint32_t va) {
    uint32_t off = va - 0xe38000u;
    if (off < 0x2000 && e->pages) return e->pages + off;
    return e->mk_address(va);
}

static inline uint32_t wt_rd32(const world_env * e, uint32_t va) {
    uint32_t v;
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(&v, wt_host(e, va), 4);
    } else {
        v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t) *e->mk_address(va + i) << (8 * i);
    }
    return v;
}

static inline void wt_wr32(const world_env * e, uint32_t va, uint32_t v) {
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(wt_host(e, va), &v, 4);
    } else {
        for (int i = 0; i < 4; i++) *e->mk_address(va + i) = (uint8_t) (v >> (8 * i));
    }
}

static inline uint8_t wt_rd8(const world_env * e, uint32_t va) {
    return *wt_host(e, va);
}

static inline uint16_t wt_rd16(const world_env * e, uint32_t va) {
    return (uint16_t) (wt_rd8(e, va) | (wt_rd8(e, va + 1) << 8));
}

static inline float wt_f(uint32_t bits) {
    float f;
    __builtin_memcpy(&f, &bits, 4);
    return f;
}

static inline uint32_t wt_bits(float f) {
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    return b;
}

static inline float wt_rdf(const world_env * e, uint32_t va) {
    return wt_f(wt_rd32(e, va));
}

static inline void wt_wrf(const world_env * e, uint32_t va, float f) {
    wt_wr32(e, va, wt_bits(f));
}

// vcvt.s32.f32 (0x75F090): truncates, saturates, NaN -> 0; one _check_event
static inline int32_t wt_int(const world_env * e, float f) {
    e->check_event();
    if (f != f) return 0;
    if (f >= 2147483648.0f) return 2147483647;
    if (f <= -2147483648.0f) return (int32_t) 0x80000000u;
    return (int32_t) f;
}

// fld a; fcomp b; fnstsw: sets C0/C2/C3 and returns C0 (a < b). As the
// recompiled fcomp does it: an unordered compare (NaN) leaves the three flags
// as they were, so C0 is the previous compare's.
static inline int wt_fcomp(world_env * e, double a, double b) {
    if (a != a || b != b) {
        e->counters[3]++;
        return e->fpu[WT_FPU_C0];
    }
    e->fpu[WT_FPU_C0] = a < b;
    e->fpu[WT_FPU_C2] = 0;
    e->fpu[WT_FPU_C3] = a == b;
    return a < b;
}

// The recompiled fdiv: k / w in double; w == 0 leaves k and sets ZE
static inline float wt_div(world_env * e, float k, float w) {
    if (w == 0.0f) {
        e->fpu[WT_FPU_ZE] = 1;
        return k;
    }
    return (float) ((double) k / (double) w);
}

// cdecl call of a recompiled function: arguments below `esp`, ESP lowered
// for the return address (the recompiled call does not store it)
static inline void wt_call(world_env * e, void (* fn)(void), uint32_t esp, int n, const uint32_t * args) {
    uint32_t sp = esp - 4 * (uint32_t) n;
    for (int i = 0; i < n; i++) wt_wr32(e, sp + 4 * (uint32_t) i, args[i]);
    e->regs[WT_ESP] = sp - 4;
    fn();
    e->regs[WT_ESP] = esp; // add esp, 4 * n
}

// 0x760CC4 (a, b, c): twice the signed area of the projected triangle, in
// double as the x87 code computes it
static inline float wt_area(world_env * e, uint32_t a, uint32_t b, uint32_t c) {
    double ax = wt_rdf(e, a), ay = wt_rdf(e, a + 4), bx = wt_rdf(e, b), by = wt_rdf(e, b + 4);
    double cx = wt_rdf(e, c), cy = wt_rdf(e, c + 4);
    double s = ax * by;
    s = s + bx * cy;
    s = s + cx * ay;
    s = s - ax * cy;
    s = s - bx * ay;
    s = s - cx * by;
    e->check_event();
    return (float) s;
}

// 0x760D1B (&z0, &z1, &z2, &lo, &mid, &hi): orders the vertices by z with
// the x87 compares of the x86 code
static inline void wt_sort(world_env * e, const float z[3], int32_t * lo, int32_t * mid, int32_t * hi) {
    int32_t s = wt_fcomp(e, z[0], z[1]) ? 1 : 0;
    int32_t m = wt_fcomp(e, z[s], z[2]) ? 2 : s;
    if (m != 0) {
        *hi = m;
        int32_t other = m == 1 ? 2 : 1;
        if (wt_fcomp(e, z[0], z[other])) {
            *lo = 0;
            *mid = other;
        } else {
            *lo = other;
            *mid = 0;
        }
    } else {
        *hi = 0;
        if (wt_fcomp(e, z[1], z[2])) {
            *lo = 1;
            *mid = 2;
        } else {
            *lo = 2;
            *mid = 1;
        }
    }
    e->check_event();
}

// 0x66E272 (1, t): room for one triangle in the texture's vertex buffer.
// The common case (the buffer is current and has room) is done here; the
// others go to the recompiled function.
static inline int wt_alloc(world_env * e, uint32_t t, uint32_t esp) {
    if (!t) {
        e->check_event();
        return 0;
    }
    uint32_t pt = wt_rd32(e, t + 0x30);
    if (wt_rd32(e, t + 0x54) < wt_rd32(e, pt) || 1 > (int32_t) wt_rd32(e, t + 0x50)) {
        uint32_t args[2] = { 1, t };
        e->counters[2]++;
        wt_call(e, e->f_66e272, esp, 2, args);
        return e->regs[WT_EAX] != 0;
    }
    uint32_t p = wt_rd32(e, t + 0x8c);
    wt_wr32(e, p + 0x8, wt_rd32(e, p + 0x8) + 1);
    wt_wr32(e, t + 0x4c, wt_rd32(e, t + 0x4c) + 1);
    wt_wr32(e, t + 0x50, wt_rd32(e, t + 0x50) - 1);
    wt_wr32(e, t + 0x70, wt_rd32(e, t + 0x80));
    wt_wr32(e, t + 0x74, wt_rd32(e, t + 0x84));
    wt_wr32(e, t + 0x78, wt_rd32(e, t + 0x88));
    wt_wr32(e, t + 0x80, wt_rd32(e, t + 0x80) + wt_rd32(e, t + 0x40));
    wt_wr32(e, t + 0x84, wt_rd32(e, t + 0x84) + wt_rd32(e, t + 0x44));
    wt_wr32(e, t + 0x88, wt_rd32(e, t + 0x88) + 1);
    wt_wr32(e, p + 0xc, wt_rd32(e, p + 0xc) + wt_rd32(e, t + 0x38));
    wt_wr32(e, p + 0x18, wt_rd32(e, p + 0x18) + wt_rd32(e, t + 0x3c));
    e->check_event();
    return 1;
}

#define WT_PROJ  0xe38938u // x y z (12 bytes)
#define WT_OUT   0xe38f38u // x y z w (16 bytes)
#define WT_FLAGS 0xe397b8u

/*
 * Runs 0x75F68C (tri, tri_end, normals, arg4) as called: x86 ESP at the
 * return address. Returns 0 (nothing done) if the x86 stack frame is not
 * contiguous on the host; the caller then runs the original.
 */
static inline int world_tri_run(world_env * e) {
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[WT_ESP], ebp_in = regs[WT_EBP];
    uint32_t ebp = esp - 4;
    uint32_t frame = ebp - 0x174; // ESP after the prologue (sub esp, 0x170; push esi)
    {
        uint8_t * lo = e->mk_address(frame - 0x40), * hi = e->mk_address(ebp + 0x14);
        if (hi != lo + (ebp + 0x14 - (frame - 0x40))) return 0;
    }
    uint32_t tri = wt_rd32(e, esp + 4), end = wt_rd32(e, esp + 8), normals = wt_rd32(e, esp + 12);
    uint32_t arg4 = wt_rd32(e, esp + 16);

    wt_wr32(e, ebp, ebp_in); // push ebp
    regs[WT_EBP] = ebp;
    // Locals passed to 0x68DAE1: src words, rgba colors, normals, outputs
    const uint32_t src_va = ebp - 0x20, col_va = ebp - 0xc8;
    const uint32_t n_va[3] = { ebp - 0x40, ebp - 0x60, ebp - 0x7c };
    const uint32_t o_va[3] = { ebp - 0xe4, ebp - 0xcc, ebp - 0xd8 };
    for (int k = 0; k < 3; k++) {
        wt_wr32(e, src_va + 4 * k, 0xffffffffu);
        for (int c = 0; c < 4; c++) wt_wr32(e, col_va + 16 * k + 4 * c, 0x437f0000u); // 255.0
    }
    uint32_t center_u = (wt_rd32(e, 0xde68f8) >> 1) + wt_rd32(e, 0xe2c424);
    float center = (float) (double) center_u; // fild qword (center, 0)

    uint32_t edx = 0;
    do {
        e->counters[0]++;
        uint32_t idx[3] = { wt_rd8(e, tri), wt_rd8(e, tri + 1), wt_rd8(e, tri + 2) };
        uint32_t f = wt_rd32(e, WT_FLAGS + 4 * idx[0]) | wt_rd32(e, WT_FLAGS + 4 * idx[1])
                   | wt_rd32(e, WT_FLAGS + 4 * idx[2]);
        if (!(f & 0xff) || !(f & 0xff00)) {
            edx = !(f & 0xff00); // neg / sbb / inc: left in EDX
            goto skip;
        }
        {
            uint32_t pv[3], ov[3];
            float z[3];
            for (int k = 0; k < 3; k++) {
                pv[k] = WT_PROJ + 12 * idx[k];
                ov[k] = WT_OUT + 16 * idx[k];
                z[k] = wt_rdf(e, pv[k] + 8);
            }
            float d01 = (float) ((double) z[0] - (double) z[1]);
            float zmax = z[2];
            float area = wt_area(e, pv[0], pv[1], pv[2]);
            uint32_t pick = wt_rd32(e, 0xe36110);
            if (pick && (int32_t) (pick - tri) / 12 == 0) wt_wr32(e, 0xe36110, 0);
            float d02 = (float) ((double) z[0] - (double) z[2]);
            uint32_t nv[3];
            nv[0] = normals + 8 * idx[0];
            if (wt_int(e, area) < 0) {
                edx = normals; // from computing the normal's address
                goto skip;
            }
            nv[1] = normals + 8 * idx[1];
            if (wt_int(e, d01) >= 0) {
                nv[2] = normals + 8 * idx[2];
                if (wt_int(e, d02) >= 0) zmax = z[0];
            } else {
                float d12 = (float) ((double) z[1] - (double) z[2]);
                nv[2] = normals + 8 * idx[2];
                if (wt_int(e, d12) >= 0) zmax = z[1];
            }

            if ((int32_t) ((uint32_t) wt_int(e, zmax) - wt_rd32(e, 0xe37d28)) <= 0) {
                int lt = 0, gt = 0;
                for (int k = 0; k < 3; k++) {
                    float x = wt_rdf(e, pv[k]);
                    lt |= wt_fcomp(e, x, center);
                    gt |= wt_fcomp(e, center, x);
                }
                if (lt & gt) {
                    lt = gt = 0;
                    for (int k = 0; k < 3; k++) {
                        float y = wt_rdf(e, pv[k] + 4);
                        lt |= wt_fcomp(e, y, center);
                        gt |= wt_fcomp(e, center, y);
                    }
                    if (lt & gt) wt_wr32(e, 0xe37520, 1);
                }
            }

            for (int k = 0; k < 3; k++)
                if (wt_int(e, wt_rdf(e, ov[k] + 0xc)) <= 0) goto next;

            if (wt_rd32(e, 0xe37524)) {
                int32_t o[3]; // lo, mid, hi
                wt_sort(e, z, &o[0], &o[1], &o[2]);
                for (int k = 0; k < 3; k++) wt_wr32(e, src_va + 4 * o[k], wt_rd32(e, 0xe2b778 + 0x10 * k));
                for (int k = 0; k < 3; k++) wt_wr32(e, col_va + 16 * o[k] + 8, wt_rd32(e, 0xe28f48 + 0x40 * k));
                for (int k = 0; k < 3; k++) wt_wr32(e, col_va + 16 * o[k] + 4, wt_rd32(e, 0xe28f44 + 0x40 * k));
                for (int k = 0; k < 3; k++) wt_wr32(e, col_va + 16 * o[k], wt_rd32(e, 0xe28f40 + 0x40 * k));
                for (int k = 2; k >= 0; k--) wt_wr32(e, col_va + 16 * k + 12, 0x437f0000u);
                uint32_t v = (uint32_t) wt_int(e, zmax) - wt_rd32(e, 0xe37524);
                if ((int32_t) v > 0) {
                    v = (v >> 3) & 0xffc0;
                    uint32_t band = (v + 0x40) >> 2;
                    for (int pass = 0; pass < 2; pass++) {
                        for (int k = 0; k < 3; k++)
                            wt_wr32(e, src_va + 4 * o[k], wt_rd32(e, 0xe2b758 + 0x10 * k + 4 * band));
                        for (int k = 0; k < 3; k++) {
                            uint32_t c = col_va + 16 * o[k];
                            wt_wr32(e, c, wt_rd32(e, 0xe28f40 + 16 * (band - 8)));
                            wt_wr32(e, c + 4, wt_rd32(e, 0xe28f44 + 16 * (band - 4)));
                            wt_wr32(e, c + 8, wt_rd32(e, 0xe28f48 + 16 * band));
                            wt_wr32(e, c + 12, 0x437f0000u);
                        }
                        if (pass) break;
                        v -= 0x3c0;
                        if ((int32_t) v <= 0) break;
                        band = 0xff;
                    }
                }
            }

            uint32_t tex = (wt_rd32(e, tri + 8) >> 16) & 0x1ff;
            e->check_event(); // 0x74C969
            if (!wt_rd32(e, 0x96994c)) goto next;
            if (!wt_alloc(e, wt_rd32(e, 0xe2bbd8 + 4 * tex), frame)) goto next;
            e->counters[1]++;

            float uv[6];
            for (int k = 0; k < 6; k++) {
                uint32_t base = wt_rd32(e, 0xe2d168);
                int32_t d = (int32_t) wt_rd8(e, tri + 4 + k) - (int32_t) wt_rd16(e, base + 4 * tex + 2 * (k & 1));
                uint32_t t = wt_rd32(e, 0xe2bbd8 + 4 * tex);
                uv[k] = (float) ((double) d * (double) wt_rdf(e, t + 0x24 + 4 * (k & 1)));
            }
            float kn = wt_rdf(e, 0x7b7efc);
            for (int k = 0; k < 3; k++)
                for (int c = 0; c < 3; c++)
                    wt_wrf(e, n_va[k] + 4 * c,
                           (float) ((double) (int16_t) wt_rd16(e, nv[k] + 2 * c) * (double) kn));
            for (int k = 0; k < 3; k++) {
                uint32_t args[5] = { wt_rd32(e, 0xe360f4) + 0xf8, n_va[k], col_va + 16 * k, src_va + 4 * k,
                                     o_va[k] };
                wt_call(e, e->f_68dae1, frame, 5, args);
            }
            float one = wt_rdf(e, 0x7b7ef8);
            for (int k = 0; k < 3; k++) {
                float inv = wt_div(e, one, wt_rdf(e, ov[k] + 0xc));
                uint32_t vb = wt_rd32(e, wt_rd32(e, 0xe2bbd8 + 4 * tex) + 0x70) + 0x20 * k;
                for (int c = 0; c < 3; c++)
                    wt_wrf(e, vb + 4 * c, (float) ((double) wt_rdf(e, ov[k] + 4 * c) * (double) inv));
                wt_wrf(e, vb + 0xc, inv);
                wt_wr32(e, vb + 0x10, wt_rd32(e, o_va[k]));
                wt_wrf(e, vb + 0x18, uv[2 * k]);
                wt_wrf(e, vb + 0x1c, uv[2 * k + 1]);
            }
        }
    next: // the first two rejections skip this
        arg4 += 0x28;
        wt_wr32(e, esp + 16, arg4);
        edx = arg4;
    skip:
        tri += 12;
    } while (tri != end);

    regs[WT_EAX] = tri;
    regs[WT_ECX] = tri;
    regs[WT_EDX] = edx;
    regs[WT_EBP] = ebp_in;
    e->check_event();
    regs[WT_ESP] = esp + 4; // ret
    return 1;
}

#endif // FF7_WORLD_TRI_H
