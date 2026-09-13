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
           -mcmodel=kernel -fno-pic -fstack-protector-strong -fno-builtin \
           -mstack-protector-guard=tls -mstack-protector-guard-reg=gs \
           -mstack-protector-guard-offset=80
LD      := ld
LDFLAGS := -n -T linker.ld -z max-page-size=0x1000

OBJS := $(BUILD)/boot.o $(BUILD)/isr.o $(BUILD)/usermode.o $(BUILD)/switch.o $(BUILD)/kernel64.o \
        $(BUILD)/cap_engine.o $(BUILD)/ipc_ring.o $(BUILD)/scrypt.o
# v1.0.0. The suffix convention still holds: a dev cycle is `X.Y.Z-dev` and the
# ISO is named after it, so a development image cannot be mistaken for a release
# artefact by its filename alone. At release time it becomes plain X.Y.Z and
# KERNEL_VERSION becomes X.Y.Z-metal. release-version-check cross-checks all
# three strings (this one, KERNEL_VERSION, and the grub menuentry).
VERSION := 1.1.0
ISO  := $(BUILD)/outrun-os-$(VERSION).iso

.PHONY: all qemu qemu-vga qemu-audio qemu-usb qemu-iommu qemu-nvme install-img clean
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

# EXTRA lets a fast-iteration build add -D flags without editing this file, e.g.
#   make EXTRA=-DPOSIX_ITER   -> boot straight into cmd_posix_stress (see kernel64.c)
EXTRA   ?=

# v0.58: the userland tools the kernel publishes into /src are REAL PROGRAMS,
# not decoration, so they live as ordinary editable files under user/ and are
# converted to C string literals at build time. Keeping several hundred lines of
# occ-subset C as escaped quotes inside kernel64.c would make them unreadable
# and, worse, unreviewable.
SDK_SRCS := user/vedit.oc user/omake.oc user/vsh.oc user/demo.mk \
            user/hello.h user/hello_a.oc user/hello_b.oc
$(BUILD)/sdk_sources.h: $(SDK_SRCS) tools/mkstr.py | $(BUILD)
	python3 tools/mkstr.py $@ \
	  SDK_VEDIT_C=user/vedit.oc \
	  SDK_OMAKE_C=user/omake.oc \
	  SDK_VSH_C=user/vsh.oc \
	  SDK_DEMO_MK=user/demo.mk \
	  SDK_HELLO_H=user/hello.h \
	  SDK_HELLO_A=user/hello_a.oc \
	  SDK_HELLO_B=user/hello_b.oc
	@echo "  sdk sources: $@ ($(words $(SDK_SRCS)) files embedded)"

# v0.80: the scrypt core, compiled freestanding into the kernel from the SAME
# source the host vector test validates. One implementation, two builds -- a
# second copy compiled differently would be a second thing to keep correct, and
# the RFC 7914 vectors would only ever have covered one of them.
$(BUILD)/scrypt.o: crypto/scrypt.c crypto/scrypt.h | $(BUILD)
	$(CC) $(CFLAGS) $(EXTRA) -c $< -o $@

$(BUILD)/kernel64.o: kernel/kernel64.c kernel/files.c kernel/key_state.h $(BUILD)/sdk_sources.h crypto/scrypt.h | $(BUILD)
	$(CC) $(CFLAGS) $(EXTRA) -c $< -o $@

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
# v0.77: -Wextra brought to parity with the kernel's CFLAGS. Ring 3 had been held
# to -Wall alone, and the three findings that appeared the moment it was raised
# were not stylistic: two `< 0` guards on a u64 return that could never fire, and
# a loop counter live across osetjmp/olongjmp without volatile. A warning level
# that differs between two halves of one tree hides defects in the quieter half.
UCFLAGS := -ffreestanding -O2 -Wall -Wextra -mcmodel=large -fno-pic -fno-stack-protector -fno-builtin \
           -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -msoft-float
# v0.57: occ.c is #included by init.c, so it is a real prerequisite. Without it
# listed here, `make` reported success while silently shipping the PREVIOUS
# compiler — an edit to occ.c alone never reached the image, which cost a full
# boot cycle to notice. A header-style dependency that make cannot see is worse
# than a slow build.
# v0.78: UEXTRA is EXTRA's ring-3 counterpart. EXTRA reaches only the kernel
# compile rule, so until now a -D could not gate anything in init.c — which meant
# a reproducer needing BOTH a kernel change and a ring-3 change could not be
# built at all. Same caveat as EXTRA, and it bites the same way: make has no
# dependency on the flag's VALUE, so `make clean` when toggling either.
$(BUILD)/user_init.o: user/init.c user/occ.c | $(BUILD)
	$(CC) $(UCFLAGS) $(UEXTRA) -c $< -o $@
$(BUILD)/user_init.elf: $(BUILD)/user_init.o user/user.ld
	$(LD) -n -T user/user.ld $< -o $@
	@echo "  user ELF linked: $@"

# ---- the desktop applications: standalone ring-3 ELFs, one per program ------
# Each is an ordinary boot module the kernel loads by NAME (see mod_find in
# kernel64.c), so adding a program here is the whole of adding a program. They
# share apps/gui.c and are held to the same -Wall -Wextra the kernel is: ring 3
# being the quieter half is exactly how v0.77 hid three real defects.
APPS      := vault_pad calc task_mgr settings outrun_term outrun_edit outrun_disks pci_view sys_trace net_deck outrun_snap outrun_media outrun_web
APP_ELFS  := $(APPS:%=$(BUILD)/apps/%.elf)
APP_SRC   := ../apps
APP_DEPS  := $(APP_SRC)/gui.h $(APP_SRC)/font.h $(APP_SRC)/build_task.h ../include/outrun_abi.h $(APP_SRC)/bmp.h

