/* ===========================================================================
 * OutRun OS — files.c: per-process descriptor tables (v1.3)
 * ===========================================================================
 * Included into kernel64.c immediately after `struct ofile` / g_ofiles.
 *
 * WHAT CHANGED, AND WHAT DELIBERATELY DID NOT.
 *
 * Through v1.2 a file descriptor number WAS an index into g_ofiles[16], a
 * single system-wide table, and "which processes may use fd 7" was a 64-bit
 * owner_mask on the entry. That design has two ceilings that a self-hosting
 * toolchain hits immediately: sixteen open files across the WHOLE MACHINE,
 * and sixty-four processes ever (the mask width). It also cannot express dup:
 * two fd numbers in one process naming one description — which is why
 * SYS_SETREDIR exists as a special case instead of dup2().
 *
 * The v1.3 model is the POSIX one, in two layers:
 *
 *   struct ofile       — the OPEN FILE DESCRIPTION. Offset, flags, volume,
 *                        pipe end, socket, epoll/eventfd instance. SHARED:
 *                        fork, dup and IPC transfer all alias one of these.
 *                        Refcounted by `nref`, a plain count of descriptor
 *                        slots pointing at it. This is the OLD struct with
 *                        owner_mask replaced by nref, and it grew from 16 to
 *                        OFILE_MAX entries.
 *
 *   struct files_struct — the PER-PROCESS DESCRIPTOR TABLE. fd number ->
 *                        ofile index. Lives on the thread-group leader (the
 *                        identity every fd check already resolves through
 *                        tg_of()). Starts inline at FD_INLINE and grows by
 *                        doubling to FD_MAX from the buddy allocator.
 *
 * DELIBERATELY UNCHANGED — every rule below was learned the hard way and is
 * tested by an existing suite:
 *
 *   - A PIPE END is refcounted PER DESCRIPTOR SLOT (pipe_ref_locked on every
 *     alias, pipe_unref_locked on every drop), not per description. That is
 *     what makes `a | b` see EOF: a forked child's copy of the write end is a
 *     separate right to write. (pipestrs, cross-fork round.) The per-slot ref
 *     now happens in fd_install / fd_release, which is where slots are made.
 *   - EPOLL and EVENTFD instances belong to the DESCRIPTION and go when its
 *     last reference goes — never per slot. (v0.64.)
 *   - A closed descriptor is PURGED from every epoll watch on the LAST
 *     reference drop, matching when the fd number could be reissued. (v0.64
 *     Phase 2, found live.)
 *   - redir_in/redir_out are references to fd NUMBERS in this process; a
 *     release of that number clears them, or the next open reattaches stdout
 *     to a stranger's file. (v0.59.)
 *   - g_ofile_lock (rank 3) still covers BOTH layers. One lock, one rank, the
 *     ABBA with g_vfs_lock still engineered out the same way (vfs resolves
 *     the name and RELEASES before claiming a descriptor).
 *
 * LOCK RANK / ALLOCATION INVARIANT. files_expand() takes buddy frames, and
 * g_zone_lock is rank 0 — beneath g_ofile_lock. So the table is grown BEFORE
 * g_ofile_lock is acquired (fd_alloc reserves the room, drops the lock,
 * expands, re-acquires and re-checks) and never under it. This is the first
 * consumer of the rank-0 invariant stated at the rank table, and it is the
 * pattern every later one must follow.
 * =========================================================================== */

/* Counters — printed by descriptor teardown's debug path and by fdstrs. */
#ifdef FDCEIL_FALSIFY
#define OFILE_LIMIT 16
#else
#define OFILE_LIMIT OFILE_MAX
#endif
static volatile uint64_t g_files_expands  = 0;   /* tables that grew           */
static volatile uint64_t g_files_max_seen = 0;   /* highest fd ever installed  */
static volatile uint64_t g_ofile_high     = 0;   /* highest ofile index in use */

static void files_init(struct files_struct *f) {
    f->max_fds = FD_INLINE; f->next_fd = 0; f->fd = f->fd_inline;
    for (uint32_t i = 0; i < FD_INLINE; i++) f->fd_inline[i] = -1;
    f->grown = 0; f->grown_order = 0; f->nopen = 0;
}

/* In the current split allocator, <=1 GiB guests have NO buddy pages.
 * A descriptor table (at most 512 bytes) still fits one legacy frame. Keep
 * the backing allocator identity so no legacy page is freed to the buddy. */
