/* scrypt (RFC 7914) — the memory-hard core, with NO allocation of its own.
 * ===========================================================================
 *
 * v0.80. This file is deliberately the NEW logic and nothing else: Salsa20/8,
 * BlockMix, ROMix and the scrypt composition. It does not contain SHA-256,
 * HMAC or PBKDF2 — those already exist in this tree, are already verified, and
 * a second copy of a verified primitive is a second thing to keep correct.
 *
 * The dependency is declared, not implemented:
 *
 *     scrypt_pbkdf2_sha256()
 *
 * The kernel supplies it from its own HMAC; the standalone vector test supplies
 * a reference. That is what lets this module be validated against published
 * vectors on a host, in seconds, with no kernel, no scheduler and no boot — the
 * design principle this milestone is built on. A KDF exercised only through the
 * login path is a KDF whose failures look like authentication bugs.
 *
 * NO ALLOCATION. scrypt's whole point is that it needs a large working set, and
 * a kernel primitive that calls an allocator inside a credential path is a
 * primitive that can fail for reasons unrelated to credentials — and that holds
 * memory while doing so. The caller passes a scratch buffer and the exact size
 * it has; this code refuses rather than overruns. See scrypt_scratch_bytes().
 *
 * ENDIANNESS. RFC 7914 defines Salsa20/8 over little-endian 32-bit words, and
 * this kernel is x86-64 only, so the words are read and written directly. On a
 * big-endian port every load and store here needs a byte swap; stated because
 * the code will look correct and produce wrong answers if that is missed. */

#include <stdint.h>
#include "scrypt.h"

/* Supplied by the caller's environment — see the header. dkLen is arbitrary:
 * scrypt needs p*128*r bytes out of the first call, which is far more than one
 * SHA-256 block, and that is exactly what this tree's existing single-block
 * PBKDF2 could not do. */
extern void scrypt_pbkdf2_sha256(const uint8_t *pw, uint32_t pwlen,
                                 const uint8_t *salt, uint32_t saltlen,
                                 uint32_t rounds, uint8_t *out, uint32_t outlen);

