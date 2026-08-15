# OutRun OS v0.78.0-metal — roadmap

Milestone 78. v0.77 is tagged (`fca93bd`). That release converted the last of
the ring-3 spin budgets, swept the build to one warning, and closed by naming
three things it did not do. This milestone takes two of them.

## OBJECTIVES

1. **Carryover 3 — the fork race.** Unaddressed since v0.75, carried explicitly
   through v0.76 and v0.77. The oldest open item in the project.
2. **The last build warning** — the kernel image's single RWX `LOAD` segment.
3. **Keep both build gates clean** under `-Wall -Wextra -Werror`.

---

## PHASE 1 RESULT — LINKER SPLIT, `make gate`, AND CARRYOVER 3 REPRODUCED

### 1. The RWX segment, and the thing it was hiding

The warning itself was cosmetic: `harden_kernel_wx()` has always remapped the
kernel at runtime so that no page is both writable and executable, and CR0.WP
makes kernel code immutable. The ELF program header said RWX because **every**
section shared one `PT_LOAD`, so its permissions had to be the union.

`linker.ld` now declares three:

```
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
```

```
LOAD 0x100000 R E    .boot .text
LOAD 0x14e000 R      .rodata .eh_frame
LOAD 0x183000 RW     .data .bss
```

**What the single segment was hiding is not cosmetic.** `harden_kernel_wx()`
derived "read-only" from `_etext` alone:

```c
if (va >= stext && va < etext) f |= 0;      /* code: R + X  */
else f |= PTE_WRITE | PTE_NX;               /* data: RW + NX */
```

`.rodata` and `.eh_frame` sit **above** `_etext`, so they fell into the `else`.
**212 KiB of kernel constants — jump tables, string literals, the embedded SDK
sources — were writable at runtime.** The split gives the region its own
`_srodata`/`_erodata` bounds, and the mapping now has three cases, with
`[_srodata,_erodata)` R+NX.

A boundary nothing tests is a boundary that drifts, so it is asserted rather
than commented: `rodata_poison()` writes into `.rodata` and requires a #PF,
exactly parallel to the existing `wp_poison()` for `.text`. **That assertion
would have FAILED before this milestone.** It is visible in the suite counts,
which went 479 → 480, 495 → 496 and 508 → 509 across the three configurations.

Build: **0 warnings**, and clean under `-Wall -Wextra -Werror`.

### 2. `make gate` — the matrix was reproducible by one person

`CLAUDE.md` has named uniprocessor / `-smp 4` SeaBIOS / `-smp 4` q35+VT-d as
required gate configurations since v0.76, but only the first had a target
(`release-verify`). The other two were driven by a harness that lived outside
the tree — so the release matrix could not be reproduced by anyone else, and the
v0.77.0 tag rests on runs nobody else could repeat.

`tools/gate-matrix.sh` + `make gate` / `make gate-all` fix that, carrying the
conventions this tree paid for individually: the ISO md5 stamped into every log,
two independent failure counters with disagreement failing the gate on its own,
a unique workdir plus `flock`, and an explicit **coverage line** naming what the
run did not test.

### 3. CARRYOVER 3 — REPRODUCED, WITH A CONTROL

This item has been carried for three milestones because v0.75's causal claim
rested on a single instrumented build that no longer exists, and a 12-run
uninstrumented negative control that never reproduced anything. It is now
reproducible on demand.

**What was actually still wrong.** `posix_fork_worker` waited with
`owaitpid(child, 30000)` — a **spin count**, the last one in the tree, and
precisely the budget the v0.75 fork-race note names as the trigger. It is now
`owaitpid_ticks(child, WAIT_T_FORK, &spent)`, it prints the ticks it waited, and
the kernel decodes role 29's two failure modes instead of printing a bare
number: **702 is a deadline, 703 is a child that ran and answered wrong.**

**The number the whole argument turns on is now permanent instrumentation.**
Every boot prints:

```
[posixstrs] fork enqueue->first dispatch: n=6  max=6 tick(s)  avg=3 tick(s)
```

`sys_fork` stamps `enq_tick`; the dispatch path records the delta on a child's
first run. Two counters, always on, so nobody has to re-instrument the kernel to
ask the only question that matters.

#### The measurements

Baseline, fixed kernel, idle host:

| configuration | dispatch max/avg | parent waitpid |
|---|---|---|
| uniprocessor | 50 / 48 | 50, 48, 46 |
| `-smp 4` SeaBIOS | 6 / 3 | 3, 3, 3 |
| `-smp 4` q35+VT-d | 6 / 3 | 1, 2, 3 |

The parent's wait tracks the child's dispatch latency almost exactly — the first
link of v0.75's chain, measured rather than argued.

