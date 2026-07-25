# Outrun OS v0.55.0-metal — POSIX Compliance & C Runtime Completeness

Ring 3 grows a real process model. `fork()` returns twice; `execve()` replaces a
running image and hands it a genuine SysV `argc`/`argv`/`envp` block; signals are
delivered onto the process's own stack and restored through `SYS_SIGRETURN`; a
catchable `SIGSEGV` lets a process survive a wild write; and several POSIX
threads share one address space with per-thread stacks, joined and synchronised
through a userland `pthread` shim built on ordinary atomics.

## What's new

### Process lifecycle: `fork` and `execve`

The blocker for a real `fork` was that `syscall_entry` handed C only the
caller-saved half of the ring-3 register file, so the kernel had no complete
context to duplicate. That is fixed at the root: the SYSCALL entry now spills
**every** ring-3 register and passes the block's address to C as a
`struct sysframe *`. With that,

- **`SYS_FORK` (47)** copies the parent's register file into the child's saved
  context with `RAX` forced to 0, marks the child *preempted*, and queues it. The
  scheduler resumes it through the existing `enter_user_resume` path — the same
  one a migrated task takes. Ring 3 does nothing special: `fork()` is a plain
  syscall that returns 0 in the child and the child's pid in the parent, with
  every other register identical. There is no userland continuation trampoline.
- Address-space duplication is a **clean eager page-table clone**
  (`vm_clone_user`), not copy-on-write. Every USER page below `UPRIVATE_VMAX`
  gets a new frame with identical contents *and identical PTE flags*, so a
  read-only text page stays `R+X` in the child and W^X holds on both sides.
  Windows at or above `UPRIVATE_VMAX` — device DMA, shared window surfaces, the
  SMP scratch window, per-thread stacks — are deliberately **not** cloned:
  duplicating an MMIO alias would hand the child a device it never asked for.
- **`SYS_EXECVE` (48)** builds the replacement image in a *fresh* address space
  first, so a failed exec leaves the caller running rather than dead, then
  switches CR3 and dismantles the old space. Same pid, same kproc slot, new
  image. Caught signal dispositions are reset to default (POSIX) while `SIG_IGN`
  survives, and the environment is **replaced wholesale** — the suite asserts
  the kernel's default env is *gone* after an exec, because a merge would not be
  an exec.
- **`SYS_WAITPID` (56)** and **`SYS_GETPPID` (55)**; a child's exit raises
  **`SIGCHLD`** to its parent.

### POSIX signals

- **`SYS_SIGACTION` (49)**, **`SYS_KILL` (50)**, **`SYS_SIGRETURN` (51)**,
  **`SYS_ALARM` (54)**, **`SYS_SIGUNMASK` (57)**. Core signals `SIGINT`,
  `SIGKILL`, `SIGSEGV`, `SIGCHLD`, `SIGALRM`.
- Delivery spills the interrupted context as a `struct sigframe` on the
  process's **own user stack** (below a 128-byte red-zone gap, 16-byte aligned,
  `access_ok`-checked), vectors `RIP` at the handler with `RDI = signo`, and
  blocks that signal for the duration. A ring-3 trampoline calls the installed
  handler and finishes with `SYS_SIGRETURN`, which restores the frame through
  **`iretq`** (`enter_user_ctx`) rather than `SYSRET` — `SYSRET` clobbers `RCX`
  and `RFLAGS` and therefore *cannot* restore an interrupted context exactly.
- `SYS_KILL` is capability-contained: a process may signal **itself or its own
  children**, nothing else.
- **Three delivery points**, each holding a complete context legitimately:
  1. **the trap frame** — a ring-3 `#PF`/`#GP` becomes a catchable `SIGSEGV`
     *before* `handle_cpl3_fault` switches off the faulting address space, so the
     faulting thread can survive;
  2. **the syscall-return boundary** — the deterministic one, and the reason the
     suite is meaningful on a uniprocessor at all;
  3. **the scheduler boundary** in `cpu_exec_proc`, for a preempted `uctx`.
- **Catchable `SIGSEGV` genuinely recovers.** A handler cannot simply return —
  the kernel resumes the *faulting instruction*, which would fault forever — so
  userland gains real `setjmp`/`longjmp` and the handler longjmps out. Because
  delivery blocks the signal, leaving by longjmp would leave it blocked forever;
  `SYS_SIGUNMASK` is that idiom's missing half (our narrow
  `sigprocmask(SIG_UNBLOCK)`), and it also drops the orphaned frame so a forged
  `SYS_SIGRETURN` cannot replay it. The suite recovers from **two** wild writes
  in a row, which is what proves the unblock actually works.

### Threading

