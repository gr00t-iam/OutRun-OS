# OutRun OS v0.77.0-metal — the last spin count, and a quiet half of the tree

Milestone 77. v0.76 was about **evidence** — it built a dirty-volume gate, found
that the gate's own failure counter was blind, and converted three suites from
spin budgets to real-time deadlines. It closed carrying two admissions: the
conversion had covered `langstrs`, `toolstrs` and `pipestrs` but not the rest of
the idiom, and one `pthreads_smp` failure was unexplained.

This release finishes the conversion, corrects v0.76's account of that failure,
and raises ring 3 to the warning level the kernel has always been held to.

## THE REMAINING RING-3 WAITS

`owaitpid()` was converted in v0.76. Three more waiters were still budgeting
iterations, and iterations are not time.

| site | was | now |
|---|---|---|
| `pthread_join` | `k < 20000` retries | `osysticks()` deadline (`WAIT_T_JOIN`) |
| `kthread_join` | `k < 20000` retries | the same |
| `cs_compile` (`compilerstrs`) | `owaitpid(pid, 250000)` | `cs_compile_ticks(..., LANG_T_COMPILE)` |
| `compilerstrs` run wait | `owaitpid(pid, 250000)` | `owaitpid_ticks(..., LANG_T_RUN)` |

`kthread_join` was not on the carryover list. It is the same 20000-retry shape
on the same syscall, and converting one of the two would have repeated exactly
what v0.76 did when it fixed `langstrs` and left `toolstrs` and `pipestrs`
behind: **the idiom is the defect, not the call site.**

The spin-budgeted `cs_compile()` is **deleted**, not left beside its
replacement. A dead helper that still compiles is how the old shape comes back —
the next person to add a round copies whichever one their eye lands on.

### `compilerstrs` now prints what it measures

The converted stage prints its own elapsed time, as `langstrs` has since v0.76.
The two-unit build measures **91–119 ds (9.1–11.9 s)** across seven boots.

That figure is the point. It is roughly **4x** the 2.8 s `omake` compile whose
budget broke `langstrs` on `-smp 4` boot 1, which is why `compilerstrs` was
named the highest-risk unconverted site — and against the 200 s ceiling it is
~17x margin, now an **observed** margin rather than a guessed one.

## A DEADLINE IS NOT A DEFECT

Every converted site now reports a timeout distinctly from a failure:

| suite | defect | deadline |
|---|---|---|
| role 31 (`pthreads + mutex`) | 902 | **908** |
| `pthreads_smp` | 945 | **937** |

Both kernel decoders name them, and `posixstrs` prints an explicit
"a join DEADLINE expired — not a join defect" line for role 31, because that
round reports through a generic `exit N (want M)` path where a bare number tells
the reader nothing.

This is the same repair v0.76 made for `pipestrs`, where a hung child and a
broken pipe both exited 957 and would have sent a reader after a pipe bug that
does not exist.

## CORRECTION: WHAT THE v0.76 `pthreads_smp` FAILURE ACTUALLY WAS

`CHANGELOG-0.76.0.md` stated that `pthread_join` polled a **non-blocking**
`SYS_THREAD_JOIN` 20000 times and that this spin count **was the mechanism**
behind the one observed failure. Both claims were wrong, and both were reached
by analogy with carryover 2 rather than by measurement — the precise move v0.76
existed to distrust. The entry has been corrected in place.

`SYS_THREAD_JOIN` **parks** the caller: `block_ring3(sf, p, JOIN_KEY_OF(L, tid),
FUTEX_DEFAULT_TICKS)`, and `FUTEX_DEFAULT_TICKS` is 20000 ticks — **200 s** at
100 Hz. The measurement, from the preserved log:

```
[posixstrs] .. waiting spins=36923 ticks=35584 (+19802)
[pthreads_smp] driver exit 945 — pthread_join failed or timed out
```

