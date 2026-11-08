# Compatibility map

Checkpoint is source commit a44fb9c and verified C++ launcher SHA-256
ca8df1b8405726783041791bf81f09e96db1f0d5379b3619364dc75317d4c63d
(macOS 27.0.1, M4 Pro). “Proven” means a recorded native differential passed
for the named case. “Modeled” means the runtime supplies a host/software model
whose recorded cases passed. Neither label claims general macOS compatibility.

For replay commands and scratch evidence pointers, see [HANDOFF.md](HANDOFF.md).

Since this checkpoint, the integrated guest-VM runtime at 8bfa6f6 passed
42 launch comparisons and 2,048 function runs with digest
3162adc7a229d9e27578ca134d7e6c98d1f5b51ef40059830b43d57413bd9e45. The
packaged archive launcher digest matched. The Makefile false-green path is
fixed by giving the Swift recipe its own SWIFT_RUNTIME target. See HANDOFF.md
for current app outcomes and direction.

## Execution, ISA, and process ABI

| Area | State at checkpoint | Source ownership | Next proof |
|---|---|---|---|
| ARM64 guest execution | **Proven, bounded.** Reachable guest blocks translate to Metal; unsupported instructions fail explicitly. No guest CPU fallback. The full ISA is not claimed. | [translate_core.cpp](cpp/translate_core.cpp), [process_decoder.cpp](cpp/process_decoder.cpp), [process_msl.cpp](cpp/process_msl.cpp), [process_runner.cpp](cpp/process_runner.cpp) | Turn each real app’s first unsupported opcode into a native/GPU fixture, then replay the unchanged app. |
| Registers, calls, stack, startup | **Proven for tested launch ABI.** LC_MAIN, argc/argv, bounded stack, direct/indirect calls, guest exit and host-call resume pass fixtures. | [process_runner.cpp](cpp/process_runner.cpp), [guest_state.cpp](cpp/guest_state.cpp) | Add an ABI fixture only when an unchanged target exposes a mismatch. |
| SIMD and floating point | **Proven for named matrices and app paths**, including vector memory, conversions, comparisons, FRINTP, FDIV and status flags. Other opcodes and FPCR/FPSR behavior remain open. | [process_decoder.cpp](cpp/process_decoder.cpp), [process_msl.cpp](cpp/process_msl.cpp), [guest_vector_crypto.cpp](cpp/guest_vector_crypto.cpp) | Use the exact failing instruction and operands; compare result bits and flags with native. |
| Syscall/import ABI | **Modeled, selective.** Known imports/services cross a numbered host boundary; unsupported requests fail with context. libc and environment behavior are partial. | [process_decoder.cpp](cpp/process_decoder.cpp), [process_runner.cpp](cpp/process_runner.cpp), [process_host.cpp](cpp/process_host.cpp), [guest_format.cpp](cpp/guest_format.cpp) | Differential-probe the first missing import/service, including errors, carry and output buffers. |

## Mach-O, dyld, memory, and pointer authentication

| Area | State at checkpoint | Source ownership | Next proof |
|---|---|---|---|
| Mach-O and image graph | **Proven for tested ARM64/arm64e images and local dylib graphs.** Dependency ordering, selected @rpath forms, exports and constructors have passing fixtures. Format/command coverage is bounded. | [macho.cpp](cpp/macho.cpp), [image_loader.cpp](cpp/image_loader.cpp), [shared_cache.cpp](cpp/shared_cache.cpp) | Preserve the unchanged failing image and identify its exact command/format before extending parsing. |
| Chained/legacy fixups | **Proven for recorded rebase/bind forms.** JVM cache slots and local-load cases were compared with dyld. The parser explicitly rejects pages with multiple chained-fixup starts at [macho.cpp:457](cpp/macho.cpp:457). | [macho.cpp](cpp/macho.cpp), [image_loader.cpp](cpp/image_loader.cpp) | First obtain a real failing image using that encoding; inspect its starts table and compare patched slots with native dyld. |
| Shared cache and residency | **Proven for focused libz, libSystem and framework paths** in resident/demand modes. Not every image or startup protocol is covered. | [shared_cache.cpp](cpp/shared_cache.cpp), [cache_residency.cpp](cpp/cache_residency.cpp), [image_loader.cpp](cpp/image_loader.cpp), [process_runner.cpp](cpp/process_runner.cpp) | Replay unchanged cache-root/provider cases. Trace the first page, binding or initializer whose state differs. |
| Dyld APIs and lifecycle | **Modeled selectively.** Individual queried APIs have native fixtures. Full bootstrap is incomplete; handoff records a cached wrapper reading gAPIs as a prior app stop. Runtime dlopen covers a subset. Full unload, delayed activation, lazy and weak binding remain open. | [image_loader.cpp](cpp/image_loader.cpp), [process_decoder.cpp](cpp/process_decoder.cpp), [process_runner.cpp](cpp/process_runner.cpp), [process_host.cpp](cpp/process_host.cpp), [guest_dlopen.hpp](cpp/guest_dlopen.hpp) | Replay Java, CoreFoundation allocator and tar. Trace the first post-query dyld operation and model its state transition from native behavior. |
| Guest VM, heap, protection | **Modeled with passing bounds, heap-growth, file-mapping and selected mmap/mprotect probes.** Arbitrary VM semantics are not established. | [guest_state.cpp](cpp/guest_state.cpp), [process_runner.cpp](cpp/process_runner.cpp), [process_host.cpp](cpp/process_host.cpp) | For a failing allocation, compare address, flags, protection, return and later accessibility with native. |
| PAC and arm64e | **Modeled, selected cases proven.** Authenticated pointers are tracked/validated in software; this is not Apple hardware PAC. | [process_msl.cpp](cpp/process_msl.cpp), [process_decoder.cpp](cpp/process_decoder.cpp), [process_runner.cpp](cpp/process_runner.cpp), [macho.cpp](cpp/macho.cpp) | Differential-test the exact key, modifier and instruction sequence from a failing binary. |
| Generated/JIT code | **Open.** Translation extends to guest text targets on demand. That does not establish guest APIs that create or mutate executable memory. | [process_runner.cpp](cpp/process_runner.cpp), [image_loader.cpp](cpp/image_loader.cpp), [process_host.cpp](cpp/process_host.cpp) | The corrected JIT transaction remains isolated. The positive RW-to-RX calls matched native (first=42, second=73, wx=13/13, exit 0); execute-after-RW negative behavior, unmapping, and MAP_JIT remain open. |

