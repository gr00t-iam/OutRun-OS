/* Exercise the actual descriptor core with a 512 MiB-shaped allocator:
 * the buddy has no pages, while the legacy pool still has free RAM. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define FD_INLINE 16
#define FD_MAX 256
#define OFILE_MAX 64
#define MAX_KPROC 2
#define PG_LEGACY 1024u
#define GFP_ZERO 8u
struct page { unsigned flags; void *memory; };
static struct page pages[32],*g_mem_map=pages;
static int g_physmap_ready=1,buddy,allocated,freed,locked;
struct files_struct { int16_t *fd,fd_inline[FD_INLINE]; uint32_t max_fds,next_fd,nopen; struct page *grown; uint32_t grown_order; };
static struct { struct files_struct files; int redir_in,redir_out,redir_in_oi,redir_out_oi; } kprocs[MAX_KPROC];
static struct { int used,dirent,nref,volume,pipe,pipe_w,ep,efd,sock,flags; uint64_t off; } g_ofiles[OFILE_MAX];
static int g_ofile_lock;
static void klock_acquire(int *l) { (void)l; assert(!locked); locked=1; }
static void klock_release(int *l) { (void)l; assert(locked); locked=0; }
static int tg_of(int s) { return s; }
static void pipe_ref_locked(int p,int w) { (void)p; (void)w; }
static void pipe_unref_locked(int p,int w) { (void)p; (void)w; }
static int ofile_last_drop_locked(int oi) { g_ofiles[oi].used=0; return 1; }
static uint64_t page_to_phys(const struct page *p) { return (uint64_t)(p-pages)*4096; }
static struct page *phys_to_page(uint64_t pa) { assert(pa/4096<32); return &pages[pa/4096]; }
static void *host_map(uint64_t pa) { return phys_to_page(pa)->memory; }
#define PHYS_TO_VIRT(pa) host_map(pa)
static struct page *new_page(unsigned flags) {
    assert(!locked && allocated<31);
    struct page *p=&pages[++allocated]; p->flags=flags;
    p->memory=calloc(1,4096); assert(p->memory); return p;
}
static struct page *alloc_pages(uint32_t order,uint32_t flags) {
    (void)flags; assert(order==0 && !locked); return buddy?new_page(0):0;
}
static uint64_t alloc_frame_limited(void) { return page_to_phys(new_page(PG_LEGACY)); }
static void free_pages(struct page *p,uint32_t order) {
    assert(!locked && order==0 && !(p->flags & PG_LEGACY)); free(p->memory); p->memory=0; freed++;
}
static int free_frame(uint64_t pa) {
    struct page *p=phys_to_page(pa); assert(!locked && (p->flags & PG_LEGACY));
    free(p->memory); p->memory=0; freed++; return 1;
}
#include "../metal/kernel/files.c"
int main(void) {
    /* Keep fallback fixture helpers referenced in the pre-fix build too. */
    (void)alloc_frame_limited; (void)free_frame; (void)g_mem_map; (void)g_physmap_ready;
    for(buddy=0;buddy<2;buddy++) {
        files_init(&kprocs[0].files);
        int fds[24];
        for(int i=0;i<24;i++) {
            klock_acquire(&g_ofile_lock); int oi=ofile_new_locked(0,0); klock_release(&g_ofile_lock);
            assert(oi>=0); fds[i]=fd_alloc_install(0,oi,0); assert(fds[i]==i);
            assert(fd_lookup_locked(0,fds[i])==oi);
        }
        for(int i=0;i<24;i++) { klock_acquire(&g_ofile_lock); assert(fd_release_locked(0,fds[i])); klock_release(&g_ofile_lock); }
        assert(kprocs[0].files.nopen==0); files_free(&kprocs[0].files);
        assert(allocated==freed);
    }
    puts("files: >16 descriptors with empty/populated buddy, matched frees, lock order PASS");
}
