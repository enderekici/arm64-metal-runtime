# Workqueue registration and worker probes

`workq-worker-request.c` checks the packed 56-byte `_pthread_registration_data`
ABI and directly issues raw 366 registration, 368 dispatch setup/request, and
367 open. Its normal guest build expects libpthread startup to register first;
the explicit duplicate then returns `EINVAL`, and the raw request must enter the
existing authenticated libpthread workqueue trampoline and invoke the fixture
callback. The native positive control prints
`register=EINVAL raw_setup=open=request=ok callback_bits=7 arg=0x8ff`.

The `WORKQ_GUEST_NO_INIT` build omits pthread APIs and uses only raw registration,
setup, open, request, yield, and write. Run it with cached initializers disabled
to test a first registration and require its own authenticated guest worker
callback to publish all 10 ABI checks. This is a reduced no-init control; the
normal build is the initialized-libpthread path.

`thread-self-carry.c` is a raw syscall 372 native positive control. It sets
carry immediately before the trap and requires a nonzero thread ID with carry
cleared.

```sh
src=fixtures/handoff/workq-worker-request.c
xcrun clang -arch arm64e -Wall -Wextra -Werror -fno-stack-protector \
  fixtures/handoff/thread-self-carry.c -o /tmp/thread-self-carry-native
/tmp/thread-self-carry-native
xcrun clang -arch arm64e -Wall -Wextra -Werror -fno-stack-protector \
  "$src" -o /tmp/workq-worker-request-native
/tmp/workq-worker-request-native
xcrun clang -arch arm64e -DWORKQ_GUEST_FIXTURE=1 -Wall -Wextra -Werror \
  -fno-stack-protector "$src" -o /tmp/workq-worker-request-guest
xcrun clang -arch arm64e -DWORKQ_GUEST_FIXTURE=1 -DWORKQ_GUEST_NO_INIT=1 \
  -Wall -Wextra -Werror -fno-stack-protector "$src" \
  -o /tmp/workq-worker-raw-noinit-guest
```

For the no-init guest control, use `ARM64_METAL_SHARED_CACHE=1
ARM64_METAL_CACHE_DEMAND=1 ARM64_METAL_CACHE_INITIALIZERS=0` and require stdout
`workq raw worker ABI ok`. For initialized startup, enable cache initializers
and require `workq libpthread worker callback ok`. With
`ARM64_METAL_TRACE_WORKQ=1`, registration diagnostics report only branch names,
status, sizes, offsets, and nullness; they do not print pointers or payload.

## Loader probes

## Late writable-TPRO rejection

This control imports the SPI directly and calls it from `main`, after
libSystem initialization. The callback writes a unique stderr marker and
mutates its context, so neither effect can be mistaken for an abort before the
callback. Native dyld exits 134 with `Illegal TPRO memory access`, prints the
`BEFORE` line, and never prints `CALLBACK_ENTERED` or `AFTER`.

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror -O0 \
  fixtures/handoff/tpro-late-negative.c -o .build/handoff-probes/tpro-late-negative
.build/handoff-probes/tpro-late-negative
```

For the focused guest late-guard comparison, build the C++ runtime and launch
with shared-cache and demand mapping only. Omitting cache bootstrap and cached
initializers lets this fixture reach `main` before the modeled libSystem
initializer path; this checks the host-service late guard and does not claim
full startup parity.

```sh
env ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 \
  ARM64_METAL_TRACE=1 \
  .build/cpp/arm64-metal launch .build/handoff-probes/tpro-late-negative
