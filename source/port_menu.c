/*
 * Port menu: hold L + R and press Start; inside it, L and R switch tabs.
 *
 * Shortcuts to every 3D minigame and to the world map in each vehicle, for
 * measuring them without reaching them in a save (docs/MINIGAMES.md), and
 * profiler control.
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
 * World map in a vehicle (v0.6.36): "MAPA-MUNDI" (LEFT/RIGHT: buggy,
 * submarine, Highwind) jumps from a field to the world map already riding
 * that vehicle, to measure the world map with each one. It uses the
 * game's own entry points: a MAPJUMP (as "go to scene") to a world map
 * entry 1..0x40 (the field engine switches to mode 3 when the target is in
 * 1..0x40, 0x63C8A6). The world map's entry script (wm0.ev, system
 * function 0) creates each vehicle from bits of the save (bank 0 =
 * 0xDC08DC, bit n = byte n >> 3, bit n & 7) and, for some entries, boards
 * it (script opcode 0x30C) and places it at fixed coordinates:
 *   entry 7    buggy     bits 0x3F8 (buggy on the map) + 0x3F9 (party in it)
 *   entry 0x36 submarine bit 0x1A92 (byte 0xDC0C2E, 0x04)
 *   entry 0x29 Highwind  bit 0x3FC (Highwind on the map)
 * Bits 0x3F8/0x3F9/0x3FC are the save's "vehicles on the world map" byte
 * (savemap 0xC23 = 0xDC095B: 0x01 buggy, 0x02 riding it, 0x10 Highwind);
 * the buggy and the Highwind share one position slot (savemap 0xF74), so
 * one is turned off when the other is set. [0xCC1644] (world map entry
 * type: 0 field, 1 battle, 2 loaded save) is set to 0 first: a save made on
 * the world map leaves it at 2, and the entry from a field would be ignored
 * (0x409A22). This changes the save in memory: do not save afterwards.
 *
 * A/B tests (v0.6.25): the minigame natives (coaster, snowboard), the
 * 30 fps cap and the texture reconversion cache can be turned off and on in
 * game, to see which one a visual problem comes from without restarting.
 * The change is applied on the game thread between frames
 * (port_menu_apply_ab: from the frame limiter's sleep, and from the
 * minigames' frame entry in ff7_framecap.c, since they never sleep).
 *
 * Tabs (v0.6.40): L and R switch between CHEATS, GAMEPLAY and DEBUG (all of
 * the above). The cheat and gameplay choices are saved in DATA_PATH
 * "cheats.txt" and applied by the game thread between frames
 * (apply_cheats):
 *   - Infinite HP/MP. In a battle, the party's actors (0x68 bytes each at
 *     0x9AB0DC; 0-2 party, 4-9 enemies): HP +0x2C = max +0x30 (dwords), MP
 *     +0x28 = max +0x2A (words), as the game's own full restore (0x5DB593)
 *     does. Elsewhere, the save's character records (0x84 bytes at 0xDBFD8C,
 *     savemap 0x54): HP +0x2C = max +0x38, MP +0x30 = max +0x3A. A KO'd
 *     character (HP 0) is left KO'd.
 *   - EXP / gil x2..x4. Each enemy dying adds its actor's EXP (+0x5C) and gil
 *     (+0x58) to the battle's totals (0x430DD0: 0x99E2C0 EXP, 0x99E2C8 gil,
 *     which the spoils screen 0x6C6B30 moves into the save). The enemies'
 *     values are multiplied once, as soon as they show up in the battle;
 *     level ups and Gil Plus work as usual.
 *   - Materia: one chosen materia (by color, or a summon) into the first
 *     free inventory slot, at level 1 (AP 0) or mastered (AP 0xFFFFFF; for
 *     Enemy Skill, every skill learned). Ids and colors from the game's
 *     kernel.bin (materia type, low nibble: 0/1/4 independent, 2/3/6/7/8
 *     command, 5 support, 9/A magic, B/C summon) and kernel2.bin (names).
 *   - Field speed x2..x4: the player's base speed [0xCC0D98] times the
 *     factor, only while the model update 0x6342C6 runs (scale_field_speed),
 *     which rewrites the entity's speed (+0x76) from it every frame. Walking
 *     and running get faster; the collision size, also made from the base,
 *     does not change.
 *   - Dialog skip: presses OK for every field MESSAGE window, never for an
 *     ASK (see dialog_skip_buttons).
 *   - World map speed x2..x4: the speed on foot, an immediate 30 in the
 *     recompiled code (scale_world_speed); vehicles are left as they are.
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
#include "ff7_submarine.h"
#include "ff7_framecap.h"
#include "ff7_snowboard.h"
#include "ff7_texture.h"
#include "ff7_unhook.h"
#include "ff7_anim.h"

#include "utils/logger.h"
#include "utils/profiler.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <kubridge.h>
#include <psp2/kernel/clib.h>
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
#define VA_WM_ENTRY 0xcc1644u // int32: world map entry type (0 field, 1 battle, 2 loaded save)
#define VA_WM_VEHICLES 0xdc095bu // savemap 0xC23: vehicles on the world map (script bits 0x3F8..)
#define VA_WM_SUB 0xdc0c2eu // script bit 0x1A92 (byte 0x352 of the bank, 0x04): submarine
#define VA_WM_NO_BUGGY 0xdc0aabu // script bit 0xE7F (0x80): no buggy when entering from a field
#define VA_MATERIA 0xdc04b4u // savemap 0x77C: 200 materia, id | AP << 8 (-1: empty slot)
#define NUM_MATERIA 200
#define SUMMON_FIRST 0x4a      // Choco/Mog .. Master Summon (kernel2.bin materia names)
#define SUMMON_LAST  0x5a
#define AP_MASTER 0xffffffu    // the most a materia holds: every summon at its top level
#define VA_ACTORS 0x9ab0dcu    // battle actors, 0x68 bytes: 0-2 party, 4-9 enemies
#define ACTOR_SIZE 0x68
#define VA_CHARS 0xdbfd8cu     // savemap 0x54: 9 character records of 0x84 bytes
#define CHAR_SIZE 0x84
#define VA_FIELD_SPEED 0xcc0d98u // int16: the player's base speed in a field
#define MODE_BATTLE 2
#define MODE_WORLD 3
#define SCENES_PATH DATA_PATH "cenas.txt"
#define CHEATS_PATH DATA_PATH "cheats.txt"
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

typedef struct {
    const char * name;
    uint16_t entry;   // world map entry (MAPJUMP target)
    uint32_t flag_va; // save byte with the vehicle's bits
    uint8_t set, clear;
} vehicle;

// Entries of wm0.ev's system function 0 that board a vehicle (opcode 0x30C)
static const vehicle vehicles[] = {
    { "BUGGY",     0x07, VA_WM_VEHICLES, 0x03, 0x10 }, // bits 0x3F8 + 0x3F9; Highwind off (same slot)
    { "SUBMARINE", 0x36, VA_WM_SUB,      0x04, 0x00 }, // bit 0x1A92
    { "HIGHWIND",  0x29, VA_WM_VEHICLES, 0x10, 0x03 }, // bit 0x3FC; buggy off (same slot)
};
#define NUM_VEHICLES ((int) (sizeof(vehicles) / sizeof(vehicles[0])))

// From the MINIGAME opcodes of the game's scripts (field: return map)
static const minigame games[] = {
    { "G-BIKE (MOTORCYCLE)",              0,   0, 507,  243,  176, 92 }, // games_2
    { "CHOCOBO RACING",         1,   0, 511, -372,   33, 81 }, // crcin_1
    { "SNOWBOARD (GLACIER)",        2,  64, 658, -201, 3116, 71 }, // snow -> hyou1
    { "FORT CONDOR",                3, 128, 356,    9,  114, 17 }, // convil_2
    { "SUBMARINE",                  4,   0, 406, -101, -156,  8 }, // subin_1b
    { "ROLLER COASTER",             5,   0, 487,  411,  -12, 50 }, // jetin1
    { "SNOWBOARD (ARCADE)",      6,   0, 507,  -37,  273, 26 }, // games_2
};
#define NUM_GAMES ((int) (sizeof(games) / sizeof(games[0])))
enum { ITEM_RECORD = NUM_GAMES, ITEM_GOTO, ITEM_WORLD, ITEM_SUMMONS, ITEM_AB_NATIVES, ITEM_AB_CAP, ITEM_AB_TEXCACHE, ITEM_AUTO, ITEM_PROFILER,
       ITEM_TITLE, ITEM_CLOSE, ITEM_HPMP, ITEM_EXP, ITEM_GIL, ITEM_LEVEL, ITEM_SUMMON_PICK, ITEM_COLOR, ITEM_MATERIA_PICK,
       ITEM_FIELD_SPEED, ITEM_WORLD_SPEED, ITEM_AUTO_SKIP, ITEM_LANGUAGE };

// Tabs (L / R): the items of each, in order
static const uint8_t tab_cheats[] = { ITEM_HPMP, ITEM_EXP, ITEM_GIL, ITEM_LEVEL, ITEM_SUMMON_PICK, ITEM_COLOR,
                                      ITEM_MATERIA_PICK, ITEM_SUMMONS, ITEM_CLOSE };
static const uint8_t tab_gameplay[] = { ITEM_FIELD_SPEED, ITEM_WORLD_SPEED, ITEM_AUTO_SKIP, ITEM_LANGUAGE, ITEM_CLOSE };
static const uint8_t tab_debug[] = { 0, 1, 2, 3, 4, 5, 6, ITEM_RECORD, ITEM_GOTO, ITEM_WORLD, ITEM_AB_NATIVES, ITEM_AB_CAP,
                                     ITEM_AB_TEXCACHE, ITEM_AUTO, ITEM_PROFILER, ITEM_TITLE, ITEM_CLOSE };
typedef struct {
    const char * name;
    const uint8_t * items;
    int count;
} tab;
static const tab tabs[] = {
    { "CHEATS",   tab_cheats,   (int) sizeof(tab_cheats) },
    { "GAMEPLAY", tab_gameplay, (int) sizeof(tab_gameplay) },
    { "DEBUG",    tab_debug,    (int) sizeof(tab_debug) },
};
#define NUM_TABS ((int) (sizeof(tabs) / sizeof(tabs[0])))
#define MAX_TAB_ITEMS ((int) sizeof(tab_debug))

// Materia ids and names (kernel.bin types, kernel2.bin names of the game's data), by color
typedef struct { uint8_t id; const char * name; } materia_name;
static const materia_name mat_green[] = {
    { 0x31, "FIRE" }, { 0x32, "ICE" }, { 0x33, "EARTH" }, { 0x34, "LIGHTNING" }, { 0x35, "RESTORE" },
    { 0x36, "HEAL" }, { 0x37, "REVIVE" }, { 0x38, "SEAL" }, { 0x39, "MYSTIFY" }, { 0x3a, "TRANSFORM" },
    { 0x3b, "EXIT" }, { 0x3c, "POISON" }, { 0x3d, "GRAVITY" }, { 0x3e, "BARRIER" }, { 0x40, "COMET" },
    { 0x41, "TIME" }, { 0x44, "DESTRUCT" }, { 0x45, "CONTAIN" }, { 0x46, "FULLCURE" }, { 0x47, "SHIELD" },
    { 0x48, "ULTIMA" }, { 0x49, "MASTER MAGIC" },
};
static const materia_name mat_yellow[] = {
    { 0x0e, "SLASH-ALL" }, { 0x0f, "DOUBLE CUT" }, { 0x13, "W-MAGIC" }, { 0x14, "W-SUMMON" },
    { 0x15, "W-ITEM" }, { 0x24, "STEAL" }, { 0x25, "SENSE" }, { 0x27, "THROW" }, { 0x28, "MORPH" },
    { 0x29, "DEATHBLOW" }, { 0x2a, "MANIPULATE" }, { 0x2b, "MIME" }, { 0x2c, "ENEMY SKILL" },
    { 0x30, "MASTER COMMAND" },
};
static const materia_name mat_purple[] = {
    { 0x00, "MP PLUS" }, { 0x01, "HP PLUS" }, { 0x02, "SPEED PLUS" }, { 0x03, "MAGIC PLUS" },
    { 0x04, "LUCK PLUS" }, { 0x05, "EXP PLUS" }, { 0x06, "GIL PLUS" }, { 0x07, "ENEMY AWAY" },
    { 0x08, "ENEMY LURE" }, { 0x09, "CHOCOBO LURE" }, { 0x0a, "PRE-EMPTIVE" }, { 0x0b, "LONG RANGE" },
    { 0x0c, "MEGA ALL" }, { 0x0d, "COUNTER ATTACK" }, { 0x10, "COVER" }, { 0x11, "UNDERWATER" },
    { 0x12, "HP<->MP" },
};
static const materia_name mat_blue[] = {
    { 0x17, "ALL" }, { 0x18, "COUNTER" }, { 0x19, "MAGIC COUNTER" }, { 0x1a, "MP TURBO" },
    { 0x1b, "MP ABSORB" }, { 0x1c, "HP ABSORB" }, { 0x1d, "ELEMENTAL" }, { 0x1e, "ADDED EFFECT" },
    { 0x1f, "SNEAK ATTACK" }, { 0x20, "FINAL ATTACK" }, { 0x21, "ADDED CUT" }, { 0x22, "STEAL AS WELL" },
    { 0x23, "QUADRA MAGIC" },
};
static const materia_name mat_red[] = { // SUMMON_FIRST .. SUMMON_LAST
    { 0x4a, "CHOCO/MOG" }, { 0x4b, "SHIVA" }, { 0x4c, "IFRIT" }, { 0x4d, "RAMUH" }, { 0x4e, "TITAN" },
    { 0x4f, "ODIN" }, { 0x50, "LEVIATHAN" }, { 0x51, "BAHAMUT" }, { 0x52, "KUJATA" }, { 0x53, "ALEXANDER" },
    { 0x54, "PHOENIX" }, { 0x55, "NEO BAHAMUT" }, { 0x56, "HADES" }, { 0x57, "TYPHON" }, { 0x58, "BAHAMUT ZERO" },
    { 0x59, "KNIGHTS OF ROUND" }, { 0x5a, "MASTER SUMMON" },
};
typedef struct {
    const char * name;
    const materia_name * list;
    int count;
} materia_color;
#define MATERIA_COLOR(n, l) { n, l, (int) (sizeof(l) / sizeof(l[0])) }
static const materia_color materia_colors[] = {
    MATERIA_COLOR("GREEN (MAGIC)", mat_green),
    MATERIA_COLOR("YELLOW (COMMAND)", mat_yellow),
    MATERIA_COLOR("PURPLE (INDEPENDENT)", mat_purple),
    MATERIA_COLOR("BLUE (SUPPORT)", mat_blue),
    MATERIA_COLOR("RED (SUMMON)", mat_red),
};
#define NUM_COLORS ((int) (sizeof(materia_colors) / sizeof(materia_colors[0])))
#define NUM_SUMMONS ((int) (sizeof(mat_red) / sizeof(mat_red[0])))

static const uint32_t * memmap;

// Shared with the game thread
static volatile int request = -1;  // minigame to launch
static volatile int launched = -1; // launched, waiting to be back in a field
static volatile int auto_profile = 1;
static volatile int game_mode;
static volatile int record_request; // 1: save the scene and start the profiler
static volatile int goto_request = -1; // scene to return to
/*
 * Back to the title screen, to load another save without restarting: from a
 * field, what the field script opcode GAMEOVER (0xFF, handler 0x6142B6)
 * does - exit state +1 = 0x1A, +0x26 = 0 in the control block. The game
 * shows its game over screen and returns to the title, whose "Continue"
 * opens the save files. The menu asks for a second press first.
 */
