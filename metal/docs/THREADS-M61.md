# M61 — Ring-3 threads become schedulable entities

This is the architectural outline written **before** the implementation, as the
milestone instructions ask for. It states what exists, what is wrong with it,
what replaces it, and which invariants the replacement has to hold.

## 1. What is there today

The kernel has **two disjoint execution models**, and neither of them is a
mistake — they were built for different jobs.

### Model A — BSP scheduler threads (`struct pcb`, `g_threads[16]`)

`sched_switch_to` / `switch_context`, cooperative `sched_yield` plus PIT
preemption. **The BSP is the only core that runs this scheduler**: `g_cur`,
`pick_next()` and the whole `g_threads[]` array are unsynchronised BSP-local
state. A pcb with `uthread = 1` owns a ring-3 process: it has its own kernel
stack (`TSTACK_SZ`, 16 KiB), its own `rsp0`/`ksrsp` installed by
`uthread_ctx_load`, and it enters ring 3 through `enter_user_thread` with **no
resume point** — it leaves only through `uthread_exit` or a fault.

Used by: kernel worker threads, and `uthread_spawn_elf` (the ELF launcher).

### Model B — per-CPU run queues (`kprocs[]` indices, `cpu_exec_proc`)

The scheduling entity is a **kproc slot index**. Its ring-3 context lives in
`kprocs[p].uctx`; `pstate = 1` means "that context is valid, resume it rather
than entering at the ELF entry point". Any core runs `cpu_exec_proc`, which is
byte-for-byte identical on BSP and APs. Preemption captures the context (IPI 50
cross-core, LAPIC vector 51 self-directed) and unwinds through `resume_kernel`
with the `RET_PREEMPTED` sentinel; `cpu_exec_proc` then requeues the context,
honouring affinity masks and directed migration. `rq_steal` balances.

Used by: essentially every suite in the tree, and every `fork`ed child.

## 2. The defect

`SYS_THREAD_CREATE` (v0.55) builds a **Model A** thread on behalf of a process
that is, in almost every real case, running under **Model B**.

That is an asymmetry, not a feature. The consequence is recorded in the kernel
source itself:

> A POSIX thread here IS a BSP scheduler thread, so only the BSP may create one.
> This restriction is enforced rather than merely documented because of a REAL,
> reproducible SMP hang […] it was NOT root caused — so rather than ship a path
> that can wedge, the kernel refuses it […] Lifting this is the first task of the
> next milestone.

The v0.55 note is honest about not having root-caused the hang. It is worth
saying plainly what the shape of it is, because it explains why the fix is a
rearchitecture rather than a patch:

A process running on an AP under `cpu_exec_proc` calls `SYS_THREAD_CREATE`. The
syscall runs on the AP, but `thread_create_ex` reads and writes `g_threads[]`
and compares against `g_cur` — **BSP scheduler state, from a core that does not
own it**. The thread it produces can only ever be dispatched by the BSP. So one
process now has members in two schedulers that do not know about each other:
the group refcount (`nthreads`) is shared, but *liveness* is not. Whether the
last exit path fires on the BSP (`uthread_exit`) or on any core
(`cpu_exec_proc`) depends on a race, and the two paths reclaim different things
in a different order.

`SYS_YIELD` makes it concrete. On an AP it is `pause` — a no-op:

```c
if (cpu_idx() == 0) sched_yield();
else __asm__ volatile("pause");
```

So a Model-B process on an AP that waits for its Model-A threads by yielding
spins its core forever, and the threads it is waiting for are queued behind a
scheduler on a different core. That is the wedge.

## 3. The change: a thread IS a scheduling entity

**A user thread becomes a Model-B entity: its own kproc slot, sharing its
leader's address space.** This is the `clone(CLONE_VM)` model, and it is the
one this kernel was already converging on — `struct kproc` even carries a
`vma_lock` described in-tree as "the seam a future multi-threaded-per-process
model would actually need".

The payoff is that threads inherit, unchanged and for free, every piece of SMP
machinery built in v0.39–v0.52:

