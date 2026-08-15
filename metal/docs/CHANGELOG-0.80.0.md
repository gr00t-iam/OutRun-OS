# OutRun OS v0.80.0-metal — a memory-hard KDF, and a migration that was tested

Milestone 80. v0.79 explained the `[mcpre]` failure and left three things on the
ledger. Two were cheap — harness hygiene and `-smp 2` — and the third has been
called "the largest structural gap" in every changelog since v0.76.

**`udb_kdf()` is now scrypt.** Every stored credential migrates transparently,
and the migration has a test rather than a hope.

## THE KDF

### Why scrypt, and not because it is better

Argon2id is the stronger primitive and the current recommendation. It is the
wrong choice **here**, and the reason is verification surface rather than
cryptography:

| | scrypt | Argon2id |
|---|---|---|
| new primitives | Salsa20/8 (~40 lines) | BLAKE2b (~250 lines) + Argon2 core |
| reuses verified in-tree code | SHA-256, HMAC-SHA-256 | none |
| new code needing its own vector campaign | one small permutation | a whole hash function, then the KDF |

This kernel is one C file built freestanding with no library to lean on. Every
primitive it gains has to be verified *here*. scrypt composes from the SHA-256
and HMAC this tree already exercises on every boot; Argon2id means landing and
validating a second hash function before the KDF can even start.

**scrypt is a genuine improvement over the status quo** — it is memory-hard,
which PBKDF2 is not, and PBKDF2's few-hundred-byte working set is exactly what
lets a GPU run thousands of candidates in parallel. Recorded as a tradeoff and
not dressed up as a preference: **Argon2id remains the better end state.**

### Verified before it was wired in

`crypto/scrypt.c` was landed and validated against RFC 7914's published vectors
**on a host, with no kernel and no boot**, before a single line connected it to
authentication. `make kdf-test`:

```
  PASS  sha256("abc") — the scaffolding itself
  PASS  scrypt(N=16 r=1 p=1)      [RFC 7914 vector 1]
  PASS  scrypt(N=1024 r=8 p=16)   [RFC 7914 vector 2]
  PASS  scrypt(N=16384 r=8 p=1)   [RFC 7914 vector 3]
  PASS  N=15 refused (not a power of two)
  PASS  short scratch refused
```

The scaffolding is checked before the thing under test, so a broken reference
SHA-256 cannot present itself as a broken scrypt. The refusals are tested
because the memory contract is part of the interface. The same source file is
compiled into the kernel — one implementation, two builds, so the vectors cover
the code that actually runs.

RFC 7914's fourth vector needs 1 GiB of scratch and is **not** run; stated
rather than skipped quietly.

### Parameters, chosen from an in-guest measurement

**N=1024, r=8, p=1 — 1 MiB, ~50 ms per derivation in the guest.**

The host says 3.6 ms at this profile. That number decided nothing. This
environment is TCG-only with no KVM, and a constant chosen on an idle native
machine is the mistake this project has made more than any other — twice
(`owaitpid`'s spin budget, `mcpre`'s 500-tick ceiling) with a milestone spent
each time. So the kernel measures it on **every boot** and prints it:

```
[udb    ] KDF cost: scrypt(N=1024 r=8 p=1, 1027 KiB) 21 ds/4 = ~52 ms each; PBKDF2(c=4096) 5 ds/4
```

Four derivations, not one: `g_ticks` is 100 Hz, and a single ~50 ms derivation
would round to one or two ticks and say nothing about which side of a threshold
it sits on. If the host, the emulator or the parameters change, the number moves
and somebody can see it.

The design document proposed N=4096 (4 MiB). The measurement chose N=1024: at
~14x host-to-guest, N=4096 lands near 200 ms per derivation, and `authstrs`
performs dozens per boot. 1 MiB is still ~10,000x PBKDF2's working set, which is
where the memory-hardness argument actually lives.

### The scratch is static, and that is the design

