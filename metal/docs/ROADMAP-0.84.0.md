# OutRun OS v0.84.0-metal — roadmap

Milestone 84. `main` is at `6e2e23b`; `VERSION` reads `0.84.0` and
`KERNEL_VERSION` reads `0.84.0-metal`. **The tag does not exist yet** — see
"Before tagging" at the end, which is the part of the Release Protocol this
commit does NOT satisfy.

This file was written mid-cycle rather than at its start, and the reason is worth
keeping: **milestones 0.81 through 0.83 have no roadmap or changelog in the tree
at all.** Their release notes exist only in annotated git tags. That is not fatal
— the tags are thorough, and each carries its artefact md5, its gate table and a
KNOWN, NOT FIXED list — but it means the only way to learn what is open is
`git tag -n200`, which nobody thinks to run. A v0.84 session was handed a
carryover-3 status that had been superseded three milestones earlier and nearly
re-implemented work that had already landed; see
`OUTRUN-0.84-carryover3-reverify.md`. A tracking doc that lives in the tree is
the cheap half of preventing that.

---

## WHAT THIS CYCLE SHIPPED

Thirteen commits, `0e042cb..6e2e23b`. The theme is one subsystem taken to
completion: the VFS learned what a file POSITION means in v0.82/0.83, and this
milestone finished the consequences of that — writes at any size, a volume that
can give storage back, an ownership model on the scratch volume, and the two
POSIX operations the positional work had named as missing.

| commit | what |
|---|---|
| `44e1051` | chunk-iterating positional writes — the 32 KiB staging ceiling is gone |
| `bb34d39` | reset `g_inr3_max` on threadstrs entry |
| `c8f8e07` | VOL_TMP creator uid + owner/root **unlink** check |
| `8c412b3` | threadstrs worker startup sync; smp2 SKIP retired |
| `c372ecf` | build the >32 KiB lseek probe with one whole-file write |
| `886750d` | carryover 3 re-verified at the tip; stale role heading fixed |
| `c492efc` | VOL_TMP permission model completed: open, read, write, truncate |
| `46978f2` | this roadmap |
| `f118112` | the dedup negative control for CAS reclamation, **before** the fix |
| `a881542` | CAS blocks reclaimed on unlink and O_TRUNC, reference-counted |
| `a48ed1c` | the unreferenced-block residual root-caused and made measurable |
| `6e2e23b` | `ftruncate` and `O_APPEND` |

**Every KNOWN, NOT FIXED item from the v0.81, v0.82 and v0.83 tags is closed**,
and so is every item this roadmap listed as open when it was written.

### The four that mattered

**Positional writes past the staging ceiling** (`44e1051`). A tail-preserving
write anywhere in a file larger than `REDIR_STAGE_MAX` returned ENOSPC, so the
only large writes the system could do were whole-file ones from offset 0. The
ceiling was the staging buffer's, not the filesystem's, and it went away by not
staging content at all.

**VOL_TMP ownership** (`c8f8e07`, then `c492efc`). The uid arrived guarding
unlink alone, and its own commit named what that left open: any holder of
`PCAP_FILESYSTEM` could still read or overwrite another user's scratch file,
having merely been unable to delete it — a leak of AUTHORITY. Open, read, write
and truncate are now judged too. Still no mode and no group: the rule is
owner-or-root, so a tmpfile cannot be shared. That is a real difference from the
root volume, and it is named in the struct rather than left to be discovered.

**CAS block reclamation** (`f118112`, `a881542`, `a48ed1c`). Since v0.48 unlink
had stranded every block a file used — the volume could only fill — and v0.48
said why: dedup lets two dirents share a chunk, so freeing without reference
counting corrupts a live file. Counts are now DERIVED from the live directory at
mount rather than stored on disk, because `cas_islot` is a 16-byte packed record
whose size *is* `CAS_SLOTS_PER_BLOCK`; deriving also cannot drift from the
directory or be left stale by a crash. See
`OUTRUN-0.84-cas-block-reclamation.md`.

**`ftruncate` and `O_APPEND`** (`6e2e23b`). Named as absent by v0.83's own
O_TRUNC comment. The append offset is resolved inside the lock that performs the
write — read-then-write would let two appenders choose the same place and lose
one of them.

### What the process cost, and what it caught

Three defects in this milestone's own work were caught by the harness rather
than by review, and all three are recorded where they happened:

- **A double-release of CAS blocks**, from an O_TRUNC handler that released a
  chunk map but left the map's hashes in the dirent. The four FRESH tiers passed
  while it was live; only `gate-dirty` — three boots on one reused image — saw
  it, as `new=2` then `new=1` growing across boots. Anyone running `make gate`
  instead of `make gate-all` would have shipped it.
- **An assertion weaker than claimed.** `underflow == 0` was described as the
  double-release detector and read zero throughout that failing run, because an
  intervening dedup had restored the count. It catches double release only when
  nothing re-references the block in between.
- **Dead code with a false explanation.** `vfs_truncate_locked` zeroed a boundary
  chunk's tail and a comment called it load-bearing; the reproducer built to
  prove that PASSED without it. `cas_put`'s length bound already discards the
  tail. Removed, comment corrected.

## VERIFIED BASELINE

**Commit `6e2e23b`**, six-tier gate on image
`46a405e4bafc70e87250883221dd5505`, measured 2026-08-20. Ten boots, zero failing
assertions, zero rank violations, zero underflow/mismatch, zero panics. Clean
rebuild with no warnings under `-Wall -Wextra`.

| tier | boots | assertions | result |
|---|---|---|---|
| `uniprocessor` | 1 | 507 | PASS (350 s) |
| `smp2-bios` | 1 | 519 | PASS (280 s) |
| `smp4-bios` | 1 | 523 | PASS (285 s) |
| `smp4-iommu` (q35 + VT-d, `intremap=on`) | 1 | 536 / 47 suites | PASS (285 s) |
| `gate-dirty` (one reused image, uniprocessor) | 3 | 0 failing each | PASS |
| `gate-dirty-smp` (one reused image, `-smp 4`) | 3 | 0 failing each | PASS |

Both dirty tiers reported empty consecutive-boot assertion diffs and intact
durable artefacts at both widths.

**What this baseline does not say.** One boot per fresh configuration and three
per dirty configuration, so it cannot see an intermittent below roughly 1 in 10
boots. **No release ISO has been built or `release-verify`'d** — this is a
development baseline measured on `outrun-os-0.84.0-dev.iso`, not on the artefact
a tag would publish. The version strings changed after it was taken, so the
published image must be measured on its own terms.

`smp4-iommu` ran at 285 s here. It is host-speed sensitive (v0.81's tag records
1080–2065 s with failures on a degraded host, at unmodified HEAD): run a control
before blaming a change, and raise `GATE_CAP` rather than reading the assertions
of a `TRUNCATED` boot.

---

## STILL OPEN

Nothing here blocked the tag; all of it was written into the v0.84.0 release
notes as KNOWN, NOT FIXED.

**One entry has since been closed.** It is struck through rather than deleted,
and it keeps its original wording, because the `v0.84.0` tag annotation is
immutable and still lists it — a reader comparing the two should be able to see
that the tag was accurate for what was true when it was cut, and that the gap
was closed afterwards. Deleting the line would make the tag look wrong.

- ~~**`O_APPEND` atomicity is argued, not measured.** The offset is resolved
  inside the write lock, which is the correct construction, but there is no
  concurrent multi-writer test that would fail if it were not.~~

  **CLOSED / VERIFIED — post-tag, in `66b0698`** (`test(vfs): add SMP multi-core
  O_APPEND atomic write stress test`).

  *Topology.* The `append-smp` phase in `cmd_vfs_stress` spawns four ring-3
  workers (roles 56..59), each `affinity`-pinned to one core and pushed to that
  core's queue, all appending to one file with no user-space synchronisation.
  Pinning is what makes the contention constructed rather than hoped for: with
  affinity unset the scheduler may run all four on one core and the test becomes
  a uniprocessor test wearing an `-smp 4` label. The kernel reads the file back
  and judges it without trusting the workers that wrote it.

  *Measured.* 100% write integrity in both sizes — every payload block intact
  and contiguous, per-worker counts exact, file length exact:

  | run | appends | file | per-worker | interleave | cores |
  |---|---|---|---|---|---|
  | gate (`bf4ef8ec…`, smp4-iommu) | 128 | 2048 B exact | 32/32/32/32 | 100 transitions | `0x0f` |
  | soak (`bc50fc44…`, smp4-bios) | 1024 | 16384 B exact | 256/256/256/256 | 810 transitions | `0x0f` |

  Transitions are reported because correct counts alone are also what a run in
  which each worker finished before the next began would produce — a pass that
  tested nothing.

  *Falsifiable.* `EXTRA=-DAPPEND_RACE_REPRO` reverts `vfs_write_append` to the
  two-step form — resolve end-of-file, drop the lock, retake it and write at the
  remembered offset — which is what the code reads like when someone "just"
  seeks to the end first. On `smp4-bios` (`6ad31a70…`) the file came back
  **896 B of an expected 2048: 72 of 128 appends lost, ~56%** — while every
  worker still reported success, which is why the file is the arbiter and not
  the workers.

  *Cost, and why the gate runs the smaller size.* Every append calls
  `vfs_journal_commit()`, which writes the journal header plus all 48 directory
  blocks — 49 block writes per append, a constant that does not shrink with the
  payload or the file. Measured at 4.67 appends/second under four-way
  contention, so 1024 appends is ~283 s: affordable once, not ten times over
  inside `gate-all`. The gate runs 128 (~28 s) on every tier; `make appendsoak`
  runs the full 1024 and sets `APPSMP_SOAK` in both `EXTRA` and `UEXTRA`, which
  have to agree.

  Logs: `OUTRUN-0.85-appendsmp-gate-smp4-iommu.log`,
  `OUTRUN-0.85-appendsmp-soak-1024.log`,
  `OUTRUN-0.85-appendsmp-race-repro.log`.

  *Still not covered by it:* the workers are pinned one per core, so this is
  four concurrent appenders and not oversubscription; no crash is injected
  mid-append; and no gate runs the soak variant.
