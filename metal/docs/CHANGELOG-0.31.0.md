# Outrun OS v0.31.0-metal — Ring-3 processes as first-class scheduler threads

## The claim this milestone proves

**An unprivileged ring-3 process is now a real scheduler thread** — its own PCB,
its own 16 KiB kernel stack, its own CR3 swapped by the context switch — running
*concurrently* with the kernel compositor. A click is routed into the app's event
queue and the app drains it and repaints **inside the same canvas pass**, with no
kernel re-entry of the process. The v0.30 root blocker ("ring-3 processes are not
scheduler threads; the surface app reacts between canvas passes, not during one")
is dissolved.

Boot-verified evidence (serial + screendump, this exact image):

```
[uthread] spawned 'surface-app': pid 17 = tid 2 (role 2) — scheduled, not entered
[canvas ] frame 30: click synthesized at screen (470,400)
  [app:r3] click at surface-local (1f,14) — repainting NOW, mid-pass
[surfin ] app repainted DURING the pass: 298 hit-marker pixels (f06a18) in its surface
```

Screendump pixel analysis: 347 hit-marker pixels (0xF06A18, a color the kernel
palette never draws) composited on screen at x 527–663, y 451–491 — inside the
VIDEO-EDITOR panel — plus the app's sweeping liveness bar (0x9B4DFF). The click
coordinates map to the same surface-local points proven in v0.30:
(470,400)→(31,20), (560,440)→(91,64), (660,470)→(157,97).

## What changed

### Scheduler: per-thread ring-3 machine context (`kernel64.c`, `boot/switch.asm` unchanged)
- `struct pcb` gains the ring-3 half: `proc` (process identity), `uthread` flag,
  `rsp0` (TSS.rsp0 while this thread runs), `ksrsp` (SYSCALL kernel stack top).
- `sched_switch_to` now swaps, per thread, exactly like it already swapped the
  stack-protector guard:
  - `current_proc_idx` — **the value the capability gate reads** is saved into
    the outgoing PCB and loaded from the incoming one. The boot thread's direct
    assignments (all eight verification suites) keep working unchanged.
  - CR3 — saved **live** (`read_cr3()`) so the boot thread may roam across
    address spaces (fuzz/stress do) and still be restored exactly; only written
    when it differs (kernel↔kernel switches stay TLB-neutral).
  - TSS.rsp0 + SYSCALL stack — a user thread traps from CPL3 onto its OWN
    kernel stack. The two can never be live at once on one thread (a thread at
    CPL3 has no syscall in flight; a thread in a syscall is not at CPL3).
- `thread_create` seeds the new fields; kernel threads keep the boot-default
  shared trap stacks (0 = default).

### SYSCALL path is now multi-thread safe (`boot/usermode.asm`)
- The saved user RSP moves from a static slot onto **the thread's own kernel
  stack** (`push qword [g_ursp]` after the stack switch; the static is now a
  two-instruction scratch under SFMASK-masked IF). A syscall that blocks
  (`SYS_YIELD`, `sys_wait_event`) can no longer have its user RSP clobbered by
  another thread's syscall. Alignment pad added for the extra push.
- New `enter_user_thread`: iretq entry that saves **no resume context** — a
  first-class thread never "returns" to `kernel_main`. The legacy
  `enter_user_mode`/`resume_kernel` pair remains for the boot thread's
  synchronous excursions (all existing suites).

### Thread lifecycle
- `uthread_create(name, proc, entry)` = `thread_create` + ring-3 half; the
  trampoline drops to ring 3 via `enter_user_thread` (CR3/identity/trap stacks
  are already installed by the switch).
- `SYS_EXIT` from a uthread → `uthread_exit`: reclaims surfaces, records the
  exit code in the kproc, marks the PCB `T_FREE`, reschedules. No unwind.
- `handle_cpl3_fault` from a uthread → same reaping (exit code 0x8000+vector),
  **kernel and sibling threads keep running**. Guard-page semantics from v0.19
  are unchanged (message, `g_guard_caught`), and the legacy path still
  `resume_kernel`s for the boot thread (stress #8 passes verbatim).
- New syscalls: `SYS_YIELD` (15) — a real scheduler yield from ring 3 — and
  `SYS_GETPID` (16). Both included in the fuzz pool (now exercising the
  per-thread switch machinery 20000 calls deep).

### Surface lifecycle (fixes the v0.30 leak)
- `surfaces_reclaim(proc)` on every thread death: slot unbound (compositor
  falls back to live system state), event queue cleared, pixel buffer pushed
  on a small free-chunk list.
- `SYS_SURFACE_CREATE` consults the free list before `alloc_frames` — a
  reclaimed buffer is recycled (verified: same physical address reused).

### The app is a real event loop (`user/init.c`)
- role 2 (surface app): `for(;;){ poll events; repaint (scene + liveness bar +
  hit markers); SYS_YIELD; }` — alive across the whole session, including at
  the shell prompt (the shell idle loop yields).
- role 3 (surface-exit probe) and role 4 (identity prober: exits 2 if
  `SYS_GETPID` ever returns another thread's pid after a yield) support the new
  suite.

### Compositor integration
- `canvas_pass(...)`: frame pacing now **yields** (the wait ticks are exactly
  the app's run time) and timer preemption is enabled for the duration, so a
  spinning app cannot starve the compositor. Clicks can be synthesized at
  chosen frames through the same state the mouse IRQ maintains.
- `cmd_surface` spawns the app and yields until the surface binds.
- `cmd_surfin` is now a **single live pass**: 150 frames continuing the
  previous camera state, three clicks at frames 30/60/90, then a kernel-side
  scan of the app's surface for the hit-marker color. The old "re-enter the
  app to drain its queue" code is gone.

### New verification suite: `threads` (7 checks, in the boot sequence and shell)
1. two ring-3 threads ran to completion concurrently with the kernel main thread
2. per-thread process identity survived every context switch (both probers exit 0)
3. SYS_EXIT reaps the thread (PCB slots returned to the scheduler)
4. surface reclaimed when its owner exits (slot unbound, buffer on free list)
5. reclaimed pixel buffer is RECYCLED by the next surface create (same phys)
6. faulting ring-3 thread hit the guard page and was terminated (not unwound)
7. kernel and sibling threads survive the fault (surface app still bound)

### Misc
- `MAX_KPROC` 24 → 40 (the new suite spawns five more processes).
- `rust/cap_engine.rs`: forward-compat for rustc ≥ 1.89 sized-hierarchy lang
  items (`pointee_sized`/`meta_sized`/`sized`, `legacy_receiver`). Codegen
  unchanged — build environment for this release uses rustc 1.94.1.

## Verification (all boot-tested on this exact image)

| Suite | Result |
|---|---|
| validate | 19/19 |
| invariants | 21/21 (incl. 192/192 capability-denial sweep) |
| stress | 10/10 (guard page + canary re-verified with the new fault path) |
| fuzz | 8/8 (20000 adversarial syscalls incl. SYS_YIELD/SYS_GETPID) |
| **threads (new)** | **7/7** |
| cursor | 6/6 |
| kinetic | 8/8 |
| iommu | 2/2 (q35 + intel-iommu config) |
| capdma | 11/11 (q35 + intel-iommu config) |

Boot configurations: BIOS ISO (i440fx), q35 + intel-iommu (`iommu_platform=on`
on both virtio devices), and the dd-able GPT/ESP install image under
UEFI/OVMF booting from NVMe.

## Honest scope gaps (unchanged unless noted)

- **Surfaces are still single-buffered** — the compositor can blit a
  half-repainted frame (tearing). The new concurrency makes this *visible*
  rather than latent; double-buffering (SYS_SURFACE_FLIP) is the natural next
  step now that the app has a real frame loop.
- **Surface pixel buffers are recycled, but process page tables and user
  stacks still leak** on exit — the frame allocator is a bump allocator with
  no general free.
- **One thread per process**; the user stack sits at a fixed vaddr per address
  space, so a second thread in the same process would need its own stack window.
- Input routing: clicks only; keyboard `type=2` still reserved but unrouted.
- The legacy synchronous excursion path (`enter_user_mode`/`resume_kernel`)
  still carries all eight original suites and `cmd_usermode`/`cmd_nicdriver`;
  it is single-instance (boot thread only) and runs with preemption gated, as
  in v0.30.
- Syscalls 13/14 (SURFACE_CREATE/POLL) remain outside the fuzz modulus, as in
  v0.30 (a fuzzed create would clobber live slot bindings); 15/16 are fuzzed.
- The serial console can interleave app and kernel lines mid-character when
  the timer preempts a `kprintf` during a canvas pass — cosmetic, and only
  while preemption is enabled.
- Uniprocessor invariants unchanged: `access_ok` remains TOCTOU-safe only
  because there is no cross-core concurrency and syscalls run with IF masked
  (SFMASK) except at explicit yield points.
- VT-x still unusable under TCG (no KVM in the build environment); IOMMU/VT-d
  remains fully real. Kinetic easing still tuned to the ~50 fps loop.
