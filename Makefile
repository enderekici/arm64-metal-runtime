BUILD := .build
CPP_RUNTIME ?= $(BUILD)/cpp/arm64-metal
CPP_INTERPRETER_METALLIB ?= $(BUILD)/cpp/interpreter/runtime-interpreter.metallib
METAL_CPP ?= ../metal-cpp
GUEST := $(BUILD)/workload.dylib
RUNTIME ?= $(CPP_RUNTIME)
CONSOLE := $(BUILD)/console
STREAMS := $(BUILD)/streams
INDIRECT := $(BUILD)/indirect
REBASED := $(BUILD)/rebased
DATA_AUTH := $(BUILD)/data-auth
UNSIGNED_CALL := $(BUILD)/unsigned-call
LINKED := $(BUILD)/linked
WRAPPER := $(BUILD)/libbridge.dylib
SUPPORT := $(BUILD)/libsupport.dylib
REGION := $(BUILD)/region
PIPE := $(BUILD)/guest-pipe
SIGNAL := $(BUILD)/guest-signal
VECTOR := $(BUILD)/vector-memory
HEAP := $(BUILD)/guest-heap
THREADS := $(BUILD)/guest-threads
MEMORY := $(BUILD)/guest-memory
CACHE_CHECK := $(BUILD)/cache-rebase-check
CACHE_ZLIB := $(BUILD)/cache-zlib
CACHE_CONFORMANCE := $(BUILD)/cache-dyld-conformance

.PHONY: all verify verify-function-stream verify-cache verify-apps verify-java bundle
all: $(RUNTIME) $(GUEST) $(CONSOLE) $(STREAMS) $(INDIRECT) $(REBASED) $(UNSIGNED_CALL) $(DATA_AUTH) $(LINKED) $(REGION) $(PIPE) $(SIGNAL) $(VECTOR) $(HEAP) $(THREADS) $(MEMORY)

verify: all
	sh verify-launch.sh $(RUNTIME) $(CONSOLE) $(STREAMS) $(INDIRECT) $(REBASED) $(LINKED) $(REGION) $(PIPE) $(BUILD) $(SIGNAL) $(VECTOR) $(HEAP) $(THREADS) $(MEMORY)
	@if $(RUNTIME) launch $(UNSIGNED_CALL) >"$(BUILD)/auth.out" 2>"$(BUILD)/auth.log"; then \
		echo 'Unsigned BLRAAZ target was accepted' >&2; exit 1; else status=$$?; fi; \
	test "$$status" -eq 1
	@test ! -s "$(BUILD)/auth.out"
	@grep -Eq '^arm64-metal: Guest branch authentication failed at 0x[0-9a-f]+, pointer 0x[0-9a-f]+, modifier 0x[0-9a-f]+$$' "$(BUILD)/auth.log"
	@$(DATA_AUTH)
	@$(RUNTIME) launch $(DATA_AUTH)
	@ulimit -c 0; for args in "modifier" "wrong key"; do \
		if $(DATA_AUTH) $$args >"$(BUILD)/data-auth.native" 2>&1; then exit 1; else test $$? -gt 128 || exit 1; fi; \
		if $(RUNTIME) launch $(DATA_AUTH) $$args >"$(BUILD)/data-auth.gpu" 2>&1; then exit 1; fi; \
		grep "Guest data-pointer authentication failed" "$(BUILD)/data-auth.gpu" || exit 1; \
	done
	@ulimit -c 0; if $(DATA_AUTH) wrong code modifier >"$(BUILD)/code-auth.native" 2>&1; then exit 1; else test $$? -gt 128; fi
	@! $(RUNTIME) launch $(DATA_AUTH) wrong code modifier >"$(BUILD)/code-auth.gpu" 2>&1
	@grep "Guest branch authentication failed" "$(BUILD)/code-auth.gpu"
	$(RUNTIME) verify $(GUEST) update_words
	$(RUNTIME) verify $(GUEST) xor_words
	@test "$$($(RUNTIME) run $(GUEST) update_words example.json)" = '[{"result":54,"words":[3,4,40]},{"result":42,"words":[]}]'
	@! $(RUNTIME) run $(GUEST) update_words example.json 1 >"$(BUILD)/budget.out" 2>"$(BUILD)/budget.log"
	@test ! -s "$(BUILD)/budget.out"
	@grep 'block budget exhausted' "$(BUILD)/budget.log"
	@printf '%s\n' '[{"words":[9],"seed":0}]' | $(RUNTIME) run $(GUEST) read_second_word - >"$(BUILD)/bounds.out" 2>"$(BUILD)/bounds.log"; test $$? -eq 1
	@test ! -s "$(BUILD)/bounds.out"
	@grep 'guest memory out of bounds' "$(BUILD)/bounds.log"
	@! $(RUNTIME) translate $(GUEST) unsupported >"$(BUILD)/reject.out" 2>"$(BUILD)/reject.log"
	@test ! -s "$(BUILD)/reject.out"
	@grep 'Unsupported ARM64 instruction 0xd4200020' "$(BUILD)/reject.log"
	@printf 'not a Mach-O\n' >"$(BUILD)/invalid.bin"
	@! $(RUNTIME) translate "$(BUILD)/invalid.bin" update_words >"$(BUILD)/invalid.out" 2>"$(BUILD)/invalid.log"
	@grep 'Expected a little-endian ARM64 Mach-O' "$(BUILD)/invalid.log"

verify-function-stream: $(RUNTIME) $(GUEST)
	./verify-function-stream.sh "$(RUNTIME)" "$(GUEST)" "$(BUILD)/function-stream"

