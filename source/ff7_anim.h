/*
 * Native bit reader for battle animation decompression (see ff7_anim.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_ANIM_H
#define FF7_ANIM_H

#include <stdint.h>

/** Hooks func_0x005e7c40 (read_bits). Call after relocation. */
void ff7_anim_patch(void);

/** The recompiled x86 register file (+0 EAX, +4 ECX, +8 EDX, +16 ESP,
 *  +20 EBP), or NULL if ff7_anim_patch() could not locate it. */
volatile uint32_t * ff7_x86_regs(void);

/** Logs call statistics accumulated over `seconds`, then resets them. */
void ff7_anim_report(double seconds);

#endif // FF7_ANIM_H
