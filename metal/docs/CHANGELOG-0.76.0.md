# OutRun OS v0.76.0-metal — the milestone about evidence

Milestone 76. v0.75 was about **identity**: a slot index is not an identity, a
pid is not a slot, a socket handle is not a socket. v0.76 is about **evidence**,
and it was defined by what the previous tag deliberately did not claim.

Nothing in this release adds a feature. Every change here is to the machinery
that decides whether the features already shipped actually work — the suites,
the gate configurations, the timing budgets, and the release artefact itself.
That is the point of the milestone, and it is why the most interesting entries
below are defects in the *measuring instruments* rather than in the kernel.

## THE SUITE SET WAS NOT IDEMPOTENT, AND NOTHING HAD EVER NOTICED

v0.75 shipped persistence, which makes "the volume already has state" a
supported configuration. The suite set could not pass on one.

Booting any v0.75 kernel twice on a single image produced three failing
assertions on the second boot — two in `usersstrs`, one in `vfsstrs`. A negative
control (the merged `main` kernel from before the persistence branch) produced
the identical three, so this was **pre-existing**, not a v0.75 regression. It had
been invisible for the project's whole history for one reason: every gate
harness built a fresh disk per boot, so nothing had ever booted twice on one
volume.

Both were **test defects, not kernel defects**, and both were one fixture
assuming it was being created for the first time:

- `usersstrs` created `m72own` with `creat=1` and then asserted the file carried
  the default mode. On a re-used volume the file already existed, carrying the
  mode *this same suite's later assertions had changed it to*. Two failing
  assertions out of one stale fixture.
- `vfsstrs` proved the journal was deferred by showing the on-disk dirent did
  not yet match the in-memory one. On a re-used volume the dirent already held
  that exact payload — and content addressing makes the identical hash follow
  *deterministically*, not luckily — so the precondition could not hold and the
  assertion was defeated by its own fixture.

Characterisation ran **before** any fix, and answered the question it existed to
answer: the failure set is exactly three, and the `boot 2 -> boot 3` diff is
empty in both directions. The volume reaches a fixed point after the first dirty
boot, so the defect is deterministic rather than accumulative, and a fix for the
three could be verified without fear of uncovering a fourth.

**The fixes.** `usersstrs` resets `m72own` before creating it, so "newly
created" is true again. `vfsstrs` varies its fixture content per boot — using
`rdtsc`, deliberately not `g_ticks`, because two boots reach that line at a
similar tick count and a fixture that is only *usually* unique would convert a
deterministic failure into a flake, which is strictly worse.

**The allow-list is enforced, not documented.** `suite_fixture_reset()` refuses
`vfs-reboot-test`, `/etc/udb.a`, `/etc/udb.b` and `udbreboot` out loud, and
counts its refusals. This is mechanical because the failure mode is silent: a
blanket sweep would delete the only cross-boot evidence in the tree, every run
would still look green, and the evidence would be gone while we believed we had
tidied up.

## A DIRTY-VOLUME GATE CONFIGURATION

`tools/gate-dirty.sh`, wired to `make gate-dirty` and `make gate-dirty-smp`:
N boots on **one** image that is never recreated. Boot 1 runs the suites and
then types `udbpersist` and `vfscrashwrite` at the serial console to create the
two deliberately durable artefacts — in that order, because `vfscrashwrite`
halts the machine by design to simulate power loss. Boots 2..N must reach the
prompt with zero failing assertions **and** still find both artefacts.

The harness asserts that boot 1 *created* them, not merely that later boots find
them. Without that check, console input failing to land would surface as "did
not survive" on boot 2 — the wrong diagnosis, on the wrong boot, and the
expensive kind to chase.

Two harness defects found while building it, both recorded because neither was
about the kernel:

1. **`mkfifo` is not portable to this working tree.** Driving the console
   through a named pipe under `build/` fails outright when the repo lives on
   `/mnt/c` — DrvFs under WSL cannot create FIFOs at all. It worked from a
   Linux-side path, so this would have been invisible to anyone not using a
   Windows-side checkout.
2. **Two concurrent runs shared one output file**, and the merged log showed six
   boot lines for a three-boot run — including a PASS reported before anyone
   noticed. The workdir now carries the invoking shell's PID and the script
   takes an `flock`.

## CARRYOVER 2 — A SPIN COUNT IS NOT A TIMEOUT

Two inherited premises were wrong, and both had to go before the fix could be
right.

**"exit 970" was never the failure code — it is the SUCCESS code.** The
assertion reads "...validated the language program (exit 970)" because 970 is
what it *expects*. Carrying "exit 970" forward as the name of the error, which
the v0.75 roadmap did, pointed at the wrong thing entirely. The real failure
codes are 971..992.

