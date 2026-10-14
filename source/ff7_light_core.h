/*
 * Vertex lighting math of ff7.exe, shared by ff7_light.c and the offline
 * tests that compare it with the recompiled code under emulation.
 * Bit-exact: build without -ffast-math and without FP contraction.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_LIGHT_CORE_H
#define FF7_LIGHT_CORE_H

#include <stdint.h>

// 0x66CFE8: l = m (3x4) * n, as plain float code (m1*n1 + m2*n2 + m0*n0 + m3)
static inline void light_core_transform(const float m[12], const float n[3], float l[3]) {
    for (int r = 0; r < 3; r++) {
        const float * row = m + 4 * r;
        float s = row[1] * n[1];
        s = s + row[2] * n[2];
        s = s + row[0] * n[0];
        l[r] = s + row[3];
    }
}

// vcvt.s32.f64 (the recompiled fistp): truncates, saturates, NaN -> 0
static inline uint8_t light_core_byte(double v) {
    int32_t i;
    if (v != v) i = 0;
    else if (v >= 2147483647.0) i = 2147483647;
    else if (v <= -2147483648.0) i = (int32_t) 0x80000000u;
    else i = (int32_t) v;
    return (uint8_t) i;
}

enum { LIGHT2_NORMAL, LIGHT2_AMBIENT, LIGHT2_SATURATED };

/*
 * 0x68DE75 (color, scale[3], m, n, rgb[3], src, out): per channel k
 * (out[k] <- rgb[k], scale[k], l[2 - k], ambient m[3 + 4 * (2 - k)]):
 *  - l < ambient:  out = rgb * ambient * scale, all in double
 *  - otherwise:    t = (float) (rgb * l); if t < 255, out = t * scale;
 *                  else out = byte k of `color` (the scale is not applied)
 * out[3] = src[3]. Returns 0 when a NaN takes part in a comparison (the
 * x87 unordered compare follows other paths: leave those to the original).
 * `last` is the path of the last channel (out[0]); `lt`/`eq` the result of
 * the last comparison the original makes (C0 / C3).
 */
static inline int light_core_vertex2(uint32_t color, const float scale[3], const float m[12], const float n[3],
                                     const float rgb[3], uint8_t src3, uint8_t out[4],
                                     int * last, int * lt, int * eq) {
    float l[3];
    light_core_transform(m, n, l);
    for (int k = 2; k >= 0; k--) {
        float li = l[2 - k], amb = m[3 + 4 * (2 - k)];
        if (li != li || amb != amb) return 0;
        if (li < amb) {
            out[k] = light_core_byte((double) rgb[k] * (double) amb * (double) scale[k]);
            *last = LIGHT2_AMBIENT;
            *lt = 1;
            *eq = 0;
            continue;
        }
        float t = (float) ((double) rgb[k] * (double) li);
        if (t != t) return 0;
        if ((double) t < 255.0) {
            out[k] = light_core_byte((double) t * (double) scale[k]);
            *last = LIGHT2_NORMAL;
            *lt = 1;
            *eq = 0;
        } else {
            out[k] = (uint8_t) (color >> (8 * k));
            *last = LIGHT2_SATURATED;
            *lt = 0;
            *eq = (double) t == 255.0;
        }
    }
    out[3] = src3;
    return 1;
}

#endif // FF7_LIGHT_CORE_H
