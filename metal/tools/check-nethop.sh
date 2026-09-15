#!/bin/bash
# Report the [nethop] verdict from both boot logs, with the image md5 each was
# produced from. Checking the stamp matters: a falsify "run" earlier turned out
# to have booted a byte-identical image to the normal build.
for f in /tmp/normal.boot /tmp/falsify.boot; do
  echo "===== $f ====="
  if [ -f "$f" ]; then
    head -2 "$f"
    echo "--- size: $(wc -c < "$f") bytes ---"
    grep -a "nethop" "$f" | head -14
    grep -a "BOOT_RC" "$f"
  else
    echo "not started yet"
  fi
  echo
done
