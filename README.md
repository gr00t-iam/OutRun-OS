OUTRUN OS — Core Prototype v0.1
A working, compiling, running prototype of the Outrun OS core: a memory-safe Rust
microkernel supervisor, user-space drivers in C, applications in C++, and hot-path
primitives in hand-written x86_64 assembly — the full polyglot stack from the
project charter, linked into one boot set by a single `make`.
This is not pseudocode. `make run` boots the kernel simulation and demonstrates
live, with real processes and real shared memory, every load-bearing claim in the
manifesto: zero-copy unrestricted hardware streaming after a one-time capability
grant, a user-space camera driver that segfaults and is restarted in milliseconds
while its consumers never lose a mapping, instant signal-free capability
revocation via generation counters, and an A/B atomic update engine that rejects
a corrupted image, rolls back untouched, and then flips roots reboot-free.
Build and run
Requirements: `gcc`, `g++`, `nasm`, `make`, and a Rust toolchain (`cargo`). No
external crates or libraries are used anywhere.
    make        # builds all four languages into build/
    make run    # boots the kernel demo (about 5 seconds)
    make clean

Layout
    include/outrun_abi.h            single source of truth for the shared memory ABI
    arch/x86_64/ring_fastpath.asm   hot-path poll, capability check, cpu_relax (NASM)
    kernel/                         Rust microkernel core: capability table, device
                                    regions, process supervisor, A/B update engine
    drivers/camera_driver.c         user-space C driver; produces 200fps frames,
                                    survives injected faults by isolated restart
    apps/stream_app.cpp             C++ app; consumes frames zero-copy via the asm
                                    fast path, exits gracefully on revocation
    services/gesture_service.c      system gesture engine; second zero-copy consumer,
                                    emits derived landmark events, never raw pixels
    docs/                           ARCHITECTURE.md, SECURITY.md, ROADMAP.md, and a
                                    captured demo-run.log from a real boot

What a real run looks like
From the captured log in `docs/demo-run.log`: the stream app sustained roughly
188 fps of zero-copy frames (11.2 MB consumed with zero per-frame syscalls); the
injected driver SIGSEGV produced a supervisor restart 18 ms later, and the worst
stall any consumer ever observed across the entire crash was 32.4 ms; revocation
of the app's capability took effect on its very next poll with no signal sent.
Design resolution baked into this code
The charter demands both unrestricted hardware access and real security. This
prototype implements the reconciliation: the kernel mediates only the setup of
a hardware mapping (mint and grant a capability, once), then leaves the data path
entirely — after the grant, access is genuinely direct, zero-copy, and
prompt-free. Revocation never requires trusting or interrupting the holder: the
kernel bumps a generation counter and the mapping is dead on the holder's next
access check, which costs one assembly compare. See `docs/SECURITY.md`.

Deploying Outrun OS 0.2.0-metal — Proxmox and Bare Metal
What this image is, honestly
This is a real, bootable Outrun OS kernel — GRUB Multiboot2 boot, hand-written
assembly bootstrap into x86_64 long mode, a freestanding C core with a live
interrupt system, dual VGA/serial console, Multiboot2 memory-map parsing, and
the Outrun capability table running in kernel space with an interactive shell.
It has been boot-verified under both SeaBIOS-style legacy boot and OVMF UEFI
firmware, the two firmware options Proxmox offers.
It is a development kernel at roadmap Phase 1, not a production operating
system. It has no filesystem, no networking, no GUI, and no user mode yet.
Anyone who tells you a production OS rivaling Windows can be produced in a day
is selling something; what you have here is the honest foundation — a kernel
you own completely, that boots on real firmware, that you can extend phase by
phase per docs/ROADMAP.md.
Proxmox VE deployment
Upload `metal/build/outrun-os-0.2.0.iso` to your Proxmox node (Datacenter →
node → local storage → ISO Images → Upload, or scp it to
`/var/lib/vz/template/iso/`). Create a VM with these settings: OS type Other,
the Outrun ISO attached as CD/DVD, BIOS either SeaBIOS (default) or OVMF —
both are verified working, 512 MB+ RAM, 1+ cores, and no hard disk required.
Display can stay Default; the kernel drives the VGA text console.
For the best experience add a serial console: in the VM's Hardware tab add a
Serial Port (serial0), then use `qm terminal <vmid>` or the xterm.js console.
The Outrun shell listens on VGA+PS/2 and COM1 simultaneously, so both the
noVNC display and the serial terminal are live shells at the same time.
Boot the VM. GRUB appears for two seconds, then the kernel banner, the memory
map Proxmox handed us, and the `outrun>` prompt. Type `help`, run `demo` for
the capability walk-through, `mem` to re-read the e820 map, `panic` to watch
the exception handler contain a deliberate CPU fault, `reboot` to warm-reset.
Bare metal / NVMe deployment
The ISO is a hybrid image (El Torito BIOS + UEFI entries plus an MBR boot
sector), so it can be written directly to any raw block device:
    dd if=outrun-os-0.2.0.iso of=/dev/nvme0n1 bs=4M status=progress
    sync

Double-check the target device name first — dd overwrites everything on it,
and this should never be a disk holding data you care about. Then boot the
machine from that drive in either legacy or UEFI mode. A USB stick works
identically and is the safer first test on real hardware.
Hardware notes for bare metal: the kernel needs an x86_64 CPU, drives the
legacy VGA text console and PS/2-compatible keyboard path (most desktop
firmware emulates PS/2 for USB keyboards via SMM; some very new boards don't,
in which case use the serial header or test in a VM), and prints to COM1 at
115200 8N1 if a serial port exists.
Building from source
    cd metal && make          # kernel.elf + hybrid ISO
    make qemu                 # boot it headless, serial shell on stdio
    make qemu-vga             # boot with a display window

Toolchain: gcc, nasm, ld, grub-mkrescue (grub-pc-bin + grub-efi-amd64-bin +
xorriso + mtools), qemu-system-x86 for testing. All stock Ubuntu/Debian
packages; no cross-compiler required.
What's next
The userspace prototype in the repo root already proved the OS's core
protocols (zero-copy device regions, generation-revoked capabilities, crash
supervision, A/B updates) with measured numbers. This metal kernel is the
vessel those protocols migrate into: Phase 2 adds user mode and ELF loading so
the driver/app/gesture trio runs on this kernel instead of Linux, then
virtio drivers, then storage and the content-addressable filesystem. Each
phase keeps the same rule this one followed — nothing claimed that isn't
booted and demonstrated.

Outrun OS — Desktop Environment Engineering & Visual Design Specification
Document version 0.3 · Target: the Outrun OS compositor, shell, and workspace layer
Authoring roles: OS Architecture + Principal UI/UX Engineering
This specification defines concrete design patterns, data structures, interface
dimensions, and layouts for the Outrun OS desktop environment. All dimensions
are given in logical points (pt); the compositor multiplies by the display
scale factor (1.0 at ~110 PPI, 2.0 at ~220 PPI) at shader time, so every value
below is resolution-independent unless a raw pixel value is stated explicitly.
---
SECTION 1 — Visual Identity & the Spatial Canvas Interface
1.1 The visual language: Generative Glass
Outrun's surfaces are not flat fills. Every panel is a glass layer composited
in three stacked passes over whatever is behind it. The compositor runs these as
GPU fragment shaders; nothing here is a pre-baked bitmap.
Each glass layer is described by this structure, which the compositor uploads as
a per-surface uniform block:
```c
struct GlassLayer {
    float    blur_sigma;        // Gaussian blur radius, in pt (see table)
    float    saturation;        // backdrop chroma boost, 1.0 = passthrough
    float    tint_rgba[4];      // material tint applied over the blur
    float    noise_amplitude;   // dither to kill banding, ~0.006
    float    depth_z;           // 0.0 = focused, larger = further back
    float    corner_radius_pt;  // 12 window, 8 panel, 6 control
    uint32_t contrast_mode;     // 0 auto, 1 force-dark-text, 2 force-light-text
};
```
The three passes, in order:
Backdrop blur. A separable Gaussian sampled from the framebuffer region
directly behind the surface. Blur sigma is quantized by role so the whole UI
reads as one coherent depth stack rather than a soup of random blurs:
Surface role	blur_sigma (pt)	saturation	tint alpha
Global status bar	20	1.15	0.55
Background window glass	32	1.05	0.42
Foreground panel / sheet	16	1.20	0.60
Accelerator HUD	48	1.30	0.72
Context ribbon	24	1.10	0.50
Higher sigma = more separation from content behind it. The HUD blurs hardest
(48pt) because it must feel like it floats above everything.
Chroma-adaptive tint. After blur, the shader samples the mean color of
the blurred backdrop and rotates the material tint toward it by 18–24% so the
glass "belongs" to the wallpaper without becoming the wallpaper. Saturation is
boosted per the table so blurred content stays lively rather than muddy.
Contrast guarantee (the important part). Before any text draws, the shader
computes the mean relative luminance `L_bg` of the blurred-and-tinted region
under the text's bounding box using the WCAG luminance formula:
```
   L = 0.2126·R_lin + 0.7152·G_lin + 0.0722·B_lin
   ```
where each channel is linearized (`c ≤ 0.03928 ? c/12.92 : ((c+0.055)/1.055)^2.4`).
It then picks the text color (near-white `#F2F4F8` or near-black `#0B0E14`)
that maximizes contrast ratio:
```
   CR = (L_lighter + 0.05) / (L_darker + 0.05)
   ```
