/* OutRun Media — plays a frame stream into a double-buffered window.
 *
 * A "frame stream" here is what OUTRUN SNAP produces: a set of 24-bit BMP
 * files in the VFS whose names sort in capture order. That is a deliberate
 * choice over a private container. The two programs already have to agree on a
 * pixel format; making that agreement an ordinary file format means a captured
 * desktop can be inspected by a host tool as well as replayed here, and means
 * this player has a real workload on a stock boot rather than needing content
 * shipped with it.
 *
 * THE WINDOW IS PAIRED (SYS_WIN_CREATE a2=1). Every frame is composed entirely
 * into the back buffer and then published by SYS_WIN_DAMAGE, which returns the
 * next buffer to draw into; the compositor only ever reads the published one.
 * That is what makes an animation tear-free here rather than merely usually
 * fine: a player that drew into a live surface would be racing the compositor
 * on every frame instead of on the occasional one.
 *
 * TIMING ADVANCES BY AT MOST ONE FRAME PER TICK. If the process is descheduled
 * for a second, catching up would run the whole clip inside a single repaint
 * and the viewer would see only its last frame — a dropped-frame policy that
 * looks like a decode failure. Falling behind in real time is the honest
 * behaviour for a player with no audio to stay in sync with. */
#include "../include/outrun_abi.h"
#include "bmp.h"

#define MEDIA_MAXFRAME 32
#define MEDIA_NAMELEN 64
#define MEDIA_FPS_MIN 1
#define MEDIA_FPS_MAX 60

struct media_fit { int x, y, w, h; };
struct media_state {
    char name[MEDIA_MAXFRAME][MEDIA_NAMELEN];
    unsigned len[MEDIA_MAXFRAME];
    unsigned long long last_ns;
    int count, frame, playing, fps, truncated, have_clock;
};

static int media_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int media_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
/* Lexicographic, unsigned. The names are fixed-width by construction (see
 * snap_name), so lexicographic order IS capture order — which is the whole
 * reason that generator pads to four digits instead of widening. */
static int media_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static void media_reset(struct media_state *m) {
    unsigned char *z = (unsigned char *)m;
    for (unsigned long i = 0; i < sizeof *m; i++) z[i] = 0;
    m->fps = 4;
}
/* Accepts a name only if it is a frame: the right prefix, the right suffix,
 * and a file long enough to hold a header. Filtering here rather than at
 * decode time is what stops the playlist filling with entries that each fail
 * individually during playback — a clip that stutters once per unrelated file
 * in the directory. */
static int media_add(struct media_state *m, const char *name, unsigned len) {
    if (!name || !name[0]) return -1;
    if (len < BMP_HEADER_BYTES) return -1;
    int n = media_strlen(name);
    if (n < 5 || n >= MEDIA_NAMELEN) return -1;
    if (!media_streq(name + n - 4, ".bmp")) return -1;
    if (m->count >= MEDIA_MAXFRAME) { m->truncated = 1; return -1; }
    int slot = m->count++;
    for (int i = 0; i <= n; i++) m->name[slot][i] = name[i];
    m->len[slot] = len;
    return 0;
}
/* Insertion sort on at most 32 names: the dirent table hands entries over in
 * slot order, which has nothing to do with the order they were written. */
static void media_sort(struct media_state *m) {
    for (int i = 1; i < m->count; i++) {
        char keyname[MEDIA_NAMELEN];
        unsigned keylen = m->len[i];
        for (int c = 0; c < MEDIA_NAMELEN; c++) keyname[c] = m->name[i][c];
        int j = i - 1;
        while (j >= 0 && media_strcmp(m->name[j], keyname) > 0) {
            for (int c = 0; c < MEDIA_NAMELEN; c++) m->name[j + 1][c] = m->name[j][c];
            m->len[j + 1] = m->len[j];
            j--;
        }
        for (int c = 0; c < MEDIA_NAMELEN; c++) m->name[j + 1][c] = keyname[c];
        m->len[j + 1] = keylen;
    }
}
/* Refused, not clamped: a control that silently substituted a supported rate
 * could not tell the caller whether the rate it asked for was honoured. */
