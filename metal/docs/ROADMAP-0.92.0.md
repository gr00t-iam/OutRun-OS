# OutRun OS v0.92 — subsystem isolation, memory reclamation, IPC latency

Status: **draft, cycle not opened.** `VERSION` still reads `0.91.0` at the time
of writing; nothing here is a commitment and nothing has been measured.

## WHERE THIS STARTS

v0.91.0. Three things landed and each is recorded with the evidence behind it in
`ROADMAP-0.91.0.md`: the `g_gpu_cmdbuf` bounds guard (`9e94f5e`), the measured
VFS rwlock A/B — ~3.7% faster reads, 2.6x lower contention (`a052543`) — and the
`capdma` repair, ~50% flake to 12 of 12 across four boots (`3876722`).

**The cycle also inherits an unresolved host constraint**, and Objective 1 exists
because of it.

---

## Objective 1 — WSL2 host performance counters and timing primitives

**Why.** v0.91 proved, with an identical-image control, that the toolchain host
became ~48% slower mid-cycle: the same md5 that booted uniprocessor in 310 s now
takes 460 s, with no kernel change responsible. Host load, memory pressure, CPU
capability, KVM availability and the QEMU/grub binaries were each eliminated with
evidence. CPU frequency and power state remain the leading hypothesis and are
**untested**, because `/sys/devices/system/cpu/*/cpufreq` does not exist under
WSL2 — the governor is a Windows-side setting the guest cannot read.

That is not a nuisance. It blocks Objective 1b of v0.91 (the 100-iteration soak,
~14 hours at this speed), it is the most likely cause of the `langstrs` wall-clock
timeout seen in one boot of four, and every timing budget in this tree —
`GATE_CAP`, `GATE_DIRTY_CAP`, the ring-3 suite deadlines, every `g_ticks`
watchdog — was calibrated on the fast host and has never been re-derived.

**What this objective actually is:** finding a measurement that survives the
host varying, not making the host fast.

1. **A host-side reading.** From Windows rather than the guest: power plan,
   processor performance state, thermal/battery status. This is the one piece
   that cannot be done from inside WSL2 and it is the piece that would settle it.
2. **A guest-side proxy that is comparable across sessions.** v0.91 recorded a
   fixed bash loop at 3.37/3.55/3.52 s as a baseline because the absence of one
   made that triage expensive. That is crude. A better primitive would calibrate
   TSC against `g_ticks` at boot and print the ratio, so every log states the
   speed of the machine that produced it.
3. **Then re-derive the budgets** from whatever "normal" turns out to mean, in
   one pass, recorded together — the same way CLAUDE.md requires for enabling
   KVM.

**A result of "the host is simply slower now and here is the number" is a
result.** It would let every subsequent measurement be normalised instead of
argued about.

### 1a — AUDIT: what this kernel's clocks actually are

| clock | state |
|---|---|
| PIT @ 100 Hz | `pit_init()` writes divisor 11932 (1193182/100); `g_ticks++` in the ISR. The only wall-clock reference. |
| TSC | `rdtsc64()` exists; used for **relative deltas only** |
| LAPIC timer | periodic, vector 51, **hardcoded** initial count 3,000,000, divider 16 |
| ACPI PM timer | **does not exist** — zero references |
| `rdtscp` | **not used** — zero references |

**`g_ticks` is not calibrated against anything.** The PIT is *programmed* to
100 Hz and trusted; nothing verifies it delivers 100 Hz, and the TSC has no
established relationship to it anywhere in the kernel. Every cycle figure this
tree has ever printed — the v0.89 CAS spin telemetry, v0.91's `vfsbench`
histograms — is therefore uncalibrated.

**Second finding, unprompted:** the AP slice timer's LAPIC initial count is a
hardcoded 3,000,000 with divider 16, commented "~tens of ms". It is derived from
nothing. The AP preemption period is unknown and would drift with host speed
exactly like the budgets under investigation.

### 1b — MEASURED: `cmd_timebench`, six runs

