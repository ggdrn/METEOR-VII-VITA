/*
 * Faster texture conversion for libjni_ff7.so (see ff7_texture.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_TEXTURE_H
#define FF7_TEXTURE_H

/** Hooks convert_image_data (C_006B42E5). Call after relocation. */
void ff7_texture_patch(void);

/** Logs conversion statistics accumulated over `seconds`, then resets them. */
void ff7_texture_report(double seconds);

#endif // FF7_TEXTURE_H
