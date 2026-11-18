# ARM64 to Metal runtime

The launcher reads a compiled ARM64 Mach-O, decodes reachable machine instructions,
builds basic blocks, and emits Metal in memory. It has a process launcher for
supported executables and a separate function runner for independent batches.

The C++ build, verification commands, and open compatibility work are
documented in [HANDOFF.md](HANDOFF.md). `make` builds the C++ launcher and its
native fixtures. `make verify` runs the launch and function gates. `make bundle`
verifies and packages the C++ launcher at the original archive path. `make
bundle-cpp` creates the C++ named archive.

## Launch an executable

Extract the archive on Apple Silicon and run this from the extracted directory:

```sh
./arm64-metal launch ./console hello
echo "$?"
./arm64-metal launch /bin/cat example.json
./arm64-metal launch ./linked argument
```

The first command prints `hello` and returns status 20, as the native executable
does. The runtime reads `LC_MAIN`, maps the image, and supplies argc, argv, and a
bounded guest stack. Direct calls and returns execute on the GPU. An indirect call into guest text
extends the translation on demand and resumes with the saved register state. The CPU host
handles supported imports, preserving guest state across dispatches. Set
`ARM64_METAL_TRACE=1` to log kernel compile
start/completion, sampled Metal dispatch start/completion, and guest host-service
entries on stderr. The trace is capped at 128 events and prints one limit notice.
After the cap, it emits at most one sampled progress event every 10 seconds, up
to 16 more events by default. Set `ARM64_METAL_TRACE_PROGRESS_LIMIT` to a decimal
value from 0 to 4096 to change that bounded report count. Progress lines include
tagged compile starts/completions, summed compile milliseconds, and dispatch
count. It also marks image-load boundaries, initializer transitions, and raw
Darwin syscall entries. Without this flag the process runner emits no progress
messages.

The verified system executable corpus covers `true`, `false`, `echo`, `pwd`,
`printenv` named lookups, `dirname`, `basename`, `cat` reading files and stdin,
and `cksum` in all four checksum modes. The verified Homebrew XZ and LZ4 paths
include version output and decompression from a file or stdin. See the ledger
for the tested payloads. The host provides a bounded stack, a heap that grows
within Metal's buffer limit, a snapshot of the launcher environment, selected
libc imports and globals, and file I/O.
Environment enumeration can expose variables initialized by the host process.

The loader resolves supported dyld chained fixups across an executable and local
dylibs, including transitive dependencies, export tries, and dependency
initializers. It handles absolute install names, `@loader_path`,
`@executable_path`, and `@rpath` for the verified cases. The bundled `linked`
example runs two dylib constructors and one executable constructor, then calls
through an imported function pointer. Its third path calls a symbol re-exported
by the wrapper. Each path exits with status 42. The loader binds
supported libSystem imports to guest-callable addresses. ARM64e returns
use a shadow stack. Authenticated indirect calls validate guest tokens against
the expected modifier and key. These mechanisms do not emulate Apple's
hardware PAC algorithm. Metal kernels are compiled in bounded regions and
cached during a run. Legacy relocations, dynamic loading at runtime,
initializer/finalizer forms outside the tested subset,
GUI frameworks and broader instruction coverage remain open. Guest pthreads
are scheduled cooperatively at dispatch boundaries and blocking synchronization
calls. Thread routines, supported atomics, and buffer operations execute on Metal.
The verified subset includes mutexes, condition variables, join, thread-local
errno, signal masks, and absolute and relative condition timeouts. Parallel guest
dispatch, cancellation, and general asynchronous host I/O remain open.
The verified signal subset includes registration, masking, self-delivery, nested
handlers, and externally delivered signals for the host bridge's supported
signal set. Guest handlers run on Metal at dispatcher boundaries. Synchronous
fault signals, alternate stacks, and full interrupted-syscall semantics remain open.
The launcher executes guest instructions on Metal and reports unsupported
instructions and imports when reached. `make verify-cfinv` covers CFINV, RMIF, SETF8,
SETF16, AXFLAG and XAFLAG against native ARM64 in translated functions, translated
processes, and typed AOT execution. Both FlagM2 conversions exercise all 16
input flag combinations. Adjacent unsupported encodings must report their opcode.

