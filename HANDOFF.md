# ARM64-to-Metal handoff

## Scope

This is a local Apple Silicon runtime for unmodified ARM64 and arm64e Mach-O binaries. Guest instructions are decoded to Metal and execute on the GPU. The CPU builds kernels, manages guest state, maps images, and services modeled macOS boundaries. There is no native guest-execution fallback. General application compatibility remains open.

The C++ implementation is under `cpp/`. The default root `make` target builds
the C++ launcher and native fixtures, `make verify` runs the default gates, and
`make bundle` verifies and packages the C++ launcher at the original archive
path. `make bundle-cpp` retains the separately named C++ archive. Python
supports verification tools. The packaged runtime uses Foundation and Metal.

Use `make verify-cfinv` to check carry inversion on the GPU and strict rejection of RMIF. The former development-only CFINV CPU companion has been removed. For an unchanged macOS CLI that creates and appends files, run `make verify-apps APP_VERIFY_CASES=tee APP_VERIFY_RUNTIME=<packaged-launcher>`. The measured package and receipt are recorded in `COMPATIBILITY.md`.

## Build and verify

Requirements are Apple Silicon, Xcode Command Line Tools, and Apple's metal-cpp headers. The last test host was an M4 Pro on macOS 27.0.1 (26A434), Apple clang 21. The local metal-cpp include root was `/tmp/metal-cpp`.

```sh
make METAL_CPP=/path/to/metal-cpp
make verify RUNTIME="$PWD/.build/cpp/arm64-metal"
make bundle METAL_CPP=/path/to/metal-cpp
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
status 0 and no diagnostics on its raw and stack fixtures. The latest unchanged Java 26 and libz probes have advanced past raw syscall
206 and now stop at raw syscall 3418. A focused clock raw-service fixture
matches native/GPU. Six local IPC fixtures passed in isolation; these bounded
results do not establish full Java/libz startup or general Mach IPC behavior.

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

## Guest service registry, pthread metadata, and printf

Source candidate `7409b52` centralizes the host-service registry, adds integer
`printf` formats, and covers width/flag behavior. Regression and interpreter
gates both exited 0. The independent interpreter receipt audit checked all
seven native/translated/interpreter status triples, stdout and traces, both
negative diagnostics, and the 29-operation probe. Runtime SHA-256 before and
after was `246bfec54dc3a6101fc15b8f5b896505d1b4ab16a60289a54dcfc1aad0a27be0`;
interpreter metallib SHA-256 was
`0925cb290eb76c041c1821ac29cc6f588fc5f7d4f376ad344d4368d5bc838b15`.

The base host-service dispatch map matched exactly; the newly added dual-view
pthread stack getter IDs 180/181 are excluded from that base-map parity claim.
A separate focused GPU fixture on pre-combined source `f320b4d` matched native
for `pthread_get_stackaddr_np` and `pthread_get_stacksize_np` across main and
worker threads. This covers those queried stack bounds only.

The printf fixture's native, translated, and guest output matched for its
recorded integer conversions, widths, and flags; the packaged printf exited 0.
The mixed packaged interpreter case exited 42 after AOT dispatch and translated
fallback. The exact archive SHA-256 is
`d20849525c4e1b1d3a3574de46faa8b38c78438d80c22f05eaf931408d4ae04f`; copied to
`.build/arm64-metal-cpp-macos.tar.gz`. Receipts are under
`/tmp/metal-format-gate.wQXVyM` and `/tmp/metal-stack-registry-gate.zgf0Ez`.

Scalar and pair demand-cache retry fixtures each return status 32 in native,
translated, and interpreter modes. Both traces record cache fault then cache
resident at the same PC before retry; fail-fast gates check the transition.
These results prove only the named fixtures, not general demand-cache behavior.

No broad application compatibility claim follows from these service and
formatting fixtures.

## Typed ADD/SUB flags, SP, and fallback boundary

The ISA release checkpoint `6bda384` adds typed ADDS/SUBS, CMP/CMN, SP/WSP
arithmetic, and flag-state checks. Regression, interpreter, and demand-cache
gates exited 0; the package smoke returned 42. Runtime SHA-256 before and after
was `a7e78d4bcfce5897b1f6bc699c9038f358b5766bd6c88a3e15961b3a527e6dd5`;
interpreter metallib SHA-256 was
`429d29b64ecd526d6f2cf80a52fdc7a835231d3fd1aa1fd1714bc5baf8f19231`. The
standalone interpreter probe reported 39 dispatches.

Two probe instruction encodings were corrected: shifted `SUB SP` from
`0xd10043ff` to `0xd14007ff`, and `ADD X0, SP, #1` from `0x910003e0` to
`0x910007e0`. The entry fixture `add-sub-sp-flags-main.s` performs SP add/sub,
then CMP and `B.ne`. The gate asserts the typed interpreter block dispatch at
`0x42d8` (8 packets), completion at `0x42ec` (3 remaining), and translated
compile beginning at that same PC. Thus the fixture checks flag preservation
across the typed-to-translated boundary. `B.ne` is translated fallback here;
typed `B.cond` is not established by this result.

