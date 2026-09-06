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
#   smp8-bios      -smp 8 on SeaBIOS (guest cores > host cores; v0.88)
#   smp4-iommu     -smp 4 on q35 behind an emulated Intel VT-d unit, intremap=on
#
# Each boots a FRESH disk image, and is accepted only if it reaches the shell
# prompt with zero failing assertions and no lock-rank fault.
#
# Exits non-zero if any configuration fails, so `make gate` fails rather than
# printing red text and succeeding.
set -u

# ===========================================================================
# v0.81: RUN CLASSIFICATION, WITH DELIBERATE PRECEDENCE
# ===========================================================================
# These checks used to be six sequential assignments to $st, so whichever fired
# LAST won. A boot killed at the cap before it ever reached the shell prompt was
# therefore labelled `FAIL`, carrying whatever suites had managed to report
# before the kill — a timeout wearing the costume of a suite failure.
#
# That is not hypothetical. An smp4-iommu run cut off mid-`cas` at the 900 s cap
# was reported as `FAIL ... failed=2`, naming [vfiostrs] and [capdma]. It had
# never reached the prompt, so it was not a verdict on anything; a reader who
# trusted the label would have gone looking for a vfiostrs defect that the run
# had produced no evidence for. `st=FAIL` had simply overwritten `st=NO-PROMPT`
# two lines later.
#
# So COMPLETENESS is now decided before CORRECTNESS. A run that did not finish
# is not a verdict on the build — it is an invalid run, and the label says so.
# TRUNCATED and NO-PROMPT are distinguished because they are different events:
# the first means we stopped the guest, the second means the guest stopped.
#
# Kept as a function so it can be exercised without booting anything:
#   GATE_LIB_ONLY=1 . tools/gate-matrix.sh
# defines these and runs nothing. See tools/gate-classify-test.sh, which pins
# the truncation case above as a regression test.
classify_run() {
    _log=$1; _suites=$2; _fails=$3; _tally=$4; _ranks=$5; _worst=$6; _elapsed=$7; _cap=$8
    if   grep -aq 'system halted' "$_log" 2>/dev/null;                then echo HALTED
    elif ! grep -aq "Type 'help' for commands" "$_log" 2>/dev/null;   then
        # We stopped it, or it stopped itself? Both are invalid; only one is ours.
        if [ "$_elapsed" -ge "$_cap" ]; then echo TRUNCATED; else echo NO-PROMPT; fi
    elif [ "$_suites" -eq 0 ];                                        then echo NO-SUITES
    elif [ "$_fails" -ne "$_tally" ];                                 then echo COUNTER-SPLIT
    elif [ "$_ranks" -ne 0 ];                                         then echo RANK-FAULT
    elif [ "$_worst" -ne 0 ];                                         then echo FAIL
    else                                                                   echo OK
    fi
}

# Only these three statuses mean "the boot ran to completion and we can read its
# assertions as a verdict". Everything else is an invalid run, whatever its
# assertion counts happen to say.
run_is_verdict() { case "$1" in OK|FAIL|RANK-FAULT) return 0 ;; *) return 1 ;; esac; }

if [ -n "${GATE_LIB_ONLY:-}" ]; then return 0 2>/dev/null || exit 0; fi

ISO=${1:?usage: gate-matrix.sh <iso> [workdir] [config ...]}
WORK=${2:-.logs/gate/matrix}
shift 2 2>/dev/null || shift 1
CONFIGS=${*:-"uniprocessor smp4-bios smp4-iommu"}
CAP=${GATE_CAP:-900}

[ -r "$ISO" ] || { echo "gate-matrix: cannot read ISO '$ISO'"; exit 2; }
command -v qemu-system-x86_64 >/dev/null || { echo "gate-matrix: qemu-system-x86_64 not found"; exit 2; }

# A unique workdir per invocation, and a lock. Two concurrent runs sharing one
# output directory once deleted each other's logs mid-run here, and the merged
# log reported a PASS that belonged to neither run. See tools/gate-dirty.sh.
#
# v0.78: and it lives under .logs/, NOT under build/. `make clean` has now
# destroyed harness evidence twice — a carryover-3 reproduction that had to be
# re-run, and the only copy of an unexplained [mcpre] failure, which is simply
# gone. Logs that a routine build step can delete are not records.
rm -rf "$WORK"; mkdir -p "$WORK"
exec 9>"$WORK/.lock"
flock -n 9 || { echo "gate-matrix: another run holds $WORK"; exit 2; }

MD5=$(md5sum "$ISO" | cut -d' ' -f1)
GPU="-vga none -device virtio-vga"

