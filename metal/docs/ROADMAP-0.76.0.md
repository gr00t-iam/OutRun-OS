# OutRun OS v0.76.0-metal — roadmap

Milestone 76. v0.75.0 is tagged (`74fd9b3`). This milestone is defined by what
that tag deliberately did **not** claim, recorded at the end of
`ROADMAP-0.75.0.md` and carried here verbatim in substance.

v0.75 was a milestone about **identity** — a slot index is not an identity, a
pid is not a slot, a socket handle is not a socket, a descriptor is not a
binding. v0.76 is a milestone about **evidence**: three of the things v0.75
shipped are believed rather than demonstrated, and one of them is the headline
fix of the milestone before it.

---

## THE CARRYOVER, WITH WHAT IS ALREADY KNOWN

### 1. The suite set is not idempotent across boots on a re-used volume

**Evidence.** Boot any v0.75 kernel twice on one disk image. First boot:
45 suites, 0 FAIL. Second boot, same image:

```
[vfsstrs]   RESULT: 18 passed, 1 failed
[usersstrs] RESULT: 16 passed, 2 failed
```

Three assertions, named exactly:

```
[vfsstrs]   FAIL  VFS journal commit is genuinely DEFERRED (on-disk dir region is stale before apply)
[usersstrs] FAIL  a newly created file gets the default mode
[usersstrs] FAIL  a stranger CAN open another user's 0644 file for reading
```

Confirmed **pre-existing** by negative control: the merged `main` kernel from
before the persistence branch produces the identical three. This is not a
regression from v0.75 — it is something v0.75 was the first thing to look for.

**Root cause, already located — and these are TEST defects, not kernel
defects.** Both suites assume they are creating their fixtures for the first
time:

- `usersstrs` (kernel64.c ~19604) does
  `vfs_open_for("m72own", alice, 1)` with `creat=1`, then asserts the file has
  `VFS_MODE_DEFAULT` and is readable by a stranger at 0644. On a re-used volume
  `m72own` already exists **carrying the mode later assertions in the same
  suite changed it to**, so "newly created" is false and the mode is no longer
  the default.
- `vfsstrs` (kernel64.c ~17713) proves the journal is deferred with
  `deferred_ok = (praw->used == 0 || praw->file_hash != DENTS[idx].file_hash)`.
  On a re-used volume the on-disk dirent for `vfs-crash-test` already exists
  from the previous boot **with the same content and therefore the same
  hash** — content addressing makes this deterministic, not lucky — so the
  "on-disk is stale" precondition cannot hold and the assertion is defeated by
  its own fixture.

**Why it is now urgent rather than cosmetic.** v0.75 shipped persistence. A
volume with prior state is, as of the tag, a **supported configuration** — it is
the entire point of step 6. Yet the suite set has never been run that way, and
cannot currently pass when it is. Worse, the two-boot dirty-volume run is *the
only configuration that can validate persistence at all*: every gate harness to
date builds a fresh image per boot, so a green gate says nothing about the
feature v0.75 closed on.

**A trap to design around.** Not every cross-boot artifact is contamination.
`vfs-reboot-test` (v0.48) and `udbreboot` (v0.75) exist **precisely** to survive
a reboot and are the proofs that recovery and persistence work. A blanket
"delete all fixtures at boot" would destroy the only cross-boot evidence in the
tree. The fix must distinguish *fixtures a suite owns and must reset* from
*artifacts that are deliberately durable*.

**Definition of done.** A `dirty-volume` gate configuration — boot twice on one
image, both boots 0 FAIL — added to the matrix and green.

### 2. Toolchain suites' wall-clock budgets break on a loaded host

**Evidence** (from v0.74/v0.75 observation, at ~6x CPU oversubscription):

```
[langstrs] 8 passed, 2 failed        (exit 970 — the compile-run-validate step)
[toolstrs] ... TIMED OUT waiting for the compiler
[pipestrs] ...
```

**Nature.** These are wall-clock budgets in the self-hosting suites, not
correctness failures. But they are what breaks a regression run on a busy
machine, and this environment is TCG-only — there is no KVM to absorb the
variance. A gate that fails under load is a gate people learn to re-run until it
passes, which is how a real failure gets waved through.

**Definition of done.** No suite fails because the host was busy. Preferred
shape: assertions wait on a *condition with an unbounded-but-observable* wait
(progress-based), or a budget scaled from a measured baseline taken during the
same boot, rather than a constant chosen on an idle machine. A suite that must
give up should say "gave up after N" distinctly from "got the wrong answer" —
today they are the same red line.

