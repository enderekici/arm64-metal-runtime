# ARM64 to Metal development ledger

| Milestone | State | Evidence |
|---|---|---|
| Translate ARM64 leaf functions and compare native/GPU state | Verified | Two symbols, 2,048 exact invocations on M4 Pro |
| Ship a runtime independent of Python | Verified | The current C++ launcher runs from an extracted bundle with its AOT Metal interpreter library; Python remains part of verification tooling |
| Run caller-supplied inputs without executing the native binary | Verified | Known-answer JSON from dylib and executable Mach-O. Universal dylib selects the ARM64 slice |
| Reject bad inputs and guest exits | Verified | Unsupported instruction, invalid Mach-O, execution budget, and a GPU load beyond the logical word buffer |
| Support nested calls and guest stack | Verified | Console `main` calls a separately compiled helper, reads argv, spills registers, and returns through the guest stack |
| Add CPU exit and resume for host services | Verified | `puts` and `write` yield to the host and resume GPU execution. `_exit` terminates with the guest status |
| Launch whole supported Mach-O executables | Verified | Eight native/GPU comparisons across bundled console and stream programs and unchanged macOS `true` and `false`. stdout, stderr, and exit status all match |
| Expand unchanged system binary corpus | Verified paths | `echo` including POSIX octal escapes, physical/logical `pwd`, named `printenv`, `dirname`, `basename`, and `cat` for a binary file, stdin, and /dev/null |
| Translate indirect targets on demand | Verified | Two function-pointer targets selected by argv, with native-equivalent stdout and exit status after GPU re-entry |
| Add guest heap, file descriptors, and libc bindings | Verified paths | `cat` allocates a buffer and opens, reads, stats, and closes files through host services. File bytes match native including NUL and 0xFF |
| Apply executable chained rebases and binds | Verified | Global data pointer, in-image function pointer, and imported function pointer work in an unchanged fixture. Malformed fixup header rejected |
| Translate authenticated indirect calls | Verified paths | ARM64e `cksum` reaches both address-diversified stack-probe imports and zero-modifier code pointers. Unsigned branch target rejected |
| Expand file-I/O corpus | Verified paths | Native `cksum` default and `-o 1/2/3` match across multiple reads. Binary stdin exercises virtual descriptor close |
| Link local dylib graphs in guest memory | Verified | Root executable uses `@rpath`, a wrapper loads a transitive dylib via `@loader_path`, and both direct and pointer-based calls use the wrapper's `puts` rather than libSystem. Missing dependency fails before output |
| Run startup initializers on Metal | Verified paths | Support, wrapper, and executable constructors match native order and stdout. A constructor's global store affects the final return value |
| Bound Metal compilation by guest region | Verified paths | 260-instruction fixture crosses region boundaries and matches native. The large `xz` startup reaches explicit guest faults without the earlier monolithic compilation stall |
| Bridge guest pipes and descriptor flags | Verified paths | A pipe round-trip, nonblocking descriptor setting, stdout, and closure match native. Homebrew `xz --version` reaches `_sigaction`; Homebrew `lz4 --version` reaches unsupported SIMD store `0x3c818000` |
| Deliver guest signals on Metal | Verified paths | Native differential covers action queries, block/pending/unblock, nested delivery, SIG_IGN, and externally sent SIGUSR1. The C host callback queues events only. GPU handlers resume the interrupted guest frame |
| Expand scalar and SIMD instruction coverage | Verified paths | Unaligned Q loads and stores match native byte-for-byte. XZ and LZ4 reach their decoder setup after vector, addressing, and scalar arithmetic gaps were closed |
| Bridge reusable heap and CLI startup services | Verified paths | Native differential covers realloc preserving data, free, and calloc zeroing. Unmodified Homebrew `xz --version` and `lz4 --version` match native stdout, stderr, and exit status |
| Run guest pthreads on Metal | Verified paths | Worker start and return, mutex ownership, condition waits, join and exit, error-checking lock, per-thread errno, and per-thread signal masks match native |
| Lower shared-memory operations to Metal | Verified paths | Native comparison covers load-acquire/store-release, atomic add and compare-exchange across guest threads, and overlapping memmove, copy, fill, and bzero |
| Expand decoder ISA coverage | Verified paths | Added unaligned scalar accesses, SIMD lane operations, and ARM CRC lowering. Homebrew XZ and LZ4 now get past pthread setup into decoder work. Neither decompression probe completed |
| Complete unmodified LZ4 decompression | Verified paths | Homebrew `lz4 -dc` matches native stdout, stderr and exit status for a named file and stdin. A separate 512 KiB output matches the original bytes |
| Complete unmodified XZ decompression | Verified paths | Homebrew `xz -dc` matches native stdout, stderr and exit status for a single-block named file and stdin. Both `-T1` and `-T2` decode all 512 KiB of an eight-block file byte-for-byte |
| Grow process memory without changing guest addresses | Verified paths | Default-thread XZ matches native stdout, stderr and exit status on the eight-block, 512 KiB fixture through both named-file and stdin paths. The file trace shows one Metal buffer expansion. The heap fixture verifies existing allocations survive growth and the new region is accessible |
| Map Darwin's default rune tables into guest memory | Verified paths | Native heap fixture covers inline ASCII classification. Guest tables contain scalar data and omit host pointers |
| Lower patterned memory fill to Metal | Verified paths | Native comparison covers four-, eight-, and sixteen-byte patterns and partial final repetitions. XZ reaches compressed LZMA2 decoding past the pattern fill |
| Resume long-running guest workloads across dispatches | Verified paths | The region fixture completes 1.2 million loop instructions in addition to its straight-line code. The 12-block compressed XZ fixture expands from 23,156 to 753,460 bytes and matches the original. Each Metal dispatch still has a finite execution quantum |
| Bridge XZ diagnostic formatting | Verified path | An invalid named XZ file matches native stdout, stderr, and exit status after adding guest variadic stream formatting and character output |
| Resolve guest dylib re-exports | Verified path | The unchanged linked fixture binds `_support` through libbridge's `LC_REEXPORT_DYLIB` and executes the original libsupport code on Metal. Native stdout, stderr, constructor order and exit status match |
| Read the installed dyld cache and subcaches | Verified inspection | ARM64e install-name lookup, subcache UUID validation, Mach-O export trie reading, and VM-to-file translation resolve the libbz2 decompression export at offset `0x688`, consistent with Apple dyld_info. Cache images are not yet mapped into the guest launcher |
| Decode version-5 dyld cache pointer chains | Verified inspection | `make verify-cache` compares unslid decoded targets plus ASLR slide against live dyld mappings. All 123 targets match across libbz2, libz and liblzma, including the address portion of 80 authenticated pointers. Auth metadata is decoded from disk for future guest relocation |
| Execute system libz from the dyld shared cache on Metal | Verified opt-in paths | The same unchanged linked Mach-O matches native for `zlibVersion`, `zError`, and `adler32`. `zError` reads a rebased pointer in a split data segment. The build with cache loading disabled rejects the system libz import |
| Resolve authenticated cache stubs and merged GOT bindings | Verified opt-in paths | System libz `uncompress` matches native on a stored block and exercises cached allocation, memory movement on Metal and release. A separate compressed-block probe gets through its merged stack-canary GOT load and reaches the SIMD checksum routine |
| Resume indirect returns and run timed condition waits | Verified paths | Guest returns resume at the Metal dispatch boundary and XZ advances beyond its earlier filter teardown fault. Native pthread comparison covers immediate absolute and finite relative timeouts with the mutex reacquired |
| General application compatibility | Active | Cancellation, asynchronous host I/O, broader library/runtime services, shared-cache images, and GUI frameworks remain open |
| Discover parallel work and measure speedup | Open | Scheduler dispatches one guest pthread at a time. Function batching is explicit |

## Application compatibility track (2026-11-07)

| Probe | Verified result | Remaining blocker |
|---|---|---|
| Unmodified `/usr/bin/gzip` | Native and Metal match `-dc` for 64-byte and 37,000-byte named inputs and the small stdin case. Deterministic `-nc` produces identical gzip bytes for both named fixtures. Invalid input matches native stderr, empty stdout and status 1 | Other modes and gzip metadata variants remain unverified |
| Preserve Metal dispatch state across switch cases | Verified path | Unchanged gzip reached libz state validation with status 42, but the uninstrumented switch path returned a failure. Both scratch instrumentation and a volatile guest PC changed the result. The released runtime uses a volatile PC without tracing and reproduces native compression bytes |
| Unmodified `/usr/bin/bzip2 -dc` | Native and Metal match output, stderr and exit status on 64-byte and 37,000-byte named inputs and the 64-byte stdin case. The small named and stdin tests also match the copied extended attribute. Libbz2 runs on Metal, with host metadata and stream I/O services | Other bzip2 modes and metadata variants remain unverified |
| Unmodified Azul JDK 25.0.3 `java -version` | Native exits 0; local `@rpath/libjli.dylib` resolves on GPU | GPU launch stops at `_strdup` |
| Cached framework consumer: positive plist via `plutil` | Native exits 0. Metal handles three Objective-C class enumeration requests and continues startup. | Passes libmalloc’s CPU-index read, `LDSETH`, `RBIT`, and `PACIA1716`. Passes tagged Data-B authentication in libobjc. Passes the cache class-count call. Completes the CoreFoundation `RTLD_NOLOAD` request. Passes libmacho’s `getsectiondata` header read. Passes cached locale formatting. Passes the continuous-clock call. Passes Darwin trap 336. Passes libdispatch’s queue assertion. Resolves Foundation/NSPLUtil2 to enabled. Passes local Objective-C stubs and AUTIZA. Passes cached selector stub `0x1080045f0`. Passes Swift’s argument startup. Passes SIMD widening and signed lane extraction. Passes the restricted-process query. Passes self signing status via `csops`. Passes executable-path lookup and signed indexed loads. Passes CASP. Passes dyld’s Swift prespecialized-data query. Passes pointer hash lookup and four-byte-aligned signed callbacks. Passes dyld’s prebound lazy-load entry. Continues after Metal region eviction. Passes malloc_size. Both plist inputs pass the former Objective-C class realization abort when cached malloc_size handles guest VM pointers. With baseline arm64e selection, both unchanged inputs pass Darwin lstat64 (340). Both pass the Swift optimization version getter and the process-specific preoptimized-conformance query. Both pass shared-cache Swift conformance lookup and UCVTF D0, X8. Both stop at FMOV D1, #0.75 (0x1e6d1001) at guest PC 0x115242ec8 on the Swift runtime. Valid native exits 0 while Metal exits 1. Malformed native exits 1 with a parser diagnostic while Metal exits 1 with an unsupported-instruction diagnostic. |
| Execute ARM AES instructions on Metal | Verified paths | Native and Metal agree byte for byte on AESE, AESD, AESMC and AESIMC. The translator emits finite-field arithmetic and byte permutations in Metal, using the guest vector registers. `make -j2 verify-cache bundle` passes. |
| Copy SIMD lanes to scalar and vector destinations | Verified paths | The native and Metal fixture agrees on scalar extraction, vector replication and cleared upper lanes. Cached AES key expansion passes the former scalar lane-copy fault. |
| Bridge guest wall-time queries | Verified paths | Native and Metal `gettimeofday` calls bracket the live wall time returned by the exported one-output commpage wrapper. Host time is copied into validated guest buffers. The internal two-output timebase entry remains open. |
| Decode BTI landing hints | Verified path | The same fixture starts with BTI c and produces native-equivalent output. The process runtime currently treats BTI encodings as hints without guarded-page enforcement. `make -j2 verify-cache bundle` passes. |
| Model guest CPU capabilities and device barriers | Verified paths | A read-only commpage word advertises one scheduled CPU, fast TLS, and CRC32. DIT and SB are clear. DMB, DSB, and ISB lower to Metal device-memory barriers for the single-thread dispatch model. The native and Metal fixture agrees on supported capability checks and data around the barriers. |
| Publish guest boot time | Verified path | The read-only microsecond field matches `kern.boottime` natively and on Metal. Cached `mach_boottime_usec` passes the former load during plutil startup. `make -j2 verify-cache bundle` passes. |
| Sign and authenticate register-modified code pointers | Verified paths | The same arm64e fixture checks PACIA/AUTIA and calls code through A- and B-key authenticated branches. A wrong modifier traps natively and is rejected on Metal. The runtime checks guest tokens against their recorded key and modifier. `make -j2 verify-cache bundle` passes. |
| Protect guest VM subranges | Verified paths | Native and Metal agree on splitting an anonymous mapping, retaining adjacent pages, restoring read/write access and deallocating the original allocation. Negative probes trap natively and fail guest memory access on Metal for a read from PROT_NONE and a write to PROT_READ. The current service covers one anonymous allocation per call. |
| Query guest link-time library versions | Verified paths | The cache fixture gets the same recorded libz version natively and on Metal by short name and install-name leaf and sees -1 for a missing library. The API reads the guest executable's Mach-O dependency commands. `make -j2 verify-cache bundle` passes. |
| Publish guest memory geometry | Verified paths | The read-only commpage memory-size field agrees with `hw.memsize` on native and Metal. The cache fixture verifies that the page shift encodes the guest page size and that the kernel page size is at least that size. Cached libmalloc consumes both additional GOT bindings. `make -j2 verify-cache bundle` passes. |
| Model guest task-local Mach VM | Verified paths | Native and Metal match page alignment, zero fill, writable anonymous map access and whole-allocation deallocation. The Mach task port is synthetic and mapping backing stays in guest memory. Fixed and object-backed mappings, partial deallocation, and Mach port namespaces remain open. `make -j2 verify-cache bundle` passes. |
| Report guest image slides | Verified paths | The native and Metal fixture independently verifies that the preferred __TEXT address plus the API's slide yields its image header. Cached libmalloc also passes its image-slide call. |
| Model guest page size | Verified paths | Guest `vm_page_size` matches native `sysconf(_SC_PAGESIZE)`. The merged GOT binding lets cached libmalloc reach its VM allocation path. `make -j2 verify-cache bundle` passes. |
| Resolve canonical cached Objective-C selectors | Verified paths | Native and Metal agree on two known selectors, repeated name identity, distinct selector identity, the string bytes, and a miss. The host dyld lookup yields a cache-relative offset. The guest checks the cache UUID and disk bytes and returns its own logical address. Guest executable-specific prebuilt selector tables remain open. |
| Lower SIMD literal loads | Verified paths | Native and Metal match PC-relative S, D, and Q loads, including upper-lane clearing for narrower loads. The cached Q load uses a negative PC displacement and reads through the guest memory map. `make -j2 verify-cache bundle` passes. |
| Lower SIMD lane comparisons | Verified paths | Native and Metal vector fixtures match CMHI, CMHS, CMGT, CMGE, and CMEQ masks, including both true and false lanes and zeroed inactive lanes. |
| Expose the guest environment vector | Verified paths | `_NSGetEnviron` returns the guest environ slot and the fixture finds a supplied marker by both enumeration and getenv. Cached libobjc passes the former load through this API. `make -j2 verify-cache bundle` passes. |
| Model guest quiescent generations | Verified paths | A read-only commpage slot advances after each completed GPU dispatch. The native and Metal probes both observe an advance across a sleep. This is a guest scheduler generation and does not mirror the host CPU counter. |
| Lower atomic swap and SIMD min/max reductions | Verified paths | The native and Metal memory fixture matches the old and new values of SWPL. The vector fixture matches the signed and unsigned reductions byte for byte, including zeroed upper result lanes. `make -j2 verify-cache bundle` passes. |
| Read-only shared guest mappings | Verified paths | Native and Metal match mapping a 4 KiB owned shared-memory object, page alignment, descriptor closure, name unlink and full unmap. A separate native writer updated an owned file after the reader closed its descriptor. Native and Metal both saw the update. Guest mappings have a 16 MiB backing arena and refresh between GPU dispatches. `make -j2 verify-cache bundle` passes. |
| Open and unlink guest shared-memory objects | Verified public path | The same fixture creates its own object with mode 0600, observes EEXIST, reopens read-only, unlinks the name, observes ENOENT, then stats and closes the live descriptor. Native and Metal match. Both Darwin open entry points share the host service and retain their mode argument ABI. The public entry is also observed in plutil. `make -j2 verify-cache bundle` passes. |
| Compare guest SDK and deployment versions | Verified paths | Native and Metal agree on platform, SDK, minimum OS and a release-set query. A binary with SDK 27.0 and minimum OS 11.0 passes the SDK 26.0 check and fails the minimum OS 26.0 check in both paths. The service uses the guest build metadata with the host dyld version-token comparison. `make -j2 verify-cache bundle` passes. |
| Lower RCpc acquire loads | Verified paths | Native and Metal match a 64-bit LDAPR load. All four register-width encodings use the existing guest ordered-load path and dispatch synchronization. |
| Demand-map authenticated GOT slots | Verified bootstrap path | Read-only relocation of the eight-byte slot at `0x16800c958` lets the original BLRAA authenticate and continue. The loader accepts slots present in the cache rebase chain inside a GOT mapping. `make -j2 verify-cache bundle` passes. |
| Allocate aligned guest memory | Verified paths | Both Darwin aligned-allocation entry points share the guest heap service. Native and Metal match 4 KiB alignment for a 65-byte allocation, EINVAL and ENOMEM with the output preserved, and subsequent realloc/free with the data intact. `make -j2 verify-cache bundle` passes. |
| Keep the high byte clear in guest Data-A signatures for untagged addresses | Verified paths | The earlier marker set libobjc FAST_IS_RW_POINTER and sent a cached read-only class pointer to AUTDB. The revised token keeps that bit clear. Native and Metal agree on the unsigned high byte and the unchanged plutil gets past the old failure. |
| Preserve addresses in Data-A signatures | Verified paths | The arm64e fixture compares the original pointer with the masked signed pointer on native and Metal. A packed ISA can retain just the guest address or retain the signature bits for a checked AUTDA. The simulated signature uses a table bucket plus the guest address and modifier. `make -j2 verify-cache bundle` passes. |
| Strip instruction pointer signatures | Verified path | PACIZA followed by XPACI restores a local code address in the same arm64e fixture on native and Metal. |
| Strip data pointer signatures | Verified paths | The same thin arm64e fixture strips a signed stack pointer and leaves an unsigned stack pointer unchanged on native and Metal. The translated instruction resolves recognized guest tokens without checking key or modifier. |
| Grow the authentication table on cache faults | Verified path | Fixed guest register offsets and token IDs survive a Metal state buffer replacement between dispatches. Guest-generated signatures still use the current buffer capacity. `make -j2 verify-cache bundle` passes. |
| Authenticate guest data pointers | Verified paths | Register-modifier PACDA/PACDB and AUTDA/AUTDB use guest tokens on Metal. The same thin arm64e binary runs natively and on Metal with either key and rejects wrong-key and wrong-modifier cases. The token layout survives the libobjc packed ISA mask and still checks the recorded key and modifier. `make -j2 verify-cache bundle` passes. |
| Tagged Data-B pointers | Verified paths | libobjc class_rw_t::ro uses bit 0 to pick the discriminator. The failing token targeted an odd pointer with key 3, but its old encoding cleared that bit. Native and Metal now agree on the tagged pointer round trip. Metal rejects a changed low bit, and unchanged plutil passes the prior AUTDB fault. The release gate exits 0. |