# v0.80: NAME THIS RUN, AND PRUNE THE REST.
#
# .logs/ was introduced in v0.78 so `make clean` could not destroy evidence. It
# worked, and it created a second hazard: runs accumulate, and any command that
# reads `.logs/gate/matrix-*/…` is reading several runs at once. That has now
# produced two wrong readings — a "19 PASS / 1 FAIL" tally whose single FAIL came
# from a deliberate reproducer directory, and a v0.79 release check that reported
# the boot banner as 0.73.0-metal by picking up a pre-bump run.
#
# Both were caught, but "caught by noticing" is not a property to rely on. So the
# run records WHERE it is, and anything reporting on a gate reads that pointer
# instead of a glob. See `make gate-summary`.
LAST=$(dirname "$WORK")/LAST_RUN
printf '%s\n' "$WORK" > "$LAST" 2>/dev/null || true

# Keep the last few runs and drop older ones, so the directory cannot grow into
# the same trap by another route. Reproducer dirs (any name not matching the
# run prefix) are deliberately left alone — they are named evidence, not runs.
PRUNE_KEEP=${GATE_KEEP:-3}
_base=$(basename "$WORK" | sed 's/-[0-9]*$//')
_dir=$(dirname "$WORK")
ls -dt "$_dir/$_base"-* 2>/dev/null | tail -n +$((PRUNE_KEEP + 1)) | while read -r old; do
    [ "$old" = "$WORK" ] && continue
    rm -rf "$old"
done

echo "=============================================================="
echo " FRESH-IMAGE RELEASE MATRIX"
echo "   iso     : $ISO"
echo "   md5     : $MD5"
echo "   configs : $CONFIGS"
echo "   run dir : $WORK"
echo "             (recorded in $(dirname "$WORK")/LAST_RUN; read it with 'make gate-summary',"
echo "              never with a glob over matrix-* — that has misreported twice)"
echo "   workdir : $WORK"
echo "=============================================================="

