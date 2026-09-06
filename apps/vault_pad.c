/* VAULT PAD: byte-oriented native text editor. No libc dependency. */
#ifndef VAULT_PAD_CORE
#define VAULT_PAD_CORE
#define VP_CAP (256 * 1024)
#define VP_NAME_CAP 63
#define VP_CLIP_CAP 1024
struct vp_editor {
    char text[VP_CAP+1], filename[VP_NAME_CAP+1];
    /* One-line clipboard, and the name of the file this buffer was last opened
     * from or saved to. A save whose target exists and is not `origin` asks
     * first; `confirm` is that pending question, and any other keystroke
     * withdraws it. */
    char clip[VP_CLIP_CAP+1], origin[VP_NAME_CAP+1];
    int len, cursor, dirty, top, left, name_len, name_cursor, focus, command, confirm;
    const char *status;
};
static void vp_init(struct vp_editor *e) {
    e->text[0]=0; e->len=e->cursor=e->dirty=e->top=e->left=0;
    e->focus=e->command=e->confirm=0; e->status="READY";
    e->clip[0]=0; e->origin[0]=0;
    const char *s="notes.txt"; int i=0;
    do { e->filename[i]=s[i]; } while(s[i++]);
    e->name_len=e->name_cursor=i-1;
}
static int vp_insert(struct vp_editor *e, int c) {
    if(e->len>=VP_CAP) { e->status="BUFFER FULL"; return 0; }
    for(int i=e->len;i>=e->cursor;i--) e->text[i+1]=e->text[i];
    e->text[e->cursor++]=(char)c; e->len++; e->dirty=1; return 1;
}
enum { VP_LEFT, VP_RIGHT, VP_UP, VP_DOWN, VP_HOME, VP_END };
static int vp_start(const struct vp_editor *e,int p) {
    while(p>0 && e->text[p-1]!='\n') p--;
    return p;
}
static int vp_end(const struct vp_editor *e,int p) {
    while(p<e->len && e->text[p]!='\n') p++;
    return p;
}
static void vp_move(struct vp_editor *e,int dir) {
    int p=e->cursor, start=vp_start(e,p), col=p-start, end=vp_end(e,p);
    if(dir==VP_LEFT && p>0) p--;
    if(dir==VP_RIGHT && p<e->len) p++;
    if(dir==VP_HOME) p=start;
    if(dir==VP_END) p=end;
    if(dir==VP_UP && start>0) {
        int prev=vp_start(e,start-1); p=prev+col; if(p>start-1) p=start-1;
    }
    if(dir==VP_DOWN && end<e->len) {
        int nextend=vp_end(e,end+1); p=end+1+col; if(p>nextend) p=nextend;
    }
    e->cursor=p;
}
static void vp_delete(struct vp_editor *e,int back) {
    if(back) { if(!e->cursor) return; e->cursor--; }
    else if(e->cursor==e->len) return;
    for(int i=e->cursor;i<e->len;i++) e->text[i]=e->text[i+1];
    e->len--; e->dirty=1;
}
static void vp_position(const struct vp_editor *e,int *row,int *col) {
    *row=*col=0;
    for(int i=0;i<e->cursor;i++) {
        if(e->text[i]=='\n') { (*row)++; *col=0; } else (*col)++;
    }
}
/* Copy the cursor line (without its newline) into the clipboard. A line that
 * does not fit is refused whole rather than silently cut short. */
static int vp_yank(struct vp_editor *e) {
    int start=vp_start(e,e->cursor), end=vp_end(e,e->cursor), n=end-start;
    if(n>VP_CLIP_CAP) { e->status="LINE TOO LONG FOR CLIPBOARD"; return 0; }
    for(int i=0;i<n;i++) e->clip[i]=e->text[start+i];
    e->clip[n]=0; e->status="LINE COPIED"; return 1;
}
/* Yank, then remove the cursor line together with one adjoining newline so
 * the surrounding lines close up. The last line takes the newline before it. */
