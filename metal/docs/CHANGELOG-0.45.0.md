# Outrun OS v0.45.0-metal — Kproc & Descriptor Lifetime Discipline

This milestone makes kproc slots themselves recyclable — closing the last
"never shrinks" gap flagged in v0.42/v0.43's changelogs — and closes the
one exit path that could still leak a resource forever: a file descriptor
left open by a process that never reached its own `SYS_CLOSE`. Building the
stress harness this milestone asked for surfaced two genuine, previously
undetected bugs, both fixed below; neither was cosmetic.

## What's new

- **`kproc_reset(struct kproc *)`** — clears every field of a kproc slot's
  per-process lifetime state (identity, address-space handle, scheduling
  state, the v0.44 DMA grant table) so a slot is indistinguishable from a
  never-used one before its next occupant is installed. Deliberately does
  not touch anything kernel-global — the frame allocator, VFS/CAS tables,
  IOMMU domain tables, or the kernel's own PML4 identity map.
- **Kproc slot recycling.** `kproc_spawn` now scans for a dead, torn-down
  slot before growing the table; `MAX_KPROC` (64) is now a *concurrent-live*
  cap, not a lifetime one. Pid identity is now separate from slot index —
  `g_next_pid` is a monotonic (wrap-safe, skips 0) counter independent of
  which slot a process lands in, so a recycled slot's new occupant can never
  be mistaken for the process that used to be there.
- **`descriptor_teardown_kproc(int proc_idx)`** — force-releases every
  `g_ofiles[]` entry still owned by an exiting process. Before this
  milestone, a descriptor was *only* ever released by the owning process's
  own `SYS_CLOSE` call; a process terminated by a fault in
  `handle_cpl3_fault` instead of running to its own `SYS_EXIT` left its fd
  permanently marked used — this is the fd-leak class the v0.42 changelog
  flagged and left open. Wired into all three exit paths (`uthread_exit`,
  `cpu_exec_proc`'s exit branch, `handle_cpl3_fault`'s uthread branch)
  immediately before the existing `dma_teardown_kproc` call, preserving the
  established teardown order.
- **`DEBUG_KPROC_LIFETIME`** (`g_debug_kproc_lifetime`, default off): logs
  every spawn's slot index, recycled/fresh, and pid; logs descriptor
  counts before/after teardown; panics if a descriptor survives past
  `descriptor_teardown_kproc`. Verified live, then reverted to the shipped
  default before the official runs below.
- **`cmd_kproc_stress`** (new suite, `kpstress` command): 200 spawn/run/exit
  cycles, each a small batch of surface-churn (role 10) and DMA-churn (role
  11) workers, plus a cio/VFS worker (role 9) for the first 6 cycles (see
  "Honest scope decision" below). Every cycle's workers are reaped and torn
  down before the next cycle's `kproc_spawn` calls run, so once the table
  fills once, every later cycle is exercising the recycle path directly —
  something no earlier suite ever touches, since none of them call
  `kproc_spawn` more than once per suite.

  ```
  [kpstrs] cycle 50/200 clean (slots seen so far: 8, recycled spawns: 142)
  ...
  [kpstrs]  PASS  every cycle's workers exited cleanly (exit == pid, recycled or fresh alike)
  [kpstrs]  PASS  no stale DMA grants survived any cycle's teardown
  [kpstrs]  PASS  no stale IOMMU domain (g_proc_slpt) survived any cycle's teardown
  [kpstrs]  PASS  no descriptor leaked past any cycle (open-file table fully released each time)
  [kpstrs]  PASS  recycling actually happened (>= 1 spawn reused an already-seen slot)
  [kpstrs]  PASS  distinct slots used stayed within MAX_KPROC (the table never grew past its cap)
  [kpstrs]  PASS  no stale surface buffer still owned by an exited worker
  [kpstrs]  PASS  free-frame count reconciles (g_frame_free_depth == lifetime freed - reused)
  [kpstrs]  PASS  the frame allocator's leaf lock never triggered a rank violation (no double-free race)
  [kpstrs] RESULT: 10 passed, 0 failed
  ```

## Two real bugs found and fixed while building the harness

### 1. Recycling on `exited` raced the teardown chain it was supposed to wait for

`exited` is set true *early* in every exit path — right after the ring-3
excursion returns, before that same core has run
`descriptor_teardown_kproc`/`dma_teardown_kproc`/`page_free_tree` — so
existing watchdogs (which only ever *read* post-exit state for assertions)
never noticed. The first version of `kproc_spawn`'s recycle scan keyed on
`exited` alone. Under `cmd_kproc_stress`'s actual churn, this raced: a slot
could get handed to a brand-new process — including a fresh
`create_address_space()` overwriting `cr3` — while another core's
`page_free_tree(kprocs[p].cr3)` was still mid-walk on the *old* address
space. That in-flight call would then read the *new* cr3 and free the new,
live process's frames instead, corrupting the free-list. First symptom
observed: `[frame] CORRUPT FREE-LIST: popped pa=..., outside the pool`,
several unrelated allocations later.

Fixed with a new `struct kproc` field, `torn_down`, set exactly once, last,
strictly after `page_free_tree` returns in all three exit paths.
`kproc_spawn`'s recycle scan (and `cmd_kproc_stress`'s own per-cycle
watchdog, which is about to hand these same slots back to `kproc_spawn`)
now wait on `torn_down`, not `exited`.

