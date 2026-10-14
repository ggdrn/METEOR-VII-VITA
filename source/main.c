/*
 * FINAL FANTASY VII (Android v1.0.29) loader for PS Vita.
 *
 * Drives libjni_ff7.so the same way MainActivity/RendererWrapper do on
 * Android (see PLANO.md for the decompiled call sequence).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/glstats.h"
#include "utils/profiler.h"
#include "utils/perf.h"
#include "ff7_texture.h"
#include "ff7_anim.h"
#include "ff7_lzss.h"
#include "ff7_swirl.h"
#include "ff7_light.h"
#include "ff7_bg.h"
#include "ff7_math.h"
#include "reimpl/gl_variants.h"
#include "reimpl/gl_queue.h"
#include "reimpl/obb_fd.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>

#include <psp2/apputil.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/system_param.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "reimpl/controls.h"
#include "utils/cpu.h"

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

// jp.co.d4e.materialg.GLESJniWrapper natives (static: JNIEnv*, jclass, ...)
static void (* ff7_setLang)(JNIEnv *, jclass, jint);
static void (* ff7_setDataPath)(JNIEnv *, jclass, jstring);
static void (* ff7_setAssetManager)(JNIEnv *, jclass, jobject);
static void (* ff7_onResume)(JNIEnv *, jclass);
static void (* ff7_onPause)(JNIEnv *, jclass);
static void (* ff7_onSurfaceChanged)(JNIEnv *, jclass, jint, jint);
static void (* ff7_onSurfaceCreated)(JNIEnv *, jclass, jboolean, jboolean);
static void (* ff7_onDrawFrame)(JNIEnv *, jclass);
static void (* ff7_onKey)(JNIEnv *, jclass, jint, jboolean);
static jint (* ff7_onKeyBack)(JNIEnv *, jclass);
static void (* ff7_onTouchBegan)(JNIEnv *, jclass, jint, jfloat, jfloat);
static void (* ff7_onTouchMoved)(JNIEnv *, jclass, jint, jfloat, jfloat);
static void (* ff7_onTouchEnded)(JNIEnv *, jclass, jint, jfloat, jfloat);

#define FF7_CLASS ((jclass) 0x42424242)

/*
 * Watchdog: logs when a single onDrawFrame() takes too long, so logs.txt can
 * tell a hang (or a long load) apart from the user quitting.
 */
static volatile SceUInt64 frame_start_us; // 0 while not inside onDrawFrame
static volatile unsigned int frame_number;

static int watchdog_thread(SceSize args, void * argp) {
    unsigned int reported_frame = 0;
    while (1) {
        sceKernelDelayThread(1000000);
        SceUInt64 start = frame_start_us;
        if (!start) continue;
        SceUInt64 stuck = sceKernelGetProcessTimeWide() - start;
        // Development aid: a hang this long gets a core dump (ux0:data/*.psp2dmp)
        // with every thread's registers, to find where the game is stuck
        if (stuck >= 15000000) {
            l_fatal("watchdog: onDrawFrame() #%u hung for %llu s, forcing a core dump",
                    frame_number, stuck / 1000000);
            *(volatile int *) 0xDEAD0000 = 0;
        }
        if (stuck >= 3000000 && reported_frame != frame_number) {
            reported_frame = frame_number;
            l_warn("watchdog: onDrawFrame() #%u running for %llu s", frame_number, stuck / 1000000);
        } else if (stuck >= 3000000 && (stuck / 1000000) % 10 == 0) {
            l_warn("watchdog: onDrawFrame() #%u still running (%llu s)", frame_number, stuck / 1000000);
        }
    }
    return 0;
}

static void log_memory(void) {
    struct mallinfo mi = mallinfo();
    l_info("memory: heap %u KB used, %u KB free (newlib arena %u KB)",
           mi.uordblks / 1024, mi.fordblks / 1024, mi.arena / 1024);
}

static void * ff7_symbol(const char * name) {
    char full[128];
    snprintf(full, sizeof(full), "Java_jp_co_d4e_materialg_GLESJniWrapper_%s", name);
    void * sym = (void *) so_symbol(&so_mod, full);
    if (!sym) l_error("Missing native: %s", full);
    return sym;
}

// MainActivity.onCreate(): en=0, fr=1, de=2, es=3, ja=4 (anything else: en)
static int ff7_lang_from_system() {
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    switch (lang) {
        case SCE_SYSTEM_PARAM_LANG_FRENCH:     return 1;
        case SCE_SYSTEM_PARAM_LANG_GERMAN:     return 2;
        case SCE_SYSTEM_PARAM_LANG_SPANISH:    return 3;
        case SCE_SYSTEM_PARAM_LANG_JAPANESE:   return 4;
        default:                               return 0;
    }
}

