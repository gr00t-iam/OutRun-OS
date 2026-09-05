# OutRun OS v0.93 — real time, and what an epoch is worth

Status: **cycle open.** Nothing here is tagged.

## WHERE THIS STARTS

v0.92.0 at `b8c3447`, artefact `outrun-os-0.92.0.iso`, md5
`becf2d8e24902349e4e3542c1240e50c`, `release-verify` PASS.

v0.92 gave this kernel three clocks it could trust and one it could not: the
ACPI PM timer as ground truth, a calibrated PIT, a re-anchored TSC clocksource
accurate to 0.03–0.06% — and `CLOCK_REALTIME`, which had to return the monotonic
value because **nothing in the kernel knew what year it was**.

---

## Objective 1 — DONE: the CMOS RTC, and a real epoch

### What was actually missing

v0.92 recorded "there is no RTC". That was true of the **driver** and not of the
machine: the MC146818 has been at ports `0x70`/`0x71` since the PC/AT and QEMU
emulates it. The comment has been corrected, because as written it told a future
reader that an epoch was impossible here.

**Implemented in `kernel64.c`, not `kernel/drivers/rtc.c` as the brief asked.**
The kernel is one source file by design — CLAUDE.md states it and `kernel/`
contains exactly `kernel64.c` — and a new file would need Makefile object-list
changes for no benefit. It sits beside the ACPI PM timer because it is the same
kind of thing: a hardware clock read through port I/O.

### The update race is the whole difficulty

The RTC increments its registers in place. A read that straddles an update can
return an hour from before the carry with minutes from after it — **wrong by an
hour, and only occasionally**, which is the worst shape a bug can have. Two
defences, both needed:

1. Wait for Status Register A's UIP bit to clear.
2. **Read the whole set twice and require agreement.** An update that begins
   after the UIP check still produces two differing reads; the retry costs a
   microsecond and closes the window the first defence leaves open.

Both loops are bounded. An RTC that never settles must not hang the boot.

BCD/binary and 12/24-hour come from Status Register B. The 12-hour conversion is
applied **after** the BCD decode, because 12 AM is stored as 12 and must become
0 while 12 PM must stay 12 — the pair of cases a naive `if (pm) h += 12` gets
wrong at both noon and midnight.

### The plausibility floor

`rtc_init` rejects any epoch before **1,700,000,000** (2023-11-14). A flat CMOS
battery reading 1980, or an unset clock reading 1970, is **worse than no epoch**:
a caller would trust it. Rejection is loud and leaves `CLOCK_REALTIME`
boot-relative, exactly as v0.92 behaved.

### Measured

```
[rtc] CMOS RTC: 2026-9-4 12:56:37 UTC — epoch 1788526597, CLOCK_REALTIME anchored
```

**The conversion was checked against an independent implementation** rather than
trusted: GNU coreutils `date -u -d '2026-09-04 12:56:37' +%s` returns exactly
1788526597, and the reverse conversion returns the original date. Agreement to
the second, both directions.

From ring 3, across two runs: `CLOCK_REALTIME` read 1,788,527,072 then
1,788,527,074 — above the floor and advancing.

### What the ring-3 check tests, and why the second half matters

Role 63 asserts `CLOCK_REALTIME > 1.7e9` **and** that `CLOCK_MONOTONIC` did not
absorb the wall-clock offset. The second is the one that would otherwise fail
silently: anchoring REALTIME by adding to the shared base would make MONOTONIC
jump by 56 years, and every interval measured with it would still *look*
plausible — small differences of enormous numbers. The check bounds MONOTONIC at
one day, which no boot approaches.

Both passed, `exit 1870`, both runs.

### Deliberately NOT changed: on-disk timestamps

`dirent.mtime`/`atime` remain boot-relative ticks. An epoch now exists, but these
are written to volumes: reinterpreting them would make a v0.92 volume's tick
values read as absolute seconds under a v0.93 kernel, silently and for every file
already stored. Migrating them needs a format decision and a mount-time
conversion, not a redefinition.

