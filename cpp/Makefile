CXX ?= clang++
CPPFLAGS ?= -I.
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -pedantic
BUILD_DIR ?= ../.build/cpp
GPU_LOCK ?=
.DEFAULT_GOAL := all

# This target checks stable C++17 components independently of Metal framework
# linking and the still-integrating whole-process host/runner.
COMPONENT_SOURCES := \
	cache_residency.cpp \
	darwin_bridge.cpp \
	function_runner.cpp \
	guest_code_policy.cpp \
	guest_format.cpp \
	guest_mach_models.cpp \
	guest_signal_runtime.cpp \
	guest_scheduler.cpp \
	guest_state.cpp \
	guest_strings.cpp \
	guest_survey.cpp \
	guest_tlv.cpp \
	guest_vector_crypto.cpp \
	image_loader.cpp \
	macho.cpp \
	main.cpp \
	process_decoder.cpp \
	process_msl.cpp \
	shared_cache.cpp \
	translate_core.cpp
COMPONENT_OBJECTS := $(addprefix $(BUILD_DIR)/,$(COMPONENT_SOURCES:.cpp=.o))
COMPONENT_DEPFILES := $(COMPONENT_OBJECTS:.o=.d)
-include $(COMPONENT_DEPFILES)

INTERPRETER_ASSETS_TEST := $(BUILD_DIR)/interpreter-assets-selection-test

$(INTERPRETER_ASSETS_TEST): ../fixtures/handoff/interpreter-assets-selection.cpp interpreter_assets.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) "$<" -o "$@"

.PHONY: verify-interpreter-assets
verify-interpreter-assets: $(INTERPRETER_ASSETS_TEST)
	"$(INTERPRETER_ASSETS_TEST)"

.PHONY: all components app verify-interpreter verify-interpreter-demand-cache verify-interpreter-stack-guard-retry verify-interpreter-mprotect-cross-page-retry verify-interpreter-pthread-runtime-owned-guard verify-interpreter-literal-load-native verify-interpreter-ret-chain-native verify-interpreter-ret-chain verify-interpreter-ldumin-ldumax-native verify-interpreter-ldumin-ldumax verify-interpreter-cmeq-zero-native verify-interpreter-cmeq-zero verify-interpreter-vector-extreme-native verify-interpreter-vector-extreme verify-interpreter-vector-memory-native verify-interpreter-vector-memory verify-interpreter-assets verify-guest-format verify-guest-image-registry verify-guest-exit-registry verify-dyld-finalizer-metadata-native verify-memcpy-chk verify-memset-chk verify-kdebug-trace64-native verify-semaphore-native verify-semaphore-scheduler verify-signal-thread-frames verify-threaded-raise-signals-native verify-jit-code-cache-native verify-pthread-kill-native verify-open-nocancel-native verify-open-append-native verify-proc-info-native verify-af-unix-socket-native verify-af-unix-connect-native verify-af-unix-connect-efault-native verify-af-unix-datagram-native verify-af-unix-datagram-blocking-native verify-af-unix-close-blocked-native verify-vector-crypto verify-mach-vm-overwrite-native verify-mach-vm-sparse-reservation-native verify-mach-vm-region-native verify-mach-vm-named-services verify-adc-sbc-carry-native verify-scalar-fp-native verify-scalar-fp-interpreter verify-scalar-fp-compare-native verify-scalar-fp-compare-interpreter diagnose-scalar-fp-compare-translated verify-authenticated-return verify-authenticated-return-native verify-authenticated-return-cpu verify-authenticated-return-interpreter verify-addp-2d-native verify-xtn-native verify-umaxp-native verify-guest-heap-allocate-at verify-cache-backing-leases verify-dlsym-arm64e-pac-native verify-dlsym-arm64e-pac clean
all: components

components: $(COMPONENT_OBJECTS)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF "$(@:.o=.d)" -c "$<" -o "$@"

# Full runtime link requires the process launcher and an installed metal-cpp
# distribution (METAL_CPP points at its include root). Do not silently link a
# partial app: once those interfaces are available, app builds every module.
METAL_CPP ?= ../metal-cpp
APP_SOURCES := \
	guest_fork.cpp \
	cache_residency.cpp \
	guest_signal_runtime.cpp \
	guest_scheduler.cpp \
	guest_code_policy.cpp \
	guest_format.cpp \
	guest_mach_models.cpp \
	guest_state.cpp \
	guest_strings.cpp \
	guest_survey.cpp \
	guest_tlv.cpp \
	guest_vector_crypto.cpp \
	gpu_interpreter.cpp \
	darwin_bridge.cpp \
	image_loader.cpp \
	macho.cpp \
	metal_backend.cpp \
	process_decoder.cpp \
	process_host.cpp \
	process_msl.cpp \
	process_runner.cpp \
	raw_proc_syscalls.cpp \
	cache_local_symbols.cpp \
	shared_cache.cpp \
	translate_core.cpp \
	function_runner.cpp \
	main.cpp
APP_HEADERS := $(wildcard *.h *.hpp *.inc)
APP := $(BUILD_DIR)/arm64-metal
APP_OBJECT_DIR := $(BUILD_DIR)/app-objects
APP_OBJECTS := $(addprefix $(APP_OBJECT_DIR)/,$(APP_SOURCES:.cpp=.o))
APP_DEPFILES := $(APP_OBJECTS:.o=.d)
APP_CONFIG := $(APP_OBJECT_DIR)/build-config
APP_LINK_LIBS := -framework Foundation -framework Metal -framework Security
OPEN_NOCANCEL_NATIVE := $(BUILD_DIR)/open-nocancel
OPEN_APPEND_NATIVE := $(BUILD_DIR)/open-append-native
PROC_INFO_NATIVE := $(BUILD_DIR)/proc-info-self
AF_UNIX_SOCKET_NATIVE := $(BUILD_DIR)/af-unix-socket
AF_UNIX_CONNECT_NATIVE := $(BUILD_DIR)/af-unix-connect
AF_UNIX_DATAGRAM_NATIVE := $(BUILD_DIR)/af-unix-datagram
AF_UNIX_CONNECT_EFAULT_NATIVE := $(BUILD_DIR)/af-unix-connect-efault
AF_UNIX_DGRAM_BLOCKING_NATIVE := $(BUILD_DIR)/af-unix-datagram-blocking
AF_UNIX_CLOSE_BLOCKED_NATIVE := $(BUILD_DIR)/af-unix-close-blocked
SEMAPHORE_NATIVE := $(BUILD_DIR)/mach-semaphore-lifecycle
KDEBUG_TRACE64_NATIVE := $(BUILD_DIR)/kdebug-trace64-disabled
SEMAPHORE_SCHEDULER := $(BUILD_DIR)/guest-semaphore-scheduler
SIGNAL_THREAD_FRAMES := $(BUILD_DIR)/guest-signal-thread-frames
THREADED_RAISE_SIGNALS_NATIVE := $(BUILD_DIR)/threaded-raise-signals
PTHREAD_KILL_NATIVE := $(BUILD_DIR)/pthread-kill-targeted
INTERPRETER_NATIVE := $(BUILD_DIR)/interpreter-native
INTERPRETER_TYPED_NATIVE := $(BUILD_DIR)/interpreter-typed-operations
AUTH_RETURN_NATIVE := $(BUILD_DIR)/authenticated-return-native
AUTH_RETURN_CPU := $(BUILD_DIR)/process-authenticated-return
AUTH_RETURN_MAIN := $(BUILD_DIR)/authenticated-return-main
AUTH_RETURN_RETAB_MAIN := $(BUILD_DIR)/authenticated-return-retab-main
AUTH_RETURN_BAD_LR_MAIN := $(BUILD_DIR)/authenticated-return-bad-lr-main
AUTH_RETURN_BAD_SP_MAIN := $(BUILD_DIR)/authenticated-return-bad-sp-main
AUTH_RETURN_BAD_LR_RETAB_MAIN := $(BUILD_DIR)/authenticated-return-bad-lr-retab-main
AUTH_RETURN_BAD_SP_RETAB_MAIN := $(BUILD_DIR)/authenticated-return-bad-sp-retab-main
AUTH_RETURN_BAD_SP_NESTED_MAIN := $(BUILD_DIR)/authenticated-return-bad-sp-nested-main
RETURN_SENTINEL_MAIN := $(BUILD_DIR)/return-sentinel-main
INTERPRETER_ADDP_2D_MAIN := $(BUILD_DIR)/interpreter-addp-2d-main
ADDP_2D_PROCESS_TEST := $(BUILD_DIR)/addp-2d-process-decode-test
XTN_PROCESS_TEST := $(BUILD_DIR)/xtn-process-decode-test
UMAXP_PROCESS_TEST := $(BUILD_DIR)/umaxp-process-decode-test
INTERPRETER_MOVE_WIDE_NATIVE := $(BUILD_DIR)/interpreter-move-wide-native
INTERPRETER_ADD_SUB_NATIVE := $(BUILD_DIR)/interpreter-add-sub-native
INTERPRETER_ADD_SUB_MAIN := $(BUILD_DIR)/interpreter-add-sub-immediate-main
INTERPRETER_ADD_SUB_FLAGS_NATIVE := $(BUILD_DIR)/interpreter-add-sub-flags-native
INTERPRETER_ADD_SUB_SP_MAIN := $(BUILD_DIR)/interpreter-add-sub-sp-flags-main
INTERPRETER_BCOND_MAIN := $(BUILD_DIR)/interpreter-bcond-nzcv-main
INTERPRETER_CSEL_MAIN := $(BUILD_DIR)/interpreter-csel-family-main
INTERPRETER_ADD_SUB_SHIFTED_MAIN := $(BUILD_DIR)/interpreter-add-sub-shifted-register-main
INTERPRETER_ADD_SUB_EXTENDED_MAIN := $(BUILD_DIR)/interpreter-add-sub-extended-register-main
INTERPRETER_LOGICAL_SHIFTED_MAIN := $(BUILD_DIR)/interpreter-logical-shifted-register-main
INTERPRETER_LOGICAL_VARIABLE_SHIFTS_MAIN := $(BUILD_DIR)/interpreter-logical-variable-shifts-main
INTERPRETER_MULTIPLY_ACCUMULATE_MAIN := $(BUILD_DIR)/interpreter-multiply-accumulate-main
INTERPRETER_BITFIELD_MAIN := $(BUILD_DIR)/interpreter-bitfield-main
INTERPRETER_BITFIELD_NATIVE := $(BUILD_DIR)/interpreter-bitfield-native
INTERPRETER_INTEGER_BITOPS_MAIN := $(BUILD_DIR)/interpreter-integer-bitops-main
INTERPRETER_INTEGER_DIVIDE_MAIN := $(BUILD_DIR)/interpreter-integer-divide-main
INTERPRETER_LITERAL_LOAD_MAIN := $(BUILD_DIR)/interpreter-literal-load-main
INTERPRETER_LDUMIN_LDUMAX_MAIN := $(BUILD_DIR)/interpreter-ldumin-ldumax-main
INTERPRETER_CMEQ_ZERO_MAIN := $(BUILD_DIR)/interpreter-cmeq-zero-main
INTERPRETER_VECTOR_EXTREME_MAIN := $(BUILD_DIR)/interpreter-vector-extreme-main
INTERPRETER_VECTOR_MEMORY_MAIN := $(BUILD_DIR)/interpreter-vector-memory-main
INTERPRETER_VECTOR_MEMORY_UNMAPPED_MAIN := $(BUILD_DIR)/interpreter-vector-memory-unmapped-main
INTERPRETER_LOGICAL_IMMEDIATE_MAIN := $(BUILD_DIR)/interpreter-logical-immediate-main
INTERPRETER_LOGICAL_IMMEDIATE_INVALID_MAIN := $(BUILD_DIR)/interpreter-logical-immediate-invalid-main
INTERPRETER_MINIMAL_MAIN := $(BUILD_DIR)/interpreter-minimal-main
INTERPRETER_MOVN_MOVK_MAIN := $(BUILD_DIR)/interpreter-movn-movk-main
INTERPRETER_BRANCH_MAIN := $(BUILD_DIR)/interpreter-branch-main
INTERPRETER_BRANCH_FALLBACK_MAIN := $(BUILD_DIR)/interpreter-branch-fallback-main
INTERPRETER_BL_MAIN := $(BUILD_DIR)/interpreter-bl-nested-main
INTERPRETER_RET_CHAIN_MAIN := $(BUILD_DIR)/interpreter-ret-chain-bl-main
INTERPRETER_BL_NATIVE := $(BUILD_DIR)/interpreter-bl-nested-native
INTERPRETER_BL_UNPLANNED_MAIN := $(BUILD_DIR)/interpreter-bl-unplanned-main
INTERPRETER_BL_UNPLANNED_NATIVE := $(BUILD_DIR)/interpreter-bl-unplanned-native
INTERPRETER_BRANCH_REGISTER_MAIN := $(BUILD_DIR)/interpreter-branch-register-main
INTERPRETER_BRANCH_REGISTER_NATIVE := $(BUILD_DIR)/interpreter-branch-register-native
INTERPRETER_BRANCH_REGISTER_IMPORT_MAIN := $(BUILD_DIR)/interpreter-branch-register-import-main
INTERPRETER_BRANCH_REGISTER_IMPORT_NATIVE := $(BUILD_DIR)/interpreter-branch-register-import-native
INTERPRETER_TYPED_MEMORY_MAIN := $(BUILD_DIR)/interpreter-typed-memory-main
INTERPRETER_TYPED_MEMORY_RO_MAIN := $(BUILD_DIR)/interpreter-typed-memory-ro-main
INTERPRETER_TYPED_MEMORY_UNMAPPED_MAIN := $(BUILD_DIR)/interpreter-typed-memory-unmapped-main
INTERPRETER_PAIR_MEMORY_MAIN := $(BUILD_DIR)/interpreter-pair-memory-stack-main
INTERPRETER_SCALAR_INDEXED_MEMORY_MAIN := $(BUILD_DIR)/interpreter-scalar-memory-indexed-main
INTERPRETER_DEMAND_CACHE_GENERATOR := $(BUILD_DIR)/interpreter-demand-cache-address
INTERPRETER_DEMAND_CACHE_SOURCE := $(BUILD_DIR)/interpreter/demand-cache-retry.s
INTERPRETER_DEMAND_CACHE_MAIN := $(BUILD_DIR)/interpreter-demand-cache-main
INTERPRETER_DEMAND_CACHE_NATIVE := $(BUILD_DIR)/interpreter-demand-cache-native
INTERPRETER_DEMAND_CACHE_SCALAR_SOURCE := $(BUILD_DIR)/interpreter/demand-cache-retry-scalar.s
INTERPRETER_DEMAND_CACHE_SCALAR_MAIN := $(BUILD_DIR)/interpreter-demand-cache-scalar-main
INTERPRETER_DEMAND_CACHE_SCALAR_NATIVE := $(BUILD_DIR)/interpreter-demand-cache-scalar-native
INTERPRETER_DEMAND_CACHE_INDEXED_SOURCE := $(BUILD_DIR)/interpreter/demand-cache-retry-indexed.s
INTERPRETER_DEMAND_CACHE_INDEXED_MAIN := $(BUILD_DIR)/interpreter-demand-cache-indexed-main
INTERPRETER_DEMAND_CACHE_INDEXED_NATIVE := $(BUILD_DIR)/interpreter-demand-cache-indexed-native
INTERPRETER_DEMAND_CACHE_BYTE_SOURCE := $(BUILD_DIR)/interpreter/demand-cache-retry-byte.s
INTERPRETER_DEMAND_CACHE_BYTE_MAIN := $(BUILD_DIR)/interpreter-demand-cache-byte-main
INTERPRETER_DEMAND_CACHE_BYTE_NATIVE := $(BUILD_DIR)/interpreter-demand-cache-byte-native
INTERPRETER_DEMAND_CACHE_HALF_SOURCE := $(BUILD_DIR)/interpreter/demand-cache-retry-half.s
INTERPRETER_DEMAND_CACHE_HALF_MAIN := $(BUILD_DIR)/interpreter-demand-cache-half-main
INTERPRETER_DEMAND_CACHE_HALF_NATIVE := $(BUILD_DIR)/interpreter-demand-cache-half-native
INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_SOURCE := $(BUILD_DIR)/interpreter/demand-cache-retry-signed-byte.s
INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_MAIN := $(BUILD_DIR)/interpreter-demand-cache-signed-byte-main
INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_NATIVE := $(BUILD_DIR)/interpreter-demand-cache-signed-byte-native
INTERPRETER_DEMAND_CACHE_SIGNED_HALF_SOURCE := $(BUILD_DIR)/interpreter/demand-cache-retry-signed-half.s
INTERPRETER_DEMAND_CACHE_SIGNED_HALF_MAIN := $(BUILD_DIR)/interpreter-demand-cache-signed-half-main
INTERPRETER_DEMAND_CACHE_SIGNED_HALF_NATIVE := $(BUILD_DIR)/interpreter-demand-cache-signed-half-native
INTERPRETER_DEMAND_CACHE_SIGNED_WORD_SOURCE := $(BUILD_DIR)/interpreter/demand-cache-retry-signed-word.s
INTERPRETER_DEMAND_CACHE_SIGNED_WORD_MAIN := $(BUILD_DIR)/interpreter-demand-cache-signed-word-main
INTERPRETER_DEMAND_CACHE_SIGNED_WORD_NATIVE := $(BUILD_DIR)/interpreter-demand-cache-signed-word-native
INTERPRETER_DEMAND_CACHE_VECTOR_POST_SOURCE := $(BUILD_DIR)/interpreter/demand-cache-retry-vector-post.s
INTERPRETER_DEMAND_CACHE_VECTOR_POST_MAIN := $(BUILD_DIR)/interpreter-demand-cache-vector-post-main
INTERPRETER_DEMAND_CACHE_VECTOR_POST_NATIVE := $(BUILD_DIR)/interpreter-demand-cache-vector-post-native
INTERPRETER_DEMAND_CACHE_VECTOR_PRE_SOURCE := $(BUILD_DIR)/interpreter/demand-cache-retry-vector-pre.s
INTERPRETER_DEMAND_CACHE_VECTOR_PRE_MAIN := $(BUILD_DIR)/interpreter-demand-cache-vector-pre-main
INTERPRETER_DEMAND_CACHE_VECTOR_PRE_NATIVE := $(BUILD_DIR)/interpreter-demand-cache-vector-pre-native
INTERPRETER_GENERATOR := $(BUILD_DIR)/generate-interpreter-msl
INTERPRETER_SOURCE := $(BUILD_DIR)/interpreter/gpu_interpreter.generated.metal
INTERPRETER_CBZ_CBNZ_MAIN := $(BUILD_DIR)/interpreter-cbz-cbnz-main
INTERPRETER_TBZ_TBNZ_MAIN := $(BUILD_DIR)/interpreter-tbz-tbnz-main
INTERPRETER_ADR_ADRP_MAIN := $(BUILD_DIR)/interpreter-adr-adrp-main
INTERPRETER_STACK_GUARD_NATIVE := $(BUILD_DIR)/interpreter-pthread-stack-guard-retry-native
INTERPRETER_MPROTECT_RETRY_NATIVE := $(BUILD_DIR)/interpreter-mprotect-cross-page-retry-native
INTERPRETER_RUNTIME_GUARD_NATIVE := $(BUILD_DIR)/interpreter-pthread-runtime-owned-guard-retry-native
INTERPRETER_ADC_SBC_CARRY_MAIN := $(BUILD_DIR)/interpreter-adc-sbc-carry-main
INTERPRETER_SCALAR_FP_MAIN := $(BUILD_DIR)/interpreter-scalar-fp-main
INTERPRETER_SCALAR_JSCVT_MAIN := $(BUILD_DIR)/interpreter-scalar-jscvt-main
INTERPRETER_SCALAR_FP_COMPARE_MAIN := $(BUILD_DIR)/interpreter-scalar-fp-compare-main
INTERPRETER_SCALAR_FMOV_D_MAIN := $(BUILD_DIR)/interpreter-scalar-fmov-d-main
INTERPRETER_SCALAR_FP_COMPARE_CASES := $(addprefix $(BUILD_DIR)/interpreter-scalar-fp-compare-case-,1 2 3 4 5)
ADC_SBC_CARRY_DECODE_TEST := $(BUILD_DIR)/adc-sbc-carry-decode-test
GUEST_FORMAT_NATIVE := $(BUILD_DIR)/guest-printf-formats-native
GUEST_FORMAT_TEST := $(BUILD_DIR)/guest-format-test
MAP_JIT_POLICY_TEST := $(BUILD_DIR)/map-jit-policy-test
JIT_CODE_CACHE_RESERVE_NATIVE := $(BUILD_DIR)/jit-code-cache-reserve
JIT_CODE_CACHE_INVALID_NATIVE := $(BUILD_DIR)/jit-code-cache-invalid
CHAINED_ADDENDS_TEST := $(BUILD_DIR)/chained-addends-test
CHAINED_ADDENDS_FIXTURE := $(BUILD_DIR)/libchained-addend-fixture.dylib
CHAINED_ADDEND_PROVIDER := $(BUILD_DIR)/libchained-addend-provider.dylib
CHAINED_ADDEND_POINTER := $(BUILD_DIR)/chained-addend-positive-pointer.o
CHAINED_ADDEND_POSITIVE := $(BUILD_DIR)/chained-addend-positive
CHAINED_ADDEND_WEAK_POINTER := $(BUILD_DIR)/chained-addend-weak-pointer.o
CHAINED_ADDEND_WEAK := $(BUILD_DIR)/chained-addend-weak
CHAINED_ADDENDS_IMAGE_TEST := $(BUILD_DIR)/chained-addends-image-test
MAP_JIT_POLICY_FIXTURE := $(BUILD_DIR)/map-jit-policy-fixture
MEMCPY_CHK_NATIVE := $(BUILD_DIR)/memcpy-chk-native
MEMCPY_CHK_SIGINFO_NATIVE := $(BUILD_DIR)/memcpy-chk-siginfo-native
MEMCPY_CHK_DECODE_TEST := $(BUILD_DIR)/memcpy-chk-decode-test
MEMSET_CHK_NATIVE := $(BUILD_DIR)/memset-chk-native
MEMSET_CHK_SIGINFO_NATIVE := $(BUILD_DIR)/memset-chk-siginfo-native
MEMSET_CHK_DECODE_TEST := $(BUILD_DIR)/memset-chk-decode-test
MACH_VM_OVERWRITE_NATIVE := $(BUILD_DIR)/mach-vm-map-overwrite-native
MACH_VM_DEFERRED_RECLAMATION_NATIVE := $(BUILD_DIR)/mach-vm-deferred-reclamation-native
MACH_VM_MEMORY_ENTRY_NATIVE := $(BUILD_DIR)/mach-vm-memory-entry-native
MACH_VM_MEMORY_ENTRY_INVALID_OUTPUT := $(BUILD_DIR)/mach-vm-memory-entry-invalid-output
GUEST_HEAP_ALLOCATE_AT_TEST := $(BUILD_DIR)/guest-heap-allocate-at-test
CACHE_BACKING_LEASES_TEST := $(BUILD_DIR)/cache-backing-leases-test
VM_BACKING_LEASE_TEST := $(BUILD_DIR)/vm-backing-lease-test
SHA256_NATIVE := $(BUILD_DIR)/sha256-instructions-native
VECTOR_CRYPTO_LOWER_TEST := $(BUILD_DIR)/vector-crypto-lowering-test
VECTOR_CRYPTO_MSL := $(BUILD_DIR)/vector-crypto.metal
VECTOR_CRYPTO_AIR := $(BUILD_DIR)/vector-crypto.air
INTERPRETER_AIR := $(BUILD_DIR)/interpreter/gpu_interpreter.air
INTERPRETER_METALLIB := $(BUILD_DIR)/interpreter/runtime-interpreter.metallib

app: $(APP)

.PHONY: app-build-config-force
app-build-config-force:

$(APP_CONFIG): app-build-config-force
	@mkdir -p "$(APP_OBJECT_DIR)"
	@{ \
	  printf '%s\n' 'format=1'; \
	  printf '%s\n' 'CXX=$(CXX)'; \
	  printf '%s\n' 'CPPFLAGS=$(CPPFLAGS)'; \
	  printf '%s\n' 'CXXFLAGS=$(CXXFLAGS)'; \
	  printf '%s\n' 'METAL_CPP=$(METAL_CPP)'; \
	  printf '%s\n' 'APP_SOURCES=$(APP_SOURCES)'; \
	  printf '%s\n' 'APP_COMPILE_EXTRA=-fblocks'; \
	  printf '%s\n' 'APP_DEP_MODE=-MD'; \
	  printf '%s\n' 'APP_LINK_LIBS=$(APP_LINK_LIBS)'; \
	} >"$@.tmp"
	@if ! cmp -s "$@.tmp" "$@"; then mv "$@.tmp" "$@"; else rm "$@.tmp"; fi

$(APP_OBJECT_DIR)/%.o: %.cpp $(APP_CONFIG)
	@mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) -isystem "$(METAL_CPP)" $(CXXFLAGS) -fblocks \
		-MD -MP -MF "$(@:.o=.d)" -c "$<" -o "$@"

-include $(APP_DEPFILES)

$(KDEBUG_TRACE64_NATIVE): ../fixtures/handoff/kdebug-trace64-disabled.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

verify-kdebug-trace64-native: $(KDEBUG_TRACE64_NATIVE)
	"$(KDEBUG_TRACE64_NATIVE)"

$(SEMAPHORE_NATIVE): ../fixtures/handoff/mach-semaphore-lifecycle.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror -pthread "$<" -o "$@"

verify-semaphore-native: $(SEMAPHORE_NATIVE)
	"$(SEMAPHORE_NATIVE)"

$(SEMAPHORE_SCHEDULER): ../fixtures/handoff/guest-semaphore-scheduler.cpp \
		guest_scheduler.cpp guest_state.cpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I. $^ -o "$@"

verify-semaphore-scheduler: $(SEMAPHORE_SCHEDULER)
	"$(SEMAPHORE_SCHEDULER)"

$(SIGNAL_THREAD_FRAMES): ../fixtures/handoff/guest-signal-thread-frames.cpp \
		guest_signal_runtime.cpp guest_state.cpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I. $^ -o "$@"

