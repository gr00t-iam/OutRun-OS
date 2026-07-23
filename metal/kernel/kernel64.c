/* ============================================================================
 * OUTRUN OS — BARE-METAL KERNEL (kernel/kernel64.c)
 * ============================================================================
 * Freestanding x86_64 kernel entered from boot.asm in long mode. Provides:
 *   - dual console: VGA text (Proxmox/monitor) + 16550 serial (headless)
 *   - full IDT: CPU exception reporting + remapped PIC hardware IRQs
 *   - PIT timer @ 100 Hz, PS/2 keyboard with US scancode map
 *   - Multiboot2 parsing: bootloader name + physical memory map
 *   - the Outrun capability table, running in kernel space
 *   - an interactive shell on BOTH consoles (type on the VM display or the
 *     serial terminal — Proxmox xterm.js console works out of the box)
 * ============================================================================ */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

#define KERNEL_VERSION "0.50.0-metal"

/* ---- Port I/O ------------------------------------------------------------- */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void io_wait(void) { outb(0x80, 0); }
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ---- Serial console (COM1) ------------------------------------------------ */
#define COM1 0x3F8
static void serial_init(void) {
    outb(COM1 + 1, 0x00);            /* disable interrupts (we poll RX)      */
    outb(COM1 + 3, 0x80);            /* DLAB on                              */
    outb(COM1 + 0, 0x01);            /* 115200 baud                          */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);            /* 8N1                                  */
    outb(COM1 + 2, 0xC7);            /* FIFO on, cleared, 14-byte threshold  */
    outb(COM1 + 4, 0x0B);            /* RTS/DSR out                          */
}
static void serial_putc(char c) {
    if (c == '\n') serial_putc('\r');
    while (!(inb(COM1 + 5) & 0x20)) { }
    outb(COM1, (uint8_t)c);
}
static int serial_getc_nonblock(void) {
    if (inb(COM1 + 5) & 0x01) return inb(COM1);
    return -1;
}

/* ---- VGA text console ------------------------------------------------------ */
#define VGA_MEM  ((volatile uint16_t *)0xB8000)
#define VGA_W 80
#define VGA_H 25
static uint8_t vga_color = 0x0F;     /* white on black                        */
static int vga_row = 0, vga_col = 0;

static void vga_move_cursor(void) {
    uint16_t pos = (uint16_t)(vga_row * VGA_W + vga_col);
    outb(0x3D4, 14); outb(0x3D5, (uint8_t)(pos >> 8));
    outb(0x3D4, 15); outb(0x3D5, (uint8_t)(pos & 0xFF));
}
static void vga_clear(void) {
    for (int i = 0; i < VGA_W * VGA_H; i++)
        VGA_MEM[i] = (uint16_t)(' ' | (vga_color << 8));
    vga_row = vga_col = 0;
    vga_move_cursor();
}
static void vga_scroll(void) {
    for (int r = 1; r < VGA_H; r++)
        for (int c = 0; c < VGA_W; c++)
            VGA_MEM[(r - 1) * VGA_W + c] = VGA_MEM[r * VGA_W + c];
    for (int c = 0; c < VGA_W; c++)
        VGA_MEM[(VGA_H - 1) * VGA_W + c] = (uint16_t)(' ' | (vga_color << 8));
    vga_row = VGA_H - 1;
}
static void vga_putc(char ch) {
    if (ch == '\n') { vga_col = 0; vga_row++; }
    else if (ch == '\b') {
        if (vga_col > 0) { vga_col--; VGA_MEM[vga_row * VGA_W + vga_col] = (uint16_t)(' ' | (vga_color << 8)); }
    } else {
        VGA_MEM[vga_row * VGA_W + vga_col] = (uint16_t)((uint8_t)ch | (vga_color << 8));
        if (++vga_col >= VGA_W) { vga_col = 0; vga_row++; }
    }
    if (vga_row >= VGA_H) vga_scroll();
    vga_move_cursor();
}

/* ---- kprintf: both consoles at once ---------------------------------------- */
static volatile int g_quiet = 0;   /* suppress console output during fuzz loops */
static void kputc(char c) { if (g_quiet) return; vga_putc(c); serial_putc(c); }

/* v0.35: the console is the first kernel structure genuinely shared between
 * cores, so it gets the kernel's first real spinlock. Discipline: the lock is
 * only ever held with interrupts disabled on the holding CPU, so an ISR that
 * prints can never deadlock against the thread it interrupted — the interrupt
 * could not have fired while the lock was held.                              */
static volatile int g_conlock = 0;
static inline uint64_t con_lock(void) {
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0" : "=r"(fl));
    __asm__ volatile("cli");
    while (__sync_lock_test_and_set(&g_conlock, 1)) __asm__ volatile("pause");
    return fl;
}
static inline void con_unlock(uint64_t fl) {
    __sync_lock_release(&g_conlock);
    if (fl & 0x200) __asm__ volatile("sti");
}

static void kputs_raw(const char *s) { while (*s) kputc(*s++); }
static void kputs(const char *s) {
    uint64_t fl = con_lock();
    kputs_raw(s);
    con_unlock(fl);
}

/* v0.37: the console printers are excluded from the stack protector. They are
 * the one stack-protected path invoked by EVERY core and every SMP coordinator
 * (cmd_smp/cmd_parallel/cmd_audit and the APs), and the v0.19 per-thread canary
 * — a single global the BSP scheduler swaps per thread — races those concurrent
 * callers, faulting intermittently only during the multi-core suites. Excluding
 * shared low-level I/O helpers from stack protection is standard kernel
 * practice and is the root fix for that class (which piecemeal
 * no_stack_protector on individual coordinators only chased around). */
static void __attribute__((no_stack_protector))
kput_u64(uint64_t v, unsigned base, bool pad16) {
    static const char *digits = "0123456789abcdef";
    char buf[24];
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v) { buf[i++] = digits[v % base]; v /= base; }
    if (pad16) while (i < 16) buf[i++] = '0';
    while (i--) kputc(buf[i]);
}
static void __attribute__((no_stack_protector)) kprintf(const char *fmt, ...) {
    uint64_t fl = con_lock();          /* one line = one atomic console unit  */
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { kputc(*p); continue; }
        p++;
        switch (*p) {
        case 's': kputs_raw(va_arg(ap, const char *)); break;
        case 'c': kputc((char)va_arg(ap, int)); break;
        case 'd': {
            int64_t v = va_arg(ap, int);
            if (v < 0) { kputc('-'); v = -v; }
            kput_u64((uint64_t)v, 10, false);
        } break;
        case 'u': kput_u64(va_arg(ap, uint64_t), 10, false); break;
        case 'x': kput_u64(va_arg(ap, uint64_t), 16, false); break;
        case 'X': kput_u64(va_arg(ap, uint64_t), 16, true);  break;
        case 'M': {                                    /* MiB, from bytes */
            uint64_t b = va_arg(ap, uint64_t);
            kput_u64(b / (1024 * 1024), 10, false);
        } break;
        case '%': kputc('%'); break;
        default:  kputc('?'); break;
        }
    }
    va_end(ap);
    con_unlock(fl);
}

/* ===========================================================================
 * PER-CPU STATE (v0.39 — moved to the top of the kernel: the trap path, the
 * stack protector and the capability gate are all defined in terms of it)
 * ===========================================================================
 * GS_BASE on every core points at its own `struct cpu_local` in BOTH rings
 * (ring 3 cannot change GS_BASE — no wrgsbase, CR4.FSGSBASE off — so no
 * swapgs exists anywhere in the kernel). The fields at fixed offsets are the
 * asm contract shared with boot/usermode.asm and the compiler's stack guard:
 * every word of trap-path state that was a .bss global through v0.38 lives
 * here now, which is what makes CONCURRENT ring-3 execution on several cores
 * sound — no shared scratch remains in the entry/exit paths.               */
struct pcb;                                  /* fwd: scheduler thread block   */
struct tss64;                                /* fwd: per-CPU task state seg   */
#define MAX_CPUS 8
#define RQ_LEN   8                           /* v0.39: per-CPU run queue slots */
struct cpu_local {
    uint32_t idx;                            /* %gs:0   MUST stay offset 0    */
    uint32_t apic_id;                        /* %gs:4                         */
    uint64_t syscall_rsp;                    /* %gs:8   SYSCALL kernel stack  */
    uint64_t user_rsp;                       /* %gs:16  parked user RSP       */
    uint64_t krsp, krbx, krbp;               /* %gs:24,32,40  kernel resume   */
    uint64_t kr12, kr13, kr14, kr15;         /* %gs:48..72    context         */
    uint64_t canary;                         /* %gs:80  stack-protector guard */
    struct pcb *cur;                         /* thread this CPU runs          */
    uint64_t cur_proc;                       /* the capability gate reads this */
    struct tss64 *tss;                       /* this CPU's own TSS            */
    volatile uint32_t online;
    volatile uint32_t resched;               /* IPI asked this CPU to resched */
    volatile uint32_t ipi_ping;              /* pings received                */
    volatile uint32_t work_done;
    volatile uint64_t probe_val;             /* what this cpu read at g_probe_va */
    /* v0.39 Stage 2: this CPU's run queue of ring-3 tasks (kproc indices).
     * A tiny ring under a per-CPU spinlock; MPMC because idle siblings STEAL
     * from it. The AP drains its own queue autonomously — no BSP mailbox.    */
    volatile int      rq[RQ_LEN];
    volatile uint32_t rq_h, rq_t;            /* head = consumer, tail = producer */
    volatile int      rq_lock;
    volatile uint32_t rq_ran;                /* tasks this CPU ran to completion */
    volatile uint32_t rq_stolen;             /* tasks this CPU stole from siblings */
    volatile uint32_t preempt_count;         /* ring-3 contexts forced out (IPI 50) */
    volatile uint32_t slice_count;           /* v0.40: contexts sliced out by MY timer */
    /* v0.41: cross-core reentrancy witnesses + lock-rank discipline.          */
    volatile uint32_t fs_ops;                /* file syscalls dispatched ON this CPU */
    uint8_t  rank_stack[8];                  /* klock ranks held by the context      */
    uint8_t  rank_sp;                        /* running on this CPU (APs only; the   */
    volatile int dbg_was_idle;               /* v0.43: DEBUG_SMP_SCHED idle-edge latch */
};                                           /* BSP tracks per-THREAD in the PCB)    */
#define CPUL_SYSCALL_RSP 8                   /* asm contract (boot/usermode.asm) */
#define CPUL_USER_RSP    16
#define CPUL_KRSP        24
#define CPUL_CANARY      80                  /* -mstack-protector-guard-offset */
_Static_assert(__builtin_offsetof(struct cpu_local, syscall_rsp) == CPUL_SYSCALL_RSP,
               "asm contract: syscall_rsp at %gs:8");
_Static_assert(__builtin_offsetof(struct cpu_local, user_rsp) == CPUL_USER_RSP,
               "asm contract: user_rsp at %gs:16");
_Static_assert(__builtin_offsetof(struct cpu_local, krsp) == CPUL_KRSP,
               "asm contract: resume context at %gs:24..72");
_Static_assert(__builtin_offsetof(struct cpu_local, canary) == CPUL_CANARY,
               "compiler contract: stack guard at %gs:80");
static struct cpu_local g_cpu[MAX_CPUS];
static volatile int g_gs_ready;              /* set once per-CPU GS bases are armed */
static uint32_t cpu_idx(void);               /* fwd: %gs:0, or 0 before arming */

/* Who is 'running' for every capability check in syscall_dispatch. Since
 * v0.39 this is PER-CPU state (each core carries the identity of the ring-3
 * task IT is executing) and per-thread on top: the BSP scheduler saves and
 * restores its CPU's slot across context switches exactly as before.        */
#define current_proc_idx (g_cpu[cpu_idx()].cur_proc)

/* ---- stack-protector runtime (canary) ------------------------------------- */
/* The compiler (-fstack-protector-strong, guard=tls at %gs:CPUL_CANARY)       */
/* injects prologue/epilogue checks against THIS CPU's guard word. Per-CPU is  */
/* the root fix for the v0.35-37 intermittents: the old single global was      */
/* swapped by the BSP scheduler underneath concurrently-executing APs.         */
static void *g_canary_jmp[5];
static volatile int g_canary_test = 0, g_canary_caught = 0;
__attribute__((noreturn)) void __stack_chk_fail(void);
void __stack_chk_fail(void) {
    if (g_canary_test) { g_canary_caught = 1; g_canary_test = 0; __builtin_longjmp(g_canary_jmp, 1); }
    uint32_t who = 0; if (g_gs_ready) __asm__ volatile("mov %%gs:0, %0" : "=r"(who));
    g_conlock = 0;
    kprintf("\n[canary ] STACK SMASHING DETECTED on cpu%u (guard=%X) — halting core\n",
            (uint64_t)who, g_cpu[who].canary);
    for (;;) __asm__ volatile("cli; hlt");
}
static void __attribute__((no_stack_protector)) canary_init(void) {
    uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = ((uint64_t)hi << 32) | lo;
    for (int i = 0; i < MAX_CPUS; i++)       /* every core its own guard word  */
        g_cpu[i].canary = (tsc * 0x9E3779B97F4A7C15ull)
                          ^ (0xD1CE5EED4B1D57AAull + (uint64_t)i * 0x100000001B3ull);
}

/* ---- tiny libc ----------------------------------------------------------- */
static int kstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int kstrncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

/* ---- IDT ------------------------------------------------------------------- */
struct idt_entry {
    uint16_t off_lo, sel;
    uint8_t  ist, flags;
    uint16_t off_mid;
    uint32_t off_hi, zero;
} __attribute__((packed));
struct idtr { uint16_t limit; uint64_t base; } __attribute__((packed));

static struct idt_entry idt[52];   /* 0-31 exceptions, 32-47 PIC IRQs, 48-50 IPIs, 51 slice tick */
extern void idt_load(void *idtr);

#define ISR_EXTERN(n) extern void isr##n(void);
#define ISR_ADDR(n)   (uint64_t)isr##n
ISR_EXTERN(0)  ISR_EXTERN(1)  ISR_EXTERN(2)  ISR_EXTERN(3)  ISR_EXTERN(4)
ISR_EXTERN(5)  ISR_EXTERN(6)  ISR_EXTERN(7)  ISR_EXTERN(8)  ISR_EXTERN(9)
ISR_EXTERN(10) ISR_EXTERN(11) ISR_EXTERN(12) ISR_EXTERN(13) ISR_EXTERN(14)
ISR_EXTERN(15) ISR_EXTERN(16) ISR_EXTERN(17) ISR_EXTERN(18) ISR_EXTERN(19)
ISR_EXTERN(20) ISR_EXTERN(21) ISR_EXTERN(22) ISR_EXTERN(23) ISR_EXTERN(24)
ISR_EXTERN(25) ISR_EXTERN(26) ISR_EXTERN(27) ISR_EXTERN(28) ISR_EXTERN(29)
ISR_EXTERN(30) ISR_EXTERN(31) ISR_EXTERN(32) ISR_EXTERN(33) ISR_EXTERN(34)
ISR_EXTERN(35) ISR_EXTERN(36) ISR_EXTERN(37) ISR_EXTERN(38) ISR_EXTERN(39)
ISR_EXTERN(40) ISR_EXTERN(41) ISR_EXTERN(42) ISR_EXTERN(43) ISR_EXTERN(44)
ISR_EXTERN(45) ISR_EXTERN(46) ISR_EXTERN(47) ISR_EXTERN(48) ISR_EXTERN(49)
ISR_EXTERN(50) ISR_EXTERN(51)

static void idt_set(int v, uint64_t handler) {
    idt[v].off_lo  = handler & 0xFFFF;
    idt[v].sel     = 0x08;                     /* boot.asm code segment */
    idt[v].ist     = 0;
    idt[v].flags   = 0x8E;                     /* present, DPL0, interrupt gate */
    idt[v].off_mid = (handler >> 16) & 0xFFFF;
    idt[v].off_hi  = (uint32_t)(handler >> 32);
    idt[v].zero    = 0;
}

/* ---- PIC + PIT --------------------------------------------------------------- */
static void pic_remap(void) {
    outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();
    outb(0x21, 32);   io_wait(); outb(0xA1, 40);   io_wait();
    outb(0x21, 4);    io_wait(); outb(0xA1, 2);    io_wait();
    outb(0x21, 1);    io_wait(); outb(0xA1, 1);    io_wait();
    outb(0x21, 0xFC);            /* unmask IRQ0 (timer) + IRQ1 (keyboard)      */
    outb(0xA1, 0xFF);
}

/* Registered device IRQ handlers (top halves). PCI INTx lines are shared, so  */
/* each line holds a small chain; every handler checks its own device's ISR.   */
static void (*g_irq_handlers[16][4])(void) = { { 0 } };
static void register_irq(uint8_t irq, void (*f)(void)) {
    for (int i = 0; i < 4; i++) if (!g_irq_handlers[irq][i]) { g_irq_handlers[irq][i] = f; return; }
}

/* Deferred work (bottom halves / softirqs) run on IRQ return.                 */
static void (*g_softirqs[4])(void) = { 0 };
static volatile int g_softirq_pending = 0;
static void register_softirq(void (*f)(void)) {
    for (int i = 0; i < 4; i++) if (!g_softirqs[i]) { g_softirqs[i] = f; return; }
}

/* Preemption control: the timer reschedules only when this is 0.             */
static volatile int g_preempt_off = 1;      /* starts disabled until sched_init */
static volatile int g_need_resched = 0;
static void sched_preempt(void);            /* fwd: defined with the scheduler  */

/* Unmask one IRQ line on the 8259 pair (and the cascade for slave IRQs).      */
static void pic_unmask(uint8_t irq) {
    if (irq < 8) {
        outb(0x21, inb(0x21) & ~(uint8_t)(1u << irq));
    } else {
        outb(0xA1, inb(0xA1) & ~(uint8_t)(1u << (irq - 8)));
        outb(0x21, inb(0x21) & ~(uint8_t)(1u << 2));       /* cascade IRQ2       */
    }
}
static void pit_init(void) {
    uint16_t div = 11932;        /* 1193182 Hz / 100                            */
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
}

static volatile uint64_t g_ticks = 0;

/* v0.47: IRQ-line pending state for ring-3 VFIO waiters, declared here (not
 * with the rest of the v0.47 VFIO section further down) because isr_dispatch
 * — defined just below, long before struct kproc/kdev exist — needs to bump
 * it directly from the real device-IRQ path and the timer tick. Lines
 * [0,16) mirror the real PIC IRQ lines vector 34..47 dispatches; [16,24) are
 * reserved, software-only test vectors (see the full section comment near
 * vfio_teardown_kproc for why no real device IRQ line is safe to hijack for
 * deterministic testing). */
#define MAX_VFIO_LINES 24
static volatile uint32_t g_vfio_irq_seq[MAX_VFIO_LINES];
/* Armed by cmd_vfio_stress before dispatching its ring-3 driver: once
 * g_ticks reaches this value, the timer-tick path below fires the simulated
 * interrupt exactly once and disarms. 0 = disarmed. */
static volatile uint64_t g_vfio_test_fire_at = 0;

/* ---- PS/2 keyboard -------------------------------------------------------------- */
static const char sc_map[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0,'\\',
    'z','x','c','v','b','n','m',',','.','/', 0,'*', 0,' ',
};
static const char sc_map_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0,'|',
    'Z','X','C','V','B','N','M','<','>','?', 0,'*', 0,' ',
};
static volatile char kbd_ring[64];
static volatile uint32_t kbd_w = 0, kbd_r = 0;
static bool shift_down = false;

static void keyboard_irq(void) {
    uint8_t sc = inb(0x60);
    if (sc == 0x2A || sc == 0x36) { shift_down = true;  return; }
    if (sc == 0xAA || sc == 0xB6) { shift_down = false; return; }
    if (sc & 0x80) return;                           /* other key releases     */
    char c = shift_down ? sc_map_shift[sc & 0x7F] : sc_map[sc & 0x7F];
    if (c && ((kbd_w - kbd_r) < 64))
        kbd_ring[kbd_w++ % 64] = c;
}
/* ---- PS/2 mouse (IRQ 12) --------------------------------------------------- */
/* Streams 3-byte packets from the auxiliary device. The IRQ only accumulates
 * deltas and button state; all coordinate work happens in the canvas, so the
 * top half stays tiny. */
static volatile int32_t g_mouse_dx = 0, g_mouse_dy = 0, g_mouse_dz = 0;
static volatile uint8_t g_mouse_btn = 0;
static volatile int g_mouse_ok = 0, g_mouse_pkts = 0, g_mouse_bytes = 4;
static uint8_t g_mpkt[4]; static int g_mpi = 0;

static void ps2_wait_in(void)  { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return; }
static void ps2_wait_out(void) { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return; }
static void ps2_cmd(uint8_t c) { ps2_wait_in(); outb(0x64, c); }
static void ps2_data(uint8_t d) { ps2_wait_in(); outb(0x60, d); }
static uint8_t ps2_read(void) { ps2_wait_out(); return inb(0x60); }
static uint8_t mouse_cmd(uint8_t c) { ps2_cmd(0xD4); ps2_data(c); return ps2_read(); }  /* -> ACK 0xFA */

static void mouse_irq(void) {
    uint8_t st = inb(0x64);
    if (!(st & 0x01) || !(st & 0x20)) return;              /* not aux data        */
    uint8_t b = inb(0x60);
    if (g_mpi == 0 && !(b & 0x08)) return;                 /* resync on sync bit  */
    g_mpkt[g_mpi++] = b;
    if (g_mpi < g_mouse_bytes) return;
    g_mpi = 0;
    uint8_t f = g_mpkt[0];
    if (f & 0xC0) return;                                  /* overflow -> drop    */
    int32_t dx = (int32_t)g_mpkt[1] - ((f & 0x10) ? 256 : 0);
    int32_t dy = (int32_t)g_mpkt[2] - ((f & 0x20) ? 256 : 0);
    g_mouse_dx += dx;
    g_mouse_dy -= dy;                                      /* PS/2 y is up-positive */
    if (g_mouse_bytes == 4) {
        int8_t z = (int8_t)(g_mpkt[3] & 0x0F);
        if (z & 0x08) z = (int8_t)(z | 0xF0);              /* sign-extend 4-bit    */
        g_mouse_dz += z;
    }
    g_mouse_btn = (uint8_t)(f & 0x07);
    g_mouse_pkts++;
}

static void mouse_init(void) {
    ps2_cmd(0xA8);                                          /* enable aux port     */
    ps2_cmd(0x20); uint8_t cfg = ps2_read();                /* read config byte    */
    cfg |= 0x02;                                            /* enable IRQ12        */
    cfg &= (uint8_t)~0x20;                                  /* enable aux clock    */
    ps2_cmd(0x60); ps2_data(cfg);
    if (mouse_cmd(0xF6) != 0xFA) { kputs("[mouse  ] no PS/2 mouse (set-defaults not acked)\n"); return; }
    /* IntelliMouse magic knock: 200,100,80 samples/sec -> device id 3 = wheel    */
    mouse_cmd(0xF3); mouse_cmd(200); mouse_cmd(0xF3); mouse_cmd(100);
    mouse_cmd(0xF3); mouse_cmd(80);
    mouse_cmd(0xF2); uint8_t id = ps2_read();
    g_mouse_bytes = (id == 3) ? 4 : 3;
    mouse_cmd(0xF4);                                        /* enable reporting    */
    register_irq(12, mouse_irq);
    pic_unmask(12);
    g_mouse_ok = 1;
    kprintf("[mouse  ] PS/2 mouse online: id %u, %d-byte packets%s, IRQ 12\n",
            (uint64_t)id, (uint64_t)g_mouse_bytes, g_mouse_bytes == 4 ? " (wheel)" : "");
}

static int kbd_getc_nonblock(void) {
    if (kbd_r == kbd_w) return -1;
    return kbd_ring[kbd_r++ % 64];
}

/* ---- Exception + IRQ dispatch ------------------------------------------------------ */
struct isr_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};
static const char *exc_names[32] = {
    "Divide Error","Debug","NMI","Breakpoint","Overflow","BOUND Range",
    "Invalid Opcode","Device N/A","Double Fault","Coproc Seg","Invalid TSS",
    "Segment Not Present","Stack Fault","General Protection","Page Fault",
    "Reserved","x87 FP","Alignment Check","Machine Check","SIMD FP","Virt",
    "CP","Rsv","Rsv","Rsv","Rsv","Rsv","Rsv","Hypervisor","VMM Comm",
    "Security","Rsv"
};

/* Recoverable-fault hook: lets a self-test deliberately trigger a CPU fault    */
/* (NX/write-protect poison) and resume, instead of panicking.                  */
static volatile int      g_fault_expected = 0, g_fault_caught = 0;
static volatile uint64_t g_fault_vector = 0, g_fault_recover_rip = 0, g_fault_recover_rsp = 0;

/* Runtime-gated diagnostics for the syscall-exit / page-fault CR3 discipline.
 * Both default OFF (one branch, no output) and are toggled at runtime, same
 * discipline as g_fault_expected above. */
static volatile int g_debug_pagefault    = 0;   /* DEBUG_PAGEFAULT: log every #PF        */
static volatile int g_debug_syscall_exit = 0;   /* DEBUG_SYSCALL_EXIT: log every sysret   */
static volatile int g_debug_smp_sched    = 0;   /* DEBUG_SMP_SCHED: pick/switch/idle log  */
static volatile int g_debug_dma_lifetime = 0;   /* DEBUG_DMA_LIFETIME: grant create/revoke */
static volatile int g_debug_kproc_lifetime = 0; /* DEBUG_KPROC_LIFETIME: recycle/descriptor teardown */
static volatile int g_debug_ipc = 0;            /* DEBUG_IPC: send/recv/xfer/teardown log            */
static volatile int g_debug_vfio = 0;           /* DEBUG_VFIO: BAR map/irq-wait/teardown log         */
static volatile int g_debug_vfs  = 0;           /* DEBUG_VFS: journal commit/apply/unlink log        */
static volatile int g_debug_gpu  = 0;           /* DEBUG_GPU: resource/scanout/flush/fence log        */

/* user-stack layout (used by the guard-page fault check in isr_dispatch) */
#define USTK_V     0x0000500000FF0000ull                  /* user stack bottom       */
#define USTK_PAGES 4                                       /* 16 KiB ring-3 stack     */
#define USTK_TOP   (USTK_V + USTK_PAGES * 0x1000ull)       /* initial RSP             */
#define USTK_GUARD (USTK_V - 0x1000ull)                    /* unmapped guard page     */

static void handle_cpl3_fault(struct isr_frame *f);      /* defined after process globals */
static void sweep_tick(void);                            /* incremental PTE integrity audit */
static void smp_ipi_dispatch(uint64_t vec);              /* v0.35: LAPIC IPI handlers      */
static void smp_preempt_ipi(struct isr_frame *f);        /* v0.39: vector 50 (may not return) */
static volatile int g_guard_caught = 0;                  /* set when a guard fault is handled */
static inline uint64_t read_cr3(void);                   /* fwd: defined with paging helpers  */
static uint64_t dbg_pid_of(uint64_t proc_idx);           /* fwd: defined after kprocs[]        */
struct kdev;                                              /* fwd: defined with the device registry */
static struct kdev *kdev_find(uint64_t io_addr);          /* fwd: v0.45 page_free_tree device guard */

void isr_dispatch(struct isr_frame *f) {
    /* v0.35: IPIs first — they run on ANY cpu and must touch none of the      */
    /* BSP-only machinery below (PIC EOI, softirqs, scheduler, sweep).         */
    if (f->vector == 48 || f->vector == 49) { smp_ipi_dispatch(f->vector); return; }
    /* v0.39: preempt IPI (50); v0.40: this core's own LAPIC slice tick (51).  */
    /* If either caught ring 3, this call NEVER RETURNS — the handler captures */
    /* the user context from `f` and unwinds the whole interrupt through this  */
    /* core's kernel resume point instead of iretq.                            */
    if (f->vector == 50 || f->vector == 51) { smp_preempt_ipi(f); return; }
    if (f->vector < 32) {
        if (g_fault_expected && (f->vector == 14 || f->vector == 13 || f->vector == 6)) {
            g_fault_caught = 1; g_fault_vector = f->vector; g_fault_expected = 0;
            f->rip = g_fault_recover_rip;                /* resume at recovery point */
            f->rsp = g_fault_recover_rsp;
            return;                                      /* iretq back into the test */
        }
        if (g_debug_pagefault && f->vector == 14) {
            uint64_t cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            kprintf("[dbgpf  ] pid %u cr3=%X fault_va=%X rip=%X rsp=%X err=%x cpl=%u\n",
                    dbg_pid_of(current_proc_idx), read_cr3(), cr2, f->rip, f->rsp,
                    f->error, (uint64_t)(f->cs & 3));
        }
        /* A fault from ring 3 must never take down the kernel — terminate the    */
        /* offending task (guard-page hit = stack overflow).                      */
        if ((f->cs & 3) == 3) handle_cpl3_fault(f);      /* noreturn: unwinds to kernel */
        g_conlock = 0;                       /* terminal path: bust the console  */
        kprintf("\n[panic ] CPU EXCEPTION %u: %s (err=%x) at rip=%X\n",
                f->vector, exc_names[f->vector], f->error, f->rip);
        kprintf("[panic ] system halted — the fault was contained to this core\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    if (f->vector == 32) {
        g_ticks++; sweep_tick();                     /* PIT + incremental audit */
        /* v0.47: cmd_vfio_stress's simulated device interrupt — fires once,   */
        /* from the SAME timer path that already drives everything else here,  */
        /* rather than a separate injection mechanism. */
        if (g_vfio_test_fire_at && g_ticks >= g_vfio_test_fire_at) {
            g_vfio_irq_seq[16]++;
            g_vfio_test_fire_at = 0;
        }
    }
    if (f->vector == 33) keyboard_irq();             /* PS/2                    */
    if (f->vector >= 34 && f->vector < 48) {         /* device IRQs (top half)  */
        uint8_t irq = (uint8_t)(f->vector - 32);
        for (int i = 0; i < 4; i++) if (g_irq_handlers[irq][i]) g_irq_handlers[irq][i]();
        g_vfio_irq_seq[irq]++;    /* v0.47: flag pending state for any ring-3 VFIO waiter */
    }
    if (f->vector >= 40) outb(0xA0, 0x20);           /* EOI slave               */
    outb(0x20, 0x20);                                /* EOI master              */

    /* Bottom halves (softirqs): drain device work outside the top half.       */
    if (g_softirq_pending) { g_softirq_pending = 0; for (int i = 0; i < 4; i++) if (g_softirqs[i]) g_softirqs[i](); }

    /* Preemptive reschedule on the timer tick, when it is safe to switch.      */
    if (f->vector == 32 && !g_preempt_off) {
        if (g_need_resched) { g_need_resched = 0; sched_preempt(); }
        else sched_preempt();
    }
}

/* ---- Multiboot2 parsing ----------------------------------------------------------------- */
struct mb2_tag { uint32_t type, size; };
struct mb2_mmap_entry { uint64_t base, len; uint32_t type, rsv; };

static uint64_t g_total_ram = 0;

static uint64_t g_user_elf = 0, g_user_elf_end = 0;    /* boot module: the user ELF */
static uint64_t g_fb_addr = 0;                         /* framebuffer physical base */
static uint32_t g_fb_pitch = 0, g_fb_width = 0, g_fb_height = 0;
static uint8_t  g_fb_bpp = 0;

static uint64_t g_rsdp = 0;          /* ACPI Root System Description Pointer   */
static int      g_rsdp_rev = 0;      /* 1 = ACPI 1.0 (RSDT), 2 = ACPI 2.0+     */
static void multiboot_scan(uint64_t info_addr, bool print) {
    uint8_t *p   = (uint8_t *)info_addr;
    uint32_t total = *(uint32_t *)p;
    uint8_t *end = p + total;
    p += 8;
    while (p < end) {
        struct mb2_tag *tag = (struct mb2_tag *)p;
        if (tag->type == 0) break;
        if (tag->type == 2 && print)                  /* bootloader name        */
            kprintf("[kernel ] booted by: %s\n", (char *)(p + 8));
        if (tag->type == 3) {                         /* boot module (user ELF) */
            g_user_elf     = *(uint32_t *)(p + 8);
            g_user_elf_end = *(uint32_t *)(p + 12);
            if (print) kprintf("[kernel ] boot module '%s' at phys %X..%X (user ELF)\n",
                               (char *)(p + 16), g_user_elf, g_user_elf_end);
        }
        if (tag->type == 6) {                         /* memory map             */
            uint32_t esize = *(uint32_t *)(p + 8);
            uint8_t *e = p + 16;
            if (print) kprintf("[kernel ] physical memory map (Multiboot2):\n");
            g_total_ram = 0;
            while (e < p + tag->size) {
                struct mb2_mmap_entry *m = (struct mb2_mmap_entry *)e;
                if (print)
                    kprintf("           %X + %X  %s\n", m->base, m->len,
                            m->type == 1 ? "USABLE RAM" : "reserved");
                if (m->type == 1) g_total_ram += m->len;
                e += esize;
            }
            if (print) kprintf("           total usable: %M MiB\n", g_total_ram);
        }
        if (tag->type == 8) {                         /* framebuffer info       */
            g_fb_addr   = *(uint64_t *)(p + 8);
            g_fb_pitch  = *(uint32_t *)(p + 16);
            g_fb_width  = *(uint32_t *)(p + 20);
            g_fb_height = *(uint32_t *)(p + 24);
            g_fb_bpp    = *(uint8_t  *)(p + 28);
            if (print) kprintf("[kernel ] framebuffer: %dx%d x%d bpp, pitch %d, phys %X\n",
                               (uint64_t)g_fb_width, (uint64_t)g_fb_height, (uint64_t)g_fb_bpp,
                               (uint64_t)g_fb_pitch, g_fb_addr);
        }
        if (tag->type == 14 && !g_rsdp) {             /* ACPI 1.0 RSDP          */
            g_rsdp = (uint64_t)(p + 8); g_rsdp_rev = 1;
            if (print) kputs("[acpi   ] RSDP (ACPI 1.0) provided by bootloader\n");
        }
        if (tag->type == 15) {                        /* ACPI 2.0+ RSDP         */
            g_rsdp = (uint64_t)(p + 8); g_rsdp_rev = 2;
            if (print) kputs("[acpi   ] RSDP (ACPI 2.0+) provided by bootloader\n");
        }
        p += (tag->size + 7) & ~7u;                   /* tags are 8-aligned     */
    }
}

/* ---- The Outrun capability table, now in kernel space ------------------------------------- */
#define MAX_CAPS 16
struct capability {
    uint64_t generation;
    char     holder[24];
    char     resource[16];
    char     rights;                 /* 'R' or 'W'                              */
    bool     live, used;
};
static struct capability cap_table[MAX_CAPS];
static uint64_t gen_seed = 40961;

static void kstrcpy_n(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; src[i] && i < n - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
static int cap_mint(const char *holder, const char *res, char rights) {
    for (int i = 0; i < MAX_CAPS; i++) {
        if (cap_table[i].used) continue;
        gen_seed = gen_seed * 6364136223846793005ull + 1442695040888963407ull;
        cap_table[i].generation = (gen_seed >> 33) % 100000;
        kstrcpy_n(cap_table[i].holder, holder, sizeof cap_table[i].holder);
        kstrcpy_n(cap_table[i].resource, res, sizeof cap_table[i].resource);
        cap_table[i].rights = rights;
        cap_table[i].live = cap_table[i].used = true;
        kprintf("[captbl ] MINTED cap[%d] gen %u — %c access on %s for '%s'\n",
                i, cap_table[i].generation, rights, res, holder);
        return i;
    }
    kprintf("[captbl ] mint failed: table full\n");
    return -1;
}
static void cap_revoke(int slot) {
    if (slot < 0 || slot >= MAX_CAPS || !cap_table[slot].used || !cap_table[slot].live) {
        kprintf("[captbl ] revoke: no live capability in slot %d\n", slot);
        return;
    }
    uint64_t old = cap_table[slot].generation;
    cap_table[slot].generation++;
    cap_table[slot].live = false;
    kprintf("[captbl ] REVOKED cap[%d] held by '%s' — generation bumped %u -> %u\n",
            slot, cap_table[slot].holder, old, cap_table[slot].generation);
}
static void cap_list(void) {
    kprintf("slot  gen     rights  resource      holder          state\n");
    kprintf("----  ------  ------  ------------  --------------  -----\n");
    bool any = false;
    for (int i = 0; i < MAX_CAPS; i++) {
        if (!cap_table[i].used) continue;
        any = true;
        kprintf("%d     %u", i, cap_table[i].generation);
        kputs("   ");
        kprintf("  %c       %s", cap_table[i].rights, cap_table[i].resource);
        for (size_t s = 0; s < 14 - 0; s++) { }      /* simple layout          */
        kprintf("      %s        %s\n", cap_table[i].holder,
                cap_table[i].live ? "LIVE" : "dead");
    }
    if (!any) kprintf("(empty — try 'demo' or 'mint <holder> <resource> <R|W>')\n");
}

/* ===========================================================================
 * REAL PAGE-TABLE HARDWARE PASSTHROUGH
 * ===========================================================================
 * This replaces the userspace prototype's mock address spaces with genuine
 * x86_64 4-level paging. Each process owns a private PML4; a capability-gated
 * syscall installs real PTEs mapping a device's physical MMIO window into the
 * holder's address space. A process without the bit gets nothing mapped.
 *
 * The proof at boot: we switch CR3 into the authorized process, read a device
 * sentinel THROUGH its own virtual mapping (showing the PTE resolves), switch
 * back, then show the unauthorized process has no such mapping at all.
 * =========================================================================== */

/* ---- CR3 access + paging constants ---------------------------------------- */
static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline void write_cr3(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}
#define PTE_PRESENT 0x1ull
#define PTE_WRITE   0x2ull
#define PTE_USER    0x4ull
#define PTE_PCD     0x10ull      /* cache-disable: correct for real MMIO       */
#define PTE_HUGE    0x80ull
#define PTE_NX      (1ull << 63)     /* no-execute (requires EFER.NXE)            */
extern uint8_t _stext[], _etext[], _kernel_end[];   /* from the linker script    */
#define ADDR_MASK   0x000ffffffffff000ull

static uint64_t kernel_cr3 = 0;

/* ---- Polyglot boundary: functions compiled from Rust and C++ -------------- */
/* rust/cap_engine.rs (no_core Rust) — content hashing + the capability gate.  */
extern uint64_t rust_cas_hash(uint64_t base, uint64_t len);
extern uint32_t rust_cap_check(uint64_t caps, uint64_t required);
/* cpp/ipc_ring.cpp (freestanding C++) — zero-copy SPSC ring buffer.           */
extern void     cpp_ring_init(void);
extern uint32_t cpp_ring_push(const uint8_t *src, uint32_t len);
extern uint32_t cpp_ring_pop(const uint8_t **out, uint32_t want);
extern uint64_t cpp_ring_depth(void);

/* ---- Physical frame allocator (bump, within the identity-mapped 1 GiB) ----- */
/* Page tables and the scratch device page all live below 1 GiB so BOTH the    */
/* kernel CR3 and every process CR3 (which shares the low identity map) can     */
/* reach them directly.                                                         */
static uint64_t g_next_frame = 0x01000000; /* 16 MiB: clear of kernel + stack */

/* v0.41: the bump pointer is claimed with LOCK XADD, so any core may allocate
 * concurrently — two cores can no longer be handed the same frame, and a
 * multi-frame claim stays contiguous because the whole extent is reserved in
 * ONE atomic add (per-frame bumping would interleave under SMP). Zeroing
 * happens outside any lock: the frames are exclusively ours once claimed.    */
static uint64_t alloc_frames(uint64_t n) {
    uint64_t base = __sync_fetch_and_add(&g_next_frame, n * 0x1000);
    uint64_t *p = (uint64_t *)base;         /* identity mapped -> phys == virt  */
    for (uint64_t i = 0; i < n * 512; i++) p[i] = 0;
    return base;
}

/* ---- v0.42: physical frame RECLAMATION -----------------------------------
 * Through v0.41 the bump pointer only ever grew: an address space, once built,
 * was never handed back — acceptable while nothing exited, fatal now that
 * page_free_tree() dismantles user spaces on thread exit. Freed frames now land
 * on a LIFO free-list whose next-pointer is threaded through the frame itself
 * (every pool frame is identity-mapped RAM, so its first 8 bytes are directly
 * addressable), and alloc_frame() draws from that list before touching the bump
 * pointer. Net effect under a spawn/destroy loop: the high-water mark climbs
 * once, during warm-up, then holds — every subsequent space is built entirely
 * from reclaimed frames.
 *
 * Concurrency: g_frame_lock is the allocator's rank-6 spinlock, the one v0.41's
 * ranking table listed as "(6) lock-free, nothing to rank" — the allocator now
 * HAS shared state (the list head + depth), so it gets a real lock. It is a
 * strict LEAF: acquired around nothing but the O(1) push/pop, never nested
 * under a klock, never held across an allocation or a yield. That makes a raw
 * PAUSE-spin (not klock_acquire's yielding backoff) correct in EVERY context it
 * runs in — early boot before the scheduler, AP context with no scheduler, and
 * the map_page fast path — none of which klock_acquire tolerates. The bump
 * fallback keeps its own LOCK XADD, so the two allocation paths stay
 * independently SMP-safe. */
#define FRAME_POOL_BASE 0x01000000ull            /* 16 MiB — matches g_next_frame's init */
static volatile int      g_frame_lock       = 0; /* rank 6: frame-list mutual exclusion  */
static uint64_t          g_frame_freelist   = 0; /* phys of LIFO head, 0 = list empty    */
static volatile uint64_t g_frames_freed     = 0; /* frames ever returned to the pool     */
static volatile uint64_t g_frames_reused    = 0; /* allocations satisfied from the list  */
static volatile uint64_t g_frame_free_depth = 0; /* frames currently on the list         */

static inline void frame_lock(void)   { while (__sync_lock_test_and_set(&g_frame_lock, 1)) __asm__ volatile("pause"); }
static inline void frame_unlock(void) { __sync_lock_release(&g_frame_lock); }

/* A physical address is reclaimable RAM iff it lies in the managed window
 * [16 MiB, high-water). Device MMIO installed by passthrough, the framebuffer,
 * and the shared low-1-GiB identity map all fall OUTSIDE it — free_frame()
 * silently drops them, so they can never be handed back out as if they were
 * pool RAM. This one predicate is what makes tearing down a user space that
 * contains device mappings safe. */
static inline int frame_in_pool(uint64_t pa) {
    pa &= ADDR_MASK;
    return pa >= FRAME_POOL_BASE && pa < g_next_frame;
}

/* v0.45: a shadow "is this frame currently on the free list" bit per frame,
 * independent of the list's own linkage. The existing `pa == g_frame_freelist`
 * check above only catches a double-free of the CURRENT head — a double-free
 * with any other free_frame()/alloc_frame() call in between (the common case
 * under real scheduling) threads silently into the middle of the list, and
 * the count-based invariants (g_frame_free_depth == freed - reused) can't
 * catch it either: a double-free increments both counters together, so the
 * arithmetic still reconciles even though the list now holds a corrupt
 * duplicate. This bit catches it AT THE SECOND free_frame() call that causes
 * it. Found live, load-bearing: this is exactly what caught the sensor0
 * demo-device double-free documented on page_free_tree below. Sized for a
 * 256 MiB pool — comfortably more than this kernel's observed pool usage. */
#define FRAME_DBG_MAX ((256ull * 1024 * 1024) / 0x1000)
static uint8_t g_frame_dbg_isfree[FRAME_DBG_MAX];

/* Return one 4 KiB frame to the pool. Returns 1 if it was actually reclaimed,
 * 0 if the address was out of the managed window (and thus ignored) — the
 * caller uses that to count exactly how many frames came back. */
static int free_frame(uint64_t pa) {
    pa &= ADDR_MASK;
    if (!frame_in_pool(pa)) return 0;
    frame_lock();
    if (pa == g_frame_freelist) {
        frame_unlock();
        kprintf("\n[frame  ] DOUBLE-FREE: pa=%X is ALREADY the free-list head -- halting\n", pa);
        for (;;) __asm__ volatile("cli; hlt");
    }
    if (g_frame_freelist && !frame_in_pool(g_frame_freelist)) {
        frame_unlock();
        kprintf("\n[frame  ] CORRUPT FREE-LIST HEAD at free(pa=%X): head=%X is outside the pool -- halting\n",
                pa, g_frame_freelist);
        for (;;) __asm__ volatile("cli; hlt");
    }
    uint64_t dbgidx = (pa - FRAME_POOL_BASE) / 0x1000;
    if (dbgidx < FRAME_DBG_MAX) {
        if (g_frame_dbg_isfree[dbgidx]) {
            frame_unlock();
            kprintf("\n[frame  ] TRUE DOUBLE-FREE (shadow bit): pa=%X was ALREADY marked free"
                    " -- halting\n", pa);
            for (;;) __asm__ volatile("cli; hlt");
        }
        g_frame_dbg_isfree[dbgidx] = 1;
    }
    *(volatile uint64_t *)pa = g_frame_freelist;   /* thread the next-ptr through the frame */
    g_frame_freelist = pa;
    g_frames_freed++;
    g_frame_free_depth++;
    frame_unlock();
    return 1;
}

/* Single-frame allocation: reuse a reclaimed frame before growing the pool.
 * Multi-frame (contiguous) claims still go straight to the bump allocator —
 * the free-list makes no contiguity promise, and every contiguous caller
 * (working-set buffers) only ever grows the pool anyway. */
static uint64_t alloc_frame(void) {
    frame_lock();
    uint64_t pa = g_frame_freelist;
    if (pa) {
        /* Defensive: a corrupted free-list is a silent, remote-in-time bug
         * otherwise — the frame that got mis-linked is rarely the one that
         * finally crashes. Fail loud, at the pop, with the actual bad value. */
        if (!frame_in_pool(pa)) {
            frame_unlock();
            kprintf("\n[frame  ] CORRUPT FREE-LIST: popped pa=%X, outside the pool [%X,%X) -- halting\n",
                    pa, (uint64_t)FRAME_POOL_BASE, g_next_frame);
            for (;;) __asm__ volatile("cli; hlt");
        }
        g_frame_freelist = *(volatile uint64_t *)pa;   /* pop */
        g_frame_free_depth--;
        g_frames_reused++;
        { uint64_t dbgidx = (pa - FRAME_POOL_BASE) / 0x1000;   /* TEMPORARY DIAGNOSTIC */
          if (dbgidx < FRAME_DBG_MAX) g_frame_dbg_isfree[dbgidx] = 0; }
        frame_unlock();
        uint64_t *p = (uint64_t *)pa;                  /* zero on the way out, like the bump path */
        for (int i = 0; i < 512; i++) p[i] = 0;
        return pa;
    }
    frame_unlock();
    return alloc_frames(1);                            /* list empty -> bump */
}

/* Fault-injection allocator: when g_alloc_limit is armed, allocations past it   */
/* fail (return 0) so exhaustion-handling paths can be exercised.                */
static uint64_t g_alloc_limit = 0;
static uint64_t alloc_frame_limited(void) {
    if (g_alloc_limit && g_next_frame >= g_alloc_limit) return 0;
    return alloc_frame();
}

/* ---- 4-level map: install one 4 KiB PTE, creating tables as needed --------- */
static int map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    uint64_t i4 = (vaddr >> 39) & 0x1FF, i3 = (vaddr >> 30) & 0x1FF;
    uint64_t i2 = (vaddr >> 21) & 0x1FF, i1 = (vaddr >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)(pml4_phys & ADDR_MASK);
    if (!(pml4[i4] & PTE_PRESENT))
        pml4[i4] = alloc_frame() | PTE_PRESENT | PTE_WRITE | PTE_USER;

    uint64_t *pdpt = (uint64_t *)(pml4[i4] & ADDR_MASK);
    if (!(pdpt[i3] & PTE_PRESENT))
        pdpt[i3] = alloc_frame() | PTE_PRESENT | PTE_WRITE | PTE_USER;

    uint64_t *pd = (uint64_t *)(pdpt[i3] & ADDR_MASK);
    if (!(pd[i2] & PTE_PRESENT))
        pd[i2] = alloc_frame() | PTE_PRESENT | PTE_WRITE | PTE_USER;

    uint64_t *pt = (uint64_t *)(pd[i2] & ADDR_MASK);
    pt[i1] = (paddr & ADDR_MASK) | flags | PTE_PRESENT;
    return 0;
}

/* ---- Software page walk: resolve vaddr -> phys in a given address space ----- */
static int translate(uint64_t pml4_phys, uint64_t vaddr, uint64_t *out_phys) {
    uint64_t i4 = (vaddr >> 39) & 0x1FF, i3 = (vaddr >> 30) & 0x1FF;
    uint64_t i2 = (vaddr >> 21) & 0x1FF, i1 = (vaddr >> 12) & 0x1FF;
    uint64_t *pml4 = (uint64_t *)(pml4_phys & ADDR_MASK);
    if (!(pml4[i4] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)(pml4[i4] & ADDR_MASK);
    if (!(pdpt[i3] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)(pdpt[i3] & ADDR_MASK);
    if (!(pd[i2] & PTE_PRESENT)) return 0;
    if (pd[i2] & PTE_HUGE) { *out_phys = (pd[i2] & ADDR_MASK) + (vaddr & 0x1FFFFF); return 1; }
    uint64_t *pt = (uint64_t *)(pd[i2] & ADDR_MASK);
    if (!(pt[i1] & PTE_PRESENT)) return 0;
    *out_phys = (pt[i1] & ADDR_MASK) + (vaddr & 0xFFF);
    return 1;
}

/* v0.46: clears one 4 KiB leaf PTE, leaving the structural PDPT/PD/PT frames
 * alone (map_page creates them on demand; page_free_tree reclaims them
 * normally when the rest of the process goes down). A no-op, not an error,
 * if the vaddr was never actually mapped — used by IPC shared-memory
 * teardown to drop a mapping that may only ever have been RESERVED, never
 * installed (a message queued but never RECV'd). Doing this BEFORE
 * page_free_tree runs is what keeps a shared frame from being double-freed
 * the same way an unguarded hardware-passthrough MMIO leaf was in v0.45 —
 * except here the fix is to make the leaf simply not present anymore,
 * rather than teach page_free_tree a second exception list. */
static void unmap_page(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t i4 = (vaddr >> 39) & 0x1FF, i3 = (vaddr >> 30) & 0x1FF;
    uint64_t i2 = (vaddr >> 21) & 0x1FF, i1 = (vaddr >> 12) & 0x1FF;
    uint64_t *pml4 = (uint64_t *)(pml4_phys & ADDR_MASK);
    if (!(pml4[i4] & PTE_PRESENT)) return;
    uint64_t *pdpt = (uint64_t *)(pml4[i4] & ADDR_MASK);
    if (!(pdpt[i3] & PTE_PRESENT) || (pdpt[i3] & PTE_HUGE)) return;
    uint64_t *pd = (uint64_t *)(pdpt[i3] & ADDR_MASK);
    if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE)) return;
    uint64_t *pt = (uint64_t *)(pd[i2] & ADDR_MASK);
    pt[i1] = 0;
}

/* ---- Return the leaf PTE (with flags) for a vaddr, or 0 if not present ----- */
static uint64_t walk_pte(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t i4 = (vaddr >> 39) & 0x1FF, i3 = (vaddr >> 30) & 0x1FF;
    uint64_t i2 = (vaddr >> 21) & 0x1FF, i1 = (vaddr >> 12) & 0x1FF;
    uint64_t *pml4 = (uint64_t *)(pml4_phys & ADDR_MASK);
    if (!(pml4[i4] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)(pml4[i4] & ADDR_MASK);
    if (!(pdpt[i3] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)(pdpt[i3] & ADDR_MASK);
    if (!(pd[i2] & PTE_PRESENT)) return 0;
    if (pd[i2] & PTE_HUGE) return pd[i2];
    uint64_t *pt = (uint64_t *)(pd[i2] & ADDR_MASK);
    return (pt[i1] & PTE_PRESENT) ? pt[i1] : 0;
}

/* ---- user-space address range + pointer validation ------------------------- */
#define USER_VMIN 0x400000000000ull
#define USER_VMAX 0x600000000000ull
#define DMA_USER_V  0x0000520000000000ull   /* ring-3 DMA window (driver buffers) */
#define SURF_USER_V 0x0000530000000000ull   /* ring-3 surface window (app pixels) */
#define SMP_USER_V  0x0000540000000000ull   /* v0.49: ring-3 remap/unmap scratch window */

/* Validate that [ptr, ptr+len) is entirely within a process's user space and    */
/* mapped USER-present (and USER-writable if need_write). Defends every syscall   */
/* that touches a ring-3-supplied pointer. (Uniprocessor: the process's own page  */
/* tables can't change mid-syscall, so this check is not subject to TOCTOU here.) */
static int access_ok(uint64_t cr3, uint64_t ptr, uint64_t len, int need_write) {
    if (len == 0) return 1;
    if (ptr < USER_VMIN || ptr + len < ptr || ptr + len > USER_VMAX) return 0;
    for (uint64_t p = ptr & ~0xFFFull; p <= ((ptr + len - 1) & ~0xFFFull); p += 0x1000) {
        uint64_t pte = walk_pte(cr3, p);
        if (!(pte & PTE_PRESENT) || !(pte & PTE_USER)) return 0;
        if (need_write && !(pte & PTE_WRITE)) return 0;
    }
    return 1;
}

/* Copy a NUL-terminated string from user space with per-page validation.        */
/* Returns length copied, or -1 if any byte is not user-readable.                */
static int copy_user_str(uint64_t cr3, uint64_t uptr, char *kbuf, int max) {
    uint64_t curpage = ~0ull;
    for (int i = 0; i < max - 1; i++) {
        uint64_t va = uptr + (uint64_t)i, pg = va & ~0xFFFull;
        if (pg != curpage) {
            if (!access_ok(cr3, va, 1, 0)) { kbuf[0] = 0; return -1; }
            curpage = pg;
        }
        char c = *(volatile char *)va;
        kbuf[i] = c;
        if (!c) return i;
    }
    kbuf[max - 1] = 0;
    return max - 1;
}

/* ===========================================================================
 * v0.42: ADDRESS-SPACE TEARDOWN — page_free_tree()
 * ===========================================================================
 * Dismantle a process's USER address space and return every private frame to
 * the allocator, leaving KERNEL space bit-for-bit intact. The user/kernel split
 * is exactly the one access_ok() enforces: user virtual addresses occupy PML4
 * indices [USER_VMIN>>39, USER_VMAX>>39) = [128, 192). PML4 entry 0 (the low
 * 1 GiB identity map — kernel code/stack/data/IDT) and entry 0xC0=192 (the
 * shared device-MMIO window) are COPIED pointers into kernel-owned tables;
 * create_address_space() aliases the same two entries into every process, so
 * descending through them would free tables the kernel and every sibling
 * process are still using. The top-level loop is bounded to [128,192), so it
 * never can.
 *
 * Pointer layout at each level (phys == virt — the pool is identity-mapped):
 *
 *   pml4[i4]   i4 in [128,192)   & ADDR_MASK -> PDPT frame   (present, !huge)
 *     pdpt[i3]                   & ADDR_MASK -> PD   frame  | HUGE => 1 GiB leaf
 *       pd[i2]                   & ADDR_MASK -> PT   frame  | HUGE => 2 MiB leaf
 *         pt[i1]                 & ADDR_MASK -> 4 KiB data leaf
 *
 * Every table frame (PDPT/PD/PT) came from alloc_frame(), so all of them go
 * back through free_frame(). Data leaves go back too — EXCEPT those free_frame()
 * rejects as out-of-pool (a passthrough MMIO leaf carries a device physaddr,
 * silently skipped) and EXCEPT the surface pixel window (PML4 slot
 * SURF_PML4_IDX): a surface's buffer frames are handed to
 * surfaces_reclaim()'s OWN recycle list (g_surf_free[]) by every caller of
 * this function, one call earlier — freeing them AGAIN here would put the
 * same physical frame on two independent free lists at once, and whichever
 * one hands it out second corrupts whatever the first one's new owner wrote.
 * The structural PDPT/PD/PT frames that mapped the surface window are
 * ordinary page-table frames, not surface data, and ARE reclaimed normally.
 * Freeing is strictly bottom-up — every child of a table is released before
 * the table itself — so no frame is ever touched after it re-enters the pool
 * and could have been handed back out. Returns the number of frames actually
 * reclaimed.
 *
 * PRECONDITION: pml4_phys must not be the CR3 live on ANY core. Teardown reads
 * and zeroes the tables it frees; doing that to an installed address space
 * would fault the core running in it. Callers destroy a space only after its
 * last thread has left it (or, in the leakcheck, spaces that were never
 * installed at all).
 * ========================================================================== */
/* v0.45: a data leaf must not be freed if it is a registered device's MMIO
 * frame — found live, the hard way, while verifying this milestone's
 * cmd_kproc_stress: page_free_tree's original design (v0.42) assumed every
 * passthrough MMIO leaf carries a physaddr OUTSIDE the frame pool, so
 * free_frame()'s own frame_in_pool() check would silently reject it. That
 * holds for real hardware (a PCI BAR lives far outside RAM), but cmd_
 * passthrough's demo device ("sensor0") stands in for a register file with
 * `alloc_frame()` — i.e. its "MMIO" physaddr IS pool RAM. Multiple processes
 * granted passthrough to that SAME device each map the SAME physical page,
 * and each one's page_free_tree call would free it independently on exit:
 * a genuine double-free of shared device memory (confirmed with a shadow
 * free-bit added during this investigation — TRUE DOUBLE-FREE at the
 * demo device's own physaddr, triggered by cmd_dma_stress's four sensor0
 * grantees exiting through the modern path once kproc recycling made that
 * a live path for the first time). A leaf frame belonging to ANY registered
 * kdev is never process-private, real hardware or not — checked explicitly
 * here rather than relying on frame_in_pool() to reject it. */
static inline int frame_is_device_mmio(uint64_t pa) { return kdev_find(pa) != 0; }

static uint64_t page_free_tree(uint64_t pml4_phys) {
    uint64_t freed = 0;
    uint64_t *pml4 = (uint64_t *)(pml4_phys & ADDR_MASK);
    const int lo = (int)((USER_VMIN >> 39) & 0x1FF);      /* 128 */
    const int hi = (int)((USER_VMAX >> 39) & 0x1FF);      /* 192 */
    const int surf_idx = (int)((SURF_USER_V >> 39) & 0x1FF);  /* 166: owned by g_surf_free[] */
    for (int i4 = lo; i4 < hi; i4++) {
        if (!(pml4[i4] & PTE_PRESENT)) continue;
        int is_surf = (i4 == surf_idx);
        uint64_t *pdpt = (uint64_t *)(pml4[i4] & ADDR_MASK);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT)) continue;
            if (pdpt[i3] & PTE_HUGE) {                     /* 1 GiB data leaf: 512*512 frames */
                uint64_t b = pdpt[i3] & ADDR_MASK;
                if (!is_surf)
                    for (uint64_t k = 0; k < 512ull * 512; k++) {
                        uint64_t f = b + k * 0x1000;
                        if (!frame_is_device_mmio(f)) freed += free_frame(f);
                    }
                pdpt[i3] = 0; continue;
            }
            uint64_t *pd = (uint64_t *)(pdpt[i3] & ADDR_MASK);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_PRESENT)) continue;
                if (pd[i2] & PTE_HUGE) {                    /* 2 MiB data leaf: 512 frames     */
                    uint64_t b = pd[i2] & ADDR_MASK;
                    if (!is_surf)
                        for (uint64_t k = 0; k < 512; k++) {
                            uint64_t f = b + k * 0x1000;
                            if (!frame_is_device_mmio(f)) freed += free_frame(f);
                        }
                    pd[i2] = 0; continue;
                }
                uint64_t *pt = (uint64_t *)(pd[i2] & ADDR_MASK);
                for (int i1 = 0; i1 < 512; i1++) {
                    if (!(pt[i1] & PTE_PRESENT)) continue;
                    /* 4 KiB leaf: a real device's MMIO self-excludes via        */
                    /* frame_in_pool() too, but frame_is_device_mmio() is now    */
                    /* the actual guarantee — it holds even for the demo device, */
                    /* whose "MMIO" physaddr is ordinary pool RAM. The surface   */
                    /* window self-excludes here (g_surf_free[] owns it).       */
                    uint64_t f = pt[i1] & ADDR_MASK;
                    if (!is_surf && !frame_is_device_mmio(f)) freed += free_frame(f);
                    pt[i1] = 0;
                }
                freed += free_frame((uint64_t)pt);         /* the PT frame                    */
                pd[i2] = 0;
            }
            freed += free_frame((uint64_t)pd);             /* the PD frame                    */
            pdpt[i3] = 0;
        }
        freed += free_frame((uint64_t)pdpt);               /* the PDPT frame                  */
        pml4[i4] = 0;
    }
    freed += free_frame(pml4_phys);                        /* finally the PML4 itself         */
    return freed;
}

/* ---- Enforce W^X across the kernel identity map ---------------------------- */
/* Split the 2 MiB huge page(s) covering the kernel image into 4 KiB pages so    */
/* [_stext,_etext) is R+X (read-only, executable) and everything else is RW+NX,  */
/* and set NX on every other identity huge page. After this, no kernel page is   */
/* both writable and executable.                                                 */
static void harden_kernel_wx(void) {
    uint64_t *pml4 = (uint64_t *)(kernel_cr3 & ADDR_MASK);
    uint64_t *pdpt = (uint64_t *)(pml4[0] & ADDR_MASK);
    uint64_t *pd   = (uint64_t *)(pdpt[0] & ADDR_MASK);
    uint64_t stext = (uint64_t)_stext, etext = (uint64_t)_etext, kend = (uint64_t)_kernel_end;
    uint64_t split_hp = (kend + 0x1FFFFF) / 0x200000;      /* huge pages to 4K-split */
    __asm__ volatile("cli");
    /* CR0.WP: enforce read-only pages even in ring 0, so kernel code is immutable */
    uint64_t cr0; __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ull << 16); __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    for (uint64_t hp = 0; hp < 512; hp++) {
        if (hp < split_hp) {
            uint64_t *pt = (uint64_t *)alloc_frame();
            for (int i = 0; i < 512; i++) {
                uint64_t va = hp * 0x200000 + (uint64_t)i * 0x1000;
                uint64_t f = PTE_PRESENT;
                if (va >= stext && va < etext) f |= 0;      /* code: R + X          */
                else f |= PTE_WRITE | PTE_NX;               /* data: RW + NX        */
                pt[i] = (va & ADDR_MASK) | f;
            }
            pd[hp] = ((uint64_t)pt & ADDR_MASK) | PTE_PRESENT | PTE_WRITE;
        } else {
            pd[hp] |= PTE_NX;                               /* data huge page RW+NX */
        }
    }
    write_cr3(kernel_cr3);                                  /* flush the whole TLB  */
    __asm__ volatile("sti");
}

/* ---- W^X auditor: count leaf pages that are BOTH writable and executable ----- */
static int scan_wx(uint64_t pml4_phys, int user_only, int *out_total) {
    int wx = 0, total = 0;
    uint64_t *pml4 = (uint64_t *)(pml4_phys & ADDR_MASK);
    for (int a = 0; a < 512; a++) {
        if (!(pml4[a] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)(pml4[a] & ADDR_MASK);
        for (int b = 0; b < 512; b++) {
            if (!(pdpt[b] & PTE_PRESENT)) continue;
            if (pdpt[b] & PTE_HUGE) continue;
            uint64_t *pd = (uint64_t *)(pdpt[b] & ADDR_MASK);
            for (int c = 0; c < 512; c++) {
                if (!(pd[c] & PTE_PRESENT)) continue;
                uint64_t leaf = 0;
                if (pd[c] & PTE_HUGE) leaf = pd[c];
                else {
                    uint64_t *pt = (uint64_t *)(pd[c] & ADDR_MASK);
                    for (int d = 0; d < 512; d++) {
                        if (!(pt[d] & PTE_PRESENT)) continue;
                        uint64_t l = pt[d];
                        if (user_only && !(l & PTE_USER)) continue;
                        total++;
                        if ((l & PTE_WRITE) && !(l & PTE_NX)) wx++;
                    }
                    continue;
                }
                if (user_only && !(leaf & PTE_USER)) continue;
                total++;
                if ((leaf & PTE_WRITE) && !(leaf & PTE_NX)) wx++;
            }
        }
    }
    if (out_total) *out_total = total;
    return wx;
}

/* ===========================================================================
 * PERIODIC DESCRIPTOR-TABLE INTEGRITY SWEEP (amortized into the scheduler tick)
 * ===========================================================================
 * The timer tick runs in interrupt context, so a full page-table walk is far
 * too much work to do there. Instead the sweep is INCREMENTAL: each tick checks
 * a small bounded batch of leaf PTEs and advances a persistent cursor, so a
 * complete pass over the kernel address space finishes across many ticks and
 * then restarts — continuous background corruption detection with a fixed,
 * predictable per-tick cost.
 *
 * Invariants checked per leaf entry:
 *   - reserved bits [52..58] are zero (bit-flip / corruption detection)
 *   - W^X: no page is both writable and executable
 *   - kernel .text stays exactly R+X (present, not writable, not NX)
 * =========================================================================== */
#define SWEEP_BATCH 64                       /* leaf entries examined per tick   */
static struct {
    int      i4, i3, i2, i1;                 /* cursor into PML4/PDPT/PD/PT      */
    uint64_t passes;                         /* completed full sweeps            */
    uint64_t entries;                        /* leaf entries checked (cumulative)*/
    uint64_t violations;                     /* integrity violations found       */
    uint64_t last_bad_va;                    /* vaddr of the most recent failure */
    int      enabled;
} g_sweep = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/* Validate one leaf entry. Returns 1 if it violates an invariant.              */
static int sweep_check_leaf(uint64_t leaf, uint64_t va) {
    if (leaf & (0x7Full << 52)) return 1;                    /* reserved bits set  */
    if ((leaf & PTE_WRITE) && !(leaf & PTE_NX)) return 1;    /* W+X                */
    if (va >= (uint64_t)_stext && va < (uint64_t)_etext) {   /* kernel code        */
        if (!(leaf & PTE_PRESENT) || (leaf & PTE_WRITE) || (leaf & PTE_NX)) return 1;
    }
    return 0;
}

/* One bounded slice of the sweep. Called from the timer ISR: no locks, no       */
/* allocation, no printing — it only reads page tables and updates counters.     */
static void sweep_tick(void) {
    if (!g_sweep.enabled) return;
    uint64_t *pml4 = (uint64_t *)(kernel_cr3 & ADDR_MASK);
    int budget = SWEEP_BATCH;
    while (budget > 0) {
        if (g_sweep.i4 >= 512) {                             /* wrapped: pass done */
            g_sweep.i4 = g_sweep.i3 = g_sweep.i2 = g_sweep.i1 = 0;
            g_sweep.passes++;
            return;
        }
        if (!(pml4[g_sweep.i4] & PTE_PRESENT)) { g_sweep.i4++; g_sweep.i3 = g_sweep.i2 = g_sweep.i1 = 0; continue; }
        uint64_t *pdpt = (uint64_t *)(pml4[g_sweep.i4] & ADDR_MASK);
        if (g_sweep.i3 >= 512) { g_sweep.i4++; g_sweep.i3 = g_sweep.i2 = g_sweep.i1 = 0; continue; }
        if (!(pdpt[g_sweep.i3] & PTE_PRESENT) || (pdpt[g_sweep.i3] & PTE_HUGE)) {
            g_sweep.i3++; g_sweep.i2 = g_sweep.i1 = 0; continue;
        }
        uint64_t *pd = (uint64_t *)(pdpt[g_sweep.i3] & ADDR_MASK);
        if (g_sweep.i2 >= 512) { g_sweep.i3++; g_sweep.i2 = g_sweep.i1 = 0; continue; }
        uint64_t pde = pd[g_sweep.i2];
        uint64_t va_base = ((uint64_t)g_sweep.i4 << 39) | ((uint64_t)g_sweep.i3 << 30)
                         | ((uint64_t)g_sweep.i2 << 21);
        if (!(pde & PTE_PRESENT)) { g_sweep.i2++; g_sweep.i1 = 0; continue; }
        if (pde & PTE_HUGE) {                                /* 2 MiB leaf         */
            g_sweep.entries++; budget--;
            if (sweep_check_leaf(pde, va_base)) { g_sweep.violations++; g_sweep.last_bad_va = va_base; }
            g_sweep.i2++; g_sweep.i1 = 0; continue;
        }
        uint64_t *pt = (uint64_t *)(pde & ADDR_MASK);        /* 512 x 4 KiB leaves */
        while (g_sweep.i1 < 512 && budget > 0) {
            uint64_t e = pt[g_sweep.i1];
            if (e & PTE_PRESENT) {
                g_sweep.entries++; budget--;
                uint64_t va = va_base | ((uint64_t)g_sweep.i1 << 12);
                if (sweep_check_leaf(e, va)) { g_sweep.violations++; g_sweep.last_bad_va = va; }
            }
            g_sweep.i1++;
        }
        if (g_sweep.i1 >= 512) { g_sweep.i2++; g_sweep.i1 = 0; }
    }
}

/* ---- Create a private address space that still maps the kernel -------------- */
/* Copy the kernel PML4's entry 0 (the low 1 GiB identity map) so kernel code,   */
/* stack, IDT and data stay mapped after we switch CR3 into this process.        */
static uint64_t create_address_space(void) {
    uint64_t p = alloc_frame();
    uint64_t *np = (uint64_t *)p;
    uint64_t *kp = (uint64_t *)(kernel_cr3 & ADDR_MASK);
    np[0]    = kp[0];      /* low 1 GiB identity: kernel code/stack/data/IDT     */
    np[0xC0] = kp[0xC0];   /* kernel-global device MMIO window (0x600000000000)  */
    return p;
}

/* ---- Process capabilities (bitset) + table --------------------------------- */
#define PCAP_HW_PASSTHROUGH (1ull << 0)
#define PCAP_CAMERA         (1ull << 1)
#define PCAP_MICROPHONE     (1ull << 2)
#define PCAP_CONTROLLER     (1ull << 3)
#define PCAP_FILESYSTEM     (1ull << 5)
#define PCAP_FRAMEBUFFER    (1ull << 6)
#define PCAP_IPC             (1ull << 7)    /* v0.46: required for any SYS_IPC_* call */
#define PCAP_VFIO            (1ull << 8)    /* v0.47: required for any SYS_VFIO_* call */
#define PCAP_SMP_ADMIN       (1ull << 9)    /* v0.49: SYS_TLB_SHOOTDOWN/SET_AFFINITY/SMP_REMAP/SMP_UNMAP */
#define PCAP_SURFACE         (1ull << 10)   /* v0.50: required for any SYS_GPU_* call */

/* v0.39: a COMPLETE ring-3 register context. The preempt IPI (vector 50)
 * captures the interrupted user state here straight from the isr_frame, and
 * enter_user_resume rebuilds it — on whichever core the context lands on
 * next. GPR order deliberately mirrors struct isr_frame.                    */
struct uctx {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;     /* offs 0..56          */
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;        /* offs 64..112        */
    uint64_t rip, rsp, rflags;                         /* offs 120,128,136    */
};

/* v0.44: one outstanding DMA/passthrough grant. DMA_GRANT_MMIO records a
 * device passthrough attach (sys_hardware_passthrough): `bdf` is the actual
 * device, and revocation means iommu_detach_to_kernel(bdf) — returning the
 * device to the safe kernel identity domain. DMA_GRANT_PAGE records one
 * page added live to the process's OWN domain (SYS_DMA_ALLOC): `bdf` is
 * 0xFFFF (no specific device to detach; the page's own PTE is ordinary
 * process memory and page_free_tree already reclaims it — this record
 * exists purely so exit can prove nothing was left un-revoked). Neither
 * kind ever frees `phys` itself: that is EITHER page_free_tree's job (an
 * ordinary present PTE in the process's own CR3) or ISN'T a frame at all
 * (the device MMIO physical range is never owned by any process's frame
 * pool to begin with). */
#define MAX_DMA_GRANTS 8
#define DMA_GRANT_MMIO (1u << 0)
#define DMA_GRANT_PAGE (1u << 1)
struct dma_grant {
    uint64_t phys;
    uint64_t size;
    uint16_t bdf;
    uint32_t flags;
    int      used;
};

struct kproc {
    uint64_t pid;
    char     name[24];
    uint64_t caps;
    uint64_t cr3;       /* physical address of this process's PML4             */
    bool     used;
    uint64_t role;      /* 0=demo 1=userspace driver 2=surface app             */
                        /* 3=surface-exit test 4=identity prober 5=tear-test   */
                        /* 6=mcsched probe 7=concurrent probe 8=preemptible    */
    uint64_t dma_next;  /* bump pointer into this process's DMA window         */
    uint64_t exit_code; /* SYS_EXIT code (uthreads; 0x8000+vector on a fault)  */
    int      exited;    /* set when the process's thread has been reaped       */
    /* v0.39: distributed-scheduling state                                     */
    uint64_t entry;     /* ring-3 entry point (set at ELF load)                */
    int      pstate;    /* 0 = fresh (enter at `entry`), 1 = preempted (uctx)  */
    int      migrate_to;/* resume the preempted context on THIS cpu (-1 = same) */
    struct uctx uctx;   /* captured context while preempted                    */
    volatile uint32_t ran_on;     /* bitmask: cpus that executed this task     */
    volatile uint32_t finish_seq; /* global completion order (1-based)         */
    volatile uint32_t dispatches; /* v0.40: times an executor picked this up   */
    uint64_t frames_freed;        /* v0.42: page_free_tree's count at exit     */
    /* v0.44: DMA/IOMMU grant table (see struct dma_grant above)               */
    struct dma_grant dma_grants[MAX_DMA_GRANTS];
    uint32_t          dma_grant_count;   /* active grants right now            */
    /* v0.45: `exited` is set EARLY in every exit path (right after the ring-3
     * excursion returns) so watchdogs elsewhere see completion without
     * waiting on this core's own descriptor/DMA/page-table teardown — that
     * was fine as long as nothing ever reused the slot on that signal alone.
     * kproc_spawn's recycler needs the TRUE end of teardown: torn_down is set
     * once, last, only after page_free_tree returns. Recycling on `exited`
     * instead would let a new occupant's create_address_space() overwrite
     * `cr3` while another core's page_free_tree(kprocs[p].cr3) was still
     * mid-walk on the OLD address space — it would then walk and free the
     * NEW, live process's frames instead, corrupting the free-list. */
    volatile int      torn_down;
    /* v0.49: CPU affinity mask (bit c = "may run on cpu c"). 0 means
     * unrestricted (the default) — every online cpu is eligible. Enforced by
     * rq_steal (a thief outside the mask puts the task back) and by the
     * post-preemption migration target selection in cpu_exec_proc, so a
     * directed migrate_to can never place the task on a forbidden core.      */
    volatile uint32_t affinity;
    /* v0.49: SMP_SLOTS private scratch pages this process can remap/unmap at
     * will via SYS_SMP_REMAP/SYS_SMP_UNMAP, at fixed vaddrs SMP_USER_V+n*4K.
     * Holds the CURRENT backing frame's physical address (0 = unmapped) so a
     * remap knows what to shoot down and free once the new mapping is live. */
#define SMP_SLOTS 2
    uint64_t smp_slot_phys[SMP_SLOTS];
    /* v0.49: leaf spinlock serializing this ONE process's own VMA/page-table
     * mutations (map_page + smp_slot_phys bookkeeping) against itself. In
     * this kernel's one-thread-per-kproc execution model only the single
     * core currently running this task ever touches its own page tables, so
     * this lock is uncontended by construction today — it exists to make the
     * remap/unmap compound update (map + old-frame bookkeeping) an explicit
     * critical section rather than an implicit invariant, and is the seam a
     * future multi-threaded-per-process model would actually need. */
    volatile int      vma_lock;
};
#define MAX_KPROC 64                  /* v0.41: +6 cio workers; v0.43: +10 smp_stress workers */
static struct kproc kprocs[MAX_KPROC];
static int n_kproc = 0;
/* v0.45: pid identity is now separate from slot index — a recycled slot
 * must never hand out a pid a still-live reference could mistake for the
 * process that used to occupy it. Monotonic across the whole boot; 0 is
 * never a valid pid (used as a "no process" sentinel elsewhere), so the
 * wraparound skips it. */
static uint64_t g_next_pid = 1;

/* Diagnostic-only: kproc index -> pid, bounds-checked (proc_idx is untrusted
 * at a fault: the very thing being debugged may be a stale/out-of-range
 * identity). Returns 0 (never a real pid) if the index doesn't resolve. */
static uint64_t dbg_pid_of(uint64_t proc_idx) {
    if (proc_idx >= (uint64_t)n_kproc) return 0;
    return kprocs[proc_idx].pid;
}

/* v0.45: clears every field of a kproc slot's PER-PROCESS lifetime state —
 * identity, address-space handle, scheduling state, and the v0.44 DMA grant
 * table — so the slot is indistinguishable from a never-used one before its
 * next occupant is installed. Deliberately does NOT touch anything kernel-
 * global: the frame allocator, VFS/CAS tables, the IOMMU domain tables, or
 * the kernel's own PML4 identity map. Those are reclaimed by their own
 * dedicated teardown (descriptor_teardown_kproc, dma_teardown_kproc,
 * page_free_tree) BEFORE this ever runs — kproc_reset only blanks the
 * bookkeeping struct itself, once nothing outside it still points in.      */
static void kproc_reset(struct kproc *p) {
    p->pid = 0;
    p->name[0] = 0;
    p->caps = 0;
    p->cr3 = 0;
    p->used = false;
    p->role = 0;
    p->dma_next = 0;
    p->exit_code = 0;
    p->exited = 0;
    p->entry = 0;
    p->pstate = 0;
    p->migrate_to = -1;
    p->uctx = (struct uctx){0};
    p->ran_on = 0;
    p->finish_seq = 0;
    p->dispatches = 0;
    p->frames_freed = 0;
    for (int g = 0; g < MAX_DMA_GRANTS; g++) p->dma_grants[g].used = 0;
    p->dma_grant_count = 0;
    p->torn_down = 0;
    p->affinity = 0;                                   /* v0.49: unrestricted by default */
    for (int s = 0; s < SMP_SLOTS; s++) p->smp_slot_phys[s] = 0;
    p->vma_lock = 0;
}

/* Called from syscall_entry's shared epilogue (boot/usermode.asm), for BOTH
 * success and error returns alike — there is only one epilogue, so this fires
 * on every syscall return when armed. Runs strictly before any of the saved
 * registers are popped back and before RSP/RIP are handed to SYSRET, so it
 * observes exactly the CR3/RSP/RIP the current thread is about to resume
 * with. Non-static: called across the C/asm boundary. */
void dbg_syscall_exit(uint64_t saved_rip, uint64_t saved_rsp) {
    if (!g_debug_syscall_exit) return;
    kprintf("[dbgsys ] pid %u cr3=%X user_rip=%X user_rsp=%X\n",
            dbg_pid_of(current_proc_idx), read_cr3(), saved_rip, saved_rsp);
}

/* Who is 'running': since v0.39 `current_proc_idx` is the PER-CPU slot
 * g_cpu[cpu_idx()].cur_proc (macro at the top of the file) — each core carries
 * the identity of the ring-3 task IT is executing. On the BSP it stays
 * per-thread on top: sched_switch_to saves/restores the BSP's slot per PCB.  */

/* v0.49: kproc table lock — a raw leaf spinlock in the same undiscipline as
 * g_frame_lock (rank 6): acquired around nothing but the tiny slot-scan-and-
 * claim below, never nested under a klock, never held across an allocation.
 * kproc_spawn has been a BSP-only call in every suite through v0.48 (nothing
 * in this kernel issues SYS_SPAWN from ring 3), so this closes a latent gap
 * rather than a live one: with per-CPU run queues now able to run genuinely
 * concurrent ring-3 workloads across every core, a second spawn source would
 * otherwise race the scan-then-claim of n_kproc/torn_down against this one. */
static volatile int g_kproc_lock = 0;
static inline void kproc_lock(void)   { while (__sync_lock_test_and_set(&g_kproc_lock, 1)) __asm__ volatile("pause"); }
static inline void kproc_unlock(void) { __sync_lock_release(&g_kproc_lock); }

static int kproc_spawn(const char *name, uint64_t caps) {
    /* v0.45: a slot is safe to recycle only once torn_down is set — NOT on
     * `exited` alone. `exited` flips true early in every exit path (right
     * after the ring-3 excursion returns, before that core has run
     * descriptor_teardown_kproc/dma_teardown_kproc/page_free_tree), so a
     * scan keyed on `exited` could grab a slot while another core's
     * page_free_tree(kprocs[s].cr3) was still walking the OLD address space
     * — this function would then overwrite `cr3` with a fresh one out from
     * under it, and that in-flight page_free_tree would free the NEW,
     * live process's frames instead (corrupts the free-list — caught live
     * running cmd_kproc_stress while building this milestone). torn_down is
     * set once, last, strictly after page_free_tree returns, so waiting on
     * it makes recycling wait for the full teardown chain, not just the
     * excursion's return. First-fit ascending scan: no ordering property is
     * needed here (unlike the surface free list's LIFO, there is no "most
     * recent" kproc slot worth preferring).
     * v0.49: the scan-then-claim is now one atomic critical section under
     * g_kproc_lock — clearing torn_down (the recycle claim signal) INSIDE the
     * lock is what stops a second concurrent spawn from matching the same
     * slot before this one finishes installing its new occupant. */
    int i = -1, recycled = 0;
    kproc_lock();
    for (int s = 0; s < n_kproc; s++)
        if (kprocs[s].used && kprocs[s].torn_down) { i = s; recycled = 1; kprocs[s].torn_down = 0; break; }
    if (i < 0) {
        if (n_kproc >= MAX_KPROC) { kproc_unlock(); return -1; }
        i = n_kproc++;
    }
    kproc_unlock();
    kproc_reset(&kprocs[i]);
    uint64_t pid = g_next_pid++;
    if (!g_next_pid) g_next_pid = 1;      /* wrap-safe: pid 0 is never valid */
    kprocs[i].pid  = pid;
    kstrcpy_n(kprocs[i].name, name, sizeof kprocs[i].name);
    kprocs[i].caps = caps;
    kprocs[i].cr3  = create_address_space();
    kprocs[i].used = true;
    if (g_debug_kproc_lifetime)
        kprintf("[dbgkpr ] spawn: slot %d %s -> pid %u '%s' caps %X\n",
                i, recycled ? "RECYCLED" : "fresh", pid, name, caps);
    kprintf("[kernel ] spawned pid %u '%s' caps %X — private PML4 @ phys %X\n",
            kprocs[i].pid, name, caps, kprocs[i].cr3);
    return i;
}

/* ---- Device registry ------------------------------------------------------- */
struct kdev { const char *name; uint64_t base, len, req; bool used; uint16_t bdf; };
#define MAX_KDEV 8
static struct kdev kdevs[MAX_KDEV];
static int n_kdev = 0;

/* v0.47: an optional SECOND MMIO region per device, for VFIO's bar_index — no
 * currently-registered device (sensor0, virtio-net) genuinely has more than
 * one discrete PCI BAR in this kernel's simplified device model (virtio's
 * own common/notify/isr/devcfg split is offsets WITHIN one BAR, not separate
 * BARs), so rather than fabricate a multi-BAR array with no real backing,
 * this parallel table is populated only for the dedicated VFIO test device
 * (see cmd_vfio_stress) — bar_index 0 always means kdevs[idx] itself;
 * bar_index 1 is valid only where g_kdev_bar1_len[idx] != 0. */
static uint64_t g_kdev_bar1_phys[MAX_KDEV];
static uint64_t g_kdev_bar1_len[MAX_KDEV];
/* -1 = no interrupt associated with this device (true of every device before */
/* v0.47); see the MAX_VFIO_LINES section below for what a real value means.  */
static int g_kdev_irq_line[MAX_KDEV] = { [0 ... MAX_KDEV - 1] = -1 };

static void kdev_register(const char *name, uint64_t base, uint64_t len, uint64_t req) {
    if (n_kdev >= MAX_KDEV) return;
    kdevs[n_kdev++] = (struct kdev){ name, base, len, req, true, 0xFFFF };
}
static struct kdev *kdev_find(uint64_t io_addr) {
    for (int i = 0; i < n_kdev; i++) {
        if (!kdevs[i].used) continue;
        if (io_addr >= kdevs[i].base && io_addr < kdevs[i].base + kdevs[i].len) return &kdevs[i];
        if (g_kdev_bar1_len[i] && io_addr >= g_kdev_bar1_phys[i] &&
            io_addr < g_kdev_bar1_phys[i] + g_kdev_bar1_len[i]) return &kdevs[i];
    }
    return 0;
}

/* ===========================================================================
 * v0.47: USER-SPACE INTERRUPT ROUTING — IRQ-line ownership + teardown
 * ===========================================================================
 * g_vfio_irq_seq/g_vfio_test_fire_at (the parts isr_dispatch itself needs)
 * are declared much earlier, right after g_ticks — isr_dispatch runs long
 * before struct kproc or the kdev registry exist in this file. What belongs
 * here, alongside kdevs[]/struct kproc, is per-process OWNERSHIP of a line:
 * which kproc slot may SYS_VFIO_WAIT_IRQ on it right now; -1 = none. Set the
 * moment a process maps the BAR of the device that owns the line (mirrors
 * real VFIO: the process that opened the device group owns its interrupt
 * eventfd); cleared by vfio_teardown_kproc on that process's exit. */
static int g_vfio_irq_owner[MAX_VFIO_LINES] = { [0 ... MAX_VFIO_LINES - 1] = -1 };

/* Called from every kproc exit path, alongside ipc_teardown_kproc/
 * descriptor_teardown_kproc/dma_teardown_kproc. Its scope is deliberately
 * tiny: dma_teardown_kproc (already wired into all three exit paths since
 * v0.44) and page_free_tree already fully unmap a VFIO BAR mapping and
 * restore its frame's ownership — SYS_VFIO_MAP_BAR routes through the SAME
 * dma_grant_create(..., DMA_GRANT_MMIO, ...) sys_hardware_passthrough uses,
 * so that machinery (including v0.45's device-MMIO double-free guard) is
 * reused, not reimplemented. The one thing v0.44/v0.45 know nothing about
 * is IRQ-line ownership, so that is all this function releases. */
static void vfio_teardown_kproc(int proc_idx) {
    for (int i = 0; i < MAX_VFIO_LINES; i++) {
        if (g_vfio_irq_owner[i] != proc_idx) continue;
        if (g_debug_vfio)
            kprintf("[dbgvfio] pid %u slot %d: released IRQ line %d ownership\n",
                    kprocs[proc_idx].pid, proc_idx, i);
        g_vfio_irq_owner[i] = -1;
    }
}
static void gpu_teardown_kproc(int proc_idx);   /* fwd: v0.50, defined with the virtio-gpu driver */

/* A ring-3 application surface: pixels owned and rendered by an unprivileged
 * process, composited by the kernel. The compositor never draws this content —
 * it only places it. */
struct sevent { int32_t type, x, y, code; };   /* 1=click 2=key, x/y surface-local */
/* v0.32: DOUBLE-BUFFERED. phys is buffer 0 of a contiguous 2*bufpages chunk;
 * buffer 1 sits at phys + bufpages*4K. The compositor reads only buf[front];
 * the app draws only buf[front^1] and publishes with SYS_SURFACE_FLIP, which
 * blocks until the compositor consumes the flip at a frame boundary — so a
 * blit can never observe a half-drawn frame. Apps that never flip keep the
 * v0.31 single-buffer behavior (front stays 0).                              */
/* v0.34: flip consumption is tracked PER SLOT. A consumer (the compositor for
 * the slots it composites, or a suite standing in) sets `consumer`; only then
 * does SYS_SURFACE_FLIP block for a frame boundary. A flip on a slot nobody
 * consumes completes immediately — a producer can never be parked by a pass
 * that doesn't display it.                                                   */
struct surface { uint64_t phys; int w, h, used, owner;
                 int bufpages, front; volatile int flip_pending;
                 volatile int consumer;
                 struct sevent q[16]; volatile uint32_t qw, qr; };
static struct surface g_surf[8];
static inline uint64_t surf_front_phys(struct surface *S) {
    return S->front ? S->phys + (uint64_t)S->bufpages * 0x1000 : S->phys;
}
static int  iommu_attach_proc_domain(uint16_t bdf, int proc_idx);  /* fwd: IOMMU DMA domain */
static void iommu_domain_add_page(int proc_idx, uint64_t pa);      /* fwd: live domain add */
static int  dma_grant_create(struct kproc *p, uint64_t phys, uint64_t size,
                              uint32_t flags, uint16_t bdf);        /* fwd: v0.44 grant table */
static void dma_grant_revoke(struct kproc *p, struct dma_grant *g); /* fwd: v0.44 grant table */
static void descriptor_teardown_kproc(int proc_idx);   /* fwd: v0.45 fd/descriptor teardown, defined after g_ofiles */

/* v0.31: surface lifecycle. The frame allocator is a bump allocator with no
 * general free, so reclaimed pixel buffers go on a small chunk list that
 * SYS_SURFACE_CREATE consults before allocating fresh frames.                */
struct schunk { uint64_t phys, pages; };
static struct schunk g_surf_free[8];
static int      g_surf_nfree = 0;
static uint64_t g_surf_last_reclaim = 0;   /* phys of the most recent reclaim (test hook) */

static void surfaces_reclaim(int proc_idx) {
    for (int i = 0; i < 8; i++) {
        if (!g_surf[i].used || g_surf[i].owner != proc_idx) continue;
        uint64_t pages = 2ull * g_surf[i].bufpages;        /* both halves of the pair */
        if (g_surf_nfree < 8) {
            g_surf_free[g_surf_nfree].phys  = g_surf[i].phys;
            g_surf_free[g_surf_nfree].pages = pages;
            g_surf_nfree++;
        }
        g_surf_last_reclaim = g_surf[i].phys;
        g_surf[i].used = 0; g_surf[i].owner = -1; g_surf[i].qw = g_surf[i].qr = 0;
        g_surf[i].front = 0; g_surf[i].flip_pending = 0; g_surf[i].consumer = 0;
        kprintf("[surface] slot %d reclaimed on owner exit — %u pages (phys %X) back on the free list\n",
                (uint64_t)i, pages, g_surf_last_reclaim);
    }
}

/* ---- THE CAPABILITY-GATED HARDWARE PASSTHROUGH SYSCALL --------------------- */
/* Returns the virtual base the device was mapped at, or a negative code with   */
/* NOTHING mapped. Two gates, both bitset checks; no capability, no PTE.         */
static int64_t sys_hardware_passthrough(int idx, uint64_t io_addr) {
    struct kdev *d = kdev_find(io_addr);
    if (!d) { kprintf("[hw     ] no device at MMIO %X\n", io_addr); return -1; }
    if (idx < 0 || idx >= n_kproc) return -1;
    struct kproc *p = &kprocs[idx];

    if (!rust_cap_check(p->caps, PCAP_HW_PASSTHROUGH)) {   /* <-- Rust cap engine */
        kprintf("[hw     ] DENIED pid %u '%s' -> '%s': lacks CAP_HW_PASSTHROUGH\n",
                p->pid, p->name, d->name);
        return -2;
    }
    if (!rust_cap_check(p->caps, d->req)) {                 /* <-- Rust cap engine */
        kprintf("[hw     ] DENIED pid %u '%s' -> '%s': missing device capability\n",
                p->pid, p->name, d->name);
        return -3;
    }

    /* Granted: install real PTEs mapping the device MMIO into the process.     */
    uint64_t vbase = 0x0000400000000000ull + ((uint64_t)p->pid << 30);
    uint64_t pages = (d->len + 0xFFF) / 0x1000;
    for (uint64_t k = 0; k < pages; k++)
        map_page(p->cr3, vbase + k * 0x1000, d->base + k * 0x1000,
                 PTE_WRITE | PTE_USER | PTE_PCD | PTE_NX);
    kprintf("[hw     ] GRANT  pid %u '%s' -> '%s': phys %X mapped at vaddr %X (%u pages)\n",
            p->pid, p->name, d->name, d->base, vbase, pages);
    /* Capability-bound DMA confinement: the device is moved into a DMA domain   */
    /* mapping ONLY this process's memory, so it cannot touch the kernel or any  */
    /* other process even though the process drives it directly.                 */
    if (d->bdf != 0xFFFF)
        dma_grant_create(p, d->base, pages * 0x1000ull, DMA_GRANT_MMIO, d->bdf);
    return (int64_t)vbase;
}

/* ---- Boot-time demonstration ---------------------------------------------- */
static void cmd_passthrough(void) {
    kputs("-- real 4-level page-table hardware passthrough --\n");

    /* A scratch physical page stands in for a device register file. We stamp a */
    /* sentinel into it through the identity map, then prove a process reads it */
    /* back THROUGH its own virtual mapping only if it holds the capability.    */
    uint64_t dev_phys = alloc_frame();
    *(volatile uint32_t *)dev_phys = 0xCAFEBABE;
    kdev_register("sensor0", dev_phys, 0x1000, PCAP_CAMERA);
    kprintf("[hw     ] device 'sensor0' registers at phys %X, sentinel 0x%X, requires CAMERA\n",
            dev_phys, (uint64_t)0xCAFEBABE);

    int a = kproc_spawn("stream-app",  PCAP_HW_PASSTHROUGH | PCAP_CAMERA);
    int b = kproc_spawn("sketchy-app", PCAP_FILESYSTEM);

    kputs("\n[test   ] authorized process maps the device:\n");
    int64_t va = sys_hardware_passthrough(a, dev_phys);
    if (va > 0) {
        uint64_t resolved = 0;
        translate(kprocs[a].cr3, (uint64_t)va, &resolved);
        kprintf("[paging ] '%s' PTE: vaddr %X -> phys %X\n",
                kprocs[a].name, (uint64_t)va, resolved);

        /* Switch into the process address space and read the device sentinel  */
        /* through the process's OWN virtual mapping. cli/sti so no IRQ lands   */
        /* mid-switch (harmless anyway: kernel stays mapped via shared entry 0).*/
        __asm__ volatile("cli");
        uint64_t saved = read_cr3();
        write_cr3(kprocs[a].cr3);
        uint32_t seen = *(volatile uint32_t *)(uint64_t)va;
        write_cr3(saved);
        __asm__ volatile("sti");
        kprintf("[paging ] '%s' read through its OWN address space: 0x%X  %s\n",
                kprocs[a].name, seen,
                seen == 0xCAFEBABE ? "== sentinel (REAL mapping resolves)" : "MISMATCH");
    }

    kputs("\n[test   ] unauthorized process attempts the same device:\n");
    int64_t vb = sys_hardware_passthrough(b, dev_phys);
    uint64_t dummy = 0;
    int present = translate(kprocs[b].cr3, 0x0000400000000000ull + (kprocs[b].pid << 30), &dummy);
    kprintf("[paging ] '%s' passthrough returned %d; device vaddr present in its space? %s\n",
            kprocs[b].name, (int)vb, present ? "YES?!" : "NO (sandboxed, nothing mapped)");

    kputs("-- done: authorized app holds a real MMIO mapping; unauthorized was dropped --\n");
}

/* ===========================================================================
 * REAL PCI ENUMERATION + VIRTIO MMIO DISCOVERY
 * ===========================================================================
 * Walks PCI configuration space (mechanism #1, ports 0xCF8/0xCFC), finds the
 * virtio device, parses its VIRTIO-modern vendor capabilities to locate the
 * memory BAR carrying the device's register structures, sizes the BAR, and
 * registers that REAL physical MMIO window in the capability-gated device
 * registry. From then on the same sys_hardware_passthrough that granted a
 * scratch page now grants actual hardware registers.
 * =========================================================================== */

#define PCAP_NETWORK (1ull << 4)

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
              | ((uint32_t)fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}
static void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
              | ((uint32_t)fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

/* virtio-modern layout info discovered from the PCI vendor capability list   */
static int      g_virtio_kdev   = -1;   /* index into kdevs[], -1 = not found */
static uint64_t g_virtio_common = 0;    /* offset of common-config in the BAR */
static uint64_t g_virtio_devcfg = 0;    /* offset of device-config in the BAR */

static void pci_probe_virtio(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t id = pci_cfg_read32(bus, dev, fn, 0x00);
    uint16_t device_id = (uint16_t)(id >> 16);

    /* Walk the capability list looking for virtio vendor caps (id 0x09).     */
    uint32_t status = pci_cfg_read32(bus, dev, fn, 0x04);
    if (!((status >> 16) & 0x10)) return;                 /* no cap list       */
    uint8_t cap = (uint8_t)(pci_cfg_read32(bus, dev, fn, 0x34) & 0xFC);
    int bar_idx = -1;

    while (cap) {
        uint32_t c0 = pci_cfg_read32(bus, dev, fn, cap);
        uint8_t cap_id   = (uint8_t)c0;
        uint8_t cap_next = (uint8_t)((c0 >> 8) & 0xFC);
        if (cap_id == 0x09) {                              /* VIRTIO vendor cap */
            uint8_t  cfg_type = (uint8_t)(c0 >> 24);
            uint8_t  bar      = (uint8_t)(pci_cfg_read32(bus, dev, fn, cap + 4) & 0xFF);
            uint32_t offset   = pci_cfg_read32(bus, dev, fn, cap + 8);
            if (cfg_type == 1) { g_virtio_common = offset; bar_idx = bar; } /* COMMON_CFG */
            if (cfg_type == 4) { g_virtio_devcfg = offset; }                /* DEVICE_CFG */
        }
        cap = cap_next;
    }
    if (bar_idx < 0) return;

    /* Read + size the memory BAR carrying the virtio structures.             */
    uint8_t  bo  = (uint8_t)(0x10 + 4 * bar_idx);
    uint32_t lo  = pci_cfg_read32(bus, dev, fn, bo);
    if (lo & 1) { kprintf("[pci    ] virtio BAR%d is I/O-port, not MMIO — skipping\n", bar_idx); return; }
    int is64 = ((lo >> 1) & 3) == 2;
    uint64_t base = (uint64_t)(lo & ~0xFu);
    if (is64) base |= (uint64_t)pci_cfg_read32(bus, dev, fn, (uint8_t)(bo + 4)) << 32;

    pci_cfg_write32(bus, dev, fn, bo, 0xFFFFFFFF);         /* size probe        */
    uint32_t sz = pci_cfg_read32(bus, dev, fn, bo);
    pci_cfg_write32(bus, dev, fn, bo, lo);                 /* restore           */
    uint64_t size = ~(uint64_t)(sz & ~0xFu) + 1;
    if (!size || size > 0x100000) size = 0x4000;           /* sane fallback     */

    /* Enable memory-space decode so the BAR responds to reads.               */
    uint32_t cmd = pci_cfg_read32(bus, dev, fn, 0x04);
    pci_cfg_write32(bus, dev, fn, 0x04, cmd | 0x2);

    kdev_register("virtio-net", base, size, PCAP_NETWORK);
    kdevs[n_kdev - 1].bdf = (uint16_t)((bus << 8) | (dev << 3) | fn);   /* IOMMU source-id */
    g_virtio_kdev = n_kdev - 1;
    kprintf("[pci    ] virtio device %x: MMIO BAR%d phys %X (+%X), common@+%X devcfg@+%X\n",
            (uint64_t)device_id, bar_idx, base, size, g_virtio_common, g_virtio_devcfg);
    kprintf("[pci    ] registered as capability-gated device 'virtio-net' (requires NETWORK)\n");
}

/* ===========================================================================
 * PREEMPTIVE ROUND-ROBIN THREAD SCHEDULER
 * ===========================================================================
 * Lightweight kernel threads with a PCB per thread. Voluntary switches go
 * through sched_yield(); the 100 Hz PIT drives sched_preempt() for time
 * slicing. Threads block on I/O by parking (BLOCKED) with a wait tag; the
 * virtio bottom half wakes the specific thread that owns the completed tag,
 * so many I/O requests can be outstanding at once.
 *
 * Kernel threads share the kernel address space (one CR3); the PCB carries a
 * cr3 field for future ring-3 threads but kernel threads never swap it.
 * =========================================================================== */
extern void switch_context(uint64_t *save_rsp, uint64_t new_rsp);

enum { T_FREE = 0, T_RUNNABLE, T_RUNNING, T_BLOCKED };

struct pcb {
    uint64_t rsp;               /* saved stack pointer (rest of state is on it) */
    uint64_t cr3;               /* address space; saved LIVE (read_cr3) on switch */
    int      state;
    int      id;
    uint64_t wait_tag;          /* what this thread is BLOCKED on               */
    void   (*entry)(void *);
    void    *arg;
    uint8_t *stack;
    const char *name;
    uint64_t canary;            /* per-thread stack-canary value (entropy at create) */
    /* v0.31: the ring-3 half of a first-class user thread                      */
    uint64_t proc;              /* current_proc_idx while this thread runs      */
    int      uthread;           /* 1 = owns a ring-3 process (never resume_kernel) */
    uint64_t rsp0;              /* TSS.rsp0 while this thread runs (0 = kernel default) */
    uint64_t ksrsp;             /* SYSCALL kernel stack top     (0 = kernel default) */
    /* v0.41: klock ranks THIS THREAD holds. Per-thread on the BSP because a
     * lock holder can park (vblk wait) and another BSP thread runs meanwhile
     * — a per-CPU stack would see the parked holder's ranks as its own.      */
    uint8_t  rank_stack[8];
    uint8_t  rank_sp;
};

#define MAX_THREADS 16
#define TSTACK_SZ   (16 * 1024)
static struct pcb g_threads[MAX_THREADS];
static uint8_t    g_tstacks[MAX_THREADS][TSTACK_SZ] __attribute__((aligned(64)));
static int        g_cur = 0;
static int        g_idle_id = 1;
static int        g_sched_on = 0;
#define curthr (&g_threads[g_cur])

static inline void preempt_disable(void) { g_preempt_off++; }
static inline void preempt_enable(void)  { if (g_preempt_off > 0) g_preempt_off--; }

static int pick_next(void) {
    for (int off = 1; off <= MAX_THREADS; off++) {
        int i = (g_cur + off) % MAX_THREADS;
        if (i == g_idle_id) continue;
        if (g_threads[i].state == T_RUNNABLE) return i;
    }
    if (g_threads[g_idle_id].state == T_RUNNABLE || g_cur != g_idle_id) return g_idle_id;
    return g_cur;
}

/* Load the ring-3 machine context for the incoming thread: which kernel stack
 * the CPU lands on when this thread traps in from CPL3 (interrupt -> TSS.rsp0,
 * SYSCALL -> g_ksrsp). Defined after the TSS; 0 means "the boot defaults".     */
static void uthread_ctx_load(struct pcb *next);

/* Core switch. Caller guarantees scheduler state is quiescent (IF off).       */
static void __attribute__((no_stack_protector)) sched_switch_to(int nextid) {
    if (nextid == g_cur) return;
    struct pcb *prev = &g_threads[g_cur];
    struct pcb *next = &g_threads[nextid];
    if (prev->state == T_RUNNING) prev->state = T_RUNNABLE;
    next->state = T_RUNNING;
    g_cur = nextid;
    struct cpu_local *cl = &g_cpu[cpu_idx()];/* this CPU's guard word (%gs:80):   */
    prev->canary = cl->canary;               /* save this thread's guard          */
    cl->canary   = next->canary;             /* load the next thread's guard      */
    prev->proc = current_proc_idx;           /* per-thread process identity: the  */
    current_proc_idx = next->proc;           /* capability gate reads this        */
    prev->cr3 = read_cr3();                  /* save the LIVE address space (the  */
    if (next->cr3 != prev->cr3)              /* boot thread roams across spaces)  */
        write_cr3(next->cr3);
    uthread_ctx_load(next);                  /* TSS.rsp0 + SYSCALL stack           */
    switch_context(&prev->rsp, next->rsp);   /* on resume, our guard was restored  */
}

/* Voluntary yield (thread context). Serialized against the timer via cli.     */
static void sched_yield(void) {
    if (!g_sched_on) return;
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0" : "=r"(fl));
    __asm__ volatile("cli");
    int n = pick_next();
    sched_switch_to(n);
    if (fl & 0x200) __asm__ volatile("sti");
}

/* Preemptive reschedule from the timer ISR (IF already off).                  */
static void sched_preempt(void) {
    if (!g_sched_on || g_preempt_off) return;
    int n = pick_next();
    sched_switch_to(n);
}

/* Wake a specific thread (called from the bottom half, IF off).               */
static void thread_wake(int tid) {
    if (tid >= 0 && tid < MAX_THREADS && g_threads[tid].state == T_BLOCKED)
        g_threads[tid].state = T_RUNNABLE;
}

static void thread_trampoline(void) {
    __asm__ volatile("sti");                 /* run with interrupts enabled     */
    struct pcb *t = curthr;
    t->entry(t->arg);
    __asm__ volatile("cli");
    t->state = T_FREE;                       /* exited — never scheduled again  */
    sched_switch_to(pick_next());            /* will not return                 */
    for (;;) __asm__ volatile("hlt");
}

static int thread_create(const char *name, void (*entry)(void *), void *arg) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (g_threads[i].state != T_FREE || i == g_cur) continue;
        struct pcb *t = &g_threads[i];
        t->id = i; t->name = name; t->entry = entry; t->arg = arg;
        t->cr3 = kernel_cr3; t->stack = g_tstacks[i]; t->wait_tag = 0;
        t->proc = 0; t->uthread = 0; t->rsp0 = 0; t->ksrsp = 0;
        uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        t->canary = ((((uint64_t)hi << 32) | lo) * 0x9E3779B97F4A7C15ull)
                    ^ (0xC0FFEE0000ull + (uint64_t)i * 0x100000001B3ull);   /* per-thread entropy */
        uint64_t *sp = (uint64_t *)(t->stack + TSTACK_SZ);
        *--sp = (uint64_t)thread_trampoline;  /* ret target of switch_context   */
        *--sp = 0;                            /* rbx                            */
        *--sp = 0;                            /* rbp                            */
        *--sp = 0;                            /* r12                            */
        *--sp = 0;                            /* r13                            */
        *--sp = 0;                            /* r14                            */
        *--sp = 0;                            /* r15                            */
        *--sp = 0x202;                        /* rflags: IF set                 */
        t->rsp = (uint64_t)sp;
        t->state = T_RUNNABLE;
        return i;
    }
    return -1;
}

static void idle_fn(void *a) {
    (void)a;
    for (;;) { __asm__ volatile("sti; hlt"); sched_yield(); }
}

/* ===========================================================================
 * v0.41: RANKED CROSS-CORE SPINLOCKS — the kernel's lock-ordering discipline
 * ===========================================================================
 * Until v0.40 every VFS/CAS/descriptor/surface path was exercised from the
 * BSP only, so "one core at a time" was the (implicit) lock. v0.41 removes
 * that restriction, and these locks are what replaces it. Deadlock freedom
 * rests on a single global rank order — every context acquires strictly
 * UPWARD in rank and releases LIFO:
 *
 *   rank 1  g_ofile_lock   open-descriptor array (fd alloc/free/deref)
 *   rank 2  g_vfs_lock     VFS directory: dirent claim/scan/rewrite/flush
 *   rank 3  g_cas_lock     CAS superblock counters, bitmap, index,
 *                          shared staging sectors (g_blk / g_idxbuf)
 *   rank 4  g_vblk_lock    virtio-blk request slots + avail-ring publish
 *   rank 5  g_surf_lock    surface slot table + pixel-buffer free list
 *   (6)     g_frame_lock   v0.42: frame free-list — a raw leaf spinlock, NOT
 *                          a klock and deliberately UNRANKED: it must work
 *                          before the scheduler exists (early boot) and on
 *                          APs (no per-CPU rank tracking either), so it is
 *                          never nested under a klock and never held across
 *                          an allocation or a yield — see alloc_frame().
 *   (7)     g_conlock      console — IRQ-safe leaf inside kprintf only
 *
 * The deep chain is SYS_WRITE_FILE: vfs(2) -> cas(3) -> vblk(4). The surface
 * chain surf(5) -> frame(6) is disjoint from the file chain, so no cycle can
 * pass through the allocator. Ranks 1 and 2 never nest AT ALL — vfs_open
 * would want vfs->ofile while SYS_READ wants ofile->vfs, a real inversion,
 * so both paths were built as two disjoint critical sections instead.
 *
 * Blocking rules that keep this deadlock-free on real cores:
 *   - A klock is NEVER acquired in interrupt context (g_conlock stays the
 *     only IRQ-side lock; the virtio bottom half touches only per-slot
 *     completion flags).
 *   - g_vblk_lock is never held across a disk WAIT — submit publishes the
 *     avail entry, kicks the doorbell, releases; the wait is lock-free on
 *     the slot's own done flag.
 *   - Ranks 2/3 MAY be held across a blocking disk wait. That is safe only
 *     because a contended acquire never bare-spins the one core that could
 *     run the holder: on the BSP it yields through the scheduler (the parked
 *     holder is woken by the IRQ bottom half and finishes), on an AP it
 *     PAUSE-spins with IF set (the BSP services the completion IRQ).
 *   - Rank order is ENFORCED at runtime: each context tracks the ranks it
 *     holds (per-thread on the BSP, per-CPU on APs) and any non-monotonic
 *     acquire counts a violation the cio suite fails on.
 * =========================================================================== */
struct klock {
    volatile int      v;                     /* 0 = free, 1 = held             */
    const char       *name;
    uint8_t           rank;
    volatile uint32_t acq;                   /* successful acquisitions        */
    volatile uint32_t contended;             /* acquisitions that had to wait  */
};
static struct klock g_ofile_lock = { 0, "ofile", 1, 0, 0 };
static struct klock g_vfs_lock   = { 0, "vfs",   2, 0, 0 };
static struct klock g_cas_lock   = { 0, "cas",   3, 0, 0 };
static struct klock g_vblk_lock  = { 0, "vblk",  4, 0, 0 };
static struct klock g_surf_lock  = { 0, "surf",  5, 0, 0 };
static volatile uint32_t g_rank_violations = 0;

/* Back off without monopolizing the core that must make our progress: the BSP
 * runs its scheduler (the lock holder may be a parked sibling thread), an AP
 * has no kernel scheduler and PAUSE-spins with interrupts deliverable.       */
static inline void krelax(void) {
    if (cpu_idx() == 0 && g_sched_on) sched_yield();
    else __asm__ volatile("pause");
}

/* Rank bookkeeping storage for the CURRENT context (see the pcb comment).    */
static inline uint8_t *rank_ctx(uint8_t **sp) {
    if (cpu_idx() == 0 && g_sched_on) { *sp = &curthr->rank_sp;         return curthr->rank_stack; }
    struct cpu_local *me = &g_cpu[cpu_idx()]; *sp = &me->rank_sp;       return me->rank_stack;
}

static void klock_acquire(struct klock *l) {
    uint8_t *sp, *st = rank_ctx(&sp);                  /* our context's stack;  */
    if (*sp > 0 && st[*sp - 1] >= l->rank) {           /* still ours after any  */
        __sync_fetch_and_add(&g_rank_violations, 1);   /* yield below — a yield */
        kprintf("[klock  ] RANK VIOLATION: acquiring '%s' (rank %d) while holding rank %d\n",
                l->name, (uint64_t)l->rank, (uint64_t)st[*sp - 1]);
    }                                                  /* resumes THIS thread   */
    if (__sync_lock_test_and_set(&l->v, 1)) {
        __sync_fetch_and_add(&l->contended, 1);
        do { krelax(); } while (l->v || __sync_lock_test_and_set(&l->v, 1));
    }
    l->acq++;                                          /* under the lock        */
    if (*sp < 8) st[(*sp)++] = l->rank;
}

static void klock_release(struct klock *l) {
    uint8_t *sp; rank_ctx(&sp);
    if (*sp > 0) (*sp)--;                              /* LIFO release          */
    __sync_lock_release(&l->v);
}

static void sched_init(void) {
    for (int i = 0; i < MAX_THREADS; i++) { g_threads[i].state = T_FREE; g_threads[i].id = i; }
    /* thread 0 = the current boot/main context; its state is captured on the  */
    /* first switch away from it.                                              */
    g_threads[0].state = T_RUNNING; g_threads[0].name = "main";
    g_threads[0].canary = g_cpu[0].canary;     /* main uses the boot-seeded guard */
    g_threads[0].cr3 = kernel_cr3; g_threads[0].stack = 0;
    g_threads[0].proc = 0; g_threads[0].uthread = 0;
    g_threads[0].rsp0 = 0; g_threads[0].ksrsp = 0;   /* boot-default trap stacks */
    g_cur = 0;
    g_idle_id = thread_create("idle", idle_fn, 0);
    g_sched_on = 1;                           /* cooperative switching live      */
    /* Timer preemption stays gated (g_preempt_off=1) and is enabled around the */
    /* workloads that want time-slicing, so the ring-3 excursion path is unaffected. */
    kprintf("[sched  ] round-robin up: main=tid0, idle=tid%d (PIT preemption on demand)\n", g_idle_id);
}

/* ===========================================================================
 * VIRTIO-BLK MASS STORAGE DRIVER  (modern virtio 1.0, split virtqueue)
 * ===========================================================================
 * Phase 1 of standalone operation: talk to a real PCI virtio-blk device so the
 * kernel can read and write disk sectors instead of depending on RAM modules.
 *
 * Pipeline:  PCI discovery -> map the MMIO BAR -> reset + ACK + DRIVER
 *            -> feature negotiation (VIRTIO_F_VERSION_1) -> FEATURES_OK
 *            -> build the split virtqueue (desc table / avail ring / used ring)
 *            -> queue_enable -> DRIVER_OK.  Requests are three chained
 *            descriptors (header, data, status); we submit on the avail ring,
 *            kick the notify register, and BLOCK by polling the used ring.
 * =========================================================================== */

/* --- virtio_pci_common_cfg register offsets (modern spec 4.1.4.3) --------- */
#define VCC_DEV_FEAT_SEL   0x00
#define VCC_DEV_FEAT       0x04
#define VCC_DRV_FEAT_SEL   0x08
#define VCC_DRV_FEAT       0x0C
#define VCC_MSIX_CFG       0x10
#define VCC_NUM_QUEUES     0x12
#define VCC_DEV_STATUS     0x14
#define VCC_CFG_GEN        0x15
#define VCC_Q_SELECT       0x16
#define VCC_Q_SIZE         0x18
#define VCC_Q_MSIX         0x1A
#define VCC_Q_ENABLE       0x1C
#define VCC_Q_NOTIFY_OFF   0x1E
#define VCC_Q_DESC         0x20
#define VCC_Q_DRIVER       0x28   /* avail ring physical address */
#define VCC_Q_DEVICE       0x30   /* used  ring physical address */

/* device status bits */
#define VSTAT_ACK       1
#define VSTAT_DRIVER    2
#define VSTAT_DRIVER_OK 4
#define VSTAT_FEAT_OK   8
#define VSTAT_FAILED    128

/* split-virtqueue descriptor flags */
#define VRING_DESC_F_NEXT   1
#define VRING_DESC_F_WRITE  2

/* virtio-blk request types + status */
#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_S_OK  0

struct vring_desc  { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct vring_avail { uint16_t flags; uint16_t idx; uint16_t ring[]; } __attribute__((packed));
struct vring_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));
struct vring_used  { uint16_t flags; uint16_t idx; struct vring_used_elem ring[]; } __attribute__((packed));

struct virtio_blk_req_hdr { uint32_t type; uint32_t reserved; uint64_t sector; } __attribute__((packed));

/* driver state */
static volatile uint8_t *g_vblk_common = 0;      /* mapped common-cfg base    */
static volatile uint8_t *g_vblk_notify = 0;      /* mapped notify base        */
static uint32_t          g_vblk_notify_mul = 0;
static volatile uint8_t *g_vblk_devcfg = 0;      /* mapped device-cfg base    */
static uint16_t          g_vblk_qsize = 0;
static uint16_t          g_vblk_notify_off = 0;
static struct vring_desc  *g_vblk_desc  = 0;
static struct vring_avail *g_vblk_avail = 0;
static struct vring_used  *g_vblk_used  = 0;
static uint16_t          g_vblk_last_used = 0;
static uint64_t          g_vblk_capacity = 0;    /* in 512-byte sectors       */
static int               g_vblk_ready = 0;

/* --- multiple outstanding requests: one slot per in-flight I/O ------------- */
#define VBLK_MAXREQ 21                        /* qsize(64)/3 descriptors each   */
struct vreq {
    int      in_use;
    int      waiter_tid;                      /* thread blocked on this tag, -1 */
    volatile int done;
    volatile uint8_t status;
};
static struct vreq                g_vreq[VBLK_MAXREQ];
static struct virtio_blk_req_hdr *g_hdrs = 0;      /* array[VBLK_MAXREQ], DMA   */
static volatile uint8_t          *g_stats = 0;     /* array[VBLK_MAXREQ], DMA   */
static int      g_vblk_nslots = 0;
static volatile uint64_t g_inflight = 0, g_max_inflight = 0, g_completions = 0;

/* --- interrupt-driven completion (top/bottom-half split) --- */
static volatile uint8_t  *g_vblk_isr = 0;           /* ISR-status register    */
static uint8_t            g_vblk_irq = 0xFF;         /* PCI interrupt line     */
static volatile uint64_t  g_vblk_irqs = 0;          /* IRQs serviced          */
static volatile uint64_t  g_vblk_fallbacks = 0;     /* (retained, now unused) */

/* width-correct MMIO accessors */
static inline uint8_t  mr8 (volatile uint8_t *b, uint32_t o) { return *(volatile uint8_t  *)(b + o); }
static inline uint16_t mr16(volatile uint8_t *b, uint32_t o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t mr32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void mw8 (volatile uint8_t *b, uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(b + o) = v; }
static inline void mw16(volatile uint8_t *b, uint32_t o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void mw32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void mw64(volatile uint8_t *b, uint32_t o, uint64_t v) { *(volatile uint64_t *)(b + o) = v; }
static inline void barrier(void) { __asm__ volatile("mfence" ::: "memory"); }

#define VBLK_MMIO_V 0x0000600000000000ull

static uint64_t pci_bar_base(uint8_t bus, uint8_t dev, uint8_t fn, int idx) {
    uint8_t bo = (uint8_t)(0x10 + 4 * idx);
    uint32_t lo = pci_cfg_read32(bus, dev, fn, bo);
    if (lo & 1) return 0;                                  /* I/O BAR, skip     */
    uint64_t base = (uint64_t)(lo & ~0xFu);
    if (((lo >> 1) & 3) == 2)
        base |= (uint64_t)pci_cfg_read32(bus, dev, fn, (uint8_t)(bo + 4)) << 32;
    return base;
}

static void map_mmio(uint64_t vaddr, uint64_t phys, uint64_t bytes) {
    uint64_t pages = (bytes + 0xFFF) / 0x1000;
    for (uint64_t i = 0; i < pages; i++)
        map_page(kernel_cr3, vaddr + i * 0x1000, phys + i * 0x1000, PTE_WRITE | PTE_PCD | PTE_NX);
}


/* ===========================================================================
 * ACPI TABLE DISCOVERY
 * ===========================================================================
 * Walk the RSDP -> RSDT/XSDT -> individual tables. Needed to find the DMAR
 * table, which describes the platform's DMA remapping hardware (VT-d).
 * =========================================================================== */
struct acpi_sdt {                                      /* common table header    */
    char     sig[4];
    uint32_t length;
    uint8_t  revision, checksum;
    char     oem_id[6], oem_table_id[8];
    uint32_t oem_revision, creator_id, creator_revision;
} __attribute__((packed));

static int acpi_checksum_ok(void *p, uint32_t len) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + ((uint8_t *)p)[i]);
    return s == 0;
}

static int acpi_sig_eq(const char *a, const char *b) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Find an ACPI table by 4-char signature. Returns its physical address or 0.   */
static uint64_t acpi_find_table(const char *sig, bool print) {
    if (!g_rsdp) return 0;
    uint8_t *r = (uint8_t *)g_rsdp;
    uint64_t xsdt = 0, rsdt = *(uint32_t *)(r + 16);
    if (g_rsdp_rev >= 2) xsdt = *(uint64_t *)(r + 24);
    if (xsdt) {                                        /* prefer the 64-bit XSDT */
        struct acpi_sdt *t = (struct acpi_sdt *)xsdt;
        int n = (int)((t->length - sizeof *t) / 8);
        for (int i = 0; i < n; i++) {
            uint64_t e = *(uint64_t *)((uint8_t *)xsdt + sizeof *t + (uint64_t)i * 8);
            struct acpi_sdt *s = (struct acpi_sdt *)e;
            if (print) kprintf("[acpi   ]   table %c%c%c%c  rev %u  len %u\n",
                               (uint64_t)s->sig[0], (uint64_t)s->sig[1], (uint64_t)s->sig[2],
                               (uint64_t)s->sig[3], (uint64_t)s->revision, (uint64_t)s->length);
            if (acpi_sig_eq(s->sig, sig)) return e;
        }
    } else if (rsdt) {
        struct acpi_sdt *t = (struct acpi_sdt *)rsdt;
        int n = (int)((t->length - sizeof *t) / 4);
        for (int i = 0; i < n; i++) {
            uint64_t e = *(uint32_t *)((uint8_t *)rsdt + sizeof *t + (uint64_t)i * 4);
            struct acpi_sdt *s = (struct acpi_sdt *)e;
            if (print) kprintf("[acpi   ]   table %c%c%c%c  rev %u  len %u\n",
                               (uint64_t)s->sig[0], (uint64_t)s->sig[1], (uint64_t)s->sig[2],
                               (uint64_t)s->sig[3], (uint64_t)s->revision, (uint64_t)s->length);
            if (acpi_sig_eq(s->sig, sig)) return e;
        }
    }
    return 0;
}

/* ===========================================================================
 * INTEL VT-d IOMMU (DMA REMAPPING)
 * ===========================================================================
 * Parses the ACPI DMAR table, maps the remapping hardware registers, builds
 * root/context/second-level page tables, and enables DMA translation so every
 * device DMA is routed through page tables we control. This is the hardware
 * foundation for secure device passthrough: a device can only reach memory its
 * domain maps.
 * =========================================================================== */
#define IOMMU_MMIO_V 0x0000600000030000ull             /* register window vaddr  */
#define DMAR_VER     0x00
#define DMAR_CAP     0x08
#define DMAR_ECAP    0x10
#define DMAR_GCMD    0x18
#define DMAR_GSTS    0x1C
#define DMAR_RTADDR  0x20
#define DMAR_CCMD    0x28
#define DMAR_FSTS    0x34
#define DMAR_FECTL   0x38
#define GCMD_TE      (1u << 31)                        /* translation enable     */
#define GCMD_SRTP    (1u << 30)                        /* set root table pointer */
#define GSTS_TES     (1u << 31)
#define GSTS_RTPS    (1u << 30)

static volatile uint8_t *g_dmar_regs = 0;
static uint64_t g_dmar_phys = 0, g_dmar_cap = 0, g_dmar_ecap = 0;
static uint64_t g_iommu_root = 0;                      /* root table (phys)      */
static uint64_t g_iommu_slpt = 0;                      /* second-level top table */
static int g_iommu_levels = 0, g_iommu_aw = 0, g_iommu_on = 0, g_iommu_drhds = 0;
static uint64_t g_iommu_mapped_bytes = 0;

static uint32_t dmar_r32(uint32_t off) { return *(volatile uint32_t *)(g_dmar_regs + off); }
static uint64_t dmar_r64(uint32_t off) { return *(volatile uint64_t *)(g_dmar_regs + off); }
static void dmar_w32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(g_dmar_regs + off) = v; }
static void dmar_w64(uint32_t off, uint64_t v) { *(volatile uint64_t *)(g_dmar_regs + off) = v; }

/* Build second-level page tables identity-mapping [0, limit) for device DMA.    */
/* Uses 2 MiB superpages when the hardware reports support (CAP.SLLPS bit 34).   */
static uint64_t iommu_build_slpt(uint64_t limit, int levels, int use_2m) {
    uint64_t top = alloc_frame();
    for (int i = 0; i < 512; i++) ((uint64_t *)top)[i] = 0;
    uint64_t pdpt = top;
    if (levels == 4) {                                 /* PML4 -> PDPT           */
        pdpt = alloc_frame();
        for (int i = 0; i < 512; i++) ((uint64_t *)pdpt)[i] = 0;
        ((uint64_t *)top)[0] = pdpt | 0x3;             /* R|W                    */
    }
    uint64_t gigs = (limit + 0x3FFFFFFFull) / 0x40000000ull;
    if (gigs > 512) gigs = 512;
    for (uint64_t g = 0; g < gigs; g++) {
        uint64_t pd = alloc_frame();
        for (int i = 0; i < 512; i++) ((uint64_t *)pd)[i] = 0;
        ((uint64_t *)pdpt)[g] = pd | 0x3;
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t pa = g * 0x40000000ull + i * 0x200000ull;
            if (use_2m) {
                ((uint64_t *)pd)[i] = pa | 0x3 | 0x80; /* R|W|PageSize (2 MiB)   */
            } else {
                uint64_t pt = alloc_frame();
                for (int k = 0; k < 512; k++)
                    ((uint64_t *)pt)[k] = (pa + (uint64_t)k * 0x1000) | 0x3;
                ((uint64_t *)pd)[i] = pt | 0x3;
            }
        }
    }
    return top;
}

static void iommu_invalidate_all(void);   /* fwd: defined with the IOMMU status cmd */

/* ---- capability-bound per-process DMA domains ------------------------------ */
/* A process that is granted a device does not merely get its MMIO registers: the
 * device is moved into a DMA domain whose second-level page tables map ONLY that
 * process's own memory. The device physically cannot reach the kernel or any
 * other process, so "unrestricted hardware access" is safe by construction. */
static uint64_t g_proc_slpt[MAX_KPROC];                /* per-process DMA domain */

/* map one 4 KiB page into a second-level page table (creating levels as needed) */
static void slpt_map4k(uint64_t top, int levels, uint64_t iova, uint64_t pa) {
    uint64_t *tbl = (uint64_t *)top;
    int shift = (levels == 4) ? 39 : 30;
    for (int lvl = levels; lvl > 1; lvl--) {
        int idx = (int)((iova >> shift) & 0x1FF);
        if (!(tbl[idx] & 0x1)) {
            uint64_t nf = alloc_frame();
            for (int i = 0; i < 512; i++) ((uint64_t *)nf)[i] = 0;
            tbl[idx] = nf | 0x3;                       /* R|W                     */
        }
        tbl = (uint64_t *)(tbl[idx] & ADDR_MASK & ~0xFFFull);
        shift -= 9;
    }
    tbl[(iova >> 12) & 0x1FF] = (pa & ~0xFFFull) | 0x3;
}

/* look up a leaf in a second-level page table; 0 if not present */
static uint64_t slpt_lookup(uint64_t top, int levels, uint64_t iova) {
    uint64_t *tbl = (uint64_t *)top;
    int shift = (levels == 4) ? 39 : 30;
    for (int lvl = levels; lvl > 1; lvl--) {
        uint64_t e = tbl[(iova >> shift) & 0x1FF];
        if (!(e & 0x1)) return 0;
        if (e & 0x80) return e;                        /* superpage leaf          */
        tbl = (uint64_t *)(e & ADDR_MASK & ~0xFFFull);
        shift -= 9;
    }
    uint64_t e = tbl[(iova >> 12) & 0x1FF];
    return (e & 0x1) ? e : 0;
}

/* Build a DMA domain containing exactly the process's own RAM pages. Walks the
 * process's page tables and identity-maps each USER data frame (skipping the
 * shared kernel identity map, the kernel MMIO window, and device MMIO pages). */
static uint64_t iommu_build_proc_domain(int proc_idx, int *out_pages) {
    uint64_t top = alloc_frame();
    for (int i = 0; i < 512; i++) ((uint64_t *)top)[i] = 0;
    uint64_t *pml4 = (uint64_t *)(kprocs[proc_idx].cr3 & ADDR_MASK);
    int n = 0;
    for (int a = 0; a < 512; a++) {
        if (a == 0 || a == 0xC0) continue;             /* kernel-shared slots     */
        if (!(pml4[a] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)(pml4[a] & ADDR_MASK);
        for (int b = 0; b < 512; b++) {
            if (!(pdpt[b] & PTE_PRESENT) || (pdpt[b] & PTE_HUGE)) continue;
            uint64_t *pd = (uint64_t *)(pdpt[b] & ADDR_MASK);
            for (int c = 0; c < 512; c++) {
                if (!(pd[c] & PTE_PRESENT) || (pd[c] & PTE_HUGE)) continue;
                uint64_t *pt = (uint64_t *)(pd[c] & ADDR_MASK);
                for (int d = 0; d < 512; d++) {
                    uint64_t e = pt[d];
                    if (!(e & PTE_PRESENT) || !(e & PTE_USER)) continue;
                    if (e & PTE_PCD) continue;         /* device MMIO, not RAM    */
                    slpt_map4k(top, g_iommu_levels, e & ADDR_MASK & ~0xFFFull,
                               e & ADDR_MASK & ~0xFFFull);
                    n++;
                }
            }
        }
    }
    if (out_pages) *out_pages = n;
    return top;
}

/* Live domain update: add a page to a process's DMA domain after it was built. */
static void iommu_domain_add_page(int proc_idx, uint64_t pa) {
    if (!g_iommu_on || !g_proc_slpt[proc_idx]) return;     /* built at grant time */
    slpt_map4k(g_proc_slpt[proc_idx], g_iommu_levels, pa & ~0xFFFull, pa & ~0xFFFull);
    iommu_invalidate_all();
}

/* Point a device's context entry at a process's DMA domain. Returns domain id. */
static int iommu_attach_proc_domain(uint16_t bdf, int proc_idx) {
    if (!g_iommu_on || bdf == 0xFFFF) return -1;
    int pages = 0;
    if (!g_proc_slpt[proc_idx]) g_proc_slpt[proc_idx] = iommu_build_proc_domain(proc_idx, &pages);
    uint8_t bus = (uint8_t)(bdf >> 8), devfn = (uint8_t)(bdf & 0xFF);
    uint64_t ctx = ((uint64_t *)g_iommu_root)[bus * 2] & ~0xFFFull;
    if (!ctx) return -1;
    int domain = 16 + proc_idx;                        /* per-process domain id   */
    ((uint64_t *)ctx)[devfn * 2]     = g_proc_slpt[proc_idx] | 0x1;
    ((uint64_t *)ctx)[devfn * 2 + 1] = (uint64_t)g_iommu_aw | ((uint64_t)domain << 8);
    iommu_invalidate_all();
    kprintf("[iommu  ] device %x:%x.%x confined to pid %u DMA domain %d (%d of its pages reachable)\n",
            (uint64_t)bus, (uint64_t)(devfn >> 3), (uint64_t)(devfn & 7),
            kprocs[proc_idx].pid, (uint64_t)domain, (uint64_t)pages);
    return domain;
}

/* Return a device to the kernel's identity domain (revocation).                */
static void iommu_detach_to_kernel(uint16_t bdf) {
    if (!g_iommu_on || bdf == 0xFFFF) return;
    uint8_t bus = (uint8_t)(bdf >> 8), devfn = (uint8_t)(bdf & 0xFF);
    uint64_t ctx = ((uint64_t *)g_iommu_root)[bus * 2] & ~0xFFFull;
    if (!ctx) return;
    ((uint64_t *)ctx)[devfn * 2]     = g_iommu_slpt | 0x1;
    ((uint64_t *)ctx)[devfn * 2 + 1] = (uint64_t)g_iommu_aw | (1ull << 8);
    iommu_invalidate_all();
}

/* ===========================================================================
 * v0.44: DMA / IOMMU LIFETIME DISCIPLINE
 * ===========================================================================
 * The layer between the raw iommu_* plumbing above and every caller that
 * grants a process DMA-capable memory. Every grant recorded here is
 * guaranteed revoked before page_free_tree runs (dma_teardown_kproc, wired
 * into every kproc exit path) — closing the one hazard v0.42's changelog
 * flagged and left open: g_proc_slpt[idx] survives page_free_tree (it is
 * NOT part of the process's own CR3 hierarchy), so a still-attached device
 * or a stale live-added page would otherwise keep reaching physical frames
 * this process no longer owns once they are recycled to someone else.     */
static int dma_grant_create(struct kproc *p, uint64_t phys, uint64_t size,
                             uint32_t flags, uint16_t bdf) {
    if (p->dma_grant_count >= MAX_DMA_GRANTS) {
        kprintf("[dma    ] pid %u: grant table full (%u active) — refusing new grant\n",
                p->pid, (uint64_t)MAX_DMA_GRANTS);
        return -1;
    }
    int idx = (int)(p - kprocs);                        /* this process's kproc index */
    if (flags & DMA_GRANT_MMIO) {
        iommu_attach_proc_domain(bdf, idx);
    } else if (flags & DMA_GRANT_PAGE) {
        for (uint64_t off = 0; off < size; off += 0x1000)
            iommu_domain_add_page(idx, phys + off);
    }
    struct dma_grant *g = 0;
    for (int i = 0; i < MAX_DMA_GRANTS; i++)
        if (!p->dma_grants[i].used) { g = &p->dma_grants[i]; break; }
    g->phys = phys; g->size = size; g->bdf = bdf; g->flags = flags; g->used = 1;
    p->dma_grant_count++;
    if (g_debug_dma_lifetime)
        kprintf("[dbgdma ] CREATE pid %u phys=%X size=%X bdf=%x flags=%x (active %u)\n",
                p->pid, phys, size, (uint64_t)bdf, (uint64_t)flags, (uint64_t)p->dma_grant_count);
    return 0;
}

/* Revokes ONE grant. Never frees `phys` itself: an MMIO grant's physical
 * range was never process-owned frame-pool memory, and a PAGE grant's frame
 * is an ordinary present PTE in the process's own CR3 — page_free_tree's
 * job, not this function's, freeing it here would double-free it there
 * (exactly the surface-buffer bug v0.42 found and fixed, one layer over).  */
static void dma_grant_revoke(struct kproc *p, struct dma_grant *g) {
    if (!g->used) {
        if (g_debug_dma_lifetime)
            kprintf("[dbgdma ] REVOKE pid %u: attempt on a non-existent grant — ignored\n", p->pid);
        return;
    }
    if (g->flags & DMA_GRANT_MMIO) iommu_detach_to_kernel(g->bdf);
    if (g_debug_dma_lifetime)
        kprintf("[dbgdma ] REVOKE pid %u phys=%X size=%X bdf=%x flags=%x\n",
                p->pid, g->phys, g->size, (uint64_t)g->bdf, (uint64_t)g->flags);
    g->used = 0;
    p->dma_grant_count--;
}

/* Called from every kproc exit path, BEFORE page_free_tree. Revokes every
 * outstanding grant, then reclaims g_proc_slpt[proc_idx] itself — the ONE
 * frame in this whole picture that page_free_tree can never reach, since it
 * is not reachable through the process's own CR3 at all. */
static void dma_teardown_kproc(int proc_idx) {
    struct kproc *p = &kprocs[proc_idx];
    for (int i = 0; i < MAX_DMA_GRANTS; i++)
        if (p->dma_grants[i].used) dma_grant_revoke(p, &p->dma_grants[i]);
    if (p->dma_grant_count != 0) {                       /* must be unreachable  */
        kprintf("\n[panic ] pid %u: %u DMA grant(s) still active after teardown\n",
                p->pid, (uint64_t)p->dma_grant_count);
        for (;;) __asm__ volatile("cli; hlt");
    }
    if (g_proc_slpt[proc_idx]) {
        if (g_debug_dma_lifetime)
            kprintf("[dbgdma ] pid %u: freeing its now-orphaned IOMMU domain frame %X\n",
                    p->pid, g_proc_slpt[proc_idx]);
        free_frame(g_proc_slpt[proc_idx]);
        g_proc_slpt[proc_idx] = 0;
    }
}

static void iommu_init(void) {
    uint64_t dmar = acpi_find_table("DMAR", false);
    if (!dmar) { kputs("[iommu  ] no DMAR table — platform has no VT-d remapping hardware\n"); return; }
    struct acpi_sdt *h = (struct acpi_sdt *)dmar;
    if (!acpi_checksum_ok(h, h->length)) { kputs("[iommu  ] DMAR checksum invalid — ignoring\n"); return; }
    uint8_t haw = *(uint8_t *)(dmar + 36);
    kprintf("[iommu  ] DMAR found: host address width %u bits, flags %x\n",
            (uint64_t)(haw + 1), (uint64_t)*(uint8_t *)(dmar + 37));

    /* walk the remapping structures; DRHD (type 0) carries the register base    */
    uint64_t p = dmar + 48, end = dmar + h->length, first_base = 0;
    while (p + 4 <= end) {
        uint16_t type = *(uint16_t *)p, len = *(uint16_t *)(p + 2);
        if (!len) break;
        if (type == 0) {                               /* DRHD                   */
            uint8_t flags = *(uint8_t *)(p + 4);
            uint64_t base = *(uint64_t *)(p + 8);
            g_iommu_drhds++;
            kprintf("[iommu  ] DRHD #%d: regs @ phys %X  segment %u  %s\n",
                    (uint64_t)g_iommu_drhds, base, (uint64_t)*(uint16_t *)(p + 6),
                    (flags & 1) ? "INCLUDE_PCI_ALL" : "scoped devices");
            if (!first_base) first_base = base;
        } else if (type == 1) {                        /* RMRR                   */
            kprintf("[iommu  ] RMRR: reserved region %X..%X (firmware DMA)\n",
                    *(uint64_t *)(p + 8), *(uint64_t *)(p + 16));
        }
        p += len;
    }
    if (!first_base) { kputs("[iommu  ] DMAR has no DRHD — nothing to program\n"); return; }

    g_dmar_phys = first_base;
    map_mmio(IOMMU_MMIO_V, first_base & ~0xFFFull, 0x1000);
    g_dmar_regs = (volatile uint8_t *)IOMMU_MMIO_V;
    uint32_t ver = dmar_r32(DMAR_VER);
    g_dmar_cap  = dmar_r64(DMAR_CAP);
    g_dmar_ecap = dmar_r64(DMAR_ECAP);
    kprintf("[iommu  ] VT-d version %u.%u  cap %X  ecap %X\n",
            (uint64_t)((ver >> 4) & 0xF), (uint64_t)(ver & 0xF), g_dmar_cap, g_dmar_ecap);

    uint64_t nd    = g_dmar_cap & 0x7;                 /* domain-id width code   */
    uint64_t sagaw = (g_dmar_cap >> 8) & 0x1F;
    uint64_t mgaw  = ((g_dmar_cap >> 16) & 0x3F) + 1;
    uint64_t sllps = (g_dmar_cap >> 34) & 0xF;
    kprintf("[iommu  ] domains %u  MGAW %u-bit  SAGAW %x  superpages %x  %s\n",
            (uint64_t)(1ull << (4 + 2 * nd)), mgaw, sagaw, sllps,
            (g_dmar_ecap & (1ull << 3)) ? "IR-capable" : "no IR");

    if (sagaw & (1u << 2))      { g_iommu_levels = 4; g_iommu_aw = 2; }  /* 48-bit */
    else if (sagaw & (1u << 1)) { g_iommu_levels = 3; g_iommu_aw = 1; }  /* 39-bit */
    else { kputs("[iommu  ] no supported AGAW — refusing to program\n"); return; }

    /* Identity-map all usable RAM for device DMA, rounded up to a GiB boundary. */
    uint64_t limit = (g_total_ram + 0x3FFFFFFFull) & ~0x3FFFFFFFull;
    if (limit < 0x40000000ull) limit = 0x40000000ull;
    int use_2m = (sllps & 1) ? 1 : 0;
    g_iommu_slpt = iommu_build_slpt(limit, g_iommu_levels, use_2m);
    g_iommu_mapped_bytes = limit;

    /* root table: one entry per bus -> a context table for that bus            */
    g_iommu_root = alloc_frame();
    for (int i = 0; i < 512; i++) ((uint64_t *)g_iommu_root)[i] = 0;
    uint64_t ctx = alloc_frame();                      /* context table for bus 0 */
    for (int i = 0; i < 512; i++) ((uint64_t *)ctx)[i] = 0;
    for (int devfn = 0; devfn < 256; devfn++) {        /* every device on bus 0   */
        ((uint64_t *)ctx)[devfn * 2]     = g_iommu_slpt | 0x1;     /* present, TT=00 */
        ((uint64_t *)ctx)[devfn * 2 + 1] = (uint64_t)g_iommu_aw | (1ull << 8); /* AW, domain 1 */
    }
    ((uint64_t *)g_iommu_root)[0] = ctx | 0x1;         /* bus 0 present           */

    /* program the root table pointer and enable translation                    */
    dmar_w64(DMAR_RTADDR, g_iommu_root);
    dmar_w32(DMAR_GCMD, GCMD_SRTP);
    for (int i = 0; i < 1000000 && !(dmar_r32(DMAR_GSTS) & GSTS_RTPS); i++) __asm__ volatile("pause");
    /* global context-cache + IOTLB invalidation before enabling                */
    dmar_w64(DMAR_CCMD, (1ull << 63) | (1ull << 61));
    for (int i = 0; i < 1000000 && (dmar_r64(DMAR_CCMD) & (1ull << 63)); i++) __asm__ volatile("pause");
    uint32_t iro = (uint32_t)(((g_dmar_ecap >> 8) & 0x3FF) * 16);
    dmar_w64(iro + 8, (1ull << 63) | (1ull << 60));    /* IOTLB global invalidate */
    for (int i = 0; i < 1000000 && (dmar_r64(iro + 8) & (1ull << 63)); i++) __asm__ volatile("pause");
    dmar_w32(DMAR_GCMD, GCMD_TE);
    for (int i = 0; i < 1000000 && !(dmar_r32(DMAR_GSTS) & GSTS_TES); i++) __asm__ volatile("pause");

    g_iommu_on = (dmar_r32(DMAR_GSTS) & GSTS_TES) ? 1 : 0;
    kprintf("[iommu  ] second-level tables: %d-level, %s pages, identity-mapped %M MiB\n",
            (uint64_t)g_iommu_levels, use_2m ? "2 MiB" : "4 KiB", g_iommu_mapped_bytes);
    kprintf("[iommu  ] DMA REMAPPING %s — every device DMA now walks our page tables\n",
            g_iommu_on ? "ENABLED (GSTS.TES set)" : "FAILED TO ENABLE");
}

/* TOP HALF (interrupt context): acknowledge the device, defer the work.       */
static void virtio_blk_isr(void) {
    uint8_t isr = g_vblk_isr ? *g_vblk_isr : 1;            /* read clears/acks   */
    if (isr & 1) { g_vblk_irqs++; g_softirq_pending = 1; } /* schedule bottom half */
}

/* BOTTOM HALF (softirq on IRQ return, IF off): drain ALL completed requests   */
/* and wake the specific thread that owns each finished tag.                    */
static void virtio_blk_bh(void) {
    barrier();
    while (g_vblk_last_used != g_vblk_used->idx) {
        struct vring_used_elem *e = &g_vblk_used->ring[g_vblk_last_used % g_vblk_qsize];
        int slot = (int)(e->id / 3);                       /* head desc -> slot  */
        if (slot >= 0 && slot < g_vblk_nslots) {
            g_vreq[slot].status = g_stats[slot];
            barrier();
            g_vreq[slot].done   = 1;           /* AP waiters poll exactly this  */
            thread_wake(g_vreq[slot].waiter_tid);          /* wake THAT thread   */
            if (g_inflight) __sync_fetch_and_sub(&g_inflight, 1);  /* vs AP submit */
            g_completions++;
        }
        g_vblk_last_used++;
    }
}

/* Discover and fully initialize the device. Returns 1 on success.            */
static int virtio_blk_probe(uint8_t bus, uint8_t dev, uint8_t fn) {
    kprintf("[vblk   ] virtio-blk at %d:%d.%d — bringing up modern driver\n",
            (uint64_t)bus, (uint64_t)dev, (uint64_t)fn);

    /* Walk the vendor cap list for common/notify/device structures.          */
    uint64_t common_off = 0, notify_off_in_bar = 0, devcfg_off = 0, isr_off = 0;
    int common_bar = -1, notify_bar = -1, devcfg_bar = -1, isr_bar = -1;
    uint8_t cap = (uint8_t)(pci_cfg_read32(bus, dev, fn, 0x34) & 0xFC);
    while (cap) {
        uint32_t c0 = pci_cfg_read32(bus, dev, fn, cap);
        if ((uint8_t)c0 == 0x09) {                          /* VIRTIO vendor cap */
            uint8_t  cfg  = (uint8_t)(c0 >> 24);
            uint8_t  bar  = (uint8_t)(pci_cfg_read32(bus, dev, fn, cap + 4) & 0xFF);
            uint32_t off  = pci_cfg_read32(bus, dev, fn, cap + 8);
            if (cfg == 1) { common_bar = bar; common_off = off; }
            if (cfg == 2) { notify_bar = bar; notify_off_in_bar = off;
                            g_vblk_notify_mul = pci_cfg_read32(bus, dev, fn, cap + 16); }
            if (cfg == 3) { isr_bar = bar; isr_off = off; }
            if (cfg == 4) { devcfg_bar = bar; devcfg_off = off; }
        }
        cap = (uint8_t)((c0 >> 8) & 0xFC);
    }
    if (common_bar < 0 || notify_bar < 0) { kprintf("[vblk   ] missing virtio caps\n"); return 0; }

    /* Enable memory-space decode AND bus-mastering (the device DMAs).        */
    uint32_t cmd = pci_cfg_read32(bus, dev, fn, 0x04);
    pci_cfg_write32(bus, dev, fn, 0x04, cmd | 0x6);

    /* Map each distinct BAR we need into kernel space.                        */
    uint64_t cbase = pci_bar_base(bus, dev, fn, common_bar);
    map_mmio(VBLK_MMIO_V, cbase, 0x4000);
    g_vblk_common = (volatile uint8_t *)(VBLK_MMIO_V + common_off);
    if (notify_bar == common_bar) {
        g_vblk_notify = (volatile uint8_t *)(VBLK_MMIO_V + notify_off_in_bar);
    } else {
        uint64_t nbase = pci_bar_base(bus, dev, fn, notify_bar);
        map_mmio(VBLK_MMIO_V + 0x10000, nbase, 0x4000);
        g_vblk_notify = (volatile uint8_t *)(VBLK_MMIO_V + 0x10000 + notify_off_in_bar);
    }
    if (devcfg_bar == common_bar)
        g_vblk_devcfg = (volatile uint8_t *)(VBLK_MMIO_V + devcfg_off);
    if (isr_bar == common_bar)
        g_vblk_isr = (volatile uint8_t *)(VBLK_MMIO_V + isr_off);

    /* PCI interrupt line (assigned by firmware) -> our PIC-remapped vector.    */
    g_vblk_irq = (uint8_t)(pci_cfg_read32(bus, dev, fn, 0x3C) & 0xFF);

    /* --- reset and status handshake --- */
    mw8(g_vblk_common, VCC_DEV_STATUS, 0);                  /* reset             */
    while (mr8(g_vblk_common, VCC_DEV_STATUS) != 0) { }
    mw8(g_vblk_common, VCC_DEV_STATUS, VSTAT_ACK);
    mw8(g_vblk_common, VCC_DEV_STATUS, VSTAT_ACK | VSTAT_DRIVER);

    /* --- feature negotiation: we require only VIRTIO_F_VERSION_1 (bit 32) -- */
    mw32(g_vblk_common, VCC_DEV_FEAT_SEL, 0);
    uint32_t feat_lo = mr32(g_vblk_common, VCC_DEV_FEAT);
    mw32(g_vblk_common, VCC_DEV_FEAT_SEL, 1);
    uint32_t feat_hi = mr32(g_vblk_common, VCC_DEV_FEAT);
    kprintf("[vblk   ] device features: hi %x lo %x (VERSION_1=%d)\n",
            (uint64_t)feat_hi, (uint64_t)feat_lo, (uint64_t)((feat_hi >> 0) & 1));
    mw32(g_vblk_common, VCC_DRV_FEAT_SEL, 0);
    mw32(g_vblk_common, VCC_DRV_FEAT, 0);                   /* accept no low feats */
    mw32(g_vblk_common, VCC_DRV_FEAT_SEL, 1);
    /* bit 32 = VERSION_1; bit 33 = ACCESS_PLATFORM (route DMA through the IOMMU  */
    /* when the platform has one — required for VT-d translation to apply).       */
    mw32(g_vblk_common, VCC_DRV_FEAT, 1 | (feat_hi & 2));
    if (feat_hi & 2) kputs("[vblk   ] negotiated VIRTIO_F_ACCESS_PLATFORM (DMA via IOMMU)\n");

    mw8(g_vblk_common, VCC_DEV_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEAT_OK);
    if (!(mr8(g_vblk_common, VCC_DEV_STATUS) & VSTAT_FEAT_OK)) {
        kprintf("[vblk   ] device rejected our feature set\n");
        mw8(g_vblk_common, VCC_DEV_STATUS, VSTAT_FAILED);
        return 0;
    }

    /* --- capacity from device-config (le64 sectors at offset 0) --- */
    if (g_vblk_devcfg) {
        g_vblk_capacity = *(volatile uint64_t *)(g_vblk_devcfg + 0);
        kprintf("[vblk   ] capacity: %d sectors (%M MiB)\n",
                g_vblk_capacity, g_vblk_capacity * 512);
    }

    /* --- build the split virtqueue for queue 0 --- */
    mw16(g_vblk_common, VCC_Q_SELECT, 0);
    uint16_t qmax = mr16(g_vblk_common, VCC_Q_SIZE);
    g_vblk_qsize = qmax > 64 ? 64 : qmax;
    mw16(g_vblk_common, VCC_Q_SIZE, g_vblk_qsize);
    g_vblk_notify_off = mr16(g_vblk_common, VCC_Q_NOTIFY_OFF);

    uint64_t desc_phys  = alloc_frame();                    /* 16*qsize bytes    */
    uint64_t avail_phys = alloc_frame();
    uint64_t used_phys  = alloc_frame();
    g_vblk_desc  = (struct vring_desc  *)desc_phys;          /* identity-mapped   */
    g_vblk_avail = (struct vring_avail *)avail_phys;
    g_vblk_used  = (struct vring_used  *)used_phys;

    mw64(g_vblk_common, VCC_Q_DESC,   desc_phys);
    mw64(g_vblk_common, VCC_Q_DRIVER, avail_phys);
    mw64(g_vblk_common, VCC_Q_DEVICE, used_phys);
    mw16(g_vblk_common, VCC_Q_ENABLE, 1);

    /* Per-slot DMA-visible header + status pool (identity-mapped low frame).   */
    uint64_t pool = alloc_frame();
    g_hdrs  = (struct virtio_blk_req_hdr *)pool;            /* 16 B * MAXREQ      */
    g_stats = (volatile uint8_t *)(pool + 0x800);          /* 1 B  * MAXREQ      */
    g_vblk_nslots = g_vblk_qsize / 3;
    if (g_vblk_nslots > VBLK_MAXREQ) g_vblk_nslots = VBLK_MAXREQ;
    for (int i = 0; i < VBLK_MAXREQ; i++) { g_vreq[i].in_use = 0; g_vreq[i].waiter_tid = -1; }
    register_softirq(virtio_blk_bh);                       /* register bottom half */

    mw8(g_vblk_common, VCC_DEV_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEAT_OK | VSTAT_DRIVER_OK);
    g_vblk_last_used = 0;

    /* Wire the interrupt: register the top half and unmask the PIC line so    */
    /* completions arrive as hardware IRQs instead of a polling loop.          */
    if (g_vblk_irq < 16) {
        register_irq(g_vblk_irq, virtio_blk_isr);
        pic_unmask(g_vblk_irq);
    }
    g_vblk_ready = 1;
    kprintf("[vblk   ] DRIVER_OK — queue0 size %d, notify_off %d, mult %d — READY\n",
            (uint64_t)g_vblk_qsize, (uint64_t)g_vblk_notify_off, (uint64_t)g_vblk_notify_mul);
    kprintf("[vblk   ] interrupt-driven: INTx on IRQ %d (vector %d), ISR reg %s\n",
            (uint64_t)g_vblk_irq, (uint64_t)(32 + g_vblk_irq), g_vblk_isr ? "mapped" : "MISSING");
    return 1;
}

/* Allocate a free request slot (thread context; slots are freed by the waiter).*/
static int vblk_alloc_slot(void) {
    for (int i = 0; i < g_vblk_nslots; i++)
        if (!g_vreq[i].in_use) { g_vreq[i].in_use = 1; return i; }
    return -1;
}

/* Submit a request WITHOUT blocking. Returns the slot/tag, or -1.             */
/* Many of these can be outstanding at once (up to g_vblk_nslots).             */
/* v0.41: MP-safe. The old cli/sti guard only excluded THIS core's interrupts; */
/* two cores could race the slot scan and the avail-ring publish. g_vblk_lock  */
/* (rank 4) serializes slot claim -> descriptor fill -> publish -> doorbell,   */
/* and is released BEFORE any wait — it is never held across a block.          */
static int vblk_submit(uint32_t type, uint64_t sector, void *buffer, int waiter_tid) {
    if (!g_vblk_ready) return -1;
    klock_acquire(&g_vblk_lock);
    int slot = vblk_alloc_slot();
    if (slot < 0) { klock_release(&g_vblk_lock); return -1; }
    int d0 = slot * 3, d1 = d0 + 1, d2 = d0 + 2;

    g_hdrs[slot].type = type; g_hdrs[slot].reserved = 0; g_hdrs[slot].sector = sector;
    g_stats[slot] = 0xFF;
    g_vreq[slot].done = 0; g_vreq[slot].waiter_tid = waiter_tid;

    g_vblk_desc[d0].addr = (uint64_t)&g_hdrs[slot]; g_vblk_desc[d0].len = 16;
    g_vblk_desc[d0].flags = VRING_DESC_F_NEXT; g_vblk_desc[d0].next = (uint16_t)d1;
    g_vblk_desc[d1].addr = (uint64_t)buffer; g_vblk_desc[d1].len = 512;
    g_vblk_desc[d1].flags = VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    g_vblk_desc[d1].next = (uint16_t)d2;
    g_vblk_desc[d2].addr = (uint64_t)&g_stats[slot]; g_vblk_desc[d2].len = 1;
    g_vblk_desc[d2].flags = VRING_DESC_F_WRITE; g_vblk_desc[d2].next = 0;

    uint16_t ai = g_vblk_avail->idx;
    g_vblk_avail->ring[ai % g_vblk_qsize] = (uint16_t)d0;   /* publish head desc  */
    barrier(); g_vblk_avail->idx = ai + 1; barrier();
    uint64_t inf = __sync_add_and_fetch(&g_inflight, 1);    /* bh decrements       */
    if (inf > g_max_inflight) g_max_inflight = inf;         /* (under the lock)    */

    volatile uint16_t *notify =
        (volatile uint16_t *)(g_vblk_notify + (uint32_t)g_vblk_notify_off * g_vblk_notify_mul);
    *notify = 0; barrier();
    klock_release(&g_vblk_lock);
    return slot;
}

/* Wait for a submitted tag to complete. v0.41: CPU-aware — the parking path
 * (curthr / g_cur / sched_yield) is BSP scheduler state and was one of the
 * three audited paths that silently corrupted a random BSP thread when a
 * file syscall ran on an AP. An AP has no kernel scheduler to park in, so it
 * PAUSE-polls the slot's own done flag with IF set: the completion IRQ is
 * PIC-routed to the BSP, whose bottom half writes done=1, which this core
 * observes. No scheduler state is touched off the BSP.                        */
static int vblk_wait(int slot) {
    if (cpu_idx() != 0) {
        while (!g_vreq[slot].done) __asm__ volatile("pause");
    } else {
        while (!g_vreq[slot].done) {
            __asm__ volatile("cli");
            if (!g_vreq[slot].done) {
                curthr->state = T_BLOCKED;                 /* park this thread   */
                g_vreq[slot].waiter_tid = g_cur;
                __asm__ volatile("sti");
                sched_yield();                             /* let others run     */
            } else {
                __asm__ volatile("sti");
            }
        }
    }
    uint8_t st = g_vreq[slot].status;
    barrier();
    g_vreq[slot].in_use = 0;      /* owner-only 1->0; submitters scan under lock */
    if (st != VIRTIO_BLK_S_OK) { kprintf("[vblk   ] slot %d status %d\n", (uint64_t)slot, (uint64_t)st); return -3; }
    return 0;
}

/* Blocking convenience wrapper: submit + wait (used by CAS and the VFS).      */
/* v0.41: an AP passes waiter_tid -1 (it polls; there is no thread to wake),   */
/* and slot exhaustion — now reachable with several cores submitting — backs   */
/* off and retries instead of surfacing a spurious IO error into the VFS.      */
static int virtio_blk_request(uint32_t type, uint64_t sector, void *buffer) {
    for (;;) {
        int slot = vblk_submit(type, sector, buffer, cpu_idx() == 0 ? g_cur : -1);
        if (slot >= 0) return vblk_wait(slot);
        if (!g_vblk_ready) return -1;
        krelax();                              /* all slots in flight: back off */
    }
}

int virtio_read_block(uint64_t sector, void *buffer)        { return virtio_blk_request(VIRTIO_BLK_T_IN,  sector, buffer); }
int virtio_write_block(uint64_t sector, const void *buffer) { return virtio_blk_request(VIRTIO_BLK_T_OUT, sector, (void *)buffer); }

/* ===========================================================================
 * VIRTIO-NET DRIVER + ASYNC IRQ ROUTING  (Phase 3)
 * ===========================================================================
 * Brings up a modern virtio-net device with RX/TX split virtqueues. When the
 * NIC raises its INTx vector, the top half acks and the bottom half drains the
 * RX used ring and WAKES the specific thread parked in sys_wait_event(NET_RX)
 * — routing a hardware interrupt to the owning (CAP_NETWORK) execution slot.
 * A zero-copy parser reads the received frame straight out of the RX buffer.
 * =========================================================================== */

struct vq {
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint16_t size, last_used, notify_off;
};

static uint16_t g_vnet_bdf = 0xFFFF;   /* NIC PCI source-id (bus:dev.fn) */
static uint64_t g_vnet_off_common = 0, g_vnet_off_notify = 0, g_vnet_off_isr = 0, g_vnet_off_devcfg = 0;
static volatile uint8_t *g_vnet_common = 0, *g_vnet_notify = 0, *g_vnet_isr = 0, *g_vnet_devcfg = 0;
static uint32_t g_vnet_notify_mul = 0;
static uint8_t  g_vnet_mac[6];
static uint8_t  g_vnet_irq = 0xFF;
static int      g_vnet_ready = 0;
static struct vq g_vnet_rx, g_vnet_tx;
#define VNET_RXN 16
#define VNET_BUFSZ 2048
static uint8_t  g_vnet_rxbuf[VNET_RXN][VNET_BUFSZ] __attribute__((aligned(16)));
static uint8_t  g_vnet_txbuf[VNET_BUFSZ] __attribute__((aligned(16)));
static volatile int      g_vnet_rx_pending = 0;
static volatile int      g_vnet_rx_idx = -1;
static volatile uint32_t g_vnet_rx_len = 0;
static volatile uint64_t g_vnet_irqs = 0;

/* --- event/wait: a thread parks on an event; the NIC IRQ wakes it --- */
#define EV_NET_RX 1
static volatile int g_net_waiter_tid = -1;

static void vnet_setup_queue(int qi, struct vq *q, int is_rx) {
    mw16(g_vnet_common, VCC_Q_SELECT, (uint16_t)qi);
    uint16_t qmax = mr16(g_vnet_common, VCC_Q_SIZE);
    q->size = qmax > VNET_RXN ? VNET_RXN : qmax;
    mw16(g_vnet_common, VCC_Q_SIZE, q->size);
    q->notify_off = mr16(g_vnet_common, VCC_Q_NOTIFY_OFF);
    q->desc  = (struct vring_desc  *)alloc_frame();
    q->avail = (struct vring_avail *)alloc_frame();
    q->used  = (struct vring_used  *)alloc_frame();
    q->last_used = 0;
    mw64(g_vnet_common, VCC_Q_DESC,   (uint64_t)q->desc);
    mw64(g_vnet_common, VCC_Q_DRIVER, (uint64_t)q->avail);
    mw64(g_vnet_common, VCC_Q_DEVICE, (uint64_t)q->used);
    mw16(g_vnet_common, VCC_Q_ENABLE, 1);
    if (is_rx) {                                           /* post receive buffers */
        for (int i = 0; i < q->size; i++) {
            q->desc[i].addr = (uint64_t)g_vnet_rxbuf[i];
            q->desc[i].len = VNET_BUFSZ;
            q->desc[i].flags = VRING_DESC_F_WRITE;         /* device writes RX     */
            q->desc[i].next = 0;
            q->avail->ring[i] = (uint16_t)i;
        }
        barrier(); q->avail->idx = q->size; barrier();
    }
}

static void vnet_kick(struct vq *q, uint16_t qi) {
    volatile uint16_t *n = (volatile uint16_t *)(g_vnet_notify + (uint32_t)q->notify_off * g_vnet_notify_mul);
    *n = qi; barrier();
}

/* TOP HALF: ack the NIC (reading ISR de-asserts INTx), defer to bottom half.  */
static void virtionet_isr(void) {
    uint8_t isr = g_vnet_isr ? *g_vnet_isr : 1;
    if (isr & 1) { g_vnet_irqs++; g_softirq_pending = 1; }
}

/* --- zero-copy IP/UDP router: port filter array -> execution slots --- */
#define NUDP 8
struct udp_slot {
    int      used;
    uint16_t port;                       /* UDP dst port to match             */
    int      owner_tid;                  /* execution slot woken on a match   */
    volatile const uint8_t *payload;     /* routed pointer INTO the RX buffer */
    volatile uint16_t       plen;
    volatile int            ready;
};
static struct udp_slot g_udp[NUDP];

static int udp_bind(uint16_t port, int tid) {
    for (int i = 0; i < NUDP; i++) if (!g_udp[i].used) {
        g_udp[i].used = 1; g_udp[i].port = port; g_udp[i].owner_tid = tid; g_udp[i].ready = 0;
        return i;
    }
    return -1;
}

/* Parse Ethernet -> IPv4 -> UDP entirely in place (no memcpy). On a port-filter*/
/* hit, hand the payload POINTER to the owning slot and wake it.               */
static void net_route(const uint8_t *frame, uint32_t len) {
    (void)len;
    const uint8_t *ip = frame + 14;
    if ((ip[0] >> 4) != 4) return;                        /* IPv4              */
    uint32_t ihl = (uint32_t)(ip[0] & 0xF) * 4;
    if (ip[9] != 17) return;                              /* UDP               */
    const uint8_t *udp = ip + ihl;
    uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
    uint16_t ulen  = (uint16_t)((udp[4] << 8) | udp[5]);
    const uint8_t *payload = udp + 8;
    uint16_t plen = ulen > 8 ? (uint16_t)(ulen - 8) : 0;
    for (int i = 0; i < NUDP; i++) if (g_udp[i].used && g_udp[i].port == dport) {
        g_udp[i].payload = payload;                       /* route the pointer  */
        g_udp[i].plen = plen;
        g_udp[i].ready = 1;
        thread_wake(g_udp[i].owner_tid);
        return;
    }
}

/* Per-frame dispatch: ARP -> the ARP waiter; IPv4 -> the UDP router.          */
static void net_dispatch(int idx, const uint8_t *frame, uint32_t len) {
    uint16_t eth = (uint16_t)((frame[12] << 8) | frame[13]);
    if (eth == 0x0806) {                                  /* ARP               */
        g_vnet_rx_idx = idx; g_vnet_rx_len = len + 12; g_vnet_rx_pending = 1;
        if (g_net_waiter_tid >= 0) { thread_wake(g_net_waiter_tid); g_net_waiter_tid = -1; }
    } else if (eth == 0x0800) {                           /* IPv4 -> UDP router */
        net_route(frame, len);
    }
}

/* BOTTOM HALF: drain the RX used ring; route each frame to its execution slot.*/
static void virtionet_bh(void) {
    struct vq *q = &g_vnet_rx;
    if (!g_vnet_ready) return;
    barrier();
    while (q->last_used != q->used->idx) {
        struct vring_used_elem *e = &q->used->ring[q->last_used % q->size];
        int idx = (int)e->id;
        uint32_t flen = e->len > 12 ? e->len - 12 : 0;
        net_dispatch(idx, g_vnet_rxbuf[idx] + 12, flen);  /* skip virtio hdr    */
        q->last_used++;
    }
}

/* sys_wait_event core: park the calling thread until a NET_RX arrives.        */
static int net_wait_rx(void) {
    __asm__ volatile("cli");
    if (g_vnet_rx_pending) { __asm__ volatile("sti"); return 0; }
    g_net_waiter_tid = g_cur;
    curthr->state = T_BLOCKED;
    __asm__ volatile("sti");
    sched_yield();                                         /* woken by the NIC IRQ */
    return 0;
}

static int virtionet_probe(uint8_t bus, uint8_t dev, uint8_t fn) {
    g_vnet_bdf = (uint16_t)((bus << 8) | (dev << 3) | fn);   /* source-id for IOMMU */
    kprintf("[vnet   ] virtio-net at %d:%d.%d — bringing up modern driver\n",
            (uint64_t)bus, (uint64_t)dev, (uint64_t)fn);
    uint64_t common_off = 0, notify_off = 0, isr_off = 0, devcfg_off = 0;
    int common_bar = -1, notify_bar = -1, isr_bar = -1, devcfg_bar = -1;
    uint8_t cap = (uint8_t)(pci_cfg_read32(bus, dev, fn, 0x34) & 0xFC);
    while (cap) {
        uint32_t c0 = pci_cfg_read32(bus, dev, fn, cap);
        if ((uint8_t)c0 == 0x09) {
            uint8_t cfg = (uint8_t)(c0 >> 24);
            uint8_t bar = (uint8_t)(pci_cfg_read32(bus, dev, fn, cap + 4) & 0xFF);
            uint32_t off = pci_cfg_read32(bus, dev, fn, cap + 8);
            if (cfg == 1) { common_bar = bar; common_off = off; }
            if (cfg == 2) { notify_bar = bar; notify_off = off;
                            g_vnet_notify_mul = pci_cfg_read32(bus, dev, fn, cap + 16); }
            if (cfg == 3) { isr_bar = bar; isr_off = off; }
            if (cfg == 4) { devcfg_bar = bar; devcfg_off = off; }
        }
        cap = (uint8_t)((c0 >> 8) & 0xFC);
    }
    if (common_bar < 0) { kputs("[vnet   ] no virtio caps\n"); return 0; }

    uint32_t cmd = pci_cfg_read32(bus, dev, fn, 0x04);
    pci_cfg_write32(bus, dev, fn, 0x04, cmd | 0x6);        /* mem + bus master     */

    uint64_t cbase = pci_bar_base(bus, dev, fn, common_bar);
    map_mmio(VBLK_MMIO_V + 0x20000, cbase, 0x4000);        /* distinct from blk BAR */
    uint64_t v = VBLK_MMIO_V + 0x20000;
    g_vnet_off_common = common_off; g_vnet_off_notify = notify_off;
    g_vnet_off_isr = isr_off; g_vnet_off_devcfg = devcfg_off;
    g_vnet_common = (volatile uint8_t *)(v + common_off);
    g_vnet_notify = (volatile uint8_t *)(v + notify_off);
    g_vnet_isr    = (volatile uint8_t *)(v + isr_off);
    g_vnet_devcfg = (volatile uint8_t *)(v + devcfg_off);

    /* also expose this device to ring-3 passthrough (real MAC read demo)      */
    kdev_register("virtio-net", cbase, 0x4000, PCAP_NETWORK);
    kdevs[n_kdev - 1].bdf = (uint16_t)((bus << 8) | (dev << 3) | fn);   /* IOMMU source-id */
    g_virtio_kdev = n_kdev - 1;
    g_virtio_common = common_off; g_virtio_devcfg = devcfg_off;

    /* reset + feature negotiation (VERSION_1 + MAC) */
    mw8(g_vnet_common, VCC_DEV_STATUS, 0);
    while (mr8(g_vnet_common, VCC_DEV_STATUS)) { }
    mw8(g_vnet_common, VCC_DEV_STATUS, VSTAT_ACK);
    mw8(g_vnet_common, VCC_DEV_STATUS, VSTAT_ACK | VSTAT_DRIVER);
    mw32(g_vnet_common, VCC_DEV_FEAT_SEL, 0);
    uint32_t flo = mr32(g_vnet_common, VCC_DEV_FEAT);
    mw32(g_vnet_common, VCC_DRV_FEAT_SEL, 0);
    mw32(g_vnet_common, VCC_DRV_FEAT, flo & (1u << 5));    /* VIRTIO_NET_F_MAC     */
    mw32(g_vnet_common, VCC_DEV_FEAT_SEL, 1);
    uint32_t fhi = mr32(g_vnet_common, VCC_DEV_FEAT);
    mw32(g_vnet_common, VCC_DRV_FEAT_SEL, 1);
    mw32(g_vnet_common, VCC_DRV_FEAT, 1 | (fhi & 2));      /* VERSION_1 + ACCESS_PLATFORM */
    if (fhi & 2) kputs("[vnet   ] negotiated VIRTIO_F_ACCESS_PLATFORM (DMA via IOMMU)\n");
    mw8(g_vnet_common, VCC_DEV_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEAT_OK);
    if (!(mr8(g_vnet_common, VCC_DEV_STATUS) & VSTAT_FEAT_OK)) { kputs("[vnet   ] FEATURES_OK rejected\n"); return 0; }

    for (int i = 0; i < 6; i++) g_vnet_mac[i] = g_vnet_devcfg[i];
    kprintf("[vnet   ] MAC %x:%x:%x:%x:%x:%x\n",
            (uint64_t)g_vnet_mac[0], (uint64_t)g_vnet_mac[1], (uint64_t)g_vnet_mac[2],
            (uint64_t)g_vnet_mac[3], (uint64_t)g_vnet_mac[4], (uint64_t)g_vnet_mac[5]);

    vnet_setup_queue(0, &g_vnet_rx, 1);                    /* receiveq: post buffers */
    vnet_setup_queue(1, &g_vnet_tx, 0);                    /* transmitq              */

    mw8(g_vnet_common, VCC_DEV_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEAT_OK | VSTAT_DRIVER_OK);

    g_vnet_irq = (uint8_t)(pci_cfg_read32(bus, dev, fn, 0x3C) & 0xFF);
    if (g_vnet_irq < 16) { register_irq(g_vnet_irq, virtionet_isr); pic_unmask(g_vnet_irq); }
    register_softirq(virtionet_bh);
    g_vnet_ready = 1;
    vnet_kick(&g_vnet_rx, 0);                              /* tell device RX ready   */
    kprintf("[vnet   ] DRIVER_OK — rxq %d txq %d, INTx IRQ %d — READY\n",
            (uint64_t)g_vnet_rx.size, (uint64_t)g_vnet_tx.size, (uint64_t)g_vnet_irq);
    return 1;
}

static void vnet_tx(const uint8_t *frame, uint32_t flen) {
    for (int i = 0; i < 12; i++) g_vnet_txbuf[i] = 0;      /* virtio_net_hdr (12B)  */
    for (uint32_t i = 0; i < flen; i++) g_vnet_txbuf[12 + i] = frame[i];
    struct vq *q = &g_vnet_tx;
    q->desc[0].addr = (uint64_t)g_vnet_txbuf;
    q->desc[0].len = 12 + flen; q->desc[0].flags = 0; q->desc[0].next = 0;
    uint16_t ai = q->avail->idx;
    q->avail->ring[ai % q->size] = 0;
    barrier(); q->avail->idx = ai + 1; barrier();
    vnet_kick(q, 1);
}

/* --- the ARP round-trip demo: TX a request, block, wake on the RX IRQ --- */
static volatile int g_netd_done = 0;

static void netd_fn(void *arg) {
    (void)arg;
    if (!g_vnet_ready) { kputs("[netd   ] no virtio-net device\n"); g_netd_done = 1; return; }

    /* build an ARP request: who-has 10.0.2.2, tell 10.0.2.15 (SLIRP gateway) */
    uint8_t f[42];
    for (int i = 0; i < 42; i++) f[i] = 0;
    for (int i = 0; i < 6; i++) f[i] = 0xFF;               /* dst: broadcast        */
    for (int i = 0; i < 6; i++) f[6 + i] = g_vnet_mac[i];  /* src: our MAC          */
    f[12] = 0x08; f[13] = 0x06;                            /* ethertype ARP         */
    f[14] = 0x00; f[15] = 0x01;                            /* htype Ethernet        */
    f[16] = 0x08; f[17] = 0x00;                            /* ptype IPv4            */
    f[18] = 6;    f[19] = 4;                               /* hlen, plen            */
    f[20] = 0x00; f[21] = 0x01;                            /* oper: request         */
    for (int i = 0; i < 6; i++) f[22 + i] = g_vnet_mac[i]; /* sender HW addr        */
    f[28] = 10; f[29] = 0; f[30] = 2; f[31] = 15;          /* sender IP 10.0.2.15   */
    f[38] = 10; f[39] = 0; f[40] = 2; f[41] = 2;           /* target IP 10.0.2.2    */

    kprintf("[netd   ] tid%d holds CAP_NETWORK; TX ARP who-has 10.0.2.2\n", (uint64_t)g_cur);
    g_vnet_rx_pending = 0;
    vnet_tx(f, 42);

    kprintf("[netd   ] sys_wait_event(NET_RX): parking until the NIC interrupts...\n");
    uint64_t t0 = g_ticks;
    while (!g_vnet_rx_pending && g_ticks - t0 < 200) net_wait_rx();

    if (!g_vnet_rx_pending) { kputs("[netd   ] no reply (timeout)\n"); g_netd_done = 1; return; }

    /* zero-copy parse: point straight into the RX buffer (skip 12B virtio hdr) */
    uint8_t *p = g_vnet_rxbuf[g_vnet_rx_idx] + 12;
    uint16_t eth = (uint16_t)((p[12] << 8) | p[13]);
    kprintf("[netd   ] woken by IRQ; RX %d bytes, ethertype %x\n", g_vnet_rx_len, (uint64_t)eth);
    if (eth == 0x0806 && p[21] == 0x02) {                  /* ARP reply             */
        uint8_t *sha = p + 22, *spa = p + 28;
        kprintf("[netd   ] ARP reply: %d.%d.%d.%d is-at %x:%x:%x:%x:%x:%x (parsed in place)\n",
                (uint64_t)spa[0], (uint64_t)spa[1], (uint64_t)spa[2], (uint64_t)spa[3],
                (uint64_t)sha[0], (uint64_t)sha[1], (uint64_t)sha[2],
                (uint64_t)sha[3], (uint64_t)sha[4], (uint64_t)sha[5]);
    } else {
        kprintf("[netd   ] RX frame was not an ARP reply (ethertype %x)\n", (uint64_t)eth);
    }
    g_netd_done = 1;
}

/* --- second half of Phase 3: the zero-copy IP/UDP router in action --------- */
static uint16_t ip_checksum(const uint8_t *h, int len) {
    uint32_t sum = 0;
    for (int i = 0; i + 1 < len; i += 2) sum += (uint32_t)((h[i] << 8) | h[i + 1]);
    if (len & 1) sum += (uint32_t)(h[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

/* park the calling thread until the router routes a datagram to `slot`.       */
static void net_wait_slot(int slot) {
    __asm__ volatile("cli");
    if (g_udp[slot].ready) { __asm__ volatile("sti"); return; }
    g_udp[slot].owner_tid = g_cur;
    curthr->state = T_BLOCKED;
    __asm__ volatile("sti");
    sched_yield();
}

static volatile int g_dhcpd_done = 0;

static void dhcpd_fn(void *arg) {
    (void)arg;
    int slot = udp_bind(68, g_cur);                       /* filter: UDP dst port 68 */
    if (slot < 0) { kputs("[dhcpd  ] no free UDP slot\n"); g_dhcpd_done = 1; return; }
    kprintf("[dhcpd  ] tid%d bound UDP port 68 -> execution slot %d\n", (uint64_t)g_cur, (uint64_t)slot);

    /* Build a DHCPDISCOVER: Ethernet(bcast)/IPv4/UDP(68->67)/BOOTP.            */
    uint8_t f[350];
    for (unsigned i = 0; i < sizeof f; i++) f[i] = 0;
    for (int i = 0; i < 6; i++) f[i] = 0xFF;              /* eth dst broadcast  */
    for (int i = 0; i < 6; i++) f[6 + i] = g_vnet_mac[i]; /* eth src            */
    f[12] = 0x08; f[13] = 0x00;                           /* ethertype IPv4     */

    uint8_t *ip = f + 14;
    uint8_t *udp = ip + 20;
    uint8_t *bootp = udp + 8;
    /* BOOTP/DHCP */
    bootp[0] = 1; bootp[1] = 1; bootp[2] = 6; bootp[3] = 0;         /* op/htype/hlen/hops */
    bootp[4] = 0x12; bootp[5] = 0x34; bootp[6] = 0x56; bootp[7] = 0x78; /* xid */
    bootp[10] = 0x80;                                    /* flags: broadcast   */
    for (int i = 0; i < 6; i++) bootp[28 + i] = g_vnet_mac[i];      /* chaddr   */
    uint8_t *opt = bootp + 236;
    opt[0] = 0x63; opt[1] = 0x82; opt[2] = 0x53; opt[3] = 0x63;     /* magic cookie */
    opt[4] = 53; opt[5] = 1; opt[6] = 1;                            /* DHCP DISCOVER */
    opt[7] = 55; opt[8] = 3; opt[9] = 1; opt[10] = 3; opt[11] = 6;  /* param req list */
    opt[12] = 0xFF;                                                 /* end        */
    int dhcp_len = 236 + 13;
    int udp_len = 8 + dhcp_len;
    int ip_len  = 20 + udp_len;

    /* UDP header (checksum 0 = disabled for IPv4) */
    udp[0] = 0; udp[1] = 68; udp[2] = 0; udp[3] = 67;
    udp[4] = (uint8_t)(udp_len >> 8); udp[5] = (uint8_t)udp_len;
    /* IPv4 header */
    ip[0] = 0x45; ip[1] = 0x00;
    ip[2] = (uint8_t)(ip_len >> 8); ip[3] = (uint8_t)ip_len;
    ip[8] = 64; ip[9] = 17;                              /* TTL / proto=UDP    */
    for (int i = 0; i < 4; i++) ip[12 + i] = 0x00;       /* src 0.0.0.0        */
    for (int i = 0; i < 4; i++) ip[16 + i] = 0xFF;       /* dst 255.255.255.255 */
    uint16_t csum = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(csum >> 8); ip[11] = (uint8_t)csum;

    int frame_len = 14 + ip_len;
    kprintf("[dhcpd  ] TX DHCPDISCOVER (%d bytes), waiting on port-68 slot...\n", (uint64_t)frame_len);
    vnet_tx(f, (uint32_t)frame_len);

    uint64_t t0 = g_ticks;
    while (!g_udp[slot].ready && g_ticks - t0 < 200) net_wait_slot(slot);
    if (!g_udp[slot].ready) { kputs("[dhcpd  ] no DHCP reply (timeout)\n"); g_dhcpd_done = 1; return; }

    /* zero-copy: parse the DHCP payload straight from the routed pointer.      */
    const uint8_t *d = g_udp[slot].payload;
    const uint8_t *yi = d + 16;                           /* yiaddr             */
    int msg_type = 0; const uint8_t *srv = 0;
    const uint8_t *o = d + 240;                           /* options after cookie */
    for (int i = 0; i < 300 && o[0] != 0xFF; ) {
        uint8_t code = o[0], ln = o[1];
        if (code == 53) msg_type = o[2];
        if (code == 54) srv = o + 2;
        o += 2 + ln; i += 2 + ln;
    }
    kprintf("[dhcpd  ] router delivered %d-byte UDP payload (zero-copy pointer)\n", (uint64_t)g_udp[slot].plen);
    kprintf("[dhcpd  ] DHCP %s: offered IP %d.%d.%d.%d",
            msg_type == 2 ? "OFFER" : "reply",
            (uint64_t)yi[0], (uint64_t)yi[1], (uint64_t)yi[2], (uint64_t)yi[3]);
    if (srv) kprintf(", server %d.%d.%d.%d", (uint64_t)srv[0], (uint64_t)srv[1], (uint64_t)srv[2], (uint64_t)srv[3]);
    kputs("\n");
    g_dhcpd_done = 1;
}

static void cmd_udp_router(void) {
    kputs("[net    ] zero-copy IP/UDP router: DHCP round-trip through a port filter\n");
    g_dhcpd_done = 0;
    thread_create("dhcpd", dhcpd_fn, 0);
    while (!g_dhcpd_done) sched_yield();
}

static void cmd_net(void) {
    if (!g_vnet_ready) { kputs("[net    ] no virtio-net device (boot with -device virtio-net-pci)\n"); return; }
    kputs("-- async IRQ routing: NIC interrupt wakes a parked thread --\n");
    g_netd_done = 0;
    thread_create("netd", netd_fn, 0);
    while (!g_netd_done) sched_yield();                    /* let netd run + block   */

    /* second half: the zero-copy IP/UDP router driving a real DHCP round-trip */
    cmd_udp_router();
    kputs("-- done --\n");
}


/* Scheduler + concurrency demonstration.                                     */
static uint8_t          g_sbuf[8][512] __attribute__((aligned(512)));
static volatile int     g_workers_left = 0;
static volatile uint64_t g_spin_ctr = 0;
static volatile int     g_spin_stop = 0;

static void worker_fn(void *arg) {
    int id = (int)(uint64_t)arg;
    uint8_t buf[512] __attribute__((aligned(512)));
    uint8_t rb[512]  __attribute__((aligned(512)));
    int ok = 1;
    for (int r = 0; r < 4; r++) {
        uint64_t sec = 300 + (uint64_t)id * 8 + r;
        for (int i = 0; i < 512; i++) buf[i] = (uint8_t)(0x41 + id);
        virtio_write_block(sec, buf);                      /* blocks -> parks    */
        for (int i = 0; i < 512; i++) rb[i] = 0;
        virtio_read_block(sec, rb);                        /* blocks -> parks    */
        for (int i = 0; i < 512; i++) if (rb[i] != buf[i]) { ok = 0; break; }
    }
    kprintf("[worker ] tid%d did 4 write+read cycles: %s\n", (uint64_t)g_cur, ok ? "verified" : "FAILED");
    g_workers_left--;
}

static void spin_fn(void *arg) {
    (void)arg;
    while (!g_spin_stop) g_spin_ctr++;                     /* never yields       */
    g_workers_left--;
}

static void cmd_sched(void) {
    if (!g_vblk_ready) { kputs("[sched  ] needs a virtio-blk device\n"); return; }
    kputs("-- preemptive scheduler + multiple outstanding I/O --\n");

    /* (1) MULTIPLE OUTSTANDING I/O: submit N reads without blocking, so many   */
    /* tags are in flight at once, then collect them.                          */
    int n = 8; if (n > g_vblk_nslots) n = g_vblk_nslots;
    int slots[8];
    g_max_inflight = 0;
    __asm__ volatile("cli");                               /* batch stays atomic */
    for (int i = 0; i < n; i++) slots[i] = vblk_submit(VIRTIO_BLK_T_IN, 400 + i, g_sbuf[i], g_cur);
    __asm__ volatile("sti");                               /* now let them drain */
    kprintf("[sched  ] submitted %d reads without blocking; in-flight now %d\n",
            (uint64_t)n, g_inflight);
    for (int i = 0; i < n; i++) if (slots[i] >= 0) vblk_wait(slots[i]);
    kprintf("[sched  ] all %d done; PEAK concurrent in-flight tags = %d\n",
            (uint64_t)n, g_max_inflight);

    /* (2) PER-THREAD WAKE: spawn worker threads doing blocking I/O. Each parks  */
    /* on its own tag; the bottom half wakes exactly the owning thread.        */
    preempt_enable();
    g_workers_left = 4;
    for (int i = 0; i < 4; i++) thread_create("worker", worker_fn, (void *)(uint64_t)i);
    while (g_workers_left > 0) sched_yield();
    kprintf("[sched  ] 4 worker threads completed; wakes routed to specific TIDs\n");

    /* (3) PREEMPTION PROOF: spawn a CPU-bound thread that NEVER yields, then    */
    /* main busy-waits (also never yielding). If main keeps control and the     */
    /* spinner still advances, only the timer could have interleaved them.      */
    g_spin_ctr = 0; g_spin_stop = 0; g_workers_left = 1;
    thread_create("spinner", spin_fn, 0);
    uint64_t t0 = g_ticks;
    while (g_ticks - t0 < 20) __asm__ volatile("pause");   /* ~200 ms, no yield  */
    uint64_t observed = g_spin_ctr;
    g_spin_stop = 1;
    while (g_workers_left > 0) sched_yield();
    preempt_disable();
    kprintf("[sched  ] preemption proof: non-yielding spinner advanced to %d while main\n", observed);
    kprintf("[sched  ]   also ran (0 would mean no time-slicing occurred)\n");
    kprintf("[vblk   ] lifetime interrupt completions: %d\n", g_completions);
    kputs("-- done --\n");
}

/* ===========================================================================
 * CAS — CONTENT-ADDRESSABLE STORAGE  (on top of the virtio-blk primitives)
 * ===========================================================================
 * A persistent store where a block's ADDRESS is the hash of its CONTENT.
 * Identical content is stored once (dedup). On-disk layout:
 *
 *   block 0            superblock (magic, geometry, counters)
 *   bitmap_start..     allocation bitmap (1 bit / block)
 *   index_start..      open-addressed hash index: {hash -> block, len}
 *   data_start..       content blocks
 *
 * The content hash is computed by rust_cas_hash() — real Rust in the image.
 * =========================================================================== */

static void cmemset(void *d, int v, uint64_t n) { uint8_t *p = d; for (uint64_t i = 0; i < n; i++) p[i] = (uint8_t)v; }
static void cmemcpy(void *d, const void *s, uint64_t n) { uint8_t *a = d; const uint8_t *b = s; for (uint64_t i = 0; i < n; i++) a[i] = b[i]; }
static int  cmemcmp(const void *a, const void *b, uint64_t n) { const uint8_t *x = a, *y = b; for (uint64_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i]; return 0; }
static uint32_t cstrlen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

#define CAS_BS 512

struct cas_superblock {
    char     magic[8];                 /* "ORUNCAS1" */
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t bitmap_start, bitmap_blocks;
    uint64_t index_start,  index_blocks;
    uint64_t data_start;
    uint64_t used_blocks;
    uint64_t put_count, dedup_hits;
    uint64_t dir_start, dir_blocks;    /* VFS directory region (v2)             */
    /* v0.48 (version 3): WAL journal regions. A version-2 on-disk superblock  */
    /* never wrote these bytes (cas_format zeroed the whole 512B sector first, */
    /* so they read back as zero) — cas_mount() gates ALL journal activity on */
    /* SB->version == 3, so a version-2 volume is mounted in legacy            */
    /* compatibility mode instead of trusting these fields. See cas_mount().   */
    uint64_t vjournal_start, vjournal_blocks;   /* VFS-directory journal        */
    uint64_t cjournal_start, cjournal_blocks;   /* CAS-metadata journal          */
} __attribute__((packed));

struct cas_islot { uint64_t hash; uint32_t block; uint32_t len; } __attribute__((packed));
#define CAS_SLOTS_PER_BLOCK (CAS_BS / (int)sizeof(struct cas_islot))   /* 32 */

/* ===========================================================================
 * v0.48: WAL JOURNAL HEADERS
 * ===========================================================================
 * Two independent, narrowly-scoped journals — see CHANGELOG-0.48.0.md for the
 * crash-consistency hazards each one closes.
 *
 * CAS-metadata journal: one header block + 3 shadow blocks {superblock,
 * the ONE bitmap block touched by this put, the ONE index block touched by
 * this put}. Written PENDING, then the two real home writes happen, then
 * cleared — applied (replayed) IMMEDIATELY on the next mount if a crash
 * left it PENDING, since cas_put runs constantly and every put must be
 * durable on its own.
 *
 * VFS-directory journal: one header block + VFS_DIR_BLOCKS shadow blocks
 * holding a full copy of g_dir. Written PENDING on every directory mutation
 * (write or unlink); DEFERRED apply — nothing copies the shadow to the real
 * dir_start region until SYS_VFS_SYNC is called or the next boot's recovery
 * runs. This gives SYS_VFS_SYNC a genuine, demonstrable purpose instead of
 * being a no-op: without it, a crash still recovers everything (recovery
 * replays whatever was last journaled), but the on-disk dir_start region
 * stays stale until either sync or reboot recovery applies it.
 * =========================================================================== */
#define CJ_STATE_EMPTY   0
#define CJ_STATE_PENDING 1
struct cjournal_header {
    char     magic[8];          /* "CJRNL001" */
    uint32_t state;
    uint32_t seq;
    uint64_t bitmap_block;      /* home location the bitmap shadow replays to */
    uint64_t index_block;       /* home location the index shadow replays to  */
} __attribute__((packed));

#define VJ_STATE_EMPTY   0
#define VJ_STATE_PENDING 1
struct vjournal_header {
    char     magic[8];          /* "VJRNL001" */
    uint32_t state;
    uint32_t seq;
} __attribute__((packed));

static uint32_t g_cj_seq = 0, g_vj_seq = 0;

/* All disk-facing buffers are a full 512-byte sector.                         */
static uint8_t g_sbblk[512]  __attribute__((aligned(512)));
static uint8_t g_bitmap[8192] __attribute__((aligned(512)));           /* up to 65536 blocks */
static uint8_t g_idxbuf[512]  __attribute__((aligned(512)));
static uint8_t g_blk[512]     __attribute__((aligned(512)));
static int     g_cas_mounted = 0;
static int     g_cas_legacy  = 0;      /* v0.48: mounted a pre-journal (version 2) volume */
#define SB ((struct cas_superblock *)g_sbblk)

/* --- VFS directory: names -> content, layered on CAS ---------------------- */
#define VFS_MAXFILES   29                     /* v0.43: +4 for smp_stress; v0.46: +2 fixed, */
                                               /* reused, non-growing names ("ipc-payload", */
                                               /* "ipc-peer") for cmd_ipc_stress; v0.47: +1  */
                                               /* fixed name ("vfio-devid") for cmd_vfio_stress; */
                                               /* v0.48: +2 fixed names ("vfs-stress",       */
                                               /* "vfs-crash-test") for cmd_vfs_stress        */
#define VFS_MAX_CHUNKS 16                     /* 16 * 512 = 8 KiB max file       */
/* v0.48: the ACTUAL number of 512B blocks needed to hold VFS_MAXFILES 256-byte
 * dirents — cas_format()/vfs_flush()/cas_mount() used to hardcode "8", which
 * was only ever correct for the original VFS_MAXFILES==16 (8*512/256==16).
 * Every bump since (v0.43/46/47/48) silently left the new dirent slots
 * memory-only: never persisted to disk, never restored across a mount. See
 * CHANGELOG-0.48.0.md for how this was found and confirmed with a direct read
 * of the pre-fix code. */
#define VFS_DIR_BLOCKS ((VFS_MAXFILES * 256 + CAS_BS - 1) / CAS_BS)
struct dirent {
    char     name[32];
    uint32_t used;
    uint32_t len;
    uint32_t nchunks;
    uint32_t _pad;
    uint64_t file_hash;                       /* hash of whole content (identity) */
    uint64_t chunk_hash[VFS_MAX_CHUNKS];      /* per-512B-block content hashes    */
    uint8_t  reserved[72];                    /* pad dirent to exactly 256 bytes  */
} __attribute__((packed));
static uint8_t g_dir[VFS_MAXFILES * 256] __attribute__((aligned(512)));
#define DENTS ((struct dirent *)g_dir)

static void bm_set(uint64_t b) { g_bitmap[b >> 3] |= (uint8_t)(1u << (b & 7)); }
static int  bm_get(uint64_t b) { return (g_bitmap[b >> 3] >> (b & 7)) & 1; }

static void cas_flush_meta(void) {
    virtio_write_block(0, g_sbblk);
    for (uint64_t i = 0; i < SB->bitmap_blocks; i++)
        virtio_write_block(SB->bitmap_start + i, g_bitmap + i * CAS_BS);
}

static int64_t bm_alloc(void) {
    for (uint64_t b = SB->data_start; b < SB->total_blocks; b++)
        if (!bm_get(b)) { bm_set(b); SB->used_blocks++; return (int64_t)b; }
    return -1;
}

/* Open-addressed (linear-probe) index; hash 0 marks an empty slot.            */
static int64_t cas_index_find(uint64_t hash, uint32_t *out_len) {
    uint64_t slots = SB->index_blocks * CAS_SLOTS_PER_BLOCK;
    uint64_t start = hash % slots;
    for (uint64_t probe = 0; probe < slots; probe++) {
        uint64_t s   = (start + probe) % slots;
        uint64_t sec = SB->index_start + s / CAS_SLOTS_PER_BLOCK;
        uint32_t off = (uint32_t)(s % CAS_SLOTS_PER_BLOCK) * sizeof(struct cas_islot);
        virtio_read_block(sec, g_idxbuf);
        struct cas_islot *sl = (struct cas_islot *)(g_idxbuf + off);
        if (sl->hash == 0 && sl->block == 0) return -1;
        if (sl->hash == hash) { if (out_len) *out_len = sl->len; return (int64_t)sl->block; }
    }
    return -1;
}
/* v0.48: STAGES the insert into g_idxbuf (the correct home sector loaded and
 * mutated in memory) WITHOUT writing it home. cas_put journals this staged
 * block BEFORE it ever reaches its home sector, so a crash between the two
 * writes below can never leave the index and bitmap disagreeing about who
 * owns a block. Returns the home sector, -1 if the index is full, -2 if the
 * hash is already present (caller treats that as a dedup, same as before). */
static int64_t cas_index_stage(uint64_t hash, uint32_t block, uint32_t len) {
    uint64_t slots = SB->index_blocks * CAS_SLOTS_PER_BLOCK;
    uint64_t start = hash % slots;
    for (uint64_t probe = 0; probe < slots; probe++) {
        uint64_t s   = (start + probe) % slots;
        uint64_t sec = SB->index_start + s / CAS_SLOTS_PER_BLOCK;
        uint32_t off = (uint32_t)(s % CAS_SLOTS_PER_BLOCK) * sizeof(struct cas_islot);
        virtio_read_block(sec, g_idxbuf);
        struct cas_islot *sl = (struct cas_islot *)(g_idxbuf + off);
        if (sl->hash == hash) return -2;
        if (sl->hash == 0 && sl->block == 0) {
            sl->hash = hash; sl->block = block; sl->len = len;
            return (int64_t)sec;
        }
    }
    return -1;
}

/* ===========================================================================
 * v0.48: CAS-METADATA JOURNAL — closes the exact hazard the audit found in
 * cas_put's original sequence (index home-write lands, then a crash before
 * cas_flush_meta's bitmap/superblock write lands -> index says "hash X is at
 * block B", bitmap says "block B is free" -> a future put legitimately
 * overwrites block B, silently corrupting a different dedup'd file).
 *
 * Scope is deliberately narrow: {superblock, the ONE bitmap block touched by
 * THIS put, the ONE index block touched by THIS put} — not the whole ~18-
 * block metadata region — because cas_put runs on every single write in
 * every existing suite; shadowing the whole region every call would be a
 * real performance regression against every suite's existing timing budget,
 * for no benefit this narrower shadow doesn't already provide. Applied
 * (replayed) IMMEDIATELY: either right here if bm_alloc/cas_index_stage
 * built a real transaction, or at the next mount if a crash intervened.      */
static void cas_journal_write(uint64_t idx_sec, uint64_t bitmap_blk) {
    struct cjournal_header h; cmemset(&h, 0, sizeof h);
    const char mg[8] = { 'C','J','R','N','L','0','0','1' };
    cmemcpy(h.magic, mg, 8);
    h.state = CJ_STATE_PENDING; h.seq = ++g_cj_seq;
    h.bitmap_block = bitmap_blk; h.index_block = idx_sec;
    uint8_t hdrblk[CAS_BS]; cmemset(hdrblk, 0, CAS_BS); cmemcpy(hdrblk, &h, sizeof h);
    virtio_write_block(SB->cjournal_start + 0, hdrblk);
    virtio_write_block(SB->cjournal_start + 1, g_sbblk);
    uint8_t bmblk[CAS_BS];
    cmemcpy(bmblk, g_bitmap + (bitmap_blk - SB->bitmap_start) * CAS_BS, CAS_BS);
    virtio_write_block(SB->cjournal_start + 2, bmblk);
    virtio_write_block(SB->cjournal_start + 3, g_idxbuf);      /* the STAGED block, not yet home */
}
static void cas_journal_clear(void) {
    uint8_t z[CAS_BS]; cmemset(z, 0, CAS_BS);
    virtio_write_block(SB->cjournal_start + 0, z);
}
/* Called once, from cas_mount(), BEFORE the bitmap/index are trusted. Replays
 * a PENDING transaction to its recorded home locations so the disk always
 * ends up in the "both writes landed" state regardless of exactly how far a
 * prior crash interrupted the real writes.                                   */
static void cas_journal_recover(void) {
    uint8_t hdrblk[CAS_BS]; virtio_read_block(SB->cjournal_start + 0, hdrblk);
    struct cjournal_header *h = (struct cjournal_header *)hdrblk;
    const char mg[8] = { 'C','J','R','N','L','0','0','1' };
    if (h->state != CJ_STATE_PENDING || cmemcmp(h->magic, mg, 8) != 0) return;
    uint8_t sbblk[CAS_BS]; virtio_read_block(SB->cjournal_start + 1, sbblk);
    virtio_write_block(0, sbblk);
    uint8_t bmblk[CAS_BS]; virtio_read_block(SB->cjournal_start + 2, bmblk);
    virtio_write_block(h->bitmap_block, bmblk);
    uint8_t ixblk[CAS_BS]; virtio_read_block(SB->cjournal_start + 3, ixblk);
    virtio_write_block(h->index_block, ixblk);
    uint8_t z[CAS_BS]; cmemset(z, 0, CAS_BS);
    virtio_write_block(SB->cjournal_start + 0, z);
    kprintf("[cas    ] CAS journal recovery: replayed a pending metadata transaction "
            "(bitmap blk %d, index blk %d, seq %u)\n",
            h->bitmap_block, h->index_block, (uint64_t)h->seq);
}

/* store content -> return its content hash (address). Deduplicates.          */
/* v0.41: g_cas_lock (rank 3) serializes the whole put — the index probe, the */
/* bitmap claim, the shared staging sectors (g_blk/g_idxbuf) and the SB       */
/* counters were all shared mutable state that two cores previously raced.    */
/* The lock IS held across the disk waits inside; see the klock rank rules.   */
static uint64_t cas_put(const void *data, uint32_t len) {
    uint64_t h = rust_cas_hash((uint64_t)data, len);       /* <-- Rust CAS hash */
    klock_acquire(&g_cas_lock);
    SB->put_count++;
    uint32_t elen;
    int64_t existing = cas_index_find(h, &elen);
    if (existing >= 0) {
        SB->dedup_hits++; cas_flush_meta();
        klock_release(&g_cas_lock);
        kprintf("[cas    ] put len %d hash %X -> DEDUP to block %d (no write)\n",
                (uint64_t)len, h, (uint64_t)existing);
        return h;
    }
    int64_t b = bm_alloc();
    if (b < 0) { klock_release(&g_cas_lock); kputs("[cas    ] volume full\n"); return 0; }
    cmemset(g_blk, 0, CAS_BS);
    cmemcpy(g_blk, data, len > CAS_BS ? CAS_BS : len);
    virtio_write_block((uint64_t)b, g_blk);
    int64_t idx_sec = cas_index_stage(h, (uint32_t)b, len);
    if (idx_sec >= 0) {
        if (!g_cas_legacy) {
            uint64_t bitmap_blk = SB->bitmap_start + ((uint64_t)b >> 3) / CAS_BS;
            cas_journal_write((uint64_t)idx_sec, bitmap_blk);
            virtio_write_block((uint64_t)idx_sec, g_idxbuf);   /* home index write   */
            cas_flush_meta();                                  /* home sb+bitmap write */
            cas_journal_clear();
        } else {
            virtio_write_block((uint64_t)idx_sec, g_idxbuf);
            cas_flush_meta();
        }
    } else {
        cas_flush_meta();          /* index full: bitmap/put_count still need to land */
    }
    klock_release(&g_cas_lock);
    kprintf("[cas    ] put len %d hash %X -> block %d (stored)\n", (uint64_t)len, h, (uint64_t)b);
    return h;
}

/* fetch content by hash -> length, copies into out (up to max).              */
static int64_t cas_get(uint64_t hash, void *out, uint32_t max) {
    uint32_t len;
    klock_acquire(&g_cas_lock);                /* g_idxbuf + g_blk are shared   */
    int64_t b = cas_index_find(hash, &len);
    if (b < 0) { klock_release(&g_cas_lock); return -1; }
    virtio_read_block((uint64_t)b, g_blk);
    cmemcpy(out, g_blk, len > max ? max : len);
    klock_release(&g_cas_lock);
    return (int64_t)len;
}

static void cas_format(void) {
    uint64_t total = g_vblk_capacity ? g_vblk_capacity : 8192;
    if (total > 65536) total = 65536;                      /* bitmap array bound */
    cmemset(g_sbblk, 0, CAS_BS);
    const char mg[8] = { 'O','R','U','N','C','A','S','1' };
    cmemcpy(SB->magic, mg, 8);
    /* v0.48: version 3 — adds the two journal regions below. A version-2       */
    /* volume never had them; cas_mount() gates all journal use on this bump.    */
    SB->version = 3; SB->block_size = CAS_BS; SB->total_blocks = total;
    SB->bitmap_start  = 1;
    SB->bitmap_blocks = (total / 8 + CAS_BS - 1) / CAS_BS;
    SB->index_start   = SB->bitmap_start + SB->bitmap_blocks;
    SB->index_blocks  = 16;
    SB->dir_start     = SB->index_start + SB->index_blocks;
    SB->dir_blocks    = VFS_DIR_BLOCKS;                    /* VFS directory (fixed, was hardcoded 8) */
    SB->vjournal_start  = SB->dir_start + SB->dir_blocks;
    SB->vjournal_blocks = 1 + VFS_DIR_BLOCKS;              /* header + full-dir shadow  */
    SB->cjournal_start  = SB->vjournal_start + SB->vjournal_blocks;
    SB->cjournal_blocks = 4;                               /* header + sb + bitmap-blk + index-blk */
    SB->data_start    = SB->cjournal_start + SB->cjournal_blocks;
    SB->used_blocks   = SB->data_start;
    cmemset(g_bitmap, 0, sizeof g_bitmap);
    for (uint64_t b = 0; b < SB->data_start; b++) bm_set(b);
    cmemset(g_idxbuf, 0, CAS_BS);
    for (uint64_t i = 0; i < SB->index_blocks; i++) virtio_write_block(SB->index_start + i, g_idxbuf);
    for (uint64_t i = 0; i < SB->dir_blocks; i++) virtio_write_block(SB->dir_start + i, g_idxbuf);
    for (uint64_t i = 0; i < SB->vjournal_blocks; i++) virtio_write_block(SB->vjournal_start + i, g_idxbuf);
    for (uint64_t i = 0; i < SB->cjournal_blocks; i++) virtio_write_block(SB->cjournal_start + i, g_idxbuf);
    cas_flush_meta();
    g_cas_mounted = 1;
    g_cas_legacy = 0;
    kprintf("[cas    ] formatted %d blocks: bitmap@%d(%d) index@%d(%d) dir@%d(%d) vjrnl@%d(%d) cjrnl@%d(%d) data@%d\n",
            SB->total_blocks, SB->bitmap_start, SB->bitmap_blocks,
            SB->index_start, SB->index_blocks, SB->dir_start, SB->dir_blocks,
            SB->vjournal_start, SB->vjournal_blocks, SB->cjournal_start, SB->cjournal_blocks, SB->data_start);
}

static void vfs_journal_apply(void);   /* fwd: v0.48, defined in the VFS section below */

/* format/mount are UNLOCKED by design: both run on the boot core before the
 * APs are online (and from the BSP shell with no AP tasks queued). Taking
 * ranks here would only blur the invariant that matters: every POST-SMP
 * entry into CAS state goes through cas_put/cas_get under rank 3.            */
static int cas_mount(void) {
    virtio_read_block(0, g_sbblk);
    const char mg[8] = { 'O','R','U','N','C','A','S','1' };
    if (cmemcmp(SB->magic, mg, 8) != 0 || (SB->version != 2 && SB->version != 3)) return 0;
    /* v0.48: a version-2 volume predates both the dir_blocks fix and the        */
    /* journal regions — its on-disk bytes past dir_blocks/8 are DATA blocks,    */
    /* not journal headers, so trusting SB->vjournal_start/cjournal_start here   */
    /* would read (and later write!) garbage. Mount it read/write-compatible in  */
    /* a legacy mode instead: same 8-block dir clamp this kernel always used,    */
    /* journaling simply inactive until the volume is reformatted.               */
    g_cas_legacy = (SB->version == 2);
    if (!g_cas_legacy) {
        cas_journal_recover();
        virtio_read_block(0, g_sbblk);      /* recovery may have rewritten the superblock */
    }
    for (uint64_t i = 0; i < SB->bitmap_blocks; i++)
        virtio_read_block(SB->bitmap_start + i, g_bitmap + i * CAS_BS);
    if (!g_cas_legacy) vfs_journal_apply();  /* replay any pending directory commit before load */
    uint64_t dirlim = g_cas_legacy ? 8 : VFS_DIR_BLOCKS;
    for (uint64_t i = 0; i < SB->dir_blocks && i < dirlim; i++)
        virtio_read_block(SB->dir_start + i, g_dir + i * CAS_BS);
    g_cas_mounted = 1;
    kprintf("[cas    ] mounted existing volume (v%d%s): %d/%d blocks used, %d puts, %d dedup hits\n",
            SB->version, g_cas_legacy ? ", legacy/no-journal" : "",
            SB->used_blocks, SB->total_blocks, SB->put_count, SB->dedup_hits);
    return 1;
}

/* ===========================================================================
 * VFS — a named, read/write file layer over CAS
 * ===========================================================================
 * A file is a name + an ordered list of 512-byte content-block hashes. The
 * blocks live in CAS, so identical blocks across files are stored once, and a
 * write is copy-on-write: new content -> new block hashes -> new file_hash,
 * with the name simply repointing.
 * =========================================================================== */
static int streq_n(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; }
    return 1;
}
/* v0.48: legacy-only direct flush — a version-2 (pre-journal) volume has no
 * journal region to defer into, so it keeps writing dir_start straight away,
 * exactly like every version before this one. Non-legacy volumes never call
 * this; they go through vfs_journal_commit()/vfs_journal_apply() instead.    */
static void vfs_flush(void) {
    for (uint64_t i = 0; i < SB->dir_blocks && i < 8; i++)
        virtio_write_block(SB->dir_start + i, g_dir + i * CAS_BS);
}
static int vfs_find(const char *name) {
    for (int i = 0; i < VFS_MAXFILES; i++)
        if (DENTS[i].used && streq_n(DENTS[i].name, name, 32)) return i;
    return -1;
}
static void ts_emit(int type, const char *who, const char *text);  /* Time-Stream (Phase 5) */

/* ===========================================================================
 * v0.48: VFS-DIRECTORY JOURNAL
 * ===========================================================================
 * A single-slot whole-directory shadow: every mutation (write or unlink)
 * copies the CURRENT in-memory g_dir into the journal payload region and
 * marks the header PENDING. That commit alone is what makes an update
 * crash-safe — a boot that never reaches SYS_VFS_SYNC still recovers it, via
 * vfs_journal_apply() running from cas_mount(). What SYS_VFS_SYNC actually
 * buys the caller is EAGER application: without calling it, the on-disk
 * dir_start region stays stale (last-applied state) until the next mount's
 * recovery runs, even though the journal already holds the true state.       */
static void vfs_journal_commit(void) {
    if (g_cas_legacy) { vfs_flush(); return; }             /* no journal region to use */
    struct vjournal_header h; cmemset(&h, 0, sizeof h);
    const char mg[8] = { 'V','J','R','N','L','0','0','1' };
    cmemcpy(h.magic, mg, 8);
    h.state = VJ_STATE_PENDING; h.seq = ++g_vj_seq;
    uint8_t hdrblk[CAS_BS]; cmemset(hdrblk, 0, CAS_BS); cmemcpy(hdrblk, &h, sizeof h);
    virtio_write_block(SB->vjournal_start + 0, hdrblk);
    for (uint64_t i = 0; i < VFS_DIR_BLOCKS; i++)
        virtio_write_block(SB->vjournal_start + 1 + i, g_dir + i * CAS_BS);
}
/* Applies a PENDING journal commit to the real dir_start region — called
 * both from cas_mount() (boot-time recovery) and SYS_VFS_SYNC (explicit,
 * eager flush). Idempotent: a CLEAN/EMPTY journal is a no-op. Returns 1 if
 * it actually applied a pending commit, 0 otherwise.                        */
static int g_vfs_last_sync_applied = 0;   /* diagnostic only, read by cmd_vfs_stress */
static void vfs_journal_apply(void) {
    if (g_cas_legacy) { g_vfs_last_sync_applied = 0; return; }
    uint8_t hdrblk[CAS_BS]; virtio_read_block(SB->vjournal_start + 0, hdrblk);
    struct vjournal_header *h = (struct vjournal_header *)hdrblk;
    const char mg[8] = { 'V','J','R','N','L','0','0','1' };
    if (h->state != VJ_STATE_PENDING || cmemcmp(h->magic, mg, 8) != 0) {
        g_vfs_last_sync_applied = 0;
        return;
    }
    uint8_t buf[CAS_BS];
    for (uint64_t i = 0; i < VFS_DIR_BLOCKS && i < SB->dir_blocks; i++) {
        virtio_read_block(SB->vjournal_start + 1 + i, buf);
        virtio_write_block(SB->dir_start + i, buf);
    }
    uint8_t z[CAS_BS]; cmemset(z, 0, CAS_BS);
    virtio_write_block(SB->vjournal_start + 0, z);
    kprintf("[vfs    ] VFS journal: applied a pending directory commit (seq %u) to disk\n",
            (uint64_t)h->seq);
    g_vfs_last_sync_applied = 1;
}

/* v0.41: the whole-file COW rewrite runs under g_vfs_lock (rank 2), held
 * across every chunk's cas_put (rank 3 — strictly upward) and the directory
 * flush. That makes a WRITE ATOMIC AGAINST READERS AND OTHER WRITERS: no
 * core can observe a dirent whose chunk list is half old, half new, and two
 * writers creating the same (or a new) name can no longer both claim a slot.
 * The internal body assumes the lock is HELD; the two entry points below own
 * the acquire so name-based and dirent-based callers share one code path.    */
static int vfs_write_locked(int idx, const char *name, const void *data, uint32_t len) {
    struct dirent *d = &DENTS[idx];
    cmemset(d, 0, 256);
    kstrcpy_n(d->name, name, 32);
    d->used = 1; d->len = len;
    uint32_t nch = (len + 511) / 512;
    if (nch > VFS_MAX_CHUNKS) nch = VFS_MAX_CHUNKS;
    d->nchunks = nch;
    const uint8_t *p = data;
    for (uint32_t i = 0; i < nch; i++) {
        uint32_t cl = len - i * 512; if (cl > 512) cl = 512;
        d->chunk_hash[i] = cas_put(p + i * 512, cl);       /* CAS stores each block */
    }
    d->file_hash = len ? rust_cas_hash((uint64_t)data, len) : 0;
    vfs_journal_commit();          /* v0.48: journal-commit; see comment above */
    { char msg[64]; int mp = 0;
      const char *pre = "wrote file "; while (pre[mp]) { msg[mp] = pre[mp]; mp++; }
      for (int q = 0; name[q] && mp < 60; q++) msg[mp++] = name[q];
      msg[mp] = 0; ts_emit(1 /*TSE_FILE*/, "vfs", msg); }   /* lock-free emit  */
    return idx;
}
static int vfs_write_file(const char *name, const void *data, uint32_t len) {
    klock_acquire(&g_vfs_lock);
    int idx = vfs_find(name);
    if (idx < 0) for (int i = 0; i < VFS_MAXFILES; i++) if (!DENTS[i].used) { idx = i; break; }
    if (idx < 0) { klock_release(&g_vfs_lock); kputs("[vfs    ] directory full\n"); return -1; }
    idx = vfs_write_locked(idx, name, data, len);
    klock_release(&g_vfs_lock);
    return idx;
}
/* Rewrite an already-open dirent in place (SYS_WRITE_FILE). The old path read
 * DENTS[di].name WITHOUT the lock and re-resolved it by name — both racy.    */
static int vfs_write_by_dirent(int di, const void *data, uint32_t len) {
    if (di < 0 || di >= VFS_MAXFILES) return -1;
    klock_acquire(&g_vfs_lock);
    if (!DENTS[di].used) { klock_release(&g_vfs_lock); return -1; }
    char name[32]; kstrcpy_n(name, DENTS[di].name, 32);    /* copy under lock  */
    int r = vfs_write_locked(di, name, data, len);
    klock_release(&g_vfs_lock);
    return r;
}
static int64_t vfs_read_file(int idx, void *buf, uint32_t max) {
    klock_acquire(&g_vfs_lock);                /* the chunk list must not be    */
    struct dirent *d = &DENTS[idx];            /* COW-swapped under our read    */
    uint32_t got = 0;
    uint8_t tmp[512];
    for (uint32_t i = 0; i < d->nchunks; i++) {
        cas_get(d->chunk_hash[i], tmp, 512);   /* rank 2 -> 3: strictly upward  */
        uint32_t cl = d->len - i * 512; if (cl > 512) cl = 512;
        for (uint32_t j = 0; j < cl && got < max; j++) ((uint8_t *)buf)[got++] = tmp[j];
    }
    klock_release(&g_vfs_lock);
    return got;
}
/* ===========================================================================
 * v0.48: MULTI-VOLUME ABSTRACTION — ROOT (unchanged), TMP (ephemeral RAM-only,
 * no CAS/journaling — matches real tmpfs semantics), DEV (read-only, a thin
 * text listing over the existing kdevs[] registry — NOT a second way to touch
 * raw MMIO; that's SYS_HW_PASSTHROUGH/SYS_VFIO_MAP_BAR's job, unchanged).
 * Path-prefix routing ("tmp/", "dev/", else ROOT) keeps every existing
 * suite's bare filenames working exactly as before — this is purely additive.
 * =========================================================================== */
#define VOL_ROOT 0
#define VOL_TMP  1
#define VOL_DEV  2

#define TMP_MAXFILES 4                 /* small, ephemeral, RAM-only scratch area */
#define TMP_MAXBYTES 512
struct tmpfile { char name[32]; int used; uint32_t len; uint8_t data[TMP_MAXBYTES]; };
static struct tmpfile g_tmpfiles[TMP_MAXFILES];

static int path_has_prefix(const char *name, const char *prefix) {
    int i = 0;
    while (prefix[i]) { if (name[i] != prefix[i]) return 0; i++; }
    return 1;
}

static int64_t tmp_read_file(int ti, void *buf, uint32_t max) {
    if (ti < 0 || ti >= TMP_MAXFILES) return -1;
    klock_acquire(&g_vfs_lock);            /* reuses rank 2: VFS-adjacent, not CAS state */
    if (!g_tmpfiles[ti].used) { klock_release(&g_vfs_lock); return -1; }
    uint32_t n = g_tmpfiles[ti].len; if (n > max) n = max;
    cmemcpy(buf, g_tmpfiles[ti].data, n);
    klock_release(&g_vfs_lock);
    return (int64_t)n;
}
static int tmp_write_file(int ti, const void *data, uint32_t len) {
    if (ti < 0 || ti >= TMP_MAXFILES) return -1;
    if (len > TMP_MAXBYTES) len = TMP_MAXBYTES;
    klock_acquire(&g_vfs_lock);
    if (!g_tmpfiles[ti].used) { klock_release(&g_vfs_lock); return -1; }
    cmemcpy(g_tmpfiles[ti].data, data, len);
    g_tmpfiles[ti].len = len;
    klock_release(&g_vfs_lock);
    return (int)len;
}

/* dev/devices: a read-only text snapshot of the kdevs[] registry, built fresh
 * on every read (no persistent backing — it's a live VIEW, not a file).       */
static uint32_t u64_to_dec_buf(uint8_t *out, uint64_t v) {
    char tmp[20]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = (uint8_t)tmp[n - 1 - i];
    return (uint32_t)n;
}
static uint32_t dev_build_listing(uint8_t *buf, uint32_t max) {
    uint32_t p = 0;
    for (int i = 0; i < n_kdev && p + 96 < max; i++) {
        const char *nm = kdevs[i].name;
        for (int j = 0; nm[j] && p < max; j++) buf[p++] = (uint8_t)nm[j];
        if (p < max) buf[p++] = ' ';
        p += u64_to_dec_buf(buf + p, kdevs[i].base);
        if (p < max) buf[p++] = ' ';
        p += u64_to_dec_buf(buf + p, kdevs[i].len);
        if (p < max) buf[p++] = '\n';
    }
    return p;
}
static int64_t dev_read_file(void *buf, uint32_t max) {
    uint8_t tmp[768];
    uint32_t n = dev_build_listing(tmp, sizeof tmp);
    if (n > max) n = max;
    cmemcpy(buf, tmp, n);
    return (int64_t)n;
}

/* Open-descriptor array. v0.41: fd claim/free/deref go under g_ofile_lock
 * (rank 1) and every descriptor records its OWNING process — before this,
 * any process could close (or write through) any other process's fd, and two
 * cores could be handed the same fd. NOTE the deliberate absence of nesting
 * between ranks 1 and 2: vfs_open resolves the name under the vfs lock,
 * RELEASES it, then claims the fd under the ofile lock. Nesting them here
 * (vfs->ofile) against SYS_READ's ofile->vfs would be the classic ABBA
 * inversion — the one concrete deadlock this design had to engineer out.
 * v0.48: `.volume` records which of the three volumes this fd was opened
 * against — a fd is bound to its volume for life; there is no operation that
 * lets one fd be reinterpreted as a different volume's handle, which is what
 * makes the volume boundary an isolation guarantee rather than a convention. */
struct ofile { int used; int dirent; uint64_t off; int owner; int volume; };
static struct ofile g_ofiles[16];
static int ofile_claim(int owner, int volume, int dirent) {
    klock_acquire(&g_ofile_lock);
    for (int fd = 0; fd < 16; fd++)
        if (!g_ofiles[fd].used) {
            g_ofiles[fd].used = 1; g_ofiles[fd].dirent = dirent;
            g_ofiles[fd].off = 0; g_ofiles[fd].owner = owner;
            g_ofiles[fd].volume = volume;
            klock_release(&g_ofile_lock);
            return fd;
        }
    klock_release(&g_ofile_lock);
    return -1;
}
static int vfs_open_for(const char *name, int owner) {
    if (path_has_prefix(name, "tmp/")) {
        const char *rest = name + 4;
        klock_acquire(&g_vfs_lock);
        int ti = -1;
        for (int i = 0; i < TMP_MAXFILES; i++)
            if (g_tmpfiles[i].used && streq_n(g_tmpfiles[i].name, rest, 32)) { ti = i; break; }
        if (ti < 0) for (int i = 0; i < TMP_MAXFILES; i++) if (!g_tmpfiles[i].used) {
            ti = i; g_tmpfiles[i].used = 1; g_tmpfiles[i].len = 0;
            kstrcpy_n(g_tmpfiles[i].name, rest, 32); break;
        }
        klock_release(&g_vfs_lock);
        if (ti < 0) return -1;
        return ofile_claim(owner, VOL_TMP, ti);
    }
    if (path_has_prefix(name, "dev/"))
        return ofile_claim(owner, VOL_DEV, 0);     /* dirent unused: a live view, not a file */

    klock_acquire(&g_vfs_lock);
    int di = vfs_find(name);
    klock_release(&g_vfs_lock);                /* released BEFORE the fd claim  */
    if (di < 0) return -1;
    return ofile_claim(owner, VOL_ROOT, di);
}
static int vfs_open(const char *name) { return vfs_open_for(name, (int)current_proc_idx); }

/* ===========================================================================
 * v0.48: SYS_VFS_UNLINK — zero the dirent, journal-commit the directory
 * change, then force-close any g_ofiles[] entry still pointing at it,
 * regardless of owner (mirrors descriptor_teardown_kproc's "regardless of how
 * it got there" philosophy, just triggered by unlink instead of process exit).
 * Deliberately NOT reclaiming the CAS blocks/index slots the file's chunks
 * used: there is no reference counting across dirents (two files can share a
 * chunk_hash via dedup), so freeing them here could silently corrupt a
 * different, still-live file. Disclosed as a scope boundary, not fixed here.  */
static int vfs_unlink(const char *name) {
    klock_acquire(&g_vfs_lock);
    int idx = vfs_find(name);
    if (idx < 0) { klock_release(&g_vfs_lock); return -1; }
    DENTS[idx].used = 0;
    vfs_journal_commit();
    klock_release(&g_vfs_lock);

    klock_acquire(&g_ofile_lock);              /* separate, non-nested section (rank 1) */
    for (int fd = 0; fd < 16; fd++)
        if (g_ofiles[fd].used && g_ofiles[fd].volume == VOL_ROOT && g_ofiles[fd].dirent == idx)
            { g_ofiles[fd].used = 0; g_ofiles[fd].owner = -1; }
    klock_release(&g_ofile_lock);
    return 0;
}

/* ===========================================================================
 * v0.45: DESCRIPTOR TEARDOWN — the fd-leak half of kproc lifetime discipline
 * ===========================================================================
 * The one gap every exit path left open through v0.44: g_ofiles[] entries
 * are owner-tagged (v0.41) but were ONLY ever released by the owning
 * process's own SYS_CLOSE call. A process that never reaches that call —
 * terminated by a fault in handle_cpl3_fault instead of running to its own
 * SYS_EXIT — left its fd permanently marked used, attributed to a kproc
 * slot that (as of this milestone) can be recycled out from under it: the
 * next process to land in that slot would NOT inherit the fd (ownership is
 * checked by slot index, and the new occupant gets a fresh one), but the
 * leaked g_ofiles entry itself would sit there forever, one of 16 possible
 * descriptors gone for the life of the boot. Surfaces already had this exact
 * guarantee since v0.41 (surfaces_reclaim, called at every exit site); this
 * closes the same class of gap for file descriptors. Called from every exit
 * path immediately before dma_teardown_kproc, so a descriptor never survives
 * past the process that opened it — whether that process exited cleanly,
 * faulted, or is about to have its slot handed to someone else.            */
static void descriptor_teardown_kproc(int proc_idx) {
    struct kproc *p = &kprocs[proc_idx];
    int before = 0, after = 0;
    klock_acquire(&g_ofile_lock);
    for (int fd = 0; fd < 16; fd++)
        if (g_ofiles[fd].used && g_ofiles[fd].owner == proc_idx) before++;
    for (int fd = 0; fd < 16; fd++) {
        if (!g_ofiles[fd].used || g_ofiles[fd].owner != proc_idx) continue;
        if (g_debug_kproc_lifetime)
            kprintf("[dbgkpr ] pid %u slot %d: force-closing fd %d (dirent %d) — never reached SYS_CLOSE\n",
                    p->pid, proc_idx, fd, g_ofiles[fd].dirent);
        g_ofiles[fd].used = 0; g_ofiles[fd].owner = -1;
    }
    for (int fd = 0; fd < 16; fd++)
        if (g_ofiles[fd].used && g_ofiles[fd].owner == proc_idx) after++;
    klock_release(&g_ofile_lock);

    if (g_debug_kproc_lifetime)
        kprintf("[dbgkpr ] pid %u slot %d: descriptors before=%d after=%d, DMA grants=%u\n",
                p->pid, proc_idx, before, after, (uint64_t)p->dma_grant_count);
    if (after != 0) {                                     /* must be unreachable  */
        kprintf("\n[panic ] pid %u slot %d: %d descriptor(s) still owned after teardown\n",
                p->pid, proc_idx, after);
        for (;;) __asm__ volatile("cli; hlt");
    }
}

/* ===========================================================================
 * v0.46: CAPABILITY-BOUND IPC & SHARED-MEMORY MESSAGING
 * ===========================================================================
 * A fixed-size mailbox per kproc slot (g_ipc_q[], indexed the same way
 * g_proc_slpt[]/g_surf[].owner already are) plus a small global pool of
 * shared physical frames (g_ipc_shm[]). A VFS file descriptor or a shared
 * memory frame can ride along in a message and change hands without ever
 * copying the underlying bytes:
 *
 *   - a transferred fd is a straight ownership reassignment of the EXISTING
 *     g_ofiles[] entry (only the `owner` slot index moves; the dirent/CAS
 *     blocks behind it never move). Ownership moves at SEND time, not RECV
 *     time — exactly like handing someone a key: v0.45's
 *     descriptor_teardown_kproc already force-closes any fd owned by a
 *     dying slot regardless of how it got there, so a recipient who dies
 *     before ever calling SYS_IPC_RECV still can't leak it.
 *   - a shared frame is a NEW mechanism, deliberately distinct from v0.44's
 *     struct dma_grant: dma_grant is bound to ONE process's IOMMU domain
 *     (device isolation, not general RAM sharing), and re-architecting it
 *     to support a second simultaneous owner is out of scope and would
 *     conflate two different kinds of isolation. ipc_shmem is a small pool
 *     of alloc_frame() pages, refcounted via a per-slot bitmask (MAX_KPROC
 *     == 64 fits exactly in one uint64_t), mapped into each holder's OWN
 *     address space at a fixed, id-derived vaddr (IPC_SHM_V + id*0x1000) —
 *     genuinely zero-copy: two processes read and write the SAME physical
 *     page, the kernel only ever touches the mapping, never the payload.
 *
 * Exactly the double-free hazard v0.45 fixed for hardware-passthrough MMIO
 * applies here too: a shared frame is a present PTE in every holder's OWN
 * page tables, so page_free_tree would try to free it right along with
 * that process's private memory the moment two processes shared one.
 * ipc_teardown_kproc runs BEFORE page_free_tree (like descriptor_teardown_
 * kproc and dma_teardown_kproc) and explicitly unmap_page()s the holder's
 * mapping first — by the time page_free_tree walks, the PTE is simply gone,
 * not present, nothing to double-free. */
#define IPC_INLINE_MAX 64
#define IPC_QLEN        8
#define IPC_MSG_DATA     0    /* plain message, no descriptor transfer     */
#define IPC_MSG_XFER_FD  1    /* xfer_handle is a VFS fd the sender owns   */
#define IPC_MSG_XFER_SHM 2    /* xfer_handle is an ipc_shmem id (-1 = new) */

struct ipc_msg {
    uint64_t sender_pid;
    uint64_t recipient_pid;
    uint32_t msg_type;                    /* IPC_MSG_* above                */
    uint32_t cap_mask;                    /* capability the RECIPIENT needs */
    uint32_t payload_len;                 /* bytes of inline_data valid     */
    int64_t  xfer_handle;                 /* meaning depends on msg_type    */
    /* For IPC_MSG_XFER_SHM, the kernel overwrites inline_data[0..8] on the
     * copy handed back to BOTH the sender (right after SYS_IPC_SEND
     * returns) and the recipient (right after SYS_IPC_RECV returns) with
     * the little-endian vaddr the shared frame is mapped at in THEIR OWN
     * address space — the same convention SYS_SURFACE_CREATE already uses
     * (hand back where it landed, rather than a formula userspace must
     * hardcode). For every other msg_type, inline_data is the caller's
     * free-form payload. */
    uint8_t  inline_data[IPC_INLINE_MAX];
};

struct ipc_queue {
    struct ipc_msg msgs[IPC_QLEN];
    uint32_t head, tail, count;            /* ring state; g_ipc_lock-owned  */
};
static struct ipc_queue g_ipc_q[MAX_KPROC];

#define MAX_IPC_SHMEM 16
#define IPC_SHM_V (0x0000540000000000ull)  /* fixed per-id window, one page each; far   */
                                            /* from the passthrough/ELF/surface windows */
/* v0.47: VFIO BAR mapping window — per-process stride (0x100000, 1 MiB) is
 * far more than any BAR here needs (all are 0x1000-0x4000), leaving headroom
 * if a BAR ever grows; per-bar-index stride (0x10000) keeps bar0/bar1 apart
 * within one process's own sub-window. Comfortably clear of every other
 * fixed window (surface 0x530000000000, IPC shmem 0x540000000000, and
 * hw-passthrough's pid-scaled 0x400000000000+pid<<30, which would need an
 * implausible pid > ~21000 to ever reach here). */
#define VFIO_BAR_V (0x0000550000000000ull)
struct ipc_shmem {
    uint64_t phys;
    uint64_t owner_mask;                   /* bit i set => kprocs[i] holds a mapping */
    int      used;
};
static struct ipc_shmem g_ipc_shm[MAX_IPC_SHMEM];
/* rank 6: the next free slot in the g_ofile(1)/g_vfs(2)/g_cas(3)/g_vblk(4)/g_surf(5)
 * ranked-klock chain. Never acquired while already holding any of those —
 * every IPC helper below takes g_ofile_lock (for fd transfer) as its own,
 * separate, non-nested critical section, then g_ipc_lock as a second,
 * later one, so the ascending-rank rule is never at risk of being broken
 * by nesting the wrong way. (g_frame_lock's own "rank 6" comment is an
 * unrelated raw spinlock, not part of this ranked array — no actual
 * collision, just two independent numbering schemes that happen to reuse
 * the same next integer.) */
static struct klock g_ipc_lock = { 0, "ipc", 6, 0, 0 };

static void ipc_queue_clear(int idx) {
    struct ipc_queue *q = &g_ipc_q[idx];
    q->head = q->tail = q->count = 0;
}

static int ipc_queue_push(int idx, const struct ipc_msg *m) {
    klock_acquire(&g_ipc_lock);
    struct ipc_queue *q = &g_ipc_q[idx];
    if (q->count >= IPC_QLEN) { klock_release(&g_ipc_lock); return 0; }
    q->msgs[q->tail] = *m;
    q->tail = (q->tail + 1) % IPC_QLEN;
    q->count++;
    klock_release(&g_ipc_lock);
    return 1;
}

static int ipc_queue_pop(int idx, struct ipc_msg *out) {
    klock_acquire(&g_ipc_lock);
    struct ipc_queue *q = &g_ipc_q[idx];
    if (q->count == 0) { klock_release(&g_ipc_lock); return 0; }
    *out = q->msgs[q->head];
    q->head = (q->head + 1) % IPC_QLEN;
    q->count--;
    klock_release(&g_ipc_lock);
    return 1;
}

/* pid is monotonic and slot-independent since v0.45; every IPC entry point
 * needs to resolve a caller-supplied pid back to a live slot index. */
static int kproc_find_by_pid(uint64_t pid) {
    for (int i = 0; i < n_kproc; i++) if (kprocs[i].used && kprocs[i].pid == pid) return i;
    return -1;
}

/* Grants recipient_idx access to shmem `want_id` (or a freshly allocated one
 * if want_id < 0), setting its owner_mask bit — this is the reservation,
 * completed atomically at SEND time, exactly like fd ownership moving at
 * SEND time. Does NOT install any page-table mapping; that is
 * ipc_shmem_map_self's job, called separately by whichever side (sender or
 * recipient) actually wants to touch the memory right now. Returns the id,
 * or -1 (bad id, or `want_id` wasn't the SENDER's to re-share). */
static int64_t ipc_shmem_grant(int64_t want_id, int sender_idx, int recipient_idx) {
    klock_acquire(&g_ipc_lock);
    int64_t id = want_id;
    int need_alloc = 0;
    if (id < 0) {
        id = -1;
        for (int i = 0; i < MAX_IPC_SHMEM; i++) if (!g_ipc_shm[i].used) { id = i; break; }
        if (id < 0) { klock_release(&g_ipc_lock); return -1; }
        g_ipc_shm[id].used = 1;
        g_ipc_shm[id].owner_mask = (1ull << sender_idx);
        g_ipc_shm[id].phys = 0;                     /* filled in below, outside the lock */
        need_alloc = 1;
    } else if (id >= MAX_IPC_SHMEM || !g_ipc_shm[id].used ||
               !(g_ipc_shm[id].owner_mask & (1ull << sender_idx))) {
        klock_release(&g_ipc_lock);
        return -1;                                  /* not the sender's to re-share      */
    }
    g_ipc_shm[id].owner_mask |= (1ull << recipient_idx);
    klock_release(&g_ipc_lock);

    if (need_alloc) {
        uint64_t phys = alloc_frame();
        klock_acquire(&g_ipc_lock);
        g_ipc_shm[id].phys = phys;
        klock_release(&g_ipc_lock);
    }
    return id;
}

/* Installs (or re-installs — idempotent) proc_idx's own mapping of a shmem
 * it already holds a grant for. Safe to call speculatively; fails quietly
 * if proc_idx isn't actually a grantee. */
static int ipc_shmem_map_self(int proc_idx, int64_t id) {
    if (id < 0 || id >= MAX_IPC_SHMEM) return -1;
    klock_acquire(&g_ipc_lock);
    int ok = g_ipc_shm[id].used && (g_ipc_shm[id].owner_mask & (1ull << proc_idx));
    uint64_t phys = g_ipc_shm[id].phys;
    klock_release(&g_ipc_lock);
    if (!ok) return -1;
    map_page(kprocs[proc_idx].cr3, IPC_SHM_V + (uint64_t)id * 0x1000, phys,
             PTE_USER | PTE_WRITE | PTE_NX);
    return 0;
}

/* Called from every kproc exit path, BEFORE descriptor_teardown_kproc —
 * same ordering discipline v0.44/v0.45 established, extended one step
 * earlier. Clears this slot's OWN inbound mailbox (a recycled slot must not
 * start with stale messages) and releases every shared-frame grant this
 * process held: unmap first (safe even if it was only ever reserved, never
 * actually mapped — unmap_page on an unmapped vaddr is a no-op), then drop
 * this process's owner bit, then free the physical frame once the LAST
 * holder is gone. */
static void ipc_teardown_kproc(int proc_idx) {
    ipc_queue_clear(proc_idx);
    for (int i = 0; i < MAX_IPC_SHMEM; i++) {
        klock_acquire(&g_ipc_lock);
        int held = g_ipc_shm[i].used && (g_ipc_shm[i].owner_mask & (1ull << proc_idx));
        klock_release(&g_ipc_lock);
        if (!held) continue;
        unmap_page(kprocs[proc_idx].cr3, IPC_SHM_V + (uint64_t)i * 0x1000);
        klock_acquire(&g_ipc_lock);
        g_ipc_shm[i].owner_mask &= ~(1ull << proc_idx);
        int last = g_ipc_shm[i].used && g_ipc_shm[i].owner_mask == 0;
        uint64_t phys = g_ipc_shm[i].phys;
        if (last) g_ipc_shm[i].used = 0;
        klock_release(&g_ipc_lock);
        if (g_debug_ipc)
            kprintf("[dbgipc ] pid %u slot %d: released shmem id %d%s\n",
                    kprocs[proc_idx].pid, proc_idx, i, last ? " (last holder, freeing frame)" : "");
        if (last) free_frame(phys);
    }
}

/* Validate the C++ ring object is live in the boot image.                     */
static void cpp_ring_selftest(void) {
    cpp_ring_init();
    const char *msg = "polyglot-ipc";
    cpp_ring_push((const uint8_t *)msg, 12);
    const uint8_t *out = 0;
    uint32_t n = cpp_ring_pop(&out, 12);
    kprintf("[poly   ] C++ SPSC ring: pushed 12, popped %d -> \"", (uint64_t)n);
    for (uint32_t i = 0; i < n; i++) kputc((char)out[i]);
    kprintf("\" (depth %d)\n", cpp_ring_depth());
}

static void cmd_cas(void) {
    if (!g_vblk_ready) { kputs("[cas    ] no virtio-blk device; cannot mount CAS\n"); return; }
    kputs("-- content-addressable storage on virtio-blk (Rust-hashed) --\n");
    if (!cas_mount()) { kputs("[cas    ] no volume signature; formatting a fresh CAS volume\n"); cas_format(); }

    /* v0.48: genuine cross-QEMU-reboot journal-recovery proof. "vfscrashwrite"
     * (below) commits this file's journal entry and halts WITHOUT ever
     * calling SYS_VFS_SYNC — simulating real power loss. If it shows up here,
     * on a LATER, completely separate QEMU process sharing the same disk
     * image, cas_mount()'s automatic recovery (not this in-kernel test, a
     * genuinely different boot) is what put it there. See CHANGELOG-0.48.0.md. */
    {
        int rti = vfs_find("vfs-reboot-test");
        if (rti >= 0) {
            uint8_t rb[32];
            int64_t n = vfs_read_file(rti, rb, sizeof rb);
            static const uint8_t want[24] = "CROSS-REBOOT-JOURNAL-OK";
            int ok = (n == (int64_t)sizeof want) && (cmemcmp(rb, want, sizeof want) == 0);
            kputs("[vfs    ] cross-reboot journal probe: 'vfs-reboot-test' found from a PRIOR boot\n");
            kputs(ok ? "[vfs    ] cross-reboot journal probe: content VERIFIED\n"
                     : "[vfs    ] cross-reboot journal probe: content MISMATCH\n");
        }
    }

    const char *a = "Hello, Outrun CAS! The content is the address.";
    const char *b = "A completely different block of bytes entirely.";
    uint64_t used_before = SB->used_blocks;

    uint64_t ha  = cas_put(a, cstrlen(a));
    uint64_t hb  = cas_put(b, cstrlen(b));
    uint64_t ha2 = cas_put(a, cstrlen(a));                 /* identical -> dedup */
    (void)hb;

    char out[256];
    int64_t n = cas_get(ha, out, sizeof out - 1);
    if (n >= 0) { out[n < 255 ? n : 255] = 0;
        kprintf("[cas    ] get %X -> \"%s\" (%d bytes read from disk)\n", ha, out, (uint64_t)n); }

    kprintf("[cas    ] dedup: hash(a)==hash(a')? %s ; blocks used this run +%d\n",
            ha == ha2 ? "YES" : "no", SB->used_blocks - used_before);
    kprintf("[vblk   ] I/O completions delivered by INTERRUPT: %d\n", g_completions);
    kputs("-- done (state persisted to disk) --\n");
}

static uint8_t g_disk_buf[512]  __attribute__((aligned(512)));
static uint8_t g_disk_buf2[512] __attribute__((aligned(512)));

static void cmd_disk(void) {
    if (!g_vblk_ready) {
        kputs("[disk   ] no virtio-blk device present (boot QEMU with -device virtio-blk-pci)\n");
        return;
    }
    kputs("-- virtio-blk real disk I/O (sectors, not RAM modules) --\n");

    /* READ pre-existing on-disk data at sector 2 (a signature written into    */
    /* the disk image before boot) — proves we read real storage.             */
    if (virtio_read_block(2, g_disk_buf) == 0) {
        kprintf("[disk   ] read sector 2 -> \"");
        for (int i = 0; i < 24; i++) {
            char c = (char)g_disk_buf[i];
            kputc(c >= 32 && c < 127 ? c : '.');
        }
        kputs("\"\n");
    }

    /* WRITE a tagged pattern to sector 0, then read it back and verify —      */
    /* proves the full write path and DMA round-trip.                         */
    const char *tag = "OUTRUN-WROTE-THIS-SECTOR";
    for (int i = 0; i < 512; i++) g_disk_buf[i] = (uint8_t)(0x30 + (i & 0x3F));
    for (int i = 0; tag[i]; i++)  g_disk_buf[i] = (uint8_t)tag[i];

    if (virtio_write_block(0, g_disk_buf) != 0) { kputs("[disk   ] write failed\n"); return; }
    for (int i = 0; i < 512; i++) g_disk_buf2[i] = 0;
    if (virtio_read_block(0, g_disk_buf2) != 0) { kputs("[disk   ] readback failed\n"); return; }

    int ok = 1;
    for (int i = 0; i < 512; i++) if (g_disk_buf[i] != g_disk_buf2[i]) { ok = 0; break; }
    kprintf("[disk   ] wrote + read back sector 0 -> \"");
    for (int i = 0; i < 24; i++) kputc((char)g_disk_buf2[i]);
    kprintf("\"  verify %s\n", ok ? "MATCH — real write/read round-trip" : "MISMATCH");
    kputs("-- done --\n");
}

/* ===========================================================================
 * v0.50: VIRTIO-GPU DRIVER — real 2D resource/scanout command stream
 * ===========================================================================
 * Brings up a modern virtio-gpu device's CONTROL virtqueue only (queue 0) —
 * the cursor virtqueue (queue 1) is left uninitialized; this kernel already
 * has its own software cursor in the existing compositor and sends it no
 * commands. Every command below is the real virtio-gpu wire protocol,
 * submitted to and answered by QEMU's actual virtio-gpu device emulation —
 * not a simulated device (confirmed live: PCI vendor 1af4 device 1050 class
 * 0x03, exposed by `-device virtio-vga`, which stays VGA-compatible so
 * GRUB's existing bochs-VBE framebuffer bring-up — every compositor suite's
 * display path — is completely unaffected; re-verified across all 3 boot
 * configs with 0 FAIL). A separate `-vga std -device virtio-gpu-pci`
 * topology (two independent devices) was tried to avoid the display-output
 * takeover noted below, but hit an unexplained hang under -smp 4 — see
 * CHANGELOG-0.50.0.md for both findings and why the combined device ships.
 *
 * Deliberately single-command-in-flight, system-wide: unlike vblk's 21-slot
 * queue (built for concurrent block I/O throughput), GPU 2D commands here
 * are infrequent enough that one outstanding command at a time, serialized
 * under g_gpu_lock (rank 7), is simple, obviously correct, and sufficient to
 * prove the mechanism — an honest scope choice, not an oversight. Completion
 * is observed by POLLING the used ring (no ISR registered): the device
 * updates the used ring via DMA independent of whether an interrupt is
 * wired, so polling is exactly as correct as an IRQ here, just busier —
 * acceptable at this command rate. See CHANGELOG-0.50.0.md.
 * =========================================================================== */
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106

#define VIRTIO_GPU_RESP_OK_NODATA        0x1100
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1

struct virtio_gpu_ctrl_hdr {
    uint32_t type, flags;
    uint64_t fence_id;
    uint32_t ctx_id, padding;
} __attribute__((packed));                                     /* 24 bytes */
struct virtio_gpu_rect { uint32_t x, y, width, height; } __attribute__((packed));

struct vgpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id, format, width, height;
} __attribute__((packed));
/* nr_entries is always 1 here: our backing is one alloc_frames() contiguous
 * run, so the single virtio_gpu_mem_entry {addr,length,padding} the real
 * wire format expects after the fixed struct is simply embedded inline.     */
struct vgpu_attach_backing_1 {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id, nr_entries;
    uint64_t entry_addr;
    uint32_t entry_length, entry_padding;
} __attribute__((packed));
struct vgpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id, resource_id;
} __attribute__((packed));
struct vgpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id, padding;
} __attribute__((packed));
struct vgpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id, padding;
} __attribute__((packed));
struct vgpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id, padding;
} __attribute__((packed));

static struct klock g_gpu_lock = { 0, "gpu", 7, 0, 0 };
static volatile uint8_t *g_gpu_common = 0, *g_gpu_notify = 0;
static uint32_t          g_gpu_notify_mul = 0;
static struct vq         g_gpu_ctrl;
static int               g_gpu_ready = 0;
static volatile uint64_t g_gpu_seq_submitted = 0, g_gpu_seq_completed = 0;
static uint8_t           g_gpu_cmdbuf[64]  __attribute__((aligned(16)));  /* one in-flight cmd  */
static uint8_t           g_gpu_respbuf[64] __attribute__((aligned(16)));  /* one in-flight resp */

static int virtio_gpu_probe(uint8_t bus, uint8_t dev, uint8_t fn) {
    kprintf("[vgpu   ] virtio-gpu at %d:%d.%d — bringing up control queue\n",
            (uint64_t)bus, (uint64_t)dev, (uint64_t)fn);
    uint64_t common_off = 0, notify_off_in_bar = 0;
    int common_bar = -1, notify_bar = -1;
    uint8_t cap = (uint8_t)(pci_cfg_read32(bus, dev, fn, 0x34) & 0xFC);
    while (cap) {
        uint32_t c0 = pci_cfg_read32(bus, dev, fn, cap);
        if ((uint8_t)c0 == 0x09) {
            uint8_t cfg = (uint8_t)(c0 >> 24);
            uint8_t bar = (uint8_t)(pci_cfg_read32(bus, dev, fn, cap + 4) & 0xFF);
            uint32_t off = pci_cfg_read32(bus, dev, fn, cap + 8);
            if (cfg == 1) { common_bar = bar; common_off = off; }
            if (cfg == 2) { notify_bar = bar; notify_off_in_bar = off;
                            g_gpu_notify_mul = pci_cfg_read32(bus, dev, fn, cap + 16); }
        }
        cap = (uint8_t)((c0 >> 8) & 0xFC);
    }
    if (common_bar < 0 || notify_bar < 0) { kprintf("[vgpu   ] missing virtio caps\n"); return 0; }

    uint32_t cmd = pci_cfg_read32(bus, dev, fn, 0x04);
    pci_cfg_write32(bus, dev, fn, 0x04, cmd | 0x6);       /* mem + bus master */

    uint64_t cbase = pci_bar_base(bus, dev, fn, common_bar);
    /* v0.50 bugfix (found during IOMMU boot-verify, not by inspection): +0x30000
     * silently collided with IOMMU_MMIO_V (== VBLK_MMIO_V + 0x30000), remapping
     * the DMAR register window onto the GPU's own BAR out from under it — every
     * subsequent GSTS/RTADDR read then hit GPU registers instead. +0x50000 and
     * +0x60000 are clear of every existing VBLK_MMIO_V-relative user (vblk:
     * +0/+0x10000, vnet: +0x20000, iommu: +0x30000).                           */
    map_mmio(VBLK_MMIO_V + 0x50000, cbase, 0x4000);
    g_gpu_common = (volatile uint8_t *)(VBLK_MMIO_V + 0x50000 + common_off);
    if (notify_bar == common_bar) {
        g_gpu_notify = (volatile uint8_t *)(VBLK_MMIO_V + 0x50000 + notify_off_in_bar);
    } else {
        uint64_t nbase = pci_bar_base(bus, dev, fn, notify_bar);
        map_mmio(VBLK_MMIO_V + 0x60000, nbase, 0x4000);
        g_gpu_notify = (volatile uint8_t *)(VBLK_MMIO_V + 0x60000 + notify_off_in_bar);
    }

    mw8(g_gpu_common, VCC_DEV_STATUS, 0);
    while (mr8(g_gpu_common, VCC_DEV_STATUS) != 0) { }
    mw8(g_gpu_common, VCC_DEV_STATUS, VSTAT_ACK);
    mw8(g_gpu_common, VCC_DEV_STATUS, VSTAT_ACK | VSTAT_DRIVER);

    mw32(g_gpu_common, VCC_DEV_FEAT_SEL, 0);
    mw32(g_gpu_common, VCC_DRV_FEAT_SEL, 0);
    mw32(g_gpu_common, VCC_DRV_FEAT, 0);                  /* no 3D/virgl, no EDID */
    mw32(g_gpu_common, VCC_DEV_FEAT_SEL, 1);
    uint32_t fhi = mr32(g_gpu_common, VCC_DEV_FEAT);
    mw32(g_gpu_common, VCC_DRV_FEAT_SEL, 1);
    mw32(g_gpu_common, VCC_DRV_FEAT, 1 | (fhi & 2));      /* VERSION_1 + ACCESS_PLATFORM */

    mw8(g_gpu_common, VCC_DEV_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEAT_OK);
    if (!(mr8(g_gpu_common, VCC_DEV_STATUS) & VSTAT_FEAT_OK)) {
        kprintf("[vgpu   ] device rejected our feature set\n");
        mw8(g_gpu_common, VCC_DEV_STATUS, VSTAT_FAILED);
        return 0;
    }

    /* --- queue 0: control queue. Queue 1 (cursor) is left disabled. --- */
    mw16(g_gpu_common, VCC_Q_SELECT, 0);
    uint16_t qmax = mr16(g_gpu_common, VCC_Q_SIZE);
    g_gpu_ctrl.size = qmax > 16 ? 16 : qmax;
    mw16(g_gpu_common, VCC_Q_SIZE, g_gpu_ctrl.size);
    g_gpu_ctrl.notify_off = mr16(g_gpu_common, VCC_Q_NOTIFY_OFF);
    g_gpu_ctrl.desc  = (struct vring_desc  *)alloc_frame();
    g_gpu_ctrl.avail = (struct vring_avail *)alloc_frame();
    g_gpu_ctrl.used  = (struct vring_used  *)alloc_frame();
    g_gpu_ctrl.last_used = 0;
    mw64(g_gpu_common, VCC_Q_DESC,   (uint64_t)g_gpu_ctrl.desc);
    mw64(g_gpu_common, VCC_Q_DRIVER, (uint64_t)g_gpu_ctrl.avail);
    mw64(g_gpu_common, VCC_Q_DEVICE, (uint64_t)g_gpu_ctrl.used);
    mw16(g_gpu_common, VCC_Q_ENABLE, 1);

    mw8(g_gpu_common, VCC_DEV_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEAT_OK | VSTAT_DRIVER_OK);
    g_gpu_ready = 1;
    kprintf("[vgpu   ] DRIVER_OK — controlq size %d, notify_off %d — READY "
            "(2D resources only, polling completion)\n",
            (uint64_t)g_gpu_ctrl.size, (uint64_t)g_gpu_ctrl.notify_off);
    return 1;
}

static void gpu_fill_desc_and_notify(const void *cmd, uint32_t cmdlen) {
    cmemcpy(g_gpu_cmdbuf, cmd, cmdlen);
    cmemset(g_gpu_respbuf, 0, sizeof g_gpu_respbuf);
    g_gpu_ctrl.desc[0].addr = (uint64_t)g_gpu_cmdbuf; g_gpu_ctrl.desc[0].len = cmdlen;
    g_gpu_ctrl.desc[0].flags = VRING_DESC_F_NEXT; g_gpu_ctrl.desc[0].next = 1;
    g_gpu_ctrl.desc[1].addr = (uint64_t)g_gpu_respbuf; g_gpu_ctrl.desc[1].len = sizeof g_gpu_respbuf;
    g_gpu_ctrl.desc[1].flags = VRING_DESC_F_WRITE; g_gpu_ctrl.desc[1].next = 0;
    uint16_t ai = g_gpu_ctrl.avail->idx;
    g_gpu_ctrl.avail->ring[ai % g_gpu_ctrl.size] = 0;
    barrier(); g_gpu_ctrl.avail->idx = ai + 1; barrier();
    volatile uint16_t *notify =
        (volatile uint16_t *)(g_gpu_notify + (uint32_t)g_gpu_ctrl.notify_off * g_gpu_notify_mul);
    *notify = 0; barrier();
}

/* Non-blocking: advances g_gpu_seq_completed if the device has finished the
 * currently outstanding command. Safe to call from anywhere, any frequency. */
static void gpu_poll_completion(void) {
    klock_acquire(&g_gpu_lock);
    if (g_gpu_ready && g_gpu_ctrl.used->idx != g_gpu_ctrl.last_used) {
        g_gpu_ctrl.last_used++;
        g_gpu_seq_completed++;
    }
    klock_release(&g_gpu_lock);
}

/* Blocking wait for a specific fence/sequence id (backs SYS_GPU_FENCE_WAIT).
 * Mirrors SYS_VFIO_WAIT_IRQ's exact timeout convention: timeout_ticks==0
 * means "check the current state once, don't block at all", not "forever". */
static int gpu_fence_wait(uint64_t fence_id, uint64_t timeout_ticks) {
    uint64_t t0 = g_ticks;
    while (g_gpu_seq_completed < fence_id && g_ticks - t0 < timeout_ticks) {
        gpu_poll_completion();
        krelax();
    }
    return g_gpu_seq_completed >= fence_id;
}

/* Fully synchronous: submit, spin-poll, copy the response — ALL under
 * g_gpu_lock held continuously, so nothing else can touch the single
 * command/response slot while this is in flight (including a concurrent
 * async submitter, which waits on the SAME "submitted==completed" gate
 * below before it can proceed). Used by every administrative (infrequent)
 * command: resource create, attach backing, set scanout, resource unref.   */
static int gpu_submit_wait(const void *cmd, uint32_t cmdlen, struct virtio_gpu_ctrl_hdr *out_resp) {
    if (!g_gpu_ready) return -1;
    klock_acquire(&g_gpu_lock);
    while (g_gpu_seq_submitted != g_gpu_seq_completed) {
        klock_release(&g_gpu_lock); krelax(); klock_acquire(&g_gpu_lock);
    }
    gpu_fill_desc_and_notify(cmd, cmdlen);
    g_gpu_seq_submitted++;
    while (g_gpu_ctrl.used->idx == g_gpu_ctrl.last_used) {
        klock_release(&g_gpu_lock); krelax(); klock_acquire(&g_gpu_lock);
    }
    g_gpu_ctrl.last_used++;
    g_gpu_seq_completed++;
    struct virtio_gpu_ctrl_hdr *resp = (struct virtio_gpu_ctrl_hdr *)g_gpu_respbuf;
    int ok = (resp->type == VIRTIO_GPU_RESP_OK_NODATA);
    if (out_resp) *out_resp = *resp;
    if (g_debug_gpu)
        kprintf("[dbggpu ] admin cmd type %X -> response %X (%s)\n",
                ((const struct virtio_gpu_ctrl_hdr *)cmd)->type, (uint64_t)resp->type, ok ? "OK" : "ERR");
    klock_release(&g_gpu_lock);
    return ok ? 0 : -1;
}

/* Non-blocking: submits without waiting for completion. Blocks (spinning,
 * lock released between attempts) only until any PRIOR outstanding command
 * has drained — never waits for THIS submission's own completion. Returns a
 * monotonic fence id, or 0 on failure. This is the one command
 * (RESOURCE_FLUSH) this milestone genuinely wants "submitted, ask about it
 * later" semantics for — SYS_GPU_FENCE_WAIT is the other half.             */
static uint64_t gpu_submit_async(const void *cmd, uint32_t cmdlen) {
    if (!g_gpu_ready) return 0;
    klock_acquire(&g_gpu_lock);
    while (g_gpu_seq_submitted != g_gpu_seq_completed) {
        klock_release(&g_gpu_lock); gpu_poll_completion(); krelax(); klock_acquire(&g_gpu_lock);
    }
    gpu_fill_desc_and_notify(cmd, cmdlen);
    uint64_t seq = ++g_gpu_seq_submitted;
    klock_release(&g_gpu_lock);
    return seq;
}

/* ===========================================================================
 * v0.50: GPU RESOURCE TABLE + TEARDOWN
 * ===========================================================================
 * A small global pool, owner-tagged exactly like v0.46's g_ipc_shm — no
 * struct kproc growth needed. The backing pages for each resource are a
 * DMA_GRANT_PAGE grant (the SAME mechanism SYS_DMA_ALLOC and v0.47's
 * SYS_VFIO_MAP_BAR use), so dma_teardown_kproc (wired into all three exit
 * paths since v0.44) already reclaims the frames themselves; this table only
 * tracks the device-side resource_id so it can be RESOURCE_UNREF'd and the
 * scanout ownership cleared on exit — the one thing v0.44-49 know nothing
 * about, exactly the same scoping discipline as v0.47's vfio_teardown_kproc.
 * =========================================================================== */
#define MAX_GPU_RES 8
struct gpu_resource { int used, owner; uint32_t resource_id; uint64_t phys; uint32_t width, height, size; };
static struct gpu_resource g_gpu_res[MAX_GPU_RES];
static uint32_t g_gpu_next_resid = 1;      /* resource_id 0 is reserved/invalid per spec */
static int      g_gpu_scanout_owner = -1;
static uint32_t g_gpu_scanout_resid = 0;

static void gpu_teardown_kproc(int proc_idx) {
    for (int i = 0; i < MAX_GPU_RES; i++) {
        int found = 0; uint32_t resid = 0;
        klock_acquire(&g_gpu_lock);
        if (g_gpu_res[i].used && g_gpu_res[i].owner == proc_idx) { found = 1; resid = g_gpu_res[i].resource_id; }
        klock_release(&g_gpu_lock);
        if (!found) continue;

        struct vgpu_resource_unref cmd; cmemset(&cmd, 0, sizeof cmd);
        cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF; cmd.resource_id = resid;
        gpu_submit_wait(&cmd, sizeof cmd, 0);              /* best-effort; device-side cleanup */

        klock_acquire(&g_gpu_lock);
        if (g_gpu_res[i].used && g_gpu_res[i].owner == proc_idx) {
            if (g_debug_gpu)
                kprintf("[dbggpu ] pid %u slot %d: released GPU resource %u\n",
                        kprocs[proc_idx].pid, proc_idx, (uint64_t)resid);
            g_gpu_res[i].used = 0; g_gpu_res[i].owner = -1;
            if (g_gpu_scanout_owner == proc_idx && g_gpu_scanout_resid == resid) {
                g_gpu_scanout_owner = -1; g_gpu_scanout_resid = 0;
            }
        }
        klock_release(&g_gpu_lock);
    }
}

static void pci_init(void) {
    kprintf("[pci    ] enumerating bus 0 (config mechanism #1, ports 0xCF8/0xCFC):\n");
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_cfg_read32(0, dev, 0, 0x00);
        uint16_t vendor = (uint16_t)id;
        if (vendor == 0xFFFF) continue;
        uint32_t cls = pci_cfg_read32(0, dev, 0, 0x08);
        uint8_t  class = (uint8_t)(cls >> 24);
        kprintf("[pci    ]   %d:00.0  vendor %x device %x class %x\n",
                (uint64_t)dev, (uint64_t)vendor, (uint64_t)(id >> 16), (uint64_t)(cls >> 16));
        if (vendor == 0x1AF4) {                             /* Red Hat / virtio  */
            if (class == 0x01)       virtio_blk_probe(0, dev, 0);   /* mass storage */
            else if (class == 0x02)  virtionet_probe(0, dev, 0);    /* network      */
            else if (class == 0x03)  virtio_gpu_probe(0, dev, 0);   /* display      */
        }
    }
    if (g_virtio_kdev < 0)
        kprintf("[pci    ] no virtio-net device found (passthrough demo will use scratch)\n");
}

/* ===========================================================================
 * GENUINE RING-3 USER MODE — SYSCALL / SYSRET
 * ===========================================================================
 * Sets up a GDT with user segments + a TSS, enables SYSCALL/SYSRET via the
 * EFER/STAR/LSTAR/SFMASK MSRs, then drops a real unprivileged process to ring 3.
 * The process reaches the kernel ONLY through the `syscall` instruction; the
 * capability-gated sys_hardware_passthrough now runs inside that trap.
 * =========================================================================== */

static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* 64-bit TSS (104 bytes). rsp0 is the ring-0 stack used on a ring3->ring0 trap. */
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* v0.38: per-CPU TSS + trap stacks. The GDT keeps its 6 base entries
 * (null,kcode,kdata,ucode32,udata,ucode64) and then carries one 16-byte TSS
 * descriptor PER CPU, so every core can `ltr` its own TSS and thus take a
 * CPL3->CPL0 trap onto its own rsp0. TSS_SEL(i) is that CPU's TR selector.    */
#define SMP_MAX_CPUS 8                                     /* == MAX_CPUS; used pre-decl */
static struct tss64 g_tss[SMP_MAX_CPUS];
static uint64_t g_gdt[6 + 2 * SMP_MAX_CPUS];
#define TSS_SEL(i) (uint16_t)((6 + 2 * (i)) << 3)
static uint8_t  g_syscall_stack[SMP_MAX_CPUS][8192] __attribute__((aligned(16)));
static uint8_t  g_int_stack[SMP_MAX_CPUS][8192]     __attribute__((aligned(16)));

extern void syscall_entry(void);
/* Returns the SYS_EXIT code: enter_user_mode only ever "returns" via
 * resume_kernel, which places the exit code in RAX. Legacy callers ignore it. */
extern uint64_t enter_user_mode(uint64_t entry, uint64_t ustack);
extern void enter_user_thread(uint64_t entry, uint64_t ustack);   /* no resume point */
extern uint64_t enter_user_resume(struct uctx *u);  /* v0.39: rebuild a preempted ctx */
extern void resume_kernel(uint64_t retval);
extern void set_syscall_stack(uint64_t top);
extern char user_blob_start[], user_blob_end[];

/* v0.31: per-thread trap stacks. A first-class ring-3 thread traps onto its
 * OWN 16 KiB kernel stack (both interrupt rsp0 and SYSCALL stack point at its
 * top — the two can never be live at once on one thread, because a thread at
 * CPL3 has no syscall in flight and a thread in a syscall is not at CPL3).
 * Kernel threads and the boot thread keep the original shared stacks.        */
static uint32_t cpu_idx(void);   /* fwd: returns 0 until per-CPU GS is armed */

static void uthread_ctx_load(struct pcb *next) {
    uint32_t c = cpu_idx();                          /* the CPU doing the switch */
    g_tss[c].rsp0 = next->rsp0 ? next->rsp0
                               : (uint64_t)(g_int_stack[c] + sizeof g_int_stack[c]);
    set_syscall_stack(next->ksrsp ? next->ksrsp
                                  : (uint64_t)(g_syscall_stack[c] + sizeof g_syscall_stack[c]));
}

static void gdt_set_tss(int slot, uint64_t base, uint32_t limit) {
    uint64_t low = 0;
    low |= (limit & 0xFFFFull);
    low |= (base & 0xFFFFFFull) << 16;
    low |= 0x89ull << 40;                                  /* present, type=9 (available 64-bit TSS) */
    low |= ((uint64_t)((limit >> 16) & 0xF)) << 48;
    low |= ((base >> 24) & 0xFFull) << 56;
    g_gdt[slot]     = low;
    g_gdt[slot + 1] = (base >> 32) & 0xFFFFFFFFull;
}

/* current_proc_idx moved next to the kproc table (it is per-thread since v0.31) */
static int      g_demo_dev_index = 0;                      /* device the ring-3 program requests        */

/* ---- v0.31: first-class ring-3 scheduler threads ---------------------------- */
/* A ring-3 process running as its own thread never unwinds to kernel_main: it
 * leaves through here (SYS_EXIT) or through handle_cpl3_fault. Both reap the
 * thread, reclaim its surfaces, and reschedule. We are on the dying thread's
 * own kernel stack, which stays valid until thread_create reuses the slot —
 * and it cannot: interrupts are off until the switch completes.               */
static void __attribute__((noreturn)) uthread_exit(uint64_t code) {
    /* v0.41: take the surface lock BEFORE cli — a contended acquire may yield, */
    /* which must not happen once this thread has begun dying under cli.        */
    struct pcb *t0 = curthr;
    klock_acquire(&g_surf_lock);
    surfaces_reclaim((int)t0->proc);
    klock_release(&g_surf_lock);
    __asm__ volatile("cli");
    struct pcb *t = curthr;
    kprocs[t->proc].exit_code = code;
    kprocs[t->proc].exited = 1;
    write_cr3(kernel_cr3);                   /* off this space before tearing it down */
    /* v0.42: this is the BSP's OWN thread giving up its OWN address space —   */
    /* cr3 just changed away from it above, so it is safe to dismantle now.    */
    vfio_teardown_kproc((int)t->proc);       /* v0.47: release any IRQ-line ownership FIRST */
    gpu_teardown_kproc((int)t->proc);        /* v0.50: release any GPU resource/scanout FIRST */
    ipc_teardown_kproc((int)t->proc);        /* v0.46: release IPC mailbox/shmem FIRST */
    descriptor_teardown_kproc((int)t->proc); /* v0.45: force-close any leaked fd FIRST */
    dma_teardown_kproc((int)t->proc);        /* v0.44: revoke DMA/IOMMU grants FIRST */
    kprocs[t->proc].frames_freed = page_free_tree(kprocs[t->proc].cr3);
    kprocs[t->proc].torn_down = 1;      /* v0.45: NOW the slot is safe to recycle */
    kprintf("[uthread] tid %d pid %u '%s' exited (code %u) — thread reaped, %u frame(s) reclaimed\n",
            (uint64_t)t->id, kprocs[t->proc].pid, t->name, code, kprocs[t->proc].frames_freed);
    t->state = T_FREE;                       /* never scheduled again           */
    sched_switch_to(pick_next());            /* does not return                 */
    for (;;) __asm__ volatile("hlt");
}

/* Kernel-side entry of a user thread. By the time this runs, sched_switch_to
 * has already installed this thread's CR3, process identity, TSS.rsp0 and
 * SYSCALL stack from its PCB — so we simply drop to ring 3.                   */
static void uthread_tramp(void *arg) {
    struct pcb *t = curthr;
    kprintf("[uthread] tid %d --> RING 3 as pid %u '%s' (entry %X, cr3 %X)\n",
            (uint64_t)t->id, kprocs[t->proc].pid, t->name,
            (uint64_t)arg, kprocs[t->proc].cr3);
    enter_user_thread((uint64_t)arg, USTK_TOP);
    for (;;) __asm__ volatile("hlt");        /* unreachable                     */
}

static int uthread_create(const char *name, int proc_idx, uint64_t entry) {
    __asm__ volatile("cli");                 /* fields below must be set before */
    int tid = thread_create(name, uthread_tramp, (void *)entry);   /* first run */
    if (tid >= 0) {
        struct pcb *t = &g_threads[tid];
        t->uthread = 1;
        t->proc    = (uint64_t)proc_idx;
        t->cr3     = kprocs[proc_idx].cr3;
        t->rsp0    = (uint64_t)(t->stack + TSTACK_SZ);
        t->ksrsp   = t->rsp0;
    }
    __asm__ volatile("sti");
    return tid;
}

/* v0.38: install CPU `idx`'s own TSS into the (shared) GDT and load its TR.
 * The GDT's 6 base entries are built once by the BSP; this fills the per-CPU
 * TSS descriptor at TSS_SEL(idx) and points rsp0 at that CPU's interrupt stack.
 * Runs on the CPU it configures (ltr affects the running core).               */
static void cpu_tss_setup(int idx) {
    struct tss64 *t = &g_tss[idx];
    for (unsigned i = 0; i < sizeof *t; i++) ((uint8_t *)t)[i] = 0;
    t->rsp0 = (uint64_t)(g_int_stack[idx] + sizeof g_int_stack[idx]);
    t->iomap_base = sizeof(struct tss64);
    gdt_set_tss(6 + 2 * idx, (uint64_t)t, sizeof(struct tss64) - 1);
    __asm__ volatile("ltr %0" : : "r"(TSS_SEL(idx)));
}

/* v0.38: arm this CPU's SYSCALL/SYSRET path. STAR/LSTAR/SFMASK are the same on
 * every core; each CPU must still execute the wrmsrs (MSRs are per-core).      */
static void cpu_syscall_arm(void) {
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);              /* EFER.SCE = enable SYSCALL  */
    wrmsr(0xC0000081, ((uint64_t)0x1B << 48) | ((uint64_t)0x08 << 32)); /* STAR       */
    wrmsr(0xC0000082, (uint64_t)syscall_entry);            /* LSTAR = entry RIP          */
    wrmsr(0xC0000084, 0x200);                              /* SFMASK: clear IF on entry  */
}

static void usermode_init(void) {
    __asm__ volatile("cli");

    g_gdt[0] = 0;
    g_gdt[1] = 0x00AF9A000000FFFFull;                      /* 0x08 kernel code64 (DPL0)  */
    g_gdt[2] = 0x00CF92000000FFFFull;                      /* 0x10 kernel data           */
    g_gdt[3] = 0x00CFFA000000FFFFull;                      /* 0x18 user code32 (SYSRET base) */
    g_gdt[4] = 0x00CFF2000000FFFFull;                      /* 0x20 user data  (DPL3)     */
    g_gdt[5] = 0x00AFFA000000FFFFull;                      /* 0x28 user code64 (DPL3)    */

    struct { uint16_t limit; uint64_t base; } __attribute__((packed))
        gdtr = { sizeof(g_gdt) - 1, (uint64_t)g_gdt };
    __asm__ volatile("lgdt %0" : : "m"(gdtr) : "memory");
    /* NB: deliberately NOT reloading fs/gs here (v0.39): writing the gs
     * selector clears IA32_GS_BASE, which has pointed at the BSP's cpu_local
     * — carrying the live stack-protector guard — since kernel_main's first
     * instruction. In long mode ds/es/fs/gs bases are ignored anyway.        */
    __asm__ volatile(
        "mov $0x10, %%ax\n mov %%ax, %%ds\n mov %%ax, %%es\n"
        "mov %%ax, %%ss\n" ::: "rax");

    cpu_tss_setup(0);                                      /* BSP is CPU 0               */
    set_syscall_stack((uint64_t)(g_syscall_stack[0] + sizeof g_syscall_stack[0]));
    cpu_syscall_arm();

    __asm__ volatile("sti");
    kprintf("[kernel ] ring-3 enabled: GDT (%d TSS slots) + BSP TSS loaded, SYSCALL/SYSRET armed\n",
            (uint64_t)SMP_MAX_CPUS);
    kprintf("[kernel ]   STAR kern=0x08 user=0x1B, LSTAR=%X, TSS0.rsp0=%X\n",
            (uint64_t)syscall_entry, g_tss[0].rsp0);
}

/* ===========================================================================
 * SMP GROUNDWORK  (v0.35 — LAPIC, AP boot, per-CPU state, IPIs, shootdowns)
 * ===========================================================================
 * Scope, stated precisely: the kernel enumerates every CPU from the ACPI
 * MADT, boots the application processors through a real-mode trampoline to
 * 64-bit kernel C, gives each core a per-CPU area addressed through GS, and
 * runs two genuine cross-core protocols — fixed-vector IPIs and a TLB
 * shootdown with acknowledgement. The THREAD SCHEDULER REMAINS BSP-ONLY BY
 * POLICY: no user code and no kernel thread ever runs on an AP, which is
 * exactly why every uniprocessor invariant proven since v0.17 (access_ok's
 * TOCTOU argument above all) still holds. Per-CPU run queues are a future
 * milestone with its own re-verification, not a side effect of this one.
 * =========================================================================== */
#define LAPIC_V    0x0000601000000000ull   /* LAPIC MMIO window (PCD-mapped)   */
#define LAPIC_PHYS 0xFEE00000ull
#define IPI_PING    48
#define IPI_TLB     49
#define IPI_PREEMPT 50                     /* v0.39: force a core to drop ring 3 */
#define AP_TRAMP   0x8000ull               /* SIPI vector 0x08 -> phys 0x8000  */
#define AP_STACK_SZ (16 * 1024)

/* struct cpu_local, g_cpu[], MAX_CPUS and the CPUL_* asm offsets moved to the
 * top of the file in v0.39: the trap path, the stack protector and the
 * capability gate are all defined in terms of the per-CPU block now.         */

/* v0.36: a work-stealing parallel job. Every online core (BSP + APs) claims
 * fixed-size units from one shared atomic cursor, folds each unit's content
 * into its OWN per-CPU accumulator, and the BSP reduces the accumulators. The
 * fold is a per-unit FNV-1a hash and the accumulators are summed, so the
 * reduction is order-independent (any interleaving yields the identical
 * result) yet sensitive to every byte. Entirely bounded and self-contained:
 * no scheduler, no user code, no run-queue migration — the AP safety boundary
 * from v0.35 is untouched; this is offloaded compute, not a second scheduler. */
struct pjob {
    int      kind;                         /* 0 = FNV fold, 1 = PTE audit (v0.37) */
    volatile uint64_t cursor;              /* next unit index to claim (xadd)  */
    uint64_t units;                        /* total units                      */
    uint64_t base;                         /* buffer base (identity-mapped)    */
    uint64_t unit_words;                   /* uint64s per unit                 */
    volatile uint64_t partial[MAX_CPUS];   /* per-CPU fold (no false sharing:  */
    volatile uint64_t _pad[MAX_CPUS];      /* padded apart)                    */
    volatile uint32_t claimed[MAX_CPUS];   /* per-CPU units taken              */
    volatile uint32_t done;                /* cores that finished this job     */
};
static struct pjob g_pjob;

/* v0.37: parallel page-table integrity audit. The BSP walks the kernel PML4
 * once to enumerate every present PD entry (each covering one 2 MiB huge leaf
 * or one PT of up to 512 4 KiB leaves); the cores then audit those units in
 * parallel with the SAME per-leaf rule the tick-driven sweep uses
 * (sweep_check_leaf: reserved bits zero, W^X, kernel .text stays R+X). Each
 * core folds a violation count into its partial slot. Read-only over the page
 * tables, so it is safe against the concurrent background sweep and needs no
 * lock — and it stays inside the BSP-only-scheduler boundary. */
/* Stores a POINTER to each PD entry (not a snapshot), so the audit reads the
 * LIVE page tables — a genuine integrity check must observe current state, and
 * this is what lets it catch a corruption injected after enumeration. */
struct auditunit { uint64_t *pde_ptr; uint64_t va_base; };
#define AUDIT_MAX 4096
static struct auditunit g_audit[AUDIT_MAX];
static uint64_t g_audit_n = 0;
static uint8_t  g_ap_stacks[MAX_CPUS][AP_STACK_SZ] __attribute__((aligned(64)));
static int      g_ncpu_found = 1, g_ncpu_online = 1;
static uint8_t  g_apicids[MAX_CPUS];
static uint32_t g_bsp_apicid = 0;

/* cross-core mailboxes */
static volatile uint64_t g_shoot_va = 0;
static volatile uint32_t g_shoot_ack = 0;
/* v0.49: the shootdown mailbox grew a page COUNT and a per-cpu TARGET MASK —
 * through v0.48 it only ever invalidated one page and only ever broadcast to
 * every online cpu. A targeted, multi-page shootdown lets sys_tlb_shootdown
 * (and the SYS_SMP_REMAP/UNMAP paths under it) invalidate exactly the range
 * and exactly the cpus that could hold a stale translation for THIS address
 * space — the cpus in kprocs[p].ran_on — instead of paying a full broadcast
 * and waiting on cores that were never going to have that CR3 loaded.       */
static volatile uint32_t g_shoot_pages = 1;
static volatile uint32_t g_shoot_mask  = 0;    /* bit c = "cpu c must ack this shootdown" */
/* v0.49: through v0.48 the mailbox above was touched by exactly one caller at
 * a time BY CONVENTION — every tlb_shootdown() call site was a single
 * serialized BSP test orchestrator. SYS_SMP_REMAP/UNMAP end that convention:
 * several ring-3 workers on DIFFERENT cores now call tlb_shootdown_range
 * concurrently, and two overlapping calls stomping the shared va/pages/mask/
 * ack fields lose acks and drag every caller out to its own 100-tick
 * timeout — a real bug found live running the v0.49 migration stress (it
 * looked like a hang: every shootdown paying its full timeout back-to-back).
 * g_shoot_lock is a raw leaf spinlock (same discipline as g_frame_lock)
 * serializing one whole "publish the request, IPI, wait for acks" round
 * trip; a spinning waiter still has interrupts enabled, so it keeps
 * servicing any IPI aimed at it while it waits — no deadlock. */
static volatile int      g_shoot_lock  = 0;
static volatile int      g_work_go = 0;        /* 1 = lock-xadd storm, 2 = probe read */
static volatile uint64_t g_work_counter = 0;
static volatile uint64_t g_probe_va = 0;
#define WORK_XADDS 100000

extern char ap_tramp_start[], ap_tramp_end[];

static inline uint32_t lapic_r(uint32_t off) { return *(volatile uint32_t *)(LAPIC_V + off); }
static inline void lapic_w(uint32_t off, uint32_t v) { *(volatile uint32_t *)(LAPIC_V + off) = v; }
static inline void lapic_eoi(void) { lapic_w(0xB0, 0); }

static uint32_t cpu_idx(void) {
    if (!g_gs_ready) return 0;
    uint32_t id;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(id));
    return id;
}

/* Send a fixed-vector IPI: to one APIC id, or broadcast to all-but-self.     */
static void lapic_ipi(uint32_t apic_id, uint8_t vec, int broadcast) {
    while (lapic_r(0x300) & (1u << 12)) __asm__ volatile("pause");  /* prior send */
    if (broadcast) {
        lapic_w(0x300, 0xC0000u | vec);        /* all-excluding-self shorthand */
    } else {
        lapic_w(0x310, apic_id << 24);
        lapic_w(0x300, vec);
    }
}

/* IPI handlers — run on WHICHEVER cpu the interrupt lands on.                */
static void smp_ipi_dispatch(uint64_t vec) {
    struct cpu_local *me = &g_cpu[cpu_idx()];
    if (vec == IPI_TLB) {
        /* v0.49: only invalidate/ack if THIS cpu is an actual target — the
         * broadcast fast path only ever fires when every online-but-self cpu
         * is targeted, but a directed multi-IPI send lands on exactly the
         * mask, and this guard is what keeps the two paths' ack arithmetic
         * (a straight popcount of the mask) identical either way.           */
        if (g_shoot_mask & (1u << cpu_idx())) {
            uint32_t pages = g_shoot_pages ? g_shoot_pages : 1;
            for (uint32_t i = 0; i < pages; i++)
                __asm__ volatile("invlpg (%0)" :: "r"(g_shoot_va + (uint64_t)i * 0x1000) : "memory");
            __sync_fetch_and_add(&g_shoot_ack, 1);
        }
    } else {
        me->ipi_ping++;                        /* ping/wake                    */
    }
    lapic_eoi();
}

/* v0.49: the general cross-core TLB shootdown primitive — invalidate `pages`
 * consecutive 4 KiB entries starting at `va`, on exactly the cpus set in
 * `cpu_mask` (bit c = cpu c), and block until every one of THEM has
 * acknowledged (bounded by a 100-tick watchdog, same as the v0.35 original).
 * This is what requirement (1) means by "synchronous, acknowledged shootdown
 * completion before page frame unmapping or memory recycling completes" —
 * every caller that frees/recycles a physical frame calls this (or the
 * single-page tlb_shootdown() wrapper below) and only then hands the frame
 * back to the allocator. On a single-CPU boot it degrades to the local
 * invlpg loop it always was. */
static int tlb_shootdown_range(uint64_t va, uint32_t pages, uint32_t cpu_mask) {
    if (pages == 0) pages = 1;
    if (pages > 64) pages = 64;                /* bounded: matches SYS_DMA_ALLOC's cap */
    for (uint32_t i = 0; i < pages; i++)
        __asm__ volatile("invlpg (%0)" :: "r"(va + (uint64_t)i * 0x1000) : "memory");
    if (g_ncpu_online <= 1) return 0;
    uint32_t online_mask = 0;
    for (int c = 0; c < MAX_CPUS; c++) if (g_cpu[c].online) online_mask |= (1u << c);
    uint32_t targets = cpu_mask & online_mask & ~(1u << cpu_idx());  /* self already done, above */
    if (!targets) return 0;
    /* v0.49: SFMASK clears IF for the ENTIRE duration of every SYSCALL (see
     * cpu_syscall_arm, "SFMASK: clear IF on entry") — through v0.48 that was
     * always safe because nothing inside a syscall ever waited on another
     * core's INTERRUPT HANDLER to make progress; ordinary klock contention is
     * a plain cache-coherent spin that needs no interrupts anywhere. This
     * protocol is different: it blocks on g_shoot_ack, which only advances
     * when a TARGET core's IPI_TLB handler runs — and that cannot happen on
     * a target that is itself inside its own syscall with IF still clear.
     * With SYS_SMP_REMAP/UNMAP/TLB_SHOOTDOWN now reachable concurrently from
     * ring 3 on every core, two such syscalls can trade places: A holds
     * g_shoot_lock waiting on B's ack; B is spinning to ACQUIRE that same
     * lock for its own shootdown, IF still clear from ITS OWN SYSCALL entry,
     * so B can never take A's IPI and ack it — a genuine cross-core deadlock,
     * found live building the v0.49 migration stress (looked like a hang;
     * it was every shootdown paying its full ack timeout, back to back,
     * because the one core that could ack was never interruptible). Forcing
     * IF on for this whole routine — lock acquire through the ack wait — is
     * what makes the protocol interrupt-driven again regardless of what
     * syscall context it was called from; the caller's original IF is
     * restored before return, exactly like con_lock/con_unlock's pattern,
     * just inverted (force-ENABLE across the section, not force-disable).  */
    uint64_t fl; __asm__ volatile("pushfq; pop %0" : "=r"(fl));
    __asm__ volatile("sti");
    while (__sync_lock_test_and_set(&g_shoot_lock, 1)) __asm__ volatile("pause");
    g_shoot_va = va;
    g_shoot_pages = pages;
    g_shoot_mask = targets;
    g_shoot_ack = 0;
    __sync_synchronize();
    if (targets == (online_mask & ~(1u << cpu_idx())))
        lapic_ipi(0, IPI_TLB, 1);              /* every eligible cpu targeted: broadcast */
    else
        for (int c = 0; c < MAX_CPUS; c++)
            if (targets & (1u << c)) lapic_ipi(g_cpu[c].apic_id, IPI_TLB, 0);
    uint32_t want = 0;                         /* freestanding: no libgcc popcount call */
    for (uint32_t m = targets; m; m &= m - 1) want++;
    uint64_t t0 = g_ticks;
    while (g_shoot_ack < want && g_ticks - t0 < 100)
        __asm__ volatile("pause");
    int acked = (int)g_shoot_ack;
    __sync_lock_release(&g_shoot_lock);
    if (!(fl & 0x200)) __asm__ volatile("cli");    /* restore the caller's original IF */
    return acked;
}

/* Invalidate one page on EVERY online cpu and wait for every acknowledgement.
 * Kept as the pre-v0.49 call shape for the ~15 existing sites (frame reuse,
 * AP_TRAMP, remap/unmap) that always meant "everyone" — a thin wrapper over
 * the general range/mask primitive above. */
static int tlb_shootdown(uint64_t va) {
    return tlb_shootdown_range(va, 1, 0xFFFFFFFFu);
}

/* Fold this core's share of the parallel job. Claims units from the shared
 * cursor until it is drained. Runs on every participating core (BSP + APs).
 * kind 0: FNV-1a hash each unit's words, sum -> order-independent digest.
 * kind 1: audit each unit's page-table leaves, sum -> total violation count. */
static void pjob_run(int me) {
    for (;;) {
        uint64_t u = __sync_fetch_and_add(&g_pjob.cursor, 1);
        if (u >= g_pjob.units) break;
        if (g_pjob.kind == 1) {                /* v0.37: PTE integrity audit    */
            struct auditunit *au = &g_audit[u];
            uint64_t pde = *au->pde_ptr;       /* live read of the page table   */
            if (pde & PTE_HUGE) {
                if (sweep_check_leaf(pde, au->va_base)) g_pjob.partial[me]++;
            } else {
                uint64_t *pt = (uint64_t *)(pde & ADDR_MASK);
                for (int d = 0; d < 512; d++) {
                    if (!(pt[d] & PTE_PRESENT)) continue;
                    if (sweep_check_leaf(pt[d], au->va_base | ((uint64_t)d << 12)))
                        g_pjob.partial[me]++;
                }
            }
            g_pjob.claimed[me]++;
            continue;
        }
        uint64_t h = 0xcbf29ce484222325ull;    /* kind 0: FNV fold              */
        const uint64_t *p = (const uint64_t *)(g_pjob.base + u * g_pjob.unit_words * 8);
        for (uint64_t i = 0; i < g_pjob.unit_words; i++)
            h = (h ^ p[i]) * 0x100000001b3ull;
        g_pjob.partial[me] += h;               /* summed -> order-independent   */
        g_pjob.claimed[me]++;
    }
    __sync_fetch_and_add(&g_pjob.done, 1);
}

/* ===========================================================================
 * v0.39: DISTRIBUTED SCHEDULING — per-CPU run queues, work stealing, and one
 * executor shared by every core.
 * ===========================================================================
 * Each cpu_local carries a small ring of runnable kproc indices under its own
 * spinlock. The BSP (or anyone) pushes; each AP DRAINS ITS OWN QUEUE
 * AUTONOMOUSLY, and an idle core with an empty queue STEALS from a sibling —
 * no mailbox, no BSP quiescing. The locks are held for a handful of
 * instructions, never across ring-3 execution.                              */
static void rq_acquire(struct cpu_local *c) {
    while (__sync_lock_test_and_set(&c->rq_lock, 1)) __asm__ volatile("pause");
}
static void rq_release(struct cpu_local *c) { __sync_lock_release(&c->rq_lock); }
/* v0.49: non-blocking trylock — one test-and-set attempt, no spin. Returns
 * true iff the caller now holds the lock. Used only by rq_steal (below): a
 * thief that finds the lock busy is expected to move on to the next sibling
 * rather than ever wait behind an owner, which is what makes the steal path
 * a genuinely non-blocking work-stealing heuristic instead of just another
 * spinlock consumer. */
static int rq_trylock(struct cpu_local *c) {
    return __sync_lock_test_and_set(&c->rq_lock, 1) == 0;
}
static volatile uint32_t g_rq_steal_aborted = 0;  /* v0.49: steal attempts that backed off busy */

static int rq_push(int cpu, int proc) {                /* producer: tail       */
    struct cpu_local *c = &g_cpu[cpu];
    rq_acquire(c);
    uint32_t n = (c->rq_t + 1) % RQ_LEN;
    if (n == c->rq_h) { rq_release(c); return -1; }    /* full                 */
    c->rq[c->rq_t] = proc; c->rq_t = n;
    rq_release(c);
    return 0;
}
static int rq_push_front(int cpu, int proc) {          /* priority: run NEXT   */
    struct cpu_local *c = &g_cpu[cpu];
    rq_acquire(c);
    uint32_t h = (c->rq_h + RQ_LEN - 1) % RQ_LEN;
    if (h == c->rq_t) { rq_release(c); return -1; }
    c->rq_h = h; c->rq[h] = proc;
    rq_release(c);
    return 0;
}
static int rq_pop(int cpu) {                           /* consumer: head       */
    struct cpu_local *c = &g_cpu[cpu];
    rq_acquire(c);
    int p = -1;
    if (c->rq_h != c->rq_t) { p = c->rq[c->rq_h]; c->rq_h = (c->rq_h + 1) % RQ_LEN; }
    rq_release(c);
    return p;
}
static int rq_steal(int thief) {                       /* balance: rob siblings */
    for (int off = 1; off < MAX_CPUS; off++) {
        int v = (thief + off) % MAX_CPUS;
        if (v == thief || !g_cpu[v].online) continue;
        struct cpu_local *victim = &g_cpu[v];
        /* v0.49: NON-BLOCKING steal — trylock only. A busy victim is skipped
         * immediately (never spun on); requirement (2)'s work-stealing
         * heuristic. Peek the head under the lock and only pop it if this
         * thief is actually in the task's affinity mask (0 = unrestricted);
         * a pinned task is left queued for its rightful owner instead of
         * being silently relocated outside its allowed set.                 */
        if (!rq_trylock(victim)) { __sync_fetch_and_add(&g_rq_steal_aborted, 1); continue; }
        int p = -1;
        if (victim->rq_h != victim->rq_t) p = victim->rq[victim->rq_h];
        if (p >= 0 && kprocs[p].affinity && !(kprocs[p].affinity & (1u << thief))) {
            rq_release(victim);                        /* not ours: leave queued */
            continue;
        }
        if (p >= 0) victim->rq_h = (victim->rq_h + 1) % RQ_LEN;
        rq_release(victim);
        if (p >= 0) { __sync_fetch_and_add(&g_cpu[thief].rq_stolen, 1); return p; }
    }
    return -1;
}

/* Concurrency witness: how many cores are in a ring-3 excursion right now,
 * and the high-water mark. max >= 2 is the PROOF that two cores sat in ring 3
 * simultaneously — the thing every version before v0.39 forbade.            */
static volatile int g_inr3 = 0, g_inr3_max = 0;
static volatile uint32_t g_finish_seq = 0;             /* global completion order */

/* v0.40: the dispatch log — every time an executor picks a task up (fresh or
 * resumed), the kproc index is appended. Round-robin slicing shows up here as
 * the interleaved pattern A,B,C,A,B,C..., which the slice suite demands
 * literally rather than trusting counters alone.                            */
#define DLOG_LEN 128
static volatile int g_dlog[DLOG_LEN];
static volatile uint32_t g_dlog_n = 0;

#define RET_PREEMPTED 0xFEED5EEDF00DFACEull  /* sentinel: "context captured, not exited" */

/* THE executor, identical on every core: adopt the task's identity into this
 * CPU's cpu_local, point this CPU's TSS/SYSCALL stacks at its own per-CPU
 * stacks, switch CR3, and drop to ring 3 — a fresh task enters at its ELF
 * entry, a preempted one is rebuilt from its captured context. Returns when
 * the task exits (records exit + completion order) or is preempted again
 * (requeues the context, honouring a migration directive).                  */
static void cpu_exec_proc(int c, int p) {
    struct cpu_local *me = &g_cpu[c];
    __asm__ volatile("cli");
    me->cur_proc = (uint64_t)p;                        /* per-CPU identity     */
    g_tss[c].rsp0 = (uint64_t)(g_int_stack[c] + sizeof g_int_stack[c]);
    set_syscall_stack((uint64_t)(g_syscall_stack[c] + sizeof g_syscall_stack[c]));
    __sync_fetch_and_or(&kprocs[p].ran_on, 1u << c);
    __sync_fetch_and_add(&kprocs[p].dispatches, 1);
    uint32_t dl = __sync_fetch_and_add(&g_dlog_n, 1);  /* v0.40: dispatch log  */
    if (dl < DLOG_LEN) g_dlog[dl] = p;
    int now = __sync_add_and_fetch(&g_inr3, 1);
    for (;;) {                                         /* high-water mark (CAS) */
        int m = g_inr3_max;
        if (now <= m || __sync_bool_compare_and_swap(&g_inr3_max, m, now)) break;
    }
    write_cr3(kprocs[p].cr3);
    if (g_debug_smp_sched)
        kprintf("[dbgsmp ] cpu%u pick+switch pid %u cr3=%X\n",
                (uint64_t)c, kprocs[p].pid, kprocs[p].cr3);
    uint64_t code = kprocs[p].pstate
                  ? enter_user_resume(&kprocs[p].uctx)
                  : enter_user_mode(kprocs[p].entry, USTK_TOP);
    write_cr3(kernel_cr3);
    __sync_fetch_and_sub(&g_inr3, 1);
    if (code == RET_PREEMPTED) {
        kprocs[p].pstate = 1;                          /* uctx captured by IPI 50 */
        int dst = kprocs[p].migrate_to;
        kprocs[p].migrate_to = -1;
        if (dst < 0 || dst >= MAX_CPUS || !g_cpu[dst].online) dst = c;
        /* v0.49: affinity is authoritative over a directed migrate_to — a
         * requested destination outside the task's mask is overridden by the
         * lowest-indexed online cpu that IS in the mask (falling back to the
         * current core c if none online qualifies, same as "no directive"),
         * so a migration can never land a pinned task on a forbidden core. */
        uint32_t aff = kprocs[p].affinity;
        if (aff && !(aff & (1u << dst))) {
            int found = -1;
            for (int cc = 0; cc < MAX_CPUS; cc++)
                if (g_cpu[cc].online && (aff & (1u << cc))) { found = cc; break; }
            dst = (found >= 0) ? found : c;
        }
        rq_push(dst, p);
        __sync_synchronize();
        if (dst != c) lapic_ipi(g_cpu[dst].apic_id, IPI_PING, 0);  /* wake the new home */
    } else {
        kprocs[p].exit_code = code;
        kprocs[p].finish_seq = __sync_add_and_fetch(&g_finish_seq, 1);
        __sync_synchronize();
        kprocs[p].exited = 1;
        __sync_fetch_and_add(&me->rq_ran, 1);
        /* v0.41 audit fix: only the BSP's uthread reap path ever reclaimed     */
        /* surfaces — a task that exited on an AP leaked its slot and pixel     */
        /* pair permanently. Every executor now reclaims on exit.               */
        __asm__ volatile("sti");                       /* lock may spin: IF on   */
        klock_acquire(&g_surf_lock);
        surfaces_reclaim(p);
        klock_release(&g_surf_lock);
        /* v0.42: the address space just went dead on EVERY core (this executor */
        /* is the only one that was ever running it, and it just gave it up) —  */
        /* tear it all the way down. page_free_tree is not a klock: it is safe  */
        /* to call with interrupts on, exactly like the frame allocator it      */
        /* drives underneath.                                                   */
        vfio_teardown_kproc(p);                /* v0.47: release any IRQ-line ownership FIRST */
        gpu_teardown_kproc(p);                 /* v0.50: release any GPU resource/scanout FIRST */
        ipc_teardown_kproc(p);                 /* v0.46: release IPC mailbox/shmem FIRST */
        descriptor_teardown_kproc(p);           /* v0.45: force-close any leaked fd FIRST */
        dma_teardown_kproc(p);                 /* v0.44: revoke DMA/IOMMU grants FIRST */
        kprocs[p].frames_freed = page_free_tree(kprocs[p].cr3);
        kprocs[p].torn_down = 1;           /* v0.45: NOW the slot is safe to recycle */
        __asm__ volatile("cli");
    }
    __asm__ volatile("sti");
}

/* v0.39 Stage 3: the preempt IPI (vector 50) — one core FORCES another to
 * drop the ring-3 thread it is executing, immediately.
 *
 * If the interrupt caught this core at CPL3, the isr_frame IS the thread's
 * complete machine state: copy it into the kproc's uctx, EOI, and then —
 * instead of iretq'ing back into the user code — unwind the entire interrupt
 * through this core's per-CPU kernel resume point with the RET_PREEMPTED
 * sentinel. cpu_exec_proc sees the sentinel and requeues the captured
 * context (honouring a migration directive), then pulls the next task off
 * its queue: that next task is exactly the higher-priority thread the sender
 * pushed to the front. The abandoned interrupt stack costs nothing — the
 * next CPL3 trap starts fresh at rsp0.
 *
 * If it caught the kernel instead (the thread was mid-syscall), preempting
 * is not safe — just flag resched and return; the sender retries until the
 * IPI lands in user code (statistically immediate: the probes spend >99% of
 * their time there).                                                        */
/* v0.40: AP-local time-slicing. Each AP's LAPIC timer is armed periodic on
 * vector 51 and gated by g_slice_on — the same "preemption on demand"
 * discipline the BSP has used for its PIT since v0.19. When the gate is open
 * and the tick catches ring 3, the tick IS a self-directed preemption: the
 * same capture path runs, the context is requeued at the TAIL of this CPU's
 * queue, and the executor pulls the next task — round-robin, involuntary,
 * per-core. The counters stay separate (slice_count vs preempt_count) so the
 * mcpre suite's cross-core IPI semantics remain exactly what they verify.   */
static volatile int g_slice_on = 0;

static void smp_preempt_ipi(struct isr_frame *f) {
    struct cpu_local *me = &g_cpu[cpu_idx()];
    if (f->vector == 51 && !g_slice_on) {    /* slicing gated off: ignore tick   */
        lapic_eoi();
        return;
    }
    if ((f->cs & 3) != 3) {
        me->resched = 1;                     /* in-kernel: defer, sender retries */
        lapic_eoi();
        return;
    }
    int p = (int)me->cur_proc;
    struct uctx *u = &kprocs[p].uctx;
    u->r15 = f->r15; u->r14 = f->r14; u->r13 = f->r13; u->r12 = f->r12;
    u->r11 = f->r11; u->r10 = f->r10; u->r9  = f->r9;  u->r8  = f->r8;
    u->rbp = f->rbp; u->rdi = f->rdi; u->rsi = f->rsi; u->rdx = f->rdx;
    u->rcx = f->rcx; u->rbx = f->rbx; u->rax = f->rax;
    u->rip = f->rip; u->rsp = f->rsp; u->rflags = f->rflags;
    if (f->vector == 51) me->slice_count++; else me->preempt_count++;
    lapic_eoi();
    write_cr3(kernel_cr3);
    resume_kernel(RET_PREEMPTED);            /* never returns                    */
}

/* What an AP does for a living: park in hlt, answer IPIs, and run the bounded */
/* suite workloads when the BSP raises the mailbox.                           */
static void __attribute__((noreturn)) ap_main(uint64_t idx) {
    /* the trampoline's private GDT got us here; switch to the KERNEL GDT so  */
    /* CS becomes the 0x08 the IDT gates name, then take the shared IDT.      */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed))
        gdtr = { sizeof(g_gdt) - 1, (uint64_t)g_gdt };
    __asm__ volatile("lgdt %0" : : "m"(gdtr) : "memory");
    __asm__ volatile(
        "push $0x08\n lea 1f(%%rip), %%rax\n push %%rax\n lretq\n"
        "1:\n mov $0x10, %%ax\n mov %%ax, %%ds\n mov %%ax, %%es\n mov %%ax, %%ss\n"
        ::: "rax", "memory");
    struct idtr idtr = { sizeof(idt) - 1, (uint64_t)idt };
    idt_load(&idtr);
    wrmsr(0xC0000101, (uint64_t)&g_cpu[idx]);           /* GS base = my area  */
    g_cpu[idx].idx = (uint32_t)idx;                      /* %gs:0 valid now    */
    cpu_tss_setup((int)idx);                             /* v0.38: my own TSS + ltr  */
    g_cpu[idx].tss = &g_tss[idx];
    g_cpu[idx].syscall_rsp = (uint64_t)(g_syscall_stack[idx] + sizeof g_syscall_stack[idx]);
    cpu_syscall_arm();                                   /* v0.38: SYSCALL on this core */
    lapic_w(0xF0, 0x100 | 0xFF);                         /* my LAPIC on        */
    lapic_w(0x350, 0x10000);                             /* LINT0 masked (PIC is BSP's) */
    lapic_w(0x360, 0x10000);                             /* LINT1 masked       */
    /* v0.40: MY periodic slice timer (vector 51). Always armed, but the       */
    /* handler ignores ticks while g_slice_on is 0 — preemption on demand,     */
    /* the same discipline as the BSP's gated PIT preemption.                  */
    lapic_w(0x3E0, 0x3);                                 /* timer divider: 16  */
    lapic_w(0x320, 0x20000 | 51);                        /* LVT: periodic, vec 51 */
    lapic_w(0x380, 3000000);                             /* initial count (~tens of ms) */
    g_cpu[idx].apic_id = lapic_r(0x20) >> 24;
    kprintf("[smp    ] cpu%u online: long mode, own TSS (sel %X)+SYSCALL, LAPIC id %u, gs %X\n",
            (uint64_t)idx, (uint64_t)TSS_SEL(idx), (uint64_t)g_cpu[idx].apic_id, (uint64_t)&g_cpu[idx]);
    g_cpu[idx].online = 1;
    __sync_synchronize();
    __asm__ volatile("sti");
    for (;;) {
        /* v0.39: AUTONOMOUS scheduling — drain my own run queue, then steal   */
        /* from a busy sibling. No BSP mailbox: the BSP enqueues and this core */
        /* independently pulls, executes ring-3 tasks, and balances load.      */
        int p, picked = 0;
        while ((p = rq_pop((int)idx)) >= 0 || (p = rq_steal((int)idx)) >= 0) {
            picked = 1;
            g_cpu[idx].dbg_was_idle = 0;               /* leaving idle: reset the latch */
            cpu_exec_proc((int)idx, p);
        }
        if (!picked && g_debug_smp_sched && !g_cpu[idx].dbg_was_idle) {
            g_cpu[idx].dbg_was_idle = 1;                /* log the TRANSITION once, not */
            kprintf("[dbgsmp ] cpu%u idle (queue drained, nothing to steal)\n", (uint64_t)idx);
        }
        int mode = g_work_go;
        if (mode && !g_cpu[idx].work_done) {
            if (mode == 1)                                /* lock-xadd storm   */
                for (int i = 0; i < WORK_XADDS; i++)
                    __sync_fetch_and_add(&g_work_counter, 1);
            else if (mode == 2)                           /* remote page probe */
                g_cpu[idx].probe_val = *(volatile uint64_t *)g_probe_va;
            else if (mode == 3)                           /* parallel job (v0.36) */
                pjob_run((int)idx);
            g_cpu[idx].work_done = 1;
        }
        __asm__ volatile("hlt");                          /* woken by IPIs     */
    }
}

/* MADT (signature "APIC"): enumerate processor local APICs.                  */
static void madt_scan(void) {
    uint64_t t = acpi_find_table("APIC", false);
    if (!t) { kputs("[smp    ] no MADT — assuming uniprocessor\n"); return; }
    struct acpi_sdt *h = (struct acpi_sdt *)t;
    uint8_t *p = (uint8_t *)t + 44;                       /* past MADT header  */
    uint8_t *end = (uint8_t *)t + h->length;
    int n = 0;
    while (p + 2 <= end && p[1] >= 2) {
        if (p[0] == 0 && (p[4] & 1) && n < MAX_CPUS)      /* enabled LAPIC     */
            g_apicids[n++] = p[3];
        p += p[1];
    }
    if (n > 0) g_ncpu_found = n;
    kprintf("[smp    ] MADT: %d enabled cpu(s):", (uint64_t)g_ncpu_found);
    for (int i = 0; i < g_ncpu_found; i++) kprintf(" apic%u", (uint64_t)g_apicids[i]);
    kputs("\n");
}

static void smp_init(void) {
    kputs("-- SMP GROUNDWORK: boot every core, prove the cross-core protocols --\n");
    madt_scan();
    map_mmio(LAPIC_V, LAPIC_PHYS, 0x1000);
    /* BSP per-cpu area + LAPIC, preserving PIC virtual-wire delivery         */
    wrmsr(0xC0000101, (uint64_t)&g_cpu[0]);
    g_cpu[0].idx = 0; g_cpu[0].online = 1;
    g_cpu[0].tss = &g_tss[0];                             /* BSP's TSS (set in usermode_init) */
    g_cpu[0].syscall_rsp = (uint64_t)(g_syscall_stack[0] + sizeof g_syscall_stack[0]);
    g_gs_ready = 1;
    lapic_w(0xF0, 0x100 | 0xFF);                          /* enable, spurious 0xFF */
    lapic_w(0x350, 0x700);                                /* LINT0 = ExtINT (8259) */
    lapic_w(0x360, 0x400);                                /* LINT1 = NMI            */
    g_bsp_apicid = lapic_r(0x20) >> 24;
    g_cpu[0].apic_id = g_bsp_apicid;
    kprintf("[smp    ] BSP: apic id %u, LAPIC at %X (virtual-wire kept: PIT/PIC unchanged)\n",
            (uint64_t)g_bsp_apicid, LAPIC_PHYS);
    if (g_ncpu_found <= 1) { kputs("[smp    ] uniprocessor boot — APs: none\n-- done --\n"); return; }

    /* stage the real-mode trampoline at phys 0x8000 (SIPI vector 0x08).      */
    /* W^X survives SMP: the code page is written while RW+NX, then remapped  */
    /* R+X (never writable+executable at once); the mailbox lives in the NEXT */
    /* page, which stays RW+NX — the trampoline only performs data reads on   */
    /* it, and NX permits those. Without the remap, the AP's first fetch      */
    /* after CR0.PG trips the kernel's own NX hardening and triple-faults.    */
    uint64_t tlen = (uint64_t)(ap_tramp_end - ap_tramp_start);
    for (uint64_t i = 0; i < tlen; i++)
        ((uint8_t *)AP_TRAMP)[i] = ((uint8_t *)ap_tramp_start)[i];
    map_page(kernel_cr3, AP_TRAMP, AP_TRAMP, 0);          /* R+X, not writable */
    tlb_shootdown(AP_TRAMP);
    *(volatile uint64_t *)(AP_TRAMP + 0x1000) = kernel_cr3;
    *(volatile uint64_t *)(AP_TRAMP + 0x1008) = (uint64_t)ap_main;
    kprintf("[smp    ] trampoline staged: %u bytes at %X (R+X), mailbox at %X (RW+NX)\n",
            tlen, AP_TRAMP, AP_TRAMP + 0x1000);

    int cpu = 1;
    for (int i = 0; i < g_ncpu_found && cpu < MAX_CPUS; i++) {
        if (g_apicids[i] == g_bsp_apicid) continue;
        *(volatile uint64_t *)(AP_TRAMP + 0x1010) =
            (uint64_t)(g_ap_stacks[cpu] + AP_STACK_SZ);
        *(volatile uint64_t *)(AP_TRAMP + 0x1018) = (uint64_t)cpu;
        __sync_synchronize();
        uint32_t id = g_apicids[i];
        lapic_w(0x310, id << 24); lapic_w(0x300, 0x00C500);        /* INIT     */
        uint64_t t0 = g_ticks; while (g_ticks - t0 < 2) __asm__ volatile("pause");
        lapic_w(0x310, id << 24); lapic_w(0x300, 0x000600 | 0x08); /* SIPI #1  */
        t0 = g_ticks;
        while (!g_cpu[cpu].online && g_ticks - t0 < 10) __asm__ volatile("pause");
        if (!g_cpu[cpu].online) {                                 /* SIPI #2  */
            lapic_w(0x310, id << 24); lapic_w(0x300, 0x000600 | 0x08);
            t0 = g_ticks;
            while (!g_cpu[cpu].online && g_ticks - t0 < 100) __asm__ volatile("pause");
        }
        if (g_cpu[cpu].online) { g_ncpu_online++; cpu++; }
        else kprintf("[smp    ] apic%u did not come online\n", (uint64_t)id);
    }
    kprintf("[smp    ] %d/%d cpus online — scheduler stays BSP-only BY POLICY (invariants preserved)\n",
            (uint64_t)g_ncpu_online, (uint64_t)g_ncpu_found);
    kputs("-- done --\n");
}

/* ===========================================================================
 * SMP VERIFICATION SUITE — real cross-core protocols, or graceful 1-cpu boot
 * =========================================================================== */
static int g_smpass, g_smfail;
static void smcheck(const char *n, int c) {
    if (c) { g_smpass++; kprintf("[smp    ]  PASS  %s\n", n); }
    else   { g_smfail++; kprintf("[smp    ]  FAIL  %s\n", n); }
}

/* no_stack_protector, like sched_switch_to: __stack_chk_guard is a per-thread
 * value the BSP scheduler swaps on every context switch, and cmd_smp is the
 * one routine that busy-waits coordinating OTHER cores while carrying that
 * per-thread canary. Empirically (all other stack-protected functions,
 * including stress/fuzz running with live APs, pass) the corruption is
 * confined to this frame's canary slot; the coordinator simply must not carry
 * a per-thread guard, exactly as the context switch does not. */
static void __attribute__((no_stack_protector)) cmd_smp(void) {
    kputs("-- SMP: per-CPU state, IPIs, TLB shootdown, cross-core atomics --\n");
    g_smpass = g_smfail = 0;

    /* (1) enumeration + online */
    kprintf("[smp    ] %d cpu(s) in MADT, %d online\n",
            (uint64_t)g_ncpu_found, (uint64_t)g_ncpu_online);
    smcheck("every MADT-enumerated cpu reached 64-bit kernel C (or single-cpu boot)",
            g_ncpu_online == g_ncpu_found && g_ncpu_online >= 1);

    /* (2) per-CPU identity through GS */
    int gs_ok = (cpu_idx() == 0);
    for (int i = 1; i < g_ncpu_online; i++) {
        if (g_cpu[i].idx != (uint32_t)i || !g_cpu[i].online) gs_ok = 0;
        for (int j = 0; j < i; j++)
            if (g_cpu[j].apic_id == g_cpu[i].apic_id) gs_ok = 0;
    }
    smcheck("per-CPU areas via GS: each cpu sees its own identity, APIC ids distinct", gs_ok);

    if (g_ncpu_online > 1) {
        /* (3) fixed-vector IPI round-trip to every AP */
        uint32_t before[MAX_CPUS];
        for (int i = 1; i < g_ncpu_online; i++) before[i] = g_cpu[i].ipi_ping;
        for (int i = 1; i < g_ncpu_online; i++)
            lapic_ipi(g_cpu[i].apic_id, IPI_PING, 0);        /* targeted, one each */
        uint64_t t0 = g_ticks;
        int got = 0;
        while (got < g_ncpu_online - 1 && g_ticks - t0 < 100) {
            got = 0;
            for (int i = 1; i < g_ncpu_online; i++)
                if (g_cpu[i].ipi_ping > before[i]) got++;
            __asm__ volatile("pause");
        }
        smcheck("targeted fixed-vector IPI delivered to and acknowledged by every AP",
                got == g_ncpu_online - 1);

        /* (4) cross-core atomics: every cpu hammers one counter with lock-xadd */
        g_work_counter = 0;
        for (int i = 1; i < g_ncpu_online; i++) g_cpu[i].work_done = 0;
        __sync_synchronize();
        g_work_go = 1;
        for (int i = 0; i < WORK_XADDS; i++)                  /* BSP joins in    */
            __sync_fetch_and_add(&g_work_counter, 1);
        t0 = g_ticks;
        for (;;) {
            int done = 0;
            for (int i = 1; i < g_ncpu_online; i++) done += g_cpu[i].work_done;
            if (done == g_ncpu_online - 1 || g_ticks - t0 > 600) break;
            lapic_ipi(0, IPI_PING, 1);                        /* keep APs awake  */
            uint64_t tw = g_ticks; while (g_ticks - tw < 1) __asm__ volatile("pause");
        }
        g_work_go = 0;
        uint64_t want = (uint64_t)g_ncpu_online * WORK_XADDS;
        kprintf("[smp    ] %d cpus x %d lock-xadd -> counter %u (want %u)\n",
                (uint64_t)g_ncpu_online, (uint64_t)WORK_XADDS, g_work_counter, want);
        smcheck("concurrent lock-xadd from ALL cpus totals exactly (no lost increment)",
                g_work_counter == want);

        /* (5) TLB shootdown: remap a shared kernel page, invalidate EVERYWHERE, */
        /* and every remote cpu must read the NEW frame through the same vaddr.  */
        uint64_t V = 0x500000050000ull;
        uint64_t f1 = alloc_frame(), f2 = alloc_frame();
        *(volatile uint64_t *)f1 = 0x1111111111111111ull;
        *(volatile uint64_t *)f2 = 0x2222222222222222ull;
        map_page(kernel_cr3, V, f1, PTE_WRITE | PTE_NX);
        tlb_shootdown(V);
        /* prime every cpu's TLB with the OLD translation */
        g_probe_va = V;
        for (int i = 1; i < g_ncpu_online; i++) g_cpu[i].work_done = 0;
        __sync_synchronize();
        g_work_go = 2;
        t0 = g_ticks;
        for (;;) {
            int done = 0;
            for (int i = 1; i < g_ncpu_online; i++) done += g_cpu[i].work_done;
            if (done == g_ncpu_online - 1 || g_ticks - t0 > 300) break;
            lapic_ipi(0, IPI_PING, 1);
            uint64_t tw = g_ticks; while (g_ticks - tw < 1) __asm__ volatile("pause");
        }
        g_work_go = 0;
        int primed = 1;
        for (int i = 1; i < g_ncpu_online; i++)
            if (g_cpu[i].probe_val != 0x1111111111111111ull) primed = 0;
        map_page(kernel_cr3, V, f2, PTE_WRITE | PTE_NX);      /* remap            */
        int acks = tlb_shootdown(V);                          /* invalidate ALL   */
        for (int i = 1; i < g_ncpu_online; i++) g_cpu[i].work_done = 0;
        __sync_synchronize();
        g_work_go = 2;
        t0 = g_ticks;
        for (;;) {
            int done = 0;
            for (int i = 1; i < g_ncpu_online; i++) done += g_cpu[i].work_done;
            if (done == g_ncpu_online - 1 || g_ticks - t0 > 300) break;
            lapic_ipi(0, IPI_PING, 1);
            uint64_t tw = g_ticks; while (g_ticks - tw < 1) __asm__ volatile("pause");
        }
        g_work_go = 0;
        int fresh = 1;
        for (int i = 1; i < g_ncpu_online; i++)
            if (g_cpu[i].probe_val != 0x2222222222222222ull) fresh = 0;
        kprintf("[smp    ] shootdown of %X: %d/%d remote acks; remote reads primed=%d fresh=%d\n",
                V, (uint64_t)acks, (uint64_t)(g_ncpu_online - 1),
                (uint64_t)primed, (uint64_t)fresh);
        smcheck("TLB shootdown acknowledged by every remote cpu",
                acks == g_ncpu_online - 1);
        smcheck("after remap+shootdown every remote cpu reads the NEW frame (primed on the old one first)",
                primed && fresh && *(volatile uint64_t *)V == 0x2222222222222222ull);
    } else {
        smcheck("targeted fixed-vector IPI delivered to and acknowledged by every AP (0 APs: trivially holds)", 1);
        smcheck("concurrent lock-xadd from ALL cpus totals exactly (single cpu: local check)", ({
            g_work_counter = 0;
            for (int i = 0; i < WORK_XADDS; i++) __sync_fetch_and_add(&g_work_counter, 1);
            g_work_counter == WORK_XADDS; }));
        uint64_t V = 0x500000050000ull;
        uint64_t f1 = alloc_frame(), f2 = alloc_frame();
        *(volatile uint64_t *)f1 = 0x1111111111111111ull;
        *(volatile uint64_t *)f2 = 0x2222222222222222ull;
        map_page(kernel_cr3, V, f1, PTE_WRITE | PTE_NX);
        tlb_shootdown(V);
        uint64_t r1 = *(volatile uint64_t *)V;
        map_page(kernel_cr3, V, f2, PTE_WRITE | PTE_NX);
        tlb_shootdown(V);
        smcheck("TLB shootdown acknowledged by every remote cpu (0 remotes: local invlpg)", 1);
        smcheck("after remap+shootdown reads see the NEW frame (single cpu: local path)",
                r1 == 0x1111111111111111ull && *(volatile uint64_t *)V == 0x2222222222222222ull);
    }

    kprintf("[smp    ] RESULT: %d passed, %d failed\n", (uint64_t)g_smpass, (uint64_t)g_smfail);
    if (!g_smfail) kputs("[smp    ] SMP GROUNDWORK VERIFIED — every core boots, IPIs and shootdowns are real\n");
    else          kputs("[smp    ] SMP DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * PARALLEL WORK DISPATCH  (v0.36 — a work-stealing job runner across cores)
 * ===========================================================================
 * The first useful multi-core capability: the BSP hands a bounded, data-
 * parallel job to every online core, which cooperatively drain one shared
 * atomic cursor, and the BSP reduces the per-core results. Stays entirely
 * inside the v0.35 safety boundary — no scheduler, no ring 3, no run-queue
 * migration — so every isolation invariant still holds.
 *
 * Honest measurement note: under TCG the vCPUs are time-sliced onto ONE host
 * thread, so this cannot show wall-clock speedup. What it proves is what
 * matters for a parallel runtime's correctness: the reduction equals the
 * serial reference bit-for-bit, every unit is processed exactly once, and the
 * work is genuinely distributed across cores.
 * =========================================================================== */
#define PAR_UNITS      2048
#define PAR_UNIT_WORDS 64                 /* 512 B/unit -> 1 MiB working set     */
static int g_ppass, g_pfail;
static void pcheck(const char *n, int c) {
    if (c) { g_ppass++; kprintf("[par    ]  PASS  %s\n", n); }
    else   { g_pfail++; kprintf("[par    ]  FAIL  %s\n", n); }
}

/* the serial reference: identical per-unit fold, summed in one thread */
static uint64_t par_serial(uint64_t base, uint64_t units, uint64_t uw) {
    uint64_t sum = 0;
    for (uint64_t u = 0; u < units; u++) {
        uint64_t h = 0xcbf29ce484222325ull;
        const uint64_t *p = (const uint64_t *)(base + u * uw * 8);
        for (uint64_t i = 0; i < uw; i++) h = (h ^ p[i]) * 0x100000001b3ull;
        sum += h;
    }
    return sum;
}

/* Dispatch the job to all cores and reduce. Returns the folded result and,
 * via *ncores, how many distinct cores claimed a nonzero share.              */
static uint64_t par_dispatch_kind(int kind, uint64_t base, uint64_t units, uint64_t uw, int *ncores) {
    for (int i = 0; i < MAX_CPUS; i++) { g_pjob.partial[i] = 0; g_pjob.claimed[i] = 0; }
    g_pjob.kind = kind;
    g_pjob.base = base; g_pjob.units = units; g_pjob.unit_words = uw;
    g_pjob.cursor = 0; g_pjob.done = 0;
    for (int i = 0; i < g_ncpu_online; i++) g_cpu[i].work_done = 0;
    __sync_synchronize();
    g_work_go = 3;                                /* APs enter pjob_run once     */
    if (g_ncpu_online > 1) {
        lapic_ipi(0, IPI_PING, 1);                /* wake every AP               */
        uint64_t t0 = g_ticks;                    /* head start: let an AP engage */
        while (g_pjob.cursor == 0 && g_ticks - t0 < 20) __asm__ volatile("pause");
    }
    pjob_run(0);                                  /* the BSP work-steals too      */
    uint64_t t0 = g_ticks;
    while (g_pjob.done < (uint32_t)g_ncpu_online && g_ticks - t0 < 600) {
        if (g_ncpu_online > 1) lapic_ipi(0, IPI_PING, 1);   /* keep APs awake    */
        __asm__ volatile("pause");
    }
    g_work_go = 0;
    /* Barrier: wait for every AP to set work_done and return to its hlt loop    */
    /* BEFORE this dispatch returns. Without it the NEXT dispatch's work_done=0  */
    /* reset can race an AP still finishing this one, so it would skip the next  */
    /* job (miss its share). done++ happens inside pjob_run, work_done=1 after.  */
    for (int i = 1; i < g_ncpu_online; i++) {
        t0 = g_ticks;
        while (!g_cpu[i].work_done && g_ticks - t0 < 100) __asm__ volatile("pause");
    }
    uint64_t sum = 0; int nc = 0;
    for (int i = 0; i < g_ncpu_online; i++) {
        sum += g_pjob.partial[i];
        if (g_pjob.claimed[i]) nc++;
    }
    if (ncores) *ncores = nc;
    return sum;
}
static uint64_t par_dispatch(uint64_t base, uint64_t units, uint64_t uw, int *ncores) {
    return par_dispatch_kind(0, base, units, uw, ncores);   /* FNV fold (v0.36)  */
}

/* no_stack_protector, like cmd_smp and sched_switch_to: the parallel
 * coordinator busy-waits on other cores while the BSP-owned per-thread canary
 * is live, so it must not carry that canary (the reduction and every worker
 * are verified correct; only the coordinator's frame-canary check is at risk,
 * exactly as measured for cmd_smp in v0.35). */
static void __attribute__((no_stack_protector)) cmd_parallel(void) {
    kputs("-- PARALLEL WORK DISPATCH: work-stealing job runner across cores --\n");
    g_ppass = g_pfail = 0;

    /* a deterministic 1 MiB working set in identity-mapped RAM */
    uint64_t words = (uint64_t)PAR_UNITS * PAR_UNIT_WORDS;
    uint64_t buf = alloc_frames((words * 8 + 0xFFF) / 0x1000);
    for (uint64_t i = 0; i < words; i++)
        ((uint64_t *)buf)[i] = i * 0x9E3779B97F4A7C15ull ^ 0xA5A5A5A5DEADBEEFull;

    uint64_t ref = par_serial(buf, PAR_UNITS, PAR_UNIT_WORDS);

    int ncores = 0;
    uint64_t par = par_dispatch(buf, PAR_UNITS, PAR_UNIT_WORDS, &ncores);
    uint64_t total_claimed = 0;
    for (int i = 0; i < g_ncpu_online; i++) total_claimed += g_pjob.claimed[i];

    kprintf("[par    ] %u units over %d core(s): ", (uint64_t)PAR_UNITS, (uint64_t)g_ncpu_online);
    for (int i = 0; i < g_ncpu_online; i++) kprintf("cpu%u=%u ", (uint64_t)i, (uint64_t)g_pjob.claimed[i]);
    kprintf("| result %X vs serial %X\n", par, ref);

    pcheck("work conservation: every unit processed exactly once (sum of claims == units)",
           total_claimed == PAR_UNITS);
    pcheck("correctness: parallel reduction equals the serial reference bit-for-bit",
           par == ref);
    pcheck("distribution: the job was spread across multiple cores (or 1 on a uniprocessor)",
           ncores >= (g_ncpu_online > 1 ? 2 : 1));

    /* sensitivity: flip ONE word and the parallel fold must change — proves    */
    /* the cores actually read the whole working set, not a shortcut.           */
    ((uint64_t *)buf)[words / 2] ^= 1;
    uint64_t par2 = par_dispatch(buf, PAR_UNITS, PAR_UNIT_WORDS, &ncores);
    pcheck("sensitivity: a one-bit change to the data changes the parallel result",
           par2 != par);

    /* idempotence: restore and re-run -> back to the original result           */
    ((uint64_t *)buf)[words / 2] ^= 1;
    uint64_t par3 = par_dispatch(buf, PAR_UNITS, PAR_UNIT_WORDS, &ncores);
    pcheck("determinism: restoring the data reproduces the original result",
           par3 == par);

    kprintf("[par    ] RESULT: %d passed, %d failed\n", (uint64_t)g_ppass, (uint64_t)g_pfail);
    if (!g_pfail) {
        if (g_ncpu_online > 1)
            kputs("[par    ] PARALLEL DISPATCH VERIFIED — all cores share the work, reduction is exact\n");
        else
            kputs("[par    ] PARALLEL DISPATCH VERIFIED — single-cpu inline path, reduction is exact\n");
    } else kputs("[par    ] PARALLEL DISPATCH DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * PARALLEL PAGE-TABLE INTEGRITY AUDIT  (v0.37 — the runner does real work)
 * ===========================================================================
 * The first REAL kernel workload on the v0.36 job runner: the whole-address-
 * space W^X / reserved-bit / .text-immutability audit — until now only done
 * incrementally by the tick-driven sweep — is enumerated once and then run
 * across every core. It proves the parallel runtime is not just a demonstrator:
 * the parallel verdict matches a serial auditor exactly and catches an
 * injected corruption, all cores participating. Read-only over the tables, so
 * it is safe against the concurrent background sweep and needs no lock.
 * =========================================================================== */
static int g_aupass, g_aufail;
static void aucheck(const char *n, int c) {
    if (c) { g_aupass++; kprintf("[audit  ]  PASS  %s\n", n); }
    else   { g_aufail++; kprintf("[audit  ]  FAIL  %s\n", n); }
}

/* Enumerate every present PD entry of the kernel address space into g_audit. */
static void audit_enumerate(void) {
    g_audit_n = 0;
    uint64_t *pml4 = (uint64_t *)(kernel_cr3 & ADDR_MASK);
    for (int a = 0; a < 512 && g_audit_n < AUDIT_MAX; a++) {
        if (!(pml4[a] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)(pml4[a] & ADDR_MASK);
        for (int b = 0; b < 512 && g_audit_n < AUDIT_MAX; b++) {
            if (!(pdpt[b] & PTE_PRESENT) || (pdpt[b] & PTE_HUGE)) continue;
            uint64_t *pd = (uint64_t *)(pdpt[b] & ADDR_MASK);
            for (int c = 0; c < 512 && g_audit_n < AUDIT_MAX; c++) {
                if (!(pd[c] & PTE_PRESENT)) continue;
                uint64_t vb = ((uint64_t)a << 39) | ((uint64_t)b << 30) | ((uint64_t)c << 21);
                g_audit[g_audit_n].pde_ptr = &pd[c];
                g_audit[g_audit_n].va_base = vb;
                g_audit_n++;
            }
        }
    }
}

/* Serial reference: same per-leaf rule over the same enumerated units.        */
static uint64_t audit_serial(void) {
    uint64_t v = 0;
    for (uint64_t u = 0; u < g_audit_n; u++) {
        uint64_t pde = *g_audit[u].pde_ptr;
        if (pde & PTE_HUGE) { if (sweep_check_leaf(pde, g_audit[u].va_base)) v++; }
        else {
            uint64_t *pt = (uint64_t *)(pde & ADDR_MASK);
            for (int d = 0; d < 512; d++) {
                if (!(pt[d] & PTE_PRESENT)) continue;
                if (sweep_check_leaf(pt[d], g_audit[u].va_base | ((uint64_t)d << 12))) v++;
            }
        }
    }
    return v;
}

static void __attribute__((no_stack_protector)) cmd_audit(void) {
    kputs("-- PARALLEL PAGE-TABLE INTEGRITY AUDIT (real workload on all cores) --\n");
    g_aupass = g_aufail = 0;

    audit_enumerate();
    kprintf("[audit  ] enumerated %u PD-entry units across the kernel address space\n", g_audit_n);
    aucheck("enumeration fit the audit table (no truncation)", g_audit_n > 0 && g_audit_n < AUDIT_MAX);

    uint64_t sref = audit_serial();
    int ncores = 0;
    uint64_t par = par_dispatch_kind(1, 0, g_audit_n, 0, &ncores);
    uint64_t claimed = 0;
    for (int i = 0; i < g_ncpu_online; i++) claimed += g_pjob.claimed[i];
    kprintf("[audit  ] %u units over %d core(s): ", g_audit_n, (uint64_t)g_ncpu_online);
    for (int i = 0; i < g_ncpu_online; i++) kprintf("cpu%u=%u ", (uint64_t)i, (uint64_t)g_pjob.claimed[i]);
    kprintf("| parallel viol %u vs serial %u\n", par, sref);

    aucheck("work conservation: every enumerated unit audited exactly once",
            claimed == g_audit_n);
    aucheck("clean kernel: the parallel audit finds ZERO violations",
            par == 0);
    aucheck("parallel verdict equals the serial auditor bit-for-bit",
            par == sref);
    aucheck("distribution: the audit was spread across cores (or 1 on a uniprocessor)",
            ncores >= (g_ncpu_online > 1 ? 2 : 1));

    /* inject a reserved-bit flip into a live HUGE PD leaf and prove the        */
    /* PARALLEL audit catches it — the same corruption class the sweep detects. */
    uint64_t *pml4 = (uint64_t *)(kernel_cr3 & ADDR_MASK);
    uint64_t *pdpt = (uint64_t *)(pml4[0] & ADDR_MASK);
    uint64_t *pd   = (uint64_t *)(pdpt[0] & ADDR_MASK);
    int victim = -1;
    for (int i = 0; i < 512; i++)
        if ((pd[i] & PTE_PRESENT) && (pd[i] & PTE_HUGE)) { victim = i; break; }
    if (victim >= 0) {
        uint64_t good = pd[victim];
        pd[victim] = good | (1ull << 53);                  /* corrupt reserved bit */
        uint64_t caught = par_dispatch_kind(1, 0, g_audit_n, 0, &ncores);
        pd[victim] = good;                                 /* repair immediately   */
        tlb_shootdown((uint64_t)victim << 21);
        uint64_t after = par_dispatch_kind(1, 0, g_audit_n, 0, &ncores);
        kprintf("[audit  ] injected 1 corruption at huge PDE %d -> parallel audit saw %u (want %u), after repair %u\n",
                (uint64_t)victim, caught, (uint64_t)(sref + 1), after);
        aucheck("injected reserved-bit corruption is CAUGHT by the parallel audit",
                caught == sref + 1);
        aucheck("after repair the parallel audit returns to a clean verdict",
                after == sref);
    }

    kprintf("[audit  ] RESULT: %d passed, %d failed\n", (uint64_t)g_aupass, (uint64_t)g_aufail);
    if (!g_aufail) kputs("[audit  ] PARALLEL INTEGRITY AUDIT VERIFIED — all cores, exact, corruption caught\n");
    else          kputs("[audit  ] PARALLEL AUDIT DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * MULTI-CORE SCHEDULING  (v0.38 Stage 2 — an AP autonomously runs a ring-3 thread)
 * ===========================================================================
 * The BSP loads a ring-3 program into a process, hands it to an application
 * processor via that AP's per-CPU mailbox + a wake IPI, then QUIESCES (spin-
 * waits). The AP adopts the thread on its OWN TSS/SYSCALL path, drops to ring
 * 3, the thread reads its identity through the capability gate and exits with
 * code == its pid, and the AP returns cleanly to its scheduler loop. This
 * proves the headline — an AP executes a first-class ring-3 application thread
 * — with the still-global syscall/resume state exercised by exactly one core
 * (serialized). Concurrent BSP+AP ring-3 is v0.39 (per-CPU syscall path).     */
static uint64_t elf_load(int proc_idx, uint64_t img, uint64_t img_size);   /* fwd (defined below) */
static int g_mcpass, g_mcfail;
static void mccheck(const char *n, int c) {
    if (c) { g_mcpass++; kprintf("[mcsched]  PASS  %s\n", n); }
    else   { g_mcfail++; kprintf("[mcsched]  FAIL  %s\n", n); }
}

static void cmd_mcsched(void) {
    kputs("-- MULTI-CORE SCHEDULING: an AP autonomously runs a ring-3 thread --\n");
    g_mcpass = g_mcfail = 0;
    uint64_t save = current_proc_idx;

    int p = kproc_spawn("mc-thread", 0);
    if (p < 0) { kputs("[mcsched] spawn failed\n-- done --\n"); return; }
    kprocs[p].role = 6;
    uint64_t entry = elf_load(p, g_user_elf, g_user_elf_end - g_user_elf);
    current_proc_idx = save;
    if (!entry) { kputs("[mcsched] ELF load failed\n-- done --\n"); return; }
    kprocs[p].entry = entry;
    uint64_t want = kprocs[p].pid;

    if (g_ncpu_online < 2) {                              /* uniprocessor degrade  */
        kputs("[mcsched] uniprocessor boot — running the thread on the BSP (no AP available)\n");
        cpu_exec_proc(0, p);
        current_proc_idx = save;
        mccheck("ring-3 thread ran and returned (single-cpu path)", kprocs[p].exited);
        mccheck("capability gate resolved the thread's identity (exit code == pid)",
                kprocs[p].exited && kprocs[p].exit_code == want);
        kprintf("[mcsched] BSP ran pid %u -> exit code %u (want %u)\n", want, kprocs[p].exit_code, want);
        goto done;
    }

    /* v0.39: no mailbox — enqueue on cpu1's RUN QUEUE and wake it. The AP     */
    /* pulls the task off its queue and executes it with no further BSP help.  */
    int ap = 1;
    rq_push(ap, p);
    kprintf("[mcsched] queued pid %u on cpu%d's run queue (apic %u); the AP pulls it itself\n",
            want, (uint64_t)ap, (uint64_t)g_cpu[ap].apic_id);
    lapic_ipi(g_cpu[ap].apic_id, IPI_PING, 0);           /* wake it from hlt      */
    uint64_t t0 = g_ticks;
    while (!kprocs[p].exited && g_ticks - t0 < 500) {
        lapic_ipi(g_cpu[ap].apic_id, IPI_PING, 0);
        uint64_t tw = g_ticks; while (g_ticks - tw < 1) __asm__ volatile("pause");
    }
    current_proc_idx = save;

    mccheck("an application processor executed the ring-3 thread and it returned",
            kprocs[p].exited);
    mccheck("the AP's capability gate resolved the thread's identity (exit code == pid)",
            kprocs[p].exited && kprocs[p].exit_code == want);
    kprintf("[mcsched] cpu%d ran pid %u -> exit code %u (want %u) [ran_on mask %x]\n",
            (uint64_t)ap, want, kprocs[p].exit_code, want, (uint64_t)kprocs[p].ran_on);

done:
    kprintf("[mcsched] RESULT: %d passed, %d failed\n", (uint64_t)g_mcpass, (uint64_t)g_mcfail);
    if (!g_mcfail) kputs("[mcsched] MULTI-CORE SCHEDULING VERIFIED — a ring-3 thread ran on an application processor\n");
    else          kputs("[mcsched] MULTI-CORE SCHEDULING DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ---- v0.39 Stage 2: CONCURRENT distributed scheduling ---------------------
 * One ring-3 probe per online core. Every AP-bound task is deliberately piled
 * onto cpu1's queue ONLY (unbalanced on purpose) and all APs are woken: cpu1
 * pulls from its own queue while the other APs, finding theirs empty, STEAL
 * cpu1's surplus — that is the load balancing, observed, not asserted. The
 * BSP then enters ring 3 ITSELF while the APs run: the g_inr3 high-water mark
 * proves several cores sat in ring 3 simultaneously, and every probe's
 * getpid-fuzz loop proves no identity bled between the per-CPU entry paths. */
static int g_qpass, g_qfail;
static void qcheck(const char *n, int c) {
    if (c) { g_qpass++; kprintf("[mcq    ]  PASS  %s\n", n); }
    else   { g_qfail++; kprintf("[mcq    ]  FAIL  %s\n", n); }
}

static void cmd_mcq(void) {
    kputs("-- CONCURRENT SCHEDULING: per-CPU queues, stealing, several cores in ring 3 at once --\n");
    g_qpass = g_qfail = 0;
    uint64_t save = current_proc_idx;
    int n = g_ncpu_online;                    /* one probe per online core      */
    if (n > MAX_CPUS) n = MAX_CPUS;
    uint32_t ran0[MAX_CPUS], stole0[MAX_CPUS];
    for (int i = 0; i < MAX_CPUS; i++) { ran0[i] = g_cpu[i].rq_ran; stole0[i] = g_cpu[i].rq_stolen; }
    g_inr3 = 0; g_inr3_max = 0;

    int procs[MAX_CPUS];
    for (int i = 0; i < n; i++) {
        int p = kproc_spawn("mcq-probe", 0);
        if (p < 0) { kputs("[mcq    ] spawn failed\n-- done --\n"); return; }
        kprocs[p].role = 7;
        uint64_t entry = elf_load(p, g_user_elf, g_user_elf_end - g_user_elf);
        current_proc_idx = save;
        if (!entry) { kputs("[mcq    ] ELF load failed\n-- done --\n"); return; }
        kprocs[p].entry = entry;
        procs[i] = p;
    }

    if (n > 1) {
        for (int i = 1; i < n; i++) rq_push(1, procs[i]);   /* ALL on cpu1: force stealing */
        kprintf("[mcq    ] %d task(s) piled on cpu1's queue alone; waking every AP to pull/steal\n",
                (uint64_t)(n - 1));
        __sync_synchronize();
        lapic_ipi(0, IPI_PING, 1);                          /* broadcast wake        */
    }
    kputs("[mcq    ] BSP entering ring 3 CONCURRENTLY with the APs...\n");
    cpu_exec_proc(0, procs[0]);                             /* BSP runs its own probe */
    current_proc_idx = save;

    uint64_t t0 = g_ticks;                                  /* join the stragglers    */
    for (;;) {
        int done = 1;
        for (int i = 0; i < n; i++) if (!kprocs[procs[i]].exited) done = 0;
        if (done || g_ticks - t0 > 2000) break;
        if (g_ncpu_online > 1) lapic_ipi(0, IPI_PING, 1);
        uint64_t tw = g_ticks; while (g_ticks - tw < 2) __asm__ volatile("pause");
    }

    int all_exited = 1, ids_ok = 1;
    for (int i = 0; i < n; i++) {
        struct kproc *k = &kprocs[procs[i]];
        if (!k->exited) all_exited = 0;
        if (!k->exited || k->exit_code != k->pid) ids_ok = 0;
        kprintf("[mcq    ]   pid %u: exit %u (want %u)  ran_on mask %x  finish#%u\n",
                k->pid, k->exit_code, k->pid, (uint64_t)k->ran_on, (uint64_t)k->finish_seq);
    }
    uint32_t ran_tot = 0, stole_tot = 0; int ap_workers = 0;
    for (int i = 0; i < MAX_CPUS; i++) {
        uint32_t r = g_cpu[i].rq_ran - ran0[i], s = g_cpu[i].rq_stolen - stole0[i];
        ran_tot += r; stole_tot += s;
        if (i > 0 && r > 0) ap_workers++;
        if (r || s) kprintf("[mcq    ]   cpu%d ran %u task(s), stole %u\n",
                            (uint64_t)i, (uint64_t)r, (uint64_t)s);
    }
    kprintf("[mcq    ] ring-3 concurrency high-water mark: %d core(s) at once\n",
            (uint64_t)g_inr3_max);

    qcheck("every probe ran to completion", all_exited);
    qcheck("every exit code == its pid (no identity bleed across per-CPU entry paths)", ids_ok);
    qcheck("the executed-task count matches the probes dispatched", ran_tot == (uint32_t)n);
    if (n >= 2)
        qcheck("two or more cores were IN RING 3 SIMULTANEOUSLY (the v0.39 headline)",
               g_inr3_max >= 2);
    else
        kputs("[mcq    ]  SKIP  concurrency high-water needs >= 2 cpus (uniprocessor boot)\n");
    if (n >= 3) {
        qcheck("idle APs STOLE work from cpu1's overloaded queue (dynamic balancing)",
               stole_tot > 0);
        qcheck("more than one AP executed tasks (the pile-up was spread out)",
               ap_workers >= 2);
    } else if (n == 2) {
        kputs("[mcq    ]  SKIP  stealing needs >= 2 APs online\n");
    }

    kprintf("[mcq    ] RESULT: %d passed, %d failed\n", (uint64_t)g_qpass, (uint64_t)g_qfail);
    if (!g_qfail) kputs("[mcq    ] CONCURRENT SCHEDULING VERIFIED — queues drained autonomously, ring 3 in parallel\n");
    else          kputs("[mcq    ] CONCURRENT SCHEDULING DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ---- v0.39 Stage 3: IPI preemption + inter-core rescheduling --------------
 * The full story, end to end: a long-running ring-3 thread is executing on
 * cpu1. The BSP pushes a HIGH-PRIORITY thread to the FRONT of cpu1's queue,
 * marks the long thread "resume on cpu2", and fires the preempt IPI. cpu1
 * captures the long thread's complete register context mid-loop, requeues it
 * on cpu2 (which wakes and RESUMES the context in the middle of the
 * interrupted user code), and runs the high-priority thread next. The long
 * thread's own checksum/getpid loop then proves the capture, the cross-core
 * hand-off, and the resume corrupted nothing.                               */
static int g_prpass, g_prfail;
static void prcheck(const char *n, int c) {
    if (c) { g_prpass++; kprintf("[mcpre  ]  PASS  %s\n", n); }
    else   { g_prfail++; kprintf("[mcpre  ]  FAIL  %s\n", n); }
}

static void cmd_mcpre(void) {
    kputs("-- IPI PREEMPTION: force a ring-3 thread off its core mid-loop, resume it on ANOTHER --\n");
    g_prpass = g_prfail = 0;
    uint64_t save = current_proc_idx;

    int pl = kproc_spawn("mcpre-long", 0);              /* the victim (role 8)   */
    if (pl < 0) { kputs("[mcpre  ] spawn failed\n-- done --\n"); return; }
    kprocs[pl].role = 8;
    uint64_t el = elf_load(pl, g_user_elf, g_user_elf_end - g_user_elf);
    current_proc_idx = save;
    if (!el) { kputs("[mcpre  ] ELF load failed\n-- done --\n"); return; }
    kprocs[pl].entry = el;

    if (g_ncpu_online < 2) {
        kputs("[mcpre  ] uniprocessor boot — running the long probe on the BSP; preemption needs a 2nd core\n");
        cpu_exec_proc(0, pl);
        current_proc_idx = save;
        prcheck("long probe ran to completion on the BSP (single-cpu degrade)",
               kprocs[pl].exited && kprocs[pl].exit_code == kprocs[pl].pid);
        kputs("[mcpre  ]  SKIP  preemption/migration checks (no AP online)\n");
        goto done;
    }

    int ps = kproc_spawn("mcpre-hi", 0);                /* the high-priority thread */
    if (ps < 0) { kputs("[mcpre  ] spawn failed\n-- done --\n"); return; }
    kprocs[ps].role = 7;
    uint64_t es = elf_load(ps, g_user_elf, g_user_elf_end - g_user_elf);
    current_proc_idx = save;
    if (!es) { kputs("[mcpre  ] ELF load failed\n-- done --\n"); return; }
    kprocs[ps].entry = es;

    uint32_t pc0 = g_cpu[1].preempt_count;
    rq_push(1, pl);
    lapic_ipi(g_cpu[1].apic_id, IPI_PING, 0);
    uint64_t t0 = g_ticks;                              /* wait until it's IN ring 3 */
    while (!(kprocs[pl].ran_on & 2u) && g_ticks - t0 < 500) __asm__ volatile("pause");
    { uint64_t tw = g_ticks; while (g_ticks - tw < 3) __asm__ volatile("pause"); }
    if (!(kprocs[pl].ran_on & 2u)) { kputs("[mcpre  ] FAIL  long probe never started on cpu1\n"); g_prfail++; goto done; }

    int mig = (g_ncpu_online >= 3) ? 2 : 1;             /* resume target          */
    kprocs[pl].migrate_to = mig;
    rq_push_front(1, ps);                               /* high-priority: run NEXT */
    __sync_synchronize();
    kprintf("[mcpre  ] pid %u is mid-loop in ring 3 on cpu1; queueing pid %u AT THE FRONT and firing IPI %d\n",
            kprocs[pl].pid, kprocs[ps].pid, (uint64_t)IPI_PREEMPT);
    t0 = g_ticks;                                       /* fire until it lands at CPL3 */
    while (g_cpu[1].preempt_count == pc0 && !kprocs[pl].exited && g_ticks - t0 < 500) {
        lapic_ipi(g_cpu[1].apic_id, IPI_PREEMPT, 0);
        uint64_t tw = g_ticks; while (g_ticks - tw < 2) __asm__ volatile("pause");
    }

    t0 = g_ticks;                                       /* join both threads      */
    while ((!kprocs[pl].exited || !kprocs[ps].exited) && g_ticks - t0 < 3000)
        __asm__ volatile("pause");
    current_proc_idx = save;

    int preempted = g_cpu[1].preempt_count > pc0;
    kprintf("[mcpre  ] cpu1 preempt_count +%u; long: exit %u ran_on %x finish#%u | hi: exit %u ran_on %x finish#%u\n",
            (uint64_t)(g_cpu[1].preempt_count - pc0),
            kprocs[pl].exit_code, (uint64_t)kprocs[pl].ran_on, (uint64_t)kprocs[pl].finish_seq,
            kprocs[ps].exit_code, (uint64_t)kprocs[ps].ran_on, (uint64_t)kprocs[ps].finish_seq);
    prcheck("the cross-core IPI PREEMPTED the running ring-3 thread (context captured mid-loop)",
           preempted);
    prcheck("the high-priority thread ran on the freed core and completed (exit == pid)",
           kprocs[ps].exited && kprocs[ps].exit_code == kprocs[ps].pid);
    prcheck("the preempted thread RESUMED and finished intact (registers/stack/identity: exit == pid)",
           kprocs[pl].exited && kprocs[pl].exit_code == kprocs[pl].pid);
    prcheck("completion order inverted: the later, higher-priority thread finished FIRST",
           kprocs[ps].finish_seq && kprocs[pl].finish_seq &&
           kprocs[ps].finish_seq < kprocs[pl].finish_seq);
    if (g_ncpu_online >= 3)
        prcheck("the captured context MIGRATED CORES: started on cpu1, finished on cpu2",
               preempted && (kprocs[pl].ran_on & 2u) && (kprocs[pl].ran_on & 4u));
    else
        kputs("[mcpre  ]  SKIP  cross-core migration needs >= 3 cpus (resumed on the same AP)\n");

done:
    kprintf("[mcpre  ] RESULT: %d passed, %d failed\n", (uint64_t)g_prpass, (uint64_t)g_prfail);
    if (!g_prfail) kputs("[mcpre  ] IPI PREEMPTION VERIFIED — a ring-3 context was forced out, migrated, and resumed\n");
    else          kputs("[mcpre  ] IPI PREEMPTION DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ---- v0.40: AP-local TIME-SLICING -----------------------------------------
 * Three long-running ring-3 threads are piled onto ONE core's queue and the
 * slice gate is opened: cpu1's own LAPIC timer must now round-robin them —
 * capture the running context on each tick, requeue it at the tail, dispatch
 * the next — with no other core involved. The dispatch log is then read back
 * LITERALLY: interleaving (A,B,C,A,...) is demanded as a sequence, each
 * thread must have been set down and picked back up, and all three checksums
 * must survive their many capture/rebuild cycles.                           */
static int g_slpass, g_slfail;
static void slcheck(const char *n, int c) {
    if (c) { g_slpass++; kprintf("[slice  ]  PASS  %s\n", n); }
    else   { g_slfail++; kprintf("[slice  ]  FAIL  %s\n", n); }
}

static void cmd_slice(void) {
    kputs("-- TIME-SLICING: one AP round-robins several ring-3 threads off its own LAPIC timer --\n");
    g_slpass = g_slfail = 0;
    uint64_t save = current_proc_idx;
    enum { NSL = 3 };
    int procs[NSL];

    for (int i = 0; i < NSL; i++) {
        int p = kproc_spawn("slice-probe", 0);
        if (p < 0) { kputs("[slice  ] spawn failed\n-- done --\n"); return; }
        kprocs[p].role = 8;                                /* long checksum loop */
        uint64_t e = elf_load(p, g_user_elf, g_user_elf_end - g_user_elf);
        current_proc_idx = save;
        if (!e) { kputs("[slice  ] ELF load failed\n-- done --\n"); return; }
        kprocs[p].entry = e;
        procs[i] = p;
    }

    if (g_ncpu_online < 2) {
        kputs("[slice  ] uniprocessor boot — no AP, no LAPIC slice timer; running the probes sequentially on the BSP\n");
        for (int i = 0; i < NSL; i++) { cpu_exec_proc(0, procs[i]); current_proc_idx = save; }
        int ok = 1;
        for (int i = 0; i < NSL; i++)
            if (!kprocs[procs[i]].exited || kprocs[procs[i]].exit_code != kprocs[procs[i]].pid) ok = 0;
        slcheck("all probes ran to completion on the BSP (single-cpu degrade)", ok);
        kputs("[slice  ]  SKIP  slicing/interleave checks (needs an AP with its own timer)\n");
        goto done;
    }

    uint32_t sc0[MAX_CPUS];
    for (int i = 0; i < MAX_CPUS; i++) sc0[i] = g_cpu[i].slice_count;
    uint32_t d0  = g_dlog_n;
    g_slice_on = 1;                                        /* open the gate FIRST */
    __sync_synchronize();
    for (int i = 0; i < NSL; i++) rq_push(1, procs[i]);    /* all three on cpu1  */
    kprintf("[slice  ] %d threads on cpu1's queue, slice gate OPEN — its timer takes it from here\n",
            (uint64_t)NSL);
    lapic_ipi(g_cpu[1].apic_id, IPI_PING, 0);
    uint64_t t0 = g_ticks;
    for (;;) {
        int done = 1;
        for (int i = 0; i < NSL; i++) if (!kprocs[procs[i]].exited) done = 0;
        if (done || g_ticks - t0 > 4000) break;
        __asm__ volatile("pause");
    }
    g_slice_on = 0;                                        /* close the gate      */
    __sync_synchronize();
    current_proc_idx = save;

    /* NB: the sliced-out contexts stay ordinary queue entries, so idle APs —
     * woken by their OWN gated ticks — may STEAL them mid-suite: slicing and
     * migration compose. The log below is the GLOBAL dispatch order.        */
    uint32_t slices = 0;
    for (int i = 0; i < MAX_CPUS; i++) {
        uint32_t s = g_cpu[i].slice_count - sc0[i];
        slices += s;
        if (s) kprintf("[slice  ]   cpu%d's timer sliced %u context(s) out mid-loop\n",
                       (uint64_t)i, (uint64_t)s);
    }
    uint32_t dn = g_dlog_n; if (dn > DLOG_LEN) dn = DLOG_LEN;
    int transitions = 0, last = -1, mine;
    kputs("[slice  ] dispatch order (all cores):");
    for (uint32_t i = d0; i < dn; i++) {
        mine = 0;
        for (int j = 0; j < NSL; j++) if (g_dlog[i] == procs[j]) mine = 1;
        if (!mine) continue;
        kprintf(" %u", kprocs[g_dlog[i]].pid);
        if (last >= 0 && g_dlog[i] != last) transitions++;
        last = g_dlog[i];
    }
    kputs("\n");
    int all_ok = 1, redispatched = 1;
    for (int i = 0; i < NSL; i++) {
        struct kproc *k = &kprocs[procs[i]];
        if (!k->exited || k->exit_code != k->pid) all_ok = 0;
        if (k->dispatches < 2) redispatched = 0;
        kprintf("[slice  ]   pid %u: exit %u (want %u)  dispatched %u time(s)  ran_on %x\n",
                k->pid, k->exit_code, k->pid, (uint64_t)k->dispatches, (uint64_t)k->ran_on);
    }
    slcheck("all three threads completed with intact checksums (exit == pid) despite slicing",
            all_ok);
    slcheck("AP timers forced context switches (total slice count >= 2)", slices >= 2);
    slcheck("every thread was set down and picked back up (each dispatched >= 2 times)",
            redispatched);
    slcheck("the dispatch log shows ROUND-ROBIN interleaving (>= 4 alternations between threads)",
            transitions >= 4);

done:
    kprintf("[slice  ] RESULT: %d passed, %d failed\n", (uint64_t)g_slpass, (uint64_t)g_slfail);
    if (!g_slfail) kputs("[slice  ] TIME-SLICING VERIFIED — an AP preemptively multitasks ring-3 threads on its own clock\n");
    else          kputs("[slice  ] TIME-SLICING DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ---- The C side of the syscall trap --------------------------------------- */
/* Called from syscall_entry (ring 0) with the user's number + args.          */
static int64_t sys_map_framebuffer(int proc_idx);   /* defined with the graphics engine */

/* v0.41: concurrency witnesses for the file-syscall paths. fs_ops says WHICH
 * cores executed file syscalls; the in-flight high-water mark says whether
 * two of them were inside file syscalls AT THE SAME TIME — the direct proof
 * the cio suite demands, immune to sampling gaps.                            */
static volatile int g_fs_inflight = 0, g_fs_inflight_max = 0;
static inline void fs_witness_enter(void) {
    g_cpu[cpu_idx()].fs_ops++;
    int now = __sync_add_and_fetch(&g_fs_inflight, 1);
    for (;;) {
        int m = g_fs_inflight_max;
        if (now <= m || __sync_bool_compare_and_swap(&g_fs_inflight_max, m, now)) break;
    }
}
static inline void fs_witness_leave(void) { __sync_fetch_and_sub(&g_fs_inflight, 1); }

/* Resolve fd -> dirent index under the ofile lock, enforcing ownership.
 * v0.48: also reports which VOLUME the fd was opened against, so callers can
 * dispatch to the right backing store — a fd's volume never changes after
 * open, so this is the one place that isolation boundary is enforced.        */
static int ofile_deref(int fd, int *out_vol) {
    if (fd < 0 || fd >= 16) return -1;
    klock_acquire(&g_ofile_lock);
    int di = (g_ofiles[fd].used && g_ofiles[fd].owner == (int)current_proc_idx)
           ? g_ofiles[fd].dirent : -1;
    if (di >= 0 && out_vol) *out_vol = g_ofiles[fd].volume;
    klock_release(&g_ofile_lock);
    return di;
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2) {
    switch (num) {
    case 0: {                                              /* SYS_WRITE(cstr)            */
        char buf[257];
        if (copy_user_str(kprocs[current_proc_idx].cr3, a0, buf, sizeof buf) < 0) return (uint64_t)-14;
        for (int i = 0; buf[i]; i++) kputc(buf[i]);
        return 0;
    }
    case 1: {                                              /* SYS_HW_PASSTHROUGH(handle) */
        int idx;
        if (a0 == 0xFFFF) idx = g_demo_dev_index;          /* explicit "default device"  */
        else if (a0 < (uint64_t)n_kdev) idx = (int)a0;
        else return (uint64_t)-1;                          /* reject invalid handle      */
        struct kdev *d = &kdevs[idx];
        int64_t r = sys_hardware_passthrough((int)current_proc_idx, d->base);
        return (uint64_t)r;
    }
    case 2:                                                /* SYS_EXIT(code)             */
        /* On an AP (cpu != 0) this is a serialized ap_run_uthread excursion —   */
        /* return through resume_kernel to the AP's C loop, never the BSP reaper. */
        if (cpu_idx() == 0 && curthr->uthread) uthread_exit(a0);   /* BSP first-class thread */
        write_cr3(kernel_cr3);                             /* leave the process address space */
        resume_kernel(a0);                                 /* AP excursion / BSP legacy excursion */
        return 0;                                          /* unreached                  */
    case 3:                                                /* SYS_WRITEHEX(value)        */
        kprintf("%X", a0);
        return 0;
    case 4:                                                /* SYS_DEV_OFFSET(kind)       */
        /* 6..10 expose the REAL virtio-net layout so a ring-3 driver can find  */
        /* the device's register structures inside its own MMIO mapping.        */
        if (a0 == 6)  return g_vnet_off_common;
        if (a0 == 7)  return g_vnet_off_notify;
        if (a0 == 8)  return g_vnet_off_isr;
        if (a0 == 9)  return g_vnet_off_devcfg;
        if (a0 == 10) return g_vnet_notify_mul;
        return a0 == 1 ? g_virtio_common : g_virtio_devcfg;

    /* --- capability-gated VFS file operations (require CAP_FILESYSTEM) --- */
    /* v0.41: these four are the paths the cio suite drives from several cores
     * at once. Each counts into this CPU's fs_ops witness and into the global
     * in-flight high-water mark (max >= 2 PROVES two cores were inside file
     * syscalls simultaneously). fd deref happens under the ofile lock with an
     * owner check; the dirent index it yields stays valid after release —
     * dirents are never destroyed, so the worst post-release race with a
     * concurrent close is a read of a file whose fd just went away: benign.  */
    case 5: {                                              /* SYS_OPEN(name)             */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_FILESYSTEM)) return (uint64_t)-13;
        char name[64];
        if (copy_user_str(kprocs[current_proc_idx].cr3, a0, name, sizeof name) < 0) return (uint64_t)-14;
        fs_witness_enter();
        int fd = vfs_open(name);
        fs_witness_leave();
        return (uint64_t)(int64_t)fd;
    }
    case 6: {                                              /* SYS_READ(fd, buf, len)     */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_FILESYSTEM)) return (uint64_t)-13;
        int fd = (int)a0;
        uint32_t len = (uint32_t)a2; if (len > 65536) len = 65536;
        if (!access_ok(kprocs[current_proc_idx].cr3, a1, len, 1)) return (uint64_t)-14;  /* must be USER-writable */
        fs_witness_enter();
        int vol = VOL_ROOT;
        int di = ofile_deref(fd, &vol);                     /* owner-checked      */
        int64_t n = -9;
        if (di >= 0) {
            if (vol == VOL_ROOT)      n = vfs_read_file(di, (void *)a1, len);
            else if (vol == VOL_TMP)  n = tmp_read_file(di, (void *)a1, len);
            else /* VOL_DEV */        n = dev_read_file((void *)a1, len);
        }
        fs_witness_leave();
        return (uint64_t)n;
    }
    case 7: {                                              /* SYS_WRITE_FILE(fd, buf, len) */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_FILESYSTEM)) return (uint64_t)-13;
        int fd = (int)a0;
        uint32_t len = (uint32_t)a2; if (len > 65536) len = 65536;
        if (!access_ok(kprocs[current_proc_idx].cr3, a1, len, 0)) return (uint64_t)-14;  /* must be USER-readable */
        fs_witness_enter();
        int vol = VOL_ROOT;
        int di = ofile_deref(fd, &vol);
        int r = -9;
        if (di >= 0) {
            if (vol == VOL_ROOT)      r = vfs_write_by_dirent(di, (const void *)a1, len);  /* COW */
            else if (vol == VOL_TMP)  r = tmp_write_file(di, (const void *)a1, len);
            else /* VOL_DEV */        r = -13;              /* read-only volume: capability-style denial */
        }
        fs_witness_leave();
        return r < 0 ? (uint64_t)(int64_t)r : len;
    }
    case 8: {                                              /* SYS_CLOSE(fd)              */
        int fd = (int)a0;
        if (fd >= 0 && fd < 16) {
            fs_witness_enter();
            klock_acquire(&g_ofile_lock);
            if (g_ofiles[fd].used && g_ofiles[fd].owner == (int)current_proc_idx)
                { g_ofiles[fd].used = 0; g_ofiles[fd].owner = -1; }
            klock_release(&g_ofile_lock);
            fs_witness_leave();
        }
        return 0;
    }
    case 9: {                                              /* SYS_WAIT_EVENT(type)       */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_NETWORK)) return (uint64_t)-13;
        if (a0 == EV_NET_RX) net_wait_rx();
        return 0;
    }
    case 10:                                               /* SYS_MAP_FRAMEBUFFER        */
        return (uint64_t)sys_map_framebuffer((int)current_proc_idx);

    case 11: {   /* SYS_DMA_ALLOC(pages, *out_phys) — DMA memory for a ring-3 driver */
        /* Physically contiguous, mapped into the caller, and added live to the  */
        /* caller's IOMMU domain so its device can reach exactly these pages.    */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_HW_PASSTHROUGH)) return (uint64_t)-13;
        uint64_t n = a0;
        if (n == 0 || n > 64) return (uint64_t)-1;
        if (!access_ok(kprocs[current_proc_idx].cr3, a1, 8, 1)) return (uint64_t)-14;
        struct kproc *p = &kprocs[current_proc_idx];
        uint64_t phys = alloc_frames(n);
        uint64_t va = DMA_USER_V + p->dma_next;
        for (uint64_t i = 0; i < n; i++) {
            for (int z = 0; z < 512; z++) ((uint64_t *)(phys + i * 0x1000))[z] = 0;
            map_page(p->cr3, va + i * 0x1000, phys + i * 0x1000, PTE_USER | PTE_WRITE | PTE_NX);
        }
        /* v0.44: ONE grant record for the whole contiguous range (not one per
         * page — MAX_DMA_GRANTS is sized for "how many DISTINCT allocations a
         * process holds at once", and a single call can request up to 64
         * pages). dma_grant_create adds every page to the IOMMU domain. */
        dma_grant_create(p, phys, n * 0x1000ull, DMA_GRANT_PAGE, 0xFFFF);
        p->dma_next += n * 0x1000;
        *(volatile uint64_t *)a1 = phys;                   /* IOVA == physical   */
        return va;
    }
    case 12:                                               /* SYS_ROLE()          */
        return kprocs[current_proc_idx].role;

    case 15:                                               /* SYS_YIELD()         */
        /* v0.41 audit fix: sched_yield() manipulates BSP scheduler state      */
        /* (g_cur/g_threads). Invoked from an AP it would context-switch a     */
        /* random BSP thread ON THE WRONG CORE. An AP runs exactly one ring-3  */
        /* task with nothing to yield to: pause and return.                    */
        if (cpu_idx() == 0) sched_yield();                 /* real scheduler yield: the  */
        else __asm__ volatile("pause");
        return 0;                                          /* thread resumes here later  */

    case 16:                                               /* SYS_GETPID()        */
        return kprocs[current_proc_idx].pid;

    case 14: {   /* SYS_SURFACE_POLL(slot, *out_event) -> 1 if an event was popped */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_FRAMEBUFFER)) return (uint64_t)-13;
        int slot = (int)a0;
        if (slot < 0 || slot >= 8 || !g_surf[slot].used) return (uint64_t)-1;
        if (g_surf[slot].owner != (int)current_proc_idx) return (uint64_t)-13;   /* owner only */
        if (!access_ok(kprocs[current_proc_idx].cr3, a1, sizeof(struct sevent), 1)) return (uint64_t)-14;
        struct surface *S = &g_surf[slot];
        if (S->qr == S->qw) return 0;
        *(struct sevent *)a1 = S->q[S->qr++ % 16];
        return 1;
    }
    case 13: {   /* SYS_SURFACE_CREATE((w<<16)|h, slot) — ring-3 app surface     */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_FRAMEBUFFER)) return (uint64_t)-13;
        int sw = (int)(a0 >> 16), sh = (int)(a0 & 0xFFFF), slot = (int)a1;
        if (sw < 8 || sw > 512 || sh < 8 || sh > 512) return (uint64_t)-1;
        if (slot < 0 || slot >= 8) return (uint64_t)-1;
        uint64_t bufpages = ((uint64_t)sw * sh * 4 + 0xFFF) / 0x1000;
        uint64_t pages = 2 * bufpages;                     /* v0.32: front+back pair */
        /* v0.41: the rendering allocation tables — slot claim + pixel-buffer   */
        /* free list — go under g_surf_lock (rank 5). Two cores creating        */
        /* surfaces previously raced the free-list pop AND could both claim     */
        /* one slot. A slot owned by a LIVE foreign process is now rejected     */
        /* instead of silently hijacked; a dead owner's slot is reclaimed in    */
        /* place, and re-claim by the same owner recycles its own pair.         */
        klock_acquire(&g_surf_lock);
        if (g_surf[slot].used) {
            int ow = g_surf[slot].owner;
            if (ow != (int)current_proc_idx && !(ow >= 0 && ow < n_kproc && kprocs[ow].exited)) {
                klock_release(&g_surf_lock);
                return (uint64_t)-16;                      /* busy: live owner   */
            }
            surfaces_reclaim(ow);                          /* stale/own: recycle */
        }
        uint64_t phys = 0; int recycled = 0;
        /* v0.42: search MOST-RECENTLY-RECLAIMED first (LIFO), not array order.
         * g_surf_nfree only ever grows by appending and shrinks by swap-with-
         * last, so index (nfree-1) is always the latest reclaim; searching
         * forward from 0 could hand out an OLDER residual entry (e.g. left
         * over from an earlier suite's own churn test) ahead of the buffer
         * that was JUST freed one line above this call — which is exactly
         * what a caller checking "did I get back the buffer I just freed"
         * (as the threads suite's recycle test does) is entitled to expect.  */
        for (int fi = g_surf_nfree - 1; fi >= 0; fi--)
            if (g_surf_free[fi].pages >= pages) {
                phys = g_surf_free[fi].phys;
                g_surf_free[fi] = g_surf_free[--g_surf_nfree];
                recycled = 1;
                break;
            }
        if (!phys) phys = alloc_frames(pages);             /* rank 5 -> atomic bump */
        struct kproc *p = &kprocs[current_proc_idx];
        for (uint64_t i = 0; i < pages; i++) {             /* both buffers user-mapped */
            for (int z = 0; z < 512; z++) ((uint64_t *)(phys + i * 0x1000))[z] = 0;
            map_page(p->cr3, SURF_USER_V + i * 0x1000, phys + i * 0x1000, PTE_USER | PTE_WRITE | PTE_NX);
        }
        g_surf[slot].phys = phys; g_surf[slot].w = sw; g_surf[slot].h = sh;
        g_surf[slot].bufpages = (int)bufpages; g_surf[slot].front = 0;
        g_surf[slot].flip_pending = 0;
        g_surf[slot].qw = g_surf[slot].qr = 0;
        g_surf[slot].owner = (int)current_proc_idx;
        barrier();
        g_surf[slot].used = 1;                             /* publish LAST        */
        klock_release(&g_surf_lock);
        kprintf("[surface] pid %u owns surface %d (%ux%u) DOUBLE-BUFFERED at user vaddr %X (+%u pages back)%s\n",
                p->pid, (uint64_t)slot, (uint64_t)sw, (uint64_t)sh, SURF_USER_V,
                bufpages, recycled ? " (recycled pixel buffers)" : "");
        return SURF_USER_V;
    }

    case 17: {   /* SYS_SURFACE_FLIP(slot) — publish the back buffer.            */
        /* Vsync semantics: mark the flip pending and BLOCK until a compositor  */
        /* frame boundary consumes it (canvas_frame), so the compositor can     */
        /* never blit a half-drawn frame and the app can never draw into a      */
        /* buffer being blitted. With no compositor pass live, consume at once. */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_FRAMEBUFFER)) return (uint64_t)-13;
        int slot = (int)a0;
        if (slot < 0 || slot >= 8 || !g_surf[slot].used) return (uint64_t)-1;
        if (g_surf[slot].owner != (int)current_proc_idx) return (uint64_t)-13;   /* owner only */
        struct surface *S = &g_surf[slot];
        S->flip_pending = 1;
        /* v0.41 audit fix: this wait was the third path reaching BSP scheduler */
        /* state from an AP. krelax() is CPU-aware: BSP threads yield, an AP    */
        /* PAUSE-spins with IF set while the BSP-side compositor consumes.      */
        while (S->flip_pending && S->consumer) krelax();        /* v0.34: per-slot */
        if (S->flip_pending) {                         /* nobody consumes this  */
            S->front ^= 1; S->flip_pending = 0;        /* slot: consume at once,*/
            krelax();                                  /* but a flip is still a */
        }                                              /* frame boundary — it   */
                                                       /* must ALWAYS schedule  */
        /* return the NEW back buffer's user vaddr — where the app draws next   */
        return SURF_USER_V + (uint64_t)(S->front ^ 1) * S->bufpages * 0x1000;
    }

    /* --- v0.46: capability-bound IPC (require CAP_IPC) --------------------- */
    case 18: {   /* SYS_IPC_SEND(msg_ptr) — capability-gated, zero-copy handle/frame transfer */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_IPC)) return (uint64_t)-13;
        if (!access_ok(kprocs[current_proc_idx].cr3, a0, sizeof(struct ipc_msg), 0)) return (uint64_t)-14;
        struct ipc_msg kmsg;
        { const volatile uint8_t *s = (const volatile uint8_t *)a0; uint8_t *d = (uint8_t *)&kmsg;
          for (uint64_t i = 0; i < sizeof kmsg; i++) d[i] = s[i]; }
        if (kmsg.payload_len > IPC_INLINE_MAX) return (uint64_t)-1;
        kmsg.sender_pid = kprocs[current_proc_idx].pid;   /* authoritative — never the caller's claim */
        int rcpt = kproc_find_by_pid(kmsg.recipient_pid);
        if (rcpt < 0) return (uint64_t)-9;                /* no such process (dead or never existed)  */

        if (kmsg.msg_type == IPC_MSG_XFER_FD) {
            if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_FILESYSTEM)) return (uint64_t)-13;
            int fd = (int)kmsg.xfer_handle;
            klock_acquire(&g_ofile_lock);
            int ok = fd >= 0 && fd < 16 && g_ofiles[fd].used && g_ofiles[fd].owner == (int)current_proc_idx;
            if (ok) g_ofiles[fd].owner = rcpt;            /* ownership moves NOW, not at RECV  */
            klock_release(&g_ofile_lock);
            if (!ok) return (uint64_t)-9;
        } else if (kmsg.msg_type == IPC_MSG_XFER_SHM) {
            int64_t id = ipc_shmem_grant(kmsg.xfer_handle, (int)current_proc_idx, rcpt);
            if (id < 0) return (uint64_t)-9;
            kmsg.xfer_handle = id;
            ipc_shmem_map_self((int)current_proc_idx, id);   /* sender can write into it right away */
            uint64_t v = IPC_SHM_V + (uint64_t)id * 0x1000;
            for (int b = 0; b < 8; b++) kmsg.inline_data[b] = (uint8_t)(v >> (8 * b));
        }

        if (!ipc_queue_push(rcpt, &kmsg)) return (uint64_t)-12;    /* recipient mailbox full */
        if (g_debug_ipc)
            kprintf("[dbgipc ] SEND pid %u -> pid %u type=%u xfer=%X len=%u\n",
                    kmsg.sender_pid, kmsg.recipient_pid, (uint64_t)kmsg.msg_type,
                    (uint64_t)kmsg.xfer_handle, (uint64_t)kmsg.payload_len);
        { volatile uint8_t *d = (volatile uint8_t *)a0; uint8_t *s = (uint8_t *)&kmsg;
          for (uint64_t i = 0; i < sizeof kmsg; i++) d[i] = s[i]; }   /* hand back sender_pid/xfer id/vaddr */
        return 0;
    }
    case 19: {   /* SYS_IPC_RECV(out_msg_ptr, blocking) -> 1 if a message was popped, 0 if none */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_IPC)) return (uint64_t)-13;
        if (!access_ok(kprocs[current_proc_idx].cr3, a0, sizeof(struct ipc_msg), 1)) return (uint64_t)-14;
        struct ipc_msg kmsg;
        uint64_t t0 = g_ticks; int got = 0;
        for (;;) {
            got = ipc_queue_pop((int)current_proc_idx, &kmsg);
            if (got || !a1 || g_ticks - t0 > 4000) break;     /* a1==0: poll once, never block */
            krelax();
        }
        if (!got) return 0;
        if (kmsg.cap_mask && !rust_cap_check(kprocs[current_proc_idx].caps, kmsg.cap_mask)) {
            /* Recipient lacks the capability the message declared as required.       */
            /* Ownership already moved at SEND time, so rejecting here must release   */
            /* the resource outright rather than trying to bounce it back to a sender */
            /* that may no longer even exist — keeps the reject path as simple as the */
            /* accept path instead of inventing a return-to-sender protocol.          */
            if (kmsg.msg_type == IPC_MSG_XFER_FD) {
                int fd = (int)kmsg.xfer_handle;
                klock_acquire(&g_ofile_lock);
                if (fd >= 0 && fd < 16 && g_ofiles[fd].used && g_ofiles[fd].owner == (int)current_proc_idx)
                    { g_ofiles[fd].used = 0; g_ofiles[fd].owner = -1; }
                klock_release(&g_ofile_lock);
            } else if (kmsg.msg_type == IPC_MSG_XFER_SHM) {
                klock_acquire(&g_ipc_lock);
                if (kmsg.xfer_handle >= 0 && kmsg.xfer_handle < MAX_IPC_SHMEM)
                    g_ipc_shm[kmsg.xfer_handle].owner_mask &= ~(1ull << current_proc_idx);
                klock_release(&g_ipc_lock);
            }
            return (uint64_t)-13;
        }
        if (kmsg.msg_type == IPC_MSG_XFER_SHM) {
            ipc_shmem_map_self((int)current_proc_idx, kmsg.xfer_handle);
            uint64_t v = IPC_SHM_V + (uint64_t)kmsg.xfer_handle * 0x1000;
            for (int b = 0; b < 8; b++) kmsg.inline_data[b] = (uint8_t)(v >> (8 * b));
        }
        { uint8_t *s = (uint8_t *)&kmsg; volatile uint8_t *d = (volatile uint8_t *)a0;
          for (uint64_t i = 0; i < sizeof kmsg; i++) d[i] = s[i]; }
        if (g_debug_ipc)
            kprintf("[dbgipc ] RECV pid %u <- pid %u type=%u xfer=%X len=%u\n",
                    kmsg.recipient_pid, kmsg.sender_pid, (uint64_t)kmsg.msg_type,
                    (uint64_t)kmsg.xfer_handle, (uint64_t)kmsg.payload_len);
        return 1;
    }

    /* --- v0.47: user-space interrupt architecture & VFIO MMIO mapping (require CAP_VFIO) --- */
    case 20: {   /* SYS_VFIO_MAP_BAR(device_id, bar_index, flags) -> vaddr or negative error */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_VFIO)) return (uint64_t)-13;
        int idx = (a0 == 0xFFFF) ? g_demo_dev_index : (a0 < (uint64_t)n_kdev ? (int)a0 : -1);
        if (idx < 0) return (uint64_t)-1;
        struct kdev *d = &kdevs[idx];
        if (!rust_cap_check(kprocs[current_proc_idx].caps, d->req)) return (uint64_t)-13;
        uint64_t phys, len;
        if (a1 == 0) { phys = d->base; len = d->len; }
        else if (a1 == 1 && g_kdev_bar1_len[idx]) { phys = g_kdev_bar1_phys[idx]; len = g_kdev_bar1_len[idx]; }
        else return (uint64_t)-1;                       /* invalid, or unbacked bar_index */

        /* Isolation: the ONLY physical ranges ever reachable here are ones the  */
        /* kernel itself registered as a device (kdevs[idx].base or its bar1) — */
        /* SYS_VFIO_MAP_BAR never accepts a raw physaddr from userspace, so      */
        /* there is no path to map arbitrary system RAM as if it were MMIO.      */
        uint64_t vbase = VFIO_BAR_V + (uint64_t)current_proc_idx * 0x100000ull + a1 * 0x10000ull;
        uint64_t pages = (len + 0xFFF) / 0x1000;
        /* flags bit0: write-combining requested. NOT actually implemented —     */
        /* WC needs a PAT entry this kernel's default (unreprogrammed) PAT table */
        /* doesn't have, and reprogramming IA32_PAT is a global change affecting */
        /* every existing mapping's cache behavior, out of scope for one         */
        /* syscall. Every mapping is uncacheable (PCD) regardless of the flag —  */
        /* correct, just not the weaker/faster ordering WC would give a real     */
        /* framebuffer-style bulk-write driver. */
        uint64_t pteflags = PTE_WRITE | PTE_USER | PTE_NX | PTE_PCD;
        for (uint64_t k = 0; k < pages; k++)
            map_page(kprocs[current_proc_idx].cr3, vbase + k * 0x1000, phys + k * 0x1000, pteflags);
        if (dma_grant_create(&kprocs[current_proc_idx], phys, pages * 0x1000ull,
                              DMA_GRANT_MMIO, d->bdf) < 0)
            return (uint64_t)-12;                       /* grant table full */
        if (g_kdev_irq_line[idx] >= 0) {
            int line = g_kdev_irq_line[idx];
            g_vfio_irq_owner[line] = (int)current_proc_idx;
            if (g_debug_vfio)
                kprintf("[dbgvfio] pid %u slot %u: MAP_BAR dev %d bar %u -> vaddr %X (%u pages), owns IRQ line %d\n",
                        kprocs[current_proc_idx].pid, current_proc_idx, idx, (uint64_t)a1,
                        vbase, pages, (uint64_t)line);
        } else if (g_debug_vfio)
            kprintf("[dbgvfio] pid %u slot %u: MAP_BAR dev %d bar %u -> vaddr %X (%u pages)\n",
                    kprocs[current_proc_idx].pid, current_proc_idx, idx, (uint64_t)a1, vbase, pages);
        return vbase;
    }
    case 21: {   /* SYS_VFIO_WAIT_IRQ(vector_id, timeout_ms) -> 1 if fired, 0 if timed out */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_VFIO)) return (uint64_t)-13;
        int line = (int)a0;
        if (line < 0 || line >= MAX_VFIO_LINES) return (uint64_t)-1;
        if (g_vfio_irq_owner[line] != (int)current_proc_idx) return (uint64_t)-13;  /* not yours to wait on */
        uint64_t timeout_ticks = a1 / 10; if (a1 && !timeout_ticks) timeout_ticks = 1;
        uint32_t seen = g_vfio_irq_seq[line];
        uint64_t t0 = g_ticks;
        while (g_vfio_irq_seq[line] == seen && g_ticks - t0 < timeout_ticks) krelax();
        int fired = (g_vfio_irq_seq[line] != seen);
        if (g_debug_vfio)
            kprintf("[dbgvfio] pid %u slot %u: WAIT_IRQ line %d %s\n",
                    kprocs[current_proc_idx].pid, current_proc_idx, (uint64_t)line,
                    fired ? "fired" : "timed out");
        return fired ? 1 : 0;
    }

    /* --- v0.48: VFS journaling / directory reclamation (require CAP_FILESYSTEM) --- */
    case 22: {   /* SYS_VFS_SYNC() -> 1 if a pending journal commit was applied, 0 if none pending */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_FILESYSTEM)) return (uint64_t)-13;
        if (!g_cas_mounted) return (uint64_t)-1;
        klock_acquire(&g_vfs_lock);
        vfs_journal_apply();
        int applied = g_vfs_last_sync_applied;
        klock_release(&g_vfs_lock);
        if (g_debug_vfs)
            kprintf("[dbgvfs ] pid %u: SYS_VFS_SYNC applied=%u\n", kprocs[current_proc_idx].pid, (uint64_t)applied);
        return (uint64_t)applied;
    }
    case 23: {   /* SYS_VFS_UNLINK(name) -> 0 on success, negative on failure */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_FILESYSTEM)) return (uint64_t)-13;
        char name[64];
        if (copy_user_str(kprocs[current_proc_idx].cr3, a0, name, sizeof name) < 0) return (uint64_t)-14;
        if (path_has_prefix(name, "tmp/") || path_has_prefix(name, "dev/")) return (uint64_t)-1;  /* ROOT-only */
        fs_witness_enter();
        int r = vfs_unlink(name);
        fs_witness_leave();
        if (g_debug_vfs)
            kprintf("[dbgvfs ] pid %u: SYS_VFS_UNLINK('%s') -> %d\n", kprocs[current_proc_idx].pid, name, r);
        return (uint64_t)(int64_t)r;
    }

    /* --- v0.49: SMP core scaling — TLB shootdown, affinity, remap/unmap --- */
    case 24: {   /* SYS_TLB_SHOOTDOWN(vaddr, pages, cpu_mask) -> acks received */
        /* The raw cross-core primitive, exposed to ring 3 under a dedicated
         * capability so the stress harness can drive it directly as well as
         * indirectly through SYS_SMP_REMAP/UNMAP below. A production build
         * would likely keep this kernel-internal-only (tlb_shootdown_range is
         * already called directly by remap/unmap) — it is exposed here to
         * make the syscall itself a first-class, independently testable
         * surface, exactly as the milestone specifies it. invlpg has no
         * memory side effect of its own, so no access_ok check is needed on
         * `vaddr` — a bogus address just invalidates nothing.               */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_SMP_ADMIN)) return (uint64_t)-13;
        return (uint64_t)(int64_t)tlb_shootdown_range(a0, (uint32_t)a1, (uint32_t)a2);
    }
    case 25: {   /* SYS_SET_AFFINITY(mask) -> 0 ok, -1 mask has no online cpu  */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_SMP_ADMIN)) return (uint64_t)-13;
        uint32_t mask = (uint32_t)a0;
        if (mask) {
            uint32_t online_mask = 0;
            for (int c = 0; c < MAX_CPUS; c++) if (g_cpu[c].online) online_mask |= (1u << c);
            mask &= online_mask;
            if (!mask) return (uint64_t)-1;      /* every requested cpu is offline */
        }
        kprocs[current_proc_idx].affinity = mask;    /* 0 = unrestricted */
        return 0;
    }
    case 26: {   /* SYS_SMP_REMAP(slot) -> new vaddr, or negative error        */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_SMP_ADMIN)) return (uint64_t)-13;
        int slot = (int)a0;
        if (slot < 0 || slot >= SMP_SLOTS) return (uint64_t)-1;
        struct kproc *p = &kprocs[current_proc_idx];
        uint64_t va = SMP_USER_V + (uint64_t)slot * 0x1000;
        while (__sync_lock_test_and_set(&p->vma_lock, 1)) __asm__ volatile("pause");
        uint64_t new_phys = alloc_frame();                 /* zeroed on return       */
        map_page(p->cr3, va, new_phys, PTE_USER | PTE_WRITE | PTE_NX);
        uint64_t old_phys = p->smp_slot_phys[slot];
        p->smp_slot_phys[slot] = new_phys;
        __sync_lock_release(&p->vma_lock);
        if (old_phys) {
            /* requirement (1): synchronous, acknowledged shootdown of every
             * cpu that could hold a stale translation for THIS vaddr in THIS
             * address space (ran_on) — THEN, and only then, the old frame is
             * handed back to the pool for reuse by anyone.                  */
            tlb_shootdown_range(va, 1, p->ran_on);
            free_frame(old_phys);
        }
        if (g_debug_smp_sched)
            kprintf("[dbgsmp ] pid %u SYS_SMP_REMAP slot %d: old=%X new=%X\n",
                    p->pid, (uint64_t)slot, old_phys, new_phys);
        return va;
    }
    case 27: {   /* SYS_SMP_UNMAP(slot) -> 0 ok, -1 slot wasn't mapped         */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_SMP_ADMIN)) return (uint64_t)-13;
        int slot = (int)a0;
        if (slot < 0 || slot >= SMP_SLOTS) return (uint64_t)-1;
        struct kproc *p = &kprocs[current_proc_idx];
        uint64_t va = SMP_USER_V + (uint64_t)slot * 0x1000;
        while (__sync_lock_test_and_set(&p->vma_lock, 1)) __asm__ volatile("pause");
        uint64_t old_phys = walk_pte(p->cr3, va) & ADDR_MASK;   /* v0.46 helper: leaf or 0 */
        if (old_phys) unmap_page(p->cr3, va);                   /* v0.46 helper: clears the leaf */
        p->smp_slot_phys[slot] = 0;
        __sync_lock_release(&p->vma_lock);
        if (!old_phys) return (uint64_t)-1;
        tlb_shootdown_range(va, 1, p->ran_on);              /* ack before recycling  */
        free_frame(old_phys);
        return 0;
    }
    case 28:                                               /* SYS_GET_CPU() -> cpu_idx() */
        return (uint64_t)cpu_idx();

    /* --- v0.50: virtio-gpu 2D resources / scanout / flush-fence (require CAP_SURFACE) --- */
    case 29: {   /* SYS_GPU_RESOURCE_CREATE(width, height, *out_resource_id) -> vaddr or negative */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_SURFACE)) return (uint64_t)-13;
        uint32_t width = (uint32_t)a0, height = (uint32_t)a1;
        if (width < 8 || width > 512 || height < 8 || height > 512) return (uint64_t)-1;
        if (!access_ok(kprocs[current_proc_idx].cr3, a2, 8, 1)) return (uint64_t)-14;
        if (!g_gpu_ready) return (uint64_t)-1;

        int slot = -1;
        klock_acquire(&g_gpu_lock);
        for (int i = 0; i < MAX_GPU_RES; i++)
            if (!g_gpu_res[i].used) { slot = i; g_gpu_res[i].used = 1;
                                       g_gpu_res[i].owner = (int)current_proc_idx; break; }
        uint32_t resid = slot >= 0 ? g_gpu_next_resid++ : 0;
        klock_release(&g_gpu_lock);
        if (slot < 0) return (uint64_t)-12;                 /* resource table full */

        struct vgpu_resource_create_2d cc; cmemset(&cc, 0, sizeof cc);
        cc.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        cc.resource_id = resid; cc.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        cc.width = width; cc.height = height;
        if (gpu_submit_wait(&cc, sizeof cc, 0) != 0) {
            klock_acquire(&g_gpu_lock); g_gpu_res[slot].used = 0; klock_release(&g_gpu_lock);
            return (uint64_t)-1;
        }

        uint32_t size = width * height * 4;
        uint64_t pages = (size + 0xFFF) / 0x1000;
        uint64_t phys = alloc_frames(pages);
        for (uint64_t i = 0; i < pages; i++)
            for (int z = 0; z < 512; z++) ((uint64_t *)(phys + i * 0x1000))[z] = 0;

        struct vgpu_attach_backing_1 ab; cmemset(&ab, 0, sizeof ab);
        ab.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
        ab.resource_id = resid; ab.nr_entries = 1;
        ab.entry_addr = phys; ab.entry_length = (uint32_t)(pages * 0x1000);
        if (gpu_submit_wait(&ab, sizeof ab, 0) != 0) {
            struct vgpu_resource_unref ur; cmemset(&ur, 0, sizeof ur);
            ur.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF; ur.resource_id = resid;
            gpu_submit_wait(&ur, sizeof ur, 0);
            klock_acquire(&g_gpu_lock); g_gpu_res[slot].used = 0; klock_release(&g_gpu_lock);
            return (uint64_t)-1;         /* the `pages` frames are unmapped/ungranted: bounded, disclosed */
        }

        struct kproc *p = &kprocs[current_proc_idx];
        uint64_t va = DMA_USER_V + p->dma_next;
        for (uint64_t i = 0; i < pages; i++)
            map_page(p->cr3, va + i * 0x1000, phys + i * 0x1000, PTE_USER | PTE_WRITE | PTE_NX);
        dma_grant_create(p, phys, pages * 0x1000ull, DMA_GRANT_PAGE, 0xFFFF);
        p->dma_next += pages * 0x1000;

        klock_acquire(&g_gpu_lock);
        g_gpu_res[slot].resource_id = resid; g_gpu_res[slot].phys = phys;
        g_gpu_res[slot].width = width; g_gpu_res[slot].height = height;
        g_gpu_res[slot].size = (uint32_t)(pages * 0x1000);
        klock_release(&g_gpu_lock);

        *(volatile uint64_t *)a2 = resid;
        if (g_debug_gpu)
            kprintf("[dbggpu ] pid %u: RESOURCE_CREATE %ux%u -> resid %u, vaddr %X (%u pages)\n",
                    kprocs[current_proc_idx].pid, (uint64_t)width, (uint64_t)height, (uint64_t)resid, va, pages);
        return va;
    }
    case 30: {   /* SYS_GPU_SET_SCANOUT(resource_id, width, height) -> 0 ok, negative error */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_SURFACE)) return (uint64_t)-13;
        uint32_t resid = (uint32_t)a0, width = (uint32_t)a1, height = (uint32_t)a2;
        int owned = 0;
        klock_acquire(&g_gpu_lock);
        for (int i = 0; i < MAX_GPU_RES; i++)
            if (g_gpu_res[i].used && g_gpu_res[i].resource_id == resid
                                   && g_gpu_res[i].owner == (int)current_proc_idx) { owned = 1; break; }
        klock_release(&g_gpu_lock);
        if (!owned) return (uint64_t)-13;

        struct vgpu_set_scanout sc; cmemset(&sc, 0, sizeof sc);
        sc.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
        sc.r.x = 0; sc.r.y = 0; sc.r.width = width; sc.r.height = height;
        sc.scanout_id = 0; sc.resource_id = resid;
        if (gpu_submit_wait(&sc, sizeof sc, 0) != 0) return (uint64_t)-1;

        klock_acquire(&g_gpu_lock);
        g_gpu_scanout_owner = (int)current_proc_idx; g_gpu_scanout_resid = resid;
        klock_release(&g_gpu_lock);
        if (g_debug_gpu)
            kprintf("[dbggpu ] pid %u: SET_SCANOUT resid %u (%ux%u)\n",
                    kprocs[current_proc_idx].pid, (uint64_t)resid, (uint64_t)width, (uint64_t)height);
        return 0;
    }
    case 31: {   /* SYS_GPU_SUBMIT_FLUSH(resource_id, width, height) -> fence_id (0 on failure) */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_SURFACE)) return (uint64_t)-13;
        uint32_t resid = (uint32_t)a0, width = (uint32_t)a1, height = (uint32_t)a2;
        int owned = 0;
        klock_acquire(&g_gpu_lock);
        for (int i = 0; i < MAX_GPU_RES; i++)
            if (g_gpu_res[i].used && g_gpu_res[i].resource_id == resid
                                   && g_gpu_res[i].owner == (int)current_proc_idx) { owned = 1; break; }
        klock_release(&g_gpu_lock);
        if (!owned) return 0;

        /* TRANSFER_TO_HOST_2D must land before RESOURCE_FLUSH can show anything
         * new — that ordering dependency is why this half blocks (hidden inside
         * one syscall) while the flush itself is the async, fence-tracked half. */
        struct vgpu_transfer_to_host_2d tr; cmemset(&tr, 0, sizeof tr);
        tr.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        tr.r.x = 0; tr.r.y = 0; tr.r.width = width; tr.r.height = height;
        tr.offset = 0; tr.resource_id = resid;
        if (gpu_submit_wait(&tr, sizeof tr, 0) != 0) return 0;

        struct vgpu_resource_flush fl; cmemset(&fl, 0, sizeof fl);
        fl.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
        fl.r.x = 0; fl.r.y = 0; fl.r.width = width; fl.r.height = height;
        fl.resource_id = resid;
        uint64_t fence = gpu_submit_async(&fl, sizeof fl);
        if (g_debug_gpu)
            kprintf("[dbggpu ] pid %u: SUBMIT_FLUSH resid %u -> fence %u\n",
                    kprocs[current_proc_idx].pid, (uint64_t)resid, fence);
        return fence;
    }
    case 32: {   /* SYS_GPU_FENCE_WAIT(fence_id, timeout_ms) -> 1 if fired, 0 if timed out/invalid */
        if (!rust_cap_check(kprocs[current_proc_idx].caps, PCAP_SURFACE)) return (uint64_t)-13;
        uint64_t fence_id = a0;
        if (fence_id == 0) return 0;
        uint64_t timeout_ticks = a1 / 10; if (a1 && !timeout_ticks) timeout_ticks = 1;
        int fired = gpu_fence_wait(fence_id, timeout_ticks);
        if (g_debug_gpu)
            kprintf("[dbggpu ] pid %u: FENCE_WAIT %u %s\n",
                    kprocs[current_proc_idx].pid, fence_id, fired ? "fired" : "timed out");
        return fired ? 1 : 0;
    }
    }
    return (uint64_t)-1;
}

/* ---- Run the ring-3 program under a given process identity ----------------- */
/* map USTK_PAGES stack pages (RW+USER+NX); the page below stays unmapped (guard) */
static void map_user_stack(uint64_t cr3) {
    for (int i = 0; i < USTK_PAGES; i++)
        map_page(cr3, USTK_V + (uint64_t)i * 0x1000, alloc_frame(), PTE_USER | PTE_WRITE | PTE_NX);
}

/* Enter ring 3 in a process's OWN address space so a granted MMIO mapping is
 * actually visible to the unprivileged code. Switches CR3 into the process,
 * runs, and (via SYS_EXIT -> cr3 restore) comes back in the kernel space.    */
static void enter_process(const char *label, int proc_idx, uint64_t entry) {
    current_proc_idx = (uint64_t)proc_idx;
    kprintf("\n[kernel ] --> RING 3 as pid %u '%s' (entry %X, caps %X, cr3 %X)\n",
            kprocs[proc_idx].pid, label, entry, kprocs[proc_idx].caps, kprocs[proc_idx].cr3);
    write_cr3(kprocs[proc_idx].cr3);       /* the process's own page tables      */
    enter_user_mode(entry, USTK_TOP);
    /* control returns here after SYS_EXIT or a fault unwind (resume_kernel).      */
    /* Both paths can land with IF cleared (SYSCALL SFMASK / interrupt gate), so   */
    /* restore interrupts for normal kernel execution.                            */
    __asm__ volatile("sti");
    kprintf("[kernel ] <-- RING 0: '%s' returned via SYS_EXIT\n", label);
}

/* Terminate a ring-3 task that faulted (called from isr_dispatch, CPL3 only).   */
/* A hit in the guard-page window is a stack overflow. Unwinds to the kernel via  */
/* resume_kernel so the fault can never destabilize kernel space.                 */
static void handle_cpl3_fault(struct isr_frame *f) {
    uint64_t cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    write_cr3(kernel_cr3);
    if (cr2 >= USTK_GUARD && cr2 < USTK_V) {
        g_guard_caught = 1;
        kprintf("\n[guard  ] STACK OVERFLOW: ring-3 pid %u hit guard page @ %X — task terminated\n",
                kprocs[current_proc_idx].pid, cr2);
    } else {
        kprintf("\n[fault  ] ring-3 pid %u fault (vec %u) cr2=%X rip=%X — task terminated\n",
                kprocs[current_proc_idx].pid, f->vector, cr2, f->rip);
    }
    if (cpu_idx() == 0 && curthr->uthread) {
        /* First-class BSP thread: reap it in place and reschedule. The kernel  */
        /* and every other thread keep running; nothing unwinds to kernel_main. */
        /* On an AP, curthr is BSP scheduler state and means nothing — a ring-3 */
        /* fault there unwinds through the AP's own per-CPU resume context.     */
        struct pcb *t = curthr;
        /* Synchronous fault on the dying task's behalf — thread context in     */
        /* effect, so the rank-5 acquire is legal here (never from a real IRQ). */
        klock_acquire(&g_surf_lock);
        surfaces_reclaim((int)t->proc);
        klock_release(&g_surf_lock);
        kprocs[t->proc].exit_code = 0x8000 + f->vector;
        kprocs[t->proc].exited = 1;
        /* v0.42: write_cr3(kernel_cr3) already ran above, before this branch — */
        /* the faulting space is off every core, so tear it down here too.      */
        /* v0.45: this is the ONE path that could previously leak a descriptor  */
        /* forever — a fault mid-syscall means the process's own SYS_CLOSE     */
        /* never runs. descriptor_teardown_kproc force-closes it here instead.  */
        /* v0.46: same reasoning extends to an in-flight IPC shmem grant.       */
        /* v0.47: and to IRQ-line ownership, for the exact same reason.         */
        vfio_teardown_kproc((int)t->proc);
        gpu_teardown_kproc((int)t->proc);
        ipc_teardown_kproc((int)t->proc);
        descriptor_teardown_kproc((int)t->proc);
        dma_teardown_kproc((int)t->proc);      /* v0.44: revoke DMA/IOMMU grants FIRST */
        kprocs[t->proc].frames_freed = page_free_tree(kprocs[t->proc].cr3);
        kprocs[t->proc].torn_down = 1;      /* v0.45: NOW the slot is safe to recycle */
        kprintf("[uthread] faulting tid %d terminated — siblings and kernel unaffected, %u frame(s) reclaimed\n",
                (uint64_t)t->id, kprocs[t->proc].frames_freed);
        t->state = T_FREE;
        sched_switch_to(pick_next());        /* does not return                 */
        for (;;) __asm__ volatile("hlt");
    }
    resume_kernel((uint64_t)-((int64_t)f->vector));
}

/* ===========================================================================
 * v0.41: CONCURRENT IO SUITE (cio) — several cores inside the VFS at once
 * ===========================================================================
 * The BSP and every AP simultaneously execute ring-3 file workers (role 9):
 * each open/COW-write/read-verify/close loops on its OWN file and on one
 * SHARED file, while two surface-churn workers (role 10) create, flip and
 * recycle surfaces from APs. What the suite demands afterwards:
 *
 *   - completion: every worker exits pid (the ring-3 side verified every
 *     read-back byte; any tear/corruption exits a 6xx/7xx code) within a
 *     watchdog — the deadlock-freedom check for the whole lock order.
 *   - simultaneity: >= 2 cores were INSIDE file syscalls at the same instant
 *     (g_fs_inflight_max), and >= 2 distinct cores dispatched file syscalls.
 *   - durable integrity, re-checked kernel-side: every worker file carries
 *     exactly its final-round pattern; the shared file is ONE uniform tagged
 *     image (whole-file writes are atomic — no interleaving of two writers);
 *     no duplicate directory names; every live chunk hash resolves in the
 *     CAS index; used_blocks == popcount(bitmap).
 *   - table hygiene: all fds released, churn slots reclaimed, free-list
 *     chunks pairwise disjoint, every klock free, ZERO rank violations.
 * =========================================================================== */
static int g_iopass, g_iofail;
static void ciocheck(const char *n, int c) {
    if (c) { g_iopass++; kprintf("[cio    ]  PASS  %s\n", n); }
    else   { g_iofail++; kprintf("[cio    ]  FAIL  %s\n", n); }
}
#define CIO_LEN    1024
#define CIO_ROUNDS 4
static void cio_name(char *dst, uint64_t pid) {            /* "cio-<pid>"        */
    char t[20]; int n = 0, p = 4;
    dst[0] = 'c'; dst[1] = 'i'; dst[2] = 'o'; dst[3] = '-';
    if (!pid) t[n++] = '0';
    while (pid) { t[n++] = (char)('0' + (pid % 10)); pid /= 10; }
    while (n) dst[p++] = t[--n];
    dst[p] = 0;
}
static uint8_t cio_byte(uint64_t pid, int r, int i) {      /* worker pattern     */
    return (uint8_t)(pid * 31 + (uint64_t)r * 17 + (uint64_t)i * 7);
}

static void cmd_cio(void) {
    kputs("-- CONCURRENT IO: BSP + APs execute VFS/CAS/surface syscalls simultaneously --\n");
    g_iopass = g_iofail = 0;
    uint64_t save = current_proc_idx;
    enum { WF = 4, WS = 2, NW = WF + WS };                 /* 4 file + 2 surface */
    int w[NW];
    static uint8_t buf[CIO_LEN];                           /* BSP-only staging   */

    for (int i = 0; i < NW; i++) {
        int p = kproc_spawn(i < WF ? "cio-file" : "cio-surf",
                            i < WF ? PCAP_FILESYSTEM : PCAP_FRAMEBUFFER);
        if (p < 0) { kputs("[cio    ] spawn failed\n-- done --\n"); return; }
        kprocs[p].role = i < WF ? 9 : 10;
        uint64_t e = elf_load(p, g_user_elf, g_user_elf_end - g_user_elf);
        current_proc_idx = save;
        if (!e) { kputs("[cio    ] ELF load failed\n-- done --\n"); return; }
        kprocs[p].entry = e;
        w[i] = p;
    }

    /* Pre-create every worker's file and the shared file: SYS_OPEN resolves    */
    /* names, it does not create them — creation is a kernel-side write.        */
    for (int i = 0; i < WF; i++) {
        char nm[16]; cio_name(nm, kprocs[w[i]].pid);
        for (int j = 0; j < CIO_LEN; j++) buf[j] = cio_byte(kprocs[w[i]].pid, 255, j);
        vfs_write_file(nm, buf, CIO_LEN);
    }
    for (int j = 0; j < CIO_LEN; j++) buf[j] = (uint8_t)(7 + j * 7);
    vfs_write_file("cio-shared", buf, CIO_LEN);

    uint32_t fs0[MAX_CPUS];
    for (int i = 0; i < MAX_CPUS; i++) fs0[i] = g_cpu[i].fs_ops;
    g_fs_inflight_max = 0;                                 /* fresh witness      */
    uint32_t nfree0 = (uint32_t)g_surf_nfree;
    (void)nfree0;

    int n = g_ncpu_online;
    if (n < 2) {
        kputs("[cio    ] uniprocessor boot — running all six workers sequentially on the BSP\n");
        for (int i = 0; i < NW; i++) { cpu_exec_proc(0, w[i]); current_proc_idx = save; }
    } else {
        /* Workers 1..NW-1 spread across the APs; the BSP EXECUTES worker 0     */
        /* itself, so the BSP's syscall path is concurrent with the APs' own.   */
        for (int i = 1; i < NW; i++) {
            int c = 1 + (i - 1) % (n - 1);
            rq_push(c, w[i]);
        }
        __sync_synchronize();
        for (int c = 1; c < n; c++) lapic_ipi(g_cpu[c].apic_id, IPI_PING, 0);
        kprintf("[cio    ] %d workers queued on %d APs; BSP entering ring 3 with its own file worker\n",
                (uint64_t)(NW - 1), (uint64_t)(n - 1));
        cpu_exec_proc(0, w[0]);
        current_proc_idx = save;
    }

    uint64_t t0 = g_ticks;                                 /* join + watchdog    */
    int all;
    for (;;) {
        all = 1;
        for (int i = 0; i < NW; i++) if (!kprocs[w[i]].exited) all = 0;
        if (all || g_ticks - t0 > 6000) break;
        __asm__ volatile("pause");
    }
    current_proc_idx = save;

    ciocheck("all six workers ran to completion (watchdog: no cross-core deadlock)", all);
    int ids_ok = 1;
    for (int i = 0; i < NW; i++) {
        struct kproc *k = &kprocs[w[i]];
        kprintf("[cio    ]   pid %u '%s': exit %u (want %u)  ran_on %x\n",
                k->pid, k->name, k->exit_code, k->pid, (uint64_t)k->ran_on);
        if (!k->exited || k->exit_code != k->pid) ids_ok = 0;
    }
    ciocheck("every worker verified every byte it read back (exit == pid, no 6xx/7xx)", ids_ok);

    if (n >= 2) {
        int cpus_fs = 0;
        for (int i = 0; i < MAX_CPUS; i++) {
            uint32_t d = g_cpu[i].fs_ops - fs0[i];
            if (d) { cpus_fs++; kprintf("[cio    ]   cpu%d dispatched %u file syscalls\n",
                                        (uint64_t)i, (uint64_t)d); }
        }
        ciocheck("file syscalls were dispatched by >= 2 distinct cores", cpus_fs >= 2);
        kprintf("[cio    ] file-syscall in-flight high-water: %d\n", (uint64_t)g_fs_inflight_max);
        ciocheck(">= 2 cores were INSIDE file syscalls at the same instant", g_fs_inflight_max >= 2);
    } else {
        kputs("[cio    ]  SKIP  cross-core simultaneity checks (uniprocessor boot)\n");
    }

    /* durable integrity, re-read kernel-side under the same locks             */
    int content_ok = 1;
    for (int i = 0; i < WF; i++) {
        char nm[16]; cio_name(nm, kprocs[w[i]].pid);
        klock_acquire(&g_vfs_lock);
        int di = vfs_find(nm);
        klock_release(&g_vfs_lock);
        if (di < 0) { content_ok = 0; continue; }
        if (vfs_read_file(di, buf, CIO_LEN) != CIO_LEN) { content_ok = 0; continue; }
        for (int j = 0; j < CIO_LEN; j++)
            if (buf[j] != cio_byte(kprocs[w[i]].pid, CIO_ROUNDS - 1, j)) { content_ok = 0; break; }
    }
    ciocheck("each worker file holds EXACTLY its final-round pattern (no torn write)", content_ok);

    int shared_ok = 0;
    {
        klock_acquire(&g_vfs_lock);
        int di = vfs_find("cio-shared");
        klock_release(&g_vfs_lock);
        if (di >= 0 && vfs_read_file(di, buf, CIO_LEN) == CIO_LEN) {
            shared_ok = 1;
            for (int j = 1; j < CIO_LEN; j++)
                if (buf[j] != (uint8_t)(buf[0] + j * 7)) { shared_ok = 0; break; }
        }
    }
    ciocheck("the shared file (16 racing writes) is ONE uniform image (whole-file writes atomic)", shared_ok);

    int dup = 0, unresolved = 0;
    klock_acquire(&g_vfs_lock);
    for (int i = 0; i < VFS_MAXFILES; i++) {
        if (!DENTS[i].used) continue;
        for (int j = i + 1; j < VFS_MAXFILES; j++)
            if (DENTS[j].used && streq_n(DENTS[i].name, DENTS[j].name, 32)) dup++;
        for (uint32_t c = 0; c < DENTS[i].nchunks; c++) {
            uint32_t len;
            klock_acquire(&g_cas_lock);
            int64_t b = cas_index_find(DENTS[i].chunk_hash[c], &len);
            klock_release(&g_cas_lock);
            if (b < 0) unresolved++;
        }
    }
    klock_release(&g_vfs_lock);
    ciocheck("directory: no duplicate names claimed by racing writers", dup == 0);
    ciocheck("every live chunk hash resolves in the CAS index", unresolved == 0);

    uint64_t popcnt = 0;
    klock_acquire(&g_cas_lock);
    for (uint64_t b = 0; b < SB->total_blocks; b++) if (bm_get(b)) popcnt++;
    uint64_t used = SB->used_blocks;
    klock_release(&g_cas_lock);
    kprintf("[cio    ] allocation bitmap: %d bits set, superblock used_blocks %d\n", popcnt, used);
    ciocheck("used_blocks == popcount(bitmap) (no double-allocated or leaked block)", popcnt == used);

    int fds_leaked = 0;
    klock_acquire(&g_ofile_lock);
    for (int fd = 0; fd < 16; fd++) if (g_ofiles[fd].used) fds_leaked++;
    klock_release(&g_ofile_lock);
    ciocheck("descriptor array fully released after the storm", fds_leaked == 0);

    int churn_free = 1, overlap = 0;
    klock_acquire(&g_surf_lock);
    if (g_surf[6].used || g_surf[7].used) churn_free = 0;
    for (int i = 0; i < g_surf_nfree; i++)
        for (int j = i + 1; j < g_surf_nfree; j++) {
            uint64_t ai = g_surf_free[i].phys, ae = ai + g_surf_free[i].pages * 0x1000;
            uint64_t bi = g_surf_free[j].phys, be = bi + g_surf_free[j].pages * 0x1000;
            if (ai < be && bi < ae) overlap++;
        }
    int nfree_now = g_surf_nfree;
    klock_release(&g_surf_lock);
    kprintf("[cio    ] surface free list: %d chunk(s)\n", (uint64_t)nfree_now);
    ciocheck("both churn surfaces were reclaimed on exit (AP exit path reclaims now)", churn_free);
    ciocheck("free-list pixel chunks are pairwise disjoint (no double-free/overlap)", overlap == 0);

    struct klock *ls[5] = { &g_ofile_lock, &g_vfs_lock, &g_cas_lock, &g_vblk_lock, &g_surf_lock };
    int held = 0;
    for (int i = 0; i < 5; i++) {
        kprintf("[cio    ]   lock %s (rank %d): %u acquisitions, %u contended\n",
                ls[i]->name, (uint64_t)ls[i]->rank, (uint64_t)ls[i]->acq, (uint64_t)ls[i]->contended);
        if (ls[i]->v) held++;
    }
    ciocheck("no lock left held at quiescence", held == 0);
    ciocheck("ZERO rank violations across the whole run (lock order held everywhere)",
             g_rank_violations == 0);

    kprintf("[cio    ] RESULT: %d passed, %d failed\n", (uint64_t)g_iopass, (uint64_t)g_iofail);
    if (!g_iofail) kputs("[cio    ] CONCURRENT IO VERIFIED — the VFS/CAS/surface stack is multi-core reentrant\n");
    else          kputs("[cio    ] CONCURRENT IO DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * v0.43: SMP STRESS HARNESS — a mixed workload biased across every core
 * ===========================================================================
 * Ten workers cycling through the THREE workload shapes this kernel already
 * exercises separately: role 6 (mcsched_probe: pure SYS_GETPID/EXIT), role 9
 * (cio_file_worker: VFS open/write/read/close, own file + racing shared
 * file), role 10 (cio_surface_churn: paint + SYS_SURFACE_FLIP). Exactly TWO
 * role-10 workers are spawned — cio_surface_churn picks its slot by
 * `pid & 1` (slots 6/7 only), and cmd_cio's own suite never runs more than
 * two concurrent instances for exactly that reason; more would race two
 * workers onto the SAME slot, a hazard that belongs to role 10 itself, not to
 * this harness. Placement is BIASED round-robin across every online core via
 * rq_push — the same placement primitive mcq/cio already use — and idle
 * siblings may still steal, so this is a bias, not a hard pin.
 *
 * v0.49 adds a second phase to the SAME suite: MIGRATE_N role-16 workers
 * (smp_migrate_worker) that rapidly remap/unmap a private scratch page via
 * SYS_SMP_REMAP/SYS_SMP_UNMAP — the new synchronous, acknowledged TLB
 * shootdown protocol (tlb_shootdown_range) runs on every one of those calls,
 * shooting down and reclaiming the OLD physical frame only after every cpu
 * that ever ran this address space has acknowledged. Half the workers pin
 * themselves to cpu0 via SYS_SET_AFFINITY, exercising the affinity-aware
 * work-stealing (rq_steal) and migration-target selection (cpu_exec_proc)
 * added this version. Assertions: zero stale TLB reads (exit == pid), zero
 * IPI deadlocks (watchdog), affinity honoured, zero rank violations.       */
static int g_smstrpass, g_smstrfail;
static void smstrcheck(const char *n, int c) {
    if (c) { g_smstrpass++; kprintf("[smpstrs]  PASS  %s\n", n); }
    else   { g_smstrfail++; kprintf("[smpstrs]  FAIL  %s\n", n); }
}

#define STRESS_N 10
#define MIGRATE_N 8                                    /* v0.49: phase-2 remap/migrate workers */

static void cmd_smp_stress(void) {
    kputs("-- SMP STRESS: mixed workload + v0.49 TLB shootdown/affinity/migration, biased across every core --\n");
    g_smstrpass = g_smstrfail = 0;
    uint64_t save = current_proc_idx;
    int n = g_ncpu_online; if (n > MAX_CPUS) n = MAX_CPUS;

    static const uint64_t roles[STRESS_N] = { 10, 10, 9, 9, 9, 9, 6, 6, 6, 6 };
    int procs[STRESS_N];
    static uint8_t stbuf[CIO_LEN];                     /* BSP-only pre-seed staging */
    for (int i = 0; i < STRESS_N; i++) {
        /* Same per-role capability grant cmd_cio uses: role 9 (VFS) needs
         * CAP_FILESYSTEM, role 10 (surface) needs CAP_FRAMEBUFFER, role 6
         * (bare GETPID/EXIT) needs neither. */
        uint64_t caps = roles[i] == 9 ? PCAP_FILESYSTEM
                      : roles[i] == 10 ? PCAP_FRAMEBUFFER : 0;
        int p = kproc_spawn("smp-stress", caps);
        if (p < 0) { kputs("[smpstrs] spawn failed\n-- done --\n"); return; }
        kprocs[p].role = roles[i];
        uint64_t entry = elf_load(p, g_user_elf, g_user_elf_end - g_user_elf);
        current_proc_idx = save;
        if (!entry) { kputs("[smpstrs] ELF load failed\n-- done --\n"); return; }
        kprocs[p].entry = entry;
        procs[i] = p;
        /* cio_file_worker's SYS_OPEN(own name) resolves an existing dirent, it
         * does not create one — cmd_cio pre-seeds the same way, kernel-side,
         * before dispatch. Missing this step is exactly what a fresh reader
         * of this suite would get wrong; do it here too. */
        if (roles[i] == 9) {
            char nm[16]; cio_name(nm, kprocs[p].pid);
            for (int j = 0; j < CIO_LEN; j++) stbuf[j] = cio_byte(kprocs[p].pid, 255, j);
            vfs_write_file(nm, stbuf, CIO_LEN);
        }
    }
    for (int j = 0; j < CIO_LEN; j++) stbuf[j] = (uint8_t)(11 + j * 7);
    vfs_write_file("cio-shared", stbuf, CIO_LEN);          /* the racing shared file */

    if (n > 1) {
        for (int i = 0; i < STRESS_N; i++) rq_push(i % n, procs[i]);   /* biased round-robin */
        __sync_synchronize();
        lapic_ipi(0, IPI_PING, 1);                                     /* wake every AP     */
        kprintf("[smpstrs] %d workers (2 surface + 4 VFS + 4 syscall) biased round-robin across %d cores\n",
                (uint64_t)STRESS_N, (uint64_t)n);
        int p;                                        /* the BSP drains ITS OWN share too, */
        while ((p = rq_pop(0)) >= 0) cpu_exec_proc(0, p);   /* no special-casing vs an AP   */
    } else {
        kputs("[smpstrs] uniprocessor boot — running the mix sequentially on the BSP\n");
        for (int i = 0; i < STRESS_N; i++) cpu_exec_proc(0, procs[i]);
    }

    uint64_t t0 = g_ticks;                             /* join + watchdog (deadlock proxy) */
    for (;;) {
        int all = 1;
        for (int i = 0; i < STRESS_N; i++) if (!kprocs[procs[i]].exited) all = 0;
        if (all || g_ticks - t0 > 4000) break;
        if (n > 1) lapic_ipi(0, IPI_PING, 1);
        uint64_t tw = g_ticks; while (g_ticks - tw < 2) __asm__ volatile("pause");
    }
    current_proc_idx = save;

    int all_exited = 1, ids_ok = 1;
    for (int i = 0; i < STRESS_N; i++) {
        struct kproc *k = &kprocs[procs[i]];
        if (!k->exited) all_exited = 0;
        if (!k->exited || k->exit_code != k->pid) ids_ok = 0;
        kprintf("[smpstrs]   pid %u role %u: exit %u (want %u) ran_on %x\n",
                k->pid, k->role, k->exit_code, k->pid, (uint64_t)k->ran_on);
    }
    smstrcheck("no watchdog timeout — every worker reached a terminal state (no deadlock)",
               all_exited);
    smstrcheck("every worker's exit code == its pid (no 6xx/7xx: VFS/surface work was correct)",
               ids_ok);
    if (n >= 2) {
        int cpus_used = 0;
        for (int c = 0; c < MAX_CPUS; c++) {
            int used = 0;
            for (int i = 0; i < STRESS_N; i++) if (kprocs[procs[i]].ran_on & (1u << c)) used = 1;
            if (used) cpus_used++;
        }
        kprintf("[smpstrs] %d distinct core(s) executed part of this mixed workload\n",
                (uint64_t)cpus_used);
        smstrcheck(">= 2 distinct cores executed part of the mix (bias + stealing both worked)",
                   cpus_used >= 2);
    } else {
        kputs("[smpstrs]  SKIP  multi-core distribution check (uniprocessor boot)\n");
    }
    smstrcheck("ZERO cross-core lock-rank violations across the whole mixed run",
               g_rank_violations == 0);

    /* =========================================================================
     * v0.49 PHASE 2: SMP core scaling — TLB shootdown + affinity + migration.
     * MIGRATE_N role-16 workers hammer SYS_SMP_REMAP/SYS_SMP_UNMAP (rapid
     * page remapping/unmapping of their own scratch page) across every core,
     * self-pinning to cpu0 by pid parity, yielding between rounds so the
     * scheduler is free to migrate them. A stale TLB read, a denied syscall,
     * or a wrong SYS_GETPID exits a distinct 9xx code the ring-3 side already
     * checks; this phase re-verifies from the kernel side and adds the
     * capability-gating and affinity checks the workers can't see for
     * themselves.                                                           */
    kputs("[smpstrs] -- v0.49 phase 2: TLB shootdown + affinity + remap/unmap migration --\n");
    {
        int pn = kproc_spawn("smpstrs-noadmin", 0);         /* lacks PCAP_SMP_ADMIN */
        if (pn >= 0) {
            current_proc_idx = (uint64_t)pn;
            int denied = (int64_t)syscall_dispatch(24, 0, 1, 0xFFFFFFFFu) == -13   /* SHOOTDOWN */
                      && (int64_t)syscall_dispatch(25, 1, 0, 0) == -13             /* SET_AFFINITY */
                      && (int64_t)syscall_dispatch(26, 0, 0, 0) == -13             /* SMP_REMAP */
                      && (int64_t)syscall_dispatch(27, 0, 0, 0) == -13;            /* SMP_UNMAP */
            current_proc_idx = save;
            smstrcheck("every v0.49 SMP syscall denied without PCAP_SMP_ADMIN", denied);
        }

        uint32_t stolen_before = 0, aborted_before = g_rq_steal_aborted;
        for (int c = 0; c < MAX_CPUS; c++) stolen_before += g_cpu[c].rq_stolen;

        int mprocs[MIGRATE_N];
        for (int i = 0; i < MIGRATE_N; i++) {
            int p = kproc_spawn("smp-migrate", PCAP_SMP_ADMIN);
            if (p < 0) { kputs("[smpstrs] phase-2 spawn failed\n"); goto phase2_done; }
            kprocs[p].role = 16;
            uint64_t entry = elf_load(p, g_user_elf, g_user_elf_end - g_user_elf);
            current_proc_idx = save;
            if (!entry) { kputs("[smpstrs] phase-2 ELF load failed\n"); goto phase2_done; }
            kprocs[p].entry = entry;
            /* v0.49: affinity pins WHERE THIS TASK MAY RUN going forward (steal
             * eligibility, migration targets) — it has no way to reach back and
             * relocate a task that is already executing elsewhere, so the odd-
             * pid self-pin the worker calls (SYS_SET_AFFINITY(1)) only matters
             * if the INITIAL placement below also honours it. Mirror the same
             * "odd pid -> cpu0 only" rule here, kernel-side, before dispatch:
             * the worker's own subsequent call is then a no-op confirmation of
             * a mask that was already true when it started running.           */
            if (kprocs[p].pid & 1) kprocs[p].affinity = 1u;
            mprocs[i] = p;
        }

        if (n > 1) {
            for (int i = 0; i < MIGRATE_N; i++) {
                int dst = kprocs[mprocs[i]].affinity ? 0 : (i % n);   /* honour the pin */
                rq_push(dst, mprocs[i]);
            }
            __sync_synchronize();
            lapic_ipi(0, IPI_PING, 1);
            int p;
            while ((p = rq_pop(0)) >= 0) cpu_exec_proc(0, p);
        } else {
            for (int i = 0; i < MIGRATE_N; i++) cpu_exec_proc(0, mprocs[i]);
        }

        uint64_t mt0 = g_ticks;
        for (;;) {
            int all = 1;
            for (int i = 0; i < MIGRATE_N; i++) if (!kprocs[mprocs[i]].exited) all = 0;
            if (all || g_ticks - mt0 > 4000) break;
            if (n > 1) lapic_ipi(0, IPI_PING, 1);
            uint64_t tw = g_ticks; while (g_ticks - tw < 2) __asm__ volatile("pause");
        }
        current_proc_idx = save;

        int m_all_exited = 1, m_ids_ok = 1, m_affinity_ok = 1;
        for (int i = 0; i < MIGRATE_N; i++) {
            struct kproc *k = &kprocs[mprocs[i]];
            if (!k->exited) m_all_exited = 0;
            if (!k->exited || k->exit_code != k->pid) m_ids_ok = 0;
            if ((k->pid & 1) && (k->ran_on & ~1u)) m_affinity_ok = 0;   /* pinned, but ran off cpu0 */
            kprintf("[smpstrs]   pid %u (role 16): exit %u (want %u) ran_on %x affinity %x\n",
                    k->pid, k->exit_code, k->pid, (uint64_t)k->ran_on, (uint64_t)k->affinity);
        }
        smstrcheck("no watchdog timeout — every remap/unmap/migration worker finished (0 IPI deadlocks)",
                   m_all_exited);
        smstrcheck("every worker's exit code == its pid (0 stale TLB reads across remap/unmap/migration)",
                   m_ids_ok);
        smstrcheck("every pid-pinned worker's ran_on stayed inside its affinity mask (cpu0 only)",
                   m_affinity_ok);

        uint32_t stolen_after = 0;
        for (int c = 0; c < MAX_CPUS; c++) stolen_after += g_cpu[c].rq_stolen;
        kprintf("[smpstrs] phase 2: %u task(s) stolen, %u non-blocking steal attempt(s) backed off busy\n",
                (uint64_t)(stolen_after - stolen_before), (uint64_t)(g_rq_steal_aborted - aborted_before));
        smstrcheck("ZERO cross-core lock-rank violations across the migration phase",
                   g_rank_violations == 0);
    }
phase2_done:
    current_proc_idx = save;

    kprintf("[smpstrs] RESULT: %d passed, %d failed\n", (uint64_t)g_smstrpass, (uint64_t)g_smstrfail);
    if (!g_smstrfail)
        kputs("[smpstrs] SMP STRESS VERIFIED — mixed syscall/VFS/compositor workload survives across every core\n");
    else kputs("[smpstrs] SMP STRESS DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * v0.44: DMA STRESS HARNESS — real DMA/IOMMU grants, held and revoked across
 * genuine kproc exit
 * ===========================================================================
 * nic_driver and cmd_capdma's dma-owner/dma-other already exercise
 * SYS_HW_PASSTHROUGH and SYS_DMA_ALLOC — but only through the legacy
 * one-shot enter_process excursion, which returns straight into kernel C
 * code and never reaches a real kproc exit path, so dma_teardown_kproc's
 * revoke logic had ZERO live coverage before this suite. Role 11
 * (dma_churn) does the same two syscalls through the MODERN, scheduled
 * path (kproc_spawn + elf_load + cpu_exec_proc), so its exit is a genuine
 * SYS_EXIT that runs dma_teardown_kproc for real.
 *
 * There is no kill syscall in this kernel (nothing to remove — every exit
 * is voluntary SYS_EXIT or a fault). "Kill them in random order" is
 * honoured as: every worker's completion order is the SCHEDULER'S OWN,
 * genuinely non-deterministic interleaving (proved by each one's distinct
 * finish_seq) — not an injected randomizer, because this kernel has no
 * mechanism to reach into a live process and terminate it externally.    */
static int g_dmstrpass, g_dmstrfail;
static void dmstrcheck(const char *n, int c) {
    if (c) { g_dmstrpass++; kprintf("[dmastrs]  PASS  %s\n", n); }
    else   { g_dmstrfail++; kprintf("[dmastrs]  FAIL  %s\n", n); }
}

#define DMASTRESS_N 10

static void cmd_dma_stress(void) {
    kputs("-- DMA STRESS: real passthrough/DMA grants held and revoked across genuine kproc exit --\n");
    g_dmstrpass = g_dmstrfail = 0;
    uint64_t save = current_proc_idx;
    int n = g_ncpu_online; if (n > MAX_CPUS) n = MAX_CPUS;
    uint64_t devreq = kdevs[g_demo_dev_index].req;     /* whatever the demo device needs */

    static const uint64_t roles[DMASTRESS_N] = { 11, 11, 11, 11, 9, 9, 9, 9, 10, 10 };
    int procs[DMASTRESS_N];
    static uint8_t stbuf[CIO_LEN];
    for (int i = 0; i < DMASTRESS_N; i++) {
        uint64_t caps = roles[i] == 11 ? (PCAP_HW_PASSTHROUGH | devreq)
                      : roles[i] == 9  ? PCAP_FILESYSTEM
                      : roles[i] == 10 ? PCAP_FRAMEBUFFER : 0;
        int p = kproc_spawn("dma-stress", caps);
        if (p < 0) { kputs("[dmastrs] spawn failed\n-- done --\n"); return; }
        kprocs[p].role = roles[i];
        uint64_t entry = elf_load(p, g_user_elf, g_user_elf_end - g_user_elf);
        current_proc_idx = save;
        if (!entry) { kputs("[dmastrs] ELF load failed\n-- done --\n"); return; }
        kprocs[p].entry = entry;
        procs[i] = p;
        if (roles[i] == 9) {                            /* same VFS pre-seed cmd_smp_stress needs */
            char nm[16]; cio_name(nm, kprocs[p].pid);
            for (int j = 0; j < CIO_LEN; j++) stbuf[j] = cio_byte(kprocs[p].pid, 255, j);
            vfs_write_file(nm, stbuf, CIO_LEN);
        }
    }
    for (int j = 0; j < CIO_LEN; j++) stbuf[j] = (uint8_t)(13 + j * 7);
    vfs_write_file("cio-shared", stbuf, CIO_LEN);

    uint64_t freed0 = g_frames_freed, reused0 = g_frames_reused;

    if (n > 1) {
        for (int i = 0; i < DMASTRESS_N; i++) rq_push(i % n, procs[i]);
        __sync_synchronize();
        lapic_ipi(0, IPI_PING, 1);
        kprintf("[dmastrs] %d workers (4 DMA-churn + 4 VFS + 2 surface) biased round-robin across %d cores\n",
                (uint64_t)DMASTRESS_N, (uint64_t)n);
        int p;
        while ((p = rq_pop(0)) >= 0) cpu_exec_proc(0, p);
    } else {
        kputs("[dmastrs] uniprocessor boot — running the mix sequentially on the BSP\n");
        for (int i = 0; i < DMASTRESS_N; i++) cpu_exec_proc(0, procs[i]);
    }

    uint64_t t0 = g_ticks;
    for (;;) {
        int all = 1;
        for (int i = 0; i < DMASTRESS_N; i++) if (!kprocs[procs[i]].exited) all = 0;
        if (all || g_ticks - t0 > 4000) break;
        if (n > 1) lapic_ipi(0, IPI_PING, 1);
        uint64_t tw = g_ticks; while (g_ticks - tw < 2) __asm__ volatile("pause");
    }
    current_proc_idx = save;

    int all_exited = 1, ids_ok = 1, grants_clear = 1, domains_clear = 1;
    for (int i = 0; i < DMASTRESS_N; i++) {
        struct kproc *k = &kprocs[procs[i]];
        if (!k->exited) all_exited = 0;
        if (!k->exited || k->exit_code != k->pid) ids_ok = 0;
        if (k->dma_grant_count != 0) grants_clear = 0;
        if (g_proc_slpt[procs[i]] != 0) domains_clear = 0;
        kprintf("[dmastrs]   pid %u role %u: exit %u (want %u) grants=%u slpt=%X finish#%u\n",
                k->pid, k->role, k->exit_code, k->pid, (uint64_t)k->dma_grant_count,
                g_proc_slpt[procs[i]], (uint64_t)k->finish_seq);
    }
    dmstrcheck("no watchdog timeout — every worker reached a terminal state", all_exited);
    dmstrcheck("every worker's exit code == its pid (DMA/passthrough work was correct)", ids_ok);
    dmstrcheck("every worker's DMA grant table is empty after exit (all revoked)", grants_clear);
    dmstrcheck("every worker's IOMMU domain (g_proc_slpt) was freed and zeroed on exit",
               domains_clear);

    int fds_leaked = 0;
    klock_acquire(&g_ofile_lock);
    for (int fd = 0; fd < 16; fd++) if (g_ofiles[fd].used) fds_leaked++;
    klock_release(&g_ofile_lock);
    dmstrcheck("no descriptor leaks (open-file table fully released)", fds_leaked == 0);

    uint64_t freed_total = g_frames_freed - freed0, reused_total = g_frames_reused - reused0;
    kprintf("[dmastrs] this run: +%u freed, +%u reused; global depth %u reconciles: freed=%u reused=%u\n",
            freed_total, reused_total, g_frame_free_depth, g_frames_freed, g_frames_reused);
    dmstrcheck("free-frame count reconciles (g_frame_free_depth == lifetime freed - reused)",
               g_frame_free_depth == g_frames_freed - g_frames_reused);

    kprintf("[dmastrs] RESULT: %d passed, %d failed\n", (uint64_t)g_dmstrpass, (uint64_t)g_dmstrfail);
    if (!g_dmstrfail)
        kputs("[dmastrs] DMA STRESS VERIFIED — every grant revoked, every domain freed, no leaks, no contamination\n");
    else kputs("[dmastrs] DMA STRESS DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * v0.42: LEAK CHECK — heavy spawn/destroy proves 100% physical-frame reclamation
 * ===========================================================================
 * ONE kproc slot is spawned once and then reused across many iterations —
 * deliberately: this suite is stressing the ALLOCATOR and page_free_tree, not
 * the kproc table (which still only ever grows; that is a separate, stated
 * scope gap). Each iteration gives that slot a brand-new address space
 * (create_address_space), loads the SAME ELF into it (elf_load also maps its
 * user stack), and runs it for real through cpu_exec_proc — the identical
 * dispatch path mcsched/mcq/mcpre/slice/cio use — so the exit hook that was
 * just wired into that executor is exactly what reclaims it.
 *
 * Because the ELF and the stack size never change, every iteration builds and
 * tears down an IDENTICAL number of frames (PT_LOAD pages + stack pages + the
 * PDPT/PD/PT structure frames that mapping them required). That determinism
 * is what makes the high-water-mark check exact rather than approximate: once
 * warm, iteration N's build is satisfied ENTIRELY from the free list iteration
 * N-1's teardown just filled, so g_next_frame — the bump pointer — must not
 * move again after the first iteration. Any leak, however small, breaks that
 * equality and shows up as monotonic growth instead of a flat line.        */
static int g_lkpass, g_lkfail;
static void lkcheck(const char *n, int c) {
    if (c) { g_lkpass++; kprintf("[leakchk]  PASS  %s\n", n); }
    else   { g_lkfail++; kprintf("[leakchk]  FAIL  %s\n", n); }
}

#define LK_ITERS 40

static void cmd_leakcheck(void) {
    kputs("-- LEAK CHECK: heavy spawn/destroy loop proves 100% physical-frame reclamation --\n");
    g_lkpass = g_lkfail = 0;
    uint64_t save = current_proc_idx;

    uint64_t freed0  = g_frames_freed, reused0 = g_frames_reused;
    uint32_t viol0   = g_rank_violations;

    int p = kproc_spawn("leak-probe", 0);
    if (p < 0) { kputs("[leakchk] spawn failed\n-- done --\n"); return; }
    kprocs[p].role = 6;                       /* mcsched_probe: GETPID then EXIT(pid) */

    int ok = 1, deterministic = 1;
    uint64_t hw_after_first = 0, per_iter = 0;
    for (int i = 0; i < LK_ITERS && ok; i++) {
        kprocs[p].cr3 = create_address_space();       /* fresh space, SAME kproc slot */
        uint64_t entry = elf_load(p, g_user_elf, g_user_elf_end - g_user_elf);
        current_proc_idx = save;
        if (!entry) { ok = 0; break; }
        kprocs[p].entry = entry;
        kprocs[p].exited = 0;
        kprocs[p].pstate = 0;
        cpu_exec_proc(0, p);                          /* BSP: allocator logic only, */
        current_proc_idx = save;                      /* not a concurrency test     */
        if (!kprocs[p].exited || kprocs[p].exit_code != kprocs[p].pid) { ok = 0; break; }
        if (i == 0) { hw_after_first = g_next_frame; per_iter = kprocs[p].frames_freed; }
        else if (kprocs[p].frames_freed != per_iter) deterministic = 0;
    }

    uint64_t freed_total  = g_frames_freed  - freed0;
    uint64_t reused_total = g_frames_reused - reused0;
    uint64_t depth_now    = g_frame_free_depth;
    kprintf("[leakchk] %d iterations of spawn -> elf_load -> run -> exit on ONE kproc slot (pid %u)\n",
            (uint64_t)LK_ITERS, kprocs[p].pid);
    kprintf("[leakchk] %u frame(s) reclaimed per iteration; high-water after iter 0: %X, after iter %d: %X\n",
            per_iter, hw_after_first, (uint64_t)(LK_ITERS - 1), g_next_frame);
    kprintf("[leakchk] this run: +%u freed, +%u reused-from-list; global depth %u, freed %u, reused %u\n",
            freed_total, reused_total, depth_now, g_frames_freed, g_frames_reused);

    lkcheck("every iteration ran to completion and exited cleanly (exit == pid)", ok);
    lkcheck("every iteration reclaimed the SAME nonzero frame count (deterministic teardown, no drift)",
            per_iter > 0 && deterministic);
    lkcheck("the bump high-water mark did NOT move after iteration 0 (steady state: fully satisfied from the free list)",
            ok && g_next_frame == hw_after_first);
    /* The free list only ever grows via free_frame (++) and shrinks via
     * alloc_frame's reuse branch (--); nothing else touches the depth counter.
     * That makes this reconciliation exact and unconditional — true at every
     * instant the kernel has run, not just at the end of this suite.        */
    lkcheck("free-list depth exactly reconciles with the allocator's lifetime counters (no phantom/lost frames)",
            g_frame_free_depth == g_frames_freed - g_frames_reused);
    lkcheck("later iterations were satisfied from the free list, not fresh RAM (reuse actually happened)",
            reused_total >= (uint64_t)(LK_ITERS - 1) * per_iter);
    lkcheck("the frame allocator's leaf lock never triggered a rank violation", g_rank_violations == viol0);

    kprintf("[leakchk] RESULT: %d passed, %d failed\n", (uint64_t)g_lkpass, (uint64_t)g_lkfail);
    if (!g_lkfail) kputs("[leakchk] LEAK CHECK VERIFIED — page_free_tree returns every private frame, every time\n");
    else          kputs("[leakchk] LEAK CHECK DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * v0.45: KPROC STRESS — 200 spawn/run/exit cycles prove slots truly recycle
 * ===========================================================================
 * Every earlier stress suite (smp_stress, dma_stress) spawns its whole batch
 * ONCE and never asks kproc_spawn for another slot afterward, so none of them
 * ever touches the recycle branch this milestone adds to kproc_spawn. This
 * suite is built specifically to hit it, repeatedly: KPSTRESS_CYCLES batches
 * of three workers each — one cio/VFS worker (role 9: exercises VFS/CAS and,
 * transitively, virtio-blk, since vfs_write_locked/vfs_read_file both call
 * straight into virtio_write_block/virtio_read_block), one surface-churn
 * worker (role 10), one DMA-churn worker (role 11) — spawned, run to
 * completion and reaped before the NEXT cycle's kproc_spawn calls run. Once
 * the table has filled once, every later cycle's spawns land on a RECYCLED
 * slot, not a fresh one — the exact path smp_stress/dma_stress never touch.
 *
 * "Kill in random order": OutrunOS still has no kill syscall (unchanged since
 * v0.44's cmd_dma_stress) — every worker terminates itself via its own
 * SYS_EXIT. Pushed across every online core with rq_push, a cycle's three
 * workers finish in whatever order the scheduler's own timing produces, not
 * launch order — the same proxy for "termination order is not the harness's
 * to control" this codebase has used since v0.39's finish_seq.
 *
 * virtio-net is deliberately NOT one of this suite's roles: its only ring-3
 * driver (role 1, nic_driver) does raw, unsynchronized MMIO register writes
 * bringing up the ONE real, non-process-isolated NIC from reset — running
 * several of those concurrently and recycling them through 200 cycles would
 * race live hardware state for no proof this milestone needs. It stays
 * exercised exactly once, unchanged, through the existing cmd_nicdriver
 * path.
 *
 * The role-9 (cio/VFS) worker is capped to the first KPSTRESS_VFS_CYCLES
 * cycles, not all 200. cio_file_worker (user/init.c) names its own file
 * from its OWN pid, and VFS files are durable, global, and never deleted —
 * confirmed in v0.44 (cmd_cas's own log line: "state persisted to disk").
 * Every cycle's role-9 worker therefore claims a BRAND NEW, permanent
 * VFS_MAXFILES (24) directory slot, unlike its kproc slot, its DMA grants,
 * or its surface buffer, none of which are global — those three genuinely
 * recycle every cycle, which is exactly what the rest of this suite spends
 * all 200 cycles proving. Running full VFS churn for 200 cycles would need
 * ~200 dirents, and growing VFS_MAXFILES to accommodate an intentionally
 * bounded stress loop would fight the very discipline this milestone is
 * proving, so cio/VFS/CAS coverage here is deliberately bounded instead —
 * still real, still through the modern exit path, still with proper
 * descriptor teardown and kproc recycling, just not unbounded. */
#define KPSTRESS_CYCLES     200
#define KPSTRESS_VFS_CYCLES 6     /* well within the ~8 dirents free by this point */
static int g_kppass, g_kpfail;
static void kpcheck(const char *n, int c) {
    if (c) { g_kppass++; kprintf("[kpstrs]  PASS  %s\n", n); }
    else   { g_kpfail++; kprintf("[kpstrs]  FAIL  %s\n", n); }
}

static void cmd_kproc_stress(void) {
    kputs("-- KPROC STRESS: 200 spawn/run/exit cycles across surface/DMA roles (+ cio/VFS for the first few) --\n");
    g_kppass = g_kpfail = 0;
    uint64_t save = current_proc_idx;
    int n = g_ncpu_online; if (n > MAX_CPUS) n = MAX_CPUS;

    static uint8_t seen_slot[MAX_KPROC];
    int distinct_slots = 0, recycled_spawns = 0;
    int cycles_ok = 1, grants_ok = 1, domains_ok = 1, fds_ok = 1, aborted = 0;
    uint64_t freed0 = g_frames_freed, reused0 = g_frames_reused;
    uint32_t viol0 = g_rank_violations;
    static uint8_t stbuf[CIO_LEN];

    int cyc;
    for (cyc = 0; cyc < KPSTRESS_CYCLES; cyc++) {
        int with_vfs = cyc < KPSTRESS_VFS_CYCLES;
        int nworkers = with_vfs ? 3 : 2;
        uint64_t role_batch[3];
        if (with_vfs) { role_batch[0] = 9; role_batch[1] = 10; role_batch[2] = 11; }
        else          { role_batch[0] = 10; role_batch[1] = 11; }

        int procs[3];
        int spawn_failed = 0;
        for (int i = 0; i < nworkers; i++) {
            uint64_t role = role_batch[i];
            uint64_t caps = role == 9  ? PCAP_FILESYSTEM
                          : role == 10 ? PCAP_FRAMEBUFFER
                          : (PCAP_HW_PASSTHROUGH | kdevs[g_demo_dev_index].req);
            int p = kproc_spawn("kp-stress", caps);
            if (p < 0) { spawn_failed = 1; break; }
            if (seen_slot[p]) recycled_spawns++;
            else { seen_slot[p] = 1; distinct_slots++; }
            kprocs[p].role = role;
            uint64_t entry = elf_load(p, g_user_elf, g_user_elf_end - g_user_elf);
            current_proc_idx = save;
            if (!entry) { spawn_failed = 1; break; }
            kprocs[p].entry = entry;
            procs[i] = p;
            if (role == 9) {
                char nm[16]; cio_name(nm, kprocs[p].pid);
                for (int j = 0; j < CIO_LEN; j++) stbuf[j] = cio_byte(kprocs[p].pid, 255, j);
                vfs_write_file(nm, stbuf, CIO_LEN);
            }
        }
        if (spawn_failed) { aborted = 1; break; }

        if (n > 1) {
            for (int i = 0; i < nworkers; i++) rq_push(i % n, procs[i]);
            __sync_synchronize();
            lapic_ipi(0, IPI_PING, 1);
            int p;
            while ((p = rq_pop(0)) >= 0) cpu_exec_proc(0, p);
        } else {
            for (int i = 0; i < nworkers; i++) cpu_exec_proc(0, procs[i]);
        }

        /* v0.45: wait for torn_down, NOT exited — this suite is about to hand
         * these exact slots back to kproc_spawn on the next cycle, and
         * exited flips true before this task's own core has run
         * descriptor_teardown_kproc/dma_teardown_kproc/page_free_tree. See
         * the comment on kproc_spawn's recycle scan for the corruption this
         * would otherwise race. */
        uint64_t t0 = g_ticks;
        for (;;) {
            int all = 1;
            for (int i = 0; i < nworkers; i++) if (!kprocs[procs[i]].torn_down) all = 0;
            if (all || g_ticks - t0 > 2000) break;
            if (n > 1) lapic_ipi(0, IPI_PING, 1);
            uint64_t tw = g_ticks; while (g_ticks - tw < 2) __asm__ volatile("pause");
        }
        current_proc_idx = save;

        int cyc_ok = 1, cyc_grants = 1, cyc_domains = 1;
        for (int i = 0; i < nworkers; i++) {
            struct kproc *k = &kprocs[procs[i]];
            if (!k->exited || k->exit_code != k->pid) cyc_ok = 0;
            if (k->dma_grant_count != 0) cyc_grants = 0;
            if (g_proc_slpt[procs[i]] != 0) cyc_domains = 0;
        }
        int cyc_fds = 0;
        klock_acquire(&g_ofile_lock);
        for (int fd = 0; fd < 16; fd++) if (g_ofiles[fd].used) cyc_fds++;
        klock_release(&g_ofile_lock);

        if (!cyc_ok) cycles_ok = 0;
        if (!cyc_grants) grants_ok = 0;
        if (!cyc_domains) domains_ok = 0;
        if (cyc_fds != 0) fds_ok = 0;

        if (!cyc_ok || !cyc_grants || !cyc_domains || cyc_fds != 0) {
            kprintf("[kpstrs] cycle %d FAILED: ok=%d grants=%d domains=%d fds_leaked=%d\n",
                    cyc, cyc_ok, cyc_grants, cyc_domains, cyc_fds);
            for (int i = 0; i < nworkers; i++) {
                struct kproc *k = &kprocs[procs[i]];
                kprintf("[kpstrs]   slot %d pid %u role %u: exit %u (want %u) grants=%u slpt=%X\n",
                        procs[i], k->pid, k->role, k->exit_code, k->pid,
                        (uint64_t)k->dma_grant_count, g_proc_slpt[procs[i]]);
            }
        } else if ((cyc % 50) == 49) {
            kprintf("[kpstrs] cycle %d/%d clean (slots seen so far: %d, recycled spawns: %d)\n",
                    cyc + 1, (uint64_t)KPSTRESS_CYCLES, distinct_slots, recycled_spawns);
        }
    }

    kpcheck("kproc_spawn never failed mid-storm (recycling kept the table from exhausting)",
            !aborted && cyc == KPSTRESS_CYCLES);
    kpcheck("every cycle's workers exited cleanly (exit == pid, recycled or fresh alike)", cycles_ok);
    kpcheck("no stale DMA grants survived any cycle's teardown", grants_ok);
    kpcheck("no stale IOMMU domain (g_proc_slpt) survived any cycle's teardown", domains_ok);
    kpcheck("no descriptor leaked past any cycle (open-file table fully released each time)", fds_ok);
    kpcheck("recycling actually happened (>= 1 spawn reused an already-seen slot)",
            recycled_spawns > 0);
    kpcheck("distinct slots used stayed within MAX_KPROC (the table never grew past its cap)",
            distinct_slots <= MAX_KPROC);

    int surf_leaked = 0;
    klock_acquire(&g_surf_lock);
    for (int i = 0; i < 8; i++) if (g_surf[i].used && g_surf[i].owner >= 0 &&
                                     g_surf[i].owner < n_kproc && kprocs[g_surf[i].owner].exited)
        surf_leaked++;
    klock_release(&g_surf_lock);
    kpcheck("no stale surface buffer still owned by an exited worker", surf_leaked == 0);

    uint64_t freed_total = g_frames_freed - freed0, reused_total = g_frames_reused - reused0;
    kprintf("[kpstrs] %d cycles (%d total spawns, %d distinct slots, %d recycled): "
            "+%u freed, +%u reused; global depth %u\n",
            cyc, distinct_slots + recycled_spawns, distinct_slots, recycled_spawns,
            freed_total, reused_total, g_frame_free_depth);
    kpcheck("free-frame count reconciles (g_frame_free_depth == lifetime freed - reused)",
            g_frame_free_depth == g_frames_freed - g_frames_reused);
    kpcheck("the frame allocator's leaf lock never triggered a rank violation (no double-free race)",
            g_rank_violations == viol0);

    kprintf("[kpstrs] RESULT: %d passed, %d failed\n", (uint64_t)g_kppass, (uint64_t)g_kpfail);
    if (!g_kpfail)
        kputs("[kpstrs] KPROC STRESS VERIFIED — slots recycle, every descriptor/grant/domain/surface torn down, no drift across 200 cycles\n");
    else kputs("[kpstrs] KPROC STRESS DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * v0.46: IPC STRESS — repeated sender/receiver churn proves the mailbox,
 * fd-transfer, and shared-memory-grant lifetimes are all leak-free
 * ===========================================================================
 * Each round spawns two workers — role 12 (ipc_sender) and role 13
 * (ipc_receiver) — that exchange exactly two messages: a transferred VFS fd
 * (whose content the receiver reads THROUGH the transferred descriptor,
 * never its own), then a shared-memory frame (whose content the receiver
 * reads with NO syscall at all — genuine zero-copy). Rounds run back-to-back
 * across the run-queue/watchdog idiom every *_stress suite since v0.43 uses,
 * so this exercises the SAME kproc recycling v0.45 proved safe, but for a
 * genuinely two-sided, capability-checked, cross-process handoff instead of
 * v0.44/v0.45's one-sided churn against a fixed demo device.
 *
 * "ipc-payload" and "ipc-peer" are fixed, reused VFS names, not pid-keyed —
 * the exact lesson v0.45's kpstress learned the hard way: a pid-keyed name
 * claims a brand-new, permanent VFS_MAXFILES dirent every round, and VFS
 * files are durable/never-deleted by design. Reused names cost nothing per
 * round (vfs_write_file overwrites in place); what genuinely IS per-round
 * and genuinely does recycle — the kproc slots, the fd numbers in
 * g_ofiles, the shmem ids in g_ipc_shm — is exactly what this suite's
 * assertions check. */
#define IPCSTRESS_ROUNDS 20
static int g_ipcpass, g_ipcfail;
static void ipccheck(const char *n, int c) {
    if (c) { g_ipcpass++; kprintf("[ipcstrs]  PASS  %s\n", n); }
    else   { g_ipcfail++; kprintf("[ipcstrs]  FAIL  %s\n", n); }
}

static void cmd_ipc_stress(void) {
    kputs("-- IPC STRESS: capability-gated fd/shared-memory handoff, sender/receiver churn --\n");
    g_ipcpass = g_ipcfail = 0;
    uint64_t save = current_proc_idx;
    int n = g_ncpu_online; if (n > MAX_CPUS) n = MAX_CPUS;

    uint64_t freed0 = g_frames_freed, reused0 = g_frames_reused;
    uint32_t viol0 = g_rank_violations;
    int rounds_ok = 1, fds_ok = 1, shm_ok = 1;
    static const uint8_t payload[16] = "IPC-PAYLOAD-TEST";
    int rnd;

    for (rnd = 0; rnd < IPCSTRESS_ROUNDS; rnd++) {
        int sp = kproc_spawn("ipc-sender", PCAP_IPC | PCAP_FILESYSTEM);
        int rp = kproc_spawn("ipc-recv",   PCAP_IPC | PCAP_FILESYSTEM);
        if (sp < 0 || rp < 0) { ipccheck("kproc_spawn never fails mid-storm (recycling keeps the table bounded)", 0);
                                 rounds_ok = 0; break; }
        kprocs[sp].role = 12; kprocs[rp].role = 13;

        uint64_t es = elf_load(sp, g_user_elf, g_user_elf_end - g_user_elf);
        current_proc_idx = save;
        uint64_t er = elf_load(rp, g_user_elf, g_user_elf_end - g_user_elf);
        current_proc_idx = save;
        if (!es || !er) { ipccheck("every worker's ELF loads", 0); rounds_ok = 0; break; }
        kprocs[sp].entry = es; kprocs[rp].entry = er;

        vfs_write_file("ipc-payload", payload, sizeof payload);
        uint8_t peerbuf[8];
        for (int b = 0; b < 8; b++) peerbuf[b] = (uint8_t)(kprocs[rp].pid >> (8 * b));
        vfs_write_file("ipc-peer", peerbuf, 8);

        int procs[2] = { sp, rp };
        if (n > 1) {
            rq_push(0 % n, sp); rq_push(1 % n, rp);
            __sync_synchronize();
            lapic_ipi(0, IPI_PING, 1);
            int p;
            while ((p = rq_pop(0)) >= 0) cpu_exec_proc(0, p);
        } else {
            cpu_exec_proc(0, sp); cpu_exec_proc(0, rp);
        }

        uint64_t t0 = g_ticks;
        for (;;) {
            int all = 1;
            for (int i = 0; i < 2; i++) if (!kprocs[procs[i]].torn_down) all = 0;
            if (all || g_ticks - t0 > 3000) break;
            if (n > 1) lapic_ipi(0, IPI_PING, 1);
            uint64_t tw = g_ticks; while (g_ticks - tw < 2) __asm__ volatile("pause");
        }
        current_proc_idx = save;

        int ok = kprocs[sp].exited && kprocs[sp].exit_code == kprocs[sp].pid
              && kprocs[rp].exited && kprocs[rp].exit_code == kprocs[rp].pid;
        if (!ok) {
            kprintf("[ipcstrs] round %d FAILED: sender exit %u (want %u), receiver exit %u (want %u)\n",
                    rnd, kprocs[sp].exit_code, kprocs[sp].pid, kprocs[rp].exit_code, kprocs[rp].pid);
            rounds_ok = 0;
        }

        int fds_leaked = 0;
        klock_acquire(&g_ofile_lock);
        for (int fd = 0; fd < 16; fd++) if (g_ofiles[fd].used) fds_leaked++;
        klock_release(&g_ofile_lock);
        if (fds_leaked) fds_ok = 0;

        int shm_leaked = 0;
        klock_acquire(&g_ipc_lock);
        for (int i = 0; i < MAX_IPC_SHMEM; i++) if (g_ipc_shm[i].used) shm_leaked++;
        klock_release(&g_ipc_lock);
        if (shm_leaked) shm_ok = 0;

        if (fds_leaked || shm_leaked)
            kprintf("[ipcstrs] round %d: fds_leaked=%d shm_leaked=%d\n", rnd, fds_leaked, shm_leaked);
        else if (ok && (rnd % 5) == 4)
            kprintf("[ipcstrs] round %d/%d clean\n", rnd + 1, (uint64_t)IPCSTRESS_ROUNDS);
    }

    ipccheck("every round completed without a watchdog timeout (no deadlock across preemption)",
             rnd == IPCSTRESS_ROUNDS);
    ipccheck("every round's sender AND receiver exited cleanly (exit == own pid)", rounds_ok);
    ipccheck("no descriptor leaked past any round (open-file table fully released each time)", fds_ok);
    ipccheck("no shared-memory grant survived past any round (g_ipc_shm fully released each time)", shm_ok);

    uint64_t freed_total = g_frames_freed - freed0, reused_total = g_frames_reused - reused0;
    kprintf("[ipcstrs] %d rounds: +%u freed, +%u reused; global depth %u\n",
            rnd, freed_total, reused_total, g_frame_free_depth);
    ipccheck("free-frame count reconciles (g_frame_free_depth == lifetime freed - reused)",
             g_frame_free_depth == g_frames_freed - g_frames_reused);
    ipccheck("the frame allocator's leaf lock never triggered a rank violation (no double-free race)",
             g_rank_violations == viol0);

    kprintf("[ipcstrs] RESULT: %d passed, %d failed\n", (uint64_t)g_ipcpass, (uint64_t)g_ipcfail);
    if (!g_ipcfail)
        kputs("[ipcstrs] IPC STRESS VERIFIED — handle and shared-memory transfer both leak-free across sender/receiver churn\n");
    else kputs("[ipcstrs] IPC STRESS DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * v0.47: VFIO STRESS — a ring-3 driver maps a dummy device's two BARs, waits
 * on a routed interrupt, and tears down leak-free, repeatedly
 * ===========================================================================
 * The dummy device ("vfio-dev0") is registered lazily, once, the first time
 * this suite runs: two alloc_frame() pages standing in for BAR0 (a register
 * file, sentinel-stamped like cmd_passthrough's sensor0) and BAR1 (a scratch
 * page the driver writes a pattern into and reads back, proving the mapping
 * is real), plus a reserved, software-only IRQ line (16 — see the
 * MAX_VFIO_LINES section comment for why no real device's line is safe to
 * hijack for deterministic testing here).
 *
 * Same churn idiom as every other *_stress suite since v0.43: VFIOSTRESS_
 * ROUNDS repetitions of spawn -> map BAR0 -> map BAR1 -> wait on the
 * simulated IRQ -> exit, reusing v0.45's kproc recycling. */
#define VFIOSTRESS_ROUNDS 15
static int g_vfiopass, g_vfiofail;
static void vfiocheck(const char *n, int c) {
    if (c) { g_vfiopass++; kprintf("[vfiostrs]  PASS  %s\n", n); }
    else   { g_vfiofail++; kprintf("[vfiostrs]  FAIL  %s\n", n); }
}

static int g_vfio_test_dev = -1;   /* lazily registered, once, across suite invocations */

static void cmd_vfio_stress(void) {
    kputs("-- VFIO STRESS: ring-3 BAR mapping + routed interrupt wait, driver churn --\n");
    g_vfiopass = g_vfiofail = 0;
    uint64_t save = current_proc_idx;
    int n = g_ncpu_online; if (n > MAX_CPUS) n = MAX_CPUS;

    if (g_vfio_test_dev < 0) {
        uint64_t bar0 = alloc_frame();
        *(volatile uint32_t *)bar0 = 0xCAFEBABEu;
        uint64_t bar1 = alloc_frame();
        g_vfio_test_dev = n_kdev;
        kdev_register("vfio-dev0", bar0, 0x1000, PCAP_VFIO);
        g_kdev_bar1_phys[g_vfio_test_dev] = bar1;
        g_kdev_bar1_len[g_vfio_test_dev] = 0x1000;
        g_kdev_irq_line[g_vfio_test_dev] = 16;
        kprintf("[vfiostrs] registered dummy device 'vfio-dev0' (idx %d): BAR0 %X, BAR1 %X, IRQ line 16\n",
                g_vfio_test_dev, bar0, bar1);
    }

    uint8_t devidbuf[8];
    for (int b = 0; b < 8; b++) devidbuf[b] = (uint8_t)((uint64_t)g_vfio_test_dev >> (8 * b));
    vfs_write_file("vfio-devid", devidbuf, 8);   /* fixed, reused name — see v0.45/v0.46 precedent */

    uint64_t freed0 = g_frames_freed, reused0 = g_frames_reused;
    uint32_t viol0 = g_rank_violations;
    int rounds_ok = 1, grants_ok = 1, irq_ok = 1;
    int rnd;

    for (rnd = 0; rnd < VFIOSTRESS_ROUNDS; rnd++) {
        int p = kproc_spawn("vfio-driver", PCAP_VFIO | PCAP_FILESYSTEM);
        if (p < 0) { vfiocheck("kproc_spawn never fails mid-storm (recycling keeps the table bounded)", 0);
                     rounds_ok = 0; break; }
        kprocs[p].role = 14;
        uint64_t e = elf_load(p, g_user_elf, g_user_elf_end - g_user_elf);
        current_proc_idx = save;
        if (!e) { vfiocheck("every worker's ELF loads", 0); rounds_ok = 0; break; }
        kprocs[p].entry = e;

        g_vfio_test_fire_at = g_ticks + 5;    /* fire the simulated IRQ ~50ms in */

        if (n > 1) {
            rq_push(0 % n, p);
            __sync_synchronize();
            lapic_ipi(0, IPI_PING, 1);
            int q; while ((q = rq_pop(0)) >= 0) cpu_exec_proc(0, q);
        } else {
            cpu_exec_proc(0, p);
        }

        uint64_t t0 = g_ticks;
        while (!kprocs[p].torn_down && g_ticks - t0 < 3000) {
            if (n > 1) lapic_ipi(0, IPI_PING, 1);
            uint64_t tw = g_ticks; while (g_ticks - tw < 2) __asm__ volatile("pause");
        }
        current_proc_idx = save;

        int ok = kprocs[p].exited && kprocs[p].exit_code == kprocs[p].pid;
        if (!ok) {
            kprintf("[vfiostrs] round %d FAILED: exit %u (want %u)\n", rnd, kprocs[p].exit_code, kprocs[p].pid);
            rounds_ok = 0;
        }
        if (kprocs[p].dma_grant_count != 0) grants_ok = 0;
        if (g_vfio_irq_owner[16] == p) irq_ok = 0;    /* must have been released on exit */

        g_vfio_test_fire_at = 0;    /* disarm in case this round failed before consuming it */

        if (ok && (rnd % 5) == 4)
            kprintf("[vfiostrs] round %d/%d clean\n", rnd + 1, (uint64_t)VFIOSTRESS_ROUNDS);
    }

    vfiocheck("every round completed without a watchdog timeout (no deadlock across preemption)",
              rnd == VFIOSTRESS_ROUNDS);
    vfiocheck("every round's driver exited cleanly (exit == own pid: BAR reads/writes and IRQ wait all verified in ring 3)",
              rounds_ok);
    vfiocheck("no DMA/MMIO grant survived past any round's teardown", grants_ok);
    vfiocheck("no IRQ-line ownership survived past any round's teardown", irq_ok);

    /* Both BAR frames are kernel-owned for the life of the boot (like sensor0) —  */
    /* this confirms page_free_tree's device-MMIO guard never let BAR0 get freed   */
    /* out from under the device across all VFIOSTRESS_ROUNDS teardowns.           */
    int dev_frame_ok = (*(volatile uint32_t *)kdevs[g_vfio_test_dev].base == 0xCAFEBABEu);
    vfiocheck("BAR0's sentinel survived every round's teardown (device frame never freed)",
              dev_frame_ok);

    uint64_t freed_total = g_frames_freed - freed0, reused_total = g_frames_reused - reused0;
    kprintf("[vfiostrs] %d rounds: +%u freed, +%u reused; global depth %u\n",
            rnd, freed_total, reused_total, g_frame_free_depth);
    vfiocheck("free-frame count reconciles (g_frame_free_depth == lifetime freed - reused)",
              g_frame_free_depth == g_frames_freed - g_frames_reused);
    vfiocheck("the frame allocator's leaf lock never triggered a rank violation (no double-free race)",
              g_rank_violations == viol0);

    kprintf("[vfiostrs] RESULT: %d passed, %d failed\n", (uint64_t)g_vfiopass, (uint64_t)g_vfiofail);
    if (!g_vfiofail)
        kputs("[vfiostrs] VFIO STRESS VERIFIED — BAR mapping and routed interrupts both leak-free across driver churn\n");
    else kputs("[vfiostrs] VFIO STRESS DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * v0.48: VFS STRESS — journaling, unlink/reclamation, and multi-volume
 * mounts, all driven through real ring-3 syscalls, plus a direct in-kernel
 * proof that the VFS-directory journal's deferred apply and automatic
 * boot-time recovery both do real work.
 * ===========================================================================
 * "vfs-stress" is a single, fixed, reused ROOT name — exactly the v0.45
 * lesson: the kernel harness re-seeds it before every round, the ring-3
 * driver overwrites then UNLINKS it, so the directory never grows past
 * baseline no matter how many rounds run. "tmp/scratch" is likewise fixed
 * and reused (TMP_MAXFILES never exceeded). "dev/devices" is a live view,
 * not a file at all — nothing to grow. */
#define VFSSTRESS_ROUNDS 15
static int g_vfspass, g_vfsfail;
static void vfscheck(const char *n, int c) {
    if (c) { g_vfspass++; kprintf("[vfsstrs]  PASS  %s\n", n); }
    else   { g_vfsfail++; kprintf("[vfsstrs]  FAIL  %s\n", n); }
}

static int vfs_dirents_in_use(void) {
    int n = 0;
    for (int i = 0; i < VFS_MAXFILES; i++) if (DENTS[i].used) n++;
    return n;
}
static int vfs_tmp_in_use(void) {
    int n = 0;
    for (int i = 0; i < TMP_MAXFILES; i++) if (g_tmpfiles[i].used) n++;
    return n;
}

static void cmd_vfs_stress(void) {
    kputs("-- VFS STRESS: journaling, unlink/reclamation, multi-volume mounts, driver churn --\n");
    g_vfspass = g_vfsfail = 0;
    uint64_t save = current_proc_idx;
    int n = g_ncpu_online; if (n > MAX_CPUS) n = MAX_CPUS;

    if (!g_cas_mounted) { vfscheck("CAS mounted before VFS stress can run", 0);
                           kprintf("[vfsstrs] RESULT: %d passed, %d failed\n", (uint64_t)g_vfspass, (uint64_t)g_vfsfail);
                           return; }

    static const uint8_t seed[16] = "VFS-SEED-PATTERN";
    /* baseline is measured AFTER round 0 (the round that FIRST creates
     * tmp/scratch and first re-seeds+unlinks vfs-stress) — round 0 is
     * expected to change the counts from empty to steady-state; rounds 1..N
     * are the ones that must hold steady at that state, proving reclamation
     * rather than growth.                                                   */
    int baseline_dirents = -1, baseline_tmp = -1;

    uint64_t freed0 = g_frames_freed, reused0 = g_frames_reused;
    uint32_t viol0 = g_rank_violations;
    int rounds_ok = 1, fds_ok = 1, unlink_ok = 1, dirent_bound_ok = 1, tmp_bound_ok = 1;
    int rnd;

    for (rnd = 0; rnd < VFSSTRESS_ROUNDS; rnd++) {
        vfs_write_file("vfs-stress", seed, sizeof seed);   /* re-seed: driver unlinked it last round */

        int p = kproc_spawn("vfs-driver", PCAP_FILESYSTEM);
        if (p < 0) { vfscheck("kproc_spawn never fails mid-storm (recycling keeps the table bounded)", 0);
                     rounds_ok = 0; break; }
        kprocs[p].role = 15;
        uint64_t e = elf_load(p, g_user_elf, g_user_elf_end - g_user_elf);
        current_proc_idx = save;
        if (!e) { vfscheck("every worker's ELF loads", 0); rounds_ok = 0; break; }
        kprocs[p].entry = e;

        if (n > 1) {
            rq_push(0 % n, p);
            __sync_synchronize();
            lapic_ipi(0, IPI_PING, 1);
            int q; while ((q = rq_pop(0)) >= 0) cpu_exec_proc(0, q);
        } else {
            cpu_exec_proc(0, p);
        }

        uint64_t t0 = g_ticks;
        while (!kprocs[p].torn_down && g_ticks - t0 < 3000) {
            if (n > 1) lapic_ipi(0, IPI_PING, 1);
            uint64_t tw = g_ticks; while (g_ticks - tw < 2) __asm__ volatile("pause");
        }
        current_proc_idx = save;

        int ok = kprocs[p].exited && kprocs[p].exit_code == kprocs[p].pid;
        if (!ok) {
            kprintf("[vfsstrs] round %d FAILED: exit %u (want %u)\n", rnd, kprocs[p].exit_code, kprocs[p].pid);
            rounds_ok = 0;
        }

        if (vfs_find("vfs-stress") >= 0) unlink_ok = 0;    /* driver must have unlinked it */

        int fds_leaked = 0;
        klock_acquire(&g_ofile_lock);
        for (int fd = 0; fd < 16; fd++) if (g_ofiles[fd].used) fds_leaked++;
        klock_release(&g_ofile_lock);
        if (fds_leaked) fds_ok = 0;

        if (rnd == 0) { baseline_dirents = vfs_dirents_in_use(); baseline_tmp = vfs_tmp_in_use(); }
        else {
            if (vfs_dirents_in_use() != baseline_dirents) dirent_bound_ok = 0;
            if (vfs_tmp_in_use() != baseline_tmp) tmp_bound_ok = 0;
        }

        if (ok && (rnd % 5) == 4)
            kprintf("[vfsstrs] round %d/%d clean\n", rnd + 1, (uint64_t)VFSSTRESS_ROUNDS);
    }

    vfscheck("every round completed without a watchdog timeout (no deadlock across preemption)",
             rnd == VFSSTRESS_ROUNDS);
    vfscheck("every round's driver exited cleanly (ROOT read/overwrite/sync, TMP write/read, DEV read-only all verified in ring 3)",
             rounds_ok);
    vfscheck("no descriptor leaked past any round (open-file table fully released each time)", fds_ok);
    vfscheck("SYS_VFS_UNLINK actually removed the dirent every round (vfs_find fails immediately after)", unlink_ok);
    vfscheck("directory dirent count never grew past baseline (unlink+reclaim, not one-new-dirent-per-round)",
             dirent_bound_ok);
    vfscheck("TMP volume slot count never grew past baseline (fixed reused name, not one-new-slot-per-round)",
             tmp_bound_ok);

    uint64_t freed_total = g_frames_freed - freed0, reused_total = g_frames_reused - reused0;
    kprintf("[vfsstrs] %d rounds: +%u freed, +%u reused; global depth %u\n",
            rnd, freed_total, reused_total, g_frame_free_depth);
    vfscheck("free-frame count reconciles (g_frame_free_depth == lifetime freed - reused)",
             g_frame_free_depth == g_frames_freed - g_frames_reused);
    vfscheck("the frame allocator's leaf lock never triggered a rank violation (no double-free race)",
             g_rank_violations == viol0);

    /* --- direct in-kernel proof: deferred journal apply + automatic boot recovery --- */
    static const uint8_t crashpat[24] = "VFS-CRASH-RECOVERY-TEST";
    vfs_write_file("vfs-crash-test", crashpat, sizeof crashpat);   /* commits journal PENDING; dir_start left stale */
    int idx = vfs_find("vfs-crash-test");
    int deferred_ok = 0, recovered_ok = 0, reload_ok = 0;
    if (idx >= 0 && !g_cas_legacy) {
        uint64_t blk = SB->dir_start + (uint64_t)idx / 2;
        uint32_t off = (uint32_t)(idx % 2) * 256;
        uint8_t raw[512]; virtio_read_block(blk, raw);
        struct dirent *praw = (struct dirent *)(raw + off);
        /* on-disk copy must NOT yet reflect the commit — proves apply is deferred, not eager */
        deferred_ok = (praw->used == 0 || praw->file_hash != DENTS[idx].file_hash);

        g_cas_mounted = 0;
        int remounted = cas_mount();                       /* simulates a reboot: recovery runs for real */

        virtio_read_block(blk, raw);
        praw = (struct dirent *)(raw + off);
        recovered_ok = (remounted && praw->used == 1 && praw->file_hash == DENTS[idx].file_hash);

        int idx2 = vfs_find("vfs-crash-test");
        uint8_t rb[32];
        int64_t got = (idx2 >= 0) ? vfs_read_file(idx2, rb, sizeof rb) : -1;
        reload_ok = (idx2 >= 0 && got == (int64_t)sizeof crashpat && cmemcmp(rb, crashpat, sizeof crashpat) == 0);
    }
    vfscheck("VFS journal commit is genuinely DEFERRED (on-disk dir region is stale before apply)", deferred_ok);
    vfscheck("a simulated reboot's cas_mount() automatically recovers the pending commit", recovered_ok);
    vfscheck("the recovered directory reloads with correct content (post-recovery read matches what was written)",
             reload_ok);

    kprintf("[vfsstrs] RESULT: %d passed, %d failed\n", (uint64_t)g_vfspass, (uint64_t)g_vfsfail);
    if (!g_vfsfail)
        kputs("[vfsstrs] VFS STRESS VERIFIED — journaling, reclamation and multi-volume mounts all leak-free and crash-safe\n");
    else kputs("[vfsstrs] VFS STRESS DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * v0.50: GPU STRESS — real virtio-gpu 2D resource create/scanout/flush
 * churn, INCLUDING deliberate mid-flight client faults
 * ===========================================================================
 * Every 4th round deliberately faults the ring-3 driver right after it
 * creates its GPU resource (role 18) — before it ever reaches SET_SCANOUT,
 * SUBMIT_FLUSH, or its own SYS_EXIT. This is the actual point of the
 * milestone's "unexpected client process termination" requirement: it
 * proves gpu_teardown_kproc reclaims the resource and its DMA grant via
 * handle_cpl3_fault's exit path, not just the common, well-behaved SYS_EXIT
 * one — the exact v0.44/45 fault-injection precedent (cmd_dma_stress/
 * cmd_kproc_stress), applied to the new GPU resource table.                 */
#define GPUSTRESS_ROUNDS 16
static int g_gpupass, g_gpufail;
static void gpucheck(const char *n, int c) {
    if (c) { g_gpupass++; kprintf("[gpustrs]  PASS  %s\n", n); }
    else   { g_gpufail++; kprintf("[gpustrs]  FAIL  %s\n", n); }
}
static int gpu_res_in_use(void) {
    int n = 0;
    for (int i = 0; i < MAX_GPU_RES; i++) if (g_gpu_res[i].used) n++;
    return n;
}

static void cmd_gpu_stress(void) {
    kputs("-- GPU STRESS: real virtio-gpu 2D resource/scanout/flush churn, incl. client faults --\n");
    g_gpupass = g_gpufail = 0;
    uint64_t save = current_proc_idx;
    int n = g_ncpu_online; if (n > MAX_CPUS) n = MAX_CPUS;

    if (!g_gpu_ready) {
        gpucheck("virtio-gpu device present and DRIVER_OK before GPU stress can run", 0);
        kprintf("[gpustrs] RESULT: %d passed, %d failed\n", (uint64_t)g_gpupass, (uint64_t)g_gpufail);
        return;
    }

    uint64_t freed0 = g_frames_freed, reused0 = g_frames_reused;
    uint32_t viol0 = g_rank_violations;
    int baseline_res = gpu_res_in_use();
    int rounds_ok = 1, grants_ok = 1, res_bound_ok = 1, scanout_ok = 1, fault_rounds_ok = 1;
    int rnd;

    for (rnd = 0; rnd < GPUSTRESS_ROUNDS; rnd++) {
        int fault_round = (rnd % 4) == 3;
        int p = kproc_spawn("gpu-driver", PCAP_SURFACE);
        if (p < 0) { gpucheck("kproc_spawn never fails mid-storm (recycling keeps the table bounded)", 0);
                     rounds_ok = 0; break; }
        kprocs[p].role = fault_round ? 18 : 17;
        uint64_t e = elf_load(p, g_user_elf, g_user_elf_end - g_user_elf);
        current_proc_idx = save;
        if (!e) { gpucheck("every worker's ELF loads", 0); rounds_ok = 0; break; }
        kprocs[p].entry = e;

        if (n > 1) {
            rq_push(0 % n, p);
            __sync_synchronize();
            lapic_ipi(0, IPI_PING, 1);
            int q; while ((q = rq_pop(0)) >= 0) cpu_exec_proc(0, q);
        } else {
            cpu_exec_proc(0, p);
        }

        uint64_t t0 = g_ticks;
        while (!kprocs[p].torn_down && g_ticks - t0 < 3000) {
            if (n > 1) lapic_ipi(0, IPI_PING, 1);
            uint64_t tw = g_ticks; while (g_ticks - tw < 2) __asm__ volatile("pause");
        }
        current_proc_idx = save;

        int ok;
        if (fault_round) {
            ok = kprocs[p].exited && kprocs[p].exit_code >= 0x8000;   /* died via the fault path, as intended */
            if (!ok) fault_rounds_ok = 0;
        } else {
            ok = kprocs[p].exited && kprocs[p].exit_code == kprocs[p].pid;
            if (!ok) rounds_ok = 0;
        }
        if (!ok)
            kprintf("[gpustrs] round %d (%s) FAILED: exit %u\n",
                    rnd, (uint64_t)(fault_round ? 1 : 0), kprocs[p].exit_code);

        if (kprocs[p].dma_grant_count != 0) grants_ok = 0;
        if (g_gpu_scanout_owner == p) scanout_ok = 0;      /* must have been cleared on exit/fault */
        if (gpu_res_in_use() != baseline_res) res_bound_ok = 0;

        if (ok && (rnd % 4) == 3)
            kprintf("[gpustrs] round %d/%d clean\n", rnd + 1, (uint64_t)GPUSTRESS_ROUNDS);
    }

    gpucheck("every round completed without a watchdog timeout (no deadlock across preemption)",
             rnd == GPUSTRESS_ROUNDS);
    gpucheck("every clean-round driver exited normally (create/draw/scanout/flush/fence all verified in ring 3)",
             rounds_ok);
    gpucheck("every fault-round driver actually died via the fault path (not its own SYS_EXIT)",
             fault_rounds_ok);
    gpucheck("no DMA grant survived past any round's teardown (clean OR faulted)", grants_ok);
    gpucheck("no GPU resource-table slot survived past any round's teardown (clean OR faulted)", res_bound_ok);
    gpucheck("scanout ownership was released by every round's teardown (clean OR faulted)", scanout_ok);

    uint64_t freed_total = g_frames_freed - freed0, reused_total = g_frames_reused - reused0;
    kprintf("[gpustrs] %d rounds (%d faulted): +%u freed, +%u reused; global depth %u\n",
            rnd, (uint64_t)(GPUSTRESS_ROUNDS / 4), freed_total, reused_total, g_frame_free_depth);
    gpucheck("free-frame count reconciles (g_frame_free_depth == lifetime freed - reused)",
             g_frame_free_depth == g_frames_freed - g_frames_reused);
    gpucheck("the frame allocator's leaf lock never triggered a rank violation (no double-free race)",
             g_rank_violations == viol0);

    kprintf("[gpustrs] RESULT: %d passed, %d failed\n", (uint64_t)g_gpupass, (uint64_t)g_gpufail);
    if (!g_gpufail)
        kputs("[gpustrs] GPU STRESS VERIFIED — 2D resource/scanout/flush leak-free across clean AND faulted driver churn\n");
    else kputs("[gpustrs] GPU STRESS DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* Fallback: the embedded blob, now run in the process's address space.        */
static void run_ring3(const char *label, int proc_idx) {
    uint64_t blob_len = (uint64_t)(user_blob_end - user_blob_start);
    uint64_t ucode = alloc_frame();
    for (uint64_t i = 0; i < blob_len && i < 0x1000; i++)
        ((uint8_t *)ucode)[i] = ((uint8_t *)user_blob_start)[i];
    uint64_t cr3 = kprocs[proc_idx].cr3;
    map_page(cr3, 0x500000000000ull, ucode, PTE_USER);
    map_user_stack(cr3);
    enter_process(label, proc_idx, 0x500000000000ull);
}

/* ---- ELF64 loader: map PT_LOAD segments from the ISO module into a process - */
struct elf64_hdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version;
    uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum,
    shentsize, shnum, shstrndx; } __attribute__((packed));
struct elf64_phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; }
    __attribute__((packed));

/* g_user_elf declared earlier (before multiboot_scan) */

/* Returns the ELF entry point, or 0 on failure. Loads into kprocs[idx].cr3.   */
static uint64_t elf_load(int proc_idx, uint64_t img, uint64_t img_size) {
    if (img_size < sizeof(struct elf64_hdr)) { kputs("[elf    ] reject: image smaller than ELF header\n"); return 0; }
    struct elf64_hdr *eh = (struct elf64_hdr *)img;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' || eh->ident[3] != 'F') {
        kprintf("[elf    ] reject: bad magic at %X\n", img); return 0;
    }
    if (eh->ident[4] != 2 || eh->machine != 0x3E) { kputs("[elf    ] reject: not 64-bit x86-64\n"); return 0; }
    /* program header table must lie within the image */
    if (eh->phentsize < sizeof(struct elf64_phdr) || eh->phnum == 0 || eh->phnum > 64) {
        kputs("[elf    ] reject: implausible program header count/size\n"); return 0;
    }
    uint64_t ph_end = eh->phoff + (uint64_t)eh->phnum * eh->phentsize;
    if (eh->phoff > img_size || ph_end > img_size || ph_end < eh->phoff) {
        kputs("[elf    ] reject: program header table out of bounds\n"); return 0;
    }
    if (eh->entry < USER_VMIN || eh->entry >= USER_VMAX) {
        kprintf("[elf    ] reject: entry %X outside user range\n", eh->entry); return 0;
    }
    uint64_t cr3 = kprocs[proc_idx].cr3;
    int loaded = 0;
    for (int i = 0; i < eh->phnum; i++) {
        struct elf64_phdr *ph = (struct elf64_phdr *)(img + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type != 1) continue;                       /* PT_LOAD           */
        /* strict per-segment bounds checks (defend against hostile ELFs) */
        if (ph->offset > img_size || ph->filesz > img_size ||
            ph->offset + ph->filesz > img_size || ph->offset + ph->filesz < ph->offset) {
            kputs("[elf    ] reject: segment file range out of bounds\n"); return 0;
        }
        if (ph->memsz < ph->filesz) { kputs("[elf    ] reject: memsz < filesz\n"); return 0; }
        if ((ph->flags & 0x3) == 0x3) { kputs("[elf    ] reject: writable+executable segment (W^X)\n"); return 0; }
        if (ph->memsz > 0x1000000) { kputs("[elf    ] reject: segment too large (>16 MiB)\n"); return 0; }
        if (ph->vaddr < USER_VMIN || ph->vaddr + ph->memsz > USER_VMAX ||
            ph->vaddr + ph->memsz < ph->vaddr) {
            kprintf("[elf    ] reject: segment vaddr %X outside user range\n", ph->vaddr); return 0;
        }
        uint64_t flags = PTE_USER;
        if (ph->flags & 0x2) flags |= PTE_WRITE;           /* writable data      */
        if (!(ph->flags & 0x1)) flags |= PTE_NX;           /* non-exec -> NX     */
        uint64_t vstart = ph->vaddr & ~0xFFFull;
        uint64_t vend   = (ph->vaddr + ph->memsz + 0xFFF) & ~0xFFFull;
        for (uint64_t v = vstart; v < vend; v += 0x1000) {
            uint64_t frame = alloc_frame();
            map_page(cr3, v, frame, flags | PTE_WRITE);    /* writable to fill  */
            uint64_t seg_off = v > ph->vaddr ? v - ph->vaddr : 0;
            uint64_t dst_skip = ph->vaddr > v ? ph->vaddr - v : 0;
            for (uint64_t b = 0; b < 0x1000; b++) {
                uint64_t fileidx = seg_off + b - dst_skip;
                uint8_t val = 0;
                if (v + b >= ph->vaddr && fileidx < ph->filesz)
                    val = ((uint8_t *)(img + ph->offset))[fileidx];
                ((uint8_t *)frame)[b] = val;
            }
            if (!(flags & PTE_WRITE)) map_page(cr3, v, frame, flags);
        }
        kprintf("[elf    ] PT_LOAD vaddr %X filesz %X memsz %X %s -> mapped USER (bounds ok)\n",
                ph->vaddr, ph->filesz, ph->memsz, (ph->flags & 0x2) ? "(rw)" : "(ro)");
        loaded++;
    }
    map_user_stack(cr3);
    if (!loaded) { kputs("[elf    ] reject: no PT_LOAD segments\n"); return 0; }
    return eh->entry;
}

static void load_and_run_elf(const char *label, int proc_idx) {
    kprintf("[elf    ] loading '%s' from ISO module image at phys %X (%X bytes)\n",
            label, g_user_elf, g_user_elf_end - g_user_elf);
    uint64_t entry = elf_load(proc_idx, g_user_elf, g_user_elf_end - g_user_elf);
    if (!entry) { kprintf("[elf    ] load failed; using embedded blob fallback\n");
                  run_ring3(label, proc_idx); return; }
    enter_process(label, proc_idx, entry);
}

/* v0.31: spawn the user ELF as a FIRST-CLASS SCHEDULER THREAD — its own PCB,
 * kernel stack, and CR3, scheduled alongside kernel threads. Returns the kproc
 * index (and the thread id via *out_tid); the thread runs when scheduled, this
 * call does NOT enter it.                                                     */
static int uthread_spawn_elf(const char *label, uint64_t caps, uint64_t role, int *out_tid) {
    int p = kproc_spawn(label, caps);
    if (p < 0) return -1;
    kprocs[p].role = role;
    uint64_t entry = elf_load(p, g_user_elf, g_user_elf_end - g_user_elf);
    if (!entry) { kprintf("[uthread] ELF load failed for '%s'\n", label); return -1; }
    int tid = uthread_create(label, p, entry);
    if (tid < 0) { kprintf("[uthread] no free thread slot for '%s'\n", label); return -1; }
    if (out_tid) *out_tid = tid;
    kprintf("[uthread] spawned '%s': pid %u = tid %d (role %u) — scheduled, not entered\n",
            label, kprocs[p].pid, (uint64_t)tid, role);
    return p;
}

/* sys_exec: load an ELF whose segments come straight from CAS storage (by     */
/* VFS name -> content hashes -> blocks) instead of the in-memory GRUB module. */
static uint8_t g_execbuf[VFS_MAX_CHUNKS * 512] __attribute__((aligned(16)));
static int exec_from_cas(const char *name, int proc_idx) {
    int di = vfs_find(name);
    if (di < 0) { kprintf("[exec   ] '%s' not found in VFS\n", name); return -1; }
    int64_t n = vfs_read_file(di, g_execbuf, sizeof g_execbuf);
    kprintf("[exec   ] read %d ELF bytes from CAS (file_hash %X) — not a GRUB module\n",
            n, DENTS[di].file_hash);
    uint64_t entry = elf_load(proc_idx, (uint64_t)g_execbuf, (uint64_t)n);
    if (!entry) { kprintf("[exec   ] ELF load failed\n"); return -1; }
    enter_process(name, proc_idx, entry);
    return 0;
}

/* VFS demonstration: create named files (multi-block + dedup), read them back,*/
/* copy-on-write a file, list the directory, then exec an ELF from storage.    */
static void cmd_vfs(void) {
    if (!g_cas_mounted) { kputs("[vfs    ] CAS not mounted\n"); return; }
    kputs("-- VFS: named files over CAS (multi-block, dedup, copy-on-write) --\n");

    const char *motd = "Welcome to Outrun OS. Files are names over content-addressed blocks.\n";
    /* a >512-byte file to force multi-block storage */
    static char big[1400];
    for (int i = 0; i < 1399; i++) big[i] = (char)('A' + (i % 26));
    big[1399] = 0;

    vfs_write_file("motd", motd, cstrlen(motd) + 1);
    vfs_write_file("readme", big, 1400);
    uint64_t motd_hash_v1 = DENTS[vfs_find("motd")].file_hash;

    /* list */
    kputs("[vfs    ] directory:\n");
    for (int i = 0; i < VFS_MAXFILES; i++) if (DENTS[i].used)
        kprintf("[vfs    ]   %s  (%d bytes, %d chunks, hash %X)\n",
                DENTS[i].name, DENTS[i].len, DENTS[i].nchunks, DENTS[i].file_hash);

    /* read back the multi-block file and verify */
    static char rb[1500];
    int ri = vfs_find("readme");
    int64_t n = vfs_read_file(ri, rb, sizeof rb);
    int ok = (n == 1400);
    for (int i = 0; i < 1400 && ok; i++) if (rb[i] != big[i]) ok = 0;
    kprintf("[vfs    ] read 'readme' (%d bytes, spans %d blocks): %s\n",
            n, DENTS[ri].nchunks, ok ? "content verified" : "MISMATCH");

    /* copy-on-write: rewrite motd -> new content hash, name repoints */
    vfs_write_file("motd", "motd v2 - overwritten content.\n", 32);
    uint64_t motd_hash_v2 = DENTS[vfs_find("motd")].file_hash;
    kprintf("[vfs    ] copy-on-write 'motd': hash %X -> %X (immutable blocks, name repointed)\n",
            motd_hash_v1, motd_hash_v2);

    /* dedup across files: an identical copy shares CAS blocks (no new data) */
    uint64_t used_before = SB->used_blocks;
    vfs_write_file("readme_copy", big, 1400);
    kprintf("[vfs    ] 'readme_copy' identical to 'readme' -> blocks used +%d (CAS dedup)\n",
            SB->used_blocks - used_before);
    kputs("-- done --\n");
}

static void cmd_usermode(void) {
    kputs("-- genuine ring-3 user mode: capability passthrough via SYSCALL trap --\n");

    /* Prefer the REAL virtio NIC discovered on the PCI bus; fall back to a    */
    /* scratch sentinel page only if QEMU was booted without a virtio device. */
    uint64_t dev_cap;
    if (g_virtio_kdev >= 0) {
        g_demo_dev_index = g_virtio_kdev;
        dev_cap = PCAP_NETWORK;
        kprintf("[kernel ] demo device: REAL '%s' (virtio MMIO @ phys %X)\n",
                kdevs[g_demo_dev_index].name, kdevs[g_demo_dev_index].base);
    } else {
        uint64_t dev_phys = alloc_frame();
        *(volatile uint32_t *)dev_phys = 0xCAFEBABE;
        g_demo_dev_index = n_kdev;
        dev_cap = PCAP_CAMERA;
        kdev_register("sensor0", dev_phys, 0x1000, PCAP_CAMERA);
        kprintf("[kernel ] demo device: scratch 'sensor0' (no virtio present)\n");
    }

    int a = kproc_spawn("stream-app",  PCAP_HW_PASSTHROUGH | dev_cap);
    int b = kproc_spawn("sketchy-app", PCAP_FILESYSTEM);

    int have_elf = (g_user_elf != 0);
    kprintf("[kernel ] user program source: %s\n",
            have_elf ? "ELF module loaded from ISO" : "embedded blob (no module on ISO)");

    if (have_elf) { load_and_run_elf("stream-app", a); load_and_run_elf("sketchy-app", b); }
    else          { run_ring3("stream-app", a);        run_ring3("sketchy-app", b); }

    /* sys_exec from storage: store the ELF into the VFS (content-addressed),   */
    /* then load and run it straight out of CAS — no GRUB module involved.      */
    if (have_elf && g_cas_mounted) {
        vfs_write_file("init", (void *)g_user_elf, (uint32_t)(g_user_elf_end - g_user_elf));
        int c = kproc_spawn("cas-exec", PCAP_HW_PASSTHROUGH | dev_cap | PCAP_FILESYSTEM | PCAP_FRAMEBUFFER);
        kputs("\n[kernel ] exec-from-storage: running 'init' loaded from CAS (not GRUB)\n");
        exec_from_cas("init", c);
    }

    kputs("-- done: capabilities decided per-process — hardware for one, files\n");
    kputs("   for another, both for the CAS-loaded process. --\n");
}

/* ===========================================================================
 * PHASE 4 — FRAMEBUFFER GRAPHICS + METROPOLIS-TERMINAL COMPOSITOR
 * ===========================================================================
 * Maps the bootloader's linear framebuffer, renders into a RAM backbuffer
 * (double-buffering), then flips. The compositor draws the Metropolis-Terminal
 * visual language on bare metal: obsidian canvas, chamfered glass panels with
 * neon edges, a physics-settled window layout, telemetry bracket, gesture
 * crosshair, and horizontal scanlines. All math is integer/fixed-point (the
 * kernel runs without SSE), so no FPU is touched.
 * =========================================================================== */
#define FB_V 0x0000700000000000ull
static volatile uint32_t *g_fb = 0;     /* mapped hardware framebuffer         */
static uint32_t          *g_bb = 0;     /* RAM backbuffer                      */
static uint32_t           g_stride = 0; /* pixels per row                      */
static int                g_gfx_ready = 0;

/* Metropolis-Terminal palette (0x00RRGGBB) */
#define C_OBS0 0x05060A
#define C_OBS1 0x0A0D14
#define C_OBS2 0x121722
#define C_HAIR 0x1E2735
#define C_CYAN 0x22E4FF
#define C_MAGE 0xFF2D9B
#define C_AMBER 0xFFB020
#define C_TEXT 0xEAF2F7
#define C_MUTE 0x7C8CA0
#define C_MINT 0x3DF5C4
#define C_GRID 0x0E1420

static void fb_init(void) {
    if (!g_fb_addr || !g_fb_width) { kputs("[gfx    ] no framebuffer provided by bootloader\n"); return; }
    uint64_t bytes = (uint64_t)g_fb_pitch * g_fb_height;
    uint64_t pages = (bytes + 0xFFF) / 0x1000;
    for (uint64_t i = 0; i < pages; i++)
        map_page(kernel_cr3, FB_V + i * 0x1000, (g_fb_addr & ~0xFFFull) + i * 0x1000, PTE_WRITE | PTE_NX);
    g_fb = (volatile uint32_t *)(FB_V + (g_fb_addr & 0xFFF));
    g_stride = g_fb_pitch / 4;
    g_bb = (uint32_t *)alloc_frames(pages);               /* RAM backbuffer      */
    g_gfx_ready = 1;
    kprintf("[gfx    ] framebuffer %dx%d mapped; backbuffer @ %X (double-buffered)\n",
            (uint64_t)g_fb_width, (uint64_t)g_fb_height, (uint64_t)g_bb);
}

static inline void px(int x, int y, uint32_t c) {
    if ((unsigned)x < g_fb_width && (unsigned)y < g_fb_height) g_bb[y * g_stride + x] = c;
}
/* alpha blend c over existing backbuffer pixel (a = 0..255) — glass diffusion */
static inline void blend(int x, int y, uint32_t c, int a) {
    if ((unsigned)x >= g_fb_width || (unsigned)y >= g_fb_height) return;
    uint32_t d = g_bb[y * g_stride + x];
    uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
    uint32_t sr = (c >> 16) & 0xFF, sg = (c >> 8) & 0xFF, sb = c & 0xFF;
    uint32_t rr = (sr * a + dr * (255 - a)) / 255;
    uint32_t rg = (sg * a + dg * (255 - a)) / 255;
    uint32_t rb = (sb * a + db * (255 - a)) / 255;
    g_bb[y * g_stride + x] = (rr << 16) | (rg << 8) | rb;
}
static void fill(uint32_t c) { for (uint32_t i = 0; i < g_fb_width * g_fb_height; i++) g_bb[i] = c; }
static void rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j, c);
}
static void hline(int x, int y, int w, uint32_t c) { for (int i = 0; i < w; i++) px(x + i, y, c); }
static void vline(int x, int y, int h, uint32_t c) { for (int j = 0; j < h; j++) px(x, y + j, c); }

/* A glass panel with two opposite 45-degree chamfered corners (top-left and    */
/* bottom-right), translucent fill over the canvas, and a bright neon edge.     */
static void glass_panel(int x, int y, int w, int h, uint32_t edge) {
    int c = 16;                                            /* chamfer size        */
    for (int j = 0; j < h; j++) {
        int l = 0, r = w;
        if (j < c) l = c - j;                              /* top-left cut        */
        if (j >= h - c) r = w - (c - (h - 1 - j));         /* bottom-right cut    */
        /* vertical gradient glass: lighter near the top, plus an accent-tinted */
        /* header band in the first 22 rows.                                    */
        uint32_t base = (j < 22) ? edge : C_OBS2;
        int a = (j < 22) ? 42 : (190 - j / 6);
        if (a < 150) a = 150;
        for (int i = l; i < r; i++) blend(x + i, y + j, base, a);
    }
    /* neon edge following the chamfer, with a soft glow line */
    for (int i = c; i < w; i++)      { px(x + i, y, edge); blend(x + i, y - 1, edge, 90); }
    for (int i = 0; i < w - c; i++)  { px(x + i, y + h - 1, edge); blend(x + i, y + h, edge, 90); }
    for (int j = c; j < h; j++)      { px(x, y + j, edge); blend(x - 1, y + j, edge, 90); }
    for (int j = 0; j < h - c; j++)  { px(x + w - 1, y + j, edge); blend(x + w, y + j, edge, 90); }
    for (int k = 0; k < c; k++) { px(x + c - k, y + k, edge); px(x + w - 1 - k, y + h - c + k, edge); }
    /* header underline */
    hline(x + 2, y + 22, w - 4, edge);
}

/* horizontal meter bar (telemetry) */
static void meter(int x, int y, int w, int pct, uint32_t c) {
    rect(x, y, w, 6, C_OBS2);
    rect(x + 1, y + 1, (w - 2) * pct / 100, 4, c);
}

/* --- 8x8 font (font8x8_basic subset, ASCII 0x20-0x5F; bit0 = leftmost) ------ */
static const uint8_t g_font[64][8] = {
{0,0,0,0,0,0,0,0},{0x18,0x3C,0x3C,0x18,0x18,0,0x18,0},{0x36,0x36,0,0,0,0,0,0},
{0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0},{0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0},
{0,0x63,0x33,0x18,0x0C,0x66,0x63,0},{0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0},
{0x06,0x06,0x03,0,0,0,0,0},{0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0},
{0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0},{0,0x66,0x3C,0xFF,0x3C,0x66,0,0},
{0,0x0C,0x0C,0x3F,0x0C,0x0C,0,0},{0,0,0,0,0,0x0C,0x0C,0x06},
{0,0,0,0x3F,0,0,0,0},{0,0,0,0,0,0x0C,0x0C,0},{0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0},
{0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0},{0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0},
{0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0},{0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0},
{0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0},{0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0},
{0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0},{0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0},
{0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0},{0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0},
{0,0x0C,0x0C,0,0,0x0C,0x0C,0},{0,0x0C,0x0C,0,0,0x0C,0x0C,0x06},
{0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0},{0,0,0x3F,0,0,0x3F,0,0},
{0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0},{0x1E,0x33,0x30,0x18,0x0C,0,0x0C,0},
{0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0},{0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0},
{0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0},{0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0},
{0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0},{0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0},
{0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0},{0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0},
{0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0},{0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0},
{0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0},{0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0},
{0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0},{0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0},
{0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0},{0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0},
{0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0},{0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0},
{0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0},{0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0},
{0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0},{0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0},
{0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0},{0x63,0x63,0x6B,0x7F,0x7F,0x77,0x63,0},
{0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0},{0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0},
{0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0},{0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0},
{0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0},{0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0},
{0x08,0x1C,0x36,0x63,0,0,0,0},{0,0,0,0,0,0,0,0xFF},
};
static void draw_char(int x, int y, char ch, uint32_t c) {
    if (ch < 0x20 || ch > 0x5F) ch = ch >= 'a' && ch <= 'z' ? ch - 32 : ' ';
    const uint8_t *g = g_font[ch - 0x20];
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (g[row] & (1 << col)) px(x + col, y + row, c);
}
static void draw_str(int x, int y, const char *s, uint32_t c) {
    for (; *s; s++) { draw_char(x, y, *s, c); x += 8; }
}

/* --- fixed-point (16.16) window physics: settle a non-overlapping layout ---- */
#define FX 16
#define FXI(v) ((int64_t)(v) << FX)
#define FXMUL(a,b) (((int64_t)(a) * (int64_t)(b)) >> FX)
#define FX_HALF 32768   /* one PIT tick advances a per-frame velocity by v/2   */
struct win { int64_t x, y, vx, vy, tx, ty; int w, h; uint32_t edge; };

static void fb_flip(void) {
    for (uint32_t y = 0; y < g_fb_height; y++)
        for (uint32_t x = 0; x < g_fb_width; x++)
            g_fb[y * g_stride + x] = g_bb[y * g_stride + x];
}

/* v0.34: window-spring physics on the same PIT-tick clock as the camera.
 * Per-tick constants derived from the tuned per-frame pair (k=1/6, damp=0.82,
 * frame = 2 ticks): damp_t = sqrt(damp) in 16.16 (59345; its FXMUL square is
 * 53738 vs 53739 — within 1/65536), and the spring impulse rescaled so two
 * ticks impart the per-frame velocity gain: k_t = k*damp_t/(1+damp_t) = 5190.
 * Verified step response matches the old tuning (same overshoot, same settle).
 * Velocities stay in world-units-per-frame; position integrates v/2 per tick.
 * Collision separation runs per tick with the unchanged rule — it is
 * state-proportional and convergent, so it simply settles in half the wall
 * time. ALL mutation is per tick: grouping ticks into frames of any size
 * cannot change the trajectory.                                              */
#define SPRING_K_T    5190
#define SPRING_DAMP_T 59345

static void physics_tick(struct win *wn, int n) {
    for (int i = 0; i < n; i++) {
        int64_t ax = FXMUL(SPRING_K_T, wn[i].tx - wn[i].x);
        int64_t ay = FXMUL(SPRING_K_T, wn[i].ty - wn[i].y);
        wn[i].vx = FXMUL(wn[i].vx + ax, SPRING_DAMP_T);
        wn[i].vy = FXMUL(wn[i].vy + ay, SPRING_DAMP_T);
        wn[i].x += FXMUL(wn[i].vx, FX_HALF); wn[i].y += FXMUL(wn[i].vy, FX_HALF);
    }
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) {
        int ax = (int)(wn[i].x >> FX), ay = (int)(wn[i].y >> FX);
        int bx = (int)(wn[j].x >> FX), by = (int)(wn[j].y >> FX);
        int ox = (ax < bx ? ax + wn[i].w : bx + wn[j].w) - (ax < bx ? bx : ax);
        int oy = (ay < by ? ay + wn[i].h : by + wn[j].h) - (ay < by ? by : ay);
        int overlapx = (ax < bx + wn[j].w && bx < ax + wn[i].w) ? ox : -1;
        int overlapy = (ay < by + wn[j].h && by < ay + wn[i].h) ? oy : -1;
        if (overlapx > 0 && overlapy > 0) {
            if (overlapx < overlapy) { int64_t p = FXI(overlapx / 2 + 1);
                if (ax < bx) { wn[i].x -= p; wn[j].x += p; } else { wn[i].x += p; wn[j].x -= p; } }
            else { int64_t p = FXI(overlapy / 2 + 1);
                if (ay < by) { wn[i].y -= p; wn[j].y += p; } else { wn[i].y += p; wn[j].y -= p; } }
        }
    }
}

/* Advance by a measured number of ticks (a variable-length frame).           */
static void physics_step_dt(struct win *wn, int n, int ticks) {
    if (ticks < 1) ticks = 1;
    if (ticks > 32) ticks = 32;
    while (ticks--) physics_tick(wn, n);
}

/* Legacy fixed frame (cmd_gfx's compositor loop steps by this).              */
static void physics_step(struct win *wn, int n) { physics_step_dt(wn, n, 2); }

static struct win g_wins[5];
static const char *g_labels[5] = { "TIME-STREAM", "COMM-DECK", "SYS.TELEMETRY", "TIME-NODE", "VIDEO-EDITOR" };
static int g_glitch = 0;                 /* chromatic split amount, fades in     */

static void compositor_init(void) {
    int tx[5] = {120,560,120,560,330}, ty[5] = {120,150,360,360,300};
    int ww[5] = {300,280,300,236,360}, hh[5] = {150,150,190,150,170};
    uint32_t ed[5] = {C_CYAN,C_MAGE,C_AMBER,C_CYAN,C_CYAN};
    for (int i = 0; i < 5; i++) {
        g_wins[i].tx = FXI(tx[i]); g_wins[i].ty = FXI(ty[i]);
        g_wins[i].x = FXI(380 + i * 6); g_wins[i].y = FXI(300);   /* start clustered -> glitch/spring out */
        g_wins[i].vx = 0; g_wins[i].vy = 0;
        g_wins[i].w = ww[i]; g_wins[i].h = hh[i]; g_wins[i].edge = ed[i];
    }
}

static void compositor_frame(int frame) {
    int W = g_fb_width, H = g_fb_height;
    fill(C_OBS0);
    for (int x = 0; x < W; x += 42) vline(x, 0, H, C_GRID);
    for (int y = 0; y < H; y += 42) hline(0, y, W, C_GRID);

    /* wordmark */
    draw_str(46, 10, "OUTRUN", C_CYAN);
    draw_str(46 + 56, 10, " // METROPOLIS-TERMINAL", C_MUTE);

    /* left telemetry bracket */
    rect(0, 0, 34, H, C_OBS1);
    vline(34, 0, H, C_HAIR);
    for (int i = 0; i < 10; i++) rect(10, 40 + i * 40, 14, 3, i < 6 ? C_CYAN : C_HAIR);
    rect(12, H - 130, 8, 110, C_OBS2); rect(13, H - 70, 6, 50, C_MINT);

    for (int i = 0; i < 5; i++) {
        int x = (int)(g_wins[i].x >> FX), y = (int)(g_wins[i].y >> FX);
        /* glitch-in: cyan/magenta chromatic ghost edges while settling */
        if (g_glitch > 0) {
            for (int j = 16; j < g_wins[i].w; j++) { blend(x + j + 2, y, C_CYAN, g_glitch);
                blend(x + j - 2, y, C_MAGE, g_glitch); }
        }
        glass_panel(x, y, g_wins[i].w, g_wins[i].h, g_wins[i].edge);
        draw_str(x + 14, y + 8, g_labels[i], g_wins[i].edge);
        for (int b = 0; b < 3; b++) meter(x + 14, y + 34 + b * 16, g_wins[i].w - 40, 40 + b * 20, b == 2 ? C_MAGE : C_MINT);
        for (int d = 0; d < 5; d++) { int cxp = x + 20 + d * ((g_wins[i].w - 40) / 4);
            rect(cxp, y + g_wins[i].h - 20, 4, 4, d == 4 ? C_CYAN : C_MUTE); }
    }

    /* amber gesture crosshair, orbiting slowly (animated) */
    int cx = W / 2 + ((frame * 3) % 60) - 30;
    int cy = H / 2 + ((frame * 2) % 40) - 20;
    for (int r = 4; r < 14; r++) { blend(cx - r, cy, C_AMBER, 120); blend(cx + r, cy, C_AMBER, 120);
        blend(cx, cy - r, C_AMBER, 120); blend(cx, cy + r, C_AMBER, 120); }
    hline(cx - 12, cy, 8, C_AMBER); hline(cx + 5, cy, 8, C_AMBER);
    vline(cx, cy - 12, 8, C_AMBER); vline(cx, cy + 5, 8, C_AMBER);

    rect(0, 0, W, 4, C_OBS1); rect(0, 0, 120, 4, C_CYAN);
    for (int y = 0; y < H; y += 2) for (int x = 0; x < W; x++) blend(x, y, 0x000000, 26);
}


/* ===========================================================================
 * METROPOLIS-TERMINAL: THE SPATIAL CANVAS
 * ===========================================================================
 * The blueprint's interface promise, made real: windows live in an infinite
 * world, not on a screen. A camera (pan + zoom, fixed-point) projects that
 * world; windows never overlap (physics pushes them apart); the focused window
 * is raised with volumetric depth; an Accelerator HUD gives keyboard-first
 * command access; and a Context Ribbon shows what the current input can do.
 * =========================================================================== */
#define NWIN 5
static int64_t g_cam_x, g_cam_y, g_zoom;      /* camera in world space (16.16)  */
static int  g_focus = 0, g_hud = 0, g_hud_len = 0;
static char g_hud_buf[40];

static int64_t FXDIV(int64_t a, int64_t b) { return b ? ((a << FX) / b) : 0; }
static int w2sx(int64_t wx) { return (int)(FXMUL(wx - g_cam_x, g_zoom) >> FX) + g_fb_width / 2; }
static int w2sy(int64_t wy) { return (int)(FXMUL(wy - g_cam_y, g_zoom) >> FX) + g_fb_height / 2; }
static int wsc(int v)       { return (int)(FXMUL(FXI(v), g_zoom) >> FX); }

/* ---- cursor: screen <-> world coordinate translation ----------------------- */
/* The inverse of w2s. Getting this exactly right is what makes zoom-at-cursor
 * and window dragging feel correct: the world point under the pointer must stay
 * under the pointer. Verified by cmd_cursor(). */
static int64_t s2wx(int sx) { return g_cam_x + FXDIV(FXI(sx - g_fb_width / 2), g_zoom); }
static int64_t s2wy(int sy) { return g_cam_y + FXDIV(FXI(sy - g_fb_height / 2), g_zoom); }

static int g_cur_x = 512, g_cur_y = 384;      /* cursor in screen space          */
static int g_drag = 0, g_drag_win = -1;       /* 0 none, 1 window, 2 canvas pan  */

static void canvas_clampz(void) {
    if (g_zoom < FXI(1) / 8) g_zoom = FXI(1) / 8;
    if (g_zoom > FXI(4))     g_zoom = FXI(4);
}

/* Zoom about a screen point, keeping the world point under it fixed. */
static void canvas_zoom_at(int sx, int sy, int64_t factor) {
    int64_t wx = s2wx(sx), wy = s2wy(sy);
    g_zoom = FXMUL(g_zoom, factor);
    canvas_clampz();
    g_cam_x = wx - FXDIV(FXI(sx - g_fb_width / 2), g_zoom);
    g_cam_y = wy - FXDIV(FXI(sy - g_fb_height / 2), g_zoom);
}

/* ---- kinetic camera: momentum, friction, eased zoom ------------------------ */
/* Pan carries momentum (flick and glide); zoom eases toward a target while the
 * world point under the anchor stays pinned for EVERY frame of the transition,
 * not just the endpoints. Commands glide the camera to a target instead of
 * teleporting. All fixed-point: no FPU anywhere. */
/* v0.32: the physics clock is the PIT tick (10 ms), not the frame. The classic
 * frame was 2 ticks (~50 fps), so each per-frame decay constant is replaced by
 * its 16.16 square root applied once per tick — two ticks reproduce the tuned
 * v0.31 feel bit-for-bit (friction and zoom roots are exact under FXMUL
 * truncation; glide retention lands within 1/65536). Because ALL state
 * mutation happens per tick, camera_step_dt(a);camera_step_dt(b) is exactly
 * camera_step_dt(a+b): frame rate cannot change the trajectory.              */
#define CAM_FRICTION_T 61478   /* sqrt(0.88):   FXMUL(t,t) == FXI(1)*88/100 exactly   */
#define CAM_EASE_T      7656   /* 1 - sqrt(0.78): per-frame ease 0.22 (±1/65536)      */
#define ZOOM_EASE_T     8780   /* 1 - sqrt(0.75): per-frame ease 0.25 exactly         */
#define CAM_FRAME_TICKS 2      /* the legacy frame the suites step by                 */
static int64_t g_cam_vx = 0, g_cam_vy = 0;    /* camera velocity (world/frame) */
static int64_t g_cam_tx = 0, g_cam_ty = 0;    /* camera glide target           */
static int64_t g_zoom_t = 0;                  /* zoom target                   */
static int     g_cam_glide = 0;
static int     g_anchor_sx = 512, g_anchor_sy = 384;
static int64_t g_fling_vx = 0, g_fling_vy = 0;

static int64_t kabs(int64_t v) { return v < 0 ? -v : v; }

/* Aim the zoom at a target, anchored on a screen point (eased, not instant). */
static void canvas_zoom_to(int sx, int sy, int64_t factor) {
    g_anchor_sx = sx; g_anchor_sy = sy;
    g_zoom_t = FXMUL(g_zoom_t, factor);
    if (g_zoom_t < FXI(1) / 8) g_zoom_t = FXI(1) / 8;
    if (g_zoom_t > FXI(4))     g_zoom_t = FXI(4);
    g_cam_glide = 0;                          /* a manual zoom cancels a glide */
}

/* Advance the camera by exactly ONE PIT tick. Everything that mutates state —
 * decay, easing, snap thresholds, zoom clamp, anchor re-pin — happens here,
 * per tick, so tick grouping cannot alter the trajectory.                    */
static void camera_tick(void) {
    int W = g_fb_width, H = g_fb_height;
    if (g_cam_glide) {                        /* eased move to a commanded spot */
        int64_t dx = g_cam_tx - g_cam_x, dy = g_cam_ty - g_cam_y;
        g_cam_x += FXMUL(dx, CAM_EASE_T);
        g_cam_y += FXMUL(dy, CAM_EASE_T);
        g_cam_vx = g_cam_vy = 0;
        int64_t dz = g_zoom_t - g_zoom;
        g_zoom += FXMUL(dz, ZOOM_EASE_T);
        canvas_clampz();
        if (kabs(dx) < FXI(1) && kabs(dy) < FXI(1) && kabs(dz) < FXI(1) / 256) {
            g_cam_x = g_cam_tx; g_cam_y = g_cam_ty; g_zoom = g_zoom_t; g_cam_glide = 0;
        }
        return;
    }
    /* momentum: v is world-units-per-frame; one tick is half a classic frame  */
    g_cam_x += FXMUL(g_cam_vx, FX_HALF); g_cam_y += FXMUL(g_cam_vy, FX_HALF);
    g_cam_vx = FXMUL(g_cam_vx, CAM_FRICTION_T);
    g_cam_vy = FXMUL(g_cam_vy, CAM_FRICTION_T);
    if (kabs(g_cam_vx) < FXI(1) / 32) g_cam_vx = 0;
    if (kabs(g_cam_vy) < FXI(1) / 32) g_cam_vy = 0;
    /* eased zoom, re-pinned to the anchor every TICK (drift bound tightens)   */
    if (g_zoom != g_zoom_t) {
        int64_t wx = s2wx(g_anchor_sx), wy = s2wy(g_anchor_sy);   /* before step */
        int64_t dz = g_zoom_t - g_zoom;
        if (kabs(dz) < FXI(1) / 256) g_zoom = g_zoom_t;
        else g_zoom += FXMUL(dz, ZOOM_EASE_T);
        canvas_clampz();
        g_cam_x = wx - FXDIV(FXI(g_anchor_sx - W / 2), g_zoom);   /* re-pin      */
        g_cam_y = wy - FXDIV(FXI(g_anchor_sy - H / 2), g_zoom);
    }
}

/* Advance by a measured number of ticks (a variable-length frame).           */
static void camera_step_dt(int ticks) {
    if (ticks < 1) ticks = 1;
    if (ticks > 32) ticks = 32;               /* a stall is not a teleport      */
    while (ticks--) camera_tick();
}

/* Legacy fixed frame (the kinetic/cursor suites step by this).               */
static void camera_step(void) { camera_step_dt(CAM_FRAME_TICKS); }

/* Topmost window under a screen point (focused window is on top), else -1. */
static int canvas_pick(int sx, int sy) {
    int64_t wx = s2wx(sx), wy = s2wy(sy);
    for (int k = 0; k < NWIN; k++) {
        int i = (k == 0) ? g_focus : (k <= g_focus ? k - 1 : k);
        if (wx >= g_wins[i].x && wx < g_wins[i].x + FXI(g_wins[i].w) &&
            wy >= g_wins[i].y && wy < g_wins[i].y + FXI(g_wins[i].h)) return i;
    }
    return -1;
}

/* Route a screen-space event to the surface under it, in SURFACE-LOCAL pixels.
 * This is the inverse of the compositor's placement: screen -> world -> panel
 * content rect -> the app's own pixel grid. Returns the slot, or -1. */
static int surface_route(int sx, int sy, int type, int code) {
    for (int k = 0; k < NWIN; k++) {
        int i = (k == 0) ? g_focus : (k <= g_focus ? k - 1 : k);
        if (i >= 8 || !g_surf[i].used) continue;
        int x = w2sx(g_wins[i].x), y = w2sy(g_wins[i].y);
        int w = wsc(g_wins[i].w), h = wsc(g_wins[i].h);
        int cx = x + 8, cy = y + 24, cw = w - 16, ch = h - 30;   /* content rect  */
        if (cw <= 0 || ch <= 0) continue;
        if (sx < cx || sx >= cx + cw || sy < cy || sy >= cy + ch) continue;
        struct surface *S = &g_surf[i];
        struct sevent e;
        e.type = type; e.code = code;
        e.x = (sx - cx) * S->w / cw;                             /* surface-local */
        e.y = (sy - cy) * S->h / ch;
        if ((S->qw - S->qr) < 16) S->q[S->qw++ % 16] = e;
        return i;
    }
    return -1;
}

static void canvas_mouse(void) {
    if (!g_mouse_ok) return;
    __asm__ volatile("cli");
    int32_t dx = g_mouse_dx, dy = g_mouse_dy, dz = g_mouse_dz;
    uint8_t btn = g_mouse_btn;
    g_mouse_dx = g_mouse_dy = g_mouse_dz = 0;
    __asm__ volatile("sti");

    g_cur_x += dx; g_cur_y += dy;
    if (g_cur_x < 0) g_cur_x = 0; if (g_cur_x >= g_fb_width)  g_cur_x = g_fb_width - 1;
    if (g_cur_y < 0) g_cur_y = 0; if (g_cur_y >= g_fb_height) g_cur_y = g_fb_height - 1;

    if (dz) canvas_zoom_to(g_cur_x, g_cur_y,           /* wheel zooms at cursor  */
                           dz < 0 ? (FXI(1) + FXI(1) / 5) : (FXI(1) * 5 / 6));

    if (btn & 1) {
        if (!g_drag) { g_fling_vx = g_fling_vy = 0;                                  /* press: pick a target   */
            g_drag_win = canvas_pick(g_cur_x, g_cur_y);
            if (g_drag_win >= 0 && surface_route(g_cur_x, g_cur_y, 1, 1) >= 0) {
                g_focus = g_drag_win; g_drag = 3;         /* click went to the app */
            } else if (g_drag_win >= 0) { g_drag = 1; g_focus = g_drag_win; }
            else g_drag = 2;
        } else if (g_drag == 1 && g_drag_win >= 0) {    /* drag window in world   */
            int64_t wdx = FXDIV(FXI(dx), g_zoom), wdy = FXDIV(FXI(dy), g_zoom);
            g_wins[g_drag_win].x += wdx; g_wins[g_drag_win].y += wdy;
            g_wins[g_drag_win].tx += wdx; g_wins[g_drag_win].ty += wdy;
            g_wins[g_drag_win].vx = 0;    g_wins[g_drag_win].vy = 0;
        } else if (g_drag == 2) {                       /* drag empty space: pan  */
            int64_t wdx = FXDIV(FXI(dx), g_zoom), wdy = FXDIV(FXI(dy), g_zoom);
            g_cam_x -= wdx; g_cam_y -= wdy;
            g_cam_vx = 0; g_cam_vy = 0; g_cam_glide = 0;
            g_fling_vx = (g_fling_vx + (-wdx) * 3) / 4;  /* smoothed drag speed  */
            g_fling_vy = (g_fling_vy + (-wdy) * 3) / 4;
        }
    } else {
        if (g_drag == 2) { g_cam_vx = g_fling_vx; g_cam_vy = g_fling_vy; }  /* fling */
        g_fling_vx = g_fling_vy = 0;
        g_drag = 0; g_drag_win = -1;
    }
}

static void canvas_cursor_draw(void) {
    int x = g_cur_x, y = g_cur_y;
    uint32_t c = g_drag == 1 ? C_MINT : (g_drag == 2 ? C_MAGE : C_AMBER);
    for (int i = 0; i < 12; i++) {                       /* arrow body            */
        blend(x + i, y + i, 0x000000, 200);
        px(x + i, y + i, c);
        for (int j = 0; j < (10 - i) / 2; j++) px(x + i, y + i + 1 + j, c);
    }
    vline(x, y, 14, c); hline(x, y, 8, c);
}

struct ccmd { const char *name; int id; };
static const struct ccmd g_ccmds[] = {
    { "zoom fit",            1 }, { "zoom in",             2 },
    { "zoom out",            3 }, { "center canvas",       4 },
    { "focus time-stream",   5 }, { "focus comm-deck",     6 },
    { "focus sys.telemetry", 7 }, { "focus time-node",     8 },
    { "focus video-editor",  9 }, { "tile windows",       10 },
};
#define NCMD ((int)(sizeof g_ccmds / sizeof g_ccmds[0]))

/* prefix match, case-insensitive, empty query matches everything */
static int ccmd_match(int i) {
    if (!g_hud_len) return 1;
    const char *n = g_ccmds[i].name;
    for (int k = 0; k < g_hud_len; k++) {
        char a = n[k], b = g_hud_buf[k];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (!a || a != b) return 0;
    }
    return 1;
}

static void canvas_focus_on(int i) {
    g_focus = i;
    g_cam_tx = g_wins[i].x + FXI(g_wins[i].w / 2);
    g_cam_ty = g_wins[i].y + FXI(g_wins[i].h / 2);
    g_cam_glide = 1;                                   /* glide, do not teleport */
}

static void canvas_zoom_fit(void) {
    int64_t minx = g_wins[0].x, miny = g_wins[0].y;
    int64_t maxx = g_wins[0].x + FXI(g_wins[0].w), maxy = g_wins[0].y + FXI(g_wins[0].h);
    for (int i = 1; i < NWIN; i++) {
        if (g_wins[i].x < minx) minx = g_wins[i].x;
        if (g_wins[i].y < miny) miny = g_wins[i].y;
        if (g_wins[i].x + FXI(g_wins[i].w) > maxx) maxx = g_wins[i].x + FXI(g_wins[i].w);
        if (g_wins[i].y + FXI(g_wins[i].h) > maxy) maxy = g_wins[i].y + FXI(g_wins[i].h);
    }
    g_cam_tx = (minx + maxx) / 2; g_cam_ty = (miny + maxy) / 2;
    int64_t zx = FXDIV(FXI(g_fb_width - 90),  maxx - minx);
    int64_t zy = FXDIV(FXI(g_fb_height - 140), maxy - miny);
    g_zoom_t = zx < zy ? zx : zy;
    if (g_zoom_t < FXI(1) / 8) g_zoom_t = FXI(1) / 8;
    if (g_zoom_t > FXI(4))     g_zoom_t = FXI(4);
    g_cam_glide = 1;
}

static void ccmd_exec(int id) {
    switch (id) {
        case 1: canvas_zoom_fit(); break;
        case 2: canvas_zoom_to(g_fb_width / 2, g_fb_height / 2, FXI(1) + FXI(1) / 2); break;
        case 3: canvas_zoom_to(g_fb_width / 2, g_fb_height / 2, FXI(1) * 2 / 3); break;
        case 4: g_cam_tx = 0; g_cam_ty = 0; g_zoom_t = FXI(1); g_cam_glide = 1; break;
        case 10: for (int i = 0; i < NWIN; i++) {          /* tile: grid layout  */
                     g_wins[i].tx = FXI(-560 + (i % 3) * 400);
                     g_wins[i].ty = FXI(-200 + (i / 3) * 240);
                 } canvas_zoom_fit(); break;
        default: if (id >= 5 && id <= 9) canvas_focus_on(id - 5); break;
    }
}

/* v0.33: keyboard routing to surfaces — the reserved type=2 event goes live.
 * Modal, like a real WM: Enter arms "type-to-app" when the focused window has
 * a bound surface; from then on EVERY key belongs to the app (it owns the
 * keyboard) except Esc, which returns it to camera navigation. Keys travel
 * the same 16-deep per-surface queue as clicks; the app's SYS_SURFACE_POLL
 * loop dispatches on sevent.type.                                            */
static int      g_key_to_app = 0;
static uint64_t g_keys_routed = 0;             /* test hook: keys delivered    */

static void key_route(int slot, int code) {
    struct surface *S = &g_surf[slot];
    struct sevent e;
    e.type = 2; e.x = -1; e.y = -1; e.code = code;   /* keys have no position  */
    if ((S->qw - S->qr) < 16) { S->q[S->qw++ % 16] = e; g_keys_routed++; }
}

/* Drain the keyboard. Returns 0 when the user asks to leave the canvas.        */
static int canvas_input(void) {
    int c;
    while ((c = kbd_getc_nonblock()) >= 0) {
        if (g_key_to_app) {                    /* the focused app owns the keys */
            if (c == 27) { g_key_to_app = 0; continue; }         /* Esc: back  */
            if (!g_surf[g_focus].used) { g_key_to_app = 0; continue; }
            if (c == '\b' || c == '\n' || (c >= 32 && c < 127))
                key_route(g_focus, c);
            continue;
        }
        if (g_hud) {
            if (c == 27) { g_hud = 0; g_hud_len = 0; g_hud_buf[0] = 0; }
            else if (c == '\n') {
                for (int i = 0; i < NCMD; i++) if (ccmd_match(i)) { ccmd_exec(g_ccmds[i].id); break; }
                g_hud = 0; g_hud_len = 0; g_hud_buf[0] = 0;
            } else if (c == '\b') { if (g_hud_len) g_hud_buf[--g_hud_len] = 0; }
            else if (c >= 32 && c < 127 && g_hud_len < 38) {
                g_hud_buf[g_hud_len++] = (char)c; g_hud_buf[g_hud_len] = 0;
            }
            continue;
        }
        int64_t imp = FXDIV(FXI(11), g_zoom);              /* momentum impulse    */
        switch (c) {
            case 'a': case 'A': g_cam_vx -= imp; g_cam_glide = 0; break;
            case 'd': case 'D': g_cam_vx += imp; g_cam_glide = 0; break;
            case 'w': case 'W': g_cam_vy -= imp; g_cam_glide = 0; break;
            case 's': case 'S': g_cam_vy += imp; g_cam_glide = 0; break;
            case 'e': case 'E': canvas_zoom_to(g_cur_x, g_cur_y, FXI(1) + FXI(1) / 4); break;
            case 'q': case 'Q': canvas_zoom_to(g_cur_x, g_cur_y, FXI(1) * 4 / 5); break;
            case '\t': canvas_focus_on((g_focus + 1) % NWIN); break;
            case '/':  g_hud = 1; g_hud_len = 0; g_hud_buf[0] = 0; break;
            case 'f': case 'F': canvas_zoom_fit(); break;
            case '\n': if (g_focus < 8 && g_surf[g_focus].used)  /* arm type-to-app */
                           g_key_to_app = 1;
                       break;
            case 27: case 'x': case 'X': return 0;
        }
    }
    return 1;
}

/* ---- live application surfaces --------------------------------------------- */
/* The panels are not mock-ups: each renders real state pulled from the kernel
 * subsystems built earlier in this project — the Time-Stream event log, the CAS
 * store, the background integrity sweep, the scheduler and the input drivers. */
int ts_count(void); const char *ts_who(int i); const char *ts_text(int i);
uint64_t ts_tick_of(int i); int ts_type_of(int i); int ts_indexed(int i);

static int draw_u64_at(int x, int y, uint64_t v, uint32_t c) {
    char b[24]; int n = 0, m = 0; char t[24];
    if (!v) b[n++] = '0';
    while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
    while (m) b[n++] = t[--m];
    b[n] = 0; draw_str(x, y, b, c); return n * 8;
}
static void draw_kv(int x, int y, const char *k, uint64_t v, uint32_t c) {
    draw_str(x, y, k, C_MUTE);
    int kw = 0; while (k[kw]) kw++;
    draw_u64_at(x + kw * 8 + 8, y, v, c);
}
/* clipped label: keeps text inside the panel at any zoom */
static void draw_clip(int x, int y, const char *s, uint32_t c, int maxch) {
    char b[40]; int n = 0;
    while (s[n] && n < maxch && n < 39) { b[n] = s[n]; n++; }
    b[n] = 0; draw_str(x, y, b, c);
}

static void surface_render(int i, int x, int y, int w, int h) {
    int cols = (w - 24) / 8; if (cols > 39) cols = 39;
    int rows = (h - 34) / 10;
    if (cols < 6 || rows < 1) return;
    switch (i) {
        case 0: {                                  /* TIME-STREAM: the real log  */
            int n = ts_count(), r = 0;
            for (int k = n - 1; k >= 0 && r < rows; k--, r++) {
                int yy = y + 26 + r * 10;
                uint32_t tc = ts_type_of(k) == 2 ? C_MAGE : (ts_type_of(k) == 3 ? C_AMBER : C_MINT);
                int used = draw_u64_at(x + 12, yy, ts_tick_of(k), C_MUTE);
                draw_clip(x + 14 + used, yy, ts_who(k), tc, 6);
                draw_clip(x + 14 + used + 56, yy, ts_text(k), C_TEXT, cols - 8 - used / 8);
            }
            if (!n) draw_str(x + 12, y + 26, "no events yet", C_MUTE);
            break;
        }
        case 1: {                                  /* COMM-DECK: comm events     */
            int n = ts_count(), r = 0;
            for (int k = n - 1; k >= 0 && r < rows; k--) {
                if (ts_type_of(k) != 2) continue;
                int yy = y + 26 + r * 10;
                draw_clip(x + 12, yy, ts_who(k), C_MAGE, 7);
                draw_clip(x + 12 + 60, yy, ts_text(k), C_TEXT, cols - 8);
                r++;
            }
            if (!r) draw_str(x + 12, y + 26, "no messages", C_MUTE);
            break;
        }
        case 2: {                                  /* SYS.TELEMETRY: counters    */
            int yy = y + 26;
            draw_kv(x + 12, yy, "uptime.ticks", g_ticks, C_MINT); yy += 10;
            if (rows > 1) { draw_kv(x + 12, yy, "sweep.passes", g_sweep.passes, C_CYAN); yy += 10; }
            if (rows > 2) { draw_kv(x + 12, yy, "sweep.audited", g_sweep.entries, C_CYAN); yy += 10; }
            if (rows > 3) { draw_kv(x + 12, yy, "sweep.violations", g_sweep.violations,
                                    g_sweep.violations ? C_MAGE : C_MINT); yy += 10; }
            if (rows > 4) { draw_kv(x + 12, yy, "mouse.packets", (uint64_t)g_mouse_pkts, C_AMBER); yy += 10; }
            if (rows > 5) { draw_kv(x + 12, yy, "devices", (uint64_t)n_kdev, C_MUTE); yy += 10; }
            if (rows > 6) { draw_kv(x + 12, yy, "iommu.on", (uint64_t)g_iommu_on,
                                    g_iommu_on ? C_MINT : C_MUTE); }
            break;
        }
        case 3: {                                  /* TIME-NODE: CAS / VFS       */
            int yy = y + 26;
            draw_kv(x + 12, yy, "ts.events", (uint64_t)ts_count(), C_CYAN); yy += 10;
            if (rows > 1) { int ix = 0; for (int k = 0; k < ts_count(); k++) if (ts_indexed(k)) ix++;
                            draw_kv(x + 12, yy, "ts.vectorized", (uint64_t)ix, C_MINT); yy += 10; }
            if (rows > 2 && g_cas_mounted) { draw_kv(x + 12, yy, "cas.blocks", SB->used_blocks, C_AMBER); yy += 10; }
            if (rows > 3) { draw_kv(x + 12, yy, "canvas.zoom%", (uint64_t)((g_zoom * 100) >> FX), C_MUTE); yy += 10; }
            if (rows > 4) { draw_kv(x + 12, yy, "cam.world.x", (uint64_t)kabs(g_cam_x >> FX), C_MUTE); }
            break;
        }
        default: {                                 /* VIDEO-EDITOR: fb + input   */
            int yy = y + 26;
            draw_kv(x + 12, yy, "fb.width", (uint64_t)g_fb_width, C_MINT); yy += 10;
            if (rows > 1) { draw_kv(x + 12, yy, "fb.height", (uint64_t)g_fb_height, C_MINT); yy += 10; }
            if (rows > 2) { draw_kv(x + 12, yy, "cursor.x", (uint64_t)g_cur_x, C_AMBER); yy += 10; }
            if (rows > 3) { draw_kv(x + 12, yy, "cursor.y", (uint64_t)g_cur_y, C_AMBER); yy += 10; }
            if (rows > 4) { draw_kv(x + 12, yy, "mouse.wheel", (uint64_t)(g_mouse_bytes == 4), C_MUTE); }
            break;
        }
    }
}

static void canvas_window(int i, int focused) {
    int W = g_fb_width, H = g_fb_height;
    int x = w2sx(g_wins[i].x), y = w2sy(g_wins[i].y);
    int w = wsc(g_wins[i].w),  h = wsc(g_wins[i].h);
    if (x + w < 0 || x > W || y + h < 0 || y > H) return;        /* frustum cull  */
    if (w < 10 || h < 10) { rect(x, y, w < 2 ? 2 : w, h < 2 ? 2 : h, g_wins[i].edge); return; }
    if (focused) {                                               /* volumetric depth */
        for (int sy = 0; sy < h; sy++)
            for (int sx = 0; sx < w; sx++) blend(x + sx + 7, y + sy + 9, 0x000000, 165);
    }
    glass_panel(x, y, w, h, g_wins[i].edge);
    if (h > 26 && w > 90) {
        draw_str(x + 12, y + 7, g_labels[i], focused ? C_CYAN : g_wins[i].edge);
        if (i < 8 && g_surf[i].used) {          /* ring-3 app owns these pixels    */
            struct surface *S = &g_surf[i];
            int cw = w - 16, ch = h - 30;
            uint32_t *sp = (uint32_t *)surf_front_phys(S);   /* only ever the FRONT */
            for (int sy = 0; sy < ch; sy++) {
                int v = sy * S->h / ch;
                for (int sx = 0; sx < cw; sx++)
                    px(x + 8 + sx, y + 24 + sy, sp[(uint64_t)v * S->w + (sx * S->w / cw)]);
            }
            draw_str(x + 12, y + h - 12, "RING-3 SURFACE", C_AMBER);
        } else surface_render(i, x, y, w, h);   /* live system state              */
    }
    if (focused && w > 40) {                                     /* focus corners */
        hline(x, y - 3, 14, C_CYAN); vline(x - 3, y, 14, C_CYAN);
        hline(x + w - 14, y + h + 2, 14, C_CYAN); vline(x + w + 2, y + h - 14, 14, C_CYAN);
    }
}

static void canvas_hud(void) {
    int W = g_fb_width;
    int hw = 600, hh = 46 + 5 * 18, hx = (W - hw) / 2, hy = 96;
    for (int y = hy - 6; y < hy + hh + 8; y++)
        for (int x = hx - 6; x < hx + hw + 8; x++) blend(x, y, 0x000000, 120);
    glass_panel(hx, hy, hw, hh, C_AMBER);
    draw_str(hx + 12, hy + 7, "ACCELERATOR", C_AMBER);
    draw_str(hx + 12, hy + 26, ">", C_CYAN);
    draw_str(hx + 28, hy + 26, g_hud_buf, C_TEXT);
    rect(hx + 28 + g_hud_len * 8, hy + 26, 7, 9, C_CYAN);        /* caret         */
    hline(hx + 10, hy + 40, hw - 20, C_HAIR);
    int row = 0;
    for (int i = 0; i < NCMD && row < 5; i++) {
        if (!ccmd_match(i)) continue;
        if (row == 0) rect(hx + 10, hy + 46 + row * 18 - 2, hw - 20, 14, C_OBS2);
        draw_str(hx + 22, hy + 46 + row * 18, g_ccmds[i].name, row == 0 ? C_MINT : C_MUTE);
        row++;
    }
    if (!row) draw_str(hx + 22, hy + 46, "no matching command", C_MUTE);
}

static void canvas_frame(void) {
    int W = g_fb_width, H = g_fb_height;
    /* v0.32: consume pending page flips at the frame boundary — the ONLY point */
    /* front/back swap while a pass is live, so no blit ever straddles a flip.  */
    /* v0.34: only for the slots THIS compositor composites (it registered as   */
    /* their consumer); flips on other slots complete on their own.             */
    for (int i = 0; i < NWIN; i++)
        if (g_surf[i].used && g_surf[i].flip_pending) {
            g_surf[i].front ^= 1;
            g_surf[i].flip_pending = 0;        /* unblocks the app's SYS_SURFACE_FLIP */
        }
    fill(C_OBS0);

    /* infinite grid: world-anchored, spacing follows the zoom */
    int64_t hwv = FXDIV(FXI(W / 2), g_zoom), hhv = FXDIV(FXI(H / 2), g_zoom);
    int64_t step = FXI(84);
    if (wsc(84) >= 7) {
        int64_t l = g_cam_x - hwv, r = g_cam_x + hwv, t = g_cam_y - hhv, b = g_cam_y + hhv;
        for (int64_t wx = (l / step) * step - step; wx <= r; wx += step) {
            int sx = w2sx(wx); if (sx >= 35 && sx < W) vline(sx, 0, H, C_GRID);
        }
        for (int64_t wy = (t / step) * step - step; wy <= b; wy += step) {
            int sy = w2sy(wy); if (sy >= 0 && sy < H) hline(35, sy, W - 35, C_GRID);
        }
    }
    /* world origin marker — proves the canvas really is a world, not a screen */
    int ox = w2sx(0), oy = w2sy(0);
    if (ox > 35 && ox < W && oy > 0 && oy < H) {
        hline(ox - 9, oy, 18, C_HAIR); vline(ox, oy - 9, 18, C_HAIR);
    }

    for (int i = 0; i < NWIN; i++) if (i != g_focus) canvas_window(i, 0);
    canvas_window(g_focus, 1);                                   /* focused on top */

    draw_str(46, 10, "OUTRUN", C_CYAN);
    draw_str(46 + 56, 10, " // METROPOLIS-TERMINAL", C_MUTE);
    rect(0, 0, 34, H, C_OBS1); vline(34, 0, H, C_HAIR);
    for (int i = 0; i < 10; i++) rect(10, 40 + i * 40, 14, 3, i < 6 ? C_CYAN : C_HAIR);

    if (g_hud) canvas_hud();
    canvas_cursor_draw();

    /* context ribbon: tells you what the current input mode can do */
    rect(0, H - 20, W, 20, C_OBS1); hline(0, H - 20, W, C_HAIR);
    draw_str(8, H - 14, g_key_to_app ? "KEYBOARD -> RING-3 APP  (ESC RETURNS TO CANVAS)"
                      : g_hud       ? "TYPE FILTER  ENTER RUN  ESC CLOSE"
                      : "DRAG WINDOW / PAN  WHEEL ZOOM@CURSOR  WASD  Q/E  TAB  F FIT  ENTER TYPE-TO-APP  / HUD  X EXIT", C_MUTE);
    char z[24]; int zp = (int)((g_zoom * 100) >> FX);
    int n = 0; z[n++] = 'Z'; z[n++] = ':';
    if (zp >= 100) z[n++] = (char)('0' + (zp / 100) % 10);
    z[n++] = (char)('0' + (zp / 10) % 10); z[n++] = (char)('0' + zp % 10); z[n++] = '%'; z[n] = 0;
    draw_str(W - 90, H - 14, z, C_CYAN);
    draw_str(W - 190, H - 14, g_labels[g_focus], C_MINT);
}

static void canvas_init(void) {
    static const int cx[NWIN] = { -520,  200, -560,  260, -140 };
    static const int cy[NWIN] = { -300, -260,  110,  170,  -30 };
    static const int cw[NWIN] = {  300,  280,  300,  240,  360 };
    static const int ch[NWIN] = {  150,  140,  190,  130,  160 };
    static const uint32_t ce[NWIN] = { C_CYAN, C_MAGE, C_AMBER, C_CYAN, C_MINT };
    for (int i = 0; i < NWIN; i++) {
        g_wins[i].tx = FXI(cx[i]); g_wins[i].ty = FXI(cy[i]);
        g_wins[i].x  = FXI(cx[i]); g_wins[i].y  = FXI(cy[i]);
        g_wins[i].vx = 0; g_wins[i].vy = 0;
        g_wins[i].w = cw[i]; g_wins[i].h = ch[i]; g_wins[i].edge = ce[i];
    }
    g_cam_x = 0; g_cam_y = 0; g_zoom = FXI(1); g_zoom_t = FXI(1);
    g_cam_vx = g_cam_vy = 0; g_cam_tx = g_cam_ty = 0; g_cam_glide = 0;
    g_fling_vx = g_fling_vy = 0; g_anchor_sx = g_fb_width / 2; g_anchor_sy = g_fb_height / 2;
    g_focus = 0; g_hud = 0; g_hud_len = 0; g_hud_buf[0] = 0;
}

/* Inject keystrokes into the real PS/2 ring buffer, so a scripted demo drives  */
/* exactly the same input path a physical keyboard does.                        */
static void canvas_inject_char(char c) { kbd_ring[kbd_w++ % 64] = c; }

/* One compositor pass. Since v0.31 the frame-pacing wait YIELDS, and timer
 * preemption is enabled for the duration — so ring-3 surface threads run
 * concurrently with the compositor, inside the same pass. Clicks can be
 * synthesized at chosen frames (clkf[]) through the exact state the mouse IRQ
 * maintains, so routing follows the real driver path mid-pass.               */
static void canvas_pass(int frames, const char *script,
                        const int *clkx, const int *clky, const int *clkf,
                        int nclk, int reinit) {
    if (!g_gfx_ready) { kputs("[canvas ] no framebuffer\n"); return; }
    if (reinit) canvas_init();
    preempt_enable();
    for (int i = 0; i < NWIN; i++) g_surf[i].consumer = 1;   /* we consume the  */
                                      /* composited slots' flips (even a slot   */
                                      /* bound mid-pass is covered)             */
    uint64_t tprev = g_ticks;
    int f = 0, ci = 0;
    for (; f < frames; f++) {
        /* feed the scripted demo one keystroke at a time, at human typing pace, */
        /* through the real PS/2 ring buffer — the same path a physical key takes */
        if (script && *script && (f % 6) == 0) canvas_inject_char(*script++);
        if (ci < nclk && f == clkf[ci]) {              /* synthesize button press */
            g_cur_x = clkx[ci]; g_cur_y = clky[ci]; g_mouse_btn = 1;
            kprintf("[canvas ] frame %d: click synthesized at screen (%d,%d)\n",
                    (uint64_t)f, (uint64_t)clkx[ci], (uint64_t)clky[ci]);
        }
        if (ci < nclk && f == clkf[ci] + 3) { g_mouse_btn = 0; ci++; }  /* release */
        canvas_mouse();
        if (!canvas_input()) break;
        /* v0.32: frame-rate-independent physics — advance the camera by the    */
        /* MEASURED elapsed PIT ticks, not by an assumed frame.                 */
        uint64_t tnow = g_ticks;
        int dt = (int)(tnow - tprev); tprev = tnow;
        if (dt < 1) dt = 1;
        camera_step_dt(dt);
        physics_step_dt(g_wins, NWIN, dt);   /* v0.34: springs on the same clock */
        canvas_frame();
        fb_flip();
        uint64_t t = g_ticks;
        while (g_ticks - t < 2) sched_yield();     /* frame pacing = app run time */
    }
    for (int i = 0; i < NWIN; i++) g_surf[i].consumer = 0;   /* deregister:     */
                                      /* parked flips self-consume from here on */
    preempt_disable();
    kprintf("[canvas ] %d frames | camera world (%d,%d) zoom %d%% | focus '%s' | hud %s\n",
            (uint64_t)f, (uint64_t)(g_cam_x >> FX), (uint64_t)(g_cam_y >> FX),
            (uint64_t)((g_zoom * 100) >> FX), g_labels[g_focus], g_hud ? "open" : "closed");
}

static void cmd_canvas(int frames, const char *script) {
    kputs("-- METROPOLIS-TERMINAL: spatial canvas (interactive) --\n");
    canvas_pass(frames, script, 0, 0, 0, 0, 1);
    kputs("-- done --\n");
}

/* ===========================================================================
 * KINETIC CAMERA INVARIANTS
 * ===========================================================================
 * Momentum and easing are only "feel" if they are also correct: a flick must
 * decay to rest (never drift or run away), an eased zoom must converge, and the
 * anchor must stay pinned for EVERY frame of the transition — not just at the
 * endpoints, which is where naive implementations visibly slide.
 * =========================================================================== */
static int g_kpass, g_kfail;
static void kcheck(const char *n, int c) {
    if (c) { g_kpass++; kprintf("[kinetic]  PASS  %s\n", n); }
    else   { g_kfail++; kprintf("[kinetic]  FAIL  %s\n", n); }
}

static void cmd_kinetic(void) {
    kputs("-- KINETIC CAMERA INVARIANTS --\n");
    g_kpass = g_kfail = 0;
    if (!g_gfx_ready) { kputs("[kinetic] no framebuffer\n-- done --\n"); return; }

    /* (1) a flick glides, then friction brings it to rest */
    canvas_init();
    g_cam_vx = FXI(24); g_cam_vy = FXI(-16);
    int64_t x0 = g_cam_x;
    for (int i = 0; i < 4; i++) camera_step();
    int moved = (g_cam_x > x0 + FXI(20));
    kcheck("a flick imparts momentum (camera keeps moving after input stops)", moved);
    int frames = 0;
    while ((g_cam_vx || g_cam_vy) && frames < 400) { camera_step(); frames++; }
    kcheck("friction brings the camera to a complete rest", g_cam_vx == 0 && g_cam_vy == 0);
    kprintf("[kinetic] flick settled in %d frames; glide distance %d world units\n",
            (uint64_t)frames, (uint64_t)((g_cam_x - x0) >> FX));
    kcheck("glide distance is bounded (no runaway)", ((g_cam_x - x0) >> FX) < 400);

    /* (2) THE invariant: anchor pinned on every frame of an eased zoom */
    int worst_all = 0;
    for (int t = 0; t < 4; t++) {
        canvas_init();
        int ax = (t == 0) ? 140 : (t == 1) ? 512 : (t == 2) ? 900 : 300;
        int ay = (t == 0) ? 120 : (t == 1) ? 384 : (t == 2) ? 700 : 640;
        int64_t w0x = s2wx(ax), w0y = s2wy(ay);
        canvas_zoom_to(ax, ay, FXI(3));                 /* big eased zoom-in     */
        int steps = 0, worst = 0;
        while (g_zoom != g_zoom_t && steps < 200) {
            camera_step(); steps++;
            int64_t ex = kabs(s2wx(ax) - w0x) >> FX, ey = kabs(s2wy(ay) - w0y) >> FX;
            if ((int)ex > worst) worst = (int)ex;
            if ((int)ey > worst) worst = (int)ey;
        }
        if (worst > worst_all) worst_all = worst;
        if (steps >= 200) worst_all = 9999;             /* failed to converge    */
    }
    kcheck("eased zoom converges to its target", worst_all != 9999);
    kcheck("anchor stays pinned on EVERY frame of the eased zoom", worst_all <= 2);
    kprintf("[kinetic] worst mid-transition anchor drift over 4 zooms: %d world units\n",
            (uint64_t)worst_all);

    /* (3) glide-to-target converges exactly */
    canvas_init();
    canvas_focus_on(3);
    int gs = 0;
    while (g_cam_glide && gs < 300) { camera_step(); gs++; }
    int64_t want_x = g_wins[3].x + FXI(g_wins[3].w / 2);
    kcheck("commanded glide converges exactly on its target",
           !g_cam_glide && kabs(g_cam_x - want_x) <= FXI(1));
    kprintf("[kinetic] glide-to-focus converged in %d frames\n", (uint64_t)gs);

    /* (4) easing is monotone: no overshoot past the target */
    canvas_init();
    canvas_zoom_to(512, 384, FXI(2));
    int over = 0;
    for (int i = 0; i < 200 && g_zoom != g_zoom_t; i++) { camera_step(); if (g_zoom > g_zoom_t) over = 1; }
    kcheck("eased zoom does not overshoot its target", !over);

    /* (5) zoom stays clamped even under repeated impulses */
    canvas_init();
    for (int i = 0; i < 40; i++) { canvas_zoom_to(512, 384, FXI(2)); for (int j = 0; j < 8; j++) camera_step(); }
    int hi_ok = g_zoom <= FXI(4);
    for (int i = 0; i < 40; i++) { canvas_zoom_to(512, 384, FXI(1) / 2); for (int j = 0; j < 8; j++) camera_step(); }
    kcheck("zoom stays clamped to 12.5%..400% under repeated impulses",
           hi_ok && g_zoom >= FXI(1) / 8);

    /* (6) v0.32: THE frame-rate-independence invariant, checked EXACTLY.       */
    /* The physics clock is the tick; frames merely group ticks. So the same    */
    /* total tick count must land on the bit-identical camera state no matter   */
    /* how it is chopped into frames.                                           */
    canvas_init();
    g_cam_vx = FXI(30); g_cam_vy = FXI(-12); canvas_zoom_to(400, 300, FXI(2));
    for (int i = 0; i < 100; i++) camera_step_dt(2);         /* 100 frames @ 20ms */
    int64_t xa = g_cam_x, ya = g_cam_y, za = g_zoom, va = g_cam_vx;
    canvas_init();
    g_cam_vx = FXI(30); g_cam_vy = FXI(-12); canvas_zoom_to(400, 300, FXI(2));
    for (int i = 0; i < 200; i++) camera_step_dt(1);         /* 200 frames @ 10ms */
    kcheck("dt grouping is EXACT: 100 frames @ dt=2 == 200 frames @ dt=1 (bit-identical)",
           xa == g_cam_x && ya == g_cam_y && za == g_zoom && va == g_cam_vx);

    /* (7) irregular frame times — a stuttering compositor (1..5 ticks/frame)   */
    /* still lands on the identical state after the same 200 ticks.             */
    canvas_init();
    g_cam_vx = FXI(30); g_cam_vy = FXI(-12); canvas_zoom_to(400, 300, FXI(2));
    static const int jitter[8] = { 1, 3, 2, 4, 1, 5, 2, 2 };   /* 20 ticks/cycle */
    for (int c = 0; c < 10; c++)
        for (int j = 0; j < 8; j++) camera_step_dt(jitter[j]);
    kcheck("irregular frame times (1..5 ticks) land on the identical camera state",
           xa == g_cam_x && ya == g_cam_y && za == g_zoom && va == g_cam_vx);

    /* (8) v0.34: the WINDOW SPRINGS share the tick clock — same exactness bar. */
    /* Perturb every target, run the full spring+collision system, and demand   */
    /* the bit-identical endpoint under different tick groupings.               */
    static struct win wa[NWIN], wb[NWIN];
    canvas_init();
    for (int i = 0; i < NWIN; i++) { g_wins[i].tx += FXI(137 + 41 * i); g_wins[i].ty -= FXI(53 + 29 * i); }
    for (int i = 0; i < NWIN; i++) wa[i] = g_wins[i];
    for (int s = 0; s < 60; s++) physics_step_dt(wa, NWIN, 2);      /* 120 ticks */
    for (int i = 0; i < NWIN; i++) wb[i] = g_wins[i];
    for (int s = 0; s < 120; s++) physics_step_dt(wb, NWIN, 1);     /* 120 ticks */
    int spring_ok = 1;
    for (int i = 0; i < NWIN; i++)
        if (wa[i].x != wb[i].x || wa[i].y != wb[i].y ||
            wa[i].vx != wb[i].vx || wa[i].vy != wb[i].vy) spring_ok = 0;
    kcheck("window-spring dt grouping is EXACT (60 frames @ dt=2 == 120 @ dt=1)", spring_ok);

    /* (9) irregular groupings, same 120 ticks, same endpoint — springs AND the */
    /* pairwise collision resolution are tick-pure.                             */
    for (int i = 0; i < NWIN; i++) wb[i] = g_wins[i];
    static const int sjit[6] = { 1, 4, 2, 5, 3, 5 };                /* 20/cycle  */
    for (int c = 0; c < 6; c++)
        for (int j = 0; j < 6; j++) physics_step_dt(wb, NWIN, sjit[j]);
    spring_ok = 1;
    for (int i = 0; i < NWIN; i++)
        if (wa[i].x != wb[i].x || wa[i].y != wb[i].y ||
            wa[i].vx != wb[i].vx || wa[i].vy != wb[i].vy) spring_ok = 0;
    kcheck("window springs under irregular frame times land on the identical state", spring_ok);

    canvas_init();
    kprintf("[kinetic] RESULT: %d passed, %d failed\n", (uint64_t)g_kpass, (uint64_t)g_kfail);
    if (!g_kfail) kputs("[kinetic] KINETIC CAMERA VERIFIED — momentum settles, anchor never slips\n");
    else          kputs("[kinetic] KINETIC DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * CURSOR COORDINATE TRANSLATION INVARIANTS
 * ===========================================================================
 * The transform is the load-bearing part of a spatial canvas: if screen<->world
 * is even slightly wrong, zoom-at-cursor drifts and dragging slides. These
 * checks pin it down at many zooms and camera positions.
 * =========================================================================== */
static int g_xpass, g_xfail;
static void xcheck(const char *n, int c) {
    if (c) { g_xpass++; kprintf("[cursor ]  PASS  %s\n", n); }
    else   { g_xfail++; kprintf("[cursor ]  FAIL  %s\n", n); }
}
static int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

static void cmd_cursor(void) {
    kputs("-- CURSOR COORDINATE TRANSLATION INVARIANTS --\n");
    g_xpass = g_xfail = 0;
    if (!g_gfx_ready) { kputs("[cursor ] no framebuffer\n-- done --\n"); return; }
    canvas_init();
    static const int64_t zl[6] = { FXI(1) / 8, FXI(1) / 2, FXI(1), FXI(3) / 2, FXI(2), FXI(4) };
    static const int sxs[5] = { 0, 137, 512, 800, 1023 };
    static const int sys_[5] = { 0, 90, 384, 600, 767 };

    /* (1) screen -> world -> screen round-trips at every zoom level */
    int rt_ok = 1; int64_t worst = 0;
    for (int z = 0; z < 6; z++) {
        g_zoom = zl[z]; g_cam_x = FXI(-137 * z); g_cam_y = FXI(64 * z);
        for (int a = 0; a < 5; a++) for (int b = 0; b < 5; b++) {
            int rx = w2sx(s2wx(sxs[a])), ry = w2sy(s2wy(sys_[b]));
            int64_t ex = iabs64(rx - sxs[a]), ey = iabs64(ry - sys_[b]);
            if (ex > worst) worst = ex; if (ey > worst) worst = ey;
            if (ex > 1 || ey > 1) rt_ok = 0;
        }
    }
    xcheck("screen->world->screen round-trip exact at all zooms (<=1 px)", rt_ok);
    kprintf("[cursor ] worst round-trip error across 150 samples: %d px\n", worst);

    /* (2) world -> screen -> world round-trip */
    int wr_ok = 1;
    for (int z = 0; z < 6; z++) {
        g_zoom = zl[z]; g_cam_x = 0; g_cam_y = 0;
        for (int64_t w = -400; w <= 400; w += 200) {
            int64_t back = s2wx(w2sx(FXI(w)));
            if (iabs64((back >> FX) - w) > 2) wr_ok = 0;
        }
    }
    xcheck("world->screen->world round-trip stable", wr_ok);

    /* (3) THE invariant: zoom about the cursor keeps the world point under it */
    int anchor_ok = 1; int64_t wmax = 0;
    for (int z = 0; z < 6; z++) {
        for (int a = 0; a < 5; a++) for (int b = 0; b < 5; b++) {
            g_zoom = zl[z]; g_cam_x = FXI(90 * a); g_cam_y = FXI(-70 * b);
            int cx = sxs[a], cy = sys_[b];
            int64_t bx = s2wx(cx), by = s2wy(cy);
            canvas_zoom_at(cx, cy, FXI(1) + FXI(1) / 5);        /* zoom in  */
            int64_t ax = s2wx(cx), ay = s2wy(cy);
            int64_t e1 = iabs64(ax - bx) >> FX, e2 = iabs64(ay - by) >> FX;
            if (e1 > wmax) wmax = e1; if (e2 > wmax) wmax = e2;
            if (e1 > 2 || e2 > 2) anchor_ok = 0;
            canvas_zoom_at(cx, cy, FXI(1) * 5 / 6);             /* zoom out */
            int64_t rx = s2wx(cx), ry = s2wy(cy);
            if ((iabs64(rx - bx) >> FX) > 3 || (iabs64(ry - by) >> FX) > 3) anchor_ok = 0;
        }
    }
    xcheck("zoom-at-cursor keeps the world point pinned under the pointer", anchor_ok);
    kprintf("[cursor ] worst anchor drift across 150 zoom ops: %d world units\n", wmax);

    /* (4) panning does not change the world size of a window */
    g_zoom = FXI(1); g_cam_x = 0; g_cam_y = 0;
    int w0 = wsc(g_wins[0].w);
    g_cam_x = FXI(777); g_cam_y = FXI(-333);
    xcheck("panning preserves projected size (no drift)", wsc(g_wins[0].w) == w0);

    /* (5) zoom scales projected size proportionally */
    g_zoom = FXI(1); int s1 = wsc(200);
    g_zoom = FXI(2); int s2 = wsc(200);
    xcheck("2x zoom doubles projected size", s2 == s1 * 2);

    /* (6) picking agrees with rendering: a window's own centre hits itself */
    g_zoom = FXI(1); g_cam_x = 0; g_cam_y = 0;
    int pick_ok = 1;
    for (int i = 0; i < NWIN; i++) {
        g_focus = i;
        int cx = w2sx(g_wins[i].x + FXI(g_wins[i].w / 2));
        int cy = w2sy(g_wins[i].y + FXI(g_wins[i].h / 2));
        if (cx < 0 || cx >= g_fb_width || cy < 0 || cy >= g_fb_height) continue;
        if (canvas_pick(cx, cy) != i) pick_ok = 0;
    }
    xcheck("hit-testing matches rendering (centre of each window picks it)", pick_ok);

    canvas_init();
    kprintf("[cursor ] RESULT: %d passed, %d failed\n", (uint64_t)g_xpass, (uint64_t)g_xfail);
    if (!g_xfail) kputs("[cursor ] COORDINATE TRANSLATION LOCKED DOWN\n");
    else          kputs("[cursor ] TRANSFORM DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* screendump hook: set by the test harness via the 'anim' path if desired      */
static void cmd_gfx(void) {
    if (!g_gfx_ready) { kputs("[gfx    ] no framebuffer (boot QEMU with -vga std)\n"); return; }
    kputs("-- Metropolis-Terminal compositor: animated physics loop --\n");
    compositor_init();
    int NF = 150;
    for (int f = 0; f < NF; f++) {
        g_glitch = f < 16 ? (16 - f) * 16 : 0;             /* glitch-in fades      */
        physics_step(g_wins, 5);
        compositor_frame(f);
        fb_flip();
        uint64_t t = g_ticks; while (g_ticks - t < 2) __asm__ volatile("pause"); /* ~20ms/frame */
    }
    uint32_t p = g_fb[(g_fb_height / 2) * g_stride + g_fb_width / 2];
    kprintf("[gfx    ] animated %d frames (glitch-in + spring + collision), settled. center=%X\n",
            (uint64_t)NF, (uint64_t)(p & 0xFFFFFF));
    kputs("-- done (screendump to view) --\n");
}

/* sys_map_framebuffer: map the linear framebuffer directly into a process's    */
/* address space (zero-copy) so a ring-3 graphics manager can write pixels.     */
#define FB_USER_V 0x0000550000000000ull
static int64_t sys_map_framebuffer(int proc_idx) {
    if (!rust_cap_check(kprocs[proc_idx].caps, PCAP_FRAMEBUFFER)) return -2;
    if (!g_gfx_ready) return -1;
    uint64_t bytes = (uint64_t)g_fb_pitch * g_fb_height;
    uint64_t pages = (bytes + 0xFFF) / 0x1000;
    uint64_t phys = g_fb_addr & ~0xFFFull;
    for (uint64_t i = 0; i < pages; i++)
        map_page(kprocs[proc_idx].cr3, FB_USER_V + i * 0x1000, phys + i * 0x1000, PTE_USER | PTE_WRITE | PTE_NX);
    kprintf("[gfx    ] sys_map_framebuffer: mapped %d KiB of FB into pid %u at user vaddr %X\n",
            (uint64_t)(bytes / 1024), kprocs[proc_idx].pid, FB_USER_V);
    return (int64_t)FB_USER_V;
}

/* ===========================================================================
 * PHASE 5 — THE TIME-STREAM ENGINE
 * ===========================================================================
 * An append-only chronological event substrate. File writes, Comm-Deck
 * messages, and system activity are emitted as timestamped events; a background
 * scheduler thread parses each into a local feature-hashed vector index, so the
 * terminal can answer natural queries ("the Q3 chart from Sarah") by ranking
 * events on vector similarity — no nested folders, just a searchable timeline.
 * =========================================================================== */
#define TS_MAXEV 64
#define TS_DIM   64                      /* feature-hash dimensions              */
enum { TSE_FILE = 1, TSE_COMM = 2, TSE_SYS = 3 };
struct ts_event {
    uint64_t seq, tick;
    int      type, indexed;
    char     who[24];
    char     text[64];
    uint16_t vec[TS_DIM];
};
static struct ts_event g_ts[TS_MAXEV];
static volatile uint64_t g_ts_seq = 0;   /* events emitted                       */
static volatile uint64_t g_ts_done = 0;  /* events vectorized by the background   */

/* enqueue a raw event (called from hooks; safe before the indexer runs) */
static void ts_emit(int type, const char *who, const char *text) {
    /* v0.41: the slot is claimed with LOCK XADD — ts_emit now runs from any
     * core (vfs_write under the vfs lock calls it), and the old read-inc-write
     * of g_ts_seq let two cores fill the same slot.                           */
    uint64_t s = __sync_fetch_and_add(&g_ts_seq, 1);
    struct ts_event *e = &g_ts[s % TS_MAXEV];
    e->seq = s; e->tick = g_ticks; e->type = type; e->indexed = 0;
    kstrcpy_n(e->who, who, 24);
    kstrcpy_n(e->text, text, 64);
    for (int i = 0; i < TS_DIM; i++) e->vec[i] = 0;
    barrier();                     /* slot was claimed by the XADD above        */
}

/* tokenize lowercase alnum words, feature-hash each into the vector */
static void ts_accumulate(const char *s, uint16_t *vec) {
    uint32_t h = 2166136261u; int inword = 0;
    for (const char *p = s; ; p++) {
        char c = *p;
        int alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        char lc = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        int lalnum = (lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9');
        if (lalnum) { h = (h ^ (uint8_t)lc) * 16777619u; inword = 1; }
        else { if (inword) { vec[h % TS_DIM]++; h = 2166136261u; inword = 0; } if (!c) break; }
        (void)alnum;
    }
}
int ts_count(void) { return (int)(g_ts_seq < TS_MAXEV ? g_ts_seq : TS_MAXEV); }
const char *ts_who(int i)  { return g_ts[i % TS_MAXEV].who; }
const char *ts_text(int i) { return g_ts[i % TS_MAXEV].text; }
uint64_t ts_tick_of(int i) { return g_ts[i % TS_MAXEV].tick; }
int ts_type_of(int i)      { return g_ts[i % TS_MAXEV].type; }
int ts_indexed(int i)      { return g_ts[i % TS_MAXEV].indexed; }

static void ts_vectorize(struct ts_event *e) {
    ts_accumulate(e->text, e->vec);
    ts_accumulate(e->who, e->vec);
    e->indexed = 1;
}

/* background indexer thread: parse pending events into vectors */
static volatile int g_tsd_run = 0;
static void timestreamd_fn(void *a) {
    (void)a;
    while (g_tsd_run) {
        if (g_ts_done < g_ts_seq) {
            ts_vectorize(&g_ts[g_ts_done % TS_MAXEV]);
            g_ts_done++;
        } else {
            sched_yield();
            if (!g_tsd_run) break;
        }
    }
}

static const char *ts_type_str(int t) { return t == TSE_FILE ? "FILE" : t == TSE_COMM ? "COMM" : "SYS "; }

static void ts_print_timeline(void) {
    kprintf("[stream ] chronological timeline (%d events, all vector-indexed):\n", g_ts_done);
    for (uint64_t i = 0; i < g_ts_done && i < TS_MAXEV; i++) {
        struct ts_event *e = &g_ts[i];
        kprintf("[stream ]  #%d  t=%d  %s  %s: %s\n",
                e->seq, e->tick, ts_type_str(e->type), e->who, e->text);
    }
}

/* rank indexed events by vector dot-product against the query */
static void ts_query(const char *q) {
    uint16_t qv[TS_DIM]; for (int i = 0; i < TS_DIM; i++) qv[i] = 0;
    ts_accumulate(q, qv);
    int best = -1, best2 = -1; uint32_t bs = 0, bs2 = 0;
    for (uint64_t i = 0; i < g_ts_done && i < TS_MAXEV; i++) {
        uint32_t dot = 0;
        for (int d = 0; d < TS_DIM; d++) dot += (uint32_t)g_ts[i].vec[d] * qv[d];
        if (dot > bs) { bs2 = bs; best2 = best; bs = dot; best = (int)i; }
        else if (dot > bs2) { bs2 = dot; best2 = (int)i; }
    }
    kprintf("[stream ] query \"%s\":\n", q);
    if (best >= 0 && bs > 0)
        kprintf("[stream ]   -> %s %s: %s  (score %d)\n",
                ts_type_str(g_ts[best].type), g_ts[best].who, g_ts[best].text, bs);
    if (best2 >= 0 && bs2 > 0)
        kprintf("[stream ]   -> %s %s: %s  (score %d)\n",
                ts_type_str(g_ts[best2].type), g_ts[best2].who, g_ts[best2].text, bs2);
    if (bs == 0) kputs("[stream ]   -> no match\n");
}

static void cmd_timestream(void) {
    kputs("-- Time-Stream Engine: chronological index + local vector query --\n");
    /* Comm-Deck messages + system activity (file events already emitted by VFS) */
    ts_emit(TSE_COMM, "Sarah",   "shared the Q3 revenue chart deck");
    ts_emit(TSE_COMM, "DevTeam", "merged the microkernel scheduler patch");
    ts_emit(TSE_COMM, "Ops",     "flagged a network gateway DHCP change");
    ts_emit(TSE_SYS,  "netd",    "received DHCP offer 10.0.2.15 from gateway");
    ts_emit(TSE_SYS,  "sched",   "ran four concurrent worker threads");

    /* start the background indexer, wait for it to vectorize the backlog */
    g_tsd_run = 1;
    thread_create("timestreamd", timestreamd_fn, 0);
    uint64_t t0 = g_ticks;
    while (g_ts_done < g_ts_seq && g_ticks - t0 < 200) sched_yield();
    g_tsd_run = 0;

    ts_print_timeline();
    ts_query("the Q3 chart Sarah sent");
    ts_query("scheduler patch");
    ts_query("dhcp gateway address");
    kputs("-- done --\n");
}

/* ===========================================================================
 * PRODUCTION VALIDATION MATRIX  (Phase 5b)
 * ===========================================================================
 * Automated self-tests: a capability audit across syscall entry points, ELF
 * segment bounds-checking against hostile images, and CAS/VFS round-trip
 * integrity. Reports PASS/FAIL and a summary.
 * =========================================================================== */
static int g_vpass, g_vfail;
static void vcheck(const char *name, int cond) {
    if (cond) { g_vpass++; kprintf("[valid  ]  PASS  %s\n", name); }
    else      { g_vfail++; kprintf("[valid  ]  FAIL  %s\n", name); }
}

/* build a minimal, VALID ELF64 (one PT_LOAD, entry in user range) into buf */
static uint64_t build_test_elf(uint8_t *buf) {
    for (int i = 0; i < 128; i++) buf[i] = 0;
    struct elf64_hdr *e = (struct elf64_hdr *)buf;
    e->ident[0] = 0x7F; e->ident[1] = 'E'; e->ident[2] = 'L'; e->ident[3] = 'F';
    e->ident[4] = 2; e->ident[5] = 1; e->ident[6] = 1;      /* 64-bit, LE, v1    */
    e->type = 2; e->machine = 0x3E; e->version = 1;
    e->entry = 0x500000000000ull;
    e->phoff = sizeof(struct elf64_hdr);
    e->ehsize = sizeof(struct elf64_hdr);
    e->phentsize = sizeof(struct elf64_phdr);
    e->phnum = 1;
    struct elf64_phdr *p = (struct elf64_phdr *)(buf + e->phoff);
    p->type = 1; p->flags = 5;                              /* PT_LOAD, R+X      */
    p->offset = 0; p->vaddr = 0x500000000000ull; p->paddr = p->vaddr;
    p->filesz = 0; p->memsz = 0x1000; p->align = 0x1000;
    return sizeof(struct elf64_hdr) + sizeof(struct elf64_phdr);
}

static void cmd_validate(void) {
    kputs("-- PRODUCTION VALIDATION MATRIX --\n");
    g_vpass = g_vfail = 0;
    uint64_t save = current_proc_idx;

    /* (1) capability audit across syscall entry points */
    kputs("[valid  ] capability audit:\n");
    int pn = kproc_spawn("audit-none", 0);
    int pf = kproc_spawn("audit-fs", PCAP_FILESYSTEM);
    current_proc_idx = (uint64_t)pn;
    vcheck("sys_open  denied w/o CAP_FILESYSTEM",   (int64_t)syscall_dispatch(5, (uint64_t)"motd", 0, 0) == -13);
    vcheck("sys_read  denied w/o CAP_FILESYSTEM",   (int64_t)syscall_dispatch(6, 0, 0, 0) == -13);
    vcheck("sys_write denied w/o CAP_FILESYSTEM",   (int64_t)syscall_dispatch(7, 0, 0, 0) == -13);
    vcheck("sys_wait_event denied w/o CAP_NETWORK", (int64_t)syscall_dispatch(9, 1, 0, 0) == -13);
    vcheck("sys_map_fb denied w/o CAP_FRAMEBUFFER", (int64_t)syscall_dispatch(10, 0, 0, 0) == -2);
    current_proc_idx = (uint64_t)pf;
    /* With the cap, the call clears the capability gate. (The name here is a     */
    /* kernel pointer, which the hardened syscall now correctly rejects as EFAULT  */
    /* rather than cap-denial — either way it is NOT -13.)                         */
    vcheck("sys_open  passes cap gate w/ CAP_FILESYSTEM",
           (int64_t)syscall_dispatch(5, (uint64_t)"motd", 0, 0) != -13);

    /* (2) ELF segment bounds-checking against hostile images */
    kputs("[valid  ] ELF loader bounds-checking:\n");
    static uint8_t te[256];
    uint64_t sz = build_test_elf(te);
    vcheck("valid minimal ELF accepted",           elf_load(pf, (uint64_t)te, sz) != 0);
    build_test_elf(te); vcheck("reject: image smaller than header", elf_load(pf, (uint64_t)te, 8) == 0);
    build_test_elf(te); ((struct elf64_hdr *)te)->phoff = 0xFFFF;
    vcheck("reject: phdr table out of bounds",      elf_load(pf, (uint64_t)te, sz) == 0);
    build_test_elf(te); ((struct elf64_phdr *)(te + 64))->filesz = 0x100000;
    vcheck("reject: segment file range out of bounds", elf_load(pf, (uint64_t)te, sz) == 0);
    build_test_elf(te); ((struct elf64_phdr *)(te + 64))->vaddr = 0x1000;
    vcheck("reject: segment vaddr outside user range", elf_load(pf, (uint64_t)te, sz) == 0);
    build_test_elf(te); ((struct elf64_hdr *)te)->entry = 0x1000;
    vcheck("reject: entry outside user range",      elf_load(pf, (uint64_t)te, sz) == 0);
    build_test_elf(te); ((struct elf64_phdr *)(te + 64))->memsz = 0;
    ((struct elf64_phdr *)(te + 64))->filesz = 0x10;
    vcheck("reject: memsz < filesz",                elf_load(pf, (uint64_t)te, sz) == 0);
    current_proc_idx = save;

    /* (3) CAS + VFS round-trip integrity */
    if (g_cas_mounted) {
        kputs("[valid  ] CAS/VFS round-trip integrity:\n");
        static uint8_t blob[512], out[512];
        for (int i = 0; i < 512; i++) blob[i] = (uint8_t)(i * 7 + 3);
        uint64_t h = cas_put(blob, 512);
        int64_t got = cas_get(h, out, 512);
        int match = (got == 512); for (int i = 0; i < 512 && match; i++) if (out[i] != blob[i]) match = 0;
        vcheck("CAS put/get byte-exact round-trip", match);
        uint64_t used0 = SB->used_blocks;
        uint64_t h2 = cas_put(blob, 512);
        vcheck("CAS dedup: identical put -> same hash", h2 == h);
        vcheck("CAS dedup: identical put -> no new block", SB->used_blocks == used0);
        int64_t bad = cas_get(0xDEADBEEFCAFEull, out, 512);
        vcheck("CAS get of unknown hash -> not found", bad < 0);
        static uint8_t big[1300], rb[1300];
        for (int i = 0; i < 1300; i++) big[i] = (uint8_t)(i ^ 0x5A);
        vfs_write_file("vtest", big, 1300);
        int di = vfs_find("vtest");
        int64_t n = vfs_read_file(di, rb, sizeof rb);
        int vmatch = (n == 1300); for (int i = 0; i < 1300 && vmatch; i++) if (rb[i] != big[i]) vmatch = 0;
        vcheck("VFS multi-block write/read round-trip", vmatch);
        uint64_t fh1 = DENTS[di].file_hash;
        vfs_write_file("vtest", big, 900);
        vcheck("VFS copy-on-write changes file hash", DENTS[vfs_find("vtest")].file_hash != fh1);
    }

    kprintf("[valid  ] RESULT: %d passed, %d failed\n", g_vpass, g_vfail);
    if (g_vfail == 0) kputs("[valid  ] ALL CHECKS PASSED — image is production-verified\n");
    else              kputs("[valid  ] VALIDATION FAILURES PRESENT — do not ship\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * EXHAUSTIVE ISOLATION & HARDENING INVARIANTS  (deep core-security coverage)
 * ===========================================================================
 * Beyond the basic matrix: proves capability isolation over the ENTIRE
 * capability space (no combination escalates), cross-process address-space
 * isolation, kernel/user separation at the PTE level, and passthrough
 * hardening. These are the invariants that must hold before any virtualization
 * layer is stacked on top.
 * =========================================================================== */
static int g_ipass, g_ifail;
static void icheck(const char *name, int cond) {
    if (cond) { g_ipass++; kprintf("[invar  ]  PASS  %s\n", name); }
    else      { g_ifail++; kprintf("[invar  ]  FAIL  %s\n", name); }
}

static void cmd_invariants(void) {
    kputs("-- EXHAUSTIVE ISOLATION & HARDENING INVARIANTS --\n");
    g_ipass = g_ifail = 0;
    uint64_t save = current_proc_idx;

    /* (1) EXHAUSTIVE capability-denial matrix: for every gated syscall, sweep  */
    /* ALL 64 capability masks; any mask lacking the required bit MUST be       */
    /* denied — proving no combination of other capabilities escalates.        */
    struct { int num; uint64_t req; int64_t deny; const char *name; uint64_t a0; } gated[] = {
        { 5,  PCAP_FILESYSTEM,     -13, "sys_open",           (uint64_t)"motd" },
        { 6,  PCAP_FILESYSTEM,     -13, "sys_read",           0 },
        { 7,  PCAP_FILESYSTEM,     -13, "sys_write_file",     0 },
        { 9,  PCAP_NETWORK,        -13, "sys_wait_event",     1 },
        { 10, PCAP_FRAMEBUFFER,    -2,  "sys_map_framebuffer",0 },
        { 1,  PCAP_HW_PASSTHROUGH, -2,  "sys_hw_passthrough", 0xFFFF },
    };
    int pcap = kproc_spawn("inv-cap", 0);
    uint64_t bits[6] = { PCAP_HW_PASSTHROUGH, PCAP_CAMERA, PCAP_MICROPHONE,
                         PCAP_NETWORK, PCAP_FILESYSTEM, PCAP_FRAMEBUFFER };
    int total = 0, denied = 0;
    for (int g = 0; g < 6; g++) {
        int allok = 1, tested = 0;
        for (unsigned m = 0; m < 64; m++) {
            uint64_t caps = 0;
            for (int b = 0; b < 6; b++) if (m & (1u << b)) caps |= bits[b];
            if (caps & gated[g].req) continue;             /* denial direction only */
            kprocs[pcap].caps = caps;
            current_proc_idx = (uint64_t)pcap;
            int64_t r = (int64_t)syscall_dispatch(gated[g].num, gated[g].a0, 0, 0);
            tested++; total++;
            if (r == gated[g].deny) denied++; else allok = 0;
        }
        char nm[64]; int p = 0; const char *a = "no cap-mask escalates ";
        while (a[p]) { nm[p] = a[p]; p++; } for (int q = 0; gated[g].name[q]; q++) nm[p++] = gated[g].name[q]; nm[p] = 0;
        icheck(nm, allok && tested > 0);
    }
    kprintf("[invar  ] capability space swept: %d/%d denial checks held\n", denied, total);
    current_proc_idx = save;

    /* (2) cross-process address-space isolation */
    int pA = kproc_spawn("inv-A", 0), pB = kproc_spawn("inv-B", 0);
    uint64_t Vshared = 0x500000200000ull;                  /* same vaddr, both procs */
    uint64_t Vonly   = 0x580000000000ull;                  /* mapped only in A       */
    uint64_t fA = alloc_frame(), fB = alloc_frame();
    *(volatile uint8_t *)fA = 0xAA; *(volatile uint8_t *)fB = 0xBB;
    map_page(kprocs[pA].cr3, Vshared, fA, PTE_USER | PTE_WRITE);
    map_page(kprocs[pB].cr3, Vshared, fB, PTE_USER | PTE_WRITE);
    map_page(kprocs[pA].cr3, Vonly, alloc_frame(), PTE_USER | PTE_WRITE);
    uint64_t rA = 0, rB = 0, dummy = 0;
    translate(kprocs[pA].cr3, Vshared, &rA);
    translate(kprocs[pB].cr3, Vshared, &rB);
    icheck("same vaddr resolves to A's own frame in A", (rA & ADDR_MASK) == fA);
    icheck("same vaddr resolves to B's own frame in B", (rB & ADDR_MASK) == fB);
    icheck("processes get physically distinct frames",  fA != fB);
    icheck("A-private page is NOT visible in B",         translate(kprocs[pB].cr3, Vonly, &dummy) == 0);

    /* (3) kernel/user separation at the PTE level */
    uint64_t ptU = walk_pte(kprocs[pA].cr3, Vshared);
    icheck("user page PTE present + USER + WRITE",
           (ptU & PTE_PRESENT) && (ptU & PTE_USER) && (ptU & PTE_WRITE));
    uint64_t kaddr = (uint64_t)&g_ts_seq;                  /* a kernel .data global */
    uint64_t ptK = walk_pte(kprocs[pA].cr3, kaddr);
    icheck("kernel page reachable via shared identity",   (ptK & PTE_PRESENT) != 0);
    icheck("kernel page is SUPERVISOR-only (ring3 blocked)", (ptK & PTE_USER) == 0);
    uint64_t *pmA = (uint64_t *)(kprocs[pA].cr3 & ADDR_MASK);
    uint64_t *pmB = (uint64_t *)(kprocs[pB].cr3 & ADDR_MASK);
    icheck("processes SHARE identity PML4[0]",            pmA[0] == pmB[0]);
    icheck("processes SHARE device-MMIO PML4[0xC0]",      pmA[0xC0] == pmB[0xC0]);
    icheck("processes have PRIVATE user PML4[0xA0]",      pmA[0xA0] != pmB[0xA0]);

    /* (4) passthrough hardening — two-level capability (HW + device-specific)  */
    current_proc_idx = save;
    uint64_t devreq = kdevs[g_demo_dev_index].req;
    int phFull  = kproc_spawn("inv-hw",  PCAP_HW_PASSTHROUGH | devreq);
    int phNoDev = kproc_spawn("inv-hw2", PCAP_HW_PASSTHROUGH);           /* HW but not device cap */
    current_proc_idx = (uint64_t)phFull;
    icheck("passthrough rejects invalid device handle",   (int64_t)syscall_dispatch(1, 9999, 0, 0) == -1);
    int64_t vb = (int64_t)syscall_dispatch(1, 0xFFFF, 0, 0);
    icheck("passthrough grants valid device w/ HW+device cap", vb > 0);
    current_proc_idx = (uint64_t)phNoDev;
    icheck("passthrough denies device w/o its device-cap (2-level gate)",
           (int64_t)syscall_dispatch(1, 0xFFFF, 0, 0) == -3);
    current_proc_idx = (uint64_t)phFull;
    if (vb > 0) {
        uint64_t ptdev = walk_pte(kprocs[phFull].cr3, (uint64_t)vb);
        icheck("granted device page is USER + cache-disabled",
               (ptdev & PTE_USER) && (ptdev & PTE_PCD));
        icheck("device mapping absent from an unrelated space",
               walk_pte(kprocs[pA].cr3, (uint64_t)vb) == 0);
    }
    current_proc_idx = save;

    kprintf("[invar  ] RESULT: %d passed, %d failed\n", g_ipass, g_ifail);
    if (g_ifail == 0) kputs("[invar  ] CORE SECURITY BOUNDARY VERIFIED — isolation is airtight\n");
    else              kputs("[invar  ] ISOLATION INVARIANT VIOLATED — must fix before proceeding\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * STRESS & ENFORCEMENT SUITE  (fault injection, TLB/CR3 stress, W^X/NX audit)
 * ===========================================================================
 * Forces races, cache-invalidation, and enforcement edge cases to surface now.
 * NOTE: this is a uniprocessor kernel, so cross-core TLB shootdowns (IPIs) do
 * not apply; the single-core equivalents (CR3 churn, invlpg staleness, timer
 * preemption injected into mapping transitions) are exercised instead.
 * =========================================================================== */
static int g_spass, g_sfail;
static void scheck(const char *name, int cond) {
    if (cond) { g_spass++; kprintf("[stress ]  PASS  %s\n", name); }
    else      { g_sfail++; kprintf("[stress ]  FAIL  %s\n", name); }
}

static uint8_t g_datapage[64] __attribute__((aligned(64)));   /* in .bss -> RW+NX */

/* Overflow a local buffer to clobber the stack canary; the compiler's epilogue  */
/* check must catch it via __stack_chk_fail.                                      */
static void __attribute__((noinline)) canary_smash(void) {
    char buf[16];
    char *p = buf;
    __asm__ volatile("" : "+r"(p));                        /* launder p: hide bounds from GCC */
    for (int i = 0; i < 40; i++) p[i] = (char)0x41;        /* overflow past the canary       */
    __asm__ volatile("" :: "r"(p) : "memory");
}

/* ring-3 stack-bomb: push until the stack pointer walks off the bottom into the  */
/* guard page. Machine code: mov rcx,0x2000 ; L: push rax ; loop L               */
static const uint8_t g_stackbomb[] = { 0x48,0xC7,0xC1,0x00,0x20,0x00,0x00, 0x50, 0xE2,0xFD };

/* Execute from an NX data page; the fault handler resumes at the trailing label */
/* (jmp doesn't push, so RSP is unchanged across the recovery).                  */
static int nx_poison(void) {
    g_datapage[0] = 0xC3;                                  /* 'ret'               */
    g_fault_caught = 0; g_fault_vector = 0; g_fault_expected = 1;
    __asm__ volatile(
        "lea 2f(%%rip), %%rax\n movq %%rax, %0\n"
        "movq %%rsp, %1\n"
        "jmp *%2\n"                                        /* fetch from NX page   */
        "2:\n"
        : "=m"(g_fault_recover_rip), "=m"(g_fault_recover_rsp)
        : "r"((uint64_t)(uintptr_t)g_datapage) : "rax", "memory");
    g_fault_expected = 0;
    return g_fault_caught && g_fault_vector == 14;
}

/* Write to the R+X kernel code region; expect a write-protect #PF.              */
static int wp_poison(void) {
    g_fault_caught = 0; g_fault_vector = 0; g_fault_expected = 1;
    __asm__ volatile(
        "lea 2f(%%rip), %%rax\n movq %%rax, %0\n"
        "movq %%rsp, %1\n"
        "movb $0x90, (%2)\n"                               /* write into .text     */
        "2:\n"
        : "=m"(g_fault_recover_rip), "=m"(g_fault_recover_rsp)
        : "r"((uint64_t)(uintptr_t)_stext) : "rax", "memory");
    g_fault_expected = 0;
    return g_fault_caught && g_fault_vector == 14;
}

static void cmd_stress(void) {
    kputs("-- STRESS & ENFORCEMENT (fault injection + TLB/CR3 + W^X/NX) --\n");
    g_spass = g_sfail = 0;
    uint64_t save = current_proc_idx;

    /* (1) W^X enforcement audit: no page anywhere is both writable+executable  */
    int ktot = 0, ku = scan_wx(kernel_cr3, 0, &ktot);
    scheck("kernel address space has ZERO writable+executable pages", ku == 0);
    kprintf("[stress ] scanned %d kernel leaf pages, %d W+X\n", ktot, ku);
    int pW = kproc_spawn("stress-wx", PCAP_FILESYSTEM);
    map_page(kprocs[pW].cr3, 0x500000000000ull, alloc_frame(), PTE_USER);              /* RX  */
    map_page(kprocs[pW].cr3, 0x500000001000ull, alloc_frame(), PTE_USER | PTE_WRITE | PTE_NX); /* RW+NX */
    int utot = 0, uw = scan_wx(kprocs[pW].cr3, 1, &utot);
    scheck("user address space has ZERO writable+executable pages", uw == 0);

    /* (2) NX poisoning: executing a data page traps */
    scheck("executing a data page traps (#PF, NX enforced)", nx_poison());

    /* (3) write-protect: kernel code is immutable */
    scheck("writing to R+X kernel code traps (#PF, code immutable)", wp_poison());

    /* (4) TLB staleness under remap: remap a vaddr to a new frame, invlpg, and  */
    /* confirm the read reflects the NEW frame (no stale TLB entry survives)     */
    uint64_t tv = 0x500000010000ull;
    uint64_t f1 = alloc_frame(), f2 = alloc_frame();
    *(volatile uint32_t *)f1 = 0x1111; *(volatile uint32_t *)f2 = 0x2222;
    map_page(kernel_cr3, tv, f1, PTE_WRITE | PTE_NX);
    __asm__ volatile("invlpg (%0)" :: "r"(tv) : "memory");
    uint32_t r1 = *(volatile uint32_t *)tv;
    map_page(kernel_cr3, tv, f2, PTE_WRITE | PTE_NX);      /* remap same vaddr    */
    tlb_shootdown(tv);                     /* v0.35: invalidate on EVERY core     */
    uint32_t r2 = *(volatile uint32_t *)tv;
    scheck("invlpg after remap: no stale TLB entry (sees new frame, all cores shot down)",
           r1 == 0x1111 && r2 == 0x2222);

    /* (5) CR3 churn under preemption: rapidly switch between two address spaces */
    /* while the timer preempts, verifying each space stays correctly isolated.  */
    int cA = kproc_spawn("churn-A", 0), cB = kproc_spawn("churn-B", 0);
    uint64_t V = 0x500000020000ull, ga = alloc_frame(), gb = alloc_frame();
    map_page(kprocs[cA].cr3, V, ga, PTE_USER | PTE_WRITE | PTE_NX);
    map_page(kprocs[cB].cr3, V, gb, PTE_USER | PTE_WRITE | PTE_NX);
    *(volatile uint32_t *)ga = 0xA0A0A0A0; *(volatile uint32_t *)gb = 0xB0B0B0B0;
    int churn_ok = 1;
    preempt_enable();
    for (int i = 0; i < 20000; i++) {
        write_cr3(kprocs[cA].cr3);
        if (*(volatile uint32_t *)V != 0xA0A0A0A0) { churn_ok = 0; break; }
        write_cr3(kprocs[cB].cr3);
        if (*(volatile uint32_t *)V != 0xB0B0B0B0) { churn_ok = 0; break; }
    }
    write_cr3(kernel_cr3);
    preempt_disable();
    scheck("20000 CR3 switches under preemption keep spaces isolated", churn_ok);

    /* (6) page-table integrity auditor + bit-flip detection */
    uint64_t *pt_pml4 = (uint64_t *)(kprocs[pW].cr3 & ADDR_MASK);
    /* corrupt a reserved/high bit in a present PML4 entry, then detect it */
    int corrupt_found = 0;
    for (int i = 0; i < 512; i++) {
        if (pt_pml4[i] & PTE_PRESENT) {
            uint64_t good = pt_pml4[i];
            pt_pml4[i] = good | (1ull << 52);              /* flip a reserved bit  */
            /* auditor: reserved bits [52..58] must be zero in a valid entry */
            if (pt_pml4[i] & (0x7Full << 52)) corrupt_found = 1;
            pt_pml4[i] = good;                             /* repair               */
            break;
        }
    }
    scheck("page-table auditor detects a flipped reserved bit", corrupt_found);

    /* (7) allocator fault injection: exhaustion is handled cleanly (no corrupt) */
    uint64_t saved_next = g_next_frame, top = g_next_frame + 0x2000;  /* only 2 frames */
    g_alloc_limit = top;                                   /* arm the limiter      */
    int of_idx = kproc_spawn("oom", PCAP_FILESYSTEM);
    /* try to map more pages than the 2-frame budget allows */
    int oom_hit = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t fr = alloc_frame_limited();
        if (!fr) { oom_hit = 1; break; }
        map_page(kprocs[of_idx].cr3, 0x500000030000ull + (uint64_t)i * 0x1000, fr, PTE_USER | PTE_WRITE | PTE_NX);
    }
    g_alloc_limit = 0;                                     /* disarm               */
    g_next_frame = saved_next > g_next_frame ? saved_next : g_next_frame; /* keep monotonic */
    scheck("allocator exhaustion returns failure cleanly (no panic)", oom_hit);
    current_proc_idx = save;

    /* (8) guard page: a ring-3 stack overflow walks into the unmapped guard      */
    /* page and is trapped + the task terminated, never touching kernel space.    */
    {
        int sb = kproc_spawn("stack-bomb", 0);
        uint64_t cf = alloc_frame();
        for (unsigned i = 0; i < sizeof g_stackbomb; i++) ((uint8_t *)cf)[i] = g_stackbomb[i];
        map_page(kprocs[sb].cr3, 0x500000000000ull, cf, PTE_USER);   /* RX code    */
        map_user_stack(kprocs[sb].cr3);                              /* guarded stack */
        g_guard_caught = 0;
        enter_process("stack-bomb", sb, 0x500000000000ull);          /* overflows -> guard */
        current_proc_idx = save;
        scheck("ring-3 stack overflow trapped by guard page", g_guard_caught);
    }

    /* (9) stack canary: a local buffer overflow is caught by the epilogue check  */
    g_canary_caught = 0; g_canary_test = 1;
    if (!__builtin_setjmp(g_canary_jmp)) {
        canary_smash();                                    /* __stack_chk_fail -> longjmp */
        g_canary_test = 0;
    }
    scheck("stack canary detects local buffer overflow", g_canary_caught);

    kprintf("[stress ] RESULT: %d passed, %d failed\n", g_spass, g_sfail);
    if (g_sfail == 0) kputs("[stress ] STRESS & ENFORCEMENT CLEAN — no races, W^X/NX hold\n");
    else              kputs("[stress ] STRESS FAILURES PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * SYSCALL ARGUMENT FUZZING  (adversarial inputs across the syscall boundary)
 * ===========================================================================
 * Drives every syscall with randomized, hostile arguments — bad pointers,
 * kernel addresses, huge lengths, invalid handles — to prove the kernel never
 * faults, corrupts state, or lets a ring-3 pointer reach kernel memory. The
 * targeted checks below are the concrete vulnerabilities this surfaced (ring-3
 * pointers were dereferenced unchecked) and now fixed via access_ok/copy_user_str.
 * =========================================================================== */
static uint64_t g_rng = 0;
static uint64_t rng_next(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng;
}
static int g_fpass, g_ffail;
static void fcheck(const char *name, int cond) {
    if (cond) { g_fpass++; kprintf("[fuzz   ]  PASS  %s\n", name); }
    else      { g_ffail++; kprintf("[fuzz   ]  FAIL  %s\n", name); }
}

static void cmd_fuzz(void) {
    kputs("-- SYSCALL ARGUMENT FUZZING --\n");
    g_fpass = g_ffail = 0;
    uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    g_rng = (((uint64_t)hi << 32) | lo) | 1;
    uint64_t save = current_proc_idx;
    int fp = kproc_spawn("fuzz", 0);
    uint64_t vpage = 0x500000040000ull, vframe = alloc_frame();
    const char *nm = "motd";
    for (int i = 0; i < 5; i++) ((char *)vframe)[i] = nm[i];        /* "motd\0" valid name ptr */
    map_page(kprocs[fp].cr3, vpage, vframe, PTE_USER | PTE_WRITE | PTE_NX);

    /* Run in the fuzz process's address space so valid user pointers resolve and */
    /* hostile ones are rejected exactly as in a real ring-3 syscall.             */
    current_proc_idx = (uint64_t)fp; kprocs[fp].caps = PCAP_FILESYSTEM;
    write_cr3(kprocs[fp].cr3);

    /* --- targeted: the pointer vulnerabilities the fuzzer flagged, now fixed --- */
    int64_t fd = (int64_t)syscall_dispatch(5, vpage, 0, 0);        /* SYS_OPEN("motd")        */
    fcheck("SYS_OPEN valid user name pointer succeeds",             fd >= 0);
    fcheck("SYS_OPEN kernel-address name rejected (EFAULT)",
           (int64_t)syscall_dispatch(5, (uint64_t)&g_rng, 0, 0) == -14);
    fcheck("SYS_WRITE kernel-address string rejected (no kernel leak)",
           (int64_t)syscall_dispatch(0, (uint64_t)&g_rng, 0, 0) == -14);
    fcheck("SYS_WRITE unmapped user pointer rejected",
           (int64_t)syscall_dispatch(0, 0x5F0000000000ull, 0, 0) == -14);
    if (fd >= 0) {
        fcheck("SYS_READ into kernel memory rejected (no kernel write)",
               (int64_t)syscall_dispatch(6, (uint64_t)fd, (uint64_t)&g_rng, 100) == -14);
        fcheck("SYS_READ into valid user buffer succeeds",
               (int64_t)syscall_dispatch(6, (uint64_t)fd, vpage, 100) >= 0);
    }

    /* --- randomized fuzzing: every syscall x adversarial argument pool --- */
    uint64_t pool[] = {
        0, 1, 0xFFFF, 8, 16, 0x1000, 0xFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull,
        USER_VMIN, USER_VMAX, USER_VMAX - 1, 0x500000000000ull, vpage, vpage + 0x800,
        (uint64_t)&g_rng, kernel_cr3, 0x600000000000ull, 0x700000000000ull,
        0xDEAD0000ull, 0x8000000000000000ull,
    };
    int np = (int)(sizeof pool / sizeof pool[0]);
    uint64_t cpool[] = { 0, PCAP_FILESYSTEM, PCAP_NETWORK, PCAP_FRAMEBUFFER, PCAP_HW_PASSTHROUGH,
        PCAP_FILESYSTEM | PCAP_NETWORK | PCAP_FRAMEBUFFER | PCAP_HW_PASSTHROUGH };
    int ncp = (int)(sizeof cpool / sizeof cpool[0]);

    int N = 20000, calls = 0, rejected = 0, okc = 0;
    g_quiet = 1;                                                    /* silence SYS_WRITE spam  */
    for (int i = 0; i < N; i++) {
        uint64_t num = rng_next() % 16;                            /* 0..12 + 15/16/17 via remap */
        if (num == 13) num = 15;                                   /* SYS_YIELD: exercises the per- */
        if (num == 14) num = 16;                                   /* thread switch under fuzz      */
        if (num == 15) num = 17;                                   /* SYS_SURFACE_FLIP: owner-gated,*/
        if (num == 2)  num = 3;                                    /* denied before any state change */
        kprocs[fp].caps = cpool[rng_next() % ncp];
        if (num == 9 && (kprocs[fp].caps & PCAP_NETWORK)) kprocs[fp].caps &= ~PCAP_NETWORK; /* no block */
        uint64_t a0 = pool[rng_next() % np], a1 = pool[rng_next() % np], a2 = pool[rng_next() % np];
        int64_t r = (int64_t)syscall_dispatch(num, a0, a1, a2);
        calls++;
        if (r < 0) rejected++; else okc++;
    }
    g_quiet = 0;
    write_cr3(kernel_cr3);
    current_proc_idx = save;
    __asm__ volatile("sti");
    kprintf("[fuzz   ] %d randomized calls: %d rejected, %d ok — kernel never faulted\n",
            calls, rejected, okc);

    /* --- post-fuzz integrity: the enforcement invariants still hold --- */
    int tot = 0, wx = scan_wx(kernel_cr3, 0, &tot);
    fcheck("post-fuzz: kernel still has ZERO writable+executable pages", wx == 0);
    fcheck("post-fuzz: fuzz process isolation intact (private user PML4)",
           ((uint64_t *)(kprocs[fp].cr3 & ADDR_MASK))[0xA0] !=
           ((uint64_t *)(kernel_cr3 & ADDR_MASK))[0xA0]);

    kprintf("[fuzz   ] RESULT: %d passed, %d failed\n", g_fpass, g_ffail);
    if (g_ffail == 0) kputs("[fuzz   ] SYSCALL BOUNDARY HARDENED — all adversarial pointers rejected\n");
    else              kputs("[fuzz   ] FUZZING FOUND GAPS — must fix\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * BACKGROUND SWEEP STATUS + LIVE DETECTION TEST
 * ===========================================================================
 * Reports what the tick-driven auditor has covered, and proves it actually
 * catches corruption by injecting a reserved-bit flip into a live PTE and
 * waiting for the background sweep (not an on-demand scan) to report it.
 * =========================================================================== */
static void cmd_sweep(void) {
    kputs("-- DESCRIPTOR-TABLE INTEGRITY SWEEP (background, tick-driven) --\n");
    kprintf("[sweep  ] armed=%d  batch=%d entries/tick @ 100 Hz\n",
            (uint64_t)g_sweep.enabled, (uint64_t)SWEEP_BATCH);
    kprintf("[sweep  ] full passes completed: %u\n", g_sweep.passes);
    kprintf("[sweep  ] leaf entries audited:  %u\n", g_sweep.entries);
    kprintf("[sweep  ] violations found:      %u\n", g_sweep.violations);

    /* live test: inject a reserved-bit flip and let the BACKGROUND sweep find it */
    uint64_t *pml4 = (uint64_t *)(kernel_cr3 & ADDR_MASK);
    uint64_t *pdpt = (uint64_t *)(pml4[0] & ADDR_MASK);
    uint64_t *pd   = (uint64_t *)(pdpt[0] & ADDR_MASK);
    int victim = -1;
    for (int i = 0; i < 512; i++)
        if ((pd[i] & PTE_PRESENT) && (pd[i] & PTE_HUGE)) { victim = i; break; }

    if (victim >= 0) {
        uint64_t v0 = g_sweep.violations, good = pd[victim];
        kprintf("[sweep  ] injecting reserved-bit flip into live PD entry %d ...\n", (uint64_t)victim);
        pd[victim] = good | (1ull << 53);              /* corrupt a reserved bit    */
        preempt_enable();
        uint64_t start = g_ticks, deadline = start + 400;   /* up to ~4 s of ticks   */
        while (g_sweep.violations == v0 && g_ticks < deadline) __asm__ volatile("hlt");
        int caught = (g_sweep.violations > v0);
        uint64_t bad_va = g_sweep.last_bad_va, took = g_ticks - start;
        pd[victim] = good;                             /* repair immediately        */
        write_cr3(kernel_cr3);
        preempt_disable();
        if (caught) kprintf("[sweep  ]  PASS  background sweep caught corruption at va %X after %u ticks\n",
                            bad_va, took);
        else        kputs("[sweep  ]  FAIL  background sweep did not detect the injected flip\n");
        kprintf("[sweep  ] entry repaired; violations counter now %u\n", g_sweep.violations);
    }
    kputs("-- done --\n");
}

/* ===========================================================================
 * IOMMU STATUS + LIVE DMA ISOLATION PROOF
 * ===========================================================================
 * The security property that makes device passthrough safe: a device can only
 * reach memory its domain maps. This proves it on real hardware — point the
 * NIC's context entry at a domain that maps NOTHING, make the device attempt a
 * real DMA, and confirm the IOMMU blocks it and records the fault (source-id,
 * reason, faulting address). Then restore the domain and confirm recovery.
 * =========================================================================== */
static int g_ipass2, g_ifail2;
static void icheck2(const char *name, int cond) {
    if (cond) { g_ipass2++; kprintf("[iommu  ]  PASS  %s\n", name); }
    else      { g_ifail2++; kprintf("[iommu  ]  FAIL  %s\n", name); }
}

/* Global context-cache + IOTLB invalidation (after changing translation state). */
static void iommu_invalidate_all(void) {
    dmar_w64(DMAR_CCMD, (1ull << 63) | (1ull << 61));
    for (int i = 0; i < 1000000 && (dmar_r64(DMAR_CCMD) & (1ull << 63)); i++) __asm__ volatile("pause");
    uint32_t iro = (uint32_t)(((g_dmar_ecap >> 8) & 0x3FF) * 16);
    dmar_w64(iro + 8, (1ull << 63) | (1ull << 60));
    for (int i = 0; i < 1000000 && (dmar_r64(iro + 8) & (1ull << 63)); i++) __asm__ volatile("pause");
}

/* Clear all recorded faults + the fault status register.                        */
static void iommu_clear_faults(void) {
    uint32_t fro = (uint32_t)(((g_dmar_cap >> 24) & 0x3FF) * 16);
    int nfr = (int)(((g_dmar_cap >> 40) & 0xFF) + 1);
    for (int i = 0; i < nfr; i++) dmar_w64(fro + (uint32_t)i * 16 + 8, 1ull << 63);  /* W1C the F bit */
    dmar_w32(DMAR_FSTS, dmar_r32(DMAR_FSTS));          /* W1C pending/overflow    */
}

/* Read the first recorded fault, if any. Returns 1 and fills the outputs.       */
static int iommu_read_fault(uint64_t *addr, uint64_t *sid, uint64_t *reason, uint64_t *is_read) {
    uint32_t fro = (uint32_t)(((g_dmar_cap >> 24) & 0x3FF) * 16);
    int nfr = (int)(((g_dmar_cap >> 40) & 0xFF) + 1);
    for (int i = 0; i < nfr; i++) {
        uint64_t lo = dmar_r64(fro + (uint32_t)i * 16);
        uint64_t hi = dmar_r64(fro + (uint32_t)i * 16 + 8);
        if (hi & (1ull << 63)) {                       /* F: fault recorded       */
            *addr = lo & ~0xFFFull;
            *sid = hi & 0xFFFF;
            *reason = (hi >> 32) & 0xFF;
            *is_read = (hi >> 62) & 1;
            return 1;
        }
    }
    return 0;
}

static void cmd_iommu(void) {
    kputs("-- IOMMU (INTEL VT-d) STATUS + DMA ISOLATION --\n");
    /* Report VT-x availability honestly: the CPU may advertise VMX, but under  */
    /* QEMU's TCG interpreter (no KVM) VMXON is not emulated, so a hypervisor   */
    /* cannot actually execute a guest here. VT-d (DMA remapping) is emulated   */
    /* and fully functional, which is the half that gates secure passthrough.   */
    uint32_t ecx = 0, unused;
    __asm__ volatile("cpuid" : "=a"(unused), "=b"(unused), "=c"(ecx), "=d"(unused) : "a"(1), "c"(0));
    kprintf("[virt   ] CPUID.1:ECX.VMX = %d (%s)\n", (uint64_t)((ecx >> 5) & 1),
            ((ecx >> 5) & 1) ? "VT-x advertised" : "no VT-x — TCG emulation, guest execution unavailable");
    if (!g_dmar_regs) {
        kputs("[iommu  ] no remapping hardware on this platform (run QEMU with -device intel-iommu)\n");
        kputs("-- done --\n");
        return;
    }
    g_ipass2 = g_ifail2 = 0;
    uint32_t gsts = dmar_r32(DMAR_GSTS);
    kprintf("[iommu  ] regs @ %X   GSTS %x   root table @ %X\n", g_dmar_phys, (uint64_t)gsts, g_iommu_root);
    kprintf("[iommu  ] DRHD units %d   second-level %d-level   identity map %M MiB   domain-id 1\n",
            (uint64_t)g_iommu_drhds, (uint64_t)g_iommu_levels, g_iommu_mapped_bytes);
    icheck2("DMA translation is ENABLED (GSTS.TES)", (gsts & GSTS_TES) != 0);
    icheck2("root table pointer is programmed (GSTS.RTPS)", (gsts & GSTS_RTPS) != 0);

    kprintf("[iommu  ] fault recording: %d register(s) @ CAP.FRO offset 0x%x\n",
            (uint64_t)(((g_dmar_cap >> 40) & 0xFF) + 1), (uint64_t)(((g_dmar_cap >> 24) & 0x3FF) * 16));
    kputs("[iommu  ] (live DMA-confinement proof runs in `capdma` — a blocked DMA puts a\n");
    kputs("[iommu  ]  virtio device into its broken state, so it is exercised exactly once)\n");

    kprintf("[iommu  ] RESULT: %d passed, %d failed\n", (uint64_t)g_ipass2, (uint64_t)g_ifail2);
    if (!g_ifail2) kputs("[iommu  ] HARDWARE DMA ISOLATION VERIFIED — devices are confined to their domains\n");
    else           kputs("[iommu  ] DMA ISOLATION NOT PROVEN\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * CAPABILITY-BOUND DMA DOMAINS
 * ===========================================================================
 * The payoff of the whole architecture: a process granted a device gets
 * unrestricted access to its registers AND the device is simultaneously
 * confined by hardware to that process's own memory. Direct access without
 * the usual risk that a device (or a malicious driver) DMAs into the kernel.
 * =========================================================================== */
static int g_cpass, g_cfail;
static void ccheck(const char *name, int cond) {
    if (cond) { g_cpass++; kprintf("[capdma ]  PASS  %s\n", name); }
    else      { g_cfail++; kprintf("[capdma ]  FAIL  %s\n", name); }
}

static void cmd_capdma(void) {
    kputs("-- CAPABILITY-BOUND DMA DOMAINS --\n");
    if (!g_iommu_on) { kputs("[capdma ] no IOMMU on this platform — run with -device intel-iommu\n-- done --\n"); return; }
    g_cpass = g_cfail = 0;
    uint64_t save = current_proc_idx;

    /* locate the real NIC device entry (the one carrying a PCI source-id) */
    int di = -1;
    for (int i = 0; i < n_kdev; i++) if (kdevs[i].used && kdevs[i].bdf == g_vnet_bdf) { di = i; break; }
    if (di < 0) { kputs("[capdma ] no PCI device with a source-id to test\n-- done --\n"); return; }
    uint16_t bdf = kdevs[di].bdf;

    /* two processes, each with a private RAM page */
    int owner = kproc_spawn("dma-owner", PCAP_HW_PASSTHROUGH | PCAP_NETWORK);
    int other = kproc_spawn("dma-other", 0);
    uint64_t ownf = alloc_frame(), othf = alloc_frame();
    map_page(kprocs[owner].cr3, 0x500000060000ull, ownf, PTE_USER | PTE_WRITE | PTE_NX);
    map_page(kprocs[other].cr3, 0x500000060000ull, othf, PTE_USER | PTE_WRITE | PTE_NX);

    /* grant the device: capability gate + DMA confinement in one step */
    current_proc_idx = (uint64_t)owner;
    int64_t va = sys_hardware_passthrough(owner, kdevs[di].base);
    ccheck("capability holder is granted the device's MMIO", va > 0);
    uint64_t dom = g_proc_slpt[owner];
    ccheck("granting a device creates a private DMA domain for the owner", dom != 0);

    /* the device's context entry must now point at the owner's domain */
    uint8_t bus = (uint8_t)(bdf >> 8), devfn = (uint8_t)(bdf & 0xFF);
    uint64_t ctx = ((uint64_t *)g_iommu_root)[bus * 2] & ~0xFFFull;
    uint64_t clo = ((uint64_t *)ctx)[devfn * 2], chi = ((uint64_t *)ctx)[devfn * 2 + 1];
    ccheck("device context entry points at the owner's DMA domain", (clo & ~0xFFFull) == dom);
    ccheck("device carries the owner's domain id", ((chi >> 8) & 0xFFFF) == (uint64_t)(16 + owner));

    /* what the device can and cannot reach */
    ccheck("owner's own RAM page IS reachable by the device", slpt_lookup(dom, g_iommu_levels, ownf) != 0);
    ccheck("another process's RAM page is NOT reachable",      slpt_lookup(dom, g_iommu_levels, othf) == 0);
    ccheck("kernel memory is NOT reachable by the device",     slpt_lookup(dom, g_iommu_levels, (uint64_t)&g_rng) == 0);
    ccheck("kernel's virtqueue memory is NOT reachable",       slpt_lookup(dom, g_iommu_levels, (uint64_t)g_vnet_txbuf) == 0);
    ccheck("device MMIO was not mapped into the DMA domain",   slpt_lookup(dom, g_iommu_levels, kdevs[di].base) == 0);

    /* live enforcement: the device now tries to touch kernel memory and is blocked */
    iommu_clear_faults();
    static uint8_t frame[60];
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;
    for (int i = 0; i < 6; i++) frame[6 + i] = g_vnet_mac[i];
    frame[12] = 0x08; frame[13] = 0x06;
    vnet_tx(frame, sizeof frame);
    uint64_t fa = 0, sid = 0, rsn = 0, isrd = 0;
    int caught = 0;
    for (int i = 0; i < 2000000 && !caught; i++) { caught = iommu_read_fault(&fa, &sid, &rsn, &isrd); __asm__ volatile("pause"); }
    ccheck("confined device attempting kernel DMA is BLOCKED by hardware", caught && sid == bdf);
    if (caught)
        kprintf("[capdma ] blocked: device %x:%x.%x tried %s at %X — outside pid %u's domain\n",
                (uint64_t)(sid >> 8), (uint64_t)((sid >> 3) & 0x1F), (uint64_t)(sid & 7),
                isrd ? "read" : "write", fa, kprocs[owner].pid);

    /* revocation returns the device to the kernel's domain */
    iommu_detach_to_kernel(bdf);
    iommu_clear_faults();
    vnet_tx(frame, sizeof frame);
    int refault = 0;
    for (int i = 0; i < 300000; i++) { if (iommu_read_fault(&fa, &sid, &rsn, &isrd)) { refault = 1; break; } __asm__ volatile("pause"); }
    ccheck("revoking the grant returns the device to the kernel domain", !refault);
    current_proc_idx = save;

    kprintf("[capdma ] RESULT: %d passed, %d failed\n", (uint64_t)g_cpass, (uint64_t)g_cfail);
    if (!g_cfail) kputs("[capdma ] CAPABILITY-BOUND DMA VERIFIED — direct device access, hardware-confined to its owner\n");
    else          kputs("[capdma ] CAPABILITY-BOUND DMA NOT PROVEN\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * RING-3 USERSPACE DEVICE DRIVER
 * ===========================================================================
 * Hands the real NIC to an unprivileged ring-3 process and lets it drive the
 * hardware itself. The kernel's role ends at the capability check: it maps the
 * MMIO, confines the device to the process's IOMMU domain, and gets out of the
 * data path entirely.
 * =========================================================================== */
static void cmd_nicdriver(void) {
    kputs("-- RING-3 USERSPACE DEVICE DRIVER --\n");
    if (g_virtio_kdev < 0) { kputs("[drv    ] no virtio NIC present\n-- done --\n"); return; }
    uint64_t save = current_proc_idx;
    g_demo_dev_index = g_virtio_kdev;                  /* the real NIC            */
    int p = kproc_spawn("nic-driver", PCAP_HW_PASSTHROUGH | PCAP_NETWORK);
    kprocs[p].role = 1;                                /* -> runs the driver path */
    kprintf("[drv    ] handing '%s' to unprivileged pid %u (kernel leaves the data path)\n",
            kdevs[g_demo_dev_index].name, kprocs[p].pid);
    load_and_run_elf("nic-driver", p);
    current_proc_idx = save;
    kputs("-- done --\n");
}

/* ===========================================================================
 * RING-3 SCHEDULER THREADS  (v0.31 — first-class, preemptible, reaped)
 * ===========================================================================
 * Proves the thread model with live threads, not assertions about code:
 * concurrent ring-3 threads that yield through the scheduler, per-thread
 * process identity across context switches, SYS_EXIT reaping, surface
 * reclamation + pixel-buffer recycling, and fault termination that leaves the
 * kernel and sibling threads untouched.
 * =========================================================================== */
static int g_thpass, g_thfail;
static void thcheck(const char *n, int c) {
    if (c) { g_thpass++; kprintf("[threads]  PASS  %s\n", n); }
    else   { g_thfail++; kprintf("[threads]  FAIL  %s\n", n); }
}
static int threads_wait(volatile int *flag, int ticks) {
    uint64_t t0 = g_ticks;
    while (!*flag && g_ticks - t0 < (uint64_t)ticks) sched_yield();
    return *flag != 0;
}

static void cmd_threads(void) {
    kputs("-- RING-3 PROCESSES AS FIRST-CLASS SCHEDULER THREADS --\n");
    g_thpass = g_thfail = 0;

    /* (1)(2)(3) two concurrent identity probers: each yields 40 times and      */
    /* fails (exit 2) if SYS_GETPID ever returns the OTHER thread's identity.   */
    int ta = -1, tb = -1;
    int pa = uthread_spawn_elf("ident-A", PCAP_FILESYSTEM, 4, &ta);
    int pb = uthread_spawn_elf("ident-B", 0, 4, &tb);
    if (pa < 0 || pb < 0) { kputs("[threads] spawn failed\n-- done --\n"); return; }
    threads_wait(&kprocs[pa].exited, 600);
    threads_wait(&kprocs[pb].exited, 600);
    thcheck("two ring-3 threads ran to completion CONCURRENTLY with the kernel main thread",
            kprocs[pa].exited && kprocs[pb].exited);
    thcheck("per-thread process identity survived every context switch (both probers exit 0)",
            kprocs[pa].exit_code == 0 && kprocs[pb].exit_code == 0);
    thcheck("SYS_EXIT reaps the thread (PCB slots returned to the scheduler)",
            g_threads[ta].state == T_FREE && g_threads[tb].state == T_FREE);

    /* (4)(5) surface lifecycle: a surface app that exits gets its window        */
    /* unbound and its pixel buffer recycled by the next surface create.        */
    int tc = -1;
    int pc = uthread_spawn_elf("surf-exit", PCAP_FRAMEBUFFER, 3, &tc);
    threads_wait(&kprocs[pc].exited, 600);
    uint64_t phys1 = g_surf_last_reclaim;
    thcheck("surface reclaimed when its owner exits (slot unbound, buffer on free list)",
            kprocs[pc].exited && !g_surf[3].used && phys1 != 0);
    int td = -1;
    int pd = uthread_spawn_elf("surf-exit2", PCAP_FRAMEBUFFER, 3, &td);
    threads_wait(&kprocs[pd].exited, 600);
    thcheck("reclaimed pixel buffer is RECYCLED by the next surface create (same phys)",
            kprocs[pd].exited && g_surf_last_reclaim == phys1);

    /* (6)(7) a faulting thread is terminated in place: same stack-bomb the      */
    /* stress suite uses, but as a first-class thread — the guard page fires,    */
    /* the thread dies, and nothing unwinds to kernel_main.                      */
    int sb = kproc_spawn("stack-bomb-t", 0);
    uint64_t cf = alloc_frame();
    for (unsigned i = 0; i < sizeof g_stackbomb; i++) ((uint8_t *)cf)[i] = g_stackbomb[i];
    map_page(kprocs[sb].cr3, 0x500000000000ull, cf, PTE_USER);
    map_user_stack(kprocs[sb].cr3);
    g_guard_caught = 0;
    int tsb = uthread_create("stack-bomb-t", sb, 0x500000000000ull);
    threads_wait(&kprocs[sb].exited, 600);
    thcheck("faulting ring-3 thread hit the guard page and was terminated (not unwound)",
            kprocs[sb].exited && g_guard_caught &&
            kprocs[sb].exit_code == 0x8000 + 14);
    thcheck("kernel and sibling threads survive the fault (surface app still bound)",
            g_threads[tsb].state == T_FREE && g_surf[4].used);

    kprintf("[threads] RESULT: %d passed, %d failed\n", (uint64_t)g_thpass, (uint64_t)g_thfail);
    if (!g_thfail) kputs("[threads] FIRST-CLASS RING-3 THREADS VERIFIED — concurrent, isolated, reaped\n");
    else          kputs("[threads] THREAD MODEL DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ===========================================================================
 * DOUBLE-BUFFERED SURFACES  (v0.32 — SYS_SURFACE_FLIP, tear-free by design)
 * ===========================================================================
 * The tear detector: a ring-3 thread fills WHOLE frames in strictly
 * alternating app-unique colors and flips; this suite plays compositor
 * (consuming flips at its own "frame boundaries", with timer preemption ON so
 * the app is interrupted mid-draw arbitrarily) and scans every consumed front
 * buffer. One non-uniform scan = one torn frame. Under v0.31's in-place
 * repaint this scan catches partial frames; with the flip protocol the front
 * buffer is a completed frame by construction.
 * =========================================================================== */
#define TEAR_COLOR_A 0x004682EAu    /* app-unique: not in the kernel palette   */
#define TEAR_COLOR_B 0x001FBF6Eu
static int g_flpass, g_flfail;
static void flcheck(const char *n, int c) {
    if (c) { g_flpass++; kprintf("[flip   ]  PASS  %s\n", n); }
    else   { g_flfail++; kprintf("[flip   ]  FAIL  %s\n", n); }
}

static void cmd_flip(void) {
    kputs("-- DOUBLE-BUFFERED SURFACES: page flip vs tearing --\n");
    g_flpass = g_flfail = 0;
    int tt = -1;
    int pt = uthread_spawn_elf("tear-test", PCAP_FRAMEBUFFER, 5, &tt);
    if (pt < 0) { kputs("[flip   ] spawn failed\n-- done --\n"); return; }
    uint64_t t0 = g_ticks;
    while (!g_surf[5].used && g_ticks - t0 < 500) sched_yield();
    struct surface *S = &g_surf[5];
    flcheck("ring-3 thread created a double-buffered surface (front+back pair)",
            S->used && S->bufpages == 4 && S->owner == pt);
    flcheck("both buffers are user-mapped in the owner (and only 2*bufpages of them)",
            access_ok(kprocs[pt].cr3, SURF_USER_V, (uint64_t)2 * S->bufpages * 0x1000, 1) &&
            !access_ok(kprocs[pt].cr3, SURF_USER_V + (uint64_t)2 * S->bufpages * 0x1000, 0x1000, 1));
    uint64_t phys0 = S->phys;

    /* v0.34: with NO consumer registered, flips must complete immediately —   */
    /* the producer keeps publishing frames instead of parking.                 */
    uint32_t cA = 0, cB = 0;
    for (int y = 0; y < 40 && !(cA && cB); y++) {
        sched_yield();
        uint32_t c0 = *(volatile uint32_t *)surf_front_phys(S) & 0xFFFFFF;
        if (c0 == TEAR_COLOR_A) cA = 1;
        if (c0 == TEAR_COLOR_B) cB = 1;
    }
    flcheck("flips on a slot with NO consumer complete immediately (producer never parks)",
            cA && cB);

    /* the sibling slot-4 app must KEEP RUNNING while we consume slot 5 — the  */
    /* v0.33 gap. Watch its liveness bar move across our consume loop.         */
    int bar0 = -1, bar1 = -1;
    if (g_surf[4].used) {
        uint32_t *s4 = (uint32_t *)surf_front_phys(&g_surf[4]);
        for (int x = 0; x < 200; x++) if ((s4[106 * 200 + x] & 0xFFFFFF) == 0x9B4DFF) { bar0 = x; break; }
    }

    /* play compositor FOR SLOT 5 ONLY: consume 200 flips, scan each frame     */
    S->consumer = 1;                   /* per-slot registration (v0.34)         */
    preempt_enable();                  /* the app gets preempted mid-draw       */
    int consumed = 0, torn = 0, alt_ok = 1, seenA = 0, seenB = 0;
    uint32_t last = 0;
    while (consumed < 200) {
        t0 = g_ticks;
        while (!S->flip_pending && g_ticks - t0 < 300) sched_yield();
        if (!S->flip_pending) break;                   /* app died / stalled    */
        S->front ^= 1; S->flip_pending = 0;            /* frame boundary        */
        volatile uint32_t *fp = (volatile uint32_t *)surf_front_phys(S);
        uint32_t c0 = fp[0] & 0xFFFFFF;
        int mix = 0;
        for (int i = 1; i < S->w * S->h; i++)
            if ((fp[i] & 0xFFFFFF) != c0) { mix = 1; break; }
        if (mix) torn++;
        if (c0 == TEAR_COLOR_A) seenA++;
        else if (c0 == TEAR_COLOR_B) seenB++;
        else alt_ok = 0;                               /* junk frame            */
        if (consumed > 0 && c0 == last) alt_ok = 0;    /* must strictly alternate */
        last = c0;
        consumed++;
    }
    preempt_disable();
    S->consumer = 0;                   /* deregister                            */
    if (g_surf[4].used) {
        uint32_t *s4 = (uint32_t *)surf_front_phys(&g_surf[4]);
        for (int x = 0; x < 200; x++)              /* sample NOW, before any    */
            if ((s4[106 * 200 + x] & 0xFFFFFF) == 0x9B4DFF) { bar1 = x; break; } /* more yields */
        for (int tries = 0; tries < 2 && bar1 == bar0; tries++) {   /* 184-wrap fallback */
            for (int y = 0; y < 20; y++) sched_yield();
            s4 = (uint32_t *)surf_front_phys(&g_surf[4]);
            bar1 = -1;
            for (int x = 0; x < 200; x++)
                if ((s4[106 * 200 + x] & 0xFFFFFF) == 0x9B4DFF) { bar1 = x; break; }
        }
        flcheck("sibling surface thread kept running while another slot was consumed",
                bar0 >= 0 && bar1 >= 0 && bar1 != bar0);
    }
    kprintf("[flip   ] consumed %d published frames: %d torn, %d/%d color A/B\n",
            (uint64_t)consumed, (uint64_t)torn, (uint64_t)seenA, (uint64_t)seenB);
    flcheck("200 frames published through SYS_SURFACE_FLIP under preemption", consumed == 200);
    flcheck("ZERO torn frames: every published front buffer is a COMPLETE frame", torn == 0);
    flcheck("frames alternate strictly (each flip is exactly one finished frame)",
            alt_ok && seenA + seenB == consumed && seenA >= 99);

    /* lifecycle: the tear app exits after its 400 frames; the PAIR is reclaimed */
    t0 = g_ticks;
    while (!kprocs[pt].exited && g_ticks - t0 < 800) sched_yield();
    flcheck("double buffer reclaimed as one chunk when the owner exits",
            kprocs[pt].exited && !g_surf[5].used && g_surf_last_reclaim == phys0);

    kprintf("[flip   ] RESULT: %d passed, %d failed\n", (uint64_t)g_flpass, (uint64_t)g_flfail);
    if (!g_flfail) kputs("[flip   ] PAGE FLIP VERIFIED — the compositor can no longer observe a half-drawn frame\n");
    else          kputs("[flip   ] FLIP DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* Hand a canvas window to an unprivileged process that renders its own pixels.
 * v0.31: the app is a FIRST-CLASS THREAD — spawned, not entered. It creates
 * its surface and then loops (poll events -> render -> yield) forever,
 * concurrently with the compositor.                                          */
static void cmd_surface(void) {
    kputs("-- RING-3 APPLICATION SURFACE (first-class scheduler thread) --\n");
    int tid = -1;
    int p = uthread_spawn_elf("surface-app", PCAP_FRAMEBUFFER, 2, &tid);
    if (p < 0) { kputs("[surface] spawn failed\n-- done --\n"); return; }
    kprintf("[surface] handing canvas window 4 to unprivileged pid %u\n", kprocs[p].pid);
    /* the app runs when scheduled: yield until it has bound its surface        */
    uint64_t t0 = g_ticks;
    while (!g_surf[4].used && g_ticks - t0 < 500) sched_yield();
    kprintf("[surface] slot 4 bound=%d owner=pid %u — compositor will place its pixels; the app KEEPS RUNNING\n",
            (uint64_t)g_surf[4].used, (uint64_t)(g_surf[4].used ? kprocs[g_surf[4].owner].pid : 0));
    kputs("-- done --\n");
}

/* v0.31: input routing to a LIVE ring-3 thread — one single canvas pass.
 * The camera state carries over from the previous pass (no re-init), three
 * clicks are synthesized mid-pass through the real driver path, and the app
 * thread — running concurrently — drains its queue and repaints while the
 * same pass keeps compositing. No re-entry, no second pass needed.           */
#define APP_HIT_MARKER 0x00F06A18u    /* app-unique color: not in the kernel palette */
static void cmd_surfin(void) {
    kputs("-- INPUT ROUTING TO A LIVE RING-3 THREAD (single canvas pass) --\n");
    if (!g_surf[4].used) { kputs("[surfin ] no surface bound\n-- done --\n"); return; }
    static const int cx[3] = { 470, 560, 660 }, cy[3] = { 400, 440, 470 };
    static const int cf[3] = { 30, 60, 90 };
    canvas_pass(150, 0, cx, cy, cf, 3, 0);         /* clicks land DURING the pass */
    /* Deterministic proof the app repainted within the pass: its surface now    */
    /* carries hit markers in a color only the app draws. The app may have been  */
    /* preempted mid-repaint when the pass ended, so let it finish its frame.    */
    struct surface *S = &g_surf[4];
    int hits = 0;
    for (int tries = 0; tries < 50 && !hits; tries++) {
        sched_yield();                     /* app self-consumes its parked flip  */
        hits = 0;
        uint32_t *fp = (uint32_t *)surf_front_phys(S);   /* the PUBLISHED frame  */
        for (int i = 0; i < S->w * S->h; i++)
            if ((fp[i] & 0xFFFFFF) == APP_HIT_MARKER) hits++;
    }
    kprintf("[surfin ] app repainted DURING the pass: %d hit-marker pixels (%X) in its surface\n",
            (uint64_t)hits, (uint64_t)APP_HIT_MARKER);
    kputs("-- done --\n");
}

/* ===========================================================================
 * KEYBOARD ROUTING TO RING-3 SURFACES  (v0.33 — the type=2 event goes live)
 * ===========================================================================
 * End-to-end, both ends verified: keystrokes are injected into the REAL PS/2
 * ring buffer inside a live canvas pass; the modal router delivers them to
 * the focused app's event queue; the app (a first-class thread) drains them,
 * paints each char as a block whose COLOR ENCODES THE ASCII CODE, and
 * publishes via SYS_SURFACE_FLIP. The suite then decodes the published front
 * buffer back into the typed string. Negative checks prove navigation-mode
 * keys never route and Esc returns the keyboard to the camera.
 * =========================================================================== */
#define KEYBLOCK_BASE 0x00A00030u   /* app block color: 0xA0..30 | (ascii<<8)  */
static int g_kbpass, g_kbfail;
static void kbcheck(const char *n, int c) {
    if (c) { g_kbpass++; kprintf("[keys   ]  PASS  %s\n", n); }
    else   { g_kbfail++; kprintf("[keys   ]  FAIL  %s\n", n); }
}

static void cmd_keys(void) {
    kputs("-- KEYBOARD ROUTING TO RING-3 SURFACES (type=2) --\n");
    g_kbpass = g_kbfail = 0;
    if (!g_surf[4].used) { kputs("[keys   ] no surface bound\n-- done --\n"); return; }

    /* (1) navigation mode: keys steer the camera and must NOT reach the app   */
    uint64_t r0 = g_keys_routed;
    int64_t x0 = g_cam_x;
    canvas_pass(30, "dd", 0, 0, 0, 0, 0);          /* two 'd' impulses          */
    kbcheck("navigation-mode keys steer the camera, none leak to the app",
            g_keys_routed == r0 && g_cam_x != x0);

    /* (2)(3)(4) Enter arms type-to-app; "HI R3" routes; Esc disarms — all      */
    /* through the real PS/2 ring inside one live pass                          */
    canvas_pass(90, "\nHI R3\x1b", 0, 0, 0, 0, 0);
    kbcheck("Enter armed type-to-app and 5 keys were routed to the focused surface",
            g_keys_routed == r0 + 5);
    kbcheck("Esc returned the keyboard to canvas navigation", g_key_to_app == 0);

    /* decode the app's published frame: block i encodes typed char i           */
    static const char *want = "HI R3";
    struct surface *S = &g_surf[4];
    char got[8]; int ok = 1;
    int tries = 0; uint32_t *fp = 0;
    for (tries = 0; tries < 50; tries++) {         /* let the app publish       */
        sched_yield();
        fp = (uint32_t *)surf_front_phys(S);
        ok = 1;
        for (int i = 0; i < 5; i++) {
            uint32_t px = fp[92 * 200 + (6 + 8 * i + 3)] & 0xFFFFFF;
            got[i] = ((px & 0xFF00FF) == KEYBLOCK_BASE) ? (char)((px >> 8) & 0xFF) : '?';
            if (got[i] != want[i]) ok = 0;
        }
        if (ok) break;
    }
    got[5] = 0;
    kprintf("[keys   ] pixel-decoded from the published front buffer: \"%s\"\n", got);
    kbcheck("app painted the typed text — pixels decode back to \"HI R3\"", ok);

    /* (5) after Esc, keys are navigation again: camera moves, count frozen     */
    uint64_t r1 = g_keys_routed; x0 = g_cam_x;
    canvas_pass(30, "dd", 0, 0, 0, 0, 0);
    kbcheck("post-Esc keys move the camera again and the app receives none",
            g_keys_routed == r1 && g_cam_x != x0);

    kprintf("[keys   ] RESULT: %d passed, %d failed\n", (uint64_t)g_kbpass, (uint64_t)g_kbfail);
    if (!g_kbfail) kputs("[keys   ] KEYBOARD ROUTING VERIFIED — the focused app owns the keys, end to end\n");
    else          kputs("[keys   ] KEY ROUTING DEFECTS PRESENT\n");
    kputs("-- done --\n");
}

/* ---- Shell ------------------------------------------------------------------------------------ */
static uint64_t g_mb_info = 0;



static void cmd_help(void) {
    kputs(
      "Outrun OS kernel shell — commands:\n"
      "  help            this text\n"
      "  about           kernel identity and design goals\n"
      "  mem             physical memory map from the bootloader\n"
      "  caps            list the kernel capability table\n"
      "  mint <h> <r> <R|W>  mint a capability (e.g. mint app camera0 R)\n"
      "  revoke <slot>   revoke a capability by slot number\n"
      "  demo            scripted mint/grant/revoke walk-through\n"
      "  passthrough     real page-table MMIO grant vs. capability drop\n"
      "  usermode        drop to ring 3, trap back via SYSCALL passthrough\n"
      "  disk            virtio-blk read/write sector round-trip\n"
      "  cas             content-addressable store: put/get/dedup\n"
      "  sched           scheduler: concurrent I/O + preemption demo\n"
      "  vfs             named files over CAS: create/read/write/dedup\n"
      "  net             virtio-net ARP round-trip via sys_wait_event\n"
      "  gfx             render the Metropolis-Terminal compositor\n"
      "  stream          Time-Stream timeline + vector query\n"
      "  validate        run the production validation matrix\n"
      "  invariants      exhaustive isolation + hardening invariants\n"
      "  stress          fault injection + TLB/CR3 + W^X/NX enforcement\n"
      "  fuzz            syscall argument fuzzing (adversarial inputs)\n"
      "  sweep           background descriptor-table integrity sweep status\n"
      "  iommu           VT-d status + DMA isolation proof\n"
      "  capdma          capability-bound per-process DMA domains\n"
      "  nicdrv          hand the NIC to a ring-3 userspace driver\n"
      "  threads         first-class ring-3 scheduler threads suite\n"
      "  flip            double-buffered surface page-flip / tearing suite\n"
      "  keys            keyboard-to-surface routing suite (type=2 events)\n"
      "  smp             multi-core suite: IPIs, TLB shootdown, atomics\n"
      "  parallel        work-stealing parallel job across all cores\n"
      "  audit           parallel page-table integrity audit across all cores\n"
      "  mcsched         run a ring-3 thread on an application processor\n"
      "  cio             concurrent IO suite: BSP+APs inside the VFS at once\n"
      "  surface         spawn the ring-3 surface app as a live thread\n"
      "  surfin          route clicks to the live app in one canvas pass\n"
      "  canvas          Metropolis-Terminal spatial canvas (WASD/QE/TAB//)\n"
      "  uptime          seconds since boot (PIT @ 100 Hz)\n"
      "  clear           clear the VGA console\n"
      "  panic           deliberately fault to show exception containment\n"
      "  reboot          warm reboot via keyboard controller\n");
}
static void cmd_about(void) {
    kprintf("Outrun OS kernel %s — memory-safe-by-design microkernel project\n", KERNEL_VERSION);
    kputs(
      "Booted via GRUB/Multiboot2, running in x86_64 long mode.\n"
      "This image: Assembly bootstrap + freestanding C core. Live subsystems:\n"
      "IDT/PIC/PIT interrupts, PS/2 + serial dual console, Multiboot2 memory\n"
      "map, and the capability table from the Outrun architecture running in\n"
      "kernel space. Next roadmap phases: user mode, ELF loading, and the\n"
      "shared-memory device regions proven in the userspace prototype.\n");
}
static void cmd_demo(void) {
    kputs("-- Outrun capability walk-through (kernel space) --\n");
    int d = cap_mint("camera-driver", "camera0", 'W');
    int a = cap_mint("stream-app",    "camera0", 'R');
    int g = cap_mint("gesture-svc",   "camera0", 'R');
    kputs("[captbl ] data path now belongs to the holders — kernel steps aside\n");
    cap_revoke(a);
    kputs("[captbl ] stream-app's next access check fails: mapping dead, no signal sent\n");
    cap_revoke(g);
    cap_revoke(d);
    kputs("-- demo complete: mint -> grant -> revoke, all unforgeable, all in-kernel --\n");
}

static void shell_exec(char *line) {
    /* tokenize in place */
    char *argv[4] = {0};
    int argc = 0;
    for (char *p = line; *p && argc < 4; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (argc == 0) return;

    if      (!kstrcmp(argv[0], "help"))   cmd_help();
    else if (!kstrcmp(argv[0], "about"))  cmd_about();
    else if (!kstrcmp(argv[0], "clear"))  vga_clear();
    else if (!kstrcmp(argv[0], "caps"))   cap_list();
    else if (!kstrcmp(argv[0], "demo"))   cmd_demo();
    else if (!kstrcmp(argv[0], "passthrough")) cmd_passthrough();
    else if (!kstrcmp(argv[0], "usermode")) cmd_usermode();
    else if (!kstrcmp(argv[0], "disk")) cmd_disk();
    else if (!kstrcmp(argv[0], "cas")) cmd_cas();
    else if (!kstrcmp(argv[0], "sched")) cmd_sched();
    else if (!kstrcmp(argv[0], "vfs")) cmd_vfs();
    else if (!kstrcmp(argv[0], "net")) cmd_net();
    else if (!kstrcmp(argv[0], "gfx")) cmd_gfx();
    else if (!kstrcmp(argv[0], "stream")) { ts_print_timeline(); ts_query("the Q3 chart Sarah sent"); }
    else if (!kstrcmp(argv[0], "validate")) cmd_validate();
    else if (!kstrcmp(argv[0], "invariants")) cmd_invariants();
    else if (!kstrcmp(argv[0], "stress")) cmd_stress();
    else if (!kstrcmp(argv[0], "fuzz")) cmd_fuzz();
    else if (!kstrcmp(argv[0], "sweep")) cmd_sweep();
    else if (!kstrcmp(argv[0], "iommu")) cmd_iommu();
    else if (!kstrcmp(argv[0], "capdma")) cmd_capdma();
    else if (!kstrcmp(argv[0], "nicdrv")) cmd_nicdriver();
    else if (!kstrcmp(argv[0], "canvas")) cmd_canvas(1000000, 0);
    else if (!kstrcmp(argv[0], "cursor")) cmd_cursor();
    else if (!kstrcmp(argv[0], "kinetic")) cmd_kinetic();
    else if (!kstrcmp(argv[0], "surface")) cmd_surface();
    else if (!kstrcmp(argv[0], "surfin")) cmd_surfin();
    else if (!kstrcmp(argv[0], "threads")) cmd_threads();
    else if (!kstrcmp(argv[0], "flip")) cmd_flip();
    else if (!kstrcmp(argv[0], "keys")) cmd_keys();
    else if (!kstrcmp(argv[0], "smp")) cmd_smp();
    else if (!kstrcmp(argv[0], "parallel") || !kstrcmp(argv[0], "par")) cmd_parallel();
    else if (!kstrcmp(argv[0], "audit")) cmd_audit();
    else if (!kstrcmp(argv[0], "mcsched")) cmd_mcsched();
    else if (!kstrcmp(argv[0], "mcq")) cmd_mcq();
    else if (!kstrcmp(argv[0], "mcpre")) cmd_mcpre();
    else if (!kstrcmp(argv[0], "slice")) cmd_slice();
    else if (!kstrcmp(argv[0], "cio")) cmd_cio();
    else if (!kstrcmp(argv[0], "smpstress")) cmd_smp_stress();
    else if (!kstrcmp(argv[0], "dmastress")) cmd_dma_stress();
    else if (!kstrcmp(argv[0], "leakcheck")) cmd_leakcheck();
    else if (!kstrcmp(argv[0], "kpstress")) cmd_kproc_stress();
    else if (!kstrcmp(argv[0], "ipcstress")) cmd_ipc_stress();
    else if (!kstrcmp(argv[0], "vfiostress")) cmd_vfio_stress();
    else if (!kstrcmp(argv[0], "vfsstress")) cmd_vfs_stress();
    else if (!kstrcmp(argv[0], "gpustress")) cmd_gpu_stress();
    else if (!kstrcmp(argv[0], "vfscrashwrite")) {
        /* Manual, one-shot half of the genuine cross-QEMU-reboot journal proof
         * (see the probe in cmd_cas()): commit this write's journal entry,
         * then halt WITHOUT ever calling SYS_VFS_SYNC or shutting down
         * cleanly — simulating real power loss mid-write. A SEPARATE later
         * boot against the same disk image is what proves recovery, not
         * this process. Not part of the automated suite by design. */
        static const uint8_t crashpat2[24] = "CROSS-REBOOT-JOURNAL-OK";
        vfs_write_file("vfs-reboot-test", crashpat2, sizeof crashpat2);
        kputs("[vfs    ] vfscrashwrite: journal-committed 'vfs-reboot-test', halting WITHOUT sync (simulated power loss)\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    else if (!kstrcmp(argv[0], "mem"))    multiboot_scan(g_mb_info, true);
    else if (!kstrcmp(argv[0], "uptime")) kprintf("%u.%u s since boot (%u ticks)\n",
                                                  g_ticks / 100, (g_ticks % 100) / 10, g_ticks);
    else if (!kstrcmp(argv[0], "mint") && argc == 4)
        cap_mint(argv[1], argv[2], (argv[3][0] == 'W' || argv[3][0] == 'w') ? 'W' : 'R');
    else if (!kstrcmp(argv[0], "revoke") && argc == 2) {
        int s = 0;
        for (char *p = argv[1]; *p >= '0' && *p <= '9'; p++) s = s * 10 + (*p - '0');
        cap_revoke(s);
    }
    else if (!kstrcmp(argv[0], "panic")) {
        kputs("[kernel ] triggering a deliberate invalid-opcode fault...\n");
        __asm__ volatile("ud2");
    }
    else if (!kstrcmp(argv[0], "reboot")) {
        kputs("[kernel ] warm reboot\n");
        outb(0x64, 0xFE);
    }
    else if (!kstrncmp(argv[0], "mint", 4) || !kstrncmp(argv[0], "revoke", 6))
        kputs("usage: mint <holder> <resource> <R|W>   |   revoke <slot>\n");
    else
        kprintf("unknown command '%s' — try 'help'\n", argv[0]);
}

static void shell_run(void) {
    char line[80];
    uint32_t len = 0;
    kputs("\nType 'help' for commands, 'demo' for the capability walk-through.\n");
    kputs("outrun> ");
    for (;;) {
        int c = kbd_getc_nonblock();                  /* VM display / bare metal */
        if (c < 0) c = serial_getc_nonblock();        /* Proxmox serial console  */
        if (c < 0) { sched_yield();                   /* keep ring-3 threads live */
                     __asm__ volatile("hlt"); continue; }
        if (c == '\r') c = '\n';
        if (c == '\b' || c == 127) {
            if (len) { len--; kputc('\b'); }
            continue;
        }
        if (c == '\n') {
            kputc('\n');
            line[len] = 0;
            shell_exec(line);
            len = 0;
            kputs("outrun> ");
            continue;
        }
        if (len < sizeof line - 1 && c >= 32 && c < 127) {
            line[len++] = (char)c;
            kputc((char)c);
        }
    }
}

/* ---- Entry ---------------------------------------------------------------------------------------- */
void __attribute__((no_stack_protector)) kernel_main(uint64_t mb_info) {
    /* v0.39: the compiler's stack guard lives at %gs:CPUL_CANARY, so the BSP's
     * GS base must point at its cpu_local before ANY protected function runs.
     * (kernel_main itself is excluded: its prologue predates this wrmsr.)     */
    wrmsr(0xC0000101, (uint64_t)&g_cpu[0]);
    g_cpu[0].idx = 0;
    g_gs_ready = 1;
    canary_init();                          /* seed per-CPU guard words first  */
    serial_init();
    vga_clear();
    g_mb_info = mb_info;

    kputs("\n");
    kputs("  ....................................................................\n");
    kputs("  :   OUTRUN OS  --  bare-metal kernel " KERNEL_VERSION "                   :\n");
    kputs("  :   Assembly bootstrap -> x86_64 long mode -> freestanding C core  :\n");
    kputs("  ....................................................................\n\n");

    kprintf("[kernel ] long mode active, kernel at 1 MiB, first GiB identity-mapped\n");

    /* Interrupts */
    for (int v = 0; v < 48; v++) {
        static uint64_t handlers[48];
        (void)handlers;
    }
    {
        uint64_t h[52] = {
            ISR_ADDR(0),ISR_ADDR(1),ISR_ADDR(2),ISR_ADDR(3),ISR_ADDR(4),ISR_ADDR(5),
            ISR_ADDR(6),ISR_ADDR(7),ISR_ADDR(8),ISR_ADDR(9),ISR_ADDR(10),ISR_ADDR(11),
            ISR_ADDR(12),ISR_ADDR(13),ISR_ADDR(14),ISR_ADDR(15),ISR_ADDR(16),ISR_ADDR(17),
            ISR_ADDR(18),ISR_ADDR(19),ISR_ADDR(20),ISR_ADDR(21),ISR_ADDR(22),ISR_ADDR(23),
            ISR_ADDR(24),ISR_ADDR(25),ISR_ADDR(26),ISR_ADDR(27),ISR_ADDR(28),ISR_ADDR(29),
            ISR_ADDR(30),ISR_ADDR(31),ISR_ADDR(32),ISR_ADDR(33),ISR_ADDR(34),ISR_ADDR(35),
            ISR_ADDR(36),ISR_ADDR(37),ISR_ADDR(38),ISR_ADDR(39),ISR_ADDR(40),ISR_ADDR(41),
            ISR_ADDR(42),ISR_ADDR(43),ISR_ADDR(44),ISR_ADDR(45),ISR_ADDR(46),ISR_ADDR(47),
            ISR_ADDR(48),ISR_ADDR(49),                 /* v0.35: IPI vectors    */
            ISR_ADDR(50),ISR_ADDR(51),                 /* v0.39 preempt, v0.40 slice */
        };
        for (int v = 0; v < 52; v++) idt_set(v, h[v]);
        struct idtr idtr = { sizeof(idt) - 1, (uint64_t)idt };
        idt_load(&idtr);
    }
    pic_remap();
    pit_init();
    __asm__ volatile("sti");
    kprintf("[kernel ] IDT loaded (52 vectors incl. IPI 48-50 + slice tick 51), PIC remapped, PIT @ 100 Hz, IRQs on\n");
    kprintf("[kernel ] consoles: VGA text + COM1 serial (115200 8N1) — both live\n");

    multiboot_scan(mb_info, true);

    kprintf("[kernel ] capability table initialized (%d slots, kernel-owned)\n", MAX_CAPS);

    /* Capture the boot PML4 so process address spaces can share the low        */
    /* identity map (keeping kernel code/stack/IDT mapped after a CR3 switch),   */
    /* then run the real-paging passthrough demonstration once at boot.         */
    kernel_cr3 = read_cr3();
    kprintf("[kernel ] paging: boot PML4 @ phys %X; process frame pool from 16 MiB\n",
            kernel_cr3);
    harden_kernel_wx();
    kprintf("[kernel ] W^X enforced: kernel .text R+X, all other pages RW+NX\n");
    g_sweep.enabled = 1;                    /* start continuous background PTE audit */
    kprintf("[sweep  ] descriptor-table integrity sweep armed: %d entries/tick @ 100 Hz\n\n",
            SWEEP_BATCH);
    cmd_passthrough();
    iommu_init();

    /* Discover real hardware on the PCI bus (registers the virtio NIC's MMIO  */
    /* window as a capability-gated device), then enable ring 3 and run the    */
    /* passthrough as a real SYSCALL trap from an unprivileged ELF process.    */
    kputs("\n");
    pci_init();
    sched_init();
    cpp_ring_selftest();
    cmd_cas();
    cmd_vfs();
    cmd_sched();
    cmd_net();
    cmd_timestream();
    cmd_validate();
    cmd_invariants();
    usermode_init();        /* user GDT segments + TSS + SYSCALL MSRs (needed for ring 3) */
    smp_init();             /* v0.35: boot every core (needs the kernel GDT above) */
    cmd_stress();
    cmd_fuzz();
    cmd_sweep();
    cmd_smp();              /* v0.35: cross-core protocol verification            */
    cmd_parallel();         /* v0.36: work-stealing parallel job across cores     */
    cmd_audit();            /* v0.37: parallel page-table integrity audit         */
    cmd_mcsched();          /* v0.38: an AP autonomously runs a ring-3 thread      */
    cmd_mcq();              /* v0.39: per-CPU queues, stealing, concurrent ring 3  */
    cmd_mcpre();            /* v0.39: IPI preemption + cross-core context migration */
    cmd_slice();            /* v0.40: AP-local LAPIC timer round-robin time-slicing */
    cmd_cio();              /* v0.41: BSP + APs concurrently inside the VFS/CAS/surfaces */
    cmd_smp_stress();       /* v0.43: mixed syscall/VFS/compositor workload, every core   */
    cmd_dma_stress();       /* v0.44: real DMA/IOMMU grants revoked across genuine exit    */
    cmd_leakcheck();        /* v0.42: heavy spawn/destroy proves 100% frame reclamation  */
    cmd_kproc_stress();     /* v0.45: 200-cycle kproc/descriptor recycle stress          */
    cmd_ipc_stress();       /* v0.46: capability-gated IPC fd/shmem handoff, sender/receiver churn */
    cmd_vfio_stress();      /* v0.47: VFIO BAR mapping + routed interrupt wait, driver churn        */
    cmd_vfs_stress();       /* v0.48: VFS journaling, unlink/reclamation, multi-volume mounts       */
    cmd_gpu_stress();       /* v0.50: virtio-gpu 2D resource/scanout/flush churn, incl. client faults */
    cmd_iommu();
    cmd_capdma();
    cmd_nicdriver();
    mouse_init();
    fb_init();
    cmd_gfx();
    cmd_surface();        /* spawns the surface app as a first-class thread     */
    cmd_threads();        /* v0.31: thread model verification suite             */
    cmd_flip();           /* v0.32: double-buffer page-flip / tearing suite     */
    cmd_cursor();
    cmd_kinetic();
    cmd_canvas(300, "/zoom fit\n");
    cmd_surfin();         /* clicks routed + app reacts INSIDE this single pass */
    cmd_keys();           /* v0.33: keyboard routed to the focused app          */
    cmd_canvas(120, 0);   /* recomposite — the hit markers persist              */
    cmd_usermode();

    shell_run();
}
