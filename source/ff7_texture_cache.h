/*
 * Incremental reconversion of 8-bit paletted textures (ff7_texture.c).
 *
 * Fields with animated palettes (the chocobo ranch: ~10 pages of 256x256 a
 * frame) make the game call convert_image_data on the same page into the
 * same destination buffer (the texture set's +0xD8 buffer) every time the
 * palette changes: the indices are the same, only some colors differ. A full
 * conversion writes 256 KB a page (~1.5 ms on the Vita).
 *
 * For each destination buffer this keeps the last 256 converted colors and a
 * hash of the source indices. When the same indices go to the same buffer
 * again (same source, size and pitch, same hash) and spot checks show the
 * buffer still holds the last conversion, only the pixels whose color
 * changed are written (none if no used color changed). The buffer ends with
 * exactly what a full conversion writes. Anything else does the full
 * conversion and records it.
 *
 * Pure C (no SDK): scripts/test_texture_cache.c checks it on the host
 * against the full conversion.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_TEXTURE_CACHE_H
#define FF7_TEXTURE_CACHE_H

#include <stdint.h>
#include <string.h>

#define TC_ENTRIES 32
#define TC_SPOTS   32

typedef struct {
    uint8_t * dst;
    const uint8_t * src;
    int w, h, pitch;
    uint32_t hash, last_use;
    uint32_t conv[256];
    uint32_t count[256]; // pixels using each index
} tc_entry;

typedef struct {
    tc_entry e[TC_ENTRIES];
    uint32_t clock;
    unsigned full, partial, skipped; // conversions by kind (stats)
} tc_cache;

// Source indices (w * h contiguous bytes): two 32-bit lanes, 4 bytes a step
static inline uint64_t tc_hash64(const uint8_t * s, uint32_t n) {
    uint32_t a = 0x811c9dc5u, b = 0x9e3779b9u ^ n;
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t w;
        memcpy(&w, s + i, 4);
        a = (a ^ w) * 0x01000193u;
        b = (b + w) * 0x85ebca6bu;
        b ^= b >> 13;
    }
    for (; i < n; i++) {
        a = (a ^ s[i]) * 0x01000193u;
        b = (b + s[i]) * 0x85ebca6bu;
    }
    return (uint64_t) a << 32 | b;
}

static inline void tc_count(const uint8_t * s, uint32_t n, uint32_t * count) {
    memset(count, 0, 256 * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) count[s[i]]++;
}

static inline void tc_blit_full(uint8_t * dst, const uint8_t * src, int w, int h, int pitch, const uint32_t * conv) {
    for (int y = 0; y < h; y++, dst += pitch, src += w) {
        uint32_t * d = (uint32_t *) dst;
        int x = 0;
        for (; x + 4 <= w; x += 4) {
            uint32_t i0 = src[x], i1 = src[x + 1], i2 = src[x + 2], i3 = src[x + 3];
            d[x] = conv[i0];
            d[x + 1] = conv[i1];
            d[x + 2] = conv[i2];
            d[x + 3] = conv[i3];
        }
        for (; x < w; x++) d[x] = conv[src[x]];
    }
}

// The buffer still holds the conversion recorded in `e` (sampled pixels)
static inline int tc_spot_check(const tc_entry * e) {
    uint32_t n = (uint32_t) e->w * (uint32_t) e->h;
    for (uint32_t k = 0; k < TC_SPOTS; k++) {
        uint32_t p = (uint32_t) (((uint64_t) n * (2 * k + 1)) / (2 * TC_SPOTS));
        uint32_t y = p / (uint32_t) e->w, x = p % (uint32_t) e->w;
        uint32_t v;
        memcpy(&v, e->dst + (int32_t) y * e->pitch + 4 * x, 4);
        if (v != e->conv[e->src[p]]) return 0;
    }
    return 1;
}

/*
 * Writes the RGBA8888 conversion of the w x h indices at `src` (rows of w
 * bytes) into `dst` (rows `pitch` bytes apart, 4-byte aligned), colors from
 * `conv`.
 */
static inline void tc_convert(tc_cache * c, uint8_t * dst, const uint8_t * src, int w, int h, int pitch,
                              const uint32_t * conv) {
    if (w <= 0 || h <= 0) return;
    uint32_t n = (uint32_t) w * (uint32_t) h;
    uint64_t h64 = tc_hash64(src, n);
    uint32_t hash = (uint32_t) (h64 >> 32) ^ (uint32_t) h64 * 0x27d4eb2du;
    c->clock++;

    tc_entry * e = NULL, * victim = &c->e[0];
    for (int i = 0; i < TC_ENTRIES; i++) {
        tc_entry * t = &c->e[i];
        if (t->dst == dst) {
            e = t;
            break;
        }
        if (t->last_use < victim->last_use) victim = t;
    }

    if (e && e->src == src && e->w == w && e->h == h && e->pitch == pitch && e->hash == hash && tc_spot_check(e)) {
        uint8_t changed[256];
        uint32_t pixels = 0;
        for (int i = 0; i < 256; i++) {
            changed[i] = e->conv[i] != conv[i];
            if (changed[i]) pixels += e->count[i];
        }
        if (pixels > n / 4) { // most of the page: the plain loop is faster
            tc_blit_full(dst, src, w, h, pitch, conv);
            memcpy(e->conv, conv, sizeof(e->conv));
            c->full++;
        } else if (pixels) {
            uint8_t * d8 = dst;
            const uint8_t * s = src;
            for (int y = 0; y < h; y++, d8 += pitch, s += w) {
                uint32_t * d = (uint32_t *) d8;
                for (int x = 0; x < w; x++)
                    if (changed[s[x]]) d[x] = conv[s[x]];
            }
            memcpy(e->conv, conv, sizeof(e->conv));
            c->partial++;
        } else {
            c->skipped++;
        }
        e->last_use = c->clock;
        return;
    }

    tc_blit_full(dst, src, w, h, pitch, conv);
    if (!e) e = victim;
    e->dst = dst;
    e->src = src;
    e->w = w;
    e->h = h;
    e->pitch = pitch;
    e->hash = hash;
    e->last_use = c->clock;
    memcpy(e->conv, conv, sizeof(e->conv));
    tc_count(src, n, e->count);
    c->full++;
}

// dst no longer holds what the cache recorded (it was left unconverted)
static inline void tc_forget(tc_cache * c, const uint8_t * dst) {
    for (int i = 0; i < TC_ENTRIES; i++)
        if (c->e[i].dst == dst) c->e[i].dst = NULL;
}

#endif // FF7_TEXTURE_CACHE_H
