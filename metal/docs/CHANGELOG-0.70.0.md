# OutRun OS v0.70.0-metal — widgets

Milestone 70. A window could be created, painted, dragged, minimized and
closed. What it could not have was a *button*. This adds the smallest complete
widget set — label, button, checkbox, progress bar — declared through syscalls,
drawn by the compositor, and reported by name when clicked.

## THE TOOLKIT IS POLLED, AND THAT IS NOT A SHORTCUT

Every desktop toolkit worth the name is callback-driven: you hand it a function
and it calls you back. This one cannot be, and the reason is a fact about this
system rather than a preference.

`occ` — the self-hosting compiler this OS ships, and the one `vsh` is built
with — **cannot produce a function pointer**. A callback toolkit would
therefore be unusable from the only shell this system has, which makes it not a
toolkit but a decoration for programs compiled elsewhere.

So widgets are **retained and polled**. An application declares a button once;
the button persists; a click on it arrives in the window's existing event queue
as a `WIDGET` event carrying the widget's id. The application asks "what
happened" on its own schedule, in a loop it already has, using only language
features `occ` can compile. Every widget is reachable from a program this
system can build itself.

## THE KERNEL DRAWS THEM

A widget is painted by the compositor, not by the application that declared it.
That is a deliberate inversion of how a userland toolkit works, and it buys the
one property a library cannot enforce: **a program that declares a button gets
a button that looks like every other button on the desktop**, including
programs written by people who have never seen the others. Consistency is the
part of "user friendly" that cannot be delegated to application authors.

It also means a widget costs the application nothing to maintain. There is no
redraw callback to get wrong, and no frame in which a button is missing because
its owner was descheduled.

## SYSCALLS

| # | call | notes |
| --- | --- | --- |
| 85 | `SYS_UI_ADD(win\|kind, rect, text)` | returns a widget id, or negative |
| 86 | `SYS_UI_SET(win\|id, what, value)` | `what`: 0 value, 1 enabled, 2 text |
| 87 | `SYS_UI_GET(win\|id, what)` | `what`: 0 value, 1 enabled |

All three are `PCAP_WIMP`-gated and all three check that the window is **owned
by the calling thread group** — resolved through `tg_of`, so a thread may
decorate a window its process created, and nothing else may touch it. The
window id and the second small integer share `a0` for the same reason
`SYS_MMAP_FILE` packs its descriptor: the dispatch ABI has three argument
registers and the rectangle needs one to itself.

**Bounds are checked at declaration, not at draw time.** A widget that would
fall outside its window's content rectangle is refused with `-EINVAL` rather
than silently clipped. Clipping would turn a layout mistake into an invisible
one, with no error attached to it anywhere.

## A WIDGET CANNOT OUTLIVE ITS WINDOW

`wm_destroy` releases every widget belonging to the window it is destroying.
That was supposed to cover the close box, the owner exiting normally, and the
owner **dying of a page fault**.

It did not, and finding out why is the most useful thing this milestone did —
see below.

The suite asserts it from the strongest position available: after the ring-3
churn rounds — clean *and* deliberately faulted, every one of which declared
widgets — the widget table must be **empty**, checked before any later
assertion could mask a survivor.

## DRAWING WITHOUT THE LOCK, ON PURPOSE

`wimp_draw_widgets` reads the widget table live and **without** `g_wm_lock`,
while the window itself is drawn from a snapshot taken under it. That is a
considered choice, not an oversight. Widget state is four integers and a short
string; the worst a concurrent `SYS_UI_SET` can do is show one frame with the
old label, which the next compose corrects. Taking the lock would put a rank-10
acquisition around the draw primitives, and the compositor already goes to
some trouble never to do that, because drawing is slow and holding a lock
across it is how a compositor stalls the system it is drawing.

A torn frame is cheaper than a held lock. This release states the trade rather
than leaving it to be inferred from the absence of an acquire.

## THREE DEFECTS THIS FOUND IN ITSELF

**Two teardown paths, and the widget release went into the wrong one.**
`wm_destroy` releases a window's widgets. `wimp_teardown_kproc` — the function
every *process exit* goes through, clean or faulted — did not call it. It
open-coded the same reset instead: a strict subset, already missing
`minimized`, and now missing the widget release too. So the close box freed
widgets and nothing else did.

The symptom was not a leak report. It was **round 15 of 16** failing to declare
a button, because a 32-entry table had filled with the corpses of fourteen
earlier rounds — a failure that looks like a capacity limit and is really a
missing call. On a desktop it would have looked like "the GUI stops working
after you have opened enough programs".

