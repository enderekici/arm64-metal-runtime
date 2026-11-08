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

CFG budget fallback source `1d9bffc` was released in `aba6a2c`. The
interpreter-typed miss now preserves the block budget when falling back to
translation. Its focused proof recorded 13 dispatches; extracted-archive
interpreter smoke covered branch result 7 and mixed result 42.

Raw VM protection source `2136057` passed the full launch/function/AOT gates:
42 launch comparisons, 2,048 function runs, and extracted archive smoke. The
native and GPU protection fixture matched stdout, stderr, and status, including
invalid target `268435459`, overflow result 4, and unmapped result 1. Runtime
SHA-256 was `c59447c4a34f2afa9359f109d053de000cc179186c72113bac24bc60dcf2c386`;
metallib SHA-256 was
`c977849dba7896b7536b14efb5ba501764bfd8024cb6f6f62ae5649d5b272bf4`.
Archive SHA-256 was
`a19af8d8b637d2a4383d845cf1a8aa9a27ca464a136179d4e06004e9e614ea4c`; extracted
smoke returned `mixed42`. Receipts: `/tmp/metal-vm-protect-gate.7ep6a4`.

Focused Mach host-info and port-release cases from `dcc1488` match native,
including invalid-host/flavor/count and rights cleanup controls. The combined
libz startup advances through the four recorded syscall flags and now stops at
raw `SYS_sysctl` (202), PC `0x10084a458`; native ABI is proven, guest handling
is not implemented. Receipt: `/tmp/metal-ipc-combined-gate.W8VmaB`. Typed GPU
memory helper work is in progress. Do not claim full libz or general Mach IPC
compatibility from these focused cases.

## Combined interpreter memory and conditional-branch qualification

The combined typed-memory and W/X `CBZ`/`CBNZ` work from `c153ae1` was
repaired and verified at exact source `3c824ed`. The full regression gate
exited 0 with 41 logged native parity lines, a direct data-auth positive
control, and 2,048 native/GPU function matches. `verify-interpreter` exited 0.
The typed stack-memory fixture and the 25-operation branch fixture matched
native and translated output/status; each returned 42. The standalone AOT
interpreter probe reported one pipeline compile, zero runtime source compiles,
and 25 dispatches.

Read-only-store and unmapped-load controls reached the interpreter checks and
reported guest memory faults at PC `0x42e0` / address `0x42d8` and PC `0x42dc` /
address `0x0`, respectively. These assert nonzero guest exits and the expected
fault diagnostics; they do not establish native signal-delivery parity. The
interpreter remains opt-in.

The packaged smoke exited 42. Its trace showed the precompiled interpreter
pipeline and a two-operation interpreter dispatch, followed by a translated
compile and dispatch. Runtime SHA-256 before and after the gates was
`addb7d90b52d07c8e7d0d52b6b36f955a2afd7881d97227b3036cb40ea22b88c`; interpreter
metallib SHA-256 was
`1caf266a865338dbe461092a96073e6caee7030b3621ae451f6ee9dac09e8e50`. The
archive SHA-256 is
`f89d77a0db30a7dc82197328da2daf74d959ea88c3a8761ba884724fa56aa395`; the tested
archive is copied to `.build/arm64-metal-cpp-macos.tar.gz`. Receipts are under
`/tmp/metal-memory-cbz-repaired.dgc6AW`, with interpreter fixtures in
`source/.build/cpp/interpreter-verify.k0vPu7`.

Demand-cache retry and native signal delivery remain separate checks. General
unchanged-app compatibility remains open; these results cover only named
fixtures, exact artifacts, and recorded configurations.

## Evidence locations

The [loader probe sources](fixtures/handoff/README.md) are included in the repository.

[LEDGER.md](LEDGER.md) has the chronological decisions and proof boundaries. Recent local receipts are indexed by `/tmp/arm64-metal-align-current`; its value is the scratch root. Relevant pointer files include `cpp-fcmp-frintp-gettid-batch-path`, `cpp-high-named-load-path`, `cpp-alias-overlap-path`, `cpp-noload-active-path`, and `cpp-dynamic-jvm-inspect-path`. Within the last directory, `jni-entry-path` and `cache-provider-path` identify the JNI and libz controls. These local paths are temporary. The build commands and conclusions above remain usable if the scratch directories are removed.

## Final checkpoint verification

See [Current wave](#current-wave) for the latest released interpreter and VM
proof, exact hashes, receipts, and the remaining libz syscall boundary. The
full compatibility matrix and its proof limits are in
[COMPATIBILITY.md](COMPATIBILITY.md).
