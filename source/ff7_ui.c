/*
 * Touch UI adjustments for libjni_ff7.so.
 *
 * The Android version draws a virtual gamepad over the game: a d-pad
 * (UIObject_DPad) and buttons mapped to keys (UIObject_ButtonMapped, key code
 * at +40). On the Vita the physical buttons already do that, so those objects
 * are removed from every UI layout the first time it is displayed. Only mapped
 * buttons with special actions stay: 0x82 and 0x85, cases
 * UIObject_ButtonMapped::onTouchBegan handles itself. Everything else is a
 * plain key (0x9C Enter/OK, 0x52 cancel, ...) or a web link (0x80/0x81).
 * 0x83 is the floating ESCAPE button of battles: it holds PageUp + PageDown,
 * the PC's L1 + R1, so it goes too (holding L + R on the Vita escapes).
 * Toggle buttons (Encounters ON/OFF), triggered buttons, swipers and the
 * screen itself are untouched.
 *
 * The first touches are logged with the objects of the current layout.
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
#define MAX_LAYOUT 32
#define MAX_FILTERED 128

enum { CLS_OTHER = -1, CLS_DPAD = 1, CLS_BUTTON_MAPPED = 8, CLS_TOGGLE = 9 };

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
static so_hook touch_began_hook, display_hook;
static int touches_logged;

typedef void (* touch_fn)(int id, float x, float y);
typedef int (* contains_fn)(void * self, float x, float y);

// SO_CONTINUE calls through an unprototyped pointer, which promotes float
// arguments to double: call the original with the real prototype instead.
static void call_original_touch_began(int id, float x, float y) {
    so_hook * h = &touch_began_hook;
    sceClibMemcpy((void *) h->addr, h->orig_instr, sizeof(h->orig_instr));
    kuKernelFlushCaches((void *) h->addr, sizeof(h->orig_instr));
    ((touch_fn) (h->thumb_addr ? h->thumb_addr : h->addr))(id, x, y);
    sceClibMemcpy((void *) h->addr, h->patch_instr, sizeof(h->patch_instr));
    kuKernelFlushCaches((void *) h->addr, sizeof(h->patch_instr));
}
static void * last_logged_layout;
static void ** filtered[MAX_FILTERED];
static int num_filtered;

static int class_of(void * obj) {
    uintptr_t vptr = *(uintptr_t *) obj;
    for (unsigned i = 0; i < NUM_CLASSES; i++)
        if (vptrs[i] && vptrs[i] == vptr) return (int) i;
    return CLS_OTHER;
}

static const char * class_name(void * obj) {
    int c = class_of(obj);
    return c >= 0 ? classes[c].name : "?";
}

static int is_virtual_gamepad(void * obj) {
    int c = class_of(obj);
    if (c == CLS_DPAD) return 1;
    if (c == CLS_BUTTON_MAPPED) {
        uint32_t key = *(uint32_t *) ((uint8_t *) obj + 40);
        return !(key == 0x82 || key == 0x85); // special actions; ESCAPE (0x83) = L + R
    }
    return 0;
}

static void log_layout(void ** list) {
    l_info("ui: layout %p objects:", list);
    for (int i = 0; list && list[i] && i < MAX_LAYOUT; i++) {
        uint8_t * o = list[i];
        float * r = (float *) (o + 8);
        int c = class_of(o);
        if (c == CLS_BUTTON_MAPPED)
            l_info("ui:   [%2d] %-16s rect (%.0f,%.0f %.0fx%.0f) flag4 %u touch %d key 0x%02x",
                   i, class_name(o), r[0], r[1], r[2], r[3], o[4], *(int32_t *) (o + 24),
                   *(uint32_t *) (o + 40));
        else
            l_info("ui:   [%2d] %-16s rect (%.0f,%.0f %.0fx%.0f) flag4 %u touch %d",
                   i, class_name(o), r[0], r[1], r[2], r[3], o[4], *(int32_t *) (o + 24));
    }
}

// Removes the virtual gamepad from a layout (in place, once per layout)
static void filter_layout(void ** list) {
    if (!list) return;
    for (int i = 0; i < num_filtered; i++)
        if (filtered[i] == list) return;
    if (num_filtered < MAX_FILTERED) filtered[num_filtered++] = list;

    int out = 0, removed = 0;
    for (int i = 0; list[i] && i < MAX_LAYOUT; i++) {
        if (is_virtual_gamepad(list[i])) { removed++; continue; }
        list[out++] = list[i];
    }
    if (removed) {
        for (int i = out; i < out + removed; i++) list[i] = NULL;
        l_info("ui: layout %p: removed %d virtual gamepad objects, %d kept", list, removed, out);
    }
}

// UI_display(UIObject ** list): draws a layout every frame
static void ui_display(void ** list) {
    filter_layout(list);
    SO_CONTINUE(int, display_hook, list);
}

// UI_onTouchBegan's conversion to the 640x480 UI space (non-rotated surface)
static void to_ui(float x, float y, float * ux, float * uy) {
    extern so_module so_mod;
    static float * dev_w, * dev_h;
    if (!dev_w) {
        dev_w = (float *) so_symbol(&so_mod, "DEVICE_W");
        dev_h = (float *) so_symbol(&so_mod, "DEVICE_H");
    }
    float u = *dev_h / 480.f;
    *ux = (x - (*dev_w - 640.f * u) * 0.5f) / u;
    *uy = y / u;
}

// ToggleButton: +0x40 -> value byte toggled by a tap, +0x44 pressed flag
static void log_toggles(void ** list, const char * when) {
    for (int i = 0; list && list[i] && i < MAX_LAYOUT; i++) {
        uint8_t * o = list[i];
        if (class_of(o) != CLS_TOGGLE) continue;
        uint8_t * value = *(uint8_t **) (o + 0x40);
        l_info("ui:   %s toggle [%d]: pressed %u, value %d (at %p), touch %d",
               when, i, o[0x44], value ? *value : -1, value, *(int32_t *) (o + 24));
    }
}

// UI_onTouchBegan(int id, float x, float y), x/y in surface pixels
static void ui_touch_began(int id, float x, float y) {
    void ** list = ui_current ? *ui_current : NULL;
    filter_layout(list);
    int log = touches_logged < TOUCH_LOG_LIMIT;
    if (log) {
        touches_logged++;
        l_info("ui: touch began id %d at (%.0f, %.0f), layout %p", id, x, y, list);
        if (list != last_logged_layout) {
            last_logged_layout = list;
            log_layout(list);
        }
        float ux, uy;
        to_ui(x, y, &ux, &uy);
        for (int i = 0; list && list[i] && i < MAX_LAYOUT; i++) {
            contains_fn contains = (contains_fn) (*(uintptr_t **) list[i])[0];
            l_info("ui:   ui point (%.0f, %.0f): [%d] %s contains %d", ux, uy, i, class_name(list[i]),
                   contains(list[i], ux, uy));
        }
        log_toggles(list, "before");
    }
    call_original_touch_began(id, x, y);
    if (log) log_toggles(list, "after ");
}

void ff7_ui_patch(void) {
    for (unsigned i = 0; i < NUM_CLASSES; i++) {
        uintptr_t vt = so_symbol(&so_mod, classes[i].vtable);
        vptrs[i] = vt ? vt + 8 : 0; // objects point past offset-to-top and RTTI
    }
    ui_current = (void ***) so_symbol(&so_mod, "UI_current");

    uintptr_t began = so_symbol(&so_mod, "UI_onTouchBegan");
    uintptr_t display = so_symbol(&so_mod, "_Z10UI_displayPP8UIObject");
    if (!began || !display) {
        l_warn("ff7_ui: UI symbols not found, touch UI unchanged");
        return;
    }
    touch_began_hook = hook_addr(began, (uintptr_t) &ui_touch_began);
    display_hook = hook_addr(display, (uintptr_t) &ui_display);
    l_info("ff7_ui: virtual gamepad filtered from layouts, touch logging on (UI_current at %p)", ui_current);
}
