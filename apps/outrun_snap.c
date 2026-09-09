/* OutRun Snap — capture a region of the desktop and save it to the VFS.
 *
 * SYS_FB_CAPTURE copies from the composited LOGICAL desktop, which is the same
 * coordinate space windows and the pointer live in, so what is asked for and
 * what is saved are the same rectangle. It REFUSES a rectangle that is not
 * wholly on screen rather than clamping it, and so does this program: a file
 * that quietly contains a different region than the one requested could never
 * afterwards be attributed to a request.
 *
 * THE SCREEN IS READ IN STRIPS. A full 1024x768 desktop is three megabytes of
 * pixels; a ring-3 program that reserved that as BSS to take an occasional
 * screenshot would be paying for it on every boot. The capture walks the
 * rectangle bottom-up in horizontal strips — which is the order the rows go
 * into a BMP anyway — encoding each into the output buffer. The strip plan is
 * exercised on the host for every height from 1 to 200: strips that overlapped
 * would duplicate rows, and a gap would produce a file shorter than its own
 * header claims.
 *
 * WHAT CAN ACTUALLY BE SAVED IS SMALL. This kernel's VFS stores at most 256
 * KiB per file, so of the four regions offered only the fixed 320x240 CENTRE
 * one fits; the others are refused, and the screen says TOO LARGE beside the
 * byte count before the button is pressed rather than after.
 *
 * The output is an ordinary 24-bit BMP (apps/bmp.h), so a captured screen can
 * be opened by a host tool once the volume is extracted, and can be played
 * back by OUTRUN MEDIA without either program owning a private format. */
#include "../include/outrun_abi.h"
#include "bmp.h"

enum { SNAP_FULL = 0, SNAP_LEFT = 1, SNAP_RIGHT = 2, SNAP_CENTRE = 3, SNAP_NPRESET = 4 };
struct snap_rect { int x, y, w, h; };

/* THE LARGEST FILE THIS VFS WILL STORE. Mirrors VFS_MAX_FILE_BYTES in
 * kernel64.c, which is the SOURCE OF TRUTH: the on-disk indirect map could
 * address 2 MiB, but the staging buffer the write path runs through caps a
 * file at 512 chunks, and it is the cap that decides. Deriving this number
 * from the format instead — which the first version of this did — produced an
 * application that believed a 589,878-byte capture would fit, wrote it, and
 * got a silently clamped short write back.
 *
 * The region is refused BEFORE anything is written, because a capture that
 * failed part-way would leave a file whose header describes an image the file
 * does not contain: the exact truncation bmp_parse exists to catch, only
 * manufactured by the writer instead of by a full disk. */
#define SNAP_MAX_FILE_BYTES (256u * 1024u)

/* SYS_WRITE_FILE CLAMPS A SINGLE WRITE TO 64 KiB and returns the shorter
 * count; it does not fail. So a 589,878-byte image handed over in one call
 * writes an eighth of itself and reports success for that eighth — which is
 * exactly what the first live run of the pipeline reported, as a short write
 * the application then blamed on a full volume. The write loops. */
#define SNAP_WRITE_CHUNK 65536u
static unsigned snap_chunk(unsigned total, unsigned done) {
    if (done >= total) return 0u;
    unsigned want = total - done;
    return want > SNAP_WRITE_CHUNK ? SNAP_WRITE_CHUNK : want;
}
static int snap_fits(const struct snap_rect *r) {
    unsigned long long n = bmp_size(r->w, r->h);
    return n != 0ull && n <= (unsigned long long)SNAP_MAX_FILE_BYTES;
}

/* Refused, not clamped — see the file header, and SYS_FB_CAPTURE, which makes
 * the same choice for the same reason. */
