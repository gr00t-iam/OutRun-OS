# Outrun OS v0.47.0-metal — User-Space Interrupt Architecture & VFIO MMIO Mapping

A ring-3 driver can now map a device's MMIO BARs directly into its own
address space and block waiting for that device's routed hardware
interrupt — no kernel-mode driver, no host scheduling in the wait path, and
(per this kernel's now-established discipline) no way to leak the mapping,
the grant, or the interrupt-line ownership if the driver dies mid-flight.

## What's new

- **`SYS_VFIO_MAP_BAR(device_id, bar_index, flags)`** (syscall 20), gated
  behind a new `PCAP_VFIO` capability. `device_id` resolves exactly like
  `sys_hardware_passthrough`'s existing handle (`0xFFFF` = the demo device,
  otherwise a `kdevs[]` index) — isolation is structural, not a checked
  parameter: the syscall never accepts a raw physical address from
  userspace, only a lookup through the kernel's own device registry, so
  there is no path to map arbitrary system RAM as if it were MMIO.
- **`bar_index`**: this kernel's device model has never had more than one
  discrete MMIO region per device (virtio's own common/notify/isr/devcfg
  split is offsets *within* one BAR, not separate BARs) — rather than
  fabricate a multi-BAR array with no real backing, `bar_index 0` always
  means the device's existing region, and `bar_index 1` is valid only for
  devices that register a genuine second region via a new, minimal parallel
  table (`g_kdev_bar1_phys/len`). Only the new VFIO test device does.
- **`SYS_VFIO_MAP_BAR` reuses `dma_grant_create(..., DMA_GRANT_MMIO, ...)`**
  — the exact mechanism `sys_hardware_passthrough` already uses — rather
  than inventing a parallel grant table. This means `dma_teardown_kproc`
  (wired into all three exit paths since v0.44) and `page_free_tree`'s
  device-MMIO double-free guard (v0.45) already fully unmap a BAR and
  restore its frame's ownership on exit; nothing new needed reimplementing
  for that half of the milestone.
- **`SYS_VFIO_WAIT_IRQ(vector_id, timeout_ms)`** (syscall 21): blocks
  (spin-wait via the existing `krelax()` primitive — the same one
  `SYS_IPC_RECV` uses) until the named IRQ line's pending-interrupt counter
  advances, or the timeout (in real milliseconds, converted against the
  100 Hz PIT) elapses. Ownership-checked: only the process that most
  recently mapped the BAR of the device that owns a line may wait on it.
- **`isr_dispatch`'s real device-IRQ path now flags pending state for every
  line**, not just the ones a ring-3 waiter cares about today: every actual
  hardware IRQ this kernel already dispatches (virtio-blk, virtio-net, PS/2
  mouse) bumps the same per-line counter `SYS_VFIO_WAIT_IRQ` polls. The
  plumbing a ring-3 waiter observes is the genuine hardware path.
- **`vfio_teardown_kproc`**, wired into all three exit paths immediately
  before `ipc_teardown_kproc` (same ordering discipline v0.44-v0.46
  established, extended one step earlier). Deliberately tiny: since BAR
  unmapping and grant/frame lifecycle are already fully covered by reusing
  `dma_grant_create`/`dma_teardown_kproc`, the only thing v0.44-v0.46 knew
  nothing about — IRQ-line ownership — is all this function releases.
- **`DEBUG_VFIO`** (`g_debug_vfio`, default off): logs every BAR map, every
  IRQ wait outcome, and every teardown release. Verified live — a captured
  trace showed the exact expected sequence across 15+ recycled rounds
  (`MAP_BAR ... owns IRQ line 16` / `WAIT_IRQ line 16 fired` / `released
  IRQ line 16 ownership`, with the SAME recycled kproc slot reused every
  round) — then reverted to the shipped default.
- **`cmd_vfio_stress`** (new suite, `vfiostress` command): 15 rounds of a
  ring-3 "driver" (role 14, `user/init.c`) that maps a dummy test device's
  two BARs (verifying BAR0's kernel-stamped sentinel and round-tripping a
  pattern through BAR1), waits on its routed interrupt, and exits — reusing
  v0.45's kproc recycling exactly like every `*_stress` suite since.

  ```
  [vfiostrs]  PASS  every round's driver exited cleanly (exit == own pid: BAR reads/writes and IRQ wait all verified in ring 3)
  [vfiostrs]  PASS  no DMA/MMIO grant survived past any round's teardown
  [vfiostrs]  PASS  no IRQ-line ownership survived past any round's teardown
  [vfiostrs]  PASS  BAR0's sentinel survived every round's teardown (device frame never freed)
  [vfiostrs]  PASS  free-frame count reconciles (g_frame_free_depth == lifetime freed - reused)
  [vfiostrs]  PASS  the frame allocator's leaf lock never triggered a rank violation (no double-free race)
  [vfiostrs] RESULT: 7 passed, 0 failed
  ```