Three uniprocessor, three `-smp 4`, one boot each configuration.

| metric | uniprocessor ×3 | `-smp 4` ×3 |
|---|---|---|
| TSC per PIT tick | 38,697,338 / 38,696,938 / **41,634,941** | 39,218,903 / 39,219,921 / 38,695,238 |
| implied TSC Hz | 3.870 / 3.870 / **4.163** GHz | 3.922 / 3.922 / 3.870 GHz |
| `rdtsc` back-to-back, mean | 88 / 88 / 89 cyc | 91 / 110 / 92 cyc |
| `rdtsc` p50 / p99.9 | ≤64 / ≤128 | ≤64 / ≤128–256 |
| **max delta** | 137k / 82k / 146k | **376k / 305k / 277k** |
| spikes >1k cyc | 30 / 22 / 43 ppm | 41 / 43 / 42 ppm |
| `klock` acquire+release | 647 / 636 / 650 cyc | 681 / 679 / 664 cyc |
| `PAUSE` | 379 / 391 / 377 cyc | 356 / 391 / 335 cyc |

**THE HEADLINE: the TSC and the PIT disagree by up to 7.6%, within a single
boot.** Five of six readings cluster at 38.70–39.22 M cyc/tick (1.35% spread);
one reads 41.63 M. Nothing changed between runs but time.

**And there is no third clock to say which one drifted.** This kernel has exactly
two time sources, so their *ratio* is measurable and their *truth* is not. An
ACPI PM timer — fixed 3.579545 MHz, independent of both CPU frequency and the
PIT — is the standard tiebreaker, and it does not exist here. **Adding one is now
the highest-value item in this objective**, because without it no amount of
further measurement can attribute the drift.

**`PAUSE` costs ~380 cycles** under TCG, against ~10–140 on real hardware. That
retroactively explains a class of bug this tree keeps finding: `capdma`'s old
2,000,000-iteration wait was ~760 M cycles ≈ 0.2 s at 3.87 GHz, and that product
moves with host speed. A spin count is not a budget; it is a budget multiplied by
an unknown.

**Host preemption is rare, large, and larger under SMP.** 22–43 events per
million samples in both configurations, but the worst case grows from 82–146 k
cycles (21–38 µs) uniprocessor to 277–376 k (71–96 µs) at four vCPUs — four
guest threads competing for host cores. Rare enough that it cannot by itself
explain a 48% throughput drop.

### 1c — the `langstrs` question, and what this data does NOT settle

The brief asked whether the compiler timeouts come from TSC drift or from VM
tick compression. **On this evidence, neither.** `langstrs` budgets in `g_ticks`
via `SYS_SYSINFO` and never reads the TSC, so TSC drift cannot reach it; and
tick *compression* would make a tick budget cover more work, not less.

The reading this data supports is duller: if the PIT delivers real 100 Hz, a
tick budget is a real-time budget, and a slower host simply completes less work
per tick until the compile no longer fits.

**That is a reading, not a finding, and the difference matters here.** It rests
on the PIT being accurate, which is precisely what the 7.6% disagreement above
leaves open — with only two clocks, the excursion could be the TSC or the PIT and
this instrument cannot tell. Confirming it needs the third clock. This cycle has
already produced several confident timing attributions that were later
falsified; this one is left explicitly unresolved rather than joining them.

### 1d — RESOLVED: the ACPI PM timer, and which clock was lying

The third clock exists now. `acpi_pm_init()` parses the FADT (`FACP`) through the
`acpi_find_table()` walker already used for DMAR and MADT, reading `PM_TMR_BLK`
(offset 76), `PM_TMR_LEN` (91) and the `TMR_VAL_EXT` flag (bit 8 of offset 112)
to tell a 24-bit counter from a 32-bit one, with `X_PM_TMR_BLK` as a fallback for
System-I/O space only. It enumerates here at **I/O port 0x608, 24-bit**.

It is the right referee because its rate is fixed by specification at 3,579,545 Hz
— one third of the NTSC colour burst — independent of CPU frequency, of power
state, and of the PIT divisor, and it cannot be reprogrammed.