`./arm64-metal inspect-cache /usr/lib/libbz2.1.0.dylib _BZ2_bzBuffToBuffDecompress`
locates a direct export in the installed ARM64e dyld cache and reads its first
word. `inspect-cache-rebases INSTALL_NAME` decodes the installed cache's
version-5 pointer chains. Set `ARM64_METAL_SHARED_CACHE=1` to opt in to mapping
direct system dependencies during launch. The runtime preserves cache-relative
addresses above 4 GiB in split segments and applies their version-5 fixups.
The process dispatcher carries the full guest PC, including indirect calls
to synthetic host-service entries above the cache window. The loader
resolves the verified address-diversified IA stub form to guest code or
modeled libSystem services. It also maps merged GOT references to modeled
libSystem globals. A focused re-export fixture also confirms execution of an
original cached libcharset routine. Set `ARM64_METAL_CACHE_DEMAND=1` alongside
`ARM64_METAL_SHARED_CACHE=1` to opt in to segment residency on first data access.
This mode uses a 64 MiB resident backing pool and has no eviction. The positive
`plutil` framework probe reads its guest thread pointer and authenticates a
packed Objective-C ISA on Metal. It passes the masked class-data load and
executes `XPACD` and `XPACI`. A guest aligned allocation passes the former
libobjc store. Set `ARM64_METAL_CACHE_BOOTSTRAP=1` with the cache flags to run
cached `_objc_init` before the executable entry. This experimental path
demand-loads authenticated GOT slots, compares guest SDK and deployment
versions from the executable metadata, and passes a read-only shared mapping
of the feature-flags object and a guest quiescent-generation read. It enumerates
the guest environment, passes a SIMD literal load and resolves canonical shared-cache selector
addresses. The guest page-size binding advances libmalloc zone creation to
a guest Mach VM allocation and image-slide query. Guest VM subrange
protections install the allocator guard pages, and the launcher answers its
link-time library version query. Register-modifier code-pointer signing
passes. The guest reports a conservative CPU capability word and supplies boot
time through the commpage. The probe passes a host wall-time query and a BTI
hint, executes AES instructions in libcorecrypto, and gets entropy from Darwin.
Guest Mach thread names let it pass libsystem_platform's unfair-lock ownership
check. The guest APRR capability byte selects libdyld's fallback path, and
the immutability query uses the guest loader's own mapping metadata. The probe
passes the previous null-task path after initializing the cached task-self
global. Startup also reads an Objective-C cache pointer from a demand-mapped
constant rebase and reads the message-send offset table from cache metadata.
It now constructs a Mach reply port and registers restartable ranges over
the Mach message trap. It passes synchronization and reaches Objective-C
callback registration and enters the initial v4 mapped-image callback.
Startup reads both Objective-C optimization tables and the relocated main
executable header. Bounded cache text windows let startup pass the earlier
residency failure. Selectors can cross read windows, and the guest can grow
its residency table during Objective-C selector registration. Cached images
now advertise their preoptimized references. Startup invokes the scoped
Objective-C mutation block, passes signed-byte loads, and reaches dyld's
cache-override query. It passes `PACGA` in libobjc and reaches dyld's
Objective-C class enumeration API. It passes cached class lookup, libmalloc's `TPIDR_EL0` read, and a cached
`LDSETH`. It passes `RBIT`, `PACIA1716`, and tagged Data-B authentication before
reading the cache class count and opening the loaded CoreFoundation image
with `RTLD_NOLOAD`. It passes the main-header read in `getsectiondata` and the cached locale formatting path. It completes the continuous-clock call and Darwin `proc_info` query
and passes the main-queue assertion. The host feature policy enables the
Swift path. Both lint fixtures pass local Objective-C stubs and AUTIZA
and the shared-cache selector stub. Swift argument startup completes and
both runs pass SIMD widening and signed lane extraction and libdyld’s restricted-process query and the Darwin `csops` status query and executable-path lookup. Both runs pass signed indexed loads and
pass `CASP` and the Swift prespecialized-data query and pointer hash lookup. Both runs pass the signed callback before a
prebound lazy load and continue through Metal region eviction. The launcher
forwards the supported sandbox path query to the host. Cached libmalloc
answers allocation-size queries for guest VM pointers on Metal.
The public add-image notification API runs callbacks
for the guest's startup image snapshot on Metal. Section lookups resolve
against guest Mach-O headers in both local and cached images. The
guest supports file-backed `PROT_READ` and `MAP_SHARED` mappings with
dispatch-boundary visibility and whole mapping unmap.
The default startup path still reaches a null allocator zone.
The thin arm64e differential exercises data-pointer signing and authentication
with valid and invalid keys and modifiers. The pthread fixture
verifies isolated guest thread-specific storage for pthread identity and errno.
Cache initializers and broader thread-local storage support remain open.
`make verify-cache` compares decoded pointers with native dyld and exercises
system libz in both cache modes, including
stored-block decompression. A focused compressed-block fixture now matches
native stdout, stderr, and exit status, including all 2,880 output bytes and
checksum `3476419374`.

