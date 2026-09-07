# OutRun OS — Architectural Specification & Roadmap: v1.0 → v2.0 structural parity

Status: DRAFT specification. Nothing in this document has been built or measured.
Baseline: `main` @ `d2d051a` (v1.0.0 tagged), kernel64.c 37,313 lines, init.c 7,356 lines.
Author role: Principal OS Architect. Benchmark: Linux 6.x / Windows NT structural parity.

---

## 0. Baseline correction — what the brief assumed vs. what the tree contains

A roadmap written against an imagined baseline is a roadmap that plans work
already done and omits work that blocks everything else. This section is the
result of reading the tree, and it changes the phase ordering in §7.

### 0.1 Already implemented (do NOT re-plan)

| Brief item | Reality in tree | Evidence |
|---|---|---|
| Copy-on-write | **Shipped, v0.63.** Refcounted frames, `PTE_COW`, sole-owner fast path, double-fork correctness fix. | `vm_fault_handle` kernel64.c:14848; `vm_clone_user` :23353 |
| Demand paging (anon) | **Shipped.** `PTE_ZFOD` parks permissions in the non-present PTE; faulted in on touch. | kernel64.c:14765 |
| File-backed demand paging | **Shipped, v0.66.** `MAP_PRIVATE` copies, `MAP_SHARED` via page cache with fault-driven dirty tracking. | kernel64.c:14772–14838 |
| Page cache | **Exists but is 32 entries, fixed, no eviction.** | `MAX_PCACHE 32`, `g_pcache[]` :1405,1427 |
| Per-CPU runqueues + work stealing | **Shipped, v0.39/v0.49.** Non-blocking `rq_trylock` steal, isolated-core policy, IPI preemption, affinity masks. | `rq_steal` :16500 |
| POSIX process model | fork/execve/waitpid/signals/pipes/pgid/futex/threads/epoll/eventfd — 117 syscalls. | init.c:26–188 |
| Credentials | Full real/effective/saved uid+gid triple, `SYS_AUTH`, scrypt KDF. | `struct kproc` :1952–1963 |
| Capability masks | 14 `PCAP_*` bits gating hardware/IPC/VFIO/GPU/audio/WIMP/console. | :1848–1866, :3190 |
| IOMMU + PCI passthrough | DMAR parse, domain management, `SYS_CLAIM_PCI_DEVICE`, BAR mapping, MSI-X. | v0.95 syscalls 111–116 |
| W^X | Enforced at `elf_load` per segment and at `mprotect`. | :28636, :21824 |
| Stack protector | `-fstack-protector-strong`, guard at `%gs:80`, per-thread entropy. | :365–381 |

### 0.2 Absent, and blocking (the real Phase 1)

| Gap | Consequence | Evidence |
|---|---|---|
| **Physical memory ceiling of 1 GiB** | `alloc_frames` hard-refuses past `IDENT_MAP_LIMIT 0x40000000` because boot.asm identity-maps exactly 1 GiB with 2 MiB pages. A self-hosting GCC does not fit. | :1002 |
| **No page reclaim of any kind** | Frame allocator is bump + LIFO freelist. No LRU, no clock, no swap, no shrinker. Memory pressure = allocation failure = fault. | `alloc_frame` :1199 |
| **Global 16-entry descriptor table** | `static struct ofile g_ofiles[16]` is SYSTEM-WIDE, not per-process. `make` + `cc1` + `as` + pipes exhausts it instantly. | :10265 |
| **No NVMe driver** | `make qemu-nvme` boots through OVMF firmware; the kernel then has no way to read the device it booted from. Zero occurrences of "nvme" in kernel64.c. | grep, `Makefile:665` |
| **ELF loader refuses dynamic binaries by design** | `PT_INTERP` and `PT_DYNAMIC` are explicit rejections with named reasons. No relocation processing exists. | :28607–28613 |
| **No libc** | init.c and apps/ are freestanding, hand-rolled against raw syscall stubs. | apps/*.c |
| **ACPI is table discovery only** | RSDP → RSDT/XSDT → signature lookup, used to find DMAR. No AML interpreter, no FADT/DSDT execution, no S-states, no C/P-states, no thermal. | `acpi_find_table` :4474 |
| **No DRM/KMS** | virtio-gpu 2D resources + scanout only; no mode objects, no atomic commit, no GEM-equivalent handles, no 3D. | `PCAP_SURFACE` path |
| **No loadable modules** | Kernel is one statically-linked translation unit. `kdf-test` is a *host-side* unit test of `crypto/scrypt.c`, unrelated to a driver framework — the acronym collision is misleading. | `Makefile:628` |
| **No SMEP/SMAP/UMIP/KASLR/CFI/TPM** | Zero `CR4` hardening writes, zero `stac`/`clac`, fixed kernel load address. | grep |
| **Static sizing throughout** | `MAX_KPROC 64`, `MAX_THREADS 16`, `MAX_FMAP 8`, `MAX_DMA_GRANTS 8`, `NWMWIN 12`, `VFS_MAX_CHUNKS 16`. | :2266, :3316, :1404 |
| **VFS is a flat CAS directory, not an inode/dentry graph** | `struct dirent` is a fixed 256-byte record with a flat 64-char name; no directories, no hierarchy, no mountpoints, no procfs/sysfs. | :6454–6512 |

### 0.3 Consequence for the requested phase ordering

The brief asks Phase 1 to be "musl porting and NVMe storage". **Neither is
reachable from the current tree without prior work**, and attempting them first
produces the class of failure CLAUDE.md is organised against — a milestone that
appears to pass because the workload could not reach the defect:

- musl needs `open`/`read` on a **hierarchical** path, a **per-process** fd
  table larger than 16, `mmap` of a **dynamic** ELF, and a `brk` beyond 4 MiB.
  Ported onto today's tree it would link, run one static hello-world, and be
  declared done while exercising none of the paths it exists to provide.
- NVMe needs >1 GiB of addressable physical memory for queue rings plus DMA
  buffers under the IOMMU, and a block layer to sit under — today's `virtio-blk`
  path is called directly, with no request abstraction between it and the VFS.

So Phase 1 below is **memory + fd + block layer foundations**, Phase 2 is
**NVMe + hierarchical VFS**, Phase 3 is **musl + ld.so + self-hosting**,
Phase 4 is **ACPI/DRM/KDF + hardening**. The brief's two named priorities land
in Phases 2 and 3 respectively, as early as they can honestly be attempted.

---

# Pillar 1 — Advanced Virtual Memory & Scheduling

## 1.1 Physical memory: escape the 1 GiB identity map

**This is the single highest-ROI item in the document** and it gates every
other pillar. The multiboot2 memory map is already parsed (`tag->type == 6`,
kernel64.c:832) and then discarded — the numbers are printed and never used to
size an allocator.

### 1.1.1 Target: direct physical map + buddy allocator

```c
/* metal/kernel/mm.h — NEW */

/* The kernel gains a linear map of ALL physical RAM at a fixed high offset,
 * replacing the 1 GiB identity map. 2 MiB pages, built at boot from the
 * multiboot2 mmap. PHYS_TO_VIRT is then valid for every frame, which is what
 * lets the allocator hand out memory above 1 GiB at all. */
#define PHYSMAP_BASE   0xFFFF888000000000ull      /* -120 TiB, Linux-style      */
#define PHYS_TO_VIRT(pa) ((void *)((uint64_t)(pa) + PHYSMAP_BASE))
#define VIRT_TO_PHYS(va) ((uint64_t)(va) - PHYSMAP_BASE)

#define MAX_ORDER 11                               /* 4 KiB .. 4 MiB contiguous */

/* One per 4 KiB frame. Kept deliberately at 32 bytes: at 16 GiB of RAM that is
 * 128 MiB of metadata, which is the price of being able to reclaim at all.
 * The existing g_frame_dbg_isfree[] debug array is subsumed by ->flags. */
struct page {
    uint32_t          flags;        /* PG_* below                              */
    uint32_t          order;        /* buddy order when free; 0 when allocated */
    volatile int32_t  refcount;     /* replaces the current frame_share table  */
    int32_t           mapcount;     /* PTEs pointing here; drives COW decisions */
    struct page      *lru_next, *lru_prev;   /* active/inactive list linkage   */
    uint64_t          private;      /* page-cache: (dirent<<32)|pgidx, or swap */
};

#define PG_FREE       (1u << 0)
#define PG_RESERVED   (1u << 1)     /* kernel image, ACPI, MMIO holes          */
#define PG_LRU        (1u << 2)     /* on a reclaim list                       */
#define PG_ACTIVE     (1u << 3)     /* active list (vs inactive)               */
#define PG_REFERENCED (1u << 4)     /* clock hand's accessed bit, accumulated  */
#define PG_DIRTY      (1u << 5)
#define PG_WRITEBACK  (1u << 6)     /* I/O in flight; must not be freed        */
#define PG_ANON       (1u << 7)     /* swap-backed, not file-backed            */
#define PG_PINNED     (1u << 8)     /* DMA target / IOMMU-mapped: never evict  */

struct zone {
    uint64_t          base_pfn, nr_pages;
    struct page      *free_area[MAX_ORDER];   /* buddy free lists, per order   */
    uint32_t          free_count[MAX_ORDER];
    struct page      *active, *inactive;      /* LRU lists (two-list clock)    */
    uint32_t          nr_active, nr_inactive;
    uint32_t          watermark_min, watermark_low, watermark_high;
    struct klock      lock;                   /* NEW RANK — see §1.1.3         */
    uint32_t          node;                   /* NUMA node id (SRAT-derived)   */
};

struct page *pfn_to_page(uint64_t pfn);
uint64_t     page_to_pfn(const struct page *p);

struct page *alloc_pages(uint32_t order, uint32_t gfp);
void         free_pages(struct page *p, uint32_t order);

#define GFP_KERNEL   0x01u   /* may sleep, may trigger reclaim                 */
#define GFP_ATOMIC   0x02u   /* IRQ context: never reclaims, may fail          */
#define GFP_DMA32    0x04u   /* must land below 4 GiB (legacy device rings)    */
#define GFP_ZERO     0x08u
```

**Migration constraint that must not be got wrong.** `alloc_frame()` currently
returns a *physical* address that callers dereference directly, because the
first 1 GiB is identity-mapped. Every one of those dereferences is a latent bug
once frames come from above 1 GiB. The migration is therefore **two commits, not
one**:

1. Introduce `struct page` + buddy + physmap, keep `alloc_frame()` as a wrapper
   that still returns a physical address, and add `_Static_assert`-backed
   accessors. Boot must still pass `make gate` unchanged.
2. Convert dereference sites to `PHYS_TO_VIRT()` mechanically, then raise the
   ceiling. Do **not** raise the ceiling first: an untranslated dereference of a
   frame at 2 GiB faults not-present inside the allocator's own zeroing loop,
   which is precisely the failure kernel64.c:1002 documents.

### 1.1.2 Reclaim: two-list clock with swap

```c
/* metal/kernel/swap.h — NEW */

