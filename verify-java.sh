#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    printf 'Usage: %s JDK_HOME RUNTIME RESULTS_ROOT\n' "$0" >&2
    exit 2
fi

jdk_home=$1
runtime=$2
results_root=$3
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
timeout_seconds=${APP_VERIFY_TIMEOUT_SECONDS:-300}
[ -x "$runtime" ] || { printf 'Runtime is not executable: %s\n' "$runtime" >&2; exit 2; }
[ -x "$jdk_home/bin/java" ] || { printf 'JDK_HOME is invalid: %s\n' "$jdk_home" >&2; exit 2; }
[ -x "$jdk_home/bin/javac" ] || { printf 'javac is missing from %s\n' "$jdk_home" >&2; exit 2; }

mkdir -p "$results_root"
results_root=$(CDPATH= cd -- "$results_root" && pwd -P)
run_dir=$(mktemp -d "$results_root/java.XXXXXX")
probe_dir=$run_dir/probes
mkdir -p "$probe_dir/classes"
runtime_hash=$(shasum -a 256 "$runtime" | awk '{print $1}')
printf '%s\n' "$runtime_hash" > "$run_dir/runtime.sha256"
printf 'jdk=%s\nruntime=%s\nsha256=%s\nresults=%s\n' \
    "$jdk_home" "$runtime" "$runtime_hash" "$run_dir" > "$run_dir/run.txt"

"$jdk_home/bin/javac" --release 17 -d "$probe_dir/classes" \
    "$repo_root/fixtures/handoff/java/Workload.java" \
    "$repo_root/fixtures/handoff/java/HotWorkload.java"
xcrun clang -Wall -Wextra -Werror -I "$jdk_home/include" \
    -I "$jdk_home/include/darwin" \
    "$repo_root/fixtures/handoff/jni-entry.c" -o "$probe_dir/jni-entry"

run_bounded() {
    run_label=$1
    shift
    timeout_marker=$run_dir/$run_label.timed-out
    "$@" >"$run_dir/$run_label.stdout" 2>"$run_dir/$run_label.stderr" &
    command_pid=$!
    (
        sleep "$timeout_seconds"
        if kill -0 "$command_pid" 2>/dev/null; then
            process_state=$(ps -p "$command_pid" -o stat= 2>/dev/null || :)
            case "$process_state" in
                *Z*|'') ;;
                *)
                    : > "$timeout_marker"
                    kill -TERM "$command_pid" 2>/dev/null || :
                    sleep 1
                    kill -KILL "$command_pid" 2>/dev/null || :
                    ;;
            esac
        fi
    ) &
    watchdog_pid=$!
    command_status=0
    wait "$command_pid" || command_status=$?
    kill "$watchdog_pid" 2>/dev/null || :
    wait "$watchdog_pid" 2>/dev/null || :
    if [ -f "$timeout_marker" ]; then
        printf 'TIMEOUT (%ss)\n' "$timeout_seconds" > "$run_dir/$run_label.status"
        return 1
    fi
    printf '%s\n' "$command_status" > "$run_dir/$run_label.status"
}

run_target() {
    run_label=$1
    shift
    case "$run_label" in
        *.native) run_bounded "$run_label" "$@" ;;
        *)
            run_bounded "$run_label" env ARM64_METAL_TRACE=1 \
                ARM64_METAL_SHARED_CACHE=1 \
                ARM64_METAL_CACHE_DEMAND=1 ARM64_METAL_CACHE_BOOTSTRAP=1 \
                ARM64_METAL_CACHE_INITIALIZERS=1 "$runtime" launch "$@"
            ;;
    esac
}

compare() {
    compare_label=$1
    expected_stdout=$2
    shift 2
    run_target "$compare_label.native" "$@" || return 1
    run_target "$compare_label.guest" "$@" || return 1
    cmp "$run_dir/$compare_label.native.status" "$run_dir/$compare_label.guest.status"
    cmp "$run_dir/$compare_label.native.stdout" "$run_dir/$compare_label.guest.stdout"
    sed '/^arm64-metal trace /d' "$run_dir/$compare_label.guest.stderr" \
        > "$run_dir/$compare_label.guest.stderr.normalized"
    cmp "$run_dir/$compare_label.native.stderr" \
        "$run_dir/$compare_label.guest.stderr.normalized"
    test "$(cat "$run_dir/$compare_label.native.status")" = 0
    test "$(cat "$run_dir/$compare_label.native.stdout")" = "$expected_stdout"
}

