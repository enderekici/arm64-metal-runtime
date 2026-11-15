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

## Process BRK #1 and SIGTRAP

`brk-sigtrap.c` first branches around an adjacent `BRK #1`, then executes BRK
immediates `#0`, `#1`, `#2`, `#0x1000`, `#0xf000`, and `#0xf001`. On native
macOS each produces SIGTRAP (`si_code=0`) with `si_addr` equal to the saved PC;
advancing that PC by four bytes resumes. No special behavior was observed for
the tested `#0xf000` and `#0xf001` values. Native output is
`untaken=0 variants=6 traps=6 signal=5 code=0 address_pc_mismatches=0 resumed=7`.

`brk-default-termination.c` leaves SIGTRAP at its default disposition. Native
and process-mode guest both print `before`, omit `after`, and exit with status
133 (`128 + SIGTRAP`).

```sh
xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -pedantic \
  fixtures/handoff/brk-sigtrap.c -o .build/handoff-probes/brk-sigtrap
.build/handoff-probes/brk-sigtrap
xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -pedantic \
  fixtures/handoff/brk-default-termination.c -o .build/handoff-probes/brk-default
.build/handoff-probes/brk-default
```

`make verify-apps APP_VERIFY_CASES=brk` compares native and process-mode guest
stdout, stderr, and exit status for both controls. Process mode delivers any
BRK immediate as a synchronous SIGTRAP; `decode_process` and the standalone
function translator continue to reject raw BRK instructions. PAC decoding and
checks are unchanged.

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

`task-special-port.c` sends raw task MIG request 3409 to the guest task name
with selector 4 (`TASK_BOOTSTRAP_PORT`). It verifies the 40-byte complex reply,
one `MOVE_SEND` port descriptor, send-right type, and the 8-byte minimum trailer.
The returned name is normalized and deallocated. Native error controls confirm
selector 1 and -1 return `KERN_DENIED` (53), while selectors 0 and 7 return
`KERN_INVALID_ARGUMENT` (4); those replies are 36 bytes plus the minimum
trailer. The guest models only selector 4 and these observed errors. Other
selectors remain unsupported. No host bootstrap port or external service right
is copied into the guest namespace.

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 \
  fixtures/handoff/task-special-port.c -o /tmp/task-special-port
/tmp/task-special-port
```

Native normalized output is status 0:

```text
selector=4 valid=1 status=0 bits=0x80001200 size=40 reply=3509 descriptor=0x10011 right_type=0x10000 type_status=0 trailer=0/8 error=0
selector=1 valid=1 status=0 bits=0x1200 size=36 reply=3509 descriptor=0x0 right_type=0x0 type_status=0 trailer=0/8 error=53
selector=0 valid=1 status=0 bits=0x1200 size=36 reply=3509 descriptor=0x0 right_type=0x0 type_status=0 trailer=0/8 error=4
selector=7 valid=1 status=0 bits=0x1200 size=36 reply=3509 descriptor=0x0 right_type=0x0 type_status=0 trailer=0/8 error=4
selector=-1 valid=1 status=0 bits=0x1200 size=36 reply=3509 descriptor=0x0 right_type=0x0 type_status=0 trailer=0/8 error=53
```

The same raw fixture is suitable for the guest launcher:

```sh
env ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 \
  /tmp/arm64-task-special-port-build/arm64-metal launch /tmp/task-special-port
```

`mach-local-move-send.c` uses raw traps `-16`, `-21`, `-18`, `-19`, `-76`, and
`-47` to cover one-, two-, and three-port complex messages using `MOVE_SEND` and
`COPY_SEND`. It checks MOVE_SEND leaves only the source receive right while
queued, COPY_SEND preserves the source send right, and successful receive
normalizes each descriptor to MOVE_SEND. Two-descriptor coverage includes a
successful mixed MOVE/COPY transfer and an invalid second right. Native Mach
validates descriptor structure before copying rights, then processes rights in
order: an invalid second right prevents message enqueue, preserves earlier
COPY_SEND refs, and leaves an earlier MOVE_SEND consumed. The invalid right
returns `MACH_SEND_INVALID_RIGHT` (`0x1000000a`). A malformed second descriptor
is rejected before right copyin, so an earlier MOVE_SEND remains in the source
namespace; it returns `MACH_SEND_MSG_TOO_SMALL` (`0x10000008`). The controls also cover
too-small receive (message and transferred right are discarded), invalid
receive buffer (message is consumed and MOVE_SEND is restored), and destination
destruction (queue contents are dropped). The guest bounds descriptor traversal
by the declared message size and validates the full descriptor array before
processing any rights. This slice supports send rights on live receive names in
the same guest namespace; it does not implement MOVE_RECEIVE, send-once, or
cross-namespace name allocation.
The binary imports only `_mach_task_self_` and `printf`; it has no bootstrap or
host-service contact.

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 \
  fixtures/handoff/mach-local-move-send.c -o /tmp/mach-local-move-send
/tmp/mach-local-move-send
```