Unmodified `/usr/bin/gzip -dc` matches native stdout, stderr and exit status
for named compressed inputs of 64 and 37,000 decoded bytes and for the small
fixture on stdin. These paths use `ARM64_METAL_SHARED_CACHE=1`.
Deterministic `gzip -nc` compression matches native byte-for-byte on the
64-byte and 37,000-byte named inputs. The malformed-input case also matches
native diagnostics and exit status.
Unmodified `/usr/bin/bzip2 -dc` matches native on 64-byte and 37,000-byte
named inputs and on the small stdin case. The metadata probe verifies the
copied extended attribute. `make verify-cache` repeats both system decoders.
Run `make verify-apps APP_VERIFY_CASES=cli-tools APP_VERIFY_RUNTIME=/path/to/arm64-metal`
for native/Metal comparisons of 16 unchanged tools, including matched and missing
paths, same-length binary differences, sorting, filtering, and field selection.
The gate uses the packaged AOT interpreter and the C locale, plus en_US.UTF-8
for paste column alignment and malformed input. Commands using
shared-cache libc run its ARM64 code with demand paging and startup initializers
disabled. The cache-off `cmp` tests use modeled read-only `fdopen` on guest-owned
descriptors and libc-owned input buffering. The sort test uses the existing
locale conversion service, and reverse tail maps regular stdin read-only.
The gate also checks the missing-input diagnostic from `diff -u` and the renamed
`strlcat` cache export. Unified comparison of two regular files reaches guest
libc's `flockfile`, which expects a guest-addressable FILE structure.
The Azul JDK 25.0.3 `java -version` launch resolves local `libjli` but stops
at `_strdup`.
See `LEDGER.md` for exact probe boundaries and remaining work.

The C++ launcher uses macOS Foundation and Metal frameworks. Running the
packaged launcher does not require Python or Xcode. Xcode and metal-cpp headers
are needed to build it. The AOT interpreter library ships with the launcher.

## Run an individual function

```sh
./arm64-metal run workload.dylib update_words example.json
./arm64-metal run-stream workload.dylib update_words < batches.jsonl
./arm64-metal translate /path/to/binary symbol
./arm64-metal verify /path/to/trusted.dylib symbol
```

The symbol is passed without the leading Mach-O underscore. Input is a JSON
array of objects with unsigned 32-bit `words` and `seed`. Omitting the input
path or passing `-` reads stdin. An optional argument after the input path sets
the maximum number of basic blocks per invocation. The launcher emits a JSON
array on success and exits with an error if any invocation fails.
`run-stream` reads one JSON batch per line and reuses its translated Metal
pipeline until EOF; each line produces one flushed JSON result line.

The function ABI is `uint32_t function(uint32_t *words, uint64_t count, uint32_t
seed)`. Each invocation gets its own word buffer at guest address zero.
The verify command deliberately loads and calls a dylib on the CPU to check
results. Use it only with trusted libraries that implement the function ABI.

## Build and verify

From the source directory on an Apple Silicon Mac with Xcode and Apple's
metal-cpp headers:

```sh
make bundle METAL_CPP=/path/to/metal-cpp
```

This compares complete process runs with native stdout, stderr, and exit status.
It also compares 2,048 native and GPU function invocations, checks the known
JSON result and rejection paths, and creates `.build/arm64-metal-macos.tar.gz`.
The extracted archive contains the C++ launcher and its AOT Metal interpreter
library. `make bundle-cpp` also creates `.build/arm64-metal-cpp-macos.tar.gz`.
