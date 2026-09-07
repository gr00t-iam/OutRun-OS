#include <assert.h>
#include <stdio.h>
#include <string.h>
#define APP_HOST_TEST
#include "outrun_edit.c"
static struct code_editor e;
static void set_text(const char *s) {
    struct vp_editor *b=&e.buffers[e.active];
    strcpy(b->text,s); b->len=(int)strlen(s); b->cursor=0;
}
static long long io(int op,unsigned long long a,unsigned long long b,unsigned long long c) {
    (void)op; (void)a; (void)b; (void)c; return -1;
}
static int glyphs;
static void draw(void *ctx,int x,int y,int w,int h,unsigned color,int ch) {
    (void)ctx; (void)x; (void)y; (void)w; (void)h; (void)color;
    if(ch) glyphs++;
}
int main(void) {
    code_init(&e); set_text("int main() { return 42; }\n// hello\n");
    code_highlight(&e,CODE_C);
    assert(e.style[0]==CODE_KEYWORD && e.style[20]==CODE_NUMBER);
    assert(e.style[strstr(e.buffers[0].text,"//")-e.buffers[0].text]==CODE_COMMENT);
    code_key(&e,27,io); code_key(&e,'2',io); assert(e.active==1);
    set_text("mov eax, 1 ; note\n"); code_highlight(&e,CODE_ASM);
    assert(e.style[0]==CODE_KEYWORD && e.style[11]==CODE_COMMENT);
    code_key(&e,27,io); code_key(&e,'3',io); assert(e.active==2);
    set_text("if true; then echo \"hi\" # note\n"); code_highlight(&e,CODE_SH);
    assert(e.style[0]==CODE_KEYWORD && e.style[19]==CODE_STRING && e.style[24]==CODE_COMMENT);
    assert(e.buffers[0].len>0 && e.buffers[1].len>0);
    int a,b;
    assert(code_find("cat cot cut",11,"c[ao]t",0,&a,&b)==1 && a==0 && b==3);
    assert(code_find("cat cot cut",11,"c[ao]t",3,&a,&b)==1 && a==4 && b==7);
    assert(code_find("abc\n1234\nz",10,"^[0-9]+$",0,&a,&b)==1 && a==4 && b==8);
    assert(code_find("xx aZZb!",8,"a.*b",0,&a,&b)==1 && a==3 && b==7);
    assert(code_find("a?",2,"a\\?",0,&a,&b)==1 && b==2);
    assert(code_find("x",1,"[",0,&a,&b)==-1);
    assert(code_find("x",1,"(x)",0,&a,&b)==-1);
    assert(code_find("x",1,"z",0,&a,&b)==0);
    set_text("cat cot cut");
    assert(code_replace(&e,"c[ao]t","dog")==2);
    assert(!strcmp(e.buffers[2].text,"dog dog cut") && e.buffers[2].dirty);
    set_text("ab"); assert(code_replace(&e,"x*","_")==3);
    assert(!strcmp(e.buffers[2].text,"_a_b_"));
    struct vp_editor *v=&e.buffers[2];
    memset(v->text,'a',VP_CAP); v->text[VP_CAP]=0; v->len=VP_CAP;
    assert(code_replace(&e,"a","aa")==-1 && v->len==VP_CAP && v->text[VP_CAP-1]=='a');
    set_text("int a=1;\n"); code_paint(&e,596,417,draw,0); assert(glyphs>20);
    code_key(&e,27,io); code_key(&e,'f',io); assert(e.field==1);
    code_key(&e,'a',io); assert(!strcmp(e.pattern,"a"));
    code_key(&e,13,io); assert(e.field==0 && v->cursor==4);
    code_key(&e,27,io); code_key(&e,'o',io); assert(!strcmp(v->status,"UNSAVED: SAVE BEFORE OPEN"));
    code_click(&e,20,10,596,417,io); assert(e.active==0);
    printf("outrun_edit: buffers, syntax, bounded regex, transactional replace, UI PASS; state=%zu bytes\n",sizeof e);
}
