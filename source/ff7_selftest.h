/*
 * Runtime switch for the native replacements' self-tests.
 *
 * The self-tests run the original x86 code next to the native version on the
 * first calls and compare the results. They were run on every boot until
 * v0.2, adding work during the first minutes of play and the first battle;
 * now they only run when DATA_PATH "selftest_on.txt" exists.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_SELFTEST_H
#define FF7_SELFTEST_H

#include <stdbool.h>

#define SELFTEST_SWITCH DATA_PATH "selftest_on.txt"

/** Set by so_patch() before the ff7_*_patch() calls. */
extern bool ff7_selftests_on;

/** Number of self-test calls to run: `n` if self-tests are on, else 0. */
static inline int ff7_selftest_count(int n) {
    return ff7_selftests_on ? n : 0;
}

#endif // FF7_SELFTEST_H
