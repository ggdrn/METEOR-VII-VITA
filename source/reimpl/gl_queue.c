/*
 * Render thread: the game thread's GL calls run on another core.
 *
 * The game thread (core 0) is the bottleneck in busy scenes, and 5-9% of it
 * went to GL calls, mostly vitaGL's work for glDrawArrays (state checks,
 * copying the vertex arrays, building the GXM draw). Here the GL functions
 * the .so imports are entry points that, on the game thread, append the call
 * to a queue; a render thread on cores 1-2 runs the queue in order through
 * the same wrappers as before (shader variants, OES mapping, statistics), so
 * vitaGL only ever runs on one thread at a time.
 *
 *  - glDrawArrays copies the vertex data it will read (the game reuses its
 *    vertex buffer right after the call) into the queue; the render thread
 *    points the attribute arrays at that copy. The game only uses client-side
 *    arrays and glDrawArrays, never buffer objects or glDrawElements.
 *  - glTexImage2D, glUniformMatrix4fv and glDeleteTextures copy their data.
 *  - glGenTextures on the game thread takes names from a pool the render
 *    thread fills ahead of time. Some fields create 2-3 textures every
 *    frame, and each glGenTextures used to wait ~1 ms for the queue to drain.
 *  - Calls that return something (glGen*, shader/program queries,
 *    glReadPixels, glGetIntegerv...) wait for the queue to drain and run on
 *    the calling thread. They are rare outside startup and texture loads.
 *  - glGetError, called after nearly every state change, answers
 *    GL_NO_ERROR without waiting (errors of queued calls cannot be reported
 *    at that point anyway; the game only logs them).
 *  - Any other thread (the main thread composites the frame in
 *    onDrawFrame, then swaps) waits for the calls queued before it and holds
 *    the execution lock around its GL calls.
 *
 *  - The render thread sleeps when the queue is empty. Waking it costs the
 *    game thread a kernel call, and in busy scenes that happened for almost
 *    every draw (~4,000/s for ~5,000 draws/s). Now the game thread wakes it
 *    every WAKE_BATCH draws, when it is about to sleep (end of its frame,
 *    gl_queue_kick from usleep), and whenever something waits for the queue
 *    (full queue, gl_queue_finish, direct GL calls), so no call is left
 *    waiting past the frame.
 *
 * Creating DATA_PATH "glthread_off.txt" turns the render thread off;
 * DATA_PATH "glbatch_off.txt" wakes it on every draw, as before v0.2.05.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/gl_queue.h"

#include "reimpl/gl_variants.h"
#include "ff7_interp.h"
#include "utils/cpu.h"
#include "utils/glstats.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <malloc.h>
#include <stdint.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

void glBindTexture_oes(GLenum target, GLuint texture);
void glTexParameteri_oes(GLenum target, GLenum pname, GLint param);

#define OFF_SWITCH  DATA_PATH "glthread_off.txt"
#define BATCH_OFF_SWITCH DATA_PATH "glbatch_off.txt"
#define WAKE_BATCH  8           // draws queued per wake-up of the render thread
#define QUEUE_SIZE  (8u << 20)  // divides 2^32: head/tail wrap consistently
#define MAX_PAYLOAD (2u << 20)  // larger data: run the call directly instead
#define MAX_ARGS    9
#define MAX_ATTRIBS 16
#define SPIN_BEFORE_SLEEP 2000  // ~10 us on cores 1-2 (mostly idle)
#define CACHE_PROGS 16          // programs / uniform locations tracked for redundant calls
#define CACHE_LOCS  16
#define NAME_POOL   64          // texture names generated ahead (power of two)
#define NAME_REFILL 32          // refill below this many

enum { OP_WRAP = 1, OP_CALL, OP_DRAW };

typedef struct {
    uint16_t op, n;  // n: arguments (OP_CALL) or attribute arrays (OP_DRAW)
    uint32_t size;   // whole record, multiple of 8
} hdr_t;

// softfp: float arguments travel in core registers, so every GL entry point
// can be called through integer arguments
typedef void (* fn_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

typedef struct {
    hdr_t h;
    fn_t fn;
    uint32_t a[];    // n arguments, then the payload (8-aligned)
} call_t;

typedef struct {
    uint8_t index, size, normalized, pad;
    GLenum type;
    GLsizei stride;
    uint32_t off;    // from the record start
} draw_attr_t;

typedef struct {
    hdr_t h;
    GLenum mode;
    GLsizei count;
    draw_attr_t attr[]; // n of them, then the vertex data
} draw_t;

static uint8_t * ring;
static volatile uint32_t head, tail; // bytes published / executed
static int active;
static uint32_t game_tls;
static SceUID wake_sema = -1;
static volatile int sleeping;
static SceKernelLwMutexWork exec_lock;
static volatile int lock_wanted; // threads waiting for exec_lock (not the render thread)
static unsigned wake_batch = WAKE_BATCH;
static unsigned pending_draws;   // queued since the last wake-up (game thread only)

// GL state the queue needs, as last set by any thread
static struct {
    int enabled;
    GLint size;
    GLenum type;
    GLboolean normalized;
    GLsizei stride;
    const uint8_t * ptr;
} attribs[MAX_ATTRIBS];
static GLint unpack_alignment = 4;

static struct {
    unsigned calls, draws, syncs, wakes, skipped, bare_draws, pooled_names;
    unsigned long long bytes, game_us, sync_us, stall_us, busy_us, other_us;
    uint32_t peak;
} st;

/*
 * Battle at 30 fps (ff7_interp.c). The GL state as the game thread set it,
 * raw values (gq_shadow), to set it again before a replay; and the
 * recording of every record the game thread publishes during a frame. The
 * state of units, caps, programs or uniform locations beyond what the
 * shadow holds is only a problem when a recorded frame changes it (the
 * frame is then not replayed): what a frame does not touch is the same
 * before and after it.
 */
#define SH_CAPS  16
#define SH_UNITS 8
#define SH_UNIS  128 // uniform values set (program, location), any ids
#define SH_MATS  32  // uniform matrices set (program, location)
enum {
    SH_FB = 1, SH_ACTIVE = 2, SH_BLEND = 4, SH_DEPTH_FUNC = 8, SH_DEPTH_MASK = 0x10, SH_COLOR_MASK = 0x20,
    SH_CULL = 0x40, SH_FRONT = 0x80, SH_VIEWPORT = 0x100, SH_CLEAR_COLOR = 0x200, SH_CLEAR_DEPTH = 0x400,
    SH_DEPTH_RANGE = 0x800, SH_LINE_WIDTH = 0x1000, SH_PROGRAM = 0x2000
};

typedef struct {
    uint32_t set;
    uint32_t fb, active, blend_s, blend_d, depth_func, depth_mask, color_mask[4], cull, front, viewport[4];
    uint32_t clear_color[4], clear_depth, depth_range[2], line_width, program;
    uint32_t tex[SH_UNITS], tex_target[SH_UNITS], tex_set;
    uint32_t ncaps, cap[SH_CAPS], cap_on[SH_CAPS];
    uint32_t nunis, nmats;
    struct { uint32_t prog; int32_t loc, val; } uni[SH_UNIS];
    struct { uint32_t prog; int32_t loc; float m[16]; } mat[SH_MATS];
} gq_shadow;

