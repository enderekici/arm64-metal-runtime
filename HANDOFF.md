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
including invalid-host/flavor/count and rights cleanup controls. Isolated
allowlisted raw-core candidate `8b67998` matched native byte-for-byte with
status 0 and no diagnostics on its raw and stack fixtures. Full four-flag libz
on the virtualized stack still exits 1 at `___pthread_init.cold.2` with
`BUG IN LIBPTHREAD: Token from the kernel is 0`; startup x3's Apple vector is
empty. The candidate is in progress. These focused cases do not establish full
libz or general Mach IPC compatibility.

## Combined typed interpreter qualification

The typed-memory and W/X `CBZ`/`CBNZ` work from `c153ae1`, plus typed ADD/SUB
immediate support from `49fec2e` (integrated as `520ad41`), was verified at
source checkpoint `a76cfd9`. The full regression gate exited 0 with 41 logged
native parity lines, a direct data-auth positive control, and 2,048 native/GPU
function matches. `verify-interpreter` exited 0. The typed stack-memory,
25-operation branch, and ADD/SUB entry fixtures matched native and translated
output/status; each entry returned 42. The direct interpreter probe reported
one pipeline compile, zero runtime source compiles, and 29 dispatches, including
W/X ADD/SUB-immediate operations without flag writes. The four-packet ADD/SUB
entry fixture exited 42.

Read-only-store and unmapped-load controls reached the interpreter checks and
reported guest memory faults at PC `0x42e0` / address `0x42d8` and PC `0x42dc` /
address `0x0`, respectively. These assert nonzero guest exits and the expected
fault diagnostics; they do not establish native signal-delivery parity. The
interpreter remains opt-in.

The corrected packaged-entry smoke exited 42 with the AOT pipeline and
four-packet interpreter dispatch. (An earlier invocation used the wrong fixture
filename; the corrected invocation used the same archive.) Runtime SHA-256
before and after the gates was
`3c7aa6168a45dc764eac5e25db26da5707b242514e391f67780202162b8b70d7`;
interpreter metallib SHA-256 before and after was
`0925cb290eb76c041c1821ac29cc6f588fc5f7d4f376ad344d4368d5bc838b15`. The
archive SHA-256 is
`ce43a81c30ee7d7bbc3cd787be987f4b80f3390ab3d321e0d2508bc3ae545ae2`; the tested
archive is copied to `.build/arm64-metal-cpp-macos.tar.gz`. Receipts are under
`/tmp/metal-add-sub-final.ukFP8v`, with interpreter fixtures in
`source/.build/cpp/interpreter-verify.xsQ1cO`.

Demand-cache retry and native signal delivery remain separate checks. General
unchanged-app compatibility remains open; these results cover only named
fixtures, exact artifacts, and recorded configurations.

## Synchronous guest-fault package oracle

C++ synchronous fault handling at source `aa19e669` passed regression and
interpreter gates (both status 0). The follow-up `4435d175` changes only the
fixture and Makefile. The extracted package oracle matched native output
`sync-faults-ok`, status 0, with empty stderr. It exercises a SIGBUS write to a
read-only page, handler `mprotect` retry and resumed write, then a SIGILL trap
whose handler advances the guest PC by four bytes.

The fixture now obtains page size with `sysconf(_SC_PAGESIZE)` and prints a
stable marker. This replaced an unsupported direct `getpagesize` import in the
fixture; the initial package command also named the wrong fixture, then the
corrected command passed using the same archive. No runtime-source change was
needed for those corrections. Runtime SHA-256 before and after was
`b76c0bdbfb64deac6c7f3f4dcd8c830bc36ece5233157498ac7fe5c90e093e67`; interpreter
metallib SHA-256 before and after was
`0925cb290eb76c041c1821ac29cc6f588fc5f7d4f376ad344d4368d5bc838b15`. The
verified archive SHA-256 is
`ed1554731cddeeaef0a265106655cd260f65c6f0386e69b27853057174cfd703`; copied to
`.build/arm64-metal-cpp-macos.tar.gz`. Receipts are in
`/tmp/metal-fault-gate.khFglN`.

This qualifies only the named synchronous-fault oracle and its corrected
packaged launch, not general Darwin signal delivery.

## Evidence locations

The [loader probe sources](fixtures/handoff/README.md) are included in the repository.

[LEDGER.md](LEDGER.md) has the chronological decisions and proof boundaries. Recent local receipts are indexed by `/tmp/arm64-metal-align-current`; its value is the scratch root. Relevant pointer files include `cpp-fcmp-frintp-gettid-batch-path`, `cpp-high-named-load-path`, `cpp-alias-overlap-path`, `cpp-noload-active-path`, and `cpp-dynamic-jvm-inspect-path`. Within the last directory, `jni-entry-path` and `cache-provider-path` identify the JNI and libz controls. These local paths are temporary. The build commands and conclusions above remain usable if the scratch directories are removed.

## Final checkpoint verification

See [Current wave](#current-wave) for the latest released interpreter and VM
proof, exact hashes, receipts, and the remaining libz syscall boundary. The
full compatibility matrix and its proof limits are in
[COMPATIBILITY.md](COMPATIBILITY.md).
