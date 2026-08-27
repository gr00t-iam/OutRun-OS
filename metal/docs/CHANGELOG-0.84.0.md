# OutRun OS v0.84.0-metal — storage that gives itself back

Milestone 84. Written into the tree in v0.85, from the annotated tag,
`ROADMAP-0.84.0.md`, and the commits they cover; the tag remains the primary
record and its checksums are reproduced below unchanged.

The VFS learned what a file POSITION means in v0.82 and v0.83. v0.84 finished
the consequences: writes at any size, a volume that can return storage, an
ownership model on the scratch volume, and the two POSIX calls the positional
work had named as missing.

## ARTEFACT

```
outrun-os-0.84.0.iso   (5,877,760 bytes)
MD5    f3b8a3ca3fad7b404f38fea96fdaba0e
SHA256 8aa39aff710a140e11e0f9b8d5c4cdf1cf228992010535dfec16d2d1a7927ceb
```

`make release-verify` PASS — 45 suites, 0 failing assertions, 0 rank faults,
345 s.

## WHAT LANDED

### Positional writes past the staging ceiling (`44e1051`)

A tail-preserving write anywhere in a file larger than `REDIR_STAGE_MAX`
returned ENOSPC, so the only large writes the system could do were whole-file
ones from offset 0. **The ceiling was the staging buffer's, not the
filesystem's**, and it went away by not staging content at all — a file already
IS an array of 512-byte CAS chunks, so a write touches only the chunks its range
covers.

### VOL_TMP ownership (`c8f8e07`, then `c492efc`)

The creator uid arrived guarding **unlink alone**, and its own commit message
named what that left open: any holder of `PCAP_FILESYSTEM` could still read or
overwrite another user's scratch file, having merely been unable to delete it.
That is a leak of **authority**, not of bytes.

`c492efc` closed it — open, read, write and truncate are all judged owner-or-root
now. Still no mode and no group: the rule is owner-or-root, so a tmpfile cannot
be shared, and that difference from the root volume is named in the struct rather
than left to be discovered.

The test needed an **inherited descriptor** to be meaningful. Once open is
owner-or-root, an unprivileged process cannot obtain a descriptor by asking, so a
test that only called `open()` would exercise one guard and leave the other two
unreachable — a check no caller can reach is a check that cannot fail.

### CAS block reclamation (`f118112`, `a881542`, `a48ed1c`)

Since v0.48, unlink had stranded every block a file used; the volume could only
fill. v0.48 said exactly why: dedup lets two dirents share a `chunk_hash`, so
freeing without reference counting corrupts a live file.

**The negative control was written first** (`f118112`) and measured against the
unfixed kernel — two files, one payload, four shared chunks, `used_blocks`
1402 → 1406 → 1406, and 4 of 4 blocks still held after both were unlinked. On
any workload where nothing is shared, a reclaimer that ignores sharing behaves
identically to one that honours it, so a suite that never builds the sharing case
goes green against either.

Counts are **derived** from the live directory at mount rather than stored on
disk: `struct cas_islot` is a 16-byte packed record whose size *is*
`CAS_SLOTS_PER_BLOCK`, so adding a field would make every existing volume's index
unreadable. Deriving is also the stronger property — it cannot drift from the
directory or be left stale by a crash.

**The dirty-volume gate caught a defect the fresh tiers did not.** An O_TRUNC
handler released a chunk map but left the map's hashes in the dirent; the next
write released them a second time, and because that write usually dedup'd the
same content back onto the same blocks, the second release took a **live** block
from 1 to 0. All four fresh tiers passed while it was live. Only `gate-dirty` —
three boots on one reused image — saw it, as `new=2` then `new=1` growing across
boots.

That also exposed an assertion weaker than it claimed: `underflow == 0` was
described as the double-release detector and read zero throughout the failing
run, because the intervening dedup had restored the count.

### `ftruncate` and `O_APPEND` (`6e2e23b`)

Both named as absent by v0.83's own O_TRUNC comment. The append offset is
resolved **inside the lock that performs the write** — resolving it first and
writing second would let two appenders choose the same place and lose one of
them.

`SYS_STAT` was deliberately **not** widened for either: `omake.oc` allocates
exactly two 64-bit words and the kernel `cmemcpy`s `sizeof(st)` into it, so
growing that struct would write past a ring-3 buffer in a program `occ` compiles
during the boot.

### Carryover 3 re-verified (`886750d`)

Closed in v0.78, re-measured at the tip because a session handover described it
as still open. Baseline 496/0; `-DFORK_RACE_REPRO` 493/3 with the stranger named.

## GATE

Development baseline at `6e2e23b`, image `46a405e4…`:

| tier | assertions |
|---|---|
| uniprocessor | 507 |
| smp2-bios | 519 |
| smp4-bios | 523 |
| smp4-iommu | 536 (47 suites) |
| gate-dirty | 3 boots, 0 failing |
| gate-dirty-smp | 3 boots, 0 failing |

## KNOWN, NOT FIXED

All four were carried into v0.85, and three are now closed there:

- **`O_APPEND` atomicity is argued via write-lock discipline rather than
  multi-core stress tested.** *(Closed in v0.85 — measured under four pinned
  appenders and again under 2:1 oversubscription.)*
- **CAS refcount underflow cannot detect a double release masked by an
  intervening dedup.** *(v0.85 design spike: no counter-shaped fix exists; a
  live-versus-derived sweep was built instead, which catches imbalance rather
  than this class.)*
- **VOL_TMP has no mode or group bits.** Deliberate.
- **`lseek`/`SEEK_END` on another user's tmp FD discloses length.** Metadata
  parity with the root volume.
