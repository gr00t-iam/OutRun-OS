#!/bin/bash
# gate-classify-test.sh — unit tests for gate-matrix.sh's run classifier.
#
# The gate itself needs a boot to say anything, which is why its own reporting
# logic went three releases without a test and shipped a defect that mislabelled
# a truncated boot as a suite failure. This exercises that logic directly, on
# synthetic logs, in well under a second.
#
# Usage: tools/gate-classify-test.sh
set -u
cd "$(dirname "$0")/.." || exit 2

# Pull in classify_run/run_is_verdict WITHOUT running the matrix.
GATE_LIB_ONLY=1 . tools/gate-matrix.sh

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0

PROMPT="Type 'help' for commands"

# mklog <name> <body...> — build a synthetic boot log.
mklog() { n=$1; shift; printf '%s\n' "$@" > "$TMP/$n.log"; echo "$TMP/$n.log"; }

# check <name> <expected> <log> <suites> <fails> <tally> <ranks> <worst> <elapsed> <cap>
check() {
    name=$1; want=$2; shift 2
    got=$(classify_run "$@")
    if [ "$got" = "$want" ]; then
        pass=$((pass+1)); printf '  ok   %-34s -> %s\n' "$name" "$got"
    else
        fail=$((fail+1)); printf '  FAIL %-34s -> got %s, want %s\n' "$name" "$got" "$want"
    fi
}

echo "gate-classify-test: classifier"

# --- the regression this file exists for ---------------------------------
# An smp4-iommu boot killed at the 900 s cap, mid-`cas`, with two suites having
# already reported failures. The old sequential logic called this FAIL and named
# [vfiostrs]/[capdma]. It must be TRUNCATED: the run is not a verdict.
L=$(mklog truncated \
    "# config=smp4-iommu md5=deadbeef" \
    "[vfiostrs]  FAIL  every round's driver exited cleanly" \
    "[capdma ]  FAIL  confined device attempting kernel DMA is BLOCKED by hardware" \
    "[vfiostrs] RESULT: 12 passed, 1 failed" \
    "[capdma ] RESULT: 10 passed, 1 failed" \
    "[cas    ] put len 512 hash 721e93b8 -> DEDUP to block 852 (no write)")
check "truncated at cap, 2 pre-kill FAILs" TRUNCATED "$L" 2 2 2 0 2 900 900

# The same log, but the guest died on its own well before the cap.
check "died before prompt, before cap"     NO-PROMPT "$L" 2 2 2 0 2 120 900

# --- completed runs are still judged on their assertions ------------------
L=$(mklog clean "[mcq    ] RESULT: 4 passed, 0 failed" "$PROMPT")
check "clean complete boot"                OK        "$L" 45 0 0 0 0 300 900

L=$(mklog failed \
    "[mcq    ]  FAIL  two or more cores were IN RING 3 SIMULTANEOUSLY" \
    "[mcq    ] RESULT: 3 passed, 1 failed" "$PROMPT")
check "completed boot with a real FAIL"    FAIL      "$L" 45 1 1 0 1 300 900

# A completed boot that failed must NOT be downgraded just because it was slow.
check "completed FAIL that also hit cap"   FAIL      "$L" 45 1 1 0 1 900 900

# --- the other invalid / unreliable states --------------------------------
L=$(mklog halted "[panic  ] system halted" "[mcq    ] RESULT: 3 passed, 1 failed")
check "guest panicked"                     HALTED    "$L" 45 1 1 0 1 300 900

L=$(mklog nosuites "$PROMPT")
check "prompt but no suites reported"      NO-SUITES "$L" 0 0 0 0 0 300 900

L=$(mklog split "[x] RESULT: 1 passed, 2 failed" "$PROMPT")
check "counters disagree"                  COUNTER-SPLIT "$L" 45 1 2 0 2 300 900

L=$(mklog rank "[klock  ] rank violations=1" "$PROMPT")
check "lock-rank fault outranks clean"     RANK-FAULT "$L" 45 0 0 1 0 300 900

# HALTED must win over a rank fault: the guest dying is the larger fact.
L=$(mklog haltrank "[klock  ] rank violations=1" "[panic  ] system halted")
check "halted outranks rank fault"         HALTED    "$L" 45 0 0 1 0 300 900

echo "gate-classify-test: run_is_verdict"
for s in OK FAIL RANK-FAULT; do
    if run_is_verdict "$s"; then pass=$((pass+1)); printf '  ok   %-34s -> verdict\n' "$s"
    else fail=$((fail+1)); printf '  FAIL %-34s -> should be a verdict\n' "$s"; fi
done
for s in TRUNCATED NO-PROMPT NO-SUITES COUNTER-SPLIT; do
    if run_is_verdict "$s"; then fail=$((fail+1)); printf '  FAIL %-34s -> should NOT be a verdict\n' "$s"
    else pass=$((pass+1)); printf '  ok   %-34s -> not a verdict\n' "$s"; fi
done

echo
echo "gate-classify-test: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