static gq_shadow sh, rec_start[2]; // game thread only
static uint8_t * rec_buf;          // recording: [original address, size, record] entries
static uint32_t rec_cap, rec_len, rec_unsafe;
static int32_t rec_last_draw = -1;
static gq_rec_why rec_why;         // the first reason the recording cannot be replayed

static inline void rec_bad_why(uint32_t why, uint32_t kind, uint32_t a, uint32_t b) {
    if (!rec_buf) return;
    if (!rec_unsafe) {
        rec_why.kind = kind;
        rec_why.a = a;
        rec_why.b = b;
    }
    rec_unsafe |= why;
}

static inline void rec_bad(uint32_t why) {
    rec_bad_why(why, why == GQ_REC_TEXTURE ? GQ_WHY_TEXTURE : why == GQ_REC_DIRECT ? GQ_WHY_DIRECT : GQ_WHY_OTHER, 0, 0);
}

static void sh_cap(uint32_t cap, uint32_t on) {
    for (uint32_t i = 0; i < sh.ncaps; i++)
        if (sh.cap[i] == cap) {
            sh.cap_on[i] = on;
            return;
        }
    if (sh.ncaps < SH_CAPS) {
        sh.cap[sh.ncaps] = cap;
        sh.cap_on[sh.ncaps++] = on;
    } else {
        rec_bad_why(GQ_REC_SHADOW, GQ_WHY_CAPS, cap, sh.ncaps);
    }
}

static void sh_texture(uint32_t target, uint32_t texture) {
    uint32_t u = sh.set & SH_ACTIVE ? sh.active - GL_TEXTURE0 : 0;
    if (u >= SH_UNITS) {
        rec_bad_why(GQ_REC_SHADOW, GQ_WHY_UNIT, u, texture);
        return;
    }
    sh.tex[u] = texture;
    sh.tex_target[u] = target;
    sh.tex_set |= 1u << u;
}

static void sh_uniform(int32_t loc, int32_t val) {
    for (uint32_t i = 0; i < sh.nunis; i++)
        if (sh.uni[i].prog == sh.program && sh.uni[i].loc == loc) {
            sh.uni[i].val = val;
            return;
        }
    if (sh.nunis < SH_UNIS) {
        sh.uni[sh.nunis].prog = sh.program;
        sh.uni[sh.nunis].loc = loc;
        sh.uni[sh.nunis++].val = val;
    } else {
        rec_bad_why(GQ_REC_SHADOW, GQ_WHY_UNIFORMS, sh.program, (uint32_t) loc);
    }
}

static void sh_matrix(int32_t loc, GLsizei count, GLboolean transpose, const GLfloat * value) {
    if (count != 1 || transpose) {
        rec_bad_why(GQ_REC_SHADOW, GQ_WHY_MATRIX, (uint32_t) count, (uint32_t) loc);
        return;
    }
    uint32_t i = 0;
    while (i < sh.nmats && !(sh.mat[i].prog == sh.program && sh.mat[i].loc == loc)) i++;
    if (i == sh.nmats) {
        if (sh.nmats == SH_MATS) {
            rec_bad_why(GQ_REC_SHADOW, GQ_WHY_MATRICES, sh.program, (uint32_t) loc);
            return;
        }
        sh.nmats++;
        sh.mat[i].prog = sh.program;
        sh.mat[i].loc = loc;
    }
    memcpy(sh.mat[i].m, value, 16 * sizeof(GLfloat));
}

static inline void sh_val(uint32_t bit, uint32_t * field, uint32_t v) {
    *field = v;
    sh.set |= bit;
}

static inline SceUInt64 now_us(void) {
    return sceKernelGetProcessTimeWide();
}

// User read-only thread ID register: distinct for every thread, one instruction
static inline uint32_t tls_reg(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v;
}

static inline int on_game(void) {
    return active && tls_reg() == game_tls;
}

static inline uint32_t align8(uint32_t v) {
    return (v + 7) & ~7u;
}

/*
 * Render thread
 */
static void exec_call(const call_t * c) {
    uint32_t a[MAX_ARGS] = {0};
    memcpy(a, c->a, c->h.n * sizeof(uint32_t));
    c->fn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
}

static void exec_draw(const draw_t * d) {
    for (int i = 0; i < d->h.n; i++) {
        const draw_attr_t * at = &d->attr[i];
        glVertexAttribPointer(at->index, at->size, at->type, at->normalized, at->stride,
                              (const uint8_t *) d + at->off);
    }
    glDrawArrays_stat(d->mode, 0, d->count);
}

static int render_thread(SceSize args, void * argp) {
    while (1) {
        if (tail == head) {
            // Short spin first: the next draw is often just behind
            for (int i = 0; i < SPIN_BEFORE_SLEEP && tail == head; i++)
                __asm__ volatile("yield" ::: "memory");
            if (tail != head) continue;
            sleeping = 1;
            __sync_synchronize();
            if (tail == head) sceKernelWaitSema(wake_sema, 1, NULL);
            sleeping = 0;
            continue;
        }
        // Bounded batches: another thread waiting for the lock gets it
        // between them even while the game thread keeps queuing
        SceUInt64 t0 = now_us();
        sceKernelLockLwMutex(&exec_lock, 1, NULL);
        uint32_t end = head;
        while (tail != end) {
            __sync_synchronize(); // the record was written before head moved
            const hdr_t * h = (const hdr_t *) (ring + tail % QUEUE_SIZE);
            if (h->op == OP_CALL) exec_call((const call_t *) h);
            else if (h->op == OP_DRAW) exec_draw((const draw_t *) h);
            uint32_t size = h->size;
            __sync_synchronize(); // done with the record before it can be reused
            tail += size;
        }
        sceKernelUnlockLwMutex(&exec_lock, 1);
        st.busy_us += now_us() - t0;
        while (lock_wanted) // let them in before the next batch
            ;
    }
    return 0;
}

/*
 * Queue (written by the game thread only)
 */
// Wakes the render thread if it is asleep (any thread)
static inline void wake(void) {
    __sync_synchronize(); // head (or the waiter's state) before reading `sleeping`
    if (sleeping && __sync_bool_compare_and_swap(&sleeping, 1, 0)) {
        st.wakes++;
        sceKernelSignalSema(wake_sema, 1);
    }
}

static void * reserve(uint32_t size) {
    uint32_t off = head % QUEUE_SIZE;
    uint32_t waste = off + size > QUEUE_SIZE ? QUEUE_SIZE - off : 0;
    if (head + waste + size - tail > QUEUE_SIZE) {
        wake();
        SceUInt64 t0 = now_us();
        while (head + waste + size - tail > QUEUE_SIZE)
            ;
        st.stall_us += now_us() - t0;
    }
    if (waste) {
        hdr_t * w = (hdr_t *) (ring + off);
        w->op = OP_WRAP;
        w->n = 0;
        w->size = waste;
        __sync_synchronize();
        head += waste;
    }
    return ring + head % QUEUE_SIZE;
}

