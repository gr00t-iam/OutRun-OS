/* ===========================================================================
 * OutRun OS — mm_impl.c: the buddy allocator, physmap build, and self-test
 * ===========================================================================
 * The second half of kernel/mm.c, #included AFTER struct klock and
 * klock_acquire/klock_release exist in the TU, because g_zone_lock is a klock
 * at rank 0 and the allocator takes it. The types, the seam accessors and
 * mm_note_usable live in mm.c, which is included early enough for the frame
 * allocator's own call sites to see them. See mm.c for the design notes.
 * =========================================================================== */

/* RANK 0: the rank the +2 shift vacated. A klock, not a raw spin, because the
 * buddy allocator is only ever called from scheduled context and never from an
 * IRQ or an AP with no task — those keep using g_frame_lock. */
static struct klock g_zone_lock = { 0, "zone", 0, 0, 0, 0, 0, 0 KLOCK_RW_INIT KLOCK_DBG_INIT };

/* ---- Physmap construction ------------------------------------------------- */

/* Map [base, base+len) at PHYSMAP_BASE+base with 2 MiB pages, creating PDPT
 * and PD tables as needed. Runs BEFORE the buddy exists, so it draws table
 * pages from the bump allocator (alloc_frames), which is identity-mapped and
 * safe here. Tables are marked PG_TABLE|PG_RESERVED once g_mem_map exists.
 *
 * Ranges are rounded OUT to 2 MiB: mapping a little past the end of a usable
 * range is harmless (the frames are never handed out) and mapping short would
 * leave a usable frame unreachable, which is the bug this map exists to end. */
static void physmap_map_range(uint64_t base, uint64_t len) {
    uint64_t *pml4 = (uint64_t *)(kernel_cr3 & ADDR_MASK);
    uint64_t start = base & ~0x1FFFFFull;
    uint64_t end   = (base + len + 0x1FFFFFull) & ~0x1FFFFFull;
    for (uint64_t pa = start; pa < end; pa += 0x200000ull) {
        uint64_t va = PHYSMAP_BASE + pa;
        uint64_t i4 = (va >> 39) & 0x1FF, i3 = (va >> 30) & 0x1FF, i2 = (va >> 21) & 0x1FF;
        if (!(pml4[i4] & PTE_PRESENT)) {
            uint64_t t = alloc_frames(1);
            pml4[i4] = t | PTE_PRESENT | PTE_WRITE;
        }
        uint64_t *pdpt = (uint64_t *)(pml4[i4] & ADDR_MASK);
        if (!(pdpt[i3] & PTE_PRESENT)) {
            uint64_t t = alloc_frames(1);
            pdpt[i3] = t | PTE_PRESENT | PTE_WRITE;
        }
        uint64_t *pd = (uint64_t *)(pdpt[i3] & ADDR_MASK);
        if (!(pd[i2] & PTE_PRESENT)) {
            /* 2 MiB leaf. NX: nothing executes out of the physmap, ever. */
            pd[i2] = pa | PTE_PRESENT | PTE_WRITE | PTE_HUGE | PTE_NX;
            g_physmap_mapped += 0x200000ull;
        }
    }
}

/* ---- Buddy allocator ------------------------------------------------------ */

static inline int mm_page_is_free_order(struct page *p, uint32_t order) {
    return (p->flags & PG_FREE) && p->order == order;
}

/* Insert a free block of `order` at `pfn`, coalescing upward with its buddy
 * while the buddy is also free at the same order. Caller holds g_zone_lock. */
static void buddy_free_locked(struct zone *z, uint64_t pfn, uint32_t order) {
    while (order < MAX_ORDER - 1) {
        uint64_t bpfn = pfn ^ (1ull << order);
        if (bpfn < z->base_pfn || bpfn >= z->base_pfn + z->nr_pages) break;
        struct page *b = &g_mem_map[bpfn];
        if (!mm_page_is_free_order(b, order)) break;
        /* Unlink the buddy from its free list. Singly linked, so walk. */
        struct page **pp = &z->free_area[order];
        while (*pp && *pp != b) pp = &(*pp)->next;
        if (!*pp) break;                              /* not on the list: inconsistent; stop */
        *pp = b->next;
        z->free_count[order]--;
        z->nr_free -= 1ull << order;                  /* the buddy leaves the free pool
                                                       * as a separate block; it comes
                                                       * back below inside the merged one */
        b->flags &= ~PG_FREE; b->order = 0; b->next = 0;
        __sync_fetch_and_add(&g_buddy_merges, 1);
        if (bpfn < pfn) pfn = bpfn;
        order++;
    }
    struct page *p = &g_mem_map[pfn];
    p->flags |= PG_FREE; p->order = order;
    p->next = z->free_area[order];
    z->free_area[order] = p;
    z->free_count[order]++;
    z->nr_free += 1ull << order;
}

/* Take a block of `order`, splitting a larger one if needed. Returns PFN or
 * ~0. Caller holds g_zone_lock. */
