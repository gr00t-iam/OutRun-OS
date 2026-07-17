# Outrun OS v0.32.0-metal — Double-buffered surfaces + a tick-based physics clock

## Claim 1: the compositor can no longer observe a half-drawn frame

`SYS_SURFACE_FLIP` (17) gives every ring-3 surface a front/back buffer pair
with vsync semantics:

- `SYS_SURFACE_CREATE` now allocates ONE contiguous `2*bufpages` chunk: buf0
  at `SURF_USER_V`, buf1 immediately after, both user-mapped. Apps that never
  flip keep the v0.31 single-buffer behavior bit-for-bit (`front` stays 0).
- The compositor reads only `buf[front]`; `front` toggles **only at a frame
  boundary** (`canvas_frame` consumes pending flips before compositing), so a
  blit can never straddle a flip.
- `SYS_SURFACE_FLIP` marks the flip pending and **blocks (yields) until the
  compositor consumes it** — the app can never draw into a buffer being
  blitted, and it runs naturally vsynced: one app frame per compositor frame.
  When no canvas pass is live (`g_canvas_live=0`) the flip consumes
  immediately but still yields — a flip is a frame boundary, hence always a
  scheduling point. (The first cut omitted that yield and the app monopolized
  the uniprocessor the moment it stopped calling `SYS_YIELD`; the boot hung at
  `cmd_surface`. Caught by boot-testing, fixed by making the immediate path
  yield.)
- Thread death reclaims the pair as one chunk; the free-list recycle from
  v0.31 carries over unchanged.

### Proof: the `flip` suite (new, 6 checks)

A ring-3 "tear-test" thread (role 5) fills whole 64x64 frames in strictly
alternating app-unique colors (0x4682EA / 0x1FBF6E) and publishes 400 of them
through `SYS_SURFACE_FLIP`. The kernel plays compositor with **timer
preemption enabled** — the app is interrupted mid-draw arbitrarily — consuming
200 flips and scanning every published front buffer:

1. ring-3 thread created a double-buffered surface (front+back pair)
2. both buffers are user-mapped in the owner (and only 2*bufpages of them)
3. 200 frames published through SYS_SURFACE_FLIP under preemption
4. **ZERO torn frames: every published front buffer is a COMPLETE frame**
5. frames alternate strictly (each flip is exactly one finished frame)
6. double buffer reclaimed as one chunk when the owner exits

A single non-uniform scan would be a torn frame; under v0.31's in-place
repaint this detector would trip.

## Claim 2: frame rate cannot change the camera trajectory

The physics clock is now the PIT tick (10 ms), not the compositor frame:

- `camera_step()`'s body became `camera_tick()`; every per-frame decay
  constant was replaced by its 16.16 square root applied once per tick.
  Chosen so the classic 2-tick frame reproduces the v0.31 tuning:
  `CAM_FRICTION_T = 61478` (FXMUL(t,t) == 0.88 exactly), `ZOOM_EASE_T = 8780`
  (retention root exact), `CAM_EASE_T = 7656` (retention within 1/65536).
  Position integrates v/2 per tick; snap thresholds, zoom clamp, and the
  anchor re-pin all moved inside the tick.
- `camera_step_dt(n)` advances n ticks (clamped 1..32 — a stall is not a
  teleport); `canvas_pass` feeds it the **measured** `g_ticks` delta per
  frame. `camera_step()` = `camera_step_dt(2)` keeps every existing suite
  call site meaningful.
- Because ALL state mutation is per tick, dt grouping is exact:
  `camera_step_dt(a); camera_step_dt(b)` ≡ `camera_step_dt(a+b)` to the bit.

### Proof: kinetic suite grows 8 → 10 checks

- "dt grouping is EXACT: 100 frames @ dt=2 == 200 frames @ dt=1
  (bit-identical)" — flick + eased anchored zoom, full state compared with
  `==` on the fixed-point integers.
- "irregular frame times (1..5 ticks) land on the identical camera state" —
  the same 200 ticks chopped as {1,3,2,4,1,5,2,2}×10.

All original kinetic invariants (anchor pinning, convergence, monotonicity,
clamps) re-verified on the per-tick engine.

## Other changes

- The surface app (role 2) renders into the back buffer and publishes with
  flip; its click-hit markers and liveness bar ride through the same path the
  `surfin` acceptance scans (now against `surf_front_phys()`, the published
  frame).
- `SYS_SURFACE_FLIP` added to the fuzz remap (20000-call run) — owner-gated,
  denied before any state change, so hostile flips cannot park a thread or
  toggle another process's front buffer.
- `cmd_surfin`'s pixel proof and the boot sequence gain the `flip` suite after
  `threads`; shell command `flip`.

## Verification (all boot-tested on this exact image)

| Suite | Result |
|---|---|
| validate | 19/19 |
| invariants | 21/21 |
| stress | 10/10 |
| fuzz | 8/8 (20000 calls, now incl. SYS_SURFACE_FLIP) |
| threads | 7/7 |
| **flip (new)** | **6/6** |
| cursor | 6/6 |
| kinetic | **10/10** (was 8; two exact dt-independence checks added) |
| iommu | 2/2 (q35 + intel-iommu config) |
| capdma | 11/11 (q35 + intel-iommu config) |

Boot configurations: BIOS ISO (i440fx), q35 + intel-iommu
(`iommu_platform=on` on both virtio devices), NVMe/UEFI install image.

## Honest scope gaps

- **Window-spring physics (`physics_step`) is still per-frame** — the
  milestone scoped frame-rate independence to camera momentum/zoom/glide.
  Springs settle in frame counts, not wall time.
- Fling velocity sampling in `canvas_mouse` still measures per-compositor-
  frame drag deltas; under irregular frame times the imparted momentum varies
  slightly with frame rate even though its decay no longer does.
- The flip protocol is single-consumer by design: only `canvas_frame`
  (or a suite standing in for it) consumes. Two concurrent compositor passes
  would race on `front`; nothing in the boot flow does this.
- A flipping app is parked while a canvas pass runs that never composites its
  slot (it waits for a consumer). It self-consumes the moment the pass ends —
  observed, not a deadlock — but a per-slot "composited this pass" bit would
  be the precise fix.
- Surfaces remain CPU-blitted with a per-pixel divide; the checker apps are
  small enough that this is not the frame-time floor, the compositor is.
- The exhaustive 192-mask capability sweep still enumerates the v0.22-era
  gated syscalls; flip's deny paths are covered by the fuzz run, not the
  sweep.
- Process page tables and user stacks still leak on exit (bump allocator);
  only surface pixel buffers are recycled.
- Uniprocessor invariants unchanged; VT-x still unavailable under TCG;
  IOMMU/VT-d remains real.
