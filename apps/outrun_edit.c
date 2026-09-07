#define VAULT_PAD_LIBRARY
#include "vault_pad.c"
#define CODE_BUFFERS 4
#define CODE_QUERY 64
#define CODE_RX_MAX 48
enum { CODE_C, CODE_ASM, CODE_SH };
enum { CODE_TEXT, CODE_KEYWORD, CODE_NUMBER, CODE_STRING, CODE_COMMENT };
struct code_editor {
    struct vp_editor buffers[CODE_BUFFERS];
    unsigned char style[VP_CAP+1];
    char pattern[CODE_QUERY],replacement[CODE_QUERY];
    int active,field,language,build,rows[32],paint_h;
    vp_draw_fn draw; void *ctx;
};
struct code_atom { unsigned char bits[32],quant; };
struct code_regex { struct code_atom atoms[CODE_RX_MAX]; int n,bol,eol; };
static void code_bit(struct code_atom *a,unsigned c) { a->bits[c/8]|=(unsigned char)(1u<<(c%8)); }
static int code_compile(struct code_regex *r,const char *p) {
    unsigned char *z=(unsigned char *)r;
    for(unsigned long i=0;i<sizeof *r;i++) z[i]=0;
    int at=0;
    if(p[at]=='^') { r->bol=1; at++; }
    while(p[at]) {
        if(p[at]=='$' && !p[at+1]) { r->eol=1; break; }
        if(r->n==CODE_RX_MAX) return -1;
        struct code_atom *a=&r->atoms[r->n++];
        unsigned char c=(unsigned char)p[at++];
        if(c=='[') {
            int negate=p[at]=='^',have=0; if(negate) at++;
            while(p[at] && p[at]!=']') {
                unsigned first=(unsigned char)p[at++];
                if(first=='\\') { if(!p[at]) return -1; first=(unsigned char)p[at++]; }
                unsigned last=first;
                if(p[at]=='-' && p[at+1] && p[at+1]!=']') {
                    at++; last=(unsigned char)p[at++];
                    if(last=='\\') { if(!p[at]) return -1; last=(unsigned char)p[at++]; }
                    if(last<first) return -1;
                }
                for(unsigned k=first;k<=last;k++) code_bit(a,k);
                have=1;
            }
            if(!have || p[at]!=']') return -1;
            at++;
            if(negate) for(int j=0;j<32;j++) a->bits[j]^=255;
        } else if(c=='.') {
            for(int j=0;j<32;j++) a->bits[j]=255;
            a->bits['\n'/8]&=(unsigned char)~(1u<<('\n'%8));
        } else {
            if(c=='\\') { if(!p[at]) return -1; c=(unsigned char)p[at++]; }
            else if(c=='(' || c==')' || c=='|' || c=='{' || c=='}' || c=='*' || c=='+' || c=='?' || c=='^' || c=='$') return -1;
            code_bit(a,c);
        }
        if(p[at]=='?') { a->quant=1; at++; }
        else if(p[at]=='*') { a->quant=2; at++; }
        else if(p[at]=='+') { a->quant=3; at++; }
    }
    return 0;
}
static void code_earliest(int *at,int value) { if(*at<0 || value<*at) *at=value; }
/* Tagged NFA: bounded states, no recursion or exponential backtracking.
 * Regex subset: literals, escapes, dot, classes/ranges, ?, *, + and line ^/$.
 * Grouping, alternation and counted repetition are refused, not misread. */
