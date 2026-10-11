/*
 * Native LZSS decompressor for libjni_ff7.so.
 *
 * ff7.exe's function at 0x6305C0 unpacks the game's LZSS data (field files
 * and others) into a buffer:
 *
 *     void lzss_decompress(const uint8_t * src, uint8_t * dst)
 *
 * src starts with the compressed size (u32). Then: a flag byte for every 8
 * items, LSB first; 1 = literal byte, 0 = reference of 2 bytes (12-bit
 * offset into a 4 KB ring that starts at 0xFEE, 4-bit length + 3). Recompiled
 * to ARM, each byte costs dozens of emulated instructions and mk_address
 * calls: profiling showed it dominating the stutters when changing areas.
 *
 * Calling convention and register file: see ff7_anim.c. The replacement
 * leaves EAX/ECX/EDX as the original does (traced from the x86 code), pops
 * the frame, calls _check_event() and returns (ESP += 4).
 *
 * x86 addresses are translated by mk_address page by page (4 KB pages that
 * need not be contiguous on the host).
 *
 * The first calls also run the original and compare the output; on a
 * mismatch the native version disables itself.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_lzss.h"
#include "ff7_anim.h"

#include "utils/logger.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define SYM_LZSS        "_Z15func_0x006305c0v"
#define SYM_CHECK_EVENT "_Z12_check_eventv"
#define SYM_MK_ADDRESS  "mk_address"
#define LZSS_PROLOGUE   0x41f0e92d // stmdb sp!, {r4-r8, lr}

#define SELFTEST_CALLS 8

enum { EAX = 0, ECX = 1, EDX = 2, ESP = 4 };

typedef uint8_t * (* mk_address_fn)(uint32_t addr);
typedef void (* check_event_fn)(void);

static mk_address_fn mk_address;
static check_event_fn check_event;
static volatile uint32_t * regs;
static so_hook lzss_hook;

static int selftest_left = SELFTEST_CALLS;
static int disabled;
static unsigned calls;
static unsigned long long bytes_out, time_us;

// Translation cache for one stream of x86 addresses
typedef struct {
    uint32_t page;
    uint8_t * base;
} xcache;

static inline uint8_t * xptr(xcache * c, uint32_t addr) {
    uint32_t page = addr >> 12;
    if (page != c->page) {
        c->page = page;
        c->base = mk_address(addr & ~0xfffu);
    }
    return c->base + (addr & 0xfff);
}

// Output: emulated memory, or a host buffer (self-test)
typedef struct {
    xcache wc, rc;
    uint8_t * host;
    uint32_t host_size;
} output;

static inline void out_put(output * o, uint32_t dst0, uint32_t addr, uint8_t v) {
    if (!o->host) {
        *xptr(&o->wc, addr) = v;
        return;
    }
    uint32_t i = addr - dst0;
    if (i >= o->host_size) {
        uint32_t n = o->host_size ? o->host_size * 2 : 65536;
        while (n <= i) n *= 2;
        o->host = realloc(o->host, n);
        o->host_size = n;
    }
    o->host[i] = v;
}

static inline uint8_t out_get(output * o, uint32_t dst0, uint32_t addr) {
    return o->host ? o->host[addr - dst0] : *xptr(&o->rc, addr);
}

typedef struct {
    uint32_t eax, ecx, edx;
    uint32_t written;
} result;

static result decompress(uint32_t src, uint32_t dst, output * o) {
    xcache sc = { 0xffffffff, NULL };
    uint32_t len = *(uint32_t *) xptr(&sc, src); // 4-byte aligned in practice
    if ((src & 0xfff) > 0xffc) { // header across pages
        len = 0;
        for (int i = 0; i < 4; i++) len |= (uint32_t) *xptr(&sc, src + i) << (8 * i);
    }
    uint32_t end = src + len + 3; // sic: the original stops one byte early
    uint32_t p = src + 4;
    uint32_t dst0 = dst;
    uint32_t flags = 0;
    int counter = 0;
    uint32_t eax, ecx = p, edx = dst;
    result r;

    for (;;) {
        eax = 1;
        if (counter == 0) {
            counter = 8;
            ecx = p;
            if (p >= end) break;
            edx = p;
            flags = *xptr(&sc, p);
            eax = flags;
            p++;
            ecx = p;
        }
        edx = flags & 1;
        eax = p;
        if (p >= end) break;
        if (flags & 1) {
            out_put(o, dst0, dst++, *xptr(&sc, p));
            p++;
        } else {
            uint32_t lo = *xptr(&sc, p);
            uint32_t hi = *xptr(&sc, p + 1);
            p += 2;
            uint32_t off = lo | ((hi & 0xf0) << 4);
            uint32_t dend = dst + (hi & 0xf) + 3;
            uint32_t ref = dst - ((dst - dst0 - (off - 0xfee)) & 0xfff);
            while (ref < dst0) {
                out_put(o, dst0, dst++, 0);
                ref++;
            }
            while (dst < dend) {
                uint8_t v = out_get(o, dst0, ref++);
                out_put(o, dst0, dst++, v);
            }
        }
        flags = (uint32_t) ((int32_t) flags >> 1);
        counter--;
        ecx = flags;
        edx = (uint32_t) counter;
    }
    r.eax = eax;
    r.ecx = ecx;
    r.edx = edx;
    r.written = dst - dst0;
    return r;
}

static void lzss_native(void) {
    uint32_t esp = regs[ESP];
    xcache ac = { 0xffffffff, NULL };
    uint32_t src = *(uint32_t *) xptr(&ac, esp + 4);
    uint32_t dst = *(uint32_t *) xptr(&ac, esp + 8);

    if (disabled) {
        SO_CONTINUE(int, lzss_hook);
        return;
    }

    if (selftest_left > 0) {
        selftest_left--;
        output o = { { 0xffffffff, NULL }, { 0xffffffff, NULL }, malloc(65536), 65536 };
        result r = decompress(src, dst, &o);
        SO_CONTINUE(int, lzss_hook);

        xcache dc = { 0xffffffff, NULL };
        uint32_t bad = r.written;
        for (uint32_t i = 0; i < r.written; i++) {
            if (*xptr(&dc, dst + i) != o.host[i]) { bad = i; break; }
        }
        if (bad != r.written || regs[EAX] != r.eax || regs[ECX] != r.ecx || regs[EDX] != r.edx) {
            l_error("ff7_lzss selftest MISMATCH (%u bytes, first diff at %u): native %08x/%08x/%08x, "
                    "original %08x/%08x/%08x - native LZSS disabled",
                    r.written, bad, r.eax, r.ecx, r.edx, regs[EAX], regs[ECX], regs[EDX]);
            disabled = 1;
        } else {
            l_info("ff7_lzss selftest ok: %u bytes identical%s", r.written,
                   selftest_left == 0 ? " (selftest finished)" : "");
        }
        free(o.host);
        return;
    }

    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    output o = { { 0xffffffff, NULL }, { 0xffffffff, NULL }, NULL, 0 };
    result r = decompress(src, dst, &o);
    regs[EAX] = r.eax;
    regs[ECX] = r.ecx;
    regs[EDX] = r.edx;
    check_event();
    regs[ESP] = esp + 4; // ret
    time_us += sceKernelGetProcessTimeWide() - t0;
    bytes_out += r.written;
    calls++;
}

void ff7_lzss_patch(void) {
    uintptr_t fn = so_symbol(&so_mod, SYM_LZSS);
    check_event = (check_event_fn) so_symbol(&so_mod, SYM_CHECK_EVENT);
    mk_address = (mk_address_fn) so_symbol(&so_mod, SYM_MK_ADDRESS);
    regs = ff7_x86_regs();

    if (!fn || !check_event || !mk_address || !regs || *(uint32_t *) (fn & ~1) != LZSS_PROLOGUE) {
        l_warn("ff7_lzss: unexpected .so layout, native LZSS disabled");
        return;
    }
    lzss_hook = hook_addr(fn, (uintptr_t) &lzss_native);
    l_info("ff7_lzss: native LZSS decompressor installed");
}

void ff7_lzss_report(double seconds) {
    if (seconds <= 0 || !calls) return;
    l_info("lzss: %u calls, %llu KB out, %.1f ms", calls, bytes_out / 1024, time_us / 1000.0);
    calls = 0;
    bytes_out = time_us = 0;
}
