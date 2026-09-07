# OutRun OS — v1.1 → v2.0 Architectural Specification & Implementation Master Plan

Document status: OFFICIAL. Supersedes `ARCH-2.0-PARITY-SPEC.md` (draft).
Baseline: `main` @ `d2d051a`, tag `v1.0.0`. `kernel64.c` 37,313 lines; `init.c` 7,356 lines.
Nothing in this document has been implemented or measured. Every number presented
as a measurement is marked `[TO MEASURE]` until a boot produces it.

---

## 0. The four blocking limits, restated from source

These are the facts the phase ordering is derived from. Each is cited so a
reader can check it rather than trust it.

| # | Limit | Source | Consequence |
|---|---|---|---|
| 1 | Physical memory hard-capped at 1 GiB | `#define IDENT_MAP_LIMIT 0x40000000ull`, kernel64.c:1002; `alloc_frames` refuses past it | `boot.asm` identity-maps exactly 1 GiB with 2 MiB pages. The bump pointer starts at 16 MiB and the first address past the map is `0x40000000` **exactly**. A compiler does not fit. |
| 2 | Global 16-entry descriptor table | `static struct ofile g_ofiles[16];` kernel64.c:10265 | System-wide, not per-process. `owner_mask` is a 64-bit process bitmask, so the design also cannot survive raising `MAX_KPROC` past 64. |
| 3 | `elf_load` refuses dynamic binaries | kernel64.c:28607–28613 | `PT_INTERP` → "needs a dynamic interpreter — this system links statically". `PT_DYNAMIC` → "no relocation processing in this kernel". Both refusals are correct and deliberate; they are narrowed, not deleted. |
| 4 | Lock-rank depth 8 | `uint8_t rank_stack[8]` in `struct pcb` (:3311) and `struct cpu_local` (:309) | A reclaim path can hold `vfs → cas → vblk → zone` while a block-layer completion runs above it. Depth 8 overflows, and an overflowed rank stack reports inversions that are artefacts of the overflow. |

**Two ranks are deliberately unranked and stay that way**: `g_frame_lock` (frame
free-list — must work before the scheduler exists and on APs where there is no
per-CPU rank tracking) and `g_conlock` (IRQ-safe console leaf inside `kprintf`).
They are absent from every table below because they have no rank, not because
they were forgotten.

---

# PHASE 1 — Physical Memory Virtualization & Process Abstraction (v1.1 – v1.3)

## 1.1 `struct page` and the buddy allocator

```c
/* metal/kernel/mm.h — NEW ------------------------------------------------- */

/* The kernel gains a linear map of ALL physical RAM at a fixed high offset,
 * replacing the 1 GiB identity map. Built at boot from the multiboot2 memory
 * map, which is ALREADY PARSED at kernel64.c:832 (tag->type == 6) and whose
 * numbers are currently printed and then discarded. */
#define PHYSMAP_BASE     0xFFFF888000000000ull   /* -120 TiB                   */
#define PHYS_TO_VIRT(pa) ((void *)((uint64_t)(pa) + PHYSMAP_BASE))
#define VIRT_TO_PHYS(va) ((uint64_t)(va) - PHYSMAP_BASE)

#define MAX_ORDER          11        /* 4 KiB .. 4 MiB contiguous             */
#define MAX_ZONES_PER_NODE  3        /* DMA32 / NORMAL / MOVABLE              */
#define MAX_NUMA_NODES      8

/* One per 4 KiB frame. Held at exactly 64 bytes so the array is cache-line
 * indexed and pfn_to_page is a shift, not a multiply. At 16 GiB that is
 * 256 MiB of metadata: the price of being able to reclaim at all.
 *
 * This SUBSUMES three existing ad-hoc structures, and all three must be
 * deleted in the same commit that introduces it or the tree carries two
 * sources of truth for frame state:
 *   - the frame_share()/frame_refs() side table  (-> refcount)
 *   - g_frame_dbg_isfree[]                        (-> flags & PG_FREE)
 *   - g_frame_freelist singly-linked LIFO         (-> free_area[])
 */
struct page {
    uint32_t          flags;         /* PG_* below                            */
    uint32_t          order;         /* buddy order when free; 0 when in use  */
    volatile int32_t  refcount;      /* owners of the frame                   */
    volatile int32_t  mapcount;      /* PTEs pointing here; drives COW         */
    struct page      *lru_next, *lru_prev;
    struct page      *buddy_next;    /* free_area[] chain                     */
    uint64_t          private;       /* file: (ino<<32)|pgidx. anon: swap slot */
    struct address_space *mapping;   /* NULL for anonymous                    */
    uint32_t          node;          /* NUMA node, for free-path routing      */
    uint32_t          _pad;
};
_Static_assert(sizeof(struct page) == 64, "struct page must stay 64 bytes");

#define PG_FREE        (1u <<  0)
#define PG_RESERVED    (1u <<  1)    /* kernel image, ACPI tables, MMIO holes  */
#define PG_LRU         (1u <<  2)
#define PG_ACTIVE      (1u <<  3)
#define PG_REFERENCED  (1u <<  4)    /* accessed bit, accumulated by the clock */
#define PG_DIRTY       (1u <<  5)
#define PG_WRITEBACK   (1u <<  6)    /* I/O in flight: MUST NOT be freed       */
#define PG_ANON        (1u <<  7)    /* swap-backed rather than file-backed    */
#define PG_PINNED      (1u <<  8)    /* DMA target / IOMMU-mapped: never evict */
#define PG_SLAB        (1u <<  9)
#define PG_TABLE       (1u << 10)    /* a page-table page                      */

struct zone {
    uint64_t       base_pfn, nr_pages;
    struct page   *free_area[MAX_ORDER];
    uint32_t       free_count[MAX_ORDER];
    struct page   *active, *inactive;
    uint32_t       nr_active, nr_inactive, nr_free;
    uint32_t       watermark_min, watermark_low, watermark_high;
    uint32_t       node;
    struct klock   lock;             /* RANK 0 after the shift — see §1.4      */
};

struct numa_node {
    uint32_t     id;
    uint64_t     cpu_mask;
    struct zone  zones[MAX_ZONES_PER_NODE];
    uint8_t      distance[MAX_NUMA_NODES];   /* SLIT; 10 == local              */
};

extern struct page      *g_mem_map;          /* pfn-indexed, one per frame     */
extern uint64_t          g_max_pfn;
extern struct numa_node  g_nodes[MAX_NUMA_NODES];
extern uint32_t          g_nr_nodes;

static inline struct page *pfn_to_page(uint64_t pfn) { return &g_mem_map[pfn]; }
static inline uint64_t     page_to_pfn(const struct page *p) { return (uint64_t)(p - g_mem_map); }
static inline uint64_t     page_to_phys(const struct page *p) { return page_to_pfn(p) << 12; }

#define GFP_KERNEL   0x01u   /* may sleep; may trigger reclaim                 */
#define GFP_ATOMIC   0x02u   /* IRQ context: never reclaims, may fail          */
#define GFP_DMA32    0x04u   /* must land below 4 GiB (legacy device rings)    */
#define GFP_ZERO     0x08u
#define GFP_NOFAIL   0x10u

struct page *alloc_pages(uint32_t order, uint32_t gfp);
void         free_pages(struct page *p, uint32_t order);
struct page *alloc_pages_node(uint32_t node, uint32_t order, uint32_t gfp);
```

### 1.1.1 The migration is two commits, and the order is not negotiable

`alloc_frame()` returns a **physical** address that callers dereference
directly, because the first 1 GiB is identity-mapped. Every one of those
dereferences is a latent fault the moment a frame comes from above 1 GiB.

- **v1.1** — introduce `struct page`, buddy, physmap, and `alloc_pages`. Keep
  `alloc_frame()`/`alloc_frames()` as wrappers that still return physical
  addresses **and still enforce `IDENT_MAP_LIMIT`**. Behaviour is bit-identical.
- **v1.2** — convert every dereference site to `PHYS_TO_VIRT()`, then and only
  then lift the ceiling.

Lifting the ceiling first produces exactly the failure kernel64.c:1002 already
documents: the allocator's own zeroing loop writes to an unmapped frame before
any caller ever sees the pointer. v1.1 is therefore the **negative control** for
v1.2 — a full `gate-all` on v1.1 proves the restructure changed nothing, so any
v1.2 failure is attributable to the ceiling lift alone.

## 1.2 Per-process file descriptors

```c
/* metal/kernel/file.h — NEW ------------------------------------------------ */

/* struct file is the OPEN INSTANCE; struct files_struct is the per-process
 * table. Today's g_ofiles[16] conflates the two AND is global. The global half
 * is what makes `make` impossible; the conflation is what makes dup() and
 * fork() inheritance impossible to express correctly. */
struct file {
    struct inode          *inode;
    struct dentry         *dentry;
    struct vfsmount       *mnt;
    uint64_t               pos;
    uint32_t               flags;      /* O_NONBLOCK, O_APPEND, O_CLOEXEC      */
    uint32_t               mode;       /* FMODE_READ | FMODE_WRITE             */
    volatile int32_t       refcount;   /* dup/fork share ONE struct file       */
    const struct file_ops *op;
    void                  *private;    /* pipe | socket | epoll | eventfd |
                                        * oring | drm_file                     */
};

#define FD_INLINE  32                  /* covers every current workload        */
#define FD_MAX   1024                  /* hard ceiling; RLIMIT_NOFILE default  */

struct files_struct {
    volatile int32_t  refcount;        /* CLONE_FILES shares the whole struct  */
    uint32_t          max_fds;         /* current capacity                     */
    uint32_t          next_fd;         /* lowest-free hint                     */
    struct file     **fd;              /* -> fd_inline, or a grown array       */
    struct file      *fd_inline[FD_INLINE];
    uint64_t         *open_fds;        /* bitmap, max_fds bits                 */
    uint64_t         *close_on_exec;   /* bitmap, max_fds bits                 */
    uint64_t          open_inline[FD_INLINE / 64];
    uint64_t          coe_inline[FD_INLINE / 64];
    struct klock      lock;            /* RANK 3 after the shift (was ofile 1) */
};

int             fd_alloc(struct files_struct *, uint32_t min);
struct file    *fd_get(struct files_struct *, int fd);   /* takes a reference  */
void            fd_put(struct file *);
int             fd_install(struct files_struct *, int fd, struct file *);
int             files_expand(struct files_struct *, uint32_t want);
struct files_struct *files_clone(struct files_struct *);  /* fork              */
struct files_struct *files_share(struct files_struct *);  /* CLONE_FILES       */
void            files_close_on_exec(struct files_struct *);
```