Native output is status 0:

```text
move=0 pending=0/0 receive=0 descriptor=1 copy=0 pending_send=1 receive=0 descriptor=1 too_large=268451844/0 dropped_send=1 retry=268451843 bad_buffer=268451848 restored_send=1 retry=268451843 destroy_move=0/0 dropped_send=1 destroy_copy=0/0 kept_send=1 mixed=0 pending=0/0+1 receive=0 descriptors=1 three=0 receive=0 descriptors=1 truncated=268435464 kept_first=1 receive=268451843 malformed_second=268435464 kept_first=1 receive=268451843 partial_move=268435466 pending=0/0 receive=268451843 partial_copy=268435466/268451843 retained_send=1
cleanup=1
```

The same binary is suitable for the guest launcher:

```sh
env ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 \
  .build/cpp/arm64-metal launch /tmp/mach-local-move-send
```

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

## Fortified guest memset

`make -C cpp verify-memset-chk` builds a native ARM64 control with an explicit
`___memset_chk` import. It checks a successful bounded fill, confirms an
oversized fill terminates through the native checked-failure trap (shell status
133 on this host), and runs CPU assertions for direct and cached GPU lowering.
An `SA_SIGINFO` native control captures SIGTRAP's signal number, `si_code`, fault
PC, and address before exiting from its handler with status 77. The guest
overflow service delivers SIGTRAP through `GuestSignalRuntime`, matching the
existing fortified-copy behavior. Successful fills remain guest-side writes.

## Guest append-mode opens

`make -C cpp verify-open-append-native` exercises native `_open` and raw
`open_nocancel` with owned temporary paths. Each opens with
`O_WRONLY|O_CREAT|O_APPEND`, writes once, seeks to offset zero, writes again,
then checks the file is `seedAB` and the descriptor offset is six. The host
service keeps the OS `O_APPEND` file-description behavior and records newly
opened writable descriptors; raw `open_nocancel` shares that same open service.

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

## Self task name port (`task_get_special_port`, selector 3)

`task-name-port-self.c` uses the same arm64 Mach-O for native and guest runs.
It asks the current task twice for `TASK_NAME_PORT`, verifies the same distinct
send-right name with two urefs, releases both references, and checks that the
local name becomes invalid. It does not request or expose a host task port.

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 \
  fixtures/handoff/task-name-port-self.c -o /tmp/task-name-port-self
/tmp/task-name-port-self
ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 \
  .build/cpp/arm64-metal launch /tmp/task-name-port-self
```

Expected native normalized result is `get=0 repeat=0 same=1 nonnull=1
distinct=1 type=0 send=1 refs2=1 refs1=1 release=0/0 after=15
output_untouched=1 result=PASS`.
The guest run validates only its synthetic task-name role right; any separate
ClientIPC identity token remains independent.

## `task_name_for_pid` self-only trap (`-44`)

`task-name-for-pid-self.c` uses the installed three-argument Mach trap wrapper
for the current task only. It verifies a distinct task-name send right, then
checks that an impossible negative PID returns `KERN_FAILURE` and clears a
valid output word; a bad output address on that failure path also returns
`KERN_FAILURE`, matching the installed trap's ignored `copyout` status. The
runtime maps only the current guest task and current process ID to the synthetic
guest task-name capability; it never forwards arbitrary PID lookups to the host.

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 \
  fixtures/handoff/task-name-for-pid-self.c -o /tmp/task-name-for-pid-self
/tmp/task-name-for-pid-self
ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 \
  .build/cpp/arm64-metal launch /tmp/task-name-for-pid-self
```

The native control prints `own=0 own_nonnull=1 own_distinct=1 type=0 send=1
release=0 missing=5 missing_null=1 invalid_output=5 result=PASS`. The guest
command is staged for differential replay; it was not run in this CPU-only
change.

## Mach port construct option bits

