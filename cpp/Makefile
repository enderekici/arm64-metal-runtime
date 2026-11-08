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

.PHONY: all components app verify-interpreter verify-interpreter-demand-cache verify-guest-format verify-memcpy-chk clean
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
INTERPRETER_NATIVE := $(BUILD_DIR)/interpreter-native
INTERPRETER_TYPED_NATIVE := $(BUILD_DIR)/interpreter-typed-operations
INTERPRETER_MOVE_WIDE_NATIVE := $(BUILD_DIR)/interpreter-move-wide-native
INTERPRETER_ADD_SUB_NATIVE := $(BUILD_DIR)/interpreter-add-sub-native
INTERPRETER_ADD_SUB_MAIN := $(BUILD_DIR)/interpreter-add-sub-immediate-main
INTERPRETER_ADD_SUB_FLAGS_NATIVE := $(BUILD_DIR)/interpreter-add-sub-flags-native
INTERPRETER_ADD_SUB_SP_MAIN := $(BUILD_DIR)/interpreter-add-sub-sp-flags-main
INTERPRETER_BCOND_MAIN := $(BUILD_DIR)/interpreter-bcond-nzcv-main
INTERPRETER_CSEL_MAIN := $(BUILD_DIR)/interpreter-csel-family-main
INTERPRETER_MINIMAL_MAIN := $(BUILD_DIR)/interpreter-minimal-main
INTERPRETER_MOVN_MOVK_MAIN := $(BUILD_DIR)/interpreter-movn-movk-main
INTERPRETER_BRANCH_MAIN := $(BUILD_DIR)/interpreter-branch-main
INTERPRETER_BRANCH_FALLBACK_MAIN := $(BUILD_DIR)/interpreter-branch-fallback-main
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
INTERPRETER_GENERATOR := $(BUILD_DIR)/generate-interpreter-msl
INTERPRETER_SOURCE := $(BUILD_DIR)/interpreter/gpu_interpreter.generated.metal
INTERPRETER_CBZ_CBNZ_MAIN := $(BUILD_DIR)/interpreter-cbz-cbnz-main
GUEST_FORMAT_NATIVE := $(BUILD_DIR)/guest-printf-formats-native
GUEST_FORMAT_TEST := $(BUILD_DIR)/guest-format-test
MAP_JIT_POLICY_TEST := $(BUILD_DIR)/map-jit-policy-test
MAP_JIT_POLICY_FIXTURE := $(BUILD_DIR)/map-jit-policy-fixture
MEMCPY_CHK_NATIVE := $(BUILD_DIR)/memcpy-chk-native
MEMCPY_CHK_SIGINFO_NATIVE := $(BUILD_DIR)/memcpy-chk-siginfo-native
MEMCPY_CHK_DECODE_TEST := $(BUILD_DIR)/memcpy-chk-decode-test
INTERPRETER_AIR := $(BUILD_DIR)/interpreter/gpu_interpreter.air
INTERPRETER_METALLIB := $(BUILD_DIR)/interpreter/runtime-interpreter.metallib

app: $(APP)

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

$(INTERPRETER_TYPED_NATIVE): ../fixtures/handoff/typed-operation.cpp translate_core.cpp translate_core.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -I. ../fixtures/handoff/typed-operation.cpp translate_core.cpp -o "$@"

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

$(INTERPRETER_BRANCH_MAIN): ../fixtures/interpreter/branch-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_BRANCH_FALLBACK_MAIN): ../fixtures/interpreter/branch-fallback-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

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