verify-signal-thread-frames: $(SIGNAL_THREAD_FRAMES)
	"$(SIGNAL_THREAD_FRAMES)"

$(THREADED_RAISE_SIGNALS_NATIVE): ../fixtures/handoff/threaded-raise-signals.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -pthread "$<" -o "$@"

verify-threaded-raise-signals-native: $(THREADED_RAISE_SIGNALS_NATIVE)
	"$(THREADED_RAISE_SIGNALS_NATIVE)"

$(PTHREAD_KILL_NATIVE): ../fixtures/handoff/pthread-kill-targeted.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror -Wno-nonnull \
		-fno-stack-protector -fno-builtin-memset -pthread "$<" -o "$@"

verify-pthread-kill-native: $(PTHREAD_KILL_NATIVE)
	perl -e 'alarm 5; exec @ARGV' "$(PTHREAD_KILL_NATIVE)"

$(OPEN_NOCANCEL_NATIVE): ../fixtures/handoff/open-nocancel.c \
		../fixtures/handoff/open-nocancel-input.txt
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

verify-open-nocancel-native: $(OPEN_NOCANCEL_NATIVE)
	"$(OPEN_NOCANCEL_NATIVE)" "$(abspath ../fixtures/handoff/open-nocancel-input.txt)"

$(BUILD_DIR)/sandbox-persona-query: ../fixtures/handoff/sandbox-persona-query.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -O2 -arch arm64 -std=c11 -Wall -Wextra -Werror "$<" -o "$@"

.PHONY: verify-sandbox-persona
verify-sandbox-persona: $(BUILD_DIR)/sandbox-persona-query $(APP) $(INTERPRETER_METALLIB)
	@set -eu; run_dir=$$(mktemp -d "$(BUILD_DIR)/sandbox-persona-verify.XXXXXX"); \
	probe="$(abspath $(BUILD_DIR)/sandbox-persona-query)"; launcher="$(abspath $(APP))"; \
	export ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))"; \
	for mode in persona null-reply path; do \
		"$$probe" "$$mode" >"$$run_dir/$$mode.native"; \
		"$$launcher" launch "$$probe" "$$mode" >"$$run_dir/$$mode.guest"; \
		cmp "$$run_dir/$$mode.native" "$$run_dir/$$mode.guest"; \
	done; \
	policy='(version 1)(allow default)(deny syscall-unix (syscall-number 494))'; \
	/usr/bin/sandbox-exec -p "$$policy" "$$probe" >"$$run_dir/deny.native"; \
	/usr/bin/sandbox-exec -p "$$policy" "$$launcher" launch "$$probe" >"$$run_dir/deny.guest"; \
	cmp "$$run_dir/deny.native" "$$run_dir/deny.guest"; \
	grep -qx 'decision=0' "$$run_dir/persona.native"; grep -qx 'decision=1' "$$run_dir/deny.native"; \
	for mode in other-number other-operation other-subject other-flags; do \
		status=0; "$$launcher" launch "$$probe" "$$mode" >"$$run_dir/$$mode.out" 2>"$$run_dir/$$mode.err" || status=$$?; \
		test "$$status" -eq 1 && test ! -s "$$run_dir/$$mode.out" || exit 1; \
		grep -Eq '^arm64-metal: Unsupported guest sandbox (status )?query shape$$' "$$run_dir/$$mode.err"; \
	done; echo "Sandbox persona parity and refusal controls passed in $$run_dir"

$(BUILD_DIR)/amfi-self-policy: ../fixtures/handoff/amfi-self-policy.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -O2 -arch arm64 -std=c11 -Wall -Wextra -Werror "$<" -o "$@"

.PHONY: verify-amfi-self-policy
verify-amfi-self-policy: $(BUILD_DIR)/amfi-self-policy $(APP) $(INTERPRETER_METALLIB)
	@set -eu; run_dir=$$(mktemp -d "$(BUILD_DIR)/amfi-self-policy-verify.XXXXXX"); \
	probe="$(abspath $(BUILD_DIR)/amfi-self-policy)"; launcher="$(abspath $(APP))"; \
	export ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))"; \
	policy='(version 1)(allow default)(deny syscall-unix (syscall-number 381))'; \
	/usr/bin/sandbox-exec -p "$$policy" "$$probe" >"$$run_dir/restricted.native"; \
	"$$launcher" launch "$$probe" >"$$run_dir/strict.guest"; \
	cmp "$$run_dir/restricted.native" "$$run_dir/strict.guest"; \
	for mode in other-flags null-output unmapped-output read-only-output null-request other-call other-policy; do \
		status=0; "$$launcher" launch "$$probe" "$$mode" >"$$run_dir/$$mode.out" 2>"$$run_dir/$$mode.err" || status=$$?; \
		test "$$status" -eq 1 && test ! -s "$$run_dir/$$mode.out" || exit 1; \
		grep -qx 'arm64-metal: Unsupported guest MAC policy or call' "$$run_dir/$$mode.err"; \
	done; echo "Guest self-policy denial and refusal controls passed in $$run_dir"

$(BUILD_DIR)/bank-voucher-traps: ../fixtures/handoff/voucher-traps.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -O2 -fno-builtin -fno-stack-protector "$<" -o "$@"

.PHONY: verify-bank-voucher
verify-bank-voucher: $(BUILD_DIR)/bank-voucher-traps $(APP) $(INTERPRETER_METALLIB)
	@set -eu; run_dir=$$(mktemp -d "$(BUILD_DIR)/bank-voucher-verify.XXXXXX"); \
	probe="$(abspath $(BUILD_DIR)/bank-voucher-traps)"; launcher="$(abspath $(APP))"; \
	export ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))"; \
	"$$probe" k >"$$run_dir/native"; \
	"$$launcher" launch "$$probe" k >"$$run_dir/guest"; \
	cmp "$$run_dir/native" "$$run_dir/guest"; \
	"$$launcher" launch "$$probe" kg >"$$run_dir/guest-refusals"; \
	cmp "$$run_dir/native" "$$run_dir/guest-refusals"; \
	echo "Default bank voucher lifetime and IPC refusals passed in $$run_dir"

$(OPEN_APPEND_NATIVE): ../fixtures/handoff/open-append.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror "$<" -o "$@"

verify-open-append-native: $(OPEN_APPEND_NATIVE)
	"$(OPEN_APPEND_NATIVE)" >"$(BUILD_DIR)/open-append.native.out"
	printf '%s\n' 'named=seedAB raw=seedAB offset=6 cleanup=ok' >"$(BUILD_DIR)/open-append.expected.out"
	cmp "$(BUILD_DIR)/open-append.expected.out" "$(BUILD_DIR)/open-append.native.out"

$(PROC_INFO_NATIVE): ../fixtures/handoff/proc-info-self.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

verify-proc-info-native: $(PROC_INFO_NATIVE)
	"$(PROC_INFO_NATIVE)"

$(AF_UNIX_SOCKET_NATIVE): ../fixtures/handoff/af-unix-socket.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

verify-af-unix-socket-native: $(AF_UNIX_SOCKET_NATIVE)
	"$(AF_UNIX_SOCKET_NATIVE)"

$(AF_UNIX_CONNECT_NATIVE): ../fixtures/handoff/af-unix-connect.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror -fno-stack-protector \
		-fno-builtin-memset "$<" -o "$@"

verify-af-unix-connect-native: $(AF_UNIX_CONNECT_NATIVE)
	@set -eu; owned_dir=$$(mktemp -d /tmp/arm64-metal-connect.XXXXXX); \
	trap 'rmdir "$$owned_dir"' EXIT; \
	"$(AF_UNIX_CONNECT_NATIVE)" "$$owned_dir/absent"

$(AF_UNIX_DATAGRAM_NATIVE): ../fixtures/handoff/af-unix-datagram.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror -fno-stack-protector \
		-fno-builtin-memset "$<" -o "$@"

verify-af-unix-datagram-native: $(AF_UNIX_DATAGRAM_NATIVE)
	@set -eu; owned_dir=$$(mktemp -d /tmp/arm64-metal-dgram.XXXXXX); \
	trap 'rm -f "$$owned_dir/server" "$$owned_dir/client"; rmdir "$$owned_dir"' EXIT; \
	"$(AF_UNIX_DATAGRAM_NATIVE)" "$$owned_dir/server" "$$owned_dir/client"

$(AF_UNIX_CONNECT_EFAULT_NATIVE): ../fixtures/handoff/af-unix-connect-efault.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror -fno-stack-protector \
		-fno-builtin-memset "$<" -o "$@"

verify-af-unix-connect-efault-native: $(AF_UNIX_CONNECT_EFAULT_NATIVE)
	"$(AF_UNIX_CONNECT_EFAULT_NATIVE)"

$(AF_UNIX_DGRAM_BLOCKING_NATIVE): ../fixtures/handoff/af-unix-datagram-blocking.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror -fno-stack-protector \
		-fno-builtin-memset -pthread "$<" -o "$@"

verify-af-unix-datagram-blocking-native: $(AF_UNIX_DGRAM_BLOCKING_NATIVE)
	@set -eu; owned_dir=$$(mktemp -d /tmp/arm64-metal-dgram-blocking.XXXXXX); \
	trap 'rm -f "$$owned_dir/server"; rmdir "$$owned_dir"' EXIT; \
	"$(AF_UNIX_DGRAM_BLOCKING_NATIVE)" "$$owned_dir/server"

$(AF_UNIX_CLOSE_BLOCKED_NATIVE): ../fixtures/handoff/af-unix-close-blocked.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror -fno-stack-protector \
		-fno-builtin-memset -pthread "$<" -o "$@"

verify-af-unix-close-blocked-native: $(AF_UNIX_CLOSE_BLOCKED_NATIVE)
	@set -eu; owned_dir=$$(mktemp -d /tmp/arm64-metal-close-blocked.XXXXXX); \
	trap 'rm -f "$$owned_dir"/socket-*; rmdir "$$owned_dir"' EXIT; \
	perl -e 'alarm 5; exec @ARGV' "$(AF_UNIX_CLOSE_BLOCKED_NATIVE)" 6 "$$owned_dir/socket-6"; \
	perl -e 'alarm 5; exec @ARGV' "$(AF_UNIX_CLOSE_BLOCKED_NATIVE)" n "$$owned_dir/socket-399"

.PHONY: verify-malloc-size-domain-native
MALLOC_SIZE_DOMAIN := $(BUILD_DIR)/malloc-size-domain
$(MALLOC_SIZE_DOMAIN): ../fixtures/handoff/malloc-size-domain.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror "$<" -o "$@"

verify-malloc-size-domain-native: $(MALLOC_SIZE_DOMAIN)
	"$(MALLOC_SIZE_DOMAIN)"

.PHONY: verify-dyld-platform-apis-native
DYLD_PLATFORM_APIS := $(BUILD_DIR)/dyld-platform-apis-native
$(DYLD_PLATFORM_APIS): ../fixtures/handoff/dyld-platform-apis.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror "$<" -o "$@"

verify-dyld-platform-apis-native: $(DYLD_PLATFORM_APIS)
	"$(DYLD_PLATFORM_APIS)"

$(APP): $(APP_OBJECTS) $(APP_CONFIG) Makefile
	@test -f "$(METAL_CPP)/Foundation/Foundation.hpp" || { echo 'Set METAL_CPP to the installed metal-cpp include root' >&2; exit 1; }
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) -isystem "$(METAL_CPP)" $(CXXFLAGS) -fblocks $(APP_OBJECTS) \
		$(APP_LINK_LIBS) -o "$@"

clean:
	rm -rf "$(BUILD_DIR)"

$(INTERPRETER_NATIVE): ../fixtures/interpreter/movz-ret.s ../fixtures/interpreter/native-main.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror $^ -o "$@"

$(INTERPRETER_STACK_GUARD_NATIVE): ../fixtures/interpreter/pthread-stack-guard-retry.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror $^ -o "$@"

verify-interpreter-stack-guard-retry: $(APP) $(INTERPRETER_STACK_GUARD_NATIVE) $(INTERPRETER_METALLIB)
	@set -eu; \
		run_dir=$$(mktemp -d "$(BUILD_DIR)/interpreter-stack-guard.XXXXXX"); \
		set +e; \
		"$(INTERPRETER_STACK_GUARD_NATIVE)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
		"$(APP)" launch "$(abspath $(INTERPRETER_STACK_GUARD_NATIVE))" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(abspath $(INTERPRETER_STACK_GUARD_NATIVE))" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
		set -e; \
		printf '%s\n' "$$native_rc" >"$$run_dir/native.rc"; \
		printf '%s\n' "$$translated_rc" >"$$run_dir/translated.rc"; \
		printf '%s\n' "$$interpreter_rc" >"$$run_dir/interpreter.rc"; \
		test "$$native_rc" -eq 42; \
		test "$$translated_rc" -eq "$$native_rc"; \
		test "$$interpreter_rc" -eq "$$native_rc"; \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out"; \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out"; \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err"; \
		sed '/^arm64-metal trace /d' "$$run_dir/interpreter.err" >"$$run_dir/interpreter.no-trace.err"; \
		cmp "$$run_dir/native.err" "$$run_dir/interpreter.no-trace.err"; \
		grep -q 'pthread-stack-guard-retry-ok' "$$run_dir/interpreter.out"; \
		grep -q 'event=interpreter-dispatch' "$$run_dir/interpreter.err"; \
		echo "verify-interpreter-stack-guard-retry receipts: $$run_dir"

$(INTERPRETER_MPROTECT_RETRY_NATIVE): ../fixtures/interpreter/mprotect-cross-page-retry.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror $^ -o "$@"

verify-interpreter-mprotect-cross-page-retry: $(APP) $(INTERPRETER_MPROTECT_RETRY_NATIVE) $(INTERPRETER_METALLIB)
	@set -eu; \
		run_dir=$$(mktemp -d "$(BUILD_DIR)/interpreter-mprotect-cross-page.XXXXXX"); \
		set +e; \
		"$(INTERPRETER_MPROTECT_RETRY_NATIVE)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
		"$(APP)" launch "$(abspath $(INTERPRETER_MPROTECT_RETRY_NATIVE))" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(abspath $(INTERPRETER_MPROTECT_RETRY_NATIVE))" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
		set -e; \
		printf '%s\n' "$$native_rc" >"$$run_dir/native.rc"; \
		printf '%s\n' "$$translated_rc" >"$$run_dir/translated.rc"; \
		printf '%s\n' "$$interpreter_rc" >"$$run_dir/interpreter.rc"; \
		test "$$native_rc" -eq 42; \
		test "$$translated_rc" -eq "$$native_rc"; \
		test "$$interpreter_rc" -eq "$$native_rc"; \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out"; \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out"; \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err"; \
		sed '/^arm64-metal trace /d' "$$run_dir/interpreter.err" >"$$run_dir/interpreter.no-trace.err"; \
		cmp "$$run_dir/native.err" "$$run_dir/interpreter.no-trace.err"; \
		grep -q 'mprotect-cross-page-retry-ok' "$$run_dir/interpreter.out"; \
		grep -q 'event=interpreter-dispatch' "$$run_dir/interpreter.err"; \
		echo "verify-interpreter-mprotect-cross-page-retry receipts: $$run_dir"

$(INTERPRETER_RUNTIME_GUARD_NATIVE): ../fixtures/interpreter/pthread-runtime-owned-guard-retry.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror $^ -o "$@"

verify-interpreter-pthread-runtime-owned-guard: $(APP) $(INTERPRETER_RUNTIME_GUARD_NATIVE) $(INTERPRETER_METALLIB)
	@set -eu; \
		run_dir=$$(mktemp -d "$(BUILD_DIR)/interpreter-pthread-runtime-guard.XXXXXX"); \
		set +e; \
		"$(INTERPRETER_RUNTIME_GUARD_NATIVE)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
		"$(APP)" launch "$(abspath $(INTERPRETER_RUNTIME_GUARD_NATIVE))" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(abspath $(INTERPRETER_RUNTIME_GUARD_NATIVE))" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
		set -e; \
		printf '%s\n' "$$native_rc" >"$$run_dir/native.rc"; \
		printf '%s\n' "$$translated_rc" >"$$run_dir/translated.rc"; \
		printf '%s\n' "$$interpreter_rc" >"$$run_dir/interpreter.rc"; \
		test "$$native_rc" -eq 42; \
		test "$$translated_rc" -eq "$$native_rc"; \
		test "$$interpreter_rc" -eq "$$native_rc"; \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out"; \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out"; \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err"; \
		sed '/^arm64-metal trace /d' "$$run_dir/interpreter.err" >"$$run_dir/interpreter.no-trace.err"; \
		cmp "$$run_dir/native.err" "$$run_dir/interpreter.no-trace.err"; \
		grep -q 'pthread-runtime-owned-guard-fault-ok' "$$run_dir/interpreter.out"; \
		grep -q 'event=interpreter-dispatch' "$$run_dir/interpreter.err"; \
		echo "verify-interpreter-pthread-runtime-owned-guard receipts: $$run_dir"

$(INTERPRETER_TYPED_NATIVE): ../fixtures/handoff/typed-operation.cpp process_decoder.cpp process_decoder.hpp guest_strings.cpp guest_vector_crypto.cpp translate_core.cpp translate_core.hpp integer_ops_shared.h
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I. ../fixtures/handoff/typed-operation.cpp process_decoder.cpp guest_strings.cpp guest_vector_crypto.cpp translate_core.cpp -o "$@"

$(AUTH_RETURN_NATIVE): ../fixtures/handoff/authenticated-return.s ../fixtures/handoff/authenticated-return-main.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64e -Wall -Wextra -Werror $^ -o "$@"

$(AUTH_RETURN_CPU): ../fixtures/handoff/process-authenticated-return.cpp process_decoder.cpp process_msl.cpp translate_core.cpp guest_strings.cpp guest_vector_crypto.cpp macho.cpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I. $^ -o "$@"

$(AUTH_RETURN_MAIN): ../fixtures/interpreter/authenticated-return-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64e -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(AUTH_RETURN_RETAB_MAIN): ../fixtures/interpreter/authenticated-return-retab-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64e -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(AUTH_RETURN_BAD_LR_MAIN): ../fixtures/interpreter/authenticated-return-bad-lr-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64e -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(AUTH_RETURN_BAD_SP_MAIN): ../fixtures/interpreter/authenticated-return-bad-sp-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64e -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(AUTH_RETURN_BAD_LR_RETAB_MAIN): ../fixtures/interpreter/authenticated-return-bad-lr-retab-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64e -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(AUTH_RETURN_BAD_SP_RETAB_MAIN): ../fixtures/interpreter/authenticated-return-bad-sp-retab-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64e -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(AUTH_RETURN_BAD_SP_NESTED_MAIN): ../fixtures/interpreter/authenticated-return-bad-sp-nested-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64e -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

verify-authenticated-return-cpu: $(AUTH_RETURN_CPU) $(AUTH_RETURN_MAIN) $(AUTH_RETURN_RETAB_MAIN)
	"$(AUTH_RETURN_CPU)" "$(AUTH_RETURN_MAIN)" "$(AUTH_RETURN_RETAB_MAIN)"

verify-authenticated-return-native: $(AUTH_RETURN_NATIVE) $(INTERPRETER_TYPED_NATIVE) $(AUTH_RETURN_MAIN) $(AUTH_RETURN_RETAB_MAIN) $(AUTH_RETURN_BAD_LR_MAIN) $(AUTH_RETURN_BAD_SP_MAIN) $(AUTH_RETURN_BAD_LR_RETAB_MAIN) $(AUTH_RETURN_BAD_SP_RETAB_MAIN) $(AUTH_RETURN_BAD_SP_NESTED_MAIN)
	@set -eu; ulimit -c 0; \
		"$(INTERPRETER_TYPED_NATIVE)"; \
		"$(AUTH_RETURN_NATIVE)"; \
		"$(AUTH_RETURN_NATIVE)" retab; \
		"$(AUTH_RETURN_MAIN)"; \
		"$(AUTH_RETURN_RETAB_MAIN)"; \
		for mismatch in bad-lr bad-sp bad-lr-retab bad-sp-retab bad-sp-nested; do \
			if "$(AUTH_RETURN_NATIVE)" "$$mismatch" >/dev/null 2>&1; then exit 1; \
			else status=$$?; test "$$status" -gt 128 || exit 1; fi; \
		done; \
		for mismatch in "$(AUTH_RETURN_BAD_LR_MAIN)" "$(AUTH_RETURN_BAD_SP_MAIN)" \
			"$(AUTH_RETURN_BAD_LR_RETAB_MAIN)" "$(AUTH_RETURN_BAD_SP_RETAB_MAIN)" \
			"$(AUTH_RETURN_BAD_SP_NESTED_MAIN)"; do \
			if "$$mismatch" >/dev/null 2>&1; then exit 1; \
			else status=$$?; test "$$status" -gt 128 || exit 1; fi; \
		done; \
		echo 'Key-B native C and no-import _main controls passed'

verify-authenticated-return: verify-authenticated-return-cpu verify-authenticated-return-native

verify-authenticated-return-interpreter: $(APP) $(INTERPRETER_METALLIB) $(AUTH_RETURN_MAIN) $(AUTH_RETURN_RETAB_MAIN) $(AUTH_RETURN_BAD_LR_MAIN) $(AUTH_RETURN_BAD_SP_MAIN) $(AUTH_RETURN_BAD_LR_RETAB_MAIN) $(AUTH_RETURN_BAD_SP_RETAB_MAIN) $(AUTH_RETURN_BAD_SP_NESTED_MAIN)
	@set -eu; run_dir=$$(mktemp -d "$(BUILD_DIR)/auth-return-trace.XXXXXX"); \
	trap 'rm -rf "$$run_dir"' EXIT; \
	run_case() { \
		name=$$1; executable=$$2; expected_status=$$3; expected_ops=$$4; \
		if ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 ARM64_METAL_TRACE_INTERPRETER_PACKETS=1 "$(APP)" launch "$$executable" >"$$run_dir/$$name.out" 2>"$$run_dir/$$name.trace"; then rc=0; else rc=$$?; fi; \
		test "$$rc" -eq "$$expected_status"; \
		awk -v wanted="$$expected_ops" ' \
			function hexval(s, i, n, c) { sub(/^pc=0x/, "", s); n=0; for(i=1;i<=length(s);i++){ c=index("0123456789abcdef",tolower(substr(s,i,1)))-1; if(c<0)return -1; n=n*16+c } return n } \
			/event=interpreter-block-ready/ { ready_pc=hexval($$5); ready_count=$$6; sub(/^detail=/,"",ready_count) } \
			/event=interpreter-packet/ { pcs[++n]=hexval($$5); ops[n]=$$6; sub(/^detail=/,"",ops[n]) } \
			END { m=split(wanted, expected, ","); start=n-m+1; if(start<1 || ready_count!=m || ready_pc!=pcs[start]) exit 1; for(i=1;i<=m;i++){ if(ops[start+i-1]!=expected[i]) exit 1; if(i>1 && pcs[start+i-1]-pcs[start+i-2]!=4) exit 1 } }' "$$run_dir/$$name.trace"; \
		if test "$$expected_status" -eq 0; then test ! -s "$$run_dir/$$name.out"; else grep -q 'event=guest-authentication-fault' "$$run_dir/$$name.trace"; fi; \
	}; \
	run_case autibsp "$(AUTH_RETURN_MAIN)" 0 36,36,1,2; \
	run_case retab "$(AUTH_RETURN_RETAB_MAIN)" 0 36,1,36; \
	run_case bad-lr "$(AUTH_RETURN_BAD_LR_MAIN)" 138 36,23,36,1,2; \
	run_case bad-sp "$(AUTH_RETURN_BAD_SP_MAIN)" 138 36,10,36,9,1,2; \
	run_case bad-sp-nested "$(AUTH_RETURN_BAD_SP_NESTED_MAIN)" 138 23,2; \
	awk -v wanted="36,10,36,9,23,28,2,23,2" 'BEGIN { count=split(wanted, expected, ","); next_expected=1 } \
		/event=interpreter-packet/ { detail=$$6; sub(/^detail=/,"",detail); n=split(detail, actual, ","); for (i=1;i<=n;i++) if (actual[i]==expected[next_expected]) next_expected++ } \
		END { if (next_expected<=count) exit 1 }' "$$run_dir/bad-sp-nested.trace"; \
	run_case bad-lr-retab "$(AUTH_RETURN_BAD_LR_RETAB_MAIN)" 138 36,23,36; \
	run_case bad-sp-retab "$(AUTH_RETURN_BAD_SP_RETAB_MAIN)" 138 36,10,36; \
	echo 'Typed PACIBSP/AUTIBSP/RETAB packet PC traces and guest outcomes passed'

$(RETURN_SENTINEL_MAIN): ../fixtures/interpreter/return-sentinel-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64e -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

.PHONY: verify-return-sentinel-native verify-return-sentinel-interpreter
verify-return-sentinel-native: $(RETURN_SENTINEL_MAIN)
	@set +e; ulimit -c 0; "$(RETURN_SENTINEL_MAIN)"; status=$$?; set -e; \
		test "$$status" -eq 42; echo "PAC/TBZ startup return sentinel native exit=$$status"

verify-return-sentinel-interpreter: $(APP) $(INTERPRETER_METALLIB) $(RETURN_SENTINEL_MAIN)
	@set +e; ARM64_METAL_INTERPRETER=1 \
		ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" \
		"$(APP)" launch "$(abspath $(RETURN_SENTINEL_MAIN))"; status=$$?; set -e; \
		test "$$status" -eq 42; echo "PAC/TBZ startup return sentinel Metal exit=$$status"

DATA_AUTH_ISA_MAIN := $(BUILD_DIR)/data-auth-isa-main
$(DATA_AUTH_ISA_MAIN): ../fixtures/interpreter/data-auth-isa-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64e -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

.PHONY: verify-data-auth-isa-native verify-data-auth-isa
verify-data-auth-isa-native: $(DATA_AUTH_ISA_MAIN)
	@set +e; ulimit -c 0; "$(DATA_AUTH_ISA_MAIN)"; status=$$?; set -e; \
		test "$$status" -eq 42; echo "Data-A ISA masking native exit=$$status"

