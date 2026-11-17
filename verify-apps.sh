#!/bin/sh
set -eu

usage() {
    printf 'Usage: %s RUNTIME RESULTS_ROOT CASE...\n' "$0" >&2
    printf 'Cases: brk dyld-sdk plutil tar libz-root libz-provider jni\n' >&2
    exit 2
}

[ "$#" -ge 3 ] || usage
runtime=$1
results_root=$2
shift 2
if [ "$#" -eq 1 ] && [ "$1" = all ]; then
    set -- brk dyld-sdk plutil tar libz-root libz-provider
    if [ -n "${JDK_HOME:-}" ]; then
        set -- "$@" jni
    fi
fi
[ -x "$runtime" ] || { printf 'Runtime is not executable: %s\n' "$runtime" >&2; exit 2; }
mkdir -p "$results_root"
results_root=$(cd "$results_root" && pwd -P)
run_dir=$(mktemp -d "$results_root/run.XXXXXX")
probe_dir=$run_dir/probes
mkdir -p "$probe_dir"
timeout_seconds=${APP_VERIFY_TIMEOUT_SECONDS:-180}
case "$timeout_seconds" in
    ''|*[!0-9]*) printf 'APP_VERIFY_TIMEOUT_SECONDS must be a positive integer\n' >&2; exit 2 ;;
esac
[ "$timeout_seconds" -gt 0 ] || { printf 'APP_VERIFY_TIMEOUT_SECONDS must be positive\n' >&2; exit 2; }

runtime_hash=$(shasum -a 256 "$runtime" | awk '{print $1}')
printf '%s\n' "$runtime_hash" > "$run_dir/runtime.sha256"
printf 'runtime=%s\nsha256=%s\nresults=%s\n' "$runtime" "$runtime_hash" "$run_dir" > "$run_dir/run.txt"

run_bounded() {
    run_label=$1
    shift
    marker=$run_dir/$run_label.timed-out
    rm -f "$marker"
    (exec "$@") >"$run_dir/$run_label.stdout" 2>"$run_dir/$run_label.stderr" &
    run_command_pid=$!
    (
        sleep "$timeout_seconds"
        if kill -0 "$run_command_pid" 2>/dev/null; then
            run_state=$(ps -p "$run_command_pid" -o stat= 2>/dev/null || :)
            case "$run_state" in
                *Z*) ;;
                '') ;;
                *)
                    : > "$marker"
                    kill -TERM "$run_command_pid" 2>/dev/null || :
                    sleep 1
                    kill -KILL "$run_command_pid" 2>/dev/null || :
                    ;;
            esac
        fi
    ) &
    run_watchdog_pid=$!
    run_command_status=0
    wait "$run_command_pid" || run_command_status=$?
    kill "$run_watchdog_pid" 2>/dev/null || :
    wait "$run_watchdog_pid" 2>/dev/null || :
    if [ -f "$marker" ]; then
        printf 'TIMEOUT (%ss)\n' "$timeout_seconds" > "$run_dir/$run_label.status"
        printf '%s timed out after %ss\n' "$run_label" "$timeout_seconds" >&2
        return 1
    fi
    printf '%s\n' "$run_command_status" > "$run_dir/$run_label.status"
    return 0
}

compare() {
    cmp_label=$1
    cmp_expected_status=$2
    cmp_binary=$3
    shift 3
    cmp_native_status=0
    cmp_guest_status=0
    run_bounded "$cmp_label.native" "$cmp_binary" "$@" || return 1
    cmp_native_status=$(cat "$run_dir/$cmp_label.native.status")
    if [ "$cmp_native_status" != "$cmp_expected_status" ]; then
        printf '%s positive control failed: native status %s, expected %s\n' \
            "$cmp_label" "$cmp_native_status" "$cmp_expected_status" >&2
        return 1
    fi
    run_bounded "$cmp_label.guest" env ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 \
        ARM64_METAL_CACHE_BOOTSTRAP=1 ARM64_METAL_CACHE_INITIALIZERS=1 \
        "$runtime" launch "$cmp_binary" "$@" || return 1
    cmp_guest_status=$(cat "$run_dir/$cmp_label.guest.status")
    if [ "$cmp_native_status" != "$cmp_guest_status" ]; then
        printf '%s status differs: native=%s guest=%s\n' "$cmp_label" "$cmp_native_status" "$cmp_guest_status" >&2
        return 1
    fi
    cmp "$run_dir/$cmp_label.native.stdout" "$run_dir/$cmp_label.guest.stdout"
    cmp "$run_dir/$cmp_label.native.stderr" "$run_dir/$cmp_label.guest.stderr"
    printf '%s matches native (status %s)\n' "$cmp_label" "$cmp_guest_status"
}

for case_name do
    case "$case_name" in
        brk)
            [ -x "$(xcrun --find clang)" ] || { printf 'Xcode clang is unavailable\n' >&2; exit 2; }
            xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -pedantic \
                fixtures/handoff/brk-sigtrap.c -o "$probe_dir/brk-sigtrap"
            compare brk-sigtrap 0 "$probe_dir/brk-sigtrap"
            xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -pedantic \
                fixtures/handoff/brk-default-termination.c -o "$probe_dir/brk-default"
            compare brk-default 133 "$probe_dir/brk-default"
            ;;
        dyld-sdk)
            [ -x "$(xcrun --find clang)" ] || { printf 'Xcode clang is unavailable\n' >&2; exit 2; }
            xcrun clang -Wall -Wextra -Werror fixtures/handoff/dyld-sdk-at-least.c \
                -o "$probe_dir/dyld-sdk-at-least"
            xcrun clang -Wall -Wextra -Werror fixtures/handoff/dyld-sdk-at-least-direct.c \
                -o "$probe_dir/dyld-sdk-at-least-direct"
            compare dyld-sdk-dlsym 0 "$probe_dir/dyld-sdk-at-least"
            compare dyld-sdk-direct 0 "$probe_dir/dyld-sdk-at-least-direct"
            ;;
        plutil)
            plutil=/usr/bin/plutil
            [ -x "$plutil" ] || { printf 'Missing %s\n' "$plutil" >&2; exit 2; }
            cat > "$probe_dir/valid.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>runtime</key><string>metal</string><key>count</key><integer>3</integer></dict></plist>