verify-memcpy-chk: $(MEMCPY_CHK_NATIVE) $(MEMCPY_CHK_SIGINFO_NATIVE) $(MEMCPY_CHK_DECODE_TEST)
	nm -u "$(MEMCPY_CHK_NATIVE)" | grep -q '___memcpy_chk'
	"$(MEMCPY_CHK_NATIVE)" >"$(BUILD_DIR)/memcpy-chk.native.out"
	printf '%s\n' 'copy=fort result=1' >"$(BUILD_DIR)/memcpy-chk.expected.out"
	cmp "$(BUILD_DIR)/memcpy-chk.expected.out" "$(BUILD_DIR)/memcpy-chk.native.out"
	@ulimit -c 0; set +e; "$(MEMCPY_CHK_NATIVE)" overflow >"$(BUILD_DIR)/memcpy-chk.overflow.out" 2>"$(BUILD_DIR)/memcpy-chk.overflow.err"; result=$$?; set -e; \
		 test "$$result" -eq 133 && test ! -s "$(BUILD_DIR)/memcpy-chk.overflow.out"
	python3 -c 'import struct, subprocess, sys; result = subprocess.run([sys.argv[1]], stdout=subprocess.PIPE, stderr=subprocess.PIPE); assert result.returncode == 77, result.returncode; assert result.stderr == b"", result.stderr; assert len(result.stdout) == 24, len(result.stdout); signum, code, pc, address = struct.unpack("<iiQQ", result.stdout); assert (signum, code) == (5, 0), (signum, code); assert pc != 0 and address != 0, (hex(pc), hex(address)); print("native SA_SIGINFO SIGTRAP: signal=5 code=0 controlled_exit=77 pc/address captured")' "$(MEMCPY_CHK_SIGINFO_NATIVE)"
	"$(MEMCPY_CHK_DECODE_TEST)"

$(INTERPRETER_DEMAND_CACHE_INDEXED_SOURCE): $(INTERPRETER_DEMAND_CACHE_GENERATOR)
	@mkdir -p "$(dir $@)"
	"$(INTERPRETER_DEMAND_CACHE_GENERATOR)" indexed >"$@"

$(INTERPRETER_DEMAND_CACHE_INDEXED_MAIN): $(INTERPRETER_DEMAND_CACHE_INDEXED_SOURCE)
	xcrun clang -arch arm64 "$<" -lz -o "$@"

$(INTERPRETER_DEMAND_CACHE_INDEXED_NATIVE): ../fixtures/interpreter/demand-cache-retry-native.c
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -DPAIR_LOAD=0 "$<" -lz -o "$@"

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

verify-interpreter: $(APP) $(INTERPRETER_TYPED_NATIVE) $(INTERPRETER_MOVE_WIDE_NATIVE) $(INTERPRETER_ADD_SUB_NATIVE) $(INTERPRETER_ADD_SUB_FLAGS_NATIVE) $(INTERPRETER_MINIMAL_MAIN) $(INTERPRETER_MOVN_MOVK_MAIN) $(INTERPRETER_ADD_SUB_MAIN) $(INTERPRETER_ADD_SUB_SP_MAIN) $(INTERPRETER_BCOND_MAIN) $(INTERPRETER_CSEL_MAIN) $(INTERPRETER_BRANCH_MAIN) $(INTERPRETER_BRANCH_FALLBACK_MAIN) $(INTERPRETER_TYPED_MEMORY_MAIN) $(INTERPRETER_TYPED_MEMORY_RO_MAIN) $(INTERPRETER_TYPED_MEMORY_UNMAPPED_MAIN) $(INTERPRETER_PAIR_MEMORY_MAIN) $(INTERPRETER_SCALAR_INDEXED_MEMORY_MAIN) $(INTERPRETER_CBZ_CBNZ_MAIN) $(INTERPRETER_METALLIB)
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
		awk '/event=interpreter-block-ready/ { ready=1; if ($$5 != "pc=0x42d8") bad=1 } \
		     /event=interpreter-dispatch/ { if (!ready || dispatched || $$5 != "pc=0x42d8") bad=1; dispatched=1 } \
		     /event=dispatch-done/ && dispatched && !completed { completed=1; endpc=$$5; if ($$5 != "pc=0x42ec") bad=1 } \
		     /event=compile-start/ && completed && !compiled { compiled=1; if ($$5 != endpc) bad=1 } \
		     END { if (!ready || !dispatched || !completed || !compiled || bad) exit 1 }' "$$run_dir/add-sub-sp.interpreter.err" || exit 1; \
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
		echo "verify-interpreter receipts: $$run_dir"

verify-interpreter-demand-cache: $(APP) $(INTERPRETER_DEMAND_CACHE_MAIN) $(INTERPRETER_DEMAND_CACHE_NATIVE) $(INTERPRETER_DEMAND_CACHE_SCALAR_MAIN) $(INTERPRETER_DEMAND_CACHE_SCALAR_NATIVE) $(INTERPRETER_DEMAND_CACHE_INDEXED_MAIN) $(INTERPRETER_DEMAND_CACHE_INDEXED_NATIVE) $(INTERPRETER_METALLIB)
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
