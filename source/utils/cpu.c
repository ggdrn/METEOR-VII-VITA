/*
 * Per-thread CPU usage, reported with the heartbeat (see cpu.h).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/cpu.h"
#include "utils/logger.h"

#include <stdio.h>
#include <string.h>

#define MAX_THREADS 24

static struct {
    char name[20];
    SceUID thid;
    SceKernelSysClock last;
} threads[MAX_THREADS];
static int num_threads;
static SceKernelLwMutexWork lock;
static int lock_ready;

void cpu_register_thread(const char * name, SceUID thid) {
    if (!lock_ready) {
        sceKernelCreateLwMutex(&lock, "cpu_threads", 0, 0, NULL);
        lock_ready = 1;
    }
    sceKernelLockLwMutex(&lock, 1, NULL);
    int i = 0;
    while (i < num_threads && strcmp(threads[i].name, name)) i++; // same name: a new instance
    if (i < MAX_THREADS) {
        strncpy(threads[i].name, name, sizeof(threads[0].name) - 1);
        threads[i].thid = thid;
        threads[i].last = 0;
        if (i == num_threads) num_threads++;
    }
    sceKernelUnlockLwMutex(&lock, 1);
}

void cpu_report(double seconds) {
    if (seconds <= 0 || !num_threads) return;
    char line[512];
    int len = 0;
    sceKernelLockLwMutex(&lock, 1, NULL);
    for (int i = 0; i < num_threads && len < (int) sizeof(line) - 48; i++) {
        SceKernelThreadInfo info;
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(threads[i].thid, &info) < 0) continue; // exited
        double pct = (info.runClocks - threads[i].last) / 10000.0 / seconds;
        threads[i].last = info.runClocks;
        if (pct < 0.5) continue;
        len += snprintf(line + len, sizeof(line) - len, "%s%s %.0f%% (p%d m%x)", len ? ", " : "",
                        threads[i].name, pct, info.currentPriority, info.currentCpuAffinityMask >> 16);
    }
    sceKernelUnlockLwMutex(&lock, 1);
    l_info("threads cpu: %s", len ? line : "-");
}