- **`SYS_THREAD_CREATE` (52)** / **`SYS_THREAD_EXIT` (53)**. A new thread shares
  the process's cr3, globals and heap, and gets its own ring-3 stack in a new
  `THR_USER_V` window (4 mapped pages + 4 unmapped guard pages per thread) plus
  its own kernel stack. `pcb.ustack` is new: before this, every uthread entered
  at one fixed stack top, which is fine when a kproc owns exactly one thread and
  corrupts instantly once several share an address space.
- **Refcounted thread-group teardown** (`posix_thread_leave`). All three exit
  paths — `uthread_exit`, `cpu_exec_proc`'s exit branch, and `handle_cpl3_fault`
  — now dismantle the address space only for the **last** thread out. A fault in
  a multi-threaded process posts an unblockable `SIGKILL` to the group, and
  siblings self-terminate at their next syscall boundary.
- **Userland `pthread` shim**, entirely in ring 3: `pthread_create`,
  `pthread_join`, `pthread_exit`, `pthread_mutex_init/lock/trylock/unlock` on
  `__sync` atomics over shared memory. `pthread_self()` is derived from `RSP`
  (thread *N* owns the window `THR_USER_V + N*stride`) — no TLS register and no
  kernel query needed.

### C runtime

- A real **`crt0`**: `_start` is assembly, because a C function cannot make
  promises about `RSP` on entry. It pulls `argc`/`argv`/`envp` off the stack,
  aligns, calls `crt0_main` → `main(argc, argv, envp)`, and turns `main`'s return
  value into `SYS_EXIT`.
- Every image now starts with a **SysV process-start block** in the top 1 KiB of
  its ring-3 stack (`argc`, `argv[]`, `NULL`, `envp[]`, `NULL`, strings), and
  enters with `RSP` pointing at it — the standard x86-64 layout. `USTK_INIT` is
  the new initial `RSP`; the block always sits *above* it, so ordinary pushes can
  never clobber it. Fresh images get a default `argv[0]` and
  `PATH`/`OUTRUN`/`HOME`; `execve` overwrites it with the caller's vectors.
- **Standard file descriptors.** A userland fd table reserves 0/1/2 for
  stdin/stdout/stderr (console-backed) and hands out 3 and up, mapping each to
  the kernel descriptor underneath. Being ordinary process memory, it is
  inherited byte-for-byte by `fork` and rebuilt by `execve`'s fresh `crt0` —
  which is the correct POSIX result here, since 0/1/2 are always the console.
  `stdout` is not closable, and `open()` can never collide with the std three.

### Cooperative ring-3 yield (prerequisite, and a real fix)

`SYS_YIELD` from a *queued* ring-3 task previously did nothing useful —
`sched_yield` switches BSP **kernel** threads, and a queued task is not one. On a
uniprocessor that made any wait-for-another-process pattern (`fork` +
`waitpid` above all) an infinite spin: the single core was inside the waiter's
ring-3 code and never came back to dispatch the child. `SYS_YIELD` now captures
the caller's context and unwinds with `RET_PREEMPTED`, requeueing it — voluntary
preemption through the exact machinery that already implements involuntary
preemption.

### `cmd_posix_stress` (new suite, `posixstress`)

Three rounds of **five** real ring-3 workers, all queued together so the
scheduler interleaves them:

| role | what it proves |
| --- | --- |
| 29 | `fork()` → child, `waitpid()` its status, receive `SIGCHLD`; child checks its own pid ≠ parent's and `getppid()` |
| 30 | `SIGSEGV` recovered **twice** via `setjmp`/`longjmp`; a `SIGINT` delivered on a syscall boundary with **every callee-saved register and the syscall's own return value** re-checked afterwards; `SIGALRM` from the timer |
| 31 | 4 pthreads × 200 mutex-guarded increments, joined, **exact** total; `trylock` semantics; per-thread return values |
| 32 → 33 | `execve` into a new image that verifies the `argv`/`envp` it received and that the old environment is gone |
| 34 | std fd table: kernel default env present, 0/1/2 reserved, `open() ≥ 3`, inherited by `fork`, `stdout` not closable |

Each worker encodes its verdict in its **exit code** (7xx/8xx/9xx families), so a
failure names the exact ring-3 assertion that broke. The kernel half then
re-checks, from outside, what ring 3 cannot honestly self-report: that children
had a **distinct pid *and* a distinct cr3**, that every child's space was fully
reclaimed, that the signal counters advanced, that no thread group was left
accounted live, and that not one frame or descriptor leaked.

## Nine bugs found live during this milestone

Every one was diagnosed from boot logs — several only after adding breadcrumbs
and reading what the kernel actually printed — and six of them reproduce only on
SMP. Three were flushed out by deliberately **oversubscribing the host** (two
4-vCPU TCG guests on 4 cores), which widens cross-core windows enormously; that
turned out to be the single most productive test technique of this milestone.