`struct kproc` gains `struct files_struct *files`. **`owner_mask` disappears** —
it is a 64-bit process bitmask sized to `MAX_KPROC 64`, replaced by
`file->refcount`. This is a semantic upgrade, not a rename: the current scheme
cannot represent two descriptors in one process sharing an offset (`dup`), which
is why `SYS_SETREDIR` exists as a special case rather than as `dup2`.

`MAX_KPROC 64` and `MAX_THREADS 16` become dynamically allocated in v1.3. Both
fall over at Phase 3's `make -j4`, where `cc1`/`as`/`ld` pipelines exceed 16
kernel threads immediately.

## 1.3 Growth policy and the exit criterion

`files_expand` doubles from `FD_INLINE`, copying pointers under
`files_struct::lock`, and **never shrinks** — a shrink races every `fd_get`
holding a raw index. Growth allocates via `alloc_pages`, which is why this lands
in v1.3 and not v1.1.

**Exit criterion, and it must be a test that can fail:** a suite opening 200
concurrent descriptors across 8 processes, with each process verifying it reads
its own data back. On the v1.0 tree this must fail at fd 16. Build the v1.3
kernel with `FD_INLINE` forced to 16 and confirm the suite goes red — per the
standing rule that a test which cannot fail has not passed.

---

### 1.3a AS BUILT — v1.1 Phase 1 Task 3 (`822e9a3`..)

Task 3 landed EARLY, in v1.1 rather than v1.3, because §1.1's buddy allocator
had already landed and `files_expand` had the allocator it was waiting for.
What shipped differs from the specification above in five ways, each recorded
here because a plan that is quietly diverged from stops being a plan.

| § spec | as built | why |
|---|---|---|
| `struct file` with `inode`/`dentry`/`vfsmount`/`file_ops` | `struct ofile` kept, `owner_mask` → `nref` | There is no inode or dentry layer in this tree yet. Introducing the struct without them would have been three empty fields and a rename; the REFCOUNT is the semantic change, and it landed. |
| `struct files_struct *files` (pointer, refcounted, `CLONE_FILES`) | `struct files_struct files` (inline, by value) | `CLONE_FILES` has no caller: threads share their leader's table through `tg_of()`, which is the identity rule every other per-process resource already follows. A refcounted pointer would be machinery with one possible value. |
| `FD_INLINE 32`, `FD_MAX 1024` | `FD_INLINE 16`, `FD_MAX 256`, `OFILE_MAX 64` | `MAX_KPROC` is 64 and every kproc carries its table INLINE, so `FD_INLINE` is paid 64 times whether used or not. 16 covers every current workload with zero allocation and doubles from there. |
| `files_struct::lock` at its own rank | `g_ofile_lock` (rank 3) covers BOTH layers | One lock, one rank, and the ABBA with `g_vfs_lock` stays engineered out exactly as it was. A second lock would have been a new rank-ordering argument for no measured contention. |
| separate `open_fds` / `close_on_exec` bitmaps | `fd[i] < 0` means free; no `close_on_exec` yet | `O_CLOEXEC` has no ABI bit in `SYS_OPEN` and no caller. A bitmap nothing sets is the "counter nothing increments" trap. Stated as a gap, not shipped as dead state. |

**The exit criterion was implemented in a reduced but falsifiable form**:
`fdceil`, in `cmd_vfs_stress`, opens **24 descriptors in one process** — past
the old global 16 — and asserts distinct numbers, distinct descriptions, and
independent per-description offsets. It is 24-in-1-process rather than
200-across-8 because `OFILE_MAX` is 64 and 200 would be testing the new ceiling
rather than the removal of the old one; 200 concurrent descriptors is a v1.3
target and is restated as such below.

**It was verified to be able to fail.** `make EXTRA=-DFDCEIL_FALSIFY` builds a
kernel whose target is forced to 8, and that build's `fdceil` assertion goes
red while the rest of the boot stays green — the reverted-build check this
document and `CLAUDE.md` both require. Both images' checksums are recorded in
the Task 3 gate table.

**Still open after Task 3**, and deliberately not smuggled in: `dup`/`dup2`/
`dup3` (the syscall numbers are unassigned — `SYS_SETREDIR` remains the special
case, though it now tracks the underlying description and no longer only a
number), `O_CLOEXEC` and `files_close_on_exec`, `CLONE_FILES`, dynamic
`MAX_KPROC`/`MAX_THREADS`, and the 200-descriptor/8-process suite.

## 1.4 Lock-rank shift: −2, one atomic commit

Three new locks must sit **below** everything, because allocation happens
underneath almost every other lock. The existing encoding is `uint8_t`, so
negative ranks are not representable: **the entire table shifts by +2 and the
new locks take 0 and 1.**

| new rank | old | lock | protects |
|---|---|---|---|
| **0** | — | `g_zone_lock[]` | per-zone buddy free lists + LRU lists (one per zone) |
| **1** | — | `g_swap_lock` | swap slot bitmap (Phase 3; rank reserved now) |
| 2 | 0 | `g_redir_lock` | fd redirection table |
| 3 | 1 | `g_files_lock` | per-process `files_struct` (was `g_ofile_lock`) |
| 4 | 2 | `g_vfs_lock` | VFS directory: dirent claim/scan/rewrite/flush |
| 5 | 3 | `g_cas_lock` | CAS superblock counters, bitmap, index, staging |
| 6 | 4 | `g_vblk_lock` | virtio-blk request slots + avail-ring publish |
| 7 | 5 | `g_surf_lock` | surface slot table + pixel-buffer free list |
| 8 | 6 | `g_ipc_lock` | IPC mailbox rings |
| 9 | 7 | `g_gpu_lock` | virtio-gpu resource / scanout state |
| 10 | 8 | `g_audio_lock` | virtio-sound stream state |
| 11 | 9 | `g_net_lock` | virtio-net rings and socket table |
| 12 | 10 | `g_wm_lock` | window-manager stacking order |
| 13 | 11 | `g_dev_lock` | device registry — claim/release, ownership |
| 14 | 12 | `g_vm_lock` | vmfile mappings |
| 15 | 13 | `g_udb_lock` | user database (a leaf) |
| **16** | — | `g_blk_lock` | block-layer queue map (Phase 2; reserved) |
| **17** | — | `g_acpi_lock` | ACPICA OSL global lock (Phase 4; reserved) |

Ranks 16–17 are **reserved now and asserted unused** so that Phase 2 and Phase 4
do not each re-shift the table. A second shift is a second chance to half-apply
one.

```c
/* struct pcb AND struct cpu_local both carry a rank stack; BOTH grow.
 * The cpu_local one has a compile-time offset contract shared with
 * boot/usermode.asm (_Static_assert at kernel64.c:346) — growing it MOVES
 * every field after it, including the stack canary at %gs:80, which is
 * referenced by -mstack-protector-guard-offset. That assert is the thing that
 * catches a half-done change; it must be updated in the SAME commit and must
 * not be relaxed. */
uint8_t rank_stack[16];
uint8_t rank_sp;
_Static_assert(sizeof(((struct pcb *)0)->rank_stack) == 16, "rank depth 16");
```

**Why one commit.** CLAUDE.md documents that a partial rank table actively
invites a collision — v0.95 proposed a lock at rank 7 on the strength of a table
missing six entries, and 7 had been `g_gpu_lock` since v0.51. A half-applied
*shift* is worse: it reports inversions that are artefacts of the shift, and the
natural response to a spurious inversion report is to "fix" a correct
acquisition order. Sequence:

1. `make gate-all` on the pre-shift tree — the before-control.
2. One commit: every `klock` initialiser, every rank constant, both `rank_stack`
   declarations, the `cpu_local` offset assert, and the table in CLAUDE.md.
3. `make gate-selftest` (runs in <1 s) then full `make gate-all`.
4. Any inversion reported after step 3 is real. Any reported *between* 2 and 3 is
   not a verdict.

## Phase 1 exit gate

| Ver | Deliverable | Exit criterion |
|---|---|---|
| v1.1 | `struct page`, buddy, physmap, rank shift −2, `rank_stack[16]` | `gate-all` green **with the 1 GiB ceiling still enforced**. Zero behavioural change. This is v1.2's negative control. |
| v1.2 | Lift `IDENT_MAP_LIMIT`; convert all physical derefs to `PHYS_TO_VIRT` | New `gate-mem4g` tier: boot `-m 4G`, allocate 2 GiB, touch every page, verify. A physmap-disabled build must fail it. |
| v1.3 | `files_struct`, `struct file`, `dup`/`dup2`/`dup3`, dynamic `MAX_KPROC`/`MAX_THREADS` | 200 concurrent fds across 8 processes. An `FD_INLINE=16` build must go red. |

---

# PHASE 2 — Block I/O, NVMe & VFS (v1.4 – v1.6)

## 2.1 Multi-queue block layer

There is no block layer today; `virtio-blk` is called directly from the CAS
code. NVMe cannot be added under that.

```c
/* metal/kernel/blk.h — NEW ------------------------------------------------- */

struct bio_vec { struct page *page; uint32_t offset, len; };

#define REQ_OP_READ     0
#define REQ_OP_WRITE    1
#define REQ_OP_FLUSH    2
#define REQ_OP_DISCARD  3
#define REQ_OP_WRITE_ZEROES 4

#define REQ_SYNC      (1u << 0)
#define REQ_FUA       (1u << 1)
#define REQ_PREFLUSH  (1u << 2)
#define REQ_NOWAIT    (1u << 3)

struct bio {
    struct block_device *bdev;
    uint64_t         sector;           /* 512-byte units, always              */
    uint32_t         op, flags;
    uint32_t         vcnt, vidx;
    struct bio_vec  *vec;
    void           (*end_io)(struct bio *, int32_t res);
    void            *private;
    volatile int32_t remaining;        /* split-bio join counter              */
    int32_t          result;
    struct bio      *chain;
};

struct request {
    struct bio          *bio, *biotail;
    uint64_t             sector;
    uint32_t             nr_sectors, op, flags;
    uint32_t             tag;          /* index into hctx->tags               */
    uint64_t             deadline_ns;  /* A DEADLINE, never a spin count      */
    struct blk_mq_hw_ctx *hctx;
    struct request      *next;
    void                *driver_data;  /* NVMe: the PRP list page             */
};

/* ONE PER CPU. That is the entire point: a per-CPU submission context whose
 * tag bitmap is uncontended is what removes the lock from the fast path, and
 * it is what lets an NVMe queue pair be owned outright by one core. */
struct blk_mq_hw_ctx {
    uint32_t          cpu;
    uint32_t          queue_depth;
    struct request  **tags;            /* tag -> in-flight request            */
    uint64_t         *tag_bitmap;
    uint32_t          nr_active;
    void             *driver_data;     /* NVMe: struct nvme_queue *           */
    struct block_device *bdev;
    struct klock      lock;            /* RANK 16                             */
};

struct block_device {
    char                  name[16];    /* "vblk0", "nvme0n1"                  */
    uint64_t              nr_sectors;
    uint32_t              logical_block_size, physical_block_size;
    uint32_t              max_segments, max_segment_size, max_hw_sectors;
    uint32_t              nr_hw_queues;
    struct blk_mq_hw_ctx *hctx;        /* nr_hw_queues entries                */
    const struct blk_ops *op;
    struct address_space  mapping;     /* buffer cache for raw device I/O     */
    void                 *driver_data;
    uint32_t              flags;       /* BLK_RO, BLK_FUA_CAPABLE             */
};

struct blk_ops {
    int  (*queue_rq)(struct blk_mq_hw_ctx *, struct request *);  /* NON-BLOCKING */
    int  (*poll)    (struct blk_mq_hw_ctx *, uint32_t budget);   /* IOPOLL      */
    int  (*flush)   (struct block_device *);
    void (*complete)(struct request *, int32_t res);
};

int  blk_submit_bio(struct bio *);         /* splits, tags, dispatches         */
void blk_mq_end_request(struct request *, int32_t res);
```

