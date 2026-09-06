# Objective 4 — Ring 0/3 CPU accounting (utime / stime split) and ITIMER_PROF

Design document. **No kernel or user-space code has been modified.** This is the
Phase 2 audit and the Phase 3 plan, presented for approval before `kernel64.c`
or `user/init.c` are touched.

Branch `obj4/utime-stime-split`, based on `7e21ae5` (v0.93.0).

---

## Phase 2 — Subsystem audit and boundary mapping

### 2a. The ring-3 excursion, as it actually is

The thing v0.93 measures is not "time in ring 3". It is the **excursion**: the
window in `cpu_exec_proc` (`kernel64.c:15571-15596`) that opens just before
`enter_user_mode` / `enter_user_resume` and closes when one of those returns.

    cpu_t0 = ktime_get_ns()                     <-- window opens
    kprocs[p].cpu_run_excl0 = cpu_excl_ns
    kprocs[p].cpu_run_t0    = cpu_t0
    code = enter_user_resume(...) / enter_user_mode(...)
    kprocs[p].cpu_run_t0 = 0                    <-- window closes
    net = elapsed - slept                       (floored at 0)
    cpu_ns += net ; cpu_grp_ns += net           (leader L)

**A syscall does not close this window.** `boot/usermode.asm:68` is the whole
story: `syscall_entry` parks the user RSP, switches to the thread's kernel
stack, `call syscall_trap`, then `o64 sysret` straight back to ring 3. It never
touches `enter_user_mode`'s return path. So the kernel time spent servicing a
syscall is measured *inside* the open excursion, indistinguishably from the
ring-3 compute either side of it.

That is not a defect — it is exactly why `cpu_ns` correctly reports
`utime+stime`, which is what `CLOCK_PROCESS_CPUTIME_ID` is defined to be. It is
also precisely why there is no split today.

The window closes at only these places, all of which unwind through this core's
per-CPU kernel resume point (`resume_kernel`, `boot/usermode.asm:241`):

| exit | site | mechanism |
|---|---|---|
| `SYS_EXIT` | `kernel64.c:14569` region | `resume_kernel(code)` |
| preemption / LAPIC slice tick at CPL3 | `smp_preempt_ipi`, `kernel64.c:15740-15762` | captures `uctx`, `resume_kernel(RET_PREEMPTED)` |
| voluntary yield / park (`sys_yield_ring3`, `futex_wait`, `thread_join`, `epoll_wait`) | `17358`, `17426`, `17498` | `resume_kernel(RET_PREEMPTED)` |
| CPL3 fault | `handle_cpl3_fault`, `21826` | noreturn, unwinds to the kernel |

`smp_preempt_ipi` already discriminates ring with `(f->cs & 3) != 3` — an
interrupt that catches the task *in the kernel* refuses to preempt and only
bumps `resched`. So the ISR layer can already tell the two rings apart; it just
does not record anything about it.

### 2b. `cpu_run_t0` / `cpu_run_excl0` / `ksleep_charge_to`

- `cpu_run_t0` (`2009`): ktime at dispatch, `0` when the task is not on a CPU.
  It is the *published* half of the window, so a **concurrent observer on
  another core** — `sig_check_alarms` is exactly that — can account for time the
  task has burned since its last fold.
- `cpu_run_excl0` (`2010`): `cpu_excl_ns` sampled at that same instant, so the
  live delta can have its slept time removed the same way the folded total does.
- `cpu_ns_inflight(p)` (`13827`): `(now - cpu_run_t0) - (cpu_excl_ns -
  cpu_run_excl0)`, floored at 0, and `0` when `cpu_run_t0 == 0`.
- `ksleep_charge_to(p, ns)` (`15079`): one atomic add to `cpu_excl_ns`. It takes
  the task **by argument**, not from `current_proc_idx`, because `krelax()`
  yields and moves `cur_proc`. `ksleep_us` charges *incrementally*, every 2000
  us of sleep, specifically so a concurrent observer never sees a sleeping
  task's CPU time racing at wall-clock rate.