APP_VERIFY_RUNTIME ?= $(BUILD)/cpp/arm64-metal
APP_VERIFY_RESULTS ?= $(BUILD)/app-verification
APP_VERIFY_CASES ?= brk dyld-sdk plutil tar libz-root libz-provider

verify-apps:
	@test -x "$(APP_VERIFY_RUNTIME)" || { echo "Build the C++ runtime first with make -C cpp app" >&2; exit 2; }
	./verify-apps.sh "$(APP_VERIFY_RUNTIME)" "$(APP_VERIFY_RESULTS)" $(APP_VERIFY_CASES)

COMPONENT_RUNTIME ?= $(APP_VERIFY_RUNTIME)
COMPONENT_RESULTS ?= $(BUILD)/component-conformance
COMPONENT_MANIFEST ?= component-manifest.json
COMPONENT_CASES ?= all
COMPONENT_JOBS ?= 2
COMPONENT_GPU_SLOTS ?= 4
COMPONENT_GPULOCK ?= $(ARM64_METAL_GPULOCK)
.PHONY: verify-components
verify-components:
	@test -x "$(COMPONENT_RUNTIME)" || { echo "Set COMPONENT_RUNTIME to the frozen C++ launcher" >&2; exit 2; }
	@set -eu; selected='$(COMPONENT_CASES)'; set --; \
	  for case_id in $$selected; do [ "$$case_id" = all ] || set -- "$$@" --case "$$case_id"; done; \
	  lock='$(COMPONENT_GPULOCK)'; if [ -n "$$lock" ]; then set -- "$$@" --gpulock "$$lock"; fi; \
	  exec python3 tools/verify-components.py --manifest "$(COMPONENT_MANIFEST)" \
	    --runtime "$(COMPONENT_RUNTIME)" \
	    --results "$(COMPONENT_RESULTS)" --jobs "$(COMPONENT_JOBS)" \
	    --gpu-slots "$(COMPONENT_GPU_SLOTS)" "$$@"

.PHONY: verify-framework-components
verify-framework-components:
	@test -x "$(COMPONENT_RUNTIME)" || { echo "Set COMPONENT_RUNTIME to the frozen C++ launcher" >&2; exit 2; }
	@set -eu; runtime=$$(realpath "$(COMPONENT_RUNTIME)"); \
	  lib="$${runtime%/*}/resources/interpreter/runtime-interpreter.metallib"; \
	  test -f "$$lib" || { echo "Missing AOT interpreter library: $$lib" >&2; exit 2; }
	@$(MAKE) verify-components COMPONENT_MANIFEST=framework-component-manifest.json

DYNAMIC_IMPORT_RUNTIME ?= $(APP_VERIFY_RUNTIME)
DYNAMIC_IMPORT_BUILD := $(BUILD)/dynamic-import
DYNAMIC_IMPORT_PROVIDER := $(DYNAMIC_IMPORT_BUILD)/libdynamic-import.dylib
DYNAMIC_IMPORT_MAIN := $(DYNAMIC_IMPORT_BUILD)/dynamic-import-main
.PHONY: verify-dynamic-import-stub
$(DYNAMIC_IMPORT_BUILD):
	mkdir -p "$@"

$(DYNAMIC_IMPORT_PROVIDER): fixtures/handoff/dynamic-import-provider.c | $(DYNAMIC_IMPORT_BUILD)
	xcrun clang -arch arm64 -dynamiclib -fPIC -Wl,-fixup_chains \
		-Wl,-install_name,@rpath/libdynamic-import.dylib -Wall -Wextra -Werror "$<" -o "$@"

$(DYNAMIC_IMPORT_MAIN): fixtures/handoff/dynamic-import-main.c | $(DYNAMIC_IMPORT_BUILD)
	xcrun clang -arch arm64 -Wl,-fixup_chains -Wall -Wextra -Werror -pedantic \
		"$<" -o "$@"

verify-dynamic-import-stub: $(DYNAMIC_IMPORT_MAIN) $(DYNAMIC_IMPORT_PROVIDER)
	@test -x "$(DYNAMIC_IMPORT_RUNTIME)" || { echo "Set DYNAMIC_IMPORT_RUNTIME to the C++ launcher" >&2; exit 2; }
	@set -eu; \
	  if "$(DYNAMIC_IMPORT_MAIN)" "$(DYNAMIC_IMPORT_PROVIDER)" \
	    >"$(DYNAMIC_IMPORT_BUILD)/native.stdout" 2>"$(DYNAMIC_IMPORT_BUILD)/native.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$(DYNAMIC_IMPORT_BUILD)/native.status"; \
	  if "$(DYNAMIC_IMPORT_RUNTIME)" launch "$(DYNAMIC_IMPORT_MAIN)" "$(DYNAMIC_IMPORT_PROVIDER)" \
	    >"$(DYNAMIC_IMPORT_BUILD)/guest.stdout" 2>"$(DYNAMIC_IMPORT_BUILD)/guest.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$(DYNAMIC_IMPORT_BUILD)/guest.status"; \
	  cmp "$(DYNAMIC_IMPORT_BUILD)/native.stdout" "$(DYNAMIC_IMPORT_BUILD)/guest.stdout"; \
	  cmp "$(DYNAMIC_IMPORT_BUILD)/native.stderr" "$(DYNAMIC_IMPORT_BUILD)/guest.stderr"; \
	  cmp "$(DYNAMIC_IMPORT_BUILD)/native.status" "$(DYNAMIC_IMPORT_BUILD)/guest.status"; \
	  test "$$rc" -eq 0; \
	  printf 'dynamic dylib host import binding and call match native\n'

