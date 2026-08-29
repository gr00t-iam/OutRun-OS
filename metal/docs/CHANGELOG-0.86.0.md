# OutRun OS v0.86.0-metal — the measurement that disproved the argument

Milestone 86, written as the cycle ran. A short milestone with one uncomfortable
result at the centre of it: the instrument built to confirm a claim about the
journal disproved it on its first boot, and the claim was mine.

## ARTEFACT

```
outrun-os-0.86.0.iso   (5,906,432 bytes)
MD5    59b983039d27fec9b0a8d75992ace0c7
SHA256 311b040bc0f994b53cb8a0647a05b9cea049b48235e30fdfa8c42ef23aa31192
```

The kernel inside, which is the artefact that identifies the *code*:

```
outrun-kernel.elf
MD5    d816e68c04ded1497b067aab59f70ca4
SHA256 229ef395badae54d06f9bdec37919a5705ebcc40a3ac7b44918a9ea3132de888
```

`make release-verify` PASS on the ISO md5 above — 45 suites,
0 failing assertions, 0 rank faults, 315 s.

**Both checksums are published because they answer different questions.** The
ISO md5 says which image a log came from. It cannot say whether two runs used
the same code: `grub-mkrescue` embeds timestamps, so three builds of identical
source produce three different ISO checksums. The kernel ELF md5 is stable
across rebuilds and is what distinguishes "rebuilt" from "changed". This is new
in v0.86 and is recorded below.

Note that this ELF md5 is *not* the `f25d8d98...` that the 537/0 development boot
carried: `KERNEL_VERSION` is compiled in, so bumping `0.86.0-dev` to
`0.86.0-metal` necessarily changes the binary. "Stable across rebuilds of
unchanged source" is the claim, and a version bump is a source change — the
release artefact was re-verified by `release-verify` on its own account rather
than inheriting that boot's result.

## WHAT LANDED

### The journal commit is preemptible — and the first answer was wrong

v0.85 shipped with this in KNOWN-NOT-FIXED: *"nothing tests a writer preempted
while the journal transaction itself is mid-flight."*

Reading the lock suggested the case was unreachable. `klock_acquire()` opens with
`klock_irq_save()`, which is a `cli`; the append path holds `g_vfs_lock` across
the commit; therefore the timer tick that would preempt a writer cannot fire.
That reads correctly, and it is **wrong**. `klock_acquire` also *closes* with
`klock_irq_restore()`, under the comment "check done: reopen IF". The mask covers
the rank-stack check only, never the critical section.

The first assertion written here said *every journal commit runs with interrupts
disabled*. It **failed on its first boot: 9 of 9 commits interruptible.** The
defect was in the reasoning, not the kernel.

Both the wrong version and its correction are left in the code comment and the
roadmap rather than quietly rewritten, because "an argument about a guard is
worth less than a measurement of it" is the convention this tree keeps
legislating, and this is what it looks like when it pays out.

**What is true is more useful than what was claimed.** A writer *is* preempted
mid-transaction, routinely, and the append phases still lose nothing — which
locates O_APPEND atomicity in **mutual exclusion on `g_vfs_lock`, not interrupt
masking**. A future change that "fixed" an append race by masking interrupts
would be treating a symptom.

`vj_publish()` — the single choke point every transaction passes through — now
samples `RFLAGS.IF`, behind four checks ordered so each later one means
something: the workload reached the journal (detection); the sampler can report
interrupts-on at all (positive control, taken outside every klock); the commits
really were preemptible (**premise guard** — if the journal ever runs under `cli`
this fires, and it should, because the oversubscription phases would silently
stop testing a preemptible path); and every appended byte reads back correct
anyway, verified by re-reading the file rather than trusting per-call return
codes, since a lost append is exactly the failure that reports success on every
call.

No `sti`-inside-the-critical-section reproducer was written: the timer ISR takes
locks, so that is a real hazard rather than a test of one, and this project has
already learnt it costs a `TRUNCATED` boot and no verdict.

### `APPSMP_OSRATIO` — the oversubscription ratio stops being a constant

v0.85 measured O_APPEND under 2:1 oversubscription and recorded the ratio as a
gap. 2:1 is the mildest oversubscription that exists, and the `2` was spelled out
in four separate expressions, so **no build in the tree could reach a defect that
needed three runnable writers per core.**

