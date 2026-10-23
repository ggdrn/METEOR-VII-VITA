/*
 * DirectSound fixes for the streamed WAV music, and diagnostics.
 *
 * Four tracks always play as WAV files instead of the porters' .akb player:
 * SATO (15), WIND (65, the snowboard), SENSUI (88, the submarine) and HEART
 * (97) - 0x7416A3 classifies them. ff7.exe streams them itself: 0x7478BD
 * opens <name>.wav, and a multimedia timer (timeSetEvent, 80 ms, callback
 * 0x74656C, run by the porters' timeEvent_step from _check_event) calls
 * 0x748950 (stream) to refill a looping DirectSound buffer from the play
 * position (GetCurrentPosition) on.
 *
 * Fix: Play on a buffer that is already playing does nothing (DirectSound).
 * The snowboard sends the sound command "resume" every frame: resume_sound
 * (0x745995) calls Play on every buffer whose position isn't 0, the music
 * stream included (v0.6.18 log: Play on the stream and on an effect each
 * frame). The porters' fw_IDirectSoundBuffer_Play passes every call to
 * SL_startPlayer with the position it kept from the last GetCurrentPosition
 * (16 KB steps), so the music restarted from stale positions every frame:
 * stutter, and positions the refill could not follow. Now Play is passed on
 * only when the porters' GetStatus says the buffer isn't playing, or its
 * looping flag changes.
 *
 * v0.6.25: only looping buffers (the music stream) get this. A one-shot
 * effect that ended can still read as playing in the porters' state (it is
 * updated when its position is asked), so ignoring its next Play lost menu
 * and save-load sounds the first time they played again.
 *
 * Diagnostics (v0.6.18-19, at most 600 lines a session): buffers created,
 * Play/Stop that reach the porters' code, the stream's refills (first 20
 * calls, every end-flag change, the end) and a stream summary every 5 s.
 *
 * v0.6.20: the v0.6.19 log shows the stream alive (refilled continuously,
 * the file read in real time, looping) but nothing heard: the diagnostics
 * add SetVolume / SetPan / SetFrequency of the music buffer and the x86
 * music volumes ([0xDE6558] master 0-100, [0xDE6584] current 0-127: the
 * buffer volume is the table 0x967660 at their product / 100, -10000 = mute
 * at 0). DATA_PATH "wavmusic_fullvolume.txt" (test) plays the music buffer
 * at full volume whatever the game asks.
 *
 * v0.6.21: the v0.6.20 log shows the music buffer at full volume (SetVolume
 * 0, master 100, current 127) and never panned or retuned, still silent. The
 * diagnostics add the music buffer's Lock: the pointers handed to the x86
 * code, whether x86 memory at those addresses is the same memory, and the
 * peak level of the PCM in the buffer (is there sound in it at all).
 *
 * DATA_PATH "dsound_play_fix_off.txt" turns the fix off,
 * "sound_diag_off.txt" the logs.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_dsound.h"
#include "ff7_anim.h"
#include "ff7_unhook.h"

#include "utils/logger.h"
#include "utils/utils.h"

#include <stdint.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define FIX_OFF     DATA_PATH "dsound_play_fix_off.txt"
#define DIAG_OFF    DATA_PATH "sound_diag_off.txt"
#define SYM_REFILL  "_Z15func_0x00748950v"
#define SYM_CREATE  "_Z33fw_IDirectSound_CreateSoundBufferjjjj"
#define SYM_PLAY    "_Z26fw_IDirectSoundBuffer_Playjjjj"
#define SYM_STOP    "_Z26fw_IDirectSoundBuffer_Stopj"
#define SYM_STATUS  "_Z31fw_IDirectSoundBuffer_GetStatusjj"
#define SYM_VOLUME  "_Z31fw_IDirectSoundBuffer_SetVolumeji"
#define SYM_PAN     "_Z28fw_IDirectSoundBuffer_SetPanji"
#define SYM_FREQ    "_Z34fw_IDirectSoundBuffer_SetFrequencyjj"
#define SYM_LOCK    "_Z26fw_IDirectSoundBuffer_Lockjjjjjjjj"
#define FULL_VOLUME DATA_PATH "wavmusic_fullvolume.txt"
#define VA_STREAM   0xde62d8u // the music stream; +0 its DirectSound buffer
#define VA_MASTER   0xde6558u
#define VA_MUSIC    0xde6584u
#define MAX_LOGS    600
#define MAX_BUFS    64

typedef uint8_t * (* mk_address_fn)(uint32_t va);
typedef int (* status_fn)(uint32_t buf, uint32_t status_va);

static mk_address_fn mk_address;
static status_fn get_status;
static volatile uint32_t * regs;
static so_hook refill_hook, create_hook, play_hook, stop_hook, volume_hook, pan_hook, freq_hook, lock_hook;
// for the level check: music_va, music_locks (declared above)
static uint32_t music_size;
static unsigned lock_logs;
static int fix_on, diag_on, full_volume;
static int logs_left = MAX_LOGS;

// Looping flag of the last Play passed on, per buffer
static struct { uint32_t buf, flags; } bufs[MAX_BUFS];

static struct {
    uint32_t stream, calls, last_flags;
    unsigned refills, skipped_plays, plays;
    SceUInt64 next_summary;
} st = { .last_flags = 0xffffffffu };

static uint32_t rd32(uint32_t va) {
    uint8_t * p = va ? mk_address(va) : NULL;
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

static uint16_t rd16(uint32_t va) {
    uint8_t * p = va ? mk_address(va) : NULL;
    uint16_t v = 0;
    if (p) memcpy(&v, p, 2);
    return v;
}

static double now_s(void) {
    return sceKernelGetProcessTimeWide() / 1000000.0;
}

static int is_music(uint32_t buf);
static int music_peak(void);
static unsigned music_locks;
static uint32_t music_va; // x86 address of the music buffer's data (from its Lock)

#define DIAG(...) do { if (diag_on && logs_left > 0) { logs_left--; l_info(__VA_ARGS__); } } while (0)

static int buf_slot(uint32_t buf) {
    int free_slot = -1;
    for (int i = 0; i < MAX_BUFS; i++) {
        if (bufs[i].buf == buf) return i;
        if (!bufs[i].buf && free_slot < 0) free_slot = i;
    }
    return free_slot;
}

static void log_stream(const char * what, uint32_t stream, uint32_t r) {
    uint32_t info = rd32(stream + 0x4);
    DIAG("sound diag %.3f: %s %08x call %u -> %u | mode %u playing %u +18 %u read-end %u loop %u end %u |"
         " size %u play %u write %u, left %d, +20 %u, chunk %u | loop %u..%u, %u Hz %u ch %u bit",
         now_s(), what, stream, st.calls, r, rd32(stream + 0x8), rd32(stream + 0x14), rd32(stream + 0x18),
         rd32(stream + 0x1c), rd32(stream + 0x30), rd32(stream + 0x34), rd32(stream + 0xc), rd32(stream + 0x2c),
         rd32(stream + 0x28), (int32_t) rd32(stream + 0x24), rd32(stream + 0x20), rd32(stream + 0x10),
         rd32(info + 0x10), rd32(info + 0x14), rd32(info + 0x18) ? rd32(rd32(info + 0x18) + 4) : 0,
         rd32(info + 0x18) ? rd16(rd32(info + 0x18) + 2) : 0, rd32(info + 0x18) ? rd16(rd32(info + 0x18) + 14) : 0);
}

static void refill_native(void) {
    uint32_t stream = rd32(regs[4] + 4);
    if (stream != st.stream) {
        st.stream = stream;
        st.calls = 0;
        st.last_flags = 0xffffffffu;
    }
    SO_CONTINUE_HELD(int, refill_hook);
    uint32_t r = regs[0];
    st.calls++;
    st.refills++;
    uint32_t flags = rd32(stream + 0x14) | rd32(stream + 0x18) << 4 | rd32(stream + 0x1c) << 8
                   | rd32(stream + 0x30) << 12 | rd32(stream + 0x34) << 16;
    if (st.calls <= 20 || r || flags != st.last_flags) log_stream(r ? "refill END" : "refill", stream, r);
    st.last_flags = flags;
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (now >= st.next_summary) {
        st.next_summary = now + 5000000;
        DIAG("sound diag %.3f: last 5 s: %u refills, %u Play passed on, %u repeated Play skipped; music buffer"
             " %08x, x86 music volume master %u current %u target %u; %u Locks, PCM peak %d of 32767 (data %p)",
             now_s(), st.refills, st.plays, st.skipped_plays, rd32(VA_STREAM), rd32(VA_MASTER), rd32(VA_MUSIC),
             rd32(VA_MUSIC + 4), music_locks, music_peak(), (void *) (uintptr_t) music_va);
        music_locks = 0;
        log_stream("stream now", stream, r);
        st.refills = st.plays = st.skipped_plays = 0;
    }
}

static int create_native(uint32_t ds, uint32_t desc, uint32_t out, uint32_t unk) {
    int r = SO_CONTINUE_HELD(int, create_hook, ds, desc, out, unk);
    uint32_t fmt = rd32(desc + 0x10);
    DIAG("sound diag %.3f: CreateSoundBuffer -> %d, buffer %08x: flags %08x, %u bytes, format %u, %u ch, %u Hz,"
         " %u bit", now_s(), r, rd32(out), rd32(desc + 4), rd32(desc + 8), rd16(fmt), rd16(fmt + 2),
         rd32(fmt + 4), rd16(fmt + 14));
    return r;
}

static int play_native(uint32_t buf, uint32_t a, uint32_t b, uint32_t flags) {
    int slot = buf ? buf_slot(buf) : -1;
    if (fix_on && (flags & 1) && slot >= 0 && bufs[slot].buf == buf && bufs[slot].flags == 1) {
        // the porters' status: playing (1) or not (0); scratch status word on
        // the x86 stack below the caller's frame (dead)
        uint32_t status_va = regs[4] - 0x200;
        get_status(buf, status_va);
        if (rd32(status_va) & 1) {
            st.skipped_plays++;
            return 0; // DS_OK: already playing
        }
    }
    int r = SO_CONTINUE_HELD(int, play_hook, buf, a, b, flags);
    if (slot >= 0) {
        bufs[slot].buf = buf;
        bufs[slot].flags = flags & 1;
    }
    st.plays++;
    static unsigned plays_logged;
    if (is_music(buf) || plays_logged < 20) { // effects replay all the time: only the first ones
        plays_logged++;
        DIAG("sound diag %.3f: Play %08x%s flags %u -> %d", now_s(), buf, is_music(buf) ? " (music)" : "", flags, r);
    }
    return r;
}

static int stop_native(uint32_t buf) {
    int r = SO_CONTINUE_HELD(int, stop_hook, buf);
    int slot = buf ? buf_slot(buf) : -1;
    if (slot >= 0 && bufs[slot].buf == buf) bufs[slot].buf = 0; // the next Play goes through
    DIAG("sound diag %.3f: Stop %08x -> %d", now_s(), buf, r);
    return r;
}

static int is_music(uint32_t buf) {
    return buf && buf == rd32(VA_STREAM);
}

// Lock (buf, offset, bytes, &p1, &n1, &p2, &n2, flags): the porters' code
// writes x86 addresses into the x86 out-parameters (the buffers live in x86
// memory). v0.6.21 read them as host pointers and crashed.
static int lock_native(uint32_t buf, uint32_t off, uint32_t bytes, uint32_t p1_va, uint32_t n1_va, uint32_t p2_va,
                       uint32_t n2_va, uint32_t flags) {
    int r = SO_CONTINUE_HELD(int, lock_hook, buf, off, bytes, p1_va, n1_va, p2_va, n2_va, flags);
    if (is_music(buf)) {
        music_locks++;
        uint32_t p1 = rd32(p1_va), n1 = rd32(n1_va), p2 = p2_va ? rd32(p2_va) : 0, n2 = n2_va ? rd32(n2_va) : 0;
        if (p1 && p1 >= off) {
            music_va = p1 - off;
            music_size = rd32(VA_STREAM + 0xc);
        }
        if (lock_logs < 12) {
            lock_logs++;
            DIAG("sound diag %.3f: music Lock %08x offset %u bytes %u flags %u -> %d: p1 %08x n1 %u p2 %08x n2 %u"
                 " (x86 addresses; p1 at host %p)", now_s(), buf, off, bytes, flags, r, p1, n1, p2, n2,
                 p1 ? (void *) mk_address(p1) : NULL);
        }
    }
    return r;
}

// Peak |sample| of the music buffer (16-bit PCM, read through mk_address), 0 = silence
static int music_peak(void) {
    if (!music_va || !music_size || music_size > 0x100000) return -1;
    int peak = 0;
    for (uint32_t off = 0; off + 2 <= music_size; off += 14) {
        int v = (int16_t) rd16(music_va + off);
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    return peak;
}

// The music buffer's volume, pan and frequency: every change is logged
static int last_set[3] = { 0x7fffffff, 0x7fffffff, 0x7fffffff };


static void log_set(int which, uint32_t buf, int value, int passed, int r) {
    static const char * const names[3] = { "SetVolume", "SetPan", "SetFrequency" };
    if (is_music(buf)) {
        if (value != last_set[which])
            DIAG("sound diag %.3f: music %s %08x %d%s -> %d", now_s(), names[which], buf, value,
                 passed != value ? " (full volume test: 0)" : "", r);
        last_set[which] = value;
    }
}

static int volume_native(uint32_t buf, int vol) {
    int v = full_volume && is_music(buf) ? 0 : vol;
    int r = SO_CONTINUE_HELD(int, volume_hook, buf, v);
    log_set(0, buf, vol, v, r);
    return r;
}

static int pan_native(uint32_t buf, int pan) {
    int r = SO_CONTINUE_HELD(int, pan_hook, buf, pan);
    log_set(1, buf, pan, pan, r);
    return r;
}

static int freq_native(uint32_t buf, uint32_t freq) {
    int r = SO_CONTINUE_HELD(int, freq_hook, buf, freq);
    log_set(2, buf, (int) freq, (int) freq, r);
    return r;
}

void ff7_dsound_patch(void) {
    fix_on = !file_exists(FIX_OFF);
    diag_on = !file_exists(DIAG_OFF);
    if (!fix_on && !diag_on) {
        l_info("ff7_dsound: disabled by %s and %s", FIX_OFF, DIAG_OFF);
        return;
    }
    mk_address = (mk_address_fn) so_symbol(&so_mod, "mk_address");
    get_status = (status_fn) so_symbol(&so_mod, SYM_STATUS);
    regs = ff7_x86_regs();
    uintptr_t refill = so_symbol(&so_mod, SYM_REFILL), create = so_symbol(&so_mod, SYM_CREATE);
    uintptr_t play = so_symbol(&so_mod, SYM_PLAY), stop = so_symbol(&so_mod, SYM_STOP);
    if (!mk_address || !get_status || !regs || !refill || !create || !play || !stop) {
        l_warn("ff7_dsound: symbols not found, off");
        return;
    }
    play_hook = hook_addr(play, (uintptr_t) &play_native);
    stop_hook = hook_addr(stop, (uintptr_t) &stop_native);
    full_volume = file_exists(FULL_VOLUME);
    if (diag_on || full_volume) {
        uintptr_t vol = so_symbol(&so_mod, SYM_VOLUME), pan = so_symbol(&so_mod, SYM_PAN);
        uintptr_t freq = so_symbol(&so_mod, SYM_FREQ);
        if (vol) volume_hook = hook_addr(vol, (uintptr_t) &volume_native);
        if (pan && diag_on) pan_hook = hook_addr(pan, (uintptr_t) &pan_native);
        if (freq && diag_on) freq_hook = hook_addr(freq, (uintptr_t) &freq_native);
    }
    if (diag_on) {
        uintptr_t lock = so_symbol(&so_mod, SYM_LOCK);
        if (lock) lock_hook = hook_addr(lock, (uintptr_t) &lock_native);
        refill_hook = hook_addr(refill, (uintptr_t) &refill_native);
        create_hook = hook_addr(create, (uintptr_t) &create_native);
    }
    l_info("ff7_dsound: repeated Play %s, diagnostics %s%s", fix_on ? "ignored while playing (fix on)" : "passed on",
           diag_on ? "on" : "off", full_volume ? ", WAV music at full volume (test)" : "");
}
