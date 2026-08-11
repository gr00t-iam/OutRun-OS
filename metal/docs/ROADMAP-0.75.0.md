# OutRun OS v0.75.0-metal — roadmap and race analysis

Milestone 75. Phase 1 is the defect v0.74 found and did not fix: an intermittent
`posixstrs` failure under `-smp 4` on SeaBIOS, in which a forked child reports
the wrong parent through `getppid` and its parent's `waitpid` then times out.

## WHAT v0.74 ESTABLISHED

Recorded in CHANGELOG-0.74.0.md and not repeated in full here. The short form:
eight runs eliminated, each by its own boot rather than by argument, the v0.74
credential model, kproc slot recycling, set-id-on-exec, suite ordering, the host
itself, and binary layout. What survived is that running a costly suite before
`posixstrs` makes the failure appear, with the mechanism unknown.

The symptom, precisely:

```
[posixstrs] round 0 'fork/waitpid/SIGCHLD' (role 29 pid 611) FAILED: exit 702 (want 700)
[posixstrs] round 0 child pid 618 (forked by role 29) FAILED: exit 44
```

From `user/init.c`, `posix_fork_worker()`:

- **exit 44** — the CHILD: `if (ogetppid() != mypid) sysc(SYS_EXIT, 44, ...)`
- **exit 702** — the PARENT: `if (code == -11) sysc(SYS_EXIT, 702, ...)`, where
  `code = owaitpid(child, 30000)` and `-11` is EAGAIN, "still running"

Rounds 0 and 2 fail, round 1 passes. Uniprocessor is clean in every run.

## THREE DEFECTS FOUND BY READING THE PATH

None of these was introduced by v0.74. All three are invisible on a
uniprocessor, which is exactly the observed shape.

### A. pid allocation is outside the lock, and is not atomic

`kproc_spawn`, `kernel/kernel64.c`:

```c
    kproc_unlock();                       /* <-- lock RELEASED here */
    kproc_reset(&kprocs[i]);
    uint64_t pid = g_next_pid++;          /* <-- non-atomic RMW on a plain global */
    if (!g_next_pid) g_next_pid = 1;
    kprocs[i].pid = pid;
```

`g_next_pid` is a plain `static uint64_t` — not `volatile`, not atomic — and the
increment happens **after** `kproc_unlock()`. The comment above this function
explains at length why the *slot* scan-and-claim had to become one atomic
critical section under `g_kproc_lock`; the pid assignment was left outside it.
The thread-spawn path has the same line, also unlocked.

Two cores calling `kproc_spawn` concurrently can therefore read the same
`g_next_pid` and **assign the same pid to two live processes**. On a
uniprocessor the two calls cannot interleave, so this cannot happen; under
`-smp 4` with `posixstrs` starting seven workers per round, plus forks and
threads, concurrent spawns are ordinary.

**This explains exit 702 directly.** `SYS_WAITPID` finds its child by scanning
for a pid and taking the first match:

```c
    for (int i = 0; i < n_kproc; i++) {
        if (!kprocs[i].used || kprocs[i].pid != (uint32_t)a0) continue;
        if (kprocs[i].ppid_slot != me) return (uint64_t)-10;   /* ECHILD */
        if (!kprocs[i].exited)         return (uint64_t)-11;   /* EAGAIN */
        return kprocs[i].exit_code;
    }
```

With a duplicate pid the scan can land on the wrong slot. If that slot is a live
process owned by the same parent, the parent is told `-11` — "still running" —
forever, which is exactly the 702 path, and it would keep saying so for all
30000 polls while the real child had long since exited.

`posixstrs`'s own harness also keys on pid, and at one point clears a parent
link for every slot whose pid matches a recorded child:

```c
    if (kprocs[i].pid == R.cpid[c]) kprocs[i].ppid_slot = -1;
```

Under duplicate pids that line severs the parent link of an **unrelated live
process**, which is a plausible route to a later round's exit 44.

### B. `ppid_slot` is a bare slot index with no generation counter

`SYS_GETPPID` validates only that something occupies the slot:

```c
    int par = kprocs[current_proc_idx].ppid_slot;
    return (par >= 0 && par < n_kproc && kprocs[par].used) ? kprocs[par].pid : 0;
```

A slot that is dead but not yet recycled still has `used == true` (`exited` and
`torn_down` are set; `used` is not cleared), so it passes. Once the slot IS
recycled it holds a different process entirely, and this returns that stranger's
pid with no check that it is still the same process.

`posixstrs` knows about this hazard on the kernel side and works around it, in a
comment that names the problem precisely:

> `ppid_slot` is a SLOT index, and slots are recycled across rounds — a previous
> round's child can therefore point at a slot this round's workers now occupy.
> pids are monotonic, so a genuine child of this worker always has a higher pid
> than its parent; that disambiguates.

**The ring-3 `getppid` path has no equivalent guard.** This is the direct
structural explanation for exit 44 — and note that the workaround above relies
on *pids being monotonic and unique*, which defect A breaks.

### C. the identity syscalls disagree about what "the caller" means

`SYS_GETPID` (case 16) reports the thread-group **leader's** pid, correctly, and
the v0.72/v0.74 credential syscalls all resolve through `tg_of()`. But:

```c
    case 55:  int par = kprocs[current_proc_idx].ppid_slot;   /* raw slot */
    case 56:  int me  = (int)current_proc_idx;                /* raw slot */
```

Both use the raw slot. For a single-threaded process `tg_of(x) == x`, so this is
currently latent rather than active — `sys_fork` also writes the raw slot, so
the two agree today. It is listed because it is the same class of mistake as A
and B (an index used as an identity), and because `posixstrs` runs twelve
threads per round, so the day something forks or waits from a thread it becomes
real.

> **Correction, made while fixing it:** "latent rather than active" was wrong.
> `sys_fork`'s own guard, `if (kprocs[par].nthreads > 1) return -11;`, reads the
> raw slot as well — and `kproc_spawn_thread` sets a thread's own `nthreads` to
> **0**, because the leader holds the count. On a thread the test was therefore
> `0 > 1`, false, so a thread of a multi-threaded process walked straight past
> the refusal that exists to stop it and forked a child parented to a thread
> slot. Nothing exercised it only because nothing in the tree forks from a
> thread yet; it was reachable from ring 3 the whole time. See the PHASE 1
> RESULT section for what was done.

## INSTRUMENTATION PLAN