**`queue_rq` must never block.** The existing virtio-blk path parks the caller
(`vblk wait`), which is thread-per-request. v1.4 re-expresses virtio-blk as a
`blk_ops` driver whose `queue_rq` publishes to the avail ring and returns; the
completion IRQ calls `blk_mq_end_request`. This is a **pure refactor and must be
provable as one** — same device, same on-disk bytes, `gate-all` byte-identical.

## 2.2 NVMe 1.4+ driver

```c
/* metal/kernel/nvme.h — NEW ------------------------------------------------ */

/* ---- Controller registers (BAR0, MMIO) ---------------------------------- */
#define NVME_REG_CAP    0x00   /* Controller Capabilities            (64-bit) */
#define NVME_REG_VS     0x08   /* Version                                     */
#define NVME_REG_INTMS  0x0C   /* Interrupt Mask Set                          */
#define NVME_REG_INTMC  0x10   /* Interrupt Mask Clear                        */
#define NVME_REG_CC     0x14   /* Controller Configuration                    */
#define NVME_REG_CSTS   0x1C   /* Controller Status                           */
#define NVME_REG_AQA    0x24   /* Admin Queue Attributes                      */
#define NVME_REG_ASQ    0x28   /* Admin SQ base address              (64-bit) */
#define NVME_REG_ACQ    0x30   /* Admin CQ base address              (64-bit) */

#define NVME_CAP_MQES(c)   (((c)        & 0xFFFFull) + 1)  /* max q entries    */
#define NVME_CAP_CQR(c)    (((c) >> 16) & 1ull)            /* contiguous req'd */
#define NVME_CAP_TO(c)     (((c) >> 24) & 0xFFull)         /* timeout, 500 ms  */
#define NVME_CAP_DSTRD(c)  (((c) >> 32) & 0xFull)          /* doorbell stride  */
#define NVME_CAP_CSS(c)    (((c) >> 37) & 0xFFull)
#define NVME_CAP_MPSMIN(c) (((c) >> 48) & 0xFull)
#define NVME_CAP_MPSMAX(c) (((c) >> 52) & 0xFull)

#define NVME_CC_EN         (1u <<  0)
#define NVME_CC_CSS_NVM    (0u <<  4)
#define NVME_CC_MPS(n)     ((uint32_t)(n) << 7)
#define NVME_CC_AMS_RR     (0u << 11)
#define NVME_CC_SHN_NORMAL (1u << 14)
#define NVME_CC_IOSQES     (6u << 16)   /* 2^6 = 64-byte SQ entry             */
#define NVME_CC_IOCQES     (4u << 20)   /* 2^4 = 16-byte CQ entry             */

#define NVME_CSTS_RDY      (1u << 0)
#define NVME_CSTS_CFS      (1u << 1)    /* controller fatal status            */
#define NVME_CSTS_SHST(s)  (((s) >> 2) & 3u)

/* Doorbell for queue qid: BAR0 + 0x1000 + ((2*qid + is_cq) << (2 + DSTRD)).
 * HARDCODING STRIDE 0 WORKS ON QEMU AND CORRUPTS A REAL CONTROLLER'S DOORBELL
 * REGION. DSTRD comes from CAP and nowhere else. */
#define NVME_DB(bar, qid, iscq, dstrd) \
    ((volatile uint32_t *)((uint8_t *)(bar) + 0x1000 + \
      ((((uint32_t)(qid) << 1) | (uint32_t)(iscq)) << (2 + (dstrd)))))

struct nvme_cmd {                       /* Submission Queue Entry             */
    uint8_t  opcode;
    uint8_t  flags;                     /* PSDT (bits 7:6) = 0 for PRP        */
    uint16_t cid;                       /* command identifier == blk tag      */
    uint32_t nsid;
    uint64_t _rsv2;
    uint64_t mptr;                      /* metadata pointer                   */
    uint64_t prp1, prp2;                /* PRP entries; SGL not used in v2.0  */
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};
_Static_assert(sizeof(struct nvme_cmd) == 64, "NVMe SQE must be 64 bytes");

struct nvme_cqe {                       /* Completion Queue Entry             */
    uint32_t result;                    /* command-specific (e.g. queue count) */
    uint32_t _rsv;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;                    /* bit 0 = PHASE TAG; 15:1 = status   */
};
_Static_assert(sizeof(struct nvme_cqe) == 16, "NVMe CQE must be 16 bytes");

#define NVME_CQE_PHASE(s)  ((s) & 1u)
#define NVME_CQE_SC(s)     (((s) >> 1) & 0xFFu)   /* status code              */
#define NVME_CQE_SCT(s)    (((s) >> 9) & 0x7u)    /* status code type         */

struct nvme_queue {
    struct nvme_cmd   *sq;              /* physically contiguous, page-aligned */
    struct nvme_cqe   *cq;
    uint64_t           sq_dma, cq_dma;  /* IOVAs when behind a domain          */
    volatile uint32_t *sq_db, *cq_db;
    uint16_t           qid, depth;
    uint16_t           sq_tail, sq_head, cq_head;
    uint8_t            cq_phase;        /* flips on wrap; THE completion test  */
    uint8_t            polled;
    uint32_t           cpu;             /* the core that owns this pair        */
    struct klock       lock;            /* RANK 16, with g_blk_lock            */
    struct request   **tags;
};

struct nvme_ctrl {
    void               *bar0;           /* mapped UNCACHED (PTE_PCD)           */
    uint64_t            cap;
    uint32_t            dstrd, mps, page_size, max_transfer;
    uint16_t            bdf;
    uint16_t            _pad;
    struct nvme_queue   admin;
    struct nvme_queue  *io;
    uint32_t            nr_io_queues;   /* GRANTED, not requested              */
    uint32_t            nsid_count;
    struct iommu_domain *dom;           /* the driver's own domain             */
    struct block_device *bdev;
    char                serial[21], model[41];
};

/* Admin opcodes */
#define NVME_ADMIN_DELETE_SQ 0x00
#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_ADMIN_DELETE_CQ 0x04
#define NVME_ADMIN_CREATE_CQ 0x05
#define NVME_ADMIN_IDENTIFY  0x06
#define NVME_ADMIN_ABORT     0x08
#define NVME_ADMIN_SET_FEAT  0x09
#define NVME_ADMIN_GET_FEAT  0x0A
/* NVM opcodes */
#define NVME_CMD_FLUSH       0x00
#define NVME_CMD_WRITE       0x01
#define NVME_CMD_READ        0x02
/* Feature identifiers */
#define NVME_FEAT_NUM_QUEUES 0x07
```

### 2.2.1 Bring-up sequence — each step's failure mode is named

1. Enumerate PCI class `0x010802` via the v0.95 config-space reader
   (`SYS_PCI_CFG_READ` / kernel-internal equivalent).
2. Map BAR0 **uncached** (`PTE_PCD`). `vm_clone_user` (:23368) already refuses to
   clone `PTE_PCD` pages, so the mapping is fork-safe for free.
3. `CC.EN = 0`; poll `CSTS.RDY == 0`. **Deadline** = `NVME_CAP_TO(cap) * 500 ms`
   measured against `ktime_get_us()`. Never a spin count — the spec states the
   timeout in time units, and a spin count means different durations at 1 vCPU
   and at 4.
4. Allocate admin SQ/CQ: `alloc_pages(order, GFP_DMA32 | GFP_ZERO)`, physically
   contiguous (`CAP.CQR` may *require* it), page-aligned.
5. Program `AQA` (depths), `ASQ`, `ACQ`. Write
   `CC = IOSQES|IOCQES|MPS|CSS_NVM|AMS_RR|EN`. Poll `CSTS.RDY == 1` on the same
   deadline. **If `CSTS.CFS` sets, the controller is fatally broken: report,
   disable, and stop.** Do not reset in a loop — a reset loop against a wedged
   controller is indistinguishable from a hang and consumes the whole `GATE_CAP`.
6. `IDENTIFY` controller (CNS=1) then namespace (CNS=0). Extract capacity, LBA
   format (`LBAF`), and `MDTS` → `max_transfer = (1 << MDTS) * page_size`.
7. `SET_FEATURES(NUM_QUEUES)` requesting `ncpu` pairs. **The controller may grant
   fewer, and the granted count is in the CQE `result` field.** A driver that
   allocates `ncpu` queues after being granted 2 writes doorbells for queues that
   do not exist — an MMIO write into a region the controller does not decode.
8. Per pair: `CREATE_CQ` **then** `CREATE_SQ`. The SQ creation command names its
   CQ, so the reverse order fails with "invalid queue identifier".
9. One MSI-X vector per queue, routed through the existing interrupt-remapping
   path. `smp4-iommu` with `intremap=on` is already a gate tier, so this is
   covered by an existing configuration rather than a new claim.

### 2.2.2 Two failure modes that must be designed against, not debugged later

**Completion is the phase tag, not a head index.** A CQE is valid when
`NVME_CQE_PHASE(cqe.status) != q->cq_phase`; the phase flips on every wrap. A
driver that polls a head index works perfectly until the first wrap and then
silently completes stale commands. This is the standard NVMe driver bug.

