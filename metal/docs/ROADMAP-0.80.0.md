# OutRun OS v0.80.0-metal — roadmap

Milestone 80. v0.79.0 is tagged (`daa3977`). It explained the `[mcpre]` failure
that had been open since v0.76 — a work-stealing race in the test, not a stalled
core — and closed the AP idle window found while looking for it.

v0.79 shipped with three things it explicitly did not do. Two of them are cheap
and are this milestone's Phase 1; the third is the largest structural gap in the
project and is where the milestone should end up.

## OBJECTIVES

1. **Harness log hygiene.** `.logs/` fixed one hazard and created another.
2. **`-smp 2` in the gate.** v0.79 made a falsifiable prediction and did not run
   it.
3. **Design prep for a memory-hard KDF.** Carried since v0.76 as "the largest
   structural gap"; this milestone should at least make it a plan.

---

## PHASE 1 — HYGIENE AND THE TWO-CORE MATRIX

### 1. The `.logs/` glob trap

v0.78 moved gate logs out of `$(BUILD)` and into `.logs/`, because `make clean`
had twice destroyed evidence — a carryover-3 reproduction that had to be re-run,
and the only copy of the unexplained v0.76 `[mcpre]` failure, whose roadmap entry
still points at a path that exists nowhere.

That fix worked and introduced its own hazard: **runs accumulate, and any command
reading `.logs/gate/matrix-*/…` reads several runs at once.** It has produced two
wrong readings already:

- A `19 PASS / 1 FAIL` tally on the lost-wake assertion, where the single FAIL
  came from a **deliberate reproducer** directory that had nothing to do with the
  run being reported.
- A v0.79 release check that read the boot banner as **`0.73.0-metal`** — from a
  matrix directory created before the version bump.

Both were caught, and both were caught *by noticing*, which is not a property to
depend on. Twenty directories were on disk by the end of v0.79.

**The fix, in three parts:**

- `gate-matrix.sh` records its run directory in `.logs/gate/LAST_RUN` and prints
  it in the run header.
- **`make gate-summary`** reports on that directory and nothing else — image
  md5, boot banner, and per-configuration pass/fail and rank faults. Anything
  reporting on a gate should use it instead of a glob.
- The harness prunes older `matrix-*` runs, keeping `GATE_KEEP` (default 3).
  **Reproducer directories are deliberately exempt** — they are named evidence,
  not runs, and the whole point of `.logs/` is that evidence survives.

`make clean` still does not touch `.logs/`. **`make clean-logs`** is the explicit
way to purge it, so discarding evidence is always something someone typed rather
than a side effect of building.

### 2. `-smp 2` joins the matrix

v0.79's account of `[mcpre]` predicts that the failure **cannot occur at two
cores**: work stealing needs a thief, and at `-smp 2` the only other core is the
BSP, which is running the suite rather than stealing from it.

That is a falsifiable prediction, and v0.79 shipped with it explicitly unrun —
listed under "what this release does not do". It is now a standing configuration
rather than a note, so the prediction is tested on every gate.

`smp2-bios` is also the smallest topology in which any cross-core path executes
at all, which makes it worth having independently of `[mcpre]`: several SMP
suites have only ever been exercised with four cores, and "works at 4" is not
"works at N".

### Phase 1 validation

`make clean && make gate`, four configurations:

| configuration | suites | passed | failed | rank faults |
|---|---|---|---|---|
| uniprocessor | 45 | 481 | **0** | 0 |
| `-smp 2`, SeaBIOS | 45 | 494 | **0** | 0 |
| `-smp 4`, SeaBIOS | 45 | 497 | **0** | 0 |
| `-smp 4`, q35 + VT-d | 47 | 510 | **0** | 0 |

**The prediction holds.** `[mcpre]` passes at two cores — the probe reached ring
3 on cpu1 in 0 ticks with `ran_on 2` — and the suite degrades correctly rather
than silently: it prints `SKIP cross-core migration needs >= 3 cpus (resumed on
the same AP)` instead of asserting something it cannot test.

**The 497 -> 494 difference is accounted for, not waved through.** Exactly three
assertions are config-gated below four cores, each with an explicit SKIP:

```
[mcq    ]  SKIP  stealing needs >= 2 APs online          (2 assertions)
[mcpre  ]  SKIP  cross-core migration needs >= 3 cpus    (1 assertion)
```

497 - 494 = 3. A suite count that differs between configurations is exactly the
kind of thing that gets assumed benign and then turns out not to be, so it was
diffed assertion by assertion. (One apparent difference in `[sweep]` was a false
positive: the same assertion, printing a different tick count.)

`make gate-summary` reports on the recorded run alone:

```
  run dir : .logs/gate/matrix-1067
  image   : md5=3b56212da5b1f62b6d97dcc2b2c3a312
  banner  : bare-metal kernel 0.79.0-metal
  smp2-bios      494 passed / 0 failed   rank-faults 0
  ...
```

and the directory holds a bounded number of runs instead of the twenty that had
accumulated by the end of v0.79.

### A Phase 1 mistake worth recording

The first attempt at this ran `cd metal && python3 …` from a shell already in
`metal`. The `cd` failed, `&&` discarded the edit, and the `file` and `bash -n`
checks that followed ran against the **unmodified** script and printed
`SYNTAX_OK`. That was read as confirmation. It was a check that could not fail,
verifying a file nothing had changed — the same shape as every "documented,
believed, and false" defect this project has been finding.

