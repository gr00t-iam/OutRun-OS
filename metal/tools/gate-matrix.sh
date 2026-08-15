#!/bin/bash
# gate-matrix.sh <iso> [workdir] [config ...]
#
# THE FRESH-IMAGE RELEASE MATRIX, as a first-class script.
#
# Until v0.78 this matrix existed only as ad-hoc commands typed by whoever was
# cutting the release: `make release-verify` covered uniprocessor, and the two
# -smp 4 configurations were driven by a harness that lived outside the tree.
# A release gate that only one person can run is not a gate, and CLAUDE.md names
# these configurations as required — so they are now in the repository, run the
# same way by anyone, and reported the same way every time.
#
# Configurations (default: all three):
#   uniprocessor   plain, one vCPU
#   smp4-bios      -smp 4 on SeaBIOS
#   smp4-iommu     -smp 4 on q35 behind an emulated Intel VT-d unit, intremap=on
#
# Each boots a FRESH disk image, and is accepted only if it reaches the shell
# prompt with zero failing assertions and no lock-rank fault.
#
# Exits non-zero if any configuration fails, so `make gate` fails rather than
# printing red text and succeeding.
set -u

ISO=${1:?usage: gate-matrix.sh <iso> [workdir] [config ...]}
WORK=${2:-build/gate-matrix}
shift 2 2>/dev/null || shift 1
CONFIGS=${*:-"uniprocessor smp4-bios smp4-iommu"}
CAP=${GATE_CAP:-900}

[ -r "$ISO" ] || { echo "gate-matrix: cannot read ISO '$ISO'"; exit 2; }
command -v qemu-system-x86_64 >/dev/null || { echo "gate-matrix: qemu-system-x86_64 not found"; exit 2; }

# A unique workdir per invocation, and a lock. Two concurrent runs sharing one
# output directory once deleted each other's logs mid-run here, and the merged
# log reported a PASS that belonged to neither run. See tools/gate-dirty.sh.
rm -rf "$WORK"; mkdir -p "$WORK"
exec 9>"$WORK/.lock"
flock -n 9 || { echo "gate-matrix: another run holds $WORK"; exit 2; }

MD5=$(md5sum "$ISO" | cut -d' ' -f1)
GPU="-vga none -device virtio-vga"

echo "=============================================================="
echo " FRESH-IMAGE RELEASE MATRIX"
echo "   iso     : $ISO"
echo "   md5     : $MD5"
echo "   configs : $CONFIGS"
echo "   workdir : $WORK"
echo "=============================================================="