**Queue DMA addresses are IOVAs, not physical addresses**, whenever the
controller sits behind a translating domain — which it does, since v0.95 gives
every claimed device its own domain. Every frame backing a queue ring or a PRP
list **must** carry `PG_PINNED` before Phase 3's reclaim exists, or a swapped-out
ring becomes a device writing into reclaimed memory. Pin at IOMMU-map time, in
the mapping function, so no driver can forget.

### 2.2.3 The test that must be able to fail

`make qemu-nvme` exists today and boots through OVMF firmware. **The kernel has
no NVMe code**, so firmware reads the image and the kernel then cannot touch the
device. A passing `qemu-nvme` boot on the v1.0 tree is therefore evidence of
nothing about NVMe.

The first commit of v1.5 adds an assertion that fails on the current tree.
Before believing the driver, build it with step 8's `CREATE_SQ` removed and
confirm `gate-nvme` goes red. Count **detections** (`g_nvme_cmds_completed`,
`g_nvme_phase_wraps`) separately from failures: a suite asserting only
`failures == 0` is green on a workload that never issued an I/O, and a suite that
never wraps the CQ has not tested the phase tag at all. `gate-nvme` must submit
`depth + 8` commands to force at least one wrap, and must assert
`g_nvme_phase_wraps > 0`.

## 2.3 VFS: inode / dentry / superblock

Today's `struct dirent` (kernel64.c:6454) is a 256-byte packed on-disk record
with a `_Static_assert` on its size, 16 direct chunk hashes, single- and
double-indirect maps, uid/gid/mode, and tick-based timestamps. **It is a good
format and is not discarded** — it becomes the backing store of one filesystem
type beneath a generic VFS.

```c
/* metal/kernel/vfs.h — NEW ------------------------------------------------- */

struct inode {
    uint64_t                 ino;
    uint32_t                 mode;      /* S_IFREG|S_IFDIR|S_IFLNK|S_IFCHR + rwx */
    uint32_t                 uid, gid;
    uint64_t                 size;
    uint64_t                 atime, mtime, ctime;   /* ns since epoch          */
    uint32_t                 nlink;
    uint32_t                 flags;     /* I_DIRTY, I_TIME_APPROX (see §2.3.2) */
    volatile int32_t         refcount;
    struct super_block      *sb;
    const struct inode_ops  *op;
    const struct file_ops   *fop;
    struct address_space    *mapping;
    void                    *private;   /* CAS: the dirent index               */
    struct inode            *hash_next; /* inode cache bucket                  */
    struct klock             lock;      /* RANK 4, with g_vfs_lock             */
};

struct dentry {
    char              name[VFS_NAME_MAX];   /* 64, matching the on-disk dirent */
    uint32_t          hash;
    uint32_t          flags;                /* D_NEGATIVE, D_MOUNTPOINT        */
    struct inode     *inode;                /* NULL == proven-absent (negative) */
    struct dentry    *parent;
    struct dentry    *child, *sibling;      /* first-child / next-sibling tree  */
    struct dentry    *hash_next;            /* dcache bucket chain              */
    volatile int32_t  refcount;
};

struct super_block {
    uint64_t                 magic;
    uint32_t                 block_size;
    uint64_t                 boot_epoch_ns;  /* §2.3.2 timestamp bridge         */
    struct dentry           *root;
    const struct super_ops  *op;
    struct block_device     *bdev;           /* NULL for pseudo-filesystems     */
    void                    *fs_private;
    uint32_t                 flags;          /* SB_RDONLY, SB_NOEXEC, SB_NOSUID */
    struct klock             lock;
};

struct vfsmount {
    struct dentry      *mountpoint;          /* in the PARENT tree              */
    struct super_block *sb;
    struct vfsmount    *parent, *next;
    uint32_t            flags;               /* MNT_RDONLY|MNT_NOEXEC|MNT_NOSUID */
    volatile int32_t    refcount;
};

struct inode_ops {
    struct dentry *(*lookup)  (struct inode *dir, struct dentry *);
    int (*create)  (struct inode *dir, struct dentry *, uint32_t mode);
    int (*mkdir)   (struct inode *dir, struct dentry *, uint32_t mode);
    int (*rmdir)   (struct inode *dir, struct dentry *);
    int (*unlink)  (struct inode *dir, struct dentry *);
    int (*rename)  (struct inode *od, struct dentry *odn,
                    struct inode *nd, struct dentry *ndn);
    int (*symlink) (struct inode *dir, struct dentry *, const char *target);
    int (*readlink)(struct dentry *, char *buf, uint32_t n);
    int (*setattr) (struct inode *, const struct iattr *);
    int (*getattr) (struct inode *, struct kstat *out);
    int (*permission)(struct inode *, uint32_t mask);   /* uses euid/egid       */
};

struct file_ops {
    int64_t (*read)    (struct file *, void *buf, uint64_t n, uint64_t *off);
    int64_t (*write)   (struct file *, const void *buf, uint64_t n, uint64_t *off);
    int64_t (*lseek)   (struct file *, int64_t off, int whence);
    int     (*mmap)    (struct file *, struct vm_area *);
    int     (*fsync)   (struct file *, int datasync);
    int     (*ioctl)   (struct file *, uint32_t cmd, uint64_t arg);
    int     (*poll)    (struct file *, struct poll_table *);
    int     (*getdents)(struct file *, struct dirent64 *out, uint32_t bytes);
    int     (*release) (struct file *);
    /* Phase 3: if present, the async path uses it and parks no thread. If
     * absent, the io worker pool falls back to ->read/->write, which is
     * correct but not fast. Both paths must exist and BOTH must be tested. */
    int     (*submit_async)(struct file *, struct oring_sqe *, struct io_ctx *);
};

/* Per-inode page cache. REPLACES the global g_pcache[MAX_PCACHE] — 32 entries,
 * fixed, no eviction (kernel64.c:1405,1427). Thirty-two pages is 128 KiB of
 * cache for the whole system; a single `cc1` invocation exceeds it. */
struct address_space {
    struct radix_tree  pages;           /* pgidx -> struct page *              */
    struct inode      *host;
    uint64_t           nrpages;
    uint32_t           flags;
    struct klock       lock;            /* RANK 4                              */
};

/* POSIX-shaped directory entry for getdents64. DISTINCT from the on-disk
 * `struct dirent`, which stays exactly 256 bytes and packed. Naming them alike
 * is a hazard; the on-disk one should be renamed `struct cas_dirent` in the
 * same commit so no call site can confuse them. */
struct dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};
```

### 2.3.1 CAS becomes a filesystem type

`struct super_ops` gets a CAS implementation whose `->private` on each inode is
the existing dirent index. The on-disk format, `VFS_DIR_BLOCKS`, the journal
record, and `cas_mount`'s restore path are **untouched**. Exit criterion: a
volume written by a pre-v1.6 kernel mounts and every file reads byte-identically.

### 2.3.2 Timestamp bridge — must be explicit or every mtime becomes nonsense

CLAUDE.md records the deliberate decision that dirent `mtime`/`atime` are
**boot-relative ticks at 100 Hz**, not epoch seconds, because redefining them
would silently reinterpret every timestamp on every existing volume. The inode
layer uses nanoseconds since epoch. The bridge lives in the CAS driver's
`->getattr`:

- `sb->boot_epoch_ns` is recorded at mount.
- A dirent timestamp of **0 still means UNKNOWN** (the v0.72 rule for `mode`) and
  maps to mount time.
- A **new superblock field** records the epoch of the boot that wrote each
  timestamp. Volumes lacking it — every existing volume — report ticks converted
  against mount time and set `I_TIME_APPROX`, surfaced in `struct kstat::flags`,
  so a caller can distinguish a measured time from a reconstructed one.

### 2.3.3 procfs and sysfs

Superblocks with `bdev == NULL`, generated on read:

```
/proc/self  /proc/<pid>/{status,cmdline,maps,stat,fd/}
/proc/{meminfo,cpuinfo,uptime,stat,mounts,interrupts,buddyinfo}
/sys/class/{block,net,gpu}/  /sys/bus/pci/devices/<bdf>/{vendor,device,class,resource}
/sys/kernel/{version,ranks}          <- the live lock-rank table
```

`/proc/self/maps` is not a convenience: musl's `dlopen` reads it, so it is a hard
dependency of Phase 3.

## Phase 2 exit gate

| Ver | Deliverable | Exit criterion |
|---|---|---|
| v1.4 | `bio`/`request`/`blk_mq_hw_ctx`; virtio-blk re-expressed as `blk_ops` | `gate-all` green, CAS bytes identical. A pure refactor, provable as one. |
| v1.5 | NVMe 1.4+ driver, per-CPU queue pairs, PRP, MSI-X, phase-tag completion | `gate-nvme`: read/write/flush against a known pattern, `depth+8` commands to force a wrap, `g_nvme_phase_wraps > 0`. A `CREATE_SQ`-removed build must go red. |
| v1.6 | inode/dentry/superblock, dcache, per-inode `address_space`, directories, `*at` syscalls, mount, procfs, sysfs | `gate-vfs`: a pre-v1.6 volume mounts byte-identically; `mkdir -p a/b/c/d` + `getdents64`; `/proc/self/maps` correct. |

---

# PHASE 3 — Reclaim, POSIX Toolchain & Dynamic Execution (v1.7 – v1.9)

## 3.1 Page reclaim and swap

```c
/* metal/kernel/swap.h — NEW ------------------------------------------------ */

struct swap_device {
    struct block_device *bdev;
    uint32_t             nr_slots;
    uint32_t             nr_free;
    uint64_t            *slot_bitmap;
    uint32_t             next_hint;
    uint32_t             prio;
    struct klock         lock;         /* RANK 1                              */
};

/* A swapped-out anonymous page leaves a NON-PRESENT PTE carrying its slot —
 * exactly the discipline PTE_ZFOD already uses to park permissions in a
 * non-present entry (kernel64.c:14765). PTE_SWAP and PTE_ZFOD are MUTUALLY
 * EXCLUSIVE, which is what keeps vm_fault_handle a flat chain of exclusive
 * tests rather than a decision tree. */
#define PTE_SWAP        (1ull << 10)   /* AVL bit, free in this kernel        */
#define SWP_SLOT(pte)   (((pte) >> 12) & 0xFFFFFFFFull)
#define SWP_ENTRY(slot, keep) (((uint64_t)(slot) << 12) | PTE_SWAP | (keep))

int swap_out(struct page *, uint64_t *pte);
int swap_in (uint64_t *pte, uint64_t cr3, uint64_t va);
int swapon  (const char *path, uint32_t flags);
```

**Policy: two-list clock (second chance)**, not true LRU — this kernel has no
reference-sampling thread and cannot afford a full scan.

- `kswapd` is an ordinary kernel thread (`g_threads[]` already supports this),
  woken when `zone->nr_free < watermark_low`, sleeping at `watermark_high`.