`scrypt()` allocates nothing; the caller passes a buffer and its size and the
function refuses a short one rather than overrunning it. In the kernel that
buffer is a fixed `.bss` reservation, not frame-allocated.

A credential path that can fail for allocation reasons reports two unrelated
failures as one — and the failing case holds memory while an attacker retries.
A fixed reservation cannot fail and cannot fragment. It costs 1 MiB of memsz
(not filesz; `.bss` is not in the image) and buys a credential path with a
single failure mode.

## THE MIGRATION

Every credential in every image written before this release is a PBKDF2 hash,
and the record format had no version field. Switching the KDF without a
migration silently invalidates every account — **including the `udbpersist`
artefact the dirty-volume gate depends on**. This was not on the list until
Phase 2 went looking.

### It cost nothing on disk

The record layout already reserved `r[81..84)` and `udb_serialize()` explicitly
zeroed it. So the scheme byte goes there, `0` means PBKDF2, and **every image
ever written already says "PBKDF2" in exactly the right place**. No format
change, no header version bump, no risk to stored databases.

### Verification uses the record's own scheme

A hash is only comparable to one produced the same way, so the scheme travels
with the record and `udb_auth()` derives with it. A single global "current KDF"
is precisely how a migration becomes a lockout: every stored credential turns
unverifiable the moment the global flips.

Upgrade happens on **successful login only** — the one moment a stored hash can
be re-derived, because the password is in hand and has just been proven correct.
The old hash is replaced only once the new one exists, so there is nothing to
roll back.

### It has a test, and the test needed constructing

A fresh boot creates every account as scrypt and exercises none of this. So
`authstrs` builds a legacy record deliberately and asserts the whole path:

```
  PASS  a legacy PBKDF2 record can be constructed for the test
  PASS  a PBKDF2-era account still authenticates after the KDF change
  PASS  ...and is transparently upgraded to scrypt by that login
  PASS  the upgraded record still accepts the same password
  PASS  the upgraded record still refuses the wrong one
  PASS  a second login does not upgrade again
```

The third-from-last is the one that earns its place: an upgrade written against
the wrong salt would pass "authenticates" and "was upgraded" and then lock the
user out on their *next* login. Shipping this untested would have meant its first
real exercise was somebody's stored database.

## MULTI-BLOCK PBKDF2

scrypt needs PBKDF2 with arbitrary output — p·128·r bytes from the first call
(1 KiB here) and dkLen from the last. The existing implementation emitted exactly
one 32-byte block; its own comment said so: *"INT_32_BE(1): the one and only
block"*.

It now spans `ceil(dkLen/32)` blocks with the standard big-endian counter. Two
things about the change are worth naming:

- **The 64-byte salt clamp had to go.** The old code copied the salt into a
  68-byte buffer under `if (saltlen > 64) saltlen = 64`. scrypt's second call
  passes B — 1 KiB — as the salt, which would have been **silently truncated to
  its first 64 bytes** and produced a confidently wrong derived key. HMAC now
  streams the two chunks instead of joining them in a buffer.
- **The 32-byte case is asserted identical, not assumed.** `shastrs` checks three
  digests produced by the *old* single-block implementation through the *new*
  multi-block code. That is not ceremony: every credential in every persisted
  image was derived by the old code, and a migration cannot verify a legacy hash
  it can no longer reproduce.

`shastrs` also asserts that `udb_kdf()` does **not** produce the legacy digest —
a wiring mistake leaving it on PBKDF2 would pass every other check and silently
ship the thing this release exists to replace.

## A BUG THIS RELEASE WROTE AND ITS OWN SUITE CAUGHT

`udb_add()` zeroed the record — setting `scheme` to 0, PBKDF2 — and then hashed
with `udb_kdf()`, which is scrypt. A new account was **stored as one scheme and
verified as another**, and nobody could log in. `authstrs` failed on the first
boot with "the right password authenticates and returns the uid".

