# OutRun OS v0.86.0-metal — roadmap

Opened at `worktree-v086-oappend-oversub`, based on `main` at the v0.85.0 tag.
`VERSION` is `0.86.0-dev` and `KERNEL_VERSION` is `"0.86.0-dev"`; both move to
`0.86.0` / `0.86.0-metal` as step 1 of the Release Protocol, not before.

**The v0.86.0 tag does not exist yet.** Nothing in this document is a released
claim.

---

## Correction carried into this cycle

This cycle was opened with a mandate to implement two items — O_APPEND under CPU
oversubscription, and a CAS refcount-underflow design spike — as **v0.85 technical
debt**. Both had already shipped in v0.85. Recording that here, because it is not
the first time in this repository that a cycle has opened against a superseded
status, and because writing the correction down is cheaper than re-implementing
landed work:

| item as briefed | actual state at v0.85.0 |
|---|---|
| "Update the O_APPEND harness to add an unpinned, oversubscribed mode" | Shipped. `append-oversub` phase in `kernel64.c`: 8 workers on 4 cores, affinity unset. |
| "Adapt interleave and core-mask checks for preemptive scheduling" | Shipped. The core-mask guard was replaced by the structural *more workers than cores* assertion for exactly this reason. |
| "Falsify by reverting `vfs_write_append()` to seek-then-write" | Shipped as `-DAPPEND_RACE_REPRO`, which **is** that revert. Measured: 1760 B of 4096 — 57% of appends lost. |
| "Design spike: CAS refcount underflow" | Ran in v0.85. Conclusion: no counter-shaped fix exists for the masked-double-release class. The TALLY sweep was built instead, and its inability to catch the v0.84 defect is recorded rather than glossed. |

What was **genuinely** still open is what the v0.85 KNOWN-NOT-FIXED list named,
and that is what this cycle executed:

> *"The oversubscription ratio is fixed at 2:1, and nothing tests a writer
> preempted while the journal transaction itself is mid-flight."*

---

## Item A — completed

### A1. The oversubscription ratio is no longer a constant

2:1 is the mildest oversubscription that exists. A scheduler defect needing three
runnable writers per core could not be reached by **any build in the tree**,
because four separate expressions each spelled the `2` out.

`APPSMP_OSRATIO` now parameterises it, and the phase derives its buffer, worker
count and per-pattern expectation from it. **The default stays 2**, so the gate
cost and assertion count do not move; a higher ratio is a build-time override
(`make EXTRA=-DAPPSMP_OSRATIO=4`), the same opt-in idiom the reproducers use.

### A2. The journal commit is preemptible — and the first answer was wrong

"Nothing tests a writer preempted mid-journal-transaction" looked, on a first
reading of the lock, like it described something unreachable: `klock_acquire()`
opens with `klock_irq_save()`, which is a `cli`, so a thread holding
`g_vfs_lock` cannot take a timer tick and cannot be preempted.

**That reading was wrong, and the measurement is what caught it.** `klock_acquire`
also *closes* with `klock_irq_restore()`, under the comment "check done: reopen
IF". The mask covers the rank-stack check only — not the critical section. The
first boot to ask the question reported **9 of 9 journal commits running with
interrupts enabled**.

This is recorded rather than quietly rewritten because it is the exact failure
mode this tree keeps legislating against: an argument about a guard, believed
because it reads correctly, and worth less than one measurement of it. The first
version of this instrument asserted "every commit runs with interrupts disabled"
and **failed on its first run** — against my own reasoning, not against a bug.

**What is actually true, and now asserted:** a writer *is* preempted
mid-transaction, routinely, and the append phases still lose nothing. That
locates the real guarantee — O_APPEND atomicity comes from **mutual exclusion**
on `g_vfs_lock`, not from interrupt masking. A future change that "fixed" an
append race by masking interrupts would be treating a symptom.

`vj_publish()` — the single choke point every journal transaction passes through
— samples `RFLAGS.IF`. Four checks, ordered so each later one means something:

1. **DETECTION** — the workload actually reached the journal (`dc > 0`). Without
   it every count below is zero for the uninteresting reason.
