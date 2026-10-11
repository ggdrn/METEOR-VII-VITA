/*
 * CPU and GPU load summary for logs.txt (see perf.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_PERF_H
#define SOLOADER_PERF_H

/** Call once after vitaGL is initialized. */
void perf_init(void);

/** Call right after each vglSwapBuffers. */
void perf_frame_submitted(void);

/** Logs the summary; call after glstats_report() and cpu_report(). */
void perf_report(double fps, double game_cpu_pct);

#endif // SOLOADER_PERF_H
