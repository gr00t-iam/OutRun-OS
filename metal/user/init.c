/* ============================================================================
 * OUTRUN OS — user/init.c   (ring-3 userland, ELF loaded from storage)
 * ============================================================================
 * Talks to the kernel ONLY through `syscall`. Demonstrates:
 *   1. capability-gated hardware passthrough (virtio MMIO) + a real MMIO read
 *   2. capability-gated VFS file access (sys_open / sys_read / sys_close)
 * A process without CAP_FILESYSTEM is cleanly denied the file ops; one with it
 * reads a file whose bytes live in content-addressed storage.
 * ==========================================================================*/

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

/* syscall ABI: RAX=num, RDI=a0, RSI=a1, RDX=a2 -> RAX (RCX/R11 clobbered).    */
static inline u64 sysc(u64 num, u64 a0, u64 a1, u64 a2) {
    u64 ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

#define SYS_WRITE          0
#define SYS_HW_PASSTHROUGH 1
#define SYS_EXIT           2
#define SYS_WRITEHEX       3
#define SYS_DEV_OFFSET     4
#define SYS_DMA_ALLOC      11
#define SYS_ROLE           12
#define SYS_SURFACE        13
#define SYS_SURFACE_POLL   14
#define SYS_OPEN           5
#define SYS_READ           6
#define SYS_WRITE_FILE     7
#define SYS_CLOSE          8
#define SYS_WAIT_EVENT     9
#define SYS_MAP_FRAMEBUFFER 10
#define SYS_YIELD          15
#define SYS_GETPID         16
#define SYS_SURFACE_FLIP   17

#define DEV_DEFAULT 0xFFFF

static void print(const char *s) { sysc(SYS_WRITE, (u64)s, 0, 0); }
static void hex(u64 v)           { sysc(SYS_WRITEHEX, v, 0, 0); }
typedef unsigned short u16;

/* ============================================================================
 * USERSPACE virtio-net DRIVER  (runs entirely at ring 3)
 * ============================================================================
 * The full architectural claim, exercised end to end: this process is granted
 * the NIC through the capability gate, receives its MMIO registers mapped into
 * its own address space, allocates its OWN DMA memory, brings the device up
 * from scratch (reset -> feature negotiation -> virtqueue setup -> DRIVER_OK),
 * and makes the hardware DMA a frame straight out of its own pages.
 *
 * No kernel driver is involved in the data path. And because the grant put the
 * device in an IOMMU domain containing only this process's memory, the device
 * physically cannot touch the kernel or any other process while doing it.
 * ==========================================================================*/
static inline void w8 (u64 a, u8  v) { *(volatile u8  *)a = v; }
static inline u8   r8 (u64 a)        { return *(volatile u8  *)a; }
static inline void w16(u64 a, u16 v) { *(volatile u16 *)a = v; }
static inline u16  r16(u64 a)        { return *(volatile u16 *)a; }
static inline void w32(u64 a, u32 v) { *(volatile u32 *)a = v; }
static inline u32  r32(u64 a)        { return *(volatile u32 *)a; }
static inline void w64(u64 a, u64 v) { *(volatile u64 *)a = v; }

/* virtio 1.0 common-config register offsets */
#define VC_DFEAT_SEL 0x00
#define VC_DFEAT     0x04
#define VC_GFEAT_SEL 0x08
#define VC_GFEAT     0x0C
#define VC_STATUS    0x14
#define VC_Q_SELECT  0x16
#define VC_Q_SIZE    0x18
#define VC_Q_ENABLE  0x1C
#define VC_Q_NOFF    0x1E
#define VC_Q_DESC    0x20
#define VC_Q_AVAIL   0x28
#define VC_Q_USED    0x30
#define ST_ACK 1
#define ST_DRV 2
#define ST_OK  4
#define ST_FEAT 8

/* A ring-3 application that owns its own window surface — since v0.31 it is a
 * FIRST-CLASS SCHEDULER THREAD: it creates its pixel buffer, then loops
 * forever (poll routed input -> repaint -> yield), running concurrently with
 * the kernel compositor. Clicks arrive and are painted DURING a canvas pass,
 * not on a later re-entry.
 * Verification colors (unique — never drawn by the kernel palette):
 *   0x101828 checker field, 0xF06A18 click-hit marker, 0x9B4DFF liveness bar. */
static void surface_app(void) {
    print("  [app:r3] ==== RING-3 SURFACE THREAD: live event/render loop ====\n");
    i64 va = (i64)sysc(SYS_SURFACE, (200u << 16) | 120u, 4, 0);   /* slot 4 */
    if (va <= 0) { print("  [app:r3] surface denied (needs CAP_FRAMEBUFFER)\n"); return; }
    /* v0.32: DOUBLE-BUFFERED. The kernel maps buf0 then buf1 contiguously; we
     * always draw into the back buffer and publish with SYS_SURFACE_FLIP,
     * which returns the next back buffer once the compositor took the frame. */
    u64 bufbytes = ((200u * 120u * 4u) + 0xFFFu) & ~0xFFFull;
    volatile unsigned int *p = (volatile unsigned int *)((u64)va + bufbytes); /* back = buf1 */
    print("  [app:r3] got double-buffered surface at "); hex((u64)va);
    print(" — drawing back buffers, publishing via SYS_SURFACE_FLIP\n");
    struct { int type, x, y, code; } ev;
    int hx[8], hy[8], nh = 0;
    char typed[24]; int nt = 0;
    u64 frame = 0;
    for (;;) {
        /* drain routed input: clicks arrive in OUR pixel space, keys (type=2) */
        /* arrive as ASCII codes — same queue, dispatched on sevent.type       */
        while (sysc(SYS_SURFACE_POLL, 4, (u64)&ev, 0) == 1) {
            if (ev.type == 2) {                            /* v0.33: keyboard  */
                print("  [app:r3] key event code ");
                hex((u64)(unsigned)ev.code); print(" — echoing into our pixels\n");
                if (ev.code == 8) { if (nt) nt--; }        /* backspace        */
                else if (ev.code >= 32 && ev.code < 127 && nt < 24)
                    typed[nt++] = (char)ev.code;
                continue;
            }
            print("  [app:r3] click at surface-local (");
            hex((u64)(unsigned)ev.x); print(","); hex((u64)(unsigned)ev.y);
            print(") — repainting NOW, mid-pass\n");
            if (nh < 8) { hx[nh] = ev.x; hy[nh] = ev.y; nh++; }
        }
        /* full repaint into the BACK buffer: scene + liveness bar + hits       */
        for (int y = 0; y < 120; y++) {
            for (int x = 0; x < 200; x++) {
                unsigned int c;
                int gx = x - 100, gy = y - 60;
                int d = gx * gx / 8 + gy * gy / 4;
                if (d < 220) c = 0x22E4FFu;                        /* cyan core     */
                else if (((x >> 3) + (y >> 3)) & 1) c = 0x101828u; /* checker field */
                else c = 0x0A0E14u;
                if (y < 3 || y > 116 || x < 3 || x > 196) c = 0xFF3EA5u; /* edge    */
                p[y * 200 + x] = c;
            }
        }
        int bar = (int)(frame % 184);                     /* sweeping = alive     */
        for (int y = 104; y < 112; y++)
            for (int x = bar; x < bar + 16 && x < 200; x++) p[y * 200 + x] = 0x9B4DFFu;
        for (int i = 0; i < nh; i++)                      /* click-hit markers    */
            for (int dy = -7; dy <= 7; dy++)
                for (int dx = -7; dx <= 7; dx++) {
                    int X = hx[i] + dx, Y = hy[i] + dy;
                    if (X < 0 || X >= 200 || Y < 0 || Y >= 120) continue;
                    if (dx * dx + dy * dy <= 49) p[Y * 200 + X] = 0xF06A18u;
                }
        /* typed text: each char is a block whose COLOR encodes its ASCII code */
        /* (0xA0..30 | code<<8) so the kernel can decode our pixels verbatim   */
        for (int i = 0; i < nt; i++) {
            unsigned int c = 0x00A00030u | ((unsigned int)(u8)typed[i] << 8);
            for (int y = 88; y < 96; y++)
                for (int x = 6 + 8 * i; x < 6 + 8 * i + 7 && x < 200; x++)
                    p[y * 200 + x] = c;
        }
        frame++;
        /* publish: blocks until the compositor's next frame boundary, then     */
        /* hands us the buffer it just retired — vsync for free                 */
        p = (volatile unsigned int *)sysc(SYS_SURFACE_FLIP, 4, 0, 0);
    }
}

/* role 5: the tear detector's producer. Fills WHOLE frames in strictly
 * alternating unique colors and flips each one; if the kernel's flip protocol
 * is sound, no observer can ever scan a front buffer mixing the two.         */
static void tear_test(void) {
    i64 va = (i64)sysc(SYS_SURFACE, (64u << 16) | 64u, 5, 0);
    if (va <= 0) { print("  [tt :r3] surface denied\n"); sysc(SYS_EXIT, 1, 0, 0); }
    volatile unsigned int *back = (volatile unsigned int *)((u64)va + 64 * 64 * 4);
    print("  [tt :r3] tear-test live: 400 alternating full frames via SYS_SURFACE_FLIP\n");
    for (int f = 0; f < 400; f++) {
        unsigned int c = (f & 1) ? 0x1FBF6Eu : 0x4682EAu;
        for (int i = 0; i < 64 * 64; i++) back[i] = c;
        back = (volatile unsigned int *)sysc(SYS_SURFACE_FLIP, 5, 0, 0);
    }
    sysc(SYS_EXIT, 0, 0, 0);
}

/* role 3: surface-lifecycle probe. Creates a small surface in slot 3, lets the
 * kernel observe it live for one yield, then exits — the kernel must unbind
 * the slot and recycle the pixel buffer.                                      */
static void surface_exit_test(void) {
    i64 va = (i64)sysc(SYS_SURFACE, (64u << 16) | 64u, 3, 0);
    if (va > 0) {
        volatile unsigned int *p = (volatile unsigned int *)va;
        for (int i = 0; i < 64 * 64; i++) p[i] = 0x406080u;
        print("  [x3 :r3] surface slot 3 live; exiting so the kernel reclaims it\n");
    }
    sysc(SYS_YIELD, 0, 0, 0);
    sysc(SYS_EXIT, 0, 0, 0);
}

/* role 4: identity prober. Two of these run concurrently; if the kernel ever
 * leaks one thread's process identity into the other across a context switch,
 * SYS_GETPID returns the WRONG pid and we exit 2 instead of 0.                */
static void ident_probe(void) {
    u64 pid0 = sysc(SYS_GETPID, 0, 0, 0);
    for (int i = 0; i < 40; i++) {
        sysc(SYS_YIELD, 0, 0, 0);
        if (sysc(SYS_GETPID, 0, 0, 0) != pid0) sysc(SYS_EXIT, 2, 0, 0);
    }
    sysc(SYS_EXIT, 0, 0, 0);
}

static void nic_driver(void) {
    print("  [drv:r3] ==== USERSPACE virtio-net DRIVER starting at ring 3 ====\n");

    i64 mmio = (i64)sysc(SYS_HW_PASSTHROUGH, 0xFFFF, 0, 0);
    if (mmio <= 0) { print("  [drv:r3] device passthrough DENIED\n"); return; }
    print("  [drv:r3] NIC registers mapped into this process at vaddr "); hex((u64)mmio);
    print("\n");

    u64 coff = sysc(SYS_DEV_OFFSET, 6, 0, 0);
    u64 noff = sysc(SYS_DEV_OFFSET, 7, 0, 0);
    u64 dcfg = sysc(SYS_DEV_OFFSET, 9, 0, 0);
    u64 nmul = sysc(SYS_DEV_OFFSET, 10, 0, 0);
    u64 c = (u64)mmio + coff;

    u64 phys = 0;
    i64 dma = (i64)sysc(SYS_DMA_ALLOC, 4, (u64)&phys, 0);   /* 4 contiguous pages */
    if (dma <= 0) { print("  [drv:r3] DMA alloc denied\n"); return; }
    print("  [drv:r3] own DMA memory: vaddr "); hex((u64)dma);
    print(" -> phys "); hex(phys); print("\n");

    /* ---- bring the device up from reset, entirely from ring 3 ---- */
    w8(c + VC_STATUS, 0);
    for (int i = 0; i < 100000 && r8(c + VC_STATUS); i++) { }
    w8(c + VC_STATUS, ST_ACK);
    w8(c + VC_STATUS, ST_ACK | ST_DRV);
    w32(c + VC_DFEAT_SEL, 1);
    u32 fhi = r32(c + VC_DFEAT);
    w32(c + VC_GFEAT_SEL, 0); w32(c + VC_GFEAT, 0);
    w32(c + VC_GFEAT_SEL, 1); w32(c + VC_GFEAT, 1 | (fhi & 2)); /* VERSION_1 (+ACCESS_PLATFORM) */
    w8(c + VC_STATUS, ST_ACK | ST_DRV | ST_FEAT);
    if (!(r8(c + VC_STATUS) & ST_FEAT)) { print("  [drv:r3] FEATURES_OK rejected\n"); return; }
    print("  [drv:r3] negotiated features, FEATURES_OK accepted by device\n");

    /* ---- set up TX virtqueue (queue 1) in our own DMA pages ---- */
    w16(c + VC_Q_SELECT, 1);
    u16 qsz = r16(c + VC_Q_SIZE);
    if (!qsz) { print("  [drv:r3] no TX queue\n"); return; }
    if (qsz > 64) { w16(c + VC_Q_SIZE, 64); qsz = 64; }
    w64(c + VC_Q_DESC,  phys);              /* descriptor table -> our page 0 */
    w64(c + VC_Q_AVAIL, phys + 0x1000);     /* avail ring       -> our page 1 */
    w64(c + VC_Q_USED,  phys + 0x2000);     /* used ring        -> our page 2 */
    u16 qnoff = r16(c + VC_Q_NOFF);
    w16(c + VC_Q_ENABLE, 1);
    w8(c + VC_STATUS, ST_ACK | ST_DRV | ST_FEAT | ST_OK);
    print("  [drv:r3] TX virtqueue live in our own pages; DRIVER_OK set\n");

    /* ---- build a frame in our DMA page and hand it to the hardware ---- */
    u64 buf = (u64)dma + 0x3000;
    for (int i = 0; i < 12; i++) w8(buf + i, 0);            /* virtio-net header */
    u64 e = buf + 12;
    for (int i = 0; i < 6; i++) w8(e + i, 0xFF);            /* broadcast         */
    for (int i = 0; i < 6; i++) w8(e + 6 + i, r8((u64)mmio + dcfg + i));  /* our MAC */
    w8(e + 12, 0x08); w8(e + 13, 0x06);
    for (int i = 14; i < 60; i++) w8(e + i, 0x5A);
    u32 total = 12 + 60;

    w64((u64)dma + 0, phys + 0x3000);       /* desc[0].addr  */
    w32((u64)dma + 8, total);               /* desc[0].len   */
    w16((u64)dma + 12, 0);                  /* desc[0].flags */
    w16((u64)dma + 14, 0);                  /* desc[0].next  */

    u64 av = (u64)dma + 0x1000;
    w16(av + 0, 0);                         /* avail.flags   */
    w16(av + 4, 0);                         /* avail.ring[0] = desc 0 */
    w16(av + 2, 1);                         /* avail.idx = 1 (publish)*/

    w16((u64)mmio + noff + (u64)qnoff * nmul, 1);   /* notify queue 1 */

    u64 us = (u64)dma + 0x2000;
    int ok = 0;
    for (int i = 0; i < 40000000; i++) { if (r16(us + 2)) { ok = 1; break; } }
    if (ok) {
        print("  [drv:r3] *** TX COMPLETED BY HARDWARE *** used.idx="); hex(r16(us + 2));
        print("\n  [drv:r3] the NIC read the descriptor and frame from THIS PROCESS's\n");
        print("  [drv:r3] memory via DMA, inside its capability-bound IOMMU domain.\n");
    } else {
        print("  [drv:r3] TX timeout — device did not consume the descriptor\n");
    }
}


/* Load sentinels into callee-saved regs, cross the SYSCALL boundary, and check  */
/* they survive — proving the kernel preserves (and does not leak into) them.    */
static int reg_preservation_ok(void) {
    u64 ok;
    __asm__ volatile(
        "push %%rbx\n push %%r12\n push %%r13\n push %%r14\n push %%r15\n"
        "movabs $0x1111111111111111, %%rbx\n"
        "movabs $0x2222222222222222, %%r12\n"
        "movabs $0x3333333333333333, %%r13\n"
        "movabs $0x4444444444444444, %%r14\n"
        "movabs $0x5555555555555555, %%r15\n"
        "mov $3, %%rax\n movabs $0x5EC0DE, %%rdi\n xor %%rsi,%%rsi\n xor %%rdx,%%rdx\n"
        "syscall\n"                                   /* SYS_WRITEHEX: real ring transition */
        "xor %%rax, %%rax\n"
        "movabs $0x1111111111111111, %%rcx\n cmp %%rcx, %%rbx\n jne 1f\n"
        "movabs $0x2222222222222222, %%rcx\n cmp %%rcx, %%r12\n jne 1f\n"
        "movabs $0x3333333333333333, %%rcx\n cmp %%rcx, %%r13\n jne 1f\n"
        "movabs $0x4444444444444444, %%rcx\n cmp %%rcx, %%r14\n jne 1f\n"
        "movabs $0x5555555555555555, %%rcx\n cmp %%rcx, %%r15\n jne 1f\n"
        "mov $1, %%rax\n"
        "1:\n mov %%rax, %0\n"
        "pop %%r15\n pop %%r14\n pop %%r13\n pop %%r12\n pop %%rbx\n"
        : "=r"(ok) :: "rax","rcx","rdi","rsi","rdx","r11","memory");
    return (int)ok;
}

void _start(void) {
    u64 role = sysc(SYS_ROLE, 0, 0, 0);
    if (role == 1) { nic_driver();  sysc(SYS_EXIT, 0, 0, 0); }
    if (role == 2) { surface_app(); sysc(SYS_EXIT, 0, 0, 0); }
    if (role == 3) { surface_exit_test(); }             /* exits itself         */
    if (role == 4) { ident_probe(); }                   /* exits itself         */
    if (role == 5) { tear_test(); }                     /* exits itself         */
    print("  [elf:r3] user_init.elf alive at ring 3\n");
    print(reg_preservation_ok() ? "  [elf:r3] callee-saved regs survive SYSCALL: PASS\n"
                                : "  [elf:r3] callee-saved regs survive SYSCALL: FAIL\n");

    /* --- (1) hardware passthrough --- */
    print("  [elf:r3] requesting virtio MMIO window via SYSCALL...\n");
    i64 vbase = (i64)sysc(SYS_HW_PASSTHROUGH, DEV_DEFAULT, 0, 0);
    if (vbase < 0) {
        print("  [elf:r3] passthrough DENIED, code "); hex((u64)vbase); print("\n");
    } else {
        u64 dev_off = sysc(SYS_DEV_OFFSET, 0, 0, 0);
        volatile u8 *devcfg = (volatile u8 *)((u64)vbase + dev_off);
        u64 mac = 0;
        for (int i = 0; i < 6; i++) mac = (mac << 8) | devcfg[i];
        print("  [elf:r3] virtio-net MAC read from device registers: "); hex(mac); print("\n");
    }

    /* --- (2) capability-gated VFS file read --- */
    print("  [elf:r3] opening \"motd\" via sys_open...\n");
    i64 fd = (i64)sysc(SYS_OPEN, (u64)"motd", 0, 0);
    if (fd < 0) {
        print("  [elf:r3] file access DENIED (this process lacks CAP_FILESYSTEM)\n");
    } else {
        char buf[160];
        i64 n = (i64)sysc(SYS_READ, (u64)fd, (u64)buf, sizeof buf - 1);
        if (n > 0) { buf[n] = 0; print("  [elf:r3] read from VFS -> "); print(buf); }
        sysc(SYS_CLOSE, (u64)fd, 0, 0);
    }

    /* --- (3) capability-gated framebuffer access: act as a ring-3 wm --- */
    i64 fb = (i64)sysc(SYS_MAP_FRAMEBUFFER, 0, 0, 0);
    if (fb < 0) {
        print("  [elf:r3] framebuffer access DENIED (no CAP_FRAMEBUFFER)\n");
    } else {
        print("  [elf:r3] mapped framebuffer; drawing a ring-3 marker\n");
        volatile u32 *fbp = (volatile u32 *)(u64)fb;
        u32 stride = 1024;                     /* 1024x768 mode                 */
        for (int yy = 0; yy < 90; yy++)
            for (int xx = 0; xx < 90; xx++) {
                u32 c = (xx < 3 || yy < 3 || xx > 86 || yy > 86) ? 0x00FFB020 : 0x00121722;
                fbp[(u32)(50 + yy) * stride + (u32)(880 + xx)] = c;
            }
    }

    sysc(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
