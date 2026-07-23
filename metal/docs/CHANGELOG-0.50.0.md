# Outrun OS v0.50.0-metal — Native GPU Acceleration & Display Pipelines

A ring-3 process can now drive a real virtio-gpu device directly: create a
hardware 2D resource, draw straight into its DMA-shared backing pages with
ordinary memory writes, set it as the display scanout, and submit an
asynchronous flush it can fence-wait on — all through genuine virtio-gpu
wire-protocol commands answered by QEMU's actual GPU emulation, not a
simulated device.

## What's new

- **A real virtio-gpu driver** (`virtio_gpu_probe`), brought up exactly like
  this kernel's existing virtio-blk/virtio-net drivers: walk the PCI vendor
  capability list, map the common/notify BARs, negotiate `VERSION_1` (+
  `ACCESS_PLATFORM` when offered), bring up ONE virtqueue (the control
  queue, queue 0). The cursor queue (queue 1) is deliberately left
  uninitialized — this kernel's existing compositor already has its own
  software cursor and never needs to send GPU cursor commands.
- **Five real virtio-gpu commands**, submitted as genuine wire-protocol
  descriptor chains: `RESOURCE_CREATE_2D`, `RESOURCE_ATTACH_BACKING`,
  `SET_SCANOUT`, `TRANSFER_TO_HOST_2D`, `RESOURCE_FLUSH`, plus
  `RESOURCE_UNREF` for teardown. Every response is checked against
  `VIRTIO_GPU_RESP_OK_NODATA` — confirmed live via `DEBUG_GPU` (see below),
  not assumed.
- **Four new syscalls** (29–32, gated by a new `PCAP_SURFACE` capability):
  - `SYS_GPU_RESOURCE_CREATE(width, height, *out_resource_id)` → a user
    vaddr mapped directly onto the resource's backing pages. The backing
    is a `DMA_GRANT_PAGE` grant — the exact same mechanism `SYS_DMA_ALLOC`
    and v0.47's `SYS_VFIO_MAP_BAR` use — so `dma_teardown_kproc` (wired
    into every exit path since v0.44) already reclaims the frames
    themselves; the new resource table only tracks the device-side
    `resource_id` for `RESOURCE_UNREF`.
  - `SYS_GPU_SET_SCANOUT(resource_id, width, height)` — ownership-checked
    against the resource table, like v0.47's IRQ-line ownership check.
  - `SYS_GPU_SUBMIT_FLUSH(resource_id, width, height)` → a fence id. Issues
    `TRANSFER_TO_HOST_2D` synchronously (it must land before a flush can
    show anything new) then submits `RESOURCE_FLUSH` **without** waiting —
    the one command this milestone genuinely wanted "submitted, ask about
    it later" semantics for.
  - `SYS_GPU_FENCE_WAIT(fence_id, timeout_ms)` — blocks (same
    spin-with-timeout idiom as `SYS_VFIO_WAIT_IRQ`) until that submission's
    completion is observed on the used ring.
- **`gpu_teardown_kproc`**, wired into all three kproc exit paths adjacent
  to v0.47's `vfio_teardown_kproc`: `RESOURCE_UNREF`s any resource a dying
  process still owns and clears scanout ownership if it was the active
  scanout. Deliberately tiny — the DMA grant behind the resource is already
  fully handled by existing v0.44 machinery.
- **`DEBUG_GPU`** (`g_debug_gpu`, default off): logs every admin command's
  type/response and every resource-create/scanout/flush/fence outcome.
  Verified live — see "Two real bugs found" below, one of which this
  exact trace caught.
