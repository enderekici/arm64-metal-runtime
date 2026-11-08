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

Interpreter candidate 7e4656f passed both release gates against one stable
runtime. The executable SHA-256 before and after qualification was
d299993b613d3de4e457c2bab276ff3cfa3bfedf1a1af6cf8d67b8d5d203ec83.
The packaged Metal library SHA-256 was
ee48b85b1e4e4b9e71feb838c81a0f978d610b58af0df98009166f254faffcea.
The gate recorded 42 launch comparisons and 2,048 function runs. Receipts are
under /tmp/metal-release-gate.5rP5Do.

The standalone interpreter probe loaded one precompiled pipeline, compiled
zero Metal source files at runtime, and completed four dispatches. The
interpreter remains opt-in with ARM64_METAL_INTERPRETER=1 and handles the
typed MOVZ/RET slice. From the extracted C++ archive, the opt-in launcher
loaded its adjacent interpreter library and ran a normal Mach-O fixture to
exit 42. The archive SHA-256 is
f1c668d27b4846b5c407975ed64f6c7c2b9c94e2acfb05186311dae0cd2d1862.
This qualifies the tested interpreter path and package. It does not establish
general application compatibility or all currently supported translated
instructions in the interpreter. The translator remains available during the
incremental migration.

A separate TPRO candidate is isolated at db2beae. On the prior frozen runtime whose SHA-256 begins deabfe4e, the observed path
advanced to raw Darwin trap -29. Recover the full digest from its receipt
before reusing that result. The latest ProgramVars relocation has not been rerun.
Protected-write behavior and the late guest negative case remain open.
Native verification is tracking the XNU host-self (-29), task-self (-28), and
pid-for-task (-46) paths. Do not treat those mappings as native-qualified
until their ABI and results are compared on the host.

MOVN/MOVK typed-decoder work is separately isolated at ec0dfd7 and has no GPU
qualification yet. Keep it separate from the passing MOVZ/RET interpreter
slice.

The TPRO late path still reads dyld4::gAPIs and reaches
__dyld_call_with_writable_tpro_memory. The prior trace reported guest 134,
startup=0, prefix=0, target=0x44b0, LR=0x10089a95c in libsystem_platform.
Native late behavior rejects after libSystem initialization and terminates
with SIGABRT. Preserve that behavior. Continue checking initialization order,
deferred system roots and the target's permissions without invoking a late
callback or relaxing the guard.

Astra's interpreter direction remains the long-term path: use typed operations
and shared GPU semantics, keep the translator during migration, and compare
native, translator and interpreter outputs for each slice. The full objective
remains general compatibility for unmodified macOS applications, with guest
instructions executing on GPU and CPU services preserving host boundaries.
CLI, VM, package, and interpreter gates are milestones, not completion.

## Evidence locations

The [loader probe sources](fixtures/handoff/README.md) are included in the repository.

[LEDGER.md](LEDGER.md) has the chronological decisions and proof boundaries. Recent local receipts are indexed by `/tmp/arm64-metal-align-current`; its value is the scratch root. Relevant pointer files include `cpp-fcmp-frintp-gettid-batch-path`, `cpp-high-named-load-path`, `cpp-alias-overlap-path`, `cpp-noload-active-path`, and `cpp-dynamic-jvm-inspect-path`. Within the last directory, `jni-entry-path` and `cache-provider-path` identify the JNI and libz controls. These local paths are temporary. The build commands and conclusions above remain usable if the scratch directories are removed.

## Final checkpoint verification

The interpreter candidate 7e4656f passed both release gates on the same
stable runtime. The runtime SHA-256 matched before and after:
d299993b613d3de4e457c2bab276ff3cfa3bfedf1a1af6cf8d67b8d5d203ec83.
The metallib SHA-256 was
ee48b85b1e4e4b9e71feb838c81a0f978d610b58af0df98009166f254faffcea.
The gates recorded 42 launch comparisons and 2,048 function runs. The archive
SHA-256 is
f1c668d27b4846b5c407975ed64f6c7c2b9c94e2acfb05186311dae0cd2d1862.
Receipts are under /tmp/metal-release-gate.5rP5Do.

The standalone interpreter run used one pipeline, zero runtime source
compilations and four dispatches. An extracted archive with the opt-in
interpreter loaded the adjacent metallib and ran a normal Mach-O fixture to
exit 42. MOVN/MOVK GPU execution and broad interpreter coverage remain open.

TPRO candidate db2beae and MOVN/MOVK candidate ec0dfd7 are isolated work.
The prior TPRO path advanced to trap -29, while ProgramVars relocation,
protected-write behavior, and the late guest negative case remain unresolved.
See the Current wave section and COMPATIBILITY.md for the exact proof boundary.
General app compatibility remains the project goal.
