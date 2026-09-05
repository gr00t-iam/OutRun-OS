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