**+19802 ticks** — the kernel's own park deadline expiring inside a **single**
call. The retry loop never went round twice. From outside, that boot took
**425 s** against 260 s and 220 s for its two siblings on the same image.

So the conflation was real and is fixed here. The spin count was real and is
converted here. **The mechanism was neither**, and the honest residue is
sharper than what v0.76 wrote down: **a wake was lost, and why is unknown.**

### What this release does NOT fix, stated where it cannot be missed

**A userland deadline cannot bound a single park.** `SYS_THREAD_JOIN` takes
`(tid, out_code)` and no timeout, so `WAIT_T_JOIN` is only checked *between*
syscalls: it bounds the "woken, still not done, ask again" path. If a wake is
genuinely lost, the first call still blocks for the kernel's 200 s and this
budget never gets to fire. Making that case fast needs a timeout argument in the
syscall ABI. The comment beside `WAIT_T_JOIN` says so, rather than implying a
fix it does not deliver.

`WAIT_T_JOIN` is 2000 ticks (20 s), deliberately below role 31's 3000-tick
`posix_drain` watchdog so the inner deadline fires first and the log names an
assertion instead of reporting "not every task reached a terminal state".

## THE WARNING SWEEP

46 warnings to 1. The build is clean under `-Wall -Wextra -Werror` for both the
kernel and ring 3; the single remaining diagnostic is a linker warning discussed
below.

| category | count | what was done |
|---|---|---|
| `-Wmisleading-indentation` | 16 | split `if (a) x; if (b) y;` onto separate lines — whitespace only |
| `-Wunused-function` | 5 | `__attribute__((unused))`, **not** deletion |
| `-Wsign-compare` | 5 | cast the unsigned side to `int`, matching the idiom already used feet away |
| `-Wshift-negative-value` | 5 | shift in the unsigned domain and convert back |
| `-Warray-bounds` | 5 | launder the fault address through an empty `asm` |
| `-Wcomment` | 4 | reorder globs so `/` never precedes `*` |
| `-Wunused-but-set-variable` | 3 | explicit `(void)` discard **plus a recorded finding** |
| `-Wdiscarded-qualifiers` | 1 | make the `volatile` discard an explicit cast |
| `ld: missing .note.GNU-stack` | 1 | added the marker to the four ELF asm objects |

Four of these deserve more than a table row.

**The deliberate faults.** Five sites write through `(volatile u32 *)0x1` to
prove the kernel reclaims resources on the FAULT exit path and not only on
`SYS_EXIT`. `-Warray-bounds` is a *true* observation about code whose entire
purpose is to be invalid. They now go through `fault_ptr()`, which launders the
address through an empty `asm` — the optimiser can no longer see the constant,
and the same store to the same address still faults. A `#pragma` would have
silenced the category everywhere, including somewhere it might one day be right.

**The unused functions are marked, not deleted.** `ocalloc` and `pthread_exit`
are API surface; `pci_probe_virtio` and `wimp_input_step` are readable
references. Deleting a working allocator to quiet a warning trades a real
capability for a cosmetic one.

**`-Wshift-negative-value` was not cosmetic.** `FXI(v)` was
`((int64_t)(v) << 16)`, and window physics uses negative coordinates constantly
— left-shifting a negative value is undefined (C11 6.5.7p4). It now shifts as
`uint64_t` and converts back, which is the identical bit pattern on two's
complement. The arithmetic did not change; the undefined behaviour did.

**A finding recorded rather than fixed.** The three set-but-unused variables in
the virtio-net probe are `notify_bar`, `isr_bar` and `devcfg_bar`. They are
unread because the driver **assumes every capability lives in `common_bar`** and
treats the others as offsets into it — a device that placed one elsewhere would
be mis-driven silently. `vblk`'s walk compares them and bails; `vnet`'s does not.
This sweep is cosmetic by contract, so the assumption is now stated in a comment
and recorded here **as a finding, not a fix**. QEMU puts all four in BAR 4,
which is why it has never bitten.