**The compile had succeeded.** Reproduced in isolation, the driver exited 984 —
"the compiler TIMED OUT building omake" — in the same boot in which
`/bin/omake` came out at 36562 bytes, well-formed, with the assertion that `occ`
built it *passing*. The work finished; the waiter had already given up.

The defect: `SYS_WAITPID` is **non-blocking** in this kernel and takes no
timeout, returning `-11` while the child runs. So ring 3's
`owaitpid(pid, spins)` budgeted a number of **the waiter's own iterations**,
which is not a duration. Under TCG the waiter's spin rate and the child's
progress rate are unrelated: at `-smp 4` the waiter spins quickly on its own
vCPU while the compiler crawls across four vCPUs multiplexed onto the host.
Measured, the omake compile takes **2.8 s**; a quarter of a million
poll-and-yield iterations were exhausted inside that window. That is why it
failed **2 of 2** rather than flakily — and it also rules out the obvious fix,
since scaling the spin count by `ncpu` would be arithmetic on a quantity with no
time meaning.

`owaitpid_ticks(pid, budget, spent)` polls the child but checks a **real-time
deadline** against `g_ticks` (100 Hz, via `SYS_SYSINFO`), sampling the clock
every 256 polls because `SYS_SYSINFO` walks the process table and checking it as
often as the child would make the waiter the expensive half of the wait. A tick
budget means the same thing at 1 vCPU and at 4, so it needs no per-core scaling
factor. `owaitpid()` itself is deliberately unchanged, so a dozen unrelated
callers do not have their budgets silently reinterpreted into a new unit.

All 14 `owaitpid()` call sites were enumerated — no truncated search — and the
six belonging to `toolstrs` (role 38) and `pipestrs` (roles 40/41) converted.
Beyond the unit change, both `pipe_worker` waits had **collapsed a timeout into
a wrong answer**: a hung child and a broken pipe both exited 957, "the pipe did
not survive fork inheritance", which would send a reader after a pipe bug that
does not exist. They now report 965, and the kernel decoders print "a child WAIT
TIMED OUT (deadline, not a pipe defect)".

**`oputu()`** was added alongside: ring 3 previously had **no way to print a
number**, which is why every timing figure was reported only as an exit code,
and part of why this defect survived — the data needed to see it could not be
emitted.

## THE RELEASE PROTOCOL, AND THE DEFECT THAT FORCED IT

`VERSION` in `metal/Makefile` still read `0.74.0` when `v0.75.0` was tagged, so
every artefact produced for that release — including the ISO its gate signed off
— was named `outrun-os-0.74.0.iso`.

This release adds `make release-iso` / `make release-verify` / `make release`,
a `release-version-check` that compares `VERSION` against `git describe --tags`
and warns loudly on a mismatch, and `CLAUDE.md`, which states the protocol as a
requirement rather than a habit: **a release is not a tag, it is a tag plus an
ISO that was built from a clean tree, checksummed, and booted.**

`release-iso` runs `make clean` first, on purpose — an incremental build can
carry an object from a source state that no longer exists anywhere in the tree,
and a published artefact is the worst place to discover that.

## VERIFICATION

The published artefact, built from a clean tree:

```
outrun-os-0.76.0.iso
md5     c672ea65398a919236d1796b1aeb1b7c
sha256  9791630d1a2d3489236e9ea54370009e7bc4c29962f68436148acfcbe6ead8a1
```

**Every configuration below booted that exact image**, and each log carries the
md5 in its first line — the habit adopted mid-v0.75, after a 20-boot
"verification" was discovered to have booted a different ISO than the one it
claimed and its conclusion had to be withdrawn.

### Fresh-image matrix

| configuration | suites | passed | failed | prompt |
|---|---|---|---|---|
| uniprocessor (`make release-verify`) | 45 | 479 | **0** | yes |
| `-smp 4`, SeaBIOS | 45 | 495 | **0** | yes |
| `-smp 4`, q35 + VT-d, `intremap=on` | 47 | 508 | **0** | yes |

The IOMMU column runs **47** rather than 45 because `iommu` and `capdma` are
config-gated on the emulated VT-d unit. That difference is not trivia: it is how
you can tell the target is actually running with an IOMMU rather than having
silently degraded to plain q35.

Logs: `metal/docs/OUTRUN-0.76-boot-{uniprocessor,smp4-bios,smp4-iommu}.log`.

### Dirty-volume gate — three boots on one image, never recreated

