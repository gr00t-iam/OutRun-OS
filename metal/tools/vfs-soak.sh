#!/bin/bash
# vfs-soak.sh — repeat cmd_vfs_stress on ONE boot, many times.
#
#   usage: vfs-soak.sh <iso> [iterations] [extra-qemu-args] [workdir]
#
# WHY THIS EXISTS. Every gate configuration in this tree runs each suite ONCE
# per boot. v0.90 put shared (reader) acquisition on g_vfs_lock and an optimistic
# probe in vfs_open_for -- new concurrency on the two hottest locks in the
# kernel, verified so far by one boot per tier. One boot cannot see an
# intermittent below roughly 1 in 4, and a reader/writer interlock is precisely
# the kind of code whose failures are rare and timing-dependent.
#
# It lives in tools/ rather than in somebody's shell history on purpose: v0.78
# records that the release matrix was once reproducible by exactly one person
# because its harness lived outside the tree. That is a habit, not a gate.
#
# WHAT IT CHECKS, all of which fail the run:
#   - a panic, a lock-rank violation, a rank mismatch or an underflow, at any
#     point in any iteration;
#   - any suite assertion failing (vfsstrs prints its own RESULT tally);
#   - the machine ceasing to answer -- which is what a reader-starvation
#     deadlock or a lost wake would look like from out here, and is the reason
#     progress is judged by iterations COMPLETED rather than by wall clock.
#
# It stamps the md5 of the image it booted into the log, per the evidence
# conventions in CLAUDE.md: a log that cannot name the binary it came from is
# not evidence.
set -u

ISO=${1:?usage: vfs-soak.sh <iso> [iterations] [extra-qemu-args] [workdir]}
ITERS=${2:-100}
QARGS=${3:-}
WORK=${4:-.logs/vfs-soak-$$}

[ -r "$ISO" ] || { echo "vfs-soak: cannot read ISO '$ISO'"; exit 2; }
mkdir -p "$WORK"
LOG="$WORK/soak.log"
IMG="$WORK/soak.img"
MD5=$(md5sum "$ISO" | cut -d' ' -f1)

dd if=/dev/zero of="$IMG" bs=1M count=64 status=none

echo "=============================================================="
echo " VFS SOAK"
echo "   iso        : $ISO"
echo "   md5        : $MD5"
echo "   iterations : $ITERS"
echo "   qemu       : ${QARGS:-<uniprocessor>}"
echo "   log        : $LOG"
echo "=============================================================="
{ echo "# vfs-soak iso=$ISO md5=$MD5 iters=$ITERS qemu='${QARGS:-uniprocessor}'"; } > "$LOG"

BOOT_CAP=${BOOT_CAP:-1500}
ITER_CAP=${ITER_CAP:-900}
STATUS="$WORK/status"
: > "$STATUS"

# NO mkfifo. The working tree is on /mnt/c, which is DrvFs, and mkfifo there
# fails outright with "Operation not supported" — gate-dirty.sh carries the same
# note for the same reason. A plain pipe needs no filesystem support: the feeder
# runs as a subshell writing to stdout, and reads the log qemu is writing to
# decide when to type next. Getting this wrong the first time produced a "SOAK
# FAIL: iteration 1 did not finish" that looked exactly like a kernel stall and
# was entirely the harness — which is the failure mode this whole tree is
# organised against, so it is recorded here rather than quietly corrected.
feeder() {
    log=$1
    t0=$SECONDS
    while [ $((SECONDS - t0)) -lt "$BOOT_CAP" ]; do
        grep -aq "Type 'help' for commands" "$log" 2>/dev/null && break
        sleep 3
    done
    if ! grep -aq "Type 'help' for commands" "$log" 2>/dev/null; then
        echo "NOPROMPT" >> "$STATUS"; return
    fi
    echo "  [soak] prompt reached after $((SECONDS - t0))s; typing $ITERS iteration(s)" >&2
    sleep 3
    for i in $(seq 1 "$ITERS"); do
        before=$(grep -ac '^\[vfsstrs\] RESULT:' "$log" 2>/dev/null || echo 0)
        printf 'vfsstress\n'
        t1=$SECONDS
        while :; do
            now=$(grep -ac '^\[vfsstrs\] RESULT:' "$log" 2>/dev/null || echo 0)
            [ "$now" -gt "$before" ] && break
            if [ $((SECONDS - t1)) -gt "$ITER_CAP" ]; then
                echo "STALL $i" >> "$STATUS"; return
            fi
            sleep 2
        done
        echo "DONE $i" > "$STATUS.n"
        if grep -aqE 'PANIC|RANK VIOLATION|RANK MISMATCH|OUT-OF-ORDER RELEASE' "$log"; then
            echo "FAULT $i" >> "$STATUS"; return
        fi
        [ $((i % 10)) -eq 0 ] && echo "  [soak] $i/$ITERS done, $((SECONDS - t0))s elapsed" >&2
    done
    echo "OK $ITERS" >> "$STATUS"
}

