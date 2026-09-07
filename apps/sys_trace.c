/* OutRun Trace — per-core load, memory, threads and interrupt distribution.
 *
 * This is the measuring half of SYS-DIAG taken further. SYS-DIAG shows each
 * PROCESS's share of CPU; this shows where the machine's time and interrupts
 * actually land, per core, which is a different question and needs data the
 * desktop ABI never carried.
 *
 * EVERYTHING HERE IS A DELTA. The kernel serves lifetime totals — busy
 * nanoseconds banked by the core that hosted each ring-3 excursion, interrupts
 * counted at the one dispatch point every vector passes through — and a
 * lifetime total rendered as a bar is a picture of uptime, not of load. Two
 * samples and the wall clock between them is what turns those into a rate.
 *
 * The four ways that goes wrong are handled explicitly rather than left to
 * arithmetic: no previous sample, no elapsed time, a counter that appears to
 * move backwards, and a core that is not online at all. An offline core is not
 * a core running at 0%, and averaging over it would report a saturated machine
 * as mostly idle. */
#include "../include/outrun_abi.h"

#define TRACE_HIST 100
enum { TRACE_TAB_CPU = 0, TRACE_TAB_MEM = 1, TRACE_TAB_IRQ = 2,
       TRACE_TAB_THREADS = 3, TRACE_NTAB = 4 };

struct trace_state {
    struct outrun_trace_info prev;
    unsigned cpu_pct[HW_MAX_CPU];
    unsigned long long excur_delta[HW_MAX_CPU];
    unsigned long long irq_delta[HW_MAX_IRQ];
    unsigned long long irq_percpu_delta[HW_MAX_IRQ][HW_MAX_CPU];
    unsigned long long frames_freed_delta, frames_reused_delta;
    unsigned mem_pct, samples;
    int irq_busiest, tab, have_prev;
    unsigned char load_hist[TRACE_HIST], mem_hist[TRACE_HIST];
};

/* Percent of `whole`, saturating at 100, zero when there is no denominator.
 * The shift-down loop matters here: wall_ns is a nanosecond clock and
 * `part * 100` overflows a 64-bit product after about six years of uptime. */
static unsigned trace_pct(unsigned long long part, unsigned long long whole) {
    if (!whole) return 0;
    if (part >= whole) return 100;
    while (part > (unsigned long long)~0ull / 100) { part >>= 1; whole >>= 1; }
    if (!whole) return 0;
    return (unsigned)(part * 100 / whole);
}
/* A monotonic counter's increase, or 0 if it did not increase. A decrease is
 * not arithmetic to be performed: it means the sample is not comparable, and
 * subtracting anyway would print an enormous rate out of a quiet machine. */
static unsigned long long trace_delta(unsigned long long now, unsigned long long was) {
    return now > was ? now - was : 0ull;
}
static int trace_core_offline(const struct trace_state *t, int c) {
    return c >= 0 && c < HW_MAX_CPU && !t->prev.cpu_online[c];
}
/* The mean over ONLINE cores. See the file header for why the denominator is
 * not simply HW_MAX_CPU, or even ncpu. */