What caught it was the gate: `gate-matrix.sh` rejects an unknown configuration
loudly and fails the run, rather than skipping it. A harness that had quietly
ignored `smp2-bios` would have produced a clean three-config gate and a roadmap
claiming four.

## PHASE 2 — MEMORY-HARD KDF (DESIGN PREP)

Carried since v0.76 and named "the largest structural gap" in every changelog
since. Not started here; the objective for this milestone is a design that can be
implemented against published vectors, not an implementation.

What is already known and should constrain the design:

- **PBKDF2-HMAC-SHA-256 (c=4096) buys serial CPU cost only.** GPUs and ASICs keep
  a large advantage, which is the entire reason this is on the list.
- **v0.75 built the structure so this is a change to `udb_kdf()` and nothing
  else.** That claim held once already for the FNV-1a → PBKDF2 swap. It should be
  tested rather than assumed a second time.
- **Land the primitive against published vectors as its own verified unit before
  wiring it in**, exactly as SHA-256 was. A KDF that is only exercised through
  the login path is a KDF whose failures look like authentication bugs.
- Memory-hardness costs memory, and this kernel's frame allocator is the thing
  that will feel it. Argon2id's parameters must be chosen against what the
  allocator can actually give up, on a machine that also has to boot a
  45-suite battery.

Open question to answer before choosing: **Argon2id or scrypt.** Argon2id is the
better primitive and the larger implementation; scrypt is smaller and composes
from PBKDF2 plus Salsa20/8, one of which already exists here.

## PHASE 2 RESULT — scrypt LANDED AND VERIFIED, NOT WIRED IN

Full reasoning in `KDF-DESIGN.md`. The short version:

**scrypt (RFC 7914), not Argon2id — and not because it is better.** Argon2id is
the stronger primitive and the current recommendation. It is the wrong choice
*here* for verification surface, not cryptography: it needs BLAKE2b (~250 lines,
its own vector campaign) before the KDF can even begin, whereas scrypt composes
from the SHA-256 and HMAC this tree already has and already verifies on every
boot, plus one ~40-line permutation. scrypt is a genuine improvement over
PBKDF2 — it is memory-hard, which PBKDF2 is not — and it is reachable now with a
verification story this project can finish. That tradeoff is recorded as a
tradeoff, not dressed up as a preference.

**Landed as `crypto/scrypt.c` + `crypto/scrypt.h`**, containing only the new
logic: Salsa20/8, BlockMix, ROMix, composition. It allocates nothing, calls no
libc, takes no lock, and declares its PBKDF2 dependency rather than duplicating
one that is already verified.

**`make kdf-test` — host build, no kernel, no boot, seconds:**

```
  PASS  sha256("abc") — the scaffolding itself
  PASS  scrypt(N=16 r=1 p=1)      [RFC 7914 vector 1]
  PASS  scrypt(N=1024 r=8 p=16)   [RFC 7914 vector 2]
  PASS  scrypt(N=16384 r=8 p=1)   [RFC 7914 vector 3]
  PASS  N=15 refused (not a power of two)
  PASS  short scratch refused
-- RESULT: 6 passed, 0 failed --
```

All three runnable published vectors, first try. The scaffolding is checked
before the thing under test, so a broken reference SHA-256 cannot masquerade as
a broken scrypt, and the refusal cases are tested because the memory contract is
part of the interface.

**Two findings that change Phase 3's shape:**

1. **The existing `pbkdf2_hmac_sha256()` cannot serve scrypt.** It emits exactly
   one 32-byte block — its own comment says "INT_32_BE(1): the one and only
   block" — and scrypt needs p·128·r bytes from the first call. Generalising it
   is contained, but it changes a function every stored credential depends on,
   so it needs re-validating in the same step.
2. **There is no stored-format migration.** Every existing credential is a
   PBKDF2 hash and the record format has no version field. Switching `udb_kdf()`
   without one silently invalidates every account — including those created by
   the `udbpersist` artefact the dirty-volume gate depends on. This was not on
   the list before Phase 2 and is now the largest piece of Phase 3.

**Measured, host, native:** 3.6 ms @ 1 MiB, 13.7 ms @ 4 MiB, 58.7 ms @ 16 MiB.
Proposed interactive profile N=4096, r=8, p=1 (4 MiB). **Proposed, not fixed:**
the guest is TCG-only and this tree has measured 10–50x host-to-guest before, so
Phase 3 must measure in-guest before committing. Choosing from host timings alone
would be a constant chosen on an idle machine, which is this project's
most-repeated mistake.

## STILL OPEN (inherited)

- **The `[mcpre]` fix is evidenced by its reproducer, not by the gate.** It fired
  roughly 1 boot in 8; clean gates are consistent with it still being present.
- **The AP idle window is closed but was never observed failing**, so nothing
  measures whether closing it mattered.
- **`SYS_THREAD_JOIN` has no timeout argument.**
- **`sys_epoll_wait`** is documented rather than converted, with the invariant
  and its expiry condition beside the code.
- **The virtio-net BAR assumption** is documented, not repaired.
- **The four reproducers are manual builds** — `FORK_FUNNEL_REPRO`,
  `FORK_TIGHT_DEADLINE`, `FUTEX_RACE_REPRO`, `CPU1_STALL_REPRO`. Nothing
  re-checks that they still reproduce what they claim, so they can rot silently
  between releases. This is now the most likely source of a future wrong
  conclusion, given how much of this project's evidence rests on them.
- **Bare metal and Proxmox remain untested.** Every result is QEMU, TCG, no KVM.
