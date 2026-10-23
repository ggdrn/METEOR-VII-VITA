/*
 * Native snowboard minigame functions (see ff7_snowboard.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_SNOWBOARD_H
#define FF7_SNOWBOARD_H

/** Hooks func_0x007334da, func_0x0073488d and func_0x00734cab. Call after ff7_engine_patch(), before ff7_memmap_patch(). */
void ff7_snowboard_patch(void);

/** Logs statistics accumulated over `seconds`, then resets them. */
void ff7_snowboard_report(double seconds);

#endif // FF7_SNOWBOARD_H
