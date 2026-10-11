/*
 * MyDecoder (FMV player) reimplementation: WebM (VP8 + Vorbis) from the OBB.
 * See movie.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_MOVIE_H
#define SOLOADER_MOVIE_H

#include <stdbool.h>
#include <stdint.h>

/** MyDecoder.START: `fname` as passed by the game ("/ff7_1.02/.../x.avi"). */
bool movie_start(const char * fname);

/** MyDecoder.FRAME: uploads the frame due now; false once the movie is over. */
bool movie_frame(void);

/** MyDecoder.RESET */
void movie_stop(void);

/** MyDecoder.SET_TEXTURE: GL texture the game draws the movie with. */
void movie_set_texture(unsigned int tex);

void movie_set_volume(float volume);
int movie_get_total_ms(void);
int movie_get_position_ms(void);
void movie_set_position_ms(int pos);

/** MainActivity onPause/onResume */
void movie_pause(bool pause);

/** Mixes the movie audio into `stream` (stereo int16 at 44.1 kHz). Audio thread. */
void movie_mix_audio(int16_t * stream, int frames);

#endif // SOLOADER_MOVIE_H
