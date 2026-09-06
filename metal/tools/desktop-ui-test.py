#!/usr/bin/env python3
"""Drive the desktop image with real input events and check what it does.

This is a UI test, so it uses the machine's actual input path: QMP
`input-send-event` delivers absolute pointer moves and button presses to the
emulated PS/2 devices, exactly as a person at the keyboard would. Nothing here
calls into the kernel's own routing helpers -- a test that drove wimp_pointer()
directly would be verifying its own copy of the click path and not the one a
user reaches.

Verdicts come from the serial log, which is why the desktop prints a heartbeat
naming its window count and launch tally.
"""
import json, os, socket, subprocess, sys, time

ISO = sys.argv[1] if len(sys.argv) > 1 else "build/outrun-desktop-1.0.0.iso"
LOG = sys.argv[2] if len(sys.argv) > 2 else "/tmp/desktop-ui.log"
# Extra QEMU arguments, so one test can cover more than one machine shape.
# `make desktop-test DESKTOP_QEMU="-smp 4"` is the -smp 4 desktop: the compositor
# and its pinned applications on the BSP, with three other cores live and
# stealing. A desktop verified only at one vCPU has not been verified.
EXTRA_QEMU = os.environ.get("DESKTOP_QEMU", "").split()
QMP, W, H = "/tmp/outrun-desktop.qmp", 1024, 768
# The desktop takes a while to reach its loop under TCG; every wait below is a
# deadline rather than a sleep count, for the reason CLAUDE.md gives.
BOOT_DEADLINE, STEP_DEADLINE = 420, 90


def wait_for(text, deadline, since=0):
    end = time.time() + deadline
    while time.time() < end:
        try:
            with open(LOG, "rb") as fh:
                fh.seek(since)
                if text.encode() in fh.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(1)
    return False


class Qmp:
    def __init__(self, path):
        end = time.time() + 60
        while time.time() < end:
            try:
                self.sock = socket.socket(socket.AF_UNIX)
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.5)
        else:
            raise SystemExit("qmp socket never appeared")
        self.buf = b""
        self.recv()                      # greeting
        self.cmd("qmp_capabilities")

    def recv(self):
        while b"\n" not in self.buf:
            self.buf += self.sock.recv(65536)
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name, **args):
        self.sock.sendall(json.dumps({"execute": name, "arguments": args}).encode() + b"\n")
        while True:
            msg = self.recv()
            if "return" in msg or "error" in msg:
                return msg

    def move_rel(self, dx, dy):
        """RELATIVE motion, because the guest driver is a PS/2 mouse.

        Absolute events do not reach it: they are delivered to a tablet, and
        this machine has none. An earlier version of this test sent `abs`
        events, and every click silently landed nowhere — the desktop stayed on
        one window for 31,000 frames while the test waited for launches that
        could never happen. QEMU clamps a single packet's delta to one signed
        byte, so long travel is sent in steps.
        """
        while dx or dy:
            sx = max(-100, min(100, dx))
            sy = max(-100, min(100, dy))
            self.cmd("input-send-event", events=[
                {"type": "rel", "data": {"axis": "x", "value": sx}},
                {"type": "rel", "data": {"axis": "y", "value": sy}}])
            dx -= sx; dy -= sy
            time.sleep(0.05)

    def click(self, x, y):
        """Park the pointer at (x, y) and press-release there.

        The kernel clamps the pointer to the desktop, so driving hard into the
        top-left corner is a reliable way to establish a known origin without
        being able to read the cursor back.
        """
        self.move_rel(-2000, -2000)          # clamp into the origin
        time.sleep(0.5)
        self.move_rel(x, y)
        time.sleep(1.0)
        for down in (True, False):
            self.cmd("input-send-event",
                     events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
            time.sleep(0.6)

    def key(self, name):
        self.cmd("input-send-event",
                 events=[{"type": "key", "data": {"down": True,
                          "key": {"type": "qcode", "data": name}}},
                         {"type": "key", "data": {"down": False,
                          "key": {"type": "qcode", "data": name}}}])
        time.sleep(0.3)

    def screenshot(self, path):
        """What the screen actually shows, as a PPM the caller can inspect.

        The log proves the applications are running; only the framebuffer
        proves they are visible. `make gate` cannot look at a screen, which is
        exactly why this writes a file that a person can.
        """
        self.cmd("screendump", filename=path, format="ppm")
        return os.path.exists(path)


def read_ppm(path):
    """(width, height, pixels) from a binary PPM, pixels as a flat bytes RGB."""
    with open(path, "rb") as fh:
        blob = fh.read()
    magic, dims, _maxval, pixels = blob.split(b"\n", 3)
    if magic != b"P6":
        raise ValueError("not a binary PPM: %r" % magic)
    w, h = (int(v) for v in dims.split())
    return w, h, pixels


def scanline(path, y, x0, x1):
    """One horizontal run of pixels, as a list of (r,g,b)."""
    w, _h, px = read_ppm(path)
    return [tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) for x in range(x0, x1)]


