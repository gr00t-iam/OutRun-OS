# OutRun OS v0.77.0-metal — the last spin count, and a quiet half of the tree

Milestone 77. v0.76 was about **evidence** — it built a dirty-volume gate, found
that the gate's own failure counter was blind, and converted three suites from
spin budgets to real-time deadlines. It closed carrying two admissions: the
conversion had covered `langstrs`, `toolstrs` and `pipestrs` but not the rest of
the idiom, and one `pthreads_smp` failure was unexplained.

This release finishes the conversion, corrects v0.76's account of that failure,
and raises ring 3 to the warning level the kernel has always been held to.

## THE REMAINING RING-3 WAITS

`owaitpid()` was converted in v0.76. Three more waiters were still budgeting
iterations, and iterations are not time.

| site | was | now |
|---|---|---|
| `pthread_join` | `k < 20000` retries | `osysticks()` deadline (`WAIT_T_JOIN`) |
| `kthread_join` | `k < 20000` retries | the same |
| `cs_compile` (`compilerstrs`) | `owaitpid(pid, 250000)` | `cs_compile_ticks(..., LANG_T_COMPILE)` |
| `compilerstrs` run wait | `owaitpid(pid, 250000)` | `owaitpid_ticks(..., LANG_T_RUN)` |

`kthread_join` was not on the carryover list. It is the same 20000-retry shape
on the same syscall, and converting one of the two would have repeated exactly
what v0.76 did when it fixed `langstrs` and left `toolstrs` and `pipestrs`
behind: **the idiom is the defect, not the call site.**

The spin-budgeted `cs_compile()` is **deleted**, not left beside its
replacement. A dead helper that still compiles is how the old shape comes back —
the next person to add a round copies whichever one their eye lands on.

### `compilerstrs` now prints what it measures

The converted stage prints its own elapsed time, as `langstrs` has since v0.76.
The two-unit build measures **91–119 ds (9.1–11.9 s)** across seven boots.

That figure is the point. It is roughly **4x** the 2.8 s `omake` compile whose
budget broke `langstrs` on `-smp 4` boot 1, which is why `compilerstrs` was
named the highest-risk unconverted site — and against the 200 s ceiling it is
~17x margin, now an **observed** margin rather than a guessed one.

## A DEADLINE IS NOT A DEFECT

Every converted site now reports a timeout distinctly from a failure:

| suite | defect | deadline |
|---|---|---|
| role 31 (`pthreads + mutex`) | 902 | **908** |
| `pthreads_smp` | 945 | **937** |

Both kernel decoders name them, and `posixstrs` prints an explicit
"a join DEADLINE expired — not a join defect" line for role 31, because that
round reports through a generic `exit N (want M)` path where a bare number tells
the reader nothing.

This is the same repair v0.76 made for `pipestrs`, where a hung child and a
broken pipe both exited 957 and would have sent a reader after a pipe bug that
does not exist.

## CORRECTION: WHAT THE v0.76 `pthreads_smp` FAILURE ACTUALLY WAS

`CHANGELOG-0.76.0.md` stated that `pthread_join` polled a **non-blocking**
`SYS_THREAD_JOIN` 20000 times and that this spin count **was the mechanism**
behind the one observed failure. Both claims were wrong, and both were reached
by analogy with carryover 2 rather than by measurement — the precise move v0.76
existed to distrust. The entry has been corrected in place.

`SYS_THREAD_JOIN` **parks** the caller: `block_ring3(sf, p, JOIN_KEY_OF(L, tid),
FUTEX_DEFAULT_TICKS)`, and `FUTEX_DEFAULT_TICKS` is 20000 ticks — **200 s** at
100 Hz. The measurement, from the preserved log:

```
[posixstrs] .. waiting spins=36923 ticks=35584 (+19802)
[pthreads_smp] driver exit 945 — pthread_join failed or timed out
```

**+19802 ticks** — the kernel's own park deadline expiring inside a **single**
call. The retry loop never went round twice. From outside, that boot took
**425 s** against 260 s and 220 s for its two siblings on the same image.

So the conflation was real and is fixed here. The spin count was real and is
converted here. **The mechanism was neither**, and the honest residue is
sharper than what v0.76 wrote down: **a wake was lost, and why is unknown.**

### What this release does NOT fix, stated where it cannot be missed

**A userland deadline cannot bound a single park.** `SYS_THREAD_JOIN` takes
`(tid, out_code)` and no timeout, so `WAIT_T_JOIN` is only checked *between*
syscalls: it bounds the "woken, still not done, ask again" path. If a wake is
genuinely lost, the first call still blocks for the kernel's 200 s and this
budget never gets to fire. Making that case fast needs a timeout argument in the
syscall ABI. The comment beside `WAIT_T_JOIN` says so, rather than implying a
fix it does not deliver.

