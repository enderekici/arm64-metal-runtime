# ARM64-to-Metal handoff

## Scope

This is a local Apple Silicon runtime for unmodified ARM64 and arm64e Mach-O binaries. Guest instructions are decoded to Metal and execute on the GPU. The CPU builds kernels, manages guest state, maps images, and services modeled macOS boundaries. There is no native guest-execution fallback. General application compatibility remains open.

The current implementation is under `cpp/`. The root Swift implementation and fixtures are retained as a reference. The root `make bundle` packages the Swift launcher. Build the current C++ launcher explicitly.

## Build and verify

Requirements are Apple Silicon, Xcode Command Line Tools, and Apple's metal-cpp headers. The last test host was an M4 Pro on macOS 27.0.1 (26A434), Apple clang 21. The local metal-cpp include root was `/tmp/metal-cpp`.

```sh
make -C cpp app METAL_CPP=/path/to/metal-cpp
make verify RUNTIME="$PWD/.build/cpp/arm64-metal"
```

A normal launch is `.build/cpp/arm64-metal launch /bin/cat example.json`. For shared-cache app probes, use:

```sh
env ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 \
  ARM64_METAL_CACHE_BOOTSTRAP=1 ARM64_METAL_CACHE_INITIALIZERS=1 \
  .build/cpp/arm64-metal launch /usr/bin/plutil -lint /path/to/input.plist
```

Capture stdout, stderr, and exit status separately for native and Metal. Freeze the runtime binary, retain its SHA-256, and record launch flags. Keep one live invocation per target and configuration. A process that disappears without a status file has an unknown result.

## What is established

- The ordinary launch gate has covered 42 native comparisons and external SIGUSR1 delivery. The two function gates cover 2,048 native/Metal invocations.
- The loader applies chained and legacy fixups. The installed Azul JVM matched native dyld on all 128,617 legacy rebase slots and 535 bind slots. Strict host staging resolved those slots and 227 stubs and found 8,243 constructor targets. This establishes a staging path for the real dylib.
- Named loads of local dylibs match native on dependency-first and nested constructors, self-reopen, symbol calls, visibility, and caller-relative paths. A required-symbol failure leaves the image hidden and a valid subsequent load succeeds. Cache-only named roots and provider activation reach guest cache initialization. The libz end-to-end controls remain open.
- A 20 MiB named dylib and an 80 MiB subsequent heap allocation preserve both regions' data. Direct Metal reads and host I/O crossing the guarded low backing boundary fail on the corrected geometry.
- ARM64 floating-point controls cover vector FMOV, scalar FDIV, FCMP/FCMPE, FRINTP, and NZCV transfers. The S/D differential matrices matched native result and exception state for their exercised inputs.
- Cached initializer order for the CoreFoundation control matched 385 native constructor addresses across ten images after slide normalization. The runtime models libSystem's own initializer separately.
- Cached pthread detach and raw Darwin gettid match native in their focused controls. Inactive prebound CryptoKit correctly misses `RTLD_NOLOAD`.

## Resume from here

1. Check the latest build result below. Replay Java, the CoreFoundation allocator control, and unchanged `tar` with cached initializers enabled. Their last completed app runs hit the same libdyld wrapper at PC `0x1004618e0`. That wrapper reads dyld's `gAPIs` object, whose slot is zero in the cache and populated by native bootstrap. Verify any narrow host route against native, and keep general dyld bootstrap state as an open compatibility item.
2. Run both valid and malformed `plutil` inputs after the FRINTP changes. The last completed pre-FRINTP runs stopped at `0x1e64c000`. Later plist runs lost their exit receipts.
3. Replay the libz cached-provider and cache-only root controls. They passed staging and reached cache constructors. The observed boundaries were commpage CPU capabilities, Mach `pid_for_task` (-28), and Darwin `kdebug_typefilter` (177). Qualify the raw ABI and result before advancing. Do not infer the syscall from the nearest exported symbol.
4. Run the installed-JVM JNI control once cache provider publication is viable. Native returns `args=0 version=65544 created=0 count=0`. Its earlier Metal process lost its exit receipt. A successful JNI control still leaves complete `java -version` and general Java execution to verify.
5. Remaining subsystem work includes lazy binding and weak-definition coalescing, delayed-image activation, full unload semantics, broader instruction and Darwin ABI coverage, and GUI application startup.

## Evidence locations

The [loader probe sources](fixtures/handoff/README.md) are included in the repository.

[LEDGER.md](LEDGER.md) has the chronological decisions and proof boundaries. Recent local receipts are indexed by `/tmp/arm64-metal-align-current`; its value is the scratch root. Relevant pointer files include `cpp-fcmp-frintp-gettid-batch-path`, `cpp-high-named-load-path`, `cpp-alias-overlap-path`, `cpp-noload-active-path`, and `cpp-dynamic-jvm-inspect-path`. Within the last directory, `jni-entry-path` and `cache-provider-path` identify the JNI and libz controls. These local paths are temporary. The build commands and conclusions above remain usable if the scratch directories are removed.

## Final checkpoint verification

The frozen C++ build passed the strict app compile and full `make verify` on 8 November 2026. The gate included 42 native launch comparisons, external SIGUSR1, both 1,024-case function gates, and the authentication, budget, bounds, instruction, and malformed-input rejection checks.

Eight focused comparisons also matched stdout, stderr, and status on that binary. They cover nested named loads, mixed memory growth, missing-symbol rollback, raw gettid, inactive CryptoKit NOLOAD, named visibility, raw kdebug_typefilter, and the no-interposition dyld query. The syscall 177 fixture verifies a zero-filled typefilter in the tested no-tracing state, unchanged size and errno, and EINVAL/EFAULT carry on invalid outputs. The dyld service models the no-insertion/no-interposition state. The full application and cache-root replays were deferred to the handoff.

The verified local launcher is `.build/cpp/arm64-metal`. Its SHA-256 is `ca8df1b8405726783041791bf81f09e96db1f0d5379b3619364dc75317d4c63d`. Final logs are under `.build/handoff.i0wxRE`. Build products and logs are Git-ignored. The branch is local.
