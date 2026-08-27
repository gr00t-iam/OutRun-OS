# OutRun OS v0.81.0-metal — the release that made its own evidence falsifiable

Milestone 81. Written into the tree in v0.85, from the annotated tag and the
commits it covers; the tag remains the primary record and its checksums are
reproduced below unchanged.

v0.80 shipped scrypt and a tested credential migration. v0.81 spent itself on a
narrower and less glamorous question: **when this project's harness says a thing
passed, what exactly did it measure?** Four of the five changes below exist
because the honest answer was "less than it appeared to".

## ARTEFACT

```
outrun-os-0.81.0.iso
MD5    3fca48afd97d6c47de393fd94233fba3
SHA256 7b3b7435550c6fbf4417cea471efc460874abc0aaf3ba702a6abaecbb4eb04e0
```

`VERSION` and `KERNEL_VERSION` bumped in `bd6f0a3`, committed **before** the
tag; the booted kernel reports `0.81.0-metal`. `make release-verify` PASS on
md5 `3fca48af` — 45 suites, 0 failing assertions, 0 rank faults, prompt reached
in 305 s uniprocessor.

## WHAT LANDED

### #86 — klock ownership validation, and the spin budget it exposed

Lock ownership is now validated rather than assumed. The interesting part was
not the validation but what it surfaced: a spin budget that had been sized for a
machine nobody was still running.

### #87 — waits became deadlines, not iteration counts

`shm` and `mmapfile` waits converted to tick deadlines. Exit codes 980, 981 and
1533 now separate *a slow host* from *a COW, sharing or mapping defect* — a
distinction a spin count cannot express, because the same budget means different
durations at one vCPU and at four.

### #88 — the gate decides COMPLETENESS before CORRECTNESS

`gate-matrix` had six sequential assignments to one status variable, so whichever
fired last won. A boot killed at `GATE_CAP` before reaching the prompt was
therefore reported as `FAIL`, carrying whatever suites had happened to report
before the kill — a timeout wearing the costume of a suite failure.

`TRUNCATED` and `NO-PROMPT` now outrank `FAIL`, and both are named in the
coverage line as untested. `tools/gate-classify-test.sh` pins the case on
synthetic logs in under a second, and `make gate` runs it **first**: a harness
that cannot classify a run correctly has nothing useful to say about a kernel.

### #89 — `cmd_mcq` gets a tick-resident probe (role 52)

A fixed 3-million-iteration probe fit inside one round-robin TCG quantum, so
cpu1 could enter and leave ring 3 before the BSP was scheduled back in. The
ring-3 concurrency assertion was reading its own absence as a pass.

It is live again at `n >= 2` **because the overlap was made real, not because
the threshold moved** — which is the distinction that matters when an assertion
starts passing after a change.

### #90 — suites measure themselves, not their predecessors

`appsstrs` and `posixstrs` reset the ring-3 high-water at entry. Previously they
sampled a global that earlier suites had already raised, so their concurrency
assertions could be satisfied by `cmd_mcq`'s leftovers. `posixstrs`'s old
baseline clause **could not fail**: it sampled a monotonically rising counter and
then asserted the counter had not fallen.

`mcpre_long` converted to a tick deadline — the last fixed-iteration budget in
these suites.

## DISPROVEN, AND KEPT IN THE TREE

A one-sided barrier in `cmd_mcq` — spin until `g_inr3 >= 1` before entering
ring 3 — does **not** fix the two-cpu concurrency failure. Measured: 5 failures
in 16 boots with it, against 1 in 8 without, and the barrier expired in every
one.

The reason is structural: a level test cannot distinguish "cpu1 has not started"
from "cpu1 has already finished". The comment survives at the site so the next
reader does not spend a session retrying it.

## GATE

Two independent full 4/4 matrices on the pre-bump tree
(md5 `274e7c511d7ee7d80a9ec23f9664e7f1`), both PASS:

| tier | assertions | failed |
|---|---|---|
| uniprocessor | 489 | 0 |
| smp2-bios | 499 | 0 |
| smp4-bios | 505 | 0 |
| smp4-iommu | 518 | 0 |

**Not run for this tag:** `gate-dirty`, `gate-dirty-smp`, bare metal, Proxmox.

## KNOWN, NOT FIXED

- **`smp4-iommu` is host-speed sensitive.** 225–250 s clean, but 1080–2065 s
  with `capdma`/`vfiostrs`/`mcpre` failures on a degraded host — **including at
  unmodified HEAD**. Run a control before blaming a change. (This note has since
  earned its keep twice: v0.84 and v0.85 both hit degraded-host failures that a
  control cleared.)
- `threadstrs` still reads the shared ring-3 high-water without resetting its
  own. Closed in v0.84.
