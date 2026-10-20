/*
 * Sampling profiler for the game's .so (development aid, ENABLE_PROFILER).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_PROFILER_H
#define SOLOADER_PROFILER_H

#include <psp2/types.h>

/**
 * Starts sampling the .so into DATA_PATH "profile.bin" (see profiler.c).
 * With `frame_start` (0 between frames) and `threshold_us`, samples are only
 * taken while the current frame has been running for longer than the
 * threshold: stutters get profiled, normal frames cost nothing.
 */
void profiler_start(volatile SceUInt64 * frame_start, SceUInt64 threshold_us);

/**
 * Bracket code that writes to the .so text and flushes the caches (hooks
 * that restore the original instructions, patching): disarms the sampler and
 * keeps it off until profiler_release(). No-ops when the profiler is off.
 */
void profiler_hold(void);
void profiler_release(void);

/** Logs sampler counters (diagnostics). */
void profiler_report(void);

#endif // SOLOADER_PROFILER_H