#define SWAP_SLOT_INVALID 0u

struct swap_device {
    int       volume;              /* backing volume (CAS or raw block)        */
    uint32_t  nr_slots;
    uint8_t  *slot_bitmap;         /* 1 bit per 4 KiB slot                     */
    uint32_t  next_hint;
    struct klock lock;
};

/* A swapped-out anonymous page leaves a NON-PRESENT PTE carrying its slot, in
 * exactly the style PTE_ZFOD already uses to park permissions in a non-present
 * entry. Bit 0 clear = not present; bits 12..43 = slot; PTE_SWAP marks it as
 * a swap entry rather than a ZFOD promise, and the two are mutually exclusive.
 * Keeping this discipline is what lets vm_fault_handle stay a flat chain of
 * mutually-exclusive tests rather than becoming a decision tree. */
#define PTE_SWAP      (1ull << 10)         /* AVL bit; free in this kernel     */
#define SWP_SLOT(pte) (((pte) >> 12) & 0xFFFFFFFFull)
#define SWP_ENTRY(slot, keep) (((uint64_t)(slot) << 12) | PTE_SWAP | (keep))

int  swap_out(struct page *p, uint64_t *pte);   /* -> writes slot into *pte    */
int  swap_in(uint64_t *pte, uint64_t cr3, uint64_t va);
```

Reclaim policy — the **clock/second-chance** variant, chosen because this kernel
has no hardware-assisted reference sampling thread and cannot afford a true LRU
scan:

- `kswapd` runs as an ordinary kernel thread (`g_threads[]` already supports
  this) woken when `zone->free_count` drops below `watermark_low`.
- It scans `zone->inactive` from the tail. For each page: if the accessed bit is
  set in any mapping PTE, clear it, set `PG_REFERENCED`, and rotate to the head
  of `inactive` (second chance). Otherwise evict.
- Eviction of a **clean file page** = drop it; it can be re-read.
  Eviction of a **dirty file page** = writeback via the block layer (§2.2),
  set `PG_WRITEBACK`, free on completion.
  Eviction of an **anonymous page** = `swap_out`.
- `PG_PINNED` pages are skipped unconditionally. Every frame currently reachable
  through a `dma_grant` or an IOMMU domain mapping **must** be pinned at grant
  time — evicting a frame a device is DMA-ing into is silent corruption that no
  existing assertion would catch.

**Accessed-bit sampling requires `mapcount` to be correct**, which requires a
reverse mapping. Full rmap is out of scope for v2.0; instead every `struct page`
that is file-backed records `(dirent, pgidx)` in `->private` and anonymous pages
are reclaimed only from the faulting process's own tables during a
`madvise`/exit walk. This is a **stated limitation**: shared anonymous pages
(post-fork COW pages held by two processes) are not reclaimable in v2.0. Record
it in the release notes rather than discovering it as a leak.

### 1.1.3 Lock rank additions

CLAUDE.md's rank table must stay complete. Three new ranks are required, and
they are inserted **below** `g_ofile_lock` because allocation happens under
almost every other lock:

| rank | lock | protects |
|---|---|---|
| **-2** | `g_zone_lock[]` | per-zone buddy free lists + LRU lists |
| **-1** | `g_swap_lock` | swap slot bitmap |
| 0 | `g_redir_lock` | (unchanged) |

Negative ranks are not acceptable in the existing `uint8_t rank_stack[8]`
encoding. **Therefore the whole table shifts by 2** and every existing
`klock` initialiser moves: `g_redir_lock` 0→2, `g_ofile_lock` 1→3, …,
`g_udb_lock` 13→15. This is a mechanical, one-commit change and it must be one
commit — a half-shifted table reports inversions that are artefacts of the
shift, and CLAUDE.md already documents what a partial rank table invites.

`rank_stack[8]` must also grow to `[16]`, since a reclaim path can legitimately
hold vfs → cas → vblk → zone.

## 1.2 Scheduler: from round-robin `pick_next` to weighted fair queueing

The current BSP scheduler (`pick_next`, kernel64.c:3331) is a linear scan over
16 slots picking the first `T_RUNNABLE`. Per-CPU runqueues with stealing exist
for *ring-3 kproc dispatch* (`rq_steal` :16500) but the kernel-thread scheduler
underneath is unweighted round-robin with no priority and no accounting-driven
selection. `proc_cpu_live` accounting already exists (v0.96, feeding
`SYS_DESKTOP_INFO`) and is the input CFS needs.

### 1.2.1 Structures

```c
/* metal/kernel/sched.h — NEW; struct pcb gains ->se */

struct sched_entity {
    uint64_t vruntime;        /* ns, weight-normalised — the CFS key           */
    uint64_t exec_start;      /* ktime_get_ns() at last dispatch               */
    uint64_t sum_exec_ns;     /* lifetime; feeds SYS_GETRUSAGE, already wired  */
    uint32_t weight;          /* from nice: prio_to_weight[nice+20]            */
    uint32_t inv_weight;      /* precomputed 2^32/weight; avoids runtime div   */
    /* Red-black tree by vruntime. A skiplist or a leftmost-cached heap would
     * also do; an rbtree is specified because O(log n) worst case matters once
     * a self-hosting `make -j4` puts hundreds of tasks on one queue. */
    struct rb_node run_node;
    int      on_rq;
    struct cfs_rq *cfs_rq;
};

struct cfs_rq {
    struct rb_root  tasks_timeline;
    struct rb_node *rb_leftmost;      /* cached min-vruntime: O(1) pick        */
    uint64_t        min_vruntime;     /* monotonic floor; new tasks start here
                                       * MINUS a wakeup bonus, so a task that
                                       * slept does not have to catch up       */
    uint64_t        load_weight;      /* sum of weights, for slice arithmetic  */
    uint32_t        nr_running;
};

struct rq {                            /* one per CPU; replaces g_rq[] ad-hoc  */
    struct cfs_rq   cfs;
    struct pcb     *curr, *idle;
    uint64_t        clock_ns;
    uint32_t        cpu;
    uint32_t        node;              /* NUMA node from SRAT                  */
    uint64_t        nr_switches, nr_migrations, nr_steals, nr_steal_aborted;
    struct klock    lock;              /* rank as today's rq lock              */
    /* Real-time band, checked BEFORE cfs. Two classes only: SCHED_FIFO and
     * SCHED_RR. The desktop compositor and the audio path are the consumers;
     * both currently rely on being pinned to CPU 0, which is a workaround for
     * not having priority at all. */
    struct pcb     *rt_queue[MAX_RT_PRIO];
    uint32_t        rt_bitmap;         /* ffs() gives highest runnable prio    */
};

/* Weight table: nice 0 == 1024, each nice step ~1.25x, so a nice-0 task gets
 * ~10% more CPU than nice 1. Identical to Linux's for one reason only — the
 * ratios have been tuned against real workloads for two decades and inventing
 * new ones would be inventing numbers. */
extern const uint32_t prio_to_weight[40];

#define SCHED_NORMAL 0
#define SCHED_FIFO   1
#define SCHED_RR     2
#define SCHED_IDLE   3
```

### 1.2.2 The timeslice, and why it must be a deadline

```c
/* Target latency: every runnable task gets the CPU once per this window.
 * Under TCG these numbers are what they claim to be ONLY as deadlines — see
 * CLAUDE.md. sched_slice returns NANOSECONDS and is compared against
 * ktime_get_ns(), never against an iteration count. */
#define SCHED_LATENCY_NS      (24ull * 1000 * 1000)   /* 24 ms                 */
#define SCHED_MIN_GRANULARITY (3ull * 1000 * 1000)    /* 3 ms floor            */

static inline uint64_t sched_slice(struct cfs_rq *q, struct sched_entity *se) {
    uint64_t period = SCHED_LATENCY_NS;
    if (q->nr_running * SCHED_MIN_GRANULARITY > period)
        period = q->nr_running * SCHED_MIN_GRANULARITY;   /* stretch, don't
                                                           * shrink below the
                                                           * floor: thrashing
                                                           * the switch path is
                                                           * worse than latency */
    return (period * se->weight) / (q->cfs.load_weight ? q->cfs.load_weight : 1);
}
```

`ktime_get_ns()` must be added; `ktime_get_us()` already exists (v0.92, backed
by the ACPI PM counter sampled at PIT edges, kernel64.c:482). Under TCG the PM
timer is the only trustworthy source — TSC frequency is not invariant in the
emulator. `SYS_CLOCK_GETTIME` already consumes this path, so the plumbing is
half-built.

### 1.2.3 NUMA

SRAT parsing reuses `acpi_find_table("SRAT")`, which already exists. Per-node
zones, and load balancing that prefers same-node steals:

```c
struct numa_node {
    uint32_t  id;
    uint64_t  cpu_mask;
    struct zone zones[MAX_ZONES_PER_NODE];
    uint8_t   distance[MAX_NUMA_NODES];   /* SLIT-derived; 10 == local         */
};
```

`rq_steal` already implements the correct *structure* (non-blocking trylock,
skip busy victims, respect isolation). The NUMA change is one line in victim
selection: iterate victims in ascending `distance[]` order rather than ascending
CPU index. **Under QEMU/TCG with a single emulated node this is unobservable**,
which is exactly why it must be tested with `-numa node,cpus=0-1 -numa
node,cpus=2-3` and a new gate tier, or it is untested code that reports success.

## 1.3 Async I/O: completion rings

Modelled on `io_uring`. Two shared-memory rings per context, mapped into the
process by an existing mechanism — `SYS_SHM_CREATE`/`SYS_SHM_MAP` (syscalls
74/75) already do refcounted shared mappings and are marked non-COW via
`PTE_SHM`, which is exactly the semantics a submission ring needs.

```c
/* include/outrun_aio.h — NEW, and it must live in include/ beside
 * outrun_abi.h, because CLAUDE.md's rule for SYS_DESKTOP_INFO applies verbatim:
 * the header is the master copy and the kernel's local declaration moves with
 * it, with a size check that REFUSES a mismatch rather than partly filling. */

#define ORING_ABI_VERSION 1

struct oring_sqe {                     /* 64 bytes, cache-line friendly        */
    uint8_t  opcode;
    uint8_t  flags;                    /* ORING_SQE_* below                    */
    uint16_t ioprio;
    int32_t  fd;
    uint64_t off;                      /* file offset, or -1 for current       */
    uint64_t addr;                     /* user buffer, or iovec array          */
    uint32_t len;                      /* bytes, or iovec count                */
    uint32_t rw_flags;
    uint64_t user_data;                /* returned verbatim in the CQE         */
    uint64_t _pad[2];
};
_Static_assert(sizeof(struct oring_sqe) == 64, "SQE must stay 64 bytes");

