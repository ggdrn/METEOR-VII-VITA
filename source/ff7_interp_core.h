/*
 * Battle at 30 fps (docs/BATALHA-30FPS.md): pairs the draws and vertices of
 * a battle frame with the previous frame's, and places the vertices of an
 * in-between frame halfway. Shared by ff7_interp.c and the host test
 * scripts/test_interp.c. Plain C, no Vita headers.
 *
 * A frame is a list of draws: state key, primitive, vertex count and where
 * its x/y(/z) and u/v floats are (no copy: ff7_interp.c points at the
 * frame's recording, which stays untouched until the frame after next).
 *
 * Matching, per pair of frames A (previous) and B (current):
 *  - draws: by state key, in order (the k-th draw with key K in B pairs with
 *    the k-th with key K in A);
 *  - fast case: same primitive, same count and the same u/v (a hash of all
 *    of them, computed once per draw): the same mesh, vertex i with vertex i;
 *  - otherwise (GL_TRIANGLES): triangles by a hash of the u/v of their 3
 *    vertices, the first unused equal triangle of A in insertion order (so
 *    repeated u/v pair up in order); map[] keeps each vertex's pair;
 *  - the rest has no pair (drawn as in B).
 * A pair of frames where most moving vertices (every 4th of a mesh, all of
 * the triangles paired by u/v) jump more than IP_CUT_PX is a cut (camera
 * cut, scene change).
 *
 * ip_mid writes the in-between positions of one draw of B into a copy of
 * its vertices; a whole triangle stays as in B when one of its vertices has
 * no pair or jumps more than IP_CUT_PX (vertices thrown off screen).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_INTERP_CORE_H
#define FF7_INTERP_CORE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define IP_TRIANGLES 4      // GL_TRIANGLES
#define IP_MOVE_PX   0.01f  // below: a vertex that stays
#define IP_CUT_PX    96.0f  // above: a jump, not a motion
#define IP_SAMPLE    4      // vertices of a mesh looked at for cuts and figures: every 4th

enum { IP_NONE, IP_FAST, IP_TRIS };

typedef struct {
    uint32_t key, mode, count, rec; // rec: the caller's tag (offset of the draw in the recording)
    uint32_t first;                 // index of its first vertex in the frame (map)
    const uint8_t * pos, * uv;      // x, y(, z) floats / u, v floats (uv NULL: zeros)
    uint16_t ps, us;                // strides
    uint8_t pos_z, kind;            // kind: IP_NONE / IP_FAST / IP_TRIS after ip_match (frame B)
    uint32_t uvh;                   // hash of the u/v, 0 until computed
    int32_t pair;                   // A's draw, -1: none
} ip_draw;

typedef struct {
    ip_draw * d;
    uint32_t nd, nv, maxd, maxv;
    uint32_t overflow; // draws or vertices beyond the buffers: not matched
} ip_frame;

typedef struct {
    uint32_t key;
    int32_t head;
} ip_keyslot;

typedef struct {
    uint32_t h;
    uint32_t tri; // triangle + 1 (0: empty), top bit: used
} ip_trislot;

// Scratch for ip_match: sized by the caller
typedef struct {
    ip_keyslot * keys;   // key_size (power of two) >= 2 * maxd
    int32_t * next;      // maxd
    ip_trislot * tris;   // tri_size (power of two)
    uint32_t key_size, tri_size;
} ip_scratch;

typedef struct {
    uint32_t pairs, cuts, overflows;
    uint64_t draws, draws_matched, verts, fast, by_uv, unmatched, moving, far, sampled;
    uint64_t draws_tri, draws_other;
    double dsum;
    float dmax;
} ip_stats;

static inline void ip_reset(ip_frame * f) {
    f->nd = f->nv = 0;
    f->overflow = 0;
}

static inline uint32_t ip_mix(uint32_t h, uint32_t w) {
    h ^= w;
    h *= 0x85ebca6bu;
    return h ^ (h >> 13);
}

// One draw (no copy: pos and uv must stay valid while the frame is matched or replayed)
static inline int ip_add(ip_frame * f, uint32_t key, uint32_t mode, uint32_t count, const uint8_t * pos, uint32_t ps,
                         int pos_z, const uint8_t * uv, uint32_t us, uint32_t rec) {
    if (f->nd >= f->maxd || count > f->maxv - f->nv || ps > 0xffff || us > 0xffff) {
        f->overflow = 1;
        return 0;
    }
    ip_draw * d = &f->d[f->nd++];
    d->key = key;
    d->mode = mode;
    d->count = count;
    d->rec = rec;
    d->first = f->nv;
    d->pos = pos;
    d->uv = uv;
    d->ps = (uint16_t) ps;
    d->us = (uint16_t) us;
    d->pos_z = pos_z != 0;
    d->kind = IP_NONE;
    d->uvh = 0;
    d->pair = -1;
    f->nv += count;
    return 1;
}

static inline void ip_xyz(const ip_draw * d, uint32_t i, float o[3]) {
    memcpy(o, d->pos + i * d->ps, d->pos_z ? 12 : 8);
    if (!d->pos_z) o[2] = 0.0f;
}

static inline void ip_uv(const ip_draw * d, uint32_t i, uint32_t o[2]) {
    if (d->uv) memcpy(o, d->uv + i * d->us, 8);
    else o[0] = o[1] = 0;
}

static inline uint32_t ip_uv_hash(ip_draw * d) {
    if (!d->uvh) {
        uint32_t h = 0x9e3779b9u ^ d->count;
        if (d->uv)
            for (uint32_t i = 0; i < d->count; i++) {
                uint32_t t[2];
                memcpy(t, d->uv + i * d->us, 8);
                h = ip_mix(ip_mix(h, t[0]), t[1]);
            }
        d->uvh = h ? h : 1;
    }
    return d->uvh;
}

typedef struct {
    uint64_t moving, far, sampled;
    double dsum;
    float dmax;
} ip_disp;

static inline void ip_moved(const float a[3], const float b[3], ip_disp * p) {
    float dx = b[0] - a[0], dy = b[1] - a[1];
    float d2 = dx * dx + dy * dy;
    p->sampled++;
    if (!(d2 > IP_MOVE_PX * IP_MOVE_PX)) return; // also NaN
    float d = sqrtf(d2);
    p->moving++;
    p->dsum += d;
    if (d > p->dmax) p->dmax = d;
    if (d > IP_CUT_PX) p->far++;
}

static inline uint32_t ip_tri_hash(const ip_draw * d, uint32_t t) {
    uint32_t h = 0x9e3779b9u;
    for (uint32_t k = 0; k < 3; k++) {
        uint32_t uv[2];
        ip_uv(d, 3 * t + k, uv);
        h = ip_mix(ip_mix(h, uv[0]), uv[1]);
    }
    return h ? h : 1;
}

static inline int ip_same_tri_uv(const ip_draw * a, uint32_t ta, const ip_draw * b, uint32_t tb) {
    for (uint32_t k = 0; k < 3; k++) {
        uint32_t x[2], y[2];
        ip_uv(a, 3 * ta + k, x);
        ip_uv(b, 3 * tb + k, y);
        if (x[0] != y[0] || x[1] != y[1]) return 0;
    }
    return 1;
}

// Triangles of b paired with triangles of a by u/v; returns the vertices matched (or -1: table too small).
// map (b->count entries): each vertex of b gets its pair's index in a, or -1
static inline int64_t ip_match_tris(const ip_draw * a, const ip_draw * b, ip_scratch * s, ip_disp * p, int32_t * map) {
    uint32_t ta = a->count / 3, tb = b->count / 3, size = 16;
    while (size < 2 * ta) size <<= 1;
    if (size > s->tri_size) return -1;
    uint32_t mask = size - 1;
    memset(s->tris, 0, size * sizeof(ip_trislot));
    for (uint32_t t = 0; t < ta; t++) {
        uint32_t h = ip_tri_hash(a, t), i = h & mask;
        while (s->tris[i].tri) i = (i + 1) & mask;
        s->tris[i].h = h;
        s->tris[i].tri = t + 1;
    }
    for (uint32_t v = 0; v < b->count; v++) map[v] = -1;
    int64_t matched = 0;
    for (uint32_t t = 0; t < tb; t++) {
        uint32_t h = ip_tri_hash(b, t), i = h & mask;
        for (; s->tris[i].tri; i = (i + 1) & mask) {
            ip_trislot * e = &s->tris[i];
            if (e->h != h || (e->tri & 0x80000000u)) continue;
            uint32_t at = e->tri - 1;
            if (!ip_same_tri_uv(a, at, b, t)) continue;
            e->tri |= 0x80000000u;
            for (uint32_t k = 0; k < 3; k++) {
                float x[3], y[3];
                ip_xyz(a, 3 * at + k, x);
                ip_xyz(b, 3 * t + k, y);
                ip_moved(x, y, p);
                map[3 * t + k] = (int32_t) (3 * at + k);
            }
            matched += 3;
            break;
        }
    }
    return matched;
}

/*
 * Pairs B's draws (and vertices) with A's and adds the figures to st. map:
 * B->nv entries (used by the draws paired triangle by triangle). Returns 1
 * for a cut (or frames that overflowed), 0 otherwise.
 */
