#!/bin/bash
# Boot the normal image then the falsify image, both under the SLIRP
# configuration `make qemu` uses. Takes NO arguments and expands NO caller
# variables: every previous attempt to pass paths through `wsl.exe -- bash -c`
# had its $VARs eaten by the Windows-side git-bash before WSL saw them, which
# silently turned `bash $B /tmp/normal.iso` into `bash /tmp/normal.iso` and
# "completed" without booting anything.
set -u

boot_one() {
    iso="$1"; out="$2"; tag="$3"
    img="/tmp/vblk_${tag}.img"
    rm -f "$img"
    qemu-img create -f raw "$img" 64M >/dev/null 2>&1
    # Stamp the image md5 into the log. A log that cannot name the binary it
    # came from is not evidence.
    echo "=== HARNESS tag=${tag} $(md5sum "$iso") ===" > "$out"
    timeout 1500 qemu-system-x86_64 \
        -cdrom "$iso" -m 512M -nographic -no-reboot \
        -drive if=none,id=vb,file="$img",format=raw \
        -device virtio-blk-pci,drive=vb \
        -netdev user,id=n0 \
        -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef \
        >> "$out" 2>&1
    echo "BOOT_RC=$?" >> "$out"
    echo "done ${tag}: $(wc -c < "$out") bytes"
}

pkill -f qemu-system-x86_64 >/dev/null 2>&1
sleep 2
boot_one /tmp/normal.iso  /tmp/normal.boot  normal
boot_one /tmp/falsify.iso /tmp/falsify.boot falsify
echo BOTH_BOOTS_COMPLETE
