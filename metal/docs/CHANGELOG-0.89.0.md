# OutRun OS v0.89.0-metal — CAS lock contention

**The v0.89.0 tag does not exist yet. Nothing here is a released claim.**
Opened on `main` at `v0.88.0`, with `VERSION` and `KERNEL_VERSION` both moved to
`0.89.0-dev` in the same commit — the Makefile derives its expectation from the
`-dev` suffix and warns when the two disagree.

## ARTEFACT

Not built. This section exists so that the release protocol's four steps have
somewhere to land, and so that an empty one is visibly empty rather than absent:

```
outrun-os-0.89.0.iso   (not built)
MD5    —
SHA256 —
```

## BASELINE

v0.88.0 at `79bd11a`, artefact `e2b212773609dfcab624b88ff9ee7ea5`, kernel ELF
`cc08fca2c4a3cf73a04a5efc4fa02f45`. All six tiers on the published artefact:

| tier | passed |
|---|---|
| uniprocessor | 555 |
| smp2-bios | 568 |
| smp4-bios | 574 |
| smp4-iommu | 587 |
| `gate-dirty` ×3 boots | 0 failing, empty diffs |
| `gate-dirty-smp` ×3 boots | 0 failing, empty diffs |

Plus `release-verify` PASS and four extra `smp2-bios` boots at 568.

## THE OPEN ITEM THIS CYCLE INHERITS

### `g_cas_lock` contention, and the question v0.88 deferred

v0.88 shipped the role-61 contention soak, which puts `2 × n` unaffined workers
through a real `cas_put` and `cas_free` per iteration — index probe, index stage,
`bm_alloc`, journal transaction, index remove, `bm_free`, generation bump, second
journal transaction — from every core at once. It passes, exactly: unreferenced
blocks and `used_blocks` return to precisely their pre-soak values, with zero
underflows and zero dangling index entries.

**What it also did was destabilise a neighbouring suite**, and that is the thread
to pull first.

Adding the soak made two `threadstrs` premise guards fail about half the time on
`-smp 2`. Bisected on one host inside one window: v0.87.0 4/4 PASS, v0.88 2/4,
v0.88 with `-DCASC_SKIP` 4/4. The guards were then aligned with `pthreads_smp`'s
`n >= 3` threshold, which is a **loosening** and was approved as one — the
alternatives are recorded below as not taken.

The underlying question is unanswered: **is the guard simply fragile at two CPUs,
or does the soak leave cpu 1 halted and slow to pick up work?** Those have
different fixes, and only one of them is a defect.

Three options were on the table at v0.88.0. Option 1 was taken:

1. **Align `threadstrs` with `pthreads_smp`** — done in v0.88.0. Cheapest, and it
   weakens an assertion.
2. **Run the soak after `threadstrs`** — restores the conditions the `n >= 2`
   guard was validated under without weakening anything, but only moves the
   fragility out of sight.
3. **Investigate the scheduler state the soak leaves behind** — whether the drain
   returns the machine to a balanced state, or leaves the BSP holding everything
   while the AP idles. The most useful answer and the most expensive.

### ANSWERED — it is PLACEMENT, not a stall

Measured, on 20 `-smp 2` boots, with the instrument built out of the v0.79
breadcrumbs (`dbg_where`, `dbg_halts`, `ipi_ping`, `slice_count`, `rq_ran`)
rather than beside them.

| evidence | reading |
|---|---|
| latency from queue to first cpu1 dispatch: **0–5 ticks**, every boot | prompt wake; never the one-timer-period signature of a spent ping |
| **0 slices** taken by cpu1 in every round | the LAPIC timer never had to rescue anything — the IPI did the work |
| ~400–460 pings taken per round | delivery is working; this is not a lost-IPI case |
| gap from soak drain to `threadstrs` queue: **~4,700 ticks (~47 s)** | dozens of suites run in between; no post-soak AP state survives that |
| the failing boot: latency **0 ticks**, cpu1 ran **6 tasks**, `cores_used = 1` | cpu1 woke instantly and worked — and one core still took the whole pool |