```

The guest must preserve the late abort and omit both callback markers.
Expected guest status is 134; with `ARM64_METAL_TRACE=1`, the log should show
the main entry dispatch before service 178. The callback marker and `AFTER`
line must both remain absent.

This late fixture cannot verify normal scope release or cancellation: native
dyld permits a callback only during early libSystem initialization. To observe
scope ownership safely, an opt-in runtime trace should report the guest thread
ID when service 178 opens the scope, then report either normal callback-frame
return or thread-frame discard for that same ID. The full-startup cache-root
probe covers normal return; cancellation requires a synthetic runner-level
fixture that discards a TPRO-scoped callback frame on a guest worker thread.
Do not use a post-main native SPI call as a scope-positive control; dyld must
abort it before invoking the callback.

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
xcrun clang -Wall -Wextra -Werror "$src/mach-host-info.c" -o "$out/mach-host-info"
xcrun clang -Wall -Wextra -Werror "$src/raw-sysctl.c" -o "$out/raw-sysctl"
xcrun clang -Wall -Wextra -Werror "$src/sysctl-usrstack.c" -o "$out/sysctl-usrstack"
xcrun clang -Wall -Wextra -Werror "$src/pthread-stackaddr.c" -o "$out/pthread-stackaddr"
xcrun clang -Wall -Wextra -Werror "$src/mach-port-namespace.c" -o "$out/mach-port-namespace"
xcrun clang -Wall -Wextra -Werror "$src/mach-local-message.c" -o "$out/mach-local-message"
xcrun clang -Wall -Wextra -Werror "$src/mach-self-pid.c" -o "$out/mach-self-pid"
xcrun clang -Wall -Wextra -Werror "$src/mach-port-namespace.c" -o "$out/mach-port-namespace"
xcrun clang -Wall -Wextra -Werror "$src/commpage-page-shift.c" -o "$out/commpage-page-shift"
xcrun clang -Wall -Wextra -Werror "$src/commpage-dev-firm.c" -o "$out/commpage-dev-firm"
xcrun clang -Wall -Wextra -Werror "$src/commpage-cpu-fields.c" -o "$out/commpage-cpu-fields"
xcrun clang -Wall -Wextra -Werror "$src/mach-vm-partial-deallocate.c" \
  -o "$out/mach-vm-partial-deallocate"
xcrun clang -Wall -Wextra -Werror -I "$JDK_HOME/include" -I "$JDK_HOME/include/darwin" \
  "$src/jni-entry.c" -o "$out/jni-entry"

"$out/cache-root"
"$out/cache-provider" "$out/libprovider.dylib"
"$out/kdebug-typefilter"
"$out/dyld-interposition"
"$out/mach-reply-port"
"$out/mach-host-info"
"$out/raw-sysctl"
"$out/sysctl-usrstack"
"$out/mach-port-namespace"
"$out/mach-local-message"
"$out/mach-self-pid"
"$out/mach-port-namespace"
"$out/commpage-page-shift"
"$out/commpage-dev-firm"
"$out/commpage-cpu-fields"
"$out/mach-vm-partial-deallocate"
"$out/jni-entry" "$JDK_HOME/lib/server/libjvm.dylib"
```

`pthread-stackaddr` is a separate native control for comparing
`pthread_get_stackaddr_np` with `KERN_USRSTACK64`; it is not part of the guest
sysctl parity run.

For a Metal comparison, prefix each invocation with `env ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 .build/cpp/arm64-metal launch`. Keep these runs separate from the unchanged application replays.

`mach-port-namespace.c` covers receive allocation (`-16`), reply-port name
generation (`-26`), MAKE_SEND insertion (`-21`), send-right deallocation
(`-18`), and receive destruction (`-25`). Its normalized native output is
`task=1 allocated=1 names_distinct=1 inserted=1 send_deallocated=1 receive_only_invalid_right=1 destroyed=1 destroy_reply=1 destroy_again_invalid_name=1 invalid_right=18 wrong_task_invalid_dest=1`.

`mach-local-message.c` directly exercises the arm64 `mach_msg2_trap` (`-47`)
register and packed-argument ABI for local send, receive, timeout, and invalid
right/name cases. It uses `MACH64_SEND_ANY` on raw sends, matching the generic
send classification selected by libSystem. Native normalized output is
`task=1 allocate=1 insert=1 invalid_destination=1 send=1 receive=1 payload=1 timeout=268451843 receive_only_send=268435459 invalid_receive=268451842`.

`mach-self-pid.c` also exercises raw `thread_self_trap` (`-27`) from the main
thread and a pthread worker. It checks stable names within each thread,
distinct names across threads, and releases both acquired references for each
name with raw `mach_port_deallocate` (`-18`). Native normalized output begins
`thread_reused=1 thread_nonnull=1 worker_valid=1 thread_distinct=1 thread_cleanup=1`.
Run the guest fixture with shared-cache and demand-cache enabled, leaving cache
bootstrap and initializer flags unset so execution enters the fixture directly.

## Bootstrap service lookup boundary