Everything above is a *subtractive* model: elapsed minus excluded. That model
is what makes the split cheap, because stime can be measured the same way —
one more published `t0`, one more excluded baseline — instead of needing a
second, independent clock discipline.

### 2c. Where the split has to happen

`syscall_trap` (`kernel64.c:21595`) is **the single C funnel every syscall
passes through**. `syscall_entry` has exactly one `call` and it is this one.
That makes it the only site where "kernel, on behalf of this task, at this
task's request" is unambiguous, and it is where the stime interval must be
bracketed.

Three hazards, all of which the design must answer rather than hope about:

1. **`syscall_trap` does not always return.** `SYS_EXIT`, `sys_sigreturn`,
   `sys_yield_ring3`, the three parking calls, and `posix_sigcheck_on_return`
   itself can all leave without reaching the epilogue. A naive "stamp at top,
   charge at bottom" silently loses the charge for every one of them — and
   `SYS_EXIT` is the last syscall of every process, so the *final* syscall of
   every worker would be uncharged.
2. **Sleeps happen inside syscalls.** `nanosleep` sits inside `syscall_trap`;
   if stime is measured as raw elapsed, a 50 ms sleep becomes 50 ms of system
   time and `stime` grows at wall-clock rate. The same `cpu_excl_ns` baseline
   the excursion uses must be applied to the syscall window.
3. **Hot path cost.** Every syscall would pay for whatever is added here. Any
   atomic or barrier is a per-syscall tax on the whole tree.

Hazard 3 has a clean answer, and it is the one the existing code already uses:
**per-task accumulation during the excursion, one atomic fold at excursion
close.** The in-excursion counters are written only by the core currently
running that task — that is what "the excursion is open" *means* — so plain
64-bit stores are sufficient and no barrier is required. The cost per syscall
becomes two plain stores at entry and one `ktime_get_ns()` plus two plain adds
at exit: **one rdtsc-derived clock read per syscall, no atomics, no barriers.**

### 2d. Two findings that change the stated task

- **`getrusage` does not exist.** There is no `SYS_GETRUSAGE`, no `struct
  rusage`, and no `RUSAGE_*` constant anywhere in the tree (`grep -n
  "getrusage\|RUSAGE" kernel/kernel64.c user/init.c` is empty). Objective 4
  step 2 says "update getrusage"; there is nothing to update. It is a **new
  syscall**, and it is scoped as one below.
- **`CLOCK_PROCESS_CPUTIME_ID` must not change.** POSIX defines it as
  utime+stime and it returns a single scalar `struct timespec`; there is no
  place in its ABI to "return a split breakdown". `clock_read_ns` (`15019`)
  stays behaviourally as it is. The split is exposed through `getrusage`, which
  is the interface that has fields for it. Changing the clock would break the
  one guarantee r63 check (5) currently pins.

---

## Phase 3 — Design

### 3a. `struct kproc` — five new accounting fields

Added next to the existing v0.93 accounting block (`kernel64.c:2016-2010`):

```c
    /* v0.94: THE RING 0/3 SPLIT. cpu_ns above stays exactly what it is — the
     * total, utime+stime — because CLOCK_PROCESS_CPUTIME_ID is defined to be
     * that and nothing here may change it. stime is measured; utime is
     * DERIVED as (total - stime).
     *
     * Deriving one of the two rather than accumulating both independently is
     * the whole correctness argument. Two counters filled from two different
     * brackets drift, and the first symptom is utime+stime disagreeing with
     * cpu_ns — at which point ITIMER_PROF and CLOCK_PROCESS_CPUTIME_ID answer
     * different questions about the same process and neither can be trusted.
     * A derived utime cannot drift from a total it is subtracted from. */
    volatile uint64_t cpu_stime_ns;        /* this task's net kernel time, folded  */
    volatile uint64_t cpu_grp_stime_ns;    /* group kernel total; kept on leader   */
    volatile uint64_t cpu_stime_run;       /* this excursion's kernel ns so far    */
    /* The OPEN SYSCALL, published for the same reason cpu_run_t0 is: an
     * observer on another core (sig_check_alarms) must be able to account for
     * a syscall that has not returned yet. Same subtractive shape — elapsed
     * since entry, minus whatever the task slept inside the call. */
    volatile uint64_t cpu_sys_t0;          /* ktime at syscall_trap entry; 0 = none */
    volatile uint64_t cpu_sys_excl0;       /* cpu_excl_ns at that instant           */
```

