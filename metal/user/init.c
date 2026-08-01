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
#define SYS_VFS_SYNC       22
#define SYS_VFS_UNLINK     23
#define SYS_TLB_SHOOTDOWN  24
#define SYS_SET_AFFINITY   25
#define SYS_SMP_REMAP      26
#define SYS_SMP_UNMAP      27
#define SYS_GET_CPU        28
#define SYS_GPU_RESOURCE_CREATE 29
#define SYS_GPU_SET_SCANOUT     30
#define SYS_GPU_SUBMIT_FLUSH    31
#define SYS_GPU_FENCE_WAIT      32
#define SYS_AUDIO_CONFIGURE     33
#define SYS_AUDIO_WRITE         34
#define SYS_SOCKET              35
#define SYS_BIND                36
#define SYS_CONNECT             37
#define SYS_SEND                38
#define SYS_RECV                39
#define SYS_WIN_CREATE          40
#define SYS_WIN_DAMAGE          41
#define SYS_WIN_POLL            42
#define SYS_WIN_INFO            43
#define SYS_SYSINFO             44
#define SYS_READDIR             45
#define SYS_RUN_CMD             46
/* v0.55: POSIX process / signal / thread calls */
#define SYS_FORK                47
#define SYS_EXECVE              48
#define SYS_SIGACTION           49
#define SYS_KILL                50
#define SYS_SIGRETURN           51
#define SYS_THREAD_CREATE       52
#define SYS_THREAD_EXIT         53
#define SYS_ALARM               54
#define SYS_GETPPID             55
#define SYS_WAITPID             56
#define SYS_SIGUNMASK           57
#define SYS_BRK                 58
#define SYS_EXECVE_PATH         59
#define SYS_TTY_READ            60
#define SYS_STAT                61
/* v0.59. SYS_PIPE fills TWO 64-BIT WORDS: out[0] read end, out[1] write end.
 * SYS_SETREDIR(which, fd) points stdin (0) or stdout (1) at a descriptor, or
 * -1 for the console; it is kproc state, so it survives SYS_EXECVE_PATH. */
#define SYS_PIPE                62
#define SYS_SETREDIR            63
/* v0.61: thread synchronisation. SYS_THREAD_CREATE gained a third argument
 * (a caller-supplied stack top; 0 = "kernel, give me one"), which is a pure
 * extension — the two-argument form still means exactly what it did.
 *
 * SYS_FUTEX_WAIT(uaddr, val, timeout_ticks) sleeps only if *uaddr == val, and
 * that compare-and-sleep is atomic against SYS_FUTEX_WAKE. Without the kernel
 * doing the comparison there is no way to close the window between reading the
 * word and going to sleep, which is the entire reason a futex is a syscall.
 * A parked thread occupies no core; before this the only way to wait was to
 * spin or yield in a loop, and on a uniprocessor spinning for a sibling thread
 * is simply a deadlock.
 *
 * Every wait is bounded: 0 means "the kernel's default", never "forever". */
#define SYS_FUTEX_WAIT          64
#define SYS_FUTEX_WAKE          65
#define SYS_THREAD_JOIN         66
#define SYS_GETTID              67
/* v0.62: job control + per-thread signal masks. SIGACTION/KILL/SIGRETURN keep
 * the numbers they have had since v0.55 (49/50/51) — renumbering working
 * syscalls would break every binary already compiled into the VFS. */
#define SYS_SETPGID             68
#define SYS_KILLPG              69
#define SYS_SIGPROCMASK         70
#define EAGAIN_NEG    (-11)
#define ETIMEDOUT_NEG (-62)
/* Mirrors the kernel's HEAP_USER_V (kernel64.c is the master). */
#define HEAP_USER_V_LO 0x0000570000000000ull

#define SIGINT   2
#define SIGKILL  9
#define SIGSEGV 11
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define NSIG    32

#define PCAP_SMP_ADMIN (1ull << 9)

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

/* v0.48: role 15 — exercises the new journaling/reclamation/multi-volume
 * surface entirely through real ring-3 syscalls: reads a kernel-pre-seeded
 * ROOT file, overwrites it, syncs the journal, unlinks it and confirms it's
 * gone, creates/writes/reads a TMP file, and reads (but is refused writing)
 * the DEV volume's read-only device listing. */
static void vfs_driver(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);
    static const u8 seed[16] = "VFS-SEED-PATTERN";

    i64 fd = (i64)sysc(SYS_OPEN, (u64)"vfs-stress", 0, 0);
    if (fd < 0) sysc(SYS_EXIT, 1001, 0, 0);
    u8 rb[16];
    if ((i64)sysc(SYS_READ, (u64)fd, (u64)rb, 16) != 16) sysc(SYS_EXIT, 1002, 0, 0);
    for (int i = 0; i < 16; i++) if (rb[i] != seed[i]) sysc(SYS_EXIT, 1002, 0, 0);

    u8 pat2[8]; for (int i = 0; i < 8; i++) pat2[i] = (u8)(pid >> (8 * i)) ^ 0xAB;
    if ((i64)sysc(SYS_WRITE_FILE, (u64)fd, (u64)pat2, 8) != 8) sysc(SYS_EXIT, 1003, 0, 0);
    sysc(SYS_CLOSE, (u64)fd, 0, 0);

    sysc(SYS_VFS_SYNC, 0, 0, 0);   /* return value not asserted here — the kernel harness checks disk state directly */

    if ((i64)sysc(SYS_VFS_UNLINK, (u64)"vfs-stress", 0, 0) != 0) sysc(SYS_EXIT, 1004, 0, 0);
    i64 fd2 = (i64)sysc(SYS_OPEN, (u64)"vfs-stress", 0, 0);
    if (fd2 >= 0) sysc(SYS_EXIT, 1005, 0, 0);          /* must be gone after unlink */

    i64 tfd = (i64)sysc(SYS_OPEN, (u64)"tmp/scratch", 0, 0);
    if (tfd < 0) sysc(SYS_EXIT, 1006, 0, 0);
    u8 tpat[8]; for (int i = 0; i < 8; i++) tpat[i] = (u8)(pid >> (8 * i)) ^ 0x7C;
    if ((i64)sysc(SYS_WRITE_FILE, (u64)tfd, (u64)tpat, 8) != 8) sysc(SYS_EXIT, 1007, 0, 0);
    u8 trb[8];
    if ((i64)sysc(SYS_READ, (u64)tfd, (u64)trb, 8) != 8) sysc(SYS_EXIT, 1008, 0, 0);
    for (int i = 0; i < 8; i++) if (trb[i] != tpat[i]) sysc(SYS_EXIT, 1008, 0, 0);
    sysc(SYS_CLOSE, (u64)tfd, 0, 0);

    i64 dfd = (i64)sysc(SYS_OPEN, (u64)"dev/devices", 0, 0);
    if (dfd < 0) sysc(SYS_EXIT, 1009, 0, 0);
    u8 devbuf[256];
    if ((i64)sysc(SYS_READ, (u64)dfd, (u64)devbuf, sizeof devbuf) < 0) sysc(SYS_EXIT, 1010, 0, 0);
    u8 one = 'x';
    if ((i64)sysc(SYS_WRITE_FILE, (u64)dfd, (u64)&one, 1) >= 0) sysc(SYS_EXIT, 1011, 0, 0);  /* must be denied */
    sysc(SYS_CLOSE, (u64)dfd, 0, 0);

    sysc(SYS_EXIT, pid, 0, 0);
}

/* v0.49: role 16 — SMP MIGRATION/REMAP WORKER. Several of these run in ring 3
 * across every core at once, each rapidly remapping and unmapping its own
 * private scratch page through SYS_SMP_REMAP/SYS_SMP_UNMAP (a fresh physical
 * frame every round, the kernel shooting down and reclaiming the old one),
 * writing and immediately re-reading a per-round pattern through the mapping
 * to catch any stale TLB translation surviving a remap, and yielding between
 * rounds so the scheduler is free to migrate this thread to a different core
 * mid-run. Half the workers pin themselves to a single cpu via
 * SYS_SET_AFFINITY first, so the kernel-side suite can confirm affinity was
 * actually honoured (ran_on never left the pinned mask). Any stale read,
 * denied syscall, or wrong SYS_GETPID exits a distinct 9xx code; success
 * exits == pid, same convention as every other concurrent probe here.       */
#define SMP_MIG_SLOT   0
#define SMP_MIG_ROUNDS 6
static void smp_migrate_worker(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);

    if (pid & 1) {                                  /* odd pids: pin to cpu 0  */
        if ((i64)sysc(SYS_SET_AFFINITY, 1, 0, 0) != 0) sysc(SYS_EXIT, 900, 0, 0);
    }

    for (int r = 0; r < SMP_MIG_ROUNDS; r++) {
        i64 va = (i64)sysc(SYS_SMP_REMAP, SMP_MIG_SLOT, 0, 0);
        if (va <= 0) sysc(SYS_EXIT, 910 + r, 0, 0);
        volatile u64 *p = (volatile u64 *)va;
        u64 pattern = pid ^ ((u64)r * 0x9E3779B97F4A7C15ull);
        p[0] = pattern;
        if (p[0] != pattern) sysc(SYS_EXIT, 920 + r, 0, 0);     /* stale TLB read */

        /* NOT SYS_YIELD here: that syscall unconditionally drives the legacy
         * BSP cooperative scheduler (sched_yield()/g_threads), which has no
         * idea this task is running under the per-CPU cpu_exec_proc executor
         * — calling it from a cpu_exec_proc-dispatched context (discovered
         * live building this worker) switches away into unrelated BSP thread
         * state and never returns, hanging the whole suite. A real gap, but
         * a pre-existing, cross-cutting one outside this milestone's scope
         * (see CHANGELOG-0.49.0.md); this worker steers around it instead.
         * A scheduling point still exists here for migration to act on: the
         * remap/shootdown round trip just above already crosses a syscall
         * boundary the scheduler can preempt or steal around.               */
        if (p[0] != pattern) sysc(SYS_EXIT, 930 + r, 0, 0);     /* survived a migration? */
        if (sysc(SYS_GETPID, 0, 0, 0) != pid) sysc(SYS_EXIT, 940 + r, 0, 0);

        /* v0.49: exercise the raw primitive directly too, not just through the
         * remap path that already calls it internally — invalidate our own
         * mapping on every online cpu and confirm the pattern still reads
         * back correctly afterward (this cpu re-establishes its own TLB entry
         * on the next access; a broken shootdown could corrupt someone else's,
         * not ours, so this is a liveness/API check, not a corruption one).  */
        sysc(SYS_TLB_SHOOTDOWN, (u64)va, 1, 0xFFFFFFFFull);
        if (p[0] != pattern) sysc(SYS_EXIT, 950 + r, 0, 0);
    }

    if ((i64)sysc(SYS_SMP_UNMAP, SMP_MIG_SLOT, 0, 0) != 0) sysc(SYS_EXIT, 960, 0, 0);
    sysc(SYS_EXIT, pid, 0, 0);
}

/* v0.50: role 17/18 — a ring-3 "GPU client". Creates a real virtio-gpu 2D
 * resource, draws directly into its mapped backing (zero-copy at the app
 * level: no syscall touches the pixel data, only ordinary memory writes),
 * sets it as the scanout, submits an async flush, and fence-waits for it.
 * Role 18 deliberately faults right after creating the resource (before
 * ever flushing or exiting cleanly) so cmd_gpu_stress can prove
 * gpu_teardown_kproc reclaims the resource/grant via the FAULT exit path,
 * not just SYS_EXIT — mirrors v0.44/45's fault-injection precedent.        */
static void gpu_driver(int fault_before_flush) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);

    u64 resid = 0;
    i64 vaddr = (i64)sysc(SYS_GPU_RESOURCE_CREATE, 64, 64, (u64)&resid);
    if (vaddr <= 0) sysc(SYS_EXIT, 1101, 0, 0);

    volatile u32 *px = (volatile u32 *)vaddr;
    u32 pattern = 0xFF000000u | (u32)(pid & 0x00FFFFFFu);
    for (int i = 0; i < 64 * 64; i++) px[i] = pattern;      /* direct draw: zero-copy */
    if (px[0] != pattern) sysc(SYS_EXIT, 1102, 0, 0);

    if (fault_before_flush) {
        volatile u32 *bad = (volatile u32 *)0x1;
        *bad = 0xDEAD;                                      /* deliberate fault: never reached past here */
    }

    if ((i64)sysc(SYS_GPU_SET_SCANOUT, resid, 64, 64) != 0) sysc(SYS_EXIT, 1103, 0, 0);
    i64 fence = (i64)sysc(SYS_GPU_SUBMIT_FLUSH, resid, 64, 64);
    if (fence <= 0) sysc(SYS_EXIT, 1104, 0, 0);
    if ((i64)sysc(SYS_GPU_FENCE_WAIT, (u64)fence, 2000, 0) != 1) sysc(SYS_EXIT, 1105, 0, 0);

    sysc(SYS_EXIT, pid, 0, 0);
}

/* v0.51: role 19/20 — a ring-3 "audio client". Configures a real virtio-
 * sound PCM stream, then writes several buffers of a tone pattern directly
 * into the mapped backing (zero-copy: no syscall touches the sample data,
 * only ordinary memory writes), each write blocking until the device
 * confirms it played — the same "direct draw + blocking confirm" shape as
 * v0.50's GPU driver, applied to audio. Role 20 deliberately faults right
 * after configuring (before any write or its own exit) so cmd_audio_stress
 * can prove audio_teardown_kproc reclaims the stream via the FAULT exit
 * path, not just SYS_EXIT — the same v0.44/45/50 fault-injection precedent.
 */
static void audio_driver(int fault_after_configure) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);

    i64 vaddr = (i64)sysc(SYS_AUDIO_CONFIGURE, 48000, 2, 0);
    if (vaddr <= 0) sysc(SYS_EXIT, 1201, 0, 0);

    if (fault_after_configure) {
        volatile u32 *bad = (volatile u32 *)0x1;
        *bad = 0xDEAD;                                      /* deliberate fault: never reached past here */
    }

    volatile u16 *pcm = (volatile u16 *)vaddr;
    for (int round = 0; round < 4; round++) {               /* v0.51: multi-buffer streaming */
        for (int i = 0; i < 512; i++) pcm[i] = (u16)((pid + (u64)round * 97 + (u64)i) & 0xFFFF);
        if ((i64)sysc(SYS_AUDIO_WRITE, 1024, 2000, 0) != 1) sysc(SYS_EXIT, 1202 + round, 0, 0);
    }

    sysc(SYS_EXIT, pid, 0, 0);
}

/* v0.52: role 21/22 — a ring-3 "socket client". Creates a datagram socket,
 * binds a local port, connects to 127.0.0.1:<same port>, then sends several
 * datagrams to itself and reads each back, verifying the bytes round-trip
 * exactly through the kernel's loopback delivery — the full
 * SOCKET/BIND/CONNECT/SEND/RECV path exercised deterministically (no reliance
 * on QEMU SLIRP timing). It also fires one REAL outbound frame at the SLIRP
 * gateway to exercise vnet_tx from the socket path (best-effort, no reply
 * awaited). Role 22 deliberately faults right after BIND (before connect/send
 * or its own exit) so cmd_net_stress can prove net_teardown_kproc reclaims the
 * socket via the FAULT exit path, not just SYS_EXIT — the same v0.44/45/50/51
 * fault-injection precedent. */
static void net_driver(int fault_after_bind) {
    u64 pid  = sysc(SYS_GETPID, 0, 0, 0);
    u16 port = (u16)(4000 + (pid & 0x3FF));

    i64 fd = (i64)sysc(SYS_SOCKET, 2, 2, 0);                 /* AF_INET, SOCK_DGRAM */
    if (fd < 0) sysc(SYS_EXIT, 1301, 0, 0);
    if ((i64)sysc(SYS_BIND, (u64)fd, (u64)port, 0) != 0) sysc(SYS_EXIT, 1302, 0, 0);

    if (fault_after_bind) {
        volatile u32 *bad = (volatile u32 *)0x1;
        *bad = 0xDEAD;                                       /* deliberate fault: socket must still be reclaimed */
    }

    if ((i64)sysc(SYS_CONNECT, (u64)fd, 0x7F000001ull, (u64)port) != 0)  /* 127.0.0.1:port */
        sysc(SYS_EXIT, 1303, 0, 0);

    u8 buf[64], rbuf[64];
    for (int round = 0; round < 4; round++) {               /* self-loopback round trip */
        for (int i = 0; i < 32; i++) buf[i] = (u8)((pid + (u64)round * 31 + (u64)i) & 0xFF);
        if ((i64)sysc(SYS_SEND, (u64)fd, (u64)buf, 32) != 32) sysc(SYS_EXIT, 1310 + round, 0, 0);
        i64 got = (i64)sysc(SYS_RECV, (u64)fd, (u64)rbuf, 64);
        if (got != 32) sysc(SYS_EXIT, 1320 + round, 0, 0);
        for (int i = 0; i < 32; i++) if (rbuf[i] != buf[i]) sysc(SYS_EXIT, 1330 + round, 0, 0);
    }

    /* open a second socket to exercise multi-socket allocation + teardown. We
     * deliberately do NOT transmit any real (non-loopback) frame from this
     * churn suite: the socket layer DOES support real vnet_tx for non-loopback
     * destinations, but that path is already proven by the existing
     * net/nicdrv/cio suites, and firing frames on the shared physical NIC TX
     * queue here would perturb cmd_capdma (which runs later and depends on a
     * quiescent TX queue to produce its confined-DMA fault). See
     * CHANGELOG-0.52.0.md. */
    i64 fd2 = (i64)sysc(SYS_SOCKET, 2, 2, 0);
    if (fd2 >= 0) {
        sysc(SYS_BIND, (u64)fd2, (u64)(port + 1), 0);
        sysc(SYS_CONNECT, (u64)fd2, 0x7F000001ull, (u64)(port + 1));  /* bound but unused */
    }

    sysc(SYS_EXIT, pid, 0, 0);
}

/* v0.53: role 23/24 — a ring-3 "WIMP app". Creates three top-level windows,
 * draws a pattern into each window's 32x32 ARGB content thumbnail (mapped by
 * the kernel at WIN_USER_V + id*4096), damages them to request recomposition,
 * and polls for routed input events, then exits — so cmd_wimp_stress can prove
 * the window manager creates/composites and, on teardown, destroys every
 * window and reclaims every content grant. Role 24 deliberately faults right
 * after creating its windows (before damage/poll or its own exit) to prove
 * wimp_teardown_kproc reclaims via the FAULT exit path, not just SYS_EXIT. */
/* ===========================================================================
 * v0.54: RING-3 GUI APPLICATION CLIENT LIBRARY
 * ===========================================================================
 * Everything a windowed ring-3 app needs, entirely in userland on top of the
 * v0.53 window syscalls: window creation with a real full-resolution content
 * surface, a non-blocking event loop with abstract event types, an 8x8 bitmap
 * font, and drawing primitives that write straight into the shared surface
 * (zero-copy — no syscall touches pixel data). */