| Implicit code-pointer authentication | Verified paths | Native and Metal agree on PACIA1716 and PACIB1716 round trips and a BRAA tail branch. The guest rejects a changed X16 modifier. Unchanged plutil passes the former PACIA1716 fault. The release gate exits 0. | BLRAA and its return continuation match native with the fixture restoring the incoming LR before RET. |

| Scalar bit reversal | Verified paths | Native and Metal match for W and X forms, including in-place writes, W zero-extension, and zero-register operands. Unchanged plutil passes the former RBIT fault. The release gate exits 0. |

| Guest CPU and cluster identity | Verified paths | Main and worker probes validate native CPU IDs against `pthread_cpu_number_np` when stable. Metal reports virtual CPU and cluster zero and preserves per-thread TSD identity through `TPIDRRO_EL0`. Cached libmalloc passes the former `TPIDR_EL0` fault. |
| LSE bitwise read-modify-write | Verified paths | Native and Metal outputs match for halfword set, byte clear with acquire/release, and word XOR with release, including old values. Cached `plutil` passes its former `LDSETH` fault. `make -j2 verify-cache bundle` exits 0. |
| Guest thread-specific data | Verified paths | The native and Metal pthread fixture match. The main thread and worker get distinct guest TSD pointers, retain them across scheduling, and read their own pthread identity and errno address from slots 0 and 1. Metal lowers `MRS TPIDRRO_EL0` to the active guest thread pointer. |
| Demand-map cache segments | Verified opt-in paths | With `ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1`, cached libz and unchanged gzip match native. CPU materializes and relocates missing segments into a 64 MiB resident pool. Metal retries the faulting instruction with its guest PC and NZCV preserved. Segment eviction and cache initializer execution remain open |
| Full-width guest PC and cached addresses | Verified paths | The process dispatcher keeps full-width targets across direct branches, indirect branches, returns and dispatch resumes. The cache starts above 4 GiB and the libz regression invokes a host-service binding through an indirect pointer above that window. Function-batch dispatch retains its existing address width |
| Cached-library graph control | Re-export fixture executes the original cached libcharset routine; native and isolated GPU stdout/status match (`reexport-ok`) | Broader cache dependency graphs and initializers remain unverified |
| SIMD compressed libz fixture | Native/GPU stdout, stderr, and exit status match; 2,880 bytes, `uncompress=0`, checksum `3476419374` | This is the focused libz fixture; no broader application claim |
| Nanosleep/signal scheduling fixture | Integrated native/GPU differential confirms sibling progress during timed wait, masked sender SIGUSR2, EINTR/remainder, and external SIGUSR1 | No known fixture gap |
| Integrated frozen verification | `make verify-cache verify` exits 0; 43 native launch comparison lines, 2,048 function matches, and 123 decoded cache pointer targets | Full-application compatibility remains open |
| TSD release gate | Verified | `make -j2 verify-cache bundle` passes with the expanded pthread differential. |
| Guest Mach thread identity | Verified paths | The expanded pthread fixture exits 22 against the previous runtime. Native and Metal now agree on odd, distinct Mach thread names in slot 3 and stable worker identity across scheduling. The cached `plutil` run advances past the lock assertion. `make -j2 verify-cache bundle` exits 0. |
| Guest APRR capability byte | Verified paths | The native probe reports 3 and native JIT write-protect support. The Metal probe reads the guest's declared value 0, and a store to the same byte is rejected. The guest has no APRR register support. `make -j2 verify-cache bundle` exits 0. |
| Guest dyld immutability query | Verified paths | Shipped symbols identify the previous fault as `_dyld_is_memory_immutable` and its missing object as `dyld4::gAPIs`. The guest query uses its loaded image ranges and cache maximum protections. Native and Metal agree for literal, writable global, and stack probes. A pointer returned by cached libz matches native in both cache modes. `make -j2 verify-cache bundle` exits 0. | Other APIs backed by `dyld4::gAPIs` still need guest loader state or service implementations. |
| Raw Darwin trap boundary | Verified paths | GPU lowering preserves NZCV and resumes after `svc #0x80`. A native/Metal fixture exercises -28 and -27 against the task global and TSD slot, then -25 with an invalid target and null/dead names. Both exits are 0. `make -j2 verify-cache bundle` exits 0. | General IPC rights, port construction, and destruction of live rights remain open. |
| Cached task self and basic Mach receive rights | Verified paths | The cache loader seeds the exported task-self scalar in both resident and deferred segments. The prior null-task trap disappears from the unchanged `plutil` run. A native/Metal raw-trap fixture checks invalid task rejection, distinct plain and guarded receive rights, and successful destruction. | Send rights, guard exceptions, Mach messages, and other construction flags remain open. |
| Demand mapping of constant cache rebases outside image segments | Verified paths | The mapper accepts exact slide-chain slots in initially read-only constant mappings. A native/Metal probe reads the same cache-relative target `0x645a81d8` at slot offset `0x6021d9c8`. Metal rejects a write to the slot. The release gate `make -j2 verify-cache bundle` exits 0. |
| Dyld message-send offsets | Verified paths | The shipped dyld method returns a table pointer in x0 and count in x1. The guest validates the versioned ObjC metadata and publishes the read-only table at its guest cache address. Native and Metal agree on the relative pointer, 35 entries, and all 280 table bytes in both cache modes. Repeated queries return the same pointer. The release gate passes. |
| Mach reply-port lifecycle | Verified paths | The guest namespace records the reply-port creation flag separately from guard semantics. Native and Metal both create distinct plain and guarded reply ports and destroy each with the correct context. The ordinary receive-right fixture and `make -j2 verify-cache bundle` pass. | Reply message delivery and send-once right enforcement remain open. |
| MIG restartable-ranges registration | Verified paths | Native and Metal both return `KERN_INVALID_ARGUMENT` for empty registration and `KERN_NOT_SUPPORTED` for duplicate registration. A first registration succeeds in the fresh guest, while native startup has already registered ranges. The raw fixture checks the reply ID, length and local port. The release gate passes. | Signal-return PC recovery is implemented and has no targeted positive fixture yet. |
| MIG restartable-ranges synchronization | Verified paths | Native and Metal accept request 8001 and return reply 8101. The guest worker paused inside its registered range returns 42 via recovery, versus 17 with synchronization skipped. Native startup already owns the range registration, so its custom worker follows the normal branch. `make -j2 verify-cache bundle` passes. |
| Guest dyld image callbacks | Verified paths | `_dyld_register_func_for_add_image` delivers the startup image snapshot using guest callback frames on Metal. The native and guest fixtures agree on the main Mach-O header and slide and on synchronous delivery before the caller continues. A demand-cache run also exercises multiple callbacks with libz present. The release gate passes. | Future dlopen notifications and the separate Objective-C v4 interface remain open. |
| Dyld section-location query | Verified paths | `_dyld_lookup_section_info` returns guest addresses from the loaded image's Mach-O commands and slide. Native and Metal agree on a known Swift section's payload, an absent section, and the unknown-kind sentinel. A callback fixture also reads cached libobjc's Objective-C image-info section on Metal and native. The release gate passes. | The query uses the Mach-O header and does not depend on native dyld loader handles. |
| Initial Objective-C v4 mapped callback | Verified paths | A native and Metal arm64e probe checks delivery of a Mach-O header and absolute path. Metal rejects a deliberately wrong mapped-slot signature. Cached metadata is scanned without faulting whole text segments into guest residency. The release gate passes. | Descriptors use the Mach-O lookup fallback and conservative optimization flags. Subsequent init, unload, and patch notifications remain open. |
| Dyld shared-cache range query | Verified paths | Both cache modes return the native span `0x1b9b14000` and matching first 16 bytes at the guest cache base. Repeated calls return a stable pointer. Cache-disabled launch returns null and zero. The release gate passes. | The span is a logical address range. Existing demand rules still control which cache data is resident. |
| Read-only Objective-C header table | Verified paths | The guest reads the cache-relative table at offset `0x749016f0` with 3,499 entries of 24 bytes each. Both cache modes match all 83,984 bytes and the relative pointer from native. A guest store is rejected and the cache-disabled query returns null. The release gate passes. |
| Writable Objective-C header table | Verified paths | The table at offset `0x66104000` has 3,499 entries of 8 bytes. Native and guest agree on the offset, dimensions, and write/readback behavior across repeated lookups. Fresh guest launches match all 28,000 on-disk bytes in both cache modes. Native by main already has process-local state in some entries. The cache-disabled query returns null. The release gate passes. |
| Main executable header | Verified paths | The loader places the executable at guest address `0x4000` and adjusts its entry. Native and Metal agree that `_dyld_get_prog_image_header` returns the executable's Mach-O header and the add-image callback sees it exactly once. The fixture passes with cache disabled, eager and demand mapping. A direct guest load from address zero faults. |
| Typed calloc | Verified paths | The typed entry shares guest calloc's count and size ABI. Native and Metal agree on zero-fill, writable memory, free, and `ENOMEM` for a multiplication overflow. Unmodified `plutil` dispatches the former null allocation through this service. The release gate passes. | The guest heap currently treats the type ID as a hint and does not partition allocations by type. |
| Bounded cache text windows | Verified paths | Cached `__TEXT` reads map a bounded read-only window. An eight-byte Metal load across the libz page boundary extends the initial 4,096-byte window to 4,100 bytes and matches native. A separate host-service read crosses the same boundary directly and also matches. The release gate passes. |
| Dyld platform and SIMD immediate | Verified paths | `dyld_get_active_platform` returns the main image platform with a macOS fallback. The native macOS value matches in plain, eager and demand launches. The Metal SIMD fixture matches all 64 bytes for narrow and wide halfword and word `MOVI` forms, including shifts and upper-half clearing. The release gate passes. | Native alternate-platform overrides were not exercised or implemented. |
| Selectors crossing cache windows | Verified paths | The formerly failing pointer starts 80 bytes before the end of its first window. A native/Metal fixture matches all 110 selector bytes. The trace shows a 4,096-byte read window followed by a 4,127-byte window including the terminator. String extension is confined to immutable cache backing and the owning `__OBJC_RO` segment. The release gate passes. |
| Growable guest residency table | Verified paths | An unchanged native/Metal fixture allocates, writes, reads and releases 300 live VM mappings. Metal grows the table from 256 to 512 rows. Selector windows now start at 16 KiB granularity, and the native boundary fixture still matches. `make -j2 verify-cache bundle` passes. |
| Objective-C preoptimized references | Verified paths | Cached dylib descriptors advertise the optimized references bit and its companion flags bit. A native/Metal arm64e fixture verifies both bits and checks pointer identity with the canonical selector lookup for all 58 libobjc selector references. The release gate passed on retry after an external SIGUSR1 failure in the first run. |
| Objective-C mutation block | Verified paths | Native and Metal allow a write to cached image info inside the callback and reject it after return. The guest validates the block and image index and restores cache permissions. Release gate passes. | General block copying and later notifications remain open. |
| Signed immediate loads | Verified paths | Native and Metal agree on byte, halfword, and word sign extension, pre/post writeback, and unscaled addressing. A native/Metal demand-cache probe reads an initially absent text page and verifies one post-index increment. A direct null load faults on both. The release gate passes. |
| Dyld cache-override query | Verified paths | The guest reports no overrides because its loader has no cache substitution path. The result matches native for an ordinary launch and in all three guest cache configurations. The release gate passes. |
| PACGA | Verified paths | Metal uses keyed SipHash-2-4 with an independently generated 128-bit guest key. Native and Metal agree on repeatability, sensitivity to each operand, zero low 32 bits, the SP modifier, and repeatability across threads. The release gate passes. | Guest codes use the guest algorithm and key, so their numeric values differ from native hardware codes. |
| Guest VM allocation size lookup | Verified paths | Cached malloc_size runs on Metal for pointers in guest VM. Runtime heap pointers still use the live allocation map. The same allocation fixture matches native with cache off and on. Both unchanged plist inputs advance past the old class data failure in the pre-selection-fix replay. | Other allocator entry points still need consistent ownership routing. |
| ARM64 Mach-O subtype selection | Verified selection fixture | Both universal fixture orders run the baseline slice and match the native baseline binary. The runtime rejects the extracted thin plutil arm64e.x1 slice. On this M4 Pro FEAT_CPA is zero and an isolated ADDPT exits on SIGILL. The launcher accepts ARM64_ALL, ARM64_V8 and, in process mode, baseline ARM64E. | Extended CPU subtypes need explicit ISA support and selection policy. |
| Scalar integer-to-floating-point lowering | Verified instruction fixture | The same ARM64 executable matches native for 352 signed and unsigned W/X to S/D conversions across all four FPCR rounding modes. It compares exact IEEE result bits and sticky FPSR inexact status, including ties, signed extrema and carry into the exponent. | FPCR only models rounding control. Floating-point traps and other FPCR behavior remain open. |
| Dyld shared-cache Swift conformance lookup | Verified paths | Native and Metal match the type-only Int: Hashable hit, metadata-only NSString: ExpressibleByStringLiteral hit, mixed input, and missing keys. Result pointers match as cache-relative offsets. Guest image graph membership determines which duplicate can be returned. Both unchanged plutil inputs advance to UCVTF. The release gate exits 0. | Positive fixture uses demand cache without Objective-C bootstrap. Duplicate ordering has no dedicated native fixture. |
| Dyld process Swift conformance availability | Verified paths | The direct main-header query matches native with cache off and on. The guest reports no optimized process loader set. Both unchanged plutil inputs pass the query, and the release gate exits 0. | Shared-cache protocol conformance lookup is a separate API. |
| Dyld Swift optimization API version | Verified paths | A direct import in the same binary returns 1 natively and under Metal with cache off and on. Both unchanged plutil inputs pass the cached wrapper. The release gate exits 0. | Version describes the interface. Precomputed conformance availability is a separate process-specific query. |
| Darwin lstat64 | Verified raw syscall probe | The same binary matches native stat bytes, carry and return value for a regular file, a symlink, missing path, null output and null path. The bridge forwards path lookup to Darwin and copies metadata into validated guest memory. The release gate exits 0. | See the plist replay for the next app-level stop. |
| Guest sandbox policy query | Verified paths | Native and Metal agree on carry, return value, and the decision word for a denied outbound query, an allowed file read, and an invalid operation (EINVAL). Both unchanged plutil cases pass syscall 381. | The bridge uses the host process policy and supports self-PID, Sandbox call 2, path filter 1, and flags 0. |
| Guest allocation size query | Verified paths | The live allocation map supplies malloc_size. Native and Metal match for two live size classes and return zero for an interior pointer, stack, null, and the tested freed pointer. Both plutil cases pass the import and stop at a sandbox query. The release gate exits 0. | Guest allocation classes can differ from native outside the tested sizes. |

