#!/bin/sh
# usage: build_tracer.sh NAMES_FILE OUT_DIR   -> OUT_DIR/dt-arm64.dylib, OUT_DIR/dt-arm64e.dylib
set -e
D=$(cd "$(dirname "$0")" && pwd)
python3 -I "$D/gen_tracer.py" "$1" "$2"
for a in arm64 arm64e; do
  xcrun clang -arch $a -dynamiclib -O1 -o "$2/dt-$a.dylib" "$2/tracer.c" "$2/stubs.S" -Wl,-undefined,dynamic_lookup
  codesign -s - -f "$2/dt-$a.dylib" >/dev/null 2>&1
done
