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

## MILESTONE PLAN

### Tier 1 — the race, and restoring the suite

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
6. **Persist the user database.** In-memory today, so no account survives a
   boot. Needs an on-disk format and a decision the CAS design forces: the
   volume is content-addressed with no timestamps, so a password change must not
   be inferable from dedup behaviour — two accounts with the same password
   already produce different digests (per-user salts), which helps, but the file
   as a whole wants thought before it is written.

### Explicitly deferred past v0.75

Carried from CHANGELOG-0.74.0.md's closing list so they are not rediscovered:
an execute permission bit (needs a default-mode story for compiler output);
directory permissions (needs real VFS directory objects); supplementary groups;
a login program; lockout expiry and administrative unlock. The queued TCP
hardening work (congestion window, slow start, fast retransmit, Karn/Jacobson
RTO, segment coalescing) is independent of all of the above and unaffected by
this milestone's ordering.
