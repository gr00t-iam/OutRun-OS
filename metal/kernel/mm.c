/* ===========================================================================
 * OutRun OS — mm.c: struct page, zones, buddy allocator, and the physmap
 * ===========================================================================
 * v1.1. Included into kernel64.c as a translation-unit fragment (the kernel is
 * one TU by convention; this file exists so the memory layer is one subsystem
 * in one place rather than another 500 lines threaded through 37,000).
 *
 * WHAT THIS IS, AND WHAT IT IS NOT YET.
 *
 * v1.1 introduces the DATA STRUCTURES and the HIGHER-HALF MAP, and it keeps
 * every existing allocator contract intact:
 *
 *   - alloc_frame()/alloc_frames()/free_frame() still return and accept
 *     PHYSICAL addresses that callers dereference directly, because the low
 *     1 GiB is still identity-mapped and IDENT_MAP_LIMIT is still enforced.
 *   - frame_share()/frame_refs() now read and write struct page::refcount
 *     instead of the old g_frame_ref[] side table, and the double-free shadow
 *     bit is struct page::flags & PG_FREE instead of g_frame_dbg_isfree[].
 *     Both old arrays are gone. Same semantics, one source of truth.
 *   - The buddy allocator is BUILT and EXERCISED (mm_selftest) but is not yet
 *     the general allocator: alloc_frame() still draws from the v0.42 LIFO
 *     list and the bump pointer. That makes v1.1 bit-for-bit behaviour-
 *     identical to v1.0 on every existing path, which is what makes it the
 *     NEGATIVE CONTROL for v1.2 — the release that lifts the ceiling and
 *     converts the 79 alloc_frame() sites to PHYS_TO_VIRT. If v1.2 breaks, a
 *     green v1.1 says the breakage is the lift, not the restructure.
 *
 * The physmap is built HERE, at boot, from the multiboot2 memory map that
 * multiboot_scan has parsed and printed since v0.1 and never used for sizing.
 * It maps every usable-RAM range at PHYSMAP_BASE + pa with 2 MiB pages, so
 * PHYS_TO_VIRT() is valid for ALL host RAM — that is the capability the 1 GiB
 * ceiling has been standing in for, and it exists from this release onward
 * even though nothing above 1 GiB is handed out until v1.2.
 *
 * LOCK RANK. g_zone_lock is a klock at rank 0 — the rank the +2 shift vacated.
 * It is the FIRST ranked lock beneath every other, and the consequence is the
 * allocation invariant stated at the rank table: NO CODE MAY TAKE g_zone_lock
 * WHILE HOLDING ANY RANKED LOCK. In v1.1 nothing does, because nothing on a
 * ranked path calls the buddy allocator yet. The invariant is stated now so it
 * governs v1.2's conversion, not discovered by it.
 *
 * g_frame_lock stays the UNRANKED raw spinlock it has always been, for the
 * reasons its own comment gives (pre-scheduler, APs, never nested). The buddy
 * allocator is NOT a replacement for it in v1.1; it is a second allocator that
 * v1.2 will make the first one delegate to.
 * =========================================================================== */

/* ---- The physmap ---------------------------------------------------------- */

/* -120 TiB, the Linux convention, chosen for one reason: PML4 slot 0x111 is
 * far from every window this kernel already uses (0 = identity, 0x80..0xBF =
 * user, 0xC0 = MMIO, 0xAA = WIN), so it cannot collide with any of them and
 * cannot be aliased into a process by accident — create_address_space copies
 * exactly two PML4 entries by index and this is neither. */
#define PHYSMAP_BASE      0xFFFF888000000000ull
#define PHYSMAP_PML4_IDX  ((PHYSMAP_BASE >> 39) & 0x1FF)            /* 0x111 */
#define PHYS_TO_VIRT(pa)  ((void *)((uint64_t)(pa) + PHYSMAP_BASE))
#define VIRT_TO_PHYS(va)  ((uint64_t)(va) - PHYSMAP_BASE)

_Static_assert(PHYSMAP_PML4_IDX != 0 && PHYSMAP_PML4_IDX != 0xC0,
               "physmap PML4 slot collides with the identity or MMIO window");

/* ---- struct page ---------------------------------------------------------- */

#define MAX_ORDER   11                    /* 4 KiB .. 4 MiB contiguous          */

/* One per 4 KiB frame of usable RAM, indexed by PFN. Sized to exactly 32
 * bytes so g_mem_map is 8 MiB per GiB of RAM: at 16 GiB that is 128 MiB of
 * metadata, which is the price of being able to reclaim. Kept small on
 * purpose — every field here is one that either the buddy allocator, the COW
 * fault path, or Phase 3's reclaim needs, and nothing else. */
struct page {
    uint32_t          flags;              /* PG_* below                         */
    uint32_t          order;              /* buddy order when PG_FREE           */
    volatile uint16_t refcount;           /* EXTRA owners; 0 = sole. Same
                                           * meaning as the old g_frame_ref[]
                                           * so frame_refs()'s callers — the
                                           * COW fault path — need no change    */
    uint16_t          node;
    uint32_t          _pad0;
    struct page      *next;               /* buddy free-list chain              */
    uint64_t          private;            /* Phase 3: (ino<<32)|pgidx, or slot  */
};
_Static_assert(sizeof(struct page) == 32, "struct page must stay 32 bytes");

