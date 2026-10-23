/*
 * MyDecoder (FMV player) reimplementation: WebM (VP8 + Vorbis) from the OBB.
 *
 * On Android the game plays its movies with MediaPlayer into a
 * SurfaceTexture bound to a GL texture it owns (SET_TEXTURE), and syncs field
 * scripts to the movie clock through FRAME/GET_POSITION. Here:
 *  - a decoder thread demuxes the .webm straight from the OBB (nestegg),
 *    decodes VP8 frames (libvpx) into a small queue of I420 frames and Vorbis
 *    audio (libvorbis) into a PCM ring;
 *  - FRAME(), on the game's GL thread, waits for the frame due at the current
 *    movie time and uploads it to the game's texture as a native vitaGL YUV
 *    texture (the GPU converts it to RGB);
 *  - the OpenSL ES output thread mixes the movie audio in (movie_mix_audio),
 *    resampled to the mixer rate.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/movie.h"

#include "reimpl/gl_queue.h"
#include "utils/logger.h"
#include "utils/zip.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nestegg/nestegg.h>
#include <vorbis/codec.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vitaGL.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include "utils/cpu.h"

extern zip_archive * obb_zip;

// MyDecoder.frame2(): reports the end when less than this is left
#define MOVIE_END_MARGIN_MS 66
#define VIDEO_QUEUE 6
#define AUDIO_RING_FRAMES (32000 * 2)   // 2 s of stereo frames at the source rate
#define MIXER_RATE 44100                // OpenSL ES mixer output (source/opensles/Vita.c)
#define FRAME_WAIT_MAX_US 100000

typedef struct {
    uint8_t * yuv;      // contiguous I420
    uint32_t pts_ms;
    int ready;
} video_frame_t;

static struct {
    // state shared with the game thread
    volatile int playing;
    volatile int paused;
    volatile int stop;
    volatile int eof;
    char name[128];
    int duration_ms;
    int width, height;
    GLuint texture;
    SceUInt64 start_us, pause_us;
    int shown;          // frames uploaded

    // container
    nestegg * ne;
    int64_t base, size, pos;
    unsigned int video_track, audio_track;
    int has_video, has_audio;

    // video
    vpx_codec_ctx_t vpx;
    video_frame_t frames[VIDEO_QUEUE];
    volatile uint32_t vq_head, vq_tail;   // written by decoder / game thread

    // audio
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    int audio_rate, audio_channels;
    int16_t * ring;                       // interleaved stereo
    volatile uint32_t ring_head, ring_tail;
    double resample_pos;
    volatile float volume;

    SceUID thread;
} mv;

static GLuint movie_texture = 0;

static struct {
    unsigned decoded, dropped;
    SceUInt64 decode_us, decode_max_us, upload_us, upload_max_us;
} ms;

/*
 * nestegg IO over the OBB entry
 */
// nestegg: returns the number of bytes read (may be short), 0 at the end, -1 on error
static int64_t io_read(void * buffer, size_t length, void * userdata) {
    int64_t avail = mv.size - mv.pos;
    if (avail <= 0) return 0;
    size_t n = (int64_t) length < avail ? length : (size_t) avail;
    if (zip_read_raw(obb_zip, mv.base + mv.pos, buffer, n) < 0) return -1;
    mv.pos += n;
    return (int64_t) n;
}

static int io_seek(int64_t offset, int whence, void * userdata) {
    int64_t p = whence == NESTEGG_SEEK_SET ? offset
              : whence == NESTEGG_SEEK_CUR ? mv.pos + offset
              : mv.size + offset;
    if (p < 0 || p > mv.size) return -1;
    mv.pos = p;
    return 0;
}

static int64_t io_tell(void * userdata) {
    return mv.pos;
}

static SceUInt64 now_us(void) {
    return sceKernelGetProcessTimeWide();
}

/*
 * Audio: Vorbis -> int16 stereo ring at the source rate
 */
static uint32_t ring_used(void) {
    return mv.ring_head - mv.ring_tail;
}

