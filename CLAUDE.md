# OutRun OS — working agreements

Repository conventions for anyone (human or agent) working in this tree.

The kernel is a single large C file, `metal/kernel/kernel64.c`; ring-3 test
drivers live in `metal/user/init.c`. Build and test targets are in
`metal/Makefile`. The toolchain is Linux-hosted (gcc, nasm, rustc,
grub-mkrescue, qemu). QEMU here is **TCG-only** — there is no KVM — which is why
timing budgets in this tree must be expressed as deadlines rather than as
iteration counts.

---

## Release Protocol — MANDATORY for every version tag

**A release is not a tag. It is a tag plus an ISO that was built from a clean
tree, checksummed, and booted.** No version tag is complete until all four steps
below have been performed and their output recorded in the release notes.

This is mandatory because it has already gone wrong: `v0.75.0` was tagged while
`VERSION` in `metal/Makefile` still read `0.74.0`, so every image produced for
that release was named `outrun-os-0.74.0.iso`. The protocol exists to make that
class of mistake impossible to complete silently.

### 1. Bump `VERSION` before tagging

`VERSION` in `metal/Makefile` names the artefact. Bump it, commit it, and only
then create the tag. `make release-iso` compares `VERSION` against
`git describe --tags` and warns loudly when they disagree — heed it.

### 2. Build the release ISO from a clean tree

```
cd metal
make release-iso
```

This runs `make clean` first, on purpose. An incremental build can carry an
object file from a source state that no longer exists anywhere in the tree, and
a published artefact is the worst place to discover that.

It produces, in `metal/build/release/`:

- `outrun-os-<VERSION>.iso`
- `outrun-os-<VERSION>.iso.md5`
- `outrun-os-<VERSION>.iso.sha256`

### 3. Verify the ISO actually boots

```
make release-verify
```

"It compiled" and "it boots" are different claims, and an artefact that has only
been built has been checked for neither. This boots the exact image that will be
published, from a fresh volume, and **fails** unless:

- it reaches the shell prompt,
- every suite that reported did so with zero failures,
- no lock-rank violation, underflow or mismatch appeared.

`make release` does steps 2 and 3 in that order.

### 4. Record the checksum in the release notes

Publish the MD5 (and SHA-256) alongside the tag. An image whose checksum is not
written down anywhere cannot later be shown to be the one that was tested.

### Why the checksum requirement is not ceremony

Twice in this project a test run has been discovered to have booted a **different
image** than the one it claimed to be testing — once invalidating a 20-boot
verification whose conclusion had to be publicly withdrawn. Every harness in
`metal/tools/` now stamps the md5 of the image it booted into every log it
writes, and release artefacts carry theirs beside them. A log that cannot name
the binary it came from is not evidence.

---

## Gate configurations

The release gate is not one command. Before tagging, all of these should be
green, and the release notes should say which were run and which were not:

| target / harness | what it covers |
|---|---|
| `make gate` | **all three fresh-image configurations, in one command** |
| `make gate-dirty` | 3 boots on ONE reused image, uniprocessor |
| `make gate-dirty-smp` | 3 boots on ONE reused image, `-smp 4` |
| `make gate-all` | `gate` + both dirty gates |
| `make gate-selftest` | the gate's own run classifier, on synthetic logs (<1 s) |
| `make release-verify` | the published artefact itself |

`make gate` (v0.78, `tools/gate-matrix.sh`) runs uniprocessor, `-smp 4` SeaBIOS
and `-smp 4` q35+VT-d with `intremap=on`, each on a fresh image. Before it
existed, only the first of those had a target: the other two were driven by a
harness that lived outside the tree, so the release matrix was reproducible by
exactly one person. That is a habit, not a gate.

It also prints a **coverage line** naming what it did not test, and it counts
failures twice — matching assertion lines against the suites' own `RESULT`
tallies — with disagreement failing the gate on its own. See below for why.

**A run that did not complete is not a verdict.** The classifier decides
completeness before correctness: `TRUNCATED` (we killed it at `GATE_CAP`) and
`NO-PROMPT` (it died on its own) outrank `FAIL`, and both are named in the
coverage line as untested. Before v0.81 these were sequential assignments to one
variable, so whichever fired last won — a boot cut off mid-`cas` at the 900 s cap
was reported as `FAIL` naming `[vfiostrs]` and `[capdma]`, which had merely been
the suites that reported before the kill. `make gate-selftest` pins that case;
`make gate` runs it first.

`GATE_CAP` defaults to 900 s. `smp4-iommu` needs more than that on a slow host —
if it reports `TRUNCATED`, raise the cap and re-run rather than reading the
assertions it printed.

The single-configuration targets (`make qemu`, `make qemu-iommu`) still exist
and are the right thing for interactive work; they do not check anything.

`gate-dirty*` exists because every other configuration builds a fresh disk per
boot, which is why the suite set was able to be non-idempotent across boots for
its whole history without anyone noticing. Persisted state is a supported
configuration; it needs a configuration that tests it.

---

## Evidence conventions

These are not style preferences. Each was adopted after a specific failure in
this repository.

- **Stamp the image.** Every harness writes the md5 of the ISO it booted into
  every log it produces. See above for why.
- **A counter nothing prints is not instrumentation.** A stale-handle counter
  was once incremented but never emitted; a verification run then grepped its
  logs for a string no code could produce and read the resulting zeroes as
  evidence of correctness.
