/*
 * Lightweight per-second counters for performance-relevant GL calls.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glstats.h"
#include "utils/logger.h"

#include "reimpl/pthr.h"
#include "reimpl/readpixels.h"
#include "reimpl/gl_variants.h"

#include <unistd.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

// Thread that issues the GL calls (the game's own render/logic thread)
static SceUID gl_thread = -1;
static SceUID main_thread = -1;
static SceKernelSysClock last_gl_clocks, last_main_clocks;

static struct {
    unsigned draws, verts; unsigned long long draw_us;
    unsigned tex_uploads; unsigned long long tex_bytes, tex_us;
    unsigned readpixels; unsigned long long readpixels_us;
    unsigned flushes; unsigned long long flush_us;
    unsigned fbo_binds, clears;
    unsigned sleeps; unsigned long long sleep_req_us, sleep_us;
    unsigned locks; unsigned long long lock_us;
    unsigned waits; unsigned long long wait_us;
} st;

static inline SceUInt64 now_us(void) { return sceKernelGetProcessTimeWide(); }

void glDrawArrays_stat(GLenum mode, GLint first, GLsizei count) {
    if (gl_thread < 0) gl_thread = sceKernelGetThreadId();
    st.draws++;
    st.verts += count;
    readpixels_invalidate();
    SceUInt64 t0 = now_us();
    gl_variants_before_draw();
    glDrawArrays(mode, first, count);
    st.draw_us += now_us() - t0;
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
    glReadPixels_cached(x, y, width, height, format, type, pixels);
    st.readpixels_us += now_us() - t0;
    if (!st.readpixels++) l_info("glReadPixels(%i, %i, %i, %i)", x, y, width, height);
}

// The game calls glFlush after almost every batch (~28 times per frame). In
// vitaGL each one ends the GXM scene: on the Vita's tile-based GPU that means
// storing the whole render target to memory and loading it back for the next
// scene, which kept the GPU near 100%. Nothing needs it here: vitaGL syncs on
// its own for glReadPixels, framebuffer switches and textures still in use.
void glFlush_stat(void) {
    st.flushes++;
}

void glBindFramebuffer_stat(GLenum target, GLuint framebuffer) {
    st.fbo_binds++;
    readpixels_invalidate();
    glBindFramebuffer(target, framebuffer);
}

void glClear_stat(GLbitfield mask) {
    st.clears++;
    readpixels_invalidate();
    glClear(mask);
}

static inline int on_gl_thread(void) {
    return sceKernelGetThreadId() == gl_thread;
}

// The game's frame limiter (func_0x0041b9d8_ex) sleeps the remaining frame
// time with usleep. sceKernelDelayThread overshoots (~50% on short sleeps),
// which made 15 fps battle frames miss their 4th vblank and drop to 12-14 fps.
// On the game thread, sleep until ~1 ms before the deadline and spin the rest.
#define PRECISE_SLEEP_MARGIN_US 1000

static int precise_usleep(unsigned int usec) {
    SceUInt64 deadline = now_us() + usec;
    if (usec > PRECISE_SLEEP_MARGIN_US)
        sceKernelDelayThread(usec - PRECISE_SLEEP_MARGIN_US);
    while (now_us() < deadline)
        ;
    return 0;
}

int usleep_stat(unsigned int usec) {
    if (!on_gl_thread()) return usleep(usec);
    SceUInt64 t0 = now_us();
    int r = precise_usleep(usec);
    st.sleep_us += now_us() - t0;
    st.sleep_req_us += usec;
    st.sleeps++;
    return r;
}

int pthread_mutex_lock_stat(void * mutex) {
    if (!on_gl_thread()) return pthread_mutex_lock_soloader(mutex);
    SceUInt64 t0 = now_us();
    int r = pthread_mutex_lock_soloader(mutex);
    st.lock_us += now_us() - t0;
    st.locks++;
    return r;
}

int pthread_cond_wait_stat(void * cond, void * mutex) {
    if (!on_gl_thread()) return pthread_cond_wait_soloader(cond, mutex);
    SceUInt64 t0 = now_us();
    int r = pthread_cond_wait_soloader(cond, mutex);
    SceUInt64 waited = now_us() - t0;
    st.wait_us += waited;
    if (waited > 100000) l_warn("game thread waited %llu ms (pthread_cond_wait)", waited / 1000);
    st.waits++;
    return r;
}

int sem_wait_stat(int * sem) {
    if (!on_gl_thread()) return sem_wait_soloader(sem);
    SceUInt64 t0 = now_us();
    int r = sem_wait_soloader(sem);
    st.wait_us += now_us() - t0;
    st.waits++;
    return r;
}

// CPU time used by `thid` since the previous call, in percent of `seconds`
static double thread_cpu(SceUID thid, SceKernelSysClock * last, double seconds, SceKernelThreadInfo * info) {
    info->size = sizeof(*info);
    if (thid < 0 || sceKernelGetThreadInfo(thid, info) < 0) return -1;
    double pct = (info->runClocks - *last) / 10000.0 / seconds;
    *last = info->runClocks;
    return pct;
}

void glstats_report(double seconds) {
    if (seconds <= 0) return;
    if (main_thread < 0) main_thread = sceKernelGetThreadId(); // called from the main loop
    SceKernelThreadInfo gi, mi;
    double gcpu = thread_cpu(gl_thread, &last_gl_clocks, seconds, &gi);
    double mcpu = thread_cpu(main_thread, &last_main_clocks, seconds, &mi);
    l_info("cpu: game thread %.0f%% (core %d, prio %d, mask 0x%x), main thread %.0f%% (core %d) | "
           "game thread blocked/s: sleep %u calls %.0f ms (asked %.0f ms), mutex %u %.0f ms, wait %u %.0f ms",
           gcpu, gi.currentCpuId, gi.currentPriority, gi.currentCpuAffinityMask, mcpu, mi.currentCpuId,
           (unsigned) (st.sleeps / seconds), st.sleep_us / 1000.0 / seconds, st.sleep_req_us / 1000.0 / seconds,
           (unsigned) (st.locks / seconds), st.lock_us / 1000.0 / seconds,
           (unsigned) (st.waits / seconds), st.wait_us / 1000.0 / seconds);
    st.sleeps = st.locks = st.waits = 0;
    st.sleep_req_us = st.sleep_us = st.lock_us = st.wait_us = 0;
    l_info("gl/s: %.0f draws (%.1f ms), %.0f verts, %.1f clears, %.1f fbo binds | tex %.1f uploads (%.0f KB, %.1f ms) | "
           "readpixels %.1f (%.1f ms) | flush %.1f (%.1f ms)",
           st.draws / seconds, st.draw_us / 1000.0 / seconds, st.verts / seconds, st.clears / seconds, st.fbo_binds / seconds,
           st.tex_uploads / seconds, st.tex_bytes / 1024.0 / seconds, st.tex_us / 1000.0 / seconds,
           st.readpixels / seconds, st.readpixels_us / 1000.0 / seconds,
           st.flushes / seconds, st.flush_us / 1000.0 / seconds);
    st.draw_us = 0;
    st.draws = st.verts = st.tex_uploads = st.readpixels = st.flushes = st.fbo_binds = st.clears = 0;
    st.tex_bytes = st.tex_us = st.readpixels_us = st.flush_us = 0;
}