static int audio_init(void) {
    vorbis_info_init(&mv.vi);
    vorbis_comment_init(&mv.vc);

    unsigned int count = 0;
    if (nestegg_track_codec_data_count(mv.ne, mv.audio_track, &count) < 0 || count != 3) return -1;
    for (unsigned int i = 0; i < count; i++) {
        unsigned char * data;
        size_t len;
        if (nestegg_track_codec_data(mv.ne, mv.audio_track, i, &data, &len) < 0) return -1;
        ogg_packet op = { .packet = data, .bytes = (long) len, .b_o_s = i == 0, .packetno = i };
        if (vorbis_synthesis_headerin(&mv.vi, &mv.vc, &op) < 0) return -1;
    }
    if (vorbis_synthesis_init(&mv.vd, &mv.vi) < 0) return -1;
    vorbis_block_init(&mv.vd, &mv.vb);
    mv.audio_rate = mv.vi.rate;
    mv.audio_channels = mv.vi.channels;
    mv.ring = calloc(AUDIO_RING_FRAMES * 2, sizeof(int16_t));
    return mv.ring ? 0 : -1;
}

static void audio_decode(unsigned char * data, size_t len) {
    ogg_packet op = { .packet = data, .bytes = (long) len };
    if (vorbis_synthesis(&mv.vb, &op) != 0) return;
    vorbis_synthesis_blockin(&mv.vd, &mv.vb);

    float ** pcm;
    int samples;
    while ((samples = vorbis_synthesis_pcmout(&mv.vd, &pcm)) > 0) {
        // Wait for room (the mixer drains it in real time)
        while (!mv.stop && ring_used() + samples > AUDIO_RING_FRAMES)
            sceKernelDelayThread(5000);
        if (mv.stop) return;

        for (int i = 0; i < samples; i++) {
            float l = pcm[0][i];
            float r = mv.audio_channels > 1 ? pcm[1][i] : l;
            int li = (int) (l * 32767.f), ri = (int) (r * 32767.f);
            li = li > 32767 ? 32767 : li < -32768 ? -32768 : li;
            ri = ri > 32767 ? 32767 : ri < -32768 ? -32768 : ri;
            uint32_t at = (mv.ring_head + i) % AUDIO_RING_FRAMES;
            mv.ring[at * 2] = (int16_t) li;
            mv.ring[at * 2 + 1] = (int16_t) ri;
        }
        __sync_synchronize();
        mv.ring_head += samples;
        vorbis_synthesis_read(&mv.vd, samples);
    }
}

// Called by the OpenSL ES output thread: adds the movie audio to `stream`
void movie_mix_audio(int16_t * stream, int frames) {
    if (!mv.playing || mv.paused || !mv.ring || !mv.audio_rate) return;
    const double step = (double) mv.audio_rate / MIXER_RATE;
    const float vol = mv.volume;
    for (int i = 0; i < frames; i++) {
        uint32_t idx = (uint32_t) mv.resample_pos;
        if (mv.ring_tail + idx + 1 >= mv.ring_head) break; // underrun: stay silent
        float frac = (float) (mv.resample_pos - idx);
        uint32_t a = (mv.ring_tail + idx) % AUDIO_RING_FRAMES;
        uint32_t b = (a + 1) % AUDIO_RING_FRAMES;
        for (int c = 0; c < 2; c++) {
            float s = mv.ring[a * 2 + c] + (mv.ring[b * 2 + c] - mv.ring[a * 2 + c]) * frac;
            int v = stream[i * 2 + c] + (int) (s * vol);
            stream[i * 2 + c] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t) v;
        }
        mv.resample_pos += step;
    }
    uint32_t consumed = (uint32_t) mv.resample_pos;
    mv.resample_pos -= consumed;
    __sync_synchronize();
    mv.ring_tail += consumed;
}

/*
 * Video: VP8 -> queue of contiguous I420 frames
 */
