#!/bin/bash
# Boot an ISO under the same QEMU configuration `make qemu` uses, and stamp the
# image md5 into the log. A log that cannot name the binary it came from is not
# evidence -- this tree has twice found a run to have booted a different image
# than it claimed.
ISO="$1"; OUT="$2"; TAG="$3"
IMG="/tmp/vblk_${TAG}.img"
qemu-img create -f raw "$IMG" 64M >/dev/null 2>&1
{
  echo "=== HARNESS: image $(md5sum "$ISO") ==="
  echo "=== HARNESS: tag $TAG ==="
} > "$OUT"
timeout 1500 qemu-system-x86_64 \
  -cdrom "$ISO" -m 512M -nographic -no-reboot \
  -drive if=none,id=vb,file="$IMG",format=raw \
  -device virtio-blk-pci,drive=vb \
  -netdev user,id=n0 \
  -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef \
  >> "$OUT" 2>&1
echo "BOOT_RC=$?" >> "$OUT"