### Verification

| tier | result |
|---|---|
| `smp4-bios` | 45 suites, 583 passed, 0 failed, 0 ranks |
| `smp4-iommu` | 47 suites, 597 passed, 0 failed, 0 ranks |
| ring-3 clock probe | OK (1870) × 2, epoch valid, MONOTONIC unaffected |
| `nanosleep` fidelity | 100 µs → 163–204; 1 ms → 1043–1076; 10 ms → 10018–10037 µs, none short |

**Not covered:** `uniprocessor`, `smp2-bios`, `smp8-bios` were not run for this
change; `gate-dirty` has not run since v0.90.0. The RTC is read once at boot, so
a wrong read would be visible in the banner of every boot above.

---

## Objective 2 — DONE: POSIX clocks, and one bug wearing four costumes

### The clock ids were wrong, and the tree said so already

v0.92 defined `CLOCK_MONOTONIC 0` and `CLOCK_REALTIME 1`. **That is inverted
from POSIX**, where REALTIME is 0 and MONOTONIC is 1 — and the `SEEK_*` comment
three lines below that definition states the exact principle it broke:

> *"They are not an internal encoding to be chosen freely — a ring-3 program
> written against any C library expects 0/1/2 to mean exactly this."*

Corrected to REALTIME 0, MONOTONIC 1, plus MONOTONIC_RAW 4. The only caller is
in this tree, so the ABI break costs nothing now and would have cost every
future ring-3 program later.

`CLOCK_MONOTONIC_RAW` is accepted and answers identically to MONOTONIC. On Linux
they differ because MONOTONIC is NTP-slewed; nothing here slews anything, so
they genuinely are the same clock. Accepting it means a program written against
a real libc gets an answer rather than `EINVAL`, and returning the same value is
honest rather than a stub.

### Two structural limits the brief did not anticipate

**`metal/user/sys.h` does not exist.** The SDK header is an embedded string in
`kernel64.c`; the wrappers live in `init.c`, which is where the bindings went.

**The syscall ABI carries three arguments.** `syscall_dispatch` and
`syscall_trap` both take `a0/a1/a2` — there is no `a3` — so POSIX's four-argument
`clock_nanosleep(clk, flags, req, rem)` cannot pass `rem` to the kernel. The
kernel takes three; the ring-3 wrapper presents the POSIX signature and zeroes
`rem` itself. That loses nothing here: no signal reaches a sleeping thread, so a
remainder cannot arise. If interruption is ever added, `rem` has to come back and
an argument has to be packed to make room.

### ONE DEFECT, FOUR APPEARANCES

Four rounds of testing, each exposing a real fault the previous fix had
concealed. They are recorded together because they are the same mistake:

| # | symptom | what it actually was |
|---|---|---|
| 1 | ids inverted | v0.92 chose non-POSIX numbers |
| 2 | ABSTIME "woke early", **772 us spread** between runs | the TEST: baseline taken from a second syscall, so it measured `T − (origin + one call)` |
| 3 | ABSTIME short by ~720 us, **42 us spread** | the KERNEL: target expressed in `ktime`, sleep executed on the PM timer |
| 4 | relative sleeps short in `ktime` while correct in PM | the mirror image of 3 |

The spread is what separated 2 from 3. A kernel waking early on a fixed target
cannot vary by 772 us between runs; the cost of an intervening syscall can. Once
the baseline was corrected the spread collapsed to 42 us and the residual
shortfall became too repeatable to be anything but real.

**The underlying error was treating "the accurate clock" as a single thing.** It
is not. The ACPI PM timer is the only clock that cannot run fast, which is why
Objective 4 pointed `ksleep_us` at it. `CLOCK_MONOTONIC` is the only clock ring 3
can see. Section 1d measured the TSC spanning 7.2% within a boot, so a sleep that
satisfies one of those clocks can legitimately look SHORT on the other, and both
readings are correct.