`mach-port-construct-flags.c` checks `MPO_QLIMIT`, `MPO_TEMPOWNER`,
`MPO_IMPORTANCE_RECEIVER`, `MPO_INSERT_SEND_RIGHT`, `MPO_STRICT` and the Azul
combination `0x33` (guard, qlimit 1, send right, strict): rights via type and
`mach_port_get_refs`, destruct, and `KERN_INVALID_VALUE` for qlimit 1025.

```sh
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 \
  fixtures/handoff/mach-port-construct-flags.c \
  -o .build/handoff-probes/mach-port-construct-flags
.build/handoff-probes/mach-port-construct-flags
ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 \
  .build/cpp/arm64-metal launch .build/handoff-probes/mach-port-construct-flags
```

## Guarded strict port construction (`0x31`)

`mach-port-construct-31.c` uses the raw construct, type, and destruct traps,
plus MIG `mach_port_get_refs` (3207). It checks the initial receive and send
rights, one send uref, `srdelta=-1` destruction with the matching guard, and
`KERN_INVALID_ARGUMENT` with an untouched output name for an unused flag bit.
It does not depend on `mach_port_get_attributes`. The companion
`mach-port-construct-31-guard-mismatch-native.c` isolates a wrong strict guard
context in a child and requires native SIGKILL termination.

```sh
mkdir -p .build/handoff-probes
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 \
  fixtures/handoff/mach-port-construct-31.c \
  -o .build/handoff-probes/mach-port-construct-31-native
.build/handoff-probes/mach-port-construct-31-native
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 \
  fixtures/handoff/mach-port-construct-31-guard-mismatch-native.c \
  -o .build/handoff-probes/mach-port-construct-31-guard-mismatch-native
.build/handoff-probes/mach-port-construct-31-guard-mismatch-native
xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 \
  fixtures/handoff/mach-port-construct-31.c \
  -o .build/handoff-probes/mach-port-construct-31-guest
ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 \
  .build/cpp/arm64-metal launch \
  .build/handoff-probes/mach-port-construct-31-guest
```

The guest command requires the runtime app build and a GPU execution slot.

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

Sparse replacement now retries with bounded shared-buffer growth when retired
backing leases still leave no single contiguous block. Growth copies the old
buffer, reserves the appended range exclusively for VM mappings, advances the
host heap start past it, and publishes an unreadable resident guard so appended
backing cannot be reached as a linear guest address. The bound is the minimum
of Metal's maximum buffer length, the guest linear-address limit, and the
allocator's signed-offset limit. Guest regions remain individually contiguous,
so existing CPU/GPU memory access rules do not span physical backing extents.
Ordinary `mach_vm_allocate` remains restricted to the original linear backing
arena; it rejects an appended sparse tail instead of forming a linear guest
address that the original deallocation bounds cannot own.

## Named Mach VM read and protect services

`_mach_vm_protect` uses the same allocation and sparse-reservation callback as
the raw `mach_vm_protect` trap. `_mach_vm_read_overwrite` reads from guest VM
through `GuestMemory`, snapshots source bytes before writing to handle aliases,
and writes the output size only after a complete successful copy. An invalid
source leaves destination and output size unchanged. A destination that reaches
an inaccessible page can receive the writable prefix before returning
`KERN_INVALID_ADDRESS`, matching the native task-self probe.

Run the native differential fixture and CPU-only dispatch checks with
`make -C cpp verify-mach-vm-named-services METAL_CPP=/path/to/metal-cpp`.
The native fixture covers overlapping source/destination, invalid source and
destination ranges, preserved output size on failure, invalid task handling,
and the named protect wrapper. The CPU test calls the registered host services
directly and checks copyout behavior without dispatching a Metal kernel.

## Named Mach VM region query

`mach-vm-region.c` queries the logical VM map through `_mach_vm_region`. Its
native oracle checks a reserved interval, two mapped owners, the next region
after a deallocated hole, basic-info count and offset fields, a null object
name, and atomic error replies for an invalid task, flavor, or info count. Run
the native oracle with `make -C cpp verify-mach-vm-region-native`. Guest runtime
parity is a separate replay gate.

## Guest pthread stack attributes and guard pages

