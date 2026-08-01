# Outrun OS v0.57.0-metal — occ Language Completeness & Native Preprocessor

`occ`, the compiler that runs at ring 3 inside OutRun, grew the three things that
separate a demonstration from a usable compiler: **a real preprocessor**, **a
type system with real memory layout**, and **the ability to compile several
source files into one program**. It also grew the ability to say *no* — four
classes of invalid program are now refused with a diagnostic naming the file,
line and offending identifier, and a suite exists specifically to prove it.

Two disk-corruption bugs were found along the way, one of which is the root
cause of a defect v0.56 shipped as unexplained.

## What's new

### A native preprocessor

A genuine pass that runs to completion **before the lexer sees a character**,
turning a source file plus its includes into one expanded translation unit. The
fused single-pass code generator is untouched by it — which is the whole reason
for a separate pass: a single-pass emitter cannot be re-entered to expand a
macro.

| Directive | Behaviour |
| --- | --- |
| `#include <h>` | resolved against `/usr/include/` on the VFS |
| `#include "h"` | the including file's own directory first, then `/usr/include/` |
| `#define NAME body` | object-like substitution |
| `#define NAME(a,b)` | function-like, positional, parenthesised on substitution so precedence survives |
| `#undef NAME` | |
| `#ifdef` `#ifndef` `#else` `#endif` | nested, so header guards work |
| `#line n FILE` | **emitted** by this pass, **consumed** by the lexer |

That last row replaces v0.56's "subtract the prelude's line count" arithmetic,
which worked only while the prelude was the single thing prepended and could not
survive `#include` at all. Diagnostics now read `occ: /src/t.c:30: …` — the
user's real file and line, after the prelude and three headers have been pasted
in front of their code.

**Comments are stripped first**, on load, before any directive is examined. That
ordering is load-bearing rather than tidy: `/usr/include/outrun_abi.h` defines
`O_CREAT` with a block comment that runs onto the next line, and processing
directives first would define it as `1 / * the ONLY …`. Newlines inside comments
are preserved so line numbers stay honest.

**Prototypes became mandatory.** The moment `#include` pastes real header text
in, `int strlen(char *s);` has to parse — the old code read it as a function
*definition* whose body turned out to be a semicolon. A prototype registers the
symbol and deliberately leaves it undefined, so the existing fixup pass still
reports "undefined function" for a promise nothing keeps.

All seven SDK headers gained include guards, which makes `#ifndef` load-bearing
rather than decorative. Their stale text is corrected too: `outrun_abi.h` no
longer claims occ has no preprocessor, and `stdlib.h` no longer declares
`void free` / `void exit` when `libc.oc` defines both as `int`.

### struct, union, typedef — and real layout

occ previously had **no type system at all**. `int` was 64-bit, every value was a
register, every local was one 8-byte slot, and `occ_is_type()` was a three-way
string compare that discarded its answer. Structs cannot be built on that,
because a struct is precisely a thing whose parts live at known *byte offsets*.

A type is now the triple `(sidx, ptr, size)`:

| | |
| --- | --- |
| `sidx >= 0`, `ptr == 0` | a struct or union **by value**; `size` is its layout size |
| `sidx >= 0`, `ptr > 0` | a pointer to one; size 8 |
| `sidx == -1` | a scalar: 1 for `char`, 8 for `int` and pointers |

**Layout is real, not one word per member.** Each member is aligned up to its own
alignment and the total is rounded up to the struct's alignment, so
`struct { char a; int b; }` puts `a` at 0, `b` at 8, and has size 16. That
distinction is the point: a layout where every member is 8 bytes wide has no
alignment or padding to verify, and a suite checking it would be checking
nothing. Unions place every member at offset 0 and take the size of the largest.

Delivered: struct and union definitions including nested structs by value;
`typedef` for scalars, pointers and structs (the alias carries the full triple);
`.` and `->` with arbitrary chaining; **size-aware loads and stores**, so a
`char` member is written one byte wide; struct locals at their real size rounded
to 8 for frame alignment; struct globals aligned before placement and reserving
their real size per array cell; and struct pointers as parameters carrying the
target type so `p->x` in the callee knows the layout.

**The member chain is one function** used by both the lvalue and rvalue paths,
deliberately. Two copies of member-offset arithmetic is how a struct comes to
read correctly and write to the wrong place.

