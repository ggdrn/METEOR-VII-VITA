/*
 * Native chocobo race primitives (see ff7_chocobo.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_CHOCOBO_H
#define FF7_CHOCOBO_H

/** Hooks func_0x0077b9d4 and func_0x0077bca4. Call after ff7_engine_patch(), before ff7_memmap_patch(). */
void ff7_chocobo_patch(void);

/** Logs statistics accumulated over `seconds`, then resets them. */
void ff7_chocobo_report(double seconds);

#endif // FF7_CHOCOBO_H