1. **Dangling thread name.** `struct pcb::name` is a *borrowed* pointer, not a
   copy. `SYS_THREAD_CREATE` built the name in a stack local, which dangled the
   instant the syscall returned — thread names came back as whatever last used
   that stack (`'motd'`). Fixed with a static name table.
2. **Thread-slot TOCTOU race (SMP only).** `thread_create` did
   "is this slot free? … ok, take it" with no atomic claim. Harmless while only
   the BSP created threads; the moment `SYS_THREAD_CREATE` became callable from
   any core, two cores could build two threads on one PCB. Fixed by CAS-claiming
   the slot into a new non-runnable `T_CLAIMED` state and publishing
   `T_RUNNABLE` last — the same reservation discipline as the v0.54 window-slot
   fix.
3. **Half-built thread published as runnable (SMP only).** `uthread_create`
   installed `cr3`, `proc`, `rsp0`, `ksrsp` and `ustack` *after* `thread_create`
   had already set `T_RUNNABLE`, relying on `cli` for safety — but `cli` masks
   the **creating** core, not the BSP that does the scheduling. Once creation
   could happen on an AP, the BSP could pick up a thread with the kernel's CR3
   and no user stack. Fixed with a `start_suspended` variant: the PCB is built
   completely, then `thread_release()` publishes it.
4. **`for(;;) hlt` under `cli` after a declined switch.** If `sched_switch_to`
   ever declines to switch (`nextid == g_cur`), the thread-exit tails halted with
   interrupts off — on the BSP that stops `g_ticks` and makes *every* watchdog in
   the kernel unreachable, turning one stalled thread into a total freeze. Now
   `sti; hlt`.
5. **Unreachable watchdog.** The suite's drain loop dispatched-then-`continue`d,
   so with a worker in a yield loop (constantly requeueing itself) the timeout
   check was never reached and a stalled round spun forever instead of failing.
   Checked on every pass now.
6. **Exit status lost to slot recycling (SMP only).** A kproc slot is recycled
   the instant it is marked `torn_down`, and `fork` is a prolific claimer of
   freed slots — so a worker's exit code could be overwritten by another
   worker's child before anything could read it. First this misattributed a
   failure to the wrong worker's label; then it hung a round outright. Fixed
   properly, in the kernel: a small **reap log** records `(pid, exit_code,
   frames_freed)` at teardown, *before* the slot becomes recyclable. That is the
   piece a real kernel keeps as a zombie, and it is now the authoritative source
   for exit status.
7. **`pthread_exit` could overwrite the process's exit status.** POSIX is
   explicit that `exit()` sets the *process* status while `pthread_exit()` sets
   only a thread's. The sibling-live exit path discarded the code entirely, so
   the last *thread*'s `SYS_THREAD_EXIT(0)` became the process's status —
   observed as a pthread worker "exiting 0" when it had in fact exited 902.
   `exit_authoritative` now records which status wins.
8. **Threads starved by the drain loop, deterministically.** Queued ring-3 tasks
   and BSP scheduler threads are two different schedulers, and POSIX threads live
   in the second. A task in a yield loop requeues itself instantly, so the queue
   is never empty and the drain loop monopolised the BSP — its own process's
   threads never ran and every `pthread_join` timed out (exit 902, every round).
   The loop now yields the boot thread after **every** dispatch, not only when the
   queue runs dry.
9. **`SIGALRM` lost to a double disarm (SMP only).** `sig_check_alarms` runs on
   every core at every syscall boundary, so two cores could see the same alarm
   expired and *both* decrement the fast-path gate counter — driving it to zero
   while another process still had an alarm armed, whose signal then never fired
   at all. Presented as `SIGALRM` missing in exactly one round out of three.
   Fixed by claiming the deadline with a compare-and-swap before firing.

   Related, and found the same way: `sys_yield_ring3` unwinds through
   `resume_kernel` and never returns, so the syscall-return delivery boundary was
   unreachable for a task whose only syscall is `SYS_YIELD`. A process polling in
   a yield loop could never receive a signal. Signals are now delivered *before*
   the yield takes effect.

## Honest scope notes

- **Eager copy, not copy-on-write.** The milestone brief allowed either. COW
  needs a `#PF` handler that can distinguish a protection fault on a shared
  frame from a genuine access violation, plus per-frame refcounts; neither
  exists. An eager copy has identical observable semantics, and it reuses
  `alloc_frame` and `page_free_tree` unchanged, so a forked child is reclaimed
  by the existing path with no special cases. It is slower, and that is the
  whole cost.
- **`execve` reloads the *same* ELF.** There is one ring-3 image in this system
  (`user_init.elf`, shipped as a boot module), so `execve` takes a role selector
  rather than a path. Everything else about it is real — argv/envp marshalled out
  of the dying address space, fresh image, old space reclaimed, same pid. A path
  argument becomes meaningful only once the VFS can store a second executable.
