#ifndef OUTRUN_GUI_H
#define OUTRUN_GUI_H
#include "../include/outrun_abi.h"
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;
#ifdef APP_HOST_TEST
u64 sysc(u64 num, u64 a0, u64 a1, u64 a2);
#else
static inline u64 sysc(u64 num, u64 a0, u64 a1, u64 a2) {
    u64 ret;
    __asm__ volatile("syscall" : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2) : "rcx", "r11", "memory");
    return ret;
}
#endif
struct app_win { int id, cw, ch; volatile u32 *surf; u32 fg, bg; };
int app_create(struct app_win *w, int width, int height, u32 accent);
void app_fill(struct app_win *w, u32 c);
void app_rect(struct app_win *w, int x, int y, int width, int height, u32 c);
void app_char(struct app_win *w, int x, int y, char ch, u32 c);
void app_str(struct app_win *w, int x, int y, const char *s, u32 c);
void app_u32(struct app_win *w, int x, int y, u32 value, u32 c);
int app_poll(struct app_win *w, struct outrun_event *event);
void app_title(struct app_win *w, const char *title);
void app_present(struct app_win *w);
void app_idle(void);
__attribute__((noreturn)) void app_exit(int status);
#endif