rc=0
ran=""
for CFG in $CONFIGS; do
    LOG=$WORK/$CFG.log
    IMG=$WORK/$CFG.img
    dd if=/dev/zero of="$IMG" bs=1M count=4 2>/dev/null
    printf 'OUTRUN-DISK-SIGNATURE-OK' | dd of="$IMG" bs=1 seek=1024 conv=notrunc 2>/dev/null

    BLK="-drive file=$IMG,if=none,format=raw,id=vd0 -device virtio-blk-pci,drive=vd0,disable-legacy=on,disable-modern=off"
    NET="-netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef"
    case "$CFG" in
      uniprocessor) SMP="" ;;
      smp4-bios)    SMP="-smp 4" ;;
      smp4-iommu)
        SMP="-smp 4 -machine q35,kernel-irqchip=split -device intel-iommu,intremap=on,caching-mode=on"
        # The virtio devices must sit BEHIND the IOMMU, or the configuration is
        # q35 with an unused unit bolted on. iommu_platform=on is what puts them
        # there; without it this target silently tests nothing extra.
        BLK="$BLK,iommu_platform=on"
        NET="$NET,iommu_platform=on" ;;
      *) echo "gate-matrix: unknown configuration '$CFG'"; rc=1; continue ;;
    esac

    # Every log names the binary it came from. A log that cannot do that is not
    # evidence — this project has twice discovered a run had booted a different
    # image than the one it claimed to be testing.
    echo "# config=$CFG iso=$ISO md5=$MD5 qemu='${SMP:-uniprocessor}'" > "$LOG"

    # shellcheck disable=SC2086
    qemu-system-x86_64 $SMP -cdrom "$ISO" -m 512M -nographic -no-reboot $GPU $BLK $NET \
      < /dev/null >> "$LOG" 2>&1 &
    q=$!; e=0
    while [ "$e" -lt "$CAP" ]; do
        kill -0 $q 2>/dev/null || break
        grep -aq "Type 'help' for commands" "$LOG" 2>/dev/null && { sleep 5; break; }
        grep -aq 'system halted' "$LOG" 2>/dev/null && { sleep 3; break; }
        sleep 5; e=$((e+5))
    done
    kill -9 $q 2>/dev/null; wait $q 2>/dev/null
    rm -f "$IMG"

    # TWO INDEPENDENT FAILURE COUNTERS, and disagreement fails the gate on its
    # own. The single regex used before v0.76 could not see `[pthreads_smp] FAIL:`
    # — an underscore in the tag and a colon after FAIL defeated it — and a gate
    # certified a boot whose own RESULT line said "1 failed".
    FAILRE='^\[[a-zA-Z0-9_. -]+\][ ]*FAIL[: ].*'
    suites=$(grep -ac 'RESULT:' "$LOG")
    fails=$(grep -aoE "$FAILRE" "$LOG" | wc -l | tr -d ' ')
    tally=$(grep -ao 'RESULT: [0-9]* passed, [0-9]* failed' "$LOG" | awk '{f+=$4} END {print f+0}')
    passed=$(grep -ao 'RESULT: [0-9]* passed, [0-9]* failed' "$LOG" | awk '{p+=$2} END {print p+0}')
    ranks=$(grep -acE 'rank violations=[1-9]|underflow=[1-9]|mismatch=[1-9]' "$LOG")
    worst=$fails; [ "$tally" -gt "$worst" ] && worst=$tally

    st=OK
    grep -aq "Type 'help' for commands" "$LOG" || { st=NO-PROMPT; rc=1; }
    grep -aq 'system halted' "$LOG" && { st=HALTED; rc=1; }
    [ "$suites" -gt 0 ] || { st=NO-SUITES; rc=1; }
    [ "$fails" -eq "$tally" ] || {
        echo "  !! $CFG: failure counters DISAGREE (lines=$fails tally=$tally)"
        echo "  !! that is a defect in this harness, not a verdict on the boot"
        st=COUNTER-SPLIT; rc=1; }
    [ "$worst" -eq 0 ] || { st=FAIL; rc=1; }
    [ "$ranks" -eq 0 ] || { st=RANK-FAULT; rc=1; }

    printf '%-13s %-13s suites=%-3s passed=%-4s failed=%-3s ranks=%-2s (%ss)\n' \
        "$CFG" "$st" "$suites" "$passed" "$worst" "$ranks" "$e"
    [ "$worst" -eq 0 ] || grep -aoE "$FAILRE" "$LOG" | sed 's/^/      /'
    ran="$ran $CFG"
done

# STATE THE COVERAGE. A gate whose gaps are invisible is how "verified" drifts
# away from "measured", so this line is part of the output and not a footnote.
echo
echo "---- coverage ----"
echo "ran            :$ran (1 boot each, fresh image per boot)"
echo "NOT covered    : dirty-volume reuse (make gate-dirty / gate-dirty-smp),"
echo "                 bare metal and Proxmox, soak/repeat runs, and any"
echo "                 intermittent whose rate is below ~1 in 1 boot per config."
echo "image          : $MD5"
echo
if [ "$rc" -eq 0 ]; then
    echo "FRESH-IMAGE MATRIX: PASS"
else
    echo "FRESH-IMAGE MATRIX: FAIL — see $WORK/*.log"
fi
exit $rc