static void vp_cut(struct vp_editor *e) {
    if(!e->len) return;
    if(!vp_yank(e)) return;
    int start=vp_start(e,e->cursor), end=vp_end(e,e->cursor);
    if(end<e->len) end++;
    else if(start>0) start--;
    int n=end-start;
    for(int i=start;i+n<=e->len;i++) e->text[i]=e->text[i+n];
    e->len-=n; e->cursor=start; e->dirty=1; e->status="LINE CUT";
}
/* Insert the clipboard as a new line below the cursor line. */
static void vp_paste(struct vp_editor *e) {
    int n=0; while(e->clip[n]) n++;
    if(!n) { e->status="CLIPBOARD EMPTY"; return; }
    if(e->len+n+1>VP_CAP) { e->status="BUFFER FULL"; return; }
    int at=vp_end(e,e->cursor);
    int extra=e->len?n+1:n;
    for(int i=e->len;i>=at;i--) e->text[i+extra]=e->text[i];
    int p=at;
    if(e->len) e->text[p++]='\n';
    for(int i=0;i<n;i++) e->text[p++]=e->clip[i];
    e->len+=extra; e->cursor=at+(e->len>extra?1:0); e->dirty=1; e->status="LINE PASTED";
}
static void vp_visible(struct vp_editor *e,int rows,int cols) {
    int row,col; vp_position(e,&row,&col);
    if(rows<1) rows=1;
    if(cols<1) cols=1;
    if(row<e->top) e->top=row;
    if(row>=e->top+rows) e->top=row-rows+1;
    if(col<e->left) e->left=col;
    if(col>=e->left+cols) e->left=col-cols+1;
}
/* OutRun ABI: OPEN flags 0 = existing, 1 = create; never use TRUNC on open.
 * IO is injected so the real editor/save loops run in host tests as well. */
typedef long long (*vp_io_fn)(int,unsigned long long,unsigned long long,unsigned long long);
static char vp_staging[VP_CAP+1];
static int vp_same_name(const char *a,const char *b) {
    int i=0; while(a[i] && a[i]==b[i]) i++;
    return a[i]==b[i];
}
static void vp_adopt(struct vp_editor *e) {
    int i=0; do { e->origin[i]=e->filename[i]; } while(e->filename[i++]);
}
static int vp_open(struct vp_editor *e,vp_io_fn io) {
    e->confirm=0;
    if(e->dirty) { e->status="UNSAVED: SAVE BEFORE OPEN"; return 0; }
    if(!e->filename[0]) { e->status="FILENAME REQUIRED"; return 0; }
    long long fd=io(5,(unsigned long long)e->filename,0,0);
    if(fd<0) { e->status="OPEN FAILED"; return 0; }
    int n=0,ok=1;
    for(;;) {
        int want=VP_CAP+1-n; if(want>65536) want=65536;
        long long r=io(6,fd,(unsigned long long)(vp_staging+n),want);
        if(r<0 || r>want) { ok=0; break; }
        if(!r) break;
        n+=(int)r;
        if(n>VP_CAP) { ok=0; break; }
    }
    if(io(8,fd,0,0)<0) ok=0;
    if(!ok) { e->status="READ FAILED OR FILE TOO LARGE"; return 0; }
    for(int i=0;i<n;i++) e->text[i]=vp_staging[i];
    e->text[n]=0; e->len=n; e->cursor=e->top=e->left=0;
    e->dirty=0; e->status="OPENED"; vp_adopt(e); return 1;
}
static int vp_save(struct vp_editor *e,vp_io_fn io) {
    if(!e->filename[0]) { e->confirm=0; e->status="FILENAME REQUIRED"; return 0; }
    /* Overwrite guard: a target that already exists and is not the file this
     * buffer came from is asked about once. The probe opens with flags 0
     * (existing only), so it cannot create anything as a side effect. */
    if(!e->confirm && !vp_same_name(e->filename,e->origin)) {
        long long probe=io(5,(unsigned long long)e->filename,0,0);
        if(probe>=0) {
            io(8,probe,0,0);
            e->confirm=1; e->status="FILE EXISTS: SAVE AGAIN TO OVERWRITE"; return 0;
        }
    }
    e->confirm=0;
    long long fd=io(5,(unsigned long long)e->filename,1,0);
    if(fd<0) { e->status="SAVE OPEN FAILED"; return 0; }
    int n=0,ok=1;
    while(n<e->len) {
        int want=e->len-n; if(want>65536) want=65536;
        long long r=io(7,fd,(unsigned long long)(e->text+n),want);
        if(r<=0 || r>want) { ok=0; break; }
        n+=(int)r;
    }
    if(ok && io(101,fd,e->len,0)<0) ok=0;
    if(io(8,fd,0,0)<0) ok=0;
    if(ok && io(22,0,0,0)<0) ok=0;
    if(!ok) { e->status="SAVE FAILED: DISK MAY BE PARTIAL"; return 0; }
    e->dirty=0; e->status="SAVED"; vp_adopt(e); return 1;
}
enum { VP_NONE, VP_OPEN, VP_SAVE };
/* The shipping PS/2 map emits ASCII only, not arrows or Ctrl chords.
 * Escape toggles a documented command mode, usable on the actual keyboard. */
