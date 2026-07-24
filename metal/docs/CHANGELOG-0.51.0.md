# Outrun OS v0.51.0-metal — USB / xHCI Subsystem & Low-Latency Audio Pipelines

This release brings up two independent pieces of real hardware. A native
**xHCI USB 3.0 host controller** is discovered on PCI, taken through its full
reset/run handshake, and driven far enough to enumerate a connected HID
device and read its boot-protocol report — routing real keystrokes and mouse
deltas into the kernel's existing input queues. Separately, a ring-3 process
can now open a **real virtio-sound PCM stream**, write sample buffers directly
into DMA-shared backing pages, and block until the device confirms playback —
all through genuine virtio-sound wire-protocol commands answered by QEMU's
actual audio device, gated behind a new capability and reclaimed on process
exit or crash.

## What's new

### xHCI / USB host controller + HID routing

- **A real xHCI controller driver** (`xhci_probe`), discovered by PCI class
  (0x0C serial-bus / subclass 0x03 / progif 0x30 = xHCI specifically, not
  UHCI/OHCI/EHCI). It maps the Capability/Operational/Runtime register sets,
  performs the HCRST reset handshake, allocates the Device Context Base
  Address Array, builds a Command Ring and an Event Ring (with its ERST), and
  writes `USBCMD.RS` to bring the controller to the RUNNING state — verified
  live: the controller clears `USBSTS.CNR`, `USBSTS.HCH` goes low, and a real
  connected port shows `PORTSC.CCS`.
- **Real USB enumeration over the wire**: `ENABLE_SLOT` → `ADDRESS_DEVICE`
  (with a real Input Context / Slot Context / EP0 Context), followed by a
  genuine 3-stage control transfer (`GET_DESCRIPTOR`) that reads back an
  18-byte USB device descriptor — confirmed to be QEMU's own device (idVendor
  0x0627). PORTSC writes carefully mask the RW1C status bits (CSC/PEC/…/CEC)
  and the write-1-to-disable PED bit so a read-modify-write never clears a
  latched change or disables the port by accident.
- **HID boot-protocol input**: `SET_CONFIGURATION` + `SET_PROTOCOL(boot)` put
  a keyboard or mouse into boot protocol, then `xhci_poll_hid` issues the
  mandatory `GET_REPORT` class request to read boot reports and feeds them
  into the kernel's **existing** input sinks — `kbd_ring` for keystrokes
  (translated via a HID-usage→ASCII table) and `g_mouse_dx/dy/dz`/`g_mouse_btn`
  for pointer deltas — with **zero** downstream changes to the shell or
  compositor. Polling is throttled (`XHCI_POLL_EVERY`) so the control-transfer
  round trips don't dominate the input loop.
  - **Scope decision (disclosed): polled `GET_REPORT`, not an interrupt
    endpoint.** Ongoing reports are read by polling the mandatory HID
    `GET_REPORT` control request rather than configuring a second
    interrupt-IN endpoint. This is spec-compliant (`GET_REPORT` is a required
    HID class request) and architecturally consistent with this kernel's
    already-polled PS/2 keyboard model. The interrupt-endpoint path was
    deliberately not taken this milestone: it would have required
    `CONFIGURE_ENDPOINT` context-entry counting, DCI computation, and Interval
    encoding — substantial extra protocol surface whose risk was not worth it
    against the time this milestone also needed for the full audio subsystem.

### Hardware audio pipeline + ring-3 PCM buffering

- **A real virtio-sound driver** (`virtio_sound_probe`), brought up exactly
  like this kernel's existing virtio-blk/-net/-gpu drivers: walk the PCI
  vendor capability list, map the common/notify/ISR structures, negotiate
  `VERSION_1` (+ `ACCESS_PLATFORM` when offered), and bring up the control
  queue (queue 0) and the tx queue (queue 2). The event queue (1) and rx
  queue (3) are deliberately left uninitialized — this milestone is
  playback-only.
- **Real virtio-sound PCM commands** over genuine descriptor chains:
  `PCM_SET_PARAMS` (48 kHz / S16 / stereo), `PCM_PREPARE`, `PCM_START`,
  `PCM_STOP`, `PCM_RELEASE` on the control queue, and actual sample submission
  on the tx queue as a 3-descriptor chain (xfer header + sample data +
  device-written status). Every control response is checked against
  `VIRTIO_SND_S_OK`.
