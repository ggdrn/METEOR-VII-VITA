/*
 * CPU and GPU load summary, logged with the heartbeat (see perf.h).
 *
 * CPU: per-core usage of the whole system from the kernel's idle clocks
 * (sceKernelGetSystemInfo), plus a breakdown of the game thread: real work,
 * GL calls, spin-waiting inside the frame limiter, sleeping and blocking.
 *
 * GPU: vitaGL's display queue callback runs when the GPU has finished a
 * frame. The time from the CPU submitting the frame (vglSwapBuffers) to that
 * callback is the GPU "tail": the rendering still pending when the CPU was
 * done. Near zero means the GPU keeps up with the game; close to the frame
 * time (or swaps blocking in the display queue) means the GPU is the
 * bottleneck.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/perf.h"

#include "utils/cpu.h"
#include "utils/glstats.h"
#include "utils/logger.h"

#include <stdio.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#define SUBMIT_RING 16

static volatile SceUInt64 submit_us[SUBMIT_RING];
static volatile uint32_t submit_head, submit_tail;

// Written by the display queue thread
static volatile SceUInt64 gpu_tail_sum_us, gpu_tail_max_us;
static volatile uint32_t gpu_frames, gpu_late_frames;

static SceKernelSysClock last_idle[4];
static SceUInt64 last_report_us;

static void display_cb(void * framebuf) {
    (void) framebuf;
    SceUInt64 now = sceKernelGetProcessTimeWide();
    uint32_t tail = submit_tail;
    if (tail == submit_head) return; // a frame we did not see submitted
    SceUInt64 t = submit_us[tail % SUBMIT_RING];
    submit_tail = tail + 1;
    SceUInt64 lag = now > t ? now - t : 0;
    gpu_tail_sum_us += lag;
    if (lag > gpu_tail_max_us) gpu_tail_max_us = lag;
    if (lag > 8000) gpu_late_frames++;
    gpu_frames++;
}

void perf_init(void) {
    vglSetDisplayCallback(display_cb);
    SceKernelSystemInfo info;
    info.size = sizeof(info);
    if (sceKernelGetSystemInfo(&info) >= 0)
        for (int i = 0; i < 4; i++) last_idle[i] = info.cpuInfo[i].idleClock;
    last_report_us = sceKernelGetProcessTimeWide();
}

void perf_frame_submitted(void) {
    uint32_t head = submit_head;
    if (head - submit_tail >= SUBMIT_RING) submit_tail = head - SUBMIT_RING + 1; // callbacks lost
    submit_us[head % SUBMIT_RING] = sceKernelGetProcessTimeWide();
    submit_head = head + 1;
}

void perf_report(double fps, double game_cpu_pct) {
    SceUInt64 now = sceKernelGetProcessTimeWide();
    double elapsed_us = (double) (now - last_report_us);
    last_report_us = now;

    // Per-core usage
    char cores[96] = "?";
    SceKernelSystemInfo info;
    info.size = sizeof(info);
    if (elapsed_us > 0 && sceKernelGetSystemInfo(&info) >= 0) {
        int len = 0;
        for (int i = 0; i < 3; i++) {
            double idle = (double) (info.cpuInfo[i].idleClock - last_idle[i]);
            double busy = 100.0 - idle * 100.0 / elapsed_us;
            if (busy < 0) busy = 0;
            if (busy > 100) busy = 100;
            len += snprintf(cores + len, sizeof(cores) - len, "%score%d %.0f%%", i ? ", " : "", i, busy);
        }
        for (int i = 0; i < 4; i++) last_idle[i] = info.cpuInfo[i].idleClock;
    }

    // Game thread breakdown
    double sleep_pct, spin_pct, wait_pct, gl_pct;
    glstats_game_breakdown(&sleep_pct, &spin_pct, &wait_pct, &gl_pct);
    double work_pct = game_cpu_pct - spin_pct - gl_pct; // emulated game code + our native helpers
    if (work_pct < 0) work_pct = 0;

    // GPU tail
    uint32_t frames = gpu_frames;
    double tail_avg = frames ? gpu_tail_sum_us / 1000.0 / frames : 0;
    double tail_max = gpu_tail_max_us / 1000.0;
    uint32_t late = gpu_late_frames;
    gpu_frames = gpu_late_frames = 0;
    gpu_tail_sum_us = gpu_tail_max_us = 0;

    double frame_ms = fps > 0 ? 1000.0 / fps : 0;
    const char * verdict;
    if (fps <= 0) verdict = "-";
    else if (frames && tail_avg > frame_ms * 0.6) verdict = "GPU-bound";
    else if (sleep_pct + wait_pct < 5) verdict = "CPU-bound (game thread never idle)";
    else verdict = "neither: game at its own frame rate";

    l_info("perf: %.1f fps | CPU %s | game thread: work %.0f%%, GL calls %.0f%%, spin-wait %.0f%%, sleep %.0f%%, "
           "blocked %.0f%% | GPU tail avg %.1f ms, max %.1f ms, %u/%u frames > 8 ms | %s",
           fps, cores, work_pct, gl_pct, spin_pct, sleep_pct - spin_pct, wait_pct,
           tail_avg, tail_max, late, frames, verdict);
}
