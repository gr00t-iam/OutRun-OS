# Outrun OS v0.54.0-metal — Ring-3 GUI Applications & Terminal Subsystem

Real applications now run on the v0.53 desktop. A ring-3 client library gives
unprivileged processes a window, a non-blocking event loop and drawing
primitives; three genuine apps are built on it — a **cyber-terminal** that
executes real kernel shell commands and renders their real stdout, a **system
monitor** reading live process/memory/core facts, and a **file inspector**
browsing the VFS. Multiple windowed apps run as independent processes and are
torn down completely — window slot *and* pixel memory — on clean exit or crash.

## What's new

### Full-resolution window content surfaces (prerequisite)

v0.53 shipped a 32×32 content thumbnail, which its own changelog flagged as a
scope gap: it cannot carry legible text, so no terminal could exist on it.
Windows now get a **real ARGB surface sized to the window's content rectangle**,
and the compositor blits it **1:1 instead of nearest-scaling**. `SYS_WIN_INFO`
(43) reports the surface dimensions so an app can lay itself out.

Surfaces are allocated **one page at a time via `alloc_frame()`**, never
`alloc_frames()`: only the single-page allocator consults the frame free list,
so a contiguous multi-page allocation would permanently consume bump-allocator
space and grow unboundedly across app churn (~5.7 MB per boot in this suite).
The pages are therefore physically scattered but mapped **virtually contiguous**
in the owner; the compositor walks a per-window page table to read them. Being
ordinary USER mappings, they are reclaimed by `page_free_tree` at exit exactly
like the user stack — no DMA-grant bookkeeping, which was never the right model
for CPU-only shared memory.

### Ring-3 GUI application client library

Entirely in userland, on top of the window syscalls:

- `app_create` / `app_damage` — window creation bound to its content surface.
- **`app_poll_events`** — a strictly **non-blocking** poll that maps the
  kernel's raw event records onto an abstract vocabulary: `EVENT_KEY_DOWN`,
  `EVENT_MOUSE_CLICK`, `EVENT_WIN_CLOSE`, `EVENT_REDRAW`, `EVENT_NONE`. The app
  owns its own loop cadence; nothing ever blocks in the library.
- An 8×8 bitmap font plus `app_px` / `app_fill` / `app_rect` / `app_char` /
  `app_str` / `app_u32`, all writing **straight into the shared surface** — no
  syscall ever touches pixel data.

### The three applications

- **Cyber-Terminal** — a character grid with a typed command line, backspace and
  enter handling, and scrollback that scrolls when the grid fills. Commands are
  executed through **`SYS_RUN_CMD` (46)**, which arms a capture hook at the
  kernel's `kputc` choke point, runs the **real `shell_exec` dispatcher**, and
  returns the command's **genuine stdout** for rendering into the window. This
  is a real shell in a window, not a simulation of one.
- **System Monitor** — live core count, RAM, frame usage and the actual process
  table (pid, name, exited state) via **`SYS_SYSINFO` (44)**, with per-core
  activity bars.
- **File Inspector** — enumerates the VFS directory via **`SYS_READDIR` (45)**
  showing names and sizes, and renders a raw byte-intensity bitmap preview of a
  file's contents read through the existing `SYS_OPEN`/`SYS_READ` — the same
  path a raw-framebuffer image viewer would use.

### `cmd_apps_stress` (new suite, `appsstress`)

Six cycles, each launching the **three different real apps as independent ring-3
processes**, with every 4th cycle additionally launching an app that **crashes
while holding a live window surface**. Verifies: every cycle completes without a
watchdog timeout, every app owned its own window, a crashed app still died via
the fault path, **no window slot survived** any cycle, **no DMA grant leaked**,
the **compositor kept composing after every cycle** (never wedged by a crash),
and frame counts reconcile with zero lock-rank violations.

## Bookkeeping

- Syscalls 43 (`SYS_WIN_INFO`), 44 (`SYS_SYSINFO`), 45 (`SYS_READDIR`),
  46 (`SYS_RUN_CMD`). `SYS_READDIR` is gated on `PCAP_FILESYSTEM`; the window
  and run-command calls on `PCAP_WIMP`.
- kproc roles 25 (terminal), 26 (system monitor), 27 (file inspector),
  28 (crashes while holding a window).
- `WIN_SURF_MAXB` is now the **page-aligned** per-window vaddr stride (258
  pages); `g_wm_pagetab[][]` holds each window's scattered surface pages.

## Four bugs found live during this milestone

All four were in code written for this milestone, and all were diagnosed from
boot logs rather than assumed — one of them only after an initial wrong theory.

