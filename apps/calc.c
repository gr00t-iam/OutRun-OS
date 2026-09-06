/* Freestanding integer calculator. No floating point or libc required. */
#define CALC_LIMIT 999999999999999999LL
/* Two precedence levels need at most two pending operands: `lhs op` is the
 * innermost pending operation, and `low_lhs low_op` is a + or - held open
 * beneath it while a * or / chain is evaluated. With only two levels there is
 * never a third, so this is a complete stack, not a truncated one. */
struct calc { long long value, lhs, low_lhs; int op, low_op, fresh, entered, error; };
static void calc_init(struct calc *c) {
    c->value = c->lhs = c->low_lhs = 0;
    c->op = c->low_op = c->entered = c->error = 0; c->fresh = 1;
}
static int calc_is_mul(int op) { return op == '*' || op == '/'; }
/* c->value = a (op) b, with the same overflow and zero-divide rules as before. */
static void calc_binop(struct calc *c, long long a, int op, long long b) {
    if (op == '+') c->value = a + b;
    if (op == '-') c->value = a - b;
    if (op == '*') {
        long long aa = a < 0 ? -a : a, bb = b < 0 ? -b : b;
        if (bb && aa > CALC_LIMIT / bb) { c->error = 1; return; }
        c->value = a * b;
    }
    if (op == '/') {
        if (!b) { c->error = 2; return; }
        c->value = a / b;
    }
    /* Addition/subtraction of two bounded 18-digit values cannot overflow i64. */
    if (c->value > CALC_LIMIT || c->value < -CALC_LIMIT) c->error = 1;
}
/* Fold the innermost pending operation into value. */
static void calc_apply(struct calc *c) {
    calc_binop(c, c->lhs, c->op, c->value);
    c->op = 0;
}
/* Fold the held low-precedence operation, if any, into value. */
static void calc_apply_low(struct calc *c) {
    if (!c->low_op || c->error) return;
    calc_binop(c, c->low_lhs, c->low_op, c->value);
    c->low_op = 0;
}
/* Precedence-aware execution: * and / bind tighter than + and -, equal
 * precedence is left-to-right. Enter is equals, including numpad ASCII. */
