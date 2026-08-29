# OutRun OS v0.87.0-metal — roadmap

Opened at `worktree-v087-init`, based on `main` at `bb4620c` (the v0.86.0 tag).
`VERSION` is `0.87.0-dev` and `KERNEL_VERSION` is `"0.87.0-dev"`; both move to
`0.87.0` / `0.87.0-metal` as step 1 of the Release Protocol, and not before.

**The v0.87.0 tag does not exist yet.** Nothing in this document is a released
claim.

---

## Baseline

v0.86.0, tag `v0.86.0` at `bb4620c`, artefact
`outrun-os-0.86.0.iso` (md5 `59b983039d27fec9b0a8d75992ace0c7`), which passed
`release-verify` at 45 suites / 0 failing / 0 rank faults.

Six-tier gate on image `fc4421e2c3de94384090a3f007ec17f9`:

| tier | passed | failed | rank faults |
|---|---|---|---|
| uniprocessor | 534 | 0 | 0 |
| smp2-bios | 548 | 0 | 0 |
| smp4-bios | 552 | 0 | 0 |
| smp4-iommu | 565 | 0 | 0 |
| `gate-dirty` (3 boots, one image) | 0 failing, empty diffs | | |
| `gate-dirty-smp` (3 boots, `-smp 4`) | 0 failing, empty diffs | | |

A later uniprocessor boot at **537** carries the three generation assertions
added after that gate; this cycle's own control boot is recorded at the bottom.

What that baseline does **not** cover, repeated here because a baseline whose
gaps are invisible is how "verified" drifts from "measured": no bare metal, no
Proxmox, no soak or repeat beyond the boots above, and one boot per fresh
configuration — so it cannot see an intermittent below roughly 1 in 10 boots.

---

## Objectives

### 1. Durable generation, if something else also wants the format break

v0.86 shipped `g_cas_gen[]` derived in RAM because the spike found no room on
disk and the shortfall was not marginal — 32 bytes needed against the 4 in
`dirent.reserved[]`, and `cas_islot` is 16 bytes *because* 512/16 = 32 slots.

The standing recommendation is unchanged: **do not spend a volume-format break
on a diagnostic.** If v0.87 grows a feature that needs the break anyway, the
durable generation rides along and the cost is paid once. If nothing does, this
stays open, and that is the correct outcome rather than a deferral to apologise
for.

### 2. Crash injection on the paths that still have none

`CRASH_INJECT_COMMIT_FAIL` covers the directory journal and the CAS free path.
It does not cover:

- the **CAS PUT path**, which has always had the consistent ordering
  (`bm_alloc` before `cas_journal_write`) and has therefore never been
  interrupted under test;
- the **legacy pre-journal branch** of `cas_free`, reached when `g_cas_legacy`
  is set.

"Has always been correct" is an argument, and v0.86 is the milestone that
demonstrated what those are worth — see the journal interrupt claim below.

### 3. A gate tier above 2:1 oversubscription

`APPSMP_OSRATIO` exists and 4:1 was measured once, by hand: 16 workers on 4
cores, 8192 B exact, 441 interleave transitions against 149 at 2:1. **No gate
tier uses it**, so the ratio is tunable but undefended — exactly the shape of
gap that made the hardcoded `2` worth removing in the first place.

### 4. Falsifiers for the three unproven journal-irq guards, or an honest ruling

Of the four checks at `vj_publish()`, only the premise guard's counter has been
observed producing both verdicts (0 and 9). Detection, positive control and byte
integrity are regression guards that have never been watched failing, and v0.86
names them as unproven rather than describing them as verified.

Either give them falsifiers or rule explicitly that a regression guard does not
need one. Both are defensible; leaving it unstated is not.

---

## Carried debt from v0.86

### A. In-RAM CAS generation: persistence bounds across warm reboots

**What is true today.** `g_cas_gen[]` is zeroed by `cas_refs_rebuild()`, which
every path that establishes a directory already calls — `cas_mount()` and
`cas_format()`. The generation, the bump tally and `g_cas_blocks_freed` are all
reset together, deliberately, so the synchronisation equality is a statement
about *this mount* rather than about boot history the table no longer holds.

**The bound, stated precisely.** The table can only ever describe the interval
since the last mount. A warm reboot re-mounts, so:

- corruption introduced in boot *N* is invisible to the sweep in boot *N+1*;
- the `gate-dirty` tiers reuse one image across three boots and therefore
  exercise the *reset*, not the *retention* — they confirm the table rebuilds
  consistently, which is a different claim from the table surviving;
- a leak that only manifests across a reboot boundary is out of reach of both
  the generation table and the v0.85 TALLY sweep, which shares this limitation
  for the same reason.

**What an audit needs to establish**, and what would make it non-vacuous:

1. That the reset is *complete* — no stale generation survives a remount. The
   dirty tiers already remount; an assertion that `sum(gen) == 0` immediately
   after `cas_refs_rebuild()` and before any free would state it directly
   instead of leaving it implied.
2. That the reset is *intended and not merely observed*. A falsifier that skips
   the zeroing should make a dirty-boot sweep report a non-zero `sum(gen)` with
   zero frees — an arithmetically impossible state that no correct build can
   produce.
3. Whether anything is actually **lost** by the reset. The honest answer may be
   "nothing that the refcount rebuild does not already re-derive", in which case
   the bound is a documentation item rather than a defect. That conclusion needs
   measuring, not assuming — which is the whole lesson of v0.86.

**Explicitly not proposed:** persisting the table by writing it to disk. That is
objective 1, it costs a format break, and it should not be spent on a
diagnostic.

#### Outcome — points 1 and 2 done, point 3 still open

**The reset is now checked, not assumed.** `cas_refs_rebuild()` verifies its own
postcondition on every call: after the reset and before any free, `sum(gen)`,
the bump tally, wraps and `blocks_freed` must all be zero. Violations are
counted across the whole boot rather than sampled once.

**Why "once" would have been worthless.** On a fresh boot `g_cas_gen[]` is BSS
and already zero, so a *completely broken* reset still satisfies the
postcondition at first mount — the zero means "nothing to carry", not "the reset
works". Only a remount following real frees separates the two. The detection
check therefore requires `rebuilds > 1`, and that is the load-bearing assertion
of the pair.

**A count corrected.** This document's first draft said the suite remounts five
times. It does not: there are six `cas_mount()` call sites, but **three are
inside `#ifdef CRASH_INJECT_COMMIT_FAIL`** and do not compile in a default
build. A default boot performs **three** rebuilds — the boot mount plus two
in-suite remounts. Source and measurement agree on three. The assertion is
written as a lower bound (`> 1`) rather than pinned at 3, so building with the
crash-injection flag — which legitimately raises the count — does not fail a
check about something else.

**Measured:**

| build | rebuilds | dirty rebuilds | worst residue | verdict |
|---|---|---|---|---|
| clean, uniprocessor | 3 | 0 | 0 | **PASS** — 539 passed / 0 failed / 0 ranks |
| clean, `gate-dirty` ×3 boots | 3 each | 0 each | 0 | **PASS** — empty diffs, artefacts intact |
| `-DCAS_GEN_SKIP_ZERO` | 3 | **2** | **391** | **FAIL**, as designed |

The falsifier behaves exactly as specified: **silent at first mount**, then
catching both warm remounts (residue 356, then 391). A check placed only at
first mount would have certified that broken build as correct. The residue
values are internally consistent — `sum(gen) == bumps == freed` at each dirty
rebuild — which is what an un-reset accumulating table looks like, and rules out
the numbers being unrelated corruption.

The dirty gate is the closest thing in the harness to a real warm-reboot
sequence, and the audit reports zero residue on boots 2 and 3, which reuse the
image.

**Still open: point 3 — is anything actually LOST by the reset?** This audit
establishes that the reset is complete. It does not establish that discarding
the table costs nothing, and the plausible answer remains "nothing the refcount
rebuild does not already re-derive". That needs measuring rather than assuming,
and it is the part of item A that carries forward.

### B. POSIX tmp `SEEK_END` metadata boundary audit

**Do not re-litigate the memory question.** v0.86 was asked to fix
"uninitialised memory disclosure past true EOF" on tmp and found no such defect:
`tmp_read_range` clamps to `flen`, `tmp_write_at` zero-fills the hole,
`tmp_truncate` zeroes on grow *and* shrink, and both requested assertions
already existed (**1682 / 1684**, and **1706** checking the gap bytes). No diff
was written, on purpose.