static void video_decode(unsigned char * data, size_t len, uint32_t pts_ms) {
    SceUInt64 t0 = now_us();
    if (vpx_codec_decode(&mv.vpx, data, (unsigned int) len, NULL, 0) != VPX_CODEC_OK) {
        l_warn("movie: VP8 decode error: %s", vpx_codec_error(&mv.vpx));
        return;
    }
    SceUInt64 dt = now_us() - t0;
    ms.decode_us += dt;
    if (dt > ms.decode_max_us) ms.decode_max_us = dt;
    ms.decoded++;

    vpx_codec_iter_t iter = NULL;
    vpx_image_t * img;
    while ((img = vpx_codec_get_frame(&mv.vpx, &iter)) != NULL) {
        while (!mv.stop && mv.vq_head - mv.vq_tail >= VIDEO_QUEUE)
            sceKernelDelayThread(2000);
        if (mv.stop) return;

        video_frame_t * f = &mv.frames[mv.vq_head % VIDEO_QUEUE];
        int w = mv.width, h = mv.height;
        uint8_t * out = f->yuv;
        for (int y = 0; y < h; y++)
            memcpy(out + y * w, img->planes[VPX_PLANE_Y] + y * img->stride[VPX_PLANE_Y], w);
        out += w * h;
        for (int p = VPX_PLANE_U; p <= VPX_PLANE_V; p++) {
            for (int y = 0; y < h / 2; y++)
                memcpy(out + y * (w / 2), img->planes[p] + y * img->stride[p], w / 2);
            out += (w / 2) * (h / 2);
        }
        f->pts_ms = pts_ms;
        __sync_synchronize();
        mv.vq_head++;
    }
}

static int decoder_thread(SceSize args, void * argp) {
    nestegg_packet * pkt;
    while (!mv.stop) {
        int r = nestegg_read_packet(mv.ne, &pkt);
        if (r <= 0) break;

        unsigned int track, count;
        uint64_t tstamp = 0;
        nestegg_packet_track(pkt, &track);
        nestegg_packet_tstamp(pkt, &tstamp);
        nestegg_packet_count(pkt, &count);
        for (unsigned int i = 0; i < count && !mv.stop; i++) {
            unsigned char * data;
            size_t len;
            if (nestegg_packet_data(pkt, i, &data, &len) < 0) continue;
            if (mv.has_video && track == mv.video_track)
                video_decode(data, len, (uint32_t) (tstamp / 1000000));
            else if (mv.has_audio && track == mv.audio_track)
                audio_decode(data, len);
        }
        nestegg_free_packet(pkt);
    }
    mv.eof = 1;
    return 0;
}

/*
 * Game-facing API
 */
static int position_ms(void) {
    SceUInt64 t = mv.paused ? mv.pause_us : now_us();
    int pos = (int) ((t - mv.start_us) / 1000);
    return pos < mv.duration_ms ? pos : mv.duration_ms;
}

static void close_container(void) {
    if (mv.thread > 0) {
        mv.stop = 1;
        sceKernelWaitThreadEnd(mv.thread, NULL, NULL);
        sceKernelDeleteThread(mv.thread);
        mv.thread = -1;
    }
    if (mv.has_video) vpx_codec_destroy(&mv.vpx);
    if (mv.has_audio && mv.audio_rate) {
        vorbis_block_clear(&mv.vb);
        vorbis_dsp_clear(&mv.vd);
    }
    if (mv.has_audio) {
        vorbis_comment_clear(&mv.vc);
        vorbis_info_clear(&mv.vi);
    }
    if (mv.ne) nestegg_destroy(mv.ne);
    mv.ne = NULL;
    free(mv.ring);
    mv.ring = NULL;
    for (int i = 0; i < VIDEO_QUEUE; i++) {
        free(mv.frames[i].yuv);
        mv.frames[i].yuv = NULL;
    }
    mv.has_video = mv.has_audio = 0;
    mv.audio_rate = 0;
}

// Height of the playing video, 0 when none: one word written by the game
// thread (START / RESET) and read by the main thread for the screen zoom,
// instead of the decoder state, which the game thread keeps changing
static volatile int active_height;

void movie_stop(void) {
    active_height = 0;
    if (mv.playing) {
        l_info("MyDecoder: stop %s at %d ms (%d frames shown, %u dropped)", mv.name, position_ms(), mv.shown, ms.dropped);
        if (ms.decoded)
            l_info("MyDecoder: VP8 decode avg %.1f ms (max %.1f), upload avg %.2f ms (max %.2f)",
                   ms.decode_us / 1000.0 / ms.decoded, ms.decode_max_us / 1000.0,
                   mv.shown ? ms.upload_us / 1000.0 / mv.shown : 0.0, ms.upload_max_us / 1000.0);
    }
    memset(&ms, 0, sizeof(ms));
    mv.playing = 0;
    mv.paused = 0;
    close_container();
}

