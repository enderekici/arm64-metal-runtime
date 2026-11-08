CXX ?= clang++
CPPFLAGS ?= -I.
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -pedantic
BUILD_DIR ?= ../.build/cpp
.DEFAULT_GOAL := all

# This target checks stable C++17 components independently of Metal framework
# linking and the still-integrating whole-process host/runner.
COMPONENT_SOURCES := \
	cache_residency.cpp \
	darwin_bridge.cpp \
	function_runner.cpp \
	guest_code_policy.cpp \
	guest_format.cpp \
	guest_signal_runtime.cpp \
	guest_scheduler.cpp \
	guest_state.cpp \
	guest_strings.cpp \
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

.PHONY: all components app verify-interpreter verify-interpreter-demand-cache verify-guest-format verify-guest-image-registry verify-memcpy-chk verify-semaphore-native verify-semaphore-scheduler verify-open-nocancel-native verify-proc-info-native verify-af-unix-socket-native verify-vector-crypto verify-mach-vm-overwrite-native verify-adc-sbc-carry-native verify-adc-sbc-carry-interpreter verify-mach-vm-deferred-reclamation-native verify-guest-heap-allocate-at clean
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
	cache_residency.cpp \
	guest_signal_runtime.cpp \
	guest_scheduler.cpp \
	guest_code_policy.cpp \
	guest_format.cpp \
	guest_state.cpp \
	guest_strings.cpp \
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
	shared_cache.cpp \
	translate_core.cpp \
	function_runner.cpp \
	main.cpp
APP_HEADERS := $(wildcard *.h *.hpp)
APP := $(BUILD_DIR)/arm64-metal
OPEN_NOCANCEL_NATIVE := $(BUILD_DIR)/open-nocancel
PROC_INFO_NATIVE := $(BUILD_DIR)/proc-info-self
AF_UNIX_SOCKET_NATIVE := $(BUILD_DIR)/af-unix-socket
SEMAPHORE_NATIVE := $(BUILD_DIR)/mach-semaphore-lifecycle
SEMAPHORE_SCHEDULER := $(BUILD_DIR)/guest-semaphore-scheduler
INTERPRETER_NATIVE := $(BUILD_DIR)/interpreter-native
INTERPRETER_TYPED_NATIVE := $(BUILD_DIR)/interpreter-typed-operations
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
INTERPRETER_LOGICAL_IMMEDIATE_MAIN := $(BUILD_DIR)/interpreter-logical-immediate-main
INTERPRETER_LOGICAL_IMMEDIATE_INVALID_MAIN := $(BUILD_DIR)/interpreter-logical-immediate-invalid-main
INTERPRETER_MINIMAL_MAIN := $(BUILD_DIR)/interpreter-minimal-main
INTERPRETER_MOVN_MOVK_MAIN := $(BUILD_DIR)/interpreter-movn-movk-main
INTERPRETER_BRANCH_MAIN := $(BUILD_DIR)/interpreter-branch-main
INTERPRETER_BRANCH_FALLBACK_MAIN := $(BUILD_DIR)/interpreter-branch-fallback-main
INTERPRETER_BL_MAIN := $(BUILD_DIR)/interpreter-bl-nested-main
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
INTERPRETER_GENERATOR := $(BUILD_DIR)/generate-interpreter-msl
INTERPRETER_SOURCE := $(BUILD_DIR)/interpreter/gpu_interpreter.generated.metal
INTERPRETER_CBZ_CBNZ_MAIN := $(BUILD_DIR)/interpreter-cbz-cbnz-main
INTERPRETER_TBZ_TBNZ_MAIN := $(BUILD_DIR)/interpreter-tbz-tbnz-main
INTERPRETER_ADR_ADRP_MAIN := $(BUILD_DIR)/interpreter-adr-adrp-main
INTERPRETER_ADC_SBC_CARRY_MAIN := $(BUILD_DIR)/interpreter-adc-sbc-carry-main
ADC_SBC_CARRY_DECODE_TEST := $(BUILD_DIR)/adc-sbc-carry-decode-test
GUEST_FORMAT_NATIVE := $(BUILD_DIR)/guest-printf-formats-native
GUEST_FORMAT_TEST := $(BUILD_DIR)/guest-format-test
MAP_JIT_POLICY_TEST := $(BUILD_DIR)/map-jit-policy-test
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
MACH_VM_OVERWRITE_NATIVE := $(BUILD_DIR)/mach-vm-map-overwrite-native
MACH_VM_DEFERRED_RECLAMATION_NATIVE := $(BUILD_DIR)/mach-vm-deferred-reclamation-native
GUEST_HEAP_ALLOCATE_AT_TEST := $(BUILD_DIR)/guest-heap-allocate-at-test
SHA256_NATIVE := $(BUILD_DIR)/sha256-instructions-native
VECTOR_CRYPTO_LOWER_TEST := $(BUILD_DIR)/vector-crypto-lowering-test
VECTOR_CRYPTO_MSL := $(BUILD_DIR)/vector-crypto.metal
VECTOR_CRYPTO_AIR := $(BUILD_DIR)/vector-crypto.air
INTERPRETER_AIR := $(BUILD_DIR)/interpreter/gpu_interpreter.air
INTERPRETER_METALLIB := $(BUILD_DIR)/interpreter/runtime-interpreter.metallib