### Multi-unit compilation

```
occ a.c b.c c.c -o out.elf
```

Every input is preprocessed, in order, into one translation unit, and a single
code-generation pass runs over the whole thing. Cross-unit references resolve
through the existing forward-call fixup table, and the output is the same unified
3-`PT_LOAD` ELF — text `R+X`, rodata `R`, data `R+W`.

Two consequences of fusing units are visible to a user, so both are handled
rather than left as traps:

- **Each input gets a fresh macro table.** In real C a `#define` in `a.c` cannot
  reach `b.c`; concatenating naively would let it. The cost is re-pasting
  whatever headers each unit includes — harmless, since a prototype emits no
  code.
- **A name defined twice is now an error**, for functions and globals.
  Previously the second definition silently overwrote the first symbol's address
  and every call went to whichever unit parsed last. The global case was worse:
  the name was re-pointed at freshly reserved storage, so writes through the old
  and new definitions landed in different places and neither saw the other.

A fresh macro table has a consequence of its own, and getting it right is most of
that work: a shared header's guard is fresh in every unit, so the header's text
really *is* pasted more than once into the fused unit, and the same struct is
genuinely defined several times. Refusing that would make shared headers unusable
across units; accepting it blindly would let two *different* definitions coexist
with whichever parsed last deciding every member offset. So a redefinition is
accepted **only when the computed layout is identical member-for-member** — same
names, offsets, sizes, pointer depths, alignment and total — and rejected with
`struct redefined with a DIFFERENT layout` otherwise.

`-o` is the real command-line form. The legacy positional form (`occ a.c out.elf`)
is kept because every existing caller uses it, and silently changing what the
trailing argument means would turn a working invocation into one that overwrites
its own source. A bare `occ a.c b.c` is **rejected** rather than guessed at: with
no `-o` there is no way to tell a second input from an output path.

### `cmd_compiler_stress` (new suite, `compilerstress`)

Where `toolstrs` proves the self-hosting *loop* works, this proves the *language*
does. The ring-3 half (role 39) authors a three-file project, compiles the two
units into one ELF, runs it, and then feeds the compiler four programs that must
not compile. The kernel half checks what ring 3 cannot honestly claim about
itself.

The positive program **measures its own struct offsets** with
`&s.member - &s`. That matters: it is not comparing against numbers the compiler
reported, it is measuring the addresses the compiler actually generated. A layout
bug cannot agree with itself here. It checks offsets for four different shapes,
that a `char` member is stored one byte wide without flattening its neighbours,
that a union overlays at offset 0, nested access two levels deep, and a
cross-unit call in each direction.

The four refused programs, each verified to fail **and** to leave no output file
behind:

| Invalid program | Diagnostic |
| --- | --- |
| `.` used on a pointer | `'.' on a pointer; use '->'` |
| `main` defined twice | `function already defined 'main'` |
| `struct Q` defined with two different layouts | `struct redefined with a DIFFERENT layout 'Q'` |
| a member that does not exist | `no such member 'zzz'` |

Checking that a refused compile left nothing behind is done from *outside* the
process that ran it — a process asserting its own negative result is the weaker
claim.

### `cc` takes several inputs

`cc a.c b.c -o out.elf` from the shell, and therefore from the Cyber-Terminal,
whose diagnostics routing is unchanged from v0.56. The shell tokenizer went from
4 tokens to 8: at 4 it would have silently dropped the tail of a multi-input
command line and compiled the wrong set of files.

## Bugs found live during this milestone

1. **The CAS content index was undersized — and this is the root cause of the
   defect v0.56 shipped as unexplained.** `index_blocks` was a hardcoded 16 and
   `CAS_SLOTS_PER_BLOCK` is 32, giving **512 entries for a volume of 8192
   blocks**. A single regression boot stores well over 512 distinct chunks, so
   the index filled part-way through *every* boot and no put after that point
   could be recorded. v0.56 described this as "a near-full CAS volume has an
   unexplained read-back mismatch: the right byte count, the wrong bytes" and
   guessed at data exhaustion — but only 537 of 8192 blocks were used. It was the
   *index* at 512 of 512. Before v0.56, `cas_put` returned the hash anyway with
   nothing recorded, so `cas_get` later missed and `vfs_read_file` handed back
   whatever stale bytes the shared bounce buffer held; v0.56 made that fail
   loudly instead of corrupting. This is the fix for why it happened at all.
   The index is now sized from the volume at two slots per data block (50% max
   load, short probe chains), and the probe *start* comes from a mixed hash
   rather than the low bits — which this hash visibly lacks entropy in, since
   every 512-byte chunk in a boot log hashes to a value ending in the same byte.
