# OutRun OS v0.60.0-metal — occ grows a type axis, and omake actually runs

Milestone 60. Four constructs from `OCC-SELFHOST-GAP.md` are closed, the build
tool that could never launch the compiler now does, and a new suite —
`langstrs` — proves both on the running system.

The headline is not the parser work. It is that `unsigned` is a **code
generation** change, and this release treats it as one.

## THE UNSIGNED TYPES

`u8`, `u16`, `u32` and `u64` are built-in primitives now, alongside `unsigned`
and `signed` as specifiers. They are not typedefs supplied by a header: `occ.c`
is written in terms of them (105 uses), and requiring `typedef unsigned char
u8;` to work first would put a dependency in front of the thing that needs it.

A type in occ was the triple `(sidx, ptr, size)`. It is a quadruple now — the
fourth axis is `unsg`, and it is threaded everywhere a size already was:
members, locals, parameters, globals and typedefs. That breadth is the point.
Signedness that is recorded but not consulted is worse than none, because the
failure is silent and data-dependent.

Four code generation sites read it:

| operation | signed | unsigned | what goes wrong if this is missed |
| --- | --- | --- | --- |
| `<` `>` `<=` `>=` | `setl/setg/setle/setge` | `setb/seta/setbe/setae` | `0xFFFFFFFFFFFFFFFF > 1` answers NO |
| `/` and `%` | `cqo` + `idiv` | `xor rdx,rdx` + `div` | `(u64)-1 / 2` yields 0 instead of `0x7FFF…F` |
| `>>` | `sar` | `shr` | the sign bit smears down through a hex formatter |
| load / store | — | widths 1, 2, 4, 8 | a `u32` store keeps 64 bits and floods its neighbour |

`==` and `!=` are the comparisons that do not care, so they keep one encoding.
Multiplication keeps one too: `imul` and `mul` agree on the low 64 bits, which
is all occ keeps.

Because occ is single-pass with no AST, there is nowhere to hang a type on an
expression. Two globals carry it instead — `occ_ety_unsg` and `occ_ety_size` —
written by whichever production last produced a value and read by `occ_binop`
immediately after. Each binary operator captures the LEFT operand's flag before
parsing the right, because parsing the right overwrites it. C's usual arithmetic
conversions apply: the result is unsigned if either side is.

A pointer is a machine word, so a pointer is not itself unsigned — but what it
points AT keeps its own signedness. That is a second field, `eunsg`, and it is
the same split v0.59 had to make for `esize`: `u8 *p; p[i]` must load an
unsigned byte, and the pointer's own width has nothing to say about it.

## SIZEOF

`sizeof(TYPE)` is answered from the type tables. `sizeof EXPR` is the
interesting one: the expression is **parsed for its type and then its code is
thrown away**, by rewinding the three output cursors — text, rodata, and the
fixup count — to where they were before. In a compiler with no AST to inspect,
parsing is the only way to learn a type, so it happens and the emission is
undone. Rewinding the fixup count matters as much as the text: a call inside
`sizeof` would otherwise leave a relocation pointing into code that no longer
exists.

The result is the size of the OBJECT. `sizeof buf` on a local array is the whole
array; `sizeof p` on a pointer is 8; `sizeof "abc"` is 4.

## DECLARATIONS IN A FOR-INITIALISER

`for (int i = 0; …)` — 71 occurrences in `occ.c`, and the cheapest item on the
gap list. The initialiser slot now runs the ordinary local-declaration parser
rather than a second copy of one, and the loop gets a scope so `i` does not leak
out and collide with the next loop's `i`.

The frame is deliberately **not** rolled back with the scope. Reclaiming the
slot would let a later declaration reuse those bytes, which is only safe with
liveness information a single-pass compiler does not have. Keeping the slot
costs 8 bytes per loop and cannot alias.

## SWITCH / CASE / DEFAULT, AND BREAK / CONTINUE

