# Outrun OS v0.34.0-metal — Tick-based window springs + per-slot flip consumption

Closes the two oldest mechanical gaps in the rendering/physics stack, both
carried in changelogs since v0.32.

## Claim 1: the whole spatial canvas now runs on one physics clock

`physics_step` (window springs + pairwise collision separation) joins the
camera on the PIT-tick clock introduced in v0.32:

- Per-tick constants derived from the tuned per-frame pair (k = 1/6,
  damp = 0.82, frame = 2 ticks): `SPRING_DAMP_T = 59345` (the 16.16 root of
  0.82; its FXMUL square is within 1/65536), and the spring impulse rescaled
  so two ticks impart the per-frame velocity gain:
  `SPRING_K_T = k·√d/(1+√d) = 5190`. The step response was simulated against
  the old per-frame system before committing: same overshoot character, same
  settle point (old peak 131.2 vs new 133.8 on a 100-unit step, both settle
  to 100 ± 1.5).
- Velocities stay in world-units-per-frame; position integrates v/2 per tick,
  identical to the camera convention.
- Collision separation runs per tick with the unchanged, state-proportional
  rule — it converges the same way, in half the wall time (the push is
  proportional to remaining overlap, so this is a settle-rate change, not a
  behavior change).
- `physics_step_dt(wins, n, ticks)` clamps 1..32 like the camera;
  `canvas_pass` feeds both integrators the same measured `g_ticks` delta.
  `physics_step()` = 2 ticks keeps `cmd_gfx`'s legacy loop meaningful.

### Proof: kinetic suite grows 10 → 12

Both checks compare the FULL window state — x, y, vx, vy of all five windows,
through the spring AND the collision resolver — with integer `==`:

- "window-spring dt grouping is EXACT (60 frames @ dt=2 == 120 @ dt=1)"
- "window springs under irregular frame times land on the identical state"
  (the same 120 ticks chopped as {1,4,2,5,3,5}×6)

## Claim 2: flip consumption is tracked per slot — no producer can be parked by a pass that doesn't display it

The v0.32 global `g_canvas_live` is gone. Each surface carries a `consumer`
bit:

- `canvas_pass` registers as consumer for the slots it composites (0..NWIN-1,
  set blanket at pass start so even a surface bound mid-pass is covered) and
  deregisters at pass end. `canvas_frame` consumes flips only for those slots.
- The `flip` suite registers for slot 5 only — exactly the situation that
  used to park the slot-4 app for the whole suite.
- `SYS_SURFACE_FLIP` blocks only while `flip_pending && consumer`; a flip on
  a slot nobody consumes completes immediately (and still yields — a flip
  remains a frame boundary, hence a scheduling point). Reclaim clears the bit.

### Proof: flip suite grows 6 → 8

- "flips on a slot with NO consumer complete immediately (producer never
  parks)" — before any consumer registration, both alternating colors are
  observed in the tear-test app's front buffer within a few yields: it is
  publishing frames, not waiting.
- "sibling surface thread kept running while another slot was consumed" —
  the slot-4 app's liveness bar is sampled immediately before and immediately
  after the 200-frame slot-5 consume loop (no intervening yields on the
  sampling side), so the observed movement happened DURING the consumption
  window. Under v0.33 semantics this check fails: the app sat parked in its
  flip for the entire loop.

All original flip guarantees re-verified: 0 torn frames in 200, strict
alternation, pair reclaim.

## Verification (all boot-tested on this exact image)

| Suite | Result |
|---|---|
| validate | 19/19 |
| invariants | 21/21 |
| stress | 10/10 |
| fuzz | 8/8 |
| threads | 7/7 |
| flip | **8/8** (was 6) |
| cursor | 6/6 |
| kinetic | **12/12** (was 10) |
| keys | 5/5 |
| iommu | 2/2 (q35 + intel-iommu config) |
| capdma | 11/11 (q35 + intel-iommu config) |

Boot configurations: BIOS ISO (i440fx), q35 + intel-iommu
(`iommu_platform=on` on both virtio devices), NVMe/UEFI install image.
Prior acceptance criteria all hold: clicks routed mid-pass (v0.31, 830
marker px on screen), zero torn frames (v0.32), "HI R3" pixel-decode (v0.33
— the five glyph blocks are still on the final screen).

## Honest scope gaps

- **Spring tuning is matched, not bit-identical, to v0.33**: the per-tick
  spring is a different discretization of the same ODE (verified same
  overshoot/settle by simulation, and the gfx demo still settles visually).
  Collision separation settles in half the wall time. These are deliberate,
  documented retunes — the exactness guarantee is about dt-grouping, not
  about reproducing the old per-frame trajectory.
- Fling velocity *sampling* in `canvas_mouse` remains per-compositor-frame —
  the one remaining frame-rate-dependent input to the physics. (Its decay is
  now fully tick-based.)
- The consumer bit is a single-consumer protocol by design; two passes
  consuming the same slot concurrently would race on `front`. Nothing in the
  system does this, and the uniprocessor + cooperative structure serializes
  the boot flow.
- HUD/`ccmd_exec` "tile windows" retargets springs mid-flight; targets are
  consumed per tick like everything else, but a retarget between ticks of one
  `physics_step_dt` call is impossible by construction (targets only change
  from `canvas_input`, outside the integrator) — worth stating because it is
  what makes the grouping-exactness claim sound.
- Prior gaps unchanged: type-to-app only inside canvas passes; ASCII-level
  key events (no make/break, no Ctrl/Alt); 16-deep queue drops on overflow;
  page tables leak on exit; uniprocessor invariants; VT-x unavailable under
  TCG.