- Scan `zone->inactive` from the tail. Accessed bit set → clear it, set
  `PG_REFERENCED`, rotate to head (second chance). Otherwise evict.
- Clean file page → drop. Dirty file page → writeback through §2.1, `PG_WRITEBACK`
  set, freed on completion. Anonymous → `swap_out`.
- `PG_PINNED` skipped unconditionally, always.

**Stated limitation:** full reverse mapping is out of scope. Anonymous pages are
reclaimed only from the faulting process's own tables, so post-fork shared COW
pages held by two processes are **not reclaimable in v2.0**. This is recorded in
the release notes rather than discovered later as a leak.

## 3.2 `elf_load`: narrow the refusal, do not delete it

The existing refusals (kernel64.c:28607–28613) are correct for a kernel with no
relocation processing. They are **narrowed**: the kernel learns to load
`PT_INTERP` and hand off, and continues to refuse a `PT_DYNAMIC` main object it
cannot service.

```c
/* elf_load gains, in this order:
 *   PT_INTERP     -> load THAT ELF at a randomised base; enter at ITS e_entry
 *   PT_LOAD       -> unchanged, INCLUDING the existing W^X refusal (:28636)
 *   PT_GNU_RELRO  -> recorded; ld.so mprotects it read-only after relocation
 *   PT_TLS        -> recorded for the initial TLS image
 *   AUXV          -> appended to the process-start block
 *
 * The empty-PT_LOAD skip added in v0.96 (memsz==0 && filesz==0) stays: ld emits
 * exactly that for a program with no writable data, and calc.elf is one.
 */
struct auxv_ent { uint64_t a_type, a_val; };
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7      /* ld.so load bias                                    */
#define AT_ENTRY  9
#define AT_UID   11
#define AT_EUID  12
#define AT_GID   13
#define AT_EGID  14
#define AT_SECURE 23
#define AT_RANDOM 25     /* 16 bytes; musl's stack guard reads this            */
```

**A sizing hazard.** `uargs_build()` (kernel64.c:23302) writes argc/argv/envp
into the **top 1 KiB** of the ring-3 stack (`USTK_TOP` region), and AUXV goes
after envp's NULL in the same block. Sixteen auxv entries at 16 bytes is 256
bytes — it fits, but only just, and `UARG_N`/`UARG_LEN` already consume most of
the rest. The bound must be a `_Static_assert`, not an assumption; overflowing it
writes past the process-start block into the stack the program is about to use.

## 3.3 musl and `ld.so`

Recommendation: **musl**, not newlib. newlib's `_impure_ptr` reent model fits
poorly with real POSIX threads (which this kernel already has), musl's syscall
surface is smaller, and it is designed for static linking — which this kernel can
already do today, giving milestone S1 a short path.

The work is not writing musl; it is making the syscall surface musl expects
exist. Gap analysis:

| musl needs | v1.0 tree | Action |
|---|---|---|
| `arch_prctl(ARCH_SET_FS)` | absent | **Hard blocker.** musl TLS is `%fs`-relative; nothing links without it. |
| `openat`/`fstatat`/`unlinkat`/`mkdirat` | flat `SYS_OPEN` only | Phase 2 §2.3 + syscalls 128–131 |
| `readv`/`writev` | absent | 138/139; `struct bio_vec` already has the iovec shape |
| `ioctl` | absent | 137; needed by termios and DRM |
| `getdents64` | `SYS_READDIR`, non-POSIX layout | 132 |
| `clone` with flags | fixed-semantics `SYS_THREAD_CREATE` | 141 |
| futex `WAIT_BITSET`/`REQUEUE` | `FUTEX_WAIT`/`WAKE` only | extend 64/65 |
| `sigaltstack` | absent | 146 |
| `wait4` with rusage | `WAITPID` + `GETRUSAGE` separately | merge |
| `uname`/`getcwd`/`chdir` | absent | 142/134/133 |
| `dup`/`dup2`/`dup3` | `SETREDIR` special case | 140 |
| `brk` > 4 MiB | `HEAP_MAX_BYTES` = 4 MiB (:1443) | raise; backed by Phase 1 |
| `mmap` > 64 MiB | `MMAP_MAX_BYTES` = 64 MiB (:1442) | raise |

### 3.3.1 The `%fs` hazard

This kernel uses `%gs` for `cpu_local` under a compile-time contract
(`-mstack-protector-guard-offset`, `_Static_assert` at kernel64.c:346). `%fs` is
free for userspace, but `FS_BASE` must be saved and restored on **every** context
switch, including migration between cores. A missed save is a cross-process TLS
leak that presents as random memory corruption, not as a scheduling bug.

**Dedicated assertion:** two threads with distinct `FS_BASE` values, forced to
migrate across cores via the existing affinity/migration machinery, each reading
its own TLS value back N times. Build with the restore removed and confirm it
fails.

### 3.3.2 Relocation processing

`ld.so` is a static PIE that self-relocates before it can call anything.

| Type | # | Computation | Ordering constraint |
|---|---|---|---|
| `R_X86_64_RELATIVE` | 8 | `*where = base + addend` | **First, inside `_dl_start`, before any global is read.** The linker cannot call through its own GOT until this completes. |
| `R_X86_64_GLOB_DAT` | 6 | `*where = sym` | after symbol resolution |
| `R_X86_64_JUMP_SLOT` | 7 | `*where = sym` | lazy via `_dl_runtime_resolve`, or eager under RELRO/`BIND_NOW` |
| `R_X86_64_64` | 1 | `*where = sym + addend` | |
| `R_X86_64_COPY` | 5 | `memcpy(where, sym, size)` | non-PIE executables only |
| `R_X86_64_TPOFF64` | 18 | `*where = tls_offset + addend` | initial-exec TLS |
| `R_X86_64_DTPMOD64` | 16 | `*where = l_tls_modid` | global-dynamic TLS |
| `R_X86_64_DTPOFF64` | 17 | `*where = sym_offset_in_module` | |
| `R_X86_64_IRELATIVE` | 37 | `*where = ((fn)(base+addend))()` | **Last**, after everything resolves. musl's `memcpy` is an ifunc. |

```c
struct link_map {
    uint64_t          l_addr;          /* load bias                            */
    const char       *l_name;
    Elf64_Dyn        *l_ld;
    struct link_map  *next, *prev;
    const char       *strtab;
    Elf64_Sym        *symtab;
    uint32_t         *gnu_hash;        /* DT_GNU_HASH preferred...             */
    uint32_t         *hash;            /* ...but DT_HASH-only libraries exist,
                                        * and assuming GNU_HASH is a real crash */
    Elf64_Rela       *rela;    uint64_t rela_count;
    Elf64_Rela       *jmprel;  uint64_t jmprel_count;
    uint64_t         *got_plt;
    uint64_t          init, fini;
    uint64_t          relro_start, relro_len;
    uint64_t          tls_modid, tls_offset, tls_size, tls_align;
};
```

`_dl_runtime_resolve` must preserve **all six integer argument registers and the
vector registers**. A resolver that clobbers `%xmm0` breaks the first varargs
call through the PLT, intermittently.

## 3.4 Self-hosting milestones

| # | Milestone | Requires | Proves |
|---|---|---|---|
| S1 | static musl hello-world | `arch_prctl`, `writev`, `*at` | the syscall ABI is real |
| S2 | busybox `ash` interactive | S1 + termios, `ioctl`, job control, per-process fds | the shell is not special-cased |
| S3 | dynamic hello-world via `ld.so` | AUXV, `PT_INTERP`, `RELATIVE`+`GLOB_DAT`+`JUMP_SLOT` | dynamic linking works |
| S4 | `dlopen`/`dlsym` | `/proc/self/maps`, `DT_NEEDED`, TLS relocs | the loader is complete |
| S5 | GNU `make` builds a 3-file project | fork/execve depth, pipes, `wait4`, ≥64 fds | process management scales |
| S6 | **`tcc` compiles + links hello-world in-guest** | S5 + heap >16 MiB + real `mmap` | **a compiler runs — the v2.0 claim** |

**S6 is the v2.0 target.** GCC self-hosting (a `cc1` needing ~1 GiB RSS on a
2 MB single translation unit) is v2.1+ and appears in §7.1 as a non-goal.
Claiming it inside a four-phase plan would be planning a result rather than a
path.

## Phase 3 exit gate

| Ver | Deliverable | Exit criterion |
|---|---|---|
| v1.7 | LRU lists, `kswapd`, `swap_device`, `PTE_SWAP`, `PG_PINNED` everywhere | `gate-swap`: a 2×RAM workload completes correctly **and** `g_swap_out`/`g_swap_in` detection counters are non-zero. A swap suite that never swaps is green for the wrong reason. |
| v1.8 | musl port; `arch_prctl`, `readv`/`writev`, `ioctl`, `clone`, extended futex; heap/mmap ceilings raised. **S1, S2** | busybox `ash` over serial: pipes, redirection, job control. TLS-migration assertion passes; a restore-removed build fails it. |
| v1.9 | `ld.so`, AUXV, `PT_INTERP`, full relocation set, lazy PLT, `dlopen`. Userspace ASLR. **S3–S6** | `gate-selfhost`: in-guest `make` + `tcc` producing a binary whose checksum matches a host build of the same source. |

---

# PHASE 4 — Driver Framework, Platform & Hardening (v2.0)

## 4.1 Kernel Driver Framework

```c
/* metal/kernel/module.h — NEW ---------------------------------------------- */

struct kernel_symbol { const char *name; uint64_t addr; };

/* Gathered by linker.ld into __ksymtab_start/__ksymtab_end. */
#define EXPORT_SYMBOL(sym)                                            \
    static const struct kernel_symbol __ksym_##sym                    \
    __attribute__((used, section("__ksymtab"), aligned(16))) =        \
    { #sym, (uint64_t)&sym }

struct module {
    char      name[64];
    uint64_t  text_base,   text_size;      /* R+X                             */
    uint64_t  rodata_base, rodata_size;    /* R+NX                            */
    uint64_t  data_base,   data_size;      /* RW+NX  — W^X preserved          */
    int     (*init)(void);
    void    (*exit)(void);
    volatile int32_t refcount;             /* nonzero blocks unload           */
    struct module   *next;
    uint64_t  build_id;                    /* ABI hash; mismatch = REFUSAL    */
    uint8_t   sha256[32];                  /* measured into PCR 9 before use  */
    uint32_t  flags;
};

int module_load  (const void *elf, uint64_t size, const char *args);
int module_unload(const char *name);
```

