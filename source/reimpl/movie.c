/*
 * MyDecoder (FMV player) reimplementation — timing only for now.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/movie.h"

#include "utils/logger.h"
#include "utils/zip.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

extern zip_archive * obb_zip;

// FF7 PC FMVs are 15 fps; the WebM files carry no DefaultDuration
#define MOVIE_FRAME_US (1000000 / 15)
// MyDecoder.frame2(): reports the end when less than this is left
#define MOVIE_END_MARGIN_MS 66
#define WEBM_HEADER_BYTES (64 * 1024)

static struct {
    bool playing;
    bool paused;
    char name[128];
    int duration_ms;
    SceUInt64 start_us;   // clock origin (moves on seek/pause)
    SceUInt64 pause_us;
    uint32_t frames;
} mv;

/*
 * Minimal EBML walk: Segment > Info > {TimecodeScale, Duration}.
 */
static int ebml_vint(const uint8_t * b, size_t len, size_t i, uint64_t * out, bool keep_marker) {
    if (i >= len) return 0;
    uint8_t first = b[i];
    int l = 1;
    uint8_t m = 0x80;
    while (l <= 8 && !(first & m)) { m >>= 1; l++; }
    if (l > 8 || i + l > len) return 0;
    uint64_t v = keep_marker ? first : (first & (m - 1));
    for (int k = 1; k < l; k++) v = (v << 8) | b[i + k];
    *out = v;
    return l;
}

static void ebml_walk(const uint8_t * b, size_t i, size_t end, uint64_t * scale, double * duration) {
    while (i < end) {
        uint64_t id, size;
        int l1 = ebml_vint(b, end, i, &id, true);
        if (!l1) return;
        int l2 = ebml_vint(b, end, i + l1, &size, false);
        if (!l2) return;
        i += l1 + l2;
        if (size == (1ULL << (7 * l2)) - 1 || i + size > end) size = end - i; // unknown/truncated

        switch (id) {
            case 0x18538067: // Segment
            case 0x1549A966: // Info
                ebml_walk(b, i, i + size, scale, duration);
                break;
            case 0x2AD7B1: { // TimecodeScale
                uint64_t v = 0;
                for (uint64_t k = 0; k < size; k++) v = (v << 8) | b[i + k];
                *scale = v;
                break;
            }
            case 0x4489: { // Duration (float)
                uint64_t raw = 0;
                for (uint64_t k = 0; k < size; k++) raw = (raw << 8) | b[i + k];
                if (size == 4) { uint32_t r32 = (uint32_t) raw; float f; memcpy(&f, &r32, 4); *duration = f; }
                else if (size == 8) { memcpy(duration, &raw, 8); }
                break;
            }
            case 0x1F43B675: // Cluster: header is over
                return;
        }
        i += size;
    }
}

static int webm_duration_ms(zip_entry * e) {
    size_t len = e->uncomp_size < WEBM_HEADER_BYTES ? e->uncomp_size : WEBM_HEADER_BYTES;
    uint8_t * b = malloc(len);
    int64_t off = zip_entry_data_offset(obb_zip, e);
    if (!b || off < 0 || zip_read_raw(obb_zip, off, b, len) < 0) {
        free(b);
        return -1;
    }

    uint64_t scale = 1000000; // default: 1 ms
    double duration = 0;
    ebml_walk(b, 0, len, &scale, &duration);
    free(b);
    return (int) (duration * (double) scale / 1000000.0);
}

static SceUInt64 now_us(void) {
    return sceKernelGetProcessTimeWide();
}

bool movie_start(const char * fname) {
    movie_stop();

    // MyDecoder.START: drop the leading '/', .avi -> .webm, under assets/
    char path[256];
    snprintf(path, sizeof(path), "assets/%s", fname[0] == '/' ? fname + 1 : fname);
    char * ext = strstr(path, ".avi");
    if (ext) memcpy(ext, ".webm", 6);

    zip_entry * e = obb_zip ? zip_find(obb_zip, path) : NULL;
    if (!e) {
        l_warn("MyDecoder.START(\"%s\"): %s not in OBB", fname, path);
        return false;
    }

    int duration = webm_duration_ms(e);
    if (duration <= 0) {
        l_warn("MyDecoder.START(\"%s\"): could not read duration", fname);
        return false;
    }

    const char * base = strrchr(path, '/');
    strncpy(mv.name, base ? base + 1 : path, sizeof(mv.name) - 1);
    mv.duration_ms = duration;
    mv.start_us = now_us();
    mv.frames = 0;
    mv.paused = false;
    mv.playing = true;
    l_info("MyDecoder.START(\"%s\"): %d ms (silent playback, no video yet)", fname, duration);
    return true;
}

int movie_get_position_ms(void) {
    if (!mv.playing) return 0;
    SceUInt64 t = mv.paused ? mv.pause_us : now_us();
    int pos = (int) ((t - mv.start_us) / 1000);
    return pos < mv.duration_ms ? pos : mv.duration_ms;
}

int movie_get_total_ms(void) {
    return mv.playing ? mv.duration_ms : 0;
}

bool movie_frame(void) {
    if (!mv.playing) return false;
    if (mv.paused) return true;

    // Android blocks here until the decoder delivers the next frame
    SceUInt64 due = mv.start_us + (SceUInt64) mv.frames * MOVIE_FRAME_US;
    SceUInt64 now = now_us();
    if (due > now) sceKernelDelayThread((SceUInt32) (due - now));
    mv.frames++;

    int left = mv.duration_ms - movie_get_position_ms();
    if (left <= MOVIE_END_MARGIN_MS) {
        l_info("MyDecoder: %s finished (%u frames)", mv.name, mv.frames);
        return false;
    }
    return true;
}

void movie_set_position_ms(int pos) {
    if (!mv.playing) return;
    if (pos < 0) pos = 0;
    mv.start_us = now_us() - (SceUInt64) pos * 1000;
    mv.frames = (uint32_t) ((SceUInt64) pos * 1000 / MOVIE_FRAME_US);
    l_info("MyDecoder.SET_POSITION(%d) on %s", pos, mv.name);
}

void movie_stop(void) {
    if (mv.playing) l_info("MyDecoder: stop %s at %d ms", mv.name, movie_get_position_ms());
    mv.playing = false;
    mv.paused = false;
}

void movie_pause(bool pause) {
    if (!mv.playing || pause == mv.paused) return;
    if (pause) {
        mv.pause_us = now_us();
    } else {
        mv.start_us += now_us() - mv.pause_us; // resume where it stopped
    }
    mv.paused = pause;
}
