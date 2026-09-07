/* Host tests for the PCI / VFIO explorer's decoding core.
 *
 * The capability walk is the part worth testing hardest. It follows a linked
 * list that lives in device memory, which means the list is written by
 * something this program does not control: a malformed or hostile pointer
 * chain must terminate, and it must say WHY it terminated rather than
 * returning a short list that looks like a device with few capabilities.
 *
 * The walk is driven through a callback so these tests can hand it a synthetic
 * configuration space. On the real machine the same code is driven by
 * SYS_PCI_CFG_READ, which is dword-aligned and bounded to the 256-byte legacy
 * header — both constraints the reader here reproduces exactly. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define APP_HOST_TEST
#include "pci_view.c"

/* A synthetic 256-byte config space, addressed as 64 dwords. */
static unsigned int cfg[64];
static int reads;
static long long fake_read(void *ctx, unsigned off) {
    (void)ctx;
    reads++;
    if (off > 0xFC || (off & 3)) return -22;      /* the kernel's own contract */
    return (long long)cfg[off / 4];
}
/* A reader that always refuses, standing in for a device the caller does not
 * own — SYS_PCI_CFG_READ returns -EPERM for exactly that. */
static long long deny_read(void *ctx, unsigned off) { (void)ctx; (void)off; return -13; }

static void cap_at(unsigned off, unsigned char id, unsigned char next) {
    unsigned shift = (off & 3) * 8;
    cfg[off / 4] = (cfg[off / 4] & ~(0xFFu << shift)) | ((unsigned)id << shift);
    unsigned n = off + 1;
    unsigned nshift = (n & 3) * 8;
    cfg[n / 4] = (cfg[n / 4] & ~(0xFFu << nshift)) | ((unsigned)next << nshift);
}

