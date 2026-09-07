/* OutRun PCI — the bus inventory, BAR windows, and the capability list.
 *
 * TWO SOURCES, AND THEY ANSWER DIFFERENT QUESTIONS.
 *
 * SYS_HW_INFO's HW_PCI domain is read-only and needs no ownership: it reports
 * what the bus walk found, what each function's BAR windows are, which
 * capability the kernel requires to touch it, whether a kernel driver is bound
 * and whether that driver has a quiesce path. That is the inventory.
 *
 * SYS_PCI_CFG_READ (116) is the capability WALK, and it is deliberately
 * narrower: it serves only a device this process has CLAIMED, because
 * configuration space is where a driver discovers the virtio register layout
 * and the kernel is not willing to hand that to an arbitrary program. A device
 * the kernel is driving cannot be claimed at all, so on a stock boot most rows
 * here will report the denial rather than a list — and the denial is printed
 * verbatim, because "no capabilities" and "not allowed to look" are different
 * facts and a tool that rendered them the same would be lying about hardware.
 *
 * Nothing in this program writes. There is no SYS_PCI_CFG_WRITE by design: the
 * BARs, the command register and the interrupt line all live in config space,
 * and a ring-3 write could move a device's window on top of kernel memory. */
#include "../include/outrun_abi.h"

#define PCI_MAXCAP 16

struct pci_cap { unsigned char off, id, next; };
enum { PCI_CAP_OK = 0, PCI_CAP_NONE, PCI_CAP_LOOP, PCI_CAP_BAD, PCI_CAP_FULL, PCI_CAP_DENIED };
struct pci_caplist { struct pci_cap cap[PCI_MAXCAP]; int n, status; };

/* Reads one DWORD of configuration space at `off`, or a negative errno. The
 * indirection is what lets the walk below be tested against a synthetic device
 * on the build host; on the machine it is SYS_PCI_CFG_READ. */
typedef long long (*pci_read_fn)(void *ctx, unsigned off);

/* ---- header decoding ----------------------------------------------------- */
static unsigned pci_vendor(unsigned cfg0)      { return cfg0 & 0xFFFFu; }
static unsigned pci_device(unsigned cfg0)      { return (cfg0 >> 16) & 0xFFFFu; }
static unsigned pci_class(unsigned cfg2)       { return (cfg2 >> 24) & 0xFFu; }
static unsigned pci_subclass(unsigned cfg2)    { return (cfg2 >> 16) & 0xFFu; }
static unsigned pci_progif(unsigned cfg2)      { return (cfg2 >> 8) & 0xFFu; }
static unsigned pci_revision(unsigned cfg2)    { return cfg2 & 0xFFu; }
static unsigned pci_header_type(unsigned cfg3) { return (cfg3 >> 16) & 0xFFu; }

static const char *pci_vendor_name(unsigned v) {
    switch (v) {
    case 0x1af4: return "RED HAT / VIRTIO";
    case 0x1b36: return "RED HAT / QEMU";
    case 0x8086: return "INTEL";
    case 0x1234: return "QEMU / BOCHS";
    case 0x1022: return "AMD";
    default:     return "UNKNOWN VENDOR";
    }
}
static const char *pci_class_name(unsigned c) {
    switch (c) {
    case 0x00: return "LEGACY";
    case 0x01: return "MASS STORAGE";
    case 0x02: return "NETWORK";
    case 0x03: return "DISPLAY";
    case 0x04: return "MULTIMEDIA";
    case 0x05: return "MEMORY";
    case 0x06: return "BRIDGE";
    case 0x07: return "COMMUNICATION";
    case 0x08: return "SYSTEM PERIPHERAL";
    case 0x09: return "INPUT";
    case 0x0c: return "SERIAL BUS";
    case 0x0d: return "WIRELESS";
    default:   return "UNCLASSIFIED";
    }
}
/* Unknown ids are reported as their number rather than as a blank: a
 * capability this program has no name for is still a capability that is there,
 * and hiding it would understate the device. */
