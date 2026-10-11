/*
 * Faster texture conversion for libjni_ff7.so.
 *
 * C_006B42E5 is ff7.exe's convert_image_data(), recompiled to ARM. For 8-bit
 * paletted images it runs, per pixel, the palette lookup C_006865F0 (which
 * translates the palette address with mk_address every time), the alpha /
 * color-key rules and the format conversion C_006861EC. Profiling showed this
 * loop taking most of the frame time in battles.
 *
 * Everything per pixel depends only on the 8-bit index, so the replacement
 * runs that exact logic once per index into a 256-entry table and then only
 * indexes it. Any other case (other source depths, raw index copy, paletted
 * destination) is forwarded to the original function.
 *
 * Field offsets come from the disassembly of v1.0.29 (see PLANO.md).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_texture.h"

#include "utils/logger.h"

#include <stdint.h>
#include <string.h>

#include <kubridge.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define SYM_CONVERT_IMAGE "_Z10C_006B42E5iiP16t_ff7_tex_headerP16t_texture_formatPh"
#define SYM_PALETTE_COLOR "_Z10C_006865F0jiP16t_texture_format"
#define SYM_RGBA_TO_FMT   "_Z10C_006861EC5tRGBAP16t_texture_format"
#define SYM_MK_ADDRESS    "mk_address"

// convert_image_data: first instruction and the literal used to reach the
// global "black is transparent" flag (ldr r5, [pc, #432] at +0x5a)
#define CONVERT_IMAGE_OFFSET   0xc3342c
#define CONVERT_IMAGE_PROLOGUE 0x4ff0e92d // stmdb sp!, {r4-r11, lr}
#define FLAG_LITERAL_OFFSET    0xc33638
#define FLAG_PC_OFFSET         0xc33494

#define U8(p, off)  (*(uint8_t *) ((uint8_t *) (p) + (off)))
#define U32(p, off) (*(uint32_t *) ((uint8_t *) (p) + (off)))
#define S32(p, off) (*(int32_t *) ((uint8_t *) (p) + (off)))

typedef uint32_t (* palette_color_fn)(uint32_t pixel, int palette, void * fmt);
typedef uint32_t (* rgba_to_fmt_fn)(uint32_t rgba, void * fmt);
typedef void * (* mk_address_fn)(uint32_t addr);

static palette_color_fn palette_color;
static rgba_to_fmt_fn rgba_to_fmt;
static mk_address_fn mk_address;
static uint8_t * black_flag; // global byte read by convert_image_data
static so_hook convert_hook;

static struct {
    unsigned fast, slow;
    unsigned long long pixels, us;
} st;

static void convert_image_data(int invert, int raw_index, void * th, void * tf, uint8_t * dst) {
    SceUInt64 t0 = sceKernelGetProcessTimeWide();

    // th: t_ff7_tex_header, source t_texture_format embedded at +0x3c
    // tf: destination t_texture_format
    if (!th || U32(th, 0x68) != 1 || raw_index != 0 || U32(tf, 0x10) != 0)
        goto original;

    // Color key: index 0 is transparent
    uint32_t color_key;
    if (U32(th, 0xbc)) {
        uint32_t table = U32(th, 0xc0);
        if (!table) goto original;
        color_key = ((uint8_t *) mk_address(table))[U32(th, 0xd0)];
    } else {
        color_key = U32(th, 0x08) != 0;
    }

    uint32_t image = U32(th, 0xd4);
    if (!image) goto original;
    const uint8_t * src = mk_address(image);

    int width = S32(th, 0x3c), height = S32(th, 0x40);
    int pitch = S32(tf, 0x08), dst_bpp = S32(tf, 0x2c);
    if (invert) {
        dst += pitch * (height - 1);
        pitch = -pitch;
    }
    if (height <= 0) goto done;

    // Per-index results: `raw` is the color after the alpha rules (stored
    // as-is for 24-bit destinations), `conv` the destination-format value
    uint32_t raw[256], conv[256];
    const uint32_t palette = U32(th, 0xd0);
    void * src_fmt = (uint8_t *) th + 0x3c;
    const uint8_t black_is_transparent = *black_flag;
    for (int i = 0; i < 256; i++) {
        uint32_t c = palette_color(i, palette, src_fmt);
        uint32_t a;
        if (color_key && i == 0) {
            c &= 0x00ffffff;
            a = 0;
        } else {
            a = c >> 24;
            if (a != 0) {
                if (a == 254 && S32(th, 0xc4) >= 0) {
                    a = U32(th, 0xc4) & 0xff;
                    c = (c & 0x00ffffff) | (a << 24);
                }
                if (!U8(th, 0xcc) && (c & 0x00ffffff) == 0)
                    c |= 0x010101; // keep opaque black distinguishable from transparent
            }
        }
        if (black_is_transparent && (c & 0x00ffffff) == 0)
            c = (c & 0x00ffffff) | ((~a & 0xff) << 24);
        raw[i] = c;
        conv[i] = rgba_to_fmt(c, tf);
    }

    for (int y = 0; y < height; y++, dst += pitch) {
        uint8_t * d = dst;
        switch (dst_bpp) {
            case 4:
                for (int x = 0; x < width; x++, d += 4) *(uint32_t *) d = conv[*src++];
                break;
            case 2:
                for (int x = 0; x < width; x++, d += 2) *(uint16_t *) d = (uint16_t) conv[*src++];
                break;
            case 1:
                for (int x = 0; x < width; x++, d += 1) *d = (uint8_t) conv[*src++];
                break;
            case 3:
                for (int x = 0; x < width; x++, d += 3) {
                    uint32_t c = raw[*src++];
                    d[0] = c; d[1] = c >> 8; d[2] = c >> 16;
                }
                break;
            default: // the original stores nothing but still advances
                src += width;
                break;
        }
    }

done:
    st.fast++;
    st.pixels += (unsigned long long) width * (height > 0 ? height : 0);
    st.us += sceKernelGetProcessTimeWide() - t0;
    return;

original:
    SO_CONTINUE(int, convert_hook, invert, raw_index, th, tf, dst);
    st.slow++;
    st.us += sceKernelGetProcessTimeWide() - t0;
}

void ff7_texture_patch(void) {
    uintptr_t base = so_mod.text_base;
    uintptr_t convert = so_symbol(&so_mod, SYM_CONVERT_IMAGE);
    palette_color = (palette_color_fn) so_symbol(&so_mod, SYM_PALETTE_COLOR);
    rgba_to_fmt = (rgba_to_fmt_fn) so_symbol(&so_mod, SYM_RGBA_TO_FMT);
    mk_address = (mk_address_fn) so_symbol(&so_mod, SYM_MK_ADDRESS);

    // Only patch the exact binary this was written against
    if (!convert || !palette_color || !rgba_to_fmt || !mk_address
        || (convert & ~1) != base + CONVERT_IMAGE_OFFSET
        || *(uint32_t *) (base + CONVERT_IMAGE_OFFSET) != CONVERT_IMAGE_PROLOGUE) {
        l_warn("ff7_texture: unexpected .so layout, fast texture conversion disabled");
        return;
    }

    uint32_t literal = *(uint32_t *) (base + FLAG_LITERAL_OFFSET);
    black_flag = *(uint8_t **) (base + FLAG_PC_OFFSET + literal);

    convert_hook = hook_addr(convert, (uintptr_t) &convert_image_data);
    l_info("ff7_texture: fast convert_image_data installed (flag at %p)", black_flag);
}

void ff7_texture_report(double seconds) {
    if (seconds <= 0) return;
    l_info("textures/s: %.1f fast, %.1f original, %.0f K pixels, %.1f ms converting",
           st.fast / seconds, st.slow / seconds, st.pixels / 1000.0 / seconds, st.us / 1000.0 / seconds);
    memset(&st, 0, sizeof(st));
}
