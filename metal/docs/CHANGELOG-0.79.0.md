# OutRun OS v0.79.0-metal — the stall that was never a stall

Milestone 79. v0.78 closed carryover 3 and fixed the lost futex wakeup, leaving
one item at the head of the ledger: **`[mcpre] long probe never started on
cpu1`**, seen in the v0.76 and v0.77 gates, unexplained through two releases.

It is explained, and it was not a stall. cpu1 was never wedged, never late, and
never missed an interrupt. **A sibling core stole the work.**

## THE ROOT CAUSE: WORK STEALING, IN THE TEST

`cmd_mcpre` queues its probe on cpu1 and asserts it *ran on* cpu1:

```c
rq_push(1, pl);
lapic_ipi(g_cpu[1].apic_id, IPI_PING, 0);
...
if (!(kprocs[pl].ran_on & 2u)) FAIL("long probe never started on cpu1");
```

`rq_steal` honours exactly two things — a task's `affinity` mask and its
one-shot `migrate_pin` — and the probe carried **neither**. It sat on cpu1's
queue freely stealable by cpu2 or cpu3.

**Queueing to a core is not running on that core in a scheduler with work
stealing.** When cpu1 won the race the assertion passed in 0 ticks, which is the
overwhelmingly common case. When cpu1 was slow enough that a sibling got there
first, the probe ran elsewhere, `ran_on` never gained bit 1, and the suite
reported a stall on a core that was perfectly healthy and had simply been beaten
to the work.

This accounts for every property the failure had:

| observed | explained by |
|---|---|
| intermittent | it is a race between cores |
| `-smp 4` only | at `-smp 2` the only other core is the BSP, running the suite |
| 0 ticks normally | cpu1 usually wins |
| machine otherwise healthy | nothing was ever wrong with cpu1 |
| survived a ceiling raise | waiting longer does not un-steal a task |

**The tree already knew.** `cpu_exec_proc` carries the comment *"a pre-existing
work-stealing race (see cmd_mcpre)"*, and role 31 already pins itself with a note
that affinity is "the kernel's own mechanism for exactly this, so use it rather
than hoping the scheduler cooperates". The knowledge was in the file and was
never joined up with the failure.

### What this corrects

**v0.77 diagnosed this as a wall-clock budget chosen on an idle machine** and
raised the `mcpre` ceiling from 500 to 6000 ticks. That reading was wrong and the
change could not have worked — once a sibling has taken the probe, waiting twelve
times longer changes nothing — and the failure duly recurred in the v0.78 gate.

The v0.77 work was not wasted: the diagnostics it added are how this was finally
answered. But it was not a fix, and the changelog implied one.

## `migrate_pin`, NOT `affinity` — the obvious fix was the wrong one

The first attempt was the obvious one, a lifetime affinity mask:

```c
kprocs[pl].affinity = 1u << 1;      /* WRONG */
```

It stopped the reported failure. Under the reproducer the probe reached ring 3
on cpu1 in 21 ticks, `ran_on 2`, and "never started on cpu1" passed. Had the
criterion been "the failing assertion now passes", it would have shipped.

It broke the suite's headline assertion instead:

```
[mcpre  ]  FAIL  the captured context MIGRATED CORES: started on cpu1, finished on cpu2
```

`cmd_mcpre` exists to prove a preempted ring-3 context can **resume on a
different core** — it deliberately sets `migrate_to = 2`. A lifetime pin to cpu1
forbids exactly the thing under test. Fixing the first half of a test by breaking
the second half is not a fix; it changes which assertion is red.

That second failure is also not new: it is what the committed
`OUTRUN-0.47-boot-smp4-bios-flake.log` and `OUTRUN-0.48-boot-smp4-iommu-flake.log`
recorded, and whose filenames call it a flake. **Both are the same root cause
seen from opposite sides** — an unpinned task in a work-stealing scheduler, once taken too
early and once prevented from moving at all.

The kernel already had the right mechanism:

```c
kprocs[pl].migrate_pin = 1;         /* one-shot, consumed by the first dispatch */
```

`migrate_pin` guarantees cpu1 gets the **first** run — all `ran_on & 2` requires
— and is spent by `cpu_exec_proc` before the directed migration to cpu2 happens.
One shot is exactly the right lifetime, and the difference between the two
mechanisms is the whole fix.

## THE `ap_main` IDLE WINDOW — REAL, CLOSED, AND NOT THE CULPRIT

The audit that found the steal also found a genuine check-then-sleep window in
the AP scheduler loop. `ap_main` drained its queue, found it empty, and halted:

```c
while ((p = rq_pop(idx)) >= 0 || (p = rq_steal(idx)) >= 0) { ... }
...
__asm__ volatile("hlt");            /* woken by IPIs */
```

`cpu_exec_proc` returns with interrupts **already enabled**, so a `rq_push` +
`IPI_PING` landing between the failed pop and the halt is delivered immediately,
handled by `smp_ipi_dispatch` — which increments a counter and EOIs — and the
core then sleeps on work already in its queue, having spent the interrupt sent
to announce it. The same shape as the futex check-vs-park window v0.78 closed,
one layer down.

### `sti; hlt` alone would have been a no-op

Worth stating precisely, because it looks like the fix:

```c
__asm__ volatile("sti; hlt");       /* changes NOTHING on its own here */
```

Interrupts are already on at that point, so the `sti` does nothing and the window
stays exactly where it was. The one-instruction interrupt shadow only buys
something when the **decision to sleep was taken with interrupts masked**. The
loop now does:

```c
__asm__ volatile("cli");
if (g_cpu[idx].rq_h != g_cpu[idx].rq_t) { __asm__ volatile("sti"); continue; }
__asm__ volatile("sti; hlt");
```

A push before the read is seen and the core loops; a push after it leaves its IPI
**pending**, delivered the instant `sti; hlt` enables interrupts. The unlocked
read of `rq_h`/`rq_t` can claim work when there is none — a wasted lap — but
never the reverse, which is the only direction that matters. Stealing is
deliberately not re-checked with interrupts off: a sibling with spare work is a
transient worth learning about on the next wake; a push aimed at this core is
not, because nothing re-announces it.

**This fixes nothing that was observed failing**, and is claimed as nothing else.
Phase 1 hypothesised it as the `[mcpre]` cause and the reproducer refuted that.

## `me->resched` — A FLAG NOTHING READ

Audited for this release: `me->resched` was written in exactly one place — the
CPL0 path of `smp_preempt_ipi` — read **nowhere**, and never cleared. A field
shaped like a mechanism that was not one. (It is distinct from `g_need_resched`,
the BSP-global flag, which is live.)

It is deliberately **not** wired into a yield check. When the preempt IPI catches
a core in the kernel there is nothing safe to yield to — that is the entire
reason the path defers — and the actual mechanism is the **sender retrying**
until the IPI lands at CPL3, which `cmd_mcpre` does in a loop. Acting on the flag
would either do nothing or preempt at exactly the point the code had just decided
it must not.

So it counts instead, and `mcpre` prints it. The comment on that path claims
deferral is *"statistically immediate: the probes spend >99% of their time"* in
ring 3 — a plausible assertion nothing measured. Now it is measured.

## INSTRUMENTATION: A BREADCRUMB, PERMANENTLY ON

This item survived two milestones for one reason: **when it fired, nothing
recorded what cpu1 was doing.** The assertion printed that the probe had not run
and nothing else, so "slow host", "lost IPI" and "wedged core" were all equally
consistent with the evidence.

`cpu_local` now carries `dbg_where` — a one-word breadcrumb over the AP loop's
states — plus `dbg_halts` and `dbg_last_run_tick`, and the failure diagnostic
prints cpu1's state, pings, slices, preempts, halts, tasks run and stolen, and
its last dispatch tick. It is **not** behind a debug flag: a breadcrumb you have
to enable is a breadcrumb you do not have when the rare thing happens.

It paid for itself immediately. The line that solved this reads:

```
cpu1 state=PREHALT pings=732739 slices=0 preempts=0 halts=135 ran=0 stolen=0
```

`pings=732739` killed the lost-interrupt theory outright. `rq depth 0` with
`ran=0` — the work left cpu1's queue without cpu1 running it — named the real
one.

### A correction inside the diagnostic itself

`slices=0` above is **not** evidence that cpu1's timer is dead. The vector-51
handler EOIs and returns *before* incrementing `slice_count` whenever
`g_slice_on` is 0. This audit nearly used that zero as evidence for a
timer-failure hypothesis that was already wrong.

## THE REPRODUCER, AND A REPEATED MISTAKE

`CPU1_STALL_REPRO` (`make clean && make EXTRA=-DCPU1_STALL_REPRO`).

**v1 was wrong in a way this project has documented before.** It masked cpu1's
LAPIC timer to remove the rescue that bounds a spent ping. It did not reproduce
the stall — and it *did* fail `[slice]`, a suite that legitimately needs that
timer. A reproducer that changes more than the path under test cannot attribute
what it observes. v0.78's changelog says exactly that, and this repeated it.

**v2 holds cpu1 in the pre-halt window with a spin count instead**, leaving every
other mechanism alone, and reproduced the exact failure on the first boot.