| capability | mechanism it comes from |
| --- | --- |
| runs on any core | `rq_push_any` + each core's `cpu_exec_proc` |
| load balancing | `rq_steal` |
| involuntary preemption | IPI 50 / LAPIC vector 51 → `uctx` capture |
| voluntary yield | `sys_yield_ring3` → `RET_PREEMPTED` requeue |
| pinning | `kproc.affinity`, honoured by steal and migration |
| directed migration | `migrate_to` / `migrate_pin` |
| signal delivery | the `cpu_exec_proc` dispatch boundary |

None of that has to be reimplemented for threads, and — the point — none of it
has to be **kept in step** with a second implementation.

### 3.1 Thread group

`struct kproc` gains:

```c
int      tg_leader;    /* slot of the group leader (own slot for a leader) */
int      tg_tid;       /* index within the group; 0 = leader              */
uint64_t tstack_base;  /* this thread's OWN ring-3 stack (0 = the leader's) */
int      tstack_pages;
```

`nthreads`, `ustack_next`, the signal dispositions, `heap_brk`, `redir_in/out`
and fd ownership stay on the **leader**. `cr3` is *copied* into each member so
every existing `kprocs[p].cr3` read keeps working without a leader lookup —
which matters, because that field is read on the hot dispatch path.

Identity follows Linux, and says so:

* each thread gets its **own** `pid` — that is its thread id, and it is what
  makes `kproc_find_by_pid`, the reap log and the run queues keep working
  unmodified;
* `SYS_GETPID` returns the **leader's** pid, so a thread group has one pid as
  POSIX requires;
* `SYS_GETTID` (new) returns the caller's own.

### 3.2 Creation

`sys_thread_create(entry, arg, stack_ptr)` — the third argument is new, and the
syscall ABI already had room for it (`syscall_dispatch` takes `a0,a1,a2`).