#define WIN_SURF_BASE  0x0000550000000000ull
/* MUST match the kernel's WIN_SURF_MAXB exactly, and must be page-aligned:
 * 600*440*4 rounded up to whole pages = 258 pages. */
#define WIN_SURF_STRIDE (258ull * 4096ull)

/* abstract event types the app sees (mapped from the kernel's raw sevent) */
#define EVENT_NONE         0
#define EVENT_MOUSE_CLICK  1
#define EVENT_KEY_DOWN     2
#define EVENT_WIN_CLOSE    3
#define EVENT_REDRAW       4

struct app_event { int type, x, y, code; };

struct app_win {
    int id, cw, ch;
    volatile u32 *surf;
    u32 fg, bg;
};

/* 8x8 font: digits, uppercase, and the handful of punctuation the apps use.
 * Compact on purpose — a full 96-glyph ROM would dwarf the apps themselves. */
static const u8 g_afont[43][8] = {
 {0,0,0,0,0,0,0,0},                                        /* ' '  */
 {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0},                   /* 0 */
 {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0},                   /* 1 */
 {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0},                   /* 2 */
 {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0},                   /* 3 */
 {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0},                   /* 4 */
 {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0},                   /* 5 */
 {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0},                   /* 6 */
 {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0},                   /* 7 */
 {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0},                   /* 8 */
 {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0},                   /* 9 */
 {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0},                   /* A */
 {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0},                   /* B */
 {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0},                   /* C */
 {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0},                   /* D */
 {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0},                   /* E */
 {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0},                   /* F */
 {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0},                   /* G */
 {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0},                   /* H */
 {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0},                   /* I */
 {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0},                   /* J */
 {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0},                   /* K */
 {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0},                   /* L */
 {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0},                   /* M */
 {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0},                   /* N */
 {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0},                   /* O */
 {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0},                   /* P */
 {0x3C,0x66,0x66,0x66,0x6E,0x6C,0x36,0},                   /* Q */
 {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0},                   /* R */
 {0x3E,0x60,0x60,0x3C,0x06,0x06,0x7C,0},                   /* S */
 {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0},                   /* T */
 {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0},                   /* U */
 {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0},                   /* V */
 {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0},                   /* W */
 {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0},                   /* X */
 {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0},                   /* Y */
 {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0},                   /* Z */
 {0,0,0,0,0,0,0x18,0},                                     /* . */
 {0,0,0,0x7E,0,0,0,0},                                     /* - */
 {0,0x18,0x18,0x7E,0x18,0x18,0,0},                         /* + */
 {0,0,0x18,0,0,0x18,0,0},                                  /* : */
 {0x3C,0x66,0x0C,0x18,0x18,0,0x18,0},                      /* ? */
 {0x00,0x18,0x3C,0x7E,0x3C,0x18,0x00,0},                   /* * (cursor/unknown) */
};
/* map an ASCII byte onto the compact font table */
static int afont_idx(char c) {
    if (c == ' ') return 0;
    if (c >= '0' && c <= '9') return 1 + (c - '0');
    if (c >= 'A' && c <= 'Z') return 11 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 11 + (c - 'a');        /* fold case */
    if (c == '.') return 37;
    if (c == '-') return 38;
    if (c == '+') return 39;
    if (c == ':') return 40;
    if (c == '?') return 41;
    return 42;                                              /* unknown glyph */
}

static void app_px(struct app_win *W, int x, int y, u32 c) {
    if (x < 0 || y < 0 || x >= W->cw || y >= W->ch) return;
    W->surf[y * W->cw + x] = c;
}
static void app_fill(struct app_win *W, u32 c) {
    for (int i = 0; i < W->cw * W->ch; i++) W->surf[i] = c;
}
static void app_rect(struct app_win *W, int x, int y, int w, int h, u32 c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) app_px(W, x + i, y + j, c);
}
static void app_char(struct app_win *W, int x, int y, char ch, u32 c) {
    const u8 *g = g_afont[afont_idx(ch)];
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 8; b++)
            if (g[r] & (0x80 >> b)) app_px(W, x + b, y + r, c);
}
static void app_str(struct app_win *W, int x, int y, const char *s, u32 c) {
    for (; *s; s++) { app_char(W, x, y, *s, c); x += 8; }
}
static void app_u32(struct app_win *W, int x, int y, u32 v, u32 c) {
    char b[12]; int n = 0;
    if (!v) b[n++] = '0';
    while (v && n < 11) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) app_char(W, x + i * 8, y, b[n - 1 - i], c);
}

/* Create a window and bind its content surface. Returns 0 on success. */
static int app_create(struct app_win *W, int w, int h, u32 accent) {
    i64 id = (i64)sysc(SYS_WIN_CREATE, ((u64)w << 16) | (u64)h, accent, 0);
    if (id < 0) return -1;
    i64 dims = (i64)sysc(SYS_WIN_INFO, (u64)id, 0, 0);
    if (dims < 0) return -1;
    W->id = (int)id;
    W->cw = (int)((u64)dims >> 16); W->ch = (int)((u64)dims & 0xFFFF);
    W->surf = (volatile u32 *)(WIN_SURF_BASE + (u64)id * WIN_SURF_STRIDE);
    W->fg = 0xEAF2F7; W->bg = 0x0A0D14;
    return 0;
}
/* Publish the current surface contents (request recomposition). */
static void app_damage(struct app_win *W) { sysc(SYS_WIN_DAMAGE, (u64)W->id, 0, 0); }

/* NON-BLOCKING event poll: returns 1 and fills *ev if an event was pending,
 * 0 otherwise. Maps the kernel's raw sevent (1=click, 2=key) onto the app-level
 * event vocabulary and never blocks — the caller owns its own loop cadence. */
static int app_poll_events(struct app_win *W, struct app_event *ev) {
    struct { int type, x, y, code; } raw;
    if ((i64)sysc(SYS_WIN_POLL, (u64)W->id, (u64)&raw, 0) != 1) { ev->type = EVENT_NONE; return 0; }
    ev->x = raw.x; ev->y = raw.y; ev->code = raw.code;
    if (raw.type == 1)      ev->type = EVENT_MOUSE_CLICK;
    else if (raw.type == 2) ev->type = (raw.code == 27) ? EVENT_WIN_CLOSE : EVENT_KEY_DOWN;
    else                    ev->type = EVENT_REDRAW;
    return 1;
}

/* ---- APP 1: CYBER-TERMINAL — a windowed shell -------------------------------
 * A real terminal emulator: a character grid rendered with the app font, a
 * typed command line with backspace/enter, scrollback that scrolls when full,
 * and command execution through SYS_RUN_CMD, whose captured kernel stdout is
 * written back into the grid. */
#define TERM_COLS 46
#define TERM_ROWS 22
struct term_state {
    char grid[TERM_ROWS][TERM_COLS + 1];
    int  row, col;
    char cmd[64]; int cmdlen;
};
static void term_newline(struct term_state *T) {
    T->col = 0;
    if (++T->row >= TERM_ROWS) {                            /* scroll history up */
        for (int r = 0; r < TERM_ROWS - 1; r++)
            for (int c = 0; c <= TERM_COLS; c++) T->grid[r][c] = T->grid[r + 1][c];
        for (int c = 0; c <= TERM_COLS; c++) T->grid[TERM_ROWS - 1][c] = 0;
        T->row = TERM_ROWS - 1;
    }
}
static void term_putc(struct term_state *T, char ch) {
    if (ch == '\n') { term_newline(T); return; }
    if (ch == '\r') return;
    if (T->col >= TERM_COLS) term_newline(T);
    T->grid[T->row][T->col++] = ch;
}
static void term_puts(struct term_state *T, const char *s) { for (; *s; s++) term_putc(T, *s); }
static void term_render(struct app_win *W, struct term_state *T) {
    app_fill(W, 0x05060A);
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS && T->grid[r][c]; c++)
            app_char(W, 2 + c * 8, 2 + r * 9, T->grid[r][c], 0x3DF5C4);
    /* prompt line + typed command + block cursor */
    int py = 2 + T->row * 9;
    app_char(W, 2, py, '>', 0x22E4FF);
    for (int i = 0; i < T->cmdlen; i++) app_char(W, 2 + (i + 2) * 8, py, T->cmd[i], 0xEAF2F7);
    app_rect(W, 2 + (T->cmdlen + 2) * 8, py, 7, 8, 0xFFB020);
}
static void term_exec(struct term_state *T) {
    static char out[1024];
    T->cmd[T->cmdlen] = 0;
    term_newline(T);
    i64 n = (i64)sysc(SYS_RUN_CMD, (u64)T->cmd, (u64)out, sizeof out);
    if (n > 0) for (i64 i = 0; i < n; i++) term_putc(T, out[i]);
    else term_puts(T, "?\n");
    T->cmdlen = 0;
}

static void app_terminal(int frames) {
    struct app_win W; struct term_state T;
    for (int r = 0; r < TERM_ROWS; r++) for (int c = 0; c <= TERM_COLS; c++) T.grid[r][c] = 0;
    T.row = 0; T.col = 0; T.cmdlen = 0;
    if (app_create(&W, 400, 230, 0x3DF5C4) != 0) sysc(SYS_EXIT, 1501, 0, 0);
    term_puts(&T, "OUTRUN CYBER-TERMINAL\n");
    term_render(&W, &T); app_damage(&W);

    for (int f = 0; f < frames; f++) {                      /* bounded event loop */
        struct app_event ev;
        while (app_poll_events(&W, &ev)) {
            if (ev.type == EVENT_KEY_DOWN) {
                char ch = (char)ev.code;
                if (ch == '\n')      term_exec(&T);
                else if (ch == 8)  { if (T.cmdlen) T.cmdlen--; }
                else if (T.cmdlen < (int)sizeof T.cmd - 1) T.cmd[T.cmdlen++] = ch;
            } else if (ev.type == EVENT_WIN_CLOSE) { f = frames; break; }
        }
        term_render(&W, &T);
        app_damage(&W);
    }
    /* headless proof-of-life: execute one REAL shell command through the
     * terminal's own path so the captured output lands in the grid. */
    T.cmd[0]='p'; T.cmd[1]='s'; T.cmdlen=2; term_exec(&T);
    term_render(&W, &T); app_damage(&W);
}

/* ---- APP 2: SYSTEM MONITOR — processes, cores, memory ---------------------- */
struct sysinfo_hdr { u32 ncpu, nproc, frames_used, frames_free, ram_mb, ticks; };
struct sysinfo_ent { u32 pid, flags; char name[24]; };

static void app_sysmon(int frames) {
    struct app_win W;
    if (app_create(&W, 360, 260, 0x22E4FF) != 0) sysc(SYS_EXIT, 1601, 0, 0);
    static u8 buf[sizeof(struct sysinfo_hdr) + 12 * 32];
    for (int f = 0; f < frames; f++) {
        struct app_event ev;
        while (app_poll_events(&W, &ev)) if (ev.type == EVENT_WIN_CLOSE) { f = frames; break; }

        i64 n = (i64)sysc(SYS_SYSINFO, (u64)buf, 0, 0);
        struct sysinfo_hdr *H = (struct sysinfo_hdr *)buf;
        struct sysinfo_ent *E = (struct sysinfo_ent *)(buf + sizeof *H);
        app_fill(&W, 0x0A0D14);
        app_str(&W, 4, 2, "SYSTEM MONITOR", 0x22E4FF);
        app_str(&W, 4, 14, "CORES:", 0x7C8CA0); app_u32(&W, 60, 14, H->ncpu, 0xEAF2F7);
        app_str(&W, 4, 24, "RAM MB:", 0x7C8CA0); app_u32(&W, 68, 24, H->ram_mb, 0xEAF2F7);
        app_str(&W, 4, 34, "FRAMES:", 0x7C8CA0); app_u32(&W, 68, 34, H->frames_used, 0xFFB020);
        app_str(&W, 4, 44, "FREE:", 0x7C8CA0);   app_u32(&W, 52, 44, H->frames_free, 0x3DF5C4);
        /* per-core utilisation bars (kernel exposes core count; bar length is a
         * simple tick-phase animation, labelled as activity not measured load) */
        for (u32 c = 0; c < H->ncpu && c < 4; c++) {
            app_rect(&W, 4, 58 + (int)c * 8, 100, 5, 0x121722);
            app_rect(&W, 4, 58 + (int)c * 8, 20 + (int)((H->ticks + c * 17) % 60), 5, 0x22E4FF);
        }
        app_str(&W, 4, 96, "PROCESSES", 0x22E4FF);
        for (i64 i = 0; i < n && i < 12; i++) {
            int yy = 108 + (int)i * 10;
            app_u32(&W, 4, yy, E[i].pid, E[i].flags ? 0x7C8CA0 : 0x3DF5C4);
            app_str(&W, 44, yy, E[i].name, E[i].flags ? 0x7C8CA0 : 0xEAF2F7);
        }
        app_damage(&W);
    }
}

/* ---- APP 3: FILE INSPECTOR — VFS browser + hex/bitmap view ----------------- */
/* Mirrors the kernel's SYS_READDIR output record exactly (kernel64.c case 45 is
 * the master). v0.56 widened it from 32 to 64 name bytes for hierarchical paths,
 * so this MUST move in lockstep — a stale 40-byte struct here would have the
 * kernel write 72 bytes into it. */
#define VFS_NAME_MAX 64
struct rd_ent { u32 len, used; char name[VFS_NAME_MAX]; };

static void app_filer(int frames) {
    struct app_win W;
    if (app_create(&W, 380, 250, 0xFF2D9B) != 0) sysc(SYS_EXIT, 1701, 0, 0);
    for (int f = 0; f < frames; f++) {
        struct app_event ev;
        while (app_poll_events(&W, &ev)) if (ev.type == EVENT_WIN_CLOSE) { f = frames; break; }

        app_fill(&W, 0x0A0D14);
        app_str(&W, 4, 2, "FILE INSPECTOR", 0xFF2D9B);
        int row = 0;
        struct rd_ent e;
        for (int i = 0; i < 29 && row < 14; i++) {
            if ((i64)sysc(SYS_READDIR, (u64)i, (u64)&e, 0) != 1) break;
            if (!e.used) continue;
            int yy = 16 + row * 10;
            app_str(&W, 4, yy, e.name, 0xEAF2F7);
            app_u32(&W, 240, yy, e.len, 0x7C8CA0);
            row++;
        }
        if (!row) app_str(&W, 4, 16, "NO FILES", 0x7C8CA0);
        /* content preview of the first file, as a raw byte-intensity bitmap —
         * the same path an image viewer would use for raw framebuffer dumps */
        i64 fd = (i64)sysc(SYS_OPEN, (u64)"README", 0, 0);
        if (fd >= 0) {
            static u8 fb[256];
            i64 got = (i64)sysc(SYS_READ, (u64)fd, (u64)fb, sizeof fb);
            app_str(&W, 4, 160, "PREVIEW:", 0x22E4FF);
            for (i64 b = 0; b < got && b < 256; b++) {
                int bx = 4 + (int)(b % 32) * 5, by = 174 + (int)(b / 32) * 5;
                u32 v = (u32)fb[b];
                app_rect(&W, bx, by, 4, 4, (v << 16) | (v << 8) | v);
            }
            sysc(SYS_CLOSE, (u64)fd, 0, 0);
        }
        app_damage(&W);
    }
}

/* v0.54 role 25/26: a GUI app harness. `which` selects terminal/sysmon/filer so
 * cmd_apps_stress can run three DIFFERENT real apps concurrently; role 26
 * faults mid-frame while holding a live window surface. */
static void app_harness(int which, int fault_mid_frame) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);
    if (fault_mid_frame) {
        struct app_win W;
        if (app_create(&W, 300, 200, 0xFFB020) == 0) {
            app_fill(&W, 0x202020);
            app_str(&W, 4, 4, "ABOUT TO FAULT", 0xFF2D9B);
            app_damage(&W);
        }
        volatile u32 *bad = (volatile u32 *)0x1;
        *bad = 0xDEAD;                                      /* crash holding a window */
    }
    if (which == 0)      app_terminal(3);
    else if (which == 1) app_sysmon(3);
    else                 app_filer(3);
    sysc(SYS_EXIT, pid, 0, 0);
}

static void wimp_driver(int fault_after_create) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);
    i64 ids[3];
    for (int i = 0; i < 3; i++) {
        i64 id = (i64)sysc(SYS_WIN_CREATE, ((u64)(180 + i * 20) << 16) | (u64)(120 + i * 10),
                           (u64)(0x22E4FF + (u64)i * 0x203040), 0);
        if (id < 0) sysc(SYS_EXIT, 1401 + i, 0, 0);
        ids[i] = id;
        /* v0.54: content is now a full-resolution surface at the WIN_SURF stride;
         * query its real dimensions and paint the whole thing. */
        i64 dims = (i64)sysc(SYS_WIN_INFO, (u64)id, 0, 0);
        if (dims < 0) sysc(SYS_EXIT, 1411 + i, 0, 0);
        int cw = (int)((u64)dims >> 16), chh = (int)((u64)dims & 0xFFFF);
        volatile u32 *surf = (volatile u32 *)(WIN_SURF_BASE + (u64)id * WIN_SURF_STRIDE);
        for (int k = 0; k < cw * chh; k++)
            surf[k] = (u32)(((pid * 7 + (u64)i * 40 + (u64)k) & 0xFFFFFF) | 0x101010);
        sysc(SYS_WIN_DAMAGE, (u64)id, 0, 0);
    }

    if (fault_after_create) {
        volatile u32 *bad = (volatile u32 *)0x1;
        *bad = 0xDEAD;                                       /* deliberate fault: windows must still be destroyed */
    }

    struct { int type, x, y, code; } ev;                    /* poll any routed events (none in headless) */
    for (int i = 0; i < 3; i++) sysc(SYS_WIN_POLL, (u64)ids[i], (u64)&ev, 0);

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


/* ============================================================================
 * v0.55: RING-3 POSIX RUNTIME  (crt0 + minimal libc + signals + pthreads)
 * ============================================================================
 * Everything below is ordinary unprivileged code. It talks to the kernel only
 * through `syscall`, exactly like the rest of this file — there is no magic
 * shared page and no kernel helper injected into the address space.
 * ==========================================================================*/

/* ---- setjmp / longjmp ------------------------------------------------------
 * Needed for real signal recovery: a SIGSEGV handler cannot simply RETURN,
 * because the kernel resumes the FAULTING instruction, which faults again
 * forever. The POSIX idiom is to longjmp out of the handler, and that needs a
 * genuine register-file save/restore, so it is written in assembly.          */
