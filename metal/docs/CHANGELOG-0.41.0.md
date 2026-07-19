# Outrun OS v0.41.0-metal — cross-core reentrancy: the VFS/CAS/surface stack goes multi-core

v0.40 closed with an honest gap: "the wider syscall table (VFS/DMA/surfaces)
is still exercised from the BSP only; cross-core reentrancy of those
subsystems remains unaudited." v0.41 is that audit, the fix, and the proof.
**Any core may now execute file and surface syscalls concurrently with any
other**, under a ranked spinlock discipline built to make cross-core deadlock
structurally impossible — and a new suite (`cio`) drives the BSP and every AP
into the VFS at the same instant to demonstrate it.

## The audit

Walking every path reachable from the syscall table produced two classes of
defect. The racy-data class was expected: the CAS staging sectors
(`g_blk`/`g_idxbuf`), superblock counters, allocation bitmap, directory
table, descriptor array, surface slot table, pixel-buffer free list, frame
bump allocator, virtio-blk slot scan + avail-ring publish (guarded only by
`cli`, which excludes nothing on another core), and the Time-Stream sequence
counter. The sharper class was **BSP-only scheduler state reached from AP
context** — three paths that would silently corrupt a random BSP thread if a
file or surface syscall ever ran on an AP:

1. `vblk_wait` parked `curthr`/`g_cur` and called `sched_yield()`,
2. `SYS_YIELD` called `sched_yield()` unconditionally,
3. `SYS_SURFACE_FLIP`'s frame-boundary wait called `sched_yield()`.

That, not just data races, is why file IO was BSP-only. Two leaks fell out of
the same walk: a task exiting on an AP never reclaimed its surfaces (only the
BSP's uthread reap path did), and any process could close or write through
any other process's file descriptor.

## The lock-ordering strategy

One global rank order, acquired strictly upward, released LIFO:

| Rank | Lock          | Protects                                              |
|------|---------------|-------------------------------------------------------|
| 1    | `g_ofile_lock`| open-descriptor array: fd alloc/free/deref + owner    |
| 2    | `g_vfs_lock`  | directory table: dirent claim/scan/COW rewrite/flush  |
| 3    | `g_cas_lock`  | superblock counters, bitmap, index, staging sectors   |
| 4    | `g_vblk_lock` | virtio-blk request slots + avail-ring publish         |
| 5    | `g_surf_lock` | surface slot table + pixel-buffer free list           |
| (6)  | `g_next_frame`| frame allocator — LOCK XADD, lock-free, nothing to rank|
| (7)  | `g_conlock`   | console — IRQ-safe leaf inside kprintf only           |

The deep chain is `SYS_WRITE_FILE`: vfs(2) → cas(3) → vblk(4). The surface
chain surf(5) → frame(6) is disjoint from the file chain, so no cycle can
pass through the shared allocator. The rules that make it hold:

- **Ranks 1 and 2 never nest.** This was the one genuine inversion the
  design had to engineer out: `vfs_open` wants name-resolution then an fd
  claim (vfs → ofile), while `SYS_READ` wants fd-deref then a file read
  (ofile → vfs) — classic ABBA. Both are built as two DISJOINT critical
  sections instead: resolve under one lock, release, claim under the other.
  The dirent index a released fd yields stays valid forever (dirents are
  never destroyed), so the worst post-release race is reading a file whose
  fd just closed — benign.
- **No klock is ever acquired in interrupt context.** The virtio bottom half
  touches only per-slot completion flags; `g_conlock` remains the only
  IRQ-side lock.
- **`g_vblk_lock` is never held across a disk wait.** Submit publishes,
  kicks the doorbell, releases; the wait is lock-free on the slot's own
  `done` flag.
- **Ranks 2/3 MAY be held across a blocking disk wait**, and this is safe
  only because a contended acquire never bare-spins the one core that could
  run the holder: on the BSP it yields through the scheduler (the parked
  holder is woken by the IRQ bottom half, finishes, releases), on an AP it
  PAUSE-spins with IF set while the BSP services the completion IRQ.
- **The order is enforced at runtime, not by convention.** Every context
  tracks the ranks it holds — per-THREAD on the BSP (a lock holder can park
  and another BSP thread runs; a per-CPU stack would blame the wrong
  context) and per-CPU on APs, where a task runs its syscall to completion.
  Any non-monotonic acquire counts a violation; `cio` fails on a nonzero
  count. The run recorded **zero** across 4,846 acquisitions.

## What landed

- **CPU-aware blocking.** `vblk_wait` parks the calling thread on the BSP
  exactly as before; on an AP it PAUSE-polls the slot's `done` flag (IF set,
  completion IRQ lands on the BSP, bottom half publishes). `SYS_YIELD` and
  the flip wait go through the same `krelax()` discipline. An AP passes
  waiter_tid −1 — no thread exists to wake.
- **MP-safe virtio-blk submit** under rank 4, with slot exhaustion — now
  reachable with several cores submitting — backing off and retrying instead
  of surfacing a spurious IO error into the VFS.
- **Whole-file write atomicity.** The COW rewrite holds `g_vfs_lock` across
  every chunk's `cas_put` and the directory flush, so no reader or writer
  can observe a dirent whose chunk list is half old, half new. `SYS_WRITE_FILE`
  also stops re-resolving the (unlocked) name — it rewrites by dirent.