static unsigned trace_mean_load(const struct trace_state *t) {
    unsigned sum = 0, n = 0;
    for (int c = 0; c < HW_MAX_CPU; c++) {
        if (!t->prev.cpu_online[c]) continue;
        sum += t->cpu_pct[c];
        n++;
    }
    return n ? sum / n : 0;
}
static const char *trace_irq_name(unsigned line) {
    static char buf[12];
    switch (line) {
    case 0:  return "PIT TIMER";
    case 1:  return "PS/2 KEYBOARD";
    case 2:  return "PIC CASCADE";
    case 12: return "PS/2 MOUSE";
    /* 10 and 11 are where this machine's virtio functions land. Named for what
     * they ARE — a shared PCI INTx line — rather than for one device that
     * happens to sit on it, because INTx lines are shared and labelling the
     * line after a single driver would misattribute every other device's
     * interrupts to it. */
    case 10: case 11: return "PCI INTX";
    default: break;
    }
    const char *p = "LINE ";
    int at = 0;
    while (*p) buf[at++] = *p++;
    if (line >= 10) buf[at++] = (char)('0' + line / 10);
    buf[at++] = (char)('0' + line % 10);
    buf[at] = 0;
    return buf;
}
/* The kernel scheduler's own enumeration; see `enum { T_FREE, ... }`. */
static const char *trace_thread_state(unsigned s) {
    switch (s) {
    case 0: return "FREE";
    case 1: return "RUNNABLE";
    case 2: return "RUNNING";
    case 3: return "BLOCKED";
    case 4: return "CLAIMED";
    default: return "UNKNOWN";
    }
}
static void trace_reset(struct trace_state *t) {
    unsigned char *z = (unsigned char *)t;
    for (unsigned long i = 0; i < sizeof *t; i++) z[i] = 0;
    t->irq_busiest = -1;
}
static void trace_sample(struct trace_state *t, const struct outrun_trace_info *s) {
    unsigned long long elapsed = trace_delta(s->wall_ns, t->prev.wall_ns);
    /* MEMORY IS A LEVEL, NOT A RATE, and is therefore taken from this sample
     * alone — it is meaningful on the very first one, where no percentage of
     * CPU can be. */
    t->mem_pct = trace_pct(s->frames_used, s->frames_total);

    if (t->have_prev && elapsed) {
        for (int c = 0; c < HW_MAX_CPU; c++) {
            t->cpu_pct[c] = trace_pct(trace_delta(s->cpu_busy_ns[c], t->prev.cpu_busy_ns[c]),
                                      elapsed);
            t->excur_delta[c] = trace_delta(s->cpu_excursions[c], t->prev.cpu_excursions[c]);
        }
        t->irq_busiest = -1;
        unsigned long long best = 0;
        for (int l = 0; l < HW_MAX_IRQ; l++) {
            t->irq_delta[l] = trace_delta(s->irq[l].total, t->prev.irq[l].total);
            for (int c = 0; c < HW_MAX_CPU; c++)
                t->irq_percpu_delta[l][c] =
                    trace_delta(s->irq[l].percpu[c], t->prev.irq[l].percpu[c]);
            if (t->irq_delta[l] > best) { best = t->irq_delta[l]; t->irq_busiest = l; }
        }
        t->frames_freed_delta = trace_delta(s->frames_freed, t->prev.frames_freed);
        t->frames_reused_delta = trace_delta(s->frames_reused, t->prev.frames_reused);
    }
    t->prev = *s;
    t->have_prev = 1;
    t->load_hist[t->samples % TRACE_HIST] = (unsigned char)trace_mean_load(t);
    t->mem_hist[t->samples % TRACE_HIST] = (unsigned char)t->mem_pct;
    t->samples++;
}
static void trace_set_tab(struct trace_state *t, int tab) {
    if (tab < 0 || tab >= TRACE_NTAB) return;      /* refused, not wrapped */
    t->tab = tab;
}

#ifndef APP_HOST_TEST
#include "gui.h"

static struct trace_state state;
static struct outrun_trace_info sample;

