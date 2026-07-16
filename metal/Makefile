# =============================================================================
# OUTRUN OS — BARE-METAL BUILD (metal/Makefile)
# =============================================================================
#   make            build kernel.elf and the bootable hybrid ISO
#   make qemu       boot the ISO headless in QEMU with serial on stdio
#   make qemu-vga   boot with a VGA window (if you have a display)
# =============================================================================

BUILD   := build
NASM    := nasm -f elf64
CC      := gcc
CXX     := g++
CFLAGS  := -ffreestanding -O2 -Wall -Wextra -std=c11 \
           -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -msoft-float \
           -mcmodel=kernel -fno-pic -fstack-protector-strong -fno-builtin
LD      := ld
LDFLAGS := -n -T linker.ld -z max-page-size=0x1000

OBJS := $(BUILD)/boot.o $(BUILD)/isr.o $(BUILD)/usermode.o $(BUILD)/switch.o $(BUILD)/kernel64.o \
        $(BUILD)/cap_engine.o $(BUILD)/ipc_ring.o
ISO  := $(BUILD)/outrun-os-0.35.0.iso

.PHONY: all qemu qemu-vga clean
all: $(ISO)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot/boot.asm | $(BUILD)
	$(NASM) $< -o $@

$(BUILD)/apboot.bin: boot/apboot.asm | $(BUILD)
	nasm -f bin $< -o $@
	@echo "  AP trampoline: $@ (flat binary, org 0x8000)"

$(BUILD)/isr.o: boot/isr.asm $(BUILD)/apboot.bin | $(BUILD)
	$(NASM) $< -o $@

$(BUILD)/usermode.o: boot/usermode.asm | $(BUILD)
	$(NASM) $< -o $@

$(BUILD)/switch.o: boot/switch.asm | $(BUILD)
	$(NASM) $< -o $@

$(BUILD)/kernel64.o: kernel/kernel64.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- polyglot: Rust (no_core) + C++ (freestanding) linked into the kernel ---
RUSTFLAGS := --edition 2021 --target x86_64-unknown-none -C panic=abort \
             -C code-model=kernel -C relocation-model=static -O \
             --emit=obj --crate-type staticlib
CXXFLAGS := -ffreestanding -O2 -Wall -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
            -fno-threadsafe-statics -mno-red-zone -mno-sse -mno-mmx -mcmodel=kernel \
            -fno-pic -fno-stack-protector -std=c++17

$(BUILD)/cap_engine.o: rust/cap_engine.rs | $(BUILD)
	RUSTC_BOOTSTRAP=1 rustc $(RUSTFLAGS) $< -o $@
	@echo "  rust obj:  $@ (no_core, x86_64-unknown-none)"

$(BUILD)/ipc_ring.o: cpp/ipc_ring.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@
	@echo "  c++ obj:   $@ (freestanding)"

# ---- userland: a standalone ring-3 ELF shipped as a boot module ------------
UCFLAGS := -ffreestanding -O2 -Wall -mcmodel=large -fno-pic -fno-stack-protector -fno-builtin \
           -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -msoft-float
$(BUILD)/user_init.o: user/init.c | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user_init.elf: $(BUILD)/user_init.o user/user.ld
	$(LD) -n -T user/user.ld $< -o $@
	@echo "  user ELF linked: $@"

$(BUILD)/outrun-kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@
	@echo "  kernel linked: $@"

$(ISO): $(BUILD)/outrun-kernel.elf $(BUILD)/user_init.elf iso/boot/grub/grub.cfg
	cp $(BUILD)/outrun-kernel.elf iso/boot/outrun-kernel.elf
	cp $(BUILD)/user_init.elf iso/boot/user_init.elf
	grub-mkrescue -o $@ iso 2>/dev/null
	@echo ""
	@echo "  Bootable hybrid image ready: $(ISO)"
	@echo "    Proxmox VM : attach as CD-ROM (SeaBIOS or OVMF), boot"
	@echo "    Bare metal : dd if=$(ISO) of=/dev/nvmeXn1 bs=4M && reboot"

QEMU_DEV := -device virtio-net-pci,disable-legacy=on,disable-modern=off
VBLK_IMG := $(BUILD)/vblk.img
QEMU_BLK := -drive file=$(VBLK_IMG),if=none,format=raw,id=vd0 \
            -device virtio-blk-pci,drive=vd0,disable-legacy=on,disable-modern=off

$(VBLK_IMG): | $(BUILD)
	dd if=/dev/zero of=$@ bs=1M count=4 2>/dev/null
	printf 'OUTRUN-DISK-SIGNATURE-OK' | dd of=$@ bs=1 seek=1024 conv=notrunc 2>/dev/null
	@echo "  test disk created: $@ (signature at sector 2)"

QEMU_NET := -netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef

qemu: $(ISO) $(VBLK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -nographic -no-reboot $(QEMU_BLK) $(QEMU_NET)

qemu-vga: $(ISO) $(VBLK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -serial stdio -no-reboot $(QEMU_BLK) $(QEMU_NET)

clean:
	rm -rf $(BUILD) iso/boot/outrun-kernel.elf iso/boot/user_init.elf

# dd-able, UEFI-bootable GPT install image (install onto raw NVMe via dd)
install-img: all
	./mkinstall.sh

qemu-nvme: install-img
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -m 512M -nographic -no-reboot \
	  -drive file=build/outrun-install.img,format=raw,if=none,id=nvm -device nvme,drive=nvm,serial=OUTRUN01 \
	  $(QEMU_BLK) $(QEMU_NET)

# Boot on q35 with an emulated Intel VT-d IOMMU; virtio devices behind it.
QEMU_IOMMU := -machine q35,kernel-irqchip=split -device intel-iommu,intremap=off,caching-mode=on
qemu-iommu: all
	qemu-system-x86_64 $(QEMU_IOMMU) -cdrom build/outrun-os-$(VERSION).iso -m 512M -nographic -no-reboot \
	  -drive file=/tmp/cas.img,if=none,format=raw,id=vd0 \
	  -device virtio-blk-pci,drive=vd0,disable-legacy=on,disable-modern=off,iommu_platform=on \
	  -netdev user,id=n0 \
	  -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef,iommu_platform=on