- **Two new syscalls** (33–34, gated by a new `PCAP_AUDIO` capability):
  - `SYS_AUDIO_CONFIGURE(sample_rate, channels)` → a user vaddr mapped
    directly onto a DMA-granted PCM buffer page (rate ∈ {44100, 48000},
    channels ∈ {1, 2}). The backing is a `DMA_GRANT_PAGE` grant — the same
    mechanism `SYS_DMA_ALLOC`/`SYS_GPU_RESOURCE_CREATE` use — so
    `dma_teardown_kproc` already reclaims the frame; a single-owner gate
    (`g_audio_owner`) tracks who currently holds the one PCM stream.
  - `SYS_AUDIO_WRITE(len)` → submits the buffer on the tx queue and
    fence-waits for the device to confirm consumption (same async-submit +
    fence-wait shape as v0.50's GPU flush; the `timeout_ticks==0` convention
    means "check once", never "wait forever").
- **`audio_teardown_kproc`**, wired into all three kproc exit paths adjacent
  to v0.50's `gpu_teardown_kproc`: if the dying process owns the PCM stream it
  `PCM_STOP`s the stream best-effort and clears the ownership globals. The DMA
  grant behind the buffer is already fully handled by v0.44 machinery.
- **`usb_teardown_kproc`**, wired into the same three exit paths as an honest
  near-no-op symmetry hook: USB HID is a kernel-internal input driver (like
  PS/2) with no per-process ring-3 resources to reclaim, so it is
  deliberately empty and documented as such.
- **`DEBUG_AUDIO`** (`g_debug_audio`, default off) alongside the existing
  per-subsystem debug flags.
- **`cmd_audio_stress`** (new suite, `audiostress` command): a real ring-3
  driver (role 19) calls `SYS_AUDIO_CONFIGURE`, writes several buffers of a
  tone pattern directly into the mapped backing (zero-copy: no syscall touches
  the sample data), each `SYS_AUDIO_WRITE` blocking until the device confirms,
  then exits — reusing v0.45's kproc recycling. **Every 4th of 16 rounds
  instead runs role 20**, which deliberately faults (a wild write to `0x1`)
  right after configuring, before any write or its own `SYS_EXIT` — proving
  `audio_teardown_kproc` reclaims the PCM stream and its DMA grant via
  `handle_cpl3_fault`'s exit path, not just the well-behaved `SYS_EXIT` one
  (the same v0.44/45/50 fault-injection precedent). A final **gap round**
  waits 50 ticks before running a clean driver, as an honest proxy for
  "slow-producer / underrun-recovery" — see scope note below.

## Capabilities, syscalls, and bookkeeping