// Only draws wake the render thread: state changes wait for the next draw
// (or for whoever waits on the queue), which saves a kernel call per GL call
static void rec_capture(const hdr_t * h, uint32_t size) {
    if (rec_len + 8 + size > rec_cap) {
        rec_unsafe |= GQ_REC_OVERFLOW;
        return;
    }
    uint32_t * e = (uint32_t *) (rec_buf + rec_len);
    e[0] = (uint32_t) h;
    e[1] = size;
    memcpy(e + 2, h, size);
    if (h->op == OP_DRAW) rec_last_draw = (int32_t) rec_len;
    rec_len += 8 + size;
}

static void publish(uint32_t size, int wake_up) {
    if (rec_buf) rec_capture((const hdr_t *) (ring + head % QUEUE_SIZE), size);
    __sync_synchronize();
    head += size;
    uint32_t used = head - tail;
    if (used > st.peak) st.peak = used;
    if (wake_up) wake();
}

// Queues fn(args...); if `data` is set, `len` bytes are copied after the
// arguments and argument `data_arg` points at the copy
static void queue_call(void * fn, int n, const uint32_t * args, const void * data, uint32_t len, int data_arg) {
    uint32_t args_end = align8(sizeof(call_t) + n * sizeof(uint32_t));
    uint32_t size = args_end + align8(data ? len : 0);
    call_t * c = reserve(size);
    c->h.op = OP_CALL;
    c->h.n = n;
    c->h.size = size;
    c->fn = (fn_t) fn;
    memcpy(c->a, args, n * sizeof(uint32_t));
    if (data) {
        memcpy((uint8_t *) c + args_end, data, len);
        c->a[data_arg] = (uint32_t) ((uint8_t *) c + args_end);
        st.bytes += len;
    }
    st.calls++;
    publish(size, 0);
}

#define QUEUE(fn, ...) do {                                         \
        const uint32_t a_[] = { __VA_ARGS__ };                       \
        queue_call((void *) (fn), sizeof(a_) / sizeof(a_[0]), a_, NULL, 0, 0); \
    } while (0)

static inline uint32_t fbits(GLfloat f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

// Waits for the calls queued before this point. Another thread does not wait
// for the queue to be empty: the game thread may keep queuing meanwhile (it
// used to call GL concurrently with the main thread's composition and swap)
void gl_queue_finish(void) {
    if (!active) return;
    uint32_t until = head;
    if ((int32_t) (tail - until) >= 0) return;
    wake();
    SceUInt64 t0 = now_us();
    while ((int32_t) (tail - until) < 0)
        ;
    if (tls_reg() == game_tls) {
        st.syncs++;
        st.sync_us += now_us() - t0;
    } else {
        st.other_us += now_us() - t0;
    }
}

void gl_queue_lock(void) {
    if (!active) return;
    gl_queue_finish();
    __sync_fetch_and_add(&lock_wanted, 1);
    sceKernelLockLwMutex(&exec_lock, 1, NULL);
    __sync_fetch_and_sub(&lock_wanted, 1);
}

void gl_queue_unlock(void) {
    if (active) sceKernelUnlockLwMutex(&exec_lock, 1);
}

// Direct GL use: after everything queued has run, with the render thread out
static SceUInt64 direct_t0;

static inline void direct_begin(void) {
    if (!active) return;
    if (tls_reg() == game_tls) direct_t0 = now_us();
    gl_queue_lock();
}

static inline void direct_end(void) {
    if (!active) return;
    gl_queue_unlock();
    if (tls_reg() == game_tls) st.game_us += now_us() - direct_t0;
}

#define DIRECT(call) do { direct_begin(); call; direct_end(); } while (0)
#define DIRECT_RET(type, call) do { direct_begin(); type r_ = call; direct_end(); return r_; } while (0)

/*
 * Setup and statistics
 */
void gl_queue_init(void) {
    if (file_exists(OFF_SWITCH)) {
        l_info("gl_queue: render thread disabled by %s", OFF_SWITCH);
        return;
    }
    ring = memalign(64, QUEUE_SIZE);
    wake_sema = sceKernelCreateSema("gl_queue_wake", 0, 0, 1, NULL);
    if (!ring || wake_sema < 0 || sceKernelCreateLwMutex(&exec_lock, "gl_queue_exec", 0, 0, NULL) < 0) {
        l_error("gl_queue: could not allocate the queue, GL stays on the game thread");
        return;
    }
    SceUID thid = sceKernelCreateThread("gl_render", render_thread, 0x10000100 - 0x04, 0x10000, 0,
                                        CPU_MASK_SECONDARY, NULL);
    if (thid < 0 || sceKernelStartThread(thid, 0, NULL) < 0) {
        l_error("gl_queue: could not start the render thread, GL stays on the game thread");
        return;
    }
    cpu_register_thread("gl_render", thid);
    if (file_exists(BATCH_OFF_SWITCH)) wake_batch = 1;
    l_info("gl_queue: render thread started (%u KB queue, woken every %u draws)", QUEUE_SIZE / 1024, wake_batch);
}

void gl_queue_set_game_thread(void) {
    if (!ring || wake_sema < 0) return;
    game_tls = tls_reg();
    __sync_synchronize();
    active = 1;
}

int gl_queue_active(void) {
    return active;
}

unsigned long long gl_queue_take_game_us(void) {
    unsigned long long us = st.game_us;
    st.game_us = 0;
    return us;
}

void gl_queue_report(double seconds) {
    if (!active || seconds <= 0) return;
    l_info("render thread: busy %.0f%% | %.0f calls/s (%.0f redundant skipped), %.0f draws/s (%u without arrays so far), %.0f wakes/s, %.0f KB/s copied, %.1f texture names/s from the pool | "
           "game thread waited: %.0f syncs/s %.1f ms/s, queue full %.1f ms/s | main thread waited %.1f ms/s | peak queue %u KB",
           st.busy_us / 10000.0 / seconds, st.calls / seconds, st.skipped / seconds, st.draws / seconds, st.bare_draws,
           st.wakes / seconds, st.bytes / 1024.0 / seconds, st.pooled_names / seconds,
           st.syncs / seconds, st.sync_us / 1000.0 / seconds, st.stall_us / 1000.0 / seconds,
           st.other_us / 1000.0 / seconds, st.peak / 1024);
    st.calls = st.draws = st.syncs = st.wakes = st.skipped = st.pooled_names = 0;
    st.bytes = st.sync_us = st.stall_us = st.busy_us = st.other_us = 0;
    st.peak = 0;
}

/*
 * Draws
 */
static unsigned type_size(GLenum type) {
    switch (type) {
        case GL_BYTE:
        case GL_UNSIGNED_BYTE: return 1;
        case GL_SHORT:
        case GL_UNSIGNED_SHORT: return 2;
        default: return 4; // GL_FLOAT, GL_FIXED
    }
}

// Points vitaGL back at the arrays the caller set (the render thread leaves
// them on queue copies), for draws that run directly
static void apply_attribs(void) {
    for (int i = 0; i < MAX_ATTRIBS; i++)
        if (attribs[i].ptr)
            glVertexAttribPointer(i, attribs[i].size, attribs[i].type, attribs[i].normalized,
                                  attribs[i].stride, attribs[i].ptr);
}

void gq_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (!on_game()) {
        DIRECT((active ? apply_attribs() : (void) 0, glDrawArrays_stat(mode, first, count)));
        return;
    }
    // Bytes each enabled array reads for vertices first .. first + count - 1
    int n = 0;
    uint8_t idx[MAX_ATTRIBS];
    const uint8_t * start[MAX_ATTRIBS];
    uint32_t len[MAX_ATTRIBS], stride[MAX_ATTRIBS], total = 0;
    const uint8_t * lo = NULL, * hi = NULL;
    for (int i = 0; i < MAX_ATTRIBS && count > 0; i++) {
        if (!attribs[i].enabled || !attribs[i].ptr) continue;
        uint32_t elem = attribs[i].size * type_size(attribs[i].type);
        uint32_t s = attribs[i].stride ? (uint32_t) attribs[i].stride : elem;
        idx[n] = i;
        stride[n] = s;
        start[n] = attribs[i].ptr + (uint32_t) first * s;
        len[n] = (uint32_t) (count - 1) * s + elem;
        if (!lo || start[n] < lo) lo = start[n];
        if (!hi || start[n] + len[n] > hi) hi = start[n] + len[n];
        total += len[n];
        n++;
    }
    // Diagnostic: a draw with vertices but no enabled array to copy would run
    // on stale pointers
    if (n == 0 && count > 0 && st.bare_draws++ < 5)
        l_warn("gl_queue: draw of %d vertices with no vertex array (enabled %d%d%d%d, ptr %p %p %p %p)", count,
               attribs[0].enabled, attribs[1].enabled, attribs[2].enabled, attribs[3].enabled,
               attribs[0].ptr, attribs[1].ptr, attribs[2].ptr, attribs[3].ptr);

    // Interleaved arrays (the usual case): copy their common span once
    int span = n > 1 && (uint32_t) (hi - lo) <= total + 64;
    uint32_t data = span ? (uint32_t) (hi - lo) : total;

    if (data > MAX_PAYLOAD) {
        rec_bad(GQ_REC_DIRECT);
        DIRECT((apply_attribs(), glDrawArrays_stat(mode, first, count)));
        return;
    }

    uint32_t head_size = align8(sizeof(draw_t) + n * sizeof(draw_attr_t));
    uint32_t size = head_size + align8(data);
    draw_t * d = reserve(size);
    d->h.op = OP_DRAW;
    d->h.n = n;
    d->h.size = size;
    d->mode = mode;
    d->count = count > 0 ? count : 0;
    uint8_t * out = (uint8_t *) d + head_size;
    if (span) memcpy(out, lo, data);
    uint32_t pos = head_size;
    for (int k = 0; k < n; k++) {
        draw_attr_t * at = &d->attr[k];
        int i = idx[k];
        at->index = i;
        at->size = attribs[i].size;
        at->normalized = attribs[i].normalized;
        at->pad = 0;
        at->type = attribs[i].type;
        at->stride = stride[k];
        if (span) {
            at->off = head_size + (uint32_t) (start[k] - lo);
        } else {
            memcpy((uint8_t *) d + pos, start[k], len[k]);
            at->off = pos;
            pos += len[k];
        }
    }
    st.draws++;
    st.bytes += data;
    publish(size, 0);
    if (ff7_ip_recording && attribs[0].enabled) // battle 30 fps: x/y/z and u/v of the draw, its record
        ff7_interp_draw(mode, first, count, attribs[0].ptr, attribs[0].size, attribs[0].type, attribs[0].stride,
                        attribs[3].enabled ? attribs[3].ptr : NULL, attribs[3].size, attribs[3].type, attribs[3].stride,
                        rec_buf ? rec_last_draw : -1);
    if (++pending_draws >= wake_batch) {
        pending_draws = 0;
        wake();
    }
}

