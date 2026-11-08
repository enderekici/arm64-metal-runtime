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

The C++ regression gate passed with a runtime built from source commit
0d64700. The tested launcher SHA-256 is
4bb8a5b116cfa23b8366a76396abfca3ed84502be0d534498958eb7c0b45b0f1.
The full gate recorded 42 native launch comparisons and 2,048 native/Metal
function runs. The packaged C++ launcher SHA-256 matched this tested runtime.

The bounded app gate is make verify-apps / verify-apps.sh. It compares native
and guest stdout, stderr, and status for valid/malformed plutil, tar, cached
libz root/provider, and optional JNI. It records each receipt under a unique
results directory, pins the runtime hash, and serializes guest invocations.
Run it with make verify-apps APP_VERIFY_RUNTIME=/path/to/frozen/arm64-metal
APP_VERIFY_RESULTS=/path/to/results APP_VERIFY_CASES=all. Set JDK_HOME to add
the optional JNI case. make bundle-cpp packages a previously verified C++
launcher. The app-gate receipt records the runtime digest for each comparison.
APP_VERIFY_TIMEOUT_SECONDS sets the per-process timeout. The recent serial
plutil run timed out at 85 seconds; this is unresolved, not a pass.
ARM64_METAL_TRACE=1 records bounded compile, dispatch, and host-service progress
(128 events maximum) to classify stalls and first boundaries.

Commpage parity is integrated at 144a70d. Its focused fixture compares both
page-shift bytes with native value 14. The final libz-root replay used the
exact 144a70d runtime. Native exited 0; guest exited 1 with a guest memory
access fault at address 0, PC 0x100466370, width 0, x10=0, tp=0x500df50.
The normalized PC 0x180466370 is in __dyld_call_with_writable_tpro_memory.
The preceding x0 load reads dyld4::gAPIs. Both page-shift reads passed. The
failure is recorded in /tmp/metal-integrated-final.dah4mT/app-root-correct.
Next compare native callback and permission behavior, then test the existing
guest callback machinery. Do not invent a vtable layout. Libz-root/provider
behavior remains unverified.

The JIT candidate is isolated at 0399c77. Native and Metal both returned
first=42 second=73 wx=13/13, exit 0, and empty stderr for the RW-to-RX call
path. Execute-after-RW negative behavior, unmapping, and MAP_JIT remain open.

A fresh binary built from source 144a70d has SHA-256
37aebd011d25dad0dd05568b6d9ca0a70d0712f5083fdf9923402def8191e29c. Its strict
app build and full make verify both passed, exit 0, with 42 launch comparisons
and 2,048 function runs. This supersedes the older full-gate runtime digest
above. The exact-artifact libz-root replay is in
/tmp/metal-integrated-final.dah4mT/app-root-correct and failed after both
commpage fields at PC 0x100466370, address 0, width 0.

Broader dyld bootstrap, general Java execution, GUI launch/interaction, full
Mach IPC, and compatibility outside named cases remain open. Continue one GPU
application invocation at a time, keep native positive controls, and preserve
stdout, stderr, exit status, timeout/crash receipt, flags, and runtime digest.

## Evidence locations

The [loader probe sources](fixtures/handoff/README.md) are included in the repository.

[LEDGER.md](LEDGER.md) has the chronological decisions and proof boundaries. Recent local receipts are indexed by `/tmp/arm64-metal-align-current`; its value is the scratch root. Relevant pointer files include `cpp-fcmp-frintp-gettid-batch-path`, `cpp-high-named-load-path`, `cpp-alias-overlap-path`, `cpp-noload-active-path`, and `cpp-dynamic-jvm-inspect-path`. Within the last directory, `jni-entry-path` and `cache-provider-path` identify the JNI and libz controls. These local paths are temporary. The build commands and conclusions above remain usable if the scratch directories are removed.

## Final checkpoint verification

The strict C++ app build and full make verify passed for the runtime built
from source commit 0d64700. The gate covered 42 native launch comparisons,
external SIGUSR1, both 1,024-case function gates, and authentication, budget,
bounds, instruction, and malformed-input rejection checks. The tested runtime
SHA-256 is
4bb8a5b116cfa23b8366a76396abfca3ed84502be0d534498958eb7c0b45b0f1.
The packaged C++ launcher digest matched this runtime.

The separate unchanged-app gate now captures bounded native/guest receipts,
validates positive-control status, compares stdout/stderr/status, pins the
runtime digest across the run, and runs guests serially. Valid and malformed
plutil timed out at 85 seconds in the recent serial run. Commpage parity is
integrated at 144a70d and compares both shift bytes with native value 14. The
exact 144a70d libz-root replay matched native status only up to the first
later memory fault at PC 0x100466370, address 0, width 0; both shifts passed.
The latest full make verify for runtime hash
37aebd011d25dad0dd05568b6d9ca0a70d0712f5083fdf9923402def8191e29c passed with
42 launch comparisons and 2,048 function runs. A bounded libz-root replay
against that runtime is in progress. The isolated JIT candidate's positive
RW-to-RX path matches native; negative execute-after-RW, unmap and MAP_JIT
remain open. See Resume and COMPATIBILITY.md.
