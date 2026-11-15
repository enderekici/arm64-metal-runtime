#!/bin/sh
# usage: build.sh ARCH OUT_DYLIB   (ARCH arm64 or arm64e); needs gen_stubs.py output in $GEN
set -e
D=$(cd "$(dirname "$0")" && pwd)
GEN=${GEN:-/Users/ender/code/evamcep/arm64-metal-work/logs/trace/ip}
cp "$D/interpose.c" "$GEN/"
xcrun clang -arch "$1" -std=gnu11 -Wno-deprecated-non-prototype -dynamiclib -O1 -o "$2" "$GEN/interpose.c" "$GEN/stubs.S" -install_name "@rpath/$(basename "$2")"
codesign -s - -f "$2" >/dev/null 2>&1
