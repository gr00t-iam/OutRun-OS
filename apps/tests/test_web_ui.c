#define _POSIX_C_SOURCE 200809L
#define WEB_HOST_NETWORK
#define WEB_UI_TEST
#define APP_HOST_TEST
#include "../outrun_web.c"
#include <assert.h>
#include <stdio.h>
static unsigned surfaces[12][WEB_W*WEB_H];
static int next_id=1, published;
u64 sysc(u64 n,u64 a,u64 b,u64 c) {
    (void)c;
    if(n==SYS_WIN_CREATE) return next_id++;
    if(n==SYS_WIN_INFO) return (WEB_W<<16)|WEB_H;
    if(n==SYS_WIN_DAMAGE) { published++; return (u64)(uintptr_t)surfaces[a]; }
    (void)b;
    return (u64)-1;
}
int main(void) {
    web_ui_init();
    assert(web_tabs[0].used && web_active==0);
    web_new_tab(); assert(web_active==1 && web_tabs[1].used);
    web_navigate(1,"http://web.test/",1);
    assert(web_tabs[1].pending && web_tabs[1].hcount==1);
    web_navigate(1,"http://web.test/second",1);
    web_back(-1); assert(!strcmp(web_tabs[1].url,"http://web.test/"));
    web_back(1); assert(!strcmp(web_tabs[1].url,"http://web.test/second"));
    web_editing=1; web_select_all=1;
    struct outrun_event e={EVENT_KEY_PRESS,0,0,'x'}; web_event(&e);
    assert(!strcmp(web_address,"x"));
    e.code=27; web_event(&e); assert(!web_editing);
    web_tabs[1].doc.height=2000;
    web_scroll(300); assert(web_tabs[1].scroll==300);
    web_scroll(9999); assert(web_tabs[1].scroll==2000-WEB_VIEW_H);
    /* Drive the engine wiring. The job cannot connect here -- there is no
     * resolver on the loopback test host -- so this asserts the UI's handling
     * of a transfer that FAILS: the tab must stop pending and report why,
     * rather than spin forever on a request that will never complete.
     * web_start() succeeds (the URL is well-formed), so the failure surfaces
     * from the socket layer rather than from parsing. */
    web_navigate(1,"http://127.0.0.1:9/",1);
    for(int i=0;i<2000 && web_tabs[1].pending;i++) web_pump();
    assert(!web_tabs[1].pending && web_tabs[1].status[0]);
    /* Trust store: an EMPTY root bundle must be rejected, not accepted as "no
     * certificates, nothing to check". A browser that treats a missing trust
     * store as success would verify every certificate against nothing. */
    assert(!web_runtime_init((const unsigned char *)"", 1));
    web_close_tab(); assert(!web_tabs[1].used && web_active==0);
    static struct web_snapshot saved;
    web_navigate(0,"http://web.test/saved",1);
    web_html(&web_tabs[0].doc,"<a href='/next'>Saved</a><img src='/photo.bmp'>");
    web_tabs[0].doc.height=1000; web_tabs[0].scroll=123;
    web_tabs[0].secure=1;
    web_bookmark(web_tabs[0].url);
    assert(web_snapshot_save(&saved));
    web_tabs[0].doc.items[0].text[0]='X';
    assert(web_snapshot_restore(&saved));
    assert(web_active==1 && !web_tabs[1].pending && !web_tabs[1].secure);
    assert(!strcmp(web_tabs[1].doc.items[0].text,"Saved"));
    assert(web_tabs[1].scroll==123 && web_tabs[1].hcount==1 && web_nmarks==1);
    assert(!web_tabs[1].image_next);
    saved.version++; assert(!web_snapshot_restore(&saved)); saved.version--;
    saved.doc.count=WEB_ITEMS+1; assert(!web_snapshot_restore(&saved));

    /* web_draw is the real renderer. Exercising it here keeps the paint path
     * compiled and run by the harness rather than only by a booted image, and
     * catches an out-of-bounds surface write under ASan/UBSan at host speed.
     * The window is the app's own root, published through the normal
     * app_create/app_present pair the stub above services. */
    {
        struct app_win root;
        assert(app_create(&root,WEB_W,WEB_H,0)==0);
        web_html(&web_tabs[web_active].doc,
                 "<h1>Title</h1><p>Body text with a <a href='/next'>link</a>."
                 "<img src='/photo.bmp'></p>");
        web_tabs[web_active].doc.height=4000;   /* force the scrollbar branch */
        web_draw(&root); app_present(&root);
        web_scroll(200); web_draw(&root); app_present(&root);
        assert(published>=2);
    }
    puts("web UI: tabs/address/history/scroll/close + offline snapshot + paint PASS");
}
