BUILD := .build
SWIFT_RUNTIME := $(BUILD)/arm64-metal
GUEST := $(BUILD)/workload.dylib
RUNTIME := $(SWIFT_RUNTIME)
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
SIGNAL_BRIDGE := $(BUILD)/SignalBridge.o
SIGNAL := $(BUILD)/guest-signal
VECTOR := $(BUILD)/vector-memory
HEAP := $(BUILD)/guest-heap
THREADS := $(BUILD)/guest-threads
MEMORY := $(BUILD)/guest-memory
CACHE_CHECK := $(BUILD)/cache-rebase-check
CACHE_ZLIB := $(BUILD)/cache-zlib

.PHONY: all verify verify-cache verify-apps bundle
all: $(RUNTIME) $(GUEST) $(CONSOLE) $(STREAMS) $(INDIRECT) $(REBASED) $(UNSIGNED_CALL) $(DATA_AUTH) $(LINKED) $(REGION) $(PIPE) $(SIGNAL) $(VECTOR) $(HEAP) $(THREADS) $(MEMORY)

verify: all
	sh verify-launch.sh $(RUNTIME) $(CONSOLE) $(STREAMS) $(INDIRECT) $(REBASED) $(LINKED) $(REGION) $(PIPE) $(BUILD) $(SIGNAL) $(VECTOR) $(HEAP) $(THREADS) $(MEMORY)
	@! $(RUNTIME) launch $(UNSIGNED_CALL) >"$(BUILD)/auth.out" 2>"$(BUILD)/auth.log"
	@test ! -s "$(BUILD)/auth.out"
	@grep 'Guest branch authentication failed' "$(BUILD)/auth.log"
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

APP_VERIFY_RUNTIME ?= $(BUILD)/cpp/arm64-metal
APP_VERIFY_RESULTS ?= $(BUILD)/app-verification
APP_VERIFY_CASES ?= plutil tar libz-root libz-provider

verify-apps:
	@test -x "$(APP_VERIFY_RUNTIME)" || { echo "Build the C++ runtime first with make -C cpp app" >&2; exit 2; }
	./verify-apps.sh "$(APP_VERIFY_RUNTIME)" "$(APP_VERIFY_RESULTS)" $(APP_VERIFY_CASES)

verify-cache: $(RUNTIME) $(CACHE_CHECK) $(CACHE_ZLIB)
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
.PHONY: verify-synchronous-fault-oracle
$(SYNCHRONOUS_FAULT_ORACLE): fixtures/handoff/synchronous-fault-oracle.c | $(BUILD)
	xcrun clang -arch arm64 -std=c11 -Wall -Wextra -Werror -pedantic "$<" -o "$@"

verify-synchronous-fault-oracle: $(SYNCHRONOUS_FAULT_ORACLE)
	$(SYNCHRONOUS_FAULT_ORACLE)

$(CACHE_ZLIB): cache-zlib.c | $(BUILD)
	xcrun clang -arch arm64 -Wall -Wextra -Werror -O2 "$<" -lz -o "$@"

$(CACHE_CHECK): cache-rebase-check.c | $(BUILD)
	xcrun clang -arch arm64 -Wall -Wextra -Werror "$<" -o "$@"

bundle: verify
	@mkdir -p "$(BUILD)/arm64-metal-macos"
	@cp "$(RUNTIME)" "$(GUEST)" "$(CONSOLE)" "$(STREAMS)" "$(INDIRECT)" "$(REBASED)" "$(LINKED)" "$(WRAPPER)" "$(SUPPORT)" "$(SIGNAL)" "$(THREADS)" example.json README.md LEDGER.md "$(BUILD)/arm64-metal-macos/"
	@cd "$(BUILD)" && tar -czf arm64-metal-macos.tar.gz arm64-metal-macos

# Package a previously verified C++ launcher without changing the Swift bundle.
CPP_RUNTIME ?= $(BUILD)/cpp/arm64-metal
CPP_INTERPRETER_METALLIB ?= $(BUILD)/cpp/interpreter/runtime-interpreter.metallib
CPP_PACKAGE := $(BUILD)/arm64-metal-cpp-macos

.PHONY: cpp-release bundle-cpp
cpp-release:
	$(MAKE) -C cpp app interpreter-metallib

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

$(SWIFT_RUNTIME): Translate.swift SharedCache.swift ImageLoader.swift Fixups.swift Exports.swift GuestFormat.swift GuestHeap.swift GuestSignals.swift GuestThreads.swift GuestStrings.swift GuestVectorCrypto.swift ProcessTranslation.swift Launch.swift main.swift $(SIGNAL_BRIDGE) SignalBridge.h | $(BUILD)
	xcrun swiftc -O -warnings-as-errors -import-objc-header SignalBridge.h $(filter-out SignalBridge.h,$^) -o "$@"

$(SIGNAL_BRIDGE): SignalBridge.c SignalBridge.h | $(BUILD)
	xcrun clang -arch arm64 -std=c11 -fblocks -D_DARWIN_C_SOURCE -O2 -Wall -Wextra -Werror -c "$<" -o "$@"

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
	xcrun clang -arch arm64 "$<" -o "$@"

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