The packaged trace shows that same boundary, then the translated fallback, and
exit 42. Archive SHA-256 is
`be8657af660dec6c94e82e376a330c6b5a02554852c114cfe53fca47b031e785`; copied to
`.build/arm64-metal-cpp-macos.tar.gz`. Receipts: `/tmp/metal-isa-release-proof.KpUcrT`.

## MAP_JIT synthetic-package qualification

The JIT release candidate `9a9b255` models guest MAP_JIT eligibility, write
protection, synchronous SIGBUS information, and opposite-mode thread resume.
The timing-independent gate-only commit `de2ca71` leaves runtime and metallib
bytes unchanged. Strict regression, interpreter, and demand-cache gates exited
0. Four extracted-package native/GPU synthetic fixtures matched: executable
JIT call status 0 including code mutation result 41 to 42; no-entitlement denial
status 21; SIGBUS retry status 77 with signal 10, code 1, and matching fault
address; and opposite-mode thread-resume status 0.

The packaged controls exercise the permission decision and resume sequence on
synthetic binaries, not an unchanged application. Runtime SHA-256 was
`7449e1418602c458574876cc8c6be333c20502bb282f699e985a03d10f314ae7`; interpreter
metallib SHA-256 was
`429d29b64ecd526d6f2cf80a52fdc7a835231d3fd1aa1fd1714bc5baf8f19231`. Archive
SHA-256 is
`adc0db4137d93888a480d616fee21f7e20273a53194441d35384aebbd0436302`; copied to
`.build/arm64-metal-cpp-macos.tar.gz`. Receipts are under
`/tmp/metal-jit-order-release.dR6ja0`; the old/fixed scheduler comparison is
in `/tmp/map-jit-switch-proof`.


The [loader probe sources](fixtures/handoff/README.md) are included in the repository.

[LEDGER.md](LEDGER.md) has the chronological decisions and proof boundaries. Recent local receipts are indexed by `/tmp/arm64-metal-align-current`; its value is the scratch root. Relevant pointer files include `cpp-fcmp-frintp-gettid-batch-path`, `cpp-high-named-load-path`, `cpp-alias-overlap-path`, `cpp-noload-active-path`, and `cpp-dynamic-jvm-inspect-path`. Within the last directory, `jni-entry-path` and `cache-provider-path` identify the JNI and libz controls. These local paths are temporary. The build commands and conclusions above remain usable if the scratch directories are removed.

## Pair memory and typed B.cond qualification

Source checkpoint `8841503` adds typed LDP/STP and conditional branch execution.
Regression, interpreter, and demand-cache gates exited 0. The pair-memory
fixture matched native and translated results and returned 42 after a 26-packet
interpreter dispatch. The NZCV fixture matched all three modes and returned 42;
it exercised three interpreter blocks of 128, 128, and 32 packets with no
translated compile fallback.

