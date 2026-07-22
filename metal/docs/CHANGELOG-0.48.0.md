# Outrun OS v0.48.0-metal — Production VFS/CAS Extensions & Journaling

A write-ahead journal now protects both the VFS directory and CAS metadata
against an unclean process exit or a power loss mid-write, unlinked files
actually reclaim their directory slot instead of accumulating forever, and
the single-root filesystem is now three volumes — ROOT, an ephemeral TMP,
and a read-only DEV device listing — with a real capability and isolation
boundary between them.

## A real bug found before any of this was designed

Auditing the existing CAS/VFS code before writing anything new (this
milestone's scope — three subsystems touching the storage every other suite
depends on — earned that much caution) turned up a genuine, previously
undetected defect: `cas_format()` hardcoded `SB->dir_blocks = 8`, and
`vfs_flush()`/`cas_mount()` both hardcoded a matching `&& i < 8` clamp on
top of it. `8` blocks is exactly right for the *original* `VFS_MAXFILES`
(16) — `8 * 512 / 256 == 16` — but `VFS_MAXFILES` grew to 24 (v0.43), 26
(v0.46), and 27 (v0.47) without anyone revisiting that constant. Every
dirent at index 16 and beyond has been silently **memory-only** since v0.43:
never written to disk, never restored across a mount. No existing suite
caught it because none of them test dirent persistence past slot 15 across
a reboot — every suite reads back its own writes from the in-memory `g_dir`
array within the same boot, which always worked regardless of what actually
made it to disk.

Fixed with `VFS_DIR_BLOCKS = ceil(VFS_MAXFILES * 256 / 512)` (14, for the
current count of 29), used consistently in `cas_format()`'s layout
computation and as the real clamp in `vfs_flush()`/`cas_mount()`.

## What's new

