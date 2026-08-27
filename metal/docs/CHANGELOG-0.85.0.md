# OutRun OS v0.85.0-metal — metadata, a journal that costs 2 writes, and three defects the harness found

Milestone 85. Written as the cycle ran rather than reconstructed afterwards —
which is itself one of this release's changes: v0.81 through v0.84 had no
changelog in the tree at all until this milestone wrote them, and a v0.84 session
was handed a carryover status superseded three milestones earlier and nearly
re-implemented work that had already landed.

## ARTEFACT

```
outrun-os-0.85.0.iso   (5,896,192 bytes)
MD5    d1bb72645fc5a0a12e40c9653261c206
SHA256 3242241a202a150a2a2ba7b19b8f473b4052630639501d67729a54437b9fd257
```

`make release-verify` PASS on that md5 — 45 suites, 0 failing assertions,
0 rank faults, 325 s. The md5 `release-verify` stamped into its own log, the
recorded `.md5`, and an independent `md5sum` all agree, so the PASS describes the
published artefact rather than a neighbouring build.

## WHAT LANDED

### VFS metadata — `rename`, timestamps, and a credential leak

**`SYS_RENAME` (102).** The namespace is flat — names are paths but there is no
directory tree — so a "cross-directory move" and a "same-directory rename" are
the same operation. The case POSIX spends its words on is renaming **onto an
existing name**, which must replace atomically and release the replaced file's
blocks, or rename becomes a way to leak storage that unlink cannot. Measured:
`used_blocks 1122 → 1120` across a replacing rename.

**`vfs_journal_commit_multi(idx1, idx2)`** is what makes that safe. Both shadows
are staged, then ONE header publishes them. Committing the two indices in turn
would **not** be atomic — the first header names only the first block, so a crash
between them leaves the entry renamed and the target not yet removed: two live
dirents carrying one name.

**Timestamps.** `mtime`/`atime` carved from `reserved[12]`, leaving 4 bytes and
holding the 256-byte static assert. Boot-relative 100 Hz ticks, documented as
such — there is no RTC, and calling them seconds-since-1970 would be inventing a
number. Zero means UNKNOWN, following v0.72's rule for `mode`.

`SYS_STAT` was **not** widened; `omake.oc` allocates exactly two 64-bit words and
the kernel `cmemcpy`s `sizeof(st)` into it. Timestamps arrive through a new
`SYS_FSTAT` (103). **`atime` is lazy and never journalled** — committing on every
read would cost 2 block writes and a publish for an operation that changed no
content, which is why real systems ship `relatime`.

**A live authority leak, fixed (`7eeab2e`).** `chmod` and `chown` read the REAL
uid while every other VFS check used the effective one. They are v0.72; the
effective pair arrived in v0.74 and these two were never converted. Because
`SYS_SETEUID` moves only the effective id, a process that dropped to euid 1000
was refused every write by `vfs_permit` **and could still change the mode of any
file on the volume**. Four tagged releases carried that, with no test — the
assertion that appeared to cover it assigned `DENTS[di].mode` directly and never
called the syscall.

### Single-block directory journal commit

`vfs_journal_commit()` wrote the header plus **all 48** directory blocks on every
mutation — 49 block writes to record a change to one 256-byte dirent, measured at
4.67 appends/second under contention.

**The `cjournal` pattern does not transfer**, and that changed the design. The
CAS metadata journal is *eager*; this one is deliberately *deferred*, and a suite
asserts the deferral. Because apply is deferred the shadow must hold **every**
un-applied change, so journaling only the newest block would let commit B
silently discard commit A. The journal now holds a **set** of dirty blocks,
bounded by construction — there are only 48, each takes at most one slot, so the
worst case is the old behaviour at the old cost.

Two things changed beyond the size of the write. **The shadow is now written
before the header**, which makes a commit atomic rather than merely survivable.
And **`vfs_open_for`'s O_CREAT path had to start journaling** — it created a
dirent and committed nothing, relying on the next unrelated commit's
whole-directory shadow to carry it.

| | before | after |
|---|---|---|
| block writes per commit | 49 | **2** |
| append phase, smp4-bios | 2846 ticks | **1038** (2.74×) |
| appends/second | 4.50 | **12.3** |
| boot, smp4-bios | 305 s | **235 s** |

### `O_APPEND` under contention, then under preemption

v0.84 shipped `O_APPEND` and recorded its atomicity as *argued* rather than
measured. v0.85 measured it twice.

