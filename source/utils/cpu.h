/*
 * CPU core assignment.
 *
 * Core 0 runs only the game thread (the recompiled ff7.exe, always the
 * bottleneck). Everything else - main/GL swap, the game's sound threads
 * (Vorbis decoding), the OpenSL mixer, the movie decoder, logging - shares
 * cores 1 and 2, so a song or movie being decoded never takes CPU time
 * from the game.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_CPU_H
#define SOLOADER_CPU_H

#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#define CPU_MASK_GAME      SCE_KERNEL_CPU_MASK_USER_0
#define CPU_MASK_SECONDARY (SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2)

/** Adds a thread to the per-thread CPU report. */
void cpu_register_thread(const char * name, SceUID thid);

/** Logs the CPU usage of every registered thread over `seconds`. */
void cpu_report(double seconds);

/** Moves the calling thread to `mask`. */
static inline void cpu_set_self(int mask) {
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
}

#endif // SOLOADER_CPU_H