int main() {
    soloader_init_all();

    ff7_setLang          = ff7_symbol("setLang");
    ff7_setDataPath      = ff7_symbol("setDataPath");
    ff7_setAssetManager  = ff7_symbol("setAssetManager");
    ff7_onResume         = ff7_symbol("onResume");
    ff7_onPause          = ff7_symbol("onPause");
    ff7_onSurfaceChanged = ff7_symbol("onSurfaceChanged");
    ff7_onSurfaceCreated = ff7_symbol("onSurfaceCreated");
    ff7_onDrawFrame      = ff7_symbol("onDrawFrame");
    ff7_onKey            = ff7_symbol("onKey");
    ff7_onKeyBack        = ff7_symbol("onKeyBack");
    ff7_onTouchBegan     = ff7_symbol("onTouchBegan");
    ff7_onTouchMoved     = ff7_symbol("onTouchMoved");
    ff7_onTouchEnded     = ff7_symbol("onTouchEnded");

    int (* JNI_OnLoad)(void *jvm) = (void *)so_symbol(&so_mod, "JNI_OnLoad");
    l_info("JNI_OnLoad: %i", JNI_OnLoad(&jvm));

    // MainActivity.onCreate()
    int lang = ff7_lang_from_system();
    l_info("setLang(%i)", lang);
    ff7_setLang(&jni, FF7_CLASS, lang);

    l_info("setDataPath(\"%s\")", DOCUMENTS_PATH);
    ff7_setDataPath(&jni, FF7_CLASS, jni->NewStringUTF(&jni, DOCUMENTS_PATH));

    // MainActivity.onResume()
    l_info("setAssetManager / onResume");
    ff7_setAssetManager(&jni, FF7_CLASS, (jobject) 0x41414141);
    ff7_onResume(&jni, FF7_CLASS);

    // GLSurfaceView: RendererWrapper.onSurfaceChanged() -> onSurfaceCreated()
    gl_init();
    gl_queue_init();
    l_info("onSurfaceChanged(960, 544)");
    ff7_onSurfaceChanged(&jni, FF7_CLASS, 960, 544);
    perf_init();
    l_info("onSurfaceCreated(false, false)");
    ff7_onSurfaceCreated(&jni, FF7_CLASS, JNI_FALSE, JNI_FALSE);

    SceUID wd = sceKernelCreateThread("watchdog", watchdog_thread, 0x10000100, 0x4000, 0, CPU_MASK_SECONDARY, NULL);
    if (wd >= 0) sceKernelStartThread(wd, 0, NULL);
    if (wd >= 0) cpu_register_thread("watchdog", wd);

    // ux0:data/ffvii-vita/profile_on.txt: spike profiler without a special build
    int profiling = 0;
#if defined(ENABLE_PROFILER)
    profiler_start(NULL, 0);
    profiling = 1;
#elif defined(ENABLE_SPIKE_PROFILER)
    profiler_start(&frame_start_us, 100000);
    profiling = 1;
#else
    if (file_exists(DATA_PATH "profile_on.txt")) {
        profiler_start(&frame_start_us, 100000);
        profiling = 1;
    }
#endif

    log_memory();
    cpu_set_self(CPU_MASK_SECONDARY); // core 0 is the game thread's
    cpu_register_thread("main", sceKernelGetThreadId());
    l_success("Entering main loop.");
    unsigned int frame = 0;
    unsigned int heartbeat_frame = 0;
    SceUInt64 draw_us = 0, swap_us = 0, worst_us = 0;
    unsigned int slow50 = 0, slow100 = 0;
    SceUInt64 heartbeat_us = sceKernelGetProcessTimeWide();
    while (1) {
        controls_poll();

        // RendererWrapper.onDrawFrame() skips the first two frames
        if (++frame >= 3) {
            if (frame == 3) l_info("First onDrawFrame()");
            frame_number = frame;
            frame_start_us = sceKernelGetProcessTimeWide();
            ff7_onDrawFrame(&jni, FF7_CLASS);
            draw_us += sceKernelGetProcessTimeWide() - frame_start_us;
            frame_start_us = 0;
            if (frame == 3) l_info("First onDrawFrame() returned");
        }

        SceUInt64 swap_start = sceKernelGetProcessTimeWide();
        gl_swap();
        perf_frame_submitted();
        SceUInt64 swap_end = sceKernelGetProcessTimeWide();
        swap_us += swap_end - swap_start;
        if (swap_end - swap_start > 30000) {
            extern uint32_t vgl_swap_stage_us[3];
            l_warn("slow swap #%u: %llu ms (scene end %u ms, display queue %u ms, garbage collector wait %u ms)",
                   frame, (swap_end - swap_start) / 1000, vgl_swap_stage_us[0] / 1000,
                   vgl_swap_stage_us[1] / 1000, vgl_swap_stage_us[2] / 1000);
        }

        // Frame-to-frame time, to count stutters
        static SceUInt64 last_frame_end;
        if (last_frame_end) {
            SceUInt64 ft = swap_end - last_frame_end;
            if (ft > worst_us) worst_us = ft;
            if (ft > 250000) l_warn("slow frame #%u: %llu ms", frame, ft / 1000);
            if (ft > 100000) slow100++;
            else if (ft > 50000) slow50++;
        }
        last_frame_end = swap_end;

        // Heartbeat: tells a hang apart from a running game in logs.txt
        SceUInt64 now = sceKernelGetProcessTimeWide();
        if (now - heartbeat_us >= 5000000) {
            unsigned int frames = frame - heartbeat_frame;
            l_info("heartbeat: frame %u, %.1f fps, per frame: onDrawFrame %.1f ms, swap %.1f ms | "
                   "stutters: %u >50 ms, %u >100 ms, worst %.0f ms", frame,
                   frames * 1000000.0 / (double) (now - heartbeat_us),
                   frames ? draw_us / 1000.0 / frames : 0, frames ? swap_us / 1000.0 / frames : 0,
                   slow50, slow100, worst_us / 1000.0);
            slow50 = slow100 = 0;
            worst_us = 0;
            glstats_report((now - heartbeat_us) / 1000000.0);
            cpu_report((now - heartbeat_us) / 1000000.0);
            perf_report(frames * 1000000.0 / (double) (now - heartbeat_us), cpu_last_pct("game"));
            if (profiling) profiler_report();
            ff7_texture_report((now - heartbeat_us) / 1000000.0);
            ff7_anim_report((now - heartbeat_us) / 1000000.0);
            ff7_lzss_report((now - heartbeat_us) / 1000000.0);
            ff7_swirl_report((now - heartbeat_us) / 1000000.0);
            ff7_light_report((now - heartbeat_us) / 1000000.0);
            ff7_bg_report((now - heartbeat_us) / 1000000.0);
            ff7_math_report((now - heartbeat_us) / 1000000.0);
            gl_variants_report((now - heartbeat_us) / 1000000.0);
            gl_queue_report((now - heartbeat_us) / 1000000.0);
            obb_fd_report((now - heartbeat_us) / 1000000.0);
            draw_us = swap_us = 0;
            heartbeat_frame = frame;
            heartbeat_us = now;
            log_memory();
        }
    }

    sceKernelExitDeleteThread(0);
}

