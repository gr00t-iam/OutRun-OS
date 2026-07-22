# Outrun OS v0.49.0-metal — SMP Core Scaling & Cross-Core TLB Shootdowns

Ring-3 code can now ask the kernel to remap or unmap its own pages while
genuinely running across every core — `SYS_SMP_REMAP`/`SYS_SMP_UNMAP` back a
private scratch page with a fresh physical frame every call, and the frame
that falls out the other end is only ever handed back to the allocator
*after* every core that could hold a stale translation for it has
acknowledged a real cross-core TLB shootdown. Per-CPU run queues (v0.39)
gained CPU affinity enforcement and a non-blocking work-stealing fast path;
the kproc table and each process's own page-table mutations got their first
dedicated spinlocks. Building this milestone found two genuine, previously
unexercised concurrency bugs — both are below, not swept under "flake".

## Two real bugs found building this milestone

Both bugs share a root cause: through v0.48, `tlb_shootdown()` had exactly
one calling convention — a single-threaded BSP test orchestrator invoking it
between suites. This milestone is the first time the shootdown protocol
becomes reachable **concurrently, from ring 3, on every core at once**
(`SYS_SMP_REMAP`/`SYS_SMP_UNMAP`/`SYS_TLB_SHOOTDOWN`), and that broke two
assumptions nothing had ever tested before.

**1. The shootdown mailbox (`g_shoot_va`/`g_shoot_pages`/`g_shoot_mask`/
`g_shoot_ack`) had no lock.** It didn't need one when only the BSP orchestrator
ever touched it. With `cmd_smp_stress`'s new phase-2 migration workers
calling it from four cores simultaneously, two overlapping shootdowns
stomped each other's mailbox — a core waiting for its own acks would see the
count reset out from under it by someone else's request and burn its full
100-tick timeout for nothing. Found live as what looked like an outright
hang (every shootdown paying its full timeout, back to back, for tens of
seconds). Fixed with `g_shoot_lock`, a raw leaf spinlock (same discipline as
`g_frame_lock`) serializing one whole "publish the mailbox, IPI, wait for
acks" round trip.

**2. `SYSCALL` entry clears `IF` for the entire syscall (`SFMASK = 0x200`,
v0.38) — and nothing had ever needed interrupts back on mid-syscall before,
because ordinary klock contention is a plain cache-coherent spin that
doesn't care about `IF`.** The shootdown wait does care: `g_shoot_ack` only
advances when a *target* core's IPI handler actually runs, and that cannot
happen on a target that is itself inside its own syscall with `IF` still
clear. With the mailbox now serialized by `g_shoot_lock` (bug #1's fix),
core A can hold the lock waiting on core B's ack while core B is spinning to
*acquire* that same lock for its own shootdown — B can never take A's IPI to
ack it, and A's bounded wait just meant every such pair now paid a full
timeout serially instead of hanging outright, which is still a
several-dozen-second stall across eight concurrent workers, not a passing
grade. Fixed by forcing `IF` on for the full span of
`tlb_shootdown_range()` — lock acquire through the ack wait — and restoring
the caller's original `IF` before return, the same `pushfq`/restore pattern
`con_lock`/`con_unlock` already uses elsewhere in this kernel, just
inverted (force-enable across the section instead of force-disable).

Both are fixed in `tlb_shootdown_range()` itself, so every future caller —
not just this milestone's own workers — gets the fix automatically.

## What's new

- **`tlb_shootdown_range(va, pages, cpu_mask)`**: the shootdown protocol
  generalized from "one page, every core" to an arbitrary page count and an
  explicit target mask. `tlb_shootdown(va)` (the pre-v0.49 call shape, still
  used by the ~15 existing frame-reuse/AP-boot sites) is now a thin wrapper
  (`pages=1, mask=all`) — no existing caller changed behavior.
- **`SYS_TLB_SHOOTDOWN(vaddr, pages, target_cpus)`** (syscall 24,
  `PCAP_SMP_ADMIN`): the raw primitive exposed to ring 3, so the stress
  harness (and any future driver) can drive it directly rather than only
  indirectly through remap/unmap. `invlpg` has no memory side effect, so
  no `access_ok` check is needed on `vaddr` — a bogus address just
  invalidates nothing.
- **`SYS_SMP_REMAP(slot)`** (syscall 26) / **`SYS_SMP_UNMAP(slot)`**
  (syscall 27): each process gets `SMP_SLOTS` (2) private scratch pages at
  fixed vaddrs (`SMP_USER_V + slot*4K`). Remap allocates a fresh frame, maps
  it over the slot, and — if a frame was already there — synchronously
  shoots down and only then frees the old one, across exactly the cpus in
  `kprocs[p].ran_on` (every core that has ever loaded this address space's
  CR3, the precise set that could hold a stale entry for it). Unmap clears
  the leaf PTE (reusing the existing v0.46 `walk_pte`/`unmap_page` helpers)
  and does the same shootdown-then-free. Both are the literal implementation
  of requirement (1): frame recycling never runs ahead of the acknowledged
  shootdown.
- **`SYS_SET_AFFINITY(mask)`** (syscall 25): pins a task to a CPU subset.
  `kprocs[].affinity` (0 = unrestricted) is enforced in two places —
  `rq_steal` refuses to steal a task outside the thief's bit, leaving it
  queued for its rightful owner instead; `cpu_exec_proc`'s post-preemption
  migration-target selection overrides an explicit `migrate_to` directive
  that would land outside the mask, falling back to the lowest-indexed
  online cpu that qualifies (or the current core if none do). Affinity is
  authoritative over an explicit migration request, not just a hint.
- **Non-blocking work-stealing.** `rq_steal` now takes a single
  `__sync_lock_test_and_set` attempt on a victim's queue lock (`rq_trylock`)
  instead of the old blocking `rq_pop()`'s spin-until-acquired: a thief that
  finds a sibling's queue busy moves straight to the next candidate rather
  than ever waiting behind an owner. `g_rq_steal_aborted` counts these
  back-offs; the new stress phase reports both it and the steal count so the
  heuristic's effect is visible, not just asserted. (This is a non-blocking,
  trylock-based heuristic layered on the existing v0.39 spinlock-protected
  ring — not a from-scratch lock-free MPMC deque; see Honest scope gaps.)
