# OutRun OS v0.89.0-metal — CAS lock contention

**The v0.89.0 tag does not exist yet. Nothing here is a released claim.**
Opened on `main` at `v0.88.0`, with `VERSION` and `KERNEL_VERSION` both moved to
`0.89.0-dev` in the same commit — the Makefile derives its expectation from the
`-dev` suffix and warns when the two disagree.

## ARTEFACT

Not built. This section exists so that the release protocol's four steps have
somewhere to land, and so that an empty one is visibly empty rather than absent:

```
outrun-os-0.89.0.iso   (not built)
MD5    —
SHA256 —
```

## BASELINE

v0.88.0 at `79bd11a`, artefact `e2b212773609dfcab624b88ff9ee7ea5`, kernel ELF
`cc08fca2c4a3cf73a04a5efc4fa02f45`. All six tiers on the published artefact:

| tier | passed |
|---|---|
| uniprocessor | 555 |
| smp2-bios | 568 |
| smp4-bios | 574 |
| smp4-iommu | 587 |
| `gate-dirty` ×3 boots | 0 failing, empty diffs |
| `gate-dirty-smp` ×3 boots | 0 failing, empty diffs |

Plus `release-verify` PASS and four extra `smp2-bios` boots at 568.

## THE OPEN ITEM THIS CYCLE INHERITS

### `g_cas_lock` contention, and the question v0.88 deferred

v0.88 shipped the role-61 contention soak, which puts `2 × n` unaffined workers
through a real `cas_put` and `cas_free` per iteration — index probe, index stage,
`bm_alloc`, journal transaction, index remove, `bm_free`, generation bump, second
journal transaction — from every core at once. It passes, exactly: unreferenced
blocks and `used_blocks` return to precisely their pre-soak values, with zero
underflows and zero dangling index entries.

**What it also did was destabilise a neighbouring suite**, and that is the thread
to pull first.

Adding the soak made two `threadstrs` premise guards fail about half the time on
`-smp 2`. Bisected on one host inside one window: v0.87.0 4/4 PASS, v0.88 2/4,
v0.88 with `-DCASC_SKIP` 4/4. The guards were then aligned with `pthreads_smp`'s
`n >= 3` threshold, which is a **loosening** and was approved as one — the
alternatives are recorded below as not taken.

The underlying question is unanswered: **is the guard simply fragile at two CPUs,
or does the soak leave cpu 1 halted and slow to pick up work?** Those have
different fixes, and only one of them is a defect.

Three options were on the table at v0.88.0. Option 1 was taken:

1. **Align `threadstrs` with `pthreads_smp`** — done in v0.88.0. Cheapest, and it
   weakens an assertion.
2. **Run the soak after `threadstrs`** — restores the conditions the `n >= 2`
   guard was validated under without weakening anything, but only moves the
   fragility out of sight.
3. **Investigate the scheduler state the soak leaves behind** — whether the drain
   returns the machine to a balanced state, or leaves the BSP holding everything
   while the AP idles. The most useful answer and the most expensive.

**Option 3 is the v0.89 item.** If the drain is leaving the machine unbalanced,
that is a scheduler observation worth having on its own account, and it would let
`threadstrs` go back to `n >= 2` on evidence rather than on hope. If it is not,
the honest outcome is to say so and leave the guard where it is — which is a
result, not a failure, and is the same shape as v0.87's `SYS_FSTAT` prediction
and v0.86's journal-interrupt claim.

`-DCASC_SKIP` remains in the tree as the bisection tool that found this.

## CARRIED FORWARD FROM v0.88

- **The standing index audit cannot see a dangling entry once the block it names
  has been reallocated.** It guards against the state persisting, not against the
  corruption having happened. Only the crash phase's measurement, taken
  immediately after recovery, catches that.
- **The generation table and the TALLY sweep are both per-boot.** v0.87 proved the
  reset is complete; nothing establishes that anything is *lost* by it.
- **The oversubscription ceiling is higher, not absent.** Above 8 cores at 4:1 the
  clamp bites again, loudly rather than silently. `MAX_KPROC = 64` is the real
  wall behind `APPSMP_MAXW = 32`.
- **The contention soak is 12 iterations per worker**, sized to fit the standing
  gate rather than to stress the allocator for minutes. No long-running opt-in
  target exists, and its behaviour above 4 cores is untested — `smp8-bios` is a
  diagnostic and was not run for v0.88.0.
- **`lseek`/`SEEK_END` on another user's tmp descriptor discloses that file's
  length.** POSIX-correct and asserted deliberately; closed as a question.
- **VOL_TMP has no mode and no group.** Deliberate; owner-or-root is the whole
  rule, so a tmpfile cannot be shared.

## WORKSPACE

`.claude/worktrees/v075-tier2-crypto` was carried through v0.87 and v0.88 without
a decision. It is dated 9 August, is not in `git worktree list`, has no branch,
and git holds no record of its contents — so removing it is a plain filesystem
deletion of files nobody has diffed. **v0.88 is the third cycle to carry it.**
Either delete it this cycle or write down why it is being kept; carrying it a
fourth time is the habit this section exists to prevent.

## FIRST COMMITS OF THE CYCLE

1. `VERSION := 0.89.0-dev` and `KERNEL_VERSION "0.89.0-dev"`, moved together —
   done in the commit that carries this file.
2. A control boot confirming the `0.89.0-dev` banner reaches ring 3, not merely
   the build.
