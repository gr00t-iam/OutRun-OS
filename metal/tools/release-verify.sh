#!/bin/bash
# release-verify.sh <iso> [extra-qemu-args] [workdir]
#
# Boot a candidate release ISO and refuse it unless it comes up clean.
#
# "It built" is not the same claim as "it boots", and a release artefact that
# has only been compiled has been checked for neither. This boots the exact
# image that will be published, from a fresh volume, and asserts:
#
#   - it reaches the shell prompt
#   - every suite that reported did so with zero failures
#   - no lock-rank violation, underflow or mismatch appeared
#
# Exits non-zero otherwise, so `make release-verify` fails rather than printing
# red text and succeeding.
set -u

ISO=${1:?usage: release-verify.sh <iso> [extra-qemu-args] [workdir]}
EXTRA=${2:-}
WORK=${3:-build/release-verify}
CAP=${RELEASE_VERIFY_CAP:-600}

[ -r "$ISO" ] || { echo "release-verify: cannot read ISO '$ISO'"; exit 2; }
command -v qemu-system-x86_64 >/dev/null || { echo "release-verify: qemu-system-x86_64 not found"; exit 2; }

rm -rf "$WORK"; mkdir -p "$WORK"
IMG=$WORK/disk.img
LOG=$WORK/boot.log
MD5=$(md5sum "$ISO" | cut -d' ' -f1)

dd if=/dev/zero of="$IMG" bs=1M count=4 2>/dev/null
printf 'OUTRUN-DISK-SIGNATURE-OK' | dd of="$IMG" bs=1 seek=1024 conv=notrunc 2>/dev/null

echo "=============================================================="
echo " RELEASE ISO VERIFICATION"
echo "   iso  : $ISO"
echo "   md5  : $MD5"
echo "   qemu : ${EXTRA:-<uniprocessor>}"
echo "=============================================================="
echo "# iso=$ISO md5=$MD5 qemu='${EXTRA:-uniprocessor}'" > "$LOG"

# shellcheck disable=SC2086
qemu-system-x86_64 $EXTRA -cdrom "$ISO" -m 512M -nographic -no-reboot \
  -vga none -device virtio-vga \
  -drive file=$IMG,if=none,format=raw,id=vd0 \
  -device virtio-blk-pci,drive=vd0,disable-legacy=on,disable-modern=off \
  -netdev user,id=n0 \
  -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef \
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

rc=0
suites=$(grep -ac 'RESULT:' "$LOG")

# TWO INDEPENDENT FAILURE COUNTERS, because one of them was blind.
#
# This read '^\[[a-z0-9 ]+\][ ]+FAIL[ ]+' — a lowercase-only tag, and a space
# required after FAIL. A real failing assertion escaped it on BOTH counts: the
# suite tag `pthreads_smp` carries an underscore, and the line reads `FAIL:`
# with a colon. The gate printed PASS on a boot whose own RESULT line said
# `5 passed, 1 failed`. A counter that cannot see a failure is worse than no
# counter, because it is believed — and this one had been believed by every
# release gate that used it.
#
# The fix is not only a wider pattern. The pattern is now cross-checked against
# a counter derived from a different line entirely (the suites' own RESULT
# tallies), and DISAGREEMENT ITSELF FAILS THE GATE. A single counter can go
# blind again the next time a suite invents a tag; two counters derived from
# different lines cannot go blind in the same direction quietly.
FAILRE='^\[[a-zA-Z0-9_. -]+\][ ]*FAIL[: ].*'
fails=$(grep -aoE "$FAILRE" "$LOG" | wc -l | tr -d ' ')
tally=$(grep -ao 'RESULT: [0-9]* passed, [0-9]* failed' "$LOG" | awk '{f+=$4} END {print f+0}')
ranks=$(grep -acE 'rank violations=[1-9]|underflow=[1-9]|mismatch=[1-9]' "$LOG")

worst=$fails; [ "$tally" -gt "$worst" ] && worst=$tally

grep -aq "Type 'help' for commands" "$LOG" || { echo "FAIL: never reached the prompt (${e}s)"; rc=1; }
grep -aq 'system halted' "$LOG" && { echo "FAIL: the kernel halted"; rc=1; }
[ "$suites" -gt 0 ] || { echo "FAIL: no suite reported at all"; rc=1; }
[ "$fails" -eq "$tally" ] || { echo "FAIL: the two failure counters DISAGREE"; \
                               echo "      assertion lines=$fails, RESULT tally=$tally"; \
                               echo "      that is a defect in the instrument, not a verdict on the run"; rc=1; }
[ "$worst" -eq 0 ] || { echo "FAIL: $worst failing assertion(s):"; \
                        grep -aoE "$FAILRE" "$LOG" | sed 's/^/    /'; rc=1; }
[ "$ranks" -eq 0 ] || { echo "FAIL: lock-rank fault reported"; rc=1; }

echo "suites reporting   : $suites"
echo "failing assertions : $fails   (RESULT tally: $tally)"
echo "rank faults        : $ranks"
echo "boot time          : ${e}s"
echo
if [ "$rc" -eq 0 ]; then
    echo "RELEASE ISO VERIFICATION: PASS"
    echo "  $MD5  $(basename "$ISO")"
else
    echo "RELEASE ISO VERIFICATION: FAIL — see $LOG"
fi
exit $rc
