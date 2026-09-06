#!/bin/bash
# Compare two regression logs on the same metric. Written as a file because
# nested quoting through wsl.exe silently mangled the greps twice.
for f in "$@"; do
    printf '%s: PASS=%s FAIL=%s prompt=%s lines=%s\n' "$f" \
        "$(grep -c '  PASS  ' "$f")" \
        "$(grep -c '  FAIL  ' "$f")" \
        "$(grep -c 'outrun>' "$f")" \
        "$(wc -l < "$f")"
done
