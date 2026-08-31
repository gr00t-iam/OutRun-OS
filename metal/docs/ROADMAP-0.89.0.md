# OutRun OS v0.89 — dirty-volume recovery & concurrency soak

**No v0.89 tag exists, and neither does a v0.88 one.** v0.88 was never sealed —
`VERSION` still reads `0.88.0-dev` and no release ISO was built or
`release-verify`'d for it. This work therefore lands in the open v0.88-dev
cycle regardless of the label on the objectives, and nothing here is a released
claim. Recorded so that a later reader does not infer a tag from a filename;
see `ROADMAP-0.88.0.md` for the objectives that preceded these.

---

## Three corrections to the brief, made before building anything

The objectives named machinery this tree does not have. Building to the names
would have produced tests nobody could find afterwards — the v0.88 §4 lesson,
which arrived the same way — so each was checked against the source first and
the real equivalent built instead.

### 1. There are no head/tail offsets, and no page structures

The requirement asked that remount "restore page structures, head/tail offsets,
and allocation bitmaps". Neither journal in this filesystem is a ring:

- the **VFS directory journal** is a single-slot whole-directory shadow — a
  header (`state`, `seq`, `count`, `blocks[]`) plus up to `VJ_MAX_SLOTS` shadow
  blocks
- the **CAS metadata journal** is exactly four blocks: header, superblock, the
  one bitmap block the transaction touched, and the one index block

No head, no tail, no offsets. Nothing in the CAS or VFS paths touches page
tables either. What is asserted instead is what these structures actually have:
index/bitmap agreement, `used_blocks` against the bitmap's popcount, byte-exact
block content, and generation-table synchronisation.

### 2. Recovery is not IRQ-guarded, and making it so would be a regression

The requirement asked that "transaction log recovery execute inside an
IRQ-guarded critical section". It does not, and it should not.

v0.86 **measured** that journal commits already run with interrupts enabled —
`klock_acquire()` ends with `klock_irq_restore()`, so the `cli` covers the
rank-stack check and not the critical section — and located the real guarantee
in mutual exclusion rather than in masking. The standing ruling, re-stated in
v0.88 §4, is that masking interrupts across a path the timer ISR can reach buys
a `TRUNCATED` boot and no verdict rather than safety.

The property actually wanted — no AP executing while the log is replayed — is
already true here, by **boot ordering**: `kernel_main` calls `cmd_cas()` (line
31899, where the boot mount and its recovery happen) before `smp_init()` (line
31939). No AP exists yet.

That was an argument about two calls forty lines apart in a function nobody
edits with the filesystem in mind, so it is now measured: `g_smp_started` is set
at the top of `smp_init()`, and the filesystem records how many CPUs were online
at the boot mount and at any pre-SMP replay. **See below for how the first
version of that instrument was wrong, and how the gate caught it.**

### 3. "Page-table integrity faults" do not belong to this workload

The soak contends on the CAS index and the block bitmap under a rank-3
spinlock. It touches no page tables. Page-table integrity is covered by the
mmap and COW-fork suites (roles 46 and 47), which is where such a claim belongs.
Written into the phase header rather than satisfied with an assertion that would
pass for reasons unrelated to the phase.

---

## Objective 1 — the CAS metadata journal, replayed across a real power cycle

### What was missing

v0.48 gave the **VFS directory journal** a genuine cross-reboot proof:
`vfscrashwrite` commits a write and halts without syncing, and a later, separate
QEMU process mounting the same image is what demonstrates recovery. The **CAS
metadata journal** never got one.

v0.88's crash injection does interrupt a put, but it remounts **in-process**.
That is weaker than it looks, and the weakness is specific: at that moment the
RAM bitmap, superblock and index buffer are already post-transaction, so a
recovery that wrote *nothing at all* would leave the in-memory picture correct.
Only the home blocks would be wrong, and nothing in that boot has to read them
again.

Across a power cycle nothing is in RAM. Every byte the next boot judges comes
off the disk, so a recovery that does not run is a recovery that shows.

### `cascrashwrite`, and why the order inside it is load-bearing

The new command leaves **both** journals dirty in one power cut:

1. `vfs_write_file("vfs-reboot-test", ...)` — the v0.48 fixture, unchanged
2. `cas_crash_stage()` — a real put, stopped between its home index write and
   its bitmap flush
3. `cli; hlt`

The order is not tidiness. `vfs_write_file` stores its content through
`cas_put()`, which runs a **complete** metadata transaction — journal write,
home writes, journal *clear*. Staging the CAS crash first and writing the file
second would erase the pending CAS journal with the file's own completed one,
and the next boot would find nothing to recover while every line still claimed
something had been staged.