bool movie_is_playing(void) {
    return active_height != 0;
}

int movie_video_height(void) {
    return active_height;
}

bool movie_start(const char * fname) {
    movie_stop();

    // MyDecoder.START: drop the leading '/', .avi -> .webm, under assets/
    char path[256];
    snprintf(path, sizeof(path), "assets/%s", fname[0] == '/' ? fname + 1 : fname);
    char * ext = strstr(path, ".avi");
    if (ext) memcpy(ext, ".webm", 6);

    zip_entry * e = obb_zip ? zip_find(obb_zip, path) : NULL;
    if (!e || e->method != 0) {
        l_warn("MyDecoder.START(\"%s\"): %s not in OBB", fname, path);
        return false;
    }
    mv.base = zip_entry_data_offset(obb_zip, e);
    mv.size = e->uncomp_size;
    mv.pos = 0;

    nestegg_io io = { io_read, io_seek, io_tell, NULL };
    if (nestegg_init(&mv.ne, io, NULL, -1) < 0) {
        l_error("MyDecoder.START(\"%s\"): not a valid WebM", fname);
        mv.ne = NULL;
        return false;
    }

    uint64_t duration = 0;
    nestegg_duration(mv.ne, &duration);
    mv.duration_ms = (int) (duration / 1000000);

    unsigned int tracks = 0;
    nestegg_track_count(mv.ne, &tracks);
    for (unsigned int t = 0; t < tracks; t++) {
        int type = nestegg_track_type(mv.ne, t);
        int codec = nestegg_track_codec_id(mv.ne, t);
        if (type == NESTEGG_TRACK_VIDEO && codec == NESTEGG_CODEC_VP8 && !mv.has_video) {
            nestegg_video_params vp;
            nestegg_track_video_params(mv.ne, t, &vp);
            mv.width = vp.width & ~1;
            mv.height = vp.height & ~1;
            vpx_codec_dec_cfg_t cfg = { .threads = 2, .w = vp.width, .h = vp.height };
            if (vpx_codec_dec_init(&mv.vpx, vpx_codec_vp8_dx(), &cfg, 0) == VPX_CODEC_OK) {
                mv.video_track = t;
                mv.has_video = 1;
            }
        } else if (type == NESTEGG_TRACK_AUDIO && codec == NESTEGG_CODEC_VORBIS && !mv.has_audio) {
            mv.audio_track = t;
            mv.has_audio = 1;
            if (audio_init() < 0) {
                l_warn("MyDecoder: could not initialize Vorbis audio");
                if (mv.audio_rate == 0) { vorbis_comment_clear(&mv.vc); vorbis_info_clear(&mv.vi); }
                mv.has_audio = 0;
            }
        }
    }

    if (mv.has_video) {
        size_t frame_size = (size_t) mv.width * mv.height * 3 / 2;
        for (int i = 0; i < VIDEO_QUEUE; i++) mv.frames[i].yuv = malloc(frame_size);
    }
    mv.vq_head = mv.vq_tail = 0;
    mv.ring_head = mv.ring_tail = 0;
    mv.resample_pos = 0;
    mv.stop = mv.eof = 0;
    mv.shown = 0;
    if (mv.volume <= 0.f) mv.volume = 1.f;

    const char * base = strrchr(path, '/');
    strncpy(mv.name, base ? base + 1 : path, sizeof(mv.name) - 1);

    mv.thread = sceKernelCreateThread("movie_decoder", decoder_thread, 0x10000100 - 0x08, 0x10000, 0, CPU_MASK_SECONDARY, NULL);
    if (mv.thread >= 0) cpu_register_thread("movie", mv.thread);
    if (mv.thread <= 0) {
        mv.thread = 0;
        l_error("MyDecoder: could not create decoder thread");
        close_container();
        return false;
    }
    sceKernelStartThread(mv.thread, 0, NULL);

    mv.start_us = now_us();
    mv.paused = 0;
    mv.playing = 1;
    active_height = mv.has_video ? mv.height : 0;
    l_info("MyDecoder.START(\"%s\"): %d ms, video %dx%d%s, audio %d Hz", fname, mv.duration_ms,
           mv.width, mv.height, mv.has_video ? "" : " (none)", mv.has_audio ? mv.audio_rate : 0);
    return true;
}

