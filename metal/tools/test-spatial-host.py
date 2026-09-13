#!/usr/bin/env python3
"""Compile and exercise the exact pure canvas primitives in kernel64.c."""
from pathlib import Path
import subprocess, tempfile
s=(Path(__file__).resolve().parents[1]/'kernel/kernel64.c').read_text()
assert '/* WM_SPATIAL_MATH_BEGIN */' in s, 'real-app canvas transform not implemented'
a=s.split('/* WM_SPATIAL_MATH_BEGIN */')[1].split('/* WM_SPATIAL_MATH_END */')[0]
t='''
int main(void) {
 for(int z=128;z<=2048;z+=128) for(int p=-10000;p<=10000;p+=137) {
  int q=sp_project(p,-319,z), r=sp_unproject(q,-319,z);
  assert(abs(r-p)<=1024/z+1);
 }
 assert(sp_lod(0,358)==1); assert(sp_lod(1,380)==1);
 assert(sp_lod(1,410)==0); assert(sp_lod(0,380)==0);
 int p=0,v=0; for(int i=0;i<500;i++)sp_spring(&p,&v,10000,1,0);
 assert(abs(p-10000)<16); assert(abs(v)<16);
 int light=0,heavy=0,lv=0,hv=0;
 sp_spring(&light,&lv,10000,1,0);sp_spring(&heavy,&hv,10000,16,0);
 assert(light>heavy && heavy>0);
 sp_spring(&p,&v,-10000,16,1);assert(p==-10000&&v==0);
 for(int i=0;i<100000;i++)sp_spring(&p,&v,i&1?100000000:-100000000,16,0);
 assert(abs(v)<=8192);assert(abs(p)<=1073741824);
 puts("PASS spatial: signed transform, LOD hysteresis, mass, convergence, reduced motion, bounds");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'t.c').write_text('#include <stdint.h>\n#include <stdlib.h>\n#include <assert.h>\n#include <stdio.h>\n'+a+t)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-fsanitize=undefined,address',str(p/'t.c'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
# Reuse (without editing) the existing real-function host harness and add
# assertions through the same draw/control/content path with canvas enabled.
h=(Path(__file__).with_name('test-wm-host.py')).read_text()
checks=r'''
    memset(g_wmwin,0,sizeof g_wmwin);
    w=&g_wmwin[0]; w->used=1;w->owner=1;w->x=-240;w->y=120;
    w->w=400;w->h=280;w->cw=396;w->ch=257;w->z=4;
    g_sp_enabled=1;g_sp_zoom=512;g_sp_x=-600;g_sp_y=-200;
    struct wmwin R=wm_screen_rect(w);
    assert(wm_topmost_at(R.x+10,R.y+10)==0);
    assert(wm_topmost_at(R.x-1,R.y)==-1);
    int ax,ay; assert(wm_content_point(w,R.x+2,R.y+WIN_TITLE_H+1,&ax,&ay));
    assert(ax==0&&ay==0);
    assert(wm_content_point(w,R.x+R.w-3,R.y+R.h-3,&ax,&ay));
    assert(ax>=w->cw-5&&ay>=w->ch-5);
    g_sp_glyph=1;g_sp_zoom=256;R=wm_screen_rect(w);
    assert(R.w==148 && R.h==76);
    for(int c=1;c<=3;c++)assert(wm_control_at(w,R.x+wm_control_x(&R,c)+5,R.y+6)==c);
    wimp_pointer(R.x+20,R.y+35,1);assert(g_wm_focus==0&&g_wm_drag==0);
    wimp_pointer(R.x+20,R.y+35,0);
    /* bx/by already exist in this scope: these checks are spliced into the
     * SAME main() as test-wm-host.py's, which declares them at its line 104.
     * Re-declaring shadowed them and failed the build under -Werror, so the
     * whole spatial suite could not run. Assign, do not redeclare. */
    bx=R.x+wm_control_x(&R,3)+5;by=R.y+6;
    wimp_pointer(bx,by,1);wimp_pointer(bx,by,0);assert(w->minimized);
    w->minimized=0;R=wm_screen_rect(w);bx=R.x+wm_control_x(&R,1)+5;
    wimp_pointer(bx,by,1);assert(w->used);wimp_pointer(bx,by,0);assert(!w->used);
    puts("PASS spatial integration: projected hit, surface inverse, glyph focus/drag/minimize/close");
'''
h=h.replace("font = Path(__file__)","tests=tests.replace('    return 0;',checks+'    return 0;')\nfont = Path(__file__)")
exec(compile(h,str(Path(__file__).with_name('test-wm-host.py')),'exec'),{'__file__':str(Path(__file__).with_name('test-wm-host.py')),'checks':checks})
