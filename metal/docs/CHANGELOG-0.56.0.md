# Outrun OS v0.56.0-metal — Native Self-Hosting Developer Toolchain

OutRun compiles and runs its own programs. A C compiler written for this system
runs **at ring 3, on OutRun's scheduler, using OutRun's heap and filesystem
syscalls**, reads a source file out of the VFS, and writes a runnable x86-64 ELF
back into it. No host toolchain is involved at any point after the boot image is
built. The filesystem grew the four capabilities that made that possible —
large files, hierarchical path names, a ring-3 heap, and exec-by-path — and a
real SDK now lives in `/usr/include` and `/usr/lib`.

The self-hosting loop, in full, is one ring-3 worker:

1. it **authors** `/src/t.c` into the filesystem,
2. `fork`s and `execve`s **`/bin/occ`**, which compiles it against
   `/usr/lib/libc.oc` and writes `/bin/t.elf`,
3. `fork`s and `execve`s **`/bin/t.elf`**, and checks the answer it computes.

## What's new

### Substrate: four things the filesystem could not do

- **Large files (`VFS_MAX_FILE_BYTES` = 256 KiB).** The chunk map was 16 inline
  hashes — an 8 KiB ceiling, which the ring-3 image itself (~50 KiB) is five
  times past. It is now 16 direct + single-indirect (64) + double-indirect
  (64×64) = **4176 chunks**. The indirect blocks are ordinary CAS objects, so
  they inherit dedup, the metadata journal and the existing lock ranks for free
  rather than needing a parallel path.
- **Hierarchical path names.** `dirent.name` widened from 32 to **64 bytes**, so
  `/usr/include/outrun_abi.h` is a name. This is still a **flat namespace** —
  there is no inode tree and no directory objects; a "directory listing" is
  prefix matching over the one table, which is all a toolchain needs to resolve
  includes and find `/bin`. Widening `name` shifts every later field, so the
  volume version went to **4** and pre-v4 volumes are refused and reformatted
  rather than misread. A `_Static_assert(sizeof(struct dirent) == 256)` guards
  the layout.
- **A ring-3 heap.** **`SYS_BRK` (58)** maps an anonymous RW+NX region in its
  own vaddr window (`0x570000000000`, 4 MiB ceiling). Growth is page-granular;
  shrinking unmaps **and frees**, with a TLB shootdown *before* the frame goes
  back on the free list. `page_free_tree` reclaims it at exit with no special
  case. `/bin/init` gains `malloc`/`free`/`realloc`/`calloc` over it.
- **`SYS_EXECVE_PATH` (59).** v0.55's `execve` took a role selector because
  there was only one image in the system. It now takes a **path**: the ELF is
  read out of the VFS, built in a fresh address space, and only then does CR3
  switch — so a failed exec leaves the caller running.

### `occ` — the Outrun C Compiler

A single-pass compiler: the parser emits machine code as it parses, with no AST
and no separate assembler or linker stage. Forward calls go through a small
fixup table patched when the function is defined. It fits in one auditable file
because "assembling" and "linking" are collapsed into the code emitter rather
than existing as passes.

**The language subset, stated honestly — this is not C89:**

| | |
| --- | --- |
| types | `int`, `char`, pointers to them. **`int` is 64-bit**: every value is a register, every stack slot 8 bytes. |
| decls | global `int`/`char*` variables **and global arrays**; functions of ≤ 6 parameters |
| stmts | blocks, local declarations, `if`/`else`, `while`, `for`, `return`, expressions |
| exprs | `=` `\|\|` `&&` `\|` `^` `&` `==` `!=` `<` `>` `<=` `>=` `<<` `>>` `+` `-` `*` `/` `%`, unary `-` `!` `*` `&`, calls, literals, `a[i]` |
| builtins | `__syscall(n,a0,a1,a2)`, `__ldb(p,i)`, `__stb(p,i,v)` |
| **not here** | structs, unions, enums, typedefs, floats, `switch`, `goto`, the preprocessor, varargs, multi-file linking, **and local arrays** |

Two facts about that table are load-bearing rather than cosmetic:

- **`a[i]` means `*(a + i*8)` — a machine word, never a byte.** That is the
  price of "every value is a register". It means `strlen` written as `while
  (s[n])` reads eight bytes at a time and stops at the first NUL-*containing*
  word. `__ldb`/`__stb` exist so a byte can be reached at all; without them a
  `string.h` on this system would be a prop.