- **Two independent, narrowly-scoped WAL journals**, both new superblock
  regions (`vjournal_start/blocks`, `cjournal_start/blocks`) computed in
  `cas_format()`'s layout — the on-disk format is now version 3.
  - **CAS-metadata journal**: closes a real crash-consistency hazard this
    audit found in `cas_put`'s original sequence — the index's home-sector
    write landed, then a crash *before* `cas_flush_meta()`'s bitmap/
    superblock write landed would leave the on-disk index saying "hash X is
    at block B" while the on-disk bitmap still said "block B is free". A
    later `cas_put` could then legitimately reallocate block B, silently
    corrupting a different, already-dedup'd file. The journal shadows
    exactly {superblock, the ONE bitmap block touched, the ONE index block
    touched} — not the whole ~18-block metadata region, because `cas_put`
    runs on every write in every existing suite and a wider shadow would be
    a real timing regression for no extra safety. Applied immediately,
    either inline (this transaction's real writes just landed) or replayed
    at the next mount if a crash intervened.
  - **VFS-directory journal**: a single-slot whole-directory shadow.
    Every write or unlink commits the CURRENT `g_dir` into the journal
    payload and marks it PENDING — that commit alone makes the change
    crash-safe, since boot-time recovery replays whatever's PENDING
    regardless of whether anything explicit ever asked for it. Applying
    that payload to the real `dir_start` region is **deferred** — nothing
    copies it there until `SYS_VFS_SYNC` is called or the next boot's
    recovery runs. This is what gives `SYS_VFS_SYNC` a genuine, demonstrable
    purpose instead of a no-op: without it, the disk's directory region
    stays at its last-applied state (still fully recoverable, just not
    eagerly current) until the next mount.
- **`SYS_VFS_SYNC()`** (syscall 22, `PCAP_FILESYSTEM`): applies any pending
  VFS-directory journal commit to disk right now; returns 1 if it applied
  something, 0 if nothing was pending.
- **`SYS_VFS_UNLINK(name)`** (syscall 23, `PCAP_FILESYSTEM`, ROOT volume
  only): zeroes the dirent, journal-commits the change, then force-closes
  any `g_ofiles[]` entry still pointing at it regardless of owner — same
  "regardless of how it got there" philosophy as v0.45's
  `descriptor_teardown_kproc`, just triggered by unlink instead of process
  exit. Deliberately does **not** reclaim the CAS blocks/index slots the
  file's chunks used — see Honest scope gaps.
- **Boot-time recovery**, wired into `cas_mount()` in order: read
  superblock -> `cas_journal_recover()` (may rewrite superblock/bitmap/index)
  -> re-read superblock (recovery may have changed its counters) -> load
  bitmap -> `vfs_journal_apply()` (may rewrite the directory region) -> load
  directory. A version-2 (pre-journal) on-disk volume is detected
  (`SB->version == 2`) and mounted in a **legacy compatibility mode**
  instead of trusting journal-region fields that volume never wrote (which
  would otherwise read as zero — block 0, i.e. the superblock itself — and
  corrupt it): same 8-block directory clamp this kernel always used,
  journaling simply inactive until the volume is reformatted.
- **Multi-volume mounts** via path-prefix routing (`vfs_open_for`), 100%
  backward compatible with every existing suite's bare filenames:
  - **ROOT** (`"name"`, unchanged): the existing CAS-backed, journaled,
    durable filesystem.
  - **TMP** (`"tmp/name"`): a small (4-file, 512-byte) RAM-only scratch
    area — no CAS, no journal, matches real tmpfs semantics (survives a
    process's fds closing, does not survive a reboot).
  - **DEV** (`"dev/devices"`): a read-only text snapshot of the `kdevs[]`
    device registry, built fresh on every read — a live *view*, not a file.
    Deliberately **not** a second way to touch raw MMIO; that's
    `SYS_HW_PASSTHROUGH`/`SYS_VFIO_MAP_BAR`'s job, unchanged. Writes to it
    are denied.
  - `struct ofile` gained a `.volume` field, fixed at open time — there is
    no operation that reinterprets an fd as a different volume's handle,
    which is what makes the boundary an isolation guarantee, not a
    convention. `ofile_deref` now reports the volume alongside the dirent
    index so `SYS_READ`/`SYS_WRITE_FILE` (cases 6/7) can dispatch per-volume.
- **`DEBUG_VFS`** (`g_debug_vfs`, default off): logs every `SYS_VFS_SYNC`
  and `SYS_VFS_UNLINK` call and outcome. Verified live — a captured trace
  showed the exact expected sequence across all 15 `vfsstress` rounds
  (`SYS_VFS_SYNC applied=1` / `SYS_VFS_UNLINK('vfs-stress') -> 0`
  for every round) — then reverted to the shipped default.
- **`cmd_vfs_stress`** (new suite, `vfsstress` command, 15 rounds): a
  ring-3 driver (role 15, `user/init.c`) that reads and overwrites a
  kernel-seeded ROOT file, calls `SYS_VFS_SYNC`, unlinks it and confirms
  it's gone, creates/writes/reads a TMP file, and reads (but is refused
  writing) the DEV volume — all through real syscalls. Plus a direct
  in-kernel proof that deferred apply and automatic recovery both do real
  work: write a file (journal PENDING, disk directory left stale), read the
  RAW on-disk dirent block and confirm it does **not** yet match, then
  force a real remount (`cas_mount()` again, simulating a reboot) and
  confirm the raw on-disk block **now** matches and the reloaded content
  reads back correctly.

  ```
  [vfsstrs]  PASS  every round's driver exited cleanly (ROOT read/overwrite/sync, TMP write/read, DEV read-only all verified in ring 3)
  [vfsstrs]  PASS  no descriptor leaked past any round (open-file table fully released each time)
  [vfsstrs]  PASS  SYS_VFS_UNLINK actually removed the dirent every round (vfs_find fails immediately after)
  [vfsstrs]  PASS  directory dirent count never grew past baseline (unlink+reclaim, not one-new-dirent-per-round)
  [vfsstrs]  PASS  TMP volume slot count never grew past baseline (fixed reused name, not one-new-slot-per-round)
  [vfsstrs]  PASS  free-frame count reconciles (g_frame_free_depth == lifetime freed - reused)
  [vfsstrs]  PASS  the frame allocator's leaf lock never triggered a rank violation (no double-free race)
  [vfsstrs]  PASS  VFS journal commit is genuinely DEFERRED (on-disk dir region is stale before apply)
  [vfsstrs]  PASS  a simulated reboot's cas_mount() automatically recovers the pending commit
  [vfsstrs]  PASS  the recovered directory reloads with correct content (post-recovery read matches what was written)
  [vfsstrs] RESULT: 11 passed, 0 failed
  ```

## Genuine cross-QEMU-reboot proof (beyond the in-kernel simulation above)

The in-kernel test above proves the mechanism using the real recovery code
path, but it's still one process calling `cas_mount()` twice. As a
distinct, stronger proof, two **completely separate QEMU processes** shared
one disk image:

1. Boot 1 ran the full automatic suite, then a manual, suite-external
   `vfscrashwrite` shell command journal-committed one more file
   (`vfs-reboot-test`) and **halted immediately** (`cli; hlt`) — never
   calling `SYS_VFS_SYNC`, never shutting down cleanly. This is a genuine
   simulated power loss: `vfs_write_locked` no longer writes `dir_start`
   directly (v0.47 and earlier did), only the journal.
2. Boot 2 — a fresh `qemu-system-x86_64` invocation against the same disk
   image, no special input — automatically printed:
   ```
   [vfs    ] VFS journal: applied a pending directory commit (seq 260) to disk
   [cas    ] mounted existing volume (v3): 213/8192 blocks used, 437 puts, 278 dedup hits
   [vfs    ] cross-reboot journal probe: 'vfs-reboot-test' found from a PRIOR boot
   [vfs    ] cross-reboot journal probe: content VERIFIED
   ```
   `cas_mount()`'s automatic recovery — not the in-kernel test, not this
   session's harness — is what put `vfs-reboot-test` on disk. If recovery
   didn't work, boot 2 would have read a zeroed (still-formatted) directory
   region and printed nothing at all.

## Honest scope decisions

- **The CAS-metadata journal shadows only 3 blocks, not the whole metadata
  region.** See "What's new" above — a full-region shadow on every
  `cas_put` would be a real performance regression against every existing
  suite's timing budget, for protection this narrower shadow already
  provides for the actual hazard found.
- **The VFS-directory journal is single-slot, not a real transaction log.**
  A second concurrent directory mutation before the first is applied
  overwrites the pending payload rather than queuing both — acceptable
  because `g_vfs_lock` (rank 2) already serializes all directory mutations,
  so there is never more than one in flight to begin with; a genuinely
  ordered multi-entry log is out of scope here.
- **`SYS_VFS_UNLINK` does not reclaim CAS blocks or index slots.** There is
  no reference counting across dirents (two files can share a
  `chunk_hash` via dedup), so freeing a block on unlink could silently
  corrupt a different, still-live file that dedup'd against it. The
  directory slot is reclaimed (that's what "reclamation" means for this
  milestone); the CAS layer's own space is not — CAS volumes only grow.
- **DEV is a read-only text view, not a real device filesystem.** No
  per-device files, no ioctl-equivalent, no writable configuration — a
  richer `/dev` was explicitly out of scope; this exists to prove the
  volume-boundary and capability-check mechanism works, not to replace
  `SYS_HW_PASSTHROUGH`/`SYS_VFIO_MAP_BAR`.
- **TMP is unjournaled and uncapped at 4 files** — deliberately: real tmpfs
  doesn't survive a reboot either, and journaling ephemeral-by-definition
  storage would be pure overhead. 4 slots is enough for this milestone's
  own test; growing it is a one-line change whenever a real need appears.
- **A version-2 volume's legacy-compatibility mode is structurally
  defensive, not exercised by this session's own verification** — every
  boot in this milestone's testing starts from a freshly-formatted (version
  3) volume, since that's how every prior milestone's verification process
  works (fresh `vblk.img` per boot, per config). The legacy path exists so
  a real upgrade-in-place would not read journal-region garbage as if it
  were valid, but it has not itself been boot-tested against a genuine
  version-2 disk image.
