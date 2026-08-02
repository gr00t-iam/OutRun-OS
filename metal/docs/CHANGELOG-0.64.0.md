# OutRun OS v0.64.0-metal — waiting for many things at once

Milestone 64. `epoll` and `eventfd`: syscalls 76–79, a thread that sleeps until
one of several descriptors is ready, and a console that can be one of them.

Phase 1 built the surface. Phase 2 made it true, and in doing so found three
defects that had nothing to do with epoll and one that was epoll's own.

## READINESS IS COMPUTED, NOT REMEMBERED

There is no ready-list. `epoll_wait` asks each watched descriptor what it is
ready for, right now, through the same predicates `read()` and `write()`
consult — `ep_poll_fd_locked` is thirty lines and reads the pipe counters and
the eventfd counter directly.

A cached ready-list is the usual design and it has one failure mode this
system cannot afford: the cache and the object disagree, and the caller is told
about an event that is no longer true (or never hears about one that is). Every
notify path would have to be complete for the cache to be right. Computing the
answer at the moment it is asked makes that class of bug unrepresentable.

The notify paths still exist, but they carry no information — they only say
"something changed on this fd, whoever is asleep should look again". Getting one
wrong costs a wakeup, not a wrong answer.

The pipe rules are the ones with content, and they are the ones v0.59 already
had to get right for `read()`: data means readable, **and so does end of file**.
A reader must be woken to *learn* there is nothing more coming, or a pipeline
hangs on its last byte instead of finishing.

## SYSCALLS

| # | call | notes |
| --- | --- | --- |
| 76 | `SYS_EPOLL_CREATE(flags)` | no flags defined yet; returns an ordinary descriptor |
| 77 | `SYS_EPOLL_CTL(epfd, op, fd_and_events, cookie_and_mask)` | ADD / MOD / DEL. The cookie is returned verbatim and never interpreted. |
| 78 | `SYS_EPOLL_WAIT(epfd, events, maxevents_and_timeout)` | POSIX return: a count, or 0 for "nothing ready" |
| 79 | `SYS_EVENTFD(initval, flags)` | flag bit 0 = `EFD_SEMAPHORE` |

An eventfd is read and written through the ordinary `SYS_READ` /
`SYS_WRITE_FILE`, not through calls of its own. That is the point of making it a
descriptor: anything that already knows how to write to an fd can signal one,
including code that has never heard of eventfd.

`EPOLL_TTY_FD` (-2) watches the console. It is readable exactly when a keystroke
is queued — the same `kbd_w != kbd_r` test `SYS_TTY_READ` consults, asked of the
same ring, so it cannot disagree with what a following read would find.

Syscalls 1–75 are untouched and the pre-compiled VFS binaries (`occ`, `vsh`,
`omake`, `emit`, `wcx`) run unmodified.

## THE ONE THAT WAS NOT ABOUT EPOLL

### A thread could not use its own process's file descriptors

`ofile.owner_mask` is indexed by kproc slot. That was exactly right in v0.59,
when a slot **was** a process. v0.61 made a thread its own slot and the
descriptor paths were never told: they kept asking whether the *calling* slot
held the fd, and a thread's bit is never set in anything.

**So every `read`, `write` and `close` a thread attempted returned EBADF.**

Nothing caught it for three releases because no suite had ever had a thread
touch a descriptor — `pthreads_smp` shares memory, not files. `epollstrs` is the
first, and the symptom was almost perfectly disguised: the poster thread's write
to the eventfd was silently refused, so the waiter it was meant to wake slept
until its deadline, and the failure read as "the wake path does not work".

The fix is one helper, `fd_owner()`, resolving to the thread-group leader, used
at every site that asks "does the caller hold this fd" — `ofile_deref`, both
pipe paths, `open`, `pipe`, `close`, and fork's inheritance loop. It is what
POSIX requires anyway: the descriptor table belongs to the process.

Descriptor **teardown** deliberately does not use it and stays per-slot. A
thread that exits must give back only its own claims; giving back the leader's
would close the whole process's files on the first thread to finish.

## THE ONE THAT WAS

### `epoll_wait` had to be restartable, not resumable

A woken task resumes with exactly one register it did not have before: RAX.
That is enough for `futex_wait` and `thread_join`, whose whole answer *is* that
value. It is not enough for `epoll_wait`, whose answer is a count plus an array
that must be filled in from the watch list, in the caller's address space, by
the caller's own thread. The waker cannot do it — wrong core, wrong address
space, holding locks ranked below the ones it would need.

Phase 1 resolved this by returning `-EAGAIN` and documenting that callers must
loop. That contract is invisible in the signature: code that reads like POSIX
behaves like POSIX right up until something is woken. The suite failed on it
immediately.

So a parked `epoll_wait` now **re-executes its syscall** instead of returning
from it. `block_ring3_restart` is `block_ring3` with the resume RIP rewound two
bytes onto the `syscall` instruction and RAX reloaded with the call number.
RCX and R11 are architecturally clobbered by `syscall`, so no caller can be
relying on them, and the argument registers are still in the frame.

Two things make it safe rather than clever:

- **The rewind is verified, not assumed.** The two bytes before the saved RIP
  must be `0f 05`. Resuming ring 3 in the middle of an instruction is not a
  failure anyone would diagnose quickly, and a caller that did not arrive
  through a `syscall` falls back to a bounded yield — a slower answer, never a
  wrong one.
