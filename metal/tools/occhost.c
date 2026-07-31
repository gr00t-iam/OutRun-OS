/* occhost.c — run occ on the HOST, then execute what it produced, in-process.
 *
 * This is a development harness for iterating on occ's code generator without
 * a 12-minute boot per attempt. It supplies the eight runtime functions occ.c
 * needs, compiles a .oc file, maps the resulting image at the fixed addresses
 * occ bakes into its immediates, and CALLS the compiled `main` directly.
 *
 * Calling `main` rather than the ELF entry point matters: the entry runs a
 * prologue that ends in SYS_EXIT, and OutRun's syscall numbers are not Linux's,
 * so entering there would execute a meaningless host syscall. Test programs
 * therefore compute a value and return it, and nothing they run touches the
 * kernel at all.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

static void *omalloc(u64 n)            { return calloc(1, (size_t)n); }
static void  ofree(void *p)            { free(p); }
static int   oopen(const char *p)      { return open(p, O_RDONLY); }
static i64   oread(int fd, void *b, u64 n) { return (i64)read(fd, b, (size_t)n); }
static int   oclose(int fd)            { return close(fd); }
static int   ocreat(const char *p)     { return open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); }
static i64   owrite(int fd, const void *b, u64 n) { return (i64)write(fd, b, (size_t)n); }
static int   ostrneq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; }
    return 1;
}

#include "occ.c"

/* Map one region at a FIXED address and copy bytes into it. occ's code holds
 * absolute addresses as immediates, so the mapping address is not negotiable. */
static int map_at(u64 base, const void *src, size_t len, int prot) {
    u64 page = base & ~0xFFFULL;
    size_t span = (size_t)((base - page) + len + 0xFFF) & ~(size_t)0xFFF;
    if (span == 0) span = 0x1000;          /* an empty section still needs a page */
    void *m = mmap((void *)page, span, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return -1; }
    memcpy((void *)base, src, len);
    if (mprotect((void *)page, span, prot) != 0) { perror("mprotect"); return -1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: occhost <src.oc> [expected]\n"); return 2; }

    /* occ_compile frees its buffers, so snapshot the three images first by
     * compiling to a file and re-reading it is possible — but occ_syms and the
     * emitted section contents are what we actually need, and occ_syms is a
     * static array that survives. Compile, then rebuild the image from the ELF. */
    const char *srcs[1] = { argv[1] };
    char out[256];
    snprintf(out, sizeof out, "%s.elf", argv[1]);

    int r = occ_compile(srcs, 1, out);
    if (r != 0) { fprintf(stderr, "occhost: occ_compile failed (%d)\n", r); return 3; }

    int ms = occ_sym_find("main");
    if (ms < 0 || !occ_syms[ms].defined) { fprintf(stderr, "occhost: no main\n"); return 4; }
    u64 mainaddr = OCC_TEXT_BASE + (u64)occ_syms[ms].addr;

    /* Re-read the ELF and map its PT_LOADs where they ask to live. */
    int fd = open(out, O_RDONLY);
    if (fd < 0) { perror("open elf"); return 5; }
    static u8 buf[8u << 20];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n < (ssize_t)sizeof(struct occ_eh)) { fprintf(stderr, "occhost: short elf\n"); return 6; }

    struct occ_eh *eh = (struct occ_eh *)buf;
    struct occ_ph *ph = (struct occ_ph *)(buf + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != 1) continue;                 /* PT_LOAD */
        int prot = PROT_READ;
        if (ph[i].flags & 1) prot |= PROT_EXEC;
        if (ph[i].flags & 2) prot |= PROT_WRITE;
        if (map_at(ph[i].vaddr, buf + ph[i].offset, (size_t)ph[i].filesz, prot) != 0) return 7;
    }

    int (*fn)(void) = (int (*)(void))(void *)mainaddr;
    int got = fn();
    printf("%s -> %d\n", argv[1], got);
    if (argc >= 3) {
        int want = atoi(argv[2]);
        if (got != want) { printf("  MISMATCH: expected %d\n", want); return 1; }
    }
    return 0;
}
