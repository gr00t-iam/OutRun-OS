#!/bin/bash
# Build the ISO to a SEPARATE name and boot it once with the given memory
# size, capturing serial to a log. Never touches build/outrun-os-*.iso so a
# concurrent `make gate` keeps booting the image it started with.
#   usage: devboot.sh <tag> <mem> [extra qemu args]
set -e
cd /mnt/c/Users/jrmym/Documents/GitHub-Pull/OutRun-OS/metal
TAG=$1; MEM=${2:-512M}; shift 2 || true
D=/tmp/dev-$TAG
CF="-ffreestanding -O2 -Wall -Wextra -std=c11 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -msoft-float -mcmodel=kernel -fno-pic -fstack-protector-strong -fno-builtin -mstack-protector-guard=tls -mstack-protector-guard-reg=gs -mstack-protector-guard-offset=80"
rm -rf $D; mkdir -p $D/iso/boot/grub
nasm -f elf64 boot/boot.asm -o $D/boot.o
gcc $CF -Ibuild -c kernel/kernel64.c -o $D/kernel64.o
ld -n -T linker.ld -z max-page-size=0x1000 $D/boot.o build/isr.o build/usermode.o build/switch.o $D/kernel64.o build/cap_engine.o build/ipc_ring.o build/scrypt.o -o $D/outrun-kernel.elf
cp -r iso/boot/* $D/iso/boot/
cp $D/outrun-kernel.elf $D/iso/boot/outrun-kernel.elf
grub-mkrescue -o $D/os.iso $D/iso 2>/dev/null
echo "iso md5: $(md5sum $D/os.iso | cut -d' ' -f1)"
# Same block + net devices the gate attaches (tools/gate-matrix.sh:138-145).
# Without the disk, CAS has no superblock and cas_index_find divides by zero
# in the first storage suite -- a pre-existing no-device defect, not a verdict.
dd if=/dev/zero of=$D/vblk.img bs=1M count=4 2>/dev/null
printf 'OUTRUN-DISK-SIGNATURE-OK' | dd of=$D/vblk.img bs=1 seek=1024 conv=notrunc 2>/dev/null
BLK="-drive file=$D/vblk.img,if=none,format=raw,id=vd0 -device virtio-blk-pci,drive=vd0,disable-legacy=on,disable-modern=off"
NET="-netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef"
timeout 900 qemu-system-x86_64 -cdrom $D/os.iso -m $MEM -nographic -no-reboot -vga none -device virtio-vga $BLK $NET "$@" < /dev/null > $D/boot.log 2>&1 || true
echo "boot log: $D/boot.log ($(wc -l < $D/boot.log) lines)"
