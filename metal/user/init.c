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
#define SYS_IPC_SEND       18
#define SYS_IPC_RECV       19
#define SYS_VFIO_MAP_BAR   20
#define SYS_VFIO_WAIT_IRQ  21

#define PCAP_FILESYSTEM (1ull << 5)
#define IPC_INLINE_MAX 64
#define IPC_MSG_XFER_FD  1
#define IPC_MSG_XFER_SHM 2
/* Mirrors kernel/kernel64.c's struct ipc_msg exactly (same field order/types,
 * so the kernel's raw byte copy lands correctly) — the kernel never exposes
 * this layout across the boundary any other way. */
struct ipc_msg {
    u64 sender_pid;
    u64 recipient_pid;
    u32 msg_type;
    u32 cap_mask;
    u32 payload_len;
    i64 xfer_handle;
    u8  inline_data[IPC_INLINE_MAX];
};

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

/* role 6: v0.38 multi-core scheduling probe. This thread is entered by an
 * APPLICATION PROCESSOR, not the BSP. It reads its own identity through the
 * capability path and exits with code == its pid, so the kernel can confirm
 * the AP resolved THIS thread's identity and got a clean return. */
static void mcsched_probe(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);
    print("  [mc :r3] ring-3 thread executing on an APPLICATION PROCESSOR\n");
    print("  [mc :r3] SYS_GETPID via the AP's own SYSCALL path -> pid "); hex(pid); print("\n");
    sysc(SYS_EXIT, pid, 0, 0);              /* exit code == pid; the BSP verifies */
}

/* role 7: v0.39 CONCURRENT scheduling probe. Several of these run in ring 3
 * on DIFFERENT cores at the same time. The compute loop keeps this core in
 * ring 3 long enough to overlap its siblings, and the periodic SYS_GETPID
 * re-check is the sharp edge: every syscall crosses the per-CPU entry path,
 * and if any core's identity bled into another's capability gate, the pid
 * comes back wrong and we exit 999 — which the kernel-side suite FAILs on. */
static void mcq_probe(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);
    volatile u64 acc = 0;
    for (u64 i = 0; i < 3000000ull; i++) {
        acc += i ^ pid;
        if ((i & 0xFFFFull) == 0 && sysc(SYS_GETPID, 0, 0, 0) != pid)
            sysc(SYS_EXIT, 999, 0, 0);       /* cross-core identity bleed */
    }
    sysc(SYS_EXIT, pid, 0, 0);
}

/* role 8: v0.39 long-running PREEMPTIBLE probe (Stage 3). Same identity fuzz,
 * ~10x the work: long enough for another core to preempt it mid-loop, requeue
 * its captured context on a DIFFERENT cpu, and resume it there. If the
 * capture/resume or the migration corrupted anything — registers, stack,
 * identity — the checksum loop or the pid check breaks and the exit code
 * betrays it. */
static void mcpre_long(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);
    volatile u64 acc = 0;
    for (u64 i = 0; i < 30000000ull; i++) {
        acc += i ^ pid;
        if ((i & 0x3FFFFull) == 0 && sysc(SYS_GETPID, 0, 0, 0) != pid)
            sysc(SYS_EXIT, 999, 0, 0);
    }
    sysc(SYS_EXIT, pid, 0, 0);
}

/* role 9: v0.41 CONCURRENT FILE WORKER. Several of these run in ring 3 on
 * DIFFERENT cores at once, each hammering the VFS: open its own "cio-<pid>"
 * file, COW-write a (pid,round)-tagged pattern, read it back and verify EVERY
 * byte, close — then do a racing write/read cycle on ONE SHARED file, where
 * the read must come back as a single uniform image (any interleaving of two
 * writers shows up as a mixed-tag buffer). Every failure exits a distinct
 * 7xx code; the kernel suite FAILs on anything but exit == pid.              */