static inline int ip_match(ip_frame * A, ip_frame * B, ip_scratch * s, ip_stats * st, int32_t * map) {
    for (uint32_t j = 0; j < B->nd; j++) {
        B->d[j].kind = IP_NONE;
        B->d[j].pair = -1;
    }
    if (A->overflow || B->overflow) {
        st->overflows++;
        return 1;
    }
    // Keys of A: per key, its draws in order (head, then next[])
    uint32_t kmask = s->key_size - 1;
    for (uint32_t i = 0; i <= kmask; i++) s->keys[i].head = -2; // empty
    for (int32_t i = (int32_t) A->nd - 1; i >= 0; i--) {
        uint32_t k = A->d[i].key, j = ip_mix(k, 0x27d4eb2fu) & kmask;
        while (s->keys[j].head != -2 && s->keys[j].key != k) j = (j + 1) & kmask;
        s->next[i] = s->keys[j].head == -2 ? -1 : s->keys[j].head;
        s->keys[j].key = k;
        s->keys[j].head = i;
    }
    ip_disp p = { 0, 0, 0, 0.0, 0.0f };
    uint64_t fast = 0, by_uv = 0, unmatched = 0, matched_draws = 0;
    for (uint32_t j = 0; j < B->nd; j++) {
        ip_draw * bd = &B->d[j];
        if (bd->mode == IP_TRIANGLES) st->draws_tri++;
        else st->draws_other++;
        uint32_t k = bd->key, q = ip_mix(k, 0x27d4eb2fu) & kmask;
        while (s->keys[q].head != -2 && s->keys[q].key != k) q = (q + 1) & kmask;
        int32_t i = s->keys[q].head == -2 ? -1 : s->keys[q].head;
        if (i < 0) {
            unmatched += bd->count;
            continue;
        }
        s->keys[q].head = s->next[i];
        matched_draws++;
        ip_draw * ad = &A->d[i];
        bd->pair = i;
        if (ad->mode == bd->mode && ad->count == bd->count && ip_uv_hash(ad) == ip_uv_hash(bd)) {
            bd->kind = IP_FAST;
            for (uint32_t v = 0; v < bd->count; v += IP_SAMPLE) {
                float x[3], y[3];
                ip_xyz(ad, v, x);
                ip_xyz(bd, v, y);
                ip_moved(x, y, &p);
            }
            fast += bd->count;
        } else if (ad->mode == IP_TRIANGLES && bd->mode == IP_TRIANGLES) {
            int64_t m = ip_match_tris(ad, bd, s, &p, map + bd->first);
            if (m > 0) bd->kind = IP_TRIS;
            if (m < 0) m = 0;
            by_uv += (uint64_t) m;
            unmatched += bd->count - (uint64_t) m;
        } else {
            unmatched += bd->count;
        }
    }
    st->pairs++;
    st->draws += B->nd;
    st->draws_matched += matched_draws;
    st->verts += B->nv;
    st->fast += fast;
    st->by_uv += by_uv;
    st->unmatched += unmatched;
    if (p.moving > 25 && 2 * p.far > p.moving) {
        st->cuts++;
        return 1;
    }
    st->moving += p.moving;
    st->sampled += p.sampled;
    st->far += p.far;
    st->dsum += p.dsum;
    if (p.dmax > st->dmax) st->dmax = p.dmax;
    return 0;
}