_Static_assert(FD_MAX * sizeof(int16_t) <= 4096, "descriptor fallback requires one page");
static void files_backing_free(struct page *p,uint32_t order) {
    if(p->flags & PG_LEGACY) free_frame(page_to_phys(p));
    else free_pages(p,order);
}

/* Grow to at least `want` slots. Called with NO lock held (see the invariant
 * above); the caller re-acquires g_ofile_lock and re-checks afterwards. Copies
 * under the lock so a concurrent installer cannot be lost. Returns 0 or -1. */
static int files_expand(struct files_struct *f, uint32_t want) {
    if (want > FD_MAX) return -1;
    uint32_t cap = f->max_fds;
    while (cap < want) cap <<= 1;
    if (cap > FD_MAX) cap = FD_MAX;
    uint64_t bytes = (uint64_t)cap * sizeof(int16_t);
    uint32_t order = 0;
    while ((0x1000ull << order) < bytes) order++;
    struct page *pg = alloc_pages(order, GFP_ZERO);
    if (!pg && order == 0 && g_physmap_ready && g_mem_map) {
        uint64_t pa = alloc_frame_limited();
        if(pa) pg = phys_to_page(pa);
    }
    if (!pg) return -1;
    int16_t *nt = (int16_t *)PHYS_TO_VIRT(page_to_phys(pg));
    for (uint32_t i = 0; i < cap; i++) nt[i] = -1;
    klock_acquire(&g_ofile_lock);
    if (f->max_fds >= cap) {                        /* someone else grew it first */
        klock_release(&g_ofile_lock);
        files_backing_free(pg, order);
        return 0;
    }
    for (uint32_t i = 0; i < f->max_fds; i++) nt[i] = f->fd[i];
    struct page *old = f->grown; uint32_t oo = f->grown_order;
    f->fd = nt; f->max_fds = cap; f->grown = pg; f->grown_order = order;
    klock_release(&g_ofile_lock);
    if (old) files_backing_free(old, oo);
    __sync_fetch_and_add(&g_files_expands, 1);
    return 0;
}

static void files_free(struct files_struct *f) {
    if (f->grown) { files_backing_free(f->grown, f->grown_order); f->grown = 0; }
    f->fd = f->fd_inline; f->max_fds = FD_INLINE;
}

/* ---- Description layer (g_ofiles) ----------------------------------------- */

/* Claim a fresh description. Caller holds g_ofile_lock. Returns index or -1.
 * nref starts at 0; fd_install_locked adds the first reference. */
static int ofile_new_locked(int volume, int dirent) {
    for (int i = 0; i < OFILE_LIMIT; i++) {
        if (g_ofiles[i].used) continue;
        g_ofiles[i].used = 1; g_ofiles[i].dirent = dirent; g_ofiles[i].off = 0;
        g_ofiles[i].nref = 0; g_ofiles[i].volume = volume;
        g_ofiles[i].pipe = -1; g_ofiles[i].pipe_w = 0;
        g_ofiles[i].ep = -1;   g_ofiles[i].efd = -1;
        g_ofiles[i].sock = -1; g_ofiles[i].flags = 0;
        if ((uint64_t)i > g_ofile_high) g_ofile_high = (uint64_t)i;
        return i;
    }
    return -1;
}

/* ---- Descriptor layer (per process) --------------------------------------- */

/* The descriptor table of the process slot `slot` belongs to.
 *
 * A slot index that is out of range resolves to a permanently EMPTY table
 * rather than to kprocs[-1]. tg_of() returns its argument unchanged for
 * anything it cannot resolve, so a caller that passes a negative or oversized
 * slot would otherwise index the array out of bounds — gcc's -Warray-bounds
 * says so, and it is right. max_fds is 0, so every lookup in it fails and no
 * fd can ever be installed: an invalid slot gets EBADF and EMFILE, which is
 * what it should get, instead of a wild pointer. */
static struct files_struct g_files_null = { g_files_null.fd_inline, {0}, 0, 0, 0, 0, 0 };

static inline struct files_struct *files_of(int slot) {
    int L = tg_of(slot);
    if (L < 0 || L >= MAX_KPROC) return &g_files_null;
    return &kprocs[L].files;
}

