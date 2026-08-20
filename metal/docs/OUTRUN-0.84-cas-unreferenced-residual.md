# The 2-block CAS residual — root-caused, and not a leak

`OUTRUN-0.84-cas-block-reclamation.md` closed with an unexplained item: after
reclamation landed, the crash-test remount reported 459 data blocks allocated
against 457 referenced. Two blocks, stable rather than growing, leak-direction,
underflow zero everywhere — and no explanation. It guessed at "a block put by an
operation that was rolled back". That guess was wrong.

## What it actually is

**`cas_put()` is reachable with no VFS involvement**, and the reference model
counts dirent references and nothing else. Content stored directly in the CAS is
therefore unreferenced *by construction*, not by defect. Three boot-suite call
sites do exactly that:

| site | what it stores | unreferenced blocks |
|---|---|---|
| `cmd_cas` | `"Hello, Outrun CAS! The content is the address."` | 1 |
| `cmd_cas` | `"A completely different block of bytes entirely."` | 1 |
| validation matrix | 512-byte `blob`, `(i*7+3)&0xFF` | **0** |
| | | **= 2** |

`cmd_cas` prints its own contribution and always has:

```
[cas    ] dedup: hash(a)==hash(a')? YES ; blocks used this run +2
```

**The validation blob contributes nothing, and that is the part that made the
arithmetic look wrong.** A first reading predicted three unreferenced blocks and
the measurement said two. The blob is 512 bytes of `(i*7+3)&0xFF` — and
`m66dat`, the v0.66 mmap fixture, is a 16 KiB LIVE FILE seeded with the same
expression. The blob is byte-identical to that file's chunk 0, so its `cas_put`
is a dedup hit onto a block a live dirent already references. It allocates
nothing and is counted as referenced.

That is dedup behaving exactly as designed, and it is why the number could not
be predicted by reading call sites alone.

## Measured

Six-tier gate on image `ee045a3b004f568716893a21973e188f`, ten boots on that one
image, zero failing assertions and ranks 0 throughout: uniprocessor 507,
smp2-bios 519, smp4-bios 523, smp4-iommu 536, plus `gate-dirty` and
`gate-dirty-smp` (3 boots each, empty consecutive-boot diffs). Every fresh tier
is +1 against `a881542` — the single new budget assertion. Clean build, no
warnings under `-Wall -Wextra`.

The reconciliation itself, from the earlier uniprocessor run on image
`4d59645d9f795a52faaa4e47e04eee2f` (45 suites, 507 passed, 0 failed):

```
[cas    ] refcounts derived from 0 live file(s): 0 data block(s) allocated, 0 referenced, 0 UNREFERENCED; volume 648/8192
[cas    ] dedup: hash(a)==hash(a')? YES ; blocks used this run +2
[cas    ] refcounts derived from 46 live file(s): 459 data block(s) allocated, 457 referenced, 2 UNREFERENCED; volume 1107/8192
[vfsstrs] cas residual: 2 allocated data block(s) unreferenced by any live file (budget 32; ...)
```

## Why the number was ambiguous in the first place

It was never printed. It was obtained by subtracting two figures from different
lines of a boot log, using a `data_start` carried by hand from a third. A
quantity nobody can read directly is a quantity nobody checks — the same shape
as the counters CLAUDE.md already warns about, one step further out: not a
counter nothing increments, but a difference nothing computes.

`cas_refs_rebuild()` now prints allocated / referenced / unreferenced on one
line, so the subtraction is gone and with it the ambiguity.

## What is asserted, and what deliberately is not

`vfsstrs` asserts the unreferenced count against a **budget**
(`CAS_UNREF_BUDGET`, 32), not an exact value.

An equality against today's 2 would encode the current demo suites' behaviour.
The first suite to add or remove a direct `cas_put` — or to change a fixture's
byte pattern, which is all it would take here, given the blob's dedup depends on
`m66dat` using the same expression — would turn the gate red for a reason that
is not a defect. A test that fails for being out of date teaches people to edit
the number rather than read it.

What is worth defending is the SHAPE: a small, steady figure is the boot suites'
own direct CAS content; a figure climbing into the hundreds is a real
reclamation leak. The budget catches the second and tolerates the first.

**The stronger signal is growth across boots**, which no single boot can see. On
a reused volume identical content dedups to the same blocks, so this figure is
stable by construction — `gate-dirty` running three boots on one image is where
a climbing value would show, and its consecutive-boot assertion diff is the
existing mechanism for noticing.

## Not covered

- The budget is a bound, not a model. Nothing verifies that the unreferenced
  blocks ARE the ones named above; it verifies only that there are few of them.
  Enumerating them by block number would need the index walked in reverse
  (block -> hash), which the format does not support.
- No boot-over-boot comparison of this figure is automated. The dirty gate would
  surface a change through its assertion diff only if the count crossed the
  budget.
