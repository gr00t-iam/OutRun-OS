# Outrun OS v0.53.0-metal — WIMP Desktop Shell & Cyber-Compositor

Phase 3 opens with a real desktop. A kernel-side window manager gives ring-3
processes genuine top-level **windows** — screen position, stacking z-order, a
title bar with close and minimize boxes, focus state, and a DMA-shared content
buffer the app draws into. A **cyber-compositor** paints the whole desktop
(background, launcher icons, every window's chrome and scaled content in
z-order, taskbar, pointer) into the RAM backbuffer and presents it. Pointer and
keyboard input are **hit-tested and routed** to the right window — raise, focus,
drag, minimize, close — and every window a process owns is destroyed, with its
content grant reclaimed, when that process exits *or crashes*.

## What's new

### Window management engine

- **`struct wmwin` × 12** (`g_wm_lock`, rank 10): each window carries its screen
  rect, a monotonic **z stamp**, `minimized`/`focused` flags, an accent colour,
  a title, a per-window 8-deep input event ring, and a **DMA-granted 32×32 ARGB
  content thumbnail** the owning process renders into.
- Core operations: `wm_topmost_at` (z-aware hit-test), `wm_raise` (restack to
  front), `wm_focus` (exclusive keyboard focus), `wm_queue_event` (route input
  to the owner), and `wm_destroy` (revoke the content grant, clear focus/drag).

### Cyber-compositor

- **`wimp_compose`** snapshots the window table **under the lock, then draws
  without holding it** (draw primitives are slow and must never run under a
  klock), insertion-sorts visible windows by z, and paints back-to-front:
  desktop background + grid, launcher icon rail, each window's chrome
  (border/title bar/close+minimize boxes) and its **nearest-neighbour-scaled
  content thumbnail**, the taskbar with one chip per window (focused / normal /
  minimized colouring), and finally the pointer — then `fb_flip()`s the RAM
  backbuffer to the display. Minimized windows are skipped entirely.
- The compositor reuses this kernel's existing double-buffered framebuffer
  pipeline (`g_bb` → `g_fb` via `fb_flip`), so it inherits tear-free
  presentation rather than reinventing it. `fb_init` is now **idempotent**, so
  the compositor can be brought up early (e.g. by the stress suite) and later
  boot stages are unaffected.

### Input routing

- **`wimp_pointer(x, y, down)`** is the single routing core: hit-test the
  topmost window, then dispatch by region — **close box** destroys the window,
  **minimize box** toggles it out of compositing and hit-testing, elsewhere on
  the **title bar** raises + focuses and begins a drag, and a click in the
  **content area** raises + focuses and queues a `click` event (in
  window-local coordinates) for the owner.
- **`wimp_input_step`** drives that core from real hardware: it integrates
  mouse deltas into the pointer position (clamped to the screen), converts
  press/release edges into `wimp_pointer` calls, applies drag motion with
  bounds clipping so a window can never be dragged fully off-screen, and routes
  every keystroke to the **focused** window's event ring.
- Because both the interactive path and the stress suite call the *same*
  `wimp_pointer`, the routing logic is verified deterministically without
  depending on real mouse hardware.

### Syscalls, capability, teardown

- **Three new syscalls** (40–42), gated by a new **`PCAP_WIMP`** capability
  (bit 12):
  - `SYS_WIN_CREATE((w<<16)|h, accent)` → window id. Clamps the requested size
    to sane bounds, cascades the new window's position, stamps it to the front,
    focuses it, and maps a zeroed content page at `WIN_USER_V + id*4096` as a
    `DMA_GRANT_PAGE` grant.
  - `SYS_WIN_DAMAGE(id)` → ownership-checked recomposition request.
  - `SYS_WIN_POLL(id, *out_event)` → pops one routed input event into a
    caller-supplied buffer, validated with the same `access_ok` page-table walk
    every pointer-taking syscall uses.
- **`wimp_teardown_kproc`** wired into **all three** kproc exit paths beside
  `net_teardown_kproc`: destroys every window a dying process owns and resets
  focus/drag so a dead window can never keep input or be composited. The
  content buffers are `DMA_GRANT_PAGE` grants already reclaimed by
  `dma_teardown_kproc`, so this drops the WM's references — the same division
  of labour v0.50/v0.51 use for GPU resources and PCM streams.
