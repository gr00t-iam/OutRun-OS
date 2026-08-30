# OutRun OS v0.88.0-metal — roadmap

Opened on `main` at the v0.87.0 tag. `VERSION` and `KERNEL_VERSION` are **not
yet** moved to `0.88.0-dev`; that is the first commit of the cycle proper, not
part of this skeleton.

**The v0.88.0 tag does not exist yet.** Nothing here is a released claim.

---

## Baseline

v0.87.0, tag `v0.87.0` at `8c0f273`. Full six-tier sweep plus the new
oversubscription tier — **eleven boots, 6,075 assertions, 0 failed, 0 rank
faults**:

| tier | passed |
|---|---|
| uniprocessor | 540 |
| smp2-bios | 554 |
| smp4-bios | 558 |
| smp4-iommu | 571 |
| `gate-dirty` ×3 boots | 540 each |
| `gate-dirty-smp` ×3 boots | 558 each |
| `gate-oversub` 4:1 | 558 |

Unreferenced blocks held at 2 across every dirty boot without growing. The
v0.87 generation-reset audit reported 0 residue on the reused-image boots.

What that baseline does **not** cover: bare metal, Proxmox, soak or repeat
beyond the boots above, and any intermittent below roughly 1 in 10 boots on the
fresh tiers.

---

## Carried forward from v0.87 — both were never started

Stated plainly because v0.87 shipped with two of its four numbered objectives
untouched, and a carried item that is only ever carried is a habit rather than a
record.

### §2 (v0.87). Crash injection on the PUT path and the legacy `cas_free` branch

`CRASH_INJECT_COMMIT_FAIL` covers the directory journal and the *journaled* CAS
free path. Two paths have never been interrupted under test:

- **The CAS PUT path.** It has always had the consistent ordering — `bm_alloc()`
  before `cas_journal_write()`, so all three of its shadows describe the same
  post-transaction state. That is an *argument*, and v0.86 is the milestone that
  demonstrated what arguments about guards are worth: the journal-interrupt claim
  read correctly and was backwards, and the instrument built to confirm it
  disproved it on its first boot.
- **The legacy pre-journal branch of `cas_free`**, reached when `g_cas_legacy` is
  set. v0.85 fixed the ordering in the journaled branch and measured it; the
  legacy arm was left alone and is untested.

**What would make this non-vacuous.** The v0.85 free-path work is the template:
build the harness against the *unfixed* ordering first and watch it go red, then
fix. For PUT specifically the interesting question is whether an interrupted put
can leave a bitmap bit set with no index entry naming it — the same unreachable
block the free path produced — and the instrument that found that was
`cas_unreferenced_locked()`, asking which allocated blocks a live file names
rather than whether the counters agree with each other.

**Note the arithmetic audit will not catch it.** `used_blocks == popcount(bitmap)`
passes either way, because recovery restores both halves from the same snapshot
and they agree with each other. That is exactly how the v0.85 defect shipped.

### §4 (v0.87). Falsifiers for the three unproven journal-IRQ guards

Of the four checks at `vj_publish()`, only the **premise guard's counter** has
been observed producing both verdicts — it read 9 when the first assertion
demanded 0, and 9 again when the corrected assertion demanded 9. The other three
have never been watched failing:

| check | status |
|---|---|
| detection (`dc > 0`) | unproven |
| positive control (sampler can report IF set) | unproven |
| byte integrity (all 128 bytes read back) | unproven |
| premise guard (`dirq == dc`) | **proven** — both verdicts observed |

**Either give them falsifiers or rule explicitly that a regression guard does not
need one.** Both positions are defensible. What is not defensible is leaving it
unstated, which is the condition v0.86 and v0.87 both shipped in.

A ruling is a real deliverable here: three of these are *regression* guards
rather than defect detectors, and the honest argument that a regression guard
earns its place by construction — it fires if a future change breaks the thing
it watches — may well be the right answer. If so, write it down and stop
carrying the item.

---

## New debt from v0.87

### The worker-cap ceiling above `APPSMP_W` cores

v0.87 added `gate-oversub` and, with it, an assertion that the phase ran at the
ratio it was **built** for. That assertion **detects** a degradation it does not
fix.

`nw` is capped at `APPSMP_W * APPSMP_OSRATIO` = 16. On a host with more than
`APPSMP_W` (4) online cores the cap bites and the effective ratio falls below the
configured one — at `-DAPPSMP_OSRATIO=4` on 8 cores that is `nw = 16, n = 8`, a
ratio of 2, identical to the default tier.

