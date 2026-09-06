# OutRun OS 1.0.0 — the desktop session

First release with two published artefacts from one kernel. A single word on
the GRUB command line selects between them:

| image | boots |
|---|---|
| `outrun-os-1.0.0.iso` | the ~45 regression suites, then the shell |
| `outrun-desktop-1.0.0.iso` | `desktop_run()`: compositor, launcher rail, four native applications |

## ARTEFACTS

```
build/release/outrun-os-1.0.0.iso
  md5     6eeda6f3bcb27217e443c2b31c0d9a70
  sha256  43eb43dbc9f1c627d0701f0fb4f9705b90eb18bb34f14af6533a8e9637eee2c2

build/release/outrun-desktop-1.0.0.iso
  md5     cb4d7a2388bec4d84e1b98154fd28f54
  sha256  efc7930de00c2fa099779ee1f2fee7893e8583a3e8560d4416c2b627697d261a
```

Both built by `make release-iso` from a clean tree (`make clean` first) under
`-Wall -Wextra -Werror` for the kernel, ring-3 init, and every application.
Zero compiler warnings or errors. The kernel banner (`KERNEL_VERSION`), the
GRUB menuentries and `VERSION` all read `1.0.0`; `release-version-check`
confirmed the three agree.

## GATE TABLE

All on the release artefact `outrun-os-1.0.0.iso` (md5 `6eeda6f3…`) unless
stated; the desktop rows are on `outrun-desktop-1.0.0.iso` or the
development image that differs from it only by version string.

```
FRESH IMAGE (make gate, GATE_CAP=2400), run dir .logs/gate/matrix-1592
  uniprocessor                          PASS  45 suites  566 passed  0 failed  0 ranks  465 s
  smp2-bios                             PASS  45 suites  580 passed  0 failed  0 ranks  410 s
  smp4-bios                             PASS  45 suites  586 passed  0 failed  0 ranks  440 s
  smp4-iommu (q35 + VT-d, intremap=on)  PASS  47 suites  601 passed  0 failed  0 ranks  445 s

  Assertion counts are IDENTICAL to the pre-milestone baseline on 0.94.0
  (matrix-1465, md5 7e83f341…): 566 / 580 / 586 / 601. The desktop changed
  nothing the regression battery measures.

DIRTY VOLUME (one reused image, GATE_DIRTY_CAP=900)
  gate-dirty      3 boots, uniprocessor  PASS  diffs 1->2 and 2->3 empty; udbreboot,
                                               vfs-reboot-test and the CAS pending->
                                               recovered transaction survived every boot
  gate-dirty-smp  3 boots, -smp 4        PASS  same: empty diffs, all artefacts found

RELEASE VERIFY (the published artefacts themselves)
  outrun-os-1.0.0.iso      via release-verify.sh   PASS  474 passed, 0 failed, prompt
                                                         reached; rank violations=0
                                                         underflow=0 mismatch=0; log
                                                         stamped md5 6eeda6f3…
  outrun-desktop-1.0.0.iso via desktop-ui-test.py  PASS  20/20, 5,538 frames, the
                                                         copy in build/release/

DESKTOP
  apps-test          5/5 host unit tests, ASan+UBSan, -Werror       ~1 s
  desktop-test       20/20 checks, 1 vCPU, 5,695 frames             ~3 min
  desktop-test-smp   20/20 checks, -smp 4, 3,875 frames                 ~3 min
  desktop-soak       50,000 frames, 17 churn cycles (51 launches,
                     51 closes), window count back to 1 after
                     every cycle, frames_used 1451 at every resting
                     point (drift 0), no launch failure, no panic     504 s
```

The soak's first run, before its close-box geometry was corrected, ratcheted
to 12 open windows and reported an allocator "leak" that was three windows
still open. That failure is what the harness is for; it is now guarded by a
fail-fast on the first cycles and by sampling only at reported resting
points. Noted here so the corrected numbers are read against it.

## WHAT IS NEW SINCE 0.94.0

### The desktop (kernel)

- `desktop_run()`: a persistent compositor loop selected by `desktop` on the
  kernel command line. Nothing about the regression boot changed; `make gate`
  boots the same battery it always did.
- **Applications are boot modules**, one ELF each, found by NAME (`mod_find`).
  `multiboot_scan` used to overwrite `g_user_elf` on every module tag, so a
  second module silently displaced the first. The first module is still "the
  user ELF"; every existing suite loads exactly what it loaded before.
- **The launcher rail and taskbar chips are hit-tested controls**, against the
  same table (`g_launch`) and geometry (`desk_chip_slot`) the compositor draws
  from. Before this they were paint only; a minimized window could not be
  restored.
- **Paired windows** (`SYS_WIN_CREATE` a2=1): two page sets, published by
  `SYS_WIN_DAMAGE`, which returns the buffer the app may now draw into. The
  compositor reads only the published set. `a2=0` is unchanged.
