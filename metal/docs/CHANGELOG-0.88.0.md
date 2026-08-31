# OutRun OS v0.88.0-metal — crash-testing the paths that were only ever argued

Milestone 88. Every item below closes a claim this tree had been making on the
strength of a code reading rather than a measurement — and three of them turned
out, when measured, to be about something other than what their names said.

## ARTEFACT

```
outrun-os-0.88.0.iso   (5,920,768 bytes)
MD5    e2b212773609dfcab624b88ff9ee7ea5
SHA256 a1f8be66ca876e18a7ed3256cb1b757d2dffc1640c13db4d5695093a3cbfc2dc
```

The kernel inside, which is the artefact that identifies the *code*:

```
outrun-kernel.elf
MD5    cc08fca2c4a3cf73a04a5efc4fa02f45
SHA256 12be14dd8aff0e7b57e45efe16dbec2a5a617d0aa691a5ba49a802ae088c6400
```

`make release-verify` **PASS** on the ISO md5 above — 45 suites, 0 failing
assertions, 0 rank faults, 305 s, uniprocessor. The full matrix was then run
against **that same artefact** rather than against a development image; see GATE.

Both checksums are published because they answer different questions: the ISO
md5 says which image a log came from and changes on every rebuild
(`grub-mkrescue` embeds timestamps), while the kernel ELF md5 is stable across
rebuilds of unchanged source and is what distinguishes "rebuilt" from "changed".
That distinction, established in v0.86, earned its keep again here — an earlier
release candidate for this same tag carried ISO md5
`725b9b7b229e071aa2cb4a3b5163be5b` and was discarded, for the reason recorded
under *The tag that was withheld* below.

## WHAT LANDED

### The CAS PUT path, crash-tested at last, against the hazard v0.48 wrote down

The CAS metadata journal's own header comment has named its reason since v0.48:
*"index home-write lands, then a crash before `cas_flush_meta`'s bitmap/superblock
write lands -> index says hash X is at block B, bitmap says block B is free -> a
future put legitimately overwrites block B, silently corrupting a different
dedup'd file."*

For forty milestones the evidence for that guard was that the ordering reads
correctly. v0.86 is the milestone that established what such evidence is worth.