verify-data-auth-isa: $(APP) $(DATA_AUTH_ISA_MAIN)
	@set +e; "$(DATA_AUTH_ISA_MAIN)"; native=$$?; \
		"$(APP)" launch "$(abspath $(DATA_AUTH_ISA_MAIN))"; guest=$$?; set -e; \
		test "$$native" -eq 42; test "$$guest" -eq 42; \
		echo "Data-A ISA masking native=$$native guest=$$guest"

DYLSYM_ARM64E_PAC := $(BUILD_DIR)/dlsym-arm64e-pac
DYLSYM_ARM64_PAC := $(BUILD_DIR)/dlsym-arm64-pac
DYLSYM_PAC_SOURCE := ../fixtures/handoff/dlsym-arm64e-pac.c
$(DYLSYM_ARM64E_PAC): $(DYLSYM_PAC_SOURCE)
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64e -Wall -Wextra -Werror -Wno-cast-function-type-strict \
		-Wl,-export_dynamic "$<" -o "$@"

.PHONY: verify-dlsym-arm64e-pac-native verify-dlsym-arm64-pac-native verify-dlsym-arm64e-pac
$(DYLSYM_ARM64_PAC): $(DYLSYM_PAC_SOURCE)
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror -Wno-cast-function-type-strict \
		-Wl,-export_dynamic "$<" -o "$@"

verify-dlsym-arm64e-pac-native: $(DYLSYM_ARM64E_PAC)
	@set +e; ulimit -c 0; "$(DYLSYM_ARM64E_PAC)"; status=$$?; set -e; \
		test "$$status" -eq 42; echo "arm64e dlsym function/data native exit=$$status"

verify-dlsym-arm64-pac-native: $(DYLSYM_ARM64_PAC)
	@set +e; ulimit -c 0; "$(DYLSYM_ARM64_PAC)"; status=$$?; set -e; \
		test "$$status" -eq 42; echo "arm64 dlsym function/data native exit=$$status"

verify-dlsym-arm64e-pac: $(APP) $(DYLSYM_ARM64E_PAC) $(DYLSYM_ARM64_PAC)
	@set +e; "$(DYLSYM_ARM64E_PAC)"; native_e=$$?; \
		"$(APP)" launch "$(abspath $(DYLSYM_ARM64E_PAC))"; guest_e=$$?; \
		"$(DYLSYM_ARM64_PAC)"; native=$$?; \
		"$(APP)" launch "$(abspath $(DYLSYM_ARM64_PAC))"; guest=$$?; set -e; \
		test "$$native_e" -eq 42; test "$$guest_e" -eq 42; \
		test "$$native" -eq 42; test "$$guest" -eq 42; \
		echo "dlsym function/data arm64e native=$$native_e guest=$$guest_e arm64 native=$$native guest=$$guest"


$(INTERPRETER_ADDP_2D_MAIN): ../fixtures/interpreter/addp-2d-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(ADDP_2D_PROCESS_TEST): ../fixtures/handoff/addp-2d-process-decode.cpp $(APP_SOURCES) $(APP_HEADERS)
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) -isystem "$(METAL_CPP)" $(CXXFLAGS) -UNDEBUG -I. -fblocks \
		$(filter-out main.cpp,$(APP_SOURCES)) $< \
		-framework Foundation -framework Metal -framework Security -o "$@"

verify-addp-2d-native: $(INTERPRETER_ADDP_2D_MAIN) $(ADDP_2D_PROCESS_TEST)
	@set -eu; \
		"$(ADDP_2D_PROCESS_TEST)"; \
		set +e; "$(INTERPRETER_ADDP_2D_MAIN)"; result=$$?; set -e; \
		test "$$result" -eq 42; \
		echo "addp.2d native guest exit=$$result"

$(XTN_PROCESS_TEST): ../fixtures/handoff/xtn-process-decode.cpp $(APP_SOURCES) $(APP_HEADERS)
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) -isystem "$(METAL_CPP)" $(CXXFLAGS) -UNDEBUG -I. -fblocks \
		$(filter-out main.cpp,$(APP_SOURCES)) $< \
		-framework Foundation -framework Metal -framework Security -o "$@"

verify-xtn-native: $(XTN_PROCESS_TEST)
	"$(XTN_PROCESS_TEST)"
	"$(XTN_PROCESS_TEST)" "$(BUILD_DIR)/xtn-lowering.metal"
	xcrun -sdk macosx metal -c "$(BUILD_DIR)/xtn-lowering.metal" -o "$(BUILD_DIR)/xtn-lowering.air"

$(UMAXP_PROCESS_TEST): ../fixtures/handoff/umaxp-process-decode.cpp $(APP_SOURCES) $(APP_HEADERS)
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) -isystem "$(METAL_CPP)" $(CXXFLAGS) -UNDEBUG -I. -fblocks \
		$(filter-out main.cpp,$(APP_SOURCES)) $< \
		-framework Foundation -framework Metal -framework Security -o "$@"

verify-umaxp-native: $(UMAXP_PROCESS_TEST)
	"$(UMAXP_PROCESS_TEST)"
	"$(UMAXP_PROCESS_TEST)" "$(BUILD_DIR)/umaxp-lowering.metal"
	xcrun -sdk macosx metal -c "$(BUILD_DIR)/umaxp-lowering.metal" -o "$(BUILD_DIR)/umaxp-lowering.air"


# CPU-only opcode sweep over Mach-O files and shared-cache images (no Metal).
SWEEP := $(BUILD_DIR)/arm64-metal-sweep
SWEEP_SOURCES := guest_strings.cpp guest_vector_crypto.cpp macho.cpp process_decoder.cpp process_msl.cpp shared_cache.cpp translate_core.cpp
$(SWEEP): tools/sweep.cpp $(SWEEP_SOURCES) $(APP_HEADERS)
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I. $(SWEEP_SOURCES) $< -o "$@"

.PHONY: sweep
sweep: $(SWEEP)

# Native-vs-GPU crypto-extension differential (GPU: run under bin/gpulock).
CRYPTO_EXT_NATIVE := $(BUILD_DIR)/crypto-ext-differential
$(CRYPTO_EXT_NATIVE): ../fixtures/handoff/crypto-ext-differential.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -O1 -fno-stack-protector "$<" -o "$@"

.PHONY: verify-crypto-ext
verify-crypto-ext: $(CRYPTO_EXT_NATIVE) $(APP)
	"$(CRYPTO_EXT_NATIVE)" >"$(BUILD_DIR)/crypto-ext.native.out"
	"$(APP)" launch "$(CRYPTO_EXT_NATIVE)" >"$(BUILD_DIR)/crypto-ext.gpu.out"
	cmp "$(BUILD_DIR)/crypto-ext.native.out" "$(BUILD_DIR)/crypto-ext.gpu.out"

DIT_NATIVE := $(BUILD_DIR)/dit-differential
$(DIT_NATIVE): ../fixtures/handoff/dit-differential.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -O1 -fno-stack-protector "$<" -o "$@"

.PHONY: verify-dit
verify-dit: $(DIT_NATIVE) $(APP)
	"$(DIT_NATIVE)" >"$(BUILD_DIR)/dit.native.out"
	"$(APP)" launch "$(DIT_NATIVE)" >"$(BUILD_DIR)/dit.gpu.out"
	cmp "$(BUILD_DIR)/dit.native.out" "$(BUILD_DIR)/dit.gpu.out"

LDNP_STNP_NATIVE := $(BUILD_DIR)/ldnp-stnp-differential
$(LDNP_STNP_NATIVE): ../fixtures/handoff/ldnp-stnp-differential.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -O1 -fno-stack-protector "$<" -o "$@"

.PHONY: verify-ldnp-stnp
verify-ldnp-stnp: $(LDNP_STNP_NATIVE) $(APP)
	"$(LDNP_STNP_NATIVE)" >"$(BUILD_DIR)/ldnp-stnp.native.out"
	"$(APP)" launch "$(LDNP_STNP_NATIVE)" >"$(BUILD_DIR)/ldnp-stnp.gpu.out"
	cmp "$(BUILD_DIR)/ldnp-stnp.native.out" "$(BUILD_DIR)/ldnp-stnp.gpu.out"

LDRAA_NATIVE := $(BUILD_DIR)/ldraa-differential
$(LDRAA_NATIVE): ../fixtures/handoff/ldraa-differential.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -O1 -fno-stack-protector "$<" -o "$@"

.PHONY: verify-ldraa
verify-ldraa: $(LDRAA_NATIVE) $(APP)
	"$(LDRAA_NATIVE)" >"$(BUILD_DIR)/ldraa.native.out"
	"$(APP)" launch "$(LDRAA_NATIVE)" >"$(BUILD_DIR)/ldraa.gpu.out"
	cmp "$(BUILD_DIR)/ldraa.native.out" "$(BUILD_DIR)/ldraa.gpu.out"

$(INTERPRETER_MOVE_WIDE_NATIVE): ../fixtures/handoff/typed-move-wide.s ../fixtures/handoff/typed-move-wide-main.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror $^ -o "$@"

$(INTERPRETER_ADD_SUB_NATIVE): ../fixtures/handoff/typed-add-sub-immediate.s ../fixtures/handoff/typed-add-sub-immediate-main.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror $^ -o "$@"

$(INTERPRETER_ADD_SUB_FLAGS_NATIVE): ../fixtures/handoff/typed-add-sub-flags.s ../fixtures/handoff/typed-add-sub-flags-main.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror $^ -o "$@"

$(INTERPRETER_MINIMAL_MAIN): ../fixtures/interpreter/minimal-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_RET_CHAIN_MAIN): ../fixtures/interpreter/ret-chain-bl-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

verify-interpreter-ret-chain-native: $(INTERPRETER_RET_CHAIN_MAIN)
	@set -eu; set +e; "$(INTERPRETER_RET_CHAIN_MAIN)"; rc=$$?; set -e; \
		test "$$rc" -eq 42 && test -z "$$(nm -u "$(INTERPRETER_RET_CHAIN_MAIN)")" || exit 1; \
		echo "Typed RET-chain native fixture exit=$$rc; imports=0"

verify-interpreter-ret-chain: $(APP) $(INTERPRETER_RET_CHAIN_MAIN) $(INTERPRETER_METALLIB)
	@set -eu; run_dir=$$(mktemp -d "$(BUILD_DIR)/ret-chain.XXXXXX"); \
	binary_sha=$$(shasum -a 256 "$(INTERPRETER_RET_CHAIN_MAIN)" | awk '{print $$1}'); \
	set +e; \
	"$(INTERPRETER_RET_CHAIN_MAIN)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
	"$(APP)" launch "$(INTERPRETER_RET_CHAIN_MAIN)" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
	ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 ARM64_METAL_TRACE_INTERPRETER_PACKETS=1 \
		"$(APP)" launch "$(INTERPRETER_RET_CHAIN_MAIN)" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
	set -e; \
	test "$$binary_sha" = "$$(shasum -a 256 "$(INTERPRETER_RET_CHAIN_MAIN)" | awk '{print $$1}')" && \
		test "$$native_rc" -eq 42 && test "$$translated_rc" -eq "$$native_rc" && \
		test "$$interpreter_rc" -eq "$$native_rc" && \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out" && \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out" && \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/interpreter.err" && \
		awk '/event=interpreter-block-ready/ { blocks++; for (i=1; i<=NF; ++i) if ($$i ~ /^detail=/) { block=$$i; sub(/^detail=/,"",block) } } \
		     /event=interpreter-dispatch/ { dispatches++; for (i=1; i<=NF; ++i) if ($$i ~ /^detail=/) { dispatch=$$i; sub(/^detail=/,"",dispatch) } } \
		     /event=interpreter-packet/ { packets++; for (i=1; i<=NF; ++i) { if ($$i == "detail=2") rets++; if ($$i == "detail=28") calls++ } } \
		     END { exit !(blocks == 1 && dispatches == 1 && block == 6 && dispatch == 6 && packets == 6 && rets == 2 && calls == 1) }' \
			"$$run_dir/interpreter.err" && \
		! grep -q 'event=compile-start' "$$run_dir/interpreter.err" || exit 1; \
	echo "Typed RET chain native/translated/AOT passed; sha256=$$binary_sha receipts=$$run_dir"

$(INTERPRETER_LDUMIN_LDUMAX_MAIN): ../fixtures/interpreter/ldumin-ldumax-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_CMEQ_ZERO_MAIN): ../fixtures/interpreter/cmeq-zero-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_VECTOR_EXTREME_MAIN): ../fixtures/interpreter/vector-extreme-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_VECTOR_MEMORY_MAIN): ../fixtures/interpreter/vector-memory-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_VECTOR_MEMORY_UNMAPPED_MAIN): ../fixtures/interpreter/vector-memory-unmapped-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

verify-interpreter-vector-extreme-native: $(INTERPRETER_VECTOR_EXTREME_MAIN)
	@set -eu; set +e; "$(INTERPRETER_VECTOR_EXTREME_MAIN)"; rc=$$?; set -e; \
		test "$$rc" -eq 42; echo "Vector min/max native guest exit=$$rc"

verify-interpreter-vector-extreme: $(APP) $(INTERPRETER_VECTOR_EXTREME_MAIN) $(INTERPRETER_METALLIB)
	@set -eu; run_dir=$$(mktemp -d "$(BUILD_DIR)/vector-extreme.XXXXXX"); \
	binary_sha=$$(shasum -a 256 "$(INTERPRETER_VECTOR_EXTREME_MAIN)" | awk '{print $$1}'); \
	set +e; \
	"$(INTERPRETER_VECTOR_EXTREME_MAIN)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
	"$(APP)" launch "$(INTERPRETER_VECTOR_EXTREME_MAIN)" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
	ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 ARM64_METAL_TRACE_INTERPRETER_PACKETS=1 \
		"$(APP)" launch "$(INTERPRETER_VECTOR_EXTREME_MAIN)" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
	set -e; \
	test "$$binary_sha" = "$$(shasum -a 256 "$(INTERPRETER_VECTOR_EXTREME_MAIN)" | awk '{print $$1}')" && \
		test "$$native_rc" -eq 42 && test "$$translated_rc" -eq "$$native_rc" && \
		test "$$interpreter_rc" -eq "$$native_rc" && \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out" && \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out" && \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/interpreter.err" && \
		awk '/event=interpreter-block-ready/ { blocks++; for (i=1; i<=NF; ++i) if ($$i ~ /^detail=/) { block=$$i; sub(/^detail=/,"",block) } } \
		     /event=interpreter-dispatch/ { dispatches++; for (i=1; i<=NF; ++i) if ($$i ~ /^detail=/) { dispatch=$$i; sub(/^detail=/,"",dispatch) } } \
		     /event=interpreter-packet/ { packets++; for (i=1; i<=NF; ++i) if ($$i == "detail=40") extreme++; else if ($$i == "detail=1") move++; else if ($$i == "detail=2") ret++ } \
		     END { exit !(blocks == 1 && dispatches == 1 && block == 23 && dispatch == 23 && packets == 23 && extreme == 21 && move == 1 && ret == 1) }' \
			"$$run_dir/interpreter.err"; \
		if grep -q 'event=compile-start' "$$run_dir/interpreter.err"; then exit 1; fi; \
	echo "Vector min/max native/translated/AOT fixture passed; sha256=$$binary_sha receipts=$$run_dir"

verify-interpreter-vector-memory-native: $(INTERPRETER_VECTOR_MEMORY_MAIN)
	@set -eu; set +e; "$(INTERPRETER_VECTOR_MEMORY_MAIN)"; rc=$$?; set -e; \
		test "$$rc" -eq 42; echo "Single-vector memory native guest exit=$$rc"

# Standalone fixtures map Mach-O text file offsets from minimum_image_address.
verify-interpreter-vector-memory: $(APP) $(INTERPRETER_VECTOR_MEMORY_MAIN) $(INTERPRETER_VECTOR_MEMORY_UNMAPPED_MAIN) $(INTERPRETER_METALLIB)
	@set -eu; run_dir=$$(mktemp -d "$(BUILD_DIR)/vector-memory.XXXXXX"); \
	binary_sha=$$(shasum -a 256 "$(INTERPRETER_VECTOR_MEMORY_MAIN)" | awk '{print $$1}'); \
	entry_addr=$$(xcrun llvm-objdump --disassemble --macho "$(INTERPRETER_VECTOR_MEMORY_MAIN)" | awk '/^_main:/ {main=1; next} main && /^[0-9a-f]+:/ {gsub(":", "", $$1); print $$1; exit}'); \
	fallback_addr=$$(xcrun llvm-objdump --disassemble --macho "$(INTERPRETER_VECTOR_MEMORY_MAIN)" | awk '/^_vector_memory_fallback:/ {fallback=1; next} fallback && /^[0-9a-f]+:/ {gsub(":", "", $$1); print $$1; exit}'); \
	text_mapping=$$(otool -l "$(INTERPRETER_VECTOR_MEMORY_MAIN)" | awk '/sectname __text/ {text=1; next} text && $$1 == "addr" {address=$$2} text && $$1 == "offset" {print address, $$2; exit}'); \
	set -- $$text_mapping; text_vmaddr=$$1; text_fileoff=$$2; \
	test -n "$$entry_addr" && test -n "$$fallback_addr" && test -n "$$text_vmaddr" && test -n "$$text_fileoff"; \
	guest_entry=$$(printf '0x%x' $$((0x4000 + text_fileoff + 0x$$entry_addr - text_vmaddr))); \
	fallback_guest=$$(printf '0x%x' $$((0x4000 + text_fileoff + 0x$$fallback_addr - text_vmaddr))); \
	unmapped_entry_addr=$$(xcrun llvm-objdump --disassemble --macho "$(INTERPRETER_VECTOR_MEMORY_UNMAPPED_MAIN)" | awk '/^_main:/ {main=1; next} main && /^[0-9a-f]+:/ {gsub(":", "", $$1); print $$1; exit}'); \
	unmapped_load_addr=$$(xcrun llvm-objdump --disassemble --macho "$(INTERPRETER_VECTOR_MEMORY_UNMAPPED_MAIN)" | awk '/^_vector_memory_fault_load:/ {fault=1; next} fault && /^[0-9a-f]+:/ {gsub(":", "", $$1); print $$1; exit}'); \
	unmapped_text_mapping=$$(otool -l "$(INTERPRETER_VECTOR_MEMORY_UNMAPPED_MAIN)" | awk '/sectname __text/ {text=1; next} text && $$1 == "addr" {address=$$2} text && $$1 == "offset" {print address, $$2; exit}'); \
	set -- $$unmapped_text_mapping; unmapped_text_vmaddr=$$1; unmapped_text_fileoff=$$2; \
	test -n "$$unmapped_entry_addr" && test -n "$$unmapped_load_addr" && test -n "$$unmapped_text_vmaddr" && test -n "$$unmapped_text_fileoff"; \
	unmapped_guest_entry=$$(printf '0x%x' $$((0x4000 + unmapped_text_fileoff + 0x$$unmapped_entry_addr - unmapped_text_vmaddr))); \
	unmapped_guest_load=$$(printf '0x%x' $$((0x4000 + unmapped_text_fileoff + 0x$$unmapped_load_addr - unmapped_text_vmaddr))); \
	set +e; \
	"$(INTERPRETER_VECTOR_MEMORY_MAIN)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
	"$(APP)" launch "$(INTERPRETER_VECTOR_MEMORY_MAIN)" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
	ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 ARM64_METAL_TRACE_INTERPRETER_PACKETS=1 \
		"$(APP)" launch "$(INTERPRETER_VECTOR_MEMORY_MAIN)" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
	set -e; \
	if test "$$binary_sha" = "$$(shasum -a 256 "$(INTERPRETER_VECTOR_MEMORY_MAIN)" | awk '{print $$1}')" && \
		test "$$native_rc" -eq 42 && test "$$translated_rc" -eq "$$native_rc" && \
		test "$$interpreter_rc" -eq "$$native_rc" && \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out" && \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out" && \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err" && \
		awk -v entry="$$guest_entry" -v fallback="$$fallback_guest" ' \
			/event=interpreter-packet/ { if (/detail=43([[:space:]]|$$)/) load++; if (/detail=44([[:space:]]|$$)/) store++ } \
			/event=interpreter-dispatch/ && index($$0, "pc=" entry " ") && !entry_dispatch { entry_dispatch=NR } \
			/event=dispatch-done/ && index($$0, "pc=" fallback " ") && !fallback_done && !first_compile { fallback_done=NR } \
			/event=compile-start/ { if (!first_compile) { first_compile=NR; if (index($$0, "pc=" fallback " ")) fallback_compile=NR } } \
			END { exit !(load == 16 && store == 20 && entry_dispatch > 0 && fallback_done > entry_dispatch && fallback_compile > fallback_done && first_compile == fallback_compile) }' \
			"$$run_dir/interpreter.err"; then \
		set +e; \
		(ulimit -c 0; ARM64_METAL_TRACE_FATAL_SIGNAL=1 ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 ARM64_METAL_TRACE_INTERPRETER_PACKETS=1 \
			"$(APP)" launch "$(INTERPRETER_VECTOR_MEMORY_UNMAPPED_MAIN)" >"$$run_dir/unmapped.out" 2>"$$run_dir/unmapped.err"); unmapped_rc=$$?; \
		set -e; \
		if test "$$unmapped_rc" -eq 139 && \
			grep -q "interpreter-dispatch.*pc=$$unmapped_guest_entry" "$$run_dir/unmapped.err" && \
			grep -q "interpreter-packet pc=$$unmapped_guest_load detail=43" "$$run_dir/unmapped.err" && \
			grep -q 'Guest process memory access fault at 0x0' "$$run_dir/unmapped.err"; then \
			echo "Single-vector memory native/translated/AOT and fault fixtures passed; sha256=$$binary_sha entry=$$guest_entry fallback=$$fallback_guest fault-entry=$$unmapped_guest_entry fault-load=$$unmapped_guest_load receipts=$$run_dir"; \
		else \
			cat "$$run_dir/unmapped.err" >&2; echo "Unmapped vector-load fault fixture failed: rc=$$unmapped_rc receipts=$$run_dir" >&2; exit 1; \
		fi; \
	else \
		cat "$$run_dir/native.err" "$$run_dir/translated.err" "$$run_dir/interpreter.err" >&2; \
		echo "Single-vector memory fixture failed: native=$$native_rc translated=$$translated_rc interpreter=$$interpreter_rc entry=0x$$entry_addr fallback=0x$$fallback_addr receipts=$$run_dir" >&2; exit 1; \
	fi

verify-interpreter-cmeq-zero-native: $(INTERPRETER_CMEQ_ZERO_MAIN)
	@set -eu; set +e; "$(INTERPRETER_CMEQ_ZERO_MAIN)"; rc=$$?; set -e; \
		test "$$rc" -eq 42; echo "CMEQ-zero native guest exit=$$rc"

verify-interpreter-cmeq-zero: $(APP) $(INTERPRETER_CMEQ_ZERO_MAIN) $(INTERPRETER_METALLIB)
	@set -eu; run_dir=$$(mktemp -d "$(BUILD_DIR)/cmeq-zero.XXXXXX"); \
	binary_sha=$$(shasum -a 256 "$(INTERPRETER_CMEQ_ZERO_MAIN)" | awk '{print $$1}'); \
	set +e; \
	"$(INTERPRETER_CMEQ_ZERO_MAIN)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
	"$(APP)" launch "$(INTERPRETER_CMEQ_ZERO_MAIN)" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
	ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 ARM64_METAL_TRACE_INTERPRETER_PACKETS=1 \
		"$(APP)" launch "$(INTERPRETER_CMEQ_ZERO_MAIN)" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
	set -e; \
	test "$$binary_sha" = "$$(shasum -a 256 "$(INTERPRETER_CMEQ_ZERO_MAIN)" | awk '{print $$1}')" && \
		test "$$native_rc" -eq 42 && test "$$translated_rc" -eq "$$native_rc" && \
		test "$$interpreter_rc" -eq "$$native_rc" && \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out" && \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out" && \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/interpreter.err" && \
		awk '/event=interpreter-block-ready/ { blocks++; for (i=1; i<=NF; ++i) if ($$i ~ /^detail=/) { block=$$i; sub(/^detail=/,"",block) } } \
		     /event=interpreter-dispatch/ { dispatches++; for (i=1; i<=NF; ++i) if ($$i ~ /^detail=/) { dispatch=$$i; sub(/^detail=/,"",dispatch) } } \
		     /event=interpreter-packet/ { packets++; for (i=1; i<=NF; ++i) if ($$i == "detail=39") cmeq++; else if ($$i == "detail=1") move++; else if ($$i == "detail=2") ret++ } \
		     END { exit !(blocks == 1 && dispatches == 1 && block == 10 && dispatch == 10 && packets == 10 && cmeq == 8 && move == 1 && ret == 1) }' \
			"$$run_dir/interpreter.err"; \
		if grep -q 'event=compile-start' "$$run_dir/interpreter.err"; then exit 1; fi; \
	echo "CMEQ-zero native/translated/AOT fixture passed; sha256=$$binary_sha receipts=$$run_dir"

verify-interpreter-ldumin-ldumax-native: $(INTERPRETER_LDUMIN_LDUMAX_MAIN)
	@set -eu; set +e; "$(INTERPRETER_LDUMIN_LDUMAX_MAIN)"; rc=$$?; set -e; \
		test "$$rc" -eq 42; echo "LDUMIN/LDUMAX native guest exit=$$rc"

