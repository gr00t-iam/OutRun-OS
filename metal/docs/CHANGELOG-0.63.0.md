# OutRun OS v0.63.0-metal — memory that arrives when you touch it

Milestone 63. `mmap`/`munmap`/`mprotect`, demand-zero paging, copy-on-write
fork, and a general shared-memory registry. Until now every page this system
mapped was allocated the instant it was named; now a page can be *promised*.

## THE PAGE TABLE IS THE MAP

Two page kinds need to be remembered — "anonymous, not yet backed" and
"writable, but the frame is shared" — and the usual way to remember them is a
VMA list. This kernel has never had one, and adding it would create a second
description of the address space that must be kept consistent with the first.
That inconsistency is exactly how an unmap frees a frame the other structure
still believes is live.

x86-64 ignores PTE bits 9–11, and ignores *every* bit of a non-present entry.
That is enough to record what a page is inside the entry itself:

| bit | meaning |
| --- | --- |
| `PTE_ZFOD` (9) | PRESENT=0. Anonymous, unbacked. **The intended permissions ride in the same word** — free, because the hardware is not looking. |
| `PTE_COW` (10) | PRESENT=1, WRITE=0. Writable, but the frame is shared; a write must copy first. |
| `PTE_SHM` (11) | A shared-memory frame: never copy, never demand-zero. |

The PTE was already authoritative. This keeps it that way.

## FRAME REFERENCE COUNTS, AND WHY ZERO MEANS ONE

COW and shared memory both need a frame to outlive the first owner that lets go
of it. `g_frame_ref[]` sits beside the existing double-free shadow array, and
**0 means sole owner, not no owners** — so the default-initialised array is
already correct for every frame that existed before this release, and no
existing path had to learn about refcounts to stay right.

`free_frame` decrements while extra owners remain and returns 0, "not reclaimed
by me". The invariant every suite asserts —
`g_frame_free_depth == g_frames_freed - g_frames_reused` — stays exactly true,
because the counters still only move when a frame really goes on or off the
list.

The v0.45 double-free detector is what makes this safe to get *wrong*: a
refcounting bug halts the machine at the second free with the offending
address, rather than handing live memory to a second owner.

## TWO BUGS THIS FOUND, BOTH WORTH THE SPACE

### 1. The kernel does not fault, so the kernel corrupts

Ring 3 never notices demand paging: the CPU faults and the handler fixes it up.
The kernel gets no such courtesy. It writes to user memory through the process's
own mapping while in ring 0 — and **CR0.WP is not set**, so a ring-0 store to a
read-only COW page does not fault. It succeeds, silently, straight into a frame
another process is still reading.

What surfaced first was the *opposite* failure, and it was loud: `access_ok`
saw a COW page as not-writable and refused, so `sig_deliver` could not build a
signal frame on a forked process's stack and killed it with "no stack".
**Thirty-two assertions failed at once**, across every fork-heavy suite, all
with exit 145 — `128 + SIGCHLD`.

Both are fixed in one place, because `access_ok` is the single gate every kernel
path already passes through before touching user memory. It now *resolves* the
page — backing a ZFOD promise, taking a private copy of a COW page — so by the
time any caller has permission it also has a frame it may genuinely write. The
refusal was a symptom; the silent write would not have been.

### 2. Forking twice

`vsh` forks once per pipeline stage. `emit | wcx` killed its second stage while
a single-stage command worked perfectly, and the PTE said why:

```
[fault] ring-3 pid 662 fault (vec 14) cr2=0000570000001000 rip=0000500000002f9a
        err=7 pte=8000000001580005 — task terminated
```

Present, user, **not writable, and no COW bit**; `err=7` is a write to it.

The first fork leaves a page read-only-plus-COW. The second fork then sees no
`PTE_WRITE`, mistakes it for read-only text, and shares it with the COW mark
**stripped**. The page is permanently unwritable by anyone, and the fault
handler declines it because nothing left says it may be copied.

The condition is `(pte & PTE_WRITE) || (pte & PTE_COW)`, and that second clause
is load-bearing rather than defensive.

This is also why the ring-3 fault message now prints the error code and the
PTE. Now that a page can be lazily backed, "never mapped" (0), "promised but
unbacked" (ZFOD) and "present but protected" are three different bugs that look
identical from outside.

## SYSCALLS

| # | call | notes |
| --- | --- | --- |
| 71 | `SYS_MMAP(len, prot, flags)` | **anonymous only** — there is no file-backed paging here, and the kernel refuses rather than returning zeroes for a file. W^X enforced at the call. |
| 72 | `SYS_MUNMAP(addr, len)` | frees what is actually backed, leaves untouched reservations costing nothing, shoots the range down cross-core. |
| 73 | `SYS_MPROTECT(addr, len, prot)` | rewrites live PTEs **and** unfaulted reservations — otherwise the first touch would install the old permissions. |
| 74 | `SYS_SHM_CREATE(size)` | frames allocated and zeroed **up front**: deferring would have two processes faulting independently on one page and racing to install different frames. |
| 75 | `SYS_SHM_MAP(id, writable)` | per-process address, `PTE_SHM` so neither COW nor ZFOD touches it. |

Syscalls 1–70 are untouched, and the pre-compiled VFS binaries (`occ`, `vsh`,
`omake`, `emit`, `wcx`) run unmodified.

`fork` no longer copies. Pages are shared and both sides lose write permission;
the parent's entry is rewritten too, which is the half that is easy to omit —
leave the parent writable and it writes through to a frame the child can still
see. `MAP_SHARED` pages are deliberately exempt: copying them is precisely the
behaviour sharing exists to avoid.

Shared memory here is the general form the v0.46 `ipc_shmem` deliberately was
not — an object with its own lifetime, created by size, named by an integer any
process may map. The v0.46 one is IPC-coupled and stays as it is; this is not a
replacement.

Userland `malloc` routes allocations ≥128 KiB through `mmap`, with a magic
header so `free` can tell the two apart without a side table. Because mmap is
demand-zero, a large request that is only partly touched never costs the frames
it did not use.

## VERIFICATION

38 suites, 0 FAIL on uniprocessor/BIOS, SMP-4/BIOS, and q35 + VT-d IOMMU
(`-smp 4`). Boot logs are in `docs/`.

The two new suites are written so that the failure mode produces a *wrong
answer*, not a slow one:

- **`mmapstrs`** maps 1 MiB (256 pages) and touches 8. The assertion is that
  the demand-zero fault count is greater than zero **and far below the number of
  pages mapped** — an eager implementation fails it from both directions at
  once. It also checks a fresh mapping reads back as zero (a stale frame would
  not), that W^X is refused at the call, and that a write to an `mprotect`ed
  read-only page raises a *catchable* SIGSEGV the handler recovers from.
- **`shmstrs`** proves COW by writing a value before the fork and a different
  one in the child after it, then checking the parent still sees the original —
  a COW that merely shared would let the child's store through. Then two
  distinct processes exchange data through one segment by id, and the kernel
  checks no segment outlived its last mapper.

### Warnings

46, all pre-existing; this release adds none.

### Not done

- No file-backed `mmap`, no `MAP_FIXED`, no swap. There is no backing store to
  page to, and a file mapping that silently returned zeroes would be worse than
  a refusal.
- `munmap` punches holes that are not re-packed. The window is 64 MiB, far past
  anything this system allocates, and a first-fit scan would be code with no
  consumer.
- `VFS_MAXFILES` is still 64 and still nearly spent — unchanged from v0.60, and
  still wants a format-version bump of its own.
