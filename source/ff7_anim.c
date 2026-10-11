/*
 * Native bit reader for battle animation decompression in libjni_ff7.so.
 *
 * ff7.exe's function at 0x5E7C40 reads `n` bits MSB-first from a byte
 * stream and sign-extends them:
 *
 *     int read_bits(const uint8_t * data, int * bit_pos, int n)
 *
 * It is the inner loop of the battle model animation decoder (0x5E7680) and,
 * recompiled to ARM, every bit costs dozens of emulated instructions and
 * several mk_address calls: profiling showed it dominating battle loading.
 *
 * Recompiled x86 functions are `void f(void)` working on a global register
 * file: +0 EAX, +4 ECX, +8 EDX, +16 ESP, +20 EBP, +24 ESI. Callers push the
 * arguments and a return address on the emulated stack; returning pops it
 * (ESP += 4). The replacement keeps the observable effects of the original:
 * EAX/ECX/EDX results, *bit_pos advanced by n, _check_event() once at the end.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_anim.h"

#include "utils/logger.h"

#include <stdint.h>

#include <kubridge.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define SYM_READ_BITS   "_Z15func_0x005e7c40v"
#define SYM_CHECK_EVENT "_Z12_check_eventv"
#define SYM_MK_ADDRESS  "mk_address"

// func_0x005e7c40: offset, first instruction, and the literal that leads to
// the register file pointer (ldr r5, [pc, #656] / add r5, pc at +0x0e)
#define READ_BITS_OFFSET   0x17a21c
#define READ_BITS_PROLOGUE 0x43f8e92d // stmdb sp!, {r3-r9, lr}
#define REGS_LITERAL       0x17a4b8
#define REGS_PC            0x17a22e

enum { EAX = 0, ECX = 1, EDX = 2, ESP = 4 };

typedef void * (* mk_address_fn)(uint32_t addr);
typedef void (* check_event_fn)(void);

static mk_address_fn mk_address;
static check_event_fn check_event;
static volatile uint32_t * regs;
static so_hook read_bits_hook;

#ifdef ENABLE_SELFTESTS
#define SELFTEST_CALLS 2000
#else
#define SELFTEST_CALLS 0
#endif
static int selftest_left = SELFTEST_CALLS;
static unsigned long long calls;

static inline uint32_t stack_arg(uint32_t esp, int index) {
    return *(uint32_t *) mk_address(esp + 4 * index);
}

// Pure computation of the original: value, ECX and EDX it leaves behind
static int32_t read_bits(const uint8_t * data, int32_t pos, int32_t n, uint32_t * ecx, uint32_t * edx) {
    int32_t v = 0;
    for (int32_t i = 0; i < n; i++, pos++) {
        // x86: byte index = pos / 8 rounded toward zero (cdq; and edx,7; add; sar 3)
        int32_t byte = (pos + ((pos >> 31) & 7)) >> 3;
        int bit = 7 - (pos & 7);
        v = (int32_t) ((uint32_t) v << 1);
        if (data[byte] & (1 << bit)) v++;
    }
    uint32_t sh = (uint32_t) (32 - n) & 31; // x86 masks shift counts to 5 bits
    uint32_t shifted = (uint32_t) v << sh;
    *ecx = (uint32_t) (32 - n);
    *edx = shifted;
    return (int32_t) shifted >> sh;
}

static void read_bits_native(void) {
    uint32_t esp = regs[ESP];
    const uint8_t * data = mk_address(stack_arg(esp, 1));
    int32_t * pos = mk_address(stack_arg(esp, 2));
    int32_t n = (int32_t) stack_arg(esp, 3);

    uint32_t ecx, edx;
    int32_t start = *pos;
    int32_t value = read_bits(data, start, n, &ecx, &edx);
    calls++;

    if (selftest_left > 0) {
        // Let the original do the real work and compare
        selftest_left--;
        SO_CONTINUE(int, read_bits_hook);
        if ((int32_t) regs[EAX] != value || *pos != start + (n > 0 ? n : 0)
            || regs[ECX] != ecx || regs[EDX] != edx) {
            l_error("ff7_anim selftest MISMATCH: pos %d n %d: native %d/%u/%u, original %d/%u/%u (pos %d)",
                    start, n, value, ecx, edx, (int32_t) regs[EAX], regs[ECX], regs[EDX], *pos);
            selftest_left = 0;
        } else if (selftest_left == 0) {
            l_info("ff7_anim selftest ok: %d calls identical", SELFTEST_CALLS);
        }
        return;
    }

    if (n > 0) *pos = start + n;
    regs[EAX] = (uint32_t) value;
    regs[ECX] = ecx;
    regs[EDX] = edx;
    check_event();
    regs[ESP] = esp + 4; // ret
}

void ff7_anim_patch(void) {
    uintptr_t base = so_mod.text_base;
    uintptr_t fn = so_symbol(&so_mod, SYM_READ_BITS);
    check_event = (check_event_fn) so_symbol(&so_mod, SYM_CHECK_EVENT);
    mk_address = (mk_address_fn) so_symbol(&so_mod, SYM_MK_ADDRESS);

    if (!fn || !check_event || !mk_address || (fn & ~1) != base + READ_BITS_OFFSET
        || *(uint32_t *) (base + READ_BITS_OFFSET) != READ_BITS_PROLOGUE) {
        l_warn("ff7_anim: unexpected .so layout, native bit reader disabled");
        return;
    }

    uint32_t literal = *(uint32_t *) (base + REGS_LITERAL);
    regs = *(volatile uint32_t **) (base + REGS_PC + literal);

    read_bits_hook = hook_addr(fn, (uintptr_t) &read_bits_native);
    l_info("ff7_anim: native read_bits installed (x86 registers at %p)", regs);
}

void ff7_anim_report(double seconds) {
    if (seconds <= 0 || !calls) return;
    l_info("anim read_bits/s: %.0f", calls / seconds);
    calls = 0;
}
