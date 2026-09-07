/* OutRun Disks — block devices, the CAS pool, and the VFS directory.
 *
 * The data comes from two kernel calls and nothing else: SYS_HW_INFO's
 * HW_STORAGE domain for the superblock and the block-device inventory, and
 * SYS_READDIR for the names. Both are read-only. This application deliberately
 * does NOT drive the `disk` or `cas` shell commands to obtain its figures —
 * those are self-tests that WRITE to the volume, and a monitor whose refresh
 * mutates what it is monitoring is not a monitor.
 *
 * Where hardware is absent the count is zero and the screen says so. There is
 * no NVMe controller in this tree; the DEVICES tab reports what the kernel
 * enumerated rather than a row that looks the same as a real one. */
#include "../include/outrun_abi.h"

#define DISK_MAXENT 96                 /* VFS_MAXFILES: the whole dirent table */
#define DISK_NAMELEN 64                /* VFS_NAME_MAX                         */
#define DISK_MAXGRP 12
enum { DISK_TAB_POOL = 0, DISK_TAB_DEVICES = 1, DISK_TAB_FILES = 2, DISK_NTAB = 3 };

struct disk_entry { char name[DISK_NAMELEN]; unsigned len; };
struct disk_group { char prefix[24]; unsigned files; unsigned long long bytes; };
struct disk_view {
    struct outrun_storage_info info;
    struct disk_entry ent[DISK_MAXENT];
    struct disk_group grp[DISK_MAXGRP];
    unsigned long long ent_bytes;
    int nent, ngrp, tab, top, rows, truncated;
    char note[64];
    const char *status;
};

/* ---- arithmetic ---------------------------------------------------------- */

/* Percent of `total`, saturating at 100, and 0 when there is no denominator.
 * The shift-down loop is not decoration: cas_total_blocks is a 64-bit block
 * count and `used * 100` overflows long before a volume is implausible. */
static unsigned disk_pct(unsigned long long used, unsigned long long total) {
    if (!total) return 0;
    if (used >= total) return 100;
    while (used > (unsigned long long)~0ull / 100) { used >>= 1; total >>= 1; }
    if (!total) return 0;
    return (unsigned)(used * 100 / total);
}
static unsigned long long disk_pool_free(const struct outrun_storage_info *s) {
    return s->cas_used_blocks >= s->cas_total_blocks
         ? 0ull : s->cas_total_blocks - s->cas_used_blocks;
}
static unsigned disk_pool_pct(const struct outrun_storage_info *s) {
    return disk_pct(s->cas_used_blocks, s->cas_total_blocks);
}
static unsigned long long disk_pool_bytes(const struct outrun_storage_info *s) {
    return s->cas_total_blocks * (unsigned long long)s->cas_block_size;
}
static unsigned disk_dedup_pct(const struct outrun_storage_info *s) {
    return disk_pct(s->cas_dedup_hits, s->cas_put_count);
}
static unsigned long long disk_capacity_bytes(const struct outrun_blockdev *d) {
    return d->capacity_sectors * (unsigned long long)d->sector_size;
}

/* Human byte counts with one decimal. Freestanding: no libc, and the kernel
 * builds without SSE, so this is integer arithmetic throughout. */
