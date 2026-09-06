# OutRun OS — working agreements

Repository conventions for anyone (human or agent) working in this tree.

The kernel is a single large C file, `metal/kernel/kernel64.c`; ring-3 test
drivers live in `metal/user/init.c`. Build and test targets are in
`metal/Makefile`. The toolchain is Linux-hosted (gcc, nasm, rustc,
grub-mkrescue, qemu). **QEMU here runs under TCG**, which is why timing budgets
in this tree must be expressed as deadlines rather than as iteration counts.

### TCG is a policy, not just a fact of the machine

This said "there is no KVM" until v0.91, and that is no longer true of the
hardware: `/dev/kvm` **exists**, and `qemu-system-x86_64 -accel help` lists
`kvm`. What keeps QEMU on TCG is that the build user is not a member of the
`kvm` group, so QEMU cannot open the device and falls back.

**Keep it that way, and change it only as a deliberate, re-baselined decision.**
Every timing budget in this tree was calibrated under TCG and they are not
independent of each other:

- suite deadlines in `metal/user/init.c` (`APPSMP_T`, `CASC_T`, `R62_T`, …)
- `GATE_CAP`, `GATE_DIRTY_CAP`, and the soak's `BOOT_CAP` / `ITER_CAP`
- every watchdog expressed in `g_ticks`

Enabling KVM would make boots several times faster and silently invalidate all
of them at once. Budgets that currently expire on a slow host would stop
expiring; a genuine stall would sit far inside a budget sized for emulation and
stop being reported. The first symptom would be tests that pass for the wrong
reason, which is the failure mode this whole file is organised against.

If it is ever switched on: add the user to `kvm`, re-measure every tier's time
to prompt, re-derive the budgets above from those numbers, and record the new
baseline the way v0.91 records the TCG one — in one commit, not incrementally.

**Do not diagnose a slow run by reaching for KVM.** v0.91 found the same ISO
booting 48% slower than it had a day earlier (310 s → 460 s, identical md5) and
proved by that control that no kernel change was responsible. The cause was
host-side and remains partly open — CPU frequency and power state are not
inspectable from inside WSL2, where `/sys/devices/system/cpu/*/cpufreq` does not
exist. See `metal/docs/ROADMAP-0.91.0.md` §1a for the full elimination, and for
the host benchmark recorded there so the next comparison has a baseline.

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
higher one is an inversion and will be reported.

**The complete set, and it must stay complete.** This list previously named only
`ofile` 1, `vfs` 2, `cas` 3, `vblk` 4, `surf` 5, `net` 9, `udb` 13 — six ranks
short. v0.95 proposed a new lock at rank 7 on the strength of it, and 7 has been
`g_gpu_lock` since v0.51. A partial rank table does not merely fail to help; it
actively invites a collision, which is the same trap this file already documents
for ring-3 role numbers. Grep `struct klock g_` before adding one.

| rank | lock | protects |
|---|---|---|
| 0 | `g_redir_lock` | fd redirection table |
| 1 | `g_ofile_lock` | open-descriptor array (fd alloc/free/deref) |
| 2 | `g_vfs_lock` | VFS directory: dirent claim/scan/rewrite/flush |
| 3 | `g_cas_lock` | CAS superblock counters, bitmap, index, staging sectors |
| 4 | `g_vblk_lock` | virtio-blk request slots + avail-ring publish |
| 5 | `g_surf_lock` | surface slot table + pixel-buffer free list |
| 6 | `g_ipc_lock` | IPC mailbox rings |
| 7 | `g_gpu_lock` | virtio-gpu resource / scanout state |
| 8 | `g_audio_lock` | virtio-sound stream state |
| 9 | `g_net_lock` | virtio-net rings and socket table |
| 10 | `g_wm_lock` | window-manager stacking order |
| **11** | **`g_dev_lock`** | **v0.95: device registry — claim/release, ownership** |
| 12 | `g_vm_lock` | vmfile mappings |
| 13 | `g_udb_lock` | user database (a leaf: never held while acquiring another) |

Two raw spinlocks are deliberately **UNRANKED** and are not `klock`s at all:
`g_frame_lock` (frame free-list — it must work before the scheduler exists and
on APs, where there is no per-CPU rank tracking, so it is never nested under a
klock and never held across an allocation or a yield) and `g_conlock` (console,
an IRQ-safe leaf inside `kprintf`). They are absent from the table because they
have no rank, not because anyone forgot them.

The common trap: helpers that take `g_ofile_lock` (rank 1) look harmless but
cannot be called while holding `g_net_lock` (rank 9). Validate against state the
lock you already hold protects, rather than re-deriving it through a lower-ranked
one.