### 3. Tier 1 step 3 — the fork race has never been reproduced uninstrumented

**This is the most important unfinished item in the project.**

v0.74 found an intermittent `posixstrs` failure under `-smp 4`: a forked child
reported the wrong parent through `getppid` (exit 44) and the parent's `waitpid`
then timed out (exit 702). v0.75 read the path, found three real defects (A:
non-atomic `g_next_pid`; B: `ppid_slot` with no generation; C: identity syscalls
using raw slots), and fixed all three. Every configuration is now green.

**But:** the original failure was never reproduced *without* instrumentation, so
the clean runs do not distinguish **"fixed"** from **"did not fire"**. Twelve
clean runs were also obtained with the fix reverted. The three defects are real
and were fixed on their own merits — each is provable by reading — but the causal
link from them to the observed symptom remains an argument, not a measurement.

This is the same epistemic hole this project has repeatedly proven costly. In
v0.75 alone: a hypothesis was shipped and later disproven by measurement; a
20-boot "verification" was run against the wrong ISO and its conclusion had to
be withdrawn; a counter was grepped for that no code could emit. Every one of
those was caught by insisting on evidence. This item is the same insistence,
applied to the milestone's own headline fix.

**Definition of done — one of these two, explicitly:**

- **(a) Reproduce it.** Find a configuration that fires the original symptom on
  an unfixed kernel, then show the fixed kernel clean on the same configuration.
  The most likely levers, per v0.74: a slower host, higher oversubscription, a
  different binary layout, or an artificially widened window (a deliberate delay
  between `kproc_unlock()` and the pid assignment on a *reverted* build, used
  only as a reproducer).
- **(b) Retire the claim honestly.** If it cannot be reproduced after a bounded
  effort, say so in the changelog in those words — "three real defects were
  fixed; the causal link to the v0.74 symptom is unproven" — and stop carrying
  it as an open verification item. What is not acceptable is carrying it
  silently into a third milestone.

---

## TIER 1 — VERIFICATION INTEGRITY

The gate must mean something before anything is built on top of it.

1. **Suite idempotency + a dirty-volume gate configuration.** Carryover 1.
2. **Load-tolerant timing in the toolchain suites.** Carryover 2.
3. **Fork-race causality: reproduce or retire.** Carryover 3.
4. **A gate that states its own coverage.** Each harness already stamps the md5
   of the image it booted — a habit adopted mid-v0.75 after a 20-boot run was
   discovered to have booted the wrong ISO. Extend it: every gate run should
   emit, in one line, which configurations ran, how many boots each, and what
   was *not* covered. A gate whose gaps are invisible is how "verified" drifts
   away from "measured".

## TIER 2 — ARCHITECTURE

1. **A memory-hard KDF.** PBKDF2-HMAC-SHA-256 (c=4096) buys serial CPU cost
   only; GPUs and ASICs keep a large advantage. Argon2id or scrypt over the
   existing primitive. The v0.75 structure was built so this is a change to
   `udb_kdf()` and nothing else — that claim held once already for the FNV-1a →
   PBKDF2 swap and should hold again. Land the primitive against published
   vectors as its own verified unit *before* wiring it in, exactly as SHA-256
   was.
2. **Password change and account deletion syscalls.** Today there is no way to
   change a password from ring 3 — which means the revert-to-an-old-password
   case that the per-segment nonce defends against **is not yet reachable**. The
   defence shipped ahead of the path that needs it; this closes the gap and
   makes the defence testable end to end.
3. **Lockout state across a reboot, without a new side channel.** v0.75
   deliberately did not persist failure counters: they change on every failed
   authentication, so storing them naively makes each failed login a visible
   write, and an observer counting writes counts failed logins. Wanted: lockout
   that survives a reboot without publishing authentication failures through
   write traffic. Likely shapes — a fixed-cadence write regardless of outcome,
   or coarse buckets that only occasionally change state. This is a design
   problem, not an implementation one, and should be designed before it is
   scheduled.
4. **Confidentiality for the stored database — scoped, or explicitly deferred.**
   There is no key store, no TPM, and nothing to encrypt the image with that
   does not live on the same volume. Absent a key story this is not
   implementable, only theatre. Recommend: **deferred**, restated in the
   changelog so it is not mistaken for an oversight.

