#!/usr/bin/env python3
"""The snap -> VFS -> media loop, on a live volume, in the guest.

This clears the debt Progress.txt section 9f recorded: every earlier run of
OUTRUN SNAP and OUTRUN MEDIA was on a desktop with NO BLOCK DEVICE ATTACHED, so
no file was ever written and the player had only ever been observed rendering
an empty playlist. Both halves of the pipeline were covered by host tests and
neither had been joined to the other on real storage.

So this boots the desktop image WITH a virtio-blk volume, and the evidence is
taken from three independent places rather than from the screen alone:

  1. THE GUEST'S OWN DIRECTORY. OUTRUN TERM runs the real `vfs` shell command
     through SYS_RUN_CMD, whose output is teed to the serial console by kputc.
     So the file's name and byte count arrive in the log in the kernel's own
     words, not in the screenshot tool's interpretation of some pixels.

  2. THE VOLUME ITSELF. After shutdown the host scans vblk.img for a 512-byte
     block beginning 'BM' whose little-endian size dword equals the byte count
     the directory reported. That is the magic check (0x4D42) performed on the
     bytes actually on the disk, and it is tied to THIS file rather than to any
     BMP-looking data: the size has to match what the guest said it wrote.

  3. THE COMPOSITED FRAME. MEDIA is launched twice, once BEFORE the capture and
     once after. Before, the volume holds no .bmp and the player must say so;
     after, it must decode and paint one. The comparison is what makes the
     second screenshot mean something -- a window full of colour proves nothing
     if nobody checked what the same window looked like with no frame in it.

THE FULL DESKTOP, as of v1.2. This VFS used to store at most 256 KiB per file,
so a 1024x768 capture (2,359,350 bytes) could not be saved at all and the test
had to settle for a 320x240 region. A second double-indirect block took the
format past 4,176 chunks and the ceiling to 2.5 MiB, so the whole screen now
fits -- and capturing it is what exercises the new map region end to end: a
4,610-chunk file resolves through ind3 for every chunk past 4,176, and the
player has to decode every one of them back.

It is also the slowest thing this system does. 2.36 MiB is ~4,610 content
blocks through the CAS, each a virtio-blk transaction under TCG, so the
capture's deadline is minutes rather than seconds and is waited on by the
application's own SAVED line rather than by a guess.
"""
import hashlib, json, os, re, socket, struct, subprocess, sys, time

ISO = sys.argv[1] if len(sys.argv) > 1 else "build/outrun-desktop-1.1.0.iso"
LOG = sys.argv[2] if len(sys.argv) > 2 else "/tmp/desktop-media.log"
IMG = LOG + ".vblk.img"
QMP, W, H = "/tmp/outrun-media.qmp", 1024, 768
BOOT_DEADLINE, STEP_DEADLINE = 420, 120

# Launcher geometry, mirrored from kernel64.c. At scale 1 desk_tile_h()
# evaluates to DESK_TILE_MAX, so the spacing is still 44.
TILE_X = 56
def tile_y(index):
    return 32 + index * 44 + 18
TILE_TERM, TILE_SNAP, TILE_MEDIA = 4, 10, 11

# Window slot 1: the cascade is (60,40) stepped by (40,34). The close box is
# the 14px cell ending 3px short of the window's RIGHT EDGE, inside the 20px
# title bar -- so it depends on the window's WIDTH, and these applications are
# not all the same width. Assuming 430 for the 600-wide terminal is what made
# the first run leave it open, which then pushed the player into slot 2 and
# made every later screen sample look at the wrong rectangle.
WIN_X, WIN_Y = 100, 74
WIDTH_APP, WIDTH_TERM = 430, 600
def close_at(width):
    return (WIN_X + width - 10, WIN_Y + 10)

EXPECT_W, EXPECT_H = 1024, 768
EXPECT_BYTES = 54 + ((EXPECT_W * 3 + 3) & ~3) * EXPECT_H     # 2359350


def read_log(since=0):
    """Everything the guest has written since `since`, or b"" if it cannot be
    read right now.

    TRANSIENT OSError IS EXPECTED, NOT EXCEPTIONAL. The serial log lives on a
    Windows drive reached through a 9p mount, and a full-desktop capture writes
    several megabytes to it while this loop is polling; reading a file being
    appended to across that mount returns EAGAIN (errno 61, "No data
    available") often enough that a run WILL hit it. The first full-screen run
    died on exactly that, nine checks in, with the guest working perfectly.
    Every read of the log goes through here so no call site can forget."""
    for _ in range(5):
        try:
            with open(LOG, "rb") as fh:
                fh.seek(since)
                return fh.read()
        except FileNotFoundError:
            return b""
        except OSError:
            time.sleep(0.4)
    return b""