2. **Two demos were scribbling on the live filesystem at hardcoded absolute
   sectors.** The thread-concurrency worker wrote sectors 300–331 and the
   virtio-blk round-trip check wrote **sector 0, the CAS superblock itself**.
   Both were harmless only by accident of layout. Growing the index to cover
   blocks 3–514 put the worker's writes *inside it*, and `0x41+id` across 32
   index sectors silently destroyed whatever entries lived there. It presented as
   exactly **one lost chunk out of 505** — with roughly one live entry per
   sector, wiping 32 sectors loses about one that matters — and the symptom
   appeared four thousand log lines later as `/bin/init` failing to exec:

   ```
   [vfs    ] SHORT READ '/bin/init': chunk 11 of 122 hash 0cb849378a453f7b
             is NOT in the CAS (dirent 0, len 62056)
   [elf    ] reject: segment file range out of bounds
   ```

   That diagnostic is one v0.56 added for an unrelated reason, and it paid for
   itself here. The fix is not a different magic number, which would rot the same
   way: the superblock now **declares** a reserved 32-block scratch region,
   `cas_format` reserves it, and both demos ask where it is. A raw-block test
   that needs blocks now says so in the layout.
3. **`occ_parse_type` consumed the `*`s.** A pointer level belongs to the
   *declarator*, not the specifier: consuming it there made `int *a, b;` declare
   two pointers, and double-counted in every context whose declarator loop also
   consumed stars — `struct P *p;` came out as pointer-to-pointer.
4. **The assignment path captured the destination size after parsing the
   right-hand side.** Evaluating the RHS can walk another member chain, which
   overwrites the recorded size, so `a.ch = b.word` would have stored eight bytes
   into a one-byte member. The size is now captured first.
5. **A struct-typed variable was being loaded as a value.** It now evaluates to
   its **address**, the rule C uses for arrays; loading it put the struct's first
   eight bytes in a register and called that its value.
6. **The Makefile did not list `user/occ.c` as a prerequisite** even though
   `init.c` `#include`s it, so `make` reported success while shipping the
   *previous* compiler. An edit to occ.c alone never reached the image, which cost
   a full boot cycle to notice. A header-style dependency make cannot see is
   worse than a slow build.
7. **Two existence probes leaked a descriptor each.** The preprocessor rework
   added `oopen()` checks that never closed, burning two of the sixteen global
   kernel descriptors on every compile.

## Honest scope notes

- **occ still cannot compile itself, and that has not changed.** It now has a
  preprocessor, structs, unions, typedefs and multi-file input — but no
  bitfields, no function pointers, no varargs, no `switch`, no `goto`, no `enum`,
  no floats, and `occ.c` uses several of those. "Self-hosting" in this project
  means *the system hosts a working compiler and builds and runs programs with no
  host toolchain*, not that the compiler is its own fixed point. Closing the
  remaining gap is a milestone, not a patch.
- **Multi-unit is not separate compilation.** There are no object files, nothing
  is compiled independently, and changing any input recompiles everything. Source
  inclusion is occ's linkage model because it has no relocations, no symbol table
  in its output, and no linker. `/usr/lib` therefore still ships `libc.oc` as
  source and still deliberately does **not** ship a faked `crt0.o` or `libc.a`.
- **`a[i]` is still word-scaled**, so indexing an array of structs would multiply
  the index by 8 instead of by the element size. It is now **diagnosed**
  (`indexing an array of structs is not supported`) rather than miscompiled.
  Fixing it means scaling by element size, which is a change to the indexing
  model rather than a patch. Local arrays of any type are also still unsupported
  — a local declaration is one slot — which is why `libc.oc`'s scratch buffers
  are globals and its formatting functions are therefore not reentrant.
- **Struct assignment, struct arguments and struct returns are refused**, not
  half-implemented: `a struct cannot be passed by value; pass a pointer`.