- **A local declaration is one 8-byte slot**, so there are no local arrays. Every
  scratch buffer in the runtime is a global, which makes those functions
  non-reentrant. Said out loud in `libc.oc` rather than hidden.

**Memory model.** Three fixed page-aligned bases — text `0x500000000000`,
rodata `+0x200000`, data `+0x400000` — mean absolute addressing works and **no
relocation machinery is needed**: text is emitted before data sizes are known,
but the data *base* is chosen up front, so a global's address is an immediate.

**Output.** Three `PT_LOAD` segments: text `R+X`, rodata `R`, data `R+W`. That
is not decoration — `elf_load` **refuses** a segment that is both writable and
executable, so a compiler that merged them would produce binaries this OS will
not run. `toolstrs` reads the produced file's own program headers back and
audits W^X rather than trusting the loader to have rejected it.

`/bin/occ` and `/bin/init` are the **same ELF** under two names (CAS dedup makes
the second name cost one dirent and no blocks). It is a **multi-call binary**
dispatched by `argv[0]`, checked *before* role dispatch — otherwise the exec'd
copy re-runs its old role and execs itself forever.

### The SDK: `/usr/include` and `/usr/lib`

Nine real files, written into the VFS at boot. Exactly one is load-bearing, and
the distinction is the point:

- **`/usr/lib/libc.oc` IS.** `occ_compile()` reads it and **prepends** it to the
  user's translation unit, so `strlen`, `strcmp`, `strcpy`, `memcpy`, `memset`,
  `atoi`, `abs`, `puts`, `putchar`, `putdec`, `puthex`, `open`, `creat`, `read`,
  `write`, `close`, `unlink`, `getpid`, `getppid`, `fork`, `waitpid`, `yield`,
  `kill`, `exit`, `sbrk`, `malloc` and `free` are in scope for every compiled
  program. Source inclusion is occ's **only** linkage model. Delete the file and
  programs that call `strlen()` stop compiling — which is exactly the property a
  real `/usr/lib` has, and the reason this is not decorative.
- **`/usr/include/*.h` are NOT read by the compiler.** occ has no preprocessor;
  it skips `#` lines. They are the normative, human-readable ABI documentation —
  `outrun_abi.h`, `stdio.h`, `stdlib.h`, `string.h`, `unistd.h`, `signal.h`,
  `pthread.h`. `toolstrs` checks that what they *declare* and what `libc.oc`
  *defines* agree, so they cannot silently drift into fiction.

**There is deliberately no `crt0.o` and no `libc.a`.** occ generates its entry
stub (call `main`; take the return value; `SYS_EXIT`) directly as the first
bytes of `.text`, so there is no object file to ship; and an `ar` archive would
need a linker to consume, which nothing here has. Shipping either would make the
directory listing look more finished than the toolchain is.
`/usr/lib/README` says so **on the running system**, not only here.

### Diagnostics reach the Cyber-Terminal

`cc <source> <output>` is a shell command. That is the whole of the routing, and
it needed no new plumbing — only noticing that the pieces already met in the
middle:

> the terminal runs a real shell command through **`SYS_RUN_CMD`**, which *arms*
> the kernel console capture → occ writes diagnostics with **`SYS_WRITE`** →
> `SYS_WRITE` goes through **`kputc`** → `kputc` appends to that capture buffer.

So a compile error typed into the terminal window comes back into that window,
with the user's own line numbers, and occ never learns a terminal exists.
Because the prelude is prepended, diagnostics **subtract its line count**: a user
sees *their* line number, and an error genuinely inside the runtime reports
`occ: /usr/lib/libc.oc line N` instead of a number the user cannot find.

### `cmd_selfhost_test` (new suite, `toolchainstress`)

Nine checks. The ring-3 half runs the author → compile → run loop; the kernel
half then verifies from outside what ring 3 cannot honestly claim about itself:

- the SDK is installed and non-empty (7 headers + runtime + README);
- prefix listing enumerates `/usr/include/` and `/usr/lib/` correctly;
- `/usr/lib/libc.oc` really defines every symbol the headers declare;
- every round authored, compiled and **ran** a program (driver exit 940);
- a genuinely new executable appeared in the filesystem;
- its own program headers describe a well-formed 3-segment x86-64 ELF;
- **no segment it emits is both writable and executable**;
- frame accounting reconciles and no lock rank was violated.