`WAIT_T_JOIN` is 2000 ticks (20 s), deliberately below role 31's 3000-tick
`posix_drain` watchdog so the inner deadline fires first and the log names an
assertion instead of reporting "not every task reached a terminal state".

## THE WARNING SWEEP

46 warnings to 1. The build is clean under `-Wall -Wextra -Werror` for both the
kernel and ring 3; the single remaining diagnostic is a linker warning discussed
below.

| category | count | what was done |
|---|---|---|
| `-Wmisleading-indentation` | 16 | split `if (a) x; if (b) y;` onto separate lines — whitespace only |
| `-Wunused-function` | 5 | `__attribute__((unused))`, **not** deletion |
| `-Wsign-compare` | 5 | cast the unsigned side to `int`, matching the idiom already used feet away |
| `-Wshift-negative-value` | 5 | shift in the unsigned domain and convert back |
| `-Warray-bounds` | 5 | launder the fault address through an empty `asm` |
| `-Wcomment` | 4 | reorder globs so `/` never precedes `*` |
| `-Wunused-but-set-variable` | 3 | explicit `(void)` discard **plus a recorded finding** |
| `-Wdiscarded-qualifiers` | 1 | make the `volatile` discard an explicit cast |
| `ld: missing .note.GNU-stack` | 1 | added the marker to the four ELF asm objects |

Four of these deserve more than a table row.

**The deliberate faults.** Five sites write through `(volatile u32 *)0x1` to
prove the kernel reclaims resources on the FAULT exit path and not only on
`SYS_EXIT`. `-Warray-bounds` is a *true* observation about code whose entire
purpose is to be invalid. They now go through `fault_ptr()`, which launders the
address through an empty `asm` — the optimiser can no longer see the constant,
and the same store to the same address still faults. A `#pragma` would have
silenced the category everywhere, including somewhere it might one day be right.

**The unused functions are marked, not deleted.** `ocalloc` and `pthread_exit`
are API surface; `pci_probe_virtio` and `wimp_input_step` are readable
references. Deleting a working allocator to quiet a warning trades a real
capability for a cosmetic one.

**`-Wshift-negative-value` was not cosmetic.** `FXI(v)` was
`((int64_t)(v) << 16)`, and window physics uses negative coordinates constantly
— left-shifting a negative value is undefined (C11 6.5.7p4). It now shifts as
`uint64_t` and converts back, which is the identical bit pattern on two's
complement. The arithmetic did not change; the undefined behaviour did.

**A finding recorded rather than fixed.** The three set-but-unused variables in
the virtio-net probe are `notify_bar`, `isr_bar` and `devcfg_bar`. They are
unread because the driver **assumes every capability lives in `common_bar`** and
treats the others as offsets into it — a device that placed one elsewhere would
be mis-driven silently. `vblk`'s walk compares them and bails; `vnet`'s does not.
This sweep is cosmetic by contract, so the assumption is now stated in a comment
and recorded here **as a finding, not a fix**. QEMU puts all four in BAR 4,
which is why it has never bitten.

### Ring 3 was being held to a lower standard, and it hid three defects

`CFLAGS` has carried `-Wall -Wextra` for the kernel for a long time. `UCFLAGS`
carried `-Wall` alone. Raising ring 3 to parity surfaced three findings that the
quieter half had been keeping, and **two of them are real**:

- `if (sysc(SYS_SYSINFO, ...) < 0)` in **`osysticks()` and `osysncpu()`** —
  `sysc()` returns `u64`, so `< 0` was always false and neither error check
  could ever fire. Now cast to `i64`. A guard that cannot fail is the same class
  as a counter nothing prints, and these two sit under every tick deadline this
  milestone and the last one added.
- `round` in the SIGSEGV round-trip test was live across `osetjmp`/`olongjmp`
  without `volatile`, whose value after a `longjmp` is indeterminate
  (C11 7.13.2.1p3). It has worked because gcc happened to spill it. Now
  `volatile`.

`-Wextra` is now permanent for ring 3. A warning level that differs between two
halves of one tree hides defects in the quieter half — which is exactly what it
had been doing.

### The one warning left, and why it stays

```
ld: warning: build/outrun-kernel.elf has a LOAD segment with RWX permissions
```

