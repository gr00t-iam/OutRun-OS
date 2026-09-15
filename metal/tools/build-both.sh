#!/bin/bash
# Build BOTH images and stage them in /tmp with distinct names.
#
# Two traps this works around, both of which have already produced a false
# "verified" in this session:
#   1. `make EXTRA=-D...` does NOT retrigger a rebuild -- make sees kernel64.o
#      as up to date and prints "Nothing to be done", leaving whichever variant
#      was built last in build/. The .o MUST be removed between variants.
#   2. `make clean` deletes build/cap_engine.o, which is gitignored and which
#      the rustc on the default PATH (1.75) can no longer compile. Only
#      kernel64.o and the ISO are removed here; cap_engine.o is preserved.
set -u
export PATH="/home/gr00t/.rustup/toolchains/nightly-x86_64-unknown-linux-gnu/bin:$PATH"
cd /mnt/c/Users/jrmym/Documents/GitHub-Pull/OutRun-OS/metal || exit 1

build_variant() {
    tag="$1"; extra="$2"
    rm -f build/kernel64.o build/outrun-os-1.1.0.iso
    if [ -z "$extra" ]; then
        make > "/tmp/build_${tag}.log" 2>&1
    else
        make EXTRA="$extra" > "/tmp/build_${tag}.log" 2>&1
    fi
    rc=$?
    errs=$(grep -cE '\berror\b' "/tmp/build_${tag}.log")
    if [ "$rc" -ne 0 ] || [ ! -f build/outrun-os-1.1.0.iso ]; then
        echo "BUILD FAILED tag=${tag} rc=${rc} errors=${errs}"
        tail -12 "/tmp/build_${tag}.log"
        return 1
    fi
    cp build/outrun-os-1.1.0.iso "/tmp/${tag}.iso"
    echo "built ${tag}: rc=${rc} errors=${errs} $(md5sum "/tmp/${tag}.iso")"
}

build_variant normal  ""                   || exit 1
build_variant falsify "-DNETHOP_FALSIFY"   || exit 1
echo "--- both staged ---"
md5sum /tmp/normal.iso /tmp/falsify.iso
if [ "$(md5sum < /tmp/normal.iso)" = "$(md5sum < /tmp/falsify.iso)" ]; then
    echo "FATAL: images are IDENTICAL -- the falsify flag did not take effect"
    exit 1
fi
echo "images differ: the falsify build is genuinely a different kernel"
