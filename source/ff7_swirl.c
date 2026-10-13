/*
 * Native 16-bit framebuffer copy for libjni_ff7.so (battle swirl).
 *
 * ff7.exe's function at 0x673D26 copies a rectangle of a 16-bit surface
 * into a buffer, with x/y steps (downsampling), zero-filling what lies
 * beyond the screen limits:
 *
 *     void copy16(struct copy_desc * d)
 *
 * The battle swirl runs it on a full screen grab: recompiled to ARM, every
 * pixel costs several mk_address calls, and profiling showed it (plus
 * mk_address) taking most of the swirl's first frame.
 *
 * Only the loop path (d->x_step or d->y_step set, d->bpp == 2) is native;
 * any other case runs the original. Register file and calling convention:
 * see ff7_anim.c / ff7_lzss.c.
 *
 * Structure fields (int32, offsets in bytes):
 *   +0x04 x, +0x08 y, +0x0C dst_w (pixels per dst row), +0x10 dst_h,
 *   +0x14 x_step, +0x18 y_step, +0x28 src_pitch (bytes), +0x2C bpp,
 *   +0x30 src (x86 address), +0xB4 dst (x86 address)
 * Screen limits: struct at *(0xDB2BB8), +0x954 width, +0x958 height.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_swirl.h"
#include "ff7_anim.h"

#include "reimpl/gl_queue.h"
#include "utils/logger.h"
#include "utils/glstats.h"

#include <stdint.h>

#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>
#include <kubridge.h>
#include <psp2/kernel/clib.h>
#include <vitaGL.h>

extern so_module so_mod;

#define SYM_COPY16      "_Z15func_0x00673d26v"
#define SYM_CHECK_EVENT "_Z12_check_eventv"
#define SYM_MK_ADDRESS  "mk_address"
#define COPY16_PROLOGUE 0xf8dfb570 // push {r4-r6, lr}; ldr.w r4, [pc, #...]
#define SCREEN_PTR      0xdb2bb8

enum { EAX = 0, ECX = 1, EDX = 2, ESP = 4 };

typedef uint8_t * (* mk_address_fn)(uint32_t addr);
typedef void (* check_event_fn)(void);

static mk_address_fn mk_address;
static check_event_fn check_event;
static volatile uint32_t * regs;
static so_hook copy16_hook;

static unsigned calls;
static unsigned long long pixels, time_us;

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

static inline int32_t rd32(uint32_t addr) {
    if ((addr & 0xfff) <= 0xffc) return *(int32_t *) (mk_address(addr));
    uint32_t v = 0; // across a page boundary
    for (int i = 0; i < 4; i++) v |= (uint32_t) *mk_address(addr + i) << (8 * i);
    return (int32_t) v;
}

// x86 halfword access; pixels are 2-byte aligned so never cross a page
static inline uint16_t rd16(xcache * c, uint32_t addr) {
    return *(uint16_t *) xptr(c, addr);
}

static inline void wr16(xcache * c, uint32_t addr, uint16_t v) {
    *(uint16_t *) xptr(c, addr) = v;
}

static void copy16_native(void) {
    uint32_t esp = regs[ESP];
    uint32_t d = (uint32_t) rd32(esp + 4);

    int32_t x_step = rd32(d + 0x14), y_step = rd32(d + 0x18);
    if ((x_step == 0 && y_step == 0) || rd32(d + 0x2c) != 2 || x_step <= 0 || y_step <= 0) {
        SO_CONTINUE(int, copy16_hook);
        return;
    }

    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    uint32_t screen = (uint32_t) rd32(SCREEN_PTR);
    int32_t w_lim = rd32(screen + 0x954), h_lim = rd32(screen + 0x958);

    int32_t x0 = rd32(d + 0x04), y0 = rd32(d + 0x08);
    int32_t dst_w = rd32(d + 0x0c), dst_h = rd32(d + 0x10);
    int32_t pitch = rd32(d + 0x28) >> 1; // in pixels (sar)
    int32_t x_end = x0 + dst_w * x_step;
    int32_t y_end = y0 + dst_h * y_step;
    int32_t row_step = pitch * y_step;
    uint32_t src_row = (uint32_t) rd32(d + 0x30) + 2u * (uint32_t) (x0 + y0 * pitch);
    uint32_t dst_row = (uint32_t) rd32(d + 0xb4);

    xcache sc = { 0xffffffff, NULL }, dc = { 0xffffffff, NULL };
    int32_t y = y0;
    int rows = 0;
    for (; y < y_end; y += y_step) {
        uint32_t s = src_row, o = dst_row;
        if (y < h_lim) {
            for (int32_t x = x0; x < x_end; x += x_step) {
                wr16(&dc, o, x < w_lim ? rd16(&sc, s) : 0);
                s += 2u * (uint32_t) x_step;
                o += 2;
            }
        } else {
            for (int32_t x = x0; x < x_end; x += x_step) {
                wr16(&dc, o, 0);
                o += 2;
            }
        }
        src_row += 2u * (uint32_t) row_step;
        dst_row += 2u * (uint32_t) dst_w;
        rows++;
    }

    // Registers as the original leaves them (see the x86 code at 0x673E81)
    regs[EAX] = (uint32_t) y;
    regs[ECX] = rows ? d : (uint32_t) rd32(d + 0xb4);
    regs[EDX] = rows ? (uint32_t) y : d;
    check_event(); // the screen getter (0x676578) ends with one too
    check_event();
    regs[ESP] = esp + 4; // ret

    time_us += sceKernelGetProcessTimeWide() - t0;
    pixels += (unsigned long long) rows * (uint32_t) dst_w;
    calls++;
}

/*
 * C_006A7BF2(x, y, w, h, screen_h, x_scale, y_scale, read_buffer, dst):
 * the porters' screen grab. Without scaling it reads the screen row by row
 * (glReadPixels(x, row, 640, 1) into a static buffer) and converts each
 * pixel to RGB565 into dst, picking source pixel (i * 640) / w: a software
 * division per pixel on the Cortex-A9. The native version uses a per-call
 * index table; every other case (scaling, no dst) runs the original.
 */