`time_calibrate_clocks()` runs at boot, after `multiboot_scan()` supplies the
RSDP and before `iommu_init()` and `pci_init()`.

**THE ANSWER: the PIT is sound; the TSC is not.**

| | boot | run 1 | run 2 | run 3 |
|---|---|---|---|---|
| **PIT** | 9997 | 9999 | 10003 | 9999 centi-Hz |
| **TSC** | 4.252 | 3.966 | 4.192 | 4.192 GHz |

The PIT holds **99.97–100.03 Hz, ±0.03%**. The TSC spans **3.966–4.252 GHz, a
7.2% spread**, on one host across one boot. §1b's "7.6% anomaly" was the TSC all
along; with two clocks it was unattributable, and with three it took one boot.

**This closes §1c.** A `g_ticks` budget is a real-time budget, because the PIT
really does deliver 100 Hz. So the duller reading there is now supported: a
slower host completes less work inside a real-time budget until the compile no
longer fits. It is no longer "a reading, not a finding".

**And it means the TSC must not be trusted for intervals.** Every cycle figure
this tree prints — the v0.89 CAS spin telemetry, v0.91's `vfsbench` histograms,
`timebench`'s own — is drawn from a clock that moves 7% within a boot. They
remain valid for A/B comparison inside one measurement window and are not
absolute.

#### The measurement was wrong before it was right

The first version of this calibration ran a fixed ~50 ms window on the PM timer
and counted PIT edges inside it. At 100 Hz such a window holds four or five
edges depending on where it starts — and four versus five *is* 7997 versus 9997
centi-Hz. It reported 9995–10000 on four boots, which looked like precision, and
7997 on the fifth, which exposed it: the figure was reporting edge alignment, not
rate. A conclusion had already been drawn from the first four and had to be
withdrawn.

The window is now bounded by **tick edges** — exact by construction, no partial
period at either end — with the PM timer supplying the resolution across them.
What caught it was the calibration printing a loud warning about its own
surprising output rather than a plausible number.

### 1e — spin budgets converted to deadlines

`HW_WAIT(cond, us)` waits for a time and returns whether the condition arrived.
It falls back to an iteration cap when no PM timer was found, because a deadline
with no clock is an unbounded loop, and an unbounded wait on a device that never
answers hangs the machine with no output — the worst failure mode this project
recognises.

`udelay()`/`mdelay()` are driven by the **PM timer directly, not by a calibrated
TSC constant**, precisely because of the 7.2% intra-boot drift above: a delay
built on a boot-time TSC figure would run ~7% long for the rest of the boot.

Audit of hardcoded spin budgets, 13 hardware waits in total:

| site | disposition |
|---|---|
| `iommu_enable` ×4 (RTPS, CCMD, IOTLB, TES) | **converted**, 100 ms deadlines |
| `iommu_invalidate_all` ×2 | **converted** — runs on every device grant and revoke, and an invalidation that has not completed leaves the device translating through stale entries, so this is a correctness hazard and not only a timing one |
| xHCI ×4 | **not converted.** CLAUDE.md keeps xHCI out of the main gate because its emulated microframe timer makes a full boot impractical, so converting it would change driver code the gate never exercises |
| `ps2_wait_in` / `ps2_wait_out` | **not converted.** They run before ACPI is parsed, so `HW_WAIT` would take its iteration-cap fallback and nothing would improve |
| `sha256` benchmark loop, `capdma` refault poll | **not converted** — workloads and a bounded poll, not hardware waits |

**Still uncalibrated at the time of writing:** the AP slice timer's hardcoded
LAPIC initial count of 3,000,000 (divider 16). Deriving it from the PIT is
straightforward now that a reference exists, but it changes AP preemption
frequency for every suite at once, and that belongs in its own change with its
own verification rather than folded into this one. **Done in §1f below.**

### 1f — THE AP PREEMPTION QUANTUM WAS 48 ms, NOT 10 ms