`wx_ok` and `elf_ok` start **false** and are only raised by an audit of a real
produced binary. Written the other way they passed vacuously on the very first
run, when no file existed at all — a check that reports success for work that
never happened is worse than no check.

The program the driver authors is built so its answer decomposes:

```
0+1+..+7 = 28   the for loop and local arithmetic
fib(10)  = 55   recursion through the fixup table
strlen   =  6   byte addressing via __ldb — a[i] alone cannot do this
atoi     = 11   byte addressing plus the digit loop
(1<<4)|3 = 19   the shift and bitwise-or precedence levels
           ---
           119
```

Nothing in that source defines `strlen`, `atoi`, `strcmp`, `strcpy`, `malloc`,
`puts` or `putdec`. If the prelude were not being read, the compile would fail
with "undefined function" rather than quietly passing.

## Bugs found live during this milestone

1. **The FD-leak Heisenbug.** Giving `SYS_OPEN` a flags word meant `cmd_fuzz`'s
   random arguments were being read as `O_CREAT`: the fuzzer was **creating**
   files — with garbage or empty names taken from whatever ring-3 pointer `a0`
   happened to be — and claiming global descriptors it never closes. That
   poisoned the single global `g_ofiles[16]` for every suite that runs after
   `fuzz` and audits it, so **six suites failed together and intermittently**
   (`cio`, `dmastrs`, `kpstrs`, `ipcstrs`, `vfsstrs`, `posixstrs`), which is
   what made it look like a scheduler race. Root-caused with failure-path-only
   probes in `cmd_cio` — deliberately failure-only, so a healthy run is never
   perturbed — which printed the holder: alive, foreign to the suite, named
   `'fuzz'`, on a dirent with an empty name, and with no watchdog line anywhere
   (ruling out the competing "the wait loop times out early" theory outright).
   `SYS_OPEN` now rejects any undefined flag bit with `-EINVAL`, and
   `vfs_open_for` will not create a dirent for an empty name. An ABI that
   silently ignores flags it does not understand can never add one safely.
2. **The VFS stress suite was deleting the SDK.** Its hierarchical-path fixture
   used the literal names `/usr/include/stdio.h`, `/usr/include/stdlib.h` and
   `/usr/lib/crt0.o` — plausible names for a path test, and harmless until this
   milestone installed a *real* SDK at exactly those paths. The suite writes six
   bytes over each and then unlinks it, so two SDK headers were being removed out
   from under the toolchain and the suite's own prefix count was inflated from 4
   to 9. Moved to `/vfstest/`. A test fixture must not share a namespace with the
   thing it is testing around.
3. **`cas_put` reported success for content it could not index.** When the
   content index is full, `cas_index_stage` fails but the block has already been
   allocated and written; the old code returned the hash **anyway**. The caller
   then stored a hash `cas_get` can never resolve. It now frees the unreachable
   block and reports the failure — and the same path also recovers the block on
   a lost dedup race, which used to leak it.
4. **`vfs_read_file` discarded `cas_get`'s return value.** On a miss, the shared
   512-byte bounce buffer still held the *previous* chunk's bytes and they were
   copied out as this chunk's: a read that returned the correct length and the
   wrong content, with no error anywhere. It now stops. A short read is a signal
   the caller can act on; a plausible-looking wrong one is not.
5. **A failed write left a half-built dirent.** Since `cas_put` can now genuinely
   fail, `vfs_write_locked` snapshots the dirent and **rolls back atomically**,
   so the previous file survives a failed overwrite instead of being replaced by
   a chunk map with holes in it.
6. **`libc.oc`'s `sbrk()` tested `SYS_BRK(want) == want`.** `SYS_BRK` is
   page-granular: it rounds up and returns the *resulting* break, so the equality
   never held and **every `malloc()` returned 0**. The test is `>=`. `unistd.h`
   now states the contract so the next caller does not repeat it.