#define GRAB_SYM        "_Z10C_006A7BF2iiiiiiiiPv"
#define GRAB_PROLOGUE   0x4ff0e92d // stmdb sp!, {r4-r11, lr}
#define GRAB_ROWBUF     0x141fad8  // u32 header + 640 RGBA pixels
#define GRAB_FLAG       0xf4f484   // u8 "skip next grab", u32 counter at +4
#define GRAB_GOT_FRAMES 0xf4c820   // GOT slot: pointer to a frame-skip switch

typedef int (* grab_fn)(int, int, int, int, int, int, int, int, void *);
static so_hook grab_hook;
static uint8_t * grab_rowbuf, * grab_flag;
static uint32_t * grab_frameskip; // variable the GOT slot points to
static unsigned grabs;
static unsigned long long grab_us;

static int grab_native(int x, int y, int w, int h, int screen_h, int x_scale, int y_scale, int read_buffer,
                       void * dst) {
    if (x_scale > 1 || y_scale > 1 || !dst || w <= 0 || w > 640) {
        sceClibMemcpy((void *) grab_hook.addr, grab_hook.orig_instr, sizeof(grab_hook.orig_instr));
        kuKernelFlushCaches((void *) grab_hook.addr, sizeof(grab_hook.orig_instr));
        int r = ((grab_fn) (grab_hook.thumb_addr ? grab_hook.thumb_addr : grab_hook.addr))(
            x, y, w, h, screen_h, x_scale, y_scale, read_buffer, dst);
        sceClibMemcpy((void *) grab_hook.addr, grab_hook.patch_instr, sizeof(grab_hook.patch_instr));
        kuKernelFlushCaches((void *) grab_hook.addr, sizeof(grab_hook.patch_instr));
        return r;
    }
    (void) read_buffer; // glReadBuffer is a no-op in the .so

    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    gl_queue_finish(); // GL used directly below, on the game thread
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    int skip = screen_h - (y + h); // rows below the grab, in GL coordinates
    uint16_t * out = dst;
    if (skip < 0) {
        out = (uint16_t *) ((uint8_t *) dst + (-skip) * w * 4); // sic
        h += skip;
        skip = 0;
    }
    if (*grab_flag) { // first grab after a reset is skipped
        *grab_flag = 0;
        return 0;
    }
    if (*grab_frameskip) {
        uint32_t n = ++*(uint32_t *) (grab_flag + 4);
        if (n & 3) return 0;
    }
    if (h <= 0) return 0;

    uint16_t idx[640];
    for (int i = 0; i < w; i++) idx[i] = (uint16_t) ((i * 640) / w);

    const uint32_t * row = (const uint32_t *) (grab_rowbuf + 4);
    int top = 480 - skip, end = 480 - (skip + h);
    for (int r = top; r != end; r--) {
        glReadPixels_stat(x, r, 640, 1, GL_RGBA, GL_UNSIGNED_BYTE, (void *) row);
        for (int i = 0; i < w; i++) {
            uint32_t p = row[idx[i]];
            *out++ = (uint16_t) (((p >> 19) & 0x1f) | (((p >> 3) & 0x1f) << 11) | (((p >> 10) & 0x3f) << 5));
        }
    }
    grab_us += sceKernelGetProcessTimeWide() - t0;
    grabs++;
    return 0;
}

