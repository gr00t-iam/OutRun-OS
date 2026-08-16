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
# v0.81 DEV CYCLE. The suffix is deliberate and it propagates: the ISO is named
# outrun-os-0.81.0-dev.iso, so a development image cannot be mistaken for a
# release artefact by its filename alone. At release time this becomes plain
# 0.81.0 and KERNEL_VERSION becomes 0.81.0-metal.
VERSION := 0.81.0
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

$(BUILD)/kernel64.o: kernel/kernel64.c $(BUILD)/sdk_sources.h crypto/scrypt.h | $(BUILD)
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

qemu-vga: $(ISO) $(VBLK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -serial stdio -no-reboot $(QEMU_GPU) $(QEMU_BLK) $(QEMU_NET)

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
.PHONY: gate gate-all gate-selftest

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

# A release image is built from a CLEAN tree. An incremental build can carry an
# object from a source state that no longer exists anywhere, and that is not
# something you want to discover from a published artefact.
release-iso: release-version-check
	$(MAKE) clean
	$(MAKE) all
	@mkdir -p $(RELEASE_DIR)
	cp $(ISO) $(RELEASE_DIR)/
	cd $(RELEASE_DIR) && md5sum outrun-os-$(VERSION).iso    > outrun-os-$(VERSION).iso.md5
	cd $(RELEASE_DIR) && sha256sum outrun-os-$(VERSION).iso > outrun-os-$(VERSION).iso.sha256
	@echo "-------------------------------------------------------------"
	@echo " RELEASE ARTEFACT"
	@echo "   $(RELEASE_DIR)/outrun-os-$(VERSION).iso"
	@cat $(RELEASE_DIR)/outrun-os-$(VERSION).iso.md5
	@cat $(RELEASE_DIR)/outrun-os-$(VERSION).iso.sha256
	@echo "-------------------------------------------------------------"
	@echo " NOT YET VERIFIED — run 'make release-verify'"

# v0.80: logs go under $(GATE_LOGS), not $(BUILD). Phase 1 moved the gate
# harnesses out of build/ so `make clean` could not destroy evidence, and left
# this one behind -- which promptly cost the v0.80.0 release-verify log, and
# then the release ARTEFACT itself, to a routine clean build. The
# published-artefact check is the last log that should be disposable.
release-verify:
	tools/release-verify.sh "$(RELEASE_DIR)/outrun-os-$(VERSION).iso" "" "$(GATE_LOGS)/release-verify"

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
