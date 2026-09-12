# Development cycle summary — post-v0.96, unreleased

Three commits on `main` since `b7581dd`, verified together on one image.

    d798707  fix(vault_pad): stop no-op edits destroying undo; derive dirty from saved text
    23f6b75  perf(cas): write only the dirty bitmap blocks; audit the bitmap against disk
    44430b4  test(vfs): crash-test the deferred append journal; raise GATE_DIRTY_CAP to 900

    apps/vault_pad.c           | 125 +++-
    metal/kernel/kernel64.c    | 311 +++++++-
    metal/tools/cfm-measure.sh |  32 +
    metal/tools/gate-dirty.sh  |  15 +-
    4 files changed, 474 insertions(+), 9 deletions(-)

---

## 44430b4 — crash-test the deferred append journal

Phase 6 deferred the append journal but nothing crash-tested it. `cascrashwrite`
now deliberately stages the dangerous state — base applied, N appends deferred,
halt with the epoch unpublished — and the NEXT boot asserts recovery landed on a
chunk boundary, that the length sits between the durable base and everything
staged, that every chunk the dirent still names resolves AND reads back
byte-exact, and that volume-wide index/bitmap agreement holds.

Measured on a reused volume: the file came back 512 bytes = 1 chunk (base 1,
staged 8), every named chunk resolved, bytes exact, dangling 0.

**`GATE_DIRTY_CAP` 480 -> 900.** The cap is the deadline for boot 1 to REACH THE
PROMPT before the harness types into it, and boots now take ~650 s. The failure
was silent in the worst way: missing the window meant boot 1 never ran
`cascrashwrite`, so no artefact was created and boots 2 and 3 correctly asserted
NOTHING. `make gate` never types, so the fresh matrix stayed green throughout.

## 23f6b75 — dirty bitmap ranges

`cas_flush_meta()` rewrote EVERY bitmap block on every call; a single put dirties
exactly one. `bm_set`/`bm_free` now record a `[lo,hi]` range and the flush writes
only that span. The superblock always moves, because `used_blocks` and the
put/dedup counters change on paths that dirty no bitmap bit.

Same ISO before and after, `vfsappend 256`, identical control (671 puts = 20
dedup + 651 stored, 1,082 flush calls):

| | before | after | |
|---|---|---|---|
| writes | 10,319 | 9,209 | -10.8% |
| transactions | 14,977 | 13,870 | -7.4% |
| flush blocks | 3,246 | 2,144 | -33.9% |

Three corrections to the plan this came from, all found by measuring:

- flush_meta was **31%** of writes, not the 97.5% claimed. A 4 MB volume has TWO
  bitmap blocks at 512 B/block, so the flush cost 3 writes, not ~40.
- dedup hits were **20 of 671**, not ~500 — the large figure is the superblock's
  LIFETIME counter, not the benchmark window. Skipping the bitmap on dedup is
  subsumed by the range being empty.
- `cas_put` is not the main caller: **411 of 1,082 calls (38%)** come from the
  free/unlink path. Scoping the function rather than its call sites covers them.

Worth much more as volumes grow: at 64 MB (`bitmap_blocks` 32) the same call
count costs ~34,000 block writes today.

**New assertion, and why it was needed.** `cas_mount()` re-reads every bitmap
block from disk, so a mutator that edits `g_bitmap` without marking its block
leaves that block unwritten and the edit VANISHES at the next mount — returning a
bitmap that calls blocks free while the index still names them. The standing
`used_blocks == popcount(bitmap)` audit cannot see it: both sides are in RAM.
Proven by negative control — a build with `bm_free`'s mark removed PASSES every
assertion in cio, popcount included, on a fresh boot. `cio` now flushes and
compares every bitmap block against the DISK; on that same broken build it
reports "1 of 2 block(s) differ" and fails.

## d798707 — vault_pad undo correctness

Five keyboard-reachable defects, each exposed by the new `undo_noop` test:

1. **A no-op edit destroyed the undo slot.** `vp_key` checkpointed before knowing
   the edit would happen, but `vp_delete` returns early with nothing to delete.
   A backspace at column 0 overwrote undo with a snapshot of UNCHANGED text,
   silently discarding the edit the user wanted back.
2. **Undo always claimed dirty**, so undoing back to saved content still showed
   "* MODIFIED" and blocked File > New. `dirty` is now DERIVED from a clean
   baseline (`clean_text`/`clean_len`, set by init/open/save).
3. **Undo/redo did not withdraw a pending overwrite question** — they return
   early, upstream of the general `confirm=0`.
4. **Nor did a mouse click** — `vp_click` never touched `confirm` at all.
5. **File > New inherited the old document's undo history**, so one undo could
   resurrect the previous file's text into a buffer believed empty, and a save
   would write it under the new name.

Also: Save-as focused the filename field but left command mode set, so the first
character typed was read as a command (`x` cut, `p` pasted).

`term_menu_action` needed no work — it is implemented at `outrun_term.c:216` and
the test drives the SAME function the compositor's menu hit-test calls at :317.

---

## Verification

Clean rebuild from scratch, **zero compiler warnings**.
Image `outrun-os-1.0.0.iso`, md5 **3953390decf251c1002bac4b61fd4127** — the same
binary for every run below.

`make gate` (gate-selftest 17/17 first):

| tier | suites | passed | failed | ranks | time |
|---|---|---|---|---|---|
| uniprocessor | 47 | 609 | 0 | 0 | 640 s |
| smp2-bios | 47 | 623 | 0 | 0 | 585 s |
| smp4-bios | 47 | 629 | 0 | 0 | 610 s |
| smp4-iommu (q35 + VT-d, intremap=on) | 49 | 654 | 0 | 0 | 610 s |

On all four tiers: `wimpstrs` 39 passed / 0 failed with the minimize box green,
and `bitmap durability: 0 of 2 block(s) differ`.

`make apps-test`: all host suites pass, including `outrun_term` and `vault_pad`.

## Not covered

- **Bare metal and Proxmox.** Every number here is QEMU under TCG.
- **Soak and repeat runs.** One boot per fresh configuration, three per dirty
  configuration: this cannot see an intermittent below roughly 1 in 10 boots.
- **Volumes large enough to matter for 23f6b75.** All runs used the 4 MB volume
  (`bitmap_blocks` 2). The 64 MB projection is arithmetic, not measurement.
- **No release ISO was built or `release-verify`'d.** This is a development
  baseline, not a tag. `VERSION` still reads 1.0.0 and was not bumped.
- **Uncommitted work is excluded.** The desktop/WM changes, the web application
  and its vendored mbedtls were stashed for every run above and are NOT part of
  this baseline.