Ordered so that each step either proves or kills a hypothesis before any fix is
written. The point is to make the failure *explain itself* in the log, not to
guess correctly.

### Step 1 — prove or kill duplicate pids (cheap, decisive)

In `kproc_spawn`, immediately after `kprocs[i].pid = pid`, scan for any other
used slot carrying the same pid; count into a global and `kprintf` the slot pair
and pid. Add the same check to the thread-spawn path.

Add a counter dump to `posixstrs`'s summary so it appears next to the failures.

**Do NOT fix the allocation yet.** Running the detector alone on smp4-bios
answers "does this actually happen?" — and if duplicates appear in the same boot
as the failures, defect A is confirmed rather than merely plausible.

### Step 2 — make the ring-3 failure print its own evidence

`posix_fork_worker` currently exits 44 with no context. Have the child print
`mypid`, the value `getppid()` returned, and its own pid before exiting, and
have the parent print what `waitpid` returned and for which pid. This converts
"exit 44" into two integers that identify the confusion directly, and costs one
boot.

### Step 3 — a generation counter, instrumented before enforced

Add `uint32_t gen` to `struct kproc`, bumped in `kproc_reset`, and store the
parent's generation alongside `ppid_slot`. First **only log** when `getppid` or
`waitpid` resolves a slot whose generation no longer matches — that measures how
often stale parent links are actually being followed, before any behaviour
changes.

### Step 4 — fix, then re-run the same instrumentation

Expected fixes, in order of confidence:

1. Move the pid assignment inside the `kproc_lock()` critical section, or use
   `__sync_fetch_and_add`. This is correct regardless of whether it turns out to
   be the trigger — a non-atomic shared counter handing out identities is a bug
   on its own terms.
2. Make `ppid_slot` a (slot, generation) pair and have `getppid`/`waitpid`
   return "no parent" / ECHILD when the generation is stale.
3. Route both through `tg_of()` for consistency with `getpid` and the credential
   syscalls.

Then re-run with the Step 1 and Step 3 counters still in place: the fix is
demonstrated when the counters read zero *and* the suite passes, not when the
suite merely passes.

### What is deliberately NOT being instrumented, and why

The brief suggested lock ranking, spinlock hold times, and APIC IPI delivery.
The evidence does not point at any of them, and instrumenting them would cost
boots at ~1 hour each:

- **Lock ranking** — `posixstrs` already asserts "no lock-rank violation anywhere
  in the POSIX paths" and that assertion **passes in every failing run**. The
  mechanism is already instrumented and already says the locks are ordered.
- **Spinlock hold times** — the failure is a wrong *value* (a pid that does not
  match), not a stall or a timeout in the kernel. `waitpid` is non-blocking by
  design and returns promptly; it returns the wrong answer.
- **APIC/IPI delivery** — the child demonstrably *runs* (it reaches ring 3, makes
  syscalls, and exits with a specific code). Delivery is working; identity
  resolution is not.

They stay available if steps 1–3 come back empty.

## PHASE 1 RESULT — WHAT WAS ACTUALLY FOUND

The instrumentation plan above was followed and it did not land where the
analysis predicted. This section is the correction; the analysis is left intact
above rather than rewritten, because what it eliminated is as much the result as
what it found.

### The defect: fork enqueued every child onto cpu 0

`sys_fork` ended with a hardcoded preferred core:

```c
    rq_push_any(0, ch);          /* was */
    rq_push_any((int)cpu_idx(), ch);   /* is */
```

`rq_push_any` falls through to a sibling only when the preferred queue is
**full**, not when it is merely busy. Every forked child in the system was
therefore funnelled into cpu 0's run queue no matter which core called `fork` —
while its parent yielded on a different core, draining a different queue. On a
uniprocessor cpu 0 is the only core, so the constant was always right and the
funnel was invisible; under `-smp 4` it is a queue the waiting parent never
touches.

Two independent reasons this line was wrong regardless of the race:

1. **Locality.** The child's address space was just cloned on the forking core
   and the parent — the one task certain to care — is running there.
2. **Affinity.** A child inherits its parent's affinity mask
   (`kprocs[ch].affinity = kprocs[par].affinity`), and `rq_push_any` does **not**
   check affinity for its *preferred* core, only for the siblings it falls back
   to. A parent pinned to cpu 2 forking a child that inherits that pin had the
   child pushed to cpu 0 — outside its own mask. The forking core is by
   construction inside the mask, so the new preference is affinity-correct where
   the constant was not.

`rq_push_any((int)cpu_idx(), p)` is already the idiom used by the futex wake
path (`kernel/kernel64.c`, the `rq_push_any` call in the futex requeue); this
change makes `sys_fork` consistent with it.

### Status of defects A, B and C

- **A (non-atomic `g_next_pid`)** — a duplicate-pid detector was added per step 1
  and observed **zero collisions across a full failing boot**. Not the cause.
  **FIXED** on its own terms: both spawn sites now call `pid_alloc()`, a single
  `__sync_fetch_and_add`. The old wrap guard (`if (!g_next_pid) g_next_pid = 1;`)
  was itself racy — two cores wrapping together would both store 1 and both hand
  out 1 — and is replaced by a per-caller retry that skips a zero without ever
  writing the counter back.
- **B (`ppid_slot` has no generation counter)** — a generation counter was added
  per step 3 and **confirmed stale resolution happens** (slot 16, gen 316→317,
  while a live child still pointed at it). It fires only *after* the real parent
  has already exited, which makes it a symptom of the timeout rather than its
  origin. **FIXED**: the parent link is now a (slot, generation) pair. `gen` is
  bumped by `kproc_reset` on every recycle, `ppid_gen` records the parent's
  generation when the link was made, and `ppid_live()` is the only sanctioned
  resolver — every reader goes through it, including the three signal-authority
  checks (`SYS_KILL`, `SYS_SETPGID`, `SYS_KILLPG`), where a stale link was worse
  than a wrong `getppid` because it granted a task authority over a stranger that
  merely inherited its dead parent's slot. `posixstrs` drops its pid-monotonicity
  workaround and asks the kernel directly — that workaround depended on pids
  being unique, i.e. on defect A never firing.