struct ojmp { u64 rbx, rbp, r12, r13, r14, r15, rsp, rip; };
extern int  osetjmp(struct ojmp *j) __attribute__((returns_twice));
extern void olongjmp(struct ojmp *j, int v) __attribute__((noreturn));
__asm__(
    ".text\n"
    ".globl osetjmp\n"
    "osetjmp:\n"
    "  mov %rbx,   0(%rdi)\n"
    "  mov %rbp,   8(%rdi)\n"
    "  mov %r12,  16(%rdi)\n"
    "  mov %r13,  24(%rdi)\n"
    "  mov %r14,  32(%rdi)\n"
    "  mov %r15,  40(%rdi)\n"
    "  lea 8(%rsp), %rax\n"          /* RSP as it will be after our RET       */
    "  mov %rax,  48(%rdi)\n"
    "  mov (%rsp), %rax\n"           /* our return address = resume point     */
    "  mov %rax,  56(%rdi)\n"
    "  xor %eax, %eax\n"
    "  ret\n"
    ".globl olongjmp\n"
    "olongjmp:\n"
    "  mov  0(%rdi), %rbx\n"
    "  mov  8(%rdi), %rbp\n"
    "  mov 16(%rdi), %r12\n"
    "  mov 24(%rdi), %r13\n"
    "  mov 32(%rdi), %r14\n"
    "  mov 40(%rdi), %r15\n"
    "  mov 48(%rdi), %rsp\n"
    "  mov %esi, %eax\n"
    "  test %eax, %eax\n"
    "  jnz 1f\n"
    "  mov $1, %eax\n"
    "1:\n"
    "  jmp *56(%rdi)\n"
);

/* ---- signal trampoline ----------------------------------------------------
 * The kernel enters the handler with RIP = handler and RSP pointing AT the
 * signal frame it just spilled, RDI = signo. A plain C function would `ret`
 * into that frame's first quadword, so ring 3 needs its own trampoline: call
 * the user's handler, then SYS_SIGRETURN, which pops the frame back into the
 * live context and never returns. `call` pushes BELOW the frame (the kernel
 * left a 128-byte gap and the stack continues downwards), so the frame the
 * kernel is going to restore is never touched.                              */
static void (*g_sighandler[NSIG])(int);
void sig_dispatch(int signo);                       /* called from the trampoline */
extern void sig_trampoline(void);
__asm__(
    ".text\n"
    ".globl sig_trampoline\n"
    "sig_trampoline:\n"
    "  call sig_dispatch\n"          /* RDI already = signo; RSP 16-aligned   */
    "  mov $51, %rax\n"              /* SYS_SIGRETURN                          */
    "  xor %edi, %edi\n"
    "  xor %esi, %esi\n"
    "  xor %edx, %edx\n"
    "  syscall\n"
    "1: jmp 1b\n"                    /* unreachable: sigreturn never returns   */
);
void sig_dispatch(int signo) {
    if (signo > 0 && signo < NSIG && g_sighandler[signo]) g_sighandler[signo](signo);
}

static int osigaction(int signo, void (*fn)(int)) {
    if (signo <= 0 || signo >= NSIG) return -1;
    g_sighandler[signo] = fn;
    return (int)(i64)sysc(SYS_SIGACTION, (u64)signo,
                          fn ? (u64)(void *)sig_trampoline : 0, 0);
}
static int okill(u32 pid, int signo)  { return (int)(i64)sysc(SYS_KILL, pid, (u64)signo, 0); }
static u64 oalarm(u64 ticks)          { return sysc(SYS_ALARM, ticks, 0, 0); }
static void osigunmask(int signo)     { sysc(SYS_SIGUNMASK, (u64)signo, 0, 0); }
static u32 ogetpid(void)              { return (u32)sysc(SYS_GETPID, 0, 0, 0); }
static u32 ogetppid(void)             { return (u32)sysc(SYS_GETPPID, 0, 0, 0); }
static void oyield(void)              { sysc(SYS_YIELD, 0, 0, 0); }

/* fork() is a plain syscall here: the kernel duplicates the caller's ENTIRE
 * register file into the child's saved context with RAX forced to 0, so the
 * child resumes at this very instruction with everything else identical. No
 * userland continuation trampoline is involved. */
static i64 ofork(void)                { return (i64)sysc(SYS_FORK, 0, 0, 0); }
/* v0.55's exec: re-run THIS SAME boot image under a different role. It predates
 * being able to load anything else and every v0.55 suite still uses it. */
static i64 oexec_role(const char **argv, const char **envp, u64 role) {
    return (i64)sysc(SYS_EXECVE, (u64)argv, (u64)envp, role);
}
/* v0.56: the real POSIX-shaped exec — replace this image with an ARBITRARY ELF
 * loaded from the VFS by path. Returns only on failure. */
static i64 oexecve(const char *path, const char **argv, const char **envp) {
    return (i64)sysc(SYS_EXECVE_PATH, (u64)path, (u64)argv, (u64)envp);
}
/* Non-blocking: -11 = still running, -10 = not our child. See the changelog —
 * this kernel has no ring-3 sleep/wake queue yet, so waiters poll + yield. */
static i64 owaitpid_poll(u32 pid)     { return (i64)sysc(SYS_WAITPID, pid, 0, 0); }
static i64 owaitpid(u32 pid, int spins) {
    for (int i = 0; i < spins; i++) {
        i64 r = owaitpid_poll(pid);
        if (r != -11) return r;
        oyield();
    }
    return -11;
}

/* ---- standard file descriptors --------------------------------------------
 * A userland fd table layered over the kernel's descriptors. fds 0/1/2 are
 * reserved for stdin/stdout/stderr and bound to the console; open() hands out
 * 3 and up, mapping each to the kernel fd underneath. The table is ordinary
 * process memory, so fork() inherits it byte for byte (the child keeps writing
 * to the same stdout and to any file the parent had open), while execve()
 * builds a fresh image whose crt0 re-initialises it to the default three —
 * which is the correct POSIX result here, since 0/1/2 are always the console. */
static u64 ostrlen(const char *s);
#define OFD_MAX      12
#define OFD_CONSOLE  (-2)
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
static int g_ofd[OFD_MAX];

static void stdio_init(void) {
    for (int i = 0; i < OFD_MAX; i++) g_ofd[i] = -1;
    g_ofd[STDIN_FILENO] = g_ofd[STDOUT_FILENO] = g_ofd[STDERR_FILENO] = OFD_CONSOLE;
}
/* v0.56: O_CREAT is bit 0 of the flags word. Creation is explicit — plain
 * oopen() still fails on a missing name, which is what every pre-existing
 * "prove this file is gone" check depends on. */
#define O_CREAT 1
static int oopen_flags(const char *path, u64 flags) {
    i64 k = (i64)sysc(SYS_OPEN, (u64)path, flags, 0);
    if (k < 0) return (int)k;
    for (int i = 3; i < OFD_MAX; i++) if (g_ofd[i] == -1) { g_ofd[i] = (int)k; return i; }
    sysc(SYS_CLOSE, (u64)k, 0, 0);
    return -24;                                          /* EMFILE */
}
static int oopen(const char *path)  { return oopen_flags(path, 0); }
static int ocreat(const char *path) { return oopen_flags(path, O_CREAT); }
static i64 owrite(int fd, const char *buf, u64 n) {
    if (fd < 0 || fd >= OFD_MAX || g_ofd[fd] == -1) return -9;          /* EBADF */
    if (g_ofd[fd] == OFD_CONSOLE) {
        char line[192];
        u64 i = 0;
        while (i < n && i < sizeof line - 1) { line[i] = buf[i]; i++; }
        line[i] = 0;
        sysc(SYS_WRITE, (u64)line, 0, 0);
        return (i64)i;
    }
    return (i64)sysc(SYS_WRITE_FILE, (u64)g_ofd[fd], (u64)buf, n);
}
static void oputs(const char *s) { owrite(STDOUT_FILENO, s, ostrlen(s)); }
static i64 oread(int fd, char *buf, u64 n) {
    if (fd < 0 || fd >= OFD_MAX || g_ofd[fd] == -1) return -9;
    if (g_ofd[fd] == OFD_CONSOLE) return 0;              /* no ring-3 tty input yet */
    return (i64)sysc(SYS_READ, (u64)g_ofd[fd], (u64)buf, n);
}
static int oclose(int fd) {
    if (fd < 3 || fd >= OFD_MAX || g_ofd[fd] == -1) return -9;  /* std three are not closable */
    sysc(SYS_CLOSE, (u64)g_ofd[fd], 0, 0);
    g_ofd[fd] = -1;
    return 0;
}

/* ---- heap: sbrk + malloc/free/realloc --------------------------------------
 * Ring 3 had no heap at all before v0.56 — every buffer in this file was a
 * static or a stack array. The compiler needs real dynamic allocation (symbol
 * tables, a token buffer, the output image), so here is the smallest allocator
 * that is actually correct: a first-fit free list over a brk-grown arena, with
 * boundary tags so free() can coalesce with its neighbours.
 *
 * Blocks are 16-byte aligned and carry an 8-byte header holding the payload
 * size plus a "in use" bit. Coalescing is forward-only against the next block,
 * which is enough to keep the compiler's alloc/free churn from fragmenting the
 * arena while staying small enough to audit by eye.                          */
static u64 obrk(u64 want) { return sysc(SYS_BRK, want, 0, 0); }

#define OH_USED 1ull
/* The header is padded to 16 bytes ON PURPOSE. Payload sizes are rounded up to
 * 16, and the arena starts page-aligned, so a 16-byte header keeps EVERY block's
 * payload 16-byte aligned by induction. An 8-byte header would have alternated
 * between 8- and 16-aligned payloads (caught live: exit 984). */
struct oblk { u64 size; u64 _pad; };  /* payload size | OH_USED; payload follows */
static u8 *g_heap_lo, *g_heap_hi;     /* arena bounds: [lo, hi) of block space   */

static void *osbrk(u64 inc) {
    u64 cur = obrk(0);
    if (!g_heap_lo) g_heap_lo = g_heap_hi = (u8 *)cur;
    if (!inc) return g_heap_hi;
    u64 got = obrk(cur + inc);
    if (got < cur + inc) return 0;    /* kernel could not map it: honest failure */
    u8 *old = g_heap_hi;
    g_heap_hi = (u8 *)got;
    return old;
}

static void *omalloc(u64 n) {
    if (!n) return 0;
    n = (n + 15) & ~15ull;                        /* 16-byte payload alignment   */
    if (!g_heap_lo) osbrk(0);
    /* first fit over the block chain */
    for (u8 *p = g_heap_lo; p + sizeof(struct oblk) <= g_heap_hi; ) {
        struct oblk *b = (struct oblk *)p;
        u64 sz = b->size & ~OH_USED;
        if (!sz) break;                            /* uninitialised tail          */
        if (!(b->size & OH_USED) && sz >= n) {
            /* split when the remainder can hold a header plus a 16-byte payload */
            if (sz >= n + sizeof(struct oblk) + 16) {
                struct oblk *nx = (struct oblk *)(p + sizeof(struct oblk) + n);
                nx->size = sz - n - sizeof(struct oblk);
                b->size = n;
            }
            b->size |= OH_USED;
            return p + sizeof(struct oblk);
        }
        p += sizeof(struct oblk) + sz;
    }
    /* nothing reusable: extend the arena. Grow in 64 KiB steps so a compiler
     * doing many small allocations does not make a syscall per allocation.   */
    u64 need = sizeof(struct oblk) + n;
    u64 step = need > 65536 ? need : 65536;
    u8 *base = (u8 *)osbrk(step);
    if (!base) return 0;
    struct oblk *b = (struct oblk *)base;
    b->size = n | OH_USED;
    if (step >= need + sizeof(struct oblk) + 16) { /* park the remainder as free */
        struct oblk *nx = (struct oblk *)(base + sizeof(struct oblk) + n);
        nx->size = step - need - sizeof(struct oblk);
    }
    return base + sizeof(struct oblk);
}

static void ofree(void *q) {
    if (!q) return;
    struct oblk *b = (struct oblk *)((u8 *)q - sizeof(struct oblk));
    b->size &= ~OH_USED;
    /* forward coalesce: absorb the next block while it is also free */
    for (;;) {
        u8 *nxp = (u8 *)b + sizeof(struct oblk) + (b->size & ~OH_USED);
        if (nxp + sizeof(struct oblk) > g_heap_hi) break;
        struct oblk *nx = (struct oblk *)nxp;
        if (nx->size & OH_USED) break;
        u64 nsz = nx->size & ~OH_USED;
        if (!nsz) break;
        b->size = (b->size & ~OH_USED) + sizeof(struct oblk) + nsz;
    }
}

static void *ocalloc(u64 cnt, u64 sz) {
    u64 n = cnt * sz;
    u8 *q = (u8 *)omalloc(n);
    if (q) for (u64 i = 0; i < n; i++) q[i] = 0;
    return q;
}

static void *orealloc(void *q, u64 n) {
    if (!q) return omalloc(n);
    struct oblk *b = (struct oblk *)((u8 *)q - sizeof(struct oblk));
    u64 old = b->size & ~OH_USED;
    if (old >= n) return q;
    u8 *nq = (u8 *)omalloc(n);
    if (!nq) return 0;
    for (u64 i = 0; i < old; i++) nq[i] = ((u8 *)q)[i];
    ofree(q);
    return nq;
}

/* ---- argc / argv / envp ---------------------------------------------------*/
static int          g_argc;
static const char **g_argv;
static const char **g_envp;

static u64 ostrlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static int ostrneq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; }
    return 1;
}
static const char *ogetenv(const char *key) {
    int kl = 0; while (key[kl]) kl++;
    for (int i = 0; g_envp && g_envp[i]; i++)
        if (ostrneq(g_envp[i], key, kl) && g_envp[i][kl] == '=') return g_envp[i] + kl + 1;
    return 0;
}

/* ===========================================================================
 * v0.62: libpthread — POSIX threads on the M61 kernel substrate
 * ===========================================================================
 * v0.55 shipped this API over BSP-only kernel threads and a mutex that spun
 * through oyield(). Both halves are replaced here and the SIGNATURES are
 * deliberately unchanged, so role 31 (posixstrs) compiles against the new
 * engine without edits — which makes an existing suite a live test of it
 * rather than a museum piece.
 *
 * What actually changed underneath:
 *   - a thread is a run-queue entity (v0.61), so it runs on any core;
 *   - the mutex has a syscall-free uncontended path and PARKS when contended,
 *     instead of burning a core yielding;
 *   - join asks the kernel instead of polling a userland flag;
 *   - there is a condition variable, which there was no way to build before.
 *
 * GUARD PAGES: the kernel places thread N's stack at THR_USER_V + N*0x8000 and
 * maps only the low 4 of those 8 pages. The 4 unmapped pages ABOVE each stack
 * are the guard: a thread that overruns its stack downward from the top runs
 * into the previous slot's guard hole and faults, rather than silently
 * corrupting a sibling's stack. That geometry is the kernel's (v0.55) and is
 * documented here because it is a property userland depends on and cannot see.
 */
#define PTHREAD_MAX 8
#define THR_USER_V     0x0000560000000000ull      /* mirrors the kernel's window */
#define THR_STK_STRIDE 0x8000ull                  /* 4 mapped + 4 guard pages    */
typedef int pthread_t;

struct pthr {
    void *(*fn)(void *);
    void *arg;
    void *ret;
    volatile int state;                            /* 0 free, 1 running, 2 done */
};
static struct pthr g_pthr[PTHREAD_MAX];

/* ---- mutex ---------------------------------------------------------------
 * Drepper's three-state mutex. 0 = free, 1 = held uncontended, 2 = held with
 * waiters. The third state is the whole point: unlock only enters the kernel
 * when it can SEE that somebody is parked, so an uncontended lock/unlock pair
 * is two atomics and no syscall at all. */
typedef struct { volatile u64 v; } pthread_mutex_t;

static int pthread_mutex_init(pthread_mutex_t *m) { m->v = 0; __sync_synchronize(); return 0; }
static int pthread_mutex_trylock(pthread_mutex_t *m) {
    return __sync_bool_compare_and_swap(&m->v, 0, 1) ? 0 : -1;
}
static int pthread_mutex_lock(pthread_mutex_t *m) {
    u64 c = __sync_val_compare_and_swap(&m->v, 0, 1);
    if (c == 0) return 0;                          /* uncontended: no syscall  */
    if (c != 2) c = __sync_lock_test_and_set(&m->v, 2);
    while (c != 0) {
        /* Sleep only while the word still reads 2. If unlock ran in between,
         * the kernel's compare fails and we get -EAGAIN back immediately —
         * that comparison is the reason a futex must be a syscall at all. */
        sysc(SYS_FUTEX_WAIT, (u64)(void *)&m->v, 2, 4000);
        c = __sync_lock_test_and_set(&m->v, 2);
    }
    return 0;
}
static int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (__sync_fetch_and_sub(&m->v, 1) != 1) {     /* was 2: someone is parked */
        m->v = 0;
        __sync_synchronize();
        sysc(SYS_FUTEX_WAKE, (u64)(void *)&m->v, 1, 0);
    }
    return 0;
}

/* ---- condition variable --------------------------------------------------
 * A sequence counter, and that is the entire state. The subtlety is the ORDER
 * in pthread_cond_wait: the sequence is sampled while the mutex is STILL HELD,
 * so any signaller that runs after we release it must have bumped the counter
 * first — and the kernel's compare-and-sleep then declines to sleep and
 * returns -EAGAIN. Sampling after the unlock instead would leave exactly the
 * window where a signal can arrive with nobody yet asleep to receive it, which
 * is the classic missed-wakeup and the reason a condvar cannot be built out of
 * a plain sleep. */
typedef struct { volatile u64 seq; } pthread_cond_t;

static int pthread_cond_init(pthread_cond_t *c) { c->seq = 0; __sync_synchronize(); return 0; }
static int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    u64 s = c->seq;                                /* sampled UNDER the mutex  */
    pthread_mutex_unlock(m);
    sysc(SYS_FUTEX_WAIT, (u64)(void *)&c->seq, s, 4000);
    pthread_mutex_lock(m);                         /* POSIX: return holding it */
    return 0;
}
static int pthread_cond_signal(pthread_cond_t *c) {
    __sync_fetch_and_add(&c->seq, 1);
    sysc(SYS_FUTEX_WAKE, (u64)(void *)&c->seq, 1, 0);
    return 0;
}
static int pthread_cond_broadcast(pthread_cond_t *c) {
    __sync_fetch_and_add(&c->seq, 1);
    sysc(SYS_FUTEX_WAKE, (u64)(void *)&c->seq, 0, 0);   /* 0 = wake everyone */
    return 0;
}

/* ---- thread lifecycle ----------------------------------------------------
 * Threads are entered through RDI — the SysV first-argument register. v0.55
 * had to use [rsp] because enter_user_thread set nothing but RIP and RSP; the
 * kernel seeds a full register context now, so the ordinary calling
 * convention is simply available. */
