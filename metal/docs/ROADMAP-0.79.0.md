# OutRun OS v0.79.0-metal — roadmap

Milestone 79. v0.78.0 is tagged (`560a831`). It closed carryover 3 — open since
v0.75 — and fixed the lost futex wakeup that had been failing `pthreads_smp`
since v0.76.

With those gone, the debt ledger has a new head: **`[mcpre] long probe never
started on cpu1`**, seen twice (v0.76, v0.77), never explained, and now the
oldest unexplained item in the project.

## OBJECTIVES

1. **Explain and fix the `[mcpre]` cpu1 stall.** Diagnose, reproduce, fix,
   demonstrate — the shape v0.78 established for carryover 3 and the lost wakeup.
2. **Audit SMP wake paths generally**, at `-smp 2` as well as `-smp 4`. The
   milestone's suspicion is that this is one instance of a class.
3. **Keep the gate green and the build at zero warnings** under
   `-Wall -Wextra -Werror`.

---

## PHASE 1 RESULT — THE AUDIT, AND A WINDOW FOUND BY READING

### What the failure has to explain

`cmd_mcpre` queues a probe on cpu1, sends `IPI_PING`, and waits for
`kprocs[pl].ran_on & 2`. v0.78 measured the normal case: the probe reaches ring
3 in **0 ticks**. The failing boots waited past a 500-tick budget — so whatever
happened, cpu1 failed to pick up queued work for **more than 500x** the normal
latency, on a machine that was otherwise healthy enough to finish the boot.

That rules out "the host was briefly busy" as a complete explanation. Something
stopped cpu1 receiving work, not merely slowed it.

### The window

`ap_main`'s scheduler loop is **check-then-sleep**:

```c
for (;;) {
    while ((p = rq_pop(idx)) >= 0 || (p = rq_steal(idx)) >= 0) { ... }  /* queue empty */
    if (!picked) { futex_timeout_scan(); tcp_timer_scan(); }
    ...
    __asm__ volatile("hlt");                       /* woken by IPIs */
}
```

`cpu_exec_proc` ends with `sti`, so interrupts are **already enabled** when this
`hlt` is reached. A `rq_push(1, p)` followed by `lapic_ipi(cpu1, IPI_PING)` that
lands between the failed `rq_pop` and the `hlt` is delivered immediately, handled
by `smp_ipi_dispatch` — which does

```c
me->ipi_ping++;      /* ping/wake */
lapic_eoi();
```

— and returns. The core then executes `hlt` and sleeps **on work that is already
in its queue, having already consumed the interrupt sent to tell it so.**

This is the same shape as the futex check-vs-park window v0.78 closed, one layer
down: a condition is tested, the decision to sleep is made, and the notification
can arrive in between and find nothing to do.

**The tree already knows the correct idiom and does not use it here.** The BSP's
`idle_fn` halts as `sti; hlt` — whose one-instruction interrupt shadow makes the
pair atomic — while `ap_main` uses a bare `hlt` with IF already set. The
difference between the two loops has never been reconciled.

### Why it is not fatal every time — and what that predicts

Each AP arms a **periodic** LAPIC timer in `ap_main` (LVT `0x320 = 0x20000|51`,
divider 16, count 3,000,000). Vector 51 fires regardless of `g_slice_on`; when
slicing is gated off the handler simply EOIs and returns. That interrupt wakes
the halted core, the loop re-runs `rq_pop`, and the queued probe is found.

So a spent ping normally costs **one timer period**, not a stall. Which gives a
sharp prediction:

> The stall requires the lost wake **and** the timer failing to rescue it.
> Neither alone is sufficient.

That is the hypothesis Phase 2 has to confirm or kill, and it is falsifiable:
if a recurrence shows cpu1 halted with its slice counter still advancing, the
timer was delivering and the window is not the whole story.

### EOI discipline: audited, and clean

Checked because a missed EOI would block every later interrupt at or below that
priority on the core, which would produce exactly this symptom:

| path | EOI |
|---|---|
| `smp_ipi_dispatch` (48 ping, 49 TLB) | unconditional, before return |
| `smp_preempt_ipi` vector 51, slicing off | EOI then return |
| `smp_preempt_ipi` from CPL0 | sets `resched`, EOI, return |
| `smp_preempt_ipi` from CPL3 | EOI **before** `resume_kernel` (which never returns) |

All four are correct. The CPL3 path is the one worth naming: it EOIs before
unwinding, which is necessary precisely because the unwind never comes back.