- **Descriptor ownership.** Every fd records its owning process; deref,
  write-through and close are owner-checked under rank 1.
- **Surface tables under rank 5.** Slot claim, free-list pop/push, and
  reclaim are serialized; a slot owned by a LIVE foreign process is rejected
  (−16) instead of silently hijacked; a dead owner's slot is reclaimed in
  place; and **every executor now reclaims surfaces on task exit** — the AP
  leak is closed.
- **Atomic frame allocator.** The bump pointer is claimed by LOCK XADD; a
  multi-frame extent is reserved in ONE add so concurrent claims stay
  contiguous.
- **`MAX_KPROC` 40 → 48** (the autorun now spawns six cio workers).

## Verified (`cio`, new suite, 14/14)

Four ring-3 file workers (one EXECUTED BY THE BSP, three across the APs) each
loop open → COW-write a (pid,round)-tagged pattern → read back and verify
EVERY byte → close, on their own file and on one shared file; two surface
churn workers create/flip/recycle surfaces from APs concurrently:

```
[cio    ]   pid 26 'cio-file': exit 26 (want 26)  ran_on 1
[cio    ]   pid 27 'cio-file': exit 27 (want 27)  ran_on 2
[cio    ]   pid 28 'cio-file': exit 28 (want 28)  ran_on 4
[cio    ]   pid 29 'cio-file': exit 29 (want 29)  ran_on 8
[cio    ]   cpu0 dispatched 32 file syscalls
[cio    ]   cpu1 dispatched 32 file syscalls
[cio    ]   cpu2 dispatched 32 file syscalls
[cio    ]   cpu3 dispatched 32 file syscalls
[cio    ] file-syscall in-flight high-water: 4
[cio    ]   lock ofile (rank 1): 832 acquisitions, 4 contended
[cio    ]   lock vfs (rank 2): 168 acquisitions, 68 contended
[cio    ]   lock cas (rank 3): 196 acquisitions, 0 contended
[cio    ]   lock vblk (rank 4): 3629 acquisitions, 0 contended
[cio    ]   lock surf (rank 5): 21 acquisitions, 1 contended
```

- **All four cores inside file syscalls at the same instant** (in-flight
  high-water 4, counted by an atomic witness at the dispatch boundary — not
  sampled), with each of the four cores executing one worker end-to-end
  (`ran_on` 1/2/4/8).
- **Real cross-core contention absorbed correctly**: 68 contended vfs
  acquisitions and every read-back byte-exact; the shared file — 16 racing
  whole-file writes — always read back as ONE uniform image.
- **Durable state re-audited from the kernel afterwards**: no duplicate
  directory names, every live chunk hash resolves in the CAS index,
  `used_blocks == popcount(bitmap)` (59 == 59), all 16 fds released, both
  churn slots reclaimed, free-list chunks pairwise disjoint, no lock held,
  zero rank violations.
- The surface log shows the intended churn: create → reclaim → re-create
  `(recycled pixel buffers)` on both slots, including the executor-exit
  reclaim running on an AP.

## Verification

| Config | Result |
|---|---|
| `-smp 4` BIOS ISO | 146 PASS / 0 FAIL (18 suites incl. cio 14/14) |
| `-smp 4` q35 + intel-iommu | 159 PASS / 0 FAIL (iommu 2/2, capdma 11/11, mcq 6/6, mcpre 5/5, slice 4/4, cio 14/14) |
| uniprocessor | 134 PASS / 0 FAIL — honest degrade: all six workers run sequentially on the BSP, cio 12/12 with the two simultaneity checks SKIPped |

Screendump captured on the IOMMU run at the shell
(`OUTRUN-0.41-cio.png`): the compositor is fully live, and the TIME-STREAM
panel shows the suite's own footprint on screen — `WROTE FILE CIO-SHARED /
CIO-26 / CIO-28 / CIO-29`, the Time-Stream events emitted (via the new
atomic slot claim) by the concurrent VFS writers, with `CAS.BLOCKS 59`
matching the bitmap audit in the log.

NVMe/UEFI remains unexercised for the OVMF boot-order quirk documented since
v0.35 (upstream of the kernel). Reported, not claimed.

## Honest scope gaps

- The locks are per-SUBSYSTEM, not per-object: one vfs lock serializes the
  whole directory, so workers on DIFFERENT files still contend at rank 2,
  and readers serialize against readers (no shared/reader mode). Fine-grained
  here means "the BSP-only restriction is gone", not "maximal parallelism".
- An AP blocked on disk IO burns its core (PAUSE-poll): there is still no
  AP-side kernel scheduler to park a task in, so AP concurrency helps
  throughput only up to the number of cores.
- The compositor's flip consumption and the input-router event queues stay
  lock-free single-producer protocols by OWNERSHIP, not by lock; a compositor
  pass racing a reclaim could blit a just-freed (still-mapped) pixel buffer —
  a one-frame display artifact, never a safety issue. No compositor pass runs
  during cio.
- `cas_format`/`cas_mount` are unlocked by design (they run pre-SMP); the
  Time-Stream slot claim is atomic but a reader can observe a claimed,
  not-yet-filled event.
- The fuzzer still drives the syscall table from the BSP only.
- Prior gaps unchanged: page tables leak on exit; VT-x unavailable under
  TCG; IOMMU/VT-d fully real.
