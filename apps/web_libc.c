/* Freestanding libc subset for the vendored Mbed TLS build.
 *
 * Mbed TLS is compiled UNMODIFIED -- that is the entire security argument for
 * vendoring a maintained TLS stack rather than writing one -- so the symbols it
 * expects from a hosted libc have to come from somewhere. They come from here.
 *
 * This file exists because the alternative is worse. Patching upstream to avoid
 * memcpy would make every version bump a merge conflict, and a TLS library
 * nobody dares update is a liability, not an asset.
 *
 * SCOPE IS DELIBERATELY THE LINKER'S. Every symbol below appeared as an
 * `undefined reference` when linking outrun_web.elf; nothing was added
 * speculatively. If upstream starts calling something new the link fails
 * loudly, which is the correct outcome -- far better than a stub that silently
 * returns a wrong answer inside certificate validation.
 *
 * The _chk variants are what -O2 emits for fortified calls. They forward to the
 * plain implementation and IGNORE the object size, which is sound only because
 * this is a freestanding build with no _FORTIFY_SOURCE guarantee to uphold; the
 * bounds checking they would normally perform is not being silently dropped
 * from code that had it, because this code never had it.
 */
#include <stddef.h>

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    if (a == b || !n) return d;
    if (a < b) { while (n--) *a++ = *b++; return d; }
    a += n; b += n;
    while (n--) *--a = *--b;
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *a = d;
    while (n--) *a++ = (unsigned char)c;
    return d;
}
int memcmp(const void *x, const void *y, size_t n) {
    const unsigned char *a = x, *b = y;
    for (; n--; a++, b++) if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}
/* Mbed TLS does not need this, but outrun_web.c's snapshot validator does:
 * it uses memchr to prove every fixed-size string field in a restored
 * snapshot is NUL-terminated before anything reads it as a C string. GCC
 * also synthesises calls to it from open-coded scan loops, so a freestanding
 * link needs a real definition even where the source never names it. */
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n--; p++) if (*p == (unsigned char)c) return (void *)p;
    return 0;
}
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (; n--; a++, b++) {
        if (*a != *b) return (int)(unsigned char)*a - (int)(unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}
char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}
char *strrchr(const char *s, int c) {
    const char *hit = 0;
    for (;; s++) {
        if (*s == (char)c) hit = s;
        if (!*s) return (char *)hit;
    }
}
char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *a = hay, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)hay;
    }
    return 0;
}

/* Fortified aliases emitted by -O2. See the header comment: the size argument
 * is accepted and not enforced, because this build makes no fortification
 * promise to begin with. */
void *__memcpy_chk(void *d, const void *s, size_t n, size_t os) { (void)os; return memcpy(d, s, n); }
void *__memmove_chk(void *d, const void *s, size_t n, size_t os) { (void)os; return memmove(d, s, n); }
void *__memset_chk(void *d, int c, size_t n, size_t os) { (void)os; return memset(d, c, n); }
void __explicit_bzero_chk(void *d, size_t n, size_t os) {
    (void)os;
    /* volatile so the clear of key material is not optimised away, which is the
     * whole point of the call upstream is making. */
    volatile unsigned char *p = d;
    while (n--) *p++ = 0;
}

/* 128-bit unsigned division. GCC emits a call to this from Mbed TLS's bignum
 * path on x86-64 when it cannot use a hardware divide; libgcc normally supplies
 * it, and a freestanding link does not pull libgcc in. Long division, base
 * 2^32, so it is correct rather than fast -- this is not on the bulk data
 * path. */
typedef unsigned __int128 web_u128;
web_u128 __udivti3(web_u128 a, web_u128 b) {
    if (!b) return 0;                 /* upstream never divides by zero here */
    if (b > a) return 0;
    web_u128 q = 0, r = 0;
    for (int i = 127; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) { r -= b; q |= (web_u128)1 << i; }
    }
    return q;
}

/* ---- the four hosted-platform calls, each pinned to a real upstream site ----
 *
 * These were identified from the linker's own diagnostics, not guessed, and
 * each is implemented to match what upstream actually needs at that site.
 */

/* memory_buffer_alloc.c calls exit() when its heap is corrupt. There is no
 * process to exit in ring 3 here and returning would continue allocating from a
 * heap just declared unusable, so trap instead: a deliberate fault is
 * debuggable, silent corruption inside a TLS allocator is not. */