#define EXIT_GAMEOVER 0x1a
static volatile int title_request;
static int title_armed;
static volatile int recording = -1; // scene being profiled
static volatile int jumping;        // map jump written, waiting for it to end
static volatile int world_request = -1; // vehicle to go to the world map in
static int vehicle_cursor; // main thread
/*
 * Summon materia (v0.6.38): "MATERIAS DE INVOCACAO" puts the 17 summons
 * (ids 0x4A-0x5A) in the materia inventory, mastered, to measure every
 * summon in battle. Only the ones not in the inventory yet, in its first
 * empty slots, as the game's own "add materia" does (0x5C864B: the id in the
 * low byte, -1 = empty); materia equipped on a character is not looked at.
 * From a field or the world map, between frames on the game thread. Saving
 * afterwards keeps them.
 */
static volatile int summons_request;
static scene scenes[MAX_SCENES];
// A/B tests: wanted (main thread) and applied (game thread) states
static volatile int ab_want[3] = { 1, 1, 1 }; // minigame natives, 30 fps cap, texture cache
static int ab_applied[3] = { 1, 1, 1 };
static volatile int num_scenes;
static int scene_cursor; // main thread
// Cheats and gameplay (main thread writes, game thread applies; saved in CHEATS_PATH)
static volatile int cheat_hpmp;
static volatile int cheat_exp = 1, cheat_gil = 1;           // reward factors, 1..4
static volatile int speed_field = 1, speed_world = 1;       // movement factors, 1..4
static volatile int auto_skip;                              // skip the field's dialogs
static int language = -1, language_loaded;                  // game text: -1 system, 0..4 setLang (next start)
static int settings_loaded;
// setLang's values (MainActivity.onCreate): the data folders lang-en, -fr, -de, -es, -ja
static const char * const language_names[] = { "ENGLISH", "FRENCH", "GERMAN", "SPANISH", "JAPANESE" };
#define NUM_LANGUAGES 5
static int materia_max = 1;                                  // add at: 0 level 1 (AP 0), 1 mastered
static int summon_cursor, color_cursor, materia_cursor;      // main thread
static volatile int materia_request = -1;                    // id | AP << 8 to add
static volatile int materia_result;                          // game thread: 1 added, 2 inventory full, 3 wrong mode