- **`VFS_MAXFILES` bumped 27->29** for two more fixed, reused (not
  per-cycle-keyed) names (`"vfs-stress"`, `"vfs-crash-test"`) —
  `cmd_vfs_stress` needs both permanently; this is the exact "reused name,
  not one-new-dirent-per-round" discipline the suite itself proves.

## Verification

| Config | Result |
|---|---|
| `-smp 4` BIOS ISO | 0 FAIL, 24 suites incl. `vfsstress` 11/11 |
| `-smp 4` q35 + intel-iommu | 0 FAIL, 26 suites incl. `iommu` 2/2, `capdma` 11/11, `vfsstress` 11/11 |
| uniprocessor | 0 FAIL, 25 suites incl. `vfsstress` 11/11 (crash/recovery simulation and remount both run identically on one core) |

Compositor/Time-Stream screendump captured at the shell showing live
`WROTE FILE VFS-CRASH-T` / `WROTE FILE VFS-STRESS` journal activity
(`OUTRUN-0.48-vfsstress.png`).

**One flake observed and disclosed, not silently re-rolled:** the first
IOMMU verification attempt hit the exact same `mcpre` (v0.39, unrelated to
this milestone) failure documented in v0.46's and v0.47's changelogs —
`the captured context MIGRATED CORES: started on cpu1, finished on cpu2`.
An immediate re-run with an identical binary and a fresh disk reproduced
0 FAIL across all 26 suites, including `mcpre` 5/5 — the same pre-existing,
timing-sensitive flake, now observed on a third independent occasion.
Nothing in this milestone touches `mcpre`'s IPI-preemption/migration
machinery. The failing attempt's log is kept alongside the clean one
(`OUTRUN-0.48-boot-smp4-iommu-flake.log`) rather than discarded.

## Honest scope gaps

- **No CAS block/index garbage collection on unlink** (see above).
- **The VFS-directory journal is single-slot** (see above) — not a real
  ordered transaction log.
- **DEV is read-only and minimal** (see above).
- **TMP is uncapped-but-tiny and unjournaled by design** (see above).
- **Version-2 legacy compatibility mode is untested against a real
  version-2 disk image** in this session (see above).
- **kproc table still has a `MAX_KPROC` concurrent-live cap** (v0.45,
  unchanged).
- VT-x unavailable under TCG; IOMMU/VT-d fully real; NVMe/UEFI boot-order
  quirk remains unexercised, upstream of the kernel (all unchanged).
