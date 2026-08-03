# OutRun OS v0.66.0-metal — memory that is a file

Milestone 66. File-backed `mmap`, a page cache that makes `MAP_SHARED` mean
something, writeback through `msync`, and the closing of a latent descriptor
hole found while tracing v0.65.

## THE CATCH-ALL THAT ANSWERED FOR EVERYTHING

`SYS_READ` and `SYS_WRITE_FILE` dispatched on volume through a chain ending in
a bare `else` labelled `VOL_DEV`. That label was a comment, not a branch: an
epoll instance or a socket — neither of which is a byte stream either call can
serve — fell through it and was answered with **device bytes**.

A read that returns plausible-looking data for the wrong object is worse than
one that fails, because nothing downstream can tell. Both chains are now
exhaustive: every volume names itself, `VOL_SOCK` and `VOL_EPOLL` are refused
with `-EINVAL` pointing at the call that does work, and an unrecognised volume
is an error rather than the last branch. `mmapfilestrs` checks it from ring 3,
since the whole point is that userland could not previously tell.

## THE PRINCIPLE THIS MILESTONE COULD NOT KEEP

v0.63 established that **the page table is the map**: a page's nature rides in
its own PTE and there is no VMA list to keep consistent with it. File mapping
is the one thing that cannot express, and it is worth being explicit rather
than quietly adding a second description of the address space.

A fault on a file page has to answer *which file, and which offset*. For a
**non-present** entry there is room — the hardware ignores every bit. For a
**present** one there is not: bits 9–11 are already `ZFOD`/`COW`/`SHM`, and
52–62 is eleven bits, nowhere near a file id plus a page index. And a shared
page must be findable by `(file, offset)` from a *second process with entirely
different page tables* — a per-PTE encoding cannot be looked up that way even
in principle.

So this adds exactly two bounded tables:

| table | what it answers |
| --- | --- |
| `g_fmap[8]` | which user range names which file. Consulted **only** on a fault the PTE could not resolve, so the ZFOD and COW fast paths never touch it. |
| `g_pcache[32]` | `(dirent, page)` → frame. This is what `MAP_SHARED` *is*. |

**Neither is authoritative about permissions — the PTE still is.** They answer
"what is behind this page", never "may it be written", so v0.63's invariant
that a page's rights are readable from its entry alone survives intact.

## SYSCALLS

| # | call | notes |
| --- | --- | --- |
| 83 | `SYS_MMAP_FILE(fd_prot_flags, length, offset)` | fd/prot/flags packed into `a0`; offset gets a register of its own |
| 84 | `SYS_MSYNC(addr, length, flags)` | `flags` accepted and ignored — see below |

A separate call rather than more arguments on `SYS_MMAP`: the dispatch ABI has
three registers, anonymous mmap already uses all three, and packing a
descriptor into one would make the anonymous path pay for a feature it does not
have. **v0.63's refusal stands** — 71 is still anonymous-only and still says so
rather than accepting a descriptor it would ignore.

`MS_ASYNC` is not implemented and `flags` is documented as ignored rather than
validated, because the write goes straight through `vfs_write_by_dirent` with
no queue behind it. Accepting `MS_ASYNC` would be a promise about ordering that
nothing here keeps.

## WRITEBACK, AND WHY IT REWRITES THE WHOLE FILE

The VFS is **content-addressed**: a file is a list of immutable chunk hashes,
and writing it means producing a new list and repointing the name. There is no
such thing as writing one page in place. A flush is therefore unavoidably a
read-modify-write of the whole file — read it, overlay the dirty pages, write
it back once. That is not a shortcut; it is what the storage model permits, and
it is why `FMAP_MAX_BYTES` (64 KiB) exists to bound it.

The file **keeps its length**. A mapping is rounded up to whole pages and the
tail past EOF reads as zero; writing that back would silently grow every mapped
file to a page multiple, which `msync` does not do and which would corrupt the
content the suite then compares.

### The bug that made dirty tracking real

Shared writable pages are installed **read-only** even when the mapping is
writable, so the first write faults — that fault is the only moment this kernel
can observe a page becoming dirty. x86 has a hardware dirty bit, but it lives
in the PTE of whichever process wrote, and a cached page is shared between
processes that each have their own tables.

The first implementation cleared the dirty flag on flush and **left the pages
writable**. The suite caught it immediately, and the failure is worth recording
because it is silent by construction: after an `msync`, a later store — in this
process or in a child that inherited the entry — completes in hardware with no
fault, no dirty mark and no trace. The next flush finds nothing to do and the
data is gone at unmap. **Cleaning and write-protecting have to happen together
or the tracking is decorative.** `fmap_writeprotect` now runs with every flush.

