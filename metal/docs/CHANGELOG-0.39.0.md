# Outrun OS v0.39.0-metal — Concurrent distributed scheduling: every core schedules, executes, steals, preempts and migrates ring-3 threads

The milestone the last four versions built toward. The "BSP-only scheduler"
boundary held since v0.17 is **gone**: application processors now pull ring-3
tasks off their own per-CPU run queues autonomously, idle cores steal work from
overloaded siblings, **all four cores sit in ring 3 simultaneously**, and one
core can fire an IPI that forces another to capture its running ring-3 context
mid-loop — a context that is then **resumed on a different core** and completes
intact. Three verified stages.

## Stage 1 — the trap path loses its last cross-CPU globals

Everything the syscall/trap machinery touches is now reached GS-relative
through this core's `struct cpu_local` (valid in both rings; no `swapgs`
anywhere):

- **`syscall_entry` is per-CPU** (`%gs:CPUL_SYSCALL_RSP` / `%gs:CPUL_USER_RSP`):
  N cores can sit in the SYSCALL path at once, each on its own kernel stack.
  The `.bss` globals `g_ksrsp`/`g_ursp` are deleted.
- **The kernel resume context is per-CPU** (`%gs:24..72`): concurrent
  synchronous ring-3 excursions on different cores unwind independently
  (`enter_user_mode`/`resume_kernel` rewritten GS-relative).
- **`current_proc_idx` is per-CPU**: now a macro over
  `g_cpu[cpu_idx()].cur_proc`, so every capability check resolves the identity
  of the task *this core* is executing. The BSP scheduler still swaps its own
  slot per-thread, exactly as before.
- **The stack-protector guard is per-CPU** (`-mstack-protector-guard=tls`,
  `reg=gs`, `offset=80` — the Linux-kernel technique). This is the ROOT fix
  for the v0.35–v0.37 canary intermittents: the old single global was swapped
  by the BSP scheduler underneath concurrently-executing APs. The BSP's
  GS_BASE is armed as `kernel_main`'s first instruction so the guard is valid
  from boot; `usermode_init` no longer reloads fs/gs (a gs selector write
  clears IA32_GS_BASE).
- Layout is enforced by `_Static_assert(__builtin_offsetof(...))` against the
  asm/compiler contracts.
- Regression gate: the full existing suite stack, `-smp 4` and uniprocessor,
  zero failures, before any Stage-2 behavior changed.

## Stage 2 — per-CPU run queues, autonomous pull, work stealing (`mcq`, 6/6)

The v0.38 hand-off mailbox is deleted. Each `cpu_local` carries a small ring
of runnable kproc indices under a per-CPU spinlock (`rq_push`/`rq_push_front`/
`rq_pop`/`rq_steal`); each AP's main loop **drains its own queue and then robs
siblings**. One executor, `cpu_exec_proc`, is shared by every core including
the BSP.

The suite is deliberately adversarial: one probe per online core, but every
AP-bound task is piled onto **cpu1's queue alone**, then all APs are woken.
Boot-verified on `-smp 4`:

```
[mcq    ]   pid 17: exit 17 (want 17)  ran_on mask 1  finish#4
[mcq    ]   pid 18: exit 18 (want 18)  ran_on mask 2  finish#3
[mcq    ]   pid 19: exit 19 (want 19)  ran_on mask 4  finish#2
[mcq    ]   pid 20: exit 20 (want 20)  ran_on mask 8  finish#5
[mcq    ]   cpu2 ran 1 task(s), stole 1
[mcq    ]   cpu3 ran 1 task(s), stole 1
[mcq    ] ring-3 concurrency high-water mark: 4 core(s) at once
```

- **4 cores in ring 3 simultaneously** (`g_inr3` high-water mark, CAS-tracked).
- **Stealing observed, not asserted**: cpu2 and cpu3 each pulled a task out of
  cpu1's overloaded queue.
