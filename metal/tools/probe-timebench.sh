#!/bin/bash
# probe-timebench.sh <iso> <logfile> [qemu smp args...]
#
# The gate harness boots with stdin on /dev/null, so `timebench` — and with it
# ring-3 roles 63 and 64 — is never reached by `make gate`. This boots the same
# image with a FIFO on stdin, waits for the shell prompt, types `timebench`, and
# captures everything it prints.
#
# Every log names the md5 of the image it booted, same as every other harness
# in this tree.
set -u
ISO=${1:?usage: probe-timebench.sh <iso> <logfile> [smp args]}
LOG=${2:?usage: probe-timebench.sh <iso> <logfile> [smp args]}
shift 2
SMP=${*:-}
CAP=${PROBE_CAP:-1500}

[ -r "$ISO" ] || { echo "probe-timebench: cannot read ISO '$ISO'"; exit 2; }
command -v qemu-system-x86_64 >/dev/null || { echo "probe-timebench: qemu-system-x86_64 not found"; exit 2; }

mkdir -p "$(dirname "$LOG")"
MD5=$(md5sum "$ISO" | cut -d' ' -f1)
WORK=$(mktemp -d)
IMG=$WORK/disk.img
FIFO=$WORK/stdin
# Clean up the scratch dir and the guest on ANY exit, including an interrupt.
# Without this an aborted run leaves a 4 MB image and an orphaned qemu behind,
# and the next run's timings are measured against a machine still busy with the
# last one's.
q=
cleanup() { [ -n "$q" ] && kill -9 "$q" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT INT TERM
dd if=/dev/zero of="$IMG" bs=1M count=4 2>/dev/null
printf 'OUTRUN-DISK-SIGNATURE-OK' | dd of="$IMG" bs=1 seek=1024 conv=notrunc 2>/dev/null
mkfifo "$FIFO"

echo "# probe=timebench iso=$ISO md5=$MD5 qemu='${SMP:-uniprocessor}'" > "$LOG"

BLK="-drive file=$IMG,if=none,format=raw,id=vd0 -device virtio-blk-pci,drive=vd0,disable-legacy=on,disable-modern=off"
NET="-netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef"
GPU="-vga none -device virtio-vga"

# Hold the FIFO open for the whole run, or qemu sees EOF the moment it starts.
#
# READ-WRITE (8<>), NOT WRITE-ONLY (8>), and that is a deadlock fix rather than a
# style choice. Opening a FIFO write-only BLOCKS until a reader attaches — and
# the only reader is the qemu below, which cannot start because the script is
# still blocked on the open. The first version of this script did exactly that
# and hung for an hour with no qemu process and a one-line log. Opening
# read-write never blocks, because the same descriptor satisfies both ends.
exec 8<>"$FIFO"

# shellcheck disable=SC2086
qemu-system-x86_64 $SMP -cdrom "$ISO" -m 512M -nographic -no-reboot $GPU $BLK $NET \
  < "$FIFO" >> "$LOG" 2>&1 &
q=$!

# Wait for the prompt, then type the command.
e=0; typed=0
while [ "$e" -lt "$CAP" ]; do
    kill -0 $q 2>/dev/null || break
    if [ "$typed" -eq 0 ] && grep -aq "Type 'help' for commands" "$LOG" 2>/dev/null; then
        sleep 3
        printf 'timebench\n' >&8
        typed=1
        echo "  probe: typed 'timebench' at ${e}s"
    fi
    # timebench's OWN completion marker, not '-- done --'. Every suite in this
    # kernel prints that, so it appears dozens of times during boot and says
    # nothing about the command we typed. This line is printed once, by the
    # last thing timebench does.
    if [ "$typed" -eq 1 ] && grep -aq 'ring-3 cpu-split probe:' "$LOG" 2>/dev/null; then
        sleep 3; break
    fi
    sleep 5; e=$((e+5))
done
kill -9 $q 2>/dev/null; wait $q 2>/dev/null
q=
exec 8>&-
rm -rf "$WORK"

echo "=============================================================="
echo " timebench probe: $ISO"
echo "   md5     : $MD5"
echo "   qemu    : ${SMP:-uniprocessor}"
echo "   elapsed : ${e}s (cap ${CAP}s)"
if [ "$typed" -eq 0 ]; then
    echo "   RESULT  : NO-PROMPT — never reached the shell; nothing was typed."
    echo "             This is an invalid run, not a verdict."
    exit 2
fi
grep -a 'r63\|r64\|clock probe\|cpu-split' "$LOG" || true
echo "=============================================================="
grep -aq -- '-- done --' "$LOG" || { echo "   RESULT  : TRUNCATED — timebench never printed '-- done --'."; exit 2; }
r63=$(grep -ao 'ring-3 clock probe: [^(]*(exit [0-9]*)' "$LOG" | tail -1)
r64=$(grep -ao 'ring-3 cpu-split probe: [^(]*(exit [0-9]*)' "$LOG" | tail -1)
echo "   role 63 : ${r63:-NOT REPORTED}"
echo "   role 64 : ${r64:-NOT REPORTED}"
rc=0
case "$r63" in *"(exit 1870)") ;; *) rc=1 ;; esac
case "$r64" in *"(exit 1900)") ;; *) rc=1 ;; esac
[ "$rc" -eq 0 ] && echo "   RESULT  : OK" || echo "   RESULT  : FAIL"
exit $rc
