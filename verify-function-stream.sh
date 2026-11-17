#!/bin/sh
set -eu
runtime=$1
guest=$2
out=$3
mkdir -p "$out"

printf '%s\n%s\n' \
    '[{"words":[1,2,3],"seed":7}]' \
    '[{"words":[1,2,3],"seed":7},{"words":[],"seed":42}]' \
    | "$runtime" run-stream "$guest" update_words >"$out/results"
printf '%s\n' \
    '[{"result":54,"words":[3,4,40]}]' \
    '[{"result":54,"words":[3,4,40]},{"result":42,"words":[]}]' \
    >"$out/expected"
cmp "$out/expected" "$out/results"

if printf '\n' | "$runtime" run-stream "$guest" update_words >"$out/empty-line.out" 2>"$out/empty-line.err"; then
    echo 'run-stream accepted an empty line' >&2
    exit 1
fi
test ! -s "$out/empty-line.out"
grep -q 'Stream line 1:' "$out/empty-line.err"

if : | "$runtime" run-stream "$guest" update_words >"$out/empty-input.out" 2>"$out/empty-input.err"; then
    echo 'run-stream accepted empty input' >&2
    exit 1
fi
test ! -s "$out/empty-input.out"
grep -q 'Stream input is empty' "$out/empty-input.err"

if printf '%s\n%s\n' '[{"words":[],"seed":0}]' 'not json' \
    | "$runtime" run-stream "$guest" update_words >"$out/malformed.out" 2>"$out/malformed.err"; then
    echo 'run-stream accepted malformed JSON' >&2
    exit 1
fi
printf '%s\n' '[{"result":0,"words":[]}]' >"$out/first-only"
cmp "$out/first-only" "$out/malformed.out"
grep -q 'Stream line 2: Invalid invocation JSON' "$out/malformed.err"

if printf '%s\n%s\n' '[{"words":[1,2],"seed":42}]' '[{"words":[9],"seed":0}]' \
    | "$runtime" run-stream "$guest" read_second_word >"$out/failed.out" 2>"$out/failed.err"; then
    echo 'run-stream accepted a failed guest invocation' >&2
    exit 1
fi
printf '%s\n' '[{"result":2,"words":[1,2]}]' >"$out/first-only"
cmp "$out/first-only" "$out/failed.out"
grep -q 'Stream line 2: Invocation 0: guest memory out of bounds' "$out/failed.err"

echo 'run-stream distinct batches, empty/malformed input, and guest failure passed'