| Bounded Metal region cache | Verified paths | The cache retains 1024 pipeline groups and evicts their lookup entries in insertion order. Active frames keep strong pipeline references. Both unchanged plutil traces show eviction and subsequent GPU work, then stop at the _malloc_size import. The release gate exits 0. |

| Prebound cache lazy loading | Verified demand-cache path | The captured libobjc request resolves through its LC_LAZY_LOAD_DYLIB_INFO record to already-loaded libswiftCore. Native and Metal agree on the flag transition and a repeated call. The adjacent flag is rejected by the record parser. Both unchanged plutil cases pass lazy loading and hit the Metal region-cache limit. The release gate exits 0. | New dependencies and records requiring runtime binding remain open. |

| Swift pointer-table lookup and signed callbacks | Verified paths | Cache-relative lookup validates the host cache UUID and matches native on a three-key hit, miss, and invalid table in both mapping modes. The host callback decoder checks the whole token and accepts four-byte-aligned targets. A signed image callback matches native. Both unchanged plutil cases pass these paths and reach dyld lazy loading. Both release gates pass. |

| Swift prespecialized cache data | Verified paths | The guest loader reads the Swift optimization header and maps the owning cache image through its normal relocation path. Native and Metal match the cache-relative address, metadata version, and a relocated pointer in both mapping modes. Cache-free guest returns null. Both unchanged plutil cases pass the query and reach the pointer hash-table lookup. The release gate exits 0. |

| Paired compare-and-swap | Verified paths | Native and Metal match 80 bytes for aligned CASP, CASPA, CASPL, and CASPAL, including failed comparison and both widths. Native rejects an unaligned pair with SIGBUS and the guest reports an alignment fault. Both unchanged plutil inputs pass CASP and reach dyld’s Swift prespecialized-data query. The release gate exits 0. | Atomicity follows the existing one-guest-thread-per-dispatch model. |

| Guest executable path and signed indexed loads | Verified paths | The path probe matches native on short-buffer sizing, exact and oversized buffers, and absolute and relative symlink spelling. Native and Metal also match 32 bytes for indexed LDRSB and LDRSH with W and X destinations and scaled and signed indices. Both unchanged plutil inputs pass the previous faults and reach CASP. Both release gates pass. |

| Guest signing status | Verified self-query cases | The raw csops status probe matches native on linker-signed and hardened ARM64 binaries, including the four-byte write, null output, EFAULT, ESRCH, and carry flag. Both unchanged plutil inputs pass csops and reach _NSGetExecutablePath. The release gate passes. | Status is modeled from guest static signing metadata. Other operations, foreign queries, and live kernel state remain open. |

| Guest dyld restriction query | Verified policy cases | Native and Metal agree for ad hoc, hardened runtime, hardened with the DYLD environment entitlement, and the legacy restriction section. Both unchanged plutil cases pass the query and reach csops. The release gate exits 0. | This models the queried policy from static metadata. AMFI and kernel enforcement are outside this implementation. |

| SIMD widening shifts and signed lane moves | Verified paths | Native and Metal match 96 bytes across USHLL/SSHLL and upper-half forms, all source widths, nonzero shifts, and overlap. SMOV matches 40 bytes across W/X and signed byte, halfword, and word moves. Both plutil inputs pass the former opcodes and reach libdyld’s restricted-process query. The combined release gate passes after an earlier pointer-authentication gate failure. |

| Guest Darwin argument accessors | Verified paths | The two cache stubs resolve to _NSGetArgc and _NSGetArgv. Native and Metal agree on startup count, vector identity and terminator, and writes through the returned pointers. Both unchanged plutil cases pass Swift argument startup and reach an unsupported widening shift. The release gate exits 0. | Writes to the startup argv vector or its strings were outside this probe. |

| Cached Objective-C selector stubs | Verified paths | The executable cache mapping and exact ADRP X1, ADD X1, B, BRK shape identify the stub without a cache stub mapping flag. Translation includes the three live instructions and branches to 0x108000800. Both unchanged plutil inputs pass this point and fault later in Swift argument startup. The isolated release gate exits 0. |

| Local Objective-C stubs and zero-modifier pointer authentication | Verified paths | The loader accepts file-backed __objc_stubs in executable __TEXT. The previous build rejects a branch into that section and the new build matches native. Native and Metal agree on zero-modifier signing and authentication for IA, IB, DA, and DB. Both unchanged plutil inputs pass local stubs and AUTIZA, then reach the same cached selector stub. The isolated release gate exits 0. |

| Guest system feature flags | Verified host service | A native and Metal probe both report enabled for Foundation/NSPLUtil2 and disabled for an unknown domain. Both unchanged plutil replays take the same enabled branch and reach the Objective-C stub at 0x25d80. The release gate exits 0. | Valid lint still exits 1, and malformed lint exits 1 without its native parser diagnostic. |

| Guest main dispatch queue | Verified demand-cache path | Startup publishes the cached main queue in the main thread’s dispatch slot and sets the queue drain-owner bits to the guest main Mach thread name. The old build hits the same BRK as plutil. Native and Metal agree on main-thread and worker assertions after the fix. The release gate exits 0. | The resident fixture exceeds the 64 MiB cache graph limit. This models the observed Darwin dispatch layout and startup state. |

| Guest process identity query | Verified paths | The raw trap 336 probe matches native for PIDINFO flavor 17 on self, including the 56-byte record, guest executable UUID, carry flag, untouched trailing bytes, and ENOMEM, EFAULT, and ESRCH. Unchanged plutil passes the observed request and reaches a libdispatch queue assertion. The release gate exits 0. | Other proc_info calls and queries for live foreign processes remain open. |

| Continuous clock service | Verified paths | The scalar fault probe identifies the missing commpage hardware clock byte at 0xfffffc091. The shipped cache stub targets mach_continuous_time. The guest calls the host clock service, and a native differential verifies nonzero, advancing ticks in both cache modes. Unchanged plutil passes the clock call. The isolated release gate exits 0. |

| Guest locale key initialization | Verified demand-cache path | Cached asprintf originally read the guest task port through the uninitialized locale key and attempted CASA at 0x613. Seeding Darwin’s reserved key 10 makes the same binary match native output. Unchanged plutil passes the former fault. The release gate passes. | The resident probe exceeds the 64 MiB cache graph limit. Per-thread locale changes and locale destruction remain open. |

| Guest executable header getter | Verified paths | The cached Swift concurrency stub resolves to _NSGetMachExecuteHeader. It now shares the guest loader service used by _dyld_get_prog_image_header. Native and Metal agree on both API results and exact main-header bytes in resident and demand modes. Unchanged plutil passes the getsectiondata header read. The release gate exits 0. |

| Guest RTLD_NOLOAD handles | Verified paths | Guest dlopen resolves names against startup images, including a framework alias with a cache-only target. Native and Metal agree on repeated libz opens in both cache modes, CoreFoundation alias opens in demand mode, RTLD_FIRST handle identity, close, and a missing image. The unchanged plutil reaches and returns from the CoreFoundation request with mode 0x10. The release gate exits 0. | General image loading, guest dlsym, and dlerror remain open. |

| Cached Objective-C class count | Verified paths | The guest loader reads the cache class hash table occupancy plus duplicate records, matching native dyld at 239,170 in resident and demand modes. A guest without a cache returns zero. Unchanged plutil completes the class-count request and reaches dlopen. The release gate exits 0. | Other dyld APIs still need guest loader state. |

| Cached Objective-C class enumeration | Verified paths | The host resolves cache-relative candidates and verifies the cache UUID. The guest invokes the signed block on Metal with guest pointers and reads loaded state from its writable header table. The probe matches native's `NSObject` cache offset, exercises both guest loaded states, and verifies a missing name has no callback. The release gate passes. | Process-specific prebuilt classes and access to class data outside the loaded guest image graph remain open. |
| Release verification | `make verify-cache bundle` includes repeatable system gzip and bzip2 comparisons, vector reversal/extract in the SIMD fixture, and bounded `snprintf` in the memory fixture | General macOS application compatibility remains open |
| Guest entropy and ARM64 lowering | Native and Metal fixture outputs match for the 256-byte entropy request, buffer guards, zero-length null request, and `EINVAL`/`EFAULT` cases. The fixture exercises byte-vector `MOVI` plus `CLZ` and `CLS` edge cases. `make -j2 verify-cache bundle` exits 0. | Real entropy is supplied by the Darwin host service. |

Earlier probes still locate additional libc gaps. Sort and grep stop at `_strdup`, and gzip with a missing input reaches `_vwarn`.

Run `make verify` to repeat the core GPU and rejection checks. Run `make bundle`
to regenerate the release archive. Validation was performed on the M4 Pro with
macOS 27.0.1.

## C++ implementation

The active runtime lives in `cpp/`. Root `make`, `make verify`, and `make bundle`
use the C++ launcher. The former root Swift launcher and its C bridge
have been removed. Earlier Swift comparisons below are historical results.
Guest instructions continue through generated MSL. The C++ host owns loader,
scheduling and Darwin service state and uses Apple's metal-cpp for device and
command submission.

Independent tracks cover Mach-O/fixups, the shared cache, executable image
loading, instruction decoding, Metal source generation, the Metal backend,
guest state, Darwin bridges, process launch, function batching, and the CLI.
Check each module with warnings-as-errors, then link the C++ executable and
run native comparisons for both launch and function paths as implementation
coverage grows.

The first port wave has focused checks for the parser, guest state, Darwin
bridge, base decoder, crypto and string code generation, and Metal backend.
The process decoder matched emitted MSL and successors against the Swift
decoder for 58,481 inputs. The standalone Metal backend produced all 257
expected buffer values. The C++ function runner now matches native for 2,048 invocations across
`update_words` and `xor_words` and enforces the block budget. The Metal
backend uses autorelease-pool ownership for command buffers. The linked
fixture loads three images with three executable initializers and bound
guest targets after correcting the chained-fixup offset base. That is a
loader check, and execution of its initializers through the C++ runner is
still pending. The earlier C++ component and Swift release gate results are
historical; use the current root Make targets for fresh verification.

