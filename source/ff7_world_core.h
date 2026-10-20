/*
 * World map vertex math of ff7.exe, shared by ff7_world.c and the offline
 * test that compares it with the recompiled code under emulation
 * (scripts/emu/test_world.py). Bit-exact: build without -ffast-math and
 * without FP contraction.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_WORLD_CORE_H
#define FF7_WORLD_CORE_H

#include <stdint.h>

#define WORLD_VERTICES  126 // transformed by 0x75F263 (3 + 3 * 41)
#define WORLD_PROJECTED 123 // projected (the last batch is transformed only)

// 0x66CE40 (porters' float code): o = v * m, m 4x4 row-major, v (x, y, z, 1).
// Each row is m[4 + j] * y + m[8 + j] * z + m[j] * x + m[12 + j], rounded to
// float after every step (vmul / vmla / vadd.f32, no fused multiply-add).
static inline void world_core_transform(const float m[16], const float v[3], float o[4]) {
    for (int j = 0; j < 4; j++) {
        float s = m[4 + j] * v[1];
        s = s + m[8 + j] * v[2];
        s = s + m[j] * v[0];
        o[j] = s + m[12 + j];
    }
}

// vcvt.s32.f32 (0x75F090, the recompiled fld / fistp): truncates,
// saturates, NaN -> 0
static inline int32_t world_core_int(float f) {
    if (f != f) return 0;
    if (f >= 2147483648.0f) return 2147483647;
    if (f <= -2147483648.0f) return (int32_t) 0x80000000u;
    return (int32_t) f;
}

/*
 * 0x75F263: transforms in[0..125] by m into out[] (x, y, z, w), projects
 * out[0..122] into proj[] (x and y only: proj[j][2] is not written) and sets
 * the clip flags of each projected vertex:
 *
 *     inv = k / w           (in double, stored as float; w == 0: inv = k and
 *                            the x87 zero-divide flag is set, as the
 *                            recompiled fdiv does)
 *     px = x * inv, py = y * inv   (in double, stored as float)
 *     flags = (a <= (int) px < c) | (b <= (int) py < d) << 8
 *
 * The x86 code transforms three vertices, then projects the previous three,
 * 41 times. Returns 1 if a w was 0.
 */
static inline int world_core_project(const float m[16], const float (* in)[3], float (* out)[4], float (* proj)[3],
                                     int32_t * flags, float k, int32_t a, int32_t b, int32_t c, int32_t d) {
    int zero_divide = 0;
    for (int i = 0; i < WORLD_VERTICES; i++) world_core_transform(m, in[i], out[i]);
    for (int j = 0; j < WORLD_PROJECTED; j++) {
        float w = out[j][3], inv;
        if (w == 0.0f) {
            inv = k;
            zero_divide = 1;
        } else {
            inv = (float) ((double) k / (double) w);
        }
        float px = (float) ((double) out[j][0] * (double) inv);
        float py = (float) ((double) out[j][1] * (double) inv);
        proj[j][0] = px;
        proj[j][1] = py;
        int32_t ix = world_core_int(px), iy = world_core_int(py);
        flags[j] = (int32_t) ((ix < c && ix >= a) | ((iy < d && iy >= b) << 8));
    }
    return zero_divide;
}

#define WORLD_MESH_VERTICES 122 // 0x75F0AD's loop

/*
 * The loop of 0x75F0AD (after it has set up the matrices m1 = 0xE39738 and
 * m2 = 0xE39778), for vertices src[i] (int16 x, y, z, pad):
 *
 *     in[i] = (float) src[i]
 *     local = in[i] * m1                       (0x66CE40)
 *     proj[i].z = row z of in[i] * m2          (0x66CF4D: m2[6] y + m2[10] z + m2[2] x + m2[14])
 *     acc -= step; i++
 *     if acc > 0:                              (world curvature)
 *         t1 = acc * k1; v = |v - c|; t2 = v * k2          (floats via double)
 *         s = t1 + t2 (double); t = s * (float) s           (float)
 *         in[i - 2].y -= t
 *     acc = (int) proj[i - 1].z
 *     v = (int) (local.x / local.w)            (w == 0: local.x, zero-divide flag)
 *     acc >>= 2                                (arithmetic)
 *
 * with acc = 0 and v = v0 (an uninitialized x86 local: the caller reads it
 * from the x86 stack) at the start. in[-1] must be writable (in[i - 2] on
 * the first vertex when step < 0). Integer arithmetic wraps like the x86
 * code. Returns 1 if a w was 0; *eax / *ecx get the x86 EAX (v) and ECX (acc).
 */
