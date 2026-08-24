# OutRun OS v0.85.0-metal — roadmap

Milestone 85, opened at `9b0c4d5`. `VERSION` reads `0.85.0-dev` and
`KERNEL_VERSION` reads `0.85.0-dev`; **no v0.85 tag exists.**

The `-dev` suffix is not decoration. It propagates into the artefact name, so a
development image is `outrun-os-0.85.0-dev.iso` and cannot be confused with the
published, checksummed `outrun-os-0.84.0.iso` by filename alone. Between the
v0.84.0 tag and this commit the tree carried `VERSION := 0.84.0` while sitting
three commits past the tag — every local build produced a file named exactly
like the release artefact and byte-different from it. That window is what this
cycle-opening commit closes.

`make release-version-check` will WARN for the whole cycle: its git half
compares `git describe --tags --abbrev=0` (`v0.84.0`) against `v$(VERSION)`
(`v0.85.0-dev`) and has no notion of a dev suffix, unlike the banner half which
v0.81 taught to expect one. The warning is the expected state until v0.85.0 is
tagged. Teaching the git half about `-dev` is itself a candidate for this
milestone — a check that cries wolf all cycle is one people learn to scroll
past, which is the reasoning v0.81 already applied to the other half.

---

## GOALS

v0.84 finished what v0.82/0.83 started: the VFS learned what a file POSITION
means, and v0.84 took the consequences to completion — writes at any size,
storage that comes back, an ownership model on the scratch volume, `ftruncate`
and `O_APPEND`.

What that leaves is a filesystem whose FILES are well served and whose
DIRECTORY is not. A name cannot be changed, a mode cannot be changed after
creation, and nothing records when a file was written. Those are the operations
a shell, a build system and a text editor reach for first, and this tree now has
all three running in ring 3.

The second goal is a performance defect this project measured rather than
guessed at, and which now has a number attached to it.

## PLANNED FEATURES

### 1. VFS metadata operations — `rename`, `chmod`, timestamps

- **`rename(old, new)`.** The directory is a flat array of `struct dirent` with
  an inline name, so a rename is a name overwrite plus a journal commit and does
  NOT touch the chunk map — no content moves, and the CAS reference counts are
  untouched by construction. The cases that need deciding are the ones POSIX
  spends its words on: renaming onto an EXISTING name must replace it
  atomically, and the replaced file's blocks must be released exactly as an
  unlink releases them (`vfs_release_map_locked`), or rename becomes a way to
  leak storage that unlink cannot.
- **`chmod` / `chown`.** ~~What is missing is any way to CHANGE them after
  creation.~~ **That was wrong.** `SYS_CHMOD` (92) and `SYS_CHOWN` (93) have
  existed since v0.72, complete with owner-or-root for chmod, root-only for
  chown, ENOENT and a tmp/dev refusal — and the v0.85 journal work already
  routed them through `vfs_journal_commit_idx(di)`.

  The actual defect, found by the Phase 1 audit, is the CREDENTIAL they read:
  both consult `kprocs[L].uid`, the REAL uid, while every other VFS check uses
  `cred_euid()`. `vfs_permit`'s own header says why that is wrong — passing the
  real pair "would make setuid() cosmetic". Because `SYS_SETEUID` moves only the
  effective id, a process that has dropped effective privilege keeps real uid 0
  and therefore keeps the authority to chmod ANY file on the volume, while a
  genuinely setuid-root program (real 1000, euid 0) is refused chown. The fix is
  two lines; the value is in the test that proves it, which does not exist.

  Neither syscall has ANY coverage, kernel-side or ring-3. The assertion reading
  "after chmod 0600 the stranger can no longer open it at all" assigns
  `DENTS[di].mode` directly and never calls chmod — true, and misleadingly
  named. See `OUTRUN-0.85-vfs-metadata-recon.md`.
- **`st_mtime` / `st_atime`.** `SYS_STAT` exists and reports no time at all.
  `g_ticks` at 100 Hz is the only clock this kernel has, so these are
  boot-relative and must be documented as such rather than presented as wall
  time. The honest version is a monotonic tick stamp; the dishonest version is a
  fabricated epoch.