static void trace_graph(struct app_win *w, const unsigned char *values, unsigned count,
                        int x, int y, int h, u32 color) {
    app_rect(w, x, y, TRACE_HIST * 4, h, 0x121722);
    unsigned n = count < TRACE_HIST ? count : TRACE_HIST;
    unsigned first = count - n;
    for (unsigned i = 0; i < n; ++i) {
        int v = (int)((unsigned)values[(first + i) % TRACE_HIST] * (unsigned)(h - 2) / 100u);
        app_rect(w, x + (int)i * 4, y + h - 1 - v, 3, v + 1, color);
    }
}
static void trace_bar(struct app_win *w, int x, int y, int width, unsigned pct, u32 color) {
    app_rect(w, x, y, width, 10, 0x121722);
    app_rect(w, x, y, (int)((unsigned)width * pct / 100u), 10, color);
}
static void trace_render(struct app_win *w, struct trace_state *t) {
    static const char *tabs[TRACE_NTAB] = { "CPU", "MEMORY", "IRQ", "THREADS" };
    const struct outrun_trace_info *s = &t->prev;
    app_fill(w, w->bg);
    for (int i = 0; i < TRACE_NTAB; i++) {
        app_rect(w, i * 78, 0, 74, 22, t->tab == i ? 0x28495d : 0x1c2636);
        app_str(w, i * 78 + 8, 7, tabs[i], t->tab == i ? 0x22e4ff : 0x7c8ca0);
    }
    int y = 32;
    if (t->tab == TRACE_TAB_CPU) {
        app_str(w, 8, y, "MEAN LOAD OVER ONLINE CORES", 0x22e4ff);
        app_u32(w, 300, y, trace_mean_load(t), w->fg);
        app_str(w, 332, y, "%", w->fg);
        y += 14;
        trace_graph(w, t->load_hist, t->samples, 8, y, 48, 0x22e4ff);
        y += 56;
        app_str(w, 8, y, "CORE", 0x7c8ca0);
        app_str(w, 56, y, "LOAD", 0x7c8ca0);
        app_str(w, 232, y, "EXCURSIONS", 0x7c8ca0);
        app_str(w, 336, y, "PID", 0x7c8ca0);
        y += 16;
        for (unsigned c = 0; c < HW_MAX_CPU; c++) {
            if (trace_core_offline(t, (int)c) && !s->cpu_busy_ns[c]) continue;
            app_u32(w, 8, y, c, w->fg);
            if (trace_core_offline(t, (int)c)) {
                /* Said out loud. An offline core drawn as an empty bar is
                 * indistinguishable from an idle one. */
                app_str(w, 56, y, "OFFLINE", 0x7c8ca0);
            } else {
                app_u32(w, 56, y, t->cpu_pct[c], w->fg);
                app_str(w, 88, y, "%", w->fg);
                trace_bar(w, 112, y, 112, t->cpu_pct[c],
                          t->cpu_pct[c] > 80 ? 0xff2d9b : 0x3df5c4);
                app_u32(w, 232, y, (u32)t->excur_delta[c], 0x3df5c4);
                app_u32(w, 336, y, (u32)s->cpu_cur_pid[c], w->fg);
            }
            y += 14;
        }
        y += 8;
        app_str(w, 8, y, "CORES ONLINE", 0x7c8ca0);
        app_u32(w, 176, y, s->ncpu, w->fg);
        y += 14;
        app_str(w, 8, y, "TICKS", 0x7c8ca0);
        app_u32(w, 176, y, (u32)s->ticks, w->fg);
        y += 14;
        app_str(w, 8, y, "UPTIME MS", 0x7c8ca0);
        app_u32(w, 176, y, (u32)(s->wall_ns / 1000000ull), w->fg);
    } else if (t->tab == TRACE_TAB_MEM) {
        app_str(w, 8, y, "PHYSICAL FRAME ALLOCATOR", 0x3df5c4);
        app_u32(w, 300, y, t->mem_pct, w->fg);
        app_str(w, 332, y, "%", w->fg);
        y += 14;
        trace_graph(w, t->mem_hist, t->samples, 8, y, 48, 0x3df5c4);
        y += 60;
        app_str(w, 8, y, "PAGES USED", 0x7c8ca0);
        app_u32(w, 200, y, (u32)s->frames_used, 0xffb020); y += 14;
        app_str(w, 8, y, "PAGES TOTAL", 0x7c8ca0);
        app_u32(w, 200, y, (u32)s->frames_total, w->fg); y += 14;
        app_str(w, 8, y, "BYTES USED", 0x7c8ca0);
        app_u32(w, 200, y, (u32)(s->frames_used * 4096ull / 1024ull), w->fg);
        app_str(w, 296, y, "KIB", w->fg); y += 18;
        app_str(w, 8, y, "FRAMES FREED", 0x7c8ca0);
        app_u32(w, 200, y, (u32)s->frames_freed, w->fg);
        app_str(w, 296, y, "+", 0x7c8ca0);
        app_u32(w, 312, y, (u32)t->frames_freed_delta, 0x3df5c4); y += 14;
        app_str(w, 8, y, "FRAMES REUSED", 0x7c8ca0);
        app_u32(w, 200, y, (u32)s->frames_reused, w->fg);
        app_str(w, 296, y, "+", 0x7c8ca0);
        app_u32(w, 312, y, (u32)t->frames_reused_delta, 0x3df5c4); y += 14;
        app_str(w, 8, y, "PAGES SHARED", 0x7c8ca0);
        app_u32(w, 200, y, (u32)s->frames_shared, w->fg); y += 14;
        app_str(w, 8, y, "COW COPIES", 0x7c8ca0);
        app_u32(w, 200, y, (u32)s->frames_cow, w->fg); y += 18;
        app_str(w, 8, y, "PROCESSES", 0x7c8ca0);
        app_u32(w, 200, y, s->nproc, w->fg);
    } else if (t->tab == TRACE_TAB_IRQ) {
        app_str(w, 8, y, "LINE", 0x7c8ca0);
        app_str(w, 48, y, "SOURCE", 0x7c8ca0);
        app_str(w, 176, y, "TOTAL", 0x7c8ca0);
        app_str(w, 264, y, "PER SAMPLE", 0x7c8ca0);
        y += 16;
        int shown = 0;
        for (unsigned l = 0; l < HW_MAX_IRQ; l++) {
            if (!s->irq[l].total) continue;
            shown++;
            u32 c = ((int)l == t->irq_busiest && t->irq_delta[l]) ? 0x22e4ff : w->fg;
            app_u32(w, 8, y, l, c);
            app_str(w, 48, y, trace_irq_name(l), c);
            app_u32(w, 176, y, (u32)s->irq[l].total, c);
            app_u32(w, 264, y, (u32)t->irq_delta[l], 0x3df5c4);
            y += 12;
            /* The per-core split, indented under its line. On a uniprocessor
             * boot every line reports cpu0 and that IS the distribution — a
             * blank here would read as missing data rather than as the answer. */
            int x = 64;
            for (unsigned c2 = 0; c2 < HW_MAX_CPU; c2++) {
                if (!s->irq[l].percpu[c2]) continue;
                app_str(w, x, y, "CPU", 0x7c8ca0);
                app_u32(w, x + 24, y, c2, 0x7c8ca0);
                app_str(w, x + 32, y, ":", 0x7c8ca0);
                app_u32(w, x + 40, y, (u32)s->irq[l].percpu[c2], 0x8293a8);
                x += 104;
                if (x > w->cw - 96) break;
            }
            y += 14;
        }
        if (!shown) app_str(w, 8, y, "NO INTERRUPTS COUNTED YET", 0xffb020);
    } else {
        app_str(w, 8, y, "TID", 0x7c8ca0);
        app_str(w, 48, y, "NAME", 0x7c8ca0);
        app_str(w, 200, y, "STATE", 0x7c8ca0);
        app_str(w, 296, y, "RING-3 PID", 0x7c8ca0);
        y += 16;
        for (unsigned i = 0; i < s->nthread && i < HW_MAX_THREAD; i++) {
            const struct outrun_thread_stat *th = &s->thread[i];
            u32 c = th->state == 2 ? 0x3df5c4 : th->state == 3 ? 0xffb020 : w->fg;
            app_u32(w, 8, y, th->tid, c);
            app_str(w, 48, y, th->name[0] ? th->name : "(unnamed)", c);
            app_str(w, 200, y, trace_thread_state(th->state), c);
            if (th->uthread) app_u32(w, 296, y, (u32)th->pid, 0x22e4ff);
            else app_str(w, 296, y, "KERNEL", 0x7c8ca0);
            y += 12;
        }
    }
    app_str(w, 8, w->ch - 26, t->samples < 2 ? "SAMPLING: RATES APPEAR AFTER TWO SAMPLES"
                                             : "RATES ARE PER SAMPLE INTERVAL, NOT PER SECOND",
            0xffb020);
    app_str(w, 8, w->ch - 12, "TAB CYCLES VIEWS  /  ALL FIGURES ARE KERNEL COUNTERS", 0x7c8ca0);
    app_present(w);
}