static int media_set_fps(struct media_state *m, int fps) {
    if (fps < MEDIA_FPS_MIN || fps > MEDIA_FPS_MAX) return -1;
    m->fps = fps;
    return 0;
}
/* Returns 1 if the frame changed. */
static int media_tick(struct media_state *m, unsigned long long now_ns) {
    if (!m->have_clock || now_ns < m->last_ns) {
        /* The FIRST tick only establishes the clock, and a clock that went
         * backwards re-establishes it. Either way no frame advances: a player
         * started at an arbitrary uptime would otherwise jump on its first
         * repaint by however long the machine had been running. */
        m->last_ns = now_ns;
        m->have_clock = 1;
        return 0;
    }
    if (!m->playing || m->count <= 0 || m->fps <= 0) { m->last_ns = now_ns; return 0; }
    unsigned long long period = 1000000000ull / (unsigned long long)m->fps;
    if (now_ns - m->last_ns < period) return 0;
    /* ONE frame, and the clock is reset to now rather than advanced by one
     * period — see the file header on why catching up is the wrong policy. */
    m->last_ns = now_ns;
    m->frame = (m->frame + 1) % m->count;
    return 1;
}
/* The largest rectangle with the source's aspect ratio that fits in the
 * destination, centred. Integer throughout; the kernel and these programs both
 * build without SSE. */
static void media_fit(struct media_fit *f, int sw, int sh, int dw, int dh) {
    f->x = f->y = f->w = f->h = 0;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    /* Compare sw/sh against dw/dh by cross-multiplying: no division, and no
     * rounding to decide the orientation before the sizes are computed. */
    if ((long long)sw * dh >= (long long)dw * sh) {
        f->w = dw;
        f->h = (int)((long long)dw * sh / sw);
        if (f->h < 1) f->h = 1;
    } else {
        f->h = dh;
        f->w = (int)((long long)dh * sw / sh);
        if (f->w < 1) f->w = 1;
    }
    if (f->w > dw) f->w = dw;
    if (f->h > dh) f->h = dh;
    f->x = (dw - f->w) / 2;
    f->y = (dh - f->h) / 2;
}
/* Nearest-neighbour: destination column dx of dw maps back to a source column.
 * The +0 offset and the truncating divide together guarantee 0 -> 0 and
 * dw-1 -> at most sw-1, which is what keeps the sampler inside the image for
 * every combination of sizes rather than for the ones that were tried. */
static int media_src_index(int dx, int dw, int sw) {
    if (dw <= 0 || sw <= 0) return 0;
    int s = (int)((long long)dx * sw / dw);
    if (s < 0) s = 0;
    if (s >= sw) s = sw - 1;
    return s;
}

#ifndef APP_HOST_TEST
#include "gui.h"

/* One decoded frame. 1024x768 at 24 bits is 2.25 MiB; this is the single
 * largest allocation in the application suite and it is why the player holds
 * exactly ONE frame rather than pre-decoding the clip. */
#define MEDIA_MAXBYTES (BMP_HEADER_BYTES + 1024 * 3 * 768)
static unsigned char framebuf[MEDIA_MAXBYTES];
static struct media_state media;
static int loaded = -1;          /* which playlist index framebuf holds        */
static int fw, fh;
static unsigned foff;
static const char *status = "SCANNING THE VOLUME FOR FRAMES";

/* EVERY LOAD SAYS WHAT IT DID, on the serial console, for the same reason
 * OUTRUN SNAP does: the player's only other account of itself is a status
 * string on a surface, so "the frame decoded", "the file was not a BMP" and
 * "the player never got that far" are otherwise indistinguishable from
 * outside — and a harness that could only count colours would be guessing
 * between them. */
