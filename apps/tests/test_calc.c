/* Host test: gcc -std=c11 -Wall -Wextra -Werror apps/tests/test_calc.c -o /tmp/test_calc */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define APP_HOST_TEST
#define APP_GUI_TEST
typedef unsigned int u32;
struct app_win { int id, cw, ch; volatile u32 *surf; u32 fg, bg; };
struct outrun_event { int type, x, y, code; };
enum { EVENT_MOUSE_DOWN = 1, EVENT_KEY_PRESS = 2 };
static int create_ok = 1, event_index, presents, idles, titles;
static char screen[32];
static int app_create(struct app_win *w, int width, int height, u32 accent) {
    (void)accent; w->cw = width; w->ch = height; return create_ok ? 0 : -1;
}
static void app_fill(struct app_win *w, u32 color) { (void)w; (void)color; }
static void app_rect(struct app_win *w, int x, int y, int width, int height, u32 color) {
    (void)color; assert(x >= 0 && y >= 0 && x + width <= w->cw && y + height <= w->ch);
}
static void app_str(struct app_win *w, int x, int y, const char *s, u32 color) {
    (void)w; (void)color; (void)x;
    if (y == 36) { assert(strlen(s) < sizeof screen); strcpy(screen, s); }
}
static void app_title(struct app_win *w, const char *s) { (void)w; assert(*s); ++titles; }
static void app_present(struct app_win *w) { (void)w; ++presents; }
static void app_idle(void) { ++idles; }
static int app_poll(struct app_win *w, struct outrun_event *e) {
    (void)w;
    static const struct outrun_event events[] = {
        {EVENT_MOUSE_DOWN, 12, 148, 0}, {EVENT_KEY_PRESS, 0, 0, '+'},
        {EVENT_KEY_PRESS, 0, 0, '2'}, {EVENT_KEY_PRESS, 0, 0, '\r'}
    };
    if (event_index == 0) { ++event_index; return 0; }
    if (event_index > 4) return -1;
    *e = events[event_index++ - 1]; return 1;
}
#include "../calc.c"
static void keys(struct calc *c, const char *s) { while (*s) calc_key(c, *s++); }
static void expect(struct calc *c, const char *s) {
    char out[32]; calc_display(c, out); assert(strcmp(out, s) == 0);
}
static void test_decimal_entry(void) {
    struct calc c; calc_init(&c); expect(&c, "0");
    keys(&c, "001234567890"); expect(&c, "1234567890");
}
static void test_immediate_arithmetic(void) {
    struct calc c; calc_init(&c);
    keys(&c, "8/3\r"); expect(&c, "2");
    keys(&c, "7*6\n"); expect(&c, "42");
    keys(&c, "9+-2="); expect(&c, "7"); /* replace pending operator */
    keys(&c, "=="); expect(&c, "7"); /* no implicit repeated operation */
}
/* * and / bind tighter than + and -; equal precedence stays left-to-right. */
static void test_precedence(void) {
    struct calc c; calc_init(&c);
    keys(&c, "2+3*4="); expect(&c, "14");
    keys(&c, "8-3-2="); expect(&c, "3");
    keys(&c, "100/5/2="); expect(&c, "10");
    keys(&c, "2*3+4="); expect(&c, "10");
    keys(&c, "12+3*4-70/3="); expect(&c, "1");
    keys(&c, "10-2*3="); expect(&c, "4");
    keys(&c, "1+2+3*4*5="); expect(&c, "63");
    /* The deferred product must be overflow-checked when it is finally applied. */
    keys(&c, "1+999999999*999999999*9="); expect(&c, "Overflow");
    keys(&c, "C"); expect(&c, "0");
    /* A deferred divide by zero is still a divide by zero. */
    keys(&c, "5+7/0="); expect(&c, "Division by zero");
    keys(&c, "3"); expect(&c, "3");
}
static void test_errors(void) {
    const char *overflow[] = {"9999999999999999999", "999999999999999999+1=",
        "0-999999999999999999-1=", "999999999999999999*2=",
        "0-999999999999999999*2="};
    struct calc c;
    for (unsigned i = 0; i < sizeof overflow / sizeof *overflow; ++i) {
        calc_init(&c); keys(&c, overflow[i]); expect(&c, "Overflow");
        keys(&c, "+="); expect(&c, "Overflow");
        keys(&c, "8"); expect(&c, "8");
    }
    calc_init(&c); keys(&c, "1/0="); expect(&c, "Division by zero");
    keys(&c, "7+2="); expect(&c, "9");
    calc_init(&c); keys(&c, "999999999999999999*1="); expect(&c, "999999999999999999");
    calc_init(&c); keys(&c, "0-999999999999999999="); expect(&c, "-999999999999999999");
}
static void test_editing(void) {
    struct calc c; calc_init(&c); keys(&c, "123\b"); expect(&c, "12");
    keys(&c, "\177"); expect(&c, "1");
    keys(&c, "s2"); expect(&c, "-12");
    keys(&c, "\b"); expect(&c, "-1");
    keys(&c, "s"); expect(&c, "1");
    keys(&c, "C9+3c2"); expect(&c, "2");
    keys(&c, "\033"); expect(&c, "0");
    keys(&c, "8+2=\b"); expect(&c, "0");
    keys(&c, "4+5s="); expect(&c, "-1");
    keys(&c, "C999999999999999999s9"); expect(&c, "Overflow");
    keys(&c, "C"); expect(&c, "0");
    keys(&c, "12.xyz "); calc_key(&c, 0x100); expect(&c, "12");
}
static void test_mouse_grid(void) {
    const char layout[] = "C\bs/789*456-123+0  =";
    struct calc c;
    for (int i = 0; i < 20; ++i) {
        int x = 12 + (i % 4) * 76, y = 104 + (i / 4) * 44;
        int want = layout[i] == ' ' ? 0 : layout[i];
        assert(calc_hit(x, y) == want);
        assert(calc_hit(x + 69, y + 37) == want);
        assert(calc_hit(x + 70, y) == 0);
        assert(calc_hit(x, y + 38) == 0);
    }
    assert(calc_hit(-1, 104) == 0); assert(calc_hit(12, -1) == 0);
    assert(calc_hit(316, 104) == 0); assert(calc_hit(12, 324) == 0);
    assert(calc_hit(2147483647, 2147483647) == 0);
    calc_init(&c);
    calc_key(&c, calc_hit(12, 148)); /* 7 */
    calc_key(&c, calc_hit(240, 236)); /* + */
    calc_key(&c, calc_hit(88, 236)); /* 2 */
    calc_key(&c, calc_hit(240, 280)); /* = */
    expect(&c, "9");
}
static void test_gui_lifecycle(void) {
    assert(calc_run() == 0); assert(strcmp(screen, "9") == 0);
    assert(event_index == 5); assert(presents == 5); assert(idles == 1); assert(titles == 1);
    create_ok = 0; assert(calc_run() == 1); assert(presents == 5);
}
int main(void) {
    test_decimal_entry(); test_immediate_arithmetic(); test_precedence(); test_errors();
    test_editing(); test_mouse_grid(); test_gui_lifecycle();
    puts("calc: all tests passed"); return 0;
}
