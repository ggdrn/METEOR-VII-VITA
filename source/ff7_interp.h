/*
 * Battle at 30 fps, phase 0: the draw matching probe (ff7_interp.c).
 *
 * gl_queue.c notes the GL state the game thread sets (ff7_ip_note) and
 * hands every draw of the game thread to ff7_interp_draw while a battle
 * frame is being recorded (ff7_ip_recording).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_INTERP_H
#define FF7_INTERP_H

#include <stdint.h>

enum {
    IP_FB, IP_ACTIVE_TEX, IP_TEX0, IP_TEX1, IP_BLEND_S, IP_BLEND_D, IP_CAPS, IP_DEPTH_FUNC, IP_DEPTH_MASK,
    IP_PROGRAM, IP_UNIFORMS, IP_MATRIX, IP_COLOR_MASK, IP_CULL, IP_VIEWPORT, IP_STATE_WORDS
};

extern uint32_t ff7_ip_state[IP_STATE_WORDS]; // game thread only
extern volatile int ff7_ip_recording;

static inline void ff7_ip_note(int word, uint32_t v) {
    ff7_ip_state[word] = v;
}

void ff7_ip_note_fb(uint32_t fb);
void ff7_ip_note_texture(uint32_t texture);
void ff7_ip_note_cap(uint32_t cap, int on);
void ff7_ip_note_program(uint32_t program);
void ff7_ip_note_uniform(int32_t location, int32_t value);
void ff7_ip_note_words(int word, const void * data, uint32_t len); // hash of a block (matrices...)
void ff7_ip_note_viewport(int32_t x, int32_t y, int32_t w, int32_t h);

void ff7_interp_draw(uint32_t mode, int32_t first, int32_t count, const void * pos, int32_t pos_size, uint32_t pos_type,
                     int32_t pos_stride, const void * uv, int32_t uv_size, uint32_t uv_type, int32_t uv_stride,
                     int32_t rec);

void ff7_interp_patch(void);
void ff7_interp_set(int on);     // battle at 30 fps on/off (port menu)
int ff7_interp_get(void);        // on (and installed)
int ff7_interp_available(void);  // installed
void ff7_interp_report(double seconds);

#endif // FF7_INTERP_H
