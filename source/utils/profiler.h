/*
 * Sampling profiler for the game's .so (development aid, ENABLE_PROFILER).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_PROFILER_H
#define SOLOADER_PROFILER_H

/** Starts sampling the .so into DATA_PATH "profile.bin" (see profiler.c). */
void profiler_start(void);

#endif // SOLOADER_PROFILER_H