The fix is not the missing assignment. There is now one `UDB_KDF_CURRENT`, and
`udb_add()` sets `scheme` and computes the hash from that same constant on
adjacent lines. Two places independently naming "the current scheme" is how they
drifted apart, and the second place is the one that was silent.

## HARNESS HYGIENE AND `-smp 2` (PHASE 1)

`.logs/` fixed one hazard in v0.78 — `make clean` destroying evidence — and
created another: runs accumulate, and reading `.logs/gate/matrix-*/` spans all of
them. It misreported twice: a `19 PASS / 1 FAIL` tally whose single FAIL came
from a deliberate reproducer directory, and a v0.79 release check that read the
boot banner as `0.73.0-metal` from a pre-bump run. Twenty directories had
accumulated.

`gate-matrix.sh` now records its run directory in `LAST_RUN`; **`make
gate-summary`** reports that directory and nothing else; old runs are pruned to
`GATE_KEEP` (reproducer directories exempt — they are named evidence, not runs);
**`make clean-logs`** is the explicit purge, and `make clean` still never touches
`.logs/`.

**`-smp 2` joined the matrix.** v0.79's account of `[mcpre]` predicts it cannot
fail at two cores — stealing needs a thief, and the only other core is the BSP
running the suite. v0.79 shipped that prediction explicitly unrun. It now runs on
every gate, and it holds. The 3-assertion difference against `-smp 4` is
accounted for one by one (two `[mcq]` stealing assertions gated on ≥2 APs, one
`[mcpre]` migration assertion gated on ≥3 cpus), each with an explicit SKIP.

## VERIFICATION

```
outrun-os-0.80.0.iso
md5     f7a20202bc43447b0a08f76136eb02aa
sha256  0359da9080959bb3429b77d31203d5c07e33b85261b5e1f55efccd67cae8a661
```

Every configuration booted that image; each log's first line carries the md5 and
the banner reads `bare-metal kernel 0.80.0-metal`.

| configuration | suites | passed | failed | rank faults | boot |
|---|---|---|---|---|---|
| uniprocessor (`release-verify`, published artefact) | 45 | — | **0** | 0 | 300 s |
| uniprocessor (`make gate`) | 45 | 489 | **0** | 0 | 305 s |
| `-smp 2`, SeaBIOS | 45 | 498 | **0** | 0 | 235 s |
| `-smp 4`, SeaBIOS | 45 | 505 | **0** | 0 | 235 s |
| `-smp 4`, q35 + VT-d | 47 | 518 | **0** | 0 | 235 s |
| `make gate-dirty-smp` | 3 boots | — | **0** | 0 | — |
| `make gate-dirty` (UP) | 3 boots | — | **0** | 0 | — |

**11 boots across six configurations, 0 failing assertions, 0 rank faults.**
`make kdf-test` 6/6. Build 0 warnings, `-Wall -Wextra -Werror` clean.

## THE GATE WENT RED FIRST, AND WHAT WAS DONE ABOUT IT

The release runs failed on `smp2-bios` twice, on different assertions:

```
[mcq       ]  FAIL  two or more cores were IN RING 3 SIMULTANEOUSLY (the v0.39 headline)
[threadstrs]  FAIL  threads were dispatched on MORE THAN ONE core
[pthreads_smp] FAIL worker threads were dispatched on MORE THAN ONE core
```

**The configuration added this milestone justified itself immediately.** `-smp 2`
went into the gate in Phase 1 to test a prediction about `[mcpre]`, and inside
three release runs it exposed **one wrong threshold repeated across three
suites** — four assertions in total — that four-core testing had hidden for
eighty milestones:

| assertion | suite | guard was | now |
|---|---|---|---|
| two+ cores in ring 3 simultaneously | `mcq` | `n >= 2` | `n >= 3` + SKIP |
| threads dispatched on >1 core | `threadstrs` | `n > 1` | `n >= 3` + SKIP |
| two+ cores in ring 3 simultaneously | `threadstrs` | `n > 1` | `n >= 3` + SKIP |
| worker threads on >1 core | `pthreads_smp` | `n > 1` | `n >= 3` + SKIP |