---

## HIGHEST PRIORITY, AND THE FIRST BRANCH

### Recommendation: carryover 1 — suite idempotency on a re-used volume

Not because it is the deepest problem. Carryover 3 is. Because it is the one
that **blocks measurement of something already shipped**, and it is cheap:

- v0.75 shipped persistence, so a volume with prior state is a supported
  configuration as of the tag. The suite set cannot currently pass on one.
  Shipping a persistence feature while being unable to boot twice on a disk
  without failures is an inconsistency inside the release itself.
- The two-boot dirty-volume run is the **only** configuration that can validate
  persistence. Until it is green, step 6 is guarded by one hand-run test.
- Both root causes are already located, and both are test defects — bounded,
  well-understood work, not research.
- It produces a new gate configuration, which then guards everything after it —
  including carryover 3, which will need to run many boots and compare
  configurations that differ only in kernel.

Carryover 3 is the milestone's headline and should be second, once there is a
gate worth trusting to measure it with. Carryover 2 pairs naturally with 3,
since reproducing an SMP race is likely to involve deliberately loading the
host — which is exactly the condition that currently breaks the toolchain
suites, and would otherwise confound the experiment.

### First branch: `v076-suite-idempotency`

**Step 0 — a dirty-volume harness, as a first-class script.**
Boot N times on one image, report per-boot suite results and diffs between
boots. Every existing harness recreates the image; this one must not. Stamp the
ISO md5 into every log, as all v0.75 harnesses now do.

**Step 1 — characterise before fixing.** Run 3 consecutive boots and enumerate
*every* assertion whose result differs between boot 1 and boot 2, and between
boot 2 and boot 3. **Do not assume the count is three.** The known three come
from a two-boot run; a third boot may expose more, and a suite that fails on
boot 2 may mask a different failure on boot 3. This step produces the actual
work list. It is also the step that would catch a fourth failure that a fix for
the first three would otherwise hide.

**Step 2 — decide the hygiene policy, once, and apply it uniformly.**
Two candidate policies, and the choice should be explicit:
  - *Own-and-reset*: each suite deletes/recreates its fixtures at entry. Matches
    what `authstrs` already does for the user database as of v0.75.
  - *Unique-per-boot names*: fixtures carry a boot-unique suffix.
  Own-and-reset is preferred — it keeps names stable in logs and does not grow
  the directory on every boot (the VFS directory has a fixed slot count and has
  run out before).
  **Constraint:** `vfs-reboot-test` and `udbreboot` are deliberately durable
  cross-boot evidence and must be exempt. The policy needs an explicit
  allow-list, not a blanket sweep.

**Step 3 — fix the three known assertions**, each addressed at its cause:
  - `usersstrs`: reset `m72own` (unlink then create) so mode and ownership are
    established by this boot, not inherited.
  - `vfsstrs`: the deferred-journal proof needs a fixture whose on-disk state is
    genuinely stale. Since content addressing makes identical content produce an
    identical hash, the fixture content must differ per boot — a counter or the
    tick value folded in — or the test must remove the dirent first.

**Step 4 — add `dirty-volume` to the gate**: two boots on one image, both
0 FAIL, alongside UP / smp4-bios / smp4-iommu. Then re-run the full matrix.

**Definition of done for the branch:** three consecutive boots on one volume,
0 FAIL each, with the cross-boot artifacts (`vfs-reboot-test`, `udbreboot`)
still doing their job — verified by the `CROSS-BOOT OK` line still appearing,
not merely by absence of failures.

### Explicitly out of scope for v0.76

Carried from v0.75 so they are not rediscovered: an execute permission bit,
directory permissions, supplementary groups, a login program, lockout expiry and
administrative unlock. The queued TCP hardening work (congestion window, slow
start, fast retransmit, Karn/Jacobson RTO, segment coalescing) remains
independent of all of the above.

---

## STEP 1 RESULT — DIRTY-VOLUME CHARACTERISATION

Run before any fix, as the plan requires. Harness: `dirty3.sh` — one 4 MB image
created once and **reused** across three consecutive boots, uniprocessor for
determinism, capturing every `FAIL` assertion and every `RESULT` line per boot
and diffing consecutive boots in both directions. ISO `rel.iso`
(md5 `c5fd5fdf8021223b7701470d8ab4c5d1`); confirmed to be the current `main`
kernel by checking that no source under `metal/{kernel,user,rust,cpp,boot}`
changed since it was built.