## Darwin services, threads, IPC, and GUI

| Area | State at checkpoint | Source ownership | Next proof |
|---|---|---|---|
| Darwin/POSIX services | **Modeled, selective.** File I/O, selected metadata, signals, signing/process/cache queries and loader services have focused comparisons. Unsupported syscall IDs remain explicit. | [process_host.cpp](cpp/process_host.cpp), [darwin_bridge.cpp](cpp/darwin_bridge.cpp), [process_decoder.cpp](cpp/process_decoder.cpp) | Both libz-root and libz-provider reach __dyld_call_with_writable_tpro_memory after reading dyld4::gAPIs. The exact VM-gate artifact passed both commpage shift reads (native value 14), then guest faulted at address 0, normalized PC 0x180466370. TPRO parser/callback work is isolated. Compare native callback and permission behavior, then test existing guest callback machinery without guessing a vtable. Both libz paths remain unresolved. |
| Threads and synchronization | **Modeled, bounded, proven for recorded cases.** Cooperative dispatch covers pthread creation, mutexes, conditions, join, TLS errno, signal masks and selected atomics. Parallel dispatch, cancellation and general asynchronous I/O remain open. | [guest_scheduler.cpp](cpp/guest_scheduler.cpp), [process_runner.cpp](cpp/process_runner.cpp), [process_host.cpp](cpp/process_host.cpp), [guest_signal_runtime.cpp](cpp/guest_signal_runtime.cpp) | Extend for the first real client primitive that diverges; retain timeout behavior in a standalone native comparison. |
| Mach IPC/services | **Open beyond individually implemented traps.** Host-service and Darwin bridge code does not establish general Mach port, message or service behavior. | [darwin_bridge.cpp](cpp/darwin_bridge.cpp), [process_host.cpp](cpp/process_host.cpp), [process_runner.cpp](cpp/process_runner.cpp) | Trace the first GUI/app Mach message path; build a native request/reply fixture for that message and its error case. |
| GUI frameworks/lifecycle | **Open.** README explicitly leaves GUI frameworks open. No GUI startup, interaction, rendering or shutdown is established. | [image_loader.cpp](cpp/image_loader.cpp), [process_host.cpp](cpp/process_host.cpp), [process_runner.cpp](cpp/process_runner.cpp), [metal_backend.cpp](cpp/metal_backend.cpp) | Select one unchanged small GUI app. Prove launch, an observable interaction/effect and clean exit against native; take its first missing framework/service as the next scoped task. |

## Packaging and acceptance boundary

| Area | State at checkpoint | Source ownership | Next proof |
|---|---|---|---|
| C++ launcher build | **Proven locally.** Strict C++ app build and full make verify passed for the recorded artifact. | [cpp/Makefile](cpp/Makefile), [Makefile](Makefile) | Rebuild candidate source and record its digest before the final matrix. |
| Bundle entry point | **C++ artifact identity proven for the integrated VM runtime.** The archive launcher digest matched the full-gate binary at 8bfa6f6. Root make bundle still uses the Swift RUNTIME target; bundle-cpp packages a supplied verified C++ launcher. | [Makefile](Makefile), [README.md](README.md) | Keep the archive digest comparison in each full delivery gate. |
| Drop-in compatibility | **Not established generally.** Passing unchanged cases establish only those binaries, inputs, OS build, launch flags and observed effects. | [HANDOFF.md](HANDOFF.md), [LEDGER.md](LEDGER.md) | Keep a named app matrix with binary identity, env/flags, native and Metal stdout/stderr/status, side effects, timeout/crash receipt and runtime digest. Mark each case independently. |

## Suggested order

1. Replay unchanged CLI and cached-loader cases on the frozen C++ binary. Keep GPU app probes serial so missing receipts and timeouts remain interpretable.
2. Fix the earliest shared divergence with a native control and regression fixture. Replay affected apps and run the full C++ gate on one frozen build.
3. Qualify Java/JNI separately, then select a GUI app and prove an observable interaction.
4. Package that exact C++ digest and rerun smoke cases from the extracted artifact. Report compatibility only for the named matrix that passed.
