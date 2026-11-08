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
xcrun clang -Wall -Wextra -Werror -I "$JDK_HOME/include" -I "$JDK_HOME/include/darwin" \
  "$src/jni-entry.c" -o "$out/jni-entry"

"$out/cache-root"
"$out/cache-provider" "$out/libprovider.dylib"
"$out/kdebug-typefilter"
"$out/dyld-interposition"
"$out/jni-entry" "$JDK_HOME/lib/server/libjvm.dylib"
```

For a Metal comparison, prefix each invocation with `env ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 .build/cpp/arm64-metal launch`. Keep these runs separate from the unchanged application replays.

## Unchanged application parity gate

Build the C++ runtime with `make -C cpp app METAL_CPP=/path/to/metal-cpp`, then run the bounded app probes from the repository root:

```sh
make verify-apps
```

The default cases are valid and malformed `plutil`, deterministic `tar -tf`, and cache-only/dynamic-provider libz loading. Select a subset with `APP_VERIFY_CASES`, for example `make verify-apps APP_VERIFY_CASES='plutil tar'`. Add `jni` when qualifying an installed JDK by setting `JDK_HOME` to its home directory and including `jni` in the case list. `APP_VERIFY_RUNTIME` selects the frozen C++ launcher, and `APP_VERIFY_RESULTS` selects the parent directory for unique per-run receipts. Set `APP_VERIFY_TIMEOUT_SECONDS` to change the per-process timeout. The gate compares stdout, stderr, and exit status, stores the runtime SHA-256, and fails on timeout or runtime mutation.