- **Kernel file descriptors are not duplicated by `fork`.** Each kernel fd has
  exactly one owning kproc, which is what lets `descriptor_teardown_kproc`
  guarantee no leaks across 25 suites. A child therefore inherits the fd *table*
  but a read through an inherited **file** fd is cleanly denied. The suite
  asserts that containment rather than assuming inheritance. Sharing them
  properly needs per-descriptor refcounts.
- **`waitpid` is non-blocking.** There is no ring-3 sleep/wake queue yet, so a
  waiter polls and yields (and so does `pthread_join`). The status itself is
  real, and now comes from the reap log.
- **POSIX threads are scheduled by the BSP, and `SYS_THREAD_CREATE` is
  BSP-only.** A thread created here is a BSP scheduler thread, so the kernel
  now **refuses** creation from an AP with `-EAGAIN` and the suite pins
  thread-creating processes to cpu 0 with the existing affinity mask. That
  restriction is enforced rather than merely documented because of a real
  reproducible hang: with the process on an AP and its threads on the BSP, the
  process's main thread would occasionally never be dispatched again after its
  last thread exited, and the machine went idle with no core making progress. It
  only reproduced with the host oversubscribed, and it was **not root-caused** —
  so rather than ship a path that can wedge, the kernel closes it. Lifting this
  (per-core thread scheduling, so a process and its threads share a core) is the
  first task of the next milestone.
- **Queued ring-3 tasks and BSP scheduler threads are two schedulers that do
  not cooperate automatically.** A yielding queued task requeues itself
  instantly, so anything driving a run queue must also give the thread scheduler
  a turn or the threads starve (bug 8 above). The POSIX suite's drain loop does;
  a general-purpose dispatcher would need the same discipline, and unifying the
  two schedulers is the honest long-term fix.
- **`fork` from a multi-threaded process is refused** (`-EAGAIN`) rather than
  given the usual "only the calling thread survives" semantics. Refusing is
  honest; half-implementing it would not be.
- **`SIGALRM` and `SIGCHLD` default to ignore** rather than POSIX's terminate,
  so an unhandled timer or child exit cannot kill a process in this kernel.
  `SIGSEGV`, `SIGINT` and `SIGKILL` have their POSIX defaults.
- **8 threads per process** and 8 `argv`/`envp` entries of 48 bytes each are
  fixed ceilings, sized to the 1 KiB start block.

## Verification

GRUB ISO in QEMU, TCG-only (no KVM), virtio-vga + virtio-blk + virtio-net.
Disk images recreated fresh before every boot, one guest at a time.

| Config | Command | `posixstress` | `appsstress` | Result |
| --- | --- | --- | --- | --- |
| Uniprocessor, BIOS | `make qemu` | 11/0 | 8/0 | all 29 suites **0 FAIL** |
| SMP `-smp 4`, BIOS | `make qemu` + `-smp 4` | 12/0 | 9/0 | all suites **0 FAIL** |
| q35 + VT-d IOMMU, `-smp 4` | `make qemu-iommu` + `-smp 4` | 12/0 | 9/0 | all suites **0 FAIL** (`capdma` 11/0) |

(The SMP rows carry one extra check each — the genuine cross-core ring-3 overlap
assertions that only a multi-core config can support.)

All prior suites (`appsstrs`, `wimpstrs`, `gpustrs`, `netstrs`, `vfsstrs`,
`ipcstrs`, `vfiostrs`, `kpstrs`, `leakchk`, `dmastrs`, `cio`, `audit`, the SMP
suites and the compositor suites) continue to pass with 0 failures. `audstrs`
SKIPs where no virtio-sound device is attached, unchanged from v0.51.

### A note on how these runs were conducted

**Run one guest at a time.** Several suites assert genuine cross-core properties
(`audit`'s work distribution, `appsstress`'s window ownership) with tick-based
watchdogs. With three 4-vCPU TCG guests sharing 4 host cores, a guest simply
does not get four cores, and those assertions fail for reasons that have nothing
to do with the kernel — observed exactly that way during this milestone, and
confirmed by re-running the same binary solo to 0 FAIL.

The inverse is also true and far more useful: **deliberately oversubscribing the
host is the best SMP bug-finder in this project so far.** Three of the nine bugs
above (the alarm double-disarm, the pthread starvation, the unresolved
thread-scheduling hang) surfaced only under two concurrent 4-vCPU guests, where
cross-core windows widen by orders of magnitude. It is worth doing on purpose —
just not while measuring a pass/fail matrix.

`make EXTRA=-DPOSIX_ITER` builds a fast-iteration kernel that boots straight
into `cmd_posix_stress` and stops (~30 s instead of ~7 min); it is never used for
a release build.