The kernel is not at fault in any of them. The guards assumed two cores were
enough to *observe* parallelism, when at two cores it is a race between one AP
wake and one short unit of work — and the pool can be serviced entirely by one
core before the other arrives.

The cause is structural. At two cores the round pushes one probe to cpu1, pings
it, and has the BSP enter ring 3 immediately with its own — so simultaneity
needs one AP wake to beat one short probe, with nothing synchronising them. At
three or more cpus, several probes spread across several APs and overlap is
near-certain.

**Measured: 3 failures across 8 smp2 boots**, on 3 distinct assertions. A
negative control on v0.79.0 passed 4 of 4 at `-smp 2`, which at that rate has
roughly a 1-in-3 chance of missing a failure — so the control does **not**
establish this is new, and it is recorded as inconclusive rather than as
exoneration. Nothing in v0.80 touches scheduling, and the guards it corrected
predate it by many milestones.

After gating, `smp2-bios` ran **three consecutive clean boots**. That is weak
evidence at a 1-in-3 rate and is stated as such: it is the difference between
"passed once" and "passed repeatedly", not proof that a fifth assertion of this
family is not waiting.

Resolved by gating all four at `n >= 3`, with explicit SKIPs at two cores that
**still print what was observed** — not covered should not mean not shown:

```
[mcq    ]  SKIP  concurrency high-water at 2 cpus is a race (one AP wake vs one
                 short probe); observed 2 at once
```

Deliberately **not** resolved by lengthening the probe until the flake hides. The
property is true at two cores; it is simply not demonstrable by this round
without synchronising the participants. Making it deterministic — have the BSP
wait, bounded, for cpu1 to reach ring 3 — is the honest follow-up and is left to
v0.81 rather than done as a behavioural change to a scheduler suite inside a
release commit, under pressure to turn a gate green.

## WHAT THIS RELEASE DOES NOT DO

- **No password-change or account-deletion syscalls.** Still no way to change a
  password from ring 3, so the revert-to-an-old-password case the per-segment
  nonce defends against remains unreachable. Now more pointed: a user whose
  account migrates cannot choose to migrate it, only log in and have it happen.
- **No confidentiality for the stored database.** Salts and digests are readable
  by anyone who can read the volume. What protects a password is the KDF, and
  that is now considerably better than it was.
- **Argon2id is still the better primitive**, and this is not it.
- **No timing-attack analysis of scrypt's access pattern.** It is data-dependent
  by construction — that is what makes it memory-hard — so cache-timing channels
  are inherent to the algorithm.
- **The scratch is not wiped after use.** It holds password-derived material for
  the life of the derivation and is not cleared afterwards. The obvious fix is a
  loop the compiler is entitled to delete, which is a real trap for exactly this
  kind of buffer, and it deserves doing properly rather than quickly.
- **No lockout across reboot, no execute bit, no directory permissions, no
  supplementary groups, no login program, no lockout expiry, no administrative
  unlock.** Carried forward.

## COVERAGE THIS GATE DID NOT PROVIDE

- **Bare metal and Proxmox are untested.** Every result is QEMU, TCG, no KVM.
- **The migration is tested with a constructed legacy record, not a real
  pre-v0.80 image.** Nothing has yet booted a database written by v0.79 and
  logged into it. That is the strongest available test and it is not the same
  thing.
- **The five reproducers are manual builds** — `FORK_FUNNEL_REPRO`,
  `FORK_TIGHT_DEADLINE`, `FUTEX_RACE_REPRO`, `CPU1_STALL_REPRO`, and now the KDF
  vector test's host-only path. Nothing re-checks that they still reproduce what
  they claim.
- **`[mcpre]`'s fix remains evidenced by its reproducer, not by the gate.**