void gl_queue_kick(void) {
    if (!on_game() || tail == head) return;
    pending_draws = 0;
    wake();
}

void gq_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                              const void * pointer) {
    if (index < MAX_ATTRIBS) {
        attribs[index].size = size;
        attribs[index].type = type;
        attribs[index].normalized = normalized;
        attribs[index].stride = stride;
        attribs[index].ptr = pointer;
    }
    if (!on_game() || index >= MAX_ATTRIBS) DIRECT(glVertexAttribPointer(index, size, type, normalized, stride, pointer));
    // Game thread: applied by each queued draw
}

void gq_glEnableVertexAttribArray(GLuint index) {
    // The game never disables an array (glDisableVertexAttribArray is not imported)
    if (on_game() && index < MAX_ATTRIBS && attribs[index].enabled) {
        st.skipped++;
        return;
    }
    if (index < MAX_ATTRIBS) attribs[index].enabled = 1;
    if (on_game()) QUEUE(glEnableVertexAttribArray, index);
    else DIRECT(glEnableVertexAttribArray(index));
}

/*
 * Calls with data
 */
static unsigned pixel_size(GLenum format, GLenum type) {
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1)
        return 2;
    if (type != GL_UNSIGNED_BYTE) return 0;
    switch (format) {
        case GL_RGBA: return 4;
        case GL_RGB: return 3;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_LUMINANCE:
        case GL_ALPHA: return 1;
        default: return 0;
    }
}

// ff7_texture.c: a page it left unconverted for this upload goes as indices + palette
int ff7_texture_upload(GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLenum format,
                       GLenum type, const void * pixels);

void gq_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h,
                     GLint border, GLenum format, GLenum type, const void * pixels) {
    if (on_game()) rec_bad(GQ_REC_TEXTURE);
    if (ff7_texture_upload(target, level, internalformat, w, h, format, type, pixels)) return;
    if (on_game()) {
        unsigned bpp = pixel_size(format, type);
        uint32_t bytes = 0;
        if (bpp && w > 0 && h > 0) {
            uint32_t row = (uint32_t) w * bpp;
            uint32_t pitch = (row + unpack_alignment - 1) / unpack_alignment * unpack_alignment;
            bytes = pitch * (uint32_t) (h - 1) + row;
        }
        if (!pixels || (bytes && bytes <= MAX_PAYLOAD)) {
            const uint32_t a[] = { target, level, internalformat, w, h, border, format, type, 0 };
            queue_call((void *) glTexImage2D_stat, 9, a, pixels, bytes, 8);
            return;
        }
    }
    DIRECT(glTexImage2D_stat(target, level, internalformat, w, h, border, format, type, pixels));
}

void gq_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat * value) {
    if (on_game() && count > 0 && value) {
        ff7_ip_note_words(IP_MATRIX, value, 16 * sizeof(GLfloat));
        sh_matrix(location, count, transpose, value);
        const uint32_t a[] = { location, count, transpose, 0 };
        queue_call((void *) glUniformMatrix4fv_variants, 4, a, value, (uint32_t) count * 16 * sizeof(GLfloat), 3);
        return;
    }
    DIRECT(glUniformMatrix4fv_variants(location, count, transpose, value));
}

/*
 * Paletted textures (ff7_texture.c): an 8-bit page goes to the GPU as its
 * indices plus the 256 colors a conversion would have produced, a GXM P8
 * texture (the GPU looks the palette up when it samples, filtering included).
 * vitaGL's own GL_COLOR_INDEX8_EXT upload stores pixel by pixel through
 * callbacks, so the texture is set up directly (vglSetupTexture: vitaGL owns
 * the index data and frees it with the texture). vitaGL does not own the
 * palette: kept per texture name here and freed once the texture is deleted
 * (or specified again) and the GPU is past the frames that drew with it.
 * These run on the thread that runs GL (the render thread when queued).
 */