static int vp_key(struct vp_editor *e,int c) {
    /* Every key but a repeated save withdraws a pending overwrite question. */
    if(!(e->command && c=='s')) e->confirm=0;
    if(c==27) { e->command=!e->command; return VP_NONE; }
    if(c==9) { e->focus=!e->focus; e->command=0; return VP_NONE; }
    if(e->command) {
        if(c=='s') return VP_SAVE;
        if(c=='o') return VP_OPEN;
        if(e->focus) {
            if(c=='h' && e->name_cursor>0) e->name_cursor--;
            if(c=='l' && e->name_cursor<e->name_len) e->name_cursor++;
            if(c=='0') e->name_cursor=0;
            if(c=='$') e->name_cursor=e->name_len;
            if(c=='x' && e->name_cursor<e->name_len) {
                for(int i=e->name_cursor;i<e->name_len;i++) e->filename[i]=e->filename[i+1];
                e->name_len--;
            }
        } else {
            if(c=='h') vp_move(e,VP_LEFT);
            if(c=='l') vp_move(e,VP_RIGHT);
            if(c=='k') vp_move(e,VP_UP);
            if(c=='j') vp_move(e,VP_DOWN);
            if(c=='0') vp_move(e,VP_HOME);
            if(c=='$') vp_move(e,VP_END);
            if(c=='g') e->cursor=0;
            if(c=='G') e->cursor=e->len;
            if(c=='x') vp_delete(e,0);
            if(c=='y') vp_yank(e);
            if(c=='D') vp_cut(e);
            if(c=='p') vp_paste(e);
            if(c=='u' || c=='d') for(int i=0;i<12;i++) vp_move(e,c=='u'?VP_UP:VP_DOWN);
        }
        return VP_NONE;
    }
    if(e->focus) {
        if(c==10 || c==13) { e->focus=0; return VP_NONE; }
        if(c==8 && e->name_cursor>0) {
            e->name_cursor--;
            for(int i=e->name_cursor;i<e->name_len;i++) e->filename[i]=e->filename[i+1];
            e->name_len--;
        } else if(c>=32 && c<=126 && e->name_len<VP_NAME_CAP) {
            for(int i=e->name_len;i>=e->name_cursor;i--) e->filename[i+1]=e->filename[i];
            e->filename[e->name_cursor++]=(char)c; e->name_len++;
        }
    } else {
        if(c==8) vp_delete(e,1);
        else if(c==127) vp_delete(e,0);
        else if(c==10 || c==13) vp_insert(e,'\n');
        else if(c>=32 && c<=126) vp_insert(e,c);
    }
    return VP_NONE;
}
static int vp_click(struct vp_editor *e,int x,int y,int w,int h) {
    if(x<0 || y<0 || x>=w || y>=h) return VP_NONE;
    if(y>=8 && y<32) {
        if(x>=w-144 && x<w-80) return VP_OPEN;
        if(x>=w-72 && x<w-8) return VP_SAVE;
        if(x>=72 && x<w-152) {
            e->focus=1; e->command=0;
            int cols=(w-232)/8, left=e->name_cursor>=cols?e->name_cursor-cols+1:0;
            e->name_cursor=left+(x-72)/8;
            if(e->name_cursor>e->name_len) e->name_cursor=e->name_len;
        }
    } else if(y>=56 && y<h-48 && x>=8 && x<w-8) {
        e->focus=0; e->command=0;
        int row=e->top+(y-56)/16, col=e->left+(x-8)/8, p=0;
        while(row>0 && p<e->len) { if(e->text[p++]=='\n') row--; }
        int end=vp_end(e,p); p+=col; if(p>end) p=end; e->cursor=p;
    }
    return VP_NONE;
}
typedef void (*vp_draw_fn)(void *,int,int,int,int,unsigned,int);
static void vp_label(vp_draw_fn draw,void *ctx,int x,int y,const char *s,unsigned color,int max) {
    for(int i=0;s[i] && i<max;i++) draw(ctx,x+8*i,y,8,16,color,(unsigned char)s[i]);
}
static void vp_number(vp_draw_fn draw,void *ctx,int x,int y,int n) {
    char buf[12]; int i=0;
    do { buf[i++]=(char)('0'+n%10); n/=10; } while(n);
    for(int j=0;j<i;j++) draw(ctx,x+j*8,y,8,16,0xb8c8df,buf[i-j-1]);
}
static void vp_paint(struct vp_editor *e,int w,int h,vp_draw_fn draw,void *ctx) {
    if(w<320 || h<160) return;
    int rows=(h-104)/16, cols=(w-16)/8;
    vp_visible(e,rows,cols);
    draw(ctx,0,0,w,h,0x101522,0);
    draw(ctx,72,8,w-224,24,e->focus?0x29394f:0x1c2636,0);
    vp_label(draw,ctx,8,12,"FILE",0x8ba4bc,8);
    int nc=(w-232)/8, nl=e->name_cursor>=nc?e->name_cursor-nc+1:0;
    vp_label(draw,ctx,72,12,e->filename+nl,0xe6f2ff,nc);
    draw(ctx,w-144,8,64,24,0x28495d,0); draw(ctx,w-72,8,64,24,0x533a63,0);
    vp_label(draw,ctx,w-136,12,"OPEN",0xffffff,6);
    vp_label(draw,ctx,w-64,12,"SAVE",0xffffff,6);
    vp_label(draw,ctx,8,36,"ESC cmd: hjkl 0/$ g/G move | x del | y/D/p copy/cut/paste line | s/o save/open",0x8ba4bc,cols);
    int row=0,col=0;
    for(int i=0;i<e->len && row<e->top+rows;i++) {
        unsigned char c=(unsigned char)e->text[i];
        if(c=='\n') { row++; col=0; continue; }
        if(row>=e->top && col>=e->left && col<e->left+cols)
            draw(ctx,8+(col-e->left)*8,56+(row-e->top)*16,8,16,0xe6f2ff,c>=32 && c<=126?c:'.');
        col++;
    }
    vp_position(e,&row,&col);
    if(e->focus) draw(ctx,72+(e->name_cursor-nl)*8,28,8,2,0xffcc66,0);
    else draw(ctx,8+(col-e->left)*8,56+(row-e->top)*16,2,16,0xffcc66,0);
    draw(ctx,0,h-48,w,48,0x1c2636,0);
    vp_label(draw,ctx,8,h-44,e->dirty?"* MODIFIED":"  SAVED",0xffcc66,12);
    vp_label(draw,ctx,112,h-44,e->command?"COMMAND":"INSERT",0xc4a6ff,8);
    vp_label(draw,ctx,192,h-44,"LN",0xb8c8df,2); vp_number(draw,ctx,216,h-44,row+1);
    vp_label(draw,ctx,280,h-44,"COL",0xb8c8df,3); vp_number(draw,ctx,312,h-44,col+1);
    vp_label(draw,ctx,8,h-24,e->status,0xb8c8df,cols);
    vp_label(draw,ctx,w-176,h-24,"TAB: filename / text",0x8ba4bc,21);
}
#endif

