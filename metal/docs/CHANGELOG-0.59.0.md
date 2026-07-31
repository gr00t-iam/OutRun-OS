# OutRun OS v0.59.0-metal — the descriptor substrate, pipes, and a native shell

Milestone 59 gives this system the thing it has never had: a **command
interpreter that is an ordinary process**. Until now the only shell was
`shell_exec()` inside the kernel — ring 0, no processes, no descriptors, and
therefore structurally incapable of `|`. `/bin/vsh` is a ring-3 program with no
privileges beyond `PCAP_FILESYSTEM` and `PCAP_CONSOLE`, compiled on the running
system by the system's own compiler.

Getting there required rebuilding what a file descriptor *is*.

## 1. The descriptor substrate

### `ofile.owner` became `ofile.owner_mask`

Through v0.58 a kernel descriptor had exactly one owning process for its whole
life. That single field is why `SYS_FORK` **refused** to give descriptors to the
child, and `sys_fork`'s own comment called it a permanent scope gap:

> It does NOT inherit DMA grants, window ownership, sockets or open descriptors
> — those are per-process kernel resources whose teardown hooks are keyed on the
> owning slot, and aliasing them into a second slot would break that invariant.

That reasoning is correct *for a single-owner field*, and it is exactly what the
mask replaces. `owner_mask` is a `uint64_t` bitmask of kproc slots; `MAX_KPROC`
is 64, so one word covers every slot exactly. This is the same pattern
`g_ipc_shm[].owner_mask` has used since v0.46.

`descriptor_teardown_kproc` now drops **one slot's** claim and frees the entry
only when the mask empties. So sharing became expressible without weakening the
guarantee that an exiting process leaves nothing behind — the leak audits that
protected the old invariant still pass, unchanged in spirit.

`SYS_FORK` now inherits open descriptors and the stdin/stdout redirections. DMA
grants, window ownership and sockets are still **not** inherited: those remain
genuinely single-owner (a grant names one IOMMU domain, a window one compositor
client) and stay a stated scope gap.

### Kernel pipe objects

A small pool of ring buffers (`PIPE_CAP` = 1024 bytes, `MAX_PIPES` = 8), each
referenced by the descriptors naming its two ends. The refcounts are counted in
**descriptors, not processes**, because fork aliases descriptors and each alias
is an independent right to read or write.

The behaviours that matter, and that `pipestrs` checks directly:

| Situation | Result | Why it must be this |
| --- | --- | --- |
| empty, writers remain | `-11` EAGAIN | "not yet" — a reader that treats this as EOF exits early on any slow producer |
| empty, no writers | `0` | genuine end of file |
| write, no readers | `-32` EPIPE | nobody can ever consume it; blocking would deadlock by construction |
| write larger than free space | short write | honest partial acceptance, never a silent truncation |
| write with no space | `-11` EAGAIN | |

**Non-blocking, returning EAGAIN where POSIX would sleep.** This kernel still
has no sleep/wake queue for ring 3, so blocking inside the syscall would hold
the caller's time slice — and on a uniprocessor the process that would supply
the missing bytes is precisely the one that then never runs. `SYS_TTY_READ` and
`SYS_WAITPID` already made this trade; pipes follow them, and userland spins on
`SYS_YIELD`.

The buffer is deliberately small so that any realistic pipeline exercises the
full-buffer path. A capacity large enough to swallow every test payload would
mean the interesting half of the code never runs in any suite we ship.

### Redirection that survives `execve`

`redir_in` / `redir_out` live on the **kproc**, not in a userland fd table, for
one decisive reason: a shell redirects by forking, pointing the child's stdout
somewhere, and *then* exec'ing the real program. Anything stored in the child's
address space is destroyed by that exec at exactly the moment it needs to take
effect. `SYS_EXECVE` keeps the kproc slot, so state parked there is still valid
when the new image's first write runs.

`SYS_WRITE` and `SYS_TTY_READ` consult them, so a program written against the
console is redirected without knowing it — `occ` did not need one line changed
to become usable on the right-hand side of a `>`.

Two consequences worth stating, because both are places a naive implementation
is silently wrong:

- **Redirected writes append.** `vfs_write_by_dirent` replaces a file's *entire*
  contents (it re-chunks and re-hashes), while a redirected stdout is a stream
  of many small writes. Passing each straight through would leave the file
  holding only the last one. The kernel stages the existing bytes, concatenates,
  and writes back — and fails with `ENOSPC` rather than truncating anything it
  could not fully read.
