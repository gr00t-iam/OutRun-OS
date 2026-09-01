# OutRun OS v0.90.0-metal — descriptor and VFS lock decoupling

Status: **development cycle**. Nothing here is tagged. `VERSION` reads
`0.90.0-dev` and the ISO is named `outrun-os-0.90.0-dev.iso` for exactly that
reason — a development image should not be mistakable for a release artefact by
its filename.

## THE ARTEFACT THIS CYCLE STARTS FROM

v0.89.0 at `8bb0c8c`, tagged and published.

| | |
|---|---|
| artefact | `outrun-os-0.89.0.iso` |
| md5 | `e8a18e15be3270b496a9e677e3b4bda3` |
| sha256 | `68cb4c9a330f7b9fb874eff0c630cada11292ae7360bdab483524c3b7d485760` |
| `release-verify` | **PASS** — 45 suites reporting, 0 failing assertions, 0 rank faults, 310 s |

The tag points at the `VERSION` bump commit rather than at `bd5e4d0`, the last
code commit. That is the protocol working rather than a slip: `VERSION` names the
artefact, so it has to be right *before* the tag exists. Tagging `bd5e4d0` would
have published `outrun-os-0.89.0-dev.iso` under a `v0.89.0` tag, which is the
v0.75.0 failure the protocol was written to make impossible.

**Fresh-tier coverage on the pre-bump tree** (`99dddd8196619ca7d8d135ed7e59ca0e`,
identical to the release but for the two version strings): uniprocessor 557,
smp2-bios 571, smp4-bios 577, smp8-bios 577, all at 0 failed / 0 ranks.
`release-verify` covered uniprocessor on the published artefact itself.

**Not covered, and said so rather than left to be assumed:** `gate-dirty` and
`gate-dirty-smp` were not re-run against the published artefact, and no tier
above uniprocessor was run on the artefact *as published*. The evidence for those
is the pre-bump image.

## WHAT v0.89 LEAVES ON THE TABLE

v0.89 set out to measure `g_cas_lock` contention and found there was none to
measure: `g_vfs_lock` (rank 2) is held across the whole write path and `cas_put`
is reachable only from inside it, so the rank-3 lock cannot be contended through
the VFS at all. Measured 0 of 1,728 acquisitions on `-smp 8`; corroborated by the
kernel's own per-lock counters at `vfs` 75/209 contended against `cas` 0/4,539.

A kernel-side `cas-direct` burst that takes the VFS lock out of the path then
contended it 0% → 6% → 47% → 71% across 1/2/4/8 cores, which is what closed the
question. **The lock that is actually hot in this kernel is `g_vfs_lock`**, and
that is the thread v0.90 picks up.

## GOALS

### 1. Fine-grained VFS lock decoupling

`g_vfs_lock` is one rank-2 lock held across an entire file write — dirent
allocation, the chunk loop, every `cas_put` under it, and the map rebuild. It is
measurably the contended lock in this tree (36% of acquisitions on `-smp 4`),
and it is contended because it is *coarse*, not because the work underneath is
serial: the CAS layer below it has its own rank-3 lock that is provably never
contended.

The question to answer before changing anything: **which parts of the path
actually need the directory lock held, and which are holding it only because the
whole function does?** The v0.89 telemetry (`struct cas_spin_stat`, per-CPU,
non-atomic) is the instrument; the same shape applied to `g_ofile_lock` and
`g_vfs_lock` is the measurement this goal depends on.

No decoupling lands without a before/after contention number from the same
instrument on the same tiers. A refactor whose only evidence is that the suites
still pass has not been shown to do anything.

### 2. File descriptor table

`g_ofiles` is **16 slots, global, shared system-wide** — the fd number *is* the
index, and `owner_mask` is a bitmask of the kprocs that hold it. There is no
per-process descriptor array.

Two things follow, and both are cycle goals rather than assumptions:

- **The table's size is a real limit, not a tuning parameter.** With a pipe
  costing two slots, a handful of concurrent pipes exhausts it system-wide.
  Whether 16 is right is an open question; raising it touches every descriptor
  path (open/close/pipe/epoll/eventfd/socket, fork aliasing, exec force-close)
  and `owner_mask` being a `uint64_t` caps kprocs at 64 independently.
- **`g_ofile_lock` (rank 1) is the lock every descriptor operation takes**, and
  its contention has never been measured. Role 62 below exists to produce that
  number before anyone optimises against a guess.

**There is no `SYS_DUP`.** The kernel has no dup/dup2 at any syscall number
(the namespace currently ends at 103). Adding one is descriptor-layer feature
work — aliasing a slot, `owner_mask` semantics, independent close of each alias,
fork/exec inheritance — and is listed here as a candidate, not as done.

### 3. Ring-3 pipe throughput

Pipes are the one descriptor object with a data path behind them, and their
throughput under SMP has never been measured — only their correctness. Wanted:
bytes/tick through a pipe with reader and writer on different cores, and how
that degrades as descriptor pressure rises.

## MILESTONE 1 — ROLE 62, THE DESCRIPTOR CONTENTION WORKER

Ring-3 role 62, `role62_fd_stress`. It exists to make `g_ofile_lock` contention
observable, which nothing in the tree currently does.

**Deliberately bounded, and the bound is the design.** Each worker holds at most
two descriptors at a time and closes them immediately:

```
per iteration:
  open("r62-<pid>") -> close          1 slot,  held briefly
  pipe(fds)         -> close both     2 slots, held briefly
```

A worker that grabbed descriptors in bulk would exhaust a 16-slot global table
within one iteration on any multi-core tier, and would do so *by design of the
table* rather than by any defect — while also starving the 44 other suites that
share it. **Table-full is therefore an expected outcome with its own exit code,
counted and tolerated, never decoded as a failure.** That is the same rule this
tree already applies to deadline expiry: a resource limit must not be reportable
as a defect.

What it asserts, on the kernel side after the drain:

- every `g_ofiles[]` slot owned by a role-62 pid is released — `owner_mask` clear
- the used-slot count returns **exactly** to its pre-phase value, not "no worse"
- `g_ofile_lock` was genuinely contended — a phase that never overlapped has
  measured nothing, and would be green having tested nothing

The last one is the premise guard. v0.89 spent most of its length on two
instruments that reported confidently about work that had not happened; a
descriptor stress that never contends is the same failure one layer over.

## OPEN, CARRIED FROM v0.89

- **The role-61 soak is named for a lock it cannot contend.** It remains a real
  concurrency test — concurrent put/free, journal transactions, positional writes
  from every core, with exact accounting recovery — but "CAS contention"
  describes `cas-direct`, not it. Renaming it, or giving it a VFS-lock name that
  matches what it stresses, is unresolved.
- **Role 62 was the last free role number** before this cycle; the next addition
  needs a fresh check of both `\.role = ` in the kernel and `role == ` in
  `init.c`, which can and have disagreed.