If the winning `CR < 4.5` (WCAG AA for body text), the shader deepens the
glass tint alpha in a local feedback loop, +0.04 per step up to +0.20, until
`CR ≥ 4.5`. For text ≥ 20pt semibold the threshold relaxes to 3.0 (AA large).
Headline chrome targets 7.0 (AAA). The result: readable text over any
wallpaper, guaranteed by math at composite time, not by the app developer
remembering to test on a bright background.
1.2 Shadow depth scaling
Windows cast shadows whose softness and offset scale with interaction
recency, not a fixed z-index. The compositor keeps a monotonic
`last_focus_tick` per window and derives a depth rank `d` (0 = most recently
focused). Shadow parameters:
```
offset_y_pt = 4 + 6·d          (clamped at d = 4)
blur_pt     = 12 + 10·d
spread_pt   = -2                (tight inner spread keeps edges crisp)
alpha       = 0.38 − 0.05·d     (further windows cast fainter shadows)
```
So the active window sits under a tight, dark, close shadow
(`0, 4pt, 12pt, rgba(0,0,0,0.38)`); a window four layers back casts a soft, wide,
faint one (`0, 28pt, 52pt, rgba(0,0,0,0.18)`). This makes focus physically
legible — your eye finds the active window because it is the one floating
lowest and hardest, exactly like a real object closest to a light source.
1.3 The Infinite Canvas: windows as physical bodies
Windows are rigid bodies on a 2D canvas with a real physics integrator running
at the compositor's refresh rate (120 Hz where available, 60 Hz floor). Each
window carries:
```c
struct CanvasBody {
    double  x, y;             // canvas-space position (pt), unbounded
    double  vx, vy;           // velocity (pt/s)
    double  w, h;             // extent (pt)
    double  mass;             // = (w·h) / 10000, so big windows feel heavier
    double  k_spring;         // restoring stiffness, default 220 N/m-equivalent
    double  zeta;             // damping ratio, default 0.82 (snappy, minimal overshoot)
    uint8_t pinned;           // 1 = ignores pushes (user-anchored)
};
```
Motion. A window being dragged is kinematic (follows the cursor/finger). On
release it integrates under a critically-tuned spring toward its resting slot:
```
c      = 2·zeta·sqrt(k_spring·mass)     // damping coefficient
F      = −k_spring·(pos − target) − c·velocity
accel  = F / mass
```
`zeta = 0.82` is deliberately just under 1.0: windows settle fast with a single
barely-perceptible overshoot, which reads as "alive" without the nauseating
bounce of an underdamped (`zeta < 0.5`) spring.
Non-overlap by collision, not by tiling. When a window is dropped onto space
occupied by another, the physics solver treats them as colliding AABBs and
resolves penetration by applying separating impulses along the minimum
translation vector, weighted by mass. A small window shoved at a large one moves
mostly itself; a large window shoved at a small one pushes the small one aside.
Neighbors ripple outward and settle. Windows never stack into a hidden pile —
they make room. Pinned windows have infinite effective mass and do not move.
Infinite zoomable space. The canvas has no edges. A camera transform
`(cam_x, cam_y, cam_scale)` maps canvas space to the display. Pinch or
Ctrl+scroll drives `cam_scale` across a range of 0.05 (whole-project bird's-eye)
to 2.0 (zoomed into one window). Below `cam_scale = 0.35` windows render as
glyph cards — title, app icon, and a 4-line content thumbnail — so a zoomed-
out project landscape stays legible instead of becoming unreadable slivers.
1.4 Main desktop wireframe
```
┌──────────────────────────────────────────────────────────────────────────────┐
│ ◐ Outrun   ⌂ Canvas ▸ "App-Core"        ⌕ Search (Alt+Space)        ▣ 78% ⏻ 10:08│  ← Global status bar (32pt)
├──────────────────────────────────────────────────────────────────────────────┤
│  ┌─ Hardware Passthrough ─────────────┐                                        │
│  │ ● camera0   → stream-app  [DIRECT] │   ← live passthrough widget (pinned)   │
│  │ ● mic0      → mixer       [DIRECT] │      shows which app owns which device  │
│  │ ○ pad0      → (unclaimed)          │                                        │
│  └────────────────────────────────────┘                                        │
│                                                                                │
│        ┌───────────────────────────┐              ┌──────────────────────┐     │
│        │  TIME-STREAM              │   (elastic    │  COMM-DECK           │     │
│        │  ◄ Mar 2026 ─── Today ►   │    spacing    │  Sarah · Dev Chat    │     │
│        │  [ img ] [ doc ] [ pdf ]        │    gap ~40pt) │  ✉ 3  ● 1            │     │
│        │  active shadow: tight     │  ◄~~~~~~~~~►  │  glass: background    │     │
│        └───────────────────────────┘              └──────────────────────┘     │
│                                                                                │
│    ┌──────────────────────────────────────────────────────────────────┐       │
│    │  ▶ Active Workspace: Video Editor                                  │       │
│    │    direct 4K camera feed + audio HAL ring · casts tightest shadow  │       │
│    │    (this window is depth rank 0 → floats lowest, darkest shadow)   │       │
│    └──────────────────────────────────────────────────────────────────┘       │
│                                                                                │
├──────────────────────────────────────────────────────────────────────────────┤
│ Context ▸ scroll: zoom canvas · 3-finger drag: pan · drop window: bodies part  │  ← Context ribbon (28pt)
└──────────────────────────────────────────────────────────────────────────────┘
```
Global status bar: 32pt tall, `blur_sigma 20`, full width. Left: system
glyph + current canvas breadcrumb. Center: the Accelerator HUD trigger. Right:
battery/network/clock. It is the only always-pinned chrome.
Hardware passthrough widget: a pinned canvas body, 260×96pt, that names
every device currently memory-mapped into an app and which app holds it. A
filled dot (`●`) = live direct passthrough; hollow (`○`) = unclaimed. This is
the visible face of the capability model — you can always see who is holding
your camera.
Context ribbon: 28pt tall, `blur_sigma 24`, bottom-anchored, monochrome.
Its text is rewritten every frame to describe what the current pointer/gesture
will do in the hovered surface.
---
SECTION 2 — The Universal Runtime & Application Layout
2.1 Dual distribution framework
Every app, regardless of origin, is delivered as an Outrun Bundle: a
content-addressed, read-only image plus a capability manifest. Two paths produce
bundles; both feed the identical sandbox.
```
             ┌──────────────────────── OUTRUN BUNDLE ───────────────────────┐
             │  image: CAS root hash (all deps included, read-only)          │
             │  manifest: requested capabilities + UI-compliance attestation │
             │  signature: publisher key (Marketplace CA) OR self-signed key │
             └───────────────────────────────────────────────────────────────┘
                    ▲                                            ▲
        ┌───────────┴───────────┐                    ┌───────────┴───────────┐
        │   OUTRUN MARKETPLACE   │                    │  DECENTRALIZED CHANNEL │
        │  • automated UI check  │                    │  • git:// or ipfs:// URL│
        │  • static bytecode audit│                   │  • self-signed key      │
        │  • 0% tax on free apps │                    │  • local build/interpret│
        │  • 2–5% flat on paid   │                    │  • web-of-trust pinning │
        └────────────────────────┘                    └────────────────────────┘
```
Marketplace intake pipeline (per submission):
UI-compliance scan. A headless compositor renders the app across four
synthetic displays (1080p@1.0, 1440p@1.5, 4K@2.0, 8K@2.0) in both light and
dark, and asserts: uses system glass tokens, honors the 8pt grid, text passes
the §1.1 contrast math, no hardcoded pixel fonts. Failures return a
screenshot diff, not a rejection letter.
Static bytecode audit. The WASM/native image is scanned for undeclared
capability use, embedded telemetry endpoints, and known-malicious code
patterns. Anything reaching hardware or network that the manifest did not
declare is flagged.
Publish. Passing bundles are pinned by content hash. Free apps pay
nothing, ever; paid apps carry a flat 2–5% processing fee that covers hosting
only — there is no 30% gate.
Decentralized path: `outrun install git://github.com/dev/app` clones,
builds or interprets locally into a bundle, signs it with the developer's key,
and pins the key on first install as a trust anchor. Subsequent updates from a
new key for the same app surface a clear trust-change prompt. No central
gatekeeper is involved, and the sandbox applied is byte-for-byte the same one
Marketplace apps get.
2.2 Compatibility: the container/VM layer
Outrun ships a low-overhead runtime matrix so users have software on day one:
Target	Mechanism	Isolation boundary
Outrun-native (WASM)	in-process WASM runtime, capability imports	capability manifest
Outrun-native (ELF)	direct exec in a sandbox namespace	capability manifest
Linux binaries	micro-VM with a paravirtual syscall bridge	hardware virtualization
Android APKs	micro-VM running an AOSP userspace + ART	hardware virtualization
The Linux/Android micro-VMs are not full emulators; they are thin VMs (a
single kernel, virtio devices, direct-mapped GPU where permitted) that boot in
well under a second and present their apps as ordinary canvas bodies. A Linux
app and a native app sit side by side on the same infinite canvas, each in its
own glass frame.
2.3 Unrestricted hardware access, inside the sandbox
This is the reconciliation the project insists on: direct MMIO to
cameras/mics/controllers with no prompt walls, while still sandboxed. The
mechanism is grant-at-setup, unrestricted-at-use, exactly as implemented in the
`caps.rs` subsystem prototype accompanying this spec.
Flow when an app requests a device:
```
app calls sys_hardware_passthrough(self, device)
        │
        ▼
microkernel checks the app's Capabilities bitset  ── lacks bit ──► request dropped
        │                                                          (app sees a
        │ holds bit                                                 secure device
        ▼                                                           picker instead,
maps the device MMIO window directly into the app's                 never a crash)
address space  →  app now reads/writes hardware registers
directly, zero-copy, with NO further kernel involvement
per frame and NO recurring "allow?" prompt
```
The capability is granted once — at install from the manifest for
Marketplace apps, or via a single fast HUD confirmation for sideloaded apps —
and then the data path is genuinely unmediated: a 4K camera feed or a
1000 Hz controller streams straight into app memory. The security is that a
capability the app never received cannot be exercised at all: the bitset check
in `sys_hardware_passthrough` returns a hard denial, and there is no mapping to
abuse. An app you never gave the microphone to physically cannot open it, no
matter how it is packaged or where it came from.
To keep this safe on real hardware, every DMA-capable device sits behind its own
IOMMU domain, so "unrestricted" MMIO means unrestricted access to that device's
registers — never to arbitrary system memory.
---
SECTION 3 — The Hybrid Radical CLI & Shell Environment
3.1 Design: the Outrun Shell (`osh`)
`osh` is a GPU-rendered, object-aware shell. It carries structured objects
through pipelines like PowerShell, keeps the muscle-memory ergonomics and
scripting compatibility of Zsh/POSIX, and renders in the compositor so it can
show inline sparklines, progress rings, and syntax-highlighted structured output
without leaving the terminal grid.
Core model: commands emit typed object streams, not just bytes. A pipeline
stage receives an array of records (each a map of typed fields). A final stage
with no consumer auto-renders the records as an aligned table. Crucially, any
stage may request a byte view of the stream (`| bytes`), at which point the
records serialize to their text form — this is the bridge that keeps every
existing POSIX tool (`grep`, `awk`, `sed`, `jq`) working unchanged inside `osh`.
```
records ──► [ osh-native stage ]──► records ──► [ | bytes ]──► text ──► grep ──► text
```
The renderer is a shader over a glyph atlas: monospace grid, ligature-aware,
with per-cell background so structured output can shade columns. Scrollback is
a ring of records, so you can re-query old output (`$LAST | where size > 1MB`)
without re-running the command.
3.2 Administrative task syntax
Task A — query local hardware pass-through addresses. `hw` emits device
records; each row is a live object you can pipe further.
```
outrun ~ ▸ hw ls --passthrough

  DEVICE        MMIO_BASE     LEN      HOLDER        CAP           STATE
  ───────────   ───────────   ──────   ───────────   ───────────   ──────
  camera0       0xF0000000    1.0 MiB  stream-app    CAMERA        DIRECT
  microphone0   0xF0100000    64 KiB   mixer         MICROPHONE    DIRECT
  controller0   0xF0200000    4 KiB    —             CONTROLLER    FREE

  3 devices · 2 mapped direct · pipe to inspect: `hw ls --passthrough | where holder == "stream-app"`

outrun ~ ▸ hw ls --passthrough | where state == DIRECT | select device, holder

  DEVICE        HOLDER
  camera0       stream-app
  microphone0   mixer
```
Task B — inspect active A/B root slots. `slot` reads the atomic update
engine's state directly.
```
outrun ~ ▸ slot status

  SLOT   ROLE      VERSION        ROOT_HASH             HEALTH
  ────   ───────   ───────────    ──────────────────    ──────
  A      ACTIVE    0.3.0-metal    4a1bc893f4f85abe      ✓ verified
  B      PASSIVE   0.3.1-metal    947e38d76ebf0f9b      ✓ staged, hash-checked

  next boot: A (unchanged) · `slot flip` to activate B reboot-free for userspace
  rollback armed: yes (auto-revert to A if B fails liveness within 90s)

outrun ~ ▸ slot diff A B | where change == modified | head 3

  PATH                         CHANGE      OLD_HASH    NEW_HASH
  /sys/lib/libcompositor.so    modified    3f9c…       a17e…
  /sys/svc/gesture-engine      modified    88 b1…      c204…
  /sys/lib/libglass.so         modified    12de…       9f45…
```
Task C — search the chronological AI Time-Stream. `ts` queries the local
vector index; results are records with a timestamp, a semantic score, and a CAS
hash you can open or restore.
```
outrun ~ ▸ ts find "the chart Sarah sent me about Q3" --since 30d

  WHEN              SCORE   KIND    TITLE                     FROM     CAS_HASH
  ───────────────   ─────   ─────   ───────────────────────   ──────   ────────────
  Mar 14 14:22      0.94    image   q3_revenue_chart.png      Sarah    868d9e65…
  Mar 14 14:19      0.71    email   "Q3 numbers for review"   Sarah    c128a5ea…
  Feb 28 09:05      0.55    doc     Q3_briefing.pdf           —        3f7a0011…

  3 results · open: `ts open 868d9e65` · restore workspace: `ts restore 868d9e65 --with-context`

outrun ~ ▸ ts find "programming session last night" | ts restore $0.first --with-context
  ↳ restoring canvas state from Mar 20 23:30: Index.rs, Build_Output.txt, 4 browser tabs, debugger
```
3.3 Hardened security without slowing developers
Capability tokens per shell tab. Each `osh` tab is a process with its own
Capabilities bitset (the same structure enforced in `caps.rs`). A fresh tab
starts with `CAP_FILESYSTEM` scoped to the current canvas project and nothing
else — no camera, no raw network, no view of other tabs' processes. Elevating is
explicit and per-tab:
```
outrun ~ ▸ cap grant net --scope this-tab --ttl 1h
  ↳ CAP_NETWORK granted to tab pid 4471 for 60m (auto-revokes; `cap drop net` to end early)
```
Isolated namespaces per tab. Each tab gets a private mount/PID/network
namespace. A destructive command in one tab (`rm -rf` inside a project) cannot
reach another tab's project, and a runaway process is visible and killable only
within its own tab. This is container-grade isolation applied at the granularity
of a terminal tab, established in well under a millisecond at tab spawn so it is
invisible to the user.
POSIX compatibility is preserved because the namespace + capability layer
sits below the command interface: scripts see a normal filesystem and normal
processes; they simply cannot see or touch anything outside their grant. A
standard `bash` script run under `osh` executes unchanged — it just runs inside
the tab's sandbox, with the `| bytes` bridge handling any structured input it is
handed. Developers pay no syntax tax and lose no tooling; the isolation is
ambient.
---
SECTION 4 — The Integrated Chronological Context Workspace
4.1 Time-Stream ↔ Comm-Deck: the shared substrate
The Time-Stream (chronological file/history engine) and the Comm-Deck
(protocol-agnostic chat/email/notification pipeline) are two faces of one local
data core. They share three services: the CAS store (every artifact — an
attachment, a document, a message body — is content-addressed, per the
`cas_vfs.rs` prototype), the edge AI embedder (turns artifacts into vectors
on idle NPU/GPU cycles), and the event log (a single append-only timeline of
typed events that both surfaces render).
Every event flowing through either surface is one record type:
```c
struct StreamEvent {
    uint64_t timestamp_ns;      // when it entered the system
    uint32_t kind;              // MSG | FILE | STATE | NOTIFY
    uint32_t source_protocol;   // SIGNAL | MATRIX | EMAIL | SMS | LOCAL
    uint64_t contact_id;        // resolved cross-protocol identity
    uint64_t cas_hash;          // content-addressed body/attachment
    uint64_t vector_id;         // handle into the local embedding DB (0 = pending)
    uint64_t workspace_ref;     // canvas state to restore, if any
    uint16_t priority_lane;     // ACTIONABLE | PASSIVE | MUTED
};
```
Because both surfaces read the same `StreamEvent` log keyed by the same
`contact_id` and `cas_hash`, a message and the file it references are already
linked with no extra work: the Comm-Deck shows the message, the Time-Stream
shows the file, and both point at the same CAS hash and the same moment on the
timeline.
4.2 End-to-end data flow: message arrival to timeline
```
  ┌────────────────────────────────────────────────────────────────────────────┐
  │  t+0 ms   INGRESS                                                            │
  │  Signal/Matrix/Email/SMS frame arrives at the protocol-agnostic Comm-Deck   │
  │  gateway. Normalized into a StreamEvent{kind=MSG, source_protocol=…,         │
  │  contact_id=resolve(sender)}.                                               │
  └───────────────────────────────┬────────────────────────────────────────────┘
                                  ▼
  ┌────────────────────────────────────────────────────────────────────────────┐
  │  t+1 ms   CONTENT-ADDRESS                                                    │
  │  Message body + any attachment are written to the CAS store (write_file).   │
  │  Deduplicated automatically; cas_hash filled into the event. If the         │
  │  attachment already exists (someone re-sent it), 0 new bytes are stored.    │
  └───────────────────────────────┬────────────────────────────────────────────┘
                                  ▼
  ┌────────────────────────────────────────────────────────────────────────────┐
  │  t+2 ms   PRIORITY LANE + INSTANT SURFACE                                    │
  │  Rule engine assigns priority_lane. Event is appended to the event log and  │
  │  BOTH surfaces update immediately:                                          │
  │     • Comm-Deck: message appears in the contact's unified thread            │
  │     • Time-Stream: a provisional card appears at "now" (vector_id = 0)      │
  │  The user sees the message with NO wait on AI.                              │
  └───────────────────────────────┬────────────────────────────────────────────┘
                                  ▼
  ┌────────────────────────────────────────────────────────────────────────────┐
  │  t+… (idle)   LOW-PRIORITY EDGE EMBEDDING                                    │
  │  On the next idle NPU/GPU window (yields instantly if a game/creative app   │
  │  claims hardware), the embedder reads the CAS body, produces a multimodal   │
  │  vector, writes it to the local vector DB, and patches vector_id into the   │
  │  event. Nothing left the machine.                                           │
  └───────────────────────────────┬────────────────────────────────────────────┘
                                  ▼
  ┌────────────────────────────────────────────────────────────────────────────┐
  │  t+… settled   SEMANTIC INDEXED                                             │
  │  The provisional Time-Stream card is now fully searchable by meaning:       │
  │  `ts find "chart Sarah sent about Q3"` resolves to it via vector_id.        │
  │  Auto-tags (people, project, content) are attached. No manual filing ever.  │
  └───────────────────────────────┬────────────────────────────────────────────┘
                                  ▼
  ┌────────────────────────────────────────────────────────────────────────────┐
  │  DEEP LINK   (when the user clicks the message later)                        │
  │  workspace_ref resolves: the infinite canvas snaps back to the exact files, │
  │  tabs, and window layout that were open when this message arrived, because  │
  │  the event captured the canvas state hash at ingress.                       │
  └────────────────────────────────────────────────────────────────────────────┘
```
4.3 Why the integration is automatic, not bolted-on
The administrative "magic" — a teammate writes "update that layout we looked at
last Tuesday" and the right file surfaces — falls out of the shared substrate
with no special-case code. The incoming message is a `StreamEvent` with a
`contact_id`; the local semantic AI embeds its text and runs a vector query
against the Time-Stream; the highest-scoring event from "last Tuesday" with that
same `contact_id` is the design file; its `workspace_ref` restores the session.
Comm-Deck and Time-Stream never call each other directly — they simply read and
write the same content-addressed, vector-indexed event log, which is what makes
the whole desktop feel like one contextual workspace instead of a pile of apps.
---
Prototype cross-reference
The subsystems referenced above are implemented as compiling, runnable code in
the `subsystems/` directory of this repository:
§1.1 contrast math / §4 content addressing → `subsystems/cas_vfs.rs`
(block-level dedup, stable content hashes, verified 4× compaction).
§2.3 capability-gated passthrough → `subsystems/caps.rs`
(grant-vs-drop enforced by bitset on `sys_hardware_passthrough`).
§4.2 zero-copy delivery to isolated workspaces → `subsystems/packet_router.cpp`
(SPSC ring, payloads copied once at ingress, delivered by pointer).
Every mechanism specified here that could be reduced to running code has been,
and each demo's invariants are asserted at runtime rather than asserted in prose.

Outrun OS — Metropolis-Terminal Visual Design Blueprint
Original visual language. No borrowed marks, characters, logos, or property from
any existing film, game, or franchise. Every color, glyph, and motion curve below
is defined from first principles as a technical parameter.
Design roles: Principal UI/UX Architecture + Lead Graphics Shader Development.
---
SECTION 1 — The Metropolis-Terminal Palette & Shader Math
1.1 Dual-theme color architecture (exact hex)
Theme A — High-Contrast Neon (default; obsidian + electric accents)
Token	Hex	Role
`bg.obsidian.0`	`#05060A`	deepest desktop void
`bg.obsidian.1`	`#0A0D14`	panel base plate
`bg.obsidian.2`	`#121722`	raised surface / input well
`stroke.hairline`	`#1E2735`	1px structural separators
`accent.cyan`	`#22E4FF`	primary accent, active edges, focus glow
`accent.cyan.dim`	`#0E7C8C`	inactive accent, idle telemetry lines
`alert.magenta`	`#FF2D9B`	errors, destructive confirms, hot alerts
`track.amber`	`#FFB020`	tracking highlights, gesture crosshair, focus
`text.primary`	`#EAF2F7`	body text on obsidian
`text.muted`	`#7C8CA0`	secondary labels, metadata tags
`ok.mint`	`#3DF5C4`	healthy status, verified hashes
Theme B — Industrial Cathode (rugged; charcoal plate + military amber)
Token	Hex	Role
`plate.charcoal.0`	`#0C0C0A`	textured backplate (paired with noise map)
`plate.charcoal.1`	`#17160F`	raised riveted panel
`plate.charcoal.2`	`#221F16`	recessed slot
`stroke.stencil`	`#3A3524`	stencil separators, bracket strokes
`amber.font`	`#FFC24A`	primary military-amber typeface color
`amber.font.dim`	`#8A6A24`	inactive amber, ghosted labels
`green.radioactive`	`#9DFF3C`	monochrome CRT readouts, live meters
`orange.hazard`	`#FF6A1A`	hazardous warning banners, over-temp
`text.stencil`	`#E8D9A8`	high-legibility stencil body text
`text.stencil.muted`	`#9A8C63`	secondary stencil metadata
`warn.zebra.a`	`#000000`	error zebra stripe A
`warn.zebra.b`	`#FFB020`	error zebra stripe B (glowing amber)
Themes swap by remapping a single uniform block; all components reference tokens,
never raw hex, so a theme flip is one buffer upload with zero relayout.
1.2 The "CRT Glass Glow" compositor filter
The compositor runs a two-stage post pass. Stage 1 is per-surface Material
Diffusion (glass); Stage 2 is a full-screen CRT Glass Glow applied to
everything behind the focused surface, so the active window stays razor-crisp
while the background reads as a warm powered-on display.
All four effects — scanlines, curvature warp, chromatic aberration, bloom — live
in one WGSL fragment shader sampling the composited backbuffer texture `tex` at
normalized `uv`.
Screen-curvature warp (1.5%). Bend UVs outward from center so the plane reads
as a subtly convex tube. `k = 0.015` is the 1.5% strength.
```wgsl
fn curve_uv(uv: vec2<f32>) -> vec2<f32> {
    let k: f32 = 0.015;                 // 1.5% barrel
    var c = uv * 2.0 - 1.0;             // center to [-1,1]
    let r2 = dot(c, c);
    c = c * (1.0 + k * r2);             // push edges out with radius^2
    return c * 0.5 + 0.5;              // back to [0,1]
}
```
Chromatic aberration. Sample R and B channels along a radial offset that
grows toward the edges (lenses disperse most at the rim), G stays centered.
```wgsl
fn sample_ca(uv: vec2<f32>) -> vec3<f32> {
    let center = vec2<f32>(0.5, 0.5);
    let dir = uv - center;
    let amt = 0.0016 + 0.0032 * dot(dir, dir); // stronger at edges
    let off = dir * amt;
    let r = textureSample(tex, samp, uv + off).r;
    let g = textureSample(tex, samp, uv).g;
    let b = textureSample(tex, samp, uv - off).b;
    return vec3<f32>(r, g, b);
}
```
Horizontal scanlines (faint). A sine over the vertical pixel index, kept at
~4% depth so it textures without harming legibility. Line pitch is 2 physical
pixels; multiply density by the display scale so it holds at any DPI.
```wgsl
fn scanline(uv: vec2<f32>, screen_h: f32) -> f32 {
    let line = sin(uv.y * screen_h * 3.14159265); // 1 cycle / 2 px
    return 1.0 - 0.04 * (0.5 + 0.5 * line);        // 4% dip on dark lines
}
```
Bloom / glass glow. A cheap 5-tap separable bright-pass on the diffused
buffer, added back so neon strokes and amber readouts halo softly.
```wgsl
@fragment
fn crt_glass_glow(@location(0) uv0: vec2<f32>) -> vec4<f32> {
    let uv = curve_uv(uv0);
    // off-tube pixels render as the bezel void
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return vec4<f32>(0.02, 0.024, 0.03, 1.0);  // bg.obsidian.0-ish bezel
    }
    var col = sample_ca(uv);                        // chromatic aberration
    // bright-pass bloom (5-tap cross)
    let px = 1.0 / vec2<f32>(u.screen_w, u.screen_h);
    var glow = vec3<f32>(0.0);
    glow += max(col - 0.6, vec3<f32>(0.0));
    glow += max(textureSample(tex, samp, uv + vec2<f32>( px.x, 0.0)).rgb - 0.6, vec3<f32>(0.0));
    glow += max(textureSample(tex, samp, uv + vec2<f32>(-px.x, 0.0)).rgb - 0.6, vec3<f32>(0.0));
    glow += max(textureSample(tex, samp, uv + vec2<f32>(0.0,  px.y)).rgb - 0.6, vec3<f32>(0.0));
    glow += max(textureSample(tex, samp, uv + vec2<f32>(0.0, -px.y)).rgb - 0.6, vec3<f32>(0.0));
    col += glow * 0.35;                              // additive halo
    col *= scanline(uv, u.screen_h);                 // scanlines
    // vignette to seat the tube in its bezel
    let v = 1.0 - 0.25 * dot(uv - 0.5, uv - 0.5) * 4.0;
    col *= v;
    return vec4<f32>(col, 1.0);
}
```
Material Diffusion (glass) — the per-surface pre-pass. Before the CRT pass,
each panel is a glass layer: separable Gaussian blur of the backdrop (sigma by
role — 16pt panels, 32pt background windows, 48pt HUD), chroma-adaptive tint
toward the sampled backdrop mean, and the WCAG contrast guarantee that deepens
tint alpha in +0.04 steps until body text clears a 4.5:1 ratio. The focused
surface is exempted from Stage 2 CRT distortion (curvature/aberration = 0 over
its rect) so the thing you are working in is always optically perfect.
Cost budget. Whole post chain is one full-screen pass, < 0.4 ms at 4K on a
mid-range GPU. Effects are per-theme scalable: Theme A runs aberration at
`0.0016`, Theme B pushes it to `0.0026` and drops scanline pitch to a chunkier
3px for the heavier industrial feel.
---
SECTION 2 — The Spatial Infinite Canvas Wireframe
Asymmetrical layout: a rugged left-aligned telemetry bracket runs the full
height; application panels float borderless with sharp 45° corner cuts (never
rounded); a glowing amber crosshair sits at the optical center as the gesture
focus point.
```
╔═╗                                                                              
║T║  OUTRUN // metropolis-terminal                    ⌗ 78%  ▲NET  22:41:07  ▮▮▮
║E║ ┌─────────────────────────────────────────────────────────────────────────┐
║L║ │                                                                          
║E║ │        ╱───────────────────────╲            ╱────────────────────╲       
║M║ │       ╱   TIME-STREAM           ▔╲          ╱  COMM-DECK          ▔╲     
║E║ │      │  ◄══ Mar '26 ═══ NOW ══►  │        │  ▸ Sarah    ▸ Dev-Net  │    
║T║ │      │  [img] [doc] [vec] [bin]  │        │  ✉ 03   ◉ 01   ◈ mic   │    
║R║ │       ╲  45° cut corners, no     ╱         ╲  borderless glass      ╱    
║Y║ │        ╲───────────────────────╱            ╲────────────────────╱      
║ ║ │                                                                          
║B║ │                              ╱▔▔╲                                        
║R║ │                          ────┤ ✛ ├────   ◄ gesture crosshair (amber)     
║A║ │                              ╲▁▁╱             tracks 21-pt hand landmark 
║C║ │                                                                          
║K║ │        ╱──────────────────────────────────────────────────╲            
║E║ │       ╱  ▸ ACTIVE WORKSPACE — Video Editor                  ▔╲          
║T║ │      │   direct 4K cam + audio HAL ring · casts tight shadow  │         
║ ║ │       ╲  focused surface: CRT distortion OFF, optically crisp ╱         
║▮║ │        ╲──────────────────────────────────────────────────╱           
╚═╝ └─────────────────────────────────────────────────────────────────────────┘
    ▸ CONTEXT: scroll=zoom canvas · 3-finger drag=pan · fist=grab · spread=zoom-out
```
Left telemetry bracket (`TELEMETRY BRACKET`). Fixed 48pt wide, full height,
`plate.charcoal.1` with a `stroke.stencil` right edge. Vertical stencil letters
label it. Bottom cell `▮` is a live vertical load bar. It never scrolls with the
canvas — it is bolted to the frame like a rugged instrument rail.
Global status strip. Top row, 32pt, right-aligned cluster: capability/CPU
`⌗`, battery %, network `▲`, clock, and a 3-cell audio meter `▮▮▮`. Left side
carries the system wordmark in `accent.cyan`.
Floating application panels. Borderless, background `bg.obsidian.1` at 82%
through the glass pass. Corners are cut at 45° via a clip-path polygon (a 14pt
chamfer on each corner), never a border-radius. A 1px `accent.cyan` edge lights
only on the focused panel; unfocused panels drop to `stroke.hairline`.
Time-Stream sits upper-left of the canvas center; Comm-Deck upper-right —
deliberately unequal spacing (the canvas breathes asymmetrically). Both are
ordinary canvas bodies and can be flung, but spawn docked to these anchor zones.
Gesture crosshair. A `track.amber` `✛` at optical center with a soft bloom
halo (it is a bright-pass contributor in the CRT shader). It is the reticle the
camera gesture engine drives; when a hand is tracked it leaves a 6-frame amber
motion trail. When no hand is present it dims to `amber.font.dim` and shrinks 20%.
---
SECTION 3 — Deep Component Specifications
3.1 The System Telemetry Monitor
Blocky tactical widget. Fixed 300×184pt. Theme B shown (Industrial Cathode):
`plate.charcoal.1` body, `stroke.stencil` brackets, `green.radioactive` live
meters, `orange.hazard` when any bar crosses threshold. Every value is wrapped in
technical brackets and reported as a percentage or explicit unit.
```
┌─[ SYS.TELEMETRY ]──────────────────────[ LIVE ]─┐
│                                                  │
│  [ WEBCAM.GESTURE ]     NODE ▸ ACTIVE      98%  │
│  ▸device: cam0   route: sys.gesture   21-pt lock │
│  ├████████████████████████████████████░░░┤ conf │
│                                                  │
│  [ MIC.DIRECT ]         PIPE ▸ PASSTHRU    ──   │
│  ▸device: mic0   route: mixer.app    zero-copy   │
│  ├██████████████████░░░░░░░░░░░░░░░░░░░░░░┤ -18dB │
│                                                  │
│  [ GPU.THERMAL ]        CORE ▸ NOMINAL    61°C  │
│  ├███████████████████████████░░░░░░░░░░░░░┤ 61%  │
│  [ GPU.THERMAL ]        HOT  ▸ HAZARD     87°C  │
│  ├████████████████████████████████████▓▓▓┤ 87%  │  ◄ bar turns orange.hazard
│                                                  │
│  [ NPU ]  IDLE·throttled     [ MMIO ] 3 mapped  │
└──[ cap: HW_PASSTHROUGH ✓ ]───────[ slot A ]─────┘
```
Definition notes:
Header brackets `[ SYS.TELEMETRY ]` / `[ LIVE ]` are `stroke.stencil` strokes
with `amber.font` labels; `[ LIVE ]` pulses `green.radioactive` at 0.5 Hz.
Meter bars are fixed 40-cell `├ ┤` gauges. Fill glyph `█` in
`green.radioactive`; when value ≥ threshold the exceeding cells switch to `▓`
in `orange.hazard` and the row's status token flips `NOMINAL → HAZARD`.
Passthrough rows show `device`, `route`, and a mode tag (`zero-copy`,
`21-pt lock`) pulled straight from the kernel's device registry — this widget is
the visual face of `sys_hardware_passthrough`. `MMIO 3 mapped` counts live grants.
Footer shows the holding process's capability check `✓` and the active A/B
root slot, tying the UI to the real kernel state.
3.2 The Time-Stream Node (single file block)
How one file appears on the timeline. Fixed 236×132pt card, 45° chamfered
corners, `bg.obsidian.2` plate (Theme A). Carries a monospaced data-matrix tag
row, an abbreviated SHA-256, and a diagnostic bar.
```
   ╱────────────────────────────────────╲
  ╱  ▣ q3_revenue_chart.png         IMG   ╲
 │  ┌─ data.matrix ──────────────────────┐ │
 │  │ kind:img  proj:#mktg  from:Sarah   │ │
 │  │ 14 Mar 26 · 14:22:07 · 1.24 MB     │ │
 │  └────────────────────────────────────┘ │
 │  sha256 ▸ 868d9e65…a1f4bb   [verified✓] │
 │  ├████████████████████████░░░░░░┤ vec 82%│
  ╲  ◈ semantic-indexed   ◉ restore-ctx   ╱
   ╲────────────────────────────────────╱
```
Definition notes:
Title row: filename in `text.primary`, right-tag (`IMG`/`DOC`/`VEC`/`BIN`)
in `accent.cyan` inside a stencil box.
data.matrix block: monospaced key:value tags the local AI auto-populated —
`kind`, `proj`, `from`, timestamp, size. Never manually entered.
SHA-256 line: first 8 + last 6 hex of the content hash (`868d9e65…a1f4bb`),
abbreviated with an ellipsis, followed by `[verified✓]` in `ok.mint` when the
stored bytes re-hash to the same digest (the CAS integrity check from the
`cas_vfs` subsystem). A mismatch shows `[CORRUPT]` in `alert.magenta`.
Diagnostic bar: `vec 82%` shows embedding-index progress. While the edge AI
is still vectorizing (`vector_id = 0`) the bar animates in `track.amber`; once
indexed it locks solid `accent.cyan` and `semantic-indexed` lights up.
Actions: `◉ restore-ctx` snaps the canvas back to the workspace state
captured when this file entered the Time-Stream.
---
SECTION 4 — Hardware-Accelerated Micro-Animations
All timings are wall-clock milliseconds at 120 Hz (values hold at 60 Hz; the
compositor drives them off a time uniform, not frame counts). Curves are given as
`cubic-bezier` for CSS-side components and as the equivalent WebGPU eased `t`.
4.1 Window open — "glitch-in"
Instantaneous digital materialization: rapid opacity+scale snap with an
intentional RGB split that resolves as it lands.
```css
@keyframes glitch-in {
  0%   { opacity: 0.90; transform: scale(0.90); filter: none; }
  35%  { opacity: 0.96; transform: scale(0.985)
         translateX(1.5px);                       /* channel tear */
         filter: drop-shadow(2px 0 0 #22E4FF)
                 drop-shadow(-2px 0 0 #FF2D9B); }  /* cyan/magenta split */
  70%  { transform: scale(1.006) translateX(-0.5px); }
  100% { opacity: 1; transform: scale(1.0) translateX(0);
         filter: none; }
}
.window-opening {
  animation: glitch-in 45ms cubic-bezier(0.2, 0.9, 0.1, 1.0) both;
  will-change: opacity, transform, filter;
}
```
Duration 45 ms, single shot. The scale runs 90%→100% while opacity runs
90%→100% (the panel is never fully invisible — it snaps in, it doesn't fade).
The mid-frame `drop-shadow` pair is the deliberate color separation: +X cyan,
−X magenta, amplitude 2px, fully gone by 100%.
WebGPU path: a per-window uniform `t01` drives `scale = mix(0.90,1.0,ease(t))`
and a horizontal `chroma_offset = 2px * (1-t) * pulse(t)` fed to the panel's own
aberration sampler, so it matches the CRT look exactly.
4.2 Error state — hazard zebra flash
The panel edge flashes a diagonal zebra of black / glowing amber at 4 Hz
(250 ms period, 125 ms per phase).
```css
@keyframes hazard-zebra {
  0%,49%   { border-image: repeating-linear-gradient(45deg,
             #000000 0 8px, #FFB020 8px 16px) 4;
             box-shadow: 0 0 12px #FFB02066; }
  50%,100% { border-image: repeating-linear-gradient(45deg,
             #FFB020 0 8px, #000000 8px 16px) 4;   /* stripes invert */
             box-shadow: 0 0 20px #FFB020AA; }
}
.window-error {
  border: 4px solid transparent;
  animation: hazard-zebra 250ms steps(1, end) infinite; /* 4 Hz, hard toggle */
}
```
4 Hz = 250 ms period; `steps(1,end)` gives the hard mechanical toggle (no
smooth tween — hazard signage snaps).
Stripe pitch 8px at 45°; the two phases swap which stripe is black vs
`warn.zebra.b` amber, reading as motion along the diagonal.
The `box-shadow` pulse makes the amber "glow" breathe with the toggle. Stops on
acknowledge; the panel eases border back to `stroke.hairline` over 120 ms.
4.3 Close / gesture-fling — matrix dissolve
Flinging a window away (camera fist-throw or 3-finger push) pixelates it into a
stream of horizontal scan-lines in the window's own accent color, which then
race off in the fling direction.
```css
@keyframes matrix-dissolve {
  0%   { opacity: 1;  filter: none;
         clip-path: inset(0 0 0 0); }
  30%  { filter: contrast(1.4) brightness(1.2)
                 url(#lineShatter);          /* SVG displacement -> H-lines */ }
  100% { opacity: 0;
         transform: translateX(var(--fling-x)) skewX(8deg) scaleY(0.4);
         filter: blur(1px) url(#lineShatter);
         clip-path: inset(0 0 0 60%); }       /* trailing edge tears away   */
}
.window-flinging {
  --fling-x: 480px;              /* set to the gesture velocity vector      */
  animation: matrix-dissolve 220ms cubic-bezier(0.4, 0.0, 0.9, 0.3) forwards;
}
```
WebGPU implementation (the real path; CSS above is the fallback for DOM apps):
```
1. Snapshot the window into an offscreen texture T.
2. Fragment shader over T, param t01:
     • quantize uv.y into N horizontal bands (N = 48 * (1 - t)) -> chunky lines
     • per-band horizontal offset = hash(band) * t * fling_dir * 40px
     • per-band alpha = step(t, hash(band))  -> bands wink out staggered
     • tint surviving bands toward the window's accent (accent.cyan or the
       app's declared accent) and add to the bloom bright-pass so they streak
3. Bands accelerate along fling_dir; last band gone at t=1 (220 ms).
```
Duration 220 ms. The window shears into horizontal matrix lines colored by
its primary accent, each line staggered so it reads as a dissolve into a data
stream rather than a uniform fade.
`--fling-x` (and a `--fling-y`) are driven by the actual gesture velocity from
the 21-point hand tracker, so a hard throw scatters the lines farther and
faster than a gentle push.
---
Cross-reference to running code
§3.1 telemetry / passthrough count reflects `sys_hardware_passthrough` in
the bare-metal kernel (`metal/kernel/kernel64.c`) — the widget's "MMIO N mapped"
and "cap: HW_PASSTHROUGH ✓" are exactly the kernel's grant path, now backed by
real 4-level page tables (see `docs/metal-passthrough-boot.log`).
§3.2 SHA / [verified✓] reflects the CAS integrity guarantee in
`subsystems/cas_vfs.rs` (content re-hash on read).
§4 motion parameters feed the same bloom bright-pass and aberration sampler
defined in Section 1's CRT shader, so widgets and window motion share one
optical model rather than bolting on separate effects.

Outrun OS v0.4 — what changed
Kernel 0.4.0-metal — genuine ring-3 user mode
The capability passthrough is now a REAL trap from an unprivileged process.
New `metal/boot/usermode.asm`:
`syscall_entry` — SYSCALL lands here in ring 0, swaps to a dedicated kernel
stack, marshals args, calls the C dispatcher, and SYSRETs back to ring 3.
`enter_user_mode` — builds an iretq frame and drops to ring 3 for the first
time, after saving a kernel resume point.
`resume_kernel` — longjmp back into kernel_main from SYS_EXIT.
`user_blob` — a self-contained, position-independent ring-3 program that
reaches the kernel ONLY through `syscall`.
`metal/kernel/kernel64.c`:
`wrmsr`/`rdmsr`; a 64-bit GDT with user code/data segments + a TSS (rsp0 for
ring3->ring0 traps); `usermode_init()` arms EFER.SCE, STAR, LSTAR, SFMASK.
`syscall_dispatch()` implementing SYS_WRITE / SYS_HW_PASSTHROUGH /
SYS_EXIT / SYS_WRITEHEX.
`run_ring3()` maps the user program + stack USER-accessible and enters ring 3.
Proof (QEMU, BIOS + UEFI, see docs/ring3-boot.log): stream-app (caps 0x3)
drops to ring 3, issues a SYSCALL, the capability bitset is checked inside the
trap, the device is GRANTED (mapped at 0x4000c0000000), SYSRET carries the
result back, SYS_EXIT returns to the kernel. sketchy-app (caps 0x20) runs the
same SYSCALL and is DENIED (-2). One privilege boundary, two outcomes, decided
in the trap.
Compositor prototype — compositor/outrun-compositor.html
A running single-file compositor for the Metropolis-Terminal blueprint:
Offscreen 2D scene: infinite grid, physics windows (spring k=220, zeta=0.82,
mass-weighted collision separation), telemetry monitor, Time-Stream node,
Comm-Deck, video editor, and the amber gesture crosshair with a motion trail.
CRT-Glass-Glow post pass with three backends tried in order: WebGPU (WGSL) ->
WebGL2 (GLSL) -> raw Canvas2D. Same shader math in each: 1.5% barrel curvature,
radial chromatic aberration, ~4% scanlines, 5-tap bloom bright-pass, vignette.
Micro-animations from the spec: 45ms glitch-in (cyan/magenta split), 4Hz hazard
zebra error border, 220ms matrix-line dissolve on fling.
Accelerator HUD (Ctrl/Cmd+K: new / error / fling / theme / resize), A/B theme
toggle, drag-to-fling physics. The active renderer + theme are shown top-right.

Outrun OS v0.5 — what changed
Two things that were previously stubbed are now real: the device behind the
capability syscall, and the origin of the user program.
1. A real virtio device behind sys_hardware_passthrough
New PCI enumeration over configuration mechanism #1 (ports 0xCF8/0xCFC):
walks bus 0, prints every function's vendor/device/class.
Finds the virtio device (vendor 0x1AF4), parses its VIRTIO-modern vendor
capability list to locate the COMMON_CFG and DEVICE_CFG structures, reads and
sizes the memory BAR that carries them, and enables PCI memory-space decode.
Registers that REAL physical MMIO window as a capability-gated device
('virtio-net', requires CAP_NETWORK). The same sys_hardware_passthrough that
used to hand out a scratch page now installs PTEs over actual device registers.
Proof (docs/virtio-elf-boot.log): the authorized ring-3 process maps the BAR
and reads the virtio-net MAC directly out of the device-config registers. With
`mac=52:54:00:ab:cd:ef` on the QEMU command line, the unprivileged program
reads back exactly 52:54:00:ab:cd:ef — through its own page tables. Works with
the BAR in the 32-bit hole (BIOS, phys 0xFE000000) and as a 64-bit BAR above
4 GiB (UEFI, phys 0xC000000000). Unauthorized process still gets -2, unmapped.
If QEMU is booted without a virtio device, the kernel falls back to the scratch
sentinel so the demo still runs.
2. The user program is a real ELF loaded from the ISO
New freestanding userland program `user/init.c`, linked at the user virtual
base (0x500000000000) as a standalone ELF64 with no libc and no kernel linkage
— its only interface is the `syscall` instruction.
Shipped on the ISO and loaded by GRUB as a Multiboot2 module (`module2` in
grub.cfg). The kernel reads the module tag, then a real ELF64 loader parses the
program headers and maps each PT_LOAD segment at its p_vaddr with PTE_USER
(read-only segments stay read-only), sets up a user stack, and jumps to e_entry.
Each process runs in its OWN address space now (CR3 switched on entry, restored
on SYS_EXIT), so a granted MMIO mapping is actually visible to the unprivileged
code that requested it.
The embedded blob remains only as a fallback if no module is present on the ISO.
syscall ABI fix
`syscall_entry` now preserves the full set of caller-saved argument registers
(rdi, rsi, rdx, r8, r9, r10) in addition to rcx/r11, matching the syscall ABI
the C compiler assumes for userspace. (Without this, values held across a
syscall in userspace were being corrupted.)
Build / run
`make` builds the kernel, the user ELF, and the hybrid ISO (now 0.5.0).
`make qemu` boots headless with `-device virtio-net-pci` attached.

Outrun OS v0.6 — Phase 1: real disk I/O (virtio-blk)
The kernel can now read and write real disk sectors over a virtio-blk PCI
device, instead of depending only on GRUB-loaded RAM modules.
The virtio-blk driver (modern virtio 1.0, split virtqueue)
PCI discovery: dispatches virtio devices by class — mass storage (0x01) to the
new block driver, network (0x02) to the existing passthrough probe.
Maps the device's MMIO BAR into kernel space (uncached PTEs) and parses the
virtio vendor capabilities for the common-config, notify, and device-config
structures (incl. the notify offset multiplier).
Enables PCI memory-space decode AND bus-mastering (the device DMAs).
Full status handshake: RESET -> ACKNOWLEDGE -> DRIVER -> feature negotiation
(requires VIRTIO_F_VERSION_1) -> FEATURES_OK (verified) -> DRIVER_OK.
Reads the capacity from device-config.
Builds the split virtqueue by hand: descriptor table, avail ring, and used
ring in DMA-visible physical memory, wired via queue_desc/queue_driver/
queue_device, then queue_enable.
Requests are three chained descriptors (16-byte header, 512-byte data,
1-byte status). Submits on the avail ring, kicks the notify register, and
BLOCKS by polling the used ring, then checks the status byte.
Public API
`int virtio_read_block(uint64_t sector, void *buffer);`
`int virtio_write_block(uint64_t sector, const void *buffer);`
Both return 0 on success, negative on error/timeout.
Proof (docs/virtio-blk-boot.log)
Booting with a 4 MiB virtio-blk disk that has a signature at sector 2:
discovery: `vendor 1af4 device 1042 class 100` (modern virtio-blk, mass storage)
`capacity: 8192 sectors (4 MiB)` read from device-config
`read sector 2 -> "OUTRUN-DISK-SIGNATURE-OK"` — reading real pre-existing
on-disk data
`wrote + read back sector 0 ... verify MATCH` — full write/DMA/read round-trip
The write is durable: after the VM exits, sector 0 of the host image contains
"OUTRUN-WROTE-THIS-SECTOR..." — a genuine DMA write to physical storage.
Verified under both SeaBIOS (BAR at 0xFE000000) and OVMF/UEFI. Falls back
gracefully with a clear message when no virtio-blk device is attached.
Build / run
`make qemu` now auto-creates a signed test disk and attaches it as virtio-blk.
Shell command `disk` re-runs the read/write round-trip on demand.
Also staged (not yet linked): polyglot components
`rust/cap_engine.rs` — a no_core Rust translation unit (FNV-1a CAS hash +
capability check) that compiles to real machine code with this toolchain.
`cpp/ipc_ring.cpp` — a freestanding C++ SPSC ring buffer with an extern "C"
surface. These are the beginnings of the unified C/C++/Rust/asm boot image.

Outrun OS v0.7 — Phase 1: CAS storage layer + polyglot boot image
Two things land together: a persistent content-addressable store on top of the
virtio-blk primitives, and the first genuinely unified C/C++/Rust/asm boot image.
Content-Addressable Storage (CAS) on virtio-blk
A block's address IS the hash of its content; identical content is stored once.
On-disk layout (persisted):
block 0        superblock ("ORUNCAS1", geometry, counters)
bitmap_start.. allocation bitmap (1 bit/block)
index_start..  open-addressed hash index {content-hash -> block, len}
data_start..   content blocks
Operations:
cas_format / cas_mount — lay out or re-open a volume; the superblock and
bitmap persist and are re-read on the next boot.
cas_put(data,len) -> hash — hashes the content (via Rust), looks it up; on a
hit it DEDUPLICATES (no allocation, no write); on a miss it allocates a block,
writes it, and inserts the index entry. All metadata flushed to disk.
cas_get(hash,out,max) -> len — resolves hash -> block and reads it back.
Proof (docs/cas-boot.log), two boots against the same disk image:
BOOT 1 (fresh): formats 8192 blocks; stores two distinct blobs at blocks 19
and 20; the third put (identical content) DEDUPs to block 19. Blocks used +2.
BOOT 2 (same disk): `mounted existing volume: 21/8192 blocks used, 3 puts, 1 dedup hits` — the superblock and counters survived the reboot. Now ALL three
puts dedup to the blocks written in boot 1 (blocks used +0), and cas_get reads
the original content back off the disk.
On the host, sector 0 shows the `ORUNCAS1` magic and blocks 19/20 hold the
literal stored strings — durable outside the VM.
Unified polyglot boot image (C + C++ + Rust + assembly)
The staged polyglot objects are now linked into the actual kernel:
rust/cap_engine.rs (no_core Rust, x86_64-unknown-none) provides:
rust_cas_hash() — the FNV-1a content hash the CAS layer uses for every put.
rust_cap_check() — now the ACTUAL gate inside sys_hardware_passthrough; the
ring-3 capability grant/deny is decided by Rust code (still grants stream-app,
denies sketchy-app in the boot trace).
cpp/ipc_ring.cpp (freestanding C++) — the zero-copy SPSC ring; a boot self-test
pushes/pops through it ("polyglot-ipc"), proving the C++ TU is live.
The Makefile now compiles the Rust object (RUSTC_BOOTSTRAP no_core build) and the
freestanding C++ object and links both into outrun-kernel.elf alongside the C and
NASM objects.
Notes / scope
The destructive `disk` write test was removed from the boot path (it clobbered
sector 0); it remains as a manual shell command for scratch disks.
CAS blocks are 512 bytes; the index is a fixed 512-slot open-addressed table
(fine for the demo, grows later). Single outstanding I/O, polling completion.

Outrun OS v0.8 — interrupt-driven virtio-blk (async backbone)
The block driver no longer spins on the used ring. Disk I/O now completes on a
real hardware interrupt, with a clean top/bottom-half split and a sleeping
waiter — the asynchronous backbone the higher layers will build on.
Interrupt path
Parses the virtio ISR-status capability (cfg_type 3) and maps that register.
Reads the PCI Interrupt Line (config 0x3C): the device is on legacy INTx,
IRQ 11 -> PIC-remapped vector 43. (No MSI-X enabled, so the device uses INTx.)
New PIC unmask helper + a per-IRQ handler table; isr_dispatch now routes
device IRQs (vectors 34..47) to the registered handler.
Top / bottom-half split
TOP HALF (interrupt context, minimal): read the ISR-status register — which
de-asserts the INTx line — and, if it's a queue interrupt, flag the bottom
half. Then EOI. Nothing heavy runs at interrupt time.
BOTTOM HALF (thread context, IRQs on): drain the used ring, capture the
request status, and mark the request complete (wake the waiter).
Sleeping waiter (poll retired)
virtio_blk_request() arms the completion flags, publishes on the avail ring,
kicks the notify register, then BLOCKS BY SLEEPING: `sti; hlt` halts the CPU
until an interrupt arrives. The cli/check/sti-hlt idiom is race-free (an IRQ
between the check and the halt is not lost). The 100 Hz timer provides a
bounded fallback so a mis-routed INTx can never hang the kernel.
Proof (docs/interrupt-driven-boot.log)
`interrupt-driven: INTx on IRQ 11 (vector 43), ISR reg mapped`
After the CAS format/put/get workload: `I/O completions delivered by INTERRUPT: 40 (timer-fallback: 0)` — every completion came from a hardware
IRQ; the fallback never fired. On a subsequent (mount-only) boot: 17, again
all by interrupt. Verified under both SeaBIOS and OVMF/UEFI, and CAS
persistence/dedup across boots is unchanged.
Why this matters
Execution no longer locks up waiting on storage — the basis for async I/O.
The top/bottom-half split is the hook a real scheduler will use to wake
sleeping threads on hardware events.
The block layer is now efficient enough to sit under a scalable index or an
immutable file namespace without a polling bottleneck.

Outrun OS v0.9 — Phase 2: preemptive scheduler + multiple outstanding I/O
The kernel now runs multiple threads with a real PCB, time-slices them off the
PIT, and keeps many disk requests in flight at once — the bottom half wakes the
specific thread that owns each completed tag.
Scheduler (boot/switch.asm + kernel)
Per-thread PCB: saved RSP, address space (cr3), state, wait tag, entry/arg,
and a dedicated 16 KiB kernel stack. (Kernel threads share kernel_cr3; the
cr3 field is the hook for ring-3 threads later.)
switch_context (assembly): saves callee-saved regs + RFLAGS, swaps stacks, so
the interrupt-enable state travels with each thread.
Round-robin pick with an idle thread (halts when nothing else is runnable).
Cooperative sched_yield() for voluntary switches; PIT-driven sched_preempt()
for time slicing, gated by a preempt-disable count so critical sections and
the ring-3 excursion are never interrupted mid-flight.
Multiple outstanding I/O (virtio-blk reworked)
The single-request path is replaced with a slot table (one slot per in-flight
request, 3 descriptors each — up to 21 concurrent on a 64-entry queue).
vblk_submit() publishes a request and returns a tag WITHOUT blocking;
vblk_wait(tag) parks the calling THREAD (not the CPU) until its tag completes.
The bottom half drains ALL completed used-ring entries per interrupt and wakes
each owning thread by TID.
Proof (docs/scheduler-boot.log)
`submitted 8 reads without blocking; in-flight now 8` ->
`all 8 done; PEAK concurrent in-flight tags = 8` (deterministic on BIOS + UEFI).
4 worker threads each run 4 write+read cycles concurrently, all verified;
completions arrive out of submission order and wakes route to the right TID.
Preemption proof: a CPU-bound thread that never yields still advances ~40M
increments while main also holds the CPU in its own non-yielding loop — only
the timer could interleave them. (0 would mean no time-slicing.)
CAS persistence/dedup and the polyglot C/C++/Rust/asm image are unchanged;
the ring-3 SYSCALL passthrough path still works.
Scope / next
Cooperative + on-demand preemption (enabled around the concurrency demo).
Ring-3 threads and full always-on preemption with per-CPU run queues come with
the VFS/user-thread work. Single virtqueue; 512-byte requests.

Outrun OS v0.10 — Phase 2: Virtual File System over CAS
Named, read/write files layered on the content-addressable store, with
capability-gated file syscalls for ring-3 and an exec that loads ELFs straight
from storage instead of the GRUB module.
VFS layer
On-disk directory region added to the CAS volume (superblock v2): a table of
dirents {name, len, per-512B-block content hashes, whole-file hash}.
A file is a name + an ordered list of block hashes; blocks live in CAS, so:
files span multiple blocks (readme = 1400 bytes across 3 blocks, verified);
identical content is stored once (a duplicate file adds 0 new blocks);
writes are copy-on-write (new content -> new hashes -> new file_hash; the
name simply repoints; old blocks are immutable).
Directory persists: flushed on change, reloaded on mount.
Capability-gated file syscalls (ring-3)
Extended the syscall ABI to 3 user args (a2 in RDX) so read/write can carry
(fd, buf, len).
New syscalls, each gated by CAP_FILESYSTEM via the Rust cap engine:
sys_open(5), sys_read(6), sys_write(7), sys_close(8).
Orthogonal capabilities demonstrated at ring 3 from the same ELF:
stream-app (HW_PASSTHROUGH): reads virtio MMIO, file access DENIED.
sketchy-app (FILESYSTEM): passthrough DENIED, reads a file from the VFS.
sys_exec from storage
exec_from_cas(name): reads an ELF's bytes back out of CAS (by VFS name ->
content hashes -> blocks) into a buffer, then runs the existing ELF loader —
no GRUB module involved.
Proof: the kernel stores the user ELF into the VFS as "init", then a third
process is loaded from CAS ("read 5624 ELF bytes from CAS ... not a GRUB
module") and runs at ring 3, exercising BOTH hardware passthrough (real
virtio-net MAC 52:54:00:ab:cd:ef) and a VFS file read.
Proof (docs/vfs-boot.log)
Multi-block store + read-verify, copy-on-write hash change, cross-file dedup
(+0 blocks), per-process capability grant/deny at ring 3, and exec-from-CAS.
Verified on BIOS + UEFI; CAS persistence and the preemptive scheduler are
unchanged; the system reaches the shell.
Scope / next
Files up to 8 KiB (16 block-hashes per dirent), 16 files, single directory —
all natural to grow into a multi-level index later. Read/write are whole-file
(offset tracking is stubbed). Ring-3 I/O runs on the main thread's context;
turning ring-3 processes into first-class scheduler threads is the next step.

Outrun OS v0.11 — Phase 3: async IRQ routing + virtio-net + zero-copy parse
The network card now raises real interrupts that the kernel routes to a specific
blocked thread, and a zero-copy parser reads frames straight out of the RX ring.
virtio-net driver (modern virtio 1.0)
Full bring-up: PCI cap parse (common/notify/isr/device), MMIO map, bus-master,
reset -> ACK -> DRIVER -> feature negotiation (VIRTIO_NET_F_MAC + VERSION_1) ->
FEATURES_OK -> DRIVER_OK. MAC read from device-config.
RX and TX split virtqueues; 16 receive buffers posted; the device is also
registered for ring-3 passthrough (so the usermode MAC-read demo still works).
Async interrupt routing (Ring-0 -> blocked thread)
PCI INTx is shared, so the IRQ table now holds a handler chain per line; each
device's top half checks its own ISR-status register. (This also fixed a clash
where virtio-blk and virtio-net share IRQ 11.)
TOP HALF: read the NIC ISR (de-asserts INTx), flag the softirq.
BOTTOM HALF: drain the RX used ring and WAKE the exact thread parked on
NET_RX (by TID) — a hardware interrupt transitions the owning thread from
BLOCKED to RUNNABLE.
sys_wait_event(NET_RX): capability-gated (CAP_NETWORK) syscall / kernel call
that parks the caller until the NIC interrupts. Also exposed as syscall #9.
Zero-copy packet parse (Phase 3 item 2)
The demo `netd` thread (holding CAP_NETWORK) transmits an ARP request for the
gateway, then blocks on sys_wait_event. When QEMU's SLIRP replies, the RX IRQ
wakes netd, which parses the Ethernet/ARP frame IN PLACE (pointers into the RX
buffer, no memcpy) and prints the gateway's MAC.
Proof (docs/net-boot.log)
`[vnet] DRIVER_OK — rxq 16 txq 16, INTx IRQ 11 — READY`
`[netd] tid2 holds CAP_NETWORK; TX ARP who-has 10.0.2.2`
`[netd] sys_wait_event(NET_RX): parking until the NIC interrupts...`
`[netd] woken by IRQ; RX 76 bytes, ethertype 806`
`[netd] ARP reply: 10.0.2.2 is-at 52:55:0a:00:02:02 (parsed in place)`
The full boot (CAS persistence, scheduler, VFS, exec-from-CAS, ring-3) is
unchanged and reaches the shell. Both virtio-blk and virtio-net operate on the
shared IRQ line simultaneously.
Scope / next
ARP-level round trip proves the interrupt/wake path; the IP/UDP router and a
full RX buffer-recycle loop are the next build. The waiter is demonstrated with
a kernel thread; wiring sys_wait_event to a blocking ring-3 process (process as
a first-class scheduler thread) is the follow-on.