- **Redirected reads advance.** `vfs_read_file` always reads from byte 0, so
  successive reads would return the same opening bytes forever and a `< file`
  program would never reach EOF. `ofile.off` — present since v0.48 and until now
  always zero — became the real read cursor.

A closed descriptor also clears any redirection pointing at it. Otherwise the fd
number is recycled by the next `open` and a process's stdout silently reattaches
to a stranger's file: data corruption, not a leak, and invisible until something
writes.

### New syscalls

| # | Name | Contract |
| --- | --- | --- |
| 62 | `SYS_PIPE(*out2)` | fills **two 64-bit words** — `out2[0]` read end, `out2[1]` write end. Words, not C `int`s, because occ's `int` is a machine word; 32-bit ints would pack both descriptors into `out2[0]`. |
| 63 | `SYS_SETREDIR(which, fd)` | `which` 0 = stdin, 1 = stdout; `fd` of −1 restores the console. The fd is validated here, so a bad redirect fails at the point of the mistake rather than silently discarding output later. |

### One ABI change

`SYS_WRITE` (0) still returns 0 when writing to the console — a console write
cannot be short. When **redirected** it returns the byte count accepted, and
`-11` when none could be. `libc.oc`'s `puts()` therefore loops, which is what
lets an ordinary `puts()`-based program sit in a pipeline unmodified.

## 2. `/bin/vsh` — the native shell

`user/vsh.oc`, published as `/src/vsh.c` and compiled by `occ` against
`/usr/lib/libc.oc`. Supports command parsing, external execution via
`SYS_EXECVE`, `<` and `>` redirection, and `|` pipelines up to four stages.
Builtins are `exit`, `status`, `help`, and a `cd` that explains that this
filesystem has no working directory rather than pretending to change one.

Two implementation notes that are load-bearing rather than stylistic:

- **The parent drops its copies of both pipe ends immediately after forking each
  stage.** Holding either open is the classic way to hang a pipeline: the reader
  never sees EOF because the shell itself still counts as a writer.
- **Script mode (`vsh <path>`) reads the file directly** rather than through
  redirected stdin. `ttyread()` returns 0 both for "no key yet" and for "end of
  file", and those are indistinguishable to the caller — so a shell driven by
  redirected stdin could never tell a quiet keyboard from a finished script.
  Reading the file directly sidesteps the ambiguity, and it is what makes vsh
  testable in a headless boot.

`vsh_av` is declared `int *`, not `char *`, so `av[i]` steps a **word**. Under
v0.58's element-scaled indexing a `char *` steps one byte and would build an
argv vector of low bytes — which is a real, still-outstanding bug in
`omake.oc`; see below.

## 3. `pipestrs`

Two ring-3 rounds plus audits the shell cannot honestly perform on itself.

- **role 40** exercises the mechanism at the syscall level: round trips, the
  finite-buffer short write and EAGAIN, the EAGAIN-vs-EOF distinction, EPIPE,
  inheritance across fork, and both flavours of redirection.
- **role 41** exercises the result: `occ` compiles `/src/vsh.c` — the shipped
  source, unmodified — plus two filters (`emit` writes a known string, `wcx`
  counts bytes on stdin), then `/bin/vsh` runs a script containing a `>`
  redirect and a real `a | b` pipeline. The **output files are checked from the
  kernel**, because a shell reporting its own success proves nothing.

The audits require the descriptor table and pipe pool to return to their exact
pre-suite state. That matters more than usual here: the failure a shared
descriptor introduces — an entry whose last owner exited but whose mask never
emptied — is invisible to a passing functional test.

### A bug this suite caught on its first run

`pipe_unref_locked` was called only when the owner mask *emptied*, but `fork`
takes a reference **per owner**. So a child's `close` never released its
reference, `writers` could never reach zero, the reader never saw EOF, and the
pipe object was never reclaimed. `a | b` would have hung forever.

Invisible to any single-process test. It was caught by the cross-fork round on
the first run of the suite, which is the entire argument for writing the suite
that way.

## 4. Fixed: `fork()` never copied the heap

Another pre-existing gap, this one from v0.56, found by `vsh` and then covered
by the suite.

`vm_clone_user` bounded its walk at `UPRIVATE_VMAX` (= `DMA_USER_V`,
`0x520000000000`). That boundary was introduced to keep fork from duplicating
**granted windows** — device DMA aliases, shared surfaces, WIMP thumbnails —
and the comment says exactly that. When the ring-3 heap arrived in v0.56 it was
placed at `HEAP_USER_V` (`0x570000000000`), *above* the line, so it silently
fell inside an exclusion written for a different class of memory.