### The complete list

```
boot 1  OK  suites=45  failing-suites=0  failing-assertions=0   (205s)
boot 2  OK  suites=45  failing-suites=2  failing-assertions=3   (190s)
boot 3  OK  suites=45  failing-suites=2  failing-assertions=3   (185s)
```

Exactly **three** failing assertions across **two** suites, and they are the same
three on boot 2 and boot 3:

| # | suite | assertion |
|---|-------|-----------|
| 1 | `usersstrs` | a newly created file gets the default mode |
| 2 | `usersstrs` | a stranger CAN open another user's 0644 file for reading |
| 3 | `vfsstrs`   | VFS journal commit is genuinely DEFERRED (on-disk dir region is stale before apply) |

```
[usersstrs] RESULT: 18 passed, 0 failed   ->  16 passed, 2 failed
[vfsstrs]   RESULT: 19 passed, 0 failed   ->  18 passed, 1 failed
```

### The finding that mattered: it converges after one boot

The `boot 2 -> boot 3` diff is **empty in both directions** — no new failures, no
failures that disappeared, no RESULT line changed. The volume reaches a fixed
point after the first dirty boot.

This is the question step 1 existed to answer. The plan warned not to assume the
count was three, because the known three came from a two-boot run and a suite
failing on boot 2 could mask a different failure on boot 3. **It does not.** The
work list is closed at three, and a fix for these three can be verified without
worrying that it merely uncovers a fourth.

It also means the defect is **deterministic, not accumulative**: prior state
either exists or it does not, and a second application of it changes nothing.
That is consistent with both root causes being "a fixture already exists"
rather than anything that grows per boot.

### Root causes (located before the run, confirmed by it)

Both are **test defects, not kernel defects**.

1 & 2 — `usersstrs`, kernel64.c ~19604. `vfs_open_for("m72own", alice, 1)` with
`creat=1`, then asserts `vfs_mode_of(...) == VFS_MODE_DEFAULT` and that a
stranger can open it at 0644. On a re-used volume `m72own` already exists,
carrying the mode **this same suite's later assertions changed it to**, so
"newly created" is false and the mode is no longer the default. Two assertions
fall out of one stale fixture.

3 — `vfsstrs`, kernel64.c ~17713.
`deferred_ok = (praw->used == 0 || praw->file_hash != DENTS[idx].file_hash)`.
On a re-used volume the on-disk dirent for `vfs-crash-test` already exists from
the previous boot with the same content and therefore **the same hash** —
content addressing makes this deterministic, not lucky — so the "on-disk is
stale" precondition cannot hold and the assertion is defeated by its own
fixture.

### A gap in this run, stated rather than glossed

The harness reports the two deliberately durable artefacts as absent in all
three boots:

```
boot 1: udb=0 vfs-reboot-test=0
boot 2: udb=0 vfs-reboot-test=0
boot 3: udb=0 vfs-reboot-test=0
```

That is **not** evidence that they survive. It means they were never created:
both require a manual command at the prompt (`udbpersist`, `vfscrashwrite`) and
this harness types nothing. So the constraint the plan flagged — that a fixture
reset must not destroy the only cross-boot evidence in the tree — is **still
untested**.

Establishing that baseline is a prerequisite for step 2, not an afterthought: a
fixture-reset policy could delete `vfs-reboot-test` and `udbreboot` and every
run above would still look identical, because neither was present to lose. The
next run must create both in boot 1 and assert they are still there in boots 2
and 3, **before** any reset policy is written.

### Not yet characterised

- **`-smp 4` on a dirty volume.** This pass was uniprocessor on purpose, to keep
  the answer deterministic. The SMP dirty-volume failure set may be a superset;
  it has not been measured, and the dirty-volume gate configuration should
  eventually cover both.
- **More than three boots.** Convergence is established between boots 2 and 3;
  a longer run has not been done and is probably unnecessary given the fixed
  point, but it is not proven beyond three.

---

## STEP 2/3 RESULT — FIXED, AND VERIFIED AGAINST A REAL BASELINE

### The durable-artefact baseline that step 1 was missing

Step 1 recorded a gap: it reported both cross-boot artefacts absent in every
boot, which is not evidence they survive — they were never created, because both
need a manual command at the prompt and the harness typed nothing.