app: $(APP)

$(SEMAPHORE_NATIVE): ../fixtures/handoff/mach-semaphore-lifecycle.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror -pthread "$<" -o "$@"

verify-semaphore-native: $(SEMAPHORE_NATIVE)
	"$(SEMAPHORE_NATIVE)"

$(SEMAPHORE_SCHEDULER): ../fixtures/handoff/guest-semaphore-scheduler.cpp \
		guest_scheduler.cpp guest_state.cpp guest_signal_runtime.cpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I. $^ -o "$@"

verify-semaphore-scheduler: $(SEMAPHORE_SCHEDULER)
	"$(SEMAPHORE_SCHEDULER)"

$(OPEN_NOCANCEL_NATIVE): ../fixtures/handoff/open-nocancel.c \
		../fixtures/handoff/open-nocancel-input.txt
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

verify-open-nocancel-native: $(OPEN_NOCANCEL_NATIVE)
	"$(OPEN_NOCANCEL_NATIVE)" "$(abspath ../fixtures/handoff/open-nocancel-input.txt)"

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

$(APP): $(APP_SOURCES) $(APP_HEADERS)
	@test -f "$(METAL_CPP)/Foundation/Foundation.hpp" || { echo 'Set METAL_CPP to the installed metal-cpp include root' >&2; exit 1; }
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) -isystem "$(METAL_CPP)" $(CXXFLAGS) -fblocks $(APP_SOURCES) \
		-framework Foundation -framework Metal -framework Security -o "$@"

clean:
	rm -rf "$(BUILD_DIR)"

$(INTERPRETER_NATIVE): ../fixtures/interpreter/movz-ret.s ../fixtures/interpreter/native-main.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror $^ -o "$@"

$(INTERPRETER_TYPED_NATIVE): ../fixtures/handoff/typed-operation.cpp process_decoder.cpp process_decoder.hpp guest_strings.cpp guest_vector_crypto.cpp translate_core.cpp translate_core.hpp integer_ops_shared.h
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I. ../fixtures/handoff/typed-operation.cpp process_decoder.cpp guest_strings.cpp guest_vector_crypto.cpp translate_core.cpp -o "$@"

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

$(MACH_VM_OVERWRITE_NATIVE): ../fixtures/handoff/mach-vm-map-overwrite.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

$(MACH_VM_DEFERRED_RECLAMATION_NATIVE): ../fixtures/handoff/mach-vm-deferred-reclamation.c
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"
$(GUEST_HEAP_ALLOCATE_AT_TEST): ../fixtures/handoff/guest-heap-allocate-at.cpp guest_state.cpp guest_state.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG "$<" guest_state.cpp -o "$@"

verify-mach-vm-overwrite-native: $(MACH_VM_OVERWRITE_NATIVE)
	"$(MACH_VM_OVERWRITE_NATIVE)" >"$(BUILD_DIR)/mach-vm-map-overwrite.native.out"
	printf '%s\n' 'mach_vm_map overwrite full=ok partial=ok hole=ok null=1 bad_flags=4' >"$(BUILD_DIR)/mach-vm-map-overwrite.expected.out"
	cmp "$(BUILD_DIR)/mach-vm-map-overwrite.expected.out" "$(BUILD_DIR)/mach-vm-map-overwrite.native.out"

verify-mach-vm-deferred-reclamation-native: $(MACH_VM_DEFERRED_RECLAMATION_NATIVE)
	"$(MACH_VM_DEFERRED_RECLAMATION_NATIVE)" >"$(BUILD_DIR)/mach-vm-deferred-reclamation.native.out"
	printf '%s\n' 'mach_vm_deferred_reclamation valid=0x20004006 invalid=4 oversized=3 outputs=unchanged' >"$(BUILD_DIR)/mach-vm-deferred-reclamation.expected.out"
	cmp "$(BUILD_DIR)/mach-vm-deferred-reclamation.expected.out" "$(BUILD_DIR)/mach-vm-deferred-reclamation.native.out"

