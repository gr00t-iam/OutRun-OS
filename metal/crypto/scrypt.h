/* scrypt (RFC 7914) — interface, including the memory contract.
 *
 * v0.80. See docs/KDF-DESIGN.md for why scrypt and not Argon2id, and for the
 * parameter profiles. This header is the contract; the two things it exists to
 * make unmissable are that the caller owns the memory and that the PBKDF2
 * dependency is injected. */
#ifndef OUTRUN_SCRYPT_H
#define OUTRUN_SCRYPT_H

#include <stdint.h>

#define SCRYPT_OK       0
#define SCRYPT_EPARAM  -1      /* N not a power of two, or a zero parameter    */
#define SCRYPT_ENOMEM  -2      /* scratch too small for (N, r, p)              */

/* How much scratch (N, r, p) needs, in bytes: V + B + XY.
 *
 * Returned as 64-bit deliberately. 128*r*N is 16 MiB at the interactive profile
 * and 1 GiB at RFC 7914's largest published vector, so a caller that computes
 * this in 32 bits gets a plausible small number instead of an obvious refusal.
 * Compute it here, compare in 64 bits, and let scrypt() refuse. */
uint64_t scrypt_scratch_bytes(uint32_t n, uint32_t r, uint32_t p);

/* Derive `outlen` bytes into `out`.
 *
 * THE CALLER OWNS THE MEMORY. `scratch` must be at least
 * scrypt_scratch_bytes(n, r, p) and must remain untouched for the call. This
 * function allocates nothing, takes no lock, blocks on nothing, and can be
 * called with interrupts either way — it is pure arithmetic over the buffer it
 * is handed. That is deliberate: a credential path that can fail for
 * allocation reasons has two failure modes reported as one, and one of them
 * holds memory while an attacker retries.
 *
 * Returns SCRYPT_OK, or a negative SCRYPT_E* — never a partial result.
 *
 * n MUST be a power of two: ROMix indexes V with a mask, not a division. */
int scrypt(const uint8_t *pw, uint32_t pwlen,
           const uint8_t *salt, uint32_t saltlen,
           uint32_t n, uint32_t r, uint32_t p,
           uint8_t *out, uint32_t outlen,
           void *scratch, uint64_t scratchlen);

/* THE INJECTED DEPENDENCY.
 *
 * scrypt needs PBKDF2-HMAC-SHA-256 with ARBITRARY output length: p*128*r bytes
 * from the first call (1 KiB at the interactive profile) and dkLen from the
 * last. This tree's existing pbkdf2_hmac_sha256() emits exactly one 32-byte
 * block — "INT_32_BE(1): the one and only block" — so wiring scrypt in requires
 * generalising it to multiple blocks. That is a contained change and it is
 * listed in the design document as the first step of Phase 3.
 *
 * Declared rather than implemented here so that this module contains only the
 * new logic, and so the vector test can supply a reference implementation and
 * validate scrypt on a host with no kernel present. */
void scrypt_pbkdf2_sha256(const uint8_t *pw, uint32_t pwlen,
                          const uint8_t *salt, uint32_t saltlen,
                          uint32_t rounds, uint8_t *out, uint32_t outlen);

#endif /* OUTRUN_SCRYPT_H */