`dirty3.sh` now types them in boot 1: `udbpersist`, then `vfscrashwrite`. Order
matters — `vfscrashwrite` halts the machine forever (`cli; hlt`) by design,
simulating power loss, so it must be last.

**Baseline, UNFIXED kernel, three boots on one image:**

```
boot 1: 0 failures | creates both markers
boot 2: 3 failures | udbreboot detected (gen 5), vfs-reboot-test found + VERIFIED
boot 3: 3 failures | udbreboot detected (gen 9), vfs-reboot-test found + VERIFIED
```

Both artefacts demonstrably survive dirty boots *before* any reset policy
exists. That is what makes "they still survive after the fix" a real comparison
rather than a vacuous one.

### The fixes

**`usersstrs`** — one stale fixture was failing two assertions. Reset `m72own`
before creating it, so "newly created" is true again.

**`vfsstrs`** — fixture content is now unique per boot. The assertion proves
deferred apply by showing the on-disk dirent does not yet match the in-memory
one; with fixed content on a re-used volume the on-disk dirent already holds
that exact payload, and content addressing makes an identical hash follow
*deterministically*. Varying the payload restores the precondition **without
touching content addressing** — a hash that differs because the content differs
is exactly what the CAS should produce.

`rdtsc`, not `g_ticks`: two boots reach that line at a similar tick count, so
`g_ticks` would be *usually* unique, and a fixture that is usually unique
reintroduces this failure as a **flake** — strictly worse than the deterministic
failure it replaces.

**The allow-list is enforced, not documented.** `suite_fixture_reset()` refuses
`vfs-reboot-test`, `/etc/udb.a`, `/etc/udb.b` and `udbreboot` out loud and counts
refusals. Mechanical because the failure mode is silent: a blanket sweep would
delete the only cross-boot evidence in the tree, every run would still look
green, and the evidence would be gone while we believed we had cleaned up.

### Verification

**Dirty volume, fixed kernel, three boots on one image:**

```
boot 1  OK  45 suites  0 failing assertions  0 resets   (fresh: nothing to reset)
boot 2  OK  45 suites  0 failing assertions  1 reset    0 refusals  both artefacts survive
boot 3  OK  45 suites  0 failing assertions  1 reset    0 refusals  both artefacts survive
```

Consecutive-boot diffs empty in both directions. The reset fires only when there
is something to reset, and `vfsstrs` passes with **zero** resets — consistent
with its fix being content variation rather than deletion. That is how we know
the two fixes each do their own work rather than one masking the other.

**Fresh-image gate (no regression to the normal path):** uniprocessor 45/0;
`-smp 4` 10 OK / 0 HANG / 0 PANIC, twice — 20 boots total, 0 rank faults.

### An anomaly, recorded rather than explained away

The first fresh-image gate run had **one** failing suite inside one boot:

```
[mcpre  ] FAIL  long probe never started on cpu1
[mcpre  ] RESULT: 0 passed, 1 failed
```

Checked rather than assumed: across **277 boot logs** on this host, `[mcpre]`
has failed exactly once, and that once is this run. So it cannot be dismissed as
known-flaky on history alone.

It did **not** reproduce: a confirmatory 10-boot run is 0 failures, and the same
suite passed in the other 9 boots of the run that caught it. Total on the fixed
kernel: **1 `[mcpre]` failure in 20 `-smp 4` boots.**

Constructing a mechanism from this branch's changes is hard — two suite fixtures
and a helper called only from `usersstrs`, none of it in the scheduling or IPI
path. But "hard to construct" is not evidence, so the finding is:
**unreproduced, unexplained, and not claimed to be pre-existing.**

It belongs to **carryover 2**: a wall-clock assertion on a TCG-only host, in a
session where QEMU runs and kernel builds had been contending for the machine
throughout. That is precisely the class the roadmap describes as "fails because
the host was busy, and is indistinguishable from a real failure" — and it is a
concrete instance to aim carryover 2's work at. Failing log preserved at
`mcpre-evidence/smp-6.log`.

### Still not done for carryover 1

- **`-smp 4` on a dirty volume** remains unmeasured. This branch verified the
  dirty-volume path uniprocessor (for determinism) and the SMP path on fresh
  images. The combination is the one square of the matrix still empty.
- **The `dirty-volume` gate configuration is not yet wired into the gate.** The
  harness exists and passes; making it a standing configuration is the remaining
  step before carryover 1 can be called closed.
