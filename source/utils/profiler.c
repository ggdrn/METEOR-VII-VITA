/*
 * Sampling profiler for the game's .so (development aid).
 *
 * A sampler thread periodically removes execute permission from the .so's
 * text. The next instruction fetched there raises a prefetch abort; the
 * kubridge exception handler records PC/LR, restores the permission and
 * resumes. Literal-pool loads from the text can also fault while the
 * permission is being changed (data abort with FAR inside the text): those
 * are handled the same way. Samples go to DATA_PATH "profile.bin" as {pc, lr, tid, ms} records
 * with addresses relative to the .so text base; scripts/profile-report.py
 * turns them into a per-function ranking.
 *
 * Changing the protection of the text is a kernel call on a 15 MB range, so
 * no two of them may overlap, nor overlap code that rewrites the text and
 * flushes the caches (v0.5.03 rebooted the console with profile_on.txt: the
 * sampler toggled the text while ff7_stack patched 333k sites in it):
 *  - `state` lets one change run at a time: the sampler arms only from IDLE,
 *    and only the thread that moves ARMED -> RESTORING gives execute back;
 *  - nothing is armed during the first START_DELAY_US (boot patching);
 *  - code that writes to the text brackets it with profiler_hold() and
 *    profiler_release(), which disarm and keep the sampler off meanwhile;
 *  - only whole pages inside the text are protected: the last, partial page
 *    holds the start of so_util's code cave (hook trampolines).
 *
 * Where samples land (v0.5.05): a core only notices the new protection after
 * its TLB drops the old entry. With the sampler on cores 1-2, core 0 (the
 * game thread) faulted only on TLB misses and after its own sleeps, so a hot
 * loop was never sampled and the return of the frame limiter's usleep took
 * half the samples of the world map. The sampler now runs on core 0, with a
 * higher priority than the game thread: it preempts the game, changes the
 * protection on that core, and the game faults at the very instruction it
 * was interrupted at. It does not arm while the game thread sleeps or waits
 * (game_idle), so idle time is not sampled.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/profiler.h"
#include "utils/logger.h"

#include <kubridge.h>
#include <so_util/so_util.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include "utils/cpu.h"

#define PROFILE_PATH DATA_PATH "profile.bin"
#define SAMPLE_PERIOD_US 5000
// profile_all.txt (sampling whenever the game runs): v0.5.05 rebooted the
// console after 20-40 s at one arm every 5 ms (~170 protection changes of
// the whole text per second); one every 20 ms still gives ~50 samples/s
#define ALL_PERIOD_US 20000
#define FLUSH_EVERY_US 5000000
#define RING_SIZE 4096
#define START_DELAY_US 20000000

#define PROT_RWX (KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_EXEC)
#define PROT_RW  (KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE)

extern so_module so_mod;

typedef struct {
    uint32_t pc, lr, tid;
    uint32_t ms; // process time, matches the [   x.xxxs] stamps in logs.txt
} sample_t;

enum { IDLE, ARMING, ARMED, RESTORING };

static sample_t ring[RING_SIZE];
static volatile uint32_t ring_head; // written by the handler
static uint32_t ring_tail;          // read by the sampler thread
static uintptr_t text_lo, text_hi;  // protected range: whole pages of the text
static volatile int state;
static volatile int holds;          // profiler_hold() nesting, sampler stays off
static volatile uint32_t foreign_addr, foreign_pc;
static unsigned arms, flushed;
static volatile SceUInt64 * spike_frame_start;
static SceUInt64 spike_threshold_us;
static SceUInt32 period_us = SAMPLE_PERIOD_US;

// Spike mode: only sample frames that are already slow, or while the game
// thread is CPU-bound (has not slept or waited for 50 ms: frames dropping
// below the game's own rate)
#define CPU_BOUND_US 50000
extern volatile SceUInt64 game_last_idle_us;
extern volatile int game_idle;

static int should_sample(void) {
    if (game_idle) return 0;
    if (!spike_frame_start) return 1;
    SceUInt64 now = sceKernelGetProcessTimeWide();
    SceUInt64 start = *spike_frame_start;
    if (start && now - start > spike_threshold_us) return 1;
    SceUInt64 idle = game_last_idle_us;
    return idle && now - idle > CPU_BOUND_US;
}

static int cas(volatile int * p, int from, int to) {
    return __atomic_compare_exchange_n(p, &from, to, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

// Gives execute back if armed. Returns 1 if this call did it.
static int disarm(void) {
    if (!cas(&state, ARMED, RESTORING)) return 0;
    kuKernelMemProtect((void *) text_lo, text_hi - text_lo, PROT_RWX);
    __atomic_store_n(&state, IDLE, __ATOMIC_RELEASE);
    return 1;
}

static void abort_handler(KuKernelExceptionContext * ctx) {
    uintptr_t addr = ctx->exceptionType == KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT ? ctx->FAR : ctx->pc;
    if (addr < text_lo || addr >= text_hi) {
        // Not caused by the profiler: drop the handler so the fault crashes
        // (and dumps) normally when the instruction is re-executed
        foreign_addr = addr;
        foreign_pc = ctx->pc;
        kuKernelReleaseExceptionHandler(ctx->exceptionType);
        return;
    }

    // ARMING (protection still being changed by the sampler) or RESTORING
    // (another core is giving execute back): return, the instruction faults
    // again until the text is executable
    if (!disarm()) return;

    uint32_t i = __atomic_fetch_add(&ring_head, 1, __ATOMIC_RELAXED) % RING_SIZE;
    ring[i].pc = ctx->pc - text_lo;
    ring[i].lr = ctx->lr - text_lo;
    ring[i].tid = sceKernelGetThreadId();
    ring[i].ms = (uint32_t) (sceKernelGetProcessTimeWide() / 1000);
    // Returning resumes at ctx->pc, now executable again
}

void profiler_hold(void) {
    if (!text_lo) return;
    __atomic_add_fetch(&holds, 1, __ATOMIC_ACQ_REL);
    while (1) {
        int s = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
        if (s == IDLE) return;
        if (s == ARMED && disarm()) return;
        sceKernelDelayThread(100); // ARMING or RESTORING: let it finish
    }
}

void profiler_release(void) {
    if (!text_lo) return;
    __atomic_sub_fetch(&holds, 1, __ATOMIC_ACQ_REL);
}

static void flush_samples(SceUID fd) {
    uint32_t head = ring_head;
    if (head - ring_tail > RING_SIZE) ring_tail = head - RING_SIZE; // overrun: keep the newest
    while (ring_tail != head) {
        uint32_t i = ring_tail % RING_SIZE;
        uint32_t n = (head - ring_tail);
        if (i + n > RING_SIZE) n = RING_SIZE - i;
        sceIoWrite(fd, &ring[i], n * sizeof(sample_t));
        ring_tail += n;
        flushed += n;
    }
}

static int sampler_thread(SceSize args, void * argp) {
    SceUID fd = sceIoOpen(PROFILE_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        l_error("profiler: could not create %s", PROFILE_PATH);
        return 0;
    }

    SceUInt64 start = sceKernelGetProcessTimeWide() + START_DELAY_US;
    SceUInt32 since_flush = 0;
    while (1) {
        sceKernelDelayThread(period_us);
        if (sceKernelGetProcessTimeWide() >= start && !holds && should_sample() && cas(&state, IDLE, ARMING)) {
            if (holds) { // profiler_hold() started meanwhile
                __atomic_store_n(&state, IDLE, __ATOMIC_RELEASE);
            } else {
                arms++;
                kuKernelMemProtect((void *) text_lo, text_hi - text_lo, PROT_RW);
                __atomic_store_n(&state, ARMED, __ATOMIC_RELEASE);
            }
        }
        since_flush += period_us;
        if (since_flush >= FLUSH_EVERY_US) {
            since_flush = 0;
            flush_samples(fd);
        }
    }
    return 0;
}

void profiler_report(void) {
    if (!text_lo) return;
    l_info("profiler: %u arms, %u samples taken, %u written, state %d", arms, ring_head, flushed, state);
    if (foreign_addr)
        l_warn("profiler: fault not caused by the profiler at 0x%08x (pc 0x%08x), handler released",
               (unsigned) foreign_addr, (unsigned) foreign_pc);
}

void profiler_start(volatile SceUInt64 * frame_start, SceUInt64 threshold_us) {
    spike_frame_start = frame_start;
    period_us = frame_start ? SAMPLE_PERIOD_US : ALL_PERIOD_US;
    spike_threshold_us = threshold_us;
    text_lo = so_mod.text_base;
    text_hi = (so_mod.text_base + so_mod.text_size) & ~0xfffu;

    KuKernelExceptionHandlerOpt opt = { .size = sizeof(opt) };
    int ret = kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT,
                                               abort_handler, NULL, &opt);
    if (ret >= 0)
        ret = kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT,
                                               abort_handler, NULL, &opt);
    if (ret < 0) {
        l_error("profiler: kuKernelRegisterExceptionHandler failed: 0x%08x (kubridge too old?)", ret);
        return;
    }

    SceUID thid = sceKernelCreateThread("profiler", sampler_thread, 0x10000100 - 0x20, 0x4000, 0, CPU_MASK_GAME, NULL);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
    if (thid >= 0) cpu_register_thread("profiler", thid);
    l_info("profiler: sampling .so text 0x%08x-0x%08x every %d us into %s, starting in %d s%s",
           text_lo, text_hi, frame_start ? SAMPLE_PERIOD_US : ALL_PERIOD_US, PROFILE_PATH, START_DELAY_US / 1000000,
           frame_start ? " (only frames slower than the spike threshold)" : " (whenever the game thread runs)");
}