# The guest never exits on its own — it sits at its prompt — so the pipeline runs
# in the background and the parent watches the status file the feeder writes.
# Killing is keyed on this run's own IMG path, which no other qemu shares.
# shellcheck disable=SC2086
( feeder "$LOG" | qemu-system-x86_64 $QARGS -cdrom "$ISO" -m 512M -nographic -no-reboot \
    -vga none -device virtio-vga \
    -drive file="$IMG",if=none,format=raw,id=vd0 \
    -device virtio-blk-pci,drive=vd0,disable-legacy=on,disable-modern=off \
    -netdev user,id=n0 \
    -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef \
    >> "$LOG" 2>&1 ) &
RUN_PID=$!
cleanup() { pkill -f "$IMG" 2>/dev/null; kill $RUN_PID 2>/dev/null; }
trap cleanup EXIT

t0=$SECONDS
RUN_CAP=${RUN_CAP:-21600}
while :; do
    [ -s "$STATUS" ] && break
    kill -0 $RUN_PID 2>/dev/null || break
    [ $((SECONDS - t0)) -gt $RUN_CAP ] && { echo "SOAK FAIL: overall cap ${RUN_CAP}s reached"; break; }
    sleep 5
done
pkill -f "$IMG" 2>/dev/null
wait $RUN_PID 2>/dev/null

done_n=$(sed -n 's/^DONE //p' "$STATUS.n" 2>/dev/null | tail -1); done_n=${done_n:-0}
if grep -q '^NOPROMPT' "$STATUS" 2>/dev/null; then
    echo "SOAK FAIL: no shell prompt within ${BOOT_CAP}s — this run measured nothing"; exit 1
fi
if grep -q '^STALL' "$STATUS" 2>/dev/null; then
    echo "SOAK FAIL: iteration $(sed -n 's/^STALL //p' "$STATUS") did not finish within ${ITER_CAP}s"
    echo "  A stalled iteration is what a reader-starvation deadlock would look like from"
    echo "  here — but check the log for console input actually landing before concluding"
    echo "  that: a harness that cannot type is indistinguishable from a guest that cannot run."
    exit 1
fi
if grep -q '^FAULT' "$STATUS" 2>/dev/null; then
    echo "SOAK FAIL: a fault appeared at iteration $(sed -n 's/^FAULT //p' "$STATUS")"
    grep -aE 'PANIC|RANK VIOLATION|RANK MISMATCH|OUT-OF-ORDER RELEASE' "$LOG" | head -5
    exit 1
fi

fails=$(grep -a '^\[vfsstrs\] RESULT:' "$LOG" | sed -n 's/.*, \([0-9]*\) failed.*/\1/p' \
        | awk '{s+=$1} END{print s+0}')
runs=$(grep -ac '^\[vfsstrs\] RESULT:' "$LOG")

echo "--------------------------------------------------------------"
echo " iterations requested : $ITERS"
echo " iterations completed : $done_n"
echo " RESULT lines seen    : $runs   (includes the boot's own run)"
echo " failing assertions   : $fails"
echo " rank faults          : $(grep -acE 'RANK VIOLATION|RANK MISMATCH|OUT-OF-ORDER RELEASE' "$LOG")"
echo " panics               : $(grep -ac 'PANIC' "$LOG")"
echo " image                : $MD5"
echo " wall clock           : $((SECONDS - t0))s"
echo "--------------------------------------------------------------"

[ "$done_n" -eq "$ITERS" ] && [ "$fails" -eq 0 ] || { echo "VFS SOAK: FAIL"; exit 1; }
echo "VFS SOAK: PASS"