`APPSMP_OSRATIO` now parameterises it; the phase derives its buffer, worker count
and per-pattern expectation from it. The **default stays 2**, so gate cost and
assertion counts do not move, and a higher ratio is a build-time override
(`make EXTRA=-DAPPSMP_OSRATIO=4`) — the same opt-in idiom the reproducers use.

Verified at 4:1 on smp4-bios:

| | ratio 2 | ratio 4 |
|---|---|---|
| workers / cores | 8 / 4 | **16 / 4** |
| file size | 4096 B exact | **8192 B exact** |
| interleave transitions | 149 | **441** |

Three times the interleaving, which is the point: the parameter buys measurable
preemption pressure rather than a bigger number.

### In-RAM per-block generation table (`g_cas_gen`)

The v0.86 spike asked whether a generation could live in the dirent or the CAS
index slot. **It cannot, and not marginally:**

| structure | size | enforced by | free |
|---|---|---|---|
| `struct dirent` | exactly 256 B | `_Static_assert` | `reserved[4]` — **4 bytes** |
| `struct cas_islot` | 16 B | `CAS_SLOTS_PER_BLOCK = 512/16 = 32` | 0 |
| indirect block | 64 × `uint64` | exactly 512 B | 0 |

Per-chunk generations need `VFS_MAX_CHUNKS × 2 B = 32 bytes` against 4 available
— short by 8× — and would cover only the direct map, since indirect blocks are
exactly full. Only 16 and 32 divide 512, so `cas_islot`'s sole growth path halves
the slots per block and makes every existing volume unreadable.

So it is **derived, not stored**: `g_cas_gen[CAS_REF_MAX]`, 128 KiB, zeroed by
`cas_refs_rebuild()` — the one function every path that establishes a directory
already calls. No dirent change, no `cas_islot` change, no format break; every
existing volume still mounts. The same reasoning v0.84 used for the refcounts: a
table rebuilt from the journaled directory cannot drift out of step with it.

One bump per completed free, placed at `g_cas_blocks_freed++` because that is
where the **journaled and legacy branches converge** — beside either `bm_free` it
would miss the other, and any earlier it would count frees `cas_index_remove`
refused. Wrap is counted and folded into the equality rather than dismissed;
65536 frees of one block will not happen, but "will not happen" is the assumption
class this tree has been burned by.

**Two invariants the refcounts cannot state, and both have been watched failing:**

- **Synchronisation** — `sum(gen) + 65536×wraps == bumps == blocks_freed`. Three
  counters kept at different points in `cas_free()` for different reasons. Exact
  equality, so there is nothing to interpret when it breaks. Holds across
  concurrent unlinks because all three mutate only under `g_cas_lock`.
- **Structure** — no block with `refs > 0` may have a clear bitmap bit. A block
  freed while a live dirent still names it is the corruption the reference model
  exists to prevent.

Measured clean: 231 freed, 231 bumps, `sum(gen)` 231, 0 wraps, 468 referenced
blocks checked, 0 referenced-but-unallocated.

**The falsification matrix is the result, not the passing run.** Each invariant
fires only for its own defect class, which is what shows they measure different
things rather than duplicating each other:

| build | synchronisation | structural |
|---|---|---|
| `-DCAS_RECLAIM_REPRO` | PASS (231 = 231 = 231) | **FAIL** — 2 referenced-but-unallocated |
| `-DCAS_GEN_FALSIFY` | **FAIL** — 202 bumps vs 231 frees | PASS |

`CAS_RECLAIM_REPRO` already existed for the dedup control, so the structural
check needed no new reproducer. `CAS_GEN_FALSIFY` is new because **nothing**
falsified the equality, and an equality never watched to fail is one nobody has
checked. It skips every 8th bump rather than all of them — a uniform skip leaves
`sum(gen) == bumps == 0`, satisfying the equality while recording nothing. The
arithmetic confirms the mechanism: 231 frees, every 8th skipped, is 29 skips;
231 − 29 = 202, which is what the falsified run printed.

### tmp `SEEK_END`: the reported debt was not the real one