static uint64_t buddy_alloc_locked(struct zone *z, uint32_t order) {
    for (uint32_t o = order; o < MAX_ORDER; o++) {
        struct page *p = z->free_area[o];
        if (!p) continue;
        z->free_area[o] = p->next;
        z->free_count[o]--;
        p->flags &= ~PG_FREE; p->next = 0;
        uint64_t pfn = page_to_pfn(p);
        /* Split back down, returning the upper halves to their lists. */
        while (o > order) {
            o--;
            uint64_t half = pfn + (1ull << o);
            struct page *h = &g_mem_map[half];
            h->flags |= PG_FREE; h->order = o;
            h->next = z->free_area[o]; z->free_area[o] = h;
            z->free_count[o]++;
            __sync_fetch_and_add(&g_buddy_splits, 1);
        }
        p->order = 0;
        z->nr_free -= 1ull << order;
        return pfn;
    }
    return ~0ull;
}

#define GFP_KERNEL   0x01u
#define GFP_ATOMIC   0x02u
#define GFP_DMA32    0x04u
#define GFP_ZERO     0x08u

/* Public interface. Returns a struct page*, never a raw address — a caller
 * that wants to touch the memory goes through PHYS_TO_VIRT(page_to_phys(p)),
 * which is the discipline that makes frames above 1 GiB usable at all. */
static struct page *alloc_pages(uint32_t order, uint32_t gfp) {
    if (order >= MAX_ORDER || !g_physmap_ready) return 0;
    klock_acquire(&g_zone_lock);
    uint64_t pfn = buddy_alloc_locked(&g_zone, order);
    klock_release(&g_zone_lock);
    if (pfn == ~0ull) return 0;
    struct page *p = &g_mem_map[pfn];
    p->refcount = 0;
    __sync_fetch_and_add(&g_buddy_allocs, 1);
    if ((pfn << 12) >= IDENT_MAP_LIMIT) __sync_fetch_and_add(&g_pages_above_1g, 1);
    if (gfp & GFP_ZERO) {
        uint64_t *v = (uint64_t *)PHYS_TO_VIRT(pfn << 12);
        for (uint64_t i = 0; i < (512ull << order); i++) v[i] = 0;
    }
    return p;
}

static void free_pages(struct page *p, uint32_t order) {
    if (!p || order >= MAX_ORDER) return;
    uint64_t pfn = page_to_pfn(p);
    /* Legacy check FIRST. PG_FREE doubles as the legacy pool's double-free
     * shadow bit, so a legacy frame can legitimately carry it; testing for
     * PG_FREE before PG_LEGACY would misreport a misrouted free as a double
     * free and name the wrong bug. */
    if (p->flags & PG_LEGACY) {
        kprintf("\n[mm     ] MISROUTED FREE: pfn %X belongs to the legacy pool, not the buddy -- halting\n", pfn);
        for (;;) __asm__ volatile("cli; hlt");
    }
    if (p->flags & PG_FREE) {
        kprintf("\n[mm     ] DOUBLE-FREE: pfn %X (order %u) is already free -- halting\n",
                pfn, (uint64_t)order);
        for (;;) __asm__ volatile("cli; hlt");
    }
    klock_acquire(&g_zone_lock);
    buddy_free_locked(&g_zone, pfn, order);
    klock_release(&g_zone_lock);
    __sync_fetch_and_add(&g_buddy_frees, 1);
}

/* ---- Initialisation ------------------------------------------------------- */

/* v1.1 OWNERSHIP SPLIT. Two allocators coexist and must never hand out the
 * same frame:
 *
 *   [FRAME_POOL_BASE, IDENT_MAP_LIMIT)  -> the legacy LIFO/bump pool, marked
 *                                         PG_LEGACY|PG_RESERVED so the buddy
 *                                         never sees it as free.
 *   everything else usable              -> the buddy.
 *
 * In v1.1 the buddy therefore owns exactly the RAM above 1 GiB (nothing, on a
 * 512 MiB guest; ~3 GiB on `-m 4G`), and every existing path keeps its pool.
 * v1.2 shrinks the legacy window to nothing. */