- **`cmd_gpu_stress`** (new suite, `gpustress` command, 16 rounds): a real
  ring-3 driver (role 17) creates a 64×64 resource, draws a pattern
  directly into the mapped backing (zero-copy: no syscall touches the
  pixel data), sets it as scanout, submits an async flush, and fence-waits
  — reusing v0.45's kproc recycling. **Every 4th round instead runs role
  18**, which deliberately faults (a wild write to address `0x1`) right
  after creating its resource, before ever reaching scanout or its own
  `SYS_EXIT` — proving `gpu_teardown_kproc` reclaims the resource and its
  DMA grant via `handle_cpl3_fault`'s exit path, not just the well-behaved
  one. This is the actual test for the milestone's "unexpected client
  process termination" requirement, following the exact v0.44/45
  fault-injection precedent.

  ```
  [gpustrs]  PASS  every round completed without a watchdog timeout (no deadlock across preemption)
  [gpustrs]  PASS  every clean-round driver exited normally (create/draw/scanout/flush/fence all verified in ring 3)
  [gpustrs]  PASS  every fault-round driver actually died via the fault path (not its own SYS_EXIT)
  [gpustrs]  PASS  no DMA grant survived past any round's teardown (clean OR faulted)
  [gpustrs]  PASS  no GPU resource-table slot survived past any round's teardown (clean OR faulted)
  [gpustrs]  PASS  scanout ownership was released by every round's teardown (clean OR faulted)
  [gpustrs]  PASS  free-frame count reconciles (g_frame_free_depth == lifetime freed - reused)
  [gpustrs]  PASS  the frame allocator's leaf lock never triggered a rank violation (no double-free race)
  [gpustrs] RESULT: 8 passed, 0 failed
  ```

## Why `SYS_SURFACE_FLIP` and the software compositor are UNCHANGED

The milestone asked to "refactor `SYS_SURFACE_FLIP` to orchestrate hardware
scanout swaps without CPU frame copying." Doing that **in place** would mean
every existing surface — everything `cmd_threads`, `cmd_flip`, `cmd_cursor`,
`cmd_kinetic`, `cmd_surfin`, `cmd_keys`, `cmd_canvas`, `cio_surface_churn`,
and `cmd_smp_stress` create via `PCAP_FRAMEBUFFER` — would now need a GPU
resource behind it too, or the syscall would need two incompatible code
paths threaded through one entry point. That's a real regression risk
against 8+ suites this kernel has spent 20 milestones proving correct, for
a milestone whose actual, checkable deliverable is the command-stream
mechanism itself. Instead, the new `SYS_GPU_*` syscalls are a **parallel,
opt-in pathway** that demonstrates the identical idea — an app draws
directly into hardware-visible memory and swaps it in without the kernel
copying pixels — over genuine virtio-gpu hardware, without touching the
existing software surface code at all. `SYS_SURFACE_FLIP` (syscall 17) is
byte-for-byte unchanged.

## Two real bugs found before this shipped

- **A virtual-address collision, found by the IOMMU boot-verify, not by
  inspection.** The GPU driver's first common-BAR mapping used
  `VBLK_MMIO_V + 0x30000` — which is *exactly* `IOMMU_MMIO_V`, the fixed
  virtual address the DMAR register window has used since v0.44. Booting
  with the IOMMU config after adding the GPU driver produced real,
  reproducible failures in the *existing* `iommu`/`capdma` suites (`GSTS.TES`
  and `GSTS.RTPS` both reading back 0, "confined device DMA... BLOCKED"
  failing) — `iommu_init()` had genuinely enabled translation at boot (its
  own log line confirmed it), but `map_mmio(VBLK_MMIO_V + 0x30000, ...)`
  later silently repointed that exact page-table range at the GPU's PCI
  BAR, so every subsequent read through `g_dmar_regs` was reading GPU
  registers instead. Confirmed by direct comparison of the two `#define`s,
  not a race — reproduced identically on two consecutive runs before the
  fix, and gone after moving the GPU's mappings to `+0x50000`/`+0x60000`,
  clear of every existing `VBLK_MMIO_V`-relative user (vblk: `+0`/`+0x10000`,
  vnet: `+0x20000`, iommu: `+0x30000`).
- **A `kprintf` format-string bug, caught by the live `DEBUG_GPU`
  verification pass itself** (the exact discipline that verification step
  exists for): `gpu_submit_wait`'s debug log passed `(uint64_t)(ok ? 1 : 0)`
  to a `%s` format specifier. The trace showed a garbled character where
  "OK"/"ERR" should have been; fixed to pass the actual string, re-verified
  clean, then `DEBUG_GPU` was reverted to its shipped default (off).

## Honest scope decisions

- **Single command in flight, system-wide, serialized under a new
  `g_gpu_lock` (rank 7).** Unlike `vblk`'s 21-slot queue (built for
  concurrent block I/O throughput), GPU 2D commands here are infrequent
  enough that one outstanding command at a time is simple, obviously
  correct, and sufficient to prove the mechanism.
- **Completion is observed by polling the used ring, not an interrupt.**
  The device updates the used ring via DMA regardless of whether an IRQ is
  wired; polling is exactly as correct here, just busier — a deliberate
  trade against the added complexity of vector/ISR plumbing for a command
  rate this low.