The heap is neither shared nor device-backed: it is ordinary anonymous private
memory, exactly like the image and stack that fork already clones, and POSIX
fork copies it. Nothing noticed for three milestones because no suite forked
and then touched malloc'd memory. `/bin/vsh` does — it mallocs its token table
and then forks per pipeline stage — and every child page-faulted on its first
access at `0x570000000000`.

The fix replaces the loop bound with an explicit predicate, `va_is_forkable()`,
so the forkable regions are stated rather than implied by an upper bound:

```c
if (va < UPRIVATE_VMAX) return 1;                                   /* image + stack */
if (va >= HEAP_USER_V && va < HEAP_USER_V + HEAP_MAX_BYTES) return 1;  /* the heap */
return 0;                                       /* DMA, surfaces, windows, thread stacks */
```

`heap_brk` is copied to the child too — cloning the heap's *contents* while
resetting the child's break to the base would hand its next `malloc()` an
address range its own live data already occupies.

`pipestrs` now asserts it directly: a child sees the parent's heap contents,
and a write in the child does **not** show up in the parent (a copy, not an
alias).

## 5. Fixed: `char *` parameters were indexed with the wrong element size

**This is a v0.58 defect, and v0.58 shipped with it failing.** In `occ`'s
parameter path the base type's width was overwritten by the pointer's width
before the element size was computed, so `char *s` **as a parameter** stepped
eight bytes per index while the identical declaration as a local stepped one.
`libc.oc`'s `strcmp()` therefore compared every eighth byte and reported two
equal strings as different.

`toolstrs` fails on `main` at `cabb460` with
`exit 946 — the compiled program returned the wrong answer`. With the fix it
reports 9 passed, 0 failed.

This is repaired here rather than deferred with the rest of the occ work
because `vsh` passes `char *` parameters everywhere — the shell could not have
been written at all without it.

## 6. Scope: occ self-compilation is explicitly deferred

Milestone 59 does **not** make `occ` compile its own source, and this is a
decision rather than an omission. `occ.c` uses, today, constructs occ cannot
parse. The full gap analysis is in **`docs/OCC-SELFHOST-GAP.md`**; in summary:

| Missing | Uses in `occ.c` |
| --- | --- |
| `unsigned` types | 67 `u8`/`u16`/`u32`/`u64` typedefs |
| `sizeof` | 21 |
| declarations in `for`-init | 64 |
| `switch` / `case` | present throughout the code generator |
| `enum` | token kinds |
| multi-dimensional arrays | `u8 sr[6][3]` with an initialiser |
| struct assignment by value | occ explicitly *refuses* this |
| array initialisers | `{ ... }` at declaration |
| constant expressions as array sizes | occ takes one `T_NUM` |

Attempting these while the system had no process plumbing would have been the
cart before the horse: a compiler that can compile itself but has no shell to
drive it is a demo, not a system. Language completeness is a later milestone
with its own suite.

## 7. Also outstanding

`omake.oc` builds its `execve` argv vector through a `char *` and therefore
stores only the **low byte** of each pointer under v0.58's element-scaled
indexing. The fix is a one-word type change, but nothing in the boot matrix
exercises `omake`, so it is left alone rather than shipped unverified —
**`omake` should be assumed non-functional until a suite covers it.** See
`CHANGELOG-0.58.0.md`.

## Verification

Full boot matrix, all suites, 0 FAIL:

| Configuration | Command |
| --- | --- |
| Uniprocessor | `make qemu` |
| SMP-4 | `make qemu EXTRA_QEMU="-smp 4"` |
| IOMMU (q35 + Intel VT-d) | `make qemu-iommu` |

`toolstrs` passes for the first time since v0.57, the pre-existing failure
having been root-caused and fixed here.

## Files

- `kernel/kernel64.c` — owner mask, pipe objects, redirection, syscalls 62/63,
  `pipestrs`, `libc.oc` and SDK header updates
- `user/vsh.oc` — the shell (new)
- `user/occ.c` — parameter element-size fix
- `user/init.c` — roles 40 and 41; role 34 updated to assert the new fd
  inheritance contract
- `Makefile`, `tools/mkstr.py` wiring for `/src/vsh.c`
- `docs/CHANGELOG-0.58.0.md` — backfilled
- `docs/OCC-SELFHOST-GAP.md` — the deferred-scope analysis