Silencing this means either `--no-warn-rwx-segments`, which hides it, or
splitting the kernel image into separate RX and RW segments, which is a change
to the linker script and the boot path — not a cosmetic change, and not one to
make in the same commit as a warning sweep, days before a tag. A single RWX LOAD
segment is ordinary for a freestanding kernel of this shape. It is left visible
and named here rather than suppressed.

## `mcpre` — CARRYOVER 2 WAS ALSO IN THE KERNEL

The first release candidate for this tag **failed its own gate**, and the fix
below is the reason there is a second one.

`make gate-dirty-smp` failed on boot 3 of 3 with:

```
[mcpre  ] FAIL  long probe never started on cpu1
```

This is the anomaly v0.76 recorded — one occurrence in 277 boot logs, never
reproduced, unexplained, and with its evidence log lost because "preserved" meant
a working directory rather than a commit. It reproduced here at **1 failing
assertion in 6 dirty-SMP boots**, and this time the log is committed and
md5-stamped.

**The mechanism, read from the source.** The assertion queues a probe on cpu1,
sends an IPI, and waits for it to reach ring 3:

```c
uint64_t t0 = g_ticks;
while (!(kprocs[pl].ran_on & 2u) && g_ticks - t0 < 500) __asm__ volatile("pause");
```

500 ticks is **5 seconds of wall clock** for another vCPU to pick up queued work.
On a TCG-only host the guest's four vCPUs are multiplexed onto host threads the
guest does not schedule, so the quantity being bounded has no fixed relationship
to the thing being waited for. **That is carryover 2's defect exactly** — the
same argument that condemned `owaitpid`'s spin budget in v0.76 and
`pthread_join`'s retry count earlier in this changelog — except in **kernel**
code, where nobody had thought to look for it.

Two milestones described this as a suspected wall-clock flake. It was not
suspected any more once the constant was read.

### What changed

All three of the suite's budgets are now named ceilings, because converting only
the one that fired would repeat the mistake this release opened by criticising:

```c
#define MCPRE_T_START    6000u    /*  60 s: cpu1 picks the queued probe up */
#define MCPRE_T_PREEMPT  6000u    /*  60 s: the preempt IPI lands at CPL3  */
#define MCPRE_T_JOIN    12000u    /* 120 s: both threads run to completion */
```

**The elapsed figure is now printed on the success path**, not only on failure:

```
[mcpre  ] long probe reached ring 3 on cpu1 in N tick(s) (ceiling 6000)
```

The normal case is well under one tick. Printing it means the next person to
touch these numbers reads a distribution instead of guessing, which is the
`oputu()` lesson from v0.76 applied to the kernel side.

**And the give-up now explains itself.** The old line said only that the probe
never started. It now reports that a DEADLINE expired, how many ticks it waited,
cpu1's run-queue depth and how many cpus were online — because the absence of
exactly that detail is most of why this stayed unexplained for two milestones.

**This raises a ceiling; it does not prove a cause.** If cpu1 is genuinely
wedged rather than merely starved, the assertion still fails — 60 s later, with
diagnostics attached. That is the intended outcome: a slow host stops being
indistinguishable from a broken one, which is the whole of carryover 2.

## VERIFICATION

```
outrun-os-0.77.0.iso
md5     f669a57b52c847627916c4bb07a78302
sha256  bcd6b436845b05c648622f7cec822b80c0c4d7fff7fcb19a96f798a4b41dfdbf
```

This is **release candidate 2**. RC1 (md5 `863590eaf2536d45225d890ff3ee516e`)
failed its own gate and was not tagged; the `mcpre` section above is what
changed between them. Every configuration below booted the image named here, and
each log's first line carries that md5.

### Fresh-image matrix

| configuration | suites | passed | failed | rank faults | boot |
|---|---|---|---|---|---|
| uniprocessor (`make release-verify`) | 45 | 479 | **0** | 0 | 300 s |
| `-smp 4`, SeaBIOS | 45 | 495 | **0** | 0 | 215 s |
| `-smp 4`, q35 + VT-d, `intremap=on` | 47 | 508 | **0** | 0 | 230 s |

47 rather than 45 under VT-d is `iommu` and `capdma`, config-gated on the
emulated unit — the tell that the target really is running with an IOMMU rather
than having quietly degraded to plain q35.

### Dirty-volume gate

| configuration | boot 1 | boot 2 | boot 3 | verdict |
|---|---|---|---|---|
| `make gate-dirty-smp`, run 1 | 45 / 0 | 45 / 0, 1 reset | 45 / 0, 1 reset | **PASS** |
| `make gate-dirty-smp`, run 2 | 45 / 0 | 45 / 0, 1 reset | 45 / 0, 1 reset | **PASS** |
| `make gate-dirty` (UP) | 45 / 0 | 45 / 0, 1 reset | 45 / 0, 1 reset | **PASS** |

