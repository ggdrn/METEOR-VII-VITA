/*
 * Music tracks read in the background for libjni_ff7.so.
 *
 * SND_play (id, name) is the porters' music player: it builds
 * BPath "/" musicDir "/" name "." musicExt, reads the whole .akb (2-4 MB) with
 * __loadFile, hands it to SdSoundSystem_AddData and starts it. It runs on the
 * game thread, and a track that isn't in the OBB RAM cache took 170-600 ms to
 * read from the card: the game froze for that long on area and battle music
 * changes (v0.2 logs: "OBB slow read [dokubo.akb] ... 277 ms").
 *
 * Now, when the file isn't cached yet, the current track is stopped (as
 * SND_play would do) and a worker thread reads the file into the RAM cache
 * (obb_cache_prefetch). The game keeps running; when the read is done, the
 * game thread calls the original SND_play from ff7_music_poll(), which finds
 * the file in RAM. Everything SdSoundSystem sees still happens on the game
 * thread, in the same order. While a track is loading:
 *   - SND_getStatus reports it as playing;
 *   - SND_stop, or SND_play of a track already loaded, cancels it;
 *   - SND_play of another track replaces it;
 *   - SND_pause / SND_restart are remembered and applied once it starts.
 * Tracks that are cached, already loaded by SdSoundSystem, or not in the OBB
 * go straight to the original as before.
 *
 * Creating DATA_PATH "music_async_off.txt" turns this off (for A/B tests).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_music.h"

#include "reimpl/obb_fd.h"
#include "utils/cpu.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <kubridge.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define OFF_SWITCH      DATA_PATH "music_async_off.txt"
#define SYM_PLAY        "SND_play"
#define SYM_STOP        "SND_stop"
#define SYM_PAUSE       "SND_pause"
#define SYM_RESTART     "SND_restart"
#define SYM_STATUS      "SND_getStatus"
#define SYM_BPATH       "BPath"
#define SYM_MUSIC_DIR   "musicDir"
#define SYM_MUSIC_EXT   "musicExt"
#define SYM_ELAPSED     "SdSoundSystem_SoundCtrl_GetElapsedPlaybackTime"
#define SYM_LENGTH      "SdSoundSystem_SoundCtrl_GetPlayTime"
#define PLAY_PROLOGUE   0x47f0e92d // push.w {r4-r10, lr}
// SND_play's state block (literal at 0xadfd00, loaded at 0xadfb60):
//   +0x000 current sound, +0x004 current id,
//   +4 * (id + 102) SdSoundSystem data of each loaded track,
//   +0x730 path of the last track read
#define STATE_LITERAL   0xadfd00
#define STATE_PC        0xadfb64
#define STATE_OFFSET    0x1492c78
#define STATE_PATH      0x730
#define MAX_TRACK_ID    100
#define MAX_NAME        64
#define MIN_ASYNC_BYTES (512 * 1024) // smaller files aren't kept in the RAM cache

enum { IDLE, LOADING, READY };

typedef int (* sound_time_fn)(uintptr_t sound);

static so_hook play_hook, stop_hook, pause_hook, restart_hook;
static sound_time_fn sound_elapsed, sound_length;
static uintptr_t * snd_state;
static const char ** bpath, ** music_dir, ** music_ext;
static int enabled;

static SceKernelLwMutexWork lock;
static SceUID wake_sema;
static volatile int state = IDLE;
static struct {
    unsigned int gen;        // bumped by every new request or cancel
    unsigned int id;
    char name[MAX_NAME];
    char path[256];
    int64_t off, len;
    int paused;
    SceUInt64 requested_us, loaded_us;
} req;

static unsigned long long async_plays, sync_plays;

static int track_loaded(unsigned int id) {
    return snd_state[102 + id] != 0;
}

static int build_path(const char * name, char * path, size_t size) {
    const char * b = *bpath, * dir = *music_dir, * ext = *music_ext;
    if (!b || !dir || !ext) return 0;
    char lower[MAX_NAME];
    size_t n = strlen(name);
    if (n >= sizeof(lower)) return 0;
    for (size_t i = 0; i <= n; i++) lower[i] = (char) tolower((unsigned char) name[i]);
    int r = snprintf(path, size, "%s/%s/%s.%s", b, dir, lower, ext);
    return r > 0 && (size_t) r < size;
}

static int worker(SceSize args, void * argp) {
    (void) args; (void) argp;
    while (1) {
        sceKernelWaitSema(wake_sema, 1, NULL);

        sceKernelLockLwMutex(&lock, 1, NULL);
        if (state != LOADING) {
            sceKernelUnlockLwMutex(&lock, 1);
            continue;
        }
        unsigned int gen = req.gen;
        int64_t off = req.off, len = req.len;
        char tag[MAX_NAME];
        strcpy(tag, req.name);
        sceKernelUnlockLwMutex(&lock, 1);

        // On failure the track still starts: the original then reads it itself
        obb_cache_prefetch(off, (size_t) len, tag);

        sceKernelLockLwMutex(&lock, 1, NULL);
        if (state == LOADING && req.gen == gen) {
            req.loaded_us = sceKernelGetProcessTimeWide();
            state = READY;
        }
        sceKernelUnlockLwMutex(&lock, 1);
    }
    return 0;
}

static void cancel(void) {
    sceKernelLockLwMutex(&lock, 1, NULL);
    req.gen++;
    state = IDLE;
    sceKernelUnlockLwMutex(&lock, 1);
}

static int play_hooked(unsigned int id, const char * name) {
    char path[256];
    int64_t off, len;

    if (!enabled || !name || id >= MAX_TRACK_ID || track_loaded(id)
        || !build_path(name, path, sizeof(path)) || !obb_entry_find(path, &off, &len)
        || len < MIN_ASYNC_BYTES || obb_cache_has(off, (size_t) len)) {
        if (state != IDLE) cancel();
        sync_plays++;
        return SO_CONTINUE(int, play_hook, id, name);
    }

    sceKernelLockLwMutex(&lock, 1, NULL);
    if (state == LOADING && req.id == id && strcmp(req.name, name) == 0) {
        sceKernelUnlockLwMutex(&lock, 1); // already on its way
        return 0;
    }
    sceKernelUnlockLwMutex(&lock, 1);

    // What SND_play does before loading: the previous track stops now
    cancel();
    SO_CONTINUE(int, stop_hook);

    sceKernelLockLwMutex(&lock, 1, NULL);
    req.gen++;
    req.id = id;
    strcpy(req.name, name);
    strcpy(req.path, path);
    req.off = off;
    req.len = len;
    req.paused = 0;
    req.requested_us = sceKernelGetProcessTimeWide();
    state = LOADING;
    sceKernelUnlockLwMutex(&lock, 1);
    sceKernelSignalSema(wake_sema, 1);
    return 0;
}

static int stop_hooked(void) {
    if (state != IDLE) cancel();
    return SO_CONTINUE(int, stop_hook);
}

static int pause_hooked(void) {
    if (state != IDLE) req.paused = 1;
    return SO_CONTINUE(int, pause_hook);
}

static int restart_hooked(void) {
    if (state != IDLE) req.paused = 0;
    return SO_CONTINUE(int, restart_hook);
}

// SND_getStatus, rewritten instead of hooked: the game may poll it often and
// SO_CONTINUE rewrites code on every call. 1 while the track plays.
static int status_hooked(void) {
    if (state != IDLE) return 1;
    uintptr_t sound = snd_state[0];
    if (!sound) return 0;
    int elapsed = sound_elapsed(sound), length = sound_length(sound);
    return elapsed < length && length > 0;
}

void ff7_music_poll(void) {
    if (state != READY) return;

    sceKernelLockLwMutex(&lock, 1, NULL);
    if (state != READY) {
        sceKernelUnlockLwMutex(&lock, 1);
        return;
    }
    unsigned int id = req.id;
    int paused = req.paused;
    char name[MAX_NAME], path[256];
    strcpy(name, req.name);
    strcpy(path, req.path);
    SceUInt64 requested = req.requested_us, loaded = req.loaded_us;
    state = IDLE;
    sceKernelUnlockLwMutex(&lock, 1);

    SO_CONTINUE(int, play_hook, id, name);
    if (paused) SO_CONTINUE(int, pause_hook);
    async_plays++;

    SceUInt64 now = sceKernelGetProcessTimeWide();
    l_info("music: %s loaded in the background in %llu ms, started %llu ms after the request",
           name, (loaded - requested) / 1000, (now - requested) / 1000);

    // The path must be the one SND_play built, or the prefetch read the wrong file
    const char * game_path = (const char *) ((uintptr_t) snd_state + STATE_PATH);
    if (strcmp(game_path, path) != 0) {
        l_warn("music: path mismatch (game \"%s\", ours \"%s\"), background loading disabled", game_path, path);
        enabled = 0;
    }
}

void ff7_music_patch(void) {
    uintptr_t base = so_mod.text_base;
    uintptr_t play = so_symbol(&so_mod, SYM_PLAY), stop = so_symbol(&so_mod, SYM_STOP);
    uintptr_t pause = so_symbol(&so_mod, SYM_PAUSE), restart = so_symbol(&so_mod, SYM_RESTART);
    uintptr_t status = so_symbol(&so_mod, SYM_STATUS);
    bpath = (const char **) so_symbol(&so_mod, SYM_BPATH);
    music_dir = (const char **) so_symbol(&so_mod, SYM_MUSIC_DIR);
    music_ext = (const char **) so_symbol(&so_mod, SYM_MUSIC_EXT);
    sound_elapsed = (sound_time_fn) so_symbol(&so_mod, SYM_ELAPSED);
    sound_length = (sound_time_fn) so_symbol(&so_mod, SYM_LENGTH);

    if (file_exists(OFF_SWITCH)) {
        l_info("ff7_music: %s found, music is read on the game thread", OFF_SWITCH);
        return;
    }
    if (!play || !stop || !pause || !restart || !status || !bpath || !music_dir || !music_ext
        || !sound_elapsed || !sound_length
        || *(uint32_t *) (play & ~1) != PLAY_PROLOGUE
        || STATE_PC + *(uint32_t *) (base + STATE_LITERAL) != STATE_OFFSET) {
        l_warn("ff7_music: unexpected .so layout, music is read on the game thread");
        return;
    }
    snd_state = (uintptr_t *) (base + STATE_OFFSET);

    sceKernelCreateLwMutex(&lock, "ff7_music", 0, 0, NULL);
    wake_sema = sceKernelCreateSema("ff7_music", 0, 0, 1000, NULL);
    SceUID thid = sceKernelCreateThread("music_loader", worker, 0x10000100, 0x4000, 0, CPU_MASK_SECONDARY, NULL);
    if (wake_sema < 0 || thid < 0 || sceKernelStartThread(thid, 0, NULL) < 0) {
        l_warn("ff7_music: could not start the loader thread, music is read on the game thread");
        return;
    }
    cpu_register_thread("music_loader", thid);

    play_hook = hook_addr(play, (uintptr_t) &play_hooked);
    stop_hook = hook_addr(stop, (uintptr_t) &stop_hooked);
    pause_hook = hook_addr(pause, (uintptr_t) &pause_hooked);
    restart_hook = hook_addr(restart, (uintptr_t) &restart_hooked);
    hook_addr(status, (uintptr_t) &status_hooked);
    enabled = 1;
    l_info("ff7_music: music files are read in the background (state at %p)", snd_state);
}