- **`g_kproc_lock`**: the `kproc_spawn` slot scan-and-claim (first-fit over
  `used && torn_down`, or bump `n_kproc`) is now one atomic critical section
  — clearing `torn_down` (the recycle claim signal) *inside* the lock is
  what stops two concurrent spawns from matching the same slot. A raw leaf
  spinlock in the same discipline as `g_frame_lock`: never held across
  `create_address_space()` or any other allocation. `kproc_spawn` has been
  BSP-only-by-convention through every suite in this kernel (nothing issues
  a ring-3 `SYS_SPAWN`), so this closes a latent gap — see Honest scope gaps
  for what it does and doesn't prove.
- **Per-kproc `vma_lock`**: a leaf spinlock guarding the compound
  "map the new frame, update `smp_slot_phys[]`" update in
  `SYS_SMP_REMAP`/`SYS_SMP_UNMAP`, making that invariant an explicit
  critical section instead of an implicit one.
- **`cmd_smp_stress` phase 2**: `MIGRATE_N` (8) role-16 workers
  (`smp_migrate_worker`, `user/init.c`) hammer remap/unmap across every
  core — write a per-round pattern through the freshly remapped page,
  read it straight back (a stale TLB entry would read the *old* frame's
  content, not the new one), call `SYS_TLB_SHOOTDOWN` directly once per
  round as an independent liveness check of the raw primitive, then unmap.
  Half self-pin to cpu0 via `SYS_SET_AFFINITY`; the kernel harness mirrors
  that pin at initial dispatch (see Honest scope gaps for why the ring-3
  self-pin alone isn't sufficient) so the affinity check is exercised
  against real steal pressure, not just recorded. Assertions: no watchdog
  timeout (0 IPI deadlocks), every exit code == pid (0 stale TLB reads
  across the whole remap/unmap/migration run), every pinned worker's
  `ran_on` stays inside its mask, 0 rank violations, plus an explicit
  capability-denial smoke test for all four new syscalls.

## Honest scope gaps

- **`SYS_YIELD` is unsound when called from a `cpu_exec_proc`-dispatched
  task on the BSP.** It unconditionally drives the *legacy* BSP cooperative
  scheduler (`sched_yield()`/`g_threads`/`curthr`) — correct for the
  pre-v0.39 uthread path, but that scheduler has no idea a
  `cpu_exec_proc`-dispatched task (the v0.39+ distributed executor every
  role in this suite and `cmd_cio`/`cmd_smp_stress` actually use) even
  exists. Calling it from such a context switches away into unrelated BSP
  thread state and never returns. No role before v0.49 ever hit this — the
  only two call sites (roles 3/4) have been dead code, never spawned, since
  before v0.43. `smp_migrate_worker` steers around it (no `SYS_YIELD` in its
  round loop; the remap/shootdown round trip itself is enough of a
  scheduling point for this milestone's purposes) rather than fixing the
  underlying mismatch, which is a pre-existing, cross-cutting scheduler gap
  well outside "SMP core scaling and TLB shootdowns."
- **The affinity check needed the KERNEL side to mirror the pin, not just
  the worker's own `SYS_SET_AFFINITY` call.** A task's self-pin only
  constrains *future* scheduling decisions (steal, migration-target
  selection) — it has no mechanism to retroactively relocate a task that is
  already executing on a core outside the new mask (there is no
  "migrate me now" primitive). `cmd_smp_stress` phase 2 sets
  `kprocs[p].affinity` right after spawn, before the initial `rq_push`
  distribution, so the *first* placement already honors the pin the worker
  is about to request for itself. Getting this wrong initially (worker-only
  self-pin, round-robin initial placement) produced a real, reproducible
  test failure during development — recorded here because it's the kind of
  mistake a first read of `SYS_SET_AFFINITY`'s doc comment invites.
- **The non-blocking work-stealing fast path is a trylock heuristic on the
  existing v0.39 spinlock-protected ring, not a lock-free MPMC deque.**
  `rq_push`/`rq_pop` (the owning core) still use the blocking spinlock
  acquire; only `rq_steal` was changed to a single non-blocking attempt.
  A genuine lock-free ring (Vyukov-style bounded MPMC, since `rq_t` is
  pushed to by multiple producers — the owner AND any orchestrator pushing
  directly into a sibling's queue) would be a materially larger rewrite of
  a data structure three prior milestones' suites already depend on being
  correct; the risk of introducing an unfindable heisenbug in code this
  kernel has no way to test beyond boot-and-log outweighed the value of the
  word "lock-free" being literally true rather than "non-blocking, trylock
  layered on a proven spinlock ring."
- **`g_kproc_lock` and the per-kproc `vma_lock` are defense-in-depth, not
  fixes for an active bug.** `kproc_spawn` remains BSP-only by convention —
  every call site in every suite is a sequential BSP loop — so the lock's
  contention is provably zero today; it exists because per-CPU run queues
  now let ring-3 tasks execute genuinely concurrently, and a future
  ring-3-triggered spawn path would have raced without it. Likewise
  `vma_lock`: this kernel is strictly one-thread-per-kproc, so no two
  contexts can ever touch the same process's page tables concurrently by
  construction — the lock makes the remap/unmap compound update an
  explicit critical section for whenever that assumption stops holding,
  not because it doesn't hold today.
- **A methodology trap worth recording**: `build/vblk.img` (and the IOMMU
  config's `/tmp/cas.img`) are *persistent* disk images that accumulate CAS/
  VFS state across every QEMU invocation that reuses them, including ones
  killed mid-boot by a test-harness timeout. Comparing a "clean" run against
  a "modified" run on the *same* stale disk image produced a spurious,
  reproducible-looking `vfsstrs` failure (`VFS journal commit is genuinely
  DEFERRED`) during development that had nothing to do with this
  milestone's code — a fresh disk image made it disappear identically on
  both the pre- and post-v0.49 kernel. Every verification run below used a
  freshly created, zeroed disk image.
- VT-x unavailable under TCG; NVMe/UEFI install-image boot path verified
  this session (fresh disk, 0 FAIL) in addition to the three configs below;
  IOMMU/VT-d fully real. `MAX_KPROC`/`MAX_CPUS` caps unchanged (v0.45/v0.35).

## Verification

Every row below is a **freshly created, zeroed disk image** per config (see
the methodology note above for why that matters).

| Config | Result |
|---|---|
| `-smp 4` BIOS ISO | 0 FAIL, 24 suites incl. `smpstrs` 9/9 |
| `-smp 4` q35 + intel-iommu | 0 FAIL, 26 suites incl. `iommu` 2/2, `capdma` 11/11, `smpstrs` 9/9 |
| uniprocessor (no `-smp`) | 0 FAIL, 24 suites incl. `smpstrs` 8/8 (the multi-core-distribution sub-check correctly SKIPs with 1 online cpu; every other check, including the full remap/unmap/migration phase, runs and passes identically) |
| OVMF + NVMe install image | 0 FAIL (spot-checked to the shell prompt) |

`smpstrs` (`cmd_smp_stress`) is the suite this milestone extended: phase 1
(v0.43, unchanged) plus the new phase 2 above. A stray reuse of an
already-populated disk image during development produced a misleading
`vfsstrs` failure unrelated to this code — see Honest scope gaps; every
number in this table is from a clean run.

## Version

`0.48.0-metal` -> `0.49.0-metal` (`Makefile`, `mkinstall.sh`,
`KERNEL_VERSION` in `kernel64.c`).
