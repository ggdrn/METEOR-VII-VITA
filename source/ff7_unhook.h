/*
 * Permanent removal of a so_util hook.
 *
 * A native replacement that disables itself (self-test mismatch) used to
 * keep its hook and call the original through SO_CONTINUE on every call:
 * two instruction writes and two cache flushes per call, at up to ~230,000
 * calls/s for _ftol. Restoring the original instructions once sends every
 * later call straight to the original. Under profiler_hold(): the profiler
 * changes the text's permissions from another core.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_UNHOOK_H
#define FF7_UNHOOK_H

#include "utils/profiler.h"

#include <kubridge.h>
#include <psp2/kernel/clib.h>
#include <so_util/so_util.h>

static inline void ff7_unhook(so_hook * h) {
    profiler_hold();
    sceClibMemcpy((void *) h->addr, h->orig_instr, sizeof(h->orig_instr));
    kuKernelFlushCaches((void *) h->addr, sizeof(h->orig_instr));
    profiler_release();
}

/*
 * SO_CONTINUE (call the original through a hook: the entry is rewritten and
 * the caches flushed twice) for hooks that stay installed and run every
 * frame. Under profiler_hold() so it never overlaps a text permission change
 * by the profiler on another core.
 */
#define SO_CONTINUE_HELD(type, h, ...) ({ \
    profiler_hold(); \
    type so_continue_held_r = SO_CONTINUE(type, h, ##__VA_ARGS__); \
    profiler_release(); \
    so_continue_held_r; \
})

#endif // FF7_UNHOOK_H
