# Loader probes

Build these from the repository root. Use the same compiled probe for native and Metal runs. Set `JDK_HOME` to the installed JDK you are qualifying.

```sh
mkdir -p .build/handoff-probes
out="$PWD/.build/handoff-probes"
src=fixtures/handoff
xcrun clang -Wall -Wextra -Werror "$src/cache-root.c" -o "$out/cache-root"
xcrun clang -Wall -Wextra -Werror -dynamiclib "$src/cache-provider.c" -lz -o "$out/libprovider.dylib"
xcrun clang -Wall -Wextra -Werror "$src/cache-provider-main.c" -o "$out/cache-provider"
xcrun clang -Wall -Wextra -Werror "$src/kdebug-typefilter.c" -o "$out/kdebug-typefilter"
xcrun clang -Wall -Wextra -Werror "$src/dyld-interposition.c" -o "$out/dyld-interposition"
xcrun clang -Wall -Wextra -Werror "$src/mach-reply-port.c" -o "$out/mach-reply-port"
xcrun clang -Wall -Wextra -Werror "$src/commpage-page-shift.c" -o "$out/commpage-page-shift"
xcrun clang -Wall -Wextra -Werror "$src/mach-vm-partial-deallocate.c" \
  -o "$out/mach-vm-partial-deallocate"
xcrun clang -Wall -Wextra -Werror -I "$JDK_HOME/include" -I "$JDK_HOME/include/darwin" \
  "$src/jni-entry.c" -o "$out/jni-entry"

"$out/cache-root"
"$out/cache-provider" "$out/libprovider.dylib"
"$out/kdebug-typefilter"
"$out/dyld-interposition"
"$out/mach-reply-port"
"$out/commpage-page-shift"
"$out/mach-vm-partial-deallocate"
"$out/jni-entry" "$JDK_HOME/lib/server/libjvm.dylib"
```

For a Metal comparison, prefix each invocation with `env ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 .build/cpp/arm64-metal launch`. Keep these runs separate from the unchanged application replays.

## Unchanged application parity gate

Build the C++ runtime with `make -C cpp app METAL_CPP=/path/to/metal-cpp`, then run the bounded app probes from the repository root:

```sh
make verify-apps
```

The default cases are valid and malformed `plutil`, deterministic `tar -tf`, and cache-only/dynamic-provider libz loading. Select a subset with `APP_VERIFY_CASES`, for example `make verify-apps APP_VERIFY_CASES='plutil tar'`. Add `jni` when qualifying an installed JDK by setting `JDK_HOME` to its home directory and including `jni` in the case list. `APP_VERIFY_RUNTIME` selects the frozen C++ launcher, and `APP_VERIFY_RESULTS` selects the parent directory for unique per-run receipts. Set `APP_VERIFY_TIMEOUT_SECONDS` to change the per-process timeout. The gate compares stdout, stderr, and exit status, stores the runtime SHA-256, and fails on timeout or runtime mutation.

## Generated-code permission and rewrite probe

The JIT probe writes a two-instruction ARM64 function into an anonymous mapping,
switches it from RW to RX, executes it, switches back to RW, rewrites its return
value, and executes it again. It also checks that RWX `mmap` and `mprotect`
requests fail with `EACCES`.

The positive case protects individual pages in a three-page mapping, rewrites
one executable page, then unmaps it while calling code from the surviving
executable page. Optional negative cases attempt to execute after returning
code memory to RW or after unmapping it. Native execution faults with SIGBUS
for RW and SIGSEGV after unmapping. The translated runtime must reject both
without executing stale code.

```sh
src=fixtures/handoff/jit-rewrite.c
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 "$src" -o .build/handoff-probes/jit-rewrite
.build/handoff-probes/jit-rewrite
.build/cpp/arm64-metal launch .build/handoff-probes/jit-rewrite
.build/handoff-probes/jit-rewrite rw-fault
.build/handoff-probes/jit-rewrite unmapped-fault
```

Expected output from both runs is
`first=42 second=81 third=73 survivor=81 wx=13/13`, with exit status 0.
Each negative case prints `calling revoked code` before the native fault. The
translated runtime reports a code rejection instead of reproducing Unix signal
delivery.

## Typed MOVZ and RET slice

The ARM64 assembly fixture provides a native oracle for X/W MOVZ shifts,
zero-register discard, zero-extension, and RET:

```sh
xcrun clang -arch arm64 -c fixtures/handoff/typed-movz-ret.s \
  -o .build/handoff-probes/typed-movz-ret.o
xcrun clang -arch arm64 fixtures/handoff/typed-movz-main.c \
  .build/handoff-probes/typed-movz-ret.o \
  -o .build/handoff-probes/typed-movz-ret
.build/handoff-probes/typed-movz-ret
```

The host-side decoder test verifies the fixed six-word operation packet and
the translator's MOVZ/RET lowering:

```sh
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -pedantic -Icpp \
  fixtures/handoff/typed-operation.cpp cpp/translate_core.cpp \
  -o .build/handoff-probes/typed-operation
.build/handoff-probes/typed-operation
```

