#ifndef OUTRUN_ABI_H
#define OUTRUN_ABI_H
/* Native desktop ABI. Existing syscall numbers and wire layouts are preserved.
 * RAX=number, RDI/RSI/RDX=args; RAX=result, negative signed results are errors.
 * This header owns the desktop extensions; legacy SDK declarations remain in
 * metal/kernel/kernel64.c until migrated without changing their ABI. */
#define SYS_WRITE 0
#define SYS_EXIT 2
#define SYS_OPEN 5
#define SYS_READ 6
#define SYS_WRITE_FILE 7
#define SYS_CLOSE 8
#define SYS_SURFACE 13
#define SYS_SURFACE_POLL 14
#define SYS_YIELD 15
#define SYS_GETPID 16
#define SYS_SURFACE_FLIP 17
#define SYS_IPC_SEND 18
#define SYS_IPC_RECV 19
#define SYS_VFS_SYNC 22
#define SYS_VFS_UNLINK 23
#define SYS_WIN_CREATE 40
#define SYS_WIN_DAMAGE 41
#define SYS_WIN_POLL 42
#define SYS_WIN_INFO 43
#define SYS_SYSINFO 44
#define SYS_READDIR 45
/* SYS_OPEN's flags word. These are the kernel's own bit values (VFS_O_* in
 * kernel64.c), NOT the POSIX numbers — writing 0x241 here because that is what
 * open(2) uses elsewhere would set O_CREAT and nothing else. O_TRUNC matters
 * on this kernel specifically: writes are POSITIONAL and a second write to an
 * existing file APPENDS, so re-authoring a file without O_TRUNC glues the new
 * content onto the end of the old. */
#define OUTRUN_O_CREAT  1
#define OUTRUN_O_TRUNC  2
#define OUTRUN_O_APPEND 4
#define SYS_RUN_CMD 46
#define SYS_KILL 50
#define SYS_LSEEK 100
#define SYS_FTRUNCATE 101
#define SYS_RENAME 102
#define SYS_NANOSLEEP 106
/* v0.95 VFIO. Config space is READ-ONLY and serves a CLAIMED device only:
 * there is no write counterpart, deliberately — see case 116 in kernel64.c. */
#define SYS_CLAIM_PCI_DEVICE 111
#define SYS_RELEASE_PCI_DEVICE 112
#define SYS_PCI_CFG_READ 116
#define SYS_DESKTOP_INFO 117
#define SYS_DESKTOP_SETTINGS 118
#define SYS_HW_INFO 119
#define SYS_FB_CAPTURE 120
#define EVENT_MOUSE_DOWN 1
#define EVENT_KEY_PRESS 2
#define OUTRUN_DESKTOP_ABI_VERSION 1
#define OUTRUN_WIN_BASE 0x0000550000000000ull
#define OUTRUN_WIN_STRIDE (((600ull * 440 * 4 + 4095) / 4096) * 4096)
/* WIN_CREATE a2=1 opts into paired buffers. WIN_DAMAGE returns the new back
 * buffer address after publication; a1 optionally supplies a NUL title.
 * Legacy a2=0 windows retain their old single-surface ABI. */
struct outrun_event { int type, x, y, code; };
/* Existing IPC wire layout, shared with kernel ipc_msg (104 bytes). */
struct outrun_ipc_msg {
    unsigned long long sender_pid, recipient_pid;
    unsigned int msg_type, cap_mask, payload_len;
    long long xfer_handle;
    unsigned char inline_data[64];
};
struct outrun_process {
    unsigned long long pid, cpu_ns;
    unsigned int flags, reserved;
    char name[24];
};
struct outrun_desktop_info {
    unsigned int version, size, ncpu, nproc;
    unsigned int width, height, scale, accent;
    unsigned int repeat_delay, repeat_period;
    unsigned long long wall_ns, frames_used, frames_total;
    struct outrun_process proc[12];
};
struct outrun_settings {
    unsigned int scale, accent, repeat_delay, repeat_period;
};

/* ===========================================================================
 * v1.0+ HARDWARE / SYSTEM OBSERVABILITY  —  SYS_HW_INFO(domain, out, size)
 *
 * Read-only. One call, four domains, each with its own struct; `size` is
 * checked against that domain's struct and a disagreement is REFUSED rather
 * than partly filled, exactly as SYS_DESKTOP_INFO does. This header is the
 * master copy of every layout below and the kernel's local declarations must
 * move with it.
 *
 * EVERY FIELD HERE HAS A LIVE SOURCE IN THE KERNEL. Nothing is reserved for a
 * counter that does not yet exist: a field that reads zero on every boot
 * because nothing can raise it is the failure this tree already documents.
 * ======================================================================== */
#define OUTRUN_HW_ABI_VERSION 1
#define HW_STORAGE 0
#define HW_PCI     1
#define HW_TRACE   2
#define HW_NET     3

#define HW_MAX_BLOCKDEV 4
#define HW_MAX_PCI     16
#define HW_MAX_CPU      8
#define HW_MAX_IRQ     16
#define HW_MAX_THREAD  16
#define HW_MAX_NETIF    2
#define HW_MAX_SOCK    16

