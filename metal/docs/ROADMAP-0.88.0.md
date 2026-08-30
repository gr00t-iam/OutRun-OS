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

#### Outcome — the PUT path is now crash-tested, and the hazard is the one v0.48 wrote down

**The question was already stated in the source.** The CAS-metadata journal's own
header comment names the failure it was added to close: *"index home-write lands,
then a crash before `cas_flush_meta`'s bitmap/superblock write lands -> index says
hash X is at block B, bitmap says block B is free -> a future put legitimately
overwrites block B, silently corrupting a different dedup'd file."* Since v0.48
the evidence for that guard has been that the ordering reads correctly. v0.86 is
the milestone that established what such evidence is worth.

**A third injection arm.** `g_cjp_arm`, beside v0.85's `g_cji_arm` (directory
commit) and `g_cjf_arm` (free), fires inside `cas_put()` after the home index
write and before `cas_flush_meta()` — the mirror of the free arm's window.
Separate trigger, for the reason those two are separate from each other.

**A new instrument, and it answers the mirror question.**
`cas_index_verify_locked()` counts index entries naming a block the bitmap calls
free. `cas_unreferenced_locked()` — the v0.84/v0.85 instrument — counts allocated
blocks nothing names. **Neither sweep can see the other's defect**, and the
standing `used_blocks == popcount(bitmap)` audit sees neither, because the index
is not party to it. Both report a `live` count alongside the defect count, so a
zero cannot come from a sweep that looked at nothing.

The index audit is a **standing** assertion, on every boot rather than only under
`-DCRASH_INJECT_COMMIT_FAIL`: the crash phase proves the journal survives a
deliberate interruption, and this proves the invariant holds across an ordinary
boot's whole workload. Different claims; the cheap one is worth keeping.

**The falsifier is not a contrived reorder — it is the shipped pre-v0.48 code.**
`-DCAS_PUT_NOJOURNAL_REPRO` performs the put with no journal at all: home index
write, then bitmap and superblock, nothing in between.

| build | fired | dangling | probe block | next put | verdict |
|---|---|---|---|---|---|
| journalled (`-DCRASH_INJECT_COMMIT_FAIL`) | 1 | 0 → **0** | 1125 | 1126 | **PASS** 554 / 0 / 0 ranks |
| falsifier (`+ -DCAS_PUT_NOJOURNAL_REPRO`) | 1 | 0 → **1** | 1125 | **1125** | **FAIL** 552 / 2 — as required |

The falsifier does not merely trip the invariant. **The very next put was handed
block 1125 while the index still named it** — the corruption the v0.48 comment
predicts, reproduced end to end rather than argued. Logs, image-stamped:
`OUTRUN-0.88-put-crash-pass.log` (md5 `3a2f5d76084b3944099f4b53bfe7ba7a`) and
`OUTRUN-0.88-put-crash-nojournal-repro.log` (md5
`8ce98513b0d6befb633ae481f6b99035`).

**What the falsifier run also showed about the standing audit — a real limit,
recorded rather than smoothed over.** In the broken build the *end-of-boot* index
audit still read **0 dangling**, because by then the next put had reallocated
block 1125 and the stale entry no longer named a *free* block — it named a block
belonging to somebody else. The standing audit's shape cannot see the aftermath
once the block is reissued; only the crash phase's measurement taken immediately
after recovery catches it. The standing audit is a guard against the state
persisting, not a detector of the corruption having happened.

Two smaller notes, both deliberate:

- The probe is a **direct `cas_put` with no dirent**, leaving one allocated,
  indexed, unreferenced block behind. Routing it through `vfs_write_file` would
  put a dirent commit and its flushes between the injection and the remount, any
  of which could write the bitmap home and mask the state under test. The residue
  costs nothing: the phase compiles only under `-DCRASH_INJECT_COMMIT_FAIL`,
  never in a gate or release build, so the `CAS_UNREF_BUDGET` audit never sees it.
- The content is **salted from `SB->put_count`**, which is persistent and
  monotonic. Identical content would dedup, `cas_put` would return before
  reaching the injection, and the phase would then be asserting over a put that
  did not happen — on a reused volume, silently.

