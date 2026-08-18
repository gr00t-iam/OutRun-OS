# Carryover 3 — re-verified at the v0.84.0-dev tip

**Carryover 3 is CLOSED.** It was reproduced and closed in v0.78
(`CHANGELOG-0.78.0.md`). This note exists because a session handover in v0.84
described it as still open and proposed re-implementing work that had already
landed — so the closure is now restated with a fresh measurement attached, at
the tip, rather than resting on a changelog three milestones back.

Measured 2026-08-18, commit `c372ecf`, uniprocessor, fresh image per boot.

## What was measured

The defect-B half: `ppid_live()`'s generation guard, and role 53's
cross-generation orphan phase that exercises it. Two boots of the same tree,
differing only in `-DFORK_RACE_REPRO`, which reverts the generation compare and
nothing else. Both ISOs were built from a clean tree and their md5s confirmed to
differ before the second boot was trusted — a reproducer that silently compiled
as the baseline would otherwise have reported the baseline's result under the
reproducer's name.

| | baseline | `EXTRA=-DFORK_RACE_REPRO` |
|---|---|---|
| iso md5 | `86efa114519b5be770e3cf9c0f3fdcd5` | `3a8c295d64d0a6a95226798012ca3a73` |
| whole boot | 45 suites, **496 passed, 0 failed** (315 s) | 45 suites, **493 passed, 3 failed** (320 s) |
| `[posixstrs]` | 16 passed, 0 failed | 13 passed, 3 failed |
| orphan's `getppid()` | exit **42** — read 0, correctly orphaned | exit **2000652** — read **stranger pid 652** |
| stale links DETECTED | 2 | 2 |
| stale links RESOLVED (the bug) | **0** | **2** |
| duplicate-pid control | 0 | 0 |

Logs, each stamped with the md5 of the image it booted:

- `OUTRUN-0.84-carryover3-defectB-baseline.log`
- `OUTRUN-0.84-carryover3-defectB-repro.log`

## Why the reverted build is the point

CLAUDE.md's standing rule is that a test that cannot fail has not passed, and
carryover 3 is the case that earned the rule: its harness reported 12/12 against
a deliberately broken kernel for a whole session because the workload could not
reach the defect. So the baseline's three green assertions are only worth
something next to a build in which those same three go red.

They do, and they go red *precisely* — the three that fail are exactly the three
that speak about the recycle, and every other assertion in the suite is
unmoved:

```
FAIL  the doomed parent's slot really was recycled while the orphan still pointed at it (positive control)
FAIL  getppid() across that recycle answered orphan (0), never a stranger's pid
FAIL  no stale parent link was ever resolved to a live slot
```

The reproducer names the stranger rather than merely reporting a wrong answer:
orphan pid 651 was told its parent was pid **652**, the throwaway fork that took
the doomed parent's vacated slot. That is defect B's signature, not a generic
failure.

DETECTED reads 2 in **both** builds, which is the load-bearing number: it says
the guard was reached on this boot in the shipping kernel too. RESOLVED is 0 in
the baseline and 2 in the reproducer — and RESOLVED is trivially 0 in a build
whose only increment site is compiled out, which is why it is asserted alongside
DETECTED and never alone.

## What this does not cover

- One boot per configuration, uniprocessor only. It cannot see an intermittent
  below roughly 1 in 1 boot, and it says nothing about `-smp 2/4` or the IOMMU
  configuration, where the orphan phase's first-fit premise is unchanged but the
  surrounding scheduling is not.
- No dirty-volume reuse (`gate-dirty`, `gate-dirty-smp`).
- The funnel half of carryover 3 (`FORK_FUNNEL_REPRO`, `FORK_TIGHT_DEADLINE`,
  the exit 702 / exit 44 pair) was **not** re-run here. v0.78 measured it; this
  note re-measures only the defect-B guard.