static int code_match(const struct code_regex *r,const char *s,int len,int from,int *start,int *end) {
    int active[CODE_RX_MAX+1],next[CODE_RX_MAX+1],best=-1,last=-1;
    for(int i=0;i<=r->n;i++) active[i]=-1;
    for(int pos=from;pos<=len;pos++) {
        if(!r->bol || !pos || s[pos-1]=='\n') code_earliest(&active[0],pos);
        for(int i=0;i<r->n;i++) if(active[i]>=0 && (r->atoms[i].quant==1 || r->atoms[i].quant==2))
            code_earliest(&active[i+1],active[i]);
        if(active[r->n]>=0 && (!r->eol || pos==len || s[pos]=='\n')) {
            if(best<0 || active[r->n]<=best) { best=active[r->n]; last=pos; }
        }
        int continuing=0;
        if(best>=0) for(int i=0;i<r->n;i++) if(active[i]>=0 && active[i]<=best) continuing=1;
        if(best>=0 && !continuing) break;
        if(pos==len) break;
        for(int i=0;i<=r->n;i++) next[i]=-1;
        unsigned c=(unsigned char)s[pos];
        for(int i=0;i<r->n;i++) if(active[i]>=0 && (r->atoms[i].bits[c/8] & (1u<<(c%8)))) {
            code_earliest(&next[i+1],active[i]);
            if(r->atoms[i].quant>=2) code_earliest(&next[i],active[i]);
        }
        for(int i=0;i<=r->n;i++) active[i]=next[i];
    }
    if(best<0) return 0;
    *start=best; *end=last; return 1;
}
static int code_find(const char *s,int len,const char *pattern,int from,int *a,int *b) {
    struct code_regex r;
    if(len<0 || len>VP_CAP || from<0 || from>len || code_compile(&r,pattern)<0) return -1;
    return code_match(&r,s,len,from,a,b);
}
static void code_init(struct code_editor *e) {
    e->active=e->field=e->language=e->build=0; e->pattern[0]=e->replacement[0]=0;
    for(int i=0;i<CODE_BUFFERS;i++) {
        vp_init(&e->buffers[i]);
        e->buffers[i].filename[0]=(char)('1'+i);
    }
}
static int code_replace(struct code_editor *e,const char *pattern,const char *replacement) {
    struct vp_editor *v=&e->buffers[e->active]; struct code_regex r;
    if(code_compile(&r,pattern)<0) { v->status="INVALID / UNSUPPORTED REGEX"; return -1; }
    int rn=0; while(replacement[rn]) rn++;
    int from=0,n=0,count=0,a,b;
    while(from<=v->len && code_match(&r,v->text,v->len,from,&a,&b)>0) {
        if(a-from>VP_CAP-n || rn>VP_CAP-n-(a-from)) { v->status="REPLACE WOULD EXCEED CAPACITY"; return -1; }
        while(from<a) vp_staging[n++]=v->text[from++];
        for(int i=0;i<rn;i++) vp_staging[n++]=replacement[i];
        count++; from=b;
        if(a==b) {
            if(from==v->len) break;
            if(n==VP_CAP) { v->status="REPLACE WOULD EXCEED CAPACITY"; return -1; }
            vp_staging[n++]=v->text[from++];
        }
    }
    if(v->len-from>VP_CAP-n) { v->status="REPLACE WOULD EXCEED CAPACITY"; return -1; }
    while(from<v->len) vp_staging[n++]=v->text[from++];
    if(count) {
        for(int i=0;i<n;i++) v->text[i]=vp_staging[i];
        v->text[n]=0; v->len=n; v->cursor=0; v->dirty=1;
    }
    v->status=count?"REPLACED (LITERAL REPLACEMENT)":"NO MATCH"; return count;
}
static int code_word(int c) { return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_'; }
static int code_keyword(const char *s,int len,const char *words) {
    while(*words) {
        int n=0; while(words[n] && words[n]!=' ') n++;
        int same=n==len; for(int i=0;same && i<n;i++) if(s[i]!=words[i]) same=0;
        if(same) return 1;
        words+=n; if(*words) words++;
    }
    return 0;
}
static void code_highlight(struct code_editor *e,int language) {
    struct vp_editor *v=&e->buffers[e->active]; const char *s=v->text;
    const char *words=language==CODE_C?"auto break case char const continue default do double else enum extern float for goto if inline int long register restrict return short signed sizeof static struct switch typedef union unsigned void volatile while":
        language==CODE_ASM?"mov lea add sub mul div xor and or not push pop call ret cmp jmp je jne section global db dw dd dq syscall":
        "if then else elif fi for while do done case esac function in echo export local return exit";
    for(int i=0;i<v->len;) {
        int first=i++,style=CODE_TEXT;
        if((language==CODE_C && s[first]=='/' && i<v->len && s[i]=='/') ||
           (language==CODE_ASM && s[first]==';') || (language==CODE_SH && s[first]=='#')) {
            style=CODE_COMMENT; while(i<v->len && s[i]!='\n') i++;
        } else if(language==CODE_C && s[first]=='/' && i<v->len && s[i]=='*') {
            style=CODE_COMMENT; i++;
            while(i<v->len && !(s[i-1]=='*' && s[i]=='/')) i++;
            if(i<v->len) i++;
        } else if(s[first]=='"' || s[first]=='\'') {
            style=CODE_STRING;
            while(i<v->len && s[i]!='\n') {
                if(s[i++]==s[first]) break;
                if(s[i-1]=='\\' && i<v->len) i++;
            }
        } else if(code_word((unsigned char)s[first])) {
            while(i<v->len && code_word((unsigned char)s[i])) i++;
            if(s[first]>='0' && s[first]<='9') style=CODE_NUMBER;
            else if(code_keyword(s+first,i-first,words)) style=CODE_KEYWORD;
        }
        for(int k=first;k<i;k++) e->style[k]=(unsigned char)style;
    }
    e->style[v->len]=CODE_TEXT;
}
static void code_search(struct code_editor *e) {
    struct vp_editor *v=&e->buffers[e->active]; int a,b;
    int rc=code_find(v->text,v->len,e->pattern,v->cursor,&a,&b);
    if(rc==1) { v->cursor=a; v->status="MATCH AT CURSOR"; }
    else v->status=rc<0?"INVALID / UNSUPPORTED REGEX":"NO MATCH AFTER CURSOR";
}
static void code_key(struct code_editor *e,int c,vp_io_fn io) {
    struct vp_editor *v=&e->buffers[e->active];
    if(e->field) {
        if(c==27) { e->field=0; return; }
        if(c==10 || c==13) { if(e->field==1) code_search(e); e->field=0; return; }
        char *s=e->field==1?e->pattern:e->replacement;
        int n=0; while(s[n]) n++;
        if(c==8 && n) s[--n]=0;
        else if(c>=32 && c<=126 && n<CODE_QUERY-1) { s[n++]=(char)c; s[n]=0; }
        return;
    }
    if(v->command) {
        if(c>='1' && c<='4') { v->command=0; e->active=c-'1'; return; }
        if(c=='f' || c=='r') { e->field=c=='f'?1:2; v->command=0; return; }
        if(c=='n') { if(v->cursor<v->len) v->cursor++; code_search(e); return; }
        if(c=='a') { code_replace(e,e->pattern,e->replacement); return; }
        if(c=='b') { e->build=1; return; }
    }
    int action=vp_key(v,c);
    if(action==VP_OPEN) vp_open(v,io);
    if(action==VP_SAVE) vp_save(v,io);
}
static void code_click(struct code_editor *e,int x,int y,int w,int h,vp_io_fn io) {
    if(x<0 || x>=w || y<0 || y>=h) return;
    if(y<26) {
        if(x<384) { e->active=x/96; e->field=0; }
        else if(x<464) e->language=(e->language+1)%3;
        else e->build=1;
        return;
    }
    if(y>=h-56) { e->field=y<h-28?1:2; return; }
    if(x<40) return;
    struct vp_editor *v=&e->buffers[e->active];
    int action=vp_click(v,x-40,y-28,w-40,h-84);
    if(action==VP_OPEN) vp_open(v,io);
    if(action==VP_SAVE) vp_save(v,io);
}
static void code_draw(void *ctx,int x,int y,int w,int h,unsigned color,int ch) {
    struct code_editor *e=(struct code_editor *)ctx;
    struct vp_editor *v=&e->buffers[e->active];
    static const unsigned colors[]={0xe6f2ff,0xc4a6ff,0xffcc66,0x3df5c4,0x8293a8};
    if(ch && y>=56 && y<e->paint_h-48 && x>=8) {
        int row=(y-56)/16,col=(x-8)/8+v->left;
        if(row<32 && e->rows[row]>=0) {
            int pos=e->rows[row]+col;
            if(pos<v->len) color=colors[e->style[pos]];
        }
    }
    e->draw(e->ctx,x+40,y+28,w,h,color,ch);
}
static void code_paint(struct code_editor *e,int w,int h,vp_draw_fn draw,void *ctx) {
    if(w<400 || h<260) return;
    struct vp_editor *v=&e->buffers[e->active];
    e->paint_h=h-84; e->draw=draw; e->ctx=ctx;
    vp_visible(v,(e->paint_h-104)/16,(w-56)/8);
    code_highlight(e,e->language);
    for(int i=0;i<32;i++) e->rows[i]=-1;
    int row=0,start=0;
    for(int i=0;i<=v->len;i++) if(i==v->len || v->text[i]=='\n') {
        if(row>=v->top && row-v->top<32) e->rows[row-v->top]=start;
        row++; start=i+1;
    }
    draw(ctx,0,0,w,h,0x101522,0);
    vp_paint(v,w-40,e->paint_h,code_draw,e);
    for(int i=0;i<CODE_BUFFERS;i++) {
        draw(ctx,i*96,0,92,24,e->active==i?0x28495d:0x1c2636,0);
        vp_number(draw,ctx,i*96+8,8,i+1);
        vp_label(draw,ctx,i*96+24,8,e->buffers[i].dirty?"MODIFIED":"BUFFER",0xe6f2ff,8);
    }
    vp_label(draw,ctx,392,8,e->language==CODE_C?"C":e->language==CODE_ASM?"ASM":"SHELL",0x3df5c4,7);
    vp_label(draw,ctx,472,8,"BUILD",0x22e4ff,6);
    for(int i=0;i<(e->paint_h-104)/16 && i<32;i++) if(e->rows[i]>=0)
        vp_number(draw,ctx,0,84+i*16,v->top+i+1);
    vp_label(draw,ctx,8,h-52,e->field==1?"> FIND":"  FIND",0xffcc66,7);
    vp_label(draw,ctx,72,h-52,e->pattern,0xe6f2ff,(w-80)/8);
    vp_label(draw,ctx,8,h-28,e->field==2?"> WITH":"  WITH",0xffcc66,7);
    vp_label(draw,ctx,72,h-28,e->replacement,0xe6f2ff,(w-80)/8);
    vp_label(draw,ctx,8,h-12,"ESC: 1-4 buffers | f/r fields | n next | a replace all | b build",0x8293a8,(w-16)/8);
}
#ifndef APP_HOST_TEST
#include "gui.h"
#include "build_task.h"
static struct code_editor editor;
static long long code_io(int op,u64 a,u64 b,u64 c) { return (i64)sysc((u64)op,a,b,c); }
static void code_native_draw(void *ctx,int x,int y,int w,int h,unsigned color,int ch) {
    if(ch) app_char(ctx,x,y,(char)ch,color); else app_rect(ctx,x,y,w,h,color);
}
void _start(void) {
    struct app_win w;
    code_init(&editor);
    if(app_create(&w,600,440,0xc4a6ff)<0) app_exit(1);
    app_title(&w,"OUTRUN CODE");
    for(;;) {
        code_paint(&editor,w.cw,w.ch,code_native_draw,&w); app_present(&w);
        struct outrun_event ev; int rc;
        while((rc=app_poll(&w,&ev))==0) app_idle();
        if(rc<0) app_exit(0);
        if(ev.type==EVENT_KEY_PRESS) code_key(&editor,ev.code,code_io);
        if(ev.type==EVENT_MOUSE_DOWN) code_click(&editor,ev.x,ev.y,w.cw,w.ch,code_io);
        if(editor.build) {
            editor.build=0;
            struct vp_editor *v=&editor.buffers[editor.active];
            if(v->dirty) { v->status="SAVE BEFORE BUILD"; continue; }
            struct outrun_desktop_info info; struct outrun_ipc_msg msg;
            u64 pid=0;
            if((i64)sysc(SYS_DESKTOP_INFO,(u64)&info,sizeof info,0)>=0 && info.version==OUTRUN_DESKTOP_ABI_VERSION && info.nproc<=12)
                for(unsigned i=0;i<info.nproc;i++) if(vp_same_name(info.proc[i].name,"OUTRUN TERM") && !info.proc[i].flags) pid=info.proc[i].pid;
            char command[96];
            if(!pid) v->status="OPEN OUTRUN TERM TO RECEIVE BUILD TASKS";
            else if(build_request(&msg,pid,v->filename)<0 || build_command(&msg,command)<0) v->status="BUILD PATH: USE LETTERS, DIGITS, / . _ -";
            else v->status=(i64)sysc(SYS_IPC_SEND,(u64)&msg,0,0)<0?"BUILD IPC SEND FAILED":"BUILD QUEUED: SEE TERMINAL; /tmp/outrun-build.elf";
        }
    }
}
#endif
