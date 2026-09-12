/* Bounded VT100/ANSI text terminal. The current backend is SYS_RUN_CMD:
 * synchronous shell commands, at most 4095 captured bytes, no streaming PTY.
 * Parser state survives read boundaries. Unsupported CSI commands are ignored.
 * The PS/2 desktop delivers ASCII and clicks: ESC enters navigation mode and
 * two clicks delimit a selection (there is no mouse-drag event in ABI v1). */
#define TERM_COLS 72
#define TERM_ROWS 24
#define TERM_HISTORY 1000
#define TERM_LINES (TERM_HISTORY+TERM_ROWS)
#define TERM_TABS 4
struct term_cell { unsigned char ch,fg,bg; };
struct term_tab {
    struct term_cell cells[TERM_LINES][TERM_COLS];
    int head,history,row,col,view,fg,bg,saved_row,saved_col;
    int parser,param[8],nparam,bad;
    int anchor,end,selected;
    char cmd[96],commands[16][96],draft[96];
    int cmdlen,cmdpos,command_count,command_view;
};
struct term_session { struct term_tab tabs[TERM_TABS]; int active,command,menu; };
typedef long long (*term_run_fn)(const char *,char *,unsigned);
static struct term_cell *term_cell(struct term_tab *t,int row,int col) {
    return &t->cells[(t->head+row)%TERM_LINES][col];
}
static void term_clear_line(struct term_tab *t,int row,int from,int to) {
    for(int c=from;c<to;c++) *term_cell(t,row,c)=(struct term_cell){' ',(unsigned char)t->fg,(unsigned char)t->bg};
}
static void term_init(struct term_session *s) {
    /* No libc dependency: this same core is linked into the native ELF. */
    unsigned char *p=(unsigned char *)s;
    for(unsigned long i=0;i<sizeof *s;i++) p[i]=0;
    for(int k=0;k<TERM_TABS;k++) {
        s->tabs[k].fg=7;
        for(int r=0;r<TERM_ROWS;r++) term_clear_line(&s->tabs[k],r,0,TERM_COLS);
    }
}
static void term_down(struct term_tab *t) {
    if(t->row<TERM_ROWS-1) { t->row++; return; }
    t->head=(t->head+1)%TERM_LINES;
    if(t->history<TERM_HISTORY) t->history++;
    if(t->view && t->view<t->history) t->view++;
    t->selected=0;
    term_clear_line(t,TERM_ROWS-1,0,TERM_COLS);
}
static int term_limit(int n,int hi) { return n<0?0:n>hi?hi:n; }
static void term_csi(struct term_tab *t,int c) {
    int n=t->param[0]?t->param[0]:1;
    switch(c) {
    case 'A': t->row=term_limit(t->row-n,TERM_ROWS-1); break;
    case 'B': t->row=term_limit(t->row+n,TERM_ROWS-1); break;
    case 'C': t->col=term_limit(t->col+n,TERM_COLS-1); break;
    case 'D': t->col=term_limit(t->col-n,TERM_COLS-1); break;
    case 'G': t->col=term_limit(n-1,TERM_COLS-1); break;
    case 'H': case 'f':
        t->row=term_limit(n-1,TERM_ROWS-1);
        t->col=term_limit((t->param[1]?t->param[1]:1)-1,TERM_COLS-1); break;
    case 'J':
        if(t->param[0]==2) for(int r=0;r<TERM_ROWS;r++) term_clear_line(t,r,0,TERM_COLS);
        else if(t->param[0]==0) {
            term_clear_line(t,t->row,t->col,TERM_COLS);
            for(int r=t->row+1;r<TERM_ROWS;r++) term_clear_line(t,r,0,TERM_COLS);
        } else if(t->param[0]==1) {
            for(int r=0;r<t->row;r++) term_clear_line(t,r,0,TERM_COLS);
            term_clear_line(t,t->row,0,term_limit(t->col+1,TERM_COLS));
        }
        break;
    case 'K':
        if(t->param[0]<=2) term_clear_line(t,t->row,t->param[0]==0?t->col:0,
            t->param[0]==1?term_limit(t->col+1,TERM_COLS):TERM_COLS);
        break;
    case 's': t->saved_row=t->row; t->saved_col=t->col; break;
    case 'u': t->row=t->saved_row; t->col=t->saved_col; break;
    case 'm':
        for(int i=0;i<=t->nparam;i++) {
            int a=t->param[i];
            if(a==0) { t->fg=7; t->bg=0; }
            else if(a>=30 && a<=37) t->fg=a-30;
            else if(a>=40 && a<=47) t->bg=a-40;
            else if(a==39) t->fg=7;
            else if(a==49) t->bg=0;
        }
        break;
    default: break;
    }
}
static void term_feed(struct term_tab *t,const char *bytes,unsigned count) {
    for(unsigned i=0;i<count;i++) {
        unsigned char c=(unsigned char)bytes[i];
        if(c==27) { t->parser=1; continue; }
        if(t->parser==1) {
            t->parser=0;
            if(c=='[') {
                t->parser=2; t->nparam=0; t->bad=0;
                for(int j=0;j<8;j++) t->param[j]=0;
            } else if(c==']') t->parser=3;
            else if(c=='7') { t->saved_row=t->row; t->saved_col=t->col; }
            else if(c=='8') { t->row=t->saved_row; t->col=t->saved_col; }
            continue;
        }
        if(t->parser==3) { if(c==7) t->parser=0; continue; }
        if(t->parser==2) {
            if(c>='0' && c<='9') {
                int *p=&t->param[t->nparam]; *p=term_limit(*p*10+c-'0',9999);
            } else if(c==';') {
                if(t->nparam<7) t->nparam++; else t->bad=1;
            } else if(c>=0x40 && c<=0x7e) {
                if(!t->bad) term_csi(t,c);
                t->parser=0;
            } else t->bad=1;
            continue;
        }
        if(c=='\r') { t->col=0; continue; }
        if(c=='\n') { term_down(t); continue; }
        if(c==8) { if(t->col) t->col--; continue; }
        if(c==9) { t->col=term_limit((t->col/8+1)*8,TERM_COLS-1); continue; }
        if(c<32 || c==127) continue;
        if(t->col==TERM_COLS) { t->col=0; term_down(t); }
        *term_cell(t,t->row,t->col++)=(struct term_cell){c,(unsigned char)t->fg,(unsigned char)t->bg};
    }
}
static void term_scroll(struct term_tab *t,int lines) {
    /* UI uses bounded steps; comparison first also handles arbitrary test input. */
    if(lines>t->history-t->view) t->view=t->history;
    else if(lines < -t->view) t->view=0;
    else t->view+=lines;
    t->selected=0;
}
static void term_select(struct term_tab *t,int col,int row) {
    if(col<0 || col>=TERM_COLS || row<0 || row>=TERM_ROWS) return;
    int p=row*TERM_COLS+col;
    if(t->selected!=1) { t->anchor=p; t->end=p; t->selected=1; }
    else { t->end=p; t->selected=2; }
}
static unsigned term_copy(struct term_tab *t,char *out,unsigned cap) {
    unsigned n=0;
    if(!cap) return 0;
    int a=t->anchor,b=t->end;
    if(a>b) { int swap=a; a=b; b=swap; }
    if(t->selected==2) for(int p=a;p<b && n+1<cap;p++) {
        int r=(t->head+TERM_LINES-t->view+p/TERM_COLS)%TERM_LINES;
        out[n++]=(char)t->cells[r][p%TERM_COLS].ch;
        if(p%TERM_COLS==TERM_COLS-1 && n+1<cap) out[n++]='\n';
    }
    out[n]=0; return n;
}
static int term_tab_switch(struct term_session *s,int index) {
    if(index<0 || index>=TERM_TABS) return 0;
    s->active=index; return 1;
}
static void term_recall(struct term_tab *t,int older) {
    if(!t->command_view) for(int i=0;i<=t->cmdlen;i++) t->draft[i]=t->cmd[i];
    if(older && t->command_view<t->command_count) t->command_view++;
    if(!older && t->command_view) t->command_view--;
    const char *s=t->command_view?t->commands[t->command_count-t->command_view]:t->draft;
    int i=0; do { t->cmd[i]=s[i]; } while(s[i++]);
    t->cmdpos=t->cmdlen=i-1;
}
static void term_key(struct term_session *s,int c,term_run_fn run) {
    struct term_tab *t=&s->tabs[s->active];
    if(c==27) { s->command=!s->command; return; }
    if(c==1) { t->cmdpos=0; return; }
    if(c==5) { t->cmdpos=t->cmdlen; return; }
    if(c==16 || c==14) { term_recall(t,c==16); return; }
    if(c==21) { t->cmdlen=t->cmdpos=0; t->cmd[0]=0; return; }
    if(s->command) {
        if(c>='1' && c<='4') term_tab_switch(s,c-'1');
        if(c=='h' && t->cmdpos) t->cmdpos--;
        if(c=='l' && t->cmdpos<t->cmdlen) t->cmdpos++;
        if(c=='0') t->cmdpos=0;
        if(c=='$') t->cmdpos=t->cmdlen;
        if(c=='k' || c=='j') term_recall(t,c=='k');
        if(c=='u') term_scroll(t,TERM_ROWS);
        if(c=='d') term_scroll(t,-TERM_ROWS);
        s->command=0; return;
    }
    if(c==8 || c==127) {
        if(c==8) { if(!t->cmdpos) return; t->cmdpos--; }
        else if(t->cmdpos==t->cmdlen) return;
        for(int i=t->cmdpos;i<t->cmdlen;i++) t->cmd[i]=t->cmd[i+1];
        t->cmdlen--; return;
    }
    if(c==13 || c==10) {
        char out[4096];
        if(t->cmdlen) {
            if(t->command_count==16) {
                for(int j=1;j<16;j++) for(int i=0;i<96;i++) t->commands[j-1][i]=t->commands[j][i];
                t->command_count--;
            }
            for(int i=0;i<=t->cmdlen;i++) t->commands[t->command_count][i]=t->cmd[i];
            t->command_count++;
        }
        t->command_view=0; t->draft[0]=0;
        t->view=0; t->selected=0;
        term_feed(t,t->cmd,(unsigned)t->cmdlen); term_feed(t,"\r\n",2);
        long long n=run(t->cmd,out,sizeof out);
        if(n<0 || n>(long long)sizeof out) term_feed(t,"command failed\r\n",16);
        else {
            /* The kernel console uses bare LF. Translate at this backend
             * boundary; the VT parser itself retains LF's column semantics. */
            for(unsigned i=0;i<(unsigned)n;i++) {
                if(out[i]=='\n') term_feed(t,"\r",1);
                term_feed(t,out+i,1);
            }
            if(n==4095) {
                static const char limit[]="\r\n[output capture limit reached]\r\n";
                term_feed(t,limit,sizeof limit-1);
            }
        }
        t->cmdlen=t->cmdpos=0; t->cmd[0]=0; return;
    }
    if(c>=32 && c<=126 && t->cmdlen<95) {
        for(int i=t->cmdlen;i>=t->cmdpos;i--) t->cmd[i+1]=t->cmd[i];
        t->cmd[t->cmdpos++]=(char)c; t->cmdlen++;
    }
}
static void term_menu_action(struct term_session *s,int menu,int item,
                             char *copy,unsigned cap,term_run_fn run) {
    struct term_tab *t=&s->tabs[s->active];
    s->command=0; s->menu=0;
    if(menu==1) {
        if(item==1) term_tab_switch(s,(s->active+TERM_TABS-1)%TERM_TABS);
        if(item==2) term_tab_switch(s,(s->active+1)%TERM_TABS);
        if(item==3) term_key(s,13,run);
    } else if(menu==2) {
        if(item==1) term_copy(t,copy,cap);
        if(item==2) for(unsigned i=0;i<cap && copy[i];i++)
            if(copy[i]>=32 && copy[i]<=126) term_key(s,copy[i],run);
        if(item==3) term_key(s,21,run);
    } else if(menu==3) {
        if(item==1) term_scroll(t,TERM_ROWS);
        if(item==2) term_scroll(t,-TERM_ROWS);
        if(item==3) {
            t->head=t->history=t->row=t->col=t->view=t->selected=0;
            t->parser=t->nparam=t->bad=t->saved_row=t->saved_col=0;
            t->fg=7; t->bg=0;
            for(int r=0;r<TERM_ROWS;r++) term_clear_line(t,r,0,TERM_COLS);
        }
    }
}
#ifndef APP_HOST_TEST
#include "gui.h"
#include "build_task.h"
static struct term_session session;
static char clipboard[TERM_ROWS*(TERM_COLS+1)+1];
static long long term_native_run(const char *cmd,char *out,unsigned cap) {
    return (i64)sysc(SYS_RUN_CMD,(u64)cmd,(u64)out,cap);
}
static void term_render(struct app_win *w) {
    static const u32 colors[8]={0x0a0d14,0xff6b81,0x3df5c4,0xffcc66,0x77aaff,0xc4a6ff,0x22e4ff,0xeaf2f7};
    struct term_tab *t=&session.tabs[session.active];
    app_fill(w,w->bg);
    static const char *menus[]={"File","Edit","View"};
    for(int m=0;m<3;m++) app_button(w,8+m*60,2,56,20,menus[m],session.menu==m+1);
    for(int k=0;k<TERM_TABS;k++) {
        app_rect(w,8+k*96,26,88,22,k==session.active?0x28495d:0x1c2636);
        app_text(w,16+k*96,34,"Tab",w->fg); app_u32(w,48+k*96,34,(u32)k+1,w->fg);
    }
    int a=t->anchor,b=t->end; if(a>b) { int tmp=a; a=b; b=tmp; }
    for(int row=0;row<TERM_ROWS;row++) for(int col=0;col<TERM_COLS;col++) {
        int r=(t->head+TERM_LINES-t->view+row)%TERM_LINES;
        struct term_cell cell=t->cells[r][col]; int p=row*TERM_COLS+col;
        app_rect(w,8+col*8,52+row*12,8,12,t->selected==2 && p>=a && p<b?0x435a76:colors[cell.bg]);
        app_char(w,8+col*8,52+row*12,(char)cell.ch,colors[cell.fg]);
    }
    app_str(w,8,348,">",0x3df5c4);
    int left=t->cmdpos>68?t->cmdpos-68:0;
    app_str(w,24,348,t->cmd+left,w->fg);
    app_rect(w,24+(t->cmdpos-left)*8,358,7,2,0x3df5c4);
    app_text(w,8,366,session.command?"NAV: h/l 0/$ cursor | k/j commands | u/d scroll":"Esc then: k/j recall | h/l cursor | 1-4 tabs",0xffcc66);
    app_button(w,8,380,56,20,"Copy",0); app_button(w,72,380,56,20,"Paste",0);
    app_text(w,144,386,"Scrollback",w->fg); app_u32(w,216,386,(u32)t->view,w->fg);
    app_text(w,8,406,"Shell capture: 4095 bytes/command; no streaming PTY",0x7c8ca0);
    if(session.menu) {
        static const char *items[3][3]={{"Previous tab","Next tab","Run command"},
            {"Copy selection","Paste text","Clear command"},{"Page up","Page down","Clear output"}};
        int mx=8+(session.menu-1)*60;
        app_rect(w,mx,22,164,78,0x344255);
        for(int i=0;i<3;i++) app_button(w,mx+2,24+i*24,160,24,items[session.menu-1][i],0);
    }
    app_present(w);
}
void _start(void) {
    struct app_win w;
    term_init(&session);
    if(app_create(&w,600,440,0x3df5c4)<0) app_exit(1);
    app_title(&w,"OUTRUN TERM"); term_render(&w);
    for(;;) {
        struct outrun_event e; int rc=app_poll(&w,&e);
        if(rc<0) app_exit(0);
        struct outrun_ipc_msg msg;
        if((i64)sysc(SYS_IPC_RECV,(u64)&msg,0,0)==1) {
            char command[96];
            if(build_command(&msg,command)==0) {
                /* Keep the user's pending command while the task prints output. */
                struct term_tab *t=&session.tabs[session.active];
                char pending[96]; int n=t->cmdlen,pos=t->cmdpos;
                for(int i=0;i<=n;i++) pending[i]=t->cmd[i];
                t->cmdlen=0;
                for(int i=0;command[i];i++) t->cmd[t->cmdlen++]=command[i];
                t->cmd[t->cmdlen]=0; t->cmdpos=t->cmdlen;
                int mode=session.command; session.command=0;
                term_key(&session,13,term_native_run); session.command=mode;
                for(int i=0;i<=n;i++) t->cmd[i]=pending[i];
                t->cmdlen=n; t->cmdpos=pos;
                term_render(&w);
            }
        }
        if(!rc) { app_idle(); continue; }
        if(e.type==EVENT_KEY_PRESS) { session.menu=0; term_key(&session,e.code,term_native_run); }
        if(e.type==EVENT_MOUSE_DOWN) {
            struct term_tab *t=&session.tabs[session.active];
            if(app_hit(e.x,e.y,8,2,180,20)) {
                int m=(e.x-8)/60+1; session.menu=session.menu==m?0:m;
            } else if(session.menu) {
                int mx=8+(session.menu-1)*60;
                if(app_hit(e.x,e.y,mx,24,164,72))
                    term_menu_action(&session,session.menu,(e.y-24)/24+1,clipboard,sizeof clipboard,term_native_run);
                session.menu=0;
            } else if(e.y>=26 && e.y<48 && e.x>=8) term_tab_switch(&session,(e.x-8)/96);
            else if(e.y>=52 && e.y<340 && e.x>=8) term_select(t,(e.x-8)/8,(e.y-52)/12);
            else if(e.y>=380 && e.y<400) {
                if(e.x>=8 && e.x<64) term_copy(t,clipboard,sizeof clipboard);
                if(e.x>=72 && e.x<136) for(unsigned i=0;clipboard[i];i++)
                    if(clipboard[i]>=32 && clipboard[i]<=126) term_key(&session,clipboard[i],term_native_run);
            }
        }
        term_render(&w);
    }
}
#endif