- **No identity bleed**: every probe hammers `SYS_GETPID` inside its compute
  loop (~45 checks each, concurrently on all cores) and exits 999 on any
  mismatch; every exit code came back `== pid`.

## Stage 3 — IPI preemption, priority hand-off, cross-core migration (`mcpre`, 5/5)

New IPI vector 50. If it lands while the target core is at CPL3, the handler
copies the **complete interrupted register state** out of the interrupt frame
into the task's `uctx`, EOIs, and unwinds the whole interrupt through the
core's per-CPU resume point with a sentinel — the executor requeues the
captured context (honouring a `migrate_to` directive) and pulls the next task,
which is exactly the high-priority thread pushed to the queue's front.
`enter_user_resume` rebuilds all 15 GPRs + RIP/RSP/RFLAGS and iretq's into the
middle of the interrupted user code — **on whichever core the context landed
on**. If the IPI catches the kernel (mid-syscall) it only flags `resched`; the
sender retries. Boot-verified on `-smp 4`:

```
[mcpre  ] pid 21 is mid-loop in ring 3 on cpu1; queueing pid 22 AT THE FRONT and firing IPI 50
[mcpre  ] cpu1 preempt_count +1; long: exit 21 ran_on 6 finish#7 | hi: exit 22 ran_on 2 finish#6
```

`ran_on 6` = cpu1 AND cpu2: the long thread **started on cpu1, was forced out
mid-loop, and finished on cpu2** with its checksum/getpid loop intact (exit ==
pid); the later-queued high-priority thread completed **first**.

## Verification

| Suite | -smp 4 BIOS | -smp 4 q35+IOMMU | uniprocessor |
|---|---|---|---|
| validate/invariants/stress/fuzz | 19/21/10/8 | same | same |
| smp / parallel / audit | 6/5/7 | same | same |
| mcsched | 2/2 | 2/2 | 2/2 (BSP degrade) |
| **mcq (new)** | **6/6** | **6/6** | 3/3 (+SKIPs, honest) |
| **mcpre (new)** | **5/5** | **5/5** | 1/1 (+SKIPs, honest) |
| threads/flip/cursor/kinetic/keys | 7/8/6/12/5 | same | same |
| iommu / capdma | — | 2/2 / 11/11 | — |

All three configurations: **zero failures** (124 PASS on BIOS `-smp 4`).
Concurrency high-water mark 4/4 cores on both SMP configs — including with
VT-d DMA remapping active. Screendump captured at the shell on the IOMMU run:
compositor intact (584 hit-marker px, liveness bar, window chrome). The
keys-suite glyph surface had already been reclaimed by capture time (two more
suites now run before the shell than in v0.38) — stated, not hidden.

NVMe/UEFI remains unexercised for the OVMF boot-order quirk documented since
v0.35 (upstream of the kernel). Reported, not claimed.

## Honest scope gaps

- **The BSP kernel-thread scheduler (`g_threads`/`g_cur`/`sched_switch_to`)
  is still BSP-only.** What is distributed is the ring-3 *task* layer (queues,
  stealing, preemption, migration). Kernel threads do not migrate; that would
  need per-CPU `g_cur`/idle threads and a rework of `sched_switch_to`.
- The preempt IPI defers (flags `resched`) when it catches the target
  mid-syscall; the sender retries. There is no in-kernel preemption point that
  acts on `resched` yet.
- Ring-3 tasks run on APs must not own surfaces (`surfaces_reclaim` stays a
  BSP-path concern); the mcq/mcpre probes hold no framebuffer capability, and
  the syscall surface exercised concurrently is WRITE/GETPID/EXIT — the wider
  syscall table (VFS, DMA, surfaces) has not been re-audited for cross-core
  reentrancy and is still exercised from the BSP only.
- `rq[8]` per CPU is a fixed ring; no overflow handling beyond push failure.
- Prior gaps unchanged: page tables leak on exit; VT-x unavailable under TCG;
  IOMMU/VT-d fully real.