static void media_say(const char *name, int fw2, int fh2, const char *outcome) {
    char line[160];
    int at = 0;
    const char *p = "[media  ] ";
    while (*p) line[at++] = *p++;
    for (p = name; *p && at < 80; ++p) line[at++] = *p;
    line[at++] = ' ';
    int dims[2] = { fw2, fh2 };
    for (int i = 0; i < 2; i++) {
        char d[12];
        int n = 0, v = dims[i] < 0 ? 0 : dims[i];
        do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) line[at++] = d[--n];
        line[at++] = i ? ' ' : 'x';
    }
    for (p = outcome; *p && at < (int)sizeof line - 2; ++p) line[at++] = *p;
    line[at++] = '\n';
    line[at] = 0;
    sysc(SYS_WRITE, (u64)line, 0, 0);
}

static void media_scan(void) {
    int keep = media.fps, was_playing = media.playing;
    media_reset(&media);
    media.fps = keep;
    media.playing = was_playing;
    for (int i = 0; i < 96; i++) {
        struct { u32 len, used; char name[64]; } e;
        if ((i64)sysc(SYS_READDIR, (u64)i, (u64)&e, 0) <= 0) break;
        if (!e.used) continue;
        e.name[63] = 0;
        media_add(&media, e.name, e.len);
    }
    media_sort(&media);
    loaded = -1;
    if (!media.count)
        status = "NO .bmp FRAMES ON THE VOLUME — TAKE ONE WITH OUTRUN SNAP";
    else if (media.truncated)
        status = "PLAYLIST FULL: ONLY THE FIRST 32 FRAMES ARE LISTED";
    else
        status = "READY";
}
/* Loads playlist entry `i` into framebuf. Returns 0, or -1 with `status` set
 * to the reason — a frame that will not decode is named, because a player that
 * showed the previous frame again would look like a stall. */
static int media_load(int i) {
    if (i < 0 || i >= media.count) return -1;
    if (media.len[i] > MEDIA_MAXBYTES) {
        status = "FRAME LARGER THAN THIS PLAYER'S BUFFER";
        media_say(media.name[i], 0, 0, status); return -1;
    }
    i64 fd = (i64)sysc(SYS_OPEN, (u64)media.name[i], 0, 0);
    if (fd < 0) { status = "COULD NOT OPEN FRAME"; media_say(media.name[i], 0, 0, status); return -1; }
    unsigned got = 0;
    while (got < media.len[i]) {
        i64 r = (i64)sysc(SYS_READ, (u64)fd, (u64)(framebuf + got), media.len[i] - got);
        if (r <= 0) break;
        got += (unsigned)r;
    }
    sysc(SYS_CLOSE, (u64)fd, 0, 0);
    if (got < media.len[i]) {
        status = "SHORT READ: FRAME IS TRUNCATED ON THE VOLUME";
        media_say(media.name[i], 0, 0, status); return -1;
    }
    if (bmp_parse(framebuf, got, &fw, &fh, &foff) < 0) {
        status = "NOT A 24-BIT UNCOMPRESSED BMP";
        media_say(media.name[i], 0, 0, status);
        return -1;
    }
    loaded = i;
    status = "PLAYING";
    media_say(media.name[i], fw, fh, "DECODED");
    return 0;
}
static void media_render(struct app_win *w) {
    app_fill(w, 0x05060a);
    int top = 24, bottom = 40;
    int vw = w->cw, vh = w->ch - top - bottom;
    if (loaded >= 0 && vh > 0) {
        struct media_fit f;
        media_fit(&f, fw, fh, vw, vh);
        /* Written STRAIGHT INTO THE BACK SURFACE, not through app_rect.
         *
         * A 430x300 frame is 129,000 pixels, and one clipped rectangle call
         * each — with its bounds arithmetic and its inner loops — is roughly a
         * hundred times the work of a store, on a machine emulated by TCG. The
         * first version of this loop did exactly that and would have spent
         * longer painting one frame than the compositor spends on a hundred.
         *
         * The bounds are established ONCE, by media_fit, which is why this can
         * skip the per-pixel clip that app_rect exists to provide: f.x/f.y and
         * f.w/f.h are guaranteed inside the content rectangle, and that
         * guarantee is what the host tests assert for every size combination
         * rather than for the ones that were tried by hand. */
        for (int y = 0; y < f.h; y++) {
            int sy = media_src_index(y, f.h, fh);
            const unsigned char *srow = framebuf + foff
                                      + (unsigned long long)bmp_stride(fw) * (unsigned)(fh - 1 - sy);
            volatile u32 *drow = w->surf + (u64)(top + f.y + y) * (u64)w->cw + f.x;
            for (int x = 0; x < f.w; x++) {
                const unsigned char *p = srow + (unsigned)media_src_index(x, f.w, fw) * 3u;
                drow[x] = ((u32)p[2] << 16) | ((u32)p[1] << 8) | (u32)p[0];
            }
        }
    } else {
        app_str(w, 8, top + 20, "NO FRAME LOADED", 0x7c8ca0);
    }
    app_rect(w, 0, 0, w->cw, 22, 0x0a0d14);
    app_rect(w, 8, 2, 60, 18, 0x1c2636);
    app_str(w, 14, 7, media.playing ? "PAUSE" : "PLAY", 0x22e4ff);
    app_rect(w, 76, 2, 40, 18, 0x1c2636);
    app_str(w, 86, 7, "<<", 0x22e4ff);
    app_rect(w, 124, 2, 40, 18, 0x1c2636);
    app_str(w, 134, 7, ">>", 0x22e4ff);
    app_rect(w, 172, 2, 60, 18, 0x1c2636);
    app_str(w, 178, 7, "RESCAN", 0x22e4ff);
    app_u32(w, 248, 7, (u32)(media.count ? media.frame + 1 : 0), w->fg);
    app_str(w, 272, 7, "/", 0x7c8ca0);
    app_u32(w, 284, 7, (u32)media.count, w->fg);
    app_u32(w, 332, 7, (u32)media.fps, 0x3df5c4);
    app_str(w, 356, 7, "FPS", 0x7c8ca0);
    app_rect(w, 0, w->ch - bottom, w->cw, bottom, 0x0a0d14);
    if (loaded >= 0) {
        app_str(w, 8, w->ch - 34, media.name[loaded], 0xeaf2f7);
        app_u32(w, 232, w->ch - 34, (u32)fw, 0x7c8ca0);
        app_str(w, 272, w->ch - 34, "X", 0x7c8ca0);
        app_u32(w, 288, w->ch - 34, (u32)fh, 0x7c8ca0);
    }
    app_str(w, 8, w->ch - 18, status, 0xffb020);
    app_present(w);
}

