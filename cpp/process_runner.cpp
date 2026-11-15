#include "guest_csops.hpp"
#include "guest_dlopen.hpp"
#include "host_service_registry.hpp"
#include "guest_exit.hpp"
#include "guest_mach_models.hpp"
#include "process_runner.hpp"

#include "image_loader.hpp"
#include "cache_residency.hpp"
#include "cache_backing_leases.hpp"
#include "darwin_bridge.hpp"
#include "guest_signal_runtime.hpp"
#include "guest_scheduler.hpp"
#include "guest_survey.hpp"
#include "guest_tlv.hpp"
#include "gpu_interpreter.hpp"
#include "metal_backend.hpp"
#include "process_decoder.hpp"
#include "process_host.hpp"
#include "process_msl.hpp"
#include "raw_proc_syscalls.hpp"
#include "translate_core.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <pthread.h>
#include <runetype.h>
#include <libproc.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/xattr.h>
#include "raw_fs_syscalls.hpp"
#include "dyld_api_abi.hpp"
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/event.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <mach/mach.h>
#include <mach/kern_return.h>
#include <mach/host_info.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>
#include <mach/clock.h>
#include <mach/ndr.h>
#include <mach/task_info.h>
#include <arpa/inet.h>
#include <tuple>
#include <thread>
#include <tuple>
#include <time.h>
#include <unordered_map>
#include <utility>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace arm64metal::process_runner {
namespace {

constexpr std::uint64_t kNoFault = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kQsortAdapterMarker = 0x51534f5254414249ULL;
constexpr std::uint32_t kQsortAdapterStatus = 17;
constexpr std::size_t kInstructionsPerDecode = 128;
constexpr std::size_t kMaximumDecodedInstructions = 131072;
constexpr std::uint64_t kMaximumDispatchBlocks = 8192;

class ScopedProcessUmask {
public:
    ScopedProcessUmask() = default;

    mode_t set(mode_t mask) {
        const auto previous = ::umask(mask);
        if (!originalMask_) originalMask_ = previous;
        return previous;
    }

    ~ScopedProcessUmask() {
        if (originalMask_) ::umask(*originalMask_);
    }

    ScopedProcessUmask(const ScopedProcessUmask&) = delete;
    ScopedProcessUmask& operator=(const ScopedProcessUmask&) = delete;

private:
    std::optional<mode_t> originalMask_;
};

std::uint32_t dyld_base_platform(std::uint32_t platform) {
    switch (platform) {
        case 6: return 2;
        case 7: return 2;
        case 8: return 3;
        case 9: return 4;
        case 12: return 11;
        default: return platform;
    }
}

std::uint32_t dyld_program_version(std::uint32_t platform, std::uint32_t version) {
    const auto basePlatform = dyld_base_platform(platform);
    if (basePlatform == 5) {  // bridgeOS versions are reported as iOS versions.
        return version + 0x00090000U;
    }
    if (basePlatform == 4 && (version >> 16U) < 26U) {  // watchOS, before 26.
        return version + 0x00070000U;
    }
    return version;
}

std::uint32_t dyld_program_sdk_version(const ExecutableImage& image) {
    if (!image.build_version) return 0;
    const auto& build = *image.build_version;
    return dyld_program_version(build.platform, build.sdk_load_command);
}

bool dyld_is_simulator_platform(std::uint32_t platform) {
    return platform == 7 || platform == 8 || platform == 9 || platform == 12;
}
// The pointer encoding stores a 1-based token index in the low 32 bits, shifted by 3.
constexpr std::size_t kGuestTokenCodecCapacity =
    static_cast<std::size_t>((std::uint64_t{0xffffffff} >> 3U));

std::optional<GuestBuildVersion> guest_image_build_version(
    std::uint64_t header, const ExecutableImage& image, const GuestMemory& memory) {
    try {
        const auto segment = std::find_if(
            image.mapped_image_segments.begin(), image.mapped_image_segments.end(),
            [header](const MappedImageSegment& candidate) {
                return candidate.guest_header == header && candidate.start >= 0 &&
                       candidate.end > candidate.start &&
                       static_cast<std::uint64_t>(candidate.start) <= header &&
                       header < static_cast<std::uint64_t>(candidate.end);
            });
        std::uint64_t segmentEnd = 0;
        if (image.image_paths.find(header) != image.image_paths.end()) {
            if (segment == image.mapped_image_segments.end()) return std::nullopt;
            segmentEnd = static_cast<std::uint64_t>(segment->end);
        } else {
            const auto readable = memory.contiguous_readable_size(header);
            if (readable > std::numeric_limits<std::uint64_t>::max() - header)
                return std::nullopt;
            segmentEnd = header + readable;
        }
        if (header > std::numeric_limits<std::uint64_t>::max() - 32 || header + 32 > segmentEnd)
            return std::nullopt;
        struct Header64 {
            std::uint32_t magic;
            std::uint32_t cpuType;
            std::uint32_t cpuSubtype;
            std::uint32_t fileType;
            std::uint32_t commandCount;
            std::uint32_t commandBytes;
            std::uint32_t flags;
            std::uint32_t reserved;
        } machHeader{};
        std::memcpy(&machHeader, memory.buffer(header, sizeof(machHeader)), sizeof(machHeader));
        if (machHeader.magic != 0xfeedfacfU || machHeader.commandBytes > 1024 * 1024 ||
            machHeader.commandCount > machHeader.commandBytes / 8 ||
            machHeader.commandBytes > segmentEnd - header - sizeof(machHeader))
            return std::nullopt;
        auto cursor = header + sizeof(machHeader);
        const auto commandsEnd = cursor + machHeader.commandBytes;
        std::optional<GuestBuildVersion> result;
        bool hasBuildVersion = false;
        for (std::uint32_t index = 0; index < machHeader.commandCount; ++index) {
            if (cursor > commandsEnd || commandsEnd - cursor < 8) return std::nullopt;
            std::uint32_t command[2]{};
            std::memcpy(command, memory.buffer(cursor, sizeof(command)), sizeof(command));
            const auto size = static_cast<std::uint64_t>(command[1]);
            if (size < 8 || (size & 3U) != 0 || size > commandsEnd - cursor) return std::nullopt;
            if (command[0] == 0x32U && size >= 24 && !hasBuildVersion) {
                std::uint32_t fields[3]{};
                std::memcpy(fields, memory.buffer(cursor + 8, sizeof(fields)), sizeof(fields));
                result = GuestBuildVersion{fields[0], fields[1], fields[2] == 0 ? fields[1] : fields[2]};
                hasBuildVersion = true;
            }
            if (command[0] == 0x24U && size >= 16 && !result) {
                std::uint32_t fields[2]{};
                std::memcpy(fields, memory.buffer(cursor + 8, sizeof(fields)), sizeof(fields));
                result = GuestBuildVersion{1, fields[0], fields[1] == 0 ? fields[0] : fields[1]};
            }
            cursor += size;
        }
        if (cursor != commandsEnd) return std::nullopt;
        return result;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

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
                      const std::set<std::int64_t>& qsortEntries,
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
                host.code.insert(host.code.begin(),
                    "if (x[0] < " + std::to_string(SharedCache::guestBase) + "ul) {");
                host.code.push_back("}");
                instruction.code.insert(instruction.code.begin(), host.code.begin(), host.code.end());
            } else if (import != imports.end()) {
                instruction = hostCall(import->second, std::nullopt, true);
            } else if (stub != guestStubs.end()) {
                instruction = {{"pc = " + std::to_string(stub->second) + "ul; break;"},
                               {stub->second}};
            } else if (qsortEntries.count(pc) != 0) {
                instruction = decode_process(image.instruction(pc), pc, imports, guestStubs, true);
                instruction.operation.reset();
                instruction.code.insert(instruction.code.begin(),
                    "if (x[2] > 1ul && "
                    "(x[3] & 0xffff000000000000ul) != 0xa64e000000000000ul && "
                    "x[35] != " + std::to_string(kQsortAdapterMarker) +
                    "ul) { x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | "
                    "(uint(c) << 3); x[33] = " + std::to_string(pc) + "ul; x[34] = " +
                    std::to_string(kQsortAdapterStatus) + "u; return; } x[35] = 0ul;");
            } else {
                instruction = decode_process(selectorWord ? *selectorWord : image.instruction(pc),
                                             pc, imports,
                                             guestStubs, true);
            }
        } catch (const std::exception& error) {
            const auto word = selectorWord ? *selectorWord : image.instruction(pc);
            if ((word & 0xffe0001fU) == 0xd4200000U) {
                // Process execution turns AArch64 BRK immediates into the
                // guest's synchronous SIGTRAP path. Keep decode_process unchanged so the raw
                // function translator continues to reject the instruction.
                instruction = {{"x[33] = " + std::to_string(pc) +
                                "ul; x[34] = 15; return;"}, {}};
            } else if ((word & 0xffe0001fU) == 0) {
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
        resident[entry + 3] = (region.writable ? 1U : 0U) |
            (region.readable ? 0U : 2U) | (region.tpro ? 4U : 0U);
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
    const std::set<std::int64_t>& qsortEntries,
    std::map<std::int64_t, std::string>& allDecodeErrors,
    std::int64_t entry,
    bool allowInterpreter) {
    ProcessKernelPlan plan;
    std::map<std::int64_t, std::string> decodeErrors;
    decode_reachable(image, imports, guestStubs, qsortEntries,
                     plan.instructions, decodeErrors, entry);
    allDecodeErrors.insert(decodeErrors.begin(), decodeErrors.end());

    if (std::any_of(qsortEntries.begin(), qsortEntries.end(), [&](std::int64_t qsortEntry) {
            return plan.instructions.find(qsortEntry) != plan.instructions.end();
        })) return plan;

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
            const bool authenticatedReturn =
                opcode == translate_core::Opcode::AuthenticatedReturn;
            if (authenticatedReturn &&
                (instruction.operation->destination > static_cast<std::uint32_t>(
                     translate_core::AuthenticatedReturnStep::Return) ||
                 instruction.operation->source != 0 ||
                 instruction.operation->immediate != 0 ||
                 instruction.operation->shift != 0 ||
                 instruction.operation->width != 0)) {
                break;
            }
            const bool authenticatedTerminal =
                authenticatedReturn && instruction.operation->destination ==
                    static_cast<std::uint32_t>(
                        translate_core::AuthenticatedReturnStep::Return);
            if (opcode == translate_core::Opcode::Return || authenticatedTerminal) {
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
            if (opcode == translate_core::Opcode::BranchLink) {
                const auto& operation = *instruction.operation;
                const auto target = static_cast<std::int64_t>(pc) +
                    static_cast<std::int32_t>(operation.immediate);
                if (!instruction.ends_block_at(pc) || instruction.next.size() != 2 ||
                    instruction.next[0] != target || instruction.next[1] != pc + 4 ||
                    target < 0 || instruction.next[1] < 0 ||
                    (operation.immediate & 3U) != 0 ||
                    static_cast<std::int32_t>(operation.immediate) < -(1 << 27) ||
                    static_cast<std::int32_t>(operation.immediate) > ((1 << 27) - 4) ||
                    operation.destination != 0 || operation.source != 0 ||
                    operation.shift != 0 || operation.width != 64) {
                    break;
                }
                block.push_back({pc, &instruction, true});
                complete = true;
                break;
            }
            if (opcode == translate_core::Opcode::BranchRegister) {
                const auto& operation = *instruction.operation;
                if (!instruction.ends_block_at(pc) || !instruction.next.empty() ||
                    operation.destination != 0 || operation.source > 31 ||
                    operation.immediate > 1 || operation.shift != 0 ||
                    operation.width != 64) {
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
            if (opcode == translate_core::Opcode::TestBitBranch) {
                const auto& operation = *instruction.operation;
                const auto target = static_cast<std::int64_t>(pc) +
                    static_cast<std::int32_t>(operation.shift);
                if (!instruction.ends_block_at(pc) || instruction.next.size() != 2 ||
                    instruction.next[0] != target || instruction.next[1] != pc + 4 ||
                    instruction.next[0] < 0 || instruction.next[1] < 0 ||
                    operation.destination >= operation.width || operation.source > 31 ||
                    operation.immediate > 1 ||
                    (operation.width != 32 && operation.width != 64)) {
                    break;
                }
                block.push_back({pc, &instruction, true});
                complete = true;
                break;
            }
            if (opcode == translate_core::Opcode::BranchCondition) {
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
            const bool authenticatedStep = authenticatedReturn &&
                instruction.operation->destination <= static_cast<std::uint32_t>(
                    translate_core::AuthenticatedReturnStep::AuthenticateLinkRegister);
            const bool unsignedMemoryOpcode =
                opcode == translate_core::Opcode::LoadUnsignedImmediate ||
                opcode == translate_core::Opcode::StoreUnsignedImmediate;
            const bool pairMemoryOpcode =
                opcode == translate_core::Opcode::LoadPair ||
                opcode == translate_core::Opcode::StorePair;
            const bool indexedScalarMemoryOpcode =
                opcode == translate_core::Opcode::LoadUnscaledWriteback ||
                opcode == translate_core::Opcode::StoreUnscaledWriteback;
            const bool signedScalarMemoryOpcode =
                opcode == translate_core::Opcode::LoadSignedScalar;
            const bool scalarLiteralMemoryOpcode =
                opcode == translate_core::Opcode::LoadLiteral;
            const bool vectorMemoryOpcode =
                opcode == translate_core::Opcode::LoadVectorMemory ||
                opcode == translate_core::Opcode::StoreVectorMemory;
            const bool unsignedMinMaxMemoryOpcode =
                opcode == translate_core::Opcode::LoadUnsignedMinMax;
            const bool memoryOpcode = unsignedMemoryOpcode || pairMemoryOpcode ||
                                      indexedScalarMemoryOpcode ||
                                      signedScalarMemoryOpcode ||
                                      scalarLiteralMemoryOpcode ||
                                      unsignedMinMaxMemoryOpcode ||
                                      vectorMemoryOpcode;
            const bool addSubImmediateOpcode =
                opcode == translate_core::Opcode::AddImmediate ||
                opcode == translate_core::Opcode::SubImmediate ||
                opcode == translate_core::Opcode::AddImmediateSetFlags ||
                opcode == translate_core::Opcode::SubImmediateSetFlags;
            const bool addSubShiftedRegisterOpcode =
                opcode == translate_core::Opcode::AddSubShiftedRegister;
            const bool addSubExtendedRegisterOpcode =
                opcode == translate_core::Opcode::AddSubExtendedRegister;
            const bool addSubCarryRegisterOpcode =
                opcode == translate_core::Opcode::AddSubCarryRegister;
            const bool logicalShiftedRegisterOpcode =
                opcode == translate_core::Opcode::LogicalShiftedRegister;
            const bool logicalImmediateOpcode =
                opcode == translate_core::Opcode::LogicalImmediate;
            const bool logicalVariableShiftOpcode =
                opcode == translate_core::Opcode::LogicalVariableShiftRegister;
            const bool multiplyAccumulateOpcode =
                opcode == translate_core::Opcode::IntegerMultiplyAccumulate;
            const bool bitfieldOpcode =
                opcode == translate_core::Opcode::Bitfield;
            const bool scalarFPOpcode =
                opcode == translate_core::Opcode::ScalarFloatingPoint;
            const bool integerBitOperationOpcode =
                opcode == translate_core::Opcode::IntegerBitOperation;
            const bool integerDivideOpcode =
                opcode == translate_core::Opcode::IntegerDivide;
            const bool conditionalSelectOpcode =
                opcode == translate_core::Opcode::ConditionalSelect;
            const bool adrOpcode = opcode == translate_core::Opcode::Adr;
            const bool vectorCompareEqualZeroOpcode =
                opcode == translate_core::Opcode::VectorCompareEqualZero;
            const bool vectorReduceExtremeOpcode =
                opcode == translate_core::Opcode::VectorReduceExtreme;
            const auto& operation = *instruction.operation;
            const bool validMemoryOperation =
                !unsignedMemoryOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 0xfff &&
                 (operation.width == 8 || operation.width == 16 ||
                  operation.width == 32 || operation.width == 64) &&
                 operation.shift == (operation.width == 8 ? 0U :
                    (operation.width == 16 ? 1U :
                     (operation.width == 32 ? 2U : 3U))));
            const auto elementBytes = operation.width / 8U;
            const auto pairOffset = static_cast<std::int32_t>(operation.immediate);
            const auto pairMode = operation.shift >> 5U;
            const auto pairSecond = operation.shift & 31U;
            const bool pairLoad = opcode == translate_core::Opcode::LoadPair;
            const bool validPairMemoryOperation =
                !pairMemoryOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 (operation.width == 32 || operation.width == 64) &&
                 (operation.shift & ~0x7fU) == 0 &&
                 pairMode >= 1 && pairMode <= 3 &&
                 pairOffset >= -64 * static_cast<std::int32_t>(elementBytes) &&
                 pairOffset <= 63 * static_cast<std::int32_t>(elementBytes) &&
                 pairOffset % static_cast<std::int32_t>(elementBytes) == 0 &&
                 !(pairLoad && operation.destination == pairSecond) &&
                 (operation.source == 31 || pairMode == 2 ||
                  (operation.source != operation.destination &&
                   operation.source != pairSecond)));
            const auto indexedScalarOffset =
                static_cast<std::int32_t>(operation.immediate);
            const bool validIndexedScalarMemoryOperation =
                !indexedScalarMemoryOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 (operation.width == 8 || operation.width == 16 ||
                  operation.width == 32 || operation.width == 64) &&
                 operation.shift <= 3 && operation.shift != 2 &&
                 indexedScalarOffset >= -256 && indexedScalarOffset <= 255 &&
                 (operation.shift == 0 || operation.source == 31 ||
                  operation.source != operation.destination));
            const auto signedScalarOffset =
                static_cast<std::int32_t>(operation.immediate);
            const auto signedScalarMode = operation.shift & 3U;
            const bool signedTargetIsX = (operation.shift & 4U) != 0;
            const auto signedAccessBytes = operation.width / 8U;
            const bool validSignedScalarMemoryOperation =
                !signedScalarMemoryOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 (operation.width == 8 || operation.width == 16 ||
                  operation.width == 32) &&
                 (operation.shift & ~7U) == 0 &&
                 (operation.width != 32 || signedTargetIsX) &&
                 (signedScalarMode == 0
                      ? (operation.immediate <= 0xfffU * signedAccessBytes &&
                         operation.immediate % signedAccessBytes == 0)
                      : signedScalarOffset >= -256 &&
                            signedScalarOffset <= 255) &&
                 (signedScalarMode < 2 || operation.source == 31 ||
                  operation.source != operation.destination));
            const auto literalDisplacement =
                static_cast<std::int32_t>(operation.immediate);
            const auto literalBytes = operation.shift == 1U ? 8U : 4U;
            const auto literalAddress = pc + literalDisplacement;
            constexpr std::uint64_t guestCodeLimit = std::uint64_t{1} << 48;
            const bool validScalarLiteralMemoryOperation =
                !scalarLiteralMemoryOpcode ||
                (operation.destination <= 31 && operation.source == 0 &&
                 operation.shift <= 2U &&
                 (operation.shift == 0U ? operation.width == 32U
                                        : operation.width == 64U) &&
                 literalDisplacement >= -(1 << 20) &&
                 literalDisplacement <= ((1 << 20) - 4) &&
                 (literalDisplacement & 3) == 0 && literalAddress >= 0 &&
                 static_cast<std::uint64_t>(literalAddress) < guestCodeLimit &&
                 literalBytes <= guestCodeLimit -
                     static_cast<std::uint64_t>(literalAddress));
            const auto vectorMode = operation.shift & 7U;
            const auto vectorDisplacement =
                static_cast<std::int32_t>(operation.immediate);
            const auto vectorBytes = operation.width / 8U;
            const auto vectorExtension = (operation.shift >> 8U) & 7U;
            const auto vectorScaled = (operation.shift >> 11U) & 1U;
            const bool vectorLiteral = vectorMode == static_cast<std::uint32_t>(
                translate_core::VectorMemoryMode::Literal);
            const bool vectorRegisterOffset = vectorMode == static_cast<std::uint32_t>(
                translate_core::VectorMemoryMode::RegisterOffset);
            const auto vectorLiteralAddress = pc + vectorDisplacement;
            const bool validVectorRegisterOffset =
                (operation.shift & ~0xfffU) == 0 &&
                (vectorExtension == 2U || vectorExtension == 3U ||
                 vectorExtension == 6U || vectorExtension == 7U) &&
                vectorScaled <= 1U && operation.immediate == 0U;
            const bool validVectorScaledImmediate =
                vectorDisplacement >= 0 && vectorBytes != 0 &&
                static_cast<std::uint32_t>(vectorDisplacement) <=
                    0xfffU * vectorBytes &&
                static_cast<std::uint32_t>(vectorDisplacement) %
                    vectorBytes == 0U;
            const bool validVectorWritebackDisplacement =
                vectorDisplacement >= -256 && vectorDisplacement <= 255;
            const bool validVectorLiteral =
                vectorBytes <= 16U &&
                vectorDisplacement >= -(1 << 20) &&
                vectorDisplacement <= ((1 << 20) - 4) &&
                (vectorDisplacement & 3) == 0 &&
                vectorLiteralAddress >= 0 &&
                static_cast<std::uint64_t>(vectorLiteralAddress) <
                    guestCodeLimit &&
                vectorBytes <= guestCodeLimit -
                    static_cast<std::uint64_t>(vectorLiteralAddress);
            const bool validVectorPayload = vectorRegisterOffset
                ? validVectorRegisterOffset
                : ((operation.shift & ~7U) == 0 &&
                   (!vectorLiteral ||
                    (opcode == translate_core::Opcode::LoadVectorMemory &&
                     operation.source == 0)) &&
                   (vectorMode != static_cast<std::uint32_t>(
                        translate_core::VectorMemoryMode::ScaledImmediate) ||
                    validVectorScaledImmediate) &&
                   ((vectorMode != static_cast<std::uint32_t>(
                         translate_core::VectorMemoryMode::Unscaled) &&
                     vectorMode != static_cast<std::uint32_t>(
                         translate_core::VectorMemoryMode::PostIndex) &&
                     vectorMode != static_cast<std::uint32_t>(
                         translate_core::VectorMemoryMode::PreIndex)) ||
                    validVectorWritebackDisplacement) &&
                   (!vectorLiteral || validVectorLiteral));
            const bool validVectorMemoryOperation =
                !vectorMemoryOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 (operation.width == 8 || operation.width == 16 ||
                  operation.width == 32 || operation.width == 64 ||
                  operation.width == 128) &&
                 validVectorPayload &&
                 ((operation.width >= 32U && vectorBytes <= 16U) ||
                  vectorRegisterOffset) &&
                 (vectorMode <= static_cast<std::uint32_t>(
                      translate_core::VectorMemoryMode::Literal)));
            const bool validUnsignedMinMaxMemoryOperation =
                !unsignedMinMaxMemoryOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 31 && operation.shift <= 1 &&
                 (operation.width == 32 || operation.width == 64));
            const bool validAddSubImmediate =
                !addSubImmediateOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 0xfff &&
                 (operation.width == 32 || operation.width == 64) &&
                 (operation.shift == 0 || operation.shift == 12));
            const bool validConditionalSelect =
                !conditionalSelectOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 31 && operation.shift <= 63 &&
                 (operation.width == 32 || operation.width == 64));
            const auto shiftedType = (operation.shift >> 6U) & 3U;
            const auto shiftedAmount = operation.shift & 63U;
            const bool validAddSubShiftedRegister =
                !addSubShiftedRegisterOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 31 &&
                 (operation.width == 32 || operation.width == 64) &&
                 shiftedType <= 2 && shiftedAmount < operation.width &&
                 (operation.shift & ~0x3ffU) == 0);
            const auto extendedAmount = operation.shift & 7U;
            const bool validAddSubExtendedRegister =
                !addSubExtendedRegisterOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 31 &&
                 (operation.width == 32 || operation.width == 64) &&
                 extendedAmount <= 4 && (operation.shift & ~0xffU) == 0);
            const bool validAddSubCarryRegister =
                !addSubCarryRegisterOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 31 &&
                 (operation.width == 32 || operation.width == 64) &&
                 (operation.shift & ~3U) == 0);
            const auto logicalAmount = (operation.shift >> 5U) & 63U;
            const auto logicalShiftType = (operation.shift >> 3U) & 3U;
            const bool validLogicalShiftedRegister =
                !logicalShiftedRegisterOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 31 &&
                 (operation.width == 32 || operation.width == 64) &&
                 logicalShiftType <= 3 && logicalAmount < operation.width &&
                 (operation.shift & ~0x7ffU) == 0);
            const bool validLogicalImmediate =
                !logicalImmediateOpcode ||
                (operation.destination <= 31 && operation.source <= 127 &&
                 (operation.width == 32 || operation.width == 64) &&
                 (operation.width != 32 || operation.shift == 0));
            const bool validLogicalVariableShift =
                !logicalVariableShiftOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 31 &&
                 (operation.width == 32 || operation.width == 64) &&
                 operation.shift <= 3);
            const bool validMultiplyAccumulate =
                !multiplyAccumulateOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 31 && operation.shift <= 63 &&
                 (operation.width == 32 || operation.width == 64));
            const auto bitfieldVariant = operation.shift >> 6U;
            const auto bitfieldEnd = operation.shift & 63U;
            const bool validBitfield =
                !bitfieldOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 (operation.width == 32 || operation.width == 64) &&
                 operation.immediate < operation.width &&
                 bitfieldEnd < operation.width && bitfieldVariant <= 2U &&
                 (operation.shift & ~0xffU) == 0);
            const auto scalarFPKind = operation.shift >> 2U;
            const auto scalarFPMaxImmediate = scalarFPKind == 1U ? 255U :
                                              scalarFPKind == 4U ? 31U : 0U;
            const bool validScalarFP =
                !scalarFPOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= scalarFPMaxImmediate &&
                 (operation.width == 32 || operation.width == 64) &&
                 (scalarFPKind == 0U ? operation.shift <= 3U :
                  scalarFPKind == 1U ? operation.shift == 4U :
                  scalarFPKind == 2U ? operation.shift == 8U :
                  scalarFPKind == 3U ? operation.shift == 12U :
                  scalarFPKind == 4U ?
                      (operation.shift >= 16U && operation.shift <= 19U) :
                  scalarFPKind == 5U ?
                      (operation.shift == 20U && operation.width == 64U) : false) &&
                 ((scalarFPKind == 1U || scalarFPKind == 4U) ||
                  operation.immediate == 0U) &&
                 (scalarFPKind != 4U || (operation.shift & 2U) == 0U ||
                  operation.immediate == 0U));
            const bool validIntegerBitOperation =
                !integerBitOperationOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 3U && operation.shift == 0 &&
                 (operation.width == 32 || operation.width == 64));
            const bool validIntegerDivide =
                !integerDivideOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 operation.immediate <= 31U && operation.shift <= 1U &&
                 (operation.width == 32 || operation.width == 64));
            const bool validAdr =
                !adrOpcode ||
                (operation.destination <= 31 && operation.source <= 1 &&
                 operation.immediate <= 0x1fffffU && operation.shift == 0 &&
                 operation.width == 64);
            const bool validVectorCompareEqualZero =
                !vectorCompareEqualZeroOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 (operation.immediate == 8U || operation.immediate == 16U ||
                  operation.immediate == 32U || operation.immediate == 64U) &&
                 operation.shift == 0U &&
                 (operation.width == 64U || operation.width == 128U) &&
                 (operation.immediate != 64U || operation.width == 128U));
            const bool validVectorReduceExtreme =
                !vectorReduceExtremeOpcode ||
                (operation.destination <= 31 && operation.source <= 31 &&
                 (operation.immediate == 8U || operation.immediate == 16U ||
                  operation.immediate == 32U) && operation.shift <= 3U &&
                 (operation.width == 64U || operation.width == 128U) &&
                 (operation.immediate != 32U || operation.width == 128U));
            if ((!moveWideOpcode && !authenticatedStep && !memoryOpcode && !addSubImmediateOpcode &&
                 !conditionalSelectOpcode && !addSubShiftedRegisterOpcode &&
                 !addSubExtendedRegisterOpcode && !addSubCarryRegisterOpcode &&
                 !logicalShiftedRegisterOpcode && !logicalImmediateOpcode &&
                 !logicalVariableShiftOpcode && !multiplyAccumulateOpcode &&
                 !bitfieldOpcode && !scalarFPOpcode &&
                 !integerBitOperationOpcode && !integerDivideOpcode &&
                 !adrOpcode && !vectorCompareEqualZeroOpcode &&
                 !vectorReduceExtremeOpcode) ||
                !validMemoryOperation || !validPairMemoryOperation ||
                !validIndexedScalarMemoryOperation ||
                !validSignedScalarMemoryOperation ||
                !validScalarLiteralMemoryOperation ||
                !validVectorMemoryOperation ||
                !validUnsignedMinMaxMemoryOperation ||
                !validAddSubImmediate || !validConditionalSelect ||
                !validAddSubShiftedRegister || !validAddSubExtendedRegister ||
                !validAddSubCarryRegister ||
                !validLogicalShiftedRegister || !validLogicalImmediate ||
                !validLogicalVariableShift || !validMultiplyAccumulate ||
                !validBitfield || !validScalarFP ||
                !validIntegerBitOperation || !validIntegerDivide || !validAdr ||
                !validVectorCompareEqualZero || !validVectorReduceExtreme ||
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
    constexpr std::uint64_t dataTokenBuckets = 255;
    constexpr std::uint64_t tokenIndexShift = 3;
    if (key == 2 && address <= addressMask) {
        const auto code = static_cast<std::uint64_t>(index) % dataTokenBuckets + 1;
        return (code << 47U) | address;
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

std::uint64_t append_guest_pointer_token(ProcessBootstrapResult& bootstrap,
                                         std::uint64_t address,
                                         std::uint64_t key,
                                         std::uint64_t modifier) {
    auto& registers = bootstrap.registers;
    const auto index = static_cast<std::size_t>(registers[guestTokenCount]);
    const auto capacity = static_cast<std::size_t>(registers[guestTokenCapacity]);
    if (index >= capacity) throw std::runtime_error("Guest authentication table is full");
    const auto slot = guestTokenStart + 3 * index;
    registers[slot] = address;
    registers[slot + 1] = modifier;
    registers[slot + 2] = key;
    registers[guestTokenCount] = index + 1;
    return encode_guest_pointer(address, key, index);
}

std::optional<std::size_t> find_guest_pointer_token(
    const std::vector<std::uint64_t>& registers,
    std::uint64_t address,
    std::uint64_t key,
    std::uint64_t modifier) {
    if (registers.size() < guestTokenStart || registers.size() <= guestTokenCapacity) {
        throw std::runtime_error("Guest authentication table is full");
    }
    const auto count = static_cast<std::size_t>(registers[guestTokenCount]);
    const auto capacity = static_cast<std::size_t>(registers[guestTokenCapacity]);
    if (count > capacity || capacity > (registers.size() - guestTokenStart) / 3) {
        throw std::runtime_error("Guest authentication table bounds are invalid");
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto slot = guestTokenStart + 3 * index;
        if (registers[slot] == address && registers[slot + 1] == modifier &&
            registers[slot + 2] == key) {
            return index;
        }
    }
    return std::nullopt;
}

std::uint64_t append_guest_pointer_token(std::vector<std::uint64_t>& registers,
                                        std::uint64_t address,
                                        std::uint64_t key,
                                        std::uint64_t modifier) {
    if (const auto existing = find_guest_pointer_token(registers, address, key, modifier)) {
        return encode_guest_pointer(address, key, *existing);
    }
    const auto count = static_cast<std::size_t>(registers[guestTokenCount]);
    const auto capacity = static_cast<std::size_t>(registers[guestTokenCapacity]);
    if (count >= capacity) throw std::runtime_error("Guest authentication table is full");
    const auto slot = guestTokenStart + 3 * count;
    registers[slot] = address;
    registers[slot + 1] = modifier;
    registers[slot + 2] = key;
    registers[guestTokenCount] = count + 1;
    return encode_guest_pointer(address, key, count);
}

struct DyldApisFacade {
    std::uint64_t object = 0;
    std::uint64_t initializeService = 0;
    std::int64_t publicationCell = 0;
    bool publicationSeeded = false;
};

DyldApisFacade initialize_dyld_apis(
    const ExecutableImage& image, ProcessBootstrapResult& bootstrap,
    std::map<std::int64_t, std::string>& imports,
    const std::map<std::int64_t, std::int64_t>& guestStubs) {
    if (!image.cache_reader) return {};
    const auto abi = derive_dyld_api_abi(*image.cache_reader);
    const auto cell = image.cache_reader->dyldApisCell();
    if (!cell) throw std::runtime_error("Cached libdyld has no __dyld_apis publication cell");
    const auto cellOffset = memory_backing_offset(image,
        static_cast<std::uint64_t>(cell->address), cell->size, bootstrap.memory.size());
    const bool deferred = std::any_of(image.deferred_cache.begin(), image.deferred_cache.end(),
        [&](const DeferredCacheSegment& segment) {
            if (segment.logical_start < 0 || segment.logical_end < segment.logical_start) return false;
            const auto begin = static_cast<std::uint64_t>(segment.logical_start);
            const auto end = static_cast<std::uint64_t>(segment.logical_end);
            const auto address = static_cast<std::uint64_t>(cell->address);
            return address >= begin && address <= end && cell->size <= end - address;
        });
    if (!cellOffset && !deferred) {
        throw std::runtime_error("Cached __dyld_apis cell is neither resident nor deferred");
    }
    const auto rawCell = image.cache_reader->bytes(
        image.cache_reader->originalAddress(cell->address), cell->size);
    if (std::any_of(rawCell.begin(), rawCell.end(), [](std::uint8_t value) { return value != 0; }) ||
        (cellOffset && load_u64(bootstrap.memory, *cellOffset) != 0)) {
        throw std::runtime_error("Cached __dyld_apis cell is already populated");
    }
    const auto method = image.cache_reader->resolveExport(
        "/usr/lib/system/libdyld.dylib", "__dyld_call_with_writable_tpro_memory");
    if (!method || method->guest_address < 0) {
        throw std::runtime_error("Cached libdyld writable-TPRO method is unavailable");
    }
    constexpr std::uint64_t serviceBase = 0x80000000ULL;
    auto serviceCursor = serviceBase;
    const auto addSyntheticService = [&](const char* symbol) {
        while (serviceCursor <= static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max()) - 4 &&
               (imports.find(static_cast<std::int64_t>(serviceCursor)) != imports.end() ||
                guestStubs.find(static_cast<std::int64_t>(serviceCursor)) != guestStubs.end())) {
            serviceCursor += 4;
        }
        if (serviceCursor >= guestCodeLimit || serviceCursor >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - 4) {
            throw std::runtime_error("Synthetic libdyld service address space exhausted");
        }
        const auto address = serviceCursor;
        serviceCursor += 4;
        imports.emplace(static_cast<std::int64_t>(address), symbol);
        return address;
    };
    const auto initializeService = addSyntheticService("__guest_libdyld_initialize");
    const auto basePlatformService = addSyntheticService("__guest_dyld_get_base_platform");
    const auto activePlatformService = addSyntheticService("__guest_dyld_get_active_platform");
    const auto simulatorPlatformService = addSyntheticService(
        "__guest_dyld_is_simulator_platform");
    const auto bulkImageService = addSyntheticService(
        "__dyld_register_for_bulk_image_loads");
    const auto sharedCacheRealPathService = addSyntheticService(
        "__guest_dyld_shared_cache_real_path");
    const auto sharedCacheContainsPathService = addSyntheticService(
        "__guest_dyld_shared_cache_contains_path");
    const auto dlopenFromService = addSyntheticService(
        "__guest_dlopen_from_facade");
    const auto programSdkVersionService = addSyntheticService(
        "__guest_dyld_get_program_sdk_version");
    const auto programMinOsVersionService = addSyntheticService(
        "__guest_dyld_get_program_min_os_version");

    constexpr std::uint64_t objectBytes = 16;
    const std::uint64_t vtableBytes = abi.vtableBytes;
    const std::uint64_t reservation = objectBytes + vtableBytes;
    const auto object = bootstrap.heap.begin;
    if (reservation > bootstrap.heap.end - bootstrap.heap.begin ||
        object > bootstrap.memory.size() || reservation > bootstrap.memory.size() - object) {
        throw std::runtime_error("Guest heap cannot hold the libdyld API façade");
    }
    const auto vtable = object + objectBytes;
    constexpr std::uint64_t addressMask = 0x0000ffffffffffffULL;
    const auto vptrModifier = (object & addressMask) | (abi.vptrDiscriminator << 48);
    store_u64(bootstrap.memory, object,
              append_guest_pointer_token(bootstrap, vtable, 2, vptrModifier));
    const auto methodSlot = vtable + abi.writableTpro.offset;
    const auto methodModifier = (methodSlot & addressMask) | (abi.writableTpro.discriminator << 48);
    store_u64(bootstrap.memory, methodSlot,
              append_guest_pointer_token(bootstrap,
                  static_cast<std::uint64_t>(method->guest_address), 0, methodModifier));
    const auto initializeSlot = vtable + abi.initialize.offset;
    const auto initializeModifier = (initializeSlot & addressMask) | (abi.initialize.discriminator << 48);
    const auto basePlatformSlot = vtable + abi.basePlatform.offset;
    const auto basePlatformModifier = (basePlatformSlot & addressMask) | (abi.basePlatform.discriminator << 48);
    const auto activePlatformSlot = vtable + abi.activePlatform.offset;
    const auto activePlatformModifier = (activePlatformSlot & addressMask) | (abi.activePlatform.discriminator << 48);
    const auto simulatorPlatformSlot = vtable + abi.simulatorPlatform.offset;
    const auto simulatorPlatformModifier =
        (simulatorPlatformSlot & addressMask) | (abi.simulatorPlatform.discriminator << 48);
    const auto bulkImageSlot = vtable + abi.bulkImageLoads.offset;
    const auto bulkImageModifier = (bulkImageSlot & addressMask) | (abi.bulkImageLoads.discriminator << 48);
    const auto sharedCacheRealPathSlot = vtable + abi.sharedCacheRealPath.offset;
    const auto sharedCacheRealPathModifier =
        (sharedCacheRealPathSlot & addressMask) | (abi.sharedCacheRealPath.discriminator << 48);
    const auto sharedCacheContainsPathSlot = vtable + abi.sharedCacheContainsPath.offset;
    const auto sharedCacheContainsPathModifier =
        (sharedCacheContainsPathSlot & addressMask) | (abi.sharedCacheContainsPath.discriminator << 48);
    const auto dlopenFromSlot = vtable + abi.dlopenFrom.offset;
    const auto dlopenFromModifier = (dlopenFromSlot & addressMask) |
                                    (abi.dlopenFrom.discriminator << 48);
    const auto programSdkVersionSlot = vtable + abi.programSdkVersion.offset;
    const auto programSdkVersionModifier = (programSdkVersionSlot & addressMask) |
                                           (abi.programSdkVersion.discriminator << 48);
    store_u64(bootstrap.memory, basePlatformSlot,
              append_guest_pointer_token(bootstrap, basePlatformService, 0,
                                         basePlatformModifier));
    store_u64(bootstrap.memory, activePlatformSlot,
              append_guest_pointer_token(bootstrap, activePlatformService, 0,
                                         activePlatformModifier));
    store_u64(bootstrap.memory, simulatorPlatformSlot,
              append_guest_pointer_token(bootstrap, simulatorPlatformService, 0,
                                         simulatorPlatformModifier));
    store_u64(bootstrap.memory, bulkImageSlot,
              append_guest_pointer_token(bootstrap, bulkImageService, 0,
                                         bulkImageModifier));
    store_u64(bootstrap.memory, sharedCacheRealPathSlot,
              append_guest_pointer_token(bootstrap, sharedCacheRealPathService, 0,
                                         sharedCacheRealPathModifier));
    store_u64(bootstrap.memory, sharedCacheContainsPathSlot,
              append_guest_pointer_token(bootstrap, sharedCacheContainsPathService, 0,
                                         sharedCacheContainsPathModifier));
    store_u64(bootstrap.memory, dlopenFromSlot,
              append_guest_pointer_token(bootstrap, dlopenFromService, 0,
                                         dlopenFromModifier));
    store_u64(bootstrap.memory, programSdkVersionSlot,
              append_guest_pointer_token(bootstrap, programSdkVersionService, 0,
                                         programSdkVersionModifier));
    const auto programMinOsVersionSlot = vtable + abi.programMinOsVersion.offset;
    const auto programMinOsVersionModifier =
        (programMinOsVersionSlot & addressMask) | (abi.programMinOsVersion.discriminator << 48);
    store_u64(bootstrap.memory, programMinOsVersionSlot,
              append_guest_pointer_token(bootstrap, programMinOsVersionService, 0,
                                         programMinOsVersionModifier));
    store_u64(bootstrap.memory, initializeSlot,
              append_guest_pointer_token(bootstrap, initializeService, 0, initializeModifier));
    bootstrap.readOnlyRanges.push_back({object, object + reservation});
    if (cellOffset) store_u64(bootstrap.memory, *cellOffset, object);
    bootstrap.heap.begin += reservation;
    return {object, initializeService, cell->address, cellOffset.has_value()};
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
    for (std::size_t index = 0; index < 5; ++index) registers[index] = bootstrap.registers[index];
    registers[30] = guestReturnSentinel;
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

std::optional<std::string> guest_unix_socket_path(const GuestMemory& memory,
        std::uint64_t address, std::uint64_t length, std::uint32_t& error) {
    const auto pathOffset = offsetof(sockaddr_un, sun_path);
    if (address == 0 || length < pathOffset || length > sizeof(sockaddr_un)) {
        error = EINVAL;
        return std::nullopt;
    }
    if (!memory.is_guest_readable(address, length)) {
        error = EFAULT;
        return std::nullopt;
    }
    try {
        const auto* bytes = memory.buffer(address, static_cast<std::size_t>(length));
        if (static_cast<unsigned char>(bytes[1]) != AF_UNIX) {
            error = EAFNOSUPPORT;
            return std::nullopt;
        }
        const auto pathLength = static_cast<std::size_t>(length) - pathOffset;
        const auto* path = bytes + pathOffset;
        const auto terminator = std::find(path, path + pathLength, '\0');
        const auto nameLength = static_cast<std::size_t>(terminator - path);
        if (nameLength == 0) {
            error = EINVAL;
            return std::nullopt;
        }
        try {
            error = 0;
            return std::string(reinterpret_cast<const char*>(path), nameLength);
        } catch (const std::bad_alloc&) {
            error = ENOMEM;
            return std::nullopt;
        }
    } catch (const std::out_of_range&) {
        error = EFAULT;
        return std::nullopt;
    }
}

bool copy_guest_unix_datagram(const HostServiceState::GuestUnixDatagram& datagram,
        GuestMemory& memory, std::uint64_t bufferAddress, std::uint64_t bufferCapacity,
        std::uint64_t sourceAddress, std::uint64_t sourceLengthAddress,
        std::uint64_t& received, std::uint32_t& error) {
    received = std::min<std::uint64_t>(bufferCapacity, datagram.payload.size());
    std::uint8_t* payloadOutput = nullptr;
    std::uint8_t* sourceOutput = nullptr;
    std::uint8_t* sourceLengthOutput = nullptr;
    std::uint32_t sourceCapacity = 0;
    if (received != 0) {
        if (!memory.is_guest_readable(bufferAddress, received)) {
            error = EFAULT;
            return false;
        }
        try {
            payloadOutput = memory.buffer(bufferAddress, received, true);
        } catch (const std::out_of_range&) {
            error = EFAULT;
            return false;
        }
    }
    if (sourceAddress != 0) {
        if (sourceLengthAddress == 0 || !memory.is_guest_readable(sourceLengthAddress,
                sizeof(sourceCapacity))) {
            error = EFAULT;
            return false;
        }
        try {
            const auto* sourceLengthInput = memory.buffer(sourceLengthAddress,
                sizeof(sourceCapacity));
            std::memcpy(&sourceCapacity, sourceLengthInput, sizeof(sourceCapacity));
            sourceLengthOutput = memory.buffer(sourceLengthAddress,
                sizeof(sourceCapacity), true);
            const auto sourceBytes = std::min<std::uint64_t>(sourceCapacity,
                datagram.sourceLength);
            if (sourceBytes != 0) {
                if (!memory.is_guest_readable(sourceAddress, sourceBytes)) {
                    error = EFAULT;
                    return false;
                }
                sourceOutput = memory.buffer(sourceAddress, sourceBytes, true);
            }
        } catch (const std::out_of_range&) {
            error = EFAULT;
            return false;
        }
    }

    sockaddr_un source{};
    source.sun_len = static_cast<unsigned char>(datagram.sourceLength);
    source.sun_family = AF_UNIX;
    if (datagram.sourcePath) {
        const auto pathBytes = std::min(datagram.sourcePath->size(), sizeof(source.sun_path) - 1);
        std::memcpy(source.sun_path, datagram.sourcePath->data(), pathBytes);
        source.sun_path[pathBytes] = '\0';
    }
    if (payloadOutput != nullptr && received != 0) {
        std::memcpy(payloadOutput, datagram.payload.data(), static_cast<std::size_t>(received));
    }
    if (sourceOutput != nullptr) {
        const auto sourceBytes = std::min<std::uint64_t>(sourceCapacity,
            datagram.sourceLength);
        std::memcpy(sourceOutput, &source, static_cast<std::size_t>(sourceBytes));
    }
    if (sourceLengthOutput != nullptr) {
        const auto actualLength = datagram.sourceLength;
        std::memcpy(sourceLengthOutput, &actualLength, sizeof(actualLength));
    }
    error = 0;
    return true;
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
        const char* packetTrace =
            std::getenv("ARM64_METAL_TRACE_INTERPRETER_PACKETS");
        packetTraceEnabled_ = enabled_ && packetTrace != nullptr &&
                              std::strcmp(packetTrace, "1") == 0;
        periodicReportLimit_ = parsePeriodicReportLimit(
            std::getenv("ARM64_METAL_TRACE_PROGRESS_LIMIT"));
    }

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    void event(const char* name, std::uint64_t pc, std::uint64_t detail = 0) {
        if (!enabled_) return;
        if (std::strcmp(name, "compile-start") == 0) {
            increment(compileStarts_);
        } else if (std::strcmp(name, "compile-done") == 0) {
            increment(compileCompletions_);
            addSaturating(compileMilliseconds_, detail);
        }
        const auto now = std::chrono::steady_clock::now();
        if (events_ == maximumEvents) {
            if (!limitReported_) {
                std::cerr << "arm64-metal trace event-limit=" << maximumEvents
                          << " progress-limit=" << periodicReportLimit_ << '\n';
                limitReported_ = true;
                lastPeriodic_ = now;
                return;
            }
            if (periodicEvents_ == periodicReportLimit_ ||
                now - lastPeriodic_ < periodicInterval) return;
            ++periodicEvents_;
            lastPeriodic_ = now;
            write("progress", name, pc, detail, now);
            return;
        }
        ++events_;
        write("event", name, pc, detail, now);
    }

    void recordDispatch() noexcept {
        if (enabled_) increment(dispatches_);
    }

    void interpreterPacket(std::uint64_t pc, std::uint64_t opcode) {
        if (!packetTraceEnabled_) return;
        if (packetEvents_ == maximumPacketEvents) {
            if (!packetLimitReported_) {
                std::cerr << "arm64-metal trace interpreter-packet-limit="
                          << maximumPacketEvents << '\n';
                packetLimitReported_ = true;
            }
            return;
        }
        ++packetEvents_;
        write("event", "interpreter-packet", pc, opcode,
              std::chrono::steady_clock::now());
    }

private:
    static constexpr std::size_t maximumEvents = 128;
    static constexpr std::size_t maximumPacketEvents = 4096;
    static constexpr std::size_t defaultPeriodicReportLimit = 16;
    static constexpr std::size_t maximumPeriodicReportLimit = 4096;
    static constexpr auto periodicInterval = std::chrono::seconds(10);

    static std::size_t parsePeriodicReportLimit(const char* text) noexcept {
        if (text == nullptr || text[0] == '\0') return defaultPeriodicReportLimit;
        std::size_t value = 0;
        for (const char* digit = text; *digit != '\0'; ++digit) {
            if (*digit < '0' || *digit > '9') return defaultPeriodicReportLimit;
            const auto next = static_cast<std::size_t>(*digit - '0');
            if (value > (maximumPeriodicReportLimit - next) / 10) {
                return maximumPeriodicReportLimit;
            }
            value = value * 10 + next;
            if (value > maximumPeriodicReportLimit) return maximumPeriodicReportLimit;
        }
        return value;
    }

    static void increment(std::uint64_t& value) noexcept {
        if (value < std::numeric_limits<std::uint64_t>::max()) ++value;
    }

    static void addSaturating(std::uint64_t& total, std::uint64_t value) noexcept {
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        total = value > maximum - total ? maximum : total + value;
    }

    void write(const char* kind, const char* name, std::uint64_t pc,
               std::uint64_t detail, std::chrono::steady_clock::time_point now) const {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - started_).count();
        std::cerr << "arm64-metal trace +" << elapsed << "ms " << kind << '=' << name
                  << " pc=0x" << hexadecimal(pc) << " detail=" << detail;
        if (std::strcmp(kind, "progress") == 0) {
            std::cerr << " compile-starts=" << compileStarts_
                      << " compile-done=" << compileCompletions_
                      << " compile-ms=" << compileMilliseconds_
                      << " dispatches=" << dispatches_;
        }
        std::cerr << '\n';
    }

    bool enabled_ = false;
    bool packetTraceEnabled_ = false;
    bool limitReported_ = false;
    std::size_t events_ = 0;
    std::size_t packetEvents_ = 0;
    bool packetLimitReported_ = false;
    std::uint64_t compileStarts_ = 0;
    std::uint64_t compileCompletions_ = 0;
    std::uint64_t compileMilliseconds_ = 0;
    std::uint64_t dispatches_ = 0;
    std::size_t periodicReportLimit_ = defaultPeriodicReportLimit;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastPeriodic_ = started_;
    std::size_t periodicEvents_ = 0;
};

class SchedulerTrace {
public:
    SchedulerTrace() {
        const char* enabled = std::getenv("ARM64_METAL_TRACE_SCHEDULER");
        enabled_ = enabled != nullptr && std::strcmp(enabled, "1") == 0;
    }

    void timeslice(std::uint64_t from, std::uint64_t to, std::uint64_t pc,
                   std::uint64_t status, std::uint64_t dispatches) {
        if (!enabled_) return;
        if (events_ == maximumEvents) {
            if (!limitReported_) {
                std::cerr << "arm64-metal scheduler-trace event-limit=" << maximumEvents << '\n';
                limitReported_ = true;
            }
            return;
        }
        ++events_;
        std::cerr << "arm64-metal scheduler-trace event=timeslice from=" << from
                  << " to=" << to << " pc=0x" << hexadecimal(pc)
                  << " status=" << status << " dispatches=" << dispatches << '\n';
    }

private:
    static constexpr std::size_t maximumEvents = 64;
    bool enabled_ = false;
    bool limitReported_ = false;
    std::size_t events_ = 0;
};

class WorkqueueTrace {
public:
    WorkqueueTrace() {
        const char* enabled = std::getenv("ARM64_METAL_TRACE_WORKQUEUE");
        enabled_ = enabled != nullptr && std::strcmp(enabled, "1") == 0;
    }

    void event(const char* name, std::uint64_t operation, std::uint64_t pc,
               std::uint64_t thread, std::uint64_t related = 0,
               std::uint64_t detail = 0) {
        if (!enabled_) return;
        std::cerr << "arm64-metal workqueue-trace event=" << name
                  << " syscall=368 operation=0x" << hexadecimal(operation)
                  << " pc=0x" << hexadecimal(pc)
                  << " thread=" << thread << " related=" << related
                  << " detail=0x" << hexadecimal(detail) << '\n';
    }

    void open(std::uint64_t pc) {
        if (!enabled_) return;
        std::cerr << "arm64-metal workqueue-trace event=open syscall=367 pc=0x"
                  << hexadecimal(pc) << '\n';
    }

    void dispatch_config(std::uint64_t pc) {
        if (!enabled_) return;
        std::cerr << "arm64-metal workqueue-trace event=dispatch-config syscall=368 pc=0x"
                  << hexadecimal(pc) << '\n';
    }

private:
    bool enabled_ = false;
};

class DyldBootstrapTrace {
public:
    DyldBootstrapTrace() {
        const char* enabled = std::getenv("ARM64_METAL_TRACE_DYLD_BOOTSTRAP");
        enabled_ = enabled != nullptr && std::strcmp(enabled, "1") == 0;
    }

    void event(const char* name, std::uint64_t detail = 0) {
        if (!enabled_) return;
        if (events_ == maximumEvents) {
            if (!limitReported_) {
                std::cerr << "arm64-metal dyld-bootstrap event-limit=" << maximumEvents << '\n';
                limitReported_ = true;
            }
            return;
        }
        ++events_;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_).count();
        std::cerr << "arm64-metal dyld-bootstrap +" << elapsed << "ms event=" << name;
        if (detail != 0) std::cerr << " detail=" << detail;
        std::cerr << '\n';
    }

    void metadata(const char* name, const std::string& path,
                  const char* format, const char* descriptor) {
        if (!enabled_) return;
        if (events_ == maximumEvents) {
            if (!limitReported_) {
                std::cerr << "arm64-metal dyld-bootstrap event-limit=" << maximumEvents << '\n';
                limitReported_ = true;
            }
            return;
        }
        ++events_;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_).count();
        std::cerr << "arm64-metal dyld-bootstrap +" << elapsed << "ms event=" << name
                  << " image=";
        for (std::size_t index = 0; index < path.size() && index < 192; ++index) {
            const auto value = static_cast<unsigned char>(path[index]);
            std::cerr << ((value >= 0x20 && value <= 0x7e && value != ' ')
                              ? static_cast<char>(value) : '_');
        }
        std::cerr << " format=" << format << " descriptor=" << descriptor << '\n';
    }

private:
    static constexpr std::size_t maximumEvents = 16;
    bool enabled_ = false;
    bool limitReported_ = false;
    std::size_t events_ = 0;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

class ProcessPerformanceSummary {
public:
    ProcessPerformanceSummary()
        : started_(std::chrono::steady_clock::now()) {
        const char* enabled = std::getenv("ARM64_METAL_PERF_SUMMARY");
        enabled_ = enabled != nullptr && std::strcmp(enabled, "1") == 0;
    }

    ~ProcessPerformanceSummary() {
        if (!enabled_) return;
        const auto wallSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started_).count();
        std::fprintf(stderr,
            "arm64-metal perf wall_s=%.6f dispatches=%llu "
            "dispatch_cpu_inclusive_s=%.6f gpu_timestamp_s=%.6f "
            "gpu_timestamp_dispatches=%llu/%llu compile_count=%llu "
            "compile_s=%.6f host_service_calls=%llu "
            "host_service_s=%.6f host_service_scope=central_dispatcher\n",
            wallSeconds,
            static_cast<unsigned long long>(dispatchCount_),
            dispatchCpuSeconds_, gpuTimestampSeconds_,
            static_cast<unsigned long long>(gpuTimestampDispatchCount_),
            static_cast<unsigned long long>(dispatchCount_),
            static_cast<unsigned long long>(compileCount_), compileSeconds_,
            static_cast<unsigned long long>(hostServiceCount_), hostServiceSeconds_);
    }

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    void recordDispatch(const DispatchTiming& timing) noexcept {
        if (!enabled_) return;
        ++dispatchCount_;
        dispatchCpuSeconds_ += timing.cpuSeconds;
        if (timing.gpuSeconds > 0.0) {
            ++gpuTimestampDispatchCount_;
            gpuTimestampSeconds_ += timing.gpuSeconds;
        }
    }

    void recordCompile(double seconds) noexcept {
        if (!enabled_) return;
        ++compileCount_;
        compileSeconds_ += seconds;
    }

    void recordHostService(double seconds) noexcept {
        if (!enabled_) return;
        ++hostServiceCount_;
        hostServiceSeconds_ += seconds;
    }

private:
    std::chrono::steady_clock::time_point started_;
    bool enabled_ = false;
    std::uint64_t dispatchCount_ = 0;
    std::uint64_t gpuTimestampDispatchCount_ = 0;
    std::uint64_t compileCount_ = 0;
    std::uint64_t hostServiceCount_ = 0;
    double dispatchCpuSeconds_ = 0.0;
    double gpuTimestampSeconds_ = 0.0;
    double compileSeconds_ = 0.0;
    double hostServiceSeconds_ = 0.0;
};

class JitExecutionTrace {
public:
    JitExecutionTrace() {
        const char* enabled = std::getenv("ARM64_METAL_TRACE_JIT");
        enabled_ = enabled != nullptr && std::strcmp(enabled, "1") == 0;
    }

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    void dispatch(std::uint64_t pc, std::uint64_t ownerBegin,
                  std::uint64_t ownerEnd, std::size_t backing,
                  std::uint64_t generation, bool mapJit,
                  bool usesInterpreter) {
        if (!enabled_) return;
        const auto key = std::make_tuple(ownerBegin, backing, generation,
                                          pc, usesInterpreter);
        if (reported_.find(key) != reported_.end()) return;
        if (reported_.size() == maximumEvents) {
            if (!limitReported_) {
                std::cerr << "arm64-metal jit-trace event-limit=" << maximumEvents << '\n';
                limitReported_ = true;
            }
            return;
        }
        reported_.insert(key);
        std::cerr << "arm64-metal jit-trace event=dispatch pc=0x" << hexadecimal(pc)
                  << " owner=dynamic-vm-executable range=0x"
                  << hexadecimal(ownerBegin) << "-0x" << hexadecimal(ownerEnd)
                  << " backing=0x" << hexadecimal(backing)
                  << " generation=" << generation << " map-jit="
                  << (mapJit ? "true" : "false") << " backend="
                  << (usesInterpreter ? "gpu-interpreter" : "gpu-translated") << '\n';
    }

private:
    static constexpr std::size_t maximumEvents = 256;
    bool enabled_ = false;
    bool limitReported_ = false;
    std::set<std::tuple<std::uint64_t, std::size_t, std::uint64_t,
                        std::uint64_t, bool>> reported_;
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
    ScopedProcessUmask guestUmask;
    RawProcState rawProcState;
    ProcessPerformanceSummary performanceSummary;
    ProgressTrace progressTrace;
    SchedulerTrace schedulerTrace;
    WorkqueueTrace workqueueTrace;
    DyldBootstrapTrace dyldBootstrapTrace;
    JitExecutionTrace jitExecutionTrace;
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
    if (image.cache_reader) {
        constexpr auto libdyldPath = "/usr/lib/system/libdyld.dylib";
        constexpr std::array cachedDyldServices{
            "__dyld_call_with_writable_tpro_memory",
            "_dyld_get_program_sdk_version_token",
            "_dyld_get_program_minos_version_token",
            "_dyld_version_token_get_platform",
            "_dyld_version_token_at_least",
        };
        for (const auto* symbol : cachedDyldServices) {
            if (const auto resolved = image.cache_reader->resolveExport(libdyldPath, symbol)) {
                imports.emplace(resolved->guest_address, symbol);
            }
        }
    }
    std::set<std::int64_t> qsortEntries;
    if (image.cache_reader) {
        constexpr auto libsystemCPath = "/usr/lib/system/libsystem_c.dylib";
        const auto qsort = image.cache_reader->resolveExport(libsystemCPath, "_qsort");
        if (qsort && qsort->guest_address > 0 && image.is_code(qsort->guest_address) &&
            image.is_arm64e_code(qsort->guest_address) == std::optional<bool>{true})
            qsortEntries.insert(qsort->guest_address);
    }
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
    auto csopsStatus = signing.status;
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
    const auto dyldApiTokenHeadroom = options.cachedInitializers ? 12U : 0U;
    if (image.authentication.size() >
        std::numeric_limits<std::size_t>::max() - dyldApiTokenHeadroom) {
        throw std::length_error("Initial guest authentication table size overflow");
    }
    input.initialAuthenticationEntries = std::max<std::size_t>(
        guestInitialTokenCapacity, image.authentication.size() + dyldApiTokenHeadroom);
    if (input.initialAuthenticationEntries > kGuestTokenCodecCapacity) {
        throw std::length_error("Initial guest authentication table exceeds pointer codec limit");
    }
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
    constexpr std::uint64_t guestServiceScratchBytes = 4096;
    const auto programVars = bootstrap.scratchAddress + guestServiceScratchBytes;
    const auto programNameSlot = programVars + 5 * sizeof(std::uint64_t);
    store_u64(bootstrap.memory, programVars, static_cast<std::uint64_t>(image.main_header));
    store_u64(bootstrap.memory, programVars + sizeof(std::uint64_t), bootstrap.argcAddress);
    store_u64(bootstrap.memory, programVars + 2 * sizeof(std::uint64_t), bootstrap.argvAddress);
    store_u64(bootstrap.memory, programVars + 3 * sizeof(std::uint64_t), bootstrap.environSlot);
    store_u64(bootstrap.memory, programVars + 4 * sizeof(std::uint64_t), programNameSlot);
    store_u64(bootstrap.memory, programNameSlot, bootstrap.programName);
    bootstrap.registers[4] = programVars;
    GuestThread mainThread;
    mainThread.id = 1;
    mainThread.stack = {static_cast<std::int64_t>(bootstrap.stack.begin),
                        static_cast<std::int64_t>(bootstrap.stack.end)};
    mainThread.errnoAddress = static_cast<std::int64_t>(bootstrap.errnoSlot);
    mainThread.tsdAddress = static_cast<std::int64_t>(bootstrap.mainThreadSpecificData);
    mainThread.registers = bootstrap.registers;
    GuestScheduler guestScheduler(std::move(mainThread));
    std::map<std::uint64_t, GuestMemoryRange> liveThreadStacks;
    liveThreadStacks.emplace(guestMainThreadID,
        GuestMemoryRange{bootstrap.stack.begin, bootstrap.stack.end});
    std::map<std::uint64_t, GuestMemoryRange> threadStackGuards;
    std::map<std::uint64_t, GuestMemoryRange> threadStackRanges;
    std::map<std::uint64_t, std::uint32_t> protectedGuestPages;
    initialize_bindings(image, bootstrap);
    initialize_authenticated_pointers(image, bootstrap);
    const auto dyldApis = options.cachedInitializers
        ? initialize_dyld_apis(image, bootstrap, imports, guestStubs)
        : DyldApisFacade{};
    bool dyldApisPublicationSeeded = dyldApis.publicationSeeded;
    if (dyldApisPublicationSeeded) {
        dyldBootstrapTrace.event("gapis-seeded");
    }
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
    std::size_t writableTproStartupTargets = 0;
    const bool cacheBootstrap = options.cacheBootstrap || options.cachedInitializers ||
        (std::getenv("ARM64_METAL_CACHE_BOOTSTRAP") != nullptr &&
         std::strcmp(std::getenv("ARM64_METAL_CACHE_BOOTSTRAP"), "1") == 0);
    if (options.cachedInitializers) {
        for (const auto& initializer : image.startup_libsystem_initializers) {
            if (initializer.authentication || initializer.high_byte != 0 ||
                initializer.target_address > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) ||
                !image.is_code(static_cast<std::int64_t>(initializer.target_address))) {
                throw std::runtime_error(
                    "libSystem initializer target is not directly callable");
            }
            startup.push_back(initializer.target_address);
        }
        writableTproStartupTargets += image.startup_libsystem_initializers.size();
    }
    if (cacheBootstrap) {
        if (image.objc_bootstrap) {
            startup.push_back(static_cast<std::uint64_t>(*image.objc_bootstrap));
        }
    }
    if (options.cachedInitializers) {
        if (!image.startup_initializer_metadata_collected) {
            throw std::runtime_error("Cached initializer plan was not collected");
        }
        for (std::size_t index = 0; index < image.startup_initializer_images.size(); ++index) {
            const auto& initializerImage = image.startup_initializer_images[index];
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
    const auto initialStateWords = backend.maxBufferLength() / sizeof(std::uint64_t);
    if (processRegisters.size() > initialStateWords) {
        throw std::runtime_error(
            "Initial guest authentication table capacity " +
            std::to_string(processRegisters[guestTokenCapacity]) +
            " exceeds Metal device buffer limit of " +
            std::to_string(initialStateWords));
    }
    auto stateBuffer = backend.makeSharedBuffer(processRegisters.size() * sizeof(std::uint64_t));
    if (image.cache_reader && !image.cache_reader->tproMappings().empty()) {
        std::vector<CacheRegion> splitRegions;
        for (const auto& region : image.cache_regions) {
            std::vector<std::uint64_t> boundaries{
                static_cast<std::uint64_t>(region.logical_start),
                static_cast<std::uint64_t>(region.logical_end)};
            for (const auto& mapping : image.cache_reader->tproMappings()) {
                const auto begin = static_cast<std::uint64_t>(mapping.guestAddress);
                const auto end = begin + mapping.size;
                if (begin > static_cast<std::uint64_t>(region.logical_start) &&
                    begin < static_cast<std::uint64_t>(region.logical_end)) {
                    boundaries.push_back(begin);
                }
                if (end > static_cast<std::uint64_t>(region.logical_start) &&
                    end < static_cast<std::uint64_t>(region.logical_end)) {
                    boundaries.push_back(end);
                }
            }
            std::sort(boundaries.begin(), boundaries.end());
            boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                             boundaries.end());
            for (std::size_t index = 1; index < boundaries.size(); ++index) {
                const auto begin = boundaries[index - 1];
                const auto end = boundaries[index];
                const bool tpro = std::any_of(
                    image.cache_reader->tproMappings().begin(),
                    image.cache_reader->tproMappings().end(), [&](const auto& mapping) {
                        const auto tproBegin = static_cast<std::uint64_t>(mapping.guestAddress);
                        return begin >= tproBegin && begin - tproBegin < mapping.size &&
                            end - begin <= mapping.size - (begin - tproBegin);
                    });
                splitRegions.push_back({static_cast<std::int64_t>(begin),
                    static_cast<std::int64_t>(end),
                    region.backing + static_cast<std::size_t>(begin -
                        static_cast<std::uint64_t>(region.logical_start)),
                    tpro ? false : region.writable, region.readable, tpro});
            }
        }
        image.cache_regions = std::move(splitRegions);
    }
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
    for (const auto& segment : image.mapped_image_segments) {
        if (segment.start >= 0 && segment.end > segment.start &&
            static_cast<std::uint64_t>(segment.end) <= guestMemory.size()) {
            guestMemory.add_guest_range({static_cast<std::uint64_t>(segment.start),
                static_cast<std::uint64_t>(segment.end)});
        }
    }
    for (const auto& range : {bootstrap.stack, bootstrap.heap}) {
        if (range.begin < range.end && range.end <= guestMemory.size()) {
            guestMemory.add_guest_range(range);
        }
    }
    std::set<std::uint64_t> objcWritableBackings;
    std::optional<std::uint64_t> tproWritableThread;
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
    struct ReadOnlyCommpageField {
        std::uint64_t address;
        std::size_t backing;
        std::size_t width;
    };
    const std::array<ReadOnlyCommpageField, 6> readOnlyCommpageFields{{
        {0xfffff4025ULL, quiescentBacking + 56, 1},
        {0xfffff4037ULL, quiescentBacking + 57, 1},
        {0xfffff4084ULL, quiescentBacking + 60, sizeof(std::uint32_t)},
        {0xfffffc02fULL, quiescentBacking + 64, 1},
        {0xfffffc035ULL, quiescentBacking + 65, 1},
        {0xfffffc036ULL, quiescentBacking + 66, 1},
    }};
    for (const auto& field : readOnlyCommpageFields) {
        std::uint64_t value = 0;
        vm_size_t bytesRead = 0;
        const auto readResult = vm_read_overwrite(
            mach_task_self(), static_cast<vm_address_t>(field.address), field.width,
            reinterpret_cast<vm_address_t>(&value), &bytesRead);
        if (readResult == KERN_SUCCESS && bytesRead == field.width) {
            publishCommpage(field.address, field.backing, value, field.width);
        }
    }
    process_msl::Config config;
    config.linear_image_end = image.linear_image_end;
    config.mapping_backing_end = image.mapping_backing_end();
    config.writable_regions.reserve(image.writable_regions.size() + 1);
    for (const auto& range : image.writable_regions) {
        if (range.first < 0 || range.second < range.first) {
            throw std::runtime_error("Executable image has an invalid writable range");
        }
        config.writable_regions.push_back({static_cast<std::uint64_t>(range.first),
                                           static_cast<std::uint64_t>(range.second)});
    }
    config.writable_regions.push_back({
        static_cast<std::uint64_t>(bootstrap.stack.begin),
        static_cast<std::uint64_t>(bootstrap.stack.end)});

    // The lower bound remains the current thread's errno slot. The linear heap
    // above it remains writable by existing policy, including reclaimed
    // allocations; explicit ranges grant live stacks below it access.
    const std::uint64_t mainErrnoSlot = bootstrap.errnoSlot;
    std::vector<std::uint64_t> writableTable;
    SharedBuffer writableBuffer;
    const auto syncWritableTable = [&](std::uint64_t errnoAddress) {
        writableTable.clear();
        const auto rangeCount = config.writable_regions.size() + liveThreadStacks.size();
        writableTable.reserve(2 + 2 * rangeCount);
        writableTable.push_back(std::min(errnoAddress, mainErrnoSlot));
        writableTable.push_back(static_cast<std::uint64_t>(rangeCount));
        for (const auto& range : config.writable_regions) {
            writableTable.push_back(range.begin);
            writableTable.push_back(range.end);
        }
        for (const auto& [threadId, range] : liveThreadStacks) {
            (void)threadId;
            writableTable.push_back(range.begin);
            writableTable.push_back(range.end);
        }
        writableTable.push_back(static_cast<std::uint64_t>(protectedGuestPages.size()));
        for (const auto& page : protectedGuestPages) {
            const auto guard = std::find_if(threadStackGuards.begin(), threadStackGuards.end(),
                [&page](const auto& item) {
                    return page.first >= item.second.begin && page.first < item.second.end;
                });
            if (guard == threadStackGuards.end() ||
                bootstrap.pageSize > guard->second.end - page.first) {
                throw std::logic_error("Protected guest page has no owning stack guard");
            }
            writableTable.push_back(page.first);
            writableTable.push_back(page.first + bootstrap.pageSize);
            writableTable.push_back(page.second);
        }
        const auto byteLength = writableTable.size() * sizeof(std::uint64_t);
        if (writableBuffer.size() < byteLength) {
            auto expanded = backend.makeSharedBuffer(byteLength);
            if (expanded.data() == nullptr) {
                throw std::runtime_error("Metal returned a null guest writable-range buffer");
            }
            writableBuffer = std::move(expanded);
        }
        if (writableBuffer.data() == nullptr) {
            throw std::runtime_error("Metal returned a null guest writable-range buffer");
        }
        std::memcpy(writableBuffer.data(), writableTable.data(), byteLength);
    };
    syncWritableTable(bootstrap.errnoSlot);
    const auto releaseThreadStackGuard = [&](std::uint64_t threadId) {
        const auto found = threadStackGuards.find(threadId);
        if (found == threadStackGuards.end()) return;
        const auto begin = found->second.begin;
        const auto end = found->second.end;
        auto page = protectedGuestPages.lower_bound(begin);
        while (page != protectedGuestPages.end() && page->first < end) {
            guestMemory.remove_mapping(page->first);
            page = protectedGuestPages.erase(page);
        }
        threadStackGuards.erase(found);
        syncWritableTable(bootstrap.errnoSlot);
    };
    const std::array<std::uint64_t, 4> memoryConfig{
        config.minimum_image_address, config.shared_guest_base,
        config.linear_image_end, config.mapping_backing_end};
    auto memoryConfigBuffer = backend.makeSharedBuffer(sizeof(memoryConfig));
    if (memoryConfigBuffer.data() == nullptr) {
        throw std::runtime_error("Metal returned a null guest memory configuration buffer");
    }
    std::memcpy(memoryConfigBuffer.data(), memoryConfig.data(), sizeof(memoryConfig));

    std::map<std::int64_t, std::string> decodeErrors;
    std::map<std::int64_t, CompiledProcessKernel> kernels;
    const auto compileKernel = [&](std::int64_t entry) {
        const auto plan = plan_process_kernel(image, imports, guestStubs, qsortEntries,
                                              decodeErrors, entry,
                                              interpreter != nullptr);
        if (plan.interpreterEligible) {
            progressTrace.event("interpreter-block-ready",
                                static_cast<std::uint64_t>(entry),
                                plan.interpretedPackets.size());
            for (const auto& packet : plan.interpretedPackets) {
                progressTrace.interpreterPacket(packet.pc,
                                                packet.operation[0]);
            }
            return CompiledProcessKernel{{}, plan.interpretedPackets, true};
        }

        const auto started = std::chrono::steady_clock::now();
        progressTrace.event("compile-start", static_cast<std::uint64_t>(entry));
        const auto cases = translate_core::render_blocks(plan.instructions, entry, "ul");
        const auto source = process_msl::render_source(
            cases, image.is_arm64e_code(image.entry) == std::optional<bool>{false});
        auto translated = backend.compileCompute(source, "run_process");
        const auto compileSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::duration<double>(compileSeconds)).count();
        performanceSummary.recordCompile(compileSeconds);
        progressTrace.event("compile-done", static_cast<std::uint64_t>(entry),
                            static_cast<std::uint64_t>(elapsed));
        return CompiledProcessKernel{std::move(translated), {}, false};
    };
    bool processCodeCacheInvalidated = false;
    std::function<bool(bool)> applyJitWriteProtection;
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
    std::size_t startupIndex = 0;
    std::string lastKernelSelectionPath = "initial-startup-entry";
    std::uint64_t stopPcForDiagnostic = 0;
    std::array<std::string, 32> transitionHistory;
    std::size_t transitionHistoryCount = 0;
    const auto recordTransition = [&](const std::string& entry) {
        transitionHistory[transitionHistoryCount++ % transitionHistory.size()] = entry;
    };
    std::function<void(std::uint64_t)> describeStopPc;
    const auto dumpTransitions = [&]() {
        if (describeStopPc && stopPcForDiagnostic != 0) describeStopPc(stopPcForDiagnostic);
        if (std::getenv("ARM64_METAL_TRACE_FATAL_SIGNAL") == nullptr) return;
        const auto count = std::min(transitionHistoryCount, transitionHistory.size());
        for (std::size_t i = transitionHistoryCount - count; i < transitionHistoryCount; ++i)
            std::cerr << "  history " << transitionHistory[i % transitionHistory.size()] << '\n';
    };
    const auto selectKernel = [&](std::int64_t entry) {
        const bool selectorStub = image.cache_reader &&
            (image.cache_reader->stubTarget(entry).has_value() ||
             (!image.is_code(entry) &&
              image.cache_reader->selectorStubInstruction(entry).has_value()));
        if (!image.is_code(entry) && imports.find(entry) == imports.end() &&
            guestStubs.find(entry) == guestStubs.end() && !selectorStub) {
            stopPcForDiagnostic = static_cast<std::uint64_t>(entry);
            dumpTransitions();
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
        lastKernelSelectionPath = "selectKernel-transition";
    };
    const auto clearInvalidatedProcessCodeCache = [&]() {
        if (!processCodeCacheInvalidated) return false;
        processCodeCacheInvalidated = false;
        kernels.clear();
        decodeErrors.clear();
        activeKernel = nullptr;
        return true;
    };

    const std::vector<BufferBinding> bindings{
        {&memoryBuffer, 0, 0}, {&stateBuffer, 1, 0}, {&sizeBuffer, 2, 0},
        {&writableBuffer, 3, 0}, {&residentBuffer, 4, 0},
        {&memoryConfigBuffer, 5, 0}};
    const gpu_interpreter::MemoryAccessContext interpreterMemoryAccess{
        &memoryBuffer, &sizeBuffer, &writableBuffer, &residentBuffer,
        &memoryConfigBuffer};
    const DispatchSize one{1, 1, 1};
    struct PreviousDispatch {
        bool valid = false;
        std::uint64_t startPc = 0;
        std::uint64_t returnedPc = 0;
        std::uint64_t status = 0;
        std::uint64_t startBudget = 0;
        std::uint64_t returnedBudget = 0;
        std::int64_t selectedEntry = 0;
        bool usedInterpreter = false;
        std::size_t packetCount = 0;
    } previousDispatch;
    std::string lastHostTransition = "none";
    HostServiceState hostServiceState;
    if (performanceSummary.enabled()) {
        hostServiceState.recordPerformanceHostService = [&](double seconds) {
            performanceSummary.recordHostService(seconds);
        };
    }
    const auto dispatchActiveKernel = [&]() {
        auto* before = static_cast<std::uint64_t*>(stateBuffer.data());
        const auto startPc = before[33];
        const auto startBudget = before[36];
        const auto selectedEntry = activeEntry;
        const bool usedInterpreter = activeKernel->usesInterpreter;
        const auto packetCount = usedInterpreter
            ? activeKernel->interpretedPackets.size() : 0;
        DispatchTiming timing;
        try {
            if (usedInterpreter) {
                timing = interpreter->dispatch(backend, activeKernel->interpretedPackets,
                                               stateBuffer, &interpreterMemoryAccess).timing;
            } else {
                timing = backend.dispatch(activeKernel->translated, bindings, one, one);
            }
        } catch (const std::invalid_argument& error) {
            constexpr std::string_view missingPc =
                "Interpreter process PC is absent from the packet table";
            if (!usedInterpreter || std::string_view(error.what()).find(missingPc) ==
                                        std::string_view::npos) {
                throw;
            }
            const auto& packets = activeKernel->interpretedPackets;
            const auto currentPc = before[33];
            std::ostringstream detail;
            detail << error.what()
                   << "; current_pc=0x" << hexadecimal(currentPc)
                   << " active_entry=0x" << hexadecimal(
                       static_cast<std::uint64_t>(selectedEntry))
                   << " selected_path=" << lastKernelSelectionPath
                   << " startup_index=" << startupIndex
                   << " packet_count=" << packets.size();
            if (!packets.empty()) {
                detail << " packet_range=0x" << hexadecimal(packets.front().pc)
                       << "..0x" << hexadecimal(packets.back().pc);
                const auto found = std::find_if(packets.begin(), packets.end(),
                    [currentPc](const auto& packet) { return packet.pc == currentPc; });
                detail << " pc_membership=" << (found != packets.end() ? "present" : "absent");
                if (found == packets.end()) {
                    const auto next = std::lower_bound(packets.begin(), packets.end(), currentPc,
                        [](const auto& packet, std::uint64_t pc) { return packet.pc < pc; });
                    if (next != packets.begin()) {
                        detail << " previous_packet=0x" << hexadecimal(std::prev(next)->pc);
                    }
                    if (next != packets.end()) {
                        detail << " next_packet=0x" << hexadecimal(next->pc);
                    }
                }
            } else {
                detail << " packet_range=empty pc_membership=absent";
            }
            if (previousDispatch.valid) {
                detail << " previous_dispatch_start=0x" << hexadecimal(previousDispatch.startPc)
                       << " previous_dispatch_returned=0x" << hexadecimal(previousDispatch.returnedPc)
                       << " previous_dispatch_status=" << previousDispatch.status
                       << " previous_dispatch_budget=" << previousDispatch.startBudget
                       << "->" << previousDispatch.returnedBudget
                       << " previous_kernel_entry=0x" << hexadecimal(
                           static_cast<std::uint64_t>(previousDispatch.selectedEntry))
                       << " previous_kernel_kind="
                       << (previousDispatch.usedInterpreter ? "interpreter" : "translated")
                       << " previous_packet_count=" << previousDispatch.packetCount;
            } else {
                detail << " previous_dispatch=none";
            }
            detail << " last_host_transition=" << lastHostTransition;
            throw std::invalid_argument(detail.str());
        }
        const auto* after = static_cast<const std::uint64_t*>(stateBuffer.data());
        previousDispatch = {true, startPc, after[33], after[34], startBudget, after[36],
                            selectedEntry, usedInterpreter, packetCount};
        performanceSummary.recordDispatch(timing);
    };
    HostServiceState::GuestUnixSocket guestSyslogSink;
    guestSyslogSink.type = SOCK_DGRAM;
    guestSyslogSink.boundPath = "/var/run/syslog";
    guestSyslogSink.captureSink = true;
    hostServiceState.guestUnixSockets.emplace(guestUnixSyslogCaptureDescriptor,
        std::move(guestSyslogSink));
    hostServiceState.guestUnixSocketPaths.emplace("/var/run/syslog",
        guestUnixSyslogCaptureDescriptor);
    if (bootstrap.heap.end > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        throw std::length_error("Guest heap address exceeds the allocator range");
    }
    hostServiceState.heapEnd = bootstrap.heap.end;
    hostServiceState.heap.emplace(GuestRange{
        static_cast<std::int64_t>(bootstrap.heap.begin),
        static_cast<std::int64_t>(bootstrap.heap.end)});
    std::map<std::uint64_t, std::int32_t> mutexAttributes;
    struct GuestThreadAttributes {
        std::uint64_t stackSize = 512U * 1024U;
        std::optional<std::uint64_t> stackAddress;
        std::uint64_t guardSize = 16U * 1024U;
        std::int32_t detachState = PTHREAD_CREATE_JOINABLE;
    };
    std::map<std::uint64_t, GuestThreadAttributes> threadAttributes;
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
    GuestImageRegistry imageRegistry(image.loaded_image_order);
    struct GuestImageNotification {
        std::uint64_t callback = 0;
        std::array<std::uint64_t, 4> arguments{};
    };
    std::vector<std::uint64_t> addImageCallbacks;
    std::vector<std::uint64_t> bulkImageCallbacks;
    std::vector<std::vector<std::uint64_t>> bulkImageCallbackLeases;
    const auto collectImageNotifications = [&](std::size_t firstImage,
                                               std::size_t endImage,
                                               std::size_t firstCallback) {
        std::vector<GuestImageNotification> notifications;
        for (auto index = firstImage; index < endImage; ++index) {
            const auto header = imageRegistry.header_at(index);
            if (!header) throw std::logic_error("Guest image registry index disappeared");
            const auto slide = guest_image_vmaddr_slide(
                imageRegistry, index, image.image_slides);
            for (auto callback = firstCallback;
                 callback < addImageCallbacks.size(); ++callback) {
                notifications.push_back({addImageCallbacks[callback],
                                         {*header, slide, 0, 0}});
            }
        }
        return notifications;
    };
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
        if (defaultScope && symbol == "_dyld_sdk_at_least") {
            for (const auto& [address, importedName] : imports) {
                if (address < 0 || importedName != symbol) continue;
                return static_cast<std::uint64_t>(address);
            }
        }
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
    if (image.mapping_backing_end() > guestMappingLimit - guestMappingBase) {
        throw std::runtime_error("Guest mapping arena exceeds the allocator address limit");
    }
    // The first 80 bytes past the cache backing hold the commpage fields published above.
    GuestHeap mappingHeap({static_cast<std::int64_t>(image.cache_backing_end() + 80),
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
    constexpr std::uint32_t machTrailerSizeDefault = sizeof(mach_msg_trailer_t);
    struct GuestMachBlockedReceive {
        std::uint32_t port;
        std::uint64_t buffer;
        std::uint32_t capacity;
        std::uint32_t trailerSize = machTrailerSizeDefault;
    };
    struct GuestUnixBlockedReceive {
        std::int32_t descriptor;
        std::uint64_t buffer;
        std::uint64_t capacity;
        std::uint64_t sourceAddress;
        std::uint64_t sourceLengthAddress;
    };
    std::map<std::uint32_t, std::deque<std::uint64_t>> guestMachReceiveWaiters;
    std::map<std::uint64_t, GuestMachBlockedReceive> guestMachBlockedReceives;
    std::map<std::int32_t, std::deque<std::uint64_t>> guestUnixReceiveWaiters;
    std::map<std::uint64_t, GuestUnixBlockedReceive> guestUnixBlockedReceives;
    std::function<void()> fireDueMachTimers;
    std::function<std::optional<std::uint64_t>()> earliestMachTimerNanoseconds;
#include "raw_net_state.inc"
    const auto pruneGuestMachReceiveWaiters = [&]() {
        for (auto waiters = guestMachReceiveWaiters.begin();
             waiters != guestMachReceiveWaiters.end();) {
            const auto port = waiters->first;
            auto& queue = waiters->second;
            queue.erase(std::remove_if(queue.begin(), queue.end(), [&](std::uint64_t threadId) {
                const auto blocked = guestMachBlockedReceives.find(threadId);
                const auto* thread = guestScheduler.find(threadId);
                const auto* wait = thread != nullptr && thread->wait.has_value()
                    ? std::get_if<GuestMachReceiveWait>(&*thread->wait) : nullptr;
                const bool stale = blocked == guestMachBlockedReceives.end() || wait == nullptr ||
                    blocked->second.port != port || wait->port != port;
                if (stale && blocked != guestMachBlockedReceives.end() &&
                    blocked->second.port == port) {
                    progressTrace.event("mach-receive-stale", threadId, port);
                    guestMachBlockedReceives.erase(blocked);
                }
                return stale;
            }), queue.end());
            if (queue.empty()) waiters = guestMachReceiveWaiters.erase(waiters);
            else ++waiters;
        }
    };
    const auto expireAndPruneGuestMachReceiveWaiters = [&](std::uint64_t now) {
        if (fireDueMachTimers) fireDueMachTimers();
        const auto expired = guestScheduler.expire_waiters(now);
        if (expired != 0) progressTrace.event("mach-waiters-expired", now, expired);
        pollGuestHostWaiters(now);
        pruneGuestMachReceiveWaiters();
        for (auto waiters = guestUnixReceiveWaiters.begin();
             waiters != guestUnixReceiveWaiters.end();) {
            const auto descriptor = waiters->first;
            auto& queue = waiters->second;
            queue.erase(std::remove_if(queue.begin(), queue.end(), [&](std::uint64_t threadId) {
                const auto blocked = guestUnixBlockedReceives.find(threadId);
                const auto* thread = guestScheduler.find(threadId);
                const auto* wait = thread != nullptr && thread->wait.has_value()
                    ? std::get_if<GuestUnixSocketReceiveWait>(&*thread->wait) : nullptr;
                const bool stale = blocked == guestUnixBlockedReceives.end() || wait == nullptr ||
                    blocked->second.descriptor != descriptor || wait->descriptor != descriptor;
                if (stale && blocked != guestUnixBlockedReceives.end() &&
                    blocked->second.descriptor == descriptor) {
                    guestUnixBlockedReceives.erase(blocked);
                }
                return stale;
            }), queue.end());
            if (queue.empty()) waiters = guestUnixReceiveWaiters.erase(waiters);
            else ++waiters;
        }
        return expired;
    };
    const auto dumpFrames = [&](const std::uint64_t* r, std::size_t count) {
        std::vector<std::uint64_t> frames{count > 33 ? r[33] : 0, count > 30 ? r[30] : 0};
        auto fp = count > 29 ? r[29] : 0;
        for (int depth = 0; depth < 24 && fp != 0 && guestMemory.is_guest_readable(fp, 16); ++depth) {
            std::uint64_t pair[2];
            std::memcpy(pair, guestMemory.buffer(fp, 16), 16);
            frames.push_back(pair[1]);
            if (pair[0] <= fp) break;
            fp = pair[0];
        }
        for (const auto pc : frames) {
            std::uint64_t best = 0, bestIndex = ~std::uint64_t{0};
            for (std::uint64_t index = 0; index < imageRegistry.size(); ++index) {
                const auto header = imageRegistry.header_at(index);
                if (header && *header <= (pc & 0x0000ffffffffffffULL) && *header >= best) {
                    best = *header; bestIndex = index;
                }
            }
            std::cerr << "  frame 0x" << std::hex << pc << std::dec;
            if (bestIndex != ~std::uint64_t{0}) {
                const auto name = imageRegistry.name_at(bestIndex);
                std::cerr << " " << (name ? std::string(*name) : "?") << "+0x" << std::hex
                          << ((pc & 0x0000ffffffffffffULL) - best) << std::dec;
            }
            std::cerr << '\n';
        }
    };
    std::optional<std::uint64_t> idleReportStart;
    bool idleReported = false;
    constexpr std::uint64_t guestDispatchTimeSlice = 32;
    std::uint64_t dispatchesInTimeSlice = 0;
    std::uint64_t timeSliceThread = guestScheduler.current_id();
    const auto syncThreadSwitch = [&](bool requeue, std::vector<std::uint64_t>& live) {
        std::vector<std::uint64_t> globalTail;
        if (live.size() >= guestTokenCount) {
            globalTail.assign(live.begin() + static_cast<std::ptrdiff_t>(guestTokenCount),
                              live.end());
        }
        const auto activeId = guestScheduler.current_id();
        auto& activeBeforeSchedule = guestScheduler.current();
        const bool activeWasWaiting = activeBeforeSchedule.wait.has_value();
        if (requeue || !activeBeforeSchedule.result.has_value()) {
            activeBeforeSchedule.registers = live;
            activeBeforeSchedule.pendingSignals = hostServiceState.threadPendingSignals;
            activeBeforeSchedule.signalMask = hostServiceState.threadSignalMask;
        }
        expireAndPruneGuestMachReceiveWaiters(monotonicNanoseconds());
        if (activeWasWaiting) {
            const auto* activeAfterExpiry = guestScheduler.find(activeId);
            if (activeAfterExpiry != nullptr && !activeAfterExpiry->wait.has_value()) {
                live = activeAfterExpiry->registers;
            }
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
            expireAndPruneGuestMachReceiveWaiters(now);
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
                    idleReportStart.reset();
                    idleReported = false;
                    break;
                }
            }
            auto deadline = guestScheduler.earliest_deadline();
            if (const auto timerDeadline = earliestMachTimerNanoseconds
                    ? earliestMachTimerNanoseconds() : std::nullopt) {
                if (!deadline || *timerDeadline < *deadline) deadline = timerDeadline;
            }
            if (!deadline) {
                progressTrace.event("threads-blocked-no-deadline", activeId);
                throw std::runtime_error("All guest threads are blocked with no deadline or runnable thread");
            }
            if (!idleReportStart) idleReportStart = now;
            if (!idleReported && now - *idleReportStart > 20'000'000'000ULL) {
                idleReported = true;
                std::cerr << "arm64-metal all guest threads blocked for 20s; threads="
                          << guestScheduler.thread_count() << '\n';
                for (std::uint64_t id = 1; id < 256; ++id) {
                    const auto* t = guestScheduler.find(id);
                    if (t == nullptr) continue;
                    std::cerr << "  thread " << id << " pc=0x" << std::hex
                              << (t->registers.size() > 33 ? t->registers[33] : 0) << std::dec
                              << " wait=" << (t->wait ? static_cast<int>(t->wait->index()) : -1)
                              << " result=" << (t->result ? "done" : "none") << '\n';
                }
                dumpTransitions();
                dumpFrames(guestScheduler.current().registers.data(),
                           guestScheduler.current().registers.size());
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
            dispatchesInTimeSlice = 0;
            timeSliceThread = guestScheduler.current_id();
            if (!applyJitWriteProtection ||
                !applyJitWriteProtection(guestScheduler.current().jitWriteProtect)) {
                throw std::runtime_error("Guest thread MAP_JIT mode could not be applied before resume");
            }
            (void)clearInvalidatedProcessCodeCache();
            const auto pc = live[33];
            if (pc > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::runtime_error("Guest thread resumed outside translated text");
            }
            selectKernel(static_cast<std::int64_t>(pc));
        }
        return switched;
    };
    const auto preemptAtDispatchBoundary = [&](std::uint64_t status) {
        const auto now = monotonicNanoseconds();
        auto deadline = guestScheduler.earliest_deadline();
        if (const auto timerDeadline = earliestMachTimerNanoseconds
                ? earliestMachTimerNanoseconds() : std::nullopt) {
            if (!deadline || *timerDeadline < *deadline) deadline = timerDeadline;
        }
        const auto expired = deadline && *deadline <= now
            ? expireAndPruneGuestMachReceiveWaiters(now) : 0;
        const auto threadId = guestScheduler.current_id();
        if (timeSliceThread != threadId) {
            timeSliceThread = threadId;
            dispatchesInTimeSlice = 0;
        }
        if (guestScheduler.ready_queue().empty()) {
            dispatchesInTimeSlice = 0;
            return false;
        }
        const auto boundaryCount = ++dispatchesInTimeSlice;
        // A returned block-budget dispatch may have executed up to 8192
        // guest instructions. Yield immediately there; ordinary PC handoffs
        // use the bounded 32-dispatch quantum.
        const auto quantum = status == 1 || status == 8
            ? std::uint64_t{1} : guestDispatchTimeSlice;
        if (expired == 0 && boundaryCount < quantum) return false;
        const auto pc = static_cast<const std::uint64_t*>(stateBuffer.data())[33];
        std::vector<std::uint64_t> live(
            static_cast<const std::uint64_t*>(stateBuffer.data()),
            static_cast<const std::uint64_t*>(stateBuffer.data()) +
                stateBuffer.size() / sizeof(std::uint64_t));
        if (syncThreadSwitch(true, live)) {
            schedulerTrace.timeslice(threadId, guestScheduler.current_id(), pc,
                                     status, boundaryCount);
            return true;
        }
        dispatchesInTimeSlice = 0;
        return false;
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
    std::map<std::uint32_t, std::uint32_t> guestSendRightUrefs{
        {static_cast<std::uint32_t>(guestMachTaskPort), 3U}};
    std::map<std::uint64_t, std::uint32_t> guestThreadPortNames;
    std::set<std::uint64_t> guestWorkqueueThreadIds;
    std::uint32_t nextGuestThreadPortName = 0x40000003U;
    constexpr std::uint32_t maxGuestPortUrefs = 0xffffU;
    const auto acquireGuestFixedSendRight = [&](std::uint32_t name) {
        auto& references = guestSendRightUrefs[name];
        if (references == 0) references = 1;
        else if (references < maxGuestPortUrefs) ++references;
        return name;
    };
    const auto acquireGuestThreadSendRight = [&](std::uint64_t threadId) {
        auto threadPort = guestThreadPortNames.find(threadId);
        if (threadPort == guestThreadPortNames.end()) {
            if (nextGuestThreadPortName >=
                std::numeric_limits<std::uint32_t>::max() - 4) {
                return std::uint32_t{0};
            }
            const auto name = nextGuestThreadPortName;
            nextGuestThreadPortName += 4;
            threadPort = guestThreadPortNames.emplace(threadId, name).first;
            guestSendRightUrefs.emplace(name, 0);
        }
        auto& references = guestSendRightUrefs[threadPort->second];
        if (references < maxGuestPortUrefs) ++references;
        return threadPort->second;
    };
    const auto finishGuestThread = [&](std::uint64_t result,
                                       std::vector<std::uint64_t>& live) {
        const auto completedId = guestScheduler.current_id();
        if (guestWorkqueueThreadIds.erase(completedId) != 0) {
            const auto port = guestThreadPortNames.find(completedId);
            if (port != guestThreadPortNames.end()) {
                guestSendRightUrefs.erase(port->second);
                guestThreadPortNames.erase(port);
            }
        }
        const auto threadTsd = guestScheduler.current().tsdAddress;
        if (threadTsd <= 0) throw std::runtime_error("Guest thread has no TSD for TLV teardown");
        guestTlvRuntime.release_thread(static_cast<std::uint64_t>(threadTsd),
                                       guestTlvRelease, guestTlvWrite);
        signalRuntime.discard_thread(completedId);
        auto completion = guestScheduler.complete_current(result);
        liveThreadStacks.erase(completion.thread);
        if (completion.joined) {
            if (completion.joined->outputAddress != 0) {
                auto* output = guestMemory.buffer(completion.joined->outputAddress,
                                                   sizeof(std::uint64_t), true);
                std::memcpy(output, &completion.joined->result, sizeof(std::uint64_t));
            }
        }
        if (completion.reapedThread) {
            releaseThreadStackGuard(completion.reapedThread->id);
            if (completion.reapedThread->allocation) {
                hostServiceState.heap->release(*completion.reapedThread->allocation);
            }
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
        std::uint32_t inheritance = 1;
        std::uint32_t tag = 0;
        bool tpro = false;
    };
    struct VmBackingLease {
        std::int64_t backing;
        std::uint64_t size;
    };
    struct VmAllocation {
        std::uint64_t begin;
        std::uint64_t end;
        std::size_t backing;
        std::vector<VmRegion> regions;
        bool sparseAddressSpace = false;
        std::vector<VmBackingLease> ownedBackings;
    };
    struct VmReservation {
        std::uint64_t begin;
        std::uint64_t end;
        std::uint32_t currentProtection;
        std::uint32_t maximumProtection;
        std::uint32_t inheritance;
        std::uint32_t tag;
        std::uint64_t offset = 0;
        bool jit = false;
    };
    std::map<std::uint64_t, VmAllocation> vmAllocations;
    std::map<std::uint64_t, VmReservation> vmReservations;
    std::set<std::uint64_t> dynamicCodeRegionStarts;
    std::uint64_t jitTraceGeneration = 0;
    describeStopPc = [&](std::uint64_t pc) {
        std::cerr << "  stop-pc 0x" << hexadecimal(pc) << " mem "
                  << guestMemory.describe_near(pc, 0x2000000) << '\n';
        for (std::uint64_t index = 0; index < imageRegistry.size(); ++index) {
            const auto header = imageRegistry.header_at(index);
            const auto name = imageRegistry.name_at(index);
            if (!header || !name || imageRegistry.cache_backed_at(index)) continue;
            std::cerr << "  image 0x" << hexadecimal(*header) << " " << *name << '\n';
        }
        for (const auto& range : image.text_regions) {
            if (range.second + 0x2000000 >= static_cast<std::int64_t>(pc) &&
                range.first <= static_cast<std::int64_t>(pc) + 0x2000000) {
                std::cerr << "  text 0x" << hexadecimal(static_cast<std::uint64_t>(range.first))
                          << "-0x" << hexadecimal(static_cast<std::uint64_t>(range.second)) << '\n';
            }
        }
        for (const auto& [unused, allocation] : vmAllocations) {
            (void)unused;
            for (const auto& region : allocation.regions) {
                if (region.end + 0x1000000 < pc || region.begin > pc + 0x1000000) continue;
                std::cerr << "  vm 0x" << hexadecimal(region.begin) << "-0x"
                          << hexadecimal(region.end) << " r=" << region.readable
                          << " w=" << region.writable << " x=" << region.executable
                          << " max=" << region.maximumProtection << " jit=" << region.jit
                          << " tag=" << region.tag << '\n';
            }
        }
        for (const auto& [unused, reservation] : vmReservations) {
            (void)unused;
            if (reservation.end + 0x1000000 < pc || reservation.begin > pc + 0x1000000) continue;
            std::cerr << "  reservation 0x" << hexadecimal(reservation.begin) << "-0x"
                      << hexadecimal(reservation.end) << " cur=" << reservation.currentProtection
                      << " max=" << reservation.maximumProtection << " jit=" << reservation.jit
                      << '\n';
        }
        if (guestMemory.is_guest_readable(pc, 16)) {
            const auto* bytes = guestMemory.buffer(pc, 16);
            std::uint32_t words[4];
            std::memcpy(words, bytes, 16);
            std::cerr << "  words " << std::hex << words[0] << ' ' << words[1] << ' ' << words[2]
                      << ' ' << words[3] << std::dec << '\n';
        } else {
            std::cerr << "  words unreadable\n";
        }
    };
    const auto executableJitOwnerFor = [&](std::uint64_t pc)
        -> std::optional<std::tuple<std::uint64_t, std::uint64_t,
                                    std::size_t, bool>> {
        for (const auto& [unused, allocation] : vmAllocations) {
            (void)unused;
            for (const auto& region : allocation.regions) {
                const bool writableExecutable =
                    (region.maximumProtection & 6U) == 6U;
                // Only guest-owned VM allocations can be generated code.
                // Entitled guests may use ordinary anonymous RW-to-RX mappings.
                const bool generatedCodeEligible = region.jit ||
                    (image.main_code_policy.map_jit_eligible && writableExecutable);
                if (generatedCodeEligible && region.executable &&
                    region.begin <= pc && pc < region.end) {
                    return std::make_tuple(region.begin, region.end,
                                           region.backing, region.jit);
                }
            }
        }
        return std::nullopt;
    };
    struct GuestReceiveRight {
        std::uint32_t flags;
        std::uint64_t context;
        std::uint32_t qlimit = 5;
        bool portSet = false;
    };
    std::map<std::uint32_t, GuestReceiveRight> guestReceiveRights;
    std::set<std::uint32_t> guestDeadPortNames;
    std::map<std::uint32_t, clock_id_t> guestClockServiceIds;
    std::set<std::uint32_t> guestSemaphoreNames;
    struct GuestMachTimer {
        std::optional<std::uint64_t> deadline;
    };
    std::map<std::uint32_t, GuestMachTimer> guestMachTimers;
    std::map<std::uint32_t, std::uint32_t> guestPortSetMembers;
    std::map<std::uint32_t, std::uint64_t> guestPortGuards;
    std::uint64_t nextGuestActivityId = 1;
    // This reference is held by the emulated task object, independently of
    // the guest namespace's send urefs for the same port name.
    std::optional<std::uint32_t> guestTaskDebugControlPort;
    bool guestTaskDebugControlPortDead = false;
    std::uint32_t nextGuestSemaphoreName = 0x20000003U;
    struct GuestLocalPortRightTransfer {
        std::uint32_t name;
        std::uint32_t disposition;
    };
    struct GuestLocalMessage {
        std::uint32_t size;
        std::uint32_t bits;
        std::uint32_t remotePort;
        std::uint32_t voucherPort;
        std::int32_t messageId;
        std::vector<std::uint8_t> body;
        std::vector<GuestLocalPortRightTransfer> transferredSendRights;
        std::uint32_t receivedBits = MACH_MSG_TYPE_MOVE_SEND << 8;
        bool kernelRights = false;
    };
    std::map<std::uint32_t, std::deque<GuestLocalMessage>> guestMachMessageQueues;
    const auto kernelObjectRoutineExists = [](std::int32_t id) {
        static constexpr std::pair<std::int32_t, std::int32_t> ranges[] = {
            {200, 207}, {209, 209}, {213, 213}, {215, 222}, {225, 225}, {227, 228},
            {231, 235}, {400, 405}, {407, 407}, {412, 416}, {418, 420}, {423, 425},
            {1000, 1002}, {2401, 2403}, {2405, 2410}, {2800, 2834}, {2836, 2891},
            {3000, 3005}, {3200, 3201}, {3203, 3224}, {3226, 3242}, {3401, 3415},
            {3418, 3421}, {3433, 3453}, {3455, 3465}, {3600, 3618}, {3624, 3633},
            {3800, 3819}, {3821, 3826}, {3830, 3831}, {3833, 3833}, {4000, 4000},
            {4005, 4005}, {4009, 4010}, {4800, 4823}, {4825, 4829}, {4900, 4904},
            {5400, 5404}, {6200, 6201}};
        for (const auto& range : ranges) {
            if (id >= range.first && id <= range.second) return true;
        }
        return false;
    };
    constexpr std::uint32_t machReceiveTooLarge = 0x10004004;
    constexpr std::uint32_t machReceiveInvalidData = 0x10004008;
    constexpr std::uint32_t machReceivePortChanged = 0x10004006;
    const auto copyGuestMachMessage = [&](const GuestLocalMessage& message,
                                          std::uint32_t receiveName,
                                          std::uint64_t outputAddress,
                                          std::uint32_t capacity,
                                          std::uint32_t trailerSize) -> std::uint32_t {
        const auto outputSize = static_cast<std::uint64_t>(message.size) + trailerSize;
        if (capacity < outputSize) return machReceiveTooLarge;
        if (!guestMemory.is_guest_readable(outputAddress, outputSize) ||
            guestMemory.is_readonly(outputAddress, outputSize)) {
            return machReceiveInvalidData;
        }
        try {
            auto* output = guestMemory.buffer(outputAddress, outputSize, true);
            mach_msg_header_t header{};
            header.msgh_bits = (message.bits & MACH_MSGH_BITS_COMPLEX) |
                               message.receivedBits;
            header.msgh_size = message.size;
            header.msgh_remote_port = message.remotePort;
            header.msgh_local_port = receiveName;
            header.msgh_voucher_port = message.voucherPort;
            header.msgh_id = message.messageId;
            std::memcpy(output, &header, sizeof(header));
            if (!message.body.empty()) {
                std::memcpy(output + sizeof(header), message.body.data(), message.body.size());
            }
            for (std::size_t index = 0; index < message.transferredSendRights.size(); ++index) {
                const auto descriptorOffset = sizeof(mach_msg_header_t) +
                    sizeof(mach_msg_body_t) + index * sizeof(mach_msg_port_descriptor_t);
                mach_msg_port_descriptor_t descriptor{};
                std::memcpy(&descriptor, output + descriptorOffset, sizeof(descriptor));
                descriptor.disposition = MACH_MSG_TYPE_MOVE_SEND;
                std::memcpy(output + descriptorOffset, &descriptor, sizeof(descriptor));
            }
            std::memset(output + message.size, 0, trailerSize);
            const mach_msg_trailer_t trailer{0, trailerSize};
            std::memcpy(output + message.size, &trailer, sizeof(trailer));
            return 0;
        } catch (const std::exception&) {
            return machReceiveInvalidData;
        }
    };
    const auto deliverGuestUnixDatagrams = [&](std::int32_t descriptor) {
        const auto waiters = guestUnixReceiveWaiters.find(descriptor);
        const auto socket = hostServiceState.guestUnixSockets.find(descriptor);
        if (waiters == guestUnixReceiveWaiters.end() ||
            socket == hostServiceState.guestUnixSockets.end()) return;
        auto& queue = waiters->second;
        while (!queue.empty() && !socket->second.receiveQueue.empty()) {
            const auto threadId = queue.front();
            queue.pop_front();
            const auto blocked = guestUnixBlockedReceives.find(threadId);
            auto* thread = guestScheduler.find(threadId);
            const auto* wait = thread != nullptr && thread->wait.has_value()
                ? std::get_if<GuestUnixSocketReceiveWait>(&*thread->wait) : nullptr;
            if (blocked == guestUnixBlockedReceives.end() || wait == nullptr ||
                blocked->second.descriptor != descriptor || wait->descriptor != descriptor) {
                if (blocked != guestUnixBlockedReceives.end() &&
                    blocked->second.descriptor == descriptor) {
                    guestUnixBlockedReceives.erase(blocked);
                }
                continue;
            }
            std::uint64_t received = 0;
            std::uint32_t error = 0;
            const auto& datagram = socket->second.receiveQueue.front();
            const bool copied = copy_guest_unix_datagram(datagram, guestMemory,
                blocked->second.buffer, blocked->second.capacity,
                blocked->second.sourceAddress, blocked->second.sourceLengthAddress,
                received, error);
            guestUnixBlockedReceives.erase(blocked);
            if (copied) socket->second.receiveQueue.pop_front();
            progressTrace.event("unix-datagram-wake", threadId,
                copied ? received : error);
            guestScheduler.wake(threadId, copied ? 0 : static_cast<std::int32_t>(error));
            thread = guestScheduler.find(threadId);
            if (thread != nullptr && thread->registers.size() > 32) {
                if (copied) {
                    thread->registers[0] = received;
                    thread->registers[32] &= ~std::uint64_t{8};
                } else {
                    thread->registers[0] = error;
                    thread->registers[32] |= std::uint64_t{8};
                }
            }
        }
        if (queue.empty()) guestUnixReceiveWaiters.erase(waiters);
    };
    const auto closeGuestUnixReceiveWaiters = [&](std::int32_t descriptor) {
        const auto waiters = guestUnixReceiveWaiters.find(descriptor);
        if (waiters == guestUnixReceiveWaiters.end()) return;
        for (const auto threadId : waiters->second) {
            const auto blocked = guestUnixBlockedReceives.find(threadId);
            auto* thread = guestScheduler.find(threadId);
            const auto* wait = thread != nullptr && thread->wait.has_value()
                ? std::get_if<GuestUnixSocketReceiveWait>(&*thread->wait) : nullptr;
            if (blocked == guestUnixBlockedReceives.end() || wait == nullptr ||
                blocked->second.descriptor != descriptor || wait->descriptor != descriptor) {
                continue;
            }
            guestUnixBlockedReceives.erase(blocked);
            guestScheduler.wake(threadId, EBADF);
            thread = guestScheduler.find(threadId);
            if (thread != nullptr && thread->registers.size() > 32) {
                thread->registers[0] = EBADF;
                thread->registers[32] |= std::uint64_t{8};
            }
            progressTrace.event("unix-recv-closed", threadId,
                                static_cast<std::uint64_t>(descriptor));
        }
        guestUnixReceiveWaiters.erase(waiters);
    };
    const auto restoreGuestTransferredSendRight = [&](const GuestLocalMessage& message) {
        if (message.kernelRights) {
            for (const auto& transfer : message.transferredSendRights) {
                auto& references = guestSendRightUrefs[transfer.name];
                if (references < maxGuestPortUrefs) ++references;
            }
            return true;
        }
        for (const auto& transfer : message.transferredSendRights) {
            if (guestReceiveRights.find(transfer.name) == guestReceiveRights.end() ||
                guestSendRightUrefs.find(transfer.name) == guestSendRightUrefs.end()) {
                return false;
            }
        }
        for (const auto& transfer : message.transferredSendRights) {
            auto& references = guestSendRightUrefs.at(transfer.name);
            if (references < maxGuestPortUrefs) ++references;
        }
        return true;
    };
    const auto restoreTransferredRightAfterInvalidReceive =
        [&](const GuestLocalMessage& message) {
            for (const auto& transfer : message.transferredSendRights) {
                if (transfer.disposition == MACH_MSG_TYPE_MOVE_SEND &&
                    (guestReceiveRights.find(transfer.name) == guestReceiveRights.end() ||
                     guestSendRightUrefs.find(transfer.name) == guestSendRightUrefs.end())) {
                    return false;
                }
            }
            for (const auto& transfer : message.transferredSendRights) {
                if (transfer.disposition != MACH_MSG_TYPE_MOVE_SEND) continue;
                auto& references = guestSendRightUrefs.at(transfer.name);
                if (references < maxGuestPortUrefs) ++references;
            }
            return true;
        };
    const auto readyPortSetMember = [&](std::uint32_t set) -> std::optional<std::uint32_t> {
        for (const auto& membership : guestPortSetMembers) {
            if (membership.second != set) continue;
            const auto queue = guestMachMessageQueues.find(membership.first);
            if (queue != guestMachMessageQueues.end() && !queue->second.empty()) {
                return membership.first;
            }
        }
        return std::nullopt;
    };
    const auto deliverGuestMachPortToWaiters = [&](std::uint32_t port, std::uint32_t source) {
        auto waiting = guestMachReceiveWaiters.find(port);
        auto queue = guestMachMessageQueues.find(source);
        if (waiting == guestMachReceiveWaiters.end() || queue == guestMachMessageQueues.end() ||
            queue->second.empty()) return;
        auto& waiters = waiting->second;
        while (!waiters.empty() && !queue->second.empty()) {
            const auto threadId = waiters.front();
            waiters.pop_front();
            const auto blocked = guestMachBlockedReceives.find(threadId);
            auto* thread = guestScheduler.find(threadId);
            const auto* wait = thread != nullptr && thread->wait.has_value()
                ? std::get_if<GuestMachReceiveWait>(&*thread->wait) : nullptr;
            if (blocked == guestMachBlockedReceives.end() || wait == nullptr ||
                wait->port != port || blocked->second.port != port) continue;
            const auto result = copyGuestMachMessage(queue->second.front(), source,
                blocked->second.buffer, blocked->second.capacity,
                blocked->second.trailerSize);
            progressTrace.event("mach-message-wake", threadId, port);
            guestMachBlockedReceives.erase(blocked);
            auto delivered = result;
            if (delivered == 0) {
                if (!restoreGuestTransferredSendRight(queue->second.front())) {
                    delivered = machReceiveInvalidData;
                }
            } else if (delivered == machReceiveInvalidData) {
                (void)restoreTransferredRightAfterInvalidReceive(queue->second.front());
            }
            if (delivered == 0 || delivered == machReceiveTooLarge ||
                delivered == machReceiveInvalidData) {
                queue->second.pop_front();
            }
            guestScheduler.wake(threadId, static_cast<std::int32_t>(delivered));
        }
        if (waiters.empty()) guestMachReceiveWaiters.erase(waiting);
    };
    const auto deliverGuestMachMessage = [&](std::uint32_t port) {
        expireAndPruneGuestMachReceiveWaiters(monotonicNanoseconds());
        deliverGuestMachPortToWaiters(port, port);
        const auto membership = guestPortSetMembers.find(port);
        if (membership == guestPortSetMembers.end()) return;
        const auto set = membership->second;
        while (guestMachReceiveWaiters.find(set) != guestMachReceiveWaiters.end()) {
            const auto member = readyPortSetMember(set);
            if (!member) break;
            const auto before = guestMachMessageQueues[*member].size();
            deliverGuestMachPortToWaiters(set, *member);
            if (guestMachMessageQueues[*member].size() == before) break;
        }
    };
    fireDueMachTimers = [&]() {
        if (guestMachTimers.empty()) return;
        const auto now = mach_absolute_time();
        std::vector<std::uint32_t> due;
        for (auto timer = guestMachTimers.begin(); timer != guestMachTimers.end();) {
            if (guestReceiveRights.find(timer->first) == guestReceiveRights.end()) {
                timer = guestMachTimers.erase(timer);
                continue;
            }
            if (timer->second.deadline && *timer->second.deadline <= now) {
                timer->second.deadline.reset();
                due.push_back(timer->first);
            }
            ++timer;
        }
        for (const auto name : due) {
            constexpr std::uint32_t timerMessageBits = MACH_MSG_TYPE_COPY_SEND;
            constexpr std::uint32_t timerMessageSize = sizeof(mach_msg_header_t) + 24;
            guestMachMessageQueues[name].push_back(GuestLocalMessage{
                timerMessageSize, timerMessageBits, 0, 0, 0,
                std::vector<std::uint8_t>(24, 0), {}});
            deliverGuestMachMessage(name);
        }
    };
    earliestMachTimerNanoseconds = [&]() -> std::optional<std::uint64_t> {
        std::optional<std::uint64_t> earliest;
        for (const auto& timer : guestMachTimers) {
            if (!timer.second.deadline) continue;
            if (!earliest || *timer.second.deadline < *earliest) earliest = timer.second.deadline;
        }
        if (!earliest) return std::nullopt;
        const auto now = mach_absolute_time();
        std::uint64_t remaining = *earliest > now ? *earliest - now : 0;
        mach_timebase_info_data_t base{};
        (void)mach_timebase_info(&base);
        if (base.denom != 0) {
            remaining = remaining > std::numeric_limits<std::uint64_t>::max() / base.numer
                ? std::numeric_limits<std::uint64_t>::max() / base.denom
                : remaining * base.numer / base.denom;
        }
        const auto nanoseconds = monotonicNanoseconds();
        return nanoseconds + std::min(remaining,
            std::numeric_limits<std::uint64_t>::max() - nanoseconds);
    };
    std::uint32_t nextGuestReceiveName = 0x80000003U;
    std::optional<std::uint32_t> guestTaskNamePort;
    const auto getGuestTaskNamePort = [&]() -> std::optional<std::uint32_t> {
        if (guestTaskNamePort.has_value()) return guestTaskNamePort;
        while (nextGuestReceiveName < std::numeric_limits<std::uint32_t>::max() - 4 &&
               (guestReceiveRights.count(nextGuestReceiveName) != 0 ||
                guestSendRightUrefs.count(nextGuestReceiveName) != 0)) {
            nextGuestReceiveName += 4;
        }
        if (nextGuestReceiveName >= std::numeric_limits<std::uint32_t>::max() - 4) {
            return std::nullopt;
        }
        guestTaskNamePort = nextGuestReceiveName;
        nextGuestReceiveName += 4;
        return guestTaskNamePort;
    };
    struct GuestBsdthreadRegistration {
        std::uint64_t threadStart;
        std::uint64_t workqueueStart;
        std::uint64_t pthreadSize;
    };
    std::optional<GuestBsdthreadRegistration> guestBsdthreadRegistration;
    hostServiceState.allocateGuestBootstrapSendRight = [&](std::uint32_t hostPort)
        -> std::optional<std::uint32_t> {
        while (nextGuestReceiveName < std::numeric_limits<std::uint32_t>::max() - 4 &&
               (guestReceiveRights.count(nextGuestReceiveName) != 0 ||
                guestSendRightUrefs.count(nextGuestReceiveName) != 0)) {
            nextGuestReceiveName += 4;
        }
        if (nextGuestReceiveName >= std::numeric_limits<std::uint32_t>::max() - 4) {
            (void)mach_port_deallocate(mach_task_self(), hostPort);
            return std::nullopt;
        }
        const auto name = nextGuestReceiveName;
        nextGuestReceiveName += 4;
        try {
            const auto [right, inserted] = guestSendRightUrefs.emplace(name, 1U);
            (void)right;
            if (!inserted) {
                (void)mach_port_deallocate(mach_task_self(), hostPort);
                return std::nullopt;
            }
            const auto [owned, hostInserted] =
                hostServiceState.guestBootstrapSendRights.emplace(name, hostPort);
            (void)owned;
            if (!hostInserted) {
                guestSendRightUrefs.erase(name);
                (void)mach_port_deallocate(mach_task_self(), hostPort);
                return std::nullopt;
            }
        } catch (...) {
            guestSendRightUrefs.erase(name);
            (void)mach_port_deallocate(mach_task_self(), hostPort);
            throw;
        }
        return name;
    };
    mach_models::GuestMachModels machModels([&] {
        mach_models::Environment environment;
        environment.hostName = static_cast<std::uint32_t>(guestMachHostPort);
        environment.taskName = static_cast<std::uint32_t>(guestMachTaskPort);
        environment.holdsSendRight = [&](std::uint32_t name) {
            const auto right = guestSendRightUrefs.find(name);
            return right != guestSendRightUrefs.end() && right->second != 0;
        };
        environment.allocateName = [&]() -> std::uint32_t {
            while (nextGuestReceiveName < std::numeric_limits<std::uint32_t>::max() - 4 &&
                   (guestReceiveRights.count(nextGuestReceiveName) != 0 ||
                    guestSendRightUrefs.count(nextGuestReceiveName) != 0)) {
                nextGuestReceiveName += 4;
            }
            if (nextGuestReceiveName >= std::numeric_limits<std::uint32_t>::max() - 4) return 0;
            const auto name = nextGuestReceiveName;
            nextGuestReceiveName += 4;
            guestSendRightUrefs.emplace(name, 0U);
            return name;
        };
        environment.acquire = [&](std::uint32_t name) { (void)acquireGuestFixedSendRight(name); };
        environment.threadName = [&](std::uint64_t threadId) -> std::uint32_t {
            const auto existing = guestThreadPortNames.find(threadId);
            if (existing != guestThreadPortNames.end()) return existing->second;
            const auto name = acquireGuestThreadSendRight(threadId);
            if (name != 0 && guestSendRightUrefs[name] != 0) --guestSendRightUrefs[name];
            return name;
        };
        environment.threadState = [&](std::uint64_t threadId, std::uint64_t* state) {
            const std::uint64_t* source = nullptr;
            if (threadId == guestScheduler.current_id()) {
                source = static_cast<const std::uint64_t*>(stateBuffer.data());
            } else if (const auto* thread = guestScheduler.find(threadId);
                       thread != nullptr && thread->registers.size() > 33) {
                source = thread->registers.data();
            }
            if (source == nullptr) return false;
            std::copy(source, source + 32, state);
            state[32] = source[33];
            state[33] = 0;
            return true;
        };
        environment.portTypes = [&]() {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> ports;
            std::set<std::uint32_t> names;
            for (const auto& entry : guestReceiveRights) names.insert(entry.first);
            for (const auto& entry : guestSendRightUrefs) {
                if (entry.second != 0) names.insert(entry.first);
            }
            for (const auto name : names) {
                std::uint32_t type = 0;
                const auto receive = guestReceiveRights.find(name);
                const auto send = guestSendRightUrefs.find(name);
                const bool hasSend = send != guestSendRightUrefs.end() && send->second != 0;
                if (receive != guestReceiveRights.end()) {
                    type |= receive->second.portSet ? 0x80000U : 0x20000U;
                }
                if (hasSend) {
                    type |= guestDeadPortNames.count(name) != 0 ? 0x100000U : 0x10000U;
                }
                ports.emplace_back(name, type);
            }
            return ports;
        };
        environment.allocateGuest = [&](const void* bytes, std::size_t size,
                                        std::uint64_t& address) {
            if (!hostServiceState.machVmAllocate || size == 0) return false;
            const auto page = bootstrap.pageSize;
            const auto rounded = (size + page - 1) & ~(page - 1);
            if (hostServiceState.machVmAllocate(rounded, page, 3, 7, address, false, 1, 0) !=
                0) {
                return false;
            }
            try {
                std::memcpy(guestMemory.buffer(address, size, true), bytes, size);
            } catch (const std::exception&) {
                return false;
            }
            return true;
        };
        environment.destroyPort = [&](std::uint32_t name) -> std::int32_t {
            const auto receive = guestReceiveRights.find(name);
            const auto send = guestSendRightUrefs.find(name);
            const bool hasSend = send != guestSendRightUrefs.end() && send->second != 0;
            if (receive == guestReceiveRights.end() && !hasSend) return KERN_INVALID_NAME;
            if (receive != guestReceiveRights.end()) {
                const auto waiting = guestMachReceiveWaiters.find(name);
                if (waiting != guestMachReceiveWaiters.end()) {
                    for (const auto threadId : waiting->second) {
                        const auto blocked = guestMachBlockedReceives.find(threadId);
                        auto* thread = guestScheduler.find(threadId);
                        const auto* receiveWait = thread != nullptr && thread->wait.has_value()
                            ? std::get_if<GuestMachReceiveWait>(&*thread->wait) : nullptr;
                        if (blocked != guestMachBlockedReceives.end() &&
                            receiveWait != nullptr && receiveWait->port == name) {
                            guestMachBlockedReceives.erase(blocked);
                            guestScheduler.wake(threadId,
                                static_cast<std::int32_t>(machReceivePortChanged));
                        }
                    }
                    guestMachReceiveWaiters.erase(waiting);
                }
                guestReceiveRights.erase(receive);
                guestMachMessageQueues.erase(name);
                guestPortSetMembers.erase(name);
                guestPortGuards.erase(name);
            }
            if (send != guestSendRightUrefs.end()) guestSendRightUrefs.erase(send);
            guestDeadPortNames.erase(name);
            hostServiceState.guestBootstrapSendRights.erase(name);
            return KERN_SUCCESS;
        };
        environment.threadHandle = [&](std::uint64_t threadId) -> std::uint64_t {
            const auto* thread = guestScheduler.find(threadId);
            return thread == nullptr ? 0 : static_cast<std::uint64_t>(thread->tsdAddress);
        };
        environment.readGuest = [&](std::uint64_t address, std::size_t size)
            -> const std::uint8_t* {
            try {
                if (!guestMemory.is_guest_readable(address, size)) return nullptr;
                return guestMemory.buffer(address, size);
            } catch (const std::exception&) {
                return nullptr;
            }
        };
        environment.writeGuest = [&](std::uint64_t address, std::size_t size) -> std::uint8_t* {
            try {
                if (!guestMemory.is_guest_readable(address, size) ||
                    guestMemory.is_readonly(address, size)) {
                    return nullptr;
                }
                return guestMemory.buffer(address, size, true);
            } catch (const std::exception&) {
                return nullptr;
            }
        };
        return environment;
    }());
    struct GuestWorkqueueDispatchConfig {
        std::uint32_t version;
        std::uint32_t flags;
        std::uint64_t queueSerialNumberOffset;
        std::uint64_t queueLabelOffset;
    };
    std::optional<GuestWorkqueueDispatchConfig> guestWorkqueueDispatchConfig;
    bool guestWorkqueueOpened = false;
    struct RestartableRange { std::uint64_t begin; std::uint64_t end; std::uint64_t recovery; };
    std::vector<RestartableRange> restartableRanges;
    struct GuestCallbackFrame {
        std::vector<std::uint64_t> savedRegisters;
        std::vector<std::uint64_t> savedVectorAndAuth;
        std::uint64_t authenticationDepth = 0;
        std::uint64_t mutationBlock = 0;
        bool tproScope = false;
        bool dyldBootstrapCallback = false;
        std::vector<std::uint64_t> objcHeaders;
        std::uint64_t callback = 0;
        std::vector<std::array<std::uint64_t, 4>> arguments;
        std::size_t nextArgument = 0;
        std::vector<std::uint64_t> initializerTargets;
        std::size_t nextInitializer = 0;
        std::vector<GuestImageNotification> imageNotifications;
        std::size_t nextImageNotification = 0;
        std::vector<std::uint64_t> bulkImageCallbacks;
        std::size_t nextBulkImageCallback = 0;
        std::vector<std::uint64_t> temporaryGuestAllocations;
        std::vector<std::uint64_t> activeBulkImageCallbackLease;
        bool registerBulkImageCallbackAfterReturn = false;
        std::optional<std::uint64_t> stopAddress;
        std::optional<std::uint64_t> exitDso;
        std::vector<std::uint64_t> exitImages;
        std::vector<std::uint64_t> drainedExitImages;
        std::size_t nextExitImage = 0;
        std::optional<std::uint64_t> currentExitImage;
        std::optional<std::uint64_t> activeExitCallbackImage;
        std::vector<std::uint64_t> activeExitLease;
        std::optional<std::int32_t> exitStatus;
        std::optional<std::uint64_t> exitThreadResult;
        std::uint64_t exitThread = 0;
    };
    std::unordered_map<std::uint64_t, std::vector<GuestCallbackFrame>> callbackFramesByThread;
    GuestExitRegistry exitRegistry;
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> dynamicImageGroups;
    std::unordered_map<std::uint64_t, std::size_t> dynamicImageOwners;
    std::set<std::uint64_t> finalizedDynamicImages;
    std::unordered_map<std::uint64_t, std::size_t> activeImageExitCallbacks;
    std::set<std::uint64_t> pendingDynamicUnloads;
    std::set<std::pair<std::int64_t, std::int64_t>> cachedReadWindows;
    CacheBackingLeases cacheMetadataBackings;
    auto ensureTokenCapacity = [&](std::size_t additional, const char* origin) {
        auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
        const auto count = static_cast<std::size_t>(live[guestTokenCount]);
        auto capacity = static_cast<std::size_t>(live[guestTokenCapacity]);
        if (additional <= capacity - std::min(capacity, count)) return;
        if (count > std::numeric_limits<std::size_t>::max() - additional) {
            throw std::runtime_error(std::string("Guest authentication table overflow at ") + origin);
        }
        const auto required = count + additional;
        // Token indices are part of signed pointers; recycling them could validate stale values.
        const auto maximumWords = std::min(backend.maxBufferLength() / sizeof(std::uint64_t),
                                           processRegisters.max_size());
        if (maximumWords <= guestTokenStart) {
            throw std::runtime_error("Metal device buffer limit cannot hold guest authentication state");
        }
        const auto maximumCapacity = std::min(
            (maximumWords - guestTokenStart) / 3, kGuestTokenCodecCapacity);
        if (required > maximumCapacity) {
            throw std::runtime_error(
                std::string("Guest authentication table growth at ") + origin +
                " requires " + std::to_string(count) + " + " +
                std::to_string(additional) + " entries, exceeding limit " +
                std::to_string(maximumCapacity) + " (Metal buffer / token codec)");
        }
        const auto doubledCapacity = capacity > maximumCapacity / 2
            ? maximumCapacity : capacity * 2;
        capacity = std::max(required, std::max<std::size_t>(doubledCapacity, 1));
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
        ensureTokenCapacity(authenticatedCount, "named-image publication");
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
        std::vector<std::vector<std::uint64_t>> unloadBoundSlots(plan.images.size());
        std::vector<std::vector<std::uint64_t>> unloadSyntheticImportStubs(
            plan.images.size());
        std::vector<std::vector<std::size_t>> unloadAuthenticationTokens(
            plan.images.size());
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
        const auto makeImportStub = [&](const std::string& name,
                                        std::vector<std::uint64_t>& ownedAddresses) {
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
            ownedAddresses.push_back(address);
            return address;
        };
        for (std::size_t index = 0; index < plan.images.size(); ++index) {
            auto& staged = plan.images[index];
            auto& ownedBoundSlots = unloadBoundSlots[index];
            auto& ownedSyntheticStubs = unloadSyntheticImportStubs[index];
            auto& ownedAuthenticationTokens = unloadAuthenticationTokens[index];
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
                ownedBoundSlots.push_back(bind.guest_slot);
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
                    target = makeImportStub(bind.resolved_name, ownedSyntheticStubs);
                } else if (!resolvedTarget && !bind.weak_null) {
                    throw std::runtime_error("Unresolved staged guest binding " +
                                             bind.resolved_name);
                }
                if ((target != 0 || bind.weak_null) && bind.imported.addend != 0) {
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
                ownedBoundSlots.push_back(staged.guest_header + offset);
                ownedAuthenticationTokens.push_back(token);
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
        for (std::size_t index = 0; index < plan.images.size(); ++index) {
            const auto& staged = plan.images[index];
            LoadedImageGraphRecord record;
            record.guest_header = staged.guest_header;
            record.canonical_path = staged.canonical_path;
            record.rpaths = staged.rpaths;
            record.exports = staged.image.exports;
            record.backing_offset = static_cast<std::size_t>(allocations[index]);
            record.image_size = staged.image_bytes;
            record.initializer_targets = staged.initializer_targets;
            record.finalizer_targets = staged.finalizer_targets;
            record.binding_slots = std::move(unloadBoundSlots[index]);
            record.synthetic_import_stubs =
                std::move(unloadSyntheticImportStubs[index]);
            record.authentication_tokens =
                std::move(unloadAuthenticationTokens[index]);
            record.dynamic_unload_candidate = true;
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
            for (const auto& segment : staged.image.mapped_segments) {
                if (segment.size == 0) continue;
                const auto begin = staged.guest_header + segment.offset;
                record.mapped_ranges.emplace_back(begin, begin + segment.size);
            }
            image.loaded_image_graph.push_back(std::move(record));
        }
        return initializerTargets;
    };
    const auto resizeGuestBuffer = [&](GuestMemory& guestMemoryView,
                                      std::size_t minimumSize, bool geometric) {
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
        auto targetSize = geometric ? std::max(roundedMinimum, doubled) : roundedMinimum;
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
    hostServiceState.growGuestMemory = [&](GuestMemory& guestMemoryView,
                                            std::size_t minimumSize) {
        return resizeGuestBuffer(guestMemoryView, minimumSize, true);
    };
    const auto extendVmBacking = [&](std::uint64_t bytes) {
        const auto oldSize = guestMemory.size();
        if (bytes == 0 || oldSize != hostServiceState.heapEnd ||
            oldSize > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
            bytes > std::numeric_limits<std::size_t>::max() - oldSize ||
            resident[0] >= residentCapacity) {
            return false;
        }
        const auto minimumSize = oldSize + static_cast<std::size_t>(bytes);
        const auto deviceLimit = backend.maxBufferLength();
        const auto linearLimit = static_cast<std::size_t>(SharedCache::guestBase - 1);
        const auto maximum = std::min(deviceLimit, linearLimit);
        const auto pageSize = static_cast<std::size_t>(bootstrap.pageSize);
        if (minimumSize <= oldSize || minimumSize > maximum || pageSize == 0 ||
            (pageSize & (pageSize - 1)) != 0 ||
            minimumSize > std::numeric_limits<std::size_t>::max() - (pageSize - 1)) {
            return false;
        }
        const auto newSize = (minimumSize + pageSize - 1) & ~(pageSize - 1);
        if (newSize > maximum || newSize > static_cast<std::size_t>(
                std::numeric_limits<std::int64_t>::max())) return false;
        try {
            auto stagedMappingHeap = mappingHeap;
            stagedMappingHeap.extend({static_cast<std::int64_t>(oldSize),
                                      static_cast<std::int64_t>(newSize)});
            auto stagedResident = resident;
            const auto row = 1 + 4 * static_cast<std::size_t>(stagedResident[0]);
            stagedResident[row] = oldSize;
            stagedResident[row + 1] = newSize;
            stagedResident[row + 2] = oldSize;
            stagedResident[row + 3] = 2U;
            ++stagedResident[0];

            auto expanded = backend.makeSharedBuffer(newSize);
            if (expanded.data() == nullptr) return false;
            const auto* previous = static_cast<const std::uint8_t*>(memoryBuffer.data());
            std::memcpy(expanded.data(), previous, oldSize);
            std::memset(static_cast<std::uint8_t*>(expanded.data()) + oldSize, 0,
                        newSize - oldSize);
            guestMemory.rebind_with_mapping(
                static_cast<std::uint8_t*>(expanded.data()), newSize,
                {oldSize, newSize, oldSize, false, false});
            memoryBuffer = std::move(expanded);
            mappingHeap = std::move(stagedMappingHeap);
            resident.swap(stagedResident);
            hostServiceState.heapEnd = newSize;
            const auto newGuestSize = static_cast<std::uint64_t>(newSize);
            std::memcpy(sizeBuffer.data(), &newGuestSize, sizeof(newGuestSize));
            std::memcpy(residentBuffer.data(), resident.data(),
                        resident.size() * sizeof(std::uint64_t));
            return true;
        } catch (...) {
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
    bool writableTproCallbackCompleted = false;
    bool dyldRuntimeInitialized = false;
    const auto initializeDyldRuntimeState = [&]() {
        if (!dyldApis.object || !writableTproCallbackCompleted || dyldRuntimeInitialized) {
            throw std::runtime_error("Invalid _libdyld_initialize lifecycle order");
        }
        // Native helper installation maps to the runner's existing pthread
        // mutex host services backed by GuestScheduler.
        std::uint64_t initializedDescriptors = 0;
        const GuestTlvRuntime::Allocate allocateTlv =
            [&](std::uint64_t size, std::uint64_t alignment)
            -> std::optional<std::uint64_t> {
            const auto allocation = allocateGuestHeap(size, alignment);
            if (!allocation || *allocation < 0) return std::nullopt;
            return static_cast<std::uint64_t>(*allocation);
        };
        const auto initializeDescriptor = [&](std::uint64_t address,
                                              std::uint32_t expectedKey,
                                              std::uint64_t templateSize,
                                              bool diskV1) {
            const auto* raw = guestMemory.buffer(address, 24);
            std::uint64_t key = 0;
            std::uint64_t offset = 0;
            if (diskV1) {
                std::memcpy(&key, raw + 8, sizeof(key));
                if (key != 0) throw std::runtime_error("Invalid disk-v1 initial-image TLV key");
                std::memcpy(&offset, raw + 16, sizeof(offset));
                key = expectedKey;
            } else {
                std::uint32_t runtimeKey = 0;
                std::uint32_t runtimeOffset = 0;
                std::memcpy(&runtimeKey, raw + 8, sizeof(runtimeKey));
                std::memcpy(&runtimeOffset, raw + 12, sizeof(runtimeOffset));
                key = runtimeKey;
                offset = runtimeOffset;
            }
            if (key != expectedKey || offset >= templateSize) {
                throw std::runtime_error("Invalid initial-image TLV descriptor");
            }
            if (key >= GuestTlvRuntime::guestTsdSlotCount) {
                throw std::runtime_error("Initial-image TLV key exceeds guest TSD capacity");
            }
            const auto storage = guestTlvRuntime.resolve(
                bootstrap.mainThreadSpecificData, key, offset, allocateTlv,
                guestTlvRelease, guestTlvRead, guestTlvWrite);
            if (!storage) throw std::runtime_error("Unable to initialize initial-image TLV");
            std::uint64_t published = 0;
            const auto* slot = guestMemory.buffer(
                bootstrap.mainThreadSpecificData +
                    static_cast<std::uint64_t>(key) * sizeof(published), sizeof(published));
            std::memcpy(&published, slot, sizeof(published));
            if (published == 0 ||
                published > std::numeric_limits<std::uint64_t>::max() - offset ||
                published + offset != *storage) {
                dyldBootstrapTrace.event("initial-tlv-publication-mismatch",
                                         initializedDescriptors);
                throw std::runtime_error(
                    "Initial-image TLV TSD base disagrees with its resolved descriptor address");
            }
            ++initializedDescriptors;
        };
        for (const auto& owner : guestTlvDescriptors) {
            for (auto address = owner.begin; address < owner.end; address += 24) {
                initializeDescriptor(address, owner.key, owner.templateSize, false);
            }
        }
        for (const auto& [header, metadata] : image.thread_local_images) {
            if (!metadata.from_shared_cache || metadata.thunk_ranges.empty()) continue;
            if (!metadata.init_callback_ranges.empty()) {
                throw std::runtime_error("Cached initial-image TLS callbacks are unsupported");
            }
            const bool diskV1 = image.cache_reader->newFormatTlvs() !=
                std::optional<bool>{true};
            const auto assignedFallback = cachedV1TlvKeys.find(header);
            const bool zeroKeyV2Fallback = !diskV1 &&
                assignedFallback != cachedV1TlvKeys.end();
            if (zeroKeyV2Fallback) {
                const auto path = image.image_paths.find(header);
                dyldBootstrapTrace.metadata("cached-tlv-fallback",
                    path == image.image_paths.end() ? std::string("<unknown>") : path->second,
                    "cache-v2", "disk-v1-key-zero-unoptimized");
            }
            if (metadata.template_size == 0 || metadata.template_alignment == 0 ||
                (metadata.template_alignment & (metadata.template_alignment - 1)) != 0) {
                throw std::runtime_error("Invalid cached initial-image TLV geometry");
            }
            std::optional<std::uint32_t> imageKey;
            for (const auto& [begin, length] : metadata.thunk_ranges) {
                if (length % 24 != 0) throw std::runtime_error("Malformed cached TLV thunk range");
                for (std::uint64_t offset = 0; offset < length; offset += 24) {
                    const auto* raw = guestMemory.buffer(begin + offset, 24);
                    std::uint32_t key = 0;
                    std::memcpy(&key, raw + 8, sizeof(key));
                    if (key == 0) {
                        if (assignedFallback == cachedV1TlvKeys.end()) {
                            const auto path = image.image_paths.find(header);
                            std::uint64_t diskKey = 0;
                            const auto diskDescriptor = image.cache_reader->bytes(
                                image.cache_reader->originalAddress(
                                    static_cast<std::int64_t>(begin + offset)), 8);
                            if (diskDescriptor.size() == sizeof(diskKey)) {
                                std::memcpy(&diskKey, diskDescriptor.data(), sizeof(diskKey));
                            }
                            dyldBootstrapTrace.metadata("cached-tlv-missing-key",
                                path == image.image_paths.end() ? std::string("<unknown>") : path->second,
                                diskV1 ? "cache-v1" : "cache-v2",
                                diskKey == 0
                                    ? (diskV1 ? "disk-v1-key-zero-no-assignment"
                                              : "cache-v2-disk-v1-key-zero-no-assignment")
                                    : (diskV1 ? "cache-v1-resident-key-zero-disk-key-nonzero"
                                              : "cache-v2-resident-key-zero-disk-key-nonzero"));
                            throw std::runtime_error("Cached initial-image TLV has no key");
                        }
                        key = assignedFallback->second;
                    }
                    if (key == 0) throw std::runtime_error("Cached initial-image TLV has no key");
                    if (imageKey && *imageKey != key) {
                        throw std::runtime_error("Cached initial-image TLV keys disagree");
                    }
                    imageKey = key;
                }
            }
            if (!imageKey) continue;
            if (registeredCachedTlvImages.insert(header).second) {
                std::vector<std::uint8_t> initial(
                    static_cast<std::size_t>(metadata.template_size), 0);
                for (const auto& [address, size] : metadata.template_ranges) {
                    if (address < metadata.template_address ||
                        address - metadata.template_address > metadata.template_size ||
                        size > metadata.template_size - (address - metadata.template_address)) {
                        throw std::runtime_error("Cached initial-image TLV template is out of bounds");
                    }
                    const auto* source = guestMemory.buffer(address, size);
                    std::copy_n(source, static_cast<std::size_t>(size),
                        initial.begin() + static_cast<std::ptrdiff_t>(
                            address - metadata.template_address));
                }
                guestTlvRuntime.register_image(*imageKey, std::move(initial), 0,
                                                metadata.template_alignment);
            }
            for (const auto& [begin, length] : metadata.thunk_ranges) {
                for (std::uint64_t offset = 0; offset < length; offset += 24) {
                    initializeDescriptor(begin + offset, *imageKey,
                                          metadata.template_size,
                                          diskV1 || zeroKeyV2Fallback);
                }
            }
        }
        dlerrorByThread.try_emplace(guestScheduler.current_id());
        dyldRuntimeInitialized = true;
        return initializedDescriptors;
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
    const auto canonicalSharedCachePath = [&](const std::string& requestedPath)
        -> std::optional<std::string> {
        const auto* cache = image.metadata_cache_reader();
        if (cache == nullptr) return std::nullopt;
        auto canonical = cache->canonicalImagePath(requestedPath);
        if (!canonical) {
            std::error_code error;
            const auto resolvedPath = std::filesystem::canonical(requestedPath, error);
            if (!error) canonical = cache->canonicalImagePath(resolvedPath.string());
        }
        return canonical;
    };
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
        selectKernel(static_cast<std::int64_t>(startup[startupIndex]));
        lastKernelSelectionPath = "startup-advance";
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
        ensureTokenCapacity(plan->authentication_count, "cache residency");
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
        if (dyldApis.publicationCell != 0) {
            const bool cellInPlan =
                dyldApis.publicationCell >= plan->logical_start &&
                static_cast<std::uint64_t>(dyldApis.publicationCell -
                    plan->logical_start) <=
                    static_cast<std::uint64_t>(plan->logical_end - plan->logical_start) &&
                sizeof(std::uint64_t) <= static_cast<std::uint64_t>(
                    plan->logical_end - dyldApis.publicationCell);
            patchWord(static_cast<std::uint64_t>(dyldApis.publicationCell),
                      dyldApis.object, sizeof(std::uint64_t));
            if (cellInPlan && !dyldApisPublicationSeeded) {
                dyldApisPublicationSeeded = true;
                dyldBootstrapTrace.event("gapis-seeded");
            }
        }
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
    const auto stagedImageEnumerationOrder = [](const StagedNamedLoad& plan) {
        std::vector<GuestLoadedImage> result;
        std::map<std::uint64_t, std::size_t> localByHeader;
        std::map<std::string, std::size_t> cacheByName;
        std::map<std::uint64_t, std::size_t> cacheByHeader;
        for (std::size_t index = 0; index < plan.images.size(); ++index)
            localByHeader.emplace(plan.images[index].guest_header, index);
        for (std::size_t index = 0; index < plan.cache_images.size(); ++index) {
            cacheByName.emplace(plan.cache_images[index].install_name, index);
            cacheByHeader.emplace(plan.cache_images[index].guest_header, index);
        }
        std::set<std::size_t> visitedLocal;
        std::set<std::size_t> visitedCache;
        std::function<void(std::size_t)> visitLocal;
        std::function<void(std::size_t)> visitCache;
        visitCache = [&](std::size_t index) {
            if (!visitedCache.insert(index).second) return;
            const auto& staged = plan.cache_images[index];
            result.push_back({staged.guest_header, staged.install_name, true});
            for (const auto& dependency : staged.image.dependencies) {
                if (const auto child = cacheByName.find(dependency.name);
                    child != cacheByName.end()) visitCache(child->second);
            }
        };
        visitLocal = [&](std::size_t index) {
            if (!visitedLocal.insert(index).second) return;
            const auto& staged = plan.images[index];
            result.push_back({staged.guest_header, staged.canonical_path, false});
            for (const auto& dependency : staged.dependencies) {
                if (dependency.staged_image_index &&
                    *dependency.staged_image_index < plan.images.size()) {
                    visitLocal(*dependency.staged_image_index);
                } else if (const auto child = cacheByName.find(dependency.declaration.name);
                           child != cacheByName.end()) {
                    visitCache(child->second);
                }
            }
        };
        if (const auto local = localByHeader.find(plan.requested_header);
            local != localByHeader.end()) {
            visitLocal(local->second);
        } else if (const auto cached = cacheByHeader.find(plan.requested_header);
                   cached != cacheByHeader.end()) {
            visitCache(cached->second);
        }
        for (std::size_t index = 0; index < plan.cache_images.size(); ++index)
            visitCache(index);
        for (std::size_t index = 0; index < plan.images.size(); ++index)
            visitLocal(index);
        return result;
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
        if (replacement.changed &&
            jitTraceGeneration < std::numeric_limits<std::uint64_t>::max()) {
            ++jitTraceGeneration;
        }
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
        table[row + 3] = (region.writable ? 1U : 0U) |
                         (region.readable ? 0U : 2U) | (region.tpro ? 4U : 0U);
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
    const auto reclaimRetiredCacheMetadataBackings = [&]() {
        std::vector<GuestBackingSpan> residentBackings;
        residentBackings.reserve(static_cast<std::size_t>(resident[0]));
        for (std::size_t index = 0; index < resident[0]; ++index) {
            const auto row = 1 + 4 * index;
            residentBackings.push_back({static_cast<std::int64_t>(resident[row + 2]),
                                        resident[row + 1] - resident[row]});
        }
        std::vector<GuestBackingSpan> activeVmBackings;
        for (const auto& [unused, allocation] : vmAllocations) {
            (void)unused;
            for (const auto& lease : allocation.ownedBackings) {
                activeVmBackings.push_back({lease.backing, lease.size});
            }
        }
        const auto retired = cacheMetadataBackings.collect_unreferenced(
            residentBackings, activeVmBackings);
        for (const auto backing : retired) mappingHeap.release(backing);
    };
    const auto setTproWrites = [&](bool enabled) {
        for (std::size_t index = 0; index < resident[0]; ++index) {
            const auto row = 1 + 4 * index;
            if ((resident[row + 3] & 4U) == 0) continue;
            if (enabled) resident[row + 3] |= 1U;
            else resident[row + 3] &= ~std::uint64_t{1};
            guestMemory.set_mapping_permissions(resident[row], true, enabled);
        }
        syncResidentTable();
    };
    const auto syncTproThreadAccess = [&]() {
        const auto thread = guestScheduler.current_id();
        const auto found = callbackFramesByThread.find(thread);
        const bool enabled = found != callbackFramesByThread.end() &&
            std::any_of(found->second.begin(), found->second.end(),
                [](const GuestCallbackFrame& frame) { return frame.tproScope; });
        const auto desiredThread = enabled ? std::optional<std::uint64_t>{thread} : std::nullopt;
        if (desiredThread == tproWritableThread) return;
        setTproWrites(enabled);
        tproWritableThread = desiredThread;
    };
    const auto setObjCCacheWrites = [&](bool enabled) {
        if (!image.cache_reader) return;
        if (enabled) {
            for (std::size_t index = 0; index < resident[0]; ++index) {
                const auto row = 1 + 4 * index;
                if ((resident[row + 3] & 6U) != 0 || (resident[row + 3] & 1U) != 0) continue;
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
        for (auto& frame : found->second) {
            releasedMutation = releasedMutation || frame.mutationBlock != 0;
            if (frame.stopAddress && hostServiceState.heap) {
                hostServiceState.heap->release(static_cast<std::int64_t>(*frame.stopAddress));
            }
            for (const auto allocation : frame.temporaryGuestAllocations) {
                auto freeRegisters = frame.savedRegisters;
                freeRegisters[0] = allocation;
                (void)dispatch_guest_host_service(
                    freeRegisters, guestMemory, bootstrap, hostServiceState, 56);
            }
            for (const auto header : frame.activeBulkImageCallbackLease) {
                const auto owner = dynamicImageOwners.find(header);
                if (owner == dynamicImageOwners.end() || owner->second == 0) continue;
                if (--owner->second == 0) {
                    dynamicImageOwners.erase(owner);
                    if (!imageHandles.nodelete(header) &&
                        !imageHandles.has_open_reference_for_image(header)) {
                        pendingDynamicUnloads.insert(header);
                    }
                }
            }
            frame.activeBulkImageCallbackLease.clear();
            if (frame.activeExitCallbackImage) {
                const auto active = activeImageExitCallbacks.find(
                    *frame.activeExitCallbackImage);
                if (active != activeImageExitCallbacks.end() && active->second != 0 &&
                    --active->second == 0) {
                    activeImageExitCallbacks.erase(active);
                }
                frame.activeExitCallbackImage.reset();
            }
            for (const auto header : frame.activeExitLease) {
                const auto owner = dynamicImageOwners.find(header);
                if (owner == dynamicImageOwners.end() || owner->second == 0) continue;
                if (--owner->second == 0) {
                    dynamicImageOwners.erase(owner);
                    if (!imageHandles.nodelete(header) &&
                        !imageHandles.has_open_reference_for_image(header)) {
                        pendingDynamicUnloads.insert(header);
                    }
                }
            }
            frame.activeExitLease.clear();
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
        struct ExistingRange {
            VmRegion region;
            std::uint64_t flags;
            std::uint64_t overlapBegin;
            std::uint64_t overlapEnd;
        };
        std::vector<ExistingRange> overlaps;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> unmapped{{
            static_cast<std::uint64_t>(address), end}};
        for (std::size_t index = 0; index < resident[0]; ++index) {
            const auto row = 1 + 4 * index;
            const auto begin = resident[row];
            const auto currentEnd = resident[row + 1];
            const auto flags = resident[row + 3];
            const bool readable = (flags & 2U) == 0;
            const bool currentWritable = (flags & 1U) != 0;
            const auto overlapBegin = std::max(begin, static_cast<std::uint64_t>(address));
            const auto overlapEnd = std::min(currentEnd, end);
            if (overlapBegin >= overlapEnd) continue;
            if (!readable || (writable && !currentWritable)) {
                throw std::runtime_error(
                    "Cannot satisfy guest cache metadata range [0x" +
                    hexadecimal(static_cast<std::uint64_t>(address)) + ",0x" +
                    hexadecimal(end) + "): resident overlap [0x" +
                    hexadecimal(overlapBegin) + ",0x" + hexadecimal(overlapEnd) +
                    ") has flags=0x" + hexadecimal(flags) +
                    " requested-writable=" + (writable ? "true" : "false"));
            }
            const bool tpro = (flags & 4U) != 0;
            const auto backing = resident[row + 2];
            overlaps.push_back({{begin, currentEnd, static_cast<std::size_t>(backing),
                                 readable, currentWritable, false, 0, false, 1, 0, tpro},
                                flags, overlapBegin, overlapEnd});
            std::vector<std::pair<std::uint64_t, std::uint64_t>> remaining;
            remaining.reserve(unmapped.size() + 1);
            for (const auto& [gapBegin, gapEnd] : unmapped) {
                if (overlapEnd <= gapBegin || gapEnd <= overlapBegin) {
                    remaining.emplace_back(gapBegin, gapEnd);
                    continue;
                }
                if (gapBegin < overlapBegin)
                    remaining.emplace_back(gapBegin, overlapBegin);
                if (overlapEnd < gapEnd)
                    remaining.emplace_back(overlapEnd, gapEnd);
            }
            unmapped.swap(remaining);
        }
        if (overlaps.size() == 1 && overlaps.front().region.begin <=
                static_cast<std::uint64_t>(address) && end <= overlaps.front().region.end) {
            return;
        }
        bool mergedWritable = writable;
        bool mergedTpro = false;
        auto mergedBegin = static_cast<std::uint64_t>(address);
        auto mergedEnd = end;
        if (!overlaps.empty()) {
            mergedWritable = overlaps.front().region.writable;
            mergedTpro = overlaps.front().region.tpro;
            for (const auto& overlap : overlaps) {
                mergedBegin = std::min(mergedBegin, overlap.region.begin);
                mergedEnd = std::max(mergedEnd, overlap.region.end);
                if (overlap.region.writable != mergedWritable ||
                    overlap.region.tpro != mergedTpro) {
                    throw std::runtime_error(
                        "Cannot consolidate guest cache metadata range [0x" +
                        hexadecimal(static_cast<std::uint64_t>(address)) + ",0x" +
                        hexadecimal(end) + "): resident overlap [0x" +
                        hexadecimal(overlap.overlapBegin) + ",0x" +
                        hexadecimal(overlap.overlapEnd) + ") has flags=0x" +
                        hexadecimal(overlap.flags) +
                        " with incompatible mapping permissions");
                }
            }
            if (!unmapped.empty() &&
                (mergedWritable != writable || mergedTpro)) {
                throw std::runtime_error(
                    "Cannot consolidate guest cache metadata range [0x" +
                    hexadecimal(static_cast<std::uint64_t>(address)) + ",0x" +
                    hexadecimal(end) + "): cache-backed holes would change resident " +
                    "writability or TPRO state");
            }
            for (const auto& [unused, allocation] : vmAllocations) {
                (void)unused;
                const auto ownedOverlap = std::find_if(
                    allocation.regions.begin(), allocation.regions.end(),
                    [mergedBegin, mergedEnd](const VmRegion& region) {
                        return mergedBegin < region.end && region.begin < mergedEnd;
                    });
                if (ownedOverlap != allocation.regions.end()) {
                    throw std::runtime_error(
                        "Cannot consolidate guest cache metadata range [0x" +
                        hexadecimal(mergedBegin) + ",0x" + hexadecimal(mergedEnd) +
                        "): overlaps a VM allocation with an active backing lease");
                }
            }
        }
        const auto mergedLength = static_cast<std::size_t>(mergedEnd - mergedBegin);
        const auto backingAllocation = mappingHeap.allocate(
            static_cast<std::int64_t>(mergedLength), 16);
        if (!backingAllocation)
            throw std::runtime_error("Guest cache metadata backing is exhausted");
        const auto backing = static_cast<std::size_t>(*backingAllocation);
        if (backing > memoryBuffer.size() || mergedLength > memoryBuffer.size() - backing) {
            mappingHeap.release(*backingAllocation);
            throw std::runtime_error("Guest cache metadata exceeds process memory");
        }
        try {
            auto* destination = static_cast<std::uint8_t*>(memoryBuffer.data()) + backing;
            const auto cacheBytes = cache.bytes(cache.originalAddress(address), length);
            std::copy(cacheBytes.begin(), cacheBytes.end(),
                      destination + (static_cast<std::uint64_t>(address) - mergedBegin));
            for (const auto& overlap : overlaps) {
                const auto sourceOffset = overlap.region.backing;
                const auto rowLength = overlap.region.end - overlap.region.begin;
                if (sourceOffset > memoryBuffer.size() ||
                    rowLength > memoryBuffer.size() - sourceOffset) {
                    throw std::runtime_error("Guest cache metadata overlap backing is invalid");
                }
                std::memcpy(destination + (overlap.region.begin - mergedBegin),
                            static_cast<const std::uint8_t*>(memoryBuffer.data()) + sourceOffset,
                            static_cast<std::size_t>(rowLength));
            }
            auto stagedCacheMetadataBackings = cacheMetadataBackings;
            const auto allocatedSize = mappingHeap.size_of(*backingAllocation);
            if (!allocatedSize || *allocatedSize <= 0) {
                throw std::runtime_error("Guest cache metadata backing lease is missing");
            }
            stagedCacheMetadataBackings.track(
                *backingAllocation, static_cast<std::uint64_t>(*allocatedSize));
            if (overlaps.empty()) {
                if (resident[0] >= residentCapacity) {
                    throw std::runtime_error("Guest resident map capacity exceeded by cache metadata");
                }
                publishVmRegion({mergedBegin, mergedEnd, backing,
                                 true, mergedWritable, false, 0, false, 1, 0, mergedTpro});
                cacheMetadataBackings.swap(stagedCacheMetadataBackings);
            } else {
                std::vector<std::uint64_t> removedBegins;
                std::vector<GuestMemoryMapping> addedMappings;
                auto stagedResident = resident;
                for (const auto& overlap : overlaps) {
                    removedBegins.push_back(overlap.region.begin);
                    removeVmRegionFromResident(stagedResident, overlap.region);
                }
                const VmRegion merged{mergedBegin, mergedEnd, backing,
                    true, mergedWritable, false, 0, false, 1, 0, mergedTpro};
                appendVmRegionToResident(stagedResident, merged);
                addedMappings.push_back({merged.begin, merged.end, merged.backing,
                                         merged.writable, merged.readable});
                guestMemory.replace_mappings(removedBegins, addedMappings);
                resident.swap(stagedResident);
                cacheMetadataBackings.swap(stagedCacheMetadataBackings);
            }
        } catch (...) {
            mappingHeap.release(*backingAllocation);
            throw;
        }
        syncResidentTable();
        reclaimRetiredCacheMetadataBackings();
    };
    const auto publishGuestImageHeader = [&](std::size_t index) {
        const auto header = imageRegistry.header_at(index);
        if (header && imageRegistry.cache_backed_at(index)) {
            const auto* cache = image.metadata_cache_reader();
            if (cache == nullptr) {
                throw std::runtime_error("Guest cached image header has no shared-cache reader");
            }
            const auto headerBytes = cache->bytes(cache->originalAddress(
                static_cast<std::int64_t>(*header)), 32);
            const auto read32 = [&headerBytes](std::size_t offset) {
                std::uint32_t value = 0;
                for (unsigned byte = 0; byte < 4; ++byte) {
                    value |= static_cast<std::uint32_t>(headerBytes[offset + byte]) << (byte * 8U);
                }
                return value;
            };
            const auto commandCount = read32(16);
            const auto commandBytes = read32(20);
            if (commandBytes > 1024U * 1024U || commandCount > commandBytes / 8U) {
                throw std::runtime_error("Invalid cached Mach-O header metadata");
            }
            publishCacheMetadata(*cache, static_cast<std::int64_t>(*header),
                                 32U + commandBytes, false);
        }
        if (!header) throw std::logic_error("Guest image registry index disappeared");
        return *header;
    };
    const auto prepareBulkImageCallback = [&](GuestCallbackFrame& frame,
                                              const std::vector<std::uint64_t>& savedRegisters,
                                              std::vector<std::uint64_t> callbacks,
                                              std::size_t firstImage,
                                              std::size_t endImage,
                                              bool registerAfterReturn) {
        if (callbacks.empty()) return;
        if (endImage < firstImage || endImage - firstImage >
                std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("Guest bulk image callback count exceeds its ABI width");
        }
        const auto count = endImage - firstImage;
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t)) {
            throw std::runtime_error("Guest bulk image callback array size overflow");
        }
        std::vector<std::string> paths;
        paths.reserve(count);
        for (auto index = firstImage; index < endImage; ++index) {
            const auto name = imageRegistry.name_at(index);
            if (!name) throw std::logic_error("Guest image registry path disappeared");
            paths.emplace_back(*name);
        }
        const auto headersBytes = std::max<std::size_t>(sizeof(std::uint64_t),
            count * sizeof(std::uint64_t));
        const auto pointersBytes = headersBytes;
        std::vector<std::uint64_t> imageHeaders;
        std::vector<std::uint64_t> pathAddresses;
        imageHeaders.reserve(count);
        pathAddresses.reserve(count);
        for (std::size_t offset = 0; offset < count; ++offset) {
            imageHeaders.push_back(publishGuestImageHeader(firstImage + offset));
            const auto pathAddress = stableGuestString(paths[offset]);
            if (!pathAddress) {
                throw std::runtime_error("Guest bulk image callback path allocation failed");
            }
            pathAddresses.push_back(*pathAddress);
        }
        const auto allocateGuest = [&](std::size_t bytes) {
            auto allocationRegisters = savedRegisters;
            allocationRegisters[0] = bytes;
            const auto result = dispatch_guest_host_service(
                allocationRegisters, guestMemory, bootstrap, hostServiceState, 26);
            if (result.exited || allocationRegisters[0] == 0) {
                throw std::runtime_error("Guest bulk image callback allocation failed");
            }
            frame.temporaryGuestAllocations.push_back(allocationRegisters[0]);
            return allocationRegisters[0];
        };
        const auto headers = allocateGuest(headersBytes);
        const auto pathPointers = allocateGuest(pointersBytes);
        auto* headerData = guestMemory.buffer(headers, headersBytes, true);
        auto* pointerData = guestMemory.buffer(pathPointers, pointersBytes, true);
        for (std::size_t offset = 0; offset < count; ++offset) {
            std::memcpy(headerData + offset * sizeof(imageHeaders[offset]),
                        &imageHeaders[offset], sizeof(imageHeaders[offset]));
            std::memcpy(pointerData + offset * sizeof(pathAddresses[offset]),
                        &pathAddresses[offset], sizeof(pathAddresses[offset]));
        }
        frame.savedRegisters.assign(savedRegisters.begin(), savedRegisters.begin() + 36);
        frame.savedRegisters[34] = 0;
        const auto* live = static_cast<const std::uint64_t*>(stateBuffer.data());
        frame.savedVectorAndAuth.assign(live + 806, live + guestTokenCount);
        frame.authenticationDepth = live[37];
        frame.callback = callbacks.front();
        frame.bulkImageCallbacks = std::move(callbacks);
        frame.nextBulkImageCallback = 0;
        frame.arguments.push_back({static_cast<std::uint64_t>(count), headers,
                                   pathPointers, 0});
        frame.nextArgument = frame.arguments.size();
        frame.registerBulkImageCallbackAfterReturn = registerAfterReturn;
    };
    constexpr std::uint32_t kernSuccess = 0;
    constexpr std::uint32_t kernInvalidAddress = 1;
    constexpr std::uint32_t kernProtectionFailure = 2;
    constexpr std::uint32_t kernNoSpace = 3;
    constexpr std::uint32_t kernInvalidArgument = 4;
    constexpr std::uint32_t machSendInvalidDest = 0x10000003U;
    std::optional<std::size_t> kdebugTypeFilterBacking;
    std::uint64_t kdebugTypeFilterLogicalCursor = guestMappingBase + (std::uint64_t{1} << 32U);
    hostServiceState.guestMapJitEligible = image.main_code_policy.map_jit_eligible;
    hostServiceState.machVmReserve = [&](std::uint64_t address, std::uint64_t size,
                                          std::uint32_t currentProtection,
                                          std::uint32_t maximumProtection,
                                          std::uint32_t inheritance, std::uint32_t tag,
                                          bool jitMapping) {
        const auto pageSize = bootstrap.pageSize;
        if (pageSize == 0 || (pageSize & (pageSize - 1)) != 0 ||
            address % pageSize != 0 || size == 0 ||
            address > guestCodeLimit || size > guestCodeLimit - address ||
            inheritance > 2U) return kernInvalidArgument;
        const auto end = address + size;
        for (const auto& [unused, reservation] : vmReservations) {
            (void)unused;
            if (address < reservation.end && reservation.begin < end) return kernNoSpace;
        }
        for (const auto& [unused, allocation] : vmAllocations) {
            (void)unused;
            for (const auto& region : allocation.regions) {
                if (address < region.end && region.begin < end) return kernNoSpace;
            }
        }
        for (std::size_t index = 0; index < resident[0]; ++index) {
            const auto row = 1 + 4 * index;
            if (address < resident[row + 1] && resident[row] < end) return kernNoSpace;
        }
        if ((currentProtection & ~maximumProtection) != 0) return kernInvalidArgument;
        if ((currentProtection & 6U) == 6U && !jitMapping) return kernProtectionFailure;
        if (currentProtection != 0) {
            if (resident[0] >= residentCapacity || pageSize > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) return kernNoSpace;
            const auto arenaEnd = guestMappingBase + image.mapping_backing_end();
            const bool linear = address >= guestMappingBase && end <= arenaEnd;
            std::optional<std::int64_t> allocated;
            if (linear) {
                allocated = mappingHeap.allocate_at(
                    static_cast<std::int64_t>(address - guestMappingBase), size,
                    static_cast<std::int64_t>(pageSize));
            } else {
                allocated = mappingHeap.allocate(size, static_cast<std::int64_t>(pageSize));
                if (!allocated && resident[0] + 1 < residentCapacity &&
                    extendVmBacking(size)) {
                    allocated = mappingHeap.allocate(size, static_cast<std::int64_t>(pageSize));
                }
            }
            if (!allocated) return kernNoSpace;
            const auto backing = static_cast<std::size_t>(*allocated);
            if (backing > memoryBuffer.size() || size > memoryBuffer.size() - backing) {
                mappingHeap.release(*allocated);
                return kernNoSpace;
            }
            const VmRegion region{address, end, backing,
                (currentProtection & 1U) != 0, (currentProtection & 2U) != 0,
                (currentProtection & 4U) != 0, maximumProtection, false,
                inheritance, tag};
            std::memset(static_cast<std::uint8_t*>(memoryBuffer.data()) + backing, 0,
                        static_cast<std::size_t>(size));
            try {
                auto stagedAllocations = vmAllocations;
                auto owner = stagedAllocations.find(address);
                if (owner == stagedAllocations.end()) {
                    stagedAllocations.emplace(address, VmAllocation{address, end, backing,
                        {region}, !linear, {{*allocated, size}}});
                } else {
                    owner->second.begin = std::min(owner->second.begin, address);
                    owner->second.end = std::max(owner->second.end, end);
                    owner->second.sparseAddressSpace = owner->second.sparseAddressSpace || !linear;
                    owner->second.regions.push_back(region);
                    std::sort(owner->second.regions.begin(), owner->second.regions.end(),
                        [](const VmRegion& left, const VmRegion& right) {
                            return left.begin < right.begin;
                        });
                    owner->second.ownedBackings.push_back({*allocated, size});
                }
                auto stagedResident = resident;
                appendVmRegionToResident(stagedResident, region);
                auto stagedCodeRegions = prepareDynamicCodeRegions(address, end, {region});
                guestMemory.replace_mappings({}, {{region.begin, region.end, region.backing,
                                                   region.writable, region.readable}});
                vmAllocations.swap(stagedAllocations);
                resident.swap(stagedResident);
                commitDynamicCodeRegions(std::move(stagedCodeRegions));
            } catch (...) {
                mappingHeap.release(*allocated);
                return kernNoSpace;
            }
            syncResidentTable();
            return kernSuccess;
        }
        try {
            const auto [unused, inserted] = vmReservations.emplace(address,
                VmReservation{address, end, currentProtection, maximumProtection,
                              inheritance, tag, 0, jitMapping});
            (void)unused;
            return inserted ? kernSuccess : kernNoSpace;
        } catch (...) {
            return kernNoSpace;
        }
    };
    hostServiceState.machVmAllocate = [&](std::uint64_t size, std::uint64_t alignment,
                                           std::uint32_t currentProtection,
                                           std::uint32_t maximumProtection,
                                           std::uint64_t& guestAddress, bool jitMapping,
                                           std::uint32_t inheritance, std::uint32_t tag) {
        if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 ||
            size > std::numeric_limits<std::uint64_t>::max() - guestMappingBase) {
            return kernInvalidArgument;
        }
        if (currentProtection == 0) {
            auto candidate = std::uint64_t{0x7180000000};
            if (candidate > std::numeric_limits<std::uint64_t>::max() - (alignment - 1)) {
                return kernNoSpace;
            }
            candidate = (candidate + alignment - 1) & ~(alignment - 1);
            for (std::size_t attempt = 0; attempt < 4096; ++attempt) {
                if (candidate > guestCodeLimit || size > guestCodeLimit - candidate) {
                    return kernNoSpace;
                }
                auto nextCandidate = candidate;
                const auto requestedEnd = candidate + size;
                const auto skipPast = [&](std::uint64_t end) {
                    if (end > nextCandidate && end <= guestCodeLimit) {
                        if (end > std::numeric_limits<std::uint64_t>::max() - (alignment - 1)) {
                            nextCandidate = guestCodeLimit;
                        } else {
                            nextCandidate = (end + alignment - 1) & ~(alignment - 1);
                        }
                    }
                };
                for (const auto& [unused, reservation] : vmReservations) {
                    (void)unused;
                    if (candidate < reservation.end && reservation.begin < requestedEnd) {
                        skipPast(reservation.end);
                    }
                }
                for (const auto& [unused, allocation] : vmAllocations) {
                    (void)unused;
                    for (const auto& region : allocation.regions) {
                        if (candidate < region.end && region.begin < requestedEnd) {
                            skipPast(region.end);
                        }
                    }
                }
                for (std::size_t index = 0; index < resident[0]; ++index) {
                    const auto row = 1 + 4 * index;
                    if (candidate < resident[row + 1] && resident[row] < requestedEnd) {
                        skipPast(resident[row + 1]);
                    }
                }
                if (nextCandidate != candidate) {
                    candidate = nextCandidate;
                    continue;
                }
                const auto result = hostServiceState.machVmReserve(
                    candidate, size, currentProtection, maximumProtection, inheritance, tag,
                    jitMapping);
                if (result == kernSuccess) {
                    guestAddress = candidate;
                    return kernSuccess;
                }
                if (result != kernNoSpace || candidate > guestCodeLimit - alignment) {
                    return result;
                }
                candidate += alignment;
            }
            return kernNoSpace;
        }
        std::vector<GuestRange> excludedBackingRanges;
        const auto arenaEnd = guestMappingBase + image.mapping_backing_end();
        const auto excludeLogicalRange = [&](std::uint64_t logicalBegin,
                                             std::uint64_t logicalEnd) {
            const auto begin = std::max(logicalBegin, guestMappingBase);
            const auto end = std::min(logicalEnd, arenaEnd);
            if (begin < end) {
                excludedBackingRanges.push_back({
                    static_cast<std::int64_t>(begin - guestMappingBase),
                    static_cast<std::int64_t>(end - guestMappingBase)});
            }
        };
        for (const auto& [unused, reservation] : vmReservations) {
            (void)unused;
            excludeLogicalRange(reservation.begin, reservation.end);
        }
        for (std::size_t index = 0; index < resident[0]; ++index) {
            const auto row = 1 + 4 * index;
            excludeLogicalRange(resident[row], resident[row + 1]);
        }
        const auto allocated = mappingHeap.allocate_avoiding(
            size, static_cast<std::int64_t>(alignment), excludedBackingRanges);
        if (!allocated || resident[0] >= residentCapacity) {
            if (allocated) mappingHeap.release(*allocated);
            return kernNoSpace;
        }
        const auto backing = static_cast<std::size_t>(*allocated);
        const auto address = guestMappingBase + static_cast<std::uint64_t>(backing);
        const auto linearBackingEnd = image.mapping_backing_end();
        if (backing >= linearBackingEnd || size > linearBackingEnd - backing ||
            address > std::numeric_limits<std::uint64_t>::max() - size ||
            (address & (alignment - 1)) != 0) {
            mappingHeap.release(*allocated);
            return kernNoSpace;
        }
        for (const auto& [unused, reservation] : vmReservations) {
            (void)unused;
            if (address < reservation.end && reservation.begin < address + size) {
                mappingHeap.release(*allocated);
                return kernNoSpace;
            }
        }
        for (std::size_t index = 0; index < resident[0]; ++index) {
            const auto row = 1 + 4 * index;
            if (address < resident[row + 1] && resident[row] < address + size) {
                mappingHeap.release(*allocated);
                return kernNoSpace;
            }
        }
        std::memset(static_cast<std::uint8_t*>(memoryBuffer.data()) + backing, 0,
                    static_cast<std::size_t>(size));
        VmRegion region{address, address + size, backing,
                        (currentProtection & 1U) != 0,
                        (currentProtection & 2U) != 0,
                        (currentProtection & 4U) != 0,
                        maximumProtection, jitMapping, inheritance, tag};
        try {
            auto stagedAllocations = vmAllocations;
            auto owner = stagedAllocations.find(address);
            if (owner == stagedAllocations.end()) {
                stagedAllocations.emplace(address, VmAllocation{address, address + size,
                    backing, {region}, false, {{*allocated, size}}});
            } else {
                owner->second.begin = std::min(owner->second.begin, address);
                owner->second.end = std::max(owner->second.end, address + size);
                owner->second.regions.push_back(region);
                std::sort(owner->second.regions.begin(), owner->second.regions.end(),
                    [](const VmRegion& left, const VmRegion& right) {
                        return left.begin < right.begin;
                    });
                owner->second.ownedBackings.push_back({*allocated, size});
            }
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
    hostServiceState.machVmOverwrite = [&](std::uint64_t& guestAddress,
                                            std::uint64_t size,
                                            std::uint32_t currentProtection,
                                            std::uint32_t maximumProtection,
                                            std::uint32_t inheritance,
                                            std::uint32_t tag) {
        progressTrace.event("vm-overwrite-enter", guestAddress, size);
        const auto pageSize = bootstrap.pageSize;
        if (size == 0 || pageSize == 0 || (pageSize & (pageSize - 1)) != 0 ||
            guestAddress > std::numeric_limits<std::uint64_t>::max() - size) {
            return kernInvalidArgument;
        }
        const auto requestedEnd = guestAddress + size;
        if (requestedEnd > std::numeric_limits<std::uint64_t>::max() - (pageSize - 1)) {
            return kernInvalidArgument;
        }
        const auto begin = guestAddress & ~(pageSize - 1);
        const auto end = (requestedEnd + pageSize - 1) & ~(pageSize - 1);
        if (image.mapping_backing_end() >
            std::numeric_limits<std::uint64_t>::max() - guestMappingBase) {
            return kernInvalidAddress;
        }
        const auto arenaEnd = guestMappingBase + image.mapping_backing_end();
        if (begin < guestMappingBase || end > arenaEnd) {
            if (begin >= guestCodeLimit || end > guestCodeLimit ||
                (currentProtection & ~maximumProtection) != 0 ||
                (currentProtection & 6U) == 6U || (currentProtection & 4U) != 0) {
                return kernInvalidAddress;
            }
            const auto sparseAllocation = std::find_if(vmAllocations.begin(), vmAllocations.end(),
                [begin, end](const auto& item) {
                    return item.second.sparseAddressSpace && begin >= item.second.begin &&
                        end <= item.second.end;
                });
            const bool reservationOverlaps = std::any_of(vmReservations.begin(),
                vmReservations.end(), [begin, end](const auto& item) {
                    return begin < item.second.end && item.second.begin < end;
                });
            const bool otherOwnerOverlaps = sparseAllocation != vmAllocations.end() &&
                std::any_of(vmAllocations.begin(), vmAllocations.end(),
                    [&](const auto& item) {
                        if (item.first == sparseAllocation->first) return false;
                        return std::any_of(item.second.regions.begin(), item.second.regions.end(),
                            [begin, end](const VmRegion& region) {
                                return begin < region.end && region.begin < end;
                            });
                    });
            if (sparseAllocation != vmAllocations.end() && !reservationOverlaps &&
                !otherOwnerOverlaps) {
                for (const auto& region : sparseAllocation->second.regions) {
                    if (region.end <= begin || region.begin >= end) continue;
                    if (region.jit || region.executable) {
                        return kernInvalidAddress;
                    }
                }
                if (pageSize > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) return kernNoSpace;
                const auto allocated = mappingHeap.allocate(
                    static_cast<std::int64_t>(end - begin), static_cast<std::int64_t>(pageSize));
                if (!allocated) return kernNoSpace;
                const auto backing = static_cast<std::size_t>(*allocated);
                if (backing > memoryBuffer.size() || end - begin > memoryBuffer.size() - backing) {
                    mappingHeap.release(*allocated);
                    return kernNoSpace;
                }
                std::vector<VmBackingLease> retiredBackings;
                try {
                    auto replacement = sparseAllocation->second.regions;
                    std::vector<VmRegion> carved;
                    for (const auto& region : replacement) {
                        if (region.end <= begin || region.begin >= end) {
                            carved.push_back(region);
                            continue;
                        }
                        if (region.begin < begin) {
                            carved.push_back({region.begin, begin, region.backing,
                                region.readable, region.writable, region.executable,
                                region.maximumProtection, region.jit,
                                region.inheritance, region.tag});
                        }
                        if (region.end > end) {
                            carved.push_back({end, region.end,
                                region.backing + static_cast<std::size_t>(end - region.begin),
                                region.readable, region.writable, region.executable,
                                region.maximumProtection, region.jit,
                                region.inheritance, region.tag});
                        }
                    }
                    carved.push_back({begin, end, backing,
                        (currentProtection & 1U) != 0, (currentProtection & 2U) != 0,
                        (currentProtection & 4U) != 0, maximumProtection, false,
                        inheritance, tag});
                    std::sort(carved.begin(), carved.end(),
                        [](const VmRegion& left, const VmRegion& right) {
                            return left.begin < right.begin;
                        });
                    if (resident[0] < replacement.size() ||
                        resident[0] - replacement.size() + carved.size() > residentCapacity) {
                        mappingHeap.release(*allocated);
                        return kernNoSpace;
                    }
                    auto stagedAllocations = vmAllocations;
                    auto stagedAllocation = stagedAllocations.find(sparseAllocation->first);
                    stagedAllocation->second.regions = carved;
                    stagedAllocation->second.ownedBackings.push_back(
                        {*allocated, end - begin});
                    auto& leases = stagedAllocation->second.ownedBackings;
                    leases.erase(std::remove_if(leases.begin(), leases.end(),
                        [&](const VmBackingLease& lease) {
                            const auto leaseBegin = static_cast<std::uint64_t>(lease.backing);
                            const auto leaseEnd = leaseBegin + lease.size;
                            const bool referenced = std::any_of(carved.begin(), carved.end(),
                                [&](const VmRegion& region) {
                                    const auto regionEnd = region.backing +
                                        static_cast<std::size_t>(region.end - region.begin);
                                    return region.backing < leaseEnd && leaseBegin < regionEnd;
                                });
                            if (!referenced) retiredBackings.push_back(lease);
                            return !referenced;
                        }), leases.end());
                    auto stagedResident = resident;
                    std::vector<std::uint64_t> removedBegins;
                    std::vector<GuestMemoryMapping> addedMappings;
                    for (const auto& region : replacement) {
                        removedBegins.push_back(region.begin);
                        removeVmRegionFromResident(stagedResident, region);
                    }
                    for (const auto& region : carved) {
                        appendVmRegionToResident(stagedResident, region);
                        addedMappings.push_back({region.begin, region.end, region.backing,
                                                 region.writable, region.readable});
                    }
                    auto stagedCodeRegions = prepareDynamicCodeRegions(
                        sparseAllocation->second.begin, sparseAllocation->second.end, carved);
                    guestMemory.replace_mappings(removedBegins, addedMappings);
                    vmAllocations.swap(stagedAllocations);
                    resident.swap(stagedResident);
                    commitDynamicCodeRegions(std::move(stagedCodeRegions));
                } catch (...) {
                    mappingHeap.release(*allocated);
                    return kernNoSpace;
                }
                std::memset(static_cast<std::uint8_t*>(memoryBuffer.data()) + backing, 0,
                            static_cast<std::size_t>(end - begin));
                syncResidentTable();
                for (const auto& lease : retiredBackings) mappingHeap.release(lease.backing);
                guestAddress = begin;
                return kernSuccess;
            }
            const auto replaceSparseRange = [&]() {
                progressTrace.event("vm-overwrite-sparse-range", begin, end - begin);
                if (pageSize > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) return kernNoSpace;
                // A replacement may need a larger contiguous lease than the free
                // heap currently has (for example, XZM replaces two 2 MiB owners
                // and a reserved 2 MiB hole with one 6 MiB mapping). Retire only
                // leases whose every mapped byte is covered by this replacement.
                std::vector<VmBackingLease> reclaimedBackings;
                for (const auto& [unused, allocation] : vmAllocations) {
                    (void)unused;
                    if (!allocation.sparseAddressSpace) continue;
                    const auto collectReclaimable = [&](const VmBackingLease& lease) {
                        const auto leaseBegin = static_cast<std::uint64_t>(lease.backing);
                        const auto leaseEnd = leaseBegin + lease.size;
                        bool referenced = false;
                        bool survives = false;
                        for (const auto& [otherUnused, other] : vmAllocations) {
                            (void)otherUnused;
                            if (!other.sparseAddressSpace) continue;
                            for (const auto& region : other.regions) {
                                const auto regionEnd = region.backing +
                                    static_cast<std::size_t>(region.end - region.begin);
                                if (region.backing >= leaseEnd || leaseBegin >= regionEnd) continue;
                                referenced = true;
                                if (region.begin < begin || region.end > end) survives = true;
                            }
                        }
                        if (referenced && !survives && std::none_of(
                                reclaimedBackings.begin(), reclaimedBackings.end(),
                                [&](const VmBackingLease& prior) {
                                    return prior.backing == lease.backing;
                                })) {
                            reclaimedBackings.push_back(lease);
                        }
                    };
                    if (allocation.ownedBackings.empty()) {
                        collectReclaimable({static_cast<std::int64_t>(allocation.backing),
                                            allocation.end - allocation.begin});
                    } else {
                        for (const auto& lease : allocation.ownedBackings) {
                            collectReclaimable(lease);
                        }
                    }
                }
                progressTrace.event("vm-overwrite-reclaim-count", begin,
                                    reclaimedBackings.size());
                for (const auto& lease : reclaimedBackings) mappingHeap.release(lease.backing);
                struct ReclaimedBackingRollback {
                    GuestHeap& heap;
                    const std::vector<VmBackingLease>& leases;
                    std::int64_t alignment;
                    bool committed = false;
                    ~ReclaimedBackingRollback() {
                        if (committed) return;
                        for (const auto& lease : leases) {
                            try {
                                (void)heap.allocate_at(lease.backing, lease.size, alignment);
                            } catch (...) {
                                std::terminate();
                            }
                        }
                    }
                } reclaimedRollback{mappingHeap, reclaimedBackings,
                                    static_cast<std::int64_t>(pageSize)};
                auto allocated = mappingHeap.allocate(
                    static_cast<std::int64_t>(end - begin), static_cast<std::int64_t>(pageSize));
                if (!allocated && extendVmBacking(end - begin)) {
                    progressTrace.event("vm-overwrite-grow-backing", begin, end - begin);
                    allocated = mappingHeap.allocate(
                        static_cast<std::int64_t>(end - begin),
                        static_cast<std::int64_t>(pageSize));
                }
                if (!allocated) {
                    progressTrace.event("vm-overwrite-allocation-failed", begin, end - begin);
                    return kernNoSpace;
                }
                const auto backing = static_cast<std::size_t>(*allocated);
                if (backing > memoryBuffer.size() || end - begin > memoryBuffer.size() - backing) {
                    mappingHeap.release(*allocated);
                    return kernNoSpace;
                }
                const VmRegion newRegion{begin, end, backing,
                    (currentProtection & 1U) != 0, (currentProtection & 2U) != 0,
                    (currentProtection & 4U) != 0, maximumProtection, false,
                    inheritance, tag};
                try {
                    auto stagedReservations = vmReservations;
                    for (const auto& [reservationBegin, reservation] : vmReservations) {
                        if (reservation.end <= begin || reservation.begin >= end) continue;
                        stagedReservations.erase(reservationBegin);
                        const auto middleBegin = std::max(begin, reservation.begin);
                        const auto middleEnd = std::min(end, reservation.end);
                        if (reservation.begin < middleBegin) {
                            auto left = reservation;
                            left.end = middleBegin;
                            stagedReservations.emplace(left.begin, left);
                        }
                        if (reservation.end > middleEnd) {
                            auto right = reservation;
                            right.begin = middleEnd;
                            right.offset += middleEnd - reservation.begin;
                            stagedReservations.emplace(right.begin, right);
                        }
                    }
                    auto stagedAllocations = vmAllocations;
                    auto stagedResident = resident;
                    std::vector<std::uint64_t> removedBegins;
                    std::vector<GuestMemoryMapping> addedMappings;
                    std::vector<VmRegion> codeRegions;
                    std::vector<VmBackingLease> retiredBackings;
                    bool overlapsManagedMapping = false;
                    for (auto allocation = vmAllocations.begin();
                         allocation != vmAllocations.end(); ++allocation) {
                        const auto& original = allocation->second;
                        if (!original.sparseAddressSpace) continue;
                        const bool overlaps = std::any_of(original.regions.begin(),
                            original.regions.end(), [begin, end](const VmRegion& region) {
                                return begin < region.end && region.begin < end;
                            });
                        if (!overlaps) continue;
                        overlapsManagedMapping = true;
                        std::vector<VmRegion> replacement;
                        for (const auto& region : original.regions) {
                            if (region.end <= begin || region.begin >= end) {
                                replacement.push_back(region);
                                continue;
                            }
                            if (region.jit || region.executable) {
                                mappingHeap.release(*allocated);
                                return kernInvalidAddress;
                            }
                            if (region.begin < begin) {
                                replacement.push_back({region.begin, begin, region.backing,
                                    region.readable, region.writable, region.executable,
                                    region.maximumProtection, region.jit,
                                    region.inheritance, region.tag});
                            }
                            if (region.end > end) {
                                replacement.push_back({end, region.end,
                                    region.backing + static_cast<std::size_t>(end - region.begin),
                                    region.readable, region.writable, region.executable,
                                    region.maximumProtection, region.jit,
                                    region.inheritance, region.tag});
                            }
                        }
                        for (const auto& region : original.regions) {
                            removedBegins.push_back(region.begin);
                            removeVmRegionFromResident(stagedResident, region);
                        }
                        for (const auto& region : replacement) {
                            appendVmRegionToResident(stagedResident, region);
                            addedMappings.push_back({region.begin, region.end, region.backing,
                                                     region.writable, region.readable});
                            codeRegions.push_back(region);
                        }
                        if (replacement.empty()) {
                            if (original.ownedBackings.empty()) {
                                retiredBackings.push_back({
                                    static_cast<std::int64_t>(original.backing),
                                    original.end - original.begin});
                            } else {
                                retiredBackings.insert(retiredBackings.end(),
                                    original.ownedBackings.begin(), original.ownedBackings.end());
                            }
                            stagedAllocations.erase(allocation->first);
                        } else {
                            auto& staged = stagedAllocations.at(allocation->first);
                            staged.regions = replacement;
                            auto& leases = staged.ownedBackings;
                            leases.erase(std::remove_if(leases.begin(), leases.end(),
                                [&](const VmBackingLease& lease) {
                                    const auto leaseBegin = static_cast<std::uint64_t>(lease.backing);
                                    const auto leaseEnd = leaseBegin + lease.size;
                                    const bool referenced = std::any_of(replacement.begin(),
                                        replacement.end(), [&](const VmRegion& region) {
                                            const auto regionEnd = region.backing +
                                                static_cast<std::size_t>(region.end - region.begin);
                                            return region.backing < leaseEnd && leaseBegin < regionEnd;
                                        });
                                    if (!referenced) retiredBackings.push_back(lease);
                                    return !referenced;
                                }), leases.end());
                        }
                    }
                    for (std::size_t index = 0; index < resident[0]; ++index) {
                        const auto row = 1 + 4 * index;
                        if (begin >= resident[row + 1] || resident[row] >= end) continue;
                        const bool owned = std::any_of(vmAllocations.begin(), vmAllocations.end(),
                            [&](const auto& allocation) {
                                if (!allocation.second.sparseAddressSpace) return false;
                                return std::any_of(allocation.second.regions.begin(),
                                    allocation.second.regions.end(), [&](const VmRegion& region) {
                                        return region.begin == resident[row] &&
                                            region.end == resident[row + 1] &&
                                            region.backing == resident[row + 2];
                                    });
                            });
                        if (!owned) {
                            mappingHeap.release(*allocated);
                            return kernInvalidAddress;
                        }
                    }
                    if (stagedResident[0] >= residentCapacity) {
                        progressTrace.event("vm-overwrite-resident-full", begin, stagedResident[0]);
                        mappingHeap.release(*allocated);
                        return kernNoSpace;
                    }
                    appendVmRegionToResident(stagedResident, newRegion);
                    addedMappings.push_back({begin, end, backing,
                                             newRegion.writable, newRegion.readable});
                    codeRegions.push_back(newRegion);
                    auto sameStart = stagedAllocations.find(begin);
                    if (sameStart != stagedAllocations.end()) {
                        auto& owner = sameStart->second;
                        owner.begin = std::min(owner.begin, begin);
                        owner.end = std::max(owner.end, end);
                        owner.sparseAddressSpace = true;
                        owner.regions.push_back(newRegion);
                        std::sort(owner.regions.begin(), owner.regions.end(),
                            [](const VmRegion& left, const VmRegion& right) {
                                return left.begin < right.begin;
                            });
                        owner.ownedBackings.push_back({*allocated, end - begin});
                    } else {
                        stagedAllocations.emplace(begin, VmAllocation{begin, end, backing,
                            {newRegion}, true, {{*allocated, end - begin}}});
                    }
                    const auto dynamicBegin = std::min(begin, [&]() {
                        auto result = begin;
                        for (const auto& [unused, owner] : vmAllocations) {
                            (void)unused;
                            if (owner.sparseAddressSpace && owner.begin < end && begin < owner.end) {
                                result = std::min(result, owner.begin);
                            }
                        }
                        return result;
                    }());
                    const auto dynamicEnd = std::max(end, [&]() {
                        auto result = end;
                        for (const auto& [unused, owner] : vmAllocations) {
                            (void)unused;
                            if (owner.sparseAddressSpace && owner.begin < end && begin < owner.end) {
                                result = std::max(result, owner.end);
                            }
                        }
                        return result;
                    }());
                    auto stagedCodeRegions = prepareDynamicCodeRegions(
                        dynamicBegin, dynamicEnd, codeRegions);
                    guestMemory.replace_mappings(removedBegins, addedMappings);
                    vmAllocations.swap(stagedAllocations);
                    vmReservations.swap(stagedReservations);
                    resident.swap(stagedResident);
                    commitDynamicCodeRegions(std::move(stagedCodeRegions));
                    std::memset(static_cast<std::uint8_t*>(memoryBuffer.data()) + backing, 0,
                                static_cast<std::size_t>(end - begin));
                    syncResidentTable();
                    reclaimedRollback.committed = true;
                    retiredBackings.erase(std::remove_if(retiredBackings.begin(),
                        retiredBackings.end(), [&](const VmBackingLease& lease) {
                            return std::any_of(reclaimedBackings.begin(),
                                reclaimedBackings.end(), [&](const VmBackingLease& reclaimed) {
                                    return lease.backing == reclaimed.backing;
                                });
                        }), retiredBackings.end());
                    for (const auto& lease : retiredBackings) mappingHeap.release(lease.backing);
                    (void)overlapsManagedMapping;
                    guestAddress = begin;
                    return kernSuccess;
                } catch (...) {
                    progressTrace.event("vm-overwrite-stage-exception", begin, end - begin);
                    mappingHeap.release(*allocated);
                    return kernNoSpace;
                }
            };
            return replaceSparseRange();
        }
        const auto allocation = std::find_if(vmAllocations.begin(), vmAllocations.end(),
            [begin, end](const auto& item) {
                return begin >= item.second.begin && end <= item.second.end;
            });
        const bool newAllocation = allocation == vmAllocations.end();
        VmAllocation current = newAllocation
            ? VmAllocation{begin, end, 0, {}, false, {}}
            : allocation->second;
        if (newAllocation) {
            for (const auto& [unused, existing] : vmAllocations) {
                (void)unused;
                if (begin < existing.end && existing.begin < end) return kernInvalidAddress;
            }
            for (std::size_t index = 0; index < resident[0]; ++index) {
                const auto row = 1 + 4 * index;
                if (begin < resident[row + 1] && resident[row] < end) {
                    return kernInvalidAddress;
                }
            }
            const auto offset = begin - guestMappingBase;
            if (offset > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) return kernNoSpace;
            current.backing = static_cast<std::size_t>(offset);
        } else if (std::any_of(current.regions.begin(), current.regions.end(),
                       [](const VmRegion& region) { return region.jit; })) {
            return kernInvalidAddress;
        }
        for (const auto& region : current.regions) {
            if (region.end <= begin || region.begin >= end) continue;
            if (region.jit || region.executable) return kernInvalidAddress;
        }
        if ((currentProtection & 4U) != 0) return kernProtectionFailure;

        std::vector<VmRegion> replacement;
        replacement.reserve(current.regions.size() + 2);
        for (const auto& region : current.regions) {
            if (region.end <= begin || region.begin >= end) {
                replacement.push_back(region);
                continue;
            }
            if (region.begin < begin) {
                replacement.push_back({region.begin, begin, region.backing,
                    region.readable, region.writable, region.executable,
                    region.maximumProtection, region.jit,
                    region.inheritance, region.tag});
            }
            if (region.end > end) {
                replacement.push_back({end, region.end,
                    region.backing + static_cast<std::size_t>(end - region.begin),
                    region.readable, region.writable, region.executable,
                    region.maximumProtection, region.jit,
                    region.inheritance, region.tag});
            }
        }
        const auto backing = current.backing + static_cast<std::size_t>(begin - current.begin);
        replacement.push_back({begin, end, backing,
            (currentProtection & 1U) != 0, (currentProtection & 2U) != 0,
            (currentProtection & 4U) != 0, maximumProtection, false,
            inheritance, tag});
        std::sort(replacement.begin(), replacement.end(),
            [](const VmRegion& left, const VmRegion& right) {
                return left.begin < right.begin;
            });
        if (resident[0] < current.regions.size() ||
            resident[0] - current.regions.size() + replacement.size() > residentCapacity) {
            return kernNoSpace;
        }

        std::optional<std::int64_t> reservedBacking;
        if (newAllocation) {
            if (pageSize > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) return kernNoSpace;
            reservedBacking = mappingHeap.allocate_at(
                static_cast<std::int64_t>(current.backing), end - begin,
                static_cast<std::int64_t>(pageSize));
            if (!reservedBacking) return kernNoSpace;
            if (static_cast<std::size_t>(*reservedBacking) > memoryBuffer.size() ||
                end - begin > memoryBuffer.size() -
                    static_cast<std::size_t>(*reservedBacking)) {
                mappingHeap.release(*reservedBacking);
                return kernNoSpace;
            }
        }
        try {
            auto stagedAllocations = vmAllocations;
            if (newAllocation) {
                auto [unused, inserted] = stagedAllocations.emplace(
                    begin, VmAllocation{begin, end, backing, replacement, false,
                        {{*reservedBacking, end - begin}}});
                (void)unused;
                if (!inserted) throw std::runtime_error("Duplicate guest VM allocation address");
            } else {
                auto stagedAllocation = stagedAllocations.find(current.begin);
                if (stagedAllocation == stagedAllocations.end()) {
                    throw std::runtime_error("Missing guest VM allocation during overwrite");
                }
                stagedAllocation->second.regions = replacement;
            }
            auto stagedResident = resident;
            std::vector<std::uint64_t> removedBegins;
            std::vector<GuestMemoryMapping> addedMappings;
            removedBegins.reserve(current.regions.size());
            addedMappings.reserve(replacement.size());
            for (const auto& region : current.regions) {
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
            vmAllocations.swap(stagedAllocations);
            resident.swap(stagedResident);
            commitDynamicCodeRegions(std::move(stagedCodeRegions));
        } catch (...) {
            if (reservedBacking) mappingHeap.release(*reservedBacking);
            return kernNoSpace;
        }
        std::memset(static_cast<std::uint8_t*>(memoryBuffer.data()) + backing, 0,
                    static_cast<std::size_t>(end - begin));
        syncResidentTable();
        guestAddress = begin;
        return kernSuccess;
    };
    hostServiceState.currentThreadStackFloor = [&]() -> std::optional<std::uint64_t> {
        const auto found = threadStackGuards.find(guestScheduler.current_id());
        if (found == threadStackGuards.end()) return std::nullopt;
        return found->second.end;
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
        const bool linearRange = begin >= guestMappingBase && end <= arenaEnd;
        if (!linearRange) {
            bool managedSparseRange = false;
            auto reservation = vmReservations.upper_bound(begin);
            if (reservation != vmReservations.begin()) {
                --reservation;
                managedSparseRange = reservation->second.begin < end && begin < reservation->second.end;
            }
            for (const auto& [unused, allocation] : vmAllocations) {
                (void)unused;
                if (!allocation.sparseAddressSpace) continue;
                if (begin < allocation.end && allocation.begin < end) managedSparseRange = true;
            }
            if (!managedSparseRange) return kernInvalidArgument;
        }
        std::map<std::uint64_t, VmAllocation> stagedAllocations;
        std::map<std::uint64_t, VmReservation> stagedReservations;
        std::vector<std::uint64_t> stagedResident;
        std::vector<std::uint64_t> removedBegins;
        std::vector<GuestMemoryMapping> addedMappings;
        std::vector<std::int64_t> releasedBackings;
        std::vector<VmRegion> stagedCodeRegionsInput;
        auto dynamicBegin = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t dynamicEnd = 0;
        try {
            stagedAllocations = vmAllocations;
            stagedReservations = vmReservations;
            stagedResident = resident;
            for (const auto& [reservationBegin, reservation] : vmReservations) {
                if (reservation.end <= begin || reservation.begin >= end) continue;
                stagedReservations.erase(reservationBegin);
                if (reservation.begin < begin) {
                    auto left = reservation;
                    left.end = begin;
                    stagedReservations.emplace(left.begin, left);
                }
                if (reservation.end > end) {
                    auto right = reservation;
                    right.begin = end;
                    right.offset += end - reservation.begin;
                    stagedReservations.emplace(right.begin, right);
                }
            }
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
                            region.maximumProtection, region.jit,
                            region.inheritance, region.tag});
                    }
                    if (region.end > mappedEnd) {
                        replacement.push_back({mappedEnd, region.end,
                            region.backing + static_cast<std::size_t>(mappedEnd - region.begin),
                            region.readable, region.writable, region.executable,
                            region.maximumProtection, region.jit,
                            region.inheritance, region.tag});
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
                    if (allocation.ownedBackings.empty()) {
                        releasedBackings.push_back(
                            static_cast<std::int64_t>(allocation.backing));
                    } else {
                        for (const auto& lease : allocation.ownedBackings) {
                            releasedBackings.push_back(lease.backing);
                        }
                    }
                    stagedAllocations.erase(stagedAllocation);
                } else {
                    stagedAllocation->second.regions = replacement;
                    auto& leases = stagedAllocation->second.ownedBackings;
                    leases.erase(std::remove_if(leases.begin(), leases.end(),
                        [&](const VmBackingLease& lease) {
                            const auto leaseBegin = static_cast<std::uint64_t>(lease.backing);
                            const auto leaseEnd = leaseBegin + lease.size;
                            const bool referenced = std::any_of(
                                stagedAllocation->second.regions.begin(),
                                stagedAllocation->second.regions.end(),
                                [&](const VmRegion& region) {
                                    const auto regionEnd = region.backing +
                                        static_cast<std::size_t>(region.end - region.begin);
                                    return region.backing < leaseEnd && leaseBegin < regionEnd;
                                });
                            if (!referenced) releasedBackings.push_back(lease.backing);
                            return !referenced;
                        }), leases.end());
                }
            }
            if (removedBegins.empty()) {
                vmReservations.swap(stagedReservations);
                return kernSuccess;
            }
            DynamicCodeReplacement stagedCodeRegions = prepareDynamicCodeRegions(
                dynamicBegin, dynamicEnd, stagedCodeRegionsInput);
            guestMemory.replace_mappings(removedBegins, addedMappings);
            vmAllocations.swap(stagedAllocations);
            vmReservations.swap(stagedReservations);
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
                                          bool setMaximum, std::uint32_t protection,
                                          bool mprotect) {
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
        const auto stackGuard = std::find_if(threadStackGuards.begin(),
            threadStackGuards.end(), [begin, end](const auto& item) {
                return begin >= item.second.begin && end <= item.second.end;
            });
        if (stackGuard != threadStackGuards.end()) {
            if (setMaximum || (protection & ~3U) != 0) return kernProtectionFailure;
            for (auto page = begin; page < end; page += pageSize) {
                if (protection == 3U) {
                    protectedGuestPages.erase(page);
                    guestMemory.remove_mapping(page);
                } else {
                    const bool wasProtected = protectedGuestPages.find(page) !=
                        protectedGuestPages.end();
                    protectedGuestPages[page] = protection;
                    if (wasProtected) {
                        guestMemory.set_mapping_permissions(page,
                            (protection & 1U) != 0, (protection & 2U) != 0);
                    } else {
                        guestMemory.add_mapping({page, page + pageSize, page,
                            (protection & 2U) != 0, (protection & 1U) != 0});
                    }
                }
            }
            syncWritableTable(bootstrap.errnoSlot);
            return kernSuccess;
        }
        auto allocation = std::find_if(vmAllocations.begin(), vmAllocations.end(),
            [begin, end](const auto& item) {
                if (begin < item.second.begin || end > item.second.end) return false;
                auto cursor = begin;
                for (const auto& region : item.second.regions) {
                    if (region.end <= cursor || region.begin >= end) continue;
                    if (region.begin > cursor) return false;
                    cursor = std::min(end, region.end);
                    if (cursor == end) return true;
                }
                return false;
            });
        if (allocation == vmAllocations.end()) {
            auto cursor = begin;
            std::vector<std::uint64_t> ownerKeys;
            std::vector<VmReservation> touchedReservations;
            std::vector<std::tuple<std::uint64_t, std::uint64_t, VmReservation>> committedRanges;
            while (cursor < end) {
                const auto owner = std::find_if(vmAllocations.begin(), vmAllocations.end(),
                    [cursor](const auto& item) {
                        return std::any_of(item.second.regions.begin(), item.second.regions.end(),
                            [cursor](const VmRegion& region) {
                                return region.begin <= cursor && cursor < region.end;
                            });
                    });
                if (owner != vmAllocations.end()) {
                    const auto region = std::find_if(owner->second.regions.begin(),
                        owner->second.regions.end(), [cursor](const VmRegion& candidate) {
                            return candidate.begin <= cursor && cursor < candidate.end;
                        });
                    if ((protection & ~region->maximumProtection) != 0) {
                        return kernProtectionFailure;
                    }
                    const auto effective = (region->readable ? 1U : 0U) |
                        (region->writable ? 2U : 0U) | (region->executable ? 4U : 0U);
                    const bool jitProtectionRequest = mprotect && region->jit && !setMaximum &&
                        (protection == 0 || protection == region->maximumProtection);
                    if ((mprotect && (protection & 6U) == 6U && !region->jit) ||
                        (region->jit && (setMaximum ||
                            (protection != effective && !jitProtectionRequest)))) {
                        return kernProtectionFailure;
                    }
                    if (std::find(ownerKeys.begin(), ownerKeys.end(), owner->first) ==
                        ownerKeys.end()) {
                        ownerKeys.push_back(owner->first);
                    }
                    cursor = std::min(end, region->end);
                    continue;
                }
                auto reservation = vmReservations.upper_bound(cursor);
                if (reservation == vmReservations.begin()) return kernInvalidAddress;
                --reservation;
                if (reservation->second.begin > cursor || reservation->second.end <= cursor) {
                    return kernInvalidAddress;
                }
                if ((protection & ~reservation->second.maximumProtection) != 0) {
                    return kernProtectionFailure;
                }
                const bool jitProtectionRequest = mprotect && reservation->second.jit &&
                    !setMaximum && protection == reservation->second.maximumProtection;
                if ((reservation->second.jit &&
                     (setMaximum || (protection != 0 && !jitProtectionRequest))) ||
                    ((protection & 6U) == 6U && !jitProtectionRequest)) {
                    return kernProtectionFailure;
                }
                touchedReservations.push_back(reservation->second);
                const auto next = std::min(end, reservation->second.end);
                if (!setMaximum && protection != 0) {
                    committedRanges.emplace_back(cursor, next, reservation->second);
                }
                cursor = next;
            }
            std::vector<std::pair<VmRegion, VmBackingLease>> committedRegions;
            try {
                committedRegions.reserve(committedRanges.size());
                for (const auto& [rangeBegin, rangeEnd, reservation] : committedRanges) {
                    const auto length = rangeEnd - rangeBegin;
                    if (bootstrap.pageSize > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max())) {
                        throw std::bad_alloc();
                    }
                    const auto backing = mappingHeap.allocate(
                        static_cast<std::int64_t>(length),
                        static_cast<std::int64_t>(bootstrap.pageSize));
                    if (!backing) throw std::bad_alloc();
                    std::memset(static_cast<std::uint8_t*>(memoryBuffer.data()) +
                                    static_cast<std::size_t>(*backing), 0,
                                static_cast<std::size_t>(length));
                    auto current = protection;
                    if (reservation.jit && protection == reservation.maximumProtection) {
                        current = guestScheduler.current().jitWriteProtect ? 5U : 3U;
                    }
                    const VmRegion region{rangeBegin, rangeEnd,
                        static_cast<std::size_t>(*backing), (current & 1U) != 0,
                        (current & 2U) != 0, (current & 4U) != 0,
                        reservation.maximumProtection, reservation.jit,
                        reservation.inheritance, reservation.tag};
                    committedRegions.push_back({region, {*backing, length}});
                }
            } catch (...) {
                for (const auto& [unused, lease] : committedRegions) {
                    (void)unused;
                    mappingHeap.release(lease.backing);
                }
                return kernNoSpace;
            }
            try {
                auto stagedAllocations = vmAllocations;
                auto stagedReservations = vmReservations;
                auto stagedResident = resident;
                std::vector<std::uint64_t> removedBegins;
                std::vector<GuestMemoryMapping> addedMappings;
                std::vector<VmRegion> stagedCodeInputs;
                auto dynamicBegin = std::numeric_limits<std::uint64_t>::max();
                std::uint64_t dynamicEnd = 0;
                for (const auto owner : ownerKeys) {
                    const auto& original = vmAllocations.at(owner);
                    dynamicBegin = std::min(dynamicBegin, original.begin);
                    dynamicEnd = std::max(dynamicEnd, original.end);
                    std::vector<VmRegion> replacement;
                    for (const auto& region : original.regions) {
                        if (region.end <= begin || region.begin >= end) {
                            replacement.push_back(region);
                            continue;
                        }
                        const auto middleBegin = std::max(region.begin, begin);
                        const auto middleEnd = std::min(region.end, end);
                        if (region.begin < middleBegin) {
                            replacement.push_back({region.begin, middleBegin, region.backing,
                                region.readable, region.writable, region.executable,
                                region.maximumProtection, region.jit,
                                region.inheritance, region.tag});
                        }
                        const auto maximum = setMaximum ? protection : region.maximumProtection;
                        const auto effective = (region.readable ? 1U : 0U) |
                            (region.writable ? 2U : 0U) | (region.executable ? 4U : 0U);
                        auto current = setMaximum ? (effective & protection) : protection;
                        if (mprotect && !setMaximum && region.jit &&
                            protection == region.maximumProtection) {
                            current = guestScheduler.current().jitWriteProtect ? 5U : 3U;
                        }
                        replacement.push_back({middleBegin, middleEnd,
                            region.backing + static_cast<std::size_t>(middleBegin - region.begin),
                            (current & 1U) != 0, (current & 2U) != 0,
                            (current & 4U) != 0, maximum, region.jit,
                            region.inheritance, region.tag});
                        if (middleEnd < region.end) {
                            replacement.push_back({middleEnd, region.end,
                                region.backing + static_cast<std::size_t>(middleEnd - region.begin),
                                region.readable, region.writable, region.executable,
                                region.maximumProtection, region.jit,
                                region.inheritance, region.tag});
                        }
                    }
                    for (const auto& region : original.regions) {
                        removedBegins.push_back(region.begin);
                        removeVmRegionFromResident(stagedResident, region);
                    }
                    for (const auto& region : replacement) {
                        appendVmRegionToResident(stagedResident, region);
                        addedMappings.push_back({region.begin, region.end, region.backing,
                                                 region.writable, region.readable});
                        stagedCodeInputs.push_back(region);
                    }
                    stagedAllocations.at(owner).regions = std::move(replacement);
                }
                for (const auto& old : touchedReservations) {
                    stagedReservations.erase(old.begin);
                    const auto middleBegin = std::max(begin, old.begin);
                    const auto middleEnd = std::min(end, old.end);
                    if (old.begin < middleBegin) {
                        auto left = old;
                        left.end = middleBegin;
                        stagedReservations.emplace(left.begin, left);
                    }
                    auto middle = old;
                    middle.begin = middleBegin;
                    middle.end = middleEnd;
                    middle.offset += middleBegin - old.begin;
                    if (setMaximum) middle.maximumProtection = protection;
                    middle.currentProtection = 0;
                    stagedReservations.emplace(middle.begin, middle);
                    if (middleEnd < old.end) {
                        auto right = old;
                        right.begin = middleEnd;
                        right.offset += middleEnd - old.begin;
                        stagedReservations.emplace(right.begin, right);
                    }
                }
                for (const auto& [region, lease] : committedRegions) {
                    auto owner = stagedAllocations.find(region.begin);
                    if (owner == stagedAllocations.end()) {
                        stagedAllocations.emplace(region.begin, VmAllocation{region.begin,
                            region.end, region.backing, {region}, true, {lease}});
                    } else {
                        owner->second.begin = std::min(owner->second.begin, region.begin);
                        owner->second.end = std::max(owner->second.end, region.end);
                        owner->second.sparseAddressSpace = true;
                        owner->second.regions.push_back(region);
                        std::sort(owner->second.regions.begin(), owner->second.regions.end(),
                            [](const VmRegion& left, const VmRegion& right) {
                                return left.begin < right.begin;
                            });
                        owner->second.ownedBackings.push_back(lease);
                    }
                    appendVmRegionToResident(stagedResident, region);
                    addedMappings.push_back({region.begin, region.end, region.backing,
                                             region.writable, region.readable});
                    stagedCodeInputs.push_back(region);
                    dynamicBegin = std::min(dynamicBegin, region.begin);
                    dynamicEnd = std::max(dynamicEnd, region.end);
                }
                if (removedBegins.empty() && addedMappings.empty()) {
                    vmReservations.swap(stagedReservations);
                    return kernSuccess;
                }
                auto stagedCodeRegions = prepareDynamicCodeRegions(
                    dynamicBegin, dynamicEnd, stagedCodeInputs);
                guestMemory.replace_mappings(removedBegins, addedMappings);
                vmAllocations.swap(stagedAllocations);
                vmReservations.swap(stagedReservations);
                resident.swap(stagedResident);
                commitDynamicCodeRegions(std::move(stagedCodeRegions));
                syncResidentTable();
                return kernSuccess;
            } catch (...) {
                for (const auto& [unused, lease] : committedRegions) {
                    (void)unused;
                    mappingHeap.release(lease.backing);
                }
                return kernNoSpace;
            }
        }
        auto& current = allocation->second;
        std::uint64_t cursor = begin;
        for (const auto& region : current.regions) {
            if (region.end <= cursor || region.begin >= end) continue;
            if (region.begin > cursor) return kernInvalidAddress;
            if ((protection & ~region.maximumProtection) != 0) return kernProtectionFailure;
            const auto effective = (region.readable ? 1U : 0U) |
                (region.writable ? 2U : 0U) | (region.executable ? 4U : 0U);
            const bool jitProtectionRequest = mprotect && region.jit && !setMaximum &&
                (protection == 0 || protection == region.maximumProtection);
            if ((mprotect && (protection & 6U) == 6U && !region.jit) ||
                (region.jit && (setMaximum ||
                    (protection != effective && !jitProtectionRequest)))) {
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
                            region.maximumProtection, region.jit,
                            region.inheritance, region.tag});
            }
            const auto maximumProtection = setMaximum ? protection : region.maximumProtection;
            auto currentProtection = setMaximum
                ? ((region.readable ? 1U : 0U) | (region.writable ? 2U : 0U) |
                   (region.executable ? 4U : 0U)) & protection
                : protection;
            if (mprotect && !setMaximum && region.jit &&
                protection == region.maximumProtection) {
                currentProtection = guestScheduler.current().jitWriteProtect ? 5U : 3U;
            }
            replacement.push_back({middleBegin, middleEnd,
                region.backing + static_cast<std::size_t>(middleBegin - region.begin),
                (currentProtection & 1U) != 0, (currentProtection & 2U) != 0,
                (currentProtection & 4U) != 0, maximumProtection, region.jit,
                region.inheritance, region.tag});
            if (middleEnd < region.end) {
                replacement.push_back({middleEnd, region.end,
                    region.backing + static_cast<std::size_t>(middleEnd - region.begin),
                    region.readable, region.writable, region.executable,
                    region.maximumProtection, region.jit,
                    region.inheritance, region.tag});
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
    hostServiceState.machVmRegion = [&](std::uint64_t address)
        -> std::optional<HostMachVmRegion> {
        std::map<std::pair<std::uint64_t, std::uint64_t>, HostMachVmRegion> intervals;
        for (std::size_t index = 0; index < resident[0]; ++index) {
            const auto row = 1 + 4 * index;
            const auto begin = resident[row];
            const auto end = resident[row + 1];
            const auto flags = resident[row + 3];
            if (begin >= end) continue;
            HostMachVmRegion region;
            region.begin = begin;
            region.end = end;
            region.protection = ((flags & 2U) == 0 ? 1U : 0U) |
                ((flags & 1U) != 0 ? 2U : 0U);
            region.maximumProtection = region.protection;
            region.inheritance = 1;
            intervals[{begin, end}] = region;
        }
        for (const auto& [unused, allocation] : vmAllocations) {
            (void)unused;
            for (const auto& mapped : allocation.regions) {
                if (mapped.begin >= mapped.end) continue;
                HostMachVmRegion region;
                region.begin = mapped.begin;
                region.end = mapped.end;
                region.protection = (mapped.readable ? 1U : 0U) |
                    (mapped.writable ? 2U : 0U) | (mapped.executable ? 4U : 0U);
                region.maximumProtection = mapped.maximumProtection;
                region.inheritance = mapped.inheritance;
                region.tag = mapped.tag;
                for (const auto& lease : allocation.ownedBackings) {
                    if (mapped.backing >= static_cast<std::size_t>(lease.backing) &&
                        mapped.backing - static_cast<std::size_t>(lease.backing) < lease.size) {
                        region.offset = mapped.backing -
                            static_cast<std::size_t>(lease.backing);
                        break;
                    }
                }
                intervals[{region.begin, region.end}] = region;
            }
        }
        for (const auto& [unused, reservation] : vmReservations) {
            (void)unused;
            if (reservation.begin >= reservation.end) continue;
            HostMachVmRegion region;
            region.begin = reservation.begin;
            region.end = reservation.end;
            region.protection = reservation.currentProtection;
            region.maximumProtection = reservation.maximumProtection;
            region.inheritance = reservation.inheritance;
            region.offset = reservation.offset;
            region.tag = reservation.tag;
            intervals[{region.begin, region.end}] = region;
        }
        std::vector<HostMachVmRegion> ordered;
        ordered.reserve(intervals.size());
        for (const auto& [unused, region] : intervals) {
            (void)unused;
            ordered.push_back(region);
        }
        const auto found = std::find_if(ordered.begin(), ordered.end(),
            [address](const HostMachVmRegion& region) { return region.end > address; });
        if (found == ordered.end()) return std::nullopt;
        auto result = *found;
        const auto sameRegionInfo = [](const HostMachVmRegion& left,
                                       const HostMachVmRegion& right) {
            return left.protection == right.protection &&
                left.maximumProtection == right.maximumProtection &&
                left.inheritance == right.inheritance && left.tag == right.tag &&
                left.offset + (left.end - left.begin) == right.offset;
        };
        const auto foundIndex = static_cast<std::size_t>(found - ordered.begin());
        auto firstIndex = foundIndex;
        while (firstIndex > 0 && ordered[firstIndex - 1].end == result.begin &&
               sameRegionInfo(ordered[firstIndex - 1], result)) {
            result.begin = ordered[firstIndex - 1].begin;
            result.offset = ordered[firstIndex - 1].offset;
            --firstIndex;
        }
        auto lastIndex = foundIndex;
        while (lastIndex + 1 < ordered.size() && result.end == ordered[lastIndex + 1].begin &&
               sameRegionInfo(result, ordered[lastIndex + 1])) {
            result.end = ordered[lastIndex + 1].end;
            ++lastIndex;
        }
        return result;
    };
    bool activeJitWriteProtection = true;
    applyJitWriteProtection = [&](bool enabled) {
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
                    if (!region.jit ||
                        (!region.readable && !region.writable && !region.executable)) continue;
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
                    0, std::numeric_limits<std::uint64_t>::max(),
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
            hostServiceState.threadSignalMask, live, live[37],
            guestScheduler.current_id(), {}, &guestMemory);
        if (!resumed) return false;
        std::memcpy(stateBuffer.data(), live.data(), live.size() * sizeof(std::uint64_t));
        const auto entry = resumed->recompile
            ? static_cast<std::int64_t>(resumed->programCounter)
            : static_cast<std::int64_t>(resumed->executionContext);
        selectKernel(entry);
        return true;
    };
    std::vector<std::uint64_t> signalRegisterSnapshot;
    const auto deliverPendingSignal = [&]() -> std::optional<std::int32_t> {
        hostServiceState.ingest_host_signals();
        const auto suspended = rawProcState.sigsuspendRestore;
        rawProcState.sigsuspendRestore.reset();
        const auto restoreSuspendedMask = [&]() {
            if (!suspended || suspended->first != guestScheduler.current_id()) return;
            hostServiceState.signals.set_mask_value(suspended->second);
            hostServiceState.threadSignalMask = hostServiceState.signals.mask();
        };
        if (!signalRuntime.has_pending_delivery(hostServiceState.signals,
                hostServiceState.threadPendingSignals, hostServiceState.threadSignalMask,
                stateBuffer.size() / sizeof(std::uint64_t))) {
            restoreSuspendedMask();
            return std::nullopt;
        }
        const auto* stateRegisters = static_cast<const std::uint64_t*>(stateBuffer.data());
        const auto stateWords = stateBuffer.size() / sizeof(std::uint64_t);
        signalRegisterSnapshot.assign(stateRegisters, stateRegisters + stateWords);
        const auto delivery = signalRuntime.deliver(
            hostServiceState.signals, hostServiceState.queuedProcessSignals,
            hostServiceState.threadPendingSignals, hostServiceState.threadSignalMask,
            signalRegisterSnapshot, signalRegisterSnapshot[37],
            static_cast<std::uint64_t>(activeEntry),
            guestScheduler.current_id(), &guestMemory);
        if (delivery.outcome == GuestSignalOutcome::ProcessExit) return delivery.exitStatus;
        if (delivery.outcome == GuestSignalOutcome::UnsupportedStop) {
            throw std::runtime_error("Guest stop and continue signals are unsupported");
        }
        if (delivery.outcome == GuestSignalOutcome::HandlerEntered) {
            std::memcpy(stateBuffer.data(), signalRegisterSnapshot.data(),
                        signalRegisterSnapshot.size() * sizeof(std::uint64_t));
            selectKernel(static_cast<std::int64_t>(delivery.handler));
            if (suspended && suspended->first == guestScheduler.current_id()) {
                signalRuntime.set_top_frame_mask(suspended->first, suspended->second);
            }
        } else {
            restoreSuspendedMask();
        }
        return std::nullopt;
    };
    const auto dynamicImageClosure = [&](std::uint64_t root) {
        std::vector<std::uint64_t> closure;
        std::set<std::uint64_t> visited;
        std::function<void(std::uint64_t)> visit;
        visit = [&](std::uint64_t header) {
            if (header == 0 || !visited.insert(header).second) return;
            const auto record = std::find_if(image.loaded_image_graph.begin(),
                image.loaded_image_graph.end(), [header](const auto& item) {
                    return item.guest_header == header;
                });
            if (record == image.loaded_image_graph.end()) return;
            for (const auto& dependency : record->dependencies) {
                if (dependency.resolved_header) visit(*dependency.resolved_header);
            }
            if (record->dynamic_unload_candidate) closure.push_back(header);
        };
        visit(root);
        std::reverse(closure.begin(), closure.end());
        return closure;
    };
    const auto acquireExitLease = [&](GuestCallbackFrame& frame, std::uint64_t header) {
        frame.activeExitCallbackImage.reset();
        if (header != 0) frame.activeExitCallbackImage = header;
        frame.activeExitLease = dynamicImageClosure(header);
        for (const auto leasedHeader : frame.activeExitLease)
            ++dynamicImageOwners[leasedHeader];
        if (header != 0) ++activeImageExitCallbacks[header];
    };
    const auto releaseExitLease = [&](GuestCallbackFrame& frame) {
        if (frame.activeExitCallbackImage) {
            const auto active = activeImageExitCallbacks.find(*frame.activeExitCallbackImage);
            if (active == activeImageExitCallbacks.end() || active->second == 0) {
                throw std::logic_error("Dynamic image finalizer activity is inconsistent");
            }
            if (--active->second == 0) activeImageExitCallbacks.erase(active);
            frame.activeExitCallbackImage.reset();
        }
        for (const auto header : frame.activeExitLease) {
            const auto owner = dynamicImageOwners.find(header);
            if (owner == dynamicImageOwners.end() || owner->second == 0) {
                throw std::logic_error("Dynamic image callback lease is inconsistent");
            }
            if (--owner->second == 0) dynamicImageOwners.erase(owner);
        }
        frame.activeExitLease.clear();
    };
    const auto ensureDynamicImageGroup = [&](std::uint64_t root) {
        if (dynamicImageGroups.find(root) != dynamicImageGroups.end()) return;
        auto closure = dynamicImageClosure(root);
        if (closure.empty() || closure.front() != root) return;
        for (const auto header : closure) ++dynamicImageOwners[header];
        dynamicImageGroups.emplace(root, std::move(closure));
    };
    const auto releaseDynamicImageGroup = [&](std::uint64_t root) {
        std::vector<std::uint64_t> finalizable;
        const auto group = dynamicImageGroups.find(root);
        if (group == dynamicImageGroups.end()) return finalizable;
        if (imageHandles.nodelete(root)) return finalizable;
        for (const auto header : group->second) {
            const auto owners = dynamicImageOwners.find(header);
            if (owners == dynamicImageOwners.end() || owners->second == 0)
                throw std::logic_error("Dynamic image ownership is inconsistent");
            if (--owners->second != 0) {
                if (!imageHandles.nodelete(header) &&
                    !imageHandles.has_open_reference_for_image(header)) {
                    pendingDynamicUnloads.insert(header);
                }
                continue;
            }
            dynamicImageOwners.erase(owners);
            const auto active = activeImageExitCallbacks.find(header);
            if (active != activeImageExitCallbacks.end() && active->second != 0 &&
                !imageHandles.nodelete(header) &&
                !imageHandles.has_open_reference_for_image(header)) {
                pendingDynamicUnloads.insert(header);
                continue;
            }
            pendingDynamicUnloads.erase(header);
            const auto record = std::find_if(image.loaded_image_graph.begin(),
                image.loaded_image_graph.end(), [header](const auto& item) {
                    return item.guest_header == header;
                });
            if (record != image.loaded_image_graph.end() &&
                record->dynamic_unload_candidate && !imageHandles.nodelete(header) &&
                !imageHandles.has_open_reference_for_image(header) &&
                (activeImageExitCallbacks.find(header) == activeImageExitCallbacks.end() ||
                 activeImageExitCallbacks.at(header) == 0)) {
                finalizable.push_back(header);
            }
        }
        dynamicImageGroups.erase(group);
        return finalizable;
    };
    const auto takePendingDynamicUnloads = [&]() {
        std::vector<std::uint64_t> ready;
        for (auto iter = pendingDynamicUnloads.begin(); iter != pendingDynamicUnloads.end();) {
            const auto header = *iter;
            const auto owners = dynamicImageOwners.find(header);
            const auto active = activeImageExitCallbacks.find(header);
            if (imageHandles.nodelete(header)) {
                iter = pendingDynamicUnloads.erase(iter);
                continue;
            }
            if (imageHandles.has_open_reference_for_image(header) ||
                (owners != dynamicImageOwners.end() && owners->second != 0) ||
                (active != activeImageExitCallbacks.end() && active->second != 0)) {
                ++iter;
                continue;
            }
            const auto record = std::find_if(image.loaded_image_graph.begin(),
                image.loaded_image_graph.end(), [header](const auto& item) {
                    return item.guest_header == header;
                });
            if (record == image.loaded_image_graph.end() ||
                !record->dynamic_unload_candidate) {
                iter = pendingDynamicUnloads.erase(iter);
                continue;
            }
            ready.push_back(header);
            iter = pendingDynamicUnloads.erase(iter);
        }
        std::unordered_set<std::uint64_t> readySet(ready.begin(), ready.end());
        std::unordered_map<std::uint64_t, std::size_t> pendingIndegree;
        std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> pendingDependencies;
        for (const auto header : ready) pendingIndegree.emplace(header, 0);
        for (const auto header : ready) {
            const auto record = std::find_if(image.loaded_image_graph.begin(),
                image.loaded_image_graph.end(), [header](const auto& item) {
                    return item.guest_header == header;
                });
            if (record == image.loaded_image_graph.end()) continue;
            for (const auto& dependency : record->dependencies) {
                if (dependency.resolved_header &&
                    readySet.find(*dependency.resolved_header) != readySet.end()) {
                    pendingDependencies[header].push_back(*dependency.resolved_header);
                    ++pendingIndegree[*dependency.resolved_header];
                }
            }
        }
        std::vector<std::uint64_t> ordered;
        std::set<std::uint64_t> roots;
        for (const auto& [header, degree] : pendingIndegree) {
            if (degree == 0) roots.insert(header);
        }
        while (!roots.empty()) {
            const auto header = *roots.begin();
            roots.erase(roots.begin());
            ordered.push_back(header);
            for (const auto dependency : pendingDependencies[header]) {
                auto& degree = pendingIndegree[dependency];
                if (--degree == 0) roots.insert(dependency);
            }
        }
        for (const auto header : readySet) {
            if (std::find(ordered.begin(), ordered.end(), header) == ordered.end())
                ordered.push_back(header);
        }
        return ordered;
    };
    const auto unloadDynamicImages = [&](const std::vector<std::uint64_t>& headers) {
        std::unordered_set<std::uint64_t> removed;
        for (const auto header : headers) {
            if (imageHandles.nodelete(header) ||
                imageHandles.has_open_reference_for_image(header) ||
                (activeImageExitCallbacks.find(header) != activeImageExitCallbacks.end() &&
                 activeImageExitCallbacks.at(header) != 0) ||
                (dynamicImageOwners.find(header) != dynamicImageOwners.end() &&
                 dynamicImageOwners.at(header) != 0)) {
                finalizedDynamicImages.erase(header);
                continue;
            }
            const auto record = std::find_if(image.loaded_image_graph.begin(),
                image.loaded_image_graph.end(), [header](const auto& item) {
                    return item.guest_header == header;
                });
            if (record == image.loaded_image_graph.end() ||
                !record->dynamic_unload_candidate) continue;
            if (record->backing_offset > static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max())) {
                throw std::runtime_error("Dynamic image backing exceeds allocator range");
            }
            mappingHeap.release(static_cast<std::int64_t>(record->backing_offset));

            for (const auto& range : record->mapped_ranges) {
                auto index = std::size_t{0};
                while (index < resident[0]) {
                    const auto row = 1 + 4 * index;
                    if (resident[row] != range.first) {
                        ++index;
                        continue;
                    }
                    guestMemory.remove_mapping(range.first);
                    const auto remaining = static_cast<std::size_t>(resident[0]) - index - 1;
                    if (remaining != 0) {
                        std::move(resident.begin() + static_cast<std::ptrdiff_t>(row + 4),
                                  resident.begin() + static_cast<std::ptrdiff_t>(row + 4 +
                                      remaining * 4),
                                  resident.begin() + static_cast<std::ptrdiff_t>(row));
                    }
                    --resident[0];
                }
            }
            const auto ownsAddress = [&record](std::uint64_t address) {
                return std::any_of(record->mapped_ranges.begin(), record->mapped_ranges.end(),
                    [address](const auto& range) {
                        return range.first <= address && address < range.second;
                    });
            };
            for (const auto slot : record->binding_slots) {
                imports.erase(static_cast<std::int64_t>(slot));
                guestStubs.erase(static_cast<std::int64_t>(slot));
                image.bindings.erase(static_cast<std::size_t>(slot));
                image.function_bindings.erase(static_cast<std::size_t>(slot));
                image.authentication.erase(static_cast<std::size_t>(slot));
            }
            for (const auto stub : record->synthetic_import_stubs)
                imports.erase(static_cast<std::int64_t>(stub));
            auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
            for (const auto token : record->authentication_tokens) {
                const auto slot = guestTokenStart + 3 * token;
                if (token < live[guestTokenCount]) {
                    live[slot] = 0;
                    live[slot + 1] = 0;
                    live[slot + 2] = 0;
                }
            }
            image.runtime_local_images.erase(std::remove_if(
                image.runtime_local_images.begin(), image.runtime_local_images.end(),
                [header](const auto& runtimeImage) {
                    return runtimeImage.guest_header == header;
                }), image.runtime_local_images.end());
            image.mapped_image_segments.erase(std::remove_if(
                image.mapped_image_segments.begin(), image.mapped_image_segments.end(),
                [header](const auto& segment) { return segment.guest_header == header; }),
                image.mapped_image_segments.end());
            image.text_regions.erase(std::remove_if(image.text_regions.begin(),
                image.text_regions.end(), [&](const auto& range) {
                    return range.first >= 0 && ownsAddress(static_cast<std::uint64_t>(range.first));
                }), image.text_regions.end());
            image.code_abi_regions.erase(std::remove_if(image.code_abi_regions.begin(),
                image.code_abi_regions.end(), [&](const auto& range) {
                    return range.start >= 0 && ownsAddress(static_cast<std::uint64_t>(range.start));
                }), image.code_abi_regions.end());
            image.global_scope_headers.erase(std::remove(image.global_scope_headers.begin(),
                image.global_scope_headers.end(), header), image.global_scope_headers.end());
            image.loaded_image_order.erase(std::remove_if(image.loaded_image_order.begin(),
                image.loaded_image_order.end(), [header](const auto& item) {
                    return item.header == header;
                }), image.loaded_image_order.end());
            image.loaded_image_graph.erase(std::remove_if(image.loaded_image_graph.begin(),
                image.loaded_image_graph.end(), [header](const auto& item) {
                    return item.guest_header == header;
                }), image.loaded_image_graph.end());
            image.thread_local_images.erase(header);
            for (auto iter = image.modeled_global_export_provenance.begin();
                 iter != image.modeled_global_export_provenance.end();) {
                if (iter->second.guest_header == header)
                    iter = image.modeled_global_export_provenance.erase(iter);
                else ++iter;
            }
            image.image_paths.erase(header);
            image.image_slides.erase(header);
            for (auto iter = imageHeadersByPath.begin(); iter != imageHeadersByPath.end();) {
                if (iter->second == header) iter = imageHeadersByPath.erase(iter);
                else ++iter;
            }
            for (auto iter = dlsymImages.begin(); iter != dlsymImages.end();) {
                if (iter->first == header) iter = dlsymImages.erase(iter);
                else ++iter;
            }
            for (auto iter = dladdrSymbols.begin(); iter != dladdrSymbols.end();) {
                if (std::get<0>(iter->first) == header) iter = dladdrSymbols.erase(iter);
                else ++iter;
            }
            imageHandles.unregister_loaded(header);
            loadedImageHeaders.erase(header);
            finalizedDynamicImages.erase(header);
            pendingDynamicUnloads.erase(header);
            removed.insert(header);
            processCodeCacheInvalidated = true;
        }
        if (!removed.empty()) {
            imageRegistry.remove(removed);
            std::memcpy(residentBuffer.data(), resident.data(),
                        resident.size() * sizeof(std::uint64_t));
        }
    };
    std::function<bool(std::vector<std::uint64_t>, std::optional<std::int32_t>,
                       std::optional<std::uint64_t>)> beginImageExitDrain;
    const auto beginExitDrain = [&](std::uint64_t dso,
                                    std::optional<std::int32_t> exitStatus) {
        const auto handler = exitRegistry.take_next(dso);
        if (!handler)
            return beginImageExitDrain(takePendingDynamicUnloads(), exitStatus, std::nullopt);
        auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
        GuestCallbackFrame frame;
        frame.savedRegisters.assign(live, live + 36);
        frame.savedRegisters[34] = 0;
        frame.savedVectorAndAuth.assign(live + 806, live + guestTokenCount);
        frame.authenticationDepth = live[37];
        frame.exitDso = dso;
        frame.exitStatus = exitStatus;
        frame.exitThread = guestScheduler.current_id();
        acquireExitLease(frame, handler->owner_image);
        callbackFramesByThread[guestScheduler.current_id()].push_back(std::move(frame));
        live[0] = handler->argument;
        live[30] = guestReturnSentinel;
        live[33] = handler->target;
        selectKernel(static_cast<std::int64_t>(handler->target));
        return true;
    };
    const auto scheduleNextImageExitHandler = [&](GuestCallbackFrame& frame) {
        while (frame.nextExitImage < frame.exitImages.size()) {
            const auto header = frame.exitImages[frame.nextExitImage];
            const bool continuingCurrentImage = frame.currentExitImage &&
                *frame.currentExitImage == header;
            if (!continuingCurrentImage) {
                const auto owners = dynamicImageOwners.find(header);
                if ((owners != dynamicImageOwners.end() && owners->second != 0) ||
                    imageHandles.nodelete(header) ||
                    imageHandles.has_open_reference_for_image(header) ||
                    (activeImageExitCallbacks.find(header) != activeImageExitCallbacks.end() &&
                     activeImageExitCallbacks.at(header) != 0)) {
                    ++frame.nextExitImage;
                    continue;
                }
            }
            const auto handler = exitRegistry.take_next_for_image(header);
            if (!handler) {
                const auto owners = dynamicImageOwners.find(header);
                if ((owners != dynamicImageOwners.end() && owners->second != 0) ||
                    imageHandles.nodelete(header) ||
                    imageHandles.has_open_reference_for_image(header) ||
                    (activeImageExitCallbacks.find(header) != activeImageExitCallbacks.end() &&
                     activeImageExitCallbacks.at(header) != 0)) {
                    finalizedDynamicImages.erase(header);
                    ++frame.nextExitImage;
                    frame.currentExitImage.reset();
                    continue;
                }
                finalizedDynamicImages.insert(header);
                frame.drainedExitImages.push_back(header);
                ++frame.nextExitImage;
                frame.currentExitImage.reset();
                continue;
            }
            frame.currentExitImage = header;
            acquireExitLease(frame, header);
            auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
            std::copy(frame.savedRegisters.begin(), frame.savedRegisters.end(), live);
            std::copy(frame.savedVectorAndAuth.begin(), frame.savedVectorAndAuth.end(),
                      live + 806);
            live[37] = frame.authenticationDepth;
            live[0] = handler->argument;
            live[30] = guestReturnSentinel;
            live[33] = handler->target;
            callbackFramesByThread[guestScheduler.current_id()].push_back(std::move(frame));
            selectKernel(static_cast<std::int64_t>(handler->target));
            return true;
        }
        return false;
    };
    beginImageExitDrain = [&](std::vector<std::uint64_t> images,
                              std::optional<std::int32_t> exitStatus,
                              std::optional<std::uint64_t> exitThreadResult) {
        if (images.empty()) return false;
        auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
        GuestCallbackFrame frame;
        frame.savedRegisters.assign(live, live + 36);
        frame.savedRegisters[34] = 0;
        frame.savedVectorAndAuth.assign(live + 806, live + guestTokenCount);
        frame.authenticationDepth = live[37];
        frame.exitImages = std::move(images);
        frame.exitStatus = exitStatus;
        frame.exitThreadResult = exitThreadResult;
        if (scheduleNextImageExitHandler(frame)) return true;
        unloadDynamicImages(frame.drainedExitImages);
        return false;
    };
    std::uint64_t dispatchSequence = 0;
    bool socketSuccessDiagnosticEmitted = false;
    bool socketUnsupportedDiagnosticEmitted = false;
    while (true) {
        syncTproThreadAccess();
        if (!applyJitWriteProtection ||
            !applyJitWriteProtection(guestScheduler.current().jitWriteProtect)) {
            throw std::runtime_error("Guest thread MAP_JIT mode could not be applied");
        }
        if (const auto signalExit = deliverPendingSignal()) return *signalExit;
        auto& callbackFrames = callbackFramesByThread[guestScheduler.current_id()];
        auto* registers = static_cast<std::uint64_t*>(stateBuffer.data());
        if (clearInvalidatedProcessCodeCache()) {
            const auto continuation = registers[33];
            if (continuation != guestReturnSentinel) {
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
        syncWritableTable(bootstrap.errnoSlot);
        registers[36] = options.dispatchBlocks;
        registers[guestJitWriteState] = guestScheduler.current().jitWriteProtect ? 0ULL : 0x200000ULL;
        registers[guestFaultAddress] = kNoFault;
        registers[guestCacheFaultAddress] = kNoFault;
        registers[guestCacheFaultWidth] = 0;
        // A guest callback may tail-call a host service. Its sentinel return
        // must complete the callback without compiling the sentinel as code.
        if (registers[33] == guestReturnSentinel) {
            registers[34] = 0;
        } else if (progressTrace.enabled() || jitExecutionTrace.enabled()) {
            ++dispatchSequence;
            if (progressTrace.enabled()) progressTrace.recordDispatch();
            const bool reportDispatch = dispatchSequence <= 16 || dispatchSequence % 32 == 0;
            const auto dispatchedPc = registers[33];
            const auto dispatchStarted = std::chrono::steady_clock::now();
            if (progressTrace.enabled() && reportDispatch) {
                progressTrace.event("dispatch-start", registers[33], dispatchSequence);
            }
            if (activeKernel->usesInterpreter) {
                if (progressTrace.enabled()) {
                    progressTrace.event("interpreter-dispatch", registers[33],
                                        activeKernel->interpretedPackets.size());
                }
            }
            dispatchActiveKernel();
            if (jitExecutionTrace.enabled()) {
                const auto owner = executableJitOwnerFor(dispatchedPc);
                if (owner) {
                    const auto [ownerBegin, ownerEnd, backing, mapJit] = *owner;
                    jitExecutionTrace.dispatch(dispatchedPc, ownerBegin, ownerEnd,
                                               backing, jitTraceGeneration, mapJit,
                                               activeKernel->usesInterpreter);
                }
            }
            if (progressTrace.enabled() && reportDispatch) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - dispatchStarted).count();
                progressTrace.event("dispatch-done", registers[33],
                                    static_cast<std::uint64_t>(elapsed));
            }
        } else {
            dispatchActiveKernel();
        }
        registers = static_cast<std::uint64_t*>(stateBuffer.data());
        const auto status = registers[34];
        if ((status == 1 || status == 7 || status == 8) &&
            preemptAtDispatchBoundary(status)) {
            continue;
        }
        if (status == 0) {
            if (signalRuntime.has_frames(guestScheduler.current_id()) && resumeSignalFrame()) continue;
            if (!callbackFrames.empty() && registers[30] == guestReturnSentinel &&
                (!callbackFrames.back().exitDso ||
                 callbackFrames.back().exitThread == guestScheduler.current_id())) {
                auto frame = std::move(callbackFrames.back());
                callbackFrames.pop_back();
                if (frame.dyldBootstrapCallback) {
                    writableTproCallbackCompleted = true;
                    dyldBootstrapTrace.event("tpro-callback-return");
                }
                if (registers[37] != frame.authenticationDepth) {
                    throw std::runtime_error("Guest image callback left authentication stack unbalanced");
                }
                if (frame.activeExitCallbackImage || !frame.activeExitLease.empty())
                    releaseExitLease(frame);
                if (frame.exitDso) {
                    const auto handler = exitRegistry.take_next(*frame.exitDso);
                    if (handler) {
                        auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
                        std::copy(frame.savedRegisters.begin(), frame.savedRegisters.end(), live);
                        std::copy(frame.savedVectorAndAuth.begin(), frame.savedVectorAndAuth.end(), live + 806);
                        live[37] = frame.authenticationDepth;
                        acquireExitLease(frame, handler->owner_image);
                        live[0] = handler->argument;
                        live[30] = guestReturnSentinel;
                        live[33] = handler->target;
                        callbackFrames.push_back(std::move(frame));
                        selectKernel(static_cast<std::int64_t>(handler->target));
                        continue;
                    }
                    frame.exitDso.reset();
                }
                auto pending = takePendingDynamicUnloads();
                for (const auto header : pending) {
                    const bool alreadyQueued =
                        std::find(frame.exitImages.begin(), frame.exitImages.end(), header) !=
                            frame.exitImages.end() ||
                        std::find(frame.drainedExitImages.begin(),
                                  frame.drainedExitImages.end(), header) !=
                            frame.drainedExitImages.end();
                    if (!alreadyQueued) frame.exitImages.push_back(header);
                }
                if (frame.nextExitImage < frame.exitImages.size() &&
                    scheduleNextImageExitHandler(frame)) {
                    continue;
                }
                unloadDynamicImages(frame.drainedExitImages);
                if (frame.exitStatus) return *frame.exitStatus;
                if (frame.exitThreadResult) {
                    auto* restored = static_cast<std::uint64_t*>(stateBuffer.data());
                    std::copy(frame.savedRegisters.begin(), frame.savedRegisters.end(), restored);
                    std::copy(frame.savedVectorAndAuth.begin(),
                              frame.savedVectorAndAuth.end(), restored + 806);
                    restored[37] = frame.authenticationDepth;
                    std::vector<std::uint64_t> live(
                        restored, restored + stateBuffer.size() / sizeof(std::uint64_t));
                    if (finishGuestThread(*frame.exitThreadResult, live)) return 0;
                    continue;
                }
                if (frame.nextImageNotification < frame.imageNotifications.size()) {
                    const auto notification =
                        frame.imageNotifications[frame.nextImageNotification++];
                    callbackFrames.push_back(std::move(frame));
                    auto* callbackRegisters = static_cast<std::uint64_t*>(stateBuffer.data());
                    const auto& pending = callbackFrames.back();
                    std::copy(pending.savedRegisters.begin(), pending.savedRegisters.end(),
                              callbackRegisters);
                    std::copy(pending.savedVectorAndAuth.begin(),
                              pending.savedVectorAndAuth.end(), callbackRegisters + 806);
                    callbackRegisters[37] = pending.authenticationDepth;
                    std::copy(notification.arguments.begin(), notification.arguments.end(),
                              callbackRegisters);
                    callbackRegisters[30] = guestReturnSentinel;
                    callbackRegisters[33] = notification.callback;
                    selectKernel(static_cast<std::int64_t>(notification.callback));
                    continue;
                }
                if (frame.nextBulkImageCallback < frame.bulkImageCallbacks.size()) {
                    const auto callback =
                        frame.bulkImageCallbacks[frame.nextBulkImageCallback++];
                    callbackFrames.push_back(std::move(frame));
                    auto* callbackRegisters = static_cast<std::uint64_t*>(stateBuffer.data());
                    const auto& pending = callbackFrames.back();
                    std::copy(pending.savedRegisters.begin(), pending.savedRegisters.end(),
                              callbackRegisters);
                    std::copy(pending.savedVectorAndAuth.begin(),
                              pending.savedVectorAndAuth.end(), callbackRegisters + 806);
                    callbackRegisters[37] = pending.authenticationDepth;
                    std::copy(pending.arguments.front().begin(),
                              pending.arguments.front().end(), callbackRegisters);
                    callbackRegisters[30] = guestReturnSentinel;
                    callbackRegisters[33] = callback;
                    selectKernel(static_cast<std::int64_t>(callback));
                    continue;
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
                    std::copy_n(bootstrap.registers.begin(), 5, callbackRegisters);
                    callbackRegisters[30] = guestReturnSentinel;
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
                    callbackRegisters[30] = guestReturnSentinel;
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
                if (frame.registerBulkImageCallbackAfterReturn) {
                    if (frame.bulkImageCallbacks.size() != 1) {
                        throw std::logic_error("Invalid deferred bulk image callback registration");
                    }
                    if (!frame.activeBulkImageCallbackLease.empty())
                        bulkImageCallbackLeases.push_back(
                            std::move(frame.activeBulkImageCallbackLease));
                    bulkImageCallbacks.push_back(frame.bulkImageCallbacks.front());
                }
                for (const auto allocation : frame.temporaryGuestAllocations) {
                    auto freeRegisters = frame.savedRegisters;
                    freeRegisters[0] = allocation;
                    const auto result = dispatch_guest_host_service(
                        freeRegisters, guestMemory, bootstrap, hostServiceState, 56);
                    if (result.exited) {
                        throw std::runtime_error("Guest bulk image callback allocation cleanup failed");
                    }
                }
                auto* restored = static_cast<std::uint64_t*>(stateBuffer.data());
                std::copy(frame.savedRegisters.begin(), frame.savedRegisters.end(), restored);
                std::copy(frame.savedVectorAndAuth.begin(), frame.savedVectorAndAuth.end(),
                          restored + 806);
                restored[37] = frame.authenticationDepth;
                if (restored[33] != guestReturnSentinel) {
                    selectKernel(static_cast<std::int64_t>(restored[33]));
                }
                continue;
            }
            if (registers[30] == guestReturnSentinel && guestScheduler.current_id() != 1) {
                discardThreadCallbacks(guestScheduler.current_id());
                if (beginImageExitDrain(takePendingDynamicUnloads(), std::nullopt,
                                        std::nullopt)) continue;
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
        if (status == kQsortAdapterStatus) {
            const auto entry = registers[33];
            if (entry > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                qsortEntries.count(static_cast<std::int64_t>(entry)) == 0) {
                throw std::runtime_error("Unexpected qsort ABI adapter dispatch");
            }
            const auto caller = registers[30] >= 4 ? registers[30] - 4 : 0;
            const auto callerAbi = caller > 0 && caller <= static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())
                ? image.is_arm64e_code(static_cast<std::int64_t>(caller))
                : std::nullopt;
            const auto comparator = registers[3];
            auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
            const auto comparatorIsCode = comparator <= static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) &&
                image.is_code(static_cast<std::int64_t>(comparator));
            const auto comparatorAbi = comparatorIsCode
                ? image.is_arm64e_code(static_cast<std::int64_t>(comparator))
                : std::nullopt;
            if (registers[2] > 1 && callerAbi && !*callerAbi && comparatorIsCode &&
                comparatorAbi && !*comparatorAbi) {
                std::optional<std::size_t> token;
                for (std::size_t index = 0; index < live[guestTokenCount]; ++index) {
                    const auto slot = guestTokenStart + 3 * index;
                    if (live[slot] == comparator && live[slot + 1] == 0 &&
                        live[slot + 2] == 0) {
                        token = index;
                        break;
                    }
                }
                if (!token) {
                    ensureTokenCapacity(1, "qsort ARM64 comparator");
                    live = static_cast<std::uint64_t*>(stateBuffer.data());
                    const auto index = static_cast<std::size_t>(live[guestTokenCount]);
                    const auto capacity = static_cast<std::size_t>(live[guestTokenCapacity]);
                    if (index >= capacity)
                        throw std::runtime_error("qsort comparator token capacity was not reserved");
                    const auto slot = guestTokenStart + 3 * index;
                    live[slot] = comparator;
                    live[slot + 1] = 0;
                    live[slot + 2] = 0;
                    live[guestTokenCount] = index + 1;
                    token = index;
                }
                live[3] = encode_guest_pointer(comparator, 0, *token);
            }
            live[35] = kQsortAdapterMarker;
            live[33] = entry;
            selectKernel(static_cast<std::int64_t>(entry));
            continue;
        }
        if (status == 4) {
            std::vector<std::uint64_t> hostRegisters(
                static_cast<const std::uint64_t*>(stateBuffer.data()),
                static_cast<const std::uint64_t*>(stateBuffer.data()) +
                    stateBuffer.size() / sizeof(std::uint64_t));
            const auto service = registers[35];
            lastHostTransition = "service=" + std::to_string(service) + "@pc=0x" +
                hexadecimal(hostRegisters[33]);
            recordTransition(lastHostTransition + " x0=0x" + hexadecimal(hostRegisters[0]) +
                " x1=0x" + hexadecimal(hostRegisters[1]));
            progressTrace.event("host-service", hostRegisters[33], service);
            if (service == kMemcpyChkFailureService ||
                service == kMemsetChkFailureService) {
                if (hostRegisters[2] <= hostRegisters[3]) {
                    throw std::runtime_error("Invalid checked memory failure dispatch");
                }
                const auto resume = hostRegisters[33];
                const auto faultPC = resume >= 4 ? resume - 4 : resume;
                hostRegisters[33] = faultPC;
                const GuestSynchronousFault fault{SIGTRAP, 0, faultPC, faultPC};
                const auto delivery = signalRuntime.deliver_synchronous(
                    hostServiceState.signals, hostServiceState.threadSignalMask,
                    hostRegisters, guestMemory, fault, hostRegisters[37],
                    static_cast<std::uint64_t>(activeEntry),
                    guestScheduler.current_id());
                if (delivery.outcome == GuestSignalOutcome::ProcessExit)
                    return delivery.exitStatus;
                if (delivery.outcome == GuestSignalOutcome::HandlerEntered) {
                    std::memcpy(stateBuffer.data(), hostRegisters.data(),
                                hostRegisters.size() * sizeof(std::uint64_t));
                    selectKernel(static_cast<std::int64_t>(delivery.handler));
                    continue;
                }
                throw std::runtime_error("Guest checked memory SIGTRAP was not delivered");
            }
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
                    const auto callerPc = hostRegisters[guestFaultAddress] != kNoFault
                        ? hostRegisters[guestFaultAddress]
                        : resume >= 4 ? resume - 4 : 0;
                    const auto callerAbi = callerPc != 0 && callerPc <=
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                        ? image.is_arm64e_code(static_cast<std::int64_t>(callerPc))
                        : std::nullopt;
                    if (!callerAbi || *callerAbi)
                        throw std::runtime_error("Guest exit callback requires an authenticated IA/0 pointer");
                }
                if (target > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                    !image.is_code(static_cast<std::int64_t>(target)))
                    throw std::runtime_error("Guest exit callback target is not executable");
                std::uint64_t dso = service == 167 ? hostRegisters[2] : 0;
                std::uint64_t ownerImage = 0;
                const auto findImageOwner = [&](std::uint64_t address) {
                    const auto owner = std::find_if(image.mapped_image_segments.begin(),
                        image.mapped_image_segments.end(), [address](
                            const MappedImageSegment& segment) {
                            return segment.start >= 0 && segment.end > segment.start &&
                                address >= static_cast<std::uint64_t>(segment.start) &&
                                address < static_cast<std::uint64_t>(segment.end);
                        });
                    return owner == image.mapped_image_segments.end()
                        ? std::uint64_t{0} : owner->guest_header;
                };
                if (service == 167) {
                    ownerImage = findImageOwner(dso);
                    if (ownerImage == 0) ownerImage = findImageOwner(target);
                } else {
                    ownerImage = findImageOwner(target);
                    dso = ownerImage;
                }
                const GuestExitHandler handler{target,
                    service == 167 ? hostRegisters[1] : 0,
                    dso,
                    ownerImage,
                    service == 167 ? GuestExitHandlerKind::Cxa : GuestExitHandlerKind::Standard};
                const bool registered = exitRegistry.register_handler(handler);
                if (registered && ownerImage != 0)
                    finalizedDynamicImages.erase(ownerImage);
                registers[0] = registered ? 0 : static_cast<std::uint64_t>(-1);
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
            if (service == kJitWriteStateService) {
                if (!applyJitWriteProtection(hostRegisters[guestJitWriteState] == 0)) {
                    throw std::runtime_error("Guest MAP_JIT write-protection state is unavailable");
                }
                continue;
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
            if (service == 178) {
                const bool cxxMethod = dyldApis.object != 0 &&
                    hostRegisters[0] == dyldApis.object;
                if (cxxMethod && !dyldApisPublicationSeeded) {
                    throw std::runtime_error("dyld API object reached before gAPIs publication");
                }
                if (startupIndex >= writableTproStartupTargets) {
                    std::fputs("dyld: Illegal TPRO memory access\n", stderr);
                    std::abort();
                }
                auto callback = hostRegisters[cxxMethod ? 1U : 0U];
                if ((callback & 0xffff000000000000ULL) == 0xa64e000000000000ULL) {
                    const auto authenticated = decode_guest_ia0_callback(
                        hostRegisters.data(), hostRegisters.size(), callback);
                    if (!authenticated) {
                        throw std::runtime_error(
                            "Guest writable-TPRO callback authentication failed");
                    }
                    callback = *authenticated;
                }
                if (callback > static_cast<std::uint64_t>(
                                   std::numeric_limits<std::int64_t>::max()) ||
                    !image.is_code(static_cast<std::int64_t>(callback))) {
                    throw std::runtime_error(
                        "Guest writable-TPRO callback target is not executable");
                }
                GuestCallbackFrame frame;
                frame.savedRegisters.assign(hostRegisters.begin(), hostRegisters.begin() + 36);
                frame.savedRegisters[33] = hostRegisters[30];
                frame.savedRegisters[34] = 0;
                frame.savedVectorAndAuth.assign(hostRegisters.begin() + 806,
                                                hostRegisters.begin() + guestTokenCount);
                frame.authenticationDepth = hostRegisters[37];
                frame.tproScope = true;
                frame.dyldBootstrapCallback = cxxMethod;
                frame.callback = callback;
                callbackFrames.push_back(std::move(frame));
                syncTproThreadAccess();
                if (cxxMethod) {
                    dyldBootstrapTrace.event("tpro-callback-enter");
                }
                auto* callbackRegisters = static_cast<std::uint64_t*>(stateBuffer.data());
                callbackRegisters[0] = hostRegisters[cxxMethod ? 2U : 1U];
                callbackRegisters[30] = guestReturnSentinel;
                callbackRegisters[33] = callback;
                callbackRegisters[34] = 0;
                selectKernel(static_cast<std::int64_t>(callback));
                continue;
            }
            if (service >= 213 && service <= 215) {
                if (dyldApis.object == 0 || hostRegisters[0] != dyldApis.object) {
                    throw std::runtime_error(
                        "Dyld platform method did not use the published API object");
                }
                if (service == 213) {
                    registers[0] = dyld_base_platform(
                        static_cast<std::uint32_t>(hostRegisters[1]));
                } else if (service == 214) {
                    registers[0] = bootstrap.activePlatform;
                } else {
                    registers[0] = dyld_is_simulator_platform(
                        static_cast<std::uint32_t>(hostRegisters[1])) ? 1U : 0U;
                }
                continue;
            }
            if (service == 216) {
                const bool cxxMethod = dyldApis.object != 0 &&
                    hostRegisters[0] == dyldApis.object;
                auto callback = hostRegisters[cxxMethod ? 1U : 0U];
                if ((callback & 0xffff000000000000ULL) == 0xa64e000000000000ULL) {
                    const auto authenticated = decode_guest_ia0_callback(
                        hostRegisters.data(), hostRegisters.size(), callback);
                    if (!authenticated) {
                        throw std::runtime_error(
                            "Bulk image callback authentication failed");
                    }
                    callback = *authenticated;
                } else {
                    const auto resume = hostRegisters[33];
                    const auto callerAbi = resume >= 4 && resume - 4 <=
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                        ? image.is_arm64e_code(static_cast<std::int64_t>(resume - 4))
                        : std::nullopt;
                    if (!callerAbi || *callerAbi) {
                        throw std::runtime_error(
                            "Bulk image callback requires an authenticated IA/0 pointer");
                    }
                }
                if (callback > static_cast<std::uint64_t>(
                                   std::numeric_limits<std::int64_t>::max()) ||
                    !image.is_code(static_cast<std::int64_t>(callback))) {
                    throw std::runtime_error(
                        "Bulk image callback target is outside guest text");
                }
                std::uint64_t callbackOwner = 0;
                const auto ownerSegment = std::find_if(
                    image.mapped_image_segments.begin(), image.mapped_image_segments.end(),
                    [callback](const MappedImageSegment& segment) {
                        return segment.executable && segment.start >= 0 &&
                               segment.end > segment.start &&
                               static_cast<std::uint64_t>(segment.start) <= callback &&
                               callback < static_cast<std::uint64_t>(segment.end);
                    });
                if (ownerSegment != image.mapped_image_segments.end())
                    callbackOwner = ownerSegment->guest_header;
                GuestCallbackFrame frame;
                prepareBulkImageCallback(frame, hostRegisters, {callback},
                    0, imageRegistry.size(), true);
                frame.activeBulkImageCallbackLease = dynamicImageClosure(callbackOwner);
                for (const auto header : frame.activeBulkImageCallbackLease)
                    ++dynamicImageOwners[header];
                frame.nextBulkImageCallback = 1;
                callbackFrames.push_back(std::move(frame));
                auto* callbackRegisters = static_cast<std::uint64_t*>(stateBuffer.data());
                const auto& pending = callbackFrames.back();
                std::copy(pending.savedRegisters.begin(), pending.savedRegisters.end(),
                          callbackRegisters);
                std::copy(pending.savedVectorAndAuth.begin(),
                          pending.savedVectorAndAuth.end(), callbackRegisters + 806);
                callbackRegisters[37] = pending.authenticationDepth;
                std::copy(pending.arguments.front().begin(),
                          pending.arguments.front().end(), callbackRegisters);
                callbackRegisters[30] = guestReturnSentinel;
                callbackRegisters[33] = callback;
                selectKernel(static_cast<std::int64_t>(callback));
                continue;
            }
            if (service == 217 || service == 218) {
                if (dyldApis.object == 0 || hostRegisters[0] != dyldApis.object) {
                    throw std::runtime_error(
                        "Dyld shared-cache path method did not use the published API object");
                }
                std::optional<std::string> canonicalPath;
                if (hostRegisters[1] != 0) {
                    canonicalPath = canonicalSharedCachePath(
                        guest_cstring(guestMemory, hostRegisters[1]));
                }
                if (service == 217) {
                    const auto guestPath = canonicalPath
                        ? stableGuestString(*canonicalPath) : std::nullopt;
                    registers[0] = guestPath.value_or(0);
                } else {
                    registers[0] = canonicalPath ? 1U : 0U;
                }
                continue;
            }
            if (service == 227) {
                if (dyldApis.object == 0 || hostRegisters[0] != dyldApis.object) {
                    throw std::runtime_error(
                        "Dyld program SDK method did not use the published API object");
                }
                hostRegisters[0] = image.build_version &&
                    image.build_version->platform == bootstrap.activePlatform
                    ? dyld_program_sdk_version(image) : 0;
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 228) {
                if (dyldApis.object == 0 || hostRegisters[0] != dyldApis.object) {
                    throw std::runtime_error(
                        "Dyld program min OS method did not use the published API object");
                }
                hostRegisters[0] = image.build_version &&
                    image.build_version->platform == bootstrap.activePlatform
                    ? dyld_program_version(image.build_version->platform,
                                           image.build_version->minimum) : 0;
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 201) {
                if (dyldApis.object == 0 || hostRegisters[0] != dyldApis.object) {
                    throw std::runtime_error(
                        "_libdyld_initialize dispatch did not use the published guest dyld API object");
                }
                dyldBootstrapTrace.event("libdyld-initialize");
                const auto initializedDescriptors = initializeDyldRuntimeState();
                if (!dyldRuntimeInitialized) {
                    throw std::runtime_error("libdyld loader state is not initialized");
                }
                dyldBootstrapTrace.event("runtime-ready", initializedDescriptors);
                // Bootstrap may grow the authentication table and replace
                // stateBuffer. Refresh this view before writing the result.
                registers = static_cast<std::uint64_t*>(stateBuffer.data());
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
            if (service == kDyldConformanceOnDiskService) {
                // Prebuilt on-disk loader sets are not modeled, so no conformance is
                // found there; the Swift runtime then scans registered image sections.
                hostRegisters[0] = 2;
                hostRegisters[1] = 0;
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
                    const auto resume = hostRegisters[33];
                    const auto callerAbi = resume >= 4 && resume - 4 <=
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                        ? image.is_arm64e_code(static_cast<std::int64_t>(resume - 4))
                        : std::nullopt;
                    const bool executable = *resolved <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()) &&
                        image.is_code(static_cast<std::int64_t>(*resolved));
                    // arm64e dlsym signs code pointers with IA/0; data pointers stay raw.
                    if (callerAbi && *callerAbi && executable) {
                        if (!find_guest_pointer_token(hostRegisters, *resolved, 0, 0)) {
                            ensureTokenCapacity(1, "dlsym");
                            const auto* live = static_cast<const std::uint64_t*>(stateBuffer.data());
                            hostRegisters.assign(live, live +
                                stateBuffer.size() / sizeof(std::uint64_t));
                        }
                        const auto count = static_cast<std::size_t>(
                            hostRegisters[guestTokenCount]);
                        hostRegisters[0] = append_guest_pointer_token(
                            hostRegisters, *resolved, 0, 0);
                        if (hostRegisters[guestTokenCount] != count) {
                            auto* stateRegisters = static_cast<std::uint64_t*>(stateBuffer.data());
                            stateRegisters[guestTokenCount] = hostRegisters[guestTokenCount];
                            const auto slot = guestTokenStart + 3 * count;
                            std::copy_n(hostRegisters.begin() + static_cast<std::ptrdiff_t>(slot), 3,
                                        stateRegisters + slot);
                        }
                    } else {
                        hostRegisters[0] = *resolved;
                    }
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
                const auto assignedFallback = cachedV1TlvKeys.find(cachedOwner->first);
                auto parsed = parse_guest_tlv_descriptor(
                    descriptor, descriptorAddress,
                    assignedFallback == cachedV1TlvKeys.end()
                        ? GuestTlvDescriptorFormat::runtimeV2
                        : GuestTlvDescriptorFormat::diskV1,
                    geometry);
                if (assignedFallback != cachedV1TlvKeys.end()) {
                    if (parsed.key != 0) {
                        throw std::runtime_error(
                            "Cached unoptimized v2 TLV disk key is not zero");
                    }
                    parsed.key = assignedFallback->second;
                } else {
                    const auto staticOwner = cachedStaticTlvOwners.find(parsed.key);
                    if (staticOwner == cachedStaticTlvOwners.end() ||
                        staticOwner->second != cachedOwner->first) {
                        throw std::runtime_error(
                            "Cached v2 guest TLV key conflicts with its owner");
                    }
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
            if (service == 52 && !signalRuntime.has_frames(guestScheduler.current_id())) {
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
            if (service >= 203 && service <= 212) {
                const auto address = hostRegisters[0];
                constexpr std::uint64_t minimumStackSize = 16U * 1024U;
                if (std::getenv("ARM64_METAL_TRACE_THREADS"))
                    std::cerr << "arm64-metal pthread-attr service=" << service << " attr=0x"
                              << hexadecimal(address) << " arg=" << hostRegisters[1] << '\n';
                std::int32_t result = 0;
                if (service == 203) {
                    auto* bytes = guestMemory.buffer(address, sizeof(pthread_attr_t), true);
                    std::memset(bytes, 0, sizeof(pthread_attr_t));
                    threadAttributes.insert_or_assign(address, GuestThreadAttributes{});
                } else {
                    const auto attribute = threadAttributes.find(address);
                    if (attribute == threadAttributes.end()) {
                        result = EINVAL;
                    } else if (service == 204) {
                        threadAttributes.erase(attribute);
                    } else if (service == 205) {
                        if (hostRegisters[1] < minimumStackSize) {
                            result = EINVAL;
                        } else if (attribute->second.stackAddress) {
                            const auto top = *attribute->second.stackAddress +
                                             attribute->second.stackSize;
                            if (hostRegisters[1] > attribute->second.stackSize) {
                                result = EINVAL;
                            } else {
                                attribute->second.stackAddress = top - hostRegisters[1];
                                attribute->second.stackSize = hostRegisters[1];
                            }
                        } else {
                            attribute->second.stackSize = hostRegisters[1];
                        }
                    } else if (service == 206) {
                        attribute->second.guardSize = hostRegisters[1];
                    } else if (service == 207) {
                        const auto detachState = static_cast<std::int32_t>(hostRegisters[1]);
                        if (detachState != PTHREAD_CREATE_JOINABLE &&
                            detachState != PTHREAD_CREATE_DETACHED) {
                            result = EINVAL;
                        } else {
                            attribute->second.detachState = detachState;
                        }
                    } else if (service == 208 || service == 209) {
                        auto* output = guestMemory.buffer(
                            hostRegisters[1], sizeof(std::uint64_t), true);
                        const auto value = service == 208
                            ? attribute->second.stackSize : attribute->second.guardSize;
                        std::memcpy(output, &value, sizeof(value));
                    } else if (service == 210) {
                        auto* output = guestMemory.buffer(
                            hostRegisters[1], sizeof(std::int32_t), true);
                        std::memcpy(output, &attribute->second.detachState,
                                    sizeof(attribute->second.detachState));
                    } else if (service == 211) {
                        const auto stackAddress = hostRegisters[1];
                        const auto stackSize = hostRegisters[2];
                        const auto pageSize = bootstrap.pageSize;
                        if (stackAddress == 0 || stackSize < minimumStackSize ||
                            pageSize == 0 || stackAddress % pageSize != 0 ||
                            stackSize > std::numeric_limits<std::uint64_t>::max() - stackAddress) {
                            result = EINVAL;
                        } else {
                            attribute->second.stackAddress = stackAddress;
                            attribute->second.stackSize = stackSize;
                        }
                    } else {
                        auto* addressOutput = guestMemory.buffer(
                            hostRegisters[1], sizeof(std::uint64_t), true);
                        auto* sizeOutput = guestMemory.buffer(
                            hostRegisters[2], sizeof(std::uint64_t), true);
                        const auto stackAddress = attribute->second.stackAddress.value_or(0);
                        std::memcpy(addressOutput, &stackAddress, sizeof(stackAddress));
                        std::memcpy(sizeOutput, &attribute->second.stackSize,
                                    sizeof(attribute->second.stackSize));
                    }
                }
                hostRegisters[0] = static_cast<std::uint32_t>(result);
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 70) {
                const auto threadIdOutput = hostRegisters[0];
                auto* threadIdDestination = guestMemory.buffer(
                    threadIdOutput, sizeof(std::uint64_t), true);
                GuestThreadAttributes attributes;
                if (hostRegisters[1] != 0) {
                    const auto found = threadAttributes.find(hostRegisters[1]);
                    if (found == threadAttributes.end()) {
                        hostRegisters[0] = EINVAL;
                        std::copy_n(hostRegisters.begin(), 36,
                                    static_cast<std::uint64_t*>(stateBuffer.data()));
                        continue;
                    }
                    attributes = found->second;
                }
                if (attributes.stackAddress) {
                    const auto begin = *attributes.stackAddress;
                    const auto limit = static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max());
                    if (begin > limit || attributes.stackSize > limit - begin) {
                        hostRegisters[0] = EINVAL;
                        std::copy_n(hostRegisters.begin(), 36,
                                    static_cast<std::uint64_t*>(stateBuffer.data()));
                        continue;
                    }
                    try {
                        (void)guestMemory.buffer(begin, attributes.stackSize, true);
                    } catch (const std::out_of_range&) {
                        hostRegisters[0] = EINVAL;
                        std::copy_n(hostRegisters.begin(), 36,
                                    static_cast<std::uint64_t*>(stateBuffer.data()));
                        continue;
                    }
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
                const auto pageSize = bootstrap.pageSize;
                const auto threadDataSize = 16U + guestThreadLocalPrefixBytes + guestTSDSize;
                if (pageSize == 0 || (pageSize & (pageSize - 1)) != 0 ||
                    threadDataSize > std::numeric_limits<std::uint64_t>::max() -
                                         (pageSize - 1)) {
                    throw std::runtime_error("Guest pthread stack guard page size is invalid");
                }
                const auto guardOffset = (threadDataSize + pageSize - 1) & ~(pageSize - 1);
                const auto guardSize = attributes.stackAddress ? 0U
                    : attributes.guardSize > std::numeric_limits<std::uint64_t>::max() -
                          (pageSize - 1)
                        ? std::numeric_limits<std::uint64_t>::max()
                        : (attributes.guardSize + pageSize - 1) & ~(pageSize - 1);
                const auto stackStorageSize = attributes.stackAddress ? 0U
                    : attributes.stackSize > std::numeric_limits<std::uint64_t>::max() - 15U
                        ? std::numeric_limits<std::uint64_t>::max()
                        : (attributes.stackSize + 15U) & ~std::uint64_t{15U};
                if (guardSize == std::numeric_limits<std::uint64_t>::max() ||
                    stackStorageSize == std::numeric_limits<std::uint64_t>::max() ||
                    guardOffset > std::numeric_limits<std::uint64_t>::max() - guardSize ||
                    guardOffset + guardSize > std::numeric_limits<std::uint64_t>::max() -
                                                   stackStorageSize) {
                    throw std::length_error("Guest pthread stack allocation overflows");
                }
                const auto allocationSize = guardOffset + guardSize + stackStorageSize;
                const auto allocation = allocateGuestHeap(allocationSize, pageSize);
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
                    const auto guardBegin = allocationAddress + guardOffset;
                    const auto guardEnd = guardBegin + guardSize;
                    const auto storageStackStart = guardEnd;
                    const auto guestStackStart = attributes.stackAddress.value_or(
                        storageStackStart);
                    const auto guestStackSize = attributes.stackAddress
                        ? attributes.stackSize : stackStorageSize;
                    const auto guestStackEnd = guestStackStart + guestStackSize;
                    const auto storeThreadWord = [&](std::uint64_t offset, std::uint64_t value) {
                        std::memcpy(threadStorage + offset, &value, sizeof(value));
                    };
                    threadStackRanges.insert_or_assign(threadId, GuestMemoryRange{guestStackStart, guestStackEnd});
                    if (std::getenv("ARM64_METAL_TRACE_THREADS"))
                        std::cerr << "arm64-metal pthread-create thread=" << threadId << " stack=["
                                  << hexadecimal(guestStackStart) << "," << hexadecimal(guestStackEnd)
                                  << ") entry=0x" << hexadecimal(entry) << " arg=0x"
                                  << hexadecimal(hostRegisters[3]) << " raw-x2=0x"
                                  << hexadecimal(hostRegisters[2]) << " lr=0x" << hexadecimal(hostRegisters[30])
                                  << " attr=0x" << hexadecimal(hostRegisters[1])
                                  << " size=" << guestStackSize << " guard=" << guardSize
                                  << " custom=" << (attributes.stackAddress ? 1 : 0) << '\n';
                    const auto tsdOffset = 16U + guestThreadLocalPrefixBytes;
                    storeThreadWord(tsdOffset, threadId);
                    storeThreadWord(tsdOffset + 8U, allocationAddress);
                    storeThreadWord(tsdOffset + guestMachThreadSlotOffset,
                                    guestMachThreadName(threadId));
                    if (guardSize != 0) {
                        if (threadStackGuards.find(threadId) != threadStackGuards.end()) {
                            throw std::logic_error("Duplicate guest pthread stack guard");
                        }
                        threadStackGuards.emplace(threadId,
                            GuestMemoryRange{guardBegin, guardEnd});
                        try {
                            for (auto page = guardBegin; page < guardEnd; page += pageSize) {
                                if (protectedGuestPages.find(page) != protectedGuestPages.end()) {
                                    throw std::logic_error("Overlapping guest pthread guard page");
                                }
                                protectedGuestPages.emplace(page, 0U);
                                guestMemory.add_mapping({page, page + pageSize, page, false, false});
                            }
                            syncWritableTable(bootstrap.errnoSlot);
                        } catch (...) {
                            auto protectedPage = protectedGuestPages.lower_bound(guardBegin);
                            while (protectedPage != protectedGuestPages.end() &&
                                   protectedPage->first < guardEnd) {
                                guestMemory.remove_mapping(protectedPage->first);
                                protectedPage = protectedGuestPages.erase(protectedPage);
                            }
                            threadStackGuards.erase(threadId);
                            hostServiceState.heap->release(*allocation);
                            throw;
                        }
                    }
                    auto thread = GuestScheduler::make_thread_frame(
                        {static_cast<std::int64_t>(guestStackStart),
                         static_cast<std::int64_t>(guestStackEnd)},
                        *allocation, *allocation, static_cast<std::int64_t>(tsdAddress),
                        entry, hostRegisters[3], hostRegisters[guestSchedulerFPCR],
                        hostRegisters[guestSchedulerFPSR], hostServiceState.threadSignalMask);
                    thread.id = threadId;
                    thread.detached = attributes.detachState == PTHREAD_CREATE_DETACHED;
                    if (attributes.stackAddress) thread.registers[31] &= ~std::uint64_t{15U};
                    const auto addedId = guestScheduler.add_thread(std::move(thread));
                    if (addedId != threadId) {
                        throw std::logic_error("Guest scheduler assigned an unexpected thread id");
                    }
                    liveThreadStacks.emplace(threadId,
                        GuestMemoryRange{guestStackStart, guestStackEnd});
                    syncWritableTable(bootstrap.errnoSlot);
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
                    if (joined.reapedThread) {
                        liveThreadStacks.erase(joined.reapedThread->id);
                        releaseThreadStackGuard(joined.reapedThread->id);
                        if (joined.reapedThread->allocation) {
                            hostServiceState.heap->release(*joined.reapedThread->allocation);
                        }
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
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                if (beginImageExitDrain(takePendingDynamicUnloads(), std::nullopt,
                                        hostRegisters[0])) continue;
                if (finishGuestThread(hostRegisters[0], hostRegisters)) return 0;
                continue;
            }
            if (service == 174) {
                const auto detached = guestScheduler.detach(hostRegisters[0]);
                hostRegisters[0] = static_cast<std::uint32_t>(detached.error);
                if (detached.reapedThread) {
                    liveThreadStacks.erase(detached.reapedThread->id);
                    releaseThreadStackGuard(detached.reapedThread->id);
                    if (detached.reapedThread->allocation) {
                        hostServiceState.heap->release(*detached.reapedThread->allocation);
                    }
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
            if (service == 135 || service == 136 || service == 219) {
                if (service == 135 || service == 219) {
                    const bool facadeDlopenFrom = service == 219;
                    const auto mode = static_cast<std::int32_t>(
                        hostRegisters[facadeDlopenFrom ? 2U : 1U]);
                    const auto pathAddress = hostRegisters[facadeDlopenFrom ? 1U : 0U];
                    hostRegisters[0] = pathAddress;
                    if (pathAddress == 0) {
                        hostRegisters[0] = imageHandles.open_main(mode);
                        clearGuestDlerror();
                    } else {
                        const auto requestedPath = guest_cstring(guestMemory, pathAddress);
                        std::uint64_t callerHeader =
                            static_cast<std::uint64_t>(image.main_header);
                        const auto resume = hostRegisters[33];
                        const auto callSite = facadeDlopenFrom
                            ? hostRegisters[3] : (resume >= 4 ? resume - 4 : resume);
                        bool callerFound = !facadeDlopenFrom;
                        for (const auto& segment : image.mapped_image_segments) {
                            if (segment.start >= 0 && segment.end >= segment.start &&
                                static_cast<std::uint64_t>(segment.start) <= callSite &&
                                callSite < static_cast<std::uint64_t>(segment.end)) {
                                callerHeader = segment.guest_header;
                                callerFound = true;
                                break;
                            }
                        }
                        if (!callerFound) {
                            hostRegisters[0] = 0;
                            setGuestDlerror("dlopen: caller address is outside loaded images");
                        }
                        auto path = requestedPath;
                        constexpr std::string_view loaderPathPrefix = "@loader_path/";
                        if (hostRegisters[0] != 0 &&
                            path.rfind(loaderPathPrefix, 0) == 0) {
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
                                if (hostRegisters[0] != 0) {
                                    if (const auto loaded =
                                            imageHandles.loaded_header(hostRegisters[0]))
                                        ensureDynamicImageGroup(*loaded);
                                }
                                if (hostRegisters[0] == 0)
                                    setGuestDlerror("dlopen: image is not loaded: " + path);
                        } else if (const auto header = imageHandles.header_for_path(path);
                                   header && imageIsLoaded(*header)) {
                                const auto handle = imageHandles.open_loaded(path, mode);
                                hostRegisters[0] = *handle;
                                if (const auto loaded =
                                        imageHandles.loaded_header(hostRegisters[0]))
                                    ensureDynamicImageGroup(*loaded);
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
                                const auto firstNewImage = imageRegistry.size();
                                auto imageTransaction = imageRegistry.begin_transaction();
                                for (auto& stagedImage : stagedImageEnumerationOrder(plan)) {
                                    imageTransaction.add(stagedImage.header,
                                        std::move(stagedImage.name), stagedImage.cache_backed);
                                }
                                imageTransaction.commit();
                                auto imageNotifications = collectImageNotifications(
                                    firstNewImage, imageRegistry.size(), 0);
                                const auto bulkCallbacks = bulkImageCallbacks;
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
                                    const bool globallyVisible =
                                        staged.guest_header != plan.requested_header ||
                                        (mode & RTLD_LOCAL) == 0;
                                    const auto manifest = std::find_if(
                                        image.loaded_image_graph.begin(),
                                        image.loaded_image_graph.end(), [&staged](const auto& item) {
                                            return item.guest_header == staged.guest_header;
                                        });
                                    if (manifest != image.loaded_image_graph.end() &&
                                        manifest->dynamic_unload_candidate) {
                                        manifest->global_scope = globallyVisible;
                                    }
                                    if (!globallyVisible) continue;
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
                                ensureDynamicImageGroup(plan.requested_header);
                                clearGuestDlerror();
                                if (!initializers.empty() || !imageNotifications.empty() ||
                                    (!bulkCallbacks.empty() &&
                                     imageRegistry.size() > firstNewImage)) {
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
                                    frame.imageNotifications = std::move(imageNotifications);
                                    if (!bulkCallbacks.empty() &&
                                        imageRegistry.size() > firstNewImage) {
                                        prepareBulkImageCallback(
                                            frame, hostRegisters, bulkCallbacks, firstNewImage,
                                            imageRegistry.size(), false);
                                    }
                                    const bool hasImageNotifications =
                                        !frame.imageNotifications.empty();
                                    const bool hasBulkNotifications =
                                        !frame.bulkImageCallbacks.empty();
                                    if (hasImageNotifications) {
                                        frame.nextImageNotification = 1;
                                    } else if (!hasBulkNotifications) {
                                        frame.nextInitializer = 1;
                                    } else {
                                        frame.nextBulkImageCallback = 1;
                                    }
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
                                    if (hasImageNotifications) {
                                        const auto& notification =
                                            pending.imageNotifications.front();
                                        std::copy(notification.arguments.begin(),
                                                  notification.arguments.end(),
                                                  callbackRegisters);
                                        callbackRegisters[33] = notification.callback;
                                    } else if (hasBulkNotifications) {
                                        std::copy(pending.arguments.front().begin(),
                                                  pending.arguments.front().end(),
                                                  callbackRegisters);
                                        callbackRegisters[33] =
                                            pending.bulkImageCallbacks.front();
                                    } else {
                                        const auto first =
                                            pending.initializerTargets.front();
                                        std::copy_n(bootstrap.registers.begin(), 5,
                                                    callbackRegisters);
                                        callbackRegisters[33] = first;
                                    }
                                    callbackRegisters[30] = guestReturnSentinel;
                                    selectKernel(static_cast<std::int64_t>(
                                        callbackRegisters[33]));
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
                            if (const auto header = imageHandles.loaded_header(hostRegisters[0])) {
                                const auto manifest = std::find_if(
                                    image.loaded_image_graph.begin(),
                                    image.loaded_image_graph.end(), [header](const auto& item) {
                                        return item.guest_header == *header;
                                    });
                                if (manifest != image.loaded_image_graph.end() &&
                                    manifest->dynamic_unload_candidate) {
                                    manifest->global_scope = true;
                                }
                            }
                        }
                        if (hostRegisters[0] != 0) clearGuestDlerror();
                        else if ((mode & RTLD_NOLOAD) != 0) {
                            setGuestDlerror("dlopen: image is not loaded: " + path);
                        }
                    }
                } else {
                    const auto header = imageHandles.loaded_header(hostRegisters[0]);
                    hostRegisters[0] = imageHandles.close(hostRegisters[0]);
                    std::vector<std::uint64_t> finalizableImages;
                    if (hostRegisters[0] == 0 && header &&
                        !imageHandles.has_open_reference_for_image(*header)) {
                        finalizableImages = releaseDynamicImageGroup(*header);
                    }
                    std::copy_n(hostRegisters.begin(), 36,
                                static_cast<std::uint64_t*>(stateBuffer.data()));
                    if (beginImageExitDrain(std::move(finalizableImages), std::nullopt,
                                            std::nullopt)) continue;
                    continue;
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
            if (service == 196) {
                const auto thread = hostRegisters[0];
                const auto outputAddress = hostRegisters[1];
                if (outputAddress == 0) {
                    hostRegisters[0] = EINVAL;
                } else if (thread != 0 && guestScheduler.find(thread) == nullptr) {
                    hostRegisters[0] = ESRCH;
                } else {
                    const auto threadId = thread == 0 ? guestScheduler.current_id() : thread;
                    auto* output = guestMemory.buffer(outputAddress, sizeof(threadId), true);
                    std::memcpy(output, &threadId, sizeof(threadId));
                    hostRegisters[0] = 0;
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 202) {
                const auto threadId = hostRegisters[0];
                const auto signal = static_cast<std::int32_t>(hostRegisters[1]);
                auto* thread = guestScheduler.find(threadId);
                if (thread == nullptr) {
                    hostRegisters[0] = ESRCH;
                } else if (signal != 0 && !GuestSignals::valid(signal)) {
                    hostRegisters[0] = EINVAL;
                } else {
                    if (signal != 0 &&
                        hostServiceState.signals.action(signal).handler != 1) {
                        thread->pendingSignals |= GuestSignals::bit(signal);
                    }
                    hostRegisters[0] = 0;
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 193) {
                hostRegisters[0] = acquireGuestFixedSendRight(
                    static_cast<std::uint32_t>(guestMachHostPort));
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 194) {
                hostRegisters[0] = acquireGuestThreadSendRight(guestScheduler.current_id());
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
                if (signalRuntime.has_frames(guestScheduler.current_id())) {
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
                ensureTokenCapacity(1, "Objective-C image callback");
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
                live[30] = guestReturnSentinel;
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
                if (signalRuntime.has_frames(guestScheduler.current_id())) {
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
                current[30] = guestReturnSentinel;
                current[33] = callback;
                ++callbackFrames.back().nextArgument;
                selectKernel(static_cast<std::int64_t>(callback));
                continue;
            }
            if (service == 185) {
                const auto build = guest_image_build_version(hostRegisters[0], image, guestMemory);
                const auto result = build ? guest_version_token_at_least(
                    (static_cast<std::uint64_t>(build->sdk) << 32U) | build->platform,
                    hostRegisters[1]) : 0;
                const char* sdkTrace = std::getenv("ARM64_METAL_TRACE_SERVICE185");
                if (sdkTrace != nullptr && std::strcmp(sdkTrace, "1") == 0) {
                    const auto requestedPlatform = static_cast<std::uint32_t>(hostRegisters[1]);
                    const auto requestedVersion = static_cast<std::uint32_t>(hostRegisters[1] >> 32U);
                    const auto mapped = image.image_paths.find(hostRegisters[0]);
                    const char* category = mapped == image.image_paths.end()
                        ? "unmapped" : build ? "mapped-with-version" : "mapped-no-version";
                    std::cerr << "arm64-metal dyld-sdk service185 platform=" << requestedPlatform
                              << " version=" << requestedVersion << " header=" << category
                              << " result=" << (result > 0 ? 1 : 0) << '\n';
                }
                hostRegisters[0] = static_cast<std::uint64_t>(result > 0);
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service >= 189 && service <= 192) {
                if (service == 189 || service == 190) {
                    const bool sdkToken = service == 189;
                    hostRegisters[0] = image.build_version
                        ? image.build_version->token(sdkToken) : 0;
                } else if (service == 191) {
                    hostRegisters[0] = static_cast<std::uint32_t>(hostRegisters[0]);
                } else {
                    const auto result = guest_version_token_at_least(
                        hostRegisters[0], hostRegisters[1]);
                    if (result < 0) {
                        throw std::runtime_error("Darwin version-set comparison is unavailable");
                    }
                    hostRegisters[0] = static_cast<std::uint64_t>(result);
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
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
            if (service == 123) {
                const auto callbackIndex = addImageCallbacks.size();
                addImageCallbacks.push_back(hostRegisters[0]);
                auto notifications = collectImageNotifications(
                    0, imageRegistry.size(), callbackIndex);
                if (notifications.empty()) {
                    std::copy_n(hostRegisters.begin(), 36,
                                static_cast<std::uint64_t*>(stateBuffer.data()));
                    continue;
                }
                GuestCallbackFrame frame;
                frame.savedRegisters.assign(hostRegisters.begin(), hostRegisters.begin() + 36);
                frame.savedRegisters[34] = 0;
                const auto* currentState =
                    static_cast<const std::uint64_t*>(stateBuffer.data());
                frame.savedVectorAndAuth.assign(currentState + 806,
                                                currentState + guestTokenCount);
                frame.authenticationDepth = hostRegisters[37];
                frame.imageNotifications = std::move(notifications);
                frame.nextImageNotification = 1;
                callbackFrames.push_back(std::move(frame));
                auto* callbackRegisters = static_cast<std::uint64_t*>(stateBuffer.data());
                const auto& pending = callbackFrames.back();
                std::copy(pending.savedRegisters.begin(), pending.savedRegisters.end(),
                          callbackRegisters);
                std::copy(pending.savedVectorAndAuth.begin(),
                          pending.savedVectorAndAuth.end(), callbackRegisters + 806);
                callbackRegisters[37] = pending.authenticationDepth;
                const auto& first = pending.imageNotifications.front();
                std::copy(first.arguments.begin(), first.arguments.end(), callbackRegisters);
                callbackRegisters[30] = guestReturnSentinel;
                callbackRegisters[33] = first.callback;
                selectKernel(static_cast<std::int64_t>(first.callback));
                continue;
            }
            if (service == 186) {
                if (imageRegistry.size() > std::numeric_limits<std::uint32_t>::max()) {
                    throw std::runtime_error("Guest dyld image count exceeds its ABI width");
                }
                hostRegisters[0] = imageRegistry.size();
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 187 || service == 188) {
                const auto index = hostRegisters[0];
                if (service == 187) {
                    const auto header = imageRegistry.header_at(index);
                    if (header && imageRegistry.cache_backed_at(index)) {
                        const auto* cache = image.metadata_cache_reader();
                        if (cache == nullptr) {
                            throw std::runtime_error(
                                "Guest cached image header has no shared-cache reader");
                        }
                        const auto headerBytes = cache->bytes(cache->originalAddress(
                            static_cast<std::int64_t>(*header)), 32);
                        const auto read32 = [&headerBytes](std::size_t offset) {
                            std::uint32_t value = 0;
                            for (unsigned byte = 0; byte < 4; ++byte) {
                                value |= static_cast<std::uint32_t>(headerBytes[offset + byte])
                                         << (byte * 8U);
                            }
                            return value;
                        };
                        const auto commandCount = read32(16);
                        const auto commandBytes = read32(20);
                        if (commandBytes > 1024U * 1024U || commandCount > commandBytes / 8U) {
                            throw std::runtime_error("Invalid cached Mach-O header metadata");
                        }
                        publishCacheMetadata(*cache, static_cast<std::int64_t>(*header),
                                             32U + commandBytes, false);
                    }
                    hostRegisters[0] = header.value_or(0);
                } else {
                    const auto name = imageRegistry.name_at(index);
                    if (name) {
                        const auto address = stableGuestString(std::string(*name));
                        hostRegisters[0] = address.value_or(0);
                    } else {
                        hostRegisters[0] = 0;
                    }
                }
                std::copy_n(hostRegisters.begin(), 36,
                            static_cast<std::uint64_t*>(stateBuffer.data()));
                continue;
            }
            if (service == 195) {
                hostRegisters[0] = guest_image_vmaddr_slide(
                    imageRegistry, hostRegisters[0], image.image_slides);
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
            HostServiceResult result;
            try {
                result = dispatch_guest_host_service(hostRegisters, guestMemory, bootstrap,
                                                     hostServiceState, service);
            } catch (const std::exception& error) {
                dumpTransitions();
                throw std::runtime_error(std::string(error.what()) + " (host service " +
                                         std::to_string(service) + " " + lastHostTransition + ")");
            }
            std::copy_n(hostRegisters.begin(), 36,
                        static_cast<std::uint64_t*>(stateBuffer.data()));
            if (result.exited) return result.exitStatus;
            if (hostRegisters[33] == guestReturnSentinel && callbackFrames.empty()) {
                if (signalRuntime.has_frames(guestScheduler.current_id()) && resumeSignalFrame()) continue;
                if (advanceStartup()) continue;
            }
            continue;
        }
        if (status == 7) {
            const auto pc = registers[33];
            if (pc > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                dumpTransitions();
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
                        (width != 1 && width != 2 && width != 4 &&
                         width != 8 && width != 16) ||
                        faultPacket == activeKernel->interpretedPackets.end()) {
                        throw std::runtime_error(
                            "Interpreter cache retry has invalid fault provenance");
                    }
                    const auto opcode = static_cast<translate_core::Opcode>(
                        faultPacket->operation[0]);
                    const bool pairAccess =
                        opcode == translate_core::Opcode::LoadPair ||
                        opcode == translate_core::Opcode::StorePair;
                    const bool scalarAccess =
                        opcode == translate_core::Opcode::LoadUnsignedImmediate ||
                        opcode == translate_core::Opcode::StoreUnsignedImmediate ||
                        opcode == translate_core::Opcode::LoadUnscaledWriteback ||
                        opcode == translate_core::Opcode::StoreUnscaledWriteback ||
                        opcode == translate_core::Opcode::LoadSignedScalar ||
                        opcode == translate_core::Opcode::LoadUnsignedMinMax;
                    const bool literalAccess =
                        opcode == translate_core::Opcode::LoadLiteral;
                    const bool vectorAccess =
                        opcode == translate_core::Opcode::LoadVectorMemory ||
                        opcode == translate_core::Opcode::StoreVectorMemory;
                    const auto expectedWidth = literalAccess
                        ? (faultPacket->operation[4] == 1U ? 8U : 4U)
                        : faultPacket->operation[5] / 8U * (pairAccess ? 2U : 1U);
                    const auto accessWidth = literalAccess
                        ? expectedWidth : faultPacket->operation[5] / 8U;
                    const bool supportedAccessWidth = vectorAccess
                        ? (accessWidth == 1U || accessWidth == 2U ||
                           accessWidth == 4U || accessWidth == 8U ||
                           accessWidth == 16U)
                        : (accessWidth == 1U || accessWidth == 2U ||
                           accessWidth == 4U || accessWidth == 8U);
                    if ((!scalarAccess && !pairAccess && !literalAccess &&
                         !vectorAccess) ||
                        (!literalAccess && faultPacket->operation[5] % 8U != 0) ||
                        (!literalAccess && !supportedAccessWidth) ||
                        width != expectedWidth) {
                        throw std::runtime_error(
                            "Interpreter cache retry does not match its memory packet");
                    }
                    progressTrace.event("interpreter-cache-fault", faultPc, requested);
                    progressTrace.event("interpreter-cache-fault-width", faultPc, width);
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
                static_cast<std::uint64_t>(activeEntry), guestScheduler.current_id());
            if (delivery.outcome == GuestSignalOutcome::ProcessExit) {
                if (hostServiceState.signals.action(fault.signal).handler == 0) {
                    std::cerr << "arm64-metal: Guest process memory access fault at 0x"
                              << hexadecimal(address) << " at PC 0x"
                              << hexadecimal(registers[33]) << " width "
                              << registers[guestCacheFaultWidth] << " x10=0x"
                              << hexadecimal(registers[10]) << " tp=0x"
                              << hexadecimal(registers[guestThreadPointer]) << " sp=0x"
                              << hexadecimal(registers[31]) << " thread=" << guestScheduler.current_id();
                    const auto range = threadStackRanges.find(guestScheduler.current_id());
                    if (range != threadStackRanges.end())
                        std::cerr << " stack=[0x" << hexadecimal(range->second.begin) << ",0x"
                                  << hexadecimal(range->second.end) << ")";
                    if (std::getenv("ARM64_METAL_TRACE_THREADS"))
                        std::cerr << " mem " << guestMemory.describe_near(address, 0x40000);
                    if (std::getenv("ARM64_METAL_TRACE_THREADS")) {
                        for (int r = 0; r < 32; ++r)
                            std::cerr << " x" << r << "=0x" << hexadecimal(registers[r]);
                    }
                    std::cerr << '\n';
                    if (std::getenv("ARM64_METAL_TRACE_FATAL_SIGNAL") != nullptr) {
                        for (std::size_t r = 0; r < 31; ++r)
                            std::cerr << " x" << r << "=0x" << hexadecimal(registers[r]);
                        std::cerr << '\n';
                        dumpTransitions();
                    }
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
        if (status == 3 || status == 14 || status == 15) {
            const auto address = status == 3 ? registers[guestFaultAddress] : registers[33];
            const auto signal = status == 3 ? SIGSEGV : status == 14 ? SIGILL : SIGTRAP;
            const auto signalCode = status == 3 ? SEGV_MAPERR :
                status == 14 ? ILL_ILLTRP : 0;
            const GuestSynchronousFault fault{
                signal, signalCode, address, registers[33]};
            std::vector<std::uint64_t> live(
                registers, registers + stateBuffer.size() / sizeof(std::uint64_t));
            const auto delivery = signalRuntime.deliver_synchronous(
                hostServiceState.signals, hostServiceState.threadSignalMask,
                live, guestMemory, fault, live[37],
                static_cast<std::uint64_t>(activeEntry), guestScheduler.current_id());
            if (delivery.outcome == GuestSignalOutcome::ProcessExit) {
                if (status == 3 &&
                    hostServiceState.signals.action(fault.signal).handler == 0) {
                    std::cerr << "arm64-metal: Guest process reached an invalid code address\n";
                } else if (status != 3) {
                    std::cerr << "arm64-metal: Guest process terminated by "
                              << (status == 14 ? "SIGILL" : "SIGTRAP") << " at PC 0x"
                              << hexadecimal(registers[33]) << " LR 0x"
                              << hexadecimal(registers[30]) << '\n';
                    if (std::getenv("ARM64_METAL_TRACE_FATAL_SIGNAL") != nullptr) {
                        std::uint64_t best = 0;
                        std::string bestName;
                        for (std::size_t i = 0; i < imageRegistry.size(); ++i) {
                            const auto header = *imageRegistry.header_at(i);
                            if (header <= registers[33] && header >= best) {
                                best = header;
                                bestName = std::string(*imageRegistry.name_at(i));
                            }
                        }
                        std::cerr << "arm64-metal fatal-signal image=0x" << hexadecimal(best)
                                  << " " << bestName << '\n';
                        for (std::size_t r = 0; r < 31; ++r)
                            std::cerr << " x" << r << "=0x" << hexadecimal(registers[r]);
                        std::cerr << '\n';
                        const auto count = std::min(transitionHistoryCount,
                                                    transitionHistory.size());
                        for (std::size_t i = transitionHistoryCount - count;
                             i < transitionHistoryCount; ++i)
                            std::cerr << "  history " << transitionHistory[i % transitionHistory.size()]
                                      << '\n';
                    }
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
        if (status == 16) {
            const GuestSynchronousFault fault{
                SIGBUS, BUS_ADRALN, 0, registers[33], 0x72000001};
            progressTrace.event("guest-authentication-fault", fault.programCounter,
                                fault.exceptionSyndrome);
            std::vector<std::uint64_t> live(
                registers, registers + stateBuffer.size() / sizeof(std::uint64_t));
            const auto delivery = signalRuntime.deliver_synchronous(
                hostServiceState.signals, hostServiceState.threadSignalMask,
                live, guestMemory, fault, live[37],
                static_cast<std::uint64_t>(activeEntry), guestScheduler.current_id());
            if (delivery.outcome == GuestSignalOutcome::ProcessExit) {
                return delivery.exitStatus;
            }
            if (delivery.outcome == GuestSignalOutcome::HandlerEntered) {
                std::memcpy(stateBuffer.data(), live.data(), live.size() * sizeof(std::uint64_t));
                selectKernel(static_cast<std::int64_t>(delivery.handler));
                continue;
            }
            throw std::runtime_error("Guest authentication fault signal was not delivered");
        }
        if (status == 9 || status == 10 || status == 12) {
            if (std::getenv("ARM64_METAL_TRACE_FATAL_SIGNAL") != nullptr) {
                dumpTransitions();
                dumpFrames(registers, 36);
            }
            throw std::runtime_error(pac_failure_message(registers,
                stateBuffer.size() / sizeof(std::uint64_t), status));
        }
        if (status == 13) {
            const auto syscall = registers[16];
            if (fireDueMachTimers) fireDueMachTimers();
            lastHostTransition = "syscall=" + std::to_string(syscall) + "@pc=0x" +
                hexadecimal(registers[33]);
            recordTransition(lastHostTransition + " x0=0x" + hexadecimal(registers[0]) +
                " x1=0x" + hexadecimal(registers[1]) + " x2=0x" + hexadecimal(registers[2]) +
                " x3=0x" + hexadecimal(registers[3]) + " x4=0x" + hexadecimal(registers[4]) +
                " x5=0x" + hexadecimal(registers[5]));
            progressTrace.event("raw-syscall", registers[33], syscall);
            const auto survey = [&](std::string_view kind, std::int64_t num, std::string_view sub,
                                    std::string_view detail = {}) {
                guest_survey::note(kind, num, sub, registers[33], registers[30],
                                   guestScheduler.current_id(), detail);
            };
            if (guest_survey::boundary_enabled()) {
                const auto signedSyscall = static_cast<std::int64_t>(syscall);
                guest_survey::boundary(signedSyscall < 0 ? "mach_trap" : "syscall",
                    signedSyscall < 0 ? -signedSyscall : signedSyscall,
                    signedSyscall == -47 ? std::to_string(registers[4] >> 32) : std::string(),
                    registers, 8, registers[33], registers[30], guestScheduler.current_id());
            }
            // Raw BSD stubs return the error in x0 with carry set. The guest's
            // cerror path does not populate errno from it here, so also store it
            // in the current thread's errno slot as the host-service paths do.
            const auto rawFailQuiet = [&](int error) {
                const auto value = static_cast<std::uint32_t>(error);
                std::memcpy(guestMemory.buffer(bootstrap.errnoSlot, sizeof(value), true),
                            &value, sizeof(value));
                registers[0] = static_cast<std::uint64_t>(error);
                registers[32] |= 8U;
                registers[34] = 0;
            };
            const auto rawFail = [&](int error) {
                if (error == ENOTSUP || error == ENOSYS) {
                    survey("errno_stub", static_cast<std::int64_t>(syscall),
                           error == ENOSYS ? "ENOSYS" : "ENOTSUP");
                }
                rawFailQuiet(error);
            };
            const auto rawOk = [&](std::uint64_t result) {
                registers[0] = result;
                registers[32] &= ~std::uint64_t{8};
                registers[34] = 0;
            };
            const auto rawPath = [&](std::uint64_t address) -> std::optional<std::string> {
                try {
                    return guest_cstring(guestMemory, address);
                } catch (const std::exception&) {
                    return std::nullopt;
                }
            };
            if (syscall == 12) {  // chdir
                const auto path = rawPath(registers[0]);
                if (!path) rawFail(EFAULT);
                else if (::chdir(path->c_str()) == 0) rawOk(0);
                else rawFail(errno);
                continue;
            }
            const auto rawOwnsDescriptor = [&](std::uint64_t value) {
                const auto descriptor = static_cast<std::int32_t>(value);
                return hostServiceState.openDescriptors.count(descriptor) != 0 ||
                       hostServiceState.guestDirectoryDescriptors.count(descriptor) != 0;
            };
            if (syscall == 13) {  // fchdir
                if (!rawOwnsDescriptor(registers[0])) rawFail(EBADF);
                else if (::fchdir(static_cast<int>(registers[0])) == 0) rawOk(0);
                else rawFail(errno);
                continue;
            }
            if (syscall == 34) {  // chflags
                const auto path = rawPath(registers[0]);
                if (!path) rawFail(EFAULT);
                else if (::chflags(path->c_str(),
                                   static_cast<std::uint32_t>(registers[1])) == 0) rawOk(0);
                else rawFail(errno);
                continue;
            }
            if (syscall == 35) {  // fchflags
                if (!rawOwnsDescriptor(registers[0])) rawFail(EBADF);
                else if (::fchflags(static_cast<int>(registers[0]),
                                    static_cast<std::uint32_t>(registers[1])) == 0) rawOk(0);
                else rawFail(errno);
                continue;
            }
            if (syscall == 463 || syscall == 464) {  // openat, openat_nocancel
                constexpr std::int32_t allowed = O_ACCMODE | O_APPEND | O_CREAT | O_TRUNC |
                    O_EXCL | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_DIRECTORY |
                    O_EXEC | O_NOFOLLOW_ANY | O_SYMLINK;
                const auto directory = static_cast<std::int32_t>(registers[0]);
                const auto flags = static_cast<std::int32_t>(registers[2]);
                const auto path = rawPath(registers[1]);
                if (!path || registers[1] == 0) {
                    rawFail(EFAULT);
                } else if (!(path->front() == '/' || directory == AT_FDCWD ||
                             rawOwnsDescriptor(registers[0]))) {
                    rawFail(EBADF);
                } else if ((flags & ~allowed) != 0) {
                    rawFail(EINVAL);
                } else {
                    const auto descriptor = (flags & O_CREAT) != 0
                        ? ::openat(directory, path->c_str(), flags | O_CLOEXEC,
                                   static_cast<mode_t>(registers[3]))
                        : ::openat(directory, path->c_str(), flags | O_CLOEXEC);
                    if (descriptor < 0) {
                        rawFail(errno);
                    } else {
                        hostServiceState.openDescriptors.insert(descriptor);
                        if ((flags & O_ACCMODE) != O_RDONLY) {
                            hostServiceState.writableDescriptors.insert(descriptor);
                        }
                        rawOk(static_cast<std::uint64_t>(descriptor));
                    }
                }
                continue;
            }
            if (syscall == 345 || syscall == 346) {  // statfs64, fstatfs64
                struct statfs result{};
                int status;
                if (syscall == 345) {
                    const auto path = rawPath(registers[0]);
                    status = path ? ::statfs(path->c_str(), &result) : -2;
                    if (!path) {
                        rawFail(EFAULT);
                        continue;
                    }
                } else if (!rawOwnsDescriptor(registers[0]) &&
                           static_cast<std::int32_t>(registers[0]) > STDERR_FILENO) {
                    rawFail(EBADF);
                    continue;
                } else {
                    status = ::fstatfs(static_cast<int>(registers[0]), &result);
                }
                if (status != 0) {
                    rawFail(errno);
                } else {
                    try {
                        std::memcpy(guestMemory.buffer(registers[1], sizeof(result), true),
                                    &result, sizeof(result));
                        rawOk(0);
                    } catch (const std::exception&) {
                        rawFail(EFAULT);
                    }
                }
                continue;
            }
            if (syscall == 341 || syscall == 342 || syscall == 343) {
                // stat64_extended, lstat64_extended, fstat64_extended: the kernel
                // fills the stat64 buffer and, when asked, an ACL blob and its size.
                const bool byDescriptor = syscall == 343;
                std::optional<std::string> path;
                if (byDescriptor) {
                    if (!rawOwnsDescriptor(registers[0]) &&
                        static_cast<std::int32_t>(registers[0]) > STDERR_FILENO) {
                        rawFail(EBADF);
                        continue;
                    }
                } else {
                    path = rawPath(registers[0]);
                    if (!path) {
                        rawFail(EFAULT);
                        continue;
                    }
                }
                try {
                    struct stat result{};
                    std::size_t guestSecuritySize = 0;
                    if (registers[2] != 0 && registers[3] != 0) {
                        std::memcpy(&guestSecuritySize,
                                    guestMemory.buffer(registers[3], sizeof(guestSecuritySize)),
                                    sizeof(guestSecuritySize));
                    }
                    std::vector<std::uint8_t> security(
                        registers[2] != 0 ? guestSecuritySize : 0);
                    std::size_t hostSecuritySize = guestSecuritySize;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                    const auto status = ::syscall(
                        static_cast<int>(syscall),
                        byDescriptor ? reinterpret_cast<void*>(registers[0])
                                     : const_cast<char*>(path->c_str()),
                        &result, registers[2] != 0 ? security.data() : nullptr,
                        registers[3] != 0 ? &hostSecuritySize : nullptr);
#pragma clang diagnostic pop
                    if (status != 0) {
                        rawFail(errno);
                        continue;
                    }
                    std::memcpy(guestMemory.buffer(registers[1], sizeof(result), true),
                                &result, sizeof(result));
                    if (registers[3] != 0) {
                        std::memcpy(guestMemory.buffer(registers[3], sizeof(hostSecuritySize), true),
                                    &hostSecuritySize, sizeof(hostSecuritySize));
                    }
                    if (registers[2] != 0 && hostSecuritySize != 0 &&
                        hostSecuritySize <= security.size()) {
                        std::memcpy(guestMemory.buffer(registers[2], hostSecuritySize, true),
                                    security.data(), hostSecuritySize);
                    }
                    rawOk(0);
                } catch (const std::exception&) {
                    rawFail(EFAULT);
                }
                continue;
            }
            if (syscall == 282 || syscall == 283) {  // chmod_extended, fchmod_extended
                // The ACL argument is a kauth_filesec: 36-byte header, entry count,
                // flags, then 24-byte entries. NULL leaves the ACL alone and -1 removes it.
                constexpr std::size_t headerBytes = 44;
                constexpr std::size_t entryBytes = 24;
                constexpr std::uint32_t maximumEntries = 128;
                const bool byDescriptor = syscall == 283;
                std::optional<std::string> path;
                if (byDescriptor) {
                    if (!rawOwnsDescriptor(registers[0])) {
                        rawFail(EBADF);
                        continue;
                    }
                } else {
                    path = rawPath(registers[0]);
                    if (!path) {
                        rawFail(EFAULT);
                        continue;
                    }
                }
                try {
                    std::vector<std::uint8_t> security;
                    void* securityArgument = reinterpret_cast<void*>(registers[4]);
                    if (registers[4] != 0 && registers[4] != ~std::uint64_t{0}) {
                        security.resize(headerBytes);
                        std::memcpy(security.data(),
                                    guestMemory.buffer(registers[4], headerBytes), headerBytes);
                        std::uint32_t entries = 0;
                        std::memcpy(&entries, security.data() + 36, sizeof(entries));
                        if (entries > maximumEntries && entries != ~std::uint32_t{0}) {
                            rawFail(EINVAL);
                            continue;
                        }
                        if (entries != ~std::uint32_t{0} && entries != 0) {
                            security.resize(headerBytes + entries * entryBytes);
                            std::memcpy(security.data() + headerBytes,
                                        guestMemory.buffer(registers[4] + headerBytes,
                                                           entries * entryBytes),
                                        entries * entryBytes);
                        }
                        securityArgument = security.data();
                    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                    const auto status = ::syscall(
                        static_cast<int>(syscall),
                        byDescriptor ? reinterpret_cast<void*>(registers[0])
                                     : const_cast<char*>(path->c_str()),
                        static_cast<std::uint32_t>(registers[1]),
                        static_cast<std::uint32_t>(registers[2]),
                        static_cast<std::int32_t>(registers[3]), securityArgument);
#pragma clang diagnostic pop
                    if (status != 0) rawFail(errno);
                    else rawOk(0);
                } catch (const std::exception&) {
                    rawFail(EFAULT);
                }
                continue;
            }
            if (syscall >= 234 && syscall <= 241) {
                // getxattr, fgetxattr, setxattr, fsetxattr, removexattr, fremovexattr,
                // listxattr, flistxattr.
                const auto index = static_cast<int>(syscall - 234);
                const bool byDescriptor = (index & 1) != 0;
                const int operation = index / 2;  // 0 get, 1 set, 2 remove, 3 list
                std::optional<std::string> path;
                if (byDescriptor) {
                    if (!rawOwnsDescriptor(registers[0])) {
                        rawFail(EBADF);
                        continue;
                    }
                } else {
                    path = rawPath(registers[0]);
                    if (!path) {
                        rawFail(EFAULT);
                        continue;
                    }
                }
                const auto descriptor = static_cast<int>(registers[0]);
                try {
                    std::optional<std::string> name;
                    if (operation != 3) {
                        name = rawPath(registers[1]);
                        if (!name) {
                            rawFail(EFAULT);
                            continue;
                        }
                    }
                    ssize_t result = -1;
                    if (operation == 0) {
                        const auto size = static_cast<std::size_t>(registers[3]);
                        const auto position = static_cast<std::uint32_t>(registers[4]);
                        const auto options = static_cast<int>(registers[5]);
                        std::vector<std::uint8_t> value(registers[2] != 0 ? size : 0);
                        result = byDescriptor
                            ? ::fgetxattr(descriptor, name->c_str(),
                                          registers[2] != 0 ? value.data() : nullptr,
                                          value.size(), position, options)
                            : ::getxattr(path->c_str(), name->c_str(),
                                         registers[2] != 0 ? value.data() : nullptr,
                                         value.size(), position, options);
                        if (result >= 0 && registers[2] != 0 && result > 0) {
                            std::memcpy(guestMemory.buffer(registers[2],
                                            static_cast<std::size_t>(result), true),
                                        value.data(), static_cast<std::size_t>(result));
                        }
                    } else if (operation == 1) {
                        const auto size = static_cast<std::size_t>(registers[3]);
                        const auto position = static_cast<std::uint32_t>(registers[4]);
                        const auto options = static_cast<int>(registers[5]);
                        std::vector<std::uint8_t> value(size);
                        if (size != 0) {
                            std::memcpy(value.data(), guestMemory.buffer(registers[2], size), size);
                        }
                        result = byDescriptor
                            ? ::fsetxattr(descriptor, name->c_str(), value.data(), size,
                                          position, options)
                            : ::setxattr(path->c_str(), name->c_str(), value.data(), size,
                                         position, options);
                    } else if (operation == 2) {
                        const auto options = static_cast<int>(registers[2]);
                        result = byDescriptor
                            ? ::fremovexattr(descriptor, name->c_str(), options)
                            : ::removexattr(path->c_str(), name->c_str(), options);
                    } else {
                        const auto size = static_cast<std::size_t>(registers[2]);
                        const auto options = static_cast<int>(registers[3]);
                        std::vector<char> names(registers[1] != 0 ? size : 0);
                        result = byDescriptor
                            ? ::flistxattr(descriptor, registers[1] != 0 ? names.data() : nullptr,
                                           names.size(), options)
                            : ::listxattr(path->c_str(), registers[1] != 0 ? names.data() : nullptr,
                                          names.size(), options);
                        if (result > 0 && registers[1] != 0) {
                            std::memcpy(guestMemory.buffer(registers[1],
                                            static_cast<std::size_t>(result), true),
                                        names.data(), static_cast<std::size_t>(result));
                        }
                    }
                    if (result < 0) rawFail(errno);
                    else rawOk(static_cast<std::uint64_t>(result));
                } catch (const std::exception&) {
                    rawFail(EFAULT);
                }
                continue;
            }
            if (syscall == 216) {  // open_dprotected_np
                constexpr std::int32_t allowed = O_ACCMODE | O_APPEND | O_CREAT | O_TRUNC |
                    O_EXCL | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_DIRECTORY |
                    O_EXEC | O_NOFOLLOW_ANY | O_SYMLINK;
                const auto flags = static_cast<std::int32_t>(registers[1]);
                const auto path = rawPath(registers[0]);
                if (!path || registers[0] == 0) {
                    rawFail(EFAULT);
                } else if ((flags & ~allowed) != 0) {
                    rawFail(EINVAL);
                } else {
                    const auto descriptor = ::open_dprotected_np(
                        path->c_str(), flags | O_CLOEXEC, static_cast<int>(registers[2]),
                        static_cast<int>(registers[3]), static_cast<int>(registers[4]));
                    if (descriptor < 0) {
                        rawFail(errno);
                    } else {
                        hostServiceState.openDescriptors.insert(descriptor);
                        if ((flags & O_ACCMODE) != O_RDONLY) {
                            hostServiceState.writableDescriptors.insert(descriptor);
                        }
                        rawOk(static_cast<std::uint64_t>(descriptor));
                    }
                }
                continue;
            }
            if (syscall == 153 || syscall == 154) {  // pread, pwrite
                const auto descriptor = static_cast<std::int32_t>(registers[0]);
                const auto count = static_cast<std::size_t>(registers[2]);
                if (!rawOwnsDescriptor(registers[0])) {
                    rawFail(EBADF);
                } else if (syscall == 154 &&
                           hostServiceState.writableDescriptors.count(descriptor) == 0) {
                    rawFail(EBADF);
                } else if (count > 0x7fffffffU) {
                    rawFail(EINVAL);
                } else {
                    try {
                        std::vector<std::uint8_t> bytes(count);
                        ssize_t transferred;
                        if (syscall == 153) {
                            transferred = ::pread(descriptor, bytes.data(), count,
                                                  static_cast<off_t>(registers[3]));
                            if (transferred > 0) {
                                std::memcpy(guestMemory.buffer(registers[1],
                                                static_cast<std::size_t>(transferred), true),
                                            bytes.data(), static_cast<std::size_t>(transferred));
                            }
                        } else {
                            if (count != 0) {
                                std::memcpy(bytes.data(), guestMemory.buffer(registers[1], count),
                                            count);
                            }
                            transferred = ::pwrite(descriptor, bytes.data(), count,
                                                   static_cast<off_t>(registers[3]));
                        }
                        if (transferred < 0) rawFail(errno);
                        else rawOk(static_cast<std::uint64_t>(transferred));
                    } catch (const std::exception&) {
                        rawFail(EFAULT);
                    }
                }
                continue;
            }
            if (syscall == 15 || syscall == 16 || syscall == 364 || syscall == 123 ||
                syscall == 124) {  // chmod, chown, lchown, fchown, fchmod
                const bool byDescriptor = syscall == 123 || syscall == 124;
                std::optional<std::string> path;
                if (byDescriptor) {
                    if (!rawOwnsDescriptor(registers[0])) {
                        rawFail(EBADF);
                        continue;
                    }
                } else {
                    path = rawPath(registers[0]);
                    if (!path) {
                        rawFail(EFAULT);
                        continue;
                    }
                }
                const auto descriptor = static_cast<int>(registers[0]);
                int status;
                if (syscall == 15) status = ::chmod(path->c_str(), static_cast<mode_t>(registers[1]));
                else if (syscall == 124) status = ::fchmod(descriptor, static_cast<mode_t>(registers[1]));
                else if (syscall == 16) {
                    status = ::chown(path->c_str(), static_cast<uid_t>(registers[1]),
                                     static_cast<gid_t>(registers[2]));
                } else if (syscall == 364) {
                    status = ::lchown(path->c_str(), static_cast<uid_t>(registers[1]),
                                      static_cast<gid_t>(registers[2]));
                } else {
                    status = ::fchown(descriptor, static_cast<uid_t>(registers[1]),
                                      static_cast<gid_t>(registers[2]));
                }
                if (status != 0) rawFail(errno);
                else rawOk(0);
                continue;
            }
            if (syscall == 10 || syscall == 137 || syscall == 128) {  // unlink, rmdir, rename
                const auto first = rawPath(registers[0]);
                const auto second = syscall == 128 ? rawPath(registers[1])
                                                   : std::optional<std::string>("");
                if (!first || !second) rawFail(EFAULT);
                else {
                    const auto status = syscall == 10 ? ::unlink(first->c_str())
                        : syscall == 137 ? ::rmdir(first->c_str())
                        : ::rename(first->c_str(), second->c_str());
                    if (status != 0) rawFail(errno);
                    else rawOk(0);
                }
                continue;
            }
            if (syscall == 334 || syscall == 423) {  // __semwait_signal[_nocancel]
                const auto condName = static_cast<std::uint32_t>(registers[0]);
                const auto mutexName = static_cast<std::uint32_t>(registers[1]);
                const bool hasTimeout = static_cast<std::uint32_t>(registers[2]) != 0;
                const bool relative = static_cast<std::uint32_t>(registers[3]) != 0;
                const auto seconds = static_cast<std::int64_t>(registers[4]);
                const auto nanos = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(registers[5]));
                const auto usable = [&](std::uint32_t name) {
                    const auto right = guestSendRightUrefs.find(name);
                    return right != guestSendRightUrefs.end() && right->second != 0 &&
                        guestSemaphoreNames.find(name) != guestSemaphoreNames.end();
                };
                if (!usable(condName) || (mutexName != 0 && !usable(mutexName))) {
                    rawFail(EINVAL);
                    continue;
                }
                std::optional<std::uint64_t> deadline;
                bool nonblocking = false;
                if (hasTimeout) {
                    if (seconds < 0 || nanos < 0 || nanos >= 1'000'000'000) {
                        rawFail(EINVAL);
                        continue;
                    }
                    const auto now = monotonicNanoseconds();
                    std::uint64_t requested = 0;
                    constexpr std::uint64_t cap = std::numeric_limits<std::uint64_t>::max() / 2;
                    if (relative) {
                        requested = static_cast<std::uint64_t>(seconds) > 9'000'000'000ULL
                            ? cap : static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL +
                                static_cast<std::uint64_t>(nanos);
                    } else {
                        timespec wall{};
                        clock_gettime(CLOCK_REALTIME, &wall);
                        const auto target = static_cast<__int128>(seconds) * 1'000'000'000 +
                            nanos;
                        const auto current = static_cast<__int128>(wall.tv_sec) *
                            1'000'000'000 + wall.tv_nsec;
                        requested = target <= current ? 0
                            : static_cast<std::uint64_t>(
                                std::min<__int128>(target - current, cap));
                    }
                    nonblocking = requested == 0;
                    deadline = now + requested;
                }
                if (mutexName != 0) (void)guestScheduler.signal_semaphore(mutexName);
                const auto result = guestScheduler.wait_semaphore(
                    condName, deadline, nonblocking, true);
                if (result.waiting) {
                    registers[0] = 0;
                    registers[32] &= ~std::uint64_t{8};
                    registers[34] = 0;
                    const auto* state = static_cast<const std::uint64_t*>(stateBuffer.data());
                    auto live = std::vector<std::uint64_t>(state,
                        state + stateBuffer.size() / sizeof(std::uint64_t));
                    if (!syncThreadSwitch(false, live)) {
                        throw std::runtime_error("Guest semaphore wait has no runnable thread");
                    }
                    continue;
                }
                if (result.error == KERN_SUCCESS) rawOk(0);
                else if (result.error == KERN_OPERATION_TIMED_OUT) rawFail(ETIMEDOUT);
                else rawFail(EINVAL);
                continue;
            }
            if (syscall == 515 || syscall == 516 || syscall == 544) {  // ulock_wait/wake/wait2
                constexpr std::uint32_t ulfWakeAll = 0x100;
                constexpr std::uint32_t ulfNoErrno = 0x01000000;
                const auto operation = static_cast<std::uint32_t>(registers[0]);
                const auto address = registers[1];
                const bool noErrno = (operation & ulfNoErrno) != 0;
                const auto finish = [&](std::int64_t result) {
                    if (result >= 0) {
                        registers[0] = static_cast<std::uint64_t>(result);
                        registers[32] &= ~std::uint64_t{8};
                    } else if (noErrno) {
                        registers[0] = static_cast<std::uint64_t>(result);
                        registers[32] &= ~std::uint64_t{8};
                    } else {
                        registers[0] = static_cast<std::uint64_t>(-result);
                        registers[32] |= 8U;
                    }
                    registers[34] = 0;
                };
                const auto kind = operation & 0xffU;
                if (kind < 1 || kind > 6 || address == 0) {
                    finish(-EINVAL);
                    continue;
                }
                if (syscall == 516) {
                    finish(guestScheduler.wake_ulock(
                        address, (operation & ulfWakeAll) != 0) == 0 ? -ENOENT : 0);
                    continue;
                }
                const bool wide = kind >= 4;
                std::uint64_t current = 0;
                try {
                    const auto* word = guestMemory.buffer(address, wide ? 8 : 4, false);
                    std::memcpy(&current, word, wide ? 8 : 4);
                } catch (const std::exception&) {
                    finish(-EFAULT);
                    continue;
                }
                const auto expected = wide ? registers[2]
                                           : static_cast<std::uint64_t>(
                                                 static_cast<std::uint32_t>(registers[2]));
                if (current != expected) {
                    finish(0);
                    continue;
                }
                std::optional<std::uint64_t> deadline;
                const auto timeout = syscall == 544 ? registers[3]
                    : static_cast<std::uint64_t>(static_cast<std::uint32_t>(registers[3]));
                if (timeout != 0) {
                    const auto nanoseconds = syscall == 544 ? timeout
                        : (timeout > std::numeric_limits<std::uint64_t>::max() / 1000
                               ? std::numeric_limits<std::uint64_t>::max() : timeout * 1000);
                    const auto now = monotonicNanoseconds();
                    deadline = now + std::min(
                        nanoseconds, std::numeric_limits<std::uint64_t>::max() - now);
                }
                if (!guestScheduler.wait_ulock(address, deadline, noErrno)) {
                    finish(-EDEADLK);
                    continue;
                }
                registers[0] = 0;
                registers[32] &= ~std::uint64_t{8};
                const auto* state = static_cast<const std::uint64_t*>(stateBuffer.data());
                auto live = std::vector<std::uint64_t>(state,
                    state + stateBuffer.size() / sizeof(std::uint64_t));
                if (!syncThreadSwitch(false, live)) {
                    throw std::runtime_error("Guest ulock wait has no runnable thread");
                }
                continue;
            }
            if (syscall == 60) {  // umask
                registers[0] = static_cast<std::uint64_t>(guestUmask.set(
                    static_cast<mode_t>(registers[0])));
                registers[32] &= ~std::uint64_t{8};
                registers[34] = 0;
                continue;
            }
            if (syscall == 336) {
                constexpr std::uint64_t carryMask = 8U;
                constexpr std::uint32_t procInfoCallPidInfo = 2;
                constexpr std::uint32_t procInfoCallSetControl = 5;
                constexpr std::uint32_t procPidUniqIdentifierInfo = 17;
                struct ProcUniqueIdentifierInfo {
                    std::uint8_t uuid[16];
                    std::uint64_t uniqueId;
                    std::uint64_t parentUniqueId;
                    std::uint64_t reserved[3];
                };
                static_assert(sizeof(ProcUniqueIdentifierInfo) == 56);
                const auto fail = [registers](int error) {
                    registers[0] = static_cast<std::uint32_t>(error);
                    registers[32] |= 8U;
                    registers[34] = 0;
                };
                const auto call = static_cast<std::uint32_t>(registers[0]);
                const auto requestedPid = static_cast<std::int32_t>(registers[1]);
                const auto flavor = static_cast<std::uint32_t>(registers[2]);
                const auto argument = registers[3];
                const auto outputAddress = registers[4];
                const auto outputSize = static_cast<std::int32_t>(registers[5]);
                const auto ownPid = static_cast<std::int32_t>(::getpid());
                if (call == procInfoCallSetControl) {
                    std::uint32_t error = 0;
                    try {
                        error = machModels.proc_info_setcontrol(
                            requestedPid, ownPid, flavor, outputAddress,
                            static_cast<std::uint32_t>(outputSize), guestScheduler.current_id());
                    } catch (const std::exception&) {
                        error = EFAULT;
                    }
                    if (error != 0) {
                        fail(static_cast<int>(error));
                    } else {
                        registers[0] = 0;
                        registers[32] &= ~carryMask;
                        registers[34] = 0;
                    }
                    continue;
                }
                if (call != procInfoCallPidInfo) {
                    survey("proc_info", 336, "call-" + std::to_string(call));
                    fail(EINVAL);
                    continue;
                }
                // The guest currently shares the runner PID. Never forward a
                // guest-selected foreign PID to the host proc-info API.
                if (requestedPid != ownPid) {
                    fail(ESRCH);
                    continue;
                }
                const std::size_t requiredSize = flavor == PROC_PIDT_SHORTBSDINFO
                    ? sizeof(proc_bsdshortinfo)
                    : flavor == procPidUniqIdentifierInfo
                        ? sizeof(ProcUniqueIdentifierInfo) : 0;
                if (requiredSize == 0) {
                    survey("proc_info", 336, "flavor-" + std::to_string(flavor));
                    fail(EINVAL);
                    continue;
                }
                if (outputSize < 0 || static_cast<std::size_t>(outputSize) < requiredSize) {
                    fail(ENOMEM);
                    continue;
                }
                if (outputAddress == 0) {
                    fail(EFAULT);
                    continue;
                }
                std::uint8_t* guestOutput = nullptr;
                try {
                    guestOutput = guestMemory.buffer(outputAddress, requiredSize, true);
                } catch (const std::out_of_range&) {
                    fail(EFAULT);
                    continue;
                }
                if (flavor == PROC_PIDT_SHORTBSDINFO) {
                    proc_bsdshortinfo info{};
                    errno = 0;
                    const auto result = ::proc_pidinfo(ownPid, static_cast<int>(flavor),
                        argument, &info, sizeof(info));
                    if (result != static_cast<int>(sizeof(info))) {
                        fail(errno == 0 ? EIO : errno);
                        continue;
                    }
                    info.pbsi_pid = static_cast<std::uint32_t>(requestedPid);
                    std::memset(info.pbsi_comm, 0, sizeof(info.pbsi_comm));
                    const auto executableName =
                        std::filesystem::path(bootstrap.resolvedExecutablePath).filename().string();
                    const auto nameSize = std::min(executableName.size(),
                        sizeof(info.pbsi_comm) - 1);
                    std::memcpy(info.pbsi_comm, executableName.data(), nameSize);
                    std::memcpy(guestOutput, &info, sizeof(info));
                } else {
                    if (image.main_uuid.size() != sizeof(ProcUniqueIdentifierInfo::uuid)) {
                        fail(EIO);
                        continue;
                    }
                    ProcUniqueIdentifierInfo info{};
                    errno = 0;
                    const auto result = ::proc_pidinfo(ownPid, static_cast<int>(flavor),
                        argument, &info, sizeof(info));
                    if (result != static_cast<int>(sizeof(info))) {
                        fail(errno == 0 ? EIO : errno);
                        continue;
                    }
                    std::copy(image.main_uuid.begin(), image.main_uuid.end(), info.uuid);
                    std::memcpy(guestOutput, &info, sizeof(info));
                }
                registers[0] = requiredSize;
                registers[32] &= ~carryMask;
                registers[34] = 0;
                continue;
            }
            if (syscall == 202) {
                constexpr std::uint64_t carryMask = 8U;
                constexpr std::uint64_t minimumGuestAddress =
                    ExecutableImage::minimum_image_address;
                registers[32] |= carryMask;
                const auto mibCount = static_cast<std::uint32_t>(registers[1]);
                const auto mibAddress = registers[0];
                bool mibReadable = false;
                bool supportedMib = false;
                std::string surveyMib;
                if (mibAddress >= minimumGuestAddress && mibCount > 0 &&
                    mibCount <= CTL_MAXNAME) {
                    try {
                        const auto* bytes = guestMemory.buffer(mibAddress,
                            static_cast<std::size_t>(mibCount) * sizeof(int));
                        std::array<int, CTL_MAXNAME> mib{};
                        std::memcpy(mib.data(), bytes,
                                    static_cast<std::size_t>(mibCount) * sizeof(int));
                        mibReadable = true;
                        if (guest_survey::enabled()) {
                            std::string name;
                            for (std::uint32_t index = 0; index < mibCount; ++index) {
                                name += (index == 0 ? "" : ".") + std::to_string(mib[index]);
                            }
                            surveyMib = name;
                        }
                        supportedMib = mibCount == 2 && mib[0] == CTL_KERN &&
                            (mib[1] == KERN_USRSTACK64 || mib[1] == KERN_USRSTACK32 ||
                             mib[1] == KERN_OSRELEASE);
                    } catch (const std::out_of_range&) {
                        mibReadable = false;
                    }
                }
                if (registers[4] != 0 || registers[5] != 0) {
                    registers[0] = EPERM;
                } else if (mibCount == 0 || mibCount > CTL_MAXNAME) {
                    registers[0] = EINVAL;
                } else if (!mibReadable) {
                    registers[0] = EFAULT;
                } else if (!supportedMib) {
                    survey("sysctl", 202, surveyMib);
                    registers[0] = EINVAL;
                } else if ((registers[3] != 0 && registers[3] < minimumGuestAddress) ||
                           (registers[3] != 0 && registers[2] != 0 &&
                            registers[2] < minimumGuestAddress)) {
                    registers[0] = EFAULT;
                } else {
                    std::size_t outputCapacity = 0;
                    bool validOutputLength = true;
                    if (registers[3] != 0) {
                        try {
                            std::memcpy(&outputCapacity, guestMemory.buffer(registers[3],
                                sizeof(outputCapacity)), sizeof(outputCapacity));
                        } catch (const std::out_of_range&) {
                            registers[0] = EFAULT;
                            validOutputLength = false;
                        }
                        if (validOutputLength && outputCapacity > static_cast<std::size_t>(
                                std::numeric_limits<std::int64_t>::max())) {
                            registers[0] = EINVAL;
                            validOutputLength = false;
                        }
                    }
                    if (validOutputLength) {
                        std::uint32_t savedErrno = 0;
                        std::memcpy(&savedErrno, guestMemory.buffer(bootstrap.errnoSlot,
                            sizeof(savedErrno)), sizeof(savedErrno));
                        auto trapRegisters = std::vector<std::uint64_t>(registers,
                                                                         registers + 36);
                        try {
                            (void)dispatch_guest_host_service(trapRegisters, guestMemory,
                                bootstrap, hostServiceState, 50);
                            if (static_cast<std::uint32_t>(trapRegisters[0]) ==
                                std::numeric_limits<std::uint32_t>::max()) {
                                std::uint32_t error = 0;
                                std::memcpy(&error, guestMemory.buffer(bootstrap.errnoSlot,
                                    sizeof(error)), sizeof(error));
                                registers[0] = error == 0 ? EIO : error;
                            } else {
                                registers[0] = trapRegisters[0];
                                registers[32] &= ~carryMask;
                            }
                        } catch (const std::out_of_range&) {
                            registers[0] = EFAULT;
                        }
                        std::memcpy(guestMemory.buffer(bootstrap.errnoSlot,
                            sizeof(savedErrno), true), &savedErrno, sizeof(savedErrno));
                    }
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-14))) {
                if (registers[0] != bootstrap.guestTaskPort) {
                    registers[0] = machSendInvalidDest;
                } else if (!hostServiceState.machVmProtect) {
                    registers[0] = kernInvalidArgument;
                } else {
                    registers[0] = hostServiceState.machVmProtect(
                        registers[1], registers[2], registers[3] != 0,
                        static_cast<std::uint32_t>(registers[4]), false);
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-33)) ||
                syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-36)) ||
                syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-38))) {
                std::optional<std::uint64_t> deadline;
                const auto operation = static_cast<std::int64_t>(syscall);
                const auto timeoutSeconds = static_cast<std::uint32_t>(registers[1]);
                const auto timeoutNanoseconds = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(registers[2]));
                const bool nonblocking = operation == -38 &&
                    timeoutSeconds == 0 && timeoutNanoseconds == 0;
                if (operation == -38) {
                    if (timeoutNanoseconds < 0 || timeoutNanoseconds >= 1'000'000'000) {
                        registers[0] = KERN_INVALID_VALUE;
                        registers[34] = 0;
                        continue;
                    }
                    const auto now = monotonicNanoseconds();
                    const auto requested = timeoutSeconds >
                            (std::numeric_limits<std::uint64_t>::max() -
                                static_cast<std::uint32_t>(timeoutNanoseconds)) /
                                1'000'000'000ULL
                        ? std::numeric_limits<std::uint64_t>::max()
                        : static_cast<std::uint64_t>(timeoutSeconds) * 1'000'000'000ULL +
                            static_cast<std::uint32_t>(timeoutNanoseconds);
                    deadline = now + std::min(
                        requested, std::numeric_limits<std::uint64_t>::max() - now);
                }
                if (operation == -33) {
                    (void)guestScheduler.expire_waiters(monotonicNanoseconds());
                    const auto name = static_cast<std::uint32_t>(registers[0]);
                    const auto right = guestSendRightUrefs.find(name);
                    registers[0] = right == guestSendRightUrefs.end() || right->second == 0
                        ? KERN_INVALID_NAME
                        : guestSemaphoreNames.find(name) == guestSemaphoreNames.end()
                            ? KERN_INVALID_CAPABILITY : static_cast<std::uint32_t>(
                                guestScheduler.signal_semaphore(name));
                } else {
                    const auto name = static_cast<std::uint32_t>(registers[0]);
                    const auto right = guestSendRightUrefs.find(name);
                    const auto result = right == guestSendRightUrefs.end() || right->second == 0
                        ? GuestSemaphoreWaitResult{KERN_INVALID_NAME, false}
                        : guestSemaphoreNames.find(name) == guestSemaphoreNames.end()
                            ? GuestSemaphoreWaitResult{KERN_INVALID_CAPABILITY, false}
                            : guestScheduler.wait_semaphore(name, deadline, nonblocking);
                    if (result.waiting) {
                        registers[0] = 0;
                        const auto* state = static_cast<const std::uint64_t*>(
                            stateBuffer.data());
                        auto live = std::vector<std::uint64_t>(state,
                            state + stateBuffer.size() / sizeof(std::uint64_t));
                        if (!syncThreadSwitch(false, live)) {
                            throw std::runtime_error(
                                "Guest semaphore wait has no runnable thread");
                        }
                        continue;
                    }
                    registers[0] = static_cast<std::uint32_t>(result.error);
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == SYS_kdebug_trace64) {
                // With kernel tracing disabled, kdebug_trace64 is a successful
                // no-op. It consumes only register values and leaves guest
                // errno untouched; do not forward events to host kdebug.
                registers[0] = 0;
                registers[32] &= ~std::uint64_t{8};
                registers[34] = 0;
                continue;
            }
            if (syscall == SYS_kdebug_typefilter) {
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
            if (syscall == SYS_bind || syscall == SYS_connect ||
                syscall == SYS_connect_nocancel) {
                constexpr std::uint64_t carryMask = 8U;
                const auto descriptor = static_cast<std::int32_t>(registers[0]);
                const auto addressLength = static_cast<std::uint32_t>(registers[2]);
                std::uint32_t error = 0;
                const auto guestSocket = hostServiceState.guestUnixSockets.find(descriptor);
                if (hostServiceState.openDescriptors.count(descriptor) == 0) {
                    error = EBADF;
                } else if (hostServiceState.socketDescriptors.count(descriptor) == 0 ||
                    guestSocket == hostServiceState.guestUnixSockets.end()) {
                    error = ENOTSOCK;
                } else {
                    auto path = guest_unix_socket_path(guestMemory, registers[1],
                        addressLength, error);
                    if (path) {
                        if (syscall == SYS_bind) {
                            if (guestSocket->second.boundPath) {
                                error = EINVAL;
                            } else {
                                try {
                                    const auto inserted = hostServiceState.guestUnixSocketPaths
                                        .emplace(*path, descriptor);
                                    if (!inserted.second) {
                                        error = EADDRINUSE;
                                    } else {
                                        guestSocket->second.boundPath = std::move(*path);
                                        guestSocket->second.boundAddressLength = addressLength;
                                        error = 0;
                                    }
                                } catch (const std::bad_alloc&) {
                                    error = ENOMEM;
                                }
                            }
                        } else {
                            const auto targetPath =
                                hostServiceState.guestUnixSocketPaths.find(*path);
                            if (targetPath == hostServiceState.guestUnixSocketPaths.end()) {
                                error = ENOENT;
                            } else if (targetPath->second == guestUnixClosedEndpoint) {
                                error = ECONNREFUSED;
                            } else {
                                const auto target = hostServiceState.guestUnixSockets.find(
                                    targetPath->second);
                                if (target == hostServiceState.guestUnixSockets.end()) {
                                    hostServiceState.guestUnixSocketPaths.erase(targetPath);
                                    error = ENOENT;
                                } else if (guestSocket->second.type != SOCK_DGRAM ||
                                    target->second.type != SOCK_DGRAM) {
                                    error = ENOTSUP;
                                    survey("enotsup", static_cast<std::int64_t>(syscall),
                                           "unix-connect-type");
                                } else {
                                    guestSocket->second.connectedPeer = target->first;
                                    guestSocket->second.connectedPeerClosed = false;
                                    error = 0;
                                }
                            }
                        }
                    }
                }
                registers[0] = error;
                if (error == 0) registers[32] &= ~carryMask;
                else registers[32] |= carryMask;
                registers[34] = 0;
                continue;
            }
            if ((syscall == SYS_sendto || syscall == SYS_recvfrom ||
                 syscall == SYS_sendto_nocancel || syscall == SYS_recvfrom_nocancel) &&
                hostServiceState.guestUnixSockets.count(
                    static_cast<std::int32_t>(registers[0])) != 0) {
                const bool sendsDatagram = syscall == SYS_sendto || syscall == SYS_sendto_nocancel;
                constexpr std::uint64_t carryMask = 8U;
                const auto descriptor = static_cast<std::int32_t>(registers[0]);
                std::uint32_t error = 0;
                std::uint64_t result = 0;
                const auto socket = hostServiceState.guestUnixSockets.find(descriptor);
                if (hostServiceState.openDescriptors.count(descriptor) == 0) {
                    error = EBADF;
                } else if (hostServiceState.socketDescriptors.count(descriptor) == 0 ||
                    socket == hostServiceState.guestUnixSockets.end()) {
                    error = ENOTSOCK;
                } else if (socket->second.type != SOCK_DGRAM) {
                    error = ENOTSUP;
                    survey("enotsup", static_cast<std::int64_t>(syscall), "unix-send-type");
                } else if (registers[3] != 0) {
                    error = ENOTSUP;
                    survey("enotsup", static_cast<std::int64_t>(syscall), "unix-send-flags");
                } else if (sendsDatagram) {
                    const auto byteCount = registers[2];
                    std::int32_t peerDescriptor = -1;
                    if (registers[4] == 0) {
                        if (socket->second.connectedPeerClosed) {
                            error = ECONNRESET;
                        } else if (!socket->second.connectedPeer) {
                            error = EDESTADDRREQ;
                        } else {
                            peerDescriptor = *socket->second.connectedPeer;
                        }
                    } else {
                        auto path = guest_unix_socket_path(guestMemory, registers[4],
                            static_cast<std::uint32_t>(registers[5]), error);
                        if (path) {
                            const auto targetPath =
                                hostServiceState.guestUnixSocketPaths.find(*path);
                            if (targetPath == hostServiceState.guestUnixSocketPaths.end()) {
                                error = ENOENT;
                            } else {
                                peerDescriptor = targetPath->second;
                            }
                        }
                    }
                    const auto peer = hostServiceState.guestUnixSockets.find(peerDescriptor);
                    if (error == 0 && peer == hostServiceState.guestUnixSockets.end()) {
                        error = ECONNREFUSED;
                    } else if (error == 0 && peer->second.type != SOCK_DGRAM) {
                        error = EPROTOTYPE;
                    } else if (error == 0 &&
                        (byteCount > std::numeric_limits<std::size_t>::max() ||
                         (byteCount != 0 && !guestMemory.is_guest_readable(
                            registers[1], byteCount)))) {
                        error = EFAULT;
                    } else if (error == 0) {
                        try {
                            HostServiceState::GuestUnixDatagram datagram;
                            if (byteCount != 0) {
                                const auto* source = guestMemory.buffer(registers[1], byteCount);
                                datagram.payload.assign(source, source + byteCount);
                            }
                            if (!peer->second.captureSink) {
                                datagram.sourcePath = socket->second.boundPath;
                                datagram.sourceLength = socket->second.boundPath
                                    ? socket->second.boundAddressLength : 16U;
                            }
                            if (peer->second.captureSink) {
                                constexpr std::size_t maximumCapturedDatagrams = 64;
                                if (hostServiceState.guestSyslogCapture.size() ==
                                    maximumCapturedDatagrams) {
                                    hostServiceState.guestSyslogCapture.pop_front();
                                }
                                hostServiceState.guestSyslogCapture.push_back(datagram);
                                if (progressTrace.enabled()) {
                                    constexpr char digits[] = "0123456789abcdef";
                                    constexpr std::size_t maximumTraceBytes = 64;
                                    const auto tracedBytes = std::min<std::size_t>(
                                        datagram.payload.size(), maximumTraceBytes);
                                    std::cerr << "arm64-metal trace syslog-datagram bytes="
                                              << datagram.payload.size() << " payload_hex=";
                                    for (std::size_t index = 0; index < tracedBytes; ++index) {
                                        const auto byte = datagram.payload[index];
                                        std::cerr << digits[byte >> 4U] << digits[byte & 0xfU];
                                    }
                                    if (tracedBytes < datagram.payload.size()) {
                                        std::cerr << "...";
                                    }
                                    std::cerr << '\n';
                                }
                            } else {
                                peer->second.receiveQueue.push_back(std::move(datagram));
                                deliverGuestUnixDatagrams(peerDescriptor);
                            }
                            result = byteCount;
                        } catch (const std::out_of_range&) {
                            error = EFAULT;
                        } catch (const std::bad_alloc&) {
                            error = ENOMEM;
                        }
                    }
                } else {
                    const auto flags = static_cast<std::int32_t>(registers[3]);
                    constexpr std::int32_t supportedReceiveFlags = MSG_DONTWAIT;
                    if ((flags & ~supportedReceiveFlags) != 0) {
                        error = ENOTSUP;
                        survey("enotsup", static_cast<std::int64_t>(syscall), "unix-recv-flags");
                    } else if (socket->second.receiveQueue.empty() &&
                        (flags & MSG_DONTWAIT) != 0) {
                        error = EAGAIN;
                    } else if (socket->second.receiveQueue.empty()) {
                        const auto threadId = guestScheduler.current_id();
                        auto& thread = guestScheduler.current();
                        if (thread.wait.has_value()) {
                            error = EALREADY;
                        } else {
                            bool blockedInserted = false;
                            try {
                                const auto inserted = guestUnixBlockedReceives.emplace(threadId,
                                    GuestUnixBlockedReceive{descriptor, registers[1],
                                        registers[2], registers[4], registers[5]});
                                if (!inserted.second) {
                                    error = EALREADY;
                                } else {
                                    blockedInserted = true;
                                    guestUnixReceiveWaiters[descriptor].push_back(threadId);
                                    thread.wait = GuestUnixSocketReceiveWait{descriptor};
                                    registers[34] = 0;
                                    std::vector<std::uint64_t> live(registers,
                                        registers + stateBuffer.size() / sizeof(std::uint64_t));
                                    if (!syncThreadSwitch(false, live)) {
                                        throw std::runtime_error(
                                            "Guest recvfrom has no runnable thread");
                                    }
                                    continue;
                                }
                            } catch (const std::bad_alloc&) {
                                if (blockedInserted) guestUnixBlockedReceives.erase(threadId);
                                auto waiting = guestUnixReceiveWaiters.find(descriptor);
                                if (waiting != guestUnixReceiveWaiters.end()) {
                                    auto& queue = waiting->second;
                                    queue.erase(std::remove(queue.begin(), queue.end(), threadId),
                                        queue.end());
                                    if (queue.empty()) guestUnixReceiveWaiters.erase(waiting);
                                }
                                thread.wait.reset();
                                error = ENOMEM;
                            }
                        }
                    } else {
                        const auto& datagram = socket->second.receiveQueue.front();
                        if (copy_guest_unix_datagram(datagram, guestMemory, registers[1],
                                registers[2], registers[4], registers[5], result, error)) {
                            socket->second.receiveQueue.pop_front();
                        }
                    }
                }
                if (error == 0) {
                    registers[0] = result;
                    registers[32] &= ~carryMask;
                } else {
                    registers[0] = error;
                    registers[32] |= carryMask;
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == SYS_socket) {
                constexpr std::uint64_t carryMask = 8U;
                const auto domain = static_cast<int>(static_cast<std::int32_t>(registers[0]));
                const auto type = static_cast<int>(static_cast<std::int32_t>(registers[1]));
                const auto protocol = static_cast<int>(static_cast<std::int32_t>(registers[2]));
                int result = -1;
                int error = 0;
                if (domain != AF_UNIX) {
                    error = EAFNOSUPPORT;
                } else if ((type != SOCK_STREAM && type != SOCK_DGRAM) || protocol != 0) {
                    error = EPROTONOSUPPORT;
                } else {
                    result = ::socket(AF_UNIX, type, 0);
                    if (result < 0) error = errno;
                    else {
                        try {
                            hostServiceState.openDescriptors.insert(result);
                            hostServiceState.socketDescriptors.insert(result);
                            HostServiceState::GuestUnixSocket guestSocket;
                            guestSocket.type = type;
                            hostServiceState.guestUnixSockets.emplace(result,
                                std::move(guestSocket));
                        } catch (const std::bad_alloc&) {
                            hostServiceState.openDescriptors.erase(result);
                            hostServiceState.socketDescriptors.erase(result);
                            hostServiceState.guestUnixSockets.erase(result);
                            (void)::close(result);
                            result = -1;
                            error = ENOMEM;
                        } catch (...) {
                            hostServiceState.openDescriptors.erase(result);
                            hostServiceState.socketDescriptors.erase(result);
                            hostServiceState.guestUnixSockets.erase(result);
                            (void)::close(result);
                            throw;
                        }
                    }
                }
                if (result >= 0 && progressTrace.enabled() &&
                    !socketSuccessDiagnosticEmitted) {
                    std::cerr << "arm64-metal trace socket-created pc=0x"
                              << hexadecimal(registers[33]) << " lr=0x"
                              << hexadecimal(registers[30]) << " x0=0x"
                              << hexadecimal(registers[0]) << " x1=0x"
                              << hexadecimal(registers[1]) << " x2=0x"
                              << hexadecimal(registers[2]) << " fd=" << result << '\n';
                    socketSuccessDiagnosticEmitted = true;
                }
                if (error != 0 && progressTrace.enabled() &&
                    !socketUnsupportedDiagnosticEmitted) {
                    std::cerr << "arm64-metal trace socket-rejected pc=0x"
                              << hexadecimal(registers[33]) << " lr=0x"
                              << hexadecimal(registers[30]) << " x1=0x"
                              << hexadecimal(registers[1]) << " x2=0x"
                              << hexadecimal(registers[2]) << '\n';
                    socketUnsupportedDiagnosticEmitted = true;
                }
                if (result < 0) {
                    registers[0] = static_cast<std::uint32_t>(error);
                    registers[32] |= carryMask;
                } else {
                    registers[0] = static_cast<std::uint64_t>(result);
                    registers[32] &= ~carryMask;
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
            if (syscall == 169 || syscall == 170) {
                const bool auditToken = syscall == 170;
                const auto pid = static_cast<std::int32_t>(registers[0]);
                const auto operation = static_cast<std::uint32_t>(registers[1]);
                const auto outputAddress = registers[2];
                const auto outputCapacity = registers[3];
                const auto auditTokenAddress = registers[4];
                if (auditToken && std::getenv("ARM64_METAL_TRACE_CSOPS_AUDITTOKEN") != nullptr) {
                    std::cerr << "arm64-metal trace csops-audittoken"
                              << " pc=0x" << hexadecimal(registers[33])
                              << " lr=0x" << hexadecimal(registers[30])
                              << " pid=" << pid
                              << " operation=" << operation
                              << " buffer=0x" << hexadecimal(outputAddress)
                              << " size=" << outputCapacity
                              << " token=0x" << hexadecimal(auditTokenAddress) << '\n';
                }
                std::uint32_t csopsError = 0;
                bool csopsTerminate = false;
                const auto verifyAuditToken = [&]() -> std::uint32_t {
                    if (auditTokenAddress < ExecutableImage::minimum_image_address) {
                        return EFAULT;
                    }
                    constexpr std::size_t auditTokenWords = TASK_AUDIT_TOKEN_COUNT;
                    std::array<std::uint32_t, auditTokenWords> guestAuditToken{};
                    std::array<integer_t, auditTokenWords> hostAuditToken{};
                    try {
                        const auto* tokenBytes = guestMemory.buffer(
                            auditTokenAddress, sizeof(guestAuditToken));
                        std::memcpy(guestAuditToken.data(), tokenBytes,
                                    sizeof(guestAuditToken));
                    } catch (const std::out_of_range&) {
                        return EFAULT;
                    }
                    auto hostAuditTokenCount = static_cast<mach_msg_type_number_t>(
                        TASK_AUDIT_TOKEN_COUNT);
                    const auto taskInfoResult = ::task_info(::mach_task_self(),
                        TASK_AUDIT_TOKEN, hostAuditToken.data(), &hostAuditTokenCount);
                    if (taskInfoResult != KERN_SUCCESS ||
                        hostAuditTokenCount != TASK_AUDIT_TOKEN_COUNT) return EIO;
                    if (guestAuditToken[5] != static_cast<std::uint32_t>(hostAuditToken[5]) ||
                        guestAuditToken[7] != static_cast<std::uint32_t>(hostAuditToken[7])) {
                        return ESRCH;
                    }
                    return 0;
                };
                if (auditToken && auditTokenAddress == 0) {
                    csopsError = EINVAL;
                } else if (pid != 0 && pid != ::getpid()) {
                    csopsError = guest_csops_other_pid_error(operation);
                } else {
                    if (auditToken) csopsError = verifyAuditToken();
                    if (csopsError == 0) {
                        try {
                            const auto outcome = guest_csops_self(guestMemory,
                                image.main_code_policy, csopsStatus, operation, outputAddress,
                                outputCapacity);
                            csopsError = outcome.error;
                            csopsTerminate = outcome.terminate;
                        } catch (const std::runtime_error&) {
                            if (!guest_survey::enabled()) throw;
                            survey("csops", static_cast<std::int64_t>(syscall),
                                   std::to_string(operation));
                            csopsError = EINVAL;
                        }
                    }
                }
                if (csopsTerminate) return 128 + SIGKILL;
                if (csopsError != 0) {
                    rawFail(static_cast<int>(csopsError));
                } else {
                    rawOk(0);
                }
                continue;
            }
            if (syscall == 6 || syscall == 396 || syscall == 397 || syscall == 399) {
                constexpr std::uint64_t carryMask = 8U;
                const auto closedDescriptor = static_cast<std::int32_t>(registers[0]);
                const bool closesSocket = (syscall == 6 || syscall == 399) &&
                    hostServiceState.socketDescriptors.count(closedDescriptor) != 0;
                const std::uint64_t service = syscall == 396 ? 23U
                    : syscall == 397 ? 2U : 22U;
                auto* errnoBytes = guestMemory.buffer(bootstrap.errnoSlot,
                    sizeof(std::uint32_t), true);
                std::uint32_t savedErrno = 0;
                std::memcpy(&savedErrno, errnoBytes, sizeof(savedErrno));
                auto callRegisters = std::vector<std::uint64_t>(registers, registers + 36);
                std::uint32_t error = 0;
                bool failed = false;
                try {
                    (void)dispatch_guest_host_service(callRegisters, guestMemory, bootstrap,
                                                      hostServiceState, service);
                    if (callRegisters[0] == std::numeric_limits<std::uint64_t>::max()) {
                        std::memcpy(&error, errnoBytes, sizeof(error));
                        if (error == 0) error = EIO;
                        failed = true;
                    }
                } catch (const std::out_of_range&) {
                    error = EFAULT;
                    failed = true;
                }
                std::memcpy(errnoBytes, &savedErrno, sizeof(savedErrno));
                if (closesSocket && !failed && callRegisters[0] == 0) {
                    closeGuestUnixReceiveWaiters(closedDescriptor);
                }
                if (failed) {
                    registers[0] = error;
                    registers[32] |= carryMask;
                } else {
                    registers[0] = callRegisters[0];
                    registers[32] &= ~carryMask;
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == 398) {
                constexpr std::uint64_t carryMask = 8U;
                auto* errnoBytes = guestMemory.buffer(bootstrap.errnoSlot,
                    sizeof(std::uint32_t), true);
                std::uint32_t savedErrno = 0;
                std::memcpy(&savedErrno, errnoBytes, sizeof(savedErrno));
                auto openRegisters = std::vector<std::uint64_t>(registers, registers + 36);
                std::uint32_t error = 0;
                bool failed = false;
                try {
                    (void)dispatch_guest_host_service(openRegisters, guestMemory, bootstrap,
                                                      hostServiceState, 21);
                    if (openRegisters[0] == std::numeric_limits<std::uint64_t>::max()) {
                        std::memcpy(&error, errnoBytes, sizeof(error));
                        if (error == 0) error = EIO;
                        failed = true;
                    }
                } catch (const std::out_of_range&) {
                    error = EFAULT;
                    failed = true;
                }
                std::memcpy(errnoBytes, &savedErrno, sizeof(savedErrno));
                if (failed) {
                    registers[0] = error;
                    registers[32] |= carryMask;
                } else {
                    registers[0] = openRegisters[0];
                    registers[32] &= ~carryMask;
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == 381) {
                const auto policy = guest_cstring(guestMemory, registers[0]);
                if (policy == "Quarantine" && registers[1] == 82) {
                    std::array<std::uint64_t, 3> arguments{};
                    const auto* argumentBytes = guestMemory.buffer(
                        registers[2], sizeof(arguments));
                    std::memcpy(arguments.data(), argumentBytes, sizeof(arguments));
                    const auto descriptor = static_cast<std::int32_t>(arguments[0]);
                    if (arguments[0] > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int32_t>::max()) ||
                        arguments[1] == 0 || arguments[1] <
                            ExecutableImage::minimum_image_address) {
                        registers[0] = EFAULT;
                        registers[32] |= 8U;
                    } else {
                        try {
                            (void)guestMemory.buffer(arguments[1], sizeof(std::uint64_t));
                            const auto savedHostErrno = errno;
                            errno = 0;
                            const auto quarantineSize = ::fgetxattr(descriptor,
                                "com.apple.quarantine", nullptr, 0, 0, 0);
                            const auto quarantineError = errno;
                            errno = savedHostErrno;
                            if (quarantineSize < 0 && quarantineError == ENOATTR) {
                                registers[0] = ENOATTR;
                                registers[32] &= ~std::uint64_t{8};
                            } else if (quarantineSize >= 0 && guest_survey::enabled()) {
                                survey("getinfo_fd", static_cast<std::int64_t>(syscall),
                                       "quarantine");
                                registers[0] = ENOTSUP;
                                registers[32] |= 8U;
                            } else if (quarantineSize >= 0) {
                                throw std::runtime_error(
                                    "Unsupported guest quarantine metadata via getinfo_fd");
                            } else {
                                registers[0] = quarantineError == 0 ? EIO : quarantineError;
                                registers[32] |= 8U;
                            }
                        } catch (const std::out_of_range&) {
                            registers[0] = EFAULT;
                            registers[32] |= 8U;
                        }
                    }
                    registers[34] = 0;
                    continue;
                }
                if ((registers[1] != 2 || policy != "Sandbox") && guest_survey::enabled()) {
                    survey("mac_policy", static_cast<std::int64_t>(syscall),
                           policy + ":" + std::to_string(registers[1]));
                    rawFailQuiet(ENOSYS);
                    continue;
                }
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
                const auto pidIsHostSelf = pid == static_cast<std::int64_t>(::getpid());
                const bool pathQuery = pidIsHostSelf && filter == 1 && flags == 0;
                const bool writeDataStatus = pid == 1 && filter == 240 &&
                                             flags == 0x20000005;
                if (!pathQuery && !writeDataStatus) {
                    const char* traceSandbox = std::getenv("ARM64_METAL_TRACE_SANDBOX");
                    if (traceSandbox != nullptr && std::strcmp(traceSandbox, "1") == 0) {
                        std::string operation;
                        if (operationAddress != 0) {
                            try {
                                operation = guest_cstring(guestMemory, operationAddress);
                            } catch (const std::exception&) {
                                operation = "<unreadable>";
                            }
                        }
                        std::cerr << "arm64-metal sandbox-query mismatch pid=" << pid
                                  << " self=" << ::getpid() << " filter=" << filter
                                  << " flags=" << flags << " path-null=" << (pathAddress == 0)
                                  << " operation=" << operation << " lr=0x" << std::hex
                                  << registers[30] << std::dec << '\n';
                    }
                    if (guest_survey::enabled()) {
                        survey("sandbox_query", static_cast<std::int64_t>(syscall), "shape",
                               "filter=" + std::to_string(filter));
                        rawFailQuiet(EINVAL);
                        continue;
                    }
                    throw std::runtime_error("Unsupported guest sandbox query shape");
                }
                registers[32] |= 8U;
                registers[0] = EFAULT;
                if (resultAddress != 0 && operationAddress != 0) {
                    bool unsupportedQuery = false;
                    try {
                        const auto operation = guest_cstring(guestMemory, operationAddress);
                        std::string argument;
                        if (writeDataStatus && pathAddress != 0) {
                            argument = guest_cstring(guestMemory, pathAddress);
                        }
                        if (writeDataStatus &&
                            (operation != "file-write-data" ||
                             (pathAddress != 0 && !argument.empty()))) {
                            unsupportedQuery = true;
                        }
                        if (!unsupportedQuery) {
                            auto* output = guestMemory.buffer(resultAddress,
                                sizeof(std::uint64_t), true);
                            std::uint64_t resultValue = 0;
                            const auto error = pathQuery
                                ? [&] {
                                      const auto path = guest_cstring(guestMemory, pathAddress);
                                      return guest_sandbox_check_path(operation.c_str(),
                                          path.c_str(), &resultValue);
                                  }()
                                : guest_sandbox_check_write_data_status(pathAddress != 0,
                                                                        &resultValue);
                            registers[0] = static_cast<std::uint64_t>(error);
                            if (error == 0) {
                                std::memcpy(output, &resultValue, sizeof(resultValue));
                                registers[32] &= ~std::uint64_t{8};
                            }
                        }
                    } catch (const std::exception&) {
                        registers[0] = EFAULT;
                    }
                    if (unsupportedQuery && guest_survey::enabled()) {
                        survey("sandbox_query", static_cast<std::int64_t>(syscall), "status");
                        rawFailQuiet(EINVAL);
                        continue;
                    }
                    if (unsupportedQuery) {
                        throw std::runtime_error("Unsupported guest sandbox status query shape");
                    }
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-47))) {
                const auto reply = static_cast<std::uint32_t>(registers[3] >> 32);
                const auto messageId = static_cast<std::uint32_t>(registers[4] >> 32);
                if (messageId == 3405 &&
                    std::getenv("ARM64_METAL_TRACE_TASK_INFO") != nullptr) {
                    constexpr std::uint32_t requestSize = 40;
                    const auto remote = static_cast<std::uint32_t>(registers[3]);
                    const bool expectedShape = registers[1] == 0x2'0000'0003ULL &&
                        static_cast<std::uint32_t>(registers[2]) == 0x1513 &&
                        (registers[2] >> 32) == requestSize && remote == guestMachTaskPort &&
                        static_cast<std::uint32_t>(registers[4]) == 0 &&
                        static_cast<std::uint32_t>(registers[5]) == 0 &&
                        static_cast<std::uint32_t>(registers[5] >> 32) == reply &&
                        (registers[6] >> 32) == 0 && registers[7] == 0 &&
                        guestReceiveRights.find(reply) != guestReceiveRights.end() &&
                        registers[0] != 0 &&
                        guestMemory.is_guest_readable(registers[0], requestSize);
                    if (expectedShape) {
                        const auto* request = guestMemory.buffer(registers[0], requestSize);
                        std::int32_t flavor = 0;
                        std::uint32_t inputCount = 0;
                        std::memcpy(&flavor, request + 32, sizeof(flavor));
                        std::memcpy(&inputCount, request + 36, sizeof(inputCount));
                        std::cerr << "arm64-metal trace task-info-request"
                                  << " pc=0x" << hexadecimal(registers[33])
                                  << " lr=0x" << hexadecimal(registers[30])
                                  << " task=guest-self flavor=" << flavor
                                  << " input-count=" << inputCount
                                  << " receive-capacity=" << static_cast<std::uint32_t>(registers[6])
                                  << '\n';
                    } else {
                        std::cerr << "arm64-metal trace task-info-request"
                                  << " pc=0x" << hexadecimal(registers[33])
                                  << " lr=0x" << hexadecimal(registers[30])
                                  << " shape=unsupported\n";
                    }
                }
                if (messageId == 3405) {
                    constexpr std::uint32_t machSendInvalidDestination = 0x10000003;
                    constexpr std::uint32_t machSendInvalidData = 0x10000002;
                    constexpr std::uint32_t machReceiveTooLarge = 0x10004004;
                    constexpr std::uint32_t requestSize = 40;
                    constexpr std::uint32_t errorReplySize = 36;
                    constexpr std::uint32_t minimumTrailerSize = 8;
                    constexpr std::uint32_t replyMessageId = 3505;
                    const auto remote = static_cast<std::uint32_t>(registers[3]);
                    const auto taskSendRight = guestSendRightUrefs.find(remote);
                    if (remote != guestMachTaskPort ||
                        taskSendRight == guestSendRightUrefs.end() ||
                        taskSendRight->second == 0) {
                        registers[0] = machSendInvalidDestination;
                        registers[34] = 0;
                        continue;
                    }
                    const auto receiveName = static_cast<std::uint32_t>(registers[5] >> 32);
                    if (registers[1] != 0x2'0000'0003ULL ||
                        static_cast<std::uint32_t>(registers[2]) != 0x1513 ||
                        (registers[2] >> 32) != requestSize ||
                        static_cast<std::uint32_t>(registers[4]) != 0 ||
                        static_cast<std::uint32_t>(registers[5]) != 0 ||
                        receiveName != reply || registers[6] >
                            std::numeric_limits<std::uint32_t>::max() ||
                        (registers[6] >> 32) != 0 || registers[7] != 0 ||
                        guestReceiveRights.find(reply) == guestReceiveRights.end()) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }
                    if (registers[0] == 0 ||
                        !guestMemory.is_guest_readable(registers[0], requestSize)) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }

                    struct TaskInfoRequest {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        task_flavor_t flavor;
                        mach_msg_type_number_t count;
                    };
                    struct TaskInfoErrorReply {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        kern_return_t returnCode;
                    };
                    struct TaskInfoMinimumTrailer {
                        mach_msg_trailer_type_t type;
                        mach_msg_trailer_size_t size;
                    };
                    static_assert(sizeof(TaskInfoRequest) == requestSize);
                    static_assert(sizeof(TaskInfoErrorReply) == errorReplySize);
                    static_assert(sizeof(TaskInfoMinimumTrailer) == minimumTrailerSize);
                    const auto* requestBytes = guestMemory.buffer(registers[0], requestSize);
                    TaskInfoRequest request{};
                    std::memcpy(&request, requestBytes, sizeof(request));
                    if (request.header.msgh_bits != 0x1513 ||
                        request.header.msgh_size != requestSize ||
                        request.header.msgh_remote_port != remote ||
                        request.header.msgh_local_port != reply ||
                        request.header.msgh_voucher_port != MACH_PORT_NULL ||
                        request.header.msgh_id != 3405 ||
                        std::memcmp(&request.ndr, &NDR_record, sizeof(request.ndr)) != 0) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }

                    constexpr std::size_t taskInfoMaxCount = 1024;
                    constexpr kern_return_t kernNotSupported = 46;
                    kern_return_t result = KERN_INVALID_ARGUMENT;
                    std::array<integer_t, taskInfoMaxCount> info{};
                    mach_msg_type_number_t infoCount = 0;
                    if (request.count > taskInfoMaxCount) {
                        result = KERN_INVALID_ARGUMENT;
                    } else if (request.flavor == TASK_DYLD_INFO) {
                        survey("msgh_id", 3405,
                               "task_info-flavor-" + std::to_string(request.flavor));
                        std::cerr << "arm64-metal mach-msg2-unhandled id=3405 dest=guest-task"
                                  << " size=40 options=0x" << hexadecimal(registers[1])
                                  << " pc=0x" << hexadecimal(registers[33])
                                  << " reason=task_info-flavor-" << request.flavor
                                  << " reply=KERN_NOT_SUPPORTED\n";
                        result = kernNotSupported;
                    } else {
                        infoCount = request.count;
                        result = ::task_info(::mach_task_self(), request.flavor,
                            reinterpret_cast<task_info_t>(info.data()), &infoCount);
                        if (result != KERN_SUCCESS || infoCount > request.count) {
                            if (result == KERN_SUCCESS) result = KERN_FAILURE;
                            infoCount = 0;
                        }
                    }

                    const std::uint32_t responseSize = result == KERN_SUCCESS
                        ? static_cast<std::uint32_t>(sizeof(mach_msg_header_t) +
                              sizeof(NDR_record_t) + 2 * sizeof(std::uint32_t) +
                              infoCount * sizeof(integer_t))
                        : errorReplySize;
                    if (registers[6] < responseSize + minimumTrailerSize) {
                        registers[0] = machReceiveTooLarge;
                        registers[34] = 0;
                        continue;
                    }
                    const auto outputSize = responseSize + minimumTrailerSize;
                    if (!guestMemory.is_guest_readable(registers[0], outputSize) ||
                        guestMemory.is_readonly(registers[0], outputSize)) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }
                    auto* replyBuffer = guestMemory.buffer(registers[0], outputSize, true);
                    const auto prepareHeader = [reply](mach_msg_header_t& header,
                                                       std::uint32_t size) {
                        header.msgh_bits = 0x1200;
                        header.msgh_size = size;
                        header.msgh_remote_port = MACH_PORT_NULL;
                        header.msgh_local_port = reply;
                        header.msgh_voucher_port = MACH_PORT_NULL;
                        header.msgh_id = replyMessageId;
                    };
                    if (result == KERN_SUCCESS) {
                        mach_msg_header_t header{};
                        prepareHeader(header, responseSize);
                        std::memcpy(replyBuffer, &header, sizeof(header));
                        std::size_t offset = sizeof(header);
                        std::memcpy(replyBuffer + offset, &NDR_record, sizeof(NDR_record_t));
                        offset += sizeof(NDR_record_t);
                        const std::uint32_t words[2] = {KERN_SUCCESS, infoCount};
                        std::memcpy(replyBuffer + offset, words, sizeof(words));
                        offset += sizeof(words);
                        std::memcpy(replyBuffer + offset, info.data(),
                                    infoCount * sizeof(integer_t));
                    } else {
                        TaskInfoErrorReply response{};
                        prepareHeader(response.header, sizeof(response));
                        response.ndr = NDR_record;
                        response.returnCode = result;
                        std::memcpy(replyBuffer, &response, sizeof(response));
                    }
                    const TaskInfoMinimumTrailer trailer{
                        MACH_MSG_TRAILER_FORMAT_0, minimumTrailerSize};
                    std::memcpy(replyBuffer + responseSize, &trailer, sizeof(trailer));
                    registers[0] = 0;
                    registers[34] = 0;
                    continue;
                }
                const bool clockServiceMessage = messageId == 1000 &&
                    guestClockServiceIds.find(static_cast<std::uint32_t>(registers[3])) !=
                        guestClockServiceIds.end();
                if (messageId == 3418 || messageId == 3419) {                    constexpr std::uint32_t machSendInvalidDestination = 0x10000003;
                    constexpr std::uint32_t machSendInvalidData = 0x1000000a;
                    constexpr std::uint32_t machMessageComplex = 0x80000000;
                    const auto remote = static_cast<std::uint32_t>(registers[3]);
                    const auto taskSendRight = guestSendRightUrefs.find(remote);
                    if (remote != guestMachTaskPort || taskSendRight == guestSendRightUrefs.end() ||
                        taskSendRight->second == 0) {
                        registers[0] = machSendInvalidDestination;
                        registers[34] = 0;
                        continue;
                    }
                    const bool isCreate = messageId == 3418;
                    if (registers[1] != 0x2'0000'0003ULL ||
                        static_cast<std::uint32_t>(registers[2]) !=
                            (isCreate ? 0x1513U : 0x80001513U) ||
                        static_cast<std::uint32_t>(registers[4]) != 0 ||
                        static_cast<std::uint32_t>(registers[5]) != (isCreate ? 0U : 1U) ||
                        (registers[5] >> 32) != reply ||
                        registers[6] > std::numeric_limits<std::uint32_t>::max() ||
                        (registers[6] >> 32) != 0 || registers[7] != 0 ||
                        guestReceiveRights.find(reply) == guestReceiveRights.end()) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }
                    if ((registers[2] >> 32) != 40 || registers[6] < 40) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }
                    struct SemaphoreCreateRequest {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        std::int32_t policy;
                        std::int32_t value;
                    };
                    struct SemaphoreDestroyRequest {
                        mach_msg_header_t header;
                        mach_msg_body_t body;
                        mach_msg_port_descriptor_t semaphore;
                    };
                    struct SemaphoreErrorReply {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        kern_return_t returnCode;
                    };
                    struct SemaphoreCreateReply {
                        mach_msg_header_t header;
                        mach_msg_body_t body;
                        mach_msg_port_descriptor_t semaphore;
                    };
                    static_assert(sizeof(SemaphoreCreateRequest) == 40);
                    static_assert(sizeof(SemaphoreDestroyRequest) == 40);
                    static_assert(sizeof(SemaphoreErrorReply) == 36);
                    static_assert(sizeof(SemaphoreCreateReply) == 40);
                    const auto prepareHeader = [reply](mach_msg_header_t& header,
                                                       std::uint32_t size,
                                                       std::uint32_t bits,
                                                       std::uint32_t id) {
                        header.msgh_bits = bits;
                        header.msgh_size = size;
                        header.msgh_remote_port = MACH_PORT_NULL;
                        header.msgh_local_port = reply;
                        header.msgh_voucher_port = MACH_PORT_NULL;
                        header.msgh_id = id;
                    };
                    const auto* requestBytes = guestMemory.buffer(registers[0], 40);
                    SemaphoreErrorReply errorReply{};
                    prepareHeader(errorReply.header, sizeof(errorReply), 0x1200,
                                  messageId + 100);
                    errorReply.ndr = NDR_record;
                    errorReply.returnCode = KERN_INVALID_ARGUMENT;
                    std::uint32_t result = KERN_INVALID_ARGUMENT;
                    if (isCreate) {
                        SemaphoreCreateRequest request{};
                        std::memcpy(&request, requestBytes, sizeof(request));
                        if (request.header.msgh_bits != 0x1513 ||
                            request.header.msgh_size != sizeof(request) ||
                            request.header.msgh_remote_port != guestMachTaskPort ||
                            request.header.msgh_local_port != reply ||
                            request.header.msgh_voucher_port != MACH_PORT_NULL ||
                            static_cast<std::uint32_t>(request.header.msgh_id) != messageId ||
                            std::memcmp(&request.ndr, &NDR_record, sizeof(request.ndr)) != 0) {
                            registers[0] = machSendInvalidData;
                            registers[34] = 0;
                            continue;
                        }
                        if (nextGuestSemaphoreName == 0 ||
                            nextGuestSemaphoreName == std::numeric_limits<std::uint32_t>::max()) {
                            result = KERN_RESOURCE_SHORTAGE;
                        } else {
                            const auto name = nextGuestSemaphoreName++;
                            result = static_cast<std::uint32_t>(guestScheduler.create_semaphore(
                                name, request.policy, request.value));
                            if (result == KERN_SUCCESS) {
                                guestSendRightUrefs.emplace(name, 1U);
                                guestSemaphoreNames.insert(name);
                                auto* replyBuffer = guestMemory.buffer(registers[0],
                                    sizeof(SemaphoreCreateReply), true);
                                SemaphoreCreateReply response{};
                                prepareHeader(response.header, sizeof(response),
                                    machMessageComplex | 0x1200, messageId + 100);
                                response.body.msgh_descriptor_count = 1;
                                response.semaphore.name = name;
                                response.semaphore.disposition = MACH_MSG_TYPE_MOVE_SEND;
                                response.semaphore.type = MACH_MSG_PORT_DESCRIPTOR;
                                std::memcpy(replyBuffer, &response, sizeof(response));
                                registers[0] = 0;
                                registers[34] = 0;
                                continue;
                            }
                        }
                    } else {
                        SemaphoreDestroyRequest request{};
                        std::memcpy(&request, requestBytes, sizeof(request));
                        if (request.header.msgh_bits !=
                                (machMessageComplex | 0x1513) ||
                            request.header.msgh_size != sizeof(request) ||
                            request.header.msgh_remote_port != guestMachTaskPort ||
                            request.header.msgh_local_port != reply ||
                            request.header.msgh_voucher_port != MACH_PORT_NULL ||
                            static_cast<std::uint32_t>(request.header.msgh_id) != messageId ||
                            request.body.msgh_descriptor_count != 1 ||
                            request.semaphore.type != MACH_MSG_PORT_DESCRIPTOR ||
                            request.semaphore.disposition != MACH_MSG_TYPE_MOVE_SEND) {
                            registers[0] = machSendInvalidData;
                            registers[34] = 0;
                            continue;
                        }
                        const auto name = request.semaphore.name;
                        const auto right = guestSendRightUrefs.find(name);
                        if (right == guestSendRightUrefs.end() || right->second == 0 ||
                            guestSemaphoreNames.find(name) == guestSemaphoreNames.end()) {
                            result = KERN_INVALID_CAPABILITY;
                        } else {
                            (void)guestScheduler.expire_waiters(monotonicNanoseconds());
                            result = static_cast<std::uint32_t>(
                                guestScheduler.destroy_semaphore(name));
                            if (result == KERN_SUCCESS) {
                                guestSemaphoreNames.erase(name);
                                guestSendRightUrefs.erase(right);
                            }
                        }
                    }
                    errorReply.returnCode = static_cast<kern_return_t>(result);
                    std::memcpy(guestMemory.buffer(registers[0], sizeof(errorReply), true),
                                &errorReply, sizeof(errorReply));
                    registers[0] = 0;
                    registers[34] = 0;
                    continue;
                }
                if (messageId == 3410) {
                    constexpr std::uint32_t machSendInvalidDestination = 0x10000003;
                    constexpr std::uint32_t machSendInvalidData = 0x10000002;
                    constexpr std::uint32_t machSendInvalidRight = 0x1000000a;
                    constexpr std::uint32_t machReceiveTooLarge = 0x10004004;
                    constexpr std::uint32_t machMessageComplex = 0x80000000;
                    constexpr std::uint32_t machCopySend = 19;
                    constexpr std::uint32_t requestSize = 52;
                    constexpr std::uint32_t responseSize = 36;
                    constexpr std::uint32_t minimumTrailerSize = 8;
                    constexpr std::uint32_t taskDebugControlPort = 10;
                    constexpr std::uint32_t replyMessageId = 3510;
                    const auto remote = static_cast<std::uint32_t>(registers[3]);
                    const auto taskSendRight = guestSendRightUrefs.find(remote);
                    if (remote != guestMachTaskPort ||
                        taskSendRight == guestSendRightUrefs.end() ||
                        taskSendRight->second == 0) {
                        registers[0] = machSendInvalidDestination;
                        registers[34] = 0;
                        continue;
                    }
                    const auto receiveName = static_cast<std::uint32_t>(registers[5] >> 32);
                    const auto message = static_cast<std::uint32_t>(registers[4] >> 32);
                    if (registers[1] != 0x2'0000'0003ULL ||
                        static_cast<std::uint32_t>(registers[2]) !=
                            (machMessageComplex | 0x1513U) ||
                        (registers[2] >> 32) != requestSize ||
                        static_cast<std::uint32_t>(registers[4]) != 0 ||
                        message != 3410 || static_cast<std::uint32_t>(registers[5]) != 1 ||
                        receiveName != reply || registers[6] >
                            std::numeric_limits<std::uint32_t>::max() ||
                        (registers[6] >> 32) != 0 || registers[7] != 0 ||
                        guestReceiveRights.find(reply) == guestReceiveRights.end()) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }

                    struct TaskSetSpecialPortRequest {
                        mach_msg_header_t header;
                        mach_msg_body_t body;
                        mach_msg_port_descriptor_t specialPort;
                        NDR_record_t ndr;
                        std::int32_t whichPort;
                    };
                    struct TaskSetSpecialPortReply {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        kern_return_t returnCode;
                    };
                    struct TaskSetSpecialPortMinimumTrailer {
                        mach_msg_trailer_type_t type;
                        mach_msg_trailer_size_t size;
                    };
                    static_assert(sizeof(TaskSetSpecialPortRequest) == requestSize);
                    static_assert(sizeof(TaskSetSpecialPortReply) == responseSize);
                    static_assert(sizeof(TaskSetSpecialPortMinimumTrailer) ==
                                  minimumTrailerSize);
                    const auto* requestBytes = guestMemory.buffer(registers[0], requestSize);
                    TaskSetSpecialPortRequest request{};
                    std::memcpy(&request, requestBytes, sizeof(request));
                    if (request.header.msgh_bits !=
                            (machMessageComplex | 0x1513U) ||
                        request.header.msgh_size != requestSize ||
                        request.header.msgh_remote_port != remote ||
                        request.header.msgh_local_port != reply ||
                        request.header.msgh_voucher_port != MACH_PORT_NULL ||
                        request.header.msgh_id != 3410 ||
                        request.body.msgh_descriptor_count != 1 ||
                        request.specialPort.disposition != machCopySend ||
                        request.specialPort.type != MACH_MSG_PORT_DESCRIPTOR ||
                        std::memcmp(&request.ndr, &NDR_record, sizeof(request.ndr)) != 0) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }

                    kern_return_t result = KERN_SUCCESS;
                    const auto specialName = request.specialPort.name;
                    if (specialName != MACH_PORT_NULL) {
                        const auto specialRight = guestSendRightUrefs.find(specialName);
                        if (specialRight == guestSendRightUrefs.end() ||
                            specialRight->second == 0 ||
                            guestReceiveRights.find(specialName) ==
                                guestReceiveRights.end() ||
                            guestDeadPortNames.find(specialName) !=
                                guestDeadPortNames.end()) {
                            registers[0] = machSendInvalidRight;
                            registers[34] = 0;
                            continue;
                        }
                    }
                    if (request.whichPort !=
                        static_cast<std::int32_t>(taskDebugControlPort)) {
                        result = KERN_INVALID_ARGUMENT;
                    } else {
                        // COPY_SEND leaves the caller's urefs unchanged. The
                        // task's held reference is tracked separately from them.
                        guestTaskDebugControlPort = specialName;
                        guestTaskDebugControlPortDead = false;
                    }

                    if (registers[6] < responseSize + minimumTrailerSize) {
                        registers[0] = machReceiveTooLarge;
                        registers[34] = 0;
                        continue;
                    }
                    auto* replyBuffer = guestMemory.buffer(
                        registers[0], responseSize + minimumTrailerSize, true);
                    TaskSetSpecialPortReply response{};
                    response.header.msgh_bits = 0x1200;
                    response.header.msgh_size = responseSize;
                    response.header.msgh_remote_port = MACH_PORT_NULL;
                    response.header.msgh_local_port = reply;
                    response.header.msgh_voucher_port = MACH_PORT_NULL;
                    response.header.msgh_id = replyMessageId;
                    response.ndr = NDR_record;
                    response.returnCode = result;
                    std::memcpy(replyBuffer, &response, sizeof(response));
                    const TaskSetSpecialPortMinimumTrailer trailer{
                        MACH_MSG_TRAILER_FORMAT_0, minimumTrailerSize};
                    std::memcpy(replyBuffer + responseSize, &trailer, sizeof(trailer));
                    registers[0] = 0;
                    registers[34] = 0;
                    continue;
                }
                if (messageId == 3409) {
                    constexpr std::uint32_t machSendInvalidDestination = 0x10000003;
                    constexpr std::uint32_t machSendInvalidData = 0x10000002;
                    constexpr std::uint32_t machReceiveTooLarge = 0x10004004;
                    constexpr kern_return_t kernInvalidArgument = 4;
                    constexpr kern_return_t kernDenied = 53;
                    constexpr kern_return_t kernNoSpace = 3;
                    constexpr std::uint32_t taskKernelPort = 1;
                    constexpr std::uint32_t taskNamePortSelector = 3;
                    constexpr std::uint32_t taskBootstrapPort = 4;
                    constexpr std::uint32_t taskDebugControlPort = 10;
                    constexpr std::uint32_t machMessageComplex = 0x80000000;
                    constexpr std::uint32_t requestSize = 36;
                    constexpr std::uint32_t successReplySize = 40;
                    constexpr std::uint32_t errorReplySize = 36;
                    constexpr std::uint32_t minimumTrailerSize = 8;
                    const auto remote = static_cast<std::uint32_t>(registers[3]);
                    const auto taskSendRight = guestSendRightUrefs.find(remote);
                    if (remote != guestMachTaskPort || taskSendRight == guestSendRightUrefs.end() ||
                        taskSendRight->second == 0) {
                        registers[0] = machSendInvalidDestination;
                        registers[34] = 0;
                        continue;
                    }
                    const auto receiveName = static_cast<std::uint32_t>(registers[5] >> 32);
                    const auto message = static_cast<std::uint32_t>(registers[4] >> 32);
                    if (registers[1] != 0x2'0000'0003ULL ||
                        static_cast<std::uint32_t>(registers[2]) != 0x1513 ||
                        (registers[2] >> 32) != requestSize ||
                        static_cast<std::uint32_t>(registers[4]) != 0 || message != 3409 ||
                        static_cast<std::uint32_t>(registers[5]) != 0 ||
                        receiveName != reply || registers[6] >
                            std::numeric_limits<std::uint32_t>::max() ||
                        (registers[6] >> 32) != 0 || registers[7] != 0 ||
                        guestReceiveRights.find(reply) == guestReceiveRights.end()) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }

                    const auto* requestBytes = guestMemory.buffer(registers[0], requestSize);
                    mach_msg_header_t requestHeader{};
                    NDR_record_t requestNdr{};
                    std::int32_t whichPort = 0;
                    std::memcpy(&requestHeader, requestBytes, sizeof(requestHeader));
                    std::memcpy(&requestNdr, requestBytes + sizeof(requestHeader),
                                sizeof(requestNdr));
                    std::memcpy(&whichPort, requestBytes + sizeof(requestHeader) +
                                sizeof(requestNdr), sizeof(whichPort));
                    if (requestHeader.msgh_bits != 0x1513 ||
                        requestHeader.msgh_size != requestSize ||
                        requestHeader.msgh_remote_port != remote ||
                        requestHeader.msgh_local_port != reply ||
                        requestHeader.msgh_voucher_port != MACH_PORT_NULL ||
                        requestHeader.msgh_id != 3409 ||
                        std::memcmp(&requestNdr, &NDR_record, sizeof(requestNdr)) != 0) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }

                    kern_return_t result = KERN_SUCCESS;
                    std::uint32_t specialPortName =
                        static_cast<std::uint32_t>(guestMachBootstrapPort);
                    bool specialPortIsDead = false;
                    if (whichPort == static_cast<std::int32_t>(taskKernelPort) ||
                        whichPort == -1) {
                        result = kernDenied;
                    } else if (whichPort == 0 || whichPort == 7) {
                        result = kernInvalidArgument;
                    } else if (whichPort ==
                               static_cast<std::int32_t>(taskNamePortSelector)) {
                        const auto taskName = getGuestTaskNamePort();
                        if (taskName.has_value()) {
                            specialPortName = *taskName;
                        } else {
                            result = kernNoSpace;
                        }
                    } else if (whichPort ==
                               static_cast<std::int32_t>(taskDebugControlPort)) {
                        if (!guestTaskDebugControlPort.has_value()) {
                            throw std::runtime_error(
                                "Guest task debug-control port has not been initialized");
                        }
                        specialPortName = *guestTaskDebugControlPort;
                        specialPortIsDead = guestTaskDebugControlPortDead;
                    } else if (whichPort != static_cast<std::int32_t>(taskBootstrapPort)) {
                        survey("msgh_id", 3409, "special-port-" + std::to_string(whichPort));
                        std::cerr << "arm64-metal mach-msg2-unhandled id=3409 dest=guest-task"
                                  << " size=" << requestSize << " options=0x"
                                  << hexadecimal(registers[1]) << " pc=0x"
                                  << hexadecimal(registers[33]) << " reason=special-port-"
                                  << whichPort << " reply=KERN_NOT_SUPPORTED\n";
                        result = 46;
                    }

                    const auto responseSize = result == KERN_SUCCESS
                        ? successReplySize : errorReplySize;
                    if (registers[6] < responseSize + minimumTrailerSize) {
                        registers[0] = machReceiveTooLarge;
                        registers[34] = 0;
                        continue;
                    }
                    auto* replyBuffer = guestMemory.buffer(
                        registers[0], responseSize + minimumTrailerSize, true);
                    struct TaskGetSpecialPortErrorReply {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        kern_return_t returnCode;
                    };
                    struct TaskGetSpecialPortReply {
                        mach_msg_header_t header;
                        mach_msg_body_t body;
                        mach_msg_port_descriptor_t specialPort;
                    };
                    struct TaskGetSpecialPortMinimumTrailer {
                        mach_msg_trailer_type_t type;
                        mach_msg_trailer_size_t size;
                    };
                    static_assert(sizeof(TaskGetSpecialPortErrorReply) == errorReplySize);
                    static_assert(sizeof(TaskGetSpecialPortReply) == successReplySize);
                    static_assert(sizeof(TaskGetSpecialPortMinimumTrailer) ==
                                  minimumTrailerSize);
                    const auto prepareHeader = [reply](mach_msg_header_t& header,
                                                       std::uint32_t size,
                                                       std::uint32_t bits) {
                        header.msgh_bits = bits;
                        header.msgh_size = size;
                        header.msgh_remote_port = MACH_PORT_NULL;
                        header.msgh_local_port = reply;
                        header.msgh_voucher_port = MACH_PORT_NULL;
                        header.msgh_id = 3509;
                    };
                    if (result == KERN_SUCCESS) {
                        TaskGetSpecialPortReply response{};
                        prepareHeader(response.header, sizeof(response),
                                      machMessageComplex | 0x1200);
                        response.body.msgh_descriptor_count = 1;
                        response.specialPort.name = specialPortName == MACH_PORT_NULL
                            ? MACH_PORT_NULL
                            : specialPortIsDead ? MACH_PORT_DEAD
                            : acquireGuestFixedSendRight(specialPortName);
                        response.specialPort.disposition = MACH_MSG_TYPE_MOVE_SEND;
                        response.specialPort.type = MACH_MSG_PORT_DESCRIPTOR;
                        std::memcpy(replyBuffer, &response, sizeof(response));
                    } else {
                        TaskGetSpecialPortErrorReply response{};
                        prepareHeader(response.header, sizeof(response), 0x1200);
                        response.ndr = NDR_record;
                        response.returnCode = result;
                        std::memcpy(replyBuffer, &response, sizeof(response));
                    }
                    const TaskGetSpecialPortMinimumTrailer trailer{
                        MACH_MSG_TRAILER_FORMAT_0, minimumTrailerSize};
                    std::memcpy(replyBuffer + responseSize, &trailer, sizeof(trailer));
                    registers[0] = 0;
                    registers[34] = 0;
                    continue;
                }
                if (messageId == 206) {
                    constexpr std::uint32_t machSendInvalidDestination = 0x10000003;
                    constexpr std::uint32_t machSendInvalidData = 0x1000000a;
                    constexpr std::uint32_t machMessageComplex = 0x80000000;
                    constexpr std::uint32_t kernResourceShortage = 6;
                    const auto remote = static_cast<std::uint32_t>(registers[3]);
                    const auto hostSendRight = guestSendRightUrefs.find(remote);
                    if (remote != guestMachHostPort ||
                        hostSendRight == guestSendRightUrefs.end() ||
                        hostSendRight->second == 0) {
                        registers[0] = machSendInvalidDestination;
                        registers[34] = 0;
                        continue;
                    }
                    if (registers[1] != 0x2'0000'0003ULL ||
                        static_cast<std::uint32_t>(registers[2]) != 0x1513 ||
                        static_cast<std::uint32_t>(registers[4]) != 0 ||
                        static_cast<std::uint32_t>(registers[5]) != 0 ||
                        (registers[5] >> 32) != reply ||
                        registers[6] > std::numeric_limits<std::uint32_t>::max() ||
                        (registers[6] >> 32) != 0 || registers[7] != 0 ||
                        guestReceiveRights.find(reply) == guestReceiveRights.end()) {
                        survey("msgh_id", messageId, "host_get_clock_service-shape");
                        std::cerr << "arm64-metal mach-msg2-unhandled id=" << messageId
                                  << " dest=0x" << hexadecimal(registers[3] & 0xffffffffULL)
                                  << " size=" << (registers[2] >> 32) << " options=0x"
                                  << hexadecimal(registers[1]) << " pc=0x"
                                  << hexadecimal(registers[33]) << " reason=host_get_clock_service-shape\n";
                        registers[0] = 0x10000002;
                        registers[34] = 0;
                        continue;
                    }
                    if ((registers[2] >> 32) != 36 || registers[6] < 36) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }
                    const auto* request = guestMemory.buffer(registers[0], 36);
                    mach_msg_header_t requestHeader{};
                    NDR_record_t requestNdr{};
                    clock_id_t clockId = 0;
                    std::memcpy(&requestHeader, request, sizeof(requestHeader));
                    std::memcpy(&requestNdr, request + sizeof(requestHeader), sizeof(requestNdr));
                    std::memcpy(&clockId, request + sizeof(requestHeader) + sizeof(requestNdr),
                                sizeof(clockId));
                    if (requestHeader.msgh_bits != 0x1513 || requestHeader.msgh_size != 36 ||
                        requestHeader.msgh_id != 206 ||
                        requestHeader.msgh_remote_port != guestMachHostPort ||
                        requestHeader.msgh_local_port != reply ||
                        requestHeader.msgh_voucher_port != MACH_PORT_NULL ||
                        std::memcmp(&requestNdr, &NDR_record, sizeof(requestNdr)) != 0) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }

                    struct HostClockServiceErrorReply {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        kern_return_t returnCode;
                    };
                    struct HostClockServiceReply {
                        mach_msg_header_t header;
                        mach_msg_body_t body;
                        mach_msg_port_descriptor_t clockService;
                    };
                    static_assert(sizeof(HostClockServiceErrorReply) == 36);
                    static_assert(sizeof(HostClockServiceReply) == 40);
                    static_assert(offsetof(HostClockServiceReply, clockService) == 28);

                    const bool supportedClock = clockId == SYSTEM_CLOCK ||
                                                clockId == CALENDAR_CLOCK;
                    const auto clockName = supportedClock
                        ? static_cast<std::uint32_t>(guestMachHostPort) + 2U +
                              static_cast<std::uint32_t>(clockId)
                        : 0U;
                    const auto references = guestSendRightUrefs.find(clockName);
                    const bool canAcquire = supportedClock &&
                        (references == guestSendRightUrefs.end() ||
                         references->second < std::numeric_limits<std::uint32_t>::max());
                    const auto responseCapacity = canAcquire ? sizeof(HostClockServiceReply)
                                                             : sizeof(HostClockServiceErrorReply);
                    if (responseCapacity > registers[6]) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }
                    auto* replyBuffer = guestMemory.buffer(registers[0], responseCapacity, true);

                    const auto prepareHeader = [reply](mach_msg_header_t& header,
                                                       std::uint32_t size,
                                                       std::uint32_t bits) {
                        header.msgh_bits = bits;
                        header.msgh_size = size;
                        header.msgh_remote_port = MACH_PORT_NULL;
                        header.msgh_local_port = reply;
                        header.msgh_voucher_port = MACH_PORT_NULL;
                        header.msgh_id = 306;
                    };
                    HostClockServiceErrorReply errorReply{};
                    prepareHeader(errorReply.header, sizeof(errorReply.header) +
                                      sizeof(errorReply.ndr) + sizeof(errorReply.returnCode),
                                  0x1200);
                    errorReply.ndr = NDR_record;
                    errorReply.returnCode = KERN_INVALID_ARGUMENT;
                    std::uint32_t result = KERN_INVALID_ARGUMENT;
                    if (supportedClock) {
                        if (references == guestSendRightUrefs.end()) {
                            guestSendRightUrefs.emplace(clockName, 1U);
                            guestClockServiceIds.emplace(clockName, clockId);
                            result = KERN_SUCCESS;
                        } else if (references->second ==
                                   std::numeric_limits<std::uint32_t>::max()) {
                            result = kernResourceShortage;
                        } else {
                            ++references->second;
                            result = KERN_SUCCESS;
                        }
                    }
                    if (result == KERN_SUCCESS) {
                        HostClockServiceReply response{};
                        prepareHeader(response.header, sizeof(response),
                                      machMessageComplex | 0x1200);
                        response.body.msgh_descriptor_count = 1;
                        response.clockService.name = clockName;
                        response.clockService.disposition = MACH_MSG_TYPE_MOVE_SEND;
                        response.clockService.type = MACH_MSG_PORT_DESCRIPTOR;
                        std::memcpy(replyBuffer, &response, sizeof(response));
                    } else {
                        errorReply.returnCode = static_cast<kern_return_t>(result);
                        std::memcpy(replyBuffer, &errorReply, sizeof(errorReply));
                    }
                    registers[0] = 0;
                    registers[34] = 0;
                    continue;
                }
                if (messageId == 1000 && clockServiceMessage) {
                    constexpr std::uint32_t machSendInvalidDestination = 0x10000003;
                    constexpr std::uint32_t machSendInvalidData = 0x1000000a;
                    const auto remote = static_cast<std::uint32_t>(registers[3]);
                    const auto service = guestClockServiceIds.find(remote);
                    const auto sendRight = guestSendRightUrefs.find(remote);
                    if (service == guestClockServiceIds.end() ||
                        sendRight == guestSendRightUrefs.end() || sendRight->second == 0) {
                        registers[0] = machSendInvalidDestination;
                        registers[34] = 0;
                        continue;
                    }
                    if (registers[1] != 0x2'0000'0003ULL ||
                        static_cast<std::uint32_t>(registers[2]) != 0x1513 ||
                        static_cast<std::uint32_t>(registers[4]) != 0 ||
                        static_cast<std::uint32_t>(registers[5]) != 0 ||
                        (registers[5] >> 32) != reply ||
                        registers[6] > std::numeric_limits<std::uint32_t>::max() ||
                        (registers[6] >> 32) != 0 || registers[7] != 0 ||
                        guestReceiveRights.find(reply) == guestReceiveRights.end()) {
                        survey("msgh_id", messageId, "clock_get_time-shape");
                        std::cerr << "arm64-metal mach-msg2-unhandled id=" << messageId
                                  << " dest=0x" << hexadecimal(registers[3] & 0xffffffffULL)
                                  << " size=" << (registers[2] >> 32) << " options=0x"
                                  << hexadecimal(registers[1]) << " pc=0x"
                                  << hexadecimal(registers[33]) << " reason=clock_get_time-shape\n";
                        registers[0] = 0x10000002;
                        registers[34] = 0;
                        continue;
                    }
                    if ((registers[2] >> 32) != sizeof(mach_msg_header_t) ||
                        registers[6] < 44) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }
                    auto* replyBuffer = guestMemory.buffer(registers[0], 44, true);
                    const auto* request = guestMemory.buffer(registers[0], sizeof(mach_msg_header_t));
                    mach_msg_header_t requestHeader{};
                    std::memcpy(&requestHeader, request, sizeof(requestHeader));
                    if (requestHeader.msgh_bits != 0x1513 ||
                        requestHeader.msgh_size != sizeof(mach_msg_header_t) ||
                        requestHeader.msgh_id != 1000 || requestHeader.msgh_remote_port != remote ||
                        requestHeader.msgh_local_port != reply ||
                        requestHeader.msgh_voucher_port != MACH_PORT_NULL) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }

                    struct ClockGetTimeErrorReply {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        kern_return_t returnCode;
                    };
                    struct ClockGetTimeReply {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        kern_return_t returnCode;
                        mach_timespec_t currentTime;
                    };
                    static_assert(sizeof(ClockGetTimeErrorReply) == 36);
                    static_assert(sizeof(ClockGetTimeReply) == 44);

                    ClockGetTimeReply response{};
                    response.header.msgh_bits = 0x1200;
                    response.header.msgh_size = sizeof(ClockGetTimeErrorReply);
                    response.header.msgh_remote_port = MACH_PORT_NULL;
                    response.header.msgh_local_port = reply;
                    response.header.msgh_voucher_port = MACH_PORT_NULL;
                    response.header.msgh_id = 1100;
                    response.ndr = NDR_record;
                    response.returnCode = KERN_FAILURE;
                    const auto nativeHost = mach_host_self();
                    clock_serv_t nativeClock = MACH_PORT_NULL;
                    response.returnCode = host_get_clock_service(nativeHost, service->second,
                                                                  &nativeClock);
                    if (response.returnCode == KERN_SUCCESS) {
                        response.returnCode = clock_get_time(nativeClock, &response.currentTime);
                    }
                    if (nativeClock != MACH_PORT_NULL) {
                        (void)mach_port_deallocate(mach_task_self(), nativeClock);
                    }
                    (void)mach_port_deallocate(mach_task_self(), nativeHost);
                    if (response.returnCode == KERN_SUCCESS) {
                        response.header.msgh_size = sizeof(response);
                        std::memcpy(replyBuffer, &response, sizeof(response));
                    } else {
                        std::memcpy(replyBuffer,
                                    &response, sizeof(ClockGetTimeErrorReply));
                    }
                    registers[0] = 0;
                    registers[34] = 0;
                    continue;
                }
                constexpr std::uint64_t machSendMessage = 0x00000001;
                constexpr std::uint64_t machReceiveMessage = 0x00000002;
                constexpr std::uint64_t machReceiveTimeout = 0x00000100;
                constexpr std::uint64_t mach64SendAny = 0x0000000800000000;
                constexpr std::uint64_t mach64SendMqCall = 0x0000000400000000;
                const auto options = registers[1];
                const auto remote = static_cast<std::uint32_t>(registers[3]);
                const auto receiveName = static_cast<std::uint32_t>(registers[5] >> 32);
                    const auto descriptorCount =
                        static_cast<std::uint32_t>(registers[5]);
                const bool sendRequested = (options & machSendMessage) != 0;
                const bool receiveRequested = (options & machReceiveMessage) != 0;
                const bool threadRemote = std::any_of(
                    guestThreadPortNames.begin(), guestThreadPortNames.end(),
                    [remote](const auto& entry) { return entry.second == remote; });
                const bool clockRemote = guestClockServiceIds.find(remote) !=
                                         guestClockServiceIds.end();
                const bool voucherRemote = machModels.is_voucher_port(remote);
                const bool bootstrapRemote = remote == guestMachBootstrapPort &&
                                             messageId == 0x400000cf;
                const bool specialRemote = remote == guestMachTaskPort ||
                                           remote == guestMachHostPort || clockRemote ||
                                           threadRemote || voucherRemote || bootstrapRemote;
                const bool knownKernelObjectShape = messageId == 200 || messageId == 3207 ||
                    messageId == 4822 || messageId == 8000 || messageId == 8001;
                const bool kernelObjectSend = sendRequested && specialRemote &&
                    !knownKernelObjectShape;
                const bool localSend = sendRequested && (!specialRemote || kernelObjectSend);
                const bool localReceive = receiveRequested && !sendRequested &&
                    receiveName != guestMachTaskPort && receiveName != guestMachHostPort;
                if (localSend || localReceive) {
                    constexpr std::uint32_t kernInvalidArgument = 4;
                    constexpr std::uint32_t machSendInvalidDestination = 0x10000003;
                    constexpr std::uint32_t machSendInvalidRight = 0x1000000a;
                    constexpr std::uint32_t machSendInvalidData = 0x10000002;
                    constexpr std::uint32_t machSendMessageTooSmall = 0x10000008;
                    constexpr std::uint32_t machReceiveInvalidName = 0x10004002;
                    constexpr std::uint32_t machReceiveTimedOut = 0x10004003;
                    constexpr std::uint32_t machReceiveTooLarge = 0x10004004;
                    constexpr std::uint32_t machMsgTypeCopySend = 19;
                    constexpr std::uint32_t machMsgComplex = 0x80000000;
                    constexpr std::uint32_t machPortDescriptorType = 0;
                    constexpr std::uint32_t machMsgTypeMoveSend = 0x11;
                    constexpr std::uint32_t machTrailerSize = sizeof(mach_msg_trailer_t);
                    constexpr std::uint64_t machReceiveLarge = 0x00000004;
                    constexpr std::uint64_t machReceiveLargeIdentity = 0x00000008;
                    constexpr std::uint64_t machReceiveInterrupt = 0x00000400;
                    constexpr std::uint64_t machReceiveVoucher = 0x00000800;
                    constexpr std::uint64_t machReceiveTrailerMask = 0xff000000;
                    constexpr std::uint64_t mach64MsgVector = 0x0000000100000000;
                    constexpr std::uint64_t mach64SendKernelObjectCall = 0x0000000200000000;
                    constexpr std::uint64_t allowedOptions = machSendMessage |
                        mach64SendKernelObjectCall |
                        machReceiveMessage | machReceiveTimeout | mach64SendAny | mach64SendMqCall |
                        machReceiveLarge | machReceiveLargeIdentity | machReceiveInterrupt |
                        machReceiveVoucher | machReceiveTrailerMask | mach64MsgVector;
                    const bool vectorMessage = (options & mach64MsgVector) != 0;
                    std::uint64_t receiveAddress = registers[0];
                    std::uint64_t receiveSize64 = registers[6];
                    if (vectorMessage && receiveRequested && !sendRequested) {
                        try {
                            const auto* vector = guestMemory.buffer(registers[0], 24);
                            std::uint64_t dataAddress = 0;
                            std::uint64_t vectorReceiveAddress = 0;
                            std::uint32_t vectorReceiveSize = 0;
                            std::memcpy(&dataAddress, vector, sizeof(dataAddress));
                            std::memcpy(&vectorReceiveAddress, vector + 8,
                                        sizeof(vectorReceiveAddress));
                            std::memcpy(&vectorReceiveSize, vector + 20,
                                        sizeof(vectorReceiveSize));
                            receiveAddress = vectorReceiveAddress != 0
                                ? vectorReceiveAddress : dataAddress;
                            receiveSize64 = vectorReceiveSize;
                        } catch (const std::out_of_range&) {
                            receiveAddress = 0;
                            receiveSize64 = 0;
                        }
                    }
                    const auto receiveSize = static_cast<std::uint32_t>(receiveSize64);
                    const std::uint32_t trailerSize = std::max<std::uint32_t>(
                        machTrailerSize,
                        REQUESTED_TRAILER_SIZE(static_cast<mach_msg_option_t>(options)));
                    std::uint64_t result = 0;
                    if ((options & ~allowedOptions) != 0 ||
                        (!sendRequested && descriptorCount != 0) ||
                        registers[6] >> 32 != 0 ||
                        (receiveRequested && !sendRequested && receiveAddress == 0)) {
                        result = kernInvalidArgument;
                        if (std::getenv("ARM64_METAL_TRACE_MACH_MSG") != nullptr) {
                            std::cerr << "mach-msg2 invalid args options=0x" << hexadecimal(options)
                                      << " x2=0x" << hexadecimal(registers[2])
                                      << " x3=0x" << hexadecimal(registers[3])
                                      << " x4=0x" << hexadecimal(registers[4])
                                      << " x5=0x" << hexadecimal(registers[5])
                                      << " x6=0x" << hexadecimal(registers[6])
                                      << " x7=0x" << hexadecimal(registers[7]) << '\n';
                        }
                    } else if (vectorMessage && sendRequested) {
                        survey("msgh_id", messageId, "vector-send");
                        std::cerr << "arm64-metal mach-msg2-unhandled id=" << messageId
                                  << " dest=0x" << hexadecimal(remote) << " size="
                                  << (registers[2] >> 32) << " options=0x" << hexadecimal(options)
                                  << " pc=0x" << hexadecimal(registers[33])
                                  << " reason=vector-send\n";
                        result = machSendInvalidData;
                    } else if (kernelObjectSend) {
                        constexpr std::uint32_t machSendInvalidReply = 0x10000009;
                        const auto sendRight = guestSendRightUrefs.find(remote);
                        const auto sendSize = registers[2] >> 32;
                        const auto bits = static_cast<std::uint32_t>(registers[2]);
                        const auto replyName = static_cast<std::uint32_t>(registers[3] >> 32);
                        const char* role = remote == guestMachTaskPort ? "guest-task"
                            : remote == guestMachHostPort ? "guest-host"
                            : threadRemote ? "guest-thread"
                            : bootstrapRemote ? "guest-bootstrap" : "guest-clock";
                        if (sendRight == guestSendRightUrefs.end() || sendRight->second == 0) {
                            result = machSendInvalidDestination;
                        } else if ((bits & 0x1f) != machMsgTypeCopySend ||
                                   static_cast<std::uint32_t>(registers[4]) != 0) {
                            result = machSendInvalidRight;
                        } else if (sendSize < sizeof(mach_msg_header_t) || (sendSize & 3) != 0) {
                            result = machSendMessageTooSmall;
                        } else if (replyName != 0 &&
                                   guestReceiveRights.find(replyName) ==
                                       guestReceiveRights.end()) {
                            result = machSendInvalidReply;
                        } else if (!guestMemory.is_guest_readable(registers[0],
                                                                  sizeof(mach_msg_header_t))) {
                            result = machSendInvalidData;
                        } else {
                            const bool inRoutineRange = kernelObjectRoutineExists(
                                static_cast<std::int32_t>(messageId));
                            constexpr std::int32_t migBadId = -303;
                            constexpr std::int32_t kernNotSupported = 46;
                            constexpr std::int32_t kernInvalidArgumentCode = 4;
                            constexpr std::int32_t kernInvalidName = 15;
                            constexpr std::int32_t kernInvalidRight = 17;
                            constexpr std::int32_t kernInvalidValue = 18;
                            std::int32_t replyCode = inRoutineRange ? kernNotSupported : migBadId;
                            std::vector<std::uint8_t> replyExtra;
                            bool modeled = false;
                            std::vector<std::uint8_t> requestBytes;
                            if (guestMemory.is_guest_readable(registers[0], sendSize)) {
                                const auto* raw = guestMemory.buffer(registers[0], sendSize);
                                requestBytes.assign(raw, raw + sendSize);
                            }
                            const auto requestWord = [&](std::size_t offset) {
                                std::uint32_t value = 0;
                                if (offset + sizeof(value) <= requestBytes.size()) {
                                    std::memcpy(&value, requestBytes.data() + offset,
                                                sizeof(value));
                                }
                                return value;
                            };
                            const auto receiveRightFor = [&](std::uint32_t name,
                                                              std::int32_t& error) {
                                const auto right = guestReceiveRights.find(name);
                                if (right != guestReceiveRights.end()) return &right->second;
                                const auto held = guestSendRightUrefs.find(name);
                                const bool known = name != 0 &&
                                    ((held != guestSendRightUrefs.end() && held->second != 0) ||
                                     guestDeadPortNames.count(name) != 0 ||
                                     name == std::numeric_limits<std::uint32_t>::max());
                                error = known ? kernInvalidRight : kernInvalidName;
                                return static_cast<GuestReceiveRight*>(nullptr);
                            };
                            if (remote == guestMachHostPort && messageId == 412 &&
                                sendSize == 40) {
                                replyCode = kernInvalidArgumentCode;
                                modeled = true;
                            } else if (remote == guestMachTaskPort && messageId == 3228 &&
                                       sendSize == 36) {
                                std::int32_t error = 0;
                                auto* right = receiveRightFor(requestWord(32), error);
                                replyCode = right == nullptr ? error : 0;
                                if (right != nullptr) {
                                    replyExtra.resize(sizeof(std::uint64_t));
                                    std::memcpy(replyExtra.data(), &right->context,
                                                sizeof(std::uint64_t));
                                }
                                modeled = true;
                            } else if (remote == guestMachTaskPort && messageId == 3229 &&
                                       sendSize == 44) {
                                std::int32_t error = 0;
                                auto* right = receiveRightFor(requestWord(32), error);
                                replyCode = right == nullptr ? error : 0;
                                if (right != nullptr) {
                                    std::memcpy(&right->context, requestBytes.data() + 36,
                                                sizeof(std::uint64_t));
                                }
                                modeled = true;
                            } else if (remote == guestMachTaskPort && messageId == 3218 &&
                                       sendSize == 48 && requestWord(36) == MACH_PORT_LIMITS_INFO &&
                                       requestWord(40) == MACH_PORT_LIMITS_INFO_COUNT) {
                                std::int32_t error = 0;
                                auto* right = receiveRightFor(requestWord(32), error);
                                const auto qlimit = requestWord(44);
                                if (right == nullptr) {
                                    replyCode = error;
                                } else if (qlimit > MACH_PORT_QLIMIT_MAX) {
                                    replyCode = kernInvalidValue;
                                } else {
                                    right->qlimit = qlimit;
                                    replyCode = 0;
                                }
                                modeled = true;
                            }
                            std::uint32_t modelSendResult = 0;
                            bool modelComplex = false;
                            bool modelRaw = false;
                            std::vector<std::uint32_t> modelRights;
                            if (!modeled) {
                                mach_models::Request modelRequest;
                                modelRequest.object = remote == guestMachTaskPort
                                    ? mach_models::Object::Task
                                    : remote == guestMachHostPort ? mach_models::Object::Host
                                    : threadRemote ? mach_models::Object::Thread
                                    : voucherRemote ? mach_models::Object::Voucher
                                    : mach_models::Object::Clock;
                                modelRequest.remote = remote;
                                modelRequest.id = static_cast<std::int32_t>(messageId);
                                modelRequest.currentThreadId = guestScheduler.current_id();
                                for (const auto& entry : guestThreadPortNames) {
                                    if (entry.second == remote) modelRequest.threadId = entry.first;
                                }
                                modelRequest.data = requestBytes.data();
                                modelRequest.size = requestBytes.size();
                                mach_models::Reply modelReply;
                                const bool modelHandled = bootstrapRemote
                                    ? machModels.bootstrap_request(
                                          requestBytes.data(), requestBytes.size(),
                                          static_cast<std::uint32_t>(::getpid()), modelReply)
                                    : machModels.handle(modelRequest, modelReply);
                                if (modelHandled) {
                                    modelRaw = modelReply.raw;
                                    modeled = true;
                                    modelSendResult = modelReply.sendResult;
                                    replyCode = modelReply.code;
                                    modelComplex = modelReply.complex;
                                    modelRights = std::move(modelReply.rights);
                                    replyExtra = (modelComplex || modelRaw) ? std::move(modelReply.body)
                                                              : std::move(modelReply.extra);
                                }
                            }
                            if (!modeled) {
                                survey("msgh_id", messageId,
                                       std::string(inRoutineRange ? "KERN_NOT_SUPPORTED:"
                                                                  : "MIG_BAD_ID:") + role);
                                std::cerr << "arm64-metal mach-msg2-unhandled id=" << messageId
                                          << " dest=" << role << ":0x" << hexadecimal(remote) << " size=" << sendSize
                                          << " options=0x" << hexadecimal(options)
                                          << " pc=0x" << hexadecimal(registers[33])
                                          << " reply=" << (inRoutineRange ? "KERN_NOT_SUPPORTED"
                                                                          : "MIG_BAD_ID")
                                          << '\n';
                            }
                            if (modelSendResult != 0) {
                                result = modelSendResult;
                            } else if (replyName != 0 && (modelComplex || modelRaw)) {
                                GuestLocalMessage message{
                                    static_cast<std::uint32_t>(sizeof(mach_msg_header_t) +
                                        replyExtra.size()),
                                    modelComplex ? MACH_MSGH_BITS_COMPLEX : 0U, 0, 0,
                                    bootstrapRemote ? 0x20000000
                                        : static_cast<std::int32_t>(messageId + 100), {}, {},
                                    MACH_MSG_TYPE_MOVE_SEND_ONCE << 8};
                                message.body = std::move(replyExtra);
                                message.kernelRights = true;
                                for (const auto name : modelRights) {
                                    message.transferredSendRights.push_back(
                                        {name, MACH_MSG_TYPE_MOVE_SEND});
                                }
                                guestMachMessageQueues[replyName].push_back(std::move(message));
                                deliverGuestMachMessage(replyName);
                            } else if (replyName != 0) {
                                struct KernelObjectErrorBody {
                                    NDR_record_t ndr;
                                    std::int32_t returnCode;
                                };
                                static_assert(sizeof(KernelObjectErrorBody) == 12);
                                KernelObjectErrorBody errorBody{NDR_record, replyCode};
                                GuestLocalMessage message{
                                    static_cast<std::uint32_t>(sizeof(mach_msg_header_t) +
                                        sizeof(errorBody) + replyExtra.size()),
                                    0, 0, 0, static_cast<std::int32_t>(messageId + 100), {}, {},
                                    MACH_MSG_TYPE_MOVE_SEND_ONCE << 8};
                                message.body.resize(sizeof(errorBody) + replyExtra.size());
                                std::memcpy(message.body.data(), &errorBody, sizeof(errorBody));
                                if (!replyExtra.empty()) {
                                    std::memcpy(message.body.data() + sizeof(errorBody),
                                                replyExtra.data(), replyExtra.size());
                                }
                                guestMachMessageQueues[replyName].push_back(std::move(message));
                                deliverGuestMachMessage(replyName);
                            }
                        }
                    } else if (sendRequested) {
                        const auto sendRight = guestSendRightUrefs.find(remote);
                        if (sendRight == guestSendRightUrefs.end() || sendRight->second == 0 ||
                            guestReceiveRights.find(remote) == guestReceiveRights.end()) {
                            if (sendRight != guestSendRightUrefs.end() && sendRight->second != 0) {
                                survey("msgh_id", messageId, "send-right-without-receive-right");
                                std::cerr << "arm64-metal mach-msg2-unhandled id=" << messageId
                                          << " dest=send-right-without-receive-right size="
                                          << (registers[2] >> 32) << " options=0x"
                                          << hexadecimal(options) << " pc=0x"
                                          << hexadecimal(registers[33]) << '\n';
                            }
                            result = machSendInvalidDestination;
                        } else if ((static_cast<std::uint32_t>(registers[2]) & 0x1f) !=
                                       machMsgTypeCopySend ||
                                   ((static_cast<std::uint32_t>(registers[2]) >> 8) & 0x1f) != 0 ||
                                   static_cast<std::uint32_t>(registers[4]) != 0 ||
                                   static_cast<std::uint32_t>(registers[3] >> 32) != 0) {
                            result = machSendInvalidRight;
                        } else {
                            const auto sendSize = registers[2] >> 32;
                            const auto bits = static_cast<std::uint32_t>(registers[2]);
                            const bool complex = (bits & machMsgComplex) != 0;
                            constexpr auto descriptorStart = sizeof(mach_msg_header_t) +
                                sizeof(mach_msg_body_t);
                            if (sendSize < sizeof(mach_msg_header_t) || (sendSize & 3) != 0) {
                                result = machSendMessageTooSmall;
                            } else if (complex != (descriptorCount != 0)) {
                                result = machSendInvalidData;
                            } else if (complex &&
                                       (sendSize < descriptorStart ||
                                        descriptorCount >
                                            (sendSize - descriptorStart) /
                                                sizeof(mach_msg_port_descriptor_t))) {
                                result = machSendMessageTooSmall;
                            } else {
                                try {
                                    const auto* bytes = guestMemory.buffer(registers[0], sendSize);
                                    mach_msg_header_t header{};
                                    std::memcpy(&header, bytes, sizeof(header));
                                    std::vector<GuestLocalPortRightTransfer> transferredSendRights;
                                    transferredSendRights.reserve(descriptorCount);
                                    if (header.msgh_bits != bits || header.msgh_size != sendSize ||
                                        header.msgh_remote_port != remote ||
                                        header.msgh_local_port != 0 ||
                                        header.msgh_voucher_port != 0 ||
                                        header.msgh_id != static_cast<mach_msg_id_t>(messageId)) {
                                        result = machSendInvalidData;
                                    } else if (complex) {
                                        mach_msg_body_t body{};
                                        std::memcpy(&body, bytes + sizeof(header), sizeof(body));
                                        if (body.msgh_descriptor_count != descriptorCount) {
                                            result = machSendInvalidData;
                                        } else {
                                            for (std::uint32_t index = 0;
                                                 index < descriptorCount; ++index) {
                                                mach_msg_port_descriptor_t descriptor{};
                                                const auto descriptorOffset = sizeof(header) +
                                                    sizeof(body) + static_cast<std::size_t>(index) *
                                                        sizeof(descriptor);
                                                std::memcpy(&descriptor, bytes + descriptorOffset,
                                                            sizeof(descriptor));
                                                if (descriptor.pad1 != 0 || descriptor.pad2 != 0 ||
                                                    (descriptor.disposition != machMsgTypeMoveSend &&
                                                     descriptor.disposition != machMsgTypeCopySend) ||
                                                    descriptor.name == 0 ||
                                                    descriptor.name ==
                                                        std::numeric_limits<std::uint32_t>::max()) {
                                                    result = machSendInvalidData;
                                                    break;
                                                }
                                                if (descriptor.type != machPortDescriptorType) {
                                                    result = machSendMessageTooSmall;
                                                    break;
                                                }
                                                transferredSendRights.push_back({descriptor.name,
                                                    descriptor.disposition});
                                            }
                                        }
                                    }
                                    if (result == 0) {
                                        for (const auto& transfer : transferredSendRights) {
                                            const auto right = guestSendRightUrefs.find(transfer.name);
                                            if (right == guestSendRightUrefs.end() ||
                                                right->second == 0 ||
                                                guestReceiveRights.find(transfer.name) ==
                                                    guestReceiveRights.end()) {
                                                result = machSendInvalidRight;
                                                break;
                                            }
                                            if (transfer.disposition == machMsgTypeMoveSend) {
                                                --right->second;
                                            }
                                        }
                                    }
                                    if (result != 0) {
                                        registers[0] = result;
                                        registers[34] = 0;
                                        continue;
                                    }
                                    GuestLocalMessage message{
                                        static_cast<std::uint32_t>(sendSize), bits,
                                        static_cast<std::uint32_t>(registers[3] >> 32),
                                        static_cast<std::uint32_t>(registers[4]),
                                        static_cast<std::int32_t>(messageId), {},
                                        std::move(transferredSendRights)};
                                    message.body.assign(bytes + sizeof(mach_msg_header_t),
                                                        bytes + sendSize);
                                    guestMachMessageQueues[remote].push_back(std::move(message));
                                    progressTrace.event("mach-message-send",
                                        guestScheduler.current_id(),
                                        static_cast<std::uint32_t>(messageId));
                                    deliverGuestMachMessage(remote);
                                } catch (const std::exception&) {
                                    result = machSendInvalidData;
                                }
                            }
                        }
                    }
                    if (result == 0 && receiveRequested) {
                        if (guestReceiveRights.find(receiveName) == guestReceiveRights.end()) {
                            result = machReceiveInvalidName;
                        } else {
                            auto sourcePort = receiveName;
                            if (guestReceiveRights.at(receiveName).portSet) {
                                if (const auto member = readyPortSetMember(receiveName)) {
                                    sourcePort = *member;
                                }
                            }
                            auto& queue = guestMachMessageQueues[sourcePort];
                            if (queue.empty()) {
                                if ((options & machReceiveTimeout) != 0) {
                                    const auto timeoutMilliseconds = registers[7];
                                    if (timeoutMilliseconds == 0) {
                                        result = machReceiveTimedOut;
                                    } else {
                                        const auto now = monotonicNanoseconds();
                                        const auto timeoutNanoseconds = timeoutMilliseconds >
                                                std::numeric_limits<std::uint64_t>::max() /
                                                    1'000'000ULL
                                            ? std::numeric_limits<std::uint64_t>::max()
                                            : timeoutMilliseconds * 1'000'000ULL;
                                        const auto deadline = now + std::min(
                                            timeoutNanoseconds,
                                            std::numeric_limits<std::uint64_t>::max() - now);
                                        const auto threadId = guestScheduler.current_id();
                                        if (!guestScheduler.wait_mach_receive(receiveName,
                                                                              deadline)) {
                                            result = kernInvalidArgument;
                                        } else {
                                            guestMachBlockedReceives[threadId] =
                                                GuestMachBlockedReceive{receiveName, receiveAddress,
                                                                        receiveSize, trailerSize};
                                            guestMachReceiveWaiters[receiveName].push_back(threadId);
                                            progressTrace.event("mach-receive-wait", threadId,
                                                                receiveName);
                                            progressTrace.event("mach-receive-deadline", now,
                                                                deadline);
                                            std::vector<std::uint64_t> liveRegisters(
                                                registers, registers + stateBuffer.size() /
                                                    sizeof(std::uint64_t));
                                            (void)syncThreadSwitch(false, liveRegisters);
                                            continue;
                                        }
                                    }
                                } else {
                                    const auto threadId = guestScheduler.current_id();
                                    if (!guestScheduler.wait_mach_receive(receiveName,
                                                                          std::nullopt)) {
                                        result = kernInvalidArgument;
                                    } else {
                                        guestMachBlockedReceives[threadId] =
                                            GuestMachBlockedReceive{receiveName, receiveAddress,
                                                                    receiveSize, trailerSize};
                                        guestMachReceiveWaiters[receiveName].push_back(threadId);
                                        progressTrace.event("mach-receive-wait", threadId,
                                                            receiveName);
                                        std::vector<std::uint64_t> liveRegisters(
                                            registers, registers + stateBuffer.size() /
                                                sizeof(std::uint64_t));
                                        (void)syncThreadSwitch(false, liveRegisters);
                                        continue;
                                    }
                                }
                            } else {
                                const auto messageSize = queue.front().size;
                                const auto outputSize = static_cast<std::uint64_t>(messageSize) +
                                                        trailerSize;
                                if (receiveSize < outputSize) {
                                    result = machReceiveTooLarge;
                                    if ((options & machReceiveLarge) == 0) {
                                        queue.pop_front();
                                    } else if (receiveSize >= sizeof(mach_msg_header_t)) {
                                        try {
                                            std::memcpy(guestMemory.buffer(receiveAddress + 4,
                                                            sizeof(messageSize), true),
                                                        &messageSize, sizeof(messageSize));
                                        } catch (const std::out_of_range&) {
                                        }
                                    }
                                } else {
                                    result = copyGuestMachMessage(queue.front(), sourcePort,
                                        receiveAddress, receiveSize, trailerSize);
                                    if (result == 0) {
                                        if (!restoreGuestTransferredSendRight(queue.front())) {
                                            result = machReceiveInvalidData;
                                        }
                                    } else if (result == machReceiveInvalidData) {
                                        (void)restoreTransferredRightAfterInvalidReceive(
                                            queue.front());
                                    }
                                    if (result == 0 || result == machReceiveInvalidData) {
                                        queue.pop_front();
                                    }
                                }
                            }
                        }
                    }
                    registers[0] = result;
                    registers[34] = 0;
                    continue;
                }
                if (messageId == 200) {
                    constexpr std::uint32_t machSendInvalidDestination = 0x10000003;
                    constexpr std::uint32_t machSendInvalidData = 0x1000000a;
                    const auto remote = static_cast<std::uint32_t>(registers[3]);
                    const auto hostSendRight = guestSendRightUrefs.find(remote);
                    if (remote != guestMachHostPort ||
                        hostSendRight == guestSendRightUrefs.end() ||
                        hostSendRight->second == 0) {
                        registers[0] = machSendInvalidDestination;
                        registers[34] = 0;
                        continue;
                    }
                    if (registers[1] != 0x2'0000'0003ULL ||
                        static_cast<std::uint32_t>(registers[2]) != 0x1513 ||
                        static_cast<std::uint32_t>(registers[4]) != 0 ||
                        static_cast<std::uint32_t>(registers[5]) != 0 ||
                        (registers[5] >> 32) != reply ||
                        registers[6] > std::numeric_limits<std::uint32_t>::max() ||
                        (registers[6] >> 32) != 0 || registers[7] != 0 ||
                        guestReceiveRights.find(reply) == guestReceiveRights.end()) {
                        survey("msgh_id", messageId, "host_info-shape");
                        std::cerr << "arm64-metal mach-msg2-unhandled id=" << messageId
                                  << " dest=0x" << hexadecimal(registers[3] & 0xffffffffULL)
                                  << " size=" << (registers[2] >> 32) << " options=0x"
                                  << hexadecimal(registers[1]) << " pc=0x"
                                  << hexadecimal(registers[33]) << " reason=host_info-shape\n";
                        registers[0] = 0x10000002;
                        registers[34] = 0;
                        continue;
                    }
                    if ((registers[2] >> 32) != 40 || registers[6] < 36) {
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }
                    const auto* request = guestMemory.buffer(registers[0], 40);
                    std::uint32_t flavor = 0;
                    std::uint32_t inputCount = 0;
                    std::memcpy(&flavor, request + 32, sizeof(flavor));
                    std::memcpy(&inputCount, request + 36, sizeof(inputCount));

                    struct HostInfoErrorReply {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        kern_return_t returnCode;
                    };
                    struct HostInfoReply {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        kern_return_t returnCode;
                        mach_msg_type_number_t count;
                        integer_t info[68];
                    };
                    static_assert(sizeof(HostInfoErrorReply) == 36);
                    static_assert(offsetof(HostInfoReply, info) == 40);

                    const auto prepareHeader = [reply](mach_msg_header_t& header,
                                                       std::uint32_t size) {
                        header.msgh_bits = 0x1200;
                        header.msgh_size = size;
                        header.msgh_remote_port = MACH_PORT_NULL;
                        header.msgh_local_port = reply;
                        header.msgh_voucher_port = MACH_PORT_NULL;
                        header.msgh_id = 300;
                    };
                    HostInfoReply response{};
                    prepareHeader(response.header, sizeof(HostInfoErrorReply));
                    response.ndr = NDR_record;
                    response.returnCode = KERN_INVALID_ARGUMENT;
                    if (inputCount <= std::size(response.info)) {
                        mach_msg_type_number_t outputCount = inputCount;
                        const auto nativeHost = mach_host_self();
                        response.returnCode = host_info(
                            nativeHost, static_cast<host_flavor_t>(flavor),
                            response.info, &outputCount);
                        (void)mach_port_deallocate(mach_task_self(), nativeHost);
                        if (response.returnCode == KERN_SUCCESS) {
                            const auto responseSize = offsetof(HostInfoReply, info) +
                                static_cast<std::size_t>(outputCount) * sizeof(integer_t);
                            if (outputCount > std::size(response.info) ||
                                responseSize > registers[6] ||
                                responseSize > std::numeric_limits<std::uint32_t>::max()) {
                                registers[0] = machSendInvalidData;
                                registers[34] = 0;
                                continue;
                            }
                            response.count = outputCount;
                            prepareHeader(response.header,
                                          static_cast<std::uint32_t>(responseSize));
                            std::memcpy(guestMemory.buffer(registers[0], responseSize, true),
                                        &response, responseSize);
                        } else {
                            std::memcpy(guestMemory.buffer(registers[0],
                                            sizeof(HostInfoErrorReply), true),
                                        &response, sizeof(HostInfoErrorReply));
                        }
                    } else {
                        response.returnCode = KERN_FAILURE;
                        std::memcpy(guestMemory.buffer(registers[0],
                                        sizeof(HostInfoErrorReply), true),
                                    &response, sizeof(HostInfoErrorReply));
                    }
                    registers[0] = 0;
                    registers[34] = 0;
                    continue;
                }
                if (messageId == 3207) {
                    constexpr std::uint32_t machSendInvalidDestination = 0x10000003;
                    constexpr kern_return_t kernInvalidTask = 16;
                    constexpr kern_return_t kernInvalidName = 15;
                    constexpr kern_return_t kernInvalidValue = 18;
                    constexpr std::uint32_t machPortNull = 0;
                    constexpr std::uint32_t machPortDead =
                        std::numeric_limits<std::uint32_t>::max();

                    struct MachPortGetRefsErrorReply {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        kern_return_t returnCode;
                    };
                    struct MachPortGetRefsReply {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        kern_return_t returnCode;
                        mach_port_urefs_t refs;
                    };
                    static_assert(sizeof(MachPortGetRefsErrorReply) == 36);
                    static_assert(sizeof(MachPortGetRefsReply) == 40);

                    const auto remote = static_cast<std::uint32_t>(registers[3]);
                    const auto hostSendRight = guestSendRightUrefs.find(remote);
                    if ((remote != guestMachTaskPort && remote != guestMachHostPort) ||
                        hostSendRight == guestSendRightUrefs.end() ||
                        hostSendRight->second == 0) {
                        registers[0] = machSendInvalidDestination;
                        registers[34] = 0;
                        continue;
                    }
                    if (registers[1] != 0x2'0000'0003ULL ||
                        static_cast<std::uint32_t>(registers[2]) != 0x1513 ||
                        (registers[2] >> 32) != 40 ||
                        static_cast<std::uint32_t>(registers[4]) != 0 ||
                        (registers[4] >> 32) != messageId ||
                        static_cast<std::uint32_t>(registers[5]) != 0 ||
                        (registers[5] >> 32) != reply ||
                        registers[6] > std::numeric_limits<std::uint32_t>::max() ||
                        (registers[6] >> 32) != 0 || registers[7] != 0 ||
                        guestReceiveRights.find(reply) == guestReceiveRights.end()) {
                        survey("msgh_id", messageId, "mach_port_get_refs-shape");
                        std::cerr << "arm64-metal mach-msg2-unhandled id=" << messageId
                                  << " dest=0x" << hexadecimal(registers[3] & 0xffffffffULL)
                                  << " size=" << (registers[2] >> 32) << " options=0x"
                                  << hexadecimal(registers[1]) << " pc=0x"
                                  << hexadecimal(registers[33]) << " reason=mach_port_get_refs-shape\n";
                        registers[0] = 0x10000002;
                        registers[34] = 0;
                        continue;
                    }
                    const auto* request = guestMemory.buffer(registers[0], 40);
                    mach_msg_header_t requestHeader{};
                    NDR_record_t requestNdr{};
                    mach_port_name_t name = 0;
                    mach_port_right_t right = MACH_PORT_RIGHT_NUMBER;
                    std::memcpy(&requestHeader, request, sizeof(requestHeader));
                    std::memcpy(&requestNdr, request + sizeof(requestHeader),
                                sizeof(requestNdr));
                    std::memcpy(&name, request + sizeof(requestHeader) + sizeof(requestNdr),
                                sizeof(name));
                    std::memcpy(&right, request + sizeof(requestHeader) +
                                sizeof(requestNdr) + sizeof(name), sizeof(right));
                    if (requestHeader.msgh_bits != 0x1513 || requestHeader.msgh_size != 40 ||
                        requestHeader.msgh_id != static_cast<mach_msg_id_t>(messageId) ||
                        requestHeader.msgh_remote_port != remote ||
                        requestHeader.msgh_local_port != reply ||
                        requestHeader.msgh_voucher_port != MACH_PORT_NULL ||
                        std::memcmp(&requestNdr, &NDR_record, sizeof(requestNdr)) != 0) {
                        registers[0] = 0x1000000a; // MACH_SEND_INVALID_DATA
                        registers[34] = 0;
                        continue;
                    }

                    kern_return_t result = KERN_SUCCESS;
                    mach_port_urefs_t refs = 0;
                    if (remote == guestMachHostPort) {
                        result = kernInvalidTask;
                    } else if (right >= MACH_PORT_RIGHT_NUMBER) {
                        result = kernInvalidValue;
                    } else if (name == machPortNull || name == machPortDead) {
                        if (right == MACH_PORT_RIGHT_SEND ||
                            right == MACH_PORT_RIGHT_SEND_ONCE) {
                            refs = 1;
                        } else {
                            result = kernInvalidName;
                        }
                    } else {
                        const auto receiveRight = guestReceiveRights.find(name);
                        const auto sendRight = guestSendRightUrefs.find(name);
                        const bool hasReceive = receiveRight != guestReceiveRights.end();
                        const bool hasSend = sendRight != guestSendRightUrefs.end() &&
                                             sendRight->second != 0;
                        const bool isDead = guestDeadPortNames.count(name) != 0;
                        if (!hasReceive && !hasSend && !isDead) {
                            result = kernInvalidName;
                        } else if (right == MACH_PORT_RIGHT_SEND) {
                            refs = !isDead && hasSend ? sendRight->second : 0;
                        } else if (right == MACH_PORT_RIGHT_RECEIVE) {
                            refs = hasReceive ? 1U : 0U;
                        } else if (right == MACH_PORT_RIGHT_DEAD_NAME) {
                            refs = isDead && hasSend ? sendRight->second : 0;
                        }
                    }

                    const auto responseSize = result == KERN_SUCCESS
                        ? sizeof(MachPortGetRefsReply) : sizeof(MachPortGetRefsErrorReply);
                    if (registers[6] < responseSize) {
                        registers[0] = 0x1000000a; // MACH_SEND_INVALID_DATA
                        registers[34] = 0;
                        continue;
                    }
                    auto* replyBuffer = guestMemory.buffer(registers[0], responseSize, true);
                    MachPortGetRefsErrorReply errorReply{};
                    errorReply.header.msgh_bits = 0x1200;
                    errorReply.header.msgh_size = sizeof(errorReply);
                    errorReply.header.msgh_remote_port = MACH_PORT_NULL;
                    errorReply.header.msgh_local_port = reply;
                    errorReply.header.msgh_voucher_port = MACH_PORT_NULL;
                    errorReply.header.msgh_id = messageId + 100;
                    errorReply.ndr = NDR_record;
                    errorReply.returnCode = result;
                    if (result == KERN_SUCCESS) {
                        MachPortGetRefsReply response{};
                        response.header = errorReply.header;
                        response.header.msgh_size = sizeof(response);
                        response.ndr = NDR_record;
                        response.returnCode = KERN_SUCCESS;
                        response.refs = refs;
                        std::memcpy(replyBuffer, &response, sizeof(response));
                    } else {
                        std::memcpy(replyBuffer, &errorReply, sizeof(errorReply));
                    }
                    registers[0] = 0;
                    registers[34] = 0;
                    continue;
                }
                if (messageId == 4822) {
                    constexpr std::uint32_t machSendInvalidDestination = 0x10000003;
                    constexpr std::uint32_t machSendInvalidData = 0x1000000a;
                    constexpr kern_return_t kernInvalidTask = 16;
                    constexpr kern_return_t kernInvalidArgument = 4;
                    constexpr kern_return_t kernNoSpace = 3;
                    constexpr kern_return_t vmResourceShortage =
                        static_cast<kern_return_t>(0x20004006U);
                    constexpr std::uint32_t requestSize = 40;
                    constexpr std::uint32_t replySize = 36;

                    struct DeferredReclamationErrorReply {
                        mach_msg_header_t header;
                        NDR_record_t ndr;
                        kern_return_t returnCode;
                    };
                    static_assert(sizeof(DeferredReclamationErrorReply) == replySize);

                    const auto remote = static_cast<std::uint32_t>(registers[3]);
                    const auto reply = static_cast<std::uint32_t>(registers[5] >> 32);
                    const auto taskSendRight = guestSendRightUrefs.find(remote);
                    if (taskSendRight == guestSendRightUrefs.end() ||
                        taskSendRight->second == 0) {
                        registers[0] = machSendInvalidDestination;
                        registers[34] = 0;
                        continue;
                    }
                    if (registers[1] != 0x2'0000'0003ULL ||
                        static_cast<std::uint32_t>(registers[2]) != 0x1513 ||
                        (registers[2] >> 32) != requestSize ||
                        (registers[3] >> 32) != reply ||
                        static_cast<std::uint32_t>(registers[4]) != 0 ||
                        (registers[4] >> 32) != messageId ||
                        static_cast<std::uint32_t>(registers[5]) != 0 ||
                        registers[6] < replySize ||
                        registers[6] > std::numeric_limits<std::uint32_t>::max() ||
                        (registers[6] >> 32) != 0 || registers[7] != 0 ||
                        guestReceiveRights.find(reply) == guestReceiveRights.end()) {
                        if (progressTrace.enabled()) {
                            std::cerr << "arm64-metal trace mach-vm-reclaim-reject"
                                      << " reason=scalar-shape pc=0x"
                                      << hexadecimal(registers[33])
                                      << " r1=0x" << hexadecimal(registers[1])
                                      << " r2=0x" << hexadecimal(registers[2])
                                      << " r3=0x" << hexadecimal(registers[3])
                                      << " r4=0x" << hexadecimal(registers[4])
                                      << " r5=0x" << hexadecimal(registers[5])
                                      << " r6=0x" << hexadecimal(registers[6])
                                      << " r7=0x" << hexadecimal(registers[7]) << '\n';
                        }
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }

                    const auto* request = guestMemory.buffer(registers[0], requestSize);
                    mach_msg_header_t requestHeader{};
                    NDR_record_t requestNdr{};
                    std::uint32_t length = 0;
                    std::uint32_t maxLength = 0;
                    std::memcpy(&requestHeader, request, sizeof(requestHeader));
                    std::memcpy(&requestNdr, request + sizeof(requestHeader),
                                sizeof(requestNdr));
                    std::memcpy(&length, request + sizeof(requestHeader) +
                                sizeof(requestNdr), sizeof(length));
                    std::memcpy(&maxLength, request + sizeof(requestHeader) +
                                sizeof(requestNdr) + sizeof(length), sizeof(maxLength));
                    if (requestHeader.msgh_bits != 0x1513 ||
                        requestHeader.msgh_remote_port != remote ||
                        requestHeader.msgh_local_port != reply ||
                        requestHeader.msgh_voucher_port != MACH_PORT_NULL ||
                        requestHeader.msgh_id != static_cast<mach_msg_id_t>(messageId) ||
                        std::memcmp(&requestNdr, &NDR_record, sizeof(requestNdr)) != 0) {
                        if (progressTrace.enabled()) {
                            std::cerr << "arm64-metal trace mach-vm-reclaim-reject"
                                      << " reason=request-header pc=0x"
                                      << hexadecimal(registers[33])
                                      << " bits=0x" << hexadecimal(requestHeader.msgh_bits)
                                      << " remote=0x" << hexadecimal(
                                             requestHeader.msgh_remote_port)
                                      << " local=0x" << hexadecimal(
                                             requestHeader.msgh_local_port)
                                      << " voucher=0x" << hexadecimal(
                                             requestHeader.msgh_voucher_port)
                                      << " id=" << requestHeader.msgh_id << '\n';
                        }
                        registers[0] = machSendInvalidData;
                        registers[34] = 0;
                        continue;
                    }

                    kern_return_t result = KERN_SUCCESS;
                    if (remote != guestMachTaskPort) {
                        result = kernInvalidTask;
                    } else if (length == 0 || maxLength == 0 || length > maxLength) {
                        result = kernInvalidArgument;
                    } else if (maxLength > 0x7ffff8U) {
                        result = kernNoSpace;
                    } else {
                        // Native task-port oracle returns VM resource shortage for valid
                        // requests when deferred reclamation is unavailable.
                        result = vmResourceShortage;
                    }

                    DeferredReclamationErrorReply response{};
                    response.header.msgh_bits = 0x1200;
                    response.header.msgh_size = replySize;
                    response.header.msgh_remote_port = MACH_PORT_NULL;
                    response.header.msgh_local_port = reply;
                    response.header.msgh_voucher_port = MACH_PORT_NULL;
                    response.header.msgh_id = messageId + 100;
                    response.ndr = NDR_record;
                    response.returnCode = result;
                    std::memcpy(guestMemory.buffer(registers[0], replySize, true),
                                &response, sizeof(response));
                    if (progressTrace.enabled()) {
                        std::cerr << "arm64-metal trace mach-vm-reclaim-reply"
                                  << " pc=0x" << hexadecimal(registers[33])
                                  << " len=" << length << " max=" << maxLength
                                  << " result=0x" << hexadecimal(
                                         static_cast<std::uint32_t>(result)) << '\n';
                    }
                    registers[0] = 0;
                    registers[34] = 0;
                    continue;
                }
                if (registers[1] != 0x2'0000'0003ULL ||
                    static_cast<std::uint32_t>(registers[2]) != 0x1513 ||
                    static_cast<std::uint32_t>(registers[3]) != guestMachTaskPort ||
                    static_cast<std::uint32_t>(registers[4]) != 0 ||
                    (messageId != 8000 && messageId != 8001) ||
                    registers[5] != (static_cast<std::uint64_t>(reply) << 32) ||
                    registers[6] < 44 || registers[6] > std::numeric_limits<std::uint32_t>::max() ||
                    registers[7] != 0 || guestReceiveRights.find(reply) == guestReceiveRights.end()) {
                    if (progressTrace.enabled()) {
                        const auto remote = static_cast<std::uint32_t>(registers[3]);
                        const char* destinationRole = remote == 0 ? "null"
                            : remote == guestMachTaskPort ? "guest-task"
                            : remote == guestMachHostPort ? "guest-host"
                            : guestReceiveRights.find(remote) != guestReceiveRights.end()
                                  ? "guest-receive-right" : "unknown";
                        std::cerr << "arm64-metal trace mach-msg2-reject"
                                  << " pc=0x" << hexadecimal(registers[33])
                                  << " id=" << static_cast<std::uint32_t>(registers[4] >> 32)
                                  << " remote=0x" << hexadecimal(remote)
                                  << " remote-role=" << destinationRole
                                  << " local=0x" << hexadecimal(registers[3] >> 32)
                                  << " options=0x" << hexadecimal(registers[1])
                                  << " bits=0x" << hexadecimal(
                                         static_cast<std::uint32_t>(registers[2]))
                                  << " send-size=" << (registers[2] >> 32)
                                  << " voucher=0x" << hexadecimal(
                                         static_cast<std::uint32_t>(registers[4]))
                                  << " descriptors=" << static_cast<std::uint32_t>(registers[5])
                                  << " receive-name=0x" << hexadecimal(registers[5] >> 32)
                                  << " receive-size=" << static_cast<std::uint32_t>(registers[6])
                                  << " priority=" << (registers[6] >> 32)
                                  << " timeout=" << registers[7];
                        std::cerr << '\n';
                    }
                    std::uint32_t unhandledResult = 0x10000002; // MACH_SEND_INVALID_DATA
                    const char* unhandledRole = "unknown";
                    {
                        const auto remote = static_cast<std::uint32_t>(registers[3]);
                        unhandledRole = remote == 0 ? "null"
                            : remote == guestMachTaskPort ? "guest-task"
                            : remote == guestMachHostPort ? "guest-host"
                            : guestReceiveRights.find(remote) != guestReceiveRights.end()
                                  ? "guest-receive-right" : "unknown";
                    }
                    if ((registers[1] & 1U) == 0) {
                        unhandledResult = (registers[1] & 2U) != 0
                            ? 0x10004002U  // MACH_RCV_INVALID_NAME
                            : 4U;          // KERN_INVALID_ARGUMENT
                    }
                    if (unhandledResult != 0x10004002U) {
                        survey("msgh_id", static_cast<std::int32_t>(registers[4] >> 32),
                               "shape:" + std::string(unhandledRole));
                    }
                    if (unhandledResult != 0x10004002U)
                    std::cerr << "arm64-metal mach-msg2-unhandled id="
                              << static_cast<std::int32_t>(registers[4] >> 32)
                              << " dest=" << unhandledRole
                              << " size=" << (registers[2] >> 32)
                              << " options=0x" << hexadecimal(registers[1])
                              << " pc=0x" << hexadecimal(registers[33])
                              << " reason=shape\n";
                    registers[0] = unhandledResult;
                    registers[34] = 0;
                    continue;
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
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-16))) {
                constexpr std::uint64_t kernSuccess = 0;
                constexpr std::uint64_t kernInvalidValue = 18;
                constexpr std::uint64_t kernInvalidAddress = 1;
                constexpr std::uint64_t machSendInvalidDest = 0x10000003;
                constexpr std::uint32_t receiveRight = 1;
                constexpr std::uint32_t portSetRight = 3;
                const auto target = static_cast<std::uint32_t>(registers[0]);
                std::uint64_t result = kernSuccess;
                const auto taskRight = guestSendRightUrefs.find(target);
                if (target != guestMachTaskPort || taskRight == guestSendRightUrefs.end() ||
                    taskRight->second == 0) {
                    result = machSendInvalidDest;
                } else if (registers[1] != receiveRight && registers[1] != portSetRight) {
                    result = kernInvalidValue;
                } else if (nextGuestReceiveName >=
                           std::numeric_limits<std::uint32_t>::max() - 4) {
                    result = 3; // KERN_NO_SPACE
                } else {
                    try {
                        auto* output = guestMemory.buffer(registers[2], sizeof(std::uint32_t), true);
                        const auto name = nextGuestReceiveName;
                        nextGuestReceiveName += 4;
                        GuestReceiveRight right{0, 0};
                        right.portSet = registers[1] == portSetRight;
                        guestReceiveRights.emplace(name, right);
                        std::memcpy(output, &name, sizeof(name));
                    } catch (const std::exception&) {
                        result = kernInvalidAddress;
                    }
                }
                registers[0] = result;
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-22))) {
                // _kernelrpc_mach_port_insert_member_trap(task, name, pset)
                constexpr std::uint64_t kernSuccess = 0;
                constexpr std::uint64_t kernInvalidName = 15;
                constexpr std::uint64_t kernInvalidRight = 17;
                constexpr std::uint64_t machSendInvalidDest = 0x10000003;
                const auto target = static_cast<std::uint32_t>(registers[0]);
                const auto name = static_cast<std::uint32_t>(registers[1]);
                const auto pset = static_cast<std::uint32_t>(registers[2]);
                std::uint64_t result = kernSuccess;
                const auto taskRight = guestSendRightUrefs.find(target);
                if (target != guestMachTaskPort || taskRight == guestSendRightUrefs.end() ||
                    taskRight->second == 0) {
                    result = machSendInvalidDest;
                } else {
                    const auto member = guestReceiveRights.find(name);
                    const auto set = guestReceiveRights.find(pset);
                    if (member == guestReceiveRights.end() ||
                        set == guestReceiveRights.end()) {
                        result = kernInvalidName;
                    } else if (member->second.portSet || !set->second.portSet) {
                        result = kernInvalidRight;
                    } else if (guestPortSetMembers.count(name) != 0) {
                        constexpr std::uint64_t kernAlreadyInSet = 11;
                        result = kernAlreadyInSet;
                    } else {
                        guestPortSetMembers[name] = pset;
                        deliverGuestMachMessage(name);
                    }
                }
                registers[0] = result;
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-21))) {
                constexpr std::uint64_t kernSuccess = 0;
                constexpr std::uint64_t kernInvalidRight = 17;
                constexpr std::uint64_t kernInvalidValue = 18;
                constexpr std::uint64_t machSendInvalidDest = 0x10000003;
                constexpr std::uint32_t makeSend = 20;
                const auto target = static_cast<std::uint32_t>(registers[0]);
                const auto name = static_cast<std::uint32_t>(registers[1]);
                const auto poly = static_cast<std::uint32_t>(registers[2]);
                std::uint64_t result = kernSuccess;
                const auto taskRight = guestSendRightUrefs.find(target);
                if (target != guestMachTaskPort || taskRight == guestSendRightUrefs.end() ||
                    taskRight->second == 0) {
                    result = machSendInvalidDest;
                } else if (registers[3] != makeSend) {
                    result = kernInvalidValue;
                } else if (poly != name || guestReceiveRights.find(name) ==
                           guestReceiveRights.end()) {
                    result = kernInvalidRight;
                } else {
                    auto& references = guestSendRightUrefs[name];
                    if (references < std::numeric_limits<std::uint32_t>::max()) ++references;
                }
                registers[0] = result;
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
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-28)) ||
                syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-29))) {
                const auto port = syscall == static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(-28))
                    ? static_cast<std::uint32_t>(guestMachTaskPort)
                    : static_cast<std::uint32_t>(guestMachHostPort);
                registers[0] = acquireGuestFixedSendRight(port);
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-27))) {
                const auto threadId = guestScheduler.current_id();
                registers[0] = acquireGuestThreadSendRight(threadId);
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-76))) {
                constexpr std::uint64_t kernInvalidName = 15;
                constexpr std::uint64_t kernMemoryError = 10;
                constexpr std::uint64_t machSendInvalidDest = 0x10000003;
                constexpr std::uint32_t machPortNull = 0;
                constexpr std::uint32_t machPortDead =
                    std::numeric_limits<std::uint32_t>::max();
                constexpr mach_port_type_t machPortTypeSend =
                    MACH_PORT_TYPE_SEND;
                constexpr mach_port_type_t machPortTypeReceive =
                    MACH_PORT_TYPE_RECEIVE;
                constexpr mach_port_type_t machPortTypeDeadName =
                    MACH_PORT_TYPE_DEAD_NAME;
                const auto target = static_cast<std::uint32_t>(registers[0]);
                const auto name = static_cast<std::uint32_t>(registers[1]);
                std::uint64_t result = 0;
                mach_port_type_t type = MACH_PORT_TYPE_NONE;
                const auto taskRight = guestSendRightUrefs.find(target);
                if (target != guestMachTaskPort || taskRight == guestSendRightUrefs.end() ||
                    taskRight->second == 0) {
                    result = machSendInvalidDest;
                } else if (name == machPortDead) {
                    type = machPortTypeDeadName;
                } else if (name == machPortNull) {
                    result = kernInvalidName;
                } else if (guestDeadPortNames.count(name) != 0) {
                    type = machPortTypeDeadName;
                } else {
                    const auto receiveRight = guestReceiveRights.find(name);
                    const auto sendRight = guestSendRightUrefs.find(name);
                    const bool hasReceive = receiveRight != guestReceiveRights.end();
                    const bool hasSend = sendRight != guestSendRightUrefs.end() &&
                                         sendRight->second != 0;
                    if (!hasReceive && !hasSend) {
                        result = kernInvalidName;
                    } else {
                        if (hasSend) type |= machPortTypeSend;
                        if (hasReceive) type |= machPortTypeReceive;
                    }
                }
                if (result == 0) {
                    if (registers[2] == 0) {
                        result = kernMemoryError;
                    } else {
                        try {
                            auto* output = guestMemory.buffer(registers[2], sizeof(type), true);
                            std::memcpy(output, &type, sizeof(type));
                        } catch (const std::exception&) {
                            result = kernMemoryError;
                        }
                    }
                }
                registers[0] = result;
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-77))) {
                constexpr std::uint64_t kernInvalidTask = 16;
                constexpr std::uint64_t kernInvalidName = 15;
                constexpr std::uint64_t kernInvalidRight = 17;
                constexpr std::uint64_t kernInvalidValue = 18;
                constexpr std::uint64_t kernMemoryError = 10;
                constexpr std::uint64_t kernInvalidCapability = 20;
                const auto target = static_cast<std::uint32_t>(registers[0]);
                const auto name = static_cast<std::uint32_t>(registers[1]);
                const auto notificationId = static_cast<std::int32_t>(registers[2]);
                const auto notify = static_cast<std::uint32_t>(registers[4]);
                const auto notifyDisposition = static_cast<std::uint32_t>(registers[5]);
                std::uint64_t result = 0;
                const auto taskRight = guestSendRightUrefs.find(target);
                const auto nameKnown = [&](std::uint32_t port) {
                    const auto held = guestSendRightUrefs.find(port);
                    return guestReceiveRights.find(port) != guestReceiveRights.end() ||
                           (held != guestSendRightUrefs.end() && held->second != 0) ||
                           guestDeadPortNames.count(port) != 0;
                };
                if (target != guestMachTaskPort || taskRight == guestSendRightUrefs.end() ||
                    taskRight->second == 0) {
                    result = kernInvalidTask;
                } else if (notificationId != MACH_NOTIFY_PORT_DESTROYED &&
                           notificationId != MACH_NOTIFY_NO_SENDERS &&
                           notificationId != MACH_NOTIFY_SEND_ONCE &&
                           notificationId != MACH_NOTIFY_DEAD_NAME) {
                    result = kernInvalidValue;
                } else if (!nameKnown(name)) {
                    result = kernInvalidName;
                } else if (notificationId != MACH_NOTIFY_DEAD_NAME &&
                           guestReceiveRights.find(name) == guestReceiveRights.end()) {
                    result = kernInvalidRight;
                } else if (notify != 0 &&
                           (!nameKnown(notify) ||
                            (notifyDisposition != MACH_MSG_TYPE_MAKE_SEND_ONCE &&
                             notifyDisposition != MACH_MSG_TYPE_MAKE_SEND &&
                             notifyDisposition != MACH_MSG_TYPE_COPY_SEND &&
                             notifyDisposition != MACH_MSG_TYPE_MOVE_SEND &&
                             notifyDisposition != MACH_MSG_TYPE_MOVE_SEND_ONCE))) {
                    result = kernInvalidCapability;
                } else if (registers[6] == 0) {
                    result = kernMemoryError;
                } else {
                    try {
                        const std::uint32_t previous = 0;
                        std::memcpy(guestMemory.buffer(registers[6], sizeof(previous), true),
                                    &previous, sizeof(previous));
                        if (notify != 0) {
                            std::cerr << "arm64-metal mach-notify-request-not-delivered name=0x"
                                      << hexadecimal(name) << " msgid=" << notificationId
                                      << " notify=0x" << hexadecimal(notify) << " pc=0x"
                                      << hexadecimal(registers[33]) << '\n';
                        }
                    } catch (const std::exception&) {
                        result = kernMemoryError;
                    }
                }
                registers[0] = result;
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-44))) {
                constexpr std::uint32_t kernFailure = 5;
                constexpr std::uint32_t kernSuccess = 0;
                constexpr std::uint32_t kernResourceShortage = 3;
                const auto target = static_cast<std::uint32_t>(registers[0]);
                const auto processId = static_cast<std::int32_t>(registers[1]);
                const auto taskRight = guestSendRightUrefs.find(target);
                const bool currentTask = target == guestMachTaskPort &&
                    taskRight != guestSendRightUrefs.end() && taskRight->second != 0;
                const bool currentProcess = processId == static_cast<std::int32_t>(getpid());
                std::uint32_t result = currentTask && currentProcess
                    ? kernSuccess : kernFailure;
                std::uint32_t outputName = MACH_PORT_NULL;
                if (result == kernSuccess) {
                    const auto taskName = getGuestTaskNamePort();
                    if (taskName.has_value()) {
                        outputName = acquireGuestFixedSendRight(*taskName);
                    } else {
                        result = kernResourceShortage;
                    }
                }
                if (registers[2] >= bootstrap.pageSize) {
                    try {
                        auto* output = guestMemory.buffer(registers[2], sizeof(outputName), true);
                        std::memcpy(output, &outputName, sizeof(outputName));
                    } catch (const std::exception&) {
                        // Native task_name_for_pid ignores copyout failures.
                    }
                }
                registers[0] = result;
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-46))) {
                constexpr std::uint64_t kernInvalidTask = 5;
                if (static_cast<std::uint32_t>(registers[0]) != guestMachTaskPort) {
                    registers[0] = kernInvalidTask;
                } else {
                    if (registers[1] >= bootstrap.pageSize) {
                        try {
                            auto* output = guestMemory.buffer(
                                registers[1], sizeof(std::int32_t), true);
                            const auto processId = static_cast<std::int32_t>(getpid());
                            std::memcpy(output, &processId, sizeof(processId));
                        } catch (const std::exception&) {
                            // Native pid_for_task ignores failed copyouts.
                        }
                    }
                    registers[0] = 0;
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-24))) {
                constexpr std::uint64_t kernSuccess = 0;
                constexpr std::uint64_t kernInvalidAddress = 1;
                constexpr std::uint64_t kernNoSpace = 3;
                constexpr std::uint64_t machSendInvalidDest = 0x10000003;
                constexpr std::uint64_t kernInvalidArgument = 4;
                constexpr std::uint64_t kernInvalidValue = 18;
                constexpr std::uint32_t guardedPortFlag = MPO_CONTEXT_AS_GUARD;
                constexpr std::uint32_t replyPortFlag = 0x00001000;
                constexpr std::uint32_t supportedConstructFlags =
                    MPO_CONTEXT_AS_GUARD | MPO_QLIMIT | MPO_TEMPOWNER |
                    MPO_IMPORTANCE_RECEIVER | MPO_INSERT_SEND_RIGHT | MPO_STRICT;
                constexpr std::uint32_t maxQueueLimit = 1024;
                constexpr std::uint32_t defaultQueueLimit = 5;
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
                    mach_port_options_t portOptions{};
                    std::memcpy(&portOptions, options, sizeof(portOptions));
                    if (std::getenv("ARM64_METAL_TRACE_MACH_PORT_CONSTRUCT") != nullptr) {
                        std::cerr << "arm64-metal trace mach-port-construct"
                                  << " pc=0x" << hexadecimal(registers[33])
                                  << " lr=0x" << hexadecimal(registers[30])
                                  << " flags=0x" << hexadecimal(flags)
                                  << " type=0x" << hexadecimal(flags & MPO_PORT_TYPE_MASK)
                                  << " qlimit=" << portOptions.mpl.mpl_qlimit
                                  << " context-present=" << (registers[2] != 0)
                                  << " service-info-present="
                                  << (portOptions.service_port_info != nullptr)
                                  << " service-name-present="
                                  << (portOptions.service_port_name != 0)
                                  << " work-interval-present="
                                  << (portOptions.work_interval_port != 0)
                                  << " output-present=" << (registers[3] != 0) << '\n';
                    }
                    const bool existingFlags =
                        (flags & ~(guardedPortFlag | replyPortFlag)) == 0;
                    const bool supportedFlags =
                        existingFlags || (flags & ~supportedConstructFlags) == 0;
                    if ((flags & MPO_UNUSED_BITS) != 0) {
                        result = kernInvalidArgument;
                    } else if (!supportedFlags && guest_survey::enabled()) {
                        survey("flag", -static_cast<std::int64_t>(syscall),
                               "0x" + hexadecimal(flags & ~supportedConstructFlags));
                        result = kernInvalidArgument;
                    } else if (!supportedFlags) {
                        throw std::runtime_error("Unsupported guest Mach port construction flags");
                    } else if ((flags & MPO_QLIMIT) != 0 &&
                               portOptions.mpl.mpl_qlimit > maxQueueLimit) {
                        result = kernInvalidValue;
                    } else if (nextGuestReceiveName >=
                               std::numeric_limits<std::uint32_t>::max() - 4) {
                        result = kernNoSpace;
                    } else {
                        const auto name = nextGuestReceiveName;
                        nextGuestReceiveName += 4;
                        guestReceiveRights.emplace(
                            name,
                            GuestReceiveRight{
                                flags, registers[2],
                                (flags & MPO_QLIMIT) != 0 ? portOptions.mpl.mpl_qlimit
                                                          : defaultQueueLimit});
                        if ((flags & MPO_INSERT_SEND_RIGHT) != 0) {
                            guestSendRightUrefs.emplace(name, 1U);
                        }
                        std::memcpy(output, &name, sizeof(name));
                    }
                }
                registers[0] = result;
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-18))) {
                constexpr std::uint64_t kernSuccess = 0;
                constexpr std::uint64_t kernInvalidName = 15;
                constexpr std::uint64_t kernInvalidRight = 17;
                constexpr std::uint64_t kernInvalidCapability = 20;
                constexpr std::uint64_t machSendInvalidDest = 0x10000003;
                const auto target = static_cast<std::uint32_t>(registers[0]);
                const auto name = static_cast<std::uint32_t>(registers[1]);
                std::uint64_t result = kernSuccess;
                const auto taskRight = guestSendRightUrefs.find(target);
                if (target != guestMachTaskPort || taskRight == guestSendRightUrefs.end() ||
                    taskRight->second == 0) {
                    result = machSendInvalidDest;
                } else if (name == 0 || name == std::numeric_limits<std::uint32_t>::max()) {
                    result = kernSuccess;
                } else {
                    const auto sendRight = guestSendRightUrefs.find(name);
                    if (sendRight != guestSendRightUrefs.end() && sendRight->second != 0) {
                        if (sendRight->second == maxGuestPortUrefs) {
                            // Saturated rights remain pegged until explicitly released by
                            // a MACH_PORT_UREFS_MAX delta through -19.
                        } else if (name == guestMachTaskPort && sendRight->second == 1) {
                            result = kernInvalidCapability;
                        } else if (--sendRight->second == 0) {
                            const auto hostRight =
                                hostServiceState.guestBootstrapSendRights.find(name);
                            if (hostRight != hostServiceState.guestBootstrapSendRights.end()) {
                                (void)mach_port_deallocate(mach_task_self(), hostRight->second);
                                hostServiceState.guestBootstrapSendRights.erase(hostRight);
                            }
                            guestSendRightUrefs.erase(sendRight);
                            guestDeadPortNames.erase(name);
                        }
                    } else if (guestReceiveRights.find(name) != guestReceiveRights.end()) {
                        result = kernInvalidRight;
                    } else {
                        result = kernInvalidName;
                    }
                }
                registers[0] = result;
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-19))) {
                constexpr std::uint64_t kernInvalidName = 15;
                constexpr std::uint64_t kernInvalidRight = 17;
                constexpr std::uint64_t kernInvalidValue = 18;
                constexpr std::uint64_t kernInvalidCapability = 20;
                constexpr std::uint64_t machSendInvalidDest = 0x10000003;
                constexpr std::uint32_t machPortNull = 0;
                constexpr std::uint32_t machPortDead =
                    std::numeric_limits<std::uint32_t>::max();
                const auto target = static_cast<std::uint32_t>(registers[0]);
                const auto name = static_cast<std::uint32_t>(registers[1]);
                const auto right = static_cast<mach_port_right_t>(registers[2]);
                const auto delta = static_cast<std::int32_t>(registers[3]);
                std::uint64_t result = 0;
                const auto taskRight = guestSendRightUrefs.find(target);
                if (target != guestMachTaskPort || taskRight == guestSendRightUrefs.end() ||
                    taskRight->second == 0) {
                    result = machSendInvalidDest;
                } else if (right >= MACH_PORT_RIGHT_NUMBER) {
                    result = kernInvalidValue;
                } else if (name == machPortNull || name == machPortDead) {
                    result = right == MACH_PORT_RIGHT_SEND ? 0 : kernInvalidName;
                } else if (right == MACH_PORT_RIGHT_SEND) {
                    const auto sendRight = guestSendRightUrefs.find(name);
                    if (guestDeadPortNames.find(name) != guestDeadPortNames.end()) {
                        result = kernInvalidRight;
                    } else if (sendRight == guestSendRightUrefs.end() ||
                               sendRight->second == 0) {
                        result = guestReceiveRights.find(name) != guestReceiveRights.end()
                            ? kernInvalidRight : kernInvalidName;
                    } else if (delta > static_cast<std::int32_t>(maxGuestPortUrefs)) {
                        result = kernInvalidValue;
                    } else {
                        const auto current = static_cast<std::int64_t>(sendRight->second);
                        const auto updated = current == maxGuestPortUrefs &&
                                delta != -static_cast<std::int32_t>(maxGuestPortUrefs)
                            ? current : current + static_cast<std::int64_t>(delta);
                        if (updated < 0) {
                            result = kernInvalidValue;
                        } else if (name == guestMachTaskPort && updated < 1) {
                            result = kernInvalidCapability;
                        } else if (updated == 0) {
                            const auto hostRight =
                                hostServiceState.guestBootstrapSendRights.find(name);
                            if (hostRight != hostServiceState.guestBootstrapSendRights.end()) {
                                (void)mach_port_deallocate(mach_task_self(), hostRight->second);
                                hostServiceState.guestBootstrapSendRights.erase(hostRight);
                            }
                            if (guestReceiveRights.find(name) != guestReceiveRights.end()) {
                                sendRight->second = 0;
                            } else {
                                guestSendRightUrefs.erase(sendRight);
                                guestDeadPortNames.erase(name);
                            }
                        } else {
                            sendRight->second = static_cast<std::uint32_t>(std::min<std::int64_t>(
                                updated, maxGuestPortUrefs));
                        }
                    }
                } else if (right == MACH_PORT_RIGHT_RECEIVE) {
                    const auto receiveRight = guestReceiveRights.find(name);
                    if (receiveRight == guestReceiveRights.end()) {
                        const auto sendRight = guestSendRightUrefs.find(name);
                        result = guestDeadPortNames.find(name) != guestDeadPortNames.end() ||
                                 (sendRight != guestSendRightUrefs.end() &&
                                  sendRight->second != 0)
                            ? kernInvalidRight : kernInvalidName;
                    } else if (delta == 0) {
                        result = 0;
                    } else if (delta != -1) {
                        result = kernInvalidValue;
                    } else if ((receiveRight->second.flags & MPO_CONTEXT_AS_GUARD) != 0) {
                        std::vector<std::uint64_t> live(
                            registers, registers + stateBuffer.size() / sizeof(std::uint64_t));
                        const GuestSynchronousFault fault{
                            SIGKILL, GUARD_TYPE_MACH_PORT, name, registers[33]};
                        const auto delivery = signalRuntime.deliver_synchronous(
                            hostServiceState.signals, hostServiceState.threadSignalMask,
                            live, guestMemory, fault, live[37],
                            static_cast<std::uint64_t>(activeEntry),
                            guestScheduler.current_id());
                        if (delivery.outcome == GuestSignalOutcome::ProcessExit) {
                            return delivery.exitStatus;
                        }
                        if (delivery.outcome == GuestSignalOutcome::HandlerEntered) {
                            std::memcpy(stateBuffer.data(), live.data(),
                                        live.size() * sizeof(std::uint64_t));
                            selectKernel(static_cast<std::int64_t>(delivery.handler));
                            continue;
                        }
                        throw std::runtime_error(
                            "Guarded Mach receive-right destruction was not terminated");
                    } else {
                        const auto waiting = guestMachReceiveWaiters.find(name);
                        if (waiting != guestMachReceiveWaiters.end()) {
                            for (const auto threadId : waiting->second) {
                                const auto blocked = guestMachBlockedReceives.find(threadId);
                                auto* thread = guestScheduler.find(threadId);
                                const auto* receiveWait = thread != nullptr &&
                                        thread->wait.has_value()
                                    ? std::get_if<GuestMachReceiveWait>(&*thread->wait) : nullptr;
                                if (blocked != guestMachBlockedReceives.end() &&
                                    receiveWait != nullptr && receiveWait->port == name) {
                                    guestMachBlockedReceives.erase(blocked);
                                    guestScheduler.wake(threadId,
                                        static_cast<std::int32_t>(machReceivePortChanged));
                                }
                            }
                            guestMachReceiveWaiters.erase(waiting);
                        }
                        const auto sendRight = guestSendRightUrefs.find(name);
                        if (sendRight != guestSendRightUrefs.end() &&
                            sendRight->second != 0) {
                            guestDeadPortNames.insert(name);
                        } else if (sendRight != guestSendRightUrefs.end()) {
                            guestSendRightUrefs.erase(sendRight);
                        }
                        guestReceiveRights.erase(receiveRight);
                        // Queued MOVE_SEND rights remain discarded; COPY_SEND rights are
                        // materialized only when copyout succeeds.
                        guestMachMessageQueues.erase(name);
                    }
                } else if (right == MACH_PORT_RIGHT_DEAD_NAME) {
                    const auto deadName = guestDeadPortNames.find(name);
                    const auto deadRefs = guestSendRightUrefs.find(name);
                    if (deadName == guestDeadPortNames.end() ||
                        deadRefs == guestSendRightUrefs.end() || deadRefs->second == 0) {
                        const bool known = guestReceiveRights.find(name) !=
                                guestReceiveRights.end() ||
                            guestSendRightUrefs.find(name) != guestSendRightUrefs.end();
                        result = known ? kernInvalidRight : kernInvalidName;
                    } else if (delta > static_cast<std::int32_t>(maxGuestPortUrefs)) {
                        result = kernInvalidValue;
                    } else {
                        const auto current = static_cast<std::int64_t>(deadRefs->second);
                        const auto updated = current == maxGuestPortUrefs &&
                                delta != -static_cast<std::int32_t>(maxGuestPortUrefs)
                            ? current : current + static_cast<std::int64_t>(delta);
                        if (updated < 0) {
                            result = kernInvalidValue;
                        } else if (updated == 0) {
                            guestDeadPortNames.erase(deadName);
                            guestSendRightUrefs.erase(deadRefs);
                        } else {
                            deadRefs->second = static_cast<std::uint32_t>(std::min<std::int64_t>(
                                updated, maxGuestPortUrefs));
                        }
                    }
                } else if (guestReceiveRights.find(name) == guestReceiveRights.end() &&
                           guestSendRightUrefs.find(name) == guestSendRightUrefs.end() &&
                           guestDeadPortNames.find(name) == guestDeadPortNames.end()) {
                    result = kernInvalidName;
                } else {
                    result = kernInvalidRight;
                }
                if (progressTrace.enabled()) {
                    static std::uint32_t modRefsTraceCount = 0;
                    if (modRefsTraceCount < 32) {
                        ++modRefsTraceCount;
                        std::cerr << "arm64-metal trace mach-port-mod-refs"
                                  << " pc=0x" << hexadecimal(registers[33])
                                  << " task=0x" << hexadecimal(target)
                                  << " name=0x" << hexadecimal(name)
                                  << " right=" << right << " delta=" << delta
                                  << " result=" << result << '\n';
                    }
                }
                registers[0] = result;
                registers[34] = 0;
                continue;
            }
            if (syscall == static_cast<std::uint64_t>(static_cast<std::int64_t>(-25))) {
                constexpr std::uint64_t kernSuccess = 0;
                constexpr std::uint64_t kernInvalidName = 15;
                constexpr std::uint64_t kernInvalidValue = 18;
                constexpr std::uint64_t machSendInvalidDest = 0x10000003;
                constexpr std::uint64_t machSendInvalidRight = 0x10000004;
                const auto target = static_cast<std::uint32_t>(registers[0]);
                const auto name = static_cast<std::uint32_t>(registers[1]);
                const auto sendRightDelta = static_cast<std::int32_t>(registers[2]);
                std::uint64_t result = kernSuccess;
                const auto taskRight = guestSendRightUrefs.find(target);
                if (target != guestMachTaskPort || taskRight == guestSendRightUrefs.end() ||
                    taskRight->second == 0) {
                    result = machSendInvalidDest;
                } else if (name == 0 || name == std::numeric_limits<std::uint32_t>::max()) {
                    result = kernInvalidName;
                } else {
                    const auto right = guestReceiveRights.find(name);
                    if (right == guestReceiveRights.end()) {
                        result = kernInvalidName;
                    } else if (((right->second.flags & MPO_CONTEXT_AS_GUARD) != 0 &&
                                right->second.context != registers[3])) {
                        if ((right->second.flags & MPO_STRICT) != 0) {
                            std::vector<std::uint64_t> live(
                                registers, registers + stateBuffer.size() / sizeof(std::uint64_t));
                            const GuestSynchronousFault fault{
                                SIGKILL, GUARD_TYPE_MACH_PORT, name, registers[33]};
                            const auto delivery = signalRuntime.deliver_synchronous(
                                hostServiceState.signals, hostServiceState.threadSignalMask,
                                live, guestMemory, fault, live[37],
                                static_cast<std::uint64_t>(activeEntry),
                                guestScheduler.current_id());
                            if (delivery.outcome == GuestSignalOutcome::ProcessExit) {
                                return delivery.exitStatus;
                            }
                            if (delivery.outcome == GuestSignalOutcome::HandlerEntered) {
                                std::memcpy(stateBuffer.data(), live.data(),
                                            live.size() * sizeof(std::uint64_t));
                                selectKernel(static_cast<std::int64_t>(delivery.handler));
                                continue;
                            }
                            throw std::runtime_error(
                                "Strict Mach port guard mismatch was not terminated");
                        }
                        result = machSendInvalidRight;
                    } else if ((right->second.flags & MPO_INSERT_SEND_RIGHT) == 0 &&
                               registers[2] != 0) {
                        result = machSendInvalidRight;
                    } else if (sendRightDelta > 0 || sendRightDelta < -1) {
                        result = kernInvalidValue;
                    } else if (sendRightDelta == -1 &&
                               (guestSendRightUrefs.find(name) == guestSendRightUrefs.end() ||
                                guestSendRightUrefs.at(name) == 0)) {
                        result = kernInvalidValue;
                    } else {
                        if (sendRightDelta == -1) {
                            auto sendRight = guestSendRightUrefs.find(name);
                            if (--sendRight->second == 0) {
                                guestSendRightUrefs.erase(sendRight);
                            }
                        }
                        const auto waiting = guestMachReceiveWaiters.find(name);
                        if (waiting != guestMachReceiveWaiters.end()) {
                            for (const auto threadId : waiting->second) {
                                const auto blocked = guestMachBlockedReceives.find(threadId);
                                auto* thread = guestScheduler.find(threadId);
                                const auto* receiveWait = thread != nullptr &&
                                        thread->wait.has_value()
                                    ? std::get_if<GuestMachReceiveWait>(&*thread->wait) : nullptr;
                                if (blocked != guestMachBlockedReceives.end() &&
                                    receiveWait != nullptr && receiveWait->port == name) {
                                    progressTrace.event("mach-receive-destroy-wake",
                                                        threadId, name);
                                    guestMachBlockedReceives.erase(blocked);
                                    guestScheduler.wake(threadId,
                                        static_cast<std::int32_t>(machReceivePortChanged));
                                }
                            }
                            guestMachReceiveWaiters.erase(waiting);
                        }
                        const auto sendRight = guestSendRightUrefs.find(name);
                        if (sendRight != guestSendRightUrefs.end() &&
                            sendRight->second != 0) {
                            guestDeadPortNames.insert(name);
                        }
                        if (guestTaskDebugControlPort == name) {
                            guestTaskDebugControlPortDead = true;
                        }
                        guestReceiveRights.erase(right);
                        // Dropping this port's queue also drops any in-transit descriptors.
                        guestMachMessageQueues.erase(name);
                    }
                }
                registers[0] = result;
                registers[34] = 0;
                continue;
            }
            if (syscall == 366) {
                constexpr std::uint64_t carryMask = 8U;
                constexpr std::uint64_t maximumPthreadSize = 64U * 1024U;
                constexpr std::uint64_t bsdthreadSupportedFeatures = 0x4000115fU;
                struct GuestBsdthreadRegistrationData {
                    std::uint64_t version;
                    std::uint64_t dispatchQueueOffset;
                    std::uint64_t mainQos;
                    std::uint32_t tsdOffset;
                    std::uint32_t returnToKernelOffset;
                    std::uint32_t machThreadSelfOffset;
                    std::uint64_t stackAddrHint;
                    std::uint32_t mutexDefaultPolicy;
                    std::uint32_t joinableOffsetBits;
                    std::uint32_t workqueueQuantumExpiryOffset;
                } __attribute__((packed));
                static_assert(sizeof(GuestBsdthreadRegistrationData) == 56);
                static_assert(offsetof(GuestBsdthreadRegistrationData, mainQos) == 16);
                static_assert(offsetof(GuestBsdthreadRegistrationData, stackAddrHint) == 36);
                std::uint64_t error = 0;
                const auto pageSize = bootstrap.pageSize;
                GuestBsdthreadRegistrationData data{};
                std::size_t copySize = 0;
                if (registers[2] > maximumPthreadSize) {
                    error = EINVAL;
                } else if (registers[3] != 0 && registers[4] < sizeof(data.version)) {
                    error = EINVAL;
                } else if (registers[3] != 0 && registers[3] < pageSize) {
                    error = EFAULT;
                } else if (registers[3] != 0) {
                    copySize = static_cast<std::size_t>(std::min<std::uint64_t>(
                        registers[4], sizeof(data)));
                    try {
                        const auto* source = guestMemory.buffer(registers[3], copySize);
                        std::memcpy(&data, source, copySize);
                        if (data.version != registers[4]) {
                            error = EINVAL;
                        }
                    } catch (const std::out_of_range&) {
                        error = EFAULT;
                    }
                } else {
                    // Pre-versioned callers pass the dispatch queue offset directly.
                    data.dispatchQueueOffset = registers[5];
                }
                if (error == 0 && guestBsdthreadRegistration) {
                    error = EINVAL;
                }
                if (error == 0) {
                    if (pageSize == 0 || (pageSize & (pageSize - 1)) != 0) {
                        error = EINVAL;
                    } else {
                        const auto pthreadSize = registers[2];
                        constexpr std::uint64_t tsdSlotSize = sizeof(std::uint64_t);
                        std::uint64_t maximumTsdOffset = 0;
                        if (pthreadSize >= tsdSlotSize &&
                            data.tsdOffset <= pthreadSize - tsdSlotSize) {
                            maximumTsdOffset = pthreadSize - data.tsdOffset - tsdSlotSize;
                        } else {
                            data.tsdOffset = 0;
                        }
                        if (data.dispatchQueueOffset > maximumTsdOffset)
                            data.dispatchQueueOffset = 0;
                        if (data.returnToKernelOffset > maximumTsdOffset)
                            data.returnToKernelOffset = 0;
                        if (data.machThreadSelfOffset > maximumTsdOffset)
                            data.machThreadSelfOffset = 0;
                        if (data.workqueueQuantumExpiryOffset > maximumTsdOffset)
                            data.workqueueQuantumExpiryOffset = 0;

                        const auto stackAddressHint = static_cast<std::uint64_t>(
                            bootstrap.stack.end) & ~(pageSize - 1);
                        if (registers[3] != 0) {
                            data.version = sizeof(data);
                            // The guest kernel does not expose host thread QoS state.
                            // Zero retains libpthread's documented unspecified fallback.
                            data.mainQos = 0;
                            data.stackAddrHint = stackAddressHint;
                            data.mutexDefaultPolicy = 0;
                        }

                        // XNU records the registration before copyout; a copyout
                        // failure returns EINVAL but leaves the registration installed.
                        guestBsdthreadRegistration = GuestBsdthreadRegistration{
                            registers[0], registers[1], pthreadSize};
                        if (registers[3] != 0) {
                            try {
                                auto* destination = guestMemory.buffer(
                                    registers[3], copySize, true);
                                std::memcpy(destination, &data, copySize);
                            } catch (const std::out_of_range&) {
                                error = EINVAL;
                            }
                        }
                    }
                }
                if (error == 0) {
                    registers[0] = bsdthreadSupportedFeatures;
                    registers[32] &= ~carryMask;
                } else {
                    registers[0] = error;
                    registers[32] |= carryMask;
                }
                registers[34] = 0;
                continue;
            }
            if (syscall == 372) {
                registers[0] = guestScheduler.current_id();
                registers[32] &= ~std::uint64_t{8};
                registers[34] = 0;
                continue;
            }
            if (syscall == 367) {
                // Native workq_open is argument-free and succeeds even before
                // an individual workqueue request is made.
                guestWorkqueueOpened = true;
                workqueueTrace.open(registers[33]);
                registers[0] = 0;
                registers[32] &= ~std::uint64_t{8};
                registers[34] = 0;
                continue;
            }
            if (syscall == 368) {
                constexpr std::uint64_t carryMask = 8U;
                constexpr std::uint64_t threadReturn = 0x4;
                constexpr std::uint64_t requestThreads = 0x20;
                constexpr std::uint64_t setupDispatch = 0x400;
                constexpr std::uint64_t dispatchConfigSize = 24;
                constexpr std::uint64_t qosClassMask = 0x00003f00;
                constexpr std::uint64_t overcommitFlag = 0x80000000;
                constexpr std::uint64_t requestFallbackFlag = 0x04000000;
                constexpr std::uint64_t requestDefaultQueueFlag = 0x10000000;
                static_assert(sizeof(GuestWorkqueueDispatchConfig) == dispatchConfigSize);
                if (registers[0] == threadReturn &&
                    guestWorkqueueThreadIds.count(guestScheduler.current_id()) != 0 &&
                    registers[1] == 0 && registers[2] == 0 && registers[3] == 0) {
                    workqueueTrace.event("worker-return", threadReturn, registers[33],
                                         guestScheduler.current_id());
                    std::vector<std::uint64_t> live(registers, registers + 36);
                    const bool exited = finishGuestThread(0, live);
                    std::copy_n(live.begin(), 36, registers);
                    if (exited) return 0;
                    continue;
                }
                if (registers[0] == requestThreads) {
                    const auto priority = registers[3];
                    const auto classBits = priority & qosClassMask;
                    const auto encodedQos = classBits >> 8;
                    const auto qos = encodedQos == 0 || (encodedQos & (encodedQos - 1)) != 0
                        ? 0U : static_cast<unsigned>(__builtin_ffs(
                            static_cast<int>(encodedQos)));
                    std::uint64_t error = 0;
                    if (!guestWorkqueueOpened || !guestWorkqueueDispatchConfig ||
                        !guestBsdthreadRegistration) {
                        error = EINVAL;
                    } else if (registers[1] != 0 || registers[2] != 1) {
                        error = ENOTSUP;
                        survey("enotsup", static_cast<std::int64_t>(syscall), "workq-request-shape");
                    } else if (qos == 0 || qos >= 7 ||
                               (priority & ~(qosClassMask | 0xffU | overcommitFlag |
                                             requestFallbackFlag | requestDefaultQueueFlag)) != 0) {
                        error = EINVAL;
                    } else {
                        const auto workqueueToken = guestBsdthreadRegistration->workqueueStart;
                        const auto* state = static_cast<const std::uint64_t*>(stateBuffer.data());
                        const auto workerEntry = decode_guest_ia0_callback(
                            state, stateBuffer.size() / sizeof(std::uint64_t), workqueueToken);
                        if (!workerEntry || *workerEntry > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max()) ||
                            !image.is_code(static_cast<std::int64_t>(*workerEntry))) {
                            error = EINVAL;
                        } else if (bootstrap.pageSize == 0 ||
                                   (bootstrap.pageSize & (bootstrap.pageSize - 1)) != 0) {
                            error = EINVAL;
                        } else {
                            constexpr std::uint64_t minimumPthreadSize = 0x18e0;
                            constexpr std::uint64_t pthreadStackOffset = 12U * 1024U;
                            constexpr std::uint64_t stackSize = 512U * 1024U;
                            constexpr std::uint64_t errnoOffset = 172;
                            constexpr std::uint64_t threadIdOffset = 216;
                            constexpr std::uint64_t tsdOffset = 224;
                            const auto pageSize = bootstrap.pageSize;
                            const auto pthreadSize = guestBsdthreadRegistration->pthreadSize;
                            if (pthreadSize < minimumPthreadSize || pthreadSize +
                                pthreadStackOffset > std::numeric_limits<std::uint64_t>::max() -
                                    (pageSize - 1)) {
                                error = EINVAL;
                            } else {
                                const auto pthreadTail = (pthreadSize + pthreadStackOffset +
                                    pageSize - 1) & ~(pageSize - 1);
                                const auto allocationSize = pageSize + stackSize + pthreadTail;
                                const auto allocation = allocateGuestHeap(
                                    allocationSize, pageSize);
                                if (!allocation) {
                                    error = EAGAIN;
                                } else if (nextGuestThreadPortName >=
                                    std::numeric_limits<std::uint32_t>::max() - 4) {
                                    hostServiceState.heap->release(*allocation);
                                    error = EAGAIN;
                                } else {
                                    const auto allocationAddress =
                                        static_cast<std::uint64_t>(*allocation);
                                    const auto stackBottom = allocationAddress + pageSize;
                                    const auto self = stackBottom + stackSize +
                                        pthreadStackOffset;
                                    const auto threadId = guestScheduler.next_thread_id();
                                    const auto threadPort = nextGuestThreadPortName;
                                    nextGuestThreadPortName += 4;
                                    try {
                                        auto* bytes = guestMemory.buffer(
                                            allocationAddress, allocationSize, true);
                                        std::memset(bytes, 0,
                                            static_cast<std::size_t>(allocationSize));
                                        std::uint64_t idValue = threadId;
                                        std::memcpy(bytes + (self - allocationAddress) +
                                            threadIdOffset, &idValue, sizeof(idValue));
                                        std::uint64_t portValue = threadPort;
                                        std::memcpy(bytes + (self - allocationAddress) +
                                            tsdOffset + guestMachThreadSlotOffset,
                                            &portValue, sizeof(portValue));
                                        const auto upcallFlags = std::uint64_t{0x00040000} |
                                            std::uint64_t{0x00004000} | qos |
                                            std::uint64_t{0x00200000} |
                                            ((priority & overcommitFlag) != 0
                                                ? std::uint64_t{0x00010000} : 0);
                                        auto thread = GuestScheduler::make_thread_frame(
                                            {static_cast<std::int64_t>(stackBottom),
                                             static_cast<std::int64_t>(self)},
                                            *allocation,
                                            static_cast<std::int64_t>(self + errnoOffset),
                                            static_cast<std::int64_t>(self + tsdOffset),
                                            *workerEntry, self, registers[
                                                guestSchedulerFPCR], registers[
                                                guestSchedulerFPSR],
                                            hostServiceState.threadSignalMask);
                                        thread.id = threadId;
                                        thread.detached = true;
                                        thread.registers[1] = threadPort;
                                        thread.registers[2] = stackBottom;
                                        thread.registers[3] = 0;
                                        thread.registers[4] = upcallFlags;
                                        thread.registers[5] = 0;
                                        guestWorkqueueThreadIds.insert(threadId);
                                        guestThreadPortNames.emplace(threadId, threadPort);
                                        guestSendRightUrefs.emplace(threadPort, 1);
                                        liveThreadStacks.emplace(threadId,
                                            GuestMemoryRange{stackBottom, self});
                                        if (guestScheduler.add_thread(std::move(thread)) !=
                                            threadId) {
                                            throw std::logic_error(
                                                "Unexpected guest workqueue thread id");
                                        }
                                        workqueueTrace.event("request", requestThreads,
                                            registers[33], guestScheduler.current_id(),
                                            threadId, *workerEntry);
                                        registers[0] = 0;
                                        registers[32] &= ~carryMask;
                                    } catch (...) {
                                        guestWorkqueueThreadIds.erase(threadId);
                                        guestThreadPortNames.erase(threadId);
                                        guestSendRightUrefs.erase(threadPort);
                                        liveThreadStacks.erase(threadId);
                                        hostServiceState.heap->release(*allocation);
                                        throw;
                                    }
                                }
                            }
                        }
                    }
                    if (error != 0) {
                        workqueueTrace.event("request-rejected", requestThreads, registers[33],
                                             guestScheduler.current_id(), error, priority);
                        registers[0] = error;
                        registers[32] |= carryMask;
                    }
                    registers[34] = 0;
                    continue;
                }
                if (registers[0] != setupDispatch && guest_survey::enabled()) {
                    survey("workq", static_cast<std::int64_t>(syscall),
                           "0x" + hexadecimal(registers[0]));
                    rawFailQuiet(EINVAL);
                    continue;
                }
                if (registers[0] != setupDispatch) {
                    throw std::runtime_error("Unsupported guest workqueue operation 0x" +
                        hexadecimal(registers[0]) + (guestWorkqueueOpened
                            ? " after workq_open" : " before workq_open"));
                }

                std::uint64_t error = 0;
                if (registers[2] != dispatchConfigSize || registers[3] != 0) {
                    error = ENOTSUP;
                    survey("enotsup", static_cast<std::int64_t>(syscall), "workq-setup-shape");
                } else if (registers[1] < ExecutableImage::minimum_image_address) {
                    error = EFAULT;
                } else {
                    try {
                        const auto* bytes = guestMemory.buffer(registers[1],
                            dispatchConfigSize);
                        GuestWorkqueueDispatchConfig config{};
                        std::memcpy(&config, bytes, sizeof(config));
                        if (config.version != 2 || config.flags != 0) {
                            error = ENOTSUP;
                            survey("enotsup", static_cast<std::int64_t>(syscall),
                                   "workq-dispatch-config");
                        } else {
                            // The kernel setup trap is repeat-tolerant. Keep
                            // only guest-supplied metadata; dispatch's callback
                            // pointers are published by guest libpthread.
                            guestWorkqueueDispatchConfig = config;
                            workqueueTrace.dispatch_config(registers[33]);
                        }
                    } catch (const std::out_of_range&) {
                        error = EFAULT;
                    }
                }
                if (error == 0) {
                    registers[0] = 0;
                    registers[32] &= ~carryMask;
                } else {
                    registers[0] = error;
                    registers[32] |= carryMask;
                }
                registers[34] = 0;
                continue;
            }
#include "raw_fs_syscalls.inc"
            if (static_cast<std::int64_t>(syscall) < 0 &&
                static_cast<std::int64_t>(syscall) >= -128) {
                constexpr std::uint64_t kernSuccess = 0;
                constexpr std::uint64_t kernInvalidAddress = 1;
                constexpr std::uint64_t kernNoSpace = 3;
                constexpr std::uint64_t kernInvalidArgument = 4;
                constexpr std::uint64_t kernFailure = 5;
                constexpr std::uint64_t kernNotInSet = 12;
                constexpr std::uint64_t kernInvalidName = 15;
                constexpr std::uint64_t kernInvalidRight = 17;
                constexpr std::uint64_t kernInvalidValue = 18;
                constexpr std::uint64_t kernNotSupported = 46;
                constexpr std::uint64_t machSendInvalidDest = 0x10000003;
                constexpr std::uint32_t machPortDead = std::numeric_limits<std::uint32_t>::max();
                const auto machTrapNumber = -static_cast<std::int64_t>(syscall);
                const auto machTaskArgumentValid = [&](std::uint64_t value) {
                    const auto name = static_cast<std::uint32_t>(value);
                    const auto right = guestSendRightUrefs.find(name);
                    return name == guestMachTaskPort && right != guestSendRightUrefs.end() &&
                           right->second != 0;
                };
                const auto machStoreGuest = [&](std::uint64_t address, const void* bytes,
                                                std::size_t size) {
                    try {
                        std::memcpy(guestMemory.buffer(address, size, true), bytes, size);
                        return true;
                    } catch (const std::exception&) {
                        return false;
                    }
                };
                const auto machTicksToNanoseconds = [&](std::uint64_t ticks) {
                    mach_timebase_info_data_t base{};
                    (void)mach_timebase_info(&base);
                    if (base.denom == 0) return ticks;
                    return ticks > std::numeric_limits<std::uint64_t>::max() / base.numer
                        ? std::numeric_limits<std::uint64_t>::max() / base.denom * base.numer
                        : ticks * base.numer / base.denom;
                };
                const auto machSleepUntil = [&](std::uint64_t deadlineNanoseconds) {
                    if (!guestScheduler.sleep_current(deadlineNanoseconds, std::nullopt)) {
                        throw std::runtime_error("Current guest thread already has a wait reason");
                    }
                    registers[0] = kernSuccess;
                    registers[34] = 0;
                    std::vector<std::uint64_t> live(
                        registers, registers + stateBuffer.size() / sizeof(std::uint64_t));
                    if (!syncThreadSwitch(false, live)) {
                        throw std::runtime_error("Guest Mach sleep has no runnable thread");
                    }
                };
                const auto machYield = [&](std::uint64_t result) {
                    registers[0] = result;
                    registers[34] = 0;
                    std::vector<std::uint64_t> live(
                        registers, registers + stateBuffer.size() / sizeof(std::uint64_t));
                    (void)syncThreadSwitch(true, live);
                };
                const auto machSemaphoreStatus = [&](std::uint64_t value) -> std::uint64_t {
                    const auto name = static_cast<std::uint32_t>(value);
                    const auto right = guestSendRightUrefs.find(name);
                    if (right == guestSendRightUrefs.end() || right->second == 0) {
                        return KERN_INVALID_NAME;
                    }
                    return guestSemaphoreNames.find(name) == guestSemaphoreNames.end()
                        ? KERN_INVALID_CAPABILITY : kernSuccess;
                };
                const auto machSemaphoreWait = [&](std::uint32_t name,
                                                    std::optional<std::uint64_t> deadline,
                                                    bool nonblocking) {
                    const auto result = guestScheduler.wait_semaphore(name, deadline, nonblocking);
                    if (!result.waiting) {
                        registers[0] = static_cast<std::uint32_t>(result.error);
                        registers[34] = 0;
                        return false;
                    }
                    registers[0] = kernSuccess;
                    registers[34] = 0;
                    std::vector<std::uint64_t> live(
                        registers, registers + stateBuffer.size() / sizeof(std::uint64_t));
                    if (!syncThreadSwitch(false, live)) {
                        throw std::runtime_error("Guest semaphore wait has no runnable thread");
                    }
                    return true;
                };
                const auto machRelativeDeadline = [&](std::uint32_t seconds,
                                                       std::int32_t nanoseconds) {
                    const auto now = monotonicNanoseconds();
                    const auto requested = seconds >
                            (std::numeric_limits<std::uint64_t>::max() -
                                static_cast<std::uint32_t>(nanoseconds)) / 1'000'000'000ULL
                        ? std::numeric_limits<std::uint64_t>::max()
                        : static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL +
                            static_cast<std::uint32_t>(nanoseconds);
                    return now + std::min(requested,
                                          std::numeric_limits<std::uint64_t>::max() - now);
                };
                const auto machDestroyReceiveRight = [&](std::uint32_t name) {
                    const auto waiting = guestMachReceiveWaiters.find(name);
                    if (waiting != guestMachReceiveWaiters.end()) {
                        for (const auto threadId : waiting->second) {
                            const auto blocked = guestMachBlockedReceives.find(threadId);
                            auto* thread = guestScheduler.find(threadId);
                            const auto* receiveWait = thread != nullptr &&
                                    thread->wait.has_value()
                                ? std::get_if<GuestMachReceiveWait>(&*thread->wait) : nullptr;
                            if (blocked != guestMachBlockedReceives.end() &&
                                receiveWait != nullptr && receiveWait->port == name) {
                                guestMachBlockedReceives.erase(blocked);
                                guestScheduler.wake(threadId,
                                    static_cast<std::int32_t>(machReceivePortChanged));
                            }
                        }
                        guestMachReceiveWaiters.erase(waiting);
                    }
                    guestReceiveRights.erase(name);
                    guestMachMessageQueues.erase(name);
                    guestPortSetMembers.erase(name);
                    guestPortGuards.erase(name);
                };
                bool machTrapHandled = true;
                switch (machTrapNumber) {
                case 91: {  // mk_timer_create
                    if (nextGuestReceiveName >= std::numeric_limits<std::uint32_t>::max() - 4) {
                        registers[0] = 0;
                    } else {
                        const auto name = nextGuestReceiveName;
                        nextGuestReceiveName += 4;
                        guestReceiveRights.emplace(name, GuestReceiveRight{0, 0});
                        guestMachTimers.emplace(name, GuestMachTimer{});
                        registers[0] = name;
                    }
                    break;
                }
                case 92:    // mk_timer_destroy
                case 93:    // mk_timer_arm
                case 94:    // mk_timer_cancel
                case 95: {  // mk_timer_arm_leeway
                    const auto name = static_cast<std::uint32_t>(registers[0]);
                    const auto timer = guestMachTimers.find(name);
                    if (timer == guestMachTimers.end() ||
                        guestReceiveRights.find(name) == guestReceiveRights.end()) {
                        registers[0] = guestReceiveRights.find(name) != guestReceiveRights.end()
                            ? kernInvalidArgument : kernInvalidName;
                    } else if (machTrapNumber == 92) {
                        guestMachTimers.erase(timer);
                        machDestroyReceiveRight(name);
                        registers[0] = kernSuccess;
                    } else if (machTrapNumber == 94) {
                        const auto previous = timer->second.deadline.value_or(0);
                        timer->second.deadline.reset();
                        if (registers[1] != 0) (void)machStoreGuest(registers[1], &previous,
                                                                    sizeof(previous));
                        registers[0] = kernSuccess;
                    } else {
                        timer->second.deadline = machTrapNumber == 93 ? registers[1] : registers[2];
                        fireDueMachTimers();
                        registers[0] = kernSuccess;
                    }
                    break;
                }
                case 90: {  // mach_wait_until
                    const auto deadlineTicks = registers[0];
                    const auto nowTicks = mach_absolute_time();
                    if (deadlineTicks == 0) {
                        machSleepUntil(std::numeric_limits<std::uint64_t>::max());
                        continue;
                    }
                    if (deadlineTicks <= nowTicks) {
                        registers[0] = kernSuccess;
                        break;
                    }
                    const auto now = monotonicNanoseconds();
                    const auto remaining = machTicksToNanoseconds(deadlineTicks - nowTicks);
                    machSleepUntil(now + std::min(remaining,
                        std::numeric_limits<std::uint64_t>::max() - now));
                    continue;
                }
                case 89: {  // mach_timebase_info_trap
                    mach_timebase_info_data_t base{};
                    (void)mach_timebase_info(&base);
                    if (registers[0] != 0) (void)machStoreGuest(registers[0], &base, sizeof(base));
                    registers[0] = kernSuccess;
                    break;
                }
                case 59:    // swtch_pri
                case 60: {  // swtch
                    const bool otherReady = !guestScheduler.ready_queue().empty();
                    if (otherReady) {
                        machYield(1);
                        continue;
                    }
                    registers[0] = 0;
                    break;
                }
                case 61: {  // thread_switch
                    const auto option = static_cast<std::uint32_t>(registers[1]);
                    if (option > 5) {
                        registers[0] = kernInvalidArgument;
                        break;
                    }
                    const auto milliseconds = static_cast<std::uint32_t>(registers[2]);
                    if ((option == 2 || option == 5) && milliseconds != 0) {
                        const auto now = monotonicNanoseconds();
                        machSleepUntil(now + static_cast<std::uint64_t>(milliseconds) *
                                                 1'000'000ULL);
                        continue;
                    }
                    if (!guestScheduler.ready_queue().empty()) {
                        machYield(kernSuccess);
                        continue;
                    }
                    registers[0] = kernSuccess;
                    break;
                }
                case 62: {  // clock_sleep_trap
                    const auto clockName = static_cast<std::uint32_t>(registers[0]);
                    const auto sleepType = static_cast<std::uint32_t>(registers[1]);
                    const auto seconds = static_cast<std::int32_t>(registers[2]);
                    const auto nanoseconds = static_cast<std::int32_t>(registers[3]);
                    clock_id_t clockId = SYSTEM_CLOCK;
                    if (clockName != 0) {
                        const auto service = guestClockServiceIds.find(clockName);
                        if (service == guestClockServiceIds.end()) {
                            registers[0] = kernInvalidArgument;
                            break;
                        }
                        clockId = service->second;
                    }
                    if (clockId != SYSTEM_CLOCK) {
                        registers[0] = kernFailure;
                        break;
                    }
                    if ((sleepType != 0 && sleepType != 1) || seconds < 0 || nanoseconds < 0 ||
                        nanoseconds >= 1'000'000'000) {
                        registers[0] = kernInvalidValue;
                        break;
                    }
                    const auto nowTicks = mach_absolute_time();
                    const auto nowNanoseconds = machTicksToNanoseconds(nowTicks);
                    const auto requested = static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL +
                        static_cast<std::uint64_t>(nanoseconds);
                    const auto wakeNanoseconds = sleepType == 0 ? requested
                                                                : nowNanoseconds + requested;
                    if (registers[4] != 0) {
                        mach_timespec_t wakeTime{
                            static_cast<unsigned int>(wakeNanoseconds / 1'000'000'000ULL),
                            static_cast<clock_res_t>(wakeNanoseconds % 1'000'000'000ULL)};
                        (void)machStoreGuest(registers[4], &wakeTime, sizeof(wakeTime));
                    }
                    if (wakeNanoseconds <= nowNanoseconds) {
                        registers[0] = kernSuccess;
                        break;
                    }
                    const auto now = monotonicNanoseconds();
                    machSleepUntil(now + (wakeNanoseconds - nowNanoseconds));
                    continue;
                }
                case 43: {  // mach_generate_activity_id
                    const auto count = static_cast<std::int32_t>(registers[1]);
                    if (count <= 0) {
                        registers[0] = kernInvalidArgument;
                        break;
                    }
                    const auto first = nextGuestActivityId;
                    nextGuestActivityId += static_cast<std::uint64_t>(count);
                    registers[0] = machStoreGuest(registers[2], &first, sizeof(first))
                        ? kernSuccess : kernInvalidAddress;
                    break;
                }
                case 50: {  // thread_get_special_reply_port
                    constexpr std::uint32_t replyPortFlag = 0x00001000;
                    if (nextGuestReceiveName >= std::numeric_limits<std::uint32_t>::max() - 4) {
                        registers[0] = 0;
                    } else {
                        const auto name = nextGuestReceiveName;
                        nextGuestReceiveName += 4;
                        guestReceiveRights.emplace(name, GuestReceiveRight{replyPortFlag, 0});
                        registers[0] = name;
                    }
                    break;
                }
                case 58:  // pfz_exit
                    registers[0] = kernSuccess;
                    break;
                case 88:  // _exclaves_ctl_trap
                    registers[0] = kernNotSupported;
                    break;
                case 100:  // iokit_user_client_trap
                    registers[0] = 0xffffffffe00002c2ULL;
                    break;
                case 45: {  // task_for_pid
                    if (!machTaskArgumentValid(registers[0]) ||
                        static_cast<std::int32_t>(registers[1]) !=
                            static_cast<std::int32_t>(getpid())) {
                        registers[0] = kernFailure;
                        break;
                    }
                    const auto name = acquireGuestFixedSendRight(guestMachTaskPort);
                    (void)machStoreGuest(registers[2], &name, sizeof(name));
                    registers[0] = kernSuccess;
                    break;
                }
                case 34: {  // semaphore_signal_all_trap
                    const auto status = machSemaphoreStatus(registers[0]);
                    registers[0] = status != kernSuccess ? status
                        : static_cast<std::uint32_t>(guestScheduler.signal_all_semaphore(
                              static_cast<std::uint32_t>(registers[0])));
                    break;
                }
                case 35: {  // semaphore_signal_thread_trap
                    const auto status = machSemaphoreStatus(registers[0]);
                    if (status != kernSuccess) {
                        registers[0] = status;
                        break;
                    }
                    const auto threadName = static_cast<std::uint32_t>(registers[1]);
                    std::optional<std::uint64_t> threadId;
                    if (threadName != 0) {
                        for (const auto& entry : guestThreadPortNames) {
                            if (entry.second == threadName) threadId = entry.first;
                        }
                        const auto right = guestSendRightUrefs.find(threadName);
                        if (!threadId || right == guestSendRightUrefs.end() ||
                            right->second == 0) {
                            registers[0] = kernInvalidArgument;
                            break;
                        }
                    }
                    registers[0] = static_cast<std::uint32_t>(guestScheduler.signal_semaphore_thread(
                        static_cast<std::uint32_t>(registers[0]), threadId));
                    break;
                }
                case 37:    // semaphore_wait_signal_trap
                case 39: {  // semaphore_timedwait_signal_trap
                    const bool timed = machTrapNumber == 39;
                    const auto timeoutSeconds = static_cast<std::uint32_t>(registers[2]);
                    const auto timeoutNanoseconds = static_cast<std::int32_t>(
                        static_cast<std::uint32_t>(registers[3]));
                    if (timed && (timeoutNanoseconds < 0 || timeoutNanoseconds >= 1'000'000'000)) {
                        registers[0] = kernInvalidValue;
                        break;
                    }
                    auto status = machSemaphoreStatus(registers[0]);
                    if (status == kernSuccess) status = machSemaphoreStatus(registers[1]);
                    if (status != kernSuccess) {
                        registers[0] = status;
                        break;
                    }
                    const auto waitName = static_cast<std::uint32_t>(registers[0]);
                    const auto signalError = guestScheduler.signal_semaphore(
                        static_cast<std::uint32_t>(registers[1]));
                    if (signalError != 0) {
                        registers[0] = static_cast<std::uint32_t>(signalError);
                        break;
                    }
                    std::optional<std::uint64_t> deadline;
                    const bool nonblocking = timed && timeoutSeconds == 0 &&
                                             timeoutNanoseconds == 0;
                    if (timed) deadline = machRelativeDeadline(timeoutSeconds, timeoutNanoseconds);
                    if (machSemaphoreWait(waitName, deadline, nonblocking)) continue;
                    break;
                }
                case 20:    // _kernelrpc_mach_port_move_member_trap
                case 23: {  // _kernelrpc_mach_port_extract_member_trap
                    const auto name = static_cast<std::uint32_t>(registers[1]);
                    const auto set = static_cast<std::uint32_t>(registers[2]);
                    if (!machTaskArgumentValid(registers[0])) {
                        registers[0] = machSendInvalidDest;
                        break;
                    }
                    const auto member = guestReceiveRights.find(name);
                    if (member == guestReceiveRights.end()) {
                        registers[0] = kernInvalidName;
                        break;
                    }
                    if (member->second.portSet) {
                        registers[0] = kernInvalidRight;
                        break;
                    }
                    if (machTrapNumber == 23) {
                        const auto target = guestReceiveRights.find(set);
                        const auto membership = guestPortSetMembers.find(name);
                        if (target == guestReceiveRights.end()) {
                            registers[0] = kernInvalidName;
                        } else if (!target->second.portSet) {
                            registers[0] = kernInvalidRight;
                        } else if (membership == guestPortSetMembers.end() ||
                                   membership->second != set) {
                            registers[0] = kernNotInSet;
                        } else {
                            guestPortSetMembers.erase(membership);
                            registers[0] = kernSuccess;
                        }
                        break;
                    }
                    if (set != 0) {
                        const auto target = guestReceiveRights.find(set);
                        if (target == guestReceiveRights.end()) {
                            registers[0] = kernInvalidName;
                            break;
                        }
                        if (!target->second.portSet) {
                            registers[0] = kernInvalidRight;
                            break;
                        }
                        guestPortSetMembers[name] = set;
                        deliverGuestMachMessage(name);
                    } else {
                        guestPortSetMembers.erase(name);
                    }
                    registers[0] = kernSuccess;
                    break;
                }
                case 40: {  // _kernelrpc_mach_port_get_attributes_trap
                    const auto name = static_cast<std::uint32_t>(registers[1]);
                    const auto flavor = static_cast<std::uint32_t>(registers[2]);
                    if (!machTaskArgumentValid(registers[0])) {
                        registers[0] = machSendInvalidDest;
                        break;
                    }
                    const auto right = guestReceiveRights.find(name);
                    if (right == guestReceiveRights.end()) {
                        registers[0] = kernInvalidName;
                        break;
                    }
                    std::uint32_t count = 0;
                    try {
                        std::memcpy(&count, guestMemory.buffer(registers[4], sizeof(count)),
                                    sizeof(count));
                    } catch (const std::exception&) {
                        registers[0] = kernInvalidAddress;
                        break;
                    }
                    if (flavor == MACH_PORT_LIMITS_INFO) {
                        if (count < MACH_PORT_LIMITS_INFO_COUNT) {
                            registers[0] = kernInvalidArgument;
                            break;
                        }
                        const mach_port_limits_t limits{right->second.qlimit};
                        const std::uint32_t used = MACH_PORT_LIMITS_INFO_COUNT;
                        registers[0] = machStoreGuest(registers[3], &limits, sizeof(limits)) &&
                                machStoreGuest(registers[4], &used, sizeof(used))
                            ? kernSuccess : kernInvalidAddress;
                    } else if (flavor == MACH_PORT_RECEIVE_STATUS) {
                        if (count < MACH_PORT_RECEIVE_STATUS_COUNT) {
                            registers[0] = kernInvalidArgument;
                            break;
                        }
                        mach_port_status_t status{};
                        status.mps_qlimit = right->second.qlimit;
                        const auto queue = guestMachMessageQueues.find(name);
                        status.mps_msgcount = queue == guestMachMessageQueues.end()
                            ? 0 : static_cast<mach_port_msgcount_t>(queue->second.size());
                        const auto membership = guestPortSetMembers.find(name);
                        status.mps_pset = membership == guestPortSetMembers.end() ? 0 : 1;
                        const std::uint32_t used = MACH_PORT_RECEIVE_STATUS_COUNT;
                        registers[0] = machStoreGuest(registers[3], &status, sizeof(status)) &&
                                machStoreGuest(registers[4], &used, sizeof(used))
                            ? kernSuccess : kernInvalidAddress;
                    } else if (guest_survey::enabled()) {
                        survey("attribute", -static_cast<std::int64_t>(syscall),
                               std::to_string(flavor));
                        registers[0] = kernInvalidArgument;
                    } else {
                        throw std::runtime_error(
                            "Unsupported guest mach_port_get_attributes flavor " +
                            std::to_string(flavor));
                    }
                    break;
                }
                case 41:    // _kernelrpc_mach_port_guard_trap
                case 42: {  // _kernelrpc_mach_port_unguard_trap
                    const auto name = static_cast<std::uint32_t>(registers[1]);
                    const auto guard = registers[2];
                    if (!machTaskArgumentValid(registers[0])) {
                        registers[0] = machSendInvalidDest;
                        break;
                    }
                    const auto right = guestReceiveRights.find(name);
                    if (name == 0 || name == machPortDead) {
                        registers[0] = kernInvalidName;
                    } else if (right == guestReceiveRights.end()) {
                        const auto send = guestSendRightUrefs.find(name);
                        registers[0] = send != guestSendRightUrefs.end() && send->second != 0
                            ? kernInvalidRight : kernInvalidName;
                    } else if (right->second.portSet) {
                        registers[0] = kernInvalidRight;
                    } else if (machTrapNumber == 41) {
                        if (guestPortGuards.count(name) != 0 ||
                            (right->second.flags & MPO_CONTEXT_AS_GUARD) != 0) {
                            registers[0] = kernInvalidArgument;
                        } else {
                            guestPortGuards[name] = guard;
                            registers[0] = kernSuccess;
                        }
                    } else {
                        const auto held = guestPortGuards.find(name);
                        if (held != guestPortGuards.end() && held->second == guard) {
                            guestPortGuards.erase(held);
                            registers[0] = kernSuccess;
                        } else {
                            std::vector<std::uint64_t> live(
                                registers, registers + stateBuffer.size() / sizeof(std::uint64_t));
                            const GuestSynchronousFault fault{
                                SIGKILL, GUARD_TYPE_MACH_PORT, name, registers[33]};
                            const auto delivery = signalRuntime.deliver_synchronous(
                                hostServiceState.signals, hostServiceState.threadSignalMask,
                                live, guestMemory, fault, live[37],
                                static_cast<std::uint64_t>(activeEntry),
                                guestScheduler.current_id());
                            if (delivery.outcome == GuestSignalOutcome::ProcessExit) {
                                return delivery.exitStatus;
                            }
                            if (delivery.outcome == GuestSignalOutcome::HandlerEntered) {
                                std::memcpy(stateBuffer.data(), live.data(),
                                            live.size() * sizeof(std::uint64_t));
                                selectKernel(static_cast<std::int64_t>(delivery.handler));
                                continue;
                            }
                            throw std::runtime_error(
                                "Guarded Mach port misuse was not terminated");
                        }
                    }
                    break;
                }
                case 10: {  // _kernelrpc_mach_vm_allocate_trap
                    constexpr std::uint32_t vmFlagsAnywhere = 1U;
                    constexpr std::uint32_t vmFlagsPurgable = 2U;
                    constexpr std::uint32_t vmFlagsMte = 0x2000U;
                    const auto rawFlags = static_cast<std::uint32_t>(registers[3]);
                    const auto flags = rawFlags & 0x00ffffffU;
                    if (!machTaskArgumentValid(registers[0])) {
                        registers[0] = machSendInvalidDest;
                        break;
                    }
                    if ((flags & ~(vmFlagsAnywhere | vmFlagsPurgable | vmFlagsMte)) != 0) {
                        registers[0] = kernInvalidArgument;
                        break;
                    }
                    std::uint64_t address = 0;
                    try {
                        std::memcpy(&address, guestMemory.buffer(registers[1], sizeof(address)),
                                    sizeof(address));
                        (void)guestMemory.buffer(registers[1], sizeof(address), true);
                    } catch (const std::exception&) {
                        registers[0] = kernInvalidAddress;
                        break;
                    }
                    const auto page = bootstrap.pageSize;
                    if (registers[2] == 0) {
                        const std::uint64_t zero = 0;
                        (void)machStoreGuest(registers[1], &zero, sizeof(zero));
                        registers[0] = kernSuccess;
                        break;
                    }
                    if (registers[2] > std::numeric_limits<std::uint64_t>::max() - (page - 1)) {
                        registers[0] = kernNoSpace;
                        break;
                    }
                    const auto size = (registers[2] + page - 1) & ~(page - 1);
                    std::uint32_t result = static_cast<std::uint32_t>(kernNoSpace);
                    if ((flags & vmFlagsAnywhere) != 0) {
                        if (hostServiceState.machVmAllocate) {
                            result = hostServiceState.machVmAllocate(
                                size, page, 3, 3, address, false, 1, rawFlags >> 24U);
                        }
                    } else if (hostServiceState.machVmReserve) {
                        result = hostServiceState.machVmReserve(
                            address, size, 3, 3, 1, rawFlags >> 24U, false);
                    }
                    if (result == kernSuccess) {
                        (void)machStoreGuest(registers[1], &address, sizeof(address));
                    }
                    registers[0] = result;
                    break;
                }
                case 11: {  // _kernelrpc_mach_vm_purgable_control_trap
                    if (!machTaskArgumentValid(registers[0])) {
                        registers[0] = machSendInvalidDest;
                    } else if (const auto region = hostServiceState.machVmRegion
                                   ? hostServiceState.machVmRegion(registers[1]) : std::nullopt;
                               !region || region->begin > registers[1] ||
                               registers[1] >= region->end) {
                        registers[0] = kernInvalidAddress;
                    } else {
                        registers[0] = kernInvalidArgument;
                    }
                    break;
                }
                case 12: {  // _kernelrpc_mach_vm_deallocate_trap
                    if (!machTaskArgumentValid(registers[0])) {
                        registers[0] = machSendInvalidDest;
                    } else if (registers[2] == 0) {
                        registers[0] = kernSuccess;
                    } else if (!hostServiceState.machVmDeallocate) {
                        registers[0] = kernInvalidArgument;
                    } else {
                        registers[0] = hostServiceState.machVmDeallocate(registers[1],
                                                                         registers[2]);
                    }
                    break;
                }
                case 15: {  // _kernelrpc_mach_vm_map_trap, anonymous memory only
                    constexpr std::uint32_t vmFlagsAnywhere = 1U;
                    constexpr std::uint32_t vmFlagsPurgable = 2U;
                    constexpr std::uint32_t vmFlagsMte = 0x2000U;
                    const auto rawFlags = static_cast<std::uint32_t>(registers[4]);
                    const auto flags = rawFlags & 0x00ffffffU;
                    const auto protection = static_cast<std::uint32_t>(registers[5]);
                    const auto mask = registers[3];
                    if (!machTaskArgumentValid(registers[0])) {
                        registers[0] = machSendInvalidDest;
                        break;
                    }
                    if ((flags & ~(vmFlagsAnywhere | vmFlagsPurgable | vmFlagsMte)) != 0) {
                        machTrapHandled = false;
                        break;
                    }
                    if (mask >= (1ULL << 62) || (mask & (mask + 1)) != 0 ||
                        (protection & ~7U) != 0 || (protection & 6U) == 6U) {
                        registers[0] = kernInvalidArgument;
                        break;
                    }
                    std::uint64_t address = 0;
                    try {
                        std::memcpy(&address, guestMemory.buffer(registers[1], sizeof(address)),
                                    sizeof(address));
                        (void)guestMemory.buffer(registers[1], sizeof(address), true);
                    } catch (const std::exception&) {
                        registers[0] = kernInvalidAddress;
                        break;
                    }
                    const auto page = bootstrap.pageSize;
                    if (registers[2] == 0) {
                        registers[0] = kernInvalidArgument;
                        break;
                    }
                    if (registers[2] > std::numeric_limits<std::uint64_t>::max() - (page - 1)) {
                        registers[0] = kernNoSpace;
                        break;
                    }
                    const auto size = (registers[2] + page - 1) & ~(page - 1);
                    std::uint32_t result = static_cast<std::uint32_t>(kernNoSpace);
                    if ((flags & vmFlagsAnywhere) != 0) {
                        if (hostServiceState.machVmAllocate) {
                            result = hostServiceState.machVmAllocate(
                                size, std::max(page, mask + 1), protection, 7, address, false, 1,
                                rawFlags >> 24U);
                        }
                    } else if ((address & mask) != 0) {
                        result = static_cast<std::uint32_t>(kernInvalidArgument);
                    } else if (hostServiceState.machVmReserve) {
                        result = hostServiceState.machVmReserve(
                            address, size, protection, 7, 1, rawFlags >> 24U, false);
                    }
                    if (result == kernSuccess) {
                        (void)machStoreGuest(registers[1], &address, sizeof(address));
                    }
                    registers[0] = result;
                    break;
                }
                default: {
                    const auto outcome = machModels.trap(machTrapNumber, registers);
                    if (outcome == mach_models::TrapOutcome::Handled) break;
                    if (outcome == mach_models::TrapOutcome::Sigsys) {
                        std::vector<std::uint64_t> live(
                            registers, registers + stateBuffer.size() / sizeof(std::uint64_t));
                        const GuestSynchronousFault fault{SIGSYS, 0, registers[33], registers[33]};
                        const auto delivery = signalRuntime.deliver_synchronous(
                            hostServiceState.signals, hostServiceState.threadSignalMask,
                            live, guestMemory, fault, live[37],
                            static_cast<std::uint64_t>(activeEntry),
                            guestScheduler.current_id());
                        if (delivery.outcome == GuestSignalOutcome::ProcessExit) {
                            return delivery.exitStatus;
                        }
                        if (delivery.outcome == GuestSignalOutcome::HandlerEntered) {
                            std::memcpy(stateBuffer.data(), live.data(),
                                        live.size() * sizeof(std::uint64_t));
                            selectKernel(static_cast<std::int64_t>(delivery.handler));
                            continue;
                        }
                    }
                    machTrapHandled = false;
                    break;
                }
                }
                if (machTrapHandled) {
                    registers[34] = 0;
                    continue;
                }
            }
#include "raw_net_syscalls.inc"
            if (syscall == 92) {  // Darwin fcntl syscall.
                const char* traceFcntl = std::getenv("ARM64_METAL_TRACE_FCNTL");
                if (traceFcntl != nullptr && std::strcmp(traceFcntl, "1") == 0) {
                    std::cerr << "arm64-metal fcntl-trace entry=raw-syscall"
                              << " pc=0x" << hexadecimal(registers[33])
                              << " lr=0x" << hexadecimal(registers[30])
                              << " fd=" << static_cast<std::int32_t>(registers[0])
                              << " cmd=" << static_cast<std::int32_t>(registers[1])
                              << " arg=0x" << hexadecimal(registers[2])
                              << " sp=0x" << hexadecimal(registers[31]) << '\n';
                }
            }
            {
                RawProcContext rawProc{registers, guestMemory, hostServiceState, guestScheduler,
                                       signalRuntime, rawProcState, bootstrap};
                rawProc.threadForPort = [&](std::uint32_t port) -> std::optional<std::uint64_t> {
                    for (const auto& entry : guestThreadPortNames) {
                        if (entry.second == port) return entry.first;
                    }
                    return std::nullopt;
                };
                rawProc.acquireTaskPort = [&]() {
                    return acquireGuestFixedSendRight(
                        static_cast<std::uint32_t>(guestMachTaskPort));
                };
                const auto rawProcOutcome = dispatch_raw_proc_syscall(rawProc, syscall);
                if (rawProcOutcome == RawProcOutcome::Exit) return rawProc.exitStatus;
                if (rawProcOutcome == RawProcOutcome::Handled) continue;
            }
            if (guest_survey::enabled()) {
                const auto signedSyscall = static_cast<std::int64_t>(syscall);
                const auto x0 = "x0=0x" + hexadecimal(registers[0]);
                if (signedSyscall < 0) {
                    survey("mach_trap", -signedSyscall, "-", x0);
                    registers[0] = 4;  // KERN_INVALID_ARGUMENT
                    registers[34] = 0;
                } else {
                    survey("syscall", signedSyscall, "-", x0);
                    rawFailQuiet(ENOSYS);
                }
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