Two SMP runs rather than one on purpose: the failure being fixed occurred at
1 in 6 dirty-SMP boots, and a single 3-boot run would have had roughly a 6-in-10
chance of passing even with nothing fixed. Six boots is still not a proof — it
is simply the smallest number that is not self-deceiving.

Consecutive-boot diffs empty in both directions in all three runs; both durable
cross-boot artefacts created in boot 1 and found in every later boot; zero
fixture-reset refusals; dual failure counters agreeing at zero in every log.

**15 boots total across six configurations, 0 failing assertions, 0 rank
faults.**

### What the new instrumentation measured

```
[mcpre  ] long probe reached ring 3 on cpu1 in 0 tick(s) (ceiling 6000)
```

**0 ticks, in all 8 SMP boots that reached it.** That figure is worth more than
the pass it accompanies. The normal case is under a single 10 ms tick, and the
budget that failed was 500 ticks — so the failure was never a marginal overrun
of a slightly-too-tight constant. Whatever happened, cpu1 did not run queued
work for more than **500 times** the normal latency.

The new ceiling therefore buys margin against host scheduling noise, and the
honest reading is that it makes a *spurious* failure much less likely without
touching whatever produced the real one. The elapsed print exists so that the
next occurrence arrives with a number attached instead of an argument.

### Build

```
0 errors, 1 warning (ld: LOAD segment with RWX permissions — see above)
clean under -Wall -Wextra -Werror for both the kernel and ring 3
```

## WHAT THIS RELEASE DOES NOT DO

- **Carryover 3 — the fork race — is still unaddressed, for a third milestone.**
  v0.75 fixed three real defects on the path; the v0.74 symptom has never been
  reproduced without instrumentation, so "fixed" and "did not fire" remain
  indistinguishable. v0.76 recorded it explicitly rather than silently, and this
  release does the same. It is the oldest open item in the project and it has
  now outlived two milestones that each said it should not.
- **The lost wake behind the `pthreads_smp` failure is unexplained.** One
  occurrence in six dirty-SMP boots, never reproduced. This release makes it
  *report itself correctly* when it recurs — 937, with a decoder line — which is
  the difference between catching it next time and mistaking it again.
- **`SYS_THREAD_JOIN` has no timeout argument.** See above; the natural v0.78
  item, and the only thing that would bound a lost wake in useful time.
- **Why cpu1 stalled in the `mcpre` failure is unexplained.** The budget it blew
  is fixed and instrumented, and that is all. The measurement makes the gap
  starker rather than smaller: the probe normally reaches ring 3 in **0 ticks**,
  and the old ceiling was 500, so cpu1 failed to run queued work for over 500x
  the normal latency. A raised ceiling cannot explain that; it only stops a host
  hiccup from being indistinguishable from it. If it recurs, the log will now
  carry the waited ticks, cpu1's run-queue depth and the online cpu count.
- **The virtio-net BAR assumption is documented, not repaired.**
- **The RWX LOAD segment** stands, deliberately.
- **No memory-hard KDF, no password-change syscalls, no reboot-surviving
  lockout, no confidentiality for the stored database.** Carried unchanged from
  v0.76; the KDF remains the largest structural gap.
- **No execute permission bit, no directory permissions, no supplementary
  groups, no login program, no lockout expiry, no administrative unlock.**
  Carried unchanged from v0.74.

## COVERAGE THIS GATE DID NOT PROVIDE

- **Bare metal and Proxmox are untested.** Every result is QEMU, TCG, no KVM.
- **One boot per fresh-image configuration**, six across two dirty-SMP runs and
  three uniprocessor. These are not soak runs. Six dirty-SMP boots against a
  1-in-6 event is the smallest sample that is not self-deceiving, **not** a
  demonstration that the event is gone — neither for `mcpre` nor for the
  `pthreads_smp` wake loss, which did not recur here and remains unexplained.
- **Neither open intermittent has a negative control.** For `mcpre` a 3-boot
  control on the pre-sweep tree passed, which at 1-in-6 proves very little; it
  was run before the fix existed and has not been repeated against it.
- **The warning sweep is verified by the gate, not by inspection of every
  site.** 16 of the 44 changes are whitespace, but the sweep touched a compiler,
  a kernel and a driver probe, and only a booting matrix distinguishes "compiles
  the same" from "behaves the same".