void * gpu_alloc_palette(const void * data, uint32_t w, uint32_t bpe); // vitaGL internals
void gpu_free_palette(void * pal);
void * gpu_alloc_mapped_aligned_for_gpu(size_t alignment, size_t size);

#define PAL_TEXTURES 16384 // vitaGL's TEXTURES_NUM
#define PAL_DEFER    1024
#define PAL_FRAMES   8     // frames a released palette is kept (vitaGL purges its own after 4)

static void * pal_of[PAL_TEXTURES];
static struct {
    void * pal;
    uint32_t frame;
} pal_defer[PAL_DEFER];
static unsigned pal_defer_head, pal_defer_tail; // ring: tail..head pending
static unsigned pal_live, pal_uploads, pal_failed;

static void pal_purge(void) {
    while (pal_defer_tail != pal_defer_head) {
        unsigned i = pal_defer_tail % PAL_DEFER;
        if (vglGetFrameNumber() - pal_defer[i].frame <= PAL_FRAMES) break;
        gpu_free_palette(pal_defer[i].pal);
        pal_defer_tail++;
    }
}

static void pal_release(GLuint id) {
    if (id >= PAL_TEXTURES || !pal_of[id]) return;
    if (pal_defer_head - pal_defer_tail == PAL_DEFER) pal_defer_tail++; // full (never seen): leak the oldest
    pal_defer[pal_defer_head % PAL_DEFER].pal = pal_of[id];
    pal_defer[pal_defer_head % PAL_DEFER].frame = vglGetFrameNumber();
    pal_defer_head++;
    pal_of[id] = NULL;
    pal_live--;
}

// data: the 256 colors (GL_RGBA bytes), then w x h indices
static void pal_teximage(GLenum target, GLsizei w, GLsizei h, const uint8_t * data) {
    pal_purge();
    GLint id = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
    uint32_t stride = ((uint32_t) w + 7) & ~7u;
    uint8_t * px = gpu_alloc_mapped_aligned_for_gpu(16, stride * (uint32_t) h);
    void * pal = px ? gpu_alloc_palette(data, 256, 4) : NULL;
    if (!pal || id <= 0 || id >= PAL_TEXTURES) {
        // no memory (or no name to keep the palette by): the colors as RGBA
        if (pal) gpu_free_palette(pal);
        if (px) vglFree(px);
        uint32_t * rgba = malloc((size_t) w * (size_t) h * 4);
        if (rgba) {
            const uint32_t * c = (const uint32_t *) data;
            for (uint32_t i = 0; i < (uint32_t) w * (uint32_t) h; i++) rgba[i] = c[data[1024 + i]];
            glTexImage2D_stat(target, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            free(rgba);
        }
        pal_failed++;
        return;
    }
    for (GLsizei y = 0; y < h; y++) memcpy(px + (uint32_t) y * stride, data + 1024 + (uint32_t) y * (uint32_t) w, (size_t) w);
    pal_release((GLuint) id);
    vglSetupTexture(GL_TEXTURE_2D, px, SCE_GXM_TEXTURE_FORMAT_P8_ABGR, SCE_GXM_TEXTURE_LINEAR, w, h, 1);
    sceGxmTextureSetPalette(vglGetGxmTexture(GL_TEXTURE_2D), pal);
    pal_of[id] = pal;
    pal_live++;
    pal_uploads++;
}

void gl_queue_palette_report(double seconds) {
    if (seconds <= 0 || (!pal_uploads && !pal_failed)) return;
    l_info("paletted textures/s: %.1f uploaded as P8 (%u live, %u palettes waiting to be freed), %.1f as RGBA"
           " (no memory)", pal_uploads / seconds, pal_live, pal_defer_head - pal_defer_tail, pal_failed / seconds);
    pal_uploads = pal_failed = 0;
}

static void delete_textures(GLsizei n, const GLuint * textures) {
    glDeleteTextures(n, textures);
    for (GLsizei i = 0; i < n; i++) pal_release(textures[i]);
    pal_purge();
}

void gl_queue_paletted_texture(GLenum target, GLsizei w, GLsizei h, const uint32_t * pal, const uint8_t * rows,
                               int stride) {
    uint32_t len = 1024 + (uint32_t) w * (uint32_t) h;
    uint8_t * d;
    call_t * c = NULL;
    if (on_game()) rec_bad(GQ_REC_TEXTURE);
    if (on_game() && len <= MAX_PAYLOAD) {
        uint32_t args_end = align8(sizeof(call_t) + 4 * sizeof(uint32_t));
        uint32_t size = args_end + align8(len);
        c = reserve(size);
        c->h.op = OP_CALL;
        c->h.n = 4;
        c->h.size = size;
        c->fn = (fn_t) pal_teximage;
        d = (uint8_t *) c + args_end;
    } else {
        d = malloc(len);
        if (!d) return;
    }
    memcpy(d, pal, 1024);
    for (GLsizei y = 0; y < h; y++) memcpy(d + 1024 + (uint32_t) y * (uint32_t) w, rows + y * stride, (size_t) w);
    if (c) {
        c->a[0] = target;
        c->a[1] = (uint32_t) w;
        c->a[2] = (uint32_t) h;
        c->a[3] = (uint32_t) d;
        st.bytes += len;
        st.calls++;
        publish(c->h.size, 0);
    } else {
        DIRECT(pal_teximage(target, w, h, d));
        free(d);
    }
}

void gq_glDeleteTextures(GLsizei n, const GLuint * textures) {
    if (on_game()) rec_bad(GQ_REC_TEXTURE);
    if (on_game() && n > 0 && textures) {
        const uint32_t a[] = { n, 0 };
        queue_call((void *) delete_textures, 2, a, textures, (uint32_t) n * sizeof(GLuint), 1);
        return;
    }
    DIRECT(delete_textures(n, textures));
}

void gq_glPixelStorei(GLenum pname, GLint param) {
    if (pname == GL_UNPACK_ALIGNMENT && (param == 1 || param == 2 || param == 4 || param == 8))
        unpack_alignment = param;
    if (on_game()) QUEUE(glPixelStorei, pname, param);
    else DIRECT(glPixelStorei(pname, param));
}

/*
 * Plain state changes: queued on the game thread
 */
#define QUEUED(name, fn, params, ...)        \
    void gq_##name params {                 \
        if (on_game()) QUEUE(fn, __VA_ARGS__); \
        else DIRECT(fn(__VA_ARGS__));       \
    }

// The same, noting the state for the battle 30 fps probe (ff7_interp.c) on the game thread
#define QUEUED_NOTE(name, fn, params, note, ...)       \
    void gq_##name params {                           \
        if (on_game()) { note; QUEUE(fn, __VA_ARGS__); } \
        else DIRECT(fn(__VA_ARGS__));                 \
    }

QUEUED_NOTE(glActiveTexture, glActiveTexture, (GLenum texture),
            (ff7_ip_note(IP_ACTIVE_TEX, texture), sh_val(SH_ACTIVE, &sh.active, texture)), texture)
QUEUED_NOTE(glBindFramebuffer, glBindFramebuffer_stat, (GLenum target, GLuint fb),
            (ff7_ip_note_fb(fb), sh_val(SH_FB, &sh.fb, fb)), target, fb)