verify-guest-heap-allocate-at: $(GUEST_HEAP_ALLOCATE_AT_TEST)
	"$(GUEST_HEAP_ALLOCATE_AT_TEST)" >"$(BUILD_DIR)/guest-heap-allocate-at.out"
	printf '%s\n' 'guest heap allocate_at exact=ok collision=reject release=ok alignment=ok' >"$(BUILD_DIR)/guest-heap-allocate-at.expected.out"
	cmp "$(BUILD_DIR)/guest-heap-allocate-at.expected.out" "$(BUILD_DIR)/guest-heap-allocate-at.out"

verify-memcpy-chk: $(MEMCPY_CHK_NATIVE) $(MEMCPY_CHK_SIGINFO_NATIVE) $(MEMCPY_CHK_DECODE_TEST)
	nm -u "$(MEMCPY_CHK_NATIVE)" | grep -q '___memcpy_chk'
	"$(MEMCPY_CHK_NATIVE)" >"$(BUILD_DIR)/memcpy-chk.native.out"
	printf '%s\n' 'copy=fort result=1' >"$(BUILD_DIR)/memcpy-chk.expected.out"
	cmp "$(BUILD_DIR)/memcpy-chk.expected.out" "$(BUILD_DIR)/memcpy-chk.native.out"
	@ulimit -c 0; set +e; "$(MEMCPY_CHK_NATIVE)" overflow >"$(BUILD_DIR)/memcpy-chk.overflow.out" 2>"$(BUILD_DIR)/memcpy-chk.overflow.err"; result=$$?; set -e; \
		 test "$$result" -eq 133 && test ! -s "$(BUILD_DIR)/memcpy-chk.overflow.out"
	python3 -c 'import struct, subprocess, sys; result = subprocess.run([sys.argv[1]], stdout=subprocess.PIPE, stderr=subprocess.PIPE); assert result.returncode == 77, result.returncode; assert result.stderr == b"", result.stderr; assert len(result.stdout) == 24, len(result.stdout); signum, code, pc, address = struct.unpack("<iiQQ", result.stdout); assert (signum, code) == (5, 0), (signum, code); assert pc != 0 and address != 0, (hex(pc), hex(address)); print("native SA_SIGINFO SIGTRAP: signal=5 code=0 controlled_exit=77 pc/address captured")' "$(MEMCPY_CHK_SIGINFO_NATIVE)"
	"$(MEMCPY_CHK_DECODE_TEST)"

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

