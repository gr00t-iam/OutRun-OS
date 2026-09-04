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
