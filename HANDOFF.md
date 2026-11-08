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

The integrated guest VM range change at source 8bfa6f6 passed the strict
C++ app build and full verification gate. The test runner SHA-256 before and
after the gate was
3162adc7a229d9e27578ca134d7e6c98d1f5b51ef40059830b43d57413bd9e45.
The gate completed 42 launch comparisons and 2,048 function runs. The packaged
archive's launcher digest matched the tested runtime. Receipts are under
/tmp/metal-vm-integrated.C6e4d6.

The false-green Makefile wiring is fixed. The Swift compiler recipe now
always builds SWIFT_RUNTIME. Previously, overriding RUNTIME for a C++ gate
could bind the Swift recipe to that path and overwrite the binary under test.
Now RUNTIME can select the already-built C++ executable for verify without
changing the Swift build target. Keep the runtime path and digest attached to
every gate result.

The app gate remains bounded and serial. It compares native and guest output,
diagnostics, and exit status for plutil, tar, both cached-libz controls and
optional JNI. Its receipts pin the runtime digest. The plutil trace shows
kernel compilation continuing late into the run before timeout; its result
remains unresolved. ARM64_METAL_TRACE samples compile/dispatch/host-service
events, then emits periodic progress plus image-load, initializer, and raw
syscall boundaries.

Both libz-root and libz-provider reach
__dyld_call_with_writable_tpro_memory after reading dyld4::gAPIs. The latest
trace reports guest 134, startup=0, prefix=0, target=0x44b0, LR=0x10089a95c
in libsystem_platform. The callback token authenticates, but no callback runs.
The native late path rejects this request after libSystem initialization
and terminates with SIGABRT. Preserve that behavior. Do not invoke the callback
after initialization or relax the guard. Investigate initialization ordering
and deferred system roots to reproduce native behavior.

Candidate commit 00541caf adds typed MOVZ/RET handling, with a fixed GPU
interpreter regression test still in progress. Astra recommends a precompiled
GPU interpreter as the eventual default while retaining translation during
migration and for later hot code. Migrate one typed instruction slice at a
time, sharing GPU semantics between interpreter and translator. Compare native,
translator, and interpreter behavior before expanding. This improves coverage
work but does not fill missing dyld, Darwin, GUI, or JIT behavior.

The JIT candidate at b545de0 passes its positive RW-to-RX call and
revocation tests in isolation. Its full gate is running. This does not establish
whole-app compatibility; retain the exact gate artifact and result separately.

The project goal remains broad compatibility for unchanged macOS applications,
with guest instructions translated for GPU execution and CPU work limited to
host services. Astra's direction is to grow the interpreter from observed
application behavior and real loader state. Use native evidence to define the
TPRO callback/permission contract, reuse the guest image/callback machinery,
and keep constructor ordering, publication, rollback, and pointer checks
consistent. Keep the full goal intact: CLI and VM gates are milestones, not
the application-compatibility finish line.

## Evidence locations

The [loader probe sources](fixtures/handoff/README.md) are included in the repository.

[LEDGER.md](LEDGER.md) has the chronological decisions and proof boundaries. Recent local receipts are indexed by `/tmp/arm64-metal-align-current`; its value is the scratch root. Relevant pointer files include `cpp-fcmp-frintp-gettid-batch-path`, `cpp-high-named-load-path`, `cpp-alias-overlap-path`, `cpp-noload-active-path`, and `cpp-dynamic-jvm-inspect-path`. Within the last directory, `jni-entry-path` and `cache-provider-path` identify the JNI and libz controls. These local paths are temporary. The build commands and conclusions above remain usable if the scratch directories are removed.

## Final checkpoint verification

The integrated VM runtime passed the strict C++ app build and full gate on
8 November 2026. Runtime digest matched before and after:
3162adc7a229d9e27578ca134d7e6c98d1f5b51ef40059830b43d57413bd9e45.
The gate completed 42 launch comparisons and 2,048 function runs. The archive
contains the same launcher digest. Receipts are under
/tmp/metal-vm-integrated.C6e4d6.

The latest plutil trace showed kernel compilation continuing late into its
bounded run before timeout. Both libz controls reach the same TPRO path. The
trace reports guest 134, startup=0, prefix=0, target=0x44b0, LR=0x10089a95c in
libsystem_platform. The authenticated callback token does not lead to callback
execution. Native late behavior rejects after libSystem initialization and
terminates with SIGABRT. Preserve it. Investigate initialization ordering and
deferred system roots, without calling the callback late or relaxing the guard.
Typed MOVZ/RET commit 00541caf and its fixed GPU interpreter test are WIP.

Astra recommends growing a precompiled GPU interpreter as the eventual default,
keeping the translator during migration and for hot code, and sharing GPU
instruction semantics across both paths. The interpreter regression should
compare native, translator, and interpreter results. This does not replace the
full goal of general unchanged-app compatibility or solve other missing
subsystems.

Anonymous JIT transaction support and protection-revocation/partial-unmap
changes are integrated on the branch. The positive RW-to-RX fixture matches
native. MAP_JIT and whole-app use remain open. General unchanged-app
compatibility remains the project goal.