// Main thread
static int open, cursor, wait_release, cur_tab;
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
static void put32(uint8_t * p, uint32_t v) { memcpy(p, &v, 4); }

// va .. va + len - 1 on one mapped page or two contiguous ones
static uint8_t * x86_span(uint32_t va, uint32_t len) {
    uint8_t * lo = x86(va), * hi = x86(va + len - 1);
    return lo && hi == lo + len - 1 ? lo : NULL;
}

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

static int clamp_factor(int v) { return v < 1 ? 1 : v > 4 ? 4 : v; }

static void load_cheats(void) {
    if (settings_loaded) return;
    settings_loaded = 1;
    FILE * f = fopen(CHEATS_PATH, "r");
    if (!f) return;
    char key[32];
    int v;
    while (fscanf(f, "%31s %d", key, &v) == 2) {
        if (!strcmp(key, "hpmp")) cheat_hpmp = v != 0;
        else if (!strcmp(key, "exp")) cheat_exp = clamp_factor(v);
        else if (!strcmp(key, "gil")) cheat_gil = clamp_factor(v);
        else if (!strcmp(key, "materia_max")) materia_max = v != 0;
        else if (!strcmp(key, "speed_field")) speed_field = clamp_factor(v);
        else if (!strcmp(key, "speed_world")) speed_world = clamp_factor(v);
        else if (!strcmp(key, "auto_skip")) auto_skip = v != 0;
        else if (!strcmp(key, "language")) language = v >= 0 && v < NUM_LANGUAGES ? v : -1;
    }
    fclose(f);
}

static void save_cheats(void) {
    FILE * f = fopen(CHEATS_PATH, "w");
    if (!f) {
        l_error("port menu: could not write %s", CHEATS_PATH);
        return;
    }
    fprintf(f, "hpmp %d\nexp %d\ngil %d\nmateria_max %d\nspeed_field %d\nspeed_world %d\nauto_skip %d\nlanguage %d\n",
            cheat_hpmp, cheat_exp, cheat_gil, materia_max, speed_field, speed_world, auto_skip, language);
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
    load_cheats();
    l_info("port menu: hold L + R and press Start (minigame shortcuts, profiler, %d saved scenes)", num_scenes);
    l_info("port menu: cheats: infinite HP/MP %d, EXP x%d, gil x%d; speed: field x%d, world map x%d", cheat_hpmp,
           cheat_exp, cheat_gil, speed_field, speed_world);
}

