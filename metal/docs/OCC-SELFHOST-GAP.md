# What `occ` still cannot compile: `occ.c`

**Status at v0.60: four of the eight blockers are CLOSED.** `occ` still cannot
compile its own source. This document records exactly what stands between it and
that, so the decision is auditable and the next milestone has a work list rather
than an aspiration.

Every count below was re-taken from the tree at **v0.60** with `grep`, and the
command is given so it can be re-run rather than trusted. The counts went UP for
the constructs that were closed, because closing them meant occ.c could use them
more freely — that is expected, and it is why the counts are re-measured rather
than carried forward.

## What v0.60 closed

| # | Construct | Status |
| --- | --- | --- |
| 1 | `unsigned` types (`u8`/`u16`/`u32`/`u64`) | **done** — built-in primitives, with `div`/`shr`/`setb` codegen and 1/2/4/8-byte narrowing |
| 2 | `sizeof` | **done** — `sizeof(TYPE)` and `sizeof EXPR`, the latter by parsing for the type and rewinding the output cursors |
| 3 | declaration in a `for` initialiser | **done** — shares the ordinary local-declaration parser, with a scope |
| 10 | `switch` / `case` | **done** — dispatch emitted after the body, so every arm offset is already known |

`break` and `continue` came with `switch`, for loops as well as switches.

See `CHANGELOG-0.60.0.md` for why `unsigned` is a code-generation change rather
than a parsing one, and for the table of which opcode each signedness selects.

## What is still in the way

### Why the rest was deferred rather than attempted

M59's job was process plumbing and a real shell. A compiler that can compile
itself but has no shell to drive it, no pipes to compose it with, and no
redirection to capture its output is a demo. The substrate comes first; the
language work is a milestone of its own, with its own suite.

There is also a correctness argument. Closing this gap means adding `unsigned`
arithmetic, `sizeof`, `enum` and multi-dimensional arrays to a single-pass code
generator with no AST. Each of those changes how expressions are *typed*, and
v0.58's element-scaled indexing already demonstrated how a typing change can
land with one path silently wrong for a whole release (see
`CHANGELOG-0.58.0.md`, and the parameter fix in `CHANGELOG-0.59.0.md`). That
work needs its own verification pass, not a corner of someone else's.

## A correction to an earlier claim

Previous notes in this project — including my own earlier summary this
milestone — listed **`switch`/`case`** among the constructs blocking
self-compilation. That is wrong. `occ.c` contains **no `switch` statement at
all**:

```
$ grep -c 'switch (' user/occ.c
0
```

The dispatch in `occ.c` is written as `if`/`else if` chains throughout, which
`occ` already supports. `switch` is a nice-to-have for the *kernel's* style, not
a blocker for self-hosting. It is removed from the list below.

## The actual gap

`occ.c` is 1,995 lines and is `#include`d into `user/init.c` (see
`Makefile:97`), so "compiling occ" means compiling that combination.

| # | Construct | Count in `occ.c` at v0.60 | How to reproduce | Blocking? |
| --- | --- | --- | --- | --- |
| 1 | `unsigned` integer types (`u8`/`u16`/`u32`/`u64`) | 105 uses | `grep -oE '\bu(8\|16\|32\|64)\b' user/occ.c \| wc -l` | **CLOSED in v0.60** |
| 2 | `sizeof` | 36 | `grep -c 'sizeof' user/occ.c` | **CLOSED in v0.60** |
| 3 | declaration in a `for` initialiser | 71 | `grep -cE 'for \((int\|i64\|u64) ' user/occ.c` | **CLOSED in v0.60** |
| 4 | `enum` | 1 declaration (6 token kinds) | `grep -n '\benum\b' user/occ.c` | **yes** |
| 5 | multi-dimensional array with initialiser | 1 (`u8 sr[6][3]`) | `grep -nE '\[[0-9]+\]\[[0-9]+\]' user/occ.c` | **yes** |
| 6 | aggregate initialisers (`= { ... }`) | 5 | `grep -cE '= *\{' user/occ.c` | **yes** |
| 7 | constant *expression* as an array size | pervasive | — | **yes** |
| 8 | struct assignment / passing by value | used in `init.c` | occ refuses it explicitly | **yes** |
| 9 | function pointers | 0 in `occ.c` | `grep -cE '\(\*[a-z_]+\)\(' user/occ.c` | no |
| 10 | `switch` / `case` | **0** | `grep -c 'switch (' user/occ.c` | **CLOSED in v0.60**, though it was never a blocker — see the correction above |

### 1. Unsigned types — the largest single item

`occ`'s keyword table is exactly:

