# OutRun OS v0.58.0-metal — occ arrays, a native editor, and a native build tool

**Backfilled during v0.59.** This changelog was written after the fact, from the
merged diff, because v0.58 shipped without one. It records what actually landed
in `0.58.0-metal`, and — because the entry is retroactive — it is explicit about
which parts were verified as part of this write-up and which were inherited and
taken on the evidence of the code and the build.

## Provenance, stated plainly

v0.58 reached `main` as PR #21, commit `263d6cb`, merged as `cabb460`. That
commit's message describes **only** the occ array work. The commit itself also
contains `user/vedit.oc`, `user/omake.oc`, `tools/mkstr.py`, the `demo.mk` /
`hello_*` example project, and a `KERNEL_VERSION` bump to `0.58.0-metal` — about
1,700 lines that the message never mentions, because the commit was made with a
blanket `git add -A` rather than a reviewed file list.

Nothing appears to have been lost or corrupted: the tree builds, and the boot
suites that existed at the time ran. But the release went out undocumented and
partly undescribed, and this file exists to close that gap rather than to paper
over it.

| Area | Status |
| --- | --- |
| occ array indexing + local arrays + reentrant `libc.oc` | **Verified** — covered by `toolstrs`/`compilerstrs`, and re-audited during v0.59 (a real defect was found, see below) |
| `SYS_TTY_READ` (60), `SYS_STAT` (61) | **Verified** — present, wired, and used |
| `tools/mkstr.py` + `/src` publishing | **Verified** — the build regenerates `sdk_sources.h` and the files appear in the VFS |
| `vedit.oc` (the editor) | **Inherited, unverified** — no automated suite exercises it |
| `omake.oc` (the build tool) | **Inherited, and now known to be defective** — see below |

## What changed

### occ: element-scaled indexing

Before v0.58 every `a[i]` was scaled by 8, which is correct only for `int *`.
A new `esize` — the width of one step — is now carried on every type, so
`char *s` steps one byte, `struct P *p` steps `sizeof(P)`, and `p->buf[i]`
indexes the member rather than eight bytes at a time.

This is why `__ldb` stopped being necessary to walk a string, and it is the
single change the rest of v0.58 rests on.

### occ: local arrays

A local declaration used to reserve exactly one 8-byte slot, so `char buf[64];`
could not be written at all. Locals now reserve `nelem` elements. The direct
consequence is that `libc.oc`'s scratch buffers moved from globals into locals,
which made `putdec`, `puthex` and `putchar` **reentrant** — they had not been,
and the previous changelog said so.

### Two syscalls

- **`SYS_TTY_READ` (60)** — non-blocking console read, so a ring-3 full-screen
  program can get keystrokes. Non-blocking on purpose: there is still no
  sleep/wake queue for ring 3.
- **`SYS_STAT` (61)** — returns a file's content hash and length. It exists
  because this filesystem has **no timestamps**; a build tool has to compare
  content, so the ABI exposes what a content-addressed store actually knows
  rather than pretending to a clock it does not have.

### The userland source tree

`tools/mkstr.py` converts real files under `user/` into C string literals at
build time, and the kernel publishes them into `/src`. The motivation is
reviewability: several hundred lines of occ-subset C embedded as `\n`-spliced
quotes inside `kernel64.c` cannot be diffed, compiled or reviewed.

`/src/vedit.c` and `/src/omake.c` are published as **source**. No prebuilt
binary ships — the running system compiles them with its own compiler.

## Defects found in this code during v0.59

Documenting these here rather than only in the v0.59 notes, because they are
v0.58's, and a backfilled changelog that records only the good news would be
worth less than no changelog.

### 1. `char *` parameters were indexed with the wrong element size (fixed in v0.59)

The element-size work missed one path. For a **parameter**, the base type's
width was overwritten by the pointer's width before the element size was
computed:

```c
int psidx = -1, pptr = 0, psize = 8;
if (occ_is_type()) occ_parse_type(&psidx, &pptr, &psize);   /* psize = 1 for char */
while (occ_accept("*")) { pptr++; psize = 8; }              /* clobbered */
L->esize = occ_elem_size(psidx, pptr, psize);               /* -> 8, should be 1 */
```

Globals and locals both pass the *preserved* base size and were correct; only
parameters were wrong. So `char *s` as a parameter stepped **eight bytes** per
index while the identical declaration as a local stepped one — which meant
`libc.oc`'s `strcmp()` compared every eighth byte and reported two equal strings
as different.

This shipped broken: `toolstrs` fails on `main` at `cabb460` with
`exit 946 — the compiled program returned the wrong answer`, and the failing
assertion is `strcmp("abc", "abc") != 0`. **v0.58 was released with a failing
suite.** Fixed in v0.59; `toolstrs` now reports 9 passed, 0 failed.

### 2. `omake.oc` builds a broken `argv` vector (NOT fixed — still outstanding)

`omake.oc` builds the vector it hands to `execve` through a `char *`:

```c
char *mk_av;                 /* allocated (MK_MAXTOK + 2) * 8 bytes */
av = mk_av;
av[0] = "/bin/occ";          /* comment says "a WORD array" */
```

That comment describes the **pre-v0.58** rule. Under element-scaled indexing a
`char *` steps one byte, so each `av[i] = ptr` stores only the **low byte** of
the pointer and the resulting `argv` is garbage. The fix is a one-word type
change (`int *`, which is what `vsh.oc` uses for exactly this reason), but it
is left alone here because nothing in the boot matrix exercises `omake`, so the
change would be unverified either way — and shipping an unverified fix to
inherited code is how the situation above arose.

**`omake` should be assumed non-functional until a suite covers it.**

## Verification

The suites that existed at v0.58 ran, with one failure that was not caught at
the time: `toolstrs` (see defect 1). Everything else was green. `vedit` and
`omake` have no automated coverage in any configuration.