/* Lowest free fd >= min in this process. Caller holds g_ofile_lock. -1 if the
 * table is full at its CURRENT size (the caller may expand and retry). */
static int fd_find_free_locked(struct files_struct *f, uint32_t min) {
    for (uint32_t i = (min > f->next_fd ? min : f->next_fd); i < f->max_fds; i++)
        if (f->fd[i] < 0) return (int)i;
    for (uint32_t i = min; i < f->next_fd && i < f->max_fds; i++)
        if (f->fd[i] < 0) return (int)i;
    return -1;
}

/* Point fd number `n` of process `slot` at description `oi`, taking one
 * reference — and, for a pipe end, one per-slot pipe ref. Caller holds the
 * lock. The slot must be free. */
static void fd_install_locked(int slot, int n, int oi) {
    struct files_struct *f = files_of(slot);
    f->fd[n] = (int16_t)oi;
    f->nopen++;
    if ((uint32_t)n >= f->next_fd) f->next_fd = (uint32_t)n + 1;
    g_ofiles[oi].nref++;
    if (g_ofiles[oi].pipe >= 0) pipe_ref_locked(g_ofiles[oi].pipe, g_ofiles[oi].pipe_w);
    if ((uint64_t)n > g_files_max_seen) g_files_max_seen = (uint64_t)n;
}

/* The description this process's fd number names, or -1. Caller holds lock. */
static inline int fd_lookup_locked(int slot, int n) {
    struct files_struct *f = files_of(slot);
    if (n < 0 || (uint32_t)n >= f->max_fds) return -1;
    int oi = f->fd[n];
    if (oi < 0 || !g_ofiles[oi].used) return -1;
    return oi;
}

/* Allocate an fd number in `slot` for description `oi`, growing the table if
 * needed. Handles the lock itself because growth must happen WITHOUT it.
 * Returns the fd number or -1 (EMFILE). */
static int fd_alloc_install(int slot, int oi, uint32_t min) {
    struct files_struct *f = files_of(slot);
    for (int attempt = 0; attempt < 3; attempt++) {
        klock_acquire(&g_ofile_lock);
        if (!g_ofiles[oi].used) { klock_release(&g_ofile_lock); return -1; }
        int n = fd_find_free_locked(f, min);
        if (n >= 0) { fd_install_locked(slot, n, oi); klock_release(&g_ofile_lock); return n; }
        uint32_t want = f->max_fds * 2;
        if (want < min + 1) want = min + 1;
        klock_release(&g_ofile_lock);
        if (f->max_fds >= FD_MAX) return -1;
        if (files_expand(f, want) < 0) return -1;
    }
    return -1;
}

/* Release fd number `n` in `slot`. Drops the description reference; when the
 * last one goes, the description's per-description resources (epoll watches,
 * the epoll/eventfd instance, the socket) are released exactly as
 * ofile_drop_locked did. Caller holds g_ofile_lock. Returns 1 if the
 * description itself was freed. */
static int fd_release_locked(int slot, int n) {
    struct files_struct *f = files_of(slot);
    if (n < 0 || (uint32_t)n >= f->max_fds) return 0;
    int oi = f->fd[n];
    if (oi < 0 || !g_ofiles[oi].used) return 0;
    f->fd[n] = -1;
    if (f->nopen) f->nopen--;
    if ((uint32_t)n < f->next_fd) f->next_fd = (uint32_t)n;
    /* per-SLOT pipe ref, dropped on every release (see the header) */
    if (g_ofiles[oi].pipe >= 0) pipe_unref_locked(g_ofiles[oi].pipe, g_ofiles[oi].pipe_w);
    /* redirections name fd NUMBERS of this process, and the DESCRIPTION each
     * one was set up on is cleared with it — a redirection whose number has
     * been given back names nothing, and leaving the description behind would
     * make a later "is this still the object I was set up on?" test compare
     * against a stale answer. Both halves or neither. */
    int L = tg_of(slot);
    if (kprocs[L].redir_in  == n) { kprocs[L].redir_in  = -1; kprocs[L].redir_in_oi  = -1; }
    if (kprocs[L].redir_out == n) { kprocs[L].redir_out = -1; kprocs[L].redir_out_oi = -1; }
    if (--g_ofiles[oi].nref > 0) return 0;           /* other references remain */
    return ofile_last_drop_locked(oi);
}