Outrun OS v0.12 — Phase 3 complete: zero-copy IP/UDP packet router
Builds on the async RX path (v0.11) with a freestanding IP/UDP router that parses
frames in place and routes datagrams to bound execution slots by port — proven
with a real DHCP exchange against QEMU's SLIRP.
Zero-copy IP/UDP router
Parses Ethernet -> IPv4 -> UDP entirely IN PLACE: pointers into the RX buffer,
no memcpy. Validates IPv4 version, walks the IHL, checks protocol 17 (UDP),
and extracts src/dst ports, length, and a payload pointer.
Port filter array (NUDP slots): udp_bind(port, tid) registers a UDP dst port
and the execution slot to route matches to.
On a match, the router hands the payload POINTER to the owning slot and wakes
that thread — the datagram is delivered without ever being copied.
Runs in the RX bottom half (net_dispatch): ARP frames go to the ARP waiter,
IPv4 frames go to the UDP router.
Proof (docs/udp-router-boot.log) — real DHCP round-trip
`[dhcpd] bound UDP port 68 -> execution slot 0`
The kernel builds a valid DHCPDISCOVER (Ethernet broadcast / IPv4 with a
computed header checksum / UDP 68->67 / BOOTP with magic cookie + options) and
transmits it, then parks on the port-68 slot.
SLIRP's DHCP server replies; the RX IRQ drives the router, which matches dst
port 68 and routes the payload pointer to slot 0, waking dhcpd.
`[dhcpd] router delivered 548-byte UDP payload (zero-copy pointer)`
`[dhcpd] DHCP OFFER: offered IP 10.0.2.15, server 10.0.2.2` — parsed in place
from the routed pointer (yiaddr + option 53/54).
Phase 3 items 1 (async IRQ routing + sys_wait_event) and 2 (zero-copy IP/UDP
router) are both demonstrated end-to-end. The full boot (CAS, scheduler, VFS,
exec-from-CAS, ARP + DHCP) is stable and reaches the shell; virtio-blk and
virtio-net share IRQ 11 without conflict.
Helpers added
ip_checksum() (one's-complement IPv4 header checksum).
udp_bind / net_wait_slot (park a thread on a bound port).
A DHCP client builder + in-place option walker.
Scope / next (Phase 4)
Single outstanding round-trip per demo; RX buffer recycling and a TX
completion reclaim loop are natural hardening steps. Phase 4 is the graphics
server: VBE/GOP framebuffer + sys_map_framebuffer + the Metropolis-Terminal
compositor on bare metal.

Outrun OS v0.13 — Phase 4: framebuffer graphics + Metropolis-Terminal compositor
The kernel now drives a real linear framebuffer and renders the Metropolis-
Terminal visual language to actual pixels, with a capability-gated zero-copy
framebuffer mapping for a ring-3 graphics manager.
Framebuffer bring-up
Added a Multiboot2 framebuffer request tag to the header; GRUB sets a
1024x768x32 linear framebuffer (VBE under BIOS, GOP under UEFI) and passes it
in the framebuffer info tag, which the kernel parses (addr/pitch/w/h/bpp).
Maps the framebuffer (phys 0xFD000000 under QEMU stdvga) into kernel space and
allocates a RAM backbuffer: all rendering is DOUBLE-BUFFERED (render to the
backbuffer, then flip to the hardware framebuffer).
Graphics engine (integer / fixed-point — no SSE/FPU)
Pixel plot, rectangle, lines, and an alpha-blend primitive (glass diffusion).
Chamfered glass panels: two opposite 45-degree cut corners, translucent
gradient fill, an accent header band, and a glowing neon edge.
Telemetry meter bars, an infinite-canvas grid, a left telemetry bracket, an
amber gesture crosshair with glow, and horizontal scanlines (every other row
darkened) — the CRT look, in software.
Window physics in 16.16 fixed-point: spring toward targets (zeta 0.82) plus
mass-weighted AABB collision separation, settled over 480 steps so the panels
push each other apart into a non-overlapping layout instead of stacking.
sys_map_framebuffer (zero-copy, capability-gated)
New PCAP_FRAMEBUFFER capability and syscall #10: maps the linear framebuffer
straight into a process's address space (user vaddr 0x550000000000) so a
ring-3 graphics manager can write pixels directly — no copy.
Proven end-to-end: the CAS-loaded ring-3 process (granted CAP_FRAMEBUFFER)
maps 3 MiB of framebuffer and draws a marker directly onto the display, on top
of the kernel compositor; processes without the capability are cleanly denied.
Proof (docs/compositor-render.png)
An actual QEMU screendump of the rendered display: obsidian canvas with grid,
five chamfered glass panels with cyan/magenta/amber neon edges settled by the
physics loop, telemetry bracket, gesture crosshair, scanlines — plus the amber
marker square drawn by the ring-3 graphics manager (top-right). Verified by
pixel read-back on the serial console and by analyzing the screenshot.
Scope / next (Phase 4 cont. / Phase 5)
Software renderer with a static settled frame; an animated compositor loop and
a text/font layer are natural extensions. The ring-3 wm draws a marker today;
a full ring-3 compositor using the mapped framebuffer is the follow-on, leading
into Phase 5 (Time-Stream workspace + production validation).

Outrun OS v0.14 — Phase 4 complete: animated compositor + text layer
Turns the static Metropolis-Terminal render into a live animation loop and adds a
bitmap font, so panels carry real titles and the desktop assembles itself on screen.
Text / font layer
Embedded 8x8 bitmap font (font8x8 subset, ASCII 0x20-0x5F). draw_char / draw_str
render directly into the backbuffer.
The compositor now labels everything: an "OUTRUN // METROPOLIS-TERMINAL"
wordmark and per-panel titles (TIME-STREAM, COMM-DECK, SYS.TELEMETRY,
TIME-NODE, VIDEO-EDITOR).
Animated compositor loop
Persistent window state stepped once per frame: spring toward targets
(zeta 0.82) plus mass-weighted AABB collision, so the panels start clustered
in the center and spring/scatter out into a settled, non-overlapping layout.
Glitch-in reveal: cyan/magenta chromatic ghost edges on the panels that fade
out over the first 16 frames.
An amber gesture crosshair that orbits, and the scanline filter, every frame.
cmd_gfx renders 150 double-buffered frames paced off the 100 Hz PIT (~20 ms
each), flipping each to the hardware framebuffer, then leaves the settled frame.
Proof (docs/)
compositor-v0.14.png — the settled desktop: obsidian canvas, five labeled
chamfered glass panels with neon edges, telemetry, crosshair, scanlines, and
the ring-3 graphics-manager marker (top-right).
compositor-animation.png — early frame vs settled frame side by side;
~90,000 pixels change between them, captured from two live screendumps, which
is the animation running.
Text verified by sampling (wordmark + subtitle pixels present); the system
still reaches the shell after the animation, and the CAS-loaded ring-3 process
still maps the framebuffer and draws its marker.
Scope / next
Software renderer at 1024x768x32, full-screen redraw per frame. Phase 4's
visual identity is now complete on bare metal; next is Phase 5 (the Time-Stream
workspace + Comm-Deck) and production hardening (ELF bounds-checks, a
capability audit across syscalls, CAS round-trip tests, a dd-able install ISO).

Outrun OS v0.15 — Phase 5a: the Time-Stream Engine
An append-only chronological event substrate with a background vector indexer and
terminal query — files, messages, and system activity on one searchable timeline
instead of nested folders.
Event substrate
struct ts_event: monotonic seq + tick timestamp, type (FILE / COMM / SYS),
source, text, and a 64-dim feature-hashed vector.
ts_emit() enqueues events; safe to call from anywhere, before the indexer runs.
Hooks (real activity, automatically captured)
File-write hook inside vfs_write_file: every VFS write emits a FILE event. The
timeline shows the actual writes from the storage demos (motd, readme, the
copy-on-write rewrite, the deduped copy) with no manual logging.
Comm-Deck messages (Sarah / DevTeam / Ops) and system events (netd DHCP, the
scheduler's worker run) are emitted as COMM / SYS events.
Background indexer (uses the Phase 2 scheduler)
A `timestreamd` kernel thread drains the event queue and parses each event into
its vector: words are lowercased, tokenized, and feature-hashed (FNV) into the
64-dim bag-of-words vector. Asynchronous substrate, real background thread.
Local vector query (terminal)
ts_query() vectorizes the query the same way and ranks every indexed event by
vector dot-product, returning the top matches. Demonstrated:
"the Q3 chart Sarah sent"  -> COMM Sarah: Q3 revenue chart deck (top)
"scheduler patch"          -> COMM DevTeam: microkernel scheduler patch
"dhcp gateway address"     -> Ops gateway/DHCP + netd DHCP offer
`stream` shell command reprints the timeline and runs a query on demand.
Proof (docs/timestream-boot.log)
The chronological timeline of 9 events (4 real file writes + 3 comm + 2 sys),
all vector-indexed by the background thread, and the three ranked queries. Full
boot (CAS, scheduler, VFS, net, DHCP router, compositor, ring-3) is unchanged and
reaches the shell.
Scope / next (Phase 5b)
In-memory index (64 events, 64-dim); persistence to CAS and larger indices are
natural extensions. Next is production hardening: strict ELF segment
bounds-checking, a capability audit across every syscall entry point, an
automated CAS round-trip integrity suite, and a dd-able install ISO for NVMe.

Outrun OS v0.16 — Phase 5 complete: production hardening + NVMe install image
Adds the production validation matrix and a dd-able, UEFI-bootable disk image that
installs the OS onto a raw NVMe drive.
ELF loader hardening (strict segment bounds-checking)
elf_load now takes the image size and validates everything before mapping:
header fits, program-header table in bounds, entry inside the user range, and
per-segment checks (file offset+filesz within the image, memsz >= filesz,
segment <= 16 MiB, vaddr+memsz inside the user range with no overflow).
Hostile or truncated images are rejected cleanly instead of faulting.
Capability audit across syscall entry points
Automated check that every gated syscall enforces its capability: sys_open /
sys_read / sys_write require CAP_FILESYSTEM, sys_wait_event requires
CAP_NETWORK, sys_map_framebuffer requires CAP_FRAMEBUFFER — each denied for a
process lacking the bit and allowed for one holding it.
Automated integrity suite (`validate` command, runs at boot)
19 checks, all passing:
6 capability-audit checks (5 denials + 1 grant),
7 ELF bounds checks (1 valid accepted + 6 hostile images rejected),
6 CAS/VFS integrity checks (byte-exact put/get round-trip, dedup gives same
hash + no new block, unknown-hash lookup rejected, multi-block VFS
write/read round-trip, copy-on-write changes the file hash).
Prints "ALL CHECKS PASSED - image is production-verified".
dd-able NVMe install image (UEFI)
New `make install-img` (mkinstall.sh + mkgpt.py) builds build/outrun-install.img:
a GPT disk with a protective MBR, primary+backup GPT headers, and an EFI System
Partition (FAT) holding a standalone GRUB EFI (BOOTX64.EFI, GOP framebuffer
negotiated), the kernel, and the ring-3 ELF.
Install to a real drive with:   dd if=outrun-install.img of=/dev/nvme0n1 bs=4M
Verified booting as an emulated NVMe drive under UEFI (OVMF): GRUB multiboot2-
loads the kernel, the framebuffer comes up (1024x768x32), DHCP completes, the
validation matrix reports 19/19, the Metropolis-Terminal compositor animates,
the ring-3 graphics manager maps the framebuffer, and the shell is reached.
(Screenshot: docs/nvme-boot-compositor.png.)
Proof
docs/validation-boot.log — the full 19/19 validation run.
docs/nvme-boot-compositor.png — the desktop rendered from the NVMe/UEFI boot.
Status
Phases 2-5 of the blueprint are implemented and verified on bare metal:
preemptive scheduler + concurrent I/O, VFS + exec-from-CAS, async IRQ routing +
zero-copy IP/UDP router, framebuffer + Metropolis-Terminal compositor, the
Time-Stream engine, and now production validation + an installable NVMe image.

Outrun OS v0.17 — core security boundary hardening (exhaustive invariants)
Deepens validation from spot-checks to exhaustive coverage of the user/kernel
boundary, plus the hardening fixes that coverage surfaced. This locks the
isolation guarantees down before any virtualization layer is considered.
Hardening fixes
sys_hardware_passthrough dispatch now REJECTS out-of-range device handles
(previously it silently fell back to a default device); the explicit 0xFFFF
"default device" sentinel is preserved.
Process table raised (MAX_KPROC) so the validation harness cannot exhaust it.
Exhaustive isolation & hardening invariants (`invariants` command)
21 checks, all passing, run at boot:
Capability isolation (the escalation invariant):
For every gated syscall, the ENTIRE 64-entry capability space is swept; any
mask lacking the required bit MUST be denied. 192/192 denial checks held —
proving no combination of other capabilities can escalate into a gated
operation.
Cross-process address-space isolation:
The same virtual address resolves to each process's OWN physical frame;
processes get physically distinct frames; a page mapped only in A is not
present in B's address space at all.
Kernel/user separation at the PTE level:
User pages carry PRESENT + USER + WRITE; the kernel's own pages are reachable
through the shared identity map but are SUPERVISOR-only (no USER bit), so
ring 3 cannot touch kernel memory. Processes share exactly the identity map
(PML4[0]) and the device-MMIO window (PML4[0xC0]) and keep a PRIVATE user
region (PML4[0xA0]).
Passthrough hardening (two-level capability gate):
Invalid device handles rejected; a device is granted only with BOTH
CAP_HW_PASSTHROUGH and the device-specific capability; the granted MMIO page
is USER + cache-disabled and is absent from any unrelated address space.
Register-preservation self-test (ring 3)
The ring-3 program loads sentinels into all callee-saved registers
(rbx, r12-r15), crosses the real SYSCALL/SYSRET boundary, and verifies they
survive — "callee-saved regs survive SYSCALL: PASS" for every ring-3 process.
Proof (docs/invariants-boot.log)
The full 21/21 invariant run + 192/192 capability sweep + register-preservation
PASS. The prior suites (19/19 production validation) and the whole system
(scheduler, VFS, net, DHCP router, compositor, Time-Stream) remain green and
the system reaches the shell. Install image rebuilt.
Status
Core security boundary is now exhaustively verified: capability isolation over
the whole capability space, cross-process memory isolation, kernel/user PTE
separation, passthrough two-level gating, and register preservation across the
ring boundary. This is the stable foundation to build on.

Outrun OS v0.18 — stress hardening: W^X/NX enforcement + fault injection + TLB/CR3 stress
Pushes invariant coverage from isolation into enforcement and stress: real NX +
W^X across kernel and user, hardware-trap-verified, plus fault injection and
TLB/CR3 stress under preemption. (Uniprocessor: cross-core TLB shootdowns/IPIs
do not apply; single-core equivalents are exercised instead.)
New hardening (not just tests)
NX enabled (EFER.NXE) + PTE_NX; CR0.WP enabled so ring 0 respects read-only
pages (kernel code is now immutable).
W^X enforced everywhere:
Kernel: the identity map is remapped so [_stext,_etext) is R+X and every
other page (data, stacks, page tables, heap) is RW+NX. The 2 MiB huge page
covering the kernel image is split to 4 KiB for exact code/data separation.
User: the ELF loader marks data/rodata/stack NX and code R+X, and REJECTS
any writable+executable segment.
Device MMIO and the framebuffer (kernel and ring-3 mappings) are now NX —
this fixed 8 kernel pages that were writable+executable.
Stress & enforcement suite (`stress` command) — 8/8 passing
W^X audit: scans every leaf page in an address space; kernel AND user report
ZERO writable+executable pages (1031 kernel pages scanned, 0 W+X).
NX poisoning: executing a data page traps (#PF) — caught by a recoverable
fault hook, confirming NX fires in hardware.
Write-protect: writing to R+X kernel code traps (#PF) — code is immutable.
TLB staleness: after remapping a vaddr to a new frame + invlpg, the read sees
the NEW frame — no stale TLB entry survives.
CR3 churn under preemption: 20000 address-space switches with the timer
preempting throughout keep the two spaces perfectly isolated.
Page-table auditor: detects a flipped reserved bit in a live PTE (bit-flip /
corruption detection).
Allocator fault injection: forced frame exhaustion returns failure cleanly,
no panic or corruption.
Proof (docs/)
stress-boot.log — the 8/8 stress run + W^X enforcement line.
wx-compositor.png — the compositor still renders correctly through the now-NX
framebuffer mapping.
Full boot remains green: 19/19 validation, 21/21 invariants, 8/8 stress, DHCP,
animated compositor, ring-3 (register preservation + VFS + passthrough), shell.
Honest scope
Single core, so no true cross-core shootdowns. NX/W^X/WP are enforced and
hardware-verified; the auditor catches injected corruption but there is no ECC.
This is the hardened, race-audited base to build the next layer on.

Outrun OS v0.19 — stack hardening: guard pages + stack canaries
Seals the last major ring-3 → kernel corruption vector: stack overflows are now
trapped by guard pages, and stack smashing is caught by compiler-injected
canaries. Two more checks join the stress suite (now 10/10).
Guard pages (ring-3 stacks)
The ring-3 stack is expanded to 16 KiB (4 pages) with an explicit, unmapped
guard page directly below it.
The #PF handler now reads CR2 and, for any fault from ring 3, terminates the
offending task instead of panicking — switching back to the kernel address
space and unwinding via resume_kernel. A fault in the guard-page window is
reported as a stack overflow. A ring-3 stack bomb (push until the stack walks
off the bottom) is trapped exactly at USTK_V-8 and the task is killed with the
kernel untouched.
Any ring-3 excursion now restores interrupts on return (both SYS_EXIT and the
fault-unwind path can land with IF cleared).
Stack canaries (whole kernel)
Built with -fstack-protector-strong: the compiler injects prologue/epilogue
canary save+check into every function with a stack buffer.
__stack_chk_guard is seeded with RDTSC-derived entropy at boot; __stack_chk_fail
halts the core on a smash (or, in test mode, recovers via __builtin_longjmp).
Per-thread canaries: each thread gets its own canary (entropy at creation,
stored in the PCB) and the scheduler saves/restores the active guard across
every context switch, so a canary is valid for the thread that owns the frame
even under heavy preemption. Verified: the preempted worker/spinner threads
never raise a false __stack_chk_fail.
Also hardened
usermode_init (user-segment GDT + TSS + SYSCALL MSRs) now runs before the
stress suite, since ring-3 entry is a prerequisite for the guard-page test.
Stress suite — now 10/10
Adds: (8) ring-3 stack overflow trapped by guard page; (9) stack canary detects
a local buffer overflow. Plus the existing W^X/NX/WP, invlpg staleness, 20000
CR3 switches under preemption, page-table auditor, and allocator fault injection.
Proof (docs/stack-hardening-boot.log)
Full boot green: 19/19 validation, 21/21 invariants, 10/10 stress, DHCP, animated
compositor, ring-3 (now on guarded stacks) with register preservation, and the
shell. Verified on BIOS ISO and the NVMe/UEFI install image.
Next
With stack boundaries sealed, syscall-argument fuzzing is the natural next step;
a periodic descriptor-table sweep can fold into the scheduler tick.

Outrun OS v0.20 — syscall argument fuzzing + user-pointer hardening
Fuzzes the syscall boundary with adversarial arguments and fixes the real
vulnerabilities it surfaces: several syscalls were dereferencing ring-3-supplied
pointers with no validation.
Vulnerability found + fixed (user-pointer validation)
Before this change, SYS_WRITE, SYS_OPEN, SYS_READ and SYS_WRITE_FILE trusted the
pointer/length a ring-3 caller passed. A hostile pointer could fault the kernel
or — worse — make it read or WRITE kernel memory (e.g. SYS_READ with a kernel
buffer address would land file data inside the kernel). Now:
access_ok(cr3, ptr, len, need_write) validates that a range lies wholly inside
user space (USER_VMIN..USER_VMAX, no overflow) and every page is present+USER
(+writable when required), checked against the process's own page tables.
copy_user_str(cr3, uptr, kbuf, max) copies a NUL-terminated string from user
space with per-page validation.
SYS_WRITE and SYS_OPEN now copy the string through copy_user_str; SYS_READ and
SYS_WRITE_FILE validate the [buf,buf+len) range (length capped at 64 KiB) with
access_ok. Any bad pointer returns EFAULT (-14) instead of faulting the kernel.
Fuzzing suite (`fuzz` command) — 8/8
Targeted (the exact holes, now closed):
SYS_OPEN with a valid user name pointer succeeds; with a kernel address it's
rejected (EFAULT).
SYS_WRITE with a kernel-address or unmapped pointer is rejected (no kernel leak).
SYS_READ into a kernel address is rejected (no kernel write); into a valid user
buffer it succeeds.
Randomized:
20000 calls across all syscall numbers x an adversarial argument pool (0, 1,
device/fb kernel windows, kernel addresses, USER_VMIN/VMAX edges, ~0, huge
lengths, valid + unmapped user pages), with rotating capability sets. Result:
~13.7k rejected, ~6.3k handled, and the KERNEL NEVER FAULTED.
Post-fuzz integrity:
Kernel still has zero writable+executable pages; fuzz-process isolation intact.
The fuzzer runs each call in the fuzz process's own address space, so valid
pointers resolve and hostile ones are rejected exactly as in a real ring-3
syscall.
Note
One production-validation check was updated: it previously "opened" a file using
a kernel string literal as the name; the hardened SYS_OPEN now correctly rejects
that kernel pointer, so the check asserts the capability gate is passed rather
than a successful open.
Proof (docs/fuzz-boot.log)
Full boot green: 19/19 validation, 21/21 invariants, 10/10 stress, 8/8 fuzz,
DHCP, animated compositor, ring-3 (guarded stacks, register preservation), shell.
Verified on BIOS ISO and the NVMe/UEFI install image.
Next
Remaining wishlist item: fold a periodic descriptor-table integrity sweep into
the scheduler tick (continuous, rather than on-demand, corruption detection).

Outrun OS v0.21 — periodic descriptor-table integrity sweep (folded into the scheduler tick)
The page-table auditor is no longer an on-demand scan: it now runs continuously
in the background, driven by the 100 Hz timer tick, so corruption is detected as
it happens rather than only when a test is invoked.
Design: amortized, bounded, interrupt-safe
The timer tick runs in interrupt context, so a full page-table walk there would
be far too much work. The sweep is INCREMENTAL instead:
Each tick audits a bounded batch (SWEEP_BATCH = 64 leaf entries) and advances
a persistent 4-level cursor (PML4 -> PDPT -> PD -> PT), so it resumes exactly
where it left off — including mid-page-table.
A full pass over the kernel address space completes across many ticks, then
the cursor wraps and the next pass begins. Fixed, predictable per-tick cost.
sweep_tick() takes no locks, allocates nothing, and prints nothing: it only
reads page tables and updates counters — safe to call from the ISR.
Invariants checked on every leaf entry
Reserved bits [52..58] are zero (bit-flip / corruption detection).
W^X: no page is both writable and executable.
Kernel .text stays exactly R+X (present, not writable, not NX).
Armed at boot
Enabled immediately after harden_kernel_wx(), so the invariants it audits are
already established: "descriptor-table integrity sweep armed: 64 entries/tick
@ 100 Hz".
`sweep` command — status + LIVE detection proof
Reports passes completed, leaf entries audited, and violations found. Then it
proves the auditor is real: it injects a reserved-bit flip into a live PD entry
and waits for the BACKGROUND sweep (not an on-demand scan) to report it.
Measured at boot:
5 full passes completed, 5285 leaf entries audited, 0 violations on the
healthy system.
Injected flip caught by the background sweep at va 0x200000 after ~6-7 ticks
(~60-70 ms), then the entry is repaired and the system continues.
Proof (docs/sweep-boot.log)
Full boot green with the sweep running continuously: 19/19 validation, 21/21
invariants, 10/10 stress, 8/8 fuzz, sweep PASS, DHCP, animated compositor,
ring-3 (guarded stacks + register preservation), shell. No measurable disruption
to the scheduler, networking, or the compositor.
Honest scope
This audits the KERNEL address space (the shared identity map + kernel tables),
which is where corruption is fatal; per-process user tables are validated by the
on-demand invariants suite. Detection is a software invariant auditor, not ECC:
it catches flips in bits the invariants constrain (reserved, W^X, .text
permissions), not arbitrary data corruption.
Status
This completes the hardening wishlist: exhaustive capability isolation, W^X/NX
enforcement, guard pages + stack canaries, syscall-argument fuzzing with
user-pointer validation, and now continuous background integrity auditing.

Outrun OS v0.22 — IOMMU track: ACPI/DMAR discovery + Intel VT-d DMA remapping
Opens the hypervisor/IOMMU track with the half that actually gates secure device
passthrough: hardware DMA isolation. Devices are now confined to page tables the
kernel controls, and this is proven on real (emulated) VT-d hardware.
ACPI table discovery
RSDP captured from Multiboot2 tags 14 (ACPI 1.0) and 15 (ACPI 2.0+), so it
works under both BIOS/GRUB and UEFI.
RSDT/XSDT walker with checksum validation; acpi_find_table() locates tables by
signature (prefers the 64-bit XSDT when present).
DMAR parsing
Parses the ACPI DMAR table: host address width, DRHD units (remapping hardware
register base + segment + INCLUDE_PCI_ALL scope), and RMRR reserved regions.
Found on QEMU q35: DRHD #1 with registers at phys 0xfed90000.
Intel VT-d driver (DMA remapping)
Maps the remapping registers into the kernel device MMIO window and decodes
VER/CAP/ECAP: domains supported, MGAW, SAGAW, superpage sizes, IR capability.
Adapts to the hardware: SAGAW is read at runtime and the second-level page
tables are built 4-level (48-bit AGAW) or 3-level (39-bit) accordingly, using
2 MiB superpages when CAP.SLLPS reports support. On QEMU this correctly selects
3-level / 2 MiB / 1024 MiB identity map.
Builds root table -> per-bus context tables -> second-level page tables, sets
RTADDR, issues SRTP, performs global context-cache + IOTLB invalidation, then
sets GCMD.TE and confirms GSTS.TES. Enabled BEFORE PCI enumeration so devices
come up already under translation.
virtio driver fix: VIRTIO_F_ACCESS_PLATFORM
Real bug found and fixed: the virtio drivers negotiated only VIRTIO_F_VERSION_1,
so with a vIOMMU present the devices BYPASSED it entirely (virtio only uses the
platform/IOMMU address space when VIRTIO_F_ACCESS_PLATFORM, feature bit 33, is
negotiated). Both virtio-blk and virtio-net now negotiate bit 33 when offered —
their DMA genuinely walks our IOMMU page tables. Backward compatible: when the
device does not offer bit 33 (no IOMMU), behaviour is unchanged.
`iommu` command — status + LIVE DMA isolation proof (6/6)
DMA translation enabled (GSTS.TES) and root table programmed (GSTS.RTPS).
Isolation: the NIC's context entry is pointed at a domain that maps NOTHING,
then the device is made to attempt a real DMA. The hardware BLOCKS it and
records the fault, e.g.:
fault: source-id 18 (0:3.0) addr 0x1017000 reason 6 (read access denied) op=read
The fault names the offending device (source-id matches the NIC's BDF) — this
is per-device DMA confinement, the exact property secure passthrough needs.
The domain is then restored and the device DMAs cleanly again; translation
stays enabled across the domain churn.
Fault-recording registers (FRCD at CAP.FRO) are parsed properly: source-id,
faulting address, reason code, read/write.
Honest scope
VT-x/VMX is NOT usable here and this is reported, not faked: there is no KVM in
this environment and QEMU's TCG interpreter does not emulate VMXON, so a
hypervisor cannot execute a guest. `[virt] CPUID.1:ECX.VMX = 0`. The IOMMU half
is fully real and is what gates safe passthrough; guest execution would need a
host with KVM.
Interrupt remapping (IR) is not enabled (QEMU reports no-IR in this config);
DMA remapping only.
All devices on bus 0 currently share one identity-mapped domain (domain-id 1).
Per-device restricted domains are demonstrated by the isolation test; wiring
them to the capability system is the natural next step.
Verified
BIOS ISO on q35 + intel-iommu: 19/19 validation, 21/21 invariants, 10/10 stress,
8/8 fuzz, sweep PASS, 6/6 IOMMU, CAS + DHCP (both DMA through the IOMMU),
compositor, ring-3, shell. Zero exceptions.
NVMe/UEFI install image on q35 + intel-iommu: same, ACPI 2.0 RSDP from UEFI.
REGRESSION: standard i440fx boot with NO IOMMU still fully green — reports
"no DMAR table" and degrades gracefully.
New `make qemu-iommu` target.

Outrun OS v0.23 — capability-bound IOMMU DMA domains
Binds the IOMMU to the capability system. Granting a process a device no longer
just maps its MMIO registers: the device is simultaneously confined by hardware
to a DMA domain containing ONLY that process's memory. This is what makes the
blueprint's "unrestricted hardware access" safe by construction — an app drives
the device directly, and the device physically cannot reach the kernel or any
other process.
How it works
Every kdev entry now carries its PCI source-id (BDF).
On a successful sys_hardware_passthrough (which already requires BOTH
CAP_HW_PASSTHROUGH and the device-specific capability), the kernel calls
iommu_attach_proc_domain(): it builds a per-process second-level page table by
walking the process's OWN page tables and identity-mapping only its USER RAM
frames — skipping the shared kernel identity map, the kernel MMIO window, and
device MMIO (PCD) pages — then points the device's context entry at that table
with a per-process domain id (16 + proc index) and invalidates the caches.
iommu_detach_to_kernel() returns a device to the kernel's identity domain
(revocation).
New second-level helpers: slpt_map4k() (creates levels on demand, adapts to the
3- or 4-level AGAW chosen at init) and slpt_lookup() (for verification).
`capdma` command — 11/11
Structural:
The capability holder is granted the device's MMIO, and the grant creates a
private DMA domain for the owner.
The device's context entry points at the owner's domain and carries the owner's
domain id.
Reachability (what the device can and cannot touch):
The owner's own RAM page IS reachable.
Another process's RAM page is NOT reachable.
Kernel memory is NOT reachable.
The kernel's virtqueue memory is NOT reachable.
Device MMIO was not mapped into the DMA domain.
Live enforcement + revocation:
The confined device attempts a real DMA into kernel memory and is BLOCKED by
hardware, with the fault naming it:
blocked: device 0:3.0 tried read at 0x1017000 — outside pid 16's domain
Revoking the grant returns the device to the kernel domain.
Note on the single live-fault test
A blocked DMA puts a QEMU virtio device into its "broken" state, so a device can
only be made to fault once per boot. The live proof therefore runs exactly once,
in `capdma` (the more meaningful demonstration); `iommu` keeps the hardware
status checks (GSTS.TES / GSTS.RTPS) and reports the fault-recording geometry.
Honest scope
The grant is exclusive by nature: once a device is confined to a process's
domain, the KERNEL's own driver for that device can no longer DMA to it (its
virtqueue is outside the domain). That is correct passthrough semantics, but
the kernel driver does not yet formally relinquish the device on grant — a real
handoff would quiesce the in-kernel driver first.
IOVA scheme is identity (device sees physical addresses of the owner's frames).
Domains are rebuilt only on first grant; pages mapped into a process after its
domain is built are not added yet (no live domain update).
VT-x remains unavailable in this environment (no KVM; TCG does not emulate
VMXON) and is reported honestly, not faked.
Verified
q35 + intel-iommu (BIOS ISO): 19/19 validation, 21/21 invariants, 10/10 stress,
8/8 fuzz, 2/2 iommu, 11/11 capdma, plus CAS + DHCP (DMA through the IOMMU),
compositor, ring-3, shell. Zero exceptions.
REGRESSION: no-IOMMU i440fx boot still fully green; capdma reports the platform
has no IOMMU and degrades gracefully.

Outrun OS v0.24 — the ring-3 userspace device driver
The architectural arc closes. An unprivileged ring-3 process is handed the real
NIC and drives it end to end: the kernel's involvement stops at the capability
check, and the device is hardware-confined to that process's memory while it
does real DMA. Direct hardware access with no ability to corrupt the system.
What the ring-3 driver does (user/init.c, role 1)
Entirely at ring 3, through syscalls only:
sys_hardware_passthrough — the capability gate (CAP_HW_PASSTHROUGH +
CAP_NETWORK); on success the NIC's MMIO registers are mapped into the
process AND the device is placed in the process's IOMMU domain.
sys_dma_alloc — allocates physically contiguous DMA memory, maps it into the
process, and adds it LIVE to the process's IOMMU domain.
Brings the device up from scratch: reset -> ACK/DRIVER -> feature negotiation
(VERSION_1 + ACCESS_PLATFORM) -> FEATURES_OK.
Builds the TX virtqueue (descriptor table, avail ring, used ring) inside its
OWN DMA pages, points the device's queue registers at those physical
addresses, enables the queue, sets DRIVER_OK.
Writes a frame into its own page, publishes a descriptor, kicks the notify
register, and polls the used ring.
Result, on real (emulated) hardware:
[drv:r3] NIC registers mapped into this process at vaddr 0000400480000000
[drv:r3] own DMA memory: vaddr 0000520000000000 -> phys 00000000010cd000
[drv:r3] negotiated features, FEATURES_OK accepted by device
[drv:r3] TX virtqueue live in our own pages; DRIVER_OK set
[drv:r3] *** TX COMPLETED BY HARDWARE *** used.idx=1
The NIC read the descriptor and the frame out of the process's memory by DMA —
no kernel driver in the data path — while confined to its owner's IOMMU domain.
New kernel support
SYS_DMA_ALLOC (11): capability-gated (CAP_HW_PASSTHROUGH), physically
contiguous, zeroed, mapped USER|WRITE|NX into a per-process DMA window
(0x520000000000), returns the IOVA/physical address through a validated user
pointer. Bounded to 64 pages.
SYS_ROLE (12): lets one ELF serve as both the demo app and the driver.
iommu_domain_add_page(): LIVE domain update, so memory allocated after a grant
joins the device's domain (exercised by the driver: it allocates DMA memory
after passthrough and the device can reach it).
SYS_DEV_OFFSET kinds 6..10 expose the NIC's real virtio layout (common /
notify / ISR / device-config offsets + notify multiplier) so a ring-3 driver
can locate the register structures inside its own MMIO mapping. (These come
from virtionet_probe; the old pci_probe_virtio path was dead code.)
kdev entries carry their PCI source-id; kproc carries role + a DMA bump.
Notes
A device reset from ring 3 also clears QEMU's "broken" virtio state, so the
driver recovers the NIC that `capdma` deliberately faulted earlier in the boot.
Verified both WITH an IOMMU (device confined to the driver's domain) and
WITHOUT one (driver still works, simply unconfined) — the driver does not
depend on the IOMMU being present.
Honest scope
TX only; the driver does not set up the RX queue or handle interrupts (it
polls the used ring). It is a demonstration driver, not a complete NIC stack.
The in-kernel NIC driver still does not formally relinquish the device on
grant; the ring-3 driver's device reset is what effects the handoff. A real
handoff protocol would quiesce the kernel driver first.
VT-x remains unavailable in this environment (no KVM, TCG does not emulate
VMXON) and is reported honestly.
Verified
q35 + intel-iommu: 19/19 validation, 21/21 invariants, 10/10 stress, 8/8 fuzz,
2/2 iommu, 11/11 capdma, ring-3 driver TX COMPLETED, plus CAS + DHCP,
compositor, ring-3 demos, shell. Zero exceptions.
REGRESSION (no IOMMU): all suites green, ring-3 driver TX still completes.

Outrun OS v0.25 — the Metropolis-Terminal Spatial Canvas
Pivots up-stack. The compositor was a fixed animation; it is now a real
interactive spatial canvas — the blueprint's interface promise, running on the
hardened kernel.
Windows live in a world, not on a screen
A camera (pan + zoom) in 16.16 fixed point projects world space to the screen:
w2sx/w2sy/wsc, with FXDIV for the inverse. No FPU is used anywhere (the kernel
is built -mno-sse -msoft-float), so the whole canvas is integer math.
The grid is world-anchored and its spacing follows the zoom, so panning and
zooming move through a continuous space rather than scrolling a bitmap. A world
origin marker makes this visible.
Frustum culling + level-of-detail: off-screen windows are skipped; windows that
shrink below ~10 px collapse to a coloured tick instead of a full panel.
Zoom is clamped to 12.5%..400%.
Interaction: keyboard-first, three input modes' worth of affordances
WASD pans (pan distance scales inversely with zoom, so it feels constant).
Q/E zoom out/in, F = zoom-to-fit, TAB cycles focus (and centres the camera on
it), X/ESC exits.
All input arrives through the existing PS/2 IRQ ring buffer.
The Accelerator HUD (`/`)
Opens a command bar with a live-filtered command list, a caret, and a
highlighted first match. ENTER runs the top match, ESC closes, backspace edits.
Commands: zoom fit / zoom in / zoom out / center canvas / tile windows and
focus <window> for each of the five panels.
Case-insensitive prefix filtering: typing "foc" narrows the list to exactly the
five focus commands (see docs/spatial-canvas.png).
Volumetric depth + Context Ribbon
The focused window is drawn last, raised with a soft offset shadow and cyan
corner brackets; background windows keep the glass-diffusion treatment.
A context ribbon at the bottom changes with the input mode:
"WASD PAN  Q/E ZOOM  TAB FOCUS  F FIT  / ACCELERATOR  X EXIT" becomes
"TYPE FILTER  ENTER RUN  ESC CLOSE" while the HUD is open. It also shows the
live zoom percentage and the focused window's name.
Verified
A scripted demo runs at boot and drives the canvas by injecting keystrokes ONE
AT A TIME, at typing pace, into the real PS/2 ring buffer — the same path a
physical key takes (not a back door into the state machine). The script pans,
zooms, moves focus, opens the HUD, types and executes "zoom fit", then reopens
the HUD and types "foc". Result:
    [canvas ] 300 frames | camera world (-30,0) zoom 88% | focus 'COMM-DECK' | hud open

The camera really moved in world space, zoom-to-fit computed an 88% fit, TAB
moved focus, and the HUD is open and filtering. Screenshot: docs/spatial-canvas.png.
`canvas` in the shell runs it interactively for a real keyboard.
Honest scope
Keyboard only so far: no mouse/trackpad or camera-gesture input yet (there is
no PS/2 mouse driver). The blueprint's three-input-modality story is one third
done; the camera/transform layer is the part all three would share.
The five panels are still rendered demo content, not live application surfaces.
The earlier fixed animation (`gfx`) is retained and still screenshot-verified.
Status
All prior suites remain green on top of this: 19/19 validation, 21/21 invariants,
10/10 stress, 8/8 fuzz, 2/2 iommu, 11/11 capdma, ring-3 userspace driver TX
completed, plus CAS, DHCP, compositor, ring-3 demos and the shell. Zero exceptions.

Outrun OS v0.26 — PS/2 mouse driver + cursor coordinate translation locked down
The canvas gains its second input modality, and the screen<->world transform that
all modalities share is now pinned by automated invariants.
PS/2 mouse driver (IRQ 12)
Full auxiliary-device bring-up through the 8042: enable aux port (0xA8), read
the config byte, set IRQ12 + enable the aux clock, set defaults (0xF6), enable
reporting (0xF4) — every command ACK-checked.
IntelliMouse detection via the 200/100/80 sample-rate knock: the device reports
id 3, so we run 4-byte packets WITH a scroll wheel (4-bit signed Z, sign-
extended). Falls back to 3-byte packets on a plain mouse.
The IRQ handler stays tiny: it validates the sync bit (bit 3), resyncs on a bad
packet boundary, drops overflow packets, sign-extends the 9-bit X/Y deltas,
flips PS/2's up-positive Y, and accumulates deltas + button state. All
coordinate work happens in the canvas, not the top half.
Registered through the existing register_irq/pic_unmask path (IRQ12 needs the
slave PIC + cascade, which pic_unmask already handled).
Cursor coordinate translation
s2wx/s2wy: the exact inverse of the w2sx/w2sy projection.
canvas_zoom_at(sx, sy, factor): zoom about a screen point, keeping the world
point under it fixed — the anchored-zoom that makes wheel-zoom feel right.
canvas_pick(): world-space hit testing, topmost (focused) window first.
Interactions: wheel zooms at the cursor; left-drag on a window moves it in
WORLD space (screen delta / zoom, so drag tracks the pointer at any zoom);
left-drag on empty canvas pans; clicking a window focuses it. The cursor
changes colour by mode (amber idle / mint dragging a window / magenta panning).
`cursor` command — 6/6 invariants
screen->world->screen round-trips at 6 zoom levels x 25 screen points: worst
error 1 px (fixed-point rounding).
world->screen->world round-trips stable.
zoom-at-cursor keeps the world point pinned under the pointer: worst drift 0
world units across 150 zoom operations.
Panning preserves projected size (no drift); 2x zoom exactly doubles it.
Hit-testing agrees with rendering: each window's centre picks that window.
Verified with a real mouse, not a simulation
Driving QEMU's PS/2 mouse from the monitor (9 x mouse_move -30 -22) moved the
cursor from its default (512,384) to exactly (242,186) — pixel-exact against the
predicted 512-270, 384-198. That exercises the real IRQ 12 -> packet decode ->
sign extension -> delta accumulation -> cursor path.
Screenshot: docs/spatial-canvas-cursor.png.
Honest scope
Two of three input modalities now: keyboard + mouse. Camera/hand-gesture input
is still absent (it needs the local-AI landmark pipeline from the blueprint),
but it would feed the same cursor/transform layer this milestone locks down.
No cursor acceleration curve and no mouse-driven window resize yet.
Status
All prior suites remain green: 19/19 validation, 21/21 invariants, 10/10 stress,
8/8 fuzz, 2/2 iommu, 11/11 capdma, 6/6 cursor, ring-3 userspace driver TX
completed, plus CAS, DHCP, compositor, ring-3 demos, canvas and the shell.
Zero exceptions.

Outrun OS v0.27 — kinetic camera physics (momentum + easing)
The canvas stops teleporting. Panning carries momentum, zoom eases toward a
target, and commands glide instead of snapping — the "fluid, physics-based"
half of the blueprint's interface promise. All fixed-point: no FPU anywhere.
Momentum
The camera has velocity and per-frame friction (0.88). Keyboard pan is now an
IMPULSE (scaled by 1/zoom so it feels constant at any scale), not a jump.
Fling: dragging empty canvas tracks a smoothed drag speed; on release the
camera keeps gliding and decays to rest. Velocity snaps to exactly zero below
a threshold, so the camera always comes to a true stop (no endless creep).
Eased zoom, anchored every frame
Wheel/keyboard zoom sets a TARGET; the camera eases toward it (0.25/frame).
The hard part: the world point under the anchor is re-pinned on EVERY frame of
the transition — the world point is sampled before each zoom step and the
camera is re-derived after it. Naive implementations only anchor at the
endpoints, which makes the view visibly slide mid-animation.
Glide instead of teleport
`focus`, `zoom fit`, `center canvas` and TAB now set a camera/zoom target and
ease into it (0.22/frame), converging exactly and then latching.
A manual zoom or pan impulse cancels an in-flight glide (input always wins).
`kinetic` command — 8/8 invariants
A flick imparts momentum (camera keeps moving after input stops).
Friction brings the camera to a COMPLETE rest — settled in 48 frames over a
199-world-unit glide.
Glide distance is bounded (no runaway).
Eased zoom converges to its target.
The anchor stays pinned on EVERY frame of the eased zoom: worst
mid-transition drift 0 world units across 4 large zooms at different anchors.
Commanded glide converges exactly on its target (25 frames to focus).
Eased zoom never overshoots.
Zoom stays clamped to 12.5%..400% under 40 repeated impulses in each direction.
Honest scope
Zoom easing is linear in linear space rather than in log space; it reads well
at these ratios, but log-space easing would be more perceptually uniform.
Friction/easing constants are hand-tuned, not derived from a frame clock, so
the feel is tied to the ~50 fps loop rather than being frame-rate independent.
Windows keep their existing spring/collision physics; momentum applies to the
camera only.
Status
All prior suites remain green: 19/19 validation, 21/21 invariants, 10/10 stress,
8/8 fuzz, 2/2 iommu, 11/11 capdma, 6/6 cursor, 8/8 kinetic, ring-3 userspace
driver TX completed, plus CAS, DHCP, compositor, ring-3 demos, canvas, shell.
Zero exceptions.

Outrun OS v0.28 — live application surfaces (Time-Stream integration)
The canvas panels stop being mock-ups. Each one now renders real state pulled
from the kernel subsystems built earlier in this project, so the UI is a live
view of the machine rather than a rendering demo.
The surfaces
TIME-STREAM — the actual Time-Stream event log: every event's tick, origin
and text, newest first, colour-coded by kind (FILE mint / COMM magenta / SYS
amber). These are the same events emitted by vfs_write_file and friends, e.g.
"t=48 COMM Sarah: shared the Q3 revenue chart deck".
COMM-DECK — filtered to communication events only, the Unified Stream in
miniature.
SYS.TELEMETRY — live kernel counters: uptime ticks, background
descriptor-sweep passes / entries audited / violations found (red if non-zero),
PS/2 mouse packet count, registered capability-gated devices, and whether DMA
remapping is active.
TIME-NODE — Time-Stream depth (events + how many the background vectorizer
has indexed), CAS blocks in use, and live canvas camera state.
VIDEO-EDITOR — framebuffer geometry, live cursor position, wheel presence.
How it is wired
ts_count/ts_who/ts_text/ts_tick_of/ts_type_of/ts_indexed accessors expose the
Time-Stream to the compositor (which is compiled before it) without moving
code or duplicating state — the panels read the live arrays, they do not copy.
surface_render(i, x, y, w, h) replaces the demo meters; it is zoom-aware and
computes how many rows/columns actually fit, so surfaces degrade gracefully as
a panel shrinks and vanish entirely below the LOD threshold.
draw_clip() keeps text inside the panel at any zoom; draw_kv()/draw_u64_at()
render integers without any libc.
Verified
Boot renders the canvas with all five surfaces live; the Time-Stream panel shows
the same events the `stream` command queries ("the Q3 chart Sarah sent" -> Sarah's
COMM event, score 4). Screenshot: docs/live-surfaces.png.
[canvas ] 300 frames | camera world (-30,0) zoom 88% | focus 'TIME-STREAM'
Honest scope
Surfaces are read-only views: you cannot yet act on an event (click a
Time-Stream entry to restore that workspace state, as the blueprint describes).
Rendering is immediate-mode and re-reads the arrays every frame; there is no
damage tracking or per-surface caching.
Panels are still kernel-drawn; they are not ring-3 application surfaces owned
by separate processes.
Status
All prior suites remain green: 19/19 validation, 21/21 invariants, 10/10 stress,
8/8 fuzz, 2/2 iommu, 11/11 capdma, 6/6 cursor, 8/8 kinetic, ring-3 userspace
driver TX completed, plus CAS, DHCP, ring-3 demos, canvas and shell. Zero exceptions.

Outrun OS v0.29 — ring-3 application surfaces
The compositor stops being the application. An unprivileged ring-3 process now
owns a canvas window's pixels: it asks the kernel for a surface, renders into it
with zero kernel involvement, and the compositor merely places the result.
The model
`SYS_SURFACE_CREATE((w<<16)|h, slot)` (syscall 13), gated on CAP_FRAMEBUFFER:
allocates a physically contiguous, zeroed pixel buffer, maps it USER|WRITE|NX
into the calling process at a dedicated surface window (0x530000000000), and
binds it to a canvas slot with the caller recorded as owner.
`struct surface { phys, w, h, used, owner }` — the kernel tracks who owns which
window's pixels.
canvas_window() composites a bound surface (nearest-neighbour scaled to the
panel at the current zoom) instead of drawing kernel content, and labels it
"RING-3 SURFACE". Unbound windows keep the live system surfaces from v0.28.
The application (user/init.c, role 2)
Runs entirely at ring 3: requests a 200x120 surface, then renders a cyan core, a
checker field, a magenta border and a mint bar directly into its own mapped
pixels. The kernel draws none of it.
Verified by pixels, not by claims
The app's checker pattern (0x101828) is a colour the kernel palette never uses.
After boot it appears composited at x 428-719, y 384-488 — exactly the
VIDEO-EDITOR panel — 12,027 pixels of it:
    [surface] handing canvas window 4 to unprivileged pid 17
    [surface] pid 17 owns surface 4 (200x120) at user vaddr 0000530000000000
      [app:r3] 200x120 surface rendered ENTIRELY at ring 3

Screenshot: docs/ring3-surface.png.
Honest scope
Single-buffered: the app renders once and exits; the kernel keeps compositing
the buffer. No double-buffering, damage tracking, or frame synchronisation, so
a continuously-animating app could tear.
Surfaces are not yet reclaimed when their owner exits (the buffer outlives the
process by design here, but a real system needs a lifecycle).
The app cannot receive input events routed to its window yet — it renders, it
does not interact.
Composited by CPU blit with a per-pixel divide; fine at this size, but it is
not a fast path.
Status
All eight suites still report zero failures — 19/19 validation, 21/21 invariants,
10/10 stress, 8/8 fuzz, 2/2 iommu, 11/11 capdma, 6/6 cursor, 8/8 kinetic — plus
the ring-3 NIC driver's hardware TX, CAS, DHCP, canvas and shell. Zero exceptions.

Outrun OS v0.30 — input routing to ring-3 surfaces
A surface stops being a picture and becomes an application: the compositor now
delivers input to the process that owns the window, translated into that app's
OWN pixel coordinates.
Routing
Each surface carries a 16-entry event queue (struct sevent {type,x,y,code}).
surface_route(sx, sy, type, code) is the exact inverse of the compositor's
placement: screen -> the window's projected rect (via the live camera) -> the
panel's content rect -> the app's surface pixel grid. Topmost/focused first.
canvas_mouse() routes a press over a surface-backed window to its owner
instead of starting a window drag — the app gets the click, not the shell.
SYS_SURFACE_POLL(slot, *out_event) (syscall 14): CAP_FRAMEBUFFER gated, and
additionally OWNER-ONLY — a process cannot poll a surface it does not own
(returns -13). The out pointer goes through access_ok, so a hostile pointer
gets EFAULT, not a kernel write.
Proof
Three clicks were driven through the real driver path (mouse button + cursor
state, then canvas_mouse()), at screen (470,400), (560,440), (660,470). The app
received them in ITS coordinates and redrew itself:
    [surfin ] routed 3 clicks into surface 4's queue (surface-local coords)
      [app:r3] event type 1 at surface-local (31,20)
      [app:r3] event type 1 at surface-local (91,64)
      [app:r3] event type 1 at surface-local (157,97)
      [app:r3] handled 3 routed events and redrew itself

Proportional and monotonic against the screen coordinates, through a camera at
88% zoom — the transform chain from v0.26/v0.27 carries all the way into the
app's pixel space. The app then draws its own markers at those points and the
compositor recomposites. Screenshot: docs/surface-input.png.
Honest scope
Clicks only: keyboard events have a type code reserved but are not yet routed.
The app is re-entered to drain its queue (ring-3 processes still run on the
main thread rather than as scheduler threads), so this is not yet a live event
loop — the app reacts between canvas passes, not during one.
Still single-buffered, and surfaces are still not reclaimed on owner exit.
Status
All eight suites still report zero failures — 19/19 validation, 21/21 invariants,
10/10 stress, 8/8 fuzz, 2/2 iommu, 11/11 capdma, 6/6 cursor, 8/8 kinetic — plus
the ring-3 NIC driver's hardware TX, CAS, DHCP, canvas and shell.