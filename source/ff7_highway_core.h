/*
 * Native 0x651A6D: vertex color of the highway (motorbike minigame, mode 6).
 *
 * Shared by ff7_highway.c and by the emulator test, which compiles it into a
 * freestanding ARM blob and runs it next to the recompiled function
 * (scripts/emu/test_highway.py). Freestanding: no libc. Build without
 * -ffast-math and without FP contraction.
 *
 * 0x651A6D (src, v, out) is a per-vertex callback (stored by 0x659B40 and
 * called through a function pointer for every vertex the highway draws):
 *
 *     z = 0x651A2B (m, v)       m = [[0xD858E0] + 0x14]: m[2] x + m[6] y +
 *                               m[10] z + m[14], x87 double, stored as float
 *     f = 0x6519D3 (z)          fog: near = [0xD85958], far = [0xD858FC]
 *                               (ints); z <= near: [0x7B79A4]; z > far:
 *                               [0x7B79A8]; else [0x7B79A4] - (z - near) /
 *                               (far - near); stored as float
 *     out[k] = src[k] * f       k = 2, 1, 0; t < [0x7B79AC] (255): _ftol of
 *                               the float t, else 0xFF
 *
 * Each step follows the recompiled x87 code: values in double between
 * loads and stores, compares that leave C0/C2/C3 alone on NaN, fidiv by 0
 * keeping the dividend and setting ZE, fnstsw building AX from the x87
 * state, and _ftol (0x40B470) truncating and normalizing the control word.
 * The final EAX / ECX / EDX are the ones the x86 code leaves.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_HIGHWAY_CORE_H
#define FF7_HIGHWAY_CORE_H

#include <stdint.h>

#include "ff7_ftol_core.h"

typedef struct highway_env {
    uint8_t * (* mk_address)(uint32_t va);
    void (* check_event)(void);
    volatile uint32_t * regs; // x86 registers of the recompiled code
    uint8_t * fpu;            // its x87 state
    uint32_t counters[2];     // vertices, NaN compares
} highway_env;

enum { HW_EAX = 0, HW_ECX = 1, HW_EDX = 2, HW_ESP = 4 };

#define HW_C0  0x5c
#define HW_C2  0x5e
#define HW_C3  0x64
#define HW_ZE  0x56
#define HW_TOP 0x60

static inline uint32_t hw_rd32(const highway_env * e, uint32_t va) {
    uint32_t v;
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(&v, e->mk_address(va), 4);
    } else {
        v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t) *e->mk_address(va + i) << (8 * i);
    }
    return v;
}

static inline float hw_rdf(const highway_env * e, uint32_t va) {
    uint32_t b = hw_rd32(e, va);
    float f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}

static inline uint32_t hw_bits(float f) {
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    return b;
}

// fcomp: C0 = a < b, C3 = a == b, C2 = 0; unordered leaves the flags alone
static inline void hw_fcomp(highway_env * e, double a, double b) {
    if (a != a || b != b) {
        e->counters[1]++;
        return;
    }
    e->fpu[HW_C0] = a < b;
    e->fpu[HW_C2] = 0;
    e->fpu[HW_C3] = a == b;
}

// fnstsw ax: the status word as the recompiled code builds it. It writes the
// whole EAX (uxth, then a 32-bit store), not just AX as the x86 does.
static inline uint32_t hw_fnstsw(const highway_env * e, uint32_t eax) {
    (void) eax;
    const uint8_t * f = e->fpu;
    uint32_t top;
    __builtin_memcpy(&top, f + HW_TOP, 4);
    uint32_t sw = f[0x54] | f[0x55] << 1 | f[0x56] << 2 | f[0x57] << 3 | f[0x58] << 4 | f[0x59] << 5
                | f[0x5a] << 6 | f[0x5b] << 7 | f[HW_C0] << 8 | f[0x5d] << 9 | f[HW_C2] << 10 | top << 11
                | f[HW_C3] << 14 | f[0x65] << 15;
    return sw & 0xffffu;
}

// _ftol (0x40B470) of a value just loaded on the x87 stack: the control
// word normalization of ff7_ftol_core, the truncated 64-bit result in
// EDX:EAX, one _check_event
static inline uint64_t hw_ftol(highway_env * e, double v) {
    uint8_t * fpu = e->fpu;
    uint32_t precision, rounding;
    __builtin_memcpy(&precision, fpu + 0x48, 4);
    __builtin_memcpy(&rounding, fpu + 0x4c, 4);
    uint16_t cw = (uint16_t) (fpu[0x40] | ((uint32_t) fpu[0x41] << 1) | ((uint32_t) fpu[0x42] << 2)
                            | ((uint32_t) fpu[0x43] << 3) | ((uint32_t) fpu[0x44] << 4)
                            | ((uint32_t) fpu[0x45] << 5) | (precision << 8) | (rounding << 10)
                            | ((uint32_t) fpu[0x50] << 12));
    for (unsigned i = 0; i < 6; i++) fpu[0x40 + i] = (cw >> i) & 1;
    precision = (cw >> 8) & 3;
    rounding = (cw >> 10) & 3;
    __builtin_memcpy(fpu + 0x48, &precision, 4);
    __builtin_memcpy(fpu + 0x4c, &rounding, 4);
    fpu[0x50] = (cw >> 12) & 1;
    uint64_t bits;
    __builtin_memcpy(&bits, &v, 8);
    uint64_t r = ff7_d2lz_bits(bits);
    e->regs[HW_EAX] = (uint32_t) r;
    e->regs[HW_EDX] = (uint32_t) (r >> 32);
    e->check_event();
    return r;
}

/*
 * Runs 0x651A6D (src, v, out) as called (x86 ESP at the return address).
 * Returns 0, nothing done, if the x87 stack top is not 0..7.
 */
