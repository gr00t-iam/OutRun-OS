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
/* v0.63: dynamic virtual memory. mmap is ANONYMOUS ONLY here — there is no
 * file-backed paging, and the kernel refuses rather than returning zeroes. */
#define SYS_MMAP                71
#define SYS_MUNMAP              72
#define SYS_MPROTECT            73
#define SYS_SHM_CREATE          74
#define SYS_SHM_MAP             75
/* v0.64: event-driven I/O. epoll_ctl and epoll_wait PACK their extra arguments
 * because the syscall ABI has three argument registers; the wrappers below
 * present the ordinary shapes over that. */
#define SYS_EPOLL_CREATE        76
#define SYS_EPOLL_CTL           77
#define SYS_EPOLL_WAIT          78
#define SYS_EVENTFD             79
#define SYS_FCNTL               80          /* v0.65 */
#define SYS_LISTEN              81          /* v0.65 */
#define SYS_ACCEPT              82          /* v0.65 */
#define SYS_MMAP_FILE           83          /* v0.66 */
#define SYS_MSYNC               84          /* v0.66 */
#define SYS_UI_ADD              85          /* v0.70 */
#define SYS_UI_SET              86          /* v0.70 */
#define SYS_UI_GET              87          /* v0.70 */
/* v0.72 added process credentials to the kernel but never published their
 * numbers here, so ring 3 had no way to ask who it was. v0.74 completes the
 * set and exposes both halves together — the identity a program HOLDS, and the
 * authentication that establishes one in the first place.
 *
 * SYS_USERADD packs uid and gid into its third argument, (gid << 32) | uid,
 * because the syscall ABI carries exactly three; see the kernel case for why
 * widening it to four was not worth touching the assembly stub for. */
#define SYS_GETUID              88          /* v0.72 */
#define SYS_GETGID              89          /* v0.72 */
#define SYS_SETUID              90          /* v0.72; v0.74 semantics: permanent drop */
#define SYS_SETGID              91          /* v0.72 */
#define SYS_CHMOD               92          /* v0.72 */
#define SYS_CHOWN               93          /* v0.72 */
#define SYS_GETEUID             94          /* v0.74 */
#define SYS_GETEGID             95          /* v0.74 */
#define SYS_SETEUID             96          /* v0.74: the REVERSIBLE drop */
#define SYS_SETEGID             97          /* v0.74 */
#define SYS_AUTH                98          /* v0.74: (name, password) -> uid, or negative */
#define SYS_USERADD             99          /* v0.74: (name, password, (gid<<32)|uid) */
#define SYS_LSEEK              100          /* v0.82: (fd, offset, whence) -> new off  */
#define SYS_FTRUNCATE          101          /* v0.84: (fd, length) -> 0, or -errno     */

/* v0.82: the three POSIX whence values, in POSIX's order and with POSIX's
 * numbers. They are not an internal encoding to be chosen freely — a ring-3
 * program written against any C library expects 0/1/2 to mean exactly this. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* v0.70: widget kinds, mirroring the kernel's table. */
#define WG_LABEL                1
#define WG_BUTTON               2
#define WG_CHECK                3
#define WG_PROGRESS             4
#define WG_ENTRY                5           /* v0.71 */
#define WG_TEXTLEN              24          /* mirrors the kernel's widget text buffer */

/* v0.70: the widget calls pack a window id and a second small integer into a0
 * for the same reason SYS_MMAP_FILE packs its descriptor — three argument
 * registers, and the rectangle needs one to itself. */
static i64 oui_add(int win, int kind, int x, int y, int w, int h, const char *text) {
    u64 a0 = ((u64)(win & 0xFFFF)) | ((u64)(kind & 0xFFFF) << 16);
    u64 a1 = ((u64)(x & 0xFFFF)) | ((u64)(y & 0xFFFF) << 16) |
             ((u64)(w & 0xFFFF) << 32) | ((u64)(h & 0xFFFF) << 48);
    return (i64)sysc(SYS_UI_ADD, a0, a1, (u64)text);
}
static i64 oui_set(int win, int id, int what, u64 value) {
    return (i64)sysc(SYS_UI_SET, ((u64)(win & 0xFFFF)) | ((u64)(id & 0xFFFF) << 16),
                     (u64)what, value);
}
static i64 oui_get(int win, int id, int what) {
    return (i64)sysc(SYS_UI_GET, ((u64)(win & 0xFFFF)) | ((u64)(id & 0xFFFF) << 16),
                     (u64)what, 0);
}
/* v0.71: read a widget's text into a caller buffer of at least WG_TEXTLEN
 * bytes; returns the string length. This is how a program reads what was
 * typed into a field. */
static i64 oui_gettext(int win, int id, char *buf) {
    return (i64)sysc(SYS_UI_GET, ((u64)(win & 0xFFFF)) | ((u64)(id & 0xFFFF) << 16),
                     2, (u64)buf);
}

/* v0.66: SYS_MMAP_FILE packs fd/prot/flags into a0 because the dispatch ABI
 * has three argument registers and the offset needs one of its own. */
static u64 ommap_file(int kfd, u64 len, int prot, int flags, u64 off) {
    u64 a0 = ((u64)(kfd & 0xFF)) | ((u64)(prot & 0xFF) << 8) | ((u64)(flags & 0xFFFF) << 16);
    return sysc(SYS_MMAP_FILE, a0, len, off);
}
static int omsync(u64 addr, u64 len, int flags) {
    return (int)(i64)sysc(SYS_MSYNC, addr, len, (u64)flags);
}

/* v0.65: descriptor flags and socket types. Values mirror Linux so the SDK
 * headers can carry them verbatim. */
#define O_NONBLOCK              04000
#define F_GETFL                 3
#define F_SETFL                 4
#define SOCK_DGRAM              2
#define SOCK_STREAM             1
#define SOCK_NONBLOCK           0x800
#define AF_INET                 2
#define IP_LOOPBACK             0x7F000001ull

static int ofcntl(int fd, int cmd, int arg) {
    return (int)(i64)sysc(SYS_FCNTL, (u64)fd, (u64)cmd, (u64)(u32)arg);
}
static int olisten(int fd, int backlog) {
    return (int)(i64)sysc(SYS_LISTEN, (u64)fd, (u64)backlog, 0);
}
static int oaccept(int fd, u32 *peer, int flags) {
    return (int)(i64)sysc(SYS_ACCEPT, (u64)fd, (u64)(void *)peer, (u64)flags);
}
#define EPOLLIN   0x001u
#define EPOLLOUT  0x004u
#define EPOLLERR  0x008u
#define EPOLLHUP  0x010u
#define EPOLLET   0x80000000u
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLL_TTY_FD  (-2)   /* the console: it has no descriptor */

struct epoll_event { u32 events; u32 _pad; u64 data; };

static int oepoll_create(void) { return (int)(i64)sysc(SYS_EPOLL_CREATE, 0, 0, 0); }
static int oepoll_ctl(int epfd, int op, int fd, u32 events, u64 cookie) {
    return (int)(i64)sysc(SYS_EPOLL_CTL, (u64)epfd,
                          ((u64)op << 32) | (u32)fd,
                          (cookie << 32) | events);
}
/* Returns the number of events written, or -11 meaning "you slept and
 * something changed — call again", the same retry contract SYS_THREAD_JOIN
 * has and for the same reason: a woken task resumes with only RAX. */
static int oepoll_wait(int epfd, struct epoll_event *ev, int maxev, int timeout_ms) {
    return (int)(i64)sysc(SYS_EPOLL_WAIT, (u64)epfd, (u64)(void *)ev,
                          ((u64)(u32)timeout_ms << 32) | (u32)maxev);
}
static int oeventfd(u64 initval, int flags) {
    return (int)(i64)sysc(SYS_EVENTFD, initval, (u64)flags, 0);
}
static i64 oeventfd_write(int fd, u64 v) {
    return (i64)sysc(SYS_WRITE_FILE, (u64)fd, (u64)(void *)&v, 8);
}
static i64 oeventfd_read(int fd, u64 *out) {
    return (i64)sysc(SYS_READ, (u64)fd, (u64)(void *)out, 8);
}
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((u64)-1)
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

/* v0.77: the fault-injection sites write through a deliberately bad pointer to
 * prove the kernel reclaims resources on the FAULT exit path as well as on
 * SYS_EXIT. Written as `*(volatile u32 *)0x1 = ...` the optimiser sees the
 * constant and emits -Warray-bounds ("subscript 0 is outside array bounds"),
 * which is a true observation about code whose entire purpose is to be invalid.
 *
 * Laundering the address through an empty asm makes it opaque to the optimiser
 * WITHOUT changing what executes: the same store to the same address, still
 * faulting. Suppressing the diagnostic with a pragma would have silenced the
 * category everywhere, including somewhere it might one day be right. */
static inline volatile u32 *fault_ptr(u64 a) {
    __asm__ volatile("" : "+r"(a));
    return (volatile u32 *)a;
}

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
 * comes back wrong and we exit 999 — which the kernel-side suite FAILs on.
 *
 * v0.81: THIS ONE STAYS SHORT, AND IT IS NOT ONLY cmd_mcq's.
 *
 * cmd_mcpre spawns its HIGH-PRIORITY thread with role 7 as well, and asserts
 * that it finishes BEFORE the long victim it preempted. Lengthening this
 * function to fix cmd_mcq therefore broke cmd_mcpre: measured 4 failures of
 * "completion order inverted" in 8 -smp 4 boots against 0 in 7 on the same
 * tree without the change. Two suites, one payload, opposite requirements —
 * so cmd_mcq got its own probe (role 52) instead. Keep this short. */
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

/* role 52: v0.81 RESIDENT concurrent scheduling probe — cmd_mcq's probe, and
 * only cmd_mcq's.
 *
 * Identical to role 7 except that its residency is A DEADLINE RATHER THAN AN
 * ITERATION COUNT, which is what CLAUDE.md requires of timing in this tree:
 * QEMU here is TCG-only, so a fixed iteration count means a different duration
 * on every host and at every -smp width.
 *
 * That was the direct cause of the two-cpu concurrency failure. Under
 * round-robin TCG only one vCPU executes at a time, and a whole 3M-iteration
 * probe fits inside a single vCPU quantum — so cpu1 could enter ring 3, finish,
 * and exit before the BSP was scheduled back to enter at all. The logs showed
 * exactly that: cpu1's probe at finish#2, the BSP's at finish#3, high-water 1.
 * A tick target cannot be swallowed by a quantum, because g_ticks advances on
 * timer interrupts — as real time passes, not as instructions retire. Whichever
 * core arrives second still finds the first one there.
 *
 * The iteration ceiling is a BACKSTOP, not the budget. If SYS_SYSINFO ever
 * failed, osysticks() returns 0 and the unsigned subtraction below would end
 * the loop immediately; if instead ticks stopped advancing, an unbounded loop
 * here would spin forever in ring 3 — and because the BSP runs its own probe
 * through cpu_exec_proc(), which does not return until that probe exits, that
 * would wedge the machine rather than fail an assertion. Invariant 4 forbids
 * that trade, so this loop always terminates on its own. */
#define MCQ_T_RESIDENT     10u          /* 0.1 s of ring-3 residency per probe  */
#define MCQ_I_CEILING  50000000ull      /* backstop only; the tick target wins  */

static u32 osysticks(void);             /* defined below, with the wait helpers */

static void mcq_resident_probe(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);
    u32 t0 = osysticks();
    volatile u64 acc = 0;
    for (u64 i = 0; i < MCQ_I_CEILING; i++) {
        acc += i ^ pid;
        /* Sample the clock on the same stride as the identity re-check, so the
         * probe does not spend its residency inside SYS_SYSINFO. */
        if ((i & 0xFFFFull) == 0) {
            if (sysc(SYS_GETPID, 0, 0, 0) != pid)
                sysc(SYS_EXIT, 999, 0, 0);   /* cross-core identity bleed */
            if (osysticks() - t0 >= MCQ_T_RESIDENT) break;
        }
    }
    sysc(SYS_EXIT, pid, 0, 0);
}

/* role 8: v0.39 long-running PREEMPTIBLE probe (Stage 3). Same identity fuzz,
 * but long enough for another core to preempt it mid-loop, requeue its captured
 * context on a DIFFERENT cpu, and resume it there. If the capture/resume or the
 * migration corrupted anything — registers, stack, identity — the checksum loop
 * or the pid check breaks and the exit code betrays it.
 *
 * v0.81: A DEADLINE, LIKE EVERYTHING ELSE THAT MEASURES TIME HERE.
 *
 * This was 30,000,000 iterations — the last fixed-iteration budget in the
 * concurrency and preemption suites, and the same pattern CLAUDE.md rules out
 * for this TCG-only tree, where an iteration count means a different duration
 * on every host and at every -smp width.
 *
 * THE BUDGET IS NOT ARBITRARY: it must stay comfortably LONGER than the
 * high-priority thread that preempts it. cmd_mcpre queues that thread (role 7,
 * a short ~3M-iteration probe) once this one is mid-loop, and then asserts the
 * completion order INVERTED — hi finishing before this one. If this probe ever
 * became the shorter of the two, that assertion fails, which is exactly how
 * v0.81 broke the suite once already by lengthening role 7 underneath it. The
 * deadline below is roughly an order of magnitude beyond role 7's runtime, and
 * the ordering is verified by boot, not by arithmetic.
 *
 * The iteration ceiling is a backstop against ticks not advancing, never the
 * budget. It is also deliberately generous: if a host is fast enough to reach
 * it before the deadline, the probe is merely shorter than intended — still far
 * longer than role 7, so the ordering the suite asserts is preserved either
 * way. */
#define MCPRE_T_LONG        40u          /* 0.4 s of preemptible residency      */
#define MCPRE_I_CEILING 100000000ull     /* backstop only; the deadline wins    */

