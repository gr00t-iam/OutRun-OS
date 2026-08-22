# v0.85 VFS metadata — Phase 1 recon

Audit of what exists before anything is written, against `main` @ `298dcec`.
Three findings, one of them a live authority leak.

## 1. `rename` — does not exist

No syscall, no stub, no helper. `grep` for it in `kernel64.c` returns one
unrelated comment. This is greenfield.

**It is NOT a one-dirent operation, which matters under per-block journaling.**
The namespace is flat — names are paths but there is no directory tree — so a
rename is a name overwrite in place. But `rename(old, new)` where `new` ALREADY
EXISTS must replace it atomically, and that touches TWO dirents:

- `DENTS[old].name = new` — the entry being moved
- `DENTS[target].used = 0` plus `vfs_release_map_locked()` — the entry being
  replaced, whose blocks must be returned exactly as unlink returns them, or
  rename becomes a way to leak storage that unlink cannot

Two dirents are two directory blocks whenever `old/2 != target/2`. v0.85's
journal records a SET of dirty blocks, so both must land under ONE header or
the rename is not atomic:

- `vfs_journal_commit_idx(a)` then `vfs_journal_commit_idx(b)` is **not enough**.
  The first writes a header naming only block A; a crash between the two leaves
  the rename half applied — the entry renamed, the target not yet removed, i.e.
  two dirents carrying the same name.
- The `-1` escape hatch (shadow every block, one header) is correct and atomic,
  and costs the old 49 writes. Acceptable for an operation this rare, and it is
  the safe default if nothing better lands.
- **Preferred:** a commit that takes both indices — write both shadows, then one
  header. 3 writes, atomic, and it reuses the slot machinery unchanged.

Note the function is `vfs_journal_commit_idx()`, not `..._index()`.

## 2. `chmod` / `chown` — implemented since v0.72, and reading the WRONG uid

`SYS_CHMOD` (92) and `SYS_CHOWN` (93) are complete: owner-or-root for chmod,
root-only for chown, ENOENT, tmp/dev refused, and already routed through
`vfs_journal_commit_idx(di)` by the v0.85 journal work.

**`ROADMAP-0.85.0.md` says "what is missing is any way to CHANGE them after
creation". That is wrong** and is corrected in this commit. They have existed
for four milestones.

**The real defect is the credential they consult.** Both read `kprocs[L].uid` —
the REAL uid — while every other VFS check uses `cred_euid()`. `vfs_permit`'s
own header says why that matters:

> Passing the real pair would make setuid() cosmetic, since a process could hold
> a privileged effective id the filesystem never consulted (and, symmetrically,
> a dropped one it never honoured).

chmod/chown are v0.72; the effective pair arrived in v0.74 and these two were
never converted. The exploitable direction is live because `SYS_SETEUID` (96)
moves ONLY the effective id — unlike `SYS_SETUID` (90), which moves real,
effective and saved together:

| call | real | euid | `vfs_permit` sees | `chmod` sees |
|---|---|---|---|---|
| root, then `seteuid(1000)` | 0 | 1000 | **1000** — refuses writes to root's files | **0** — permits chmod on ANY file |
| setuid-root binary | 1000 | 0 | **0** — allows | **1000** — refuses chown |

So a process that dropped effective privilege keeps the authority to change the
mode of every file on the volume, and a genuinely setuid-root program is denied
chown. Fixing it is a two-line change to use `cred_euid(L)`; the value is in the
test that proves it, which does not exist yet.

**There is no test coverage for either syscall — kernel-side or ring-3.**
`init.c` carries only the two `#define`s. The kernel assertion that reads

> "after chmod 0600 the stranger can no longer open it at all"

does not call `SYS_CHMOD`; it assigns `DENTS[di].mode = 0600` directly and then
tests `vfs_permit`. The assertion is true and its name is misleading — the same
shape as the v0.82 comment that claimed a role which did not exist.

## 3. `st_mtime` / `st_atime` — absent, and there is room

No timestamp field anywhere; `SYS_STAT` reports none.

`struct dirent` ends with `uint8_t reserved[12]` behind
`_Static_assert(sizeof(struct dirent) == 256)`. Two `uint32_t` stamps take 8 of
those and leave 4, so the on-disk layout, `VFS_DIR_BLOCKS`, the journal record
and `cas_mount`'s restore are all untouched — the same carve-out v0.56 used for
the indirect map and v0.72 used for ownership.

`g_ticks` at 100 Hz is the only clock. A `uint32_t` of ticks covers ~497 days,
far past any boot. These are therefore BOOT-RELATIVE and must be documented as
such: the honest thing is a monotonic tick stamp, the dishonest one is a
fabricated epoch that looks like wall time and is not.

**Compatibility rule, following the one v0.72 wrote for `mode`:** on a volume
written by an older kernel these read as ZERO, which must mean UNKNOWN rather
than "the very first tick" — otherwise every pre-existing file appears to have
been written at boot.

## Test cases to add in `cmd_vfs_stress`

Written before the code, per the v0.84 dedup-control and v0.85 crash-harness
precedent. Each names the reproducer that must make it fail.

### rename

1. **Simple rename.** `rename(A, B)`: B has A's content byte-for-byte, A is
   gone (`vfs_find(A) < 0`), and the dirent count is unchanged — no orphan.
2. **Rename ONTO an existing name.** Target's blocks are released: the CAS
   unreferenced count must not climb, using the v0.84 accounting already in the
   suite. This is the leak rename can introduce that unlink cannot.
3. **No duplicate names, ever.** After each rename, scan the directory for two
   live dirents sharing a name. This is the assertion the half-applied crash
   case would trip.
4. **Crash-atomic.** Arm `CRASH_INJECT_COMMIT_FAIL` before a replacing rename,
   remount, and require the directory to show EITHER the old pair or the new
   single entry — never both names live, never neither.
   *Falsifier:* journal only one of the two dirty blocks; case 3 or 4 must go
   red.

### chmod / chown

5. **The syscall, not the struct.** Call `SYS_CHMOD` from ring 3 and prove the
   mode is what a later `SYS_STAT` reports and what `vfs_permit` then enforces —
   closing the gap where the existing assertion assigns `.mode` directly.
6. **Persistence across remount.** chmod, then remount, then confirm the mode
   survived. This is what makes it a JOURNALLED metadata change rather than an
   in-memory one.
7. **The effective-uid rule, from a genuinely dropped process.** A worker that
   `seteuid(1000)`s must be REFUSED chmod on root's file. Against today's kernel
   this FAILS, which is the point: it is the regression test for finding 2 and
   must be written before the fix.
   *Falsifier:* the unfixed kernel itself.

### timestamps

8. **mtime moves on write, and only on write.** Stamp, write, confirm it
   advanced; read the file, confirm mtime did NOT move.
9. **atime moves on read.**
10. **Both survive a remount**, and a file on a volume with no stamps reads as
    UNKNOWN rather than as tick 0.
    *Falsifier:* stamp at creation only; case 8 must go red.