struct oring_cqe {                     /* 16 bytes                             */
    uint64_t user_data;
    int32_t  res;                      /* bytes transferred, or -errno         */
    uint32_t flags;
};
_Static_assert(sizeof(struct oring_cqe) == 16, "CQE must stay 16 bytes");

/* Ring head/tail live in the shared page. The KERNEL owns sq.head and cq.tail;
 * USERSPACE owns sq.tail and cq.head. Neither side ever writes the other's
 * index, which is what makes the ring lock-free without a shared lock — and it
 * is the invariant to assert, because a kernel that writes sq.tail "to fix up"
 * a malformed submission silently reintroduces the race. */
struct oring_params {
    uint32_t sq_entries, cq_entries;   /* powers of two                        */
    uint32_t flags;                    /* ORING_SETUP_*                        */
    uint32_t sq_off_head, sq_off_tail, sq_off_mask, sq_off_array;
    uint32_t cq_off_head, cq_off_tail, cq_off_mask, cq_off_cqes;
    uint32_t size;                     /* sizeof(struct oring_params)          */
    uint32_t shm_id;                   /* feed to SYS_SHM_MAP                  */
};

/* opcodes */
#define ORING_OP_NOP        0
#define ORING_OP_READV      1
#define ORING_OP_WRITEV     2
#define ORING_OP_FSYNC      3
#define ORING_OP_POLL_ADD   4
#define ORING_OP_TIMEOUT    5
#define ORING_OP_ACCEPT     6
#define ORING_OP_CONNECT    7
#define ORING_OP_SEND       8
#define ORING_OP_RECV       9
#define ORING_OP_CLOSE     10

/* sqe flags */
#define ORING_SQE_FIXED_FILE  (1u << 0)
#define ORING_SQE_IO_LINK     (1u << 1)   /* next SQE waits for this one       */
#define ORING_SQE_IO_DRAIN    (1u << 2)

/* setup flags */
#define ORING_SETUP_SQPOLL    (1u << 0)   /* kernel poller thread; no syscall
                                           * per submission at all             */
#define ORING_SETUP_IOPOLL    (1u << 1)   /* busy-poll NVMe CQ; needs §2.2     */
```

### 1.3.1 Syscall interface

```
119  SYS_ORING_SETUP    (entries, struct oring_params *p, 0)        -> ring_fd
120  SYS_ORING_ENTER    (ring_fd, to_submit, min_complete|flags<<32) -> n_submitted
121  SYS_ORING_REGISTER (ring_fd, opcode, arg)                       -> 0 | -errno
```

Numbers 119–121 are chosen because 118 is the highest currently assigned
(`SYS_DESKTOP_SETTINGS`). **Grep both `#define SYS_` in init.c and the dispatch
switch in kernel64.c before claiming a number is free** — CLAUDE.md documents
the role-number collision that cost v0.81 four boots in eight, and syscall
numbers are the same shared namespace.

### 1.3.2 Execution model

`ORING_OP_*` handlers must **never block in the submitting thread**. The
existing `virtio-blk` path parks the caller (`vblk wait`), which is precisely
the thread-per-request overhead this pillar exists to remove. The design:

- A submission that can complete inline (cached read, `NOP`, ready poll) writes
  its CQE immediately and returns.
- One that cannot is handed to a per-CPU **io worker** kernel thread pool, which
  parks on the device instead. The submitter returns at once.
- `ORING_SETUP_SQPOLL` runs a dedicated kernel thread spinning on `sq.tail` with
  an idle timeout, so a saturated ring costs zero syscalls.

`epoll` (syscalls 76–78) stays; it is not superseded. `ORING_OP_POLL_ADD`
integrates with the same wait-tag mechanism `struct pcb::wait_tag` already uses.

**Test discipline.** A completion ring that is only ever exercised with
`min_complete == to_submit` never leaves the inline path and never tests the
worker pool — the same class of defect as a counter nothing increments. The
suite must include a deliberate deep-queue case (submit 256, complete 1) and
must assert on a **detections** counter (`g_oring_deferred`) being non-zero, not
merely on the failure count being zero.

---

# Pillar 2 — VFS, Storage & Dynamic Drivers

## 2.1 VFS: from a flat CAS directory to an inode/dentry graph

Today's `struct dirent` (kernel64.c:6454) is a 256-byte packed on-disk record
with a flat 64-byte name, 16 direct chunk hashes, one single-indirect and one
double-indirect hash, uid/gid/mode and tick-based timestamps. It is
content-addressed, journalled, and has a hard `_Static_assert` on its size.
**It is a good format and must not be discarded** — five releases of on-disk
compatibility live in it. It becomes the *backing store of one filesystem type*
beneath a generic VFS, not the VFS itself.

```c
/* metal/kernel/vfs.h — NEW */

struct inode {
    uint64_t          ino;
    uint32_t          mode;          /* S_IFREG|S_IFDIR|S_IFCHR|... + rwx      */
    uint32_t          uid, gid;
    uint64_t          size;
    uint64_t          atime, mtime, ctime;   /* NANOSECONDS since epoch — see
                                              * the migration note below       */
    uint32_t          nlink;
    volatile int32_t  refcount;
    struct super_block      *sb;
    const struct inode_ops  *op;
    const struct file_ops   *fop;
    struct address_space    *mapping;   /* page cache for this inode           */
    void             *private;          /* CAS: the dirent index               */
    struct klock      lock;             /* rank: g_vfs_lock's, per-inode       */
};

struct dentry {                        /* name -> inode, and the tree edges    */
    char                name[VFS_NAME_MAX];
    uint32_t            hash;
    struct inode       *inode;         /* NULL == negative dentry (proven absent) */
    struct dentry      *parent;
    struct dentry      *child, *sibling;   /* first-child / next-sibling tree  */
    struct dentry      *hash_next;         /* dcache bucket chain              */
    volatile int32_t    refcount;
    uint32_t            flags;
};

struct super_block {
    uint64_t                 magic;
    uint32_t                 block_size;
    struct dentry           *root;
    const struct super_ops  *op;
    struct block_device     *bdev;     /* NULL for pseudo-filesystems          */
    void                    *fs_private;
    struct klock             lock;
};

struct vfsmount {
    struct dentry     *mountpoint;     /* in the PARENT tree                   */
    struct super_block *sb;
    struct vfsmount   *parent, *next;
    uint32_t           flags;          /* MNT_RDONLY, MNT_NOEXEC, MNT_NOSUID   */
};

struct inode_ops {
    struct dentry *(*lookup)(struct inode *dir, struct dentry *d);
    int  (*create) (struct inode *dir, struct dentry *d, uint32_t mode);
    int  (*mkdir)  (struct inode *dir, struct dentry *d, uint32_t mode);
    int  (*unlink) (struct inode *dir, struct dentry *d);
    int  (*rmdir)  (struct inode *dir, struct dentry *d);
    int  (*rename) (struct inode *od, struct dentry *odn,
                    struct inode *nd, struct dentry *ndn);
    int  (*setattr)(struct inode *i, const struct iattr *a);
    int  (*getattr)(struct inode *i, struct kstat *out);
    int  (*symlink)(struct inode *dir, struct dentry *d, const char *target);
    int  (*readlink)(struct dentry *d, char *buf, uint32_t n);
};

struct file_ops {
    int64_t (*read)  (struct file *f, void *buf, uint64_t n, uint64_t *off);
    int64_t (*write) (struct file *f, const void *buf, uint64_t n, uint64_t *off);
    int64_t (*lseek) (struct file *f, int64_t off, int whence);
    int     (*mmap)  (struct file *f, struct vm_area *vma);
    int     (*fsync) (struct file *f, int datasync);
    int     (*ioctl) (struct file *f, uint32_t cmd, uint64_t arg);
    int     (*poll)  (struct file *f, struct poll_table *pt);
    int     (*readdir)(struct file *f, struct dirent64 *out, uint32_t n);
    int     (*release)(struct file *f);
    /* async: if present, the oring path uses it and never parks a thread.
     * If absent, the oring worker pool falls back to ->read/->write, which is
     * correct but not fast. Both must exist and both must be tested. */
    int     (*submit_async)(struct file *f, struct oring_sqe *s, struct io_ctx *c);
};

struct address_space {                 /* per-inode page cache; replaces the
                                        * global 32-entry g_pcache[]           */
    struct radix_tree  pages;          /* pgidx -> struct page *               */
    struct inode      *host;
    uint64_t           nrpages;
    struct klock       lock;
};
```

### 2.1.1 The per-process file table — the fd-16 fix

```c
/* struct file is the OPEN INSTANCE (offset, flags, mode). struct files_struct
 * is the per-process descriptor array. Today's g_ofiles[16] conflates the two
 * AND is global; both halves of that are wrong, and the global half is what
 * makes `make` impossible. */
struct file {
    struct inode           *inode;
    struct dentry          *dentry;
    struct vfsmount        *mnt;
    uint64_t                pos;
    uint32_t                flags;     /* O_NONBLOCK, O_APPEND, O_CLOEXEC      */
    uint32_t                mode;      /* FMODE_READ | FMODE_WRITE             */
    volatile int32_t        refcount;  /* dup/fork share ONE struct file       */
    const struct file_ops  *op;
    void                   *private;   /* pipe, socket, epoll, eventfd, oring  */
};

#define FD_INLINE 32
struct files_struct {
    volatile int32_t  refcount;        /* CLONE_FILES shares this whole struct */
    uint32_t          max_fds;
    struct file     **fd;              /* -> fd_inline, or a grown array       */
    struct file      *fd_inline[FD_INLINE];
    uint64_t         *close_on_exec;   /* bitmap                               */
    uint64_t         *open_fds;        /* bitmap                               */
    struct klock      lock;
};
```

`struct kproc` gains `struct files_struct *files`. The existing `owner_mask`
field on `struct ofile` — a bitmask of owning processes, which is how the
current code fakes fork inheritance — **disappears**, replaced by
`file->refcount`. That is a semantic upgrade, not a rename: `owner_mask` is a
64-bit mask and `MAX_KPROC` is 64, so the current design cannot survive raising
the process limit at all.

### 2.1.2 procfs and sysfs

Both are `struct super_block`s with no `bdev`, generated on read:

```
/proc/self, /proc/<pid>/{status,cmdline,maps,fd/,stat}
/proc/{meminfo,cpuinfo,uptime,stat,mounts,interrupts}
/sys/class/{block,net,gpu}/, /sys/bus/pci/devices/<bdf>/{vendor,device,class,resource}
```