void exit(int status) {
    (void)status;
    for (;;) __asm__ volatile("ud2");
}

/* platform_util.c's mbedtls_ms_time(). MBEDTLS_PLATFORM_TIME_ALT redirects
 * mbedtls_time() to the browser's own web_time(), but mbedtls_ms_time is a
 * SEPARATE entry point with no _ALT hook, so it still needs these two.
 *
 * They are deliberately monotonic-from-zero rather than wall clock: this
 * function is used for timing/backoff, never for certificate validity, which
 * goes through mbedtls_time() and therefore through web_time()'s RTC-backed
 * value. A zero epoch here cannot make an expired certificate look valid. */
struct web_timespec { long long tv_sec; long long tv_nsec; };
int clock_gettime(int clk, struct web_timespec *ts) {
    (void)clk;
    if (ts) { ts->tv_sec = 0; ts->tv_nsec = 0; }
    return 0;
}
long long time(long long *out) { if (out) *out = 0; return 0; }

/* x509_crt.c uses inet_pton to decide whether a certificate CN is an IP
 * literal, and to compare it against the connection's address.
 *
 * THIS ONE IS SECURITY-RELEVANT, so it is implemented properly rather than
 * stubbed. A version that always failed would silently reclassify every
 * IP-address CN as a hostname; one that always succeeded would accept
 * malformed literals. Both weaken verification in a way no test here would
 * notice. AF_INET=2 and AF_INET6=10 are the Linux values Mbed TLS compiles
 * against; anything else is refused rather than assumed.
 *
 * Returns 1 on success, 0 on a malformed address, -1 for an unsupported
 * family -- the contract upstream checks. */
int inet_pton(int af, const char *src, void *dst) {
    unsigned char *out = dst;
    if (!src || !dst) return 0;
    if (af == 2) {                                   /* AF_INET: a.b.c.d */
        unsigned char q[4]; int part = 0;
        for (;;) {
            if (*src < '0' || *src > '9') return 0;
            unsigned v = 0, digits = 0;
            while (*src >= '0' && *src <= '9') {
                v = v * 10 + (unsigned)(*src++ - '0');
                if (++digits > 3 || v > 255) return 0;
            }
            q[part++] = (unsigned char)v;
            if (part == 4) break;
            if (*src++ != '.') return 0;
        }
        if (*src) return 0;                          /* trailing rubbish */
        for (int i = 0; i < 4; i++) out[i] = q[i];
        return 1;
    }
    if (af == 10) {                                  /* AF_INET6 */
        unsigned char buf[16]; int have = 0, gap = -1;
        for (int i = 0; i < 16; i++) buf[i] = 0;
        if (src[0] == ':') { if (src[1] != ':') return 0; src++; }
        while (*src) {
            if (*src == ':') {
                if (gap >= 0) return 0;              /* only one "::" allowed */
                gap = have; src++;
                if (!*src) break;
                continue;
            }
            unsigned v = 0, digits = 0;
            while (*src) {
                int d;
                if (*src >= '0' && *src <= '9') d = *src - '0';
                else if (*src >= 'a' && *src <= 'f') d = *src - 'a' + 10;
                else if (*src >= 'A' && *src <= 'F') d = *src - 'A' + 10;
                else break;
                v = (v << 4) | (unsigned)d; src++;
                if (++digits > 4) return 0;
            }
            if (!digits) return 0;
            if (have + 2 > 16) return 0;
            buf[have++] = (unsigned char)(v >> 8);
            buf[have++] = (unsigned char)(v & 255);
            if (!*src) break;
            if (*src != ':') return 0;
            src++;
            if (!*src) return 0;                     /* a trailing single ':' */
        }
        if (gap < 0) { if (have != 16) return 0; }
        else {
            if (have == 16) return 0;                /* "::" must cover >= 1 group */
            int tail = have - gap;
            for (int i = 0; i < tail; i++) buf[16 - tail + i] = buf[gap + i];
            for (int i = gap; i < 16 - tail; i++) buf[i] = 0;
        }
        for (int i = 0; i < 16; i++) out[i] = buf[i];
        return 1;
    }
    return -1;                                       /* unsupported family */
}