int main(void) {
    char buf[32];

    /* ---- header decoding ------------------------------------------------- */
    assert(pci_vendor(0x10001af4u) == 0x1af4);
    assert(pci_device(0x10001af4u) == 0x1000);
    assert(pci_class(0x02000001u) == 0x02);
    assert(pci_subclass(0x02000001u) == 0x00);
    assert(pci_progif(0x02000001u) == 0x00);
    assert(pci_revision(0x02000001u) == 0x01);
    assert(pci_header_type(0x00800000u) == 0x80);   /* multifunction bit kept  */

    assert(!strcmp(pci_vendor_name(0x1af4), "RED HAT / VIRTIO"));
    assert(!strcmp(pci_vendor_name(0x8086), "INTEL"));
    assert(!strcmp(pci_vendor_name(0x1234), "QEMU / BOCHS"));
    assert(!strcmp(pci_vendor_name(0xdead), "UNKNOWN VENDOR"));
    assert(!strcmp(pci_class_name(0x01), "MASS STORAGE"));
    assert(!strcmp(pci_class_name(0x02), "NETWORK"));
    assert(!strcmp(pci_class_name(0x03), "DISPLAY"));
    assert(!strcmp(pci_class_name(0x06), "BRIDGE"));
    assert(!strcmp(pci_class_name(0xff), "UNCLASSIFIED"));
    assert(!strcmp(pci_cap_name(0x01), "POWER MGMT"));
    assert(!strcmp(pci_cap_name(0x05), "MSI"));
    assert(!strcmp(pci_cap_name(0x09), "VENDOR (VIRTIO)"));
    assert(!strcmp(pci_cap_name(0x10), "PCI EXPRESS"));
    assert(!strcmp(pci_cap_name(0x11), "MSI-X"));
    assert(!strcmp(pci_cap_name(0x7f), "ID 7F"));

    pci_bdf_str(buf, 0x0018);        assert(!strcmp(buf, "00:03.0"));
    pci_bdf_str(buf, 0x0019);        assert(!strcmp(buf, "00:03.1"));
    pci_bdf_str(buf, 0x01FF);        assert(!strcmp(buf, "01:1F.7"));

    /* ---- a well-formed capability list ----------------------------------- */
    struct pci_caplist caps;
    memset(cfg, 0, sizeof cfg);
    cfg[1] = 1u << 20;               /* status: capability list present        */
    cfg[0x34 / 4] = 0x40;            /* first capability at 0x40               */
    cap_at(0x40, 0x09, 0x50);        /* virtio vendor cap  -> 0x50             */
    cap_at(0x50, 0x11, 0x60);        /* MSI-X              -> 0x60             */
    cap_at(0x60, 0x10, 0x00);        /* PCI Express        -> end              */
    reads = 0;
    assert(pci_walk_caps(&caps, fake_read, 0) == 3);
    assert(caps.status == PCI_CAP_OK && caps.n == 3);
    assert(caps.cap[0].off == 0x40 && caps.cap[0].id == 0x09 && caps.cap[0].next == 0x50);
    assert(caps.cap[1].off == 0x50 && caps.cap[1].id == 0x11);
    assert(caps.cap[2].off == 0x60 && caps.cap[2].id == 0x10 && caps.cap[2].next == 0x00);
    assert(reads > 0);
    assert(!strcmp(pci_cap_status(&caps), "3 CAPABILITIES"));

    /* The bottom two bits of a capability pointer are RESERVED and must be
     * masked, per the PCI specification, not chased. A device that sets them
     * still points at the aligned structure below, and following the raw value
     * would read a byte that is not a capability id. */
    memset(cfg, 0, sizeof cfg);
    cfg[1] = 1u << 20;
    cfg[0x34 / 4] = 0x43;            /* reserved bits set                      */
    cap_at(0x40, 0x05, 0x00);        /* the real capability is at 0x40         */
    assert(pci_walk_caps(&caps, fake_read, 0) == 1);
    assert(caps.cap[0].off == 0x40 && caps.cap[0].id == 0x05 && caps.cap[0].next == 0);

    /* Every capability therefore sits at a 4-aligned offset, and the id and
     * next bytes are byte 0 and byte 1 of the SAME dword — which is why the
     * byte reader's shift is exercised at two different shifts and never
     * across a dword edge. This checks the shift, not the edge. */
    memset(cfg, 0, sizeof cfg);
    cfg[1] = 1u << 20;
    cfg[0x34 / 4] = 0x40;
    cap_at(0x40, 0xAB, 0x00);
    assert(pci_walk_caps(&caps, fake_read, 0) == 1 && caps.cap[0].id == 0xAB);

    /* ---- a device with no capability list -------------------------------- */
    memset(cfg, 0, sizeof cfg);
    assert(pci_walk_caps(&caps, fake_read, 0) == 0);
    assert(caps.status == PCI_CAP_NONE && caps.n == 0);
    assert(!strcmp(pci_cap_status(&caps), "NO CAPABILITY LIST (STATUS BIT CLEAR)"));

    /* The bit is set but the pointer is null: also no list, and distinctly so. */
    memset(cfg, 0, sizeof cfg);
    cfg[1] = 1u << 20;
    assert(pci_walk_caps(&caps, fake_read, 0) == 0);
    assert(caps.status == PCI_CAP_NONE);

    /* ---- a LOOP terminates and is named ---------------------------------- */
    memset(cfg, 0, sizeof cfg);
    cfg[1] = 1u << 20;
    cfg[0x34 / 4] = 0x40;
    cap_at(0x40, 0x09, 0x50);
    cap_at(0x50, 0x11, 0x40);        /* points back at the first              */
    assert(pci_walk_caps(&caps, fake_read, 0) == 2);
    assert(caps.status == PCI_CAP_LOOP && caps.n == 2);
    assert(!strcmp(pci_cap_status(&caps), "MALFORMED: CAPABILITY LIST LOOPS"));

    /* A capability pointing at itself is a loop of length one. */
    memset(cfg, 0, sizeof cfg);
    cfg[1] = 1u << 20;
    cfg[0x34 / 4] = 0x40;
    cap_at(0x40, 0x09, 0x40);
    assert(pci_walk_caps(&caps, fake_read, 0) == 1 && caps.status == PCI_CAP_LOOP);

    /* ---- an out-of-header pointer is refused, not chased ------------------ */
    memset(cfg, 0, sizeof cfg);
    cfg[1] = 1u << 20;
    cfg[0x34 / 4] = 0x40;
    cap_at(0x40, 0x09, 0x20);        /* below 0x40: inside the standard header */
    assert(pci_walk_caps(&caps, fake_read, 0) == 1);
    assert(caps.status == PCI_CAP_BAD);
    assert(!strcmp(pci_cap_status(&caps), "MALFORMED: CAPABILITY POINTER OUT OF RANGE"));

    /* The TOP of the header needs no guard, and this pins that: the reserved
     * bits are masked, so 0xFE becomes 0xFC — the last dword of the legacy
     * header, which holds an id and a next byte perfectly well. A rejection
     * here would be this program refusing a legal capability. */
    memset(cfg, 0, sizeof cfg);
    cfg[1] = 1u << 20;
    cfg[0x34 / 4] = 0xFE;
    cap_at(0xFC, 0x01, 0x00);
    assert(pci_walk_caps(&caps, fake_read, 0) == 1);
    assert(caps.status == PCI_CAP_OK && caps.cap[0].off == 0xFC && caps.cap[0].id == 0x01);

    /* ---- more capabilities than the view holds --------------------------- */
    memset(cfg, 0, sizeof cfg);
    cfg[1] = 1u << 20;
    cfg[0x34 / 4] = 0x40;
    for (int i = 0; i < PCI_MAXCAP + 2; i++) {
        unsigned off = 0x40u + (unsigned)i * 4u;
        unsigned next = (i == PCI_MAXCAP + 1) ? 0u : off + 4u;
        cap_at(off, 0x09, (unsigned char)next);
    }
    assert(pci_walk_caps(&caps, fake_read, 0) == PCI_MAXCAP);
    assert(caps.status == PCI_CAP_FULL);
    assert(!strcmp(pci_cap_status(&caps), "MORE CAPABILITIES THAN THIS VIEW HOLDS"));

    /* ---- a device the caller does not own -------------------------------- */
    assert(pci_walk_caps(&caps, deny_read, 0) == 0);
    assert(caps.status == PCI_CAP_DENIED && caps.n == 0);
    assert(!strcmp(pci_cap_status(&caps),
                   "CONFIG SPACE DENIED: CLAIM THE DEVICE FIRST (NEEDS VFIO)"));

    /* ---- device state labels -------------------------------------------- */
    assert(!strcmp(pci_state_label(0), "FREE"));
    assert(!strcmp(pci_state_label(HW_PCI_BOUND_HOST), "HOST-BOUND (NO QUIESCE PATH)"));
    assert(!strcmp(pci_state_label(HW_PCI_BOUND_HOST | HW_PCI_QUIESCABLE),
                   "HOST-BOUND (QUIESCABLE)"));
    assert(!strcmp(pci_state_label(HW_PCI_CLAIMED), "CLAIMED BY RING 3"));
    assert(!strcmp(pci_state_label(HW_PCI_SYNTHETIC), "SYNTHETIC (NO CONFIG SPACE)"));
    /* A synthetic device outranks every other label: it has no BDF at all, so
     * calling it "free" would invite a claim that can never succeed. */
    assert(!strcmp(pci_state_label(HW_PCI_SYNTHETIC | HW_PCI_BOUND_HOST),
                   "SYNTHETIC (NO CONFIG SPACE)"));

    /* ---- BAR summary ----------------------------------------------------- */
    struct outrun_pci_dev dev;
    memset(&dev, 0, sizeof dev);
    assert(pci_bar_count(&dev) == 0);
    dev.bar_base[0] = 0xfe000000ull; dev.bar_len[0] = 0x4000;
    dev.bar_base[4] = 0xfd000000ull; dev.bar_len[4] = 0x1000;
    assert(pci_bar_count(&dev) == 2);
    assert(pci_bar_present(&dev, 0) && !pci_bar_present(&dev, 1) && pci_bar_present(&dev, 4));
    pci_hex(buf, 0xfe000000ull, 8);  assert(!strcmp(buf, "FE000000"));
    pci_hex(buf, 0ull, 4);           assert(!strcmp(buf, "0000"));

    printf("pci_view: header decode, bounded capability walk (loops, out-of-range, "
           "overflow, denial), BDF and state labels PASS\n");
    return 0;
}