`.ko` files are **ET_REL**, not ET_DYN. Relocations needed: `R_X86_64_64`,
`R_X86_64_PC32`, `R_X86_64_32S`, `R_X86_64_PLT32`.

**`PC32`/`PLT32` are ±2 GiB displacements.** Module text must therefore be
allocated **within 2 GiB of the kernel image**; a general high-address vmalloc
region produces relocation overflows that appear only when the allocator happens
to return a far address — an intermittent that looks like memory corruption.
Reserve a dedicated `MODULE_VADDR` window adjacent to the kernel and assert the
displacement at relocation time rather than truncating it.

**`build_id` refusal.** A module built against a different kernel that loads
anyway corrupts silently. `build_id` hashes every exported symbol name plus the
sizes of every ABI struct the module can see; a mismatch is a refusal — the same
discipline `SYS_DESKTOP_SETTINGS` already applies when it refuses a disagreeing
size rather than partly filling a buffer.

Resource arbitration reuses `g_dev_lock` (rank 13 post-shift) and the v0.95
claim/release registry unchanged: a module claiming a PCI device takes exactly
the path a ring-3 userspace driver takes.

## 4.2 ACPICA

Port ACPICA; do not write an AML interpreter. AML is a bytecode with a dynamic
namespace, mutexes and region handlers, and firmware finds new ways to break
interpreters indefinitely. The OSL is ~40 functions, most of which map onto
machinery that already exists (PCI config reads from v0.95, `klock`,
`ktime_get_us` from v0.92, the kernel thread pool).

```c
struct cpufreq_policy {
    uint32_t cpu, cur_khz, min_khz, max_khz, nr_pstates, governor;
    struct { uint32_t khz, mw, latency_us, ctrl, status; } pstate[16];  /* _PSS */
    const struct cpufreq_driver *drv;
};

struct cpuidle_state {                 /* _CST                                 */
    uint32_t type;                     /* C1 | C2 | C3                         */
    uint32_t exit_latency_us, target_residency_us;
    uint32_t addr;                     /* IO port, or MWAIT hint               */
    uint32_t entry_method;             /* HLT | IO | FFH(MWAIT)                */
};

struct thermal_zone {
    char     name[16];
    int32_t  temp_mdeg;                                  /* _TMP, millidegrees */
    int32_t  trip_passive, trip_hot, trip_critical;      /* _PSV / _HOT / _CRT */
    uint32_t polling_ms;
};
```

**C/P-states ship DISABLED by default, behind `cpuidle=on`.** Under TCG, idle
states are either unimplemented or lies, and `HLT` in a TCG guest does not do
what it does on metal. Every timing budget in this tree is TCG-calibrated;
changing how long a core spends not executing perturbs them the same way
enabling KVM would. The gate continues to run with `cpuidle` **off** and says so
in the coverage line until a deliberate re-baseline is performed and recorded in
one commit — the procedure CLAUDE.md already prescribes for the KVM switch.

S5 poweroff (`PM1a_CNT` `SLP_TYP|SLP_EN`) and the power-button GPE are cheap and
land first. S3/S4 are non-goals (§7.1).

## 4.3 DRM/KMS

```c
/* metal/kernel/drm.h — NEW ------------------------------------------------- */

struct drm_gem_object {
    uint32_t          handle;
    uint64_t          size;
    struct page     **pages;           /* size/4096 entries                    */
    uint64_t          iova;            /* IOMMU address when device-visible    */
    volatile int32_t  refcount;
    uint32_t          flags;           /* GEM_SCANOUT | GEM_CACHED | GEM_PINNED */
    struct drm_device *dev;
    void             *driver_private;  /* virtio-gpu: resource id              */
};

struct drm_framebuffer {
    uint32_t id, width, height, format;      /* DRM_FORMAT_XRGB8888 etc.       */
    uint32_t pitch[4], offset[4];
    struct drm_gem_object *bo[4];
    volatile int32_t refcount;
};

struct drm_plane_state {
    struct drm_framebuffer *fb;
    int32_t  crtc_x, crtc_y;
    uint32_t crtc_w, crtc_h;
    uint32_t src_x, src_y, src_w, src_h;     /* 16.16 fixed point              */
    uint32_t alpha, zpos, rotation;
};

struct drm_plane {
    uint32_t id, type;                       /* PRIMARY | CURSOR | OVERLAY     */
    uint32_t possible_crtcs;
    uint32_t nr_formats; const uint32_t *formats;
    struct drm_plane_state state;
};

struct drm_display_mode {
    uint32_t clock_khz;
    uint32_t hdisplay, hsync_start, hsync_end, htotal;
    uint32_t vdisplay, vsync_start, vsync_end, vtotal;
    uint32_t flags;
};

struct drm_crtc {
    uint32_t id;
    struct drm_display_mode mode;
    struct drm_plane *primary, *cursor;
    int      enabled, active;
    uint64_t vblank_count;
    uint64_t last_vblank_ns;
};

struct drm_connector {
    uint32_t id, type;                       /* VIRTUAL | HDMI | DP | eDP      */
    uint32_t status;                         /* CONNECTED | DISCONNECTED | UNKNOWN */
    uint8_t  edid[256];
    uint32_t nr_modes;
    struct drm_display_mode modes[16];
    struct drm_crtc *crtc;
};

/* ATOMIC: the whole display state changes at once or not at all. The CHECK
 * phase must be side-effect free — that is what lets a client TEST_ONLY a
 * configuration, and it is the property most easily destroyed by a driver that
 * "helpfully" allocates during check. */
#define DRM_ATOMIC_TEST_ONLY     (1u << 0)
#define DRM_ATOMIC_NONBLOCK      (1u << 1)
#define DRM_ATOMIC_ALLOW_MODESET (1u << 2)

struct drm_atomic_state {
    struct drm_crtc_state      *crtcs;   uint32_t nr_crtcs;
    struct drm_plane_state     *planes;  uint32_t nr_planes;
    struct drm_connector_state *conns;   uint32_t nr_conns;
    uint32_t flags;
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

**Coexistence with the desktop is non-negotiable.** CLAUDE.md separates the
regression ISO from the desktop ISO because a desktop loop underneath the suites
changes what they measure, and `make gate` boots the regression ISO.

- `SYS_WIN_*` / `SYS_SURFACE_*` / `SYS_GPU_*` **do not change**. The v0.96 paired
  buffers, `OUTRUN_WIN_BASE`/`WIN_BACK_V(id)`, `desk_chip_slot` hit testing, and
  the CPU-0 pinning that makes the compositor's lock-free surface read sound all
  stay exactly as they are.
- `desktop_run()` becomes a DRM client owning one CRTC. `fb_flip`'s scale
  magnification moves into the primary plane's src/crtc rectangles; the WM keeps
  working in logical coordinates (`desk_w()`/`desk_h()`), as CLAUDE.md requires.
- **One hit-test table, one geometry function.** A DRM plane path computing
  window rectangles independently of `desk_chip_slot` is the launcher version of
  the duplicated-hit-box trap already documented in this tree.
- `tools/desktop-ui-test.py` must pass **unchanged**, still sending `rel` events —
  the machine has PS/2 and no tablet, and absolute events are delivered nowhere.

## 4.4 Security hardening

### 4.4.1 SMEP / SMAP / UMIP

```c
#define CR4_UMIP  (1u << 11)
#define CR4_SMEP  (1u << 20)
#define CR4_SMAP  (1u << 21)

static inline void user_access_begin(void) { __asm__ volatile("stac" ::: "cc"); }
static inline void user_access_end  (void) { __asm__ volatile("clac" ::: "cc"); }

/* THE ONLY sanctioned way kernel code touches user memory. Introduce these
 * FIRST, convert every site, and set CR4.SMAP LAST — so the stac/clac bracket
 * lives in three functions rather than fifty. access_ok validates DIRECTION
 * and RANGE against USER_VMIN/USER_VMAX and the process's own mappings, not
 * merely "is it non-NULL". */
int64_t copy_from_user   (void *dst, const void *usrc, uint64_t n);
int64_t copy_to_user     (void *udst, const void *src, uint64_t n);
int64_t strncpy_from_user(char *dst, const char *usrc, uint64_t n);
int     access_ok        (const void *uaddr, uint64_t n, int write);
```

SMEP is one bit and nearly free. **SMAP is not.** It faults every kernel access
to a user address unless bracketed, and this kernel touches user memory in dozens
of places — `uargs_build`, the surface blit path, VFS copy loops, the WIMP damage
path, and every syscall that dereferences a pointer argument. One missed site is
a ring-0 `#PF` with SMAP as the only clue. Hence: convert first, enable second,
in separate commits, with the conversion commit gated green on its own.

### 4.4.2 seccomp

```c
/* Classic cBPF, deliberately NOT eBPF: a verifier for a Turing-incomplete
 * filter language is a few hundred auditable lines; an eBPF verifier is where
 * the CVEs live. */
struct sock_filter { uint16_t code; uint8_t jt, jf; uint32_t k; };

struct seccomp_data {
    int32_t  nr;
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t args[6];
};

struct seccomp_filter {
    volatile int32_t       refcount;
    uint32_t               len;
    struct seccomp_filter *prev;   /* filters STACK; most restrictive wins    */
    struct sock_filter     insn[];
};

#define SECCOMP_RET_KILL_PROCESS 0x80000000u
#define SECCOMP_RET_KILL_THREAD  0x00000000u
#define SECCOMP_RET_TRAP         0x00030000u
#define SECCOMP_RET_ERRNO        0x00050000u
#define SECCOMP_RET_ALLOW        0x7fff0000u
```

Installing a filter requires privilege **or** `no_new_privs`, and `no_new_privs`
is irreversible. Without that pairing an unprivileged process can filter a setuid
binary's syscalls and change what privileged code does — the classic seccomp
escalation. `no_new_privs` goes on `struct kproc` and is honoured by the existing
`suid`/`sgid` handling at exec.

### 4.4.3 CFI, KASLR, TPM

- **CFI:** `-fsanitize=cfi-icall` with LTO. This is testable under TCG and is the
  v2.0 claim. `-fcf-protection=full` emits `ENDBR64` for future hardware, but
  **QEMU/TCG does not emulate CET** — a CET build passing here proves nothing
  about CET, which is the "counter nothing increments" failure. Recorded as
  UNVERIFIED (§7.1).
- **Kernel KASLR:** requires `-fPIE` + `.rela.dyn` + a boot self-relocator, slide
  granularity 2 MiB over 1 GiB = **512 possible bases**. That is weak; say so
  rather than claiming "KASLR" flat. Widening needs the physmap, so it cannot
  precede Phase 1.