Before v0.87 that would have passed silently: every other assertion in the phase
is derived from `nw`, so a capped run satisfies all of them. Now it fails loudly,
which is strictly better and still not a fix. **The tier cannot run above 4:1 on
a host with more than 4 online cores**, and the reference host has exactly 4, so
the ceiling is invisible here and would bite the first person to run the gate on
a larger machine.

**Options, in rough order of cost:**

1. Raise `APPSMP_W` and add worker roles. Roles are a shared namespace matched by
   nothing but the integer — `\.role = ` in the kernel and `role == ` in
   `init.c` — and v0.81/v0.82 both lost time to two suites sharing role 7.
   Cheapest to write, easiest to get subtly wrong.
2. Decouple the worker count from the role count: let *N* workers cycle through
   *W* roles with N > W, which the phase already does (`i % APPSMP_W`), and lift
   only the cap. The per-pattern expectation is already computed rather than
   assumed, so it may need no test change at all — worth checking before
   assuming it does.
3. Cap by a separate `APPSMP_MAXW` decoupled from `APPSMP_W * ratio`, so the
   ratio and the ceiling stop being the same number.

Option 2 looks right and is recorded as a *hypothesis*, not a decision. The v0.87
cycle spent real time on a prediction about `SYS_FSTAT` that measurement
disproved; the lesson taken is to write the option down and then check it, rather
than to write it down as settled.

---

## Workspace

### Orphaned directory: `.claude/worktrees/v075-tier2-crypto`

Found during v0.87 release hygiene and deliberately not removed then. Measured
state, carried forward so the decision does not have to be re-investigated:

| property | value |
|---|---|
| path | `.claude/worktrees/v075-tier2-crypto` |
| dated | 9 August, v0.75 era — twelve milestones back |
| in `git worktree list` | **no** |
| `.git` pointer file | present, naming `.git/worktrees/v075-tier2-crypto` |
| that admin directory | **absent** — git already pruned the registry |
| associated branch | none; `git branch -a` lists only `main` |

Git tracks nothing about it. It cannot hold branch state, it is not a registered
worktree so `git worktree remove` does not apply, and — unlike the `v0-86` case
closed in v0.87 — **git cannot tell us whether its contents are unique**, because
it has no record of them.

Removing it is therefore a plain filesystem deletion of files nobody has diffed:

```
rm -rf .claude/worktrees/v075-tier2-crypto
```

**Logged rather than executed.** It predates this work by three weeks and was not
created by any cycle that has run since. If it is still here at the v0.88
release, that is the point to either delete it or say why it is being kept —
carrying it a third time without a decision would be the habit this section
exists to avoid.

Also present: an empty `.worktrees/` left when `v0-86` was removed. Inert — git
does not track empty directories — and removable with `rmdir .worktrees`.

---

## Inherited KNOWN-NOT-FIXED

Carried from v0.87 unchanged unless noted:

- **The generation table and the TALLY sweep are both per-boot.** v0.87 proved
  the reset is *complete*; it did **not** establish that anything is *lost* by
  it. The plausible answer remains "nothing the refcount rebuild does not already
  re-derive", and it is unmeasured. This is the open point 3 of v0.87 debt A.
- **No crash injection on the CAS PUT path or the legacy `cas_free` branch** —
  §2 above.
- **Three of the four journal-IRQ checks have never been observed failing** —
  §4 above.
- **`lseek`/`SEEK_END` on another user's tmp descriptor discloses that file's
  length.** POSIX-correct and asserted deliberately (exit 1793). v0.87 enumerated
  the full surface — `SYS_FSTAT`, `SYS_STAT` and `SYS_READDIR` all refuse tmp —
  and confirmed length is the whole of it. **Closed as a question; the wording
  stands.**
- **VOL_TMP has no mode and no group.** Deliberate; owner-or-root is the whole
  rule, so a tmpfile cannot be shared.
- **The oversubscription cap bites above `APPSMP_W` cores** — new in v0.87,
  detailed above.

---

## First commits of the cycle

Not done by this skeleton, and listed so the order is not re-derived:

1. `VERSION := 0.88.0-dev`, `KERNEL_VERSION "0.88.0-dev"`, moved together — the
   Makefile derives its expectation from the `-dev` suffix and warns when they
   disagree.
2. A control boot confirming the `0.88.0-dev` banner reaches ring 3, not merely
   the build.
