#!/usr/bin/env python3
"""Exercise actual kernel WM functions, without hardware or QEMU.
Run: python3 tools/test-wm-host.py (Linux with cc).
No copied WM algorithms: functions/structs are extracted from kernel64.c.
"""
from pathlib import Path
import subprocess
import tempfile

src = (Path(__file__).resolve().parents[1] / 'kernel/kernel64.c').read_text()
def definition(name):
    import re
    m = re.search(r'^static (?:inline )?[^\n]+\b' + name + r'\([^;\n]*\) \{', src, re.M)
    assert m, f'Missing kernel function: {name}'
    start = m.start(); pos = src.index('{', start); depth = 1; end = pos + 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}'); end += 1
    return src[start:end] + '\n'
def struct(name):
    start = src.index('struct ' + name + ' {')
    return src[start:src.index('\n};', start)+3] + '\n'
pre = '''#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#define NWMWIN 16
#define NWIDGET 32
#define WG_TEXTLEN 24
#define WG_LABEL 1
#define WG_BUTTON 2
#define WG_CHECK 3
#define WG_PROGRESS 4
#define WG_ENTRY 5
#define WIN_TITLE_H 20
#define WIN_TASKBAR_H 28
#define WIN_MIN_W 120
#define WIN_MIN_H 80
#define WIMP_CLOSE_W 14
#define WIMP_MIN_W 14
#define DESK_RAIL_W 112
struct sevent { int type,x,y,code; };
static int g_wm_lock;
static void klock_acquire(int *p) {(void)p;}
static void klock_release(int *p) {(void)p;}
#define barrier() ((void)0)
static void kprintf(const char *fmt, ...) {(void)fmt;}
#define C_OBS0 0x080C14
#define C_OBS1 0x101820
#define C_OBS2 0x182430
#define C_HAIR 0x334455
#define C_MUTE 0x667788
#define C_TEXT 0xFFFFFF
#define C_MAGE 0xFF00FF
#define C_AMBER 0xFFAA00
static uint32_t pixels[768][1024];
static void px(int x,int y,uint32_t c) {if(x>=0&&x<1024&&y>=0&&y<768)pixels[y][x]=c;}
static void blend(int x,int y,uint32_t c,int a) {
    if(x<0||y<0||x>=1024||y>=768) return;
    uint32_t out=0,bg=pixels[y][x];
    for(int s=0;s<=16;s+=8) out|=((((c>>s)&255)*a+((bg>>s)&255)*(255-a))/255)<<s;
    px(x,y,out);
}
static void rect(int x,int y,int w,int h,uint32_t c) {for(int j=0;j<h;j++)for(int i=0;i<w;i++)px(x+i,y+j,c);}
static void hline(int x,int y,int w,uint32_t c) {rect(x,y,w,1,c);}
static void vline(int x,int y,int h,uint32_t c) {rect(x,y,1,h,c);}
static void draw_char(int x,int y,char ch,uint32_t c) {(void)x;(void)y;(void)ch;(void)c;}
static void draw_str(int x,int y,const char *s,uint32_t c) {(void)x;(void)y;(void)s;(void)c;}
static int desk_w(void) {return 1024;}
static int desk_h(void) {return 768;}
static int desk_chip_at(int x,int y) {(void)x;(void)y;return -1;}
static int desk_launch_at(int x,int y) {(void)x;(void)y;return -1;}
static int desk_launch(int i) {return i;}
'''
state = '''static struct wmwin g_wmwin[NWMWIN];
static struct widget g_wg[NWIDGET];
static int g_wm_focus=-1, g_wm_znext=1;
static int g_wm_drag=-1, g_wm_drag_dx, g_wm_drag_dy;
static uint64_t g_wg_clicks, g_ticks;
'''
extra = ''
if '/* WM_XP_STATE_BEGIN */' in src:
    extra += src.split('/* WM_XP_STATE_BEGIN */')[1].split('/* WM_XP_STATE_END */')[0]