static inline int ip_near(const float a[3], const float b[3]) {
    float dx = b[0] - a[0], dy = b[1] - a[1];
    return dx * dx + dy * dy <= IP_CUT_PX * IP_CUT_PX; // false for NaN
}

/*
 * The in-between positions of B's draw bd (matched by ip_match against A)
 * written over a copy of its vertices: x, y(, z) floats at out (stride os),
 * holding B's own values. Triangles (IP_TRIANGLES) move only if their 3
 * vertices have a pair near enough; other primitives vertex by vertex.
 * Returns the vertices moved.
 */
static inline uint32_t ip_mid(const ip_frame * A, const ip_draw * bd, const int32_t * map, uint8_t * out, uint32_t os) {
    if (bd->kind == IP_NONE || bd->pair < 0) return 0;
    const ip_draw * ad = &A->d[bd->pair];
    const int32_t * m = map + bd->first;
    uint32_t moved = 0, group = bd->mode == IP_TRIANGLES ? 3 : 1, n = bd->pos_z ? 12 : 8;
    for (uint32_t i = 0; i + group <= bd->count; i += group) {
        float a[3][3], b[3][3] = { { 0 } };
        int ok = 1;
        for (uint32_t k = 0; k < group && ok; k++) {
            int32_t ai = bd->kind == IP_FAST ? (int32_t) (i + k) : m[i + k];
            memcpy(b[k], out + (i + k) * os, n);
            ok = ai >= 0 && (uint32_t) ai < ad->count;
            if (ok) {
                ip_xyz(ad, (uint32_t) ai, a[k]);
                ok = ip_near(a[k], b[k]);
            }
        }
        if (!ok) continue;
        for (uint32_t k = 0; k < group; k++) {
            float v[3] = { 0.5f * (a[k][0] + b[k][0]), 0.5f * (a[k][1] + b[k][1]), 0.5f * (a[k][2] + b[k][2]) };
            memcpy(out + (i + k) * os, v, n);
        }
        moved += group;
    }
    return moved;
}

#endif // FF7_INTERP_CORE_H
