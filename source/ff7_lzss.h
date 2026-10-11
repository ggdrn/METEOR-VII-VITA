/*
 * Native LZSS decompressor for libjni_ff7.so (see ff7_lzss.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_LZSS_H
#define FF7_LZSS_H

/** Hooks func_0x006305c0. Call after ff7_anim_patch(). */
void ff7_lzss_patch(void);

/** Logs statistics accumulated over `seconds`, then resets them. */
void ff7_lzss_report(double seconds);

#endif // FF7_LZSS_H