/*
 * Input. Button ids follow MainActivity.getKeyId():
 * 0 up, 1 left, 2 down, 3 right, 4 A, 5 B, 6 X, 7 Y,
 * 8 L1, 9 R1, 10 start, 11 select, 12 L2, 13 R2
 */

static int ff7_button_from_keycode(int32_t keycode) {
    switch (keycode) {
        case AKEYCODE_DPAD_UP:       return 0;
        case AKEYCODE_DPAD_LEFT:     return 1;
        case AKEYCODE_DPAD_DOWN:     return 2;
        case AKEYCODE_DPAD_RIGHT:    return 3;
        case AKEYCODE_BUTTON_A:      return 4;
        case AKEYCODE_BUTTON_B:      return 5;
        case AKEYCODE_BUTTON_X:      return 6;
        case AKEYCODE_BUTTON_Y:      return 7;
        case AKEYCODE_BUTTON_L1:     return 8;
        case AKEYCODE_BUTTON_R1:     return 9;
        case AKEYCODE_BUTTON_START:  return 10;
        case AKEYCODE_BUTTON_SELECT: return 11;
        case AKEYCODE_BUTTON_L2:     return 12;
        case AKEYCODE_BUTTON_R2:     return 13;
        default:                     return -1;
    }
}

void controls_handler_key(int32_t keycode, ControlsAction action) {
    int button = ff7_button_from_keycode(keycode);
    if (button < 0 || action == CONTROLS_ACTION_MOVE) return;
    ff7_onKey(&jni, FF7_CLASS, button, action == CONTROLS_ACTION_DOWN);
}

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    // MainActivity.MakeIdForFF7(): pointer id + 1
    switch (action) {
        case CONTROLS_ACTION_DOWN: ff7_onTouchBegan(&jni, FF7_CLASS, id + 1, x, y); break;
        case CONTROLS_ACTION_MOVE: ff7_onTouchMoved(&jni, FF7_CLASS, id + 1, x, y); break;
        case CONTROLS_ACTION_UP:   ff7_onTouchEnded(&jni, FF7_CLASS, id + 1, x, y); break;
    }
}

// Left stick doubles as the d-pad
#define STICK_THRESHOLD 0.5f

void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
    static bool held[4]; // up, left, down, right
    if (which != CONTROLS_STICK_LEFT) return;

    const bool now[4] = { y < -STICK_THRESHOLD, x < -STICK_THRESHOLD,
                          y > STICK_THRESHOLD,  x > STICK_THRESHOLD };
    for (int i = 0; i < 4; i++) {
        if (now[i] != held[i]) {
            held[i] = now[i];
            ff7_onKey(&jni, FF7_CLASS, i, now[i]);
        }
    }
}