**VOL_TMP parity is part of the feature, not a follow-up.** v0.83 and v0.84 both
spent effort making the two volumes agree about what an operation means, and a
metadata call honoured on one volume and not the other reopens exactly that gap.
Note that tmp has no mode and no group by design (owner-or-root is the whole
rule), so `chmod` on a tmp file has to refuse rather than pretend.

### DONE — measured

`7eeab2e` (credential fix) and the commit that follows it, plus the Phase 1
audit `5c446fb`.

**A live authority leak, fixed.** `chmod`/`chown` judged by the REAL uid while
every other VFS check used the effective one, so a process that had dropped with
`seteuid(1000)` was refused every write by `vfs_permit` and could still re-mode
ANY file on the volume. Four tagged releases carried that, with no test — the
assertion that appeared to cover it assigned `DENTS[di].mode` directly and never
called the syscall.

**`SYS_RENAME` (102).** The namespace is FLAT, so a "cross-directory move" and a
"same-directory rename" are the same operation: one dirent's inline name is
overwritten, no content moves, and the CAS reference counts are untouched by
construction. The case that is genuinely two dirents is renaming ONTO AN
EXISTING NAME, which must replace atomically and release the replaced file's
blocks — measured at `used_blocks 1122 -> 1120`, so the storage really comes
back rather than the return code merely saying so.

**`vfs_journal_commit_multi(idx1, idx2)`.** Both shadows, then one header: 3
writes when the dirents sit in different blocks, 2 when they share one, 2 when
`idx2` is -1. Committing the two indices in turn would NOT be atomic — the first
header names only the first block, and a crash between the two leaves the entry
renamed and the target not yet removed, i.e. two live dirents carrying one name.

**Timestamps.** `mtime`/`atime` carved from `reserved[12]`, leaving 4 bytes and
holding the 256-byte static assert. Boot-relative 100 Hz ticks, documented as
such — there is no RTC, and calling them seconds-since-1970 would be inventing a
number. Zero means UNKNOWN, following v0.72's rule for `mode`, so a file on an
older volume does not appear to have been touched at startup.

Two deviations from the brief, both deliberate:

- **`SYS_STAT` was NOT widened.** `omake.oc` allocates exactly two 64-bit words
  and the kernel `cmemcpy`s `sizeof(st)` into it, so growing that struct would
  write 16 bytes past a ring-3 buffer in a program `occ` compiles during the
  boot. Timestamps arrive through a new `SYS_FSTAT` (103) instead, which is also
  what fstat properly means. Same reasoning the v0.58 comment gives for not
  widening `SYS_READDIR`.
- **`atime` is lazy and never journalled.** Committing on every read would cost
  2 block writes and a publish for an operation that changed no content, making
  reads as expensive as writes — which is why real systems ship `relatime`. The
  stamp reaches disk with the next commit that dirties the block, so a crash can
  lose it. A stated trade, not an oversight.

**Coverage: all 10 Phase 1 cases implemented.** +7 assertions on every tier in a
normal build, +6 more in the crash-injection build.

| tier | before | after |
|---|---|---|
| uniprocessor | 513 | **520** |
| smp2-bios | 527 | **534** |
| smp4-bios | 531 | **538** |
| smp4-iommu | 544 | **551** |
| gate-dirty / gate-dirty-smp | 0 failing | **0 failing**, empty diffs |

Six tiers on one image, `cf842dc0876dc95d2b2aa7e888c0e709`, `ranks=0`
throughout, clean build with no warnings.

**Falsifiability.** `EXTRA=-DCHMOD_REALUID_REPRO` restores the real-uid check:
exactly one assertion moves, and its decode names the defect —
*"an effectively-unprivileged caller CHANGED THE MODE of root's file (chmod is
judging the real uid, not the effective one)"*, worker exit 1972.
`EXTRA=-DCRASH_INJECT_COMMIT_FAIL` interrupts a replacing rename and the
directory comes back with the OLD PAIR intact — the interrupted commit simply
not applied.

**Two corrections from doing the work**, both recorded because both were wrong
first:

- The crash-atomic rename test first reported `fired=0`. The refactor that
  introduced `vj_stage_block` had left `vfs_journal_commit_idx` carrying a
  DUPLICATE staging loop, and the injection hook lived only in the old copy — so
  rename, the operation whose atomicity most needed testing, could not be
  interrupted at all. The main assertion would have passed; only the
  "did it actually fire" guard caught it. There is now one staging
  implementation and one injection point.
- A `gate-all` run failed on `smp4-bios` with `[langstrs]` red. The tier had
  taken **1615 s against 235 s** for the others, and the failure was
  `finished == false` — a drain watchdog — while the driver's own exit code was
  970, its success sentinel. A control re-run of the SAME image passed 538/0 in
  240 s. Host degradation, per the standing note in the v0.81 tag. Because
  `gate-all` stops when the fresh matrix fails, the dirty tiers had not run on
  that build and were run separately rather than cited from an earlier binary.

### 2. Single-block directory journal commit

`vfs_journal_commit()` writes the journal header plus **all** `VFS_DIR_BLOCKS`
(48) directory blocks on every VFS mutation — 49 block writes for a change that
touched one 256-byte dirent.

This is measured, not suspected. The v0.85 append harness recorded **4.67
appends per second** under four-way contention, i.e. ~230 block writes/second,
and that constant is why the gate runs 128 appends instead of the 1024 the soak
runs: 1024 appends is ~283 s, and ten gate boots could not afford it.

The shape of the fix is to journal the ONE block a mutation dirtied, with the
header naming which block it is — the CAS metadata journal already works this
way (`cjournal_header` carries `bitmap_block` and `index_block` home locations),
so the pattern exists in this tree and does not need inventing.

**What makes it delicate:** this is the crash-consistency path. Recovery
currently replays a whole-directory shadow, which is idempotent and cannot
partially apply; a per-block journal has to stay that way. Any change here needs
a crash-injection test BEFORE the change, in the shape v0.84's dedup negative
control established — write it, watch it pass against the current kernel, then
change the kernel.

### DONE — measured

`c2e04fe` (harness) and the commit that follows it (optimisation), in that
order and as separate commits, because the baseline is the point.

