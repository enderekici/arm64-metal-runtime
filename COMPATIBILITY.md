# Compatibility map

Checkpoint is source commit a44fb9c and verified C++ launcher SHA-256
ca8df1b8405726783041791bf81f09e96db1f0d5379b3619364dc75317d4c63d
(macOS 27.0.1, M4 Pro). “Proven” means a recorded native differential passed
for the named case. “Modeled” means the runtime supplies a host/software model
whose recorded cases passed. Neither label claims general macOS compatibility.

For replay commands and scratch evidence pointers, see [HANDOFF.md](HANDOFF.md).

Latest releases are CFG budget fallback source `1d9bffc` in `aba6a2c` and raw
VM protection source `2136057`. The VM artifact passed 42 launch comparisons,
2,048 function runs, and AOT/extracted archive smoke. Runtime digest
`c59447c4a34f2afa9359f109d053de000cc179186c72113bac24bc60dcf2c386`, metallib
digest `c977849dba7896b7536b14efb5ba501764bfd8024cb6f6f62ae5649d5b272bf4`,
and archive digest
`a19af8d8b637d2a4383d845cf1a8aa9a27ca464a136179d4e06004e9e614ea4c`. See
HANDOFF.md for receipts and current proof boundaries.

## Execution, ISA, and process ABI

| Area | State at checkpoint | Source ownership | Next proof |
|---|---|---|---|
| ARM64 guest execution | **Proven, bounded.** Reachable guest blocks translate to Metal; unsupported instructions fail explicitly. No guest CPU fallback. The full ISA is not claimed. | [translate_core.cpp](cpp/translate_core.cpp), [process_decoder.cpp](cpp/process_decoder.cpp), [process_msl.cpp](cpp/process_msl.cpp), [process_runner.cpp](cpp/process_runner.cpp) | Turn each real app’s first unsupported opcode into a native/GPU fixture, then replay the unchanged app. |
| Opt-in AOT GPU interpreter | **Proven for recorded typed operations, budget fallback, stack/pair memory, W/X CBZ/CBNZ and B.cond, W/X ADD/SUB flags, and SP/WSP arithmetic.** Source checkpoint `8841503` passed regression, interpreter, and demand-cache gates; pair-memory and B.cond package fixtures returned 42. | [gpu_interpreter.cpp](cpp/gpu_interpreter.cpp), [gpu_interpreter.metal](cpp/gpu_interpreter.metal), [guest_memory_msl.hpp](cpp/guest_memory_msl.hpp), [process_decoder.cpp](cpp/process_decoder.cpp), [main.cpp](cpp/main.cpp), [Makefile](cpp/Makefile) | B.cond evidence covers the named 128/128/32 packet fixture without compile fallback; pair-memory covers one 26-packet stack fixture. Scalar and pair demand-cache retries each cover one status-32 fixture. The full ISA remains open. |
| Registers, calls, stack, startup | **Proven for tested launch ABI.** LC_MAIN, argc/argv, bounded stack, direct/indirect calls, guest exit and host-call resume pass fixtures. | [process_runner.cpp](cpp/process_runner.cpp), [guest_state.cpp](cpp/guest_state.cpp) | Add an ABI fixture only when an unchanged target exposes a mismatch. |
| SIMD and floating point | **Proven for named matrices and app paths**, including vector memory, conversions, comparisons, FRINTP, FDIV and status flags. Other opcodes and FPCR/FPSR behavior remain open. | [process_decoder.cpp](cpp/process_decoder.cpp), [process_msl.cpp](cpp/process_msl.cpp), [guest_vector_crypto.cpp](cpp/guest_vector_crypto.cpp) | Use the exact failing instruction and operands; compare result bits and flags with native. |
| Syscall/import ABI | **Modeled, selective.** The base host-service dispatch map has exact parity; dual-view pthread stack getter IDs 180/181 are new and excluded from that comparison. Integer printf formats, width and flags match the named fixture and packaged printf. libc and environment behavior remain partial. | [host_service_registry.hpp](cpp/host_service_registry.hpp), [process_decoder.cpp](cpp/process_decoder.cpp), [process_runner.cpp](cpp/process_runner.cpp), [process_host.cpp](cpp/process_host.cpp), [guest_format.cpp](cpp/guest_format.cpp) | Differential-probe each new service ID and format behavior against native, including errors and output buffers. |

## Mach-O, dyld, memory, and pointer authentication