QUEUED(glBindRenderbuffer, glBindRenderbuffer, (GLenum target, GLuint rb), target, rb)
QUEUED_NOTE(glBindTexture, glBindTexture_oes, (GLenum target, GLuint texture),
            (ff7_ip_note_texture(texture), sh_texture(target, texture)), target, texture)
QUEUED_NOTE(glBlendFunc, glBlendFunc, (GLenum s, GLenum d),
            (ff7_ip_note(IP_BLEND_S, s), ff7_ip_note(IP_BLEND_D, d), sh.blend_s = s, sh_val(SH_BLEND, &sh.blend_d, d)),
            s, d)
QUEUED(glClear, glClear_stat, (GLbitfield mask), mask)
QUEUED_NOTE(glColorMask, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a),
            (ff7_ip_note(IP_COLOR_MASK, r | g << 1 | b << 2 | a << 3), sh.color_mask[0] = r, sh.color_mask[1] = g,
             sh.color_mask[2] = b, sh_val(SH_COLOR_MASK, &sh.color_mask[3], a)), r, g, b, a)
QUEUED_NOTE(glCullFace, glCullFace, (GLenum mode), (ff7_ip_note(IP_CULL, mode), sh_val(SH_CULL, &sh.cull, mode)), mode)
QUEUED(glDeleteShader, glDeleteShader, (GLuint shader), shader)
QUEUED_NOTE(glDepthFunc, glDepthFunc, (GLenum func),
            (ff7_ip_note(IP_DEPTH_FUNC, func), sh_val(SH_DEPTH_FUNC, &sh.depth_func, func)), func)
QUEUED_NOTE(glDepthMask, glDepthMask, (GLboolean flag),
            (ff7_ip_note(IP_DEPTH_MASK, flag), sh_val(SH_DEPTH_MASK, &sh.depth_mask, flag)), flag)
QUEUED_NOTE(glDisable, glDisable, (GLenum cap), (ff7_ip_note_cap(cap, 0), sh_cap(cap, 0)), cap)
QUEUED_NOTE(glEnable, glEnable, (GLenum cap), (ff7_ip_note_cap(cap, 1), sh_cap(cap, 1)), cap)
QUEUED(glFramebufferRenderbuffer, glFramebufferRenderbuffer,
       (GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb), target, attachment, rbtarget, rb)
QUEUED(glFramebufferTexture2D, glFramebufferTexture2D,
       (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level),
       target, attachment, textarget, texture, level)
QUEUED_NOTE(glFrontFace, glFrontFace, (GLenum mode), sh_val(SH_FRONT, &sh.front, mode), mode)
QUEUED(glRenderbufferStorage, glRenderbufferStorage, (GLenum target, GLenum format, GLsizei w, GLsizei h),
       target, format, w, h)
QUEUED(glTexParameteri, glTexParameteri_oes, (GLenum target, GLenum pname, GLint param), target, pname, param)
QUEUED_NOTE(glViewport, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h),
            (ff7_ip_note_viewport(x, y, w, h), sh.viewport[0] = x, sh.viewport[1] = y, sh.viewport[2] = w,
             sh_val(SH_VIEWPORT, &sh.viewport[3], h)), x, y, w, h)

/*
 * glUseProgram / glUniform1i with the values already set are skipped on the
 * game thread (VIDEO_enableTex sets both before every draw). The cache only
 * follows the game thread: a call from another thread empties it.
 */
static int cur_prog = -1;
static int32_t uniform_val[CACHE_PROGS][CACHE_LOCS];
static uint8_t uniform_set[CACHE_PROGS][CACHE_LOCS];

static void forget_program(GLuint program) {
    if (program < CACHE_PROGS) memset(uniform_set[program], 0, sizeof(uniform_set[program]));
}

static void forget_all(void) {
    cur_prog = -1;
    memset(uniform_set, 0, sizeof(uniform_set));
}

void gq_glUseProgram(GLuint program) {
    if (!on_game()) {
        forget_all();
        DIRECT(glUseProgram_variants(program));
        return;
    }
    ff7_ip_note_program(program);
    sh_val(SH_PROGRAM, &sh.program, program);
    if ((int) program == cur_prog) {
        st.skipped++;
        return;
    }
    cur_prog = (int) program;
    QUEUE(glUseProgram_variants, program);
}

void gq_glUniform1i(GLint location, GLint v0) {
    if (!on_game()) {
        forget_all();
        DIRECT(glUniform1i_variants(location, v0));
        return;
    }
    ff7_ip_note_uniform(location, v0);
    sh_uniform(location, v0);
    if (cur_prog >= 0 && cur_prog < CACHE_PROGS && location >= 0 && location < CACHE_LOCS) {
        if (uniform_set[cur_prog][location] && uniform_val[cur_prog][location] == v0) {
            st.skipped++;
            return;
        }
        uniform_set[cur_prog][location] = 1;
        uniform_val[cur_prog][location] = v0;
    }
    QUEUE(glUniform1i_variants, location, v0);
}

void gq_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    if (on_game()) {
        sh.clear_color[0] = fbits(r);
        sh.clear_color[1] = fbits(g);
        sh.clear_color[2] = fbits(b);
        sh_val(SH_CLEAR_COLOR, &sh.clear_color[3], fbits(a));
        QUEUE(glClearColor, fbits(r), fbits(g), fbits(b), fbits(a));
        return;
    }
    DIRECT(glClearColor(r, g, b, a));
}

void gq_glClearDepthf(GLfloat depth) {
    if (on_game()) {
        sh_val(SH_CLEAR_DEPTH, &sh.clear_depth, fbits(depth));
        QUEUE(glClearDepthf, fbits(depth));
        return;
    }
    DIRECT(glClearDepthf(depth));
}

void gq_glDepthRangef(GLfloat n, GLfloat f) {
    if (on_game()) {
        sh.depth_range[0] = fbits(n);
        sh_val(SH_DEPTH_RANGE, &sh.depth_range[1], fbits(f));
        QUEUE(glDepthRangef, fbits(n), fbits(f));
        return;
    }
    DIRECT(glDepthRangef(n, f));
}

void gq_glLineWidth(GLfloat width) {
    if (on_game()) {
        sh_val(SH_LINE_WIDTH, &sh.line_width, fbits(width));
        QUEUE(glLineWidth, fbits(width));
        return;
    }
    DIRECT(glLineWidth(width));
}

GLenum gq_glGetError(void) {
    if (on_game()) return GL_NO_ERROR;
    DIRECT_RET(GLenum, glGetError());
}

/*
 * Texture names: the render thread generates them ahead into a ring that only
 * it fills (pool_head) and only the game thread takes from (pool_tail). The
 * names are reserved in vitaGL when generated, so nothing else can get them.
 */
static GLuint name_pool[NAME_POOL];
static volatile uint32_t pool_head, pool_tail;
static volatile int pool_refill_queued;

static void pool_refill(void) {
    uint32_t room = NAME_POOL - (pool_head - pool_tail);
    GLuint ids[NAME_POOL] = {0};
    if (room) glGenTextures(room, ids);
    uint32_t h = pool_head;
    for (uint32_t i = 0; i < room; i++)
        if (ids[i]) name_pool[h++ % NAME_POOL] = ids[i];
    __sync_synchronize(); // names before the count
    pool_head = h;
    pool_refill_queued = 0;
}