| configuration | boot 1 | boot 2 | boot 3 | verdict |
|---|---|---|---|---|
| `make gate-dirty` (UP) | 45 / 0 fail | 45 / 0 fail, 1 reset | 45 / 0 fail, 1 reset | **PASS** |
| `make gate-dirty-smp`, run 1 | 45 / 0 fail | 45 / **1 fail** | 45 / 0 fail | **FAIL** (see below) |
| `make gate-dirty-smp`, run 2 | 45 / 0 fail | 45 / 0 fail, 1 reset | 45 / 0 fail, 1 reset | **PASS** |

Run 2 was a deliberate reproduction attempt for run 1's failure, and it did not
reproduce: `pthreads_smp` reported 6 passed / 0 failed in all three of its
boots. **One failure in six dirty-SMP boots**, on this host, at this load.
Run 2 is also the first dirty-SMP result produced by the *corrected* failure
counter described below, so its zero is a measured zero rather than an unlooked-
for one.

`make release-verify` was re-run against the same published ISO under the
corrected counter as well: 45 suites, 0 failing assertions, RESULT tally 0, the
two counters in agreement, 300 s. Its exit status was captured directly rather
than through a pipeline this time, and it is genuinely 0.

Both durable artefacts were created in boot 1 and found in boots 2 and 3 in
every run, uniprocessor and SMP alike; consecutive-boot diffs empty; refusals
zero. The fixture reset fires only when there is something to reset, and
`vfsstrs` passes with **zero** resets — consistent with its fix being content
variation rather than deletion, which is how we know the two fixes each do their
own work rather than one masking the other.

`gate-dirty-smp` reaching boot 1 clean is the direct payoff of carryover 2: that
boot formats the CAS from scratch under four vCPUs of TCG contention, and before
`owaitpid_ticks` it failed the `langstrs` compile budget **2 of 2**.

## FOUND AFTER THE TAG: THE GATE'S FAILURE COUNTER WAS BLIND

The `gate-dirty-smp` failure above was **not reported by the gate**. It printed
`failing-assertions=0` and `DIRTY-VOLUME GATE: PASS` on a boot whose own output
read:

```
[pthreads_smp] driver exit 945 — pthread_join failed or timed out
[pthreads_smp] FAIL: mutex contention, condvar broadcast and joins all behaved (exit 940)
[pthreads_smp] RESULT: 5 passed, 1 failed
```

Both `release-verify.sh` and `gate-dirty.sh` counted failures with

```
^\[[a-z0-9 ]+\][ ]+FAIL[ ]+
```

which requires a **lowercase** tag and a **space** after `FAIL`. This line
satisfies neither: `pthreads_smp` contains an underscore, and the line reads
`FAIL:` with a colon. Either miss alone was sufficient. The gate could not see
the failure, so it certified the boot.

This is the same class as the stale-handle counter recorded in `CLAUDE.md` — a
counter that nothing prints is not instrumentation — with the sharper edge that
this one *was* printed, and printed a zero that was believed.

**The fix is not merely a wider pattern.** A wider pattern is the same single
point of blindness, moved: the next suite to invent a tag shape can defeat it
again. Both harnesses now compute a **second, independent** count from a
different line entirely — the suites' own `RESULT: N passed, M failed` tallies —
and **disagreement between the two counters fails the gate on its own**, as a
defect in the instrument rather than a verdict on the run.

Validated against the nine boot logs the first pass produced: the corrected
counter flags exactly the one boot the old one missed, agrees with the
independent tally on all nine, and manufactures no failure anywhere else. It
then ran live — on a second `gate-dirty-smp` run and on a re-run of
`make release-verify` — where both counters agreed at zero on every boot.

These harness changes landed **after** the `v0.76.0` tag. `metal/tools/` is not
compiled into the image, so the published ISO and its checksums are unaffected —
the artefact above is still the artefact that was booted.

## WHAT THIS RELEASE DOES NOT DO

Stated plainly, in the tradition of v0.67, v0.68, v0.72 and v0.74, so the next
milestone inherits a list rather than a discovery.

- **Carryover 3 — the fork race — was not addressed at all.** v0.74 found an
  intermittent `posixstrs` failure under `-smp 4`; v0.75 found and fixed three
  real defects on the path (non-atomic `g_next_pid`, `ppid_slot` with no
  generation, identity syscalls using raw slots). Every configuration is green.
  But the original symptom was **never reproduced without instrumentation**, so
  the clean runs still do not distinguish "fixed" from "did not fire" — twelve
  clean runs were also obtained with the fix reverted. The three defects are
  real and are provable by reading. **The causal link from them to the v0.74
  symptom remains an argument, not a measurement.** The v0.76 roadmap set a
  condition — reproduce it, or retire the claim in the changelog in those words,
  but do not carry it silently into a third milestone. It is carried here
  **explicitly**, unaddressed, and it is now the oldest open item in the
  project.
