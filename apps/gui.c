#include "gui.h"
#include "font.h"
#include "ui_font.h"

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
/* UI labels use a baked antialiased proportional font. app_char/app_str keep
 * their existing cell geometry for terminal output and editable source text. */
int app_text_width(const char *s) {
    int width=0;
    for(;*s;s++) {
        unsigned c=(u8)*s; if(c<32 || c>126) c='?';
        int n=ui_font_advance[c-32];
        if(width>2147483647-n) return 2147483647;
        width+=n;
    }
    return width;
}
void app_text(struct app_win *w,int x,int y,const char *s,u32 color) {
    i64 pen=x;
    for(;*s && pen<w->cw;s++) {
        unsigned c=(u8)*s; if(c<32 || c>126) c='?';
        const u8 *glyph=ui_font_coverage[c-32];
        for(int r=0;r<UI_FONT_H;r++) for(int col=0;col<UI_FONT_W;col++) {
            i64 xx=pen+col, yy=(i64)y+r;
            unsigned a=glyph[r*UI_FONT_W+col];
            if(!a || xx<0 || yy<0 || xx>=w->cw || yy>=w->ch) continue;
            u32 bg=w->surf[yy*w->cw+xx], out=0;
            for(int shift=0;shift<=16;shift+=8)
                out|=((((color>>shift)&255)*a+((bg>>shift)&255)*(255-a)+127)/255)<<shift;
            w->surf[yy*w->cw+xx]=out;
        }
        pen+=ui_font_advance[c-32];
    }
}
int app_hit(int px,int py,int x,int y,int width,int height) {
    return width>0 && height>0 && (i64)px>=x && (i64)py>=y &&
        (i64)px<(i64)x+width && (i64)py<(i64)y+height;
}
int app_scroll_to(int requested,int total,int visible) {
    if(visible<1) visible=1;
    int max=total>visible?total-visible:0;
    return requested<0?0:requested>max?max:requested;
}
void app_button(struct app_win *w,int x,int y,int width,int height,const char *label,int active) {
    if(width<2 || height<2) return;
    app_rect(w,x,y,width,height,active?0x43687b:0x344255);
    app_rect(w,x+1,y+1,width-2,height-2,active?0x28495d:0x1c2636);
    int tx=x+(width-app_text_width(label))/2;
    /* Only draw labels that fit; never leak into a neighbouring control. */
    if(tx>=x+2 && height>=UI_FONT_H+2) app_text(w,tx,y+(height-UI_FONT_H)/2,label,active?0x9deaff:0xeaf2f7);
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