void pthread_body(u64 i);                          /* called from the trampoline */
void pthread_body(u64 i) {
    if (i >= PTHREAD_MAX) return;
    g_pthr[i].ret = g_pthr[i].fn(g_pthr[i].arg);
    __sync_synchronize();
    g_pthr[i].state = 2;
}
extern void pthread_tramp(void);
__asm__(
    ".text\n"
    ".globl pthread_tramp\n"
    "pthread_tramp:\n"
    "  and $-16, %rsp\n"             /* SysV: 16-byte aligned before the call  */
    "  call pthread_body\n"          /* RDI already holds our slot index       */
    "  xor %edi, %edi\n"
    "  mov $53, %rax\n"              /* SYS_THREAD_EXIT(0) if the body returns */
    "  xor %esi, %esi\n"
    "  xor %edx, %edx\n"
    "  syscall\n"
    "1: jmp 1b\n"
);

static volatile int g_pthr_n = 0;
/* Slots are handed out monotonically and never recycled inside a process, so a
 * userland index always equals the kernel's tid. The two allocators are
 * independent, so their agreement is CHECKED rather than assumed: a mismatch
 * would silently join the wrong thread. */
static int pthread_create(pthread_t *out, void *(*fn)(void *), void *arg) {
    int i = __sync_fetch_and_add(&g_pthr_n, 1);
    if (i >= PTHREAD_MAX) { __sync_fetch_and_sub(&g_pthr_n, 1); return -11; }  /* EAGAIN */
    g_pthr[i].fn = fn; g_pthr[i].arg = arg; g_pthr[i].ret = 0; g_pthr[i].state = 1;
    __sync_synchronize();
    /* stack 0 = "kernel, give me one", which is also how the guard pages get
     * arranged; a caller-supplied stack is the third argument and its guard is
     * then the caller's business. */
    i64 k = (i64)sysc(SYS_THREAD_CREATE, (u64)(void *)pthread_tramp, (u64)i, 0);
    if (k < 0)   { g_pthr[i].state = 0; return (int)k; }
    if (k != i)  { g_pthr[i].state = 0; return -1; }   /* allocators desynced: refuse */
    if (out) *out = (pthread_t)i;
    return 0;
}

/* SYS_THREAD_JOIN answers -EAGAIN for "you slept, the state changed, ask
 * again": a woken task resumes with only RAX to carry a result, and the waker
 * is in another address space and cannot fill in our pointer. The loop is
 * bounded so a join can FAIL rather than hang. */
static int pthread_join(pthread_t t, void **ret) {
    if (t < 0 || t >= PTHREAD_MAX) return -1;
    u64 code = 0;
    for (int k = 0; k < 20000; k++) {
        i64 r = (i64)sysc(SYS_THREAD_JOIN, (u64)t, (u64)(void *)&code, 0);
        if (r == EAGAIN_NEG) continue;
        if (r != 0) return (int)r;
        if (ret) *ret = g_pthr[t].ret;             /* the value, not the code */
        g_pthr[t].state = 3;                       /* joined; slot NOT recycled */
        return 0;
    }
    return ETIMEDOUT_NEG;
}

static pthread_t pthread_self(void) { return (pthread_t)sysc(SYS_GETTID, 0, 0, 0); }

static void pthread_exit(void *ret) {
    int i = pthread_self();
    if (i >= 0 && i < PTHREAD_MAX) {
        g_pthr[i].ret = ret;
        __sync_synchronize();
        g_pthr[i].state = 2;
    }
    sysc(SYS_THREAD_EXIT, 0, 0, 0);
    for (;;) { }
}

/* ---- the low-level thread interface --------------------------------------
 * pthread_create always asks the kernel for a stack, because that is what a
 * portable program wants. This layer sits under it and exposes the third
 * argument of SYS_THREAD_CREATE — a CALLER-SUPPLIED stack — which role 43
 * exercises specifically to prove the kernel accepts one and does not reclaim
 * memory it never mapped. Same trampoline shape, different argument passing:
 * the body here takes and returns a u64, so the thread's exit CODE is its
 * return value rather than a pointer parked in a table. */
#define KTHR_MAX 8
struct kthr { u64 (*fn)(u64); u64 arg; };
static struct kthr g_kthr[KTHR_MAX];
static volatile int g_kthr_n = 0;

u64 kthr_body(u64 i);
u64 kthr_body(u64 i) {
    if (i >= KTHR_MAX) return 0;
    return g_kthr[i].fn(g_kthr[i].arg);
}
extern void kthr_tramp(void);
__asm__(
    ".text\n"
    ".globl kthr_tramp\n"
    "kthr_tramp:\n"
    "  and $-16, %rsp\n"
    "  call kthr_body\n"
    "  mov %rax, %rdi\n"             /* the body's return IS the exit code     */
    "  mov $53, %rax\n"              /* SYS_THREAD_EXIT                        */
    "  xor %esi, %esi\n"
    "  xor %edx, %edx\n"
    "  syscall\n"
    "1: jmp 1b\n"
);

static int kthread_create(u64 (*fn)(u64), u64 arg, u64 stack_top) {
    int i = __sync_fetch_and_add(&g_kthr_n, 1);
    if (i >= KTHR_MAX) { __sync_fetch_and_sub(&g_kthr_n, 1); return EAGAIN_NEG; }
    g_kthr[i].fn = fn; g_kthr[i].arg = arg;
    __sync_synchronize();
    return (int)(i64)sysc(SYS_THREAD_CREATE, (u64)(void *)kthr_tramp, (u64)i, stack_top);
}
static int kthread_join(int tid, u64 *code) {
    for (int k = 0; k < 20000; k++) {
        i64 r = (i64)sysc(SYS_THREAD_JOIN, (u64)tid, (u64)(void *)code, 0);
        if (r == EAGAIN_NEG) continue;
        return (int)r;
    }
    return ETIMEDOUT_NEG;
}

/* The bare-word futex mutex role 43 was written against. It is the SAME mutex
 * pthread_mutex_t is — pthread_mutex_t is exactly one volatile u64 — so this
 * is a cast and not a second implementation to keep in step. */
static void fmutex_lock(volatile u64 *m)   { pthread_mutex_lock((pthread_mutex_t *)m); }
static void fmutex_unlock(volatile u64 *m) { pthread_mutex_unlock((pthread_mutex_t *)m); }

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

/* ============================================================================
 * v0.55: POSIX VERIFICATION ROLES (29-34) — the ring-3 half of `posixstrs`
 * ==========================================================================*/

/* --- role 29: fork / waitpid / SIGCHLD --------------------------------------*/
static volatile int g_chld_hits = 0;
static void on_sigchld(int s) { (void)s; g_chld_hits++; }

static void posix_fork_worker(void) {
    u32 mypid = ogetpid();
    osigaction(SIGCHLD, on_sigchld);
    i64 r = ofork();
    if (r < 0) sysc(SYS_EXIT, 701, 0, 0);
    if (r == 0) {                                     /* ---- CHILD ---- */
        /* Prove this really is a child of a real fork: a new pid, our parent's
         * pid visible through getppid, and the inherited stdout still working. */
        if (ogetpid() == mypid)   sysc(SYS_EXIT, 43, 0, 0);
        if (ogetppid() != mypid)  sysc(SYS_EXIT, 44, 0, 0);
        oputs("  [posix ] forked child alive at ring 3 (inherited stdout)\n");
        sysc(SYS_EXIT, 42, 0, 0);
    }
    /* ---- PARENT ---- */
    u32 child = (u32)r;
    i64 code = owaitpid(child, 30000);
    if (code == -11) sysc(SYS_EXIT, 702, 0, 0);       /* child never finished */
    if (code != 42)  sysc(SYS_EXIT, 703, 0, 0);       /* wrong exit status    */
    /* SIGCHLD is posted by the kernel when the child's space is reclaimed; give
     * the delivery boundary a few syscalls to hand it to our handler.        */
    for (int i = 0; i < 64 && !g_chld_hits; i++) oyield();
    if (!g_chld_hits) sysc(SYS_EXIT, 704, 0, 0);
    sysc(SYS_EXIT, 700, 0, 0);
}

/* --- role 30: signals ------------------------------------------------------*/
static volatile int g_segv_hits = 0, g_int_hits = 0, g_alrm_hits = 0;
static struct ojmp  g_segv_jb;

/* A catchable SIGSEGV cannot simply return: the kernel resumes the faulting
 * instruction, which would fault forever. Unblock the signal and longjmp out —
 * the textbook POSIX recovery, and the reason SYS_SIGUNMASK exists.          */
static void on_sigsegv(int s) {
    (void)s;
    g_segv_hits++;
    osigunmask(SIGSEGV);
    olongjmp(&g_segv_jb, 1);
}
static void on_sigint(int s)  { (void)s; g_int_hits++; }
static void on_sigalrm(int s) { (void)s; g_alrm_hits++; }

/* SYS_KILL on ourselves, with sentinels in every callee-saved register. The
 * signal is delivered on the way OUT of that syscall, so by the time the next
 * instruction runs the handler has been entered, has returned, and
 * SYS_SIGRETURN has restored the frame. If ANY register — or the syscall's own
 * return value in RAX — comes back wrong, the frame round-trip is broken.   */
static int sigint_frame_ok(u32 pid) {
    u64 ok;
    __asm__ volatile(
        "push %%rbx\n push %%r12\n push %%r13\n push %%r14\n push %%r15\n"
        "movabs $0x1111111111111111, %%rbx\n"
        "movabs $0x2222222222222222, %%r12\n"
        "movabs $0x3333333333333333, %%r13\n"
        "movabs $0x4444444444444444, %%r14\n"
        "movabs $0x5555555555555555, %%r15\n"
        "mov $50, %%rax\n"                          /* SYS_KILL(pid, SIGINT)  */
        "syscall\n"
        "test %%rax, %%rax\n jnz 1f\n"              /* kill() must still read 0 */
        "movabs $0x1111111111111111, %%rcx\n cmp %%rcx, %%rbx\n jne 1f\n"
        "movabs $0x2222222222222222, %%rcx\n cmp %%rcx, %%r12\n jne 1f\n"
        "movabs $0x3333333333333333, %%rcx\n cmp %%rcx, %%r13\n jne 1f\n"
        "movabs $0x4444444444444444, %%rcx\n cmp %%rcx, %%r14\n jne 1f\n"
        "movabs $0x5555555555555555, %%rcx\n cmp %%rcx, %%r15\n jne 1f\n"
        "mov $1, %%rax\n jmp 2f\n"
        "1: xor %%rax, %%rax\n"
        "2:\n"
        "pop %%r15\n pop %%r14\n pop %%r13\n pop %%r12\n pop %%rbx\n"
        : "=a"(ok) : "D"((u64)pid), "S"((u64)SIGINT), "d"(0ull)
        : "rcx", "r11", "memory");
    return (int)ok;
}

static void posix_signal_worker(void) {
    osigaction(SIGSEGV, on_sigsegv);
    osigaction(SIGINT,  on_sigint);
    osigaction(SIGALRM, on_sigalrm);

    /* Two wild writes in a row. The second one matters as much as the first:
     * it only reaches the handler if SYS_SIGUNMASK really cleared the block
     * sig_deliver installed, so this catches a one-shot-only implementation. */
    for (int round = 0; round < 2; round++) {
        if (osetjmp(&g_segv_jb) == 0) {
            volatile u32 *wild = (volatile u32 *)0x0000500000004000ull;  /* unmapped, in range */
            *wild = 0xDEADBEEF;                       /* -> SIGSEGV -> handler -> longjmp */
            sysc(SYS_EXIT, 801, 0, 0);                /* fell through: never delivered   */
        }
        if (g_segv_hits != round + 1) sysc(SYS_EXIT, 802, 0, 0);
    }

    if (!sigint_frame_ok(ogetpid())) sysc(SYS_EXIT, 803, 0, 0);
    if (g_int_hits < 1)              sysc(SYS_EXIT, 804, 0, 0);

    oalarm(2);                                        /* fires ~2 timer ticks out */
    for (int i = 0; i < 20000 && !g_alrm_hits; i++) oyield();
    if (!g_alrm_hits) sysc(SYS_EXIT, 805, 0, 0);

    sysc(SYS_EXIT, 800, 0, 0);
}

/* --- role 31: pthreads + mutex --------------------------------------------*/
#define PW_THREADS 4
#define PW_BUMPS   200
static pthread_mutex_t g_pw_mutex;
static volatile u64    g_pw_counter = 0;   /* guarded by g_pw_mutex           */
static volatile u64    g_pw_racy    = 0;   /* deliberately unguarded, for contrast */
static volatile int    g_pw_ran[PW_THREADS];

static void *pw_body(void *arg) {
    int id = (int)(u64)arg;
    if (id >= 0 && id < PW_THREADS) g_pw_ran[id] = 1;
    for (int i = 0; i < PW_BUMPS; i++) {
        pthread_mutex_lock(&g_pw_mutex);
        u64 v = g_pw_counter;               /* read-modify-write across a yield  */
        g_pw_counter = v + 1;               /* point: the CS must be atomic      */
        pthread_mutex_unlock(&g_pw_mutex);
        g_pw_racy++;
        if ((i & 31) == 0) oyield();        /* invite interleaving               */
    }
    return (void *)(u64)(id + 1);
}

static void posix_thread_worker(void) {
    pthread_mutex_init(&g_pw_mutex);
    /* A fresh mutex must be acquirable exactly once until released. */
    if (pthread_mutex_trylock(&g_pw_mutex) != 0) sysc(SYS_EXIT, 905, 0, 0);
    if (pthread_mutex_trylock(&g_pw_mutex) == 0) sysc(SYS_EXIT, 906, 0, 0);
    pthread_mutex_unlock(&g_pw_mutex);

    pthread_t t[PW_THREADS];
    for (int i = 0; i < PW_THREADS; i++)
        if (pthread_create(&t[i], pw_body, (void *)(u64)i) != 0) sysc(SYS_EXIT, 901, 0, 0);
    for (int i = 0; i < PW_THREADS; i++) {
        void *ret = 0;
        if (pthread_join(t[i], &ret) != 0)          sysc(SYS_EXIT, 902, 0, 0);
        if ((u64)ret != (u64)(i + 1))               sysc(SYS_EXIT, 907, 0, 0);
    }
    for (int i = 0; i < PW_THREADS; i++) if (!g_pw_ran[i]) sysc(SYS_EXIT, 904, 0, 0);
    if (g_pw_counter != (u64)PW_THREADS * PW_BUMPS)  sysc(SYS_EXIT, 903, 0, 0);
    sysc(SYS_EXIT, 900, 0, 0);
}

/* --- role 43: v0.61 threads — cross-core, futex-blocking, kernel join ------
 *
 * What separates this from role 31 is not "more threads": it is that these
 * threads can SLEEP. Role 31's mutex spins through oyield(), so a contended
 * lock keeps a core busy achieving nothing; here a waiter is parked in no run
 * queue at all until the unlock that concerns it.
 *
 * Every assertion below is written so that the failure mode it guards against
 * produces a WRONG ANSWER rather than a slow one — a counter that is short, a
 * gate that was never observed, a wait that never expired. */
#define TW_THREADS 4
#define TW_BUMPS   150

static volatile u64 g_tw_mutex   = 0;    /* the futex word: 0 free, 1 held, 2 contended */
static volatile u64 g_tw_counter = 0;    /* guarded by g_tw_mutex                  */
static volatile u64 g_tw_ran     = 0;    /* bit i = worker i executed              */
static volatile u64 g_tw_gate    = 0;    /* futex word a thread parks on           */
static volatile u64 g_tw_passed  = 0;    /* the gate thread's observation          */
static volatile u64 g_tw_pid_bad = 0;    /* a thread saw a getpid() != the group's */
static volatile u64 g_tw_pid     = 0;    /* the group's pid, sampled by main       */

static u64 tw_body(u64 id) {
    __sync_fetch_and_or(&g_tw_ran, 1ull << id);
    /* POSIX: one pid per thread group, a distinct tid per thread. Both halves
     * are checked, because reporting the slot's own pid from getpid() would
     * make the answer depend on which thread asked. */
    if (sysc(SYS_GETPID, 0, 0, 0) != g_tw_pid) g_tw_pid_bad = 1;
    if (sysc(SYS_GETTID, 0, 0, 0) != id)       g_tw_pid_bad = 1;
    for (int i = 0; i < TW_BUMPS; i++) {
        fmutex_lock(&g_tw_mutex);
        u64 v = g_tw_counter;
        /* Read-modify-write ACROSS a reschedule. The point of the suite: if
         * the critical section is not really exclusive, the final count comes
         * out short and no amount of re-running hides it. */
        if ((i & 15) == 0) oyield();
        g_tw_counter = v + 1;
        fmutex_unlock(&g_tw_mutex);
    }
    return 200 + id;
}

/* Parks until main opens the gate. This is the one that proves a wake actually
 * reaches a SLEEPING thread — the mutex test alone could pass on a system
 * where FUTEX_WAIT silently returned immediately every time. */
static u64 tw_gate_body(u64 arg) {
    (void)arg;
    while (g_tw_gate == 0) sysc(SYS_FUTEX_WAIT, (u64)(void *)&g_tw_gate, 0, 6000);
    g_tw_passed = 1;
    __sync_synchronize();
    return 300;
}

/* Runs on a stack this process allocated itself, not one the kernel handed
 * out — the third argument to SYS_THREAD_CREATE. */
static u64 tw_stack_body(u64 arg) {
    u64 probe[16];
    for (int i = 0; i < 16; i++) probe[i] = arg + i;   /* touch the caller's stack */
    u64 s = 0;
    for (int i = 0; i < 16; i++) s += probe[i];
    return (s == arg * 16 + 120) ? arg + 1 : 0;
}

