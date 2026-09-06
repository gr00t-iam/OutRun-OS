#!/usr/bin/env python3
"""Long-run desktop soak: does the session drift over tens of thousands of frames?

WHAT THIS IS FOR. `desktop-test` proves the desktop works. It runs for about
three minutes, which cannot see a leak that costs a handful of frames per hour.
This holds the session open, keeps driving real input at it, and watches the
quantities that would move if something were leaking:

  frames        must keep advancing; a stalled compositor is a hung desktop
  windows       must return to the resting count after every churn cycle; a
                count that ratchets upward is a window that did not close
  frames_used   the kernel's own allocator accounting, printed in the desktop
                heartbeat by the SAME expression SYS_DESKTOP_INFO reports.
                THE POINT OF THE WHOLE EXERCISE: create and destroy windows
                repeatedly and this must come back to where it started.

WHY IT DRIVES INPUT rather than idling. An idle desktop composites the same
frame forever and exercises almost nothing. Each cycle opens applications from
the launcher and closes them again through their close boxes, so window
surfaces (multi-page allocations, two sets per paired window) are allocated
and reclaimed continuously. That is the path a leak would live in, and an
idle soak would never touch it.

Usage: desktop-soak.py <iso> <log> [target_frames]
  SOAK_QEMU="-smp 4"   extra QEMU arguments
  SOAK_CAP=5400        wall-clock ceiling in seconds
"""
import importlib.util
import os
import re
import subprocess
import sys
import time

# The UI test module's name has a hyphen, so it cannot be imported by name.
# Loading it by path keeps ONE implementation of the QMP client and the log
# waiter: a second copy here would be a second thing to keep correct.
_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "desktop_ui_test", os.path.join(_HERE, "desktop-ui-test.py"))
_ui = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_ui)

ISO = sys.argv[1] if len(sys.argv) > 1 else "build/outrun-desktop-1.0.0.iso"
LOG = sys.argv[2] if len(sys.argv) > 2 else "/tmp/desktop-soak.log"
TARGET = int(sys.argv[3]) if len(sys.argv) > 3 else 50000
EXTRA_QEMU = os.environ.get("SOAK_QEMU", "").split()
QMP = "/tmp/outrun-soak.qmp"
BOOT_DEADLINE = 600
WALL_CAP = int(os.environ.get("SOAK_CAP", "5400"))

HEARTBEAT = re.compile(
    r"\[desktop\] (\d+) frames, (\d+) window\(s\), (\d+) launched, (\d+) launch failure\(s\), frames_used=(\d+)")

# Geometry mirrored from kernel64.c. Launcher tiles: g_launch; windows cascade
# from (60,40) in steps of (40,34) by id, WIN_TITLE_H 20, and the close box is
# WIMP_CLOSE_W(14)+3 in from the right edge of the title bar.
TILES = [(56, 32 + i * 44 + 18) for i in range(4)]
WIN_TITLE_H, CLOSE_W = 20, 14


