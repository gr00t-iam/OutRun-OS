# A memory-hard KDF for OutRun OS — design

v0.80, Phase 2. Decides the algorithm, fixes the parameter profiles, and states
the memory contract. **Nothing here is wired into authentication yet**, which is
the point: the primitive is landed and verified as its own unit first.

## THE PROBLEM

`udb_kdf()` is PBKDF2-HMAC-SHA-256 with c=4096 (v0.75). PBKDF2 buys **serial CPU
cost only**. Its working set is a few hundred bytes, so an attacker with a GPU or
an ASIC runs thousands of candidates in parallel at a cost per candidate far
below the defender's. Raising c raises both sides equally and never closes that
gap. Memory-hardness is the property that does: force each candidate to occupy
megabytes for the duration, and parallelism starts costing silicon area.

This has been named "the largest structural gap" in every changelog since v0.76.

## THE CHOICE: scrypt (RFC 7914), NOT Argon2id

**Argon2id is the better primitive.** It is the Password Hashing Competition
winner, it is the current recommendation, and it resists the tradeoff attacks
scrypt is theoretically weaker against. If this were a hosted system with a
library available, it would be the answer and there would be nothing to discuss.

It is the wrong choice **here**, and the reason is verification surface rather
than cryptography:

| | scrypt | Argon2id |
|---|---|---|
| new primitives needed | Salsa20/8 (~40 lines) | BLAKE2b (~250 lines) + Argon2 core |
| reuses in-tree, already-verified code | SHA-256, HMAC-SHA-256 | none |
| published vectors | RFC 7914 §11 | RFC 9106 |
| new code requiring its own vector campaign | one small permutation | a full hash function, then the KDF on top |

This kernel is one 25,000-line C file built freestanding, and every primitive it
gains has to be verified in it, by this project, with no library to lean on. The
SHA-256 and HMAC it already has were landed against vectors and are exercised on
every boot. scrypt is *composed* of those plus one small permutation; Argon2id
means introducing a second hash function, validating **that** against its own
vectors, and only then starting on the KDF.

**scrypt is a genuine improvement over the status quo** — it is memory-hard,
which PBKDF2 is not — and it is reachable now with a verification story this
project can actually complete. Argon2id remains the better end state and should
be revisited if BLAKE2b ever arrives for another reason.

Recorded plainly so the tradeoff is not mistaken for a claim that scrypt is
superior: **it is not. It is the stronger primitive this tree can verify today.**

## WHAT THE FREESTANDING CONSTRAINT ACTUALLY COSTS

- **No libc.** `crypto/scrypt.c` uses no string.h, no allocator, no floating
  point. Its only external symbol is the injected PBKDF2.
- **No allocation inside the primitive.** The caller passes scratch and its size;
  the function refuses if it is short. A credential path that can fail for
  allocation reasons reports two different failures as one, and the failing case
  holds memory while an attacker retries.
- **No blocking, no locks, no interrupts required.** It is pure arithmetic over
  the caller's buffer, so it can run with interrupts either way and holds nothing
  anyone else needs.
- **Little-endian assumed.** RFC 7914's Salsa20/8 is defined over LE words and
  this kernel is x86-64 only, so words are loaded directly. A big-endian port
  needs byte swaps at every load and store; the code will look right and be
  wrong without them.
- **`N` must be a power of two**, because ROMix indexes V with a mask rather than
  a division. Enforced, and refused with `SCRYPT_EPARAM`.

## PARAMETER PROFILES

Measured on the build host (native x86-64, gcc -O2), via `crypto/scrypt.c`:

| N | r | p | scratch | host time |
|---|---|---|---|---|
| 1024 | 8 | 1 | 1.0 MiB | 3.6 ms |
| 4096 | 8 | 1 | 4.0 MiB | 13.7 ms |
| 16384 | 8 | 1 | 16.0 MiB | 58.7 ms |

**Proposed interactive profile: N=4096, r=8, p=1 — 4 MiB, ~14 ms on the host.**

Reasoning:

- **4 MiB is the point of the exercise.** It is ~10,000x PBKDF2's working set,
  which is where GPU parallelism starts paying for memory instead of arithmetic.
- **p=1.** Parallelism is for defenders with cores to spare. This KDF runs in a
  credential path on a kernel whose scheduler is the thing several past
  milestones went wrong in; p>1 would mean a KDF with a scheduling story.
- **r=8** is RFC 7914's own choice in two of three published vectors, so the
  parameter set stays on well-trodden ground.