`kproc_reset` (`2500`) zeroes all five alongside the existing three.

### 3b. Accessors

```c
/* The live total: folded plus the excursion in flight. Three sites already
 * open-code this expression (13894, 15020, 21424); giving it a name is what
 * keeps the timer scan and the arming path from drifting apart again — they
 * did exactly that between d319e51 and c536dc0. */
static inline uint64_t proc_cpu_live(int p) {
    return proc_cpu_ns(p) + cpu_ns_inflight(p);
}

/* Kernel time burned in the syscall the task is inside RIGHT NOW. Zero when it
 * is not in one. Mirrors cpu_ns_inflight exactly, including the sleep
 * subtraction — without it a nanosleep would be reported as system time. */
static inline uint64_t sys_ns_inflight(int p) {
    uint64_t t0 = kprocs[p].cpu_sys_t0;
    if (!t0) return 0;
    uint64_t now = ktime_get_ns();
    if ((int64_t)(now - t0) <= 0) return 0;          /* raced with entry */
    uint64_t el = now - t0;
    uint64_t sl = kprocs[p].cpu_excl_ns - kprocs[p].cpu_sys_excl0;
    return el > sl ? el - sl : 0;
}

static inline uint64_t proc_stime_ns(int p) {
    return kprocs[tg_of(p)].cpu_grp_stime_ns
         + kprocs[p].cpu_stime_run + sys_ns_inflight(p);
}

/* utime is the REMAINDER, and the order of the two reads is load-bearing.
 *
 * These are two live reads of quantities another core can be advancing, taken
 * microseconds apart, so their difference is not monotonic by construction.
 * Reading the TOTAL first and the stime second means stime can only have grown
 * relative to the total between the reads — so utime can only be UNDER-stated,
 * never over-stated. A virtual timer compared against an under-stated utime
 * fires LATE. It can never fire EARLY, which is the failure that matters and
 * the same discipline this tree already applies to sleeps and to VIRTUAL's
 * arming path. */
static inline uint64_t proc_utime_ns(int p) {
    uint64_t tot = proc_cpu_live(p);
    uint64_t sys = proc_stime_ns(p);
    return tot > sys ? tot - sys : 0;
}
```

Per-thread variants (`thread_stime_ns`, `thread_utime_ns`) use
`kprocs[p].cpu_stime_ns` / `kprocs[p].cpu_ns` in place of the group fields, for
`getrusage(RUSAGE_THREAD)`.

### 3c. The two brackets

**Bracket 1 — `syscall_trap`** (`21595`). The function body is wrapped; the
prologue publishes and the single normal epilogue folds.

```c
uint64_t syscall_trap(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                      struct sysframe *sf) {
    int sp = (int)current_proc_idx;
    /* Two plain stores. Only the core running this task writes these, which is
     * what "inside an excursion" means, so no atomic and no barrier. */
    kprocs[sp].cpu_sys_excl0 = kprocs[sp].cpu_excl_ns;
    kprocs[sp].cpu_sys_t0    = ktime_get_ns();
    ... existing dispatch, unchanged ...
    posix_sigcheck_on_return(sf, r);            /* may never return */
    sys_charge_close(sp);                       /* NEW: one clock read, two adds */
    return r;
}
```

**Bracket 2 — the catch-all.** `sys_charge_close()` is idempotent (it clears
`cpu_sys_t0`, and returns immediately when it is already 0), and is called from
exactly three places:

1. `syscall_trap`'s normal epilogue, above — the common case;
2. immediately before each `enter_user_ctx` call (signal delivery and
   `SYS_SIGRETURN`), which abandons the syscall frame and returns to ring 3
   *without* closing the excursion;
3. **unconditionally in `cpu_exec_proc` right after `cpu_run_t0 = 0`**, before
   `net` is computed.

(3) is the safety net and it is why this design is not fragile. Every
`resume_kernel` unwind — `SYS_EXIT`, preemption, all three parking calls, a
CPL3 fault — lands there. A syscall left open by any path nobody enumerated is
still charged, at the excursion boundary, to the right task. Missing a site
costs a few microseconds of attribution accuracy; it cannot lose a charge and
it cannot corrupt a counter.

```c
static inline void sys_charge_close(int p) {
    uint64_t t0 = kprocs[p].cpu_sys_t0;
    if (!t0) return;
    kprocs[p].cpu_sys_t0 = 0;                       /* clear FIRST: a concurrent
                                                     * sys_ns_inflight must not
                                                     * see the interval twice */
    uint64_t el = ktime_get_ns() - t0;
    uint64_t sl = kprocs[p].cpu_excl_ns - kprocs[p].cpu_sys_excl0;
    kprocs[p].cpu_stime_run += (el > sl) ? (el - sl) : 0;
}
```

**Fold at excursion close** (`cpu_exec_proc`, `15584-15594`), alongside the
existing `net` fold, so the group total takes exactly one atomic per excursion:

```c
    kprocs[p].cpu_run_t0 = 0;
    sys_charge_close(p);                            /* NEW */
    uint64_t elapsed = ktime_get_ns() - cpu_t0;
    uint64_t slept   = kprocs[p].cpu_excl_ns - slept0;
    uint64_t net = (elapsed > slept) ? (elapsed - slept) : 0;
    uint64_t sn  = kprocs[p].cpu_stime_run;
    /* CLAMPED TO THE TOTAL. Both are (elapsed - slept) over nested windows, so
     * sn <= net holds by construction — but they are sampled across a window
     * another core writes into, and a derived utime that could go negative
     * would corrupt a monotonic quantity permanently. Same reason `net` itself
     * is floored rather than trusted. */
    if (sn > net) sn = net;
    kprocs[p].cpu_stime_run = 0;
    if (net) { __sync_fetch_and_add(&kprocs[p].cpu_ns, net);
               __sync_fetch_and_add(&kprocs[L].cpu_grp_ns, net); }
    if (sn)  { __sync_fetch_and_add(&kprocs[p].cpu_stime_ns, sn);
               __sync_fetch_and_add(&kprocs[L].cpu_grp_stime_ns, sn); }
```

`cpu_stime_run` is also zeroed where `cpu_run_t0` is published at dispatch, so
a task that somehow left a residue cannot carry it into the next excursion.

### 3d. What is deliberately NOT measured, and why

**Interrupt and fault time is counted as utime.** Time in the PIT ISR, the
LAPIC slice tick, `smp_preempt_ipi`, and page-fault handling lands inside the
excursion but outside `syscall_trap`, so the subtraction puts it in utime.

That is an approximation and it is stated rather than hidden. Linux would call
it hardirq/softirq time; this kernel has no third bucket, and the alternative
is an `rdtsc` pair in the interrupt entry path — a per-interrupt tax for a
quantity nothing currently asks for. The hook if it is ever wanted is already
half-built: `(f->cs & 3)` is available at ISR entry (`kernel64.c:708`,
`15747`), so the dispatcher can already tell which ring it interrupted.

The bound is small: under TCG the slice quantum is 10 ms and the ISR is a few
microseconds, so this is well under 1% of any window the tests measure. It is
recorded here so that a future reading of "utime" knows what is in it.

### 3e. `ITIMER_VIRTUAL` — change of basis

`sig_check_alarms` (`13878-13903`) and `case 108`'s arming (`21424`) both
currently use `proc_cpu_ns(i) + cpu_ns_inflight(i)`, i.e. the total.