2. **POSITIVE CONTROL** — the instrument can report interrupts-on at all,
   sampled outside every klock. A sampler stuck at one value would otherwise
   satisfy check 3 whatever the kernel did; this repository has shipped that
   mistake once, as `g_reproc_stale_ppid`.
3. **PREMISE GUARD** — the commits really were preemptible. If a future change
   makes the journal run under `cli`, this fires, and it *should*: it would mean
   the `append-oversub` phases are no longer testing a preemptible path and
   their conclusion has quietly changed meaning. Same role as that phase's
   "more workers than cores" guard.
4. **THE INVARIANT** — every appended byte is present and correct anyway,
   verified by reading the file back rather than trusting the per-call return
   codes, since a lost append is precisely the failure that reports success on
   every call.

**Deliberately not falsified by re-enabling interrupts inside the critical
section.** The timer ISR takes locks, so `sti` there is a real hazard rather
than a test of one — and this project has already learnt that costs a
`TRUNCATED` boot and no verdict at all. Check 2 is the falsifiable half, and it
can fail.

#### What these four checks do NOT catch — measured, not assumed

The `-DAPPEND_RACE_REPRO` build was re-run against this kernel to confirm the
reverted `vfs_write_append()` still fires. It does, hard:

| phase | got | wanted | lost |
|---|---|---|---|
| `append-smp` (4 pinned) | 880 B | 2048 B | 57% |
| `append-oversub` (8 on 4) | 1760 B | 4096 B | 57% |

**All four of the new journal-irq checks passed on that knowingly-broken
kernel.** That is expected — the `vjirq` workload is single-threaded, so a
seek-then-write race has no competing writer to lose against — but it has to be
written down, because "passed against the reverted build" is exactly the
signature of an assertion that cannot fail.

So, precisely which of the four has demonstrated it can fail:

- **Check 3's counter has.** Its first form asserted `dirq == 0` and failed at
  `dirq == 9`; the current form asserts `dirq == dc`. The same counter has been
  observed producing both verdicts, so it is live and discriminating.
- **Checks 1, 2 and 4 have not.** They are regression guards: check 4 would fire
  if a future change broke the single-threaded append path, and check 2 if the
  sampler or its call site moved under a lock. Neither has been *observed*
  failing, and neither should be described as proven until it has.

The honest summary is that this block characterises where atomicity comes from
and guards the premise of the oversubscription phases. **It is not a second
detector for the append race** — `append-smp` and `append-oversub` are that, and
they are the two that went red under the revert.

---

## Incidental finding: the ISO md5 does not identify the code

Noticed while checking that the tree still built what had just been tested, and
recorded because it qualifies the evidence convention in `CLAUDE.md`.

Building the *same source* three times produced three different ISO checksums:

```
ba2525f809ba19f3af2e88937767a60c
01e49c41fbcd192ea461318699a5e639
423ba1d935cabebcdaf3873f5514208e
```

while the binaries inside were bit-identical every time:

```
outrun-kernel.elf  1eaa49e67f5bcc876df5759fa55d8b24
user_init.elf      8c47bf5721d3650490f14358e7a254ed
```

The ISO wrapper embeds build metadata (`grub-mkrescue` timestamps), so the image
md5 is **not** reproducible from source.

This does not weaken the stamping rule — the whole point of that rule is to name
*which image* a log came from, and a per-build md5 does that perfectly. But it
does mean the image md5 cannot answer a different question that looks similar:
"is this the same code as the run that passed?" Two boots of identical source
carry different image stamps, so a mismatch there is not evidence of a code
change, and matching stamps across a rebuild is not something to wait for.

**Use the kernel ELF md5 for that question.** It is stable across rebuilds of
unchanged source, which makes it the artefact that can actually distinguish
"rebuilt" from "changed".

---

## Item B — design spike: per-block generation tracking

**Question as posed:** can per-block generation tracking be embedded in dirent
metadata or reserved fields, without breaking layout constraints, to catch a double
release masked by an intervening dedup?

**Answer: not in the on-disk layout. It does not fit, and the shortfall is not
marginal.**

