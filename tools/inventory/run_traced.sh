#!/bin/sh
# usage: run_traced.sh TAG DYLIB OUTDIR cmd...   runs cmd under the tracer; decodes to OUTDIR/TAG.json
set -u
TAG=$1; DYLIB=$2; OUTDIR=$3; shift 3
D=$(cd "$(dirname "$0")" && pwd)
GEN=${GEN:-/Users/ender/code/evamcep/arm64-metal-work/logs/trace/ip}
mkdir -p "$OUTDIR/$TAG"
# SIP strips DYLD_* across platform binaries (perl), so the variables are set inside perl just before exec.
perl -e '$ENV{DYLD_INSERT_LIBRARIES}=shift; $ENV{TRACE_OUT}=shift; $ENV{TRACE_TAG}=shift; alarm 90; exec @ARGV' "$DYLIB" "$OUTDIR/$TAG/raw" "$TAG" "$@" > "$OUTDIR/$TAG/stdout" 2> "$OUTDIR/$TAG/stderr"
echo "exit=$?" > "$OUTDIR/$TAG/status"
for f in "$OUTDIR/$TAG"/raw.[0-9]*[0-9]; do
  [ -e "$f" ] && python3 -I "$D/trace_report.py" "$GEN/names.tsv" "$f" "$f.json"
done
