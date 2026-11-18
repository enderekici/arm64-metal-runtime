#!/bin/sh
set -eu
unset ARM64_METAL_TRACE ARM64_METAL_TRACE_INTERPRETER_PACKETS

runtime=$1
cfinv=$2
flagm=$3
reserved=$4
library=$5
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

printf '%s\n' '[{"words":[],"seed":0}]' >"$receipts/input"
printf '%s\n' '[{"result":42,"words":[]}]' >"$receipts/expected"
unset ARM64_METAL_INTERPRETER ARM64_METAL_INTERPRETER_LIBRARY ARM64_METAL_INTERPRETER_SOURCE
if [ ! -f "$(dirname "$runtime")/resources/interpreter/AOT_REQUIRED" ]; then
    export ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$library"
fi

check_positive() {
    fixture=$1
    name=$2
    opcode=$3
    packets=$4
    expect_exit 42 "$name-native" "$fixture"
    expect_exit 42 "$name-translated" env ARM64_METAL_INTERPRETER=0 "$runtime" launch "$fixture"
    cmp "$receipts/$name-native.out" "$receipts/$name-translated.out"
    cmp "$receipts/$name-native.err" "$receipts/$name-translated.err"

    expect_exit 0 "$name-function" "$runtime" run "$fixture" main "$receipts/input"
    cmp "$receipts/expected" "$receipts/$name-function.out"
    test ! -s "$receipts/$name-function.err"

    expect_exit 42 "$name-aot" env ARM64_METAL_TRACE=1 ARM64_METAL_TRACE_INTERPRETER_PACKETS=1 "$runtime" launch "$fixture"
    cmp "$receipts/$name-native.out" "$receipts/$name-aot.out"
    awk -v opcode="detail=$opcode" -v expected="$packets" '
        /event=interpreter-packet / && $NF == opcode { matched++ }
        /event=dispatch-start / { dispatches++ }
        /event=interpreter-dispatch / { interpreted++ }
        /event=compile-start / { compiled++ }
        END { exit !(matched == expected && dispatches == 1 && interpreted == 1 && !compiled) }
    ' "$receipts/$name-aot.err"
}

check_positive "$cfinv" cfinv 45 2
check_positive "$flagm" flagm 46 19

check_reserved() {
    name=$1
    opcode=$2
    shift 2
    expect_exit 1 "$name-translated" env ARM64_METAL_INTERPRETER=0 "$runtime" launch "$reserved" "$@"
    expect_exit 1 "$name-aot" "$runtime" launch "$reserved" "$@"
    for mode in translated aot; do
        test ! -s "$receipts/$name-$mode.out"
        grep -Eq "^arm64-metal: Unsupported ARM64 instruction $opcode at __text\+0x[0-9a-f]+$" "$receipts/$name-$mode.err"
    done
    expect_exit 1 "$name-function" "$runtime" translate "$reserved" "reserved_$name"
    grep -Fq "Unsupported ARM64 instruction $opcode" "$receipts/$name-function.err"
}

check_reserved rmif 0xba000410
check_reserved setf 0x3a00080c extra
echo 'CFINV, RMIF and SETF matched native NZCV in translated and AOT execution. Reserved encodings rejected.'
