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
APP_HEADERS := $(wildcard *.hpp)
APP := $(BUILD_DIR)/arm64-metal
INTERPRETER_NATIVE := $(BUILD_DIR)/interpreter-native
INTERPRETER_MINIMAL_MAIN := $(BUILD_DIR)/interpreter-minimal-main
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

$(INTERPRETER_MINIMAL_MAIN): ../fixtures/interpreter/minimal-main.s
	@mkdir -p "$(BUILD_DIR)"
	xcrun clang -arch arm64 -nostdlib -Wl,-e,_main "$<" -lSystem -o "$@"

.PHONY: interpreter-metallib
interpreter-metallib: $(INTERPRETER_METALLIB)

$(INTERPRETER_AIR): gpu_interpreter.metal
	@mkdir -p "$(dir $@)"
	xcrun -sdk macosx metal -c "$<" -o "$@"

$(INTERPRETER_METALLIB): $(INTERPRETER_AIR)
	xcrun -sdk macosx metal -o "$@" "$<"

verify-interpreter: $(APP) $(INTERPRETER_NATIVE) $(INTERPRETER_MINIMAL_MAIN) $(INTERPRETER_METALLIB)
	ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" "$(APP)" interpreter-probe "$(abspath $(INTERPRETER_METALLIB))" >"$(BUILD_DIR)/interpreter.gpu.out" 2>"$(BUILD_DIR)/interpreter.gpu.err"
	"$(INTERPRETER_NATIVE)" >"$(BUILD_DIR)/interpreter.native.out"
	cmp "$(BUILD_DIR)/interpreter.native.out" "$(BUILD_DIR)/interpreter.gpu.out"
	grep -qx 'pipeline_compiles=1 runtime_source_compiles=0 completed_dispatches=4' "$(BUILD_DIR)/interpreter.gpu.err"
	@set +e; \
		"$(INTERPRETER_MINIMAL_MAIN)" >"$(BUILD_DIR)/interpreter-process.native.out" 2>"$(BUILD_DIR)/interpreter-process.native.err"; native_rc=$$?; \
		"$(APP)" launch "$(INTERPRETER_MINIMAL_MAIN)" >"$(BUILD_DIR)/interpreter-process.translated.out" 2>"$(BUILD_DIR)/interpreter-process.translated.err"; translated_rc=$$?; \
		ARM64_METAL_INTERPRETER=1 ARM64_METAL_INTERPRETER_LIBRARY="$(abspath $(INTERPRETER_METALLIB))" ARM64_METAL_TRACE=1 "$(APP)" launch "$(INTERPRETER_MINIMAL_MAIN)" >"$(BUILD_DIR)/interpreter-process.interpreter.out" 2>"$(BUILD_DIR)/interpreter-process.interpreter.err"; interpreter_rc=$$?; \
		printf '%s\n' "$$native_rc" >"$(BUILD_DIR)/interpreter-process.native.rc"; \
		printf '%s\n' "$$translated_rc" >"$(BUILD_DIR)/interpreter-process.translated.rc"; \
		printf '%s\n' "$$interpreter_rc" >"$(BUILD_DIR)/interpreter-process.interpreter.rc"; \
		set -e; \
		test "$$native_rc" -eq 42 && test "$$translated_rc" -eq "$$native_rc" && \
		test "$$interpreter_rc" -eq "$$native_rc" && \
		cmp "$(BUILD_DIR)/interpreter-process.native.out" "$(BUILD_DIR)/interpreter-process.translated.out" && \
		cmp "$(BUILD_DIR)/interpreter-process.native.err" "$(BUILD_DIR)/interpreter-process.translated.err" && \
		grep -q 'interpreter-pipeline-ready.*detail=1' "$(BUILD_DIR)/interpreter-process.interpreter.err" && \
		grep -q 'interpreter-block-ready.*detail=2' "$(BUILD_DIR)/interpreter-process.interpreter.err" && \
		grep -q 'interpreter-dispatch.*detail=2' "$(BUILD_DIR)/interpreter-process.interpreter.err"