verify-interpreter-ldumin-ldumax: $(APP) $(INTERPRETER_LDUMIN_LDUMAX_MAIN) $(INTERPRETER_METALLIB)
	@set -eu; run_dir=$$(mktemp -d "$(BUILD_DIR)/ldumin-ldumax.XXXXXX"); \
	binary_sha=$$(shasum -a 256 "$(INTERPRETER_LDUMIN_LDUMAX_MAIN)" | awk '{print $$1}'); \
	set +e; \
	"$(INTERPRETER_LDUMIN_LDUMAX_MAIN)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
	"$(APP)" launch "$(INTERPRETER_LDUMIN_LDUMAX_MAIN)" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
	ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 ARM64_METAL_TRACE_INTERPRETER_PACKETS=1 \
		"$(APP)" launch "$(INTERPRETER_LDUMIN_LDUMAX_MAIN)" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
	set -e; \
	test "$$binary_sha" = "$$(shasum -a 256 "$(INTERPRETER_LDUMIN_LDUMAX_MAIN)" | awk '{print $$1}')" && \
		test "$$native_rc" -eq 42 && test "$$translated_rc" -eq "$$native_rc" && \
		test "$$interpreter_rc" -eq "$$native_rc" && \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out" && \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out" && \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/interpreter.err" && \
		awk '/event=interpreter-block-ready/ { blocks++; for (i=1; i<=NF; ++i) if ($$i ~ /^detail=/) { block=$$i; sub(/^detail=/,"",block) } } \
		     /event=interpreter-dispatch/ { dispatches++; for (i=1; i<=NF; ++i) if ($$i ~ /^detail=/) { dispatch=$$i; sub(/^detail=/,"",dispatch) } } \
		     /event=interpreter-packet/ { packets++; for (i=1; i<=NF; ++i) if ($$i == "detail=37") atomics++ } \
		     END { exit !(blocks == 1 && dispatches == 1 && block > 0 && dispatch == block && packets == block && atomics == 7) }' \
			"$$run_dir/interpreter.err" && \
		! grep -q 'event=compile-start' "$$run_dir/interpreter.err" || exit 1; \
	echo "LDUMIN/LDUMAX native/translated/AOT fixture passed; sha256=$$binary_sha receipts=$$run_dir"

$(INTERPRETER_MOVN_MOVK_MAIN): ../fixtures/interpreter/movn-movk-entry.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_ADD_SUB_MAIN): ../fixtures/interpreter/add-sub-immediate-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_ADD_SUB_SP_MAIN): ../fixtures/interpreter/add-sub-sp-flags-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_BCOND_MAIN): ../fixtures/interpreter/bcond-nzcv-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_CSEL_MAIN): ../fixtures/interpreter/csel-family-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_ADD_SUB_SHIFTED_MAIN): ../fixtures/interpreter/add-sub-shifted-register-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_ADD_SUB_EXTENDED_MAIN): ../fixtures/interpreter/add-sub-extended-register-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_LOGICAL_SHIFTED_MAIN): ../fixtures/interpreter/logical-shifted-register-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_LOGICAL_VARIABLE_SHIFTS_MAIN): ../fixtures/interpreter/logical-variable-shifts-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_MULTIPLY_ACCUMULATE_MAIN): ../fixtures/interpreter/multiply-accumulate-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_BITFIELD_MAIN): ../fixtures/interpreter/bitfield-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_BITFIELD_NATIVE): ../fixtures/interpreter/bitfield-main.s ../fixtures/interpreter/bitfield-native-main.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -DARM64_GUEST_NATIVE_TEST=1 -Wall -Wextra -Werror -x assembler-with-cpp ../fixtures/interpreter/bitfield-main.s -x none ../fixtures/interpreter/bitfield-native-main.c -o "$@"

$(INTERPRETER_INTEGER_BITOPS_MAIN): ../fixtures/interpreter/integer-bitops-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_INTEGER_DIVIDE_MAIN): ../fixtures/interpreter/integer-divide-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_LITERAL_LOAD_MAIN): ../fixtures/interpreter/literal-load-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

verify-interpreter-literal-load-native: $(INTERPRETER_LITERAL_LOAD_MAIN)
	@set +e; "$(INTERPRETER_LITERAL_LOAD_MAIN)"; result=$$?; set -e; test "$$result" -eq 42

$(INTERPRETER_LOGICAL_IMMEDIATE_MAIN): ../fixtures/interpreter/logical-immediate-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_LOGICAL_IMMEDIATE_INVALID_MAIN): ../fixtures/interpreter/logical-immediate-invalid-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_BRANCH_MAIN): ../fixtures/interpreter/branch-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_BRANCH_FALLBACK_MAIN): ../fixtures/interpreter/branch-fallback-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_BL_NATIVE): ../fixtures/interpreter/bl-nested-main.s ../fixtures/interpreter/bl-nested-functions.s ../fixtures/interpreter/bl-nested-native-main.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -DARM64_GUEST_NATIVE_TEST=1 -Wall -Wextra -Werror -x assembler-with-cpp ../fixtures/interpreter/bl-nested-main.s -x none ../fixtures/interpreter/bl-nested-functions.s ../fixtures/interpreter/bl-nested-native-main.c -o "$@"


$(INTERPRETER_BL_MAIN): ../fixtures/interpreter/bl-nested-main.s ../fixtures/interpreter/bl-nested-functions.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main -x assembler-with-cpp ../fixtures/interpreter/bl-nested-main.s -x none ../fixtures/interpreter/bl-nested-functions.s -lSystem -o "$@"

$(INTERPRETER_BL_UNPLANNED_NATIVE): ../fixtures/interpreter/bl-unplanned-main.s ../fixtures/interpreter/bl-unplanned-functions.s ../fixtures/interpreter/bl-unplanned-native-main.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -DARM64_GUEST_NATIVE_TEST=1 -Wall -Wextra -Werror -x assembler-with-cpp ../fixtures/interpreter/bl-unplanned-main.s -x none ../fixtures/interpreter/bl-unplanned-functions.s ../fixtures/interpreter/bl-unplanned-native-main.c -o "$@"


$(INTERPRETER_BL_UNPLANNED_MAIN): ../fixtures/interpreter/bl-unplanned-main.s ../fixtures/interpreter/bl-unplanned-functions.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main -x assembler-with-cpp ../fixtures/interpreter/bl-unplanned-main.s -x none ../fixtures/interpreter/bl-unplanned-functions.s -lSystem -o "$@"

$(INTERPRETER_BRANCH_REGISTER_NATIVE): ../fixtures/interpreter/branch-register-main.s ../fixtures/interpreter/branch-register-native-main.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -DARM64_GUEST_NATIVE_TEST=1 -Wall -Wextra -Werror -x assembler-with-cpp ../fixtures/interpreter/branch-register-main.s -x none ../fixtures/interpreter/branch-register-native-main.c -o "$@"

$(INTERPRETER_BRANCH_REGISTER_MAIN): ../fixtures/interpreter/branch-register-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main -x assembler-with-cpp "$<" -lSystem -o "$@"

$(INTERPRETER_BRANCH_REGISTER_IMPORT_NATIVE): ../fixtures/interpreter/branch-register-import-main.s ../fixtures/interpreter/branch-register-import-native-main.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -DARM64_GUEST_NATIVE_TEST=1 -Wall -Wextra -Werror -x assembler-with-cpp ../fixtures/interpreter/branch-register-import-main.s -x none ../fixtures/interpreter/branch-register-import-native-main.c -o "$@"

$(INTERPRETER_BRANCH_REGISTER_IMPORT_MAIN): ../fixtures/interpreter/branch-register-import-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main -x assembler-with-cpp "$<" -lSystem -o "$@"

$(INTERPRETER_TYPED_MEMORY_MAIN): ../fixtures/interpreter/typed-memory-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_TYPED_MEMORY_RO_MAIN): ../fixtures/interpreter/typed-memory-ro-store.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_TYPED_MEMORY_UNMAPPED_MAIN): ../fixtures/interpreter/typed-memory-unmapped-load.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_PAIR_MEMORY_MAIN): ../fixtures/interpreter/pair-memory-stack-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_SCALAR_INDEXED_MEMORY_MAIN): ../fixtures/interpreter/scalar-memory-indexed-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_CBZ_CBNZ_MAIN): ../fixtures/interpreter/cbz-cbnz-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_TBZ_TBNZ_MAIN): ../fixtures/interpreter/tbz-tbnz-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_ADR_ADRP_MAIN): ../fixtures/interpreter/adr-adrp-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_ADC_SBC_CARRY_MAIN): ../fixtures/interpreter/adc-sbc-carry-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_SCALAR_FP_MAIN): ../fixtures/interpreter/scalar-fp-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_SCALAR_JSCVT_MAIN): ../fixtures/interpreter/scalar-jscvt-main.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -O0 -Wall -Wextra -Werror -pedantic "$<" -o "$@"

$(INTERPRETER_SCALAR_FMOV_D_MAIN): ../fixtures/interpreter/scalar-fmov-d-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_SCALAR_FP_COMPARE_MAIN): ../fixtures/interpreter/scalar-fp-compare-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(BUILD_DIR)/interpreter-scalar-fp-compare-case-%: ../fixtures/interpreter/scalar-fp-compare-cases.S
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main -DFP_CASE=$* -x assembler-with-cpp "$<" -lSystem -o "$@"

verify-scalar-fp-native: $(INTERPRETER_TYPED_NATIVE) $(INTERPRETER_SCALAR_FP_MAIN)
	@set -eu; \
		"$(INTERPRETER_TYPED_NATIVE)"; \
		set +e; "$(INTERPRETER_SCALAR_FP_MAIN)"; status=$$?; set -e; \
		test "$$status" -eq 42; \
		echo "scalar-fp native guest exit=$$status"

.PHONY: verify-scalar-fmov-d-native verify-scalar-fmov-d-interpreter
verify-scalar-fmov-d-native: $(INTERPRETER_TYPED_NATIVE) $(INTERPRETER_SCALAR_FMOV_D_MAIN)
	@set -eu; \
		"$(INTERPRETER_TYPED_NATIVE)"; \
		set +e; "$(INTERPRETER_SCALAR_FMOV_D_MAIN)"; status=$$?; set -e; \
		test "$$status" -eq 42; \
		echo "scalar-fmov-d native guest exit=$$status"

verify-scalar-fmov-d-interpreter: $(APP) $(INTERPRETER_SCALAR_FMOV_D_MAIN) $(INTERPRETER_METALLIB)
	@set -eu; \
		run_dir=$$(mktemp -d "$(BUILD_DIR)/scalar-fmov-d-interpreter.XXXXXX"); \
		set +e; \
		"$(INTERPRETER_SCALAR_FMOV_D_MAIN)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_SCALAR_FMOV_D_MAIN)" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_SCALAR_FMOV_D_MAIN)" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
		set -e; \
		test "$$native_rc" -eq 42 && test "$$translated_rc" -eq 42 && test "$$interpreter_rc" -eq 42 && \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out" && \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out" && \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/interpreter.err" && \
		grep -q 'interpreter-dispatch' "$$run_dir/interpreter.err" && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/interpreter.err" && \
		echo "scalar-fmov-d native/translated/interpreter verified: $$run_dir"

verify-scalar-fp-interpreter: $(APP) $(INTERPRETER_SCALAR_FP_MAIN) $(INTERPRETER_METALLIB)
	@set -eu; \
		run_dir=$$(mktemp -d "$(BUILD_DIR)/scalar-fp-interpreter.XXXXXX"); \
		set +e; \
		"$(INTERPRETER_SCALAR_FP_MAIN)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_SCALAR_FP_MAIN)" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_SCALAR_FP_MAIN)" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
		set -e; \
		test "$$native_rc" -eq 42 && test "$$translated_rc" -eq 42 && test "$$interpreter_rc" -eq 42 && \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out" && \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out" && \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/interpreter.err" && \
		grep -q 'interpreter-dispatch' "$$run_dir/interpreter.err" && \
		test "$$(grep -c 'event=interpreter-dispatch' "$$run_dir/interpreter.err")" -eq 1 && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/interpreter.err" && \
		echo "scalar-fp native/translated/interpreter verified: $$run_dir"

.PHONY: verify-scalar-jscvt-interpreter
verify-scalar-jscvt-interpreter: $(APP) $(INTERPRETER_SCALAR_JSCVT_MAIN) $(INTERPRETER_METALLIB)
	@set -eu; \
		run_dir=$$(mktemp -d "$(BUILD_DIR)/scalar-jscvt-interpreter.XXXXXX"); \
		set +e; \
		"$(INTERPRETER_SCALAR_JSCVT_MAIN)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_SCALAR_JSCVT_MAIN)" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
		$(if $(GPU_LOCK),$(GPU_LOCK) )env ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_SCALAR_JSCVT_MAIN)" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
		set -e; \
		test "$$native_rc" -eq 0 && test "$$translated_rc" -eq 0 && test "$$interpreter_rc" -eq 0 && \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out" && \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out" && \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/interpreter.err" && \
		grep -q 'interpreter-dispatch' "$$run_dir/interpreter.err" && \
		echo "scalar-jscvt native/translated/interpreter verified: $$run_dir"

verify-scalar-fp-compare-native: $(INTERPRETER_TYPED_NATIVE) $(INTERPRETER_SCALAR_FP_COMPARE_MAIN) $(INTERPRETER_SCALAR_FP_COMPARE_CASES)
	@set -eu; \
		"$(INTERPRETER_TYPED_NATIVE)"; \
		set +e; "$(INTERPRETER_SCALAR_FP_COMPARE_MAIN)"; status=$$?; set -e; \
		test "$$status" -eq 42; \
		for fixture in $(INTERPRETER_SCALAR_FP_COMPARE_CASES); do \
			set +e; "$$fixture"; case_status=$$?; set -e; \
			test "$$case_status" -eq 42; \
		done; \
		echo "scalar-fp-compare native guest exit=$$status"

verify-scalar-fp-compare-interpreter: $(APP) $(INTERPRETER_SCALAR_FP_COMPARE_MAIN) $(INTERPRETER_METALLIB)
	@set -eu; \
		run_dir=$$(mktemp -d "$(BUILD_DIR)/scalar-fp-compare-interpreter.XXXXXX"); \
		set +e; \
		"$(INTERPRETER_SCALAR_FP_COMPARE_MAIN)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_SCALAR_FP_COMPARE_MAIN)" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_SCALAR_FP_COMPARE_MAIN)" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
		set -e; \
		test "$$native_rc" -eq 42 && test "$$translated_rc" -eq 42 && test "$$interpreter_rc" -eq 42 && \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out" && \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out" && \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/interpreter.err" && \
		grep -q 'interpreter-dispatch' "$$run_dir/interpreter.err" && \
		test "$$(grep -c 'event=interpreter-dispatch' "$$run_dir/interpreter.err")" -eq 1 && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/interpreter.err" && \
		echo "scalar-fp-compare native/translated/interpreter verified: $$run_dir"

diagnose-scalar-fp-compare-translated: $(APP) $(INTERPRETER_SCALAR_FP_COMPARE_CASES)
	@set -eu; \
		run_dir=$$(mktemp -d "$(BUILD_DIR)/scalar-fp-compare-translated.XXXXXX"); \
		failures=0; \
		for fixture in $(INTERPRETER_SCALAR_FP_COMPARE_CASES); do \
			case_id=$${fixture##*-}; \
			set +e; "$$fixture" >"$$run_dir/case-$$case_id.native.out" 2>"$$run_dir/case-$$case_id.native.err"; native_rc=$$?; \
			"$(APP)" launch "$$fixture" >"$$run_dir/case-$$case_id.translated.out" 2>"$$run_dir/case-$$case_id.translated.err"; translated_rc=$$?; set -e; \
			printf '%s\n' "$$native_rc" >"$$run_dir/case-$$case_id.native.rc"; \
			printf '%s\n' "$$translated_rc" >"$$run_dir/case-$$case_id.translated.rc"; \
			printf '%s native=%s translated=%s\n' "$$case_id" "$$native_rc" "$$translated_rc" >>"$$run_dir/statuses.txt"; \
			if test "$$native_rc" -ne 42 || test "$$translated_rc" -ne 42; then failures=1; fi; \
			if test "$$native_rc" -eq 42 && test "$$translated_rc" -eq 42; then \
				if ! cmp "$$run_dir/case-$$case_id.native.out" "$$run_dir/case-$$case_id.translated.out" || \
				   ! cmp "$$run_dir/case-$$case_id.native.err" "$$run_dir/case-$$case_id.translated.err"; then failures=1; fi; \
			fi; \
		done; \
		cat "$$run_dir/statuses.txt"; \
		test "$$failures" -eq 0; \
		echo "scalar-fp compare translated cases recorded: $$run_dir"

$(ADC_SBC_CARRY_DECODE_TEST): ../fixtures/handoff/adc-sbc-carry-decode.cpp process_decoder.cpp process_decoder.hpp translate_core.cpp translate_core.hpp integer_ops_shared.h guest_strings.cpp guest_strings.hpp guest_vector_crypto.cpp guest_vector_crypto.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I. ../fixtures/handoff/adc-sbc-carry-decode.cpp process_decoder.cpp guest_strings.cpp guest_vector_crypto.cpp translate_core.cpp -o "$@"

$(GUEST_FORMAT_NATIVE): ../fixtures/handoff/guest-printf-formats.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -Wall -Wextra -Werror "$<" -o "$@"

$(GUEST_FORMAT_TEST): ../fixtures/handoff/guest-format-test.cpp guest_format.cpp guest_format.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG ../fixtures/handoff/guest-format-test.cpp guest_format.cpp -o "$@"

verify-guest-format: $(GUEST_FORMAT_NATIVE) $(GUEST_FORMAT_TEST)
	"$(GUEST_FORMAT_NATIVE)" >"$(BUILD_DIR)/guest-printf-formats.native.out"
	printf '%s\n' '1 2 3 4 5 6|0a|0x123456789abcdef|4294967301|0|-0042' >"$(BUILD_DIR)/guest-printf-formats.expected.out"
	cmp "$(BUILD_DIR)/guest-printf-formats.expected.out" "$(BUILD_DIR)/guest-printf-formats.native.out"
	"$(GUEST_FORMAT_TEST)"

GUEST_IMAGE_REGISTRY_TEST := $(BUILD_DIR)/guest-image-registry-test
$(GUEST_IMAGE_REGISTRY_TEST): ../fixtures/handoff/guest-image-registry-test.cpp guest_image_registry.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) ../fixtures/handoff/guest-image-registry-test.cpp -o "$@"

verify-guest-image-registry: $(GUEST_IMAGE_REGISTRY_TEST)
	"$(GUEST_IMAGE_REGISTRY_TEST)"

GUEST_EXIT_REGISTRY_TEST := $(BUILD_DIR)/guest-exit-registry-test
$(GUEST_EXIT_REGISTRY_TEST): ../fixtures/handoff/guest-exit-registry-test.cpp guest_exit.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) ../fixtures/handoff/guest-exit-registry-test.cpp -o "$@"

verify-guest-exit-registry: $(GUEST_EXIT_REGISTRY_TEST)
	"$(GUEST_EXIT_REGISTRY_TEST)"

DYLD_FINALIZER_FIXTURE := $(BUILD_DIR)/libdyld-finalizer-metadata.dylib
DYLD_FINALIZER_METADATA_TEST := $(BUILD_DIR)/dyld-finalizer-metadata-test
$(DYLD_FINALIZER_FIXTURE): ../fixtures/handoff/dyld-finalizer-metadata.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -dynamiclib -fPIC -Wl,-fixup_chains \
		-Wl,-install_name,@rpath/libdyld-finalizer-metadata.dylib \
		-Wall -Wextra -Werror "$<" -o "$@"

$(DYLD_FINALIZER_METADATA_TEST): ../fixtures/handoff/dyld-finalizer-metadata-test.cpp \
		image_loader.cpp macho.cpp shared_cache.cpp guest_code_policy.cpp guest_tlv.cpp \
		guest_state.cpp guest_format.cpp guest_scheduler.cpp guest_strings.cpp \
		guest_vector_crypto.cpp translate_core.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I. "$<" image_loader.cpp macho.cpp \
		shared_cache.cpp guest_code_policy.cpp guest_tlv.cpp guest_state.cpp guest_format.cpp \
		guest_scheduler.cpp guest_strings.cpp guest_vector_crypto.cpp translate_core.cpp \
		-framework Foundation -framework Security -framework CoreFoundation -o "$@"

verify-dyld-finalizer-metadata-native: $(DYLD_FINALIZER_FIXTURE) $(DYLD_FINALIZER_METADATA_TEST)
	"$(DYLD_FINALIZER_METADATA_TEST)" "$(DYLD_FINALIZER_FIXTURE)" \
		>"$(BUILD_DIR)/dyld-finalizer-metadata.out" \
		2>"$(BUILD_DIR)/dyld-finalizer-metadata.err"
	printf '%s\n' 'native-init' 'native-cxa-final' \
		'default_reject=1 metadata initializers=2 finalizers=1 finalizer_text=1 native_value=42' \
		>"$(BUILD_DIR)/dyld-finalizer-metadata.expected"
	cmp "$(BUILD_DIR)/dyld-finalizer-metadata.expected" \
		"$(BUILD_DIR)/dyld-finalizer-metadata.out"
	test ! -s "$(BUILD_DIR)/dyld-finalizer-metadata.err"

$(INTERPRETER_DEMAND_CACHE_GENERATOR): ../fixtures/interpreter/demand-cache-retry-address.cpp shared_cache.cpp macho.cpp shared_cache.hpp macho.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) ../fixtures/interpreter/demand-cache-retry-address.cpp shared_cache.cpp macho.cpp -o "$@"

$(INTERPRETER_DEMAND_CACHE_SOURCE): $(INTERPRETER_DEMAND_CACHE_GENERATOR)
	@mkdir -p "$(dir $@)"
	"$(INTERPRETER_DEMAND_CACHE_GENERATOR)" >"$@"

$(INTERPRETER_DEMAND_CACHE_MAIN): $(INTERPRETER_DEMAND_CACHE_SOURCE)
	xcrun clang -arch arm64 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_NATIVE): ../fixtures/interpreter/demand-cache-retry-native.c
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -DPAIR_LOAD=1 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_SCALAR_SOURCE): $(INTERPRETER_DEMAND_CACHE_GENERATOR)
	@mkdir -p "$(dir $@)"
	"$(INTERPRETER_DEMAND_CACHE_GENERATOR)" scalar >"$@"

$(INTERPRETER_DEMAND_CACHE_SCALAR_MAIN): $(INTERPRETER_DEMAND_CACHE_SCALAR_SOURCE)
	xcrun clang -arch arm64 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_SCALAR_NATIVE): ../fixtures/interpreter/demand-cache-retry-native.c
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -DPAIR_LOAD=0 "$<" -lz -o "$@"

$(MEMCPY_CHK_NATIVE): ../fixtures/handoff/memcpy-chk.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror "$<" -o "$@"

$(MEMCPY_CHK_SIGINFO_NATIVE): ../fixtures/handoff/memcpy-chk-siginfo.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -O0 -fno-builtin -fno-stack-protector -Wall -Wextra -Werror "$<" -o "$@"

$(MEMCPY_CHK_DECODE_TEST): ../fixtures/handoff/memcpy-chk-decode.cpp process_decoder.cpp process_decoder.hpp host_service_registry.hpp guest_strings.cpp guest_strings.hpp guest_vector_crypto.cpp guest_vector_crypto.hpp translate_core.cpp translate_core.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG ../fixtures/handoff/memcpy-chk-decode.cpp process_decoder.cpp guest_strings.cpp guest_vector_crypto.cpp translate_core.cpp -o "$@"

$(MEMSET_CHK_NATIVE): ../fixtures/handoff/memset-chk.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror "$<" -o "$@"

$(MEMSET_CHK_SIGINFO_NATIVE): ../fixtures/handoff/memset-chk-siginfo.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -O0 -fno-builtin -fno-stack-protector -Wall -Wextra -Werror "$<" -o "$@"

$(MEMSET_CHK_DECODE_TEST): ../fixtures/handoff/memset-chk-decode.cpp process_decoder.cpp process_decoder.hpp host_service_registry.hpp guest_strings.cpp guest_strings.hpp guest_vector_crypto.cpp guest_vector_crypto.hpp translate_core.cpp translate_core.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG ../fixtures/handoff/memset-chk-decode.cpp process_decoder.cpp guest_strings.cpp guest_vector_crypto.cpp translate_core.cpp -o "$@"

$(MACH_VM_OVERWRITE_NATIVE): ../fixtures/handoff/mach-vm-map-overwrite.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

$(BUILD_DIR)/mach-vm-sparse-reservation-native: ../fixtures/handoff/mach-vm-sparse-reservation.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

$(BUILD_DIR)/mach-vm-region-native: ../fixtures/handoff/mach-vm-region.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

$(MACH_VM_DEFERRED_RECLAMATION_NATIVE): ../fixtures/handoff/mach-vm-deferred-reclamation.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

$(MACH_VM_MEMORY_ENTRY_NATIVE): ../fixtures/handoff/mach-vm-memory-entry.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

$(MACH_VM_MEMORY_ENTRY_INVALID_OUTPUT): ../fixtures/handoff/mach-vm-memory-entry-invalid-output.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

.PHONY: verify-mach-vm-memory-entry-native
verify-mach-vm-memory-entry-native: $(MACH_VM_MEMORY_ENTRY_NATIVE)
	"$(MACH_VM_MEMORY_ENTRY_NATIVE)"
$(GUEST_HEAP_ALLOCATE_AT_TEST): ../fixtures/handoff/guest-heap-allocate-at.cpp guest_state.cpp guest_state.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG "$<" guest_state.cpp -o "$@"

$(CACHE_BACKING_LEASES_TEST): ../fixtures/handoff/cache-backing-leases.cpp cache_backing_leases.hpp guest_state.cpp guest_state.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG "$<" guest_state.cpp -o "$@"

$(VM_BACKING_LEASE_TEST): vm_backing_lease_test.cpp vm_backing_lease.hpp guest_state.cpp guest_state.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I. "$<" guest_state.cpp -o "$@"

.PHONY: verify-vm-backing-lease
verify-vm-backing-lease: $(VM_BACKING_LEASE_TEST)
	"$(VM_BACKING_LEASE_TEST)"

verify-mach-vm-overwrite-native: $(MACH_VM_OVERWRITE_NATIVE)
	"$(MACH_VM_OVERWRITE_NATIVE)" >"$(BUILD_DIR)/mach-vm-map-overwrite.native.out"
	printf '%s\n' 'mach_vm_map overwrite full=ok partial=ok hole=ok null=1 bad_flags=4' >"$(BUILD_DIR)/mach-vm-map-overwrite.expected.out"
	cmp "$(BUILD_DIR)/mach-vm-map-overwrite.expected.out" "$(BUILD_DIR)/mach-vm-map-overwrite.native.out"

