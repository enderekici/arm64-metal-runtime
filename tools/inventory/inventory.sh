#!/bin/sh
# Re-run the whole service-gap inventory. usage: inventory.sh WORKTREE   (no GPU, no sudo)
# Output: $W/inventory/gaps.md (+ gaps.json, handled.json, static_svc.json, ...), raw traces under $W/logs/trace/runs-<epoch>.
set -eu
WT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
W=${W:-/Users/ender/code/evamcep/arm64-metal-work}
T=$WT/tools/inventory; INV=$W/inventory; L=$W/logs/trace
mkdir -p "$INV/dsc" "$L/ip"
# 1. dyld-cache images (text bytes, exports, com.apple.* strings) via the runtime's own SharedCache reader
xcrun clang++ -std=c++20 -O1 -I"$WT/cpp" "$T/dsc_dump.cpp" "$WT/cpp/shared_cache.cpp" -o "$L/dsc-dump"
"$T/dump_images.sh" "$L/dsc-dump" "$INV/dsc" 2>/dev/null
# 2. static: SVC sites, kernel call graph, MIG tables, closure imports/strings, runtime handled set
J=$HOME/Library/Java/JavaVirtualMachines/azul-25/Contents/Home
python3 -I "$T/static_scan.py" "$INV/dsc" "$INV/static_svc.json" "$J/bin/java" \
  "$J"/lib/libjli.dylib "$J"/lib/libjava.dylib "$J"/lib/libzip.dylib "$J"/lib/libverify.dylib "$J"/lib/libnio.dylib \
  "$J"/lib/libnet.dylib "$J"/lib/libjsig.dylib "$J"/lib/libjimage.dylib "$J"/lib/server/libjvm.dylib /usr/bin/plutil /usr/bin/tar
python3 -I "$T/kcallgraph.py" "$INV/dsc" "$INV/static_svc.json" "$INV/kgraph.json"
python3 -I "$T/migdefs.py" "$INV/mig.json"
python3 -I "$T/imports.py" "$INV"
python3 -I "$T/handled.py" "$WT" "$INV/handled.json"
# 3. native: generate + build the interposer, run the controls and the four targets
python3 -I "$T/interpose/gen_stubs.py" "$INV/dsc" "$L/ip"
GEN=$L/ip "$T/interpose/build.sh" arm64 "$L/ip/trace-arm64.dylib"
GEN=$L/ip "$T/interpose/build.sh" arm64e "$L/ip/trace-arm64e.dylib"
RUNS=$("$T/run_all.sh" "$WT" "$L" | tail -1)
# 4. join
python3 -I "$T/gaps.py" "$L" "$RUNS" "$INV"
echo "report: $INV/gaps.md   runs: $RUNS"