Two fixes follow from stating it that way:

- **`ksleep_us` waits for whichever clock is slower.** The PM timer bounds it in
  real time; a top-up against `ktime_get_ns` bounds it in the clock the caller
  measures with. "At least N" is the contract and overshoot is permitted by it,
  so taking the maximum is correct rather than a fudge.
- **`TIMER_ABSTIME` re-checks and converges in its target's own clock.** An
  absolute deadline must be satisfied in the clock it was expressed in, so the
  kernel re-reads that clock after each sleep and sleeps the remaining distance
  as that clock sees it. Bounded at 64 passes — a deadline unreachable in 64
  halvings means a broken clock, and a syscall must not spin forever. A
  sub-microsecond remainder polls directly, since `ksleep_us` would round it to
  zero and spin the loop.

### Measured, three runs at `-smp 4`

| check | run 1 | run 2 | run 3 |
|---|---|---|---|
| `clock_getres` MONOTONIC / REALTIME | 1 ns / 1 ns | 1 ns / 1 ns | 1 ns / 1 ns |
| invalid clock id | `-EINVAL` | `-EINVAL` | `-EINVAL` |
| `CLOCK_REALTIME` | 1788572483 | 1788572485 | 1788572487 |
| **`TIMER_ABSTIME` +5000 us** | **5100** | **5079** | **5119 us** |
| `nanosleep` 100 us | 200 | 164 | 162 us |
| `nanosleep` 1 ms | 1135 | 1082 | 1090 us |
| `nanosleep` 10 ms | 10143 | 10186 | 10204 us |
| probe verdict | OK (1870) | OK (1870) | OK (1870) |

Every sleep overshoots; none returns early, on either the PM timer (measured
kernel-side) or `CLOCK_MONOTONIC` (measured from ring 3).

`clock_getres` derives its answer from `g_tsc_khz` rather than hardcoding 1 ns.
At the measured ~3.86 GHz it computes to 1 ns, which satisfies the requirement —
but on a slower TSC it would report more, rather than claiming a resolution the
clock cannot deliver.

### What role 63 checks beyond the brief

An **invalid clock id must be refused**: a clock layer that accepts anything will
silently return the wrong clock the first time a caller passes a constant this
kernel lacks. And **a target already in the past must return immediately** —
where an unsigned subtraction would wrap to roughly 584 years, which is why the
kernel tests it with a signed compare before any arithmetic.

### Verification

| tier | result |
|---|---|
| `smp4-bios` | 45 suites, 583 passed, 0 failed, 0 ranks |
| `smp4-iommu` | 47 suites, 597 passed, 0 failed, 0 ranks |

**Not covered:** `uniprocessor`, `smp2-bios` and `smp8-bios` were not run for this
change, and `gate-dirty` has not run since v0.90.0.

---

## Objective 3 — SPEC: interval timers and CPU time accounting

Status at time of writing: **specified, not yet implemented.** Results land below.

### Scope

| syscall | number | what it must do |
|---|---|---|
| `SYS_SETITIMER` | 108 | arm/disarm `ITIMER_REAL` (0) and `ITIMER_VIRTUAL` (1), returning the previous value |
| `SYS_GETITIMER` | 109 | report the remaining time and the reload interval |
| `clock_gettime` | 104 | additionally accept `CLOCK_PROCESS_CPUTIME_ID` (2) and `CLOCK_THREAD_CPUTIME_ID` (3) |

### Target criteria

1. `ITIMER_REAL` fires `SIGALRM` on a monotonic deadline, never early, and re-arms
   itself when an interval is set.
2. `ITIMER_VIRTUAL` fires `SIGVTALRM` against consumed process CPU time, not
   wall-clock.
3. `CLOCK_PROCESS_CPUTIME_ID` strictly increases while the process computes and
   stays flat while it sleeps.
