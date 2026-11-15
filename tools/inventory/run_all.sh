#!/bin/sh
# Native capture of every finish-line target under the interposer tracer.
# usage: run_all.sh WORKTREE WORKDIR      (WORKDIR holds ip/ built dylibs + names.tsv; writes WORKDIR/runs/<tag>/)
set -u
WT=$1; W=$2
T="$WT/tools/inventory"
J=$HOME/Library/Java/JavaVirtualMachines/azul-25/Contents/Home
R=$W/runs-$(date +%s)
A=$W/ip/trace-arm64.dylib; E=$W/ip/trace-arm64e.dylib
export GEN=$W/ip
mkdir -p "$R" "$W/bins" "$W/tarsrc/d/sub" "$W/tarout-$$"
# controls: known positives (control) and the blindness check (control_svc)
xcrun clang -arch arm64 -O1 "$T/control.c" -o "$W/control" && "$T/run_traced.sh" control "$A" "$R" "$W/control"
xcrun clang -arch arm64 -O1 "$T/control_svc.c" -o "$W/control_svc" && "$T/run_traced.sh" control-svc-0 "$A" "$R" "$W/control_svc" 0 && "$T/run_traced.sh" control-svc-7 "$A" "$R" "$W/control_svc" 7
# system binaries are SIP/platform: DYLD_* is ignored, so trace ad-hoc re-signed arm64e copies
for b in plutil tar; do lipo /usr/bin/$b -thin arm64e -output "$W/bins/$b-arm64e" && codesign -s - -f "$W/bins/$b-arm64e" 2>/dev/null; done
[ -f "$W/in.plist" ] || printf '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict><key>a</key><integer>1</integer></dict></plist>\n' > "$W/in.plist"
echo hello > "$W/tarsrc/d/a.txt"; echo x > "$W/tarsrc/d/sub/b.txt"; ln -sf a.txt "$W/tarsrc/d/link"
/usr/bin/tar -cf "$W/tarsrc.tar" -C "$W/tarsrc" d
"$T/run_traced.sh" java-version-a "$A" "$R" "$J/bin/java" -version
"$T/run_traced.sh" java-version-b "$A" "$R" "$J/bin/java" -version
xcrun clang -arch arm64 -O1 -I"$J/include" -I"$J/include/darwin" "$WT/fixtures/handoff/jni-entry.c" -o "$W/jni-entry" && \
  "$T/run_traced.sh" jni-entry "$A" "$R" "$W/jni-entry" "$J/lib/server/libjvm.dylib"
"$T/run_traced.sh" plutil-lint "$E" "$R" "$W/bins/plutil-arm64e" -lint "$W/in.plist"
"$T/run_traced.sh" tar-list "$E" "$R" "$W/bins/tar-arm64e" -tvf "$W/tarsrc.tar"
"$T/run_traced.sh" tar-extract "$E" "$R" "$W/bins/tar-arm64e" -xf "$W/tarsrc.tar" -C "$W/tarout-$$"
echo "$R"
