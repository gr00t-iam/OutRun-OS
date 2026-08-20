# CAS blocks are reclaimed on unlink — v0.84

Since v0.48 `vfs_unlink` has zeroed a dirent and stranded every CAS block the
file used. The comment said so, and said why:

> Deliberately NOT reclaiming the CAS blocks/index slots the file's chunks used:
> there is no reference counting across dirents (two files can share a
> chunk_hash via dedup), so freeing them here could silently corrupt a
> different, still-live file. Disclosed as a scope boundary, not fixed here.

The reason was correct. This adds the reference counting it names, so the volume
stops being write-only.

## Phase 1 — the negative control, written first

The hazard exists ONLY when two dirents share a chunk. On any workload where
nothing is shared, a reclaimer that ignores sharing behaves identically to one
that honours it, so a suite that never builds the sharing case goes green
against either — and keeps going green until real content collides. The control
was therefore written and measured BEFORE the fix (`f118112`).

Two names, one payload, four chunks. Sharing is proven rather than assumed: the
four hashes are compared pairwise and `used_blocks` is required not to move when
the second file is written.

On the unfixed kernel (`abda15f595389986c3bff745973503fd`):

```
casrec: dirents A=47 B=48, 4 shared chunk hash(es),
        used_blocks 1402 -> 1406 -> 1406, +4 dedup hit(s)
casrec: used_blocks after unlink A = 1406, after unlink B = 1406;
        4 of 4 chunk block(s) STILL HELD with no file referencing them
```

Every assertion passed and the leak was a printed number, not a red suite. The
reclamation assertion was withheld until the code that makes it true — a suite
expected to be red is a suite nobody reads.

## Phases 2–3 — the design, and why it is not what was asked for

**Refcounts are per-block, in memory, and DERIVED — not a new on-disk field.**
`struct cas_islot` is a 16-byte packed on-disk record and `CAS_SLOTS_PER_BLOCK`
is literally `CAS_BS / sizeof(struct cas_islot)` == 32. Adding a field changes
that divisor, which changes which slot every hash probes to, which makes every
existing volume's index unreadable — a superblock version bump and a reformat,
against a dirty-volume gate whose whole job is asserting durable artefacts
survive across boots.

Deriving is also the stronger property, not merely the cheaper one.
`cas_refs_rebuild()` walks the live directory at mount and at format, so the
count is computed from state that is already journaled: it cannot drift from the
directory, cannot be left stale by a crash between two writes, and heals a
miscount a previous boot introduced. An on-disk counter would have to be
journaled in lockstep with both the bitmap and the directory to claim as much.

**Not atomics.** Every mutation happens under `g_cas_lock` (rank 3), which
already serialises the bitmap, the superblock counters and the shared staging
sectors. A count reaching zero must remove an index entry, clear a bitmap bit
and decrement `used_blocks` as one indivisible step; an atomic on the counter
alone would make the counter race-free while leaving what it guards
inconsistent.

**`cas_get` does NOT increment.** It is the read path — it runs on every file
read, so counting there would climb without bound and never reach zero. A
reference means "a live dirent names this block". The edges are
`vfs_retain_map_locked()` when a map becomes a dirent's, and
`vfs_release_map_locked()` on unlink, O_TRUNC, and the previous map of a
rewrite. `cas_put` does not increment either: it runs while a map is still being
built, including for content a failed write is about to roll back.

**Retain before release.** A rewrite usually stores content it already held, so
identical chunks dedup to the same blocks; releasing the old map first would
drop a shared block to zero, free it, and leave the map just built pointing into
the free pool. Both write paths retain the new map before releasing the old.

**The index needs tombstones.** The table is open-addressed with linear probing
and `cas_index_find` stops at the first slot empty in BOTH fields, so zeroing a
removed slot would cut every entry beyond it out of the index — content present
and referenced, reported absent. The empty test is `hash == 0 && block == 0`, so
`hash == 0 && block != 0` is a tombstone neither loop mistakes for empty, and it
costs no format change. `cas_index_stage` now reuses one, remembering the first
seen and filling it only after the probe proves the hash absent.

