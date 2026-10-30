#!/usr/bin/env python3
"""Exercise the real final frame compositor with Vita-sized quad fixtures."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = re.sub(r'^#include .*$', '', (root/'source/ff7_screen.c').read_text(), flags=re.M)
stubs = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define DATA_PATH ""
#define WIDE_EXTRA 104
#define l_info(...) ((void)0)
#define l_warn(...) ((void)0)
#define SO_CONTINUE_HELD(type,h,...) ((type)0)
typedef struct { uintptr_t addr; } so_hook;
typedef struct { uintptr_t text_base; } so_module;
so_module so_mod;
static int wide_fixture, movie_fixture, wide_option=1;
volatile uint64_t ff7_bg_last_us=1000;
static uint64_t sceKernelGetProcessTimeWide(void) { return 1001; }
static int ff7_wide_rendering(void) { return wide_fixture; }
static int ff7_wide_get(void) { return wide_option; }
static int movie_is_playing(void) { return movie_fixture; }
static int movie_video_height(void) { return 448; }
static uintptr_t so_symbol(so_module *m, const char *s) { return 0; }
static so_hook hook_addr(uintptr_t a,uintptr_t b) { return (so_hook){0}; }
static int file_exists(const char *s) { return 0; }
'''
tests = r'''
static float quad[6][6];
static uint8_t *end;
static const int side[6]={0,1,0,0,1,1}, bottom[6]={0,0,1,1,0,1};
static void reset(void) {
    memset(quad,0,sizeof(quad));
    end=(uint8_t *)(quad+6); cur_vertex=&end;
    for(int i=0;i<6;i++) {
        quad[i][0]=480+(side[i] ? 1 : -1)*(640.0f*544/480)/2;
        quad[i][1]=bottom[i]*544.0f;
        quad[i][4]=side[i]*(640.0f/1024);
        quad[i][5]=bottom[i]*(480.0f/512);
    }
    zoom_enabled=1; stretch=0; field_lines=448;
}
static void eq(float a,float b) { assert(fabsf(a-b)<0.005f); }
static void fullscreen(int mode,float lines,float shown) {
    reset(); frame_quad_adjust(mode);
    for(int i=0;i<6;i++) {
        eq(quad[i][0],side[i]*960.0f);
        eq(quad[i][1],bottom[i]*544.0f*480/lines);
        eq(quad[i][4],(424+(side[i]?1:-1)*shown/2)/1024);
    }
    eq(zoom_left,0); eq(zoom_right,960); eq(zoom_center,480);
}
int main(void) {
    wide_fixture=1;
    fullscreen(1,448,448.0f*960/544); // available field art fills the display
    // Field dialogue must have the same physical scale in both axes,
    // independently of whether art exists outside the original 640 pixels.
    float field_u0=quad[0][4], field_u1=quad[1][4];
    float horizontal_scale=960.0f/((field_u1-field_u0)*1024);
    eq(horizontal_scale,544.0f/448);
    fullscreen(2,472,472.0f*960/544);
    // Original HUD x=0..640 stays centered inside the wide battle source.
    float u0=quad[0][4], u1=quad[1][4];
    float hud_l=(104.0f/1024-u0)/(u1-u0)*960;
    float hud_r=(744.0f/1024-u0)/(u1-u0)*960;
    eq(hud_l+hud_r,960);
    eq(hud_r-hud_l,640.0f*544/472);
    fullscreen(3,480,480.0f*960/544);
    // All menu/save modes ignore the fresh field timestamp, even if the
    // previous frame happened to have a wide framebuffer.
    for(int mode=0;mode<28;mode++) if(mode!=1 && mode!=2 && mode!=3 && mode!=23) {
        for(int old_wide=0;old_wide<2;old_wide++) {
            wide_fixture=old_wide; reset(); frame_quad_adjust(mode);
            assert(!zoomed);
            eq(quad[0][0],480-640.0f*544/480/2);
            eq(quad[1][0],480+640.0f*544/480/2);
            eq(quad[2][1],544);
            eq(quad[0][4],old_wide ? 104.0f/1024 : 0);
            eq(quad[1][4],(old_wide ? 744.0f : 640.0f)/1024);
        }
    }
    // Entry from either field or world: stretch only the 640-pixel effect.
    for(int from=1;from<=3;from+=2) for(int padded=0;padded<2;padded++) {
        wide_fixture=1; reset(); frame_quad_adjust(from);
        wide_fixture=padded; reset(); frame_quad_adjust(23);
        assert(zoomed==5);
        eq(quad[0][0],0); eq(quad[1][0],960); eq(quad[2][1],544);
        eq(quad[0][4],padded ? 104.0f/1024 : 0);
        eq(quad[1][4],(padded ? 744.0f : 640.0f)/1024);
        eq(zoom_left,0); eq(zoom_right,960);
    }
    wide_option=0; wide_fixture=0; reset(); frame_quad_adjust(23);
    assert(!zoomed); eq(quad[1][0]-quad[0][0],640.0f*544/480);
    wide_option=1;
    wide_fixture=0; movie_fixture=1; reset(); frame_quad_adjust(1);
    assert(zoomed==2); eq(quad[1][4],640.0f/1024);
    puts("screen: full-width fields/world/battle, centered original HUD, 4:3 saves/menus, movies and full-width battle transitions OK");
}
'''
with tempfile.TemporaryDirectory(prefix='ff7-screen-') as d:
    c=Path(d)/'screen.c'; exe=Path(d)/'screen'
    c.write_text(stubs+source+tests)
    subprocess.run(['cc','-std=gnu11','-O3','-ffast-math','-Wno-int-to-pointer-cast',str(c),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