verify-mach-vm-sparse-reservation-native: $(BUILD_DIR)/mach-vm-sparse-reservation-native
	"$(BUILD_DIR)/mach-vm-sparse-reservation-native" >"$(BUILD_DIR)/mach-vm-sparse-reservation.native.out"
	printf '%s\n' 'reserve_align32m=1 fixed_24g=0 fixed_exact=1' 'fixed_overlap=3 protect_none_to_rw=2 overwrite_tag2=0 zero_fill=1 write_read=1 left_reserved_read=1 right_reserved_read=1 left_fixed_overlap=3 right_fixed_overlap=3 overwrite_tag2_mte=0 repeated_overwrite=0 repeated_zero=1' 'multi_owner_overwrite=0 exact=1 protect_mixed_none=0 zero=1/1/1 reads=0/0/0' 'adjacent_owners=0/0 protect_cross=0 query=0/0 current=1/1' 'reserved_fixed=3 partial_deallocate=0 failed_owner_hole=3 marker=0:6c hole=1 reuse=0 exact=1 zero=1' 'prefix_map=0 deallocate=0 reuse=0 exact=1 zero=1 suffix=0:7b' 'deallocate=0 fixed_reuse=0 reuse_exact=1' 'protect_commit_map=0 protect_rw=0 write_read=1 deallocate=0' 'anywhere_4m=0 aligned4m=1' 'anywhere_mte=0' 'anywhere_purgable=0' >"$(BUILD_DIR)/mach-vm-sparse-reservation.expected.out"
	cmp "$(BUILD_DIR)/mach-vm-sparse-reservation.expected.out" "$(BUILD_DIR)/mach-vm-sparse-reservation.native.out"

verify-mach-vm-region-native: $(BUILD_DIR)/mach-vm-region-native
	"$(BUILD_DIR)/mach-vm-region-native" >"$(BUILD_DIR)/mach-vm-region.native.out"
	printf '%s\n' 'reserve=0 owners=0/0 deallocate_hole=0 queries=1/1/1/1' 'errors task=268435459/1 flavor=4/1 count=4/1' >"$(BUILD_DIR)/mach-vm-region.expected.out"
	cmp "$(BUILD_DIR)/mach-vm-region.expected.out" "$(BUILD_DIR)/mach-vm-region.native.out"

HOST_VM_SERVICE_TEST := $(BUILD_DIR)/host-vm-service-test
HOST_FILE_METADATA_SERVICE_TEST := $(BUILD_DIR)/host-file-metadata-service-test
MACH_VM_READ_OVERWRITE_NATIVE := $(BUILD_DIR)/mach-vm-read-overwrite

verify-mach-vm-named-services:
	xcrun clang -arch arm64 -Wall -Wextra -Werror ../fixtures/handoff/mach-vm-read-overwrite.c -o "$(MACH_VM_READ_OVERWRITE_NATIVE)"
	"$(MACH_VM_READ_OVERWRITE_NATIVE)"
	xcrun clang++ $(CPPFLAGS) -isystem "$(METAL_CPP)" $(CXXFLAGS) -fblocks \
		$(filter-out main.cpp,$(APP_SOURCES)) ../fixtures/handoff/host-vm-service-test.cpp \
		-framework Foundation -framework Metal -framework Security -o "$(HOST_VM_SERVICE_TEST)"
	"$(HOST_VM_SERVICE_TEST)"

verify-host-file-metadata-services: $(HOST_FILE_METADATA_SERVICE_TEST)
	"$(HOST_FILE_METADATA_SERVICE_TEST)"

$(HOST_FILE_METADATA_SERVICE_TEST): $(filter-out main.cpp,$(APP_SOURCES)) \
		../fixtures/handoff/host-file-metadata-service-test.cpp
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang++ $(CPPFLAGS) -isystem "$(METAL_CPP)" $(CXXFLAGS) -fblocks $^ \
		-framework Foundation -framework Metal -framework Security -o "$@"

verify-mach-vm-deferred-reclamation-native: $(MACH_VM_DEFERRED_RECLAMATION_NATIVE)
	"$(MACH_VM_DEFERRED_RECLAMATION_NATIVE)" >"$(BUILD_DIR)/mach-vm-deferred-reclamation.native.out"
	printf '%s\n' 'mach_vm_deferred_reclamation valid=0x20004006 invalid=4 oversized=3 outputs=unchanged' >"$(BUILD_DIR)/mach-vm-deferred-reclamation.expected.out"
	cmp "$(BUILD_DIR)/mach-vm-deferred-reclamation.expected.out" "$(BUILD_DIR)/mach-vm-deferred-reclamation.native.out"

verify-guest-heap-allocate-at: $(GUEST_HEAP_ALLOCATE_AT_TEST)
	"$(GUEST_HEAP_ALLOCATE_AT_TEST)" >"$(BUILD_DIR)/guest-heap-allocate-at.out"
	printf '%s\n' 'guest heap allocate_at exact=ok collision=reject release=ok alignment=ok avoid=ok extend=ok' >"$(BUILD_DIR)/guest-heap-allocate-at.expected.out"
	cmp "$(BUILD_DIR)/guest-heap-allocate-at.expected.out" "$(BUILD_DIR)/guest-heap-allocate-at.out"

verify-cache-backing-leases: $(CACHE_BACKING_LEASES_TEST)
	"$(CACHE_BACKING_LEASES_TEST)" >"$(BUILD_DIR)/cache-backing-leases.out"
	printf '%s\n' 'cache backing leases repeated=128 alias=retained-until-unmapped active-vm=retained selector=untouched' >"$(BUILD_DIR)/cache-backing-leases.expected.out"
	cmp "$(BUILD_DIR)/cache-backing-leases.expected.out" "$(BUILD_DIR)/cache-backing-leases.out"

verify-memcpy-chk: $(MEMCPY_CHK_NATIVE) $(MEMCPY_CHK_SIGINFO_NATIVE) $(MEMCPY_CHK_DECODE_TEST)
	nm -u "$(MEMCPY_CHK_NATIVE)" | grep -q '___memcpy_chk'
	"$(MEMCPY_CHK_NATIVE)" >"$(BUILD_DIR)/memcpy-chk.native.out"
	printf '%s\n' 'copy=fort result=1' >"$(BUILD_DIR)/memcpy-chk.expected.out"
	cmp "$(BUILD_DIR)/memcpy-chk.expected.out" "$(BUILD_DIR)/memcpy-chk.native.out"
	@ulimit -c 0; set +e; "$(MEMCPY_CHK_NATIVE)" overflow >"$(BUILD_DIR)/memcpy-chk.overflow.out" 2>"$(BUILD_DIR)/memcpy-chk.overflow.err"; result=$$?; set -e; \
		 test "$$result" -eq 133 && test ! -s "$(BUILD_DIR)/memcpy-chk.overflow.out"
	python3 -c 'import struct, subprocess, sys; result = subprocess.run([sys.argv[1]], stdout=subprocess.PIPE, stderr=subprocess.PIPE); assert result.returncode == 77, result.returncode; assert result.stderr == b"", result.stderr; assert len(result.stdout) == 24, len(result.stdout); signum, code, pc, address = struct.unpack("<iiQQ", result.stdout); assert (signum, code) == (5, 0), (signum, code); assert pc != 0 and address != 0, (hex(pc), hex(address)); print("native SA_SIGINFO SIGTRAP: signal=5 code=0 controlled_exit=77 pc/address captured")' "$(MEMCPY_CHK_SIGINFO_NATIVE)"
	"$(MEMCPY_CHK_DECODE_TEST)"

verify-memset-chk: $(MEMSET_CHK_NATIVE) $(MEMSET_CHK_SIGINFO_NATIVE) $(MEMSET_CHK_DECODE_TEST)
	nm -u "$(MEMSET_CHK_NATIVE)" | grep -q '___memset_chk'
	"$(MEMSET_CHK_NATIVE)" >"$(BUILD_DIR)/memset-chk.native.out"
	printf '%s\n' 'fill=ffff result=1' >"$(BUILD_DIR)/memset-chk.expected.out"
	cmp "$(BUILD_DIR)/memset-chk.expected.out" "$(BUILD_DIR)/memset-chk.native.out"
	@ulimit -c 0; set +e; "$(MEMSET_CHK_NATIVE)" overflow >"$(BUILD_DIR)/memset-chk.overflow.out" 2>"$(BUILD_DIR)/memset-chk.overflow.err"; result=$$?; set -e; \
		 test "$$result" -eq 133 && test ! -s "$(BUILD_DIR)/memset-chk.overflow.out"
	python3 -c 'import struct, subprocess, sys; result = subprocess.run([sys.argv[1]], stdout=subprocess.PIPE, stderr=subprocess.PIPE); assert result.returncode == 77, result.returncode; assert result.stderr == b"", result.stderr; assert len(result.stdout) == 24, len(result.stdout); signum, code, pc, address = struct.unpack("<iiQQ", result.stdout); assert (signum, code) == (5, 0), (signum, code); assert pc != 0 and address != 0, (hex(pc), hex(address)); print("native SA_SIGINFO SIGTRAP: signal=5 code=0 controlled_exit=77 pc/address captured")' "$(MEMSET_CHK_SIGINFO_NATIVE)"
	"$(MEMSET_CHK_DECODE_TEST)"

$(CHAINED_ADDENDS_TEST): ../fixtures/handoff/chained-addends-test.cpp macho.cpp macho.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG ../fixtures/handoff/chained-addends-test.cpp macho.cpp -o "$@"

$(CHAINED_ADDENDS_FIXTURE): ../fixtures/handoff/chained-addend-fixture.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -dynamiclib -fPIC -Wl,-fixup_chains \
		-Wl,-undefined,dynamic_lookup -Wall -Wextra -Werror "$<" -o "$@"

$(CHAINED_ADDEND_PROVIDER): ../fixtures/handoff/chained-addend-positive-provider.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -dynamiclib -fPIC -Wl,-fixup_chains \
		-Wl,-install_name,@rpath/libchained-addend-provider.dylib \
		-Wall -Wextra -Werror "$<" -o "$@"

$(CHAINED_ADDEND_POINTER): ../fixtures/handoff/chained-addend-positive-pointer.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -c "$<" -o "$@"

$(CHAINED_ADDEND_POSITIVE): ../fixtures/handoff/chained-addend-positive-main.c $(CHAINED_ADDEND_POINTER) $(CHAINED_ADDEND_PROVIDER)
	xcrun clang -arch arm64 -Wl,-fixup_chains -Wl,-rpath,@executable_path \
		-Wl,-L,$(BUILD_DIR) -Wl,-lchained-addend-provider "$<" \
		"$(CHAINED_ADDEND_POINTER)" -o "$@"

$(CHAINED_ADDEND_WEAK_POINTER): ../fixtures/handoff/chained-addend-weak-pointer.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -c "$<" -o "$@"

$(CHAINED_ADDEND_WEAK): ../fixtures/handoff/chained-addend-weak-main.c $(CHAINED_ADDEND_WEAK_POINTER)
	xcrun clang -arch arm64 -Wl,-fixup_chains -Wl,-undefined,dynamic_lookup \
		"$<" "$(CHAINED_ADDEND_WEAK_POINTER)" -o "$@"

$(CHAINED_ADDENDS_IMAGE_TEST): ../fixtures/handoff/chained-addends-image-test.cpp image_loader.cpp macho.cpp shared_cache.cpp guest_code_policy.cpp guest_tlv.cpp guest_state.cpp guest_format.cpp guest_scheduler.cpp guest_strings.cpp guest_vector_crypto.cpp translate_core.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I. "$<" image_loader.cpp macho.cpp \
		shared_cache.cpp guest_code_policy.cpp guest_tlv.cpp guest_state.cpp guest_format.cpp \
		guest_scheduler.cpp guest_strings.cpp guest_vector_crypto.cpp translate_core.cpp \
		-framework Foundation -framework Security -framework CoreFoundation -o "$@"

.PHONY: verify-chained-addends
verify-chained-addends: $(CHAINED_ADDENDS_TEST) $(CHAINED_ADDENDS_FIXTURE) \
	$(CHAINED_ADDEND_POSITIVE) $(CHAINED_ADDEND_WEAK) $(CHAINED_ADDENDS_IMAGE_TEST)
	"$(CHAINED_ADDENDS_TEST)" "$(CHAINED_ADDENDS_FIXTURE)"
	xcrun dyld_info -arch arm64 -fixup_chain_details "$(CHAINED_ADDEND_POSITIVE)" | grep -q 'addend: 12'
	xcrun dyld_info -arch arm64 -imports "$(CHAINED_ADDEND_WEAK)" | \
		grep -q '_chained_missing_target \[weak-import\]'
	"$(CHAINED_ADDEND_POSITIVE)" >"$(BUILD_DIR)/chained-addend-positive.native"
	printf '%s\n' 'marker=4242 offset=3' >"$(BUILD_DIR)/chained-addend-positive.expected"
	cmp "$(BUILD_DIR)/chained-addend-positive.expected" "$(BUILD_DIR)/chained-addend-positive.native"
	"$(CHAINED_ADDEND_WEAK)" >"$(BUILD_DIR)/chained-addend-weak.native"
	printf '%s\n' 'weak_pointer=12' >"$(BUILD_DIR)/chained-addend-weak.expected"
	cmp "$(BUILD_DIR)/chained-addend-weak.expected" "$(BUILD_DIR)/chained-addend-weak.native"
	"$(CHAINED_ADDENDS_IMAGE_TEST)" strong "$(CHAINED_ADDEND_POSITIVE)" >"$(BUILD_DIR)/chained-addend-positive.loader"
	printf '%s\n' 'strong_addend=12' >"$(BUILD_DIR)/chained-addend-positive.loader.expected"
	cmp "$(BUILD_DIR)/chained-addend-positive.loader.expected" "$(BUILD_DIR)/chained-addend-positive.loader"
	"$(CHAINED_ADDENDS_IMAGE_TEST)" weak "$(CHAINED_ADDEND_WEAK)" >"$(BUILD_DIR)/chained-addend-weak.loader"
	printf '%s\n' 'weak_null_addend=12' >"$(BUILD_DIR)/chained-addend-weak.loader.expected"
	cmp "$(BUILD_DIR)/chained-addend-weak.loader.expected" "$(BUILD_DIR)/chained-addend-weak.loader"

$(SHA256_NATIVE): ../fixtures/handoff/sha256-instructions.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -O2 -Wall -Wextra -Werror "$<" -o "$@"

$(VECTOR_CRYPTO_LOWER_TEST): ../fixtures/handoff/vector-crypto-lowering.cpp guest_vector_crypto.cpp guest_vector_crypto.hpp translate_core.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG ../fixtures/handoff/vector-crypto-lowering.cpp guest_vector_crypto.cpp -o "$@"

$(VECTOR_CRYPTO_MSL): $(VECTOR_CRYPTO_LOWER_TEST)
	@mkdir -p "$(BUILD_DIR)"
	"$(VECTOR_CRYPTO_LOWER_TEST)" --emit-metal >"$@"

$(VECTOR_CRYPTO_AIR): $(VECTOR_CRYPTO_MSL)
	xcrun -sdk macosx metal -I . -c "$<" -o "$@"

verify-vector-crypto: $(SHA256_NATIVE) $(VECTOR_CRYPTO_LOWER_TEST) $(VECTOR_CRYPTO_AIR)
	"$(SHA256_NATIVE)" >"$(BUILD_DIR)/sha256-instructions.native.out"
	cmp ../fixtures/handoff/sha256-instructions.expected.out "$(BUILD_DIR)/sha256-instructions.native.out"
	"$(VECTOR_CRYPTO_LOWER_TEST)"

$(INTERPRETER_DEMAND_CACHE_INDEXED_SOURCE): $(INTERPRETER_DEMAND_CACHE_GENERATOR)
	@mkdir -p "$(dir $@)"
	"$(INTERPRETER_DEMAND_CACHE_GENERATOR)" indexed >"$@"

$(INTERPRETER_DEMAND_CACHE_INDEXED_MAIN): $(INTERPRETER_DEMAND_CACHE_INDEXED_SOURCE)
	xcrun clang -arch arm64 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_INDEXED_NATIVE): ../fixtures/interpreter/demand-cache-retry-native.c
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -DPAIR_LOAD=0 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_BYTE_SOURCE): $(INTERPRETER_DEMAND_CACHE_GENERATOR)
	@mkdir -p "$(dir $@)"
	"$(INTERPRETER_DEMAND_CACHE_GENERATOR)" byte >"$@"

$(INTERPRETER_DEMAND_CACHE_BYTE_MAIN): $(INTERPRETER_DEMAND_CACHE_BYTE_SOURCE)
	xcrun clang -arch arm64 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_BYTE_NATIVE): ../fixtures/interpreter/demand-cache-retry-native.c
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -DPAIR_LOAD=0 -DACCESS_WIDTH=1 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_HALF_SOURCE): $(INTERPRETER_DEMAND_CACHE_GENERATOR)
	@mkdir -p "$(dir $@)"
	"$(INTERPRETER_DEMAND_CACHE_GENERATOR)" half >"$@"

$(INTERPRETER_DEMAND_CACHE_HALF_MAIN): $(INTERPRETER_DEMAND_CACHE_HALF_SOURCE)
	xcrun clang -arch arm64 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_HALF_NATIVE): ../fixtures/interpreter/demand-cache-retry-native.c
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -DPAIR_LOAD=0 -DACCESS_WIDTH=2 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_SOURCE): $(INTERPRETER_DEMAND_CACHE_GENERATOR)
	@mkdir -p "$(dir $@)"
	"$(INTERPRETER_DEMAND_CACHE_GENERATOR)" signed-byte >"$@"

$(INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_MAIN): $(INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_SOURCE)
	xcrun clang -arch arm64 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_NATIVE): ../fixtures/interpreter/demand-cache-retry-native.c
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -DPAIR_LOAD=0 -DACCESS_WIDTH=1 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_SIGNED_HALF_SOURCE): $(INTERPRETER_DEMAND_CACHE_GENERATOR)
	@mkdir -p "$(dir $@)"
	"$(INTERPRETER_DEMAND_CACHE_GENERATOR)" signed-half >"$@"

$(INTERPRETER_DEMAND_CACHE_SIGNED_HALF_MAIN): $(INTERPRETER_DEMAND_CACHE_SIGNED_HALF_SOURCE)
	xcrun clang -arch arm64 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_SIGNED_HALF_NATIVE): ../fixtures/interpreter/demand-cache-retry-native.c
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -DPAIR_LOAD=0 -DACCESS_WIDTH=2 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_SIGNED_WORD_SOURCE): $(INTERPRETER_DEMAND_CACHE_GENERATOR)
	@mkdir -p "$(dir $@)"
	"$(INTERPRETER_DEMAND_CACHE_GENERATOR)" signed-word >"$@"

$(INTERPRETER_DEMAND_CACHE_SIGNED_WORD_MAIN): $(INTERPRETER_DEMAND_CACHE_SIGNED_WORD_SOURCE)
	xcrun clang -arch arm64 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_SIGNED_WORD_NATIVE): ../fixtures/interpreter/demand-cache-retry-native.c
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -DPAIR_LOAD=0 -DACCESS_WIDTH=4 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_VECTOR_POST_SOURCE): $(INTERPRETER_DEMAND_CACHE_GENERATOR)
	@mkdir -p "$(dir $@)"
	"$(INTERPRETER_DEMAND_CACHE_GENERATOR)" vector-post >"$@"

$(INTERPRETER_DEMAND_CACHE_VECTOR_POST_MAIN): $(INTERPRETER_DEMAND_CACHE_VECTOR_POST_SOURCE)
	xcrun clang -arch arm64 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_VECTOR_POST_NATIVE): ../fixtures/interpreter/demand-cache-vector-writeback-native.c
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -DVECTOR_PRE=0 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_VECTOR_PRE_SOURCE): $(INTERPRETER_DEMAND_CACHE_GENERATOR)
	@mkdir -p "$(dir $@)"
	"$(INTERPRETER_DEMAND_CACHE_GENERATOR)" vector-pre >"$@"

$(INTERPRETER_DEMAND_CACHE_VECTOR_PRE_MAIN): $(INTERPRETER_DEMAND_CACHE_VECTOR_PRE_SOURCE)
	xcrun clang -arch arm64 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_VECTOR_PRE_NATIVE): ../fixtures/interpreter/demand-cache-vector-writeback-native.c
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -DVECTOR_PRE=1 "$<" -lz -o "$@"

.PHONY: interpreter-metallib
interpreter-metallib: $(INTERPRETER_METALLIB)

$(INTERPRETER_GENERATOR): generate_interpreter_msl.cpp guest_memory_msl.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) "$<" -o "$@"

$(INTERPRETER_SOURCE): gpu_interpreter.metal $(INTERPRETER_GENERATOR)
	@mkdir -p "$(dir $@)"
	"$(INTERPRETER_GENERATOR)" "$<" "$@"

$(INTERPRETER_AIR): $(INTERPRETER_SOURCE) integer_ops_shared.h
	@mkdir -p "$(dir $@)"
	xcrun -sdk macosx metal -I . -c "$<" -o "$@"

$(INTERPRETER_METALLIB): $(INTERPRETER_AIR)
	xcrun -sdk macosx metal -o "$@" "$<"

verify-adc-sbc-carry-native: $(INTERPRETER_TYPED_NATIVE) $(INTERPRETER_ADC_SBC_CARRY_MAIN) $(ADC_SBC_CARRY_DECODE_TEST)
	@set -eu; \
		"$(INTERPRETER_TYPED_NATIVE)"; \
		"$(ADC_SBC_CARRY_DECODE_TEST)"; \
		set +e; \
		"$(INTERPRETER_ADC_SBC_CARRY_MAIN)"; native_rc=$$?; \
		set -e; \
		test "$$native_rc" -eq 42; \
		echo "adc-sbc-carry native guest exit=$$native_rc"

verify-adc-sbc-carry-interpreter: $(APP) $(INTERPRETER_ADC_SBC_CARRY_MAIN) $(INTERPRETER_METALLIB)
	@set -eu; \
		run_dir=$$(mktemp -d "$(BUILD_DIR)/adc-sbc-carry-interpreter.XXXXXX"); \
		set +e; \
		"$(INTERPRETER_ADC_SBC_CARRY_MAIN)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_ADC_SBC_CARRY_MAIN)" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_ADC_SBC_CARRY_MAIN)" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
		set -e; \
		test "$$native_rc" -eq 42 && test "$$translated_rc" -eq 42 && \
		test "$$interpreter_rc" -eq 42 && \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out" && \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out" && \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err" && \
		grep -q 'sbcs x9, x9, x9' ../fixtures/interpreter/adc-sbc-carry-main.s && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/interpreter.err" && \
		grep -q 'interpreter-block-ready.*pc=0x42d8 detail=36' "$$run_dir/interpreter.err" && \
		grep -q 'interpreter-dispatch.*pc=0x42d8 detail=36' "$$run_dir/interpreter.err" && \
		test "$$(grep -c 'event=interpreter-dispatch' "$$run_dir/interpreter.err")" -eq 1 && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/interpreter.err" && \
		sed '/^arm64-metal trace /d' "$$run_dir/interpreter.err" >"$$run_dir/interpreter.no-trace.err" && \
		cmp "$$run_dir/native.err" "$$run_dir/interpreter.no-trace.err" && \
		echo "adc-sbc-carry interpreter dispatch verified: $$run_dir"

