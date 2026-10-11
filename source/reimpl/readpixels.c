/*
 * glReadPixels cache.
 *
 * The battle-entry "swirl" of FF7 PC captures the screen one row at a time
 * (glReadPixels(0, y, 640, 1) for every y). On the Vita each call waits for
 * the GPU to finish, which stalled every battle start for seconds. The first
 * row read after rendering now fetches the whole visible area once; the
 * following rows are copied from that snapshot until anything is drawn again.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/readpixels.h"

#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>

static struct {
    int valid;
    GLint x, y0, rows;       // cached rectangle (full rows of `width`)
    GLsizei width;
    GLenum format, type;
    GLint pack_alignment;
    size_t stride;
    uint8_t * data;
    size_t capacity;
} cache;

static unsigned misses;

void readpixels_invalidate(void) {
    cache.valid = 0;
}

static int bytes_per_pixel(GLenum format, GLenum type) {
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1)
        return 2;
    if (type != GL_UNSIGNED_BYTE) return 0;
    switch (format) {
        case GL_RGBA: return 4;
        case GL_RGB: return 3;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_ALPHA:
        case GL_LUMINANCE: return 1;
        default: return 0;
    }
}

static size_t row_stride(GLsizei width, int bpp, GLint alignment) {
    size_t row = (size_t) width * bpp;
    return (row + alignment - 1) / alignment * alignment;
}

void glReadPixels_cached(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void * pixels) {
    int bpp = bytes_per_pixel(format, type);
    GLint alignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    if (bpp == 0 || width <= 0 || height <= 0 || y < 0 || alignment <= 0) {
        glReadPixels(x, y, width, height, format, type, pixels);
        return;
    }

    int hit = cache.valid && x == cache.x && width == cache.width && format == cache.format
              && type == cache.type && alignment == cache.pack_alignment
              && y >= cache.y0 && y + height <= cache.y0 + cache.rows;

    if (!hit) {
        // Snapshot every row from 0 up to the viewport top (or the request)
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        GLint rows = vp[1] + vp[3];
        if (rows < y + height) rows = y + height;

        size_t stride = row_stride(width, bpp, alignment);
        size_t size = stride * rows;
        if (size > cache.capacity) {
            free(cache.data);
            cache.data = malloc(size);
            cache.capacity = cache.data ? size : 0;
        }
        if (!cache.data) {
            cache.valid = 0;
            glReadPixels(x, y, width, height, format, type, pixels);
            return;
        }

        glReadPixels(x, 0, width, rows, format, type, cache.data);
        cache.valid = 1;
        cache.x = x;
        cache.y0 = 0;
        cache.rows = rows;
        cache.width = width;
        cache.format = format;
        cache.type = type;
        cache.pack_alignment = alignment;
        cache.stride = stride;
        if (misses++ < 4) l_info("glReadPixels cache: snapshot %ix%i at x=%i", width, rows, x);
    }

    // Same layout glReadPixels would produce: bottom-up rows, padded stride
    uint8_t * dst = pixels;
    for (GLsizei r = 0; r < height; r++)
        memcpy(dst + r * cache.stride, cache.data + (size_t) (y - cache.y0 + r) * cache.stride, (size_t) width * bpp);
}
