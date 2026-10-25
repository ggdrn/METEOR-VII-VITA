/*
 * Native shared 3D engine conversions (see ff7_engine.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_ENGINE_H
#define FF7_ENGINE_H

/** Hooks func_0x00661465, func_0x006611da and func_0x00661098. Call before ff7_memmap_patch(). */
void ff7_engine_patch(void);

/** Logs statistics accumulated over `seconds`, then resets them. */
void ff7_engine_report(double seconds);

#endif // FF7_ENGINE_H