A third crash-injection arm (`g_cjp_arm`, beside v0.85's commit and free arms)
fires inside `cas_put()` after the home index write and before the bitmap flush.
A new instrument, `cas_index_verify_locked()`, asks the mirror of the v0.84
question: not *which allocated blocks does nothing name* but **which index
entries name a block the bitmap calls free**. Neither sweep can see the other's
defect, and the standing `used_blocks == popcount(bitmap)` audit sees neither,
because the index is not party to it.

The falsifier is not a contrived reorder — `-DCAS_PUT_NOJOURNAL_REPRO` is the
shipped pre-v0.48 sequence:

| build | dangling | probe block | next put | result |
|---|---|---|---|---|
| journalled | 0 → **0** | 1125 | 1126 | PASS |
| no journal | 0 → **1** | 1125 | **1125** | FAIL, as required |

It does not merely trip the invariant. **The very next put was handed block 1125
while the index still named it** — the corruption reproduced end to end.

A limit of the new standing audit, found by that same falsifier and recorded
rather than smoothed over: at *end of boot* it read 0 dangling even on the broken
build, because block 1125 had been reallocated by then and the stale entry no
longer named a *free* block. It guards against the state persisting; it does not
detect the corruption having happened.

### `g_cas_legacy` — ten branches on a compile-time constant, removed

The second half of that objective asked for crash injection on the legacy
pre-journal branch of `cas_free`. **Nothing can reach it.** Measured, not
inferred: thirteen references and all three assignments write `0` — the
initialiser, `cas_format()`, and `cas_mount()`, where the line still carried its
v0.56 reason (*"pre-v4 volumes no longer mount at all"*). Only version 5 mounts.

A branch nothing enters is the mirror of a counter nothing increments, and this
tree has been misled by both. The declaration, both assignments and all ten
branch sites are gone, along with `vfs_flush()`, which the removal orphaned — and
which would have been wrong to keep as a general-purpose flush anyway, since its
loop clamps at 8 directory blocks, the pre-v0.48 bound.

Assertion counts did not move, which is the expected result for deleting what the
compiler had already proved unreachable.

### The three unproven journal-IRQ guards now have falsifiers

Of the four checks in the v0.86 journal-IRQ phase, only the premise guard had
ever been observed producing both verdicts. The source carried one sentence —
*"deliberately NOT falsified by re-enabling interrupts inside a critical
section"* — and v0.86 and v0.87 both shipped reading it as covering all four. It
covers exactly one: check 3, which is also the only one already proven.

| flag | target | measured |
|---|---|---|
| `-DVJIRQ_DETECT_REPRO` | detection | FAIL — `0 append(s)` |
| `-DVJIRQ_CONTROL_REPRO` | positive control | FAIL — `control IF=0` |
| `-DVJIRQ_BYTES_REPRO` | byte integrity | FAIL — `read back 127/128` |

The control falsifier is the one that mattered: a positive control that has only
ever returned one value is the `g_reproc_stale_ppid` mistake, and this one now
demonstrably returns both.

**A prediction the measurement disproved.** The comment written alongside the
detection falsifier claimed checks 1 and 3 were not independent, so falsifying
detection would red both. It does not — check 3 *passes*, because creating the
probe file is itself a journal commit. Which means **check 3's `dc > 0` clause is
satisfied by the setup alone**, and check 1 must assert `wrote == 8` rather than
merely `dc > 0`. Delete check 1 as redundant, as a tidying pass reasonably might,
and the phase goes green on a boot that appended nothing.

The surviving ruling is narrow and stated: check 3 gets no falsifier, because its
falsifier is a journal commit under `cli` and the timer ISR takes locks.

### Journal replay across a real power cycle, and the boot ordering that protects it

v0.48 gave the **VFS directory journal** a cross-reboot proof. The **CAS metadata
journal** never had one. This cycle's own crash injection remounts *in-process*,
where the RAM bitmap, superblock and index buffer are already post-transaction —
so a recovery that wrote nothing at all would still leave the in-memory picture
correct. Across a power cycle nothing is in RAM.

`cascrashwrite` leaves **both** journals dirty in one cut, and the order inside it
is load-bearing: `vfs_write_file` stores through `cas_put`, which runs a complete
metadata transaction *including the journal clear*, so staging the CAS crash
first would erase it with the file's own. VFS marker first, CAS transaction
second.

**The falsifier broke a different check than predicted, and the reason is the
hazard finishing its work.** `-DCAS_NORECOVER_REPRO` fails the dirty gate as
required — but the *allocated* check passed and the **byte-for-byte read-back**
went red. Between mount and probe the boot's own suites allocate; block *b* is
free on disk, `bm_alloc()` hands it out, and somebody else's content lands on it.
By probe time *b* is allocated by the wrong owner while the index still maps the
marker's hash to it. A suite carrying only the allocation check would have gone
green.

**Recovery is not IRQ-guarded, and must not become so.** v0.86 measured that
journal commits already run with interrupts enabled and located atomicity in
mutual exclusion; masking across a path the timer ISR reaches buys a `TRUNCATED`
boot, not safety. The property actually wanted — no AP running during replay —
holds by **boot ordering**: `kernel_main` calls `cmd_cas()` before `smp_init()`.
That is now measured rather than read off two calls forty lines apart.

### Role 61 — the CAS index and bitmap under multi-core contention

The append-oversubscription phases contend on the VFS append offset and, after
their first iteration, on almost nothing else: every worker writes the *same*
payload, so every chunk dedups and the CAS allocates no block, frees no block and
mutates no index entry for the rest of the run.

Role 61 makes each iteration a real `cas_put` *and* a real `cas_free` from every
core at once, with content unique to (worker, iteration) so nothing dedups. One
role rather than four: these workers need only to be distinct, and `SYS_GETPID`
already provides that — four more entries in a namespace matched by nothing but
the integer is a cost v0.81 and v0.82 each paid once already.

```
smp4-bios: 8 workers on 4 cores, 12 iterations each — 8 ok, 0 deadline, 0 failed,
ran on 4 cores in 1167 ticks
unreferenced 2 -> 2, used_blocks 1121 -> 1121, 187 real frees,
underflow 0, dangling 0
```

The reclamation assertions are **exact, not bounded**: unreferenced blocks and
`used_blocks` must return to precisely their pre-soak values. One leaked block per
cycle is invisible for a long time and then is not.

### The oversubscription worker ceiling stops wearing the role count's name

v0.87 added an assertion that the tier ran at the ratio it was *built* for, and
recorded the ceiling itself as debt. `APPSMP_W` is the number of worker **roles**
and says nothing about how many workers may run; the clamp was the array bound
wearing the role count's name, and it bit whenever `n > APPSMP_W` **independent
of the ratio** — so the 4:1 tier passing on a 4-core host had never exercised it.

The fix is six lines. Three more layers hid behind it, each found by *running* the
fix rather than reviewing it: the phase watchdog did not scale; the completion
check recognised the deadline codes, printed *"a slow host, not a lost append"*,
and then failed for them anyway; and the ring-3 worker deadline did not scale
either. A regression introduced by the first attempt — a bare multiply that
scaled the deadline *down* below 8 workers, invisible to the 4-core gate — is
recorded in the roadmap as such.

## THE TAG THAT WAS WITHHELD

The first v0.88.0 candidate was built, checksummed and `release-verify`'d clean,
and **was not tagged**. Running the full matrix against that artefact — rather
than inheriting a development image's result — found `smp2-bios` failing
intermittently on two `threadstrs` premise guards.

Bisected on one host inside one window:

| build | `smp2-bios` |
|---|---|
| v0.87.0, same host and window | **4 / 4 PASS** |
| v0.88 candidate | **2 / 4 PASS** — 2 failed |
| v0.88 with `-DCASC_SKIP` (soak omitted) | **4 / 4 PASS** |

The control mattered: the host was measurably slower in that window (smp2 at
245–265 s against 130 s earlier the same day), which is the profile this tree has
correctly blamed on load before. v0.87.0 going 4/4 under the same conditions
ruled that out; `-DCASC_SKIP` going 4/4 ruled the soak in.

**Nothing was wrong in a failing boot.** Both assertions are premise guards —
*did the scheduler use both cores for these workers* — and no byte, count or
invariant differed. `mcq` reported two cores in ring 3 on the very same boot.

### The guard was aligned with its sibling, deliberately and on the record

`threadstrs` asserted this at `n >= 2`; `pthreads_smp` has always guarded its own
copy at `n >= 3`, with the rationale in the source: *"At two cpus the whole worker
pool can be serviced by one core before the other reaches it, so this is a race
rather than a defect."* The v0.84 evidence that promoted `threadstrs` to `n >= 2`
was 8 of 8 boots — collected on a boot sequence with no phase spawning unaffined
workers immediately beforehand, which is exactly what role 61 now does.

At `n == 2` the assertions become an observation carrying the measured numbers,
so a degraded run is still distinguishable from a skipped one. `n >= 3` keeps the
hard assertions unchanged.

This is a **loosening**, and it is called one. Making a gate green by widening a
guard is the move that has to be argued out loud rather than slipped in beside a
tag; it was put to the maintainer as one of three options and approved
explicitly, with the two alternatives — moving the soak after `threadstrs`, or
investigating whether the drain leaves cpu 1 halted — recorded in
`ROADMAP-0.89.0.md` as not taken. `-DCASC_SKIP` remains in the tree as the
bisection tool that found it.

Cost: `smp2-bios` reports **568** rather than 570. The two assertions are not
deleted, and the property is printed on every `-smp 2` boot.

## TWO DEFECTS FOUND IN THE TESTS THEMSELVES

Both were caught by a tier rather than by review, and both are the same family:
code that measured something other than what its name claimed.

**A counter incremented somewhere other than where its name said.** The pre-SMP
instrument began as *"`g_ncpu_online` at the first call to
`cas_journal_recover()`"*, named `boot_cpu`, asserted `== 1`. Uniprocessor
passed. **`smp4-bios` read 4 and failed, correctly.** On a fresh volume
`cas_mount()` returns at the magic check without ever reaching recovery, so there
*is* no boot-time recovery — and the "first call" was an in-suite remount running
after `smp_init()` with every AP live. The measurement was real; the label was
fiction. It would have shipped green on the uniprocessor tier alone.

`g_smp_started` — set at the top of `smp_init()` — replaces the call ordinal with
a state the code can test, and the two claims are now separate: the boot **mount**
is always pre-SMP and is asserted on every volume; a boot-time **replay** only
happens on a dirty one and is asserted only where it occurred. Measured on the
dirty `-smp 4` boot 2: *"1 replay(s) of which 1 ran pre-SMP; CPUs online at the
boot MOUNT = 1 … (this boot has 4 online now)"*.

**A worker that assumed whole-file writes.** The first contention run had 8 of 8
workers exit on *"the read-back was the wrong CONTENT"* — which reads exactly like
the CAS handing back somebody else's block, the v0.56 failure the soak exists to
catch. It was the test: `SYS_WRITE_FILE` has been **positional since v0.83**, so
two writes make a 384-byte file rather than replacing the first payload. The
uniform 8-of-8 failure was the tell; a real concurrency defect does not fire
deterministically. The fix verifies both halves, which also proves the second
write did not disturb the first.

## GATE

Full six tiers, **all on the published artefact** `e2b212773609dfcab624b88ff9ee7ea5`
— not on a development image, and not assembled from separate builds. That
distinction is not ceremony here: it is what caught the withheld candidate above.

| tier | v0.87 | v0.88 |
|---|---|---|
| uniprocessor | 540 | **555** |
| smp2-bios | 554 | **568** |
| smp4-bios | 558 | **574** |
| smp4-iommu | 571 | **587** |
| `gate-dirty` (3 boots, one image) | 0 failing | **0 failing**, empty diffs |
| `gate-dirty-smp` (3 boots, `-smp 4`) | 0 failing | **0 failing**, empty diffs |

Zero rank faults throughout; clean build with no warnings. Durable cross-boot
artefacts intact on every dirty boot, now including the CAS metadata journal's
own.

`smp2-bios` was additionally run **4 more times** on the fixed build: 4/4 PASS at
568, with the `threadstrs` observation reporting 2 cores and a ring-3 high-water
of 2 on all four — the property still holds routinely at two CPUs; it is simply
no longer *asserted* there.

Falsifier runs, each required to fail and each having done so:
`-DCAS_PUT_NOJOURNAL_REPRO`, `-DCAS_NORECOVER_REPRO`, `-DVJIRQ_DETECT_REPRO`,
`-DVJIRQ_CONTROL_REPRO`, `-DVJIRQ_BYTES_REPRO`.

NOT run for this tag: bare metal, Proxmox, soak/repeat beyond the boots above,
and `smp8-bios`, which exists as a diagnostic. This cannot see an intermittent
below roughly 1 in 10 boots on the fresh tiers — a gap more relevant than usual
now that a contention phase is in the set, and one this milestone has already
been bitten by once.

## KNOWN, NOT FIXED

- **The `threadstrs` 2-CPU dispatch guard is now an observation, not an
  assertion.** The underlying question — whether the soak leaves cpu 1 halted and
  slow to pick up work, or whether the guard was always this fragile — is
  unanswered. Option 3 in `ROADMAP-0.89.0.md`.
- **The standing index audit cannot see a dangling entry once the block it names
  has been reallocated.** It guards against the state persisting, not against the
  corruption having happened.
- **The generation table and the TALLY sweep are both per-boot.** v0.87 proved the
  reset is complete; nothing establishes that anything is *lost* by it.
- **The oversubscription ceiling is higher, not absent.** Above 8 cores at 4:1 the
  clamp bites again — loudly now, rather than silently. `MAX_KPROC = 64` is the
  real wall behind `APPSMP_MAXW = 32`.
- **The contention soak is 12 iterations per worker**, sized to fit the standing
  gate rather than to stress the allocator for minutes. No long-running opt-in
  target exists yet, and its behaviour above 4 cores is untested.
- **`lseek`/`SEEK_END` on another user's tmp descriptor discloses that file's
  length.** POSIX-correct and asserted deliberately. Closed as a question in
  v0.87; the wording stands.
- **VOL_TMP has no mode and no group.** Deliberate; owner-or-root is the whole
  rule, so a tmpfile cannot be shared.
