#include "gui.h"
#define HISTORY 100
/* Sort keys. There is deliberately no memory key: outrun_process carries no
 * per-process memory figure, and a column the kernel does not report would be
 * decoration presented as measurement. */
enum { SORT_PID = 0, SORT_CPU = 1 };
struct monitor_state {
    struct outrun_desktop_info previous;
    u32 cpu, ram, count;
    /* Per-row cpu_ns delta from the last sample, matched by pid; 0 when the
     * row is new or the pid was recycled. This is what SORT_CPU orders by. */
    u64 delta[12];
    u8 cpu_history[HISTORY], ram_history[HISTORY];
};
static void monitor_sample(struct monitor_state *m, const struct outrun_desktop_info *s) {
    u64 delta = 0, elapsed = s->wall_ns - m->previous.wall_ns;
    m->cpu = 0;
    for (u32 i = 0; i < 12; ++i) m->delta[i] = 0;
    if (m->previous.wall_ns && s->wall_ns > m->previous.wall_ns && s->ncpu) {
        for (u32 i = 0; i < s->nproc && i < 12; ++i)
            for (u32 j = 0; j < m->previous.nproc && j < 12; ++j)
                if (s->proc[i].pid == m->previous.proc[j].pid &&
                    s->proc[i].cpu_ns >= m->previous.proc[j].cpu_ns) {
                    m->delta[i] = s->proc[i].cpu_ns - m->previous.proc[j].cpu_ns;
                    delta += m->delta[i];
                    break;
                }
        /* ns -> us before multiplying avoids overflow on long uptime. */
        u64 capacity = (elapsed / 1000) * s->ncpu;
        u64 used = delta / 1000;
        if (capacity) m->cpu = used >= capacity ? 100 : (u32)(used * 100 / capacity);
    }
    m->ram = !s->frames_total ? 0 : s->frames_used >= s->frames_total ? 100 :
             (u32)(s->frames_used * 100 / s->frames_total);
    m->cpu_history[m->count % HISTORY] = (u8)m->cpu;
    m->ram_history[m->count % HISTORY] = (u8)m->ram;
    m->count++;
    m->previous = *s;
}
/* Fill order[] with row indices into m->previous.proc, sorted by key. The
 * list itself is never reordered: monitor_sample matches pids across samples
 * against the kernel's own row order, and permuting it would be the same
 * mistake as sorting a table by one column only. Insertion sort on at most 12
 * rows, and stable, so equal keys keep the kernel's order. */
static void monitor_order(const struct monitor_state *m, int key, u32 order[12]) {
    u32 n = m->previous.nproc < 12 ? m->previous.nproc : 12;
    for (u32 i = 0; i < n; ++i) {
        u32 row = i, j = i;
        while (j > 0) {
            u32 prev = order[j - 1];
            int after;
            if (key == SORT_CPU) after = m->delta[prev] >= m->delta[row];
            else after = m->previous.proc[prev].pid <= m->previous.proc[row].pid;
            if (after) break;
            order[j] = prev; --j;
        }
        order[j] = row;
    }
}
/* Bar heights for the visible window of a history ring, scaled so the
 * observed peak fills `plot` pixels. GRAPH_FLOOR stops an idle graph being
 * stretched into noise: below it, values are scaled as if the peak were the
 * floor. Nothing can exceed `plot`, because nothing exceeds the peak. */
#define GRAPH_FLOOR 20
static void graph_heights(const u8 *values, u32 count, int *out, int plot) {
    u32 n = count < HISTORY ? count : HISTORY;
    u32 first = count - n;
    u32 peak = GRAPH_FLOOR;
    for (u32 i = 0; i < n; ++i)
        if (values[(first + i) % HISTORY] > peak) peak = values[(first + i) % HISTORY];
    for (u32 i = 0; i < n; ++i)
        out[i] = (int)((u32)values[(first + i) % HISTORY] * (u32)plot / peak);
}
#ifndef APP_HOST_TEST
static void graph(struct app_win *w, const u8 *values, u32 count, int y, u32 color) {
    int heights[HISTORY];
    app_rect(w, 8, y, HISTORY * 4, 54, 0x121722);
    u32 n = count < HISTORY ? count : HISTORY;
    graph_heights(values, count, heights, 50);
    for (u32 i = 0; i < n; ++i)
        app_rect(w, 8 + (int)i * 4, y + 51 - heights[i], 3, heights[i] + 1, color);
}
/* Rows are drawn in order[] sequence, and the click handler maps a row back
 * through the SAME table, so what is clicked is what was seen. */
