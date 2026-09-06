# OutRun OS — v1.1 development notes: Phase 1 Tasks 1 & 2

Baseline `d2d051a` (v1.0.0). Nothing here is tagged. Every number below was
produced by a boot named in this file; every image is identified by md5.

---

## What landed

### Task 1 — lock-rank +2 shift, `rank_stack[16]`, allocation invariant

All fourteen `klock` ranks moved up by two in one commit so ranks 0 and 1 are
free for the memory locks. 16 (`g_blk_lock`) and 17 (`g_acpi_lock`) are
reserved in the table comment so Phase 2 and Phase 4 do not each re-shift.

| new | old | lock | | new | old | lock |
|---|---|---|---|---|---|---|
| **0** | — | `g_zone_lock` (v1.1, mm_impl.c) | | 8 | 6 | `g_ipc_lock` |
| **1** | — | `g_swap_lock` (reserved, v1.7) | | 9 | 7 | `g_gpu_lock` |
| 2 | 0 | `g_redir_lock` | | 10 | 8 | `g_audio_lock` |
| 3 | 1 | `g_ofile_lock` | | 11 | 9 | `g_net_lock` |
| 4 | 2 | `g_vfs_lock` | | 12 | 10 | `g_wm_lock` |
| 5 | 3 | `g_cas_lock` | | 13 | 11 | `g_dev_lock` |
| 6 | 4 | `g_vblk_lock` | | 14 | 12 | `g_vm_lock` |
| 7 | 5 | `g_surf_lock` | | 15 | 13 | `g_udb_lock` |

`rank_stack[8]` → `rank_stack[KLOCK_RANK_DEPTH]` (16) in all three structs that
carry one (`cpu_local`, `kproc`, `pcb`). The five sites that spelled the literal
8 — two reset loops, the BSP-adoption copy, and both push ceilings — now use the
constant. Before this a raise that missed one would have been a buffer overrun
in the two that index the array.

**Correction to the task brief, verified by `_Static_assert`.** The brief stated
that growing `rank_stack` in `struct cpu_local` would shift the canary off
`%gs:80` and require updating `CPUL_CANARY`. It does not: `canary` is at offset
80 near the *top* of the struct and `rank_stack` is ~40 fields *below* it, so
growing it moves nothing any contract names. All four existing offset asserts
passed unchanged. A fifth assert was added stating the ordering invariant
itself — `offsetof(rank_stack) > CPUL_CANARY` — so a future reorder that *would*
break the compiler's `-mstack-protector-guard-offset` contract fails the build
instead of silently reading the guard from an unrelated field.

**Allocation invariant** stated at the rank table (kernel64.c, above
`g_rank_violations`): with `g_zone_lock` at rank 0, no code may take it while
holding any ranked lock. In v1.1 nothing does. The tree currently gets away with
allocating under `g_vfs_lock`/`g_cas_lock` only because `g_frame_lock` is
deliberately unranked; ranking the buddy makes that exemption visible.

### Task 2 — `struct page`, buddy allocator, physmap, 4 GiB reach

New files `kernel/mm.c` (types, seam accessors, `mm_note_usable`) and
`kernel/mm_impl.c` (buddy, physmap build, self-test), both `#include`d into the
single TU — the first early enough for `alloc_frame`'s call sites to see the
accessors, the second after `struct klock` exists.

- **`struct page`, 32 bytes, one per PFN**, in `g_mem_map` (10,240 pages at 4 GiB).
  Subsumes and DELETES both legacy side tables: `g_frame_ref[]` → `refcount`,
  `g_frame_dbg_isfree[]` → `flags & PG_FREE`. Same semantics; the accessors
  (`frame_refc_*`, `frame_shadow_*`) are the whole seam. Behavioural difference,
  and it is an improvement: the old arrays stopped tracking at 256 MiB
  (`FRAME_DBG_MAX`); `struct page` covers all of RAM.
- **Physmap at `PHYSMAP_BASE = 0xFFFF888000000000`** (PML4 slot 0x111), built by
  `mm_init()` from the multiboot2 memory map that `multiboot_scan` has printed
  since v0.1 and never used for sizing. 2 MiB NX leaves; `PHYS_TO_VIRT()` valid
  for every usable frame. Aliased into each process by `create_address_space`
  alongside slot 0 and 0xC0.
- **Buddy allocator**, `MAX_ORDER` 11, `alloc_pages`/`free_pages` under
  `g_zone_lock` (rank 0). Owns every usable frame **above** 1 GiB; the legacy
  LIFO/bump pool keeps `[16 MiB, 1 GiB)` marked `PG_LEGACY`, and `free_pages`
  refuses a legacy frame with a named halt. Two allocators, disjoint ownership.
- **`boot.asm` identity map 1 GiB → 4 GiB** (four PDs). Required, not optional:
  QEMU places ACPI tables at `0xBFFE0000` on a 4 GiB guest and `acpi_find_table`
  runs before any physmap exists. **Negative control:** unmodified v1.0 at
  `-m 4G` panics identically (`cr2=00000000bffe1c3b`, PDPT[2] not present).
- **`IDENT_MAP_LIMIT` is unchanged at 1 GiB** and still bounds the legacy pool.
  What changed is the range that is *addressable*, not the range the old
  allocator hands out. That is what makes v1.1 the negative control for v1.2.