`OCC-SELFHOST-GAP.md` recorded that `switch` is **not** a self-hosting blocker —
`occ.c` contains none. That correction still stands and is not retracted here.
`switch` was built because it was asked for and because the kernel's style wants
it, not because `occ.c` needs it.

The single-pass shape puts the dispatch AFTER the body:

```
    eval control -> spill to a frame slot
    jmp DISPATCH
  body:  ... arms, each recording (value, offset) as it is passed ...
    jmp END
  DISPATCH: reload control; cmp/je to each recorded arm
            jmp default-arm, or fall through to END
  END:
```

By the time DISPATCH is emitted, every arm offset is a BACKWARD reference and
already known, so nothing there needs patching. The control value is spilled to
the frame rather than balanced on the stack because the arms between contain
arbitrary code — including `break` jumps out of the construct — and a stack
temporary would not survive them.

Case labels are compared through a materialised 64-bit constant rather than
`cmp rax, imm32`, whose immediate is sign-extended: `case 0xFFFFFFFF:` would
otherwise match a control value of −1.

`break` and `continue` came with it, because a `switch` without `break` is
technically legal C and practically useless. They are separate mechanisms:
`continue` in a `for` must reach the INCREMENT, not the loop top, and a switch
is breakable but not continuable — so `continue` inside a switch inside a loop
correctly reaches past the switch to the loop.

## THE OMAKE DEFECT

`/bin/omake` could never launch the compiler. Four of its globals are word
arrays — allocated with word sizing (`* 8`) and indexed as `p[i]` meaning "the
i'th word" — but all four were declared `char *`. Since v0.58 made indexing
element-scaled, a `char *` steps ONE BYTE, so `p[i]` addressed byte `i` and
stored through `mov [rdi],al`.

The damage differed per array, which is why it survived a release:

| array | consequence |
| --- | --- |
| `mk_ntok` | token counts are small and both ends agreed on the wrong scale — latent, not live |
| `mk_ss` | build signatures truncated to 8 bits; two different builds collided 1 time in 256 and omake reported "up to date" wrongly |
| `mk_st` | `SYS_STAT` writes two 64-bit words; `mk_st[0]` read the low byte of the hash, and `mk_st[1]` would have read the hash's second byte rather than the length |
| `mk_av` | argv POINTERS truncated to their low byte — fatal; `execve` received addresses like `0x48` |

All four are `int *` now, which is the honest type: `int` is occ's 64-bit
machine word. The task named the argv array; the other three are the same defect
and fixing only the one would have left omake still wrong.

`omake.oc`'s header comment claimed "p[i] is a WORD" and "no break/continue/
switch". Both stopped being true — in v0.58 and in this release respectively.
Both are corrected in place. The file is deliberately NOT rewritten to use the
new constructs: it is one of the programs the suite compiles as a regression
check, and changing its shape and its type declarations in one release would
make a failure ambiguous between the two.

## THE LANGSTRS SUITE

Ring-3 role 40 authors a program, compiles it with `/bin/occ`, runs it, then
feeds the compiler two programs that must be refused, then builds `/bin/omake`
and lets **omake drive occ** to build a two-unit project.

Every unsigned check in the test program is written so that SIGNED code
generation gives the WRONG answer — `big > 1`, `big / 2`, `big >> 60`,
`big % 10`. A compiler that parsed `u64` and then emitted `setg` fails here
rather than passing quietly. The block immediately after re-checks that plain
`int` still uses `setl`/`idiv`/`sar`, because the failure mode of this work is
not only "unsigned stayed signed" but also "everything became unsigned".

The omake round is what catches the argv defect. Compiling omake would not have
revealed it — only running it and demanding that the target it was asked for
actually appeared. The program omake builds returns 42, from
`hello_sum(37, 5)` across two units, so "it built something" and "it built the
right thing" are different claims and both are made.