## Why the simulated interrupt fires from the timer tick, not a real device IRQ

The dummy test device is software-only (two `alloc_frame()` pages standing
in for BAR0/BAR1, same convention as `cmd_passthrough`'s `sensor0`), so
nothing will ever spontaneously interrupt it the way real hardware would.
Hijacking an ALREADY-real device's IRQ line (virtio-blk/net) for this test
was rejected: those lines are load-bearing for other suites, and racing a
synthetic "interrupt" onto a line real hardware also drives would be a
genuine, if narrow, correctness hazard for no real benefit. Instead, lines
`[16,24)` are reserved, software-only vectors `isr_dispatch`'s real IRQ path
never touches; `cmd_vfio_stress` arms `g_vfio_test_fire_at` before
dispatching its driver, and the SAME timer-tick handler that already drives
every other periodic thing in this kernel (`g_ticks`, the integrity sweep,
preemption) fires it once, a few ticks later. This is reentrant and safe
under any core count — including uniprocessor, verified — because it needs
no second core: the timer IRQ can interleave with a spinning
`SYS_VFIO_WAIT_IRQ` on the SAME core, exactly the way `SYS_IPC_RECV`'s
blocking wait already relies on interrupts staying enabled during a
syscall's spin-wait.

## Honest scope decisions

- **Write-combining (`flags` bit 0) is accepted but not implemented.** Every
  mapping is uncacheable (`PTE_PCD`) regardless of the requested flag. Real
  WC needs a PAT entry this kernel's default, unreprogrammed `IA32_PAT`
  doesn't have — reprogramming the PAT is a global change affecting every
  existing mapping's cache behavior system-wide, legitimately out of scope
  for one syscall. Documented in the code, not silently dropped.
- **`bar_index` only ever has two valid values, and only for one device.**
  See above — this kernel's device model has no genuine multi-BAR hardware
  to back a richer implementation honestly.
- **IRQ-line ownership is single-owner, not shared.** Mapping a BAR again
  (by the same or a different process) simply reassigns line ownership to
  whoever mapped most recently — matches real VFIO's "the process that
  opened the device group owns the eventfd" model for the one-owner-at-a-
  time case this milestone's test exercises, but doesn't model VFIO's
  actual multi-process device-group semantics.
- **`VFS_MAXFILES` bumped 26→27** for one more fixed, reused (not
  device-id-keyed) name (`"vfio-devid"`) `cmd_vfio_stress` needs
  permanently — not a growth pattern, same reasoning as v0.46's bump.
- **`DMA_GRANT_MMIO`'s revoke path still has no live-fire proof from a
  device with a real PCI `bdf`** (unchanged since v0.44) — this milestone's
  own VFIO grants are also all `bdf=0xFFFF` (the test device isn't real PCI
  hardware), so that gap is not closed here either.

## Verification

| Config | Result |
|---|---|
| `-smp 4` BIOS ISO | 0 FAIL, 23 suites incl. `vfiostress` 7/7 |
| `-smp 4` q35 + intel-iommu | 0 FAIL, all green incl. iommu 2/2, capdma 11/11, `vfiostress` 7/7 |
| uniprocessor | 0 FAIL, `vfiostress` 7/7 (simulated IRQ delivered on the same core as the spinning waiter) |

Compositor/vfiostress screendump captured at the shell on the IOMMU run
(`OUTRUN-0.47-vfiostress-iommu.png`).

**One flake observed and disclosed, not silently re-rolled:** the first BIOS
verification attempt hit the exact same `mcpre` (v0.39, unrelated to this
milestone) failure documented in v0.46's changelog —
`the captured context MIGRATED CORES: started on cpu1, finished on cpu2`.
An immediate re-run with an identical binary and a fresh disk reproduced
0 FAIL across all 23 suites, including `mcpre` 5/5 — the same pre-existing,
timing-sensitive flake, now observed on a second, independent occasion.
Both this milestone's changes and v0.46's touch nothing in `mcpre`'s
IPI-preemption/migration machinery. The failing attempt's log is kept
alongside the clean one (`OUTRUN-0.47-boot-smp4-bios-flake.log`) rather
than discarded.

## Honest scope gaps

- **No genuine multi-process VFIO device-group model** (see above).
- **Write-combining not implemented** (see above).
- **`DMA_GRANT_MMIO`'s revoke path still has no live-fire proof** (unchanged
  since v0.44).
- **kproc table still has a `MAX_KPROC` concurrent-live cap** (v0.45,
  unchanged).
- VT-x unavailable under TCG; IOMMU/VT-d fully real; NVMe/UEFI boot-order
  quirk remains unexercised, upstream of the kernel (all unchanged).