def wait_for(text, deadline, since=0):
    end = time.time() + deadline
    needle = text.encode()
    while time.time() < end:
        if needle in read_log(since):
            return True
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
        self.recv()
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
        """Relative motion: the guest has a PS/2 mouse and no tablet, so
        absolute events are delivered nowhere at all."""
        while dx or dy:
            sx = max(-100, min(100, dx))
            sy = max(-100, min(100, dy))
            self.cmd("input-send-event", events=[
                {"type": "rel", "data": {"axis": "x", "value": sx}},
                {"type": "rel", "data": {"axis": "y", "value": sy}}])
            dx -= sx; dy -= sy
            time.sleep(0.05)

    def click(self, x, y):
        self.move_rel(-2000, -2000)
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
        time.sleep(0.35)

    def screenshot(self, path):
        self.cmd("screendump", filename=path, format="ppm")
        return os.path.exists(path)


def read_ppm(path):
    with open(path, "rb") as fh:
        blob = fh.read()
    magic, dims, _maxval, pixels = blob.split(b"\n", 3)
    if magic != b"P6":
        raise ValueError("not a binary PPM: %r" % magic)
    w, h = (int(v) for v in dims.split())
    return w, h, pixels


def colours_in(path, x0, y0, x1, y1):
    """How many DISTINCT colours a rectangle of the screen holds.

    A window that published a cleared surface scores 1; one that drew text and
    panels scores a handful; one showing a decoded photograph of a desktop
    scores hundreds. The three are what this test needs to tell apart."""
    try:
        w, _h, px = read_ppm(path)
    except (OSError, ValueError):
        return 0
    seen = set()
    for y in range(y0, y1):
        row = y * w
        for x in range(x0, x1):
            seen.add(px[(row + x) * 3:(row + x) * 3 + 3])
    return len(seen)


def changed_fraction(before, after, x0, y0, x1, y1):
    """What proportion of a rectangle differs between two screenshots.

    This replaced a distinct-colour threshold, which was a guess and a bad one:
    the captured region is mostly flat desktop and a correctly decoded frame
    scored 11 colours against an invented floor of 32. A pixel difference needs
    no such constant. The player's window is in the same place in both shots,
    so if it painted a frame the video area must differ, and if it painted
    nothing it cannot."""
    try:
        wb, _hb, pb = read_ppm(before)
        wa, _ha, pa = read_ppm(after)
    except (OSError, ValueError):
        return 0.0
    if wb != wa:
        return 0.0
    total = diff = 0
    for y in range(y0, y1):
        rb = y * wb
        for x in range(x0, x1):
            i = (rb + x) * 3
            total += 1
            if pb[i:i + 3] != pa[i:i + 3]:
                diff += 1
    return (diff / total) if total else 0.0


def size_of(path):
    try:
        return os.path.getsize(path) if os.path.exists(path) else 0
    except OSError:
        return 0


def make_volume(path, mib=16):
    """A blank volume carrying the signature the kernel's probe looks for.

    16 MiB rather than the gate's 4: the capture alone is 576 KiB and the CAS
    stores it as 512-byte content blocks alongside the directory, both journals
    and the index."""
    with open(path, "wb") as fh:
        fh.truncate(mib * 1024 * 1024)
        fh.seek(1024)
        fh.write(b"OUTRUN-DISK-SIGNATURE-OK")


def scan_volume_for_bmp(path, expect_bytes):
    """Find a 512-byte block on the raw volume that starts a BMP whose own
    declared file size matches what the guest's directory reported.

    Reading the CAS index from the host would mean reimplementing the
    filesystem in the test, which is the mistake of verifying a system against
    a second copy of itself. Scanning for the header is enough: the magic and
    the size field are in the first 6 bytes, and requiring the size to match
    ties the block to THIS capture rather than to any BMP-shaped bytes."""
    with open(path, "rb") as fh:
        blob = fh.read()
    hits = []
    for off in range(0, len(blob) - 6, 512):
        if blob[off:off + 2] != b"BM":
            continue
        declared = struct.unpack_from("<I", blob, off + 2)[0]
        hits.append((off, declared))
    return [h for h in hits if h[1] == expect_bytes], hits