4. Every `struct itimerval` pointer is bounds-checked with `access_ok` before any
   read or write, and a bad pointer returns `-EFAULT` without partial writes.
5. `smp4-bios` and `smp4-iommu` at 0 failures and 0 rank faults.

### Two decisions this objective has to make before writing any code

**`alarm()` and `setitimer(ITIMER_REAL)` are ONE timer, not two.** POSIX is
explicit that they share state and interfere with each other. This kernel already
has `alarm_deadline`, a `g_ticks` value driving `SIGALRM` from `sig_check_alarms`.
Adding a second, independent REAL timer beside it would produce a kernel where
arming an itimer silently fails to cancel a pending alarm — two timers racing to
deliver the same signal to the same process. So `alarm_deadline` is being
converted in place: one field, expressed in nanoseconds, that both syscalls
write. `SYS_ALARM`'s tick-granularity ABI does not change.

**What counts as CPU time, stated before it is measured.** The accounting hook is
the ring-3 excursion in `cpu_exec_proc` — stamped before `enter_user_*` and
accumulated after it returns. That window contains user time AND the kernel time
spent servicing this process's syscalls, which is the right answer:
`CLOCK_PROCESS_CPUTIME_ID` is utime+stime, not utime alone.

It also contains time parked inside a *sleeping* syscall, which is NOT CPU time
and would make criterion 3 unsatisfiable — a process asleep in `nanosleep` would
accrue CPU time at wall-clock rate. Sleep duration is therefore measured and
subtracted explicitly.

The honest consequence, recorded here rather than discovered later: this kernel
cannot separate utime from stime, because it has no ring-0/ring-3 accounting
split. `ITIMER_VIRTUAL` is specified by POSIX against user time alone, and what
it actually gets here is process CPU time. For a workload that computes in ring 3
these differ by the cost of its syscalls. **`ITIMER_VIRTUAL` is therefore
approximate by construction**, and the ring-3 probe must not assert a tight bound
on it.

### Objective 3 — RESULTS

| criterion | result |
|---|---|
| 1. `ITIMER_REAL` fires, never early | armed 50,000 us → fired 50,123 / 50,106 / 50,312 us |
| 2. `ITIMER_VIRTUAL` against CPU time | implemented, fires `SIGVTALRM`; see the caveat above |
| 3. CPU clock advances computing, flat asleep | +20,097 us over 20,000 us of compute; **+91 us over 51,699 us of sleep** |
| 4. `access_ok` on every `itimerval` | 32 bytes checked both directions; `old` may alias `new` |
| 5. matrix at 0 fail / 0 rank | see the verification table |

`getitimer` reported 49,898 us remaining of a 50,000 us timer. `clock_nanosleep`
on a CPU-time clock returns `-EINVAL`, and an unknown clock id still does.

### THREE REAL DEFECTS, AND TWO OF MY OWN MEASUREMENTS THAT WERE WRONG

Recorded together because separating them is the only way the next reader can
tell which findings are about the kernel and which are about the instrument.

**Kernel defects, each independently proven:**

1. **CPU time was only accumulated when an excursion CLOSED.** A task reading its
   own CPU clock is by definition inside an open one, so it never saw the time it
   had burned since its last dispatch. Measured: 20 ms of ring-3 compute reported
   `+0 ns`. Fixed with `cpu_run_t0` / `cpu_run_excl0`, published at dispatch, and
   `cpu_ns_inflight()`.

2. **Sleeps were billed to the wrong task.** `krelax()` calls `sched_yield()` on
   cpu 0, which runs other processes and reassigns `g_cpu[0].cur_proc`. The
   charge helper read `current_proc_idx` AFTER that, so it credited whichever
   task had run most recently. Proven by printing the counter rather than
   inferring it: of a 52,522 us sleep only ~30,000 us reached the sleeper. Fixed
   by capturing the task index once, at entry (`ksleep_charge_to`).