static void pool_check(void) {
    if (!pool_refill_queued && pool_head - pool_tail < NAME_REFILL) {
        pool_refill_queued = 1;
        static const uint32_t no_args[1];
        queue_call((void *) pool_refill, 0, no_args, NULL, 0, 0);
    }
}

void gq_glGenTextures(GLsizei n, GLuint * ids) {
    if (on_game() && n > 0 && ids) {
        uint32_t t = pool_tail;
        if (pool_head - t >= (uint32_t) n) {
            __sync_synchronize(); // count before the names
            for (int i = 0; i < n; i++) ids[i] = name_pool[t++ % NAME_POOL];
            __sync_synchronize();
            pool_tail = t;
            st.pooled_names += n;
        } else {
            DIRECT(glGenTextures(n, ids));
        }
        pool_check();
        return;
    }
    DIRECT(glGenTextures(n, ids));
}

/*
 * Calls that return something: run directly once the queue is empty
 */
void gq_glAttachShader(GLuint program, GLuint shader) {
    DIRECT(glAttachShader_variants(program, shader));
}
void gq_glBindAttribLocation(GLuint program, GLuint index, const GLchar * name) {
    DIRECT(glBindAttribLocation(program, index, name));
}
GLenum gq_glCheckFramebufferStatus(GLenum target) {
    DIRECT_RET(GLenum, glCheckFramebufferStatus(target));
}
void gq_glCompileShader(GLuint shader) {
    DIRECT(glCompileShader_soloader(shader));
}
GLuint gq_glCreateProgram(void) {
    DIRECT_RET(GLuint, glCreateProgram());
}
GLuint gq_glCreateShader(GLenum type) {
    DIRECT_RET(GLuint, glCreateShader(type));
}
void gq_glGenFramebuffers(GLsizei n, GLuint * ids) {
    DIRECT(glGenFramebuffers(n, ids));
}
void gq_glGenRenderbuffers(GLsizei n, GLuint * ids) {
    DIRECT(glGenRenderbuffers(n, ids));
}
void gq_glGetIntegerv(GLenum pname, GLint * data) {
    DIRECT(glGetIntegerv(pname, data));
}
void gq_glGetProgramInfoLog(GLuint program, GLsizei size, GLsizei * length, GLchar * log) {
    DIRECT(glGetProgramInfoLog(program, size, length, log));
}
void gq_glGetProgramiv(GLuint program, GLenum pname, GLint * params) {
    DIRECT(glGetProgramiv(program, pname, params));
}
void gq_glGetShaderInfoLog(GLuint shader, GLsizei size, GLsizei * length, GLchar * log) {
    DIRECT(glGetShaderInfoLog(shader, size, length, log));
}
void gq_glGetShaderiv(GLuint shader, GLenum pname, GLint * params) {
    DIRECT(glGetShaderiv(shader, pname, params));
}
const GLubyte * gq_glGetString(GLenum name) {
    DIRECT_RET(const GLubyte *, glGetString(name));
}
GLint gq_glGetUniformLocation(GLuint program, const GLchar * name) {
    DIRECT_RET(GLint, glGetUniformLocation_variants(program, name));
}
void gq_glLinkProgram(GLuint program) {
    forget_program(program); // uniforms are reset by linking
    DIRECT(glLinkProgram(program));
}

void gq_glDeleteProgram(GLuint program) {
    forget_program(program);
    if ((int) program == cur_prog) cur_prog = -1;
    if (on_game()) QUEUE(glDeleteProgram, program);
    else DIRECT(glDeleteProgram(program));
}
void gq_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void * pixels) {
    DIRECT(glReadPixels_stat(x, y, w, h, format, type, pixels));
}
void gq_glShaderSource(GLuint shader, GLsizei count, const GLchar ** string, const GLint * length) {
    DIRECT(glShaderSource_soloader(shader, count, string, length));
}

/*
 * Battle at 30 fps (ff7_interp.c): recording a frame's records and replaying
 * them, game thread only. A replay sets the state the frame started with
 * (rec_start[slot], with the game's framebuffer fb_from swapped for fb_to),
 * then publishes the frame's records again: state changes and draws, the
 * draws' positions changed by `lerp` on the copy; the original arguments
 * that pointed into a record (uniform matrices) are moved to the copy.
 * Records that change textures, framebuffers or objects make the frame
 * unfit for a replay (nothing is published then).
 */
void gq_rec_begin(uint8_t * buf, uint32_t cap, int slot) {
    if (!on_game()) return;
    rec_start[slot & 1] = sh;
    rec_len = 0;
    rec_unsafe = 0;
    rec_last_draw = -1;
    rec_cap = cap;
    memset(&rec_why, 0, sizeof(rec_why));
    rec_buf = buf;
}

uint32_t gq_rec_end(uint32_t * unsafe, gq_rec_why * why) {
    if (rec_buf && (rec_unsafe & GQ_REC_OVERFLOW) && !rec_why.kind) rec_why.kind = GQ_WHY_OVERFLOW;
    rec_buf = NULL;
    if (unsafe) *unsafe = rec_unsafe;
    if (why) *why = rec_why;
    return rec_len;
}

int gq_rec_attr(const uint8_t * buf, uint32_t rec, uint32_t index, const uint8_t ** ptr, uint32_t * stride,
                int * size, uint32_t * type) {
    const draw_t * d = (const draw_t *) (buf + rec + 8);
    if (d->h.op != OP_DRAW) return 0;
    for (int i = 0; i < d->h.n; i++)
        if (d->attr[i].index == index) {
            *ptr = (const uint8_t *) d + d->attr[i].off;
            *stride = (uint32_t) d->attr[i].stride;
            *size = d->attr[i].size;
            *type = d->attr[i].type;
            return 1;
        }
    return 0;
}

enum { RK_REFUSE, RK_COPY, RK_SKIP };