`bootstrap-lookup-native.c` is a native, headless oracle for the allowed
WindowServer bootstrap lookup and a nonexistent service. It validates the
returned send right and releases it. `bootstrap-lookup.c` exercises the guest
imports and synthetic-right deallocation path with the same positive and
negative requests. Neither fixture sends a WindowServer protocol message or
opens UI.

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror \
  fixtures/handoff/bootstrap-lookup-native.c -o /tmp/bootstrap-lookup-native
/tmp/bootstrap-lookup-native
xcrun clang -arch arm64 -Wall -Wextra -Werror \
  fixtures/handoff/bootstrap-lookup.c -o /tmp/bootstrap-lookup
/tmp/bootstrap-lookup
```

The native oracle's normalized output is
`lookup=windowserver-active status=0 port_valid=1 type_status=0 send_right=1 deallocate_status=0`
and `lookup=windowserver-active-duplicate status=0 port_valid=1 type_status=0 send_right=1 deallocate_status=0`,
and `lookup=missing status=1102 port_valid=0 type_status=15 send_right=0 deallocate_status=15`.
The guest fixture prints `lookup=windowserver-active status=0 port_valid=1 deallocate_status=0`
and `lookup=windowserver-active-duplicate status=0 port_valid=1 deallocate_status=0`,
and `lookup=missing status=1102 port_valid=0 deallocate_status=15`.
The translated slice exposes a synthetic guest `_bootstrap_port` and maps only
`_bootstrap_look_up` for `com.apple.windowserver.active`; a guest port name is
never the host Mach name. The host send right belongs to the guest process and
is released when its guest send-right reference reaches zero or the process
ends. The native fixture establishes platform behavior; it does not claim a
guest runtime lookup without executing that runtime.

## Unchanged application parity gate

Build the C++ runtime with `make -C cpp app METAL_CPP=/path/to/metal-cpp`, then run the bounded app probes from the repository root:

```sh
make verify-apps
```

The default cases include direct and `dlsym` dyld SDK tuple controls, valid and malformed `plutil`, deterministic `tar -tf`, and cache-only/dynamic-provider libz loading. Select a subset with `APP_VERIFY_CASES`, for example `make verify-apps APP_VERIFY_CASES='dyld-sdk plutil'`. Add `jni` when qualifying an installed JDK by setting `JDK_HOME` to its home directory and including `jni` in the case list. `APP_VERIFY_RUNTIME` selects the frozen C++ launcher, and `APP_VERIFY_RESULTS` selects the parent directory for unique per-run receipts. Set `APP_VERIFY_TIMEOUT_SECONDS` to change the per-process timeout. The gate compares stdout, stderr, and exit status, stores the runtime SHA-256, and fails on timeout or runtime mutation.

`make verify-dyld-image-enumeration-native` records the host dyld image count,
checks header/name bounds, verifies failed-load snapshot stability, and proves
that a successful `dlopen` publishes its requested image before a new dependency.
`make -C cpp verify-guest-image-registry` checks guest registry ordering,
out-of-range results, deduplication and uncommitted transaction rollback.
`make verify-dyld-image-slide-native` checks that every native image index
returns the slide implied by its `__TEXT` vmaddr and that an out-of-range index
returns zero.

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

## Guest printf integer formats

The native ARM64 oracle checks `%02x`, `%#llx`, and `%zu` across the Darwin
variadic stack slots. The CPU-only formatter check uses the same slot sequence,
verifies 32-bit narrowing versus 64-bit values, preserves existing conversions,
and rejects unsupported or oversized formats:

```sh
mkdir -p .build/handoff-probes
xcrun clang -Wall -Wextra -Werror \
  fixtures/handoff/guest-printf-formats.c -o .build/handoff-probes/guest-printf-formats
.build/handoff-probes/guest-printf-formats
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -pedantic -UNDEBUG -Icpp \
  fixtures/handoff/guest-format-test.cpp cpp/guest_format.cpp \
  -o .build/handoff-probes/guest-format-test
.build/handoff-probes/guest-format-test
```

Expected native output is `1 2 3 4 5 6|0a|0x123456789abcdef|4294967301|0|-0042`.

## Fortified guest memcpy