At the halt the volume carries a PENDING vjournal and a PENDING cjournal, and
the home bitmap and superblock still say the staged block is **free**.

### What the next boot asserts

Not "the file came back" — losing an interrupted transaction is correct
behaviour. Five explicit checks:

| # | assertion |
|---|---|
| 1 | the marker's content is in the index at all |
| 2 | **the block it names is ALLOCATED.** At the halt the home bitmap said FREE; only `cas_journal_recover()` can have reconciled them |
| 3 | the block reads back all 512 bytes exactly |
| 4 | volume-wide index/bitmap agreement holds — the repair did not fix this entry by breaking another |
| 5 | `used_blocks` still equals the bitmap popcount, so the **superblock** shadow was replayed too and not only the bitmap block |

On a fresh volume the phase asserts nothing and says so, which is why the four
fresh tiers do not carry these five. `tools/gate-dirty.sh` fails the run if the
`VERIFIED` line is missing from a boot that should have had one — a skipped
probe must not read as a pass.

**Falsifier:** `-DCAS_NORECOVER_REPRO` mounts without replaying anything, which
is what the pre-v0.48 kernel did. It fails the dirty gate, as required —
`boot 2  failing-assertions=1`.

#### It broke a different check than predicted, and the reason matters

The prediction was check 2: with no replay the home bitmap still says FREE, so
the block the index names is unallocated. **Check 2 passed. Check 3 — the
byte-for-byte read-back — is what went red.**

The reason is the hazard itself, running to completion. Between the mount and
the probe, the boot's own suites do ordinary `cas_put`s. Block *b* is marked
free on disk, so `bm_alloc()` hands it out, and somebody else's content is
written over it. By the time the probe runs, *b* is allocated (check 2 is
satisfied, by the wrong owner) and the index still maps the marker's hash to it
— so the volume answers that hash with content that is not that content.

That is the v0.48 sentence exactly: *"a future put legitimately overwrites block
B, silently corrupting a different dedup'd file."* The falsifier does not merely
trip an invariant; it reproduces the corruption end to end.

**The design lesson, since it was nearly missed.** Check 2 catches the
*immediate* absence of recovery — a probe run before anything else allocates.
Check 3 catches the *consequence* once the volume has carried on working. A
suite carrying only check 2 would have gone green on this build, which is the
argument for asserting the state and the content rather than picking one.

---

## Objective 2 — CAS index and bitmap under multi-core contention

### Why the existing append soak does not cover this

The oversubscription phases (roles 56..59) put many writers on one file and
prove no byte is lost. After their first iteration they contend on almost
nothing else: every worker writes the **same** payload, so every chunk dedups
and the CAS allocates no block, frees no block and mutates no index entry for
the rest of the run.

### The new phase (role 61)

Each worker creates a file whose content is unique to (worker, iteration),
writes it twice, reads both halves back, and unlinks it. Every iteration is a
real `cas_put` — index probe, index stage, `bm_alloc`, journal transaction —
followed by a real `cas_free` — index remove, `bm_free`, generation bump, second
journal transaction — from every core at once. Uniqueness is what makes it work
rather than theatre: identical content would dedup to one block and the soak
would allocate once and measure nothing.

**One role, not four.** The append workers take their index from their role
number because they must agree on four fixed payload patterns. These workers
need only to be distinct, and `SYS_GETPID` already gives each one a value
nothing else shares. Four role numbers is a cost paid in a namespace matched by
nothing but the integer — v0.81 and v0.82 both lost time to two suites sharing
role 7 — and there was no reason to pay it twice. Role 61 is the first free
number; the highest previously assigned was 60.

### Measured, `smp4-bios`

```
8 worker(s) on 4 core(s), 12 iteration(s) each — 8 ok, 0 deadline, 0 failed;
ran on 4 core(s) (mask f) in 1167 ticks
unreferenced 2 -> 2, used_blocks 1121 -> 1121, gen bumps 386 -> 573,
frees 386 -> 573, underflow 0 -> 0, dangling 0
```

187 real frees during the soak, and every block came back. The assertions are
exact rather than bounded: unreferenced and `used_blocks` must return to
**exactly** their pre-soak values, not "no worse than" — one leaked block per
cycle is invisible for a long time and then is not.

The premise guard is the one that makes the rest mean anything: **the workers
ran on more than one core**, asserted wherever more than one exists. A soak
confined to the boot core is a single-threaded test reporting a concurrency
result.