**cpu1 sits at `HALTED` at drain-end and at queue-time in every boot, passing and
failing alike.** That is ordinary idle, not a stall, and it is why the state
alone was never going to answer this: the discriminator is the *latency*, and it
is zero.

The verdict line prints the reading and the numbers behind it on every
multi-core boot, not only on the bad ones — a number that appears only when
something goes wrong cannot establish what normal looks like, which is the
condition that left this question open for two milestones.

#### Two corrections, both to my own instrument

**The first version suppressed the bug it was built to observe.** The marker was
armed in the three statements immediately before `rq_push_any` — two global
stores and a `__sync_synchronize()`, a full memory barrier dropped into the exact
race under test. Fourteen instrumented boots then showed two cores in 14 of 14,
against roughly half failing on the uninstrumented build. Arming it earlier, so
the sequence from the last pre-existing statement to the push is untouched,
restored the failure: **1 of 6**. An instrument that makes the thing it measures
stop happening has not measured it.

**The verdict string named the wrong core.** It read *"cpu0 had already drained
the thread pool"*. The first boot that actually reached it disproved that:
`ran_on mask 2`, so all six threads ran on **CPU 1, the AP**, and cpu0 got none
of them. The category was right and the detail was invented. The true statement
is symmetric and more useful — whichever core reaches the queue first takes the
whole pool, and it is as likely to be the AP as the BSP — which also disposes of
"the BSP is simply quicker off the mark". The mask is now printed beside the
verdict so the claim stays checkable against the same line.

**The same defect had a second instance, in the line the suite actually reports.**
Fixing the `AP1 PROFILE` verdict left its sibling untouched: the `n == 2` OBSERVE
line still read *"completed on 1 core before AP pick-up"*, which asserts the same
unmeasured mechanism — that the BSP drained the pool and the AP arrived late. It
is the more visible of the two, since it is what `threadstrs` prints on every
two-core boot whether or not anyone is reading the profile.

It now reads the winner out of `g_thr_ran_mask` and names it (`0aa783a`):

```
OBSERVE: 2-CPU worker dispatch completed entirely on CPU 1 (AP 1) before the
other core reached the pool (ran_on mask 2, cores 1, ring-3 high-water 1)
```

A third case came out of writing it. `cores_used < 2` is also true when *zero*
cores were recorded, and the mask is fed by departing threads — so an empty mask
means the round did not run, not that it ran on one core. Reporting that as
"completed on CPU 0" would dress an instrument fault as a placement result, which
is the defect being fixed rather than a fix for it. It gets its own line naming
itself as an instrument fault.

**The branch went 0 for 7 on natural boots** — the single-core case is roughly
1 in 4 and simply did not come up — so it was exercised with a throwaway
`-DTS_FORCE_SINGLE` build that substitutes a synthetic mask, once per case:

| forced mask | line printed |
|---|---|
| 1 | `completed entirely on CPU 0 (the BSP) … (ran_on mask 1, …)` |
| 2 | `completed entirely on CPU 1 (AP 1) … (ran_on mask 2, …)` |
| 0 | `recorded NO core at all (ran_on mask 0, …) — instrument fault` |

That proves the line renders and names the right core; the 7 natural boots are
what establish the branch is reachable at all. The scaffolding is not in the
tree — it existed to answer "has this code ever executed", which "it compiles"
does not, and a branch that has never run has not been checked.

#### What this means for the guard

`threadstrs`' `n >= 3` threshold stands, and now on evidence rather than on
convenience: at two CPUs the pool is small enough that one core routinely takes
all of it, the other core is neither late nor stalled, and there is nothing to
fix in the scheduler. Restoring `n >= 2` would require making the *pool* bigger
or the rendezvous stricter — a change to the suite, not to the kernel — and is
not proposed here.

**Option 3 is therefore closed.** Options 1 (taken in v0.88.0) and 2 are
unchanged below for the record.