`/proc/<pid>/maps` and `/sys/bus/pci/devices/` are not conveniences — they are
what a ported `libc`, a `ld.so` debugging path, and a userspace driver framework
read. `/proc/self/maps` in particular is required by musl's `dlopen`.

### 2.1.3 Timestamp migration

CLAUDE.md records the deliberate decision that dirent `mtime`/`atime` are
**boot-relative ticks**, not epoch seconds, because redefining them would
silently reinterpret every timestamp on every existing volume. The inode layer
uses nanoseconds-since-epoch. The bridge is in the CAS filesystem driver's
`->getattr`, and it must be explicit:

- On mount, record `sb->boot_epoch_ns` = the volume's superblock epoch stamp.
- A dirent timestamp of 0 still means UNKNOWN and maps to the mount time.
- A **new superblock field** carries the epoch of the boot that wrote each
  timestamp. Volumes without it (every pre-v2.0 volume) report ticks converted
  against mount time, flagged `ST_TIME_APPROX` in `struct kstat::flags`, so a
  caller can tell a measured time from a reconstructed one.

## 2.2 Block layer + NVMe

There is currently **no block layer**. `virtio-blk` is called directly from the
CAS code. NVMe cannot be added under that; it needs a request abstraction.

```c
/* metal/kernel/blk.h — NEW */

struct bio_vec { struct page *page; uint32_t offset, len; };

struct bio {
    struct block_device *bdev;
    uint64_t   sector;                 /* 512-byte units                       */
    uint32_t   op;                     /* REQ_OP_READ / WRITE / FLUSH / DISCARD */
    uint32_t   flags;                  /* REQ_FUA, REQ_SYNC, REQ_PREFLUSH      */
    uint32_t   vcnt, vidx;
    struct bio_vec *vec;
    void     (*end_io)(struct bio *, int32_t res);
    void      *private;
    volatile int32_t remaining;        /* split-bio join counter               */
    int32_t    result;
};

struct blk_mq_hw_ctx {                 /* ONE PER CPU: this is the whole point */
    uint32_t          cpu;
    uint32_t          queue_depth;
    struct request  **tags;            /* tag -> in-flight request             */
    uint64_t         *tag_bitmap;
    void             *driver_data;     /* NVMe: struct nvme_queue *            */
    struct klock      lock;
};

struct block_device {
    char                 name[16];     /* "vblk0", "nvme0n1"                   */
    uint64_t             nr_sectors;
    uint32_t             logical_block_size, physical_block_size;
    uint32_t             max_segments, max_segment_size;
    uint32_t             nr_hw_queues;
    struct blk_mq_hw_ctx *hctx;        /* nr_hw_queues entries                 */
    const struct blk_ops *op;
    struct address_space  mapping;     /* the buffer cache for raw device I/O  */
    void                 *driver_data;
};

struct blk_ops {
    int  (*queue_rq)(struct blk_mq_hw_ctx *h, struct request *r);  /* non-blocking */
    int  (*poll)    (struct blk_mq_hw_ctx *h, uint32_t budget);    /* IOPOLL      */
    int  (*flush)   (struct block_device *b);
};
```

### 2.2.1 NVMe 1.4 driver

```c
/* metal/kernel/nvme.h — NEW */

/* --- Controller registers (BAR0, MMIO, 64-bit little-endian) --------------- */
#define NVME_REG_CAP      0x00   /* Controller Capabilities (64)               */
#define NVME_REG_VS       0x08   /* Version                                    */
#define NVME_REG_INTMS    0x0C
#define NVME_REG_INTMC    0x10
#define NVME_REG_CC       0x14   /* Controller Configuration                   */
#define NVME_REG_CSTS     0x1C   /* Controller Status                          */
#define NVME_REG_AQA      0x24   /* Admin Queue Attributes                     */
#define NVME_REG_ASQ      0x28   /* Admin SQ base (64)                         */
#define NVME_REG_ACQ      0x30   /* Admin CQ base (64)                         */

#define NVME_CAP_MQES(c)  (((c) & 0xFFFFull) + 1)      /* max queue entries    */
#define NVME_CAP_TO(c)    (((c) >> 24) & 0xFFull)      /* timeout, 500 ms units */
#define NVME_CAP_DSTRD(c) (((c) >> 32) & 0xFull)       /* doorbell stride      */
#define NVME_CAP_CSS(c)   (((c) >> 37) & 0xFFull)
#define NVME_CAP_MPSMIN(c)(((c) >> 48) & 0xFull)

#define NVME_CC_EN        (1u << 0)
#define NVME_CC_CSS_NVM   (0u << 4)
#define NVME_CC_MPS_SHIFT 7
#define NVME_CC_AMS_RR    (0u << 11)
#define NVME_CC_SHN_NORMAL (1u << 14)
#define NVME_CC_IOSQES    (6u << 16)   /* 2^6 = 64-byte SQ entry               */
#define NVME_CC_IOCQES    (4u << 20)   /* 2^4 = 16-byte CQ entry               */

#define NVME_CSTS_RDY     (1u << 0)
#define NVME_CSTS_CFS     (1u << 1)    /* controller fatal status              */
#define NVME_CSTS_SHST(s) (((s) >> 2) & 3u)

/* Doorbell for queue qid: BAR0 + 0x1000 + ((2*qid + is_cq) << (2 + DSTRD)).
 * The DSTRD shift is the classic mistake — hardcoding stride 0 works on QEMU
 * and corrupts a real controller's doorbell region. Compute it from CAP. */
#define NVME_DB(bar, qid, iscq, dstrd) \
    ((volatile uint32_t *)((uint8_t *)(bar) + 0x1000 + \
     ((((uint32_t)(qid) << 1) | (iscq)) << (2 + (dstrd)))))

struct nvme_cmd {                      /* 64 bytes, submission queue entry     */
    uint8_t  opcode, flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t _rsv2;
    uint64_t mptr;
    uint64_t prp1, prp2;               /* PRP list; SGL not used in v2.0       */
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};
_Static_assert(sizeof(struct nvme_cmd) == 64, "NVMe SQE is 64 bytes");

struct nvme_cqe {                      /* 16 bytes                             */
    uint32_t result;
    uint32_t _rsv;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;                   /* bit 0 = PHASE TAG                    */
};
_Static_assert(sizeof(struct nvme_cqe) == 16, "NVMe CQE is 16 bytes");

struct nvme_queue {
    struct nvme_cmd  *sq;              /* DMA-coherent, physically contiguous  */
    struct nvme_cqe  *cq;
    uint64_t          sq_dma, cq_dma;  /* IOMMU IOVAs — see the note below     */
    volatile uint32_t *sq_db, *cq_db;
    uint16_t          qid, depth;
    uint16_t          sq_tail, cq_head;
    uint8_t           cq_phase;        /* flips every wrap; the completion test */
    uint8_t           polled;
    struct klock      lock;            /* rank: below g_dev_lock               */
    struct request  **tags;
};

struct nvme_ctrl {
    void             *bar0;            /* from SYS_MAP_PCI_BAR-equivalent      */
    uint64_t          cap;
    uint32_t          dstrd, page_size, max_transfer;
    uint16_t          bdf;
    struct nvme_queue admin;
    struct nvme_queue *io;             /* one per CPU                          */
    uint32_t          nr_io_queues;
    uint32_t          nsid_count;
    struct iommu_domain *dom;          /* the driver's own IOMMU domain        */
};

/* Admin opcodes used */
#define NVME_ADMIN_DELETE_SQ  0x00
#define NVME_ADMIN_CREATE_SQ  0x01
#define NVME_ADMIN_DELETE_CQ  0x04
#define NVME_ADMIN_CREATE_CQ  0x05
#define NVME_ADMIN_IDENTIFY   0x06
#define NVME_ADMIN_SET_FEAT   0x09
#define NVME_ADMIN_GET_FEAT   0x0A
/* NVM opcodes used */
#define NVME_CMD_FLUSH        0x00
#define NVME_CMD_WRITE        0x01
#define NVME_CMD_READ         0x02
```

**Initialisation sequence** (each step has a failure mode that must be reported,
not retried blindly):

1. Enumerate PCI class `0x010802`. The tree's PCI config-space reader (v0.95,
   `SYS_PCI_CFG_READ`) already provides this.
2. Map BAR0 as **uncached** (`PTE_PCD`). `vm_clone_user` already refuses to
   clone `PTE_PCD` pages, so the mapping is correctly fork-safe for free.
3. `CC.EN = 0`; poll `CSTS.RDY == 0` with a deadline of `CAP.TO * 500 ms`.
   **A deadline, in `ktime_get_us()`, never a spin count** — CLAUDE.md's rule,
   and NVMe's own timeout is already specified in time units.
4. Allocate admin SQ (64 B × depth) and CQ (16 B × depth), physically
   contiguous, page-aligned, **zeroed**. `alloc_pages(order, GFP_DMA32|GFP_ZERO)`.
5. Program `AQA`, `ASQ`, `ACQ`. Set `CC` = `IOSQES|IOCQES|MPS|CSS_NVM|EN`.
   Poll `CSTS.RDY == 1`, same deadline. If `CSTS.CFS` sets, the controller is
   fatally broken — report and disable the device; do not reset in a loop.
6. `IDENTIFY` controller (CNS=1) then namespace (CNS=0) → capacity, LBA format,
   `MDTS` → `max_transfer`.
7. `SET_FEATURES(NUM_QUEUES)` requesting `ncpu` pairs. **The controller may grant
   fewer**; the granted count is in the completion `result` and must be honoured.
   A driver that allocates `ncpu` queues after being granted 2 writes doorbells
   for queues that do not exist.
8. `CREATE_CQ` before `CREATE_SQ` for each pair — the SQ command names its CQ,
   so the reverse order fails with an invalid-queue-identifier status.
9. MSI-X vector per queue, routed through the existing IOMMU interrupt-remapping
   path (`intremap=on` is already a gate tier).

**Completion detection is the phase tag, not the doorbell.** A CQE is valid when
`(cqe.status & 1) != q->cq_phase`. The phase flips on each wrap. Polling a head
index instead is the standard NVMe driver bug and produces a driver that works
until the first wrap.

**IOMMU interaction.** The queue DMA addresses are **IOVAs**, not physical
addresses, whenever the controller sits behind a translating domain. The tree
already has domain management from v0.95; the NVMe driver must allocate its own
domain and map its rings, and every frame so mapped must be `PG_PINNED` (§1.1.2)
before reclaim exists, or a swapped queue ring becomes a device writing into
reclaimed memory.

### 2.2.2 Test discipline for NVMe

