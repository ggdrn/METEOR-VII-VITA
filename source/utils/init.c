/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/settings.h"
#include "utils/zip.h"

#include <reimpl/controls.h>

#include <stdlib.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

// Base address for the Android .so to be loaded at
#define LOAD_ADDRESS 0x98000000

extern so_module so_mod;

// Original APK, kept open for the AAssetManager reimplementation
zip_archive * apk_zip = NULL;
// Original OBB, served to the game through ExpansionFile (java.c)
zip_archive * obb_zip = NULL;

void soloader_init_all() {
    l_info("FINAL FANTASY VII (PS Vita) %s", PORT_VERSION);
    l_info("Data: %s", DATA_PATH);
	// Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Set default overclock values
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    // Game data is never bundled: it comes from the user's own Play Store copy
    if (!file_exists(APK_PATH) || !file_exists(OBB_PATH)) {
        fatal_error("Game data not found.\n\nCopy your own FINAL FANTASY VII "
                    "(Android v1.0.29) files to:\n%s\n%s",
                    APK_PATH, OBB_PATH);
    }
    sceIoMkdir(DOCUMENTS_PATH, 0777);

    apk_zip = zip_open(APK_PATH);
    zip_entry * so_entry = apk_zip ? zip_find(apk_zip, SO_APK_ENTRY) : NULL;
    if (!so_entry) {
        l_fatal("%s not found in %s.", SO_APK_ENTRY, APK_PATH);
        fatal_error("Error: %s is not a valid FINAL FANTASY VII APK.", APK_PATH);
    }

    size_t so_size;
    void * so_data = zip_read_entry(apk_zip, so_entry, &so_size);
    if (!so_data || so_mem_load(&so_mod, so_data, so_size, LOAD_ADDRESS) < 0) {
        l_fatal("SO could not be loaded.");
        fatal_error("Error: could not load %s from %s.", SO_APK_ENTRY, APK_PATH);
    }
    free(so_data);
    l_success("SO loaded from APK (%u bytes).", so_size);

    obb_zip = zip_open(OBB_PATH);
    if (!obb_zip) {
        fatal_error("Error: %s is not a valid FINAL FANTASY VII OBB file.", OBB_PATH);
    }

    settings_load();
    l_success("Settings loaded.");

    so_relocate(&so_mod);
    l_success("SO relocated.");

    resolve_imports(&so_mod);
    l_success("SO imports resolved.");

    so_patch();
    l_success("SO patched.");

    so_flush_caches(&so_mod);
    l_success("SO caches flushed.");

    so_initialize(&so_mod);
    l_success("SO initialized.");

    gl_preload();
    l_success("OpenGL preloaded.");

    jni_init();
    l_success("FalsoJNI initialized.");

#ifndef NDK_PORT
    controls_init();
    l_success("Controls initialized.");
#endif
}