- **Userspace ASLR is cheaper and higher value**, but note that `va_is_forkable()`
  (:23340) and `WIN_BACK_V(id)` both encode fixed layout assumptions against
  `USTK_V`, `HEAP_USER_V`, `MMAP_USER_V`, `THR_USER_V`. Randomising those
  constants without re-expressing both against per-process bases reintroduces the
  trap CLAUDE.md already flags — one stride above a window's back buffer is
  another window's front buffer.
- **TPM 2.0:** PCR 0 firmware, 4 bootloader, 8 kernel, 9 boot modules,
  11 OutRun sealed-boot policy — the TCG PC Client allocation, because a
  measurement whose PCR index means something different from every other system
  cannot be attested against any existing policy. `module_load` extends PCR 9
  with the image hash **before** relocation begins; measuring after mapping
  measures a decision already taken. Prior art exists in this tree
  (`crypto/scrypt.c`, verified HMAC, `OUTRUN-0.87-sealed-boot.log`) and should be
  read before designing this. Testable via `-tpmdev emulator` + swtpm.

---

# 5. Syscall vector table — 119 through 146

**Verification requirement before implementing any of these:** grep both
`#define SYS_` in `metal/user/init.c` **and** the dispatch switch in
`kernel64.c`. The two can disagree, and CLAUDE.md documents the role-number
collision (roles 7 in `cmd_mcq` and `cmd_mcpre`) that cost v0.81 four boots in
eight. Syscall numbers are the same shared namespace. Highest currently assigned:
**118** (`SYS_DESKTOP_SETTINGS`).

| # | Name | Args (RDI, RSI, RDX) | Returns | Cap | Phase |
|---|---|---|---|---|---|
| 119 | `SYS_ORING_SETUP` | entries, `struct oring_params *`, 0 | ring fd | — | 1 |
| 120 | `SYS_ORING_ENTER` | ring_fd, to_submit, min_complete\|flags<<32 | n submitted | — | 1 |
| 121 | `SYS_ORING_REGISTER` | ring_fd, opcode, arg | 0 / −errno | — | 1 |
| 122 | `SYS_MODULE_LOAD` | image, size, args | 0 / −errno | `PCAP_MODULE` | 4 |
| 123 | `SYS_MODULE_UNLOAD` | name, 0, 0 | 0 / −errno | `PCAP_MODULE` | 4 |
| 124 | `SYS_MODULE_LIST` | out, n, 0 | count | — | 4 |
| 125 | `SYS_SECCOMP` | op, flags, arg | 0 / −errno | — | 4 |
| 126 | `SYS_PRCTL` | op, a, b | 0 / −errno | — | 4 |
| 127 | `SYS_ARCH_PRCTL` | code, addr, 0 | 0 / −errno | — | 3 |
| 128 | `SYS_OPENAT` | dirfd, path, flags\|mode<<32 | fd | — | 2 |
| 129 | `SYS_MKDIRAT` | dirfd, path, mode | 0 / −errno | — | 2 |
| 130 | `SYS_UNLINKAT` | dirfd, path, flags | 0 / −errno | — | 2 |
| 131 | `SYS_FSTATAT` | dirfd, path, `struct kstat *` | 0 / −errno | — | 2 |
| 132 | `SYS_GETDENTS64` | fd, buf, bytes | bytes / −errno | — | 2 |
| 133 | `SYS_CHDIR` | path, 0, 0 | 0 / −errno | — | 2 |
| 134 | `SYS_GETCWD` | buf, n, 0 | len / −errno | — | 2 |
| 135 | `SYS_MOUNT` | src, target, fstype\|flags<<32 | 0 / −errno | `PCAP_MOUNT` | 2 |
| 136 | `SYS_UMOUNT` | target, flags, 0 | 0 / −errno | `PCAP_MOUNT` | 2 |
| 137 | `SYS_IOCTL` | fd, cmd, arg | 0 / −errno | — | 3 |
| 138 | `SYS_READV` | fd, iov, iovcnt | bytes | — | 3 |
| 139 | `SYS_WRITEV` | fd, iov, iovcnt | bytes | — | 3 |
| 140 | `SYS_DUP3` | oldfd, newfd, flags | fd | — | 1 |
| 141 | `SYS_CLONE` | flags, stack, ptid | tid | — | 3 |
| 142 | `SYS_UNAME` | `struct utsname *`, 0, 0 | 0 | — | 3 |
| 143 | `SYS_MADVISE` | addr, len, advice | 0 / −errno | — | 3 |
| 144 | `SYS_SWAPON` | path, flags, 0 | 0 / −errno | `PCAP_MOUNT` | 3 |
| 145 | `SYS_SCHED_SETATTR` | pid, attr, flags | 0 / −errno | `PCAP_ADMIN`¹ | 1 |
| 146 | `SYS_SIGALTSTACK` | new, old, 0 | 0 / −errno | — | 3 |

¹ `PCAP_ADMIN` only when targeting a process other than self.

**Note on 140:** `SYS_DUP3` lands in Phase 1 with `files_struct`, not Phase 3 —
`dup2` semantics are what `SYS_SETREDIR` currently approximates, and the fd
rework is the right place to retire that special case.

## 5.1 New capability bits

Bits 0–13 are assigned (`PCAP_HW_PASSTHROUGH` … `PCAP_CONSOLE`; note
`PCAP_NETWORK` is bit 4 and is defined separately at kernel64.c:3190 with an
alias `PCAP_NET` at :12711 — **grep both** before reusing a bit).

```c
#define PCAP_MODULE  (1ull << 14)   /* SYS_MODULE_{LOAD,UNLOAD}                */
#define PCAP_MOUNT   (1ull << 15)   /* SYS_MOUNT / SYS_UMOUNT / SYS_SWAPON     */
#define PCAP_DRM     (1ull << 16)   /* DRM master: modeset                     */
#define PCAP_TPM     (1ull << 17)   /* PCR extend / seal / unseal              */
#define PCAP_ADMIN   (1ull << 18)   /* sched_setattr on another process        */
```

## 5.2 Async I/O ring ABI

Lives in `include/outrun_aio.h`, beside `outrun_abi.h`, under the same rule
CLAUDE.md states for `SYS_DESKTOP_INFO`: **the header is the master copy, the
kernel's local declaration moves with it, and the call refuses a size that
disagrees rather than partly filling the buffer.**

```c
#define ORING_ABI_VERSION 1

struct oring_sqe {
    uint8_t  opcode, flags;
    uint16_t ioprio;
    int32_t  fd;
    uint64_t off;                       /* file offset, or ~0 for current      */
    uint64_t addr;                      /* user buffer, or iovec array         */
    uint32_t len;                       /* bytes, or iovec count               */
    uint32_t rw_flags;
    uint64_t user_data;                 /* returned verbatim in the CQE        */
    uint64_t _pad[2];
};
_Static_assert(sizeof(struct oring_sqe) == 64, "SQE must stay 64 bytes");

struct oring_cqe { uint64_t user_data; int32_t res; uint32_t flags; };
_Static_assert(sizeof(struct oring_cqe) == 16, "CQE must stay 16 bytes");
```

**The ownership invariant, which is the whole basis of the lock-free ring:** the
kernel owns `sq.head` and `cq.tail`; userspace owns `sq.tail` and `cq.head`.
Neither side ever writes the other's index. A kernel that writes `sq.tail` "to
fix up" a malformed submission silently reintroduces the race — assert it, do
not merely document it.

Ring memory is allocated through the existing `SYS_SHM_CREATE`/`SYS_SHM_MAP`
(74/75), which are already refcounted and already marked `PTE_SHM` so COW never
touches them — exactly the semantics a submission ring needs.

**Test discipline:** a ring exercised only with `min_complete == to_submit`
never leaves the inline completion path and never tests the worker pool. The
suite must include a deep-queue case (submit 256, complete 1) asserting
`g_oring_deferred > 0` — a **detection** counter, not just a failure count.

---

# 6. Timing budget & lock-rank discipline under TCG

## 6.1 Measured harness parameters (current tree)

| Parameter | Default | Location |
|---|---|---|
| `GATE_CAP` | 900 s | `tools/gate-matrix.sh:75` |
| `GATE_DIRTY_CAP` | 480 s | `tools/gate-dirty.sh:46` |
| `GATE_DIRTY_BOOTS` | 3 | `Makefile` |
| `BOOT_CAP` / `ITER_CAP` | 1500 s / 900 s | `tools/vfs-soak.sh:53,54` |
| `GATE_CONFIGS` | `uniprocessor smp2-bios smp4-bios smp4-iommu` | `Makefile` |
| `GATE_OSR` | 4 | `Makefile` (oversub tier; own image + trailing `make clean`) |
| `GATE_KEEP` | 3 | `tools/gate-matrix.sh` run pruning |
| `APPSMP_TBASE` | 45000 ticks (450 s) | `init.c:3489` |
| `APPSMP_TSCALE` | 1 (4 for smp8 diagnostics) | `init.c:3517` |
| `R62_T`,`R64_T`,`R65_T`,`R66_T`,`CASC_T` | 6000 ticks = 60 s @ 100 Hz | `init.c:3626,4033,4282,4415,4614` |

`smp4-iommu` already needs `GATE_CAP=2400` on the reference host; at 900 s it is
cut off before the prompt and reports `TRUNCATED`.

## 6.2 The three perturbations, and the rule for each

Every budget above was calibrated under TCG and they are **not independent of
each other**. Three items in this plan move them:

| Item | Ver | Effect | Rule |
|---|---|---|---|
| Swap | v1.7 | A swapping boot is dramatically slower; `ITER_CAP` and `APPSMP_T` both become reachable on a slow host | Re-derive **in the same commit** that enables swap. |
| `cpuidle` | v2.0-b | Changes how long a core spends not executing | Ships **off**; re-baseline only as a deliberate, separate, recorded decision. |
| SMAP | v2.0-d | Adds `stac`/`clac` to every user copy — measurable on copy-heavy suites | Measure the conversion commit and the enable commit separately. |

**Standing rule:** do not adjust budgets incrementally as failures appear. That
is how a budget stops expiring on a genuine stall, and it is the exact failure
mode CLAUDE.md warns about for the KVM switch — tests that pass for the wrong
reason. Re-derive the whole set at once, from measurement, in one commit, with
the new baseline recorded the way v0.91 recorded the TCG one.

`APPSMP_T` has an additional hard constraint already documented in the tree: it
**must stay below `APPSMP_WATCH_FOR`**, the kernel-side phase watchdog, or the
phase gives up first and the per-worker exit code — the thing that distinguishes
"ran out of time" from "broke a rule" — is never collected.

## 6.3 Deadlines, not iteration counts — restated for the new subsystems