DYNAMIC_TLV_RUNTIME ?= $(APP_VERIFY_RUNTIME)
DYNAMIC_TLV_BUILD := $(BUILD)/dynamic-tlv
DYNAMIC_TLV_PROVIDER := $(DYNAMIC_TLV_BUILD)/libdynamic-tlv.dylib
DYNAMIC_TLV_MAIN := $(DYNAMIC_TLV_BUILD)/dynamic-tlv-main
.PHONY: verify-dynamic-tlv
$(DYNAMIC_TLV_BUILD):
	mkdir -p "$@"

$(DYNAMIC_TLV_PROVIDER): fixtures/handoff/dynamic-tlv-provider.c | $(DYNAMIC_TLV_BUILD)
	xcrun clang -arch arm64 -dynamiclib -fPIC -Wl,-fixup_chains \
		-Wl,-install_name,@rpath/libdynamic-tlv.dylib -Wall -Wextra -Werror "$<" -o "$@"

$(DYNAMIC_TLV_MAIN): fixtures/handoff/dynamic-tlv-main.c | $(DYNAMIC_TLV_BUILD)
	xcrun clang -arch arm64 -Wl,-fixup_chains -Wall -Wextra -Werror -pedantic \
		"$<" -o "$@"

verify-dynamic-tlv: $(DYNAMIC_TLV_MAIN) $(DYNAMIC_TLV_PROVIDER)
	@test -x "$(DYNAMIC_TLV_RUNTIME)" || { echo "Set DYNAMIC_TLV_RUNTIME to the C++ launcher" >&2; exit 2; }
	@set -eu; \
	  if "$(DYNAMIC_TLV_MAIN)" "$(DYNAMIC_TLV_PROVIDER)" \
	    >"$(DYNAMIC_TLV_BUILD)/native.stdout" 2>"$(DYNAMIC_TLV_BUILD)/native.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$(DYNAMIC_TLV_BUILD)/native.status"; \
	  if "$(DYNAMIC_TLV_RUNTIME)" launch "$(DYNAMIC_TLV_MAIN)" "$(DYNAMIC_TLV_PROVIDER)" \
	    >"$(DYNAMIC_TLV_BUILD)/guest.stdout" 2>"$(DYNAMIC_TLV_BUILD)/guest.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$(DYNAMIC_TLV_BUILD)/guest.status"; \
	  cmp "$(DYNAMIC_TLV_BUILD)/native.stdout" "$(DYNAMIC_TLV_BUILD)/guest.stdout"; \
	  cmp "$(DYNAMIC_TLV_BUILD)/native.stderr" "$(DYNAMIC_TLV_BUILD)/guest.stderr"; \
	  cmp "$(DYNAMIC_TLV_BUILD)/native.status" "$(DYNAMIC_TLV_BUILD)/guest.status"; \
	  test "$$rc" -eq 0; \
	  printf 'dynamic dylib TLS template, per-thread isolation, and dlclose match native\n'

THREAD_SERVICES_RUNTIME ?= $(APP_VERIFY_RUNTIME)
THREAD_SERVICES_CASES ?= pthread8 condvar tsd async group timer mainq
THREAD_SERVICES_TIMEOUT ?= 1200
.PHONY: verify-thread-services
verify-thread-services:
	@test -x "$(THREAD_SERVICES_RUNTIME)" || { echo "Set THREAD_SERVICES_RUNTIME to the built C++ launcher" >&2; exit 2; }
	@set -eu; \
	  out=$$(mktemp -d /tmp/arm64-metal-thread-services.XXXXXX); \
	  printf 'thread-services receipts: %s\n' "$$out"; \
	  xcrun clang -arch arm64 -Wall -Wextra -Werror -fblocks \
	    fixtures/handoff/thread-services.c -o "$$out/thread-services"; \
	  for mode in $(THREAD_SERVICES_CASES); do \
	    if /usr/bin/timeout "$(THREAD_SERVICES_TIMEOUT)" "$$out/thread-services" "$$mode" \
	      >"$$out/$$mode.native.stdout" 2>"$$out/$$mode.native.stderr"; then rc=0; else rc=$$?; fi; \
	    printf '%s\n' "$$rc" >"$$out/$$mode.native.status"; \
	    if /usr/bin/timeout "$(THREAD_SERVICES_TIMEOUT)" env \
	      ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 \
	      ARM64_METAL_CACHE_BOOTSTRAP=1 ARM64_METAL_CACHE_INITIALIZERS=1 \
	      "$(THREAD_SERVICES_RUNTIME)" launch "$$out/thread-services" "$$mode" \
	      >"$$out/$$mode.guest.stdout" 2>"$$out/$$mode.guest.stderr"; then rc=0; else rc=$$?; fi; \
	    printf '%s\n' "$$rc" >"$$out/$$mode.guest.status"; \
	    cmp "$$out/$$mode.native.stdout" "$$out/$$mode.guest.stdout"; \
	    cmp "$$out/$$mode.native.stderr" "$$out/$$mode.guest.stderr"; \
	    cmp "$$out/$$mode.native.status" "$$out/$$mode.guest.status"; \
	    test "$$(cat "$$out/$$mode.native.status")" = 0; \
	    printf '%s matches native (status 0)\n' "$$mode"; \
	  done