static inline int world_core_mesh(const int16_t * src, int32_t step, const float m1[16], const float m2[16],
                                  float (* in)[3], float (* proj)[3], int32_t c, int32_t v0, float k1, float k2,
                                  int32_t * eax, int32_t * ecx) {
    int32_t acc = 0, v = v0;
    int zero_divide = 0;
    for (int i = 0; i < WORLD_MESH_VERTICES;) {
        const int16_t * p = src + 4 * i;
        in[i][0] = (float) p[0];
        in[i][1] = (float) p[1];
        in[i][2] = (float) p[2];
        float local[4];
        world_core_transform(m1, in[i], local);
        float z = m2[6] * in[i][1];
        z = z + m2[10] * in[i][2];
        z = z + m2[2] * in[i][0];
        proj[i][2] = z + m2[14];
        acc = (int32_t) ((uint32_t) acc - (uint32_t) step);
        i++;
        if (acc > 0) {
            float t1 = (float) ((double) acc * (double) k1);
            v = (int32_t) ((uint32_t) v - (uint32_t) c);
            if (v < 0) v = (int32_t) (0u - (uint32_t) v);
            float t2 = (float) ((double) v * (double) k2);
            double s = (double) t1 + (double) t2;
            float t = (float) (s * (double) (float) s);
            in[i - 2][1] = (float) ((double) in[i - 2][1] - (double) t);
        }
        acc = world_core_int(proj[i - 1][2]);
        float q;
        if (local[3] == 0.0f) {
            q = local[0];
            zero_divide = 1;
        } else {
            q = (float) ((double) local[0] / (double) local[3]);
        }
        v = world_core_int(q);
        acc >>= 2;
    }
    *eax = v;
    *ecx = acc;
    return zero_divide;
}

enum { WORLD_LIGHT_NEG, WORLD_LIGHT_SAT, WORLD_LIGHT_NORMAL };

/*
 * 0x68DAE1 (m, n, rgb, src, out): vertex color, l = m (3x4) * n computed by
 * 0x66CFE8 (m1*n1 + m2*n2 + m0*n0 + m3 per row, float). Per channel
 * (out[2] <- rgb[2], l[0]; out[1] <- rgb[1], l[1]; out[0] <- rgb[0], l[2]):
 *
 *     l < zero:            0                   (NEG)
 *     t = rgb * l (float); t >= sat:   0xff    (SAT)
 *     otherwise:           (int) t             (NORMAL, vcvt.s32.f64: truncates)
 *
 * zero and sat are ff7.exe's constants 0.0 and 255.0. out[3] = src[3] is
 * left to the caller. Returns 0 if a NaN takes part in a comparison (x87
 * unordered: left to the original). `path` gets each channel's path in the
 * order the x86 code runs them (out[2], out[1], out[0]); `lt`/`eq` the last
 * comparison (x87 C0 / C3).
 */
static inline int world_core_light(const float m[12], const float n[3], const float rgb[3], float zero, float sat,
                                   uint8_t out[3], int path[3], int * lt, int * eq) {
    float l[3];
    for (int r = 0; r < 3; r++) {
        const float * row = m + 4 * r;
        float s = row[1] * n[1];
        s = s + row[2] * n[2];
        s = s + row[0] * n[0];
        l[r] = s + row[3];
    }
    for (int c = 0; c < 3; c++) {
        int k = 2 - c; // out[k], rgb[k], l[c]
        if (l[c] != l[c] || zero != zero) return 0;
        if (l[c] < zero) {
            out[k] = 0;
            path[c] = WORLD_LIGHT_NEG;
            *lt = 1;
            *eq = 0;
            continue;
        }
        float t = (float) ((double) rgb[k] * (double) l[c]);
        if (t != t || sat != sat) return 0;
        *lt = t < sat;
        *eq = t == sat;
        if (!(t < sat)) {
            out[k] = 0xff;
            path[c] = WORLD_LIGHT_SAT;
        } else {
            out[k] = (uint8_t) world_core_int(t); // |t| < 2^31 here unless t is huge negative: saturates the same
            path[c] = WORLD_LIGHT_NORMAL;
        }
    }
    return 1;
}

enum { WORLD_EDX_M, WORLD_EDX_OUT, WORLD_EDX_RGB };

/*
 * EDX after 0x68DAE1, bits 8-31 (bits 0-7 are src[3]). The x86 code loads
 * EDX = m to pass it to 0x66CFE8 (which leaves the registers alone), then
 * EDX = out on the SAT path of out[2] and on the NEG and NORMAL paths of
 * out[1], and EDX = rgb whenever out[0] is not NEG (to read rgb[0]).
 */
static inline int world_core_light_edx(const int path[3]) {
    if (path[2] != WORLD_LIGHT_NEG) return WORLD_EDX_RGB;
    if (path[1] != WORLD_LIGHT_SAT || path[0] == WORLD_LIGHT_SAT) return WORLD_EDX_OUT;
    return WORLD_EDX_M;
}

#endif // FF7_WORLD_CORE_H
