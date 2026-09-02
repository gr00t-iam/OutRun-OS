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

## MILESTONE 2 — THE `g_vfs_lock` AUDIT

### The 35 acquisition sites

| class | sites |
|---|---|
| **read / lookup** | `vfs_read_range`, `vfs_read_file`, `vfs_len_of`, `tmp_len_of`, `tmp_read_range`, `tmp_read_file`, `SYS_FSTAT` (×2), `cmd_cio` (×3) |
| **write / mutation** | `vfs_write_file`, `vfs_write_by_dirent`, `vfs_write_at`, `vfs_write_append` (×3), `vfs_truncate`, `vfs_rename`, `vfs_unlink` (×2), `tmp_write_at`, `tmp_write_append`, `tmp_write_file`, `tmp_truncate`, `SYS_WRITE_FILE`, `SYS_VFS_SYNC`, `SYS_VFS_UNLINK`, `SYS_CHOWN`, `SYS_FTRUNCATE` |
| **open (lookup, may create)** | `vfs_open_for` (×2) |
| **suite code, not a production path** | `cmd_vfs_stress` (×2), `cmd_users_stress` |

Counting sites is the cheap half and on its own it is misleading — writes
outnumber reads nearly two to one, which would argue against read decoupling.
What matters is which sites the *waits* come from, and that is measured.

### Measured: who actually waits

`klock_acquire` now records `__builtin_return_address(0)` on the **contended path
only** for `g_vfs_lock`, into a 16-slot table with an overflow bucket, reported by
`cmd_cio` and resolved with `addr2line` on the ELF. Attribution by return address
rather than by tagging 35 call sites: there is nothing at the sites to drift out
of step. smp4-bios, 207 acquisitions, 72 contended, five distinct callers, no
overflow:

| caller | what the critical section does | hits | share |
|---|---|---|---|
| `vfs_open_for:9866` | `vfs_find(name)` — scans DENTS | 25 | 35% |
| `SYS_WRITE_FILE:16939` | `vfs_permit(&DENTS[di], …)` — reads mode/uid/gid | 21 | 29% |
| `vfs_read_range:8389` | reads the chunk list | 20 | 28% |
| `SYS_WRITE_FILE:16969` → `vfs_write_at` → `vfs_len_of` | reads `DENTS[di].len` | 5 | 7% |
| `vfs_write_by_dirent:8072` | **mutates** | 1 | 1% |

**Seventy-one of seventy-two contended waits are on READ-ONLY critical sections.
Exactly one is a mutation.**

Reproduced across the whole matrix rather than argued from one boot — the same
four callers dominate at every width, and the one mutating site appears in a
single tier at four hits:

| tier | acquisitions | contended | read-only share |
|---|---|---|---|
| uniprocessor | 214 | **0** | — (no second acquirer; the correct reading) |
| smp2-bios | 212 | 36 (17%) | 36 / 36 — **100%** |
| smp4-bios | 209 | 69 (33%) | 69 / 69 — **100%** |
| smp8-bios | 209 | 66 (32%) | 62 / 66 — **94%** |

Acquisition count is flat at ~210 across every tier while contention climbs from
0 to a third: the workload is not doing more VFS work on more cores, it is
queueing for the same lock. The uniprocessor zero is the negative control.

Two of those rows would have been classified wrongly without the inline chain.
`addr2line` without `-i` attributes `0x146cdd` to `cred_egid` and `0x14a7e3` to
`vfs_len_of`, which reads as "a credential helper and a length getter". The chain
shows both are inlined inside `case 7: SYS_WRITE_FILE` — so by *syscall* they are
writes, while by *critical section* they are reads. The second reading is the one
that governs whether reader/writer semantics can help, and the first would have
sent the decoupling at the wrong target. **Resolve with `-i` or do not resolve.**

### What this says about the strategy

Read-path decoupling is the right instinct and the measurement raises its ceiling
sharply: not the ~28% a naive read/write split predicts, but ~99% of observed
waits. The contention is overwhelmingly **readers blocking readers** — a
permission check, a name scan, a length fetch and a chunk-list walk, none of
which mutate anything, all serialised against each other by one exclusive lock.

That makes reader/writer semantics on `g_vfs_lock` the indicated change, and it
is a change to the lock rather than to the call sites.

**Not implemented in this commit, and the reason is not caution for its own
sake.** `struct klock` is exclusive by construction and carries rank tracking,
IRQ save/restore, self-deadlock detection and the `rank_st`/`rank_spp` machinery
that exists because acquire and release must agree on where the rank entry lives.
A reader/writer variant has to preserve all of it, and reader-shared acquisition
breaks the "held by exactly one context" assumption that note rests on. Shipping
that at the end of an audit and calling it verified because 45 suites still
passed would be precisely the reasoning this cycle's own goal text rules out:

> No decoupling lands without a before/after contention number from the same
> instrument. A refactor whose only evidence is that the suites still pass has not
> been shown to do anything.

The instrument to produce that before/after now exists and its baseline is the
table above. The refactor is the next piece of work, with its own verification.

## OPEN, CARRIED FROM v0.89

- **The role-61 soak is named for a lock it cannot contend.** It remains a real
  concurrency test — concurrent put/free, journal transactions, positional writes
  from every core, with exact accounting recovery — but "CAS contention"
  describes `cas-direct`, not it. Renaming it, or giving it a VFS-lock name that
  matches what it stresses, is unresolved.
- **Role 62 was the last free role number** before this cycle; the next addition
  needs a fresh check of both `\.role = ` in the kernel and `role == ` in
  `init.c`, which can and have disagreed.
