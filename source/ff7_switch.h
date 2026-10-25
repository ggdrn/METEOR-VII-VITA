/*
 * Off switches of the native replacements.
 *
 * Every native module has its own DATA_PATH "<name>_off.txt"; DATA_PATH
 * "natives_off.txt" turns all the drawing/math natives off at once (to tell a
 * port bug from the game's own behavior, then find the module with the
 * individual switches).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_SWITCH_H
#define FF7_SWITCH_H

#include "utils/logger.h"
#include "utils/utils.h"

#define FF7_ALL_NATIVES_OFF DATA_PATH "natives_off.txt"

/* 1 if `file` or natives_off.txt exists (logged as `module: disabled by ...`) */
static inline int ff7_switch_off(const char * module, const char * file) {
    const char * why = file_exists(file) ? file : file_exists(FF7_ALL_NATIVES_OFF) ? FF7_ALL_NATIVES_OFF : NULL;
    if (why) l_info("%s: disabled by %s", module, why);
    return why != NULL;
}

#endif // FF7_SWITCH_H