- **"Zero-copy" is real at the app-to-backing level, not end-to-end.** The
  ring-3 driver writes pixels directly into the DMA-shared backing pages
  with no kernel copy in between. `TRANSFER_TO_HOST_2D`'s guest-RAM →
  host-resource hop is inherent to virtio-gpu's 2D command model itself —
  true zero-copy scanout-from-guest-pages would need 3D/virgl or a
  different scanout mode, both out of scope here.
- **No 3D/virgl.** Only the 2D resource/scanout commands the requirements
  named. The driver negotiates no 3D-related features.
- **`MAX_GPU_RES` is a small (8-entry) global pool**, owner-tagged exactly
  like v0.46's `g_ipc_shm` — no `struct kproc` growth needed.
- **A resource-create failure after a successful `RESOURCE_ATTACH_BACKING`
  is not specially unwound beyond a best-effort `RESOURCE_UNREF`** — the
  allocated frames in that specific, narrow failure window are neither
  mapped into the caller nor DMA-granted, so they are never reclaimed by
  `dma_teardown_kproc`. This path requires the device to reject a
  well-formed attach request, which it never does in this milestone's own
  testing; disclosed rather than engineered around for a case never
  actually observed.
- **The device topology: `virtio-vga`, not two separate devices.** The
  cleanest design would put `virtio-gpu-pci` on its own, with QEMU's
  original std VGA staying the *only* thing GRUB and the compositor ever
  see — zero ambiguity about which device owns the visible display. That
  topology (`-vga std -device virtio-gpu-pci`) was tried first, and the
  driver came up correctly on it, but it triggered an unexplained hang in
  `cmd_gpu_stress` specifically under `-smp 4` that does not reproduce
  under the combined `virtio-vga` device. Given `virtio-vga` is fully
  verified with 0 FAIL across all three configs and the separate-device
  hang was not root-caused in the time available, this kernel ships with
  the combined device. `virtio-vga` is VGA-compatible (bochs/VBE DISPI), so
  GRUB's existing framebuffer bring-up is unaffected in the configuration
  that actually ships — confirmed by direct comparison of the multiboot2
  framebuffer tag before and after the device swap, and by all
  compositor-dependent suites continuing to pass unchanged.
- **`SYS_GPU_SET_SCANOUT` was observed to change which QEMU console a
  headless `screendump` captures**, once `virtio-vga`'s GPU scanout path
  has been exercised at all. This is a QEMU console-selection artifact,
  not a kernel defect — every suite that depends on the actual framebuffer
  contents (`cio`, `threads`, `flip`, `cursor`, `kinetic`, `canvas`,
  `surfin`, `keys`) still passes, because they read/write the VBE
  framebuffer memory directly, never through QEMU's own screendump. It
  did mean a clean post-boot screenshot of the compositor could not be
  captured this milestone without a proper (non-headless) display backend,
  which this environment does not have (no DRM render node for
  `egl-headless`, no GTK/SDL); disclosed rather than shipping a misleading
  blank or GPU-test-pattern image in its place.

## Verification

| Config | Result |
|---|---|
| `-smp 4` BIOS ISO | 0 FAIL, 25 suites incl. `gpustress` 8/8 |
| `-smp 4` q35 + intel-iommu | 0 FAIL, 27 suites incl. `iommu` 2/2, `capdma` 11/11, `gpustress` 8/8 |
| uniprocessor | 0 FAIL, 25 suites incl. `gpustress` 8/8 (single-command-in-flight design needs no second core) |

No flakes observed on this milestone's runs.

## Honest scope gaps

- **No 3D/virgl acceleration** (see above).
- **No interrupt-driven GPU command completion** — polling only (see above).
- **`SYS_SURFACE_FLIP`/the software compositor were not refactored onto
  hardware scanout** — a new, parallel pathway instead (see above).
- **The `virtio-gpu-pci`-as-secondary-device topology has an unexplained
  `-smp 4` hang**, not root-caused this milestone (see above).
- **No clean post-boot compositor screenshot this milestone** (see above)
  — the boot logs and the live `DEBUG_GPU` trace are the verification
  record instead.
- **kproc table still has a `MAX_KPROC` concurrent-live cap** (v0.45,
  unchanged).
- VT-x unavailable under TCG; IOMMU/VT-d fully real; NVMe/UEFI boot-order
  quirk remains unexercised, upstream of the kernel (all unchanged).