- `harden_kernel_wx` now NX-marks the huge leaves in GiBs 1–3. The parallel
  page-table audit caught this first (1536 W+X, three GiB of exactly 512) —
  the audit doing exactly what it was built to do.
- `AUDIT_MAX` 4096 → 16384. A 4 GiB boot enumerates exactly 4096 PDEs (2048
  identity + 2048 physmap) and the "no truncation" assertion correctly refused
  to distinguish full from overflowing.

### Not done: Task 3

`kernel/files.c` exists as an uncommitted scaffold (`files_struct` growth,
`fd_alloc_install`, `fd_release_locked`, `ofile_new_locked`) and is not wired
in. The wiring touches ~200 sites (152 `g_ofiles`, 47 `owner_mask`, 37 hard
`fd < 16` bounds). One semantic fork was put to the user: fd numbers become
process-local (POSIX), which means `epoll_ctl`, `SYS_SETREDIR` and IPC
fd-transfer stop passing raw global indices. **Decision: POSIX.** Epoll watches
will key on `(owner, fdnum)`; IPC transfer installs a fresh number in the
recipient. That is v1.3's own commit.

---

## Evidence

### Task 1 alone — `make gate`, 512 MiB, image `b20017b09dc36d7eee2edfd146b2097e`

| tier | suites | passed | failed | ranks | time |
|---|---|---|---|---|---|
| uniprocessor | 45 | 566 | 0 | 0 | 470 s |
| smp4-bios | 45 | 586 | 0 | 0 | 450 s |
| smp4-iommu | 47 | 611 | 0 | 0 | 465 s |

`FRESH-IMAGE MATRIX: PASS`. `gate-selftest` 17/17 first. All three inside the
900 s `GATE_CAP`. Log dir `.logs/gate/matrix-1332`.

### Task 1 + 2 — single boot, **4 GiB**, image `68cb8f6e6a57b4504faf2e83401f0e8f`

46 suites (45 baseline + `[mm]`), **576 passed, 0 failed**, 0 rank violations,
0 underflow, 0 mismatch, prompt reached. `[mm]` 10/10:

```
[mm     ] physmap: 4096 MiB mapped at ffff888000000000 (2 MiB pages, NX); mem_map 10240 pages
[mm     ] buddy: 786397 frames released above 1 GiB, 258048 legacy frames kept by the v0.42 pool
[mm     ] ok   frames came from above the 1 GiB ceiling
[mm     ] ok   free returns every frame (nr_free restored)
[mm     ] RESULT: 10 passed, 0 failed
```

Archived: `metal/docs/OUTRUN-1.1-mem4g-pass.log`. Booted with the same
virtio-blk + virtio-net devices the gate attaches (`tools/devboot.sh`).

### Task 1 + 2 — `make gate`, 512 MiB, image `50e3810a75e9ae873feb8815bb799367`

| tier | suites | passed | failed | ranks | time |
|---|---|---|---|---|---|
| uniprocessor | 46 | 569 | 0 | 0 | 475 s |
| smp4-bios | 46 | 589 | 0 | 0 | 455 s |
| smp4-iommu | 48 | 614 | 0 | 0 | 470 s |

`FRESH-IMAGE MATRIX: PASS`. All three inside the 900 s `GATE_CAP`. Log dir
`.logs/gate/matrix-3425`. Suite counts are +1 over the Task-1-only run because
`[mm]` now reports on every boot (its allocation checks are NOT RUN below 1 GiB
and it says so; the 3 structural checks and the RESULT line always emit).

**Across both gates and the 4 GiB boot: 3,515 assertions, 0 failed, 7 boots.**

---

## Not covered — stated, not implied

- **Dirty-volume tiers** (`gate-dirty`, `gate-dirty-smp`) were not run.
- **`gate-oversub`** was not run.
- **`-smp 2`** was not in the requested config set.
- **The 4 GiB boot is one boot, uniprocessor.** No `-smp 4 -m 4G` boot exists.
- **`[mm]` on a 512 MiB guest reports its allocation checks NOT RUN** — the
  buddy owns nothing below 1 GiB by design. Only the 4 GiB boot exercises it,
  and there is no gate tier for that yet (`gate-mem4g` is v1.2 work).
- **Nothing allocates from the buddy on any existing path.** v1.1 is
  deliberately behaviour-identical for every pre-existing allocation; the
  seam accessors are the only code that runs differently. That is the point,
  and it means the buddy's only coverage is `mm_selftest`.
- **The legacy pool is still capped at 1 GiB.** Frames above it are reachable
  and allocatable through `alloc_pages` only. Converting the 79 `alloc_frame()`
  sites is v1.2.
- **`PG_PINNED` is defined and unused.** No reclaim exists to honour it yet.

## Three things found by booting, not by reading

1. ACPI tables above 1 GiB on a 4 GiB guest — would have blocked every
   `-m 4G` boot regardless of the memory work.
2. `harden_kernel_wx` hardening one GiB of a four-GiB map — caught by the
   audit suite, not by inspection.
3. `alloc_frame()` through the physmap from a *process* CR3 — the physmap was
   kernel-global in exactly the way the MMIO window is, and needed aliasing
   the same way.

None of these was in the plan. All three are the class of thing a plan cannot
contain and a boot always does.