- **C (raw slot vs `tg_of()`)** — **FIXED**, and it was not latent. Every
  identity decision on the fork/wait/signal paths now resolves through `tg_of()`,
  matching `getpid` (case 16), `fd_owner()` and the credential accessors:
  - `sys_fork` resolves `par` to the leader immediately after its bounds check,
    so the `nthreads > 1` refusal reads the count from the slot that holds it.
    This was the live bug — on a thread the test was `0 > 1` and a threaded
    process could fork after all. Everything else `sys_fork` copies is
    process-level and now comes from the leader too; the child's registers still
    come from `sf`, which is correctly per-thread.
  - `SYS_GETPPID` and `SYS_WAITPID` resolve the caller to its leader, so any
    thread can reap the process's children. `waitpid` additionally matches only
    thread-group leaders: a thread's pid is distinct and carries the leader's
    inherited parent link, so without that filter a thread's exit could be
    reported as a child process ending. Threads are joined, not waited on.
  - `SYS_KILL` resolves both caller and target to their leaders, as
    `SYS_SETPGID` and `SYS_KILLPG` already did.
  - `posixstrs`' child accounting counts leaders only, so a child that created
    threads is one child rather than several.

  **What this does NOT do:** fork from a multi-threaded process is still
  refused, and now refused correctly rather than by accident of which slot was
  asked. Allowing it is a separate feature — POSIX gives the child only the
  calling thread — and is not what defect C describes.

The scaffolding for both A and B was removed rather than left half-wired; the
findings are recorded in comments at `struct kproc` and at `g_next_pid`.

### VERIFICATION — AND WHAT IT DOES NOT SHOW

Read this section before citing the clean matrix as evidence the race is cured,
because it is not.

Twenty-one boots, all reaching the shell prompt, each from a freshly created
disk image. Two builds, differing **only** in the one line above (kernel ELF
md5 `09377b34…` fixed, `03a92f9f…` reverted):

| build | runs | posixstrs |
|---|---|---|
| fixed | 9 (5 smp4-bios, 3 smp4-iommu, 1 uni) | 12/12 every run (11/11 uni) |
| reverted (negative control) | 12 (10 smp4-bios, 2 smp4-iommu), 6 of them with the host oversubscribed 6x | 12/12 every run |

Three-config matrix on the fixed build, uncontended, **0 FAIL**:

| configuration | suites | failed |
|---|---|---|
| uniprocessor | 44 | **0** |
| `-smp 4`, SeaBIOS | 44 | **0** |
| `-smp 4`, q35 + VT-d | 46 | **0** |

`authstrs` is inside those counts — it is back in the boot sequence, which was
the acceptance test Tier 1 asked for.

**The negative control did not reproduce the defect.** With the fix reverted and
`authstrs` in the boot sequence — the configuration v0.74 recorded as failing
roughly 3 runs in 4 — posixstrs passed 12 of 12 assertions in all twelve runs,
including six under 6x host oversubscription deliberately intended to slow the
guest the way v0.74's slow host did. A suite that does not fail without the fix
cannot certify the fix. The clean matrix above establishes **no regression**, not
a cure.

The one measurement that does support the causal claim was taken on an
INSTRUMENTED build: enqueue-to-first-dispatch showed failing children waiting
past the parent's 30000-poll `waitpid` budget, and 0–2 ticks once the preference
was corrected, with posixstrs going 4 failures → 0. v0.74 established that this
failure is sensitive to binary layout and host speed, and instrumentation is
itself ~8KB of perturbation — so the instrumented build reproducing while the
uninstrumented one does not is *consistent*, but consistency is not proof.

Honest status: **the enqueue was wrong on its own terms and is now right; the
causal link to the posixstrs race rests on a single instrumented measurement and
has not been reproduced uninstrumented.**

### TWO PRE-EXISTING DEFECTS THIS EXPOSED (neither caused by the fix)

Both were reproduced on the **reverted** build under the same load, which is what
rules the fix out as their cause. Both need only a sufficiently slow host, and
neither appears in any committed v0.73/v0.74 log (all of which show zero).

1. **`g_net_lock` is re-entered under load.** `[klock] RANK VIOLATION: acquiring
   'net' (rank 9) while holding rank 9`, in bursts (84 and 976 occurrences on the
   fixed build; 873 on the reverted one), clustering at the TCP retransmission
   test and failing `[tcpstrs] no lock-rank violation across the TCP paths`. Two
   paths take `g_net_lock` at top level — `net_rx_tcp` (NIC bottom half) and
   `tcp_timer_scan` (idle path) — and each documents that it must be entered
   without the lock held. One violation is a rank *inversion* (`ofile` rank 1
   taken while holding rank 9), consistent with re-entering an fd path from
   inside the net lock. Only observed on `-smp 4` + VT-d so far.

   > **Escalated, measured during the Tier 2 work.** It is no longer
   > contention-only and no longer IOMMU-only: on `-smp 4` SeaBIOS with a single
   > guest and an idle host it now reproduces in **3 runs out of 5**. Ten runs,
   > alternating the Tier 2 build against `origin/main` so any host drift hit
   > both arms equally, gave the SAME distribution for each: 1 clean, 3 with the
   > tcpstrs rank-violation failure, 1 panic (below). So it is squarely in
   > `main` and nothing in Tier 2 touches it — but it means **`main` currently
   > does not meet the release gate on this configuration**, which matters more
   > than where it was first seen. Whatever changed is host- or timing-related,
   > since the same tree ran this config clean five times earlier in the
   > milestone.