**Order inside `cas_free`.** The index entry is removed BEFORE the bitmap bit is
cleared. A block freed while its index entry survives is worse than a leak: a
later `cas_put` of the same content would find that entry, report a dedup hit,
and return a hash pointing at storage since reallocated — the "right byte count,
wrong content" failure v0.56 already found once on this path.

**The map walk includes the indirect blocks.** `ind1_hash`, `ind2_hash` and the
L1 blocks named by `ind2` are ordinary CAS objects. Retain and release share ONE
enumerator so they cannot drift: a release that walked only data chunks would
leak a block per indirect level, and a rebuild that counted only data chunks
would free those same blocks out from under a live file. Data chunks are
released first, because resolving chunk `i` of a large file READS the indirect
blocks. A hash appearing twice in one map is counted twice, deliberately — every
extending write produces repeated zero chunks, and de-duplicating the count
would drop a block to zero while the file still used it.

### What was asked for and deliberately not done

- **"Increment during cas_get"** — see above; it would make the count a read
  tally that never reaches zero.
- **"Preserve open descriptor semantics so unlinked files with active references
  retain their chunks until final close()"** — this kernel has no such state.
  `vfs_unlink` force-closes every descriptor pointing at the file, by design
  since v0.48. POSIX unlink-while-open would be a change to that force-close, not
  a refcounting concern, and inventing it here would have redefined unlink
  silently.

## Phase 4 — measured

Fixed build, uniprocessor tier of the final gate, image
`aedb0cfc6c76b15a59eaa85991137e61`: 45 suites, **506 passed, 0 failed**, ranks 0.

```
casrec: used_blocks after unlink A = 1314, after unlink B = 1310 (started at 1310);
        freed 117 block(s), 431 drop(s), 0 underflow(s)
PASS  unlinking a file whose every chunk is still shared frees NO blocks
PASS  unlinking the LAST file referencing those chunks returns every block to the pool
cas reclaim: 380 block(s) freed, 930 reference drop(s), 0 underflow(s)
```

### Falsifiability

`make EXTRA=-DCAS_RECLAIM_REPRO` frees on the first drop, ignoring sharing —
precisely the implementation v0.48 refused to ship. Re-run against the FINAL
source after the O_TRUNC fix, so both halves of the pair describe one tree
rather than two: image `0eb722937b0c75194c74414717516234`, 45 suites,
**497 passed, 9 failed**.

```
[vfs] SHORT READ 'casrec-b': chunk 0 of 4 hash 4a9109191c5b4325 is NOT in the CAS
casrec: B DIVERGED at byte 0 of chunk 0
FAIL  unlinking one of two files that share EVERY chunk leaves the other byte-for-byte readable
FAIL  unlinking a file whose every chunk is still shared frees NO blocks
PASS  unlinking the LAST file referencing those chunks returns every block to the pool
```

The two reclamation assertions are split for exactly this reason. A reclaimer
that ignores sharing fails the first and passes the second; one that never frees
does the reverse. Neither alone is the property.

The reproducer is LOUD: nine assertions fail across several suites, because
freeing every referenced block corrupts far more than this test's two files.
That is downstream damage of one defect rather than independent breakage — the
`SHORT READ` line names the mechanism exactly, and the casrec pair is what
attributes it. A reproducer this broad is worth keeping only because the two
named assertions localise the cause; the blast radius is context, not evidence.

Logs: `OUTRUN-0.84-casrec-phase1-baseline.log` (unfixed),
`OUTRUN-0.84-casrec-reclaim-fixed.log`, `OUTRUN-0.84-casrec-reclaim-repro.log`.

## The six-tier gate

Image `aedb0cfc6c76b15a59eaa85991137e61`, all ten boots on that one image, clean
rebuild with no compiler warnings or errors.