`SYS_WAITPID` is non-blocking, so a ring-3 "timeout" expressed as a spin count
budgets the *waiter's iterations*, not time, and means different durations at
1 vCPU and at 4. Every new wait in this plan is a deadline:

- NVMe `CSTS.RDY` polling → `NVME_CAP_TO * 500 ms` against `ktime_get_us()`.
- `struct request::deadline_ns` → absolute, not a retry count.
- `kswapd` writeback waits → deadline.
- ACPICA `AcpiOsWaitSemaphore` → deadline.
- Ring-3 suite workers → `owaitpid_ticks()`; `SYS_SYSINFO` reports `g_ticks` at
  100 Hz. **Deadline expiry keeps its own distinct exit code** so a slow host is
  never decoded as a defect.

## 6.4 Lock-rank discipline for the new call stacks

The deepest legal stack after this plan:

```
files(3) -> vfs(4) -> cas(5) -> blk(16) -> zone(0)     ILLEGAL — 16 then 0 is a drop
```

That is exactly why zone and swap take ranks **0 and 1** rather than being
appended at the top: allocation is reachable from under almost every other lock,
and acquisition must be upward. The legal deep stack is:

```
zone(0) ... no
files(3) -> vfs(4) -> cas(5) -> vblk(6) -> blk(16)     legal, depth 5
files(3) -> vfs(4) -> cas(5) -> blk(16) -> acpi(17)    legal, depth 5
```

with an allocation at any point taking rank 0 — which is a **drop**, and
therefore forbidden. **The consequence, and it is a design constraint on every
new subsystem in this document: no code may allocate while holding a ranked
lock.** Pre-allocate before acquisition, or use a per-CPU reserve. `g_frame_lock`
being deliberately unranked is why the current tree gets away with this; the
buddy allocator being ranked is what makes it explicit.

Depth 16 accommodates a block completion running above a VFS stack plus the
module and ACPI reserves. The existing depth-8 stack overflows there, and an
overflowed rank stack reports inversions that are artefacts — which is the same
class of false signal as a partially-applied rank shift.

---

# 7. Gate matrix specification

## 7.1 Non-goals — explicit scope boundaries

A gate whose gaps are invisible is how "verified" drifts from "measured". These
are deliberate exclusions and the v2.0 release notes must repeat them verbatim.

| Non-goal | Why | Earliest |
|---|---|---|
| **GCC self-hosting (kernel compiles itself)** | `cc1` needs ~1 GiB RSS for one large TU, and `kernel64.c` is 2 MB in a single file. Needs ≥8 GiB, `make -j`, and hours of uptime. **`tcc` (S6) is the v2.0 claim.** | v2.1+ |
| **Bare-metal NUMA** | Testable here only against QEMU's synthetic `-numa` topology. Real node-distance behaviour, memory interleaving and cross-socket latency are unobservable under TCG on a single-node host. `gate-numa` proves the code path executes, **not** that the policy is correct on hardware. | needs hardware |
| **Intel CET (IBT + shadow stack)** | **TCG does not emulate CET.** A CET build passing here proves nothing — the enforcement never happens, so the test cannot fail. `-fcf-protection=full` emits the encodings; enforcement is recorded UNVERIFIED. Software CFI (`-fsanitize=cfi-icall`) is the testable v2.0 claim. | needs KVM or metal |
| **ACPI S3/S4 deep sleep** | Requires `_PTS`/`_GTS`, a suspend/resume callback chain on **every** driver, and a FACS wake vector. S5 poweroff and the power-button GPE ship in v2.0; suspend does not. | v2.1+ |
| **ACPI C/P-states enabled by default** | Perturbs every TCG-calibrated budget (§6.2). Ships behind `cpuidle=on`, default off, gate runs with it off and says so. | needs re-baseline |
| **Full reverse mapping (rmap)** | Post-fork shared anonymous COW pages held by two processes are **not reclaimable** in v2.0. A real limitation, stated, not an omission. | v2.1+ |
| **virtio-gpu 3D / VIRGL / Mesa** | Needs a userspace GL driver and a Mesa port. DRM/KMS in v2.0 is modesetting and dumb buffers only. | v2.1+ |
| **eBPF** | seccomp is classic cBPF with a small auditable verifier, by choice. An eBPF verifier is a larger attack surface than the feature justifies here. | not planned |
| **SMP-safe module unload under load** | v2.0 quiesces before unload; concurrent unload against an in-flight call needs RCU-style deferral that does not exist. | v2.1+ |
| **KVM acceleration** | Would invalidate every budget in §6.1 simultaneously. `/dev/kvm` exists on the host; the build user is deliberately not in the `kvm` group. Changing this is a re-baseline, not a speed fix. | deliberate policy |

## 7.2 New gate tiers

| Tier | QEMU configuration | Asserts | Cap |
|---|---|---|---|
| `gate-mem4g` | `-m 4G` | Allocates 2 GiB, touches every page, verifies contents. `g_pages_above_1g > 0`. A physmap-disabled build must fail. | `GATE_CAP=2400` |
| `gate-nvme` | `-device nvme,drive=nvm,serial=OUTRUN01` + `-drive if=none` | Driver-level read/write/flush vs a known pattern. Submits `depth+8` commands; asserts `g_nvme_phase_wraps > 0` **and** `g_nvme_failures == 0`. A `CREATE_SQ`-removed build must go red. | `GATE_CAP=2400` |
| `gate-vfs` | default `-smp 4` | Mounts a pre-v1.6 CAS volume and reads every file byte-identically; `mkdir -p a/b/c/d`; `getdents64`; `/proc/self/maps` well-formed; `I_TIME_APPROX` set on the legacy volume. | `GATE_CAP=2400` |
| `gate-swap` | `-m 512M` with a 2 GiB working set | Workload completes correctly; `g_swap_out > 0` **and** `g_swap_in > 0` (detections, not just failures). | `GATE_CAP=2400` |
| `gate-selfhost` | `-m 4G -smp 4` | In-guest `make` + `tcc` build; output checksum matches a host build of identical source. | `GATE_CAP=3600` |
| `gate-numa` | `-numa node,cpus=0-1 -numa node,cpus=2-3 -m 4G` | Same-node steal preference exercised; `g_steals_local`/`g_steals_remote` both non-zero. Coverage line must state this is synthetic topology. | `GATE_CAP=2400` |
| `gate-tpm` | `-tpmdev emulator,id=tpm0 -device tpm-tis` + swtpm | PCR 8/9 match independently recomputed SHA-256 of kernel and modules. | `GATE_CAP=2400` |

## 7.3 Harness changes required

1. **`GATE_CAP` default rises to 2400.** `smp4-iommu` already needs it, and every
   new tier exceeds 900 s under TCG. `TRUNCATED` is not a verdict, and a tier
   that is routinely truncated is a tier that tests nothing however green its
   printed assertions look.
2. **`GATE_CONFIGS` extends** to include `gate-mem4g`, `gate-nvme`, `gate-vfs`,
   `gate-swap`, `gate-numa` as they land — one per phase, never all at once, so a
   regression is attributable.
3. **`gate-selfhost` runs separately from `make gate`.** At `GATE_CAP=3600` it is
   longer than the rest of the matrix combined; putting it inline means a
   fresh-image break waits behind an hour of compilation, which violates the
   "fails cheapest first" ordering `gate-all` already implements.
4. **The coverage line must name every tier NOT run**, including `cpuidle=off`,
   CET-unverified, and single-node NUMA. This is part of the output, not a
   footnote.
5. **The classifier is untouched.** `OK|FAIL|RANK-FAULT` remain the only verdict
   statuses; `TRUNCATED`, `NO-PROMPT`, `NO-SUITES`, `COUNTER-SPLIT` remain
   invalid runs. The dual failure counters (line-regex `FAILRE` vs the suites'
   own `RESULT:` tally) and the disagreement-fails-the-gate rule stay exactly as
   they are — every new suite must emit a conforming `RESULT: N passed, M failed`
   line or it is invisible to one of the two counters, which trips
   `COUNTER-SPLIT` and correctly refuses to certify the boot.
6. **Every new harness stamps the image md5 into every log it writes.** Twice in
   this project a run was found to have booted a different image than it claimed.
   A log that cannot name the binary it came from is not evidence.
7. **`gate-selftest` runs first**, unchanged, as `make gate` already does.

## 7.4 Release protocol — unchanged and mandatory

Every phase tag follows the existing four steps without exception: bump `VERSION`
in `metal/Makefile` **before** tagging; `make release-iso` from a clean tree;
`make release-verify` on the exact published image; publish MD5 and SHA-256 in
the release notes. `v0.75.0` was tagged while `VERSION` still read `0.74.0` and
every artefact for that release carried the wrong name — the protocol exists to
make that class of mistake impossible to complete silently.

---

# 8. Effort and risk summary

| Phase | Kernel LoC | Ported LoC | Dominant risk |
|---|---|---|---|
| 1 (v1.1–1.3) | ~6,000 | — | Rank-shift breadth; untranslated physical dereferences after the ceiling lift |
| 2 (v1.4–1.6) | ~11,000 | — | VFS migration without breaking on-disk CAS compatibility; NVMe phase-tag and granted-queue-count correctness |
| 3 (v1.7–1.9) | ~4,000 | ~90,000 (musl, busybox, tcc) | `%fs`/TLS across the ring boundary and across core migration; relocation ordering |
| 4 (v2.0) | ~9,000 | ~60,000 (ACPICA) | SMAP conversion completeness; DRM/desktop coexistence without a second hit-test table |

Kernel total ≈ **30,000 lines** on top of today's 37,313: the kernel roughly
doubles. That is the honest scale of structural parity, and it is the figure
against which any shorter plan should be judged.

## 8.1 The five things most likely to go wrong

1. **A half-applied rank shift** reporting artefact inversions, prompting a "fix"
   to a correct acquisition order. Mitigation: one commit, `gate-selftest` plus
   full `gate-all` on both sides.
2. **An untranslated physical dereference** surviving into v1.2, faulting only
   when the allocator happens to return a frame above 1 GiB — intermittent by
   construction. Mitigation: v1.1 as a behaviour-identical negative control.
3. **NVMe queue count** taken as requested rather than granted, producing MMIO
   writes into an undecoded region. Mitigation: read the CQE `result`; assert
   `nr_io_queues <= granted`.
4. **A missed `FS_BASE` restore** presenting as random corruption rather than a
   scheduling bug. Mitigation: the dedicated cross-core TLS migration assertion,
   verified by a restore-removed build.
5. **A budget adjusted incrementally** until a genuine stall sits inside it.
   Mitigation: §6.2's re-derive-all-at-once rule, one commit, recorded.