static char g_capname[12];
static const char *pci_cap_name(unsigned id) {
    switch (id) {
    case 0x01: return "POWER MGMT";
    case 0x02: return "AGP";
    case 0x03: return "VPD";
    case 0x05: return "MSI";
    case 0x07: return "PCI-X";
    case 0x09: return "VENDOR (VIRTIO)";
    case 0x0a: return "DEBUG PORT";
    case 0x10: return "PCI EXPRESS";
    case 0x11: return "MSI-X";
    case 0x12: return "SATA";
    default: break;
    }
    static const char hex[] = "0123456789ABCDEF";
    g_capname[0] = 'I'; g_capname[1] = 'D'; g_capname[2] = ' ';
    g_capname[3] = hex[(id >> 4) & 0xF];
    g_capname[4] = hex[id & 0xF];
    g_capname[5] = 0;
    return g_capname;
}
static void pci_hex(char *out, unsigned long long v, int digits) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; --i) { out[i] = hex[v & 0xF]; v >>= 4; }
    out[digits] = 0;
}
/* bdf packs (bus << 8) | (slot << 3) | func — the same source-id the IOMMU
 * uses, so this prints what lspci would and what a DMAR trace would name. */
static void pci_bdf_str(char *out, unsigned bdf) {
    static const char hex[] = "0123456789ABCDEF";
    unsigned slot = (bdf >> 3) & 0x1Fu;      /* FIVE bits, not four: slot 0x1F */
    out[0] = hex[(bdf >> 12) & 0xF];         /* is a real address and truncating */
    out[1] = hex[(bdf >> 8) & 0xF];          /* it would alias it onto slot 0x0F */
    out[2] = ':';
    out[3] = hex[(slot >> 4) & 0x1];
    out[4] = hex[slot & 0xF];
    out[5] = '.';
    out[6] = hex[bdf & 0x7];
    out[7] = 0;
}

/* ---- the capability walk -------------------------------------------------- */
/* One BYTE out of configuration space, assembled from the dword the kernel is
 * willing to serve. SYS_PCI_CFG_READ refuses an unaligned offset, and a
 * capability pointer is a byte offset with no alignment guarantee, so the
 * shifting has to happen here rather than being wished away. */
static int pci_byte(pci_read_fn rd, void *ctx, unsigned off, unsigned *out) {
    long long dw = rd(ctx, off & ~3u);
    if (dw < 0) return (int)dw;
    *out = ((unsigned)dw >> ((off & 3u) * 8u)) & 0xFFu;
    return 0;
}
/* Walks the list rooted at 0x34 and returns the number of entries recorded.
 *
 * EVERY ABNORMAL TERMINATION SETS A DISTINCT STATUS. A list that loops, one
 * that points outside the legacy header, one longer than this view holds and
 * one the caller is not permitted to read all produce a short list, and a
 * program that reported "n capabilities" for each of them would be presenting
 * four different hardware situations as the same measurement. */