static void thread_stress_worker(void) {
    g_tw_pid = sysc(SYS_GETPID, 0, 0, 0);
    if (sysc(SYS_GETTID, 0, 0, 0) != 0) sysc(SYS_EXIT, 961, 0, 0);

    int t[TW_THREADS];
    for (int i = 0; i < TW_THREADS; i++) {
        t[i] = kthread_create(tw_body, (u64)i, 0);
        /* The kernel's tid allocator and the userland index allocator are
         * independent, so their agreement is CHECKED rather than assumed —
         * a mismatch would silently join the wrong thread. */
        if (t[i] < 0 || t[i] != i) sysc(SYS_EXIT, 962, 0, 0);
    }
    for (int i = 0; i < TW_THREADS; i++) {
        u64 code = 0;
        if (kthread_join(t[i], &code) != 0) sysc(SYS_EXIT, 963, 0, 0);
        if (code != (u64)(200 + i))         sysc(SYS_EXIT, 964, 0, 0);
    }
    if (g_tw_ran != ((1ull << TW_THREADS) - 1))          sysc(SYS_EXIT, 966, 0, 0);
    if (g_tw_counter != (u64)TW_THREADS * TW_BUMPS)      sysc(SYS_EXIT, 965, 0, 0);
    if (g_tw_pid_bad)                                    sysc(SYS_EXIT, 973, 0, 0);

    /* A stack of our own. 16 KiB from the heap; the kernel must accept it,
     * must NOT free it at thread exit, and the thread must actually run on it. */
    u64 stk = (u64)omalloc(16384);
    if (!stk) sysc(SYS_EXIT, 967, 0, 0);
    u64 top = (stk + 16384) & ~15ull;
    int ct = kthread_create(tw_stack_body, 0xC0DE, top);
    if (ct < 0) sysc(SYS_EXIT, 967, 0, 0);
    u64 sc = 0;
    if (kthread_join(ct, &sc) != 0) sysc(SYS_EXIT, 968, 0, 0);
    if (sc != 0xC0DE + 1)           sysc(SYS_EXIT, 968, 0, 0);

    /* A wake that must reach a thread which is genuinely asleep. */
    int gt = kthread_create(tw_gate_body, 0, 0);
    if (gt < 0) sysc(SYS_EXIT, 969, 0, 0);
    for (int i = 0; i < 300; i++) oyield();       /* let it get all the way parked */
    if (g_tw_passed) sysc(SYS_EXIT, 970, 0, 0);   /* passed the gate before it opened */
    g_tw_gate = 1;
    __sync_synchronize();
    sysc(SYS_FUTEX_WAKE, (u64)(void *)&g_tw_gate, 0, 0);   /* 0 = wake everyone */
    u64 gc = 0;
    if (kthread_join(gt, &gc) != 0)  sysc(SYS_EXIT, 970, 0, 0);
    if (gc != 300 || !g_tw_passed)   sysc(SYS_EXIT, 970, 0, 0);

    /* The two answers a futex must give WITHOUT sleeping forever. A kernel
     * that got either of these wrong would hang the machine instead of
     * failing a test, which is exactly why the timeout is not optional. */
    {
        static volatile u64 lonely = 7;
        i64 r = (i64)sysc(SYS_FUTEX_WAIT, (u64)(void *)&lonely, 7, 400);
        if (r != ETIMEDOUT_NEG) sysc(SYS_EXIT, 971, 0, 0);   /* nobody wakes it */
        r = (i64)sysc(SYS_FUTEX_WAIT, (u64)(void *)&lonely, 8, 400);
        if (r != EAGAIN_NEG)    sysc(SYS_EXIT, 972, 0, 0);   /* value mismatch  */
    }
    sysc(SYS_EXIT, 960, 0, 0);
}

/* --- role 44: pthreads_smp — mutex contention and a condition variable -----
 *
 * Role 43 (v0.61) proved threads RUN on every core. This one proves they can
 * COORDINATE: a mutex whose critical section survives a reschedule, and a
 * condition variable, which is the primitive you cannot build without the
 * kernel closing the gap between "I released the lock" and "I am asleep".
 *
 * The condvar round is the interesting one. Workers block on a predicate that
 * is false when they start, so every one of them MUST reach the wait; main
 * then sets it and broadcasts. If cond_wait ever returned without the
 * predicate holding, or a broadcast failed to reach a sleeper, the tally comes
 * out wrong — it cannot come out right by luck. */
#define PS_THREADS 4
#define PS_BUMPS   200

static pthread_mutex_t g_ps_mx;
static pthread_cond_t  g_ps_cv;
static volatile u64 g_ps_counter = 0;
static volatile u64 g_ps_ready   = 0;   /* the predicate the condvar guards  */
static volatile u64 g_ps_waiting = 0;   /* workers that reached the wait     */
static volatile u64 g_ps_passed  = 0;   /* workers that got through it       */
static volatile u64 g_ps_ran     = 0;   /* bit i = worker i executed         */
static volatile u64 g_ps_tidbad  = 0;

static void *ps_body(void *arg) {
    int id = (int)(u64)arg;
    __sync_fetch_and_or(&g_ps_ran, 1ull << id);
    if (pthread_self() != id) g_ps_tidbad = 1;   /* tid must equal our index */

    /* Phase 1: contend hard on the mutex, holding it across a reschedule. */
    for (int i = 0; i < PS_BUMPS; i++) {
        pthread_mutex_lock(&g_ps_mx);
        u64 v = g_ps_counter;
        if ((i & 15) == 0) oyield();
        g_ps_counter = v + 1;
        pthread_mutex_unlock(&g_ps_mx);
    }

    /* Phase 2: block on the condition variable until main says go. The loop
     * around the wait is not decoration — a condvar may wake spuriously, and
     * re-testing the predicate is the only correct way to use one. */
    pthread_mutex_lock(&g_ps_mx);
    __sync_fetch_and_add(&g_ps_waiting, 1);
    /* The predicate is a COUNT, not a flag, and that is what makes the
     * signal-versus-broadcast distinction testable: one permit must release
     * exactly one waiter, however many are asleep. */
    while (g_ps_ready == 0) pthread_cond_wait(&g_ps_cv, &g_ps_mx);
    g_ps_ready = g_ps_ready - 1;                 /* consume one permit         */
    __sync_fetch_and_add(&g_ps_passed, 1);
    pthread_mutex_unlock(&g_ps_mx);              /* cond_wait returns holding it */
    return (void *)(u64)(500 + id);
}

static void pthreads_smp_worker(void) {
    pthread_mutex_init(&g_ps_mx);
    pthread_cond_init(&g_ps_cv);
    if (pthread_mutex_trylock(&g_ps_mx) != 0) sysc(SYS_EXIT, 941, 0, 0);
    if (pthread_mutex_trylock(&g_ps_mx) == 0) sysc(SYS_EXIT, 941, 0, 0);
    pthread_mutex_unlock(&g_ps_mx);

    pthread_t t[PS_THREADS];
    for (int i = 0; i < PS_THREADS; i++)
        if (pthread_create(&t[i], ps_body, (void *)(u64)i) != 0) sysc(SYS_EXIT, 942, 0, 0);

    /* Wait until every worker is actually parked on the condvar. If we
     * broadcast before they arrive the test proves nothing — they would find
     * the predicate already true and never exercise the wait at all. */
    for (int i = 0; i < 200000 && g_ps_waiting < PS_THREADS; i++) oyield();
    if (g_ps_waiting != PS_THREADS) sysc(SYS_EXIT, 943, 0, 0);
    if (g_ps_passed  != 0)          sysc(SYS_EXIT, 944, 0, 0);   /* woke early */

    /* ONE permit, released with cond_signal: exactly one worker may get through,
     * and the other three must still be asleep afterwards. A signal that
     * behaved like a broadcast would show up here as too many passing. */
    pthread_mutex_lock(&g_ps_mx);
    g_ps_ready = 1;
    pthread_mutex_unlock(&g_ps_mx);
    pthread_cond_signal(&g_ps_cv);
    for (int i = 0; i < 200000 && g_ps_passed < 1; i++) oyield();
    if (g_ps_passed != 1) sysc(SYS_EXIT, 938, 0, 0);   /* woke the wrong number */

    /* The remaining three, released together. */
    pthread_mutex_lock(&g_ps_mx);
    g_ps_ready = PS_THREADS - 1;
    pthread_mutex_unlock(&g_ps_mx);
    pthread_cond_broadcast(&g_ps_cv);

    for (int i = 0; i < PS_THREADS; i++) {
        void *r = 0;
        if (pthread_join(t[i], &r) != 0)      sysc(SYS_EXIT, 945, 0, 0);
        if ((u64)r != (u64)(500 + i))         sysc(SYS_EXIT, 946, 0, 0);
    }
    if (g_ps_ran != ((1ull << PS_THREADS) - 1))          sysc(SYS_EXIT, 947, 0, 0);
    if (g_ps_counter != (u64)PS_THREADS * PS_BUMPS)      sysc(SYS_EXIT, 948, 0, 0);
    if (g_ps_passed  != PS_THREADS)                      sysc(SYS_EXIT, 949, 0, 0);
    if (g_ps_tidbad)                                     sysc(SYS_EXIT, 939, 0, 0);
    sysc(SYS_EXIT, 940, 0, 0);
}

/* --- role 45: sigstrs — masks, dispositions, and process groups ------------
 *
 * Role 30 (v0.55) covers handler execution, frames and SIGSEGV/SIGALRM. This
 * suite covers what v0.62 adds and what role 30 cannot reach: sigprocmask's
 * three modes, SIG_IGN, and process-group delivery — the mechanism that makes
 * an interrupt reach a job and not the shell waiting on it. */
static volatile int g_sg_hits = 0;
static volatile int g_sg_last = 0;
static void sg_handler(int s) { g_sg_last = s; __sync_fetch_and_add(&g_sg_hits, 1); }

static void sig_stress_worker(void) {
    /* (1) A handler runs, and SYS_SIGRETURN puts the interrupted context back.
     * `witness` is a local: if the frame were restored wrongly it is the first
     * thing that would come back as garbage. */
    volatile int witness = 0x5EED;
    osigaction(SIGTERM, sg_handler);
    okill(ogetpid(), SIGTERM);
    for (int i = 0; i < 40000 && g_sg_hits < 1; i++) oyield();
    if (g_sg_hits != 1)          sysc(SYS_EXIT, 951, 0, 0);
    if (g_sg_last != SIGTERM)    sysc(SYS_EXIT, 952, 0, 0);
    if (witness != 0x5EED)       sysc(SYS_EXIT, 953, 0, 0);

    /* (2) A blocked signal is HELD, not lost: unblocking must deliver it. */
    sysc(SYS_SIGPROCMASK, 0, 1ull << SIGTERM, 0);          /* block */
    int before = g_sg_hits;
    okill(ogetpid(), SIGTERM);
    for (int i = 0; i < 3000; i++) oyield();
    if (g_sg_hits != before)     sysc(SYS_EXIT, 954, 0, 0);   /* leaked past the mask */
    sysc(SYS_SIGPROCMASK, 1, 1ull << SIGTERM, 0);          /* unblock */
    for (int i = 0; i < 40000 && g_sg_hits == before; i++) oyield();
    if (g_sg_hits != before + 1) sysc(SYS_EXIT, 955, 0, 0);   /* dropped, not held */

    /* (3) SETMASK replaces rather than merges, and returns the old mask. */
    u64 prev = sysc(SYS_SIGPROCMASK, 2, 1ull << SIGTERM, 0);
    u64 now  = sysc(SYS_SIGPROCMASK, 2, 0, 0);             /* clear, read back */
    if (now != (1ull << SIGTERM)) sysc(SYS_EXIT, 956, 0, 0);
    (void)prev;

    /* (4) SIG_IGN discards outright — no handler call, no pending residue. */
    osigaction(SIGTERM, 0);                                 /* SIG_DFL first   */
    sysc(SYS_SIGACTION, SIGTERM, 1, 0);                     /* now SIG_IGN     */
    before = g_sg_hits;
    okill(ogetpid(), SIGTERM);
    for (int i = 0; i < 3000; i++) oyield();
    if (g_sg_hits != before)     sysc(SYS_EXIT, 957, 0, 0);
    osigaction(SIGTERM, sg_handler);                        /* restore         */

    /* (5) PROCESS GROUPS. Two children in one group, signalled as a unit with
     * one call. They carry the DEFAULT disposition for SIGINT — terminate — so
     * their exit codes prove delivery reached them and not merely the group. */
    u32 kid[2];
    for (int i = 0; i < 2; i++) {
        i64 f = ofork();
        if (f < 0) sysc(SYS_EXIT, 958, 0, 0);
        if (f == 0) {
            sysc(SYS_SIGACTION, SIGINT, 0, 0);              /* SIG_DFL: terminate */
            sysc(SYS_SIGPROCMASK, 2, 0, 0);                 /* nothing blocked    */
            for (int k = 0; k < 400000; k++) oyield();      /* wait to be killed  */
            sysc(SYS_EXIT, 199, 0, 0);                      /* never signalled    */
        }
        kid[i] = (u32)f;
    }
    /* Both children into ONE group named after the first. */
    int job = (int)kid[0];
    for (int i = 0; i < 2; i++)
        if (sysc(SYS_SETPGID, kid[i], (u64)job, 0) != 0) sysc(SYS_EXIT, 959, 0, 0);
    i64 hit = (i64)sysc(SYS_KILLPG, (u64)job, SIGINT, 0);
    if (hit != 2) sysc(SYS_EXIT, 960, 0, 0);                /* one call, both got it */
    for (int i = 0; i < 2; i++) {
        i64 st = owaitpid(kid[i], 400000);
        if (st != 128 + SIGINT) sysc(SYS_EXIT, 961, 0, 0);  /* default action ran */
    }
    sysc(SYS_EXIT, 950, 0, 0);
}

/* --- roles 32/33: execve with argv + envp ---------------------------------*/
static void posix_exec_parent(void) {
    static const char *argv[] = { "posix-exec", "MARKER-55", 0 };
    static const char *envp[] = { "OUTRUN_EXEC=yes", "STAGE=exec", 0 };
    oexec_role(argv, envp, 33);                      /* becomes role 33; never returns */
    sysc(SYS_EXIT, 921, 0, 0);                       /* execve failed */
}
static void posix_exec_child(void) {
    /* This is the SAME kproc/pid as role 32, running a freshly loaded image.
     * Its argc/argv/envp came through the kernel's SysV start block. */
    if (g_argc != 2)                                  sysc(SYS_EXIT, 951, 0, 0);
    if (!ostrneq(g_argv[1], "MARKER-55", 10))         sysc(SYS_EXIT, 952, 0, 0);
    const char *v = ogetenv("OUTRUN_EXEC");
    if (!v || !ostrneq(v, "yes", 4))                  sysc(SYS_EXIT, 953, 0, 0);
    /* POSIX: execve REPLACES the environment wholesale. The kernel's default
     * block (PATH/OUTRUN/HOME, which role 34 verifies it does receive) must
     * therefore be gone here — anything else would be a merge, not an exec. */
    if (ogetenv("OUTRUN"))                            sysc(SYS_EXIT, 954, 0, 0);
    if (ogetenv("HOME"))                              sysc(SYS_EXIT, 956, 0, 0);
    /* exec must have RESET caught dispositions to default (POSIX). */
    if (g_sighandler[SIGSEGV])                        sysc(SYS_EXIT, 955, 0, 0);
    sysc(SYS_EXIT, 950, 0, 0);
}

/* --- role 34: std fd table across fork -----------------------------------*/
static void posix_fd_worker(void) {
    /* The kernel's default SysV start block must have reached us: argv[0] is
     * this image's name and the default environment is present. (Role 33 proves
     * the complementary case — that execve replaces it.) */
    if (g_argc < 1 || !g_argv || !g_argv[0] || !g_argv[0][0]) sysc(SYS_EXIT, 970, 0, 0);
    if (!ogetenv("OUTRUN") || !ogetenv("PATH"))               sysc(SYS_EXIT, 971, 0, 0);
    /* stdout must be usable the instant crt0 hands control over, and open()
     * must never hand back 0/1/2 — the whole point of reserving them. */
    if (owrite(STDOUT_FILENO, "  [posix ] stdout via fd 1\n",
               ostrlen("  [posix ] stdout via fd 1\n")) <= 0) sysc(SYS_EXIT, 962, 0, 0);
    if (owrite(STDERR_FILENO, "  [posix ] stderr via fd 2\n",
               ostrlen("  [posix ] stderr via fd 2\n")) <= 0) sysc(SYS_EXIT, 962, 0, 0);
    if (oread(STDIN_FILENO, (char *)0, 0) != 0) sysc(SYS_EXIT, 968, 0, 0);  /* stdin: clean EOF */

    int fd = oopen("motd");
    if (fd >= 0 && fd < 3) sysc(SYS_EXIT, 961, 0, 0);   /* collided with the std three */
    char buf[64]; i64 pn = 0;
    if (fd >= 3) {
        pn = oread(fd, buf, sizeof buf - 1);
        if (pn <= 0) sysc(SYS_EXIT, 964, 0, 0);
    }

    i64 r = ofork();
    if (r == 0) {                                       /* ---- CHILD ---- */
        /* The userland table is ordinary process memory, so fork inherits it
         * byte for byte: the child sees the same fd numbers bound the same way,
         * and the console-backed std three work immediately.
         *
         * v0.59 INVERTED THE ASSERTION BELOW, deliberately. Through v0.58 the
         * kernel descriptor under an inherited fd was NOT duplicated — each
         * kernel fd had exactly one owning kproc — so this test required a read
         * through an inherited FILE fd to be DENIED, and called that
         * containment. With ofile.owner_mask the child is a genuine co-owner,
         * so the read must now SUCCEED and return the same bytes the parent
         * saw. That is POSIX behaviour and the thing pipelines are built on;
         * the leak guarantee it used to protect is now kept by refcounting
         * (the entry survives until BOTH owners drop it) rather than by
         * forbidding the share. */
        if (g_ofd[STDOUT_FILENO] != OFD_CONSOLE) sysc(SYS_EXIT, 963, 0, 0);
        if (g_ofd[STDERR_FILENO] != OFD_CONSOLE) sysc(SYS_EXIT, 963, 0, 0);
        oputs("  [posix ] child wrote through the inherited stdout mapping\n");
        if (fd >= 3) {
            char b2[64];
            if (g_ofd[fd] < 0) sysc(SYS_EXIT, 963, 0, 0);        /* table not inherited */
            i64 cn = oread(fd, b2, sizeof b2 - 1);
            if (cn != pn) sysc(SYS_EXIT, 969, 0, 0);   /* inherited fd must READ, and read the same */
            for (i64 k = 0; k < cn; k++)
                if (b2[k] != buf[k]) sysc(SYS_EXIT, 969, 0, 0);
        }
        sysc(SYS_EXIT, 42, 0, 0);
    }
    if (r < 0) sysc(SYS_EXIT, 965, 0, 0);
    if (owaitpid((u32)r, 30000) != 42) sysc(SYS_EXIT, 963, 0, 0);

    if (fd >= 3 && oclose(fd) != 0)   sysc(SYS_EXIT, 966, 0, 0);
    if (oclose(STDOUT_FILENO) == 0)   sysc(SYS_EXIT, 967, 0, 0);  /* stdout is not closable */
    sysc(SYS_EXIT, 960, 0, 0);
}

/* --- role 35: the ring-3 heap ---------------------------------------------
 * Exit codes: 980 = every check passed. The individual failures are numbered so
 * a boot log names the exact property that broke rather than just "the heap". */