void ff7_swirl_patch(void) {
    uintptr_t grab = so_symbol(&so_mod, GRAB_SYM);
    if (grab && *(uint32_t *) (grab & ~1) == GRAB_PROLOGUE) {
        grab_rowbuf = (uint8_t *) (so_mod.text_base + GRAB_ROWBUF);
        grab_flag = (uint8_t *) (so_mod.text_base + GRAB_FLAG);
        grab_frameskip = *(uint32_t **) (so_mod.text_base + GRAB_GOT_FRAMES);
        uintptr_t v = (uintptr_t) grab_frameskip;
        if (v < so_mod.text_base || v >= so_mod.text_base + 0x1498e40) { // end of .bss
            l_warn("ff7_swirl: unexpected frame-skip pointer %p, screen grab left as is", grab_frameskip);
        } else {
            grab_hook = hook_addr(grab, (uintptr_t) &grab_native);
            l_info("ff7_swirl: native screen grab installed (frame-skip switch at %p)", grab_frameskip);
        }
    } else {
        l_warn("ff7_swirl: screen grab not found, left as is");
    }

    uintptr_t fn = so_symbol(&so_mod, SYM_COPY16);
    check_event = (check_event_fn) so_symbol(&so_mod, SYM_CHECK_EVENT);
    mk_address = (mk_address_fn) so_symbol(&so_mod, SYM_MK_ADDRESS);
    regs = ff7_x86_regs();

    if (!fn || !check_event || !mk_address || !regs || *(uint32_t *) (fn & ~1) != COPY16_PROLOGUE) {
        l_warn("ff7_swirl: unexpected .so layout, native copy disabled");
        return;
    }
    copy16_hook = hook_addr(fn, (uintptr_t) &copy16_native);
    l_info("ff7_swirl: native 16-bit screen copy installed");
}

void ff7_swirl_report(double seconds) {
    if (grabs) l_info("screen grab: %u calls, %.1f ms", grabs, grab_us / 1000.0);
    grabs = 0;
    grab_us = 0;
    if (seconds <= 0 || !calls) return;
    l_info("swirl copy: %u calls, %llu K pixels, %.1f ms", calls, pixels / 1000, time_us / 1000.0);
    calls = 0;
    pixels = time_us = 0;
}
