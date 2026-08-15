/* kdf_vector_test — validate crypto/scrypt.c against RFC 7914's published
 * vectors, on a host, with no kernel and no boot.
 * ===========================================================================
 *
 * v0.80. This exists because of a rule this project adopted after paying for
 * it: land a primitive against published vectors as its own verified unit
 * BEFORE wiring it in. A KDF exercised only through the login path is a KDF
 * whose failures look like authentication bugs — and this tree has twice spent
 * a milestone on a defect that was mis-attributed because the failing layer had
 * no independent test.
 *
 * It supplies the PBKDF2-HMAC-SHA-256 that crypto/scrypt.c declares. That
 * reference SHA-256 is here only to make the test self-contained; the KERNEL
 * will inject its own, already-verified one. What is under test here is the new
 * logic — Salsa20/8, BlockMix, ROMix, and the scrypt composition.
 *
 * Build and run:  make kdf-test
 * Exit code 0 only if every vector matches. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../crypto/scrypt.h"

/* ---- reference SHA-256 / HMAC / PBKDF2 (test scaffolding only) ------------ */

#define SHA256_BLOCK 64
#define SHA256_DIGEST 32

static const uint32_t K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];f=h[5];g=h[6];hh=h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
}

static void sha256(const void *data, uint64_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const uint8_t *p = (const uint8_t *)data;
    uint64_t n = len;
    while (n >= 64) { sha256_block(h, p); p += 64; n -= 64; }
    uint8_t tail[128]; uint64_t t = 0;
    for (uint64_t i = 0; i < n; i++) tail[t++] = p[i];
    tail[t++] = 0x80;
    while ((t % 64) != 56) tail[t++] = 0;
    uint64_t bits = len * 8;
    for (int i = 7; i >= 0; i--) tail[t++] = (uint8_t)(bits >> (i * 8));
    for (uint64_t i = 0; i < t; i += 64) sha256_block(h, tail + i);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(h[i] >> 24); out[i*4+1] = (uint8_t)(h[i] >> 16);
        out[i*4+2] = (uint8_t)(h[i] >> 8);  out[i*4+3] = (uint8_t)h[i];
    }
}

static void hmac_sha256(const uint8_t *key, uint32_t keylen,
                        const uint8_t *msg, uint64_t msglen, uint8_t out[32]) {
    uint8_t k[64] = {0}, ip[64], op[64], inner[32];
    if (keylen > 64) sha256(key, keylen, k);
    else for (uint32_t i = 0; i < keylen; i++) k[i] = key[i];
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    uint8_t *buf = malloc(64 + msglen); if (!buf) exit(2);
    memcpy(buf, ip, 64); memcpy(buf + 64, msg, msglen);
    sha256(buf, 64 + msglen, inner);
    free(buf);
    uint8_t buf2[64 + 32];
    memcpy(buf2, op, 64); memcpy(buf2 + 64, inner, 32);
    sha256(buf2, 96, out);
}

/* The multi-block PBKDF2 scrypt needs, and which the kernel's current
 * single-block version cannot provide. */
void scrypt_pbkdf2_sha256(const uint8_t *pw, uint32_t pwlen,
                          const uint8_t *salt, uint32_t saltlen,
                          uint32_t rounds, uint8_t *out, uint32_t outlen) {
    uint8_t u[32], t[32];
    uint8_t *si = malloc((size_t)saltlen + 4); if (!si) exit(2);
    memcpy(si, salt, saltlen);
    uint32_t blocks = (outlen + 31) / 32;
    for (uint32_t i = 1; i <= blocks; i++) {
        si[saltlen+0] = (uint8_t)(i >> 24); si[saltlen+1] = (uint8_t)(i >> 16);
        si[saltlen+2] = (uint8_t)(i >> 8);  si[saltlen+3] = (uint8_t)i;
        hmac_sha256(pw, pwlen, si, (uint64_t)saltlen + 4, u);
        memcpy(t, u, 32);
        for (uint32_t r = 1; r < rounds; r++) {
            uint8_t nx[32];
            hmac_sha256(pw, pwlen, u, 32, nx);
            for (int j = 0; j < 32; j++) { u[j] = nx[j]; t[j] ^= nx[j]; }
        }
        uint32_t off = (i - 1) * 32;
        uint32_t take = (outlen - off) < 32 ? (outlen - off) : 32;
        memcpy(out + off, t, take);
    }
    free(si);
}

/* ---- vectors -------------------------------------------------------------- */

static int fails = 0, passes = 0;

static void check(const char *name, const uint8_t *got, const uint8_t *want, int n) {
    if (memcmp(got, want, n) == 0) { passes++; printf("  PASS  %s\n", name); return; }
    fails++;
    printf("  FAIL  %s\n        got  ", name);
    for (int i = 0; i < n; i++) printf("%02x", got[i]);
    printf("\n        want ");
    for (int i = 0; i < n; i++) printf("%02x", want[i]);
    printf("\n");
}

/* RFC 7914 section 11. The fourth published vector (N=1048576, r=8, p=1) needs
 * 1 GiB of scratch and is deliberately not run — see KDF-DESIGN.md. Its absence
 * is stated rather than silently skipped. */
struct vec { const char *pw, *salt; uint32_t n, r, p; uint8_t want[64]; };