helpers = ['wg_release_win','wm_topmost_at','wm_raise','wm_focus']
if 'static void wm_focus_next(' in src: helpers += ['wm_focus_next']
helpers += ['wm_queue_event','wm_destroy']
if '/* WM_XP_HELPERS_BEGIN */' in src:
    extra_helpers = src.split('/* WM_XP_HELPERS_BEGIN */')[1].split('/* WM_XP_HELPERS_END */')[0]
else: extra_helpers = ''
tests = '''int main(void) {
    struct wmwin *w=&g_wmwin[0];
    w->used=1; w->owner=0; w->x=100; w->y=100; w->w=300; w->h=220;
    w->cw=296; w->ch=197; w->z=1;
    wm_focus(0);
    int x=w->x+w->w-WIMP_CLOSE_W-1, y=w->y+5;
    wimp_pointer(x,y,1);
    assert(w->used && "close must wait for release");
    wimp_pointer(x-40,y+30,0);
    assert(w->used && "release outside cancels close");
    wimp_pointer(x,y,1); wimp_pointer(x,y,0);
    assert(!w->used && "release inside closes");
    puts("PASS: close release activation and cancel");
    w->used=1; w->cw=296; w->ch=197;
    wimp_draw_window(w,1);
    int bx=w->x+wm_control_x(w,1), by=w->y+3;
    uint32_t red=pixels[by+2][bx+2];
    assert(((red>>16)&255)>((red>>8)&255)*2 && ((red>>16)&255)>(red&255)*2);
    assert(pixels[by+4][bx+4]==0xFFFFFF && "close needs white diagonal X");
    assert(pixels[w->y+3][w->x+20]!=pixels[w->y+16][w->x+20] && "active title must be gradient");
    uint32_t normal=pixels[by+2][bx+2];
    w->hover_control=1; wimp_draw_window(w,1);
    assert(pixels[by+2][bx+2]!=normal && "hover must be visible");
    uint32_t hover=pixels[by+2][bx+2];
    w->pressed_control=1; wimp_draw_window(w,1);
    assert(pixels[by+2][bx+2]!=hover && "press must be visible");
    puts("PASS: red close X, title gradient, hover and press pixels");
    w->qw=w->qr=0; w->pressed_control=0;
    wimp_pointer(w->x+2,w->y+WIN_TITLE_H+1,1);
    wimp_pointer(w->x+2,w->y+WIN_TITLE_H+1,0);
    assert(w->qw==1 && w->q[0].x==2 && w->q[0].y==1 && "preserve raw mouse ABI offsets");
    static uint32_t page[1024]={0x112233,0x445566,0x778899,0xaabbcc};
    uint64_t pt=(uint64_t)(uintptr_t)page;
    w->ppage=&pt; w->cpages=1; w->cw=2; w->ch=2;
    wimp_draw_content(w,0,0,4,4);
    assert(pixels[0][0]==0x112233 && pixels[1][1]==0x112233);
    assert(pixels[3][3]==0xaabbcc && "resized content must actually fill its frame");
    puts("PASS: surface resampling and legacy mouse ABI");
    return 0;
}
'''
font = Path(__file__).resolve().parents[2] / 'apps/ui_font.h'
code = pre + '\n#include "' + str(font) + '"\n' + struct('wmwin') + struct('widget') + state + extra
code += ''.join(definition(n) for n in helpers) + extra_helpers + definition('wimp_pointer')
if 'static uint32_t wm_color_mix(' in src:
    code += definition('wm_color_mix') + definition('wimp_draw_control')
if 'static void wm_ui_text(' in src:
    code += definition('wm_ui_text')
code += definition('wimp_draw_content') + definition('wimp_draw_window') + tests
with tempfile.TemporaryDirectory(prefix='outrun-wm-test-') as tmp:
    c=Path(tmp)/'test.c'; exe=Path(tmp)/'test'; c.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-variable','-fsanitize=undefined,address','-g',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