`make -C cpp verify-memcpy-chk` builds a native ARM64 control with an explicit
`___memcpy_chk` import. It checks a successful bounded copy, confirms an
oversized copy terminates through the native checked-failure trap (shell status
133 on this host), and runs CPU assertions for direct and cached GPU lowering.
An `SA_SIGINFO` native control captures SIGTRAP's signal number, `si_code`, fault
PC, and address before exiting from its handler with status 77. The guest
overflow service is wired to deliver the same signal and code through
`GuestSignalRuntime`; its fault PC/address are guest call-site values, not native
libsystem addresses. The successful GPU path remains a GPU byte-copy; only an
object-size violation crosses to the host failure service.

## SHA-256 crypto instructions

The failing instruction at `libcorecrypto`'s `_AccelerateCrypto_SHA256_compress`
is `SHA256SU0`; its adjacent compression loop uses `SHA256H`, `SHA256H2`, and
`SHA256SU1` as well. `make -C cpp verify-vector-crypto` checks direct lowering,
compiles the emitted Metal helpers to AIR without dispatching a GPU kernel, and
compares a native ARM64 intrinsic control against the committed result file.

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
value expressions live in `cpp/integer_ops_shared.h`; that header supplies
the runtime-generated translator/function shader source and the AOT
interpreter implementation.

## Typed ADD/SUB immediate slice

The native control covers W/X results, LSL-by-12 immediates, and preservation
of NZCV across non-flag-setting ADD and SUB:

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror \
  fixtures/handoff/typed-add-sub-immediate.s \
  fixtures/handoff/typed-add-sub-immediate-main.c \
  -o .build/handoff-probes/typed-add-sub-immediate
.build/handoff-probes/typed-add-sub-immediate
```

Expected output is
`add=123005,10000 sub=2332d000,1221d000 nzcv=20000000`.
The native edge fixture also checks X/W overflow, carry and borrow, CMP/CMN
aliases, and SP/WSP updates. Non-flag-setting ADD/SUB uses SP for encoded
register 31 as either operand; flag-setting forms use SP for Rn=31 and discard
the result for Rd=31, matching CMP/CMN. The common helper preserves NZCV for
non-flag-setting forms and applies W-result zero-extension.

The flag and stack-pointer native control is built with:

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror \
  fixtures/handoff/typed-add-sub-flags.s \
  fixtures/handoff/typed-add-sub-flags-main.c \
  -o .build/handoff-probes/typed-add-sub-flags
.build/handoff-probes/typed-add-sub-flags
```

Expected output is
`adds=90000000,60000000 subs=80000000,80000000 cmp=60000000 cmn=60000000 cmpsp=20000000 wz=0 sp=2000,fffffffffffff000,80000001`.

`fixtures/interpreter/add-sub-immediate-main.s` is the repeatable process-entry
probe: MOVZ W0, ADD W0, SUB W0, RET must exit 42 with a four-operation
interpreter dispatch. `fixtures/interpreter/add-sub-sp-flags-main.s` adds SP
round-tripping and CMP-to-ZR followed by a conditional branch, and must exit
42 after a four-operation interpreter block and translated branch fallback.

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
`invalid=268435459 overflow=4 unmapped=1 page=16384 current=0 restore=0/read=0 lower=0 raise=2 write=2 mprotect-rw=-1/13 read=0/0 middle=83 edges=0,0:50,116 dealloc=0 hole=-1/12 release=0,0` and empty stderr. The invalid task result is `MACH_SEND_INVALID_DEST`; arithmetic overflow returns `KERN_INVALID_ARGUMENT`, and an unmapped target returns `KERN_INVALID_ADDRESS`.
## Typed scalar memory decode

The canonical operation decoder records unsigned-immediate W/X loads and
stores as destination, base, scaled immediate, shift, and bit width. This
unchanged ARM64 fixture supplies native output for W/X accesses and SP base:

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror fixtures/handoff/typed-memory.s \
  fixtures/handoff/typed-memory-main.c -o .build/handoff-probes/typed-memory
.build/handoff-probes/typed-memory
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -pedantic -Icpp \
  fixtures/handoff/typed-operation.cpp cpp/translate_core.cpp \
  -o .build/handoff-probes/typed-operation