- `SYS_DESKTOP_INFO` (117) and `SYS_DESKTOP_SETTINGS` (118), declared in
  `include/outrun_abi.h`. Every accepted setting has a live consumer and an
  out-of-range value is REFUSED, not clamped: `scale` is applied in `fb_flip`,
  `accent` by the compositor, `repeat_delay`/`repeat_period` by
  `wimp_input_step`. CPU figures are `proc_cpu_live`, the scheduler's real
  accounting.
- The desktop heartbeat now prints `frames_used=` by the same expression
  `SYS_DESKTOP_INFO` reports, so a soak can read the allocator out of the
  serial log. Without it, "no leak" would have been a claim about a counter
  nothing printed.

### The applications (`apps/`)

All four are freestanding C, `-mno-sse -msoft-float`, no libc, and each has a
host-side unit test under ASan+UBSan (`make apps-test`, about a second).

- **NUMWORKS** (`calc.c`): integer calculator, 18-digit bound. **1.0 adds
  operator precedence** (`*` `/` before `+` `-`, equal precedence
  left-to-right: `2+3*4=` is 14, `8-3-2=` is 3) with a two-level pending
  stack, and overflow/zero-divide are checked when the DEFERRED operation is
  finally applied (`1+999999999*999999999*9=` is `Overflow`). Errors are
  recoverable: C/Esc resets, a fresh digit clears.
- **VAULT PAD** (`vault_pad.c`): 256 KiB text editor with VFS open/save over
  an injected I/O function, so the real save loop runs under the host test
  against real files. **1.0 adds** `g`/`G` document jumps, a one-line
  clipboard (`y` yank, `D` cut, `p` paste; a line too long for it is refused,
  not truncated), and an **overwrite confirmation**: saving onto a file that
  exists and is not the one this buffer was opened from or last saved to
  asks once; the next save confirms, any other key withdraws. The probe opens
  with flags 0 (existing only), so it cannot create anything as a side
  effect.
- **SYS-DIAG** (`task_mgr.c`): measured per-process CPU from `cpu_ns` deltas
  matched by pid (pid reuse handled), allocator RAM from `frames_used`.
  **1.0 adds** click-to-sort by PID or by measured CPU delta — a stable sort
  producing an index table over the kernel's list, never reordering the list
  itself, so the cross-sample pid matching is untouched. There is
  deliberately **no memory sort**: `outrun_process` has no per-process memory
  field, and a column the kernel does not report would be decoration
  presented as measurement. The graphs now scale to the observed peak with a
  floor of 20 so an idle graph is not amplified noise.
- **CTRL DECK** (`settings.c`): **1.0 makes settings apply on the row click
  itself** — there is no APPLY button. If the kernel refuses, the local copy
  rolls back to the pre-click value, so the screen never shows a setting the
  desktop is not honouring. RELOAD re-reads the kernel's state.

### Verification (`metal/tools/`, `metal/Makefile`)

- `make desktop-test` drives the REAL input path over QMP (relative mouse
  events; the guest has a PS/2 mouse and absolute events go nowhere) and reads
  verdicts from the serial log and the framebuffer. 20 checks, including that
  a scale-2 setting genuinely magnifies the framebuffer (every 2x2 block
  uniform), which distinguishes a setting that was stored from one that was
  applied. `make desktop-test-smp` is the same at `-smp 4`.
- `make desktop-soak` (`tools/desktop-soak.py`): holds the session past
  50,000 composited frames while opening and closing three applications per
  cycle through their close boxes. Fails if the compositor stalls, any cycle
  fails to return the window count to 1, or `frames_used` at the resting
  points drifts by even one frame.
- `make gate-desktop` = `apps-test` + `desktop-test` + `desktop-test-smp`.
- `make release-iso` now builds and checksums BOTH artefacts; `make
  release-verify` boots both — the regression ISO through
  `release-verify.sh`, the desktop ISO through `desktop-ui-test.py` against
  the copy in `build/release/`.

## WHAT THIS RELEASE DOES NOT CLAIM

- Settings do not persist across boots; `SYS_DESKTOP_SETTINGS` is session
  state and the CTRL DECK says so on screen.
- The desktop soak is 17 churn cycles over 50,000 frames on one image at one
  vCPU. It cannot see a leak on a path it does not drive (it never minimizes,
  drags, or resizes), and it did not run at `-smp 4`.
- The no-tearing check in `desktop-test` SAMPLES six frames. It can catch
  tearing; it cannot prove its absence. The claim rests on the paired-buffer
  design plus CPU-0 pinning of the applications, which is what makes the
  compositor's lock-free surface read sound.
- Nothing was run on bare metal or Proxmox.
- `CLAUDE.md` still says the highest assigned ring-3 role is 54; it is 66
  (`role65_claim_negative`, `role66_claim_positive`, via `R[k].role`). That
  file is protected from agent edits and was left for a human to correct.
