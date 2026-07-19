# Outrun OS v0.44.0-metal — DMA / IOMMU Lifetime Discipline

This milestone closes the exact hazard v0.42's changelog flagged as a known
scope gap: a process's IOMMU second-level page table
(`g_proc_slpt[proc_idx]`) is not part of that process's own CR3 hierarchy, so
`page_free_tree` — which only walks the process's PML4 — structurally cannot
see it. Before this milestone, nothing ever detached or freed a domain on
kproc exit; a process that had been granted DMA/passthrough access left its
IOMMU mappings live forever, even after every one of its physical frames was
reclaimed. v0.44 makes every DMA window a tracked, revocable object with a
lifetime tied to its owning kproc, and adds a real, live-fired test for the
whole path — not just the parts that were already being exercised.

## What's new

- **`struct dma_grant`** — a small fixed record (`phys`, `size`, `bdf`,
  `flags`, `used`) — and a per-kproc table of `MAX_DMA_GRANTS` (8) of them,
  appended to `struct kproc` (`dma_grants[]`, `dma_grant_count`) following
  this codebase's established append-only field-growth convention.
- **`dma_grant_create(kproc*, phys, size, flags, bdf)`** — the single entry
  point every DMA/passthrough grant now goes through. It records the grant,
  then does exactly what the two call sites used to do inline:
  `iommu_domain_add_page` for each page of a `DMA_GRANT_PAGE` grant, or
  `iommu_attach_proc_domain` for a `DMA_GRANT_MMIO` grant. Grants are tracked
  **per allocation, not per page** — `SYS_DMA_ALLOC` can request up to 64
  pages in one call, which would blow past an 8-entry table if each page
  claimed its own slot; one grant now covers the whole contiguous
  allocation, looping internally only for the underlying page-table calls.
- **`dma_grant_revoke(kproc*, grant)`** — tears down one grant:
  `iommu_detach_to_kernel(bdf)` for an MMIO grant (returns the device to the
  kernel identity domain), or nothing device-side for a PAGE grant (the
  domain itself is discarded wholesale by `dma_teardown_kproc`, see below) —
  either way, clears `used` and decrements `dma_grant_count`. It does **not**
  free `phys` itself; that's `page_free_tree`'s job, and it still runs
  afterward, unchanged.