.build/handoff-probes/typed-operation
```

The process decoder keeps its existing guest-memory lowering for these
instructions so permission checks and demand-cache retry remain active.
## Blocking local Mach receive

The native arm64 oracle uses raw `mach_msg2` traps inside a pthread receiver.
It covers guest-local send wakeup, a finite receive timeout, and destruction
of a receive right while another thread is blocked. The checked-in source is
the oracle for exact native return values; the guest runtime comparison is
performed only when a GPU slot is explicitly assigned.

```sh
src=fixtures/handoff/mach-local-blocking-receive.c
out=.build/handoff-probes/mach-local-blocking-receive
mkdir -p .build/handoff-probes
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 "$src" -o "$out"
"$out"
```

Native output is `send_woke=1 timeout=268451843 destroy_result=0 destroy_wake=268451846`; status is 0.

## Late local Mach message after receive timeout

`mach-local-late-message.c` leaves the main thread runnable while a worker's
10 ms local receive times out. The main thread yields until the worker blocks,
then busy-spins for 100 ms and sends two messages immediately without joining
or yielding. It then joins and receives them from the same port in FIFO
order. This catches stale waiter delivery consuming a message after its
deadline while also checking that the late message remains queued.

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 \
  fixtures/handoff/mach-local-late-message.c -o /tmp/mach-local-late-message
/tmp/mach-local-late-message
```

Expected normalized output is
`receiver_entered=1 receiver_timed_out=1 sender_runnable_past_deadline=1 late_sends=1 late_receive=1 fifo=1 cleanup=1`; status is 0.

## Mach port type query (`-76`)

`mach-port-type.c` calls the SDK-declared raw `_kernelrpc_mach_port_type_trap`
(task name, queried name, output pointer) and checks live task/host/thread
send rights, receive and combined send/receive rights, the dead-name transition
when a send right survives receive destruction, and the dead sentinel. It also
checks synthetic clock/bootstrap service names, invalid and deallocated names,
invalid task destinations, output preservation on errors, and a bad output
pointer. The ordinary rights tested have no DNREQUEST, SPREQUEST, or delayed
SPREQUEST flags.

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 \
  fixtures/handoff/mach-port-type.c -o /tmp/mach-port-type
/tmp/mach-port-type
```

Native normalized output:

```text
special_ports task=0x00010000/1 host=0x00010000/1 thread=0x00010000/1
local_rights receive=0x00020000/1 send_receive=0x00030000/1 after_send_release=0x00020000/1 destroyed_name=1
dead_names right=0x00100000/1 sentinel=0x00100000/1 released=1
errors null=15/1 unknown=15/1 invalid_task=0x10000003/1 host_as_task=0x10000003/1 null_name_null_output=15 unknown_null_output=15 bad_task_precedence=1 invalid_output=10
services clock=0x00010000/1 bootstrap=0x00010000/1 bootstrap_released_invalid=1 kernel_flags_absent=1
```

Status is 0. Error outputs use a sentinel and remain unchanged for invalid
names and task destinations; a null output pointer returns `KERN_MEMORY_ERROR`
(10). Task validation precedes name validation, which precedes output copyout;
invalid task/name results therefore take precedence over a null output pointer.
The guest maps these queries only through its synthetic right namespace;
clock and bootstrap host send rights are kept in host-owned maps and are never
returned as guest names. Fixture `host-clock-service.c` is the existing end to
end clock-right query/release control.

## Mach port reference query (`mach_port_get_refs`, MIG 3207)

`mach-port-get-refs.c` exercises the public MIG path used by
`host-clock-service.c` after acquiring a clock service. The SDK maps
`mach_port_get_refs` to request ID 3207; the success reply is ID 3307. The
runtime supports exact send, receive, and dead-name counts from its guest
namespace, including zero counts for absent right kinds on known names. It
returns `KERN_INVALID_TASK` for a host port used as a task target and preserves
the caller's output on MIG errors. Null/dead sentinel names follow native
send-right count behavior. This fixture contacts only the task/host ports
already used by the local Mach slice and the previously scoped clock/bootstrap
service probes.

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 \
  fixtures/handoff/mach-port-get-refs.c -o /tmp/mach-port-get-refs
/tmp/mach-port-get-refs
```

Native normalized output:

```text
special_send=1
receive allocate=0 right=0:1 send_zero=0:0 insert=0 send=0:1 remove=0 send_after=0:0 destroy=0 gone=1
dead destroy=0 dead_name=0:1 send=0:0 release=0 gone=1
sentinels null_send=0:1 dead_send=0:1 null_receive_invalid=1 dead_name_invalid=1
errors unknown=1 invalid_task=1 host_task=1 right=1 send_once=0:0 port_set=0:0
```

Status is 0. The strict guest error precedence is separate from this MIG
message; public self wrappers have their own control below.