int port_menu_language(void) {
    load_cheats();
    language_loaded = language;
    return language;
}

// ---------------------------------------------------------------- cheats (game thread)

// First free slot of the materia inventory (-1: full)
static int add_materia(uint32_t m) {
    uint8_t * inv = x86_span(VA_MATERIA, 4 * NUM_MATERIA);
    if (!inv) return -1;
    for (int i = 0; i < NUM_MATERIA; i++) {
        if ((uint32_t) get32(inv + 4 * i) == 0xffffffffu) {
            put32(inv + 4 * i, m);
            return i;
        }
    }
    return -1;
}

static void refill_hpmp(int mode) {
    if (mode == MODE_BATTLE) {
        for (int i = 0; i < 3; i++) { // the party's actors
            uint8_t * a = x86_span(VA_ACTORS + ACTOR_SIZE * i, 0x34);
            if (!a) continue;
            uint32_t hp = (uint32_t) get32(a + 0x2c), max_hp = (uint32_t) get32(a + 0x30);
            if (hp && hp < max_hp) put32(a + 0x2c, max_hp);
            uint16_t mp = get16(a + 0x28), max_mp = get16(a + 0x2a);
            if (mp < max_mp) put16(a + 0x28, max_mp);
        }
        return; // the battle writes its HP/MP back to the save when it ends
    }
    for (int i = 0; i < 9; i++) {
        uint8_t * c = x86_span(VA_CHARS + CHAR_SIZE * i, 0x3c);
        if (!c) continue;
        uint16_t hp = get16(c + 0x2c), max_hp = get16(c + 0x38);
        if (hp && hp < max_hp && max_hp <= 9999) put16(c + 0x2c, max_hp);
        uint16_t mp = get16(c + 0x30), max_mp = get16(c + 0x3a);
        if (mp < max_mp && max_mp <= 999) put16(c + 0x30, max_mp);
    }
}

/*
 * EXP and gil of the enemies (actors 4-9: +0x58 gil, +0x5C EXP), multiplied
 * once: a value other than the one written is new (a new battle, an enemy
 * that joins). Forgotten outside a battle.
 */
static void scale_rewards(int mode) {
    static uint32_t written[6][2];
    static int valid[6][2];
    if (mode != MODE_BATTLE) {
        memset(valid, 0, sizeof(valid));
        return;
    }
    const int factor[2] = { cheat_gil, cheat_exp };
    for (int e = 0; e < 6; e++) {
        uint8_t * a = x86_span(VA_ACTORS + ACTOR_SIZE * (4 + e) + 0x58, 8);
        if (!a) continue;
        for (int k = 0; k < 2; k++) {
            uint32_t v = (uint32_t) get32(a + 4 * k);
            if (valid[e][k] && v == written[e][k]) continue;
            uint64_t w = (uint64_t) v * (uint32_t) factor[k];
            if (w > 0x7fffffffu) w = 0x7fffffffu;
            if (w != v) put32(a + 4 * k, (uint32_t) w);
            written[e][k] = (uint32_t) w;
            valid[e][k] = 1;
        }
    }
}

/*
 * Field: every frame the field's model update (0x6342C6, called from
 * 0x63C17F) writes the player entity's movement speed (+0x76) from the base
 * [0xCC0D98]: x3 walking, x8 running, x2 / x12 in the other cases (key state
 * and the byte [0xCC0DC2]). The base also sizes the player when a field
 * starts (0x633C2F: collision radius +0x72 = base * 34 / 512) and the
 * walkmesh look-ahead (0x644075: base / 4), so it is only multiplied while
 * 0x6342C6 runs: a hook on it scales the base, calls the original and puts
 * the base back. Inside 0x6342C6 the base is read only for the speed. The
 * hook is installed the first time a factor is chosen (game thread, between
 * frames) and stays.
 */
#define MODELS_UPDATE_SYM "_Z15func_0x006342c6v"
static so_hook models_hook;

static void models_update_scaled(void) {
    uint8_t * p = x86_span(VA_FIELD_SPEED, 2);
    int16_t base = p ? (int16_t) get16(p) : 0;
    int32_t w = (int32_t) base * speed_field;
    if (w > 0x7fff) w = 0x7fff;
    int scaled = p && base > 0 && w != base;
    if (scaled) put16(p, (uint16_t) w);
    SO_CONTINUE_HELD(int, models_hook);
    if (scaled && (int16_t) get16(p) == (int16_t) w) put16(p, (uint16_t) base);
}

static void scale_field_speed(void) {
    static int installed, broken;
    if (installed || broken || speed_field == 1) return;
    uintptr_t fn = so_symbol(&so_mod, MODELS_UPDATE_SYM);
    if (!fn) {
        l_warn("port menu: %s not found, field speed left as is", MODELS_UPDATE_SYM);
        broken = 1;
        return;
    }
    profiler_hold();
    models_hook = hook_addr(fn, (uintptr_t) &models_update_scaled);
    profiler_release();
    installed = 1;
    l_info("port menu: field speed hook on %s (factor %d)", MODELS_UPDATE_SYM, speed_field);
}

/*
 * World map: on foot, the player moves at 30 * [0xDFC480] a frame (0x74EA48:
 * 60 chocobo, 45 buggy / submarine, 120 Highwind, 30 for every other model;
 * stored in a byte, so 30 * 2 * 4 = 240 still fits). The 30 is an immediate
 * of the recompiled code, "movs r2, #30" at func_0x0074ea48 + 0x276A: the
 * port writes 30 * factor in its place. [0xDFC480] is left alone (it times
 * the whole world map). Game thread, between frames: the function is not
 * running.
 */
#define WM_WALK_SYM "_Z15func_0x0074ea48v"
#define WM_WALK_OFF 0x276a
#define WM_WALK_SPEED 30
static void scale_world_speed(void) {
    static uint8_t * imm;
    static int applied = 1, broken;
    if (broken || speed_world == applied) return;
    if (!imm) {
        uintptr_t fn = so_symbol(&so_mod, WM_WALK_SYM);
        uint8_t * p = fn ? (uint8_t *) ((fn & ~1u) + WM_WALK_OFF) : NULL;
        if (!p || p[0] != WM_WALK_SPEED || p[1] != 0x22) { // movs r2, #30
            l_warn("port menu: world map walk speed not found (%s + 0x%X), left as is", WM_WALK_SYM, WM_WALK_OFF);
            broken = 1;
            return;
        }
        imm = p;
    }
    uint8_t v = (uint8_t) (WM_WALK_SPEED * speed_world);
    sceClibMemcpy(imm, &v, 1);
    kuKernelFlushCaches(imm, 2);
    applied = speed_world;
    l_info("port menu: world map walk speed %d (x%d)", v, applied);
}