KEVENT_QOS_RUNTIME ?= $(APP_VERIFY_RUNTIME)
KEVENT_QOS_TIMEOUT ?= 1200
.PHONY: verify-kevent-qos
verify-kevent-qos:
	@test -x "$(KEVENT_QOS_RUNTIME)" || { echo "Set KEVENT_QOS_RUNTIME to the built C++ launcher" >&2; exit 2; }
	@set -eu; \
	  out=$$(mktemp -d /tmp/arm64-metal-kevent-qos.XXXXXX); \
	  printf 'kevent_qos receipts: %s\n' "$$out"; \
	  xcrun clang -arch arm64 -ffreestanding -fno-builtin -fno-stack-protector \
	    -nostdlib -Wl,-e,_start -lSystem -Wall -Wextra -Werror \
	    fixtures/handoff/kevent-qos-two-events.c -o "$$out/kevent-qos-two-events"; \
	  if /usr/bin/timeout "$(KEVENT_QOS_TIMEOUT)" "$$out/kevent-qos-two-events" \
	    >"$$out/native.stdout" 2>"$$out/native.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$$out/native.status"; \
	  if /usr/bin/timeout "$(KEVENT_QOS_TIMEOUT)" \
	    "$(KEVENT_QOS_RUNTIME)" launch "$$out/kevent-qos-two-events" \
	    >"$$out/guest.stdout" 2>"$$out/guest.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$$out/guest.status"; \
	  cmp "$$out/native.stdout" "$$out/guest.stdout"; \
	  cmp "$$out/native.stderr" "$$out/guest.stderr"; \
	  cmp "$$out/native.status" "$$out/guest.status"; \
	  test "$$(cat "$$out/native.status")" = 0; \
	  printf 'Two kevent_qos events and 72-byte stride match native (status 0)\n'

WORKLOOP_RUNTIME ?= $(APP_VERIFY_RUNTIME)
WORKLOOP_TIMEOUT ?= 20
.PHONY: verify-workloop-debounce
verify-workloop-debounce:
	@test -x "$(WORKLOOP_RUNTIME)" || { echo "Set WORKLOOP_RUNTIME to the built C++ launcher" >&2; exit 2; }
	@set -eu; \
	  out=$$(mktemp -d /tmp/arm64-metal-workloop-debounce.XXXXXX); \
	  printf 'workloop-debounce receipts: %s\n' "$$out"; \
	  xcrun clang -arch arm64 -Wall -Wextra -Werror \
	    fixtures/handoff/kevent-workloop-debounce.c -o "$$out/workloop-debounce"; \
	  if /usr/bin/timeout "$(WORKLOOP_TIMEOUT)" "$$out/workloop-debounce" \
	    >"$$out/native.stdout" 2>"$$out/native.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$$out/native.status"; \
	  if /usr/bin/timeout "$(WORKLOOP_TIMEOUT)" "$(WORKLOOP_RUNTIME)" launch \
	    "$$out/workloop-debounce" >"$$out/guest.stdout" 2>"$$out/guest.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$$out/guest.status"; \
	  cmp "$$out/native.stdout" "$$out/guest.stdout"; \
	  cmp "$$out/native.stderr" "$$out/guest.stderr"; \
	  cmp "$$out/native.status" "$$out/guest.status"; \
	  test "$$(cat "$$out/native.status")" = 0; \
	  printf 'Workloop QoS and debounce matrix matches native (status 0)\n'

.PHONY: verify-workloop-delivery
verify-workloop-delivery:
	@test -x "$(WORKLOOP_RUNTIME)" || { echo "Set WORKLOOP_RUNTIME to the built C++ launcher" >&2; exit 2; }
	@set -eu; \
	  out=$$(mktemp -d /tmp/arm64-metal-workloop-delivery.XXXXXX); \
	  printf 'workloop-delivery receipts: %s\n' "$$out"; \
	  xcrun clang -arch arm64 -march=armv8.3-a -Wall -Wextra -Werror -fblocks \
	    fixtures/handoff/kevent-workloop-delivery.c -o "$$out/workloop-delivery"; \
	  if /usr/bin/timeout "$(WORKLOOP_TIMEOUT)" "$(WORKLOOP_RUNTIME)" launch \
	    "$$out/workloop-delivery" >"$$out/guest.stdout" 2>"$$out/guest.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$$out/guest.status"; \
	  cat "$$out/guest.stdout"; cat "$$out/guest.stderr"; \
	  test "$$rc" = 0; \
	  test "$$(cat "$$out/guest.stdout")" = 'workloop delivery and owner return ok'; \
	  printf 'Dynamic workloop worker delivery and return observed (status 0)\n'

.PHONY: verify-workloop-missing-trampoline
verify-workloop-missing-trampoline:
	@test -x "$(WORKLOOP_RUNTIME)" || { echo "Set WORKLOOP_RUNTIME to the built C++ launcher" >&2; exit 2; }
	@set -eu; \
	  out=$$(mktemp -d /tmp/arm64-metal-workloop-boundary.XXXXXX); \
	  printf 'workloop-boundary receipts: %s\n' "$$out"; \
	  xcrun clang -arch arm64 -Wall -Wextra -Werror \
	    fixtures/handoff/kevent-workloop-missing-trampoline.c -o "$$out/workloop-boundary"; \
	  if /usr/bin/timeout "$(WORKLOOP_TIMEOUT)" "$(WORKLOOP_RUNTIME)" launch \
	    "$$out/workloop-boundary" >"$$out/guest.stdout" 2>"$$out/guest.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$$out/guest.status"; \
	  cat "$$out/guest.stdout"; cat "$$out/guest.stderr"; \
	  test "$$rc" = 0; \
	  test "$$(cat "$$out/guest.stdout")" = \
	    'matching workloop attach rejected without guest worker trampoline (ENOTSUP)'; \
	  printf 'Missing-trampoline attach rejected without fake success (status 0)\n'

