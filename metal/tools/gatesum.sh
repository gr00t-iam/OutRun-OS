#!/bin/bash
# Summarise every log in a gate run dir.
cd "$1"
for f in *.log; do
  r=$(grep -ac 'RESULT:' "$f")
  fl=$(grep -acE '^\[[a-zA-Z0-9_. -]+\][ ]*FAIL[: ]' "$f")
  tally=$(grep -ao 'RESULT: [0-9]* passed, [0-9]* failed' "$f" | awk '{f+=$4} END {print f+0}')
  pass=$(grep -ao 'RESULT: [0-9]* passed, [0-9]* failed' "$f" | awk '{p+=$2} END {print p+0}')
  rk=$(grep -acE 'rank violations=[1-9]|underflow=[1-9]|mismatch=[1-9]' "$f")
  pr=$(grep -ac 'outrun>' "$f")
  echo "$f: suites=$r passed=$pass failed_lines=$fl failed_tally=$tally rankbad=$rk prompt=$pr lines=$(wc -l < "$f")"
done