static void disk_bytes(char *out, unsigned long long b) {
    static const char *unit[] = { "B", "KIB", "MIB", "GIB", "TIB", "PIB" };
    int u = 0;
    unsigned long long scale = 1;
    while (u < 5 && b / scale >= 1024ull) { scale *= 1024ull; u++; }
    unsigned long long whole = b / scale;
    int at = 0;
    char digits[24];
    int n = 0;
    do { digits[n++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    while (n) out[at++] = digits[--n];
    if (u) {
        unsigned long long frac = ((b % scale) * 10ull) / scale;
        out[at++] = '.';
        out[at++] = (char)('0' + frac);
    }
    out[at++] = ' ';
    for (const char *p = unit[u]; *p; ++p) out[at++] = *p;
    out[at] = 0;
}

/* The verdict is ordered by consequence, not by which counter is cheapest to
 * read. An underflow means a block may have been freed while still referenced,
 * which outranks a full pool and a replayed journal both. */
static const char *disk_health(const struct outrun_storage_info *s) {
    if (!s->cas_mounted)          return "CAS NOT MOUNTED";
    if (s->cas_ref_underflow)     return "REFCOUNT UNDERFLOW: DATA AT RISK";
    if (disk_pool_pct(s) > 90)    return "POOL ABOVE 90% USED";
    if (s->cas_recover_replays)   return "JOURNAL REPLAYED ON MOUNT";
    return "HEALTHY";
}

/* ---- the directory listing ----------------------------------------------- */

static int disk_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void disk_reset(struct disk_view *v) {
    v->nent = 0; v->ngrp = 0; v->ent_bytes = 0; v->truncated = 0; v->top = 0;
    v->note[0] = 0;
}
/* Returns 0 if the entry was recorded, -1 if it was not.
 *
 * A refusal is never silent: an empty name is a slot SYS_READDIR filtered out
 * (used == 0) and counting it would inflate every total, while a full table
 * raises `truncated`, which the status line prints. A listing that quietly
 * stopped early is indistinguishable from a volume that really has that many
 * files, which is the whole reason this returns a value at all. */
static int disk_add(struct disk_view *v, const char *name, unsigned len) {
    if (!name || !name[0]) return -1;
    if (v->nent >= DISK_MAXENT) { v->truncated = 1; return -1; }
    struct disk_entry *e = &v->ent[v->nent++];
    int i = 0;
    while (i < DISK_NAMELEN - 1 && name[i]) { e->name[i] = name[i]; i++; }
    e->name[i] = 0;
    e->len = len;
    v->ent_bytes += len;
    return 0;
}
/* First path component, with its trailing slash: "/usr/include/x" -> "/usr/".
 * A name with no second slash — and any name that is not rooted at all —
 * groups under "/". The VFS directory is FLAT; these prefixes are derived from
 * the names, not read from a mount table, because this kernel has no mount
 * table and inventing one would be a screen that does not correspond to
 * anything. */
static void disk_prefix_of(const char *name, char *out, int cap) {
    int at = 0;
    if (name[0] == '/') {
        int i = 1;
        while (name[i] && name[i] != '/') i++;
        if (name[i] == '/') {
            while (at < cap - 1 && at <= i) { out[at] = name[at]; at++; }
            out[at] = 0;
            return;
        }
    }
    (void)cap;
    out[0] = '/'; out[1] = 0;
}
static void disk_group(struct disk_view *v) {
    v->ngrp = 0;
    for (int i = 0; i < v->nent; i++) {
        char pfx[24];
        disk_prefix_of(v->ent[i].name, pfx, (int)sizeof pfx);
        int g = -1;
        for (int j = 0; j < v->ngrp; j++) if (disk_streq(v->grp[j].prefix, pfx)) { g = j; break; }
        if (g < 0) {
            if (v->ngrp >= DISK_MAXGRP - 1) continue;   /* keep room for TOTAL */
            g = v->ngrp++;
            int c = 0;
            while (c < (int)sizeof v->grp[g].prefix - 1 && pfx[c]) { v->grp[g].prefix[c] = pfx[c]; c++; }
            v->grp[g].prefix[c] = 0;
            v->grp[g].files = 0; v->grp[g].bytes = 0;
        }
        v->grp[g].files++;
        v->grp[g].bytes += v->ent[i].len;
    }
    struct disk_group *t = &v->grp[v->ngrp++];
    const char *label = "TOTAL";
    int c = 0;
    while (label[c]) { t->prefix[c] = label[c]; c++; }
    t->prefix[c] = 0;
    t->files = (unsigned)v->nent;
    t->bytes = v->ent_bytes;
}
static void disk_u32str(char *out, int *at, unsigned long long value) {
    char digits[24];
    int n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) out[(*at)++] = digits[--n];
}
static const char *disk_listing_note(struct disk_view *v) {
    if (v->truncated) {
        const char *m = "LISTING TRUNCATED: MORE FILES THAN THIS VIEW HOLDS";
        int i = 0;
        while (m[i] && i < (int)sizeof v->note - 1) { v->note[i] = m[i]; i++; }
        v->note[i] = 0;
        return v->note;
    }
    int at = 0;
    disk_u32str(v->note, &at, (unsigned long long)v->nent);
    const char *tail = " FILES";
    for (int i = 0; tail[i]; i++) v->note[at++] = tail[i];
    v->note[at] = 0;
    return v->note;
}
static void disk_scroll(struct disk_view *v, int dir) {
    int rows = v->rows > 0 ? v->rows : 1;
    int max = v->nent > 0 ? ((v->nent - 1) / rows) * rows : 0;
    v->top += dir * rows;
    if (v->top > max) v->top = max;
    if (v->top < 0) v->top = 0;
}
static void disk_set_tab(struct disk_view *v, int tab) {
    if (tab < 0 || tab >= DISK_NTAB) return;
    v->tab = tab;
    v->top = 0;
}

#ifndef APP_HOST_TEST
#include "gui.h"

static struct disk_view view;
static struct outrun_storage_info fetched;

static void disk_refresh(struct disk_view *v) {
    i64 rc = (i64)sysc(SYS_HW_INFO, HW_STORAGE, (u64)&fetched, sizeof fetched);
    if (rc < 0 || fetched.version != OUTRUN_HW_ABI_VERSION) {
        v->status = "SYS_HW_INFO REFUSED: KERNEL/HEADER MISMATCH";
        return;
    }
    v->info = fetched;
    disk_reset(v);
    /* SYS_READDIR walks the WHOLE table by index and reports a filtered slot
     * as used == 0 rather than ending the walk, so the loop must not stop on
     * the first empty entry. */
    for (int i = 0; i < DISK_MAXENT; i++) {
        struct { u32 len, used; char name[64]; } e;
        if ((i64)sysc(SYS_READDIR, (u64)i, (u64)&e, 0) <= 0) break;
        if (!e.used) continue;
        e.name[63] = 0;
        disk_add(v, e.name, e.len);
    }
    disk_group(v);
    v->status = disk_health(&v->info);
}

static void disk_kv(struct app_win *w, int x, int y, const char *k, const char *val, u32 c) {
    app_str(w, x, y, k, 0x7c8ca0);
    app_str(w, x + 168, y, val, c);
}
static void disk_kv_num(struct app_win *w, int x, int y, const char *k, unsigned long long n, u32 c) {
    char b[24];
    int at = 0;
    disk_u32str(b, &at, n);
    b[at] = 0;
    disk_kv(w, x, y, k, b, c);
}

static void disk_render(struct app_win *w, struct disk_view *v) {
    static const char *tabs[DISK_NTAB] = { "POOL", "DEVICES", "FILES" };
    char b[32];
    app_fill(w, w->bg);
    for (int i = 0; i < DISK_NTAB; i++) {
        app_rect(w, i * 104, 0, 100, 22, v->tab == i ? 0x28495d : 0x1c2636);
        app_str(w, i * 104 + 10, 7, tabs[i], v->tab == i ? 0x22e4ff : 0x7c8ca0);
    }
    int y = 34;
    if (v->tab == DISK_TAB_POOL) {
        const struct outrun_storage_info *s = &v->info;
        app_str(w, 8, y, s->cas_mounted ? "CAS VOLUME MOUNTED" : "NO CAS VOLUME", 0x3df5c4);
        y += 20;
        disk_kv_num(w, 8, y, "FORMAT VERSION", s->cas_version, w->fg); y += 14;
        disk_kv_num(w, 8, y, "BLOCK SIZE", s->cas_block_size, w->fg); y += 14;
        disk_bytes(b, disk_pool_bytes(s));
        disk_kv(w, 8, y, "POOL CAPACITY", b, w->fg); y += 14;
        disk_kv_num(w, 8, y, "BLOCKS TOTAL", s->cas_total_blocks, w->fg); y += 14;
        disk_kv_num(w, 8, y, "BLOCKS USED", s->cas_used_blocks, 0xffb020); y += 14;
        disk_kv_num(w, 8, y, "BLOCKS FREE", disk_pool_free(s), 0x3df5c4); y += 18;
        /* The bar is the same number as the percent beside it, not a second
         * derivation: one expression, drawn and printed. */
        unsigned pct = disk_pool_pct(s);
        app_rect(w, 8, y, 300, 14, 0x121722);
        app_rect(w, 8, y, (int)(300u * pct / 100u), 14, pct > 90 ? 0xff2d9b : 0x22e4ff);
        app_u32(w, 316, y + 3, pct, w->fg); app_str(w, 348, y + 3, "% USED", w->fg);
        y += 24;
        disk_kv_num(w, 8, y, "CAS PUTS", s->cas_put_count, w->fg); y += 14;
        disk_kv_num(w, 8, y, "DEDUP HITS", s->cas_dedup_hits, 0x3df5c4); y += 14;
        app_str(w, 8, y, "DEDUP RATE", 0x7c8ca0);
        app_u32(w, 176, y, disk_dedup_pct(s), 0x3df5c4);
        app_str(w, 208, y, "%", 0x3df5c4); y += 18;
        disk_kv_num(w, 8, y, "BLOCKS FREED", s->cas_blocks_freed, w->fg); y += 14;
        disk_kv_num(w, 8, y, "REF DROPS", s->cas_ref_drops, w->fg); y += 14;
        disk_kv_num(w, 8, y, "REF UNDERFLOW", s->cas_ref_underflow,
                    s->cas_ref_underflow ? 0xff2d9b : 0x3df5c4); y += 14;
        disk_kv_num(w, 8, y, "JOURNAL RECOVERIES", s->cas_recover_calls, w->fg); y += 14;
        disk_kv_num(w, 8, y, "JOURNAL REPLAYS", s->cas_recover_replays,
                    s->cas_recover_replays ? 0xffb020 : w->fg);
    } else if (v->tab == DISK_TAB_DEVICES) {
        app_str(w, 8, y, "BLOCK DEVICES ENUMERATED BY THE KERNEL", 0x7c8ca0);
        y += 20;
        if (!v->info.ndev) {
            app_str(w, 8, y, "NONE. NO VIRTIO-BLK DEVICE IS PRESENT ON", 0xffb020);
            app_str(w, 8, y + 14, "THIS MACHINE, AND THIS KERNEL HAS NO NVME", 0xffb020);
            app_str(w, 8, y + 28, "DRIVER — SO THERE IS NOTHING TO REPORT,", 0xffb020);
            app_str(w, 8, y + 42, "NOT A DEVICE THAT FAILED TO APPEAR.", 0xffb020);
        }
        for (unsigned i = 0; i < v->info.ndev && i < HW_MAX_BLOCKDEV; i++) {
            const struct outrun_blockdev *d = &v->info.dev[i];
            app_rect(w, 4, y - 4, w->cw - 8, 90, 0x121722);
            app_str(w, 8, y, d->name, 0x22e4ff); y += 16;
            disk_bytes(b, disk_capacity_bytes(d));
            disk_kv(w, 8, y, "  CAPACITY", b, w->fg); y += 14;
            disk_kv_num(w, 8, y, "  SECTORS", d->capacity_sectors, w->fg); y += 14;
            disk_kv_num(w, 8, y, "  SECTOR SIZE", d->sector_size, w->fg); y += 14;
            disk_kv_num(w, 8, y, "  QUEUE DEPTH", d->queue_size, w->fg); y += 14;
            disk_kv_num(w, 8, y, "  IRQ LINE", d->irq_line, w->fg); y += 14;
            disk_kv_num(w, 8, y, "  IRQS SERVICED", d->irqs, 0x3df5c4); y += 22;
        }
    } else {
        app_str(w, 8, y, "PREFIX", 0x7c8ca0);
        app_str(w, 200, y, "FILES", 0x7c8ca0);
        app_str(w, 268, y, "BYTES", 0x7c8ca0);
        y += 16;
        for (int i = 0; i < v->ngrp && i < 7; i++) {
            u32 c = i == v->ngrp - 1 ? 0x22e4ff : w->fg;
            app_str(w, 8, y, v->grp[i].prefix, c);
            app_u32(w, 200, y, v->grp[i].files, c);
            disk_bytes(b, v->grp[i].bytes);
            app_str(w, 268, y, b, c);
            y += 14;
        }
        y += 8;
        app_str(w, 8, y, "NAME", 0x7c8ca0);
        app_str(w, 300, y, "BYTES", 0x7c8ca0);
        y += 16;
        v->rows = (w->ch - 60 - y) / 12;
        if (v->rows < 1) v->rows = 1;
        for (int i = 0; i < v->rows && v->top + i < v->nent; i++) {
            const struct disk_entry *e = &v->ent[v->top + i];
            app_str(w, 8, y, e->name, w->fg);
            app_u32(w, 300, y, e->len, 0x3df5c4);
            y += 12;
        }
        app_rect(w, 8, w->ch - 54, 60, 20, 0x1c2636);
        app_str(w, 20, w->ch - 48, "UP", 0x22e4ff);
        app_rect(w, 76, w->ch - 54, 60, 20, 0x1c2636);
        app_str(w, 84, w->ch - 48, "DOWN", 0x22e4ff);
        app_str(w, 148, w->ch - 48, disk_listing_note(v), 0x7c8ca0);
    }
    app_rect(w, 144, w->ch - 54, 60, 20, 0x1c2636);
    app_str(w, 8, w->ch - 26, v->status ? v->status : "", 0xffb020);
    app_str(w, 8, w->ch - 12, "READ-ONLY: NOTHING HERE WRITES TO THE VOLUME", 0x7c8ca0);
    app_present(w);
}

void _start(void) {
    struct app_win w;
    if (app_create(&w, 430, 440, 0x3df5c4)) app_exit(1);
    app_title(&w, "OUTRUN DISKS");
    view.rows = 10;
    view.status = "";
    disk_refresh(&view);
    u64 last = 0;
    for (;;) {
        struct outrun_desktop_info now;
        int dirty = 0;
        if ((i64)sysc(SYS_DESKTOP_INFO, (u64)&now, sizeof now, 0) >= 0 &&
            (!last || now.wall_ns - last >= 1000000000ull)) {
            last = now.wall_ns;
            disk_refresh(&view);
            dirty = 1;
        }
        struct outrun_event e;
        int rc;
        while ((rc = app_poll(&w, &e)) > 0) {
            if (e.type == EVENT_MOUSE_DOWN) {
                if (e.y < 22) disk_set_tab(&view, e.x / 104);
                else if (view.tab == DISK_TAB_FILES && e.y >= w.ch - 54 && e.y < w.ch - 34) {
                    if (e.x < 68) disk_scroll(&view, -1);
                    else if (e.x < 136) disk_scroll(&view, 1);
                }
                dirty = 1;
            } else if (e.type == EVENT_KEY_PRESS) {
                if (e.code == '\t') disk_set_tab(&view, (view.tab + 1) % DISK_NTAB);
                if (e.code == 'r' || e.code == 'R') disk_refresh(&view);
                dirty = 1;
            }
        }
        if (rc < 0) app_exit(0);
        if (dirty) disk_render(&w, &view);
        app_idle();
    }
}
#endif
