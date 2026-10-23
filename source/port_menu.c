/*
 * Port menu: hold L + R and press Start.
 *
 * Shortcuts to every 3D minigame, for measuring them without reaching them
 * in a save (docs/MINIGAMES.md), and profiler control.
 *
 * Launching a minigame does what the field script opcode MINIGAME (0x20,
 * handler 0x6134C0) does in its first step: it writes the return map and
 * position, the parameter and the minigame type into the field control
 * block ([0xCBF9D8]: +2 map, +4 x, +6 y, +0x22 z, +0x24 parameter, +0xF2
 * type), clears +0x26 and sets the exit state +1 = 0xC; the field engine
 * then switches modes as for the real opcode. The values are the ones the
 * game's own scripts use (extracted from flevel.lgp), so each minigame
 * starts as it does in the game and returns to the place where it is played
 * (the Wonder Square, the chocobo square, Fort Condor...). It only works
 * from a field (mode 1). The writes happen on the game thread, between
 * frames (port_menu_game_tick, from the frame limiter's sleep). Back in a
 * field, if the control block is still in the minigame state with +0x26 = 2
 * (returned), it is cleared as the opcode's second step would.
 *
 * Profiler: "automatic" profiles every minigame (profile_all mode, started
 * when a minigame mode begins, paused when it ends); it can also be started
 * and paused by hand. The log marks every change, to cut the profile.
 *
 * Scenes: "record scene" starts the profiler and saves where the player is
 * (DATA_PATH "cenas.txt": game mode, field map, x, y, walkmesh triangle,
 * direction); pressing it again pauses the profiler. The log marks the start
 * and end with the scene number, so the profile can be cut per scene. "Go to
 * scene" returns there later to measure again: in a field it does what the
 * script opcode MAPJUMP (0x60, handler 0x6131C4) does - map, x, y, triangle
 * and direction into the control block (+2, +4, +6, +0x22, +0x24), +0x26 =
 * 0, exit state +1 = 1 - and clears the state once the jump is done (+0x26 =
 * 2), as the opcode's second step would; a scene recorded in a minigame
 * starts that minigame. Only the place is restored, not the game's state
 * (story, party, items stay as in the current save). The player is the
 * field entity [ctrl + 0x2A] (set by the PC opcode); entities are 0x88 bytes
 * at [0xCC0B60]: x, y, z (<< 12) at +0xC/+0x10/+0x14, direction +0x38,
 * triangle +0x78 (opcodes XYZI and DIR). The current field is [0xCC15D0].
 *
 * A/B tests (v0.6.25): the minigame natives (coaster, snowboard), the
 * 30 fps cap and the texture reconversion cache can be turned off and on in
 * game, to see which one a visual problem comes from without restarting.
 * The change is applied on the game thread between frames
 * (port_menu_apply_ab: from the frame limiter's sleep, and from the
 * minigames' frame entry in ff7_framecap.c, since they never sleep).
 *
 * Drawing: one fixed-function vitaGL draw of colored rectangles (a 5x7
 * font drawn as runs of pixels), on the main thread with GL locked, right
 * before the swap. Every piece of GL state it changes is saved and restored
 * (the render thread caches the current program).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "port_menu.h"
#include "ff7_coaster.h"
#include "ff7_framecap.h"
#include "ff7_snowboard.h"
#include "ff7_texture.h"

#include "utils/logger.h"
#include "utils/profiler.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>
#include <vitaGL.h>

extern so_module so_mod;

#define VA_MODE   0xcbf9dcu // uint16: 1 field, 2 battle, 3 world map, 6.. minigames
#define VA_CTRL   0xcbf9d8u // pointer to the field control block
#define VA_FIELD_ID 0xcc15d0u // uint16: current field map
#define VA_ENTITIES 0xcc0b60u // pointer to the field entities (0x88 bytes each)
#define MODE_FIELD 1
#define EXIT_MAPJUMP 1
#define EXIT_MINIGAME 0xc
#define SCENES_PATH DATA_PATH "cenas.txt"
#define MAX_SCENES 16

typedef struct {
    int mode, map, x, y, triangle, dir;
} scene;

typedef struct {
    const char * name;
    uint8_t type, param;
    uint16_t map;
    int16_t x, y;
    uint16_t z;
} minigame;

// From the MINIGAME opcodes of the game's scripts (field: return map)
static const minigame games[] = {
    { "G-BIKE (MOTO)",              0,   0, 507,  243,  176, 92 }, // games_2
    { "CORRIDA DE CHOCOBO",         1,   0, 511, -372,   33, 81 }, // crcin_1
    { "SNOWBOARD (GELEIRA)",        2,  64, 658, -201, 3116, 71 }, // snow -> hyou1
    { "FORT CONDOR",                3, 128, 356,    9,  114, 17 }, // convil_2
    { "SUBMARINO",                  4,   0, 406, -101, -156,  8 }, // subin_1b
    { "MONTANHA-RUSSA",             5,   0, 487,  411,  -12, 50 }, // jetin1
    { "SNOWBOARD (FLIPERAMA)",      6,   0, 507,  -37,  273, 26 }, // games_2
};
#define NUM_GAMES ((int) (sizeof(games) / sizeof(games[0])))
enum { ITEM_RECORD = NUM_GAMES, ITEM_GOTO, ITEM_AB_NATIVES, ITEM_AB_CAP, ITEM_AB_TEXCACHE, ITEM_AUTO, ITEM_PROFILER,
       ITEM_CLOSE, NUM_ITEMS };

static const uint32_t * memmap;

// Shared with the game thread
static volatile int request = -1;  // minigame to launch
static volatile int launched = -1; // launched, waiting to be back in a field
static volatile int auto_profile = 1;
static volatile int game_mode;
static volatile int record_request; // 1: save the scene and start the profiler
static volatile int goto_request = -1; // scene to return to
static volatile int recording = -1; // scene being profiled
static volatile int jumping;        // map jump written, waiting for it to end
static scene scenes[MAX_SCENES];
// A/B tests: wanted (main thread) and applied (game thread) states
static volatile int ab_want[3] = { 1, 1, 1 }; // minigame natives, 30 fps cap, texture cache
static int ab_applied[3] = { 1, 1, 1 };
static volatile int num_scenes;
static int scene_cursor; // main thread

// Main thread
static int open, cursor, wait_release;
static char message[64];
static SceUInt64 message_until;

static uint8_t * x86(uint32_t va) {
    uint32_t page = memmap ? memmap[va >> 12] : 0;
    return page ? (uint8_t *) (page + (va & 0xfff)) : NULL;
}

static int read_mode(void) {
    uint8_t * p = x86(VA_MODE);
    if (!p) return 0;
    uint16_t m;
    memcpy(&m, p, 2);
    return m;
}

static uint8_t * control_block(void) {
    uint8_t * p = x86(VA_CTRL);
    if (!p) return NULL;
    uint32_t va;
    memcpy(&va, p, 4);
    // the fields written below, +0 .. +0xF2, on one mapped page or two contiguous
    uint8_t * lo = va ? x86(va) : NULL, * hi = va ? x86(va + 0xf2) : NULL;
    return lo && hi == lo + 0xf2 ? lo : NULL;
}

static void put16(uint8_t * p, uint16_t v) { memcpy(p, &v, 2); }
static uint16_t get16(const uint8_t * p) { uint16_t v; memcpy(&v, p, 2); return v; }

static void set_message(const char * m) {
    snprintf(message, sizeof(message), "%s", m);
    message_until = sceKernelGetProcessTimeWide() + 4000000;
}

static int is_minigame(int mode) {
    return (mode >= 6 && mode <= 11) || mode == 14;
}

static int get32(const uint8_t * p) { int32_t v; memcpy(&v, p, 4); return v; }

// The minigame shortcut for a minigame mode (-1: none)
static int game_for_mode(int mode) {
    switch (mode) {
        case 6: return 0;  // highway
        case 7: return 1;  // chocobo
        case 8: return 2;  // snowboard
        case 9: return 3;  // condor
        case 10: return 4; // submarine
        case 11: return 5; // coaster
        case 14: return 6; // snowboard (arcade)
        default: return -1;
    }
}

static void load_scenes(void) {
    FILE * f = fopen(SCENES_PATH, "r");
    if (!f) return;
    char line[128];
    int n = 0;
    while (n < MAX_SCENES && fgets(line, sizeof(line), f)) {
        scene sc;
        int id;
        if (sscanf(line, "%d %d %d %d %d %d %d", &id, &sc.mode, &sc.map, &sc.x, &sc.y, &sc.triangle, &sc.dir) == 7)
            scenes[n++] = sc;
    }
    fclose(f);
    num_scenes = n;
}

static void save_scenes(void) {
    FILE * f = fopen(SCENES_PATH, "w");
    if (!f) {
        l_error("port menu: could not write %s", SCENES_PATH);
        return;
    }
    fprintf(f, "# cena modo mapa x y triangulo direcao (port menu: GRAVAR CENA)\n");
    for (int i = 0; i < num_scenes; i++)
        fprintf(f, "%d %d %d %d %d %d %d\n", i + 1, scenes[i].mode, scenes[i].map, scenes[i].x, scenes[i].y,
                scenes[i].triangle, scenes[i].dir);
    fclose(f);
}

// Where the player is (game thread). Fields other than mode are 0 outside a field.
static int capture_scene(scene * sc, int mode) {
    memset(sc, 0, sizeof(*sc));
    sc->mode = mode;
    if (mode != MODE_FIELD) return 1;
    uint8_t * c = control_block(), * id = x86(VA_FIELD_ID), * ents = x86(VA_ENTITIES);
    if (!c || !id || !ents) return 0;
    sc->map = get16(id);
    uint32_t e = (uint32_t) get32(ents) + 0x88u * get16(c + 0x2a);
    uint8_t * px = x86(e + 0xc), * py = x86(e + 0x10), * pd = x86(e + 0x38), * pt = x86(e + 0x78);
    if (!px || !py || !pd || !pt) return 0;
    sc->x = (int16_t) (get32(px) >> 12);
    sc->y = (int16_t) (get32(py) >> 12);
    sc->dir = *pd;
    sc->triangle = get16(pt);
    return 1;
}

void port_menu_init(void) {
    memmap = (const uint32_t *) so_symbol(&so_mod, "MEMMAP");
    load_scenes();
    l_info("port menu: hold L + R and press Start (minigame shortcuts, profiler, %d saved scenes)", num_scenes);
}

// ---------------------------------------------------------------- game thread

void port_menu_apply_ab(void) {
    for (int i = 0; i < 3; i++) {
        int w = ab_want[i];
        if (w == ab_applied[i]) continue;
        ab_applied[i] = w;
        if (i == 0) {
            ff7_coaster_set(w);
            ff7_snowboard_set(w);
        } else if (i == 1) {
            ff7_framecap_set(w);
        } else {
            ff7_texture_cache_set(w);
        }
    }
}

void port_menu_game_tick(void) {
    static int tries, seen_other, auto_started, last_mode = -1;
    port_menu_apply_ab();
    int mode = read_mode();
    game_mode = mode;

    if (record_request) {
        record_request = 0;
        scene sc;
        if (num_scenes >= MAX_SCENES) {
            l_warn("port menu: %d scenes saved already (%s)", MAX_SCENES, SCENES_PATH);
        } else if (!capture_scene(&sc, mode)) {
            l_warn("port menu: scene not saved (mode %d, field data not found)", mode);
        } else {
            scenes[num_scenes] = sc;
            recording = num_scenes;
            num_scenes++;
            save_scenes();
            profiler_start_now();
            l_info("port menu: scene %d recording (profiler on): mode %d, map %d, x %d, y %d, triangle %d,"
                   " direction %d", recording + 1, sc.mode, sc.map, sc.x, sc.y, sc.triangle, sc.dir);
        }
    }

    int g = goto_request;
    if (g >= 0) {
        const scene * sc = &scenes[g];
        uint8_t * c = mode == MODE_FIELD ? control_block() : NULL;
        if (sc->mode != MODE_FIELD) {
            int game = game_for_mode(sc->mode);
            goto_request = -1;
            if (game >= 0) {
                request = game; // as the minigame shortcut
                l_info("port menu: scene %d is minigame mode %d: launching it", g + 1, sc->mode);
            } else {
                l_warn("port menu: scene %d (mode %d) cannot be returned to", g + 1, sc->mode);
            }
        } else if (c && c[1] == 0) {
            put16(c + 0x02, (uint16_t) sc->map);
            put16(c + 0x04, (uint16_t) sc->x);
            put16(c + 0x06, (uint16_t) sc->y);
            put16(c + 0x22, (uint16_t) sc->triangle);
            put16(c + 0x24, (uint16_t) sc->dir);
            put16(c + 0x26, 0);
            c[1] = EXIT_MAPJUMP;
            jumping = 1;
            goto_request = -1;
            tries = 0;
            l_info("port menu: going to scene %d: map %d, x %d, y %d, triangle %d", g + 1, sc->map, sc->x, sc->y,
                   sc->triangle);
        } else if (++tries > 600) {
            l_warn("port menu: scene %d not reached (mode %d, field busy)", g + 1, mode);
            goto_request = -1;
            tries = 0;
        }
    }

    if (jumping && mode == MODE_FIELD) { // the MAPJUMP opcode's second step
        uint8_t * c = control_block();
        if (c && c[1] == EXIT_MAPJUMP && get16(c + 0x26) == 2) {
            c[1] = 0;
            jumping = 0;
            l_info("port menu: map jump done (field %d)", x86(VA_FIELD_ID) ? get16(x86(VA_FIELD_ID)) : -1);
        } else if (c && c[1] != EXIT_MAPJUMP) {
            jumping = 0;
        }
    }

    int r = request;
    if (r >= 0) {
        uint8_t * c = mode == MODE_FIELD ? control_block() : NULL;
        if (c && c[1] == 0) {
            const minigame * g = &games[r];
            put16(c + 0x02, g->map);
            put16(c + 0x04, (uint16_t) g->x);
            put16(c + 0x06, (uint16_t) g->y);
            put16(c + 0x22, g->z);
            put16(c + 0x24, g->param);
            c[0xf2] = g->type;
            put16(c + 0x26, 0);
            c[1] = EXIT_MINIGAME;
            l_info("port menu: launching %s (type %d, param %d, back to map %d)", g->name, g->type, g->param, g->map);
            launched = r;
            seen_other = 0;
            request = -1;
            tries = 0;
        } else if (++tries > 600) { // ~20 s: not in a field, or the field is busy (event, menu...)
            l_warn("port menu: %s not launched (mode %d, field busy)", games[r].name, mode);
            request = -1;
            tries = 0;
        }
    }

    if (launched >= 0) {
        if (mode != MODE_FIELD) {
            seen_other = 1;
        } else if (seen_other) { // back in a field
            uint8_t * c = control_block();
            if (c && c[1] == EXIT_MINIGAME && get16(c + 0x26) == 2) {
                c[1] = 0;
                put16(c + 0x26, 0);
                l_info("port menu: minigame state cleared in the field");
            }
            launched = -1;
        }
    }

    if (mode != last_mode) {
        last_mode = mode;
        if (auto_profile && is_minigame(mode) && !profiler_active()) {
            l_info("port menu: mode %d, profiler on (automatic)", mode);
            profiler_start_now();
            auto_started = 1;
        } else if (auto_started && !is_minigame(mode)) {
            l_info("port menu: mode %d, profiler paused (automatic)", mode);
            profiler_set_paused(1);
            auto_started = 0;
        }
    }
}

// ---------------------------------------------------------------- input

#define BTN_L (SCE_CTRL_L1 | SCE_CTRL_LTRIGGER)
#define BTN_R (SCE_CTRL_R1 | SCE_CTRL_RTRIGGER)

int port_menu_is_open(void) {
    return open;
}

static void activate(int item) {
    if (item < NUM_GAMES) {
        if (game_mode != MODE_FIELD) {
            char m[64];
            snprintf(m, sizeof(m), "SO A PARTIR DE UM CAMPO (MODO %d)", game_mode);
            set_message(m);
            return;
        }
        request = item;
        char m[64];
        snprintf(m, sizeof(m), "INICIANDO %s", games[item].name);
        set_message(m);
        open = 0;
        wait_release = 1;
    } else if (item == ITEM_RECORD) {
        if (recording >= 0) {
            profiler_set_paused(1);
            l_info("port menu: scene %d recording stopped (profiler paused)", recording + 1);
            char m[64];
            snprintf(m, sizeof(m), "CENA %d GRAVADA: ENVIE PROFILE.BIN E LOGS.TXT", recording + 1);
            set_message(m);
            recording = -1;
        } else if (num_scenes >= MAX_SCENES) {
            set_message("LIMITE DE CENAS: EDITE CENAS.TXT");
        } else {
            record_request = 1;
            set_message("GRAVANDO CENA: PROFILER LIGADO");
            open = 0;
            wait_release = 1;
        }
    } else if (item == ITEM_GOTO) {
        if (!num_scenes) {
            set_message("NENHUMA CENA GRAVADA");
        } else if (game_mode != MODE_FIELD) {
            set_message("SO A PARTIR DE UM CAMPO");
        } else {
            goto_request = scene_cursor;
            char m[64];
            snprintf(m, sizeof(m), "INDO PARA A CENA %d", scene_cursor + 1);
            set_message(m);
            open = 0;
            wait_release = 1;
        }
    } else if (item >= ITEM_AB_NATIVES && item <= ITEM_AB_TEXCACHE) {
        int k = item - ITEM_AB_NATIVES;
        ab_want[k] = !ab_want[k];
        static const char * const what[3] = { "NATIVOS DOS MINI-GAMES", "LIMITE DE 30 FPS", "CACHE DE TEXTURAS" };
        char m[64];
        snprintf(m, sizeof(m), "%s: %s", what[k], ab_want[k] ? "LIGADO" : "DESLIGADO");
        set_message(m);
        l_info("port menu: A/B %s %s (mode %d)", what[k], ab_want[k] ? "on" : "off", game_mode);
    } else if (item == ITEM_AUTO) {
        auto_profile = !auto_profile;
        l_info("port menu: automatic profiling %s", auto_profile ? "on" : "off");
    } else if (item == ITEM_PROFILER) {
        if (profiler_active()) {
            profiler_set_paused(1);
            set_message("PROFILER PAUSADO");
        } else {
            profiler_start_now();
            set_message("PROFILER LIGADO: PROFILE.BIN");
            l_info("port menu: profiler on (manual, mode %d)", game_mode);
        }
    } else {
        open = 0;
        wait_release = 1;
    }
}

uint32_t port_menu_input(uint32_t buttons) {
    static uint32_t prev;
    uint32_t pressed = buttons & ~prev;
    prev = buttons;
    int combo = (buttons & SCE_CTRL_START) && (buttons & BTN_L) && (buttons & BTN_R)
                && (pressed & (SCE_CTRL_START | BTN_L | BTN_R));

    if (!open) {
        if (wait_release) { // nothing reaches the game until every button is up
            if (buttons) return 0;
            wait_release = 0;
        }
        if (combo) {
            open = 1;
            cursor = 0;
            return 0; // the game sees every held button go up
        }
        // L + R held: the Start of the combo is not passed on
        if ((buttons & BTN_L) && (buttons & BTN_R)) buttons &= ~SCE_CTRL_START;
        return buttons;
    }

    if (combo || (pressed & SCE_CTRL_CIRCLE)) {
        open = 0;
        wait_release = 1;
    } else if (pressed & SCE_CTRL_UP) {
        cursor = (cursor + NUM_ITEMS - 1) % NUM_ITEMS;
    } else if (pressed & SCE_CTRL_DOWN) {
        cursor = (cursor + 1) % NUM_ITEMS;
    } else if ((pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) && cursor == ITEM_GOTO && num_scenes) {
        scene_cursor = (scene_cursor + (pressed & SCE_CTRL_LEFT ? num_scenes - 1 : 1)) % num_scenes;
    } else if (pressed & SCE_CTRL_CROSS) {
        activate(cursor);
    }
    return 0;
}

// ---------------------------------------------------------------- drawing

// 5x7 font: 7 rows of 5 bits (bit 4 = left column)
typedef struct { char c; uint8_t rows[7]; } glyph;
static const glyph font[] = {
    { 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } }, { 'B', { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } },
    { 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } }, { 'D', { 0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c } },
    { 'E', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } }, { 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f } }, { 'H', { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } }, { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } }, { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
    { 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } }, { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } }, { 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } }, { 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } }, { 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } }, { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a } }, { 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04 } }, { 'Z', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f } },
    { '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } }, { '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
    { '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } }, { '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
    { '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } }, { '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
    { '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } }, { '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } }, { '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
    { '-', { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 } }, { ':', { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c } }, { '(', { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } },
    { ')', { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } }, { '/', { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 } },
    { '>', { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 } }, { '+', { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 } },
    { '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } },
    { '<', { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 } },
};

#define SCALE   3
#define ADVANCE (6 * SCALE)
#define LINE_H  26 // 15 items fit in 544 lines
#define MAX_VERTICES 30000

static float verts[MAX_VERTICES * 2];
static uint8_t colors[MAX_VERTICES * 4];
static int nverts;

static void rect(float x0, float y0, float x1, float y1, uint32_t rgba) {
    if (nverts + 6 > MAX_VERTICES) return;
    const float v[12] = { x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1 };
    memcpy(verts + 2 * nverts, v, sizeof(v));
    for (int i = 0; i < 6; i++) memcpy(colors + 4 * (nverts + i), &rgba, 4);
    nverts += 6;
}

static const uint8_t * glyph_rows(char c) {
    for (unsigned i = 0; i < sizeof(font) / sizeof(font[0]); i++)
        if (font[i].c == c) return font[i].rows;
    return NULL;
}

static void text(float x, float y, const char * s, uint32_t rgba) {
    for (; *s; s++, x += ADVANCE) {
        const uint8_t * rows = glyph_rows(*s);
        if (!rows) continue;
        for (int r = 0; r < 7; r++) {
            int c = 0;
            while (c < 5) { // runs of lit pixels in the row
                if (!(rows[r] & (0x10 >> c))) { c++; continue; }
                int start = c;
                while (c < 5 && (rows[r] & (0x10 >> c))) c++;
                rect(x + start * SCALE, y + r * SCALE, x + c * SCALE, y + (r + 1) * SCALE, rgba);
            }
        }
    }
}

#define RGBA(r, g, b, a) ((uint32_t) (r) | (uint32_t) (g) << 8 | (uint32_t) (b) << 16 | (uint32_t) (a) << 24)

static void flush_draw(void) {
    if (!nverts) return;
    GLint prog, fbo, vp[4], sc[4], src_rgb, dst_rgb, src_a, dst_a;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_SCISSOR_BOX, sc);
    glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST), tex = glIsEnabled(GL_TEXTURE_2D);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 960, 544);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(0);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, colors);
    glDrawArrays(GL_TRIANGLES, 0, nverts);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    glUseProgram(prog);
    glBlendFuncSeparate(src_rgb, dst_rgb, src_a, dst_a);
    if (!blend) glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (tex) glEnable(GL_TEXTURE_2D);
    glScissor(sc[0], sc[1], sc[2], sc[3]);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    nverts = 0;
}

void port_menu_draw(void) {
    // Frame rate seen by the main thread (one call per swap)
    static SceUInt64 fps_t0;
    static int fps_frames;
    static float fps;
    SceUInt64 now = sceKernelGetProcessTimeWide();
    fps_frames++;
    if (now - fps_t0 >= 1000000) {
        fps = fps_frames * 1000000.0f / (float) (now - fps_t0);
        fps_frames = 0;
        fps_t0 = now;
    }

    int show_message = message[0] && now < message_until;
    if (!open && !show_message) return;

    char line[64];
    if (open) {
        const float x0 = 120, y0 = 16, x1 = 840; // 15 items: the box ends at ~540 of 544 lines
        const float y1 = y0 + 30 + (NUM_ITEMS + 4) * LINE_H;
        rect(x0, y0, x1, y1, RGBA(8, 12, 32, 225));
        rect(x0, y0, x1, y0 + 3, RGBA(120, 150, 230, 255));
        float y = y0 + 15;
        text(x0 + 20, y, "MENU DO PORT", RGBA(255, 255, 255, 255));
        y += LINE_H;
        snprintf(line, sizeof(line), "MODO %d   %.1f FPS   PROFILER %s", game_mode, fps,
                 profiler_active() ? "LIGADO" : "DESLIGADO");
        text(x0 + 20, y, line, RGBA(170, 190, 230, 255));
        y += LINE_H + 6;
        for (int i = 0; i < NUM_ITEMS; i++, y += LINE_H) {
            const char * label = line;
            if (i < NUM_GAMES) label = games[i].name;
            else if (i == ITEM_RECORD) {
                if (recording >= 0) snprintf(line, sizeof(line), "GRAVANDO CENA %d: PARAR", recording + 1);
                else snprintf(line, sizeof(line), "GRAVAR CENA (MARCADOR + PROFILER)");
            } else if (i == ITEM_GOTO) {
                if (!num_scenes) snprintf(line, sizeof(line), "IR PARA CENA: NENHUMA");
                else if (scenes[scene_cursor].mode == MODE_FIELD)
                    snprintf(line, sizeof(line), "IR PARA CENA < %d/%d > MAPA %d", scene_cursor + 1, num_scenes,
                             scenes[scene_cursor].map);
                else
                    snprintf(line, sizeof(line), "IR PARA CENA < %d/%d > MODO %d", scene_cursor + 1, num_scenes,
                             scenes[scene_cursor].mode);
            } else if (i == ITEM_AB_NATIVES) {
                snprintf(line, sizeof(line), "A/B NATIVOS DOS MINI-GAMES: %s", ab_want[0] ? "SIM" : "NAO");
            } else if (i == ITEM_AB_CAP) {
                snprintf(line, sizeof(line), "A/B LIMITE DE 30 FPS: %s", ab_want[1] ? "SIM" : "NAO");
            } else if (i == ITEM_AB_TEXCACHE) {
                snprintf(line, sizeof(line), "A/B CACHE DE TEXTURAS: %s", ab_want[2] ? "SIM" : "NAO");
            } else if (i == ITEM_AUTO) snprintf(line, sizeof(line), "PERFIL AUTOMATICO NOS MINI-GAMES: %s", auto_profile ? "SIM" : "NAO");
            else if (i == ITEM_PROFILER) snprintf(line, sizeof(line), "PROFILER AGORA: %s", profiler_active() ? "PAUSAR" : "INICIAR");
            else label = "FECHAR";
            if (i == cursor) rect(x0 + 10, y - 6, x1 - 10, y + 7 * SCALE + 6, RGBA(60, 90, 170, 255));
            int needs_field = i < NUM_GAMES || i == ITEM_GOTO;
            uint32_t color = needs_field && game_mode != MODE_FIELD ? RGBA(130, 130, 140, 255) : RGBA(255, 255, 255, 255);
            text(x0 + 30, y, label, color);
        }
        y += 6;
        text(x0 + 20, y, "X: ESCOLHER  O: FECHAR  ESQ/DIR: CENA", RGBA(170, 190, 230, 255));
        if (game_mode != MODE_FIELD) text(x0 + 20, y + LINE_H, "MINI-GAMES: SO A PARTIR DE UM CAMPO", RGBA(230, 190, 120, 255));
    }
    if (show_message) {
        int w = (int) strlen(message) * ADVANCE + 40;
        rect(480 - w / 2, 480, 480 + w / 2, 480 + 7 * SCALE + 24, RGBA(8, 12, 32, 225));
        text(480 - w / 2 + 20, 492, message, RGBA(255, 255, 255, 255));
    }
    flush_draw();
}
