/*
 * The porters' triangle renderer C_006A8002 (count, indices, vertices,
 * matrix), shared by ff7_tri.c and the offline tests. Bit-exact: build
 * without -ffast-math and without FP contraction.
 *
 * For every triangle (three 16-bit indices into 32-byte t_dx_rend_vertex_20
 * {x, y, z, ?, color, ...}): the three vertices are transformed by the 4x4
 * matrix (C_0066CE40); if each w is > 0 (checked in order, stopping at the
 * first one that is not), the three are emitted through the GL 1.x emulation:
 * fake_glColor4ub(color) - alpha forced to 0x80 when the "keep alpha" flag is
 * 0 - then glVertex3f(x / w, y / w, z / w), which appends a 0x18-byte vertex
 * {x, y, z, r, g, b, a, u, v} to VERTICES unless the buffer is full.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_TRI_CORE_H
#define FF7_TRI_CORE_H

#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t ** cur;         // pCurVertex
    uint8_t * const * last; // pLastVertex
    uint8_t * r, * g, * b, * a;
    const uint32_t * u, * v;
    const uint32_t * keep_alpha;
} tri_state;

// C_0066CE40: out = m (column-major 4x4) * (v, 1), multiply-accumulate with
// intermediate rounding (vmla.f32)
static inline void tri_core_transform(const float m[16], const float * v, float out[4]) {
    for (int j = 0; j < 4; j++) {
        float t = m[4 + j] * v[1];
        t = t + m[j] * v[0];
        t = t + m[8 + j] * v[2];
        out[j] = t + m[12 + j];
    }
}

static inline void tri_core_emit(const tri_state * s, const uint8_t * vtx, const float o[4], uint8_t ** cur,
                                 uint8_t * last, uint8_t col[4], uint32_t u, uint32_t v) {
    uint32_t c;
    memcpy(&c, vtx + 0x10, 4);
    col[0] = (uint8_t) (c >> 16);
    col[1] = (uint8_t) (c >> 8);
    col[2] = (uint8_t) c;
    col[3] = *s->keep_alpha ? (uint8_t) (c >> 24) : 0x80;
    float x = o[0] / o[3], y = o[1] / o[3], z = o[2] / o[3];
    uint8_t * p = *cur;
    if (p >= last) return;
    memcpy(p, &x, 4);
    memcpy(p + 4, &y, 4);
    memcpy(p + 8, &z, 4);
    memcpy(p + 0xc, col, 4);
    memcpy(p + 0x10, &u, 4);
    memcpy(p + 0x14, &v, 4);
    *cur = p + 0x18;
}

// The loop between glBegin(GL_TRIANGLES) and glEnd()
static inline void tri_core_draw(const tri_state * s, int count, const uint16_t * idx, const uint8_t * verts,
                                 const float m[16]) {
    uint8_t * cur = *s->cur, * last = *s->last;
    const uint32_t u = *s->u, v = *s->v;
    uint8_t col[4];
    int emitted = 0;
    for (int i = 0; count > 0; count -= 3, i += 3) {
        const uint8_t * v0 = verts + 32 * idx[i], * v1 = verts + 32 * idx[i + 1], * v2 = verts + 32 * idx[i + 2];
        float o0[4], o1[4], o2[4];
        tri_core_transform(m, (const float *) v0, o0);
        if (!(o0[3] > 0.0f)) continue;
        tri_core_transform(m, (const float *) v1, o1);
        if (!(o1[3] > 0.0f)) continue;
        tri_core_transform(m, (const float *) v2, o2);
        if (!(o2[3] > 0.0f)) continue;
        tri_core_emit(s, v0, o0, &cur, last, col, u, v);
        tri_core_emit(s, v1, o1, &cur, last, col, u, v);
        tri_core_emit(s, v2, o2, &cur, last, col, u, v);
        emitted = 1;
    }
    *s->cur = cur;
    if (emitted) { // fake_glColor4ub leaves the last color behind
        *s->r = col[0];
        *s->g = col[1];
        *s->b = col[2];
        *s->a = col[3];
    }
}

#endif // FF7_TRI_CORE_H