verify-interpreter: $(APP) $(INTERPRETER_TYPED_NATIVE) $(INTERPRETER_MOVE_WIDE_NATIVE) $(INTERPRETER_ADD_SUB_NATIVE) $(INTERPRETER_ADD_SUB_FLAGS_NATIVE) $(INTERPRETER_MINIMAL_MAIN) $(INTERPRETER_MOVN_MOVK_MAIN) $(INTERPRETER_ADD_SUB_MAIN) $(INTERPRETER_ADD_SUB_SP_MAIN) $(INTERPRETER_BCOND_MAIN) $(INTERPRETER_CSEL_MAIN) $(INTERPRETER_ADD_SUB_SHIFTED_MAIN) $(INTERPRETER_ADD_SUB_EXTENDED_MAIN) $(INTERPRETER_LOGICAL_SHIFTED_MAIN) $(INTERPRETER_LOGICAL_VARIABLE_SHIFTS_MAIN) $(INTERPRETER_MULTIPLY_ACCUMULATE_MAIN) $(INTERPRETER_BITFIELD_MAIN) $(INTERPRETER_BITFIELD_NATIVE) $(INTERPRETER_INTEGER_BITOPS_MAIN) $(INTERPRETER_INTEGER_DIVIDE_MAIN) $(INTERPRETER_LITERAL_LOAD_MAIN) $(INTERPRETER_LOGICAL_IMMEDIATE_MAIN) $(INTERPRETER_LOGICAL_IMMEDIATE_INVALID_MAIN) $(INTERPRETER_BRANCH_MAIN) $(INTERPRETER_BRANCH_FALLBACK_MAIN) $(INTERPRETER_BL_MAIN) $(INTERPRETER_BL_NATIVE) $(INTERPRETER_BL_UNPLANNED_MAIN) $(INTERPRETER_BL_UNPLANNED_NATIVE) $(INTERPRETER_BRANCH_REGISTER_MAIN) $(INTERPRETER_BRANCH_REGISTER_NATIVE) $(INTERPRETER_BRANCH_REGISTER_IMPORT_MAIN) $(INTERPRETER_BRANCH_REGISTER_IMPORT_NATIVE) $(INTERPRETER_TYPED_MEMORY_MAIN) $(INTERPRETER_TYPED_MEMORY_RO_MAIN) $(INTERPRETER_TYPED_MEMORY_UNMAPPED_MAIN) $(INTERPRETER_PAIR_MEMORY_MAIN) $(INTERPRETER_SCALAR_INDEXED_MEMORY_MAIN) $(INTERPRETER_CBZ_CBNZ_MAIN) $(INTERPRETER_TBZ_TBNZ_MAIN) $(INTERPRETER_ADR_ADRP_MAIN) $(INTERPRETER_METALLIB) verify-interpreter-vector-memory
	@set -eu; \
		mkdir -p "$(BUILD_DIR)"; \
		run_dir=$$(mktemp -d "$(BUILD_DIR)/interpreter-verify.XXXXXX"); \
		"$(INTERPRETER_TYPED_NATIVE)"; \
		ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" "$(APP)" interpreter-probe "$(abspath $(INTERPRETER_METALLIB))" >"$$run_dir/interpreter.gpu.out" 2>"$$run_dir/interpreter.gpu.err"; \
		"$(INTERPRETER_MOVE_WIDE_NATIVE)" >"$$run_dir/interpreter.native.out"; \
		"$(INTERPRETER_ADD_SUB_NATIVE)" >>"$$run_dir/interpreter.native.out"; \
		"$(INTERPRETER_ADD_SUB_FLAGS_NATIVE)" >>"$$run_dir/interpreter.native.out"; \
		cmp "$$run_dir/interpreter.native.out" "$$run_dir/interpreter.gpu.out"; \
		grep -qx 'pipeline_compiles=1 runtime_source_compiles=0 completed_dispatches=43' "$$run_dir/interpreter.gpu.err"; \
		set +e; \
		"$(INTERPRETER_MINIMAL_MAIN)" >"$$run_dir/interpreter-process.native.out" 2>"$$run_dir/interpreter-process.native.err"; native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_MINIMAL_MAIN)" >"$$run_dir/interpreter-process.translated.out" 2>"$$run_dir/interpreter-process.translated.err"; translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_MINIMAL_MAIN)" >"$$run_dir/interpreter-process.interpreter.out" 2>"$$run_dir/interpreter-process.interpreter.err"; interpreter_rc=$$?; \
		printf '%s\n' "$$native_rc" >"$$run_dir/interpreter-process.native.rc"; \
		printf '%s\n' "$$translated_rc" >"$$run_dir/interpreter-process.translated.rc"; \
		printf '%s\n' "$$interpreter_rc" >"$$run_dir/interpreter-process.interpreter.rc"; \
		set -e; \
		test "$$native_rc" -eq 42 && test "$$translated_rc" -eq "$$native_rc" && \
		test "$$interpreter_rc" -eq "$$native_rc" && \
		cmp "$$run_dir/interpreter-process.native.out" "$$run_dir/interpreter-process.translated.out" && \
		cmp "$$run_dir/interpreter-process.native.err" "$$run_dir/interpreter-process.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/interpreter-process.interpreter.err" && \
		grep -q 'interpreter-block-ready.*detail=2' "$$run_dir/interpreter-process.interpreter.err" && \
		grep -q 'interpreter-dispatch.*detail=2' "$$run_dir/interpreter-process.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_MOVN_MOVK_MAIN)" >"$$run_dir/movn-movk.native.out" 2>"$$run_dir/movn-movk.native.err"; move_wide_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_MOVN_MOVK_MAIN)" >"$$run_dir/movn-movk.translated.out" 2>"$$run_dir/movn-movk.translated.err"; move_wide_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_MOVN_MOVK_MAIN)" >"$$run_dir/movn-movk.interpreter.out" 2>"$$run_dir/movn-movk.interpreter.err"; move_wide_interpreter_rc=$$?; \
		printf '%s\n' "$$move_wide_native_rc" >"$$run_dir/movn-movk.native.rc"; \
		printf '%s\n' "$$move_wide_translated_rc" >"$$run_dir/movn-movk.translated.rc"; \
		printf '%s\n' "$$move_wide_interpreter_rc" >"$$run_dir/movn-movk.interpreter.rc"; \
		set -e; \
		test "$$move_wide_native_rc" -eq 42 && test "$$move_wide_translated_rc" -eq "$$move_wide_native_rc" && \
		test "$$move_wide_interpreter_rc" -eq "$$move_wide_native_rc" && \
		cmp "$$run_dir/movn-movk.native.out" "$$run_dir/movn-movk.translated.out" && \
		cmp "$$run_dir/movn-movk.native.err" "$$run_dir/movn-movk.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/movn-movk.interpreter.err" && \
		grep -q 'interpreter-block-ready.*detail=3' "$$run_dir/movn-movk.interpreter.err" && \
		grep -q 'interpreter-dispatch.*detail=3' "$$run_dir/movn-movk.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_BRANCH_MAIN)" >"$$run_dir/branch.native.out" 2>"$$run_dir/branch.native.err"; branch_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_BRANCH_MAIN)" >"$$run_dir/branch.translated.out" 2>"$$run_dir/branch.translated.err"; branch_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_BRANCH_MAIN)" >"$$run_dir/branch.interpreter.out" 2>"$$run_dir/branch.interpreter.err"; branch_interpreter_rc=$$?; \
		printf '%s\n' "$$branch_native_rc" >"$$run_dir/branch.native.rc"; \
		printf '%s\n' "$$branch_translated_rc" >"$$run_dir/branch.translated.rc"; \
		printf '%s\n' "$$branch_interpreter_rc" >"$$run_dir/branch.interpreter.rc"; \
		set -e; \
		test "$$branch_native_rc" -eq 7 && test "$$branch_translated_rc" -eq "$$branch_native_rc" && \
		test "$$branch_interpreter_rc" -eq "$$branch_native_rc" && \
		cmp "$$run_dir/branch.native.out" "$$run_dir/branch.translated.out" && \
		cmp "$$run_dir/branch.native.out" "$$run_dir/branch.interpreter.out" && \
		cmp "$$run_dir/branch.native.err" "$$run_dir/branch.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/branch.interpreter.err" && \
		grep -q 'interpreter-block-ready.*detail=3' "$$run_dir/branch.interpreter.err" && \
		grep -q 'interpreter-dispatch.*detail=3' "$$run_dir/branch.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_BRANCH_FALLBACK_MAIN)" >"$$run_dir/branch-fallback.native.out" 2>"$$run_dir/branch-fallback.native.err"; fallback_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_BRANCH_FALLBACK_MAIN)" >"$$run_dir/branch-fallback.translated.out" 2>"$$run_dir/branch-fallback.translated.err"; fallback_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_BRANCH_FALLBACK_MAIN)" >"$$run_dir/branch-fallback.interpreter.out" 2>"$$run_dir/branch-fallback.interpreter.err"; fallback_interpreter_rc=$$?; \
		printf '%s\n' "$$fallback_native_rc" >"$$run_dir/branch-fallback.native.rc"; \
		printf '%s\n' "$$fallback_translated_rc" >"$$run_dir/branch-fallback.translated.rc"; \
		printf '%s\n' "$$fallback_interpreter_rc" >"$$run_dir/branch-fallback.interpreter.rc"; \
		set -e; \
		test "$$fallback_native_rc" -eq 42 && test "$$fallback_translated_rc" -eq "$$fallback_native_rc" && \
		test "$$fallback_interpreter_rc" -eq "$$fallback_native_rc" && \
		cmp "$$run_dir/branch-fallback.native.out" "$$run_dir/branch-fallback.translated.out" && \
		cmp "$$run_dir/branch-fallback.native.out" "$$run_dir/branch-fallback.interpreter.out" && \
		cmp "$$run_dir/branch-fallback.native.err" "$$run_dir/branch-fallback.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/branch-fallback.interpreter.err" && \
		grep -q 'interpreter-block-ready.*detail=2' "$$run_dir/branch-fallback.interpreter.err" && \
		grep -q 'interpreter-dispatch.*detail=2' "$$run_dir/branch-fallback.interpreter.err" && \
		awk '/event=interpreter-dispatch/ { seen=1; next } seen && /event=compile-start/ { found=1 } END { exit !found }' "$$run_dir/branch-fallback.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_BL_NATIVE)" >"$$run_dir/bl-native.out" 2>"$$run_dir/bl-native.err"; bl_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_BL_MAIN)" >"$$run_dir/bl-translated.out" 2>"$$run_dir/bl-translated.err"; bl_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_BL_MAIN)" >"$$run_dir/bl-interpreter.out" 2>"$$run_dir/bl-interpreter.err"; bl_interpreter_rc=$$?; \
		set -e; \
		test "$$bl_native_rc" -eq 42 && test "$$bl_translated_rc" -eq "$$bl_native_rc" && \
		test "$$bl_interpreter_rc" -eq "$$bl_native_rc" && \
		cmp "$$run_dir/bl-native.out" "$$run_dir/bl-translated.out" && \
		cmp "$$run_dir/bl-native.out" "$$run_dir/bl-interpreter.out" && \
		cmp "$$run_dir/bl-native.err" "$$run_dir/bl-translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/bl-interpreter.err" && \
		grep -q 'interpreter-dispatch' "$$run_dir/bl-interpreter.err" && \
		awk '/event=interpreter-dispatch/ { seen=1 } seen && /event=compile-start/ { found=1 } END { exit found }' "$$run_dir/bl-interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_BL_UNPLANNED_NATIVE)" >"$$run_dir/bl-unplanned.native.out" 2>"$$run_dir/bl-unplanned.native.err"; bl_unplanned_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_BL_UNPLANNED_MAIN)" >"$$run_dir/bl-unplanned.translated.out" 2>"$$run_dir/bl-unplanned.translated.err"; bl_unplanned_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_BL_UNPLANNED_MAIN)" >"$$run_dir/bl-unplanned.interpreter.out" 2>"$$run_dir/bl-unplanned.interpreter.err"; bl_unplanned_interpreter_rc=$$?; \
		printf '%s\n' "$$bl_unplanned_native_rc" >"$$run_dir/bl-unplanned.native.rc"; \
		printf '%s\n' "$$bl_unplanned_translated_rc" >"$$run_dir/bl-unplanned.translated.rc"; \
		printf '%s\n' "$$bl_unplanned_interpreter_rc" >"$$run_dir/bl-unplanned.interpreter.rc"; \
		set -e; \
		test "$$bl_unplanned_native_rc" -eq 42 && test "$$bl_unplanned_translated_rc" -eq "$$bl_unplanned_native_rc" && \
		test "$$bl_unplanned_interpreter_rc" -eq "$$bl_unplanned_native_rc" && \
		cmp "$$run_dir/bl-unplanned.native.out" "$$run_dir/bl-unplanned.translated.out" && \
		cmp "$$run_dir/bl-unplanned.native.out" "$$run_dir/bl-unplanned.interpreter.out" && \
		cmp "$$run_dir/bl-unplanned.native.err" "$$run_dir/bl-unplanned.translated.err" && \
		awk '/event=interpreter-dispatch pc=0x42d8/ { entry=1; next } entry && /event=dispatch-done pc=0x4300/ { target_exit=1; next } target_exit && /event=compile-start pc=0x4300/ { target_compile=1; next } target_compile && /event=dispatch-done pc=0x42f0/ { return_exit=1; next } return_exit && /event=compile-start pc=0x42f0/ { return_compile=1; next } return_compile && /event=interpreter-dispatch pc=0x42e0/ { resumed=1; exit } END { exit !resumed }' "$$run_dir/bl-unplanned.interpreter.err" && \
		sed '/^arm64-metal trace /d' "$$run_dir/bl-unplanned.interpreter.err" >"$$run_dir/bl-unplanned.interpreter.no-trace.err" && \
		cmp "$$run_dir/bl-unplanned.native.err" "$$run_dir/bl-unplanned.interpreter.no-trace.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_BRANCH_REGISTER_NATIVE)" >"$$run_dir/branch-register.native.out" 2>"$$run_dir/branch-register.native.err"; branch_register_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_BRANCH_REGISTER_MAIN)" >"$$run_dir/branch-register.translated.out" 2>"$$run_dir/branch-register.translated.err"; branch_register_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_BRANCH_REGISTER_MAIN)" >"$$run_dir/branch-register.interpreter.out" 2>"$$run_dir/branch-register.interpreter.err"; branch_register_interpreter_rc=$$?; \
		printf '%s\n' "$$branch_register_native_rc" >"$$run_dir/branch-register.native.rc"; \
		printf '%s\n' "$$branch_register_translated_rc" >"$$run_dir/branch-register.translated.rc"; \
		printf '%s\n' "$$branch_register_interpreter_rc" >"$$run_dir/branch-register.interpreter.rc"; \
		set -e; \
		test "$$branch_register_native_rc" -eq 42 && test "$$branch_register_translated_rc" -eq "$$branch_register_native_rc" && \
		test "$$branch_register_interpreter_rc" -eq "$$branch_register_native_rc" && \
		cmp "$$run_dir/branch-register.native.out" "$$run_dir/branch-register.translated.out" && \
		cmp "$$run_dir/branch-register.native.out" "$$run_dir/branch-register.interpreter.out" && \
		cmp "$$run_dir/branch-register.native.err" "$$run_dir/branch-register.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/branch-register.interpreter.err" && \
		awk '/event=interpreter-dispatch pc=0x42d8/ { step=1; next } step == 1 && /event=interpreter-dispatch pc=0x42f8/ { step=2; next } step == 2 && /event=interpreter-dispatch pc=0x42e4/ { step=3; next } step == 3 && /event=interpreter-dispatch pc=0x42f4/ { step=4; next } /event=compile-start/ { compile=1 } END { exit !(step == 4 && !compile) }' "$$run_dir/branch-register.interpreter.err" && \
		sed '/^arm64-metal trace /d' "$$run_dir/branch-register.interpreter.err" >"$$run_dir/branch-register.interpreter.no-trace.err" && \
		cmp "$$run_dir/branch-register.native.err" "$$run_dir/branch-register.interpreter.no-trace.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_BRANCH_REGISTER_IMPORT_NATIVE)" >"$$run_dir/branch-register-import.native.out" 2>"$$run_dir/branch-register-import.native.err"; branch_register_import_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_BRANCH_REGISTER_IMPORT_MAIN)" >"$$run_dir/branch-register-import.translated.out" 2>"$$run_dir/branch-register-import.translated.err"; branch_register_import_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_BRANCH_REGISTER_IMPORT_MAIN)" >"$$run_dir/branch-register-import.interpreter.out" 2>"$$run_dir/branch-register-import.interpreter.err"; branch_register_import_interpreter_rc=$$?; \
		printf '%s\n' "$$branch_register_import_native_rc" >"$$run_dir/branch-register-import.native.rc"; \
		printf '%s\n' "$$branch_register_import_translated_rc" >"$$run_dir/branch-register-import.translated.rc"; \
		printf '%s\n' "$$branch_register_import_interpreter_rc" >"$$run_dir/branch-register-import.interpreter.rc"; \
		set -e; \
		test "$$branch_register_import_native_rc" -eq 42 && test "$$branch_register_import_translated_rc" -eq "$$branch_register_import_native_rc" && \
		test "$$branch_register_import_interpreter_rc" -eq "$$branch_register_import_native_rc" && \
		cmp "$$run_dir/branch-register-import.native.out" "$$run_dir/branch-register-import.translated.out" && \
		cmp "$$run_dir/branch-register-import.native.out" "$$run_dir/branch-register-import.interpreter.out" && \
		cmp "$$run_dir/branch-register-import.native.err" "$$run_dir/branch-register-import.translated.err" && \
		grep -qx 'blr-import' "$$run_dir/branch-register-import.native.out" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/branch-register-import.interpreter.err" && \
		awk '/event=interpreter-dispatch pc=0x4370/ { step=1; next } step == 1 && /event=dispatch-done pc=0x80000000/ { step=2; next } step == 2 && /event=compile-start pc=0x80000000/ { step=3; next } step == 3 && /event=compile-done pc=0x80000000/ { step=4; next } step == 4 && /event=dispatch-start pc=0x80000000/ { step=5; next } step == 5 && /event=dispatch-done pc=0x4384/ { step=6; next } step == 6 && /event=host-service pc=0x4384/ { step=7; next } step == 7 && /event=interpreter-dispatch pc=0x4384/ { step=8; next } END { exit !(step == 8) }' "$$run_dir/branch-register-import.interpreter.err" && \
		sed '/^arm64-metal trace /d' "$$run_dir/branch-register-import.interpreter.err" >"$$run_dir/branch-register-import.interpreter.no-trace.err" && \
		cmp "$$run_dir/branch-register-import.native.err" "$$run_dir/branch-register-import.interpreter.no-trace.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_TYPED_MEMORY_MAIN)" >"$$run_dir/typed-memory.native.out" 2>"$$run_dir/typed-memory.native.err"; memory_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_TYPED_MEMORY_MAIN)" >"$$run_dir/typed-memory.translated.out" 2>"$$run_dir/typed-memory.translated.err"; memory_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_TYPED_MEMORY_MAIN)" >"$$run_dir/typed-memory.interpreter.out" 2>"$$run_dir/typed-memory.interpreter.err"; memory_interpreter_rc=$$?; \
		printf '%s\n' "$$memory_native_rc" >"$$run_dir/typed-memory.native.rc"; \
		printf '%s\n' "$$memory_translated_rc" >"$$run_dir/typed-memory.translated.rc"; \
		printf '%s\n' "$$memory_interpreter_rc" >"$$run_dir/typed-memory.interpreter.rc"; \
		set -e; \
		test "$$memory_native_rc" -eq 42 && test "$$memory_translated_rc" -eq "$$memory_native_rc" && \
		test "$$memory_interpreter_rc" -eq "$$memory_native_rc" && \
		cmp "$$run_dir/typed-memory.native.out" "$$run_dir/typed-memory.translated.out" && \
		cmp "$$run_dir/typed-memory.native.out" "$$run_dir/typed-memory.interpreter.out" && \
		cmp "$$run_dir/typed-memory.native.err" "$$run_dir/typed-memory.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/typed-memory.interpreter.err" && \
		grep -q 'interpreter-block-ready.*detail=6' "$$run_dir/typed-memory.interpreter.err" && \
		grep -q 'interpreter-dispatch.*detail=6' "$$run_dir/typed-memory.interpreter.err" || exit 1; \
		set +e; \
		(ulimit -c 0; ARM64_METAL_TRACE_FATAL_SIGNAL=1 ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_TYPED_MEMORY_RO_MAIN)" >"$$run_dir/typed-memory-ro.interpreter.out" 2>"$$run_dir/typed-memory-ro.interpreter.err"); memory_ro_rc=$$?; \
		(ulimit -c 0; ARM64_METAL_TRACE_FATAL_SIGNAL=1 ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_TYPED_MEMORY_UNMAPPED_MAIN)" >"$$run_dir/typed-memory-unmapped.interpreter.out" 2>"$$run_dir/typed-memory-unmapped.interpreter.err"); memory_unmapped_rc=$$?; \
		set -e; \
		test "$$memory_ro_rc" -eq 138 && test "$$memory_unmapped_rc" -eq 139 && \
		grep -q 'interpreter-block-ready.*detail=4' "$$run_dir/typed-memory-ro.interpreter.err" && \
		grep -q 'interpreter-dispatch.*detail=4' "$$run_dir/typed-memory-ro.interpreter.err" && \
		grep -q 'Guest process memory access fault at 0x42d8' "$$run_dir/typed-memory-ro.interpreter.err" && \
		grep -q 'interpreter-block-ready.*detail=3' "$$run_dir/typed-memory-unmapped.interpreter.err" && \
		grep -q 'interpreter-dispatch.*detail=3' "$$run_dir/typed-memory-unmapped.interpreter.err" && \
		grep -q 'Guest process memory access fault at 0x0' "$$run_dir/typed-memory-unmapped.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_CBZ_CBNZ_MAIN)" >"$$run_dir/cbz-cbnz.native.out" 2>"$$run_dir/cbz-cbnz.native.err"; cbz_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_CBZ_CBNZ_MAIN)" >"$$run_dir/cbz-cbnz.translated.out" 2>"$$run_dir/cbz-cbnz.translated.err"; cbz_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_CBZ_CBNZ_MAIN)" >"$$run_dir/cbz-cbnz.interpreter.out" 2>"$$run_dir/cbz-cbnz.interpreter.err"; cbz_interpreter_rc=$$?; \
		printf '%s\n' "$$cbz_native_rc" >"$$run_dir/cbz-cbnz.native.rc"; \
		printf '%s\n' "$$cbz_translated_rc" >"$$run_dir/cbz-cbnz.translated.rc"; \
		printf '%s\n' "$$cbz_interpreter_rc" >"$$run_dir/cbz-cbnz.interpreter.rc"; \
		set -e; \
		test "$$cbz_native_rc" -eq 42 && test "$$cbz_translated_rc" -eq "$$cbz_native_rc" && \
		test "$$cbz_interpreter_rc" -eq "$$cbz_native_rc" && \
		cmp "$$run_dir/cbz-cbnz.native.out" "$$run_dir/cbz-cbnz.translated.out" && \
		cmp "$$run_dir/cbz-cbnz.native.out" "$$run_dir/cbz-cbnz.interpreter.out" && \
		cmp "$$run_dir/cbz-cbnz.native.err" "$$run_dir/cbz-cbnz.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/cbz-cbnz.interpreter.err" && \
		grep -q 'interpreter-block-ready.*detail=25' "$$run_dir/cbz-cbnz.interpreter.err" && \
		grep -q 'interpreter-dispatch.*detail=25' "$$run_dir/cbz-cbnz.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_TBZ_TBNZ_MAIN)" >"$$run_dir/tbz-tbnz.native.out" 2>"$$run_dir/tbz-tbnz.native.err"; tbz_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_TBZ_TBNZ_MAIN)" >"$$run_dir/tbz-tbnz.translated.out" 2>"$$run_dir/tbz-tbnz.translated.err"; tbz_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_TBZ_TBNZ_MAIN)" >"$$run_dir/tbz-tbnz.interpreter.out" 2>"$$run_dir/tbz-tbnz.interpreter.err"; tbz_interpreter_rc=$$?; \
		set -e; \
		test "$$tbz_native_rc" -eq 42 && test "$$tbz_translated_rc" -eq "$$tbz_native_rc" && \
		test "$$tbz_interpreter_rc" -eq "$$tbz_native_rc" && \
		cmp "$$run_dir/tbz-tbnz.native.out" "$$run_dir/tbz-tbnz.translated.out" && \
		cmp "$$run_dir/tbz-tbnz.native.out" "$$run_dir/tbz-tbnz.interpreter.out" && \
		cmp "$$run_dir/tbz-tbnz.native.err" "$$run_dir/tbz-tbnz.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/tbz-tbnz.interpreter.err" && \
		grep -q 'interpreter-block-ready.*detail=21' "$$run_dir/tbz-tbnz.interpreter.err" && \
		grep -q 'interpreter-dispatch.*detail=21' "$$run_dir/tbz-tbnz.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_ADD_SUB_MAIN)" >"$$run_dir/add-sub.native.out" 2>"$$run_dir/add-sub.native.err"; add_sub_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_ADD_SUB_MAIN)" >"$$run_dir/add-sub.translated.out" 2>"$$run_dir/add-sub.translated.err"; add_sub_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_ADD_SUB_MAIN)" >"$$run_dir/add-sub.interpreter.out" 2>"$$run_dir/add-sub.interpreter.err"; add_sub_interpreter_rc=$$?; \
		printf '%s\n' "$$add_sub_native_rc" >"$$run_dir/add-sub.native.rc"; \
		printf '%s\n' "$$add_sub_translated_rc" >"$$run_dir/add-sub.translated.rc"; \
		printf '%s\n' "$$add_sub_interpreter_rc" >"$$run_dir/add-sub.interpreter.rc"; \
		set -e; \
		test "$$add_sub_native_rc" -eq 42 && test "$$add_sub_translated_rc" -eq "$$add_sub_native_rc" && \
		test "$$add_sub_interpreter_rc" -eq "$$add_sub_native_rc" && \
		cmp "$$run_dir/add-sub.native.out" "$$run_dir/add-sub.translated.out" && \
		cmp "$$run_dir/add-sub.native.out" "$$run_dir/add-sub.interpreter.out" && \
		cmp "$$run_dir/add-sub.native.err" "$$run_dir/add-sub.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/add-sub.interpreter.err" && \
		grep -q 'interpreter-block-ready.*detail=4' "$$run_dir/add-sub.interpreter.err" && \
		grep -q 'interpreter-dispatch.*detail=4' "$$run_dir/add-sub.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_ADD_SUB_SP_MAIN)" >"$$run_dir/add-sub-sp.native.out" 2>"$$run_dir/add-sub-sp.native.err"; add_sub_sp_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_ADD_SUB_SP_MAIN)" >"$$run_dir/add-sub-sp.translated.out" 2>"$$run_dir/add-sub-sp.translated.err"; add_sub_sp_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_ADD_SUB_SP_MAIN)" >"$$run_dir/add-sub-sp.interpreter.out" 2>"$$run_dir/add-sub-sp.interpreter.err"; add_sub_sp_interpreter_rc=$$?; \
		printf '%s\n' "$$add_sub_sp_native_rc" >"$$run_dir/add-sub-sp.native.rc"; \
		printf '%s\n' "$$add_sub_sp_translated_rc" >"$$run_dir/add-sub-sp.translated.rc"; \
		printf '%s\n' "$$add_sub_sp_interpreter_rc" >"$$run_dir/add-sub-sp.interpreter.rc"; \
		set -e; \
		test "$$add_sub_sp_native_rc" -eq 42 && test "$$add_sub_sp_translated_rc" -eq "$$add_sub_sp_native_rc" && \
		test "$$add_sub_sp_interpreter_rc" -eq "$$add_sub_sp_native_rc" && \
		cmp "$$run_dir/add-sub-sp.native.out" "$$run_dir/add-sub-sp.translated.out" && \
		cmp "$$run_dir/add-sub-sp.native.out" "$$run_dir/add-sub-sp.interpreter.out" && \
		cmp "$$run_dir/add-sub-sp.native.err" "$$run_dir/add-sub-sp.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/add-sub-sp.interpreter.err" && \
		grep -q 'interpreter-block-ready.*pc=0x42d8 detail=10' "$$run_dir/add-sub-sp.interpreter.err" && \
		grep -q 'interpreter-dispatch.*pc=0x42d8 detail=10' "$$run_dir/add-sub-sp.interpreter.err" && \
		awk '/event=interpreter-dispatch/ { if ($$5 != "pc=0x42d8" || $$6 != "detail=10" || dispatched++) bad=1; next } \
		     /event=dispatch-done/ { if (!dispatched || $$5 != "pc=0x42d8" || $$6 !~ /^detail=[0-9]+$$/ || completed++) bad=1 } \
		     END { if (dispatched != 1 || completed != 1 || bad) exit 1 }' "$$run_dir/add-sub-sp.interpreter.err" && \
		test "$$(grep -c 'event=interpreter-dispatch' "$$run_dir/add-sub-sp.interpreter.err")" -eq 1 && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/add-sub-sp.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_BCOND_MAIN)" >"$$run_dir/bcond.native.out" 2>"$$run_dir/bcond.native.err"; bcond_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_BCOND_MAIN)" >"$$run_dir/bcond.translated.out" 2>"$$run_dir/bcond.translated.err"; bcond_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_BCOND_MAIN)" >"$$run_dir/bcond.interpreter.out" 2>"$$run_dir/bcond.interpreter.err"; bcond_interpreter_rc=$$?; \
		printf '%s\n' "$$bcond_native_rc" >"$$run_dir/bcond.native.rc"; \
		printf '%s\n' "$$bcond_translated_rc" >"$$run_dir/bcond.translated.rc"; \
		printf '%s\n' "$$bcond_interpreter_rc" >"$$run_dir/bcond.interpreter.rc"; \
		set -e; \
		test "$$bcond_native_rc" -eq 42 && test "$$bcond_translated_rc" -eq "$$bcond_native_rc" && \
		test "$$bcond_interpreter_rc" -eq "$$bcond_native_rc" && \
		cmp "$$run_dir/bcond.native.out" "$$run_dir/bcond.translated.out" && \
		cmp "$$run_dir/bcond.native.out" "$$run_dir/bcond.interpreter.out" && \
		cmp "$$run_dir/bcond.native.err" "$$run_dir/bcond.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/bcond.interpreter.err" && \
		grep -q 'interpreter-block-ready' "$$run_dir/bcond.interpreter.err" && \
		grep -q 'interpreter-dispatch' "$$run_dir/bcond.interpreter.err" && \
		awk '/event=interpreter-dispatch/ { seen=1; next } seen && /event=compile-start/ { found=1 } END { exit found }' "$$run_dir/bcond.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_CSEL_MAIN)" >"$$run_dir/csel.native.out" 2>"$$run_dir/csel.native.err"; csel_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_CSEL_MAIN)" >"$$run_dir/csel.translated.out" 2>"$$run_dir/csel.translated.err"; csel_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_CSEL_MAIN)" >"$$run_dir/csel.interpreter.out" 2>"$$run_dir/csel.interpreter.err"; csel_interpreter_rc=$$?; \
		printf '%s\n' "$$csel_native_rc" >"$$run_dir/csel.native.rc"; \
		printf '%s\n' "$$csel_translated_rc" >"$$run_dir/csel.translated.rc"; \
		printf '%s\n' "$$csel_interpreter_rc" >"$$run_dir/csel.interpreter.rc"; \
		set -e; \
		test "$$csel_native_rc" -eq 42 && test "$$csel_translated_rc" -eq "$$csel_native_rc" && \
		test "$$csel_interpreter_rc" -eq "$$csel_native_rc" && \
		cmp "$$run_dir/csel.native.out" "$$run_dir/csel.translated.out" && \
		cmp "$$run_dir/csel.native.out" "$$run_dir/csel.interpreter.out" && \
		cmp "$$run_dir/csel.native.err" "$$run_dir/csel.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/csel.interpreter.err" && \
		grep -q 'interpreter-block-ready' "$$run_dir/csel.interpreter.err" && \
		grep -q 'interpreter-dispatch' "$$run_dir/csel.interpreter.err" && \
		awk '/event=interpreter-dispatch/ { seen=1; next } seen && /event=compile-start/ { found=1 } END { exit found }' "$$run_dir/csel.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_ADD_SUB_SHIFTED_MAIN)" >"$$run_dir/add-sub-shifted.native.out" 2>"$$run_dir/add-sub-shifted.native.err"; shifted_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_ADD_SUB_SHIFTED_MAIN)" >"$$run_dir/add-sub-shifted.translated.out" 2>"$$run_dir/add-sub-shifted.translated.err"; shifted_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_ADD_SUB_SHIFTED_MAIN)" >"$$run_dir/add-sub-shifted.interpreter.out" 2>"$$run_dir/add-sub-shifted.interpreter.err"; shifted_interpreter_rc=$$?; \
		printf '%s\n' "$$shifted_native_rc" >"$$run_dir/add-sub-shifted.native.rc"; \
		printf '%s\n' "$$shifted_translated_rc" >"$$run_dir/add-sub-shifted.translated.rc"; \
		printf '%s\n' "$$shifted_interpreter_rc" >"$$run_dir/add-sub-shifted.interpreter.rc"; \
		set -e; \
		test "$$shifted_native_rc" -eq 42 && test "$$shifted_translated_rc" -eq "$$shifted_native_rc" && \
		test "$$shifted_interpreter_rc" -eq "$$shifted_native_rc" && \
		cmp "$$run_dir/add-sub-shifted.native.out" "$$run_dir/add-sub-shifted.translated.out" && \
		cmp "$$run_dir/add-sub-shifted.native.out" "$$run_dir/add-sub-shifted.interpreter.out" && \
		cmp "$$run_dir/add-sub-shifted.native.err" "$$run_dir/add-sub-shifted.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/add-sub-shifted.interpreter.err" && \
		grep -q 'interpreter-dispatch.*pc=0x42d8' "$$run_dir/add-sub-shifted.interpreter.err" && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/add-sub-shifted.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_ADD_SUB_EXTENDED_MAIN)" >"$$run_dir/add-sub-extended.native.out" 2>"$$run_dir/add-sub-extended.native.err"; extended_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_ADD_SUB_EXTENDED_MAIN)" >"$$run_dir/add-sub-extended.translated.out" 2>"$$run_dir/add-sub-extended.translated.err"; extended_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_ADD_SUB_EXTENDED_MAIN)" >"$$run_dir/add-sub-extended.interpreter.out" 2>"$$run_dir/add-sub-extended.interpreter.err"; extended_interpreter_rc=$$?; \
		printf '%s\n' "$$extended_native_rc" >"$$run_dir/add-sub-extended.native.rc"; \
		printf '%s\n' "$$extended_translated_rc" >"$$run_dir/add-sub-extended.translated.rc"; \
		printf '%s\n' "$$extended_interpreter_rc" >"$$run_dir/add-sub-extended.interpreter.rc"; \
		set -e; \
		test "$$extended_native_rc" -eq 42 && test "$$extended_translated_rc" -eq "$$extended_native_rc" && \
		test "$$extended_interpreter_rc" -eq "$$extended_native_rc" && \
		cmp "$$run_dir/add-sub-extended.native.out" "$$run_dir/add-sub-extended.translated.out" && \
		cmp "$$run_dir/add-sub-extended.native.out" "$$run_dir/add-sub-extended.interpreter.out" && \
		cmp "$$run_dir/add-sub-extended.native.err" "$$run_dir/add-sub-extended.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/add-sub-extended.interpreter.err" && \
		grep -q 'interpreter-dispatch.*pc=0x42d8' "$$run_dir/add-sub-extended.interpreter.err" && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/add-sub-extended.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_PAIR_MEMORY_MAIN)" >"$$run_dir/pair-memory.native.out" 2>"$$run_dir/pair-memory.native.err"; pair_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_PAIR_MEMORY_MAIN)" >"$$run_dir/pair-memory.translated.out" 2>"$$run_dir/pair-memory.translated.err"; pair_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_PAIR_MEMORY_MAIN)" >"$$run_dir/pair-memory.interpreter.out" 2>"$$run_dir/pair-memory.interpreter.err"; pair_interpreter_rc=$$?; \
		printf '%s\n' "$$pair_native_rc" >"$$run_dir/pair-memory.native.rc"; \
		printf '%s\n' "$$pair_translated_rc" >"$$run_dir/pair-memory.translated.rc"; \
		printf '%s\n' "$$pair_interpreter_rc" >"$$run_dir/pair-memory.interpreter.rc"; \
		set -e; \
		test "$$pair_native_rc" -eq 42 && test "$$pair_translated_rc" -eq "$$pair_native_rc" && \
		test "$$pair_interpreter_rc" -eq "$$pair_native_rc" && \
		cmp "$$run_dir/pair-memory.native.out" "$$run_dir/pair-memory.translated.out" && \
		cmp "$$run_dir/pair-memory.native.out" "$$run_dir/pair-memory.interpreter.out" && \
		cmp "$$run_dir/pair-memory.native.err" "$$run_dir/pair-memory.translated.err" && \
		grep -q 'interpreter-block-ready.*detail=26' "$$run_dir/pair-memory.interpreter.err" && \
		grep -q 'interpreter-dispatch.*detail=26' "$$run_dir/pair-memory.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_SCALAR_INDEXED_MEMORY_MAIN)" >"$$run_dir/scalar-indexed-memory.native.out" 2>"$$run_dir/scalar-indexed-memory.native.err"; indexed_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_SCALAR_INDEXED_MEMORY_MAIN)" >"$$run_dir/scalar-indexed-memory.translated.out" 2>"$$run_dir/scalar-indexed-memory.translated.err"; indexed_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_SCALAR_INDEXED_MEMORY_MAIN)" >"$$run_dir/scalar-indexed-memory.interpreter.out" 2>"$$run_dir/scalar-indexed-memory.interpreter.err"; indexed_interpreter_rc=$$?; \
		printf '%s\n' "$$indexed_native_rc" >"$$run_dir/scalar-indexed-memory.native.rc"; \
		printf '%s\n' "$$indexed_translated_rc" >"$$run_dir/scalar-indexed-memory.translated.rc"; \
		printf '%s\n' "$$indexed_interpreter_rc" >"$$run_dir/scalar-indexed-memory.interpreter.rc"; \
		set -e; \
		test "$$indexed_native_rc" -eq 42 && test "$$indexed_translated_rc" -eq "$$indexed_native_rc" && \
		test "$$indexed_interpreter_rc" -eq "$$indexed_native_rc" && \
		cmp "$$run_dir/scalar-indexed-memory.native.out" "$$run_dir/scalar-indexed-memory.translated.out" && \
		cmp "$$run_dir/scalar-indexed-memory.native.out" "$$run_dir/scalar-indexed-memory.interpreter.out" && \
		cmp "$$run_dir/scalar-indexed-memory.native.err" "$$run_dir/scalar-indexed-memory.translated.err" && \
		sed '/^arm64-metal trace /d' "$$run_dir/scalar-indexed-memory.interpreter.err" >"$$run_dir/scalar-indexed-memory.interpreter.no-trace.err" && \
		cmp "$$run_dir/scalar-indexed-memory.native.err" "$$run_dir/scalar-indexed-memory.interpreter.no-trace.err" && \
		grep -q 'interpreter-block-ready' "$$run_dir/scalar-indexed-memory.interpreter.err" && \
		grep -q 'interpreter-dispatch' "$$run_dir/scalar-indexed-memory.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_LOGICAL_SHIFTED_MAIN)" >"$$run_dir/logical-shifted.native.out" 2>"$$run_dir/logical-shifted.native.err"; logical_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_LOGICAL_SHIFTED_MAIN)" >"$$run_dir/logical-shifted.translated.out" 2>"$$run_dir/logical-shifted.translated.err"; logical_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_LOGICAL_SHIFTED_MAIN)" >"$$run_dir/logical-shifted.interpreter.out" 2>"$$run_dir/logical-shifted.interpreter.err"; logical_interpreter_rc=$$?; \
		printf '%s\n' "$$logical_native_rc" >"$$run_dir/logical-shifted.native.rc"; \
		printf '%s\n' "$$logical_translated_rc" >"$$run_dir/logical-shifted.translated.rc"; \
		printf '%s\n' "$$logical_interpreter_rc" >"$$run_dir/logical-shifted.interpreter.rc"; \
		set -e; \
		test "$$logical_native_rc" -eq 42 && test "$$logical_translated_rc" -eq "$$logical_native_rc" && \
		test "$$logical_interpreter_rc" -eq "$$logical_native_rc" && \
		cmp "$$run_dir/logical-shifted.native.out" "$$run_dir/logical-shifted.translated.out" && \
		cmp "$$run_dir/logical-shifted.native.out" "$$run_dir/logical-shifted.interpreter.out" && \
		cmp "$$run_dir/logical-shifted.native.err" "$$run_dir/logical-shifted.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/logical-shifted.interpreter.err" && \
		grep -q 'interpreter-dispatch' "$$run_dir/logical-shifted.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_LOGICAL_VARIABLE_SHIFTS_MAIN)" >"$$run_dir/logical-variable-shifts.native.out" 2>"$$run_dir/logical-variable-shifts.native.err"; logical_variable_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_LOGICAL_VARIABLE_SHIFTS_MAIN)" >"$$run_dir/logical-variable-shifts.translated.out" 2>"$$run_dir/logical-variable-shifts.translated.err"; logical_variable_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_LOGICAL_VARIABLE_SHIFTS_MAIN)" >"$$run_dir/logical-variable-shifts.interpreter.out" 2>"$$run_dir/logical-variable-shifts.interpreter.err"; logical_variable_interpreter_rc=$$?; \
		printf '%s\n' "$$logical_variable_native_rc" >"$$run_dir/logical-variable-shifts.native.rc"; \
		printf '%s\n' "$$logical_variable_translated_rc" >"$$run_dir/logical-variable-shifts.translated.rc"; \
		printf '%s\n' "$$logical_variable_interpreter_rc" >"$$run_dir/logical-variable-shifts.interpreter.rc"; \
		set -e; \
		test "$$logical_variable_native_rc" -eq 42 && test "$$logical_variable_translated_rc" -eq "$$logical_variable_native_rc" && \
		test "$$logical_variable_interpreter_rc" -eq "$$logical_variable_native_rc" && \
		cmp "$$run_dir/logical-variable-shifts.native.out" "$$run_dir/logical-variable-shifts.translated.out" && \
		cmp "$$run_dir/logical-variable-shifts.native.out" "$$run_dir/logical-variable-shifts.interpreter.out" && \
		cmp "$$run_dir/logical-variable-shifts.native.err" "$$run_dir/logical-variable-shifts.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/logical-variable-shifts.interpreter.err" && \
		grep -q 'interpreter-dispatch' "$$run_dir/logical-variable-shifts.interpreter.err" && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/logical-variable-shifts.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_MULTIPLY_ACCUMULATE_MAIN)" >"$$run_dir/multiply-accumulate.native.out" 2>"$$run_dir/multiply-accumulate.native.err"; multiply_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_MULTIPLY_ACCUMULATE_MAIN)" >"$$run_dir/multiply-accumulate.translated.out" 2>"$$run_dir/multiply-accumulate.translated.err"; multiply_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_MULTIPLY_ACCUMULATE_MAIN)" >"$$run_dir/multiply-accumulate.interpreter.out" 2>"$$run_dir/multiply-accumulate.interpreter.err"; multiply_interpreter_rc=$$?; \
		printf '%s\n' "$$multiply_native_rc" >"$$run_dir/multiply-accumulate.native.rc"; \
		printf '%s\n' "$$multiply_translated_rc" >"$$run_dir/multiply-accumulate.translated.rc"; \
		printf '%s\n' "$$multiply_interpreter_rc" >"$$run_dir/multiply-accumulate.interpreter.rc"; \
		set -e; \
		test "$$multiply_native_rc" -eq 42 && test "$$multiply_translated_rc" -eq "$$multiply_native_rc" && \
		test "$$multiply_interpreter_rc" -eq "$$multiply_native_rc" && \
		cmp "$$run_dir/multiply-accumulate.native.out" "$$run_dir/multiply-accumulate.translated.out" && \
		cmp "$$run_dir/multiply-accumulate.native.out" "$$run_dir/multiply-accumulate.interpreter.out" && \
		cmp "$$run_dir/multiply-accumulate.native.err" "$$run_dir/multiply-accumulate.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/multiply-accumulate.interpreter.err" && \
		grep -q 'interpreter-dispatch' "$$run_dir/multiply-accumulate.interpreter.err" && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/multiply-accumulate.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_BITFIELD_NATIVE)" >"$$run_dir/bitfield.native.out" 2>"$$run_dir/bitfield.native.err"; bitfield_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_BITFIELD_MAIN)" >"$$run_dir/bitfield.translated.out" 2>"$$run_dir/bitfield.translated.err"; bitfield_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_BITFIELD_MAIN)" >"$$run_dir/bitfield.interpreter.out" 2>"$$run_dir/bitfield.interpreter.err"; bitfield_interpreter_rc=$$?; \
		printf '%s\n' "$$bitfield_native_rc" >"$$run_dir/bitfield.native.rc"; \
		printf '%s\n' "$$bitfield_translated_rc" >"$$run_dir/bitfield.translated.rc"; \
		printf '%s\n' "$$bitfield_interpreter_rc" >"$$run_dir/bitfield.interpreter.rc"; \
		set -e; \
		test "$$bitfield_native_rc" -eq 42 && test "$$bitfield_translated_rc" -eq "$$bitfield_native_rc" && \
		test "$$bitfield_interpreter_rc" -eq "$$bitfield_native_rc" && \
		cmp "$$run_dir/bitfield.native.out" "$$run_dir/bitfield.translated.out" && \
		cmp "$$run_dir/bitfield.native.out" "$$run_dir/bitfield.interpreter.out" && \
		cmp "$$run_dir/bitfield.native.err" "$$run_dir/bitfield.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/bitfield.interpreter.err" && \
		grep -q 'interpreter-dispatch' "$$run_dir/bitfield.interpreter.err" && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/bitfield.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_LOGICAL_IMMEDIATE_MAIN)" >"$$run_dir/logical-immediate.native.out" 2>"$$run_dir/logical-immediate.native.err"; logical_immediate_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_LOGICAL_IMMEDIATE_MAIN)" >"$$run_dir/logical-immediate.translated.out" 2>"$$run_dir/logical-immediate.translated.err"; logical_immediate_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_LOGICAL_IMMEDIATE_MAIN)" >"$$run_dir/logical-immediate.interpreter.out" 2>"$$run_dir/logical-immediate.interpreter.err"; logical_immediate_interpreter_rc=$$?; \
		printf '%s\n' "$$logical_immediate_native_rc" >"$$run_dir/logical-immediate.native.rc"; \
		printf '%s\n' "$$logical_immediate_translated_rc" >"$$run_dir/logical-immediate.translated.rc"; \
		printf '%s\n' "$$logical_immediate_interpreter_rc" >"$$run_dir/logical-immediate.interpreter.rc"; \
		set -e; \
		test "$$logical_immediate_native_rc" -eq 42 && test "$$logical_immediate_translated_rc" -eq "$$logical_immediate_native_rc" && \
		test "$$logical_immediate_interpreter_rc" -eq "$$logical_immediate_native_rc" && \
		cmp "$$run_dir/logical-immediate.native.out" "$$run_dir/logical-immediate.translated.out" && \
		cmp "$$run_dir/logical-immediate.native.out" "$$run_dir/logical-immediate.interpreter.out" && \
		cmp "$$run_dir/logical-immediate.native.err" "$$run_dir/logical-immediate.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/logical-immediate.interpreter.err" && \
		grep -q 'interpreter-dispatch.*pc=0x42d8' "$$run_dir/logical-immediate.interpreter.err" && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/logical-immediate.interpreter.err" || exit 1; \
		ulimit -c 0; \
		set +e; \
		"$(INTERPRETER_LOGICAL_IMMEDIATE_INVALID_MAIN)" >"$$run_dir/logical-immediate-invalid.native.out" 2>"$$run_dir/logical-immediate-invalid.native.err"; logical_invalid_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_LOGICAL_IMMEDIATE_INVALID_MAIN)" >"$$run_dir/logical-immediate-invalid.translated.out" 2>"$$run_dir/logical-immediate-invalid.translated.err"; logical_invalid_translated_rc=$$?; \
		set -e; \
		test "$$logical_invalid_native_rc" -eq 132 && test "$$logical_invalid_translated_rc" -ne 0 && \
		grep -q 'Reserved logical immediate' "$$run_dir/logical-immediate-invalid.translated.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_ADR_ADRP_MAIN)" >"$$run_dir/adr-adrp.native.out" 2>"$$run_dir/adr-adrp.native.err"; adr_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_ADR_ADRP_MAIN)" >"$$run_dir/adr-adrp.translated.out" 2>"$$run_dir/adr-adrp.translated.err"; adr_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_ADR_ADRP_MAIN)" >"$$run_dir/adr-adrp.interpreter.out" 2>"$$run_dir/adr-adrp.interpreter.err"; adr_interpreter_rc=$$?; \
		set -e; \
		test "$$adr_native_rc" -eq 42 && test "$$adr_translated_rc" -eq "$$adr_native_rc" && \
		test "$$adr_interpreter_rc" -eq "$$adr_native_rc" && \
		cmp "$$run_dir/adr-adrp.native.out" "$$run_dir/adr-adrp.translated.out" && \
		cmp "$$run_dir/adr-adrp.native.out" "$$run_dir/adr-adrp.interpreter.out" && \
		cmp "$$run_dir/adr-adrp.native.err" "$$run_dir/adr-adrp.translated.err" && \
		grep -q 'interpreter-dispatch.*pc=0x' "$$run_dir/adr-adrp.interpreter.err" && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/adr-adrp.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_INTEGER_BITOPS_MAIN)" >"$$run_dir/integer-bitops.native.out" 2>"$$run_dir/integer-bitops.native.err"; bitops_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_INTEGER_BITOPS_MAIN)" >"$$run_dir/integer-bitops.translated.out" 2>"$$run_dir/integer-bitops.translated.err"; bitops_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_INTEGER_BITOPS_MAIN)" >"$$run_dir/integer-bitops.interpreter.out" 2>"$$run_dir/integer-bitops.interpreter.err"; bitops_interpreter_rc=$$?; \
		set -e; \
		test "$$bitops_native_rc" -eq 42 && test "$$bitops_translated_rc" -eq "$$bitops_native_rc" && \
		test "$$bitops_interpreter_rc" -eq "$$bitops_native_rc" && \
		cmp "$$run_dir/integer-bitops.native.out" "$$run_dir/integer-bitops.translated.out" && \
		cmp "$$run_dir/integer-bitops.native.out" "$$run_dir/integer-bitops.interpreter.out" && \
		cmp "$$run_dir/integer-bitops.native.err" "$$run_dir/integer-bitops.translated.err" && \
		grep -q 'interpreter-dispatch.*pc=0x42d8' "$$run_dir/integer-bitops.interpreter.err" && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/integer-bitops.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_INTEGER_DIVIDE_MAIN)" >"$$run_dir/integer-divide.native.out" 2>"$$run_dir/integer-divide.native.err"; divide_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_INTEGER_DIVIDE_MAIN)" >"$$run_dir/integer-divide.translated.out" 2>"$$run_dir/integer-divide.translated.err"; divide_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_INTEGER_DIVIDE_MAIN)" >"$$run_dir/integer-divide.interpreter.out" 2>"$$run_dir/integer-divide.interpreter.err"; divide_interpreter_rc=$$?; \
		set -e; \
		test "$$divide_native_rc" -eq 42 && test "$$divide_translated_rc" -eq "$$divide_native_rc" && \
		test "$$divide_interpreter_rc" -eq "$$divide_native_rc" && \
		cmp "$$run_dir/integer-divide.native.out" "$$run_dir/integer-divide.translated.out" && \
		cmp "$$run_dir/integer-divide.native.out" "$$run_dir/integer-divide.interpreter.out" && \
		cmp "$$run_dir/integer-divide.native.err" "$$run_dir/integer-divide.translated.err" && \
		grep -q 'interpreter-dispatch.*pc=0x42d8' "$$run_dir/integer-divide.interpreter.err" && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/integer-divide.interpreter.err" || exit 1; \
		set +e; \
		"$(INTERPRETER_LITERAL_LOAD_MAIN)" >"$$run_dir/literal-load.native.out" 2>"$$run_dir/literal-load.native.err"; literal_native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_LITERAL_LOAD_MAIN)" >"$$run_dir/literal-load.translated.out" 2>"$$run_dir/literal-load.translated.err"; literal_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_LITERAL_LOAD_MAIN)" >"$$run_dir/literal-load.interpreter.out" 2>"$$run_dir/literal-load.interpreter.err"; literal_interpreter_rc=$$?; \
		set -e; \
		test "$$literal_native_rc" -eq 42 && test "$$literal_translated_rc" -eq "$$literal_native_rc" && \
		test "$$literal_interpreter_rc" -eq "$$literal_native_rc" && \
		cmp "$$run_dir/literal-load.native.out" "$$run_dir/literal-load.translated.out" && \
		cmp "$$run_dir/literal-load.native.out" "$$run_dir/literal-load.interpreter.out" && \
		cmp "$$run_dir/literal-load.native.err" "$$run_dir/literal-load.translated.err" && \
		grep -q 'interpreter-dispatch.*pc=0x' "$$run_dir/literal-load.interpreter.err" && \
		awk '/event=compile-start/ { found=1 } END { exit found }' "$$run_dir/literal-load.interpreter.err" && \
		sed '/^arm64-metal trace /d' "$$run_dir/literal-load.interpreter.err" >"$$run_dir/literal-load.interpreter.no-trace.err" && \
		cmp "$$run_dir/literal-load.native.err" "$$run_dir/literal-load.interpreter.no-trace.err" || exit 1; \
		echo "verify-interpreter receipts: $$run_dir"