The guest pthread runner applies stack size and detach state, enforces a
page-rounded guard for runtime-owned stacks, and uses caller-owned stack memory
without adding a guard; caller-owned guard pages remain controlled by guest
`mprotect`. Attribute import IDs are checked by
`process-attribute-import-map.cpp`. Native controls are `pthread-custom-stack.c`
and `../interpreter/pthread-runtime-owned-guard-retry.c`.
`pthread-attribute-fields.c` checks default values, set/get stack and guard size,
detached creation, and the null-attribute joinable path.

## Raw Mach trap sweep (`mach-trap-raw.c`)

`mach-trap-raw.c` issues raw `svc #0x80` Mach traps (all eight argument
registers set explicitly) and prints one line per observation. Sections are
selected by the first argument: `timers`, `sem`, `misc`, `vm`, `ports`
(default `all`). Timing results are printed as booleans so native and guest
output are identical.

- `timers`: `mk_timer_create/destroy/arm/cancel/arm_leeway` (`-91..-95`),
  expiry message shape (48 bytes, id 0, local port set), blocking, timed and
  polled receives, re-arm, cancel, past deadline, plain-port and dead-name
  errors.
- `sem`: `semaphore_signal_all/signal_thread/wait_signal/timedwait_signal`
  (`-34/-35/-37/-39`) including two blocked waiters woken at once and a
  directed wake.
- `misc`: `mach_wait_until` (`-90`), `mach_timebase_info_trap` (`-89`),
  `swtch_pri/swtch/thread_switch` (`-59/-60/-61`), `clock_sleep_trap` (`-62`),
  `mach_generate_activity_id` (`-43`), `thread_get_special_reply_port` (`-50`),
  `_exclaves_ctl_trap`, `iokit_user_client_trap`, `task_for_pid` (`-45`).
- `vm`: `_kernelrpc_mach_vm_allocate/deallocate/map_trap` (`-10/-12/-15`).
- `ports`: `move_member/extract_member/guard/unguard/get_attributes`
  (`-20/-23/-41/-42/-40`) and `_kernelrpc_mach_vm_purgable_control_trap`.

```sh
xcrun clang -arch arm64 -Wall -Wextra -O1 -fno-stack-protector \
  fixtures/handoff/mach-trap-raw.c -o /tmp/mach-trap-raw
/tmp/mach-trap-raw timers
ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 \
  ARM64_METAL_CACHE_BOOTSTRAP=1 ARM64_METAL_CACHE_INITIALIZERS=1 \
  .build/arm64-metal-cpp-macos/arm64-metal launch /tmp/mach-trap-raw timers
```

Guest and native stdout must match line for line. Build without
`-fno-stack-protector` or call `setvbuf(stdout, 0, _IONBF, 0)` and the guest
dies at startup ("resumed outside translated text at PC 0x0"), so the fixture
uses `fflush` instead. Documented deviations from the native kernel: the
`result_time` of `mk_timer_cancel` is the armed deadline (the kernel returns it
plus timer leeway), `clock_sleep_trap` stores the computed wake time rather than
the time observed after waking, and the special reply port is a plain receive
right (native also carries a send-once right).

## Port-set receive (`mach-portset-recv.c`)

Receives on a Mach port set holding an `mk_timer` port, through libc `mach_msg` (vector-form `mach_msg2`
with RCV_LARGE, RCV_VOUCHER and requested trailer elements): blocking wake, timeout, queued message and
RCV_LARGE too-large. Native and guest stdout are identical.

```sh
clang -arch arm64 fixtures/handoff/mach-portset-recv.c -o /tmp/mach-portset-recv
/tmp/mach-portset-recv
.build/arm64-metal-cpp-macos/arm64-metal launch /tmp/mach-portset-recv
```

## csops all-operations differential

`csops-all-ops.c` issues every CS_OPS_* operation 0..24 against the calling
process through libc `csops`/`csops_audittoken` and raw `svc` 169/170, for pid
0, the own pid and a missing pid, with several buffer sizes, then walks the
status-mutating operations. Output goes through `write(2)` (guest `fflush`
is unsupported). Run it natively and as a guest and require identical stdout:
build it, then make `-ent` (`codesign -s - -i com.test.csops --options runtime
--entitlements <allow-jit plist>`) and `-plain` (`codesign -s -`) copies next to the
linker-signed original. `csops-all-ops kill <mode>` (mode 0..3) issues
MARKINVALID and must exit 137 (SIGKILL) in both worlds. The full guest run
takes about 20 minutes per binary.

## Generic mach_msg2 kernel-object sends and JVM startup RPCs

