/*
 * Music tracks read in the background for libjni_ff7.so (see ff7_music.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_MUSIC_H
#define FF7_MUSIC_H

/** Hooks SND_play and friends. Call before ff7_memmap_patch(). */
void ff7_music_patch(void);

/** Starts a track whose file finished loading. Game thread only; cheap when
 *  there is nothing to do. */
void ff7_music_poll(void);

#endif // FF7_MUSIC_H
