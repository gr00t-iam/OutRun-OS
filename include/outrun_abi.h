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
#define SYS_VFS_SYNC 22
#define SYS_VFS_UNLINK 23
#define SYS_WIN_CREATE 40
#define SYS_WIN_DAMAGE 41
#define SYS_WIN_POLL 42
#define SYS_WIN_INFO 43
#define SYS_SYSINFO 44
#define SYS_KILL 50
#define SYS_LSEEK 100
#define SYS_FTRUNCATE 101
#define SYS_RENAME 102
#define SYS_NANOSLEEP 106
#define SYS_DESKTOP_INFO 117
#define SYS_DESKTOP_SETTINGS 118
#define EVENT_MOUSE_DOWN 1
#define EVENT_KEY_PRESS 2
#define OUTRUN_DESKTOP_ABI_VERSION 1
#define OUTRUN_WIN_BASE 0x0000550000000000ull
#define OUTRUN_WIN_STRIDE (((600ull * 440 * 4 + 4095) / 4096) * 4096)
/* WIN_CREATE a2=1 opts into paired buffers. WIN_DAMAGE returns the new back
 * buffer address after publication; a1 optionally supplies a NUL title.
 * Legacy a2=0 windows retain their old single-surface ABI. */
struct outrun_event { int type, x, y, code; };
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
#endif