A spin count, not a tick deadline — the same lesson as `FUTEX_RACE_REPRO`:
`g_ticks` is advanced by the BSP timer, and a core holding interrupts off can
wait on it forever. What is wanted is "hold this core", a quantity of execution.

### The validation that matters

```
[mcpre  ] long probe reached ring 3 on cpu1 in 36 tick(s) (ceiling 6000)
[mcpre  ] long: exit 21 ran_on 6          <- bits 1|2: cpu1 then cpu2
[mcpre  ]  PASS x5, including MIGRATED CORES
```

36 ticks against a 0-tick baseline: the reproducer was **still delaying cpu1**,
and the probe waited for it instead of being stolen. The pin changed the outcome,
not the timing. That is the discriminating result, and the gate is not.

## VERIFICATION

```
outrun-os-0.79.0.iso
md5     c2f4bf12f1ba59d97846223d87fe5651
sha256  b279ae7348cc48ce4553effe9553b0806600cb118ef559e8f2c46bfe01b10f79
```

Every configuration below booted that image; each log's first line carries the
md5, and the boot banner in them reads `bare-metal kernel 0.79.0-metal`.

### Fresh-image matrix

| configuration | suites | passed | failed | rank faults | boot |
|---|---|---|---|---|---|
| uniprocessor (`release-verify`, published artefact) | 45 | — | **0** | 0 | 305 s |
| uniprocessor (`make gate`) | 45 | 481 | **0** | 0 | 305 s |
| `-smp 4`, SeaBIOS | 45 | 497 | **0** | 0 | 230 s |
| `-smp 4`, q35 + VT-d, `intremap=on` | 47 | 510 | **0** | 0 | 235 s |

### Dirty-volume gates

| configuration | boot 1 | boot 2 | boot 3 | verdict |
|---|---|---|---|---|
| `make gate-dirty-smp` | 45 / 0 | 45 / 0, 1 reset | 45 / 0, 1 reset | **PASS** |
| `make gate-dirty` (UP) | 45 / 0 | 45 / 0, 1 reset | 45 / 0, 1 reset | **PASS** |

**10 boots across six configurations, 0 failing assertions, 0 rank faults.**

### What the new counter measured

```
[mcpre  ] cpu1 preempt_count +1 (deferred at CPL0: 0); long: exit 21 ran_on 6 ...
```

**Zero deferrals**, in both SMP configurations. The preempt IPI landed at CPL3 on
the first attempt every time, so the `smp_preempt_ipi` comment's claim that
deferral is "statistically immediate" is now measured rather than asserted — and
the sender-retry loop it depends on did not have to retry at all. `ran_on 6`
(bits 1|2) is the pinned probe doing exactly what the suite asks: first run on
cpu1, resume on cpu2.

### Build

```
0 errors, 0 warnings; clean under -Wall -Wextra -Werror both halves
release-version-check: version check OK + kernel banner check OK: 0.79.0-metal
```

## WHAT THIS RELEASE DOES NOT DO

- **The gate does not establish that `[mcpre]` is fixed.** It fired roughly 1
  boot in 8; ten clean boots are consistent with it still being present. The
  evidence is the reproducer — deterministic failure before the pin, pass after,
  window unchanged. The same distinction v0.78 drew for the lost wakeup.
- **`-smp 2` is still not in the gate.** The steal account predicts `[mcpre]`
  cannot fail there, because the only other core is the BSP running the suite.
  That is a falsifiable prediction and it has not been run.
- **The AP idle window is fixed but was never observed failing**, so nothing
  measures whether closing it mattered.
- **`SYS_THREAD_JOIN` still has no timeout argument.**
- **`sys_epoll_wait`** remains documented rather than converted (v0.78), with the
  invariant and its expiry condition beside the code.
- **The virtio-net BAR assumption** is documented, not repaired.
- **No memory-hard KDF, no password-change syscalls, no reboot-surviving
  lockout, no confidentiality for the stored database.** The KDF remains the
  largest structural gap.
- **No execute permission bit, no directory permissions, no supplementary
  groups, no login program, no lockout expiry, no administrative unlock.**

## COVERAGE THIS GATE DID NOT PROVIDE

- **Bare metal and Proxmox are untested.** Every result is QEMU, TCG, no KVM.
- **The four reproducers are manual builds** — `FORK_FUNNEL_REPRO`,
  `FORK_TIGHT_DEADLINE`, `FUTEX_RACE_REPRO`, `CPU1_STALL_REPRO`. Nothing
  automatically re-checks that they still reproduce what they claim, so they can
  rot silently between releases.
- **One boot per fresh-image configuration**, three per dirty configuration.
  Not soak runs.
