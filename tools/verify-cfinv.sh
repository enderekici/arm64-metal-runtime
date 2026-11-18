#!/bin/sh
set -eu
unset ARM64_METAL_TRACE ARM64_METAL_TRACE_INTERPRETER_PACKETS

runtime=$1
cfinv=$2
rmif=$3
library=$4
receipts=$(mktemp -d "$(dirname "$cfinv")/cfinv.XXXXXX")
echo "CFINV receipts $receipts"

expect_exit() {
    expected=$1
    receipt=$2
    shift 2
    status=0
    "$@" >"$receipts/$receipt.out" 2>"$receipts/$receipt.err" || status=$?
    if [ "$status" -ne "$expected" ]; then
        echo "$receipt returned $status, expected $expected" >&2
        cat "$receipts/$receipt.err" >&2
        exit 1
    fi
}

expect_exit 42 native "$cfinv"
expect_exit 42 rmif-native "$rmif"
expect_exit 42 translated env ARM64_METAL_INTERPRETER=0 "$runtime" launch "$cfinv"
cmp "$receipts/native.out" "$receipts/translated.out"
cmp "$receipts/native.err" "$receipts/translated.err"

printf '%s\n' '[{"words":[],"seed":0}]' >"$receipts/input"
expect_exit 0 function "$runtime" run "$cfinv" main "$receipts/input"
printf '%s\n' '[{"result":42,"words":[]}]' >"$receipts/expected"
cmp "$receipts/expected" "$receipts/function.out"
test ! -s "$receipts/function.err"

expect_exit 1 rmif-translated env ARM64_METAL_INTERPRETER=0 "$runtime" launch "$rmif"
unset ARM64_METAL_INTERPRETER ARM64_METAL_INTERPRETER_LIBRARY ARM64_METAL_INTERPRETER_SOURCE
if [ ! -f "$(dirname "$runtime")/resources/interpreter/AOT_REQUIRED" ]; then
    export ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$library"
fi
expect_exit 42 aot env ARM64_METAL_TRACE=1 ARM64_METAL_TRACE_INTERPRETER_PACKETS=1 "$runtime" launch "$cfinv"
cmp "$receipts/native.out" "$receipts/aot.out"
awk '/event=interpreter-packet .* detail=45$/ { packets++ }
     /event=dispatch-start / { dispatches++ }
     /event=interpreter-dispatch / { interpreted++ }
     /event=compile-start / { compiled++ }
     END { exit !(packets == 2 && dispatches == 1 && interpreted == 1 && !compiled) }' "$receipts/aot.err"

expect_exit 1 rmif-aot "$runtime" launch "$rmif"
for mode in translated aot; do
    test ! -s "$receipts/rmif-$mode.out"
    grep -Eq '^arm64-metal: Unsupported ARM64 instruction 0xba0007e1 at __text\+0x[0-9a-f]+$' "$receipts/rmif-$mode.err"
done
echo 'CFINV matched native in translated function, process and AOT. RMIF rejected in both launch modes.'
