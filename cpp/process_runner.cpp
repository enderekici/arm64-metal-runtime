#include "guest_dlopen.hpp"
#include "host_service_registry.hpp"
#include "guest_exit.hpp"
#include "process_runner.hpp"

#include "image_loader.hpp"
#include "cache_residency.hpp"
#include "darwin_bridge.hpp"
#include "guest_signal_runtime.hpp"
#include "guest_scheduler.hpp"
#include "guest_tlv.hpp"
#include "gpu_interpreter.hpp"
#include "metal_backend.hpp"
#include "process_decoder.hpp"
#include "process_host.hpp"
#include "process_msl.hpp"
#include "translate_core.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <pthread.h>
#include <runetype.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <tuple>
#include <thread>
#include <tuple>
#include <time.h>
#include <unordered_map>
#include <utility>
#include <unistd.h>

extern char** environ;

namespace arm64metal::process_runner {
namespace {

constexpr std::uint64_t kNoFault = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t kInstructionsPerDecode = 128;
constexpr std::size_t kMaximumDecodedInstructions = 131072;
constexpr std::uint64_t kMaximumDispatchBlocks = 8192;

const std::unordered_map<std::string, int>& host_service_ids() {
    return runner_host_service_ids();
}

const std::set<std::string>& guest_memory_imports() {
    static const std::set<std::string> imports{
        "_memcpy", "_memmove", "_memset", "_bzero", "_memset_pattern4",
        "_memset_pattern8", "_memset_pattern16", "_strsep", "_memchr", "_strchr"};
    return imports;
}

std::vector<std::string> capture_environment() {
    std::vector<std::string> values;
    if (environ == nullptr) return values;
    for (char** item = environ; *item != nullptr; ++item) values.emplace_back(*item);
    return values;
}

std::vector<GuestMemoryRange> readonly_ranges(const ExecutableImage& image) {
    std::vector<GuestMemoryRange> ranges;
    ranges.reserve(image.immutable_regions.size());
    for (const auto& range : image.immutable_regions) {
        if (range.first < 0 || range.second < range.first) {
            throw std::runtime_error("Executable image has an invalid immutable range");
        }
        ranges.push_back({static_cast<std::uint64_t>(range.first),
                          static_cast<std::uint64_t>(range.second)});
    }
    return ranges;
}

void decode_reachable(const ExecutableImage& image,
                      const std::map<std::int64_t, std::string>& imports,
                      const std::map<std::int64_t, std::int64_t>& guestStubs,
                      std::map<std::int64_t, Instruction>& instructions,
                      std::map<std::int64_t, std::string>& decodeErrors,
                      std::int64_t entry) {
    std::deque<std::int64_t> pending{entry};
    std::size_t decoded = 0;
    while (!pending.empty() && decoded < kInstructionsPerDecode) {
        const auto pc = pending.front();
        pending.pop_front();
        if (instructions.find(pc) != instructions.end()) continue;
        const auto import = imports.find(pc);
        const auto stub = guestStubs.find(pc);
        const auto selectorTarget = image.cache_reader
            ? image.cache_reader->stubTarget(pc) : std::optional<std::int64_t>{};
        const auto selectorWord = image.cache_reader && !image.is_code(pc) && !selectorTarget
            ? image.cache_reader->selectorStubInstruction(pc) : std::optional<std::uint32_t>{};
        if (!image.is_code(pc) && import == imports.end() && stub == guestStubs.end() &&
            !selectorTarget && !selectorWord) continue;
        if (instructions.size() >= kMaximumDecodedInstructions) {
            throw std::runtime_error("Process decode cache limit exceeded");
        }
        Instruction instruction;
        try {
            if (selectorTarget) {
                instruction = {{"pc = " + std::to_string(*selectorTarget) + "ul; break;"},
                               {*selectorTarget}};
            } else if (import != imports.end() && import->second == "_malloc_size" &&
                       pc >= SharedCache::guestBase && image.is_code(pc)) {
                instruction = decode_process(image.instruction(pc), pc, imports, guestStubs, true);
                auto host = hostCall(import->second, std::nullopt, true);
                constexpr std::uint64_t mappingBase = std::uint64_t{1} << 45U;
                const auto lower = mappingBase + image.cache_backing_end();
                const auto upper = mappingBase + image.mapping_backing_end();
                host.code.insert(host.code.begin(),
                    "if (x[0] < " + std::to_string(lower) + "ul || x[0] >= " +
                    std::to_string(upper) + "ul) {");
                host.code.push_back("}");
                instruction.code.insert(instruction.code.begin(), host.code.begin(), host.code.end());
            } else if (import != imports.end()) {
                instruction = hostCall(import->second, std::nullopt, true);
            } else if (stub != guestStubs.end()) {
                instruction = {{"pc = " + std::to_string(stub->second) + "ul; break;"},
                               {stub->second}};
            } else {
                instruction = decode_process(selectorWord ? *selectorWord : image.instruction(pc),
                                             pc, imports,
                                             guestStubs, true);
            }
        } catch (const std::exception& error) {
            const auto word = selectorWord ? *selectorWord : image.instruction(pc);
            if ((word & 0xffe0001fU) == 0) {
                instruction = {{"x[33] = " + std::to_string(pc) +
                                "ul; x[34] = 14; return;"}, {}};
            } else {
                decodeErrors[pc] = error.what();
                instruction = {{"x[33] = " + std::to_string(pc) +
                                "ul; x[34] = 5; return;"}, {}};
            }
        }
        if (!image.deferred_cache.empty()) {
            const auto checkpoint = "x[33] = " + std::to_string(pc) +
                "ul; x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | "
                "(uint(c) << 3); x[34] = 2;";
            for (auto& statement : instruction.code) {
                std::size_t offset = 0;
                while ((offset = statement.find("x[34] = 2;", offset)) != std::string::npos) {
                    statement.replace(offset, sizeof("x[34] = 2;") - 1, checkpoint);
                    offset += checkpoint.size();
                }
            }
        }
        const auto successors = instruction.next;
        instructions.emplace(pc, std::move(instruction));
        ++decoded;
        for (const auto successor : successors) {
            const bool selectorStub = image.cache_reader &&
                (image.cache_reader->stubTarget(successor).has_value() ||
                 (!image.is_code(successor) &&
                  image.cache_reader->selectorStubInstruction(successor).has_value()));
            if ((image.is_code(successor) || selectorStub) &&
                instructions.find(successor) == instructions.end()) {
                pending.push_back(successor);
            }
        }
    }
}

std::vector<std::uint64_t> resident_cache_table(const ExecutableImage& image,
                                               std::size_t capacity) {
    std::vector<std::uint64_t> resident(1 + 4 * capacity, 0);
    if (capacity < image.cache_regions.size()) {
        throw std::runtime_error("Guest resident map capacity is smaller than initial cache map");
    }
    resident[0] = image.cache_regions.size();
    for (std::size_t index = 0; index < image.cache_regions.size(); ++index) {
        const auto& region = image.cache_regions[index];
        const auto entry = 1 + 4 * index;
        if (region.logical_start < 0 || region.logical_end < region.logical_start) {
            throw std::runtime_error("Executable image has an invalid shared-cache region");
        }
        resident[entry] = static_cast<std::uint64_t>(region.logical_start);
        resident[entry + 1] = static_cast<std::uint64_t>(region.logical_end);
        resident[entry + 2] = region.backing;
        resident[entry + 3] = (region.writable ? 1U : 0U) | (region.readable ? 0U : 2U);
    }
    return resident;
}

std::vector<GuestMemoryMapping> guest_memory_mappings(const ExecutableImage& image) {
    std::vector<GuestMemoryMapping> mappings;
    mappings.reserve(image.cache_regions.size());
    for (const auto& region : image.cache_regions) {
        if (region.logical_start < 0 || region.logical_end < region.logical_start) {
            throw std::runtime_error("Executable image has an invalid shared-cache mapping");
        }
        mappings.push_back({static_cast<std::uint64_t>(region.logical_start),
                            static_cast<std::uint64_t>(region.logical_end),
                            static_cast<std::uint64_t>(region.backing), region.writable,
                            region.readable});
    }
    return mappings;
}

struct ProcessKernelPlan {
    std::map<std::int64_t, Instruction> instructions;
    std::vector<gpu_interpreter::InstructionPacket> interpretedPackets;
    bool interpreterEligible = false;
};

struct CompiledProcessKernel {
    ComputeKernel translated;
    std::vector<gpu_interpreter::InstructionPacket> interpretedPackets;
    bool usesInterpreter = false;
};

ProcessKernelPlan plan_process_kernel(
    const ExecutableImage& image,
    const std::map<std::int64_t, std::string>& imports,
    const std::map<std::int64_t, std::int64_t>& guestStubs,
    std::map<std::int64_t, std::string>& allDecodeErrors,
    std::int64_t entry,
    bool allowInterpreter) {
    ProcessKernelPlan plan;
    std::map<std::int64_t, std::string> decodeErrors;
    decode_reachable(image, imports, guestStubs, plan.instructions, decodeErrors, entry);
    allDecodeErrors.insert(decodeErrors.begin(), decodeErrors.end());

    if (!allowInterpreter) return plan;

    const auto leaders = translate_core::block_entries(plan.instructions, entry);
    std::map<std::int64_t, gpu_interpreter::InstructionPacket> eligiblePackets;
    for (const auto leader : leaders) {
        struct PlannedInstruction {
            std::int64_t pc;
            const Instruction* instruction;
            bool blockEnd;
        };
        std::vector<PlannedInstruction> block;
        auto pc = leader;
        bool complete = false;
        while (block.size() < 8192) {
            const auto found = plan.instructions.find(pc);
            if (found == plan.instructions.end() || !found->second.operation) break;
            const auto& instruction = found->second;
            const auto opcode = instruction.operation->opcode;
            if (opcode == translate_core::Opcode::Return) {
                if (!instruction.next.empty()) break;
                block.push_back({pc, &instruction, true});
                complete = true;
                break;
            }
            if (opcode == translate_core::Opcode::Branch) {
                if (!instruction.ends_block_at(pc) || instruction.next.size() != 1 ||
                    instruction.next.front() < 0) {
                    break;
                }
                block.push_back({pc, &instruction, true});
                complete = true;
                break;
            }
            if (opcode == translate_core::Opcode::BranchZero) {
                if (!instruction.ends_block_at(pc) || instruction.next.size() != 2 ||
                    instruction.next[0] < 0 || instruction.next[1] < 0) {
                    break;
                }
                block.push_back({pc, &instruction, true});
                complete = true;
                break;
            }
            const bool moveWideOpcode =
                opcode == translate_core::Opcode::MoveWideZero ||
                opcode == translate_core::Opcode::MoveWideNot ||
                opcode == translate_core::Opcode::MoveWideKeep;
            const bool memoryOpcode =
                opcode == translate_core::Opcode::LoadUnsignedImmediate ||
                opcode == translate_core::Opcode::StoreUnsignedImmediate;
            const bool addSubImmediateOpcode =
                opcode == translate_core::Opcode::AddImmediate ||
                opcode == translate_core::Opcode::SubImmediate ||
                opcode == translate_core::Opcode::AddImmediateSetFlags ||
                opcode == translate_core::Opcode::SubImmediateSetFlags;
            const auto& operation = *instruction.operation;
            const bool validMemoryOperation =
                !memoryOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 0xfff &&
                 (operation.width == 32 || operation.width == 64) &&
                 operation.shift == (operation.width == 32 ? 2U : 3U));
            const bool validAddSubImmediate =
                !addSubImmediateOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 0xfff &&
                 (operation.width == 32 || operation.width == 64) &&
                 (operation.shift == 0 || operation.shift == 12));
            if ((!moveWideOpcode && !memoryOpcode && !addSubImmediateOpcode) ||
                !validMemoryOperation || !validAddSubImmediate ||
                instruction.next.size() != 1 || pc > std::numeric_limits<std::int64_t>::max() - 4 ||
                instruction.next.front() != pc + 4) {
                break;
            }
            const bool leaderBoundary = leaders.find(pc + 4) != leaders.end();
            block.push_back({pc, &instruction, leaderBoundary});
            if (leaderBoundary) {
                complete = true;
                break;
            }
            pc += 4;
        }
        if (!complete || block.empty() || block.size() + eligiblePackets.size() > 8192) {
            if (leader == entry) return plan;
            continue;
        }
        for (std::size_t index = 0; index < block.size(); ++index) {
            const auto& planned = block[index];
            gpu_interpreter::InstructionPacket packet;
            packet.operation = translate_core::serialize_operation(
                *planned.instruction->operation);
            packet.pc = static_cast<std::uint64_t>(planned.pc);
            if (index == 0) packet.flags |= gpu_interpreter::packetBlockStart;
            if (planned.blockEnd) {
                packet.flags |= gpu_interpreter::packetBlockEnd;
                const auto& successors = planned.instruction->next;
                if (successors.size() > 2) return plan;
                packet.successorCount = static_cast<std::uint32_t>(successors.size());
                if (!successors.empty()) {
                    if (successors.front() < 0) return plan;
                    packet.successor0 = static_cast<std::uint64_t>(successors.front());
                }
                if (successors.size() > 1) {
                    if (successors[1] < 0) return plan;
                    packet.successor1 = static_cast<std::uint64_t>(successors[1]);
                }
            }
            eligiblePackets.emplace(planned.pc, packet);
        }
    }
    if (eligiblePackets.find(entry) == eligiblePackets.end()) return plan;
    plan.interpretedPackets.reserve(eligiblePackets.size());
    for (const auto& [pc, packet] : eligiblePackets) {
        (void)pc;
        plan.interpretedPackets.push_back(packet);
    }
    plan.interpreterEligible = true;
    return plan;
}

bool cache_faulted(const std::uint64_t* registers) {
    return registers[guestCacheFaultAddress] != kNoFault;
}

std::optional<std::uint64_t> memory_backing_offset(const ExecutableImage& image,
                                                   std::uint64_t address,
                                                   std::uint64_t width,
                                                   std::size_t memorySize) {
    if (address <= memorySize && width <= memorySize - address) return address;
    for (const auto& region : image.cache_regions) {
        if (region.logical_start < 0 || region.logical_end < region.logical_start) continue;
        const auto begin = static_cast<std::uint64_t>(region.logical_start);
        const auto end = static_cast<std::uint64_t>(region.logical_end);
        if (address < begin || address > end || width > end - address) continue;
        const auto offset = static_cast<std::uint64_t>(region.backing) + address - begin;
        if (offset <= memorySize && width <= memorySize - offset) return offset;
    }
    return std::nullopt;
}

void store_u64(std::vector<std::uint8_t>& memory, std::uint64_t address,
               std::uint64_t value) {
    if (address > memory.size() || memory.size() - static_cast<std::size_t>(address) < 8) {
        throw std::runtime_error("Executable binding exceeds guest backing memory");
    }
    for (unsigned byte = 0; byte < 8; ++byte) {
        memory[static_cast<std::size_t>(address) + byte] =
            static_cast<std::uint8_t>(value >> (byte * 8U));
    }
}

std::uint64_t load_u64(const std::vector<std::uint8_t>& memory, std::uint64_t address) {
    if (address > memory.size() || memory.size() - static_cast<std::size_t>(address) < 8) {
        throw std::runtime_error("Authenticated pointer exceeds guest backing memory");
    }
    std::uint64_t value = 0;
    for (unsigned byte = 0; byte < 8; ++byte) {
        value |= static_cast<std::uint64_t>(memory[static_cast<std::size_t>(address) + byte])
                 << (byte * 8U);
    }
    return value;
}

std::uint32_t load_u32(const std::vector<std::uint8_t>& memory, std::uint64_t address) {
    if (address > memory.size() || memory.size() - static_cast<std::size_t>(address) < 4) {
        throw std::runtime_error("Guest TLV descriptor exceeds bootstrap memory");
    }
    std::uint32_t value = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
        value |= static_cast<std::uint32_t>(memory[static_cast<std::size_t>(address) + byte])
                 << (byte * 8U);
    }
    return value;
}

void store_u32(std::vector<std::uint8_t>& memory, std::uint64_t address,
               std::uint32_t value) {
    if (address > memory.size() || memory.size() - static_cast<std::size_t>(address) < 4) {
        throw std::runtime_error("Guest TLV descriptor exceeds bootstrap memory");
    }
    for (unsigned byte = 0; byte < 4; ++byte) {
        memory[static_cast<std::size_t>(address) + byte] =
            static_cast<std::uint8_t>(value >> (byte * 8U));
    }
}

std::uint64_t encode_guest_pointer(std::uint64_t address, std::uint64_t key,
                                   std::size_t index) {
    constexpr std::uint64_t addressMask = (std::uint64_t{1} << 47U) - 1;
    constexpr std::uint64_t tokenTag = 0xa64e000000000000ULL;
    constexpr std::uint64_t dataTokenMarker = std::uint64_t{1} << 55U;
    constexpr std::uint64_t dataTokenBuckets = 255;
    constexpr std::uint64_t tokenIndexShift = 3;
    if (key == 2 && address <= addressMask) {
        const auto code = static_cast<std::uint64_t>(index) % dataTokenBuckets + 1;
        return dataTokenMarker | (code << 47U) | address;
    }
    return tokenTag | (static_cast<std::uint64_t>(index + 1) << tokenIndexShift) |
           (address & 7U);
}

void initialize_bindings(const ExecutableImage& image, ProcessBootstrapResult& bootstrap) {
    for (const auto& [address, symbol] : image.bindings) {
        const auto offset = memory_backing_offset(image, address, 8, bootstrap.memory.size());
        if (!offset) throw std::runtime_error("Invalid executable pointer binding");
        const auto global = bootstrap.modeledGlobalAddresses.find(symbol);
        if (global != bootstrap.modeledGlobalAddresses.end()) {
            store_u64(bootstrap.memory, *offset, global->second);
            continue;
        }
        const auto function = image.function_bindings.find(address);
        if (function != image.function_bindings.end()) {
            store_u64(bootstrap.memory, *offset, static_cast<std::uint64_t>(function->second));
        }
    }
}

void initialize_authenticated_pointers(const ExecutableImage& image,
                                       ProcessBootstrapResult& bootstrap) {
    auto& registers = bootstrap.registers;
    const auto tokenCount = static_cast<std::size_t>(registers[guestTokenCount]);
    const auto tokenCapacity = static_cast<std::size_t>(registers[guestTokenCapacity]);
    if (tokenCount > tokenCapacity || image.authentication.size() > tokenCapacity - tokenCount) {
        throw std::runtime_error("Initial guest authentication table is too small");
    }
    std::vector<std::pair<std::size_t, ChainedAuthentication>> authentication(
        image.authentication.begin(), image.authentication.end());
    std::sort(authentication.begin(), authentication.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    for (const auto& [address, signature] : authentication) {
        const auto offset = memory_backing_offset(image, address, 8, bootstrap.memory.size());
        if (!offset) throw std::runtime_error("Authenticated pointer exceeds guest backing memory");
        const auto pointer = load_u64(bootstrap.memory, *offset);
        if (pointer == 0) continue;
        const auto index = static_cast<std::size_t>(registers[guestTokenCount]);
        if (index >= tokenCapacity) throw std::runtime_error("Initial guest authentication table is full");
        const auto slot = guestTokenStart + 3 * index;
        registers[slot] = pointer;
        registers[slot + 1] = signature.modifier;
        registers[slot + 2] = signature.key;
        ++registers[guestTokenCount];
        store_u64(bootstrap.memory, *offset,
                  encode_guest_pointer(pointer, signature.key, index));
    }
}

void initialize_cached_globals(const ExecutableImage& image,
                               ProcessBootstrapResult& bootstrap) {
    const auto writeCached = [&](std::uint64_t address, std::uint64_t value,
                                 std::uint64_t width) {
        const auto offset = memory_backing_offset(image, address, width, bootstrap.memory.size());
        if (!offset) return;
        if (width == 4) {
            for (unsigned byte = 0; byte < 4; ++byte) {
                bootstrap.memory[static_cast<std::size_t>(*offset) + byte] =
                    static_cast<std::uint8_t>(value >> (byte * 8U));
            }
        } else {
            store_u64(bootstrap.memory, *offset, value);
        }
    };
    if (image.cached_task_port) writeCached(*image.cached_task_port, guestMachTaskPort, 4);
    if (image.cache_reader) {
        if (const auto localeKey = image.cache_reader->localeKeyAddress()) {
            writeCached(static_cast<std::uint64_t>(*localeKey), 10, 8);
        }
    }
    if (image.cached_dispatch_main_queue) {
        const auto stateAddress = static_cast<std::uint64_t>(*image.cached_dispatch_main_queue) + 0x38U;
        const auto offset = memory_backing_offset(image, stateAddress, 8, bootstrap.memory.size());
        if (offset) {
            constexpr std::uint64_t ownerMask = 0xffff'fffcULL;
            const auto prior = load_u64(bootstrap.memory, *offset);
            const auto owner = guestMachThreadName(guestMainThreadID) & ownerMask;
            store_u64(bootstrap.memory, *offset, (prior & ~ownerMask) | owner);
        }
    }
}

struct GuestTlvDescriptorOwner {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t templateSize;
    std::uint32_t key;
};

std::vector<GuestTlvDescriptorOwner> initialize_local_thread_local_images(
    const ExecutableImage& image, ProcessBootstrapResult& bootstrap,
    GuestTlvRuntime& runtime, std::set<std::uint32_t> occupiedKeys) {
    std::vector<std::pair<std::uint64_t, const ThreadLocalImageMetadata*>> ordered;
    ordered.reserve(image.thread_local_images.size());
    for (const auto& [header, metadata] : image.thread_local_images) {
        if (metadata.thunk_ranges.empty()) continue;
        if (metadata.from_shared_cache) {
            continue;
        }
        if (!metadata.init_callbacks.empty() || !metadata.init_callback_ranges.empty()) {
            throw std::runtime_error("Dynamic guest TLS initializers and destructors are unsupported");
        }
        ordered.emplace_back(header, &metadata);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    for (const auto& [header, metadata] : ordered) {
        (void)header;
        for (const auto& [begin, length] : metadata->thunk_ranges) {
            if (length % 24 != 0) throw std::runtime_error("Malformed guest TLV thunk range");
            for (std::uint64_t offset = 0; offset < length; offset += 24) {
                const auto backing = memory_backing_offset(
                    image, begin + offset, 24, bootstrap.memory.size());
                if (!backing) throw std::runtime_error("Guest TLV thunk is outside mapped memory");
                const auto existingKey = load_u32(bootstrap.memory, *backing + 8);
                if (existingKey >= 512 && existingKey < GuestTlvRuntime::guestTsdSlotCount) {
                    occupiedKeys.insert(existingKey);
                }
            }
        }
    }

    std::uint32_t nextKey = GuestTlvRuntime::guestTsdSlotCount;
    std::vector<GuestTlvDescriptorOwner> descriptors;
    for (const auto& [header, metadata] : ordered) {
        (void)header;
        if (metadata->template_size == 0 ||
            metadata->template_size > std::numeric_limits<std::size_t>::max() ||
            metadata->template_alignment == 0 ||
            (metadata->template_alignment & (metadata->template_alignment - 1)) != 0) {
            throw std::runtime_error("Invalid guest TLV template size or alignment");
        }
        if (metadata->template_address >
            std::numeric_limits<std::uint64_t>::max() - metadata->template_size) {
            throw std::runtime_error("Guest TLV template address overflows");
        }

        std::uint32_t key = 0;
        while (nextKey > 512) {
            --nextKey;
            if (occupiedKeys.insert(nextKey).second) {
                key = nextKey;
                break;
            }
        }
        if (key == 0) throw std::runtime_error("Guest TLV key space is exhausted");

        std::vector<std::uint8_t> templateBytes(
            static_cast<std::size_t>(metadata->template_size), 0);
        for (const auto& [rangeAddress, rangeSize] : metadata->template_ranges) {
            if (rangeAddress < metadata->template_address ||
                rangeAddress - metadata->template_address > metadata->template_size ||
                rangeSize > metadata->template_size -
                                (rangeAddress - metadata->template_address)) {
                throw std::runtime_error("Guest TLV template section exceeds its combined span");
            }
            const auto backing = memory_backing_offset(
                image, rangeAddress, rangeSize, bootstrap.memory.size());
            if (!backing) throw std::runtime_error("Guest TLV template section is unmapped");
            std::copy_n(bootstrap.memory.begin() + static_cast<std::ptrdiff_t>(*backing),
                        static_cast<std::size_t>(rangeSize),
                        templateBytes.begin() + static_cast<std::ptrdiff_t>(
                            rangeAddress - metadata->template_address));
        }
        runtime.register_image(key, std::move(templateBytes), 0,
                               metadata->template_alignment);

        for (const auto& [begin, length] : metadata->thunk_ranges) {
            for (std::uint64_t offset = 0; offset < length; offset += 24) {
                const auto guestAddress = begin + offset;
                const auto backing = memory_backing_offset(
                    image, guestAddress, 24, bootstrap.memory.size());
                if (!backing) throw std::runtime_error("Guest TLV thunk is outside mapped memory");
                const auto target = load_u64(bootstrap.memory, *backing + 16);
                const auto descriptorOffset =
                    target >= metadata->template_address &&
                            target - metadata->template_address <= metadata->template_size
                        ? target - metadata->template_address
                        : target;
                if (descriptorOffset >= metadata->template_size ||
                    descriptorOffset > std::numeric_limits<std::uint32_t>::max()) {
                    throw std::runtime_error("Guest TLV descriptor offset is outside its template");
                }
                store_u32(bootstrap.memory, *backing + 8, key);
                store_u32(bootstrap.memory, *backing + 12,
                          static_cast<std::uint32_t>(descriptorOffset));
            }
            if (begin > std::numeric_limits<std::uint64_t>::max() - length) {
                throw std::runtime_error("Guest TLV thunk range overflows");
            }
            descriptors.push_back({begin, begin + length, metadata->template_size, key});
        }
    }
    return descriptors;
}

struct CachedGuestTlvOwner {
    std::uint64_t header;
    const ThreadLocalImageMetadata* metadata;
};

std::set<std::uint32_t> cached_tlv_static_keys(
    const ExecutableImage& image, std::map<std::uint64_t, std::uint32_t>& v1Keys,
    std::map<std::uint32_t, std::uint64_t>& staticOwners) {
    std::set<std::uint32_t> keys;
    if (!image.cache_reader) return keys;
    for (const auto& [header, metadata] : image.thread_local_images) {
        (void)header;
        if (!metadata.from_shared_cache || metadata.thunk_ranges.empty()) continue;
        for (const auto& [begin, length] : metadata.thunk_ranges) {
            if (length % 24 != 0) throw std::runtime_error("Malformed cached TLV thunk range");
            const auto original = image.cache_reader->originalAddress(
                static_cast<std::int64_t>(begin));
            const auto raw = image.cache_reader->bytes(original, length);
            if (raw.size() != length) throw std::runtime_error("Truncated cached TLV thunk range");
            for (std::size_t offset = 0; offset < raw.size(); offset += 24) {
                std::uint32_t key = 0;
                std::memcpy(&key, raw.data() + offset + 8, sizeof(key));
                if (key == 0) continue;
                if (key >= GuestTlvRuntime::guestTsdSlotCount) {
                    throw std::runtime_error("Conflicting cached static TLV key " +
                                             std::to_string(key));
                }
                const auto [owner, inserted] = staticOwners.emplace(key, header);
                if (!inserted && owner->second != header) {
                    throw std::runtime_error("Conflicting cached static TLV key " +
                                             std::to_string(key));
                }
                keys.insert(key);
            }
        }
    }
    std::uint32_t nextKey = 512;
    for (const auto& [header, metadata] : image.thread_local_images) {
        if (!metadata.from_shared_cache || metadata.thunk_ranges.empty()) continue;
        while (nextKey < GuestTlvRuntime::guestTsdSlotCount &&
               keys.find(nextKey) != keys.end()) ++nextKey;
        if (nextKey == GuestTlvRuntime::guestTsdSlotCount) {
            throw std::runtime_error("Cached v1 TLV key space is exhausted");
        }
        if (std::none_of(metadata.thunk_ranges.begin(), metadata.thunk_ranges.end(),
                         [&](const auto& range) {
                             const auto original = image.cache_reader->originalAddress(
                                 static_cast<std::int64_t>(range.first));
                             const auto raw = image.cache_reader->bytes(original, 24);
                             std::uint32_t key = 0;
                             std::memcpy(&key, raw.data() + 8, sizeof(key));
                             return key != 0;
                         })) {
            v1Keys.emplace(header, nextKey);
            keys.insert(nextKey++);
        }
    }
    return keys;
}

void enter_startup_frame(std::vector<std::uint64_t>& registers,
                         const ProcessBootstrapResult& bootstrap,
                         std::uint64_t address) {
    std::fill_n(registers.begin(), 36, 0);
    for (std::size_t index = 0; index < 4; ++index) registers[index] = bootstrap.registers[index];
    registers[30] = kNoFault;
    registers[31] = bootstrap.stack.end;
    registers[33] = address;
}

std::string guest_cstring(const GuestMemory& memory, std::uint64_t address) {
    constexpr std::size_t maximumLength = 4096;
    std::string value;
    for (std::size_t offset = 0; offset < maximumLength; ++offset) {
        if (address > std::numeric_limits<std::uint64_t>::max() - offset) {
            throw std::runtime_error("Guest string address overflow");
        }
        const auto* byte = memory.buffer(address + offset, 1);
        if (*byte == 0) return value;
        value.push_back(static_cast<char>(*byte));
    }
    throw std::runtime_error("Guest string exceeds runner limit");
}

std::string hexadecimal(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << value;
    return output.str();
}

class ProgressTrace {
public:
    ProgressTrace() {
        const char* enabled = std::getenv("ARM64_METAL_TRACE");
        enabled_ = enabled != nullptr && std::strcmp(enabled, "1") == 0;
    }

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    void event(const char* name, std::uint64_t pc, std::uint64_t detail = 0) {
        if (!enabled_) return;
        const auto now = std::chrono::steady_clock::now();
        if (events_ == maximumEvents) {
            if (!limitReported_) {
                std::cerr << "arm64-metal trace event-limit=" << maximumEvents << '\n';
                limitReported_ = true;
                lastPeriodic_ = now;
                return;
            }
            if (periodicEvents_ == maximumPeriodicEvents ||
                now - lastPeriodic_ < periodicInterval) return;
            ++periodicEvents_;
            lastPeriodic_ = now;
            write("progress", name, pc, detail, now);
            return;
        }
        ++events_;
        write("event", name, pc, detail, now);
    }

private:
    static constexpr std::size_t maximumEvents = 128;
    static constexpr std::size_t maximumPeriodicEvents = 16;
    static constexpr auto periodicInterval = std::chrono::seconds(10);
    void write(const char* kind, const char* name, std::uint64_t pc,
               std::uint64_t detail, std::chrono::steady_clock::time_point now) const {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - started_).count();
        std::cerr << "arm64-metal trace +" << elapsed << "ms " << kind << '=' << name
                  << " pc=0x" << hexadecimal(pc) << " detail=" << detail << '\n';
    }

    bool enabled_ = false;
    bool limitReported_ = false;
    std::size_t events_ = 0;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastPeriodic_ = started_;
    std::size_t periodicEvents_ = 0;
};

std::string pac_failure_message(const std::uint64_t* state, std::size_t words,
                                std::uint64_t status) {
    const std::string kind = status == 9 ? "data-pointer" : status == 10 ? "branch" : "code-pointer";
    std::string message = "Guest " + kind + " authentication failed at 0x" + hexadecimal(state[33]);
    if (words <= guestCacheFaultAddress) return message;
    const auto pointer = state[guestFaultAddress];
    const auto modifier = state[guestCacheFaultAddress];
    if (pointer == kNoFault && modifier == kNoFault) return message + " (PAC operands unavailable)";
    message += ", pointer 0x" + hexadecimal(pointer) + ", modifier 0x" + hexadecimal(modifier);
    if (words < guestTokenStart) return message;
    const auto count = state[guestTokenCount];
    const auto capacity = state[guestTokenCapacity];
    if (count > capacity || capacity > (words - guestTokenStart) / 3) {
        return message + " (PAC token metadata unavailable: invalid table bounds)";
    }
    std::size_t matched = 0;
    std::size_t matchedSlot = 0;
    if ((pointer & ~std::uint64_t{0xffffffff}) == 0xa64e000000000000ULL) {
        const auto index = (pointer & 0xffffffffU) >> 3U;
        if (index > 0 && index <= count) {
            const auto slot = guestTokenStart + 3 * static_cast<std::size_t>(index - 1);
            if (pointer == encode_guest_pointer(state[slot], state[slot + 2], index - 1)) {
                matched = 1;
                matchedSlot = slot;
            }
        }
    } else {
        const auto bucket = (pointer >> 47U) & 0xffU;
        const auto upper = pointer >> 55U;
        const auto address = pointer & ((std::uint64_t{1} << 47U) - 1);
        if (bucket != 0 && upper <= 1) {
            for (std::uint64_t index = bucket - 1; index < count; index += 255) {
                const auto slot = guestTokenStart + 3 * static_cast<std::size_t>(index);
                if (state[slot + 2] == 2 && state[slot] == address &&
                    (upper == 0 || pointer == encode_guest_pointer(state[slot], 2, index))) {
                    ++matched;
                    matchedSlot = slot;
                }
            }
        }
    }
    if (matched == 1) {
        message += "; token target 0x" + hexadecimal(state[matchedSlot]) +
                   ", modifier 0x" + hexadecimal(state[matchedSlot + 1]) +
                   ", key " + std::to_string(state[matchedSlot + 2]);
    } else if (matched > 1) {
        message += "; ambiguous data-token candidates " + std::to_string(matched);
    }
    return message;
}

}  // namespace