- **Reference-count underflow cannot see a masked double release** (above). Not
  fixable by a counter; the dirty-volume tier is the thing that catches it.
- **VOL_TMP has no mode and no group.** Deliberate — owner-or-root is the whole
  rule — so a tmpfile cannot be shared. Revisit only if something needs it.
- **`lseek`/`SEEK_END` on another user's tmp descriptor** reports that file's
  length. Metadata, not content, and it matches the root volume.
- **CAS reclamation completeness is not asserted.** `blocks freed > 0` says it
  ran and the dedup control says it is correct for the shared case; nothing
  asserts that every unreferenced block is eventually returned.
- **No crash injection between a free's two writes.** The existing crash test
  covers puts, not frees.
- **Changelogs for 0.81–0.84 do not exist in the tree.** The tags carry the
  content. Writing them out remains bookkeeping this project should finish.

---

## BEFORE TAGGING v0.84.0

The Release Protocol in `CLAUDE.md` is mandatory and **this commit satisfies
only its first step**. `VERSION` is `0.84.0` and `KERNEL_VERSION` is
`0.84.0-metal`, committed before any tag exists — which is the step v0.75.0 got
wrong, tagging while `VERSION` still read `0.74.0` so every image for that
release was named `outrun-os-0.74.0.iso`.

Remaining, in order:

1. `make release-iso` — clean rebuild, checksums, manifest into
   `build/release/`.
2. `make release-verify` — boot the exact published image from a fresh volume;
   it fails unless the prompt is reached with zero failing assertions and no
   rank fault.
3. Record the MD5 **and** SHA-256 beside the tag. An image whose checksum is not
   written down cannot later be shown to be the one that was tested.
4. Re-run `make gate-all` on the release commit if any source changed after the
   baseline above, and state in the notes which tiers were run and which were
   not.

`make release` does steps 1 and 2 in that order. `make release-version-check`
confirms the banner matches `VERSION`; it now expects `0.84.0-metal` and will
warn loudly if the two disagree.

**Cut the release from the MAIN CHECKOUT, not from a linked worktree.**
`release-version-check` has two halves, and only one of them survives a worktree
here. The banner half reads `kernel64.c` directly and works anywhere. The git
half runs `git rev-parse --git-dir` first, and in a linked worktree that fails
under WSL — the worktree's `.git` file records a Windows-style absolute path
(`C:/Users/...`) which git-in-WSL concatenates onto its own cwd. The recipe then
prints `*** CANNOT VERIFY VERSION: git did not answer here` and, correctly, says
that is NOT a pass. Measured 2026-08-20: the same command in the main checkout
answers `v0.83.0` and the check runs properly.

So a release cut from a worktree would publish an artefact whose name nothing
cross-checked, while printing a message most people would read as noise. That is
the shape of a check that cannot fail, and it is avoided by building the release
where git can answer.

Note also that the git half will WARN until the tag exists — it compares
`git describe --tags --abbrev=0` (today `v0.83.0`) against `v$(VERSION)`
(`v0.84.0`). Pre-tag that mismatch is the expected state, not a defect; it
resolves when `v0.84.0` is created.