DYLD_IMAGE_ORACLE := $(BUILD)/dyld-image-enumeration-oracle
DYLD_IMAGE_DEPENDENCY := $(BUILD)/libdyld-image-dependency.dylib
DYLD_IMAGE_ROOT := $(BUILD)/libdyld-image-root.dylib
.PHONY: verify-dyld-image-enumeration-native
$(DYLD_IMAGE_DEPENDENCY): fixtures/handoff/dyld-image-dependency.c | $(BUILD)
	xcrun clang -arch arm64 -dynamiclib -install_name @rpath/libdyld-image-dependency.dylib "$<" -o "$@"

$(DYLD_IMAGE_ROOT): fixtures/handoff/dyld-image-root.c $(DYLD_IMAGE_DEPENDENCY) | $(BUILD)
	xcrun clang -arch arm64 -dynamiclib -L"$(BUILD)" -Wl,-rpath,@loader_path -install_name @rpath/libdyld-image-root.dylib "$<" -ldyld-image-dependency -o "$@"

$(DYLD_IMAGE_ORACLE): fixtures/handoff/dyld-image-enumeration-oracle.c | $(BUILD)
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -pedantic "$<" -o "$@"

verify-dyld-image-enumeration-native: $(DYLD_IMAGE_ORACLE) $(DYLD_IMAGE_ROOT)
	"$(DYLD_IMAGE_ORACLE)" "$(BUILD)/missing-image.dylib" "$(DYLD_IMAGE_ROOT)" "$(DYLD_IMAGE_DEPENDENCY)"

DYLD_IMAGE_SLIDE_ORACLE := $(BUILD)/dyld-image-slide-oracle
.PHONY: verify-dyld-image-slide-native
$(DYLD_IMAGE_SLIDE_ORACLE): fixtures/handoff/dyld-image-slide-oracle.c | $(BUILD)
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -pedantic "$<" -o "$@"

verify-dyld-image-slide-native: $(DYLD_IMAGE_SLIDE_ORACLE)
	"$(DYLD_IMAGE_SLIDE_ORACLE)"

DYLD_ADD_IMAGE_DEPENDENCY := $(BUILD)/libdyld-add-image-dep.dylib
DYLD_ADD_IMAGE_ROOT := $(BUILD)/libdyld-add-image-new.dylib
DYLD_ADD_IMAGE_NESTED := $(BUILD)/libdyld-add-image-nested.dylib
DYLD_ADD_IMAGE_ORACLE := $(BUILD)/dyld-add-image-probe
DYLD_ADD_IMAGE_REENTRANT := $(BUILD)/dyld-add-image-reentrant
.PHONY: verify-dyld-add-image-native
$(DYLD_ADD_IMAGE_DEPENDENCY): fixtures/handoff/dep.c | $(BUILD)
	xcrun clang -arch arm64 -dynamiclib -Wl,-install_name,@rpath/libdyld-add-image-dep.dylib "$<" -o "$@"

$(DYLD_ADD_IMAGE_ROOT): fixtures/handoff/new_image.c $(DYLD_ADD_IMAGE_DEPENDENCY) | $(BUILD)
	xcrun clang -arch arm64 -dynamiclib -L"$(BUILD)" -Wl,-rpath,@loader_path -Wl,-install_name,@rpath/libdyld-add-image-new.dylib "$<" -ldyld-add-image-dep -o "$@"

$(DYLD_ADD_IMAGE_NESTED): fixtures/handoff/dyld-add-image-nested.c | $(BUILD)
	xcrun clang -arch arm64 -dynamiclib -Wl,-install_name,@rpath/libdyld-add-image-nested.dylib "$<" -o "$@"

$(DYLD_ADD_IMAGE_ORACLE): fixtures/handoff/probe.c | $(BUILD)
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -ldl -o "$@"

$(DYLD_ADD_IMAGE_REENTRANT): fixtures/handoff/dyld-add-image-reentrant.c | $(BUILD)
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -ldl -o "$@"

verify-dyld-add-image-native: $(DYLD_ADD_IMAGE_ORACLE) $(DYLD_ADD_IMAGE_REENTRANT) $(DYLD_ADD_IMAGE_ROOT) $(DYLD_ADD_IMAGE_NESTED)
	"$(DYLD_ADD_IMAGE_ORACLE)" "$(DYLD_ADD_IMAGE_ROOT)" "$(BUILD)/missing-dyld-add-image.dylib"
	"$(DYLD_ADD_IMAGE_REENTRANT)" "$(DYLD_ADD_IMAGE_ROOT)" "$(BUILD)/missing-dyld-add-image.dylib" "$(DYLD_ADD_IMAGE_NESTED)"
JAVA_VERIFY_RUNTIME ?= $(BUILD)/cpp/arm64-metal
JAVA_VERIFY_RESULTS ?= $(BUILD)/java-verification

verify-java:
	@test -n "$(JDK_HOME)" || { echo "Set JDK_HOME to the Azul JDK being qualified" >&2; exit 2; }
	./verify-java.sh "$(JDK_HOME)" "$(JAVA_VERIFY_RUNTIME)" "$(JAVA_VERIFY_RESULTS)"

REALPATH_BUILD := $(BUILD)/realpath-contract
REALPATH_FIXTURE := $(REALPATH_BUILD)/realpath-contract
REALPATH_INVALID_FIXTURE := $(REALPATH_BUILD)/realpath-invalid-output
REALPATH_RESULTS ?= $(REALPATH_BUILD)/results
REALPATH_GPULOCK ?= $(ARM64_METAL_GPULOCK)
.PHONY: verify-realpath
$(REALPATH_BUILD):
	mkdir -p "$@/dir"
	printf 'fixture\n' > "$@/dir/file"
	ln -sf dir/file "$@/link"

