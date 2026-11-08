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

## Current wave

The VM multi-allocation release candidate at source 224a7c9 passed both gates.
Its code tree matches the VM changes at 8647989; the tree difference is limited
to HANDOFF.md and COMPATIBILITY.md. The runtime SHA-256 before and after the
gate was
410dfba09f8dde6cadba9b2751bac252abb7ae03b62253cfd569a7519db22047.
The metallib SHA-256 was
7dcb6055879dee609f199a23f705bce98df9dc36973aff7529239c8b41214ec9.
Both gates recorded 42 launch comparisons and 2,048 function runs. Receipts
are under /tmp/metal-vm-multi-gate.HuuxCB.

The multi-allocation VM fixture's native and GPU stdout, stderr, and status
match exactly. The extracted archive smoke used the adjacent AOT library and
returned exit status 42. Its trace reports interpreter-block-ready detail=3.
Archive SHA-256:
596af44896f4af94c30ce87da505088d3e0fdc7884d5961d0f3077ca9def2015.
The interpreter remains opt-in.

The focused host_info candidate at cad3d149 matches native for exact and
reduced flags. Full libz startup still stops at raw Darwin trap -18; that
candidate is in progress. The earlier observed request was mach_msg2 host_info
(ID 200, flavor 5, count 8). Do not claim full libz compatibility from the
focused host_info match.

VM deallocation across allocations matches the native/GPU fixture. Keep the
full gate evidence attached to the exact runtime hash above.

CFG candidate f1ddb46 passed earlier gates, then exposed a confirmed
double-charge when an interpreter-typed operation misses and falls back to an
untyped translated block. Hold that candidate until the counter behavior is
fixed and the full gates rerun. A prior green result does not qualify the
current CFG path.

General unchanged-app compatibility remains open. These VM, package, and
interpreter results cover only named fixtures and runtime artifacts.

## Evidence locations

The [loader probe sources](fixtures/handoff/README.md) are included in the repository.

[LEDGER.md](LEDGER.md) has the chronological decisions and proof boundaries. Recent local receipts are indexed by `/tmp/arm64-metal-align-current`; its value is the scratch root. Relevant pointer files include `cpp-fcmp-frintp-gettid-batch-path`, `cpp-high-named-load-path`, `cpp-alias-overlap-path`, `cpp-noload-active-path`, and `cpp-dynamic-jvm-inspect-path`. Within the last directory, `jni-entry-path` and `cache-provider-path` identify the JNI and libz controls. These local paths are temporary. The build commands and conclusions above remain usable if the scratch directories are removed.

## Final checkpoint verification

The VM multi-allocation release source 224a7c9 passed both gates with runtime
SHA-256 410dfba09f8dde6cadba9b2751bac252abb7ae03b62253cfd569a7519db22047 and
metallib SHA-256 7dcb6055879dee609f199a23f705bce98df9dc36973aff7529239c8b41214ec9.
The gates recorded 42 launch comparisons and 2,048 function runs. The
multi-allocation fixture matched native stdout, stderr and status. The extracted
archive smoke exited 42; its trace reports interpreter-block-ready detail=3.
Archive digest:
596af44896f4af94c30ce87da505088d3e0fdc7884d5961d0f3077ca9def2015.
Receipts: /tmp/metal-vm-multi-gate.HuuxCB.

The host_info focused candidate at cad3d149 matches native exact/reduced flags.
Full libz startup stops at raw trap -18 and its candidate remains in progress.
CFG candidate f1ddb46 is held for a confirmed typed-to-untyped miss double-charge.
Neither result establishes broad application compatibility.