3. **`alarm()` and `setitimer(ITIMER_REAL)` would have been two racing timers.**
   Caught in design, before it could be written — see the decision above.

**And two mistakes in my own instrument, which cost more time than the defects:**

- **A window that measured more than the sleep.** The check read the process
  clock before a `print()` and after the sleep, and reported that 41% of a 50 ms
  sleep was billed as CPU time. It was not: `print()` writes to the serial
  console and costs real milliseconds of real computation, and the window
  contained it. The THREAD clock, read either side of the sleep alone, showed
  +64 us across the same sleep — and that discrepancy between two clocks that
  read the same underlying value is what exposed it. **This is the same error as
  Objective 2's ABSTIME baseline, in the same cycle.**

- **A threshold loose enough to pass the bug it was written for.** The first
  version allowed CPU time to grow by half the sleep duration, and duly returned
  OK on a build measuring 43%. Tightened to a quarter. A bound that admits the
  defect is not a test, which is the same lesson as `a test that cannot fail`
  one step removed.

**One hypothesis falsified by control, and the change reverted.** Believing a
sleep longer than a quantum is preempted mid-way and mis-billed at the excursion
boundary, the PM-timer loop was split into 2 ms steps charging as it went. The
control — charge once at the end, single 4,000,000 us chunk — gave 83 us against
the split version's 91 us. No difference. The machinery was removed rather than
kept on a plausible story.

**One hypothesis falsified outright.** `current_proc_idx` was suspected of being
stale after a `krelax()` yield, which would have been a kernel-wide `access_ok`
hazard rather than a timer bug. `SYS_GETPID` either side of a sleep returned the
same pid both times. It is not stale; the concern was unfounded and is recorded
so it is not re-raised.

### A harness error worth writing down

A boot launched without `$(QEMU_BLK)` panics with a **divide error in
`cas_index_find`** — the CAS superblock reads back `index_blocks = 0`, and
`% slots` divides by zero. It looks exactly like a kernel regression. It is not:
the same bare invocation panics identically on the already-verified v0.93
Objective 2 image, which is the control that settled it. `make qemu` passes
`$(QEMU_GPU) $(QEMU_BLK) $(QEMU_NET)`; a hand-rolled qemu line must too.

Separately, a fixed `sleep N` before typing a console command is not a deadline.
One run typed `timebench` before the prompt existed and reported nothing at all.
Wait for the prompt in the log.

### Verification

Image `3e3fedb3b40e9bd1f2199f9d818e3afe`, clean build, zero compiler warnings.

| tier | result |
|---|---|
| `smp4-bios` | OK — 45 suites, 583 passed, 0 failed, 0 ranks (450 s) |
| `smp4-iommu` | OK — 47 suites, 597 passed, 0 failed, 0 ranks (465 s) |

Both counts are identical to the pre-Objective-3 baseline, so the shared-timer
conversion of `SYS_ALARM` changed no existing assertion. `smp4-iommu` needed
`GATE_CAP=2400`; at the 900 s default it is cut off before the prompt.

**Not covered:** `uniprocessor`, `smp2-bios`, `smp8-bios`, `gate-dirty` and
`gate-dirty-smp`. `ITIMER_VIRTUAL` is implemented and reachable but **no ring-3
test arms it** — role 63 exercises `ITIMER_REAL` only, so the VIRTUAL path has
compiled and is wired to `SIGVTALRM` without having been shown to fire. It is
listed here rather than claimed as verified.

### The GRUB literal, and why it drifted for sixty releases

`grub.cfg` said `kernel 0.30.0-metal` since v0.31.0. It is the FIRST version
string a user sees — on screen before the kernel prints anything — and nothing
checked it. `release-version-check` now reads it and compares against the same
`want` the kernel banner is held to.

`want` is recomputed in that recipe line rather than carried over: every make
recipe line runs in its own shell, so reusing the variable from the check above
would compare against an empty string and pass silently.

