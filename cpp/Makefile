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

.PHONY: all components app verify-interpreter clean
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
INTERPRETER_MINIMAL_MAIN := $(BUILD_DIR)/interpreter-minimal-main
INTERPRETER_MOVN_MOVK_MAIN := $(BUILD_DIR)/interpreter-movn-movk-main
INTERPRETER_BRANCH_MAIN := $(BUILD_DIR)/interpreter-branch-main
INTERPRETER_BRANCH_FALLBACK_MAIN := $(BUILD_DIR)/interpreter-branch-fallback-main
INTERPRETER_TYPED_MEMORY_MAIN := $(BUILD_DIR)/interpreter-typed-memory-main
INTERPRETER_TYPED_MEMORY_RO_MAIN := $(BUILD_DIR)/interpreter-typed-memory-ro-main
INTERPRETER_TYPED_MEMORY_UNMAPPED_MAIN := $(BUILD_DIR)/interpreter-typed-memory-unmapped-main
INTERPRETER_GENERATOR := $(BUILD_DIR)/generate-interpreter-msl
INTERPRETER_SOURCE := $(BUILD_DIR)/interpreter/gpu_interpreter.generated.metal
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

$(INTERPRETER_MINIMAL_MAIN): ../fixtures/interpreter/minimal-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

$(INTERPRETER_MOVN_MOVK_MAIN): ../fixtures/interpreter/movn-movk-entry.s
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

.PHONY: interpreter-metallib
interpreter-metallib: $(INTERPRETER_METALLIB)

$(INTERPRETER_GENERATOR): generate_interpreter_msl.cpp guest_memory_msl.hpp
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) "$<" -o "$@"

$(INTERPRETER_SOURCE): gpu_interpreter.metal $(INTERPRETER_GENERATOR)
	@mkdir -p "$(dir $@)"
	"$(INTERPRETER_GENERATOR)" "$<" "$@"

$(INTERPRETER_AIR): $(INTERPRETER_SOURCE) move_wide_shared.h
	@mkdir -p "$(dir $@)"
	xcrun -sdk macosx metal -I . -c "$<" -o "$@"

$(INTERPRETER_METALLIB): $(INTERPRETER_AIR)
	xcrun -sdk macosx metal -o "$@" "$<"

verify-interpreter: $(APP) $(INTERPRETER_TYPED_NATIVE) $(INTERPRETER_MOVE_WIDE_NATIVE) $(INTERPRETER_MINIMAL_MAIN) $(INTERPRETER_MOVN_MOVK_MAIN) $(INTERPRETER_BRANCH_MAIN) $(INTERPRETER_BRANCH_FALLBACK_MAIN) $(INTERPRETER_TYPED_MEMORY_MAIN) $(INTERPRETER_TYPED_MEMORY_RO_MAIN) $(INTERPRETER_TYPED_MEMORY_UNMAPPED_MAIN) $(INTERPRETER_METALLIB)
	@set -eu; \
		mkdir -p "$(BUILD_DIR)"; \
		run_dir=$$(mktemp -d "$(BUILD_DIR)/interpreter-verify.XXXXXX"); \
		"$(INTERPRETER_TYPED_NATIVE)"; \
		ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" "$(APP)" interpreter-probe "$(abspath $(INTERPRETER_METALLIB))" >"$$run_dir/interpreter.gpu.out" 2>"$$run_dir/interpreter.gpu.err"; \
		"$(INTERPRETER_MOVE_WIDE_NATIVE)" >"$$run_dir/interpreter.native.out"; \
		cmp "$$run_dir/interpreter.native.out" "$$run_dir/interpreter.gpu.out"; \
		grep -qx 'pipeline_compiles=1 runtime_source_compiles=0 completed_dispatches=13' "$$run_dir/interpreter.gpu.err"; \
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
		grep -q 'interpreter-dispatch.*detail=2' "$$run_dir/interpreter-process.interpreter.err"; \
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
		grep -q 'interpreter-dispatch.*detail=3' "$$run_dir/movn-movk.interpreter.err"; \
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
		grep -q 'interpreter-dispatch.*detail=3' "$$run_dir/branch.interpreter.err"; \
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
		awk '/event=interpreter-dispatch/ { seen=1; next } seen && /event=compile-start/ { found=1 } END { exit !found }' "$$run_dir/branch-fallback.interpreter.err"; \
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
		grep -q 'interpreter-dispatch.*detail=6' "$$run_dir/typed-memory.interpreter.err"; \
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
		grep -q 'Guest process memory access fault at 0x0' "$$run_dir/typed-memory-unmapped.interpreter.err"; \
		echo "verify-interpreter receipts: $$run_dir"