static int snap_valid(const struct snap_rect *r, int dw, int dh) {
    if (r->w <= 0 || r->h <= 0) return 0;
    if (r->x < 0 || r->y < 0) return 0;
    if (r->x > dw - r->w || r->y > dh - r->h) return 0;
    return 1;
}
/* The right-hand preset is derived by SUBTRACTION from the desktop edge rather
 * than by doubling the left half: on an odd width the halves differ by a
 * pixel, and a rectangle computed the other way would run one column off the
 * screen and be refused.
 *
 * CENTRE IS A FIXED 320x240 WINDOW, not a fraction of the screen. Every
 * proportional preset is far larger than SNAP_MAX_FILE_BYTES on any desktop
 * this kernel produces — a half-screen capture at 1024x768 is 1.1 MiB against
 * a 256 KiB ceiling — so without one fixed region the application would have
 * had nothing it could actually save. It shrinks to fit a smaller desktop
 * rather than producing a rectangle that would then be refused as off-screen. */
#define SNAP_CENTRE_W 320
#define SNAP_CENTRE_H 240
static void snap_preset(struct snap_rect *r, int which, int dw, int dh) {
    switch (which) {
    case SNAP_LEFT:   r->x = 0;      r->y = 0;      r->w = dw / 2;      r->h = dh; break;
    case SNAP_RIGHT:  r->x = dw / 2; r->y = 0;      r->w = dw - dw / 2; r->h = dh; break;
    case SNAP_CENTRE:
        r->w = dw < SNAP_CENTRE_W ? dw : SNAP_CENTRE_W;
        r->h = dh < SNAP_CENTRE_H ? dh : SNAP_CENTRE_H;
        r->x = (dw - r->w) / 2;
        r->y = (dh - r->h) / 2;
        break;
    default:          r->x = 0;      r->y = 0;      r->w = dw;          r->h = dh; break;
    }
}
/* Strips run BOTTOM-UP: strip 0 is the bottom of the image, because that is
 * the first row a bottom-up BMP stores. The LAST strip is the short one. */
static int snap_strips(int h, int strip) {
    if (h <= 0 || strip <= 0) return 0;
    return (h + strip - 1) / strip;
}
static int snap_strip_top(int h, int strip, int i) {
    int top = h - (i + 1) * strip;
    return top < 0 ? 0 : top;
}
static int snap_strip_rows(int h, int strip, int i) {
    if (i < 0 || i >= snap_strips(h, strip)) return 0;
    return (h - i * strip) - snap_strip_top(h, strip, i);
}
/* snapNNNN.bmp, four digits, wrapping at 10000. Wrapping rather than widening
 * keeps every name the same length, which is what makes them sort in the
 * directory listing in the order they were taken. */
static void snap_name(char *out, unsigned seq) {
    const char *p = "snap";
    int at = 0;
    while (*p) out[at++] = *p++;
    seq %= 10000u;
    out[at++] = (char)('0' + (seq / 1000u) % 10u);
    out[at++] = (char)('0' + (seq / 100u) % 10u);
    out[at++] = (char)('0' + (seq / 10u) % 10u);
    out[at++] = (char)('0' + seq % 10u);
    const char *e = ".bmp";
    while (*e) out[at++] = *e++;
    out[at] = 0;
}

#ifndef APP_HOST_TEST
#include "gui.h"

/* THE CAPTURE is strip-wise; THE WRITE is not, and the two are separate
 * decisions made for opposite reasons.
 *
 * Reading the screen in strips is what keeps the PIXEL buffer small: a full
 * desktop is 3 MiB at four bytes a pixel, and 32 rows of it is 128 KiB.
 *
 * Writing, though, must happen in AS FEW CALLS AS THE SYSCALL PERMITS. Writes
 * on this VFS are not appends into an open extent — every write re-stores the
 * file's whole chunk map through the CAS, so N writes to a growing file cost
 * O(N^2) block puts. The first version issued one write per ROW: 384 writes
 * against a 450-chunk file, which measured at tens of thousands of puts and
 * did not finish inside any deadline this harness has. The encoded image is
 * therefore buffered whole and handed over in 64 KiB chunks, which is the
 * largest a single SYS_WRITE_FILE will accept — four calls, not 384. */