`EXTRA=-DFORK_FUNNEL_REPRO` restores v0.75's defect (the hardcoded preferred
core) so the funnel can be measured:

| | fixed | funnel restored |
|---|---|---|
| `-smp 4`, idle | 6 / 3 | **48 / 19** |
| `-smp 4`, 6 guests on 16 cores | 6–8 max | **52–92 max** |
| uniprocessor | 50 / 48 | 48 / 47 — **unchanged** |

**The uniprocessor row is the control.** On one core, cpu0 *is* the forking
core, so the funnel is a no-op — and the numbers do not move. The same binary
change costs 8–11x under `-smp 4` and nothing at all where the mechanism cannot
apply. Under load the reproducer's wait distribution goes **bimodal** — 1–3
ticks when the forking core happens to be cpu0, 53–94 when it is not — which is
the funnel's signature and not something a generically slower build produces.

#### Firing the original symptom

None of the above fires the v0.74 symptom, because v0.78's own deadline
conversion put it out of reach: 94 ticks is 21x inside a 2000-tick budget. So
the budget was moved to where the machine already is, which is the "artificially
widened window" the v0.76 roadmap sanctioned for exactly this purpose.
`UEXTRA=-DFORK_TIGHT_DEADLINE` sets `WAIT_T_FORK` to 20 ticks — deliberately
**between** the two measured populations (fixed 1–8, funnel 52–94).

Both builds carry the tight deadline. Only one fails.

| `-smp 4`, `WAIT_T_FORK`=20 | dispatch max/avg | waitpid | posixstrs |
|---|---|---|---|
| fixed enqueue (control) | 6 / 2 | 3, 2, 4 | **12 passed, 0 failed** |
| funnel restored | 44 / 17 | 20, 20, 5 | **8 passed, 4 failed** |

```
[posixstrs] round 0 'fork/waitpid/SIGCHLD' (role 29 pid 609) FAILED: exit 702 (want 700)
[posixstrs]   ^ the parent's waitpid DEADLINE expired — the child had not exited yet
[posixstrs] round 0 child pid 620 (forked by role 29) FAILED: exit 44
```

**Exit 702 and exit 44 — the exact pair v0.74 reported.** And 44 arrives
alongside 702, confirming the downstream link v0.75 asserted: the parent gives
up, exits, its slot is recycled, and only then does the child ask who its parent
was.

This satisfies the v0.76 roadmap's definition of done (a), as written: a
configuration that fires the original symptom on an unfixed kernel, and the
fixed kernel clean on that same configuration.

#### What this does and does not establish

- **Establishes:** the funnel is sufficient to produce both symptoms; the fix
  removes them; the deadline alone does not cause them (the control shares it
  and passes 12/0); the effect is absent where the mechanism cannot apply
  (uniprocessor); and it scales with contention.
- **Does not establish:** that the funnel is what happened on the v0.74 host.
  That machine is gone and its binary with it. What can be said is that the
  mechanism v0.75 named reproduces both of v0.74's symptoms on demand, which is
  a great deal more than "twelve runs passed".
- **The reproduction needs a moved deadline.** On this host, with the shipping
  20 s budget, the funnel costs under a second and nothing fails. That is worth
  stating plainly: the funnel is a real defect that was really fixed, but on
  *this* hardware it is now a latency bug rather than a correctness one.

#### Reproducer hygiene

Both flags print an unmissable banner into the boot log, and the harness asserts
the banner is present. A reproducer kernel that looks like a normal one has
already been misread as a clean baseline once in this project. `EXTRA` reaches
only the kernel; `UEXTRA` (new) reaches ring 3. Neither triggers a rebuild on
its own, so **`make clean` when toggling either** — the same trap, twice over.

---

## PHASE 2 RESULT — THE LOST WAKEUP, FOUND AND FIXED

Phase 1 characterised it: both failing boots short by exactly one wake, every
passing boot balanced. Phase 2 found the code.

### The defect

`sys_futex_wait` took the futex lock for its compare, **released it**, and then
called `block_ring3`, which took the lock again to arm:

```c
futex_lock();
if (*(volatile uint64_t *)uaddr != val) { futex_unlock(); return -11; }
futex_unlock();                       /* <-- lock dropped between the two */
block_ring3(sf, p, key, timeout);     /* <-- re-takes it to arm */
```

A waker landing between those two critical sections scans for `parked` or
`wait_armed`, finds the waiter in **neither** state, matches nobody, and spends
its wake on empty air. The waiter then arms and parks against a wake that has
already happened, and sleeps until the deadline.

Three lines above that code sat its own comment: *"The compare-and-park is
atomic against SYS_FUTEX_WAKE (both take the futex lock), which is the whole
reason a futex needs kernel help at all: without it, a wake landing between 'I
read the value' and 'I am asleep' is lost."* The property was documented,
believed, and not implemented — the third time in this milestone that a comment
asserted an invariant the code did not have.

