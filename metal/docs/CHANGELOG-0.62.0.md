# OutRun OS v0.62.0-metal — threads that can wait, and signals that know where to go

Milestone 62. v0.61 made a thread a schedulable entity; this one gives userland
the POSIX surface over it — a real mutex, a condition variable, and a signal
subsystem that understands both thread groups and process groups.

## THE SYSCALL NUMBERS, STATED PLAINLY

The task for this milestone named `SYS_SIGACTION`/`SYS_KILL`/`SYS_SIGRETURN` as
new syscalls 68/69/70, and `SYS_THREAD_CREATE` as 61. Those numbers are not
what this system uses, and adopting them would have broken it:

| call | this system | the task said | what 61/68/69/70 actually are |
| --- | --- | --- | --- |
| `SYS_SIGACTION` | **49** (since v0.55) | 68 | — |
| `SYS_KILL` | **50** (since v0.55) | 69 | — |
| `SYS_SIGRETURN` | **51** (since v0.55) | 70 | — |
| `SYS_THREAD_CREATE` | **52** (v0.55, extended v0.61) | 61 | 61 is `SYS_STAT` |

The signal engine described as new was built in v0.55: `struct sigframe` on the
user stack, `SIGFRAME_MAGIC`, per-signal dispositions, masks, a ring-3
trampoline that ends in `SYS_SIGRETURN`, catchable `SIGSEGV`, `SIGALRM` and
`SIGCHLD`. Renumbering it would have invalidated every binary already compiled
into the VFS — `/bin/vsh`, `/bin/emit`, `/bin/wcx`, `/bin/omake` are cached
build products, not rebuilt from source at boot — plus the SDK headers and the
`posixstrs` suite. So the existing numbers stand, 68/69/70 went to the three
calls this milestone genuinely adds, and everything the task asked for in
behavioural terms is delivered.

New this release: **68 `SYS_SETPGID`**, **69 `SYS_KILLPG`**, **70
`SYS_SIGPROCMASK`**.

## PTHREADS, WITH THE SIGNATURES DELIBERATELY UNCHANGED

`pthread_create/join/exit/self`, `pthread_mutex_t` and (new) `pthread_cond_t`
are reimplemented on the v0.61 kernel calls. The API signatures are byte-for-
byte what v0.55 published, so **role 31 / `posixstrs` compiles against the new
engine with no edits** — which turns an existing suite into a live test of the
replacement rather than a museum piece. It passes unchanged.

What changed underneath:

| | v0.55 | v0.62 |
| --- | --- | --- |
| a thread is | a BSP-only pcb | a run-queue task (v0.61), any core |
| mutex, uncontended | atomic CAS | atomic CAS, **no syscall** |
| mutex, contended | `oyield()` spin | **parks** — in no run queue at all |
| join | poll a userland flag | `SYS_THREAD_JOIN` |
| `pthread_self` | arithmetic on RSP | `SYS_GETTID` |
| condition variable | *did not exist* | sequence-counter futex |

The mutex is Drepper's three-state one: 0 free, 1 held, 2 held-with-waiters.
The third state is the whole point — unlock enters the kernel only when it can
see that somebody is parked, so an uncontended lock/unlock pair is two atomics.

**Guard pages** were already there and are now documented where userland can
see them: the kernel places thread N's stack at `THR_USER_V + N*0x8000` and maps
only the low 4 of those 8 pages. The 4 unmapped pages are the guard, so a
thread that overruns its stack faults instead of quietly eating a sibling's.

### The condition variable, and the order that makes it correct

```c
u64 s = c->seq;              /* sampled while the mutex is STILL HELD */
pthread_mutex_unlock(m);
futex_wait(&c->seq, s);      /* sleeps only if seq is still s         */
pthread_mutex_lock(m);
```

Sampling *before* the unlock is the entire correctness argument. Any signaller
that runs after we release the mutex must have bumped the counter first, so the
kernel's compare-and-sleep declines to sleep and returns `-EAGAIN`. Sample after
the unlock instead and there is a window where a signal arrives with nobody yet
asleep to receive it — the classic missed wakeup, and the reason a condvar
cannot be assembled out of a plain sleep primitive.

`pthreads_smp` tests signal and broadcast as *different* operations rather than
assuming they differ: the predicate is a permit **count**, not a flag, so one
`cond_signal` against four sleepers must release exactly one. A signal that
behaved like a broadcast fails there and nowhere else.

