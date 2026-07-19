# Outrun OS v0.42.0-metal — Memory & resource lifecycle: address spaces die completely, physical frames come back

Through v0.41 an address space, once built, was permanent: the frame allocator
was a one-way bump pointer, and a process that exited left its page tables and
data frames simply abandoned. v0.42 closes that loop: **`page_free_tree()`**
walks a dead process's entire user-range page-table hierarchy and hands every
private frame back to a real, concurrency-safe **free list**, and every task-exit
path in the kernel now calls it. A new `leakcheck` suite spawns and tears down
40 real ring-3 processes in a row and proves the physical memory footprint
does not grow after the first one warms the pool.

## What landed

- **`page_free_tree(pml4_phys)`**: bounded to the user PML4 range `[128,192)`
  (exactly what `access_ok` already enforces), it descends
  PML4→PDPT→PD→PT, frees every data leaf and every structural table frame
  bottom-up, and returns the count reclaimed. The kernel's shared low-1-GiB
  identity map (PML4 entry 0) and the shared MMIO window (entry 192) are
  aliased into every process by `create_address_space()` and are outside the
  loop's bounds — never touched, never freed out from under a sibling.
- **A real frame free-list** (`g_frame_freelist`, LIFO, threaded through the
  first 8 bytes of each freed frame — every pool frame is identity-mapped RAM,
  so this needs no side table). `alloc_frame()` now draws from it before
  falling back to the bump path; frames are zeroed on reuse exactly as the
  bump path always zeroed them. Guarded by `g_frame_lock`, a raw leaf
  spinlock deliberately **outside** the v0.41 ranked-klock system: it must
  work before the scheduler exists and on APs, so it is never nested under a
  klock and never held across a yield or an allocation.
- **Wired into every place a task's address space stops being live**: the
  shared BSP+AP executor (`cpu_exec_proc`'s normal-exit branch — the modern
  path used by mcsched/mcq/mcpre/slice/cio and the new suite below),
  `uthread_exit` (the legacy first-class-thread reap path), and
  `handle_cpl3_fault`'s uthread branch (fault termination). Every path already
  called `write_cr3(kernel_cr3)` before reaching this point, so the space is
  provably off the running core before teardown reads and zeroes it.
- **Defensive corruption detection, not silent failure**: `free_frame()`
  rejects out-of-pool addresses and now also refuses a double-free of the
  current list head and a corrupted head; `alloc_frame()` validates a popped
  address is in-pool before dereferencing it. An allocator bug halts with a
  diagnostic instead of quietly handing out garbage — this is what actually
  caught the bug below.

## A real double-free, found and fixed during this milestone's own verification

Wiring `page_free_tree` into every exit path exposed a genuine bug, not in the
new code but in how it interacts with an **existing** subsystem: a surface's
pixel-buffer frames are recycled by `surfaces_reclaim()` onto the surface
subsystem's **own** free list (`g_surf_free[]`) — but those same frames are
also present PTEs in the exiting process's own page tables at `SURF_USER_V`
(PML4 index 166), so `page_free_tree`, walking right behind
`surfaces_reclaim()`, was **also** freeing them into the generic pool. The
same physical frame ended up tracked by two independent free lists at once;
whichever handed it out second overwrote whatever the first one's new owner
had written. Caught via the corruption diagnostics above (`CORRUPT
FREE-LIST: popped pa=... outside the pool`) during the very first `-smp 4`
boot of this milestone, root-caused to the double-management, and fixed by
excluding the surface window's **data leaves** from `page_free_tree` (its
structural PDPT/PD/PT frames are ordinary page-table frames and are still
reclaimed normally — only the pixel data itself is skipped, exactly the
`frame_in_pool()` treatment MMIO already gets, for the same reason).

Chasing this down also surfaced a second, unrelated latent bug in the
**v0.41** surface recycle path: `g_surf_free[]`'s reuse search scanned in
array order rather than most-recent-first, so a residual entry left by the
(also pre-existing, unrelated to this milestone) `cio` suite's own churn test
could shadow a buffer a **later** suite had just freed, breaking the "reclaimed
buffer is recycled by the next create" assertion the `threads` suite has
carried since v0.31. Fixed by searching `g_surf_free[]` LIFO (most recently
reclaimed first) — the same reuse discipline the generic frame allocator now
uses.