### 2. `page_free_tree` double-frees a hardware-passthrough device's own MMIO frame

`page_free_tree`'s original design (v0.42) relies on `free_frame`'s
`frame_in_pool()` check to silently reject a passthrough MMIO leaf, since a
real device's PCI BAR lives far outside the frame pool. `cmd_passthrough`'s
demo device (`"sensor0"`) breaks that assumption: it stands in for a
register file with a page from `alloc_frame()` — its "MMIO" physaddr *is*
ordinary pool RAM. Every process granted passthrough to that same demo
device maps the *same* physical page; each one's `page_free_tree` call on
exit tried to free it independently — a genuine double-free of shared
device memory. `cmd_dma_stress`'s four sensor0-grantee workers are the
first thing in this kernel's history to exit *multiple* passthrough
grantees of the same device through the modern path, and kproc recycling
made that a live path for the first time — which is what surfaced it.

Confirmed with a new permanent hardening check, not just theory: a shadow
"is this frame currently free" bit per frame (`g_frame_dbg_isfree[]`),
independent of the free-list's own linkage. The existing
`pa == g_frame_freelist` check only ever caught a double-free of the
*current* head; a double-free with any other free/alloc in between (the
common case under real scheduling) threads silently into the middle of the
list, and the count-based invariant
(`g_frame_free_depth == freed - reused`) can't catch it either — both
counters increment together on a double-free, so the arithmetic still
reconciles even though the list now holds a corrupt duplicate. This is kept
as a permanent, load-bearing check (`[frame] TRUE DOUBLE-FREE (shadow bit)`)
because the milestone's own "no double-frees" requirement needs a check
with real teeth, not just a count that can't see this class of bug.

Fixed with `frame_is_device_mmio(pa)` (`kdev_find(pa) != 0`), checked at all
three data-leaf sites in `page_free_tree` (1 GiB, 2 MiB, 4 KiB) before
calling `free_frame` — a leaf frame belonging to any registered `kdev` is
never process-private, real hardware or not.

## Honest scope decision: cio/VFS coverage in `cmd_kproc_stress` is bounded, not full-200

`cio_file_worker` (unchanged, `user/init.c`) names its own file from its own
pid, and VFS files are durable, global, and never deleted — confirmed in
v0.44 (`cmd_cas`'s own log line: "state persisted to disk"). Every cycle's
role-9 worker would therefore claim a brand-new, *permanent*
`VFS_MAXFILES` (24) directory slot — unlike its kproc slot, its DMA grants,
or its surface buffer, none of which are global, and all of which genuinely
recycle every one of the 200 cycles. Running full VFS churn for all 200
cycles would need roughly 200 dirents; growing `VFS_MAXFILES` to
accommodate an intentionally bounded stress loop would fight the very
discipline this milestone is proving. cio/VFS/CAS coverage is therefore
capped at `KPSTRESS_VFS_CYCLES` (6) — still real, still through the modern
exit path, still with proper descriptor teardown and kproc recycling —
while surface and DMA churn, both genuinely bounded per-process resources,
run for the full 200.

virtio-net is deliberately not one of this suite's roles: its only ring-3
driver (role 1, `nic_driver`) does raw, unsynchronized MMIO register writes
bringing up the one real, non-process-isolated NIC from reset. Running
several of those concurrently and recycling them through 200 cycles would
race live hardware state for no proof this milestone needs. It stays
exercised exactly once, unchanged, through the existing `cmd_nicdriver`
path. virtio-blk is exercised transitively — every VFS read/write in this
suite's role-9 cycles goes straight through `virtio_write_block`/
`virtio_read_block`.

## Verification

| Config | Result |
|---|---|
| `-smp 4` BIOS ISO | 0 FAIL, 21 suites incl. `kpstress` 10/10 |
| `-smp 4` q35 + intel-iommu | 0 FAIL, all green incl. iommu 2/2, capdma 11/11, `kpstress` 10/10 |
| uniprocessor | 0 FAIL, `kpstress` 10/10 (sequential-on-BSP fallback path) |

Compositor screendump captured at the shell on the IOMMU run
(`OUTRUN-0.45-kpstress-iommu.png`).

## Honest scope gaps

- **`DMA_GRANT_MMIO`'s revoke path still has no live-fire proof** (unchanged
  since v0.44) — every device with a real PCI `bdf` is only ever granted
  through the out-of-scope legacy excursion path.
- **VT-x unavailable under TCG; IOMMU/VT-d fully real** (unchanged).
- **No true CPU affinity/pinning** (v0.43, unchanged).
- NVMe/UEFI boot-order quirk remains unexercised, upstream of the kernel
  (unchanged since v0.35).
- **`cmd_leakcheck`'s single-slot reuse is a separate, narrower mechanism**
  from `kproc_spawn`'s recycler, deliberately left untouched: it manages one
  fixed pid across 40 iterations by design (to keep its frame-count
  determinism exact), never calls `kproc_spawn` a second time, and its
  role-6 workers never touch fds/DMA grants/surfaces — `kproc_reset`'s
  extra clearing would be a no-op for it either way.