| tier | suites | passed | failed | ranks |
|---|---|---|---|---|
| uniprocessor | 45 | 506 | 0 | 0 |
| smp2-bios | 45 | 518 | 0 | 0 |
| smp4-bios | 45 | 522 | 0 | 0 |
| smp4-iommu (q35 + VT-d) | 47 | 535 | 0 | 0 |
| gate-dirty (3 boots, one reused image) | 45 | 0 failing each | — | 0 |
| gate-dirty-smp (3 boots, `-smp 4`) | 45 | 0 failing each | — | 0 |

Every fresh tier is +8 against `46978f2` (498/510/514/527): the three Phase 1
control assertions plus the five reclamation assertions, and nothing silently
dropped. Both dirty tiers report empty consecutive-boot assertion diffs and
intact durable artefacts at both widths — the check that had been failing before
the O_TRUNC fix.

## The bug the dirty gate caught, and what it says about the assertions

The first full gate run **FAILED**, and only on the reused-image tier: all four
fresh tiers passed at 506/518/522/535, while `gate-dirty` went 0, 2, 3 failing
assertions across its three boots with consecutive-boot diffs of `new=2` then
`new=1`.

```
[vfs] SHORT READ '/bin/emit': chunk 16 of 17 has no hash (dirent 63, len 8201)
[pipestrs] FAIL vsh's '>' captured a program's stdout into a file
[pipestrs] FAIL vsh's '|' carried one program's stdout into another's stdin
```

Chunk 16 is the first SINGLE-INDIRECT chunk, so the block that had gone missing
was `ind1`.

**The defect was in the O_TRUNC handler added by this work.** It released the
chunk map but zeroed only `len`, `nchunks` and `file_hash`, leaving
`chunk_hash[]`, `ind1_hash` and `ind2_hash` still holding the values it had just
released. The next write to that dirent snapshots `prev` — stale hashes and
all — and releases them a SECOND time. Because that write has usually dedup'd
the same content straight back onto the same blocks, the second release takes a
live block from 1 to 0 and frees it.

Three things worth keeping from that:

- **The `underflow == 0` assertion could not see it, and I had claimed it
  would.** It read zero throughout the failing run, because the count genuinely
  *was* 1 when the second release arrived — an intervening dedup had raised it.
  A double release is only detectable as an underflow when nothing re-references
  the block in between. That assertion is a useful tripwire, not the proof it
  was described as.
- **A fresh volume hides this completely.** The bug needs a populated volume to
  make the intervening dedup near-certain. `gate-dirty` is the only
  configuration that reuses an image, which is exactly why the roadmap named it
  for this work — and it is the tier that would have been skipped by anyone
  running `make gate` rather than `make gate-all`.
- **The growing diff is what made it legible.** `new=2` then `new=1` across one
  image is the signature of accumulating corruption; a flaky assertion does not
  climb.

`vfs_unlink` now clears its map for the same reason. It was already safe there,
but only because `vfs_write_locked` skips the release when `prev.used` is
false — safe by a caller's accident rather than by the dirent's own state.

## Not covered, and one residual

- ~~**Two blocks are allocated but unreferenced at the crash-test remount.**~~
  **ROOT-CAUSED — see `OUTRUN-0.84-cas-unreferenced-residual.md`. It is not a
  leak.** `cas_put()` is reachable with no VFS involvement, and `cmd_cas` uses it
  that way to demonstrate content addressing: two distinct strings, `+2 blocks`,
  named by no dirent and therefore correctly reported as unreferenced by a model
  that counts dirent references. The guess recorded here — "a block put by an
  operation that was rolled back" — was wrong.
- Reclamation is not asserted to be complete — `blocks freed > 0` says it ran,
  and the casrec pair says it is correct for the shared case. There is no
  assertion that every unreferenced block is eventually returned.
- Crash consistency of a free is journaled the same way a put is (index home
  write under `cas_journal_write`, then bitmap/superblock), but no crash was
  injected between those writes. The existing crash test covers puts, not frees.
- Index tombstones accumulate only until reused; a volume that churns far more
  distinct hashes than it has slots is untested.
