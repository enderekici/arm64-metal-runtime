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

Release source 9252d86 is integrated at aa4fa1c. It passed both release gates
on one stable runtime. The runtime SHA-256 before and after was
2f800e32089c805835cc60c62df21c802b49e29bb6e0211eea1130b1c99d8567.
The packaged metallib SHA-256 was
7dcb6055879dee609f199a23f705bce98df9dc36973aff7529239c8b41214ec9.
The gates recorded 42 launch comparisons and 2,048 function runs. The archive
SHA-256 is
17d9c280790308f1aee2cf1389cf71858106425a0fedcf7b2c3043dcabb70be9.
Receipts are under /tmp/move-wide-release-gate.4pSw6y.

The standalone interpreter used 10 dispatches, one precompiled pipeline, and
zero runtime Metal source compilations. A typed MOVN/MOVK entry fixture matches
native, translator, and interpreter, returns exit 42, and produces packed value
3. The interpreter remains opt-in and has only its qualified typed coverage.
The extracted archive includes the adjacent metallib, and its opt-in launcher
ran a normal Mach-O fixture to exit 42. A missing packaged .metallib correctly
fails even when a source override is supplied. This protects the AOT package
contract; it is not general application compatibility.

The TPRO investigation now matches direct import 134 between native and guest
at 6d5ae31. The full libz path has reached mach_msg2 for host_info, trap ID
200, flavor 5, count 8. Its focused GPU control is in flight. Native
verification is also tracking raw XNU trap paths host-self (-29), task-self
(-28), and pid-for-task (-46). Confirm each native ABI/result before relying
on those mappings. Keep the full libz root/provider cases unresolved until the
native differential and unchanged app replay pass. Preserve the earlier
confirmed late TPRO behavior: after libSystem initialization, native rejects
and terminates with SIGABRT. Do not call the callback late or relax the guard.

VM deallocation across allocations at deb0734 matches the focused native/GPU
fixture exactly. The combined multi-allocation fixture at 224a7c9 is pending
the full gate. CFG interpreter work at 50740ab is isolated and not release
qualified. Keep its branch/fault behavior separate from the passing MOVN/MOVK
typed slice until its own differential and both gates pass.

The full goal remains broad compatibility for unchanged macOS applications,
with guest instructions executing on GPU and CPU services confined to host
boundaries. The interpreter stays opt-in while typed coverage grows. CLI,
VM, packaging and interpreter passes are milestones, not broad compatibility.

## Evidence locations

The [loader probe sources](fixtures/handoff/README.md) are included in the repository.

[LEDGER.md](LEDGER.md) has the chronological decisions and proof boundaries. Recent local receipts are indexed by `/tmp/arm64-metal-align-current`; its value is the scratch root. Relevant pointer files include `cpp-fcmp-frintp-gettid-batch-path`, `cpp-high-named-load-path`, `cpp-alias-overlap-path`, `cpp-noload-active-path`, and `cpp-dynamic-jvm-inspect-path`. Within the last directory, `jni-entry-path` and `cache-provider-path` identify the JNI and libz controls. These local paths are temporary. The build commands and conclusions above remain usable if the scratch directories are removed.

## Final checkpoint verification

Release source 9252d86, integrated at aa4fa1c, passed both release gates on the
same stable runtime. Runtime SHA-256 before and after:
2f800e32089c805835cc60c62df21c802b49e29bb6e0211eea1130b1c99d8567.
Packaged metallib SHA-256:
7dcb6055879dee609f199a23f705bce98df9dc36973aff7529239c8b41214ec9.
The gates recorded 42 launch comparisons and 2,048 function runs. Archive
SHA-256:
17d9c280790308f1aee2cf1389cf71858106425a0fedcf7b2c3043dcabb70be9.
Receipts: /tmp/move-wide-release-gate.4pSw6y.

The interpreter ran the MOVN/MOVK entry path across native, translator and
GPU interpreter with matching exit 42 and packed value 3. It used 10
dispatches, one pipeline, and zero runtime source compiles. The extracted
archive opt-in path used its adjacent metallib and exited 42 on a normal
Mach-O fixture. Missing packaged metallib is rejected even with a source
override.

TPRO direct import 134 parity is recorded at 6d5ae31. Full libz next observed
mach_msg2 host_info (200, flavor 5, count 8); focused GPU test is pending.
VM cross-allocation deallocation matches its focused fixture; the combined
multi-allocation gate is pending. CFG interpreter branch handling remains
isolated. Broad application compatibility remains open.