def read_log():
    try:
        with open(LOG, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def heartbeats(text):
    return [tuple(int(v) for v in hit) for hit in HEARTBEAT.findall(text)]


def main():
    for stale in (QMP, LOG):
        if os.path.exists(stale):
            os.remove(stale)
    cmd = ["qemu-system-x86_64", "-accel", "tcg", "-cdrom", ISO, "-m", "512M",
           "-no-reboot", "-vga", "none", "-device", "virtio-vga",
           "-display", "none", "-serial", "file:" + LOG,
           "-qmp", "unix:%s,server,nowait" % QMP] + EXTRA_QEMU
    print("soak: %s -> %d frames%s" % (ISO, TARGET,
                                       (" (%s)" % " ".join(EXTRA_QEMU)) if EXTRA_QEMU else ""))
    print("image md5: " + subprocess.run(["md5sum", ISO], capture_output=True,
                                         text=True).stdout.split()[0])
    qemu = subprocess.Popen(cmd)
    started = time.time()
    samples = []
    resting = []
    failures = []
    cycles = 0
    try:
        _ui.LOG = LOG
        if not _ui.wait_for("[desktop] logical desktop", BOOT_DEADLINE):
            print("soak: desktop never reached its loop")
            return 1
        qmp = _ui.Qmp(QMP)
        # Window geometry for the three churned apps. Window id 0 is the
        # editor launched at boot (720 wide, clamped to WIN_MAX_W 600); the
        # rail's other three tiles then open ids 1..3 in tile order. wm_create
        # hands out the lowest free id, so after each close the same ids come
        # back, and the same close boxes are in the same places. Widths are
        # each app's app_create width (the outer width; none of these exceeds
        # the clamp). Getting one of these wrong is not harmless: a click that
        # misses a close box lands in whatever is under it, and a content click
        # RAISES that window over the ones still to be closed. The first run
        # of this soak had ids 2 and 3 swapped and ratcheted to 12 windows.
        widths = {1: 328,      # NUMWORKS  apps/calc.c      app_create(&w, 328, 374, ..)
                  2: 430,      # SYS-DIAG  apps/task_mgr.c  app_create(&w, 430, 440, ..)
                  3: 390}      # CTRL DECK apps/settings.c  app_create(&w, 390, 350, ..)
        closers = []
        for wid, ww in widths.items():
            wx, wy = 60 + (wid % 6) * 40, 40 + (wid % 6) * 34
            closers.append((wx + ww - CLOSE_W - 3 + CLOSE_W // 2, wy + WIN_TITLE_H // 2))
        # A first cycle that does not bring the count back to 1 means the
        # geometry above is wrong for this image, and every later number would
        # be noise. Fail fast and say so instead of soaking a broken harness.
        resting_seen = False

        while True:
            hbs = heartbeats(read_log())
            if hbs:
                samples.append((time.time() - started,) + hbs[-1])
            if hbs and hbs[-1][0] >= TARGET:
                break
            if time.time() - started > WALL_CAP:
                failures.append("wall-clock cap %ds hit before %d frames" % (WALL_CAP, TARGET))
                break
            # One churn cycle: open calc, monitor and deck from the rail, then
            # close them in reverse so the topmost is always the one being
            # hit. Close boxes are hit-tested by the SAME code path a person's
            # click goes through.
            for tile in TILES[1:]:
                qmp.click(*tile)
            time.sleep(4)
            for cx, cy in reversed(closers):
                qmp.click(cx, cy)
            cycles += 1
            # Wait for the desktop to REPORT the resting state before the next
            # cycle. The heartbeat is every 5 s of guest time; a resting sample
            # is one that says exactly one window. Sampling mid-cycle and
            # calling it a floor is how the first version of this harness
            # reported an allocator "leak" that was three windows still open.
            settle_end = time.time() + 40
            while time.time() < settle_end:
                hbs = heartbeats(read_log())
                if hbs and hbs[-1][1] == 1:
                    resting.append(hbs[-1])
                    resting_seen = True
                    break
                time.sleep(1)
            if not resting_seen and cycles >= 3:
                failures.append("window count never returned to 1 in the first %d cycles "
                                "(harness close-box geometry is wrong for this image)" % cycles)
                break

        text = read_log()
        hbs = heartbeats(text)
        print("\nresting samples (one per cycle, desktop reporting 1 window):")
        print("%-7s %-8s %-9s %s" % ("cycle", "frames", "launched", "frames_used"))
        step = max(1, len(resting) // 15)
        for i, r in enumerate(resting):
            if i % step == 0 or i == len(resting) - 1:
                print("%-7d %-8d %-9d %d" % (i + 1, r[0], r[2], r[4]))

        ok = True

        def check(name, cond):
            nonlocal ok
            print("  %s  %s" % ("PASS" if cond else "FAIL", name))
            if not cond:
                ok = False
                failures.append(name)

        last = hbs[-1] if hbs else None
        check("reached %d composited frames" % TARGET, bool(last) and last[0] >= TARGET)
        if len(samples) >= 4:
            mid = samples[len(samples) // 2][1]
            check("the compositor was still advancing in the second half",
                  bool(last) and last[0] > mid)
        check("no panic, fault or lock-rank violation",
              not any(b in text for b in ("PANIC", "rank violation", "UNDERFLOW", "page fault")))
        check("no launch failure during the whole soak (%d)" % (last[3] if last else -1),
              bool(last) and last[3] == 0)
        check("%d churn cycles were driven (each: 3 launches, 3 closes)" % cycles, cycles >= 10)
        # Windows: every cycle must have ended with the desktop REPORTING one
        # window. `resting` holds only heartbeats that said so, one per cycle;
        # a cycle that never produced one is a window that did not close.
        check("every cycle returned the window count to 1 (%d of %d cycles)"
              % (len(resting), cycles), cycles > 0 and len(resting) == cycles)
        # Allocator: frames_used at the resting points must not trend up. The
        # first resting value is the baseline; every later one must equal it.
        # Surfaces are page-granular and the same three apps are opened each
        # time, so there is no legitimate jitter to allow for -- a rise of even
        # one frame per cycle is a leak, and it would show as `cycles` frames.
        rest_used = [r[4] for r in resting]
        drift = (max(rest_used) - rest_used[0]) if rest_used else -1
        check("allocator resting level did not drift (frames_used %s, max drift %d over %d cycles)"
              % (("%d..%d" % (rest_used[0], rest_used[-1])) if rest_used else "none", drift, cycles),
              bool(rest_used) and drift == 0)
        print("\nwall clock: %ds" % (time.time() - started))
        if failures:
            print("FAILURES: " + "; ".join(failures))
        print("log: %s" % LOG)
        return 0 if ok else 1
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=15)
        except subprocess.TimeoutExpired:
            qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
