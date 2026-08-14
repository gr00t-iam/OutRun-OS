#!/bin/bash
# gate-dirty.sh — the DIRTY-VOLUME release-gate configuration.
#
#   usage: gate-dirty.sh <iso> [boots] [extra-qemu-args] [workdir]
#
# Every other gate configuration builds a FRESH disk image per boot. That is
# exactly why the suite set was able to be non-idempotent across boots for its
# whole history without anyone noticing: nothing ever booted twice on one
# volume. v0.75 shipped persistence, which makes "the volume already has state"
# a supported configuration, so it needs a configuration that tests it.
#
# What this does:
#
#   boot 1   fresh image. Runs the suites, then TYPES two commands at the serial
#            console to create the deliberately durable cross-boot artefacts:
#
#              udbpersist     -> account 'udbreboot'    (v0.75 persistence)
#              vfscrashwrite  -> file 'vfs-reboot-test' (v0.48 journal recovery)
#
#            In that order: vfscrashwrite halts the machine forever (cli; hlt)
#            on purpose, simulating power loss, so nothing can follow it.
#
#   boot 2.. the SAME image, untouched. Must reach the prompt with zero failing
#            assertions, AND must still find both artefacts. The second half
#            matters as much as the first: a fixture-reset policy that deleted
#            them would otherwise leave every run looking green while the only
#            cross-boot evidence in the tree had been destroyed.
#
# Exits non-zero on any failing assertion, any lost artefact, or any boot that
# does not reach the prompt — so `make gate-dirty` fails rather than printing
# red text and succeeding.
set -u

ISO=${1:?usage: gate-dirty.sh <iso> [boots] [extra-qemu-args] [workdir]}
BOOTS=${2:-3}
EXTRA=${3:-}
WORK=${4:-build/gate-dirty}
CAP=${GATE_DIRTY_CAP:-480}

[ -r "$ISO" ] || { echo "gate-dirty: cannot read ISO '$ISO'"; exit 2; }
command -v qemu-system-x86_64 >/dev/null || { echo "gate-dirty: qemu-system-x86_64 not found"; exit 2; }

# v0.76: REFUSE TO RUN CONCURRENTLY.
#
# This script begins by `rm -rf`-ing its workdir. Three of these were once
# launched against the same default workdir at the same time; each wiped the
# directory out from under the others and killed their QEMU processes, and the
# resulting truncated logs were misread as an environment problem before the
# real cause was found.
#
# Two guards, because either alone is thin. The lock stops two runs existing at
# once — which is what actually corrupts state, and which would invalidate the
# timing anyway, since two -smp 4 boots on one host contend for the same cores.
# The PID-unique workdir the Makefile passes means that even if the lock is
# somehow bypassed (a stale filesystem without flock, say), no run can delete
# another's logs.
LOCK=${GATE_DIRTY_LOCK:-$(dirname "$WORK")/.gate-dirty.lock}
mkdir -p "$(dirname "$LOCK")"
exec 9>"$LOCK" || { echo "gate-dirty: cannot create lock $LOCK"; exit 2; }
if command -v flock >/dev/null 2>&1; then
    if ! flock -n 9; then
        echo "gate-dirty: REFUSING TO START — another gate run holds $LOCK."
        echo "  Concurrent runs wipe each other's workdir and contend for the"
        echo "  cores whose timing this gate measures. Wait for the other run."
        exit 3
    fi
else
    echo "gate-dirty: WARNING: flock unavailable; concurrency is NOT guarded."
fi

rm -rf "$WORK"; mkdir -p "$WORK"
IMG=$WORK/disk.img
MD5=$(md5sum "$ISO" | cut -d' ' -f1)

# One image, created ONCE, reused by every boot. Never recreate it in the loop.
dd if=/dev/zero of="$IMG" bs=1M count=4 2>/dev/null
printf 'OUTRUN-DISK-SIGNATURE-OK' | dd of="$IMG" bs=1 seek=1024 conv=notrunc 2>/dev/null

echo "=============================================================="
echo " DIRTY-VOLUME GATE"
echo "   iso     : $ISO"
echo "   md5     : $MD5"
echo "   boots   : $BOOTS   (one image, reused — never recreated)"
echo "   qemu    : ${EXTRA:-<uniprocessor>}"
echo "   workdir : $WORK"
echo "=============================================================="