void _start(void) {
    struct app_win w;
    if (app_create(&w, 430, 340, 0xc4a6ff)) app_exit(1);
    app_title(&w, "OUTRUN MEDIA");
    media_reset(&media);
    media_scan();
    if (media.count) media_load(0);
    media_render(&w);
    for (;;) {
        struct outrun_desktop_info clock;
        int dirty = 0;
        if ((i64)sysc(SYS_DESKTOP_INFO, (u64)&clock, sizeof clock, 0) >= 0 &&
            media_tick(&media, clock.wall_ns))
            dirty = 1;
        if (media.count && loaded != media.frame && media_load(media.frame) == 0) dirty = 1;
        struct outrun_event e;
        int rc;
        while ((rc = app_poll(&w, &e)) > 0) {
            if (e.type == EVENT_MOUSE_DOWN && e.y < 22) {
                if (e.x < 68) media.playing = !media.playing;
                else if (e.x < 116) { if (media.count) media.frame = (media.frame + media.count - 1) % media.count; }
                else if (e.x < 164) { if (media.count) media.frame = (media.frame + 1) % media.count; }
                else if (e.x < 232) { media_scan(); if (media.count) media_load(0); }
                dirty = 1;
            } else if (e.type == EVENT_KEY_PRESS) {
                if (e.code == ' ') media.playing = !media.playing;
                if (e.code == 'r' || e.code == 'R') { media_scan(); if (media.count) media_load(0); }
                if (e.code == '+' || e.code == '=') media_set_fps(&media, media.fps + 1);
                if (e.code == '-') media_set_fps(&media, media.fps - 1);
                dirty = 1;
            }
        }
        if (rc < 0) app_exit(0);
        if (dirty) media_render(&w);
        app_idle();
    }
}
#endif
