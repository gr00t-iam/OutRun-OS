# OutRun OS v0.78.0-metal — the milestone that closed the oldest item

Milestone 78. v0.76 built a gate and found the gate's own counter blind. v0.77
converted the last ring-3 spin budgets and swept the build to one warning. This
release closes the item both of those carried forward without touching —
**carryover 3, open since v0.75** — and, in the course of proving it, found and
fixed the lost wakeup that had been failing `pthreads_smp` intermittently since
v0.76.

Four defects here were **invariants that were documented, believed, and false**.
That is the theme, and it is worth naming: this project's comments are unusually
good, which makes them unusually easy to trust instead of checking.

## CARRYOVER 3 — REPRODUCED, AND CLOSED

Open since v0.75, carried **explicitly** through v0.76 and v0.77 because the
causal claim rested on a single instrumented build that no longer existed and a
12-run negative control that never reproduced anything.

### What was still wrong

`posix_fork_worker` waited with `owaitpid(child, 30000)` — a **spin count**, the
last one in the tree, and precisely the budget the v0.75 fork-race note names as
the trigger: *"the failing children waiting long enough for the parent's
30000-poll waitpid budget to expire"*. A spin count means different durations at
1 vCPU and at 4, which is exactly why the failure looked like a race sensitive to
host speed and binary layout.

It is now `owaitpid_ticks(child, WAIT_T_FORK, &spent)`, prints the ticks it
waited, and the kernel decodes role 29's two modes: **702 is a deadline, 703 is a
child that ran and answered wrong.**

### The number the argument turns on is now permanent instrumentation

```
[posixstrs] fork enqueue->first dispatch: n=6  max=6 tick(s)  avg=3 tick(s)
```

`sys_fork` stamps `enq_tick`; the dispatch path records the delta on a child's
first run. Two counters, always on, so nobody ever again has to re-instrument
the kernel to ask the only question that matters.

### The measurement, and the control

| | fixed | funnel restored (`EXTRA=-DFORK_FUNNEL_REPRO`) |
|---|---|---|
| `-smp 4`, idle | 6 / 3 | **48 / 19** |
| `-smp 4`, 6 guests on 16 cores | 6–8 max | **52–92 max** |
| uniprocessor | 50 / 48 | 48 / 47 — **unchanged** |

**The uniprocessor row is the control.** On one core cpu0 *is* the forking core,
so the funnel cannot apply — and the numbers do not move. The same one-line
change costs 8–11x under `-smp 4` and nothing where the mechanism is absent.
Under load the reproducer's waits go **bimodal** (1–3 ticks when the forking core
happens to be cpu0, 53–94 when it is not), which is the funnel's signature and
not what a generically slower build produces.

### Firing the original symptom

The above does not fire the v0.74 symptom, because this milestone's own deadline
conversion put it out of reach — 94 ticks is 21x inside a 2000-tick budget. So
the budget was moved to where the machine already is:
`UEXTRA=-DFORK_TIGHT_DEADLINE` sets `WAIT_T_FORK` to 20 ticks, deliberately
**between** the two measured populations. Both builds carry it; only one fails.

```
[posixstrs] round 0 'fork/waitpid/SIGCHLD' (role 29 pid 609) FAILED: exit 702 (want 700)
[posixstrs]   ^ the parent's waitpid DEADLINE expired — the child had not exited yet
[posixstrs] round 0 child pid 620 (forked by role 29) FAILED: exit 44
```

| `WAIT_T_FORK`=20, `-smp 4` | dispatch max/avg | posixstrs |
|---|---|---|
| fixed enqueue (control) | 6 / 2 | **12 passed, 0 failed** |
| funnel restored | 44 / 17 | **8 passed, 4 failed** |

**Exit 702 and exit 44 — the exact pair v0.74 reported**, with 44 arriving
alongside 702 and confirming the downstream link v0.75 asserted: the parent gives
up, exits, its slot is recycled, and only then does the child ask who its parent
was. This is the v0.76 roadmap's definition of done (a), as written.

**What it does not establish:** that the funnel is what happened on the v0.74
host. That machine is gone and its binary with it. What can be said is that the
mechanism v0.75 named reproduces both symptoms on demand — a great deal more
than "twelve runs passed".

## THE LOST WAKEUP

`pthreads_smp` had failed intermittently since v0.76 with no explanation. v0.77's
instrumentation named the failure mode (937, a deadline, not a join defect) and
the futex counters gave a discriminator: both failing boots short by **exactly
one** wake (45/44, 46/45) where five passing boots balanced.

### The defect

`sys_futex_wait` took the futex lock for its compare, **released it**, then called
`block_ring3` which took the lock again to arm:

```c
futex_lock();
if (*(volatile uint64_t *)uaddr != val) { futex_unlock(); return -11; }
futex_unlock();                       /* <-- dropped between the two */
block_ring3(sf, p, key, timeout);     /* <-- re-takes it to arm */
```