PLIST
            cat > "$probe_dir/malformed.plist" <<'PLIST'
<?xml version="1.0"?><plist version="1.0"><dict><key>broken</dict></plist>
PLIST
            compare plutil-valid 0 "$plutil" -lint "$probe_dir/valid.plist"
            compare plutil-malformed 1 "$plutil" -lint "$probe_dir/malformed.plist"
            ;;
        tar)
            tar=/usr/bin/tar
            [ -x "$tar" ] || { printf 'Missing %s\n' "$tar" >&2; exit 2; }
            mkdir -p "$probe_dir/tar-input/subdir"
            printf 'metal runtime\n' > "$probe_dir/tar-input/readme.txt"
            printf '\000\001\377\n' > "$probe_dir/tar-input/subdir/payload.bin"
            (cd "$probe_dir" && "$tar" -cf "$probe_dir/input.tar" tar-input)
            test -s "$probe_dir/input.tar"
            compare tar-list 0 "$tar" -tf "$probe_dir/input.tar"
            mkdir -p "$probe_dir/native-extract" "$probe_dir/guest-extract"
            run_bounded tar-extract.native "$tar" -xf "$probe_dir/input.tar" \
                -C "$probe_dir/native-extract" || exit 1
            test "$(cat "$run_dir/tar-extract.native.status")" = 0
            run_bounded tar-extract.guest env ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 \
                ARM64_METAL_CACHE_BOOTSTRAP=1 ARM64_METAL_CACHE_INITIALIZERS=1 \
                "$runtime" launch "$tar" -xf "$probe_dir/input.tar" -C "$probe_dir/guest-extract" || exit 1
            test "$(cat "$run_dir/tar-extract.guest.status")" = 0
            cmp "$run_dir/tar-extract.native.stdout" "$run_dir/tar-extract.guest.stdout"
            cmp "$run_dir/tar-extract.native.stderr" "$run_dir/tar-extract.guest.stderr"
            cmp "$probe_dir/tar-input/readme.txt" "$probe_dir/native-extract/tar-input/readme.txt"
            cmp "$probe_dir/tar-input/readme.txt" "$probe_dir/guest-extract/tar-input/readme.txt"
            cmp "$probe_dir/tar-input/subdir/payload.bin" \
                "$probe_dir/native-extract/tar-input/subdir/payload.bin"
            cmp "$probe_dir/tar-input/subdir/payload.bin" \
                "$probe_dir/guest-extract/tar-input/subdir/payload.bin"
            cmp "$probe_dir/native-extract/tar-input/readme.txt" "$probe_dir/guest-extract/tar-input/readme.txt"
            cmp "$probe_dir/native-extract/tar-input/subdir/payload.bin" \
                "$probe_dir/guest-extract/tar-input/subdir/payload.bin"
            printf 'tar extracted file contents match native\n'
            ;;
        libz-root|libz-provider)
            [ -x "$(xcrun --find clang)" ] || { printf 'Xcode clang is unavailable\n' >&2; exit 2; }
            xcrun clang -Wall -Wextra -Werror fixtures/handoff/cache-root.c -o "$probe_dir/cache-root"
            xcrun clang -Wall -Wextra -Werror -dynamiclib fixtures/handoff/cache-provider.c -lz -o "$probe_dir/libprovider.dylib"
            xcrun clang -Wall -Wextra -Werror fixtures/handoff/cache-provider-main.c -o "$probe_dir/cache-provider"
            if [ "$case_name" = libz-root ]; then
                compare libz-root 0 "$probe_dir/cache-root"
            else
                compare libz-provider 0 "$probe_dir/cache-provider" "$probe_dir/libprovider.dylib"
            fi
            ;;
        jni)
            : "${JDK_HOME:?Set JDK_HOME to the JDK being qualified}"
            [ -f "$JDK_HOME/include/jni.h" ] || { printf 'Missing JNI headers under %s\n' "$JDK_HOME" >&2; exit 2; }
            [ -f "$JDK_HOME/lib/server/libjvm.dylib" ] || { printf 'Missing libjvm under %s\n' "$JDK_HOME" >&2; exit 2; }
            xcrun clang -Wall -Wextra -Werror -I "$JDK_HOME/include" -I "$JDK_HOME/include/darwin" \
                fixtures/handoff/jni-entry.c -o "$probe_dir/jni-entry"
            compare jni-entry 0 "$probe_dir/jni-entry" "$JDK_HOME/lib/server/libjvm.dylib"
            ;;
        *)
            printf 'Unknown case: %s\n' "$case_name" >&2
            usage
            ;;
    esac
done

final_hash=$(shasum -a 256 "$runtime" | awk '{print $1}')
[ "$runtime_hash" = "$final_hash" ] || { printf 'Runtime changed during app verification\n' >&2; exit 1; }
printf 'app verification passed\nresults=%s\n' "$run_dir"