2. **Toolchain suites time out under contention.** `[langstrs] 8 passed, 2
   failed` (`exit 970`, the driver's compile-run-validate step), plus
   `[toolstrs]` and `[pipestrs]` failures at 6x oversubscription with the
   explicit message `TIMED OUT waiting for the compiler`. These are wall-clock
   budgets in the self-hosting suites, not correctness failures, but they are
   what will break a regression run on a loaded machine.

3. **An intermittent page-fault panic late in the `-smp 4` boot.** Found during
   the Tier 2 work, on `origin/main` and on the Tier 2 branch alike, once each
   across five runs of each build:

   ```
   [panic ] CPU EXCEPTION 14: Page Fault (err=0) at rip=0000000000122cb8
   [panic ] system halted — the fault was contained to this core
   ```

   It lands after ~38 suites, around the final ring-3 `user_init.elf` stage, and
   the guest halts without reaching the prompt — so a run that hits it is not a
   failing matrix, it is an ABSENT one, and any harness that waits for the shell
   banner will sit there until its timeout rather than reporting anything. `err=0`
   means a not-present page on a read, in kernel context. Not diagnosed. This is
   the most serious open item in this file: the other two produce a wrong answer,
   this one stops the machine.

Neither the net-lock re-entrancy nor the toolchain budgets is in scope for the
fork fix, and none of the three is fixed here.

## MILESTONE PLAN

### Tier 1 — the race, and restoring the suite

> **Status:** steps 1 and 2 are done and step 4 is done (`authstrs` is back in the
> boot sequence, the `AUTH_ITER` hook is gone, and the three-config matrix is at
> 0 FAIL). Step 3 is **NOT satisfied**: three consecutive clean smp4-bios runs
> were obtained, but so were twelve clean runs with the fix reverted, so the runs
> do not distinguish "fixed" from "did not fire". See PHASE 1 RESULT above. What
> Tier 1 still needs is a configuration that reproduces the failure without
> instrumentation — the most likely lever, per v0.74, is a slower host or a
> different binary layout, not more runs on this one.

1. **Instrument** per steps 1–3 above. One boot per step on smp4-bios; the
   failure reproduces in roughly 3 runs out of 4 with `authstrs` in the boot
   sequence, so **re-enable `authstrs` in the boot path for the duration of the
   investigation** — it is the most reliable trigger currently known, which
   makes it a tool rather than a nuisance.
2. **Fix** per step 4.
3. **Verify** across at least **three consecutive clean smp4-bios runs** before
   calling it fixed. v0.74 recorded, at cost, that a single passing run does not
   distinguish "fixed" from "did not fire this time" — that error is not worth
   repeating.
4. **Restore `authstrs` to the regression boot sequence**, delete the
   `AUTH_ITER` hook's justification comment, and confirm the full three-config
   matrix at 0 FAIL. Restoring the suite is the actual acceptance test for the
   fix: it is the only configuration known to trigger the race.

### Tier 2 — auth's structural gaps

5. **SHA-256, with published test vectors.** CHANGELOG-0.74.0.md is explicit that
   FNV-1a is not a cryptographic hash and that this is the single largest gap in
   the release. Land the primitive against NIST vectors as its own verified unit
   *before* wiring it into the KDF, so a failure is attributable to one or the
   other. `udb_kdf()` is the only function that changes; the schema, both
   syscalls, the lockout and all 35 `authstrs` assertions stay as they are —
   that was the stated point of building the structure first.

   > **Status: DONE.** Landed as the two attributable steps this item asks for —
   > the primitive first, verified on its own, then the KDF pointed at it.
   >
   > **Step 2 (the swap).** `udb_kdf()` is now **PBKDF2-HMAC-SHA-256**
   > (RFC 8018), c = `UDB_KDF_ROUNDS` = 4096, dkLen = 32. PBKDF2 rather than the
   > old four-lane shape ported onto SHA-256, for one reason: a bespoke KDF can
   > only ever be checked against itself, whereas PBKDF2 has published vectors
   > and independent implementations to check against. Every property the v0.74
   > design comment claimed is preserved by the standard instead of by argument —
   > the U-chain is serial so the work factor cannot be parallelised within a
   > candidate, and the password is the HMAC key on every iteration so no
   > attacker precomputes past the input and amortises the rounds across guesses.
   > `HMAC-SHA-256` (RFC 2104) was added because PBKDF2 is defined over a PRF.
   >
   > **No truncation anywhere.** dkLen equals the PRF output equals
   > `UDB_HASH_LEN` equals 32, so the derived key is one whole HMAC output with
   > nothing discarded and no multi-block concatenation. `shastrs` asserts
   > `UDB_HASH_LEN == SHA256_DIGEST` so a future schema change cannot silently
   > start truncating.
   >
   > **`udb_kdf()` really was the only function that changed.** `UDB_MAX`,
   > `UDB_NAME_MAX`, `UDB_SALT_LEN`, `UDB_HASH_LEN`, `UDB_KDF_ROUNDS`,
   > `UDB_MAX_FAILS`, `struct udbent`, `udb_make_salt()`, `udb_ct_eq()`,
   > `udb_add()`, `udb_auth()`, the lockout state machine and both syscalls are
   > untouched — and **all 35 `authstrs` assertions still pass, unmodified and
   > still numbering 35.** The digests they compare are different values now, but
   > none of them ever asserted a value; they assert relationships (same input →
   > same digest, different salt → different digest, one character changes it),
   > which is precisely what made the primitive swappable.
   >
   > **Verification of the swap:** `shastrs` grew from 9 to 19 assertions —
   > RFC 4231 HMAC test cases 1, 2, 3 and 6 (6 being the longer-than-block key
   > that exercises the hash-the-key branch), the published PBKDF2-HMAC-SHA256
   > `"password"`/`"salt"` series at c=1, c=2 and c=4096 (c=2 is what catches an
   > implementation returning `Uc` instead of `U1^...^Uc`), and `udb_kdf()`
   > itself end to end against digests from an independent implementation.
   > Separately, HMAC, PBKDF2 **and `udb_kdf()` itself** were extracted verbatim
   > from `kernel64.c`, compiled natively and diffed against CPython/OpenSSL's
   > `hashlib`: 11 of 11 values bit-identical.
   >
   > Uniprocessor boot: **45 suites, 0 FAIL**, `authstrs` 35/35, `shastrs` 19/19.
   > The KDF costs ~15s of extra boot time across the whole suite, which is a
   > work factor doing its job.
   >
   > **Still not done, and still the honest next step:** PBKDF2 is *not*
   > memory-hard. It buys serial CPU cost only, so GPUs and ASICs still enjoy a
   > large advantage over a defender, and 4096 iterations is modest by modern
   > standards. The "then Argon2 or scrypt over it" half of the v0.74 sentence
   > remains open.
   >
   > *(Historical note: the status below described step 1 alone.)*
   >
   > `sha256_init/update/final` plus a one-shot `sha256()` wrapper sit in their
   > own section ahead of the user database. `shastrs` runs before `authstrs` in
   > the boot sequence (and as `shastress` at the prompt) and asserts the FIPS
   > 180-4 / CAVP digests verbatim — empty, `"abc"`, the 56-byte B.2 message
   > whose padding spills into a second block, the 112-byte CAVP message, and
   > B.3's one million `'a'` streamed. Because a one-shot vector test cannot see
   > a partial-block carry bug, it also asserts that chunked `update()` equals
   > one-shot across every length 0..200 × chunk 1..17 (3417 splits).
   >
   > Independently cross-checked on the host: the section was extracted verbatim
   > from `kernel64.c`, compiled natively, and matched against coreutils
   > `sha256sum` on the published vectors, 1 KiB and 100 KiB of urandom, and 201
   > random messages covering every length 0..200. All match.
   >
   > **Next step:** point `udb_kdf()` at it. `authstrs` is 35/35 today with
   > FNV-1a; after the swap it must be 35/35 again, and if it is not, `shastrs`
   > being green says the hash is fine and the KDF is not. That is the whole
   > reason these are two commits.
   >
   > *(Done — see the DONE status above. It was 35/35 again, and the split paid
   > for itself in an unexpected way: the first cross-check run came back with
   > one of eleven values wrong, and because the primitive vectors were separate
   > it was immediately clear the failure was a transcribed message length in the
   > TEST — RFC 4231 TC6's message is 54 bytes, not 53 — and not the HMAC.)*
6. **Persist the user database.** In-memory today, so no account survives a
   boot. Needs an on-disk format and a decision the CAS design forces: the
   volume is content-addressed with no timestamps, so a password change must not
   be inferable from dedup behaviour — two accounts with the same password
   already produce different digests (per-user salts), which helps, but the file
   as a whole wants thought before it is written.

   > **Status: DONE.** Fixed-size A/B image, a fresh nonce in every 512-byte
   > segment so nothing ever dedups, generation + root flip, SHA-256 integrity,
   > fail-closed load, lockout state deliberately not stored. Proven by booting
   > twice on one disk, which is the only test that can see it. See the STEP 6
   > section at the end of this document — including the two defects that run
   > caught and the pre-existing volume-reuse failures it surfaced.

### WHERE v0.75 STANDS

| item | state |
|---|---|
| Tier 1 · defect A — non-atomic `g_next_pid` | **done** (`pid_alloc`, atomic RMW) |
| Tier 1 · defect B — stale `ppid_slot` | **done** ((slot, generation) + `ppid_live()`) |
| Tier 1 · defect C — raw slot vs `tg_of()` | **done**, and it was not latent |
| Tier 1 · the fork enqueue funnel | **fixed**, causality NOT reproduced uninstrumented |
| Tier 1 · step 3, three clean smp4-bios runs | **NOT satisfied** — see PHASE 1 RESULT |
| Tier 2 · item 5, SHA-256 + KDF | **done** — primitive and KDF, both vector-verified |
| Tier 2 · item 6, persist the user database | **done** — see STEP 6 below |
| Network stack hardening (phase 5) | **done** — AP yields, socket generation, fd revalidation |

**This table was stale for most of the milestone and is corrected here.** The
three "open defects, none of them fixed" recorded below it were written before
the SMP work; two of them have since been fixed, and leaving the list as it was
would have had this document arguing against its own later sections.

1. ~~**The intermittent `-smp 4` page-fault panic.**~~ **FIXED.** The BSP's
   ring-3 kernel resume point was per-CPU where it had to be per-THREAD (#63).
2. ~~**`g_net_lock` re-entrancy.**~~ **FIXED.** Lock-rank storage is now keyed
   to the task, which is the thing that migrates (#64), and the residual
   suite-39 hang was an AP calling the BSP-only `sched_yield()` (#65). Twenty
   consecutive `-smp 4` boots at 0 violations, 0 underflow, 0 mismatch.
3. **Toolchain suites' wall-clock budgets** on a loaded host. **Still open** —
   not a correctness failure, but it is what breaks a regression run on a busy
   machine.
4. **The suite set is not idempotent across boots on a re-used volume.**
   **NEW, still open.** Booting twice on one disk fails `[vfsstrs]` (1) and
   `[usersstrs]` (2). Surfaced by the step 6 cross-boot test and confirmed
   pre-existing by negative control against the `main` kernel from before that
   branch. Every regression run to date has used a fresh image, so this has
   never been exercised — which is exactly why it survived this long.

With (1) and (2) closed, the release gate is meaningful again: all three
configurations reach the prompt. What remains for a v0.75.0 tag is the gate
itself, run as the gate — see the closing section.

### Explicitly deferred past v0.75

Carried from CHANGELOG-0.74.0.md's closing list so they are not rediscovered:
an execute permission bit (needs a default-mode story for compiler output);
directory permissions (needs real VFS directory objects); supplementary groups;
a login program; lockout expiry and administrative unlock. The queued TCP
hardening work (congestion window, slow start, fast retransmit, Karn/Jacobson
RTO, segment coalescing) is independent of all of the above and unaffected by
this milestone's ordering.

---

## SUITE 39 (`tcpstrs`) `-smp 4` HANG — RESOLVED

The intermittent hang that stalled every `-smp 4` boot at `[tcpstrs]` is fixed.
Two defects, both on the `sys_connect` SYN-retransmit path, both of the same
family: **something treated a transient handle as a stable identity.**

### 1. An AP called the BSP's scheduler (the cause)

`sched_yield()` operates on `g_cur` / `g_threads[]` — the BSP scheduler's
tables — and carries no core guard. Five syscall paths called it unguarded, so
an AP reaching any of them picked a BSP kernel thread and switched *itself*
onto that thread's stack while the BSP might be running the very same thread:
`sys_connect`, `sys_futex_wait`, `sys_epoll_wait` (x2), `sys_thread_join`.

All five now call `krelax()`, which already existed for exactly this case: it
yields on the BSP and `PAUSE`-spins on an AP, where there is no kernel
scheduler to yield to. BSP behaviour is unchanged by construction.

### 2. A socket index is not a socket identity (latent, unproven)

`sys_connect` released `g_net_lock` inside its retransmit loop while holding a
raw `struct nsock *`. `g_sock[]` is static, so the pointer never dangles — it
just addresses whoever occupies that slot when the lock returns, and the loop
would retransmit our SYN with our sequence numbers onto their session.

`struct nsock` now carries a generation, every slot wipe goes through
`sock_slot_wipe()` (which carries the counter across the `cmemset` and bumps
it), and the loop revalidates after each re-acquire, returning `-ECONNRESET` on
mismatch. Same shape as defect B's `ppid_slot` fix, for the same reason.

**This one is committed on its argument, not on evidence: it never fired.**
`stale=0` across all 20 verification boots. It is correct and, on this
workload, inert.

### Evidence

3-arm interleaved matrix, 8 rounds, round-robin so host drift hits all arms:

| arm | build | OK | HANG |
|-----|-------|----|------|
| `ap`  | neither fix    | 4 | 4 |
| `fix` | AP-yield only  | 7 | 1 |
| `sg`  | both fixes     | 8 | 0 |

Final binary, 20 boots at `-smp 4`: **20 OK / 0 HANG / 0 PANIC**, 44 suites and
0 failures every boot, rank violations/underflow/mismatch all 0. An AP entered
the retransmit loop in 14 of the 20, so the fixed path was genuinely exercised
rather than merely not taken.

Pooling the baseline arm with ten earlier boots of the same `ap.iso`: the
unfixed kernel hangs **11 times in 18 boots** against **1 in 16** for the arms
carrying the AP-yield fix.

### Two process notes, because both cost real time

- **A run that boots the wrong image is worse than no run.** The first
  verification of the AP-yield fix booted the *detector* ISO while writing to a
  directory named for the fixed one, and its result was reported as evidence
  that the fix did not work. It was evidence about the unfixed kernel. Every
  harness now stamps the md5 of the image it boots into every log it writes.
- **A counter nothing prints is not instrumentation.** `g_connect_stale` was
  incremented but never emitted; the first matrix grepped for a string no code
  could produce and got zeroes from all three arms. Both `sys_connect` counters
  now print in the end-of-boot summary. Note they are readable only on boots
  that survive to reach it.

`g_connect_ap_yield` counts AP **arrivals** at the yield site, before the
backoff runs. Non-zero is expected and healthy on `-smp 4` — it measures
coverage of the AP path, not faults. `g_connect_stale` is the fault counter.

### Still open on the socket paths

`sock_of_fd()` resolves the fd outside `g_net_lock` and the index is used after
the lock is taken, so the fd->slot mapping is still a TOCTOU window. The
generation counter now makes that window *detectable* wherever a caller cares
to check it, but only `sys_connect` checks today.

---

## `sock_of_fd()` TOCTOU IN `sys_connect()` — CLOSED

`sock_of_fd()` resolves a descriptor under `g_ofile_lock` (**rank 1**) and
releases it; the caller then climbs to `g_net_lock` (**rank 9**) and indexes
`g_sock[si]`. The descriptor can be closed and reissued in that gap, leaving a
slot index that names a socket the caller no longer owns.

Two changes in `sys_connect()`:

1. **One resolution, not two.** It called `sock_of_fd()` twice — once for the
   slot, again for the flags — and discarded the slot the second call resolved.
   Two independent lookups of a descriptor that can change between them, with
   nothing checking they agreed.
2. **Revalidate after taking the net lock**, via `sock_fd_still_ours()`: the
   slot is ours iff it is in use and its own back-pointer still names our
   descriptor. The retransmit loop's post-yield check uses it too, on top of the
   generation compare.

### Why NOT "move sock_of_fd() inside g_net_lock"

That is the obvious-looking fix and it is wrong. `sock_of_fd()` takes rank 1;
inside `g_net_lock` we hold rank 9. Acquiring 1 under 9 is a rank **inversion**
— and specifically the one this document already records above as a
pre-existing defect ("`ofile` rank 1 taken while holding rank 9, consistent with
re-entering an fd path from inside the net lock"). v0.75 spent its whole
rank-tracking effort driving those to zero. Trading a narrow race for a
deadlock class is not a fix.

`g_sock[].fd` is the right authority precisely because `g_net_lock` already
protects it: validating against state the held lock covers needs no second
acquisition and cannot invert anything. There is no `dup()`/`dup2()` for
sockets in this tree, so the back-pointer is unambiguous — one descriptor per
slot.

**Residual, and deliberate:** if a descriptor is closed and a new socket lands
on *both* the same slot and the same fd number, this compares equal. That is a
socket the descriptor legitimately names now, and an application racing
`close()` against `connect()` on one descriptor has no defined answer to be
denied.

### Verification

Uniprocessor 44 suites / 0 failed, `[tcpstrs] 28 passed`. Ten `-smp 4` boots:
**10 OK / 0 HANG / 0 PANIC**, 44 suites and 0 failures every boot, and **0 rank
violations, underflow or mismatch in any boot** — which is the check that
matters here, since the rejected alternative would have produced them. An AP
entered the connect retransmit loop in 8 of the 10, so the path was exercised.
Build 0 errors, 35 warnings (unchanged baseline).

Like the generation counter, the new check **never fired** (`stale=0`
throughout). It is committed on its argument, not on evidence of the race
occurring.

### The five sibling syscalls — now also closed

`sys_connect()` was fixed first; the identical resolve-then-climb gap existed in
every other `sock_of_fd()` caller. All five now revalidate with
`sock_fd_still_ours()` immediately after acquiring the net lock, returning
`-EBADF` and releasing the lock.

| syscall | before | after |
|---------|--------|-------|
| `bind`   | no check          | `sock_fd_still_ours()` |
| `send`   | `used` only       | `sock_fd_still_ours()`, before the ENOTCONN test |
| `recv`   | `used` only       | `sock_fd_still_ours()`, **inside** the blocking poll loop |
| `listen` | no check          | `sock_fd_still_ours()` |
| `accept` | no check          | `sock_fd_still_ours()` in both branches |

Checking `used` was not the same question: a slot freed and immediately
reallocated is `used` again, and that is the common case under load.

Three of these are not mechanical copies of the `sys_connect` change:

- **`recv`** puts the check inside its blocking poll loop rather than before
  it. The window there is not the resolve->acquire gap alone — it reopens on
  every iteration, which makes it the longest exposure of the five.
- **`send`** checks before the ENOTCONN test, so a stale handle reports EBADF
  instead of being described as a live-but-unconnected socket. The caller's
  descriptor is the thing that went away.
- **`accept`** now distinguishes EBADF (descriptor gone) from EINVAL (live
  socket that is not a listener); the old test collapsed both into EINVAL. Its
  unlocked read of `g_sock[li].stream` is deliberately left alone: `stream` is
  set at allocation and cleared only by the wipe that frees the slot, so it
  never changes under a live socket. A stale index can steer the wrong branch
  and nothing worse, because both branches revalidate under the lock — a wrong
  branch costs an error path, not a wrong action.

Verified: uniprocessor 44 suites / 0 failed, `[tcpstrs] 28 passed`; ten `-smp 4`
boots **10 OK / 0 HANG / 0 PANIC**, 0 suite failures and **0 rank violations,
underflow or mismatch in any boot**. Build 0 errors, 35 warnings.

As with `sys_connect`, **none of these checks fired** (`stale=0` throughout).
They are committed on their argument, not on evidence of the race occurring.

### Phase 5 (network stack hardening) — CLOSED

Every `sock_of_fd()` caller now validates its descriptor under the lock that
protects the answer. The v0.75 network work is: the AP-yield fix, the socket
generation counter, and this descriptor revalidation across all six syscalls.

## TIER 2 CRYPTO ENGINE — CLOSED

Both halves are merged to `main`.

- **PR #61** — SHA-256 (FIPS 180-4) as its own verified unit. Merged
  (`285069b`).
- **PR #62** — `udb_kdf()` is PBKDF2-HMAC-SHA-256 (RFC 8018), c = 4096,
  dkLen = 32. Merged (`320acc2`).

`udb_kdf()` was the only function that changed. `UDB_MAX`, `UDB_NAME_MAX`,
`UDB_SALT_LEN`, `UDB_HASH_LEN`, `UDB_KDF_ROUNDS`, `UDB_MAX_FAILS`,
`struct udbent`, `udb_make_salt()`, `udb_ct_eq()`, `udb_add()`, `udb_auth()`,
the lockout state machine and both syscalls are untouched — and all 35
`authstrs` assertions still pass, unmodified and still numbering 35. They
assert *relationships* (same input → same digest, different salt → different
digest, one character changes it), never values, which is exactly what made the
primitive swappable.

No truncation anywhere: dkLen = PRF output = `UDB_HASH_LEN` = 32, so the derived
key is one whole HMAC output with nothing discarded and no multi-block
concatenation. `shastrs` asserts `UDB_HASH_LEN == SHA256_DIGEST` so a future
schema change cannot silently start truncating.

Verified on merged `main`: **45 suites, 0 FAIL**, `shastrs` 19/19,
`authstrs` 35/35; build 0 errors, 35 warnings.

### A merge hazard worth recording

#61 and #62 were stacked, and #61 was **squash**-merged. Main then carried the
SHA-256 primitive as one new commit while #62's branch still carried the
original commits that produced it, so git saw the same content added twice and
conflicted — three kernel hunks, two roadmap hunks. Two of those conflicts were
not merely stale but actively **wrong** on main's side: `shastrs` asserted "NOT
yet wired into `udb_kdf()`", which #62 makes false.

Resolved hunk-by-hunk, keeping #62's side, rather than by taking whole files —
main's `sock_of_fd()` revalidation work (#66) lives in the same file and had
auto-merged cleanly, and a whole-file resolution would have silently reverted
it. #62's PR base also still pointed at `v075-tier2-crypto` and had to be
retargeted to `main` after #61 merged.

**For the next stacked pair:** merge the parent first, then re-merge `main` into
the child and expect conflicts proportional to the parent's size. They are
mechanical, but they are not automatic, and a whole-file "take ours" is the
wrong reflex.

### Step 6 — also done

Persistence landed (#68). The design question it was blocked on — the volume is
content-addressed with no timestamps, so a password change must not be inferable
from dedup behaviour — is answered by a fresh nonce in every 512-byte segment,
so no write ever dedups and dedup behaviour carries no signal. Tier 2 is
complete. See the plan and the STEP 6 result section below.

### Not done, and honest about it

PBKDF2 is **not memory-hard**. It buys serial CPU cost only, so GPUs and ASICs
retain a large advantage over a defender, and c = 4096 is modest by modern
standards. The "then Argon2 or scrypt over it" half of the v0.74 sentence
remains open and is not claimed by this milestone.

---

## STEP 6 PLAN — PERSISTENT USER DATABASE STORAGE

### The constraint that shapes everything

The volume is content-addressed with no timestamps. An observer who can see
which blocks exist learns things from *dedup behaviour alone*, without reading
any content:

- **A write that dedups is a write that changed nothing new.** If the UDB image
  is written verbatim and a user reverts to a previous password, the image
  becomes byte-identical to an earlier one, the store dedups, and no new block
  appears. That is a direct signal: "this account went back to a password it
  used before." Per-user salts do not help here — the salt is stable across a
  password change for the same user, so the same password re-derives the same
  digest and the same bytes.
- **Per-record blocks leak which account changed.** If each user is its own
  content unit, a password change replaces exactly one block, naming the
  account even though the content is a digest.
- **Size leaks population.** A file that grows with each account publishes how
  many accounts exist.

Per-user salts already solve the *cross-account* case: two users sharing a
password produce different digests and therefore different bytes. The gap is
the *same-account-over-time* case.

### Proposed design

1. **One fixed-size image, not per-record blocks.** Serialise the whole table —
   `UDB_MAX` slots, every slot written whether occupied or not, unused slots
   filled with random bytes rather than zeroes. Size is then constant and
   independent of population, and no per-account block exists to be correlated.

2. **A fresh random nonce in every write.** A 16-byte `image_nonce`, redrawn on
   every mutation and covered by the checksum. This is the mechanism that
   actually closes the stated hole: no two writes ever produce identical bytes,
   so nothing ever dedups, so dedup behaviour carries no signal at all. Reverting
   a password produces a fresh block exactly like any other change.

   This costs one block per mutation with no dedup savings. That is the point —
   the dedup savings *are* the leak.

3. **Integrity: SHA-256 over the image**, which we now have. Store
   `sha256(nonce || records)` in the header. A torn or tampered image is then
   detectable rather than silently loaded.

4. **Atomic replacement via a generation counter and a root flip.** Write the
   new image, then update a single root reference. Header carries a
   monotonically increasing `generation`; on load, prefer the highest generation
   whose checksum verifies. Same shape as `kproc::gen` and `nsock::gen` — an
   index or a location is not an identity, and the generation is what makes
   "which version is real" answerable after a crash.

5. **Fail closed on load.** If no image verifies, the database must NOT quietly
   become empty — an empty UDB is a system where the next caller creates the
   first account. Boot with the table marked unavailable so `udb_auth()` denies
   everything, and require an explicit operator action to reinitialise.

### The decision this forces, flagged rather than assumed

**Should lockout state persist?** Failed-attempt counters change on every failed
authentication. If they live in the image, every failed login writes a new
block, and an observer counting writes counts failed logins — a fresh side
channel introduced by the fix for another one. If they stay in memory, lockout
resets across a reboot, which weakens it against an attacker who can force
reboots.

Recommendation: **persist accounts, keep lockout in memory for now**, and record
the weakness explicitly rather than trading a visible leak for an invisible one.
Revisit when there is a way to write without publishing that a write occurred.

### What this does not provide

Persistence adds no confidentiality. Salts and digests are readable by anyone
who can read the volume; what protects passwords is the KDF, not the storage.
There is no key store, no TPM, and nothing to encrypt the image *with* that is
not itself on the same volume — so image encryption is deliberately out of
scope here rather than half-done.

### Suggested order

1. On-disk format + `sha256` header + serialise/deserialise, with a `udbstrs`
   round-trip assertion. No persistence wired in yet.
2. Load/store against the volume, generation + root flip, crash-consistency
   assertions (write, interrupt, reload → previous generation intact).
3. Wire into boot and `udb_add()`/password change, then assert the property that
   motivated all of it: **two writes of the same logical content produce
   different blocks.**

Same discipline as Tier 2: land the format as its own verified unit before
wiring it in, so a failure has one candidate.

---

## STEP 6 — PERSISTENT USER DATABASE STORAGE — IMPLEMENTED

Accounts survive a reboot. The plan above is implemented essentially as written;
what follows is what changed, what it cost, and the two things the first real
cross-boot run caught.

### Shape

`/etc/udb.a` and `/etc/udb.b`, written alternately, each `UDB_IMG_BYTES` (1536)
— three fixed 512-byte segments, each carrying its own freshly drawn nonce.

```
payload (1488 B, fixed)          segment on disk (512 B, x3)
  0  magic "ORUNUDB1"              0  nonce[16]     <- fresh EVERY write
  8  version, reserved            16  payload slice
 16  generation
 24  sha256 over [56..end)
 56  16 records x 84 B
 ..  random tail
```

Record: `name[24] uid gid salt[16] hash[32] used pad[3]`. No `fails`, no
`locked` — see below.

### Why a nonce per SEGMENT and not per image

A VFS file is stored as per-512-byte-block CAS chunks (`chunk_hash[i]`), so
dedup is observable **per chunk**. One nonce in the header would leave the other
chunks byte-identical whenever they had not changed, which is the same leak the
design exists to close, one level quieter. With a nonce in every segment, every
chunk is new on every write and dedup behaviour carries no information at all.

`authstrs` asserts this rather than assuming it: two writes of the same accounts
must produce different bytes, **and every 512-byte segment must differ**.

Cost: one block per segment per write, no dedup savings, forever. That is the
trade — the savings were the leak.

### What was NOT persisted, and what that costs

Lockout counters. They change on every FAILED authentication, so storing them
would make each failed login write a block, and an observer counting writes
would be counting failed logins — a new side channel opened by closing another.

The price is real and is not hidden: **lockout state resets across a reboot**,
which weakens it against an attacker who can force reboots. Recorded here as a
known weakness rather than presented as a design win.

### Fail closed

An image that is present but does not verify sets `g_udb_unavailable` and
`udb_auth()` denies everything. Carrying on with an empty table would be a
system where the next caller creates the first account.

### Two defects the first cross-boot run found

1. **The generation was being rewound.** `authstrs` snapshots and restores the
   table so it stays hermetic, and the first version restored the *counter* too.
   Unsound: the suite writes several generations, so a rolled-back counter can
   make the next save carry a LOWER number than an image still sitting in the
   other slot, and the loader prefers the highest that verifies — it would have
   adopted the suite's throwaway accounts on a later boot. Table restored,
   generation never.
2. The slot letter printed as an empty string (a `char` passed to `%s`) —
   cosmetic, but it made the A/B alternation unreadable in precisely the log
   being used to check it.

Neither was found by the compiler or by the 10-boot gate. Both were found by
booting twice on one disk, which is the only test that can see this feature.

### authstrs had to change, and its count moved

The suite CREATES accounts, which now outlive the boot. Left alone, the next
boot on the same volume would find `m74alice` present, get EEXIST, and fail
"root can create an account" — the suite breaking itself on the second run only.
It now snapshots, restores and re-saves, leaving the volume as it found it.

**Assertions move 35 -> 43.** The "still numbering 35" invariant recorded in the
Tier 2 section above was about the KDF swap not disturbing existing assertions,
and it still holds — all 35 are unmodified. The eight additions are new
coverage: round-trip, digest rejection, magic rejection, fixed size, lockout
never persisting, and the no-dedup property itself.

### Verification

The gate cannot see this feature. `tocver.sh` and every harness before it build
a FRESH disk image per boot, so a green 10-boot run says nothing about whether
anything persisted. Persistence is demonstrated separately, by hand:

```
boot 1 (fresh disk):  [udb] no stored database - starting empty, persistence armed
                      [udb] created 'udbreboot' and saved at generation 5
boot 2 (SAME disk, a different QEMU process):
                      [udb] loaded generation 5, 1 account(s) (both slots valid)
                      [udb] CROSS-BOOT OK: account 'udbreboot' (uid 4242)
                            survived a reboot at generation 5
```

The marker is a manual `udbpersist` command, not a boot-time act, in the shape
of v0.48's `vfs-reboot-test`: a marker the boot creates for itself proves
nothing about the boot before it.

Standard gate, fresh images: uniprocessor **45 suites / 0 failed**; ten `-smp 4`
boots **10 OK / 0 HANG / 0 PANIC**, 0 suite failures and 0 rank faults in any
boot. Build 0 errors, 35 warnings.

### A pre-existing defect this surfaced (NOT caused by this work)

Booting twice on one volume fails two suites: `[vfsstrs] 18 passed, 1 failed`
("VFS journal commit is genuinely DEFERRED") and `[usersstrs] 16 passed, 2
failed` ("a newly created file gets the default mode", "a stranger CAN open
another user's 0644 file for reading").

Established as pre-existing by negative control: the **merged `main` kernel from
before this branch** was put through the identical two-boot sequence and
produces the same two failures. The suite set is not idempotent across boots on
a re-used volume, independently of the user database. Untouched here, and it
deserves its own investigation — every regression run to date has used a fresh
image, so this has never been exercised.

### Still open

- The lockout-across-reboot weakness above.
- No confidentiality: salts and digests are readable by anyone who can read the
  volume. There is no key store and nothing to encrypt with that does not live
  on the same volume, so this remains deliberately out of scope.
- No account deletion or password change syscall yet, so the revert-to-an-old-
  password case the nonce defends against is not yet reachable from ring 3. The
  defence is in place ahead of the path that needs it.
