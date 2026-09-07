/* Host tests for the system trace's sampling core.
 *
 * Every number this application shows is a DELTA between two samples of
 * SYS_HW_INFO's HW_TRACE domain, and deltas are where monitors go wrong. The
 * cases pinned here are the ones that produce a plausible-looking wrong answer
 * rather than an obvious one:
 *
 *   - the FIRST sample, which has nothing to subtract from and must show
 *     nothing rather than showing uptime as though it were load;
 *   - two samples with the same wall clock, which would divide by zero;
 *   - a counter that appears to go backwards, which must floor at zero rather
 *     than wrapping into an enormous rate;
 *   - a core that is offline, which is not a core running at 0%.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define APP_HOST_TEST
#include "sys_trace.c"

static struct trace_state st;
static struct outrun_trace_info s;

static void base(void) {
    memset(&s, 0, sizeof s);
    s.version = OUTRUN_HW_ABI_VERSION;
    s.size = (unsigned)sizeof s;
    s.ncpu = 4;
    s.nirq = HW_MAX_IRQ;
    s.wall_ns = 1000000000ull;              /* 1 s of uptime                   */
    s.frames_total = 1000;
    s.frames_used = 250;
    for (int c = 0; c < 4; c++) s.cpu_online[c] = 1;
}

int main(void) {
    /* ---- percentages ----------------------------------------------------- */
    assert(trace_pct(0, 0) == 0);
    assert(trace_pct(7, 0) == 0);
    assert(trace_pct(1, 2) == 50);
    assert(trace_pct(3, 2) == 100);                 /* saturates              */
    assert(trace_pct(1ull << 62, 1ull << 63) == 50);/* no overflow            */

    /* ---- the first sample has no deltas ---------------------------------- */
    trace_reset(&st);
    base();
    s.cpu_busy_ns[0] = 900000000ull;                /* 0.9 s of accrued time  */
    trace_sample(&st, &s);
    assert(st.samples == 1);
    for (int c = 0; c < 4; c++) assert(st.cpu_pct[c] == 0);
    assert(st.mem_pct == 25);                       /* a level, not a delta   */
    assert(trace_mean_load(&st) == 0);

    /* ---- a second sample measures the interval, not the lifetime ---------- */
    s.wall_ns += 1000000000ull;                     /* one more second        */
    s.cpu_busy_ns[0] += 500000000ull;               /* half of it busy        */
    s.cpu_busy_ns[1] += 1000000000ull;              /* fully busy             */
    s.cpu_busy_ns[2] += 0;                          /* idle                   */
    s.cpu_excursions[0] += 12;
    s.frames_used = 500;
    trace_sample(&st, &s);
    assert(st.cpu_pct[0] == 50);
    assert(st.cpu_pct[1] == 100);
    assert(st.cpu_pct[2] == 0);
    assert(st.excur_delta[0] == 12);
    assert(st.mem_pct == 50);
    assert(trace_mean_load(&st) == (50 + 100 + 0 + 0) / 4);

    /* A core busier than wall-clock time cannot happen, and if the accounting
     * ever said so the display must saturate rather than print 300%. */
    s.wall_ns += 1000000000ull;
    s.cpu_busy_ns[3] += 3000000000ull;
    trace_sample(&st, &s);
    assert(st.cpu_pct[3] == 100);

    /* ---- an offline core is not a 0% core -------------------------------- */
    s.cpu_online[2] = 0;
    s.wall_ns += 1000000000ull;
    s.cpu_busy_ns[0] += 1000000000ull;
    trace_sample(&st, &s);
    assert(st.cpu_pct[0] == 100);
    assert(trace_core_offline(&st, 2));
    assert(!trace_core_offline(&st, 0));
    /* The mean is over ONLINE cores only: dividing by a core that cannot run
     * anything would make a fully loaded machine look three-quarters idle. */
    assert(trace_mean_load(&st) == (100 + 0 + 0) / 3);

    /* ---- a wall clock that did not advance ------------------------------- */
    unsigned before = st.cpu_pct[0];
    s.cpu_busy_ns[0] += 500000000ull;               /* time charged, no wall   */
    trace_sample(&st, &s);
    assert(st.cpu_pct[0] == before);                /* held, not divided by 0  */

    /* ---- counters that appear to go backwards ---------------------------- */
    s.wall_ns += 1000000000ull;
    s.cpu_busy_ns[0] = 1;                           /* implausible: floor at 0 */
    s.irq[0].total = 0;
    trace_sample(&st, &s);
    assert(st.cpu_pct[0] == 0);
    assert(st.irq_delta[0] == 0);

    /* ---- interrupt distribution ------------------------------------------ */
    trace_reset(&st);
    base();
    s.irq[0].total = 100;   s.irq[0].percpu[0] = 100;
    s.irq[1].total = 5;     s.irq[1].percpu[0] = 5;
    s.irq[11].total = 40;   s.irq[11].percpu[0] = 30; s.irq[11].percpu[1] = 10;
    trace_sample(&st, &s);
    assert(st.irq_delta[0] == 0);                   /* first sample: no rate   */
    s.wall_ns += 1000000000ull;
    s.irq[0].total = 200;   s.irq[0].percpu[0] = 200;
    s.irq[11].total = 55;   s.irq[11].percpu[0] = 40; s.irq[11].percpu[1] = 15;
    trace_sample(&st, &s);
    assert(st.irq_delta[0] == 100);
    assert(st.irq_delta[1] == 0);
    assert(st.irq_delta[11] == 15);
    assert(st.irq_percpu_delta[11][0] == 10 && st.irq_percpu_delta[11][1] == 5);
    assert(st.irq_busiest == 0);                    /* line 0 moved the most   */

    assert(!strcmp(trace_irq_name(0), "PIT TIMER"));
    assert(!strcmp(trace_irq_name(1), "PS/2 KEYBOARD"));
    assert(!strcmp(trace_irq_name(2), "PIC CASCADE"));
    assert(!strcmp(trace_irq_name(12), "PS/2 MOUSE"));
    assert(!strcmp(trace_irq_name(11), "PCI INTX"));
    assert(!strcmp(trace_irq_name(7), "LINE 7"));

    /* ---- thread states are the kernel's own enumeration ------------------- */
    assert(!strcmp(trace_thread_state(0), "FREE"));
    assert(!strcmp(trace_thread_state(1), "RUNNABLE"));
    assert(!strcmp(trace_thread_state(2), "RUNNING"));
    assert(!strcmp(trace_thread_state(3), "BLOCKED"));
    assert(!strcmp(trace_thread_state(4), "CLAIMED"));
    assert(!strcmp(trace_thread_state(9), "UNKNOWN"));

    /* ---- the history ring keeps the most recent window ------------------- */
    trace_reset(&st);
    base();
    for (int i = 0; i < TRACE_HIST + 25; i++) {
        s.wall_ns += 1000000000ull;
        s.cpu_busy_ns[0] += 1000000000ull;          /* core 0 pinned at 100%   */
        s.frames_used = 100;
        trace_sample(&st, &s);
    }
    assert(st.samples == (unsigned)(TRACE_HIST + 25));
    assert(st.load_hist[(st.samples - 1) % TRACE_HIST] == trace_mean_load(&st));
    assert(st.mem_hist[(st.samples - 1) % TRACE_HIST] == 10);

    /* ---- tabs ------------------------------------------------------------ */
    trace_set_tab(&st, TRACE_TAB_IRQ);
    assert(st.tab == TRACE_TAB_IRQ);
    trace_set_tab(&st, 99);
    assert(st.tab == TRACE_TAB_IRQ);                /* refused, not wrapped    */
    trace_set_tab(&st, TRACE_TAB_CPU);
    assert(st.tab == TRACE_TAB_CPU);

    printf("sys_trace: per-core load deltas, offline cores, zero and backwards "
           "intervals, IRQ distribution, thread states and history PASS; "
           "state=%zu bytes\n", sizeof st);
    return 0;
}