$(REALPATH_FIXTURE): fixtures/handoff/realpath-contract.c | $(REALPATH_BUILD)
	xcrun clang -arch arm64 -Wall -Wextra -Werror -pedantic "$<" -o "$@"

$(REALPATH_INVALID_FIXTURE): fixtures/handoff/realpath-contract.c | $(REALPATH_BUILD)
	xcrun clang -arch arm64 -Wall -Wextra -Werror -pedantic -DREALPATH_GUEST_INVALID_OUTPUT "$<" -o "$@"

verify-realpath: $(REALPATH_FIXTURE) $(REALPATH_INVALID_FIXTURE)
	@test -n "$(REALPATH_GPULOCK)" || { echo "Set REALPATH_GPULOCK to the shared GPU lock wrapper" >&2; exit 2; }
	@test -x "$(RUNTIME)" || { echo "Set RUNTIME to the C++ launcher" >&2; exit 2; }
	@set -eu; results="$(abspath $(REALPATH_RESULTS))"; mkdir -p "$$results"; \
	  cd "$(abspath $(REALPATH_BUILD))"; \
	  if ./realpath-contract link missing >"$$results/native.stdout" 2>"$$results/native.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$$results/native.status"; \
	  if env GPULOCK_SLOTS=2 "$(REALPATH_GPULOCK)" "$(abspath $(RUNTIME))" launch ./realpath-contract link missing >"$$results/guest.stdout" 2>"$$results/guest.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$$results/guest.status"; \
	  cmp "$$results/native.status" "$$results/guest.status"; \
	  cmp "$$results/native.stdout" "$$results/guest.stdout"; \
	  cmp "$$results/native.stderr" "$$results/guest.stderr"; \
	  test "$$rc" -eq 0; \
	  if env GPULOCK_SLOTS=2 "$(REALPATH_GPULOCK)" "$(abspath $(RUNTIME))" launch ./realpath-invalid-output link missing >"$$results/invalid-output.stdout" 2>"$$results/invalid-output.stderr"; then invalid_rc=0; else invalid_rc=$$?; fi; \
	  test "$$invalid_rc" -eq 0; \
	  test "$$(cat "$$results/invalid-output.stdout")" = 'invalid_output_errno=14'; \
	  printf 'realpath symlink, allocation, caller buffer, and ENOENT match native\n'