#define SNAP_MAXW 1024
#define SNAP_STRIP 32
static unsigned int capbuf[SNAP_MAXW * SNAP_STRIP];
static unsigned char filebuf[SNAP_MAX_FILE_BYTES];

static struct snap_rect rect;
/* The only region this filesystem can store, so it is the one offered first:
 * a default the CAPTURE button would always refuse is not a default. */
static int preset = SNAP_CENTRE;
static unsigned seq;
static const char *status = "PICK A REGION, THEN CAPTURE";
static char lastfile[24];
static int have_last;

static int snap_write(int fd, const void *buf, unsigned len) {
    return (int)(i64)sysc(SYS_WRITE_FILE, (u64)fd, (u64)buf, len);
}

/* EVERY CAPTURE ATTEMPT SAYS WHAT IT DID, on the serial console.
 *
 * Without this the application's only account of itself is a status string on
 * a surface, which a harness can photograph but not read — so a capture that
 * never happened and a capture that was refused look identical from outside,
 * and a click that missed its button looks like both. The first run of the
 * pipeline test spent two boots unable to distinguish those three, which is
 * the same class of problem as a counter nothing prints. */
static void snap_say(const struct snap_rect *r, unsigned long long bytes, const char *outcome) {
    char line[192];
    int at = 0;
    const char *p = "[snap   ] capture ";
    while (*p) line[at++] = *p++;
    struct { int v; char sep; } parts[] = { {r->w, 'x'}, {r->h, '@'}, {r->x, ','}, {r->y, ' '} };
    for (unsigned i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        char d[12];
        int n = 0, v = parts[i].v;
        if (v < 0) { line[at++] = '-'; v = -v; }
        do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) line[at++] = d[--n];
        line[at++] = parts[i].sep;
    }
    char d[24];
    int n = 0;
    do { d[n++] = (char)('0' + (int)(bytes % 10ull)); bytes /= 10ull; } while (bytes);
    while (n) line[at++] = d[--n];
    p = " bytes: ";
    while (*p) line[at++] = *p++;
    for (p = outcome; *p && at < (int)sizeof line - 2; ++p) line[at++] = *p;
    line[at++] = '\n';
    line[at] = 0;
    sysc(SYS_WRITE, (u64)line, 0, 0);
}

/* Returns 0 on success, or a negative code identifying WHICH step failed —
 * every failure point gets its own number so the status line can name the
 * rule that broke instead of reporting that the capture failed. */