static void sc_memcpy(void *d, const void *s, uint32_t n) {
    uint8_t *dd = (uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    for (uint32_t i = 0; i < n; i++) dd[i] = ss[i];
}

static uint32_t rotl32(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

/* Salsa20/8 core (RFC 7914 section 3). Eight rounds — four double-rounds —
 * then add the input, which is what makes it non-invertible and is the step
 * most often dropped by accident. */
static void salsa20_8(uint32_t out[16], const uint32_t in[16]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = in[i];
    for (int i = 0; i < 8; i += 2) {
        /* column round */
        x[ 4] ^= rotl32(x[ 0] + x[12],  7);  x[ 8] ^= rotl32(x[ 4] + x[ 0],  9);
        x[12] ^= rotl32(x[ 8] + x[ 4], 13);  x[ 0] ^= rotl32(x[12] + x[ 8], 18);
        x[ 9] ^= rotl32(x[ 5] + x[ 1],  7);  x[13] ^= rotl32(x[ 9] + x[ 5],  9);
        x[ 1] ^= rotl32(x[13] + x[ 9], 13);  x[ 5] ^= rotl32(x[ 1] + x[13], 18);
        x[14] ^= rotl32(x[10] + x[ 6],  7);  x[ 2] ^= rotl32(x[14] + x[10],  9);
        x[ 6] ^= rotl32(x[ 2] + x[14], 13);  x[10] ^= rotl32(x[ 6] + x[ 2], 18);
        x[ 3] ^= rotl32(x[15] + x[11],  7);  x[ 7] ^= rotl32(x[ 3] + x[15],  9);
        x[11] ^= rotl32(x[ 7] + x[ 3], 13);  x[15] ^= rotl32(x[11] + x[ 7], 18);
        /* row round */
        x[ 1] ^= rotl32(x[ 0] + x[ 3],  7);  x[ 2] ^= rotl32(x[ 1] + x[ 0],  9);
        x[ 3] ^= rotl32(x[ 2] + x[ 1], 13);  x[ 0] ^= rotl32(x[ 3] + x[ 2], 18);
        x[ 6] ^= rotl32(x[ 5] + x[ 4],  7);  x[ 7] ^= rotl32(x[ 6] + x[ 5],  9);
        x[ 4] ^= rotl32(x[ 7] + x[ 6], 13);  x[ 5] ^= rotl32(x[ 4] + x[ 7], 18);
        x[11] ^= rotl32(x[10] + x[ 9],  7);  x[ 8] ^= rotl32(x[11] + x[10],  9);
        x[ 9] ^= rotl32(x[ 8] + x[11], 13);  x[10] ^= rotl32(x[ 9] + x[ 8], 18);
        x[12] ^= rotl32(x[15] + x[14],  7);  x[13] ^= rotl32(x[12] + x[15],  9);
        x[14] ^= rotl32(x[13] + x[12], 13);  x[15] ^= rotl32(x[14] + x[13], 18);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

/* BlockMix (RFC 7914 section 4), r blocks in, r blocks out, 64-byte units.
 * The output permutation — evens first, then odds — is the part that is easy
 * to write "obviously" and get wrong. */
static void block_mix(uint32_t *b, uint32_t *y, uint32_t r) {
    uint32_t x[16];
    sc_memcpy(x, &b[(2 * r - 1) * 16], 64);
    for (uint32_t i = 0; i < 2 * r; i++) {
        for (int k = 0; k < 16; k++) x[k] ^= b[i * 16 + k];
        salsa20_8(x, x);
        sc_memcpy(&y[i * 16], x, 64);
    }
    for (uint32_t i = 0; i < r; i++) {
        sc_memcpy(&b[i * 16], &y[(i * 2) * 16], 64);
        sc_memcpy(&b[(i + r) * 16], &y[(i * 2 + 1) * 16], 64);
    }
}

/* Integerify (RFC 7914 section 5): the LAST 64-byte block, as a little-endian
 * integer. Only the low 32 bits are needed because N is a power of two well
 * under 2^32, and the modulus is therefore a mask. */
static uint32_t integerify(const uint32_t *b, uint32_t r) {
    return b[(2 * r - 1) * 16];
}

/* ROMix (RFC 7914 section 5). This is the memory-hard part: the first loop
 * fills V with N sequential states, the second walks V in an order that depends
 * on the data, so an attacker who keeps less than V must recompute. */
static void ro_mix(uint32_t *b, uint32_t *v, uint32_t *xy, uint32_t r, uint32_t n) {
    const uint32_t words = 32 * r;               /* 128*r bytes as u32 */
    for (uint32_t i = 0; i < n; i++) {
        sc_memcpy(&v[i * words], b, words * 4);
        block_mix(b, xy, r);
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = integerify(b, r) & (n - 1);  /* n is a power of two */
        const uint32_t *vj = &v[j * words];
        for (uint32_t k = 0; k < words; k++) b[k] ^= vj[k];
        block_mix(b, xy, r);
    }
}

uint64_t scrypt_scratch_bytes(uint32_t n, uint32_t r, uint32_t p) {
    /* V (128*r*N) + B (128*r*p) + XY (128*r*2), in bytes, as a 64-bit value so
     * a caller can see an absurd request rather than a wrapped one. */
    uint64_t rr = (uint64_t)r;
    return 128 * rr * (uint64_t)n + 128 * rr * (uint64_t)p + 128 * rr * 2;
}

int scrypt(const uint8_t *pw, uint32_t pwlen,
           const uint8_t *salt, uint32_t saltlen,
           uint32_t n, uint32_t r, uint32_t p,
           uint8_t *out, uint32_t outlen,
           void *scratch, uint64_t scratchlen) {
    if (!n || (n & (n - 1))) return SCRYPT_EPARAM;    /* N must be a power of 2 */
    if (!r || !p) return SCRYPT_EPARAM;
    if (!out || !outlen) return SCRYPT_EPARAM;
    /* Guard the products the sizing arithmetic depends on. 128*r*p and 128*r*N
     * are the two that can overflow a 32-bit size on paper; they are computed in
     * 64 bits here and the caller's buffer is checked against the 64-bit value,
     * so an overflow becomes a refusal rather than a short buffer. */
    uint64_t need = scrypt_scratch_bytes(n, r, p);
    if (!scratch || scratchlen < need) return SCRYPT_ENOMEM;

    const uint32_t words = 32 * r;                    /* u32 per 128*r block */
    uint32_t *B  = (uint32_t *)scratch;               /* p * 128*r           */
    uint32_t *V  = B + (uint64_t)words * p;           /* N * 128*r           */
    uint32_t *XY = V + (uint64_t)words * n;           /* 2 * 128*r           */

    scrypt_pbkdf2_sha256(pw, pwlen, salt, saltlen, 1,
                         (uint8_t *)B, 128 * r * p);
    for (uint32_t i = 0; i < p; i++)
        ro_mix(&B[(uint64_t)i * words], V, XY, r, n);
    scrypt_pbkdf2_sha256(pw, pwlen, (const uint8_t *)B, 128 * r * p, 1,
                         out, outlen);
    return SCRYPT_OK;
}
