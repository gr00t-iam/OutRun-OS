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
/* Indent guides are the only 1px-wide filled rect the editor emits, which is
 * what makes them countable from here without a framebuffer. */
static int guides;
static void probe(void *ctx,int x,int y,int w,int h,unsigned color,int ch) {
    (void)ctx; (void)x; (void)y; (void)color;
    if(!ch && w==1 && h==16) guides++;
}
int main(void) {
    code_init(&e);
    struct vp_editor *undo=&e.buffers[0];
    vp_key(undo,'a'); vp_key(undo,'b'); vp_key(undo,26);
    assert(!strcmp(undo->text,"a"));
    vp_key(undo,25); assert(!strcmp(undo->text,"ab"));
    vp_click(undo,20,40,600,400);
    assert(undo->menu==1);
    assert(vp_click(undo,20,90,600,400)==VP_OPEN);
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

    /* ---- Slice 1: view features -------------------------------------- */
    /* Bracket matching. The style array must be current: brackets inside
     * strings and comments are NOT code and must be skipped, which is the
     * whole reason this consults e.style rather than scanning raw bytes. */
    code_init(&e); set_text("int f(void) { if (a[1]) { return 0; } }\n");
    code_highlight(&e,CODE_C);
    {
        const char *t=e.buffers[0].text;
        int open=(int)(strchr(t,'{')-t);
        int close=(int)(strrchr(t,'}')-t);
        assert(code_bracket_match(&e,t,e.buffers[0].len,open)==close);
        assert(code_bracket_match(&e,t,e.buffers[0].len,close)==open);
        /* Matching is also offered when the cursor sits just AFTER a bracket. */
        assert(code_bracket_match(&e,t,e.buffers[0].len,open+1)==close);
        int lb=(int)(strchr(t,'[')-t);
        assert(code_bracket_match(&e,t,e.buffers[0].len,lb)==lb+2);
        /* Not on a bracket at all. */
        assert(code_bracket_match(&e,t,e.buffers[0].len,0)==-1);
    }
    /* A brace inside a string is not a bracket. Without the style check the
     * scan below would pair the real '{' with the quoted '}' and report a
     * match three characters too early. */
    code_init(&e); set_text("{ s = \"}\"; }\n");
    code_highlight(&e,CODE_C);
    {
        const char *t=e.buffers[0].text;
        int last=(int)(strrchr(t,'}')-t);
        assert(code_bracket_match(&e,t,e.buffers[0].len,0)==last);
    }
    /* Unbalanced input reports no match rather than running off the buffer. */
    code_init(&e); set_text("{ { }\n"); code_highlight(&e,CODE_C);
    assert(code_bracket_match(&e,e.buffers[0].text,e.buffers[0].len,0)==-1);

    /* Auto-indent, by language FAMILY. Slice 2 maps the new languages onto
     * these same families; the rules are tested here directly so the mapping
     * is the only thing left to verify later. */
    {
        const char *c="void f() {\n";
        assert(code_auto_indent(CODE_FAM_CLIKE,c,(int)strlen(c),(int)strlen(c),4)==4);
        const char *plain="    int x = 1;\n";
        assert(code_auto_indent(CODE_FAM_CLIKE,plain,(int)strlen(plain),(int)strlen(plain),4)==4);
        const char *nested="    if (a) {\n";
        assert(code_auto_indent(CODE_FAM_CLIKE,nested,(int)strlen(nested),(int)strlen(nested),4)==8);
        /* A tab counts as a full tab stop, not one column. */
        const char *tabbed="\tint x;\n";
        assert(code_auto_indent(CODE_FAM_CLIKE,tabbed,(int)strlen(tabbed),(int)strlen(tabbed),4)==4);
        const char *py="def f():\n";
        assert(code_auto_indent(CODE_FAM_PY,py,(int)strlen(py),(int)strlen(py),4)==4);
        const char *pyplain="x = 1\n";
        assert(code_auto_indent(CODE_FAM_PY,pyplain,(int)strlen(pyplain),(int)strlen(pyplain),4)==0);
        /* ':' only opens a block in Python; C-like ignores it. */
        assert(code_auto_indent(CODE_FAM_CLIKE,py,(int)strlen(py),(int)strlen(py),4)==0);
        const char *html="<div>\n";
        assert(code_auto_indent(CODE_FAM_MARKUP,html,(int)strlen(html),(int)strlen(html),2)==2);
        /* A CLOSING tag also ends in '>', and must not indent. This is the
         * case a naive last-character rule gets wrong. */
        const char *close="  </div>\n";
        assert(code_auto_indent(CODE_FAM_MARKUP,close,(int)strlen(close),(int)strlen(close),2)==2);
        const char *self="  <br/>\n";
        assert(code_auto_indent(CODE_FAM_MARKUP,self,(int)strlen(self),(int)strlen(self),2)==2);
        assert(code_auto_indent(CODE_FAM_PLAIN,c,(int)strlen(c),(int)strlen(c),4)==0);
    }
    /* Enter applies the indent through the REAL key path, and the newline plus
     * its indent are ONE undo step -- not two. */
    code_init(&e); e.language=CODE_C;
    {
        struct vp_editor *b=&e.buffers[0];
        const char *seed="void f() {";
        strcpy(b->text,seed); b->len=(int)strlen(seed); b->cursor=b->len;
        vp_mark_clean(b);
        code_key(&e,13,io);
        assert(!strcmp(b->text,"void f() {\n    "));
        assert(b->cursor==b->len && b->dirty);
        vp_key(b,26);                       /* undo */
        assert(!strcmp(b->text,seed) && !b->dirty);
    }

    /* Bookmarks are per buffer, bounded, and toggle. */
    code_init(&e); set_text("a\nb\nc\nd\n");
    assert(code_mark_count(&e,0)==0);
    assert(code_mark_toggle(&e,0,2)==1 && code_mark_count(&e,0)==1);
    assert(code_marked(&e,0,2) && !code_marked(&e,0,1));
    assert(code_mark_toggle(&e,0,2)==0 && code_mark_count(&e,0)==0);
    code_mark_toggle(&e,0,1); code_mark_toggle(&e,0,3);
    assert(code_mark_next(&e,0,0,+1)==1 && code_mark_next(&e,0,1,+1)==3);
    assert(code_mark_next(&e,0,3,+1)==1);      /* wraps */
    assert(code_mark_next(&e,0,3,-1)==1 && code_mark_next(&e,0,0,-1)==3);
    assert(code_mark_next(&e,1,0,+1)==-1);     /* other buffer is independent */
    {   /* The limit is refused, not silently dropped. */
        code_init(&e);
        for(int i=0;i<CODE_MARKS;i++) assert(code_mark_toggle(&e,0,i)==1);
        assert(code_mark_count(&e,0)==CODE_MARKS);
        assert(code_mark_toggle(&e,0,CODE_MARKS+1)==-1);
        assert(code_mark_count(&e,0)==CODE_MARKS);
    }
    /* Command-mode keys reach bookmark toggle and navigation. Note ESC is a
     * TOGGLE: command mode persists across 'm', so a second ESC would leave
     * it and type a literal 'm' into the document instead. */
    code_init(&e); set_text("a\nb\nc\n");
    code_key(&e,27,io); code_key(&e,'m',io);
    assert(code_marked(&e,0,0));
    code_key(&e,'m',io);
    assert(!code_marked(&e,0,0));
    assert(e.buffers[0].len==6);            /* nothing was typed into the text */

    /* Indent guides are painted. They must land only inside leading
     * whitespace, so a file with none draws no guides at all. */
    code_init(&e); e.language=CODE_C;
    set_text("void f() {\n        int x;\n}\n");
    guides=0; code_paint(&e,596,417,probe,0);
    assert(guides>0);
    code_init(&e); e.language=CODE_C; set_text("int x;\n");
    guides=0; code_paint(&e,596,417,probe,0);
    assert(guides==0);

    printf("outrun_edit: buffers, syntax, bounded regex, transactional replace, UI,\n");
    printf("             brackets, auto-indent, bookmarks, guides PASS; state=%zu bytes\n",sizeof e);
}