1. Resolve the leader `L`.
2. Claim a kproc slot `T`, sharing `L`'s `cr3`, caps, role, affinity, redirections.
3. Stack: use `stack_ptr` if the caller supplied one (validated writable in
   `L`'s address space), else allocate `THR_STK_PAGES` at
   `THR_USER_V + slot * THR_STK_STRIDE`.
4. **Seed `T->uctx` directly**: `rip = entry`, `rsp = 16-byte-aligned top`,
   `rdi = arg`, `rflags = 0x202`. Set `T->pstate = 1`.
5. `rq_push_any(cpu_idx(), T)`, then IPI-ping the destination core.

Step 4 is the whole trick: `pstate = 1` makes `cpu_exec_proc` take the
`enter_user_resume(&uctx)` branch, so a brand-new thread and a preempted one
enter ring 3 through exactly one path. There is no `enter_user_thread` for
threads any more and no second entry convention to get wrong.

**This runs on any core**, because every structure it touches (`kprocs[]` slot
claim, the frame allocator, `rq_push_any`) is already cross-core safe. The
`cpu_idx() != 0` refusal is deleted.

`arg` is passed in **RDI** — the SysV first-argument register — rather than
v0.55's `[rsp]` convention. The old convention existed only because
`enter_user_thread` set nothing but RIP and RSP. `[rsp]` is still populated so
the existing `/bin/init` pthread shim keeps working unchanged.

### 3.3 Exit and teardown — who owns what

This is where a thread model is usually wrong, so it is stated as a rule:

> **A non-final thread reclaims exactly what is its own — its ring-3 stack and
> its kproc slot. The final member of the group reclaims the address space and
> every shared resource, and it does so against the LEADER's slot.**

`posix_thread_leave(leader)` stays the arbiter; every exit path already calls
it. What changes:

* a non-final thread now unmaps and frees its own `tstack_base` pages before
  releasing its slot — without this, a process that spawns and joins threads in
  a loop leaks 16 KiB a time and `leakcheck` would (correctly) fail it;
* the shared teardown calls (`descriptor_teardown_kproc`, `ipc_teardown_kproc`,
  `dma_teardown_kproc`, `page_free_tree`, …) take the **leader** slot, not
  whichever member happened to exit last;
* `exit_authoritative` keeps its v0.55 meaning: `exit()` sets the group's
  status, `thread_exit()` never overwrites one that `exit()` already set.

### 3.4 Blocking: the futex, and the lost-wakeup window

Synchronisation needs a thread to stop consuming a core. In Model B "blocked"
has a natural spelling: **not in any run queue, and not running**. A blocked
task is simply absent, and a wake is an `rq_push_any`.

The hazard is the classic one. Between a waiter deciding to sleep and its
context actually being safe to run elsewhere, there is a window; a waker that
requeues inside that window hands a half-captured context to another core.
Note that this window is *wider* here than in a conventional kernel, because
the capture finishes in `cpu_exec_proc` — after the syscall has unwound.

So parking is **two-phase**, and the decision to park is made *after* the
unwind, by the core that owns the context:

```
WAIT (syscall, on the waiting core)
    lock
    if (*uaddr != val)        -> unlock, return -EAGAIN     /* no sleep */
    wait_key = phys(uaddr); wait_deadline = now + timeout
    wait_armed = 1
    unlock
    capture uctx; pstate = 1; unwind RET_PREEMPTED

PARK (cpu_exec_proc, same core, context now fully captured)
    if (!wait_armed) -> normal requeue
    lock
    if (wake_pending) { wake_pending = 0; wait_armed = 0; requeue }
    else              { parked = 1;  /* deliberately NOT requeued */ }
    unlock

WAKE (any core)
    lock
    for each member with wait_key == key:
        if (parked)          { parked = 0; wait_armed = 0; collect }
        else if (wait_armed) { wake_pending = 1 }      /* wake in flight */
    unlock
    rq_push_any(collect) + IPI
```

`parked` is only ever set by the core that just captured the context, strictly
after the capture completed — so anything that observes `parked` observes a
complete, runnable context. A wake that arrives during the window sets
`wake_pending` instead, and the parking core requeues itself. Neither order
loses the wakeup.

The key is the **physical** address of the futex word. Threads share a `cr3`
so a virtual address would do, but a physical key costs nothing and means a
futex in shared memory works between processes.

**Timeouts are mandatory, not a convenience.** A lost wake in a kernel with no
timeout is an unrecoverable wedge, and this system is verified by a bounded
boot suite that has to terminate. Every park carries a deadline; a watchdog on
the timer tick requeues expired waiters with `-ETIMEDOUT` in RAX. A futex bug
therefore surfaces as a *failing assertion*, not a hung machine.

`sys_thread_join(tid, *code)` uses the same park/wake machinery with the target
slot as the key instead of a user address, so there is one blocking primitive
in the kernel and join is a caller of it.

### 3.5 Model A is not deleted

`uthread_spawn_elf` still makes pcb threads, and kernel worker threads are pcbs.
Both stay. What changes is that they are no longer what `SYS_THREAD_CREATE`
produces. A pcb uthread that calls `SYS_FUTEX_WAIT` gets a bounded
yield-and-recheck loop rather than a park — correct, less efficient, and
documented as such rather than quietly different. The group refcount already
abstracts over both models, so a process may legitimately have one pcb member
and several run-queue members; that is what makes the transition safe rather
than a flag day.

## 4. Invariants the implementation must hold

1. A thread slot's `cr3` is only ever freed by the transition of the group
   refcount to zero — checked by `leakcheck`, which fails on any frame the
   kernel cannot account for.
2. `parked` is never set on a task whose `uctx` is not complete.
3. A parked task is in no run queue; a runnable one is in exactly one.
4. Every park has a deadline; no park is unbounded.
5. `SYS_GETPID` is group-wide, `SYS_GETTID` is per-thread, and neither is the
   kproc slot index.
6. Shared teardown always names the leader slot.
7. Nothing in the change may make `cpu_exec_proc` behave differently for a task
   that is not a thread — the other 32 suites are the regression gate.

## 5. Scope held back

* No thread-local storage (`fs`-base per thread). Nothing in the tree needs it,
  and adding an unused MSR write to the dispatch path would be cost without a
  consumer.
* No priority or fairness work. Round-robin per core plus stealing is what the
  scheduler is; threads inherit it exactly.
* No `pthread_cancel`. Asynchronous cancellation is a correctness minefield and
  nothing asks for it.
* `occ` still cannot take a function pointer, so this API stays reachable from
  `/bin/init` and not from occ-compiled source — the v0.55 note in `pthread.h`
  remains accurate and is left standing.