#define PG_FREE        (1u << 0)          /* on a buddy free list               */
#define PG_RESERVED    (1u << 1)          /* kernel image, tables, holes        */
#define PG_LRU         (1u << 2)          /* Phase 3                            */
#define PG_ACTIVE      (1u << 3)
#define PG_REFERENCED  (1u << 4)
#define PG_DIRTY       (1u << 5)
#define PG_WRITEBACK   (1u << 6)
#define PG_ANON        (1u << 7)
#define PG_PINNED      (1u << 8)          /* DMA / IOMMU target: never evict    */
#define PG_TABLE       (1u << 9)          /* a page-table page                  */
#define PG_LEGACY      (1u << 10)         /* v1.1: owned by the v0.42 LIFO pool,
                                           * NOT the buddy — see mm_frame_of()  */

struct zone {
    uint64_t        base_pfn, nr_pages;
    struct page    *free_area[MAX_ORDER];
    uint32_t        free_count[MAX_ORDER];
    uint64_t        nr_free;
    uint32_t        node;
};

#define MM_MAX_RANGES 32
struct mm_range { uint64_t base, len; };

static struct page     *g_mem_map      = 0;   /* PFN-indexed                   */
static uint64_t         g_max_pfn      = 0;   /* one past the highest usable   */
static uint64_t         g_mem_map_pa   = 0;   /* where g_mem_map lives (phys)  */
static uint64_t         g_mem_map_pages= 0;
static struct zone      g_zone;               /* v1.1: ONE zone. NUMA is v1.2+ */
static struct mm_range  g_usable[MM_MAX_RANGES];
static uint32_t         g_n_usable     = 0;
static uint64_t         g_physmap_mapped = 0; /* bytes mapped at PHYSMAP_BASE  */
static int              g_physmap_ready  = 0;

/* ---- Forward declarations for the implementation half ------------------- */
/* Defined in mm_impl.c, which is #included after klock exists (~line 4400).
 * mm_note_usable is called from multiboot_scan and is klock-free, so it lives
 * here; everything that takes g_zone_lock lives there. */
static struct page *alloc_pages(uint32_t order, uint32_t gfp);
static void         free_pages(struct page *p, uint32_t order);
static void         mm_init(void);
static void         mm_selftest(void);

/* Counters. Printed by mm_selftest and the `mem` shell command, because a
 * counter nothing prints is not instrumentation. */
static volatile uint64_t g_buddy_allocs   = 0;
static volatile uint64_t g_buddy_frees    = 0;
static volatile uint64_t g_buddy_splits   = 0;
static volatile uint64_t g_buddy_merges   = 0;
static volatile uint64_t g_pages_above_1g = 0;   /* v1.2's gate-mem4g detector */

static inline struct page *pfn_to_page(uint64_t pfn) {
    return (pfn < g_max_pfn) ? &g_mem_map[pfn] : 0;
}
static inline uint64_t page_to_pfn(const struct page *p) {
    return (uint64_t)(p - g_mem_map);
}
static inline uint64_t page_to_phys(const struct page *p) {
    return page_to_pfn(p) << 12;
}
static inline struct page *phys_to_page(uint64_t pa) {
    return pfn_to_page((pa & ADDR_MASK) >> 12);
}

/* Called from multiboot_scan's type-6 loop, once per usable-RAM entry. The
 * scan already runs before kernel_cr3 is captured, so we only RECORD here and
 * build in mm_init(), which kmain calls after kernel_cr3 is known. */
static void mm_note_usable(uint64_t base, uint64_t len) {
    if (g_n_usable >= MM_MAX_RANGES) return;
    /* Page-align inward: a partial page at either end is not a frame. */
    uint64_t b = (base + 0xFFFull) & ~0xFFFull;
    uint64_t e = (base + len) & ~0xFFFull;
    if (e <= b) return;
    g_usable[g_n_usable].base = b;
    g_usable[g_n_usable].len  = e - b;
    g_n_usable++;
    if (e > (g_max_pfn << 12)) g_max_pfn = e >> 12;
}

/* ---- Legacy-pool accessors ------------------------------------------------ */

/* The v0.42 pool indexes frames as (pa - FRAME_POOL_BASE) / 4096; struct page
 * indexes by PFN. The seam converts. Before g_mem_map exists (the first few
 * allocations at boot, all of them page tables for the physmap itself) these
 * read as "not free, sole owner" — exactly what a zero-initialised legacy
 * array said — so nothing early-boot can trip a check that has no storage yet.
 *
 * There is a real behavioural difference and it is an IMPROVEMENT: the old
 * arrays were sized to FRAME_DBG_MAX (256 MiB) and silently stopped tracking
 * frames past that. struct page covers all of RAM, so refcounts and the
 * double-free shadow bit now work on every frame the pool can hand out. The
 * FRAME_DBG_MAX bound at the call sites is kept for one release so the seam
 * is a pure move; v1.2 removes it. */
static inline uint64_t frame_legacy_pfn(uint64_t idx) {
    return (FRAME_POOL_BASE >> 12) + idx;
}
static inline int frame_shadow_get(uint64_t idx) {
    struct page *p = g_mem_map ? pfn_to_page(frame_legacy_pfn(idx)) : 0;
    return p ? ((p->flags & PG_FREE) != 0) : 0;
}
static inline void frame_shadow_set(uint64_t idx, int v) {
    struct page *p = g_mem_map ? pfn_to_page(frame_legacy_pfn(idx)) : 0;
    if (!p) return;
    if (v) p->flags |= PG_FREE; else p->flags &= ~PG_FREE;
}
static inline uint16_t frame_refc_get(uint64_t idx) {
    struct page *p = g_mem_map ? pfn_to_page(frame_legacy_pfn(idx)) : 0;
    return p ? p->refcount : 0;
}
static inline void frame_refc_add(uint64_t idx, int delta) {
    struct page *p = g_mem_map ? pfn_to_page(frame_legacy_pfn(idx)) : 0;
    if (p) p->refcount = (uint16_t)((int)p->refcount + delta);
}

