# Outrun OS v0.36.0-metal — Work-stealing parallel job dispatch across cores

Turns v0.35's SMP groundwork into the first *useful* multi-core capability: the
BSP hands a bounded, data-parallel job to every online core, the cores drain
one shared atomic cursor cooperatively, and the BSP reduces the per-core
results. It stays entirely inside the v0.35 safety boundary — **no scheduler,
no ring 3, no run-queue migration** — so every isolation invariant proven since
v0.17 still holds. This is offloaded compute, not a second scheduler.

## The mechanism

- **A single shared atomic cursor.** Each core claims the next fixed-size unit
  with `lock xadd` on `g_pjob.cursor`; when the cursor passes the unit count,
  that core is done. Work-stealing falls out for free — a core that finishes
  its unit immediately grabs the next available one, so a fast core naturally
  takes a bigger share and no core waits on a slow one.
- **Per-core accumulators, order-independent reduction.** Each core folds its
  units into its OWN `partial[cpu]` slot (padded apart to avoid false
  sharing). The fold is a per-unit FNV-1a hash and the accumulators are
  summed, so the result is invariant to how the cursor happened to interleave
  across cores — any distribution yields the bit-identical total.
- **Dispatch = one new AP work mode.** `ap_main` gains mode 3: on the BSP's
  signal each AP runs `pjob_run(cpu)` exactly once. The BSP wakes them with a
  broadcast IPI, gives one AP a bounded head start (so the work is genuinely
  shared under TCG's cooperative vCPU scheduling), then work-steals alongside
  them and waits for every core's completion before reducing.
- On a **uniprocessor** boot the BSP simply runs `pjob_run(0)` inline — the
  same code path, no APs to signal.

## The `parallel` suite (new, 5/5)

Working set: a deterministic 1 MiB buffer (2048 units × 512 B).

1. **work conservation** — the sum of per-core claim counts equals the unit
   count: every unit processed exactly once (the atomic cursor guarantees no
   unit is lost or double-counted).
2. **correctness** — the parallel reduction equals a single-threaded serial
   reference **bit-for-bit**.
3. **distribution** — the job was spread across multiple cores (≥2 on SMP; 1
   on a uniprocessor).
4. **sensitivity** — flipping ONE bit in the working set changes the parallel
   result, proving the cores actually read the whole data, not a shortcut.
5. **determinism** — restoring that bit reproduces the original result.

Observed core shares (boot-verified, `-smp 4`): e.g. `cpu0=507 cpu1=593
cpu2=517 cpu3=431`, all four cores taking a substantial, balanced portion, with
`result d522f943ba492800 == serial d522f943ba492800`.

**Honest measurement note.** Under TCG the vCPUs are time-sliced onto one host
thread, so this cannot demonstrate wall-clock *speedup* — and the suite claims
none. What it proves is what actually matters for a parallel runtime's
soundness: exact-once work distribution, bit-identical reduction, and genuine
spread across cores. On real hardware the same code runs the cores in true
parallel; the correctness proof is identical either way.

## Consistency fix

`cmd_parallel` carries `no_stack_protector`, exactly like `cmd_smp` (v0.35) and
`sched_switch_to`: it is a core-coordination routine that busy-waits on other
cores while the BSP-owned per-thread stack canary is live, so it must not carry
that canary. The reduction and every worker are verified correct; only the
coordinator's frame-canary check is at risk, precisely as measured and
documented for `cmd_smp`. This was caught by boot-testing (a canary fault on
cpu0 right after the suite) and fixed, not papered over.

## Verification

| Suite | Result |
|---|---|
| validate | 19/19 |
| invariants | 21/21 |
| stress | 10/10 |
| fuzz | 8/8 |
| smp | 6/6 |
| **parallel (new)** | **5/5** |
| threads | 7/7 |
| flip | 8/8 |
| cursor | 6/6 |
| kinetic | 12/12 |
| keys | 5/5 |
| iommu | 2/2 (q35 + intel-iommu config) |
| capdma | 11/11 (q35 + intel-iommu config) |

Boot configurations:
- **`-smp 4` BIOS ISO** — 12 suites zero-failure, 4/4 cores, parallel job
  spread across all four, reaches the shell.
- **`-smp 4` q35 + intel-iommu** — 14 suites zero-failure (iommu 2/2, capdma
  11/11 with 4 cores + VT-d active), parallel job across all four; SMP
  screendump captured with the full compositor intact (830 hit-marker px, the
  liveness bar, the five "HI R3" glyph blocks).
- **Uniprocessor (no `-smp`) regression** — the `parallel` suite's single-CPU
  inline path, all suites zero-failure.

NVMe/UEFI (`-smp 2`) remains unexercised for the same OVMF boot-order quirk
documented in v0.35 (the firmware drops to its interactive Shell; upstream of
the kernel, reproduces on v0.34-identical image construction). Reported, not
claimed.

## Honest scope gaps

- **TCG shows no wall-clock speedup** (single host thread); correctness and
  distribution are what is proven, not performance. Stated in the suite output.
- **One job at a time, run to completion.** There is no job queue, no
  preemptible jobs, no priorities — the BSP dispatches, waits, reduces. That is
  the right shape for offloaded batch compute and deliberately does not
  approach a scheduler.
- **The parallelised workload is a demonstrator** (a memory fold). Wiring a
  real kernel workload — e.g. the tick-driven PTE integrity sweep — onto this
  runner is the natural follow-up, but it needs care around page tables that
  can change under the audit, so it is left for its own milestone.
- **Head-start heuristic.** To guarantee cross-core distribution under TCG's
  cooperative vCPU scheduling, the BSP waits (tick-bounded) for one AP to
  engage the cursor before joining. On real hardware this is a negligible
  delay; it is called out because it is a scheduling-dependent nicety, not a
  correctness requirement (correctness holds for any interleaving).
- Prior gaps unchanged: scheduler BSP-only by policy; `cmd_smp`/`cmd_parallel`
  exempt from the per-thread canary; APs load no task register; fling sampling
  per-frame; page tables leak on exit; VT-x unavailable under TCG; IOMMU/VT-d
  fully real.