rc=0

# Drive qemu's console with a PIPE, not a FIFO. The repo is routinely checked
# out on a filesystem that cannot create named pipes at all — under WSL a
# /mnt/c working tree is DrvFs, and mkfifo there fails outright with
# "Operation not supported". A pipe needs no filesystem support and behaves
# identically for this purpose.
#
# The feeder subshell waits for the prompt by watching the log qemu is writing,
# then types. Only boot 1 needs input; the rest read /dev/null.
feed_boot1() {
    w=0
    while [ "$w" -lt "$CAP" ]; do
        grep -aq "Type 'help' for commands" "$1" 2>/dev/null && break
        sleep 5; w=$((w+5))
    done
    sleep 2
    printf 'udbpersist\n'
    sleep 25                            # one 4096-round KDF, then the save
    printf 'vfscrashwrite\n'            # LAST: halts the machine by design
    sleep 10
}

for i in $(seq 1 "$BOOTS"); do
    LOG=$WORK/boot$i.log
    echo "# iso=$ISO md5=$MD5 boot=$i qemu='${EXTRA:-uniprocessor}'" > "$LOG"

    # shellcheck disable=SC2086
    if [ "$i" = "1" ]; then
        feed_boot1 "$LOG" | qemu-system-x86_64 $EXTRA -cdrom "$ISO" -m 512M -nographic -no-reboot \
          -vga none -device virtio-vga \
          -drive file=$IMG,if=none,format=raw,id=vd0 \
          -device virtio-blk-pci,drive=vd0,disable-legacy=on,disable-modern=off \
          -netdev user,id=n0 \
          -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef \
          >> "$LOG" 2>&1 &
    else
        qemu-system-x86_64 $EXTRA -cdrom "$ISO" -m 512M -nographic -no-reboot \
          -vga none -device virtio-vga \
          -drive file=$IMG,if=none,format=raw,id=vd0 \
          -device virtio-blk-pci,drive=vd0,disable-legacy=on,disable-modern=off \
          -netdev user,id=n0 \
          -device virtio-net-pci,netdev=n0,disable-legacy=on,disable-modern=off,mac=52:54:00:ab:cd:ef \
          < /dev/null >> "$LOG" 2>&1 &
    fi
    q=$!; e=0
    # Boot 1 must outlive the prompt — it still has two commands to type — so it
    # waits for vfscrashwrite's own confirmation instead of for the prompt.
    while [ "$e" -lt "$CAP" ]; do
        kill -0 $q 2>/dev/null || break
        if [ "$i" = "1" ]; then
            grep -aq 'vfscrashwrite: journal-committed' "$LOG" 2>/dev/null && { sleep 5; break; }
        else
            grep -aq "Type 'help' for commands" "$LOG" 2>/dev/null && { sleep 5; break; }
            grep -aq 'system halted' "$LOG" 2>/dev/null && { sleep 3; break; }
        fi
        sleep 5; e=$((e+5))
    done

    reached=0
    grep -aq "Type 'help' for commands" "$LOG" && reached=1

    kill -9 $q 2>/dev/null; wait $q 2>/dev/null
    pkill -f "qemu-system-x86_64 .*$IMG" 2>/dev/null

    suites=$(grep -ac 'RESULT:' "$LOG")

    # TWO INDEPENDENT FAILURE COUNTERS. See the same comment in
    # release-verify.sh: the original pattern required a lowercase tag and a
    # space after FAIL, and `[pthreads_smp] FAIL:` satisfied neither. This gate
    # reported PASS on a -smp 4 boot whose own RESULT line read
    # `5 passed, 1 failed`. The assertion-line count is now cross-checked
    # against the suites' own RESULT tallies, and disagreement fails the gate:
    # a wider pattern alone would just be the same single point of blindness,
    # moved.
    FAILRE='^\[[a-zA-Z0-9_. -]+\][ ]*FAIL[: ].*'
    fails=$(grep -aoE "$FAILRE" "$LOG" | wc -l | tr -d ' ')
    tally=$(grep -ao 'RESULT: [0-9]* passed, [0-9]* failed' "$LOG" | awk '{f+=$4} END {print f+0}')
    grep -aoE "$FAILRE" "$LOG" | sed 's/[[:space:]]\+/ /g' | sort > "$WORK/fail$i.txt"
    resets=$(grep -ac '^\[fixture\] reset' "$LOG")
    refus=$(grep -ac '^\[fixture\] REFUSED' "$LOG")

    status=OK
    [ "$reached" = "1" ] || status=NO-PROMPT
    grep -aq 'system halted' "$LOG" && [ "$i" != "1" ] && status=PANIC

    printf 'boot %-2s %-9s suites=%-3s failing-assertions=%-3s resets=%-2s refusals=%-2s (%ss)\n' \
        "$i" "$status" "$suites" "$fails" "$resets" "$refus" "$e"

    [ "$reached" = "1" ] || rc=1
    [ "$fails" -eq 0 ] || rc=1
    [ "$tally" -eq 0 ] || rc=1
    [ "$fails" -eq "$tally" ] || {
        echo "  !! failure counters DISAGREE on boot $i: assertion lines=$fails, RESULT tally=$tally"
        echo "  !! that is a defect in this harness, not a verdict on the boot"
        rc=1
    }
    # A refusal means a suite tried to reset a durable artefact. The allow-list
    # stopped it, but the attempt is a defect in the suite and must be loud.
    [ "$refus" -eq 0 ] || { echo "  !! a suite attempted to reset a durable artefact"; rc=1; }

    if [ "$fails" -gt 0 ]; then
        echo "  failing assertions:"; sed 's/^/    /' "$WORK/fail$i.txt"
    fi