static int replay_kind(fn_t fn) {
    static const fn_t copy[] = {
        (fn_t) glActiveTexture, (fn_t) glBindFramebuffer_stat, (fn_t) glBindTexture_oes, (fn_t) glBlendFunc,
        (fn_t) glClear_stat, (fn_t) glColorMask, (fn_t) glCullFace, (fn_t) glDepthFunc, (fn_t) glDepthMask,
        (fn_t) glDisable, (fn_t) glEnable, (fn_t) glFrontFace, (fn_t) glViewport, (fn_t) glUseProgram_variants,
        (fn_t) glUniform1i_variants, (fn_t) glUniformMatrix4fv_variants, (fn_t) glClearColor, (fn_t) glClearDepthf,
        (fn_t) glDepthRangef, (fn_t) glLineWidth, (fn_t) glEnableVertexAttribArray,
    };
    static const fn_t skip[] = { // object state the frame left as it is now, or nothing to draw
        (fn_t) glTexParameteri_oes, (fn_t) glPixelStorei, (fn_t) pool_refill, (fn_t) glBindRenderbuffer,
    };
    for (unsigned i = 0; i < sizeof(copy) / sizeof(copy[0]); i++)
        if (fn == copy[i]) return RK_COPY;
    for (unsigned i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
        if (fn == skip[i]) return RK_SKIP;
    return RK_REFUSE;
}

static void emit_state(const gq_shadow * s, uint32_t fb_from, uint32_t fb_to) {
    if (s->set & SH_FB) QUEUE(glBindFramebuffer_stat, GL_FRAMEBUFFER, s->fb == fb_from ? fb_to : s->fb);
    for (uint32_t u = 0; u < SH_UNITS; u++)
        if (s->tex_set & (1u << u)) {
            QUEUE(glActiveTexture, GL_TEXTURE0 + u);
            QUEUE(glBindTexture_oes, s->tex_target[u], s->tex[u]);
        }
    if (s->set & SH_ACTIVE) QUEUE(glActiveTexture, s->active);
    if (s->set & SH_BLEND) QUEUE(glBlendFunc, s->blend_s, s->blend_d);
    if (s->set & SH_DEPTH_FUNC) QUEUE(glDepthFunc, s->depth_func);
    if (s->set & SH_DEPTH_MASK) QUEUE(glDepthMask, s->depth_mask);
    if (s->set & SH_COLOR_MASK) QUEUE(glColorMask, s->color_mask[0], s->color_mask[1], s->color_mask[2], s->color_mask[3]);
    if (s->set & SH_CULL) QUEUE(glCullFace, s->cull);
    if (s->set & SH_FRONT) QUEUE(glFrontFace, s->front);
    if (s->set & SH_VIEWPORT) QUEUE(glViewport, s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    if (s->set & SH_CLEAR_COLOR)
        QUEUE(glClearColor, s->clear_color[0], s->clear_color[1], s->clear_color[2], s->clear_color[3]);
    if (s->set & SH_CLEAR_DEPTH) QUEUE(glClearDepthf, s->clear_depth);
    if (s->set & SH_DEPTH_RANGE) QUEUE(glDepthRangef, s->depth_range[0], s->depth_range[1]);
    if (s->set & SH_LINE_WIDTH) QUEUE(glLineWidth, s->line_width);
    for (uint32_t i = 0; i < s->ncaps; i++) {
        if (s->cap_on[i]) QUEUE(glEnable, s->cap[i]);
        else QUEUE(glDisable, s->cap[i]);
    }
    uint32_t prog = 0xffffffffu; // uniforms and matrices go to their programs
    for (uint32_t i = 0; i < s->nunis; i++) {
        if (s->uni[i].prog != prog) QUEUE(glUseProgram_variants, prog = s->uni[i].prog);
        QUEUE(glUniform1i_variants, (uint32_t) s->uni[i].loc, (uint32_t) s->uni[i].val);
    }
    for (uint32_t i = 0; i < s->nmats; i++) {
        if (s->mat[i].prog != prog) QUEUE(glUseProgram_variants, prog = s->mat[i].prog);
        const uint32_t a[] = { (uint32_t) s->mat[i].loc, 1, 0, 0 };
        queue_call((void *) glUniformMatrix4fv_variants, 4, a, s->mat[i].m, 16 * sizeof(float), 3);
    }
    if (s->set & SH_PROGRAM) QUEUE(glUseProgram_variants, s->program);
}

int gq_replay(const uint8_t * buf, uint32_t len, int slot, uint32_t fb_from, uint32_t fb_to, gq_lerp_fn lerp,
              void * ctx, gq_rec_why * why) {
    if (!on_game()) return 0;
    for (uint32_t off = 0; off < len;) { // all or nothing
        const uint32_t * e = (const uint32_t *) (buf + off);
        const hdr_t * h = (const hdr_t *) (e + 2);
        if (h->op == OP_CALL) {
            const call_t * c = (const call_t *) h;
            int other_fb = c->fn == (fn_t) glBindFramebuffer_stat && c->a[1] != fb_from;
            if (replay_kind(c->fn) == RK_REFUSE || other_fb) {
                if (why) {
                    why->kind = other_fb ? GQ_WHY_FRAMEBUFFER : GQ_WHY_CALL;
                    why->a = other_fb ? c->a[1] : (uint32_t) c->fn;
                    why->b = c->h.n ? c->a[0] : 0;
                }
                return 0;
            }
        }
        off += 8 + e[1];
    }
    // A clean target first: it shares the game's depth buffer, which holds the new frame's depths
    const gq_shadow * s0 = &rec_start[slot & 1];
    QUEUE(glBindFramebuffer_stat, GL_FRAMEBUFFER, fb_to);
    QUEUE(glColorMask, 1, 1, 1, 1);
    QUEUE(glDepthMask, 1);
    QUEUE(glDisable, GL_SCISSOR_TEST);
    if (s0->set & SH_CLEAR_COLOR)
        QUEUE(glClearColor, s0->clear_color[0], s0->clear_color[1], s0->clear_color[2], s0->clear_color[3]);
    QUEUE(glClearDepthf, fbits(1.0f));
    QUEUE(glClear_stat, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    emit_state(s0, fb_from, fb_to); // sets again everything above the game had set
    unsigned draws = 0;
    for (uint32_t off = 0; off < len;) {
        const uint32_t * e = (const uint32_t *) (buf + off);
        uint32_t orig = e[0], size = e[1], at_off = off;
        const hdr_t * h = (const hdr_t *) (e + 2);
        off += 8 + size;
        if (h->op == OP_CALL) {
            if (replay_kind(((const call_t *) h)->fn) == RK_SKIP) continue;
            call_t * c = reserve(size);
            memcpy(c, h, size);
            for (int i = 0; i < c->h.n; i++)
                if (c->a[i] - orig < size) c->a[i] = (uint32_t) c + (c->a[i] - orig);
            if (c->fn == (fn_t) glBindFramebuffer_stat) c->a[1] = fb_to;
            st.calls++;
            publish(size, 0);
        } else if (h->op == OP_DRAW) {
            draw_t * d = reserve(size);
            memcpy(d, h, size);
            for (int i = 0; i < d->h.n; i++) {
                draw_attr_t * at = &d->attr[i];
                if (at->index == 0 && at->type == GL_FLOAT)
                    lerp(ctx, at_off, (uint8_t *) d + at->off, (uint32_t) at->stride, at->size, (uint32_t) d->count,
                         d->mode);
            }
            st.draws++;
            publish(size, 0);
            if (++draws % wake_batch == 0) wake();
        }
    }
    wake();
    return 1;
}

/*
 * The in-between frame's target: a w x h RGBA texture in a framebuffer of
 * its own that shares the game's depth buffer (depth_rb). Run directly on
 * the game thread with the render thread out, which then finds the
 * framebuffer and texture bindings the game thread left.
 */
int gq_make_target(uint32_t w, uint32_t h, uint32_t depth_rb, uint32_t * fb_out, uint32_t * tex_out) {
    gl_queue_lock();
    GLuint tex = 0, fb = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei) w, (GLsizei) h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, sh.set & SH_FB ? sh.fb : 0);
    uint32_t u = sh.set & SH_ACTIVE ? sh.active - GL_TEXTURE0 : 0;
    int known = u < SH_UNITS && (sh.tex_set & (1u << u));
    glBindTexture_oes(known ? sh.tex_target[u] : GL_TEXTURE_2D, known ? sh.tex[u] : 0);
    gl_queue_unlock();
    *fb_out = fb;
    *tex_out = tex;
    return status == GL_FRAMEBUFFER_COMPLETE ? 1 : -(int) status;
}
