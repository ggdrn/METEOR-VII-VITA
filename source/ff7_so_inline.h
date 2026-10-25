/*
 * What native code needs to inline mk_address and _check_event (see
 * ff7_chocobo_core.h): mk_address's page table and _check_event's call
 * counter, read from the PC-relative loads at the start of the two
 * functions and checked against their known places in libjni_ff7.so 1.0.29.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_SO_INLINE_H
#define FF7_SO_INLINE_H

#include <stdint.h>

#include <so_util/so_util.h>

#define FF7_MEMMAP_OFF   0x101de5c // the page table (static array)
#define FF7_EVCOUNT_OFF  0x1492b78 // _check_event's counter

/* 1 and the two pointers if mk_address and _check_event are the expected
 * code, else 0. mk and ce: their addresses (Thumb bit allowed). */
static inline int ff7_so_inline_lookup(const so_module * mod, uintptr_t mk, uintptr_t ce, uint8_t * const ** memmap,
                                       uint32_t ** ev_count) {
    mk &= ~1u;
    ce &= ~1u;
    // mk_address: cbz r0; ldr r3, [pc, #44]; lsrs r2, r0, #12; add r3, pc; ldr r3, [r3]
    // _check_event: push {r3-r5, lr}; ldr r4, [pc, #60]; add r4, pc; ldr r3, [r4]
    if (*(uint32_t *) mk != 0x4b0bb170 || *(uint32_t *) (mk + 4) != 0x447b0b02
        || *(uint32_t *) ce != 0x4c0fb538 || *(uint16_t *) (ce + 4) != 0x447c)
        return 0;
    uintptr_t got = mk + 6 + 4 + *(uint32_t *) (mk + 0x30); // add r3, pc at +6
    uintptr_t cnt = ce + 4 + 4 + *(uint32_t *) (ce + 0x40); // add r4, pc at +4
    uint8_t * const * table = *(uint8_t * const **) got;
    if ((uintptr_t) table != mod->text_base + FF7_MEMMAP_OFF || cnt != mod->text_base + FF7_EVCOUNT_OFF) return 0;
    *memmap = table;
    *ev_count = (uint32_t *) cnt;
    return 1;
}

#endif // FF7_SO_INLINE_H