Both become `proc_utime_ns(i)`. Nothing else about the branch changes — the
CAS claim, the `itimer_next` skip-missed-periods loop, the leader-only
(`tg_of(i) == i`) test and the `g_alarms_armed` bookkeeping are all correct as
they stand and are reused verbatim.

Two **stale comments** must be corrected in the same commit, because both now
describe behaviour the code does not have:

- `case 108` (`21414-21423`) still says "sig_check_alarms compares the
  accumulated total alone... so the timer can only fire LATE". That stopped
  being true in `c536dc0`, which moved the scan to the live total. The real
  late-not-early argument is now the read-ordering one in 3b, and the comment
  should say so.
- The `it_virt_ns` field comment (`1993-1995`) says "measured against consumed
  process CPU time"; it becomes user time.

### 3f. `ITIMER_PROF` — new

Two new `kproc` fields, `it_prof_ns` / `it_prof_iv_ns`, and `#define SIGPROF 27`
(`NSIG` is 32 and `sig_pending` is a `uint32_t` bitmask over signals 1..31, so
27 fits with room; 28-31 remain free).

`case 108/109` already resolves `(dlp, ivp, now)` by `which` through a pointer
indirection, so PROF is one branch and one relaxed validity check:

```c
-       if (a0 != 0 && a0 != 1) return (uint64_t)-22;          /* EINVAL */
+       if (a0 > 2) return (uint64_t)-22;                      /* EINVAL */
...
+       /* ITIMER_PROF — user + system. This is the quantity ITIMER_VIRTUAL was
+        * measured against before v0.94, which is the honest way to say that
+        * PROF is not new behaviour so much as the correct home for behaviour
+        * VIRTUAL should never have had. */
+       else if (a0 == 2) { dlp = &kprocs[p].it_prof_ns; ivp = &kprocs[p].it_prof_iv_ns;
+                           now = proc_cpu_live(p); }
```

The arm/disarm `g_alarms_armed` bookkeeping, the `old_rem` reporting and the
`itimerval` marshalling are all expressed in terms of `dlp`/`ivp`/`now` and
therefore need no change at all.

In `sig_check_alarms`, a third block modelled on the VIRTUAL one, leader-only,
evaluated against `proc_cpu_live(i)`, raising `SIGPROF`. Note that
`g_alarms_armed` now counts up to three timers per process; it is a count of
armed timers rather than of processes, so that is already correct — and the
existing `if (old_dl) __sync_fetch_and_sub(...)` already prevents double-count
per timer.

### 3g. `getrusage` — a new syscall, honestly scoped

`SYS_GETRUSAGE = 110` (109 is the highest in use; `user/init.c:176-177`).

```c
case 110: {  /* SYS_GETRUSAGE(who, struct orusage *out) -> 0, or negative errno */
```

`who`: `RUSAGE_SELF` (0) → group totals; `RUSAGE_THREAD` (1) → this task alone.
`RUSAGE_CHILDREN` (-1) is **rejected with `-EINVAL`**, not answered with zeroes:
this kernel does not accumulate a reaped child's CPU time into its parent, and
returning a well-formed zero would be a measurement nobody made.

The struct is deliberately **not** POSIX's full `struct rusage`:

```c
/* struct orusage — 32 bytes, four fields, all of them measured.
 *
 * POSIX's struct rusage has sixteen members and this kernel can honestly fill
 * two of them. Emitting the full struct with fourteen zeroes would hand a
 * caller ru_maxrss = 0 and ru_nvcsw = 0 as though they had been counted. A
 * smaller struct that is entirely real is worth more than a familiar one that
 * is mostly fiction; the name differs from `rusage` so nobody mistakes it for
 * the standard layout. */
struct orusage { uint64_t utime_sec, utime_usec, stime_sec, stime_usec; };
```

The invariant `utime + stime == CLOCK_PROCESS_CPUTIME_ID` holds by construction
for `RUSAGE_SELF`, because utime is derived by subtraction from the very value
that clock returns. That is a testable claim and 3h tests it.