The strict integrated C++17 checkpoint at `.build/cpp-verified/arm64-metal`
matches native stdout, stderr and exit status for nine process cases:
console, unchanged true, false, echo, cat and cksum, both linked fixture
modes, and the 352-conversion floating-point fixture. The linked runs
include all three constructors in native order. The C++ CLI also matches
native for 2,048 function invocations. Startup now applies modeled-global
and function bindings, initializes PAC tokens, advances initializer frames
after host tail calls, and handles synthetic import targets. Named-file
cat also matches with the shared-cache option enabled.

The broader launch gate passes 36 native comparisons and both malformed
loader rejection checks. This includes guest pipe, nonblocking fcntl,
shared-memory mapping and five cksum paths. Guest signal registration and delivery now match native for nested
handlers, resumption, and an externally sent SIGUSR1. Missing-file cat and cksum diagnostics match
native byte for byte with exit 1.

The full cache-zlib fixture matches native in both resident and demand
modes, including all 2,927 output bytes. Resident loading now publishes
the eight-byte merged data binding when its slot falls outside the loaded
image's resident regions. The same strict C++ executable matches unchanged
gzip compression and decompression for 38,000-byte data in both modes,
and matches the malformed demand-mode diagnostic. The two obsolete
large-region gzip probes were stopped after 24 minutes once the bounded
build verified the compression path.

On the M4 Pro, the published C++ checkpoint took a median 530 ms to
launch unmodified `/bin/echo`, versus 2.28 ms native (seven runs each
with matching output and exit status). Native gzip decompression of the
37,000-byte fixture took a median 1.98 ms over 15 runs. The C++ demand-cache
run hit the 20-second cutoff. These measurements include process startup.
A separate unchanged gzip compression of a 115 KB text fixture completed
in 72.9 seconds on the published C++ runner, with matching stdout, stderr
and exit status. The native timer in that probe rounded to 0.0 seconds.

## Resume here

1. Continue the C++ launch gate past pthread_mutex_lock (67). A strict snapshot
   passed the 65 MiB allocation fixture with matching output and exit status.
   Heap growth copies the existing Metal backing and rebinds guest memory at
   a host boundary. The app now links the scheduler and dispatches
   pthread_self (73) through it. The unchanged thread fixture advances
   from service 73 to service 67. The remainder of thread lifecycle and
   synchronization still need runner integration.
   The current strict gate logged 39 native matches before stopping at 67.
   A focused fixture made two actual `pthread_self` calls and matched the
   native nonzero, stable handle result. The earlier heap and VM build also
   passed the separate VM/environment fixture, external SIGUSR1 comparison
   and 2,048 native/Metal function comparisons. Frozen source and logs are
   under the directory recorded by `cpp-checkpoint-path` in the scratch root
   identified by `/tmp/arm64-metal-align-current`.
2. Signals run as guest Metal frames. Strict internal and external
   comparisons match native, and a separate interruptible nanosleep probe
   checks EINTR and a nonzero remainder after SIGUSR1. The mapped process
   decoder now uses resident translations even without the shared cache.
