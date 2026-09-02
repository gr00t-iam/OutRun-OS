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

### CORRECTION — the audit's own classification was wrong

The table above says "71 of 72 contended waits are on READ-ONLY critical
sections". **That was measured correctly and classified wrongly**, and the error
is worth naming because it is the same shape this cycle keeps finding: a return
address tells you where the acquire happened, not what the critical section goes
on to do, and the classification was made from the statement at the address.

Read against the whole section instead:

| caller | hits | the section actually | convertible |
|---|---|---|---|
| `vfs_open_for:9866` | 25 | `vfs_find`, then **CREATES a dirent** under the same acquire | **NO** |
| `SYS_WRITE_FILE:16939` | 21 | `vfs_permit` — `const struct dirent *`, pure | yes |
| `vfs_read_range:8389` | 20 | chunk walk + one `uint32_t` relatime store | yes |
| `vfs_len_of` | 5 | reads one field | yes |
| `vfs_write_by_dirent` | 1 | mutates | **NO** |

`vfs_open_for` is the largest hot spot and it is the one that cannot move: the
code says why in a v0.56 comment — *"The new dirent is claimed under the same
lock that found it missing, so two cores opening the same new path cannot both
claim a slot."* Shared mode there is a race that lets two cores create one name.

So the convertible share was **46 of 72 (64%)**, not 99%. The strategy survives
the correction — 64% is still most of the contention — but the ceiling was
overstated and the corrected figure is what the result below should be judged
against.

### IMPLEMENTED — shared acquisition, and what it bought

`struct klock` gains `rdrs` (contexts inside a shared acquire) and `rcon`
(shared acquires that had to wait). `v` keeps its exact meaning, so all 13 locks
that never take a reader behave as before and pay one predictable load.

**The rank slot could not be extended; it had to move.** `rank_st`/`rank_spp`/
`rank_idx` live *in the lock*, which is sound only because a klock is held by
exactly one context at a time — precisely the assumption shared acquisition
breaks. Two readers would overwrite each other's slot and then pop entries
belonging to someone else, which is the "push and pop went to different stacks"
fault the v0.75 notes already describe, reintroduced one layer up. Readers
therefore carry the slot in a **token on their own stack**; `klock_read_release`
pops from the token. Nothing is shared, so nothing can be clobbered, and two
readers pushing rank 2 onto *different* context stacks is why this raises no
false `g_rank_violations`.

Interlock: the writer takes `v` then drains `rdrs`; the reader waits for `v`,
increments `rdrs`, then **re-checks `v`** and backs out if a writer slipped in.
Neither can be starved into deadlock and both cannot be inside at once.

**Before (`3dcb309`) → after, same instrument, same tiers:**

| tier | acq | contended before | contended after | change |
|---|---|---|---|---|
| uniprocessor | ~210 | 0 | **0** | — (negative control) |
| smp2-bios | ~211 | 36 | **12** | **−67%** |
| smp4-bios | ~210 | 69 | **23** | **−67%** |
| smp8-bios | ~212 | 66 | **29** | **−56%** |

Measured reduction lands on the corrected 64% prediction rather than the
overstated one, which is the check that the corrected classification was right.

**What remains is exclusion that has to exist.** The contended callers after the
change are only two, both resolved with `addr2line -i`:

| caller | why it must stay exclusive |
|---|---|
| `vfs_open_for:9993` | lookup-then-create; sharing it races two creators |
| `vfs_write_by_dirent:8187` | a mutation |

Every reader-on-reader wait is gone. `shared-waits` (7/33/32 across the tiers) is
readers waiting on a **writer**, which is correct exclusion, not queueing.

45/45 suites at every tier, `g_rank_violations == 0`, zero rank mismatches, and
`cmd_cio`'s quiescence check now also fails a lock left held in shared mode —
which would otherwise have passed invisibly.

## MILESTONE 3 — THE OPTIMISTIC PROBE, AND WHAT `g_ofile_lock` IS ACTUALLY DOING

### `vfs_open_for`: exclusive contention to zero

Milestone 2 left this function as **all** of the remaining contention (23 of 23
on smp4) because its lookup is followed by dirent creation and sharing that
races two creators for one name.

Split by **outcome**, not by call site. An open that FINDS its file and is not
truncating mutates nothing — `vfs_find` scans, `vfs_permit` reads, and the fd
claim happens after the lock is dropped — so that case takes the lock shared.
Everything else (name missing, so possibly creating; or `O_TRUNC`, which empties
the file) takes it exclusively, exactly as before.

**The second `vfs_find` is the whole correctness argument.** Between dropping the
shared lock and taking the exclusive one another core may have created this very
name, so the probe's answer is stale by then: the slow path re-resolves under
exclusion and uses only that result. The probe is a hint, never a decision.

| tier | M2 contended | M3 contended |
|---|---|---|
| uniprocessor | 0 | **0** |
| smp2-bios | 12 | **0** |
| smp4-bios | 23 | **0** |
| smp8-bios | 29 | **0** |