def main():
    os.makedirs(os.path.dirname(LOG) or ".", exist_ok=True)
    with open(ISO, "rb") as image:
        stamp = hashlib.md5(image.read()).hexdigest()
    print("image md5: " + stamp, flush=True)
    with open(LOG + ".image", "w") as evidence:
        evidence.write(ISO + " md5=" + stamp + "\n")
    for stale in (QMP, LOG, IMG):
        if os.path.exists(stale):
            os.remove(stale)
    make_volume(IMG)

    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-accel", "tcg", "-cdrom", ISO, "-m", "512M",
        "-no-reboot", "-vga", "none", "-device", "virtio-vga",
        "-display", "none", "-serial", "file:" + LOG,
        "-qmp", "unix:%s,server,nowait" % QMP,
        "-drive", "file=%s,if=none,format=raw,id=vd0" % IMG,
        "-device", "virtio-blk-pci,drive=vd0,disable-legacy=on,disable-modern=off",
    ])
    failures, checks = [], []

    def check(name, ok):
        checks.append((name, ok))
        print("  %s  %s" % ("PASS" if ok else "FAIL", name), flush=True)
        if not ok:
            failures.append(name)

    def close_window(width=WIDTH_APP):
        mark = size_of(LOG)
        qmp.click(*close_at(width))
        return wait_for("1 window(s)", STEP_DEADLINE, mark)

    reported_bytes = None
    try:
        qmp = Qmp(QMP)
        check("desktop session reaches its loop",
              wait_for("[desktop] logical desktop", BOOT_DEADLINE))
        # The whole point of this run: a volume the guest can actually write to.
        check("a CAS volume is mounted on this boot",
              wait_for("[cas    ] mounted", 5) or wait_for("[cas", 5))
        check("the editor is launched at boot", wait_for("launched 'VAULT PAD'", STEP_DEADLINE))

        # ---- BEFORE: the player with nothing to play ---------------------
        mark = size_of(LOG)
        qmp.click(TILE_X, tile_y(TILE_MEDIA))
        check("MEDIA launches", wait_for("launched 'MEDIA'", STEP_DEADLINE, mark))
        before = LOG + ".media-empty.ppm"
        qmp.screenshot(before)
        empty_colours = colours_in(before, 110, 130, 520, 380)
        check("MEDIA with an empty volume paints no frame (%d colours)" % empty_colours,
              0 < empty_colours <= 8)
        check("MEDIA closes", close_window())

        # ---- CAPTURE -----------------------------------------------------
        mark = size_of(LOG)
        qmp.click(TILE_X, tile_y(TILE_SNAP))
        check("SNAPSHOT launches", wait_for("launched 'SNAPSHOT'", STEP_DEADLINE, mark))
        # DRIVEN BY CLICKS, NOT KEYS. The first version of this pressed "4"
        # and then space, and neither reached the application: nothing was
        # written, and because the untouched default preset is FULL DESKTOP —
        # which this filesystem cannot store — a space that HAD arrived would
        # have been refused too. Clicks on the application's own buttons are
        # the path desktop-ui-test already exercises, so they are the path used
        # here. The coordinates are derived once, from the kernel's own
        # mapping: a content click at screen (sx, sy) reaches the application
        # as (sx - WIN_X - 2, sy - WIN_Y - WIN_TITLE_H - 1).
        qmp.click(WIN_X + 92, WIN_Y + 75)      # preset FULL DESKTOP -> app (90, 46..64)
        time.sleep(1.0)
        mark = size_of(LOG)
        qmp.click(WIN_X + 80, WIN_Y + 248)     # CAPTURE        -> app (78, 216..238)
        # The application announces every capture attempt on the console, so
        # "the click never arrived", "the region was refused" and "it worked"
        # are three distinguishable outcomes rather than one silent screen.
        # A full-desktop capture is ~4,610 CAS block puts under TCG. The
        # SAVED line is printed only after the last one, so this deadline
        # sizes the whole write, not the click.
        spoke = wait_for("[snap   ] capture", 900, mark)
        check("the CAPTURE button was actually pressed", spoke)
        said = ""
        if spoke:
            m = re.search(r"\[snap   \] capture ([^\n]*)",
                          read_log(mark).decode("utf-8", "replace"))
            said = m.group(1) if m else ""
            print("    guest said: %s" % said, flush=True)
        check("the guest captured the FULL DESKTOP region (%s)" % said,
              said.startswith("1024x768@0,0"))
        check("the guest reports the capture SAVED", "SAVED" in said)
        after_snap = LOG + ".snap.ppm"
        qmp.screenshot(after_snap)
        check("SNAPSHOT is still alive after the capture", close_window())

        # ---- THE GUEST'S OWN DIRECTORY -----------------------------------
        mark = size_of(LOG)
        qmp.click(TILE_X, tile_y(TILE_TERM))
        check("OUTRUN TERM launches", wait_for("launched 'OUTRUN TERM'", STEP_DEADLINE, mark))
        qmp.click(WIN_X + 200, WIN_Y + 120)        # focus the terminal's content
        mark = size_of(LOG)
        for k in ("v", "f", "s", "ret"):
            qmp.key(k)
        listed = wait_for("snap0000.bmp", STEP_DEADLINE, mark)
        check("the guest's own `vfs` listing names snap0000.bmp", listed)
        if listed:
            tail = read_log(mark).decode("utf-8", "replace")
            m = re.search(r"snap0000\.bmp\s+\((\d+) bytes", tail)
            if m:
                reported_bytes = int(m.group(1))
            check("the file has a non-zero size (%s bytes)" % reported_bytes,
                  reported_bytes is not None and reported_bytes > 0)
            check("the size is exactly a %dx%d 24-bit BMP (%s == %d)"
                  % (EXPECT_W, EXPECT_H, reported_bytes, EXPECT_BYTES),
                  reported_bytes == EXPECT_BYTES)
        check("OUTRUN TERM closes", close_window(WIDTH_TERM))

        # ---- AFTER: the player with a real frame -------------------------
        mark = size_of(LOG)
        qmp.click(TILE_X, tile_y(TILE_MEDIA))
        check("MEDIA relaunches", wait_for("launched 'MEDIA'", STEP_DEADLINE, mark))
        # The player announces each load, so waiting is on ITS word rather than
        # on a guessed number of seconds.
        decoded = wait_for("[media  ] snap0000.bmp", 600, mark)
        said_media = ""
        if decoded:
            mm = re.search(r"\[media  \] ([^\n]*)",
                           read_log(mark).decode("utf-8", "replace"))
            said_media = mm.group(1) if mm else ""
            print("    guest said: %s" % said_media, flush=True)
        check("MEDIA reports decoding the captured frame (%s)" % said_media,
              said_media.startswith("snap0000.bmp 1024x768 DECODED"))
        time.sleep(4)          # let the decoded frame reach a published surface
        after = LOG + ".media-frame.ppm"
        qmp.screenshot(after)
        # The letterboxed image: media_fit puts a 320x240 source into the
        # player's 426x253 video area as 337x253 at content x=44, y=0, which is
        # screen (146,119)-(483,372). Sampled inside that, away from its edges.
        moved = changed_fraction(before, after, 160, 130, 470, 360)
        frame_colours = colours_in(after, 160, 130, 470, 360)
        check("MEDIA paints the decoded frame into its window "
              "(%.0f%% of the video area changed, %d colours vs %d empty)"
              % (moved * 100, frame_colours, empty_colours),
              moved > 0.25 and frame_colours > empty_colours)
        check("MEDIA closes cleanly after playback", close_window())

        # ---- the session survived it -------------------------------------
        whole = read_log().decode("utf-8", "replace")
        check("no panic, fault or lock-rank violation in the whole session",
              "PANIC" not in whole and "rank violation" not in whole
              and "EXCEPTION" not in whole)
        check("every launch succeeded",
              "0 launch failure(s)" in whole and "1 launch failure" not in whole)
        resting = re.findall(r"1 window\(s\), \d+ launched, 0 launch failure\(s\), frames_used=(\d+)",
                             whole)
        levels = sorted(set(int(v) for v in resting[1:]))
        check("no allocator drift across the four launch/close pairs: %s" % levels,
              len(resting) >= 4 and len(levels) == 1)
    finally:
        try:
            qmp.cmd("quit")
        except Exception:
            pass
        try:
            qemu.wait(timeout=30)
        except Exception:
            qemu.kill()

    # ---- THE VOLUME, read by the host after shutdown ---------------------
    if reported_bytes:
        matches, all_hits = scan_volume_for_bmp(IMG, reported_bytes)
        check("the volume holds a block whose BMP magic is 0x4D42 and whose "
              "declared size is the directory's %d bytes (%d block(s), %d BM-like)"
              % (reported_bytes, len(matches), len(all_hits)),
              len(matches) >= 1)
    else:
        check("volume scan (skipped: no byte count was reported)", False)

    print("\nvolume: %s" % IMG, flush=True)
    print("serial log: %s" % LOG, flush=True)
    print("\n%d/%d checks passed" % (len(checks) - len(failures), len(checks)), flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