7. **The self-hosting driver's wait budget was too small.** With the SDK prelude
   in front of it the compiler has ~4 KiB more source to get through and emits an
   image nine times larger; under TCG that pushed past the old 60000-spin budget,
   so the parent gave up while the child was still working and reported the
   generic "child failed" — even though the compile went on to succeed and print
   `compiled OK`. Budget raised, and **a timeout now has its own exit code**: a
   wait that expires and a child that fails are different events.
8. **`cmd_cio`'s CAS auditor read `chunk_hash[c]` past index 15.** Harmless while
   every file fit in 16 chunks; wrong the moment `/bin/init` (96 chunks) existed.
   Fixed to walk the indirect map through `vfs_chunk_hash_at()`.
9. **occ's `malloc` alignment.** An 8-byte block header left every payload
   8-aligned, which the compiler's own 16-byte-aligned structures did not
   survive. The header is padded to 16.
10. **A write past the file ceiling was silent data loss.** `nchunks` was clamped
    to 16 while `len` recorded the full size, so the dirent claimed content it
    did not store. The size check now runs *before* the dirent is touched, so a
    rejected write is also atomic.
11. **`rq_push` could drop a runnable task on the floor — and every caller
    ignored it.** The per-CPU run queue was a ring of 8, which holds **seven**
    entries. `cmd_posix_stress` queues seven workers, and on a **uniprocessor**
    all seven go to queue 0 — so the queue was exactly full before the suite had
    executed one ring-3 instruction. The next enqueue, `fork()` handing over its
    brand-new child, hit `full`, and `rq_push` returned `-1` to a caller that did
    not look: the task was lost, marked `pstate = 1` (preempted, resumable) but
    present in **no** queue, unreachable by `rq_pop`, by `rq_steal`, and by every
    driver loop in the tree. Every uniprocessor `posixstress` round wedged on it.

    Latent since v0.39 and only reachable when a queue fills; v0.56 roughly
    doubled the ring-3 image and shifted the timing enough to hit it on every UP
    boot. Two fixes: `RQ_LEN` 8 → 32, and a new `rq_push_any()` that tries the
    preferred core, then any other online core the task's affinity allows, and
    **prints loudly** if every queue is full instead of returning a value
    history shows nobody checks. The two paths where a dropped enqueue is
    unrecoverable — the post-preemption requeue, which holds the only copy of a
    task's ring-3 context, and `fork`'s child enqueue — now use it.

    Worth recording *how* this was found, because the first theory was wrong.
    It looked like a watchdog budget problem, so the budget was tripled from
    3000 to 9000 ticks. It made no difference at all: the breadcrumb printed the
    same pid, at the same `rip`, for three times as long. That is what proved
    "parked" rather than "slow" — so the budget was put **back** to 3000, where
    the suite now passes, because a watchdog raised to hide a hang stops being a
    watchdog. The breadcrumb was then extended to print `pstate`, `rip`,
    `sig_pending` and `sig_mask`, and one look at `pstate 1 ... rq0=0` was the
    whole diagnosis.

## Honest scope notes

Read this section as the definition of what "self-hosting" does and does not
mean here.

- **occ compiles a C *subset*, and it cannot compile itself.** It has no
  structs, no typedefs, no preprocessor and no varargs; `occ.c` uses all four.
  "Self-hosting" in this release means **the system hosts a working compiler and
  builds and runs programs with no host toolchain**, not that the compiler is
  its own fixed point. The original brief asked for TCC or an "equivalent
  ELF-generating toolchain"; porting TCC needs a preprocessor, a linker, a
  relocation model and a far larger libc than exists here, so an original
  compiler that genuinely runs on the machine was chosen over a port that would
  not have.
- **There is no linker, no assembler, and no object format.** Source inclusion
  is the linkage model. This is why there is no `crt0.o` and no `libc.a`, and
  why those two files are absent rather than faked. Reaching them means giving
  occ relocations, a symbol table and a linker — a milestone, not a file.
- **`#include` is skipped, not processed.** The SDK headers are documentation
  and are verified against the runtime by `toolstrs`, but a program does not get
  a declaration by including one. Every symbol it uses must come from the
  prelude or be defined in the file.
- **`libc.oc`'s scratch buffers are globals, so its formatting functions are not
  reentrant**, and `free()` is a **no-op** — `malloc` is a bump allocator over
  `SYS_BRK`. Both are stated in the header comments on the running system.