- **A counter nothing *increments* is not evidence either.** The same mistake
  one step earlier. `g_reproc_stale_ppid` was printed on every boot and read
  zero on every boot — because its only increment site was inside
  `#ifdef FORK_RACE_REPRO`, so in a shipping build no code could raise it. Zero
  meant "unreachable", not "did not happen". Any counter that gates a claim must
  be live in the build the claim is about. Where a guard is being verified,
  count the DETECTIONS (the guard fired) separately from the FAILURES (it fired
  and did the wrong thing anyway): detections > 0 is what proves the boot
  exercised the path at all, and a suite that asserts only the failure count is
  green on a workload that never reached the code. See `ppid_live()`.
- **A test that cannot fail has not passed.** Before believing a new assertion,
  build the tree with the fix reverted and watch it fail. Carryover 3's harness
  reported 12/12 against a deliberately broken kernel for a whole session
  because the workload could not reach the defect; the reverted build is what
  exposed that, and it is now the standing check for any guard-verifying test.
- **Negative controls.** Before attributing a failure to a change, reproduce it
  on the build *without* the change. Two "fixes" in this tree were disproven
  this way, and one pre-existing defect was correctly cleared of blame.
- **A timeout must be a deadline.** `SYS_WAITPID` is non-blocking, so a ring-3
  "timeout" expressed as a spin count budgets the *waiter's* iterations, not
  time — and means completely different durations at 1 vCPU and at 4. Use
  `owaitpid_ticks()`; `SYS_SYSINFO` reports `g_ticks` at 100 Hz.
- **Don't assume a search was complete.** Truncated `grep | head` output has
  twice produced a confidently wrong conclusion here — once missing two of five
  call sites, once reporting a Makefile variable as undefined when it was
  defined 7 lines past the cut.
- **Say what was not covered.** A gate whose gaps are invisible is how
  "verified" drifts away from "measured".

---

## Lock ranks

`klock` enforces an acquisition order. Acquiring a lower rank while holding a
higher one is an inversion and will be reported. Current ranks: `ofile` 1,
`vfs` 2, `cas` 3, `vblk` 4, `surf` 5, `net` 9, `udb` 13.

The common trap: helpers that take `g_ofile_lock` (rank 1) look harmless but
cannot be called while holding `g_net_lock` (rank 9). Validate against state the
lock you already hold protects, rather than re-deriving it through a lower-ranked
one.

---

## Verified baseline — v0.82 development

**Commit `2a83086`** (`user/init & kernel: add Role 54 setuid/setgid ring-3
privilege drop worker`, #93), image md5 `2eeeba2aaf9b9c42c8616dbcf6800564`,
measured 2026-08-16. Every tier below reached the shell prompt with zero failing
assertions, zero lock-rank violations, zero underflow/mismatch and zero panics.

| tier | boots | assertions | result |
|---|---|---|---|
| `uniprocessor` | 1 | 495 | PASS (300 s) |
| `smp2-bios` | 1 | 505 | PASS (225 s) |
| `smp4-bios` | 1 | 511 | PASS (220 s) |
| `smp4-iommu` (q35 + VT-d, `intremap=on`) | 1 | 524 / 47 suites | PASS (225 s) |
| `gate-dirty` (one reused image, uniprocessor) | 3 | 495 each | PASS |
| `gate-dirty-smp` (one reused image, `-smp 4`) | 3 | 511 each | PASS |

**5,053 assertions, 0 failed**, across 10 boots and ~43 minutes of guest time.
The clean rebuild emitted no compiler warnings or errors.

Two things this baseline does **not** say, recorded because a baseline whose
gaps are invisible is how "verified" drifts from "measured":

- It is one boot per fresh configuration and three per dirty configuration. It
  cannot see an intermittent below roughly 1 in 10 boots.
- No release ISO was built or `release-verify`'d for `2a83086` itself — it was a
  development baseline, not a tag. **v0.82.0 was tagged later** from `701b5fe`,
  on artefact `outrun-os-0.82.0.iso` (md5 `0a077f3660a68674e4a78b18842abaa2`),
  which did pass `release-verify`; see that tag for its own gate table. v0.83.0
  followed the same way — each tag carries its own artefact and gate table, so
  read the tag rather than this section for anything but the v0.82 baseline.

`smp4-iommu` needs `GATE_CAP=2400` on the reference host; at the 900 s default it
is cut off before the prompt and reports `TRUNCATED`.

---

## Ring-3 worker roles

A ring-3 test worker is selected by number, not by name. The kernel sets
`kprocs[p].role = N` before `elf_load`, and `user/init.c`'s dispatch table runs
whichever `if (role == N)` matches. **The two halves are matched by nothing but
the integer**, which makes the numbering a shared namespace with the same
hazards as the lock ranks above.

Highest assigned: **54**. Recent: 52 `mcq_resident_probe` (cmd_mcq), 53
`posix_orphan_worker` (posixstrs), 54 `setuid_privdrop_worker` (usersstrs).

Two failures in v0.81/v0.82, both cheap to avoid:

- **Check for a free number before adding one.** `cmd_mcq` and `cmd_mcpre` shared
  role 7 for two suites with opposite requirements — mcq wanted a long-resident
  probe, mcpre's high-priority thread had to finish *first*. Making role 7
  resident to fix mcq broke mcpre in 4 of 8 `-smp 4` boots. Grep both
  `\.role = ` in the kernel and `role == ` in `init.c`; they can disagree.
- **A comment naming a role is not evidence the role exists.** usersstrs claimed
  for several releases that "setuid/setgid are exercised from ring 3 by role 52"
  when role 52 was unassigned and no ring-3 caller of `SYS_SETUID` existed at
  all. Nothing checks that a comment's subject is real — v0.82 added role 54 to
  make the claim true rather than deleting it.

A worker reports by **exit code**, decoded at its call site: pick one success
sentinel and give every failure point a distinct code, so a suite can name which
rule broke instead of reporting that the worker failed. Keep deadline expiry on
its own code — a slow host must never be decoded as a defect.
