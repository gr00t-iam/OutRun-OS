#include "gui.h"
#include "font.h"

static void pixel(struct app_win *w, int x, int y, u32 c) {
    if ((unsigned)x < (unsigned)w->cw && (unsigned)y < (unsigned)w->ch)
        w->surf[y * w->cw + x] = c;
}
void app_fill(struct app_win *w, u32 c) {
    for (int i = 0; i < w->cw * w->ch; ++i) w->surf[i] = c;
}
void app_rect(struct app_win *w, int x, int y, int width, int height, u32 c) {
    if (width <= 0 || height <= 0) return;
    i64 right = (i64)x + width, bottom = (i64)y + height;
    if (right > w->cw) right = w->cw;
    if (bottom > w->ch) bottom = w->ch;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    for (int j = y; j < bottom; ++j)
        for (int i = x; i < right; ++i) pixel(w, i, j, c);
}
void app_char(struct app_win *w, int x, int y, char ch, u32 c) {
    if ((unsigned char)ch < 32 || (unsigned char)ch > 126) ch = '?';
    const u8 *glyph = ch >= 96 ? g_font_lower[(unsigned)ch - 96] : g_font[(unsigned)ch - 32];
    for (int r = 0; r < 8; ++r)
        for (int b = 0; b < 8; ++b)
            if (glyph[r] & (1u << b)) pixel(w, x+b, y+r, c);
}
void app_str(struct app_win *w, int x, int y, const char *s, u32 c) {
    for (; *s && x < w->cw; ++s, x += 8) app_char(w, x, y, *s, c);
}
void app_u32(struct app_win *w, int x, int y, u32 value, u32 c) {
    char digits[10]; int n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) { app_char(w, x, y, digits[--n], c); x += 8; }
}
int app_create(struct app_win *w, int width, int height, u32 accent) {
    i64 id = (i64)sysc(SYS_WIN_CREATE, ((u64)width << 16) | (u64)height, accent, 1);
    if (id < 0) return -1;
    i64 dims = (i64)sysc(SYS_WIN_INFO, (u64)id, 0, 0);
    if (dims < 0) return -1;
    w->id = (int)id; w->cw = (int)((u64)dims >> 16); w->ch = (int)(dims & 65535);
    /* First publication obtains the drawing buffer; never draw into front. */
    i64 back = (i64)sysc(SYS_WIN_DAMAGE, id, 0, 0);
    if (back <= 0) return -1;
    w->surf = (volatile u32 *)(u64)back; w->fg = 0xeaf2f7; w->bg = 0x0a0d14;
    return 0;
}
int app_poll(struct app_win *w, struct outrun_event *e) {
    i64 result = (i64)sysc(SYS_WIN_POLL, (u64)w->id, (u64)e, 0);
    if (result == 1 && e->type == EVENT_MOUSE_DOWN) { e->x -= 2; e->y -= 1; }
    return result < 0 ? -1 : (int)result;
}
void app_title(struct app_win *w, const char *title) {
    i64 next = (i64)sysc(SYS_WIN_DAMAGE, (u64)w->id, (u64)title, 0);
    if (next <= 0) app_exit(1);
    w->surf = (volatile u32 *)(u64)next;
}
void app_present(struct app_win *w) {
    i64 next = (i64)sysc(SYS_WIN_DAMAGE, (u64)w->id, 0, 0);
    if (next <= 0) app_exit(0);
    w->surf = (volatile u32 *)(u64)next;
}
void app_idle(void) { sysc(SYS_YIELD, 0, 0, 0); }
void app_exit(int status) {
    sysc(SYS_EXIT, (u64)status, 0, 0);
    for (;;) __asm__ volatile("pause");
}
