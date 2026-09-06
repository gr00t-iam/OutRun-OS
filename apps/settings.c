#include "gui.h"
static int settings_change(struct outrun_settings *s, int item) {
    static const u32 colors[] = {0x22e4ff, 0x3df5c4, 0xffb020, 0xff2d9b};
    if (item == 0) s->scale = s->scale == 1 ? 2 : 1;
    else if (item == 1) {
        int next = 0;
        for (int i = 0; i < 4; ++i) if (s->accent == colors[i]) next = (i + 1) % 4;
        s->accent = colors[next];
    } else if (item == 2) s->repeat_delay = s->repeat_delay >= 100 ? 25 : s->repeat_delay + 25;
    else if (item == 3) s->repeat_period = s->repeat_period >= 20 ? 5 : s->repeat_period + 5;
    else return 0;
    return 1;
}
/* Instant apply. A row click changes the value AND pushes it to the kernel
 * in the same step, so what is on screen is what is live. If the kernel
 * refuses, the local copy is rolled back to the pre-click value: a settings
 * app must never display a value the desktop is not honouring. Returns 1
 * applied, -1 refused (and rolled back), 0 not a settable item. */
typedef long long (*settings_push_fn)(const struct outrun_settings *);
static int settings_apply_item(struct outrun_settings *s, int item, settings_push_fn push) {
    struct outrun_settings before = *s;
    if (!settings_change(s, item)) return 0;
    if (push(s) < 0) { *s = before; return -1; }
    return 1;
}
#ifndef APP_HOST_TEST
static long long settings_push(const struct outrun_settings *s) {
    return (long long)sysc(SYS_DESKTOP_SETTINGS, (u64)s, sizeof *s, 0);
}
static void settings_render(struct app_win *w, struct outrun_settings *s, const char *status) {
    static const char *labels[] = {"DESKTOP SCALE", "THEME ACCENT", "REPEAT DELAY MS", "REPEAT PERIOD MS"};
    app_fill(w, w->bg);
    app_str(w, 8, 8, "CONTROL DECK", s->accent);
    app_str(w, 8, 25, "CLICK A ROW TO CHANGE; APPLIES AT ONCE", 0x7c8ca0);
    for (int i = 0; i < 4; ++i) {
        int y = 48 + i * 42;
        app_rect(w, 8, y, w->cw - 16, 32, 0x172331);
        app_str(w, 16, y + 12, labels[i], w->fg);
        if (i == 1) app_rect(w, 252, y + 7, 60, 18, s->accent);
        else app_u32(w, 252, y + 12, i == 0 ? s->scale : i == 2 ? s->repeat_delay * 10 : s->repeat_period * 10, w->fg);
    }
    app_rect(w, 8, 224, 112, 30, 0x25374b);
    app_str(w, 24, 235, "RELOAD", w->fg);
    app_str(w, 8, 272, status, 0xffb020);
    app_str(w, 8, 294, "SESSION SETTINGS; NO DISK WRITE", 0x7c8ca0);
    app_present(w);
}
static int settings_read(struct outrun_settings *s) {
    struct outrun_desktop_info info;
    if ((i64)sysc(SYS_DESKTOP_INFO, (u64)&info, sizeof info, 0) < 0 || info.version != 1) return -1;
    *s = (struct outrun_settings){info.scale, info.accent, info.repeat_delay, info.repeat_period};
    return 0;
}
void _start(void) {
    struct app_win w;
    struct outrun_settings s;
    const char *status = "READY";
    if (settings_read(&s) || app_create(&w, 390, 350, 0xffb020)) app_exit(1);
    app_title(&w, "CONTROL DECK");
    settings_render(&w, &s, status);
    for (;;) {
        struct outrun_event e;
        int rc, dirty = 0;
        while ((rc = app_poll(&w, &e)) > 0) {
            if (e.type != EVENT_MOUSE_DOWN) continue;
            if (e.x >= 8 && e.x < w.cw - 8 && e.y >= 48 && e.y < 216) {
                int row = (e.y - 48) / 42;
                if ((e.y - 48) % 42 < 32) {
                    int r = settings_apply_item(&s, row, settings_push);
                    if (r) {
                        status = r > 0 ? "APPLIED" : "REJECTED / UNSUPPORTED / NOT ADMIN";
                        dirty = 1;
                    }
                }
            } else if (e.y >= 224 && e.y < 254 && e.x >= 8 && e.x < 120) {
                status = settings_read(&s) ? "READBACK FAILED" : "RELOADED"; dirty = 1;
            }
        }
        if (rc < 0) app_exit(0);
        if (dirty) settings_render(&w, &s, status);
        app_idle();
    }
}
#endif