### 3h. Validation — new ring-3 role 64

**Role 64, not an extension of 63.** Checked per the CLAUDE.md rule before
claiming it: `grep -n '\.role = ' kernel/kernel64.c` and `grep -n 'role == '
user/init.c` agree that 63 is the highest assigned and 64 is free. Role 63 is
already a long probe with a 6000-tick watchdog and a full exit-code namespace
(1870-1891); loading two more scenarios onto it risks a `TRUNCATED` that reads
as a defect.

Exit codes 1900-1915, success sentinel **1900**, deadline expiry on its own
code (**1915**) so a slow host is never decoded as a defect. Deadline constant
`R64_T` in `user/init.c` beside `R62_T`/`R63_T`, expressed in ticks via
`owaitpid_ticks()` / `SYS_SYSINFO`, never as an iteration count.

**Scenario (a) — ring-3 compute.** A register-only spin loop that reads the
clock only once every 100,000 iterations, so syscall cost is a small fraction
of the window rather than the window itself.

- assert `d_utime > 0` — utime advances;
- assert `d_stime < d_total / 5` — stime is a *small fraction*. **Not** `== 0`:
  the loop must read a clock to have a deadline at all, so an equality
  assertion would be measuring the sampling rate, not the split. A bound that
  can only pass with a working split is the requirement; an assertion that
  cannot pass at all is not an improvement on one that always does.
- arm `ITIMER_VIRTUAL` and `ITIMER_PROF` for the same amount; **both** must
  expire, and neither earlier than armed for.

**Scenario (b) — syscall storm.** A tight `getpid` loop (the cheapest syscall in
the tree, so the ratio reflects entry/exit cost rather than one handler).

- assert `d_stime > d_total * 3 / 10` — system time is now a large fraction;
- assert `d_utime` still advances (the loop overhead is real ring-3 work);
- **the discriminating assertion:** arm both timers for the same amount and
  require `ITIMER_PROF` to expire while `ITIMER_VIRTUAL` has *not*. Without
  this, both timers could be wired to the same quantity and every other check
  in both scenarios would still pass. This is the single assertion that proves
  the split exists rather than that two names were added.

**Scenario (c) — the invariant.** `getrusage(RUSAGE_SELF)` against
`clock_gettime(CLOCK_PROCESS_CPUTIME_ID)`, read adjacently: `utime + stime`
must equal the clock to within one sampling window. Catches a units error and
catches a double-count, neither of which (a) or (b) can see.

**Detections, not just failures.** Per CLAUDE.md, the kernel-side decode prints
a *detection* count — `prof_fires` and `virt_fires`, raised in the handlers —
separately from the failure codes. `prof_fires > 0` is what proves the boot
reached the code at all; a suite asserting only "no failures" is green on a
workload that never armed a timer. The existing `[timebench] r63 accounting:`
diagnostic gains two new lines in the same format:

    [timebench] r64 accounting: cpu_ns=%u stime=%u utime=%u excl=%u (us)
    [timebench] r64 timers: prof_fires=%d virt_fires=%d it_prof=%u it_virt=%u

**The falsifiability check, before any of this is believed.** Per the standing
rule that a test which cannot fail has not passed: build with the split
reverted — `sys_charge_close` made a no-op, so `stime == 0` and
`utime == cpu_ns` — and confirm scenario (b) **fails** on the `d_stime`
assertion and on the PROF-vs-VIRTUAL discriminator. If it passes against that
build, the test is measuring nothing and the result is void.

---

## Diff surface — the complete list of sites