#define CIO_LEN    1024
#define CIO_ROUNDS 4
static void u64_dec(u64 v, char *dst) {
    char t[20]; int n = 0, p = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) dst[p++] = t[--n];
    dst[p] = 0;
}
static void cio_file_worker(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);
    char name[20];
    name[0] = 'c'; name[1] = 'i'; name[2] = 'o'; name[3] = '-';
    u64_dec(pid, name + 4);
    unsigned char wb[CIO_LEN], rb[CIO_LEN];
    for (int r = 0; r < CIO_ROUNDS; r++) {
        /* own file: write must read back byte-exact despite sibling cores     */
        for (int i = 0; i < CIO_LEN; i++)
            wb[i] = (u8)(pid * 31 + (u64)r * 17 + (u64)i * 7);
        i64 fd = (i64)sysc(SYS_OPEN, (u64)name, 0, 0);
        if (fd < 0)                                        sysc(SYS_EXIT, 700 + (u64)r, 0, 0);
        if ((i64)sysc(SYS_WRITE_FILE, (u64)fd, (u64)wb, CIO_LEN) != CIO_LEN)
                                                           sysc(SYS_EXIT, 710 + (u64)r, 0, 0);
        for (int i = 0; i < CIO_LEN; i++) rb[i] = 0;
        if ((i64)sysc(SYS_READ, (u64)fd, (u64)rb, CIO_LEN) != CIO_LEN)
                                                           sysc(SYS_EXIT, 720 + (u64)r, 0, 0);
        for (int i = 0; i < CIO_LEN; i++)
            if (rb[i] != wb[i])                            sysc(SYS_EXIT, 730 + (u64)r, 0, 0);
        sysc(SYS_CLOSE, (u64)fd, 0, 0);

        /* shared file: whole-file atomicity across racing writers. We may     */
        /* read ANY single writer's image — but never a mix of two.            */
        i64 sfd = (i64)sysc(SYS_OPEN, (u64)"cio-shared", 0, 0);
        if (sfd < 0)                                       sysc(SYS_EXIT, 740 + (u64)r, 0, 0);
        u8 tag = (u8)(pid * 31 + (u64)r * 17);
        for (int i = 0; i < CIO_LEN; i++) wb[i] = (u8)(tag + (u64)i * 7);
        if ((i64)sysc(SYS_WRITE_FILE, (u64)sfd, (u64)wb, CIO_LEN) != CIO_LEN)
                                                           sysc(SYS_EXIT, 750 + (u64)r, 0, 0);
        if ((i64)sysc(SYS_READ, (u64)sfd, (u64)rb, CIO_LEN) != CIO_LEN)
                                                           sysc(SYS_EXIT, 760 + (u64)r, 0, 0);
        for (int i = 1; i < CIO_LEN; i++)
            if (rb[i] != (u8)(rb[0] + (u64)i * 7))         sysc(SYS_EXIT, 770 + (u64)r, 0, 0);
        sysc(SYS_CLOSE, (u64)sfd, 0, 0);

        volatile u64 acc = 0;                    /* stagger the cores so rounds */
        for (u64 i = 0; i < 120000ull; i++)      /* overlap instead of lockstep */
            acc += i ^ pid;
    }
    sysc(SYS_EXIT, pid, 0, 0);
}

/* role 10: v0.41 SURFACE CHURN. Runs on an AP: claims a churn slot (6 or 7 by
 * pid parity), paints, flips through the CPU-aware flip path, then re-creates
 * the SAME slot — which recycles its own pixel pair through the locked free
 * list — and exits, leaving reclamation to the executor's exit path.         */
static void cio_surface_churn(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);
    u64 slot = 6 + (pid & 1);
    for (int r = 0; r < 2; r++) {
        i64 va = (i64)sysc(SYS_SURFACE, (64u << 16) | 64u, slot, 0);
        if (va <= 0) sysc(SYS_EXIT, 600 + (u64)r, 0, 0);
        volatile unsigned int *back = (volatile unsigned int *)((u64)va + 64 * 64 * 4);
        for (int i = 0; i < 64 * 64; i++) back[i] = 0x00C10000u | (u32)(pid & 0xFFu);
        for (int f = 0; f < 3; f++)
            back = (volatile unsigned int *)sysc(SYS_SURFACE_FLIP, slot, 0, 0);
    }
    sysc(SYS_EXIT, pid, 0, 0);
}

/* role 11: v0.44 DMA CHURN. Requests the demo device's MMIO passthrough and
 * a small DMA buffer, touches both to prove they resolve, then exits
 * normally through the MODERN scheduler path (cpu_exec_proc). nic_driver and
 * cmd_capdma's dma-owner/dma-other already exercise SYS_HW_PASSTHROUGH and
 * SYS_DMA_ALLOC, but only via the legacy one-shot enter_process excursion,
 * which never reaches dma_teardown_kproc — this role is what actually
 * exercises real grant revocation on exit. */
static void dma_churn(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);
    i64 mmio = (i64)sysc(SYS_HW_PASSTHROUGH, 0xFFFF, 0, 0);
    if (mmio <= 0) sysc(SYS_EXIT, 800, 0, 0);
    u64 phys = 0;
    i64 dma = (i64)sysc(SYS_DMA_ALLOC, 2, (u64)&phys, 0);
    if (dma <= 0) sysc(SYS_EXIT, 810, 0, 0);
    volatile u64 *buf = (volatile u64 *)dma;
    u64 pattern = pid ^ 0xD44AC0DEull;
    buf[0] = pattern;
    if (buf[0] != pattern) sysc(SYS_EXIT, 820, 0, 0);
    sysc(SYS_EXIT, pid, 0, 0);
}