3. The strict C++ guest-memory fixture now matches native stdout, stderr
   and exit status. It covers read-only sysctl, Mach VM allocation, map,
   protection splitting and deallocation, plus the dyld image slide.
   A reversed split comparison initially dropped the leading page during
   middle-page mprotect. Correcting it preserved both neighboring ranges.
   The launcher was passing the cached task-port slot address as the port
   value. Using the synthetic port value `0x103` made the allocator's Mach
   VM map and protect calls succeed. The strict C++ snapshot with cache
   metadata services 122, 127 and 128 advances unchanged
   `/usr/bin/plutil -lint` on valid and malformed fixtures to Darwin trap `-24`
   (Mach port construction) at guest PC `0x100848b18`.
   A direct service 122 fixture matched the native entry count and complete
   message-send-offset table, 288 bytes in all.
   A later strict snapshot passed the raw Mach traps `-24` and `-25`
   on the M4 Pro. It matched all 48 output bytes and exit status for plain
   and guarded receive right construction, distinct names, destruction
   with matching context, and the null-name error. The first implementation
   used guard bit `0x02` and failed this probe. The SDK constant is `0x01`.
   The same snapshot advances unchanged valid `plutil -lint` to service
   125, Objective-C callback registration. A preceding Mach message
   snapshot reached that service on both valid and malformed inputs.
   The callback runner and Mach-O section lookup now link in a strict C++
   snapshot. Both unchanged valid and malformed `plutil -lint` invocations
   advance to service 126, the dyld shared-cache range query, before emitting
   a lint result. That snapshot's launch gate retains 39 native matches
   before pthread_mutex_lock. Service 126 now returns the cache base,
   mapped span and readable cache header. The focused range fixture
   matches all 24 native bytes (size `0x1b9b14000` and 16-byte magic)
   and exits 0. Unchanged valid and malformed `plutil` both advanced to
   service 124, the dyld section-info query. Service 124 is now wired.
   A direct native/Metal ABI fixture matched all eight bytes and exit
   status, including the section address relative to the fixture symbol,
   section size, bytes read through that address and zeros for an absent
   section. Strict section-enabled runs advance both unchanged
   `plutil` inputs to service 133, Objective-C class enumeration.
   The class enumeration implementation now gathers cache offsets from
   the host and uses Metal callback frames for guest code. A native
   differential fixture matched all three output bytes and exit status
   for a real NSObject callback, a guest stop-flag write and an absent
   class. Both unchanged lint invocations advance to Darwin syscall 381
   (`__mac_syscall`) at PC `0x10084a728`. The bounded Sandbox
   self-query implementation now matches native carry, return value and
   decision word on all three fixture cases. The strict build matched
   all 19 output bytes and exit status. A later strict build also matched
   EFAULT and carry for three non-null invalid guest pointers (operation,
   path and output), with the decision word unchanged. Both unchanged lint replays now
   pass the query and hit an Objective-C realization abort:
   `malloc_size(0x200004104090) = 0`. Syscall 521 is the resulting
   `abort_with_payload`, after libobjc's diagnostic. The C++ direct
   branch decoder recognizes cached malloc_size, but the reachable
   entry still emits the host heap service unconditionally. A strict
   scratch port of the Swift entry routing sends guest VM pointers to
   cached libmalloc on Metal. Both unchanged lint inputs clear class
   realization and reach service 135 (`dlopen`). Land the route in the
   shared runner and model loaded-image handles for `RTLD_NOLOAD`.
   The focused native control returns five passing bytes for an
   already loaded libz, stable handles, an absent path and two matching
   closes. The prior C++ build stops at service 135 on that fixture too.
   The new header-only loaded-image helper handles aliases and reference
   counts without loading host code. A strict scratch runner wired to
   the helper matches all five native bytes and exit status. Both
   unchanged lint replays use that runner. Evidence is under
   `cpp-noload-path` and `cpp-noload-integrated-path`.
   A real cache-only CoreFoundation alias fixture matched all seven
   native bytes and exit status, including canonical-path identity,
   `RTLD_FIRST` distinction and three closes.
   The shared runner now includes both the allocator route and loaded
   image handlers. Its strict build passes both NOLOAD fixtures and
   the native 48-byte host-heap allocation-size fixture. Both prior
   plist replays advance past dlopen and fault reading address `0x10`
   at guest PC `0x1114396ac`, the prior Swift main-header gap. A focused
   main-header fixture reproduced the failure. The runner now passes
   `image.main_header` into bootstrap, which service 129 uses.
   Both header getter aliases now match the 32 native header bytes and
   exit status with cache disabled and in demand mode. Both unchanged
   plist inputs pass the header read and reach service 138, the host
   feature-policy query. Service 138 now uses the Darwin bridge. The
   focused known/unknown flag fixture matches native `10\n`, empty
   stderr and exit 0. Both unchanged inputs are replaying with that
   build under `cpp-feature-stream-path`. Both inputs advance to
   unresolved `/usr/lib/libobjc.A.dylib:_objc_msgSend`; native dyld lists
   a re-export and the cached provider has real ARM64 text at export
   offset `0x800`. The cache parser now treats an empty imported name
   as a same-name re-export. A raw trie probe confirms this actual alias
   is empty, and umbrella/provider resolve to the same guest code at
   `0x108000800`. A native/Metal fixture calling the import returns
   identical integer and floating-point zero bits for a nil receiver.
   Both unchanged plist inputs advance to selector stub `0x1080045f0`,
   which the cache reader recognizes but the runner does not admit as
   executable text. A strict scratch port admits and decodes selector
   instructions. Both unchanged plutil inputs advance to Darwin 169
   (`csops`) at guest PC `0x10084a8d8`. The guest signing metadata
   helper is shared and matches the native fixture's status
   `0x22020201`. A scratch runner adds the bounded status syscall.
   Its first ABI replay caught address 1 accepted by raw guest backing.
   With a null-page guard, both ordinary signed and hardened controls
   match native status bytes, return and carry behavior, and exit 0.
   Both unchanged plist inputs pass signing and reach service 143,
   the Swift prespecialized metadata query. Wire the guest cache
   metadata pointer and ensure its provider image is mapped. The shared
   loader now discovers the cache metadata and maps its provider, and
   service 143 returns the guest pointer. The demand-mode fixture
   matches all 24 native bytes and exit status in resident and demand
   modes. The same strict build passes all 42 launch comparisons and
   external SIGUSR1. Both unchanged inputs advanced to service 144,
   the Swift cached pointer-hash lookup. A focused native fixture
   queries the real table with one and three cache-relative keys and
   reports two misses. The shared service now matches both native
   misses and exit status. The exact build passes 42 launch
   comparisons, external SIGUSR1 and 2,048 function comparisons.
   The byte-identical executable is installed at
   `.build/cpp-verified/arm64-metal` (SHA-256
   `da9bb4a7827fe033c151ba493624863e78825e03e1d82b2f5951fdf5c2f6cfd2`).
   Both plist inputs passed pointer-hash dispatch and advance to
   service 145, dyld's prebound lazy-dependency flag. The C++ runner
   now validates the owning cache image and flag, requires the
   dependency in the loaded guest graph, and writes exactly four bytes.
   The combined strict build passes 42 native launch comparisons,
   external SIGUSR1, and 2,048 native function comparisons. Snapshot
   SHA-256 is
   `47afc97ad827ec1ee77576bf740fc436232af8da4a682567a003ec4bcbd932f9`.
   Original replays are under
   `cpp-swift-hash-integrated-path`, with the new snapshot under
   `cpp-lazy-strchr-path`. Both unchanged plist inputs validate the
   flag metadata and report that `/usr/lib/swift/libswiftCore.dylib`
   is absent from the guest image headers. Determine whether the
   loaded graph uses the canonical install name or omits the image.
   Cache enumeration confirms the exact canonical name and identifies
   libobjc's LC_DYLD_LOADABLES edge, which the startup dependency
   traversal omits. Keep these edges separate from Mach-O ordinals
   and enqueue validated prebound targets in the bounded cache graph.
   The strict C++ build now validates LC_DYLD_LOADABLES metadata
   with the same parser used by service 145, and enqueues known cache
   targets recursively with the existing image caps and a separate
   4096-record budget. Both unchanged plist replays use the frozen
   build under `cpp-plutil-loadables-path`. The metadata control
   finds libobjc's aligned flag at `0x1643f00d0` in its `__DATA`,
   resolves the intended target, and rejects a one-byte-shifted flag.
   A separate plutil graph construction enumerates 668 images and
   finds both libswiftCore and the prespecialized provider. The same
   frozen build passes 42 native launch comparisons, external SIGUSR1
   and 2048 native function comparisons.
   Both full replays still failed the service 145 membership check.
   A scratch launcher using the actual host/memory import maps
   confirms 668 startup images, including libobjc and libswiftCore.
   The runner moved the header map into GuestImageHandles and later
   checked the moved-from map. Keep service 145 on the stable index.
   Exact startup evidence is under `cpp-plutil-graph-exact-path`.
   Service 145 now uses the stable path index and the forced strict
   build passes. A frozen dyld batch under `cpp-dyld-batch-path`
   compares unchanged valid and malformed plutil and installed Java
   with their native stdout, stderr and status. Java now reaches
   cached dladdr and faults at guest PC 0x100462138 when libdyld
   dereferences its uninitialized API table. Local libjli disassembly
   shows dlsym(RTLD_DEFAULT, "main") feeding dladdr, then using
   dli_fname to locate its runtime. Guest dladdr image/nearest-symbol
   metadata and native comparison fixtures are being developed.
   The native control under `cpp-dladdr-controls-path` verifies
   main/dylib addresses, an interior address, a private static
   symbol, and an unmapped miss. The frozen guest baseline rejects
   the unsupported import. Loader work preserves actual segment
   ownership and nearest defined local symbols. A strict standalone
   parser control finds the exact fixture's main export, private
   static symbol and dylib export in their __TEXT segments, in
   address order. A second native fixture checks cached libz function
   and libc data ownership, plus unchanged Dl_info sentinel fields
   on an unmapped address. A mixed-segment native control queries
   an executable function and a data symbol in the same image in
   sequence. The local symbol parser retains the installed
   equal-address tie behavior (the local alias wins in its native
   control). Service 165 matches native for executable and dylib
   code, private symbols, two segments of one image, exact/interior
   libz lookup and unmapped misses. Cached libc optind is still a
   miss because its modeled address lacks image ownership. The
   precise follow-up is an exact synthetic-slot alias derived
   from a verified cache export owner, with no widened image range.
   The loader now records that provenance for actual modeled bindings,
   rejecting ambiguous or unmapped ownership. The exact cache fixture
   resolves optind to libsystem_c header 0x100715000 and guest export
   0x1643fe00c in __DATA. Runner alias lookup is still pending.
   The
   frozen binary under `cpp-dladdr-app-batch-path` independently
   matches the getrlimit control and replays both plutil inputs
   plus installed Java. The unchanged Java replay advances beyond
   dladdr and reaches raw Darwin 220, getattrlist, at guest PC
   0x100849628. Add a checked path-based bridge and preserve raw
   syscall carry/errno semantics. The raw SVC control under
   `cpp-getattrlist-raw-path` records native carry clear and an
   eight-byte VREG result, then carry set with ENOTDIR or EFAULT.
   ENOTDIR leaves the caller buffer intact. The old runtime
   reproduces unsupported syscall 220 on this control. The direct
   host getattrlist bridge now matches native output, missing path,
   undersized buffer and invalid pointers. A stronger raw control
   confirms failing trap 220 and trap 340 leave preexisting guest
   errno untouched, separately from the errno returned in x0. A
   strict scratch integration under `cpp-raw-fs-integrated-path`
   matches both raw traps on Metal for carry, result, output and
   untouched errno. Shared runner routing now also passes both
   controls. The same strict build matches the read-only CSR raw
   fixture and the complete cached dladdr fixture, including optind.
   The next unchanged app batch is frozen under
   `cpp-raw-csr-app-batch-path`. On that frozen binary the launch
   gate passes 42 comparisons and external SIGUSR1. Both function
   gates pass 1,024 comparisons each. Static
   libjli inspection shows a later named JVM load with RTLD_NOW |
   RTLD_GLOBAL and three JNI dlsym requests. The installed server
   libjvm is 20,222,928 bytes on disk and maps 16,089,088 bytes across
   three segments. That leaves little room in the initial 16 MiB
   anonymous backing. The exact guest JVM load is still unobserved.
   A dynamic-load transaction needs image backing, relocation,
   ordered visibility, and guest Metal constructors.
   A native dynamic-dlopen fixture proves dependency-first constructors,
   global visibility, stable repeated handles and hidden state after
   a failed open. The same guest control stops at named NOLOAD-only.
   The installed libjvm now parses after applying validated legacy
   rebases before reading initializer and TLS pointers. All 128,617
   rebase slots and unslid targets match dyld_info -fixups. The first
   slot at 0x00da0048 points to unslid 0x21cc and guest 0x61cc at
   placement 0x4000. The decoder emits one slot for
   DO_REBASE_ADD_ADDR_ULEB and rejects malformed operands and segments.
   The loader extracts 8,243 initializer targets and two dependencies.
   A strict snapshot launches the legacy dylib pair as startup
   dependencies. Metal matches native stdout, stderr and status 0,
   including dependency-first constructors and result 42.
   Exact outputs are under `cpp-dynamic-jvm-inspect-path` in
   `inspect-rebased.out` and `legacy-startup`. Legacy binding and
   named dynamic loading are separate open work.
   Apple dyld and Astra review establish the transaction boundary.
   Validate dependencies and required NOW binds before publication,
   pin the handle before callbacks, and run constructors dependency-first
   on Metal. Preserve per-image initialization state and per-thread
   continuations for nested opens. Roll back failed mapping or binding
   before constructors start. See Apple dyld DyldAPIs.cpp and Loader.cpp.
   The legacy regular bind stream may end exactly at its declared
   size without a DONE opcode. Installed libjvm ends with DO_BIND
   at file offset 0xed0167, and Apple BindOpcodes.cpp accepts the
   bounded stream. All 535 JVM bindings match dyld_info by slot,
   kind, symbol and library (308 regular, 227 lazy). The combined
   MachImage inspection reports 535 bindings and 8,243 initializers.
   Startup uses the existing binding table. Weak-definition coalescing
   and runtime lazy binding still require dedicated semantics. Evidence
   is in bind-compare.out and inspect-bound.out under
   cpp-dynamic-jvm-inspect-path.
   On the frozen raw-CSR binary, unchanged Java exits 1 at
   pthread_detach with error 3 (ESRCH). Native Java exits 0. This is
   the observed launcher gap before named JVM loading. Scheduler
   detach state and runner dispatch are separate work in progress.
   The strict legacy-bind binary independently matches the linked
   dylib startup fixture in stdout, stderr and status. The native
   nested dlopen fixture in /tmp/cpp-dynamic-jvm-inspect.oRVONa
   establishes dependency, target begin, nested side, target end
   ordering, self-reopen and lookup from the target constructor,
   global visibility, stable repeated handle, and one call per
   constructor. Its GPU comparison awaits named image loading.
   The frozen malformed plutil exits 1 at host service 149,
   __dyld_find_protocol_conformance. Native exits 1 with a missing
   dictionary value diagnostic. C++ already contains a shared-cache
   lookup that the runner does not call. The corresponding Swift
   launcher provides the dispatch reference. Validate real cache
   hits and misses before wiring the x0 kind / x1 guest pointer ABI.
   Swift skips cache conformance sections when dyld optimization
   is active, so returning a blanket miss could drop valid results.
   Scheduler detach now models live detach, completed detach,
   repeated detach, join rejection and retirement at thread exit.
   Strict components and the lifecycle fixture pass. A native
   control matches ESRCH and EINVAL cases. Runner service 174 and
   the full native/Metal detach comparison are pending.
   The first broader legacy-bind gate attempt stopped at an invalid
   local pipe fixture path. The corrected run uses the Makefile
   fixture names and records its result under bind-gate-path.
   The corrected gate exits 0 with 42 comparisons and external
   SIGUSR1 parity on the strict legacy-bind binary.
   The staged graph build retains canonical path, expanded rpaths,
   dependency ordinal order and resolved guest headers from normal
   startup. The legacy fixture resolves 16384 to 49152 to 98304
   and matches native constructor order and output. Strict binary
   is under /tmp/cpp-staged-graph-build. Transactional append and
   publication are the next named-load steps.
   Runner now dispatches pthread_detach through service 174 and
   reclaims completed detached thread allocations. The frozen
   detach-loaded snapshot matches native stdout, stderr and status
   for live detach, repeat detach, join rejection and worker run.
   On the same binary, named RTLD_NOW|RTLD_GLOBAL succeeds for an
   already-mapped dylib. Its constructors run once, repeated opens
   reuse the handle, dlsym returns callable Metal code, and both
   closes succeed. Native and GPU outputs, diagnostics and status
   match. See detach-loaded-path under cpp-dynamic-jvm-inspect-path.
   The frozen raw-CSR tar and CoreFoundation probe both exit 1 at
   ARM64 word 0x6f03f601 at guest PC 0x103464758. Lowering this
   instruction is the next shared startup fix. Valid plutil remains
   live on the same frozen batch at the time of this entry.
   Valid plutil later exits 1 at the same Swift conformance service
   149 as malformed plutil. All five raw-CSR runs are terminal.
   Runner service 149 now dispatches to the shared-cache lookup,
   returning x0=0 and x1=guest descriptor on a loaded-owner hit,
   or x0=2 and x1=0 on a miss. The strict build passes. Native
   cache-hit validation and the new plutil replay remain open.
   Named-load staging now accepts a high logical placement and
   builds a dependency-first list without publishing it. The host
   control in /tmp/stage-named-probe stages two legacy dylibs with
   one constructor each and five target binds. A missing dependency
   fails and leaves the existing image graph, mapped segments and
   backing unchanged. Guest publication and callback execution
   still require runner integration.
   Frozen service149 binary matches native for three conformance
   hits and two misses in the demand-cache Swift fixture. The
   no-cache miss also matches. Descriptor values are compared as
   cache-relative offsets. The predicate currently uses mapped
   image membership, which includes prebound lazy-only images.
   Exact active-owner parity still needs activation tracking.
   C++ now defines the hash result at shift 64 without undefined
   behavior. See service149-path under cpp-dynamic-jvm-inspect-path.
   Word 0x6f03f601 decodes as FMOV V1.2D, #1.0. The vector
   immediate lowering covers .2D, .4S and .2S and matches all 160
   native fixture bytes across ten immediates. The next unchanged
   app batch is frozen under cpp-detach-swift-fmov-batch-path.
   Strict snapshot SHA-256 is
   b1150fa69a8058905fec6964dbfd20d423bbc75783acd88a3f9ef779f3f31ed2.
   The first launch omitted shared-cache flags and stopped early.
   Those outputs are retained under no-cache-control. The corrected
   batch explicitly uses SHARED_CACHE=1, CACHE_DEMAND=1 and
   CACHE_BOOTSTRAP=1 with the ARM64_METAL_ prefix. CoreFoundation
   and tar additionally enable cached initializers. An independent
   launch gate is running on the same binary.
   The gate exits 0 with 42 native launch comparisons and external
   SIGUSR1 parity. The same binary passes 1,024 function comparisons
   each for update_words and xor_words. The frozen loader stages the
   actual installed libjvm at 0x120000000 without guest publication.
   Its 16,089,088 bytes include 8,243 relocated constructors
   (first 0x1200021cc), 535 bind slots and 227 stub entries. Slots
   divide into 244 libSystem and 291 libc++; stubs divide into 224
   and 3, respectively. See jvm-stage-path under the JVM scratch root.
   These are image and binding-plan inputs. Resolver and publication
   tests are separate from this stage-only control.
   The runner now stores callback frames by guest thread and
   keeps Objective-C cache write permissions while any thread
   owns a mutation frame. Retiring a guest thread discards its
   remaining callback allocations. A strict isolated runner build
   matches native stdout, stderr and status for return, exit,
   __cxa_finalize, _Exit, and relocated arm64e TLS. Receipts are
   under cpp-thread-callback-check-path. Cross-thread sentinel
   ownership and nested dlopen still need their direct GPU checks.
   CoreFoundation probe on the corrected frozen batch passes
   the prior FMOV site and exits 1 at AUTDB PC 0x1009e9474. The
   pointer loaded from 0x1663d76f0 is zero, with modifier
   0xcb870001663d76f0. The installed cache stores zero on disk
   with no rebase for that __DATA_DIRTY slot. Native CFInitialize
   fills it with an authenticated allocator bridge class. Cached
   initializers were enabled for this launch; trace the planned
   CF initializer and slot across residency before changing PAC.
   Host inspection finds CFInitialize scheduled last among 19
   images and 444 constructors. The cache parser drops upward
   dependency attributes on Foundation and CoreServicesInternal.
   Dyld defers these links until normal dependencies finish.
   Correct that ordering before evaluating the remaining PAC fault.
   Java still exits 1 at pthread_detach ESRCH on the cache-enabled
   replay despite the local service174 fixture. Verify cache
   routing and actual pthread handle provenance before replay.
   Actual installed Java resolver probe uses 0x400000000000,
   outside its cache mappings. Without modeled host services it
   resolves all 291 libc++ slots and all three libc++ stubs, but
   direct libSystem lookup finds 57 of 244 slots and 52 of 224
   stubs. Remaining libSystem symbols need reexport traversal or
   their scoped host service. Evidence is in jvm-resolve-path.
   Both unchanged plutil inputs pass service149 and stop at
   ARM64 word 0x1e611800, PC 0x115242ecc. Tar passes vector
   FMOV and reaches the same uninitialized CF allocator slot as
   the probe. All five replays are terminal.
   Cached pthread_detach used the decoder table but was missing
   from the runner host-export table. The runner now lists 174
   in both paths. A new cached differential is pending. The old
   diagnostic fixture waited on its worker after detach failure
   and was stopped.
   The direct callback discriminator passes on the per-thread
   build. A guest worker returns during Objective-C enumeration,
   then the main callback resumes and joins the worker. Native
   and Metal outputs, diagnostics and exit status match. The old
   global stack skips callback resumption and faults.
   SharedCache now preserves upward edges (two in installed CF)
   and exposes declared dependency closure for lazy activation.
   Native CryptoKit conformance misses before open and resolves
   descriptor offset 0x5bb38218 afterward. The cache-only closure
   control finds 508 headers including the root, without following
   prebound lazy edges. Runner activation closure and initializer
   traversal are separate integration work.
   The corrected cached detach route passes the bounded differential.
   Native and Metal return detach 0, repeat 22, join 22 and worker 1
   with status 0. The prior frozen cache binary returns detach 3,
   repeat 3, join 0 and worker 1 with status 1. See cached-detach-path
   under the JVM scratch root. Unchanged Java awaits the next batch.
   Service145 now activates the full declared cache dependency
   closure in runner state. The strict build passes. The live
   Swift conformance before/after control still needs guest coverage.
   Staged symbol resolution now distinguishes local, startup-cache
   and modeled service targets, weak null and required NOW failure.
   Remaining JVM libSystem imports need cache owner provenance and
   mapping activation, because the demand startup graph only
   materializes providers for imports it sees at launch.
   Native scalar FDIV control in /tmp/fdiv-native-flags.08eRTC
   verifies signaling NaN priority and the FPSR differences between
   gradual underflow and FPCR flush-to-zero. The Metal implementation
   is being checked against those native semantics before replay.
   SharedCache resolveExport follows export ordinals and reexport
   edges and returns both target and owning cache image. The
   independent installed JVM corpus checks 176 previously unresolved
   libSystem names against native dlsym and dladdr, including owner
   header and address. 174 match, with strcmp and strncmp still
   native-only in this lookup. See jvm-cache-providers-path.
   Native DYLD_PRINT_INITIALIZERS for the exact unchanged allocator
   fixture exits 0 and puts CoreFoundation fifth, before SwiftCore
   and Network. The prior guest plan placed it nineteenth. The
   native trace and image ordering are in cf-native-init-path.
   strcmp and strncmp resolve through libsystem_c to arm64e
   libsystem_platform dynamic-resolver exports. Native selects
   concrete targets; these are distinct from stub/resolver entries.
   Existing host services cover these names while general resolver
   execution remains open.
   The isolated Metal FDIV control matches signaling NaN and
   gradual subnormal cases. It still differs for flush-to-zero
   because guest MSR FPCR masks off bit 24. Preserve the modeled
   flush-to-zero and default-NaN bits, then repeat the raw binary
   comparison under cpp-fdiv-flags-check-path.
   Installed macOS also uses dylib_use_command with marker
   0x1a741800 and delayed-init flag 0x8. Examples include
   CoreText to ATS and Foundation to libcmark-gfm. Parse these
   flags and distinguish delayed edges from legacy upward links.
   FPCR writes now retain FZ and DN. The exact six-case raw native
   comparison matches stdout bytes, stderr and exit status. A second
   deterministic fixture tests 1,024 S and 1,024 D divisions across
   sixteen FPCR settings. All result bits match, but 77 FPSR words
   contain extra IXC on FZ underflow. See cpp-fdiv-fpcr-path.
   The delayed-init cache parser passes installed controls for ATS,
   TextRecognition, libODIECompiler and libcmark-gfm. The startup
   traversal still needs to consume these flags. Seven of nine extra
   guest initializer images are behind delayed edges, so the native
   plan comparison also checks eager roots reached via upward edges.
   Final FDIV lowering flushes tiny results before rounding or IXC,
   preserving only UFC for output flush. A strict isolated snapshot
   matches six raw edge cases (96 bytes) and all 2,048 matrix
   divisions (32,768 bytes) against native ARM64, including FPSR,
   diagnostics and exit status. Evidence is under cpp-fdiv-final-path.
   Modern delayed-init flags are now preserved by the cache parser;
   Apple dyld partitions delayed-only images out of the active list
   and excludes their constructors until activation. Startup traversal
   and active-scope integration remain separate checks.
   Initial runner publication matches native for a previously
   unloaded chained-fixup dylib and dependency: both constructors
   execute on Metal, dlsym calls return 42, global visibility is
   correct and repeat open reuses the handle. A real missing strong
   flat data binding fails before constructors, keeps its symbol
   hidden and RTLD_NOLOAD absent, then a valid retry succeeds. All
   output, diagnostics and exit status match native. The legacy
   @rpath pair still stops at dyld_stub_binder in staging. Evidence
   is under named-publication-path. Publisher addends, authenticated
   rebases and preflight before writes are under review.
   The first revised CF host plan puts CF before SwiftCore and
   Network and removes seven extra initializer images. It still
   includes CoreDisplay and SkyLight, and schedules libsystem_trace
   later than native. The diff is in cf-native-init-path.
   Final corrected startup traversal uses exactly two deferred
   upward passes, preserving delayed activation and ordering modeled
   libSystem dependencies first. Native linker trace confirms
   CoreDisplay and SkyLight are eager but beyond the executed
   upward passes for this launch. The strict plan matches all
   385 native constructor addresses across ten images after
   subtracting cache slide and excluding modeled libSystem itself.
   See /tmp/cf-startup-order-comparison.json and cf-native-init-path.
   The strict NOW installed Java graph stages all 535 JVM slots
   and 227 stubs with existing strcmp and strncmp service IDs.
   It stages one new local image of 16,089,088 bytes and reports
   zero additional cache provider images. See jvm-now-stage-path.
   The follow-up audit finds no cache target outside mapped image
   segments. This remains a staging check; the Metal load is pending.
   The combined app freeze encountered an unused publication validator
   while runner integration was in flight. No app replay was started.
   The subsequent strict combined build passes 42 native launch
   comparisons and external SIGUSR1. The same binary starts Java,
   both plist inputs, the allocator probe and tar at 04:21:29 UTC
   with explicit shared-cache flags. Evidence is in
   cpp-fdiv-init-publication-batch-path.
   On that binary the real missing strong binding rollback still
   matches native. Legacy @rpath now runs both constructors and
   resolves the handle symbol, then faults on a mapped data read.
   The nested fixture faults earlier on its dependency counter load.
   Native disassembly identifies both reads, and Metal guest_offset
   takes its linear route for their low guest addresses, bypassing
   the resident rows. Move named images into a distinct high guest
   arena and rerun both discriminators.
   The strict high-arena snapshot passes all three native
   differentials: legacy @rpath with data reads, missing-symbol
   rollback, and nested constructors with a self-reopen and a new
   side image. Outputs, diagnostics and exit statuses match.
   The nested test omits only its unsupported setvbuf instrumentation;
   native and Metal use the same resulting binary. See
   cpp-high-named-load-path. A 20 MiB named BSS control returns 42
   natively and hits the fixed backing limit on Metal, providing a
   discriminator for capacity growth.
   Unchanged Java passes the former detach failure and exits 1
   at unsupported ARM64 word 0x1e692120, PC 0x1009116f4.
   The scalar FP compare lowering is the next Java instruction fix.
   The named-image backing growth build loads the 20 MiB BSS
   control and returns 42. A mixed control allocates an 80 MiB
   guest heap region afterward, writes its endpoints, and returns
   42 from both heap and original dylib data. Native and Metal
   outputs, diagnostics and status match in both cases. See the
   growth-path receipt under cpp-high-named-load-path. A remaining
   review item is excluding grown physical backing from the low
   linear guest address path.
   The alias negative control computes the original linear extent
   at 0x580c000. After loading the 20 MiB dylib, the unguarded
   Metal binary reads the dylib Mach-O magic CF FA ED FE through
   that low address. A non-readable resident/host range guard is
   in progress; rerun the exact alias and mixed-growth controls.
   The corrected initializer-order batch advances both the allocator
   probe and unchanged tar past the previous authenticated-null fault.
   Both stop at Darwin syscall 286 (gettid), PC 0x10084a880.
   Raw uid/gid copyout and error semantics are being modeled.
   FCMP/FCMPE S/D focused comparison passes. A larger independent
   matrix reaches unsupported MRS NZCV before comparison; the
   register instruction is a separate missing decoder path.
   Valid plutil passes scalar FDIV and stops at FRINTP D0,D0
   (0x1e64c000) at PC 0x115242ed0. The scalar integral rounding
   lowering is the next plist instruction step.
   MRS/MSR NZCV now use architectural bits 31:28. The strict
   independent FCMP/FCMPE matrix matches all 2,048 comparisons
   across S/D register and zero forms, including NZCV and FPSR,
   stderr and exit status. See cpp-fcmp-nzcv-path.
   Native raw gettid without a per-thread identity override returns
   ESRCH with carry set before inspecting either pointer. Seven
   valid/null/invalid combinations leave both outputs unchanged.
   This verifies the observed startup state, not assumed-identity
   copyout. The host bridge is in progress.
   The strict alias guard snapshot blocks the low physical alias
   both in translated LDR and through host write. The unguarded
   control emits the dylib Mach-O magic through both paths; the
   guarded control emits no bytes from the alias. The mixed
   20 MiB dylib and 80 MiB heap test still matches native.
   See cpp-mapping-alias-guard-path.
   gettid service175 now models no guest identity override. Its
   direct host sanitizer control passes 16 pointer combinations;
   raw runner wiring and native/Metal comparison are pending.
   Strict FRINTP snapshot matches 2,048 S/D native operations
   across 16 FPCR settings, including NaNs, signed zero, FZ and
   FPSR. The 32,768-byte differential and status are under
   cpp-frintp-matrix-path. Unchanged plutil replay awaits the batch.
   The next strict combined snapshot includes FCMP, NZCV, FRINTP,
   raw gettid and the mapping alias guard. Seven raw gettid cases
   match native output (413 bytes), empty stderr and status 0.
   Java, valid plutil, allocator probe and tar start at 04:33:10 UTC.
   The malformed replay is queued after the previous unchanged
   malformed process ends. See cpp-fcmp-frintp-gettid-batch-path.
   A separate scope control shows native RTLD_LOCAL hides only
   the requested root, not its newly loaded dependency. Reopening
   with RTLD_GLOBAL promotes that root with the same handle, and
   RTLD_NOW without RTLD_LOCAL exposes a new root globally. The
   high-arena runtime currently fails promotion and default global
   visibility. Receipts are under scope-path in the high-arena root.
   The next strict scope build matches native local-root hiding,
   ordinary dependency global visibility, promotion of the same
   handle and default-global mode. See cpp-dlopen-scope-path.
   A separate named local dylib calls libz crc32 without a startup
   libz dependency. Native returns 27515853 and exits 0; the frozen
   runtime fails the transaction at unresolved staged _crc32.
   This is the cache-provider publication discriminator, under
   cache-provider-path in the JVM scratch root.
   The FCMP/FRINTP/gettid frozen launch gate exits 0 after 42
   native comparisons and external SIGUSR1 parity.
   Two native caller dylibs use the same @loader_path spelling
   to open different canonical files and return 11 and 22. The
   scope build returns 11 twice because the raw alias is global.
   The same-install-name @rpath control returns 11 twice on both
   native and Metal, so this is specifically a caller-relative
   identity bug. See rpath-caller-path in the high-arena root.
   The cache-only libz root native control verifies NOLOAD miss
   before open, stable handle after open, and crc32=27515853.
   It joins the dependent-provider fixture under cache-provider-path.
   The old malformed plutil replay exits 1 at the same FRINTP
   instruction as valid input. Its replacement on the FRINTP
   snapshot starts at 04:38:16 UTC after the old process ends.
   Java on the FRINTP snapshot passes FCMP and reaches the CF
   authentication fault when cached initializers are omitted.
   Native DYLD_PRINT_INITIALIZERS confirms CF initialization
   before libjvm. The same frozen binary now replays Java with
   CACHE_INITIALIZERS=1; prior Java run is terminal.
   Both frozen function verifiers pass, 1,024 comparisons each.
   Cache-only libz host staging with the actual runner service
   maps reaches the new cached-root path and rejects the negative
   cache slide. Guest cache addresses are below preferred VM
   addresses; startup already uses modular slide subtraction.
   Fix staged slide representation before cache publication. See
   cache-root-stage-path/runtime.err.
   Caller-relative resolution now canonicalizes @loader_path
   against the actual calling image before handle lookup. The
   strict snapshot matches native 11 and 22 with both callers
   using the same spelling. See cpp-loader-path-fix-path.
   Signed cached slides now pass host staging with the runner
   service tables. The libz root and local crc32-dependent dylib
   each stage 46 cache image records. The local stubs and slots
   retain libz as their deferred provider. See cache-root-slide-path.
   Runner segment publication and guest cache code execution remain open.
   The strict cache-root host control starts with zero cache
   mappings. Libz stages 46 cache entries and CryptoKit 508,
   with no new local images. CryptoKit lists about 817 MiB of
   segment metadata spans; demand publication must avoid making
   all of those bytes resident. Mapped and startup-active flags
   are separate; active does not prove constructor execution.
   The installed libjvm JNI control loads the actual dylib and
   calls JNI_GetDefaultJavaVMInitArgs and JNI_GetCreatedJavaVMs.
   Native returns success, JNI version 65544 and zero created VMs.
   Its Metal baseline is running. See jni-entry-path.
   The alias guard initially rejected accesses starting inside
   backing but missed overlaps. A 4-byte read starting at
   0x580bffe crosses into the guard and emits 00 00 CF FA
   through both Metal LDR and host write. Fix interval checks
   on both sides and copy the physical Metal buffer directly
   during growth. See cross-gpu and cross-host receipts under
   cpp-mapping-alias-guard-path.
   Java with cached initializers exits at a new null access
   at PC 0x1004618e0 (x10=0x1663b9000, tp=0x502a2c0).
   The prior CoreFoundation authentication fault is absent.
   Exact function and operand provenance are under investigation.
   Installed cache disassembly identifies _dyld_has_inserted_or_interposing_libraries.
   The wrapper loads x0 from dyld4::gAPIs, which is zero on disk and
   has no cache fixup. Native dyld bootstrap supplies an authenticated
   object/vtable. The same null wrapper fault is confirmed on Java,
   the allocator probe and tar. A scoped host-service route is in work.
   The last valid/malformed plutil and JNI baseline PIDs disappeared
   without terminal status receipts during the gap in observation.
   Cache publication reaches guest constructors in libz provider/root
   controls. The commpage capability query at 0xfffffc100 is wired.
   Provider subsequently reaches raw Darwin 177 (kdebug_typefilter).
   Cache-root also reaches Mach trap -28 (pid_for_task), now routed
   but without independent valid-task output verification.
   Native NOLOAD misses on mapped-but-inactive CryptoKit. The prior
   strict runtime returned a handle. Verify the new active-header
   gate with the exact fixture under /tmp/cpp-dynamic-jvm-inspect.oRVONa/noload.
   The latest frozen named-cache runtime returns null for inactive
   CryptoKit, matching exact native stdout, stderr and status.
   The recalculated guard with shared-cache reservation starts at
   0x980c1b0. A crossing 4-byte read at 0x980c1ae fails on both
   Metal LDR and host write without leaking bytes. The earlier
   0x580c000 probe was using obsolete geometry. Receipts are in
   cpp-noload-active-path and cpp-alias-overlap-path.
   A separate native dylib pair built without chained fixups verifies
   legacy constructor order and runtime symbol visibility. Its target
   has a real initialized data rebase and a lazy bind resolved under
   RTLD_NOW. Source and dyld_info evidence are under /tmp/guest_dlopen_legacy_*.
   The frozen binary
   SHA-256 is `5826c1c97141ca38206fb367d851dcefe8bd5551918243e4b7866064a9b3c0c6`.
   On that exact binary, RTLD_DEFAULT, dlopen(NULL)/dlerror and
   pthread error isolation match native. Both unchanged plutil inputs
   pass the fixed lazy-load check and reach unsupported host service
   41, getrlimit. The native status remains 0 for valid input and 1
   with parser diagnostics for malformed input. Port the checked
   resource-limit bridge before the next app batch. The host service
   now copies Darwin rlimit into checked writable guest memory,
   preserves output on error and returns guest errno. Strict
   component compilation passes. The frozen native/Metal probe
   under `cpp-getrlimit-path` matches valid, invalid-resource and
   null-buffer results, including unchanged output on failure.
   Both unchanged plist inputs pass getrlimit on the next frozen
   batch and stop at raw Darwin 340, lstat64, guest PC 0x1008548d0.
   The native raw control under `cpp-lstat64-raw-path` confirms
   symlink type/size, ENOTDIR without output changes, and EFAULT
   for a null destination. The old guest reports the missing trap. The launch gate passes 42
   comparisons and external SIGUSR1. Another 2048 function
   comparisons pass.
   Earlier evidence is under `cpp-prespec-integrated-path` and
   `cpp-swift-hash-abi-path`.
   Selector stub and code-signing paths are now also in the shared
   runner. Both code-signing controls independently pass its strict
   build under `cpp-shared-bridges-path`.
   Evidence is under `cpp-csops-path`. The malformed replay and
   scratch changes are under
   `cpp-selector-probe-path`. Re-export evidence is under
   `cpp-objc-reexport-path`.
   Shared evidence is under `cpp-shared-noload-path` and the header
   verification run is under `cpp-shared-header-path`.
   The before and after replays are under `cpp-sandbox-path` and
   `cpp-malloc-route-probe-path`.
   The stronger class probe matched native's cache offset 1715141744
   and the loaded state, and independently observed the unloaded state
   with the same offset. Class callback evidence is under
   `cpp-class-enum-retry-path`.
   Prior evidence is under `cpp-objc-callback-path`,
   `cpp-cache-range-path` and `cpp-section-query-path` in the scratch root.
   The initial Mach message path recognized restartable-range
   messages. Full range validation, registration and recovery still need
   native behavioral evidence. Snapshot and outputs are under the directory
   recorded by `cpp-mach-guard-path` in the scratch root.
   For guest dlsym, reuse the parsed export tables and re-export resolution.
   Native dyld signs function results with IA and modifier zero.
   New-image dlopen, symbol lookup on guest handles, and dlerror remain open.
   The internal wall-time entry with an optional timebase output, general
   cache initializers, and Mach-O TLS remain open.