verify-cache: $(RUNTIME) $(CACHE_CHECK) $(CACHE_ZLIB) $(CACHE_CONFORMANCE)
	@set -e; for library in libbz2.1.0.dylib libz.1.dylib liblzma.5.dylib; do \
		$(RUNTIME) inspect-cache-rebases "/usr/lib/$$library" >"$(BUILD)/$$library.rebases"; \
		$(CACHE_CHECK) "/usr/lib/$$library" <"$(BUILD)/$$library.rebases"; \
	done
	@$(CACHE_ZLIB) >"$(BUILD)/cache-zlib.native" 2>"$(BUILD)/cache-zlib.native.err"
	@ARM64_METAL_SHARED_CACHE=1 $(RUNTIME) launch $(CACHE_ZLIB) >"$(BUILD)/cache-zlib.gpu" 2>"$(BUILD)/cache-zlib.gpu.err"
	@cmp "$(BUILD)/cache-zlib.native" "$(BUILD)/cache-zlib.gpu"
	@cmp "$(BUILD)/cache-zlib.native.err" "$(BUILD)/cache-zlib.gpu.err"
	@echo "System libz version, relocated error table, checksum and decompression match native"
	@ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 $(RUNTIME) launch $(CACHE_ZLIB) >"$(BUILD)/cache-zlib.demand" 2>"$(BUILD)/cache-zlib.demand.err"
	@cmp "$(BUILD)/cache-zlib.native" "$(BUILD)/cache-zlib.demand"
	@cmp "$(BUILD)/cache-zlib.native.err" "$(BUILD)/cache-zlib.demand.err"
	@echo "Demand-mapped libz decompression and relocation match native"
	@$(CACHE_CONFORMANCE) >"$(BUILD)/cache-conformance.native" 2>"$(BUILD)/cache-conformance.native.err"
	@ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 ARM64_METAL_CACHE_BOOTSTRAP=1 ARM64_METAL_CACHE_INITIALIZERS=1 $(RUNTIME) launch $(CACHE_CONFORMANCE) >"$(BUILD)/cache-conformance.gpu" 2>"$(BUILD)/cache-conformance.gpu.err"
	@cmp "$(BUILD)/cache-conformance.native" "$(BUILD)/cache-conformance.gpu"
	@cmp "$(BUILD)/cache-conformance.native.err" "$(BUILD)/cache-conformance.gpu.err"
	@echo "On-disk protocol conformance lookup matches native"
	@awk 'BEGIN {for (i=0; i<1000; ++i) printf "compile record %04d module-%02d output\n", i, i%31}' >"$(BUILD)/gzip-input"
	@/usr/bin/gzip -nc "$(BUILD)/gzip-input" >"$(BUILD)/gzip-input.gz"
	@ARM64_METAL_SHARED_CACHE=1 $(RUNTIME) launch /usr/bin/gzip -nc "$(BUILD)/gzip-input" >"$(BUILD)/gzip-encode.gpu" 2>"$(BUILD)/gzip-encode.gpu.err"
	@cmp "$(BUILD)/gzip-input.gz" "$(BUILD)/gzip-encode.gpu"
	@test ! -s "$(BUILD)/gzip-encode.gpu.err"
	@echo "Unmodified /usr/bin/gzip compression of 37,000 bytes matches native"
	@/usr/bin/gzip -dc "$(BUILD)/gzip-input.gz" >"$(BUILD)/gzip.native" 2>"$(BUILD)/gzip.native.err"
	@ARM64_METAL_SHARED_CACHE=1 $(RUNTIME) launch /usr/bin/gzip -dc "$(BUILD)/gzip-input.gz" >"$(BUILD)/gzip.gpu" 2>"$(BUILD)/gzip.gpu.err"
	@cmp "$(BUILD)/gzip-input" "$(BUILD)/gzip.native"
	@cmp "$(BUILD)/gzip.native" "$(BUILD)/gzip.gpu"
	@cmp "$(BUILD)/gzip.native.err" "$(BUILD)/gzip.gpu.err"
	@echo "Unmodified /usr/bin/gzip decompression of 37,000 bytes matches native"
	@printf 'invalid gzip fixture\n' >"$(BUILD)/gzip-invalid.gz"
	@/usr/bin/gzip -dc "$(BUILD)/gzip-invalid.gz" >"$(BUILD)/gzip-invalid.native" 2>"$(BUILD)/gzip-invalid.native.err"; test $$? -eq 1
	@ARM64_METAL_SHARED_CACHE=1 $(RUNTIME) launch /usr/bin/gzip -dc "$(BUILD)/gzip-invalid.gz" >"$(BUILD)/gzip-invalid.gpu" 2>"$(BUILD)/gzip-invalid.gpu.err"; test $$? -eq 1
	@test ! -s "$(BUILD)/gzip-invalid.native"
	@cmp "$(BUILD)/gzip-invalid.native" "$(BUILD)/gzip-invalid.gpu"
	@cmp "$(BUILD)/gzip-invalid.native.err" "$(BUILD)/gzip-invalid.gpu.err"
	@echo "Unmodified /usr/bin/gzip malformed-input rejection matches native"

	@/usr/bin/bzip2 -c "$(BUILD)/gzip-input" >"$(BUILD)/bzip2-input.bz2"
	@/usr/bin/xattr -w com.example.metal-bzip metadata-check "$(BUILD)/bzip2-input.bz2"
	@rm -f "$(BUILD)/bzip2.native" "$(BUILD)/bzip2.gpu"
	@/usr/bin/bzip2 -dc "$(BUILD)/bzip2-input.bz2" >"$(BUILD)/bzip2.native" 2>"$(BUILD)/bzip2.native.err"
	@ARM64_METAL_SHARED_CACHE=1 $(RUNTIME) launch /usr/bin/bzip2 -dc "$(BUILD)/bzip2-input.bz2" >"$(BUILD)/bzip2.gpu" 2>"$(BUILD)/bzip2.gpu.err"
	@cmp "$(BUILD)/gzip-input" "$(BUILD)/bzip2.native"
	@cmp "$(BUILD)/bzip2.native" "$(BUILD)/bzip2.gpu"
	@cmp "$(BUILD)/bzip2.native.err" "$(BUILD)/bzip2.gpu.err"
	@test "$$(/usr/bin/xattr -p com.example.metal-bzip "$(BUILD)/bzip2.native")" = metadata-check
	@test "$$(/usr/bin/xattr -p com.example.metal-bzip "$(BUILD)/bzip2.gpu")" = metadata-check
	@echo "Unmodified /usr/bin/bzip2 decompression of 37,000 bytes and copied xattr match native"

SYNCHRONOUS_FAULT_ORACLE := $(BUILD)/synchronous-fault-oracle
SYNCHRONOUS_FAULT_GUEST := $(BUILD)/synchronous-fault-guest
.PHONY: verify-synchronous-fault-oracle sync-fault-guest
$(SYNCHRONOUS_FAULT_ORACLE): fixtures/handoff/synchronous-fault-oracle.c | $(BUILD)
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -pedantic "$<" -o "$@"

$(SYNCHRONOUS_FAULT_GUEST): fixtures/handoff/synchronous-fault-oracle.c | $(BUILD)
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -pedantic "$<" -o "$@"

sync-fault-guest: $(SYNCHRONOUS_FAULT_GUEST)

verify-synchronous-fault-oracle: $(SYNCHRONOUS_FAULT_ORACLE)
	$(SYNCHRONOUS_FAULT_ORACLE)

$(CACHE_ZLIB): cache-zlib.c | $(BUILD)
	xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 "$<" -lz -o "$@"

$(CACHE_CONFORMANCE): cache-dyld-conformance.c | $(BUILD)
	xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 "$<" -o "$@"

$(CACHE_CHECK): cache-rebase-check.c | $(BUILD)
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

bundle: verify bundle-cpp
	@rm -rf "$(BUILD)/arm64-metal-macos"
	@mkdir -p "$(BUILD)/arm64-metal-macos"
	@cp -R "$(CPP_PACKAGE)/." "$(BUILD)/arm64-metal-macos/"
	@cp "$(GUEST)" "$(CONSOLE)" "$(STREAMS)" "$(INDIRECT)" "$(REBASED)" "$(LINKED)" "$(WRAPPER)" "$(SUPPORT)" "$(SIGNAL)" "$(THREADS)" example.json "$(BUILD)/arm64-metal-macos/"
	@cd "$(BUILD)" && tar -czf arm64-metal-macos.tar.gz arm64-metal-macos

# Keep the C++ package name and archive path for existing callers.
CPP_PACKAGE := $(BUILD)/arm64-metal-cpp-macos

.PHONY: cpp-release bundle-cpp
cpp-release:
	$(MAKE) -C cpp app interpreter-metallib METAL_CPP="$(METAL_CPP)"