static int snap_capture(void) {
    struct outrun_desktop_info info;
    if ((i64)sysc(SYS_DESKTOP_INFO, (u64)&info, sizeof info, 0) < 0) return -1;
    snap_preset(&rect, preset, (int)info.width, (int)info.height);
    if (!snap_valid(&rect, (int)info.width, (int)info.height)) return -2;
    if (rect.w > SNAP_MAXW) return -3;
    if (!snap_fits(&rect)) return -9;

    /* Encode the WHOLE image first, then write it once. See the note on
     * filebuf for why the write cannot be incremental on this filesystem. */
    if (bmp_header(filebuf, rect.w, rect.h) < 0) return -5;
    unsigned stride = bmp_stride(rect.w);
    int n = snap_strips(rect.h, SNAP_STRIP);
    for (int i = 0; i < n; i++) {
        int rows = snap_strip_rows(rect.h, SNAP_STRIP, i);
        int top = snap_strip_top(rect.h, SNAP_STRIP, i);
        u64 pos = ((u64)(rect.x) << 16) | (u64)(rect.y + top);
        u64 dim = ((u64)rect.w << 16) | (u64)rows;
        if ((i64)sysc(SYS_FB_CAPTURE, (u64)capbuf, pos, dim) < 0) return -7;
        /* Strip i is the i'th block of rows from the BOTTOM of the image, and
         * within it the rows also go out bottom-up — so its first encoded row
         * lands at file row (i * SNAP_STRIP). */
        for (int r = rows - 1; r >= 0; --r) {
            unsigned file_row = (unsigned)(i * SNAP_STRIP) + (unsigned)(rows - 1 - r);
            bmp_row(filebuf + BMP_HEADER_BYTES + (unsigned long long)file_row * stride,
                    &capbuf[(unsigned)r * (unsigned)rect.w], rect.w);
        }
    }
    unsigned total = (unsigned)bmp_size(rect.w, rect.h);

    char name[24];
    snap_name(name, seq);
    /* O_TRUNC is load-bearing, not habit: writes on this kernel are positional
     * and a second write APPENDS, so re-capturing to an existing name would
     * otherwise glue a whole second image onto the end of the first — and the
     * BMP header at the front would still describe only the first. */
    i64 fd = (i64)sysc(SYS_OPEN, (u64)name, OUTRUN_O_CREAT | OUTRUN_O_TRUNC, 0);
    if (fd < 0) return -4;
    /* Writes are positional and the cursor advances, so successive chunks
     * append; O_TRUNC above is what put the cursor at zero. */
    unsigned done = 0, want;
    while ((want = snap_chunk(total, done)) != 0u) {
        if (snap_write((int)fd, filebuf + done, want) != (int)want) {
            sysc(SYS_CLOSE, (u64)fd, 0, 0);
            return -6;
        }
        done += want;
    }
    sysc(SYS_CLOSE, (u64)fd, 0, 0);
    sysc(SYS_VFS_SYNC, 0, 0, 0);
    for (int i = 0; i < 24; i++) lastfile[i] = name[i];
    have_last = 1;
    seq++;
    return 0;
}
static const char *snap_error(int rc) {
    switch (rc) {
    case 0:  return "SAVED";
    case -1: return "SYS_DESKTOP_INFO REFUSED";
    case -2: return "REGION IS NOT WHOLLY ON SCREEN";
    case -3: return "REGION WIDER THAN THIS TOOL'S STRIP BUFFER";
    case -4: return "COULD NOT CREATE THE FILE";
    case -5: return "REGION HAS NO PIXELS";
    case -6: return "SHORT WRITE: VOLUME FULL?";
    case -7: return "SYS_FB_CAPTURE REFUSED THE RECTANGLE";
    case -9: return "REGION EXCEEDS THIS FILESYSTEM'S MAXIMUM FILE SIZE";
    default: return "UNKNOWN FAILURE";
    }
}

