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

#define PROFILE_PATH DATA_PATH "profile.bin"
#define SAMPLE_PERIOD_US 5000
#define FLUSH_EVERY_US 5000000
#define RING_SIZE 4096

#define PROT_RWX (KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_EXEC)
#define PROT_RW  (KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE)

extern so_module so_mod;

typedef struct {
    uint32_t pc, lr, tid;
    uint32_t ms; // process time, matches the [   x.xxxs] stamps in logs.txt
} sample_t;

static sample_t ring[RING_SIZE];
static volatile uint32_t ring_head; // written by the handler
static uint32_t ring_tail;          // read by the sampler thread
static uintptr_t text_lo, text_hi;
static volatile int armed;
static volatile SceUInt64 * spike_frame_start;
static SceUInt64 spike_threshold_us;

// Spike mode: only sample frames that are already slow
static int should_sample(void) {
    if (!spike_frame_start) return 1;
    SceUInt64 start = *spike_frame_start;
    return start && sceKernelGetProcessTimeWide() - start > spike_threshold_us;
}

static void abort_handler(KuKernelExceptionContext * ctx) {
    uintptr_t addr = ctx->exceptionType == KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT ? ctx->FAR : ctx->pc;
    if (addr < text_lo || addr >= text_hi) {
        // Not caused by the profiler: drop the handler so the fault crashes
        // (and dumps) normally when the instruction is re-executed
        kuKernelReleaseExceptionHandler(ctx->exceptionType);
        return;
    }

    if (armed) {
        armed = 0;
        kuKernelMemProtect((void *) text_lo, text_hi - text_lo, PROT_RWX);
    }

    uint32_t i = __atomic_fetch_add(&ring_head, 1, __ATOMIC_RELAXED) % RING_SIZE;
    ring[i].pc = ctx->pc - text_lo;
    ring[i].lr = ctx->lr - text_lo;
    ring[i].tid = sceKernelGetThreadId();
    ring[i].ms = (uint32_t) (sceKernelGetProcessTimeWide() / 1000);
    // Returning resumes at ctx->pc, now executable again
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
    }
}

static int sampler_thread(SceSize args, void * argp) {
    SceUID fd = sceIoOpen(PROFILE_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        l_error("profiler: could not create %s", PROFILE_PATH);
        return 0;
    }

    SceUInt32 since_flush = 0;
    while (1) {
        sceKernelDelayThread(SAMPLE_PERIOD_US);
        if (!armed && should_sample()) {
            armed = 1;
            kuKernelMemProtect((void *) text_lo, text_hi - text_lo, PROT_RW);
        }
        since_flush += SAMPLE_PERIOD_US;
        if (since_flush >= FLUSH_EVERY_US) {
            since_flush = 0;
            flush_samples(fd);
        }
    }
    return 0;
}

void profiler_start(volatile SceUInt64 * frame_start, SceUInt64 threshold_us) {
    spike_frame_start = frame_start;
    spike_threshold_us = threshold_us;
    text_lo = so_mod.text_base;
    text_hi = so_mod.text_base + so_mod.text_size;

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

    SceUID thid = sceKernelCreateThread("profiler", sampler_thread, 0x10000100 - 0x20, 0x4000, 0, 0, NULL);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
    l_info("profiler: sampling .so text 0x%08x-0x%08x every %d us into %s%s",
           text_lo, text_hi, SAMPLE_PERIOD_US, PROFILE_PATH,
           frame_start ? " (only frames slower than the spike threshold)" : "");
}