rc=0
ran=""
invalid=""
for CFG in $CONFIGS; do
    LOG=$WORK/$CFG.log
    IMG=$WORK/$CFG.img
    dd if=/dev/zero of="$IMG" bs=1M count=4 2>/dev/null
    printf 'OUTRUN-DISK-SIGNATURE-OK' | dd of="$IMG" bs=1 seek=1024 conv=notrunc 2>/dev/null

    BLK="-drive file=$IMG,if=none,format=raw,id=vd0 -device virtio-blk-pci,drive=vd0,disable-legacy=on,disable-modern=off"
    # v0.95: THE UNBOUND DEVICE, for ring-3 passthrough (role 66). Empty for
    # every configuration that does not set it below.
    UNBOUND=""
    NET="-netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef"
    case "$CFG" in
      uniprocessor) SMP="" ;;
      # v0.80: two cores. Work stealing needs a thief, and at -smp 2 the only
      # other core is the BSP -- which is running the suite, not stealing. The
      # v0.79 account of the [mcpre] failure predicts it therefore cannot fail
      # here, which is a falsifiable claim this configuration exists to test.
      # It is also the smallest topology in which any cross-core path runs at
      # all, and several SMP suites have only ever been exercised at four.
      smp2-bios)    SMP="-smp 2" ;;
      smp4-bios)    SMP="-smp 4" ;;
      # v0.88: EIGHT vCPUs, and the reason is specific. The oversubscription
      # worker cap only ever bit when the guest reported MORE THAN APPSMP_W (4)
      # online cores -- at exactly 4 the clamp is a no-op, which is why the 4:1
      # tier passed on the reference host while the ceiling it depended on was
      # still there. Reproducing that needed a guest with more cores than the
      # host has, which TCG will happily emulate: correctness of the ratio
      # arithmetic does not depend on those vCPUs running in parallel.
      #
      # Slower than smp4 for the obvious reason -- eight emulated cores
      # time-slicing on however many the host really has -- so this is not in
      # GATE_CONFIGS by default. It is the configuration that proves the cap is
      # gone, and it is named here so proving it again is one argument rather
      # than a rediscovery.
      smp8-bios)    SMP="-smp 8" ;;
      smp4-iommu)
        SMP="-smp 4 -machine q35,kernel-irqchip=split -device intel-iommu,intremap=on,caching-mode=on"
        # The virtio devices must sit BEHIND the IOMMU, or the configuration is
        # q35 with an unused unit bolted on. iommu_platform=on is what puts them
        # there; without it this target silently tests nothing extra.
        BLK="$BLK,iommu_platform=on"
        NET="$NET,iommu_platform=on"
        # v0.95: AN UNBOUND PCI DEVICE, so role 66 has something to claim.
        #
        # Every other function on this machine has a kernel driver bound to it,
        # so the claim path was only ever exercised NEGATIVELY -- role 66
        # reported NOT EXERCISED on every boot. This device closes that.
        #
        # virtio-rng-pci SPECIFICALLY, and the choice is not arbitrary. It is
        # vendor 1af4 but PCI class 0x00 (unclassified), and pci_probe_fn binds
        # drivers to 1af4 classes 01/02/03/04 only -- so nothing in this kernel
        # claims it and it stays free for ring 3. A second virtio-net would NOT
        # work: virtionet_probe binds any 1af4 class 0x02 function, so the
        # duplicate would be host-bound too and refused exactly like the first.
        # e1000 would not work either -- its BAR0 is 128 KiB and SYS_MAP_PCI_BAR
        # refuses a BAR larger than its 64 KiB window rather than truncating it.
        #
        # iommu_platform=on puts it BEHIND the IOMMU, which is the whole point:
        # without it a CLAIM_DMA would attach a domain to a device that is not
        # translated, and the confinement would be decorative.
        #
        # disable-legacy=on,disable-modern=off IS REQUIRED, not decoration.
        # virtio-rng-pci defaults to a TRANSITIONAL device and
        # iommu_platform=on is a modern-only feature, so without these qemu
        # refuses the command line outright:
        #   "VIRTIO_F_IOMMU_PLATFORM was supported by neither legacy nor
        #    transitional device"
        # and the guest never starts at all. Same flags BLK and NET already
        # carry, for the same reason.
        UNBOUND="-device virtio-rng-pci,disable-legacy=on,disable-modern=off,iommu_platform=on" ;;
      *) echo "gate-matrix: unknown configuration '$CFG'"; rc=1; continue ;;
    esac

    # Every log names the binary it came from. A log that cannot do that is not
    # evidence — this project has twice discovered a run had booted a different
    # image than the one it claimed to be testing.
    echo "# config=$CFG iso=$ISO md5=$MD5 qemu='${SMP:-uniprocessor}'" > "$LOG"

    # shellcheck disable=SC2086
    qemu-system-x86_64 $SMP -cdrom "$ISO" -m 512M -nographic -no-reboot $GPU $BLK $NET $UNBOUND \
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

    st=$(classify_run "$LOG" "$suites" "$fails" "$tally" "$ranks" "$worst" "$e" "$CAP")
    [ "$st" = OK ] || rc=1
    [ "$st" = COUNTER-SPLIT ] && {
        echo "  !! $CFG: failure counters DISAGREE (lines=$fails tally=$tally)"
        echo "  !! that is a defect in this harness, not a verdict on the boot"; }

    printf '%-13s %-13s suites=%-3s passed=%-4s failed=%-3s ranks=%-2s (%ss)\n' \
        "$CFG" "$st" "$suites" "$passed" "$worst" "$ranks" "$e"
    # An incomplete boot's assertions are still printed — they may be the only
    # clue why it died — but never bare, because a bare list of FAIL lines reads
    # as a verdict no matter what the status column says.
    run_is_verdict "$st" || {
        echo "      ^^ INVALID RUN ($st) — NOT a verdict on the build."
        case "$st" in
        TRUNCATED)
            echo "         We killed it at GATE_CAP=${CAP}s before it reached the prompt."
            echo "         Any assertions below are only those that reported before the"
            echo "         kill. Raise GATE_CAP and re-run; do not read them as failures." ;;
        NO-PROMPT)
            echo "         The guest stopped on its own before reaching the prompt. Any"
            echo "         assertions below are only those that reported before it died." ;;
        NO-SUITES)
            echo "         It reached the prompt, but no suite emitted a RESULT line —"
            echo "         so this boot tested nothing, however green it looks." ;;
        COUNTER-SPLIT)
            echo "         The two failure counters disagree, so neither can be trusted."
            echo "         That is a harness defect; fix it before reading this boot." ;;
        esac; }
    [ "$worst" -eq 0 ] || grep -aoE "$FAILRE" "$LOG" | sed 's/^/      /'
    run_is_verdict "$st" || invalid="$invalid $CFG($st)"
    ran="$ran $CFG"
done

# STATE THE COVERAGE. A gate whose gaps are invisible is how "verified" drifts
# away from "measured", so this line is part of the output and not a footnote.
echo
echo "---- coverage ----"
echo "ran            :$ran (1 boot each, fresh image per boot)"
# An invalid run tested NOTHING, so it belongs in the coverage line and not only
# in the status column. A gate that says "ran: smp4-iommu" about a boot that was
# killed before the prompt has overstated its own coverage.
[ -z "$invalid" ] || \
echo "DID NOT COMPLETE:$invalid — these configurations are UNTESTED by this run,"
[ -z "$invalid" ] || \
echo "                 whatever assertion counts appear above."
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
