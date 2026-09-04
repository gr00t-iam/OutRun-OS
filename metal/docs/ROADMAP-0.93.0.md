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