#ifndef APP_HOST_TEST
#include "gui.h"
static struct vp_editor vp_app;
static long long vp_system(int op,unsigned long long a,unsigned long long b,unsigned long long c) {
    return (long long)sysc(op,a,b,c);
}
static void vp_native_draw(void *ctx,int x,int y,int w,int h,unsigned color,int c) {
    struct app_win *win=(struct app_win *)ctx;
    if(c) app_char(win,x,y,(char)c,color); else app_rect(win,x,y,w,h,color);
}
void _start(void) {
    struct app_win win;
    if(app_create(&win,720,480,0xc4a6ff)<0) app_exit(1);
    app_title(&win,"VAULT PAD"); vp_init(&vp_app);
    vp_paint(&vp_app,win.cw,win.ch,vp_native_draw,&win); app_present(&win);
    for(;;) {
        struct outrun_event ev; int r=app_poll(&win,&ev);
        if(r<0) app_exit(0);
        if(!r) { app_idle(); continue; }
        int action=VP_NONE;
        if(ev.type==EVENT_KEY_PRESS) action=vp_key(&vp_app,ev.code);
        if(ev.type==EVENT_MOUSE_DOWN) action=vp_click(&vp_app,ev.x,ev.y,win.cw,win.ch);
        if(action==VP_OPEN) vp_open(&vp_app,vp_system);
        if(action==VP_SAVE) vp_save(&vp_app,vp_system);
        vp_paint(&vp_app,win.cw,win.ch,vp_native_draw,&win); app_present(&win);
    }
}
#endif
