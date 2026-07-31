# OutRun OS v0.61.0-metal — a thread becomes a scheduling entity

Milestone 61. Ring-3 threads stop being a special case owned by one core and
become ordinary run-queue tasks, which is what lets them be created anywhere,
run anywhere, and — for the first time — **block without spinning**.

The design was written before the code and is in `docs/THREADS-M61.md`. This
document is what happened.

## THE RESTRICTION THIS REMOVES

v0.55 shipped `SYS_THREAD_CREATE` with a refusal in it, and said why:

> A POSIX thread here IS a BSP scheduler thread, so only the BSP may create one.
> […] because of a REAL, reproducible SMP hang […] it was NOT root caused — so
> rather than ship a path that can wedge, the kernel refuses it. Lifting this is
> the first task of the next milestone.

That was the honest thing to do and it is now paid off. The restriction is gone,
and so is the reason for it.

## WHAT THE HANG ACTUALLY WAS

The kernel has two execution models, and neither is a mistake:

| | Model A — `struct pcb` | Model B — run queues |
| --- | --- | --- |
| scheduling entity | `g_threads[]` slot | `kprocs[]` slot index |
| who runs it | **the BSP only** | every core, `cpu_exec_proc` |
| context lives in | its kernel stack | `kprocs[p].uctx` |
| gets | cooperative + PIT preemption | stealing, IPI preemption, affinity, migration |

v0.55's `SYS_THREAD_CREATE` built a **Model A** thread for a process that, in
almost every real case, was running under **Model B**. One process then had
members in two schedulers that did not know about each other: the group
refcount was shared, but liveness was not, and which exit path fired last was a
race between two paths that reclaim different things in a different order.

`SYS_YIELD` made it terminal. On an AP it was literally `pause` — a no-op. So a
Model-B process on an AP waiting for its Model-A threads spun its core forever
while the threads it waited on sat behind a scheduler on a different core.

The fix is not a patch to that arrangement. It is removing the asymmetry.

## A THREAD IS NOW ITS OWN KPROC SLOT

`clone(CLONE_VM)`, essentially: a thread gets its own scheduling identity and
shares its leader's address space. `struct kproc` gained `tg_leader` / `tg_tid`,
and the leader owns everything shared — the address space, the fd table, the
signal dispositions, the heap, the group refcount.

The payoff is that threads inherit, unchanged, every piece of SMP machinery
built between v0.39 and v0.52:

| capability | comes from |
| --- | --- |
| runs on any core | `rq_push_any` + each core's `cpu_exec_proc` |
| load balancing | `rq_steal` |
| involuntary preemption | IPI 50 / LAPIC vector 51 → `uctx` capture |
| voluntary yield | `sys_yield_ring3` → `RET_PREEMPTED` requeue |
| pinning, migration | `affinity`, `migrate_to` / `migrate_pin` |

None of it was reimplemented for threads. More to the point, none of it now has
to be **kept in step** with a second implementation.

Creation seeds `uctx` directly — `rip = entry`, `rsp = stack top`, `rdi = arg` —
and sets `pstate = 1`, which is the flag meaning "resume this context rather
than entering at the ELF entry point". So a brand-new thread and a preempted one
enter ring 3 through one path, `enter_user_resume`. There is no second entry
convention to get wrong, and no assembly was written for any of this.

`SYS_THREAD_CREATE` takes a third argument now, a caller-supplied stack top;
0 still means "kernel, give me one", so the two-argument form is unchanged. The
argument reaches the thread in **RDI** rather than v0.55's `[rsp]`, because a
seeded context can use the ordinary calling convention. `[rsp]` is still written
so the v0.55 pthread shim keeps working untouched.

Identity follows Linux and says so: each thread has its own pid (that is its
tid), `SYS_GETPID` reports the **leader's**, and `SYS_GETTID` reports the
caller's own.

## BLOCKING, AND THE WINDOW THAT MAKES IT HARD

In this model "blocked" has a natural spelling: **in no run queue, and not
running**. Waking is an `rq_push_any`.

The hazard is the classic lost wakeup, and it is *wider* here than in a
conventional kernel: the context capture completes in `cpu_exec_proc`, after the
syscall has already unwound through `resume_kernel`. A waker that requeues
inside that gap hands a half-captured context to another core.

So the decision to park is made **after** the unwind, by the core that owns the
context, and arming is separated from parking:

```
WAIT  (waiting core)   value check, publish wait_key + wait_armed, capture, unwind
PARK  (same core, in cpu_exec_proc, capture now complete)
                       wake_pending ? requeue : parked = 1
WAKE  (any core)       parked ? requeue : wake_pending = 1
```

`parked` is only ever set strictly after the capture completed, so anything that
observes it observes a runnable context. A wake arriving during the arming
window sets `wake_pending` and the parking core requeues itself. Neither
interleaving loses the wakeup. The suite counts how often a wake lands in that
window, so the race is *observed* rather than assumed.

**Every park carries a deadline, and that is not a convenience.** A lost wake
with no timeout is an unrecoverable wedge, and this system is verified by a
bounded boot suite that has to terminate. With one, a futex bug costs a failed
assertion instead of the machine. The scan that enforces it runs when a core
finds nothing to run — deliberately *not* on the timer tick, because it requeues,
requeueing takes run-queue locks, and a timer interrupt landing on a core that
already holds its own `rq_lock` would deadlock against itself.