**The real item is narrower and is a design question, not a bug.** `SEEK_END` on
an *inherited* tmp descriptor reports that file's **length** to a caller who may
no longer read its contents. That is POSIX-correct — permissions are checked at
`open()`, not per operation — and the tree asserts it deliberately (**1793**,
*"lseek must not be permission-checked"*).

**What the audit should establish**, since "matches the root volume" has been
asserted more often than measured:

1. **Exactly which metadata an unprivileged holder of an inherited tmp
   descriptor can obtain.** Length via `SEEK_END` is known. `SYS_FSTAT` on the
   same descriptor is the open question — if it returns mode, uid, gid or
   timestamps, the surface is wider than the one sentence in KNOWN-NOT-FIXED
   admits, and the entry is understated rather than wrong.
2. **Whether root-volume parity actually holds.** The claim is that tmp
   discloses no more than the root volume does through the same inherited-fd
   path. It is plausible and it has not been tested side by side.
3. **A ruling, written down.** Either the disclosure is acceptable POSIX
   behaviour and the entry says so with its surface enumerated, or tmp
   descriptors are declared to be not-quite-POSIX and `lseek`/`fstat` gain a
   check — which breaks assertion 1793 and must be done deliberately, at the
   level of "tmp stops behaving like a POSIX descriptor", never as an incidental
   fix.

The likely outcome is (1) plus a more precise KNOWN-NOT-FIXED entry. That is a
real deliverable: an accurately bounded known issue is worth more than a vague
one, and this one is currently vague.

### C. Untracked worktree: `.worktrees/v0-86`

Checked at cycle open. **Not created by the v0.86 work** — that cycle used
`.claude/worktrees/`, which is the convention this repository and its tooling
expect. This one sits at `.worktrees/`, outside it, which is why it shows as
untracked in `git status` on `main`.

Measured state:

| property | value |
|---|---|
| path | `.worktrees/v0-86` |
| branch | `V0.86` |
| commit | `a49bee6` — *docs(changelog): record the v0.85.0 artefact…* |
| merged into `main`? | **yes** — `a49bee6` is an ancestor of `bb4620c` |
| unique commits | **none** |
| uncommitted changes | **none** (`git status --porcelain` empty) |

**It holds no work that is not already on `main`, and nothing uncommitted.** It
is therefore safe to remove:

```
git worktree remove .worktrees/v0-86 --force
git worktree prune
git branch -d V0.86
```

**Left in place deliberately.** This cycle did not create it, and removing
another session's or another person's worktree is not a decision to take
silently — the state above is recorded so the removal is a one-line
confirmation rather than an investigation. If it is still present at the v0.87
release, this entry should be revisited rather than copied forward again: a
carried item that is only ever carried is a habit, not a record.

---

## Control boot for this commit

The version bump changes `KERNEL_VERSION`, which is compiled in, so the v0.86
kernel ELF md5 does **not** describe this tree. A fresh boot is what verifies it,
rather than inheritance from the release.

```
uniprocessor  OK   suites=45  passed=537  failed=0  ranks=0  (320s)
FRESH-IMAGE MATRIX: PASS
image  7a41d8b5aebea6ef0527fd5acbb8f1e9
```

537 assertions matches the v0.86 post-gate count exactly, which is the expected
result: this commit changes two version strings and adds a document, so a
different count would mean something unintended had moved.

The boot banner reads `bare-metal kernel 0.87.0-dev` — the build-level
`release-version-check` says the macro is right, and this says the string
actually reached ring 3 and the console. Those are different claims, and v0.75.0
is the reason this tree checks both.

Clean build, zero compiler warnings. Artefact named `outrun-os-0.87.0-dev.iso`.

Kernel ELF md5 `5d33b479e5de7ad2adf5e6ce1bf2ac53` — the artefact that identifies
the code, per the v0.86 finding that `grub-mkrescue` timestamps make the ISO md5
unable to answer "is this the same code".

**Not covered by this boot:** the other five tiers, dirty-volume reuse, bare
metal, and any intermittent below roughly 1 in 1 boot. It is a cycle-open sanity
check, not a gate.
