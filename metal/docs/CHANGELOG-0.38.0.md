# Outrun OS v0.38.0-metal — Distributed scheduling groundwork: a ring-3 thread runs on an application processor

Begins dismantling the "BSP-only scheduler" boundary that has held since v0.17.
An application processor now boots with its own full machine state and
**autonomously executes a first-class ring-3 application thread** — dropping to
ring 3 on its own TSS and SYSCALL path, resolving the thread's identity through
the capability gate, and returning cleanly. Delivered in two verified stages.

## Stage 1 — per-CPU machine state (`struct cpu_local`)

Every core is now backed by a per-CPU block reached through its `GS_BASE`.
Because ring 3 cannot change `GS_BASE` (no `wrgsbase`; CR4.FSGSBASE off), the
base stays valid in **both rings**, so the syscall entry can reach per-CPU
state GS-relative with **no `swapgs`** — the key simplification.

- `struct cpu_local` (offset 0 = `idx`, preserving the `cpu_idx()` contract):
  `syscall_rsp`, `user_rsp` (fixed offsets the asm can use), `cur`, `cur_proc`,
  `tss`, plus the v0.35/36 online/ipi/work fields.
- **Per-CPU TSS + trap stacks.** The GDT keeps its 6 base descriptors and now
  carries one TSS descriptor **per CPU**; `g_tss[MAX_CPUS]`,
  `g_int_stack[MAX_CPUS]`, `g_syscall_stack[MAX_CPUS]`. A shared
  `cpu_tss_setup(idx)` installs a CPU's descriptor and `ltr`s it; `cpu_syscall_arm()`
  arms EFER.SCE/STAR/LSTAR/SFMASK per core.
- **APs finally load a task register.** Each AP runs `cpu_tss_setup(idx)` +
  `cpu_syscall_arm()`, so it can take a CPL3→CPL0 trap onto its own `rsp0` and
  execute SYSCALLs on its own kernel stack. Boot log: `cpu1 online: own TSS
  (sel 0x40)+SYSCALL`, `cpu2 … 0x50`, `cpu3 … 0x60`.
- The BSP scheduler path is otherwise untouched — which is why **all 15
  existing suites pass unchanged**, the regression gate proving the foundation
  is inert.

## Stage 2 — an AP runs a ring-3 thread (`mcsched`, new suite 2/2)

The BSP loads a ring-3 program into a process and hands it to cpu 1 via that
AP's per-CPU run-mailbox plus a wake IPI, then **quiesces** (spin-waits). The
AP adopts the thread (`ap_run_uthread`): sets the thread's identity, points its
own `TSS.rsp0`/SYSCALL stack at its per-CPU stacks, switches CR3, and drops to
ring 3. A role-6 probe reads its identity through the capability path
(`SYS_GETPID`) and exits with `code == pid`; `SYS_EXIT` on a non-BSP core is
gated to return through `resume_kernel` to the AP's C loop instead of the BSP
reaper. Boot-verified:

```
[mcsched] cpu1 adopting ring-3 pid 16 (entry 500000000000) on its own TSS/SYSCALL
  [mc :r3] ring-3 thread executing on an APPLICATION PROCESSOR
  [mc :r3] SYS_GETPID via the AP's own SYSCALL path -> pid 10
[mcsched]  PASS  an application processor executed the ring-3 thread and it returned
[mcsched]  PASS  the AP's capability gate resolved the thread's identity (exit code == pid)
[mcsched] cpu1 ran pid 16 -> exit code 16 (want 16)
```

On a uniprocessor boot the suite degrades cleanly — the thread runs on the BSP
via the same path (also verified, exit code 16 == pid).

**Serialized by design (honest scope).** This first demonstration quiesces the
BSP while the AP runs, so the still-global syscall entry stack (`g_ksrsp`),
user-RSP scratch, resume context, and `current_proc_idx` are exercised by
exactly one core and stay race-free. That is a real limit, stated plainly:
**concurrent** per-CPU scheduling (BSP + APs in ring 3 simultaneously) needs the
syscall entry converted to the GS-relative per-CPU path and `current_proc_idx`
made per-CPU — that is v0.39. The `cpu_local` fields, GS offsets, and per-CPU
TSS/stacks landed here are the scaffolding for exactly that.

## Verification

| Suite | Result |
|---|---|
| validate | 19/19 |
| invariants | 21/21 |
| stress | 10/10 |
| fuzz | 8/8 |
| smp | 6/6 |
| parallel | 5/5 |
| audit | 7/7 |
| **mcsched (new)** | **2/2** |
| threads | 7/7 |
| flip | 8/8 |
| cursor | 6/6 |
| kinetic | 12/12 |
| keys | 5/5 |
| iommu | 2/2 (q35 + intel-iommu) |
| capdma | 11/11 (q35 + intel-iommu) |

Boot configurations, all zero-failure:
- **`-smp 4` BIOS ISO** — 16 suites, cpu1 executed the ring-3 thread, reaches shell.
- **`-smp 4` q35 + intel-iommu** — all suites incl. iommu 2/2, capdma 11/11
  (AP ring-3 execution alongside 4 cores + VT-d); screendump captured with the
  full compositor intact (830 hit-marker px, liveness bar, five "HI R3" glyphs).
- **Uniprocessor (no `-smp`) regression** — mcsched single-cpu degrade path;
  the whole 15-suite stack unchanged, proving the per-CPU foundation is inert
  on one core.

NVMe/UEFI (`-smp 2`) remains unexercised for the OVMF boot-order quirk
documented since v0.35 (upstream of the kernel). Reported, not claimed.

## Honest scope gaps

- **Serialized AP execution only** (above) — no BSP+AP concurrent ring 3 yet;
  that is v0.39 and needs the GS-relative syscall path + per-CPU
  `current_proc_idx`. The `set_syscall_stack` global is restored to the BSP's
  stack after the AP excursion as a stopgap; the per-CPU path retires it.
- **No per-CPU run queues / load balancing / migration / IPI preemptive
  rescheduling** — the AP runs exactly one handed-off thread to completion.
  The `resched` field and the wake-IPI plumbing are in place for the real
  scheduler.
- The scheduler proper (`g_threads`, `g_cur`, `sched_switch_to`) is still
  single, BSP-owned; `current_proc_idx` is still a swapped global. Converting
  these is the load-bearing v0.39 work and its own invariant re-verification.
- Prior gaps unchanged: fling sampling per-frame; page tables leak on exit;
  VT-x unavailable under TCG; IOMMU/VT-d fully real.
