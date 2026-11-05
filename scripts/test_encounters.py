#!/usr/bin/env python3
"""Exercise real UI filtering and settings I/O with synthetic Android layouts."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
ui = re.sub(r'^#include .*$', '', (root / 'source/ff7_ui.c').read_text(), flags=re.M)
menu = (root / 'source/port_menu.c').read_text()
settings = menu[menu.index('static int clamp_factor('):menu.index('// Where the player is')]
stubs = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define l_info(...) ((void)0)
#define l_warn(...) ((void)0)
#define l_error(...) ((void)0)
#define SO_CONTINUE_HELD(type, h, ...) fake_original(&(h))
#define sceClibMemcpy memcpy
typedef struct { uintptr_t addr, thumb_addr; unsigned char orig_instr[8], patch_instr[8]; } so_hook;
typedef struct { uintptr_t text_base; } so_module;
so_module so_mod;
static void kuKernelFlushCaches(void *p, unsigned n) {}
static void profiler_hold(void) {}
static void profiler_release(void) {}
static uintptr_t so_symbol(so_module *m, const char *s) { return 0; }
static so_hook hook_addr(uintptr_t a, uintptr_t b) { return (so_hook){0}; }
static void fake_original(so_hook *h);
static volatile uint32_t * ff7_x86_regs(void) { return NULL; }
void port_menu_save_settings(void);
#define CHEATS_PATH "cheats.txt"
#define NUM_LANGUAGES 5
static int settings_loaded, cheat_hpmp, cheat_exp=1, cheat_gil=1, materia_max=1;
static int speed_field=1, speed_world=1, auto_skip, widescreen, language=-1;
static int encounters_saved=1, encounters_button_saved=1;
'''
tests = r'''
static int expected_field_rate, original_calls;
static void fake_original(so_hook *h) {
    original_calls++;
    if (h == &field_encounter_hook) {
        assert(encounter_memmap[0xdbc][0xad9] == expected_field_rate);
        // Adjacent preemptive and battle-type probabilities must stay intact.
        assert(encounter_memmap[0xdbc][0xad8] == 16);
        assert(encounter_memmap[0xdbc][0xada] == 7);
    } else if (h == &world_rate_hook) {
        unsigned rate = encounter_memmap[0xdbc][0xad9];
        encounter_regs[0] = rate * (rate > 16 ? 2 : 1);
    }
}
typedef union { uintptr_t align; uint8_t bytes[96]; } object;
static void init(object *o, int cls, uint8_t *flag) {
    memset(o, 0, sizeof(*o));
    *(uintptr_t *)o = vptrs[cls];
    if (cls == CLS_TOGGLE) *(uint8_t **)(o->bytes + 0x40) = flag;
}
static int count(void **list, void *obj) {
    int n=0;
    for (int i=0; list[i]; i++) n += list[i] == obj;
    return n;
}
static void native_touch(int id, float x, float y) {
    for (int i=0; (*ui_current)[i]; i++) {
        uint8_t *o=(*ui_current)[i];
        // The Android object stores a 32-bit pointer at +0x40 next to +0x44.
        // Use the fixture flag directly on a 64-bit host after touch cleanup.
        if (class_of(o)==CLS_TOGGLE) *no_encounters_flag ^= 1;
    }
}
int main(void) {
    uint8_t native=0, dpad_mode=0, other_flag=0;
    assert(ff7_ui_encounters_get() == -1);
    ff7_ui_encounters_set(0); // unsupported binary: safe no-op
    no_encounters_flag=&native; touch_mode_flag=&dpad_mode;
    ff7_ui_encounters_set(2); // only ON/OFF if rate hooks are unavailable
    assert(ff7_ui_encounters_get()==1);
    encounter_rate_ready=1;
    static uint8_t page[4096];
    static uint8_t *pages[0xdbd];
    static uint32_t regs[8];
    pages[0xdbc]=page; encounter_memmap=pages; encounter_regs=regs;
    page[0xad8]=16; page[0xada]=7;
    const int rates[]={0,1,2,4};
    const int base_rates[]={0,1,8,16,24,32,64,128,255};
    for (unsigned b=0;b<sizeof(base_rates)/sizeof(base_rates[0]);b++) {
        unsigned base=base_rates[b];
        page[0xad9]=base;
        for (unsigned i=0;i<4;i++) {
            int rate=rates[i];
            ff7_ui_encounters_set(rate);
            assert(ff7_ui_encounters_get()==rate);
            unsigned scaled=base*rate;
            expected_field_rate=scaled>255?255:scaled;
            for (int repeat=0;repeat<3;repeat++) {
                field_encounter();
                assert(page[0xad9]==base); // no accumulation or save contamination
                world_encounter_rate();
                assert(regs[0]==base*(base>16?2:1)*rate);
                assert(page[0xad9]==base);
            }
        }
    }
    assert(original_calls==216);
    ff7_ui_encounters_set(2);
    ff7_ui_encounters_set(3); ff7_ui_encounters_set(-1); ff7_ui_encounters_set(100);
    assert(ff7_ui_encounters_get()==2);
    ff7_ui_encounters_set(1);
    vptrs[CLS_TOGGLE]=9; vptrs[CLS_DPAD]=1; vptrs[CLS_BUTTON_MAPPED]=8;
    vptrs[2]=2;
    object encounter, mode, other, dpad, screen, l2;
    init(&encounter, CLS_TOGGLE, &native); init(&mode, CLS_TOGGLE, &dpad_mode);
    init(&other, CLS_TOGGLE, &other_flag); init(&dpad, CLS_DPAD, NULL);
    init(&screen, 2, NULL); init(&l2, CLS_BUTTON_MAPPED, NULL);
    *(uint32_t *)(l2.bytes+40)=KEY_L2;
    void *layout[MAX_LAYOUT]={&encounter, &mode, &dpad, &screen, &other, &l2, NULL};
    int f=filter_layout(layout);
    assert(f>=0 && count(layout,&encounter)==1 && count(layout,&other)==1);
    assert(!count(layout,&mode) && !count(layout,&dpad) && !count(layout,&l2));
    assert(ff7_ui_encounters_get()==1 && ff7_ui_encounters_button_get()==1);
    // Hiding must clear a held touch and leave both unrelated toggles and encounters alone.
    *(int32_t *)(encounter.bytes+24)=7;
    encounter.bytes[0x3c]=1; encounter.bytes[0x44]=1;
    ff7_ui_encounters_button_set(0); sync_encounters(f);
    assert(!count(layout,&encounter) && count(layout,&other)==1);
    assert(!*(int32_t *)(encounter.bytes+24) && !encounter.bytes[0x44]);
    assert(native==0);
    ff7_ui_encounters_set(0); assert(native==1 && ff7_ui_encounters_get()==0);
    for(int i=0;i<100;i++) {
        ff7_ui_encounters_button_set(1); sync_encounters(f); sync_encounters(f);
        assert(count(layout,&encounter)==1 && native==1);
        ff7_ui_encounters_button_set(0); sync_encounters(f);
        assert(!count(layout,&encounter) && native==1);
    }
    // Another layout shares the same native flag; hidden before its first display.
    object encounter2; init(&encounter2, CLS_TOGGLE, &native);
    void *second[MAX_LAYOUT]={&screen,&encounter2,NULL};
    int g=filter_layout(second); sync_encounters(g);
    assert(!count(second,&encounter2));
    ff7_ui_encounters_button_set(1); sync_encounters(g);
    assert(count(second,&encounter2)==1 && native==1);
    assert(filter_layout(NULL)==-1); sync_encounters(-1);
    // Existing settings without new keys retain ON/SHOW defaults.
    FILE *fp=fopen(CHEATS_PATH,"w"); assert(fp);
    fputs("exp 3\nwidescreen 1\nlanguage 2\n",fp); fclose(fp);
    load_cheats();
    assert(encounters_saved==1 && encounters_button_saved==1);
    assert(cheat_exp==3 && widescreen==1 && language==2);
    ff7_ui_encounters_button_set(0); port_menu_save_settings();
    settings_loaded=0; load_cheats();
    assert(encounters_saved==0 && encounters_button_saved==0);
    assert(cheat_exp==3 && widescreen==1 && language==2);
    // A touch changes the same native byte; saving must capture the live value.
    native=0; port_menu_save_settings(); settings_loaded=0; load_cheats();
    assert(encounters_saved==1 && encounters_button_saved==0);
    ff7_ui_encounters_set(encounters_saved);
    ff7_ui_encounters_button_set(encounters_button_saved);
    assert(ff7_ui_encounters_get()==1 && !ff7_ui_encounters_button_get());
    // Run the real touch wrapper: hidden buttons never reach the original hit tester;
    // shown buttons update the live value and persist it through the menu callback.
    object touch_obj; init(&touch_obj, CLS_TOGGLE, &native);
    void *touch_list[MAX_LAYOUT]={&touch_obj,NULL};
    void **current=touch_list; ui_current=&current;
    uint8_t hook_code[8]={0};
    touch_began_hook.addr=(uintptr_t)hook_code;
    touch_began_hook.thumb_addr=(uintptr_t)native_touch;
    touches_logged=TOUCH_LOG_LIMIT;
    ui_touch_began(1, 0, 0); assert(native==0 && !touch_list[0]);
    ff7_ui_encounters_button_set(1);
    ui_touch_began(2, 0, 0); assert(native==1 && touch_list[0]==&touch_obj);
    settings_loaded=0; load_cheats();
    assert(encounters_saved==0 && encounters_button_saved==1);
    // Each supported multiplier survives a settings round-trip and touch OFF/ON.
    for (int i=1;i<4;i++) {
        ff7_ui_encounters_set(rates[i]);
        port_menu_save_settings(); settings_loaded=0; load_cheats();
        assert(encounters_saved==rates[i]);
        ff7_ui_encounters_set(1); ff7_ui_encounters_set(encounters_saved);
        assert(ff7_ui_encounters_get()==rates[i]);
        ui_touch_began(3,0,0); assert(ff7_ui_encounters_get()==0);
        ui_touch_began(4,0,0); assert(ff7_ui_encounters_get()==rates[i]);
        settings_loaded=0; load_cheats(); assert(encounters_saved==rates[i]);
    }
    FILE *invalid=fopen(CHEATS_PATH,"w"); assert(invalid);
    fputs("encounters 3\n",invalid); fclose(invalid);
    settings_loaded=0; load_cheats(); assert(encounters_saved==1);
    puts("encounters: UI visibility, touch cleanup, shared layouts, native state, field/world multipliers and settings round-trip OK");
}
'''
with tempfile.TemporaryDirectory(prefix='ff7-encounters-') as td:
    source = Path(td) / 'test.c'
    source.write_text(stubs + ui + settings + tests)
    binary = Path(td) / 'test'
    subprocess.run(['cc', '-std=c11', '-Wno-unused-value', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], cwd=td, check=True)
