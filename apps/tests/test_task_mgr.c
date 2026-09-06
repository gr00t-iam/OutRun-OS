#define APP_HOST_TEST
#include "../task_mgr.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void test_measured_deltas(void) {
    struct monitor_state m = {0};
    struct outrun_desktop_info a = {.ncpu=2,.nproc=1,.wall_ns=1000000000,.frames_used=10,.frames_total=100};
    a.proc[0].pid = 42; a.proc[0].cpu_ns = 100000000;
    monitor_sample(&m, &a);
    assert(m.cpu == 0 && m.ram == 10);
    a.wall_ns += 1000000000; a.proc[0].cpu_ns += 1000000000;
    monitor_sample(&m, &a);
    assert(m.cpu == 50 && m.ram == 10);
    a.wall_ns += 1000000000; a.proc[0].pid = 43;
    monitor_sample(&m, &a);
    assert(m.cpu == 0); /* recycled list row is not a CPU delta */
    a.frames_total = 0; monitor_sample(&m, &a); assert(m.ram == 0);
    a.frames_used = 1000; a.frames_total = 1;
    monitor_sample(&m, &a); assert(m.ram == 100);
}
/* The sort permutes ROW ORDER only. It must produce an index table over the
 * kernel's list rather than reorder the list itself, because monitor_sample
 * matches processes by pid across samples and must keep seeing the kernel's
 * own ordering. The ABI has no per-process memory field, so there is no memory
 * key: sorting by a column the kernel does not report would be a fabrication. */
static void test_sort_by_pid_ascending(void) {
    struct monitor_state m = {0};
    struct outrun_desktop_info a = {.ncpu=1,.nproc=4,.wall_ns=1000000000,.frames_total=100};
    a.proc[0].pid = 30; a.proc[1].pid = 10; a.proc[2].pid = 40; a.proc[3].pid = 20;
    monitor_sample(&m, &a);
    u32 order[12];
    monitor_order(&m, SORT_PID, order);
    assert(order[0] == 1 && order[1] == 3 && order[2] == 0 && order[3] == 2);
}
static void test_sort_by_cpu_descending_is_stable(void) {
    struct monitor_state m = {0};
    struct outrun_desktop_info a = {.ncpu=1,.nproc=4,.wall_ns=1000000000,.frames_total=100};
    for (u32 i = 0; i < 4; ++i) a.proc[i].pid = 100 + i;
    monitor_sample(&m, &a);
    a.wall_ns += 1000000000;
    a.proc[0].cpu_ns += 100; a.proc[1].cpu_ns += 500;
    a.proc[2].cpu_ns += 100; a.proc[3].cpu_ns += 900;
    monitor_sample(&m, &a);
    u32 order[12];
    monitor_order(&m, SORT_CPU, order);
    assert(order[0] == 3 && order[1] == 1);
    /* Equal deltas keep the kernel's order: row 0 before row 2. */
    assert(order[2] == 0 && order[3] == 2);
    /* The per-row delta the sort used must be visible to the caller too. */
    assert(m.delta[3] == 900 && m.delta[0] == 100);
}
static void test_sort_does_not_disturb_cpu_measurement(void) {
    struct monitor_state m = {0};
    struct outrun_desktop_info a = {.ncpu=2,.nproc=2,.wall_ns=1000000000,.frames_total=100};
    a.proc[0].pid = 7; a.proc[1].pid = 3;
    monitor_sample(&m, &a);
    u32 order[12];
    monitor_order(&m, SORT_PID, order);
    assert(order[0] == 1);
    a.wall_ns += 1000000000; a.proc[0].cpu_ns += 1000000000;
    monitor_sample(&m, &a);
    assert(m.cpu == 50); /* the sort changed row order; the delta did not move */
    monitor_order(&m, SORT_CPU, order);
    assert(order[0] == 0 && order[1] == 1);
}
static void test_sort_of_empty_and_full_lists(void) {
    struct monitor_state m = {0};
    struct outrun_desktop_info a = {.ncpu=1,.nproc=0,.wall_ns=1000000000,.frames_total=100};
    u32 order[12];
    monitor_sample(&m, &a);
    monitor_order(&m, SORT_PID, order); /* must not touch order[] at all */
    a.nproc = 12;
    for (u32 i = 0; i < 12; ++i) a.proc[i].pid = 12 - i;
    a.wall_ns += 1000000000;
    monitor_sample(&m, &a);
    monitor_order(&m, SORT_PID, order);
    for (u32 i = 0; i < 12; ++i) assert(order[i] == 11 - i);
}
/* Bars scale to the observed peak, not to a fixed 0-100. A floor stops an idle
 * graph being amplified into noise; a ceiling stops a full-scale value from
 * drawing past the plot. */
static void test_graph_scale_all_zero(void) {
    u8 hist[HISTORY] = {0};
    int h[HISTORY];
    graph_heights(hist, 5, h, 50);
    for (int i = 0; i < 5; ++i) assert(h[i] == 0);
}
static void test_graph_scale_single_sample(void) {
    u8 hist[HISTORY] = {0};
    int h[HISTORY];
    hist[0] = 40;
    graph_heights(hist, 1, h, 50);
    /* Peak 40 is above the floor of 20, so 40 fills the plot. */
    assert(h[0] == 50);
}
static void test_graph_scale_floor(void) {
    u8 hist[HISTORY] = {0};
    int h[HISTORY];
    hist[0] = 5; hist[1] = 10;
    graph_heights(hist, 2, h, 50);
    /* Peak 10 is under the floor of 20: scale as if the peak were 20. */
    assert(h[0] == 12 && h[1] == 25);
}
static void test_graph_scale_clamps_to_plot(void) {
    u8 hist[HISTORY];
    int h[HISTORY];
    for (int i = 0; i < HISTORY; ++i) hist[i] = 100;
    graph_heights(hist, HISTORY, h, 50);
    for (int i = 0; i < HISTORY; ++i) assert(h[i] == 50);
}
static void test_graph_scale_peak_at_wrap(void) {
    u8 hist[HISTORY] = {0};
    int h[HISTORY];
    /* count = HISTORY + 3: the ring has wrapped and the oldest visible sample
     * lives at slot 3, the newest at slot 2. Put the peak at slot 2 so the
     * scan must reach the LAST visible sample to see it. */
    hist[2] = 80; hist[3] = 40;
    graph_heights(hist, HISTORY + 3, h, 50);
    assert(h[0] == 25);            /* slot 3, oldest visible: 40 of peak 80 */
    assert(h[HISTORY - 1] == 50);  /* slot 2, newest: the peak */
}
int main(void) {
    test_measured_deltas();
    test_sort_by_pid_ascending();
    test_sort_by_cpu_descending_is_stable();
    test_sort_does_not_disturb_cpu_measurement();
    test_sort_of_empty_and_full_lists();
    test_graph_scale_all_zero();
    test_graph_scale_single_sample();
    test_graph_scale_floor();
    test_graph_scale_clamps_to_plot();
    test_graph_scale_peak_at_wrap();
    puts("task_mgr: measured deltas, PID reuse, memory bounds, sorting and graph scaling PASS");
}