static int pci_walk_caps(struct pci_caplist *out, pci_read_fn rd, void *ctx) {
    unsigned char seen[256];
    for (int i = 0; i < 256; i++) seen[i] = 0;
    out->n = 0;
    out->status = PCI_CAP_NONE;

    long long status_dw = rd(ctx, 0x04);
    if (status_dw < 0) { out->status = PCI_CAP_DENIED; return 0; }
    if (!((unsigned)status_dw & (1u << 20))) return 0;      /* no list, per the device */

    unsigned ptr = 0;
    if (pci_byte(rd, ctx, 0x34, &ptr) < 0) { out->status = PCI_CAP_DENIED; return 0; }
    ptr &= 0xFCu;
    if (!ptr) return 0;

    for (;;) {
        /* A pointer below 0x40 aims into the STANDARD header — the BARs, the
         * command register, the interrupt line — and reading a field there as
         * a capability id would report hardware that is not present. There is
         * deliberately no upper-bound test beside it: the reserved-bit mask
         * above leaves ptr <= 0xFC by construction, and 0xFC is the last dword
         * of the legacy header, so a check for "too high" would be a guard
         * nothing could ever trip. */
        if (ptr < 0x40u) { out->status = PCI_CAP_BAD; return out->n; }
        if (seen[ptr]) { out->status = PCI_CAP_LOOP; return out->n; }
        seen[ptr] = 1;
        unsigned id = 0, next = 0;
        if (pci_byte(rd, ctx, ptr, &id) < 0 || pci_byte(rd, ctx, ptr + 1, &next) < 0) {
            out->status = PCI_CAP_DENIED;
            return out->n;
        }
        if (out->n >= PCI_MAXCAP) { out->status = PCI_CAP_FULL; return out->n; }
        out->cap[out->n].off = (unsigned char)ptr;
        out->cap[out->n].id = (unsigned char)id;
        out->cap[out->n].next = (unsigned char)next;
        out->n++;
        if (!next) { out->status = PCI_CAP_OK; return out->n; }
        ptr = next & 0xFCu;
    }
}
static char g_capstatus[64];
static const char *pci_cap_status(const struct pci_caplist *c) {
    switch (c->status) {
    case PCI_CAP_NONE:   return "NO CAPABILITY LIST (STATUS BIT CLEAR)";
    case PCI_CAP_LOOP:   return "MALFORMED: CAPABILITY LIST LOOPS";
    case PCI_CAP_BAD:    return "MALFORMED: CAPABILITY POINTER OUT OF RANGE";
    case PCI_CAP_FULL:   return "MORE CAPABILITIES THAN THIS VIEW HOLDS";
    case PCI_CAP_DENIED: return "CONFIG SPACE DENIED: CLAIM THE DEVICE FIRST (NEEDS VFIO)";
    default: break;
    }
    int at = 0;
    unsigned n = (unsigned)c->n;
    char d[8];
    int k = 0;
    do { d[k++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (k) g_capstatus[at++] = d[--k];
    const char *tail = " CAPABILITIES";
    for (int i = 0; tail[i]; i++) g_capstatus[at++] = tail[i];
    g_capstatus[at] = 0;
    return g_capstatus;
}

/* ---- device state -------------------------------------------------------- */
static int pci_bar_present(const struct outrun_pci_dev *d, int i) {
    return i >= 0 && i < 6 && d->bar_len[i] != 0;
}
static int pci_bar_count(const struct outrun_pci_dev *d) {
    int n = 0;
    for (int i = 0; i < 6; i++) if (d->bar_len[i]) n++;
    return n;
}
/* Ordered by what a caller can DO with the device. Synthetic first: it has no
 * BDF, so no claim by BDF can ever succeed and calling it "free" would invite
 * one. Then claimed, then host-bound split by whether the driver can stand
 * down — "busy now" and "never available" are different answers. */
static const char *pci_state_label(unsigned state) {
    if (state & HW_PCI_SYNTHETIC)  return "SYNTHETIC (NO CONFIG SPACE)";
    if (state & HW_PCI_CLAIMED)    return "CLAIMED BY RING 3";
    if (state & HW_PCI_BOUND_HOST)
        return (state & HW_PCI_QUIESCABLE) ? "HOST-BOUND (QUIESCABLE)"
                                           : "HOST-BOUND (NO QUIESCE PATH)";
    return "FREE";
}

#ifndef APP_HOST_TEST
#include "gui.h"

static struct outrun_pci_info info;
static struct pci_caplist caps;
static int selected;
static const char *status = "SELECT A DEVICE, THEN READ CAPS";
static int cap_device = -1;
static int claimed_id = -1;

/* The live reader: SYS_PCI_CFG_READ against a device this process has claimed.
 * `ctx` carries the kernel's device id, which is what the syscall addresses —
 * NOT the BDF, and not this program's row index. */
static long long live_read(void *ctx, unsigned off) {
    u64 device_id = (u64)(unsigned long)ctx;
    return (long long)(i64)sysc(SYS_PCI_CFG_READ, device_id, off, 0);
}

static void pci_refresh(void) {
    i64 rc = (i64)sysc(SYS_HW_INFO, HW_PCI, (u64)&info, sizeof info);
    if (rc < 0 || info.version != OUTRUN_HW_ABI_VERSION) {
        info.ndev = 0;
        status = "SYS_HW_INFO REFUSED: KERNEL/HEADER MISMATCH";
    }
    if (selected >= (int)info.ndev) selected = 0;
}

/* Claim, walk, release. The claim is NOT held across frames: holding an
 * exclusive claim on a device for as long as a viewer window happens to be
 * open would make this program a denial of service against every real driver
 * that wanted it. */
static void pci_read_caps(void) {
    if (selected < 0 || selected >= (int)info.ndev) return;
    cap_device = selected;
    const struct outrun_pci_dev *d = &info.dev[selected];
    caps.n = 0;
    if (d->state & HW_PCI_SYNTHETIC) {
        caps.status = PCI_CAP_DENIED;
        status = "SYNTHETIC DEVICE: THERE IS NO CONFIG SPACE TO READ";
        return;
    }
    i64 id = (i64)sysc(SYS_CLAIM_PCI_DEVICE, d->bdf, 0, 0);
    if (id < 0) {
        caps.status = PCI_CAP_DENIED;
        status = (d->state & HW_PCI_BOUND_HOST)
               ? "CLAIM REFUSED: A KERNEL DRIVER IS BOUND TO THIS DEVICE"
               : "CLAIM REFUSED: NEEDS THE VFIO CAPABILITY";
        return;
    }
    claimed_id = (int)id;
    pci_walk_caps(&caps, live_read, (void *)(unsigned long)id);
    sysc(SYS_RELEASE_PCI_DEVICE, (u64)id, 0, 0);
    claimed_id = -1;
    status = pci_cap_status(&caps);
}

static void pci_render(struct app_win *w) {
    char b[40];
    app_fill(w, w->bg);
    app_str(w, 8, 6, "OUTRUN PCI  /  BUS INVENTORY", 0x22e4ff);
    app_str(w, 300, 6, info.iommu_on ? "IOMMU ON" : "IOMMU OFF",
            info.iommu_on ? 0x3df5c4 : 0x7c8ca0);
    app_str(w, 8, 24, "BDF", 0x7c8ca0);
    app_str(w, 72, 24, "NAME", 0x7c8ca0);
    app_str(w, 240, 24, "VENDOR:DEV", 0x7c8ca0);
    app_str(w, 348, 24, "BARS", 0x7c8ca0);
    int y = 40;
    for (unsigned i = 0; i < info.ndev && i < HW_MAX_PCI; i++) {
        const struct outrun_pci_dev *d = &info.dev[i];
        if ((int)i == selected) app_rect(w, 4, y - 2, w->cw - 8, 14, 0x25374b);
        if (d->state & HW_PCI_SYNTHETIC) app_str(w, 8, y, "  --  ", 0x7c8ca0);
        else { pci_bdf_str(b, d->bdf); app_str(w, 8, y, b, w->fg); }
        app_str(w, 72, y, d->name, w->fg);
        pci_hex(b, pci_vendor(d->cfg[0]), 4);
        b[4] = ':';
        pci_hex(b + 5, pci_device(d->cfg[0]), 4);
        app_str(w, 240, y, b, 0x3df5c4);
        app_u32(w, 348, y, (u32)pci_bar_count(d), w->fg);
        y += 14;
    }
    if (!info.ndev) app_str(w, 8, y, "NO DEVICES REGISTERED", 0xffb020);

    int py = 40 + 14 * (int)(info.ndev < HW_MAX_PCI ? info.ndev : HW_MAX_PCI) + 10;
    app_rect(w, 4, py, w->cw - 8, 1, 0x1e2735);
    py += 8;
    if (selected < (int)info.ndev) {
        const struct outrun_pci_dev *d = &info.dev[selected];
        app_str(w, 8, py, d->name, 0x22e4ff);
        app_str(w, 8, py + 14, pci_state_label(d->state), 0xffb020);
        app_str(w, 8, py + 28, pci_vendor_name(pci_vendor(d->cfg[0])), w->fg);
        app_str(w, 8, py + 42, pci_class_name(pci_class(d->cfg[1])), w->fg);
        /* class/subclass/prog-if printed as the triple lspci prints, because a
         * class name alone cannot tell a SCSI controller from an NVMe one. */
        pci_hex(b, pci_class(d->cfg[1]), 2);
        b[2] = '.';
        pci_hex(b + 3, pci_subclass(d->cfg[1]), 2);
        b[5] = '.';
        pci_hex(b + 6, pci_progif(d->cfg[1]), 2);
        app_str(w, 232, py + 42, b, 0x3df5c4);
        app_str(w, 8, py + 56, "REV", 0x7c8ca0);
        pci_hex(b, pci_revision(d->cfg[1]), 2);
        app_str(w, 48, py + 56, b, w->fg);
        app_str(w, 88, py + 56, "HDR", 0x7c8ca0);
        pci_hex(b, pci_header_type(d->cfg[2]), 2);
        app_str(w, 128, py + 56, b, w->fg);
        py += 72;
        for (int i = 0; i < 6; i++) {
            if (!pci_bar_present(d, i)) continue;
            app_str(w, 8, py, "BAR", 0x7c8ca0);
            app_u32(w, 40, py, (u32)i, 0x7c8ca0);
            pci_hex(b, d->bar_base[i], 12);
            app_str(w, 64, py, b, w->fg);
            app_str(w, 176, py, "LEN", 0x7c8ca0);
            pci_hex(b, d->bar_len[i], 8);
            app_str(w, 208, py, b, 0x3df5c4);
            py += 12;
        }
        py += 6;
        app_str(w, 8, py, "CAPABILITIES AT 0x34", 0x7c8ca0);
        py += 14;
        if (cap_device != selected) {
            app_str(w, 8, py, "NOT READ FOR THIS DEVICE YET", 0x7c8ca0);
        } else {
            for (int i = 0; i < caps.n; i++) {
                pci_hex(b, caps.cap[i].off, 2);
                app_str(w, 8, py, b, 0xffb020);
                app_str(w, 40, py, pci_cap_name(caps.cap[i].id), w->fg);
                app_str(w, 200, py, "NEXT", 0x7c8ca0);
                pci_hex(b, caps.cap[i].next, 2);
                app_str(w, 240, py, b, 0x7c8ca0);
                py += 12;
            }
        }
    }
    app_rect(w, 8, w->ch - 54, 140, 20, 0x1c2636);
    app_str(w, 16, w->ch - 48, "READ CAPS", 0x22e4ff);
    app_rect(w, 156, w->ch - 54, 100, 20, 0x1c2636);
    app_str(w, 164, w->ch - 48, "REFRESH", 0x22e4ff);
    app_str(w, 8, w->ch - 26, status, 0xffb020);
    app_str(w, 8, w->ch - 12, "READ-ONLY: THIS KERNEL HAS NO CONFIG-SPACE WRITE", 0x7c8ca0);
    app_present(w);
}

void _start(void) {
    struct app_win w;
    if (app_create(&w, 430, 440, 0xffb020)) app_exit(1);
    app_title(&w, "OUTRUN PCI");
    pci_refresh();
    pci_render(&w);
    for (;;) {
        struct outrun_event e;
        int rc, dirty = 0;
        while ((rc = app_poll(&w, &e)) > 0) {
            if (e.type == EVENT_MOUSE_DOWN) {
                int row = (e.y - 38) / 14;
                if (e.y >= 38 && row >= 0 && row < (int)info.ndev) { selected = row; dirty = 1; }
                else if (e.y >= w.ch - 54 && e.y < w.ch - 34) {
                    if (e.x < 148) { pci_read_caps(); dirty = 1; }
                    else if (e.x < 256) { pci_refresh(); cap_device = -1; dirty = 1; }
                }
            } else if (e.type == EVENT_KEY_PRESS) {
                if (e.code == 'c' || e.code == 'C') pci_read_caps();
                if (e.code == 'r' || e.code == 'R') { pci_refresh(); cap_device = -1; }
                if (e.code == 'j' && selected + 1 < (int)info.ndev) selected++;
                if (e.code == 'k' && selected > 0) selected--;
                dirty = 1;
            }
        }
        if (rc < 0) {
            /* A claim outliving this process would leave the device
             * unclaimable by anyone, so the exit path releases it. */
            if (claimed_id >= 0) sysc(SYS_RELEASE_PCI_DEVICE, (u64)claimed_id, 0, 0);
            app_exit(0);
        }
        if (dirty) pci_render(&w);
        app_idle();
    }
}
#endif
