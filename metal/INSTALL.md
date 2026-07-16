# Deploying Outrun OS 0.2.0-metal — Proxmox and Bare Metal

## What this image is, honestly

This is a real, bootable Outrun OS kernel — GRUB Multiboot2 boot, hand-written
assembly bootstrap into x86_64 long mode, a freestanding C core with a live
interrupt system, dual VGA/serial console, Multiboot2 memory-map parsing, and
the Outrun capability table running in kernel space with an interactive shell.
It has been boot-verified under both SeaBIOS-style legacy boot and OVMF UEFI
firmware, the two firmware options Proxmox offers.

It is a development kernel at roadmap Phase 1, not a production operating
system. It has no filesystem, no networking, no GUI, and no user mode yet.
Anyone who tells you a production OS rivaling Windows can be produced in a day
is selling something; what you have here is the honest foundation — a kernel
you own completely, that boots on real firmware, that you can extend phase by
phase per docs/ROADMAP.md.

## Proxmox VE deployment

Upload `metal/build/outrun-os-0.2.0.iso` to your Proxmox node (Datacenter →
node → local storage → ISO Images → Upload, or scp it to
`/var/lib/vz/template/iso/`). Create a VM with these settings: OS type Other,
the Outrun ISO attached as CD/DVD, BIOS either SeaBIOS (default) or OVMF —
both are verified working, 512 MB+ RAM, 1+ cores, and no hard disk required.
Display can stay Default; the kernel drives the VGA text console.

For the best experience add a serial console: in the VM's Hardware tab add a
Serial Port (serial0), then use `qm terminal <vmid>` or the xterm.js console.
The Outrun shell listens on VGA+PS/2 and COM1 simultaneously, so both the
noVNC display and the serial terminal are live shells at the same time.

Boot the VM. GRUB appears for two seconds, then the kernel banner, the memory
map Proxmox handed us, and the `outrun>` prompt. Type `help`, run `demo` for
the capability walk-through, `mem` to re-read the e820 map, `panic` to watch
the exception handler contain a deliberate CPU fault, `reboot` to warm-reset.

## Bare metal / NVMe deployment

The ISO is a hybrid image (El Torito BIOS + UEFI entries plus an MBR boot
sector), so it can be written directly to any raw block device:

    dd if=outrun-os-0.2.0.iso of=/dev/nvme0n1 bs=4M status=progress
    sync

Double-check the target device name first — dd overwrites everything on it,
and this should never be a disk holding data you care about. Then boot the
machine from that drive in either legacy or UEFI mode. A USB stick works
identically and is the safer first test on real hardware.

Hardware notes for bare metal: the kernel needs an x86_64 CPU, drives the
legacy VGA text console and PS/2-compatible keyboard path (most desktop
firmware emulates PS/2 for USB keyboards via SMM; some very new boards don't,
in which case use the serial header or test in a VM), and prints to COM1 at
115200 8N1 if a serial port exists.

## Building from source

    cd metal && make          # kernel.elf + hybrid ISO
    make qemu                 # boot it headless, serial shell on stdio
    make qemu-vga             # boot with a display window

Toolchain: gcc, nasm, ld, grub-mkrescue (grub-pc-bin + grub-efi-amd64-bin +
xorriso + mtools), qemu-system-x86 for testing. All stock Ubuntu/Debian
packages; no cross-compiler required.

## What's next

The userspace prototype in the repo root already proved the OS's core
protocols (zero-copy device regions, generation-revoked capabilities, crash
supervision, A/B updates) with measured numbers. This metal kernel is the
vessel those protocols migrate into: Phase 2 adds user mode and ELF loading so
the driver/app/gesture trio runs on *this* kernel instead of Linux, then
virtio drivers, then storage and the content-addressable filesystem. Each
phase keeps the same rule this one followed — nothing claimed that isn't
booted and demonstrated.
