#!/bin/bash
# Boot, wait for the prompt, type `vfsappend 256`, capture the sizing lines.
cd /mnt/c/Users/jrmym/Documents/GitHub-Pull/OutRun-OS/metal || exit 1
LOG=build/cfm2.log
dd if=/dev/zero of=build/cfm2.img bs=1M count=4 2>/dev/null

{
  # Wait for the shell prompt to appear in the log, then type the benchmark.
  for i in $(seq 1 1400); do
    if grep -aq "outrun>" "$LOG" 2>/dev/null; then break; fi
    sleep 1
  done
  sleep 2
  printf 'vfsappend 256\n'
  # Let the benchmark finish (it builds 256 chunks; ~25 s guest, allow plenty).
  for i in $(seq 1 400); do
    if grep -aq "cas_flush_meta if dirty-scoped" "$LOG" 2>/dev/null; then break; fi
    sleep 1
  done
  sleep 3
} | timeout 1800 qemu-system-x86_64 \
      -cdrom build/outrun-os-1.0.0.iso -m 512M -nographic -no-reboot \
      -vga none -device virtio-vga \
      -drive file=build/cfm2.img,if=none,format=raw,id=vd0 \
      -device virtio-blk-pci,drive=vd0,disable-legacy=on,disable-modern=off \
      -netdev user,id=n0 \
      -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef \
      > "$LOG" 2>&1

echo "EXIT=$?"
echo "=== SIZING ==="
grep -a "cas puts:\|cas_flush_meta\|virtio-blk:\|built 256 chunks\|journal:" "$LOG"
