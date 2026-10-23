/*
 * Native renderers for C_006A8A3E (highway minigame triangle renderer) and
 * its siblings C_006A8242, C_006A83AE, C_006A8BFC (tri2_core_draw_mode),
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

/*
 * The porters' four renderers of pre-projected triangle lists, each
 * (count, indices, verts) with its own glBegin(GL_TRIANGLES) / glEnd():
 *
 *              color                     u, v
 *   C_006A8242 first vertex (flat)       -                (the current ones)
 *   C_006A83AE every vertex (smooth)     -
 *   C_006A8A3E first vertex (flat)       every vertex
 *   C_006A8BFC every vertex (smooth)     every vertex
 *
 * fake_glColor4ub(+18, +17, +16, +19) sets the current r, g, b, a,
 * glTexCoord2f(+24, +28) the current u, v, and glVertex3f(+0, +4, +8) writes
 * x, y, z with the current color and u, v (nothing when the buffer is full).
 * The loop runs while count > 0, 3 indices a triangle. `s` is the tri_state
 * of tri_core_draw (pCurVertex, pLastVertex, the current color and u/v).
 */
enum { TRI2_FLAT = 0, TRI2_SMOOTH = 1, TRI2_NO_UV = 0, TRI2_UV = 2 };

static inline void tri2_core_draw_mode(const tri_state * s, int mode, int count, const uint16_t * idx,
                                       const uint8_t * verts) {
    uint8_t * cur = *s->cur;
    const uint8_t * last = *s->last;
    uint32_t * cur_u = (uint32_t *) s->u, * cur_v = (uint32_t *) s->v;
    uint8_t col[4] = { *s->r, *s->g, *s->b, *s->a };

    for (; count > 0; count -= 3, idx += 3) {
        for (int k = 0; k < 3; k++) {
            const uint8_t * v = verts + 32u * idx[k];
            if (k == 0 || (mode & TRI2_SMOOTH)) {
                col[0] = v[18]; // r, g, b, a
                col[1] = v[17];
                col[2] = v[16];
                col[3] = v[19];
            }
            if (mode & TRI2_UV) {
                memcpy(cur_u, v + 24, 4);
                memcpy(cur_v, v + 28, 4);
            }
            if (cur < last) {
                memcpy(cur, v, 12); // x, y, z
                memcpy(cur + 12, col, 4);
                memcpy(cur + 16, cur_u, 4);
                memcpy(cur + 20, cur_v, 4);
                cur += 0x18;
            }
        }
    }
    *s->r = col[0];
    *s->g = col[1];
    *s->b = col[2];
    *s->a = col[3];
    *s->cur = cur;
}

// C_006A8A3E (the highway and snowboard minigames)
static inline void tri2_core_draw(const tri_state * s, int count, const uint16_t * idx, const uint8_t * verts) {
    tri2_core_draw_mode(s, TRI2_FLAT | TRI2_UV, count, idx, verts);
}

#endif /* FF7_TRI2_CORE_H */