- **The deadline is absolute and lives in the kproc** (`ep_deadline`), not in
  the syscall frame, which is gone by the time the restart runs. Recomputing it
  on each entry would give a spuriously-woken waiter a fresh timeout every time,
  and a stream of wakes would postpone it indefinitely — M61 invariant 4's
  unbounded park, reintroduced through the back door of a restart.

And one subtlety that cost a boot to find: `futex_requeue` writes the wake value
into `uctx.rax`, which for a restarting syscall is **the register holding the
call number**. Syscall 78 resumed as syscall 0xFFFFFFF5 and dispatched to
nothing. `wait_restart` marks the parks whose RAX is not a return value.

## TWO MORE, BOTH FOUND BY THE SUITE

### A watch outlived its descriptor

`ep_poll_fd` answers `EPOLLERR` for an unused slot, and `EPOLLERR` is reported
whether or not it was asked for. So a watch left behind by a `close` fired on
every subsequent wait, forever, for a descriptor that no longer existed — the
suite closed a watched pipe end and the next wait returned two events where one
was expected. Linux removes the watch on close for exactly this reason, and now
so does `ofile_drop_locked`.

It is purged on the **last** drop, matching when the fd number itself becomes
free to reissue: an fd still held by a forked sibling is still a real
descriptor and its watch is still meaningful.

### Losing the last writer woke nobody

Every other readiness transition has a write behind it, and the write notifies.
End of file has nothing behind it — the *absence* of a writer is what makes the
read end ready, so no notify path ran and a parked reader sat until its deadline
expired to be told something that had already been true for seconds. Not a hang,
because no park here is unbounded, but a bounded wait for something already true
is still the wrong answer. `ep_collect_eof_locked` is shared by all three close
paths: `SYS_CLOSE`, exit-time descriptor teardown (a dying pipeline stage still
closes its write end), and the IPC path that rejects a transferred descriptor.

## THE SMP AUDIT

Every mutation of a watch list happens under `g_ofile_lock`; that was already
true and is now the stated invariant. Two readers were not, and both are fixed
the same way — **identify under the lock, notify outside it**:

- `ep_notify_fd` scanned the instances lock-free and could read a half-installed
  entry: a watch whose `used` is set before its `fd` is.
- `pipe_write_fd` scanned the descriptor table lock-free to find the read ends
  of the pipe it had just written, which races a concurrent close and can hand
  back an fd that has since been reissued as something else.

Both halves are load-bearing and they pull in opposite directions. Notifying
while still holding the lock would be worse than the race: waking takes
run-queue locks, and the lock ranking puts those strictly outside the descriptor
lock, never nested inside it. Collecting first satisfies both.

`epollstrs` asserts `g_rank_violations` did not move across the suite, so this
is checked rather than argued.

## VERIFICATION

39 suites, 0 FAIL on uniprocessor/BIOS, SMP-4/BIOS, and q35 + VT-d IOMMU
(`-smp 4`). Each configuration differs from its v0.63 baseline by exactly one
line — `[epollstrs]`. Boot logs are in `docs/`.

The suite is built so that the interesting failures produce a *wrong answer*
rather than a slow one:

- **An unposted eventfd must not be reported ready**, and a deleted watch must
  report nothing. Both fail an implementation that returns "ready" by default.
- **Writes accumulate** (7 + 5 must read back as 12, once) and drain to EAGAIN.
- **Edge and level are tested as different things.** One `EPOLLET` arrival is
  reported once; the same descriptor switched back to level-triggered reports
  again while still ready. A level-triggered implementation passes the first
  assertion and fails the second, which is the distinction being checked.
- **The park is the headline.** A waiter that spun would satisfy every assertion
  the driver makes about itself and still be wrong; only the kernel can see that
  it left the run queue, so the kernel half checks `g_epoll_parks` moved *and*
  that `g_futex_timeouts` did not — woken by the other thread's write, not by
  its own deadline. Phase 1 reported 0 parks, which is what started all of this.
- **The console round is split across the ring boundary** on purpose: ring 3
  asserts an idle console is not readable, and the kernel half injects a
  keystroke and re-checks, because that is the half the process cannot do for
  itself.

### Warnings

46, all pre-existing; this release adds none.

### Not done

- **`SYS_SETREDIR` is still per-slot.** A thread inherits its leader's
  redirections when it is created, so a thread's `write` to stdout goes to the
  right place; but a thread calling `SYS_SETREDIR` changes only its own. Making
  it process-wide is the same argument as `fd_owner()` and should follow it —
  it is left out here because nothing calls it from a thread, and changing it
  would alter `vsh`'s behaviour on a path this release has no test for.
- No `EPOLLONESHOT`, no `epoll_pwait`, no nesting one epoll inside another (a
  watched epoll fd reports never-ready rather than pretending).
- `MAX_EPOLL` is 4 and `EPOLL_MAXWATCH` is 8. Both are array bounds, not design
  limits; the scan is linear and cheap at that size and would want an index at a
  larger one.
- `VFS_MAXFILES` is still 64 and still nearly spent — unchanged since v0.60, and
  still wants a format-version bump of its own.