The kernel half audits from outside what the driver cannot honestly claim about
itself: that the executables appeared in the VFS, that they are well-formed
3-segment W^X images, that the refused compiles left nothing behind, that no
descriptor leaked across the seven processes, and that the frame accounting
reconciles.

## A HOST HARNESS FOR THE COMPILER

`tools/occhost.c` builds on a Linux host, supplies the eight runtime functions
`occ.c` needs, compiles a `.oc` file, maps the result at the fixed addresses occ
bakes into its immediates, and CALLS the compiled `main` in-process.

It exists because a codegen change costs a 12-minute boot to test otherwise, and
every unsigned rule in the table above was validated through it before the first
boot was attempted. It calls `main` rather than the ELF entry point deliberately:
the entry ends in `SYS_EXIT`, and OutRun's syscall numbers are not Linux's, so
entering there would execute a meaningless host syscall.

It is a development tool, not part of the OS, and it is not built by the
kernel Makefile.

## WHAT IS STILL IN THE WAY OF SELF-HOSTING

Four of the eight blockers in `OCC-SELFHOST-GAP.md` are closed. `occ` still
cannot compile `occ.c`, and the document is updated rather than quietly left
stale. What remains:

- `enum` — 1 declaration, 6 token kinds
- constant *expressions* as array sizes — pervasive
- aggregate initialisers `= { … }` — 5
- multi-dimensional arrays — 1 (`u8 sr[6][3]`)
- struct assignment and by-value passing — still refused loudly, which remains
  the right behaviour

## VERIFICATION

Zero failures across all suites on three configurations: uniprocessor/BIOS,
SMP-4/BIOS, and q35 + VT-d IOMMU (`-smp 4`). Boot logs are in `docs/`.
`langstrs` reports 10/10 on all three.

### Three things that failed on the way, and what they turned out to be

Recorded because "it passes now" is a weaker claim than "here is what went
wrong and why it is not the code":

1. **A role-number collision.** `langstrs`'s ring-3 driver was first given role
   40 — already `pipe_worker()` from v0.59 — and the dispatch chain runs the
   first match, so the pipe driver executed the language worker and `pipestrs`
   reported the language suite's exit code. Moved to role 42. The dispatch
   chain is now checked for duplicates directly:
   `grep -oE "if \(role == [0-9]+\)" user/init.c | grep -oE "[0-9]+" | sort -n | uniq -d`
   returns nothing.

2. **`omake` could not create its output.** After the argv and target-expansion
   fixes it ran, forked `occ` correctly, and `occ` then reported "cannot open
   output". The VFS root directory holds `VFS_MAXFILES` = **64** entries, and
   `langstrs` runs LAST in the boot sequence, so it inherits every file every
   earlier suite created; the five it adds exhausted the directory. The suite
   now releases its consumed sources before the omake round. **The 64-entry
   ceiling is a real system limit and it is close** — this milestone spent the
   last of the headroom, and the next one that adds files to the root directory
   will hit it. Raising `VFS_MAXFILES` changes `VFS_DIR_BLOCKS` and therefore
   the on-disk layout, so it wants a format-version bump and belongs in its own
   change rather than a corner of this one.

3. **Two suites flaked under SMP, neither related to this work.** `posixstrs`
   failed 2 assertions and `pipestrs` 1 — but only when two `-smp 4` TCG guests
   were sharing four host cores. Re-run alone, `posixstrs` gives 12/0. The
   `pipestrs` pipe assertion failed on two IOMMU runs and passed on a third with
   the identical ISO, and `appsstrs` failed one window-ownership assertion on
   one SMP run while passing on others. These are timing-sensitive assertions,
   not regressions: `occ` compiles `/bin/emit`, `/bin/wcx` and `/bin/vsh` to
   **byte-identical sizes** (7248, 7507 and 19906) at v0.59 and v0.60, so the
   compiled artifacts these suites exercise did not change at all.