static void mcpre_long(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);
    u32 t0 = osysticks();
    volatile u64 acc = 0;
    for (u64 i = 0; i < MCPRE_I_CEILING; i++) {
        acc += i ^ pid;
        if ((i & 0x3FFFFull) == 0) {
            if (sysc(SYS_GETPID, 0, 0, 0) != pid)
                sysc(SYS_EXIT, 999, 0, 0);
            if (osysticks() - t0 >= MCPRE_T_LONG) break;
        }
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
        /* v0.83: REWIND BEFORE READING BACK. The write above advanced this
         * descriptor's position, so the read would otherwise start at EOF and
         * return nothing. That is POSIX, and it is new here: until writes became
         * positional they left the cursor at 0 and a read-back "just worked" —
         * this loop was relying on that, and every suite that spawns role 9
         * (cio, smpstrs, dmastrs, kpstrs) failed identically with exit 720 the
         * moment it stopped being true. */
        if ((i64)sysc(SYS_LSEEK, (u64)fd, 0, (u64)SEEK_SET) != 0)
                                                           sysc(SYS_EXIT, 715 + (u64)r, 0, 0);
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
        /* Same rewind, same reason — and here it matters for what the round is
         * actually testing: the read must see ONE writer's whole image, so it
         * has to start at byte 0 of that image and not wherever this process's
         * own write happened to leave the cursor. */
        if ((i64)sysc(SYS_LSEEK, (u64)sfd, 0, (u64)SEEK_SET) != 0)
                                                           sysc(SYS_EXIT, 755 + (u64)r, 0, 0);
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
    /* v0.83: rewind before reading back. tmp writes became POSITIONAL in this
     * release, so the write above left this descriptor at byte 8 and the read
     * would otherwise start at EOF — the same correction role 9 needed on the
     * root volume when writes there stopped resetting to zero. */
    if ((i64)sysc(SYS_LSEEK, (u64)tfd, 0, (u64)SEEK_SET) != 0) sysc(SYS_EXIT, 1012, 0, 0);
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
        volatile u32 *bad = fault_ptr(0x1);
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
        volatile u32 *bad = fault_ptr(0x1);
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
        volatile u32 *bad = fault_ptr(0x1);
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
        volatile u32 *bad = fault_ptr(0x1);
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

    /* v0.70: declare widgets from real ring 3, with a real user-mapped label.
     * wimpstrs' kernel half cannot pass one — a .rodata pointer in kernel
     * context is correctly refused by access_ok — so this is the only place
     * the string path is exercised at all. Any failure exits with a code the
     * suite will not mistake for success. */
    {
        i64 dims = (i64)sysc(SYS_WIN_INFO, (u64)ids[0], 0, 0);
        if (dims < 0) sysc(SYS_EXIT, 1420, 0, 0);
        int wcw = (int)((u64)dims >> 16), wch = (int)((u64)dims & 0xFFFF);
        i64 b = oui_add((int)ids[0], WG_BUTTON, 8, 8, 90, 20, "OK");
        if (b < 0) sysc(SYS_EXIT, 1421, 0, 0);
        i64 pr = oui_add((int)ids[0], WG_PROGRESS, 8, 34, 90, 12, 0);
        if (pr < 0) sysc(SYS_EXIT, 1422, 0, 0);
        /* A widget that runs off the bottom of the content rect is refused
         * here exactly as it is in the kernel-side suite — same check, but
         * reached through the real syscall boundary from ring 3. */
        if (oui_add((int)ids[0], WG_BUTTON, 8, wch, 90, 20, "NO") >= 0)
            sysc(SYS_EXIT, 1423, 0, 0);
        if (wcw <= 0 || wch <= 0) sysc(SYS_EXIT, 1424, 0, 0);
        /* A value written from ring 3 reads back through the accessor — this
         * is the whole contract of a polled toolkit. */
        if (oui_set((int)ids[0], (int)pr, 0, 60) < 0) sysc(SYS_EXIT, 1425, 0, 0);
        if (oui_get((int)ids[0], (int)pr, 0) != 60)   sysc(SYS_EXIT, 1426, 0, 0);
        if (oui_set((int)ids[0], (int)b, 2, (u64)"GO") < 0) sysc(SYS_EXIT, 1427, 0, 0);
        /* Disabling and re-enabling must both be observable, or SYS_UI_SET
         * could be writing nothing and the read-back would still agree. */
        if (oui_set((int)ids[0], (int)b, 1, 0) < 0)  sysc(SYS_EXIT, 1428, 0, 0);
        if (oui_get((int)ids[0], (int)b, 1) != 0)    sysc(SYS_EXIT, 1429, 0, 0);
        if (oui_set((int)ids[0], (int)b, 1, 1) < 0)  sysc(SYS_EXIT, 1430, 0, 0);
        if (oui_get((int)ids[0], (int)b, 1) != 1)    sysc(SYS_EXIT, 1431, 0, 0);
        /* An id this window never declared is not readable through it. The
         * probe steps aside from our own two ids rather than assuming a
         * particular slot is free — the widget table is global and other
         * processes are declaring into it concurrently. */
        int probe = 31;
        if (probe == (int)b || probe == (int)pr) probe = 29;
        if (oui_get((int)ids[0], probe, 0) >= 0)     sysc(SYS_EXIT, 1432, 0, 0);

        /* v0.71: a text field, and the text read back out through the real
         * syscall into a real user buffer. The kernel-side suite cannot do
         * this half — it has no user-mapped destination to copy into — so
         * without it SYS_UI_GET(what=2) would ship unexercised. */
        i64 e = oui_add((int)ids[0], WG_ENTRY, 8, 52, 100, 18, "abc");
        if (e < 0) sysc(SYS_EXIT, 1433, 0, 0);
        char tb[WG_TEXTLEN];
        for (int k = 0; k < WG_TEXTLEN; k++) tb[k] = 0x5A;   /* poison, so a
                                                              * no-op copy is
                                                              * detectable */
        i64 n = oui_gettext((int)ids[0], (int)e, tb);
        if (n != 3) sysc(SYS_EXIT, 1434, 0, 0);
        if (tb[0] != 'a' || tb[1] != 'b' || tb[2] != 'c' || tb[3] != 0)
            sysc(SYS_EXIT, 1435, 0, 0);
        /* Relabelling through SYS_UI_SET must be visible to the same reader. */
        if (oui_set((int)ids[0], (int)e, 2, (u64)"zz") < 0) sysc(SYS_EXIT, 1436, 0, 0);
        if (oui_gettext((int)ids[0], (int)e, tb) != 2 || tb[0] != 'z' || tb[2] != 0)
            sysc(SYS_EXIT, 1437, 0, 0);
        /* A NULL destination is refused rather than written through. */
        if ((i64)sysc(SYS_UI_GET, ((u64)ids[0] & 0xFFFF) | ((u64)(int)e << 16), 2, 0) >= 0)
            sysc(SYS_EXIT, 1438, 0, 0);
    }

    if (fault_after_create) {
        volatile u32 *bad = fault_ptr(0x1);
        *bad = 0xDEAD;                                       /* deliberate fault: windows AND their widgets must still be released */
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

/* v0.76 carryover 2: A SPIN COUNT IS NOT A TIMEOUT.
 *
 * owaitpid() above gives up after a fixed number of poll-and-yield iterations.
 * That number means completely different amounts of REAL TIME depending on how
 * fast the waiter gets to spin relative to the child's progress — and under TCG
 * those two rates are unrelated. With -smp 4 the waiter sits on its own vCPU
 * spinning quickly while the child crawls, four vCPUs multiplexed onto however
 * many host cores QEMU actually has, so the budget burns down at a rate that
 * has nothing to do with whether the child is nearly finished.
 *
 * Measured, not theorised: `make gate-dirty-smp` failed on its first boot in
 * 2 of 2 runs with "the compiler TIMED OUT building omake" (exit 984) — while
 * /bin/omake was 36562 bytes, well-formed, and the assertion that occ built it
 * PASSED in the same log. The compile finished; the waiter had already given up.
 *
 * So wait on the CLOCK instead. SYS_SYSINFO reports g_ticks (100 Hz), which
 * advances in real time regardless of how many vCPUs are contending, so a
 * budget expressed in ticks means the same thing at 1 vCPU and at 4. That is
 * the whole point — it needs no scaling factor per core count, because a
 * deadline is already invariant to the waiter's spin rate.
 *
 * owaitpid() itself is left alone: every other caller's spin budget keeps its
 * current behaviour rather than being silently reinterpreted into a new unit. */
#define TICKS_PER_SEC 100u

static u32 osysticks(void) {
    /* 24-byte header + up to 12 x 32-byte entries. */
    static u32 si[104 / 4 + 12 * 8];
    /* v0.77: the cast is NOT cosmetic. sysc() returns u64, so `< 0` was always
     * false and this error check could never fire — a guard that cannot fail is
     * the same class as a counter nothing prints. Now a failed SYS_SYSINFO
     * really does return 0 ticks, which is what every caller already assumed. */
    if ((i64)sysc(SYS_SYSINFO, (u64)si, 0, 0) < 0) return 0;
    return si[5];                       /* hdr: ncpu nproc used free ram TICKS */
}

static u32 osysncpu(void) {
    static u32 si[104 / 4 + 12 * 8];
    if ((i64)sysc(SYS_SYSINFO, (u64)si, 0, 0) < 0) return 1;   /* v0.77: see osysticks */
    return si[0] ? si[0] : 1;
}

/* Wait for `pid` until it exits or `budget` ticks of REAL TIME have passed.
 * Returns the exit code, or -11 on deadline. If `spent` is non-null it receives
 * the ticks actually consumed — so a log can say how long a stage really took
 * rather than only that it fitted, which is what makes the next budget an
 * observation instead of another guess. */
/* Budgets in TICKS (100 Hz), shared by every driver that waits on a child.
 * Ceilings for a pathological host, not expectations — measured figures on this
 * host are 1.2 s for a small compile and 2.8 s for the largest one, so these are
 * roughly 70x margin. They live here, next to the waiter, so there is one source
 * of truth rather than a constant per suite. */
#define WAIT_T_COMPILE 20000u      /* 200 s: one occ invocation                */
#define WAIT_T_TOOL    20000u      /* 200 s: a tool that forks occ again       */
#define WAIT_T_RUN      6000u      /*  60 s: running a produced program        */

/* v0.77: joining a thread. 20 s, deliberately BELOW role 31's 3000-tick (30 s)
 * posix_drain watchdog so the inner deadline fires first and the log names the
 * assertion instead of reporting "not every task reached a terminal state".
 *
 * READ THIS BEFORE TRUSTING IT. Unlike SYS_WAITPID, SYS_THREAD_JOIN *parks* the
 * caller (block_ring3, FUTEX_DEFAULT_TICKS = 20000 ticks = 200 s). A userland
 * deadline can only be checked BETWEEN syscalls, so it bounds the retry loop —
 * the "woken, still not done, ask again" path — and it does NOT bound a single
 * park. If a wake is genuinely lost, the first call blocks for the kernel's
 * 200 s and this budget never gets to fire. Making that case fast needs a
 * timeout argument to SYS_THREAD_JOIN, which the ABI does not have; see the
 * v0.77 notes. Claiming otherwise would be the same error as budgeting a
 * waiter's spins and calling it a timeout. */
#define WAIT_T_JOIN     2000u      /*  20 s: one thread join, retries included */

/* v0.78: role 29's fork/waitpid wait — the LAST spin budget in this file, and
 * the one carryover 3 is actually about. It was `owaitpid(child, 30000)`, and
 * the v0.75 fork-race account names its expiry as the trigger for both observed
 * symptoms: the parent gives up (702), exits, its slot is recycled, and only
 * then does the child ask who its parent is (44).
 *
 * A spin count made that budget mean different durations at 1 vCPU and at 4,
 * which is exactly why the failure looked like a race sensitive to host speed
 * and binary layout. 2000 ticks sits below role 29's own 3000-tick posix_drain
 * watchdog so the inner deadline fires first and the log names an assertion
 * rather than reporting that the round never finished. */
#ifdef FORK_TIGHT_DEADLINE
/* v0.78 REPRODUCER, NEVER shipped. Build with
 *   make clean && make EXTRA=-DFORK_FUNNEL_REPRO UEXTRA=-DFORK_TIGHT_DEADLINE
 *
 * 20 ticks sits deliberately BETWEEN the two measured populations: a fixed
 * kernel dispatches a forked child in 1-8 ticks even at 1.5x host
 * oversubscription, while the funnel reproducer takes 52-94. A deadline in the
 * gap fires for one and not the other, which is what makes the pair a
 * controlled experiment instead of two anecdotes.
 *
 * This is how the v0.74 symptom is reached on a host that is otherwise too fast
 * to show it: not by making the machine slower, but by moving the budget down
 * to where the machine already is. */
#define WAIT_T_FORK       20u      /* 0.2 s: REPRODUCER ONLY                   */
#else
#define WAIT_T_FORK     2000u      /*  20 s: fork -> child exit                */
#endif

static i64 owaitpid_ticks(u32 pid, u32 budget, u32 *spent) {
    u32 t0 = osysticks(), n = 0;
    for (;;) {
        i64 r = owaitpid_poll(pid);
        if (r != -11) { if (spent) *spent = osysticks() - t0; return r; }
        /* Sample the clock every 256 polls, not every poll: SYS_SYSINFO walks
         * the process table, and checking it as often as we check the child
         * would make the waiter the expensive half of the wait. */
        if ((++n & 255u) == 0 && (osysticks() - t0) >= budget) {
            if (spent) *spent = osysticks() - t0;
            return -11;
        }
        oyield();
    }
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
/* v0.83: O_TRUNC. Needed because SYS_WRITE_FILE became positional and
 * tail-preserving in the same release — a write no longer replaces a file's
 * whole contents, so "author this file" has to say so at open. */
#define O_TRUNC 2
/* v0.84: O_APPEND. Every write through this descriptor lands at end-of-file,
 * whatever the descriptor's offset says — and the kernel resolves that end
 * inside the same lock that performs the write, so two appenders cannot both
 * choose the same place. An lseek before the write is not merely unnecessary,
 * it is IGNORED, which is the property the suite checks rather than assumes. */
#define O_APPEND 4
static int oopen_flags(const char *path, u64 flags) {
    i64 k = (i64)sysc(SYS_OPEN, (u64)path, flags, 0);
    if (k < 0) return (int)k;
    for (int i = 3; i < OFD_MAX; i++) if (g_ofd[i] == -1) { g_ofd[i] = (int)k; return i; }
    sysc(SYS_CLOSE, (u64)k, 0, 0);
    return -24;                                          /* EMFILE */
}
static int oopen(const char *path)  { return oopen_flags(path, 0); }
/* v0.83: ocreat() now truncates, which RESTORES its previous effective
 * behaviour rather than changing it. It always meant "author this file", and it
 * got that for free while a write replaced the whole file. Now that writes are
 * positional and preserve the tail, the truncation has to be asked for, or
 * re-authoring a path with shorter content would leave the old tail behind.
 * Callers that want to write INTO an existing file without emptying it want
 * oopen() plus olseek(), which is the POSIX split. */
static int ocreat(const char *path) { return oopen_flags(path, O_CREAT | O_TRUNC); }
/* Create without emptying an existing file — the old ocreat() semantics, for a
 * caller that means "ensure it exists" rather than "author it". Nothing needs
 * it today; it is kept because O_CREAT-without-O_TRUNC is a real POSIX mode and
 * the alternative is the next caller re-deriving the flag word by hand. */
__attribute__((unused))
static int ocreat_keep(const char *path) { return oopen_flags(path, O_CREAT); }
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
/* v0.76: ring 3 had no way to print a number, which is why every timing figure
 * in this file was previously reported only as an exit code. A measurement you
 * cannot print is a measurement nobody acts on. */
static void oputu(u64 v) {
    char b[24]; int i = 24;
    b[--i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + (int)(v % 10)); v /= 10; }
    owrite(STDOUT_FILENO, &b[i], ostrlen(&b[i]));
}
static i64 oread(int fd, char *buf, u64 n) {
    if (fd < 0 || fd >= OFD_MAX || g_ofd[fd] == -1) return -9;
    if (g_ofd[fd] == OFD_CONSOLE) return 0;              /* no ring-3 tty input yet */
    return (i64)sysc(SYS_READ, (u64)g_ofd[fd], (u64)buf, n);
}
/* The userland table maps its own indices onto kernel descriptors; mmap needs
 * the kernel one, since the kernel has never heard of the userland table. */
static int okfd(int fd) {
    if (fd < 0 || fd >= OFD_MAX) return -1;
    return g_ofd[fd];
}

/* v0.82: move a descriptor's file position. Returns the new absolute offset, or
 * a negative errno. The console is refused here rather than passed down: fd 0/1/2
 * map to OFD_CONSOLE, which is not a kernel descriptor at all, so handing it to
 * the kernel would have it interpret the sentinel as an fd number. */
static i64 olseek(int fd, i64 off, int whence) {
    if (fd < 0 || fd >= OFD_MAX || g_ofd[fd] == -1) return -9;      /* EBADF  */
    if (g_ofd[fd] == OFD_CONSOLE)                   return -29;     /* ESPIPE */
    return (i64)sysc(SYS_LSEEK, (u64)g_ofd[fd], (u64)off, (u64)(i64)whence);
}
/* v0.84: set a file's length through an open descriptor. Returns 0, or a
 * negative errno. The console is refused here rather than passed down for the
 * same reason olseek refuses it: fd 0/1/2 map to OFD_CONSOLE, which is not a
 * kernel descriptor, so handing it over would have the kernel read the sentinel
 * as an fd number. */
static i64 oftruncate(int fd, i64 len) {
    if (fd < 0 || fd >= OFD_MAX || g_ofd[fd] == -1) return -9;      /* EBADF  */
    if (g_ofd[fd] == OFD_CONSOLE)                   return -22;     /* EINVAL */
    return (i64)sysc(SYS_FTRUNCATE, (u64)g_ofd[fd], (u64)len, 0);
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

/* v0.63: allocations this large go straight to the kernel instead of through
 * the heap. Two reasons, and the second is the one that matters: a multi-
 * hundred-KiB block carved out of a first-fit arena leaves a hole almost
 * nothing can reuse, and — because mmap is demand-zero — a big request that
 * is only partly touched never costs the frames it did not use. Freeing one
 * returns its address space outright rather than parking it on a free list. */
#define OMMAP_MIN (128u * 1024u)
#define OMMAP_MAGIC 0x4D4D4150ull                  /* "MMAP" */
struct ommap_hdr { u64 magic; u64 len; };

static void *omalloc(u64 n) {
    if (!n) return 0;
    n = (n + 15) & ~15ull;                        /* 16-byte payload alignment   */
    if (n >= OMMAP_MIN) {
        u64 total = n + sizeof(struct ommap_hdr);
        u64 r = sysc(SYS_MMAP, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        if (r == MAP_FAILED || (i64)r < 0) return 0;
        /* The header is what lets ofree tell an mmap block from a heap block
         * without a side table — and the magic is what stops it mistaking a
         * heap block's bytes for one. */
        struct ommap_hdr *h = (struct ommap_hdr *)r;
        h->magic = OMMAP_MAGIC;
        h->len = total;
        return (void *)(r + sizeof(struct ommap_hdr));
    }
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
    /* v0.63: an mmap block carries its own header and is released to the
     * kernel outright. Checked FIRST and by magic, because the alternative —
     * treating every pointer as a heap block — would read a size field out of
     * whatever happens to precede an mmap payload. */
    {
        struct ommap_hdr *h = (struct ommap_hdr *)((u8 *)q - sizeof(struct ommap_hdr));
        if (h->magic == OMMAP_MAGIC) {
            u64 len = h->len;
            h->magic = 0;                       /* poison: catch a double free  */
            sysc(SYS_MUNMAP, (u64)(void *)h, len, 0);
            return;
        }
    }
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

/* v0.77: part of the ring-3 heap API and kept deliberately; no driver happens
 * to call it today. Marked rather than deleted — removing a working allocator
 * to quiet a warning trades a real capability for a cosmetic one. */
__attribute__((unused))
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
 * bounded so a join can FAIL rather than hang.
 *
 * v0.77: the bound is now a DEADLINE. It was `for (k = 0; k < 20000; k++)`,
 * which counts retries — and a retry here is a park/wake cycle of unknown
 * duration, so the constant described no amount of time at all. Same class as
 * the v0.76 owaitpid() defect, though NOT the same mechanism, which is worth
 * being exact about because the v0.76 changelog first got it wrong: this
 * syscall blocks, so 20000 retries was never a fast spin.
 *
 * What was actually measured (gate-dirty-smp, boot 2, preserved log
 * OUTRUN-0.76-gate-dirty-smp-boot2-pthreads_smp.log): the posixstrs breadcrumb
 * reached +19802 ticks and the driver then exited — the kernel's own park
 * deadline (FUTEX_DEFAULT_TICKS, 20000 ticks) expiring in a SINGLE call, which
 * returned -ETIMEDOUT. The retry loop never went round twice. That boot took
 * 425 s against 260 s and 220 s for its siblings, which is the same 200 s
 * seen from outside.
 *
 * So the deadline below is the honest bound on the RETRY path, and the return
 * value is what actually fixes the observed symptom: a timeout now comes back
 * as ETIMEDOUT_NEG distinctly, instead of being handed to a caller that cannot
 * tell it from a broken join. */
static int pthread_join(pthread_t t, void **ret) {
    if (t < 0 || t >= PTHREAD_MAX) return -1;
    u64 code = 0;
    u32 t0 = osysticks(), n = 0;
    for (;;) {
        i64 r = (i64)sysc(SYS_THREAD_JOIN, (u64)t, (u64)(void *)&code, 0);
        if (r == ETIMEDOUT_NEG) return ETIMEDOUT_NEG;   /* the kernel's own park deadline */
        if (r == EAGAIN_NEG) {
            /* Sample the clock every 256 retries, not every retry: SYS_SYSINFO
             * walks the process table. Same reasoning as owaitpid_ticks(). */
            if ((++n & 255u) == 0 && (osysticks() - t0) >= WAIT_T_JOIN) return ETIMEDOUT_NEG;
            /* A wake that finds the thread still running returns immediately,
             * so without this a wake storm would busy-spin through the syscall.
             * The common case is parked inside the call and never reaches here. */
            oyield();
            continue;
        }
        if (r != 0) return (int)r;
        if (ret) *ret = g_pthr[t].ret;             /* the value, not the code */
        g_pthr[t].state = 3;                       /* joined; slot NOT recycled */
        return 0;
    }
}

static pthread_t pthread_self(void) { return (pthread_t)sysc(SYS_GETTID, 0, 0, 0); }

/* v0.77: the pthread shim is an API surface, not a call graph. Workers return
 * normally today, so nothing calls this — that is not a reason to delete it. */
__attribute__((unused))
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
/* v0.77: converted with pthread_join, and for the same reason. Converting one
 * of two joins that call the same syscall in the same shape is how v0.76 left
 * toolstrs and pipestrs behind when it fixed langstrs — the idiom is the defect,
 * not the call site. */
static int kthread_join(int tid, u64 *code) {
    u32 t0 = osysticks(), n = 0;
    for (;;) {
        i64 r = (i64)sysc(SYS_THREAD_JOIN, (u64)tid, (u64)(void *)code, 0);
        if (r == ETIMEDOUT_NEG) return ETIMEDOUT_NEG;
        if (r == EAGAIN_NEG) {
            if ((++n & 255u) == 0 && (osysticks() - t0) >= WAIT_T_JOIN) return ETIMEDOUT_NEG;
            oyield();
            continue;
        }
        return (int)r;
    }
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
#ifdef FORK_TIGHT_DEADLINE
    oputs("  [posix ] *** FORK_TIGHT_DEADLINE build: waitpid budget is 20 ticks, NOT 2000\n");
#endif
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
    /* v0.78: a real-time deadline, and it REPORTS ITSELF. The old line waited a
     * spin count and the only thing the log ever learned was the exit code —
     * so "the child never ran" and "I stopped asking too early" arrived as the
     * same number, on a budget that meant different durations per core count. */
    u32 spent = 0;
    i64 code = owaitpid_ticks(child, WAIT_T_FORK, &spent);
    oputs("  [posix ] waitpid on the forked child returned after ");
    oputu(spent);
    oputs(" tick(s)\n");
    if (code == -11) sysc(SYS_EXIT, 702, 0, 0);       /* DEADLINE: gave up waiting */
    if (code != 42)  sysc(SYS_EXIT, 703, 0, 0);       /* ran, but wrong exit status */
    /* SIGCHLD is posted by the kernel when the child's space is reclaimed; give
     * the delivery boundary a few syscalls to hand it to our handler.        */
    for (int i = 0; i < 64 && !g_chld_hits; i++) oyield();
    if (!g_chld_hits) sysc(SYS_EXIT, 704, 0, 0);
    sysc(SYS_EXIT, 700, 0, 0);
}

/* --- role 53: v0.77 carryover 3 — THE CROSS-GENERATION ORPHAN --------------
 *
 * Numbered 52 when it was written against v0.77; v0.81 gave 52 to
 * mcq_resident_probe, so it dispatches at 53 (see PX_ORPH_ROLE in kernel64.c,
 * which carries the same rebase note). This heading said 52 until v0.84 — the
 * exact hazard CLAUDE.md names, a comment whose subject nothing checks.
 *
 * What role 29 above cannot test, and why this exists.
 *
 * v0.75 defect B was that ppid_live() resolved a parent link by testing the
 * parent slot's `used` flag alone. Slots are recycled, so `used` is also true
 * when the slot has been handed to a completely different process — and
 * getppid() then returned a stranger's pid. The fix pins the link to a (slot,
 * generation) pair.
 *
 * Role 29's child calls getppid() immediately after fork(), while its parent is
 * still sitting in waitpid(). The parent therefore cannot have exited, its slot
 * cannot have been recycled, and the generation compare has nothing to
 * disagree with. Built with -DFORK_RACE_REPRO — which reverts that compare and
 * nothing else — a full boot reported ZERO stale resolutions and posixstrs
 * passed 12/12. That is not evidence the fix works. It is evidence the suite
 * never asked: exit 44 was unreachable in that shape at any core count.
 *
 * So this role constructs the shape the defect needs, deliberately:
 *
 *     W ──fork──> P ──fork──> C      C's parent link names P's SLOT
 *     │           (exits at once)
 *     └──fork──> throwaway           which lands on P's vacated slot and
 *                                    bumps that slot's generation, while C
 *                                    is still pointing at it
 *
 * WHAT THIS FOUND, on its first run. Not a confirmation: the normal build —
 * generation compare compiled in, no -DFORK_RACE_REPRO — failed exactly as the
 * reverted build did, C reading its parent as pid 647 when its parent had been
 * pid 645. v0.75 defect B was still live in the shipped kernel, because
 * kproc_reset's v0.72 cmemset backstop zeroed `gen` before the `p->gen++` at
 * the bottom of the same function, pinning every slot's generation at 1 and
 * making the compare 1 == 1 forever. See kproc_reset() in kernel64.c. The fix
 * is there; this role is what could see it.
 *
 * The recycle is ARRANGED, not waited for. kproc_spawn hands out the LOWEST
 * torn-down slot, so W gets a lower slot than P and P a lower one than C. Once
 * P has exited — proven, because W's waitpid() on it returned — P's slot is the
 * lowest torn-down slot in the table: W's own is below it and still alive, and
 * everything below W's was alive when W was allocated. W's next fork therefore
 * takes P's slot by construction, bumping its generation while C still points
 * at it. No timing window, no polling for luck.
 *
 * C then asks getppid() what its parent is. There are exactly three answers:
 *
 *   0            correct. The link was pinned to a generation that no longer
 *                matches, so C reads as the orphan it is. Also the POSITIVE
 *                CONTROL: seeing it proves the recycle really happened, so a
 *                pass cannot be a test that quietly did nothing.
 *   a stranger   DEFECT B. Only reachable when the generation compare is gone.
 *   P's pid      not yet recycled; keep looking until the deadline.
 *
 * C learns P's identity from INHERITED MEMORY, not from getppid(): the call
 * under test must not also be the source of ground truth.                    */
#define ORPH_CHURN    4u      /* forks W makes to drive the recycle; 1 suffices */
#define ORPH_T_STEP  600u     /* 6 s: ceiling on any single wait in this role   */
#define ORPH_T_WATCH 900u     /* 9 s: C's deadline for observing the recycle    */

/* Written by P before it forks C; C inherits the page and reads it there. */
static volatile u32 g_orph_parent_pid = 0;

static void posix_orphan_child(void) {
    u32 p0 = g_orph_parent_pid;
    if (!p0) sysc(SYS_EXIT, 1750, 0, 0);         /* fork did not carry the page */
    u32 t0 = osysticks();
    for (;;) {
        u32 pp = ogetppid();
        if (pp == 0)  sysc(SYS_EXIT, 42, 0, 0);  /* orphaned, correctly         */
        /* The stranger's pid travels out in the exit code. "getppid() returned
         * the wrong thing" is not a finding; WHICH wrong thing it returned is
         * what separates a kernel defect from a defective test. */
        if (pp != p0) sysc(SYS_EXIT, 2000000 + (u64)pp, 0, 0);   /* A STRANGER  */
        /* A deadline, not a spin count: this role is meant to run at 1 vCPU and
         * at 4, and an iteration budget means different durations at each. */
        if (osysticks() - t0 >= ORPH_T_WATCH) sysc(SYS_EXIT, 47, 0, 0);  /* INCONCLUSIVE */
        oyield();
    }
}

/* role 55: v0.82 SYS_LSEEK — the file position, exercised from ring 3.
 *
 * The position always existed (struct ofile carries `off`, and every read
 * advances it); what did not exist was any way to MOVE it. So a ring-3 program
 * could read a file forwards exactly once and could never rewind to re-read a
 * header, skip a section, or size a file without reading all of it. This worker
 * is the test for closing that.
 *
 * The REWIND is the load-bearing case. Everything else here checks arithmetic
 * and error returns, but step (3) is the one that was impossible before: read
 * some bytes, seek back to 0, and get the SAME bytes again. If lseek moved the
 * offset but reads ignored it, every arithmetic check below would still pass
 * and only the re-read would fail — which is why the re-read is compared byte
 * for byte rather than merely counted.
 *
 * The error cases are checked with the same weight as the successes, and each
 * one re-reads the position afterwards: a refused seek that still moved the
 * offset is a defect even though it returned the right errno, and nothing in
 * the return value alone would show it.
 *
 * Exit 42 on full success; each failure point has its own code. */
#define LS_PATH "/lseek-probe"
#define LS_N    26

/* v0.84: 10 x 4 KiB = 40 KiB, comfortably past the old 32 KiB staging ceiling
 * and well under VFS_MAX_FILE_BYTES. The buffer is file-scope rather than on
 * the stack because a ring-3 worker's stack is not the place for 4 KiB. */
#define LSB_CHUNK 4096
#define LSB_N     10
#define LSB_TOTAL (LSB_CHUNK * LSB_N)     /* 40 KiB, past the old 32 KiB ceiling */
static u8 g_lsb[LSB_TOTAL];

/* Defined with the other VFS helpers further down; declared here because this
 * worker removes its own probe file. The dirty-volume gate reuses one image
 * across three boots, so a suite that leaves litter behind changes the state
 * the next boot starts from. */
static int ounlink(const char *path);

static void lseek_worker(void) {
    static const char alpha[LS_N] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char buf[LS_N];

    /* (1) Author a file of known content. */
    { int fd = ocreat(LS_PATH);
      if (fd < 0) sysc(SYS_EXIT, 1600, 0, 0);
      if (owrite(fd, alpha, LS_N) != LS_N) sysc(SYS_EXIT, 1601, 0, 0);
      oclose(fd); }

    int fd = oopen(LS_PATH);
    if (fd < 0) sysc(SYS_EXIT, 1602, 0, 0);

    /* (2) A fresh descriptor starts at 0, and SEEK_CUR+0 is how you ask where
     * you are without moving. */
    if (olseek(fd, 0, SEEK_CUR) != 0)          sysc(SYS_EXIT, 1603, 0, 0);
    if (oread(fd, buf, 5) != 5)                sysc(SYS_EXIT, 1604, 0, 0);
    if (buf[0] != 'A' || buf[4] != 'E')        sysc(SYS_EXIT, 1605, 0, 0);
    if (olseek(fd, 0, SEEK_CUR) != 5)          sysc(SYS_EXIT, 1606, 0, 0);  /* read advanced it */

    /* (3) THE REWIND — the operation that did not exist. Same bytes, twice. */
    if (olseek(fd, 0, SEEK_SET) != 0)          sysc(SYS_EXIT, 1607, 0, 0);
    for (int i = 0; i < LS_N; i++) buf[i] = 0;
    if (oread(fd, buf, 5) != 5)                sysc(SYS_EXIT, 1608, 0, 0);
    for (int i = 0; i < 5; i++)
        if (buf[i] != alpha[i])                sysc(SYS_EXIT, 1609, 0, 0);

    /* (4) SEEK_SET to an interior offset. */
    if (olseek(fd, 10, SEEK_SET) != 10)        sysc(SYS_EXIT, 1610, 0, 0);
    if (oread(fd, buf, 3) != 3)                sysc(SYS_EXIT, 1611, 0, 0);
    if (buf[0] != 'K' || buf[2] != 'M')        sysc(SYS_EXIT, 1612, 0, 0);

    /* (5) SEEK_CUR backwards. Position is 13 after the read above; -2 -> 11. */
    if (olseek(fd, -2, SEEK_CUR) != 11)        sysc(SYS_EXIT, 1613, 0, 0);
    if (oread(fd, buf, 1) != 1)                sysc(SYS_EXIT, 1614, 0, 0);
    if (buf[0] != 'L')                         sysc(SYS_EXIT, 1615, 0, 0);

    /* (6) SEEK_END. Offset 0 from the end is the file's length — which is how a
     * program sizes a file without reading it. */
    if (olseek(fd, 0, SEEK_END) != LS_N)       sysc(SYS_EXIT, 1616, 0, 0);
    if (oread(fd, buf, 1) != 0)                sysc(SYS_EXIT, 1617, 0, 0);  /* at EOF */
    if (olseek(fd, -4, SEEK_END) != LS_N - 4)  sysc(SYS_EXIT, 1618, 0, 0);
    if (oread(fd, buf, 4) != 4)                sysc(SYS_EXIT, 1619, 0, 0);
    if (buf[0] != 'W' || buf[3] != 'Z')        sysc(SYS_EXIT, 1620, 0, 0);

    /* (7) Seeking PAST the end is legal, and must not be mistaken for an error.
     * A read there returns 0 bytes rather than failing. */
    if (olseek(fd, 1000, SEEK_SET) != 1000)    sysc(SYS_EXIT, 1621, 0, 0);
    if (oread(fd, buf, 4) != 0)                sysc(SYS_EXIT, 1622, 0, 0);

    /* (8) REFUSALS. Each is checked for the right errno AND for having left the
     * position alone — a refusal that still moved the offset is a defect the
     * return value cannot show. */
    if (olseek(fd, 0, SEEK_SET) != 0)          sysc(SYS_EXIT, 1623, 0, 0);  /* known state */
    if (olseek(fd, -1, SEEK_SET) != -22)       sysc(SYS_EXIT, 1624, 0, 0);  /* EINVAL      */
    if (olseek(fd, 0, SEEK_CUR) != 0)          sysc(SYS_EXIT, 1625, 0, 0);  /* unmoved     */
    if (olseek(fd, -100, SEEK_CUR) != -22)     sysc(SYS_EXIT, 1626, 0, 0);
    if (olseek(fd, 0, SEEK_CUR) != 0)          sysc(SYS_EXIT, 1627, 0, 0);  /* unmoved     */
    if (olseek(fd, 0, 99) != -22)              sysc(SYS_EXIT, 1628, 0, 0);  /* bad whence  */
    if (olseek(fd, 0, SEEK_CUR) != 0)          sysc(SYS_EXIT, 1629, 0, 0);  /* unmoved     */

    /* (9) A descriptor that was never opened is EBADF, not a silent 0. */
    if (olseek(OFD_MAX - 1, 0, SEEK_SET) != -9) sysc(SYS_EXIT, 1630, 0, 0);

    /* (10) A PIPE has no position. POSIX says ESPIPE, and the kernel refuses
     * the whole non-seekable class rather than falling through to a dirent. */
    { i64 pv[2];
      if ((i64)sysc(SYS_PIPE, (u64)(void *)pv, 0, 0) != 0) sysc(SYS_EXIT, 1631, 0, 0);
      i64 pr = (i64)sysc(SYS_LSEEK, (u64)pv[0], 0, (u64)SEEK_SET);
      if (pr != -29)                           sysc(SYS_EXIT, 1632, 0, 0);  /* ESPIPE */
      sysc(SYS_CLOSE, (u64)pv[0], 0, 0);
      sysc(SYS_CLOSE, (u64)pv[1], 0, 0); }

    /* (11) The position is a property of the DESCRIPTION, so a fork's child
     * inherits where the parent had seeked to. */
    if (olseek(fd, 7, SEEK_SET) != 7)          sysc(SYS_EXIT, 1633, 0, 0);
    { i64 c = ofork();
      if (c < 0) sysc(SYS_EXIT, 1634, 0, 0);
      if (c == 0) {
          if (olseek(fd, 0, SEEK_CUR) != 7)    sysc(SYS_EXIT, 81, 0, 0);
          char cb[2];
          if (oread(fd, cb, 1) != 1)           sysc(SYS_EXIT, 82, 0, 0);
          if (cb[0] != 'H')                    sysc(SYS_EXIT, 83, 0, 0);
          sysc(SYS_EXIT, 80, 0, 0);
      }
      i64 st = owaitpid_ticks((u32)c, WAIT_T_FORK, 0);
      if (st == -11) sysc(SYS_EXIT, 1635, 0, 0);   /* DEADLINE, not a defect */
      if (st != 80)  sysc(SYS_EXIT, 1636, 0, 0); }

    oclose(fd);

    /* ==================================================================
     * v0.83: POSITIONAL WRITES. Same descriptor cursor, now driving writes.
     * ================================================================== */

    /* (12) MID-FILE OVERWRITE, tail preserved. The bytes at [10,13) become
     * "xyz" and everything after them must survive — that is the case the old
     * whole-file write could not express at all. */
    { int w = oopen(LS_PATH);
      if (w < 0) sysc(SYS_EXIT, 1640, 0, 0);
      if (olseek(w, 10, SEEK_SET) != 10)        sysc(SYS_EXIT, 1641, 0, 0);
      if (owrite(w, "xyz", 3) != 3)             sysc(SYS_EXIT, 1642, 0, 0);
      if (olseek(w, 0, SEEK_CUR) != 13)         sysc(SYS_EXIT, 1643, 0, 0);  /* cursor advanced */
      oclose(w); }
    { int v = oopen(LS_PATH);
      if (v < 0) sysc(SYS_EXIT, 1644, 0, 0);
      char c[LS_N + 8];
      i64 got = oread(v, c, sizeof c);
      if (got != LS_N)                          sysc(SYS_EXIT, 1645, 0, 0);  /* length UNCHANGED */
      if (c[9] != 'J' || c[10] != 'x' || c[11] != 'y' || c[12] != 'z')
                                                sysc(SYS_EXIT, 1646, 0, 0);  /* overwrote in place */
      if (c[13] != 'N' || c[LS_N - 1] != 'Z')   sysc(SYS_EXIT, 1647, 0, 0);  /* THE TAIL SURVIVED */
      oclose(v); }

    /* (13) SEQUENTIAL writes through ONE descriptor. Three calls with no seek
     * between them must land end to end, which is only true if each advanced
     * the cursor — before v0.83 each would have replaced the file. */
    { int w = ocreat("/lseek-seq");
      if (w < 0) sysc(SYS_EXIT, 1648, 0, 0);
      if (owrite(w, "aaa", 3) != 3)             sysc(SYS_EXIT, 1649, 0, 0);
      if (owrite(w, "bbb", 3) != 3)             sysc(SYS_EXIT, 1650, 0, 0);
      if (owrite(w, "ccc", 3) != 3)             sysc(SYS_EXIT, 1651, 0, 0);
      if (olseek(w, 0, SEEK_CUR) != 9)          sysc(SYS_EXIT, 1652, 0, 0);
      oclose(w); }
    { int v = oopen("/lseek-seq");
      if (v < 0) sysc(SYS_EXIT, 1653, 0, 0);
      char c[16];
      if (oread(v, c, sizeof c) != 9)           sysc(SYS_EXIT, 1654, 0, 0);
      if (c[0] != 'a' || c[3] != 'b' || c[6] != 'c' || c[8] != 'c')
                                                sysc(SYS_EXIT, 1655, 0, 0);
      oclose(v); }

    /* (14) SEEK PAST EOF, then write: the file EXTENDS and the hole reads as
     * zeroes. Writing at 12 in a 9-byte file leaves [9,12) as a gap. */
    { int w = oopen("/lseek-seq");
      if (w < 0) sysc(SYS_EXIT, 1656, 0, 0);
      if (olseek(w, 12, SEEK_SET) != 12)        sysc(SYS_EXIT, 1657, 0, 0);
      if (owrite(w, "ZZ", 2) != 2)              sysc(SYS_EXIT, 1658, 0, 0);
      oclose(w); }
    { int v = oopen("/lseek-seq");
      if (v < 0) sysc(SYS_EXIT, 1659, 0, 0);
      char c[24];
      for (int i = 0; i < 24; i++) c[i] = 0x7F;
      if (oread(v, c, sizeof c) != 14)          sysc(SYS_EXIT, 1660, 0, 0);  /* 12 + 2 */
      if (c[8] != 'c')                          sysc(SYS_EXIT, 1661, 0, 0);  /* original kept */
      if (c[9] || c[10] || c[11])               sysc(SYS_EXIT, 1662, 0, 0);  /* HOLE reads zero */
      if (c[12] != 'Z' || c[13] != 'Z')         sysc(SYS_EXIT, 1663, 0, 0);
      oclose(v); }

    /* (15) O_TRUNC is what "author this file" means now. Re-authoring the
     * 14-byte file with 2 bytes must leave 2 bytes, not 2 followed by the old
     * tail — the property that made O_TRUNC necessary in this release. */
    { int w = ocreat("/lseek-seq");
      if (w < 0) sysc(SYS_EXIT, 1664, 0, 0);
      if (owrite(w, "hi", 2) != 2)              sysc(SYS_EXIT, 1665, 0, 0);
      oclose(w); }
    { int v = oopen("/lseek-seq");
      if (v < 0) sysc(SYS_EXIT, 1666, 0, 0);
      char c[24];
      if (oread(v, c, sizeof c) != 2)           sysc(SYS_EXIT, 1667, 0, 0);  /* NOT 14 */
      if (c[0] != 'h' || c[1] != 'i')           sysc(SYS_EXIT, 1668, 0, 0);
      oclose(v); }

    /* ==================================================================
     * v0.83: VOL_TMP POSITIONAL READS — descriptor parity with the root
     * volume. tmp is a flat RAM buffer on a different code path, so none of
     * the coverage above says anything about it.
     * ================================================================== */
    /* REUSES tmp/scratch rather than claiming a new name, and that is a
     * constraint rather than a preference: TMP_MAXFILES is 4, all four slots
     * are already spoken for (scratch, redir.txt, one.txt, two.txt), and a tmp
     * slot can never be released — vfs_unlink resolves through DENTS and has no
     * tmp path at all. Claiming a fifth name starved vsh of the one it needed
     * and failed pipestrs with "cannot create tmp/two.txt". scratch is already
     * scratch by name and purpose, and every suite that uses it authors it
     * fresh before reading, so reusing it costs nothing. */
    { int t = ocreat("tmp/scratch");
      if (t < 0) sysc(SYS_EXIT, 1670, 0, 0);
      if (owrite(t, alpha, LS_N) != LS_N) sysc(SYS_EXIT, 1671, 0, 0);
      oclose(t); }

    { int t = oopen("tmp/scratch");
      if (t < 0) sysc(SYS_EXIT, 1672, 0, 0);
      char b[LS_N + 8];

      /* SEQUENTIAL ADVANCEMENT: two reads with no seek must return DIFFERENT,
       * consecutive halves. Before v0.83 both returned the opening bytes, and
       * a tmp read loop could never reach EOF. */
      if (oread(t, b, 5) != 5)                  sysc(SYS_EXIT, 1673, 0, 0);
      if (b[0] != 'A' || b[4] != 'E')           sysc(SYS_EXIT, 1674, 0, 0);
      if (olseek(t, 0, SEEK_CUR) != 5)          sysc(SYS_EXIT, 1675, 0, 0);
      if (oread(t, b, 5) != 5)                  sysc(SYS_EXIT, 1676, 0, 0);
      if (b[0] != 'F' || b[4] != 'J')           sysc(SYS_EXIT, 1677, 0, 0);

      /* Arbitrary offset, and a MID-FILE PARTIAL read: 10 bytes asked for at
       * offset 20 of a 26-byte file must return the 6 that exist, not 10 and
       * not an error. */
      if (olseek(t, 20, SEEK_SET) != 20)        sysc(SYS_EXIT, 1678, 0, 0);
      if (oread(t, b, 10) != 6)                 sysc(SYS_EXIT, 1679, 0, 0);
      if (b[0] != 'U' || b[5] != 'Z')           sysc(SYS_EXIT, 1680, 0, 0);
      if (olseek(t, 0, SEEK_CUR) != LS_N)       sysc(SYS_EXIT, 1681, 0, 0);  /* landed at EOF */

      /* READ PAST EOF is 0 bytes, not an error — that is what ends a loop. */
      if (oread(t, b, 4) != 0)                  sysc(SYS_EXIT, 1682, 0, 0);
      if (olseek(t, 100, SEEK_SET) != 100)      sysc(SYS_EXIT, 1683, 0, 0);
      if (oread(t, b, 4) != 0)                  sysc(SYS_EXIT, 1684, 0, 0);

      /* SEEK_END on a TMP descriptor. This is the one that was quietly wrong:
       * ofile.dirent holds a tmp INDEX, so resolving the end through DENTS[]
       * returned an unrelated root file's length. Invisible while tmp reads
       * ignored the offset; load-bearing now. */
      if (olseek(t, 0, SEEK_END) != LS_N)       sysc(SYS_EXIT, 1685, 0, 0);
      if (olseek(t, -6, SEEK_END) != LS_N - 6)  sysc(SYS_EXIT, 1686, 0, 0);
      if (oread(t, b, 6) != 6)                  sysc(SYS_EXIT, 1687, 0, 0);
      if (b[0] != 'U' || b[5] != 'Z')           sysc(SYS_EXIT, 1688, 0, 0);

      /* And the rewind, so a tmp file can be re-read like any other. */
      if (olseek(t, 0, SEEK_SET) != 0)          sysc(SYS_EXIT, 1689, 0, 0);
      if (oread(t, b, 3) != 3)                  sysc(SYS_EXIT, 1690, 0, 0);
      if (b[0] != 'A' || b[2] != 'C')           sysc(SYS_EXIT, 1691, 0, 0);
      oclose(t); }

    /* (16) TMP POSITIONAL WRITES. Same three cases the root volume gets, on the
     * volume that until v0.83 replaced its whole contents on every write. */
    { int t = oopen("tmp/scratch");
      if (t < 0) sysc(SYS_EXIT, 1692, 0, 0);
      /* Mid-file overwrite: [10,13) becomes "xyz", length unchanged, tail kept. */
      if (olseek(t, 10, SEEK_SET) != 10)        sysc(SYS_EXIT, 1693, 0, 0);
      if (owrite(t, "xyz", 3) != 3)             sysc(SYS_EXIT, 1694, 0, 0);
      if (olseek(t, 0, SEEK_CUR) != 13)         sysc(SYS_EXIT, 1695, 0, 0);  /* cursor advanced */
      if (olseek(t, 0, SEEK_END) != LS_N)       sysc(SYS_EXIT, 1696, 0, 0);  /* length UNCHANGED */
      if (olseek(t, 9, SEEK_SET) != 9)          sysc(SYS_EXIT, 1697, 0, 0);
      char b[LS_N + 8];
      if (oread(t, b, 6) != 6)                  sysc(SYS_EXIT, 1698, 0, 0);
      if (b[0] != 'J' || b[1] != 'x' || b[3] != 'z')
                                                sysc(SYS_EXIT, 1699, 0, 0);  /* overwrote in place */
      if (b[4] != 'N')                          sysc(SYS_EXIT, 1700, 0, 0);  /* THE TAIL SURVIVED */

      /* Extension past the old end, with a zero-filled hole before it. */
      if (olseek(t, LS_N + 2, SEEK_SET) != LS_N + 2) sysc(SYS_EXIT, 1701, 0, 0);
      if (owrite(t, "QQ", 2) != 2)              sysc(SYS_EXIT, 1702, 0, 0);
      if (olseek(t, 0, SEEK_END) != LS_N + 4)   sysc(SYS_EXIT, 1703, 0, 0);  /* file EXTENDED */
      if (olseek(t, LS_N, SEEK_SET) != LS_N)    sysc(SYS_EXIT, 1704, 0, 0);
      if (oread(t, b, 4) != 4)                  sysc(SYS_EXIT, 1705, 0, 0);
      if (b[0] || b[1])                         sysc(SYS_EXIT, 1706, 0, 0);  /* HOLE reads zero */
      if (b[2] != 'Q' || b[3] != 'Q')           sysc(SYS_EXIT, 1707, 0, 0);
      oclose(t); }

    /* (17) TMP UNLINK, and the slot it frees. Before v0.83 vfs_unlink resolved
     * only through DENTS, so a tmp name could never be removed and its slot was
     * gone for the boot. The unlink must report success, the name must stop
     * resolving as an EXISTING file, and — the part that matters — the freed
     * slot must be reusable. */
    { if (ounlink("tmp/scratch") != 0)          sysc(SYS_EXIT, 1708, 0, 0);
      /* A tmp open auto-creates, so "is it gone" is asked by LENGTH: a fresh
       * slot is empty, whereas the file just unlinked held LS_N + 4 bytes. */
      int t = oopen("tmp/scratch");
      if (t < 0)                                sysc(SYS_EXIT, 1709, 0, 0);
      if (olseek(t, 0, SEEK_END) != 0)          sysc(SYS_EXIT, 1710, 0, 0);  /* empty, not stale */
      oclose(t);
      if (ounlink("tmp/scratch") != 0)          sysc(SYS_EXIT, 1711, 0, 0);
      /* Unlinking a name that does not exist must FAIL rather than quietly
       * succeed — otherwise nothing distinguishes "removed" from "never was". */
      if (ounlink("tmp/never-existed") == 0)    sysc(SYS_EXIT, 1712, 0, 0); }

    /* ==================================================================
     * v0.84: POSITIONAL WRITES PAST THE OLD 32 KiB STAGING CEILING.
     *
     * Until this release a tail-preserving write was staged through a 32 KiB
     * buffer, so any positional write whose range ended beyond that returned
     * ENOSPC (-28) — large files could be replaced whole but never edited. The
     * assertions below all sit past 32768 on purpose; every one of them would
     * have failed with -28 before the chunk-map rewrite.
     * ================================================================== */
    { int f = ocreat("/lseek-big");
      if (f < 0) sysc(SYS_EXIT, 1720, 0, 0);
      /* Build 40 KiB in ONE write at offset 0.
       *
       * Deliberately the whole-file path, not ten sequential positional writes.
       * A positional write recomputes file_hash by streaming the finished file
       * back through the chunk map, so building this way costs O(n) per write
       * and O(n^2) overall -- about 220 KiB of chunk reads for a 40 KiB file.
       * That intermittently blew the round's 30 s watchdog on a REUSED volume,
       * where the CAS index is already populated: gate-dirty-smp boot 2 timed
       * out at exit -1 while boots 1 and 3 passed. Cursor advance across
       * sequential writes is already covered by /lseek-seq above; what this file
       * exists to test is positional work PAST 32 KiB, and that is unchanged
       * below. */
      for (int k = 0; k < LSB_TOTAL; k++) g_lsb[k] = (u8)((k / LSB_CHUNK) * 7 + (k % LSB_CHUNK));
      { i64 w = owrite(f, (const char *)g_lsb, LSB_TOTAL);
        if (w == -28) sysc(SYS_EXIT, 1721, 0, 0);   /* ENOSPC: the ceiling is back */
        if (w != LSB_TOTAL) sysc(SYS_EXIT, 1722, 0, 0); }
      if (olseek(f, 0, SEEK_END) != LSB_TOTAL) sysc(SYS_EXIT, 1723, 0, 0);
      oclose(f); }

    { int f = oopen("/lseek-big");
      if (f < 0) sysc(SYS_EXIT, 1724, 0, 0);
      /* A mid-file overwrite at 36000 — past the old ceiling, and deliberately
       * NOT on a 512-byte chunk boundary, so it straddles two chunks and
       * exercises both partial-chunk rebuilds. */
      if (olseek(f, 36000, SEEK_SET) != 36000)  sysc(SYS_EXIT, 1725, 0, 0);
      i64 w = owrite(f, "OUTRUN84", 8);
      if (w == -28)                             sysc(SYS_EXIT, 1726, 0, 0);  /* ENOSPC */
      if (w != 8)                               sysc(SYS_EXIT, 1727, 0, 0);
      if (olseek(f, 0, SEEK_CUR) != 36008)      sysc(SYS_EXIT, 1728, 0, 0);
      if (olseek(f, 0, SEEK_END) != LSB_TOTAL) sysc(SYS_EXIT, 1729, 0, 0); /* length UNCHANGED */
      oclose(f); }

    { int f = oopen("/lseek-big");
      if (f < 0) sysc(SYS_EXIT, 1730, 0, 0);
      char c[16];
      if (olseek(f, 36000, SEEK_SET) != 36000)  sysc(SYS_EXIT, 1731, 0, 0);
      if (oread(f, c, 8) != 8)                  sysc(SYS_EXIT, 1732, 0, 0);
      if (c[0] != 'O' || c[7] != '4')           sysc(SYS_EXIT, 1733, 0, 0);
      /* THE TAIL, past the write and past the old ceiling, must be the original
       * pattern: byte p was written as (u8)(p/4096 * 7 + p%4096). */
      if (oread(f, c, 8) != 8)                  sysc(SYS_EXIT, 1734, 0, 0);
      for (int j = 0; j < 8; j++) {
          int p = 36008 + j;
          if ((u8)c[j] != (u8)((p / LSB_CHUNK) * 7 + (p % LSB_CHUNK)))
                                                sysc(SYS_EXIT, 1735, 0, 0);
      }
      /* And a byte far BEFORE the write is equally untouched — the chunks that
       * were reused by hash rather than rebuilt. */
      if (olseek(f, 1000, SEEK_SET) != 1000)    sysc(SYS_EXIT, 1736, 0, 0);
      if (oread(f, c, 1) != 1)                  sysc(SYS_EXIT, 1737, 0, 0);
      if ((u8)c[0] != (u8)((1000 / LSB_CHUNK) * 7 + (1000 % LSB_CHUNK)))
                                                sysc(SYS_EXIT, 1738, 0, 0);
      oclose(f); }

    { int f = oopen("/lseek-big");
      if (f < 0) sysc(SYS_EXIT, 1739, 0, 0);
      /* EXTENSION past the old ceiling, with a sparse hole before it. */
      if (olseek(f, 45000, SEEK_SET) != 45000)  sysc(SYS_EXIT, 1740, 0, 0);
      i64 w = owrite(f, "ZZZZ", 4);
      if (w == -28)                             sysc(SYS_EXIT, 1741, 0, 0);  /* ENOSPC */
      if (w != 4)                               sysc(SYS_EXIT, 1742, 0, 0);
      if (olseek(f, 0, SEEK_END) != 45004)      sysc(SYS_EXIT, 1743, 0, 0);
      char c[8];
      if (olseek(f, 44996, SEEK_SET) != 44996)  sysc(SYS_EXIT, 1744, 0, 0);
      if (oread(f, c, 8) != 8)                  sysc(SYS_EXIT, 1745, 0, 0);
      if (c[0] || c[1] || c[2] || c[3])         sysc(SYS_EXIT, 1746, 0, 0);  /* HOLE reads zero */
      if (c[4] != 'Z' || c[7] != 'Z')           sysc(SYS_EXIT, 1747, 0, 0);
      oclose(f); }
    ounlink("/lseek-big");

    /* ==================================================================
     * v0.84: TMP UNLINK IS OWNER-OR-ROOT. Until this release the volume
     * recorded no owner, so any holder of PCAP_FILESYSTEM could remove any
     * tmp name. A refusal is only meaningful if someone can actually be
     * refused, so the negative case runs from a process that has genuinely
     * dropped privilege rather than from this root worker.
     * ================================================================== */
    { /* Root creates a slot it owns. */
      int t = ocreat("tmp/owned");
      if (t < 0) sysc(SYS_EXIT, 1750, 0, 0);
      if (owrite(t, "own", 3) != 3) sysc(SYS_EXIT, 1751, 0, 0);
      oclose(t);

      i64 c = ofork();
      if (c < 0) sysc(SYS_EXIT, 1752, 0, 0);
      if (c == 0) {
          /* Drop to an unprivileged identity. Group first, user second — see
           * setuid_privdrop_worker for why that order is load-bearing. */
          if ((i64)sysc(SYS_SETGID, 1000, 0, 0) != 0) sysc(SYS_EXIT, 90, 0, 0);
          if ((i64)sysc(SYS_SETUID, 1000, 0, 0) != 0) sysc(SYS_EXIT, 91, 0, 0);
          if (sysc(SYS_GETEUID, 0, 0, 0) != 1000)     sysc(SYS_EXIT, 92, 0, 0);

          /* THE REFUSAL: root's tmp file, and this caller is not root. EACCES,
           * and specifically NOT the -1 that means "no such tmp file" — a
           * refusal that cannot be told from an absence is not a refusal. */
          if (ounlink("tmp/owned") != -13)            sysc(SYS_EXIT, 93, 0, 0);
          /* And it really is still there: a check that passed because the file
           * had already vanished would prove nothing. */
          /* v0.84: this used to REOPEN the file and measure it, to show the
           * refused unlink had not removed it. It cannot any more — open on
           * this volume is now owner-or-root too — and it MUST NOT be replaced
           * by "the open was refused" either, however tempting: that would make
           * an assertion about the UNLINK rule depend on the OPEN rule, so a
           * build with the open check reverted would report "the refused unlink
           * removed the file" about an unlink that was refused correctly. An
           * experiment that names the wrong guard when it fails is worse than
           * one that tests less.
           *
           * So the child proves only what it can prove without the open guard:
           * that the refusal is STABLE rather than a one-shot that half-did
           * something. Whether the three bytes survived is checked by the
           * parent, which owns the file and may simply read it. */
          if (ounlink("tmp/owned") != -13)            sysc(SYS_EXIT, 94, 0, 0);

          /* The owner path with a NON-ROOT uid, so the rule is shown to be
           * "owner or root" rather than "root only": this process creates its
           * own tmp file and removes it. */
          { int m = ocreat("tmp/mine");
            if (m < 0)                                sysc(SYS_EXIT, 95, 0, 0);
            if (owrite(m, "m", 1) != 1)               sysc(SYS_EXIT, 96, 0, 0);
            oclose(m);
            if (ounlink("tmp/mine") != 0)             sysc(SYS_EXIT, 97, 0, 0); }
          sysc(SYS_EXIT, 89, 0, 0);
      }
      i64 st = owaitpid_ticks((u32)c, WAIT_T_FORK, 0);
      if (st == -11) sysc(SYS_EXIT, 1753, 0, 0);      /* DEADLINE, not a defect */
      if (st != 89)  sysc(SYS_EXIT, 1754 + (u64)(st >= 90 && st <= 97 ? st - 90 : 8), 0, 0);

      /* The bytes the child could neither read nor remove are still the bytes
       * root wrote. Checked from the owner, because the refused party is now
       * refused the read as well — and checked at all because a refusal that
       * had quietly truncated the file would satisfy every return-value
       * assertion above. */
      { int v = oopen("tmp/owned");
        if (v < 0)                                   sysc(SYS_EXIT, 1765, 0, 0);
        if (olseek(v, 0, SEEK_END) != 3)             sysc(SYS_EXIT, 1765, 0, 0);
        oclose(v); }

      /* ROOT OVERRIDE: the same file the unprivileged child could not touch. */
      if (ounlink("tmp/owned") != 0) sysc(SYS_EXIT, 1764, 0, 0); }

    /* ==================================================================
     * v0.84: TMP OPEN, READ AND WRITE ARE OWNER-OR-ROOT TOO.
     *
     * The unlink rule above shipped earlier in this release and guarded
     * removal alone, which left the larger hole open: any holder of
     * PCAP_FILESYSTEM could still READ or OVERWRITE another user's scratch
     * file, having merely been unable to delete it.
     *
     * THE DESCRIPTOR IS OPENED BEFORE THE FORK AND DELIBERATELY LEFT OPEN.
     * That is not incidental setup — it is the only way the read and write
     * checks are reachable at all. Once open is owner-or-root, an
     * unprivileged process cannot obtain a descriptor on someone else's tmp
     * file by asking for one, so a test that only called oopen() would
     * exercise a single guard and leave the other two unreachable. An
     * INHERITED descriptor is the real case those guards exist for, and it
     * is the same case the root volume's write check was written for: a
     * process may hold a perfectly valid descriptor it is not entitled to
     * use.
     * ================================================================== */
    { int t = ocreat("tmp/rperm");
      if (t < 0)                       sysc(SYS_EXIT, 1766, 0, 0);
      if (owrite(t, "abc", 3) != 3)    sysc(SYS_EXIT, 1767, 0, 0);

      i64 c = ofork();
      if (c < 0)                       sysc(SYS_EXIT, 1768, 0, 0);
      if (c == 0) {
          /* Group first, user second — see setuid_privdrop_worker for why the
           * order is load-bearing: after setuid(1000) the process can no
           * longer change its group. */
          if ((i64)sysc(SYS_SETGID, 1000, 0, 0) != 0) sysc(SYS_EXIT, 110, 0, 0);
          if ((i64)sysc(SYS_SETUID, 1000, 0, 0) != 0) sysc(SYS_EXIT, 111, 0, 0);
          if (sysc(SYS_GETEUID, 0, 0, 0) != 1000)     sysc(SYS_EXIT, 112, 0, 0);

          /* THE THREE REFUSALS, REPORTED AS A BITMASK RATHER THAN AS THE FIRST
           * ONE TO FAIL.
           *
           * Exiting at the first failure is the pattern everywhere else in this
           * file and is right when the checks are steps in a sequence. These
           * are not steps; they are three independent guards over the same
           * file, and the build that reverts them reverts all three at once. A
           * first-failure exit would report only `read` and leave `write` and
           * `open` untested in the very run whose job is to show they can fail
           * — which is how a guard ends up believed rather than measured.
           *
           * bit 0 read, bit 1 write, bit 2 open. 0 means all three refused. */
          { int bad = 0;
            char b[4];
            /* SEEK_SET first, so the read happens at a known offset. lseek
             * moves an offset and consults no content, so it is deliberately
             * NOT permission-checked and must still succeed for a caller that
             * may not read — asserted here so that stays a decision rather
             * than an accident. */
            if (olseek(t, 0, SEEK_SET) != 0)          sysc(SYS_EXIT, 113, 0, 0);
            if (oread(t, b, 3) != -13)                bad |= 1;
            if (owrite(t, "XYZ", 3) != -13)           bad |= 2;
            /* A fresh open by name, so the inherited descriptor is not merely a
             * loophole beside a guarded front door. Closed when it wrongly
             * succeeds: a reproducing build must not also leak a descriptor and
             * fail later for an unrelated reason. */
            { int u = oopen("tmp/rperm");
              if (u != -13) { bad |= 4; if (u >= 0) oclose(u); } }
            if (bad) sysc(SYS_EXIT, 130 + (u64)bad, 0, 0); }

          /* THE RULE IS OWNER-OR-ROOT, NOT "unprivileged processes may not use
           * tmp". This caller's OWN file must remain fully usable — create,
           * write, rewind, read back, remove. Without this the whole block
           * would still pass if the kernel simply refused uid 1000 everything,
           * which is a different and much worse rule. */
          { int m = ocreat("tmp/rmine");
            if (m < 0)                                sysc(SYS_EXIT, 117, 0, 0);
            if (owrite(m, "q", 1) != 1)               sysc(SYS_EXIT, 118, 0, 0);
            if (olseek(m, 0, SEEK_SET) != 0)          sysc(SYS_EXIT, 119, 0, 0);
            { char z[2];
              if (oread(m, z, 1) != 1 || z[0] != 'q') sysc(SYS_EXIT, 120, 0, 0); }
            oclose(m);
            if (ounlink("tmp/rmine") != 0)            sysc(SYS_EXIT, 121, 0, 0); }
          sysc(SYS_EXIT, 109, 0, 0);
      }
      i64 st = owaitpid_ticks((u32)c, WAIT_T_FORK, 0);
      if (st == -11) sysc(SYS_EXIT, 1769, 0, 0);      /* DEADLINE, not a defect */
      if (st != 109) {
          /* The refusal mask gets its own range so the decoded line can name
           * WHICH of the three guards let the caller through, rather than
           * reporting that "the permission test failed". */
          u64 e = (st >= 131 && st <= 137) ? 1810 + (u64)(st - 130)
                : (st >= 110 && st <= 121) ? 1790 + (u64)(st - 110)
                : 1802;
          sysc(SYS_EXIT, e, 0, 0);
      }

      /* THE BYTES ARE UNTOUCHED. The refusals above were all return values; a
       * kernel that refused the caller and mutated the file anyway would pass
       * every one of them. This is the check that makes them mean something. */
      { char b[4]; b[0] = b[1] = b[2] = 0;
        if (olseek(t, 0, SEEK_SET) != 0)              sysc(SYS_EXIT, 1803, 0, 0);
        if (oread(t, b, 3) != 3)                      sysc(SYS_EXIT, 1804, 0, 0);
        if (b[0] != 'a' || b[1] != 'b' || b[2] != 'c') sysc(SYS_EXIT, 1805, 0, 0); }
      oclose(t);
      if (ounlink("tmp/rperm") != 0)                  sysc(SYS_EXIT, 1806, 0, 0); }

    /* ==================================================================
     * v0.84: ftruncate — SHRINK, GROW, and the shrink-then-grow case that
     * is the whole reason the boundary chunk is re-put.
     *
     * A shortened file whose last chunk still holds the old bytes past the
     * new end READS correctly, because reads are bounded by the length. It
     * only stops reading correctly when the file is extended again, and
     * POSIX says that gap must be zeroes. So the shrink is checked, and
     * then the file is grown back over the same region and the bytes are
     * checked AGAIN — a test that stopped at the shrink would pass against
     * an implementation that never zeroed anything.
     * ================================================================== */
#define FTR_PATH "/ftr-probe"
#define FTR_LEN  2048u
#define FTR_CUT  700u
    { for (u32 i = 0; i < FTR_LEN; i++) g_lsb[i] = (u8)((i * 13u + 5u) & 0xFF);
      int f = ocreat(FTR_PATH);
      if (f < 0)                                    sysc(SYS_EXIT, 1820, 0, 0);
      if (owrite(f, (const char *)g_lsb, FTR_LEN) != (i64)FTR_LEN)
                                                    sysc(SYS_EXIT, 1821, 0, 0);

      /* SHRINK to a non-chunk boundary: 700 is mid-chunk on purpose. */
      if (oftruncate(f, FTR_CUT) != 0)              sysc(SYS_EXIT, 1822, 0, 0);
      if (olseek(f, 0, SEEK_END) != (i64)FTR_CUT)   sysc(SYS_EXIT, 1823, 0, 0);
      if (olseek(f, 0, SEEK_SET) != 0)              sysc(SYS_EXIT, 1824, 0, 0);
      if (oread(f, (char *)g_lsb + FTR_LEN, FTR_CUT) != (i64)FTR_CUT)
                                                    sysc(SYS_EXIT, 1824, 0, 0);
      for (u32 i = 0; i < FTR_CUT; i++)
          if (g_lsb[FTR_LEN + i] != g_lsb[i])       sysc(SYS_EXIT, 1824, 0, 0);

      /* GROW back over the region just discarded. Every byte from the old
       * cut to the new end must be ZERO — if the boundary chunk kept its
       * tail, the original pattern reappears here instead. */
      if (oftruncate(f, FTR_LEN) != 0)              sysc(SYS_EXIT, 1825, 0, 0);
      if (olseek(f, 0, SEEK_END) != (i64)FTR_LEN)   sysc(SYS_EXIT, 1826, 0, 0);
      if (olseek(f, (i64)FTR_CUT, SEEK_SET) != (i64)FTR_CUT)
                                                    sysc(SYS_EXIT, 1826, 0, 0);
      { u32 tail = FTR_LEN - FTR_CUT;
        for (u32 i = 0; i < tail; i++) g_lsb[FTR_LEN + i] = 0xAA;   /* poison */
        if (oread(f, (char *)g_lsb + FTR_LEN, tail) != (i64)tail)
                                                    sysc(SYS_EXIT, 1826, 0, 0);
        for (u32 i = 0; i < tail; i++)
            if (g_lsb[FTR_LEN + i] != 0)            sysc(SYS_EXIT, 1827, 0, 0); }

      /* To zero, and back up again: the empty-map case. */
      if (oftruncate(f, 0) != 0)                    sysc(SYS_EXIT, 1828, 0, 0);
      if (olseek(f, 0, SEEK_END) != 0)              sysc(SYS_EXIT, 1828, 0, 0);
      if (oftruncate(f, 1000) != 0)                 sysc(SYS_EXIT, 1828, 0, 0);
      if (olseek(f, 0, SEEK_END) != 1000)           sysc(SYS_EXIT, 1828, 0, 0);
      if (olseek(f, 0, SEEK_SET) != 0)              sysc(SYS_EXIT, 1828, 0, 0);
      { for (u32 i = 0; i < 1000; i++) g_lsb[FTR_LEN + i] = 0xAA;
        if (oread(f, (char *)g_lsb + FTR_LEN, 1000) != 1000)
                                                    sysc(SYS_EXIT, 1828, 0, 0);
        for (u32 i = 0; i < 1000; i++)
            if (g_lsb[FTR_LEN + i] != 0)            sysc(SYS_EXIT, 1827, 0, 0); }

      /* A negative length is EINVAL, and is refused BEFORE anything moves. */
      if (oftruncate(f, -1) != -22)                 sysc(SYS_EXIT, 1829, 0, 0);
      if (olseek(f, 0, SEEK_END) != 1000)           sysc(SYS_EXIT, 1829, 0, 0);
      oclose(f);
      ounlink(FTR_PATH); }

    /* A pipe has no length to set. EINVAL rather than ESPIPE: the caller did
     * not attempt to seek, and answering "illegal seek" would name an
     * operation that never happened. */
    { i64 pv[2];
      if ((i64)sysc(SYS_PIPE, (u64)(void *)pv, 0, 0) != 0) sysc(SYS_EXIT, 1830, 0, 0);
      if ((i64)sysc(SYS_FTRUNCATE, (u64)pv[0], 0, 0) != -22) sysc(SYS_EXIT, 1830, 0, 0);
      sysc(SYS_CLOSE, (u64)pv[0], 0, 0);
      sysc(SYS_CLOSE, (u64)pv[1], 0, 0); }

    /* ==================================================================
     * v0.84: O_APPEND — on BOTH volumes.
     *
     * The load-bearing check is that an explicit lseek to 0 is IGNORED. A
     * naive implementation that merely seeks to the end at open time passes
     * a "two writes land end to end" test and fails this one, because the
     * offset it set is then honoured by the next write.
     * ================================================================== */
#define APP_PATH "/app-probe"
    { int f = ocreat(APP_PATH);
      if (f < 0)                                    sysc(SYS_EXIT, 1831, 0, 0);
      if (owrite(f, "abc", 3) != 3)                 sysc(SYS_EXIT, 1832, 0, 0);
      oclose(f);

      int a = oopen_flags(APP_PATH, O_APPEND);
      if (a < 0)                                    sysc(SYS_EXIT, 1833, 0, 0);
      /* Seek to the START, then write: the bytes must still land at the end. */
      if (olseek(a, 0, SEEK_SET) != 0)              sysc(SYS_EXIT, 1834, 0, 0);
      if (owrite(a, "DE", 2) != 2)                  sysc(SYS_EXIT, 1834, 0, 0);
      if (owrite(a, "F", 1) != 1)                   sysc(SYS_EXIT, 1834, 0, 0);
      if (olseek(a, 0, SEEK_END) != 6)              sysc(SYS_EXIT, 1835, 0, 0);
      if (olseek(a, 0, SEEK_SET) != 0)              sysc(SYS_EXIT, 1835, 0, 0);
      { char b[8]; for (int i = 0; i < 8; i++) b[i] = 0;
        if (oread(a, b, 6) != 6)                    sysc(SYS_EXIT, 1835, 0, 0);
        if (b[0] != 'a' || b[1] != 'b' || b[2] != 'c' ||
            b[3] != 'D' || b[4] != 'E' || b[5] != 'F')
                                                    sysc(SYS_EXIT, 1836, 0, 0); }
      oclose(a);
      ounlink(APP_PATH); }

    /* The same rule on the tmp volume, which has agreed with the root volume
     * about what a position means since v0.83. */
    { int t = ocreat("tmp/appprobe");
      if (t < 0)                                    sysc(SYS_EXIT, 1837, 0, 0);
      if (owrite(t, "12", 2) != 2)                  sysc(SYS_EXIT, 1837, 0, 0);
      oclose(t);
      int a = oopen_flags("tmp/appprobe", O_APPEND);
      if (a < 0)                                    sysc(SYS_EXIT, 1837, 0, 0);
      if (olseek(a, 0, SEEK_SET) != 0)              sysc(SYS_EXIT, 1837, 0, 0);
      if (owrite(a, "34", 2) != 2)                  sysc(SYS_EXIT, 1837, 0, 0);
      if (olseek(a, 0, SEEK_END) != 4)              sysc(SYS_EXIT, 1838, 0, 0);
      if (olseek(a, 0, SEEK_SET) != 0)              sysc(SYS_EXIT, 1838, 0, 0);
      { char b[6]; for (int i = 0; i < 6; i++) b[i] = 0;
        if (oread(a, b, 4) != 4)                    sysc(SYS_EXIT, 1838, 0, 0);
        if (b[0] != '1' || b[1] != '2' || b[2] != '3' || b[3] != '4')
                                                    sysc(SYS_EXIT, 1838, 0, 0); }
      /* ftruncate on the tmp volume too, so the parity is tested and not
       * merely claimed. */
      if (oftruncate(a, 2) != 0)                    sysc(SYS_EXIT, 1839, 0, 0);
      if (olseek(a, 0, SEEK_END) != 2)              sysc(SYS_EXIT, 1839, 0, 0);
      oclose(a);
      ounlink("tmp/appprobe"); }

    ounlink("/lseek-seq");
    ounlink(LS_PATH);
    sysc(SYS_EXIT, 42, 0, 0);
}

/* role 54: v0.82 RING-3 ONE-WAY PRIVILEGE DROP.
 *
 * usersstrs has always checked the credential rules from the KERNEL side, by
 * reaching into kprocs[] and reading the six id fields directly. That proves
 * the state machine, and it cannot prove the thing an attacker actually needs
 * to be false: that a process which has dropped privilege cannot get it back by
 * asking, through the syscall boundary, the way a real program would. A comment
 * in usersstrs claimed for several releases that ring 3 exercised this. Nothing
 * did — there was no ring-3 caller of SYS_SETUID or SYS_SETGID anywhere in this
 * file. This worker is that caller.
 *
 * THE ORDER OF THE TWO DROPS IS LOAD-BEARING, and is not the order the obvious
 * reading of "drop uid, then drop gid" would give. Privilege here is euid == 0,
 * never uid == 0 and never egid == 0 — a process with egid 0 but a non-zero
 * euid is NOT privileged and must not be able to hand itself an arbitrary gid.
 * So setuid(1000) first would leave euid == 1000, and the following setgid(1000)
 * would then be REFUSED — correctly, because 1000 is not among that process's
 * real/effective/saved gids. Testing in that order would prove nothing about
 * dropping and would quietly conflate a correct refusal with a failure to drop.
 * Group first, while still privileged; user second, because it is the one that
 * closes the door.
 *
 * setuid() from a privileged process moves all three ids — real, effective AND
 * saved — which is what makes the drop irrevocable: it overwrites the very id a
 * return would have been authorised against. So the escalation attempts below
 * include seteuid(0)/setegid(0), not just setuid(0)/setgid(0). The reversible
 * setter is the sharper test: if `saved` had been left at 0 by the permanent
 * drop, seteuid(0) would legally succeed and the "permanent" drop would be a
 * loan. Checking only setuid(0) would miss that entirely.
 *
 * Exit 42 on full success; every failure point has its own code so the kernel
 * side can name which rule broke rather than reporting "the worker failed". */
#define PD_UID   1000u
#define PD_GID   1000u

static void setuid_privdrop_worker(void) {
    /* (0) The premise. A freshly spawned process inherits nothing: kproc_reset
     * memsets the slot, so all six ids are 0. If that is ever untrue the rest of
     * this worker is measuring the wrong starting state, so it is asserted
     * rather than assumed. */
    if (sysc(SYS_GETUID,  0, 0, 0) != 0) sysc(SYS_EXIT, 900, 0, 0);
    if (sysc(SYS_GETEUID, 0, 0, 0) != 0) sysc(SYS_EXIT, 901, 0, 0);
    if (sysc(SYS_GETGID,  0, 0, 0) != 0) sysc(SYS_EXIT, 902, 0, 0);
    if (sysc(SYS_GETEGID, 0, 0, 0) != 0) sysc(SYS_EXIT, 903, 0, 0);

    /* (1) Drop the GROUP first — see the note above on why this cannot come
     * second. Privileged setgid() moves real, effective and saved together. */
    if ((i64)sysc(SYS_SETGID, PD_GID, 0, 0) != 0) sysc(SYS_EXIT, 904, 0, 0);
    if (sysc(SYS_GETGID,  0, 0, 0) != PD_GID)     sysc(SYS_EXIT, 905, 0, 0);
    if (sysc(SYS_GETEGID, 0, 0, 0) != PD_GID)     sysc(SYS_EXIT, 906, 0, 0);

    /* (2) Drop the USER. After this the process is unprivileged by definition,
     * because privilege is euid == 0 and euid is now PD_UID. */
    if ((i64)sysc(SYS_SETUID, PD_UID, 0, 0) != 0) sysc(SYS_EXIT, 907, 0, 0);
    if (sysc(SYS_GETUID,  0, 0, 0) != PD_UID)     sysc(SYS_EXIT, 908, 0, 0);
    if (sysc(SYS_GETEUID, 0, 0, 0) != PD_UID)     sysc(SYS_EXIT, 909, 0, 0);

    /* (3) THE POINT OF THE WORKER. Four ways back to root, all of which must be
     * refused, and after each one the ids must be UNCHANGED — a refusal that
     * still moved an id is a failure even though it returned an error. */
    if ((i64)sysc(SYS_SETUID, 0, 0, 0) >= 0)  sysc(SYS_EXIT, 910, 0, 0);  /* REGAINED root uid */
    if (sysc(SYS_GETUID,  0, 0, 0) != PD_UID) sysc(SYS_EXIT, 911, 0, 0);
    if (sysc(SYS_GETEUID, 0, 0, 0) != PD_UID) sysc(SYS_EXIT, 912, 0, 0);

    if ((i64)sysc(SYS_SETEUID, 0, 0, 0) >= 0) sysc(SYS_EXIT, 913, 0, 0);  /* saved id leaked   */
    if (sysc(SYS_GETUID,  0, 0, 0) != PD_UID) sysc(SYS_EXIT, 914, 0, 0);
    if (sysc(SYS_GETEUID, 0, 0, 0) != PD_UID) sysc(SYS_EXIT, 915, 0, 0);

    if ((i64)sysc(SYS_SETGID, 0, 0, 0) >= 0)  sysc(SYS_EXIT, 916, 0, 0);  /* REGAINED root gid */
    if (sysc(SYS_GETGID,  0, 0, 0) != PD_GID) sysc(SYS_EXIT, 917, 0, 0);
    if (sysc(SYS_GETEGID, 0, 0, 0) != PD_GID) sysc(SYS_EXIT, 918, 0, 0);

    if ((i64)sysc(SYS_SETEGID, 0, 0, 0) >= 0) sysc(SYS_EXIT, 919, 0, 0);  /* saved gid leaked  */
    if (sysc(SYS_GETGID,  0, 0, 0) != PD_GID) sysc(SYS_EXIT, 920, 0, 0);
    if (sysc(SYS_GETEGID, 0, 0, 0) != PD_GID) sysc(SYS_EXIT, 921, 0, 0);

    /* (4) The drop must survive a fork. A child that came back up as root would
     * be the same leak by another route, and fork() copies six fields by hand. */
    { i64 c = ofork();
      if (c < 0) sysc(SYS_EXIT, 922, 0, 0);
      if (c == 0) {
          if (sysc(SYS_GETUID,  0, 0, 0) != PD_UID) sysc(SYS_EXIT, 71, 0, 0);
          if (sysc(SYS_GETEUID, 0, 0, 0) != PD_UID) sysc(SYS_EXIT, 72, 0, 0);
          if (sysc(SYS_GETGID,  0, 0, 0) != PD_GID) sysc(SYS_EXIT, 73, 0, 0);
          if ((i64)sysc(SYS_SETUID, 0, 0, 0) >= 0)  sysc(SYS_EXIT, 74, 0, 0);
          sysc(SYS_EXIT, 70, 0, 0);
      }
      i64 st = owaitpid_ticks((u32)c, WAIT_T_FORK, 0);
      if (st == -11) sysc(SYS_EXIT, 923, 0, 0);       /* DEADLINE, not a defect */
      if (st != 70)  sysc(SYS_EXIT, 924, 0, 0);       /* child saw the wrong ids */
    }

    sysc(SYS_EXIT, 42, 0, 0);
}

static void posix_orphan_worker(void) {
    i64 p = ofork();
    if (p < 0) sysc(SYS_EXIT, 1751, 0, 0);
    if (p == 0) {                                     /* ---- P: doomed parent ---- */
        g_orph_parent_pid = ogetpid();
        i64 c = ofork();
        if (c < 0)  sysc(SYS_EXIT, 1752, 0, 0);
        if (c == 0) posix_orphan_child();             /* never returns              */
        /* P's exit code carries C's pid outward. W cannot waitpid() a grandchild,
         * and the kernel half needs C's pid to find its verdict in the reap log.
         * Offset so a pid can never be mistaken for one of P's failure sentinels
         * above — pids here reach the high hundreds, and a channel whose values
         * overlap its own error codes is one bad boot away from reporting a
         * failure as a successful result. */
        sysc(SYS_EXIT, 3000000 + (u64)c, 0, 0);
    }

    /* ---- W ---- */
    i64 rp = owaitpid_ticks((u32)p, ORPH_T_STEP, 0);
    if (rp == -11)      sysc(SYS_EXIT, 1753, 0, 0);   /* P never exited             */
    if (rp <  3000000)  sysc(SYS_EXIT, 1754, 0, 0);   /* P failed before forking C  */
    u64 cpid = (u64)rp - 3000000;

    /* P has exited, so its slot is now the lowest recyclable one. Each fork here
     * claims it, bumps its generation, and hands it straight back — which is all
     * the experiment needs, because a generation only ever increases. */
    for (u32 i = 0; i < ORPH_CHURN; i++) {
        i64 t = ofork();
        if (t < 0)  sysc(SYS_EXIT, 1755, 0, 0);
        if (t == 0) sysc(SYS_EXIT, 42, 0, 0);
        if (owaitpid_ticks((u32)t, ORPH_T_STEP, 0) != 42) sysc(SYS_EXIT, 1756, 0, 0);
    }
    /* W exits without lingering for C on purpose. The generation has already
     * been bumped and generations never go backwards, so C's answer no longer
     * depends on anything W does — and a "wait a while for the other process"
     * step is exactly the kind of timing assumption this role exists to remove. */
    sysc(SYS_EXIT, 1000000 + cpid, 0, 0);
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
    /* v0.77: volatile because osetjmp/olongjmp cross this loop — a non-volatile
     * local held in a register at setjmp time has an indeterminate value after
     * the longjmp (C11 7.13.2.1p3). It has worked so far because gcc happened
     * to spill it; that is luck, not a guarantee. */
    for (volatile int round = 0; round < 2; round++) {
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
        /* v0.77: a deadline is not a defect. These were one code, so a slow host
         * and a broken join were the same red line. */
        i64 jr = pthread_join(t[i], &ret);
        if (jr == ETIMEDOUT_NEG)                    sysc(SYS_EXIT, 908, 0, 0);
        if (jr != 0)                                sysc(SYS_EXIT, 902, 0, 0);
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

/* v0.84: THE STARTUP RENDEZVOUS.
 *
 * Every worker announces itself, then holds until main releases them all at
 * once. Without it the threads reached ring 3 whenever the scheduler got to
 * them and immediately contended on g_tw_mutex — and a futex that blocks PARKS,
 * which takes the thread out of ring 3. So the suite's own workload pushed the
 * threads apart, and simultaneous residency was luck rather than design.
 *
 * THE ARRIVAL WAIT YIELDS; THE HOLD SPINS. That split is not stylistic, and the
 * first attempt got it wrong in a way only measurement caught: waiting with a
 * bare spin made the rendezvous time out on every -smp 2 boot, because a
 * spinning worker keeps its core and the remaining workers can only get in on a
 * preemption. Main then released on the deadline rather than on the fact, and
 * the barrier was decoration — the overlap that showed up came entirely from
 * the HOLD below.
 *
 * So arrival yields, which is free: nothing is being measured yet. Residency is
 * created afterwards by the HOLD, which spins, because FUTEX_WAIT and oyield()
 * both leave ring 3 and a hold built from either would measure its own absence.
 * osysticks() is a syscall but does NOT leave ring 3 — g_inr3 is decremented in
 * cpu_exec_proc when a task's excursion ENDS, not on every trap.
 *
 * Both waits are BOUNDED and non-fatal. If the rendezvous never completes the
 * threads proceed anyway and the assertion fails with its diagnostics, which is
 * the trade invariant 4 requires: a barrier that hangs converts a failed
 * assertion into a wedged machine. v0.81 learned that on cmd_mcq's disproven
 * barrier and the lesson is the same one.
 *
 * The HOLD after release is what v0.81 established on cmd_mcq: overlap has to
 * come from residency, not from arriving together. Threads released together
 * would otherwise dive straight back into the mutex and park again. */
#define TW_T_BARRIER 300u        /* 3 s ceiling on the rendezvous              */
#define TW_T_HOLD      6u        /* 60 ms of guaranteed ring-3 residency       */
#define TW_I_CEIL 50000000ull    /* backstop; the tick target wins             */
static volatile u64 g_tw_arrived = 0;    /* workers that reached the barrier       */
static volatile u64 g_tw_start   = 0;    /* main opens this to release them all    */

static u64 tw_body(u64 id) {
    __sync_fetch_and_or(&g_tw_ran, 1ull << id);
    /* POSIX: one pid per thread group, a distinct tid per thread. Both halves
     * are checked, because reporting the slot's own pid from getpid() would
     * make the answer depend on which thread asked. */
    if (sysc(SYS_GETPID, 0, 0, 0) != g_tw_pid) g_tw_pid_bad = 1;
    if (sysc(SYS_GETTID, 0, 0, 0) != id)       g_tw_pid_bad = 1;

    /* v0.84: arrive, then wait to be released — spinning, so ring-3 residency
     * is not surrendered while waiting for it to be measured. */
    __sync_fetch_and_add(&g_tw_arrived, 1);
    { u32 b0 = osysticks();
      while (!g_tw_start && (osysticks() - b0) < TW_T_BARRIER) oyield(); }

    /* Released. Stay in ring 3 long enough for the overlap to be observable
     * rather than instantaneous, then get on with the real work. */
    { u32 h0 = osysticks(); volatile u64 acc = 0;
      for (u64 k = 0; k < TW_I_CEIL; k++) {
          acc += k ^ id;
          if ((k & 0xFFFFull) == 0 && (osysticks() - h0) >= TW_T_HOLD) break;
      } }

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

    /* v0.84: hold until every worker has ARRIVED, then release them together.
     *
     * Waiting on the count rather than on a delay is the whole point: a sleep
     * long enough to "probably" cover thread startup is the same guess the
     * suite was already making implicitly. This waits for the fact.
     *
     * Bounded and non-fatal — if a worker never arrives the release happens
     * anyway and the round proceeds to fail its assertions with diagnostics,
     * rather than hanging the machine.
     *
     * Yielding rather than spinning, for the reason the workers do: main holding
     * a core while waiting is exactly what stopped the workers from reaching the
     * barrier at all on two cpus. Main's own residency does not need protecting
     * here — the overlap is created after the release, not during the wait. */
    { u32 w0 = osysticks();
      while (g_tw_arrived < (u64)TW_THREADS && (osysticks() - w0) < TW_T_BARRIER) oyield();
      if (g_tw_arrived < (u64)TW_THREADS)
          oputs("  [thr:r3] rendezvous incomplete; releasing anyway\n");
      g_tw_start = 1;
      __sync_synchronize(); }

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
        /* v0.77: 945 meant "pthread_join failed OR timed out", and the one
         * observed failure of this suite was the timeout half — a lost wake that
         * expired the kernel's 200 s park. Reported as a join defect, it sent a
         * reader after a join bug that does not exist. 937 is the deadline. */
        i64 jr = pthread_join(t[i], &r);
        if (jr == ETIMEDOUT_NEG)              sysc(SYS_EXIT, 937, 0, 0);
        if (jr != 0)                          sysc(SYS_EXIT, 945, 0, 0);
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

/* --- role 46: mmapstrs — demand paging, protection, and release ------------
 *
 * The interesting assertion is the one about frames: a 1 MiB mapping that is
 * never touched must cost NOTHING. That is checked from the kernel side, where
 * the allocator's counters can see it; here the driver's job is to touch a
 * known number of pages so the kernel has an exact number to expect. */
#define MM_BIG (1024u * 1024u)                 /* 256 pages */
#define MM_TOUCH 8                             /* ...of which we touch 8       */

static struct ojmp g_mm_jb;
static volatile int g_mm_segv = 0;
static void mm_on_segv(int s) { (void)s; __sync_fetch_and_add(&g_mm_segv, 1); olongjmp(&g_mm_jb, 1); }

static void mmap_stress_worker(void) {
    /* (1) A large anonymous mapping succeeds and reads back as ZERO. Demand
     * paging must not hand out a frame with somebody else's data in it. */
    u64 a = sysc(SYS_MMAP, MM_BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (a == MAP_FAILED || (i64)a < 0) sysc(SYS_EXIT, 981, 0, 0);
    if (a & 0xFFF)                     sysc(SYS_EXIT, 982, 0, 0);   /* not page aligned */
    volatile u64 *p = (volatile u64 *)a;
    for (int i = 0; i < MM_TOUCH; i++) if (p[i * 512] != 0) sysc(SYS_EXIT, 983, 0, 0);

    /* (2) Write to exactly MM_TOUCH pages, then read them back. */
    for (int i = 0; i < MM_TOUCH; i++) p[i * 512] = 0xA5A50000ull + i;
    for (int i = 0; i < MM_TOUCH; i++)
        if (p[i * 512] != 0xA5A50000ull + (u64)i) sysc(SYS_EXIT, 984, 0, 0);

    /* (3) W^X is refused at the source, not discovered later. */
    u64 wx = sysc(SYS_MMAP, 0x1000, PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS);
    if (wx != MAP_FAILED && (i64)wx >= 0) sysc(SYS_EXIT, 985, 0, 0);

    /* (4) mprotect to read-only, then a write must raise SIGSEGV — and the
     * handler must be able to recover, which is what makes this a protection
     * test rather than a crash test. */
    u64 ro = sysc(SYS_MMAP, 0x2000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (ro == MAP_FAILED || (i64)ro < 0) sysc(SYS_EXIT, 986, 0, 0);
    volatile u64 *rp = (volatile u64 *)ro;
    rp[0] = 0x1234;                                        /* fault it in, writable */
    if (sysc(SYS_MPROTECT, ro, 0x2000, PROT_READ) != 0) sysc(SYS_EXIT, 987, 0, 0);
    if (rp[0] != 0x1234) sysc(SYS_EXIT, 988, 0, 0);        /* still readable        */
    osigaction(SIGSEGV, mm_on_segv);
    if (osetjmp(&g_mm_jb) == 0) {
        rp[0] = 0xDEAD;                                    /* -> SIGSEGV            */
        sysc(SYS_EXIT, 989, 0, 0);                         /* write to RO succeeded */
    }
    if (g_mm_segv != 1) sysc(SYS_EXIT, 990, 0, 0);
    /* (5) ...and mprotect back to writable makes it work again. */
    if (sysc(SYS_MPROTECT, ro, 0x2000, PROT_READ | PROT_WRITE) != 0) sysc(SYS_EXIT, 991, 0, 0);
    rp[0] = 0xBEEF;
    if (rp[0] != 0xBEEF) sysc(SYS_EXIT, 992, 0, 0);

    /* (6) Release both. The kernel checks the frames actually came back. */
    if (sysc(SYS_MUNMAP, a, MM_BIG, 0) != 0)   sysc(SYS_EXIT, 993, 0, 0);
    if (sysc(SYS_MUNMAP, ro, 0x2000, 0) != 0)  sysc(SYS_EXIT, 994, 0, 0);

    /* (7) malloc over the threshold must route through mmap, and free it. */
    void *big = omalloc(OMMAP_MIN + 4096);
    if (!big) sysc(SYS_EXIT, 995, 0, 0);
    volatile u8 *bp = (volatile u8 *)big;
    bp[0] = 7; bp[OMMAP_MIN] = 9;
    if (bp[0] != 7 || bp[OMMAP_MIN] != 9) sysc(SYS_EXIT, 996, 0, 0);
    ofree(big);
    sysc(SYS_EXIT, 980, 0, 0);
}

/* --- role 47: shmstrs — zero copy between processes, and COW --------------- */
#define SH_BYTES 8192

static void shm_stress_worker(void) {
    /* (1) COW: a value written BEFORE the fork is visible to the child; a value
     * written by the child afterwards must NOT be visible to the parent. That
     * second half is what proves the copy actually happened on write — a
     * broken COW that simply shared the page would let the child's store
     * through. */
    static volatile u64 cow_probe = 0;
    cow_probe = 0x1111;
    i64 f = ofork();
    if (f < 0) sysc(SYS_EXIT, 971, 0, 0);
    if (f == 0) {
        if (cow_probe != 0x1111) sysc(SYS_EXIT, 101, 0, 0);   /* pre-fork value lost */
        cow_probe = 0x2222;                                   /* private from here on */
        if (cow_probe != 0x2222) sysc(SYS_EXIT, 102, 0, 0);
        sysc(SYS_EXIT, 100, 0, 0);
    }
    /* v0.81: tick deadline, and a code of its own. 972 meant BOTH "the COW
     * child exited wrong" and "the wait expired" — a slow host reported as a
     * COW defect, which is the conflation v0.76/v0.78/v0.81 have each fixed
     * elsewhere. 980 is the deadline; 972 keeps its meaning. */
    u32 tw1 = 0;
    i64 st = owaitpid_ticks((u32)f, WAIT_T_FORK, &tw1);
    if (st == -11)          sysc(SYS_EXIT, 980, 0, 0);   /* DEADLINE          */
    if (st != 100)          sysc(SYS_EXIT, 972, 0, 0);   /* ran, wrong answer */
    if (cow_probe != 0x1111) sysc(SYS_EXIT, 973, 0, 0);        /* child leaked through */

    /* (2) Zero-copy shared memory between two distinct processes. */
    i64 id = (i64)sysc(SYS_SHM_CREATE, SH_BYTES, 0, 0);
    if (id < 0) sysc(SYS_EXIT, 974, 0, 0);
    u64 base = sysc(SYS_SHM_MAP, (u64)id, 1, 0);
    if ((i64)base < 0) sysc(SYS_EXIT, 975, 0, 0);
    volatile u64 *sp = (volatile u64 *)base;
    for (int i = 0; i < 8; i++) if (sp[i] != 0) sysc(SYS_EXIT, 976, 0, 0);  /* zeroed */
    sp[0] = 0xC0FFEE;

    i64 g = ofork();
    if (g < 0) sysc(SYS_EXIT, 977, 0, 0);
    if (g == 0) {
        /* The child maps the SAME segment by id. It is not inherited — an
         * attachment is explicit — so this is a genuine second mapper. */
        u64 cb = sysc(SYS_SHM_MAP, (u64)id, 1, 0);
        if ((i64)cb < 0) sysc(SYS_EXIT, 111, 0, 0);
        volatile u64 *cp = (volatile u64 *)cb;
        if (cp[0] != 0xC0FFEE) sysc(SYS_EXIT, 112, 0, 0);     /* parent's write unseen */
        cp[1] = 0xBEEFBEEF;                                    /* reply, zero copy     */
        sysc(SYS_EXIT, 110, 0, 0);
    }
    /* v0.81: as above. 981 is the deadline; 978 keeps "the sharing child
     * exited wrong (it could not see the segment)". */
    u32 tw2 = 0;
    st = owaitpid_ticks((u32)g, WAIT_T_FORK, &tw2);
    if (st == -11)            sysc(SYS_EXIT, 981, 0, 0);   /* DEADLINE          */
    if (st != 110)            sysc(SYS_EXIT, 978, 0, 0);   /* ran, wrong answer */
    if (sp[1] != 0xBEEFBEEF)  sysc(SYS_EXIT, 979, 0, 0);      /* child's write unseen */
    sysc(SYS_EXIT, 970, 0, 0);
}

/* --- role 51: tcpstrs — a real stream, not a datagram in a stream's clothes --
 *
 * Everything is loopback and everything is deterministic. The assertions are
 * written so that a stack which merely COPIED bytes between two sockets would
 * fail them: byte-stream framing is checked across a payload larger than one
 * segment, ordering is checked by content rather than by arrival, and
 * end-of-stream is checked as a distinct answer from "nothing yet". */
#define TCP_PORT_BASE 7000

static void tcp_stress_worker(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);
    u16 sport = (u16)(TCP_PORT_BASE + (pid & 0x1FF));

    /* (1) A listener. */
    int ls = (int)(i64)sysc(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    if (ls < 0) sysc(SYS_EXIT, 1601, 0, 0);
    if ((i64)sysc(SYS_BIND, (u64)ls, (u64)sport, 0) != 0) sysc(SYS_EXIT, 1602, 0, 0);
    if (olisten(ls, 4) != 0) sysc(SYS_EXIT, 1603, 0, 0);

    /* An idle listener has nothing to accept and says so at once. */
    u32 peer[2];
    if (oaccept(ls, peer, 0) != -11) sysc(SYS_EXIT, 1604, 0, 0);

    /* (2) THE HANDSHAKE. connect() on a stream is the one call that genuinely
     * exchanges something before it can return. */
    int cl = (int)(i64)sysc(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    if (cl < 0) sysc(SYS_EXIT, 1605, 0, 0);
    if ((i64)sysc(SYS_CONNECT, (u64)cl, IP_LOOPBACK, (u64)sport) != 0) sysc(SYS_EXIT, 1606, 0, 0);

    /* The connection is ESTABLISHED before accept, not by it. */
    int sv = oaccept(ls, peer, 0);
    if (sv < 0) sysc(SYS_EXIT, 1607, 0, 0);
    if (peer[0] != (u32)IP_LOOPBACK) sysc(SYS_EXIT, 1608, 0, 0);

    /* (3) BYTE STREAM, not message boundaries. 1500 bytes is three segments at
     * a 512-byte MSS, so a stack that preserved message framing — or that lost
     * anything at a segment boundary — produces the wrong bytes here. */
    static u8 big[1500], got[1500];
    for (int i = 0; i < 1500; i++) big[i] = (u8)((i * 31 + 7) & 0xFF);
    int sent = 0;
    while (sent < 1500) {
        i64 n = (i64)sysc(SYS_SEND, (u64)cl, (u64)(void *)(big + sent), (u64)(1500 - sent));
        if (n == -11) { oyield(); continue; }        /* send buffer full: legal */
        if (n <= 0) sysc(SYS_EXIT, 1609, 0, 0);
        sent += (int)n;
    }
    int rcv = 0;
    for (int guard = 0; guard < 20000 && rcv < 1500; guard++) {
        i64 n = (i64)sysc(SYS_RECV, (u64)sv, (u64)(void *)(got + rcv), (u64)(1500 - rcv));
        if (n == -11 || n == 0) { oyield(); continue; }
        if (n < 0) sysc(SYS_EXIT, 1610, 0, 0);
        rcv += (int)n;
    }
    if (rcv != 1500) sysc(SYS_EXIT, 1611, 0, 0);
    for (int i = 0; i < 1500; i++) if (got[i] != big[i]) sysc(SYS_EXIT, 1612, 0, 0);

    /* (4) BIDIRECTIONAL. The server answers on the same connection. */
    u8 rep[16]; for (int i = 0; i < 16; i++) rep[i] = (u8)(0x40 + i);
    if ((i64)sysc(SYS_SEND, (u64)sv, (u64)(void *)rep, 16) != 16) sysc(SYS_EXIT, 1613, 0, 0);
    u8 cb[16]; int cr = 0;
    for (int guard = 0; guard < 20000 && cr < 16; guard++) {
        i64 n = (i64)sysc(SYS_RECV, (u64)cl, (u64)(void *)(cb + cr), (u64)(16 - cr));
        if (n == -11 || n == 0) { oyield(); continue; }
        if (n < 0) sysc(SYS_EXIT, 1614, 0, 0);
        cr += (int)n;
    }
    if (cr != 16) sysc(SYS_EXIT, 1615, 0, 0);
    for (int i = 0; i < 16; i++) if (cb[i] != rep[i]) sysc(SYS_EXIT, 1616, 0, 0);

    /* (5) epoll over a stream: readable on data, writable when it can send. */
    int ep = oepoll_create();
    if (ep < 0) sysc(SYS_EXIT, 1617, 0, 0);
    if (oepoll_ctl(ep, EPOLL_CTL_ADD, sv, EPOLLIN | EPOLLOUT, 0x71) != 0) sysc(SYS_EXIT, 1618, 0, 0);
    struct epoll_event evs[4];
    int n2 = oepoll_wait(ep, evs, 4, 0);
    if (n2 != 1 || !(evs[0].events & EPOLLOUT)) sysc(SYS_EXIT, 1619, 0, 0);  /* established = writable */
    if (evs[0].events & EPOLLIN)                sysc(SYS_EXIT, 1620, 0, 0);  /* drained  = not readable */
    if ((i64)sysc(SYS_SEND, (u64)cl, (u64)(void *)rep, 4) != 4) sysc(SYS_EXIT, 1621, 0, 0);
    for (int guard = 0; guard < 20000; guard++) {
        n2 = oepoll_wait(ep, evs, 4, 0);
        if (n2 == 1 && (evs[0].events & EPOLLIN)) break;
        oyield();
    }
    if (!(evs[0].events & EPOLLIN)) sysc(SYS_EXIT, 1622, 0, 0);
    sysc(SYS_RECV, (u64)sv, (u64)(void *)cb, 4);

    /* (6) ORDERLY CLOSE. Closing the client sends FIN; the server must read
     * END OF STREAM — 0, distinct from EAGAIN — rather than waiting forever. */
    sysc(SYS_CLOSE, (u64)cl, 0, 0);
    int saw_eof = 0;
    for (int guard = 0; guard < 20000; guard++) {
        i64 n = (i64)sysc(SYS_RECV, (u64)sv, (u64)(void *)cb, 16);
        if (n == 0) { saw_eof = 1; break; }
        if (n == -11) { oyield(); continue; }
        if (n < 0) break;
    }
    if (!saw_eof) sysc(SYS_EXIT, 1623, 0, 0);
    /* And epoll must report the hang-up, not merely stop reporting readable. */
    for (int guard = 0; guard < 2000; guard++) {
        n2 = oepoll_wait(ep, evs, 4, 0);
        if (n2 == 1 && (evs[0].events & EPOLLHUP)) break;
        oyield();
    }
    if (!(evs[0].events & EPOLLHUP)) sysc(SYS_EXIT, 1624, 0, 0);

    /* (7) THE WIRE. One non-blocking connect to an address that is NOT
     * loopback, which forces tcp_output down the real encoder and onto the
     * real NIC. Deliberately NOT gated on a reply: whether the SLIRP gateway
     * answers is QEMU's business, and testing it would be testing QEMU. What
     * is asserted, in the kernel half, is that a frame was genuinely built and
     * transmitted — otherwise the encoder is verified only against itself. */
    int ws = (int)(i64)sysc(SYS_SOCKET, AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ws >= 0) {
        sysc(SYS_CONNECT, (u64)ws, 0x0A000202ull, 80);   /* 10.0.2.2:80, the gateway */
        sysc(SYS_CLOSE, (u64)ws, 0, 0);
    }

    sysc(SYS_CLOSE, (u64)sv, 0, 0);
    sysc(SYS_CLOSE, (u64)ls, 0, 0);
    sysc(SYS_CLOSE, (u64)ep, 0, 0);
    sysc(SYS_EXIT, 1600, 0, 0);
}

/* --- role 50: mmapfilestrs — memory that IS a file --------------------------
 *
 * The fixture 'm66dat' is created by the kernel half, deliberately: a suite
 * that corrupts a shared file is exactly the hazard v0.65 spent a milestone
 * chasing, so this one owns its own file and touches nothing else.
 *
 * Byte i of the file is (i*7+3)&0xFF, which is not constant within a page and
 * not equal across pages — so a mapping that returned zeroes, or returned page
 * 0 for every page, fails rather than accidentally passing. */
#define M66_PAGES 4
#define M66_LEN   (M66_PAGES * 4096)
static u8 m66_expect(u64 i) { return (u8)((i * 7 + 3) & 0xFF); }

static void mmapfile_stress_worker(void) {
    int uf = oopen("m66dat");
    if (uf < 0) sysc(SYS_EXIT, 1501, 0, 0);
    int kf = okfd(uf);
    if (kf < 0) sysc(SYS_EXIT, 1502, 0, 0);

    /* (1) MAP_PRIVATE, read-only. The bytes must be the FILE's bytes — and the
     * check reaches into page 3, which only demand paging at a nonzero offset
     * can satisfy. */
    u64 pa = ommap_file(kf, M66_LEN, PROT_READ, MAP_PRIVATE, 0);
    if (pa == MAP_FAILED || (i64)pa < 0) sysc(SYS_EXIT, 1503, 0, 0);
    volatile u8 *pp = (volatile u8 *)pa;
    for (u64 i = 0; i < 64; i++)        if (pp[i] != m66_expect(i))        sysc(SYS_EXIT, 1504, 0, 0);
    for (u64 i = 0; i < 64; i++)        if (pp[4096 + i] != m66_expect(4096 + i)) sysc(SYS_EXIT, 1505, 0, 0);
    for (u64 i = 0; i < 64; i++)        if (pp[3 * 4096 + i] != m66_expect(3 * 4096 + i)) sysc(SYS_EXIT, 1506, 0, 0);

    /* (2) A PRIVATE mapping is a copy: writing it must not reach the file. */
    u64 pw = ommap_file(kf, M66_LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
    if (pw == MAP_FAILED || (i64)pw < 0) sysc(SYS_EXIT, 1507, 0, 0);
    volatile u8 *wp = (volatile u8 *)pw;
    if (wp[0] != m66_expect(0)) sysc(SYS_EXIT, 1508, 0, 0);
    wp[0] = 0xEE; wp[1] = 0xFF;
    if (wp[0] != 0xEE)          sysc(SYS_EXIT, 1509, 0, 0);
    if (omsync(pw, M66_LEN, 0) != 0) sysc(SYS_EXIT, 1510, 0, 0);   /* legal, and a no-op */
    u8 chk[8];
    if (oread(uf, (char *)chk, 8) != 8) sysc(SYS_EXIT, 1511, 0, 0);
    if (chk[0] != m66_expect(0))        sysc(SYS_EXIT, 1512, 0, 0); /* PRIVATE leaked to the file */

    /* (3) MAP_SHARED, writable: the write must reach the file, and only after
     * it is asked to. */
    u64 sa = ommap_file(kf, M66_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
    if (sa == MAP_FAILED || (i64)sa < 0) sysc(SYS_EXIT, 1513, 0, 0);
    volatile u8 *sp = (volatile u8 *)sa;
    if (sp[0] != m66_expect(0)) sysc(SYS_EXIT, 1514, 0, 0);
    sp[0] = 0xA1; sp[1] = 0xA2; sp[4096] = 0xB1;      /* two different pages */
    if (sp[0] != 0xA1 || sp[4096] != 0xB1) sysc(SYS_EXIT, 1515, 0, 0);
    if (omsync(sa, M66_LEN, 0) != 0) sysc(SYS_EXIT, 1516, 0, 0);

    /* Re-read through the ORDINARY file path: writeback is only real if a
     * reader that never mapped anything can see it. */
    int uf2 = oopen("m66dat");
    if (uf2 < 0) sysc(SYS_EXIT, 1517, 0, 0);
    u8 rb[8];
    if (oread(uf2, (char *)rb, 8) != 8) sysc(SYS_EXIT, 1518, 0, 0);
    if (rb[0] != 0xA1 || rb[1] != 0xA2) sysc(SYS_EXIT, 1519, 0, 0);
    oclose(uf2);

    /* (4) CROSS-PROCESS SHARING. The child writes through its own mapping of
     * the same file; the parent must see it through memory it mapped BEFORE
     * the fork, with no syscall in between. That can only happen if both
     * resolve to one frame — which is the entire content of MAP_SHARED and the
     * thing a private-copy implementation fails. */
    i64 r = ofork();
    if (r == 0) {
        /* The child maps the file ITSELF rather than reusing the inherited
         * range. That is the real test: a second, independent mapping of the
         * same file must resolve to the SAME frames, which only a page cache
         * can arrange — an inherited mapping would agree even if every mapper
         * got a private copy, because it agrees by descent rather than by
         * sharing. */
        u64 ca = ommap_file(kf, M66_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
        if (ca == MAP_FAILED || (i64)ca < 0) sysc(SYS_EXIT, 62, 0, 0);
        volatile u8 *cm = (volatile u8 *)ca;
        if (cm[0] != 0xA1) sysc(SYS_EXIT, 61, 0, 0);   /* not the parent's page */
        cm[2] = 0xC3;
        sysc(SYS_EXIT, 60, 0, 0);
    }
    if (r < 0) sysc(SYS_EXIT, 1520, 0, 0);
    /* v0.81: the last raw spin site. 1521 meant both "the child could not see
     * the shared file mapping" and "the wait expired"; 1533 is the deadline. */
    {   u32 tw3 = 0;
        i64 wr3 = owaitpid_ticks((u32)r, WAIT_T_FORK, &tw3);
        if (wr3 == -11) sysc(SYS_EXIT, 1533, 0, 0);   /* DEADLINE          */
        if (wr3 != 60)  sysc(SYS_EXIT, 1521, 0, 0);   /* ran, wrong answer */
    }
    if (sp[2] != 0xC3)                  sysc(SYS_EXIT, 1522, 0, 0);  /* NOT shared */

    /* (5) Unmapping a shared writable mapping flushes it — a process that
     * forgets msync must not lose its writes. */
    if ((i64)sysc(SYS_MUNMAP, sa, M66_LEN, 0) != 0) sysc(SYS_EXIT, 1523, 0, 0);
    int uf3 = oopen("m66dat");
    if (uf3 < 0) sysc(SYS_EXIT, 1524, 0, 0);
    if (oread(uf3, (char *)rb, 8) != 8) sysc(SYS_EXIT, 1525, 0, 0);
    if (rb[2] != 0xC3)                  sysc(SYS_EXIT, 1526, 0, 0);  /* child's write lost */
    oclose(uf3);

    /* (6) Refusals that must stay refusals. */
    if ((i64)ommap_file(kf, M66_LEN, PROT_WRITE | PROT_EXEC, MAP_SHARED, 0) >= 0)
        sysc(SYS_EXIT, 1527, 0, 0);                                  /* W^X */
    if ((i64)ommap_file(kf, M66_LEN, PROT_READ, MAP_SHARED, 0x800) >= 0)
        sysc(SYS_EXIT, 1528, 0, 0);                                  /* unaligned offset */
    if ((i64)ommap_file(99, M66_LEN, PROT_READ, MAP_SHARED, 0) >= 0)
        sysc(SYS_EXIT, 1529, 0, 0);                                  /* EBADF */

    /* (7) The v0.66 catch-all patch, checked from ring 3: an epoll descriptor
     * is not a byte stream and read() must say so rather than hand back
     * device bytes. */
    int ep = oepoll_create();
    if (ep < 0) sysc(SYS_EXIT, 1530, 0, 0);
    if ((i64)sysc(SYS_READ, (u64)ep, (u64)(void *)rb, 8) != -22) sysc(SYS_EXIT, 1531, 0, 0);
    if ((i64)sysc(SYS_WRITE_FILE, (u64)ep, (u64)(void *)rb, 8) != -22) sysc(SYS_EXIT, 1532, 0, 0);
    sysc(SYS_CLOSE, (u64)ep, 0, 0);

    sysc(SYS_MUNMAP, pa, M66_LEN, 0);
    sysc(SYS_MUNMAP, pw, M66_LEN, 0);
    oclose(uf);
    sysc(SYS_EXIT, 1500, 0, 0);
}

/* --- role 49: netepollstrs — sockets as descriptors, and epoll over them ---
 *
 * Everything here is loopback, deliberately. The socket layer's loopback path
 * is synchronous, so an assertion that fails does so because the MECHANISM is
 * wrong, not because SLIRP was slow — the same discipline v0.52 used for
 * netstrs and v0.51 for audio. A round trip through QEMU's NAT would test
 * QEMU's timing, not this kernel's readiness model.
 *
 * "Connection" here means a datagram SESSION: a peer (addr,port) a listener
 * has heard from. There is no TCP in this system, so there is no handshake to
 * perform and none is faked. What is real is everything the milestone is
 * actually about — a descriptor that becomes readable when a peer arrives, an
 * accept that answers EAGAIN when none has, and a wait that is WOKEN by the
 * arrival instead of polling for it. */
static u32 g_nep_srv_port, g_nep_cli_port;
static int g_nep_cli_fd;

/* Posts to the server from a second thread, late enough that the main thread
 * has reached its park. Same shape as v0.64's ew_poster and for the same
 * reason: only the kernel can tell a park from a spin, so the driver has to
 * actually be asleep when the datagram lands. */
static u64 nep_poster(u64 arg) {
    (void)arg;
    for (int i = 0; i < 600; i++) oyield();
    u8 msg[8]; for (int i = 0; i < 8; i++) msg[i] = (u8)(0xA0 + i);
    sysc(SYS_SEND, (u64)g_nep_cli_fd, (u64)(void *)msg, 8);
    return 777;
}

static void netepoll_stress_worker(void) {
    u64 pid = sysc(SYS_GETPID, 0, 0, 0);
    g_nep_srv_port = (u32)(5000 + (pid & 0x1FF));
    g_nep_cli_port = g_nep_srv_port + 512;

    /* (1) A NON-BLOCKING socket answers EAGAIN rather than waiting. This is
     * the assertion the whole milestone rests on: before it, an empty socket
     * burned 2000 ticks before admitting it had nothing. */
    int sv = (int)(i64)sysc(SYS_SOCKET, AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sv < 0) sysc(SYS_EXIT, 1401, 0, 0);
    u8 rb[64];
    if ((i64)sysc(SYS_RECV, (u64)sv, (u64)(void *)rb, 64) != -11) sysc(SYS_EXIT, 1402, 0, 0);

    /* (2) FCNTL round trip. F_GETFL must SEE the flag SOCK_NONBLOCK set at
     * creation — two routes to one piece of state that must agree. */
    if ((ofcntl(sv, F_GETFL, 0) & O_NONBLOCK) == 0) sysc(SYS_EXIT, 1403, 0, 0);
    if (ofcntl(sv, F_SETFL, 0) != 0)                sysc(SYS_EXIT, 1404, 0, 0);
    if (ofcntl(sv, F_GETFL, 0) & O_NONBLOCK)        sysc(SYS_EXIT, 1405, 0, 0);
    if (ofcntl(sv, F_SETFL, O_NONBLOCK) != 0)       sysc(SYS_EXIT, 1406, 0, 0);
    if ((ofcntl(sv, F_GETFL, 0) & O_NONBLOCK) == 0) sysc(SYS_EXIT, 1407, 0, 0);

    /* (3) Bind and listen. */
    if ((i64)sysc(SYS_BIND, (u64)sv, (u64)g_nep_srv_port, 0) != 0) sysc(SYS_EXIT, 1408, 0, 0);
    if (olisten(sv, 4) != 0) sysc(SYS_EXIT, 1409, 0, 0);

    /* (4) An idle listener has NOTHING to accept, and says so immediately. */
    u32 peer[2];
    if (oaccept(sv, peer, 0) != -11) sysc(SYS_EXIT, 1410, 0, 0);

    /* (5) epoll must report an idle listener as NOT ready. A listener that
     * claimed readiness with no peer would send a server into an accept loop
     * that spins on EAGAIN forever — the exact bug epoll exists to prevent. */
    int ep = oepoll_create();
    if (ep < 0) sysc(SYS_EXIT, 1411, 0, 0);
    if (oepoll_ctl(ep, EPOLL_CTL_ADD, sv, EPOLLIN, 0x51) != 0) sysc(SYS_EXIT, 1412, 0, 0);
    struct epoll_event evs[4];
    if (oepoll_wait(ep, evs, 4, 0) != 0) sysc(SYS_EXIT, 1413, 0, 0);

    /* (6) A client speaks. The listener becomes readable, and epoll says so. */
    int cl = (int)(i64)sysc(SYS_SOCKET, AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (cl < 0) sysc(SYS_EXIT, 1414, 0, 0);
    if ((i64)sysc(SYS_BIND, (u64)cl, (u64)g_nep_cli_port, 0) != 0) sysc(SYS_EXIT, 1415, 0, 0);
    if ((i64)sysc(SYS_CONNECT, (u64)cl, IP_LOOPBACK, (u64)g_nep_srv_port) != 0)
        sysc(SYS_EXIT, 1416, 0, 0);
    u8 hello[16]; for (int i = 0; i < 16; i++) hello[i] = (u8)(i * 7 + 1);
    if ((i64)sysc(SYS_SEND, (u64)cl, (u64)(void *)hello, 16) != 16) sysc(SYS_EXIT, 1417, 0, 0);

    int n = oepoll_wait(ep, evs, 4, 0);
    if (n != 1)                       sysc(SYS_EXIT, 1418, 0, 0);
    if (!(evs[0].events & EPOLLIN))   sysc(SYS_EXIT, 1419, 0, 0);
    if (evs[0].data != 0x51)          sysc(SYS_EXIT, 1420, 0, 0);

    /* (7) ACCEPT hands back a NEW descriptor carrying the peer's FIRST
     * datagram. A server that had to read the opening message from the
     * listener instead would have no way to attribute it to a peer. */
    int cs = oaccept(sv, peer, SOCK_NONBLOCK);
    if (cs < 0)                          sysc(SYS_EXIT, 1421, 0, 0);
    if (cs == sv)                        sysc(SYS_EXIT, 1422, 0, 0);
    if (peer[0] != (u32)IP_LOOPBACK)     sysc(SYS_EXIT, 1423, 0, 0);
    if (peer[1] != g_nep_cli_port)       sysc(SYS_EXIT, 1424, 0, 0);
    if ((i64)sysc(SYS_RECV, (u64)cs, (u64)(void *)rb, 64) != 16) sysc(SYS_EXIT, 1425, 0, 0);
    for (int i = 0; i < 16; i++) if (rb[i] != hello[i]) sysc(SYS_EXIT, 1426, 0, 0);

    /* (8) The listener is drained again, and the backlog really emptied. */
    if (oaccept(sv, peer, 0) != -11)     sysc(SYS_EXIT, 1427, 0, 0);
    if (oepoll_wait(ep, evs, 4, 0) != 0) sysc(SYS_EXIT, 1428, 0, 0);

    /* (9) ECHO. The accepted session replies to its peer, and the client — a
     * different descriptor entirely — receives it. This is the round trip the
     * milestone names, and it goes through the session socket, not the
     * listener. */
    if ((i64)sysc(SYS_SEND, (u64)cs, (u64)(void *)rb, 16) != 16) sysc(SYS_EXIT, 1429, 0, 0);
    u8 eb[64];
    if ((i64)sysc(SYS_RECV, (u64)cl, (u64)(void *)eb, 64) != 16) sysc(SYS_EXIT, 1430, 0, 0);
    for (int i = 0; i < 16; i++) if (eb[i] != hello[i]) sysc(SYS_EXIT, 1431, 0, 0);

    /* (10) EPOLLOUT: a connected socket can send, an unconnected one cannot.
     * Reporting writable on a socket with no destination invites a send that
     * can only fail. */
    if (oepoll_ctl(ep, EPOLL_CTL_MOD, sv, EPOLLIN | EPOLLOUT, 0x51) != 0) sysc(SYS_EXIT, 1432, 0, 0);
    if (oepoll_wait(ep, evs, 4, 0) != 0) sysc(SYS_EXIT, 1433, 0, 0);   /* listener: never writable */
    if (oepoll_ctl(ep, EPOLL_CTL_ADD, cl, EPOLLOUT, 0x52) != 0) sysc(SYS_EXIT, 1434, 0, 0);
    n = oepoll_wait(ep, evs, 4, 0);
    if (n != 1 || !(evs[0].events & EPOLLOUT) || evs[0].data != 0x52) sysc(SYS_EXIT, 1435, 0, 0);

    /* (11) EDGE TRIGGERING over a socket. One report per arrival, not one per
     * call — the distinction a level-triggered implementation fails. */
    if (oepoll_ctl(ep, EPOLL_CTL_DEL, cl, 0, 0) != 0) sysc(SYS_EXIT, 1436, 0, 0);
    if (oepoll_ctl(ep, EPOLL_CTL_ADD, cl, EPOLLIN | EPOLLET, 0x53) != 0) sysc(SYS_EXIT, 1437, 0, 0);
    if (oepoll_wait(ep, evs, 4, 0) != 0) sysc(SYS_EXIT, 1438, 0, 0);   /* drained earlier */
    if ((i64)sysc(SYS_SEND, (u64)cs, (u64)(void *)hello, 8) != 8) sysc(SYS_EXIT, 1439, 0, 0);
    n = oepoll_wait(ep, evs, 4, 0);
    if (n != 1 || evs[0].data != 0x53)   sysc(SYS_EXIT, 1440, 0, 0);   /* the edge */
    if (oepoll_wait(ep, evs, 4, 0) != 0) sysc(SYS_EXIT, 1441, 0, 0);   /* no second edge */
    if ((i64)sysc(SYS_RECV, (u64)cl, (u64)(void *)eb, 64) != 8) sysc(SYS_EXIT, 1442, 0, 0);

    /* (12) SEND BACKPRESSURE. The receive ring is four deep; the fifth
     * datagram has nowhere to go and must SAY so rather than vanish. A silent
     * drop is legal for UDP and useless as a non-blocking contract. */
    int sent = 0;
    for (int i = 0; i < 8; i++) {
        i64 r = (i64)sysc(SYS_SEND, (u64)cs, (u64)(void *)hello, 4);
        if (r == -11) break;
        if (r != 4) sysc(SYS_EXIT, 1443, 0, 0);
        sent++;
    }
    if (sent == 0 || sent > 4) sysc(SYS_EXIT, 1444, 0, 0);   /* must fill, then refuse */
    while ((i64)sysc(SYS_RECV, (u64)cl, (u64)(void *)eb, 64) > 0) { }   /* drain */

    /* (13) THE PARK. Nothing is ready; a second thread sends only after this
     * one has had time to fall asleep. The kernel half checks the park counter
     * moved AND that no timeout fired — woken by the datagram, not the clock. */
    g_nep_cli_fd = cs;
    if (oepoll_ctl(ep, EPOLL_CTL_DEL, cl, 0, 0) != 0) sysc(SYS_EXIT, 1445, 0, 0);
    if (oepoll_ctl(ep, EPOLL_CTL_ADD, cl, EPOLLIN, 0x54) != 0) sysc(SYS_EXIT, 1446, 0, 0);
    int wt = kthread_create(nep_poster, 0, 0);
    if (wt < 0) sysc(SYS_EXIT, 1447, 0, 0);
    n = oepoll_wait(ep, evs, 4, 30000);          /* backstop, not a race */
    if (n != 1)                sysc(SYS_EXIT, 1448, 0, 0);
    if (evs[0].data != 0x54)   sysc(SYS_EXIT, 1449, 0, 0);
    u64 wc = 0;
    if (kthread_join(wt, &wc) != 0) sysc(SYS_EXIT, 1450, 0, 0);
    if (wc != 777)                  sysc(SYS_EXIT, 1451, 0, 0);

    /* (14) A socket is a DESCRIPTOR: SYS_CLOSE releases it, and a closed
     * socket is EBADF to every socket call. Before this milestone the only
     * thing that ever reclaimed a socket was the owner dying. */
    if ((i64)sysc(SYS_CLOSE, (u64)cl, 0, 0) != 0) sysc(SYS_EXIT, 1452, 0, 0);
    if ((i64)sysc(SYS_RECV, (u64)cl, (u64)(void *)eb, 64) != -9) sysc(SYS_EXIT, 1453, 0, 0);

    /* (15) Closing a LISTENER hangs up its sessions. The session descriptor is
     * still ours and still valid — it reports end-of-conversation, it does not
     * become a dangling fd. */
    if ((i64)sysc(SYS_CLOSE, (u64)sv, 0, 0) != 0) sysc(SYS_EXIT, 1454, 0, 0);
    if ((i64)sysc(SYS_RECV, (u64)cs, (u64)(void *)eb, 64) != 0) sysc(SYS_EXIT, 1455, 0, 0);

    sysc(SYS_CLOSE, (u64)cs, 0, 0);
    sysc(SYS_CLOSE, (u64)ep, 0, 0);
    sysc(SYS_EXIT, 1400, 0, 0);
}

/* --- role 48: epollstrs — readiness, edges, and end-of-file ----------------
 *
 * Phase 1 driver. Every check is written so the failure mode is a WRONG
 * ANSWER: a readiness bit that should not be set, a cookie that came back
 * altered, a counter that did not drain. */
static volatile int g_ew_fd = -1;
/* Posts the eventfd after a delay long enough for the waiter to be genuinely
 * parked rather than still on its way there. The delay is yields, not ticks:
 * on a uniprocessor the waiter can only reach the park if this thread gives
 * the core back, so yielding IS the thing that lets the race resolve. */
/* The delay exists to lose a race deliberately: the main thread has to reach
 * its park BEFORE this write lands, or it finds the event already waiting,
 * returns without sleeping, and the kernel's park counter never moves — the
 * one thing this round is for. It must not be so long that the waiter's
 * deadline beats it, which is what happened on the first run: 4000 yields
 * outlasted a 3 s park under TCG, the wait timed out, and the poster's write
 * never appeared in the kernel's tally at all. Fewer yields here, a far larger
 * backstop on the wait there — the timeout is a safety net, not the thing the
 * test is racing. */
static u64 ew_poster(u64 arg) {
    (void)arg;
    for (int i = 0; i < 600; i++) oyield();
    u64 one = 1;
    sysc(SYS_WRITE_FILE, (u64)g_ew_fd, (u64)(void *)&one, 8);
    return 555;
}

static void epoll_stress_worker(void) {
    /* (1) An eventfd accumulates writes and drains on read. */
    int ef = oeventfd(0, 0);
    if (ef < 0) sysc(SYS_EXIT, 921, 0, 0);
    u64 v = 0;
    if (oeventfd_read(ef, &v) != -11) sysc(SYS_EXIT, 922, 0, 0);   /* empty = EAGAIN */
    if (oeventfd_write(ef, 7) != 8)   sysc(SYS_EXIT, 923, 0, 0);
    if (oeventfd_write(ef, 5) != 8)   sysc(SYS_EXIT, 924, 0, 0);
    if (oeventfd_read(ef, &v) != 8)   sysc(SYS_EXIT, 925, 0, 0);
    if (v != 12)                      sysc(SYS_EXIT, 926, 0, 0);   /* writes ACCUMULATE */
    if (oeventfd_read(ef, &v) != -11) sysc(SYS_EXIT, 927, 0, 0);   /* drained */

    /* (2) epoll reports the eventfd readable only once it has been posted. */
    int ep = oepoll_create();
    if (ep < 0) sysc(SYS_EXIT, 928, 0, 0);
    if (oepoll_ctl(ep, EPOLL_CTL_ADD, ef, EPOLLIN, 0xAB) != 0) sysc(SYS_EXIT, 929, 0, 0);
    struct epoll_event evs[4];
    if (oepoll_wait(ep, evs, 4, 0) != 0) sysc(SYS_EXIT, 930, 0, 0);  /* nothing posted */
    if (oeventfd_write(ef, 1) != 8)      sysc(SYS_EXIT, 931, 0, 0);
    int n = oepoll_wait(ep, evs, 4, 0);
    if (n != 1)                     sysc(SYS_EXIT, 932, 0, 0);
    if (!(evs[0].events & EPOLLIN)) sysc(SYS_EXIT, 933, 0, 0);
    if (evs[0].data != 0xAB)        sysc(SYS_EXIT, 934, 0, 0);     /* cookie verbatim */

    /* (3) EPOLL_CTL_DEL really removes the watch. */
    if (oepoll_ctl(ep, EPOLL_CTL_DEL, ef, 0, 0) != 0) sysc(SYS_EXIT, 935, 0, 0);
    if (oepoll_wait(ep, evs, 4, 0) != 0)              sysc(SYS_EXIT, 936, 0, 0);

    /* (4) A PIPE — the source epoll exists for. Empty is not readable; written
     * is; and the read end must STILL report readable at END OF FILE, which is
     * what stops a pipeline hanging on its last byte. */
    u64 pfd[2];
    if (sysc(SYS_PIPE, (u64)(void *)pfd, 0, 0) != 0) sysc(SYS_EXIT, 937, 0, 0);
    int rd = (int)pfd[0], wr = (int)pfd[1];
    if (oepoll_ctl(ep, EPOLL_CTL_ADD, rd, EPOLLIN, 0xCD) != 0) sysc(SYS_EXIT, 938, 0, 0);
    if (oepoll_wait(ep, evs, 4, 0) != 0) sysc(SYS_EXIT, 939, 0, 0);   /* empty pipe */
    if (sysc(SYS_WRITE_FILE, (u64)wr, (u64)(void *)"hello", 5) != 5) sysc(SYS_EXIT, 940, 0, 0);
    n = oepoll_wait(ep, evs, 4, 0);
    if (n != 1 || !(evs[0].events & EPOLLIN) || evs[0].data != 0xCD) sysc(SYS_EXIT, 941, 0, 0);
    char sink[8];
    if (sysc(SYS_READ, (u64)rd, (u64)(void *)sink, 5) != 5) sysc(SYS_EXIT, 942, 0, 0);
    sysc(SYS_CLOSE, (u64)wr, 0, 0);
    n = oepoll_wait(ep, evs, 4, 0);
    if (n != 1 || !(evs[0].events & EPOLLIN)) sysc(SYS_EXIT, 943, 0, 0);
    if (!(evs[0].events & EPOLLHUP))          sysc(SYS_EXIT, 944, 0, 0);
    /* Close the read end WITHOUT removing its watch first. The kernel must
     * purge the watch, or a watch left behind on a dead descriptor answers
     * EPOLLERR — which is reported whether or not it was asked for — on every
     * subsequent wait, forever. */
    sysc(SYS_CLOSE, (u64)rd, 0, 0);
    if (oepoll_wait(ep, evs, 4, 0) != 0) sysc(SYS_EXIT, 962, 0, 0);

    /* (5) EDGE vs LEVEL. Re-arm the eventfd, this time edge-triggered: one
     * report per transition, not one per call. Level triggering would answer
     * the second wait identically to the first, which is exactly the
     * distinction being checked. */
    if (oeventfd_write(ef, 1) != 8) sysc(SYS_EXIT, 945, 0, 0);
    if (oepoll_ctl(ep, EPOLL_CTL_ADD, ef, EPOLLIN | EPOLLET, 0xEE) != 0) sysc(SYS_EXIT, 946, 0, 0);
    if (oepoll_wait(ep, evs, 4, 0) != 1) sysc(SYS_EXIT, 947, 0, 0);   /* the edge */
    if (oepoll_wait(ep, evs, 4, 0) != 0) sysc(SYS_EXIT, 948, 0, 0);   /* no second edge */
    if (oepoll_ctl(ep, EPOLL_CTL_MOD, ef, EPOLLIN, 0xEE) != 0) sysc(SYS_EXIT, 949, 0, 0);
    if (oepoll_wait(ep, evs, 4, 0) != 1) sysc(SYS_EXIT, 950, 0, 0);   /* level: still ready */
    if (oeventfd_read(ef, &v) != 8)      sysc(SYS_EXIT, 951, 0, 0);   /* drain it */
    if (oepoll_ctl(ep, EPOLL_CTL_DEL, ef, 0, 0) != 0) sysc(SYS_EXIT, 952, 0, 0);

    /* (6) An idle console is NOT readable. The kernel half injects a keystroke
     * and re-checks, which is the half this process cannot do for itself. */
    if (oepoll_ctl(ep, EPOLL_CTL_ADD, EPOLL_TTY_FD, EPOLLIN, 0x77) != 0) sysc(SYS_EXIT, 953, 0, 0);
    if (oepoll_wait(ep, evs, 4, 0) != 0) sysc(SYS_EXIT, 954, 0, 0);
    if (oepoll_ctl(ep, EPOLL_CTL_DEL, EPOLL_TTY_FD, 0, 0) != 0) sysc(SYS_EXIT, 955, 0, 0);

    /* (7) THE PARK. A worker thread posts the eventfd only after this thread
     * has had time to fall asleep on it. The point is not that the event
     * arrives — round 2 proved that — but that the waiter got there by
     * SLEEPING: the kernel half checks its park counter moved, which it cannot
     * if this loop merely span. */
    g_ew_fd = ef;
    if (oepoll_ctl(ep, EPOLL_CTL_ADD, ef, EPOLLIN, 0x99) != 0) sysc(SYS_EXIT, 956, 0, 0);
    int wt = kthread_create(ew_poster, 0, 0);
    if (wt < 0) sysc(SYS_EXIT, 957, 0, 0);
    /* 30 s is a BACKSTOP, not an expectation: the wake should arrive in
     * milliseconds. A tight timeout here would make the round a race between
     * the poster and the clock, and the answer it gave would depend on how
     * loaded the host running QEMU happened to be.
     *
     * No retry loop: SYS_EPOLL_WAIT restarts itself across a wake and returns
     * POSIX's answer. The loop that used to be here is what proved the Phase 1
     * -EAGAIN contract was the wrong shape — every caller had to know. */
    int got = oepoll_wait(ep, evs, 4, 30000);
    if (got != 1)                   sysc(SYS_EXIT, 958, 0, 0);
    if (evs[0].data != 0x99)        sysc(SYS_EXIT, 959, 0, 0);
    u64 wc = 0;
    if (kthread_join(wt, &wc) != 0) sysc(SYS_EXIT, 960, 0, 0);
    if (wc != 555)                  sysc(SYS_EXIT, 961, 0, 0);

    sysc(SYS_CLOSE, (u64)ep, 0, 0);
    sysc(SYS_CLOSE, (u64)ef, 0, 0);
    sysc(SYS_EXIT, 920, 0, 0);
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
            /* v0.82 CORRECTED THE EXPECTATION, not just the code.
             *
             * This used to read the inherited fd and assert it returned THE
             * SAME BYTES the parent read, calling that POSIX. It is the
             * opposite of POSIX. fork shares the file DESCRIPTION, and the
             * offset lives in the description — so a child reads on from where
             * the parent stopped. Getting the same bytes twice is what you get
             * from a SECOND open(), which has its own position.
             *
             * The old assertion held only because SYS_READ ignored the offset
             * and always read from the start. Once reads became positional it
             * failed, which is the correct outcome: the test had been encoding
             * the kernel's behaviour rather than the standard's, and passing
             * for the wrong reason.
             *
             * So: the parent already read to EOF, therefore this read must
             * return 0. That IS the evidence the offset is shared rather than
             * copied — a stronger claim than the old one, and one the previous
             * kernel could not have made. */
            if (oread(fd, b2, sizeof b2 - 1) != 0) sysc(SYS_EXIT, 969, 0, 0);
            /* And now the child rewinds the shared description and reads
             * exactly what the parent read — the same assertion as before, but
             * earned rather than assumed. */
            if (olseek(fd, 0, SEEK_SET) != 0) sysc(SYS_EXIT, 968, 0, 0);
            i64 cn = oread(fd, b2, sizeof b2 - 1);
            if (cn != pn) sysc(SYS_EXIT, 969, 0, 0);
            for (i64 k = 0; k < cn; k++)
                if (b2[k] != buf[k]) sysc(SYS_EXIT, 969, 0, 0);
        }
        sysc(SYS_EXIT, 42, 0, 0);
    }
    if (r < 0) sysc(SYS_EXIT, 965, 0, 0);
    /* v0.81: a tick deadline, and a code of its own.
     *
     * This was `owaitpid((u32)r, 30000)` — a SPIN COUNT, the defect class v0.77
     * fixed in langstrs/toolstrs/pipestrs and v0.78 fixed in role 29. v0.78's
     * changelog said plainly what was left: "owaitpid(pid, spins) is still used
     * throughout the tree; this change converted one driver, not the idiom."
     * This is one of the four that remained, and it duly failed a -smp 4 gate
     * under the KERNEL_DEBUG build — which adds work to every lock operation and
     * is therefore exactly the slow-host lever a spin budget cannot survive.
     *
     * 963 was ALSO overloaded: it meant "the fd table was not inherited", a real
     * defect, and "the wait expired", a slow host. Reporting both as one number
     * sends a reader after an inheritance bug that does not exist — the same
     * conflation v0.76 fixed for pipestrs (957) and v0.78 for role 29 (702).
     * 972 is the deadline; 963 keeps its original meaning.
     *
     * WAIT_T_FORK (2000 ticks) rather than WAIT_T_RUN, because this wait sits
     * inside a posixstrs round whose posix_drain watchdog is 3000 ticks: the
     * inner deadline has to fire first or the round reports "not every task
     * reached a terminal state" instead of naming the assertion. */
    {
        u32 spent = 0;
        i64 wr = owaitpid_ticks((u32)r, WAIT_T_FORK, &spent);
        if (wr == -11) sysc(SYS_EXIT, 972, 0, 0);      /* DEADLINE, not a defect */
        if (wr != 42)  sysc(SYS_EXIT, 963, 0, 0);      /* ran, but the wrong answer */
    }

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
    /* v0.76: a deadline, and a timeout no longer masquerades as "the pipe did
     * not survive fork inheritance" — which is what exit 957 claims. */
    { i64 pst = owaitpid_ticks((u32)r, WAIT_T_RUN, 0);
      if (pst == -11)                                       sysc(SYS_EXIT, 965, 0, 0);
      if (pst != 43)                                        sysc(SYS_EXIT, 957, 0, 0); }

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
    { i64 hst = owaitpid_ticks((u32)hr, WAIT_T_RUN, 0);
      if (hst == -11)                                       sysc(SYS_EXIT, 965, 0, 0);
      if (hst != 44)                                        sysc(SYS_EXIT, 949, 0, 0); }
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
    /* v0.76: a deadline, not a spin count. -2 distinguishes "the wait expired"
     * from "the compiler said no" — the caller must not report a slow host as a
     * compiler defect. */
    i64 st = owaitpid_ticks((u32)pid, WAIT_T_COMPILE, 0);
    if (st == -11) return -2;
    return st == 0 ? 0 : -1;
}

static void vsh_worker(void) {
    if (selfhost_author("/src/emit.c", VSH_EMIT_SRC) < 0)  sysc(SYS_EXIT, 963, 0, 0);
    if (selfhost_author("/src/wcx.c",  VSH_WCX_SRC)  < 0)  sysc(SYS_EXIT, 963, 0, 0);
    if (selfhost_author("/src/t.vsh",  VSH_SCRIPT)   < 0)  sysc(SYS_EXIT, 963, 0, 0);

    /* /src/vsh.c is published by the kernel's SDK, not authored here — the
     * point is that the shipped source compiles, unmodified. */
    { int c1 = vsh_compile("/src/vsh.c",  "/bin/vsh");
      if (c1 == -2) sysc(SYS_EXIT, 965, 0, 0);
      if (c1 < 0)   sysc(SYS_EXIT, 961, 0, 0); }
    { int c2 = vsh_compile("/src/emit.c", "/bin/emit");
      if (c2 == -2) sysc(SYS_EXIT, 965, 0, 0);
      if (c2 < 0)   sysc(SYS_EXIT, 961, 0, 0); }
    { int c3 = vsh_compile("/src/wcx.c",  "/bin/wcx");
      if (c3 == -2) sysc(SYS_EXIT, 965, 0, 0);
      if (c3 < 0)   sysc(SYS_EXIT, 961, 0, 0); }
    oputs("  [vsh   ] occ built /bin/vsh, /bin/emit and /bin/wcx from source\n");

    i64 pid = ofork();
    if (pid == 0) {
        const char *av[] = { "/bin/vsh", "/src/t.vsh", 0 };
        const char *ev[] = { "OUTRUN=1", 0 };
        oexecve("/bin/vsh", av, ev);
        sysc(SYS_EXIT, 199, 0, 0);
    }
    if (pid < 0)                                           sysc(SYS_EXIT, 962, 0, 0);
    { i64 vst = owaitpid_ticks((u32)pid, WAIT_T_RUN, 0);
      if (vst == -11)                                      sysc(SYS_EXIT, 965, 0, 0);
      if (vst != 0)                                        sysc(SYS_EXIT, 964, 0, 0); }

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
    /* v0.76: the comment above describes this exact defect being hit once
     * already — the parent gave up while the child was still working. The
     * response then was a distinct exit code, which made the symptom legible
     * but left the cause in place: the budget was still a count of the WAITER'S
     * own iterations, which means nothing in real time. It is a deadline now. */
    i64 cst = owaitpid_ticks((u32)pid, WAIT_T_COMPILE, 0);
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
    i64 rst = owaitpid_ticks((u32)pid, WAIT_T_RUN, 0);
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

/* v0.77: the spin-budgeted cs_compile() is GONE rather than left beside its
 * replacement. Both rounds now call cs_compile_ticks() below. A dead helper
 * that still compiles is how the old idiom comes back: the next person to add
 * a round copies whichever one their eye lands on. */

/* v0.76: the same compile, waited on by the CLOCK rather than by a spin count.
 * See owaitpid_ticks(). Budgets below are ceilings for a pathological host, not
 * expectations — the elapsed figure is printed so they can be tightened from
 * measurement later instead of from taste. */
/* v0.76: these were LANG_T_*; they are now the shared WAIT_T_* defined beside
 * owaitpid_ticks(), because toolstrs and pipestrs need the same budgets and two
 * copies of the same number is how they drift apart. */
#define LANG_T_COMPILE WAIT_T_COMPILE
#define LANG_T_OMAKE   WAIT_T_TOOL
#define LANG_T_RUN     WAIT_T_RUN

static i64 cs_compile_ticks(const char **av, u32 budget, u32 *spent) {
    i64 pid = ofork();
    if (pid == 0) {
        static const char *ev[] = { "STAGE=compilerstrs", 0 };
        oexecve("/bin/occ", av, ev);
        sysc(SYS_EXIT, 199, 0, 0);
    }
    if (pid < 0) return -1;
    return owaitpid_ticks((u32)pid, budget, spent);
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

    /* v0.76: report the shape of the machine this ran on. A budget failure on
     * 4 vCPUs and one on 1 vCPU are different findings, and the log should not
     * make the reader guess which it is looking at. */
    { u32 nc = osysncpu();
      oputs("  [lang  ] toolchain round starting on ");
      oputu(nc); oputs(" cpu(s), clock-based budgets\n"); }

    /* ---- the language round ---- */
    { static const char *av[] = { "/bin/occ", "/src/lang.c", "-o", "/bin/lang.elf", 0 };
      u32 el = 0;
      i64 st = cs_compile_ticks(av, LANG_T_COMPILE, &el);
      oputs("  [lang  ] compiled /src/lang.c in "); oputu(el / 10); oputs(" ds\n");
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
      i64 rst = owaitpid_ticks((u32)pid, LANG_T_RUN, 0);
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
          i64 st = cs_compile_ticks(n1, LANG_T_COMPILE, 0);
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
      u32 el = 0;
      /* THE STAGE THAT FAILED. Under -smp 4 this compile finished — /bin/omake
       * came out 36562 bytes and well-formed — but the old spin-count waiter had
       * already given up on it. A clock-based budget waits for the work, not for
       * a number of its own iterations. */
      i64 st = cs_compile_ticks(av, LANG_T_OMAKE, &el);
      oputs("  [lang  ] compiled /src/omake.c in "); oputu(el / 10); oputs(" ds\n");
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
      u32 el = 0;
      i64 rst = owaitpid_ticks((u32)pid, LANG_T_OMAKE, &el);
      oputs("  [lang  ] omake ran in "); oputu(el / 10); oputs(" ds\n");
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
      i64 rst = owaitpid_ticks((u32)pid, LANG_T_RUN, 0);
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
      /* v0.77: was cs_compile(), which waited 250000 of the WAITER's iterations.
       * This is the last compile-heavy spin budget in the tree and the one the
       * v0.76 changelog named as highest-risk: same stage shape as langstrs,
       * which failed 2 of 2 on -smp 4 boot 1 before it was converted. */
      u32 el = 0;
      i64 st = cs_compile_ticks(av, LANG_T_COMPILE, &el);
      oputs("  [compst] compiled the two-unit build in "); oputu(el); oputs(" ds\n");
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
      i64 rst = owaitpid_ticks((u32)pid, LANG_T_RUN, 0);   /* v0.77: was 250000 spins */
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
          i64 st = cs_compile_ticks(n1, LANG_T_COMPILE, 0);     /* v0.77 */
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
    if (role == 48) { epoll_stress_worker(); }         /* v0.64 epoll readiness, edges, eventfd, EOF        */
    if (role == 49) { netepoll_stress_worker(); }      /* v0.65 non-blocking sockets multiplexed with epoll */
    if (role == 50) { mmapfile_stress_worker(); }      /* v0.66 file-backed mappings and writeback           */
    if (role == 51) { tcp_stress_worker(); }           /* v0.67 TCP: handshake, byte stream, FIN             */
    if (role == 52) { mcq_resident_probe(); }          /* v0.81 tick-resident concurrency probe (cmd_mcq)    */
    /* Rebase note: this worker was written as role 52 against v0.77, but v0.81
     * gave 52 to mcq_resident_probe. Renumbered to 53 here; the kernel-side
     * spawn was renumbered to match. */
    if (role == 53) { posix_orphan_worker(); }         /* v0.77 cross-generation orphan: getppid() after a slot recycle */
    if (role == 54) { setuid_privdrop_worker(); }      /* v0.82 one-way privilege drop, observed from ring 3          */
    if (role == 55) { lseek_worker(); }                /* v0.82 SYS_LSEEK: the file position, from ring 3             */
    if (role == 46) { mmap_stress_worker(); }          /* v0.63 demand paging, mprotect, munmap             */
    if (role == 47) { shm_stress_worker(); }           /* v0.63 COW fork + zero-copy shared memory          */
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