`lapic_timer_calibrate()` arms a masked one-shot counting down from
`0xFFFFFFFF` at divider 16 and measures elapsed LAPIC ticks across a ~10 ms
window bounded by the ACPI PM timer. It runs per core, and it measures against
the PM timer rather than `g_ticks` because only the BSP's PIT ISR advances
`g_ticks` — an AP calibrating during bring-up has no other readable reference.

| core | measured LAPIC | initial count for 10 ms |
|---|---|---|
| cpu1 | 62,578,894 Hz | 625,788 |
| cpu2 | 62,474,068 Hz | 624,740 |
| cpu3 | 62,503,643 Hz | 625,036 |

**The hardcoded 3,000,000 was 4.8x too large.** At ~62.5 MHz it produced a
**48 ms** period, so the APs were preempting at about **21 Hz while the BSP ran
at 100 Hz**. That asymmetry existed for the whole life of the AP scheduler,
behind a comment reading "~tens of ms" — accidentally accurate, and never
stating which tens or against what.

Every multi-core scheduling result this project has published ran under it: the
v0.89 AP-1 placement investigation, `mcq`/`mcpre` dispatch, and the `threadstrs`
two-core guards that consumed two milestones. It does not invalidate those
conclusions, which were about *whether* work landed on a second core rather than
how often it was interrupted — but it was a silent per-core asymmetry in every
one of them.

Cross-core spread is now **0.17%**, so the quantum is uniform as well as correct.
The historical constant remains as an explicit fallback if the PM timer is
absent or the measurement implausible, and the boot log states which path was
taken rather than leaving it ambiguous.

**Only one site existed.** The brief asked for the constant to be replaced "across
both BSP and secondary AP cores"; the BSP arms no LAPIC timer at all — it
preempts from the PIT on vector 32, which is why `g_slice_on` gates only vector
51.

### 1g — `ktime_get_us()`: monotonic, and NOT accurate

Composes `g_ticks` (monotonic 64-bit base) with the PM timer for sub-tick
resolution, the PIT ISR publishing the PM reading at each tick edge. Lockless
and IRQ-safe by construction, using `g_ticks` as its own sequence counter — the
seqlock idiom without a lock, sound because only the BSP's ISR writes either
field.

Measured rather than assumed, three runs at `-smp 4`:

| | run 1 | run 2 | run 3 |
|---|---|---|---|
| reported | 55,417 us | 60,491 us | 54,104 us |
| true interval (PM) | 50,036 us | 50,001 us | 50,002 us |
| error | +10.8% | **+21.0%** | +8.2% |
| backward steps / 200,000 reads | **0** | **0** | **0** |

**Monotonicity holds absolutely** — zero regressions in 600,000 reads. **Accuracy
does not**: it overshoots by 8–21%.

The overshoot is inherited from `g_ticks` and cannot be fixed at this layer.
Across a genuine 50 ms window the clock saw `g_ticks` advance five edges
sometimes and six others, because the emulated PIT delivers **coalesced catch-up
interrupts** after the host deschedules qemu — several ticks arriving
back-to-back, so tick-derived time runs ahead of real time. Boot calibration
reads 99.99 Hz because its window is short and edge-bounded; a window containing
a stall is where this appears.

**This is the tick compression the brief in §1c asked about**, found at last — but
running the opposite way to the hypothesis. Ticks are not compressed into less
time; they are *deferred and then delivered in a burst*, so a tick count
overstates elapsed time rather than understating it. It does not change §1c's
conclusion: `langstrs` budgets in ticks, and a burst makes a tick budget expire
*sooner* in real terms, which is another way for a loaded host to fail it.

So `ktime_get_us` is for ordering and coarse elapsed time. The accurate
instrument is the PM timer directly (`acpi_pm_us_since`), which is what `udelay`
and `HW_WAIT` are built on. Making it both would require extending the PM
timer's 24 bits in software; nothing here needs that yet.

### The KVM decision sits underneath this