### Ring 3 was being held to a lower standard, and it hid three defects

`CFLAGS` has carried `-Wall -Wextra` for the kernel for a long time. `UCFLAGS`
carried `-Wall` alone. Raising ring 3 to parity surfaced three findings that the
quieter half had been keeping, and **two of them are real**:

- `if (sysc(SYS_SYSINFO, ...) < 0)` in **`osysticks()` and `osysncpu()`** —
  `sysc()` returns `u64`, so `< 0` was always false and neither error check
  could ever fire. Now cast to `i64`. A guard that cannot fail is the same class
  as a counter nothing prints, and these two sit under every tick deadline this
  milestone and the last one added.
- `round` in the SIGSEGV round-trip test was live across `osetjmp`/`olongjmp`
  without `volatile`, whose value after a `longjmp` is indeterminate
  (C11 7.13.2.1p3). It has worked because gcc happened to spill it. Now
  `volatile`.

`-Wextra` is now permanent for ring 3. A warning level that differs between two
halves of one tree hides defects in the quieter half — which is exactly what it
had been doing.

### The one warning left, and why it stays

```
ld: warning: build/outrun-kernel.elf has a LOAD segment with RWX permissions
```

Silencing this means either `--no-warn-rwx-segments`, which hides it, or
splitting the kernel image into separate RX and RW segments, which is a change
to the linker script and the boot path — not a cosmetic change, and not one to
make in the same commit as a warning sweep, days before a tag. A single RWX LOAD
segment is ordinary for a freestanding kernel of this shape. It is left visible
and named here rather than suppressed.

## VERIFICATION

```
outrun-os-0.77.0.iso
md5     863590eaf2536d45225d890ff3ee516e
sha256  6fc7a26bce1455eeffa345e4fefe9765132a3b695063b9ded39cdfd58f5d39b3
```

Every configuration below booted that exact image; each log's first line carries
the md5.

### Fresh-image matrix

| configuration | suites | passed | failed | rank faults | boot |
|---|---|---|---|---|---|
| uniprocessor (`make release-verify`) | 45 | 479 | **0** | 0 | 305 s |
| `-smp 4`, SeaBIOS | 45 | 495 | **0** | 0 | 225 s |
| `-smp 4`, q35 + VT-d, `intremap=on` | 47 | 508 | **0** | 0 | 230 s |

47 rather than 45 under VT-d is `iommu` and `capdma`, which are config-gated on
the emulated unit — the tell that the target really is running with an IOMMU.

Logs: `metal/docs/OUTRUN-0.77-boot-{uniprocessor,smp4-bios,smp4-iommu}.log`.

### Dirty-volume gate — and it FAILED

| run | binary | boot 1 | boot 2 | boot 3 | verdict |
|---|---|---|---|---|---|
| `make gate-dirty-smp` | v0.77.0 release ISO | 45 / 0 | 45 / 0 | 45 / **1 fail** | **FAIL** |
| repeat, same ISO | v0.77.0 release ISO | 45 / 0 | 45 / 0 | 45 / 0 | PASS |
| negative control | pre-sweep `00f7a8c` (md5 `6364ac09`) | 45 / 0 | 45 / 0 | 45 / 0 | PASS |

```
[mcpre  ] FAIL  long probe never started on cpu1
[mcpre  ] RESULT: 0 passed, 1 failed
```

**The `[mcpre]` anomaly v0.76 could not explain has reproduced.** Preserved,
md5-stamped, at `metal/docs/OUTRUN-0.77-gate-dirty-smp-boot3-mcpre.log` — v0.76
recorded its own occurrence as "preserved" at a path that was never committed
and is now gone, so this one is committed.

**The mechanism, read from the source rather than guessed at.** The assertion
queues a probe on cpu1, sends an IPI, and waits:

```c
uint64_t t0 = g_ticks;
while (!(kprocs[pl].ran_on & 2u) && g_ticks - t0 < 500) __asm__ volatile("pause");
if (!(kprocs[pl].ran_on & 2u)) { ... "long probe never started on cpu1" ... }
```

That is a **500-tick (5 s) wall-clock budget** for another vCPU to pick up queued
work — a constant chosen on an idle machine, on a TCG-only host where the guest's
four vCPUs are multiplexed onto host threads the guest does not control. It is
carryover 2's defect exactly, in **kernel** code this time rather than ring 3,
and it is the same reasoning that condemned `owaitpid`'s spin budget: the
quantity being bounded has no fixed relationship to the thing being waited for.

**What the evidence supports, and what it does not.**

- The suite has a documented `-smp 4` flake history predating all of this:
  `OUTRUN-0.47-boot-smp4-bios-flake.log` and
  `OUTRUN-0.48-boot-smp4-iommu-flake.log` are named "flake" in the tree. Those
  two are a **different** assertion in the same suite ("the captured context
  MIGRATED CORES"), so they are context, not precedent.
- *This* assertion has now fired **twice in the project's history** — once in
  v0.76, once here — both under `-smp 4`, never uniprocessor.
- **The negative control does not exonerate the warning sweep.** It passed 3
  boots, but at the observed rate of 1 in 6 a 3-boot control has roughly a 6-in-10
  chance of missing the failure even if the rate were unchanged. It is weak
  evidence, and calling it a clean bill of health would be the same error this
  project has made before. What carries more weight is that the sweep touched no
  scheduling, IPI or run-queue code, and that the failing budget is a constant
  that predates it by many milestones.

**Rate on this artefact: 1 failing assertion in 6 dirty-SMP boots** (two runs of
three), plus 0 in the three fresh-image configurations.

## WHAT THIS RELEASE DOES NOT DO

- **Carryover 3 — the fork race — is still unaddressed, for a third milestone.**
  v0.75 fixed three real defects on the path; the v0.74 symptom has never been
  reproduced without instrumentation, so "fixed" and "did not fire" remain
  indistinguishable. v0.76 recorded it explicitly rather than silently, and this
  release does the same. It is the oldest open item in the project and it has
  now outlived two milestones that each said it should not.
- **The lost wake behind the `pthreads_smp` failure is unexplained.** One
  occurrence in six dirty-SMP boots, never reproduced. This release makes it
  *report itself correctly* when it recurs — 937, with a decoder line — which is
  the difference between catching it next time and mistaking it again.
- **`SYS_THREAD_JOIN` has no timeout argument.** See above; the natural v0.78
  item, and the only thing that would bound a lost wake in useful time.
- **The virtio-net BAR assumption is documented, not repaired.**
- **The RWX LOAD segment** stands, deliberately.
- **No memory-hard KDF, no password-change syscalls, no reboot-surviving
  lockout, no confidentiality for the stored database.** Carried unchanged from
  v0.76; the KDF remains the largest structural gap.
- **No execute permission bit, no directory permissions, no supplementary
  groups, no login program, no lockout expiry, no administrative unlock.**
  Carried unchanged from v0.74.

## COVERAGE THIS GATE DID NOT PROVIDE

- **Bare metal and Proxmox are untested.** Every result is QEMU, TCG, no KVM.
- **One boot per fresh-image configuration**, three for the dirty one. These are
  not soak runs; a 1-in-6 event like the `pthreads_smp` wake loss would not be
  expected to appear, and its absence here is **not** evidence it is gone.
- **`make gate-dirty` (uniprocessor) was not re-run for this tag** — the SMP
  dirty configuration was, and it is the stricter of the two.
- **The warning sweep is verified by the gate, not by inspection of every
  site.** 16 of the 44 changes are whitespace, but the sweep touched a compiler,
  a kernel and a driver probe, and only a booting matrix distinguishes "compiles
  the same" from "behaves the same".