The check was confirmed to FAIL before it was trusted — restoring the old
`0.30.0-metal` string makes it warn, and putting it back makes it pass.

### Objective 3 — multi-core validation (second sample)

The first matrix pass verified the SUITES. It did not verify the probe, because
**the gate never runs it**: `gate-matrix.sh` launches qemu with `< /dev/null`
and types nothing, and role 63 only runs when `timebench` is entered at the
shell. Both tier logs from the first pass contain zero occurrences of it. So the
probe was run separately under each tier's exact device configuration — the
first time it has executed behind VT-d.

Image `7ec553b76b7eb98e1eea7cc74d6d5014`, rebuilt from `d319e51`.

| tier | suites | passed | failed | ranks | time |
|---|---|---|---|---|---|
| `smp4-bios` | 45 | 583 | 0 | 0 | 435 s |
| `smp4-iommu` | 47 | 597 | 0 | 0 | 450 s |

| probe check | `smp4-bios` | `smp4-iommu` |
|---|---|---|
| `ITIMER_REAL` armed 50,000 us | fired 50,107 us | fired 50,122 us |
| `getitimer` remaining | 49,937 us | 49,941 us |
| CPU time over 20,000 us compute | +20,044 us | +20,038 us |
| CPU time over a ~50 ms SLEEP | **+73 us (0.13%)** | **+60 us (0.12%)** |
| verdict | OK (1870) | OK (1870) |

Note the ISO md5 differs from the first pass despite an identical tree —
`grub-mkrescue` output is not byte-reproducible. The md5 identifies which binary
a log came from; it is not a claim that the build is deterministic.

### Per-core evidence, and one correction to how this was framed

`cpu_run_t0` and `cpu_run_excl0` are fields of `struct kproc`
(`kernel64.c:2028-2029`) — **per TASK, not per CPU**. They are not in
`struct cpu_local` / `g_cpu[N]`. That distinction decides what can even be
asserted about them:

- There is no per-CPU replica to corrupt. Each is written only by the core
  currently dispatching that task, and a task is dispatched by exactly one core.
- **This path takes no locks**, so cross-core lock contention cannot arise in it
  by construction. `ksleep_charge_to` uses `__sync_fetch_and_add`; the dispatch
  stamps are plain volatile stores. `ranks=0` from the gate is real, but it is
  not evidence about a lock that does not exist.

What the logs DO show per core is the LAPIC timer, calibrated independently on
each secondary core against the ACPI PM timer:

| tier | cpu1 | cpu2 | cpu3 | spread |
|---|---|---|---|---|
| `smp4-bios` | 62,467,873 Hz | 62,411,999 Hz | 62,389,182 Hz | 0.13% |
| `smp4-iommu` | 62,598,566 Hz | 62,507,126 Hz | 62,387,386 Hz | 0.34% |

Each derives its own initial count (~625,000) for the 10 ms quantum.

No backward time steps and no underflows in either tier. Every match for those
terms is a counter REPORTING zero (`underflow 0 -> 0`) — live instrumentation
reading zero, not a grep for a string nothing emits.

`smp4-iommu` came up with DMA remapping enabled (`GSTS.TES`), root table
programmed (`GSTS.RTPS`), 3-level second-level tables and interrupt remapping,
all four cores online with timer vectors allocated. `capdma` passed 12/12,
including the deliberately blocked cross-domain DMA that is its confinement
proof.

**Still not covered:** `ITIMER_VIRTUAL` is wired to `SIGVTALRM` but no ring-3
test arms it. `uniprocessor`, `smp2-bios`, `smp8-bios` and both dirty-volume
gates were not run for this objective. Role 63's sleeps carry no per-core
attribution, so the drift figures above are per-process, not per-core; the LAPIC
table is the per-core measurement.

---

## Pre-release hardening — ITIMER_VIRTUAL was broken, and the probe is what found it

