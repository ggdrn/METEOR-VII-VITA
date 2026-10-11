/*
 * Lightweight per-second counters for performance-relevant GL calls.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glstats.h"
#include "utils/logger.h"

#include <psp2/kernel/processmgr.h>

static struct {
    unsigned draws, verts;
    unsigned tex_uploads; unsigned long long tex_bytes, tex_us;
    unsigned readpixels; unsigned long long readpixels_us;
    unsigned flushes; unsigned long long flush_us;
    unsigned fbo_binds, clears;
} st;

static inline SceUInt64 now_us(void) { return sceKernelGetProcessTimeWide(); }

void glDrawArrays_stat(GLenum mode, GLint first, GLsizei count) {
    st.draws++;
    st.verts += count;
    glDrawArrays(mode, first, count);
}

static unsigned bytes_per_pixel(GLenum format, GLenum type) {
    if (type != GL_UNSIGNED_BYTE) return 2; // packed 16-bit formats
    switch (format) {
        case GL_RGBA: return 4;
        case GL_RGB:  return 3;
        case GL_LUMINANCE_ALPHA: return 2;
        default: return 1;
    }
}

void glTexImage2D_stat(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                       GLint border, GLenum format, GLenum type, const void * pixels) {
    SceUInt64 t0 = now_us();
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    st.tex_us += now_us() - t0;
    st.tex_uploads++;
    st.tex_bytes += (unsigned long long) width * height * bytes_per_pixel(format, type);
}

void glReadPixels_stat(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void * pixels) {
    SceUInt64 t0 = now_us();
    glReadPixels(x, y, width, height, format, type, pixels);
    st.readpixels_us += now_us() - t0;
    if (!st.readpixels++) l_info("glReadPixels(%i, %i, %i, %i)", x, y, width, height);
}

void glFlush_stat(void) {
    SceUInt64 t0 = now_us();
    glFlush();
    st.flush_us += now_us() - t0;
    st.flushes++;
}

void glBindFramebuffer_stat(GLenum target, GLuint framebuffer) {
    st.fbo_binds++;
    glBindFramebuffer(target, framebuffer);
}

void glClear_stat(GLbitfield mask) {
    st.clears++;
    glClear(mask);
}

void glstats_report(double seconds) {
    if (seconds <= 0) return;
    l_info("gl/s: %.0f draws, %.0f verts, %.1f clears, %.1f fbo binds | tex %.1f uploads (%.0f KB, %.1f ms) | "
           "readpixels %.1f (%.1f ms) | flush %.1f (%.1f ms)",
           st.draws / seconds, st.verts / seconds, st.clears / seconds, st.fbo_binds / seconds,
           st.tex_uploads / seconds, st.tex_bytes / 1024.0 / seconds, st.tex_us / 1000.0 / seconds,
           st.readpixels / seconds, st.readpixels_us / 1000.0 / seconds,
           st.flushes / seconds, st.flush_us / 1000.0 / seconds);
    st.draws = st.verts = st.tex_uploads = st.readpixels = st.flushes = st.fbo_binds = st.clears = 0;
    st.tex_bytes = st.tex_us = st.readpixels_us = st.flush_us = 0;
}