`/dev/kvm` exists on this host; only the build user's group membership keeps QEMU
on TCG. Enabling it would make boots several times faster and would invalidate
every budget above simultaneously — see CLAUDE.md's TCG policy note. If
Objective 1 concludes the host cannot be made predictable under TCG, that is the
moment to weigh the switch, with the re-baselining pass it requires.

---

## Objective 2 — scheduling and memory reclamation

**Deliberately unspecified until something is measured.** v0.89 recorded a
measured *no* on descriptor-layer optimisation after finding `g_ofile_lock` at
~1% under real traffic; v0.90 shipped a lock decoupling whose benefit turned out
to be ~4% and said so rather than rounding up. The pattern worth keeping is that
the measurement chooses the work.

Candidates, in the order the existing evidence supports them:

1. **Where the time actually goes.** v0.91 built `vfsbench` and it reports that
   a 512-byte read spends most of its time in `cas_get` — hashing and copying
   under the rank-3 CAS lock — not in the VFS layer that was optimised. That is
   a lead nobody has followed. The same instrument extended to attribute time
   per layer would say whether the CAS hash, the block copy, or the journal is
   the cost.
2. **Frame reclamation.** `alloc_frame` has no counterpart in the audited paths;
   what the kernel does under sustained allocation pressure over a long boot has
   never been measured. `role 62` and the CAS soak both allocate heavily and both
   only assert that accounting returns to its pre-phase value, which is a
   correctness claim and not a fragmentation one.
3. **IPC latency across cores.** The `mcq`/`mcpre` suites establish correctness
   of cross-core dispatch and say nothing about its cost. `vfsbench`'s histogram
   is reusable: the same log2-bucket approach applied to a ring-3 round trip
   would give p50/p95/p99 in TSC cycles.

**None of these is committed to.** Objective 1 comes first for a concrete
reason: any latency figure taken before the host is characterised measures the
host, and this cycle has already spent a great deal of effort learning that the
hard way.

---

### 1h — driver timeouts, and four waits that had no bound at all

The brief asked for NVMe, AHCI, VirtIO and xHCI. **NVMe and AHCI do not exist in
this kernel** — zero references. What the audit found instead was worse than the
spin counts it went looking for:

```c
mw8(common, VCC_DEV_STATUS, 0);          /* reset */
while (mr8(common, VCC_DEV_STATUS) != 0) { }
```