- **`pthread_join` is still a spin count, not a deadline**, and its caller
  collapses a timeout into `EXIT(945)` — "pthread_join failed **or** timed out"
  — the same conflation #76 fixed for `pipestrs`. It was observed **once in six
  dirty-SMP boots** — boot 2 of run 1, not reproduced by a deliberate three-boot
  repeat — and does not appear in any of the 150 committed boot logs in this
  tree. The failing log is committed at
  `metal/docs/OUTRUN-0.76-gate-dirty-smp-boot2-pthreads_smp.log`, md5-stamped
  with the image it booted. **Carried to v0.77 as a carryover item**, alongside
  `compilerstrs` — same idiom, same conversion to `owaitpid_ticks`-style
  deadlines, and the same requirement that a deadline report itself distinctly
  from a defect.

  > **CORRECTED IN v0.77, BY MEASUREMENT.** This entry originally stated that
  > the spin count *was the mechanism* behind the failure, and described
  > `SYS_THREAD_JOIN` as non-blocking. Both were wrong, and both were inferred
  > by analogy with carryover 2 rather than measured — the exact move this
  > milestone exists to distrust. `SYS_THREAD_JOIN` **parks** the caller
  > (`block_ring3`, `FUTEX_DEFAULT_TICKS` = 20000 ticks = 200 s). The preserved
  > log shows the posixstrs breadcrumb reaching **+19802 ticks** before the
  > driver exited, and that boot took **425 s** against 260 s and 220 s for its
  > siblings: the kernel's own park deadline expired inside a **single** call.
  > The 20000-iteration loop never went round twice and had nothing to do with
  > it. What is real in the entry above is the *conflation* — a deadline
  > reported as a join defect — which is what v0.77 fixes. The unexplained part
  > is now sharper, not softer: **a wake was lost, and why is unknown.**
- **`compilerstrs` is still spin-based** (`cs_compile`, plus a direct wait). It
  is compile-heavy — the same risk class as `langstrs`, which started all of
  this — and remains the highest-risk unconverted site. Carried to v0.77, where
  it and `pthread_join` are the two open instances of the same idiom.
- **The `[mcpre]` anomaly is unexplained, and its evidence is gone.** One
  failure ("long probe never started on cpu1") in 277 boot logs on this host,
  never reproduced, not claimed to be pre-existing. `ROADMAP-0.76.0.md` records
  the failing log as "preserved at `mcpre-evidence/smp-6.log`" — **that file is
  not in the tree and was never committed on any branch.** It lived in a working
  directory that no longer exists. The finding therefore now rests entirely on
  its own prose summary, which is exactly the position this project's evidence
  conventions exist to prevent. Preserved means committed.
- **No memory-hard KDF.** PBKDF2-HMAC-SHA-256 (c=4096) buys serial CPU cost
  only. Argon2id or scrypt remains a v0.77+ item, to be landed against published
  vectors as its own verified unit before being wired in.
- **No password-change or account-deletion syscalls**, so the
  revert-to-an-old-password case that the per-segment nonce defends against is
  still not reachable from ring 3.
- **Lockout does not survive a reboot**, deliberately: naive persistence makes
  every failed authentication a visible write, and an observer counting writes
  counts failed logins.
- **No confidentiality for the stored database.** There is no key store, no TPM,
  and nothing to encrypt the image with that does not live on the same volume.
  Explicitly **deferred**, restated here so it is not mistaken for an oversight.
- **No execute permission bit, no directory permissions, no supplementary
  groups, no login program, no lockout expiry, no administrative unlock.**
  Carried unchanged from v0.74.

## COVERAGE THIS GATE DID NOT PROVIDE

A gate whose gaps are invisible is how "verified" drifts away from "measured".

- **Bare metal and Proxmox were not tested.** Every result above is QEMU, TCG,
  no KVM.
- **One boot per fresh-image configuration.** These are not soak runs; an
  intermittent failure at low rate would not be expected to appear.
- **Three boots per dirty configuration**, not more. Convergence is established
  between boots 2 and 3 and is not proven beyond three.
- **No negative control for the `pthreads_smp` failure.** A three-boot repeat
  did not reproduce it, which is not the same as a control: nothing was run with
  the suspected mechanism removed, and six boots is far too few to bound the
  rate of something seen once. It is unexplained, and it is not claimed to be
  pre-existing.
- **The first pass captured `make gate-dirty*` exit status through a pipeline**,
  so `$?` was the exit status of `tail`, not of `make`, and those two runs'
  reported statuses mean nothing. The authoritative record for them is the
  script's own verdict line — printed only on `rc == 0`, immediately before
  `exit $rc` — and, for the run that mattered, the boot logs themselves, which
  is how the missed failure was found at all. The re-verify and the second
  dirty-SMP run redirected to a file instead, so their statuses are genuine.