The first packaged B.cond command used the wrong fixture filename. The corrected
`interpreter-bcond-nzcv-main` invocation with the same archive exited 42. Its
trace confirms the three AOT dispatches. This is the named W/X condition-code
fixture, not a claim for the full ARM64 ISA. The separate SP/CMP fallback fixture
places unsupported MADD before translated `B.ne`; the gate asserts dispatch
ends at `0x42ec` and translated compilation starts at that same PC, preserving
NZCV across the boundary.

Runtime SHA-256 before and after was
`6410960da1edd3c86e7d45df359dd73ad5221e7b8719b5ed767931fc52ab91cf`;
interpreter metallib SHA-256 was
`55f6b96e9288afa3ac88b3d0291393a452607aa75c1e65c1bc221fc513fa933c`. Archive
SHA-256 is
`ac33951bc7aadd4b74aab32a3e6497316536cb9df256eb68b045ea7ed8da02ed`; copied to
`.build/arm64-metal-cpp-macos.tar.gz`. Receipts are under
`/tmp/metal-jit-pair-bcond-release.m7yDQP`.

## Final checkpoint verification

See [Current wave](#current-wave) and the ISA sections above for latest
interpreter/VM proof, exact hashes, receipts, and remaining syscall boundaries. The
full compatibility matrix and its proof limits are in
[COMPATIBILITY.md](COMPATIBILITY.md).

## Fortified memcpy and SIGTRAP handler package proof

Release source `0a32d1f` was integrated as `f8ad262`. The regression,
interpreter, and demand-cache gates exited 0. From the extracted package, the
valid `__memcpy_chk` fixture matched native with status 0 and output
`copy=fort result=1`. The overflow fixture's installed `SA_SIGINFO` handler
received SIGTRAP (signal 5, code 0), recorded a nonzero PC equal to `si_addr`
in each process, and exited 77 in both native and guest runs. Addresses are
process-local; byte-for-byte address equality is not expected. Exit 77 is the
fixture handler's explicit exit, not proof of default unhandled-signal wait
semantics or general signal parity.

Regression, interpreter, and exact package receipts are under
`/tmp/metal-memcpy-pair-release.wiQMMa` and
`/tmp/metal-memcpy-pair-exact.kG8S2R`. Runtime SHA-256 before and after was
`a5004f55fdafc10f8b7418ea8e1f2a1e97dd748cb70a4afb3dc89af59bf87f4c`;
metallib SHA-256 was
`55f6b96e9288afa3ac88b3d0291393a452607aa75c1e65c1bc221fc513fa933c`. The
copied tested archive SHA-256 is
`787ce90ad393c4b123c44b4e94481f4eaf3f22d66d63d13455ae3e6623956c8f`.

## Cross-stack permission package proof

Source `8963a80` (`1cef7d7` plus the completion-revocation fix) permits a
thread to write into another currently live guest stack interval and revokes
that permission when the stack owner finishes. The pre-existing linear-heap
write policy is unchanged. Regression, interpreter, and demand-cache gates
exited 0. The extracted package's `pthread-stack-cross-write` fixture exited 0
with byte-identical native and guest stdout (`cross-stack writes ok`) and empty
stderr. The focused exact-baseline native/guest run at
`/tmp/metal-stack-fortified-exact.QXB4K4` also matched output and status.

This is a focused live-stack interval/lifetime case, not full pthread or stack
permission parity. Runtime SHA-256 before and after was
`e34a9c0b8d6982c4ebf100454331ab3984d07fb9986d67c97b25c6d1f0691cc8`;
interpreter metallib SHA-256 was
`55f6b96e9288afa3ac88b3d0291393a452607aa75c1e65c1bc221fc513fa933c`. The
promoted archive SHA-256 is
`2ae3bb42387bac78ac60d7515334d9180609a2927130f037d2f73bec27c738e6`.
Release receipts: `/tmp/metal-stack-fortified-release.aYBcnE`.

## CSEL and scalar indexed-memory interpreter release

The release source `e61cf67` adds the AArch64 conditional-select family and
scalar indexed memory operations to the opt-in GPU interpreter. Regression,
interpreter, and demand-cache gates exited 0. Extracted-package CSEL and scalar
indexed-memory fixtures each exited 42 after respectively 81 and 34 interpreter
packet dispatches, with no translated compile fallback. Two fixture-only
repairs replaced register-form compares with equivalent CMN/CMP immediate
forms to keep the cases on the intended typed path; the result is bounded to
those corrected fixtures and instruction sequences.

The unchanged Java 26/libz startup probe now reaches `_dyld_sdk_at_least` and
stops there; its isolated fix is pending. Focused semaphore and clock controls
passed, as did a late Mach GPU control in isolation. These do not establish full
Java/libz startup, general Mach IPC, or broad semaphore parity. Runtime SHA-256
before/after was
`3ecbc57935635857c8b1ad15d9df93c005f232a74480aeefd5f8154c4861c168`;
metallib SHA-256 was
`ac7facd7eaebc4b597764f806d4f4cb346dfff6aa122f4b133022a5bc9c5cabf`. The
promoted archive SHA-256 is
`6ba7f7e0db14a498107d2476fa6365a0f712ca18d00bc89f069a19644bffd80a`.
Receipts: `/tmp/metal-csel-scalar-typed-release.JUQhu9`.

## Shifted-register arithmetic interpreter release

Source `c1c0ff8` adds typed W/X shifted-register ADD/SUB, flag updates, and
CMP/CMN aliases. Strict demand-cache/interpreter gates and full regression
exited 0. The extracted package fixture exited 42 after one 72-packet
interpreter dispatch with no translated compile fallback. Runtime and metallib
hashes were stable across the gate: `c3168cb33edcad2a70227b642c8cb5a6c44c0ce8ff02a6db607dd763053a1618`
and `a8af3e1098d69750a1f2609d1c30a56e14017652b2fd459a480ffdf9f229da34`.
The promoted archive SHA-256 is
`65e9e4b1f2e70f8799fcfdabdd6e86394c928bd590fcb8c28b28fc82575c90c9`.
Receipts: `/tmp/metal-shifted-release.hc5bf0`.

An isolated SDK-bridge candidate changes the Java first stop from null access
to raw `open_nocancel` syscall 398. Exact no-cache SDK replay is pending, so
this is a narrowed diagnostic frontier, not an SDK compatibility result.

## Narrow scalar-memory interpreter release

Source `a72ef6c` adds the named byte/halfword scalar memory forms to the
interpreter: W-register zero-extension and unsigned, unscaled, pre-indexed,
and post-indexed accesses. Strict interpreter/demand-cache and full regression
gates exited 0. The packaged scalar fixture returned 42 after one 67-packet
interpreter dispatch without translated fallback. Byte and halfword demand-cache
retry fixtures returned status 32 in native, translated, and interpreter modes
at the same PC. This is limited to those widths, extension rules, addressing
forms, and fixtures.

Runtime SHA-256 before/after was
`2e480f45aaea854e6febdbfb97a1d7bb0c9fa50e5df1dad02b197b004d5895fb`;
interpreter metallib SHA-256 was
`e60d90491149838020e228e93dd619097ce158f5ad94f0f4f27ebca8f5633fee`. The
promoted archive SHA-256 is
`2299873dd14638809461b549b0c0e02836d926ae65ae143c4b0e3c3a8141404d`.
Receipts: `/tmp/metal-narrow-release.E4BUj0`.

The isolated SDK bridge/read candidate advances the unchanged Java/libz stop
from `open_nocancel` 398 to raw syscall 396. App replay remains pending; this is
not a Java/libz compatibility result.

## Current `codex/tu-thread` checkpoint: `b6e8994`

On Apple M4 Pro / macOS 27.2.1, the exact source passed the launch regression,
interpreter, and demand-cache gates (all status 0), with 42 launch comparisons,
external SIGUSR1, and 2,048 function matches. The extracted-package AOT smoke
returned 42. The extracted raw wall-time control exited 0 with `walltime ok`;
the QoS fixture matched native. The signal fixture matched native EINTR,
handler, and signal-mask behavior (native/guest status 0). Signal receipts are
under `/tmp/metal-thread-resume.E1NxHc/sigsuspend-two-waiters/`; extracted QoS
and AOT receipts are under `/tmp/metal-thread-resume.E1NxHc/qos-package-*` and
`/tmp/metal-thread-resume.E1NxHc/signal-final-package-aot.*`. Runtime SHA-256 is
`50f8d65d3ea6c03b49f8ea0537c429031d9e8601bab74f29cd68f3592713af96`,
interpreter metallib SHA-256 is
`f3c8a415872170216a78cc47af5c9e17545bd629c6ea143ea2741802d82a00fd`, and the
promoted `.build/arm64-metal-cpp-macos.tar.gz` SHA-256 is
`c1b6e9cfb4d8b5052c24aebbf4d8054501926a35dd78f4620c49b2a46a766303`. Build,
gate statuses, and before/after hashes are under
`/tmp/metal-thread-resume.E1NxHc/signal-final*` and
`/tmp/metal-thread-resume.E1NxHc/signal-final.after.sha256`.

The extracted `mainq` fixture matches native stdout (`main queue ran`), empty
stderr, and status 0. Receipts are under
`/tmp/arm64-metal-thread-services.fHD5G1/`.

The earlier `f76a922` guest reached raw
`gettimeofday` (116), then trapped with status 133 at `kevent_id` (375), flags
`0x403` and `EVFILT_WORKLOOP` (-17); it produced no timer ticks. Native LLDB
shows `_dispatch_kq_poll` returning 0 for the owner-address/state request.
Receipts: `/tmp/metal-thread-fc128-fix/timer.guest.*` and
`/tmp/metal-thread-fc128-fix/timer.native.lldb.txt`. The later `b6e8994`
checkpoint retained that failure. The timer candidate subsequently committed as
`33a7d32` passed the unchanged opt-in Objective-C fixture with a warm kernel
cache: both native and guest delivered the callback within the two-second
deadline and exited cleanly. See `COMPATIBILITY.md` for the frozen runtime
identity and exact receipt. Other workloop behavior remains unverified.

General macOS compatibility now proceeds through reusable contract-differential
lanes; full applications are occasional holdouts, not the repeated test loop.
These results qualify only the recorded artifacts, fixtures, flags, and OS
build; full Azul/JNI startup and broad macOS compatibility remain open.

## Promoted local package checkpoint: `66d6005`

Runtime SHA-256 `8877627f32a918002cef03197314bcb9eee1982ef38c9359f2ff6c641b0084a4`
is in the promoted `.build/arm64-metal-cpp-macos.tar.gz` (SHA-256
`365186c907efd4b939c3748822e52a206d0ca198343d28e42e9edc641e089834`). The
`f63f633` manifest produced 21 exact component matches in one immutable-runtime
run: `/tmp/metal-lifecycle-combined.qkAayK/components/run-20261117T001217Z-b0847925/summary.json`.
Build, regression (42 launch comparisons, 2,048 function matches), both
interpreter gates, named Mach memory-entry, static TLS reopen, scoped workloop,
and extracted-package AOT CSEL all returned status 0 under
`/tmp/metal-lifecycle-combined.qkAayK/` (native and extracted CSEL both returned
42). These results cover only the recorded contracts; later process-tree code
is not in this package. Runtime source `ca87080` with manifest `98e6fed` passed
22/22 exact component cases in
`/tmp/metal-next-milestone.GiLP0u/components/run-20261117T010050Z-3b02eba0/summary.json`;
the spawn control also matched native in that wave. The measured scope covers
null spawn actions/attributes, one tracked PID, two waiters, and sibling
release. An earlier pre-fix wait run returned status 1 despite printing
`exited23=0` (`/tmp/metal-next-milestone.GiLP0u/wait-red.*`). SA_RESTART
delivery may defer. The later `6682a34` candidate passed imported `fork()` for
one single-threaded, cache-off guest: inherited pipes, matching child PID, and
isolated private stack, global and heap writes. Raw fork and general fork/exec
remain open. The `062bbb6` package passed both Objective-C framework fixtures;
the guest AppKit window gate remains open. See
[COMPATIBILITY.md](COMPATIBILITY.md) for the contract map and receipts.