A waker landing between those critical sections scans for `parked` or
`wait_armed`, finds the waiter in **neither**, matches nobody, and spends its
wake on nothing. Three lines above sat the claim that this could not happen:
*"The compare-and-park is atomic against SYS_FUTEX_WAKE (both take the futex
lock)."* `sys_thread_join` was worse — it tested `thr_done` under **no lock at
all**. That is the path `pthreads_smp` exercises.

### The fix

`block_ring3_locked()` requires the caller to hold the lock across its decision
and hands it off. Waker-first means its change precedes our check and we never
sleep; waiter-first means we are armed before the waker can scan. `thread_exit`
sets `thr_done` with a barrier **before** `futex_wake_key` — checked, not
assumed, because the second half of that argument depends on it.

The unconditional `block_ring3()` wrapper is **deleted**. Its signature is the
footgun that invites test-then-park.

**No memory barriers were added.** `__sync_lock_test_and_set` is a full barrier
on x86-64 and `__sync_lock_release` is a release store, so the lock already
orders publication against every waker's scan. The ordering was never the
defect; the lock GAP was. Barriers would have looked like a fix and changed
nothing.

### Demonstrated, not hoped

At a natural rate near 1 boot in 8 a passing gate proves nothing, so the old
shape was restored behind `EXTRA=-DFUTEX_RACE_REPRO`:

| `-smp 4` | parked/woken | assertion | `pthreads_smp` |
|---|---|---|---|
| pre-v0.78 shape | 44 / **43** | **FAIL** | 5 passed, **2 failed**, exit 937 |
| fixed | 44 / 45 | PASS | **7 passed, 0 failed** |

The reproducer shows the exact signature of both natural failures **and** proves
the new assertion is not vacuous — it fails when the defect is present.

`pthreads_smp` now asserts `g_futex_timeouts == tmo0`. Note that wakes may
legitimately **exceed** parks (44/45 above): a wake can count a waiter caught in
the arming window, so a parks-equals-wakes test would have failed that run
spuriously. The counter semantics were checked before the assertion was trusted.

## `sys_epoll_wait` — AUDITED AND DELIBERATELY LEFT ALONE

Same check-then-arm shape, so converting it looks like the obvious next step.

**It would not deadlock; it would leak.** `block_ring3_restart()` is `noreturn`:
it unwinds through `resume_kernel()` and the frame never runs again, which is why
it releases the futex lock itself immediately before unwinding. A `g_ofile_lock`
held across it is released by **nobody** — the first epoll park would permanently
wedge every descriptor operation in the system. The obstacle is liveness, not
ordering. (On ordering: `ep_notify_fd` already collects under `g_ofile_lock` and
wakes outside it, and holding ofile across the arm would be the tree's first
violation of that rule.)

**And a lost wake here is late, never wrong.** This park uses the restart
protocol — RIP rewound onto the `syscall`, RAX carrying 78 — so *every* exit,
including the deadline expiring, re-executes `SYS_EPOLL_WAIT` and rescans
readiness before consulting anything else. The cost is a delay bounded by
`ep_deadline`, after which the rescan finds the event and reports it correctly.
Contrast `sys_thread_join`, where a lost wake became `-ETIMEDOUT`, an **error**,
reported as a join defect after 200 s.

The invariant is now written beside the code:

> Any parker whose wake carries INFORMATION the waiter cannot recompute must arm
> atomically with its condition check. Any parker that RE-EXECUTES and recomputes
> its condition on resume may arm separately, and must have a bounded deadline so
> the recomputation is guaranteed to happen.

It also states its own expiry condition: if epoll's wake ever carries something
the rescan cannot rediscover, the reasoning dies with it.

## THE IMAGE SPLIT, AND 212 KiB THAT WERE WRITABLE

`linker.ld` declares three `PT_LOAD`s (`R E` / `R` / `RW`) instead of one, which
removes the last build warning — **the build is now at zero warnings**, clean
under `-Wall -Wextra -Werror` for both halves.

That part is cosmetic; what the single segment hid is not. `harden_kernel_wx()`
derived "read-only" from `_etext` alone, and `.rodata`/`.eh_frame` sit **above**
`_etext`, so they were mapped `RW+NX`. **212 KiB of kernel constants — jump
tables, string literals, the embedded SDK sources — were writable at runtime.**
The split gives them `_srodata`/`_erodata` bounds and the mapping a third case.

`rodata_poison()` asserts a write there faults, exactly parallel to the existing
`wp_poison()` for `.text`. **That assertion would have failed before this
release**, and it is visible in the suite counts: 479→480, 495→496, 508→509.

## `make gate` — THE MATRIX WAS REPRODUCIBLE BY ONE PERSON

`CLAUDE.md` has required three fresh-image configurations since v0.76, but only
uniprocessor had a target. The other two were driven by a harness outside the
tree — so **the v0.77.0 tag rests on runs nobody else could repeat.**

`tools/gate-matrix.sh` + `make gate` / `make gate-all` fix that, carrying the
conventions this tree paid for one at a time: md5 in every log, two independent
failure counters with disagreement failing the gate by itself, `flock` and a
unique workdir, and a **coverage line naming what was not tested**.