## PHASE 1 DELIVERABLE — INSTRUMENTATION AND A REPRODUCER

### The breadcrumb

The reason this item survived two milestones unexplained is not that it is
subtle. It is that **when it fired, nothing recorded what cpu1 was doing.** The
assertion printed that the probe had not run, and nothing else — so "slow host",
"lost IPI" and "wedged core" were all equally consistent with the evidence,
which is another way of saying there was no evidence.

`cpu_local` now carries `dbg_where`, updated at each point the AP loop can be
sitting in, plus `dbg_halts` and `dbg_last_run_tick`. A store of a constant to a
per-CPU line costs far less than the work between the points it marks, so it is
**not** behind a debug flag — a breadcrumb you have to enable is a breadcrumb you
do not have when the rare thing happens.

The failure diagnostic now prints cpu1's state, pings, slices, preempts, halts,
tasks run and stolen, and the tick of its last dispatch. The cases that
previously looked identical from outside now separate:

| observed | meaning |
|---|---|
| `HALTED`, `pings` unchanged | the ping never arrived |
| `HALTED`, `pings` advanced | it arrived and was **spent before the halt** — the window |
| `PREHALT` | caught inside the window itself |
| `EXEC` | busy on something else entirely |
| `BOOT` | never reached the scheduler loop |

`slices` was intended to answer whether cpu1's own timer is delivering. **It
does not** — see the correction below. The vector-51 handler EOIs and returns
before incrementing `slice_count` whenever `g_slice_on` is 0, so a zero there
means slicing was gated off, not that the timer is silent. Left in the table
because it is still worth printing; the wrong reading of it is called out
rather than quietly removed.

### `CPU1_STALL_REPRO`

Build with `make clean && make EXTRA=-DCPU1_STALL_REPRO`. On cpu1 only, and only
at the point of halting, it masks that core's LAPIC timer LVT — removing the
rescue that normally bounds a spent ping, and leaving the window itself exactly
as the shipping kernel has it.

Masking at the halt rather than at setup is deliberate: the arming code stays
byte-identical to the shipping kernel, so the reproducer differs from the real
thing in **delivery**, not in configuration.

### THE RESULT: THE HYPOTHESIS WAS WRONG, AND THE REPRODUCER SAID SO

**v1 did not reproduce it.** Masking cpu1's timer left the probe reaching ring 3
in 0 ticks exactly as before — and failed `[slice]`, a suite that legitimately
needs that timer. A reproducer that changes more than the path under test cannot
attribute what it observes; v0.78 wrote that lesson down and this repeated it.

**v2 reproduced it on the first boot**, and the breadcrumb answered the question
two milestones could not:

```
[mcpre  ] FAIL  long probe never started on cpu1 — DEADLINE after 6000 tick(s); cpu1 rq depth 0, 4 cpu(s) online
[mcpre  ]   cpu1 state=PREHALT pings=732739 slices=0 preempts=0 halts=135 ran=0 stolen=0 last-dispatch@tick 0 (now 11886)
```

`pings=732739`. The ping was never lost — pings arrived in floods. And **`rq
depth 0` with `ran=0`**: the probe left cpu1's queue without cpu1 running it.

**A sibling stole it.**

### The actual mechanism — a test defect, not a kernel one

`rq_steal` honours exactly two guards: a task's `affinity` mask, and the
one-shot `migrate_pin`. `cmd_mcpre` sets **neither** before

```c
rq_push(1, pl);
lapic_ipi(g_cpu[1].apic_id, IPI_PING, 0);
```

so the probe sits on cpu1's queue freely stealable by cpu2 or cpu3. The
assertion then requires `kprocs[pl].ran_on & 2` — that it ran *on cpu1*. Queueing
to a core does not mean running on that core in a scheduler with work stealing,
and nothing here bridges the gap.

When cpu1 wins the race, `ran_on` gets bit 1 and the assertion passes — the
normal case, 0 ticks. When cpu1 is slow enough that a sibling gets there first,
the probe runs elsewhere, bit 1 never sets, and the assertion fails **while
cpu1 is perfectly healthy**. In the wild the delay is host scheduling under TCG;
here it is a deliberate spin. Same outcome, same mechanism.