`vfs-wait` reports **no contended acquisition at all** on every tier. Across the
three milestones: 36/69/66 → 12/23/29 → 0/0/0. `shared-waits` (19/53/62) is
readers waiting on a genuine writer, which is the exclusion working.

45/45 suites on all four tiers, `g_rank_violations == 0`, zero rank mismatches.

### `g_ofile_lock`: the 45% is the hammer measuring itself

**The first attempt at this audit measured the wrong window, and that is the
finding worth recording.** The attribution instrument was attached to
`g_ofile_lock` and dumped from `cmd_cio` — which runs at kernel64.c:33070, while
`cmd_vfs_stress` runs at :33077. So the table reported 10 waits from early boot
while the role-62 phase that produced the 45% had not run yet, 1,300 log lines
later. Reporting those 10 as "the sites responsible" would have been a number
measured correctly over the wrong window — the third time this cycle has produced
that exact shape. The dump is now also called from the role-62 phase, after its
own workload.

Measured there, `-smp 8`: **73,745 acquisitions, 33,047 contended (44%)**, eleven
distinct callers, no overflow. The top four are 99.9% of it:

| caller | site | hits | share |
|---|---|---|---|
| `SYS_CLOSE` (case 8) | `fd_owner` → `cpu_idx`, :17231 | 13,748 | 42% |
| `ofile_claim:9691` | the 16-slot `for (fd = 0; fd < 16; fd++)` scan | 8,675 | 26% |
| `pipe_create_for:9909` | the `MAX_PIPES` free-slot scan | 6,826 | 21% |
| `pipe_create_for:9928` | wiring the two ends | 3,829 | 12% |

Every one of them is `pipe()` or `close()` — which is **exactly and only what
role 62 does**. Set against ordinary suite traffic on the same boots:

| tier | ordinary traffic (`cmd_cio`) | role-62 hammer |
|---|---|---|
| uniprocessor | 0 / 1,128 (0%) | — |
| smp2-bios | 4 / 1,110 (0.4%) | — |
| smp4-bios | 9 / 1,080 (0.8%) | 16% |
| smp8-bios | 13 / 1,133 (1.1%) | 44% |

**So the 45% is not evidence that `g_ofile_lock` is a bottleneck.** It is a
purpose-built pipe/close hammer contending with itself; the same lock under the
rest of the boot's forty-odd suites sits near 1%. The honest reading of
Milestone 1's number is "this is what the lock does when you attack it", not
"this is what the lock costs" — and the v0.90 goal text that called it a
candidate for optimisation was written before this distinction existed.

That makes descriptor-layer optimisation **unjustified on current evidence**, and
recording that is the point: the measurement was taken to find out, and the
answer came back no. The three sites above are still where any future work would
go — the two linear scans especially — but a scan over 16 slots is not obviously
worth replacing to serve a workload nothing but a stress test generates.

## THE DIRTY-VOLUME GATE — and a harness that could report green on nothing

Every v0.90 figure before this point came from a **fresh** image per boot. The
lock work touches `vfs_open_for`'s create path and the whole read path, and a
reused volume is where a stale dirent or a double-create shows: the v0.84 O_TRUNC
map bug was found by this tier and hidden completely by fresh images.

Image `99c42fe78cd108972b52762b5027d459`, three boots per tier on ONE image:

| tier | boots | suites | failing | boot-to-boot diffs | cross-boot artefacts |
|---|---|---|---|---|---|
| uniprocessor | 3 | 45 | 0 | empty | `udbreboot` + `vfs-reboot-test` + CAS replay |
| `-smp 2` | 3 | 45 | 0 | empty | all present |
| `-smp 4` | 3 | 45 | 0 | empty | all present |
| `-smp 8` | 3 | 45 | 0 | empty | all present |

Empty consecutive-boot assertion diffs at every width, `cas-pending=1` on boot 1
becoming `cas-recovered=1` on boots 2 and 3. **No stale dirent, no double
allocation, no corruption across open/truncate on a populated volume**, with
shared acquisition and the optimistic probe live. `vfs_open_for`'s create path
runs on boot 1 and its lookup path on the reboots, against state that already
exists — which is the case the fresh tiers structurally cannot produce.

`-smp 8` needed `GATE_DIRTY_CAP=1500`; at the 480 s default its boot never
reached the prompt and the gate reported the resulting nothing. That is the same
"raise the cap and re-run rather than reading the assertions it printed" case
CLAUDE.md already records for `smp4-iommu`.

### The harness bug this tier surfaced

The first UP run reported `vfs-reboot-test=0`, said in its own output that the
CAS cross-reboot probe's **five assertions tested nothing** on boots 2 and 3 —
and **exited 0**.

`gate-dirty.sh` typed its two console commands with a fixed `sleep 25` between
them, commented "one 4096-round KDF, then the save". On a host loaded enough for
the KDF to run long, `cascrashwrite` was typed while the shell was still busy and
was **dropped** — the command never echoed at all. Reproduced with an unrelated
QEMU sharing the host.

That is a budget, not a deadline, and CLAUDE.md is explicit that a timing budget
in this tree must be a deadline: the same 25 seconds is a different amount of
work on a quiet host and a loaded one. It now waits for `udbpersist`'s own
completion marker, capped at 180 s. Every tier above produced its artefacts after
the fix; none did reliably before it.