`mach-port-get-refs-special.c` is the reduced MIG 3207 control: it asks only
for the current task's send urefs, with no port allocation, insertion, or
deallocation. Native output is `task_send_refs_result=0 valid=1`; status is 0.
Use this fixture for the focused cached 3207 gate, separately from raw trap
`-19` rights-lifecycle coverage below.

## Public Mach host and thread self wrappers

`mach-public-self.c` calls the executable-imported `_mach_host_self` and
`_mach_thread_self` wrappers, checks for non-sentinel stable names, then releases
each acquired right through the raw deallocate trap. `_mach_task_self_` remains a
modeled data symbol. The fixture avoids the separate `_mach_port_get_refs` and
`_mach_port_deallocate` executable imports, so it isolates the public self
wrappers and raw right lifecycle. It makes no host service connection.

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 \
  fixtures/handoff/mach-public-self.c -o /tmp/mach-public-self
/tmp/mach-public-self
```

Native normalized output is
`task_valid=1 host_valid=1 host_same=1 thread_valid=1 thread_same=1 releases=1`;
status is 0.

## Raw Mach port reference modification (`-19`)

`mach-port-mod-refs.c` uses the SDK-declared raw
`_kernelrpc_mach_port_mod_refs_trap(target, name, right, delta)` ABI and raw
receive allocation. It covers send uref increment/decrement, underflow and
oversized-delta errors, invalid target/name/right ordering, null/dead send
sentinels, the receive-right decrement lifecycle, and dead-name uref increment
and decrement after receive destruction. The native task-self baseline has
three send urefs in this process; both controls reduce it to the one-reference
floor, then verify `KERN_INVALID_CAPABILITY` instead of destroying the active
task name. Send-once mutation remains outside this supported slice. It also
checks that SEND and DEAD_NAME urefs peg at
`MACH_PORT_UREFS_MAX` until a `-MACH_PORT_UREFS_MAX` delta releases them.

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 \
  fixtures/handoff/mach-port-mod-refs.c -o /tmp/mach-port-mod-refs
/tmp/mach-port-mod-refs
```

Native normalized output:

```text
task_send plus=0 minus=0 floor_step=0 floor_hit=20 deallocate_floor=20 underflow=18 oversized_delta=18
errors target=268435459 unknown=15 right=18 unsupported=17/15 sentinels null=0 dead=0
receive allocate=0 zero=0 plus=18 minus=0 missing=15
saturated allocate=0 insert=0 plus=0 deallocate=0 minus_one=0 minus_max=0 send_missing=17 destroy=0
dead allocate=0 insert=0 destroy=0 send=17 receive=17 send_once=17 refs=0/0/0/0 missing=15
```

Status is 0.

`mach-port-mod-refs-guarded.c` constructs a guarded receive right and then
attempts `mod_refs(RECEIVE, -1)`. The native kernel delivers a Mach port guard
violation as `SIGKILL`; expected stdout is `guarded_port_ready=1` and status is
137. The guest path uses the same synchronous signal runtime and must terminate
instead of returning success.

## Allocator fixed `mach_vm_map` overwrite

`libsystem_malloc`'s `_mfm_initialize` maps a tagged 4 MiB anonymous region over
an address it already allocated. The installed allocator uses tag
`VM_MEMORY_MALLOC_TINY` plus `VM_FLAGS_OVERWRITE`, RW current protection and
all maximum protections. The success path retains the fixed address and clears
the replaced bytes. Its `BRK #1` cold path is reached only when this map call
returns an error; it is not a breakpoint to skip.

The guest service supports this overwrite only inside one owned dynamic mapping,
with a fully covered page range and non-executable, non-JIT regions. It preserves
the mapping's backing ownership, updates current and maximum permissions, and
zero-fills only after replacement metadata is staged. Loader/pinned mappings,
MTE, executable destinations, ranges crossing unmapped pages, and arbitrary
fixed mappings into free holes remain unsupported. Native control confirms the
kernel does allow a fixed overwrite into a valid hole; the guest implementation
does not yet allocate backing at arbitrary guest addresses. A null target is
rejected with `KERN_INVALID_ADDRESS`. `make -C cpp verify-mach-vm-overwrite-native`
checks native full and partial overwrites, a valid hole mapping, address
preservation and zero fill, the null-target error, and rejection of an
unsupported flag.