**The tree already knew.** `cpu_exec_proc` carries the comment *"a pre-existing
work-stealing race (see cmd_mcpre)"*, and role 31 already does the right thing —
`kprocs[p].affinity = 1u`, with a note that affinity is "the kernel's own
mechanism for exactly this, so use it rather than hoping the scheduler
cooperates". The knowledge was in the file; it was never connected to the
failure.

### What this explains, and what it corrects

It accounts for every observed property:

- **Intermittent** — it depends on which core wins a race.
- **`-smp 4` only** — at `-smp 2` the only other core is the BSP, which is
  running the suite and not stealing.
- **0 ticks normally** — cpu1 usually wins.
- **Healthy machine otherwise** — nothing is wedged; the boot completes.

And it corrects this project's own account. **v0.77 diagnosed this as a
wall-clock budget chosen on an idle machine and raised the ceiling 500 → 6000
ticks.** That reasoning was wrong and the fix could not have worked: once a
sibling has stolen the probe, waiting twelve times longer changes nothing. The
failure duly recurred in v0.78's gate. The ceiling raise and the added
diagnostics were still worth having — the diagnostics are how this was finally
answered — but they were not a fix, and the v0.77 changelog should not have
implied one.

### A correction to this document's own reasoning

`slices=0` above is **not** evidence that cpu1's timer is dead. The vector-51
handler returns early — EOI and out — when `g_slice_on` is 0, *before* it
increments `slice_count`. So `slices=0` is expected whenever slicing is gated
off, which it is here. This audit nearly used that zero as evidence for the
timer-failure half of the hypothesis; it means nothing of the kind.

The lost-wake window in `ap_main` documented above **is still real** — it is
visible by reading, and `idle_fn` uses the correct idiom while `ap_main` does
not. It is simply not what `[mcpre]` was failing on. It should be fixed on its
own merits, and now demonstrably not as a bugfix for this.

## PHASE 2 — TWO FIXES, NOW SEPARABLE

Phase 1 was diagnosis, and it found two independent things. Keeping them apart
matters: v0.77 conflated a symptom with a mechanism here once already.

### 1. `cmd_mcpre` must pin the probe it asserts about (closes the failure)

One line, using the mechanism the kernel already has and role 31 already uses:

```c
kprocs[pl].affinity = 1u << 1;    /* the assertion is that it runs on cpu1 */
rq_push(1, pl);
```

`rq_steal` honours `affinity` and will leave it alone. Validation is now cheap
and decisive: `CPU1_STALL_REPRO` fires this deterministically on the first boot,
so the fix must make that reproducer pass while the reproducer still holds cpu1
in the pre-halt window — which proves the pin, not the timing, is what changed.

### 2. The `ap_main` halt window (real, unrelated, worth fixing anyway)

The standard race-free idle, which `idle_fn` already approximates:

```c
__asm__ volatile("cli");
if (rq_nonempty(idx) || steal_available(idx)) { __asm__ volatile("sti"); continue; }
__asm__ volatile("sti; hlt");     /* atomic: the sti shadow covers the hlt */
```

The check must happen with IF clear, and the `sti; hlt` pair must be adjacent —
`sti` alone does not close the window, and a bare `hlt` after an enabled `sti`
is what is there now.

Open questions Phase 2 must answer rather than assume:

- **Is the halt window reachable in a way that matters?** With `[mcpre]`
  explained, there is no longer any observed failure attributed to it. It should
  be fixed because it is wrong, and claimed to fix nothing until something
  measures otherwise.
- **Does `-smp 2` behave differently from `-smp 4`?** The steal account predicts
  `[mcpre]` cannot fail at `-smp 2`, because the only other core is the BSP
  running the suite. That is a falsifiable prediction and worth running.
- **`me->resched` is set by the CPL0 preempt path and, as far as this audit
  found, read by nobody.** A separate potential dropped wake, needing its own
  answer rather than being folded into either fix above.

## STILL OPEN (inherited)

- **`SYS_THREAD_JOIN` has no timeout argument.**
- **`sys_epoll_wait`** is documented rather than converted (v0.78, with the
  invariant and its expiry condition written beside the code).
- **The virtio-net BAR assumption** is documented, not repaired.
- **The reproducers are manual builds** — `FORK_FUNNEL_REPRO`,
  `FORK_TIGHT_DEADLINE`, `FUTEX_RACE_REPRO`, and now `CPU1_STALL_REPRO`. Nothing
  automatically re-checks that they still reproduce what they claim.
- The v0.76/v0.77 security and POSIX gaps carry forward unchanged; the KDF
  remains the largest structural gap.