4. Keep the frozen integrated gate green after further changes. Focused
   host probes pass `_strdup` and `_asprintf`. In the fresh unchanged binary
   census, sort stops at `_semaphore_create`; grep and sed both stop at
   `_regcomp` even in demand-cache mode. The decoder now routes
   `_malloc_type_realloc` through guest realloc. A focused fixture matched
   native bytes and exit status after growing and shrinking an allocation.
   The regex exports exist in cached `libsystem_c`, while the unchanged
   tools import through the libSystem umbrella. Resolve those re-exports
   to cached guest code for translation. A direct-link fixture bypassed
   the umbrella and reached original cached code on Metal. Native regex
   produced four passing match/offset bytes, and the Metal run stopped at
   guest PC `0x1007315a8`, word `0x6943a909`. LLVM disassembled the word
   as `ldpsw x9, x10, [x8, #0x1c]`. Signed pair-load decoding now passes
   124 checks on the M4 Pro, covering native signed values and address
   updates, overlap rejection and fault retry. The original cached regex
   replay then reached guest PC `0x10078b14c`, word `0x1e614008`, which
   LLVM disassembles as `fneg d8, d0`. Scalar FNEG S/D now matches 410
   focused native checks, including raw NaN payloads, signed zero,
   vector zeroing and FP state. The first replay is recorded by
   `cpp-ldpsw-path`. The demand-mode re-export resolver now routes both
   unchanged grep and sed into cached code, where both stopped at
   `ldr w9, [x8, #0x40]!` (word `0xb8440d09`, PC `0x10073256c`).
   The missing pre-index W load now passes the 1,042-check GPU/native
   writeback matrix. Grep now reaches option parsing and file handling,
   but treats `-E` and `a` as file operands and rejects the stream in
   `ferror`. Successful libSystem re-exports were qualifying names and
   bypassing bootstrap's modeled globals. The loader now retains bare
   names for those bindings. The unchanged grep graph verifies
   `_optind`, `___stdinp` and `___stderrp` stay bare. Unchanged
   `grep -n -E a` now matches native stdout (12 bytes), empty stderr
   and exit 0 on the stdin fixture. The named-input no-match case
   also matches native stdout, stderr and exit 1. The invalid pattern
   exits 2 natively. The first Metal run stopped at
   `cmeq v0.16b, v0.16b, #0` (word `0x4e209800`, PC `0x100894ee8`).
   CMEQ now passes 964 native/Metal checks, and the invalid-regex run
   advances to `fprintf`, which rejects the supplied stream. With the
   updated loader, unchanged `sed -n p` reaches cached `flockfile` at
   `0x100725598`; `ldr x8,[x19,#0x68]` at `0x1007255b4` attempts to
   dereference opaque host stream handle `0x7000000000000000`.
   A focused `fgets` fixture reproduces the exact fault. The new host
   handler plus scratch import routing matches all 18 native bytes,
   including embedded NUL, EOF and untouched bytes. Unchanged sed still
   reaches the same fault through another path. It also imports
   `getline` and `getc`. The focused `getline` fixture reproduces the
   fault before routing. After routing, all 308 bytes match native
   across heap growth, embedded NUL, EOF and an unterminated final line.
   Unchanged sed still reaches the same fault with both imports routed.
   The scratch `getc` alias to existing host `fgetc` clears the fault.
   Unchanged `sed -n p` then reached service 90, `fputc`. The first
   handler used buffered host stdio, which reordered newlines after
   immediate writes. A focused `fputc(-1)`, `fflush` and `write` probe
   caught the same byte reordering. Modeled stdout/stderr now write
   directly. The focused probe and unchanged `sed -n p` match native
   stdout, stderr and exit status in the strict scratch build. Its
   runner included the three stdio import mappings. Unchanged
   `sed 's/a/x/'` also matched native stdout, stderr and exit status.
   The three mappings are now in the shared runner, and an unmodified
   shared-source build independently matches `sed -n p`. Evidence is
   under `cpp-ordered-fputc-path` and `cpp-shared-stdio-path`.
   On the strict header-enabled build, unchanged `head -n 1` also
   reaches `pthread_mutex_lock` (67). Unchanged `wc -l -c` reaches
   Darwin 478 with arguments `(0x1000, 1, 0, 0)`. Apple XNU defines
   that subcommand as `BSDTHREAD_CTL_WORKQ_ALLOW_KILL`; it is used on
   abort paths as well, so trace the caller before treating it as the
   first causal failure. Evidence is under `cpp-shared-header-path`
   and `cpp-wc-syscall-path`.
   A fresh unchanged gzip decompression matches native. Bzip2 reaches
   host `fcopyfile` (92). The bounded metadata-only bridge matches
   native status and actual xattr copy from an open descriptor, with
   no copied data. Bzip2 still enters its error path, and a separate
   `fopen`/`fileno` source control proved the bridge rejected a live
   guest-owned FILE descriptor. The shared fix now matches native
   return flags and on-disk xattr for that path. Unchanged bzip2 moves
   on to `fgetattrlist` (95). The native FinderInfo control verifies
   36 attribute bytes and actual metadata readback, with destination
   data still empty. The new bounded services 95/96 pass this
   differential on the strict C++ build, including the FinderInfo
   written to disk. Unchanged bzip2 decompression matches native
   output, empty stderr and exit 0 on the same build.
   Evidence is under `cpp-fcopyfile-path`,
   `cpp-fcopyfile-stream-path` and `cpp-attrlist-abi-path`.
   Unchanged `uname -m` also matches native. Tar archive listing
   first rejected `sigaction(SIGINFO)`. Adding SIGINFO forwarding
   matches native handler delivery for both guest raise and external
   delivery during nanosleep. Tar now reaches raw Darwin 33
   (`access`) before listing. The shared access bridge matches all
   four native control bytes, including an absent path and null-page
   EFAULT. Tar advances to a null-derived memory fault at address
   `0x28`, guest PC `0x10084a5e8`. A traced replay proves access
   returns ENOENT, then cached `_cerror` reads a null
   `__libkernel_functions` table before accessing its cancellation
   callback. Keep raw carry/errno semantics, route the POSIX access
   wrapper through the same checked host I/O service, and track
   general libkernel bootstrap separately. The unified raw and POSIX
   bridge passes the four native control bytes. Unchanged tar then
   reaches `_strchr`. The shared C++ runner now lowers that import to
   Metal. The isolated direct and indirect fixture matches native bytes,
   empty stderr and exit 0, including unsigned-char conversion, NUL
   and missing targets. The same fixture matches in demand-cache
   mode. Unchanged tar advances to an unresolved eight-byte cache
   read at guest address `0x16021d800`, PC `0x1009e9430`. Cache bytes
   identify `ldr x10,[x10,#0x800]` reading the scalar ISA-class mask
   `0x007ffffffffffff8` in DATA_CONST. The fallback handles rebased
   pointer slots and rejects this non-rebased scalar. A full cache
   image scan places the scalar outside every image segment, in the
   cache-owned DATA_CONST mapping. Its reader is CoreFoundation
   `_CFAllocatorCreateWithZone+0x200`. The residency planner now
   materializes a bounded read-only span in validated cache-owned
   DATA_CONST, then runs normal rebasing and authentication. The
   GPU probe loads the exact mask and rejects a store. The mixed
   scalar/pointer control preserves its rebase, and executable,
   writable, unknown, overlapping and over-capacity spans are
   rejected. The original const-rebase and deferred text controls
   pass. The strict tar snapshot passes 42 launch comparisons,
   external SIGUSR1, and 2048 native function comparisons. Its libz
   control also matches native in resident and demand cache modes. Full tar
   replay uses `cpp-tar-const-path`. Unchanged tar passes the previous
   cache fault and exits at guest status 9. Swift handles status 9
   as data-pointer authentication failure; C++ still reports it as
   unknown. The new C++ diagnostic handles statuses 9, 10 and 12
   with the guest PC and available operands, plus bounds-checked token
   metadata. A positive authentication fixture succeeds and GPU
   negative controls distinguish data, branch and code authentication.
   Six sanitizer-backed bounds/forgery/ambiguity controls pass. The
   unchanged diagnostic tar replay is under `cpp-tar-pac-path`.
   That replay exits 1 with data-pointer authentication failure at
   `0x1009e9474`, operand zero and modifier `0xcb870001663d76f0`.
   Identify the nullable slot producer and native instruction behavior
   before changing authentication. Installed cache inspection
   identifies unconditional AUTDB on a CoreFoundation bridge-class
   slot, zero in the file with no rebase. Native publishes a signed
   class pointer there, and raw null authentication raises SIGBUS.
   The slot requires guest bootstrap or retention of the producer
   write, rather than an authentication exception. Native dyld_info
   identifies a CoreFoundation initializer at image offset 0x1750.
   Cache disassembly ties it to ___CFInitialize and calls to
   _CFRuntimeBridgeClasses. Cached initializer discovery is in
   progress, separate from guest startup execution. The cache metadata
   probe matches dyld_info for CoreFoundation (0x1750) and libc++
   (0x1d3e8, 0x3adb4). The loader is adding dependency-first ordering
   while preserving the separate modeled libSystem startup phase.
   A scan of 4088 installed cache images finds 124 with offset-based
   initializer sections and none using initializer pointers or
   LC_ROUTINES_64. Those forms are represented, with pointer rebase
   authentication retained, but still lack a positive installed
   control. Cached execution is being staged behind
   `ARM64_METAL_CACHE_INITIALIZERS`. The native scratch control
   `/tmp/guest_cfallocator_probe` verifies allocator type and context
   and confirms custom allocate/deallocate callbacks each fire once.
   It does not instrument the internal allocator entry taken by tar.
   The opt-in loader plan verifies Leaf, A, B, main against native
   and retains exact local initializer order. A prebound-only cache
   image remains mapped without being scheduled. The plan excludes
   the modeled libSystem phase; upward-link ordering remains open.
   GPU startup dispatch is wired behind the opt-in. The local
   constructor order fixture matches native both with and without
   the flag. A frozen experimental CoreFoundation allocator replay
   under `cpp-cf-init-path` compares the native type/context and
   callback fixture with cached constructors enabled. It exits 139
   before dispatch. The host crash report points to ExecutableImage
   reading the moved-from SharedCache while classifying code ABI.
   The cache owner is now cache_reader, so use that instance for
   header lookup. The shared loader now uses cache_reader. A strict
   frozen build with that exact ownership fix replays the allocator
   probe under `cpp-cf-init-owner-path`. It passes the host crash and
   exits 1 after a guest atexit_register assertion at line 115.
   Darwin 478 follows in the abort path. Investigate the assertion
   before modeling its cleanup syscall. Apple libc identifies the
   asserted pointer as __atexit, initialized by __atexit_init via
   os_alloc_once. This points to the omitted libc bootstrap phase.
   Cached constructors must preserve guest exit-handler semantics;
   the shipped _libc_initializer calls __atexit_init and stores to
   the exact asserted slot. Native controls confirm reverse order,
   registration during drain, immediate _Exit bypass and once-only
   DSO-scoped finalization. The guest registry component passes
   sanitizer checks for these state transitions. Runner dispatch
   executes callbacks on Metal using guest callback frames. The
   frozen direct-write control under `cpp-guest-exit-path` matches
   all four native modes: return, exit, _Exit and DSO finalization.
   A nonzero-status ARM64 control independently matches return 7,
   exit 13 and immediate exit 23, including whether the handler runs.
   The first arm64e run rejects malformed authentication controls
   but reaches a tail-call resume fault: the callback tail-calls
   write with the end-of-frame PC sentinel. Route that completion
   through the callback state machine before the next dispatch.
   With that fix, all eight ARM64/arm64e native comparisons pass,
   both relocated-TLS regressions pass, and recursive finalization
   plus late registration matches native exit 37. Three invalid
   arm64e callback controls are rejected. The final exact binary
   and receipts are under /tmp/guest-exit-final-build and
   /tmp/guest-exit-final-results.
   The previous baseline stopped at unsupported _atexit. Unchanged
   tar and the CoreFoundation allocator control are replaying with
   cached initializers enabled under `cpp-init-exit-path`. The
   first allocator replay passes the atexit assertion and stops at
   the guest return sentinel on the pre-tail-fix build. The exact
   tail-fixed binary advances the same allocator fixture to raw
   Darwin 483, csrctl, at 0x1008496dc with x0=0. Exact operation
   and startup caller are under investigation. Installed libsystem_kernel
   confirms csr_check routes op 0 with a four-byte mask. Apple XNU
   exposes only check and active-config query operations. The native
   raw fixture under `cpp-csrctl-raw-path` records carry/errno for
   success, denied mask, config copyout, null, invalid and unknown op.
   The read-only host service and raw routing now match native.
   The tail-fixed tar replay independently reaches the same CSR
   query as the allocator control. Both are included in the newer
   raw-CSR app batch.
   the earlier tar process also ended at the return sentinel.
   Unchanged tar was restarted only on the distinct tail-fixed
   binary after the earlier run terminated.
   The exact tail-fixed build passes 42 native launch comparisons,
   external SIGUSR1, and 2048 native function comparisons. Its libz
   control also matches native in resident and demand cache modes.
   CoreFoundation parity is open.
   On the earlier loadables snapshot, unchanged `sw_vers
   -productVersion` also stops at status 9. Native reports
   `27.0.1` and exits 0. Evidence is under `cpp-swvers-path`;
   the older diagnostic does not identify the exact PAC site.
   The earlier
   runs are under `cpp-lazy-strchr-path`. The intermediate replay is under
   `cpp-access-service-path`.
   The access fixture is under
   `cpp-access-abi-path`; signal and prior tar evidence is under
   `cpp-siginfo-path`.
   The installed unchanged OpenJDK 26 `java -version` exits 0 natively
   and stops on Metal at scalar literal load `ldr w8, literal`
   (word `0x18093808`, guest PC `0x11bf0`). Replays and tar register
   traces are under `cpp-tar-trace-path`. The C++ decoder now lowers
   LDR W, LDR X and LDRSW literal forms. The focused M4 Pro differential
   passes 578 checks, including reserved PRFM, XZR and fault retry.
   The unchanged Java replay passes the old instruction and faults at
   address zero, guest PC `0x100461a18`, in `cpp-java-literal-path`.
   Cache inspection identifies `_dlsym+0x5c`, where `ldr x16,[x0]`
   dereferences dyld4::gAPIs. The zero slot has no cache rebase and
   requires runtime initialization. Add guest-aware symbol lookup using
   guest image and export state. The native control for explicit libz
   and libSystem handles is `/tmp/guest_dlsym_handles`. The strict C++
   build now resolves explicit guest handles using loaded images,
   exports and re-exports, with null for a missing symbol. Special
   handle scopes and `dlerror` remain open. Native and guest replay
   outputs, including the lookup-only control, are under
   `cpp-dlsym-path`. The frozen executable SHA-256 is
   `9f136ae3ef275a8e76f7c6891da02b420ffa38a0f9f241986e9a0e64fc280bd1`.
   Both libSystem probes stop at RTLD_NOLOAD before symbol lookup.
   The first confirms a guest libz handle is present. A separate
   libz-only control under the same path isolates symbol resolution
   and invocation from the missing libSystem handle. That probe
   reaches the import and exposes a missing service 162 entry in
   the decoder's host-call dispatch table. After wiring the dispatch,
   the strict build matches native stdout, stderr and status for a
   cached libz symbol call and a local dylib symbol call, including a
   missing symbol returning null. Evidence is under
   `cpp-dlsym-decode-path`. The same build exposes two further gaps
   in scratch controls: RTLD_NOLOAD misses a local
   `@executable_path` handle name, and RTLD_FIRST misses an implicit
   LC_REEXPORT_DYLIB symbol on an otherwise valid handle. The older
   Java replay confirms it reached the unmapped `_dlsym` import.
   The newer frozen binary includes the decoder mapping and its
   unchanged Java replay is under `cpp-dlsym-decode-path`. The
   RTLD_FIRST probe now matches native stdout, stderr and exit status
   with the loaded umbrella's implicit re-export, tested on the
   frozen executable under `cpp-plutil-loadables-path`. The final
   resolver also preserves names whose C spelling starts with an
   underscore; a focused `_leading` call matches native.
   Guest RTLD_NOLOAD now expands `@executable_path` against the
   launched executable directory before resolving loaded paths.
   The original alias control matches native after the change.
   Caller-dependent `@loader_path` and `@rpath` remain open.
   Unchanged Java on the mapped decoder build enters the guest
   resolver and stops at its unsupported handle-scope check.
   The local libjli contains RTLD_DEFAULT and dlopen(NULL) paths.
   A scratch replay under `cpp-java-dlsym-trace-path` captures the
   actual handle and requested symbol before implementing its scope.
   That trace confirms `dlsym(0xfffffffffffffffe, "main")`,
   Darwin RTLD_DEFAULT.
   A separate RTLD_DEFAULT control under `cpp-rtld-default-path`
   proves native executable precedence, a callable dependency-only
   symbol and a null miss. The current runtime fails at the handle
   check. Retain ordered globally visible startup headers and use
   that order for the scope rather than enumerating the image map.
   A second native collision control places sibling B before
   A's transitive dependency, establishing breadth-first declared
   order for the tested startup graph. The loader and resolver now
   implement that order. A strict launch matches native for main
   precedence, a callable dependency export and a null miss, and
   preserves named-handle and RTLD_FIRST re-export controls. Main
   metadata is parsed as MH_EXECUTE. A second native/Metal execution
   of the sibling/transitive collision fixture matches value 42 in
   `cpp-rtld-default-path/order`, proving sibling precedence.
   A native dlerror control verifies a missing
   lookup sets an error, reading clears it, and a successful lookup
   clears a pending error. Guest dlopen(NULL) scope and per-thread
   dlerror now match native in focused linked-dylib and pthread
   controls, alongside the existing named and RTLD_FIRST handles.
   Develop this and guest TLV storage as independent subsystems,
   verify focused native controls, then batch unchanged app replays.
   Retain individual app traces when a fault needs operand evidence.
   The `wc` trace identifies Darwin 478 as cleanup inside cached
   `_abort`, from LR `0x10079043c`. A new diagnostic breaks at
   abort entry under `cpp-wc-abort-path` and captures inbound LR
   `0x100790574`, the return address of the call to abort inside
   `_abort_report_np`. The nearby formatter and string-scan PCs belong
   to abort reporting. The next scratch probe stops at report entry
   `0x100790520` to capture the original caller and arguments. It is
   under `cpp-wc-report-path`. The replay captures caller LR
   `0x100463ea0` and format pointer `0x10048017c`. Reading the cache
   bytes at that pointer yields `thread locals not initialized`.
   Investigate guest TLV descriptors and initialization before the
   cached dyld callback runs. Cache disassembly confirms the fast path
   loads descriptor key and offset, reads the per-thread slot through
   TPIDRRO_EL0 and enters slow allocation on a null slot. The abort
   guard reads an uninitialized dyld global containing UINT64_MAX.
   Swift and C++ both initialize the common TSD slots, but neither
   loader records TLV templates or descriptor keys. The new isolated
   C++ TLV storage component validates keys and offsets, initializes
   per-thread guest memory, publishes through guest TSD, and releases
   storage on thread teardown. Strict component tests cover distinct
   threads, stable repeat access, teardown/reuse, and rollback after
   a TSD-write failure. The native arm64e pthread fixture confirms
   initialized and zero-fill values and thread isolation. Loader
   template metadata, authenticated thunk routing and runner
   integration remain open. Dynamic TLS callbacks remain open.
   Apple's ThreadLocalVariables.cpp confirms disk TLV descriptors
   are rewritten to v2. The 64-bit runtime record has the thunk at
   +0, key at +8, offset at +12, template delta at +16 and size at
   +20. The native probe's +16 qword encodes delta=8 and size=8.
   The installed libdyld disassembly matches Apple's helper: the
   exported bootstrap branches to the error path and the v2 fast
   entry is eight bytes later. The local image loader must derive
   template and descriptor metadata before launch; cached optimized
   versus unoptimized thunks are being classified separately.
   A frozen native/Metal baseline with the arm64e pthread fixture
   is under `cpp-tlv-controls-path`. Native shows initialized and
   zero-fill values and thread isolation. The no-cache Metal launch
   reaches the original authenticated thunk binding and stops at
   unsupported `__tlv_bootstrap`, before any TLV state is created.
   Local metadata now passes the exact pthread fixture: two thunks,
   initialized value 41 and zero-fill, size 8 and alignment 4.
   Cached metadata independently locates libxo's six thunks in
   __AUTH and the 0x7ec-byte all-zero template. The actual wc
   image graph now exposes that metadata under libxo's guest image
   header, with ownership checks and unverified thunk encoding.
   The build includes
   the TLV component and rebuilds on local header changes.
   A second native arm64e fixture, `relocated-tls` in
   `cpp-tlv-controls-path`, links a separate TLV dylib and verifies
   two image templates, per-thread isolation and a stable address.
   dyld_info confirms both authenticated thunk binds and a pointer
   rebase inside its initialized TLS data. A strict integrated build
   matches native on the ARM64 pthread variant and the arm64e
   single-thread two-image variant, including the rebased pointer.
   The full arm64e pthread variant stops at the guest executable
   entry check for its signed callback. Trace and support that ABI
   without weakening PAC. Disassembly identifies PACIZA on the
   worker pointer before pthread_create, key IA and zero modifier.
   Service 70 currently treats the token as a raw executable PC.
   The IA/0 validator accepts a real GPU PACIZA token and rejects
   20 malformed, unsigned, wrong-key/modifier and table-bound cases
   under sanitizers. Runner wiring and caller-image ABI tracking are
   separate work
   so valid arm64e callbacks can enter guest scheduling safely.
   Loader ABI classification reports subtype arm64e for the exact
   signed-callback fixture and ARM64 for its unsigned counterpart;
   unaligned and invalid guest PCs return unknown. The cached libxo
   code also reports arm64e in resident and demand modes. Pthread
   creation now validates signed IA/0 callback tokens and accepts raw
   callbacks only from a known ARM64 caller, using the checked return
   PC to classify the caller. Strict compile passed. The two-image
   relocated TLS fixture now matches native on both ARM64 and arm64e
   with pthreads, including worker isolation and the template rebase.
   The frozen build digest and outputs are under
   `cpp-tlv-controls-path`. Four modified arm64e callback fixtures
   reject unsigned, wrong-key, wrong-modifier and forged-index
   tokens before worker execution; `/tmp/pthread-pac-results.json`
   records both positive and negative runs.
   The same executable matches the native libz control in both
   resident and demand cache modes. Output and executable digest are
   under `cpp-tlv-controls-path`.
   The installed arm64e cache has newFormatTLVs set. A MediaAnalysis
   descriptor proves optimized v2 with key 209, offset 72, delta 72
   and size 80; its delta resolves to the validated template. The
   unoptimized libxo descriptor remains a separate v1 control.
   SharedCache now validates the header flag and identifies the
   unique fast-entry instruction signature. The installed v2
   descriptor's authenticated rebase resolves to that entry.
   The parser validates installed libxo v1, initialized MediaAnalysis
   v2 and zero-fill CorePhotogrammetry v2, plus malformed records
   and signed negative deltas under sanitizers. Guest runtime
   dispatch now has a strict frozen build using the authenticated
   v1 bootstrap and verified v2 fast entry. ARM64 and arm64e local
   TLS regressions match native. Unchanged wc reaches a new demand
   fault at guest PC 0x22d0c0f24 and a resident nontranslated PC
   0x100894b40. Verify whether the new route executed before the
   faults, rather than inferring success from the changed PCs.
   Evidence is under `cpp-fgets-abi-path`, `cpp-getline-abi-path` and
   `cpp-getc-path`.
   The invalid-regex fprintf argument is `0x1663bf7e8`. Cache metadata
   identifies it as the third `__sF` record reached by `__stderrp`.
   The shared runner decodes all three standard stream rebases and
   canonicalizes their identities at host stdio dispatch. Its strict
   build reproduces the scratch result: unchanged invalid-regex grep
   emits `grep: ` then stops at `pthread_mutex_lock` (67).
   The shared runner now dispatches mutex and attribute services through
   GuestScheduler. The 16-result fixture matches native for static normal
   and error-checking locks and dynamically configured recursive locks.
   Blocking contention still needs thread frame switching. Evidence is
   under `cpp-feature-stream-path`, `cpp-mutex-abi-path` and
   `cpp-shared-mutex-path`.
   With mutex dispatch enabled, the thread fixture advances to
   `pthread_create` (70). Unchanged head advances to a null-derived
   address `0x48` at guest PC `0x100747720`; invalid-regex grep gets
   past the mutex but faults at guest PC `0x100889c84`, address
   `0x501e0d8`. Neither text-tool failure case has native parity yet.
   A scratch register trace shows head enters cached `flockfile` with
   the synthetic stdin slot, reads a null FILE lock field and reaches
   the `0x48` fault. Head imports `fgetln`. With its host bridge and
   scratch import routing, the focused fixture matches all nine
   native output bytes and exit status, including embedded NUL and EOF.
   Unchanged `head -n 1` also matches native stdout, stderr and exit 0.
   The shared runner mapping landed and independently passes both
   fixtures. Evidence is under `cpp-fgetln-abi-path`.
   The invalid grep trace reaches `__pthread_once_handler`. At
   `0x100889c84`, `stur x8,[x20,#-0xd8]` writes below the guest TSD
   into a region the GPU treats as read-only. A scratch dedicated
   TSD prefix clears that fault. Grep then reaches Darwin 397,
   `write_nocancel`, from cached stdio. Probe the existing checked
   host write bridge with Darwin carry/errno conventions. The strict
   scratch build with both changes matches the invalid-regex native
   diagnostic and exit 2. The main and worker TSD prefix and syscall
   routing are now in the shared runtime. Unchanged invalid-regex
   grep independently matches native stderr, empty stdout and exit 2
   in its strict build under `cpp-shared-bridges-path`. Earlier evidence is
   under `cpp-text-faults-path`, `cpp-tsd-prefix-path` and
   `cpp-write-nocancel-path`.
   A direct native syscall 397 probe caught low pointers reading the
   backing and high pointers throwing instead of returning EFAULT.
   The shared write service now preserves native descriptor precedence,
   rejects low/high pointers with carry and EFAULT, and avoids reading
   the buffer for a valid zero-count write. All six output bytes and
   exit status match native. Evidence is under `cpp-write-abi-path`
   and `cpp-write-condition-path`.
   Initial shared thread-frame integration builds, but the full
   `guest-threads` fixture returns 13 at the interrupted-sleep check.
   The focused create/yield/contended-lock/join fixture independently
   faults on a worker stack store at guest address `0x92020`, below
   the GPU writable boundary. Shared worker storage now uses the guest
   heap. The focused fixture matches all five native bytes and exit
   status for create/yield, lock handoff, join and separate errno
   values. The next shared build preserves the PAC table across
   switches and schedules nanosleep cooperatively. Saving/restoring
   pending thread-directed signals now keeps main's blocked SIGUSR1
   away from the worker. A focused fixture independently matches all
   three native bytes, including delivery when main unblocks. The
   full fixture advances past exit 13 to service 82
   (`pthread_cond_signal`). A separate condition control matches the
   expected six native results and the current runtime stops at
   service 79 (`pthread_cond_init`). Evidence is under
   `cpp-cooperative-sleep-path`, `cpp-thread-pending-path` and
   `cpp-cond-abi-path`.
   The condition services now run through GuestScheduler. Wakeup,
   mutex reacquisition, join and destroy match native. Both timeout
   forms return zero instead of native ETIMEDOUT (60). The full
   thread fixture prints `worker` and stops at exit 11. Scheduler
   save/restore overwrote a wake result while retrying selection of the
   still-current thread. The fixed scheduler preserves it, and the
   focused condition fixture and full guest-threads fixture now match
   native. The launch gate then reaches guest-memory, which returns
   43 because sched_yield leaves stale x0. Service 75 now explicitly
   returns zero. The strict build then passes the whole launch gate:
   42 comparisons and external SIGUSR1 delivery. Evidence is under `cpp-conditions-path`,
   `cpp-timeout-fix-path` and `cpp-yield-gate-path`.
   Evidence is under `cpp-threads-integrated-path`.
   Replays and cache disassembly are under `cpp-cmeq-path`. Its strict
   launch gate logged 39 native matches before `pthread_mutex_lock`.
   Replays are under `cpp-writeback-path` in scratch.
   The earlier ls probe
   stopped at `_compat_mode`. Native probes identify it as a function
   that uses `COMMAND_MODE` and recognizes `Unix2003` and `Legacy`
   case-insensitively. The integrated host query now matches native
   output, diagnostic and exit status with Legacy and posix environment
   values, including the invalid BSD request. The default is Unix2003.
   GPU lowerings for `_strsep` and `_memchr` now match eight native fixture
   results covering empty tokens, delimiter sets, exhaustion, unsigned-byte
   matching and misses. Both cache configurations passed in a frozen
   snapshot whose host dispatcher aborts on either string service.
   Failed address resolution now preserves the input registers and leaves
   the string and cursor unchanged for retry. A separate eight-`BLR`
   fixture matched the same native results with CPU dispatch disabled.
   The shared tree now has no CPU handlers or dispatch IDs for either
   import. A strict snapshot verified both direct and indirect fixtures
   in both cache configurations after removing the fallbacks. Evidence
   is under `cpp-no-string-fallback-path` in the scratch root. This snapshot
   uses the previous loader files to isolate it from concurrent section
   lookup changes. Its launch gate again logged 39 matches before
   `pthread_mutex_lock`.

Known discrepancies include full environment enumeration, partial libc
semantics, and launch state outside the exercised process ABI. No speedup
claim is made.

## Repository checkpoint

- The current C++ source passes a strict app build and full `make verify` on macOS 27.0.1. It also passes eight focused native comparisons, including raw Darwin 177 and the no-injection libdyld query. The verified binary is copied to `.build/cpp/arm64-metal`.
- Darwin 177 now rejects low invalid output addresses with EFAULT. The cached interposition query uses the installed Mach-O export spelling `_dyld_has_inserted_or_interposing_libraries`.
- See [HANDOFF.md](HANDOFF.md) for the exact checkpoint, reproducible loader probes, and the next unchanged app runs. General compatibility remains open.
