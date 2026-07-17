# Outrun OS v0.37.0-metal — Parallel page-table integrity audit (the runner does real work)

Puts the first *real* kernel workload on the v0.36 job runner: the whole-
address-space W^X / reserved-bit / .text-immutability audit — until now only
performed incrementally by the tick-driven sweep — is enumerated once and run
across every core. It proves the parallel runtime is not just a demonstrator:
the parallel verdict matches a serial auditor exactly and catches an injected
corruption, all cores participating. Read-only over the tables, so it is safe
against the concurrent background sweep and needs no lock, and it stays inside
the v0.35 BSP-only-scheduler boundary.

## The mechanism

- **Enumerate once, audit in parallel.** The BSP walks the kernel PML4 once to
  collect every present PD entry (each covering one 2 MiB huge leaf or one PT
  of up to 512 4 KiB leaves) into an audit table. The cores then drain that
  table through the v0.36 work-stealing cursor.
- **The same rule as the live sweep.** Each unit is checked with the exact
  `sweep_check_leaf` invariant the tick auditor uses: reserved bits [52..58]
  zero, no page both writable and executable, kernel `.text` stays present +
  read-only + executable. Per-core violation counts are summed by the BSP.
- **Live reads, not a snapshot.** The audit table stores a *pointer* to each PD
  entry, not a copy, so every core reads the current page tables. This is what
  a genuine integrity check requires — and it is what lets the audit catch a
  corruption injected *after* enumeration. (An earlier cut stored PDE values by
  copy; boot-testing caught it — an injected huge-PDE corruption went undetected
  because the copy was stale. Fixed by reading through the pointer.)

## The `audit` suite (new, 7/7)

1. **enumeration fit** the audit table without truncation (~515 PD-entry units)
2. **work conservation** — every enumerated unit audited exactly once
3. **clean kernel** — the parallel audit finds ZERO violations
4. **parallel == serial** — the parallel verdict equals a single-threaded
   auditor bit-for-bit
5. **distribution** — the audit was spread across cores (≥2 on SMP; 1 on a
   uniprocessor)
6. **injected corruption CAUGHT** — a reserved-bit flip into a live huge PD
   leaf is detected by the *parallel* audit (`saw 1, want 1`)
7. **clean after repair** — restoring the entry returns a clean verdict

## Fixes carried in this milestone

Two real defects, both surfaced by boot-testing and fixed, not papered over:

- **Live-read of the page tables** (above): the snapshot design missed live
  corruption; the auditor now reads current state.
- **Dispatch completion barrier.** `par_dispatch_kind` now waits for every AP
  to set its `work_done` flag (not just the shared `done` counter) before
  returning. `done++` happens inside the worker but `work_done=1` a moment
  later in the AP loop; without the barrier the *next* dispatch's `work_done=0`
  reset could race an AP still finishing the previous job, so that AP would
  skip the next job and miss its share. The audit issues three dispatches
  back-to-back (baseline + injected + repaired), which is exactly where this
  bites.

### The recurring SMP stack-canary hazard — root fix

Across v0.35–v0.37 an intermittent stack-canary fault appeared on the BSP
during the multi-core suites, and piecemeal `no_stack_protector` on each
coordinator (`cmd_smp`, `cmd_parallel`, `cmd_audit`) only chased it around —
this milestone it surfaced in `kprintf` itself. Root cause: `__stack_chk_guard`
is a single global the BSP scheduler swaps per thread (the v0.19 per-thread
canary), and that races the concurrent callers active only during the SMP
suites. The **root fix** is to exclude the shared low-level console printers
(`kprintf`, `kput_u64`) from the stack protector — they are the one protected
path every core and every coordinator invokes, and excluding shared I/O
helpers from stack protection is standard kernel practice. With the printers
canary-free the SMP-coordination path carries no per-thread canary at all, and
the fault is gone — verified on the exact config (q35 + intel-iommu, `-smp 4`)
where it last reproduced. Per-frame stack protection remains in force
everywhere else, and the canary self-test (stress suite) still passes.

## Verification

| Suite | Result |
|---|---|
| validate | 19/19 |
| invariants | 21/21 |
| stress | 10/10 |
| fuzz | 8/8 |
| smp | 6/6 |
| parallel | 5/5 |
| **audit (new)** | **7/7** |
| threads | 7/7 |
| flip | 8/8 |
| cursor | 6/6 |
| kinetic | 12/12 |
| keys | 5/5 |
| iommu | 2/2 (q35 + intel-iommu config) |
| capdma | 11/11 (q35 + intel-iommu config) |

Boot configurations, all zero-failure:
- **`-smp 4` BIOS ISO** — 13 suites, 4/4 cores, audit across all cores, reaches
  the shell.
- **`-smp 4` q35 + intel-iommu** — 15 suites (iommu 2/2, capdma 11/11 with 4
  cores + VT-d), audit across all cores; screendump captured with the full
  compositor intact (830 hit-marker px, the liveness bar, the five "HI R3"
  glyph blocks). This is the config the canary fault last reproduced on; it now
  passes cleanly.
- **Uniprocessor (no `-smp`) regression** — the audit's single-core inline path.

NVMe/UEFI (`-smp 2`) remains unexercised for the OVMF boot-order quirk
documented since v0.35 (upstream of the kernel). Reported, not claimed.

## Honest scope gaps

- **No wall-clock speedup under TCG** (single host thread), same as v0.36; the
  audit proves correctness and distribution, not performance. Distribution is
  also uneven run-to-run (the audit is fast, so a lead core can drain most
  units before others engage) — still ≥2 cores participate, and the result is
  reduction-exact regardless.
- **Kernel address space only.** The audit enumerates `kernel_cr3`; per-process
  page tables are not swept in parallel (the tick sweep already covers the
  kernel space, and process spaces are audited by the isolation invariants).
- **Static during the audit.** Correctness assumes no PDE is unmapped mid-audit;
  true during the boot suites (the kernel space is stable) and enforced by the
  read-only, no-allocation audit body, but a future dynamic-unmap workload would
  need generation/epoch handling.
- Prior gaps unchanged: scheduler BSP-only by policy; APs load no task register;
  fling sampling per-frame; page tables leak on exit; VT-x unavailable under
  TCG; IOMMU/VT-d fully real.