**The kernel was the prime suspect and was cleared by evidence, not assumption.**
`cascrashwrite` creates its file through `vfs_open_for` — the function this
milestone rewrote — so it was the obvious cause. It is not: the command text
never appeared in the log at all, and a broken `vfs_open_for` would have run the
command and failed rather than made it vanish. `udbpersist`, typed first, worked
on the same boot.

## THE SOAK — and the append assertion it did NOT convict this change of

`tools/vfs-soak.sh` repeats `cmd_vfs_stress` on one boot, judging each iteration
by its own `RESULT` line so a hung iteration is a timeout rather than a silent
pass — a reader-starvation deadlock would surface as exactly that.

`-smp 8`, image `99c42fe78cd108972b52762b5027d459`: **3 of 3 iterations
completed, zero rank faults, zero rank mismatches, zero panics, no stall.** The
shared-acquisition interlock does not deadlock and does not starve.

It also reported the `append-oversub` preemption assertion failing on three of
four runs — **including the boot's own first run**, before any repetition:

```
FAIL append-oversub: the file is EXACTLY workers x iterations x payload bytes
     under PREEMPTION (a writer stopped mid-syscall must not lose its append)
```

**A mechanism pointed straight at this milestone.** The append path resolves
end-of-file through `vfs_len_of`, which v0.90 converted to shared acquisition.
Two appenders reading one length concurrently would both write at the same offset
and one would lose its append — precisely what this assertion checks. The race
pre-exists (the length read and the write were already separate critical
sections), but shared access lets readers overlap where they were serialised,
which could plausibly raise its rate.

**Measured instead of argued.** Control: `3dcb309` — the attribution instrument
present, `klock_read_acquire` call sites **zero**, exclusive locks throughout —
same soak, same tier, same host.

| build | shared acquisition | runs | failures |
|---|---|---|---|
| `3dcb309` (control) | none | 4 | **4 of 4** |
| `92c7e87` (this work) | 7 sites | 4 | **3 of 4** |

The failure is *more* frequent on the build that does not contain the code, which
no defect introduced by that code can produce. **The change is cleared.**

The real variable is the host. These boots reach the prompt in **707–718 s**
against **330 s** for the same tier in the fresh matrix earlier the same day, on
an idle machine — the toolchain host was updated between those runs. A preemption
assertion is exactly what degrades first when the machine slows, and this tree
has cleared a change this way before (v0.89's `smp4-iommu`, and two "fixes"
disproven in earlier cycles).

**Recorded as an open flake, not as a v0.90 defect and not as a pass.** The
assertion is real and the append race it guards is real; what is not established
is that anything in this milestone made it worse. It should be re-measured on a
healthy host before anyone acts on it.

### STILL OPEN

Both items this section carried after Milestone 2 are closed above:
`vfs_open_for` now probes shared and re-checks under exclusion (contention 0 on
every tier), and `g_ofile_lock` has been audited (its 45% is the role-62 hammer
contending with itself; ordinary traffic is ~1%). What remains:

- **The `append-oversub` preemption assertion flakes on a degraded host**, in
  this build and in a pre-v0.90 control alike (4/4 against 3/4 — see the soak
  section). It needs re-measuring on a healthy machine, and if it persists there
  it is its own investigation: `vfs_write_append` resolves end-of-file and writes
  in two separate critical sections, which is a real race independent of this
  milestone.
- **The soak ran 3 iterations, not 100.** The harness is verified and the full
  run is a matter of wall clock (~700 s to the prompt, then ~400 s per
  iteration at `-smp 8` on this host), but a 100-iteration figure on a machine
  that already fails a timing assertion at iteration 1 would measure the host,
  not the kernel.
- **The probe widened a window that already existed.** `vfs_open_for` has always
  released the lock before `ofile_claim`; it now also drops and retakes it
  between the lookup and the create. The re-check makes the create safe, but any
  future code added between probe and slow path must not assume the probe's
  answer still holds.
- **No reader/writer support for `g_ofile_lock`.** Deliberately: the audit says
  it is not a bottleneck under real traffic, so adding shared acquisition there
  would be optimising a number no workload produces.

This milestone met the bar its own goal text set:

> No decoupling lands without a before/after contention number from the same
> instrument. A refactor whose only evidence is that the suites still pass has not
> been shown to do anything.

The before came from `3dcb309`, the after from this commit, both from the same
per-caller instrument on the same four tiers.

## OPEN, CARRIED FROM v0.89

- **The role-61 soak is named for a lock it cannot contend.** It remains a real
  concurrency test — concurrent put/free, journal transactions, positional writes
  from every core, with exact accounting recovery — but "CAS contention"
  describes `cas-direct`, not it. Renaming it, or giving it a VFS-lock name that
  matches what it stresses, is unresolved.
- **Role 62 was the last free role number** before this cycle; the next addition
  needs a fresh check of both `\.role = ` in the kernel and `role == ` in
  `init.c`, which can and have disagreed.