void _start(void) {
    struct app_win w;
    if (app_create(&w, 430, 440, 0x22e4ff)) app_exit(1);
    app_title(&w, "OUTRUN TRACE");
    trace_reset(&state);
    u64 last = 0;
    for (;;) {
        int dirty = 0;
        i64 rc = (i64)sysc(SYS_HW_INFO, HW_TRACE, (u64)&sample, sizeof sample);
        if (rc < 0 || sample.version != OUTRUN_HW_ABI_VERSION) app_exit(2);
        if (!last || sample.wall_ns - last >= 500000000ull) {
            trace_sample(&state, &sample);
            last = sample.wall_ns;
            dirty = 1;
        }
        struct outrun_event e;
        int prc;
        while ((prc = app_poll(&w, &e)) > 0) {
            if (e.type == EVENT_MOUSE_DOWN && e.y < 22) { trace_set_tab(&state, e.x / 78); dirty = 1; }
            if (e.type == EVENT_KEY_PRESS) {
                if (e.code == '\t') trace_set_tab(&state, (state.tab + 1) % TRACE_NTAB);
                if (e.code >= '1' && e.code <= '4') trace_set_tab(&state, e.code - '1');
                dirty = 1;
            }
        }
        if (prc < 0) app_exit(0);
        if (dirty) trace_render(&w, &state);
        app_idle();
    }
}
#endif