- **Function-like macro invocations must fit on one line**, macro rescanning is
  bounded rather than a proper recursive expansion (a self-referential macro is
  additionally blocked by a per-macro flag, so neither can loop), and there is no
  `#if` with expressions, no `#elif`, no `##`, no stringification and no varargs
  macros. `#ifdef`/`#ifndef` is what header guards need and that is what is here.
  An unrecognised directive is **reported**, not silently skipped.
- **`#include` is processed, but a declaration still has to be parseable.** The
  SDK headers are now genuinely consumed, which is why they needed guards and why
  their prototypes had to start parsing — but a name declared in a header and
  never defined in the runtime still fails the compile, by design.
- **`sigaction` and `pthread_create` remain declared in the SDK and unreachable
  from occ.** Both take a function pointer and occ cannot produce one. The kernel
  machinery behind them is real and exercised by `posixstrs` from `/bin/init`'s
  own C. `signal.h` and `pthread.h` say so in the file.
- **The volume format is version 5 and is not backward compatible.** A v4
  volume's `data_start` and journal regions sit at different offsets than this
  kernel computes, so `cas_mount` refuses anything below 5 and reformats — the
  same discipline as v0.56's v4 dirent bump.
- **`VFS_MAXFILES` is still 64 dirents**, and files are durable and never garbage
  collected, so repeated boots against the *same* volume accumulate names. This
  milestone adds five more (`/src/shared.h`, `/src/lib2.c`, `/src/cs_hdr.h`,
  `/src/cs_a.c`, `/src/cs_b.c`, plus two outputs), which brings that ceiling
  closer than it was.
- **One `posixstrs` failure seen once in v0.56 was never explained** — a
  descriptor read returning 0 bytes, not reproduced in the three v0.57 matrix
  runs either. Still recorded as open rather than quietly dropped.

## Verification

GRUB ISO in QEMU, TCG-only (no KVM), virtio-vga + virtio-blk + virtio-net. Disk
images recreated fresh before every boot, **one guest at a time**.

| Config | `compstrs` | `toolstrs` | `posixstrs` | `vfsstrs` | Result |
| --- | --- | --- | --- | --- | --- |
| Uniprocessor, BIOS | 8/0 | 9/0 | 11/0 | 19/0 | all 31 suites **0 FAIL** |
| SMP `-smp 4`, BIOS | 8/0 | 9/0 | 12/0 | 19/0 | all suites **0 FAIL** |
| q35 + VT-d IOMMU, `-smp 4` | 8/0 | 9/0 | 12/0 | 19/0 | all suites **0 FAIL** |

The SMP rows carry one extra `posixstrs` check each — the cross-core ring-3
overlap assertion only a multi-core config can support. The uniprocessor boot
correctly SKIPs the cross-core checks in `mcq`, `mcpre`, `slice`, `cio` and
`smpstrs`, and `audstrs` SKIPs on all three where no virtio-sound device is
attached (unchanged since v0.51).

Both compiler proofs appear in every config's log:

```
occ: 2 input files compiled as one translation unit
  [a.out ] compiled by occ against /usr/lib/libc.oc; main returns 147
  [self  ] authored, compiled and RAN a program without a host toolchain
[toolstrs] SELF-HOSTING VERIFIED
  [compst] offsets, alignment, nesting and cross-unit calls all verified
  [compst] all four invalid programs were REFUSED, and none left an output file
[compstrs] LANGUAGE VERIFIED
```

`main returns 147` is a composed check, not a smoke test — 28 from a loop, 55
from recursion, 6 from `strlen`, 11 from `atoi`, 19 from `(1<<4)|3`, 7 from an
object-like macro, 3 from a function-like macro, 5 from the taken `#ifdef` branch
(the untaken one is 999, so a wrong branch is unmissable), 2 from an `#ifndef`
guard, 7 from a nested struct read, and 4 from a cross-unit call that itself
calls back into the first unit.

### A note on how these runs were conducted

Unchanged from v0.55 and v0.56, and still decisive for how this milestone was
debugged. **Run one guest at a time when measuring** — several suites assert
cross-core properties with tick-based watchdogs, and three 4-vCPU TCG guests
sharing 4 host cores means no guest actually gets four cores. **Deliberately
oversubscribing the host remains the best SMP bug-finder in this project**, just
not while measuring a pass/fail matrix.