static void posix_heap_worker(void) {
    /* sbrk(0) must report a break inside the heap window before anything is
     * allocated, and must not move on its own. */
    u64 b0 = obrk(0);
    if (b0 != HEAP_USER_V_LO)            sysc(SYS_EXIT, 981, 0, 0);
    if (obrk(0) != b0)                   sysc(SYS_EXIT, 982, 0, 0);

    /* Allocations must be distinct, 16-byte aligned, and independently writable
     * — the last part is what actually proves the pages are mapped RW. */
    #define NB 24
    u8 *v[NB];
    for (int i = 0; i < NB; i++) {
        v[i] = (u8 *)omalloc((u64)(i + 1) * 37);
        if (!v[i])                       sysc(SYS_EXIT, 983, 0, 0);
        if (((u64)v[i] & 15) != 0)       sysc(SYS_EXIT, 984, 0, 0);
        for (u64 k = 0; k < (u64)(i + 1) * 37; k++) v[i][k] = (u8)(i + 1);
    }
    for (int i = 0; i < NB; i++)          /* no allocation overlapped another  */
        for (u64 k = 0; k < (u64)(i + 1) * 37; k++)
            if (v[i][k] != (u8)(i + 1))  sysc(SYS_EXIT, 985, 0, 0);

    if (obrk(0) <= b0)                   sysc(SYS_EXIT, 986, 0, 0);  /* brk grew */

    /* free + realloc: freed space must be reusable, and realloc must preserve
     * contents when it relocates. */
    for (int i = 0; i < NB; i += 2) ofree(v[i]);
    u8 *big = (u8 *)omalloc(4096);
    if (!big)                            sysc(SYS_EXIT, 987, 0, 0);
    for (int k = 0; k < 4096; k++) big[k] = (u8)(k & 0xFF);
    u8 *grown = (u8 *)orealloc(big, 16384);
    if (!grown)                          sysc(SYS_EXIT, 988, 0, 0);
    for (int k = 0; k < 4096; k++)
        if (grown[k] != (u8)(k & 0xFF))  sysc(SYS_EXIT, 989, 0, 0);

    /* Churn: many alloc/free cycles must not walk the break upward without
     * bound — that is the difference between a free list and a leak. */
    u64 before = obrk(0);
    for (int r = 0; r < 400; r++) {
        u8 *t = (u8 *)omalloc(512);
        if (!t)                          sysc(SYS_EXIT, 990, 0, 0);
        t[0] = 1; t[511] = 2;
        ofree(t);
    }
    if (obrk(0) != before)               sysc(SYS_EXIT, 991, 0, 0);  /* reused! */

    /* The kernel must refuse a break outside the window rather than map wild. */
    if (obrk(1) != obrk(0))              sysc(SYS_EXIT, 992, 0, 0);

    sysc(SYS_EXIT, 980, 0, 0);
}

/* ============================================================================
 * v0.56 Stage E: the native compiler is part of this image (see user/occ.c).
 * Included rather than linked because the Makefile builds exactly one ring-3
 * translation unit; occ uses this file's heap, fd table and string helpers.
 * ==========================================================================*/
#include "occ.c"

/* --- role 37: run occ ------------------------------------------------------
 * argv is the command line: occ <source.c> <output.elf>. Diagnostics go to
 * stderr, so a compile error lands in the Cyber-Terminal like any other
 * program's output. Exit status is 0 on success. */
#define OCC_MAXINPUT 8

/* v0.57: the command line accepts SEVERAL sources.
 *
 *     occ a.c b.c c.c -o out.elf     explicit output
 *     occ a.c out.elf                the legacy two-argument form
 *
 * The `-o` form is the real one. The positional form is kept because every
 * existing caller uses it — including the self-hosting driver and the `cc`
 * shell command — and silently changing what the last argument means would
 * turn a working invocation into one that overwrites its own source. When no
 * -o is given the LAST argument is the output, which is why a bare
 * `occ a.c b.c` is rejected rather than guessed at: with no -o there is no way
 * to tell a second input from an output path. */
static void occ_main(int argc, const char **argv) {
    const char *srcs[OCC_MAXINPUT];
    int nsrc = 0;
    const char *out = 0;

    for (int i = 1; i < argc && argv[i]; i++) {
        if (ostrneq(argv[i], "-o", 3)) {
            if (i + 1 < argc && argv[i + 1]) out = argv[++i];
            else { oputs("occ: -o needs a path\n"); sysc(SYS_EXIT, 902, 0, 0); }
            continue;
        }
        if (nsrc < OCC_MAXINPUT) srcs[nsrc++] = argv[i];
        else { oputs("occ: too many input files\n"); sysc(SYS_EXIT, 903, 0, 0); }
    }

    if (!out) {
        /* legacy positional form: the last argument is the output */
        if (nsrc < 2) {
            oputs("usage: occ <source.c>... -o <output.elf>\n");
            sysc(SYS_EXIT, 904, 0, 0);
        }
        out = srcs[--nsrc];
    }
    if (!nsrc) { oputs("occ: no input files\n"); sysc(SYS_EXIT, 905, 0, 0); }

    int r = occ_compile(srcs, nsrc, out);
    if (r == 0) oputs("  [occ   ] compiled OK\n");
    sysc(SYS_EXIT, r == 0 ? 0 : (u64)(900 + (-r)), 0, 0);
}

/* --- role 36: execve BY PATH ----------------------------------------------
 * Replaces this image with /bin/init loaded out of the VFS. The exec'd copy is
 * the same program, so it must be told what to do through argv rather than
 * through its role — which is exactly the point: it proves the path-loaded
 * image received a real argv and envp built by the kernel on its new stack.
 * The child (see main() below) exits 970. */
static void posix_execpath_worker(void) {
    static const char *argv[] = { "/bin/init", "exec-child", 0 };
    static const char *envp[] = { "EXECD_BY=path", 0 };
    oexecve("/bin/init", argv, envp);          /* never returns on success */
    sysc(SYS_EXIT, 971, 0, 0);                 /* execve failed */
}

/* --- role 38: SELF-HOSTING ------------------------------------------------
 * The whole point of the milestone in one worker: author a C source file from
 * ring 3, compile it with the native compiler running as a separate process,
 * then run the binary that compiler produced. Nothing here touches a host
 * toolchain, and the only thing linking the three steps is the filesystem.
 *
 * v0.56 Stage F: the program it authors now CALLS THE SDK — strlen, strcmp,
 * strcpy, atoi, malloc, putdec and puts all come from /usr/lib/libc.oc, which
 * occ prepends to the translation unit. That is deliberate: it is the only
 * check that can tell a real /usr/lib from a directory of props. Nothing in
 * this source defines those names, so if the prelude were not being read the
 * compile would fail with "undefined function" and this worker would exit 944
 * rather than quietly passing.
 *
 * The expected answer is built from pieces that each prove a different thing:
 *     0+1+..+7 = 28   the for loop and local arithmetic
 *     fib(10)  = 55   recursion through the fixup table
 *     strlen   =  6   byte addressing via __ldb (a[i] alone cannot do this)
 *     atoi     = 11   byte addressing plus the digit loop
 *     (1<<4)|3 = 19   the shift and bitwise-or levels
 *     BONUS    =  7   v0.57: an object-like #define
 *     ADD(1,2) =  3   v0.57: a function-like #define with arguments
 *     PPOK     =  5   v0.57: #ifdef took the taken branch (the other is 999,
 *                     so picking wrong is unmissable rather than off-by-one)
 *     GUARD_OK =  2   v0.57: #ifndef — the shape every header guard uses
 *     n1.in.a+b =  7   v0.57: NESTED struct member reads through two levels
 *     helper2(2) = 4   v0.57: a CROSS-UNIT call into /src/lib2.c, which itself
 *                     calls fib() back in this file — 2 + fib(3) = 4
 *                ---
 *                147
 *
 * The struct block also returns 910-918 on specific failures rather than a
 * wrong total, so a broken offset says WHICH property broke. The two that
 * matter most:
 *   n1.tag = 300 then n1.tag == 44  — a `char` member is stored ONE byte
 *     wide (300 & 0xFF == 44). A qword store here would also flatten val,
 *     which is why n1.val is re-checked immediately after.
 *   u.word = 0; u.byte = 7; u.word == 7  — a union really overlays its
 *     members at offset 0.
 *
 * Exit 940 = the produced binary ran and returned exactly that. */
/* v0.57: the source now starts with REAL preprocessing. Every line above main()
 * is load-bearing: the three #includes are resolved against /usr/include on the
 * VFS and their prototypes are parsed, SYS_WRITE comes from outrun_abi.h rather
 * than a hardcoded 0, ADD is a function-like macro, and the #ifdef/#else pair
 * proves conditional compilation picks the taken branch and discards the other.
 * If any of that silently did nothing, the arithmetic below stops adding up. */
/* v0.57 Stage C: a header shared by BOTH units, authored next to them so the
 * quoted #include form has to resolve it relative to /src/ rather than
 * /usr/include. Because each unit is preprocessed with a fresh macro table,
 * this header's guard is fresh too and its text really is pasted into both —
 * which is exactly why occ accepts a struct redefinition whose layout is
 * identical and rejects one whose layout is not. */
#define SELF_HDR \
  "#ifndef SHARED_H\n" \
  "#define SHARED_H 1\n" \
  "struct Inner { int a; int b; };\n" \
  "struct Outer { char tag; int val; struct Inner in; struct Outer *next; };\n" \
  "union U { int word; char byte; };\n" \
  "typedef struct Outer Node;\n" \
  "int helper2(int x);\n" \
  "int fib(int n);\n" \
  "#endif\n"

/* The SECOND translation unit. It calls fib(), which is defined in the FIRST
 * one, and is itself called from the first — so a single compile has to resolve
 * a reference in each direction: backward to an already-emitted function, and
 * forward through the fixup table to one that does not exist yet. It also uses
 * Node, proving the shared header reached this unit too. */
#define SELF_SRC2 \
  "#include \"shared.h\"\n" \
  "int helper2(int x) { return x + fib(3); }\n" \
  "int tag_of(Node *p) { return p->tag; }\n"

#define SELF_SRC \
  "#include <stdio.h>\n" \
  "#include <string.h>\n" \
  "#include <outrun_abi.h>\n" \
  "#define BONUS 7\n" \
  "#define ADD(a,b) ((a) + (b))\n" \
  "#define WANT_PP 1\n" \
  "#ifdef WANT_PP\n" \
  "#define PPOK 5\n" \
  "#else\n" \
  "#define PPOK 999\n" \
  "#endif\n" \
  "#ifndef NOT_DEFINED\n" \
  "#define GUARD_OK 2\n" \
  "#endif\n" \
  "#include \"shared.h\"\n" \
  "int bump(Node *p) { p->val = p->val + 1; return p->val; }\n" \
  "int fib(int n) { if (n < 2) return n; return fib(n-1) + fib(n-2); }\n" \
  "int main() {\n" \
  "  int s; int i; char *p;\n" \
  "  s = 0;\n" \
  "  for (i = 0; i < 8; i = i + 1) { s = s + i; }\n" \
  "  s = s + fib(10);\n" \
  "  s = s + strlen(\"outrun\");\n" \
  "  s = s + atoi(\"11\");\n" \
  "  if (strcmp(\"abc\", \"abc\") != 0) { return 901; }\n" \
  "  if (strcmp(\"abc\", \"abd\") == 0) { return 902; }\n" \
  "  p = malloc(64);\n" \
  "  if (p == 0) { return 903; }\n" \
  "  strcpy(p, \"sdk\");\n" \
  "  if (strlen(p) != 3) { return 904; }\n" \
  "  if (__ldb(p, 0) != 115) { return 905; }\n" \
  "  s = s + ((1 << 4) | 3);\n" \
  "  s = s + BONUS;\n" \
  "  s = s + ADD(1, 2);\n" \
  "  s = s + PPOK;\n" \
  "  s = s + GUARD_OK;\n" \
  "  { Node n1; Node n2; union U u;\n" \
  "    n1.tag = 65; n1.val = 10; n1.in.a = 3; n1.in.b = 4; n1.next = &n2;\n" \
  "    n2.tag = 66; n2.val = 20; n2.in.a = 0; n2.in.b = 0; n2.next = 0;\n" \
  "    if (n1.tag != 65) { return 910; }\n" \
  "    if (n1.in.a + n1.in.b != 7) { return 911; }\n" \
  "    if (n1.next->val != 20) { return 912; }\n" \
  "    if (bump(&n2) != 21) { return 913; }\n" \
  "    if (n1.next->val != 21) { return 914; }\n" \
  "    n1.next->in.b = 9;\n" \
  "    if (n2.in.b != 9) { return 915; }\n" \
  "    u.word = 0; u.byte = 7;\n" \
  "    if (u.word != 7) { return 916; }\n" \
  "    n1.tag = 300;\n" \
  "    if (n1.tag != 44) { return 917; }\n" \
  "    if (n1.val != 10) { return 918; }\n" \
  "    if (tag_of(&n1) != 44) { return 919; }\n" \
  "    { char cb[8]; int iv[4]; int j;\n" \
  "      strcpy(cb, \"abc\");\n" \
  "      if (strlen(cb) != 3) { return 920; }\n" \
  "      if (cb[0] != 97) { return 921; }\n" \
  "      if (cb[2] != 99) { return 922; }\n" \
  "      cb[1] = 90;\n" \
  "      if (cb[1] != 90) { return 923; }\n" \
  "      if (cb[0] != 97) { return 924; }\n" \
  "      if (cb[2] != 99) { return 925; }\n" \
  "      j = 0; while (j < 4) { iv[j] = j * 100; j = j + 1; }\n" \
  "      if (iv[0] != 0) { return 926; }\n" \
  "      if (iv[3] != 300) { return 927; }\n" \
  "      if ((&cb[1]) - (&cb[0]) != 1) { return 928; }\n" \
  "      if ((&iv[1]) - (&iv[0]) != 8) { return 929; }\n" \
  "    }\n" \
  "    s = s + n1.in.a + n1.in.b;\n" \
  "    s = s + helper2(2);\n" \
  "  }\n" \
  "  __syscall(SYS_WRITE, \"  [a.out ] SYS_WRITE came from <outrun_abi.h>\\n\", 0, 0);\n" \
  "  puts(\"  [a.out ] compiled by occ against /usr/lib/libc.oc; main returns \");\n" \
  "  putdec(s); puts(\"\\n\");\n" \
  "  return s;\n" \
  "}\n"

static int ounlink(const char *path) { return (int)(i64)sysc(SYS_VFS_UNLINK, (u64)path, 0, 0); }

/* Author one file into the VFS; returns 0 on success. */
static int selfhost_author(const char *path, const char *text) {
    int fd = ocreat(path);
    if (fd < 0) return -1;
    if (owrite(fd, text, ostrlen(text) + 1) <= 0) { oclose(fd); return -2; }
    oclose(fd);
    return 0;
}

/* --- role 40: pipe mechanics (the ring-3 half of `pipestrs`) ---------------
 *
 * Works on RAW kernel descriptors rather than through g_ofd[], because a pipe
 * end IS a kernel descriptor and layering the userland table over it would
 * only put a second mapping between the assertion and the thing asserted.
 *
 * Exit codes name the broken property, in the 95x family:
 *   950 all assertions held      955 EOF not reported once the last writer left
 *   951 SYS_PIPE failed          956 EPIPE not reported with no reader left
 *   952 round trip corrupted     957 cross-process transfer failed
 *   953 EAGAIN not reported      958 redirection failed
 *   954 buffer bound wrong       959 fork failed
 */