---

## Two defects found in this work, both in my own code

Recorded because the tree's convention is that the correction is worth more than
the conclusion, and because each was caught by a specific tier rather than by
review.

### 1. The worker assumed whole-file writes. Every worker failed identically.

First run: 8 of 8 workers exited 1846, "the read-back was the wrong CONTENT" —
which reads exactly like the CAS handing back somebody else's block, the v0.56
failure this soak exists to catch.

It was the test. `SYS_WRITE_FILE` has been **positional since v0.83**, so the
worker's two writes produced a 384-byte file rather than replacing the first
payload, and the read-back compared 192 bytes from offset 0 against the *second*
payload. The uniform failure across every worker and every core was the tell: a
real concurrency defect does not fire 8 times out of 8 deterministically.

The fix verifies **both** halves, which is the stronger test anyway — it proves
the second write did not disturb the first.

### 2. A counter incremented somewhere other than where its name claimed

The pre-SMP instrument began as "`g_ncpu_online` at the first call to
`cas_journal_recover()`", named `boot_cpu`, asserted `== 1`. Uniprocessor
passed. **smp4-bios read 4 and failed**, correctly.

On a fresh volume `cas_mount()` returns at the magic check without ever reaching
`cas_journal_recover()`, so there is no boot-time recovery at all — and the
"first call" was an **in-suite remount**, which runs after `smp_init()` with
every AP live. The measurement was real; the label was fiction.

This is the counter-that-lies family, one step along from the counter nothing
increments (`g_reproc_stale_ppid`): a counter incremented in a place its name
does not describe. It would have shipped green on the uniprocessor tier alone,
and the only reason it did not is that the assertion was run on a tier where the
two readings differ.

The repair keys off a state the code can test (`g_smp_started`, set at the top of
`smp_init()`) rather than a call ordinal, and separates the two claims:

- the **boot mount** is always pre-SMP, so that is asserted on every volume
- a **boot-time replay** only happens on a dirty volume, so it is asserted only
  where one actually occurred, and the fresh case prints why it did not

---

## Verified

Default build, image md5 `bc1c521f01e4ed74255b3ae0b39d4ce6`, clean rebuild with
zero warnings. **Every row below is that one image**, re-run at the end rather
than assembled from the builds made along the way — an earlier identical-source
build produced a different ISO md5 (grub-mkrescue is not reproducible here), and
a table whose rows name different binaries is the evidence problem this
repository has been burned by twice.

| tier | result | required minimum |
|---|---|---|
| uniprocessor | **PASS** 555 / 0 / 0 ranks (325 s) | 542 |
| smp2-bios | **PASS** 570 / 0 / 0 ranks (255 s) | 556 |
| smp4-bios | **PASS** 574 / 0 / 0 ranks (270 s) | 560 |
| smp4-iommu | **PASS** 587 / 0 / 0 ranks (270 s) | 573 |
| `gate-dirty` ×3 boots, uniprocessor | **PASS** — artefacts on every boot ≥2 | — |
| `gate-dirty-smp` ×3 boots, `-smp 4` | **PASS** — artefacts on every boot ≥2 | — |

Every tier is above its stated minimum. The increases are accounted for rather
than assumed: +2 from the boot-mount CPU assertions and the rest from the
contention soak, whose count varies by tier because the multi-core premise guard
is asserted only where `n > 1`.

### The dirty gate, in detail

Both configurations report `cas-recovered=1` on boots 2 and 3, and the
measurement from the `-smp 4` boot 2 is the one worth quoting, because it is the
claim the corrected instrument exists to make:

```
cas cross-reboot: marker at block 1125, bitmap says ALLOCATED, 512/512 byte(s)
exact, dangling 0, used_blocks 1290 vs popcount 1290
cas recovery context: 3 recover call(s), 1 replay(s) of which 1 ran pre-SMP;
CPUs online at the boot MOUNT = 1, at the first pre-SMP replay = 1,
highest at any replay = 1 (this boot has 4 online now)
```

A real replay, on a four-CPU boot, having run while exactly one CPU was online —
and the conditional replay assertion firing rather than being skipped. Archived
as `OUTRUN-0.89-dirty-up-boot2.log` and `OUTRUN-0.89-dirty-smp4-boot2.log`, both
carrying their image md5 on the first line.

### The falsifier

`-DCAS_NORECOVER_REPRO`, image md5 `652c08fa3af8feba72db2069dce7d50c`:
`DIRTY-VOLUME GATE: FAIL`, `boot 2 failing-assertions=1`, and the consecutive-boot
diff reports `new=1` — the assertion appears exactly where the crash was staged
and nowhere else.