## Typed MOVN and MOVK slice

The native assembly control covers X/W MOVN complements, X/W MOVK field
replacement, and zero-register discard:

```sh
xcrun clang -arch arm64 -c fixtures/handoff/typed-move-wide.s \
  -o .build/handoff-probes/typed-move-wide.o
xcrun clang -arch arm64 fixtures/handoff/typed-move-wide-main.c \
  .build/handoff-probes/typed-move-wide.o \
  -o .build/handoff-probes/typed-move-wide
.build/handoff-probes/typed-move-wide
```

The host-side typed-operation check verifies MOVN/MOVK packet fields and that
the translator emits calls to the shared move-wide MSL helper. The helper's
value expression lives in `cpp/move_wide_shared.h`; the same header supplies
the runtime-generated translator/function shader source and the AOT
interpreter implementation.

## Multi-allocation Mach VM deallocation

The fixture allocates three adjacent anonymous regions, removes the middle
allocation, then deallocates a range crossing the surviving allocations and the
hole. It verifies the surviving edge data and confirms the five removed pages
are inaccessible before releasing the edges.

```sh
src=fixtures/handoff/mach-vm-multi-deallocate.c
out=.build/handoff-probes/mach-vm-multi-deallocate
mkdir -p .build/handoff-probes
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 "$src" -o "$out"
"$out" > "$out.native.out" 2> "$out.native.err"
printf '%s\n' "$?" > "$out.native.status"
.build/cpp/arm64-metal launch "$out" > "$out.runtime.out" 2> "$out.runtime.err"
printf '%s\n' "$?" > "$out.runtime.status"
cmp "$out.native.out" "$out.runtime.out"
cmp "$out.native.err" "$out.runtime.err"
cmp "$out.native.status" "$out.runtime.status"
```

Expected stdout is `page=16384 deltas=49152,81920 removed=0 span=0 data=49,115 protection=0,12,12,12,12,12,0` followed by `release=0,0`; stderr is empty and status is 0.

## Raw `mach_vm_protect` trap and per-range maximums

`mach-vm-protect-raw.c` issues the installed ARM64 Mach trap directly: x0 task,
x1 address, x2 size, x3 `set_maximum`, x4 protection, x16 = -14, then `svc #0x80`.
It compares the public wrapper with the raw trap, splits current protections
across pages, lowers maximum protection on only the middle page, then checks
that raising that maximum or requesting WRITE above it returns
`KERN_PROTECTION_FAILURE`.

Run the native oracle and inspect the installed kernel stub:

```sh
src=fixtures/handoff/mach-vm-protect-raw.c
out=.build/handoff-probes/mach-vm-protect-raw
mkdir -p .build/handoff-probes
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 "$src" -o "$out"
"$out"
otool -tvV /usr/lib/system/libsystem_kernel.dylib | awk '/^_mach_vm_protect:/{p=1} p{print; if (++n==25)exit}'
otool -tvV /usr/lib/system/libsystem_kernel.dylib | awk '/^__kernelrpc_mach_vm_protect_trap:/{p=1} p{print; if (++n==3)exit}'
```

Native stdout is `page=16384 wrapper=0 split=0 q=0:1/7,0:1/7,0:3/7 lower=0 raise=2 over=2 final=0:1/1` followed by `release=0`. The exported wrapper forwards x0 through x4 unchanged; the trap stub loads x16 with -14 and executes `svc #0x80`.

## Minimal raw protection differential fixture

`mach-vm-protect-raw-probe.c` avoids `mach_vm_region` and other MIG query APIs.
It uses `mprotect` return codes and guarded reads/writes to observe the raw trap:
current READ can be restored to RW before max-lowering; lowering the middle
page's maximum to READ rejects both a later raw WRITE request and POSIX RW
upgrade with `EACCES`, while the middle byte remains readable. It also checks
that both edge pages remain writable with their data intact, that the deallocated
middle page reports `ENOMEM`, and that both edge fragments can be released.

```sh
src=fixtures/handoff/mach-vm-protect-raw-probe.c
out=.build/handoff-probes/mach-vm-protect-raw-probe
mkdir -p .build/handoff-probes
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 "$src" -o "$out"
"$out" > "$out.native.out" 2> "$out.native.err"
printf '%s\n' "$?" > "$out.native.status"
.build/cpp/arm64-metal launch "$out" > "$out.runtime.out" 2> "$out.runtime.err"
printf '%s\n' "$?" > "$out.runtime.status"
cmp "$out.native.out" "$out.runtime.out"
cmp "$out.native.err" "$out.runtime.err"
cmp "$out.native.status" "$out.runtime.status"
```

The native control exits 0 with stdout
`page=16384 current=0 restore=0/read=0 lower=0 raise=2 write=2 mprotect-rw=-1/13 read=0/0 middle=83 edges=0,0:50,116 dealloc=0 hole=-1/12 release=0,0` and empty stderr.