/* v0.46: role 12 — the SENDER half of an IPC handle/shared-memory exchange.
 * Transfers an open VFS fd, then a shared memory frame, to whichever pid the
 * kernel pre-seeded at "ipc-peer". The shared-memory step is deliberately
 * two-stage: a SELF-addressed send (recipient_pid == our own pid) creates
 * and maps the frame, and — because that send's own g_ipc_lock release is
 * what makes the loopback message visible at all — our own subsequent
 * pattern write is guaranteed to happen-before anything we send the REAL
 * peer next. Sending the real peer straight after the loopback (skipping
 * the write) would race: the peer could pop that notification and read the
 * shared page on another core before our write instruction ever executed. */
static void ipc_sender(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);

    i64 pfd = (i64)sysc(SYS_OPEN, (u64)"ipc-peer", 0, 0);
    if (pfd < 0) sysc(SYS_EXIT, 950, 0, 0);
    u8 pbuf[8];
    if ((i64)sysc(SYS_READ, (u64)pfd, (u64)pbuf, 8) != 8) sysc(SYS_EXIT, 951, 0, 0);
    sysc(SYS_CLOSE, (u64)pfd, 0, 0);
    u64 peer = 0; for (int i = 0; i < 8; i++) peer |= ((u64)pbuf[i]) << (8 * i);

    /* Fixed name, reused every round: unlike cio_file_worker's pid-keyed own-
     * file, this content never needs to vary, and VFS files are durable/
     * never-deleted (v0.44) — a pid-keyed name here would claim a brand-new,
     * permanent VFS_MAXFILES dirent every round, exactly the growth v0.45's
     * kpstress hit and deliberately bounded. Reopening the SAME name every
     * round still hands back a fresh, distinct global fd number each time
     * (g_ofiles is the thing that's actually per-round here, not the name). */
    i64 fd = (i64)sysc(SYS_OPEN, (u64)"ipc-payload", 0, 0);
    if (fd < 0) sysc(SYS_EXIT, 952, 0, 0);

    struct ipc_msg m;
    for (int i = 0; i < (int)sizeof m; i++) ((u8 *)&m)[i] = 0;
    m.recipient_pid = peer;
    m.msg_type = IPC_MSG_XFER_FD;
    m.cap_mask = PCAP_FILESYSTEM;
    m.payload_len = 11;
    m.xfer_handle = fd;
    const char *tag = "IPC-FD-XFER";
    for (int i = 0; i < 11; i++) m.inline_data[i] = (u8)tag[i];
    if ((i64)sysc(SYS_IPC_SEND, (u64)&m, 0, 0) != 0) sysc(SYS_EXIT, 953, 0, 0);

    struct ipc_msg self;
    for (int i = 0; i < (int)sizeof self; i++) ((u8 *)&self)[i] = 0;
    self.recipient_pid = pid;                 /* loopback: creates the shared frame */
    self.msg_type = IPC_MSG_XFER_SHM;
    self.xfer_handle = -1;                    /* -1 = allocate a NEW shared frame    */
    if ((i64)sysc(SYS_IPC_SEND, (u64)&self, 0, 0) != 0) sysc(SYS_EXIT, 954, 0, 0);
    i64 shm_id = self.xfer_handle;            /* SEND wrote back the assigned id     */
    u64 vaddr = 0; for (int i = 0; i < 8; i++) vaddr |= ((u64)self.inline_data[i]) << (8 * i);
    if (shm_id < 0 || !vaddr) sysc(SYS_EXIT, 955, 0, 0);

    struct ipc_msg drain;                      /* pop our own loopback so it doesn't  */
    if ((i64)sysc(SYS_IPC_RECV, (u64)&drain, 1, 0) != 1) sysc(SYS_EXIT, 956, 0, 0);

    volatile u64 *shm = (volatile u64 *)vaddr;
    u64 pattern = pid ^ 0x5EA5FEEDull;
    shm[0] = pattern;

    struct ipc_msg m2;
    for (int i = 0; i < (int)sizeof m2; i++) ((u8 *)&m2)[i] = 0;
    m2.recipient_pid = peer;
    m2.msg_type = IPC_MSG_XFER_SHM;
    m2.xfer_handle = shm_id;                  /* re-share the SAME, now-populated id */
    if ((i64)sysc(SYS_IPC_SEND, (u64)&m2, 0, 0) != 0) sysc(SYS_EXIT, 957, 0, 0);

    sysc(SYS_EXIT, pid, 0, 0);
}

/* v0.46: role 13 — the RECEIVER half. Blocks on SYS_IPC_RECV for each of the
 * two messages ipc_sender issues, uses the transferred fd exactly as if it
 * had opened the file itself, then reads the shared frame directly — no
 * syscall at all for that last step, which is the entire point. */
