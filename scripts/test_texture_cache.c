/*
 * Host test of source/ff7_texture_cache.h: sequences of conversions as an
 * animated-palette field makes them (same page, same buffer, a few colors
 * changing), plus index changes, a buffer written by someone else, inverted
 * pages (negative pitch), odd widths and more buffers than cache entries.
 * After every call the buffer must equal a plain full conversion.
 *
 *   cc -O2 -I source scripts/test_texture_cache.c -o /tmp/tc && /tmp/tc
 */
#include "ff7_texture_cache.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t rng = 12345;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

#define NBUF 40

typedef struct {
    int w, h, pitch, invert;
    uint8_t * mem, * src;
    uint32_t conv[256];
} page;

static void reference(const page * p, uint8_t * out) {
    uint8_t * dst = p->invert ? out + (size_t) p->pitch * (p->h - 1) : out;
    int pitch = p->invert ? -p->pitch : p->pitch;
    for (int y = 0; y < p->h; y++)
        for (int x = 0; x < p->w; x++) {
            uint32_t v = p->conv[p->src[y * p->w + x]];
            memcpy(dst + y * pitch + 4 * x, &v, 4);
        }
}

int main(void) {
    static tc_cache cache;
    page pages[NBUF];
    for (int i = 0; i < NBUF; i++) {
        page * p = &pages[i];
        static const int sizes[][2] = { { 256, 256 }, { 128, 64 }, { 37, 21 }, { 256, 128 }, { 1, 1 } };
        p->w = sizes[i % 5][0];
        p->h = sizes[i % 5][1];
        p->pitch = 4 * p->w + (i % 3 == 2 ? 16 : 0);
        p->invert = i % 4 == 3;
        p->mem = calloc((size_t) p->pitch * p->h, 1);
        p->src = malloc((size_t) p->w * p->h);
        // a few indices dominate, as in real pages
        for (int k = 0; k < p->w * p->h; k++) p->src[k] = rnd() % 4 ? rnd() % 24 : rnd() % 256;
        for (int k = 0; k < 256; k++) p->conv[k] = rnd();
    }
    uint8_t * ref = malloc(256 * 300 * 4);
    int bad = 0, calls = 0;
    for (int it = 0; it < 20000; it++) {
        // mostly the first 8 pages (animated), sometimes any of the 40 (evictions)
        page * p = &pages[rnd() % 8 ? rnd() % 8 : rnd() % NBUF];
        int r = rnd() % 100;
        if (r < 50) { // palette animation: a few colors
            for (int k = rnd() % 4; k > 0; k--) p->conv[rnd() % 24] = rnd();
        } else if (r < 60) { // a color no pixel may use
            p->conv[200 + rnd() % 56] = rnd();
        } else if (r < 65) { // most colors
            for (int k = 0; k < 256; k++) p->conv[k] = rnd();
        } else if (r < 70) { // the indices change
            p->src[rnd() % (p->w * p->h)] ^= 1 + rnd() % 255;
        } else if (r < 73) { // someone else rewrites the buffer
            memset(p->mem, rnd() & 0xff, (size_t) p->pitch * p->h);
        } // else: nothing changed
        uint8_t * dst = p->invert ? p->mem + (size_t) p->pitch * (p->h - 1) : p->mem;
        tc_convert(&cache, dst, p->src, p->w, p->h, p->invert ? -p->pitch : p->pitch, p->conv);
        calls++;
        reference(p, ref);
        for (int y = 0; y < p->h; y++)
            if (memcmp(p->mem + (size_t) y * p->pitch, ref + (size_t) y * p->pitch, (size_t) 4 * p->w)) {
                if (bad++ < 5) printf("MISMATCH call %d page %dx%d row %d\n", it, p->w, p->h, y);
                // resync, so one miss is counted once
                memcpy(p->mem, ref, (size_t) p->pitch * p->h);
                break;
            }
    }
    printf("texture cache: %d mismatches in %d calls (%u full, %u partial, %u skipped)\n", bad, calls,
           cache.full, cache.partial, cache.skipped);
    return bad != 0;
}