- **`sigaction` and `pthread_create` are declared in the SDK but not reachable
  from occ.** Both take a function pointer and occ cannot produce one. The
  kernel machinery behind them is real and fully exercised by `posixstrs` from
  `/bin/init`'s own C. `signal.h` and `pthread.h` say this in the file rather
  than leaving a caller to discover it.
- **`SYS_THREAD_CREATE` is still BSP-only and still enforced**, for the
  unresolved v0.55 hang. Unchanged this milestone.
- **The volume format is not backward compatible.** Widening `dirent.name` moved
  every later field, so a v3 volume cannot be parsed at all and is reformatted.
  Silently misreading a directory would corrupt it on the next flush.
- **A near-full CAS volume still has an unexplained read-back mismatch.** Booting
  the *same binary* against an already-populated 4 MiB volume (537/8192 blocks
  used, 512 index slots) produced `vfsstrs` chunk-map failures: the right byte
  count, the wrong bytes. Bugs 3–5 above were found while investigating it and
  are real fixes in their own right, but they did **not** make that scenario
  pass, and the residual cause is not yet identified — every put on that volume
  deduped, so neither the "index full" nor the "volume full" path was even
  reached. The chunk-map check now reports which byte, in which chunk, and
  whether that chunk is retrievable from the CAS at all, so the next run has
  evidence instead of a verdict. **The release matrix runs on fresh volumes**,
  which is what it has always done; this is a known open defect, not a fixed one.
- **`VFS_MAXFILES` is 64 dirents**, and files are durable and never garbage
  collected, so repeated boots against the *same* volume accumulate names (the
  pid-keyed `cio-<pid>` files especially). The SDK's nine entries make that
  ceiling closer than it was.

## Verification

GRUB ISO in QEMU, TCG-only (no KVM), virtio-vga + virtio-blk + virtio-net.
Disk images recreated fresh before every boot, **one guest at a time**.

| Config | `toolstrs` | `posixstrs` | `vfsstrs` | Result |
| --- | --- | --- | --- | --- |
| Uniprocessor, BIOS | 9/0 | 11/0 | 19/0 | all suites **0 FAIL** |
| SMP `-smp 4`, BIOS | 9/0 | 12/0 | 19/0 | all suites **0 FAIL** |
| q35 + VT-d IOMMU, `-smp 4` | 9/0 | 12/0 | 19/0 | all suites **0 FAIL** |

The SMP rows carry one extra `posixstrs` check each — the genuine cross-core
ring-3 overlap assertion that only a multi-core config can support. The
uniprocessor boot correctly SKIPs the cross-core checks in `mcq`, `mcpre`,
`slice`, `cio` and `smpstrs`, and `audstrs` SKIPs on all three where no
virtio-sound device is attached (unchanged since v0.51 — see that changelog for
why it has its own target).

The self-hosting proof appears in the log of every config:

```
  [occ   ] compiled OK
  [a.out ] compiled by occ against /usr/lib/libc.oc; main returns 119
  [self  ] authored, compiled and RAN a program without a host toolchain
[toolstrs] SELF-HOSTING VERIFIED — OutRun compiled and ran a program with no host toolchain
```

### One failure seen once and not reproduced

Being exact about this rather than rounding it away: the **first** uniprocessor
run after the run-queue fix reported `posixstrs` 9/2 — round 2's std-fd worker
exited 964, meaning its read of `motd` through an ordinary descriptor returned
0 bytes. The two subsequent uniprocessor runs of the same binary were clean, and
the failure-only "SHORT READ" diagnostic added to `vfs_read_file` (which names
the file, the chunk and whether that chunk is in the CAS) **never fired** in
either. So there is a load-dependent path where a descriptor read comes back
empty that has been observed once and is not yet explained. It is recorded here
because it happened, not because it is understood.

### A note on how these runs were conducted

Unchanged from v0.55, and worth repeating because it decided how this milestone
was debugged:

**Run one guest at a time when measuring.** Several suites assert genuine
cross-core properties with tick-based watchdogs; three 4-vCPU TCG guests sharing
4 host cores means no guest actually gets four cores, and those assertions fail
for reasons that have nothing to do with the kernel.

**Deliberately oversubscribing the host is still the best SMP bug-finder in this
project.** It is worth doing on purpose — just not while measuring a pass/fail
matrix.