| Area | State at checkpoint | Source ownership | Next proof |
|---|---|---|---|
| Mach-O and image graph | **Proven for tested ARM64/arm64e images and local dylib graphs.** Dependency ordering, selected @rpath forms, exports and constructors have passing fixtures. Format/command coverage is bounded. | [macho.cpp](cpp/macho.cpp), [image_loader.cpp](cpp/image_loader.cpp), [shared_cache.cpp](cpp/shared_cache.cpp) | Preserve the unchanged failing image and identify its exact command/format before extending parsing. |
| Chained/legacy fixups | **Proven for recorded rebase/bind forms.** JVM cache slots and local-load cases were compared with dyld. The parser explicitly rejects pages with multiple chained-fixup starts at [macho.cpp:457](cpp/macho.cpp:457). | [macho.cpp](cpp/macho.cpp), [image_loader.cpp](cpp/image_loader.cpp) | First obtain a real failing image using that encoding; inspect its starts table and compare patched slots with native dyld. |
| Shared cache and residency | **Proven for focused libz, libSystem and framework paths** in resident/demand modes. Not every image or startup protocol is covered. | [shared_cache.cpp](cpp/shared_cache.cpp), [cache_residency.cpp](cpp/cache_residency.cpp), [image_loader.cpp](cpp/image_loader.cpp), [process_runner.cpp](cpp/process_runner.cpp) | Replay unchanged cache-root/provider cases. Trace the first page, binding or initializer whose state differs. |
| Dyld APIs and lifecycle | **Modeled selectively.** Individual queried APIs have native fixtures. Full bootstrap is incomplete; handoff records a cached wrapper reading gAPIs as a prior app stop. Runtime dlopen covers a subset. Full unload, delayed activation, lazy and weak binding remain open. | [image_loader.cpp](cpp/image_loader.cpp), [process_decoder.cpp](cpp/process_decoder.cpp), [process_runner.cpp](cpp/process_runner.cpp), [process_host.cpp](cpp/process_host.cpp), [guest_dlopen.hpp](cpp/guest_dlopen.hpp) | Replay Java, CoreFoundation allocator and tar. Trace the first post-query dyld operation and model its state transition from native behavior. |
| Guest VM, heap, protection | **Proven for named bounds, heap-growth, file-mapping, multi-allocation deallocation, and raw protection cases.** | [guest_state.cpp](cpp/guest_state.cpp), [process_runner.cpp](cpp/process_runner.cpp), [process_host.cpp](cpp/process_host.cpp) | Source `2136057` matched native/GPU output and status, including invalid target `268435459`, overflow 4, and unmapped 1; passed 42+2,048+AOT gates. Runtime `c59447c4a34f2afa9359f109d053de000cc179186c72113bac24bc60dcf2c386`; general VM semantics remain open. |
| PAC and arm64e | **Modeled, selected cases proven.** Authenticated pointers are tracked/validated in software; this is not Apple hardware PAC. | [process_msl.cpp](cpp/process_msl.cpp), [process_decoder.cpp](cpp/process_decoder.cpp), [process_runner.cpp](cpp/process_runner.cpp), [macho.cpp](cpp/macho.cpp) | Differential-test the exact key, modifier and instruction sequence from a failing binary. |
| Generated/JIT code | **Partially modeled.** Guest MAP_JIT eligibility, permission transitions, SIGBUS retry, and opposite-mode resume pass four extracted-package synthetic fixtures with native/GPU parity. This does not qualify an unchanged application or all JIT APIs. | [process_runner.cpp](cpp/process_runner.cpp), [image_loader.cpp](cpp/image_loader.cpp), [process_host.cpp](cpp/process_host.cpp), [guest_code_policy.cpp](cpp/guest_code_policy.cpp), [guest_signal_runtime.cpp](cpp/guest_signal_runtime.cpp) | Test one unchanged JIT-using application with its real entitlement, code mutation, execution, and thread interactions. Keep broader runtime and packaging claims tied to exact artifacts. |

## Darwin services, threads, IPC, and GUI

| Area | State at checkpoint | Source ownership | Next proof |
|---|---|---|---|
| Darwin/POSIX services | **Modeled, selective.** File I/O, selected metadata, external signals and named synchronous fault/handler fixtures, clock query, signing/process/cache queries and loader services have focused comparisons. A packaged `__memcpy_chk` overflow reaches an installed SIGTRAP handler (signal 5, code 0, PC equals `si_addr` within each process); its explicit handler exit 77 does not prove default wait semantics or general signal parity. Four synthetic MAP_JIT package fixtures match native/GPU; six local IPC fixtures passed in isolation. | [process_host.cpp](cpp/process_host.cpp), [darwin_bridge.cpp](cpp/darwin_bridge.cpp), [process_decoder.cpp](cpp/process_decoder.cpp), [guest_signal_runtime.cpp](cpp/guest_signal_runtime.cpp) | Latest unchanged Java 26 and libz probes advance past raw syscall 206 and now stop at raw syscall 3418. Focused clock and IPC cases do not prove full Java/libz startup or general Mach IPC. |
| Threads and synchronization | **Modeled, bounded, proven for recorded cases.** Cooperative dispatch covers pthread creation, mutexes, conditions, join, TLS errno, signal masks, selected atomics, and focused `pthread_get_stackaddr_np` / `pthread_get_stacksize_np` queries for main and worker threads. A named packaged fixture also proves writes across two live guest stack intervals and permission revocation when an owner finishes; the prior linear-heap policy is unchanged. Parallel dispatch, cancellation, broader stack lifetime cases and general asynchronous I/O remain open. | [guest_scheduler.cpp](cpp/guest_scheduler.cpp), [process_runner.cpp](cpp/process_runner.cpp), [process_host.cpp](cpp/process_host.cpp), [host_service_registry.hpp](cpp/host_service_registry.hpp), [guest_signal_runtime.cpp](cpp/guest_signal_runtime.cpp) | Extend for the first real client primitive that diverges; retain timeout behavior in a standalone native comparison. |
| Mach IPC/services | **Open beyond individually implemented traps.** Focused `dcc1488` host-info and port-release fixtures match native, including invalid arguments and rights cleanup; isolated raw allowlisted core candidate `8b67998` passes raw and stack controls. This does not establish general Mach port, message or service behavior. | [darwin_bridge.cpp](cpp/darwin_bridge.cpp), [process_host.cpp](cpp/process_host.cpp), [process_runner.cpp](cpp/process_runner.cpp) | Continue the isolated libz startup investigation from raw syscall 3418; retain a separate GUI/app Mach request/reply fixture. |
| GUI frameworks/lifecycle | **Open.** README explicitly leaves GUI frameworks open. No GUI startup, interaction, rendering or shutdown is established. | [image_loader.cpp](cpp/image_loader.cpp), [process_host.cpp](cpp/process_host.cpp), [process_runner.cpp](cpp/process_runner.cpp), [metal_backend.cpp](cpp/metal_backend.cpp) | Select one unchanged small GUI app. Prove launch, an observable interaction/effect and clean exit against native; take its first missing framework/service as the next scoped task. |