**Option 3 was the v0.89 item.** If the drain is leaving the machine unbalanced,
that is a scheduler observation worth having on its own account, and it would let
`threadstrs` go back to `n >= 2` on evidence rather than on hope. If it is not,
the honest outcome is to say so and leave the guard where it is — which is a
result, not a failure, and is the same shape as v0.87's `SYS_FSTAT` prediction
and v0.86's journal-interrupt claim.

`-DCASC_SKIP` remains in the tree as the bisection tool that found this.

## THE CAS SOAK AT FULL SIZE, AND UNDER ACTIVE REPLAY

`make cascstress` builds the role-61 soak at **4 workers per online core and 96
iterations each**, against the gate's 2 and 12. It sets the flag on *both* halves
for the reason `appendsoak` does: `CASC_ITERS` reaches the kernel through `EXTRA`
and `user/init.c` through `UEXTRA`, and raising only one side makes the worker
and the kernel disagree about how many iterations happened — a discrepancy that
reads as a lost file rather than as a build mistake. The phase now prints the
pair it was compiled with, so the two are distinguishable at a glance.

**Fresh `smp4-bios`, 16 workers on 4 cores, 96 iterations each:**

```
14 ok, 2 deadline, 0 failed; ran on 4 core(s) (mask f) in 18629 ticks
unreferenced 2 -> 2, used_blocks 1121 -> 1121, gen bumps 386 -> 3244,
frees 386 -> 3244, underflow 0 -> 0, dangling 0
```

**2,858 real frees**, and every block came back — `unreferenced` and
`used_blocks` return to *exactly* their pre-soak values, not merely close to
them. Zero underflows, zero dangling index entries, generation table in step.
All eleven phase assertions PASS, and the suite total is unchanged at 574.

The two deadline expiries are the v0.88 convention working as designed: at 96
iterations two of sixteen workers ran out their 60 s budget, and because deadline
expiry carries its own exit code the phase reports it separately and declines to
call it a defect. The byte-level checks are untouched and still fatal, so this
cannot hide a lost write.

**Under active journal replay**: the same stress image through
`gate-dirty --smp 4`, three boots on one volume. **PASS**, with `cas-recovered=1`
on boots 2 and 3 — so the full-size soak ran on a volume whose CAS metadata
journal had just been replayed from a real power cut, and the ring-3 worker's own
per-iteration checks held throughout.

That is the double-write and positional-write claim, verified where it matters:
each iteration writes twice to one descriptor, reads back **both** halves, and
exits on a distinct code if either is wrong (1846 for the first payload, 1850 for
the second). `SYS_WRITE_FILE` being positional since v0.83 is what makes the file
2 × payload rather than a replacement, and the second half reading back correct
is what proves the second write did not disturb the first — under contention from
every core, on a freshly recovered volume.

### MEASURED — the soak does not contend `g_cas_lock`, and cannot

Per-CPU telemetry was added to the `cas` acquire path this cycle: contended
acquisitions, retries, TSC cycles and worst case, per core, baselined at soak
start so the numbers are the soak's and not the boot's. It was built to quantify
the contention. It found there is none.

| config | workers | `g_cas_lock` acquisitions | contended |
|---|---|---|---|
| uniprocessor | 2 | 216 | 0 |
| smp2-bios | 4 | 432 | 0 |
| smp4-bios | 8 | 864 | 0 |
| smp8-bios | 16 | 1,728 | 0 |

**This is not a dead counter.** `acq` is exactly 108 per worker at every width,
scaling 216 → 432 → 864 → 1,728; the zero means *did not happen*, not
*unreachable*, which is the distinction `g_reproc_stale_ppid` cost this project a
milestone to learn. The kernel's own pre-existing per-lock counters — nothing to
do with this cycle — agree boot-wide on smp4: `vfs` **75 of 209 contended (36%)**,
`cas` **0 of 4,539**.

The cause is structural, and is visible in the rank order the tree already
documents:

```
klock_acquire(&g_vfs_lock)        rank 2, held across the whole write
  -> vfs_write_locked()
     -> cas_put()
        -> klock_acquire(&g_cas_lock)   rank 3
```

Only the holder of the VFS lock can reach the CAS lock, so `g_cas_lock` is not
merely uncontended under this workload — it is **uncontendable** through it. The
role-61 soak is a real concurrency test and its accounting assertions mean what
they say (16 workers across 8 cores, mask `ff`, exact recovery of `unreferenced`
and `used_blocks`, zero underflow, zero dangling). But the lock it stresses is
`g_vfs_lock`, and both its name and this document's title said otherwise.

**Left as an observation, not an assertion.** `tot_con > 0` fails every boot on a
correctly working kernel — it did, on smp2, smp4 and smp8, which is how this was
found. `tot_con == 0` would freeze the current nesting into a requirement and
make a limitation look like intent. Neither is honest, so the line prints the
measurement and names the mechanism, and carries a second branch that fires if
`g_cas_lock` ever *is* contended, telling the reader this note has gone stale.

Making the title true needs a workload that reaches `cas_put` without holding the
VFS lock. That is a change to the suite, and it is a maintainer's decision rather
than a side effect of adding telemetry — recorded here as the open item it is.

## CARRIED FORWARD FROM v0.88

- **The standing index audit cannot see a dangling entry once the block it names
  has been reallocated.** It guards against the state persisting, not against the
  corruption having happened. Only the crash phase's measurement, taken
  immediately after recovery, catches that.
- **The generation table and the TALLY sweep are both per-boot.** v0.87 proved the
  reset is complete; nothing establishes that anything is *lost* by it.
- **The oversubscription ceiling is higher, not absent.** Above 8 cores at 4:1 the
  clamp bites again, loudly rather than silently. `MAX_KPROC = 64` is the real
  wall behind `APPSMP_MAXW = 32`.
- **The contention soak is 12 iterations per worker**, sized to fit the standing
  gate rather than to stress the allocator for minutes. No long-running opt-in
  target exists, and its behaviour above 4 cores is untested — `smp8-bios` is a
  diagnostic and was not run for v0.88.0.
- **`lseek`/`SEEK_END` on another user's tmp descriptor discloses that file's
  length.** POSIX-correct and asserted deliberately; closed as a question.
- **VOL_TMP has no mode and no group.** Deliberate; owner-or-root is the whole
  rule, so a tmpfile cannot be shared.

## WORKSPACE

`.claude/worktrees/v075-tier2-crypto` was carried through v0.87 and v0.88 without
a decision. **Removed in v0.89, after the check that had never been done.**

The three previous entries all said the same thing — *"git holds no record of its
contents, so removing it is a plain filesystem deletion of files nobody has
diffed"* — and that was true of `git worktree list`, not of the object database.
Every file was hashed and asked for by name:

| measured | result |
|---|---|
| source blobs (`kernel64.c`, `init.c`, `Makefile`) | **in the repo**, traceable to `c0baf76` / `320acc2` — the v0.75 Tier 2 crypto work, merged as #62 and #68 |
| 147 `OUTRUN-*.log` files | **zero real content differences**; every one differs from its committed copy only by CRLF, from the checkout's line-ending conversion |
| 58 files whose blobs git does not hold | all of them `metal/build/*`, `metal/iso/boot/*.elf`, or `.claude/settings.local.json` — build output and local config, both gitignored |

Nothing unique was lost, and that is a measurement rather than the assumption the
previous three entries were resting on. 119 MB reclaimed; the empty `.worktrees/`
directory left by the v0-86 removal went with it. `git worktree list` and
`git status` are both clean.

## FIRST COMMITS OF THE CYCLE

1. `VERSION := 0.89.0-dev` and `KERNEL_VERSION "0.89.0-dev"`, moved together —
   done in the commit that carries this file.
2. A control boot confirming the `0.89.0-dev` banner reaches ring 3, not merely
   the build.