`make qemu-nvme` already exists and boots through OVMF with **no kernel driver**
— firmware reads the image, the kernel then cannot touch the device. A passing
`qemu-nvme` boot today is therefore **not evidence of anything about NVMe**, and
the first thing this work must do is add an assertion that would fail on the
current tree. Per CLAUDE.md's "a test that cannot fail has not passed": build
with the driver's `CREATE_SQ` step removed and confirm the suite goes red.

## 2.3 Kernel Driver Framework — loadable modules

```c
/* metal/kernel/module.h — NEW */

struct kernel_symbol { const char *name; uint64_t addr; };

/* Exported by macro into a dedicated section the linker script gathers.
 * linker.ld gains __ksymtab_start/__ksymtab_end. */
#define EXPORT_SYMBOL(sym) \
    static const struct kernel_symbol __ksym_##sym \
    __attribute__((used, section("__ksymtab"), aligned(16))) = \
    { #sym, (uint64_t)&sym }

struct module {
    char      name[64];
    uint64_t  core_base;              /* vmalloc'd, W^X-split                  */
    uint64_t  text_base, text_size;   /* R+X                                   */
    uint64_t  rodata_base, rodata_size; /* R+NX                                */
    uint64_t  data_base, data_size;   /* RW+NX                                 */
    int     (*init)(void);
    void    (*exit)(void);
    volatile int32_t refcount;        /* nonzero blocks unload                 */
    struct module *next;
    uint64_t  build_id;               /* must match the kernel's — see below   */
    uint32_t  flags;
};

int module_load(const void *elf, uint64_t size, const char *args);
int module_unload(const char *name);
```

`.ko` files are **ET_REL** (relocatable objects), not ET_DYN. Relocation types
required: `R_X86_64_64`, `R_X86_64_PC32`, `R_X86_64_32S`, `R_X86_64_PLT32`.
`PC32`/`PLT32` are ±2 GiB displacements, which forces module text to be
allocated **within 2 GiB of the kernel image** — a general `vmalloc` region at
an arbitrary high address will produce relocation overflows that only appear
once the allocator happens to hand out a far address. Reserve a dedicated
`MODULE_VADDR` window adjacent to the kernel.

**Version safety.** A module built against a different kernel that loads anyway
corrupts silently. Each module carries a `build_id` derived from a hash of every
exported symbol's name and the sizes of the ABI structs it can see; a mismatch
is a **refusal**, matching how `SYS_DESKTOP_SETTINGS` refuses a disagreeing size
rather than partly filling a buffer.

**Resource arbitration** reuses `g_dev_lock` (rank 11 today, 13 after the §1.1.3
shift) and the v0.95 claim/release device registry unchanged — a module claiming
a PCI device goes through exactly the path a ring-3 userspace driver does.

```
122  SYS_MODULE_LOAD    (const void *image, uint64_t size, const char *args) -> 0|-errno
123  SYS_MODULE_UNLOAD  (const char *name, 0, 0)                             -> 0|-errno
124  SYS_MODULE_LIST    (struct module_info *out, uint32_t n, 0)             -> count
```
Requires a new `PCAP_MODULE` capability bit (bit 14 — bits 0–13 are taken).

---

# Pillar 3 — Platform Management & Display

## 3.1 ACPI: from table discovery to AML

Current state: `acpi_find_table()` walks RSDP → RSDT/XSDT and matches
signatures, used to locate DMAR and the PM timer. There is no AML interpreter,
so no `_PTS`, `_S3`, `_PSS`, `_CST`, `_TMP`, or GPE handling exists.

**Recommendation: port ACPICA rather than write an AML interpreter.** AML is a
bytecode with a full object model, dynamic namespace, mutexes and region
handlers; a from-scratch interpreter is a multi-year project that firmware will
find new ways to break. ACPICA is BSD/GPL dual-licensed and its OS layer is a
well-defined ~40-function shim.

```c
/* metal/kernel/acpi_osl.c — the ACPICA OS services layer, ~1200 lines */
ACPI_STATUS AcpiOsInitialize(void);
void       *AcpiOsAllocate(ACPI_SIZE);          /* -> kmalloc                   */
void       *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS, ACPI_SIZE);  /* -> physmap   */
ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *);  /* -> struct klock, new rank    */
ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 irq, ACPI_OSD_HANDLER, void *);
ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *, UINT32 reg, UINT64 *, UINT32);
UINT64      AcpiOsGetTimer(void);               /* 100 ns units -> ktime_get_ns */
ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE, ACPI_OSD_EXEC_CALLBACK, void *);
```

Most of these map onto machinery that already exists: PCI config reads (v0.95),
klock, `ktime_get_us`, the kernel thread pool. `AcpiOsExecute` needs a
**deferred work queue**, which the oring worker pool from §1.3 already provides.

### 3.1.1 Power management

```c
struct cpufreq_policy {
    uint32_t cpu;
    uint32_t cur_khz, min_khz, max_khz;
    uint32_t nr_pstates;
    struct { uint32_t khz, mw, latency_us, ctrl, status; } pstate[16]; /* _PSS  */
    const struct cpufreq_driver *drv;   /* acpi-cpufreq (MSR) or pstate         */
    uint32_t governor;                  /* PERFORMANCE|POWERSAVE|ONDEMAND       */
};

struct cpuidle_state {                  /* from _CST                            */
    uint32_t type;                      /* C1/C2/C3                             */
    uint32_t exit_latency_us, target_residency_us;
    uint32_t addr;                      /* IO port, or MWAIT hint               */
    uint32_t entry_method;              /* HLT | IO | FFH(MWAIT)                */
};

struct thermal_zone {
    char     name[16];
    int32_t  temp_mdeg;                 /* _TMP, millidegrees C                 */
    int32_t  trip_passive, trip_hot, trip_critical;   /* _PSV/_HOT/_CRT         */
    uint32_t polling_ms;
};
```

