#!/bin/bash
# Host-side unit tests for the OutRun desktop applications. The GUI layer needs
# APP_HOST_TEST so its syscall stub comes from the test; the application cores
# are included directly by their own tests.
set -e
cd "$(dirname "$0")/../.."
CFLAGS="-std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined"
OUT="${TMPDIR:-/tmp}/outrun-apptests"
mkdir -p "$OUT"
gcc $CFLAGS -DAPP_HOST_TEST apps/tests/test_gui.c apps/gui.c -o "$OUT/gui"
"$OUT/gui"
for name in calc vault_pad task_mgr settings; do
    gcc $CFLAGS "apps/tests/test_$name.c" -o "$OUT/$name"
    "$OUT/$name"
done
echo "apps: all host tests passed"