.PHONY: verify-interpreter-demand-cache-vector
verify-interpreter-demand-cache-vector: $(APP) $(INTERPRETER_DEMAND_CACHE_VECTOR_POST_MAIN) $(INTERPRETER_DEMAND_CACHE_VECTOR_POST_NATIVE) $(INTERPRETER_DEMAND_CACHE_VECTOR_PRE_MAIN) $(INTERPRETER_DEMAND_CACHE_VECTOR_PRE_NATIVE) $(INTERPRETER_METALLIB)
	@set -eu; run_dir=$$(mktemp -d "$(BUILD_DIR)/interpreter-demand-cache-vector.XXXXXX"); \
	run_case() { \
		case_name=$$1; main=$$2; native=$$3; \
		set +e; \
		"$$native" >"$$run_dir/$$case_name.native.out" 2>"$$run_dir/$$case_name.native.err"; native_rc=$$?; \
		ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 "$(APP)" launch "$$main" >"$$run_dir/$$case_name.translated.out" 2>"$$run_dir/$$case_name.translated.err"; translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 ARM64_METAL_TRACE_INTERPRETER_PACKETS=1 "$(APP)" launch "$$main" >"$$run_dir/$$case_name.interpreter.out" 2>"$$run_dir/$$case_name.interpreter.err"; interpreter_rc=$$?; \
		set -e; \
		printf '%s\n' "$$native_rc" >"$$run_dir/$$case_name.native.rc"; \
		printf '%s\n' "$$translated_rc" >"$$run_dir/$$case_name.translated.rc"; \
		printf '%s\n' "$$interpreter_rc" >"$$run_dir/$$case_name.interpreter.rc"; \
		test "$$native_rc" -ne 99 && test "$$translated_rc" -eq "$$native_rc" && test "$$interpreter_rc" -eq "$$native_rc"; \
		cmp "$$run_dir/$$case_name.native.out" "$$run_dir/$$case_name.translated.out"; \
		cmp "$$run_dir/$$case_name.native.out" "$$run_dir/$$case_name.interpreter.out"; \
		cmp "$$run_dir/$$case_name.native.err" "$$run_dir/$$case_name.translated.err"; \
		sed '/^arm64-metal trace /d' "$$run_dir/$$case_name.interpreter.err" >"$$run_dir/$$case_name.interpreter.no-trace.err"; \
		cmp "$$run_dir/$$case_name.native.err" "$$run_dir/$$case_name.interpreter.no-trace.err"; \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$$run_dir/$$case_name.interpreter.err"; \
		awk '$$4 == "event=interpreter-packet" && $$6 == "detail=43" { packets[$$5]=1 } \
		     $$4 == "event=interpreter-cache-fault" { faults++; fault_pc=$$5; fault_addr=$$6; fault_line=NR; if (fault_addr !~ /^detail=/) bad=1 } \
		     $$4 == "event=interpreter-cache-fault-width" { widths++; if ($$5 != fault_pc || $$6 != "detail=16") bad=1 } \
		     $$4 == "event=interpreter-cache-resident" { residents++; if (NR <= fault_line || $$5 != fault_pc || $$6 != fault_addr) bad=1; resident_line=NR } \
		     $$4 == "event=interpreter-dispatch" && residents && !resumed && NR > resident_line { resumed=1; if ($$5 != fault_pc) bad=1 } \
		     $$4 == "event=compile-start" { compile=1 } \
		     END { if (!fault_pc || !packets[fault_pc] || faults != 1 || widths != 1 || residents != 1 || !resumed || compile || bad) exit 1 }' \
			"$$run_dir/$$case_name.interpreter.err"; \
		printf '%s vector demand-cache retry: exit=%s fault-pc=%s fault=%s receipts=%s\n' "$$case_name" "$$interpreter_rc" "$$(awk '$$4 == "event=interpreter-cache-fault" {print $$5, $$6; exit}' "$$run_dir/$$case_name.interpreter.err")" "$$run_dir"; \
	}; \
	run_case post "$(INTERPRETER_DEMAND_CACHE_VECTOR_POST_MAIN)" "$(INTERPRETER_DEMAND_CACHE_VECTOR_POST_NATIVE)"; \
	run_case pre "$(INTERPRETER_DEMAND_CACHE_VECTOR_PRE_MAIN)" "$(INTERPRETER_DEMAND_CACHE_VECTOR_PRE_NATIVE)"; \
	echo "Vector Q pre/post demand-cache controls passed; receipts=$$run_dir"

verify-interpreter-demand-cache: verify-interpreter-demand-cache-vector $(APP) $(INTERPRETER_DEMAND_CACHE_MAIN) $(INTERPRETER_DEMAND_CACHE_NATIVE) $(INTERPRETER_DEMAND_CACHE_SCALAR_MAIN) $(INTERPRETER_DEMAND_CACHE_SCALAR_NATIVE) $(INTERPRETER_DEMAND_CACHE_INDEXED_MAIN) $(INTERPRETER_DEMAND_CACHE_INDEXED_NATIVE) $(INTERPRETER_DEMAND_CACHE_BYTE_MAIN) $(INTERPRETER_DEMAND_CACHE_BYTE_NATIVE) $(INTERPRETER_DEMAND_CACHE_HALF_MAIN) $(INTERPRETER_DEMAND_CACHE_HALF_NATIVE) $(INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_MAIN) $(INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_NATIVE) $(INTERPRETER_DEMAND_CACHE_SIGNED_HALF_MAIN) $(INTERPRETER_DEMAND_CACHE_SIGNED_HALF_NATIVE) $(INTERPRETER_DEMAND_CACHE_SIGNED_WORD_MAIN) $(INTERPRETER_DEMAND_CACHE_SIGNED_WORD_NATIVE) $(INTERPRETER_METALLIB)
	@set -eu; \
		run_dir=$$(mktemp -d "$(BUILD_DIR)/interpreter-demand-cache.XXXXXX"); \
		set +e; \
		"$(INTERPRETER_DEMAND_CACHE_NATIVE)" >"$$run_dir/native.out" 2>"$$run_dir/native.err"; native_rc=$$?; \
		ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_MAIN)" >"$$run_dir/translated.out" 2>"$$run_dir/translated.err"; translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_CACHE_DEMAND=1 ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_MAIN)" >"$$run_dir/interpreter.out" 2>"$$run_dir/interpreter.err"; interpreter_rc=$$?; \
		set -e; \
		printf '%s\n' "$$native_rc" >"$$run_dir/native.rc"; \
		printf '%s\n' "$$translated_rc" >"$$run_dir/translated.rc"; \
		printf '%s\n' "$$interpreter_rc" >"$$run_dir/interpreter.rc"; \
		test "$$native_rc" -eq 32; \
		test "$$translated_rc" -eq "$$native_rc"; \
		test "$$interpreter_rc" -eq "$$native_rc"; \
		cmp "$$run_dir/native.out" "$$run_dir/translated.out"; \
		cmp "$$run_dir/native.out" "$$run_dir/interpreter.out"; \
		cmp "$$run_dir/native.err" "$$run_dir/translated.err"; \
		sed '/^arm64-metal trace /d' "$$run_dir/interpreter.err" >"$$run_dir/interpreter.no-trace.err"; \
		cmp "$$run_dir/native.err" "$$run_dir/interpreter.no-trace.err"; \
		grep -q 'interpreter-block-ready.*detail=5' "$$run_dir/interpreter.err"; \
		awk '/event=interpreter-dispatch/ && !first { first=1; firstpc=$$5 } \
		     $$4 == "event=interpreter-cache-fault" { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
		     /event=interpreter-cache-resident/ { resident=1; if (!fault || NR <= faultline || $$5 != faultpc || $$6 != faultaddr) bad=1; residentline=NR } \
		     /event=interpreter-dispatch/ && resident && !resumed && NR > residentline { resumed=1; if ($$5 != faultpc) bad=1 } \
		     END { if (!first || !fault || !resident || !resumed || firstpc == faultpc || bad) exit 1 }' "$$run_dir/interpreter.err"; \
		set +e; \
		"$(INTERPRETER_DEMAND_CACHE_SCALAR_NATIVE)" >"$$run_dir/scalar.native.out" 2>"$$run_dir/scalar.native.err"; scalar_native_rc=$$?; \
		ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_SCALAR_MAIN)" >"$$run_dir/scalar.translated.out" 2>"$$run_dir/scalar.translated.err"; scalar_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_CACHE_DEMAND=1 ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_SCALAR_MAIN)" >"$$run_dir/scalar.interpreter.out" 2>"$$run_dir/scalar.interpreter.err"; scalar_interpreter_rc=$$?; \
		printf '%s\n' "$$scalar_native_rc" >"$$run_dir/scalar.native.rc"; \
		printf '%s\n' "$$scalar_translated_rc" >"$$run_dir/scalar.translated.rc"; \
		printf '%s\n' "$$scalar_interpreter_rc" >"$$run_dir/scalar.interpreter.rc"; \
		set -e; \
		test "$$scalar_native_rc" -eq 32; \
		test "$$scalar_translated_rc" -eq "$$scalar_native_rc"; \
		test "$$scalar_interpreter_rc" -eq "$$scalar_native_rc"; \
		cmp "$$run_dir/scalar.native.out" "$$run_dir/scalar.translated.out"; \
		cmp "$$run_dir/scalar.native.out" "$$run_dir/scalar.interpreter.out"; \
		cmp "$$run_dir/scalar.native.err" "$$run_dir/scalar.translated.err"; \
		sed '/^arm64-metal trace /d' "$$run_dir/scalar.interpreter.err" >"$$run_dir/scalar.interpreter.no-trace.err"; \
		cmp "$$run_dir/scalar.native.err" "$$run_dir/scalar.interpreter.no-trace.err"; \
		grep -q 'interpreter-block-ready.*detail=5' "$$run_dir/scalar.interpreter.err"; \
		awk '/event=interpreter-dispatch/ && !first { first=1; firstpc=$$5 } \
		     $$4 == "event=interpreter-cache-fault" { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
		     /event=interpreter-cache-resident/ { resident=1; if (!fault || NR <= faultline || $$5 != faultpc || $$6 != faultaddr) bad=1; residentline=NR } \
		     /event=interpreter-dispatch/ && resident && !resumed && NR > residentline { resumed=1; if ($$5 != faultpc) bad=1 } \
		     END { if (!first || !fault || !resident || !resumed || firstpc == faultpc || bad) exit 1 }' "$$run_dir/scalar.interpreter.err"; \
		set +e; \
		"$(INTERPRETER_DEMAND_CACHE_INDEXED_NATIVE)" >"$$run_dir/indexed.native.out" 2>"$$run_dir/indexed.native.err"; indexed_native_rc=$$?; \
		ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_INDEXED_MAIN)" >"$$run_dir/indexed.translated.out" 2>"$$run_dir/indexed.translated.err"; indexed_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_CACHE_DEMAND=1 ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_INDEXED_MAIN)" >"$$run_dir/indexed.interpreter.out" 2>"$$run_dir/indexed.interpreter.err"; indexed_interpreter_rc=$$?; \
		printf '%s\n' "$$indexed_native_rc" >"$$run_dir/indexed.native.rc"; \
		printf '%s\n' "$$indexed_translated_rc" >"$$run_dir/indexed.translated.rc"; \
		printf '%s\n' "$$indexed_interpreter_rc" >"$$run_dir/indexed.interpreter.rc"; \
		set -e; \
		test "$$indexed_native_rc" -eq 32; \
		test "$$indexed_translated_rc" -eq "$$indexed_native_rc"; \
		test "$$indexed_interpreter_rc" -eq "$$indexed_native_rc"; \
		cmp "$$run_dir/indexed.native.out" "$$run_dir/indexed.translated.out"; \
		cmp "$$run_dir/indexed.native.out" "$$run_dir/indexed.interpreter.out"; \
		cmp "$$run_dir/indexed.native.err" "$$run_dir/indexed.translated.err"; \
		sed '/^arm64-metal trace /d' "$$run_dir/indexed.interpreter.err" >"$$run_dir/indexed.interpreter.no-trace.err"; \
		cmp "$$run_dir/indexed.native.err" "$$run_dir/indexed.interpreter.no-trace.err"; \
		grep -q 'interpreter-block-ready.*detail=5' "$$run_dir/indexed.interpreter.err"; \
		awk '/event=interpreter-dispatch/ && !first { first=1; firstpc=$$5 } \
		     $$4 == "event=interpreter-cache-fault" { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
		     /event=interpreter-cache-resident/ { resident=1; if (!fault || NR <= faultline || $$5 != faultpc || $$6 != faultaddr) bad=1; residentline=NR } \
		     /event=interpreter-dispatch/ && resident && !resumed && NR > residentline { resumed=1; if ($$5 != faultpc) bad=1 } \
		     END { if (!first || !fault || !resident || !resumed || firstpc == faultpc || bad) exit 1 }' "$$run_dir/indexed.interpreter.err"; \
		set +e; \
		"$(INTERPRETER_DEMAND_CACHE_BYTE_NATIVE)" >"$$run_dir/byte.native.out" 2>"$$run_dir/byte.native.err"; byte_native_rc=$$?; \
		ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_BYTE_MAIN)" >"$$run_dir/byte.translated.out" 2>"$$run_dir/byte.translated.err"; byte_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_CACHE_DEMAND=1 ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_BYTE_MAIN)" >"$$run_dir/byte.interpreter.out" 2>"$$run_dir/byte.interpreter.err"; byte_interpreter_rc=$$?; \
		printf '%s\n' "$$byte_native_rc" >"$$run_dir/byte.native.rc"; \
		printf '%s\n' "$$byte_translated_rc" >"$$run_dir/byte.translated.rc"; \
		printf '%s\n' "$$byte_interpreter_rc" >"$$run_dir/byte.interpreter.rc"; \
		set -e; \
		test "$$byte_native_rc" -eq 32; \
		test "$$byte_translated_rc" -eq "$$byte_native_rc"; \
		test "$$byte_interpreter_rc" -eq "$$byte_native_rc"; \
		cmp "$$run_dir/byte.native.out" "$$run_dir/byte.translated.out"; \
		cmp "$$run_dir/byte.native.out" "$$run_dir/byte.interpreter.out"; \
		cmp "$$run_dir/byte.native.err" "$$run_dir/byte.translated.err"; \
		sed '/^arm64-metal trace /d' "$$run_dir/byte.interpreter.err" >"$$run_dir/byte.interpreter.no-trace.err"; \
		cmp "$$run_dir/byte.native.err" "$$run_dir/byte.interpreter.no-trace.err"; \
		grep -q 'interpreter-block-ready.*detail=5' "$$run_dir/byte.interpreter.err"; \
		awk '/event=interpreter-dispatch/ && !first { first=1; firstpc=$$5 } \
		     $$4 == "event=interpreter-cache-fault" { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
		     /event=interpreter-cache-resident/ { resident=1; if (!fault || NR <= faultline || $$5 != faultpc || $$6 != faultaddr) bad=1; residentline=NR } \
		     /event=interpreter-dispatch/ && resident && !resumed && NR > residentline { resumed=1; if ($$5 != faultpc) bad=1 } \
		     END { if (!first || !fault || !resident || !resumed || firstpc == faultpc || bad) exit 1 }' "$$run_dir/byte.interpreter.err"; \
		set +e; \
		"$(INTERPRETER_DEMAND_CACHE_HALF_NATIVE)" >"$$run_dir/half.native.out" 2>"$$run_dir/half.native.err"; half_native_rc=$$?; \
		ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_HALF_MAIN)" >"$$run_dir/half.translated.out" 2>"$$run_dir/half.translated.err"; half_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_CACHE_DEMAND=1 ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_HALF_MAIN)" >"$$run_dir/half.interpreter.out" 2>"$$run_dir/half.interpreter.err"; half_interpreter_rc=$$?; \
		printf '%s\n' "$$half_native_rc" >"$$run_dir/half.native.rc"; \
		printf '%s\n' "$$half_translated_rc" >"$$run_dir/half.translated.rc"; \
		printf '%s\n' "$$half_interpreter_rc" >"$$run_dir/half.interpreter.rc"; \
		set -e; \
		test "$$half_native_rc" -eq 32; \
		test "$$half_translated_rc" -eq "$$half_native_rc"; \
		test "$$half_interpreter_rc" -eq "$$half_native_rc"; \
		cmp "$$run_dir/half.native.out" "$$run_dir/half.translated.out"; \
		cmp "$$run_dir/half.native.out" "$$run_dir/half.interpreter.out"; \
		cmp "$$run_dir/half.native.err" "$$run_dir/half.translated.err"; \
		sed '/^arm64-metal trace /d' "$$run_dir/half.interpreter.err" >"$$run_dir/half.interpreter.no-trace.err"; \
		cmp "$$run_dir/half.native.err" "$$run_dir/half.interpreter.no-trace.err"; \
		grep -q 'interpreter-block-ready.*detail=5' "$$run_dir/half.interpreter.err"; \
		awk '/event=interpreter-dispatch/ && !first { first=1; firstpc=$$5 } \
		     $$4 == "event=interpreter-cache-fault" { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
		     /event=interpreter-cache-resident/ { resident=1; if (!fault || NR <= faultline || $$5 != faultpc || $$6 != faultaddr) bad=1; residentline=NR } \
		     /event=interpreter-dispatch/ && resident && !resumed && NR > residentline { resumed=1; if ($$5 != faultpc) bad=1 } \
		     END { if (!first || !fault || !resident || !resumed || firstpc == faultpc || bad) exit 1 }' "$$run_dir/half.interpreter.err"; \
		set +e; \
		"$(INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_NATIVE)" >"$$run_dir/signed-byte.native.out" 2>"$$run_dir/signed-byte.native.err"; signed_byte_native_rc=$$?; \
		ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_MAIN)" >"$$run_dir/signed-byte.translated.out" 2>"$$run_dir/signed-byte.translated.err"; signed_byte_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_CACHE_DEMAND=1 ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_MAIN)" >"$$run_dir/signed-byte.interpreter.out" 2>"$$run_dir/signed-byte.interpreter.err"; signed_byte_interpreter_rc=$$?; \
		printf '%s\n' "$$signed_byte_native_rc" >"$$run_dir/signed-byte.native.rc"; \
		printf '%s\n' "$$signed_byte_translated_rc" >"$$run_dir/signed-byte.translated.rc"; \
		printf '%s\n' "$$signed_byte_interpreter_rc" >"$$run_dir/signed-byte.interpreter.rc"; \
		set -e; \
		test "$$signed_byte_native_rc" -eq 32; \
		test "$$signed_byte_translated_rc" -eq "$$signed_byte_native_rc"; \
		test "$$signed_byte_interpreter_rc" -eq "$$signed_byte_native_rc"; \
		cmp "$$run_dir/signed-byte.native.out" "$$run_dir/signed-byte.translated.out"; \
		cmp "$$run_dir/signed-byte.native.out" "$$run_dir/signed-byte.interpreter.out"; \
		cmp "$$run_dir/signed-byte.native.err" "$$run_dir/signed-byte.translated.err"; \
		sed '/^arm64-metal trace /d' "$$run_dir/signed-byte.interpreter.err" >"$$run_dir/signed-byte.interpreter.no-trace.err"; \
		cmp "$$run_dir/signed-byte.native.err" "$$run_dir/signed-byte.interpreter.no-trace.err"; \
		grep -q 'interpreter-block-ready.*detail=5' "$$run_dir/signed-byte.interpreter.err"; \
		awk '/event=interpreter-dispatch/ && !first { first=1; firstpc=$$5 } \
		     $$4 == "event=interpreter-cache-fault" { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
		     /event=interpreter-cache-resident/ { resident=1; if (!fault || NR <= faultline || $$5 != faultpc || $$6 != faultaddr) bad=1; residentline=NR } \
		     /event=interpreter-dispatch/ && resident && !resumed && NR > residentline { resumed=1; if ($$5 != faultpc) bad=1 } \
		     END { if (!first || !fault || !resident || !resumed || firstpc == faultpc || bad) exit 1 }' "$$run_dir/signed-byte.interpreter.err"; \
		set +e; \
		"$(INTERPRETER_DEMAND_CACHE_SIGNED_HALF_NATIVE)" >"$$run_dir/signed-half.native.out" 2>"$$run_dir/signed-half.native.err"; signed_half_native_rc=$$?; \
		ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_SIGNED_HALF_MAIN)" >"$$run_dir/signed-half.translated.out" 2>"$$run_dir/signed-half.translated.err"; signed_half_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_CACHE_DEMAND=1 ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_SIGNED_HALF_MAIN)" >"$$run_dir/signed-half.interpreter.out" 2>"$$run_dir/signed-half.interpreter.err"; signed_half_interpreter_rc=$$?; \
		printf '%s\n' "$$signed_half_native_rc" >"$$run_dir/signed-half.native.rc"; \
		printf '%s\n' "$$signed_half_translated_rc" >"$$run_dir/signed-half.translated.rc"; \
		printf '%s\n' "$$signed_half_interpreter_rc" >"$$run_dir/signed-half.interpreter.rc"; \
		set -e; \
		test "$$signed_half_native_rc" -eq 32; \
		test "$$signed_half_translated_rc" -eq "$$signed_half_native_rc"; \
		test "$$signed_half_interpreter_rc" -eq "$$signed_half_native_rc"; \
		cmp "$$run_dir/signed-half.native.out" "$$run_dir/signed-half.translated.out"; \
		cmp "$$run_dir/signed-half.native.out" "$$run_dir/signed-half.interpreter.out"; \
		cmp "$$run_dir/signed-half.native.err" "$$run_dir/signed-half.translated.err"; \
		sed '/^arm64-metal trace /d' "$$run_dir/signed-half.interpreter.err" >"$$run_dir/signed-half.interpreter.no-trace.err"; \
		cmp "$$run_dir/signed-half.native.err" "$$run_dir/signed-half.interpreter.no-trace.err"; \
		grep -q 'interpreter-block-ready.*detail=5' "$$run_dir/signed-half.interpreter.err"; \
		awk '/event=interpreter-dispatch/ && !first { first=1; firstpc=$$5 } \
		     $$4 == "event=interpreter-cache-fault" { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
		     /event=interpreter-cache-resident/ { resident=1; if (!fault || NR <= faultline || $$5 != faultpc || $$6 != faultaddr) bad=1; residentline=NR } \
		     /event=interpreter-dispatch/ && resident && !resumed && NR > residentline { resumed=1; if ($$5 != faultpc) bad=1 } \
		     END { if (!first || !fault || !resident || !resumed || firstpc == faultpc || bad) exit 1 }' "$$run_dir/signed-half.interpreter.err"; \
		set +e; \
		"$(INTERPRETER_DEMAND_CACHE_SIGNED_WORD_NATIVE)" >"$$run_dir/signed-word.native.out" 2>"$$run_dir/signed-word.native.err"; signed_word_native_rc=$$?; \
		ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_CACHE_DEMAND=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_SIGNED_WORD_MAIN)" >"$$run_dir/signed-word.translated.out" 2>"$$run_dir/signed-word.translated.err"; signed_word_translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_CACHE_DEMAND=1 ARM64_METAL_SHARED_CACHE=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_DEMAND_CACHE_SIGNED_WORD_MAIN)" >"$$run_dir/signed-word.interpreter.out" 2>"$$run_dir/signed-word.interpreter.err"; signed_word_interpreter_rc=$$?; \
		printf '%s\n' "$$signed_word_native_rc" >"$$run_dir/signed-word.native.rc"; \
		printf '%s\n' "$$signed_word_translated_rc" >"$$run_dir/signed-word.translated.rc"; \
		printf '%s\n' "$$signed_word_interpreter_rc" >"$$run_dir/signed-word.interpreter.rc"; \
		set -e; \
		test "$$signed_word_native_rc" -eq 32; \
		test "$$signed_word_translated_rc" -eq "$$signed_word_native_rc"; \
		test "$$signed_word_interpreter_rc" -eq "$$signed_word_native_rc"; \
		cmp "$$run_dir/signed-word.native.out" "$$run_dir/signed-word.translated.out"; \
		cmp "$$run_dir/signed-word.native.out" "$$run_dir/signed-word.interpreter.out"; \
		cmp "$$run_dir/signed-word.native.err" "$$run_dir/signed-word.translated.err"; \
		sed '/^arm64-metal trace /d' "$$run_dir/signed-word.interpreter.err" >"$$run_dir/signed-word.interpreter.no-trace.err"; \
		cmp "$$run_dir/signed-word.native.err" "$$run_dir/signed-word.interpreter.no-trace.err"; \
		grep -q 'interpreter-block-ready.*detail=5' "$$run_dir/signed-word.interpreter.err"; \
		awk '/event=interpreter-dispatch/ && !first { first=1; firstpc=$$5 } \
		     $$4 == "event=interpreter-cache-fault" { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
		     /event=interpreter-cache-resident/ { resident=1; if (!fault || NR <= faultline || $$5 != faultpc || $$6 != faultaddr) bad=1; residentline=NR } \
		     /event=interpreter-dispatch/ && resident && !resumed && NR > residentline { resumed=1; if ($$5 != faultpc) bad=1 } \
		     END { if (!first || !fault || !resident || !resumed || firstpc == faultpc || bad) exit 1 }' "$$run_dir/signed-word.interpreter.err"; \
		echo "verify-interpreter-demand-cache receipts: $$run_dir"
$(MAP_JIT_POLICY_TEST): \
	../fixtures/handoff/map-jit-policy-test.cpp guest_code_policy.cpp guest_code_policy.hpp \
	guest_state.hpp macho.cpp macho.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I. ../fixtures/handoff/map-jit-policy-test.cpp \
		guest_code_policy.cpp macho.cpp -framework Security -framework CoreFoundation -o "$@"

$(MAP_JIT_POLICY_FIXTURE): ../fixtures/handoff/map-jit-policy-main.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

.PHONY: verify-map-jit-policy
verify-map-jit-policy: $(MAP_JIT_POLICY_TEST) $(MAP_JIT_POLICY_FIXTURE)
	../fixtures/handoff/run-map-jit-policy-test.sh "$(abspath $(MAP_JIT_POLICY_TEST))" \
		"$(abspath $(MAP_JIT_POLICY_FIXTURE))" "$(abspath ../fixtures/handoff)" \
		"$(abspath $(BUILD_DIR))"

$(JIT_CODE_CACHE_RESERVE_NATIVE): ../fixtures/handoff/jit-code-cache-reserve.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror -pthread "$<" -o "$@"

$(JIT_CODE_CACHE_INVALID_NATIVE): ../fixtures/handoff/jit-code-cache-invalid.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

.PHONY: verify-jit-code-cache-native
verify-jit-code-cache-native: $(JIT_CODE_CACHE_RESERVE_NATIVE) $(JIT_CODE_CACHE_INVALID_NATIVE)
	"$(JIT_CODE_CACHE_RESERVE_NATIVE)"
	"$(JIT_CODE_CACHE_INVALID_NATIVE)"

.PHONY: prepare-map-jit-guest-fixtures
prepare-map-jit-guest-fixtures:
	../fixtures/handoff/prepare-map-jit-guest-fixtures.sh \
		"$(abspath ../fixtures/handoff)" "$(abspath $(BUILD_DIR))"