v0.86 was asked to patch tmp `SEEK_END` against "uninitialised memory disclosure
past true EOF" and to add assertions that reads past EOF return 0. **No such
defect exists, and both assertions were already present.** Verified in the tree:

| path | behaviour |
|---|---|
| `tmp_read_range` | returns 0 at/after EOF, clamps to `flen` — no byte past the end is ever copied |
| `tmp_write_at` | zero-fills the hole when writing past EOF |
| `tmp_truncate` | zeroes on grow **and** shrink, with a comment naming this exact risk |

Existing ring-3 coverage: **1682 / 1684** *"a tmp read past EOF did not return 0"*,
and **1706** `/* HOLE reads zero */`, which checks the gap bytes rather than only
that the file grew.

**The real v0.85 item is different in kind.** `SEEK_END` on an *inherited* tmp
descriptor discloses that file's **length** — metadata, not memory — and that is
POSIX-correct: permissions are checked at `open()`, not per operation, so a
descriptor legitimately opened by its owner and inherited across a fork may still
seek. The tree asserts this deliberately (**exit 1793**, *"lseek must not be
permission-checked"*).

Closing it would mean permission-checking `lseek`: breaking POSIX semantics and
tripping that assertion, a real regression traded for a metadata leak that
matches the root volume's own behaviour. **It stays in KNOWN-NOT-FIXED**, now
with the reasoning written down instead of implied. Writing a "fix" would have
produced a diff that changed no behaviour and an assertion duplicating one that
already passes.

### The ISO md5 does not identify the code

Found while checking that the tree still built what had just been tested. Three
builds of identical source produced three ISO checksums while the binaries inside
were bit-identical every time — `grub-mkrescue` embeds build metadata.

This does not weaken the stamping rule, whose purpose is to name *which image* a
log came from. But the image md5 cannot answer the similar-looking question "is
this the same code as the run that passed", so a mismatch across a rebuild is not
evidence of a change. **Use the kernel ELF md5 for that**, which is why v0.86
publishes both.

## GATE

Full six tiers on image `fc4421e2c3de94384090a3f007ec17f9`, before the version
bump:

| tier | v0.85 | v0.86 |
|---|---|---|
| uniprocessor | 530 | **534** |
| smp2-bios | 544 | **548** |
| smp4-bios | 548 | **552** |
| smp4-iommu | 561 | **565** |
| `gate-dirty` (3 boots, one image) | 0 failing | **0 failing**, empty diffs |
| `gate-dirty-smp` (3 boots, `-smp 4`) | 0 failing | **0 failing**, empty diffs |

Zero rank faults throughout; clean build with no warnings. Durable cross-boot
artefacts intact on every dirty boot.

**Unreferenced blocks held at 2 across all three dirty boots and did not grow**,
which is the leak signal that matters — a bare non-zero value is explained, since
`cas_put()` is reachable without a dirent and the boot suites use it that way.

A later uniprocessor boot at **537** carries the three generation assertions
added after the gate.

NOT run for this tag: bare metal, Proxmox, soak/repeat beyond the boots above.
This cannot see an intermittent below roughly 1 in 10 boots on the fresh tiers.

## KNOWN, NOT FIXED

- **The generation table and the TALLY sweep are both per-boot.** Each is rebuilt
  at mount, so corruption from an earlier boot of a reused volume is erased
  before either can see it. Closing that needs a durable generation, which needs
  the on-disk format break the spike declined to spend on a diagnostic.
- **`lseek`/`SEEK_END` on another user's tmp descriptor discloses that file's
  length.** POSIX-correct and deliberate — see above for why closing it would be
  a regression.
- **VOL_TMP has no mode and no group.** Deliberate; owner-or-root is the whole
  rule, so a tmpfile cannot be shared.
- **No crash injection on the PUT path**, which has always had the consistent
  ordering, and none inside the legacy pre-journal branch of `cas_free`.
- **Three of the four journal-irq checks have not been observed failing.** The
  premise guard's counter has (at both 0 and 9); the detection, positive-control
  and byte-integrity checks are regression guards and are described as unproven
  rather than verified.
- **The oversubscription ratio is now tunable but the gate still runs at 2:1.**
  4:1 was measured once, by hand, and is not part of any gate tier.
