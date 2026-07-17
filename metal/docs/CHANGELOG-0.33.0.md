# Outrun OS v0.33.0-metal — Keyboard routing to surfaces (type=2 goes live)

## The claim

The `sevent.type=2` slot, reserved since v0.29, is now a real input path: the
focused ring-3 application owns the keyboard. Keystrokes travel the identical
pipeline a physical key takes — PS/2 ring buffer → canvas input → the focused
surface's event queue → the app's `SYS_SURFACE_POLL` loop → its back buffer →
`SYS_SURFACE_FLIP` → the composited screen — and the proof decodes the typed
string back out of the published pixels.

Boot-verified evidence (this exact image):

```
  [app:r3] key event code 48 — echoing into our pixels        (H)
  [app:r3] key event code 49 — echoing into our pixels        (I)
  [app:r3] key event code 20 — echoing into our pixels        (space)
  [app:r3] key event code 52 — echoing into our pixels        (R)
  [app:r3] key event code 33 — echoing into our pixels        (3)
[keys   ] pixel-decoded from the published front buffer: "HI R3"
```

Screendump analysis of the final composited screen finds exactly the five
glyph blocks {H, I, space, R, 3} at 96 px each.

## Design

- **Modal, focus-based routing** (a real WM focus model, not a heuristic):
  `Enter` arms "type-to-app" when the focused window has a bound surface;
  from then on EVERY key belongs to the app — including WASD, Tab and `/`,
  which deliberately stop being canvas hotkeys — except `Esc`, which returns
  the keyboard to camera navigation. The context ribbon shows the mode
  (`KEYBOARD -> RING-3 APP  (ESC RETURNS TO CANVAS)`).
- Keys are focus-addressed while clicks stay geometry-addressed
  (`surface_route`'s inverse-projection). That asymmetry is intentional:
  camera motion during typing cannot misroute a keystroke.
- Delivery reuses the existing per-surface 16-deep queue and the existing
  syscall surface — **no new syscall**; the app dispatches on `sevent.type`.
  Event: `{type=2, x=-1, y=-1, code=ascii}` for backspace, Enter, and
  printable 32..126. Queue-full drops keys, same policy as clicks.
- The mode self-disarms if the focused surface disappears (owner exit).
- App side (role 2): typed characters render as 8x7 blocks whose **color
  encodes the ASCII code** (`0xA00030 | code<<8`, a family nothing else in
  the system draws) — which is what lets the kernel suite, and the screenshot
  analysis, decode the text verbatim from pixels.

## The `keys` suite (new, 5 checks — suite #11)

1. navigation-mode keys steer the camera, none leak to the app
2. Enter armed type-to-app and 5 keys were routed to the focused surface
3. Esc returned the keyboard to canvas navigation
4. app painted the typed text — pixels decode back to "HI R3"
5. post-Esc keys move the camera again and the app receives none

All keystrokes are injected with `canvas_inject_char` into the real PS/2 ring
inside live canvas passes — the same code path a physical scancode reaches
after translation.

## Verification (all boot-tested on this exact image)

| Suite | Result |
|---|---|
| validate | 19/19 |
| invariants | 21/21 |
| stress | 10/10 |
| fuzz | 8/8 |
| threads | 7/7 |
| flip | 6/6 |
| cursor | 6/6 |
| kinetic | 10/10 |
| **keys (new)** | **5/5** |
| iommu | 2/2 (q35 + intel-iommu config) |
| capdma | 11/11 (q35 + intel-iommu config) |

Boot configurations: BIOS ISO (i440fx), q35 + intel-iommu
(`iommu_platform=on` on both virtio devices), NVMe/UEFI install image.
The v0.31 surfin acceptance (clicks routed mid-pass, 447 marker pixels) and
the v0.32 flip acceptance (0 torn frames in 200) both still hold.

## Honest scope gaps

- **The shell and the canvas still share one keyboard**: type-to-app exists
  only inside a canvas pass. At the `outrun>` prompt keys go to the shell;
  there is no global focus arbiter.
- Key events carry ASCII from the existing scancode translation — no key-up
  events, no modifiers beyond what translation bakes in (shifted chars work,
  Ctrl/Alt do not exist as events). A scancode-level event (`code` = make/
  break) would be the next fidelity step.
- The 16-deep shared queue means a burst of >16 events between app slices
  drops the overflow silently (same policy as clicks since v0.30).
- Only the focused window's surface receives keys; there is no input-method
  layer, no per-surface keymap.
- Prior gaps unchanged: window-spring physics still per-frame; fling sampling
  per-frame; single consumer for flips; page tables leak on exit;
  uniprocessor invariants; VT-x unavailable under TCG.