static void pipe_worker(void) {
    u64 fds[2];
    char buf[128];
    int i;
    const char *msg = "outrun-pipe";
    u64 mlen = ostrlen(msg);

    /* 1. a pipe hands back two DISTINCT descriptors. */
    if ((i64)sysc(SYS_PIPE, (u64)fds, 0, 0) != 0)          sysc(SYS_EXIT, 951, 0, 0);
    int rfd = (int)fds[0], wfd = (int)fds[1];
    if (rfd < 0 || wfd < 0 || rfd == wfd)                  sysc(SYS_EXIT, 951, 0, 0);

    /* 2. bytes survive the round trip, in order. */
    if ((i64)sysc(SYS_WRITE_FILE, (u64)wfd, (u64)msg, mlen) != (i64)mlen)
                                                           sysc(SYS_EXIT, 952, 0, 0);
    if ((i64)sysc(SYS_READ, (u64)rfd, (u64)buf, sizeof buf) != (i64)mlen)
                                                           sysc(SYS_EXIT, 952, 0, 0);
    for (i = 0; i < (int)mlen; i++) if (buf[i] != msg[i])   sysc(SYS_EXIT, 952, 0, 0);

    /* 3. EMPTY but a writer still exists: that is "not yet" (EAGAIN), NOT end
     *    of file. A reader that confuses the two exits early on a slow
     *    producer, which is the single most common way to break a pipeline. */
    if ((i64)sysc(SYS_READ, (u64)rfd, (u64)buf, 8) != -11)  sysc(SYS_EXIT, 953, 0, 0);

    /* 4. the buffer is FINITE, and says so honestly: a write larger than the
     *    free space is SHORT (never a silent truncation), and a write with no
     *    space at all is EAGAIN. The exact capacity is deliberately not
     *    asserted here — only that the bound exists and is reported. */
    static char big[4096];
    for (i = 0; i < 4096; i++) big[i] = (char)('a' + (i % 26));
    i64 w = (i64)sysc(SYS_WRITE_FILE, (u64)wfd, (u64)big, 4096);
    if (w <= 0 || w >= 4096)                                sysc(SYS_EXIT, 954, 0, 0);
    if ((i64)sysc(SYS_WRITE_FILE, (u64)wfd, (u64)big, 8) != -11)
                                                            sysc(SYS_EXIT, 954, 0, 0);
    u64 drained = 0;
    while (drained < (u64)w) {
        i64 n = (i64)sysc(SYS_READ, (u64)rfd, (u64)buf, sizeof buf);
        if (n <= 0)                                         sysc(SYS_EXIT, 954, 0, 0);
        drained += (u64)n;
    }
    if (drained != (u64)w)                                  sysc(SYS_EXIT, 954, 0, 0);

    /* 5. the LAST writer closing turns the same empty read into a real EOF. */
    sysc(SYS_CLOSE, (u64)wfd, 0, 0);
    if ((i64)sysc(SYS_READ, (u64)rfd, (u64)buf, 8) != 0)    sysc(SYS_EXIT, 955, 0, 0);
    sysc(SYS_CLOSE, (u64)rfd, 0, 0);

    /* 6. writing with no reader left can never succeed, so it must fail loudly
     *    rather than block forever. */
    if ((i64)sysc(SYS_PIPE, (u64)fds, 0, 0) != 0)           sysc(SYS_EXIT, 951, 0, 0);
    rfd = (int)fds[0]; wfd = (int)fds[1];
    sysc(SYS_CLOSE, (u64)rfd, 0, 0);
    if ((i64)sysc(SYS_WRITE_FILE, (u64)wfd, (u64)msg, mlen) != -32)
                                                            sysc(SYS_EXIT, 956, 0, 0);
    sysc(SYS_CLOSE, (u64)wfd, 0, 0);

    /* 7. ACROSS A FORK — the property the whole milestone exists for. The
     *    child inherits both ends, writes through its own, and closes it. The
     *    parent drops its write end FIRST, so the EOF the parent finally sees
     *    can only have come from the child's close: that is what proves the
     *    refcount is per-DESCRIPTOR and not per-process. */
    const char *cmsg = "child-says-hi";
    u64 clen = ostrlen(cmsg);
    if ((i64)sysc(SYS_PIPE, (u64)fds, 0, 0) != 0)           sysc(SYS_EXIT, 951, 0, 0);
    rfd = (int)fds[0]; wfd = (int)fds[1];
    i64 r = ofork();
    if (r == 0) {
        sysc(SYS_CLOSE, (u64)rfd, 0, 0);                    /* not the child's end */
        sysc(SYS_WRITE_FILE, (u64)wfd, (u64)cmsg, clen);
        sysc(SYS_CLOSE, (u64)wfd, 0, 0);
        sysc(SYS_EXIT, 43, 0, 0);
    }
    if (r < 0)                                              sysc(SYS_EXIT, 959, 0, 0);
    sysc(SYS_CLOSE, (u64)wfd, 0, 0);
    u64 tot = 0; int spins = 0;
    while (tot < clen && spins < 200000) {
        i64 n = (i64)sysc(SYS_READ, (u64)rfd, (u64)(buf + tot), sizeof buf - tot);
        if (n > 0) tot += (u64)n;
        else if (n == 0) break;                             /* EOF before the payload */
        else if (n == -11) { sysc(SYS_YIELD, 0, 0, 0); spins++; }
        else                                                sysc(SYS_EXIT, 957, 0, 0);
    }
    if (tot != clen)                                        sysc(SYS_EXIT, 957, 0, 0);
    for (i = 0; i < (int)clen; i++) if (buf[i] != cmsg[i])  sysc(SYS_EXIT, 957, 0, 0);
    if ((i64)sysc(SYS_READ, (u64)rfd, (u64)buf, 8) != 0)    sysc(SYS_EXIT, 957, 0, 0);
    sysc(SYS_CLOSE, (u64)rfd, 0, 0);
    if (owaitpid((u32)r, 30000) != 43)                      sysc(SYS_EXIT, 957, 0, 0);

    /* 8. REDIRECTION into a pipe: an ordinary SYS_WRITE, which knows nothing
     *    about any of this, lands in the pipe instead of on the console. */
    if ((i64)sysc(SYS_PIPE, (u64)fds, 0, 0) != 0)           sysc(SYS_EXIT, 951, 0, 0);
    rfd = (int)fds[0]; wfd = (int)fds[1];
    if ((i64)sysc(SYS_SETREDIR, 1, (u64)(i64)wfd, 0) != 0)  sysc(SYS_EXIT, 958, 0, 0);
    sysc(SYS_WRITE, (u64)"redirected", 0, 0);
    sysc(SYS_SETREDIR, 1, (u64)(i64)-1, 0);                 /* back to the console */
    sysc(SYS_CLOSE, (u64)wfd, 0, 0);
    if ((i64)sysc(SYS_READ, (u64)rfd, (u64)buf, sizeof buf) != 10)
                                                            sysc(SYS_EXIT, 958, 0, 0);
    for (i = 0; i < 10; i++) if (buf[i] != "redirected"[i]) sysc(SYS_EXIT, 958, 0, 0);
    sysc(SYS_CLOSE, (u64)rfd, 0, 0);

    /* 9. REDIRECTION into a FILE, twice, to prove the write APPENDS. A
     *    redirected stdout is a stream of writes and the store's write
     *    primitive replaces whole files, so this is the case that would
     *    silently keep only the last line if the append staging were wrong. */
    i64 k = (i64)sysc(SYS_OPEN, (u64)"tmp/redir.txt", 1, 0);   /* O_CREAT */
    if (k < 0)                                              sysc(SYS_EXIT, 958, 0, 0);
    if ((i64)sysc(SYS_SETREDIR, 1, (u64)k, 0) != 0)         sysc(SYS_EXIT, 958, 0, 0);
    sysc(SYS_WRITE, (u64)"one:", 0, 0);
    sysc(SYS_WRITE, (u64)"two", 0, 0);
    sysc(SYS_SETREDIR, 1, (u64)(i64)-1, 0);
    sysc(SYS_CLOSE, (u64)k, 0, 0);
    k = (i64)sysc(SYS_OPEN, (u64)"tmp/redir.txt", 0, 0);
    if (k < 0)                                              sysc(SYS_EXIT, 958, 0, 0);
    i64 fn = (i64)sysc(SYS_READ, (u64)k, (u64)buf, sizeof buf);
    sysc(SYS_CLOSE, (u64)k, 0, 0);
    if (fn != 7)                                            sysc(SYS_EXIT, 958, 0, 0);
    for (i = 0; i < 7; i++) if (buf[i] != "one:two"[i])     sysc(SYS_EXIT, 958, 0, 0);

    /* 10. THE HEAP SURVIVES FORK. Not strictly a pipe property, but it is the
     *     other half of what a shell needs from fork and it was silently
     *     missing until v0.59: the heap sits above the window boundary that
     *     vm_clone_user used as its upper bound, so a forked child faulted on
     *     its first access to malloc'd memory. Nothing caught it because no
     *     suite forked and then used the heap — this one does. */
    char *hp = (char *)omalloc(256);
    if (!hp)                                                sysc(SYS_EXIT, 959, 0, 0);
    for (i = 0; i < 256; i++) hp[i] = (char)(i & 0x7F);
    i64 hr = ofork();
    if (hr == 0) {
        for (int k = 0; k < 256; k++)
            if (hp[k] != (char)(k & 0x7F)) sysc(SYS_EXIT, 949, 0, 0);
        hp[0] = 'X';                                        /* private, not shared */
        sysc(SYS_EXIT, 44, 0, 0);
    }
    if (hr < 0)                                             sysc(SYS_EXIT, 959, 0, 0);
    if (owaitpid((u32)hr, 30000) != 44)                     sysc(SYS_EXIT, 949, 0, 0);
    if (hp[0] != 0)                                         sysc(SYS_EXIT, 949, 0, 0);  /* copy, not alias */

    oputs("  [pipe  ] bounds, EAGAIN/EOF/EPIPE, fork inheritance, redirection "
          "and heap-across-fork all hold\n");
    sysc(SYS_EXIT, 950, 0, 0);
}

/* --- role 41: build /bin/vsh with occ, then run a real script through it ---
 *
 * This is the end-to-end claim of the milestone: the shell is COMPILED on the
 * running system out of /src/vsh.c by /bin/occ, and then executes a script
 * that uses `>` and `|` against programs that were themselves compiled the
 * same way. Nothing here was built by a host toolchain.
 *
 * Two tiny filters are authored and compiled alongside it, because a pipeline
 * needs something to pipe: /bin/emit writes a known string, /bin/wcx counts
 * the bytes arriving on its stdin. wcx is the piece that proves a REDIRECTED
 * READER works — it calls ttyread() exactly as an interactive program would,
 * and gets the upstream stage's bytes instead of the keyboard.
 *
 * Exit codes, 96x family:
 *   960 all good        963 authoring a source failed
 *   961 a compile failed 964 vsh itself did not run
 *   962 fork failed
 */
#define VSH_EMIT_SRC \
  "int main() { puts(\"PIPEDATA\"); return 0; }\n"

/* Counts stdin bytes and prints the count. The EAGAIN/EOF distinction is the
 * whole subtlety: 0 means the upstream writer is gone for good, negative means
 * "nothing yet, try again". Treating EAGAIN as EOF would report 0 bytes on any
 * producer that was not already finished. */
#define VSH_WCX_SRC \
  "int main() {\n" \
  "  int n; int t; char b[64];\n" \
  "  t = 0;\n" \
  "  while (1) {\n" \
  "    n = ttyread(b, 64);\n" \
  "    if (n == 0) { putdec(t); return 0; }\n" \
  "    if (n < 0) { yield(); } else { t = t + n; }\n" \
  "  }\n" \
  "  return 0;\n" \
  "}\n"

#define VSH_SCRIPT \
  "# generated by pipestrs\n" \
  "/bin/emit > tmp/one.txt\n" \
  "/bin/emit | /bin/wcx > tmp/two.txt\n"

static int vsh_compile(const char *src, const char *out) {
    i64 pid = ofork();
    if (pid == 0) {
        const char *av[] = { "/bin/occ", src, "-o", out, 0 };
        const char *ev[] = { "STAGE=compile", 0 };
        oexecve("/bin/occ", av, ev);
        sysc(SYS_EXIT, 199, 0, 0);
    }
    if (pid < 0) return -1;
    i64 st = owaitpid((u32)pid, 250000);
    return st == 0 ? 0 : -1;
}

static void vsh_worker(void) {
    if (selfhost_author("/src/emit.c", VSH_EMIT_SRC) < 0)  sysc(SYS_EXIT, 963, 0, 0);
    if (selfhost_author("/src/wcx.c",  VSH_WCX_SRC)  < 0)  sysc(SYS_EXIT, 963, 0, 0);
    if (selfhost_author("/src/t.vsh",  VSH_SCRIPT)   < 0)  sysc(SYS_EXIT, 963, 0, 0);

    /* /src/vsh.c is published by the kernel's SDK, not authored here — the
     * point is that the shipped source compiles, unmodified. */
    if (vsh_compile("/src/vsh.c",  "/bin/vsh") < 0)        sysc(SYS_EXIT, 961, 0, 0);
    if (vsh_compile("/src/emit.c", "/bin/emit") < 0)       sysc(SYS_EXIT, 961, 0, 0);
    if (vsh_compile("/src/wcx.c",  "/bin/wcx") < 0)        sysc(SYS_EXIT, 961, 0, 0);
    oputs("  [vsh   ] occ built /bin/vsh, /bin/emit and /bin/wcx from source\n");

    i64 pid = ofork();
    if (pid == 0) {
        const char *av[] = { "/bin/vsh", "/src/t.vsh", 0 };
        const char *ev[] = { "OUTRUN=1", 0 };
        oexecve("/bin/vsh", av, ev);
        sysc(SYS_EXIT, 199, 0, 0);
    }
    if (pid < 0)                                           sysc(SYS_EXIT, 962, 0, 0);
    if (owaitpid((u32)pid, 250000) != 0)                   sysc(SYS_EXIT, 964, 0, 0);

    oputs("  [vsh   ] /bin/vsh ran a script using '>' and '|'\n");
    sysc(SYS_EXIT, 960, 0, 0);
}

static void posix_selfhost_worker(void) {
    static const char *csrc  = SELF_SRC;
    static const char *csrc2 = SELF_SRC2;
    static const char *chdr  = SELF_HDR;
    /* 1. author a shared header and TWO translation units into the VFS */
    if (selfhost_author("/src/shared.h", chdr) < 0) sysc(SYS_EXIT, 941, 0, 0);
    if (selfhost_author("/src/t.c",      csrc) < 0) sysc(SYS_EXIT, 942, 0, 0);
    if (selfhost_author("/src/lib2.c",   csrc2) < 0) sysc(SYS_EXIT, 949, 0, 0);

    /* 2. compile BOTH units in one invocation, in a separate process running
     *    the real compiler. -o is the explicit output form. */
    i64 pid = ofork();
    if (pid == 0) {
        static const char *av[] = { "/bin/occ", "/src/t.c", "/src/lib2.c",
                                    "-o", "/bin/t.elf", 0 };
        static const char *ev[] = { "STAGE=compile", 0 };
        oexecve("/bin/occ", av, ev);
        sysc(SYS_EXIT, 199, 0, 0);                /* exec failed */
    }
    if (pid < 0)                                  sysc(SYS_EXIT, 943, 0, 0);
    /* v0.56 Stage F: the budget went from 60000 to 250000 spins, and a timeout
     * now has its OWN exit code. With the SDK prelude in front of it the
     * compiler has ~4 KiB more source to get through and emits an image nine
     * times larger, and under TCG that pushed it past the old budget — the
     * parent gave up while the child was still working, then reported the
     * generic "child failed" 944 even though the compile went on to succeed
     * and print "compiled OK". A wait that times out and a child that fails
     * are different events and must not share an exit code. */
    i64 cst = owaitpid((u32)pid, 250000);
    if (cst == -11)                               sysc(SYS_EXIT, 947, 0, 0);
    if (cst != 0)                                 sysc(SYS_EXIT, 944, 0, 0);

    /* 3. run what the compiler produced */
    pid = ofork();
    if (pid == 0) {
        static const char *av2[] = { "/bin/t.elf", 0 };
        static const char *ev2[] = { 0 };
        oexecve("/bin/t.elf", av2, ev2);
        sysc(SYS_EXIT, 198, 0, 0);
    }
    if (pid < 0)                                  sysc(SYS_EXIT, 945, 0, 0);
    i64 rst = owaitpid((u32)pid, 250000);
    if (rst == -11)                               sysc(SYS_EXIT, 948, 0, 0);
    /* 28 + 55 + 6 + 11 + 19 + 7 (BONUS) + 3 (ADD) + 5 (PPOK) + 2 (GUARD_OK)
     * — see the SELF_SRC comment for what each proves.
     * The value is PRINTED on failure: "the program returned the wrong answer"
     * is not actionable, but "it returned 100" points straight at which of the
     * five contributions above did not happen. */
    if (rst != 147) {
        oputs("  [self  ] the compiled program returned ");
        sysc(SYS_WRITEHEX, (u64)rst, 0, 0);
        oputs(" hex (want 93 hex = 147)\n");
        sysc(SYS_EXIT, 946, 0, 0);
    }

    oputs("  [self  ] authored, compiled and RAN a program without a host toolchain\n");
    sysc(SYS_EXIT, 940, 0, 0);
}

/* --- role 39: COMPILER COMPLETENESS (the ring-3 half of `compilerstrs`) ----
 * Where role 38 proves the self-hosting LOOP works, this one interrogates the
 * LANGUAGE. It authors a small project, compiles it, runs it, and then proves
 * the compiler REJECTS four things it must reject — because a compiler that
 * accepts everything is not a compiler that understands anything.
 *
 * The positive program checks its own struct offsets from the inside, using
 * `&s.member - &s`. That is the strongest form this test can take: it is not
 * comparing against numbers the compiler reported, it is measuring the
 * addresses the compiler actually generated.
 *
 * Exit 950 = everything passed. Anything else names the step that failed.  */
#define CS_HDR \
  "#ifndef CS_HDR_H\n" \
  "#define CS_HDR_H 1\n" \
  "#define CS_ONE 1\n" \
  "#define CS_TWO (CS_ONE + 1)\n" \
  "#define CS_MUL(a,b) ((a) * (b))\n" \
  "#ifdef CS_HDR_H\n" \
  "#define CS_GUARDED 4\n" \
  "#else\n" \
  "#define CS_GUARDED 999\n" \
  "#endif\n" \
  "struct S1 { char a; int b; };\n" \
  "struct S2 { char a; char b; int c; };\n" \
  "struct N  { char t; struct S1 s; };\n" \
  "union  U  { int w; char b; };\n" \
  "typedef struct N Nest;\n" \
  "int cs_sum(Nest *p);\n" \
  "int cs_off_b(void);\n" \
  "#endif\n"

/* Unit A: main, plus a helper the OTHER unit calls back into. */
#define CS_A \
  "#include \"cs_hdr.h\"\n" \
  "int cs_off_b(void) { struct S1 s; int base; int m; base = &s; m = &s.b; return m - base; }\n" \
  "int main() {\n" \
  "  struct S1 s1; struct S2 s2; Nest n; union U u;\n" \
  "  int base;\n" \
  "  if (CS_TWO != 2) { return 1; }\n" \
  "  if (CS_MUL(3,4) != 12) { return 2; }\n" \
  "  if (CS_GUARDED != 4) { return 3; }\n" \
  "  base = &s1;\n" \
  "  if ((&s1.a) - base != 0) { return 10; }\n" \
  "  if ((&s1.b) - base != 8) { return 11; }\n" \
  "  base = &s2;\n" \
  "  if ((&s2.a) - base != 0) { return 12; }\n" \
  "  if ((&s2.b) - base != 1) { return 13; }\n" \
  "  if ((&s2.c) - base != 8) { return 14; }\n" \
  "  base = &n;\n" \
  "  if ((&n.t) - base != 0) { return 15; }\n" \
  "  if ((&n.s) - base != 8) { return 16; }\n" \
  "  if ((&n.s.b) - base != 16) { return 17; }\n" \
  "  base = &u;\n" \
  "  if ((&u.w) - base != 0) { return 18; }\n" \
  "  if ((&u.b) - base != 0) { return 19; }\n" \
  "  s2.a = 0; s2.b = 0; s2.c = 7;\n" \
  "  s2.a = 300;\n" \
  "  if (s2.a != 44) { return 20; }\n" \
  "  if (s2.b != 0)  { return 21; }\n" \
  "  if (s2.c != 7)  { return 22; }\n" \
  "  n.t = 5; n.s.a = 2; n.s.b = 30;\n" \
  "  if (cs_sum(&n) != 37) { return 23; }\n" \
  "  if (cs_off_b() != 8)  { return 24; }\n" \
  "  return 0;\n" \
  "}\n"

/* Unit B: reads a nested struct through a pointer, and is reached only if the
 * shared header laid the type out identically in both units. */
#define CS_B \
  "#include \"cs_hdr.h\"\n" \
  "int cs_sum(Nest *p) { return p->t + p->s.a + p->s.b; }\n"

/* Four programs that MUST NOT compile. */
#define CS_N1 "#include \"cs_hdr.h\"\nint main() { struct S1 *p; return p.a; }\n"
#define CS_N2 "int main() { return 0; }\nint main() { return 1; }\n"
#define CS_N3 "struct Q { int a; };\nstruct Q { char a; };\nint main() { return 0; }\n"
#define CS_N4 "#include \"cs_hdr.h\"\nint main() { struct S1 s; return s.zzz; }\n"

/* Compile `srcs` and return the compiler's exit status, or -1 if it could not
 * be run at all. Used for both the positive and the negative rounds. */
static i64 cs_compile(const char **av) {
    i64 pid = ofork();
    if (pid == 0) {
        static const char *ev[] = { "STAGE=compilerstrs", 0 };
        oexecve("/bin/occ", av, ev);
        sysc(SYS_EXIT, 199, 0, 0);
    }
    if (pid < 0) return -1;
    return owaitpid((u32)pid, 250000);
}

/* --- role 40: LANGUAGE COMPLETENESS (the ring-3 half of `langstrs`) --------
 * v0.60. compilerstrs interrogates types and linkage; this interrogates the
 * four constructs v0.60 added — sizeof, declarations in a for-initialiser,
 * switch/case/default, and the unsigned integer types — plus break/continue,
 * which switch is useless without.
 *
 * Every unsigned check below is written so that SIGNED code generation gives
 * the WRONG answer. That is deliberate and it is the whole value of the test:
 * `big > 1` is true for an unsigned 0xFFFF...F and false for a signed -1, so a
 * compiler that parsed `u64` and then emitted setg fails here rather than
 * passing quietly and corrupting arithmetic somewhere far away. The signed
 * block immediately after re-checks that plain `int` still uses setl/idiv/sar,
 * because the failure mode of this work is not only "unsigned stayed signed"
 * but also "everything became unsigned".
 *
 * The program returns the number of the check that failed, or 0. Exit 970 from
 * the driver means every round passed.                                       */