done

echo
echo "---- consecutive-boot assertion diffs (must all be empty) ----"
for i in $(seq 2 "$BOOTS"); do
    p=$((i-1))
    n=$(comm -13 "$WORK/fail$p.txt" "$WORK/fail$i.txt" | wc -l)
    d=$(comm -23 "$WORK/fail$p.txt" "$WORK/fail$i.txt" | wc -l)
    printf 'boot %s -> %s : new=%s disappeared=%s\n' "$p" "$i" "$n" "$d"
    [ "$n" -eq 0 ] && [ "$d" -eq 0 ] || rc=1
done

echo
echo "---- durable cross-boot artefacts ----"
echo "boot 1 creates them; every later boot must still find them."
for i in $(seq 1 "$BOOTS"); do
    # "CROSS-BOOT OK" alone is ambiguous: cmd_udb_persist's own help text
    # contains that string, so it matches on the boot that CREATES the marker.
    # Only the detection line carries "CROSS-BOOT OK: account".
    udb=$(grep -ac 'CROSS-BOOT OK: account' "$WORK/boot$i.log")
    vfs=$(grep -ac 'cross-reboot journal probe: content VERIFIED' "$WORK/boot$i.log")
    if [ "$i" = "1" ]; then
        # Check boot 1 actually CREATED them. Without this, a failure to type at
        # the console shows up as "did not survive" on boot 2 — the wrong
        # diagnosis for the wrong boot, and the expensive kind to chase.
        cu=$(grep -ac "created 'udbreboot'" "$WORK/boot$i.log")
        cv=$(grep -ac 'vfscrashwrite: journal-committed' "$WORK/boot$i.log")
        printf 'boot %-2s created: udbreboot=%s vfs-reboot-test=%s\n' "$i" "$cu" "$cv"
        [ "$cu" -ge 1 ] || { echo "  !! boot 1 never created 'udbreboot' — console input did not land"; rc=1; }
        [ "$cv" -ge 1 ] || { echo "  !! boot 1 never created 'vfs-reboot-test' — console input did not land"; rc=1; }
    else
        printf 'boot %-2s udbreboot=%s vfs-reboot-test=%s\n' "$i" "$udb" "$vfs"
        [ "$udb" -ge 1 ] || { echo "  !! 'udbreboot' did not survive into boot $i"; rc=1; }
        [ "$vfs" -ge 1 ] || { echo "  !! 'vfs-reboot-test' did not survive into boot $i"; rc=1; }
    fi
done

echo
if [ "$rc" -eq 0 ]; then
    echo "DIRTY-VOLUME GATE: PASS  ($BOOTS boots, one image, ${EXTRA:-uniprocessor})"
else
    echo "DIRTY-VOLUME GATE: FAIL  — see $WORK/boot*.log"
fi
exit $rc
