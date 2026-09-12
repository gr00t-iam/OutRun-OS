#!/bin/bash
# Host-side checks for the browser: core parsing (no TLS) and the UI layer.
cd /mnt/c/Users/jrmym/Documents/GitHub-Pull/OutRun-OS || exit 1
CF="-std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined"
INC="-Iapps -Iinclude -Iapps/vendor/mbedtls/include"
MBEDCFG="-DMBEDTLS_CONFIG_FILE=\"web_mbedtls_config.h\""
rc=0

echo "=== core (no TLS) ==="
if gcc $CF $INC apps/tests/test_web.c -o /tmp/web_core 2>/tmp/e1; then
    /tmp/web_core || rc=1
else
    echo "BUILD FAILED"; head -20 /tmp/e1; rc=1
fi

echo "=== UI layer ==="
LIB=$(ls apps/vendor/mbedtls/library/*.c 2>/dev/null | wc -l)
echo "  (mbedtls library sources available: $LIB)"
if gcc $CF $INC $MBEDCFG apps/tests/test_web_ui.c apps/gui.c \
       apps/vendor/mbedtls/library/*.c -o /tmp/web_ui 2>/tmp/e2; then
    /tmp/web_ui || rc=1
else
    echo "BUILD FAILED"; head -25 /tmp/e2; rc=1
fi
exit $rc