**Four completely unbounded waits** — virtio-blk, virtio-net, virtio-gpu,
virtio-snd. A device that never clears its status register hangs the machine
there with no output, which is invariant 4 ("a lost wake must cost a failed
assertion, not the machine") and is strictly worse than a miscalibrated spin
count, because a spin count at least terminates.

| site | before | after |
|---|---|---|
| virtio blk/net/gpu/snd reset | **unbounded** `while` | `HW_WAIT(..., 100000)` + diagnostic |
| xHCI halt / reset-bit / CNR / run | 1,000,000 iterations | 100 ms / 100 ms / 500 ms / 100 ms |
| IOMMU enable ×4, invalidate ×2 | 1,000,000 iterations | 100 ms (done in §1e) |

xHCI bounds come from the specification — 16 ms for reset, 500 ms for Controller
Not Ready — rounded up, rather than invented. Every failure path now reports the
microseconds it waited instead of a loop count that means nothing on another
host. **No deadline fired in verification**: every handshake completed inside its
bound, so these are guards rather than workarounds.

`ps2_wait_in`/`ps2_wait_out` remain unconverted: they run before ACPI is parsed,
so `HW_WAIT` would take its iteration-cap fallback and nothing would improve.

### 1i — `ktime_get_ns()`: the TSC clocksource, re-anchored

**Why re-anchoring is the design and not a refinement.** A TSC clock does avoid
the PIT's catch-up bursts — `rdtsc` is read directly and nothing can bunch it
up. But §1d measured this TSC at 3.966–4.252 GHz *within one boot*, a 7.2%
spread, so a clock scaled by one boot-time frequency would simply trade the
PIT's burst error for a frequency error of the same order.

So the clock keeps an anchor pair and the PIT tick refreshes it against the PM
timer about once a second:

```
ns = anchor_ns + (rdtsc() - anchor_tsc) * 1e6 / khz
```

The TSC supplies resolution between anchors, which the PM timer cannot (its
reads are port I/O). The PM timer supplies accuracy at each anchor, which the
TSC cannot. Drift is bounded by one anchor interval instead of accumulating.

**Monotonicity across an anchor** is the one thing that can break, and it is
handled rather than hoped for: a new anchor may never publish a value below what
the old anchor would have reported for the same instant, so the update takes
whichever of TSC-elapsed and PM-elapsed is larger. The clock runs marginally
fast rather than ever stepping backwards.

Measured at `-smp 4`, three runs, against 50 ms intervals timed on the PM timer:

| | run 1 | run 2 | run 3 |
|---|---|---|---|
| `ktime_get_us` error | +8,144 us (**16.3%**) | +2,221 us (4.4%) | +1,118 us (2.2%) |
| `ktime_get_ns` error | **+16 us (0.03%)** | **+29 us (0.06%)** | **+29 us (0.06%)** |
| backward steps / 200,000 reads | 0 / 0 | 0 / 0 | 0 / 0 |

**Roughly 100x to 500x more accurate, with monotonicity intact** — 1.2 million
reads across the two clocks and six runs, zero regressions. 716–720 re-anchors
had been performed by the time of measurement, so the mechanism is demonstrably
live rather than dormant. Calibrated TSC: 3,813,101 kHz.

`ktime_get_us` is kept as the coarse clock and its limits are documented at the
definition; `ktime_get_ns` is the one to use when the number matters.

### 1j — verification for the LAPIC and timeout work

| tier | result |
|---|---|
| `smp4-bios` | 45 suites, 583 passed, 0 failed, 0 ranks |
| `smp4-iommu` | 47 suites, 597 passed, 0 failed, 0 ranks |
| `mcq` / `mcpre` / `threadstrs` | 6/0, 5/0, 14/0 |

The dispatch suites matter specifically here: they run with APs preempting 4.8x
more often than before §1f, which is real added scheduler pressure rather than a
cosmetic change, and with every driver handshake now time-bounded.

### 1k — sub-millisecond sleep, and POSIX clocks in ring 3

**`ksleep_us` is a yielding poll, not a block, and that is structural.** Ring-3
parking exists (`block_ring3_locked`) and is real, but `wait_deadline` is
expressed in `g_ticks` and `futex_timeout_scan` runs off the 100 Hz tick, so a
parked thread cannot wake sooner than **10 ms** whatever deadline it asks for.
Sleeping 100 us by parking would sleep somewhere between 0 and 10 ms. A true
sub-millisecond block needs a tickless LAPIC one-shot per deadline — a scheduler
change that would reprogram the timer §1f just calibrated to a uniform 100 Hz,
and not something to fold into a sleep primitive.

So: `udelay` below 100 us, and above it a poll that yields through `krelax()`, so
the caller occupies a run-queue slot rather than a core.

**THE FIRST VERSION RETURNED SHORT, WHICH IS THE ONE OUTCOME A SLEEP MUST NOT
PRODUCE.** It derived its deadline from `ktime_get_ns()`, and one run gave:

```
ksleep_us(10000): actually 9280 us (SHORT by 720 us)
ktime_get_ns: advanced 54369 us over a 50001 us delay   <- 8.7% fast
```

Both from the same run, and the second explains the first. `ktime_get_ns`
guarantees monotonicity by taking `max(pm_elapsed, tsc_elapsed)` at every
re-anchor — and that guarantee is precisely what lets it **ratchet ahead of real
time and never settle back**. A deadline computed from a clock running fast
expires early.

The fix is not to weaken the monotonicity guarantee, which callers depend on: it
is to stop using that clock for deadlines. `ksleep_us` now polls the PM timer
directly, whose rate is fixed by specification and cannot run fast.
**`ktime_get_ns` is the right clock for timestamping and ordering; it is not the
right clock for a deadline**, and that is now stated at both definitions.

After the fix, 15 measurements across five rounds at `-smp 4`:

| request | measured range | short sleeps |
|---|---|---|
| 100 us | 158–338 us | **0** |
| 1 ms | 1021–1150 us | **0** |
| 10 ms | 10008–10117 us | **0** |

Overshoot is 8–238 us and is the poll's own granularity plus whatever the
scheduler was doing; it never returns early. The 10 ms case lands within 0.1% on
its best runs.

**The ring-3 probe passed both runs even while the defect was live** — its sleeps
overshot by 54–111 us because the syscall round trip masked the drift. Only the
kernel-side test, timed against the PM timer rather than against the clock under
test, exposed it. That is the argument for measuring against an independent
reference, in one line.

#### The syscalls

`SYS_CLOCK_GETTIME` (104), `SYS_CLOCK_GETRES` (105), `SYS_NANOSLEEP` (106).
`timespec` is two 64-bit words `{tv_sec, tv_nsec}` — occ's `int` is a machine
word and `SYS_STAT`/`SYS_PIPE` set that precedent; a 32-bit `tv_nsec` would pack
wrong between a C kernel and an occ-compiled reader.

**`CLOCK_REALTIME` is boot-relative, and says so rather than inventing an
epoch.** There is no RTC on this machine — the kernel states that where `g_ticks`
is defined — and no time protocol, so nothing here knows what year it is.
REALTIME returns the monotonic value; `g_realtime_off_ns` exists as a named zero
so a future RTC or set-time call changes exactly one place.

**`clock_getres` reports what the clock can resolve**, derived from `g_tsc_khz`
rather than hardcoded to 1 ns. At the measured ~3.8 GHz that rounds to 1 ns, but
on a slower TSC it would honestly report more.

`SYS_NANOSLEEP` caps at 60 s: a ring-3 process should not pin a run-queue slot
for an unbounded time on a value it may have computed wrongly, and every suite
here finishes far inside that. `rem` is zeroed rather than ignored — this kernel
delivers no signals to a sleeping thread so a remainder cannot arise today, and
zeroing keeps a caller that checks it correct if interruption is ever added.

#### Ring 3, and role 63

`oclock_ns` / `omono_us` / `onanosleep_us` give ring 3 microseconds for the first
time; every ring-3 budget in this tree was previously `osysticks()` at 10 ms
granularity, subject to the catch-up bursts of §1g.

Role 63 checks the two properties that only exist across the syscall boundary —
monotonicity, where a context switch, a migration and a re-anchor can all fall
between two reads, and sleep fidelity at the three scales. It treats a **short**
sleep as the failure and an overshoot as tolerable, and it separately checks the
clock *advances*, because a clock stuck at one value never goes backwards either
and would otherwise pass. Five runs, all `OK (exit 1870)`, zero backward steps in
200,000 ring-3 reads per run.

## STANDING DEBT, carried forward

- **v0.91's Objective 1b is unfinished** — 10 soak iterations passed, 100 was
  never run, gated on the host question above.
- **`gate-dirty` / `gate-dirty-smp` have not been run since v0.90.0.** Neither
  v0.90.1 nor v0.91 re-ran them. The reasoning each time was that the changes
  were narrow; that is an argument, not a measurement.
- **The `append-oversub` deadline was never re-examined.** v0.90.1 fixed the
  assertion that misread a worker expiry; nobody has asked whether one worker in
  sixteen expiring at `-smp 8` means the budget is too tight for that width.
- **`capdma`'s used-ring signal is unexplained.** The NIC does not retire a TX
  descriptor at that point in the boot even after a successful reset and while
  unconfined. The fault-based assertion is sound and deterministic without it,
  so this is not blocking — but it is a device-state question nobody has
  answered, and it is written down so it is not rediscovered.

## NOT IN THIS CYCLE

No new ring-3 roles and no new lock modes unless a measurement above asks for
one. If an objective turns up a defect, fixing it is in scope; going looking for
features is not.
