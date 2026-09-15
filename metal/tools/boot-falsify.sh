#!/bin/bash
# Boot ONLY the falsify image. Separate from boot-both.sh because that script's
# leading pkill also kills any shell watching it, which stopped the second boot
# from ever starting.
set -u
rm -f /tmp/vblk_falsify.img
qemu-img create -f raw /tmp/vblk_falsify.img 64M >/dev/null 2>&1
echo "=== HARNESS tag=falsify $(md5sum /tmp/falsify.iso) ===" > /tmp/falsify.boot
timeout 1500 qemu-system-x86_64 \
    -cdrom /tmp/falsify.iso -m 512M -nographic -no-reboot \
    -drive if=none,id=vb,file=/tmp/vblk_falsify.img,format=raw \
    -device virtio-blk-pci,drive=vb \
    -netdev user,id=n0 \
    -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef \
    >> /tmp/falsify.boot 2>&1
echo "BOOT_RC=$?" >> /tmp/falsify.boot
echo "FALSIFY_BOOT_COMPLETE $(wc -c < /tmp/falsify.boot) bytes"
grep -a "nethop" /tmp/falsify.boot