`mach-msg2-generic.c <case>` sends raw `mach_msg2` (trap -47) messages with unknown ids to the task, host and
thread ports, invalid/null/dead destinations, a receive-only on a send right, and legacy/vector sends to an own port.
`jvm-startup-rpcs.c` runs the libsystem/CF startup calls a JVM makes. Run both natively and as guests with the
shared-cache env vars; remaining differences are listed by the `arm64-metal mach-msg2-unhandled` stderr lines.

## Boundary survey and trace

`ARM64_METAL_SURVEY=1` turns unmodeled-service fatals (unknown BSD syscall or
Mach trap, workqueue op, port-construct flags, port attribute flavor, MAC
policy, sandbox query shape, quarantine `getinfo_fd`, raw sigaction flags) into
a logged error return and the run continues. It also logs soft fallthroughs that
already return an error: `mach-msg2-unhandled` ids, ENOTSUP/ENOSYS stubs, and
unknown proc_info and sysctl selectors. One `SURVEY kind=.. num=.. sub=.. pc=..
lr=.. thread=..` line per (kind, num, sub) is appended to `ARM64_METAL_SURVEY_LOG`
(stderr if unset). Without the variable the fatals are unchanged.
`errno_stub` and `enotsup` lines are hints only; some fire when the host
itself returns ENOTSUP. `ARM64_METAL_BOUNDARY_TRACE=<path>` appends a `BOUNDARY` line for every raw
syscall and Mach trap with x0-x7 (mach_msg2 carries msgh_id as `sub`).

`survey-unhandled.c` sends msgh_id 0x7777 to the host port twice, then issues
aio_cancel and aio_error twice each. Native output is a MIG_BAD_ID (-303) reply,
then EBADF and EINVAL. Without the survey variable the guest prints the same
MIG_BAD_ID replies (plus the existing `mach-msg2-unhandled` stderr lines) and then
stops at syscall 0x13c. With it the guest continues: ENOSYS (78) for both
syscalls, then `done`. The log gets three lines: msgh_id 30583, syscall 316 and
syscall 317.

## dyld APIs vtable slots (`dyld-api-slots.c`)

libdyld exports that are not in `host_service_registry.hpp` run their cached code and
call a `dyld4::APIs` vtable slot. Native reachability comes from
`tools/dyldapis/build_tracer.sh` (DYLD_INSERT_LIBRARIES counting stubs over all 225
libdyld text exports) under java -version, the JNI entry, plutil -lint and tar list/extract.
Outside the registry, natively reached are the façade core slots plus four
reached only from libsystem_trace under java: `_dyld_get_shared_cache_uuid` (0x1e0),
`_dyld_register_func_for_remove_image` (0x38), `dyld_image_header_containing_address`
(0x1b0) and `dyld_image_path_containing_address` (0x168). These are served by
services 260-263 (`cpp/dyld_api_services.inc`, slots from `cpp/dyld_api_slots.hpp`).

- containing-address answers for any shared-cache image, loaded or not (native does).
  Addresses in a non-text segment of an unloaded cache image return null (gap).
- remove-image registration is recorded; dyld calls it on unload, the runtime does not,
  so unloading an image after a registration stops with
  `Guest image 0x... unload needs N _dyld_register_func_for_remove_image callback(s); not modeled`.
- any other slot reached by guest code stops with
  `...; unmodeled dyld APIs vtable slot 0x<off> (index N) reached from libdyld <exports>, guest lr 0x...`.
  `ARM64_METAL_TRACE_DYLD_API_ABI=1` prints every derived slot.

```sh
xcrun clang -arch arm64e -O1 -fno-stack-protector fixtures/handoff/dyld-api-slots.c -o dyld-api-slots
xcrun clang -arch arm64e -O1 -dynamiclib fixtures/handoff/dyld-api-slots-lib.c -o lib.dylib
OFF=$(./dyld-api-slots --find-appkit)      # native only: AppKit's cache offset
./dyld-api-slots $OFF                      # guest output must equal native
./dyld-api-slots --unmodeled               # guest: named slot 0x1d8 error
./dyld-api-slots --remove-exit $PWD/lib.dylib     # guest = native, exit 0
./dyld-api-slots --remove-dlclose $PWD/lib.dylib  # native removed_after_close=1; guest: named unload error
```
On the guest the `cache-image` line reads `header=mismatch dladdr_base=none path=AppKit`:
the slot answers match native, the guest `dladdr` (service 165) does not resolve unloaded
cache images.