static const struct vec V[] = {
{ "", "", 16, 1, 1, {
0x77,0xd6,0x57,0x62,0x38,0x65,0x7b,0x20,0x3b,0x19,0xca,0x42,0xc1,0x8a,0x04,0x97,
0xf1,0x6b,0x48,0x44,0xe3,0x07,0x4a,0xe8,0xdf,0xdf,0xfa,0x3f,0xed,0xe2,0x14,0x42,
0xfc,0xd0,0x06,0x9d,0xed,0x09,0x48,0xf8,0x32,0x6a,0x75,0x3a,0x0f,0xc8,0x1f,0x17,
0xe8,0xd3,0xe0,0xfb,0x2e,0x0d,0x36,0x28,0xcf,0x35,0xe2,0x0c,0x38,0xd1,0x89,0x06}},
{ "password", "NaCl", 1024, 8, 16, {
0xfd,0xba,0xbe,0x1c,0x9d,0x34,0x72,0x00,0x78,0x56,0xe7,0x19,0x0d,0x01,0xe9,0xfe,
0x7c,0x6a,0xd7,0xcb,0xc8,0x23,0x78,0x30,0xe7,0x73,0x76,0x63,0x4b,0x37,0x31,0x62,
0x2e,0xaf,0x30,0xd9,0x2e,0x22,0xa3,0x88,0x6f,0xf1,0x09,0x27,0x9d,0x98,0x30,0xda,
0xc7,0x27,0xaf,0xb9,0x4a,0x83,0xee,0x6d,0x83,0x60,0xcb,0xdf,0xa2,0xcc,0x06,0x40}},
{ "pleaseletmein", "SodiumChloride", 16384, 8, 1, {
0x70,0x23,0xbd,0xcb,0x3a,0xfd,0x73,0x48,0x46,0x1c,0x06,0xcd,0x81,0xfd,0x38,0xeb,
0xfd,0xa8,0xfb,0xba,0x90,0x4f,0x8e,0x3e,0xa9,0xb5,0x43,0xf6,0x54,0x5d,0xa1,0xf2,
0xd5,0x43,0x29,0x55,0x61,0x3f,0x0f,0xcf,0x62,0xd4,0x97,0x05,0x24,0x2a,0x9a,0xf9,
0xe6,0x1e,0x85,0xdc,0x0d,0x65,0x1e,0x40,0xdf,0xcf,0x01,0x7b,0x45,0x57,0x58,0x87}},
};

int main(void) {
    printf("-- KDF VECTOR TEST: scrypt (RFC 7914), standalone, no kernel --\n");

    /* A PBKDF2 self-check first. If the injected dependency is wrong, every
     * scrypt vector fails and the reason looks like scrypt. RFC 6070 case 1. */
    {
        uint8_t out[20];
        static const uint8_t want[20] = {
            0x0c,0x60,0xc8,0x0f,0x96,0x1f,0x0e,0x71,0xf3,0xa9,
            0xb5,0x24,0xaf,0x60,0x12,0x06,0x2f,0xe0,0x37,0xa6};
        /* RFC 6070 is PBKDF2-HMAC-SHA1; the SHA-256 analogue is checked instead
         * via a known SHA-256 digest so the scaffolding is not trusted blind. */
        uint8_t d[32];
        static const uint8_t abc[32] = {
            0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
            0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
        sha256("abc", 3, d);
        check("sha256(\"abc\") — the scaffolding itself", d, abc, 32);
        (void)out; (void)want;
    }

    for (unsigned i = 0; i < sizeof(V)/sizeof(V[0]); i++) {
        const struct vec *v = &V[i];
        uint64_t need = scrypt_scratch_bytes(v->n, v->r, v->p);
        void *scratch = malloc((size_t)need);
        if (!scratch) { printf("  FAIL  vector %u: could not allocate %llu bytes\n",
                               i, (unsigned long long)need); fails++; continue; }
        uint8_t got[64];
        char name[160];
        snprintf(name, sizeof name, "scrypt(N=%u r=%u p=%u) [%.1f MiB scratch]",
                 v->n, v->r, v->p, (double)need / (1024.0 * 1024.0));
        int rc = scrypt((const uint8_t *)v->pw, (uint32_t)strlen(v->pw),
                        (const uint8_t *)v->salt, (uint32_t)strlen(v->salt),
                        v->n, v->r, v->p, got, 64, scratch, need);
        if (rc != SCRYPT_OK) { printf("  FAIL  %s: rc=%d\n", name, rc); fails++; }
        else check(name, got, v->want, 64);
        free(scratch);
    }

    /* The refusals matter as much as the answers: this module's contract is
     * that it never overruns a short buffer and never accepts a non-power-of-two
     * N, because ROMix indexes with a mask. */
    {
        uint8_t o[32]; uint8_t sc[4096];
        int rc = scrypt((const uint8_t *)"p", 1, (const uint8_t *)"s", 1,
                        15, 1, 1, o, 32, sc, sizeof sc);
        if (rc == SCRYPT_EPARAM) { passes++; printf("  PASS  N=15 refused (not a power of two)\n"); }
        else { fails++; printf("  FAIL  N=15 returned %d, expected SCRYPT_EPARAM\n", rc); }
        rc = scrypt((const uint8_t *)"p", 1, (const uint8_t *)"s", 1,
                    1024, 8, 1, o, 32, sc, sizeof sc);
        if (rc == SCRYPT_ENOMEM) { passes++; printf("  PASS  short scratch refused\n"); }
        else { fails++; printf("  FAIL  short scratch returned %d, expected SCRYPT_ENOMEM\n", rc); }
    }

    printf("-- RESULT: %d passed, %d failed --\n", passes, fails);
    printf("-- NOT COVERED: RFC 7914's N=1048576 vector (1 GiB scratch); see KDF-DESIGN.md --\n");
    return fails ? 1 : 0;
}
