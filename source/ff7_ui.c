/*
 * Touch UI adjustments for libjni_ff7.so.
 *
 * The Android version draws a virtual gamepad (UIObject_DPad and
 * UIObject_ButtonMapped, from button_xx.png) over the game. On the Vita the
 * physical buttons already drive the game, so those two classes are neither
 * drawn nor allowed to take touches. Every other UI object (toggle buttons
 * such as the boosters, triggered buttons, swipers, the screen itself) keeps
 * working with the front touch screen.
 *
 * The first touches are also logged together with the objects of the current
 * UI layout, to identify each element.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_ui.h"

#include "utils/logger.h"

#include <stdint.h>

#include <kubridge.h>
#include <psp2/kernel/clib.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define TOUCH_LOG_LIMIT 40

static const struct {
    const char * vtable;
    const char * name;
} classes[] = {
    { "_ZTV8UIObject", "UIObject" },
    { "_ZTV13UIObject_DPad", "DPad" },
    { "_ZTV14UIObject_Image", "Image" },
    { "_ZTV15UIObject_Screen", "Screen" },
    { "_ZTV15UIObject_Swiper", "Swiper" },
    { "_ZTV16UIObject_DebugUI", "DebugUI" },
    { "_ZTV16UIObject_Trigger", "Trigger" },
    { "_ZTV18UIObject_Copyright", "Copyright" },
    { "_ZTV21UIObject_ButtonMapped", "ButtonMapped" },
    { "_ZTV21UIObject_ToggleButton", "ToggleButton" },
    { "_ZTV24UIObject_ButtonTriggered", "ButtonTriggered" },
    { "_ZTV25UIObject_ButtonTriggered2", "ButtonTriggered2" },
};
#define NUM_CLASSES (sizeof(classes) / sizeof(classes[0]))

static uintptr_t vptrs[NUM_CLASSES];
static void *** ui_current;     // &UI_current (UIObject **)
static so_hook touch_began_hook;
static int touches_logged;
static void * last_logged_layout;

static const char * class_name(void * obj) {
    uintptr_t vptr = *(uintptr_t *) obj;
    for (unsigned i = 0; i < NUM_CLASSES; i++)
        if (vptrs[i] && vptrs[i] == vptr) return classes[i].name;
    return "?";
}

static void log_layout(void ** list) {
    l_info("ui: layout %p objects:", list);
    for (int i = 0; list && list[i] && i < 32; i++) {
        uint8_t * o = list[i];
        float * r = (float *) (o + 8);
        l_info("ui:   [%2d] %-16s rect (%.0f,%.0f %.0fx%.0f) flag4 %u touch %d",
               i, class_name(o), r[0], r[1], r[2], r[3], o[4], *(int32_t *) (o + 24));
    }
}

// UI_onTouchBegan(int id, float x, float y), x/y in surface pixels
static void ui_touch_began(int id, float x, float y) {
    if (touches_logged < TOUCH_LOG_LIMIT) {
        touches_logged++;
        void ** list = ui_current ? *ui_current : NULL;
        l_info("ui: touch began id %d at (%.0f, %.0f), layout %p", id, x, y, list);
        if (list != last_logged_layout) {
            last_logged_layout = list;
            log_layout(list);
        }
    }
    SO_CONTINUE(int, touch_began_hook, id, x, y);
}

// Virtual gamepad: not drawn, never takes a touch
static void hidden_render(void * self) {
}

static int hidden_touch(void * self, float x, float y) {
    return 0;
}

static void hook_symbol(const char * symbol, void * dst) {
    uintptr_t addr = so_symbol(&so_mod, symbol);
    if (!addr) {
        l_warn("ff7_ui: %s not found", symbol);
        return;
    }
    hook_addr(addr, (uintptr_t) dst);
}

void ff7_ui_patch(void) {
    for (unsigned i = 0; i < NUM_CLASSES; i++) {
        uintptr_t vt = so_symbol(&so_mod, classes[i].vtable);
        vptrs[i] = vt ? vt + 8 : 0; // objects point past offset-to-top and RTTI
    }
    ui_current = (void ***) so_symbol(&so_mod, "UI_current");

    uintptr_t began = so_symbol(&so_mod, "UI_onTouchBegan");
    if (began) touch_began_hook = hook_addr(began, (uintptr_t) &ui_touch_began);

    hook_symbol("_ZN13UIObject_DPad6renderEv", hidden_render);
    hook_symbol("_ZN13UIObject_DPad12onTouchBeganE8CGPoint_", hidden_touch);
    hook_symbol("_ZN13UIObject_DPad12onTouchMovedE8CGPoint_", hidden_touch);
    hook_symbol("_ZN13UIObject_DPad12onTouchEndedE8CGPoint_", hidden_touch);
    hook_symbol("_ZN21UIObject_ButtonMapped6renderEv", hidden_render);
    hook_symbol("_ZN21UIObject_ButtonMapped12onTouchBeganE8CGPoint_", hidden_touch);
    hook_symbol("_ZN21UIObject_ButtonMapped12onTouchMovedE8CGPoint_", hidden_touch);
    hook_symbol("_ZN21UIObject_ButtonMapped12onTouchEndedE8CGPoint_", hidden_touch);

    l_info("ff7_ui: virtual gamepad hidden, touch logging enabled (UI_current at %p)", ui_current);
}
