#!/bin/bash
# Inspect a gate log: RESULT count, rank counters, FAIL lines, tail.
L="$1"
echo "file: $L  lines: $(wc -l < "$L")"
echo "RESULT lines: $(grep -ac 'RESULT:' "$L")"
echo "--- rank counters (last 3)"
grep -a 'rank violations=\|INVERSION\|VIOLATION\|MISMATCH\|underflow' "$L" | tail -3
echo "--- FAIL lines"
grep -aE '^\[[a-zA-Z0-9_. -]+\][ ]*FAIL[: ]' "$L" | head -10
echo "--- mm lines"
grep -a '^\[mm     \]' "$L" | head -20
echo "--- tail"
tail -5 "$L" | cut -c1-140