---

## BLOCKER — the contention soak destabilises `threadstrs` at two CPUs

Found while cutting v0.88.0. **The tag was not created.** The release protocol
says a version tag is a tag plus an ISO that booted clean, and `release-verify`'s
own contract fails unless every suite reports zero failures. This one does not.

### What happens

The v0.88.0 release artefact (`725b9b7b229e071aa2cb4a3b5163be5b`) fails
`smp2-bios` intermittently, on two `threadstrs` assertions:

```
[threadstrs] FAIL: threads were dispatched on MORE THAN ONE core
[threadstrs] FAIL: at least two cores were inside ring 3 simultaneously
```

Both are **premise guards** — "did the scheduler actually use both cores for
these workers" — not correctness assertions. No byte, count or invariant is
wrong in a failing boot.

### Localised by measurement, not by reading

| build | `smp2-bios` |
|---|---|
| v0.87.0, same host and time window | **4 / 4 PASS** |
| v0.88 release artefact | **2 / 4 PASS** — 2 failed |
| v0.88 with `-DCASC_SKIP` (soak omitted) | **4 / 4 PASS** |

The control matters here: the host was measurably slower during this window
(smp2 at 245–265 s against 130 s earlier the same day), which is the profile of
the flakiness this tree has blamed on host load before. v0.87.0 going 4/4 under
those same conditions rules that out. `-DCASC_SKIP` going 4/4 rules in the soak.

`vfsstrs` (where the soak lives) runs at log line ~6119; `threadstrs` at ~7429.
The soak precedes it, spawns `2 × n` unaffined workers, and adds roughly twelve
seconds before `threadstrs` starts.

### What this is, most likely — and what it is not

The soak did not break threading. It changed the timing enough to land a
pre-existing race on its bad side. The sibling suite says so about its own copy
of this guard, in the source, today:

> v0.80: same family as threadstrs and mcq — see the note there. **At two cpus
> the whole worker pool can be serviced by one core before the other reaches it,
> so this is a race rather than a defect.** Skipped with the observation printed,
> not deleted.

`pthreads_smp` therefore guards this at `n >= 3`. `threadstrs` guards it at
`n >= 2`, on the strength of 8-of-8 boots measured when the threshold was moved.
**That 8-of-8 evidence is what this change invalidates**: it was collected
without a phase that spawns four unaffined workers immediately beforehand.

In a failing boot `mcq` still reports *"ring-3 concurrency high-water mark: 2
core(s) at once"* — the machine does get both cores into ring 3. The failure is
specific to `threadstrs`' own worker dispatch.

### Options, and the one I would not take quietly

1. **Align `threadstrs` with `pthreads_smp`**: assert at `n >= 3`, print the
   observation at `n == 2`. Consistent with the sibling and with its written
   rationale — but it is also "weaken the assertion until my change passes", and
   that is precisely the move this repository's conventions exist to make
   somebody argue for out loud rather than slip in beside a release. It needs a
   deliberate decision, not a release engineer's judgement call at tag time.
2. **Restore the conditions the guard was validated under**: run the soak after
   `threadstrs` rather than before it. Weakens nothing, but only moves the
   fragility out of sight.
3. **Investigate the scheduler state the soak leaves behind** — whether cpu 1 is
   left halted and slow to pick up work after the drain. The most useful answer
   and the most expensive; it would say whether the guard is fragile or the
   drain is leaving the machine unbalanced.

Recorded rather than chosen. `-DCASC_SKIP` is kept in the tree as the bisection
tool that found this.

## Not covered

- **Bare metal and Proxmox.** Unchanged from every previous cycle.
- **Soak/repeat.** One boot per fresh tier, three per dirty configuration. This
  cannot see an intermittent below roughly 1 in 10 boots — and a contention
  phase is exactly the kind of thing whose failures are intermittent, so this
  gap is more relevant here than usual.
- **The soak's own duration.** 12 iterations per worker, sized to fit inside the
  standing gate rather than to stress the allocator for minutes. A longer run is
  a separate, opt-in target and does not exist yet.
- **Larger core counts.** `smp8-bios` exists as a diagnostic but was not run for
  this work; the soak's worker count is `2 × cores` capped at 32, so its
  behaviour above 4 cores is untested here.
- **The `cascrashwrite` fixture on SMP dirty boots** stages its transaction on
  boot 1, which halts before the APs matter. The recovery it leaves is judged by
  boots 2 and 3, which do run `-smp 4`.