static void calc_key(struct calc *c, int key) {
    if (key == 'C' || key == 'c' || key == 27) { calc_init(c); return; }
    if (c->error) {
        if (key >= '0' && key <= '9') calc_init(c);
        else return;
    }
    if (key >= '0' && key <= '9') {
        if (c->fresh) c->value = 0;
        c->fresh = 0; c->entered = 1;
        long long magnitude = c->value < 0 ? -c->value : c->value;
        if (magnitude > (CALC_LIMIT - (key - '0')) / 10) c->error = 1;
        else c->value = c->value * 10 + (c->value < 0 ? -(key - '0') : key - '0');
    } else if (key == 8 || key == 127) {
        if (c->fresh) c->value = 0;
        else c->value /= 10;
        c->fresh = 0; c->entered = 1;
    } else if (key == 's' || key == 'S') {
        if (c->fresh && c->op) c->value = 0;
        c->value = -c->value; c->fresh = 0; c->entered = 1;
    } else if (key == '+' || key == '-' || key == '*' || key == '/' ||
               key == '=' || key == '\r' || key == '\n') {
        int equals = (key == '=' || key == '\r' || key == '\n');
        if (c->op && c->entered) {
            /* An operand was typed after the pending operator. A tighter
             * incoming operator over a looser pending one is the ONE case
             * where the pending operation must wait: hold it low. */
            if (!equals && calc_is_mul(key) && !calc_is_mul(c->op)) {
                if (c->low_op) {
                    /* Two lows cannot be held: fold the older one first. The
                     * held one is always older than the pending one, and both
                     * are + or -, so left-to-right order is preserved. */
                    long long v = c->value; c->value = c->lhs;
                    calc_apply_low(c); c->lhs = c->value; c->value = v;
                }
                c->low_lhs = c->lhs; c->low_op = c->op; c->op = 0;
            } else {
                calc_apply(c);
                if (equals || !calc_is_mul(key)) calc_apply_low(c);
            }
        } else if (!c->op && c->entered && c->low_op && (equals || !calc_is_mul(key))) {
            /* No pending operator, but a held low one: fold it now. */
            calc_apply_low(c);
        }
        if (c->error) return;
        c->lhs = c->value;
        c->op = equals ? 0 : key;
        c->fresh = 1; c->entered = 0;
    }
}
/* Shared by rendering and hit testing: 4 columns, 5 rows, pixel content coords. */
#define CALC_GRID_X 12
#define CALC_GRID_Y 104
#define CALC_CELL_W 76
#define CALC_CELL_H 44
#define CALC_BUTTON_W 70
#define CALC_BUTTON_H 38
static const char calc_buttons[] = "C\bs/789*456-123+0  =";
static int calc_hit(int x, int y) {
    if (x < CALC_GRID_X || x >= CALC_GRID_X + 4 * CALC_CELL_W ||
        y < CALC_GRID_Y || y >= CALC_GRID_Y + 5 * CALC_CELL_H) return 0;
    x -= CALC_GRID_X; y -= CALC_GRID_Y;
    if (x % CALC_CELL_W >= CALC_BUTTON_W || y % CALC_CELL_H >= CALC_BUTTON_H) return 0;
    int key = calc_buttons[(y / CALC_CELL_H) * 4 + x / CALC_CELL_W];
    return key == ' ' ? 0 : key;
}
/* Caller supplies at least 32 bytes. */
static void calc_display(const struct calc *c, char out[32]) {
    char rev[24]; int n = 0, p = 0; long long v = c->value;
    if (c->error) {
        const char *s = c->error == 1 ? "Overflow" : "Division by zero";
        while (*s) out[p++] = *s++;
        out[p] = 0; return;
    }
    if (v < 0) { out[p++] = '-'; v = -v; }
    do { rev[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[p++] = rev[--n];
    out[p] = 0;
}

#ifndef APP_HOST_TEST
#include "gui.h"
#endif
#if !defined(APP_HOST_TEST) || defined(APP_GUI_TEST)
static void calc_render(struct app_win *w, const struct calc *c) {
    char text[32];
    app_fill(w, 0x101521);
    app_str(w, 12, 10, "INTEGER  |  +/-18 digits", 0x99AABD);
    app_rect(w, 12, 28, 298, 32, 0x070B12);
    calc_display(c, text);
    app_str(w, 20, 36, text, c->error ? 0xFF7790 : 0xF0F4FF);
    app_str(w, 12, 66, "* / before + -;  / truncates to zero", 0x99AABD);
    /* A held + or - shows beside the pending * or /, so "2 + 3 *" reads "+ *". */
    text[0] = c->low_op ? (char)c->low_op : ' ';
    text[1] = ' ';
    text[2] = c->op ? (char)c->op : ' '; text[3] = 0;
    app_str(w, 12, 84, "Pending:", 0x99AABD);
    app_str(w, 84, 84, text, 0x67DBCF);
    for (int i = 0; i < 20; ++i) {
        int key = calc_buttons[i];
        int x = CALC_GRID_X + (i % 4) * CALC_CELL_W;
        int y = CALC_GRID_Y + (i / 4) * CALC_CELL_H;
        if (key == ' ') continue;
        app_rect(w, x, y, CALC_BUTTON_W, CALC_BUTTON_H,
                 key == '=' ? 0x216B68 : (key >= '0' && key <= '9' ? 0x293346 : 0x40405A));
        text[0] = (char)key; text[1] = 0;
        app_str(w, x + 22, y + 12, key == 8 ? "DEL" : key == 's' ? "+/-" : text, 0xF0F4FF);
    }
    app_str(w, 12, 332, "C/Esc clear  S sign  Backspace del", 0x99AABD);
    app_str(w, 12, 350, "Enter =   Keyboard/numpad ASCII", 0x99AABD);
    app_present(w);
}
static int calc_run(void) {
    struct app_win w; struct calc c; struct outrun_event e;
    if (app_create(&w, 328, 374, 0x67DBCF) < 0) return 1;
    app_title(&w, "Calculator - Integer");
    calc_init(&c); calc_render(&w, &c);
    for (;;) {
        int status = app_poll(&w, &e);
        if (status < 0) return 0;
        if (!status) { app_idle(); continue; }
        int key = 0;
        if (e.type == EVENT_KEY_PRESS) key = e.code;
        if (e.type == EVENT_MOUSE_DOWN) key = calc_hit(e.x, e.y);
        if (key) { calc_key(&c, key); calc_render(&w, &c); }
    }
}
#endif
#ifndef APP_HOST_TEST
void _start(void) { app_exit(calc_run()); }
#endif
