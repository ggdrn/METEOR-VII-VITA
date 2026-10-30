#!/usr/bin/env python3
"""Host regression for the real widescreen hook with the real SO_CONTINUE macro.
Platform services are stubbed; no Vita/GPU is emulated. Optional source path
allows checking that an older version reproduces the argument corruption.
Sky patch decoding requires Unicorn and build/libjni_ff7.so from the user's
APK. Unicorn's JIT may need execution outside the macOS sandbox.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = pathlib.Path(sys.argv[1]).read_text() if len(sys.argv) > 1 else (root / 'source/ff7_wide.c').read_text()
source = re.sub(r'^#include .*$', '', source, flags=re.M)
# Host pointers exceed the game's 32-bit MEMMAP. Keep logical x86 addresses
# in a bounded fixture while exercising the real mode/quad accessors.
source = re.sub(r'static uint8_t \* x86\(uint32_t va\) \{.*?^\}',
                'static uint8_t * x86(uint32_t va) { return va < sizeof(x86_memory) ? x86_memory + va : NULL; }',
                source, flags=re.M | re.S)
so_util = (root / 'lib/so_util/so_util.h').read_text()
macro = re.search(r'#define SO_CONTINUE\(type, h, \.\.\.\).*?^\}\)', so_util, re.M | re.S).group()
stubs = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#define WIDE_EXTRA 104
#define l_info(...) ((void)0)
#define l_warn(...) ((void)0)
typedef struct { uintptr_t addr, thumb_addr; uint32_t orig_instr[4], patch_instr[4]; } so_hook;
typedef struct { uintptr_t text_base; } so_module;
so_module so_mod;
static uint8_t x86_memory[0x1000000];
static volatile uint32_t test_regs[8];
static volatile uint32_t * ff7_x86_regs(void) { return test_regs; }
static void profiler_hold(void) {}
static void profiler_release(void) {}
static void sceClibMemcpy(void *a, const void *b, size_t n) { if (a) memcpy(a,b,n); }
static void kuKernelFlushCaches(void *a, size_t n) {}
static uintptr_t so_symbol(so_module *m, const char *s) { return 0; }
static so_hook hook_addr(uintptr_t a, uintptr_t b) { return (so_hook){0}; }
static int ff7_screen_quad_installed(void) { return 1; }
static int ff7_screen_zoom_enabled(void) { return 1; }
static int ff7_fieldbg_installed(void) { return 1; }
static int viewport[4];
static void gq_glViewport(int x, int y, int w, int h) {
    viewport[0]=x; viewport[1]=y; viewport[2]=w; viewport[3]=h;
}
'''
held = r'''
#define SO_CONTINUE_HELD(type, h, ...) ({ \
    profiler_hold(); \
    type so_continue_held_r = SO_CONTINUE(type, h, ##__VA_ARGS__); \
    profiler_release(); \
    so_continue_held_r; \
})
'''
tests = r'''
static uint32_t captured[6];
static int original(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
    uint32_t values[] = {a,b,c,d,e,f};
    memcpy(captured, values, sizeof(values));
    return 123;
}
static void projection(float left, float right, float bottom, float top, int wide, int mode) {
    *(uint16_t *)(x86_memory + VA_MODE) = mode;
    uint32_t args[] = {bits(left),bits(right),bits(bottom),bits(top),bits(1),bits(-1)};
    want = active = wide;
    int result = ortho_hooked(args[0],args[1],args[2],args[3],args[4],args[5]);
    assert(result == 123);
    int canonical = (left == 0 && right == 640 || left == -104 && right == 744)
                    && top >= 0 && bottom > top && bottom <= 480;
    int wide_scene = wide && (mode == 1 || mode == 2 || mode == 3);
    for (int i=0; i<6; i++) {
        uint32_t expected = args[i];
        if (canonical && i<2) expected = wide_scene ? bits(i ? 744 : -104) : bits(i ? 640 : 0);
        if (canonical && wide_scene && mode==2 && top==0 && bottom==332 && i==2) expected=bits(480);
        if (captured[i] != expected) {
            fprintf(stderr,"ortho %.0f..%.0f, %.0f..%.0f: arg %d: got %08x expected %08x\n",
                    left,right,top,bottom,i,captured[i],expected);
            assert(captured[i] == expected);
        }
    }
    int local_window = wide_scene && left>=0 && right>left && right<=640 && top>=0 && bottom>top && bottom<=480;
    assert(rendering == (canonical ? wide_scene : local_window));
    if (wide_scene && canonical) {
        int scene_bottom=mode==2 && top==0 && bottom==332 ? 480 : (int)bottom;
        assert(viewport[0]==0 && viewport[1]==480-scene_bottom && viewport[2]==848
               && viewport[3]==scene_bottom-(int)top);
    }
    if (!canonical && local_window) assert(viewport[0]==(int)left+104 && viewport[1]==480-(int)bottom
                                           && viewport[2]==(int)(right-left) && viewport[3]==(int)(bottom-top));
}
static int cloud_allocate, cloud_new_frame;
static uint8_t cloud_baseline[128];
static int original_cloud(void) {
    if (cloud_allocate) {
        memcpy(x86_memory+0xa000,cloud_baseline,128);
        *(uint32_t *)(x86_memory+0x9070)=0xa000;
        if (cloud_new_frame) {
            *(uint32_t *)(x86_memory+0x9080)=0xa080;
            *(uint32_t *)(x86_memory+0x904c)=1;
            *(uint32_t *)(x86_memory+0x9054)+=1;
        } else {
            *(uint32_t *)(x86_memory+0x9080)+=128;
            *(uint32_t *)(x86_memory+0x904c)+=1;
        }
    }
    return 0;
}
static void clouds_test(void) {
    cloud_quad_hook.thumb_addr=(uintptr_t)original_cloud;
    regs=test_regs;
    regs[4]=0x7000;
    *(uint32_t *)(x86_memory+0x7004)=0x8000;
    *(uint32_t *)(x86_memory+0x8028)=0x9000;
    *(uint32_t *)(x86_memory+0x9040)=128;
    memset(cloud_baseline,0x57,128);
    for(int i=0;i<4;i++) {
        float x=i&1 ? 640 : 0;
        memcpy(cloud_baseline+32*i,&x,4);
    }
    for(int wide=0;wide<2;wide++) for(int mode=1;mode<=5;mode++) {
        *(uint16_t *)(x86_memory+VA_MODE)=mode;
        rendering=wide; cloud_allocate=1;
        cloud_quad_hooked();
        for(int i=0;i<4;i++) {
            float x;
            memcpy(&x,x86_memory+0xa000+32*i,4);
            assert(fabsf(x - (wide && mode==3 ? (i&1 ? 744 : -104) : (i&1 ? 640 : 0))) < .001f);
            assert(!memcmp(x86_memory+0xa004+32*i,cloud_baseline+32*i+4,28));
        }
        // A failed allocation must not expand the preceding cloud again.
        uint8_t before[128];
        memcpy(before,x86_memory+0xa000,128);
        cloud_allocate=0;
        cloud_quad_hooked();
        assert(!memcmp(before,x86_memory+0xa000,128));
        // Reused first slot on the next frame must still expand.
        *(uint32_t *)(x86_memory+0x9080)=0xa080;
        *(uint32_t *)(x86_memory+0x904c)=1;
        cloud_new_frame=1; cloud_allocate=1;
        cloud_quad_hooked();
        assert(!memcmp(before,x86_memory+0xa000,128));
        cloud_new_frame=0;
    }
}
static uint32_t layer_arg;
static int original_layer2(void) {
    layer_arg=read32(regs[4]+4);
    regs[4]+=4;
    return 1;
}
static void layer2_test(void) {
    field_layer2_hook.thumb_addr=(uintptr_t)original_layer2;
    regs=test_regs; layer2_ok=1;
    for(int on=0;on<2;on++) {
        active=on; regs[4]=0x7000;
        *(uint32_t *)(x86_memory+0x7004)=0x12340140;
        field_layer2_hooked();
        assert(layer_arg==(on ? 0x12340166 : 0x12340140));
        assert(regs[4]==0x7004);
        assert(read32(0x7004)==0x12340140);
    }
}
static void quads(void) {
    float p[6][6] = {{0,0},{640,0},{0,480},{0,480},{640,0},{640,480}};
    float saved[6][6];
    uint8_t colors[6][4] = {{0}};
    memcpy(saved,p,sizeof(p));
    rendering=0;
    ff7_wide_draw(4,6,(uint8_t*)p,24,(uint8_t*)colors,4,NULL,0);
    assert(!memcmp(p,saved,sizeof(p)));
    rendering=1;
    ff7_wide_draw(4,6,(uint8_t*)p,24,(uint8_t*)colors,4,NULL,0);
    for(int i=0;i<6;i++) assert(p[i][0] == (saved[i][0] ? 744 : -104));
    // A solid bottom HUD background must keep the original width.
    memcpy(p,saved,sizeof(p));
    for(int i=0;i<6;i++) if(p[i][1]==0) p[i][1]=332;
    memcpy(saved,p,sizeof(p));
    ff7_wide_draw(4,6,(uint8_t*)p,24,(uint8_t*)colors,4,NULL,0);
    assert(!memcmp(p,saved,sizeof(p)));
}
int main(int argc, char **argv) {
    static const uint32_t scales[] = {1,1};
    scale=scales;
    ortho_hook.thumb_addr=(uintptr_t)original;
    // The fallback path used to forward an uninitialized SO_CONTINUE local r.
    for(int wide=0;wide<2;wide++) for(int mode=0;mode<28;mode++) {
        projection(0,640,448,0,wide,mode);
        projection(0,640,332,0,wide,mode);
        projection(0,960,544,0,wide,mode);
        projection(10,630,462,51,wide,mode);
        projection(0,640,480,0,wide,mode);
        projection(-104,744,448,0,wide,mode);
        projection(0,640,462,51,wide,mode);
        projection(320,322,390,388,wide,mode);
        projection(56,176,448,332,wide,mode);
    }
    quads();
    clouds_test();
    layer2_test();
#ifdef SKY_LEFT_SITE
    static uint8_t text[SKY_RIGHT_SITE+2];
    so_mod.text_base=(uintptr_t)text;
    sky_ok=1; layer2_ok=1; active=0; want=1;
    FILE *out=fopen(argv[1],"wb");
    assert(out);
    apply();
    fwrite(text+SKY_LEFT_SITE,1,6,out);
    fwrite(text+LAYER2_ORIGIN,1,4,out);
    fwrite(text+LAYER2_LEFT,1,4,out);
    want=0;
    apply();
    fwrite(text+SKY_LEFT_SITE,1,6,out);
    fwrite(text+LAYER2_ORIGIN,1,4,out);
    fwrite(text+LAYER2_LEFT,1,4,out);
    fclose(out);
#endif
    puts("widescreen: partial-height projections, menu modes, cloud quads/reused buffers and fades OK");
}
'''
with tempfile.TemporaryDirectory(prefix='ff7-wide-') as d:
    c = pathlib.Path(d) / 'wide.c'
    exe = pathlib.Path(d) / 'wide'
    c.write_text(stubs + macro + '\n' + held + source + tests)
    cmd = ['cc', '-std=gnu11', '-O3', '-ffast-math', '-Wno-deprecated-non-prototype', '-Wno-int-to-pointer-cast']
    subprocess.run(cmd + [str(c), '-lm', '-o', str(exe)], check=True)
    output = pathlib.Path(d) / 'sky.bin'
    subprocess.run([str(exe), str(output)], check=True)
    if output.exists():
        from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB
        from unicorn.arm_const import UC_ARM_REG_R7, UC_ARM_REG_R8
        data = output.read_bytes()
        so = (root / 'build/libjni_ff7.so').read_bytes()
        assert data[14:20] == so[0x61921c:0x619222], "4:3 must restore original sky instructions"
        for code, half in ((data[:6], 240), (data[14:20], 180)):
            uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
            uc.mem_map(0x1000, 0x1000)
            uc.mem_write(0x1000, code)
            uc.emu_start(0x1001, 0x1006, count=2)
            assert uc.reg_read(UC_ARM_REG_R8) == 0x10000 - half
            assert uc.reg_read(UC_ARM_REG_R7) == half
        print('sky: emitted ARM instructions produce +/-240 and restore original +/-180')

        from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R2, UC_ARM_REG_R3
        for code, origin, width in ((data[6:14],358,412),(data[20:28],320,336)):
            uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB)
            uc.mem_map(0x1000,0x1000); uc.mem_write(0x1000,code)
            uc.reg_write(UC_ARM_REG_R3,500); uc.reg_write(UC_ARM_REG_R0,320)
            uc.emu_start(0x1001,0x1008,count=2)
            assert uc.reg_read(UC_ARM_REG_R2)==(origin-500)&0xffffffff
            assert uc.reg_read(UC_ARM_REG_R0)==(320-width)&0xffffffff
        assert data[20:24]==so[0x2756a2:0x2756a6]
        assert data[24:28]==so[0x275942:0x275946]
        print('second field layer: expanded window instructions and original restoration OK')