/* ---- HW_STORAGE ---------------------------------------------------------- */
struct outrun_blockdev {
    unsigned long long capacity_sectors, irqs;
    unsigned int sector_size, queue_size, ready, irq_line;
    char name[16];
};
struct outrun_storage_info {
    unsigned int version, size, ndev, cas_mounted;
    unsigned int cas_version, cas_block_size, vfs_files_used, vfs_files_max;
    unsigned long long cas_total_blocks, cas_used_blocks;
    unsigned long long cas_put_count, cas_dedup_hits;
    unsigned long long cas_blocks_freed, cas_ref_drops, cas_ref_underflow;
    unsigned long long cas_recover_calls, cas_recover_replays;
    unsigned long long vfs_bytes;
    struct outrun_blockdev dev[HW_MAX_BLOCKDEV];
};

/* ---- HW_PCI -------------------------------------------------------------- */
/* state bits mirror the kernel device registry. */
#define HW_PCI_BOUND_HOST 1u   /* a kernel driver is bound to this function   */
#define HW_PCI_CLAIMED    2u   /* a live ring-3 process owns it (VFIO)        */
#define HW_PCI_SYNTHETIC  4u   /* no real config space: bdf is not meaningful */
/* A host-bound device with a QUIESCE HOOK can be handed to ring 3 by its own
 * driver standing down; one without a hook cannot be claimed at all while the
 * kernel is driving it. The distinction is the difference between "busy now"
 * and "never available", and a VFIO explorer that could not tell them apart
 * would report the same EBUSY for both. */
#define HW_PCI_QUIESCABLE 8u
struct outrun_pci_dev {
    unsigned long long bar_base[6], bar_len[6];
    unsigned long long req_cap, owner_pid;
    unsigned int cfg[4];       /* config dwords 0x00, 0x08, 0x0C and 0x34      */
    unsigned int bdf, state, claim_flags, reserved;
    char name[24];
};
struct outrun_pci_info {
    unsigned int version, size, ndev, iommu_on;
    struct outrun_pci_dev dev[HW_MAX_PCI];
};

/* ---- HW_TRACE ------------------------------------------------------------ */
struct outrun_irq_stat {
    unsigned long long total, percpu[HW_MAX_CPU];
    unsigned int line, reserved;
};
/* state values are the kernel scheduler's own: 0 free, 1 runnable, 2 running,
 * 3 blocked, 4 claimed-but-not-yet-built. */
struct outrun_thread_stat {
    unsigned long long pid;      /* the ring-3 process this thread runs, or 0 */
    unsigned int tid, state, uthread, proc_slot;
    char name[16];
};
struct outrun_trace_info {
    unsigned int version, size, ncpu, nirq;
    unsigned int nthread, nproc, reserved0, reserved1;
    unsigned long long ticks, wall_ns;
    unsigned long long frames_total, frames_used, frames_freed, frames_reused;
    unsigned long long frames_shared, frames_cow;
    /* PER-CORE ring-3 busy time, accumulated on the core that hosted each
     * excursion at the same site that charges the process (see the fold in
     * resume_kernel). Load is a DELTA of this against wall_ns, not a snapshot:
     * a monitor that printed the raw total would be drawing uptime. */
    unsigned long long cpu_busy_ns[HW_MAX_CPU], cpu_excursions[HW_MAX_CPU];
    unsigned long long cpu_cur_pid[HW_MAX_CPU];
    unsigned int cpu_online[HW_MAX_CPU];
    struct outrun_irq_stat irq[HW_MAX_IRQ];
    struct outrun_thread_stat thread[HW_MAX_THREAD];
    struct outrun_process proc[12];
};

/* ---- HW_NET -------------------------------------------------------------- */
/* Bytes AND frames. A frame count is not bandwidth: sixty 60-byte ARP frames
 * and sixty 1514-byte segments produce the same number, so a monitor built on
 * frames alone draws the same graph for two links an order of magnitude apart.
 * ipv4 is this guest's address in host byte order. */
struct outrun_netif {
    unsigned char mac[6];
    unsigned short bdf;
    unsigned long long tx_frames, rx_frames, tx_bytes, rx_bytes, irqs;
    unsigned int ready, irq_line, mtu, ipv4;
    char name[16];
};
#define HW_SOCK_STREAM    1u
#define HW_SOCK_LISTENING 2u
#define HW_SOCK_CONNECTED 4u
struct outrun_socket {
    unsigned long long owner_pid;
    unsigned int lport, rport, raddr, state;
    unsigned int flags, rx_queued, tx_queued, gen;
};
struct outrun_net_info {
    unsigned int version, size, nif, nsock;
    unsigned long long tx_frames, loop_deliveries, accepts, eagain, sessions;
    struct outrun_netif iface[HW_MAX_NETIF];
    struct outrun_socket sock[HW_MAX_SOCK];
};

/* ---- SYS_FB_CAPTURE(out, (x<<16)|y, (w<<16)|h) ---------------------------
 * Copies a rectangle of the LOGICAL desktop (desk_w x desk_h, the coordinate
 * space windows and the pointer live in) into `out` as w*h packed 32-bit
 * pixels, and returns the byte count. Refuses a rectangle that is not wholly
 * on screen rather than clamping it, so a screenshot tool never silently saves
 * a region other than the one it asked for.
 * ======================================================================== */
#define HW_CAPTURE_MAX_PIXELS (1920u * 1200u)
#endif