## Verified (`leakcheck`, new suite, 6/6)

One kproc slot spawned once, then reused directly across 40 iterations of
`create_address_space → elf_load → run to completion via cpu_exec_proc →
page_free_tree`:

```
[leakchk] 40 iterations of spawn -> elf_load -> run -> exit on ONE kproc slot (pid 32)
[leakchk] 12 frame(s) reclaimed per iteration; high-water after iter 0: 00000000011c9000, after iter 39: 00000000011c9000
[leakchk] this run: +480 freed, +481 reused-from-list; global depth 87, freed 694, reused 607
[leakchk]  PASS  every iteration ran to completion and exited cleanly (exit == pid)
[leakchk]  PASS  every iteration reclaimed the SAME nonzero frame count (deterministic teardown, no drift)
[leakchk]  PASS  the bump high-water mark did NOT move after iteration 0 (steady state: fully satisfied from the free list)
[leakchk]  PASS  free-list depth exactly reconciles with the allocator's lifetime counters (no phantom/lost frames)
[leakchk]  PASS  later iterations were satisfied from the free list, not fresh RAM (reuse actually happened)
[leakchk]  PASS  the frame allocator's leaf lock never triggered a rank violation
```

The high-water mark is **identical** after iteration 0 and after iteration 39
— every one of the 39 remaining builds was satisfied entirely from
previously-freed frames. The reconciliation check (`g_frame_free_depth ==
g_frames_freed - g_frames_reused`, both lifetime counters) is an
unconditional invariant of the allocator's own bookkeeping, checked directly
against absolute counts rather than a fragile per-run delta.

## Verification

| Config | Result |
|---|---|
| `-smp 4` BIOS ISO | 114 PASS / 0 FAIL, 18 suites incl. leakcheck 6/6, threads 7/7 |
| `-smp 4` q35 + intel-iommu | all green: iommu 2/2, capdma 11/11, mcq 6/6, mcpre 5/5, slice 4/4, leakcheck 6/6 |
| uniprocessor | honest degrade: mcq 3/3, mcpre 1/1, slice 1/1 (SKIPs for cross-core checks), leakcheck 6/6 unaffected — the allocator fix is core-count-independent |

The kernel's own pixel-readback (inside the `surfin`/`keys` suites) verified
447 hit-marker pixels and the "HI R3" glyph decode correctly on the IOMMU
run; the external screendump was taken after the final recomposite, whose
camera had already moved to a different window (`TIME-STREAM`) than the one
carrying those markers (`VIDEO-EDITOR`) — a deliberate later state, not a
regression, and the compositor screenshot is included for the record.

NVMe/UEFI remains unexercised for the OVMF boot-order quirk documented since
v0.35 (upstream of the kernel). Reported, not claimed.

## Honest scope gaps

- **Discovered but out of scope, left as-is**: the `cio` suite's descriptor-
  leak check (`"descriptor array fully released after the storm"`) failed
  intermittently in one of several verification boots during this milestone
  (unrelated file-descriptor bookkeeping across concurrent VFS workers, not
  physical frames or page tables). Reproduced once in ~5 boots, absent in the
  three official release-verification runs above. Flagged honestly rather
  than silently re-rolling past it: this is v0.41 territory and needs its own
  investigation, not a fix bundled into a memory-lifecycle milestone.
- **The kproc table itself never shrinks** — `n_kproc` is still a permanent
  bump counter (`MAX_KPROC = 48`); only the *physical frames* backing a dead
  process are reclaimed, not its slot in `kprocs[]`. A genuinely long-running
  system would still exhaust the process table even though memory is fully
  recovered. Recycling kproc slots is separable future work.
- **DMA memory is safely reclaimed but never re-added to a device's IOMMU
  domain tracking on reuse** — page_free_tree frees a process's DMA pages
  like any other private data leaf, but `iommu_domain_add_page`'s own
  bookkeeping for the freed process's grants is not explicitly revoked here
  (v0.37/v0.41's existing revoke path is unaffected; this is about the
  general exit path, not the explicit revoke syscall).
- Prior gaps unchanged: VT-x unavailable under TCG; IOMMU/VT-d fully real.