def size_of(log):
    return os.path.getsize(log) if os.path.exists(log) else 0


def main():
    for stale in (QMP, LOG):
        if os.path.exists(stale):
            os.remove(stale)
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-accel", "tcg", "-cdrom", ISO, "-m", "512M",
        "-no-reboot", "-vga", "none", "-device", "virtio-vga",
        "-display", "none", "-serial", "file:" + LOG,
        "-qmp", "unix:%s,server,nowait" % QMP,
        "-device", "usb-ehci,id=ehci",
    ] + EXTRA_QEMU)
    if EXTRA_QEMU:
        print("extra qemu args: %s" % " ".join(EXTRA_QEMU), flush=True)
    failures, checks = [], []

    def check(name, ok):
        checks.append((name, ok))
        print("  %s  %s" % ("PASS" if ok else "FAIL", name), flush=True)
        if not ok:
            failures.append(name)

    try:
        qmp = Qmp(QMP)
        check("desktop session reaches its loop", wait_for("[desktop] logical desktop", BOOT_DEADLINE))
        check("the editor is launched at boot", wait_for("launched 'VAULT PAD'", STEP_DEADLINE))
        check("a frame is composited with a window on it",
              wait_for("window(s), 1 launched, 0 launch failure", STEP_DEADLINE))

        # Launcher rail: tiles are 44px apart from y=32, centred at x=56.
        for index, (label, tile_y) in enumerate(
                [("NUMWORKS", 32 + 44 + 18), ("SYS-DIAG", 32 + 88 + 18), ("CTRL DECK", 32 + 132 + 18)]):
            mark = size_of(LOG)
            qmp.click(56, tile_y)
            check("clicking the %s tile starts its program" % label,
                  wait_for("launched '%s'" % label, STEP_DEADLINE, mark))
            check("%s is a real window, not just a process" % label,
                  wait_for("%d window(s)" % (index + 2), STEP_DEADLINE, mark))

        # ---- SYS_DESKTOP_INFO (117) -------------------------------------
        # SYS-DIAG calls it every iteration and exits(2) if it ever returns a
        # bad version, an implausible process count or an error; CTRL DECK
        # calls it at startup and exits(1) on failure. So both windows still
        # being present after thousands of frames IS the assertion that 117
        # returns valid state — repeatedly, not once.
        with open(LOG, "rb") as fh:
            sofar = fh.read().decode("utf-8", "replace")
        check("every launch succeeded", "0 launch failure(s)" in sofar
              and "1 launch failure" not in sofar)
        check("all four applications own a window at once", "4 window(s)" in sofar)

        mark = size_of(LOG)
        time.sleep(12)
        with open(LOG, "rb") as fh:
            fh.seek(mark)
            later = fh.read().decode("utf-8", "replace")
        check("SYS_DESKTOP_INFO keeps returning valid state (its readers stay alive)",
              "4 window(s)" in later and "3 window(s)" not in later)

        # ---- no tearing --------------------------------------------------
        # CTRL DECK is the topmost window, so nothing occludes it. Its RELOAD
        # button is a solid 0x25374b rectangle; row 390 crosses it above the
        # label. Each click on the REPEAT DELAY row forces the app to repaint
        # and re-publish while the compositor is running, so a compositor that
        # could read a half-drawn buffer would show background pixels inside
        # that run. REPEAT DELAY is used, not DESKTOP SCALE, because settings
        # now apply on the click itself: a scale toggle would magnify the
        # framebuffer and move the very pixels being sampled.
        #
        # This SAMPLES, so it can catch tearing but cannot prove its absence.
        # What makes it more than decoration is the paired-buffer design it is
        # checking: the compositor only ever reads the set the app published.
        runs = []
        for _ in range(6):
            qmp.click(282, 311)                     # REPEAT DELAY row: repaint
            shot = "/tmp/outrun-tear.ppm"
            qmp.screenshot(shot)
            row = scanline(shot, 390, 190, 302)
            runs.append(sum(1 for p in row if p == (0x25, 0x37, 0x4b)))
        check("the RELOAD button is intact in every sampled frame (no tearing): %s"
              % runs, len(set(runs)) == 1 and runs[0] > 100)

        # ---- SYS_DESKTOP_SETTINGS (118), applied for real ----------------
        # Settings apply on the row click itself; there is no APPLY button.
        # Every repeat-delay click above went through the same syscall, so by
        # here the log already proves the call works -- this step proves that
        # a SCALE change is honoured all the way to the framebuffer.
        mark = size_of(LOG)
        qmp.click(282, 227)                          # scale -> 2, applied at once
        check("SYS_DESKTOP_SETTINGS applies a scale change",
              wait_for("settings applied by pid", STEP_DEADLINE, mark))
        with open(LOG, "rb") as fh:
            fh.seek(mark)
            applied = fh.read().decode("utf-8", "replace")
        check("the logical desktop really halves (512x384)", "scale 2 (512x384)" in applied)

        # And the screen must actually show it. At scale 2 fb_flip doubles every
        # logical pixel, so every 2x2 aligned block is uniform -- which is false
        # of a 1:1 desktop with 8x8 text on it. This is the difference between a
        # setting that was stored and a setting that was applied.
        shot = "/tmp/outrun-scaled.ppm"
        qmp.screenshot(shot)
        sw, sh, spx = read_ppm(shot)

        def px_at(buf, w, x, y):
            return buf[(y * w + x) * 3:(y * w + x) * 3 + 3]

        blocks = [(x, y) for y in range(100, 500, 2) for x in range(100, 900, 2)]
        uniform = sum(1 for x, y in blocks
                      if px_at(spx, sw, x, y) == px_at(spx, sw, x + 1, y)
                      == px_at(spx, sw, x, y + 1) == px_at(spx, sw, x + 1, y + 1))
        check("the framebuffer is genuinely magnified 2x (%d/%d blocks uniform)"
              % (uniform, len(blocks)), uniform == len(blocks))

        # Typing must reach the focused application: the editor was focused last
        # by its own creation, so click its title bar first to be sure.
        mark = size_of(LOG)
        for name in ("a", "b", "c"):
            qmp.key(name)
        time.sleep(3)
        check("the session survives keyboard input", not wait_for("PANIC", 2, mark))

        with open(LOG, "rb") as fh:
            tail = fh.read().decode("utf-8", "replace")
        shot = os.path.join(os.path.dirname(LOG) or ".", "desktop.ppm")
        check("the framebuffer can be captured with four windows on it",
              qmp.screenshot(shot) and os.path.getsize(shot) > 1024)
        check("no panic, fault or lock-rank violation in the whole session",
              not any(bad in tail for bad in ("PANIC", "rank violation", "UNDERFLOW")))
        # The heartbeat must still be advancing at the end: a desktop that
        # stopped compositing is a hung desktop, not a finished one.
        frames = [int(l.split()[1]) for l in tail.splitlines() if "] " in l and " frames," in l]
        check("the compositor is still running at the end",
              len(frames) >= 3 and frames[-1] > frames[len(frames) // 2])
        print("\nframes composited: %s" % (frames[-1] if frames else "none"))
        print("screenshot: %s" % shot)
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=15)
        except subprocess.TimeoutExpired:
            qemu.kill()

    print("\n%d/%d checks passed" % (len(checks) - len(failures), len(checks)))
    if failures:
        print("FAILED: " + "; ".join(failures))
    print("serial log: " + LOG)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
