/* Native MSVC _ftol (x86 0x40B470, Android ARM offset 0xAFCD74).
 * The highway profile attributes 477/7102 game-thread samples to this
 * conversion wrapper, excluding its memory translation/runtime callees.
 * Keep the complete x86 state, stack scratch writes and event ordering.
 * Equivalence harness: scripts/emu/test_ftol.py, compiled Cortex-A9 core.
 */
#include "ff7_ftol.h"
#include "ff7_ftol_core.h"
#include "ff7_anim.h"
#include "ff7_selftest.h"
#include "ff7_unhook.h"
#include "utils/logger.h"
#include "utils/profiler.h"
#include "utils/utils.h"

#include <so_util/so_util.h>

extern so_module so_mod;

#define SYM_FTOL "_Z15func_0x0040b470v"
#define FTOL_OFFSET 0xafcd74u
#define FPU_GOT 0xf4c82cu
#define OFF_SWITCH DATA_PATH "ftol_off.txt"

static so_hook ftol_hook;
static volatile uint32_t *regs;
static uint8_t *fpu;
static uint8_t *(*mk_address)(uint32_t);
static void (*check_event)(void);
static int disabled, selftest_left, selftest_total;
static unsigned long long calls, fallbacks;

static void original(void) {
    /* SO_CONTINUE rewrites the original entry. Do not race the profiler's
     * text permission changes, including during the optional selftest. */
    profiler_hold();
    SO_CONTINUE(int, ftol_hook);
    profiler_release();
}

static void ftol_native(void) {
    uint32_t esp = regs[4], top;
    memcpy(&top, fpu + 0x60, 4);
    if (disabled || top > 7 || esp < 16) {
        fallbacks++;
        original();
        return;
    }
    uint8_t *frame = mk_address(esp - 16);
    if (((esp - 16) >> 12) != ((esp - 1) >> 12)
        && (uintptr_t)mk_address(esp - 1) != (uintptr_t)frame + 15) {
        fallbacks++;
        original();
        return;
    }

    if (selftest_left > 0) {
        uint32_t r[8];
        uint8_t f[0x68], scratch[16];
        for (unsigned i = 0; i < 8; i++) r[i] = regs[i];
        memcpy(f, fpu, sizeof(f));
        memcpy(scratch, frame, sizeof(scratch));
        ff7_ftol_core(r, f, scratch);
        r[4] += 4;
        original();
        int ok = !memcmp(f, fpu, sizeof(f)) && !memcmp(scratch, frame, sizeof(scratch));
        for (unsigned i = 0; i < 8; i++) if (r[i] != regs[i]) ok = 0;
        selftest_left--;
        if (!ok) {
            disabled = 1;
            ff7_unhook(&ftol_hook); // later calls go straight to the original
            l_error("ff7_ftol selftest MISMATCH: expected eax/edx/esp %08x/%08x/%08x, original %08x/%08x/%08x; disabled",
                    r[0], r[2], r[4], regs[0], regs[2], regs[4]);
        } else if (!selftest_left) {
            l_info("ff7_ftol selftest ok: %d calls (registers, x87 and stack identical)", selftest_total);
        }
        return; /* original's result is authoritative while checking */
    }

    ff7_ftol_core(regs, fpu, frame);
    check_event();
    regs[4] += 4;
    calls++;
}

void ff7_ftol_patch(void) {
    if (file_exists(OFF_SWITCH)) {
        l_info("ff7_ftol: disabled by %s", OFF_SWITCH);
        return;
    }
    uintptr_t fn = so_symbol(&so_mod, SYM_FTOL);
    const uint32_t entry[2] = {0x41f0e92du, 0x4c6a4d69u};
    regs = ff7_x86_regs();
    mk_address = (void *)so_symbol(&so_mod, "mk_address");
    check_event = (void *)so_symbol(&so_mod, "_Z12_check_eventv");
    if (!fn || (fn & ~(uintptr_t)1) != so_mod.text_base + FTOL_OFFSET
        || memcmp((const void *)(fn & ~(uintptr_t)1), entry, sizeof(entry))
        || !regs || !mk_address || !check_event) {
        l_warn("ff7_ftol: unexpected .so layout; original conversion retained");
        return;
    }
    fpu = *(uint8_t **)(so_mod.text_base + FPU_GOT);
    if ((uintptr_t)fpu != so_mod.text_base + 0x1492bc0u) {
        l_warn("ff7_ftol: unexpected x87 state; original conversion retained");
        return;
    }
    /* Always check a short live sample; the diagnostic switch extends it. */
    selftest_total = selftest_left = ff7_selftests_on ? 512 : 64;
    ftol_hook = hook_addr(fn, (uintptr_t)&ftol_native);
    l_info("ff7_ftol: native x87 integer conversion installed (selftest on %d calls)", selftest_total);
}

void ff7_ftol_report(double seconds) {
    if (seconds > 0 && (calls || fallbacks))
        l_info("x87 integer conversions/s: %.0f native, %.0f original", calls / seconds, fallbacks / seconds);
    calls = fallbacks = 0;
}
