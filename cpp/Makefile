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

verify-interpreter: $(APP) $(INTERPRETER_NATIVE)
	"$(APP)" interpreter-probe >"$(BUILD_DIR)/interpreter.gpu.out"
	"$(INTERPRETER_NATIVE)" >"$(BUILD_DIR)/interpreter.native.out"
	cmp "$(BUILD_DIR)/interpreter.native.out" "$(BUILD_DIR)/interpreter.gpu.out"