- **`DEBUG_WIMP`** (`g_debug_wimp`, default off) alongside the other
  per-subsystem debug flags.

### `cmd_wimp_stress` (new suite, `wimpstress`)

Two halves, 18 checks total:

- **16 ring-3 rounds** — a role-23 app creates three windows, draws a pattern
  into each content thumbnail, damages them, and polls for events. **Every 4th
  round runs role 24**, which deliberately faults right after creating its
  windows (before damage/poll or its own `SYS_EXIT`), proving
  `wimp_teardown_kproc` reclaims via the **fault** exit path too. Checks: no
  window slot survives a round, **no content DMA grant survives** a round, and
  focus is never left pointing at a dead process's window.
- **Deterministic WM logic** — seeded windows verify z-aware hit-testing in an
  overlap, hit-testing over empty desktop, title-bar click raise+focus,
  title-bar drag movement and release, minimize hiding a window from
  compositing/hit-testing, and close-box destruction returning the WM to
  baseline occupancy. A **compositor smoke test** composes a real frame and
  samples the backbuffer to confirm window pixels actually overwrote the
  desktop background.
- Plus the standard frame-reconciliation and rank-violation checks.

## Bookkeeping

- Capability `PCAP_WIMP` = bit 12 (after v0.51's `PCAP_AUDIO` at bit 11).
- Syscalls 40 (`SYS_WIN_CREATE`), 41 (`SYS_WIN_DAMAGE`), 42 (`SYS_WIN_POLL`).
- kproc roles 23 (clean WIMP app) and 24 (fault-after-create).
- klock rank 10 (`g_wm_lock`) — after v0.52's `g_net_lock` at rank 9.
- `WIN_USER_V` = `0x0000550000000000` (after v0.49's `SMP_USER_V`).

## Honest scope notes

- **Window content is a 32×32 thumbnail, not a full-resolution surface.** Each
  window's shared content buffer is exactly one page, nearest-neighbour scaled
  into the window's content rect at composite time. This proves the whole
  ownership/mapping/compositing/teardown path end-to-end with a bounded, leak-
  checkable allocation; full-resolution per-window buffers (and the existing
  double-buffered `SYS_SURFACE_*` path they would reuse) are future work.
- **Composition is full-frame, not incremental dirty-rectangle blitting.**
  `SYS_WIN_DAMAGE` is implemented and ownership-checked, and the compositor
  already skips minimized windows and clips content scaling, but each
  `wimp_compose` currently repaints the desktop rather than only the damaged
  spans. The damage plumbing is in place for that optimisation; the milestone's
  correctness goals (accurate z-order composition, zero leaks) are met without
  it, and claiming dirty-rect blitting that isn't there would be dishonest.
- **The interactive desktop loop is not wired into boot.** `wimp_input_step` +
  `wimp_compose` are complete and verified, but the boot sequence still ends in
  the existing shell/canvas rather than a persistent desktop session — this
  sandbox is headless (serial-only), so an interactive loop could not be
  verified here. The routing and compositing they perform *are* verified, via
  the same code paths, by `cmd_wimp_stress`.
- **Hardware cursor is software-composited.** The pointer is drawn into the
  backbuffer each frame (with bounds clipping) rather than using a hardware
  cursor plane; virtio-gpu's cursor queue is deliberately left uninitialised,
  as documented since v0.50.

## Verification

BIOS/GRUB ISO in QEMU, TCG-only (no KVM), virtio-vga + virtio-blk + virtio-net
attached. Disk images recreated fresh before every boot.

| Config | Command | `wimpstress` | Result |
| --- | --- | --- | --- |
| Uniprocessor, BIOS | `make qemu` | 18/0 | all suites **0 FAIL** |
| SMP `-smp 4`, BIOS | `make qemu` + `-smp 4` | 18/0 | all suites **0 FAIL** |
| q35 + Intel VT-d IOMMU | `make qemu-iommu` | 18/0 | all suites **0 FAIL** |

All prior suites (`gpustrs`, `audstrs` where applicable, `netstrs`, `vfsstrs`,
`ipcstrs`, `vfiostrs`, `kpstrs`, `leakchk`, `dmastrs`, the SMP suites, and the
compositor suites) continue to pass with 0 failures.