# ---- OutRun Web: the one application with a third-party dependency ----------
# It needs Mbed TLS, which is vendored UNMODIFIED under apps/vendor/mbedtls and
# must stay that way: the security value of a maintained TLS stack is precisely
# that it is the upstream one. It therefore gets its own compile and link rules
# rather than the generic single-object pattern above.
#
# Three things here are load-bearing:
#
#  * -Wno-error on the VENDORED sources only. Upstream is held to upstream's
#    warning policy, not ours; our own code keeps -Werror. Applying our flags to
#    a dependency would make every upstream bump a tree-wide build break, which
#    is how a project ends up pinned to an old TLS library.
#  * The CA trust store is GENERATED (tools/mk-ca-roots.py) into a header. The
#    browser is a boot module with no path to a system trust store, so the roots
#    it verifies against have to be in the binary and visible in the diff that
#    changes them.
#  * MBEDTLS_CONFIG_FILE selects the freestanding profile in
#    apps/web_mbedtls_config.h -- no filesystem, no POSIX sockets, no time(2).
WEB_SRC   := $(APP_SRC)/outrun_web.c
WEB_VENDOR := $(APP_SRC)/vendor/mbedtls
WEB_CA    := $(APP_SRC)/web_ca_roots.h
WEB_LIB_SRC := $(wildcard $(WEB_VENDOR)/library/*.c)
WEB_LIB_OBJ := $(patsubst $(WEB_VENDOR)/library/%.c,$(BUILD)/apps/mbedtls/%.o,$(WEB_LIB_SRC))
WEB_CFLAGS := $(UCFLAGS) -I$(APP_SRC) -I$(WEB_VENDOR)/include \
              -DMBEDTLS_CONFIG_FILE='"web_mbedtls_config.h"'

$(WEB_CA): $(APP_SRC)/vendor/cacert.pem tools/mk-ca-roots.py
	python3 tools/mk-ca-roots.py $< $@

$(BUILD)/apps/mbedtls:
	mkdir -p $@

$(BUILD)/apps/mbedtls/%.o: $(WEB_VENDOR)/library/%.c | $(BUILD)/apps/mbedtls
	$(CC) $(WEB_CFLAGS) -Wno-error -c $< -o $@

$(BUILD)/apps/outrun_web.o: $(WEB_SRC) $(APP_DEPS) $(WEB_CA) | $(BUILD)/apps
	$(CC) $(WEB_CFLAGS) -Werror $(UEXTRA) -c $< -o $@

# The libc subset Mbed TLS expects from a hosted platform. See web_libc.c for
# why this exists rather than a patch to the vendored tree.
$(BUILD)/apps/web_libc.o: $(APP_SRC)/web_libc.c | $(BUILD)/apps
	$(CC) $(WEB_CFLAGS) -Werror -c $< -o $@

$(BUILD)/apps/outrun_web.elf: $(BUILD)/apps/outrun_web.o $(BUILD)/apps/gui.o \
                              $(BUILD)/apps/web_libc.o $(WEB_LIB_OBJ) user/user.ld
	$(LD) -n -T user/user.ld $< $(BUILD)/apps/gui.o $(BUILD)/apps/web_libc.o \
	      $(WEB_LIB_OBJ) -o $@
	@echo "  app linked: $@ (with vendored Mbed TLS)"

$(BUILD)/apps:
	mkdir -p $@

$(BUILD)/apps/gui.o: $(APP_SRC)/gui.c $(APP_DEPS) | $(BUILD)/apps
	$(CC) $(UCFLAGS) -Werror $(UEXTRA) -c $< -o $@

$(BUILD)/apps/%.o: $(APP_SRC)/%.c $(APP_DEPS) | $(BUILD)/apps
	$(CC) $(UCFLAGS) -Werror $(UEXTRA) -c $< -o $@

$(BUILD)/apps/%.elf: $(BUILD)/apps/%.o $(BUILD)/apps/gui.o user/user.ld
	$(LD) -n -T user/user.ld $< $(BUILD)/apps/gui.o -o $@
	@echo "  app linked: $@"

apps: $(APP_ELFS)

$(BUILD)/apps/outrun_edit.o: $(APP_SRC)/vault_pad.c

# The applications' own host-side unit tests. They run in under a second on the
# build host and need no image, so there is no excuse for them not to run before
# a boot: a defect in the calculator's overflow guard is far cheaper to see here
# than in a serial log 300 s into an emulated boot.
.PHONY: apps apps-test
apps-test: kernel-test web-test
	bash ../apps/tests/run.sh

# The KERNEL's host-testable logic. These cover code that lives in kernel64.c
# and its headers, so they are not app tests and run.sh is the wrong place for
# them -- but they need no image either, and until now nothing ran them at all.
#
# test-wm-host.py and test-spatial-host.py were both reachable only by typing
# their path by hand, which is the harness version of a counter nothing prints:
# the assertions existed, passed, and gated nothing. test-spatial-host.py was in
# fact BROKEN (it redeclared bx/by into test-wm-host.py's main() and failed
# -Werror), and that went unnoticed precisely because no target invoked it.
.PHONY: kernel-test
kernel-test:
	python3 tools/test-wm-host.py
	python3 tools/test-spatial-host.py

# The BROWSER's host tests. Same story as kernel-test, one step worse: these
# three files existed, contained real assertions, and NOTHING RAN THEM. run.sh
# iterates a fixed app list plus apps/test_*.c, and all three browser tests
# live in apps/tests/, which no glob covers -- so `make apps-test` reported
# "all host tests passed" while grep for "web" in its output returned nothing.
#
# They cannot join run.sh's generic loop because the browser is the one app
# with a third-party dependency: they need the Mbed TLS include path, the
# freestanding config, the GENERATED CA trust store, and host builds of the
# vendored library to link against. The ring-3 objects above cannot be reused
# for that -- they are freestanding and -mcmodel=large -- so this target builds
# a host copy, but derives every flag from the same variables.
WEB_TEST_CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
                   -I$(APP_SRC) -I$(WEB_VENDOR)/include \
                   -DMBEDTLS_CONFIG_FILE='"web_mbedtls_config.h"'
WEB_HOST_OBJ := $(patsubst $(WEB_VENDOR)/library/%.c,$(BUILD)/webtest/mbedtls/%.o,$(WEB_LIB_SRC))
$(BUILD)/webtest/mbedtls:
	mkdir -p $@
$(BUILD)/webtest/mbedtls/%.o: $(WEB_VENDOR)/library/%.c | $(BUILD)/webtest/mbedtls
	$(CC) $(WEB_TEST_CFLAGS) -Wno-error -c $< -o $@
# gui.c needs APP_HOST_TEST on the command line (it has no #define of its own);
# the test sources define it themselves, so passing -D globally would redefine
# it under -Werror. Compile the toolkit separately and link the object.
$(BUILD)/webtest/gui.o: $(APP_SRC)/gui.c | $(BUILD)/webtest/mbedtls
	$(CC) $(WEB_TEST_CFLAGS) -DAPP_HOST_TEST -c $< -o $@
.PHONY: web-test
web-test: $(WEB_CA) $(WEB_HOST_OBJ) $(BUILD)/webtest/gui.o
	@mkdir -p $(BUILD)/webtest
# test_web defines WEB_CORE_TEST, which compiles the ENTIRE TLS section out of
# outrun_web.c -- including its mbedtls_platform_gmtime_r definition. Linking
# the library into it therefore fails on a symbol that build deliberately does
# not have, so the core test is built WITHOUT the vendored objects and the two
# network tests with them.
	gcc $(WEB_TEST_CFLAGS) $(APP_SRC)/tests/test_web.c -o $(BUILD)/webtest/test_web
	$(BUILD)/webtest/test_web
# test_web_ui exercises the real renderer, so it links the GUI toolkit and
# supplies its own sysc() stub. test_web_network drives POSIX sockets and never
# touches a window, so it links neither -- giving it gui.o would demand a sysc()
# it has no reason to have.
	gcc $(WEB_TEST_CFLAGS) $(APP_SRC)/tests/test_web_ui.c $(BUILD)/webtest/gui.o \
	    $(WEB_HOST_OBJ) -o $(BUILD)/webtest/test_web_ui
	$(BUILD)/webtest/test_web_ui
# test_web_network drives the TRANSPORT state machine only -- no parsing, no
# history, no image decode -- so those functions are genuinely unreferenced in
# that translation unit. They are live code reached by the other two tests and
# by the shipping ELF, so the honest flag is to stop treating unused-function
# as an error for this one binary rather than to delete or fake a caller.
# Sanitizers stay FULLY on, alignment included: the misaligned x509_crt accesses
# UBSan found here were fixed at the root with MBEDTLS_MEMORY_ALIGN_MULTIPLE 8
# in web_mbedtls_config.h, not suppressed.
	gcc $(WEB_TEST_CFLAGS) \
	    -Wno-error=unused-function -Wno-unused-function \
	    $(APP_SRC)/tests/test_web_network.c $(WEB_HOST_OBJ) \
	    -o $(BUILD)/webtest/test_web_network
# test_web_network is DRIVEN, not standalone: run-web-network.py takes the
# binary as argv[1] and stands up real loopback HTTP/TLS servers with ephemeral
# certificates and ports around it. Running the binary bare exits non-zero
# because there is nothing listening -- which is correct behaviour, not a
# failure. It needs python3-cryptography; when that is missing the harness is
# SKIPPED with a message rather than silently counted as a pass.
	@if python3 -c 'import cryptography' 2>/dev/null; then \
	  python3 $(APP_SRC)/tests/run-web-network.py $(BUILD)/webtest/test_web_network || exit 1; \
	else \
	  echo "web network: SKIPPED (python3-cryptography not installed)"; \
	fi

# The desktop's own acceptance test: boots the desktop image, drives the REAL
# input path over QMP, and fails unless every launcher tile starts its program
# and the compositor is still running at the end. Roughly 3 minutes.
.PHONY: desktop-test
desktop-test: $(DESKTOP_ISO)
	python3 tools/desktop-ui-test.py $(DESKTOP_ISO) $(GATE_LOGS)/desktop-ui-$$$$.log

# The desktop at -smp 4. The compositor and its applications are pinned to the
# BSP, so the other cores are live, idle and STEALING — which is the shape that
# would expose a compositor racing an application's surface publication. A
# desktop verified only at one vCPU has not been verified.
.PHONY: desktop-test-smp
desktop-test-smp: $(DESKTOP_ISO)
	DESKTOP_QEMU="-smp 4" python3 tools/desktop-ui-test.py $(DESKTOP_ISO) \
	  $(GATE_LOGS)/desktop-ui-smp-$$$$.log

# The snap -> VFS -> media pipeline on a LIVE VOLUME. Every other desktop
# target boots with no block device, so OUTRUN SNAP has never written a file
# and OUTRUN MEDIA has only ever been seen rendering an empty playlist. This
# one attaches a virtio-blk volume, captures a region in the guest, reads the
# guest's own `vfs` listing back out of the serial log, decodes the frame in
# the player, and then scans the raw volume from the host for the BMP magic.
# About 8 minutes.
.PHONY: desktop-media-test
desktop-media-test: $(DESKTOP_ISO)
	python3 tools/desktop-media-test.py $(DESKTOP_ISO) $(GATE_LOGS)/desktop-media-$$$$.log

# Long-run desktop soak: holds the session open past SOAK_FRAMES composited
# frames while opening and closing applications continuously, and fails if
# the compositor stalls, a window fails to close, or the allocator's resting
# level drifts. desktop-test cannot see a leak that costs a few frames an
# hour; this is the target that can. About 45 min at 50,000 frames under TCG.
#   make desktop-soak                       50,000 frames, uniprocessor
#   make desktop-soak SOAK_QEMU="-smp 4"    the -smp 4 desktop
SOAK_FRAMES ?= 50000
.PHONY: desktop-soak
desktop-soak: $(DESKTOP_ISO)
	python3 tools/desktop-soak.py $(DESKTOP_ISO) $(GATE_LOGS)/desktop-soak-$$$$.log $(SOAK_FRAMES)

# Everything the desktop image is checked by, in cost order. `make gate` boots
# the REGRESSION image and is deliberately untouched by this: every timing
# budget in the tree was calibrated against the suite boot, and a desktop loop
# underneath the suites would change what they measure. The two images share a
# kernel, so a kernel regression shows in `gate`; what only THIS can show is a
# regression in the session the kernel runs when told to.
.PHONY: gate-desktop
gate-desktop: apps-test desktop-test desktop-test-smp

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

# v0.50: virtio-vga is VGA-compatible (bochs/VBE DISPI), so GRUB's existing
# video_bochs framebuffer bring-up (multiboot2 tag 8) works completely
# unchanged — verified standalone before adopting this device-wide, and
# re-verified across all 3 configs with 0 FAIL after. It ALSO exposes the
# real modern virtio-gpu PCI capability list (vendor 1af4 device 1050,
# class 0x03) our new virtio_gpu_probe() drives for real 2D acceleration.
# A separate `-vga std -device virtio-gpu-pci` (two independent devices)
# was tried to avoid SYS_GPU_SET_SCANOUT visually taking over the shared
# device's display output (see CHANGELOG-0.50.0.md) — but it introduced an
# unexplained hang under -smp 4 that virtio-vga does not have, so this
# kernel ships with the proven, fully-verified combined device instead.
QEMU_GPU := -vga none -device virtio-vga

qemu: $(ISO) $(VBLK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -nographic -no-reboot $(QEMU_GPU) $(QEMU_BLK) $(QEMU_NET)

# ---- the desktop image -----------------------------------------------------
# A SECOND ISO, not a second kernel. It carries the same kernel.elf and the same
# user_init.elf plus the four application modules, and its GRUB entry passes
# `desktop` on the command line — which is the only thing that makes kmain run
# a session instead of the regression battery.
#
# Two images rather than a runtime choice because `make gate` must keep booting
# EXACTLY what it has always booted. A desktop that could start underneath the
# suites would change what every timing budget in this tree was calibrated
# against, which CLAUDE.md rules out in as many words.
DESKTOP_ISO := $(BUILD)/outrun-desktop-$(VERSION).iso

# Prerequisites expand when read: the earlier recipes precede this variable.
desktop-test desktop-test-smp desktop-soak: $(DESKTOP_ISO)

$(DESKTOP_ISO): $(BUILD)/outrun-kernel.elf $(BUILD)/user_init.elf $(APP_ELFS) \
                iso/boot/grub/grub-desktop.cfg
	rm -rf $(BUILD)/desktop-iso
	mkdir -p $(BUILD)/desktop-iso/boot/grub $(BUILD)/desktop-iso/boot/apps
	cp $(BUILD)/outrun-kernel.elf $(BUILD)/desktop-iso/boot/outrun-kernel.elf
	cp $(BUILD)/user_init.elf     $(BUILD)/desktop-iso/boot/user_init.elf
	cp $(APP_ELFS)                $(BUILD)/desktop-iso/boot/apps/
	cp iso/boot/grub/grub-desktop.cfg $(BUILD)/desktop-iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/desktop-iso 2>/dev/null
	@echo "  Desktop image ready: $@ ($(words $(APPS)) applications)"

.PHONY: desktop
desktop: $(DESKTOP_ISO)

# QEMU_EXTRA exists so a harness can add -display/-qmp without editing this
# file; -accel tcg is explicit because every budget in this tree assumes it.
QEMU_EXTRA ?=
qemu-vga: $(DESKTOP_ISO) $(VBLK_IMG)
	qemu-system-x86_64 -accel tcg -cdrom $(DESKTOP_ISO) -m 512M -serial stdio -no-reboot \
	  $(QEMU_GPU) $(QEMU_BLK) $(QEMU_NET) $(QEMU_EXTRA)

# The regression image with a VGA window, which is what qemu-vga used to be.
qemu-vga-suites: $(ISO) $(VBLK_IMG)
	qemu-system-x86_64 -accel tcg -cdrom $(ISO) -m 512M -serial stdio -no-reboot \
	  $(QEMU_GPU) $(QEMU_BLK) $(QEMU_NET) $(QEMU_EXTRA)

# v0.51: virtio-sound-pci is verified in its OWN dedicated target, NOT the
# main virtio-vga regression targets above. Reason (root-caused live, see
# CHANGELOG-0.51.0.md): attaching -device virtio-sound-pci ALONGSIDE
# -device virtio-vga makes virtio-gpu's controlq stop completing under this
# environment's TCG-only QEMU — the very first RESOURCE_CREATE never gets a
# used-ring completion, hanging cmd_gpu_stress. Reproduced even with the
# kernel's virtio_sound_probe() ENTIRELY disabled (the device merely present
# on the bus is enough), so it is a QEMU device-topology interaction, not a
# kernel bug — the same class of finding as v0.50's "-vga std + virtio-gpu-pci
# hang under -smp 4" and this release's qemu-usb note. The virtio-sound driver
# itself is fully real and leak-free: this target uses plain -vga std (no
# virtio-gpu, so cmd_gpu_stress cleanly SKIPS) so cmd_audio_stress runs and is
# verified end-to-end (16 rounds + 4 fault-injection + 1 gap round, 0 FAIL).
# The "none" audiodev backend produces no host audio (correct for a headless
# sandbox) but drives a fully real virtio controlq/txq/DMA path.
qemu-audio: $(ISO) $(VBLK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -nographic -no-reboot -vga std \
	  -audiodev none,id=snd0 -device virtio-sound-pci,audiodev=snd0 \
	  $(QEMU_BLK) $(QEMU_NET)

# v0.51: xHCI + attached USB HID devices, kept OUT of the main qemu/qemu-vga/
# qemu-iommu targets deliberately. A running xHCI controller continuously
# emulates its own microframe (125us) timer, and under this environment's
# TCG-only execution (no KVM — confirmed unavailable in every prior
# changelog) that timer overhead was measured to slow the WHOLE guest so
# severely that the full ~25-suite regression boot no longer completes in
# any practical wall-clock time — reproduced even with the xHCI DRIVER CODE
# ITSELF disabled, so it is QEMU's own device emulation cost, not a kernel
# bug. Verified instead via this short, targeted boot (see
# CHANGELOG-0.51.0.md): controller bring-up, port detection, slot
# enumeration, and HID boot-protocol configuration all complete and are
# visible in the serial log within a few seconds.
qemu-usb: $(ISO) $(VBLK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -nographic -no-reboot $(QEMU_GPU) \
	  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0 \
	  $(QEMU_BLK) $(QEMU_NET)

# v0.76: THE DIRTY-VOLUME GATE CONFIGURATION.
#
# Every other qemu target here builds a fresh disk image per boot. That is
# precisely why the suite set could be non-idempotent across boots for its whole
# history without anyone noticing — nothing ever booted twice on one volume.
# v0.75 shipped a persistent user database, which makes "the volume already has
# state" a supported configuration, so it needs a configuration that tests it.
#
# Boot 1 runs the suites and then creates the two deliberately durable
# cross-boot artefacts; boots 2..N reuse that same image and must reach the
# prompt with zero failing assertions AND still find both artefacts. The script
# exits non-zero on any of those, so this target FAILS rather than printing red
# text and succeeding.
#
#   make gate-dirty                       3 boots, uniprocessor
#   make gate-dirty-smp                   3 boots, -smp 4
#   make gate-dirty GATE_DIRTY_BOOTS=5    longer run
# v0.78: GATE EVIDENCE LIVES OUTSIDE build/.
#
# Every harness used to write its logs under $(BUILD), which `make clean`
# removes. That has now cost this project real evidence twice: a carryover-3
# reproduction had to be re-run because clean ate its logs, and the only copy of
# the unexplained v0.76 [mcpre] failure is simply gone — its roadmap entry still
# says "preserved at" a path that no longer exists anywhere.
#
# .logs/ is gitignored, so this is about surviving a build step, not about
# committing everything: what matters gets copied into docs/ deliberately.
GATE_LOGS ?= .logs/gate

GATE_DIRTY_BOOTS ?= 3
GATE_DIRTY_QEMU  ?=
# The workdir carries the shell's PID so two invocations can never `rm -rf`
# each other's logs. gate-dirty.sh also takes an flock and refuses to start
# while another run holds it — see the comment there for what happened without
# these. The chosen path is printed in the run's own header.
gate-dirty: $(ISO)
	tools/gate-dirty.sh "$(ISO)" "$(GATE_DIRTY_BOOTS)" "$(GATE_DIRTY_QEMU)" "$(GATE_LOGS)/dirty-$$$$"

gate-dirty-smp: $(ISO)
	$(MAKE) gate-dirty GATE_DIRTY_QEMU="-smp 4"

# ===========================================================================
# v0.94: THE PROBE THE GATE CANNOT RUN
# ===========================================================================
# gate-matrix.sh boots qemu with `< /dev/null`, deliberately: a gate must not
# depend on anything being typed. The consequence went unnoticed for two
# releases — `timebench` is an interactive command, so ring-3 roles 63 and 64
# are unreachable from `make gate`, and v0.93's release notes carried a
# "role 63 probe: OK (1870)" line that no target in this tree could produce.
#
# That is the same failure CLAUDE.md records for the -smp 4 configurations
# before v0.78: a result reproducible by exactly one person is a habit, not a
# gate. This drives the command over a FIFO instead of a terminal, so it needs
# no tty and no human, and it FAILS unless role 63 exits 1870 and role 64
# exits 1900. It is not part of `make gate` because it is a different kind of
# run — one command, typed at the prompt, after the suites are already done.
#
#   make probe-timebench                    uniprocessor
#   make probe-timebench PROBE_QEMU="-smp 4"
PROBE_QEMU ?=
.PHONY: probe-timebench
probe-timebench: $(ISO)
	tools/probe-timebench.sh "$(ISO)" "$(GATE_LOGS)/probe-timebench-$$$$/timebench.log" $(PROBE_QEMU)

# v0.78: THE FRESH-IMAGE MATRIX, as a target anyone can run.
#
# CLAUDE.md has named uniprocessor / -smp 4 SeaBIOS / -smp 4 q35+VT-d as gate
# configurations since v0.76, but only the first had a target (release-verify).
# The other two were driven by a harness that lived outside the tree, which
# means the release matrix was reproducible by exactly one person. That is not
# a gate, it is a habit — so it is now tools/gate-matrix.sh and `make gate`.
#
#   make gate                                  all three fresh configurations
#   make gate GATE_CONFIGS="smp4-iommu"        just one
#   make gate-all                              fresh matrix + both dirty gates
#
# gate-matrix.sh prints an explicit coverage line naming what it did NOT test,
# because a gate whose gaps are invisible is how "verified" drifts away from
# "measured".
# v0.85: the O_APPEND contention SOAK -- the same test the gate runs, at the full
# 1024 appends instead of 128.
#
# It is a target rather than a note in a doc because APPSMP_SOAK has to reach
# BOTH halves: EXTRA compiles the kernel's expected total, UEXTRA compiles the
# ring-3 worker's loop count. Setting one and forgetting the other builds a
# kernel expecting 16384 bytes and a worker that writes 2048, which looks like a
# lost append and is not one. Nobody should have to remember that; this target
# is the memory.
#
# Roughly 220 s of guest time on top of a normal boot -- affordable deliberately,
# which is exactly why it is not in `make gate`.
.PHONY: appendsoak
appendsoak:
	$(MAKE) clean
	$(MAKE) EXTRA=-DAPPSMP_SOAK UEXTRA=-DAPPSMP_SOAK
	@echo "  [appendsoak] built with 1024 appends; boot it with 'make qemu' or tools/gate-matrix.sh"

# v0.89: the CAS contention soak at full size — 4 workers per online core and 96
# iterations each, against the gate's 2 and 12. Sets the flag on BOTH halves for
# the reason appendsoak does: CASC_ITERS reaches the kernel through EXTRA and
# user/init.c through UEXTRA, and a build with only one side raised makes the
# worker and the kernel disagree about how many iterations happened — a
# discrepancy that reads as a lost file rather than as a build mistake.
.PHONY: cascstress
cascstress:
	$(MAKE) clean
	$(MAKE) EXTRA=-DCASC_STRESS UEXTRA=-DCASC_STRESS
	@echo "  [cascstress] built at 4 workers/core x 96 iterations; boot with tools/gate-matrix.sh"
	@echo "  [cascstress] for replay-under-load, run tools/gate-dirty.sh against this image"

.PHONY: gate gate-all gate-selftest gate-oversub

# v0.81: the gate's own reporting logic, tested before it is trusted.
#
# gate-matrix.sh needs a boot to say anything, so its reporting went three
# releases untested and shipped a defect that labelled a truncated boot `FAIL`
# and named suites that had merely reported before the kill. This runs in well
# under a second on synthetic logs, and `gate` runs it FIRST — a harness that
# cannot classify a run correctly has nothing useful to say about a kernel.
gate-selftest:
	tools/gate-classify-test.sh
# v0.80: -smp 2 joins the matrix. Work stealing needs a thief, and at two
# cores the only other core is the BSP running the suite -- so v0.79's
# account of the [mcpre] failure predicts it cannot fail there. That
# prediction was left unrun when v0.79 shipped; this makes it part of every
# gate rather than a note in a roadmap.
GATE_CONFIGS ?= uniprocessor smp2-bios smp4-bios smp4-iommu
gate: $(ISO) gate-selftest
	tools/gate-matrix.sh "$(ISO)" "$(GATE_LOGS)/matrix-$$$$" $(GATE_CONFIGS)

# The whole standing gate in one command, in the order that fails cheapest
# first: a fresh-image break should not wait behind nine dirty boots.
gate-all: $(ISO)
	$(MAKE) gate
	$(MAKE) gate-dirty
	$(MAKE) gate-dirty-smp
	$(MAKE) gate-oversub

# v0.87: THE OVERSUBSCRIPTION TIER.
#
# v0.86 made APPSMP_OSRATIO a parameter and measured 4:1 exactly once, by hand.
# No tier used it, so the ratio was tunable but UNDEFENDED -- the same shape of
# gap that made the hardcoded 2 worth removing in the first place.
#
# This needs its own image, unlike every other tier. The others vary QEMU flags
# over one ISO; the ratio is compile-time, so a 4:1 run is a different kernel.
#
# THE TRAILING `make clean` IS LOAD-BEARING, not tidiness. Without it build/
# would be left holding a 4:1 kernel under the ordinary ISO name, and the next
# `make gate` would find that ISO up to date and boot it while reporting as
# though it were the default build. This repository has twice discovered a run
# that booted a DIFFERENT IMAGE than it claimed -- once invalidating a 20-boot
# verification. Ending on a clean tree makes that impossible rather than
# unlikely.
#
# smp4-bios because oversubscription needs more runnable workers than cores and
# 4 is what the gate host provides. GATE_OSR overrides the ratio.
GATE_OSR ?= 4
gate-oversub:
	$(MAKE) clean
	$(MAKE) EXTRA=-DAPPSMP_OSRATIO=$(GATE_OSR)
	tools/gate-matrix.sh "$(ISO)" "$(GATE_LOGS)/oversub-$$$$" smp4-bios
	@echo "  [gate-oversub] ran at $(GATE_OSR):1; cleaning so no later gate boots this image"
	$(MAKE) clean

# ===========================================================================
# v0.76: RELEASE PACKAGING — mandatory for every version tag
# ===========================================================================
# See "Release Protocol" in CLAUDE.md at the repo root. The short version: a
# release is not a tag, it is a tag plus an ISO that was built from a clean
# tree, checksummed, and BOOTED.
#
#   make iso              build the bootable image (alias for `all`)
#   make release-iso      clean rebuild + checksums + manifest
#   make release-verify   boot the release image and refuse it unless clean
#   make release          both, in the order that matters
#
# `all` already produces $(ISO); `iso` exists because "make iso" is what people
# type and reach for, and a release protocol that depends on remembering that
# the target is called something else is a protocol that gets skipped.
RELEASE_DIR := $(BUILD)/release

.PHONY: iso release-iso release-verify release release-version-check

iso: $(ISO)
	@echo "  ISO: $(ISO)"

# The version in this Makefile names the artefact. If it disagrees with the git
# tag, the artefact is misnamed and a "v0.76.0" release ships an
# outrun-os-0.75.0.iso — which has already happened here once: v0.75.0 was
# tagged while VERSION still read 0.74.0. Warn loudly rather than fail, because
# building between releases is normal and a hard error would train people to
# bypass the target.
#
# THREE outcomes, deliberately distinguished. A check that could not RUN must
# not print the same reassuring thing as a check that ran and passed — and the
# first draft of this did exactly that, reporting "no git tag found" when the
# truth was "git refused to answer". (Seen live: a git worktree created by
# Windows git stores a Windows-style path in its .git file, which Linux git
# cannot resolve, so every git command inside it fails. The tag was there the
# whole time.)
release-version-check:
	@if ! git rev-parse --git-dir >/dev/null 2>&1; then \
	  echo "  [release] *** CANNOT VERIFY VERSION: git did not answer here."; \
	  echo "  [release] *** This is NOT a pass. The artefact will be named"; \
	  echo "  [release] *** outrun-os-$(VERSION).iso with nothing cross-checking it."; \
	else \
	  tag=$$(git describe --tags --abbrev=0 2>/dev/null); \
	  if [ -z "$$tag" ]; then \
	    echo "  [release] git has no tags yet; artefact will be named v$(VERSION)"; \
	  elif [ "$$tag" != "v$(VERSION)" ]; then \
	    echo "  [release] *** WARNING: git tag $$tag != Makefile VERSION $(VERSION)"; \
	    echo "  [release] *** the ISO will be named outrun-os-$(VERSION).iso"; \
	    echo "  [release] *** bump VERSION in metal/Makefile before tagging a release"; \
	  else \
	    echo "  [release] version check OK: $$tag"; \
	  fi; \
	fi
# v0.81: the check now understands a dev cycle. Hardcoding "$(VERSION)-metal"
# would have warned on every build for the whole cycle, and a check that cries
# wolf all cycle is one people learn to scroll past -- which is exactly how the
# stale 0.73.0-metal banner survived four releases and made this check
# necessary in the first place.
	@kv=$$(sed -n 's/^#define KERNEL_VERSION "\(.*\)"$$/\1/p' kernel/kernel64.c); \
	case "$(VERSION)" in \
	  *-dev) want="$(VERSION)" ;;      # dev cycle: banner IS the version \
	  *)     want="$(VERSION)-metal" ;; \
	esac; \
	if [ "$$kv" != "$$want" ]; then \
	  echo "  [release] *** WARNING: KERNEL_VERSION is \"$$kv\", expected \"$$want\""; \
	  echo "  [release] *** the kernel will PRINT $$kv at boot, tell every ring-3"; \
	  echo "  [release] *** process OUTRUN=$$kv, and answer 'ver' with it, no matter"; \
	  echo "  [release] *** what the ISO is called. Fix kernel64.c before tagging."; \
	else \
	  echo "  [release] kernel banner check OK: $$kv"; \
	fi
# v0.93: the GRUB menu entry is the THIRD place a version is written down, and
# it is the one a user reads FIRST -- it is on screen before the kernel has
# printed a single line. Nothing checked it, so it sat at "0.30.0-metal" for
# roughly sixty releases and was noticed only when a boot log was read for an
# unrelated reason. A literal nothing verifies is a literal that drifts.
#
# `want` is recomputed here rather than carried over: every recipe line runs in
# its own shell, so the variable set by the check above does not survive to this
# one. Reusing it would silently compare against an empty string and pass.
	@want="$(VERSION)"; case "$(VERSION)" in *-dev) ;; *) want="$(VERSION)-metal" ;; esac; \
	gv=$$(sed -n 's/^menuentry "Outrun OS (kernel \(.*\))".*/\1/p' iso/boot/grub/grub.cfg | tr -d '\r'); \
	if [ -z "$$gv" ]; then \
	  echo "  [release] *** WARNING: could not read a version out of grub.cfg --"; \
	  echo "  [release] *** the menuentry line is not in the shape this expects."; \
	elif [ "$$gv" != "$$want" ]; then \
	  echo "  [release] *** WARNING: grub.cfg menu says \"$$gv\", expected \"$$want\""; \
	  echo "  [release] *** that is the FIRST version string a user sees at boot."; \
	else \
	  echo "  [release] grub menu check OK: $$gv"; \
	fi

# A release image is built from a CLEAN tree. An incremental build can carry an
# object from a source state that no longer exists anywhere, and that is not
# something you want to discover from a published artefact.
release-iso: release-version-check
	$(MAKE) clean
	$(MAKE) all
	$(MAKE) desktop
	@mkdir -p $(RELEASE_DIR)
	cp $(ISO) $(DESKTOP_ISO) $(RELEASE_DIR)/
	cd $(RELEASE_DIR) && md5sum outrun-os-$(VERSION).iso    > outrun-os-$(VERSION).iso.md5
	cd $(RELEASE_DIR) && sha256sum outrun-os-$(VERSION).iso > outrun-os-$(VERSION).iso.sha256
	cd $(RELEASE_DIR) && md5sum outrun-desktop-$(VERSION).iso    > outrun-desktop-$(VERSION).iso.md5
	cd $(RELEASE_DIR) && sha256sum outrun-desktop-$(VERSION).iso > outrun-desktop-$(VERSION).iso.sha256
	@echo "-------------------------------------------------------------"
	@echo " RELEASE ARTEFACTS"
	@echo "   $(RELEASE_DIR)/outrun-os-$(VERSION).iso"
	@cat $(RELEASE_DIR)/outrun-os-$(VERSION).iso.md5
	@cat $(RELEASE_DIR)/outrun-os-$(VERSION).iso.sha256
	@echo "   $(RELEASE_DIR)/outrun-desktop-$(VERSION).iso"
	@cat $(RELEASE_DIR)/outrun-desktop-$(VERSION).iso.md5
	@cat $(RELEASE_DIR)/outrun-desktop-$(VERSION).iso.sha256
	@echo "-------------------------------------------------------------"
	@echo " NOT YET VERIFIED — run 'make release-verify'"

# v0.80: logs go under $(GATE_LOGS), not $(BUILD). Phase 1 moved the gate
# harnesses out of build/ so `make clean` could not destroy evidence, and left
# this one behind -- which promptly cost the v0.80.0 release-verify log, and
# then the release ARTEFACT itself, to a routine clean build. The
# published-artefact check is the last log that should be disposable.
#
# v1.0: the desktop artefact is verified too, by the same acceptance test that
# gates it in development, run against the COPY in release/ rather than the
# one in build/. Both files must boot; one file having booted says nothing
# about the other.
release-verify:
	tools/release-verify.sh "$(RELEASE_DIR)/outrun-os-$(VERSION).iso" "" "$(GATE_LOGS)/release-verify"
	python3 tools/desktop-ui-test.py "$(RELEASE_DIR)/outrun-desktop-$(VERSION).iso" \
	  "$(GATE_LOGS)/release-verify/desktop-$(VERSION).log"

release: release-iso release-verify
	@echo "  [release] ISO built, checksummed and booted clean."

# v0.80: .logs/ is NOT removed by `clean` -- that is the entire point of it
# (see GATE_LOGS above; make clean has destroyed harness evidence twice).
# clean-logs is the explicit way to purge it, so discarding evidence is always
# something someone typed, never a side effect of building.
.PHONY: clean-logs gate-summary
# v0.80: THE KDF PRIMITIVE IS VALIDATED BEFORE IT IS WIRED IN.
#
# crypto/scrypt.c is compiled and run HERE, on the host, against RFC 7914's
# published vectors — no kernel, no boot, no scheduler, seconds not minutes.
# The rule it serves was adopted after this project paid for its absence: a
# primitive exercised only through the login path is a primitive whose
# failures look like authentication bugs.
#
# It is a HOST build on purpose. The same source compiles freestanding into
# the kernel (it allocates nothing and calls no libc); what differs is who
# supplies scrypt_pbkdf2_sha256 — the test supplies a reference, the kernel
# will supply its own already-verified HMAC.
.PHONY: kdf-test
kdf-test: | $(BUILD)
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/kdf_vector_test tools/kdf_vector_test.c crypto/scrypt.c
	@echo ""
	$(BUILD)/kdf_vector_test

clean-logs:
	rm -rf $(GATE_LOGS)
	@echo "  purged $(GATE_LOGS) -- gate evidence; make clean never touches it"

# v0.80: report on THE LAST RUN, not on a glob.
#
# Reading .logs/gate/matrix-*/ spans every run still on disk, and that has
# twice produced a wrong answer here: a failure tally that counted a
# deliberate reproducer directory, and a v0.79 release check that read the
# boot banner from a pre-bump run and reported 0.73.0-metal. Both were caught
# by noticing, which is not a property to depend on. gate-matrix.sh records
# its directory in LAST_RUN; this reads that and nothing else.
gate-summary:
	@d=$$(cat $(GATE_LOGS)/LAST_RUN 2>/dev/null); \
	if [ -z "$$d" ] || [ ! -d "$$d" ]; then \
	  echo "  no recorded gate run (looked in $(GATE_LOGS)/LAST_RUN)"; exit 1; fi; \
	echo "  run dir : $$d"; \
	echo "  image   : $$(grep -aho 'md5=[a-f0-9]*' $$d/*.log | sort -u)"; \
	echo "  banner  : $$(grep -aho 'bare-metal kernel [0-9.]*-metal' $$d/*.log | sort -u)"; \
	for f in $$d/*.log; do \
	  p=$$(grep -ao 'RESULT: [0-9]* passed, [0-9]* failed' $$f | awk '{p+=$$2; f+=$$4} END {printf "%d passed / %d failed", p, f}'); \
	  r=$$(grep -acE 'rank violations=[1-9]|underflow=[1-9]|mismatch=[1-9]' $$f); \
	  printf "  %-14s %-28s rank-faults %s\n" "$$(basename $$f .log)" "$$p" "$$r"; \
	done

clean:
	rm -rf $(BUILD) iso/boot/outrun-kernel.elf iso/boot/user_init.elf

# dd-able, UEFI-bootable GPT install image (install onto raw NVMe via dd)
install-img: all
	./mkinstall.sh

qemu-nvme: install-img
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -m 512M -nographic -no-reboot $(QEMU_GPU) \
	  -drive file=build/outrun-install.img,format=raw,if=none,id=nvm -device nvme,drive=nvm,serial=OUTRUN01 \
	  $(QEMU_BLK) $(QEMU_NET)

# Boot on q35 with an emulated Intel VT-d IOMMU; virtio devices behind it.
# (virtio-sound omitted here for the same reason as the main qemu target — it
# breaks virtio-gpu's controlq under this QEMU; see qemu-audio + CHANGELOG.)
# v0.75: this is the THIRD release-gate configuration, and two things stopped it
# from being that.
#
#   intremap=off  Interrupt remapping was disabled, so the one part of VT-d that
#                 interacts with the APIC/MSI path this kernel actually drives
#                 was never exercised. DMA translation was, which is why the
#                 target still found real bugs — but "IOMMU" was doing half the
#                 job the config name implies. Now `on`, which is also why
#                 kernel-irqchip=split is required rather than incidental.
#
#   no -smp       The target passed no -smp at all, so it booted UNIPROCESSOR
#                 while every log it produced is named "smp4-iommu". The gate's
#                 third config is meant to be the SMP one under VT-d; without
#                 -smp 4 it duplicates the uniprocessor config with an IOMMU
#                 bolted on and cannot catch anything SMP-specific.
#
# Also: this target used to point -drive at a hardcoded /tmp/cas.img with no
# rule to create it, unlike every other qemu target here, so it failed outright
# on a clean machine and silently reused whatever a previous run had left behind
# on a dirty one. It now uses $(QEMU_BLK)/$(QEMU_NET) with iommu_platform=on
# appended, and depends on $(VBLK_IMG) like the rest.
#
# Verified as written: 5/5 clean boots, 47 suites, 0 failures, 0 lock-rank
# faults. Note 47 rather than the 45 of the other configs — the VT-d
# configuration runs two additional IOMMU-specific suites, and their absence is
# how you can tell this target is not actually running with an IOMMU.
QEMU_IOMMU := -smp 4 -machine q35,kernel-irqchip=split \
              -device intel-iommu,intremap=on,caching-mode=on
QEMU_BLK_IOMMU := -drive file=$(VBLK_IMG),if=none,format=raw,id=vd0 \
  -device virtio-blk-pci,drive=vd0,disable-legacy=on,disable-modern=off,iommu_platform=on
QEMU_NET_IOMMU := -netdev user,id=n0 \
  -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef,iommu_platform=on
qemu-iommu: $(ISO) $(VBLK_IMG)
	qemu-system-x86_64 $(QEMU_IOMMU) -cdrom $(ISO) -m 512M -nographic -no-reboot $(QEMU_GPU) \
	  $(QEMU_BLK_IOMMU) $(QEMU_NET_IOMMU)