static void ipc_receiver(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);

    struct ipc_msg m1;
    if ((i64)sysc(SYS_IPC_RECV, (u64)&m1, 1, 0) != 1) sysc(SYS_EXIT, 960, 0, 0);
    if (m1.msg_type != IPC_MSG_XFER_FD) sysc(SYS_EXIT, 961, 0, 0);
    const char *tag = "IPC-FD-XFER";
    for (int i = 0; i < 11; i++) if (m1.inline_data[i] != (u8)tag[i]) sysc(SYS_EXIT, 962, 0, 0);

    u8 rb[16];
    if ((i64)sysc(SYS_READ, (u64)m1.xfer_handle, (u64)rb, 16) != 16) sysc(SYS_EXIT, 963, 0, 0);
    const char *expect = "IPC-PAYLOAD-TEST";
    for (int i = 0; i < 16; i++) if (rb[i] != (u8)expect[i]) sysc(SYS_EXIT, 964, 0, 0);
    sysc(SYS_CLOSE, (u64)m1.xfer_handle, 0, 0);

    struct ipc_msg m2;
    if ((i64)sysc(SYS_IPC_RECV, (u64)&m2, 1, 0) != 1) sysc(SYS_EXIT, 965, 0, 0);
    if (m2.msg_type != IPC_MSG_XFER_SHM) sysc(SYS_EXIT, 966, 0, 0);
    u64 vaddr = 0; for (int i = 0; i < 8; i++) vaddr |= ((u64)m2.inline_data[i]) << (8 * i);
    if (!vaddr) sysc(SYS_EXIT, 967, 0, 0);
    volatile u64 *shm = (volatile u64 *)vaddr;
    u64 pattern = m1.sender_pid ^ 0x5EA5FEEDull;
    if (shm[0] != pattern) sysc(SYS_EXIT, 968, 0, 0);

    sysc(SYS_EXIT, pid, 0, 0);
}

/* v0.47: role 14 — a ring-3 "driver" for the dummy VFIO test device. Maps
 * both of its BARs directly into its own address space (no kernel mediation
 * of the reads/writes that follow — that's the whole point of VFIO-style
 * passthrough), then blocks on the device's routed interrupt. */
static void vfio_driver(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);

    i64 didfd = (i64)sysc(SYS_OPEN, (u64)"vfio-devid", 0, 0);
    if (didfd < 0) sysc(SYS_EXIT, 980, 0, 0);
    u8 idbuf[8];
    if ((i64)sysc(SYS_READ, (u64)didfd, (u64)idbuf, 8) != 8) sysc(SYS_EXIT, 981, 0, 0);
    sysc(SYS_CLOSE, (u64)didfd, 0, 0);
    u64 devid = 0; for (int i = 0; i < 8; i++) devid |= ((u64)idbuf[i]) << (8 * i);

    i64 v0 = (i64)sysc(SYS_VFIO_MAP_BAR, devid, 0, 0);
    if (v0 <= 0) sysc(SYS_EXIT, 982, 0, 0);
    volatile u32 *bar0 = (volatile u32 *)v0;
    if (*bar0 != 0xCAFEBABEu) sysc(SYS_EXIT, 983, 0, 0);

    i64 v1 = (i64)sysc(SYS_VFIO_MAP_BAR, devid, 1, 0);
    if (v1 <= 0) sysc(SYS_EXIT, 984, 0, 0);
    volatile u64 *bar1 = (volatile u64 *)v1;
    u64 pattern = pid ^ 0xB4700000ull;
    bar1[0] = pattern;
    if (bar1[0] != pattern) sysc(SYS_EXIT, 985, 0, 0);

    i64 fired = (i64)sysc(SYS_VFIO_WAIT_IRQ, 16, 2000, 0);
    if (fired != 1) sysc(SYS_EXIT, 986, 0, 0);

    sysc(SYS_EXIT, pid, 0, 0);
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
    if (role == 6) { mcsched_probe(); }                 /* exits itself (on an AP) */
    if (role == 7) { mcq_probe(); }                     /* concurrent multi-core probe */
    if (role == 8) { mcpre_long(); }                    /* preemptible long probe      */
    if (role == 9) { cio_file_worker(); }               /* v0.41 concurrent file worker */
    if (role == 10) { cio_surface_churn(); }            /* v0.41 surface churn (AP)     */
    if (role == 11) { dma_churn(); }                    /* v0.44 DMA/passthrough churn  */
    if (role == 12) { ipc_sender(); }                   /* v0.46 IPC handle/shmem sender */
    if (role == 13) { ipc_receiver(); }                 /* v0.46 IPC handle/shmem receiver */
    if (role == 14) { vfio_driver(); }                  /* v0.47 VFIO BAR map + IRQ wait   */
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
