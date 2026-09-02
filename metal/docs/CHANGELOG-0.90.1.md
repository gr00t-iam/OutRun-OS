# OutRun OS v0.90.1-metal — append-oversub test harness patch

A **test-only** patch release. No kernel behaviour changes: the VFS lock
decoupling shipped in v0.90.0 is untouched, and no code outside
`cmd_vfs_stress`'s append phase and one shell harness was modified.

It exists because v0.90.0's `append-oversub` phase could report a lost append
that never happened, and — more quietly — could skip its own corruption checks
on exactly the runs where corruption was most likely.

## ARTEFACT

| | |
|---|---|
| artefact | `outrun-os-0.90.1.iso` |
| md5 | `3c946a834b6284ee81c5aa5ae4949ffd` |
| sha256 | `ee1ecfc8ae2b56e7089b29f02f75a225d5355c257c87348dc73e2551e50e328c` |
| `release-verify` | **PASS** — 45 suites reporting, 0 failing assertions, 0 rank faults, 450 s |

Built from a clean tree by `make release`, checksummed, and booted from a fresh
volume before these numbers were written down. The `.md5` and `.sha256` files sit
beside the image in `build/release/`.

**The ISO is not reproducible and `make clean` destroys it** — measured during
v0.90.0, where two builds of one unchanged tree gave different checksums because
`grub-mkrescue` embeds non-deterministic data. A checksum names one artefact and
never a source state, so "rebuild it and compare" is not a recovery path and
`build/release/` should be treated as output only `make release` creates.

## WHAT WAS WRONG

`append-oversub` puts `2 x cores` unaffined writers on one file with no
user-space synchronisation, and checks that no byte is lost. Each worker carries
a **deadline** rather than a spin budget, and expiring on it is deliberately
tolerated:

> *"no worker FAILED — deadline expiry is counted separately and tolerated,
> because a slow host must not be decoded as a defect"*

Six lines later the phase asserted:

```c
flen == total    /* total = nw x APPSMP_ITERS x APPSMP_PAY */
```

**Those two claims cannot both hold.** A worker that expires writes fewer
appends, so the file is legitimately shorter than the nominal total — and the
assertion then reports a lost append as a kernel defect.

Measured over 8 runs on a degraded host across two builds:

| condition | file | assertion |
|---|---|---|
| 7 runs, a worker expired | 7712–8128 B (short by 64–448 B) | **FAIL** |
| 1 run, none expired | 8192 B exactly | PASS |

Every deficit was under 512 B — the most a single expired worker can owe
(32 iterations x 16 B). The correlation is exact.

### The kernel was never losing appends

`SYS_WRITE_FILE` routes an `O_APPEND` write to `vfs_write_append`, which takes
the offset from the dirent **inside** the lock that performs the write; the v0.84
note there describes precisely the race that design prevents, and
`-DAPPEND_RACE_REPRO` exists to reintroduce it deliberately.

The three hypotheses this was investigated against are each ruled out by
evidence rather than argument:

| hypothesis | why not |
|---|---|
| APIC timer interval drift | the deadline is measured in guest ticks via `SYS_SYSINFO`, not wall clock |
| spinlock acquire latency | `g_vfs_lock` exclusive contention has been **0** on every tier since v0.90.0's optimistic probe |
| missed barriers at context switch | **zero** torn blocks and **zero** alien patterns in every run — bytes that arrived were never interleaved or half-written |

A negative control had already pointed the same way before the root cause was
found: the failure was **more** frequent on a pre-v0.90 build with no shared
acquisition at all (4 of 4, against 3 of 4).

## WHAT CHANGED

**1. The expectation now depends on who finished.** Exact when nothing expired —
`n_slow == 0` reduces it to the original `flen == total`, unweakened, which is
what every healthy run hits — and bounded by what expiry can account for
otherwise. A deficit larger than the expired workers could owe is still a lost
append and still fatal.

**2. The byte-level checks run again on partial writes.** The torn-block,
alien-pattern and interleave checks sat inside `if (got == total)`, so a single
expired worker skipped all three — the run lost its corruption coverage exactly
when the host was most loaded and concurrency most stressed. They now run on what
was actually written.

They are bounded by bytes **read**, not by the nominal total: scanning to `total`
over a short file would walk past what `vfs_read_file` produced and count
uninitialised buffer as torn blocks, manufacturing the very defect it looks for.

The per-pattern counts follow the same rule — exact equality when nothing
expired, an upper bound otherwise (a pattern appearing *more* often than its
workers could have written it is corruption either way), with the totals still
required to sum to the bytes on disk.

**3. `tools/vfs-soak.sh` counter evaluation.** `grep -c` prints `0` **and** exits
1 when it matches nothing, so `|| echo 0` appended a second value and the
comparison saw `"0\n0"`. Harmless to the verdict, wrong as written, and noisy on
every iteration.

## VERIFICATION

**Falsified before it was believed**, per this tree's standing rule that a test
which cannot fail has not passed. Built with `-DAPPEND_RACE_REPRO`, which splits
the append across two critical sections and genuinely loses writes:

```
file is 2336 B (want 8192), read 2336
FAIL  append-oversub: no append was LOST under preemption
```

The loosened assertion still catches real loss. No worker expired in that run, so
`owed == 0` and it demanded strict exactness — the original strength intact.

**Both branches observed, not merely compiled.** The four-tier matrix exercised
only the strict path (no worker expired at any width), so the tolerant path was
run separately on the host that produced the original flake:

| run | expired | deficit | owed cap | verdict |
|---|---|---|---|---|
| 1 | 1 of 16 | 160 B | 512 B | PASS |
| 2 | 1 of 16 | 448 B | 512 B | PASS |
| 3 | 1 of 16 | 464 B | 512 B | PASS |
| 4 | 1 of 16 | 480 B | 512 B | PASS |

Soak failing assertions on that host: **3 → 0**.

**Regression matrix**, fresh image per boot, image
`aea65027e294c835f318d56a552bf23c`:

| tier | passed | failed | ranks |
|---|---|---|---|
| uniprocessor | 563 | 0 | 0 |
| smp2-bios | 577 | 0 | 0 |
| smp4-bios | 583 | 0 | 0 |
| smp8-bios | 583 | 0 | 0 |

## NOT COVERED

Said plainly, because a gate whose gaps are invisible is how "verified" drifts
from "measured":

- **`gate-dirty` / `gate-dirty-smp` were not re-run for this patch.** They passed
  at all four tiers for v0.90.0 and this change touches only assertion logic in
  one phase, but that is an argument, not a measurement.
- **`smp4-iommu` was not run**, as in v0.90.0.
- **The 200-iteration isolation soak was not run.** On this host a `vfsstress`
  boot is ~700 s and an iteration ~400 s, so 200 iterations is roughly 22 hours,
  and it would re-measure a failure mode already characterised deterministically
  on a machine already shown to be degraded. The falsification run is the
  stronger evidence and took eight minutes.
- **The host is slow.** Boots reach the prompt in 405–719 s against ~330 s for
  the same tiers earlier in the v0.90 cycle. That is the condition that exposed
  this defect and it has not been explained; it is a property of the toolchain
  host, not of the kernel.