for mode in default xint c1 c2; do
    set --
    case "$mode" in
        xint) set -- -Xint ;;
        c1) set -- -XX:TieredStopAtLevel=1 -Xbatch ;;
        c2) set -- -XX:TieredStopAtLevel=4 -Xbatch ;;
    esac
    compare "$mode-workload" 'values=[2, 5, 7, 9] checksum=23' \
        "$jdk_home/bin/java" "$@" -cp "$probe_dir/classes" Workload
    compare "$mode-hot" 'iterations=500000 checksum=4045347071' \
        "$jdk_home/bin/java" "$@" -cp "$probe_dir/classes" HotWorkload
done

for tier in c1 c2; do
    if [ "$tier" = c1 ]; then
        tier_level=1
        compiler=c1
    else
        tier_level=4
        compiler=c2
    fi
    set -- -XX:TieredStopAtLevel="$tier_level" -Xbatch \
        -XX:+UnlockDiagnosticVMOptions -XX:+PrintCompilation -XX:+PrintNMethods \
        -XX:CompileCommand=dontinline,HotWorkload::mix
    profile_label=hot-$tier-compiled
    run_target "$profile_label.native" "$jdk_home/bin/java" "$@" \
        -cp "$probe_dir/classes" HotWorkload || exit 1
    run_target "$profile_label.guest" "$jdk_home/bin/java" "$@" \
        -cp "$probe_dir/classes" HotWorkload || exit 1
    test "$(cat "$run_dir/$profile_label.native.status")" = 0
    test "$(cat "$run_dir/$profile_label.guest.status")" = 0
    python3 - "$run_dir/$profile_label.native.stdout" "$compiler" <<'PY'
import re
import sys

stdout = open(sys.argv[1], encoding="utf-8", errors="replace").read()
compiler = sys.argv[2]
assert "iterations=500000 checksum=4045347071" in stdout
assert re.search(
    rf"Compiled method \({compiler}\).*HotWorkload::mix \(16 bytes\)", stdout
), f"native HotWorkload::mix compilation was not observed at {compiler}"
PY
    python3 - "$run_dir/$profile_label.guest.stdout" \
        "$run_dir/$profile_label.guest.stderr" "$compiler" <<'PY'
import re
import sys

stdout_path, trace_path, compiler = sys.argv[1:]
stdout = open(stdout_path, encoding="utf-8", errors="replace").read()
trace = open(trace_path, encoding="utf-8", errors="replace").read()
assert "iterations=500000 checksum=4045347071" in stdout
header = re.search(
    rf"Compiled method \({compiler}\).*HotWorkload::mix \(16 bytes\)", stdout
)
assert header, f"HotWorkload::mix did not compile with {compiler}"
tail = stdout[header.end():]
range_match = re.search(r"main code\s+\[0x([0-9a-f]+),0x([0-9a-f]+)\]", tail)
assert range_match, "compiled HotWorkload::mix code range missing"
start, end = (int(value, 16) for value in range_match.groups())
assert end > start
events = re.findall(
    r"event=(compile-start|dispatch-start) pc=0x([0-9a-f]+)", trace
)
compiled = any(name == "compile-start" and start <= int(pc, 16) < end
               for name, pc in events)
dispatched = any(name == "dispatch-start" and start <= int(pc, 16) < end
                 for name, pc in events)
assert compiled, "runtime never translated the HotWorkload::mix JIT code range"
assert dispatched, "runtime trace did not observe dispatch in the JIT code range"
PY
    test "$(tail -n 1 "$run_dir/$profile_label.guest.stdout")" = \
        'iterations=500000 checksum=4045347071'
done

compare jni-entry 'args=0 version=65544 created=0 count=0' \
    "$probe_dir/jni-entry" "$jdk_home/lib/server/libjvm.dylib"

final_hash=$(shasum -a 256 "$runtime" | awk '{print $1}')
[ "$runtime_hash" = "$final_hash" ] || { printf 'Runtime changed during Java gate\n' >&2; exit 1; }
printf 'Java execution and JIT translation cases passed\nresults=%s\n' "$run_dir"
