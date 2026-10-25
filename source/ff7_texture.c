/*
 * Faster texture conversion for libjni_ff7.so.
 *
 * C_006B42E5 is ff7.exe's convert_image_data(), recompiled to ARM. For 8-bit
 * paletted images it runs, per pixel, the palette lookup C_006865F0 (which
 * translates the palette address with mk_address every time), the alpha /
 * color-key rules and the format conversion C_006861EC. Profiling showed this
 * loop taking most of the frame time in battles.
 *
 * The replacement reproduces that logic natively:
 *  - 8-bit paletted sources: everything depends only on the index, so the
 *    256 results are computed once and the pixels just index them;
 *  - 16/24/32-bit sources (e.g. the battle "swirl" screen capture): the
 *    per-channel divisions of C_00686514 become 256-entry tables.
 * The destination encoding (C_006861EC) is also table-driven. Raw index
 * copies and paletted destinations are forwarded to the original function.
 *
 * GPU palettes (v0.6.31): gfx_load_texture converts a page into the texture
 * set's buffer and passes that buffer straight to glTexImage2D (in
 * C_006A753A). Fields with animated palettes (the chocobo ranch after the
 * race: ~145 pages of 256x256 a second, 22 ms of a 61 ms frame converting
 * plus 7 ms copying 256 KB a page into the render queue) redo that on every
 * palette change. For an 8-bit page converted from there into a contiguous
 * RGBA8888 buffer, the conversion is left pending: the next glTexImage2D of
 * that buffer, as GL_RGBA bytes of the same size, becomes a GXM P8 texture of
 * the indices with the 256 converted colors as its palette
 * (gl_queue_paletted_texture): the same colors on screen, a quarter of the
 * data and no per-pixel work on the CPU. Anything else first converts the
 * pending page as before. DATA_PATH "gpu_palette_off.txt" turns it off.
 *
 * Field offsets come from the disassembly of v1.0.29 (see PLANO.md).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_texture.h"
#include "ff7_switch.h"
#include "ff7_buttons.h"
#include "ff7_texture_cache.h"

#include "reimpl/gl_queue.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <kubridge.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define SYM_CONVERT_IMAGE "_Z10C_006B42E5iiP16t_ff7_tex_headerP16t_texture_formatPh"
#define SYM_PALETTE_COLOR "_Z10C_006865F0jiP16t_texture_format"
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
typedef void * (* mk_address_fn)(uint32_t addr);

static palette_color_fn palette_color;
static mk_address_fn mk_address;
static uint8_t * black_flag; // global byte read by convert_image_data
static tc_cache tcache;      // incremental reconversion (game thread only)
static int tc_off;           // DATA_PATH "texcache_off.txt"

static so_hook convert_hook;

#define GFX_LOAD_TEXTURE_CALL 0xc33700 // return address of its convert_image_data call
static uintptr_t load_texture_ret; // in the loaded .so
static int gpu_palette_off;        // DATA_PATH "gpu_palette_off.txt"

// A page left unconverted for the glTexImage2D that follows (game thread)
static struct {
    uint8_t * dst;               // the buffer as passed (what glTexImage2D gets)
    const uint8_t * src;         // w x h indices
    int w, h, invert;
    uint32_t conv[256];
    unsigned uploads, converted; // stats: as P8, converted after all
} pend;

static struct {
    unsigned fast, slow, enc_reuse, big;
    unsigned long long pixels, us, setup_us;
    int max_w, max_h;
} st;

// Alpha/black rules applied by convert_image_data to every source color.
// `a` is the alpha the original keeps in a register (it may differ from c>>24
// when the color key forced it to 0).
static inline uint32_t alpha_rules(uint32_t c, uint32_t a, const void * th, uint8_t black_is_transparent) {
    if (a != 0) {
        if (a == 254 && S32(th, 0xc4) >= 0) {
            a = U32(th, 0xc4) & 0xff;
            c = (c & 0x00ffffff) | (a << 24);
        }
        if (!U8(th, 0xcc) && (c & 0x00ffffff) == 0)
            c |= 0x010101; // keep opaque black distinguishable from transparent
    }
    if (black_is_transparent && (c & 0x00ffffff) == 0)
        c = (c & 0x00ffffff) | ((~a & 0xff) << 24);
    return c;
}

// C_006861EC for a non-paletted destination, as four 256-entry tables
typedef struct {
    uint32_t r[256], g[256], b[256], a[256];
    int has_alpha;
} encoder_t;

// ARM register shifts by >= 32 yield 0; C leaves them undefined
static inline uint32_t shl(uint32_t v, uint32_t s) { return s < 32 ? v << s : 0; }

// The destination format rarely changes: the 1,024 divisions (software on
// the Vita's CPU) only run when it does
static uint32_t enc_key[9];
static int enc_valid;

static int encoder_reuse(const void * tf) {
    uint32_t k[9] = { U32(tf, 0x70), U32(tf, 0x74), U32(tf, 0x78), U32(tf, 0x7c), U32(tf, 0x50), U32(tf, 0x54),
                      U32(tf, 0x58), U32(tf, 0x5c), U32(tf, 0x3c) != 0 };
    if (enc_valid && !memcmp(k, enc_key, sizeof(k))) return 1;
    memcpy(enc_key, k, sizeof(k));
    enc_valid = 1;
    return 0;
}

static void encoder_init(encoder_t * e, const void * tf) {
    uint32_t mr = U32(tf, 0x70), mg = U32(tf, 0x74), mb = U32(tf, 0x78), ma = U32(tf, 0x7c);
    uint32_t sr = U32(tf, 0x50), sg = U32(tf, 0x54), sb = U32(tf, 0x58), sa = U32(tf, 0x5c);
    e->has_alpha = U32(tf, 0x3c) != 0;
    for (uint32_t v = 0; v < 256; v++) {
        // value * max / 255, rounded (remainder > 127 rounds up) for RGB, floored for alpha
        uint32_t x;
        x = v * mr; e->r[v] = shl(x / 255 + (x % 255 > 127), sr);
        x = v * mg; e->g[v] = shl(x / 255 + (x % 255 > 127), sg);
        x = v * mb; e->b[v] = shl(x / 255 + (x % 255 > 127), sb);
        e->a[v] = shl((v * ma) / 255, sa);
    }
}

static inline uint32_t encode(const encoder_t * e, uint32_t c) {
    uint32_t out = e->r[(c >> 16) & 0xff] | e->g[(c >> 8) & 0xff] | e->b[c & 0xff];
    if (e->has_alpha) out |= e->a[c >> 24];
    return out;
}

// C_00686514 for a non-paletted source: per channel ((px & mask) >> shift) * 255 / max
#define DECODE_LUT_MAX 1024
typedef struct {
    uint32_t mask[4], shift[4];
    uint8_t lut[4][DECODE_LUT_MAX];
    int has_alpha;
} decoder_t;

static int decoder_init(decoder_t * d, const void * fmt) {
    for (int ch = 0; ch < 4; ch++) {
        d->mask[ch] = U32(fmt, 0x40 + 4 * ch);
        d->shift[ch] = U32(fmt, 0x50 + 4 * ch);
        uint32_t max = U32(fmt, 0x70 + 4 * ch);
        if (d->shift[ch] >= 32) d->mask[ch] = d->shift[ch] = 0; // channel always reads 0
        uint32_t top = d->mask[ch] >> d->shift[ch];
        if (top >= DECODE_LUT_MAX) return 0; // unusual layout: let the original handle it
        for (uint32_t v = 0; v <= top; v++)
            d->lut[ch][v] = max ? (uint8_t) (v * 255 / max) : 0; // __udivsi3 by 0 yields 0
    }
    d->has_alpha = U32(fmt, 0x3c) != 0;
    return 1;
}

static inline uint32_t decode(const decoder_t * d, uint32_t px) {
    uint32_t c0 = d->lut[0][(px & d->mask[0]) >> d->shift[0]];
    uint32_t c1 = d->lut[1][(px & d->mask[1]) >> d->shift[1]];
    uint32_t c2 = d->lut[2][(px & d->mask[2]) >> d->shift[2]];
    uint32_t a = d->has_alpha ? d->lut[3][(px & d->mask[3]) >> d->shift[3]] : 255;
    return c2 | (c1 << 8) | (c0 << 16) | (a << 24);
}

static inline void store(uint8_t * d, int dst_bpp, uint32_t conv, uint32_t raw) {
    switch (dst_bpp) {
        case 4: *(uint32_t *) d = conv; break;
        case 2: *(uint16_t *) d = (uint16_t) conv; break;
        case 1: *d = (uint8_t) conv; break;
        case 3: d[0] = raw; d[1] = raw >> 8; d[2] = raw >> 16; break;
        default: break; // the original stores nothing but still advances
    }
}

// Paletted rows: the per-pixel switch on the destination depth cost ~10
// cycles a pixel, and some fields convert 2-3 256x256 textures every frame
static void blit8_32(uint32_t * d, const uint8_t * s, int w, const uint32_t * lut) {
    int x = 0;
    for (; x + 4 <= w; x += 4) {
        uint32_t i0 = s[x], i1 = s[x + 1], i2 = s[x + 2], i3 = s[x + 3];
        d[x] = lut[i0];
        d[x + 1] = lut[i1];
        d[x + 2] = lut[i2];
        d[x + 3] = lut[i3];
    }
    for (; x < w; x++) d[x] = lut[s[x]];
}

static void blit8_16(uint16_t * d, const uint8_t * s, int w, const uint16_t * lut) {
    int x = 0;
    for (; x + 4 <= w; x += 4) {
        uint32_t i0 = s[x], i1 = s[x + 1], i2 = s[x + 2], i3 = s[x + 3];
        d[x] = lut[i0];
        d[x + 1] = lut[i1];
        d[x + 2] = lut[i2];
        d[x + 3] = lut[i3];
    }
    for (; x < w; x++) d[x] = lut[s[x]];
}

// Development self-test: the first conversions of each kind also run the
// original into a scratch buffer and the outputs are compared byte by byte.
#ifdef ENABLE_SELFTESTS
#define SELFTEST_RUNS 12
#else
#define SELFTEST_RUNS 0
#endif
static int selftest_left[5] = {0, SELFTEST_RUNS, SELFTEST_RUNS, SELFTEST_RUNS, SELFTEST_RUNS};

static void selftest(int invert, int raw_index, void * th, void * tf, uint8_t * dst, int src_bpp) {
    int width = S32(th, 0x3c), height = S32(th, 0x40);
    int pitch = S32(tf, 0x08), dst_bpp = S32(tf, 0x2c);
    int apitch = pitch < 0 ? -pitch : pitch;
    if (height <= 0 || width <= 0 || dst_bpp < 1 || dst_bpp > 4) return;
    uint8_t * ref = malloc((size_t) apitch * height);
    if (!ref) return;
    memset(ref, 0, (size_t) apitch * height);
    SO_CONTINUE(int, convert_hook, invert, raw_index, th, tf, ref);

    // `dst` is the base pointer as passed by the game (same as `ref`)
    for (int y = 0; y < height; y++) {
        const uint8_t * a = dst + (size_t) y * apitch;
        const uint8_t * b = ref + (size_t) y * apitch;
        if (memcmp(a, b, (size_t) width * dst_bpp) != 0) {
            int x = 0;
            while (x < width * dst_bpp && a[x] == b[x]) x++;
            l_error("ff7_texture selftest MISMATCH: src %d bpp -> dst %d bpp, %dx%d, row %d byte %d: "
                    "native %02x original %02x", src_bpp, dst_bpp, width, height, y, x, a[x], b[x]);
            free(ref);
            return;
        }
    }
    l_info("ff7_texture selftest ok: src %d bpp -> dst %d bpp, %dx%d", src_bpp, dst_bpp, width, height);
    free(ref);
}

// Converts the pending page into its buffer, as convert_image_data would have
static void pend_convert(void) {
    if (!pend.dst) return;
    int pitch = pend.w * 4;
    uint8_t * d = pend.dst;
    if (pend.invert) {
        d += pitch * (pend.h - 1);
        pitch = -pitch;
    }
    const uint8_t * s = pend.src;
    for (int y = 0; y < pend.h; y++, d += pitch, s += pend.w) blit8_32((uint32_t *) d, s, pend.w, pend.conv);
    pend.dst = NULL;
    pend.converted++;
}

int ff7_texture_upload(GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLenum format,
                       GLenum type, const void * pixels) {
    if (!pend.dst) return 0;
    if (pixels != pend.dst || level || internalformat != GL_RGBA || format != GL_RGBA || type != GL_UNSIGNED_BYTE
        || w != pend.w || h != pend.h) {
        pend_convert();
        return 0;
    }
    // row 0 of the buffer is the last row of the page when converted inverted
    const uint8_t * rows = pend.invert ? pend.src + (size_t) pend.w * (pend.h - 1) : pend.src;
    gl_queue_paletted_texture(target, w, h, pend.conv, rows, pend.invert ? -pend.w : pend.w);
    pend.dst = NULL;
    pend.uploads++;
    return 1;
}

static void convert_image_data(int invert, int raw_index, void * th, void * tf, uint8_t * dst) {
    uintptr_t caller = (uintptr_t) __builtin_return_address(0) & ~1u;
    pend_convert(); // a page nobody uploaded: as before
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    uint8_t * dst_base = dst;
    ff7_buttons_font(th); // PlayStation symbols in the menu font, before converting it

    // th: t_ff7_tex_header, source t_texture_format embedded at +0x3c
    // tf: destination t_texture_format (must not be paletted)
    if (!th || U32(tf, 0x10) != 0)
        goto original;

    const int src_bpp = S32(th, 0x68);
    void * src_fmt = (uint8_t *) th + 0x3c;
    if (src_bpp == 1) {
        if (raw_index != 0) goto original; // raw index copy
    } else if (src_bpp < 2 || src_bpp > 4 || U32(src_fmt, 0x10) != 0) {
        goto original; // paletted wide pixels or unknown depth
    }

    uint32_t color_key = 0;
    if (src_bpp == 1) {
        if (U32(th, 0xbc)) {
            uint32_t table = U32(th, 0xc0);
            if (!table) goto original;
            color_key = ((uint8_t *) mk_address(table))[U32(th, 0xd0)];
        } else {
            color_key = U32(th, 0x08) != 0;
        }
    }

    uint32_t image = U32(th, 0xd4);
    if (!image) goto original;

    static decoder_t dec; // only used by the game thread
    if (src_bpp != 1 && !decoder_init(&dec, src_fmt)) goto original;

    const uint8_t * src = mk_address(image);
    int width = S32(th, 0x3c), height = S32(th, 0x40);
    int pitch = S32(tf, 0x08), dst_bpp = S32(tf, 0x2c);
    if (invert) {
        dst += pitch * (height - 1);
        pitch = -pitch;
    }
    if (height <= 0) goto done;

    const uint8_t black_is_transparent = *black_flag;
    static encoder_t enc;
    if (encoder_reuse(tf)) st.enc_reuse++;
    else encoder_init(&enc, tf);

    if (src_bpp == 1) {
        // Everything depends only on the index: build the 256 results once
        uint32_t raw[256], conv[256];
        const uint32_t palette = U32(th, 0xd0);
        // C_006865F0 for a paletted format: entry stride * palette + i of the
        // palette at [fmt + 0x24] (one mk_address, as it does), 0xffff0000
        // past [fmt + 0x1c] entries or without a palette
        const uint32_t * pal = NULL;
        uint32_t pal_base = 0, pal_n = 0;
        if (U32(src_fmt, 0x10) && U32(src_fmt, 0x24)) {
            pal = mk_address(U32(src_fmt, 0x24));
            pal_base = U32(src_fmt, 0x20) * palette;
            pal_n = U32(src_fmt, 0x1c);
        }
        for (int i = 0; i < 256; i++) {
            uint32_t c;
            if (pal) c = pal_base + i < pal_n ? pal[pal_base + i] : 0xffff0000u;
            else if (U32(src_fmt, 0x10)) c = 0xffff0000u;
            else c = palette_color(i, palette, src_fmt);
            uint32_t a = c >> 24;
            if (color_key && i == 0) {
                c &= 0x00ffffff;
                a = 0;
            }
            c = alpha_rules(c, a, th, black_is_transparent);
            raw[i] = c;
            conv[i] = encode(&enc, c);
        }
        st.setup_us += sceKernelGetProcessTimeWide() - t0;
        if (!gpu_palette_off && caller == load_texture_ret && dst_bpp == 4 && S32(tf, 0x08) == width * 4
            && width > 0 && width <= 1024 && height <= 1024 && !(((uintptr_t) dst_base) & 3)) {
            // gfx_load_texture uploads this buffer next: as a GPU-paletted texture
            pend.dst = dst_base;
            pend.src = src;
            pend.w = width;
            pend.h = height;
            pend.invert = invert;
            memcpy(pend.conv, conv, sizeof(conv));
            tc_forget(&tcache, dst);
        } else if (dst_bpp == 4 && !(((uintptr_t) dst | (uint32_t) pitch) & 3)) {
            // The usual case (RGBA8888): one table load and store per pixel,
            // or only the pixels whose color changed when the same page goes
            // to the same buffer again (animated palettes)
            if (tc_off) {
                for (int y = 0; y < height; y++, dst += pitch, src += width)
                    blit8_32((uint32_t *) dst, src, width, conv);
            } else {
                tc_convert(&tcache, dst, src, width, height, pitch, conv);
            }
        } else if (dst_bpp == 2 && !(((uintptr_t) dst | (uint32_t) pitch) & 1)) {
            uint16_t conv16[256];
            for (int i = 0; i < 256; i++) conv16[i] = (uint16_t) conv[i];
            for (int y = 0; y < height; y++, dst += pitch, src += width)
                blit8_16((uint16_t *) dst, src, width, conv16);
        } else {
            for (int y = 0; y < height; y++, dst += pitch) {
                uint8_t * d = dst;
                for (int x = 0; x < width; x++, d += dst_bpp, src++)
                    store(d, dst_bpp, conv[*src], raw[*src]);
            }
        }
    } else {
        for (int y = 0; y < height; y++, dst += pitch) {
            uint8_t * d = dst;
            for (int x = 0; x < width; x++, d += dst_bpp, src += src_bpp) {
                uint32_t px = src_bpp == 2 ? *(const uint16_t *) src
                            : src_bpp == 4 ? *(const uint32_t *) src
                            : (uint32_t) src[0] | (src[1] << 8) | (src[2] << 16);
                uint32_t c = decode(&dec, px);
                c = alpha_rules(c, c >> 24, th, black_is_transparent);
                store(d, dst_bpp, encode(&enc, c), c);
            }
        }
    }

done:
    if (!pend.dst && src_bpp >= 1 && src_bpp <= 4 && selftest_left[src_bpp] > 0) {
        selftest_left[src_bpp]--;
        selftest(invert, raw_index, th, tf, dst_base, src_bpp);
    }
    st.fast++;
    st.pixels += (unsigned long long) width * (height > 0 ? height : 0);
    if (width * height > st.max_w * st.max_h) {
        st.max_w = width;
        st.max_h = height;
    }
    if (width >= 256 && height >= 256) st.big++;
    st.us += sceKernelGetProcessTimeWide() - t0;
    return;

original:
    SO_CONTINUE(int, convert_hook, invert, raw_index, th, tf, dst);
    st.slow++;
    st.us += sceKernelGetProcessTimeWide() - t0;
}

void ff7_texture_patch(void) {
    if (ff7_switch_off("ff7_texture", DATA_PATH "texture_off.txt")) return;
    uintptr_t base = so_mod.text_base;
    uintptr_t convert = so_symbol(&so_mod, SYM_CONVERT_IMAGE);
    palette_color = (palette_color_fn) so_symbol(&so_mod, SYM_PALETTE_COLOR);
    mk_address = (mk_address_fn) so_symbol(&so_mod, SYM_MK_ADDRESS);

    // Only patch the exact binary this was written against
    if (!convert || !palette_color || !mk_address
        || (convert & ~1) != base + CONVERT_IMAGE_OFFSET
        || *(uint32_t *) (base + CONVERT_IMAGE_OFFSET) != CONVERT_IMAGE_PROLOGUE) {
        l_warn("ff7_texture: unexpected .so layout, fast texture conversion disabled");
        return;
    }

    uint32_t literal = *(uint32_t *) (base + FLAG_LITERAL_OFFSET);
    black_flag = *(uint8_t **) (base + FLAG_PC_OFFSET + literal);

    tc_off = ff7_switch_off("ff7_texture cache", DATA_PATH "texcache_off.txt");
    gpu_palette_off = ff7_switch_off("ff7_texture GPU palettes", DATA_PATH "gpu_palette_off.txt");
    load_texture_ret = base + GFX_LOAD_TEXTURE_CALL;
    convert_hook = hook_addr(convert, (uintptr_t) &convert_image_data);
    l_info("ff7_texture: fast convert_image_data installed (flag at %p, incremental reconversion %s, GPU palettes %s)",
           black_flag, tc_off ? "off" : "on", gpu_palette_off ? "off" : "on");
}

// Port menu A/B test (game thread): back on, the cache starts empty
void ff7_texture_cache_set(int on) {
    if (on == !tc_off) return;
    if (on) memset(tcache.e, 0, sizeof(tcache.e));
    tc_off = !on;
    l_info("ff7_texture: incremental reconversion %s", on ? "on" : "off (port menu)");
}

void ff7_texture_report(double seconds) {
    if (seconds <= 0) return;
    unsigned avg = st.fast ? (unsigned) (st.pixels / st.fast) : 0;
    l_info("textures/s: %.1f fast, %.1f original, %.1f ms converting (%.1f ms 8-bit setup); %u pixels each on"
           " average, %u of %u >= 256x256, largest %dx%d; reconversions: %u full, %u partial, %u unchanged",
           st.fast / seconds, st.slow / seconds, st.us / 1000.0 / seconds, st.setup_us / 1000.0 / seconds, avg,
           st.big, st.fast, st.max_w, st.max_h, tcache.full, tcache.partial, tcache.skipped);
    if (pend.uploads || pend.converted)
        l_info("textures/s: %.1f pages to the GPU as indices + palette, %.1f converted after all", pend.uploads / seconds,
               pend.converted / seconds);
    pend.uploads = pend.converted = 0;
    memset(&st, 0, sizeof(st));
    tcache.full = tcache.partial = tcache.skipped = 0;
}