**Pinned** — four workers, one per core, affinity set so the contention is
constructed rather than hoped for. **Oversubscribed** — twice as many workers as
cores, affinity unset, so a writer is preempted mid-syscall. The assertions were
adapted rather than relaxed: size, uniformity and per-worker counts are
statements about atomicity and hold whatever the schedule, while the "ran on more
than one core" guard is replaced by the structural one (more workers than cores)
because on one core two workers time-slicing *is* the case under test.

Falsified in both modes by `-DAPPEND_RACE_REPRO`: **57% of appends lost** under
oversubscription, 56% under pinning.

### A CAS free that leaked one block per crash

`cas_journal_write()` snapshots the superblock, the live bitmap block and the
staged index block. `cas_put` has always called `bm_alloc()` **before** it;
`cas_free` called `bm_free()` **after**. So a free's index shadow was post-free
while its bitmap and superblock shadows were pre-free, and replaying that pair
marked the block allocated with no index entry naming it — unreachable, and
unfreeable for the life of the volume. Shipped since v0.84.

**Nothing would have noticed.** The standing `used_blocks == popcount(bitmap)`
audit passes either way, because recovery restores both halves from the same
snapshot and they agree with each other:

| build | unreferenced | used_blocks |
|---|---|---|
| unfixed | 4 → **5** | 1125 → 1125 |
| fixed | 4 → 4 | 1125 → **1124** |

One line moved. What made it findable was writing the crash harness against the
unfixed kernel first and measuring *which allocated blocks a live file names*
rather than whether the counters agree with each other.

### Live-versus-derived reference sweep (TALLY)

`vfs_map_walk_locked` gained a third mode. `cas_tally_verify()` derives what the
directory says and compares it against the live table using two sums —
`Σ refs[b]` and `Σ (b+1)·refs[b]` — so the auxiliary cost is O(1) rather than
another 128 KiB, and the weighting both prevents compensating errors cancelling
and localises the offending block.

**It does not catch the v0.84 defect it was first proposed for**, and the roadmap
records that correction. `cas_free` decrements and *then* de-indexes, so a bad
release leaves a hash that no longer resolves and the derivation contributes
nothing for it. What it does catch is retain/release **imbalance** while the
block is still indexed — live high means a missed release and a leak, live low
means a missed retain and a block that can be freed while a file still names it.

## GATE

Six tiers on one image, `ranks=0` throughout, clean build with no warnings:

Every tier on ONE image, `e9fc67d773b5223fe65b277f3c3f5789`:

| tier | v0.84 baseline | v0.85 |
|---|---|---|
| uniprocessor | 507 | **530** |
| smp2-bios | 519 | **544** |
| smp4-bios | 523 | **548** |
| smp4-iommu | 536 | **561** (47 suites) |
| gate-dirty / gate-dirty-smp | 0 failing | **0 failing**, empty diffs |

NOT run for this tag: bare metal, Proxmox, and any soak or repeat beyond the
boots above — so this table cannot see an intermittent below roughly 1 in 10
boots on the fresh tiers.

## WHAT THE PROCESS COST, AND WHAT IT CAUGHT

Three defects in this milestone's own work were caught by the harness rather
than by review, and each is recorded where it happened:

- A **duplicate staging loop** left the crash-injection hook in only one of two
  commit paths, so `rename` — the operation whose atomicity most needed
  testing — could not be interrupted at all. The main assertion would have
  passed; only the "did it actually fire" guard caught it.
- A **reproducer that reproduced nothing**: skipping the journal slot-map reset
  turned out to be benign, and the comment claiming otherwise was withdrawn.
- A **reproducer that hung the boot** by assigning to a loop variable, reported
  `TRUNCATED` and correctly refused a verdict.

A `gate-all` run also failed on `smp4-bios` with `[langstrs]` red after that tier
took 1615 s against 235 s — a drain watchdog on a degraded host, cleared by a
control re-run of the same image at 538/0 in 240 s. v0.81's tag predicted exactly
that class of false attribution.

## KNOWN, NOT FIXED

- **The TALLY sweep sees only since the last rebuild**, so corruption in an
  earlier boot of a reused volume is erased by the mount that precedes it.
- **VOL_TMP has no mode and no group.** Deliberate; owner-or-root is the whole
  rule, so a tmpfile cannot be shared.
- **`lseek`/`SEEK_END` on another user's tmp descriptor** discloses that file's
  length. Metadata rather than content, and it matches the root volume.
- **No crash injection on the PUT path**, which has always had the consistent
  ordering, and none inside the legacy pre-journal branch of `cas_free`.
- **The oversubscription ratio is fixed at 2:1**, and nothing tests a writer
  preempted while the journal transaction itself is mid-flight.