Gate logs now live under `.logs/gate/`, not `$(BUILD)`. `make clean` has
destroyed harness evidence twice — a carryover-3 reproduction that had to be
re-run, and the only copy of the unexplained v0.76 `[mcpre]` failure, whose
roadmap entry still says "preserved at" a path that exists nowhere.

## THE KERNEL HAD BEEN LYING ABOUT ITS VERSION FOR FOUR RELEASES

`KERNEL_VERSION` read `"0.73.0-metal"` and had since v0.73. The boot banner, the
`ver` command and the `OUTRUN=` environment variable handed to every ring-3
process all announced **0.73.0** through v0.74, v0.75, v0.76 and v0.77.

Same class as the v0.75 incident that created the release protocol: an artefact
naming itself wrongly. It survived that protocol because `release-version-check`
compared the Makefile's `VERSION` against the git tag and **never looked at the
string the kernel prints**. There were three names for a release; the gate
checked two. It now greps `kernel64.c` and warns loudly on disagreement.

## VERIFICATION

```
outrun-os-0.78.0.iso
md5     acb0e22d0f94fd6db40a7da1c4b67f93
sha256  983a38dedb75de4f4dc6a2c14c321299fee09137911a58b1e7d64b4df2f09f38
```

Every configuration below booted that exact image; each log's first line carries
the md5, and all ten do.

### Fresh-image matrix

| configuration | suites | passed | failed | rank faults | boot |
|---|---|---|---|---|---|
| uniprocessor (`make release-verify`, the published artefact) | 45 | — | **0** | 0 | 305 s |
| uniprocessor (`make gate`) | 45 | 481 | **0** | 0 | 305 s |
| `-smp 4`, SeaBIOS | 45 | 497 | **0** | 0 | 235 s |
| `-smp 4`, q35 + VT-d, `intremap=on` | 47 | 510 | **0** | 0 | 235 s |

47 rather than 45 under VT-d is `iommu` and `capdma`, config-gated on the
emulated unit — the tell that the target really is running with an IOMMU.

### Dirty-volume gates

| configuration | boot 1 | boot 2 | boot 3 | verdict |
|---|---|---|---|---|
| `make gate-dirty-smp` | 45 / 0 | 45 / 0, 1 reset | 45 / 0, 1 reset | **PASS** |
| `make gate-dirty` (UP) | 45 / 0 | 45 / 0, 1 reset | 45 / 0, 1 reset | **PASS** |

Consecutive-boot diffs empty in both directions; both durable cross-boot
artefacts created in boot 1 and found in every later boot; zero fixture-reset
refusals.

**10 boots across six configurations, 0 failing assertions, 0 rank faults**, and
the lost-wake assertion PASS in every one of them.

### Build

```
0 errors, 0 warnings
clean under -Wall -Wextra -Werror for both the kernel and ring 3
release-version-check: version check OK + kernel banner check OK: 0.78.0-metal
```

## WHAT THIS RELEASE DOES NOT DO

- **Why cpu1 stalled in the v0.77 `mcpre` failure is unexplained.** The budget
  is fixed and instrumented; the cause is not known. With carryover 3 closed,
  this inherits the position of oldest unexplained item in the project.
- **`SYS_THREAD_JOIN` has no timeout argument.** Less pressing than it looked in
  v0.77 — the 200 s park deadline is no longer reachable by the defect that used
  to reach it — but a genuinely wedged thread still costs 200 s.
- **`sys_epoll_wait` is documented, not converted**, for the reasons above.
- **The virtio-net BAR assumption** is documented, not repaired: the probe
  assumes every capability lives in `common_bar` and never checks, where `vblk`
  does.
- **The kernel image keeps its RWX-free split but no W^X enforcement changes
  beyond `.rodata`.**
- **No memory-hard KDF, no password-change syscalls, no reboot-surviving
  lockout, no confidentiality for the stored database.** Carried from v0.76;
  the KDF remains the largest structural gap.
- **No execute permission bit, no directory permissions, no supplementary
  groups, no login program, no lockout expiry, no administrative unlock.**
  Carried from v0.74.

## COVERAGE THIS GATE DID NOT PROVIDE

- **Bare metal and Proxmox are untested.** Every result is QEMU, TCG, no KVM.
- **One boot per fresh-image configuration**, three per dirty configuration.
  Not soak runs. The lost wakeup fired about 1 boot in 8 before it was fixed, so
  a green matrix alone would not have distinguished "fixed" from "did not fire" —
  which is precisely why the reproducer exists and why its result, not the
  matrix, is the evidence for that fix.
- **The reproducers are not run by the gate.** `FORK_FUNNEL_REPRO`,
  `FORK_TIGHT_DEADLINE` and `FUTEX_RACE_REPRO` are manual builds; nothing
  automatically re-checks that they still reproduce what they claim.
- **`sys_epoll_wait`'s restart argument is reasoned, not measured.** No test
  drops an epoll wake on purpose to confirm the rescan recovers it.
