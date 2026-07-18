# Outrun OS v0.40.0-metal — AP-local time-slicing: every application processor preemptively multitasks on its own clock

v0.39 gave every core the machinery to capture, requeue, migrate and resume a
ring-3 context — but a preemption still had to be *commanded* by another core.
v0.40 turns that machinery inward: **each AP arms its own LAPIC timer**, and
when the slice gate is open, the tick itself preempts — capture the running
thread mid-loop, requeue it at the tail, dispatch the next. One core,
several ring-3 threads, genuine involuntary round-robin multitasking.

## What landed

- **Vector 51 — the per-AP slice tick.** Every AP arms its LAPIC LVT timer
  periodic (divider 16, ~tens of ms) during bring-up. The tick shares the
  vector-50 capture path (`smp_preempt_ipi`) but counts separately
  (`slice_count` vs `preempt_count`), so the `mcpre` cross-core IPI suite
  still verifies exactly what it verified in v0.39.
- **Gated, like everything else.** `g_slice_on` follows the same "preemption
  on demand" discipline as the BSP's PIT gating (v0.19): ticks are ignored
  while the gate is closed, so every pre-existing suite runs under identical
  scheduling semantics — the regression gate stays meaningful.
- **The dispatch log.** `cpu_exec_proc` appends every task pick-up (fresh or
  resumed) to a global log; `kproc.dispatches` counts per task. The slice
  suite reads the interleaving back LITERALLY instead of trusting counters.

## Verified (`slice`, new suite, 4/4)

Three long-running ring-3 checksum threads piled on cpu1's queue, gate open:

```
[slice  ]   cpu1's timer sliced 3 context(s) out mid-loop
[slice  ]   cpu2's timer sliced 2 context(s) out mid-loop
[slice  ]   cpu3's timer sliced 2 context(s) out mid-loop
[slice  ] dispatch order (all cores): 23 24 25 23 24 25 23 24 25 23
[slice  ]   pid 23: exit 23 (want 23)  dispatched 4 time(s)  ran_on 6
[slice  ]   pid 24: exit 24 (want 24)  dispatched 3 time(s)  ran_on 8
[slice  ]   pid 25: exit 25 (want 25)  dispatched 3 time(s)  ran_on 2
```

- **Round-robin, literally**: `23 24 25 23 24 25 23 24 25 23` — 7 timer
  slices + 3 first dispatches = 10 log entries, every thread set down and
  picked back up at least twice.
- **Slicing and migration COMPOSE**: a sliced-out context is an ordinary
  queue entry, so idle APs — woken by their own gated ticks — steal them:
  the three threads finished on three different cores (`ran_on` 6, 8, 2)
  after being captured mid-loop up to 4 times each, **checksums intact**
  (every exit code == pid, with the in-loop `SYS_GETPID` identity fuzz
  passing throughout).
- The suite intentionally does not pin work: what it demands is completion
  integrity, ≥2 slices, ≥2 dispatches per thread, and ≥4 literal
  alternations in the dispatch log — all observed.

## Verification

| Config | Result |
|---|---|
| `-smp 4` BIOS ISO | 132 PASS / 0 FAIL (17 suites incl. slice 4/4) |
| `-smp 4` q35 + intel-iommu | all green: iommu 2/2, capdma 11/11, mcq 6/6 (high-water 4/4), mcpre 5/5, slice 4/4 |
| uniprocessor | honest degrade: probes run sequentially on the BSP, slicing checks SKIPped, 0 FAIL |

Screendump captured on the IOMMU run at the shell: compositor fully live —
two dumps 2 s apart differ by 228k pixels (the canvas is panning), and the
frame at camera home shows the complete 830-pixel hit-marker overlay. The
first dump caught the camera mid-pan away from the overlay; stated because
a single static screenshot can under-represent an animating scene.

NVMe/UEFI remains unexercised for the OVMF boot-order quirk documented since
v0.35 (upstream of the kernel). Reported, not claimed.

## Honest scope gaps

- The BSP does not self-slice ring 3 with the LAPIC timer — its tick source
  remains the PIT and its kernel-thread scheduler is unchanged. AP-side
  kernel *threads* still don't exist: what is sliced is the ring-3 task layer.
- A tick that lands mid-syscall defers (flags `resched`, ignored beyond
  waking the core); the thread keeps its slice until the next tick catches
  it at CPL3. No tick accounting/fairness beyond round-robin order.
- The slice quantum is a fixed initial-count constant, not calibrated
  against real time; under TCG the effective quantum varies.
- The wider syscall table (VFS/DMA/surfaces) is still exercised from the
  BSP only; cross-core reentrancy of those subsystems remains unaudited.
- Prior gaps unchanged: page tables leak on exit; VT-x unavailable under
  TCG; IOMMU/VT-d fully real.