### The three hard constraints

| structure | size | enforced by | free space |
|---|---|---|---|
| `struct dirent` | exactly 256 B | `_Static_assert` | **`reserved[4]` — 4 bytes** |
| `struct cas_islot` | 16 B packed | `CAS_SLOTS_PER_BLOCK = 512/16 = 32` | 0 |
| indirect block | 64 × `uint64` | exactly 512 B | 0 |

### Why each placement fails

**In the dirent.** A generation per direct chunk needs `VFS_MAX_CHUNKS × 2 B =
32 bytes`. Four are available — short by a factor of eight. Narrowing to one byte
per chunk still needs 16 and still does not fit, and an 8-bit generation wraps
after 256 reuses of a block, which on a volume that recycles blocks is not a
diagnostic, it is a coin flip. It would also cover only the **direct** map: chunks
reached through `ind1_hash` / `ind2_hash` live in blocks that are exactly full at
64 × 8 B, so a parallel generation array there means a *second block per indirect
level* — doubling indirect I/O to carry a debug field.

**In the CAS index slot.** `cas_islot` is 16 bytes and its size *is* the divisor
that makes `CAS_SLOTS_PER_BLOCK` come out at 32. Only 16 and 32 divide 512 evenly;
20 gives 25.6. So the only growth available is 16 → 32 bytes, which **doubles the
index region and halves the slots per block** — a volume-format break that makes
every existing volume unreadable.

**Widening the format deliberately.** Precedented: v0.56 and v0.72 both carved
`reserved[]`, and v0.85 bumped the volume signature to force a reformat. But those
paid a format break for a *feature* — indirect maps, ownership. This would pay one
for an *assertion*. That trade is available and should not be taken yet.

### The viable proposal: derive it, do not store it

The refcounts themselves are already **derived at mount** rather than stored, and
v0.84 recorded why that is the stronger property — it cannot drift from the
directory or be left stale by a crash. A generation table can be exactly the same
shape:

```c
static uint16_t g_cas_gen[CAS_REF_MAX];    /* CAS_REF_MAX == 65536 */
```

128 KiB of RAM, mirroring `g_cas_refs` — no on-disk change, no format break, no
volume compatibility question. `cas_free` bumps a block generation as it clears the
bitmap bit; a release presenting a generation older than the current one is a
double release, and it is caught **at the moment it happens** rather than inferred
later from a count that dedup has already restored.

**What it still would not catch**, stated because a proposal whose gaps are
invisible is how the TALLY sweep came to be recommended for a defect it cannot
see: it is per-boot, exactly like the TALLY sweep, so corruption in an earlier boot
of a reused volume is erased by the mount that precedes it. Closing *that* needs
the generation to be durable, which needs the format break above.

**Recommendation:** implement the derived table in v0.86 behind the same
build-time falsifier discipline as `CAS_TALLY_FALSIFY`; defer the durable form
until something other than a diagnostic also wants the format break, so the cost
is paid once.

---

## Carried forward from v0.85

Unchanged unless marked:

- ~~**The oversubscription ratio is fixed at 2:1**~~ — **closed, A1.**
- ~~**Nothing tests a writer preempted while the journal transaction is
  mid-flight**~~ — **closed, A2.** It is not unreachable: every commit is
  preemptible, and the invariant holds by mutual exclusion instead.
- **The TALLY sweep sees only since the last rebuild.** Open. The Item B proposal
  inherits this limitation and says so.
- **VOL_TMP has no mode and no group.** Deliberate; owner-or-root is the whole
  rule, so a tmpfile cannot be shared.
- **`lseek` / `SEEK_END` on another user tmp descriptor discloses that file
  length.** Metadata rather than content, and it matches the root volume.
- **No crash injection on the PUT path**, which has always had the consistent
  ordering, and none inside the legacy pre-journal branch of `cas_free`.

## Objectives for the rest of the cycle

1. The Item B derived generation table, with its falsifier.
2. Crash injection on the CAS PUT path and the legacy `cas_free` branch.
3. `CHANGELOG-0.86.0.md` written **as the cycle runs**, per the v0.85 precedent.