std::int32_t launch_executable(const std::filesystem::path& executable,
                               const std::vector<std::string>& arguments,
                               const LaunchOptions& options) {
    ProgressTrace progressTrace;
    if (options.dispatchBlocks == 0 || options.dispatchBlocks > kMaximumDispatchBlocks) {
        throw std::invalid_argument("Process dispatch block budget must be in [1, 8192]");
    }
    bool sharedCache = options.sharedCache;
    if (!sharedCache) {
        const char* cacheEnvironment = std::getenv("ARM64_METAL_SHARED_CACHE");
        sharedCache = cacheEnvironment != nullptr && std::strcmp(cacheEnvironment, "1") == 0;
    }

    progressTrace.event("image-load-start", 0);
    ExecutableImage image(executable, sharedCache, options.demandCache,
                          host_service_ids(), guest_memory_imports(),
                          options.cachedInitializers);
    progressTrace.event("image-load-done", static_cast<std::uint64_t>(image.entry));
    std::map<std::int64_t, std::string> imports(image.imports.begin(), image.imports.end());
    if (image.cache_reader &&
        image.cache_reader->newFormatTlvs() == std::optional<bool>{true}) {
        const auto fastEntry = image.cache_reader->tlvGetAddrFastEntry();
        if (!fastEntry) throw std::runtime_error("Validated cached TLV fast entry is unavailable");
        const auto [unused, inserted] = imports.emplace(*fastEntry, "__tlv_get_addr");
        (void)unused;
        if (!inserted) throw std::runtime_error("Cached TLV fast entry collides with an image import");
    }
    std::map<std::int64_t, std::int64_t> guestStubs(image.guest_stubs.begin(),
                                                     image.guest_stubs.end());
    const auto signing = guest_signing_policy(executable.string(), image.main_restricted);
    ProcessBootstrapInput input;
    input.mappedImage = image.memory;
    input.readOnlyRanges = readonly_ranges(image);
    input.entry = image.entry;
    input.mainHeader = static_cast<std::uint64_t>(image.main_header);
    input.mappingBackingEnd = image.mapping_backing_end();
    if (input.mappingBackingEnd > input.mappedImage.size()) {
        input.mappedImage.resize(static_cast<std::size_t>(input.mappingBackingEnd), 0);
    }
    input.executablePath = executable.string();
    input.arguments = arguments;
    input.environment = capture_environment();
    input.mainQueue = image.cached_dispatch_main_queue.value_or(0);
    input.pageSize = static_cast<std::uint64_t>(getpagesize());
    input.kernelPageSize = input.pageSize;
    input.initialAuthenticationEntries = std::max<std::size_t>(
        guestInitialTokenCapacity, image.authentication.size());
    const bool usesDefaultRuneLocale = std::any_of(
        image.bindings.begin(), image.bindings.end(),
        [](const auto& binding) { return binding.second == "__DefaultRuneLocale"; });
    if (usesDefaultRuneLocale) {
        input.defaultRuneLocaleBytes.resize(sizeof(_RuneLocale));
        std::memcpy(input.defaultRuneLocaleBytes.data(), &_DefaultRuneLocale,
                    sizeof(_RuneLocale));
        input.runeLocaleByteCount = sizeof(_RuneLocale);
    }
    input.guestTaskPort = guestMachTaskPort;
    auto bootstrap = build_process_bootstrap(std::move(input));
    GuestThread mainThread;
    mainThread.id = 1;
    mainThread.stack = {static_cast<std::int64_t>(bootstrap.stack.begin),
                        static_cast<std::int64_t>(bootstrap.stack.end)};
    mainThread.errnoAddress = static_cast<std::int64_t>(bootstrap.errnoSlot);
    mainThread.tsdAddress = static_cast<std::int64_t>(bootstrap.mainThreadSpecificData);
    mainThread.registers = bootstrap.registers;
    GuestScheduler guestScheduler(std::move(mainThread));
    initialize_bindings(image, bootstrap);
    initialize_authenticated_pointers(image, bootstrap);
    initialize_cached_globals(image, bootstrap);
    GuestTlvRuntime guestTlvRuntime;
    std::set<std::uint64_t> registeredCachedTlvImages;
    std::map<std::uint64_t, std::uint32_t> cachedV1TlvKeys;
    std::map<std::uint32_t, std::uint64_t> cachedStaticTlvOwners;
    auto occupiedTlvKeys = cached_tlv_static_keys(
        image, cachedV1TlvKeys, cachedStaticTlvOwners);
    const auto guestTlvDescriptors = initialize_local_thread_local_images(
        image, bootstrap, guestTlvRuntime, std::move(occupiedTlvKeys));

    std::vector<std::uint64_t> startup;
    const bool cacheBootstrap = options.cacheBootstrap || options.cachedInitializers ||
        (std::getenv("ARM64_METAL_CACHE_BOOTSTRAP") != nullptr &&
         std::strcmp(std::getenv("ARM64_METAL_CACHE_BOOTSTRAP"), "1") == 0);
    if (cacheBootstrap) {
        if (image.objc_bootstrap) {
            startup.push_back(static_cast<std::uint64_t>(*image.objc_bootstrap));
        }
    }
    if (options.cachedInitializers) {
        if (!image.startup_initializer_metadata_collected) {
            throw std::runtime_error("Cached initializer plan was not collected");
        }
        for (const auto& initializerImage : image.startup_initializer_images) {
            if (!initializerImage.from_shared_cache) {
                startup.insert(startup.end(), initializerImage.local_targets.begin(),
                               initializerImage.local_targets.end());
                continue;
            }
            for (const auto& initializer : initializerImage.cached_targets) {
                if (initializer.authentication) {
                    throw std::runtime_error(
                        "Authenticated cached initializer dispatch is unsupported for " +
                        initializerImage.path);
                }
                if (initializer.high_byte != 0 ||
                    initializer.target_address > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()) ||
                    !image.is_code(static_cast<std::int64_t>(initializer.target_address))) {
                    throw std::runtime_error("Cached initializer target is not directly callable in " +
                                             initializerImage.path);
                }
                startup.push_back(initializer.target_address);
            }
        }
    } else {
        startup.insert(startup.end(), image.initializers.begin(), image.initializers.end());
    }
    startup.push_back(image.entry);
    auto processRegisters = bootstrap.registers;
    enter_startup_frame(processRegisters, bootstrap, startup.front());
    processRegisters[guestThreadPointer] = bootstrap.mainThreadSpecificData;

    auto backend = Backend::createDefault();
    std::unique_ptr<gpu_interpreter::Interpreter> interpreter;
    if (options.interpreter) {
        const auto pipelinesBefore = backend.compiledPipelineCount();
        if (!options.interpreterLibrary.empty()) {
            interpreter = std::make_unique<gpu_interpreter::Interpreter>(
                backend, options.interpreterLibrary.string());
        } else if (!options.interpreterSource.empty()) {
            interpreter = std::make_unique<gpu_interpreter::Interpreter>(
                gpu_interpreter::Interpreter::fromSourceFile(
                    backend, options.interpreterSource.string()));
        } else {
            throw std::runtime_error("GPU interpreter has no selected AOT library or development source");
        }
        progressTrace.event("interpreter-pipeline-ready", startup.front(),
                            backend.compiledPipelineCount() - pipelinesBefore);
    }
    auto memoryBuffer = backend.makeSharedBuffer(bootstrap.memory.size());
    auto stateBuffer = backend.makeSharedBuffer(processRegisters.size() * sizeof(std::uint64_t));
    const auto residentCapacity = image.cache_regions.size() + 4096;
    auto resident = resident_cache_table(image, residentCapacity);
    auto residentBuffer = backend.makeSharedBuffer(resident.size() * sizeof(std::uint64_t));
    auto sizeBuffer = backend.makeSharedBuffer(sizeof(std::uint64_t));
    if (memoryBuffer.data() == nullptr || stateBuffer.data() == nullptr ||
        residentBuffer.data() == nullptr || sizeBuffer.data() == nullptr) {
        throw std::runtime_error("Metal returned a null guest-process buffer");
    }
    std::memcpy(memoryBuffer.data(), bootstrap.memory.data(), bootstrap.memory.size());
    std::memcpy(stateBuffer.data(), processRegisters.data(),
                processRegisters.size() * sizeof(std::uint64_t));
    std::memcpy(residentBuffer.data(), resident.data(), resident.size() * sizeof(std::uint64_t));
    const auto guestSize = static_cast<std::uint64_t>(bootstrap.memory.size());
    std::memcpy(sizeBuffer.data(), &guestSize, sizeof(guestSize));

    GuestMemory guestMemory(static_cast<std::uint8_t*>(memoryBuffer.data()), memoryBuffer.size(),
                            bootstrap.readOnlyRanges, guest_memory_mappings(image));
    std::set<std::uint64_t> objcWritableBackings;
    const auto publishCommpage = [&](std::uint64_t address, std::size_t backing,
                                     std::uint64_t value, std::size_t width) {
        if (resident[0] >= residentCapacity || backing > memoryBuffer.size() ||
            width > memoryBuffer.size() - backing) {
            throw std::runtime_error("Guest commpage backing or resident capacity exceeded");
        }
        auto* memory = static_cast<std::uint8_t*>(memoryBuffer.data());
        for (std::size_t byte = 0; byte < width; ++byte) {
            memory[backing + byte] = static_cast<std::uint8_t>(value >> (byte * 8U));
        }
        const auto row = 1 + 4 * static_cast<std::size_t>(resident[0]);
        resident[row] = address;
        resident[row + 1] = address + width;
        resident[row + 2] = backing;
        resident[row + 3] = 0;
        resident[0] += 1;
        std::memcpy(residentBuffer.data(), resident.data(), resident.size() * sizeof(std::uint64_t));
        guestMemory.add_mapping({address, address + width, backing, false, true});
    };
    const auto quiescentBacking = image.cache_backing_end();
    const std::uint64_t capabilities = (1ULL << 16) | (1ULL << 15) | (1ULL << 7) | (1ULL << 26);
    publishCommpage(0xfffffc180ULL, quiescentBacking, 0, 8);
    publishCommpage(0xfffffc010ULL, quiescentBacking + 16, capabilities, 8);
    publishCommpage(0xfffffc10cULL, quiescentBacking + 32, 0, 1);
    std::uint64_t capabilities64 = 0;
    vm_size_t capabilities64Bytes = 0;
    const auto commpageRead = vm_read_overwrite(
        mach_task_self(), static_cast<vm_address_t>(0xfffffc100ULL),
        sizeof(capabilities64), reinterpret_cast<vm_address_t>(&capabilities64),
        &capabilities64Bytes);
    if (commpageRead != KERN_SUCCESS || capabilities64Bytes != sizeof(capabilities64)) {
        throw std::runtime_error("Unable to read host Darwin 64-bit commpage capabilities");
    }
    publishCommpage(0xfffffc100ULL, quiescentBacking + 48, capabilities64, 8);
    std::uint64_t physicalMemory = 0;
    std::size_t physicalMemoryLength = sizeof(physicalMemory);
    if (sysctlbyname("hw.memsize", &physicalMemory, &physicalMemoryLength, nullptr, 0) == 0 &&
        physicalMemoryLength == sizeof(physicalMemory)) {
        publishCommpage(0xfffffc038ULL, quiescentBacking + 8, physicalMemory, 8);
    }
    timeval bootTime{};
    std::size_t bootTimeLength = sizeof(bootTime);
    if (sysctlbyname("kern.boottime", &bootTime, &bootTimeLength, nullptr, 0) == 0 &&
        bootTimeLength == sizeof(bootTime) && bootTime.tv_sec >= 0 && bootTime.tv_usec >= 0) {
        const auto microseconds = static_cast<std::uint64_t>(bootTime.tv_sec) * 1'000'000ULL +
                                  static_cast<std::uint64_t>(bootTime.tv_usec);
        publishCommpage(0xfffffc0a0ULL, quiescentBacking + 24, microseconds, 8);
    }
    const std::array<std::pair<std::uint64_t, std::size_t>, 2> readOnlyPageShiftFields{{
        {0xfffff4025ULL, quiescentBacking + 56},
        {0xfffff4037ULL, quiescentBacking + 57},
    }};
    for (const auto& [address, backing] : readOnlyPageShiftFields) {
        std::uint8_t value = 0;
        vm_size_t bytesRead = 0;
        const auto readResult = vm_read_overwrite(
            mach_task_self(), static_cast<vm_address_t>(address), sizeof(value),
            reinterpret_cast<vm_address_t>(&value), &bytesRead);
        if (readResult == KERN_SUCCESS && bytesRead == sizeof(value)) {
            publishCommpage(address, backing, value, sizeof(value));
        }
    }
    process_msl::Config config;
    config.linear_image_end = image.linear_image_end;
    config.mapping_backing_end = image.mapping_backing_end();
    config.writable_regions.reserve(image.writable_regions.size());
    for (const auto& range : image.writable_regions) {
        if (range.first < 0 || range.second < range.first) {
            throw std::runtime_error("Executable image has an invalid writable range");
        }
        config.writable_regions.push_back({static_cast<std::uint64_t>(range.first),
                                           static_cast<std::uint64_t>(range.second)});
    }

    std::vector<std::uint64_t> writableTable{
        bootstrap.errnoSlot,
        static_cast<std::uint64_t>(config.writable_regions.size())};
    writableTable.reserve(2 + 2 * config.writable_regions.size());
    for (const auto& range : config.writable_regions) {
        writableTable.push_back(range.begin);
        writableTable.push_back(range.end);
    }
    auto writableBuffer = backend.makeSharedBuffer(
        writableTable.size() * sizeof(std::uint64_t));
    const std::array<std::uint64_t, 4> memoryConfig{
        config.minimum_image_address, config.shared_guest_base,
        config.linear_image_end, config.mapping_backing_end};
    auto memoryConfigBuffer = backend.makeSharedBuffer(sizeof(memoryConfig));
    if (writableBuffer.data() == nullptr || memoryConfigBuffer.data() == nullptr) {
        throw std::runtime_error("Metal returned a null guest memory configuration buffer");
    }
    std::memcpy(writableBuffer.data(), writableTable.data(),
                writableTable.size() * sizeof(std::uint64_t));
    std::memcpy(memoryConfigBuffer.data(), memoryConfig.data(), sizeof(memoryConfig));

    std::map<std::int64_t, std::string> decodeErrors;
    std::map<std::int64_t, CompiledProcessKernel> kernels;
    const auto compileKernel = [&](std::int64_t entry) {
        const auto plan = plan_process_kernel(image, imports, guestStubs,
                                              decodeErrors, entry,
                                              interpreter != nullptr);
        if (plan.interpreterEligible) {
            progressTrace.event("interpreter-block-ready",
                                static_cast<std::uint64_t>(entry),
                                plan.interpretedPackets.size());
            return CompiledProcessKernel{{}, plan.interpretedPackets, true};
        }

        const auto started = std::chrono::steady_clock::now();
        progressTrace.event("compile-start", static_cast<std::uint64_t>(entry));
        const auto cases = translate_core::render_blocks(plan.instructions, entry, "ul");
        const auto source = process_msl::render_source(cases);
        auto translated = backend.compileCompute(source, "run_process");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        progressTrace.event("compile-done", static_cast<std::uint64_t>(entry),
                            static_cast<std::uint64_t>(elapsed));
        return CompiledProcessKernel{std::move(translated), {}, false};
    };
    bool processCodeCacheInvalidated = false;
    const auto* state = static_cast<const std::uint64_t*>(stateBuffer.data());
    if (state[33] > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("Guest entry address exceeds the supported code range");
    }
    const auto initialEntry = static_cast<std::int64_t>(startup.front());
    auto [initialKernel, inserted] = kernels.emplace(
        initialEntry, compileKernel(initialEntry));
    (void)inserted;
    auto* activeKernel = &initialKernel->second;
    std::int64_t activeEntry = initialEntry;
    const auto selectKernel = [&](std::int64_t entry) {
        const bool selectorStub = image.cache_reader &&
            (image.cache_reader->stubTarget(entry).has_value() ||
             (!image.is_code(entry) &&
              image.cache_reader->selectorStubInstruction(entry).has_value()));
        if (!image.is_code(entry) && imports.find(entry) == imports.end() &&
            guestStubs.find(entry) == guestStubs.end() && !selectorStub) {
            throw std::runtime_error("Guest process resumed outside translated text at PC 0x" +
                                     hexadecimal(static_cast<std::uint64_t>(entry)));
        }
        auto found = kernels.find(entry);
        if (found == kernels.end()) {
            auto [compiled, wasInserted] = kernels.emplace(entry, compileKernel(entry));
            (void)wasInserted;
            found = compiled;
        }
        activeKernel = &found->second;
        activeEntry = entry;
    };

    const std::vector<BufferBinding> bindings{
        {&memoryBuffer, 0, 0}, {&stateBuffer, 1, 0}, {&sizeBuffer, 2, 0},
        {&writableBuffer, 3, 0}, {&residentBuffer, 4, 0},
        {&memoryConfigBuffer, 5, 0}};
    const gpu_interpreter::MemoryAccessContext interpreterMemoryAccess{
        &memoryBuffer, &sizeBuffer, &writableBuffer, &residentBuffer,
        &memoryConfigBuffer};
    const DispatchSize one{1, 1, 1};
    HostServiceState hostServiceState;
    if (bootstrap.heap.end > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        throw std::length_error("Guest heap address exceeds the allocator range");
    }
    hostServiceState.heapEnd = bootstrap.heap.end;
    hostServiceState.heap.emplace(GuestRange{
        static_cast<std::int64_t>(bootstrap.heap.begin),
        static_cast<std::int64_t>(bootstrap.heap.end)});
    std::map<std::uint64_t, std::int32_t> mutexAttributes;
    if (image.cache_reader) {
        constexpr auto provider = "/usr/lib/system/libsystem_c.dylib";
        const auto base = image.cache_reader->images().find(provider);
        if (base != image.cache_reader->images().end()) {
            const std::array<std::pair<const char*, std::uint64_t>, 3> standardFiles{{
                {"___stdinp", bootstrap.stdinSlot},
                {"___stdoutp", bootstrap.stdoutSlot},
                {"___stderrp", bootstrap.stderrSlot},
            }};
            for (const auto& [symbolName, slot] : standardFiles) {
                const auto symbol = image.cache_reader->exportSymbol(provider, symbolName);
                if (!symbol) continue;
                const auto location = base->second + symbol->offset;
                const auto chain = image.cache_reader->rebases(location, location + 8);
                if (chain.size() != 1 || chain.front().location != location) continue;
                const auto fileAddress = static_cast<std::uint64_t>(
                    image.cache_reader->guestAddress(chain.front().target)) |
                    (static_cast<std::uint64_t>(chain.front().highByte) << 56U);
                hostServiceState.standardFileAliases.emplace(fileAddress, slot);
            }
        }
    }
    std::unordered_map<std::string, std::uint64_t> imageHeaders;
    for (const auto& [header, path] : image.image_paths) imageHeaders.emplace(path, header);
    GuestImageHandles imageHandles(std::move(imageHeaders), executable);
    std::set<std::uint64_t> activatedLazyHeaders;
    std::set<std::uint64_t> loadedImageHeaders(
        image.startup_eager_headers.begin(), image.startup_eager_headers.end());
    const auto imageIsLoaded = [&loadedImageHeaders](std::uint64_t header) {
        return loadedImageHeaders.find(header) != loadedImageHeaders.end();
    };
    struct GuestDlerrorState {
        std::int64_t allocation = 0;
        std::uint64_t capacity = 0;
        bool pending = false;
    };
    std::unordered_map<std::uint64_t, GuestDlerrorState> dlerrorByThread;
    struct DlsymDependency {
        std::optional<std::uint64_t> header;
        bool reexported = false;
    };
    struct DlsymImage {
        std::string path;
        bool cached = false;
        std::map<std::string, MachExport> exports;
        std::unordered_map<std::string, std::int64_t> cacheExports;
        std::vector<DlsymDependency> dependencies;
    };
    std::unordered_map<std::string, std::uint64_t> imageHeadersByPath;
    const auto normalizeImagePath = [](const std::filesystem::path& path) {
        std::error_code error;
        auto normalized = std::filesystem::weakly_canonical(path, error);
        if (error) normalized = path.lexically_normal();
        return normalized.string();
    };
    for (const auto& [header, path] : image.image_paths) {
        imageHeadersByPath[path] = header;
        imageHeadersByPath[normalizeImagePath(path)] = header;
    }
    const auto executablePath = std::filesystem::path(executable).lexically_normal();
    const auto locateDependency = [&](const std::string& ownerPath,
                                      const std::vector<std::string>& rpaths,
                                      const std::string& dependency) -> std::optional<std::uint64_t> {
        const std::filesystem::path owner(ownerPath);
        const auto ownerDirectory = owner.parent_path();
        const auto executableDirectory = executablePath.parent_path();
        const auto expandPath = [&](const std::string& value) {
            if (value == "@loader_path") return ownerDirectory;
            if (value.rfind("@loader_path/", 0) == 0) {
                return ownerDirectory / value.substr(std::string("@loader_path/").size());
            }
            if (value == "@executable_path") return executableDirectory;
            if (value.rfind("@executable_path/", 0) == 0) {
                return executableDirectory / value.substr(std::string("@executable_path/").size());
            }
            return std::filesystem::path(value);
        };
        std::vector<std::filesystem::path> candidates;
        if (dependency.rfind("@rpath/", 0) == 0) {
            const auto suffix = dependency.substr(std::string("@rpath/").size());
            for (const auto& rpath : rpaths) candidates.push_back(expandPath(rpath) / suffix);
        } else {
            const auto expanded = expandPath(dependency);
            candidates.push_back(expanded.is_absolute() ? expanded : ownerDirectory / expanded);
        }
        for (const auto& candidate : candidates) {
            const auto normalized = normalizeImagePath(candidate);
            const auto found = imageHeadersByPath.find(normalized);
            if (found != imageHeadersByPath.end()) return found->second;
            const auto exact = imageHeadersByPath.find(candidate.lexically_normal().string());
            if (exact != imageHeadersByPath.end()) return exact->second;
        }
        return std::nullopt;
    };
    std::unordered_map<std::uint64_t, DlsymImage> dlsymImages;
    struct GuestDladdrSymbol {
        std::string name;
        std::uint64_t address = 0;
        bool external = false;
    };
    std::map<std::tuple<std::uint64_t, std::int64_t, std::int64_t>,
             std::vector<GuestDladdrSymbol>> dladdrSymbols;
    const auto dlsymImage = [&](std::uint64_t header) -> const DlsymImage& {
        if (const auto found = dlsymImages.find(header); found != dlsymImages.end()) {
            return found->second;
        }
        const auto pathEntry = image.image_paths.find(header);
        if (pathEntry == image.image_paths.end()) {
            throw std::runtime_error("Guest dlsym handle does not name a loaded image");
        }
        DlsymImage metadata;
        metadata.path = pathEntry->second;
        std::vector<std::string> rpaths;
        std::vector<std::pair<std::string, bool>> dependencies;
        if (image.cache_reader) {
            if (auto cachedImage = image.cache_reader->guestImage(metadata.path)) {
                metadata.cached = true;
                metadata.cacheExports = std::move(cachedImage->exports);
                for (const auto& dependency : cachedImage->dependencies) {
                    dependencies.emplace_back(dependency.name, dependency.reexported);
                }
            }
        }
        if (!metadata.cached) {
            const bool mainExecutable =
                header == static_cast<std::uint64_t>(image.main_header);
            MachImage localImage(metadata.path, mainExecutable,
                                 static_cast<std::size_t>(header));
            metadata.exports = std::move(localImage.exports);
            rpaths = std::move(localImage.rpaths);
            for (const auto& dependency : localImage.dependencies) {
                dependencies.emplace_back(dependency.name, dependency.reexported);
            }
        }
        metadata.dependencies.reserve(dependencies.size());
        for (const auto& dependency : dependencies) {
            metadata.dependencies.push_back({
                locateDependency(metadata.path, rpaths, dependency.first), dependency.second});
        }
        return dlsymImages.emplace(header, std::move(metadata)).first->second;
    };
    const auto symbolsForSegment = [&](const MappedImageSegment& segment)
        -> const std::vector<GuestDladdrSymbol>& {
        const auto key = std::make_tuple(segment.guest_header, segment.start, segment.end);
        if (const auto found = dladdrSymbols.find(key);
            found != dladdrSymbols.end()) return found->second;
        std::vector<GuestDladdrSymbol> symbols;
        bool cachedMetadata = false;
        if (image.cache_reader) {
            if (const auto cached = image.cache_reader->guestImage(segment.image_path)) {
                cachedMetadata = true;
                for (const auto& [name, address] : cached->exports) {
                    if (address < 0 ||
                        static_cast<std::uint64_t>(address) <
                            static_cast<std::uint64_t>(segment.start) ||
                        static_cast<std::uint64_t>(address) >=
                            static_cast<std::uint64_t>(segment.end)) continue;
                    const auto visibleName = !name.empty() && name.front() == '_'
                        ? name.substr(1) : name;
                    symbols.push_back({visibleName, static_cast<std::uint64_t>(address), true});
                }
            }
        }
        if (!cachedMetadata) {
            const auto slide = image.image_slides.find(segment.guest_header);
            if (slide != image.image_slides.end()) {
                const auto preferredBase = segment.guest_header - slide->second;
                MachO file(segment.image_path, true);
                for (const auto& symbol : file.defined_section_symbols()) {
                    if (symbol.segment_name != segment.segment_name ||
                        symbol.address < preferredBase) {
                        continue;
                    }
                    const auto relative = symbol.address - preferredBase;
                    if (relative > std::numeric_limits<std::uint64_t>::max() -
                                       segment.guest_header) continue;
                    const auto address = segment.guest_header + relative;
                    if (address < static_cast<std::uint64_t>(segment.start) ||
                        address >= static_cast<std::uint64_t>(segment.end)) continue;
                    const auto visibleName = !symbol.name.empty() && symbol.name.front() == '_'
                        ? symbol.name.substr(1) : symbol.name;
                    symbols.push_back({visibleName, address, symbol.external});
                }
            }
        }
        if (cachedMetadata) {
            std::stable_sort(symbols.begin(), symbols.end(), [](const auto& left, const auto& right) {
                return left.address < right.address;
            });
        }
        return dladdrSymbols.emplace(key, std::move(symbols)).first->second;
    };
    const auto resolveDlsym = [&](std::uint64_t handle, const std::string& name)
        -> std::optional<std::uint64_t> {
        const bool defaultScope = imageHandles.is_rtld_default(handle);
        const bool mainOnly = imageHandles.is_rtld_main_only(handle);
        const auto header = imageHandles.loaded_header(handle);
        if (!defaultScope && !mainOnly && !header) {
            throw std::runtime_error("Guest dlsym supports only open guest image handles");
        }
        const bool firstOnly = imageHandles.first_only(handle);
        const auto lookupName = [&](const std::string& symbol, std::uint64_t startHeader,
                                    bool limitToImage) {
            std::set<std::pair<std::uint64_t, std::string>> visited;
            std::function<std::optional<std::uint64_t>(std::uint64_t, const std::string&, bool)> resolve;
            resolve = [&](std::uint64_t imageHeader, const std::string& current,
                          bool limitToImage) -> std::optional<std::uint64_t> {
                if (!visited.emplace(imageHeader, current).second) return std::nullopt;
                const auto& metadata = dlsymImage(imageHeader);
                if (metadata.cached) {
                    if (image.cache_reader) {
                        if (const auto direct = image.cache_reader->exportSymbol(
                                metadata.path, current)) {
                            const auto original = image.cache_reader->images().find(metadata.path);
                            if (original != image.cache_reader->images().end() &&
                                direct->offset <= std::numeric_limits<std::uint64_t>::max() -
                                                      original->second) {
                                const auto guest = image.cache_reader->guestAddress(
                                    original->second + direct->offset);
                                if (guest >= 0) return static_cast<std::uint64_t>(guest);
                            }
                        }
                    }
                    if (const auto resolved = metadata.cacheExports.find(current);
                        resolved != metadata.cacheExports.end() && resolved->second >= 0) {
                        const auto expected = static_cast<std::uint64_t>(resolved->second);
                        for (const auto& dependency : metadata.dependencies) {
                            if (!dependency.header || (limitToImage && !dependency.reexported)) continue;
                            if (const auto target = resolve(*dependency.header, current,
                                                            limitToImage);
                                target && *target == expected) return target;
                        }
                    }
                } else {
                    const auto exported = metadata.exports.find(current);
                    if (exported != metadata.exports.end()) {
                        if (exported->second.kind == ExportKind::address) {
                            if (exported->second.address <=
                                std::numeric_limits<std::uint64_t>::max() - imageHeader) {
                                return imageHeader + exported->second.address;
                            }
                            return std::nullopt;
                        }
                        const auto ordinal = exported->second.ordinal;
                        if (ordinal != 0 && ordinal <= metadata.dependencies.size()) {
                            const auto& dependency = metadata.dependencies[
                                static_cast<std::size_t>(ordinal - 1)];
                            if (dependency.header && (!limitToImage || dependency.reexported)) {
                                const auto& imported = exported->second.imported_name.empty()
                                    ? current : exported->second.imported_name;
                                return resolve(*dependency.header, imported, limitToImage);
                            }
                        }
                    }
                }
                for (const auto& dependency : metadata.dependencies) {
                    if (!dependency.header || (limitToImage && !dependency.reexported)) continue;
                    if (const auto target = resolve(*dependency.header, current, limitToImage)) {
                        return target;
                    }
                }
                return std::nullopt;
            };
            return resolve(startHeader, symbol, limitToImage);
        };
        if (name.empty()) return std::nullopt;
        const auto symbol = "_" + name;
        if (mainOnly) {
            const auto mainHeader = static_cast<std::uint64_t>(image.main_header);
            const auto& metadata = dlsymImage(mainHeader);
            if (metadata.cached && image.cache_reader) {
                if (const auto direct = image.cache_reader->exportSymbol(metadata.path, symbol)) {
                    const auto original = image.cache_reader->images().find(metadata.path);
                    if (original != image.cache_reader->images().end() &&
                        direct->offset <= std::numeric_limits<std::uint64_t>::max() -
                                              original->second) {
                        const auto guest = image.cache_reader->guestAddress(
                            original->second + direct->offset);
                        if (guest >= 0) return static_cast<std::uint64_t>(guest);
                    }
                }
                return std::nullopt;
            }
            if (const auto exported = metadata.exports.find(symbol);
                exported != metadata.exports.end() &&
                exported->second.kind == ExportKind::address &&
                exported->second.address <=
                    std::numeric_limits<std::uint64_t>::max() - mainHeader) {
                return mainHeader + exported->second.address;
            }
            return std::nullopt;
        }
        if (defaultScope) {
            for (const auto globalHeader : image.global_scope_headers) {
                if (const auto address = lookupName(symbol, globalHeader, true)) return address;
            }
            return std::nullopt;
        }
        return lookupName(symbol, *header, firstOnly);
    };

    GuestSignalRuntime signalRuntime;
    const auto ensureMutex = [&](std::uint64_t address) {
        auto* bytes = guestMemory.buffer(address, sizeof(pthread_mutex_t), true);
        if (guestScheduler.has_mutex(address)) return true;
        std::uint64_t signature = 0;
        std::memcpy(&signature, bytes, sizeof(signature));
        std::int32_t kind = -1;
        if (signature == 0x32aaaba7ULL) kind = PTHREAD_MUTEX_NORMAL;
        else if (signature == 0x32aaaba1ULL) kind = PTHREAD_MUTEX_ERRORCHECK;
        else if (signature == 0x32aaaba2ULL) kind = PTHREAD_MUTEX_RECURSIVE;
        return kind >= 0 && guestScheduler.initialize_mutex(address, kind) == 0;
    };
    const auto ensureCondition = [&](std::uint64_t address) {
        auto* bytes = guestMemory.buffer(address, sizeof(pthread_cond_t), true);
        if (guestScheduler.has_condition(address)) return true;
        std::uint64_t signature = 0;
        std::memcpy(&signature, bytes, sizeof(signature));
        return signature == 0x3cb0b1bbULL &&
               guestScheduler.initialize_condition(address) == 0;
    };
    std::size_t cacheCursor = image.cache_backing_start();
    if (image.cache_backing_end() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
        image.mapping_backing_end() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("Guest mapping backing exceeds allocator range");
    }
    GuestHeap mappingHeap({static_cast<std::int64_t>(image.cache_backing_end() + 40),
                           static_cast<std::int64_t>(image.mapping_backing_end())});
    std::uint64_t namedImagePlacementCursor = std::uint64_t{1} << 46U;
    const auto monotonicNanoseconds = []() -> std::uint64_t {
        timespec now{};
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0) {
            throw std::runtime_error("Unable to read the monotonic clock");
        }
        const auto seconds = static_cast<std::uint64_t>(now.tv_sec);
        const auto fraction = static_cast<std::uint64_t>(now.tv_nsec);
        if (seconds > (std::numeric_limits<std::uint64_t>::max() - fraction) / 1'000'000'000ULL) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return seconds * 1'000'000'000ULL + fraction;
    };
    const auto conditionDeadline = [&](const timespec& value, bool relative)
        -> std::optional<std::uint64_t> {
        if (value.tv_nsec < 0 || value.tv_nsec >= 1'000'000'000L ||
            (relative && value.tv_sec < 0)) return std::nullopt;
        const auto fraction = static_cast<std::uint64_t>(value.tv_nsec);
        std::uint64_t requested = fraction;
        if (value.tv_sec >= 0) {
            const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
            requested = seconds >
                    (std::numeric_limits<std::uint64_t>::max() - fraction) /
                        1'000'000'000ULL
                ? std::numeric_limits<std::uint64_t>::max()
                : seconds * 1'000'000'000ULL + fraction;
        }
        const auto monotonicNow = monotonicNanoseconds();
        std::uint64_t remaining = requested;
        if (!relative) {
            timespec wall{};
            if (clock_gettime(CLOCK_REALTIME, &wall) != 0) return std::nullopt;
            std::uint64_t wallNanos = static_cast<std::uint64_t>(wall.tv_nsec);
            if (wall.tv_sec >= 0) {
                const auto seconds = static_cast<std::uint64_t>(wall.tv_sec);
                wallNanos = seconds >
                        (std::numeric_limits<std::uint64_t>::max() - wallNanos) /
                            1'000'000'000ULL
                    ? std::numeric_limits<std::uint64_t>::max()
                    : seconds * 1'000'000'000ULL + wallNanos;
            }
            remaining = requested > wallNanos ? requested - wallNanos : 0;
        }
        return monotonicNow + std::min(
            remaining, std::numeric_limits<std::uint64_t>::max() - monotonicNow);
    };
    const auto syncThreadSwitch = [&](bool requeue, std::vector<std::uint64_t>& live) {
        std::vector<std::uint64_t> globalTail;
        if (live.size() >= guestTokenCount) {
            globalTail.assign(live.begin() + static_cast<std::ptrdiff_t>(guestTokenCount),
                              live.end());
        }
        bool switched = guestScheduler.schedule(
            requeue, live, hostServiceState.threadPendingSignals,
            hostServiceState.threadSignalMask);
        while (!switched && !requeue) {
            const auto now = monotonicNanoseconds();
            const auto activeId = guestScheduler.current_id();
            const auto* activeBeforeWake = guestScheduler.find(activeId);
            const bool activeWasWaiting = activeBeforeWake != nullptr &&
                                          activeBeforeWake->wait.has_value();
            hostServiceState.ingest_host_signals();
            if (const auto interruption = guestScheduler.interrupt_sleeping_thread(
                    hostServiceState.queuedProcessSignals, hostServiceState.signals, now)) {
                auto* errorAddress = guestMemory.buffer(interruption->errnoAddress,
                                                        sizeof(std::uint32_t), true);
                const std::uint32_t interruptedError = EINTR;
                std::memcpy(errorAddress, &interruptedError, sizeof(interruptedError));
                if (interruption->remainderAddress) {
                    timespec remaining{};
                    remaining.tv_sec = static_cast<time_t>(
                        interruption->remainingNanoseconds / 1'000'000'000ULL);
                    remaining.tv_nsec = static_cast<long>(
                        interruption->remainingNanoseconds % 1'000'000'000ULL);
                    auto* remainder = guestMemory.buffer(*interruption->remainderAddress,
                                                          sizeof(remaining), true);
                    std::memcpy(remainder, &remaining, sizeof(remaining));
                }
                guestScheduler.wake(interruption->thread, -1);
            }
            (void)guestScheduler.expire_waiters(now);
            if (activeWasWaiting) {
                const auto* activeAfterWake = guestScheduler.find(activeId);
                if (activeAfterWake != nullptr && !activeAfterWake->wait.has_value()) {
                    live = activeAfterWake->registers;
                }
            }
            if (!guestScheduler.ready_queue().empty()) {
                const bool selected = guestScheduler.schedule(
                    false, live, hostServiceState.threadPendingSignals,
                    hostServiceState.threadSignalMask);
                if (selected) {
                    switched = true;
                    break;
                }
            }
            const auto deadline = guestScheduler.earliest_deadline();
            if (!deadline) {
                throw std::runtime_error("All guest threads are blocked with no deadline or runnable thread");
            }
            if (*deadline > now) {
                const auto interval = std::min<std::uint64_t>(*deadline - now, 1'000'000ULL);
                std::this_thread::sleep_for(std::chrono::nanoseconds(interval));
            }
        }
        if (live.size() < guestTokenCount + globalTail.size()) {
            live.resize(guestTokenCount + globalTail.size(), 0);
        }
        std::copy(globalTail.begin(), globalTail.end(),
                  live.begin() + static_cast<std::ptrdiff_t>(guestTokenCount));
        std::memcpy(stateBuffer.data(), live.data(), live.size() * sizeof(std::uint64_t));
        if (switched) {
            const auto pc = live[33];
            if (pc > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::runtime_error("Guest thread resumed outside translated text");
            }
            selectKernel(static_cast<std::int64_t>(pc));
        }
        return switched;
    };
    const GuestTlvRuntime::Read guestTlvRead = [&](
        std::uint64_t address, std::uint8_t* destination, std::size_t size) {
        const auto* source = guestMemory.buffer(address, size);
        std::memcpy(destination, source, size);
    };
    const GuestTlvRuntime::Write guestTlvWrite = [&](
        std::uint64_t address, const std::uint8_t* source, std::size_t size) {
        auto* destination = guestMemory.buffer(address, size, true);
        std::memcpy(destination, source, size);
    };
    const GuestTlvRuntime::Release guestTlvRelease = [&](std::uint64_t address) {
        if (address > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error("Guest TLV allocation exceeds the heap address range");
        }
        hostServiceState.heap->release(static_cast<std::int64_t>(address));
    };
    const auto finishGuestThread = [&](std::uint64_t result,
                                       std::vector<std::uint64_t>& live) {
        const auto threadTsd = guestScheduler.current().tsdAddress;
        if (threadTsd <= 0) throw std::runtime_error("Guest thread has no TSD for TLV teardown");
        guestTlvRuntime.release_thread(static_cast<std::uint64_t>(threadTsd),
                                       guestTlvRelease, guestTlvWrite);
        auto completion = guestScheduler.complete_current(result);
        if (completion.joined) {
            if (completion.joined->outputAddress != 0) {
                auto* output = guestMemory.buffer(completion.joined->outputAddress,
                                                   sizeof(std::uint64_t), true);
                std::memcpy(output, &completion.joined->result, sizeof(std::uint64_t));
            }
        }
        if (completion.reapedThread && completion.reapedThread->allocation) {
            hostServiceState.heap->release(*completion.reapedThread->allocation);
        }
        if (completion.allComplete) return true;
        if (!syncThreadSwitch(false, live)) {
            throw std::runtime_error("All guest threads are blocked with no runnable thread");
        }
        return false;
    };
    struct VmRegion {
        std::uint64_t begin;
        std::uint64_t end;
        std::size_t backing;
        bool readable;
        bool writable;
        bool executable;
        std::uint32_t maximumProtection = 0;
        bool jit = false;
    };
    struct VmAllocation {
        std::uint64_t begin;
        std::uint64_t end;
        std::size_t backing;
        std::vector<VmRegion> regions;
    };
    std::map<std::uint64_t, VmAllocation> vmAllocations;
    std::set<std::uint64_t> dynamicCodeRegionStarts;
    struct GuestReceiveRight { std::uint32_t flags; std::uint64_t context; };
    std::map<std::uint32_t, GuestReceiveRight> guestReceiveRights;
    std::uint32_t nextGuestReceiveName = 0x80000003U;
    struct RestartableRange { std::uint64_t begin; std::uint64_t end; std::uint64_t recovery; };
    std::vector<RestartableRange> restartableRanges;
    struct GuestCallbackFrame {
        std::vector<std::uint64_t> savedRegisters;
        std::vector<std::uint64_t> savedVectorAndAuth;
        std::uint64_t authenticationDepth = 0;
        std::uint64_t mutationBlock = 0;
        std::vector<std::uint64_t> objcHeaders;
        std::uint64_t callback = 0;
        std::vector<std::array<std::uint64_t, 4>> arguments;
        std::size_t nextArgument = 0;
        std::vector<std::uint64_t> initializerTargets;
        std::size_t nextInitializer = 0;
        std::optional<std::uint64_t> stopAddress;
        std::optional<std::uint64_t> exitDso;
        std::optional<std::int32_t> exitStatus;
        std::uint64_t exitThread = 0;
    };
    std::unordered_map<std::uint64_t, std::vector<GuestCallbackFrame>> callbackFramesByThread;
    GuestExitRegistry exitRegistry;
    std::set<std::pair<std::int64_t, std::int64_t>> cachedReadWindows;
    auto ensureTokenCapacity = [&](std::size_t additional) {
        auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
        const auto count = static_cast<std::size_t>(live[guestTokenCount]);
        auto capacity = static_cast<std::size_t>(live[guestTokenCapacity]);
        if (additional <= capacity - std::min(capacity, count)) return;
        if (count > std::numeric_limits<std::size_t>::max() - additional) {
            throw std::runtime_error("Guest authentication table size overflow");
        }
        const auto required = count + additional;
        constexpr std::size_t maximumCapacity = 131072;
        if (required > maximumCapacity) {
            throw std::runtime_error("Guest authentication table exceeds runner capacity");
        }
        capacity = std::min(maximumCapacity, std::max(required, std::max<std::size_t>(capacity * 2, 1)));
        auto expanded = backend.makeSharedBuffer((guestTokenStart + 3 * capacity) * sizeof(std::uint64_t));
        if (expanded.data() == nullptr) throw std::runtime_error("Guest authentication table allocation failed");
        std::memcpy(expanded.data(), stateBuffer.data(), stateBuffer.size());
        auto* expandedRegisters = static_cast<std::uint64_t*>(expanded.data());
        expandedRegisters[guestTokenCapacity] = capacity;
        stateBuffer = std::move(expanded);
        processRegisters.resize(guestTokenStart + 3 * capacity, 0);
        std::memcpy(processRegisters.data(), stateBuffer.data(), stateBuffer.size());
    };
    const auto validateStagedNamedLoad = [&](const StagedNamedLoad& plan) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        std::uint64_t synthetic = 0x80000000ULL;
        const auto observeSynthetic = [&synthetic](std::int64_t address) {
            if (address >= 0x80000000LL &&
                static_cast<std::uint64_t>(address) >= synthetic &&
                static_cast<std::uint64_t>(address) <=
                    std::numeric_limits<std::uint64_t>::max() - 4) {
                synthetic = static_cast<std::uint64_t>(address) + 4;
            }
        };
        for (const auto& [address, unused] : imports) {
            (void)unused;
            observeSynthetic(address);
        }
        for (const auto& [address, unused] : guestStubs) {
            (void)unused;
            observeSynthetic(address);
        }
        for (std::size_t index = 0; index < plan.images.size(); ++index) {
            const auto& staged = plan.images[index];
            if (staged.image_bytes != staged.image.memory.size() ||
                staged.image_bytes == 0 || staged.guest_header >
                    std::numeric_limits<std::uint64_t>::max() - staged.image_bytes) {
                throw std::runtime_error("Invalid staged guest image extent");
            }
            for (const auto& dependency : staged.dependencies) {
                if (dependency.staged_image_index &&
                    *dependency.staged_image_index >= plan.images.size()) {
                    throw std::runtime_error("Invalid staged dependency index");
                }
            }
            for (const auto& bind : staged.binds) {
                if (bind.ignored_eager_lazy_binder) continue;
                const auto width = bind.is_stub ? std::uint64_t{4} : std::uint64_t{8};
                if (bind.guest_slot < staged.guest_header ||
                    bind.guest_slot - staged.guest_header > staged.image_bytes ||
                    width > staged.image_bytes -
                        static_cast<std::size_t>(bind.guest_slot - staged.guest_header)) {
                    throw std::runtime_error("Staged binding slot is outside its image");
                }
                if (bind.unresolved || (!bind.resolved_target && !bind.deferred_target &&
                    !bind.host_service_id &&
                    !bind.guest_memory_import && !bind.weak_null)) {
                    throw std::runtime_error("Unresolved staged guest binding " +
                                             bind.resolved_name);
                }
                const auto resolvedTarget = bind.resolved_target
                    ? bind.resolved_target : bind.deferred_target;
                if (resolvedTarget && bind.imported.addend != 0) {
                    const auto target = *resolvedTarget;
                    const auto addend = bind.imported.addend;
                    if ((addend > 0 && target > std::numeric_limits<std::uint64_t>::max() -
                                                  static_cast<std::uint64_t>(addend)) ||
                        (addend < 0 && target < static_cast<std::uint64_t>(-(addend + 1)) + 1)) {
                        throw std::runtime_error("Staged binding addend overflows");
                    }
                    const auto adjusted = addend > 0
                        ? target + static_cast<std::uint64_t>(addend)
                        : target - (static_cast<std::uint64_t>(-(addend + 1)) + 1);
                    if (adjusted > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max())) {
                        throw std::runtime_error("Staged binding target exceeds guest range");
                    }
                }
                if (!bind.is_stub && !resolvedTarget &&
                    (bind.host_service_id || bind.guest_memory_import)) {
                    while (synthetic <= static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()) &&
                           (imports.find(static_cast<std::int64_t>(synthetic)) != imports.end() ||
                            guestStubs.find(static_cast<std::int64_t>(synthetic)) !=
                                guestStubs.end())) {
                        synthetic += 4;
                    }
                    const auto addend = bind.imported.addend;
                    if ((addend > 0 && synthetic > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max()) -
                            static_cast<std::uint64_t>(addend)) ||
                        (addend < 0 && synthetic <
                            static_cast<std::uint64_t>(-(addend + 1)) + 1)) {
                        throw std::runtime_error("Staged synthetic binding addend overflows");
                    }
                    synthetic += 4;
                }
            }
            for (const auto& [offset, unused] : staged.image.authentication) {
                (void)unused;
                if (offset > staged.image_bytes || sizeof(std::uint64_t) >
                    staged.image_bytes - offset) {
                    throw std::runtime_error("Staged authentication slot is outside its image");
                }
            }
            for (const auto& segment : staged.image.mapped_segments) {
                if (segment.size == 0) continue;
                if (segment.offset > staged.image_bytes ||
                    segment.size > staged.image_bytes - segment.offset ||
                    segment.offset > std::numeric_limits<std::uint64_t>::max() -
                                         staged.guest_header ||
                    segment.size > std::numeric_limits<std::uint64_t>::max() -
                        (staged.guest_header + segment.offset)) {
                    throw std::runtime_error("Staged guest segment exceeds image bounds");
                }
                const auto begin = staged.guest_header + segment.offset;
                const auto end = begin + segment.size;
                if (end > static_cast<std::uint64_t>(
                              std::numeric_limits<std::int64_t>::max())) {
                    throw std::runtime_error("Staged guest segment exceeds address range");
                }
                for (const auto& prior : ranges) {
                    if (begin < prior.second && prior.first < end) {
                        throw std::runtime_error("Staged guest segments overlap");
                    }
                }
                ranges.emplace_back(begin, end);
            }
        }
    };
    const auto publishStagedNamedLoad = [&](StagedNamedLoad& plan) {
        std::size_t segmentCount = 0;
        std::size_t authenticatedCount = 0;
        for (const auto& staged : plan.images) {
            segmentCount += static_cast<std::size_t>(std::count_if(
                staged.image.mapped_segments.begin(), staged.image.mapped_segments.end(),
                [](const auto& segment) { return segment.size != 0; }));
            authenticatedCount += staged.image.authentication.size();
        }
        const auto requiredRows = segmentCount + plan.images.size();
        if (requiredRows > residentCapacity - std::min<std::size_t>(
                residentCapacity, static_cast<std::size_t>(resident[0]))) {
            throw std::runtime_error("Named guest image exceeds resident mapping capacity");
        }
        ensureTokenCapacity(authenticatedCount);
        const auto allocateImageBacking = [&](std::size_t bytes)
            -> std::optional<std::int64_t> {
            auto allocation = mappingHeap.allocate(bytes, 16);
            if (allocation) return allocation;
            if (resident[0] >= residentCapacity) {
                throw std::runtime_error("Guest resident map capacity exceeded by mapping guard");
            }
            const auto oldSize = guestMemory.size();
            if (!hostServiceState.growGuestMemory ||
                bytes > std::numeric_limits<std::size_t>::max() - oldSize ||
                !hostServiceState.growGuestMemory(guestMemory, oldSize + bytes)) {
                return std::nullopt;
            }
            const auto newSize = guestMemory.size();
            if (newSize <= oldSize || oldSize > static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max()) ||
                newSize > static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max())) {
                return std::nullopt;
            }
            if (resident[0] >= residentCapacity) {
                throw std::runtime_error("Guest resident map capacity exceeded by mapping guard");
            }
            mappingHeap.extend({static_cast<std::int64_t>(oldSize),
                                static_cast<std::int64_t>(newSize)});
            hostServiceState.heapEnd = newSize;
            const auto row = 1 + 4 * static_cast<std::size_t>(resident[0]);
            resident[row] = oldSize;
            resident[row + 1] = newSize;
            resident[row + 2] = oldSize;
            resident[row + 3] = 2U;
            ++resident[0];
            guestMemory.add_mapping({oldSize, newSize, oldSize, false, false});
            std::memcpy(residentBuffer.data(), resident.data(),
                        resident.size() * sizeof(std::uint64_t));
            return mappingHeap.allocate(bytes, 16);
        };
        std::vector<std::int64_t> allocations;
        allocations.reserve(plan.images.size());
        for (const auto& staged : plan.images) {
            auto allocation = allocateImageBacking(staged.image_bytes);
            if (!allocation || *allocation < 0 ||
                static_cast<std::uint64_t>(*allocation) > memoryBuffer.size() ||
                staged.image_bytes > memoryBuffer.size() -
                    static_cast<std::size_t>(*allocation)) {
                if (allocation) mappingHeap.release(*allocation);
                for (const auto prior : allocations) mappingHeap.release(prior);
                throw std::runtime_error(
                    "Named guest image exceeds reserved Metal mapping backing; memory growth is required");
            }
            allocations.push_back(*allocation);
        }
        auto* guestBytes = static_cast<std::uint8_t*>(memoryBuffer.data());
        std::vector<std::uint64_t> initializerTargets;
        std::uint64_t synthetic = 0x80000000ULL;
        for (const auto& [address, unused] : imports) {
            (void)unused;
            if (address >= 0x80000000LL &&
                static_cast<std::uint64_t>(address) >= synthetic &&
                static_cast<std::uint64_t>(address) <=
                    std::numeric_limits<std::uint64_t>::max() - 4) {
                synthetic = static_cast<std::uint64_t>(address) + 4;
            }
        }
        const auto makeImportStub = [&](const std::string& name) {
            while (synthetic <= static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max()) &&
                   (imports.find(static_cast<std::int64_t>(synthetic)) != imports.end() ||
                    guestStubs.find(static_cast<std::int64_t>(synthetic)) != guestStubs.end())) {
                synthetic += 4;
            }
            if (synthetic > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) - 4) {
                throw std::runtime_error("Guest synthetic import address space exhausted");
            }
            const auto address = synthetic;
            synthetic += 4;
            imports.emplace(static_cast<std::int64_t>(address), name);
            return address;
        };
        for (std::size_t index = 0; index < plan.images.size(); ++index) {
            auto& staged = plan.images[index];
            const auto backing = static_cast<std::size_t>(allocations[index]);
            std::memcpy(guestBytes + backing, staged.image.memory.data(), staged.image_bytes);
            const auto storeGuestWord = [&](std::uint64_t address, std::uint64_t value) {
                if (address < staged.guest_header ||
                    address - staged.guest_header > staged.image_bytes ||
                    sizeof(value) > staged.image_bytes -
                        static_cast<std::size_t>(address - staged.guest_header)) {
                    throw std::runtime_error("Named image binding is outside its staged image");
                }
                auto* destination = guestBytes + backing +
                    static_cast<std::size_t>(address - staged.guest_header);
                for (unsigned byte = 0; byte < sizeof(value); ++byte) {
                    destination[byte] = static_cast<std::uint8_t>(value >> (byte * 8U));
                }
            };
            for (const auto& bind : staged.binds) {
                if (bind.ignored_eager_lazy_binder) continue;
                if (bind.is_stub) {
                    if (bind.host_service_id || bind.guest_memory_import) {
                        imports[static_cast<std::int64_t>(bind.guest_slot)] = bind.resolved_name;
                    } else if (bind.resolved_target || bind.deferred_target) {
                        auto target = bind.resolved_target ? *bind.resolved_target
                                                           : *bind.deferred_target;
                        const auto addend = bind.imported.addend;
                        if (addend > 0) target += static_cast<std::uint64_t>(addend);
                        else if (addend < 0)
                            target -= static_cast<std::uint64_t>(-(addend + 1)) + 1;
                        guestStubs[static_cast<std::int64_t>(bind.guest_slot)] =
                            static_cast<std::int64_t>(target);
                    } else if (bind.weak_null) {
                        guestStubs[static_cast<std::int64_t>(bind.guest_slot)] = 0;
                    } else {
                        throw std::runtime_error("Unresolved staged guest stub " +
                                                 bind.resolved_name);
                    }
                    continue;
                }
                const auto resolvedTarget = bind.resolved_target
                    ? bind.resolved_target : bind.deferred_target;
                std::uint64_t target = resolvedTarget.value_or(0);
                if (bind.host_service_id || bind.guest_memory_import) {
                    target = makeImportStub(bind.resolved_name);
                } else if (!resolvedTarget && !bind.weak_null) {
                    throw std::runtime_error("Unresolved staged guest binding " +
                                             bind.resolved_name);
                }
                if (target != 0 && bind.imported.addend != 0) {
                    const auto addend = bind.imported.addend;
                    if (addend > 0) target += static_cast<std::uint64_t>(addend);
                    else target -= static_cast<std::uint64_t>(-(addend + 1)) + 1;
                }
                storeGuestWord(bind.guest_slot, target);
                image.bindings[static_cast<std::size_t>(bind.guest_slot)] = bind.resolved_name;
                image.function_bindings[static_cast<std::size_t>(bind.guest_slot)] =
                    static_cast<std::int64_t>(target);
            }
            for (const auto& [offset, authentication] : staged.image.authentication) {
                auto* slotBytes = guestBytes + backing + offset;
                std::uint64_t target = 0;
                for (unsigned byte = 0; byte < sizeof(target); ++byte) {
                    target |= static_cast<std::uint64_t>(slotBytes[byte]) << (byte * 8U);
                }
                const auto guestSlot = staged.guest_header + offset;
                image.authentication[static_cast<std::size_t>(guestSlot)] = authentication;
                if (target == 0) continue;
                auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
                const auto token = static_cast<std::size_t>(live[guestTokenCount]);
                const auto tokenSlot = guestTokenStart + 3 * token;
                live[tokenSlot] = target;
                live[tokenSlot + 1] = authentication.modifier;
                live[tokenSlot + 2] = authentication.key;
                live[guestTokenCount] = token + 1;
                const auto encoded = encode_guest_pointer(target, authentication.key, token);
                for (unsigned byte = 0; byte < sizeof(encoded); ++byte) {
                    slotBytes[byte] = static_cast<std::uint8_t>(encoded >> (byte * 8U));
                }
            }
            RuntimeLocalImageBytes runtimeImage;
            runtimeImage.guest_header = staged.guest_header;
            runtimeImage.bytes = staged.image.memory;
            for (const auto& code : staged.image.code) {
                runtimeImage.executable_ranges.emplace_back(
                    staged.guest_header + code.first, staged.guest_header + code.second);
                image.text_regions.emplace_back(
                    static_cast<std::int64_t>(staged.guest_header + code.first),
                    static_cast<std::int64_t>(staged.guest_header + code.second));
                image.code_abi_regions.push_back({
                    static_cast<std::int64_t>(staged.guest_header + code.first),
                    static_cast<std::int64_t>(staged.guest_header + code.second),
                    staged.image.arm64e_code_abi});
            }
            image.runtime_local_images.push_back(std::move(runtimeImage));
            image.image_paths[staged.guest_header] = staged.canonical_path;
            image.image_slides[staged.guest_header] =
                staged.guest_header - staged.image.preferred_base;
            imageHeadersByPath[staged.canonical_path] = staged.guest_header;
            imageHeadersByPath[normalizeImagePath(staged.canonical_path)] = staged.guest_header;
            imageHandles.register_loaded(staged.canonical_path, staged.guest_header);
            for (const auto& segment : staged.image.mapped_segments) {
                if (segment.size == 0) continue;
                if (segment.offset > staged.image_bytes ||
                    segment.size > staged.image_bytes - segment.offset ||
                    segment.offset > std::numeric_limits<std::uint64_t>::max() - staged.guest_header ||
                    segment.size > std::numeric_limits<std::uint64_t>::max() -
                        (staged.guest_header + segment.offset)) {
                    throw std::runtime_error("Staged guest segment exceeds image bounds");
                }
                const auto begin = staged.guest_header + segment.offset;
                const auto end = begin + segment.size;
                if (end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    throw std::runtime_error("Staged guest segment exceeds address range");
                }
                const auto row = 1 + 4 * static_cast<std::size_t>(resident[0]);
                resident[row] = begin;
                resident[row + 1] = end;
                resident[row + 2] = backing + segment.offset;
                resident[row + 3] = segment.data_segment ? 1U : 0U;
                ++resident[0];
                guestMemory.add_mapping({begin, end, backing + segment.offset,
                                         segment.data_segment, true});
                image.mapped_image_segments.push_back({
                    staged.guest_header, static_cast<std::int64_t>(begin),
                    static_cast<std::int64_t>(end), staged.canonical_path, segment.name,
                    segment.executable, segment.data_segment});
            }
            initializerTargets.insert(initializerTargets.end(),
                                      staged.initializer_targets.begin(),
                                      staged.initializer_targets.end());
        }
        std::memcpy(residentBuffer.data(), resident.data(),
                    resident.size() * sizeof(std::uint64_t));
        for (const auto& staged : plan.images) {
            LoadedImageGraphRecord record;
            record.guest_header = staged.guest_header;
            record.canonical_path = staged.canonical_path;
            record.rpaths = staged.rpaths;
            record.exports = staged.image.exports;
            record.dependencies.reserve(staged.dependencies.size());
            for (const auto& dependency : staged.dependencies) {
                LoadedImageDependency loaded{dependency.declaration, dependency.existing_header};
                if (dependency.staged_image_index) {
                    if (*dependency.staged_image_index >= plan.images.size()) {
                        throw std::runtime_error("Invalid staged dependency index");
                    }
                    loaded.resolved_header = plan.images[*dependency.staged_image_index].guest_header;
                }
                record.dependencies.push_back(std::move(loaded));
            }
            image.loaded_image_graph.push_back(std::move(record));
        }
        return initializerTargets;
    };
    hostServiceState.growGuestMemory = [&](GuestMemory& guestMemoryView,
                                            std::size_t minimumSize) {
        const auto oldSize = guestMemoryView.size();
        const auto deviceLimit = backend.maxBufferLength();
        const auto linearLimit = static_cast<std::size_t>(SharedCache::guestBase - 1);
        const auto maximum = std::min(deviceLimit, linearLimit);
        if (minimumSize <= oldSize || minimumSize > maximum) return false;
        const auto pageSize = static_cast<std::size_t>(bootstrap.pageSize);
        if (pageSize == 0 || minimumSize > std::numeric_limits<std::size_t>::max() - (pageSize - 1)) {
            return false;
        }
        const auto roundedMinimum = (minimumSize + pageSize - 1) & ~(pageSize - 1);
        const auto doubled = oldSize > maximum / 2 ? maximum : oldSize * 2;
        auto targetSize = std::max(roundedMinimum, doubled);
        if (targetSize > maximum) targetSize = roundedMinimum;
        if (targetSize > maximum) return false;
        try {
            auto expanded = backend.makeSharedBuffer(targetSize);
            if (expanded.data() == nullptr) return false;
            const auto* previous = static_cast<const std::uint8_t*>(memoryBuffer.data());
            std::memcpy(expanded.data(), previous, oldSize);
            std::memset(static_cast<std::uint8_t*>(expanded.data()) + oldSize, 0,
                        targetSize - oldSize);
            memoryBuffer = std::move(expanded);
            guestMemoryView.rebind(static_cast<std::uint8_t*>(memoryBuffer.data()), targetSize);
            const auto newGuestSize = static_cast<std::uint64_t>(targetSize);
            std::memcpy(sizeBuffer.data(), &newGuestSize, sizeof(newGuestSize));
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    const auto allocateGuestHeap = [&](std::uint64_t size,
                                       std::uint64_t alignment = 16)
        -> std::optional<std::int64_t> {
        if (alignment == 0 || alignment > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()) ||
            (alignment & (alignment - 1)) != 0) return std::nullopt;
        auto allocation = hostServiceState.heap->allocate(
            size, static_cast<std::int64_t>(alignment));
        if (allocation || !hostServiceState.growGuestMemory) return allocation;
        const auto padding = alignment - 1;
        if (size > std::numeric_limits<std::uint64_t>::max() - padding) return std::nullopt;
        const auto extra = std::max<std::uint64_t>(size + padding, 16);
        if (extra > std::numeric_limits<std::size_t>::max() - guestMemory.size()) {
            return std::nullopt;
        }
        const auto minimum = guestMemory.size() + static_cast<std::size_t>(extra);
        if (!hostServiceState.growGuestMemory(guestMemory, minimum) ||
            guestMemory.size() <= hostServiceState.heapEnd ||
            guestMemory.size() > static_cast<std::size_t>(
                std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        hostServiceState.heap->extend({static_cast<std::int64_t>(hostServiceState.heapEnd),
                                       static_cast<std::int64_t>(guestMemory.size())});
        hostServiceState.heapEnd = guestMemory.size();
        return hostServiceState.heap->allocate(size, static_cast<std::int64_t>(alignment));
    };
    const auto setGuestDlerror = [&](const std::string& message) {
        auto& error = dlerrorByThread[guestScheduler.current_id()];
        const auto required = static_cast<std::uint64_t>(message.size()) + 1;
        if (required > error.capacity) {
            const auto allocation = allocateGuestHeap(required);
            if (!allocation) throw std::runtime_error("Unable to allocate guest dlerror text");
            if (error.allocation != 0) hostServiceState.heap->release(error.allocation);
            error.allocation = *allocation;
            error.capacity = required;
        }
        auto* destination = guestMemory.buffer(
            static_cast<std::uint64_t>(error.allocation), required, true);
        std::memcpy(destination, message.data(), message.size());
        destination[message.size()] = 0;
        error.pending = true;
    };
    const auto clearGuestDlerror = [&]() {
        if (const auto error = dlerrorByThread.find(guestScheduler.current_id());
            error != dlerrorByThread.end()) error->second.pending = false;
    };
    const auto takeGuestDlerror = [&]() -> std::uint64_t {
        const auto error = dlerrorByThread.find(guestScheduler.current_id());
        if (error == dlerrorByThread.end() || !error->second.pending) return 0;
        error->second.pending = false;
        return static_cast<std::uint64_t>(error->second.allocation);
    };
    std::unordered_map<std::string, std::uint64_t> stableGuestStrings;
    const auto stableGuestString = [&](const std::string& value)
        -> std::optional<std::uint64_t> {
        if (const auto found = stableGuestStrings.find(value);
            found != stableGuestStrings.end()) return found->second;
        if (value.size() == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
        const auto allocation = allocateGuestHeap(static_cast<std::uint64_t>(value.size()) + 1);
        if (!allocation || *allocation < 0) return std::nullopt;
        const auto address = static_cast<std::uint64_t>(*allocation);
        auto* destination = guestMemory.buffer(address,
            static_cast<std::uint64_t>(value.size()) + 1, true);
        std::memcpy(destination, value.data(), value.size());
        destination[value.size()] = 0;
        stableGuestStrings.emplace(value, address);
        return address;
    };
    std::size_t startupIndex = 0;
    const auto advanceStartup = [&]() {
        if (startupIndex + 1 >= startup.size()) return false;
        progressTrace.event("initializer-done", startup[startupIndex], startupIndex);
        const auto* registers = static_cast<const std::uint64_t*>(stateBuffer.data());
        if (registers[37] != 0) {
            throw std::runtime_error("Guest initializer left its authentication stack unbalanced");
        }
        processRegisters.resize(stateBuffer.size() / sizeof(std::uint64_t));
        std::copy(registers, registers + processRegisters.size(), processRegisters.begin());
        ++startupIndex;
        enter_startup_frame(processRegisters, bootstrap, startup[startupIndex]);
        progressTrace.event(startupIndex + 1 == startup.size() ? "main-entry" : "initializer-start",
                            startup[startupIndex], startupIndex);
        std::memcpy(stateBuffer.data(), processRegisters.data(),
                    processRegisters.size() * sizeof(std::uint64_t));
        return true;
    };
    const auto makeCacheResident = [&](std::uint64_t requested,
                                       std::uint64_t width) -> bool {
        if (!image.cache_reader || image.deferred_cache.empty() ||
            requested > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            width > std::numeric_limits<std::size_t>::max()) return false;
        std::vector<CacheResidencyPlanner::Span> residentSpans;
        residentSpans.reserve(static_cast<std::size_t>(resident[0]));
        for (std::size_t index = 0; index < resident[0]; ++index) {
            const auto row = 1 + 4 * index;
            residentSpans.push_back({static_cast<std::int64_t>(resident[row]),
                                     static_cast<std::int64_t>(resident[row + 1])});
        }
        std::vector<CacheResidencyPlanner::DeferredSegment> deferred;
        deferred.reserve(image.deferred_cache.size());
        for (const auto& item : image.deferred_cache) {
            deferred.push_back({item.logical_start, item.logical_end, item.segment});
        }
        std::map<std::int64_t, std::string> deferredBindings;
        for (const auto& [address, symbol] : image.deferred_bindings) {
            if (address <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
                deferredBindings.emplace(static_cast<std::int64_t>(address), symbol);
            }
        }
        const CacheResidencyPlanner::Capacity capacity{
            cacheCursor, image.cache_backing_end(), static_cast<std::size_t>(resident[0]),
            residentCapacity};
        const auto plan = CacheResidencyPlanner::plan(
            *image.cache_reader, deferred, static_cast<std::int64_t>(requested),
            static_cast<std::size_t>(width), static_cast<std::size_t>(getpagesize()),
            residentSpans, cachedReadWindows, capacity, deferredBindings);
        if (!plan) return false;
        if (plan->backing > memoryBuffer.size() ||
            plan->bytes.size() > memoryBuffer.size() - plan->backing) {
            throw std::runtime_error("Planned guest cache bytes exceed process memory");
        }
        auto* memory = static_cast<std::uint8_t*>(memoryBuffer.data());
        std::copy(plan->bytes.begin(), plan->bytes.end(), memory + plan->backing);
        ensureTokenCapacity(plan->authentication_count);
        auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
        for (const auto& patch : plan->rebases) {
            std::uint64_t pointer = patch.guest_pointer;
            bool modeledBinding = false;
            if (patch.deferred_binding) {
                const auto modeled = bootstrap.modeledGlobalAddresses.find(*patch.deferred_binding);
                if (modeled != bootstrap.modeledGlobalAddresses.end()) {
                    pointer = modeled->second;
                    modeledBinding = true;
                }
            }
            if (patch.authentication && !modeledBinding) {
                const auto& auth = *patch.authentication;
                const auto index = static_cast<std::size_t>(live[guestTokenCount]);
                const auto slot = guestTokenStart + 3 * index;
                const auto modifier = auth.addressDiversity
                    ? static_cast<std::uint64_t>(plan->logical_start +
                          static_cast<std::int64_t>(patch.offset)) |
                          (static_cast<std::uint64_t>(auth.diversity) << 48U)
                    : auth.diversity;
                const std::uint64_t key = auth.dataKey ? 2 : 0;
                live[slot] = pointer;
                live[slot + 1] = modifier;
                live[slot + 2] = key;
                live[guestTokenCount] = index + 1;
                pointer = encode_guest_pointer(pointer, key, index);
            }
            if (patch.offset > plan->bytes.size() ||
                plan->bytes.size() - patch.offset < sizeof(pointer)) {
                throw std::runtime_error("Planned cache rebase exceeds mapped segment");
            }
            for (unsigned byte = 0; byte < 8; ++byte) {
                memory[plan->backing + patch.offset + byte] =
                    static_cast<std::uint8_t>(pointer >> (byte * 8U));
            }
        }
        const auto patchWord = [&](std::uint64_t address, std::uint64_t value,
                                   std::size_t size) {
            if (address < static_cast<std::uint64_t>(plan->logical_start) ||
                address + size > static_cast<std::uint64_t>(plan->logical_end)) return;
            const auto offset = plan->backing +
                static_cast<std::size_t>(address - plan->logical_start);
            for (std::size_t byte = 0; byte < size; ++byte) {
                memory[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8U));
            }
        };
        if (image.cached_task_port) patchWord(*image.cached_task_port, guestMachTaskPort, 4);
        if (const auto locale = image.cache_reader->localeKeyAddress()) patchWord(*locale, 10, 8);
        if (image.cached_dispatch_main_queue) {
            const auto stateAddress = static_cast<std::uint64_t>(
                *image.cached_dispatch_main_queue) + 0x38U;
            if (stateAddress >= static_cast<std::uint64_t>(plan->logical_start) &&
                stateAddress + 8 <= static_cast<std::uint64_t>(plan->logical_end)) {
                const auto offset = static_cast<std::size_t>(
                    stateAddress - static_cast<std::uint64_t>(plan->logical_start));
                std::uint64_t prior = 0;
                for (unsigned byte = 0; byte < 8; ++byte) {
                    prior |= static_cast<std::uint64_t>(plan->bytes[offset + byte]) <<
                             (byte * 8U);
                }
                constexpr std::uint64_t ownerMask = 0xffff'fffcULL;
                const auto owner = guestMachThreadName(guestMainThreadID) & ownerMask;
                patchWord(stateAddress, (prior & ~ownerMask) | owner, 8);
            }
        }
        const auto row = 1 + 4 * static_cast<std::size_t>(resident[0]);
        resident[row] = static_cast<std::uint64_t>(plan->logical_start);
        resident[row + 1] = static_cast<std::uint64_t>(plan->logical_end);
        resident[row + 2] = plan->backing;
        resident[row + 3] = (plan->writable ? 1U : 0U) | (plan->readable ? 0U : 2U);
        resident[0] += 1;
        std::memcpy(residentBuffer.data(), resident.data(), resident.size() * sizeof(std::uint64_t));
        guestMemory.add_mapping({static_cast<std::uint64_t>(plan->logical_start),
                                 static_cast<std::uint64_t>(plan->logical_end),
                                 static_cast<std::uint64_t>(plan->backing),
                                 plan->writable, plan->readable});
        if (!plan->writable && (plan->segment_name == "__TEXT" ||
                                plan->segment_name == "__OBJC_RO")) {
            cachedReadWindows.emplace(plan->logical_start, plan->logical_end);
        }
        cacheCursor = plan->next_cursor;
        return true;
    };
    std::optional<SharedCache::HostExports> stagedCacheHostExports;
    const auto publishStagedCacheImages = [&](const StagedNamedLoad& plan) {
        if (plan.cache_images.empty()) return std::vector<std::uint64_t>{};
        if (!image.cache_reader) {
            throw std::runtime_error("Staged cache image has no shared-cache reader");
        }
        if (!stagedCacheHostExports) {
            const auto memoryImports = guest_memory_imports();
            const std::unordered_set<std::string> memoryImportSet(
                memoryImports.begin(), memoryImports.end());
            stagedCacheHostExports.emplace(image.cache_reader->hostExportAddresses(
                host_service_ids(), memoryImportSet));
        }
        std::unordered_map<std::uint64_t, std::string> dataNames;
        for (const auto& [address, name] : stagedCacheHostExports->data)
            dataNames.emplace(address, name);
        const auto mergedDataBindings = image.cache_reader->mergedDataBindings(dataNames);
        std::vector<std::uint64_t> initializers;
        for (const auto& staged : plan.cache_images) {
            loadedImageHeaders.insert(staged.guest_header);
            activatedLazyHeaders.insert(staged.guest_header);
            image.image_slides[staged.guest_header] = staged.slide;
            const auto existingPath = image.image_paths.find(staged.guest_header);
            if (existingPath == image.image_paths.end() ||
                staged.install_name < existingPath->second) {
                image.image_paths[staged.guest_header] = staged.install_name;
            }
            image.cached_export_addresses[staged.install_name] = staged.image.exports;
            auto& dependencies = image.cached_image_dependencies[staged.install_name];
            dependencies.clear();
            dependencies.reserve(staged.image.dependencies.size());
            for (const auto& dependency : staged.image.dependencies) {
                dependencies.push_back({dependency.name, dependency.weak,
                                        dependency.reexported, 0, dependency.upward,
                                        false, dependency.delayed_init});
            }
            if (std::none_of(image.loaded_image_graph.begin(), image.loaded_image_graph.end(),
                    [&staged](const auto& existing) {
                        return existing.guest_header == staged.guest_header;
                    })) {
                LoadedImageGraphRecord record;
                record.guest_header = staged.guest_header;
                record.canonical_path = staged.install_name;
                record.dependencies.reserve(staged.image.dependencies.size());
                for (const auto& dependency : staged.image.dependencies) {
                    std::optional<std::uint64_t> dependencyHeader;
                    const auto stagedDependency = std::find_if(
                        plan.cache_images.begin(), plan.cache_images.end(),
                        [&dependency](const auto& candidate) {
                            return candidate.install_name == dependency.name;
                        });
                    if (stagedDependency != plan.cache_images.end()) {
                        dependencyHeader = stagedDependency->guest_header;
                    } else {
                        const auto loadedDependency = std::find_if(
                            image.image_paths.begin(), image.image_paths.end(),
                            [&dependency](const auto& candidate) {
                                return candidate.second == dependency.name;
                            });
                        if (loadedDependency != image.image_paths.end())
                            dependencyHeader = loadedDependency->first;
                    }
                    ImageDependency declaration{dependency.name, dependency.weak,
                        dependency.reexported, 0, dependency.upward, false,
                        dependency.delayed_init};
                    record.dependencies.push_back({std::move(declaration), dependencyHeader});
                }
                image.loaded_image_graph.push_back(std::move(record));
            }
            for (const auto& segment : staged.segments) {
                if (segment.logical_start < 0 || segment.logical_end <= segment.logical_start ||
                    static_cast<std::uint64_t>(segment.logical_end - segment.logical_start) !=
                        segment.segment.size || segment.segment.fileSize > segment.segment.size) {
                    throw std::runtime_error("Invalid staged cached image segment");
                }
                const auto knownSegment = std::any_of(
                    image.mapped_image_segments.begin(), image.mapped_image_segments.end(),
                    [&staged, &segment](const auto& existing) {
                        return existing.guest_header == staged.guest_header &&
                               existing.start == segment.logical_start &&
                               existing.end == segment.logical_end &&
                               existing.segment_name == segment.segment.name;
                    });
                const auto dataSegment = segment.segment.name.rfind("__DATA", 0) == 0 ||
                                         segment.segment.name.rfind("__AUTH", 0) == 0;
                if (!knownSegment) {
                    image.mapped_image_segments.push_back({
                        staged.guest_header, segment.logical_start, segment.logical_end,
                        staged.install_name, segment.segment.name, !dataSegment, dataSegment});
                }
                if (dataSegment) {
                    image.cached_image_ranges.push_back({segment.logical_start,
                        segment.logical_end, static_cast<std::int64_t>(staged.guest_header)});
                }
                const auto knownDeferred = std::any_of(
                    image.deferred_cache.begin(), image.deferred_cache.end(),
                    [&segment](const auto& existing) {
                        return existing.logical_start == segment.logical_start &&
                               existing.logical_end == segment.logical_end &&
                               existing.segment.address == segment.segment.address &&
                               existing.segment.name == segment.segment.name;
                    });
                if (!knownDeferred) {
                    image.deferred_cache.push_back({segment.logical_start,
                        segment.logical_end, segment.segment});
                }
            }
            for (const auto& text : staged.image.text) {
                if (text.second == text.first) continue;
                const auto start = image.cache_reader->guestAddress(text.first);
                const auto end = image.cache_reader->guestAddress(text.second);
                if (start < 0 || end <= start) {
                    throw std::runtime_error("Invalid staged cached image text range " +
                        staged.install_name + " [" + std::to_string(text.first) + "," +
                        std::to_string(text.second) + ") -> [" + std::to_string(start) +"," +
                        std::to_string(end) + ")");
                }
                const auto range = std::make_pair(start, end);
                if (std::find(image.text_regions.begin(), image.text_regions.end(), range) ==
                    image.text_regions.end()) {
                    image.text_regions.push_back(range);
                    image.code_abi_regions.push_back({start, end, true});
                }
            }
            for (const auto& hostExport : stagedCacheHostExports->functions) {
                const auto address = hostExport.first;
                const auto& name = hostExport.second;
                const auto owned = std::any_of(staged.segments.begin(), staged.segments.end(),
                    [address](const auto& segment) {
                        return address >= segment.logical_start && address < segment.logical_end;
                    });
                if (owned) imports[address] = name;
            }
            for (const auto& binding : mergedDataBindings) {
                const auto address = image.cache_reader->guestAddress(binding.rebase.location);
                const auto owned = std::any_of(staged.segments.begin(), staged.segments.end(),
                    [address](const auto& segment) {
                        return address >= segment.logical_start && address < segment.logical_end;
                    });
                if (owned) image.deferred_bindings[static_cast<std::size_t>(address)] = binding.name;
            }
            if (!staged.startup_active && !staged.delayed_initialization) {
                for (const auto& initializer : staged.initializers) {
                    if (initializer.high_byte != 0 || initializer.authentication) {
                        throw std::runtime_error(
                            "Authenticated cached initializer dispatch is unsupported");
                    }
                    initializers.push_back(initializer.target_address);
                }
            }
        }
        return initializers;
    };
    const auto mapSelectorWindow = [&](std::uint64_t address,
                                       std::size_t length) -> bool {
        if (!image.cache_reader || address < static_cast<std::uint64_t>(SharedCache::guestBase) ||
            address >= static_cast<std::uint64_t>(SharedCache::guestLimit) || length == 0) return false;
        const auto logicalAddress = static_cast<std::int64_t>(address);
        const auto window = image.cache_reader->selectorWindow(logicalAddress, length);
        auto lower = window.first;
        auto upper = window.second;
        const auto overlaps = [&](std::int64_t begin, std::int64_t end) {
            return begin < upper && lower < end;
        };
        for (std::size_t index = 0; index < resident[0]; ++index) {
            const auto row = 1 + 4 * index;
            const auto begin = static_cast<std::int64_t>(resident[row]);
            const auto end = static_cast<std::int64_t>(resident[row + 1]);
            if (!overlaps(begin, end)) continue;
            if (begin <= logicalAddress && logicalAddress < end) return false;
            if (end <= logicalAddress) lower = std::max(lower, end);
            if (begin > logicalAddress) upper = std::min(upper, begin);
        }
        for (const auto& segment : image.deferred_cache) {
            if (!overlaps(segment.logical_start, segment.logical_end)) continue;
            if (segment.logical_start <= logicalAddress && logicalAddress < segment.logical_end) return false;
            if (segment.logical_end <= logicalAddress) lower = std::max(lower, segment.logical_end);
            if (segment.logical_start > logicalAddress) upper = std::min(upper, segment.logical_start);
        }
        if (logicalAddress < lower || logicalAddress >= upper ||
            length > static_cast<std::uint64_t>(upper - logicalAddress)) return false;
        const auto regionSize = static_cast<std::size_t>(upper - lower);
        const auto allocated = mappingHeap.allocate(regionSize, 16);
        if (!allocated || resident[0] >= residentCapacity) {
            if (allocated) mappingHeap.release(*allocated);
            throw std::runtime_error("Cached selector mapping exceeds guest limits");
        }
        const auto backing = static_cast<std::size_t>(*allocated);
        const auto bytes = image.cache_reader->bytes(
            image.cache_reader->originalAddress(lower), regionSize);
        auto* memory = static_cast<std::uint8_t*>(memoryBuffer.data());
        std::copy(bytes.begin(), bytes.end(), memory + backing);
        const auto row = 1 + 4 * static_cast<std::size_t>(resident[0]);
        resident[row] = static_cast<std::uint64_t>(lower);
        resident[row + 1] = static_cast<std::uint64_t>(upper);
        resident[row + 2] = backing;
        resident[row + 3] = 0;
        resident[0] += 1;
        std::memcpy(residentBuffer.data(), resident.data(), resident.size() * sizeof(std::uint64_t));
        guestMemory.add_mapping({static_cast<std::uint64_t>(lower),
                                 static_cast<std::uint64_t>(upper),
                                 static_cast<std::uint64_t>(backing), false, true});
        cachedReadWindows.emplace(lower, upper);
        return true;
    };
    struct DynamicCodeReplacement {
        std::set<std::uint64_t> starts;
        std::vector<std::uint64_t> removedStarts;
        std::vector<RuntimeLocalImageBytes> addedImages;
        std::vector<std::pair<std::int64_t, std::int64_t>> addedTextRegions;
        bool changed = false;
    };
    const auto prepareDynamicCodeRegions = [&](std::uint64_t begin, std::uint64_t end,
                                                const std::vector<VmRegion>& regions) {
        DynamicCodeReplacement replacement;
        replacement.starts = dynamicCodeRegionStarts;
        for (auto iter = replacement.starts.begin(); iter != replacement.starts.end();) {
            if (*iter < begin || *iter >= end) {
                ++iter;
                continue;
            }
            replacement.removedStarts.push_back(*iter);
            iter = replacement.starts.erase(iter);
            replacement.changed = true;
        }
        auto* backingBytes = static_cast<const std::uint8_t*>(memoryBuffer.data());
        for (const auto& region : regions) {
            if (!region.executable) continue;
            if (region.begin > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                region.end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::runtime_error("Dynamic guest code exceeds the translated address range");
            }
            RuntimeLocalImageBytes runtimeImage;
            runtimeImage.guest_header = region.begin;
            runtimeImage.bytes.assign(backingBytes + region.backing,
                                      backingBytes + region.backing + (region.end - region.begin));
            runtimeImage.executable_ranges.emplace_back(region.begin, region.end);
            replacement.addedImages.push_back(std::move(runtimeImage));
            replacement.addedTextRegions.emplace_back(static_cast<std::int64_t>(region.begin),
                                                       static_cast<std::int64_t>(region.end));
            replacement.starts.insert(region.begin);
            replacement.changed = true;
        }
        image.runtime_local_images.reserve(
            image.runtime_local_images.size() + replacement.addedImages.size());
        image.text_regions.reserve(image.text_regions.size() +
                                   replacement.addedTextRegions.size());
        return replacement;
    };
    const auto commitDynamicCodeRegions = [&](DynamicCodeReplacement&& replacement) noexcept {
        const auto wasRemoved = [&replacement](std::uint64_t start) {
            return std::find(replacement.removedStarts.begin(),
                             replacement.removedStarts.end(), start) !=
                   replacement.removedStarts.end();
        };
        image.runtime_local_images.erase(std::remove_if(image.runtime_local_images.begin(),
            image.runtime_local_images.end(), [&](const RuntimeLocalImageBytes& runtimeImage) {
                return wasRemoved(runtimeImage.guest_header);
            }), image.runtime_local_images.end());
        image.text_regions.erase(std::remove_if(image.text_regions.begin(), image.text_regions.end(),
            [&](const auto& range) { return wasRemoved(static_cast<std::uint64_t>(range.first)); }),
            image.text_regions.end());
        for (auto& runtimeImage : replacement.addedImages) {
            image.runtime_local_images.push_back(std::move(runtimeImage));
        }
        for (const auto& range : replacement.addedTextRegions) image.text_regions.push_back(range);
        dynamicCodeRegionStarts.swap(replacement.starts);
        processCodeCacheInvalidated = processCodeCacheInvalidated || replacement.changed;
    };
    const auto appendVmRegionToResident = [&](std::vector<std::uint64_t>& table,
                                               const VmRegion& region) {
        if (table[0] >= residentCapacity) {
            throw std::runtime_error("Guest resident map capacity exceeded by VM mapping");
        }
        const auto row = 1 + 4 * static_cast<std::size_t>(table[0]);
        table[row] = region.begin;
        table[row + 1] = region.end;
        table[row + 2] = region.backing;
        table[row + 3] = (region.writable ? 1U : 0U) | (region.readable ? 0U : 2U);
        table[0] += 1;
    };
    const auto publishVmRegion = [&](const VmRegion& region) {
        if (resident[0] >= residentCapacity) {
            throw std::runtime_error("Guest resident map capacity exceeded by VM mapping");
        }
        guestMemory.add_mapping({region.begin, region.end, region.backing,
                                 region.writable, region.readable});
        appendVmRegionToResident(resident, region);
    };
    const auto removeVmRegionFromResident = [&](std::vector<std::uint64_t>& table,
                                                 const VmRegion& region) {
        for (std::size_t index = 0; index < table[0]; ++index) {
            const auto row = 1 + 4 * index;
            if (table[row] != region.begin || table[row + 1] != region.end ||
                table[row + 2] != region.backing) continue;
            const auto last = 1 + 4 * (static_cast<std::size_t>(table[0]) - 1);
            for (std::size_t column = 0; column < 4; ++column) {
                table[row + column] = table[last + column];
                table[last + column] = 0;
            }
            table[0] -= 1;
            return;
        }
        throw std::runtime_error("Missing guest VM resident row");
    };
    const auto syncResidentTable = [&]() {
        std::memcpy(residentBuffer.data(), resident.data(), resident.size() * sizeof(std::uint64_t));
    };
    const auto setObjCCacheWrites = [&](bool enabled) {
        if (!image.cache_reader) return;
        if (enabled) {
            for (std::size_t index = 0; index < resident[0]; ++index) {
                const auto row = 1 + 4 * index;
                if ((resident[row + 3] & 2U) != 0 || (resident[row + 3] & 1U) != 0) continue;
                const auto begin = static_cast<std::int64_t>(resident[row]);
                const auto end = static_cast<std::int64_t>(resident[row + 1]);
                if (!image.cache_reader->isCacheDataConst(begin, end)) continue;
                objcWritableBackings.insert(resident[row + 2]);
                resident[row + 3] |= 1U;
                guestMemory.set_mapping_permissions(resident[row], true, true);
            }
        }
        for (std::size_t index = 0; index < resident[0]; ++index) {
            const auto row = 1 + 4 * index;
            if (objcWritableBackings.find(resident[row + 2]) == objcWritableBackings.end()) continue;
            if (enabled) resident[row + 3] |= 1U;
            else resident[row + 3] &= ~std::uint64_t{1};
            guestMemory.set_mapping_permissions(resident[row], true, enabled);
        }
        if (!enabled) objcWritableBackings.clear();
        syncResidentTable();
    };
    const auto discardThreadCallbacks = [&](std::uint64_t threadId) {
        const auto found = callbackFramesByThread.find(threadId);
        if (found == callbackFramesByThread.end()) return;
        bool releasedMutation = false;
        for (const auto& frame : found->second) {
            releasedMutation = releasedMutation || frame.mutationBlock != 0;
            if (frame.stopAddress && hostServiceState.heap) {
                hostServiceState.heap->release(static_cast<std::int64_t>(*frame.stopAddress));
            }
        }
        callbackFramesByThread.erase(found);
        if (!releasedMutation) return;
        const bool mutationStillOwned = std::any_of(callbackFramesByThread.begin(),
            callbackFramesByThread.end(), [](const auto& entry) {
                return std::any_of(entry.second.begin(), entry.second.end(),
                    [](const GuestCallbackFrame& frame) { return frame.mutationBlock != 0; });
            });
        if (!mutationStillOwned && !objcWritableBackings.empty()) setObjCCacheWrites(false);
    };
    const auto publishCacheMetadata = [&](const SharedCache& cache, std::int64_t address,
                                           std::size_t length, bool writable) {
        if (address < SharedCache::guestBase || length == 0 ||
            length > static_cast<std::uint64_t>(SharedCache::guestLimit - address)) {
            throw std::runtime_error("Invalid guest cache metadata range");
        }
        const auto end = static_cast<std::uint64_t>(address) + length;
        for (std::size_t index = 0; index < resident[0]; ++index) {
            const auto row = 1 + 4 * index;
            const auto begin = resident[row];
            const auto currentEnd = resident[row + 1];
            const auto flags = resident[row + 3];
            const bool readable = (flags & 2U) == 0;
            const bool currentWritable = (flags & 1U) != 0;
            if (readable && currentWritable == writable &&
                static_cast<std::uint64_t>(address) >= begin && end <= currentEnd) return;
            if (static_cast<std::uint64_t>(address) < currentEnd && begin < end) {
                throw std::runtime_error("Cannot map overlapping guest cache metadata");
            }
        }
        if (resident[0] >= residentCapacity) {
            throw std::runtime_error("Guest resident map capacity exceeded by cache metadata");
        }
        const auto allocated = mappingHeap.allocate(length, 16);
        if (!allocated) throw std::runtime_error("Guest cache metadata backing is exhausted");
        const auto backing = static_cast<std::size_t>(*allocated);
        if (backing > memoryBuffer.size() || length > memoryBuffer.size() - backing) {
            mappingHeap.release(*allocated);
            throw std::runtime_error("Guest cache metadata exceeds process memory");
        }
        const auto bytes = cache.bytes(cache.originalAddress(address), length);
        std::copy(bytes.begin(), bytes.end(),
                  static_cast<std::uint8_t*>(memoryBuffer.data()) + backing);
        publishVmRegion({static_cast<std::uint64_t>(address), end, backing, true, writable, false});
        syncResidentTable();
    };
    constexpr std::uint64_t guestMappingBase = std::uint64_t{1} << 45U;
    constexpr std::uint32_t kernSuccess = 0;
    constexpr std::uint32_t kernInvalidAddress = 1;
    constexpr std::uint32_t kernProtectionFailure = 2;
    constexpr std::uint32_t kernNoSpace = 3;
    constexpr std::uint32_t kernInvalidArgument = 4;
    constexpr std::uint32_t machSendInvalidDest = 0x10000003U;
    std::optional<std::size_t> kdebugTypeFilterBacking;
    std::uint64_t kdebugTypeFilterLogicalCursor = guestMappingBase + (std::uint64_t{1} << 32U);
    hostServiceState.guestMapJitEligible = image.main_code_policy.map_jit_eligible;
    hostServiceState.machVmAllocate = [&](std::uint64_t size, std::uint64_t alignment,
                                           std::uint32_t currentProtection,
                                           std::uint32_t maximumProtection,
                                           std::uint64_t& guestAddress, bool jitMapping) {
        if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 ||
            alignment > 16U * 1024U * 1024U ||
            size > std::numeric_limits<std::uint64_t>::max() - guestMappingBase) {
            return kernInvalidArgument;
        }
        const auto allocated = mappingHeap.allocate(size, static_cast<std::int64_t>(alignment));
        if (!allocated || resident[0] >= residentCapacity) {
            if (allocated) mappingHeap.release(*allocated);
            return kernNoSpace;
        }
        const auto backing = static_cast<std::size_t>(*allocated);
        const auto address = guestMappingBase + static_cast<std::uint64_t>(backing);
        if (size > image.mapping_backing_end() - backing ||
            address > std::numeric_limits<std::uint64_t>::max() - size) {
            mappingHeap.release(*allocated);
            return kernNoSpace;
        }
        std::memset(static_cast<std::uint8_t*>(memoryBuffer.data()) + backing, 0,
                    static_cast<std::size_t>(size));
        VmRegion region{address, address + size, backing,
                        (currentProtection & 1U) != 0,
                        (currentProtection & 2U) != 0,
                        (currentProtection & 4U) != 0,
                        maximumProtection, jitMapping};
        VmAllocation allocation{address, address + size, backing, {region}};
        try {
            auto stagedAllocations = vmAllocations;
            const auto [position, inserted] = stagedAllocations.emplace(
                address, std::move(allocation));
            (void)position;
            if (!inserted) throw std::runtime_error("Duplicate guest VM allocation address");
            auto stagedResident = resident;
            appendVmRegionToResident(stagedResident, region);
            auto stagedCodeRegions = prepareDynamicCodeRegions(address, address + size, {region});
            guestMemory.replace_mappings({}, {{region.begin, region.end, region.backing,
                                               region.writable, region.readable}});
            vmAllocations.swap(stagedAllocations);
            resident.swap(stagedResident);
            commitDynamicCodeRegions(std::move(stagedCodeRegions));
        } catch (...) {
            mappingHeap.release(*allocated);
            throw;
        }
        syncResidentTable();
        guestAddress = address;
        return kernSuccess;
    };
    hostServiceState.machVmDeallocate = [&](std::uint64_t address, std::uint64_t size) {
        const auto pageSize = bootstrap.pageSize;
        if (size == 0 || pageSize == 0 || (pageSize & (pageSize - 1)) != 0 ||
            address > std::numeric_limits<std::uint64_t>::max() - size) {
            return kernInvalidArgument;
        }
        const auto requestedEnd = address + size;
        if (requestedEnd > std::numeric_limits<std::uint64_t>::max() - (pageSize - 1)) {
            return kernInvalidArgument;
        }
        const auto begin = address & ~(pageSize - 1);
        const auto end = (requestedEnd + pageSize - 1) & ~(pageSize - 1);
        if (image.mapping_backing_end() >
            std::numeric_limits<std::uint64_t>::max() - guestMappingBase) {
            return kernInvalidArgument;
        }
        const auto arenaEnd = guestMappingBase + image.mapping_backing_end();
        if (begin < guestMappingBase || end > arenaEnd) return kernInvalidArgument;
        std::map<std::uint64_t, VmAllocation> stagedAllocations;
        std::vector<std::uint64_t> stagedResident;
        std::vector<std::uint64_t> removedBegins;
        std::vector<GuestMemoryMapping> addedMappings;
        std::vector<std::int64_t> releasedBackings;
        std::vector<VmRegion> stagedCodeRegionsInput;
        auto dynamicBegin = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t dynamicEnd = 0;
        try {
            stagedAllocations = vmAllocations;
            stagedResident = resident;
            for (const auto& [allocationBegin, allocation] : vmAllocations) {
                if (allocation.end <= begin || allocation.begin >= end) continue;
                const auto mappedBegin = std::max(begin, allocation.begin);
                const auto mappedEnd = std::min(end, allocation.end);
                std::vector<VmRegion> replacement;
                for (const auto& region : allocation.regions) {
                    if (region.end <= mappedBegin || region.begin >= mappedEnd) {
                        replacement.push_back(region);
                        continue;
                    }
                    if (region.begin < mappedBegin) {
                        replacement.push_back({region.begin, mappedBegin, region.backing,
                            region.readable, region.writable, region.executable,
                            region.maximumProtection, region.jit});
                    }
                    if (region.end > mappedEnd) {
                        replacement.push_back({mappedEnd, region.end,
                            region.backing + static_cast<std::size_t>(mappedEnd - region.begin),
                            region.readable, region.writable, region.executable,
                            region.maximumProtection, region.jit});
                    }
                }

                for (const auto& region : allocation.regions) {
                    removedBegins.push_back(region.begin);
                    removeVmRegionFromResident(stagedResident, region);
                }
                for (const auto& region : replacement) {
                    appendVmRegionToResident(stagedResident, region);
                    addedMappings.push_back({region.begin, region.end, region.backing,
                                             region.writable, region.readable});
                    stagedCodeRegionsInput.push_back(region);
                }

                dynamicBegin = std::min(dynamicBegin, allocation.begin);
                dynamicEnd = std::max(dynamicEnd, allocation.end);
                auto stagedAllocation = stagedAllocations.find(allocationBegin);
                if (replacement.empty()) {
                    releasedBackings.push_back(
                        static_cast<std::int64_t>(allocation.backing));
                    stagedAllocations.erase(stagedAllocation);
                } else {
                    stagedAllocation->second.regions = std::move(replacement);
                }
            }
            if (removedBegins.empty()) return kernSuccess;
            DynamicCodeReplacement stagedCodeRegions = prepareDynamicCodeRegions(
                dynamicBegin, dynamicEnd, stagedCodeRegionsInput);
            guestMemory.replace_mappings(removedBegins, addedMappings);
            vmAllocations.swap(stagedAllocations);
            resident.swap(stagedResident);
            commitDynamicCodeRegions(std::move(stagedCodeRegions));
        } catch (...) {
            return kernNoSpace;
        }
        for (const auto backing : releasedBackings) mappingHeap.release(backing);
        syncResidentTable();
        return kernSuccess;
    };
    hostServiceState.machVmProtect = [&](std::uint64_t address, std::uint64_t size,
                                          bool setMaximum, std::uint32_t protection) {
        if (size == 0) return kernSuccess;
        const auto pageSize = bootstrap.pageSize;
        if (pageSize == 0 || (pageSize & (pageSize - 1)) != 0 ||
            address > std::numeric_limits<std::uint64_t>::max() - size) {
            return kernInvalidArgument;
        }
        const auto requestedEnd = address + size;
        if (requestedEnd > std::numeric_limits<std::uint64_t>::max() - (pageSize - 1)) {
            return kernInvalidArgument;
        }
        const auto begin = address & ~(pageSize - 1);
        const auto end = (requestedEnd + pageSize - 1) & ~(pageSize - 1);
        if ((protection & ~7U) != 0) return kernInvalidArgument;
        auto allocation = std::find_if(vmAllocations.begin(), vmAllocations.end(),
            [begin, end](const auto& item) {
                return begin >= item.second.begin && end <= item.second.end;
            });
        if (allocation == vmAllocations.end()) return kernInvalidAddress;
        auto& current = allocation->second;
        std::uint64_t cursor = begin;
        for (const auto& region : current.regions) {
            if (region.end <= cursor || region.begin >= end) continue;
            if (region.begin > cursor) return kernInvalidAddress;
            if ((protection & ~region.maximumProtection) != 0) return kernProtectionFailure;
            const auto effective = (region.readable ? 1U : 0U) |
                (region.writable ? 2U : 0U) | (region.executable ? 4U : 0U);
            if (region.jit && (setMaximum || protection != effective)) {
                return kernProtectionFailure;
            }
            cursor = std::min(end, region.end);
            if (cursor == end) break;
        }
        if (cursor != end) return kernInvalidAddress;
        std::vector<VmRegion> replacement;
        for (const auto& region : current.regions) {
            if (region.end <= begin || region.begin >= end) {
                replacement.push_back(region);
                continue;
            }
            const auto middleBegin = std::max(region.begin, begin);
            const auto middleEnd = std::min(region.end, end);
            if (region.begin < middleBegin) {
                replacement.push_back({region.begin, middleBegin, region.backing,
                                       region.readable, region.writable, region.executable,
                            region.maximumProtection, region.jit});
            }
            const auto maximumProtection = setMaximum ? protection : region.maximumProtection;
            const auto currentProtection = setMaximum
                ? ((region.readable ? 1U : 0U) | (region.writable ? 2U : 0U) |
                   (region.executable ? 4U : 0U)) & protection
                : protection;
            replacement.push_back({middleBegin, middleEnd,
                region.backing + static_cast<std::size_t>(middleBegin - region.begin),
                (currentProtection & 1U) != 0, (currentProtection & 2U) != 0,
                (currentProtection & 4U) != 0, maximumProtection, region.jit});
            if (middleEnd < region.end) {
                replacement.push_back({middleEnd, region.end,
                    region.backing + static_cast<std::size_t>(middleEnd - region.begin),
                    region.readable, region.writable, region.executable,
                    region.maximumProtection, region.jit});
            }
        }
        if (resident[0] < current.regions.size() ||
            resident[0] - current.regions.size() + replacement.size() > residentCapacity) {
            return kernNoSpace;
        }
        const auto oldRegions = current.regions;
        try {
            auto stagedResident = resident;
            std::vector<std::uint64_t> removedBegins;
            std::vector<GuestMemoryMapping> addedMappings;
            removedBegins.reserve(oldRegions.size());
            addedMappings.reserve(replacement.size());
            for (const auto& region : oldRegions) {
                removedBegins.push_back(region.begin);
                removeVmRegionFromResident(stagedResident, region);
            }
            for (const auto& region : replacement) {
                appendVmRegionToResident(stagedResident, region);
                addedMappings.push_back({region.begin, region.end, region.backing,
                                         region.writable, region.readable});
            }
            auto stagedCodeRegions = prepareDynamicCodeRegions(
                current.begin, current.end, replacement);
            guestMemory.replace_mappings(removedBegins, addedMappings);
            resident.swap(stagedResident);
            current.regions.swap(replacement);
            commitDynamicCodeRegions(std::move(stagedCodeRegions));
        } catch (...) {
            return kernNoSpace;
        }
        syncResidentTable();
        return kernSuccess;
    };
    bool activeJitWriteProtection = true;
    const auto applyJitWriteProtection = [&](bool enabled) {
        auto& thread = guestScheduler.current();
        if (activeJitWriteProtection == enabled) {
            thread.jitWriteProtect = enabled;
            hostServiceState.jitWriteProtected = enabled;
            return true;
        }
        try {
            auto stagedAllocations = vmAllocations;
            auto stagedResident = resident;
            std::vector<std::uint64_t> removedBegins;
            std::vector<GuestMemoryMapping> addedMappings;
            std::vector<VmRegion> stagedCodeInputs;
            bool changed = false;
            for (auto& [unused, allocation] : stagedAllocations) {
                (void)unused;
                for (auto& region : allocation.regions) {
                    if (!region.jit) continue;
                    const bool writable = !enabled &&
                        (region.maximumProtection & 2U) != 0;
                    const bool executable = enabled &&
                        (region.maximumProtection & 4U) != 0;
                    if (region.writable == writable && region.executable == executable) continue;
                    VmRegion replacement = region;
                    replacement.writable = writable;
                    replacement.executable = executable;
                    removedBegins.push_back(region.begin);
                    removeVmRegionFromResident(stagedResident, region);
                    appendVmRegionToResident(stagedResident, replacement);
                    addedMappings.push_back({replacement.begin, replacement.end,
                        replacement.backing, replacement.writable, replacement.readable});
                    region = replacement;
                    changed = true;
                }
            }
            if (changed) {
                for (const auto& [unused, allocation] : stagedAllocations) {
                    (void)unused;
                    stagedCodeInputs.insert(stagedCodeInputs.end(),
                                            allocation.regions.begin(), allocation.regions.end());
                }
                auto stagedCodeRegions = prepareDynamicCodeRegions(
                    guestMappingBase, guestMappingBase + image.mapping_backing_end(),
                    stagedCodeInputs);
                guestMemory.replace_mappings(removedBegins, addedMappings);
                vmAllocations.swap(stagedAllocations);
                resident.swap(stagedResident);
                commitDynamicCodeRegions(std::move(stagedCodeRegions));
                syncResidentTable();
            }
        } catch (...) {
            return false;
        }
        thread.jitWriteProtect = enabled;
        hostServiceState.jitWriteProtected = enabled;
        activeJitWriteProtection = enabled;
        return true;
    };
    hostServiceState.setJitWriteProtection = [&](bool enabled) {
        return applyJitWriteProtection(enabled);
    };
    guestMemory.set_mapping_resolver([&](std::uint64_t address, std::uint64_t length, bool) {
        return makeCacheResident(address, length);
    });
    const auto resumeSignalFrame = [&]() {
        std::vector<std::uint64_t> live(
            static_cast<const std::uint64_t*>(stateBuffer.data()),
            static_cast<const std::uint64_t*>(stateBuffer.data()) +
                stateBuffer.size() / sizeof(std::uint64_t));
        const auto resumed = signalRuntime.resume(hostServiceState.signals,
            hostServiceState.threadSignalMask, live, live[37], {}, &guestMemory);
        if (!resumed) return false;
        std::memcpy(stateBuffer.data(), live.data(), live.size() * sizeof(std::uint64_t));
        const auto entry = resumed->recompile
            ? static_cast<std::int64_t>(resumed->programCounter)
            : static_cast<std::int64_t>(resumed->executionContext);
        selectKernel(entry);
        return true;
    };
    const auto deliverPendingSignal = [&]() -> std::optional<std::int32_t> {
        hostServiceState.ingest_host_signals();
        std::vector<std::uint64_t> live(
            static_cast<const std::uint64_t*>(stateBuffer.data()),
            static_cast<const std::uint64_t*>(stateBuffer.data()) +
                stateBuffer.size() / sizeof(std::uint64_t));
        const auto delivery = signalRuntime.deliver(
            hostServiceState.signals, hostServiceState.queuedProcessSignals,
            hostServiceState.threadPendingSignals, hostServiceState.threadSignalMask,
            live, live[37], static_cast<std::uint64_t>(activeEntry), &guestMemory);
        if (delivery.outcome == GuestSignalOutcome::ProcessExit) return delivery.exitStatus;
        if (delivery.outcome == GuestSignalOutcome::UnsupportedStop) {
            throw std::runtime_error("Guest stop and continue signals are unsupported");
        }
        if (delivery.outcome == GuestSignalOutcome::HandlerEntered) {
            std::memcpy(stateBuffer.data(), live.data(), live.size() * sizeof(std::uint64_t));
            selectKernel(static_cast<std::int64_t>(delivery.handler));
        }
        return std::nullopt;
    };
    const auto beginExitDrain = [&](std::uint64_t dso,
                                    std::optional<std::int32_t> exitStatus) {
        const auto handler = exitRegistry.take_next(dso);
        if (!handler) return false;
        auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
        GuestCallbackFrame frame;
        frame.savedRegisters.assign(live, live + 36);
        frame.savedRegisters[34] = 0;
        frame.savedVectorAndAuth.assign(live + 806, live + guestTokenCount);
        frame.authenticationDepth = live[37];
        frame.exitDso = dso;
        frame.exitStatus = exitStatus;
        frame.exitThread = guestScheduler.current_id();
        callbackFramesByThread[guestScheduler.current_id()].push_back(std::move(frame));
        live[0] = handler->argument;
        live[30] = kNoFault;
        live[33] = handler->target;
        selectKernel(static_cast<std::int64_t>(handler->target));
        return true;
    };
    std::uint64_t dispatchSequence = 0;
    while (true) {
        if (!applyJitWriteProtection(guestScheduler.current().jitWriteProtect)) {
            throw std::runtime_error("Guest thread MAP_JIT mode could not be applied");
        }
        if (const auto signalExit = deliverPendingSignal()) return *signalExit;
        auto& callbackFrames = callbackFramesByThread[guestScheduler.current_id()];
        auto* registers = static_cast<std::uint64_t*>(stateBuffer.data());
        if (processCodeCacheInvalidated) {
            processCodeCacheInvalidated = false;
            const auto continuation = registers[33];
            kernels.clear();
            decodeErrors.clear();
            activeKernel = nullptr;
            if (continuation != kNoFault) {
                if (continuation > static_cast<std::uint64_t>(
                                       std::numeric_limits<std::int64_t>::max())) {
                    throw std::runtime_error("Guest continuation exceeds translated code range");
                }
                selectKernel(static_cast<std::int64_t>(continuation));
            }
        }
        const auto& currentThread = guestScheduler.current();
        registers[guestThreadPointer] = static_cast<std::uint64_t>(currentThread.tsdAddress);
        bootstrap.errnoSlot = static_cast<std::uint64_t>(currentThread.errnoAddress);
        writableTable[0] = bootstrap.errnoSlot;
        std::memcpy(writableBuffer.data(), writableTable.data(),
                    writableTable.size() * sizeof(std::uint64_t));
        registers[36] = options.dispatchBlocks;
        registers[guestFaultAddress] = kNoFault;
        registers[guestCacheFaultAddress] = kNoFault;
        registers[guestCacheFaultWidth] = 0;
        // A guest callback may tail-call a host service. Its sentinel return
        // must complete the callback without compiling the sentinel as code.
        if (registers[33] == kNoFault) {
            registers[34] = 0;
        } else if (progressTrace.enabled()) {
            ++dispatchSequence;
            const bool reportDispatch = dispatchSequence <= 16 || dispatchSequence % 32 == 0;
            const auto dispatchStarted = std::chrono::steady_clock::now();
            if (reportDispatch) {
                progressTrace.event("dispatch-start", registers[33], dispatchSequence);
            }
            if (activeKernel->usesInterpreter) {
                progressTrace.event("interpreter-dispatch", registers[33],
                                    activeKernel->interpretedPackets.size());
                (void)interpreter->dispatch(backend, activeKernel->interpretedPackets,
                                            stateBuffer, &interpreterMemoryAccess);
            } else {
                (void)backend.dispatch(activeKernel->translated, bindings, one, one);
            }
            if (reportDispatch) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - dispatchStarted).count();
                progressTrace.event("dispatch-done", registers[33],
                                    static_cast<std::uint64_t>(elapsed));
            }
        } else if (activeKernel->usesInterpreter) {
            (void)interpreter->dispatch(backend, activeKernel->interpretedPackets,
                                        stateBuffer, &interpreterMemoryAccess);
        } else {
            (void)backend.dispatch(activeKernel->translated, bindings, one, one);
        }
        registers = static_cast<std::uint64_t*>(stateBuffer.data());
        const auto status = registers[34];
        if (status == 0) {
            if (signalRuntime.has_frames() && resumeSignalFrame()) continue;
            if (!callbackFrames.empty() && registers[30] == kNoFault &&
                (!callbackFrames.back().exitDso ||
                 callbackFrames.back().exitThread == guestScheduler.current_id())) {
                auto frame = std::move(callbackFrames.back());
                callbackFrames.pop_back();
                if (registers[37] != frame.authenticationDepth) {
                    throw std::runtime_error("Guest image callback left authentication stack unbalanced");
                }
                if (frame.exitDso) {
                    const auto handler = exitRegistry.take_next(*frame.exitDso);
                    if (handler) {
                        auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
                        std::copy(frame.savedRegisters.begin(), frame.savedRegisters.end(), live);
                        std::copy(frame.savedVectorAndAuth.begin(), frame.savedVectorAndAuth.end(), live + 806);
                        live[37] = frame.authenticationDepth;
                        live[0] = handler->argument;
                        live[30] = kNoFault;
                        live[33] = handler->target;
                        callbackFrames.push_back(std::move(frame));
                        selectKernel(static_cast<std::int64_t>(handler->target));
                        continue;
                    }
                    if (frame.exitStatus) return *frame.exitStatus;
                }
                if (frame.nextInitializer < frame.initializerTargets.size()) {
                    const auto target = frame.initializerTargets[frame.nextInitializer++];
                    callbackFrames.push_back(std::move(frame));
                    auto* callbackRegisters = static_cast<std::uint64_t*>(stateBuffer.data());
                    const auto& pending = callbackFrames.back();
                    std::copy(pending.savedRegisters.begin(), pending.savedRegisters.end(),
                              callbackRegisters);
                    std::copy(pending.savedVectorAndAuth.begin(),
                              pending.savedVectorAndAuth.end(), callbackRegisters + 806);
                    callbackRegisters[37] = pending.authenticationDepth;
                    std::copy_n(bootstrap.registers.begin(), 4, callbackRegisters);
                    callbackRegisters[30] = kNoFault;
                    callbackRegisters[33] = target;
                    selectKernel(static_cast<std::int64_t>(target));
                    continue;
                }
                if (frame.stopAddress &&
                    *guestMemory.buffer(*frame.stopAddress, 1) != 0) {
                    frame.nextArgument = frame.arguments.size();
                }
                if (frame.nextArgument < frame.arguments.size()) {
                    const auto args = frame.arguments[frame.nextArgument++];
                    callbackFrames.push_back(std::move(frame));
                    auto* callbackRegisters = static_cast<std::uint64_t*>(stateBuffer.data());
                    std::copy(callbackFrames.back().savedRegisters.begin(),
                              callbackFrames.back().savedRegisters.end(), callbackRegisters);
                    std::copy(callbackFrames.back().savedVectorAndAuth.begin(),
                              callbackFrames.back().savedVectorAndAuth.end(), callbackRegisters + 806);
                    callbackRegisters[37] = callbackFrames.back().authenticationDepth;
                    std::copy(args.begin(), args.end(), callbackRegisters);
                    callbackRegisters[30] = kNoFault;
                    callbackRegisters[33] = callbackFrames.back().callback;
                    selectKernel(static_cast<std::int64_t>(callbackFrames.back().callback));
                    continue;
                }
                const bool outerMutation = std::any_of(callbackFramesByThread.begin(),
                    callbackFramesByThread.end(), [](const auto& entry) {
                        return std::any_of(entry.second.begin(), entry.second.end(),
                            [](const GuestCallbackFrame& candidate) {
                                return candidate.mutationBlock != 0;
                            });
                    });
                if (frame.mutationBlock != 0 && !outerMutation &&
                    !objcWritableBackings.empty()) {
                    setObjCCacheWrites(false);
                }
                if (frame.stopAddress && hostServiceState.heap) {
                    hostServiceState.heap->release(static_cast<std::int64_t>(*frame.stopAddress));
                }
                auto* restored = static_cast<std::uint64_t*>(stateBuffer.data());
                std::copy(frame.savedRegisters.begin(), frame.savedRegisters.end(), restored);
                std::copy(frame.savedVectorAndAuth.begin(), frame.savedVectorAndAuth.end(),
                          restored + 806);
                restored[37] = frame.authenticationDepth;
                if (restored[33] != kNoFault) {
                    selectKernel(static_cast<std::int64_t>(restored[33]));
                }
                continue;
            }
            if (registers[30] == kNoFault && guestScheduler.current_id() != 1) {
                discardThreadCallbacks(guestScheduler.current_id());
                std::vector<std::uint64_t> live(
                    static_cast<const std::uint64_t*>(stateBuffer.data()),
                    static_cast<const std::uint64_t*>(stateBuffer.data()) +
                        stateBuffer.size() / sizeof(std::uint64_t));
                if (finishGuestThread(live[0], live)) return 0;
                continue;
            }
            if (advanceStartup()) continue;
            const auto exitStatus = static_cast<std::int32_t>(registers[0]);
            if (beginExitDrain(0, exitStatus)) continue;
            return exitStatus;
        }
        if (status == 1 || status == 8) continue;
        if (status == 4) {
            std::vector<std::uint64_t> hostRegisters(
                static_cast<const std::uint64_t*>(stateBuffer.data()),
                static_cast<const std::uint64_t*>(stateBuffer.data()) +
                    stateBuffer.size() / sizeof(std::uint64_t));
            const auto service = registers[35];
            progressTrace.event("host-service", hostRegisters[33], service);
            if (service == 166 || service == 167) {
                auto target = hostRegisters[0];
                if ((target & 0xffff000000000000ULL) == 0xa64e000000000000ULL) {
                    const auto authenticated = decode_guest_ia0_callback(
                        hostRegisters.data(), hostRegisters.size(), target);
                    if (!authenticated)
                        throw std::runtime_error("Guest exit callback authentication failed");
                    target = *authenticated;
                } else {
                    const auto resume = hostRegisters[33];
                    const auto callerAbi = resume >= 4 && resume - 4 <=
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                        ? image.is_arm64e_code(static_cast<std::int64_t>(resume - 4))
                        : std::nullopt;
                    if (!callerAbi || *callerAbi)
                        throw std::runtime_error("Guest exit callback requires an authenticated IA/0 pointer");
                }
                if (target > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                    !image.is_code(static_cast<std::int64_t>(target)))
                    throw std::runtime_error("Guest exit callback target is not executable");
                std::uint64_t dso = service == 167 ? hostRegisters[2] : 0;
                if (service == 166) {
                    const auto owner = std::find_if(image.mapped_image_segments.begin(),
                        image.mapped_image_segments.end(), [target](const MappedImageSegment& segment) {
                            return segment.start >= 0 && segment.end > segment.start &&
                                target >= static_cast<std::uint64_t>(segment.start) &&
                                target < static_cast<std::uint64_t>(segment.end);
                        });
                    if (owner != image.mapped_image_segments.end()) dso = owner->guest_header;
                }
                const GuestExitHandler handler{target,
                    service == 167 ? hostRegisters[1] : 0,
                    dso,
                    service == 167 ? GuestExitHandlerKind::Cxa : GuestExitHandlerKind::Standard};
                registers[0] = exitRegistry.register_handler(handler) ? 0 :
                    static_cast<std::uint64_t>(-1);
                continue;
            }
            if (service == 168) {
                const auto dso = hostRegisters[0];
                registers[0] = 0;
                (void)beginExitDrain(dso, std::nullopt);
                continue;
            }
            if (service == 169) {
                const auto exitStatus = static_cast<std::int32_t>(hostRegisters[0]);
                if (beginExitDrain(0, exitStatus)) continue;
                return exitStatus;
            }
            if (service == 141) {
                registers[0] = signing.restricted ? 1U : 0U;
                continue;
            }
            if (service == 176) {
                // This runtime does not implement DYLD_INSERT_LIBRARIES or
                // __interpose sections; ordinary guest dlopen is not insertion.
                registers[0] = 0;
                continue;
            }
            if (service == 143) {
                registers[0] = image.swift_prespecialized_data.value_or(0);
                continue;
            }
            if (service == 144) {
                const auto table = hostRegisters[0];
                hostRegisters[0] = 0;
                if (image.cache_reader && image.swift_prespecialized_data &&
                    hostRegisters[2] < 64) {
                    const auto base = static_cast<std::uint64_t>(SharedCache::guestBase);
                    const auto mappedSize = image.cache_reader->mappedSize();
                    if (mappedSize <= std::numeric_limits<std::uint64_t>::max() - base) {
                        const auto end = base + mappedSize;
                        const auto inCache = [end](std::uint64_t address) {
                            return address >= base && address < end;
                        };
                        if (inCache(table) && inCache(hostRegisters[1])) {
                            std::vector<std::uint64_t> keys{hostRegisters[1] - base};
                            const auto keyCount = static_cast<std::size_t>(hostRegisters[2]);
                            if (keyCount != 0) {
                                const auto bytes = keyCount * sizeof(std::uint64_t);
                                const auto* keyData = guestMemory.buffer(hostRegisters[3], bytes);
                                bool validKeys = true;
                                for (std::size_t index = 0; index < keyCount; ++index) {
                                    std::uint64_t key = 0;
                                    std::memcpy(&key, keyData + index * sizeof(key), sizeof(key));
                                    if (!inCache(key)) {
                                        validKeys = false;
                                        break;
                                    }
                                    keys.push_back(key - base);
                                }
                                if (!validKeys) keys.clear();
                            }
                            const auto& uuid = image.cache_reader->uuid();
                            if (!keys.empty() && uuid.size() == 16) {
                                std::uint64_t offset = 0;
                                const auto result = guest_cached_pointer_lookup(
                                    table - base, keys.data(),
                                    static_cast<std::uint32_t>(keys.size()), uuid.data(), &offset);
                                if (result < 0) {
                                    throw std::runtime_error(
                                        "Host and guest Swift pointer caches do not match");
                                }
                                if (result == 1) {
                                    if (offset >= mappedSize || offset >
                                            std::numeric_limits<std::uint64_t>::max() - base) {
                                        throw std::runtime_error(
                                            "Host pointer cache returned an out-of-range guest offset");
                                    }
                                    hostRegisters[0] = base + offset;
                                }
                            } else if (uuid.size() != 16) {
                                throw std::runtime_error("Guest shared-cache UUID has invalid size");
                            }
                        }
                    }
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 149) {
                const auto protocol = hostRegisters[0];
                const auto metadata = hostRegisters[1];
                const auto type = hostRegisters[2];
                hostRegisters[0] = 2;
                hostRegisters[1] = 0;
                if (image.cache_reader) {
                    const auto descriptor = image.cache_reader->swiftConformance(
                        protocol, metadata, type,
                        [&image, &activatedLazyHeaders](std::size_t index) {
                            const auto header = image.cache_reader->objcImageHeader(index);
                            if (!header) return false;
                            const auto guestHeader = static_cast<std::uint64_t>(*header);
                            const bool mapped = std::any_of(
                                image.mapped_image_segments.begin(),
                                image.mapped_image_segments.end(),
                                [guestHeader](const auto& segment) {
                                    return segment.guest_header == guestHeader;
                                });
                            if (!mapped) return false;
                            return image.startup_eager_headers.find(guestHeader) !=
                                       image.startup_eager_headers.end() ||
                                   activatedLazyHeaders.find(guestHeader) !=
                                       activatedLazyHeaders.end();
                        });
                    const bool descriptorMapped = descriptor && *descriptor >= 0 &&
                        std::any_of(image.mapped_image_segments.begin(),
                                    image.mapped_image_segments.end(),
                            [descriptor](const auto& segment) {
                                return segment.start >= 0 && segment.end > segment.start &&
                                       *descriptor >= segment.start &&
                                       *descriptor < segment.end;
                            });
                    if (descriptorMapped) {
                        hostRegisters[0] = 0;
                        hostRegisters[1] = static_cast<std::uint64_t>(*descriptor);
                    }
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 145) {
                const auto ownerImage = image.image_paths.find(hostRegisters[1]);
                const auto owner = ownerImage == image.image_paths.end()
                    ? std::string("unknown") : ownerImage->second;
                if (!image.cache_reader || hostRegisters[1] <
                        static_cast<std::uint64_t>(SharedCache::guestBase)) {
                    throw std::runtime_error(
                        "Unsupported guest lazy-load flag 0x" + hexadecimal(hostRegisters[0]) +
                        " in " + owner);
                }
                const auto dependency = image.cache_reader->preboundLazyDependency(
                    owner, hostRegisters[0]);
                if (!dependency) {
                    throw std::runtime_error(
                        "Unsupported guest lazy-load flag 0x" + hexadecimal(hostRegisters[0]) +
                        " in " + owner);
                }
                if (imageHeadersByPath.find(*dependency) == imageHeadersByPath.end()) {
                    throw std::runtime_error(
                        "Guest lazy dependency is not loaded: " + *dependency);
                }
                const auto declaredHeaders = image.cache_reader->declaredDependencyHeaders(
                    *dependency);
                for (const auto header : declaredHeaders) {
                    activatedLazyHeaders.insert(static_cast<std::uint64_t>(header));
                    loadedImageHeaders.insert(static_cast<std::uint64_t>(header));
                }
                auto* flag = guestMemory.buffer(hostRegisters[0], sizeof(std::uint32_t));
                const std::array<std::uint8_t, sizeof(std::uint32_t)> initialized{1, 0, 0, 0};
                std::memcpy(flag, initialized.data(), initialized.size());
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 162) {
                const auto name = guest_cstring(guestMemory, hostRegisters[1]);
                const auto handle = hostRegisters[0];
                if (imageHandles.is_closed_guest_handle(handle)) {
                    setGuestDlerror("dlsym: invalid closed handle");
                    hostRegisters[0] = 0;
                } else if (const auto resolved = resolveDlsym(handle, name)) {
                    clearGuestDlerror();
                    hostRegisters[0] = *resolved;
                } else {
                    setGuestDlerror("dlsym: symbol not found: " + name);
                    hostRegisters[0] = 0;
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 163) {
                hostRegisters[0] = takeGuestDlerror();
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 165) {
                const auto infoAddress = hostRegisters[1];
                if (infoAddress == 0 || infoAddress < bootstrap.pageSize) {
                    hostRegisters[0] = 0;
                    std::copy_n(hostRegisters.begin(), 36,
                                static_cast<std::uint64_t*>(stateBuffer.data()));
                    continue;
                }
                auto targetAddress = hostRegisters[0];
                if ((targetAddress & 0xffff000000000000ULL) == 0xa64e000000000000ULL) {
                    const auto authenticated = decode_guest_ia0_callback(
                        hostRegisters.data(), hostRegisters.size(), targetAddress);
                    if (!authenticated) {
                        hostRegisters[0] = 0;
                        std::copy_n(hostRegisters.begin(), 36,
                                    static_cast<std::uint64_t*>(stateBuffer.data()));
                        continue;
                    }
                    targetAddress = *authenticated;
                }
                const auto modeledAlias = std::find_if(
                    bootstrap.modeledGlobalAddresses.begin(),
                    bootstrap.modeledGlobalAddresses.end(),
                    [&image, targetAddress](const auto& entry) {
                        const auto provenance = image.modeled_global_export_provenance.find(
                            entry.first);
                        return entry.second == targetAddress &&
                               provenance != image.modeled_global_export_provenance.end();
                    });
                if (modeledAlias != bootstrap.modeledGlobalAddresses.end()) {
                    const auto provenance = image.modeled_global_export_provenance.find(
                        modeledAlias->first);
                    std::uint8_t* output = nullptr;
                    try {
                        output = guestMemory.buffer(infoAddress, sizeof(Dl_info), true);
                    } catch (const std::out_of_range&) {
                        hostRegisters[0] = 0;
                        std::copy_n(hostRegisters.begin(), 36,
                                    static_cast<std::uint64_t*>(stateBuffer.data()));
                        continue;
                    }
                    std::string symbolName = modeledAlias->first;
                    if (!symbolName.empty() && symbolName.front() == '_') {
                        symbolName.erase(symbolName.begin());
                    }
                    const auto fileName = stableGuestString(provenance->second.canonical_path);
                    const auto guestSymbol = stableGuestString(symbolName);
                    if (!fileName || !guestSymbol) {
                        hostRegisters[0] = 0;
                    } else {
                        Dl_info result{};
                        result.dli_fname = reinterpret_cast<const char*>(
                            static_cast<std::uintptr_t>(*fileName));
                        result.dli_fbase = reinterpret_cast<void*>(
                            static_cast<std::uintptr_t>(provenance->second.guest_header));
                        result.dli_sname = reinterpret_cast<const char*>(
                            static_cast<std::uintptr_t>(*guestSymbol));
                        result.dli_saddr = reinterpret_cast<void*>(
                            static_cast<std::uintptr_t>(targetAddress));
                        std::memcpy(output, &result, sizeof(result));
                        hostRegisters[0] = 1;
                    }
                    std::copy_n(hostRegisters.begin(), 36,
                                static_cast<std::uint64_t*>(stateBuffer.data()));
                    continue;
                }
                const auto owner = std::find_if(
                    image.mapped_image_segments.begin(), image.mapped_image_segments.end(),
                    [targetAddress](const MappedImageSegment& segment) {
                        return segment.start >= 0 && segment.end > segment.start &&
                               targetAddress >= static_cast<std::uint64_t>(segment.start) &&
                               targetAddress < static_cast<std::uint64_t>(segment.end);
                    });
                if (owner == image.mapped_image_segments.end()) {
                    hostRegisters[0] = 0;
                    std::copy_n(hostRegisters.begin(), 36,
                                static_cast<std::uint64_t*>(stateBuffer.data()));
                    continue;
                }
                std::uint8_t* output = nullptr;
                try {
                    output = guestMemory.buffer(infoAddress, sizeof(Dl_info), true);
                } catch (const std::out_of_range&) {
                    hostRegisters[0] = 0;
                    std::copy_n(hostRegisters.begin(), 36,
                                static_cast<std::uint64_t*>(stateBuffer.data()));
                    continue;
                }
                const auto& symbols = symbolsForSegment(*owner);
                const GuestDladdrSymbol* nearest = nullptr;
                GuestDladdrSymbol dsoHandleSymbol{"__dso_handle", owner->guest_header, false};
                if (targetAddress == owner->guest_header) {
                    nearest = &dsoHandleSymbol;
                } else {
                    for (const auto& symbol : symbols) {
                        if (symbol.address > targetAddress) break;
                        if (nearest == nullptr || symbol.address >= nearest->address) {
                            nearest = &symbol;
                        }
                    }
                }
                const auto fileName = stableGuestString(owner->image_path);
                const auto symbolName = nearest == nullptr
                    ? std::optional<std::uint64_t>{}
                    : stableGuestString(nearest->name);
                if (!fileName || (nearest != nullptr && !symbolName)) {
                    hostRegisters[0] = 0;
                } else {
                    Dl_info result{};
                    result.dli_fname = reinterpret_cast<const char*>(
                        static_cast<std::uintptr_t>(*fileName));
                    result.dli_fbase = reinterpret_cast<void*>(
                        static_cast<std::uintptr_t>(owner->guest_header));
                    if (nearest != nullptr) {
                        result.dli_sname = reinterpret_cast<const char*>(
                            static_cast<std::uintptr_t>(*symbolName));
                        result.dli_saddr = reinterpret_cast<void*>(
                            static_cast<std::uintptr_t>(nearest->address));
                    }
                    std::memcpy(output, &result, sizeof(result));
                    hostRegisters[0] = 1;
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 164) {
                const auto descriptorAddress = hostRegisters[0];
                const auto owner = std::find_if(
                    guestTlvDescriptors.begin(), guestTlvDescriptors.end(),
                    [descriptorAddress](const GuestTlvDescriptorOwner& candidate) {
                        return descriptorAddress >= candidate.begin &&
                               descriptorAddress <= candidate.end &&
                               candidate.end - descriptorAddress >= 24 &&
                               (descriptorAddress - candidate.begin) % 24 == 0;
                    });
                std::uint32_t key = 0;
                std::uint64_t descriptorOffset = 0;
                if (owner != guestTlvDescriptors.end()) {
                    const auto* descriptor = guestMemory.buffer(descriptorAddress, 24);
                    std::uint32_t descriptorKey = 0;
                    std::uint32_t offset = 0;
                    std::memcpy(&descriptorKey, descriptor + 8, sizeof(descriptorKey));
                    std::memcpy(&offset, descriptor + 12, sizeof(offset));
                    if (descriptorKey != owner->key || offset >= owner->templateSize) {
                        throw std::runtime_error(
                            "Guest TLV descriptor was modified or is out of bounds");
                    }
                    key = descriptorKey;
                    descriptorOffset = offset;
                } else {
                    const auto cachedOwner = std::find_if(
                        image.thread_local_images.begin(), image.thread_local_images.end(),
                        [descriptorAddress](const auto& entry) {
                            const auto& metadata = entry.second;
                            return metadata.from_shared_cache &&
                                std::any_of(metadata.thunk_ranges.begin(),
                                            metadata.thunk_ranges.end(),
                                    [descriptorAddress](const auto& range) {
                                        return descriptorAddress >= range.first &&
                                            descriptorAddress <= range.first + range.second &&
                                            range.first + range.second - descriptorAddress >= 24 &&
                                            (descriptorAddress - range.first) % 24 == 0;
                                    });
                        });
                    if (cachedOwner == image.thread_local_images.end()) {
                        throw std::runtime_error(
                            "Unsupported or unregistered guest TLV descriptor at 0x" +
                            hexadecimal(descriptorAddress));
                    }
                    const auto& metadata = cachedOwner->second;
                    if (!metadata.init_callback_ranges.empty()) {
                        throw std::runtime_error(
                            "Cached dynamic guest TLS initialization/destruction is unsupported");
                    }
                    if (metadata.template_size == 0 || metadata.template_alignment == 0 ||
                        (metadata.template_alignment & (metadata.template_alignment - 1)) != 0) {
                        throw std::runtime_error("Invalid cached guest TLV template geometry");
                    }
                    const auto* raw = guestMemory.buffer(descriptorAddress, 24);
                    std::vector<std::uint8_t> descriptor(raw, raw + 24);
                    GuestTlvOwnerGeometry geometry;
                    geometry.templateBegin = metadata.template_address;
                    geometry.templateSize = metadata.template_size;
                    geometry.allZeroFill = metadata.all_zero_fill;
                    for (const auto& [begin, size] : metadata.thunk_ranges) {
                        geometry.thunkRanges.push_back({begin, size});
                    }
                    const auto parsed = parse_guest_tlv_descriptor(
                        descriptor, descriptorAddress, GuestTlvDescriptorFormat::diskV1,
                        geometry);
                    const auto assigned = cachedV1TlvKeys.find(cachedOwner->first);
                    if (assigned == cachedV1TlvKeys.end()) {
                        throw std::runtime_error("Cached v1 guest TLV owner has no assigned key");
                    }
                    key = assigned->second;
                    descriptorOffset = parsed.offset;
                    if (registeredCachedTlvImages.find(cachedOwner->first) ==
                        registeredCachedTlvImages.end()) {
                        std::vector<std::uint8_t> bytes(
                            static_cast<std::size_t>(metadata.template_size), 0);
                        for (const auto& [address, size] : metadata.template_ranges) {
                            if (address < metadata.template_address ||
                                address - metadata.template_address > metadata.template_size ||
                                size > metadata.template_size -
                                           (address - metadata.template_address)) {
                                throw std::runtime_error(
                                    "Cached guest TLV section exceeds its combined span");
                            }
                            const auto* source = guestMemory.buffer(address, size);
                            std::copy_n(source, static_cast<std::size_t>(size),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(
                                            address - metadata.template_address));
                        }
                        guestTlvRuntime.register_image(key, std::move(bytes), 0,
                                                       metadata.template_alignment);
                        registeredCachedTlvImages.insert(cachedOwner->first);
                    }
                }
                const GuestTlvRuntime::Allocate allocateTlv =
                    [&](std::uint64_t size, std::uint64_t alignment)
                    -> std::optional<std::uint64_t> {
                    const auto allocation = allocateGuestHeap(size, alignment);
                    if (!allocation) return std::nullopt;
                    return static_cast<std::uint64_t>(*allocation);
                };
                const auto threadTsd = hostRegisters[guestThreadPointer];
                const auto address = guestTlvRuntime.resolve(
                    threadTsd, key, descriptorOffset, allocateTlv, guestTlvRelease,
                    guestTlvRead, guestTlvWrite);
                if (!address) throw std::runtime_error("Unable to allocate guest TLV storage");
                hostRegisters[0] = *address;
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 172) {
                const auto descriptorAddress = hostRegisters[0];
                const auto cachedOwner = std::find_if(
                    image.thread_local_images.begin(), image.thread_local_images.end(),
                    [descriptorAddress](const auto& entry) {
                        const auto& metadata = entry.second;
                        return metadata.from_shared_cache &&
                            std::any_of(metadata.thunk_ranges.begin(), metadata.thunk_ranges.end(),
                                [descriptorAddress](const auto& range) {
                                    return descriptorAddress >= range.first &&
                                        descriptorAddress <= range.first + range.second &&
                                        range.first + range.second - descriptorAddress >= 24 &&
                                        (descriptorAddress - range.first) % 24 == 0;
                                });
                    });
                if (cachedOwner == image.thread_local_images.end()) {
                    throw std::runtime_error("Cached TLV fast entry received an unknown descriptor");
                }
                const auto& metadata = cachedOwner->second;
                if (!metadata.init_callback_ranges.empty()) {
                    throw std::runtime_error(
                        "Cached dynamic guest TLS initialization/destruction is unsupported");
                }
                const auto* raw = guestMemory.buffer(descriptorAddress, 24);
                std::vector<std::uint8_t> descriptor(raw, raw + 24);
                GuestTlvOwnerGeometry geometry;
                geometry.templateBegin = metadata.template_address;
                geometry.templateSize = metadata.template_size;
                geometry.allZeroFill = metadata.all_zero_fill;
                for (const auto& [begin, size] : metadata.thunk_ranges) {
                    geometry.thunkRanges.push_back({begin, size});
                }
                const auto parsed = parse_guest_tlv_descriptor(
                    descriptor, descriptorAddress, GuestTlvDescriptorFormat::runtimeV2,
                    geometry);
                const auto staticOwner = cachedStaticTlvOwners.find(parsed.key);
                if (staticOwner == cachedStaticTlvOwners.end() ||
                    staticOwner->second != cachedOwner->first) {
                    throw std::runtime_error("Cached v2 guest TLV key conflicts with its owner");
                }
                if (registeredCachedTlvImages.find(cachedOwner->first) ==
                    registeredCachedTlvImages.end()) {
                    std::vector<std::uint8_t> bytes(
                        static_cast<std::size_t>(metadata.template_size), 0);
                    if (!metadata.all_zero_fill) {
                        for (const auto& [address, size] : metadata.template_ranges) {
                            if (address < metadata.template_address ||
                                address - metadata.template_address > metadata.template_size ||
                                size > metadata.template_size -
                                           (address - metadata.template_address)) {
                                throw std::runtime_error(
                                    "Cached guest TLV section exceeds its combined span");
                            }
                            const auto* source = guestMemory.buffer(address, size);
                            std::copy_n(source, static_cast<std::size_t>(size),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(
                                            address - metadata.template_address));
                        }
                    }
                    guestTlvRuntime.register_image(parsed.key, std::move(bytes), 0,
                                                   metadata.template_alignment);
                    registeredCachedTlvImages.insert(cachedOwner->first);
                }
                const GuestTlvRuntime::Allocate allocateTlv =
                    [&](std::uint64_t size, std::uint64_t alignment)
                    -> std::optional<std::uint64_t> {
                    const auto allocation = allocateGuestHeap(size, alignment);
                    if (!allocation) return std::nullopt;
                    return static_cast<std::uint64_t>(*allocation);
                };
                const auto address = guestTlvRuntime.resolve(
                    hostRegisters[guestThreadPointer], parsed.key, parsed.offset, allocateTlv,
                    guestTlvRelease, guestTlvRead, guestTlvWrite);
                if (!address) throw std::runtime_error("Unable to allocate cached guest TLV storage");
                hostRegisters[0] = *address;
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 52 && !signalRuntime.has_frames()) {
                timespec request{};
                const auto* requestBytes = guestMemory.buffer(
                    hostRegisters[0], sizeof(request));
                std::memcpy(&request, requestBytes, sizeof(request));
                if (hostRegisters[1] != 0) {
                    (void)guestMemory.buffer(hostRegisters[1], sizeof(timespec), true);
                }
                if (request.tv_sec < 0 || request.tv_nsec < 0 ||
                    request.tv_nsec >= 1'000'000'000L) {
                    const std::uint32_t invalid = EINVAL;
                    auto* error = guestMemory.buffer(bootstrap.errnoSlot, sizeof(invalid), true);
                    std::memcpy(error, &invalid, sizeof(invalid));
                    hostRegisters[0] = std::numeric_limits<std::uint32_t>::max();
                } else {
                    const auto now = monotonicNanoseconds();
                    const auto seconds = static_cast<std::uint64_t>(request.tv_sec);
                    const auto fraction = static_cast<std::uint64_t>(request.tv_nsec);
                    const auto requested = seconds >
                            (std::numeric_limits<std::uint64_t>::max() - fraction) /
                                1'000'000'000ULL
                        ? std::numeric_limits<std::uint64_t>::max()
                        : seconds * 1'000'000'000ULL + fraction;
                    const auto deadline = now + std::min(
                        requested, std::numeric_limits<std::uint64_t>::max() - now);
                    if (deadline <= now) {
                        hostRegisters[0] = 0;
                    } else {
                        if (!guestScheduler.sleep_current(
                                deadline, hostRegisters[1] == 0
                                    ? std::nullopt
                                    : std::optional<std::uint64_t>{hostRegisters[1]})) {
                            throw std::runtime_error("Current guest thread already has a wait reason");
                        }
                        hostRegisters[0] = 0;
                        if (!syncThreadSwitch(false, hostRegisters)) {
                            throw std::runtime_error("Guest nanosleep has no runnable thread");
                        }
                        continue;
                    }
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service >= 79 && service <= 85) {
                const auto address = hostRegisters[0];
                if (service == 79) {
                    (void)guestMemory.buffer(address, sizeof(pthread_cond_t), true);
                    if (hostRegisters[1] != 0) {
                        throw std::runtime_error("Guest condition attributes unsupported");
                    }
                    hostRegisters[0] = static_cast<std::uint32_t>(
                        guestScheduler.initialize_condition(address));
                } else if (!ensureCondition(address)) {
                    hostRegisters[0] = EINVAL;
                } else if (service == 80) {
                    const auto result = guestScheduler.destroy_condition(address);
                    if (result == 0) {
                        auto* bytes = guestMemory.buffer(address, sizeof(std::uint64_t), true);
                        std::memset(bytes, 0, sizeof(std::uint64_t));
                    }
                    hostRegisters[0] = static_cast<std::uint32_t>(result);
                } else if (service == 81 || service == 84 || service == 85) {
                    std::optional<std::uint64_t> deadline;
                    if (service != 81) {
                        timespec value{};
                        const auto* bytes = guestMemory.buffer(hostRegisters[2], sizeof(value));
                        std::memcpy(&value, bytes, sizeof(value));
                        deadline = conditionDeadline(value, service == 85);
                        if (!deadline) {
                            hostRegisters[0] = EINVAL;
                            std::copy_n(hostRegisters.begin(), 36,
                                static_cast<std::uint64_t*>(stateBuffer.data()));
                            continue;
                        }
                    }
                    if (!ensureMutex(hostRegisters[1])) {
                        hostRegisters[0] = EINVAL;
                    } else {
                        const auto waited = guestScheduler.wait_condition(
                            address, hostRegisters[1], deadline);
                        if (waited.waiting) {
                            hostRegisters[0] = 0;
                            if (!syncThreadSwitch(false, hostRegisters)) {
                                throw std::runtime_error(
                                    "Guest condition wait has no runnable thread");
                            }
                            continue;
                        }
                        hostRegisters[0] = static_cast<std::uint32_t>(waited.error);
                    }
                } else {
                    const auto result = guestScheduler.signal_condition(
                        address, service == 83);
                    hostRegisters[0] = static_cast<std::uint32_t>(result);
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 70) {
                const auto threadIdOutput = hostRegisters[0];
                auto* threadIdDestination = guestMemory.buffer(
                    threadIdOutput, sizeof(std::uint64_t), true);
                if (hostRegisters[1] != 0) {
                    throw std::runtime_error("Guest pthread creation attributes unsupported");
                }
                auto entry = hostRegisters[2];
                if ((entry & 0xffff000000000000ULL) == 0xa64e000000000000ULL) {
                    const auto authenticated = decode_guest_ia0_callback(
                        hostRegisters.data(), hostRegisters.size(), entry);
                    if (!authenticated) {
                        throw std::runtime_error("Guest pthread callback authentication failed");
                    }
                    entry = *authenticated;
                } else {
                    const auto resume = hostRegisters[33];
                    const auto callerAbi = resume >= 4 &&
                        resume - 4 <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                        ? image.is_arm64e_code(static_cast<std::int64_t>(resume - 4))
                        : std::nullopt;
                    if (!callerAbi || *callerAbi) {
                        throw std::runtime_error("Guest pthread callback requires an authenticated IA/0 pointer");
                    }
                }
                if (entry > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()) ||
                    !image.is_code(static_cast<std::int64_t>(entry))) {
                    throw std::runtime_error("Guest pthread entry is not executable");
                }
                constexpr std::uint64_t stackSize = 512U * 1024U;
                const auto allocationSize = 16U + guestThreadLocalPrefixBytes +
                                            guestTSDSize + stackSize;
                const auto allocation = allocateGuestHeap(allocationSize);
                if (!allocation) {
                    hostRegisters[0] = EAGAIN;
                } else {
                    const auto allocationAddress = static_cast<std::uint64_t>(*allocation);
                    auto* threadStorage = guestMemory.buffer(allocationAddress,
                        allocationSize, true);
                    std::memset(threadStorage, 0, static_cast<std::size_t>(allocationSize));
                    const auto threadId = guestScheduler.next_thread_id();
                    const auto tsdAddress = allocationAddress + 16U +
                                            guestThreadLocalPrefixBytes;
                    const auto stackStart = tsdAddress + guestTSDSize;
                    const auto storeThreadWord = [&](std::uint64_t offset, std::uint64_t value) {
                        std::memcpy(threadStorage + offset, &value, sizeof(value));
                    };
                    const auto tsdOffset = 16U + guestThreadLocalPrefixBytes;
                    storeThreadWord(tsdOffset, threadId);
                    storeThreadWord(tsdOffset + 8U, allocationAddress);
                    storeThreadWord(tsdOffset + guestMachThreadSlotOffset,
                                    guestMachThreadName(threadId));
                    auto thread = GuestScheduler::make_thread_frame(
                        {static_cast<std::int64_t>(stackStart),
                         static_cast<std::int64_t>(stackStart + stackSize)},
                        *allocation, *allocation, static_cast<std::int64_t>(tsdAddress),
                        entry, hostRegisters[3], hostRegisters[guestSchedulerFPCR],
                        hostRegisters[guestSchedulerFPSR], hostServiceState.threadSignalMask);
                    thread.id = threadId;
                    const auto addedId = guestScheduler.add_thread(std::move(thread));
                    if (addedId != threadId) {
                        throw std::logic_error("Guest scheduler assigned an unexpected thread id");
                    }
                    std::memcpy(threadIdDestination, &threadId, sizeof(threadId));
                    hostRegisters[0] = 0;
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 71) {
                const auto joined = guestScheduler.join(hostRegisters[0], hostRegisters[1]);
                if (joined.status == GuestJoinResult::Status::Error) {
                    hostRegisters[0] = static_cast<std::uint32_t>(joined.error);
                } else if (joined.status == GuestJoinResult::Status::Completed) {
                    if (hostRegisters[1] != 0) {
                        auto* output = guestMemory.buffer(hostRegisters[1],
                            sizeof(std::uint64_t), true);
                        std::memcpy(output, &joined.value, sizeof(joined.value));
                    }
                    if (joined.reapedThread && joined.reapedThread->allocation) {
                        hostServiceState.heap->release(*joined.reapedThread->allocation);
                    }
                    hostRegisters[0] = 0;
                    std::copy_n(hostRegisters.begin(), 36,
                                static_cast<std::uint64_t*>(stateBuffer.data()));
                } else {
                    hostRegisters[0] = 0;
                    if (!syncThreadSwitch(false, hostRegisters)) {
                        throw std::runtime_error("Guest pthread_join has no runnable thread");
                    }
                }
                if (joined.status != GuestJoinResult::Status::Waiting) {
                    std::copy_n(hostRegisters.begin(), 36,
                                static_cast<std::uint64_t*>(stateBuffer.data()));
                }
                continue;
            }
            if (service == 72) {
                discardThreadCallbacks(guestScheduler.current_id());
                if (finishGuestThread(hostRegisters[0], hostRegisters)) return 0;
                continue;
            }
            if (service == 174) {
                const auto detached = guestScheduler.detach(hostRegisters[0]);
                hostRegisters[0] = static_cast<std::uint32_t>(detached.error);
                if (detached.reapedThread && detached.reapedThread->allocation) {
                    hostServiceState.heap->release(*detached.reapedThread->allocation);
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 74) {
                hostRegisters[0] = hostRegisters[0] == hostRegisters[1] ? 1 : 0;
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 75) {
                hostRegisters[0] = 0;
                (void)syncThreadSwitch(true, hostRegisters);
                continue;
            }
            if (service >= 65 && service <= 69) {
                const auto address = hostRegisters[0];
                std::int32_t result = 0;
                if (service == 65) {
                    (void)guestMemory.buffer(address, sizeof(pthread_mutex_t), true);
                    const auto attribute = hostRegisters[1];
                    const auto kind = attribute == 0 ? PTHREAD_MUTEX_NORMAL
                        : (mutexAttributes.count(attribute) != 0
                            ? mutexAttributes.at(attribute) : -1);
                    result = kind < 0 ? EINVAL : guestScheduler.initialize_mutex(address, kind);
                } else if (service == 66) {
                    if (!ensureMutex(address)) result = EINVAL;
                    else result = guestScheduler.destroy_mutex(address);
                    if (result == 0) {
                        auto* bytes = guestMemory.buffer(address, sizeof(std::uint64_t), true);
                        std::memset(bytes, 0, sizeof(std::uint64_t));
                    }
                } else if (service == 67 || service == 68) {
                    if (!ensureMutex(address)) result = EINVAL;
                    else {
                        const auto locked = guestScheduler.lock_mutex(
                            address, guestScheduler.current_id(), service == 68);
                        if (locked.status == GuestMutex::LockStatus::Wait) {
                            hostRegisters[0] = 0;
                            if (!syncThreadSwitch(false, hostRegisters)) {
                                throw std::runtime_error(
                                    "Guest mutex wait has no runnable thread");
                            }
                            continue;
                        }
                        result = locked.error;
                    }
                } else {
                    if (!ensureMutex(address)) result = EINVAL;
                    else result = guestScheduler.unlock_mutex(address, guestScheduler.current_id());
                }
                hostRegisters[0] = static_cast<std::uint32_t>(result);
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service >= 76 && service <= 78) {
                const auto address = hostRegisters[0];
                (void)guestMemory.buffer(address, sizeof(pthread_mutexattr_t), true);
                std::int32_t result = 0;
                if (service == 76) {
                    mutexAttributes.insert_or_assign(address, PTHREAD_MUTEX_NORMAL);
                } else {
                    const auto attribute = mutexAttributes.find(address);
                    if (attribute == mutexAttributes.end()) result = EINVAL;
                    else if (service == 77) mutexAttributes.erase(attribute);
                    else {
                        const auto kind = static_cast<std::int32_t>(hostRegisters[1]);
                        if (kind != PTHREAD_MUTEX_NORMAL && kind != PTHREAD_MUTEX_ERRORCHECK &&
                            kind != PTHREAD_MUTEX_RECURSIVE) result = EINVAL;
                        else attribute->second = kind;
                    }
                }
                hostRegisters[0] = static_cast<std::uint32_t>(result);
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 135 || service == 136) {
                if (service == 135) {
                    const auto mode = static_cast<std::int32_t>(hostRegisters[1]);
                    if (hostRegisters[0] == 0) {
                        hostRegisters[0] = imageHandles.open_main(mode);
                        clearGuestDlerror();
                    } else {
                        const auto requestedPath = guest_cstring(guestMemory, hostRegisters[0]);
                        std::uint64_t callerHeader =
                            static_cast<std::uint64_t>(image.main_header);
                        const auto resume = hostRegisters[33];
                        const auto callSite = resume >= 4 ? resume - 4 : resume;
                        for (const auto& segment : image.mapped_image_segments) {
                            if (segment.start >= 0 && segment.end >= segment.start &&
                                static_cast<std::uint64_t>(segment.start) <= callSite &&
                                callSite < static_cast<std::uint64_t>(segment.end)) {
                                callerHeader = segment.guest_header;
                                break;
                            }
                        }
                        auto path = requestedPath;
                        constexpr std::string_view loaderPathPrefix = "@loader_path/";
                        if (path.rfind(loaderPathPrefix, 0) == 0) {
                            const auto caller = std::find_if(
                                image.loaded_image_graph.begin(), image.loaded_image_graph.end(),
                                [callerHeader](const auto& item) {
                                    return item.guest_header == callerHeader;
                                });
                            if (caller == image.loaded_image_graph.end()) {
                                hostRegisters[0] = 0;
                                setGuestDlerror("dlopen: caller image is unavailable for " + path);
                            } else {
                                std::error_code pathError;
                                auto resolved = std::filesystem::path(caller->canonical_path)
                                    .parent_path() / path.substr(loaderPathPrefix.size());
                                resolved = std::filesystem::weakly_canonical(resolved, pathError);
                                if (pathError) resolved = resolved.lexically_normal();
                                path = resolved.string();
                            }
                        }
                        if (hostRegisters[0] == 0) {
                            std::copy_n(hostRegisters.begin(), 36,
                                        static_cast<std::uint64_t*>(stateBuffer.data()));
                            continue;
                        }
                        if ((mode & RTLD_NOLOAD) != 0) {
                                const auto header = imageHandles.header_for_path(path);
                                hostRegisters[0] = header && imageIsLoaded(*header)
                                    ? imageHandles.open(path, mode) : 0;
                                if (hostRegisters[0] == 0)
                                    setGuestDlerror("dlopen: image is not loaded: " + path);
                        } else if (const auto header = imageHandles.header_for_path(path);
                                   header && imageIsLoaded(*header)) {
                                const auto handle = imageHandles.open_loaded(path, mode);
                                hostRegisters[0] = *handle;
                        } else {
                                const auto placement = namedImagePlacementCursor;
                                std::optional<StagedNamedLoad> stagedPlan;
                                try {
                                    stagedPlan.emplace(image.stage_named_load(
                                        path, callerHeader, placement,
                                        (mode & RTLD_NOW) != 0));
                                } catch (const std::exception& error) {
                                    hostRegisters[0] = 0;
                                    setGuestDlerror("dlopen: " + std::string(error.what()));
                                }
                                bool validatedPlan = false;
                                if (stagedPlan) {
                                    try {
                                        validateStagedNamedLoad(*stagedPlan);
                                        validatedPlan = true;
                                    } catch (const std::exception& error) {
                                        hostRegisters[0] = 0;
                                        setGuestDlerror("dlopen: " + std::string(error.what()));
                                    }
                                }
                                if (validatedPlan) {
                                auto& plan = *stagedPlan;
                                if (!plan.cache_images.empty())
                                    image.activate_named_cache_reader();
                                auto cacheInitializers = publishStagedCacheImages(plan);
                                auto initializers = publishStagedNamedLoad(plan);
                                cacheInitializers.insert(cacheInitializers.end(),
                                    initializers.begin(), initializers.end());
                                initializers = std::move(cacheInitializers);
                                for (const auto& staged : plan.images) {
                                    namedImagePlacementCursor = std::max(
                                        namedImagePlacementCursor, staged.guest_end);
                                }
                                constexpr std::uint64_t imageAlignment = 16'384;
                                if (namedImagePlacementCursor >
                                    std::numeric_limits<std::uint64_t>::max() -
                                        (imageAlignment - 1)) {
                                    throw std::runtime_error("Named image placement overflows");
                                }
                                namedImagePlacementCursor =
                                    (namedImagePlacementCursor + imageAlignment - 1) &
                                    ~(imageAlignment - 1);
                                for (const auto& staged : plan.images) {
                                    loadedImageHeaders.insert(staged.guest_header);
                                    if (staged.guest_header == plan.requested_header &&
                                        (mode & RTLD_LOCAL) != 0) continue;
                                    if (std::find(image.global_scope_headers.begin(),
                                                  image.global_scope_headers.end(),
                                                  staged.guest_header) ==
                                        image.global_scope_headers.end()) {
                                        image.global_scope_headers.push_back(
                                            staged.guest_header);
                                    }
                                }
                                for (const auto& staged : plan.cache_images) {
                                    imageHandles.register_loaded(staged.install_name,
                                                                 staged.guest_header);
                                    if (staged.guest_header == plan.requested_header &&
                                        (mode & RTLD_LOCAL) != 0) continue;
                                    if (std::find(image.global_scope_headers.begin(),
                                                  image.global_scope_headers.end(),
                                                  staged.guest_header) ==
                                        image.global_scope_headers.end()) {
                                        image.global_scope_headers.push_back(
                                            staged.guest_header);
                                    }
                                }
                                if (requestedPath.rfind(loaderPathPrefix, 0) != 0)
                                    imageHandles.register_loaded(path, plan.requested_header);
                                hostRegisters[0] = imageHandles.retain(
                                    plan.requested_header, mode);
                                clearGuestDlerror();
                                if (!initializers.empty()) {
                                    GuestCallbackFrame frame;
                                    frame.savedRegisters.assign(hostRegisters.begin(),
                                                                hostRegisters.begin() + 36);
                                    frame.savedRegisters[34] = 0;
                                    const auto* currentState =
                                        static_cast<const std::uint64_t*>(stateBuffer.data());
                                    frame.savedVectorAndAuth.assign(currentState + 806,
                                                                    currentState + guestTokenCount);
                                    frame.authenticationDepth = hostRegisters[37];
                                    frame.initializerTargets = std::move(initializers);
                                    frame.nextInitializer = 1;
                                    const auto first = frame.initializerTargets.front();
                                    callbackFrames.push_back(std::move(frame));
                                    auto* callbackRegisters =
                                        static_cast<std::uint64_t*>(stateBuffer.data());
                                    const auto& pending = callbackFrames.back();
                                    std::copy(pending.savedRegisters.begin(),
                                              pending.savedRegisters.end(), callbackRegisters);
                                    std::copy(pending.savedVectorAndAuth.begin(),
                                              pending.savedVectorAndAuth.end(),
                                              callbackRegisters + 806);
                                    callbackRegisters[37] = pending.authenticationDepth;
                                    std::copy_n(bootstrap.registers.begin(), 4,
                                                callbackRegisters);
                                    callbackRegisters[30] = kNoFault;
                                    callbackRegisters[33] = first;
                                    selectKernel(static_cast<std::int64_t>(first));
                                    continue;
                                }
                                }
                        }
                        if (hostRegisters[0] != 0 && (mode & RTLD_LOCAL) == 0) {
                            if (const auto header = imageHandles.loaded_header(hostRegisters[0]);
                                header && std::find(image.global_scope_headers.begin(),
                                                    image.global_scope_headers.end(), *header) ==
                                              image.global_scope_headers.end()) {
                                image.global_scope_headers.push_back(*header);
                            }
                        }
                        if (hostRegisters[0] != 0) clearGuestDlerror();
                        else if ((mode & RTLD_NOLOAD) != 0) {
                            setGuestDlerror("dlopen: image is not loaded: " + path);
                        }
                    }
                } else {
                    hostRegisters[0] = imageHandles.close(hostRegisters[0]);
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }

            if (service == 73) {
                hostRegisters[0] = guestScheduler.current_id();
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 180 || service == 181) {
                const auto threadId = hostRegisters[0];
                const auto* thread = guestScheduler.find(threadId);
                if (thread == nullptr) {
                    throw std::runtime_error("Guest pthread stack query references unknown thread");
                }
                if (thread->stack.begin < 0 || thread->stack.end <= thread->stack.begin) {
                    throw std::runtime_error("Guest pthread has invalid stack metadata");
                }
                hostRegisters[0] = service == 180
                    ? static_cast<std::uint64_t>(thread->stack.end)
                    : static_cast<std::uint64_t>(thread->stack.end - thread->stack.begin);
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 117) {
                hostRegisters[0] = image.link_time_version(guest_cstring(guestMemory,
                                                                         hostRegisters[0]));
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 122) {
                const auto table = image.cache_reader
                    ? image.cache_reader->objcMessageSendOffsets()
                    : std::optional<SharedCache::MessageSendOffsets>{};
                if (!table) {
                    hostRegisters[0] = 0;
                    hostRegisters[1] = 0;
                } else {
                    const auto bytes = static_cast<std::size_t>(table->count) * sizeof(std::uint64_t);
                    publishCacheMetadata(*image.cache_reader, table->address, bytes, false);
                    hostRegisters[0] = static_cast<std::uint64_t>(table->address);
                    hostRegisters[1] = table->count;
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 126) {
                if (hostRegisters[0] == 0) {
                    throw std::runtime_error("Null guest dyld cache range output");
                }
                auto* output = guestMemory.buffer(hostRegisters[0], sizeof(std::uint64_t), true);
                const auto mappedSize = image.cache_reader
                    ? image.cache_reader->mappedSize() : std::uint64_t{0};
                if (image.cache_reader && mappedSize != 0) {
                    bool containsCacheBase = false;
                    for (std::size_t index = 0; index < resident[0]; ++index) {
                        const auto row = 1 + 4 * index;
                        if (resident[row] <= static_cast<std::uint64_t>(SharedCache::guestBase) &&
                            static_cast<std::uint64_t>(SharedCache::guestBase) < resident[row + 1]) {
                            containsCacheBase = true;
                            break;
                        }
                    }
                    if (!containsCacheBase) {
                        publishCacheMetadata(*image.cache_reader, SharedCache::guestBase, 4096, false);
                    }
                }
                std::memcpy(output, &mappedSize, sizeof(mappedSize));
                hostRegisters[0] = mappedSize == 0
                    ? 0 : static_cast<std::uint64_t>(SharedCache::guestBase);
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 124) {
                const auto kind = static_cast<std::size_t>(
                    static_cast<std::uint32_t>(hostRegisters[2]));
                const auto section = hostRegisters[0] <=
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                    ? image.lookup_guest_section(static_cast<std::int64_t>(hostRegisters[0]), kind)
                    : std::nullopt;
                hostRegisters[0] = section
                    ? static_cast<std::uint64_t>(section->first) : 0;
                hostRegisters[1] = section ? section->second : 0;
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 127 || service == 128) {
                const bool writable = service == 128;
                const auto table = image.cache_reader
                    ? image.cache_reader->objcHeaderTable(writable)
                    : std::optional<SharedCache::HeaderTable>{};
                if (!table) {
                    hostRegisters[0] = 0;
                } else {
                    publishCacheMetadata(*image.cache_reader, table->address,
                                         static_cast<std::size_t>(table->size), writable);
                    hostRegisters[0] = static_cast<std::uint64_t>(table->address);
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 125) {
                if (signalRuntime.has_frames()) {
                    throw std::runtime_error("Guest Objective-C registration inside a signal handler is unsupported");
                }
                const auto tableAddress = hostRegisters[0];
                const auto* table = guestMemory.buffer(tableAddress, 40);
                std::uint64_t version = 0;
                std::uint64_t signedCallback = 0;
                std::memcpy(&version, table, sizeof(version));
                std::memcpy(&signedCallback, table + 8, sizeof(signedCallback));
                if (version != 4) {
                    throw std::runtime_error("Unsupported guest Objective-C callback table version");
                }
                const auto modifier = ((tableAddress + 8) & 0x0000ffffffffffffULL) |
                                      0xd15a000000000000ULL;
                const auto tokenIndex = static_cast<std::size_t>(
                    (signedCallback & 0xffffffffULL) >> 3U);
                auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
                std::uint64_t callback = signedCallback;
                if ((signedCallback & 0xffff000000000000ULL) == 0xa64e000000000000ULL) {
                    if (tokenIndex == 0 || tokenIndex > live[guestTokenCount]) {
                        throw std::runtime_error("Invalid authenticated Objective-C callback pointer");
                    }
                    const auto slot = guestTokenStart + 3 * (tokenIndex - 1);
                    if (signedCallback != encode_guest_pointer(live[slot], live[slot + 2], tokenIndex - 1) ||
                        live[slot + 1] != modifier || live[slot + 2] != 1) {
                        throw std::runtime_error("Unsupported authentication for Objective-C callback");
                    }
                    callback = live[slot];
                } else {
                    throw std::runtime_error("Missing authenticated Objective-C callback pointer");
                }
                if (callback > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                    !image.is_code(static_cast<std::int64_t>(callback))) {
                    throw std::runtime_error("Objective-C callback target is outside guest text");
                }
                std::vector<std::pair<std::uint64_t, std::string>> objcImages;
                for (const auto& [header, path] : image.image_paths) {
                    const auto info = image.lookup_guest_section(
                        static_cast<std::int64_t>(header), 6);
                    if (info && info->second != 0) objcImages.emplace_back(header, path);
                }
                std::sort(objcImages.begin(), objcImages.end(),
                    [](const auto& left, const auto& right) { return left.first < right.first; });
                if (objcImages.empty()) {
                    std::copy_n(hostRegisters.begin(), 36,
                                static_cast<std::uint64_t*>(stateBuffer.data()));
                    continue;
                }
                const auto recordsBytes = objcImages.size() * 32;
                std::size_t pathsBytes = 0;
                for (const auto& entry : objcImages) {
                    if (entry.second.size() + 1 > std::numeric_limits<std::size_t>::max() - pathsBytes) {
                        throw std::runtime_error("Objective-C image batch size overflow");
                    }
                    pathsBytes += entry.second.size() + 1;
                }
                if (pathsBytes > std::numeric_limits<std::size_t>::max() - recordsBytes) {
                    throw std::runtime_error("Objective-C image batch size overflow");
                }
                const auto allocateGuest = [&](std::uint64_t amount) -> std::optional<std::uint64_t> {
                    auto allocationRegisters = hostRegisters;
                    allocationRegisters[0] = amount;
                    const auto allocation = dispatch_guest_host_service(
                        allocationRegisters, guestMemory, bootstrap, hostServiceState, 26);
                    if (allocation.exited || allocationRegisters[0] == 0) return std::nullopt;
                    return allocationRegisters[0];
                };
                const auto batch = allocateGuest(recordsBytes + pathsBytes);
                const auto block = allocateGuest(48);
                if (!batch || !block) throw std::runtime_error("Guest Objective-C image batch allocation failed");
                std::memset(guestMemory.buffer(*block, 48, true), 0, 48);
                ensureTokenCapacity(1);
                live = static_cast<std::uint64_t*>(stateBuffer.data());
                const auto newTokenIndex = static_cast<std::size_t>(live[guestTokenCount]);
                const auto tokenSlot = guestTokenStart + 3 * newTokenIndex;
                const auto invoke = static_cast<std::uint64_t>(ExecutableImage::objc_mutation_invoke);
                live[tokenSlot] = invoke;
                live[tokenSlot + 1] = *block + 16;
                live[tokenSlot + 2] = 0;
                ++live[guestTokenCount];
                auto* blockData = guestMemory.buffer(*block, 48, true);
                const auto invokePointer = encode_guest_pointer(invoke, 0, newTokenIndex);
                const auto recordPointer = *block + 32;
                const std::uint64_t recordSize = 32;
                std::memcpy(blockData + 16, &invokePointer, sizeof(invokePointer));
                std::memcpy(blockData + 24, &recordPointer, sizeof(recordPointer));
                std::memcpy(blockData + 40, &recordSize, sizeof(recordSize));
                auto* batchData = guestMemory.buffer(*batch, recordsBytes + pathsBytes, true);
                std::size_t pathOffset = recordsBytes;
                for (std::size_t index = 0; index < objcImages.size(); ++index) {
                    const auto record = index * 32;
                    const auto header = objcImages[index].first;
                    const auto pathAddress = *batch + pathOffset;
                    const std::uint64_t third = 0;
                    const std::uint64_t flags = header >= static_cast<std::uint64_t>(SharedCache::guestBase)
                        ? 5 : 0;
                    std::memcpy(batchData + record, &header, sizeof(header));
                    std::memcpy(batchData + record + 8, &pathAddress, sizeof(pathAddress));
                    std::memcpy(batchData + record + 16, &third, sizeof(third));
                    std::memcpy(batchData + record + 24, &flags, sizeof(flags));
                    const auto& path = objcImages[index].second;
                    std::memcpy(batchData + pathOffset, path.c_str(), path.size() + 1);
                    pathOffset += path.size() + 1;
                }
                GuestCallbackFrame frame;
                const auto* stateWordsPtr = static_cast<const std::uint64_t*>(stateBuffer.data());
                frame.savedRegisters.assign(stateWordsPtr, stateWordsPtr + 36);
                frame.savedRegisters[34] = 0;
                if (stateBuffer.size() / sizeof(std::uint64_t) < guestTokenCount) {
                    throw std::runtime_error("Guest callback register image is truncated");
                }
                frame.savedVectorAndAuth.assign(stateWordsPtr + 806,
                                                stateWordsPtr + guestTokenCount);
                frame.authenticationDepth = live[37];
                frame.mutationBlock = *block;
                frame.callback = callback;
                frame.arguments.push_back({static_cast<std::uint64_t>(objcImages.size()),
                                           *batch, *block, 0});
                frame.nextArgument = 1;
                for (const auto& entry : objcImages) frame.objcHeaders.push_back(entry.first);
                callbackFrames.push_back(std::move(frame));
                live[0] = objcImages.size();
                live[1] = *batch;
                live[2] = *block;
                live[3] = 0;
                live[30] = kNoFault;
                live[33] = callback;
                selectKernel(static_cast<std::int64_t>(callback));
                continue;
            }
            if (service == 131) {
                if (callbackFrames.empty() || callbackFrames.back().mutationBlock == 0 ||
                    hostRegisters[0] != callbackFrames.back().mutationBlock ||
                    hostRegisters[1] >= callbackFrames.back().objcHeaders.size()) {
                    throw std::runtime_error("Invalid guest Objective-C mutation request");
                }
                const auto header = callbackFrames.back().objcHeaders[
                    static_cast<std::size_t>(hostRegisters[1])];
                if (header >= static_cast<std::uint64_t>(SharedCache::guestBase) &&
                    objcWritableBackings.empty()) {
                    setObjCCacheWrites(true);
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 134) {
                hostRegisters[0] = image.cache_reader
                    ? image.cache_reader->objcClassCount() : 0;
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 133) {
                if (!image.cache_reader) {
                    hostRegisters[0] = 0;
                    std::copy_n(hostRegisters.begin(), 36,
                                static_cast<std::uint64_t*>(stateBuffer.data()));
                    continue;
                }
                if (signalRuntime.has_frames()) {
                    throw std::runtime_error("Guest class enumeration inside a signal handler is unsupported");
                }
                const auto name = guest_cstring(guestMemory, hostRegisters[0]);
                std::array<std::uint8_t, 16> uuid{};
                const auto count = guest_cached_classes(name.c_str(), nullptr, 0, uuid.data());
                if (count < 0 || !std::equal(uuid.begin(), uuid.end(),
                                             image.cache_reader->uuid().begin(),
                                             image.cache_reader->uuid().end())) {
                    throw std::runtime_error("Host and guest Objective-C class caches do not match");
                }
                std::vector<std::uint64_t> offsets(static_cast<std::size_t>(count));
                if (count > 0 && guest_cached_classes(name.c_str(), offsets.data(), count,
                                                       uuid.data()) != count) {
                    throw std::runtime_error("Objective-C cache class enumeration changed");
                }
                if (offsets.empty()) {
                    std::copy_n(hostRegisters.begin(), 36,
                                static_cast<std::uint64_t*>(stateBuffer.data()));
                    continue;
                }
                const auto blockAddress = hostRegisters[1];
                const auto* block = guestMemory.buffer(blockAddress, 24);
                std::uint64_t signedCallback = 0;
                std::memcpy(&signedCallback, block + 16, sizeof(signedCallback));
                const auto modifier = blockAddress + 16;
                std::uint64_t callback = signedCallback;
                if ((signedCallback & 0xffff000000000000ULL) == 0xa64e000000000000ULL) {
                    const auto index = static_cast<std::size_t>(
                        (signedCallback & 0xffffffffULL) >> 3U);
                    auto* current = static_cast<std::uint64_t*>(stateBuffer.data());
                    if (index == 0 || index > current[guestTokenCount]) {
                        throw std::runtime_error("Invalid authenticated Objective-C class callback");
                    }
                    const auto slot = guestTokenStart + 3 * (index - 1);
                    if (signedCallback != encode_guest_pointer(current[slot], current[slot + 2], index - 1) ||
                        current[slot + 1] != modifier || current[slot + 2] != 0) {
                        throw std::runtime_error("Unsupported authentication for Objective-C class callback");
                    }
                    callback = current[slot];
                }
                if (callback > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                    !image.is_code(static_cast<std::int64_t>(callback))) {
                    throw std::runtime_error("Objective-C class callback target is outside guest text");
                }
                const auto allocateGuest = [&](std::uint64_t amount) -> std::optional<std::uint64_t> {
                    auto allocationRegisters = hostRegisters;
                    allocationRegisters[0] = amount;
                    const auto allocation = dispatch_guest_host_service(
                        allocationRegisters, guestMemory, bootstrap, hostServiceState, 26);
                    if (allocation.exited || allocationRegisters[0] == 0) return std::nullopt;
                    return allocationRegisters[0];
                };
                const auto stop = allocateGuest(1);
                if (!stop) throw std::runtime_error("Guest Objective-C class stop flag allocation failed");
                *guestMemory.buffer(*stop, 1, true) = 0;
                const auto writableTable = image.cache_reader->objcHeaderTable(true);
                if (writableTable) {
                    publishCacheMetadata(*image.cache_reader, writableTable->address,
                                         static_cast<std::size_t>(writableTable->size), true);
                }
                std::vector<std::array<std::uint64_t, 4>> arguments;
                arguments.reserve(offsets.size());
                for (const auto offset : offsets) {
                    if (offset >= image.cache_reader->mappedSize()) {
                        throw std::runtime_error("Invalid Objective-C cache class offset");
                    }
                    const auto address = static_cast<std::uint64_t>(SharedCache::guestBase) + offset;
                    std::uint64_t loaded = 0;
                    if (writableTable) {
                        const auto owner = std::find_if(image.cached_image_ranges.begin(),
                            image.cached_image_ranges.end(), [address](const CachedImageRange& range) {
                                return address >= static_cast<std::uint64_t>(range.logical_start) &&
                                       address < static_cast<std::uint64_t>(range.logical_end);
                            });
                        if (owner != image.cached_image_ranges.end()) {
                            const auto index = image.cache_reader->objcHeaderIndex(owner->header);
                            if (index && writableTable->size >= 16 &&
                                *index <= (writableTable->size - 16) / 8) {
                                std::uint64_t value = 0;
                                const auto slotAddress = static_cast<std::uint64_t>(writableTable->address) +
                                                         8 + *index * 8;
                                std::memcpy(&value, guestMemory.buffer(slotAddress, 8), 8);
                                loaded = value & 1U;
                            }
                        }
                    }
                    arguments.push_back({blockAddress, address, loaded, *stop});
                }
                GuestCallbackFrame frame;
                const auto* stateWords = static_cast<const std::uint64_t*>(stateBuffer.data());
                frame.savedRegisters.assign(stateWords, stateWords + 36);
                frame.savedRegisters[34] = 0;
                frame.savedVectorAndAuth.assign(stateWords + 806, stateWords + guestTokenCount);
                frame.authenticationDepth = stateWords[37];
                frame.callback = callback;
                frame.arguments = std::move(arguments);
                frame.stopAddress = *stop;
                callbackFrames.push_back(std::move(frame));
                const auto first = callbackFrames.back().arguments.front();
                auto* current = static_cast<std::uint64_t*>(stateBuffer.data());
                std::copy(first.begin(), first.end(), current);
                current[30] = kNoFault;
                current[33] = callback;
                ++callbackFrames.back().nextArgument;
                selectKernel(static_cast<std::int64_t>(callback));
                continue;
            }
            if (service == 102 || service == 103) {
                if (!image.build_version) {
                    throw std::runtime_error("Guest executable has no supported build version");
                }
                const auto result = guest_version_token_at_least(
                    image.build_version->token(service == 102), hostRegisters[0]);
                if (result < 0) {
                    throw std::runtime_error("Darwin version-set comparison is unavailable");
                }
                hostRegisters[0] = static_cast<std::uint64_t>(result);
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 115) {
                const auto slide = image.image_slides.find(hostRegisters[0]);
                hostRegisters[0] = slide == image.image_slides.end() ? 0 : slide->second;
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 101) {
                hostRegisters[0] = static_cast<std::uint64_t>(::issetugid());
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 111) {
                const auto name = guest_cstring(guestMemory, hostRegisters[0]);
                std::uint64_t offset = 0;
                std::array<std::uint8_t, 16> uuid{};
                const auto result = guest_cached_selector(name.c_str(), &offset, uuid.data());
                if (result == 0) {
                    hostRegisters[0] = 0;
                } else {
                    if (result < 0 || !image.cache_reader ||
                        !std::equal(uuid.begin(), uuid.end(), image.cache_reader->uuid().begin(),
                                    image.cache_reader->uuid().end()) ||
                        offset >= static_cast<std::uint64_t>(SharedCache::guestLimit -
                                                             SharedCache::guestBase)) {
                        throw std::runtime_error("Host and guest selector caches do not match");
                    }
                    const auto address = SharedCache::guestBase + static_cast<std::int64_t>(offset);
                    std::vector<std::uint8_t> expected(name.begin(), name.end());
                    expected.push_back(0);
                    const auto original = image.cache_reader->originalAddress(address);
                    if (image.cache_reader->bytes(original, expected.size()) != expected) {
                        throw std::runtime_error("Cached selector bytes do not match the guest name");
                    }
                    bool residentAlready = false;
                    for (std::size_t index = 0; index < resident[0]; ++index) {
                        const auto row = 1 + 4 * index;
                        if (address >= static_cast<std::int64_t>(resident[row]) &&
                            address < static_cast<std::int64_t>(resident[row + 1]) &&
                            expected.size() <= static_cast<std::uint64_t>(resident[row + 1]) -
                                                   static_cast<std::uint64_t>(address)) {
                            residentAlready = true;
                            break;
                        }
                    }
                    if (!residentAlready &&
                        !makeCacheResident(static_cast<std::uint64_t>(address), expected.size()) &&
                        !mapSelectorWindow(static_cast<std::uint64_t>(address), expected.size())) {
                        throw std::runtime_error("Cached selector could not be made resident");
                    }
                    hostRegisters[0] = static_cast<std::uint64_t>(address);
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            const auto result = dispatch_guest_host_service(hostRegisters, guestMemory, bootstrap,
                                                            hostServiceState, service);
            std::copy_n(hostRegisters.begin(), 36,
                        static_cast<std::uint64_t*>(stateBuffer.data()));
            if (result.exited) return result.exitStatus;
            if (hostRegisters[33] == kNoFault && callbackFrames.empty()) {
                if (signalRuntime.has_frames() && resumeSignalFrame()) continue;
                if (advanceStartup()) continue;
            }
            continue;
        }
        if (status == 7) {
            const auto pc = registers[33];
            if (pc > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::runtime_error("Guest process resumed outside translated text at PC 0x" +
                                         hexadecimal(pc));
            }
            const auto entry = static_cast<std::int64_t>(pc);
            selectKernel(entry);
            continue;
        }
        if (status == 2) {
            if (cache_faulted(registers) && options.demandCache) {
                const auto requested = registers[guestCacheFaultAddress];
                const auto width = registers[guestCacheFaultWidth];
                const auto faultPc = registers[33];
                if (activeKernel->usesInterpreter) {
                    const auto faultPacket = std::find_if(
                        activeKernel->interpretedPackets.begin(),
                        activeKernel->interpretedPackets.end(),
                        [faultPc](const auto& packet) { return packet.pc == faultPc; });
                    if (registers[guestFaultAddress] != requested ||
                        (width != 4 && width != 8) ||
                        faultPacket == activeKernel->interpretedPackets.end()) {
                        throw std::runtime_error(
                            "Interpreter cache retry has invalid fault provenance");
                    }
                    const auto opcode = static_cast<translate_core::Opcode>(
                        faultPacket->operation[0]);
                    if ((opcode != translate_core::Opcode::LoadUnsignedImmediate &&
                         opcode != translate_core::Opcode::StoreUnsignedImmediate) ||
                        faultPacket->operation[5] / 8U != width) {
                        throw std::runtime_error(
                            "Interpreter cache retry does not match its memory packet");
                    }
                    progressTrace.event("interpreter-cache-fault", faultPc, requested);
                }
                if (!makeCacheResident(requested, width)) {
                    throw std::runtime_error("Guest cache fault could not be resolved at PC 0x" +
                        hexadecimal(registers[33]) + ", address 0x" + hexadecimal(requested) +
                        ", width " + std::to_string(width));
                }
                auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
                live[guestFaultAddress] = kNoFault;
                live[guestCacheFaultAddress] = kNoFault;
                live[guestCacheFaultWidth] = 0;
                if (activeKernel->usesInterpreter) {
                    live[35] = faultPc | gpu_interpreter::cacheRetryPcMarker;
                    progressTrace.event("interpreter-cache-resident", faultPc, requested);
                }
                continue;
            }
            const auto address = registers[guestFaultAddress];
            std::vector<std::uint64_t> live(
                registers, registers + stateBuffer.size() / sizeof(std::uint64_t));
            const bool protectionFault = guestMemory.is_protection_fault(address);
            const GuestSynchronousFault fault{
                protectionFault ? SIGBUS : SIGSEGV,
                protectionFault ? BUS_ADRALN : SEGV_MAPERR,
                address, registers[33]};
            const auto delivery = signalRuntime.deliver_synchronous(
                hostServiceState.signals, hostServiceState.threadSignalMask,
                live, guestMemory, fault, live[37],
                static_cast<std::uint64_t>(activeEntry));
            if (delivery.outcome == GuestSignalOutcome::ProcessExit) {
                if (hostServiceState.signals.action(fault.signal).handler == 0) {
                    std::cerr << "arm64-metal: Guest process memory access fault at 0x"
                              << hexadecimal(address) << " at PC 0x"
                              << hexadecimal(registers[33]) << " width "
                              << registers[guestCacheFaultWidth] << " x10=0x"
                              << hexadecimal(registers[10]) << " tp=0x"
                              << hexadecimal(registers[guestThreadPointer]) << '\n';
                }
                return delivery.exitStatus;
            }
            if (delivery.outcome == GuestSignalOutcome::HandlerEntered) {
                std::memcpy(stateBuffer.data(), live.data(), live.size() * sizeof(std::uint64_t));
                selectKernel(static_cast<std::int64_t>(delivery.handler));
                continue;
            }
            throw std::runtime_error("Guest process memory access fault at 0x" +
                hexadecimal(address) + " at PC 0x" + hexadecimal(registers[33]) +
                " width " + std::to_string(registers[guestCacheFaultWidth]) +
                " x10=0x" + hexadecimal(registers[10]) +
                " tp=0x" + hexadecimal(registers[guestThreadPointer]));
        }
        if (status == 3 || status == 14) {
            const auto address = status == 3 ? registers[guestFaultAddress] : registers[33];
            const GuestSynchronousFault fault{
                status == 3 ? SIGSEGV : SIGILL,
                status == 3 ? SEGV_MAPERR : ILL_ILLTRP,
                address, registers[33]};
            std::vector<std::uint64_t> live(
                registers, registers + stateBuffer.size() / sizeof(std::uint64_t));
            const auto delivery = signalRuntime.deliver_synchronous(
                hostServiceState.signals, hostServiceState.threadSignalMask,
                live, guestMemory, fault, live[37],
                static_cast<std::uint64_t>(activeEntry));
            if (delivery.outcome == GuestSignalOutcome::ProcessExit) {
                if (status == 3 &&
                    hostServiceState.signals.action(fault.signal).handler == 0) {
                    std::cerr << "arm64-metal: Guest process reached an invalid code address\n";
                }
                return delivery.exitStatus;
            }
            if (delivery.outcome == GuestSignalOutcome::HandlerEntered) {
                std::memcpy(stateBuffer.data(), live.data(), live.size() * sizeof(std::uint64_t));
                selectKernel(static_cast<std::int64_t>(delivery.handler));
                continue;
            }
            throw std::runtime_error("Guest synchronous signal was not delivered");
        }
        if (status == 5) {
            const auto pc = static_cast<std::int64_t>(registers[33]);
            const auto error = decodeErrors.find(pc);
            if (error != decodeErrors.end()) throw std::runtime_error(error->second);
            throw std::runtime_error("Guest process reached an unsupported translated instruction at PC 0x" +
                                     hexadecimal(static_cast<std::uint64_t>(pc)));
        }
        if (status == 6) throw std::runtime_error("Guest return authentication failed");
        if (status == 9 || status == 10 || status == 12) {
            throw std::runtime_error(pac_failure_message(registers,
                stateBuffer.size() / sizeof(std::uint64_t), status));
        }
        if (status == 13) {
            const auto syscall = registers[16];
            progressTrace.event("raw-syscall", registers[33], syscall);
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-14))) {
                if (registers[0] != bootstrap.guestTaskPort) {
                    registers[0] = machSendInvalidDest;
                } else if (!hostServiceState.machVmProtect) {
                    registers[0] = kernInvalidArgument;
                } else {
                    registers[0] = hostServiceState.machVmProtect(
                        registers[1], registers[2], registers[3] != 0,
                        static_cast<std::uint32_t>(registers[4]));
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == 177) {
                constexpr std::uint64_t carryMask = 8U;
                const auto outputAddress = registers[0];
                const auto sizeAddress = registers[1];
                registers[32] |= carryMask;
                if (outputAddress == 0 || sizeAddress == 0) {
                    registers[0] = EINVAL;
                } else if (outputAddress < ExecutableImage::minimum_image_address) {
                    registers[0] = EFAULT;
                } else {
                    try {
                        auto* output = guestMemory.buffer(outputAddress,
                            sizeof(std::uint64_t), true);
                        if (!kdebugTypeFilterBacking) {
                            const auto allocated = mappingHeap.allocate(
                                bootstrap.pageSize, static_cast<std::int64_t>(bootstrap.pageSize));
                            if (!allocated || resident[0] >= residentCapacity) {
                                if (allocated) mappingHeap.release(*allocated);
                                registers[0] = ENOMEM;
                                registers[34] = 0;
                                continue;
                            }
                            kdebugTypeFilterBacking = static_cast<std::size_t>(*allocated);
                            std::memset(static_cast<std::uint8_t*>(memoryBuffer.data()) +
                                *kdebugTypeFilterBacking, 0, bootstrap.pageSize);
                        }
                        if (kdebugTypeFilterLogicalCursor >
                                std::numeric_limits<std::uint64_t>::max() - bootstrap.pageSize ||
                            resident[0] >= residentCapacity) {
                            registers[0] = ENOMEM;
                            registers[34] = 0;
                            continue;
                        }
                        const auto mappedAddress = kdebugTypeFilterLogicalCursor;
                        publishVmRegion({mappedAddress,
                            mappedAddress + bootstrap.pageSize,
                            *kdebugTypeFilterBacking, true, false, false});
                        syncResidentTable();
                        std::memcpy(output, &mappedAddress, sizeof(mappedAddress));
                        kdebugTypeFilterLogicalCursor += bootstrap.pageSize;
                        registers[0] = 0;
                        registers[32] &= ~carryMask;
                    } catch (const std::out_of_range&) {
                        registers[0] = EFAULT;
                    }
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == 33) {
                registers[32] |= 8U;
                if (registers[0] < ExecutableImage::minimum_image_address) {
                    registers[0] = EFAULT;
                } else {
                    try {
                        (void)guest_cstring(guestMemory, registers[0]);
                        std::vector<std::uint64_t> accessRegisters(registers, registers + 36);
                        (void)dispatch_guest_host_service(accessRegisters, guestMemory,
                            bootstrap, hostServiceState, 161);
                        if (accessRegisters[0] == std::numeric_limits<std::uint64_t>::max()) {
                            std::uint32_t error = 0;
                            std::memcpy(&error, guestMemory.buffer(bootstrap.errnoSlot,
                                sizeof(error)), sizeof(error));
                            registers[0] = error;
                        } else {
                            registers[0] = accessRegisters[0];
                            registers[32] &= ~std::uint64_t{8};
                        }
                    } catch (const std::exception&) {
                        registers[0] = EFAULT;
                    }
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == 220 || syscall == 286 || syscall == 340 || syscall == 483) {
                std::vector<std::uint64_t> trapRegisters(registers, registers + 36);
                if (syscall == 220) {
                    auto* errnoBytes = guestMemory.buffer(bootstrap.errnoSlot,
                        sizeof(std::uint32_t), true);
                    std::uint32_t savedErrno = 0;
                    std::memcpy(&savedErrno, errnoBytes, sizeof(savedErrno));
                    (void)dispatch_guest_host_service(trapRegisters, guestMemory,
                        bootstrap, hostServiceState, 170);
                    if (static_cast<std::uint32_t>(trapRegisters[0]) ==
                        std::numeric_limits<std::uint32_t>::max()) {
                        std::uint32_t error = 0;
                        std::memcpy(&error, errnoBytes, sizeof(error));
                        trapRegisters[0] = error;
                        trapRegisters[32] |= 8U;
                    } else {
                        trapRegisters[32] &= ~std::uint64_t{8};
                    }
                    std::memcpy(errnoBytes, &savedErrno, sizeof(savedErrno));
                } else if (syscall == 286) {
                    (void)dispatch_guest_host_service(trapRegisters, guestMemory,
                        bootstrap, hostServiceState, 175);
                } else if (syscall == 340) {
                    (void)dispatch_guest_host_service(trapRegisters, guestMemory,
                        bootstrap, hostServiceState, 171);
                } else {
                    auto* errnoBytes = guestMemory.buffer(bootstrap.errnoSlot,
                        sizeof(std::uint32_t), true);
                    std::uint32_t savedErrno = 0;
                    std::memcpy(&savedErrno, errnoBytes, sizeof(savedErrno));
                    (void)dispatch_guest_host_service(trapRegisters, guestMemory,
                        bootstrap, hostServiceState, 173);
                    std::memcpy(errnoBytes, &savedErrno, sizeof(savedErrno));
                }
                registers[0] = trapRegisters[0];
                registers[32] = trapRegisters[32];
                registers[34] = 0;
                continue;
            }
            if (syscall == 169) {
                if (static_cast<std::uint32_t>(registers[1]) != 0) {
                    throw std::runtime_error("Unsupported guest csops operation");
                }
                const auto pid = static_cast<std::int32_t>(registers[0]);
                registers[32] |= 8U;
                if (pid != 0 && pid != ::getpid()) {
                    registers[0] = ESRCH;
                } else if (registers[2] != 0 &&
                           registers[2] < ExecutableImage::minimum_image_address) {
                    registers[0] = EFAULT;
                } else {
                    try {
                        if (registers[2] != 0) {
                            std::memcpy(guestMemory.buffer(registers[2],
                                sizeof(signing.status), true), &signing.status,
                                sizeof(signing.status));
                        }
                        registers[0] = 0;
                        registers[32] &= ~std::uint64_t{8};
                    } catch (const std::out_of_range&) {
                        registers[0] = EFAULT;
                    }
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == 397) {
                std::vector<std::uint64_t> writeRegisters(registers, registers + 36);
                (void)dispatch_guest_host_service(writeRegisters, guestMemory, bootstrap,
                                                  hostServiceState, 2);
                if (writeRegisters[0] == std::numeric_limits<std::uint64_t>::max()) {
                    std::uint32_t error = 0;
                    std::memcpy(&error, guestMemory.buffer(bootstrap.errnoSlot,
                        sizeof(error)), sizeof(error));
                    registers[0] = error;
                    registers[32] |= 8U;
                } else {
                    registers[0] = writeRegisters[0];
                    registers[32] &= ~std::uint64_t{8};
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == 381) {
                const auto policy = guest_cstring(guestMemory, registers[0]);
                if (registers[1] != 2 || policy != "Sandbox") {
                    throw std::runtime_error("Unsupported guest MAC policy or call");
                }
                const auto* request = guestMemory.buffer(registers[2], 48);
                std::array<std::uint64_t, 6> fields{};
                std::memcpy(fields.data(), request, sizeof(fields));
                const auto resultAddress = fields[0];
                const auto pid = static_cast<std::int64_t>(fields[1]);
                const auto operationAddress = fields[2];
                const auto filter = fields[3];
                const auto pathAddress = fields[4];
                const auto flags = fields[5];
                if (pid != static_cast<std::int64_t>(::getpid()) || filter != 1 || flags != 0) {
                    throw std::runtime_error("Unsupported guest sandbox path query");
                }
                registers[32] |= 8U;
                registers[0] = EFAULT;
                if (resultAddress != 0 && operationAddress != 0 && pathAddress != 0) {
                    try {
                        const auto operation = guest_cstring(guestMemory, operationAddress);
                        const auto path = guest_cstring(guestMemory, pathAddress);
                        auto* output = guestMemory.buffer(resultAddress, sizeof(std::uint64_t), true);
                        std::uint64_t resultValue = 0;
                        const auto error = guest_sandbox_check_path(operation.c_str(), path.c_str(),
                                                                    &resultValue);
                        registers[0] = static_cast<std::uint64_t>(error);
                        if (error == 0) {
                            std::memcpy(output, &resultValue, sizeof(resultValue));
                            registers[32] &= ~std::uint64_t{8};
                        }
                    } catch (const std::exception&) {
                        registers[0] = EFAULT;
                    }
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-47))) {
                const auto reply = static_cast<std::uint32_t>(registers[3] >> 32);
                const auto messageId = static_cast<std::uint32_t>(registers[4] >> 32);
                if (registers[1] != 0x2'0000'0003ULL ||
                    static_cast<std::uint32_t>(registers[2]) != 0x1513 ||
                    static_cast<std::uint32_t>(registers[3]) != guestMachTaskPort ||
                    static_cast<std::uint32_t>(registers[4]) != 0 ||
                    (messageId != 8000 && messageId != 8001) ||
                    registers[5] != (static_cast<std::uint64_t>(reply) << 32) ||
                    registers[6] < 44 || registers[6] > std::numeric_limits<std::uint32_t>::max() ||
                    registers[7] != 0 || guestReceiveRights.find(reply) == guestReceiveRights.end()) {
                    throw std::runtime_error("Unsupported guest Mach message");
                }
                const auto sendSize = registers[2] >> 32;
                auto result = std::uint32_t{4}; // KERN_INVALID_ARGUMENT
                const auto responseSize = std::uint64_t{44};
                if ((messageId == 8000 && (sendSize < 36 || sendSize > 1060)) ||
                    (messageId == 8001 && sendSize != 24) ||
                    registers[6] < responseSize ||
                    registers[6] > std::numeric_limits<std::uint32_t>::max()) {
                    result = 0x1000000a; // MACH_SEND_INVALID_DATA
                } else {
                    const auto* request = guestMemory.buffer(registers[0], sendSize);
                    (void)guestMemory.buffer(registers[0], responseSize, true);
                    if (messageId == 8001) {
                        {
                            auto& thread = guestScheduler.current();
                            const auto pc = thread.registers.size() > 33
                                ? thread.registers[33] : 0;
                            const auto recovery = std::find_if(
                                restartableRanges.begin(), restartableRanges.end(),
                                [pc](const RestartableRange& range) {
                                    return range.begin < pc && pc < range.end &&
                                           range.recovery != 0;
                                });
                            if (recovery != restartableRanges.end()) {
                                thread.registers[33] = recovery->recovery;
                            }
                        }
                        result = 0;
                    } else {
                        std::uint32_t count = 0;
                        std::memcpy(&count, request + 32, sizeof(count));
                        if (count > 0 && count <= 64 && sendSize == 36 + count * 16) {
                            std::vector<RestartableRange> ranges;
                            ranges.reserve(count);
                            bool valid = true;
                            for (std::uint32_t index = 0; index < count; ++index) {
                                const auto offset = static_cast<std::size_t>(36 + index * 16);
                                std::uint64_t start = 0;
                                std::uint16_t length = 0;
                                std::uint16_t recoveryOffset = 0;
                                std::uint32_t flags = 0;
                                std::memcpy(&start, request + offset, sizeof(start));
                                std::memcpy(&length, request + offset + 8, sizeof(length));
                                std::memcpy(&recoveryOffset, request + offset + 10,
                                            sizeof(recoveryOffset));
                                std::memcpy(&flags, request + offset + 12, sizeof(flags));
                                const auto span = std::max<std::uint64_t>(length, recoveryOffset);
                                if (flags != 0 || length > 4096 || recoveryOffset > 4096 ||
                                    start > std::numeric_limits<std::uint64_t>::max() - span) {
                                    valid = false;
                                    break;
                                }
                                ranges.push_back({start, start + length,
                                                  start + recoveryOffset});
                            }
                            std::sort(ranges.begin(), ranges.end(),
                                [](const RestartableRange& left, const RestartableRange& right) {
                                    return std::tie(left.begin, left.end) <
                                           std::tie(right.begin, right.end);
                                });
                            for (std::size_t index = 1; index < ranges.size(); ++index) {
                                if (ranges[index - 1].end > ranges[index].begin) valid = false;
                            }
                            if (valid) {
                                if (guestScheduler.thread_count() > 1 ||
                                    !restartableRanges.empty()) {
                                    result = 46; // KERN_NOT_SUPPORTED
                                } else {
                                    restartableRanges = std::move(ranges);
                                    result = 0;
                                }
                            }
                        }
                    }
                }
                std::array<std::uint32_t, 11> response{
                    0x1200, 36, 0, reply, 0, messageId + 100, 0, 1, result, 0, 8};
                std::memcpy(guestMemory.buffer(registers[0], sizeof(response), true),
                            response.data(), sizeof(response));
                registers[0] = 0;
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-26))) {
                constexpr std::uint32_t replyPortFlag = 0x00001000;
                if (nextGuestReceiveName >= std::numeric_limits<std::uint32_t>::max() - 4) {
                    registers[0] = 0;
                } else {
                    const auto name = nextGuestReceiveName;
                    nextGuestReceiveName += 4;
                    guestReceiveRights.emplace(name, GuestReceiveRight{replyPortFlag, 0});
                    registers[0] = name;
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-28))) {
                constexpr std::uint64_t kernInvalidAddress = 1;
                constexpr std::uint64_t kernInvalidTask = 5;
                if (static_cast<std::uint32_t>(registers[0]) != guestMachTaskPort) {
                    registers[0] = kernInvalidTask;
                } else if (registers[1] == 0) {
                    registers[0] = kernInvalidAddress;
                } else {
                    try {
                        auto* output = guestMemory.buffer(registers[1], sizeof(std::int32_t), true);
                        const auto processId = static_cast<std::int32_t>(getpid());
                        std::memcpy(output, &processId, sizeof(processId));
                        registers[0] = 0;
                    } catch (const std::exception&) {
                        registers[0] = kernInvalidAddress;
                    }
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-24))) {
                constexpr std::uint64_t kernSuccess = 0;
                constexpr std::uint64_t kernInvalidAddress = 1;
                constexpr std::uint64_t kernNoSpace = 3;
                constexpr std::uint64_t machSendInvalidDest = 0x10000003;
                constexpr std::uint32_t guardedPortFlag = MPO_CONTEXT_AS_GUARD;
                constexpr std::uint32_t replyPortFlag = 0x00001000;
                auto result = kernSuccess;
                if (static_cast<std::uint32_t>(registers[0]) != guestMachTaskPort) {
                    result = machSendInvalidDest;
                } else if (registers[1] == 0 || registers[3] == 0) {
                    result = kernInvalidAddress;
                } else {
                    const auto* options = guestMemory.buffer(
                        registers[1], sizeof(mach_port_options_t));
                    auto* output = guestMemory.buffer(registers[3], sizeof(std::uint32_t), true);
                    std::uint32_t flags = 0;
                    std::memcpy(&flags, options, sizeof(flags));
                    if ((flags & ~(guardedPortFlag | replyPortFlag)) != 0) {
                        throw std::runtime_error("Unsupported guest Mach port construction flags");
                    }
                    if (nextGuestReceiveName >= std::numeric_limits<std::uint32_t>::max() - 4) {
                        result = kernNoSpace;
                    } else {
                        const auto name = nextGuestReceiveName;
                        nextGuestReceiveName += 4;
                        guestReceiveRights.emplace(name, GuestReceiveRight{flags, registers[2]});
                        std::memcpy(output, &name, sizeof(name));
                    }
                }
                registers[0] = result;
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-25))) {
                constexpr std::uint64_t kernSuccess = 0;
                constexpr std::uint64_t kernInvalidName = 15;
                constexpr std::uint64_t machSendInvalidDest = 0x10000003;
                constexpr std::uint64_t machSendInvalidRight = 0x10000004;
                const auto target = static_cast<std::uint32_t>(registers[0]);
                const auto name = static_cast<std::uint32_t>(registers[1]);
                std::uint64_t result = kernSuccess;
                if (target != guestMachTaskPort) {
                    result = machSendInvalidDest;
                } else if (name == 0 || name == std::numeric_limits<std::uint32_t>::max()) {
                    result = kernInvalidName;
                } else {
                    const auto right = guestReceiveRights.find(name);
                    if (right == guestReceiveRights.end()) {
                        result = kernInvalidName;
                    } else if (registers[2] != 0 ||
                               ((right->second.flags & MPO_CONTEXT_AS_GUARD) != 0 &&
                                right->second.context != registers[3])) {
                        result = machSendInvalidRight;
                    } else {
                        guestReceiveRights.erase(right);
                    }
                }
                registers[0] = result;
                registers[34] = 0;
                continue;
            }
            throw std::runtime_error("Guest reached unsupported Darwin syscall 0x" +
                hexadecimal(registers[16]) + " at PC 0x" + hexadecimal(registers[33]) +
                " (x0=0x" + hexadecimal(registers[0]) + ")");
        }
        throw std::runtime_error("Unknown guest process status " + std::to_string(status));
    }
}

}  // namespace arm64metal::process_runner