`g_dev_lock` sits at 11 — above `net`, `gpu` and `audio` — precisely so a driver
holding its own lock can claim or release its device, since acquisition is
upward. The discipline that follows is: driver lock first, then `g_dev_lock`,
never the reverse.

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

---

## The desktop session (v0.96)

There are now **two images from one kernel**, and the difference is a single
word on the GRUB command line:

| image | built by | GRUB entry passes | kmain runs |
|---|---|---|---|
| `outrun-os-<V>.iso` | `make` | nothing | the ~45 regression suites, then `shell_run` |
| `outrun-desktop-<V>.iso` | `make desktop` | `desktop` | `desktop_run()`, forever |

**This separation is not tidiness, it is the gate's correctness.** Every timing
budget in this tree was calibrated against the suite boot; a desktop loop
running underneath the suites would change what they measure. `make gate` boots
the regression ISO and is untouched by anything in this section.

`make qemu-vga` now boots the DESKTOP image — that is what a windowed target is
for. The old behaviour (regression suites in a VGA window) is `make
qemu-vga-suites`.

### What the desktop actually consists of

- **Applications are ordinary boot modules**, one ELF each, in `apps/`. They are
  found by NAME (`mod_find`), not by module order. `multiboot_scan` used to
  overwrite `g_user_elf` on every module tag, so before v0.96 a second module
  silently displaced the first; the first module is still "the user ELF" so
  every existing suite loads exactly what it loaded before.
- **The launcher rail and taskbar chips are controls**, hit-tested against the
  same table and the same geometry function the compositor draws from
  (`g_launch`, `desk_chip_slot`). Before this they were painted and nothing
  tested them, so a minimized window could not be restored — the chips existed
  only as decoration. Never add a second copy of a hit box; that is the launcher
  version of the role-number trap above.
- **`desk_launch` queues, it never enters.** It is called from inside the
  desktop loop's input step, and running a ring-3 program there would suspend
  the compositor the program is about to wait on.
- **Windows may be PAIRED** (`SYS_WIN_CREATE` a2=1): two page sets, published by
  `SYS_WIN_DAMAGE`, which returns the buffer the app may now draw into. The
  compositor only ever reads the published set. `a2=0` is the legacy
  single-buffer window and behaves exactly as it always has. The back buffers
  live at `WIN_BACK_V(id)`, ABOVE all `NWMWIN` primary surfaces — one stride
  above the front buffer is window id+1's front buffer, which is a live trap.
- **`SYS_WIN_DAMAGE`'s a1 is an optional title.** a1 == 0 is the old call.
- **Apps are pinned to CPU 0**, which is also where the desktop loop runs. That
  is what makes the compositor's lock-free surface read sound: an app cannot be
  drawing on another core while this core composites.

### Desktop settings must stay honest

`SYS_DESKTOP_INFO` (117) and `SYS_DESKTOP_SETTINGS` (118) are declared in
`include/outrun_abi.h`, which is the master copy of those structs — the kernel's
local declaration must move with it, and the call REFUSES a size that disagrees
rather than partly filling the buffer.

Every accepted setting has a live consumer, and an out-of-range value is
**refused, not clamped**, so a settings app can distinguish "applied" from "not
supported":

- `scale` is applied in `fb_flip`, which magnifies the logical desktop on the
  way to the hardware. The window manager and pointer work in LOGICAL
  coordinates (`desk_w()`/`desk_h()`) and need no scale term.
- `accent` is painted by the compositor.
- `repeat_delay`/`repeat_period` drive auto-repeat in `wimp_input_step`. The
  PS/2 controller's own typematic rate is deliberately not used: it cannot be
  changed per-desktop, so a settings app pointed at it would be adjusting a
  number nothing honoured.

CPU figures in `SYS_DESKTOP_INFO` are the scheduler's real per-process
accounting (`proc_cpu_live`), in-flight excursion included. The old
`SYS_SYSINFO` had no time field at all, which is why the previous monitor drew a
tick-phase bar it had to label "activity" rather than load.

### Testing the desktop

- `make apps-test` runs the applications' host-side unit tests (`apps/tests/`)
  in about a second. Application logic — the calculator's overflow guard, the
  editor's cursor arithmetic, the monitor's CPU deltas — is tested here, not in
  a 300 s emulated boot.
- `tools/desktop-ui-test.py` drives the REAL input path over QMP and reads its
  verdicts out of the serial log.
  **Send `rel` events, never `abs`.** The machine has a PS/2 mouse and no
  tablet, so absolute events are delivered nowhere: the first version of that
  test clicked at absolute coordinates and watched the desktop sit on one window
  for 31,000 frames while it waited for launches that could not happen. It is
  the same class of mistake as a counter nothing increments — a test that
  cannot press the button has not tested the button.