static inline int highway_color_run(highway_env * e) {
    volatile uint32_t * regs = e->regs;
    uint8_t * fpu = e->fpu;
    uint32_t top;
    __builtin_memcpy(&top, fpu + HW_TOP, 4);
    if (top > 7) return 0;
    uint32_t esp = regs[HW_ESP];
    uint32_t src = hw_rd32(e, esp + 4), v = hw_rd32(e, esp + 8), out = hw_rd32(e, esp + 12);
    e->counters[0]++;

    // 0x651A2B (m, v)
    uint32_t m = hw_rd32(e, hw_rd32(e, 0xd858e0) + 0x14);
    double z = (double) hw_rdf(e, m + 0x08) * (double) hw_rdf(e, v);
    z = z + (double) hw_rdf(e, m + 0x18) * (double) hw_rdf(e, v + 4);
    z = z + (double) hw_rdf(e, m + 0x28) * (double) hw_rdf(e, v + 8);
    z = z + (double) hw_rdf(e, m + 0x38);
    uint32_t eax = m, ecx = m, edx = v;
    e->check_event();
    float zf = (float) z;

    // 0x6519D3 (zf)
    eax = hw_bits(zf);
    int32_t near = (int32_t) hw_rd32(e, 0xd85958);
    double fog;
    hw_fcomp(e, (double) near, zf);
    eax = hw_fnstsw(e, eax);
    if (!(fpu[HW_C0] | fpu[HW_C3])) {
        fog = hw_rdf(e, 0x7b79a4);
    } else {
        int32_t far = (int32_t) hw_rd32(e, 0xd858fc);
        hw_fcomp(e, (double) far, zf);
        eax = hw_fnstsw(e, eax);
        if (fpu[HW_C0]) {
            fog = hw_rdf(e, 0x7b79a8);
        } else {
            double d = (double) zf - (double) near;
            int32_t span = (int32_t) ((uint32_t) far - (uint32_t) near);
            eax = (uint32_t) span;
            if (span == 0) fpu[HW_ZE] = 1;
            else d = d / (double) span;
            fog = (double) hw_rdf(e, 0x7b79a4) - d;
        }
    }
    e->check_event();
    float f = (float) fog;

    // Channels out[2], out[1], out[0]
    float sat = hw_rdf(e, 0x7b79ac);
    uint8_t * s = e->mk_address(src);
    for (int k = 2; k >= 0; k--) {
        uint32_t byte = s[k];
        // the x86 registers that load src[k]: k = 2: ecx = src, edx = byte;
        // k = 1: edx = src, eax = byte; k = 0: eax = src, ecx = byte
        if (k == 2) { ecx = src; edx = byte; }
        else if (k == 1) { edx = src; eax = byte; }
        else { eax = src; ecx = byte; }
        double t = (double) (int32_t) byte * (double) f;
        float tf = (float) t; // fst: st0 stays double
        hw_fcomp(e, t, sat);
        eax = hw_fnstsw(e, eax);
        uint8_t b;
        if (fpu[HW_C0]) {
            uint64_t r = hw_ftol(e, (double) tf);
            eax = (uint32_t) r;
            edx = (uint32_t) (r >> 32);
            b = (uint8_t) r;
        } else {
            b = 0xff;
        }
        *e->mk_address(out + (uint32_t) k) = b;
        // the store: k = 2: eax = out, cl = b; k = 1: ecx = out, dl = b;
        // k = 0: edx = out, al = b
        if (k == 2) { eax = out; ecx = (ecx & ~0xffu) | b; }
        else if (k == 1) { ecx = out; edx = (edx & ~0xffu) | b; }
        else { edx = out; eax = (eax & ~0xffu) | b; }
    }

    regs[HW_EAX] = eax;
    regs[HW_ECX] = ecx;
    regs[HW_EDX] = edx;
    e->check_event();
    regs[HW_ESP] = esp + 4; // ret
    return 1;
}

#endif // FF7_HIGHWAY_CORE_H