static void monitor_render(struct app_win *w, struct monitor_state *m, const u32 *order,
                           int sort, u64 selected, const char *status) {
    struct outrun_desktop_info *s = &m->previous;
    app_fill(w, w->bg);
    app_str(w, 8, 8, "SYS-DIAG  /  MEASURED APP CPU", 0x22e4ff);
    app_u32(w, 320, 8, m->cpu, w->fg); app_str(w, 352, 8, "%", w->fg);
    graph(w, m->cpu_history, m->count, 22, 0x22e4ff);
    app_str(w, 8, 84, "ALLOCATOR RAM", 0x3df5c4);
    app_u32(w, 160, 84, m->ram, w->fg); app_str(w, 192, 84, "%", w->fg);
    graph(w, m->ram_history, m->count, 98, 0x3df5c4);
    app_str(w, 8, 160, "PID", sort == SORT_PID ? 0x22e4ff : 0x7c8ca0);
    app_str(w, 80, 160, "PROCESS", 0x7c8ca0);
    app_str(w, 232, 160, "CPU", sort == SORT_CPU ? 0x22e4ff : 0x7c8ca0);
    app_str(w, 300, 160, "STATE", 0x7c8ca0);
    u64 total = 0;
    for (u32 i = 0; i < 12; ++i) total += m->delta[i];
    for (u32 i = 0; i < s->nproc && i < 12; ++i) {
        int y = 175 + (int)i * 14;
        struct outrun_process *p = &s->proc[order[i]];
        if (p->pid == selected) app_rect(w, 4, y-2, w->cw-8, 14, 0x25374b);
        app_u32(w, 8, y, (u32)p->pid, w->fg);
        app_str(w, 80, y, p->name, w->fg);
        /* This row's share of the measured app CPU, as a whole percentage. */
        app_u32(w, 232, y, total ? (u32)(m->delta[order[i]] * 100 / total) : 0, w->fg);
        app_str(w, 300, y, p->flags ? "EXITED" : "LIVE", 0x3df5c4);
    }
    app_rect(w, 8, 350, 144, 24, 0x7b2345);
    app_str(w, 16, 358, "TERMINATE", w->fg);
    app_str(w, 8, 383, status, 0xffb020);
    app_str(w, 8, 397, "CLICK PID/CPU TO SORT  GRAPH SCALES TO PEAK", 0x7c8ca0);
    app_present(w);
}
void _start(void) {
    struct app_win w;
    static struct monitor_state m;
    static struct outrun_desktop_info info;
    u32 order[12];
    int sort = SORT_PID;
    u64 selected = 0, armed = 0, last = 0;
    const char *status = "SELECT PID THEN TERMINATE TWICE";
    if (app_create(&w, 430, 440, 0x22e4ff)) app_exit(1);
    app_title(&w, "SYS-DIAG");
    for (;;) {
        int dirty = 0;
        i64 rc = (i64)sysc(SYS_DESKTOP_INFO, (u64)&info, sizeof info, 0);
        if (rc < 0 || info.version != OUTRUN_DESKTOP_ABI_VERSION || info.nproc > 12) app_exit(2);
        if (!last || info.wall_ns - last >= 500000000ull) {
            monitor_sample(&m, &info); last = info.wall_ns; dirty = 1;
        }
        struct outrun_event e;
        while ((rc = app_poll(&w, &e)) > 0) {
            if (e.type != EVENT_MOUSE_DOWN) continue;
            if (e.y >= 158 && e.y < 173) {
                if (e.x >= 8 && e.x < 72) { sort = SORT_PID; dirty = 1; }
                if (e.x >= 232 && e.x < 296) { sort = SORT_CPU; dirty = 1; }
            } else if (e.y >= 173 && e.y < 343) {
                u32 row = (u32)(e.y - 173) / 14;
                if (row < m.previous.nproc && row < 12) {
                    monitor_order(&m, sort, order);
                    selected = m.previous.proc[order[row]].pid; armed = 0; dirty = 1;
                    status = "CLICK TERMINATE TO ARM";
                }
            } else if (e.x >= 8 && e.x < 152 && e.y >= 350 && e.y < 374 && selected) {
                dirty = 1;
                if (selected == sysc(SYS_GETPID, 0, 0, 0)) status = "USE WINDOW CLOSE FOR SYS-DIAG";
                else if (armed != selected) { armed = selected; status = "CLICK AGAIN TO SEND SIGKILL"; }
                else {
                    i64 killed = (i64)sysc(SYS_KILL, selected, 9, 0);
                    status = killed < 0 ? "KILL DENIED OR PID NO LONGER LIVE" : "SIGKILL SENT";
                    armed = 0;
                }
            }
        }
        if (rc < 0) app_exit(0);
        if (dirty) {
            monitor_order(&m, sort, order);
            monitor_render(&w, &m, order, sort, selected, status);
        }
        app_idle();
    }
}
#endif
