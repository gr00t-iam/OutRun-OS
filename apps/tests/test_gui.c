#include <assert.h>
#include <stdio.h>
#include "../gui.h"
static u32 pixels[16];
static int polls;
u64 sysc(u64 n, u64 a, u64 b, u64 c) {
    (void)a; (void)c;
    if (n == SYS_WIN_POLL) {
        if (polls++) return (u64)-1;
        struct outrun_event *e = (void *)b;
        *e = (struct outrun_event){EVENT_MOUSE_DOWN, 2, 1, 0};
        return 1;
    }
    return 0;
}
int main(void) {
    struct app_win w = {.id=0,.cw=4,.ch=4,.surf=pixels};
    app_fill(&w, 7);
    app_rect(&w, -2, -2, 3, 3, 9);
    assert(pixels[0] == 9 && pixels[1] == 7 && pixels[15] == 7);
    app_rect(&w, 3, 3, 10, 10, 11);
    assert(pixels[15] == 11 && pixels[14] == 7);
    struct outrun_event e;
    assert(app_poll(&w, &e) == 1 && e.x == 0 && e.y == 0);
    assert(app_poll(&w, &e) == -1);
    u32 glyphs[128] = {0};
    struct app_win text = {.cw=16,.ch=8,.surf=glyphs};
    app_char(&text, 0, 0, 'A', 1);
    app_char(&text, 8, 0, 'a', 1);
    int different = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            different |= glyphs[y*16+x] != glyphs[y*16+x+8];
    assert(different); /* an editor must display the actual letter case */
    assert(sizeof(struct outrun_event) == 16);
    assert(sizeof(struct outrun_process) == 48);
    assert(sizeof(struct outrun_desktop_info) == 640);
    puts("gui: clipping, event coordinates, close and ABI layout PASS");
}