```c
{ "int", "char", "return", "if", "else", "while",
  "for", "void", "__syscall", "__ldb", "__stb",
  "struct", "union", "typedef", 0 }
```

There is no `unsigned`, and `occ`'s `int` is a signed 64-bit machine word. This
is not a parsing problem — it is a **code generation** problem. Getting
`unsigned` right means unsigned division and modulo (`div` vs `idiv`), unsigned
comparisons (`jb`/`ja` vs `jl`/`jg`), and correct narrowing for `u8`/`u16`/`u32`
loads and stores. A compiler that parses `unsigned` and then emits signed
compares is worse than one that rejects it, because the failure is silent and
data-dependent.

`occ.c` also relies on the typedefs `u8`/`u16`/`u32`/`u64` that live in
`init.c`. `typedef` itself is supported (v0.57); the underlying types are not.

### 2. `sizeof`

Needs a compile-time type query. `occ` already computes and stores type sizes
for structs, members and pointers (`occ_elem_size`, `occ_align_of`), so the
information exists — what is missing is an expression form that reads it, plus
constant folding so `sizeof x` can appear where a constant is required.

### 3. Declarations in a `for` initialiser

`occ` parses `for` (`occ.c:1575`) but its initialiser slot calls `occ_expr()`,
which parses an *expression*. `for (int i = 0; ...)` therefore does not parse.
This is the cheapest item on the list — the initialiser needs to accept a
declaration statement and the loop needs a scope for it.

### 4. `enum`

Six token kinds in one declaration. The minimal implementation is a named
integer-constant table consulted by the identifier path — no code generation
changes at all. Cheap, and independently useful.

### 5–7. Arrays: dimensions, initialisers, and computed sizes

Three related limits:

- **Local arrays take exactly one `T_NUM`.** `occ`'s declaration parser reads a
  single number token, so `char buf[N + 8]` fails to parse — this is not
  theoretical, it is the exact error hit while writing `vsh.oc` this milestone
  (`expected ']'`), and the workaround was to spell the constant out. Fixing it
  requires constant-expression folding, which item 2 also wants.
- **No second dimension.** `u8 sr[6][3]` needs row-stride addressing.
- **No aggregate initialisers.** `= { ... }` at a declaration must emit
  initialised data (and, for the 2-D case above, in row-major order).

### 8. Struct assignment and by-value passing

`occ` refuses this outright and says so:

```c
occ_err("a struct cannot be passed by value; pass a pointer", occ_txt);
```

Supporting it means a real memory-to-memory copy at assignment and argument
setup, plus a calling-convention decision for aggregates. Refusing loudly is
the right behaviour for now; silently passing a pointer would be a miscompile.

## Suggested order for the NEXT milestone

The v0.59 list is reproduced with the finished items struck out, so the ordering
argument stays visible:

1. ~~**`enum`**~~ — still open. A constant table; no codegen change. Now the
   cheapest remaining item.
2. ~~**Declarations in `for`-init**~~ — done in v0.60.
3. **Constant-expression folding** — still open, and now the most valuable
   remaining item: it unblocks array sizes (`char buf[N + 8]`), and `sizeof` can
   already supply operands to it.
4. ~~**`sizeof`**~~ — done in v0.60.
5. **Aggregate initialisers** (1-D), then **multi-dimensional arrays**.
6. ~~**`unsigned` types**~~ — done in v0.60, with the arithmetic-identity suite
   the v0.59 note asked for. It is `langstrs`, and every unsigned assertion in it
   is written so that signed codegen gives the wrong answer.
7. **Struct by value.**

A note on tooling for whoever picks this up: `tools/occhost.c` compiles `occ.c`
on a Linux host and runs what it produces in-process, so a codegen change costs
seconds instead of a boot. Every rule in v0.60's unsigned table was validated
through it before the first boot was attempted.

Only after all of these is "compile `occ.c`" a meaningful target — and the
honest test of it is not that the compile *succeeds* but that the resulting
binary compiles a third-generation binary that is **byte-identical** to the
second. Anything less is a compile that happened to produce output.

## What is NOT in the way

Worth recording, because these were the hard parts and they are done:

- the preprocessor — `#include`, `#define` (object- and function-like),
  `#ifdef`/`#ifndef`/`#else`/`#endif` (v0.57)
- `struct` / `union` / `typedef` with real layout and alignment (v0.57)
- multi-unit compilation into one image (v0.57)
- element-scaled indexing and local arrays (v0.58, with the parameter path
  repaired in v0.59)
- a ring-3 heap, `execve` by path, and now pipes and a shell to drive it all