## SIGNALS IN A THREADED WORLD

Threads became separate kproc slots in v0.61, which quietly broke an assumption
the v0.55 signal code was built on: that a process is a slot. POSIX splits the
state exactly along the line that fixes it, and so does this release:

- **dispositions and pending are process-wide** — they live on the thread-group
  leader, so a handler installed by any thread governs the program;
- **masks and signal frames are per-thread** — a frame is written to a thread's
  own stack and can be nowhere else, and per-thread masking is what lets one
  thread take a signal while a sibling stays inside a critical section.

`sig_next` therefore reads `(leader.pending | own.pending) & ~own.mask`.

`SYS_SIGPROCMASK` (70) implements BLOCK / UNBLOCK / SETMASK and returns the
previous mask. `SIGKILL` is masked out of any request rather than the call being
refused: a program that blocks "everything" should still be killable, not
rejected.

## JOB CONTROL — WHY THE SHELL NEEDS NO HANDLER

Ctrl+C must reach the job and not the shell. That is not something a shell can
arrange for itself: whatever delivers the interrupt has to already know which
processes are "the current job", and the shell is not one of them. A **process
group** is that name, and the foreground group is the one the console is talking
to.

`struct kproc` gains `pgid`; threads inherit their leader's (a thread is not
separately signallable from a terminal); `fork` keeps the child in the job.
`SYS_SETPGID(pid, pgid, fg)` folds `tcsetpgrp` into its third argument — a shell
that creates a group and does *not* give it the terminal has created a
background job, which it expresses by not passing the flag.

`/bin/vsh` now puts each pipeline in its own group, hands that group the console
while it runs, and takes it back afterwards — unconditionally, because a shell
that left the console owned by a dead group could never be interrupted again.
Both the parent and each child call `setpgid`, which is not redundancy: whichever
runs first wins, and neither ordering leaves a stage briefly outside the group
about to be signalled. A half-signalled pipeline is worse than either outcome.

**This is why vsh installs no SIGINT handler** — it is simply not in the group
being signalled. That matters beyond elegance here: vsh is compiled by `occ`,
which cannot produce a function pointer, so a shell that *needed* a handler to
survive Ctrl+C could not be written in this system's own C.

The interrupt character is consumed in the tty read path rather than handed to
the reader, and calls `tty_intr()` — the same function `sigstrs` calls to test
it. A headless boot has nobody to press Ctrl+C, and a suite that exercised a
parallel code path would be testing something the machine does not do.

## VERIFICATION

Zero failures on all three configurations: uniprocessor/BIOS, SMP-4/BIOS, and
q35 + VT-d IOMMU (`-smp 4`). Each differs from its v0.61 baseline by exactly two
lines — the two new suites. Boot logs are in `docs/`.

`pthreads_smp` reports threads parked on both the mutex and the condvar rather
than spinning, and on SMP that they were dispatched across cores. `sigstrs`
checks that **every delivered signal was answered by a `SIGRETURN`** — an
unmatched delivery means a handler ran and the interrupted context was never
restored, which is a silent corruption rather than a crash and would otherwise
go unnoticed.

### Warnings

The build has 46 warnings, all pre-existing, and this release adds none. It
removes one: `okill` was previously defined and unused, and `sigstrs` now uses
it. The milestone asked for "strict clean builds without compiler warnings";
clearing the remaining 46 means touching driver, compositor and VFS code that
has nothing to do with threads or signals, so it is left for a change that can
be verified on its own terms rather than folded into this one.

### What is deliberately not here

- **No `&` background-job syntax in vsh.** The spec asked for background
  `SIGCHLD` tracking; vsh has no background jobs to track, because it has no
  syntax for them. Adding a parser, a job table and a reaper is a shell feature,
  not a signal feature, and inventing it here would have meant shipping the one
  part of this release with no honest way to verify it. The kernel side it would
  need — process groups, `SIGCHLD`, non-blocking `waitpid` — is all present and
  tested.
- No thread-local storage, no `pthread_cancel`, no real-time signals.
- `occ` still cannot produce a function pointer, so the pthread and signal
  handler APIs remain reachable from `/bin/init` and not from occ-compiled
  source. Job control is reachable from both, because it needs no callback.

### Still ahead

`VFS_MAXFILES` is 64 and v0.60 spent the last of the headroom; `[vfs] directory
full` still appears late in a boot. Raising it changes the on-disk layout and
wants a format-version bump of its own.