/*
 * Dialog skip (Gameplay): the field's message loop 0x630D50 runs every frame
 * while a MESSAGE opcode (0x618DBD, 0x618F86, 0x61900A) shows a window; ASK
 * (questions) has a loop of its own (0x6310A1) and is never skipped. A hook
 * on 0x630D50 (game thread) notes the window (byte [ESP + 4], as the loop
 * reads [EBP + 8]) and the time. While a window was seen in the last 150 ms,
 * the main thread adds Cross (OK, docs/BOTOES-PS.md) to the pad, except while
 * the window closes (state 7, so no press reaches the field after it): held for
 * 3 polls, released for 3, so the game sees each press. The first press
 * shows the whole page, the next one turns it or closes the window. A window
 * whose state (window_mode, 0xCFF5B8 + 0x30 * id + 0x2C) and text pointer
 * (0xCBF578 + 4 * id) stay the same for 20 presses is left alone (a window
 * the player cannot close) until it closes.
 */
#define MESSAGE_LOOP_SYM "_Z15func_0x00630d50v"
#define VA_WINDOWS 0xcff5b8u   // 4 text boxes of 0x30 bytes; +0x2C window_mode
#define VA_DIALOG_PTR 0xcbf578u // the text pointer of each window
#define NUM_WINDOWS 4
static so_hook message_hook;
static volatile uint32_t message_seen_ms[NUM_WINDOWS];

static uint32_t now_ms(void) { return (uint32_t) (sceKernelGetProcessTimeWide() / 1000); }

static void message_loop_seen(void) {
    volatile uint32_t * regs = ff7_x86_regs();
    uint8_t * arg = regs ? x86(regs[4] + 4) : NULL; // ESP + 4: the window id
    int w = arg ? *arg : -1;
    SO_CONTINUE_HELD(int, message_hook);
    if (w >= 0 && w < NUM_WINDOWS) message_seen_ms[w] = now_ms() | 1;
}

static void install_dialog_skip(void) {
    static int installed, broken;
    if (installed || broken || !auto_skip) return;
    uintptr_t fn = so_symbol(&so_mod, MESSAGE_LOOP_SYM);
    if (!fn || !ff7_x86_regs()) {
        l_warn("port menu: %s not found, dialog skip off", MESSAGE_LOOP_SYM);
        broken = 1;
        return;
    }
    profiler_hold();
    message_hook = hook_addr(fn, (uintptr_t) &message_loop_seen);
    profiler_release();
    installed = 1;
    l_info("port menu: dialog skip hook on %s", MESSAGE_LOOP_SYM);
}

// Main thread, every pad poll: Cross to add for the dialog skip (0: none)
static uint32_t dialog_skip_buttons(void) {
    static int phase, presses[NUM_WINDOWS];
    static uint16_t last_mode[NUM_WINDOWS];
    static uint32_t last_ptr[NUM_WINDOWS];
    if (!auto_skip || game_mode != MODE_FIELD) return 0;
    uint32_t t = now_ms();
    int want = 0;
    for (int w = 0; w < NUM_WINDOWS; w++) {
        uint32_t seen = message_seen_ms[w];
        uint8_t * win = x86_span(VA_WINDOWS + 0x30 * w + 0x2c, 2), * ptr = x86_span(VA_DIALOG_PTR + 4 * w, 4);
        if (!seen || t - seen > 150 || !win || !ptr) {
            presses[w] = 0;
            continue;
        }
        uint16_t m = get16(win);
        uint32_t p = (uint32_t) get32(ptr);
        if (m == 0 || m != last_mode[w] || p != last_ptr[w]) presses[w] = 0;
        last_mode[w] = m;
        last_ptr[w] = p;
        if (m != 0 && m != 7 && presses[w] < 20) { // 7: closing
            want = 1;
            if (phase == 0) presses[w]++;
        }
    }
    if (!want) {
        phase = 0;
        return 0;
    }
    int down = phase < 3;
    phase = (phase + 1) % 6;
    return down ? SCE_CTRL_CROSS : 0;
}

static void apply_cheats(int mode) {
    if (cheat_hpmp) refill_hpmp(mode);
    scale_rewards(mode);
    scale_field_speed();
    scale_world_speed();
    install_dialog_skip();

    int m = materia_request;
    if (m != -1) {
        materia_request = -1;
        if (mode != MODE_FIELD && mode != MODE_WORLD) {
            materia_result = 3;
            l_warn("port menu: materia 0x%02X not added (mode %d)", m & 0xff, mode);
        } else {
            int slot = add_materia((uint32_t) m);
            materia_result = slot >= 0 ? 1 : 2;
            l_info("port menu: materia 0x%02X, AP 0x%X: %s %d", m & 0xff, (uint32_t) m >> 8,
                   slot >= 0 ? "slot" : "inventory full", slot);
        }
    }
}

// ---------------------------------------------------------------- game thread