| # | file:line | change |
|---|---|---|
| 1 | `kernel64.c:1829` | `#define SIGPROF 27` |
| 2 | `kernel64.c:1993-1995` | `it_virt_ns` comment: process CPU -> user time |
| 3 | `kernel64.c:~1996` | new `it_prof_ns`, `it_prof_iv_ns` |
| 4 | `kernel64.c:2016-2010` | five new accounting fields + rationale comment |
| 5 | `kernel64.c:2500` | `kproc_reset` zeroes all seven new fields |
| 6 | `kernel64.c:13818` | `proc_cpu_live`, `sys_ns_inflight`, `proc_stime_ns`, `proc_utime_ns`, `sys_charge_close` |
| 7 | `kernel64.c:13894` | VIRTUAL scan -> `proc_utime_ns(i)` |
| 8 | `kernel64.c:~13903` | new PROF scan block -> `proc_cpu_live(i)`, `SIGPROF` |
| 9 | `kernel64.c:15020-15022` | `clock_read_ns` — behaviour **unchanged**, uses `proc_cpu_live` for readability only |
| 10 | `kernel64.c:15584` | `sys_charge_close(p)` + stime fold at excursion close |
| 11 | `kernel64.c:15577` | zero `cpu_stime_run` at dispatch |
| 12 | `kernel64.c:~14440,~15540` | `sys_charge_close` before each `enter_user_ctx` |
| 13 | `kernel64.c:21399-21424` | `case 108/109`: accept `which == 2`, PROF branch, VIRTUAL -> utime, stale comment corrected |
| 14 | `kernel64.c:~21590` | new `case 110` SYS_GETRUSAGE |
| 15 | `kernel64.c:21597` | `syscall_trap` prologue/epilogue bracket |
| 16 | `kernel64.c:23651-23700` | role 64 spawn, accounting diagnostic, exit-code decode |
| 17 | `kernel64.c:27056` | SDK `signal.h`: `SIGPROF`, `ITIMER_PROF`; `sys/resource.h` stub |
| 18 | `user/init.c:176` | `SYS_GETRUSAGE 110`, `RUSAGE_SELF/THREAD` |
| 19 | `user/init.c:197` | `ITIMER_PROF 2` |
| 20 | `user/init.c:323` | `SIGPROF 27` |
| 21 | `user/init.c:1837` | `ogetrusage()` wrapper |
| 22 | `user/init.c:~3900` | `role64_cpusplit_probe()`, `R64_T` |
| 23 | `user/init.c:6663` | dispatch `if (role == 64)` |

**Hot-path cost, stated for the record:** two plain 64-bit stores on syscall
entry, one `ktime_get_ns()` and two plain adds on syscall exit. No atomic, no
barrier, no lock. The two `__sync_fetch_and_add`s for the group total run once
per *excursion*, not once per syscall — the same batching `cpu_ns` already uses.

---

## AS BUILT — Phase 4 results

Implemented on branch `obj4/utime-stime-split`. Field names differ slightly from
the plan above: `cpu_sys_t0` / `cpu_sys_excl0` shipped as `sys_enter_ns` /
`sys_enter_excl0`.

### Measurements (uniprocessor, role 64, image `84e371fd…`)

| window | utime | stime | total | stime share |
|---|---|---|---|---|
| ring-3 compute | 59,787 us | 328 us | 60,102 us | **0.5%** |
| syscall storm (`getpid`) | 57,808 us | 182,274 us | 240,082 us | **75.9%** |

The invariant held to **2 us**: `utime + stime = 60,638` against
`CLOCK_PROCESS_CPUTIME_ID = 60,640`.

The discriminating check separated cleanly: in the storm, with both timers armed
on the same 30 ms interval at the same instant, **ITIMER_PROF fired 9 times and
ITIMER_VIRTUAL once.**

### The negative control, which is what makes the above mean anything

Built with `sys_charge_close` stubbed to return immediately — the split reverted,
nothing ever charged to stime — image `3800ae9a…`, everything else identical.

    [r64] storm 4674 rounds: utime +240,034  stime +0  total +240,036 us
    [r64] storm timers: PROF fired 9  VIRTUAL fired 9
    [r64] stime did not advance across a syscall storm
    ring-3 cpu-split probe: stime DID NOT ADVANCE across a syscall storm (exit 1910)

