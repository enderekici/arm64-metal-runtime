# Loader probes

Build these from the repository root. Use the same compiled probe for native and Metal runs. Set `JDK_HOME` to the installed JDK you are qualifying.

```sh
mkdir -p .build/handoff-probes
out="$PWD/.build/handoff-probes"
src=fixtures/handoff
xcrun clang -Wall -Wextra -Werror "$src/cache-root.c" -o "$out/cache-root"
xcrun clang -Wall -Wextra -Werror -dynamiclib "$src/cache-provider.c" -lz -o "$out/libprovider.dylib"
xcrun clang -Wall -Wextra -Werror "$src/cache-provider-main.c" -o "$out/cache-provider"
xcrun clang -Wall -Wextra -Werror "$src/kdebug-typefilter.c" -o "$out/kdebug-typefilter"
xcrun clang -Wall -Wextra -Werror "$src/dyld-interposition.c" -o "$out/dyld-interposition"
xcrun clang -Wall -Wextra -Werror "$src/mach-reply-port.c" -o "$out/mach-reply-port"
xcrun clang -Wall -Wextra -Werror "$src/commpage-page-shift.c" -o "$out/commpage-page-shift"
xcrun clang -Wall -Wextra -Werror "$src/mach-vm-partial-deallocate.c" \
  -o "$out/mach-vm-partial-deallocate"
xcrun clang -Wall -Wextra -Werror -I "$JDK_HOME/include" -I "$JDK_HOME/include/darwin" \
  "$src/jni-entry.c" -o "$out/jni-entry"

"$out/cache-root"
"$out/cache-provider" "$out/libprovider.dylib"
"$out/kdebug-typefilter"
"$out/dyld-interposition"
"$out/mach-reply-port"
"$out/commpage-page-shift"
"$out/mach-vm-partial-deallocate"
"$out/jni-entry" "$JDK_HOME/lib/server/libjvm.dylib"
```

For a Metal comparison, prefix each invocation with `env ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 .build/cpp/arm64-metal launch`. Keep these runs separate from the unchanged application replays.

## Unchanged application parity gate

Build the C++ runtime with `make -C cpp app METAL_CPP=/path/to/metal-cpp`, then run the bounded app probes from the repository root:

```sh
make verify-apps
```

The default cases are valid and malformed `plutil`, deterministic `tar -tf`, and cache-only/dynamic-provider libz loading. Select a subset with `APP_VERIFY_CASES`, for example `make verify-apps APP_VERIFY_CASES='plutil tar'`. Add `jni` when qualifying an installed JDK by setting `JDK_HOME` to its home directory and including `jni` in the case list. `APP_VERIFY_RUNTIME` selects the frozen C++ launcher, and `APP_VERIFY_RESULTS` selects the parent directory for unique per-run receipts. Set `APP_VERIFY_TIMEOUT_SECONDS` to change the per-process timeout. The gate compares stdout, stderr, and exit status, stores the runtime SHA-256, and fails on timeout or runtime mutation.

## Generated-code permission and rewrite probe

The JIT probe writes a two-instruction ARM64 function into an anonymous mapping,
switches it from RW to RX, executes it, switches back to RW, rewrites its return
value, and executes it again. It also checks that RWX `mmap` and `mprotect`
requests fail with `EACCES`.

The positive case protects individual pages in a three-page mapping, rewrites
one executable page, then unmaps it while calling code from the surviving
executable page. Optional negative cases attempt to execute after returning
code memory to RW or after unmapping it. Native execution faults with SIGBUS
for RW and SIGSEGV after unmapping. The translated runtime must reject both
without executing stale code.

```sh
src=fixtures/handoff/jit-rewrite.c
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 "$src" -o .build/handoff-probes/jit-rewrite
.build/handoff-probes/jit-rewrite
.build/cpp/arm64-metal launch .build/handoff-probes/jit-rewrite
.build/handoff-probes/jit-rewrite rw-fault
.build/handoff-probes/jit-rewrite unmapped-fault
```

Expected output from both runs is
`first=42 second=81 third=73 survivor=81 wx=13/13`, with exit status 0.
Each negative case prints `calling revoked code` before the native fault. The
translated runtime reports a code rejection instead of reproducing Unix signal
delivery.