## Packaging and acceptance boundary

| Area | State at checkpoint | Source ownership | Next proof |
|---|---|---|---|
| C++ launcher build | **Proven locally.** Strict C++ app build and full make verify passed for the recorded artifact. | [cpp/Makefile](cpp/Makefile), [Makefile](Makefile) | Rebuild candidate source and record its digest before the final matrix. |
| Bundle entry point | **Artifact identity proven for VM protection source `2136057`.** Runtime `c59447c4a34f2afa9359f109d053de000cc179186c72113bac24bc60dcf2c386`; metallib `c977849dba7896b7536b14efb5ba501764bfd8024cb6f6f62ae5649d5b272bf4`; archive `a19af8d8b637d2a4383d845cf1a8aa9a27ca464a136179d4e06004e9e614ea4c`. Extracted smoke returned `mixed42`. | [Makefile](Makefile), [README.md](README.md), [main.cpp](cpp/main.cpp) | Keep digest checks attached to the exact gate artifact and qualify application paths separately. |
| Drop-in compatibility | **Not established generally.** Passing unchanged cases establish only those binaries, inputs, OS build, launch flags and observed effects. | [HANDOFF.md](HANDOFF.md), [LEDGER.md](LEDGER.md) | Keep a named app matrix with binary identity, env/flags, native and Metal stdout/stderr/status, side effects, timeout/crash receipt and runtime digest. Mark each case independently. |

## Interpreter and remaining qualification

The released CFG budget-fallback source `1d9bffc` in `aba6a2c` recorded 13
dispatches and extracted-archive branch 7 / mixed 42 results. The combined
interpreter memory/branch and typed ADD/SUB immediate source was verified at
checkpoint `a76cfd9`; regression, `verify-interpreter`, and corrected package
smoke passed. The explicit read-only-store/unmapped-load fault controls do not
establish native signal parity. The 39-dispatch ISA probe includes W/X flag operations and SP/WSP arithmetic.
The later `8841503` checkpoint adds named W/X B.cond and LDP/STP fixtures; the
B.cond package trace has three AOT blocks and no compile fallback. The SP/CMP
fixture separately verifies translated `B.ne` after MADD at the typed-to-
translated boundary. Scalar and pair demand retries each match status 32 at the
same PC. See HANDOFF.md for exact artifact hashes and receipts. The
raw VM protection source `2136057` passed 42 launch comparisons,
2,048 function runs, and AOT/extracted archive smoke. The current service/format
source `7409b52` passed regression and interpreter gates; the packaged printf
matched native and the mixed AOT/fallback package case exited 42. The pthread
getter proof is scoped to its main/worker stack metadata fixture. These remain
named-fixture proofs, not broad app qualification.
Astra's direction is a precompiled GPU interpreter as the eventual default,
with translation retained during migration and for hot code. Share instruction
semantics on GPU, compare native/translator/interpreter results, and expand one
typed slice at a time. This does not close dyld, TPRO, Darwin, GUI, or JIT gaps.

## Suggested order

1. Replay unchanged CLI and cached-loader cases on the frozen C++ binary. Keep GPU app probes serial so missing receipts and timeouts remain interpretable.
2. Fix the earliest shared divergence with a native control and regression fixture. Replay affected apps and run the full C++ gate on one frozen build.
3. Qualify Java/JNI separately, then select a GUI app and prove an observable interaction.
4. Package that exact C++ digest and rerun smoke cases from the extracted artifact. Report compatibility only for the named matrix that passed.