Role 64 **FAILED**, at 1910. Note the line above it: PROF and VIRTUAL fired an
identical nine times, which is exactly the condition 1913 exists to catch — so
both discriminators were live against the broken build, and 1910 simply reached
its check first. Role 63 stayed OK (1870) on the same image, confirming the
control disabled the split and nothing else.

### Verification performed

| tier | image | result |
|---|---|---|
| `uniprocessor` gate | `0d3895ce…` | PASS — 45 suites, 563 passed, 0 failed, 0 ranks, 485 s |
| `smp4-bios` gate | `039a73c3…` | PASS — 45 suites, 583 passed, 0 failed, 0 ranks, 440 s |
| `smp4-iommu` gate (q35 + VT-d, `intremap=on`) | `039a73c3…` | PASS — 47 suites, 597 passed, 0 failed, 0 ranks, 445 s |
| `probe-timebench`, roles 63 / 64 | `84e371fd…` | OK (1870) / OK (1900) |
| `probe-timebench`, roles 63 / 64 | `468359ad…` (v0.94.0) | OK (1870) / OK (1900) |
| `probe-timebench -smp 4`, roles 63 / 64 | `468359ad…` (v0.94.0) | OK (1870) / OK (1900) |
| negative control, role 64 | `3800ae9a…` | **FAIL (1910)** — as required |

Every build emitted zero compiler warnings.

### The lockless read, under four cores

3b argues that `proc_utime_ns` is safe without a lock because its two reads are
ordered so the difference can only be UNDER-stated, making a virtual timer fire
late and never early. That is a claim about concurrency, and only an SMP tier
can exercise it: the probe runs with `affinity = 0` (unrestricted — see
`kernel64.c:1948`), so under `-smp 4` it migrates between cores while
`sig_check_alarms` reads its counters from a different one.

| window, `-smp 4` | utime | stime | total | stime share |
|---|---|---|---|---|
| ring-3 compute | 62,051 us | 257 us | 62,295 us | 0.4% |
| syscall storm | 58,169 us | 181,868 us | 240,038 us | 75.8% |

Invariant held to **2 us** (62,928 against 62,930). PROF fired 9 times, VIRTUAL
once. **No 1914** (a CPU total went backwards — the underflow detector) and **no
1908** (PROF fired early) on any run. The figures sit within noise of the
uniprocessor run, which is the outcome the read-ordering argument predicts.

Stated honestly: this is one boot. It shows no underflow and no early firing;
it does not prove none exists at a rate below one boot in one.

### What this does NOT cover, stated so the gaps are visible

- **`smp2-bios`, `smp8-bios`, `gate-dirty` and `gate-dirty-smp` were not run.**
  The dirty tiers were last run at v0.90.0 and persisted-state reuse across
  boots is therefore unverified for four cycles now.
- **One boot per configuration**, so nothing below roughly 1 in 10 boots is
  visible.
- **`make gate` still does not reach role 64.** `gate-matrix.sh` boots QEMU with
  stdin on `/dev/null` — deliberately, since a gate must not depend on anything
  being typed. `make probe-timebench` (added this cycle) is the target that
  drives it over a FIFO, and it is a separate command: role 63 and role 64 are
  covered only when someone runs it.
- **No release ISO was built or `release-verify`'d.** `VERSION`,
  `KERNEL_VERSION` and the GRUB menuentry now read `0.94.0`, but no tag exists
  and the four-step release protocol has not been performed.
- Interrupt and fault time is billed as utime, by design — see 3d.

### A note on ISO checksums

`grub-mkrescue` is not reproducible: two builds of identical source produced ISO
md5s `5186382602ee394e60da56fd469f81c6` and `039a73c3267f0eddd5f65fd61815fbae`,
while `build/outrun-kernel.elf` and `build/user_init.elf` were byte-identical
across both (`98edd859…`, `211c0f6e…`). So an ISO md5 identifies a *build*, not a
source state, and a rebuild of the tested tree will not reproduce `84e371fd…`.
Where a claim needs to be tied to a source state rather than to an artefact, the
ELF checksums are the ones that carry it.