**THE GUEST IS NOT THE HOST, AND THIS IS THE OPEN NUMBER.** Every timing above is
native. This project's QEMU is TCG-only with no KVM, and past measurements in
this tree have run 10–50x slower in-guest than on the host. That puts N=4096
somewhere between 0.14 s and 0.7 s per derivation, and N=16384 between 0.6 s and
3 s. `authstrs` performs many derivations per boot, and the boot suite has to
terminate.

**So the profile is proposed, not fixed.** Phase 3 must measure in-guest before
committing, and N=1024 (1 MiB, ~36–180 ms projected) is the fallback if the
interactive profile makes the gate unacceptably slow. Choosing a parameter from
host timings alone would repeat this project's most-repeated mistake: a constant
chosen on an idle machine.

## THE MEMORY CONTRACT

```c
uint64_t scrypt_scratch_bytes(uint32_t n, uint32_t r, uint32_t p);
int scrypt(..., void *scratch, uint64_t scratchlen);
```

- Scratch is **V (128·r·N) + B (128·r·p) + XY (256·r)**. At the proposed profile
  that is 4 MiB + 1 KiB + 2 KiB.
- Sizes are computed and compared in **64 bits**. 128·r·N is 1 GiB at RFC 7914's
  largest published vector; a caller computing that in 32 bits gets a plausible
  small number instead of an obvious refusal.
- `scrypt()` **refuses** rather than truncating: `SCRYPT_ENOMEM` if the buffer is
  short, `SCRYPT_EPARAM` if N is not a power of two or a parameter is zero. There
  is no partial result.

**Who provides the memory, in the kernel:** the caller, from the frame allocator,
before entering the credential path — 1024 contiguous-enough frames at the
proposed profile. Phase 3 must answer two questions this document cannot:

1. **Can the allocator give up 4 MiB at authentication time**, on a machine that
   has already booted a 45-suite battery, without failing or fragmenting into
   something the KDF cannot use?
2. **Is the scratch wiped after use?** It contains password-derived material for
   its whole lifetime. The obvious answer is yes and the obvious implementation
   is a loop the compiler is entitled to delete, which is a real and well-known
   trap for exactly this kind of buffer.

## WIRING IT IN (PHASE 3, NOT DONE HERE)

1. **Generalise `pbkdf2_hmac_sha256()` to multiple output blocks.** The existing
   one emits exactly one 32-byte block — its comment says so: *"INT_32_BE(1): the
   one and only block"*. scrypt needs p·128·r bytes from the first call (1 KiB at
   the proposed profile) and dkLen from the last. This is a contained change: a
   loop over blocks with the block index in the salt suffix. It should be
   re-validated against vectors as part of the same step, because it changes a
   function every stored credential currently depends on.
2. **Point `udb_kdf()` at scrypt.** v0.75 built the structure so this is a change
   to that one function; the claim held once already for the FNV-1a → PBKDF2
   swap and should be tested rather than assumed a second time.
3. **Decide the stored-format migration.** Every existing credential is a PBKDF2
   hash. There is no upgrade path in the record format today, and a KDF change
   without one silently invalidates every account — including any created by the
   `udbpersist` cross-boot artefact the dirty-volume gate depends on.
4. **Measure in-guest and re-choose N if needed** (see above).

## VALIDATION AS IT STANDS

`make kdf-test` — host build, no kernel, no boot:

```
  PASS  sha256("abc") — the scaffolding itself
  PASS  scrypt(N=16 r=1 p=1)      [RFC 7914 §11 vector 1]
  PASS  scrypt(N=1024 r=8 p=16)   [RFC 7914 §11 vector 2]
  PASS  scrypt(N=16384 r=8 p=1)   [RFC 7914 §11 vector 3]
  PASS  N=15 refused (not a power of two)
  PASS  short scratch refused
-- RESULT: 6 passed, 0 failed --
```

The scaffolding is checked before the thing under test, so a broken reference
SHA-256 cannot present itself as a broken scrypt. The two refusal cases are there
because the memory contract is part of the interface: a primitive that overruns a
short buffer is worse than one that is merely slow.

### Not covered

- **RFC 7914's fourth vector (N=1048576, r=8, p=1) needs 1 GiB of scratch** and
  is not run. Stated here rather than silently skipped.
- **The in-guest path is entirely untested** — nothing has run this inside the
  kernel, by design.
- **No timing-attack analysis.** scrypt's access pattern is data-dependent by
  construction; that is what makes it memory-hard, and it also means cache-timing
  side channels are inherent to the algorithm. On this kernel the credential path
  is not shared with untrusted concurrent code today, but that is a property of
  the current system, not of the primitive.