bundle-cpp: cpp-release
	@test -x "$(CPP_RUNTIME)" || { echo 'Set CPP_RUNTIME to the verified C++ launcher' >&2; exit 1; }
	@test -f "$(CPP_INTERPRETER_METALLIB)" || { echo 'Missing AOT interpreter library; run make cpp-release' >&2; exit 1; }
	@mkdir -p "$(CPP_PACKAGE)"
	@cp "$(CPP_RUNTIME)" "$(CPP_PACKAGE)/arm64-metal"
	@mkdir -p "$(CPP_PACKAGE)/resources/interpreter"
	@cp "$(CPP_INTERPRETER_METALLIB)" "$(CPP_PACKAGE)/resources/interpreter/runtime-interpreter.metallib"
	@printf '%s\n' 'AOT interpreter library required' >"$(CPP_PACKAGE)/resources/interpreter/AOT_REQUIRED"
	@cp README.md LEDGER.md HANDOFF.md "$(CPP_PACKAGE)/"
	@cd "$(BUILD)" && tar -czf arm64-metal-cpp-macos.tar.gz arm64-metal-cpp-macos

$(BUILD):
	mkdir -p "$@"

$(GUEST): workload.c | $(BUILD)
	xcrun clang -arch arm64 -O2 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -dynamiclib "$<" -o "$@"

$(CPP_RUNTIME): cpp-release

$(CONSOLE): console.c score.c | $(BUILD)
	xcrun clang -arch arm64 -O2 $^ -o "$@"

$(STREAMS): streams.c | $(BUILD)
	xcrun clang -arch arm64 -O2 "$<" -o "$@"

$(INDIRECT): indirect.c targets.c | $(BUILD)
	xcrun clang -arch arm64 -O2 $^ -o "$@"

$(REBASED): rebased.c targets.c | $(BUILD)
	xcrun clang -arch arm64 -O2 $^ -o "$@"

$(DATA_AUTH): data-auth.s | $(BUILD)
	xcrun clang -arch arm64e "$<" -o "$@"

$(UNSIGNED_CALL): unsigned-call.s | $(BUILD)
	xcrun clang -arch arm64e "$<" -o "$@"

$(SUPPORT): linked-support.c | $(BUILD)
	xcrun clang -arch arm64 -O2 -dynamiclib "$<" -Wl,-install_name,@loader_path/libsupport.dylib -o "$@"

$(WRAPPER): linked-wrapper.c $(SUPPORT) Makefile | $(BUILD)
	xcrun clang -arch arm64 -O2 -dynamiclib "$<" -Wl,-reexport_library,$(SUPPORT) -Wl,-install_name,@rpath/libbridge.dylib -o "$@"

$(LINKED): linked-main.c $(WRAPPER) | $(BUILD)
	xcrun clang -arch arm64 -O2 $^ -Wl,-rpath,@executable_path -o "$@"

$(REGION): region.s | $(BUILD)
	xcrun clang -arch arm64 "$<" -o "$@"

$(PIPE): guest-pipe.c | $(BUILD)
	xcrun clang -arch arm64 -O2 "$<" -o "$@"

$(SIGNAL): guest-signal.c | $(BUILD)
	xcrun clang -arch arm64 -O2 "$<" -o "$@"

$(VECTOR): vector-memory.s | $(BUILD)
	xcrun clang -arch arm64 "$<" -o "$@"

$(HEAP): heap.c | $(BUILD)
	xcrun clang -arch arm64 -O2 -fno-unroll-loops "$<" -o "$@"

$(THREADS): guest-threads.c | $(BUILD)
	xcrun clang -arch arm64 -O2 "$<" -o "$@"

$(MEMORY): guest-memory.c | $(BUILD)
	xcrun clang -arch arm64 -O2 -fno-builtin "$<" -o "$@"

SIGSUSPEND_RUNTIME ?= $(APP_VERIFY_RUNTIME)
SIGSUSPEND_TIMEOUT ?= 120
.PHONY: verify-sigsuspend-cooperative
verify-sigsuspend-cooperative:
	@test -x "$(SIGSUSPEND_RUNTIME)" || { echo "Set SIGSUSPEND_RUNTIME to the built C++ launcher" >&2; exit 2; }
	@set -eu; \
	  out=$$(mktemp -d /tmp/arm64-metal-sigsuspend.XXXXXX); \
	  printf 'sigsuspend receipts: %s\n' "$$out"; \
	  xcrun clang -arch arm64 -Wall -Wextra -Werror -pthread \
	    fixtures/handoff/sigsuspend-cooperative.c -o "$$out/sigsuspend-cooperative"; \
	  if /usr/bin/timeout "$(SIGSUSPEND_TIMEOUT)" "$$out/sigsuspend-cooperative" \
	    >"$$out/native.stdout" 2>"$$out/native.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$$out/native.status"; \
	  if /usr/bin/timeout "$(SIGSUSPEND_TIMEOUT)" "$(SIGSUSPEND_RUNTIME)" launch \
	    "$$out/sigsuspend-cooperative" >"$$out/guest.stdout" 2>"$$out/guest.stderr"; then rc=0; else rc=$$?; fi; \
	  printf '%s\n' "$$rc" >"$$out/guest.status"; \
	  cmp "$$out/native.stdout" "$$out/guest.stdout"; \
	  cmp "$$out/native.stderr" "$$out/guest.stderr"; \
	  cmp "$$out/native.status" "$$out/guest.status"; \
	  test "$$(cat "$$out/native.status")" = 0; \
	  printf 'sigsuspend cooperative matches native (status 0)\n'