The fix is not to add the missing line to the second copy. It is that
`wimp_teardown_kproc` now calls `wm_destroy`, so there is **one** teardown
path and the next thing a window owns cannot be forgotten in the same way. Two
places that must agree is the defect; the missing line was only how it
surfaced.

Worth stating plainly: this bug is older than the widgets. Duplicating the
reset was always the hazard, and v0.70 is simply the first release to add
something to a window that the duplicate did not know about.


**A test fixture that broke a test.** The widget block seeded its widgets on a
window the surrounding WM-logic assertions still needed, and destroyed it to
prove widget release. Every widget assertion passed; the *minimize* assertion
forty lines later failed, because it was hit-testing a window that no longer
existed. The fix is that the block owns and destroys its own window. Worth
recording because the failure appeared nowhere near its cause, and because the
first instinct — that the new code was wrong — was the wrong instinct.

**A label could have faulted the kernel.** The first implementation validated
the user's text pointer with a single-byte `access_ok` and then read up to 23
bytes from it. A label whose first byte is the last byte of a mapping would
have walked the kernel into an unmapped page — from ring 3, with an ordinary
argument. `copy_user_str` already exists and re-validates on every page
crossing; both call sites now use it. Nothing in the suite would have caught
this, because no test places a string at a page boundary.

## VERIFICATION

42 suites, 0 FAIL on uniprocessor/BIOS, SMP-4/BIOS, and q35 + VT-d IOMMU
(`-smp 4`). No suite is added — `wimpstrs` grows from 18 assertions to 28 — so
every configuration's suite set is identical to its v0.69 baseline. Boot logs
are in `docs/`.

The widget assertions are driven through the **real syscalls** and the **real
`wimp_pointer` path**, not by writing the table directly, so what is verified
is the interface an application would use:

- a button and a checkbox can be declared, and one that would not fit is
  refused;
- clicking a button delivers a `WIDGET` event **naming that button**, rather
  than a raw click the application would have to hit-test for itself;
- clicking a checkbox toggles it and the new state reads back — the entire
  contract of a polled toolkit in one assertion;
- a **disabled** widget does not fire, and the click falls through as ordinary
  content, which is the behaviour that distinguishes "disabled" from "absent";
- a click on bare content is still a plain click, so the widget layer cannot be
  swallowing input generally;
- destroying a window releases every widget it owned.

That the disabled case and the bare-content case are both present matters
together: a router that consumed every click passes the button test and fails
those two, and a router that consumed none passes those two and fails the
button test.

And separately, `wimp_driver` declares widgets **from real ring 3, with a real
user-mapped label string**. The kernel half cannot do that — a `.rodata`
pointer in kernel context is correctly refused by `access_ok` — so without the
ring-3 half the text path would ship entirely unexercised.

### A note on how the matrix must be run

Each configuration gets a **freshly created** block device, which is how every
matrix since v0.48 has been run and is now written down because getting it
wrong cost a cycle here. `vfsstrs` proves the directory journal is *deferred*
by checking that the on-disk dirent does not yet match the in-memory one. Run a
second boot against the volume the first one left behind and the file is
already there with the same content hash — the storage is content-addressed, so
identical content is the identical dirent — and "not yet applied" becomes
indistinguishable from "already applied". The assertion fails, and it is
reporting the fixture, not the kernel.

That is a real limitation of that assertion rather than a rule about disks, and
it is recorded here rather than papered over. Fixing it belongs to whoever next
touches the v0.48 journal proofs, not to a GUI release.

### Warnings

46, all pre-existing; this release adds none.

### Not done

- **No text entry.** There is no editable field, which is the widget a real
  desktop needs next and the one that needs a caret, a selection and a
  keyboard-focus model per widget rather than per window.
- **No layout.** Every widget is placed at absolute coordinates inside the
  content rect. There is no box model, no reflow on resize, and a window that
  changed size would keep its widgets where they were.
- **No scrolling, no lists, no menus**, and no keyboard traversal — a widget is
  reachable by pointer only, which is an accessibility gap and is named as one.
- **32 widgets system-wide**, first-fit, and a full table refuses the next
  declaration. Bounded and honest, like every other table in this kernel, but
  it is a global budget rather than a per-window one.
- **The click consumes; it does not press.** There is no armed/pressed visual
  state between button-down and button-up, so a button cannot be cancelled by
  releasing off it the way every real toolkit allows.
