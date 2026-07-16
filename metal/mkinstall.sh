#!/bin/bash
# Build a dd-able, UEFI-bootable GPT disk image (install onto raw NVMe via dd).
set -e
cd "$(dirname "$0")"
VER=0.36.0
cat > /tmp/embed.cfg << 'CFG'
set timeout=0
insmod part_gpt
insmod fat
insmod efi_gop
insmod video_bochs
insmod multiboot2
insmod normal
set gfxmode=1024x768x32
set gfxpayload=keep
search --no-floppy --file --set=root /outrun-kernel.elf
multiboot2 /outrun-kernel.elf outrun VER-metal
module2 /user_init.elf user_init.elf
boot
CFG
sed -i "s/VER-metal/${VER}-metal/" /tmp/embed.cfg
grub-mkstandalone -O x86_64-efi -o /tmp/BOOTX64.EFI "boot/grub/grub.cfg=/tmp/embed.cfg"
ESP=/tmp/esp.img
dd if=/dev/zero of=$ESP bs=1M count=90 status=none
mformat -i $ESP -F -v OUTRUN ::
mmd -i $ESP ::/EFI ::/EFI/BOOT
mcopy -i $ESP /tmp/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy -i $ESP build/outrun-kernel.elf ::/outrun-kernel.elf
mcopy -i $ESP build/user_init.elf ::/user_init.elf
python3 mkgpt.py
echo "install image: build/outrun-install.img"