**The `cjournal_header` pattern turned out not to be transferable, and that
changed the design.** The CAS metadata journal is EAGER: it writes the index and
bitmap home blocks immediately after journaling them, so its journal only ever
covers one in-flight write. The VFS directory journal is deliberately DEFERRED —
nothing copies shadow to `dir_start` until `SYS_VFS_SYNC` or the next mount, and
a suite asserts exactly that (`deferred_ok`, "proves apply is deferred, not
eager"). Because apply is deferred, the shadow has to hold EVERY un-applied
change, not just the newest, which is what the whole-directory shadow was
buying. A naive single-block journal would have let commit B silently discard
commit A.

So the journal holds a SET of dirty blocks instead: the header names the
directory block living in each shadow slot, and a re-dirtied block rewrites its
own slot rather than appending a second entry. There are only `VFS_DIR_BLOCKS`
directory blocks and each takes at most one slot, so the set is bounded by
construction and cannot overflow — the worst case is every block dirty, which is
exactly the old behaviour at exactly the old cost. A static assertion fails the
BUILD if `VFS_MAXFILES` ever grows the directory past what the header can name.

**Two things had to change beyond the size of the write.**

- **The shadow is now written BEFORE the header**, which makes a commit atomic
  rather than merely survivable. Previously a crash between the two left a
  PENDING header over a half-updated shadow; now it leaves the PREVIOUS header,
  whose entries all still have intact shadows, so the incomplete commit is not
  replayed at all.
- **`vfs_open_for`'s O_CREAT path had to start journaling.** It created a dirent
  and committed nothing, and the name still reached disk because the next
  commit — whatever it was for — shadowed the whole directory and carried it
  along. That free ride ends under per-block journaling.

`VJRNL001` journals are still replayed as whole-directory shadows, so upgrading
a kernel does not discard a commit that was already durable.

**Measured.** Block writes per commit **49 → 2**, a 24.5x reduction. The
throughput gain is smaller than that ratio because the journal was the dominant
per-append cost, not the only one — each append still does its own `cas_put` and
file-hash walk.

| | before (`bf4ef8ec`) | after (`0b536c35`) | |
|---|---|---|---|
| append phase, smp4-bios | 2846 ticks | **1038** | **2.74x** |
| append phase, smp4-iommu | 2630 ticks | **972** | **2.71x** |
| appends/second, smp4-bios | 4.50 | **12.3** | |
| boot, uniprocessor | 375 s | **300 s** | |
| boot, smp2-bios | 295 s | **235 s** | |
| boot, smp4-bios | 305 s | **235 s** | |
| boot, smp4-iommu | 305 s | **235 s** | |

Six-tier gate on `0b536c355d51ced7ac558648696752a5`: 513 / 527 / 531 / 544 plus
both dirty tiers, zero failing assertions and `ranks=0` throughout, empty
consecutive-boot diffs. **The assertion counts are identical to the pre-change
baseline** — the change is faster and asserts exactly as much, which is the
result worth having. Roughly 12 minutes comes off a full `gate-all`.

**Crash consistency, before and after.** The harness fires once, on a chosen
commit, and checks that the recovered directory contains no dirent it cannot
produce and that a file made durable earlier survives byte-for-byte:

| build | result |
|---|---|
| harness vs OLD journal (`d104783a`) | PASS — 48 live dirents, 0 unreadable |
| harness vs NEW journal | PASS — 49 live dirents, 0 unreadable |
| `-DJOURNAL_WRONG_BLOCK_REPRO` | **FAIL — 18 assertions** |

The reproducer records the entry under the block that really changed but shadows
the CONTENTS of block 0, so replay overwrites the changed block and the mutation
is lost at the next mount. It is the failure mode per-block journaling
introduces and whole-directory journaling was structurally incapable of, and it
takes down the harness's own durability check, the pre-existing
`recovered directory reloads with correct content`, and three `toolstrs`
assertions whose SDK files simply are not there any more.

**Two corrections worth keeping**, because both were wrong first:

- A `-DJOURNAL_STALE_SLOTS_REPRO` build — skipping the slot-map reset after
  apply — was written as the falsifiability target and **passed**. It was right
  to pass: every dirtied block rewrites its own slot, so a stale map still
  points each entry at that block's current content and replay stays correct.
  Measured cost of skipping the reset was 305 s against 300 s. The reset is
  hygiene, not correctness, and the code now says so instead of implying a role
  it does not have.
- The first `WRONG_BLOCK` reproducer assigned to the loop variable and hung the
  boot until the gate cap. The classifier reported `TRUNCATED` and refused to
  score it as either a pass or a failure, which is the only reason it was not
  read as "0 failures".

## KNOWN TECHNICAL DEBT

Carried from v0.84. The first two are named in this milestone's brief; the rest
are the remainder of that milestone's STILL OPEN list, kept here so the tracker
is complete rather than convenient.

- ~~**`O_APPEND` under CPU OVERSUBSCRIPTION is untested.**~~ **CLOSED.**

  An `append-oversub` phase now runs alongside the pinned one: twice as many
  workers as online cores, cycling the same four roles, with **affinity left
  unset** so the scheduler may stack and migrate them. The worker is unchanged —
  same roles, payload and loop — so nothing measured here can be true only of a
  special build.

  **The assertions were adapted, not relaxed.** Size, block uniformity and
  per-worker counts are statements about ATOMICITY and hold whatever the
  schedule, so they are identical. The two premise guards changed:

  - The pinned phase asserts the workers ran on MORE THAN ONE CORE, which is how
    it proves it is not a uniprocessor test wearing an `-smp 4` label. That is
    the wrong question here: on one core, two workers time-slicing IS the
    oversubscription under test. It is replaced by the structural guarantee —
    more workers than cores — asserted rather than assumed, so a later change to
    the worker count cannot quietly turn this back into a pinned test.
  - Interleave transitions were previously only meaningful at `n > 1`. Under
    oversubscription they are meaningful at EVERY core count, because two
    workers sharing one core can only produce interleaved output by being
    preempted.

  Two workers share a pattern byte when there are more workers than roles, so
  the per-pattern expectation is COMPUTED from the worker-to-pattern mapping.
  Asserting "ITERS each" would be wrong the moment the count stops dividing
  evenly by four — which it does at one and two cores.

  Measured, smp4-bios, 8 workers on 4 cores:

  | build | image | file | per-pattern | verdict |
  |---|---|---|---|---|
  | current | `20db2cf4` | 4096 B exact, 149 transitions | 64/64/64/64 | PASS |
  | `-DAPPEND_RACE_REPRO` | `8137f151` | **1760 B of 4096** | — | FAIL |

  The reproducer loses **146 of 256 appends (57%)**, close to the 56% the pinned
  phase loses, so the new mode genuinely exercises the atomicity path rather
  than passing for incidental reasons. The existing `APPEND_RACE_REPRO` flag was
  reused rather than a second reproducer written: a new switch would only
  re-test the same defect, and this cycle has already produced two reproducers
  that reproduced the wrong thing.
- **Dedup-masked reference-count underflow cannot be observed.** **DESIGN SPIKE
  DONE — NOT IMPLEMENTED, and the reason is structural rather than effort.**

  `g_cas_ref_underflow` detects a double release only when nothing
  re-references the block in between. v0.84's O_TRUNC double-release ran with
  that counter reading zero throughout; the dirty-volume gate caught it, the
  counter did not.

  **Why zero was the honest answer.** Tracing the actual sequence: the first
  release took the block 1 -> 0 and REMOVED ITS INDEX ENTRY, so the block was
  genuinely free. A later `cas_put` of the same content then found no index
  entry, allocated a NEW block, and the retain took it 0 -> 1. The stale-hash
  release that followed resolved that same hash to the new block and took it
  1 -> 0. The count was legitimately 1 at both releases. No counter positioned
  at the decrement can distinguish them, because nothing is wrong AT the
  decrement — the reference that authorised it belonged to a map that no longer
  exists.

  **This is carryover 3's problem in a different subsystem:** a stale reference
  resolving to a recycled identity. That was fixed by pinning the reference to a
  `(slot, generation)` pair and rejecting a mismatch — and the same shape is
  what would work here.

  **The obstacle is storage, and it is specific.** A per-block generation is
  cheap: `g_cas_refs` is already a 65536-entry table, and a parallel generation
  array costs the same again in BSS. The missing half is the RETAIN-side record.
  A reference lives in `dirent.chunk_hash[]` as a bare 64-bit hash with no room
  beside it, and `struct dirent` is fixed at 256 bytes by
  `_Static_assert` — with `reserved[]` down to 4 bytes after v0.85's timestamps,
  which is not enough for a generation per chunk even in the direct tier, let
  alone the 4176 an indirect map can reach.

  So a counter-shaped fix is not available, and the honest options are:

  1. **Widen the on-disk dirent** to carry a generation per chunk. Breaks the
     256-byte layout, `VFS_DIR_BLOCKS`, the journal record and every existing
     volume. Disproportionate to the defect class.
  2. **Validate at release** that the dirent still names the hash it is
     releasing. Does not work: the release is called WITH the stale map, which
     does name it — that is exactly the bug.
  3. **Detect after the fact** rather than at the decrement: a periodic sweep
     comparing derived reference counts against the live table, which
     `cas_refs_rebuild()` already computes. This is the tractable one, and it is
     the recommendation — it catches a corrupted count wherever it came from,
     without needing to attribute it to a particular release.

  **Recommendation: do not add a counter here.** The v0.84 defect was caught by
  the dirty-volume gate, and the v0.85 free-path defect was caught by measuring
  allocated-but-unreferenced blocks — both times by asking about the WHOLE
  volume rather than about an individual operation. Option 3 follows that grain.
  A per-release counter would reassure without detecting, which is the failure
  mode this project has documented repeatedly.
- **VOL_TMP has no mode and no group.** Deliberate; owner-or-root is the whole
  rule, so a tmpfile cannot be shared. Revisit only if something needs it, and
  see the `chmod` note above.
- **`lseek`/`SEEK_END` on another user's tmp descriptor** discloses that file's
  length. Metadata rather than content, and it matches the root volume.
- **CAS reclamation completeness is not asserted.** `blocks freed > 0` says it
  ran, and the dedup control says it is correct for the shared case; nothing
  asserts that every unreferenced block is eventually returned.
- ~~**No crash injection between a free's two writes.** The existing crash test
  covers puts, not frees.~~ **CLOSED — and it was hiding a real defect, not just
  a test gap.**

  `cas_journal_write()` snapshots the superblock, the live bitmap block and the
  staged index block. `cas_put` has always called `bm_alloc()` BEFORE it, so all
  three shadows describe the same post-transaction state. `cas_free` called
  `bm_free()` AFTER it — so its index shadow was post-free while its bitmap and
  superblock shadows were PRE-free. Replaying that pair marks the block
  allocated with no index entry naming it: unreachable, and unfreeable for the
  life of the volume. One block per interrupted free, shipped since v0.84.

  **Nothing would have noticed.** The standing `used_blocks ==
  popcount(bitmap)` audit passes either way, because recovery restores both
  halves from the same snapshot and they agree with each other. The volume
  mounts, every file reads back, every counter reconciles. Only asking which
  ALLOCATED blocks are named by a live file exposes it — and that instrument
  (`cas_unreferenced_locked`) exists only because v0.84 added it to explain an
  unrelated 2-block residual.

  Measured, uniprocessor, one interrupted free:

  | build | image | unreferenced | used_blocks | verdict |
  |---|---|---|---|---|
  | unfixed | `407e14e4` | 4 → **5** | 1125 → 1125 | FAIL |
  | fixed | `90f6c9b6` | 4 → 4 | 1125 → **1124** | PASS |
  | fixed + `-DCAS_FREE_ORDER_REPRO` | `1c26c723` | 4 → **5** | 1125 → 1125 | FAIL |

  `used_blocks` not moving on the broken build is the whole point: the leak is
  invisible to arithmetic that only checks self-consistency.

  **The fix is one line moved** — `bm_free()` above `cas_journal_write()`, giving
  `cas_free` the ordering `cas_put` already had. The harness is a second arm on
  the existing `-DCRASH_INJECT_COMMIT_FAIL` build, runtime-armed and
  self-clearing, firing between the journal write and the bitmap flush.

  Six-tier gate on `701b824b1da2963b9676a16819d5d2e3`: 520 / 534 / 538 / 551
  plus both dirty tiers, `ranks=0`, empty consecutive-boot diffs, clean build
  with no warnings. Assertion counts are IDENTICAL to the pre-change baseline —
  the crash phase is opt-in, so the gate is confirming the reorder regressed
  nothing rather than counting the new test.

  Logs: `OUTRUN-0.85-casfree-baseline-fail.log`,
  `OUTRUN-0.85-casfree-fixed.log`, `OUTRUN-0.85-casfree-order-repro.log`.
- **Changelogs for 0.81–0.84 do not exist in the tree.** The tags carry the
  content. Writing them out remains bookkeeping this project should finish, and
  it is the reason this roadmap exists at all: a v0.84 session was handed a
  carryover status superseded three milestones earlier and nearly
  re-implemented work that had already landed.

## VERIFIED BASELINE INHERITED FROM v0.84

`66b0698`, six-tier gate on image `bf4ef8ecf65c6dad5b59e23242d7d70e`: 513 / 527
/ 531 / 544 across uniprocessor, smp2-bios, smp4-bios and smp4-iommu, plus
`gate-dirty` and `gate-dirty-smp` at 3 boots each with empty consecutive-boot
diffs. Zero failing assertions and `ranks=0` throughout.

This milestone starts from that. Anything it breaks should show as a delta
against those counts, and a tier whose count moves for a reason nobody can name
is a regression until someone names it.

## BEFORE TAGGING v0.85.0

The Release Protocol in `CLAUDE.md` applies unchanged: bump `VERSION` and
`KERNEL_VERSION` and COMMIT before tagging, `make release-iso` from a clean
tree, `make release-verify` on the exact published image, and record the MD5 and
SHA-256 beside the tag. Cut the release from the MAIN CHECKOUT — the git half of
`release-version-check` cannot answer inside a linked worktree under WSL, and
prints `CANNOT VERIFY VERSION` rather than passing.