Only the caller's own address space is re-protected. A second process mapping
the same file has its own tables and its own `fmap` entry and re-protects on
its own flush; there is no reverse map from frame to PTE in this kernel, and
building one to cover a case nothing exercises would be machinery without a
consumer.

## FORK

A child inherits its parent's file mappings, not just its page tables. Without
that its already-faulted pages would be present (shared ones exempt from COW,
private ones copied) but any page the parent had **not** yet touched would
fault in the child, find no range naming a file, and be filled with zeroes — a
mapping that is file-backed at the start and anonymous from wherever the parent
happened to stop reading.

An exiting process flushes before its mappings are dropped, so a program that
never calls `msync` does not lose its writes. Cached pages are not dropped with
the process: they belong to the file, and the frame refcount decides when the
last reference goes.

## VERIFICATION

41 suites, 0 FAIL on uniprocessor/BIOS, SMP-4/BIOS, and q35 + VT-d IOMMU
(`-smp 4`). Each configuration differs from its v0.65 baseline by exactly one
line — `[mmapfilestrs]`. Boot logs are in `docs/`.

**The suite owns its fixture.** v0.65 lost most of a milestone to a verification
suite that corrupted a file eleven other suites depended on, so `mmapfilestrs`
gets its own `m66dat` and touches nothing else. It is seeded *before* the suite
battery, because the toolchain suites fill the directory mid-boot and by suite
41 there is no room left.

The pattern is `(i*7+3)&0xFF`: not constant within a page and not repeating
across pages, so a mapping that returned zeroes — or returned page 0 for every
page — produces a **wrong answer** rather than accidentally agreeing. The
assertions reach into page 3 for exactly that reason.

What ring 3 proves: private mappings start from file content and their writes
never reach the file; shared writes reach it only when asked; `msync` output is
visible to a reader that never mapped anything; W^X, unaligned offsets and bogus
descriptors are refused; and an epoll descriptor now fails `read`/`write`
instead of returning device bytes.

What only the kernel half can see: that pages arrived **from the file** rather
than being eagerly copied (`g_file_faults` moved), that **the page cache served
a second mapper** (`g_pc_hits` moved — the child maps the file itself rather
than reusing the inherited range, because an inherited mapping would agree even
if every mapper got a private copy), that writeback happened, that the file kept
its length, and that no mapping outlived its process.

### VFS_MAXFILES: 64 -> 96

Seeding one fixture file broke `langstrs` on SMP-4 — it could no longer author
its refusal-test source. That is not a bug in either suite: **the tree ran out
of names.** `[vfs] directory full` has appeared mid-boot in every log since
v0.60 and had been carried as a known deferral for six releases; v0.66 is where
it stopped being a warning and started failing verification.

Raising it needs no format break, which is why it is done here rather than
deferred a seventh time. `VFS_DIR_BLOCKS` is derived from the constant and
**recorded in the superblock** — v0.48 made it dynamic for exactly this reason
— and mount reads `min(SB->dir_blocks, VFS_DIR_BLOCKS)`, so a volume written by
an older kernel still mounts with its own smaller directory. The version gate is
untouched and the static directory image grows by 8 KiB.

`[vfs] directory full` no longer appears in any boot log.

### Warnings

46, all pre-existing; this release adds none. One was added and removed during
development — a nested `/*` inside a comment — rather than left to sit.

### Not done

- **No shared-mapping coherence with `write()`.** A process that `write()`s a
  file another process has mapped shared does not invalidate the cached pages.
  The page cache has no invalidation path at all; adding one needs the VFS to
  notify on write, which is a VFS change, not a VM one.
- **No page-cache eviction.** 32 entries, first-fit, and a full cache makes the
  next shared fault fail rather than reclaiming. Bounded and honest; an LRU
  would be machinery with no pressure behind it yet.
- **`FMAP_MAX_BYTES` is 64 KiB** and a mapping may not span more, because
  writeback stages the whole file.
- **No `MAP_FIXED`, no `mremap`, no partial `munmap`** of a file mapping — an
  unmap that does not cover the whole range still drops the `fmap` entry.
- **The directory bump is capacity, not a redesign.** 96 entries removes today's
  pressure; the directory is still a flat fixed array with a linear scan, and a
  system that wanted thousands of names would want a different structure rather
  than a larger constant.