void movie_set_texture(unsigned int tex) {
    movie_texture = tex;
}

static void upload(video_frame_t * f) {
    if (!movie_texture) return;
    gl_queue_finish(); // GL used directly below, on the game thread
    SceUInt64 t0 = now_us();
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture(GL_TEXTURE_2D, movie_texture);

    // New texture memory for every frame. vitaGL renders a frame on the GPU
    // after the CPU has moved on, so the previous movie frame may still be
    // being sampled: writing over its memory in place showed up as torn,
    // half-updated frames. Re-specifying the texture gives each frame its own
    // buffer, and vitaGL frees the old one only once the GPU is done with it.
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, VGL_YUV420P_BT601, mv.width, mv.height, 0,
                           mv.width * mv.height * 3 / 2, f->yuv);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, (GLuint) previous);
    mv.shown++;

    SceUInt64 dt = now_us() - t0;
    ms.upload_us += dt;
    if (dt > ms.upload_max_us) ms.upload_max_us = dt;
}

bool movie_frame(void) {
    if (!mv.playing) return false;
    if (mv.paused) return true;

    // Like MediaPlayer + SurfaceTexture: wait for the frame due now, upload it
    SceUInt64 waited = 0;
    while (waited < FRAME_WAIT_MAX_US) {
        uint32_t now = (uint32_t) position_ms();
        uint32_t head = mv.vq_head; // decoder may append meanwhile; use a snapshot

        // Drop frames that are late, keeping the newest one already due
        while (head - mv.vq_tail >= 2 && mv.frames[(mv.vq_tail + 1) % VIDEO_QUEUE].pts_ms <= now) {
            mv.vq_tail++;
            ms.dropped++;
        }

        if (mv.vq_tail != head && mv.frames[mv.vq_tail % VIDEO_QUEUE].pts_ms <= now) {
            upload(&mv.frames[mv.vq_tail % VIDEO_QUEUE]);
            __sync_synchronize();
            mv.vq_tail++;
            break;
        }
        if (mv.vq_tail == mv.vq_head && mv.eof) break; // nothing left to show

        // Next frame not due yet (or still decoding): wait a bit
        SceUInt64 nap = 2000;
        if (mv.vq_tail != mv.vq_head) {
            int64_t ahead = (int64_t) mv.frames[mv.vq_tail % VIDEO_QUEUE].pts_ms - now;
            if (ahead > 0) nap = (SceUInt64) ahead * 1000;
        }
        if (nap > FRAME_WAIT_MAX_US - waited) nap = FRAME_WAIT_MAX_US - waited;
        sceKernelDelayThread((SceUInt32) nap);
        waited += nap;
    }

    int left = mv.duration_ms - position_ms();
    if (left <= MOVIE_END_MARGIN_MS || (mv.eof && mv.vq_tail == mv.vq_head && left <= 500)) {
        l_info("MyDecoder: %s finished (%d frames shown)", mv.name, mv.shown);
        return false;
    }
    return true;
}

int movie_get_position_ms(void) {
    return mv.playing ? position_ms() : 0;
}

int movie_get_total_ms(void) {
    return mv.playing ? mv.duration_ms : 0;
}

void movie_set_position_ms(int pos) {
    if (!mv.playing) return;
    // Rare in FF7: move the clock only (late frames are dropped, early ones wait)
    l_info("MyDecoder.SET_POSITION(%d) on %s", pos, mv.name);
    mv.start_us = now_us() - (SceUInt64) (pos > 0 ? pos : 0) * 1000;
}

void movie_set_volume(float volume) {
    mv.volume = volume < 0.f ? 0.f : volume > 1.f ? 1.f : volume;
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
