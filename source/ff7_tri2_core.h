/*
 * Native renderer for C_006A8A3E (highway minigame triangle renderer),
 * shared by ff7_tri.c and the offline test.
 *
 * C_006A8A3E(count, indices, verts) — called from within a glBegin/glEnd
 * block by the highway module.  The vertices are already in screen space
 * (no matrix multiply): each t_dx_rend_vertex_20 is 32 bytes:
 *   +0   float x
 *   +4   float y
 *   +8   float z
 *   +12  (unused here)
 *   +16  uint8  b
 *   +17  uint8  g
 *   +18  uint8  r
 *   +19  uint8  a
 *   +20  (pad)
 *   +24  float u
 *   +28  float v
 *
 * Per triangle (indices uint16, vertex stride 32), as the .so does it:
 *   fake_glColor4ub(v0 r, g, b, a)    once, from the FIRST vertex (+18, +17,
 *                                     +16, +19): the triangle is flat
 *   for v0, v1, v2:
 *     glTexCoord2f(u, v)              (+24, +28) into the current u/v
 *     glVertex3f(x, y, z)             (+0, +4, +8): the vertex gets x, y, z,
 *                                     the current color and u/v; nothing is
 *                                     written when the buffer is full
 * so after it the current color is the last triangle's first vertex and the
 * current u/v its last vertex (later glVertex3f calls use them).
 *
 * v0.6.20: the first version took the color of every vertex (smooth instead
 * of flat) and left the current u/v as they were; both differ from the
 * original (the snowboard's trees stretched over the scenery).
 *
 * No w-divide, no matrix, no keep_alpha logic.  The function ends with a
 * tail-call to glEnd(), so the native version must do the same.
 *
 * Bit-exact: build without -ffast-math and without FP contraction.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_TRI2_CORE_H
#define FF7_TRI2_CORE_H

#include <stdint.h>
#include <string.h>

// glTexCoord2f + glVertex3f for one vertex; the color is the current one
static inline void tri2_core_emit(const uint8_t * vtx, uint8_t ** cur, const uint8_t * last,
                                  const uint8_t col[4], uint32_t * cur_u, uint32_t * cur_v) {
    memcpy(cur_u, vtx + 24, 4);
    memcpy(cur_v, vtx + 28, 4);
    uint8_t * p = *cur;
    if (p >= last) return;
    memcpy(p, vtx, 12); // x, y, z
    memcpy(p + 12, col, 4);
    memcpy(p + 16, cur_u, 4);
    memcpy(p + 20, cur_v, 4);
    *cur = p + 0x18;
}

/*
 * The loop between glBegin(GL_TRIANGLES) and glEnd() for C_006A8A3E.
 * `s` is the same tri_state as used by tri_core_draw (shares pCurVertex,
 * pLastVertex and the current color and u/v).
 */
static inline void tri2_core_draw(const tri_state * s, int count,
                                   const uint16_t * idx,
                                   const uint8_t * verts) {
    uint8_t * cur = *s->cur;
    const uint8_t * last = *s->last;
    uint32_t * cur_u = (uint32_t *) s->u, * cur_v = (uint32_t *) s->v;

    for (; count > 0; count -= 3, idx += 3) {
        const uint8_t * v0 = verts + 32u * idx[0];
        const uint8_t * v1 = verts + 32u * idx[1];
        const uint8_t * v2 = verts + 32u * idx[2];
        const uint8_t col[4] = { v0[18], v0[17], v0[16], v0[19] }; // r, g, b, a
        *s->r = col[0];
        *s->g = col[1];
        *s->b = col[2];
        *s->a = col[3];
        tri2_core_emit(v0, &cur, last, col, cur_u, cur_v);
        tri2_core_emit(v1, &cur, last, col, cur_u, cur_v);
        tri2_core_emit(v2, &cur, last, col, cur_u, cur_v);
    }
    *s->cur = cur;
}

#endif /* FF7_TRI2_CORE_H */