1. **Userland ELF rejected for W^X.** `user.ld` merged every section into a
   single `PT_LOAD`, which was `R+X` only because `init.c` happened to contain
   no writable data. The moment the app library introduced static buffers
   (first-ever `.bss`), the merged segment became **RWE** and the kernel's
   loader correctly rejected the image. Fixed by splitting the image into three
   segments — text `R+X`, rodata `R`, data/bss `R+W` — so **ring 3 now obeys the
   same W^X policy the kernel enforces on itself**, rather than weakening the
   check. (First misdiagnosed as RAM exhaustion; the `+0 freed / depth 0`
   numbers were simply the suite aborting at round 0.)
2. **Unaligned surface stride.** `600×440×4 = 0x101CC0` is not page-aligned, so
   every window with id ≥ 1 received a misaligned surface base and the tail of
   its surface fell outside the pages actually mapped — the app page-faulted
   writing its own window. Fixed by rounding the stride up to whole pages in
   both kernel and userland.
3. **Bump-allocator growth on multi-page surfaces** — described above; fixed by
   page-by-page allocation. This one was a genuine robustness improvement, *not*
   the cause of the failures it was initially credited with.
4. **Window-slot TOCTOU race in `SYS_WIN_CREATE` (SMP only).** The free slot was
   chosen under `g_wm_lock`, but the lock was then *dropped* for the (slow) page
   allocation and `used` was published only afterwards. A second core scanning
   concurrently therefore picked the **same slot id** and clobbered the first
   app's window; the victim then failed its own `SYS_WIN_INFO` ownership check
   and exited with an app-level error (1501/1601/1701). Invisible on
   uniprocessor, reproducible on every SMP cycle. Fixed by **reserving the slot
   inside the claim critical section** (`used = 1` before the lock is released)
   and publishing `ppage` last as the readiness marker, with `SYS_WIN_INFO`
   gated on it — every other consumer already tolerated `ppage == 0`.

   Worth noting: the SMP-only cross-core overlap assertion added in this same
   milestone is precisely what surfaced this race. A weaker "concurrency" check
   that passed everywhere would have shipped the bug.

## Honest scope notes

- **Uniprocessor concurrency is time-multiplexed, and the suite says so.**
  Instrumentation showed every app runs to completion inside a single dispatch
  on a uniprocessor (they are short enough never to be preempted), so two app
  windows never co-reside there. The suite therefore asserts **per-app window
  ownership on every config** — the property "multi-app windowed desktop"
  actually rests on — and asserts **genuine simultaneous ring-3 overlap only on
  SMP**, using the kernel's existing `g_inr3_max` high-water mark. On
  uniprocessor it prints an explicit NOTE rather than passing a weaker check
  dressed up as proof of simultaneity.
- **Composition is still full-frame, not dirty-rectangle.** `SYS_WIN_DAMAGE`
  remains ownership-checked plumbing; the compositor repaints the desktop each
  frame. Unchanged from v0.53 and still outstanding.
- **The interactive desktop session is still not wired into boot.** This sandbox
  is serial-only/headless, so an interactive loop cannot be verified here. The
  apps, event loops, routing and compositing are exercised through the same code
  paths by `cmd_apps_stress`.
- **The app font is a compact 43-glyph subset** (digits, A–Z folded from
  lowercase, and a little punctuation); unknown characters render as a
  placeholder glyph. A full 96-glyph ROM would dwarf the apps.
- **`SYS_RUN_CMD` is single-slot and BSP-oriented** — it arms one global capture
  buffer, runs the command synchronously, and disarms; a second concurrent
  invocation is rejected rather than interleaved.

## Verification

BIOS/GRUB ISO in QEMU, TCG-only (no KVM), virtio-vga + virtio-blk + virtio-net.
Disk images recreated fresh before every boot.

| Config | Command | `appsstress` | `wimpstress` | Result |
| --- | --- | --- | --- | --- |
| Uniprocessor, BIOS | `make qemu` | 8/0 | 18/0 | all suites **0 FAIL** |
| SMP `-smp 4`, BIOS | `make qemu` + `-smp 4` | 9/0 | 18/0 | all suites **0 FAIL** |
| q35 + Intel VT-d IOMMU | `make qemu-iommu` | 8/0 | 18/0 | all suites **0 FAIL** |

(The SMP row carries one extra `appsstress` check — the genuine cross-core
ring-3 overlap assertion that only a multi-core config can support.)

All prior suites (`gpustrs`, `netstrs`, `wimpstrs`, `vfsstrs`, `ipcstrs`,
`vfiostrs`, `kpstrs`, `leakchk`, `dmastrs`, the SMP suites and the compositor
suites) continue to pass with 0 failures.
