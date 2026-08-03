# OutRun OS v0.71.0-metal — typing into it

Milestone 71. v0.70 gave the desktop buttons, checkboxes and progress bars, and
listed first among its limits:

> **No text entry.** There is no editable field, which is the widget a real
> desktop needs next and the one that needs a caret, a selection and a
> keyboard-focus model per widget rather than per window.

This is that field, and the focus model underneath it.

## FOCUS WAS PER WINDOW; IT HAD TO BECOME PER WIDGET

Since v0.53 the compositor has known exactly one thing about keyboard input:
which *window* has it. That is enough to deliver a keystroke to an
application, and not nearly enough to decide **which of its controls** the
keystroke is for. A window with two text fields has no answer.

`struct wmwin` gains `focus_wg`, and the routing gains one rule: **the focused
widget gets first refusal, and everything it does not want falls through to the
application unchanged.** A toolkit that swallowed keys its focused control had
no meaning for would silently break every application shortcut on the desktop,
so the suite asserts the fall-through directly rather than trusting it.

### Validating the index instead of clearing it everywhere

`focus_wg` names a widget slot, and widget slots are recycled. The obvious
implementation clears it wherever a window is allocated, destroyed or reused —
which is three or more sites that must all agree, forever.

v0.70 shipped exactly that shape of bug: `wm_destroy` released a window's
widgets and `wimp_teardown_kproc` open-coded the same reset without the
release, so the release never ran on process exit. **One place that must be
right beats N places that must agree.** So `wg_focused()` validates on read —
a stale index that no longer names a live widget of this window simply reads as
"nothing focused" — and a recycled slot is harmless by construction rather than
by everyone remembering.

## THE FIELD

`WG_ENTRY`. Printable ASCII appends; backspace and DEL remove the last
character; the border brightens and a caret appears when it holds focus, so
"where does my typing go" is answerable by looking rather than by remembering
what was clicked last.

**A full field eats the keystroke rather than passing it on.** Dropping the
character is a visible non-event the user immediately understands; letting it
fall through to the application, which would then see stray input it never
asked for, is worse than doing nothing. The overflow case is asserted
explicitly, because nothing else in the suite would notice a field that ran off
the end of its own 24-byte buffer.

## THE KEYBOARD CAN NOW DRIVE THE DESKTOP

v0.70 named the gap plainly:

> **No scrolling, no lists, no menus**, and no keyboard traversal — a widget is
> reachable by pointer only, which is an accessibility gap and is named as one.

Half of that is now closed. **Tab** moves focus to the next enabled interactive
control, wrapping, skipping labels, progress bars and anything disabled.
**Enter or Space** operates the focused control — same toggle, same `WIDGET`
event, same counter as a click, so a keyboard user and a mouse user are
indistinguishable to the application receiving the event.

That last property is the one worth stating: it is not a parallel code path
that happens to do something similar. It is the same effect, which is why an
application written for the mouse is keyboard-operable without knowing.

## SYSCALLS

No new ones. `SYS_UI_GET` grows two selectors:

| `what` | returns |
| --- | --- |
| 2 | copies the widget's text into the user buffer, returns its length |
| 3 | the window's focused widget id, or -1 |

`what = 2` is the call that reads what was typed. Its text is copied out **into
a kernel buffer under the lock, and only then written to user memory**, because
`access_ok` can fault and faulting with a rank-10 lock held is how a compositor
deadlocks. A NULL destination is refused rather than written through.

## VERIFICATION

42 suites (44 on VT-d), 0 FAIL on uniprocessor/BIOS, SMP-4/BIOS, and q35 + VT-d
IOMMU (`-smp 4`), each against a freshly created volume. No suite is added —
`wimpstrs` grows from 29 assertions to 39 — so every configuration's suite set
is identical to its v0.70 baseline. Boot logs are in `docs/`.

Every keyboard assertion is driven through **`wimp_key`, the same function the
PS/2 and USB keyboards feed**, for the reason `wimp_pointer` is shared: a suite
that reimplemented the routing would be verifying its own copy and not the
kernel's.

The assertions are written so that a plausible-but-wrong implementation fails
at least one:

- a field that ignored focus passes "typing inserts" and fails "a key with no
  widget focused is not consumed";
- a router that consumed every key passes the typing tests and fails the
  fall-through;
- one that consumed none fails the typing tests;
- a `disabled` that only greyed the pixels passes everything except "a disabled
  entry ignores keystrokes";
- Tab traversal is asserted as a **set** — every enabled control reached, the
  disabled one never — so it does not depend on which table slots the widgets
  happened to land in, which is what made an earlier ring-3 probe fragile.

And `wimp_driver` exercises `SYS_UI_GET(what = 2)` **from real ring 3 into a
real user buffer**, poisoned first so a no-op copy is detectable. The
kernel-side suite cannot do that half — it has no user-mapped destination — so
without the ring-3 half the read-back path would ship entirely unexercised.

### Warnings

46, all pre-existing; this release adds none.

### Not done

- **No caret movement.** Insertion is at the end and backspace removes from the
  end; there are no arrow keys, no Home/End, and no click-to-position. The
  caret is drawn where the next character will go, which is honest, but it is a
  cursor in the display sense and not yet in the editing sense.
- **No selection, no clipboard, no cut/copy/paste.** These belong together with
  caret movement and with a system-wide clipboard that does not exist yet.
- **Still no layout.** Widgets remain at absolute coordinates inside the
  content rect; a resized window keeps its widgets where they were.
- **Still no lists, menus or scrolling**, and Tab traversal does not yet
  reverse (no Shift-Tab), so the keyboard can reach every control but only in
  one direction.
- **24 bytes per field.** Long enough to demonstrate the contract and to make
  the overflow case testable, far too short for real input; a longer field
  needs the text to live somewhere other than inline in a fixed widget record.