- **`dma_teardown_kproc(proc_idx)`** — walks the exiting process's grant
  table, revokes every live grant, and then does the thing that could never
  happen before this milestone: destroys `g_proc_slpt[proc_idx]`'s IOMMU
  domain and zeroes the slot. Wired in immediately before all three existing
  `page_free_tree` call sites (`uthread_exit`, `cpu_exec_proc`'s exit branch,
  `handle_cpl3_fault`'s uthread branch) — DMA/IOMMU teardown always happens
  *before* the frame reclaim it depends on being able to still see.
- **`sys_hardware_passthrough`** and **`SYS_DMA_ALLOC`** now call
  `dma_grant_create` instead of touching the IOMMU directly.
- **`DEBUG_DMA_LIFETIME`** (`g_debug_dma_lifetime`, default off): logs every
  grant create/revoke (kind, phys/bdf, size), logs an attempted revoke of a
  non-existent grant, and panics if `dma_teardown_kproc` ever finds a live
  IOMMU domain still mapped after every tracked grant was revoked — the
  exact condition that would mean a DMA window survived process exit.
  Verified live: armed the flag, confirmed clean CREATE/REVOKE pairs for
  every `PAGE`-kind grant through a real scheduled exit, then reverted to
  the shipped default before the official verification runs below.
- **`cmd_dma_stress`** (new suite, `dmastress` command): ten workers — 4× a
  new role (11, `dma_churn` in `user/init.c`: grant a passthrough MMIO
  window, `SYS_DMA_ALLOC` a page, touch it, exit), 4× role 9 (VFS), 2× role
  10 (surface) — biased round-robin across every online core (or run
  sequentially on the BSP under uniprocessor), joined with a watchdog, then
  six assertions: no timeout, every exit code equals its own pid, every
  worker's grant table is empty, every worker's `g_proc_slpt` entry is
  zeroed, no descriptor leaks, and the global free-frame count reconciles.

  ```
  [dmastrs] 10 workers (4 DMA-churn + 4 VFS + 2 surface) biased round-robin across 4 cores
  [dmastrs]   pid 42 role 11: exit 42 (want 42) grants=0 slpt=0000000000000000 finish#29
  ...
  [dmastrs]  PASS  no watchdog timeout — every worker reached a terminal state
  [dmastrs]  PASS  every worker's exit code == its pid (DMA/passthrough work was correct)
  [dmastrs]  PASS  every worker's DMA grant table is empty after exit (all revoked)
  [dmastrs]  PASS  every worker's IOMMU domain (g_proc_slpt) was freed and zeroed on exit
  [dmastrs]  PASS  no descriptor leaks (open-file table fully released)
  [dmastrs]  PASS  free-frame count reconciles (g_frame_free_depth == lifetime freed - reused)
  ```

## Why a new user-program role was necessary, not optional

Auditing the existing DMA-granting call sites (`dma-owner`/`dma-other`'s
`cmd_capdma`, `nic-driver`'s `cmd_nicdriver`) found that all of them run
through the **legacy** `enter_process`/`resume_kernel` synchronous excursion,
not the modern scheduled `kproc_spawn` + `cpu_exec_proc` path — an
established, intentional scope boundary documented since v0.42 (the legacy
path never reaches any of the three wired exit hooks). Arming
`DEBUG_DMA_LIFETIME` and booting confirmed this concretely: CREATE lines
appeared for all three legacy-path grants, and **zero REVOKE lines ever
appeared** for them, for the mundane reason that those processes never exit
through code that calls `dma_teardown_kproc`. That's not a bug in this
milestone — the legacy path's exit behavior is out of scope, exactly as
documented before — but it meant the new revoke logic had **no live
coverage** without a genuinely new workload that reaches a modern exit while
holding a real grant. That's what role 11 / `cmd_dma_stress` is for.

## Verification

| Config | Result |
|---|---|
| `-smp 4` BIOS ISO | 0 FAIL, 20 suites incl. `dmastress` 6/6 |
| `-smp 4` q35 + intel-iommu | 0 FAIL, all green incl. iommu 2/2, capdma 11/11, `dmastress` 6/6 |
| uniprocessor | 0 FAIL, `dmastress` 6/6 (all workers run sequentially on the BSP) |

Compositor/dma-stress screendump captured at the shell on the IOMMU run
(`OUTRUN-0.44-dmastress-iommu.png`).

**One test-harness mistake caught before it was reported as a kernel bug:**
the first uniprocessor run reused the same `vblk.img` the BIOS run had just
written to. OutrunOS's VFS/CAS layer genuinely persists file state to the
virtio-blk disk across boots by design (`cmd_cas`'s own log line says so:
"state persisted to disk") — that's correct, intended behavior, not a leak.
Reusing a disk image the BIOS boot had already populated meant the
uniprocessor boot's fresh set of VFS writes landed on top of the *previous
boot's* surviving directory entries instead of a clean volume, and the
combined total exceeded `VFS_MAXFILES` (24), producing four `[dmastrs]`
`exit 700` failures that looked like a real regression at first read. A
fresh, reformatted disk image for the uniprocessor run reproduced 0 FAIL
with the identical kernel binary — confirming this was disk-image reuse
across separate boots, not a DMA-lifetime defect. Recorded here rather than
silently re-run, per this project's standing practice of disclosing
anything that could be mistaken for a real bug.

## Honest scope gaps

- **`DMA_GRANT_MMIO`'s revoke path (`iommu_detach_to_kernel`) has no live-fire
  proof in this milestone.** The only devices ever granted with a genuine
  PCI `bdf` are granted through the legacy excursion path (out of scope, see
  above); `cmd_dma_stress`'s role-11 workers request passthrough on the
  "sensor0" scratch device, which — like every `kdev_register()`-only
  device — carries `bdf=0xFFFF`, so their grants and revokes are always
  `DMA_GRANT_PAGE`. The `PAGE`-grant revoke path (the one v0.42 originally
  flagged, and the one `g_proc_slpt` teardown depends on) is fully proven
  end-to-end through a real scheduled exit. The `MMIO`-grant branch is
  implemented and structurally identical, but unexercised by any live test
  here — a real device with a PCI `bdf` reaching a modern scheduled exit
  while holding an MMIO grant does not exist yet in this kernel's test
  surface.
- **kproc table still never shrinks** (v0.42, unchanged).
- **VT-x unavailable under TCG; IOMMU/VT-d fully real** (unchanged).
- **No true CPU affinity/pinning** (v0.43, unchanged).
- NVMe/UEFI boot-order quirk remains unexercised, upstream of the kernel
  (unchanged since v0.35).