#define LANG_SRC \
  "struct S1 { char a; int b; };\n" \
  "int classify(int x) {\n" \
  "  int r; r = 0;\n" \
  "  switch (x) {\n" \
  "    case 1: r = 10; break;\n" \
  "    case 2: r = 20; break;\n" \
  "    case 3:\n" \
  "    case 4: r = 34; break;\n" \
  "    default: r = 99;\n" \
  "  }\n" \
  "  return r;\n" \
  "}\n" \
  "int main() {\n" \
  "  char buf[64]; int s; int i; int n;\n" \
  "  u64 big; u8 b; u16 h; u32 w; u8 ub[4]; u8 *up;\n" \
  "  /* ---- sizeof ---- */\n" \
  "  if (sizeof(char) != 1) { return 1; }\n" \
  "  if (sizeof(int)  != 8) { return 2; }\n" \
  "  if (sizeof(u8)   != 1) { return 3; }\n" \
  "  if (sizeof(u16)  != 2) { return 4; }\n" \
  "  if (sizeof(u32)  != 4) { return 5; }\n" \
  "  if (sizeof(u64)  != 8) { return 6; }\n" \
  "  if (sizeof(struct S1) != 16) { return 7; }\n" \
  "  if (sizeof(char *) != 8) { return 8; }\n" \
  "  if (sizeof buf != 64) { return 9; }\n" \
  "  /* ---- declaration in a for-initialiser, and its scope ---- */\n" \
  "  s = 0;\n" \
  "  for (int k = 0; k < 10; k = k + 1) { s = s + k; }\n" \
  "  if (s != 45) { return 10; }\n" \
  "  for (int k = 0; k < 5; k = k + 1) { s = s + 100; }\n" \
  "  if (s != 545) { return 11; }\n" \
  "  /* ---- switch / case / default, including fallthrough ---- */\n" \
  "  if (classify(1) != 10) { return 20; }\n" \
  "  if (classify(2) != 20) { return 21; }\n" \
  "  if (classify(3) != 34) { return 22; }\n" \
  "  if (classify(4) != 34) { return 23; }\n" \
  "  if (classify(9) != 99) { return 24; }\n" \
  "  /* ---- unsigned: each of these is WRONG under signed codegen ---- */\n" \
  "  big = 0; big = big - 1;\n" \
  "  if (big <= 1) { return 30; }\n" \
  "  if (big / 2 <= 1000) { return 31; }\n" \
  "  if ((big >> 60) != 15) { return 32; }\n" \
  "  if (big % 10 != 5) { return 33; }\n" \
  "  /* ---- signed must STAY signed ---- */\n" \
  "  n = 0 - 1;\n" \
  "  if (n >= 1) { return 34; }\n" \
  "  if (n / 2 != 0) { return 35; }\n" \
  "  if ((n >> 8) != 0 - 1) { return 36; }\n" \
  "  /* ---- narrowing on store, zero-extension on load ---- */\n" \
  "  b = 300;    if (b != 44) { return 40; }\n" \
  "  h = 70000;  if (h != 4464) { return 41; }\n" \
  "  w = 0; w = w - 1; if (w != 4294967295) { return 42; }\n" \
  "  /* ---- break and continue ---- */\n" \
  "  i = 0;\n" \
  "  while (1) { i = i + 1; if (i == 5) { break; } }\n" \
  "  if (i != 5) { return 50; }\n" \
  "  s = 0;\n" \
  "  for (int k = 0; k < 10; k = k + 1) { if (k % 2 == 0) { continue; } s = s + k; }\n" \
  "  if (s != 25) { return 51; }\n" \
  "  /* continue inside a switch belongs to the enclosing LOOP */\n" \
  "  s = 0;\n" \
  "  for (int k = 0; k < 6; k = k + 1) {\n" \
  "    switch (k) { case 2: continue; case 4: break; default: s = s + 1000; }\n" \
  "    s = s + 1;\n" \
  "  }\n" \
  "  if (s != 4005) { return 52; }\n" \
  "  /* ---- an unsigned ELEMENT through a pointer ---- */\n" \
  "  up = ub;\n" \
  "  __stb(ub, 0, 200); __stb(ub, 1, 1);\n" \
  "  if (up[0] != 200) { return 60; }\n" \
  "  if (up[0] <= 100) { return 61; }\n" \
  "  if (up[1] != 1)   { return 62; }\n" \
  "  return 0;\n" \
  "}\n"

/* Two programs that MUST be refused. Both are new failure modes that only
 * exist because v0.60 added the constructs, so neither could be caught by the
 * refusal round compilerstrs already runs. */
#define LANG_N1 "int main() { break; return 0; }\n"
#define LANG_N2 "int main() { switch (1) { default: ; default: ; } return 0; }\n"

static void lang_stress_worker(void) {
    if (selfhost_author("/src/lang.c", LANG_SRC) < 0) sysc(SYS_EXIT, 971, 0, 0);

    /* ---- the language round ---- */
    { static const char *av[] = { "/bin/occ", "/src/lang.c", "-o", "/bin/lang.elf", 0 };
      i64 st = cs_compile(av);
      if (st == -11) sysc(SYS_EXIT, 972, 0, 0);
      if (st != 0)   sysc(SYS_EXIT, 973, 0, 0);
    }
    { i64 pid = ofork();
      if (pid == 0) {
          static const char *av2[] = { "/bin/lang.elf", 0 };
          static const char *ev2[] = { 0 };
          oexecve("/bin/lang.elf", av2, ev2);
          sysc(SYS_EXIT, 198, 0, 0);
      }
      if (pid < 0) sysc(SYS_EXIT, 974, 0, 0);
      i64 rst = owaitpid((u32)pid, 250000);
      if (rst == -11) sysc(SYS_EXIT, 975, 0, 0);
      if (rst != 0) {
          oputs("  [lang  ] the compiled program failed check ");
          sysc(SYS_WRITEHEX, (u64)rst, 0, 0);
          oputs(" hex\n");
          sysc(SYS_EXIT, 976, 0, 0);
      }
      oputs("  [lang  ] sizeof, for-init scope, switch/case, unsigned arithmetic and break/continue all verified\n");
    }

    /* ---- refusals ---- */
    { static const char *n1[] = { "/bin/occ", "/src/lang_n.c", "-o", "/bin/lang_n.elf", 0 };
      static const char *bodies[2] = { LANG_N1, LANG_N2 };
      for (int i = 0; i < 2; i++) {
          ounlink("/bin/lang_n.elf");
          if (selfhost_author("/src/lang_n.c", bodies[i]) < 0) sysc(SYS_EXIT, 977, 0, 0);
          i64 st = cs_compile(n1);
          if (st == -11) sysc(SYS_EXIT, 978 + i, 0, 0);
          if (st == 0)   sysc(SYS_EXIT, 980 + i, 0, 0);   /* WRONGLY accepted */
          { int t = oopen("/bin/lang_n.elf");
            if (t >= 0) { oclose(t); sysc(SYS_EXIT, 982 + i, 0, 0); } }
      }
      oputs("  [lang  ] a stray break and a duplicated default were both REFUSED\n");
    }

    /* Release the sources now that they have been consumed. The VFS root
     * directory holds VFS_MAXFILES (64) entries and this suite runs LAST in the
     * boot sequence, so it inherits every file every earlier suite created —
     * without this the omake round below cannot create its output and occ
     * reports "cannot open output" from a directory that is simply full.
     * /bin/lang.elf is deliberately kept: the kernel half audits it after the
     * driver exits, and a test that deleted its own evidence would be checking
     * nothing. */
    ounlink("/src/lang.c");
    ounlink("/src/lang_n.c");

    /* ---- the toolchain round: build omake, then let omake drive occ ----
     * This is what the argv fix is for. omake hands execve an array of
     * POINTERS; while that array was declared `char *` every entry was
     * truncated to its low byte, so the compiler could never be launched. A
     * test that only compiled omake would not have noticed — it has to RUN it
     * and check that the target it was asked for actually appeared. */
    { static const char *av[] = { "/bin/occ", "/src/omake.c", "-o", "/bin/omake", 0 };
      i64 st = cs_compile(av);
      if (st == -11) sysc(SYS_EXIT, 984, 0, 0);
      if (st != 0)   sysc(SYS_EXIT, 985, 0, 0);
    }
    ounlink("/bin/hello.elf");
    ounlink("/var/omake.stamp");
    { i64 pid = ofork();
      if (pid == 0) {
          static const char *av2[] = { "/bin/omake", "-f", "/src/demo.mk", 0 };
          static const char *ev2[] = { "PATH=/usr/lib", 0 };
          oexecve("/bin/omake", av2, ev2);
          sysc(SYS_EXIT, 198, 0, 0);
      }
      if (pid < 0) sysc(SYS_EXIT, 986, 0, 0);
      i64 rst = owaitpid((u32)pid, 400000);
      if (rst == -11) sysc(SYS_EXIT, 987, 0, 0);
      if (rst != 0)   sysc(SYS_EXIT, 988, 0, 0);
      { int t = oopen("/bin/hello.elf");
        if (t < 0) sysc(SYS_EXIT, 989, 0, 0);            /* omake claimed success
                                                          * but built nothing   */
        oclose(t); }
      oputs("  [lang  ] omake parsed a makefile and drove occ to build /bin/hello.elf\n");
    }
    /* Run what omake built: hello_sum(37, 5) == 42. */
    { i64 pid = ofork();
      if (pid == 0) {
          static const char *av2[] = { "/bin/hello.elf", 0 };
          static const char *ev2[] = { 0 };
          oexecve("/bin/hello.elf", av2, ev2);
          sysc(SYS_EXIT, 198, 0, 0);
      }
      if (pid < 0) sysc(SYS_EXIT, 990, 0, 0);
      i64 rst = owaitpid((u32)pid, 250000);
      if (rst == -11) sysc(SYS_EXIT, 991, 0, 0);
      if (rst != 42)  sysc(SYS_EXIT, 992, 0, 0);
    }
    sysc(SYS_EXIT, 970, 0, 0);
}

static void compiler_stress_worker(void) {
    if (selfhost_author("/src/cs_hdr.h", CS_HDR) < 0) sysc(SYS_EXIT, 951, 0, 0);
    if (selfhost_author("/src/cs_a.c",   CS_A)   < 0) sysc(SYS_EXIT, 952, 0, 0);
    if (selfhost_author("/src/cs_b.c",   CS_B)   < 0) sysc(SYS_EXIT, 953, 0, 0);

    /* ---- positive round: two units, one ELF, run it ---- */
    { static const char *av[] = { "/bin/occ", "/src/cs_a.c", "/src/cs_b.c",
                                  "-o", "/bin/cs.elf", 0 };
      i64 st = cs_compile(av);
      if (st == -11) sysc(SYS_EXIT, 954, 0, 0);           /* compiler timed out */
      if (st != 0)   sysc(SYS_EXIT, 955, 0, 0);           /* should have built  */
    }
    { i64 pid = ofork();
      if (pid == 0) {
          static const char *av2[] = { "/bin/cs.elf", 0 };
          static const char *ev2[] = { 0 };
          oexecve("/bin/cs.elf", av2, ev2);
          sysc(SYS_EXIT, 198, 0, 0);
      }
      if (pid < 0) sysc(SYS_EXIT, 956, 0, 0);
      i64 rst = owaitpid((u32)pid, 250000);
      if (rst == -11) sysc(SYS_EXIT, 957, 0, 0);
      if (rst != 0) {
          /* the program returns the number of the check that failed */
          oputs("  [compst] the compiled program failed check ");
          sysc(SYS_WRITEHEX, (u64)rst, 0, 0);
          oputs(" hex\n");
          sysc(SYS_EXIT, 958, 0, 0);
      }
      oputs("  [compst] offsets, alignment, nesting and cross-unit calls all verified\n");
    }

    /* ---- negative rounds: each of these MUST be refused ----
     * The output is unlinked first, so "the compiler failed" and "the compiler
     * silently produced something anyway" are distinguishable. */
    { static const char *n1[] = { "/bin/occ", "/src/cs_n.c", "-o", "/bin/cs_n.elf", 0 };
      static const char *bodies[4] = { CS_N1, CS_N2, CS_N3, CS_N4 };
      for (int i = 0; i < 4; i++) {
          ounlink("/bin/cs_n.elf");
          if (selfhost_author("/src/cs_n.c", bodies[i]) < 0) sysc(SYS_EXIT, 959, 0, 0);
          i64 st = cs_compile(n1);
          if (st == -11)      sysc(SYS_EXIT, 960 + i, 0, 0);   /* timed out      */
          if (st == 0)        sysc(SYS_EXIT, 964 + i, 0, 0);   /* WRONGLY built  */
          { int t = oopen("/bin/cs_n.elf");
            if (t >= 0) { oclose(t); sysc(SYS_EXIT, 968 + i, 0, 0); } }  /* left output */
      }
      oputs("  [compst] all four invalid programs were REFUSED, and none left an output file\n");
    }
    sysc(SYS_EXIT, 950, 0, 0);
}

int main(int argc, const char **argv, const char **envp);

/* ---- crt0 -----------------------------------------------------------------
 * The real ELF entry point. The kernel enters ring 3 with RSP pointing at the
 * SysV process-start block, so this is the textbook x86-64 crt0: pull argc,
 * argv and envp off the stack, align, call into C, and turn main's return
 * value into SYS_EXIT. Written in assembly because a C function cannot make
 * guarantees about RSP on entry.                                            */
__asm__(
    ".text\n"
    ".globl _start\n"
    "_start:\n"
    "  xor %rbp, %rbp\n"
    "  mov (%rsp), %rdi\n"           /* argc                                  */
    "  lea 8(%rsp), %rsi\n"          /* argv                                  */
    "  mov %rdi, %rax\n"
    "  lea 16(%rsp,%rax,8), %rdx\n"  /* envp = argv + argc + 1                */
    "  and $-16, %rsp\n"
    "  call crt0_main\n"
    "  mov %rax, %rdi\n"             /* exit status = main's return value      */
    "  mov $2, %rax\n"               /* SYS_EXIT                               */
    "  xor %esi, %esi\n"
    "  xor %edx, %edx\n"
    "  syscall\n"
    "1: jmp 1b\n"
);

int crt0_main(int argc, const char **argv, const char **envp);
int crt0_main(int argc, const char **argv, const char **envp) {
    g_argc = argc; g_argv = argv; g_envp = envp;
    stdio_init();                                    /* stdin/stdout/stderr = 0/1/2 */
    return main(argc, argv, envp);
}

int main(int argc, const char **argv, const char **envp) {
    (void)envp;
    /* v0.56: an image exec'd BY PATH is this same program, so it cannot be
     * distinguished by role — argv is how it learns what it is. Checked before
     * the role dispatch precisely so the path-loaded copy takes this branch
     * instead of re-running whatever role its kproc still carries (which would
     * exec itself forever). */
    /* v0.56: a path-exec'd image is dispatched by ARGV[0], the way a real system
     * dispatches a multi-call binary. /bin/occ and /bin/init are the same ELF;
     * what differs is the name it was invoked under. */
    if (argc >= 1 && argv && argv[0] && ostrneq(argv[0], "/bin/occ", 9)) {
        occ_main(argc, argv);
    }
    if (argc >= 2 && argv && argv[1] && ostrneq(argv[1], "exec-child", 11)) {
        if (!ostrneq(argv[0], "/bin/init", 10))            sysc(SYS_EXIT, 972, 0, 0);
        const char *v = ogetenv("EXECD_BY");
        if (!v || !ostrneq(v, "path", 5))                  sysc(SYS_EXIT, 973, 0, 0);
        oputs("  [posix ] /bin/init re-exec'd FROM THE VFS BY PATH, argv+envp intact\n");
        sysc(SYS_EXIT, 970, 0, 0);
    }
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
    if (role == 15) { vfs_driver(); }                   /* v0.48 VFS journal/unlink/multi-volume */
    if (role == 16) { smp_migrate_worker(); }           /* v0.49 SMP remap/unmap/migration worker */
    if (role == 17) { gpu_driver(0); }                  /* v0.50 GPU client: clean create/draw/scanout/flush */
    if (role == 18) { gpu_driver(1); }                  /* v0.50 GPU client: deliberate fault before flush   */
    if (role == 19) { audio_driver(0); }                /* v0.51 audio client: clean configure/write/exit    */
    if (role == 20) { audio_driver(1); }                /* v0.51 audio client: deliberate fault after configure */
    if (role == 21) { net_driver(0); }                  /* v0.52 socket client: clean bind/connect/send/recv  */
    if (role == 22) { net_driver(1); }                  /* v0.52 socket client: deliberate fault after bind   */
    if (role == 23) { wimp_driver(0); }                 /* v0.53 WIMP app: clean window create/damage/poll    */
    if (role == 24) { wimp_driver(1); }                 /* v0.53 WIMP app: deliberate fault after window create */
    if (role == 25) { app_harness(0, 0); }              /* v0.54 GUI app: cyber-terminal                      */
    if (role == 26) { app_harness(1, 0); }              /* v0.54 GUI app: system monitor                      */
    if (role == 27) { app_harness(2, 0); }              /* v0.54 GUI app: file inspector                      */
    if (role == 28) { app_harness(0, 1); }              /* v0.54 GUI app: faults while holding a window       */
    if (role == 29) { posix_fork_worker(); }            /* v0.55 fork / waitpid / SIGCHLD                     */
    if (role == 30) { posix_signal_worker(); }          /* v0.55 SIGSEGV recovery, SIGINT frame, SIGALRM      */
    if (role == 31) { posix_thread_worker(); }          /* v0.55 pthread_create/join/exit + mutex             */
    if (role == 32) { posix_exec_parent(); }            /* v0.55 execve into role 33 with argv/envp           */
    if (role == 33) { posix_exec_child(); }             /* v0.55 the exec'd image: verifies argc/argv/envp    */
    if (role == 34) { posix_fd_worker(); }              /* v0.55 std fd table + inheritance across fork       */
    if (role == 35) { posix_heap_worker(); }            /* v0.56 ring-3 heap: sbrk/malloc/free/realloc        */
    if (role == 36) { posix_execpath_worker(); }        /* v0.56 execve /bin/init BY PATH from the VFS        */
    if (role == 37) { occ_main(argc, argv); }           /* v0.56 the native C compiler                        */
    if (role == 39) { compiler_stress_worker(); }       /* v0.57 language completeness + refusal checks        */
    if (role == 38) { posix_selfhost_worker(); }        /* v0.56 author -> compile -> run, natively           */
    if (role == 40) { pipe_worker(); }                  /* v0.59 pipe mechanics: bounds, EOF, EPIPE, fork      */
    if (role == 41) { vsh_worker(); }                   /* v0.59 build /bin/vsh with occ and run a real script */
    if (role == 44) { pthreads_smp_worker(); }         /* v0.62 mutex contention + condvar across cores     */
    if (role == 45) { sig_stress_worker(); }           /* v0.62 masks, SIG_IGN, process-group delivery      */
    if (role == 43) { thread_stress_worker(); }        /* v0.61 futex threads, kernel join, own stack       */
    if (role == 42) { lang_stress_worker(); }           /* v0.60 sizeof/for-init/switch/unsigned + omake       */
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

    return 0;                       /* crt0 turns this into SYS_EXIT(0) */
}
