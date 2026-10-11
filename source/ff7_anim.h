/*
 * Native bit reader for battle animation decompression (see ff7_anim.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_ANIM_H
#define FF7_ANIM_H

/** Hooks func_0x005e7c40 (read_bits). Call after relocation. */
void ff7_anim_patch(void);

/** Logs call statistics accumulated over `seconds`, then resets them. */
void ff7_anim_report(double seconds);

#endif // FF7_ANIM_H
