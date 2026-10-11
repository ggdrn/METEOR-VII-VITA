/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * FF7 Vita port: replacement for lib/opensles/libopensles/Vita.c.
 *
 * The original backend mixes a single engine: each slCreateEngine() calls
 * SDL_open(), which tears down the previous engine's audio. FF7 creates two
 * engines, so the first one (music and sound effects) went silent and every
 * call that waits for a mixer acknowledgement (Play::SetPlayState(STOPPED),
 * BufferQueue::Clear, Object::Destroy) hung the game forever.
 *
 * This backend keeps a list of engines and one output thread that mixes the
 * output mix of every registered engine.
 */

#include "sles_allinclusive.h"
#include <vitasdk.h>
#include <psp2/kernel/cpu.h>

#define MAX_ENGINES 4

// FMV audio (source/reimpl/movie.c) is mixed into the same output
void movie_mix_audio(int16_t *stream, int frames);
#define FRAMES (SndFile_BUFSIZE / 4) // stereo 16-bit frames per output buffer

static IEngine *engines[MAX_ENGINES];
static SceKernelLwMutexWork engines_lock;
static int engines_lock_ready;

static SceUID audio_thread = -1;
static volatile int audio_shutdown_requested;

static int16_t out_buf[SndFile_NUMBUFS][FRAMES * 2];
static int16_t mix_buf[FRAMES * 2];

static int opensles_output_freq(void) {
	return (&_opensles_user_freq != NULL && _opensles_user_freq > 0) ? _opensles_user_freq : 44100;
}

static void lock_engines(void) {
	if (!engines_lock_ready) {
		// recursive: buffer queue callbacks run on the audio thread while it holds the lock
		sceKernelCreateLwMutex(&engines_lock, "opensles_engines", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
		engines_lock_ready = 1;
	}
	sceKernelLockLwMutex(&engines_lock, 1, NULL);
}

static void unlock_engines(void) {
	sceKernelUnlockLwMutex(&engines_lock, 1);
}

static inline int16_t clamp16(int v) {
	return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t) v);
}

// Mixes every registered engine into `stream` (stereo 16-bit, FRAMES frames)
static void fill_output_buffer(int16_t *stream) {
	int first = 1;
	sceClibMemset(stream, 0, sizeof(out_buf[0]));

	lock_engines();
	for (int e = 0; e < MAX_ENGINES; e++) {
		if (NULL == engines[e] || NULL == engines[e]->mOutputMix)
			continue;
		SLOutputMixExtItf ext = &engines[e]->mOutputMix->mOutputMixExt.mItf;
		if (first) {
			IOutputMixExt_FillBuffer(ext, stream, sizeof(out_buf[0]));
			first = 0;
		} else {
			IOutputMixExt_FillBuffer(ext, mix_buf, sizeof(mix_buf));
			for (int i = 0; i < FRAMES * 2; i++)
				stream[i] = clamp16(stream[i] + mix_buf[i]);
		}
	}
	unlock_engines();

	movie_mix_audio(stream, FRAMES);
}

static int audioThread(SceSize args, void *argp) {
	(void) args; (void) argp;

	int ch = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, FRAMES, opensles_output_freq(),
	                             SCE_AUDIO_OUT_MODE_STEREO);
	if (ch < 0) {
		SL_LOGE("Unable to open Vita audio port: 0x%x", ch);
		return ch;
	}
	SL_LOGI("Opened Vita audio port %d (%d Hz, %d frames)", ch, opensles_output_freq(), FRAMES);

	int vol[] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
	sceAudioOutSetVolume(ch, (SceAudioOutChannelFlag) (SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);

	int buf = 0;
	while (!audio_shutdown_requested) {
		fill_output_buffer(out_buf[buf]);
		int res = sceAudioOutOutput(ch, out_buf[buf]);
		if (res < 0) {
			SL_LOGE("Vita audio output failed on port %d: 0x%x", ch, res);
			break;
		}
		buf = (buf + 1) % SndFile_NUMBUFS;
	}

	sceAudioOutReleasePort(ch);
	return 0;
}

/** \brief Called during slCreateEngine: registers the engine with the mixer */
void SDL_open(IEngine *thisEngine)
{
	if (NULL == thisEngine)
		return;

	lock_engines();
	int slot = -1;
	for (int e = 0; e < MAX_ENGINES; e++) {
		if (engines[e] == thisEngine) { slot = e; break; }
		if (slot < 0 && NULL == engines[e]) slot = e;
	}
	if (slot >= 0) {
		engines[slot] = thisEngine;
		SL_LOGI("SDL_open: engine %p registered in slot %d", thisEngine, slot);
	} else {
		SL_LOGE("SDL_open: too many engines (%d), engine %p will be silent", MAX_ENGINES, thisEngine);
	}
	unlock_engines();

	if (audio_thread < 0) {
		audio_shutdown_requested = 0;
		audio_thread = sceKernelCreateThread("OpenSLES Playback", audioThread, 0x10000100, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2, NULL) /* off the game thread's core 0 */;
		{ extern void cpu_register_thread(const char *, int); if (audio_thread >= 0) cpu_register_thread("sl_mixer", audio_thread); }
		if (audio_thread < 0) {
			SL_LOGE("Unable to create OpenSLES playback thread: 0x%x", audio_thread);
			return;
		}
		sceKernelStartThread(audio_thread, 0, NULL);
	}
}

/** \brief Called from CEngine_Destroy: the engine stops being mixed */
void SDL_close_engine(IEngine *thisEngine)
{
	// Taking the lock waits for any mix pass that may still be using this engine
	lock_engines();
	for (int e = 0; e < MAX_ENGINES; e++) {
		if (engines[e] == thisEngine) {
			engines[e] = NULL;
			SL_LOGI("SDL_close_engine: engine %p unregistered from slot %d", thisEngine, e);
		}
	}
	unlock_engines();
}

/** \brief Stops the output thread altogether */
void SDL_close(void)
{
	lock_engines();
	for (int e = 0; e < MAX_ENGINES; e++)
		engines[e] = NULL;
	unlock_engines();

	if (audio_thread >= 0) {
		audio_shutdown_requested = 1;
		sceKernelWaitThreadEnd(audio_thread, NULL, NULL);
		sceKernelDeleteThread(audio_thread);
		audio_thread = -1;
	}
}
