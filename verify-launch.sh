#!/bin/sh
set -eu
runtime=$1
console=$2
streams=$3
indirect=$4
rebased=$5
linked=$6
region=$7
pipe=$8
out=$9
signals=${10}
vector=${11}
heap=${12}
threads=${13}
memory=${14}

compare() {
    binary=$1
    shift
    native_status=0
    guest_status=0
    "$binary" "$@" <"${fixture:-/dev/null}" >"$out/native.stdout" 2>"$out/native.stderr" || native_status=$?
    "$runtime" launch "$binary" "$@" <"${fixture:-/dev/null}" >"$out/guest.stdout" 2>"$out/guest.stderr" || guest_status=$?
    if [ "$native_status" -ne "$guest_status" ]; then
        printf '%s: native status %s differs from guest status %s\n' "$binary" "$native_status" "$guest_status" >&2
        cat "$out/guest.stderr" >&2
        return 1
    fi
    cmp "$out/native.stdout" "$out/guest.stdout"
    cmp "$out/native.stderr" "$out/guest.stderr"
    printf '%s: stdout, stderr and exit status %s match native\n' "$binary" "$guest_status"
}

compare "$console"
compare "$console" hello
compare "$console" 'two words'
compare "$console" 'çağrı'
compare "$console" ''
compare "$streams"
compare /usr/bin/true
compare /usr/bin/false

compare "$indirect"
compare "$indirect" first
compare /bin/echo
compare /bin/echo hello world
compare /bin/echo -n 'çağrı ☃'
POSIXLY_CORRECT=1 compare /bin/echo 'a\012b'
compare /bin/pwd -P
compare /bin/pwd -L
compare /usr/bin/printenv HOME
compare /usr/bin/printenv ARM64_METAL_NONEXISTENT_TEST_ENV_VAR
compare /usr/bin/dirname '/foo/bar' '/x/y' '/'
compare /usr/bin/basename '/foo/bar.txt' .txt
compare /usr/bin/basename -a '/foo/bar' '/x/y'
printf 'binary input\000more\377\n' >"$out/binary-input"
compare /bin/cat "$out/binary-input"
compare /bin/cat /dev/null
fixture="$out/binary-input" compare /bin/cat

compare "$rebased"
compare "$rebased" argument
compare "$linked"
compare "$linked" argument
compare "$linked" reexport argument
compare "$region"
compare "$pipe"
# Failure should name the missing dependency before any initializer runs.
mkdir -p "$out/no-deps"
cp "$linked" "$out/no-deps/linked"
if "$runtime" launch "$out/no-deps/linked" >"$out/no-deps.stdout" 2>"$out/no-deps.stderr"; then
    printf 'Missing guest dylib was accepted\n' >&2
    exit 1
fi
test ! -s "$out/no-deps.stdout"
grep 'Unable to resolve guest dylib @rpath/libbridge.dylib' "$out/no-deps.stderr"
# Exceeds the largest cksum read buffer and includes nonzero binary bytes.
dd if=/dev/zero of="$out/large-input" bs=1024 count=65 2>/dev/null
cat "$out/binary-input" >>"$out/large-input"
compare /usr/bin/cksum "$out/large-input"
compare /usr/bin/cksum -o 1 "$out/large-input"
compare /usr/bin/cksum -o 2 "$out/large-input"
compare /usr/bin/cksum -o 3 "$out/large-input"
fixture="$out/binary-input" compare /usr/bin/cksum

fixups=$(otool -l "$rebased" | awk '/cmd LC_DYLD_CHAINED_FIXUPS/ { found = 1 } found && /dataoff/ { print $2; exit }')
test -n "$fixups"
cp "$rebased" "$out/invalid-fixups"
printf '\001' | dd of="$out/invalid-fixups" bs=1 seek="$fixups" conv=notrunc 2>/dev/null
if "$runtime" launch "$out/invalid-fixups" >"$out/fixup.stdout" 2>"$out/fixup.stderr"; then
    printf 'Invalid dyld fixups were accepted\n' >&2
    exit 1
fi
test ! -s "$out/fixup.stdout"
grep 'Unsupported dyld chained fixup header' "$out/fixup.stderr"

compare "$signals"
compare "$vector"
compare "$heap"
compare "$threads"
ARM64_METAL_ENV_TEST=guest-vector compare "$memory" environment

external_signal() {
    label=$1
    shift
    "$@" >"$out/$label-signal.stdout" 2>"$out/$label-signal.stderr" &
    child=$!
    tries=0
    until grep -qx ready "$out/$label-signal.stdout"; do
        if ! kill -0 "$child" 2>/dev/null || [ "$tries" -ge 200 ]; then
            kill -TERM "$child" 2>/dev/null || :
            wait "$child" || :
            cat "$out/$label-signal.stderr" >&2
            return 1
        fi
        tries=$((tries + 1))
        sleep 0.02
    done
    kill -USR1 "$child"
    wait "$child"
}
external_signal native "$signals" external
external_signal guest "$runtime" launch "$signals" external
cmp "$out/native-signal.stdout" "$out/guest-signal.stdout"
cmp "$out/native-signal.stderr" "$out/guest-signal.stderr"
printf 'External SIGUSR1 handler and resume match native\n'
