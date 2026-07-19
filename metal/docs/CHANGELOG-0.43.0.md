# Outrun OS v0.43.0-metal — SMP scheduler observability + a mixed-workload stress harness

Every structural piece of "SMP bring-up & per-CPU scheduling" this milestone
asked for — AP real-mode→long-mode bring-up (`boot/apboot.asm`), per-CPU
`struct cpu_local`, per-CPU run queues with work stealing, and LAPIC-timer-
driven per-core preemption — was already load-bearing, verified
infrastructure from v0.35 through v0.40. v0.43 does not re-implement any of
that. It adds the two pieces that were genuinely missing: a scheduler-
transition debug flag in the same family as v0.42's `DEBUG_SYSCALL_EXIT`/
`DEBUG_PAGEFAULT`, and a stress harness that exercises the existing SMP
scheduler under a workload that mixes all three shapes this kernel already
runs separately (bare syscalls, VFS, compositor/surface work) instead of one
at a time.

## What already existed (audited, not touched)

- AP bring-up: `ap_main` in `kernel64.c`, driven by the real-mode trampoline
  in `boot/apboot.asm` — per-CPU GDT/TSS/GS-base, LAPIC arm, then the
  autonomous executor loop.
- `struct cpu_local` (idx, apic_id, syscall_rsp, kernel-resume context,
  stack-canary slot, cur_proc, TSS pointer, run queue, per-core stats) —
  v0.38/v0.39.
- Per-CPU run queues (`rq_push`/`rq_push_front`/`rq_pop`/`rq_steal`) and the
  shared executor (`cpu_exec_proc`) — v0.39.
- Per-AP LAPIC periodic timer (vector 51) driving round-robin preemption,
  gated by `g_slice_on` — v0.40.

## What's new

- **`DEBUG_SMP_SCHED`** (`g_debug_smp_sched`, default off): logs `pick+switch`
  inside `cpu_exec_proc` — cpu id, pid, and the CR3 it just wrote, right after
  the write and before the ring-3 entry — and an edge-triggered `idle` log in
  `ap_main`'s executor loop (fires once per idle *transition*, latched by a
  new `dbg_was_idle` field appended to `cpu_local`, not once per `hlt` — a
  naive per-iteration log would flood the console into uselessness).
  Verified live: armed the flag, confirmed a captured/resumed context (pid 21)
  logged with the **identical CR3 on cpu1 then cpu2** — the concrete proof
  this milestone's Task 5 asked for, that address-space identity survives a
  cross-core migration — then reverted to the shipped default and re-ran the
  full suite (0 regressions).

- **`cmd_smp_stress`** (new suite, `smpstress` command): ten workers —
  4× role 6 (bare `SYS_GETPID`/`EXIT`), 4× role 9 (`cio_file_worker`: VFS
  open/write/read/close plus a racing shared file), 2× role 10
  (`cio_surface_churn`: paint + `SYS_SURFACE_FLIP`) — biased round-robin
  across every online core via `rq_push`, joined with a watchdog. Exactly two
  role-10 workers, matching `cmd_cio`'s own concurrency for that role
  precisely: `cio_surface_churn` picks its surface slot by `pid & 1` (only
  slots 6/7 exist), so a third concurrent instance would race two workers
  onto the same slot — a hazard of role 10 itself, not something this harness
  should paper over by "trying anyway."

  ```
  [smpstrs] 10 workers (2 surface + 4 VFS + 4 syscall) biased round-robin across 4 cores
  [smpstrs]   pid 32 role 10: exit 32 (want 32) ran_on 1
  ...
  [smpstrs]   pid 41 role 6:  exit 41 (want 41) ran_on 8
  [smpstrs]  PASS  no watchdog timeout — every worker reached a terminal state (no deadlock)
  [smpstrs]  PASS  every worker's exit code == its pid (no 6xx/7xx: VFS/surface work was correct)
  [smpstrs]  PASS  >= 2 distinct cores executed part of the mix (bias + stealing both worked)
  [smpstrs]  PASS  ZERO cross-core lock-rank violations across the whole mixed run
  ```

## Two real bugs found and fixed while building the harness (not pre-existing)

- **Missing capability grants.** First draft spawned every worker with
  `caps=0`; role 9/10 syscalls (`SYS_OPEN`, `SYS_SURFACE`) correctly denied
  them (exit 700/600). Fixed by granting `PCAP_FILESYSTEM`/`PCAP_FRAMEBUFFER`
  per role — exactly what `cmd_cio` already does for the same roles.
- **Missing VFS pre-seed.** `SYS_OPEN` resolves an existing dirent, it does
  not create one — `cmd_cio` pre-writes each worker's file kernel-side before
  dispatch, a step this harness's first draft skipped, so every role-9
  worker's own-file `SYS_OPEN` failed (exit 700). Fixed by replicating that
  exact pre-seed step with the same `cio_name`/`cio_byte`/`vfs_write_file`
  helpers.
- **`VFS_MAXFILES` (16) exhausted.** With `cmd_cio`'s own files still
  resident, this harness's four new per-worker files pushed the flat
  directory table (`g_dir[VFS_MAXFILES*256]`) past capacity — three
  `vfs_write_file` pre-seed calls silently failed (`"directory full"`),
  reproducing as the SAME 700 exit even after the fix above. `DENTS` is a
  cast over `g_dir`, no other structure hardcodes 16, so bumping to 24 is a
  self-contained, low-risk fix with headroom for what runs after it.

## Verification

| Config | Result |
|---|---|
| `-smp 4` BIOS ISO | 0 FAIL, 19 suites incl. `smpstress` 4/4 |
| `-smp 4` q35 + intel-iommu | 0 FAIL, all green incl. iommu 2/2, capdma 11/11, `smpstress` 4/4 |
| uniprocessor | 0 FAIL, `smpstress` 3/3 (multi-core distribution check SKIPped, honestly) |

Compositor screendump captured at the shell on the IOMMU run (attached).

NVMe/UEFI remains unexercised for the OVMF boot-order quirk documented since
v0.35 (upstream of the kernel). Reported, not claimed.

## Honest scope gaps

- **The pre-existing, intermittent `cio` descriptor-leak flake** (first noted
  in v0.42's changelog) reproduced twice more across this milestone's ~6 boot
  attempts — same frequency as before, unrelated to anything changed here
  (file-descriptor bookkeeping across concurrent VFS workers, not scheduling
  or CR3 discipline). Still open, still out of scope for this milestone.
- **No true CPU affinity/pinning** — `cmd_smp_stress`'s placement is an
  *initial bias* via `rq_push(cpu, ...)`; idle siblings can and do steal work
  from it (by design — that's the load-balancing this kernel already has).
  A hard-pin primitive that disables stealing for a specific task does not
  exist and was not added.
- **`DEBUG_SMP_SCHED`'s "switch" event is folded into "pick"** — one log line
  covers "this core picked task X" and "this core's CR3 is now X's" together,
  since in this executor model they are the same instant (`cpu_exec_proc`
  writes CR3 immediately after adopting the task). There's no separate
  "switch without a fresh pick" case to log, because this scheduler has no
  such transition.
- Prior gaps unchanged: kproc table never shrinks (v0.42); VT-x unavailable
  under TCG; IOMMU/VT-d fully real.
