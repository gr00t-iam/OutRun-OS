# OutRun OS v0.84.0-metal — roadmap

Milestone 84. `main` is at `c492efc`; `VERSION` reads `0.84.0-dev` and **no
v0.84 tag exists yet** — this milestone is open.

This file is written mid-cycle rather than at its start, and the reason is worth
recording: **milestones 0.81 through 0.83 have no roadmap or changelog in the
tree at all.** Their release notes exist only in annotated git tags. That is not
fatal — the tags are thorough, and each carries its artefact md5, its gate table
and a KNOWN, NOT FIXED list — but it means the only way to learn what is open is
`git tag -n200`, which nobody thinks to run. A v0.84 session was handed a
carryover-3 status that had been superseded three milestones earlier and nearly
re-implemented work that had already landed; see
`OUTRUN-0.84-carryover3-reverify.md`. A tracking doc that lives in the tree is
the cheap half of preventing that.

---

## WHAT THIS CYCLE HAS SHIPPED

| commit | what |
|---|---|
| `44e1051` | chunk-iterating positional writes, no staging ceiling |
| `bb34d39` | reset `g_inr3_max` on threadstrs entry |
| `c8f8e07` | VOL_TMP creator uid + owner/root **unlink** check |
| `8c412b3` | threadstrs worker startup sync; smp2 SKIP retired |
| `c372ecf` | build the >32 KiB lseek probe with one whole-file write |
| `886750d` | carryover 3 re-verified at the tip; stale role heading fixed |
| `c492efc` | VOL_TMP permission model completed: open, read, write, truncate |

**Every KNOWN, NOT FIXED item from the v0.82 and v0.83 tags is now closed.**

- v0.83's *"a tail-preserving positional write beyond REDIR_STAGE_MAX still
  returns ENOSPC"* → `44e1051`.
- v0.83's *"tmp unlink is NOT permission-checked"* → `c8f8e07`, and the larger
  asymmetry it disclosed → `c492efc`.
- v0.82's and v0.81's *"threadstrs still reads the shared ring-3 high-water
  without resetting its own"* → `bb34d39` / `8c412b3`.

## VERIFIED BASELINE

**Commit `c492efc`**, six-tier gate on image
`fc7595b88864e8c901a7bf55fcbee797`, measured 2026-08-18. Ten boots, zero failing
assertions, zero rank violations, zero underflow/mismatch, zero panics.

| tier | boots | assertions | result |
|---|---|---|---|
| `uniprocessor` | 1 | 498 | PASS (320 s) |
| `smp2-bios` | 1 | 510 | PASS (250 s) |
| `smp4-bios` | 1 | 514 | PASS (245 s) |
| `smp4-iommu` (q35 + VT-d, `intremap=on`) | 1 | 527 / 47 suites | PASS (250 s) |
| `gate-dirty` (one reused image, uniprocessor) | 3 | 0 failing each | PASS |
| `gate-dirty-smp` (one reused image, `-smp 4`) | 3 | 0 failing each | PASS |

Post-merge sanity on `main` (`c492efc`, image
`8c697ae2ab80cd2a92e428bbb7dbefe0`): uniprocessor 45 suites, 498 passed, 0
failed, ranks 0. The tmp counters read 77 decisions / 5 refusals — identical to
the branch measurement, which is what says integration changed nothing.

`smp4-iommu` ran at 250 s here. It is **host-speed sensitive** (v0.81's tag
records 1080–2065 s with failures on a degraded host, at unmodified HEAD): run a
control before blaming a change, and raise `GATE_CAP` rather than reading the
assertions of a `TRUNCATED` boot.

What this baseline does **not** say: one boot per fresh configuration and three
per dirty configuration, so it cannot see an intermittent below roughly 1 in 10
boots. No release ISO was built or `release-verify`'d — this is a development
baseline, not a tag.

---

## NEXT: CAS BLOCKS ARE NEVER RECLAIMED ON UNLINK

The largest disclosed gap left in this subsystem, and it is disclosed in the
code rather than inferred (`vfs_unlink`, `kernel64.c`):

> Deliberately NOT reclaiming the CAS blocks/index slots the file's chunks used:
> there is no reference counting across dirents (two files can share a
> chunk_hash via dedup), so freeing them here could silently corrupt a
> different, still-live file. Disclosed as a scope boundary, not fixed here.

**The state today.** `cas_put()` and `cas_get()` exist; there is no `cas_free()`,
no refcount on a CAS chunk, and no collector. Frames are refcounted
(`frame_refs()`); CAS blocks are not. So every unlink — and every O_TRUNC, which
empties a dirent's chunk map — permanently strands the blocks the file used. The
volume can only fill.

**Why it is the right next target.** It is a real resource leak with a *named*
cause, in the subsystem this milestone has been working all cycle, and the thing
that blocks it (no refcounting under dedup) is a design question rather than a
missing line. It is also the gap most likely to be discovered the worst way: as
a volume that mysteriously fills after enough churn, on the dirty-volume gate,
long after the change that made the churn.

**The trap, stated up front.** Dedup means two dirents may name the same
`chunk_hash`. Freeing on unlink without refcounting corrupts a live file, which
is precisely why v0.48 declined to do it. Any fix must survive a test in which
file A and file B share a chunk, A is unlinked, and B is then read back byte for
byte.

**Verification strategy, in the shape this tree now uses:**

- Count and print reclaimed blocks and index slots. A reclamation nothing
  reports is not evidence.
- Assert the volume's used-block count *returns to its pre-write value* after
  authoring and unlinking, rather than asserting merely that unlink returned 0.
- **The dedup negative control is the load-bearing test**, and it must be
  written before the fix: A and B share a chunk, A is unlinked, B still reads
  correctly. Without it, a refcount that is simply absent looks identical to one
  that works, on any workload where no chunk is shared.
- Paired falsifiability, per CLAUDE.md: an `EXTRA=-D…` build that frees without
  consulting the refcount must corrupt B and be *watched* doing it.
- The dirty-volume gate is where this genuinely bites, because it is the only
  configuration that reuses an image across boots.

### Also open, smaller

- **`ftruncate` and `O_APPEND`.** The O_TRUNC comment names both as absent.
  O_TRUNC now covers "empty it at open"; shortening an open file, and atomic
  append, still have no expression.
- **`lseek`/`SEEK_END` on another user's tmp descriptor** reports that file's
  length. Metadata, not content, and it matches the root volume — named in
  `OUTRUN-0.84-tmp-permission-model.md` rather than fixed.
- **VOL_TMP has no mode and no group.** Owner-or-root is the whole rule, so a
  tmpfile cannot be shared. Deliberate; revisit only if something needs it.
- **Changelogs for 0.81–0.84 do not exist in the tree.** The tags carry the
  content; writing them out is bookkeeping this milestone should finish.

## BEFORE TAGGING v0.84.0

The Release Protocol in `CLAUDE.md` is mandatory and is not satisfied by any of
the above: bump `VERSION` and `KERNEL_VERSION` and commit **before** tagging,
`make release-iso` from a clean tree, `make release-verify` on the exact
published image, and record the MD5 and SHA-256 in the release notes. `v0.75.0`
was tagged with `VERSION` still reading `0.74.0`; the protocol exists so that
cannot complete silently.
