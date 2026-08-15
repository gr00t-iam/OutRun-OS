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

## STILL OPEN

- **`make clean` ate the first reproduction's boot logs** before they were
  copied out of `build/`. The run was repeated and preserved; the incident is
  recorded here because it is the third time in this project that evidence has
  been lost to a working directory, and the lesson has still not been made
  mechanical. A harness that produces evidence should write it somewhere
  `clean` cannot reach.
- **The lost wake behind the v0.76 `pthreads_smp` failure** — unexplained.
- **Why cpu1 stalled in the v0.77 `mcpre` failure** — unexplained; the budget
  is fixed and instrumented, the cause is not known.
- **`SYS_THREAD_JOIN` still has no timeout argument.**
- **The virtio-net BAR assumption** is documented, not repaired.
- The v0.76/v0.77 security and POSIX gaps carry forward unchanged.