verify-interpreter: $(APP) $(INTERPRETER_TYPED_NATIVE) $(INTERPRETER_MOVE_WIDE_NATIVE) $(INTERPRETER_ADD_SUB_NATIVE) $(INTERPRETER_ADD_SUB_FLAGS_NATIVE) $(INTERPRETER_MINIMAL_MAIN) $(INTERPRETER_MOVN_MOVK_MAIN) $(INTERPRETER_ADD_SUB_MAIN) $(INTERPRETER_ADD_SUB_SP_MAIN) $(INTERPRETER_BCOND_MAIN) $(INTERPRETER_CSEL_MAIN) $(INTERPRETER_ADD_SUB_SHIFTED_MAIN) $(INTERPRETER_ADD_SUB_EXTENDED_MAIN) $(INTERPRETER_LOGICAL_SHIFTED_MAIN) $(INTERPRETER_LOGICAL_VARIABLE_SHIFTS_MAIN) $(INTERPRETER_MULTIPLY_ACCUMULATE_MAIN) $(INTERPRETER_LOGICAL_IMMEDIATE_MAIN) $(INTERPRETER_LOGICAL_IMMEDIATE_INVALID_MAIN) $(INTERPRETER_BRANCH_MAIN) $(INTERPRETER_BRANCH_FALLBACK_MAIN) $(INTERPRETER_BL_MAIN) $(INTERPRETER_BL_NATIVE) $(INTERPRETER_BL_UNPLANNED_MAIN) $(INTERPRETER_BL_UNPLANNED_NATIVE) $(INTERPRETER_BRANCH_REGISTER_MAIN) $(INTERPRETER_BRANCH_REGISTER_NATIVE) $(INTERPRETER_BRANCH_REGISTER_IMPORT_MAIN) $(INTERPRETER_BRANCH_REGISTER_IMPORT_NATIVE) $(INTERPRETER_TYPED_MEMORY_MAIN) $(INTERPRETER_TYPED_MEMORY_RO_MAIN) $(INTERPRETER_TYPED_MEMORY_UNMAPPED_MAIN) $(INTERPRETER_PAIR_MEMORY_MAIN) $(INTERPRETER_SCALAR_INDEXED_MEMORY_MAIN) $(INTERPRETER_CBZ_CBNZ_MAIN) $(INTERPRETER_TBZ_TBNZ_MAIN) $(INTERPRETER_ADR_ADRP_MAIN) $(INTERPRETER_METALLIB)
	@set -eu; \
		mkdir -p "$(BUILD_DIR)"; \
		run_dir=$$(mktemp -d "$(BUILD_DIR)/interpreter-verify.XXXXXX"); \
		"$(INTERPRETER_TYPED_NATIVE)"; \
		ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" "$(APP)" interpreter-probe "$(abspath $(INTERPRETER_METALLIB))" >"$$run_dir/interpreter.gpu.out" 2>"$$run_dir/interpreter.gpu.err"; \
		"$(INTERPRETER_MOVE_WIDE_NATIVE)" >"$$run_dir/interpreter.native.out"; \
		"$(INTERPRETER_ADD_SUB_NATIVE)" >>"$$run_dir/interpreter.native.out"; \
		"$(INTERPRETER_ADD_SUB_FLAGS_NATIVE)" >>"$$run_dir/interpreter.native.out"; \
		cmp "$$run_dir/interpreter.native.out" "$$run_dir/interpreter.gpu.out"; \
		grep -qx 'pipeline_compiles=1 runtime_source_compiles=0 completed_dispatches=39' "$$run_dir/interpreter.gpu.err"; \
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
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_TYPED_MEMORY_RO_MAIN)" >"$$run_dir/typed-memory-ro.interpreter.out" 2>"$$run_dir/typed-memory-ro.interpreter.err"; memory_ro_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_TYPED_MEMORY_UNMAPPED_MAIN)" >"$$run_dir/typed-memory-unmapped.interpreter.out" 2>"$$run_dir/typed-memory-unmapped.interpreter.err"; memory_unmapped_rc=$$?; \
		set -e; \
		test "$$memory_ro_rc" -ne 0 && test "$$memory_unmapped_rc" -ne 0 && \
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
		grep -q 'dispatch-done.*pc=0x42d8 detail=2' "$$run_dir/add-sub-sp.interpreter.err" && \
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
		echo "verify-interpreter receipts: $$run_dir"

verify-interpreter-demand-cache: $(APP) $(INTERPRETER_DEMAND_CACHE_MAIN) $(INTERPRETER_DEMAND_CACHE_NATIVE) $(INTERPRETER_DEMAND_CACHE_SCALAR_MAIN) $(INTERPRETER_DEMAND_CACHE_SCALAR_NATIVE) $(INTERPRETER_DEMAND_CACHE_INDEXED_MAIN) $(INTERPRETER_DEMAND_CACHE_INDEXED_NATIVE) $(INTERPRETER_DEMAND_CACHE_BYTE_MAIN) $(INTERPRETER_DEMAND_CACHE_BYTE_NATIVE) $(INTERPRETER_DEMAND_CACHE_HALF_MAIN) $(INTERPRETER_DEMAND_CACHE_HALF_NATIVE) $(INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_MAIN) $(INTERPRETER_DEMAND_CACHE_SIGNED_BYTE_NATIVE) $(INTERPRETER_DEMAND_CACHE_SIGNED_HALF_MAIN) $(INTERPRETER_DEMAND_CACHE_SIGNED_HALF_NATIVE) $(INTERPRETER_DEMAND_CACHE_SIGNED_WORD_MAIN) $(INTERPRETER_DEMAND_CACHE_SIGNED_WORD_NATIVE) $(INTERPRETER_METALLIB)
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
		     /event=interpreter-cache-fault/ { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
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
		     /event=interpreter-cache-fault/ { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
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
		     /event=interpreter-cache-fault/ { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
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
		     /event=interpreter-cache-fault/ { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
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
		     /event=interpreter-cache-fault/ { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
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
		     /event=interpreter-cache-fault/ { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
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
		     /event=interpreter-cache-fault/ { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
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
		     /event=interpreter-cache-fault/ { fault=1; faultpc=$$5; faultaddr=$$6; faultline=NR; if (faultaddr !~ /^detail=/) bad=1 } \
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

.PHONY: prepare-map-jit-guest-fixtures
prepare-map-jit-guest-fixtures:
	../fixtures/handoff/prepare-map-jit-guest-fixtures.sh \
		"$(abspath ../fixtures/handoff)" "$(abspath $(BUILD_DIR))"