static void mm_init(void) {
    if (!g_n_usable || !g_max_pfn) {
        kputs("[mm     ] no usable-RAM ranges recorded; physmap not built\n");
        return;
    }
    /* 1. Physmap every usable range. Table pages come from the bump pool. */
    for (uint32_t i = 0; i < g_n_usable; i++)
        physmap_map_range(g_usable[i].base, g_usable[i].len);
    write_cr3(kernel_cr3);                                    /* flush */
    g_physmap_ready = 1;

    /* 2. g_mem_map: one struct page per PFN up to g_max_pfn. Allocated from
     *    the bump pool (identity-mapped, so its address is usable either way)
     *    and accessed through the physmap from here on, as a first consumer. */
    uint64_t bytes = g_max_pfn * sizeof(struct page);
    g_mem_map_pages = (bytes + 0xFFFull) >> 12;
    g_mem_map_pa    = alloc_frames(g_mem_map_pages);          /* zeroed */
    g_mem_map       = (struct page *)PHYS_TO_VIRT(g_mem_map_pa);

    /* 3. Everything is RESERVED until proven usable. Then release usable
     *    ranges to the buddy — except the legacy window, which is marked
     *    PG_LEGACY and stays with the old pool. */
    for (uint64_t pfn = 0; pfn < g_max_pfn; pfn++) g_mem_map[pfn].flags = PG_RESERVED;

    g_zone.base_pfn = 0; g_zone.nr_pages = g_max_pfn; g_zone.node = 0;
    uint64_t released = 0, legacy = 0;
    for (uint32_t i = 0; i < g_n_usable; i++) {
        uint64_t b = g_usable[i].base >> 12, e = (g_usable[i].base + g_usable[i].len) >> 12;
        for (uint64_t pfn = b; pfn < e; pfn++) {
            uint64_t pa = pfn << 12;
            if (pa >= FRAME_POOL_BASE && pa < IDENT_MAP_LIMIT) {
                g_mem_map[pfn].flags = PG_RESERVED | PG_LEGACY; legacy++; continue;
            }
            if (pa < FRAME_POOL_BASE) continue;              /* kernel, tables, low mem: reserved */
            /* the mem_map itself and the bump pool's high-water are below
             * IDENT_MAP_LIMIT and so already excluded above. */
            g_mem_map[pfn].flags = 0;
            buddy_free_locked(&g_zone, pfn, 0);
            released++;
        }
    }
    kprintf("[mm     ] physmap: %M MiB mapped at %X (2 MiB pages, NX); mem_map %u pages @ %X\n",
            g_physmap_mapped, (uint64_t)PHYSMAP_BASE, g_mem_map_pages, g_mem_map_pa);
    kprintf("[mm     ] buddy: %u frames released above 1 GiB, %u legacy frames kept by the v0.42 pool\n",
            released, legacy);
}

/* ---- Self-test ------------------------------------------------------------ */

/* Exercises the buddy through the physmap: allocate, write through
 * PHYS_TO_VIRT, read back, free, confirm coalescing. On a 512 MiB guest the
 * buddy owns nothing and this reports that honestly rather than passing
 * vacuously — a test that cannot fail has not passed. The gate-mem4g tier
 * (v1.2) is where this becomes load-bearing. Emits a RESULT: line so the
 * harness's two failure counters both see it. */
static void mm_selftest(void) {
    int pass = 0, fail = 0;
#define mmcheck(msg, cond) do { if (cond) { pass++; kprintf("[mm     ] ok   %s\n", msg); } \
                                else      { fail++; kprintf("[mm     ] FAIL: %s\n", msg); } } while (0)
    mmcheck("physmap built", g_physmap_ready);
    mmcheck("mem_map allocated", g_mem_map != 0);
    /* The identity window and the physmap must agree on a known frame. */
    if (g_physmap_ready) {
        volatile uint64_t *id = (volatile uint64_t *)kernel_cr3;
        volatile uint64_t *pm = (volatile uint64_t *)PHYS_TO_VIRT(kernel_cr3);
        mmcheck("physmap aliases the identity map (PML4[0] identical via both)", id[0] == pm[0]);
    }
    if (g_zone.nr_free == 0) {
        kprintf("[mm     ] buddy owns 0 frames on this guest (RAM <= 1 GiB): allocation checks NOT RUN\n");
    } else {
        struct page *a = alloc_pages(0, GFP_ZERO);
        struct page *b = alloc_pages(3, GFP_ZERO);       /* 32 KiB */
        mmcheck("order-0 allocation succeeds", a != 0);
        mmcheck("order-3 allocation succeeds", b != 0);
        if (a && b) {
            uint64_t *va = (uint64_t *)PHYS_TO_VIRT(page_to_phys(a));
            uint64_t *vb = (uint64_t *)PHYS_TO_VIRT(page_to_phys(b));
            va[0] = 0xA110CA7EDull; vb[4095] = 0xB0DDEull;
            mmcheck("write through physmap reads back (order 0)", va[0] == 0xA110CA7EDull);
            mmcheck("write through physmap reads back (order 3, last word)", vb[4095] == 0xB0DDEull);
            mmcheck("order-3 block is 32 KiB aligned", (page_to_phys(b) & 0x7FFF) == 0);
            mmcheck("frames came from above the 1 GiB ceiling", g_pages_above_1g >= 2);
            uint64_t before = g_zone.nr_free;
            free_pages(a, 0); free_pages(b, 3);
            mmcheck("free returns every frame (nr_free restored)", g_zone.nr_free == before + 1 + 8);
        }
    }
    kprintf("[mm     ] RESULT: %d passed, %d failed\n", pass, fail);
#undef mmcheck
}
