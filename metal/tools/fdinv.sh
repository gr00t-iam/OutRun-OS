#!/bin/bash
# Inventory of the descriptor table's consumers, by enclosing function.
cd /mnt/c/Users/jrmym/Documents/GitHub-Pull/OutRun-OS/metal/kernel
echo "g_ofiles refs:   $(grep -c 'g_ofiles' kernel64.c)"
echo "owner_mask refs: $(grep -c 'owner_mask' kernel64.c)"
echo "ofile_lock refs: $(grep -c 'g_ofile_lock' kernel64.c)"
echo "--- enclosing functions of g_ofiles/owner_mask refs (unique) ---"
grep -n 'g_ofiles\|owner_mask' kernel64.c | cut -d: -f1 | while read n; do
  awk -v n="$n" '/^static [a-zA-Z_0-9 *]+\(/ {f=$0} /^[a-z].*\(.*\) *\{$/ {f=$0} NR==n {print substr(f,1,90); exit}' kernel64.c
done | sort | uniq -c | sort -rn