void port_menu_apply_ab(void) {
    for (int i = 0; i < 3; i++) {
        int w = ab_want[i];
        if (w == ab_applied[i]) continue;
        ab_applied[i] = w;
        if (i == 0) {
            ff7_coaster_set(w);
            ff7_submarine_set(w);
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
    apply_cheats(mode);

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

    if (title_request) {
        static int title_tries;
        uint8_t * c = mode == MODE_FIELD ? control_block() : NULL;
        if (c && c[1] == 0) {
            put16(c + 0x26, 0);
            c[1] = EXIT_GAMEOVER;
            title_request = 0;
            title_tries = 0;
            l_info("port menu: back to the title screen (field %d, as the GAMEOVER opcode)",
                   x86(VA_FIELD_ID) ? get16(x86(VA_FIELD_ID)) : -1);
        } else if (++title_tries > 600) {
            l_warn("port menu: back to the title not done (mode %d, field busy)", mode);
            title_request = 0;
            title_tries = 0;
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

    int v = world_request;
    if (v >= 0) {
        static int world_tries;
        const vehicle * vh = &vehicles[v];
        uint8_t * c = mode == MODE_FIELD ? control_block() : NULL;
        uint8_t * flags = x86(vh->flag_va), * entry = x86(VA_WM_ENTRY), * nob = x86(VA_WM_NO_BUGGY);
        if (c && c[1] == 0 && flags && entry) {
            uint8_t old = *flags;
            *flags = (uint8_t) ((old & ~vh->clear) | vh->set);
            int32_t type;
            memcpy(&type, entry, 4);
            if (type != 0) {
                int32_t zero = 0;
                memcpy(entry, &zero, 4);
            }
            put16(c + 0x02, vh->entry);
            put16(c + 0x04, 0);
            put16(c + 0x06, 0);
            put16(c + 0x22, 0);
            put16(c + 0x24, 0);
            put16(c + 0x26, 0);
            c[1] = EXIT_MAPJUMP; // the field leaves for the world map; no second step, as for the real opcode
            l_info("port menu: world map in the %s: field %d, entry %d, save byte 0x%X 0x%02X -> 0x%02X, entry type %d -> 0",
                   vh->name, x86(VA_FIELD_ID) ? get16(x86(VA_FIELD_ID)) : -1, vh->entry, (unsigned) vh->flag_va, old,
                   *flags, (int) type);
            if (v == 0 && nob && (*nob & 0x80))
                l_warn("port menu: script bit 0xE7F is set (0x%X = 0x%02X): the world map will not create the buggy",
                       (unsigned) VA_WM_NO_BUGGY, *nob);
            world_request = -1;
            world_tries = 0;
        } else if (++world_tries > 600) {
            l_warn("port menu: world map in the %s not reached (mode %d, field busy)", vh->name, mode);
            world_request = -1;
            world_tries = 0;
        }
    }

    if (summons_request) {
        summons_request = 0;
        uint8_t * inv = x86(VA_MATERIA);
        if (!inv || (mode != MODE_FIELD && mode != 3)) {
            l_warn("port menu: summon materia not added (mode %d)", mode);
        } else {
            int have[SUMMON_LAST - SUMMON_FIRST + 1] = { 0 }, added = 0, full = 0;
            for (int i = 0; i < NUM_MATERIA; i++) {
                uint32_t m = (uint32_t) get32(inv + 4 * i);
                if (m != 0xffffffffu && (m & 0xff) >= SUMMON_FIRST && (m & 0xff) <= SUMMON_LAST)
                    have[(m & 0xff) - SUMMON_FIRST] = 1;
            }
            int slot = 0;
            for (int id = SUMMON_FIRST; id <= SUMMON_LAST; id++) {
                if (have[id - SUMMON_FIRST]) continue;
                while (slot < NUM_MATERIA && (uint32_t) get32(inv + 4 * slot) != 0xffffffffu) slot++;
                if (slot == NUM_MATERIA) {
                    full++;
                    continue;
                }
                uint32_t m = (uint32_t) id | AP_MASTER << 8;
                memcpy(inv + 4 * slot, &m, 4);
                added++;
            }
            l_info("port menu: summon materia: %d added (mastered), %d already in the inventory, %d without a free slot",
                   added, SUMMON_LAST - SUMMON_FIRST + 1 - added - full, full);
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

static int step(int v, int n, int dir) { return (v + (dir < 0 ? n - 1 : 1)) % n; }

static const char * factor_name(int f) {
    static const char * const names[] = { "NORMAL", "2X", "3X", "4X" };
    return names[clamp_factor(f) - 1];
}

// LEFT / RIGHT on an item (dir -1 / 1)
static void change(int item, int dir) {
    char m[64];
    m[0] = 0;
    switch (item) {
        case ITEM_GOTO:
            if (num_scenes) scene_cursor = step(scene_cursor, num_scenes, dir);
            return;
        case ITEM_WORLD:
            vehicle_cursor = step(vehicle_cursor, NUM_VEHICLES, dir);
            return;
        case ITEM_SUMMON_PICK:
            summon_cursor = step(summon_cursor, NUM_SUMMONS, dir);
            return;
        case ITEM_COLOR:
            color_cursor = step(color_cursor, NUM_COLORS, dir);
            materia_cursor = 0;
            return;
        case ITEM_MATERIA_PICK:
            materia_cursor = step(materia_cursor, materia_colors[color_cursor].count, dir);
            return;
        case ITEM_HPMP:
            cheat_hpmp = !cheat_hpmp;
            snprintf(m, sizeof(m), "INFINITE HP AND MP: %s", cheat_hpmp ? "ON" : "OFF");
            break;
        case ITEM_LEVEL:
            materia_max = !materia_max;
            save_cheats();
            return;
        case ITEM_EXP:
            cheat_exp = step(cheat_exp - 1, 4, dir) + 1;
            snprintf(m, sizeof(m), "EXP: %s (FROM THE NEXT BATTLE)", factor_name(cheat_exp));
            break;
        case ITEM_GIL:
            cheat_gil = step(cheat_gil - 1, 4, dir) + 1;
            snprintf(m, sizeof(m), "GIL: %s (FROM THE NEXT BATTLE)", factor_name(cheat_gil));
            break;
        case ITEM_FIELD_SPEED:
            speed_field = step(speed_field - 1, 4, dir) + 1;
            break;
        case ITEM_WORLD_SPEED:
            speed_world = step(speed_world - 1, 4, dir) + 1;
            break;
        case ITEM_LANGUAGE:
            language = step(language + 1, NUM_LANGUAGES + 1, dir) - 1;
            snprintf(m, sizeof(m), "DIALOG LANGUAGE: RESTART THE GAME TO APPLY");
            break;
        case ITEM_AUTO_SKIP:
            auto_skip = !auto_skip;
            snprintf(m, sizeof(m), "DIALOG SKIP: %s (NOT QUESTIONS)", auto_skip ? "ON" : "OFF");
            break;
        default:
            return;
    }
    if (m[0]) set_message(m);
    l_info("port menu: cheats: infinite HP/MP %d, EXP x%d, gil x%d; speed: field x%d, world map x%d; dialog skip %d",
           cheat_hpmp, cheat_exp, cheat_gil, speed_field, speed_world, auto_skip);
    save_cheats();
}

static void activate(int item) {
    if (item < NUM_GAMES) {
        if (game_mode != MODE_FIELD) {
            char m[64];
            snprintf(m, sizeof(m), "ONLY FROM A FIELD (MODE %d)", game_mode);
            set_message(m);
            return;
        }
        request = item;
        char m[64];
        snprintf(m, sizeof(m), "STARTING %s", games[item].name);
        set_message(m);
        open = 0;
        wait_release = 1;
    } else if (item == ITEM_RECORD) {
        if (recording >= 0) {
            profiler_set_paused(1);
            l_info("port menu: scene %d recording stopped (profiler paused)", recording + 1);
            char m[64];
            snprintf(m, sizeof(m), "SCENE %d SAVED: SEND PROFILE.BIN AND LOGS.TXT", recording + 1);
            set_message(m);
            recording = -1;
        } else if (num_scenes >= MAX_SCENES) {
            set_message("SCENE LIMIT: EDIT CENAS.TXT");
        } else {
            record_request = 1;
            set_message("RECORDING SCENE: PROFILER ON");
            open = 0;
            wait_release = 1;
        }
    } else if (item == ITEM_GOTO) {
        if (!num_scenes) {
            set_message("NO SCENE RECORDED");
        } else if (game_mode != MODE_FIELD) {
            set_message("ONLY FROM A FIELD");
        } else {
            goto_request = scene_cursor;
            char m[64];
            snprintf(m, sizeof(m), "GOING TO SCENE %d", scene_cursor + 1);
            set_message(m);
            open = 0;
            wait_release = 1;
        }
    } else if (item == ITEM_WORLD) {
        if (game_mode != MODE_FIELD) {
            set_message("ONLY FROM A FIELD");
        } else {
            world_request = vehicle_cursor;
            char m[64];
            snprintf(m, sizeof(m), "WORLD MAP: %s (DO NOT SAVE AFTER)", vehicles[vehicle_cursor].name);
            set_message(m);
            open = 0;
            wait_release = 1;
        }
    } else if (item == ITEM_SUMMONS) {
        if (game_mode != MODE_FIELD && game_mode != 3) {
            set_message("ONLY IN A FIELD OR ON THE WORLD MAP");
        } else {
            summons_request = 1;
            set_message("SUMMONS IN THE INVENTORY (SEE THE LOG)");
        }
    } else if (item >= ITEM_AB_NATIVES && item <= ITEM_AB_TEXCACHE) {
        int k = item - ITEM_AB_NATIVES;
        ab_want[k] = !ab_want[k];
        static const char * const what[3] = { "MINIGAME NATIVES", "30 FPS CAP", "TEXTURE CACHE" };
        char m[64];
        snprintf(m, sizeof(m), "%s: %s", what[k], ab_want[k] ? "ON" : "OFF");
        set_message(m);
        l_info("port menu: A/B %s %s (mode %d)", what[k], ab_want[k] ? "on" : "off", game_mode);
    } else if (item == ITEM_AUTO) {
        auto_profile = !auto_profile;
        l_info("port menu: automatic profiling %s", auto_profile ? "on" : "off");
    } else if (item == ITEM_TITLE) {
        if (game_mode != MODE_FIELD) {
            set_message("ONLY FROM A FIELD");
        } else if (!title_armed) {
            title_armed = 1;
            set_message("PRESS AGAIN: UNSAVED PROGRESS IS LOST");
        } else {
            title_armed = 0;
            title_request = 1;
            set_message("BACK TO THE TITLE: CHOOSE CONTINUE");
            open = 0;
            wait_release = 1;
        }
    } else if (item == ITEM_PROFILER) {
        if (profiler_active()) {
            profiler_set_paused(1);
            set_message("PROFILER PAUSED");
        } else {
            profiler_start_now();
            set_message("PROFILER ON: PROFILE.BIN");
            l_info("port menu: profiler on (manual, mode %d)", game_mode);
        }
    } else if (item == ITEM_SUMMON_PICK || item == ITEM_MATERIA_PICK) {
        const materia_name * mn = item == ITEM_SUMMON_PICK ? &mat_red[summon_cursor]
                                                           : &materia_colors[color_cursor].list[materia_cursor];
        if (game_mode != MODE_FIELD && game_mode != MODE_WORLD) {
            set_message("ONLY IN A FIELD OR ON THE WORLD MAP");
        } else {
            materia_result = 0;
            materia_request = (int) ((uint32_t) mn->id | (materia_max ? AP_MASTER : 0) << 8);
            char m[64];
            snprintf(m, sizeof(m), "%s (%s)...", mn->name, materia_max ? "MAX" : "LEVEL 1");
            set_message(m);
        }
    } else if (item == ITEM_CLOSE) {
        open = 0;
        wait_release = 1;
    } else {
        change(item, 1); // the options: X steps them as RIGHT
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
            title_armed = 0;
            cursor = 0;
            return 0; // the game sees every held button go up
        }
        // L + R held: the Start of the combo is not passed on
        if ((buttons & BTN_L) && (buttons & BTN_R)) buttons &= ~SCE_CTRL_START;
        return buttons | dialog_skip_buttons();
    }

    const tab * t = &tabs[cur_tab];
    if (combo || (pressed & SCE_CTRL_CIRCLE)) {
        open = 0;
        wait_release = 1;
    } else if (pressed & (BTN_L | BTN_R)) { // L / R: tab
        cur_tab = step(cur_tab, NUM_TABS, pressed & BTN_L ? -1 : 1);
        cursor = 0;
        title_armed = 0;
    } else if (pressed & SCE_CTRL_UP) {
        cursor = step(cursor, t->count, -1);
        title_armed = 0;
    } else if (pressed & SCE_CTRL_DOWN) {
        cursor = step(cursor, t->count, 1);
        title_armed = 0;
    } else if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
        change(t->items[cursor], pressed & SCE_CTRL_LEFT ? -1 : 1);
    } else if (pressed & SCE_CTRL_CROSS) {
        activate(t->items[cursor]);
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
#define LINE_H  22 // 18 items fit in 544 lines
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

    // Result of a materia added by the game thread
    static const char * const materia_results[] = { NULL, "MATERIA IN THE INVENTORY", "MATERIA INVENTORY FULL",
                                                    "ONLY IN A FIELD OR ON THE WORLD MAP" };
    int mr = materia_result;
    if (mr > 0 && mr <= 3) {
        materia_result = 0;
        set_message(materia_results[mr]);
    }

    int show_message = message[0] && now < message_until;
    if (!open && !show_message) return;

    char line[64];
    if (open) {
        const tab * t = &tabs[cur_tab];
        const float x0 = 120, y0 = 16, x1 = 840; // 17 items + the tabs: the box ends at 530 of 544 lines
        const float y1 = y0 + 30 + (MAX_TAB_ITEMS + 5) * LINE_H;
        rect(x0, y0, x1, y1, RGBA(8, 12, 32, 225));
        rect(x0, y0, x1, y0 + 3, RGBA(120, 150, 230, 255));
        float y = y0 + 15;
        text(x0 + 20, y, "PORT MENU", RGBA(255, 255, 255, 255));
        y += LINE_H + 2;
        // Tabs: L  CHEATS  GAMEPLAY  DEBUG  R
        float tx = x0 + 20;
        text(tx, y, "L", RGBA(170, 190, 230, 255));
        tx += 2 * ADVANCE;
        for (int i = 0; i < NUM_TABS; i++) {
            float w = (float) strlen(tabs[i].name) * ADVANCE;
            if (i == cur_tab) rect(tx - 6, y - 5, tx + w, y + 7 * SCALE + 5, RGBA(60, 90, 170, 255));
            text(tx, y, tabs[i].name, i == cur_tab ? RGBA(255, 255, 255, 255) : RGBA(130, 140, 170, 255));
            tx += w + ADVANCE;
        }
        text(tx, y, "R", RGBA(170, 190, 230, 255));
        y += LINE_H;
        snprintf(line, sizeof(line), "MODE %d   %.1f FPS   PROFILER %s", game_mode, fps,
                 profiler_active() ? "ON" : "OFF");
        text(x0 + 20, y, line, RGBA(170, 190, 230, 255));
        y += LINE_H + 6;
        for (int n = 0; n < t->count; n++, y += LINE_H) {
            int i = t->items[n];
            const char * label = line;
            if (i < NUM_GAMES) label = games[i].name;
            else if (i == ITEM_HPMP) snprintf(line, sizeof(line), "INFINITE HP AND MP: %s", cheat_hpmp ? "YES" : "NO");
            else if (i == ITEM_EXP) snprintf(line, sizeof(line), "BATTLE EXP < %s >", factor_name(cheat_exp));
            else if (i == ITEM_GIL) snprintf(line, sizeof(line), "BATTLE GIL < %s >", factor_name(cheat_gil));
            else if (i == ITEM_LEVEL)
                snprintf(line, sizeof(line), "MATERIA LEVEL < %s >", materia_max ? "MAX" : "1 (NO AP)");
            else if (i == ITEM_SUMMON_PICK) snprintf(line, sizeof(line), "SUMMON < %s >", mat_red[summon_cursor].name);
            else if (i == ITEM_COLOR) snprintf(line, sizeof(line), "COLOR < %s >", materia_colors[color_cursor].name);
            else if (i == ITEM_MATERIA_PICK)
                snprintf(line, sizeof(line), "MATERIA < %s > %d/%d", materia_colors[color_cursor].list[materia_cursor].name,
                         materia_cursor + 1, materia_colors[color_cursor].count);
            else if (i == ITEM_FIELD_SPEED)
                snprintf(line, sizeof(line), "FIELD SPEED < %s >", factor_name(speed_field));
            else if (i == ITEM_WORLD_SPEED)
                snprintf(line, sizeof(line), "WORLD MAP SPEED ON FOOT < %s >", factor_name(speed_world));
            else if (i == ITEM_LANGUAGE)
                snprintf(line, sizeof(line), "DIALOG LANGUAGE < %s >%s", language < 0 ? "VITA SYSTEM" : language_names[language],
                         language != language_loaded ? " *" : "");
            else if (i == ITEM_AUTO_SKIP)
                snprintf(line, sizeof(line), "AUTO SKIP DIALOGS: %s", auto_skip ? "YES" : "NO");
            else if (i == ITEM_RECORD) {
                if (recording >= 0) snprintf(line, sizeof(line), "RECORDING SCENE %d: STOP", recording + 1);
                else snprintf(line, sizeof(line), "RECORD SCENE (MARKER + PROFILER)");
            } else if (i == ITEM_GOTO) {
                if (!num_scenes) snprintf(line, sizeof(line), "GO TO SCENE: NONE");
                else if (scenes[scene_cursor].mode == MODE_FIELD)
                    snprintf(line, sizeof(line), "GO TO SCENE < %d/%d > MAP %d", scene_cursor + 1, num_scenes,
                             scenes[scene_cursor].map);
                else
                    snprintf(line, sizeof(line), "GO TO SCENE < %d/%d > MODE %d", scene_cursor + 1, num_scenes,
                             scenes[scene_cursor].mode);
            } else if (i == ITEM_WORLD) {
                snprintf(line, sizeof(line), "WORLD MAP < %s >", vehicles[vehicle_cursor].name);
            } else if (i == ITEM_SUMMONS) {
                label = "ALL SUMMON MATERIA (MASTERED)";
            } else if (i == ITEM_AB_NATIVES) {
                snprintf(line, sizeof(line), "A/B MINIGAME NATIVES: %s", ab_want[0] ? "YES" : "NO");
            } else if (i == ITEM_AB_CAP) {
                snprintf(line, sizeof(line), "A/B 30 FPS CAP: %s", ab_want[1] ? "YES" : "NO");
            } else if (i == ITEM_AB_TEXCACHE) {
                snprintf(line, sizeof(line), "A/B TEXTURE CACHE: %s", ab_want[2] ? "YES" : "NO");
            } else if (i == ITEM_AUTO) snprintf(line, sizeof(line), "AUTO PROFILE IN MINIGAMES: %s", auto_profile ? "YES" : "NO");
            else if (i == ITEM_PROFILER) snprintf(line, sizeof(line), "PROFILER NOW: %s", profiler_active() ? "PAUSE" : "START");
            else if (i == ITEM_TITLE) label = title_armed ? "BACK TO THE TITLE: CONFIRM" : "BACK TO THE TITLE (LOAD A SAVE)";
            else label = "CLOSE";
            if (n == cursor) rect(x0 + 10, y - 6, x1 - 10, y + 7 * SCALE + 6, RGBA(60, 90, 170, 255));
            int needs_field = i < NUM_GAMES || i == ITEM_GOTO || i == ITEM_WORLD || i == ITEM_TITLE;
            int adds_materia = i == ITEM_SUMMONS || i == ITEM_SUMMON_PICK || i == ITEM_MATERIA_PICK;
            int greyed = (needs_field && game_mode != MODE_FIELD)
                         || (adds_materia && game_mode != MODE_FIELD && game_mode != MODE_WORLD);
            uint32_t color = greyed ? RGBA(130, 130, 140, 255) : RGBA(255, 255, 255, 255);
            text(x0 + 30, y, label, color);
        }
        y += 6;
        if (cur_tab == 0) {
            text(x0 + 20, y, "X: OK  O: EXIT  LEFT/RIGHT: CHANGE", RGBA(170, 190, 230, 255));
            text(x0 + 20, y + LINE_H, "X ON SUMMON/MATERIA: ADD IT", RGBA(170, 190, 230, 255));
        } else if (cur_tab == 1) {
            text(x0 + 20, y, "X: OK  O: EXIT  LEFT/RIGHT: CHANGE", RGBA(170, 190, 230, 255));
        } else {
            text(x0 + 20, y, "X: OK  O: EXIT  < >: SCENE/VEHICLE", RGBA(170, 190, 230, 255));
            if (game_mode != MODE_FIELD)
                text(x0 + 20, y + LINE_H, "MINIGAMES: ONLY FROM A FIELD", RGBA(230, 190, 120, 255));
        }
    }
    if (show_message) {
        int w = (int) strlen(message) * ADVANCE + 40;
        rect(480 - w / 2, 480, 480 + w / 2, 480 + 7 * SCALE + 24, RGBA(8, 12, 32, 225));
        text(480 - w / 2 + 20, 492, message, RGBA(255, 255, 255, 255));
    }
    flush_draw();
}
