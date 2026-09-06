#!/bin/bash
# Tally a single boot log the way gate-matrix does, and archive it.
#   usage: tally.sh <log> [archive-path]
L="$1"
echo "suites=$(grep -ac 'RESULT:' "$L")"
grep -ao 'RESULT: [0-9]* passed, [0-9]* failed' "$L" | awk '{p+=$2; f+=$4} END {print "passed="p" failed="f}'
echo "fail_lines=$(grep -acE '^\[[a-zA-Z0-9_. -]+\][ ]*FAIL[: ]' "$L")"
grep -a 'rank violations=' "$L" | tail -1
echo "prompt=$(grep -ac 'outrun>' "$L")"
echo "--- [mm] suite"
grep -a '^\[mm     \]' "$L"
if [ -n "$2" ]; then cp "$L" "$2"; echo "archived -> $2"; fi