`sys_thread_join` was worse: it tested `thr_done` under **no lock at all** and
then parked. That is the path `pthreads_smp` exercises, and the one that failed.

### The fix

`block_ring3_locked()` requires the caller to hold the futex lock across its own
decision and hands the lock off. Both orderings are now safe — a waker that
acquires first has its state change precede our check, so we never sleep; a
waiter that acquires first is armed before the waker can scan, so the waker sees
`wait_armed`. The thread-exit path sets `thr_done` (with a barrier) *before*
calling `futex_wake_key`, which is what makes the second half of that argument
hold; it was checked, not assumed.

**The unconditional `block_ring3()` wrapper is deleted, not kept.** Its signature
is the footgun: it invites a caller to test a condition and then park. Removing
it makes the dangerous shape unavailable rather than merely discouraged, for the
same reason v0.77 deleted the spin-budgeted `cs_compile()`.

**No memory barriers were added.** `__sync_lock_test_and_set` is a full barrier
on x86-64 and `__sync_lock_release` is a release store, so the lock already
orders publication of `wait_key`/`wait_armed` against every waker's scan. The
ordering was never the defect; the lock GAP was. Barriers here would have looked
like a fix and changed nothing.

### The demonstration

A passing gate cannot establish this, at a natural rate near 1 boot in 8. So the
old shape was restored behind `EXTRA=-DFUTEX_RACE_REPRO` — test outside the
lock, arm separately, window widened, join deadline shortened to 2 s so the boot
still reaches the prompt to report itself.

| `-smp 4`, smp4-bios | parked/woken | assertion | `pthreads_smp` |
|---|---|---|---|
| pre-v0.78 shape | 44 / **43** | **FAIL** | 5 passed, **2 failed**, exit 937 |
| fixed | 44 / 45 | PASS | **7 passed, 0 failed** |

The reproducer shows the exact signature of both natural failures, and it proves
the new assertion is not vacuous: it fails when the defect is present and passes
when it is not. Logs committed as `OUTRUN-0.78-futex-{repro,fixed}.log`, each
md5-stamped and carrying the banner that says which build produced it.

Note that wakes may legitimately EXCEED parks (44 parked, 45 woken in the fixed
run): a wake can count a waiter caught in the arming window. The assertion is on
`g_futex_timeouts`, not on the balance — a balance test would have failed that
run spuriously, which is why the counter semantics were checked before the
assertion was trusted.

### Two reproducer mistakes, recorded

1. **The first version shortened `FUTEX_DEFAULT_TICKS` globally.** Every park in
   the kernel uses it, so a 2 s deadline perturbed subsystems unrelated to the
   defect and wedged the boot 24 suites in. A reproducer that changes more than
   the path under test cannot attribute what it observes. Only the join deadline
   moves now.
2. **The window was first widened by waiting on `g_ticks`.** That hangs: the BSP
   timer interrupt advances `g_ticks`, and the delay runs inside a syscall on the
   BSP with interrupts masked, so the clock it waits for cannot move. It is now a
   spin count — the one place in this tree where a spin count is the correct
   instrument, because what is wanted is "hold this core", a quantity of
   execution, not a duration.

### Not fixed

`sys_epoll_wait` parks with the same check-then-arm shape. Closing it means
holding the raw futex spinlock across an fd-table scan that takes
`g_ofile_lock` (rank 1) — a new lock ordering, and trading an intermittent stall
for a possible deadlock. Left alone deliberately; epoll re-executes its syscall
on wake and re-scans readiness, so a lost wake there costs a bounded wait rather
than a wrong answer.

## STILL OPEN

- ~~`make clean` ate the first reproduction's boot logs~~ — **fixed.** Both gate
  harnesses now write under `.logs/gate/` (gitignored, outside `$(BUILD)`), so a
  routine build step can no longer destroy the record. Verified with a canary
  file across `make clean`. What matters is still copied into `docs/`
  deliberately; this only stops the accidental loss.
- ~~The lost wake behind the v0.76 `pthreads_smp` failure~~ — **FOUND AND FIXED
  in phase 2 above.** It was a check-vs-park window in `sys_futex_wait` and
  `sys_thread_join`, reproduced on demand and closed.
- **Why cpu1 stalled in the v0.77 `mcpre` failure** — unexplained; the budget
  is fixed and instrumented, the cause is not known.
- **`SYS_THREAD_JOIN` still has no timeout argument.**
- **The virtio-net BAR assumption** is documented, not repaired.
- The v0.76/v0.77 security and POSIX gaps carry forward unchanged.