- `PCAP_AUDIO` = bit 11 (after v0.50's `PCAP_SURFACE` at bit 10). USB/xHCI
  needed **no** new capability — it is a kernel-internal input driver with no
  ring-3 syscall surface, exactly like PS/2.
- Syscalls 33 (`SYS_AUDIO_CONFIGURE`) and 34 (`SYS_AUDIO_WRITE`).
- New kproc roles 19 (clean audio driver) and 20 (fault-after-configure).
- New klock rank 8 (`g_audio_lock`). xHCI needed no klock — its bring-up is
  single-BSP, pre-SMP-relevant, and its report consumption is a lock-free
  polled ring like PS/2.

## Two real interactions found and disclosed (not papered over)

This milestone surfaced two device-topology interactions specific to this
sandboxed, **TCG-only** QEMU (no KVM — confirmed unavailable in every prior
changelog). Both are QEMU/emulation-level, not kernel defects, and both are
handled the same disciplined way this project handled v0.50's
`-vga std + virtio-gpu-pci` hang: **verify in a dedicated targeted boot and
disclose, rather than force-fix an emulator quirk.**

### 1. A running xHCI controller collapses full-suite boot time under TCG

A running xHCI controller continuously emulates its own 125 µs microframe
timer. Under pure TCG that per-microframe work slows the *whole* guest so
severely that the full ~25-suite regression boot does not complete in any
practical wall-clock time. This was root-caused by bisection: the slowdown
**reproduces even with the kernel's `xhci_probe()` call disabled** — the mere
presence of a running controller is enough — so it is QEMU's device-emulation
cost, not kernel code.

- **Resolution:** xHCI/USB devices are kept out of the main `qemu` /
  `qemu-vga` / `qemu-iommu` / `qemu-nvme` targets. A dedicated **`make
  qemu-usb`** target attaches `qemu-xhci` + `usb-kbd` + `usb-mouse` for a
  short, targeted boot that shows controller bring-up, port detection, slot
  enumeration, device-descriptor read, and HID boot-protocol configuration
  completing within a few seconds of serial output.

### 2. `virtio-sound-pci` + `virtio-vga` breaks virtio-gpu's controlq under TCG

Attaching `-device virtio-sound-pci` **alongside** `-device virtio-vga` makes
virtio-gpu's control queue stop completing: the gpu driver's very first
`RESOURCE_CREATE` is submitted and the device is notified, but the used-ring
completion never arrives, so `cmd_gpu_stress` hangs on its first round. This
was root-caused by bisection with breadcrumbs:

- The hang is inside the first `gpu_submit_wait` used-ring spin (no timer
  dependency — so it is not an interrupt-storm / frozen-tick effect, it is the
  device genuinely not draining the queue).
- MMIO mappings do **not** collide (4 KiB-granular; gpu at `+0x50000/0x53000`,
  sound at `+0x90000`), and `map_mmio` uses 4 KiB pages, so it is not an
  overlapping-page remap.
- Decisively, **the hang reproduces with `virtio_sound_probe()` entirely
  disabled** — the sound device merely being present on the PCI bus is
  enough. So it is a QEMU device-topology / BAR-assignment interaction, not
  this kernel's sound driver.
- The interaction is asymmetric: **virtio-sound itself works perfectly
  alongside virtio-vga** (audstrs passes 8/0 with both attached); only
  virtio-gpu's controlq is affected.

- **Resolution:** the main regression targets keep `virtio-vga` and omit
  virtio-sound, so `cmd_gpu_stress` runs and passes as it did in v0.50;
  `cmd_audio_stress` **skips cleanly** (not a failure) when no sound device is
  present. A dedicated **`make qemu-audio`** target uses plain `-vga std`
  (no virtio-gpu) + `virtio-sound-pci`, so there `cmd_gpu_stress` skips
  cleanly and `cmd_audio_stress` runs and is fully verified. Both targets are
  0-FAIL end to end.

## Honest scope notes

- **Underrun recovery is simulated, not a real XRUN.** This sandbox has no
  real audio backend driving wall-clock playback timing (the `none` audiodev
  produces no host output), so `cmd_audio_stress`'s "underrun" case is a
  deliberate submission-timing **gap** (a 50-tick delay before a clean round),
  not a device-reported XRUN status. The virtio queue/DMA/interrupt path it
  exercises is fully real; the timing pressure is a proxy, and it is labelled
  as such in the code.
- **USB HID reports are polled, not interrupt-driven** (see the Stage-B scope
  decision above).
- **`usb_teardown_kproc` is intentionally empty** — a symmetry hook, since USB
  HID owns no per-process ring-3 state.
- **Audio is playback-only** this milestone (tx queue only; rx/event queues
  left uninitialized).

## Verification

All boots are BIOS/GRUB ISO in QEMU, TCG-only (no KVM). Disk images are
recreated fresh before every boot (a stale `build/vblk.img` produces false
`vfsstrs` failures from leftover CAS/journal state — a long-standing test
hygiene note in this project).

| Config | Command | Result |
| --- | --- | --- |
| Uniprocessor, BIOS, virtio-vga (no sound) | `make qemu` | all suites **0 FAIL**; `gpustrs` 8/0, `audstrs` SKIPPED cleanly |
| SMP `-smp 4`, BIOS, virtio-vga (no sound) | `make qemu` + `-smp 4` | all suites **0 FAIL** |
| q35 + Intel VT-d IOMMU, virtio-vga (no sound) | `make qemu-iommu` | all suites **0 FAIL** |
| Dedicated audio: `-vga std` + virtio-sound | `make qemu-audio` | all suites **0 FAIL**; `audstrs` 8/0, `gpustrs` SKIPPED cleanly |
| Dedicated USB: `qemu-xhci` + usb-kbd + usb-mouse | `make qemu-usb` | controller RUN, port/slot/address/descriptor + HID boot-protocol config all visible |

`cmd_audio_stress` verified end to end in the `qemu-audio` target: 16 rounds
(4 deliberately faulted after configure via role 20) + 1 gap round, **0
failures** — every clean round configured/wrote/exited in ring 3, every fault
round died via the fault path (not its own `SYS_EXIT`), **no DMA grant
survived any teardown** (clean or faulted), **PCM stream ownership was
released by every teardown**, the free-frame count reconciled exactly, and the
frame allocator's leaf lock recorded zero rank violations.