Objective 3 shipped `ITIMER_VIRTUAL` in `d319e51` with the note that no ring-3
test armed it. Writing that test found **two real kernel defects**. Both were in
shipped code. Neither would have been caught by tagging on the Objective 3
evidence, because nothing in that evidence ever armed the timer.

### Defect 1 — the timer could never fire for the workload it exists to serve

`sig_check_alarms` compared the target against `proc_cpu_ns(i)`, the ACCUMULATED
CPU total. That total only advances when a ring-3 excursion CLOSES, and a
CPU-bound process can run its entire life inside one open excursion. Measured
directly, with the comparison instrumented:

    [vtdbg] SCAN pid 763 used=0 vd=383999 delta=-383999 exited=0

`used` reads **zero** while that same process ended with 423,993 us of CPU
accumulated — all of it in flight, none of it closed. `vd` decomposes as
`0 accumulated + 333,999 in flight + 50,000 requested`, which is the same fact
from the other side.

This is the SAME defect fixed for `clock_gettime` earlier in this cycle
(`cpu_ns_inflight`, Objective 3). The clock was fixed; the timer scan was left
reading the stale basis. Arming already used the live total, so the two halves
were comparing different quantities and could never meet. Both now use
`proc_cpu_ns(i) + cpu_ns_inflight(i)`.

### Defect 2 — sleep-charge granularity is invisible to the sleeper and obvious to an observer

With defect 1 fixed the timer fired — **in the middle of a 100 ms sleep**,
remaining going 49,778 -> 0.

`ksleep_charge_to` charged once, on completion. `cpu_ns_inflight` computes
`elapsed - slept`, so for the whole duration of a sleep `elapsed` climbs while
`slept` stays flat. The sleeping task itself never notices — it reads its own
clock only after the final charge lands. Any CONCURRENT observer sees its CPU
time racing at wall-clock rate, and `sig_check_alarms` running on another core
is exactly that observer.

**This is the change an earlier control told me to revert.** That control was
correctly run and correctly reported: charge-at-end gave 83 us against the split
loop's 91 us across a 50 ms sleep. It was also narrower than the conclusion
drawn from it — it measured only `clock_gettime` called by the sleeping task
itself, which cannot distinguish the two designs. The distinguishing case is an
observer reading mid-sleep, and no such observer existed until this probe
created one. The incremental loop is restored, and the comment in `ksleep_us`
records why so it is not reverted again on the same reasoning.

### Measured, after both fixes

| check | result |
|---|---|
| `ITIMER_VIRTUAL` across a 100,000 us sleep | remaining 49,931 -> 49,904 (**27 us**) |
| fired during that sleep | no |
| armed 50,000 us of CPU | **fired after 50,114 us of CPU** |
| probe verdict | OK (1870) |

Late by 114 us, never early — the same standard applied to sleeps and to
`ITIMER_REAL` throughout this cycle.

### A harness failure worth recording, because it cost four hours

A probe run sat for 3 h 49 m. Two causes, compounding:

- The guest hung in `posixstrs` waiting for a child (~840 s of spin) and never
  reached the prompt. It did NOT reproduce: a control on the committed HEAD
  image and a re-run of the same image both booted clean. Intermittent, and
  consistent with the host degradation recorded in ROADMAP-0.91 §1a.
- The feeder waited for the prompt with an unbounded `while ! grep`. `timeout`
  bounded QEMU, not the feeder, so when the marker never appeared the wait ran
  forever. **A timeout must be a deadline** — the same rule this tree already
  states for ring-3 waits applies to the harness driving it.

`.logs/probe.sh` now bounds every wait and prints `PROMPT: NEVER REACHED`
instead of hanging.

### Documentation

`README.md` and `metal/INSTALL.md` referenced `outrun-os-0.2.0.iso` — stale for
most of the project's life, for the same reason `grub.cfg` was: nothing checks
them. They now read `outrun-os-<VERSION>.iso` with a note naming
`metal/Makefile` as the source. A placeholder cannot go stale; a pinned number
would simply restart the decay.