**Verified.** Default build, image md5 `a868830fd228f72b29eeb47dc2e87226`, clean
rebuild with zero warnings:

| tier | result |
|---|---|
| uniprocessor | **PASS** 542 / 0 / 0 ranks (305 s) |
| smp2-bios | **PASS** 556 / 0 / 0 ranks (230 s) |
| smp4-bios | **PASS** 560 / 0 / 0 ranks (245 s) |
| smp4-iommu | **PASS** 573 / 0 / 0 ranks (250 s) |
| `gate-selftest` | 17 passed, 0 failed |

Every tier is **+2 on v0.87**, and +2 is the whole of it: the two standing index
audits. The five crash-phase assertions do not compile into a gate build, which
is why the counts move by exactly two and not by seven.

Not covered by the above: dirty-volume reuse, bare metal, Proxmox, soak, and any
intermittent below roughly 1 in 1 boot per configuration. The crash phase itself
was run **uniprocessor only**, in both its passing and its falsified build.

#### Ruling — the legacy `cas_free` branch is UNREACHABLE, not untested

The second half of §2 asked for crash injection on the pre-journal branch of
`cas_free`, reached when `g_cas_legacy` is set. **Nothing sets it.** Measured,
not inferred — `g_cas_legacy` appears at thirteen sites and every one of the
three assignments writes `0`: its initialiser, `cas_format()`, and `cas_mount()`,
where the line still carries its own v0.56 explanation (*"pre-v4 volumes no
longer mount at all"*). Only version 5 mounts; the volume that once set the flag
is refused outright.

So there are ten branch sites on a variable that is a compile-time constant. A
crash harness for that arm could only be written by inventing a way to set the
flag, and it would then be testing a path no volume on disk can produce — the
`ppid_live()` mistake with the sign reversed: not a counter nothing increments,
but a branch nothing enters.

**Closed as unreachable.** The remaining question is whether the dead arms should
be deleted, which is a mechanical ten-site change and a separate commit from this
one — it is a decision about the code, not about the evidence, and mixing it into
a measurement commit is how the measurement gets harder to read. Recommended:
delete them, and keep the v0.48 comment as history.

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

#### Outcome — option 2 was right, and it was the smallest part of the work

**Root cause: a conflation, not a resource limit.** The clamp read
`nw > APPSMP_W * APPSMP_OSRATIO`. `APPSMP_W` is the number of distinct worker
**roles** — four payload patterns, roles 56..59 — and says nothing about how many
workers may run. The phase already cycles roles with `i % APPSMP_W` and computes
the per-pattern expectation from `nw` rather than assuming it. The cap was the
array bound wearing the role count's name.

**A detail that reframes the v0.87 result.** That clamp bites exactly when
`n > APPSMP_W`, *independent of the ratio*. At n = 4 it is a no-op. So the 4:1
tier passing on the reference host never exercised the ceiling at all — the debt
was real and structurally unreachable here, which is why it took a new
configuration to see.

**`smp8-bios`.** Added to `gate-matrix.sh`: eight vCPUs, which TCG emulates
regardless of what the host has. The ratio arithmetic's correctness does not
depend on those vCPUs running truly in parallel, and this is the only way to
reach `n > 4` on a 4-core machine. Not in `GATE_CONFIGS` — it is slower, and it
is a diagnostic rather than a standing tier.

**The fix itself is six lines**: `APPSMP_MAXW`, decoupled from the role count and
sized by what actually bounds it — BSS (`MAXW × ITERS × PAY`, 16 KiB default and
128 KiB soak, the same order as `g_cas_refs` and `g_cas_gen`) and `MAX_KPROC`,
64 system-wide and shared with every other suite, which is why 32 rather than
something arbitrary. Ceiling moves from 4 cores to **8 at 4:1**, 16 at 2:1.

### Three more layers, each hidden behind the one before it

Everything below was found by running the fix, not by reviewing it. Three
constants had been correct for a ceiling of 16 workers and stopped being correct
at 32:

**1. The phase watchdog did not scale.** Fixing the cap alone made the run
*worse*: 32 workers reached a true 4:1 and then blew a deadline sized for 8, with
12 still live. Three assertions went red for a non-defect. `APPSMP_WATCH` is now
per-worker via `APPSMP_WATCH_FOR(nw)`.

**2. The completion check contradicted its own diagnostic.** It recognised exit
codes 1920..1923, printed *"a slow host, not a lost append"*, and then set
`codes_ok = 0` for them anyway. Deadline and failure are now counted separately,
which is what `CLAUDE.md`'s ring-3 convention asks for — deadline expiry gets its
own code *so that* a suite can decline to treat it as a defect.

This cannot hide a lost append, and that was checked rather than argued: the
byte-level assertions are untouched and still fatal. On the very next run one
worker hit its deadline, the tolerance held, **and the byte check caught a real
48-byte shortfall anyway.**

**3. The ring-3 worker deadline did not scale either.** That 48-byte shortfall
was three appends **never attempted** — one worker of 32 exhausted its 60 s while
the other 31 finished. Not a lost append and not an atomicity failure, but
`nw × ITERS × PAY` assumes every worker *attempted* every iteration, and that
stops being true once a worker times out mid-loop.

The worker cannot see how many peers it has: it is selected by role, the same
four roles serve both phases, and the count lives in the kernel. Rather than
invent a channel for one constant, `APPSMP_TSCALE` multiplies its budget —
default 1, so every existing configuration is bit-identical, and 4 for the
`smp8` diagnostic. The **ordering invariant** this exposed is now written down:
the worker's own deadline must stay below the kernel's phase watchdog, or the
phase gives up first and the precise per-worker exit code is never collected.

### A regression I introduced, stated as such

The first `APPSMP_WATCH_FOR` was a bare multiply. It scales up past 8 workers and
silently scales **down** below it: at `nw = 2` — uniprocessor, ratio 2 × 1 core —
it yields 2250 ticks where the flat constant allowed 9000, a **4× cut to the
small-configuration deadline**.

**The 4-core gate cannot see this**, because `nw = 8` there makes the buggy and
correct forms numerically identical. That is the same blindness this whole
objective is about, reproduced by the fix for it. A floor is the remedy.

Honest about what the measurement shows: uniprocessor completed the phase in
**522 ticks**, so it would have passed against 2250 as well. This was a latent
margin reduction, not an observed failure — it would have bitten on a degraded
host, which is precisely when the gate matters most. Found by reasoning about
`nw = 2`, not by running it.

### Verified

| configuration | result |
|---|---|
| `smp8-bios`, 4:1, `TSCALE=4` | **PASS** 558 / 0 / 0 ranks — 32 workers on 8 cores, effective **4:1**, file 16384 B exact, per-pattern 256/256/256/256, 837 interleave transitions, no deadline expiries |
| `make gate` uniprocessor | **PASS** 540 / 0 / 0 ranks |
| `make gate` smp2-bios | **PASS** 554 / 0 / 0 ranks |
| `make gate` smp4-bios | **PASS** 558 / 0 / 0 ranks |
| `make gate` smp4-iommu | **PASS** 571 / 0 / 0 ranks |
| `make gate-oversub` 4:1 | **PASS** 558 / 0 / 0 ranks |

Every count is **identical to v0.87**, which is the expected result: on a 4-core
host the new ceiling is never reached, so the reference configuration is
unchanged by construction. Clean build, zero warnings throughout.

**Still not fixed.** The ceiling is higher, not absent. Above 8 cores at 4:1 the
clamp bites again — and now it *says so* rather than degrading quietly, which was
already true in v0.87 and remains the backstop. `MAX_KPROC = 64` is the real wall
behind `APPSMP_MAXW = 32`, and raising it further is a decision about the whole
process table rather than about this phase.

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
- ~~**No crash injection on the CAS PUT path or the legacy `cas_free` branch**~~
  — **closed in v0.88**, §2 above. The PUT path now has an injection arm, a
  falsifier that reproduces the v0.48 corruption end to end, and a standing
  index/bitmap audit; the legacy branch is ruled **unreachable** rather than
  untested. What replaces it as open: the standing audit cannot see a dangling
  entry once the block it names has been reallocated, and the dead `g_cas_legacy`
  arms have a recommendation but not yet a deletion.
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
