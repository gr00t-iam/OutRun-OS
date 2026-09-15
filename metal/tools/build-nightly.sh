#!/bin/bash
# Build helper: use the nightly rustc cap_engine.rs requires.
# The rustc on PATH is 1.75 and rejects pointee_sized/meta_sized/legacy_receiver.
export PATH="/home/gr00t/.rustup/toolchains/nightly-x86_64-unknown-linux-gnu/bin:$PATH"
cd /mnt/c/Users/jrmym/Documents/GitHub-Pull/OutRun-OS/metal || exit 1
rustc --version
make "$@" > /tmp/build_out.log 2>&1
rc=$?
echo "MAKE_EXIT=$rc"
grep -cE '\berror\b' /tmp/build_out.log
tail -4 /tmp/build_out.log
ls -la build/outrun-os-1.1.0.iso 2>/dev/null && md5sum build/outrun-os-1.1.0.iso