**A hard warning specific to this tree.** Under TCG, C-states and P-states are
either unimplemented or lies, and `HLT` in a TCG guest does not do what it does
on metal. Every timing budget in this tree is TCG-calibrated (CLAUDE.md §"TCG is
a policy"). Introducing idle states that change how long a core spends not
executing **will** perturb those budgets in the same way enabling KVM would.
Therefore: C/P-state support ships **disabled by default**, enabled by a boot
parameter (`cpuidle=on`), and the gate continues to run with it off until a
deliberate re-baseline is performed and recorded — one commit, with re-measured
numbers, exactly as CLAUDE.md prescribes for the KVM switch.

S3/S4 need `_PTS`/`_GTS`, a device suspend/resume callback chain on every driver,
and a wake vector in the FACS. **Scope them out of v2.0**; S5 (poweroff via
`PM1a_CNT` `SLP_TYP|SLP_EN`) and the power button GPE are cheap and genuinely
useful, and should land first.

## 3.2 DRM/KMS

```c
/* metal/kernel/drm.h — NEW */

struct drm_gem_object {                /* a buffer, ref-counted, page-backed   */
    uint32_t          handle;
    uint64_t          size;
    struct page     **pages;           /* size/4096 entries                    */
    uint64_t          iova;            /* IOMMU address if device-visible      */
    volatile int32_t  refcount;
    uint32_t          flags;           /* GEM_SCANOUT | GEM_CACHED             */
    void             *driver_private;  /* virtio-gpu: resource id              */
};

struct drm_framebuffer {
    uint32_t id, width, height, format;   /* DRM_FORMAT_XRGB8888 etc.          */
    uint32_t pitch[4], offset[4];
    struct drm_gem_object *bo[4];
};

struct drm_plane {
    uint32_t id, type;                 /* PRIMARY | CURSOR | OVERLAY           */
    uint32_t possible_crtcs;
    struct drm_plane_state { struct drm_framebuffer *fb;
                             int32_t crtc_x, crtc_y;
                             uint32_t crtc_w, crtc_h;
                             uint32_t src_x, src_y, src_w, src_h;
                             uint32_t alpha, zpos; } state;
};

struct drm_crtc {
    uint32_t id;
    struct drm_display_mode { uint32_t clock_khz, hdisplay, hsync_start,
                                       hsync_end, htotal, vdisplay, vsync_start,
                                       vsync_end, vtotal, flags; } mode;
    struct drm_plane *primary, *cursor;
    int      enabled;
    uint64_t vblank_count;
};

struct drm_connector {
    uint32_t id, type;                 /* VIRTUAL | HDMI | DP | eDP            */
    uint32_t status;                   /* CONNECTED | DISCONNECTED | UNKNOWN   */
    uint8_t  edid[256];
    uint32_t nr_modes;
    struct drm_display_mode modes[16];
    struct drm_crtc *crtc;
};

/* ATOMIC COMMIT: the entire display state changes at once, or not at all.
 * The check phase must be side-effect free — that is the property that lets a
 * client TEST_ONLY a configuration, and it is the property most easily lost by
 * a driver that "helpfully" allocates during check. */
struct drm_atomic_state {
    struct drm_crtc_state      *crtcs;   uint32_t nr_crtcs;
    struct drm_plane_state     *planes;  uint32_t nr_planes;
    struct drm_connector_state *conns;   uint32_t nr_conns;
    uint32_t flags;                      /* ATOMIC_TEST_ONLY | ATOMIC_NONBLOCK |
                                          * ATOMIC_ALLOW_MODESET               */
    uint64_t user_data;                  /* echoed in the completion event     */
};

struct drm_driver {
    int (*atomic_check) (struct drm_device *, struct drm_atomic_state *);
    int (*atomic_commit)(struct drm_device *, struct drm_atomic_state *);
    int (*gem_create)   (struct drm_device *, uint64_t size, uint32_t flags,
                         struct drm_gem_object **out);
    int (*gem_mmap)     (struct drm_gem_object *, struct vm_area *);
    int (*enable_vblank)(struct drm_crtc *);
};
```

### 3.2.1 Syscall / ioctl interface

DRM in Linux is an ioctl surface on `/dev/dri/card0`. That requires a device
node, which requires §2.1's VFS. Interface:

```
DRM_IOCTL_MODE_GETRESOURCES    -> crtc/connector/encoder/plane id lists
DRM_IOCTL_MODE_GETCONNECTOR    -> modes + EDID
DRM_IOCTL_MODE_CREATE_DUMB     -> a GEM handle for a linear scanout buffer
DRM_IOCTL_MODE_MAP_DUMB        -> an mmap offset (goes through file_ops->mmap)
DRM_IOCTL_MODE_ADDFB2          -> framebuffer id from GEM handles
DRM_IOCTL_MODE_ATOMIC          -> check + commit
DRM_IOCTL_GEM_CLOSE            -> drop handle
```

### 3.2.2 Coexistence with the existing desktop — non-negotiable

CLAUDE.md is explicit that the regression ISO and the desktop ISO are separated
because a desktop loop underneath the suites changes what the suites measure,
and that `make gate` boots the regression ISO. DRM/KMS must preserve that:

- The existing `SYS_WIN_*` / `SYS_SURFACE_*` / `SYS_GPU_*` ABI **does not
  change**. The v0.96 paired-buffer protocol, `WIN_BACK_V(id)`, `desk_chip_slot`
  hit-testing and the CPU-0 pinning that makes the compositor's lock-free
  surface read sound all stay exactly as they are.
- `desktop_run()` is re-expressed as a DRM client that owns one CRTC. Its
  `fb_flip` scale magnification moves into the primary plane's src/crtc
  rectangle, where it belongs — the window manager keeps working in logical
  coordinates, as CLAUDE.md requires.
- **One hit-test table, one geometry function.** Adding a DRM plane path that
  computes window rectangles independently of `desk_chip_slot` is the launcher
  version of the duplicated-hit-box trap already documented. The compositor and
  the DRM plane state must derive from the same function.
- `tools/desktop-ui-test.py` must keep passing unchanged, and it must keep
  sending **`rel`** mouse events — the machine has PS/2 and no tablet.

virtio-gpu 3D (VIRGL) requires a Mesa port and a userspace GL driver; it is
**out of scope for v2.0** and should be named as out of scope rather than left
implied.

---

# Pillar 4 — POSIX Userland & Self-Hosting

## 4.1 libc: musl, not newlib

Recommendation: **musl**. newlib's `_impure_ptr`/reent model is a poor fit for a
kernel that already has real POSIX threads, and musl's syscall surface is
smaller, its threading is native, and it is designed for static linking — which
is what this kernel can already do today.

The port is a `arch/x86_64` + `src/internal/syscall_arch.h` mapping plus a
`configure` target. The work is not writing musl; it is **making the syscall
surface musl expects actually exist**.

### 4.1.1 Gap analysis against musl's minimum

| musl requirement | Tree today | Action |
|---|---|---|
| `openat`/`fstatat`/`unlinkat` (`*at` family) | `SYS_OPEN` only, flat names | Add `*at` variants once §2.1 lands |
| `SYS_readv`/`writev` | absent | Add; oring `struct bio_vec` shares the iovec shape |
| `SYS_ioctl` | absent | Required for termios and DRM |
| `SYS_getdents64` | `SYS_READDIR`, non-standard layout | Add a POSIX `struct dirent64` variant |
| `SYS_set_thread_area` / `arch_prctl(ARCH_SET_FS)` | absent | **Blocking**: musl's TLS is `%fs`-relative. Without it nothing links. |
| `SYS_rt_sigprocmask`, `sigaltstack` | `SYS_SIGPROCMASK` yes, `sigaltstack` no | Add |
| `SYS_clone` with `CLONE_*` flags | `SYS_THREAD_CREATE`, fixed semantics | Add a flags-driven `clone` |
| `SYS_futex` (FUTEX_WAIT_BITSET, requeue) | `FUTEX_WAIT`/`WAKE` only | Extend |
| `SYS_mmap` MAP_FIXED / MAP_NORESERVE | partial | Extend |
| `SYS_uname`, `SYS_getcwd`, `SYS_chdir` | absent | Add (needs §2.1's tree) |
| `SYS_pipe2`, `SYS_dup`, `SYS_dup2/3` | `SYS_PIPE`, `SYS_SETREDIR` | Add real `dup` on `struct file` |
| `SYS_wait4` with `rusage` | `SYS_WAITPID` + `SYS_GETRUSAGE` separately | Merge |
| `brk` beyond 4 MiB | `HEAP_MAX_BYTES` = 4 MiB | **Blocking for GCC**: raise, and back with §1.1 |

`arch_prctl(ARCH_SET_FS)` deserves emphasis. This kernel uses `%gs` for
`cpu_local` with a compile-time contract (`-mstack-protector-guard-offset`,
`_Static_assert` at kernel64.c:346). `%fs` is free for userspace, but
`swapgs`/`wrmsr` discipline at the ring transition must be audited: writing
`FS_BASE` from ring 3 via `arch_prctl` and restoring it correctly on every
context switch is a place where a missed save is a cross-process TLS leak, which
would look like memory corruption rather than like a scheduling bug.

## 4.2 Dynamic linker

`elf_load` currently **refuses** `PT_INTERP` and `PT_DYNAMIC` with named reasons
(kernel64.c:28607). That refusal is correct and should be *narrowed*, not
deleted: the kernel should learn to load `PT_INTERP` and hand off, while still
refusing a `PT_DYNAMIC` main object it cannot service.

### 4.2.1 Kernel side

```c
/* elf_load gains, in this order:
 *  1. PT_INTERP  -> read the interpreter path, load THAT ELF at a randomised
 *                   base (see KASLR/ASLR, §5.2), and enter at ITS e_entry.
 *  2. PT_LOAD    -> unchanged, including the existing W^X refusal.
 *  3. PT_GNU_RELRO -> record; the linker mprotects it read-only after reloc.
 *  4. PT_TLS     -> record for the initial TLS image.
 *  5. AUXV       -> the process-start block gains AT_PHDR/AT_PHENT/AT_PHNUM/
 *                   AT_BASE/AT_ENTRY/AT_PAGESZ/AT_RANDOM/AT_SECURE.
 *
 * The existing uargs_build() (kernel64.c:23302) already writes argc/argv/envp
 * into the top 1 KiB of the ring-3 stack. AUXV goes after envp's NULL, in the
 * same block. That block is currently 1 KiB (USTK_TOP region) and 16 auxv
 * entries at 16 bytes each is 256 bytes -- it fits, but only just, and the
 * bound must be asserted rather than assumed. */

struct auxv_ent { uint64_t a_type, a_val; };
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_SECURE 23
#define AT_RANDOM 25       /* 16 bytes of entropy; musl's stack guard reads it */
```

### 4.2.2 Userspace `ld.so`

A static PIE that self-relocates before it can call anything.

```c
struct link_map {
    uint64_t     l_addr;             /* load bias                             */
    const char  *l_name;
    Elf64_Dyn   *l_ld;
    struct link_map *next, *prev;
    /* parsed from PT_DYNAMIC */
    const char  *strtab;
    Elf64_Sym   *symtab;
    uint32_t    *gnu_hash;           /* prefer DT_GNU_HASH; fall back to
                                      * DT_HASH, and handle a library that has
                                      * only one -- assuming GNU_HASH is
                                      * present is a real-world crash          */
    Elf64_Rela  *rela;  uint64_t rela_count;
    Elf64_Rela  *jmprel; uint64_t jmprel_count;
    uint64_t    *got_plt;
    uint64_t     init, fini;
    uint64_t     tls_modid, tls_offset, tls_size, tls_align;
};
```

Relocation types, in the order they must be handled:

| Type | Value | Computation | Notes |
|---|---|---|---|
| `R_X86_64_RELATIVE` | 8 | `*where = base + addend` | The bulk of a PIE's relocations. Handled **first, in `_dl_start`, before any global is read** — the linker cannot call a function through its own GOT until this is done. |
| `R_X86_64_GLOB_DAT` | 6 | `*where = sym_addr` | GOT entries for data and eagerly-bound functions. |
| `R_X86_64_JUMP_SLOT` | 7 | `*where = sym_addr` | PLT. Bound lazily via `_dl_runtime_resolve`, or eagerly under `LD_BIND_NOW`/RELRO. |
| `R_X86_64_64` | 1 | `*where = sym_addr + addend` | |
| `R_X86_64_COPY` | 5 | `memcpy(where, sym_addr, size)` | Only for non-PIE executables importing library data. |
| `R_X86_64_TPOFF64` | 18 | `*where = tls_offset + addend` | Initial-exec TLS. |
| `R_X86_64_DTPMOD64` | 16 | `*where = l_tls_modid` | Global-dynamic TLS. |
| `R_X86_64_DTPOFF64` | 17 | `*where = sym_offset_in_module` | |
| `R_X86_64_IRELATIVE` | 37 | `*where = ((fn)(base+addend))()` | ifuncs; musl's `memcpy` uses them. Must run **last**, after everything else resolves. |

Lazy binding needs a PLT0 trampoline that pushes `link_map` and the relocation
index and jumps to `_dl_runtime_resolve`, which must preserve **all** argument
registers *and* the vector registers — a resolver that clobbers `%xmm0` breaks
the first varargs call through the PLT and does so intermittently.

## 4.3 Self-hosting milestones

Ordered by dependency; each is a hard gate on the next.

| # | Milestone | Requires | Proves |
|---|---|---|---|
| S1 | Static `musl` hello-world runs | `*at`, `writev`, `arch_prctl` | syscall ABI is real |
| S2 | Static `busybox ash` runs interactively | S1 + job control, termios, `ioctl`, per-process fds | the shell is not special-cased |
| S3 | `ld.so` self-relocates and runs a dynamic hello-world | AUXV, `PT_INTERP`, `RELATIVE`+`GLOB_DAT`+`JUMP_SLOT` | dynamic linking works |
| S4 | `dlopen`/`dlsym` on a `.so` | `/proc/self/maps`, `DT_NEEDED` walk, TLS relocs | the loader is complete |
| S5 | `make` (GNU make 4.x) builds a 3-file project | `fork`+`execve` at depth, pipes, `wait4`, ≥64 fds | process management scales |
| S6 | `tcc` compiles and links hello-world **in-guest** | S5 + `>16 MiB` heap + real `mmap` | a compiler runs |
| S7 | `binutils` `as` + `ld` run in-guest | S6 + large-file I/O + `mmap` of >64 MiB | a toolchain runs |
| S8 | `gcc -O0` compiles one C file in-guest | S7 + **≥2 GiB RAM** + swap + `MMAP_MAX_BYTES` raised | self-hosting is reachable |
| S9 | The kernel compiles itself | S8 + `make -j`, ≥8 GiB, hierarchical FS, hours of uptime | **self-hosting** |

**S6 (tcc) is the honest v2.0 target. S8/S9 are v2.1+.** `cc1` for a
modern GCC needs ~1 GiB of RSS for a single large translation unit — and
`kernel64.c` is 2 MB of source in one file, which is a considerably harder case
than a typical compile. Claiming S9 in a 4-phase roadmap would be planning a
result rather than a path.

`MAX_KPROC 64` and `MAX_THREADS 16` both fall over at S5: `make -j4` running
`cc1`/`as`/`ld` pipelines exceeds 16 kernel threads immediately. Both must
become dynamically allocated, which is itself gated on §1.1's allocator.

---

# Pillar 5 — Security & Hardware Hardening

## 5.1 Access control

The credential model (real/effective/saved uid+gid) and the 14-bit `PCAP_*`
capability mask are **already strong** — stronger than the brief assumes. What
is missing is per-syscall filtering and per-file ACL granularity.

```c
/* metal/kernel/seccomp.h — NEW. A cBPF-style filter, deliberately NOT full
 * eBPF: a verifier for a Turing-incomplete cBPF program is a few hundred lines
 * and is auditable; an eBPF verifier is where the CVEs live. */

struct sock_filter { uint16_t code; uint8_t jt, jf; uint32_t k; };

struct seccomp_data {                  /* what the filter sees                 */
    int32_t  nr;                       /* syscall number                       */
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t args[6];
};

struct seccomp_filter {
    volatile int32_t      refcount;
    uint32_t              len;
    struct seccomp_filter *prev;       /* filters STACK; all must agree, and
                                        * the MOST RESTRICTIVE answer wins     */
    struct sock_filter    insn[];
};

#define SECCOMP_RET_KILL_PROCESS 0x80000000u
#define SECCOMP_RET_KILL_THREAD  0x00000000u
#define SECCOMP_RET_TRAP         0x00030000u
#define SECCOMP_RET_ERRNO        0x00050000u
#define SECCOMP_RET_ALLOW        0x7fff0000u

/* 125  SYS_SECCOMP (op, flags, const void *arg) -> 0 | -errno
 * Installing a filter requires either CAP-equivalent privilege OR the
 * no_new_privs bit, and no_new_privs is IRREVERSIBLE. Without that pairing an
 * unprivileged process can filter a setuid binary's syscalls and change what
 * privileged code does -- the classic seccomp escalation. */
```

`no_new_privs` must be added to `struct kproc` and must be honoured by
`SYS_EXECVE`'s setuid handling — the existing `suid`/`sgid` logic in
`elf_load`/exec is where it plugs in.

## 5.2 Hardware hardening

### 5.2.1 SMEP / SMAP / UMIP

None are currently enabled. Enabling SMEP alone is one `CR4` bit and is nearly
free. **SMAP is not free** and is the item that will break things:

```c
#define CR4_UMIP  (1u << 11)
#define CR4_SMEP  (1u << 20)
#define CR4_SMAP  (1u << 21)

/* SMAP makes EVERY kernel access to a user address fault, unless bracketed.
 * That is the point, and it is also why it cannot be flipped on in one commit:
 * this kernel touches user memory in dozens of places (uargs_build, the surface
 * blit path, vfs read/write copy loops, the WIMP damage path, every syscall
 * that dereferences a pointer argument). Each one needs a bracket, and one
 * missed site is a #PF in ring 0 with SMAP as the only clue. */
static inline void user_access_begin(void) { __asm__ volatile("stac" ::: "cc"); }
static inline void user_access_end(void)   { __asm__ volatile("clac" ::: "cc"); }

/* The disciplined form: a checked copy that is the ONLY way kernel code
 * touches user memory. Introduce these FIRST, convert every site to them, and
 * only then set CR4.SMAP -- so the bracket lives in two functions, not fifty. */
int64_t copy_from_user(void *dst, const void *usrc, uint64_t n);
int64_t copy_to_user(void *udst, const void *src, uint64_t n);
int64_t strncpy_from_user(char *dst, const char *usrc, uint64_t n);
```

`copy_from_user`/`copy_to_user` must validate direction and range
(`access_ok`), and the validation must be **against `USER_VMIN`/`USER_VMAX` and
the process's own mappings**, not merely "is it non-NULL". This is the audit
focus this project already applies; SMAP turns a missed check from a silent
cross-ring read into a loud fault, which is an improvement either way.

### 5.2.2 KASLR

Requires the kernel to be position-independent or relocatable. `linker.ld`
currently fixes the load address. Steps:

1. Build with `-fPIE`/`-mcmodel=kernel` and emit `.rela.dyn`.
2. A boot-time self-relocator applies `R_X86_64_RELATIVE` against a random
   slide, drawn from `RDRAND` with `RDTSC` mixing as fallback.
3. Slide granularity 2 MiB (the boot page-table granularity), range 1 GiB →
   **512 possible bases**. That is weak; widening it requires the physmap of
   §1.1 first. Say so in the release notes rather than claiming "KASLR" flat.

**Userspace ASLR is cheaper and higher value in the short term**: randomise the
mmap base, the `ld.so` load base, the heap start and the stack top, all of which
are currently fixed constants (`USTK_V`, `HEAP_USER_V`, `MMAP_USER_V`,
`THR_USER_V`). Note that `va_is_forkable()` (kernel64.c:23340) and
`WIN_BACK_V(id)` both encode fixed layout assumptions and must be re-expressed
against per-process bases, not compile-time constants — this is the trap
CLAUDE.md already flags about `WIN_BACK_V` being one stride from another
window's front buffer.

### 5.2.3 CFI

Forward-edge: `-fsanitize=cfi-icall` with LTO, or hardware `CET-IBT`
(`ENDBR64` + `CR4.CET`). Backward-edge: CET Shadow Stack.
**QEMU/TCG does not emulate CET.** Building with CET produces `ENDBR64`
instructions that are correct but never enforced, so a CET build that passes
under TCG has proven nothing about CET. This is precisely "a counter nothing
increments". Recommendation: build with `-fcf-protection=full` for the
instruction encoding, and **explicitly record CET as UNVERIFIED** until a KVM or
bare-metal baseline exists. Compiler-based `-fsanitize=cfi-icall` *is* testable
under TCG and should be preferred for the v2.0 claim.

### 5.2.4 TPM 2.0

```c
struct tpm_chip {
    void     *base;                    /* TIS at 0xFED40000, or CRB            */
    uint32_t  iface;                   /* TPM_IFACE_TIS | TPM_IFACE_CRB        */
    uint32_t  vendor, did;
    uint8_t   locality;
};

/* PCR allocation. Following the TCG PC Client spec rather than inventing one,
 * because a measurement whose PCR index means something different from every
 * other system cannot be attested against any existing policy. */
#define PCR_FIRMWARE       0
#define PCR_BOOTLOADER     4     /* GRUB stage + config                        */
#define PCR_KERNEL         8     /* the kernel image itself                    */
#define PCR_INITRD_MODULES 9     /* boot modules: apps/, the user ELF          */
#define PCR_KDF_POLICY    11     /* OutRun-specific: the sealed-boot policy    */

int tpm2_pcr_extend(struct tpm_chip *, uint32_t pcr, const uint8_t sha256[32]);
int tpm2_pcr_read  (struct tpm_chip *, uint32_t pcr, uint8_t out[32]);
int tpm2_get_random(struct tpm_chip *, void *out, uint32_t n);
int tpm2_seal      (struct tpm_chip *, const void *in, uint32_t n,
                    uint32_t pcr_mask, void *blob, uint32_t *blob_n);
int tpm2_unseal    (struct tpm_chip *, const void *blob, uint32_t n,
                    void *out, uint32_t *out_n);
```

This lands on existing ground: the tree already has `crypto/scrypt.c`, an HMAC
implementation the KDF work verified against vectors, and a
`OUTRUN-0.87-sealed-boot.log` in docs — there is prior art for a sealed-boot
notion here that should be read before designing this, not after.

**Measure before use, always.** A module loader (§2.3) that extends a PCR
*after* mapping the module measures a decision already taken. `module_load`
extends `PCR_INITRD_MODULES` with the hash of the image **before** relocation
processing begins.

QEMU supports `swtpm` via `-tpmdev emulator`, so this is testable in the
existing harness — a new gate tier, not a bare-metal-only claim.

---

# 6. Complete syscall additions

New numbers, contiguous from 119. **Verify each against both `#define SYS_` in
`metal/user/init.c` and the dispatch switch in `kernel64.c` before use.**

| # | Name | Signature | Pillar |
|---|---|---|---|
| 119 | `SYS_ORING_SETUP` | `(entries, struct oring_params*, 0) -> fd` | 1 |
| 120 | `SYS_ORING_ENTER` | `(fd, to_submit, min_complete\|flags<<32) -> n` | 1 |
| 121 | `SYS_ORING_REGISTER` | `(fd, op, arg) -> 0` | 1 |
| 122 | `SYS_MODULE_LOAD` | `(img, size, args) -> 0` | 2 |
| 123 | `SYS_MODULE_UNLOAD` | `(name, 0, 0) -> 0` | 2 |
| 124 | `SYS_MODULE_LIST` | `(out, n, 0) -> count` | 2 |
| 125 | `SYS_SECCOMP` | `(op, flags, arg) -> 0` | 5 |
| 126 | `SYS_PRCTL` | `(op, a, b) -> 0` — carries `NO_NEW_PRIVS` | 5 |
| 127 | `SYS_ARCH_PRCTL` | `(code, addr, 0) -> 0` — `ARCH_SET_FS` | 4 |
| 128 | `SYS_OPENAT` | `(dirfd, path, flags\|mode<<32) -> fd` | 2/4 |
| 129 | `SYS_MKDIRAT` | `(dirfd, path, mode) -> 0` | 2 |
| 130 | `SYS_UNLINKAT` | `(dirfd, path, flags) -> 0` | 2 |
| 131 | `SYS_FSTATAT` | `(dirfd, path, struct kstat*) -> 0` | 2 |
| 132 | `SYS_GETDENTS64` | `(fd, buf, n) -> bytes` | 2/4 |
| 133 | `SYS_CHDIR` | `(path, 0, 0) -> 0` | 2 |
| 134 | `SYS_GETCWD` | `(buf, n, 0) -> len` | 2 |
| 135 | `SYS_MOUNT` | `(src, tgt, fstype\|flags<<32) -> 0` | 2 |
| 136 | `SYS_UMOUNT` | `(tgt, flags, 0) -> 0` | 2 |
| 137 | `SYS_IOCTL` | `(fd, cmd, arg) -> 0` | 3/4 |
| 138 | `SYS_READV` | `(fd, iov, cnt) -> bytes` | 1/4 |
| 139 | `SYS_WRITEV` | `(fd, iov, cnt) -> bytes` | 1/4 |
| 140 | `SYS_DUP3` | `(old, new, flags) -> fd` | 4 |
| 141 | `SYS_CLONE` | `(flags, stack, ptid) -> tid` | 4 |
| 142 | `SYS_UNAME` | `(struct utsname*, 0, 0) -> 0` | 4 |
| 143 | `SYS_MADVISE` | `(addr, len, advice) -> 0` | 1 |
| 144 | `SYS_SWAPON` | `(path, flags, 0) -> 0` | 1 |
| 145 | `SYS_SCHED_SETATTR` | `(pid, attr, flags) -> 0` — nice/policy | 1 |
| 146 | `SYS_SIGALTSTACK` | `(new, old, 0) -> 0` | 4 |

New capability bits (0–13 taken):

```c
#define PCAP_MODULE  (1ull << 14)   /* SYS_MODULE_*                            */
#define PCAP_MOUNT   (1ull << 15)   /* SYS_MOUNT / SYS_UMOUNT / SYS_SWAPON     */
#define PCAP_DRM     (1ull << 16)   /* DRM master: modeset                     */
#define PCAP_TPM     (1ull << 17)   /* PCR extend / seal / unseal              */
#define PCAP_ADMIN   (1ull << 18)   /* SYS_SCHED_SETATTR on another process    */
```

---

# 7. Four-phase execution roadmap

Every phase ends with the full CLAUDE.md release protocol: `VERSION` bump before
tag, `make release-iso` from a clean tree, `make release-verify`, checksums in
the notes, `make gate-all` green, and an explicit **not-covered** list.

## Phase 1 — Foundations (v1.1 – v1.3)

The brief's Phase 1 is musl + NVMe. Neither is reachable yet; this phase is what
makes them reachable, and it is where the ROI actually is because every other
pillar is blocked on it.

| Ver | Deliverable | Exit criterion |
|---|---|---|
| v1.1 | `struct page`, buddy allocator, physmap at `PHYSMAP_BASE`, multiboot2 mmap consumed. `alloc_frame` kept as a compatibility wrapper. Lock-rank table shifted by 2 in ONE commit; `rank_stack[8]`→`[16]`. | `make gate-all` green with the ceiling still at 1 GiB. Zero behavioural change — this is the negative control for v1.2. |
| v1.2 | Raise the ceiling. Convert every physical dereference to `PHYS_TO_VIRT`. Boot with `-m 4G` and allocate above 1 GiB. | A suite that allocates 2 GiB and touches every page. Reverted-fix control: with the physmap disabled it must fail. |
| v1.3 | `struct file` / `files_struct` / per-process fd table, `FD_INLINE 32` growing to 1024. `dup`/`dup2`/`dup3`. Kill `owner_mask`. `MAX_KPROC`/`MAX_THREADS` dynamically sized. | A suite opening 200 concurrent descriptors across 8 processes. On today's tree it must fail at fd 16. |

**Risk.** The lock-rank shift touches every `klock` initialiser and every
`KLOCK_RANK_*` constant. It is mechanical but wide, and a half-applied shift
reports inversions that are artefacts. Do it alone, in one commit, with
`gate-selftest` and a full `gate-all` on either side.

## Phase 2 — Storage & the block layer (v1.4 – v1.6)

This is where the brief's "NVMe" lands, as early as it honestly can.

| Ver | Deliverable | Exit criterion |
|---|---|---|
| v1.4 | Block layer: `struct bio`, `blk_mq_hw_ctx`, per-CPU queues. `virtio-blk` **re-expressed as a `blk_ops` driver** — same device, new plumbing. | `gate-all` green, byte-identical CAS behaviour. This is a pure refactor and must be provable as one. |
| v1.5 | NVMe 1.4 driver: admin queue bring-up, per-CPU I/O queue pairs, PRP lists, MSI-X through the existing IOMMU remap path, phase-tag completion. `make qemu-nvme` gains a **driver-level** suite. | Read/write/flush verification against a known pattern; a `CREATE_SQ`-removed build must go red. Deadline-based timeouts throughout, never spin counts. |
| v1.6 | VFS: inode/dentry/superblock, dcache, `address_space` per-inode page cache replacing `g_pcache[32]`, directories, `*at` syscalls, mount, procfs, sysfs. CAS becomes a filesystem type under it, on-disk format unchanged. | A pre-v1.6 CAS volume mounts and every file reads byte-identically. Nested `mkdir -p a/b/c/d` + `getdents64`. `/proc/self/maps` correct. |

**Risk.** v1.6 is the largest single change in the document. The `dirent`
`_Static_assert(256)` and the journal record must survive untouched, and the
timestamp bridge (§2.1.3) must be explicit or every existing volume's mtimes
silently become nonsense.

## Phase 3 — Userland & self-hosting (v1.7 – v1.9)

The brief's "musl", now on ground that can carry it.

| Ver | Deliverable | Exit criterion |
|---|---|---|
| v1.7 | Reclaim: LRU lists, `kswapd`, `swap_out`/`swap_in`, `PTE_SWAP`, `PG_PINNED` on every DMA/IOMMU frame. `SYS_SWAPON`, `SYS_MADVISE`. | A workload allocating 2× RAM completes correctly. `g_swap_out`/`g_swap_in` **detection** counters non-zero — a swap suite that never swaps is green for the wrong reason. |
| v1.8 | musl port. `arch_prctl(ARCH_SET_FS)`, `readv`/`writev`, `ioctl`, `clone` flags, extended futex, `wait4`, `uname`, `getcwd`. Heap ceiling raised and backed by v1.7. Milestones **S1, S2**. | busybox `ash` runs interactively over serial and can pipe, redirect, and job-control. |
| v1.9 | `ld.so`: AUXV, `PT_INTERP` handoff, `RELATIVE`/`GLOB_DAT`/`JUMP_SLOT`/`64`/`TPOFF64`/`IRELATIVE`, lazy PLT, `dlopen`/`dlsym`, `DT_NEEDED`. Userspace ASLR. Milestones **S3, S4, S5, S6**. | `make` builds a 3-file project **in-guest**; `tcc` compiles and runs hello-world **in-guest**; a dynamic binary against a `.so` runs. |

**Risk.** `%fs` handling at the ring transition (§4.1.1). A missed `FS_BASE`
save/restore on context switch is a cross-process TLS leak that presents as
random corruption. Add a dedicated assertion: two threads with distinct TLS
values, forced to migrate across cores, each reading its own value back.

## Phase 4 — Platform, display, hardening (v2.0)

| Ver | Deliverable | Exit criterion |
|---|---|---|
| v2.0-a | KDF: `.ko` ET_REL loader, `EXPORT_SYMBOL`, `MODULE_VADDR` within ±2 GiB, build-id refusal, `PCAP_MODULE`. virtio-net rebuilt as the first out-of-tree module. | Load/unload/reload 50× with no leak; a mismatched build-id is **refused**. |
| v2.0-b | ACPICA + OSL. S5 poweroff, power-button GPE, thermal zones read-only. **C/P-states behind `cpuidle=on`, default OFF.** | `poweroff` works; `/proc/acpi` reports zones. Gate runs with cpuidle off and says so in the coverage line. |
| v2.0-c | DRM/KMS: GEM, framebuffer, plane/CRTC/connector, atomic check+commit, `/dev/dri/card0`. `desktop_run()` re-expressed as a DRM client. **`SYS_WIN_*` ABI unchanged.** | `desktop-ui-test.py` passes **unchanged**, still sending `rel` events. `apps-test` green. `make gate` on the regression ISO unaffected. |
| v2.0-d | Hardening: `copy_from_user`/`copy_to_user` everywhere **first**, then `CR4.SMEP\|SMAP\|UMIP`. seccomp-cBPF + `no_new_privs`. Kernel KASLR (2 MiB, 512 slots). TPM 2.0 measured boot via swtpm. CFI via `-fsanitize=cfi-icall`. | A ring-0 read of a user pointer outside a bracket faults. A seccomp-filtered process is killed on a denied syscall. PCR 8/9 match a recomputed hash. **CET recorded as UNVERIFIED under TCG.** |

## 7.1 Out of scope for v2.0 — stated, not implied

A gate whose gaps are invisible is how "verified" drifts from "measured". These
are deliberate exclusions, and the release notes must repeat them:

- **S8/S9 self-hosting GCC.** `tcc` (S6) is the v2.0 claim. GCC needs multi-GiB
  RSS and hours of uptime; it is v2.1+.
- **virtio-gpu 3D / VIRGL / Mesa.** DRM/KMS is modesetting and dumb buffers only.
- **ACPI S3/S4 suspend.** Needs a device suspend/resume chain on every driver.
- **Full reverse mapping (rmap).** Post-fork shared anonymous COW pages are not
  reclaimable in v2.0. This is a real limitation, not an omission.
- **eBPF.** seccomp is classic cBPF with a small auditable verifier, by choice.
- **CET shadow stack / IBT enforcement.** Not emulated by TCG; unverifiable here.
- **NUMA on real hardware.** Testable only via QEMU's synthetic `-numa` topology.
- **SMP-safe module unload under load.** v2.0 quiesces before unload.

## 7.2 Gate additions required

| New tier | Why |
|---|---|
| `gate-mem4g` | `-m 4G`; nothing today boots above 1 GiB of usable RAM |
| `gate-nvme` | `-device nvme` with a **driver-level** suite, not a firmware boot |
| `gate-numa` | `-numa node,cpus=0-1 -numa node,cpus=2-3`; otherwise NUMA is untested code |
| `gate-tpm` | `-tpmdev emulator` + swtpm; PCR verification |
| `gate-swap` | memory overcommit workload asserting on swap **detections** |
| `gate-selfhost` | in-guest `make` + `tcc` build, checksummed output compared against a host build of the same source |

Every one of these must be added to `tools/gate-matrix.sh`'s coverage line, and
`GATE_CAP` re-derived: `gate-selfhost` will not complete in 900 s under TCG, and
a `TRUNCATED` run is not a verdict.

## 7.3 Standing risk: the TCG timing baseline

Every budget in this tree was calibrated under TCG, and they are not independent
of each other (`APPSMP_T`, `CASC_T`, `R62_T`, `GATE_CAP`, `GATE_DIRTY_CAP`,
`BOOT_CAP`, `ITER_CAP`, and every `g_ticks` watchdog). Three items in this
roadmap perturb them:

1. **v1.7 swap** — a swapping boot is dramatically slower.
2. **v2.0-b cpuidle** — changes how long a core spends not executing.
3. **v2.0-d SMAP** — adds `stac`/`clac` to every user copy.

Each must be introduced with the budgets **re-measured and re-derived in the
same commit**, recorded the way v0.91 recorded the TCG baseline. Do not adjust
them incrementally as failures appear; that is how a budget stops expiring on a
genuine stall.

---

## 8. Effort estimate

| Phase | New/changed LoC (kernel) | Ported LoC (external) | Dominant risk |
|---|---|---|---|
| 1 | ~6,000 | — | lock-rank shift breadth; untranslated physical derefs |
| 2 | ~11,000 | — | VFS migration without breaking on-disk CAS compatibility |
| 3 | ~4,000 | ~90,000 (musl + busybox + tcc) | `%fs`/TLS at the ring boundary; relocation correctness |
| 4 | ~9,000 | ~60,000 (ACPICA) | SMAP conversion completeness; DRM/desktop coexistence |

Kernel total ≈ **30,000 lines** on top of today's 37,313 — the kernel roughly
doubles. That is the honest scale of "structural parity", and it is the number
against which any shorter plan should be judged.