static void snap_render(struct app_win *w) {
    static const char *names[SNAP_NPRESET] = { "FULL DESKTOP", "LEFT HALF", "RIGHT HALF", "CENTRE 320x240" };
    struct outrun_desktop_info info;
    int dw = 0, dh = 0;
    if ((i64)sysc(SYS_DESKTOP_INFO, (u64)&info, sizeof info, 0) >= 0) {
        dw = (int)info.width; dh = (int)info.height;
    }
    struct snap_rect r;
    snap_preset(&r, preset, dw, dh);
    app_fill(w, w->bg);
    app_str(w, 8, 8, "OUTRUN SNAP  /  DESKTOP CAPTURE", 0x22e4ff);
    app_str(w, 8, 30, "REGION", 0x7c8ca0);
    for (int i = 0; i < SNAP_NPRESET; i++) {
        int y = 46 + i * 22;
        app_rect(w, 8, y, 180, 18, preset == i ? 0x28495d : 0x1c2636);
        app_str(w, 16, y + 5, names[i], preset == i ? 0x22e4ff : 0x7c8ca0);
    }
    int y = 46 + SNAP_NPRESET * 22 + 10;
    app_str(w, 8, y, "DESKTOP", 0x7c8ca0);
    app_u32(w, 128, y, (u32)dw, w->fg);
    app_str(w, 176, y, "X", 0x7c8ca0);
    app_u32(w, 192, y, (u32)dh, w->fg);
    y += 16;
    app_str(w, 8, y, "RECTANGLE", 0x7c8ca0);
    app_u32(w, 128, y, (u32)r.w, w->fg);
    app_str(w, 176, y, "X", 0x7c8ca0);
    app_u32(w, 192, y, (u32)r.h, w->fg);
    app_str(w, 248, y, "AT", 0x7c8ca0);
    app_u32(w, 272, y, (u32)r.x, w->fg);
    app_str(w, 312, y, ",", 0x7c8ca0);
    app_u32(w, 320, y, (u32)r.y, w->fg);
    y += 16;
    app_str(w, 8, y, "FILE BYTES", 0x7c8ca0);
    app_u32(w, 128, y, (u32)bmp_size(r.w, r.h), snap_fits(&r) ? 0x3df5c4 : 0xff2d9b);
    /* Said BEFORE the button is pressed. A region this filesystem cannot store
     * is refused by snap_capture, and a control that looked available right up
     * to the moment it failed would be the launcher-tile mistake again. */
    if (!snap_fits(&r)) app_str(w, 232, y, "TOO LARGE", 0xff2d9b);
    y += 16;
    app_str(w, 8, y, "STRIPS", 0x7c8ca0);
    app_u32(w, 128, y, (u32)snap_strips(r.h, SNAP_STRIP), w->fg);
    app_str(w, 176, y, "OF", 0x7c8ca0);
    app_u32(w, 200, y, SNAP_STRIP, w->fg);
    app_str(w, 240, y, "ROWS", 0x7c8ca0);
    y += 24;
    app_rect(w, 8, y, 140, 22, 0x1f5d4a);
    app_str(w, 22, y + 7, "CAPTURE", 0xeaf2f7);
    y += 34;
    if (have_last) {
        app_str(w, 8, y, "LAST FILE", 0x7c8ca0);
        app_str(w, 128, y, lastfile, 0x3df5c4);
        y += 16;
        app_str(w, 8, y, "OPEN IT WITH OUTRUN MEDIA", 0x7c8ca0);
    }
    app_str(w, 8, w->ch - 26, status, 0xffb020);
    app_str(w, 8, w->ch - 12, "24-BIT BMP: READABLE OFF THE VOLUME BY HOST TOOLS", 0x7c8ca0);
    app_present(w);
}

void _start(void) {
    struct app_win w;
    if (app_create(&w, 430, 300, 0xff2d9b)) app_exit(1);
    app_title(&w, "OUTRUN SNAP");
    snap_render(&w);
    for (;;) {
        struct outrun_event e;
        int rc, dirty = 0;
        while ((rc = app_poll(&w, &e)) > 0) {
            if (e.type == EVENT_MOUSE_DOWN) {
                int i = (e.y - 46) / 22;
                if (e.y >= 46 && i >= 0 && i < SNAP_NPRESET && e.x < 188) { preset = i; dirty = 1; }
                else if (e.x < 148 && e.y >= 46 + SNAP_NPRESET * 22 + 82 &&
                         e.y < 46 + SNAP_NPRESET * 22 + 104) {
                    int rc2 = snap_capture();
                    status = snap_error(rc2);
                    snap_say(&rect, bmp_size(rect.w, rect.h), status);
                    dirty = 1;
                }
            } else if (e.type == EVENT_KEY_PRESS) {
                if (e.code >= '1' && e.code <= '4') { preset = e.code - '1'; dirty = 1; }
                if (e.code == ' ' || e.code == 10 || e.code == 13) {
                    int rc2 = snap_capture();
                    status = snap_error(rc2);
                    snap_say(&rect, bmp_size(rect.w, rect.h), status);
                    dirty = 1;
                }
            }
        }
        if (rc < 0) app_exit(0);
        if (dirty) snap_render(&w);
        app_idle();
    }
}
#endif