New syscalls: `SYS_FUTEX_WAIT` (64), `SYS_FUTEX_WAKE` (65), `SYS_THREAD_JOIN`
(66), `SYS_GETTID` (67). The futex key is the word's **physical** address —
threads share a `cr3` so a virtual one would do, but physical costs nothing more
and makes a futex in shared memory work between processes.

`SYS_THREAD_JOIN` answers `-EAGAIN` to mean "you slept, the state changed, ask
again". That is not a shortcut: a woken task resumes with only RAX to carry a
result, and the waker runs in a different address space, so it cannot fill in
the joiner's output pointer. The retry loop is two lines of userland and the
alternative would have been a lie about where the value came from.

## A REAL BUG THIS FOUND: THE EXEC STAGING BUFFER

`g_execbuf` is **one** 256 KiB global buffer. Both exec paths read a whole ELF
image into it and then parse it out of it, and neither serialised that. Its
signature in a boot log is unmistakable once you know it:

```
[kernel ] spawned pid 666 ...
[kernel ] spawned pid 667 ...
[elf    ] PT_LOAD ... filesz 0000000000001b50 ...      <- pid 666, /bin/emit
[elf    ] reject: segment file range out of bounds     <- 667 overwrote it
[elf    ] PT_LOAD ... filesz 0000000000001c63 ...      <- pid 667, /bin/wcx
```

One core's image replaced the other's mid-parse, so the second segment header
pointed past the end of what was now a different, shorter file. The exec was
refused — correctly, given the buffer's contents — and the program never ran.
In `a | b`, that empties the pipeline.

**This is the v0.60 "pipestrs flake".** That changelog recorded the pipeline
assertion as timing-sensitive, noted it passed and failed on a byte-identical
ISO, and did not root cause it. It is not a flake; it is this, and it has been
present since v0.56 gave the system a second way to exec. The counting is
consistent: a uniprocessor log contains exactly two `segment file range out of
bounds` lines, both from the `[valid]` suite's deliberate malformed-ELF tests,
and a failing SMP run contains three.

Threads neither caused it nor depend on it. But a milestone about running more
things on more cores at once is the wrong one in which to leave a concurrency
defect in the exec path unfixed, so it is fixed: a yield-aware lock held from
the read through the parse, released before the process is entered. The suite
reports the contention count, so the claim "two cores really do exec at the same
time" is evidence rather than assertion.

## THE THREADSTRS SUITE

Role 43 runs four futex-synchronised workers, a thread on a caller-supplied
stack, and one that genuinely sleeps until woken. The critical section does its
read-modify-write **across a reschedule**, so a mutex that is not really
exclusive produces a short count rather than a slow run.

The kernel half audits what the driver cannot honestly claim about itself:
which cores its threads were dispatched on, whether their stacks came back to
the allocator, whether anything is still parked when the round is over, and
whether the group refcount reconciled. Two checks exist specifically because a
suite can pass for the wrong reason:

- **"threads genuinely PARKED rather than spinning."** A futex whose wait
  silently returned immediately would still produce the right counter.
- **"the caller's stack was NOT freed."** Five kernel-allocated stacks come back
  and the sixth must not, because reclaiming memory the kernel did not map is a
  bug that only shows up much later.

The headline assertion is **"threads were dispatched on MORE THAN ONE core"**.
Before v0.61 that mask was always exactly 1, by construction.

## VERIFICATION

Zero failures across every suite on all three configurations: uniprocessor/BIOS,
SMP-4/BIOS, and q35 + VT-d IOMMU (`-smp 4`). Boot logs are in `docs/`.

On SMP the new suite reports `ran_on mask f` — all four cores ran ring-3
threads — and `g_inr3_max >= 2`, so at least two were inside ring 3 at once.

### What did not change

The v0.55 pthread shim and its suite (role 31, `posixstrs`) are untouched, and
that is deliberate: they are the regression gate proving this rearchitecture is
invisible to code written against the old API. `uthread_spawn_elf` still makes
Model-A pcb threads and kernel worker threads are still pcbs; a pcb that calls
`SYS_FUTEX_WAIT` gets a bounded yield-and-recheck loop rather than a park —
correct, less efficient, and documented rather than silently different.

### Held back on purpose

- No thread-local storage. Nothing needs it, and an unused MSR write on the
  dispatch path is cost without a consumer.
- No priority or fairness work. Threads inherit round-robin-plus-stealing
  exactly as processes have it.
- No `pthread_cancel`. Asynchronous cancellation is a correctness minefield and
  nothing asks for it.
- `occ` still cannot produce a function pointer, so this API remains reachable
  from `/bin/init` and not from occ-compiled source. The v0.55 note saying so is
  still true and still in `pthread.h`; the parts of it that stopped being true
  were corrected rather than left standing.

### Still ahead

`VFS_MAXFILES` is 64 and v0.60 spent the last of the headroom; `[vfs] directory
full` still appears late in a boot. Raising it changes `VFS_DIR_BLOCKS` and the
on-disk layout, so it needs a format-version bump and belongs in its own change.
