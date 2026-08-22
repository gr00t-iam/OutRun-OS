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
- **`chmod` / `chown`.** `struct dirent` already carries `uid`, `gid` and
  `mode`, and `vfs_permit` already reads all three; what is missing is any way
  to CHANGE them after creation. Ownership transfer needs the usual rule — only
  root may give a file away — and `chmod` needs owner-or-root.
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

## KNOWN TECHNICAL DEBT

Carried from v0.84. The first two are named in this milestone's brief; the rest
are the remainder of that milestone's STILL OPEN list, kept here so the tracker
is complete rather than convenient.

- **`O_APPEND` under CPU OVERSUBSCRIPTION is untested.** The v0.85 harness pins
  one worker per core, so it measures four concurrent appenders and not more
  writers than cores. Oversubscription is the case where a writer is preempted
  mid-syscall, which is a different schedule from four writers each owning a
  core. The harness already takes its worker and iteration counts from defines,
  so testing it is mostly a matter of unpinning and raising the count — and of
  deciding what the assertion should be when the deadline is the binding
  constraint rather than the defect.
- **Dedup-masked reference-count underflow cannot be observed.**
  `g_cas_ref_underflow` detects a double release only when nothing
  re-references the block in between. v0.84's O_TRUNC double-release ran with
  that counter reading zero throughout, because an intervening dedup had raised
  the count back to 1 — the dirty-volume gate caught it, the counter did not.
  There may be no counter-shaped fix; what would help is a way to distinguish
  "released twice" from "released, re-referenced, released", for instance a
  per-block release generation checked against the retain that raised it.
- **VOL_TMP has no mode and no group.** Deliberate; owner-or-root is the whole
  rule, so a tmpfile cannot be shared. Revisit only if something needs it, and
  see the `chmod` note above.
- **`lseek`/`SEEK_END` on another user's tmp descriptor** discloses that file's
  length. Metadata rather than content, and it matches the root volume.
- **CAS reclamation completeness is not asserted.** `blocks freed > 0` says it
  ran, and the dedup control says it is correct for the shared case; nothing
  asserts that every unreferenced block is eventually returned.
- **No crash injection between a free's two writes.** The existing crash test
  covers puts, not frees — and see the journal work above, which needs this
  machinery anyway.
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
