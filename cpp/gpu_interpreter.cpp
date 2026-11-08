#include "gpu_interpreter.hpp"
#include "guest_memory_msl.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <stdexcept>
#include <string_view>

namespace arm64metal::gpu_interpreter {
namespace {

void replace_all(std::string& text, std::string_view needle, std::string_view value) {
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        text.replace(position, needle.size(), value);
        position += value.size();
    }
}

}  // namespace

Interpreter::Interpreter(const Backend& backend, std::string_view libraryPath)
    : kernel_(backend.loadComputeLibrary(libraryPath, "run_interpreter")) {}

Interpreter::Interpreter(ComputeKernel kernel) : kernel_(std::move(kernel)) {}

Interpreter Interpreter::fromSourceFile(const Backend& backend,
                                         std::string_view sourcePath) {
    if (sourcePath.empty()) throw std::invalid_argument("Interpreter source path must be nonempty");
    std::ifstream sourceFile(std::string(sourcePath), std::ios::binary);
    if (!sourceFile) {
        throw std::runtime_error("Unable to read interpreter MSL source: " + std::string(sourcePath));
    }
    const std::string source{std::istreambuf_iterator<char>(sourceFile),
                             std::istreambuf_iterator<char>()};
    auto renderedSource = source;
    replace_all(renderedSource, "@GUEST_MEMORY_MSL_SOURCE@", guestMemoryMslSource);
    return Interpreter(backend.compileCompute(renderedSource, "run_interpreter"));
}

Result Interpreter::run(const Backend& backend,
                        const std::vector<InstructionPacket>& packets,
                        ProcessState initialState) const {
    auto stateBuffer = backend.makeSharedBuffer(sizeof(initialState));
    if (stateBuffer.data() == nullptr) {
        throw std::runtime_error("Metal interpreter state allocation failed");
    }
    std::memcpy(stateBuffer.data(), initialState.data(), sizeof(initialState));
    return dispatch(backend, packets, stateBuffer);
}

Result Interpreter::dispatch(const Backend& backend,
                             const std::vector<InstructionPacket>& packets,
                             SharedBuffer& processState,
                             const MemoryAccessContext* memoryAccess) const {
    if (!kernel_) throw std::logic_error("GPU interpreter kernel is not compiled");
    if (packets.empty() || packets.size() > 8192) {
        throw std::invalid_argument("Interpreter packet count is outside [1, 8192]");
    }
    if (processState.data() == nullptr ||
        processState.size() < processRegisterCount * sizeof(std::uint64_t)) {
        throw std::invalid_argument("Interpreter process state buffer is too small");
    }
    const auto* initialState = static_cast<const std::uint64_t*>(processState.data());
    constexpr std::uint64_t guestCodeLimit = std::uint64_t{1} << 48;
    bool hasInitialPc = false;
    std::size_t initialPacketIndex = packets.size();
    bool hasMemoryAccess = false;
    std::uint64_t previousPc = 0;
    for (std::size_t index = 0; index < packets.size(); ++index) {
        const auto& packet = packets[index];
        if (packet.pc >= guestCodeLimit || (packet.pc & 3U) != 0 ||
            (index != 0 && packet.pc <= previousPc)) {
            throw std::invalid_argument("Interpreter packets must have ordered aligned PCs");
        }
        if (packet.pc == initialState[programCounterIndex]) {
            hasInitialPc = true;
            initialPacketIndex = index;
        }
        previousPc = packet.pc;
        if ((packet.flags & ~(packetBlockStart | packetBlockEnd)) != 0 ||
            packet.successorCount > 2) {
            throw std::invalid_argument("Interpreter packet has invalid control-flow metadata");
        }
        for (std::size_t edge = 0; edge < packet.successorCount; ++edge) {
            const auto target = edge == 0 ? packet.successor0 : packet.successor1;
            if (target >= guestCodeLimit || (target & 3U) != 0) {
                throw std::invalid_argument("Interpreter packet successor is outside guest code space");
            }
        }
        const auto& operation = packet.operation;
        const auto opcode = static_cast<translate_core::Opcode>(operation[0]);
        if (opcode == translate_core::Opcode::Return) {
            if ((packet.flags & packetBlockEnd) == 0 || packet.successorCount != 0 ||
                operation[1] != 0 ||
                operation[2] != 30 || operation[3] != 0 || operation[4] != 0 ||
                operation[5] != 64) {
                throw std::invalid_argument("Interpreter RET packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::Branch) {
            if ((packet.flags & packetBlockEnd) == 0 || packet.successorCount != 1 ||
                operation[1] != 0 || operation[2] != 0 || operation[4] != 0 ||
                operation[5] != 64) {
                throw std::invalid_argument("Interpreter B packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::BranchLink) {
            const auto displacement = static_cast<std::int32_t>(operation[3]);
            const auto branchTarget = static_cast<std::int64_t>(packet.pc) +
                displacement;
            if ((packet.flags & packetBlockEnd) == 0 || packet.successorCount != 2 ||
                operation[1] != 0 || operation[2] != 0 || operation[4] != 0 ||
                operation[5] != 64 || (operation[3] & 3U) != 0 ||
                displacement < -(1 << 27) || displacement > ((1 << 27) - 4) ||
                branchTarget < 0 ||
                packet.successor0 != static_cast<std::uint64_t>(branchTarget) ||
                packet.successor1 != packet.pc + 4) {
                throw std::invalid_argument("Interpreter BL packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::BranchZero) {
            const auto branchTarget = static_cast<std::int64_t>(packet.pc) +
                static_cast<std::int32_t>(operation[4]);
            if ((packet.flags & packetBlockEnd) == 0 || packet.successorCount != 2 ||
                operation[1] != 0 || operation[2] > 31 || operation[3] > 1 ||
                (operation[5] != 32 && operation[5] != 64) || branchTarget < 0 ||
                packet.successor0 != static_cast<std::uint64_t>(branchTarget) ||
                packet.successor1 != packet.pc + 4) {
                throw std::invalid_argument("Interpreter CBZ/CBNZ packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::TestBitBranch) {
            const auto branchTarget = static_cast<std::int64_t>(packet.pc) +
                static_cast<std::int32_t>(operation[4]);
            if ((packet.flags & packetBlockEnd) == 0 || packet.successorCount != 2 ||
                operation[1] >= operation[5] || operation[2] > 31 || operation[3] > 1 ||
                (operation[5] != 32 && operation[5] != 64) || branchTarget < 0 ||
                packet.successor0 != static_cast<std::uint64_t>(branchTarget) ||
                packet.successor1 != packet.pc + 4) {
                throw std::invalid_argument("Interpreter TBZ/TBNZ packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::BranchCondition) {
            const auto branchTarget = static_cast<std::int64_t>(packet.pc) +
                static_cast<std::int32_t>(operation[3]);
            if ((packet.flags & packetBlockEnd) == 0 || packet.successorCount != 2 ||
                operation[1] != 0 || operation[2] > 15 || operation[4] != 0 ||
                operation[5] != 64 || branchTarget < 0 ||
                packet.successor0 != static_cast<std::uint64_t>(branchTarget) ||
                packet.successor1 != packet.pc + 4) {
                throw std::invalid_argument("Interpreter B.cond packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::ConditionalSelect) {
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            if (operation[1] > 31 || operation[2] > 31 || operation[3] > 31 ||
                operation[4] > 63 || (operation[5] != 32 && operation[5] != 64) ||
                packet.successorCount != (blockEnd ? 1U : 0U) ||
                (blockEnd && packet.successor0 != packet.pc + 4)) {
                throw std::invalid_argument("Interpreter conditional-select packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::MoveWideZero ||
                   opcode == translate_core::Opcode::MoveWideNot ||
                   opcode == translate_core::Opcode::MoveWideKeep) {
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            if (operation[1] > 31 || operation[2] != 0 || operation[3] > 0xffff ||
                (operation[5] != 32 && operation[5] != 64) ||
                operation[4] >= operation[5] || (operation[4] & 15U) != 0 ||
                (operation[5] == 32 && operation[4] > 16) ||
                packet.successorCount != (blockEnd ? 1U : 0U)) {
                throw std::invalid_argument("Interpreter move-wide packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::AddImmediate ||
                   opcode == translate_core::Opcode::SubImmediate ||
                   opcode == translate_core::Opcode::AddImmediateSetFlags ||
                   opcode == translate_core::Opcode::SubImmediateSetFlags) {
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            if (operation[1] > 31 || operation[2] > 31 ||
                operation[3] > 0xfff ||
                (operation[4] != 0 && operation[4] != 12) ||
                (operation[5] != 32 && operation[5] != 64) ||
                packet.successorCount != (blockEnd ? 1U : 0U)) {
                throw std::invalid_argument("Interpreter add/sub immediate packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::AddSubShiftedRegister) {
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            const auto packed = operation[4];
            const auto shiftType = (packed >> 6U) & 3U;
            const auto amount = packed & 63U;
            if (operation[1] > 31 || operation[2] > 31 || operation[3] > 31 ||
                (operation[5] != 32 && operation[5] != 64) || shiftType > 2 ||
                amount >= operation[5] || (packed & ~0x3ffU) != 0 ||
                packet.successorCount != (blockEnd ? 1U : 0U) ||
                (blockEnd && packet.successor0 != packet.pc + 4)) {
                throw std::invalid_argument(
                    "Interpreter add/sub shifted-register packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::AddSubExtendedRegister) {
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            const auto packed = operation[4];
            const auto amount = packed & 7U;
            if (operation[1] > 31 || operation[2] > 31 || operation[3] > 31 ||
                (operation[5] != 32 && operation[5] != 64) || amount > 4 ||
                (packed & ~0xffU) != 0 ||
                packet.successorCount != (blockEnd ? 1U : 0U) ||
                (blockEnd && packet.successor0 != packet.pc + 4)) {
                throw std::invalid_argument(
                    "Interpreter add/sub extended-register packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::AddSubCarryRegister) {
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            if (operation[1] > 31 || operation[2] > 31 || operation[3] > 31 ||
                (operation[4] & ~3U) != 0 ||
                (operation[5] != 32 && operation[5] != 64) ||
                packet.successorCount != (blockEnd ? 1U : 0U) ||
                (blockEnd && packet.successor0 != packet.pc + 4)) {
                throw std::invalid_argument(
                    "Interpreter ADC/SBC register packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::LogicalShiftedRegister) {
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            const auto packed = operation[4];
            const auto amount = (packed >> 5U) & 63U;
            const auto shiftType = (packed >> 3U) & 3U;
            if (operation[1] > 31 || operation[2] > 31 || operation[3] > 31 ||
                (operation[5] != 32 && operation[5] != 64) || amount >= operation[5] ||
                shiftType > 3 || (packed & ~0x7ffU) != 0 ||
                packet.successorCount != (blockEnd ? 1U : 0U) ||
                (blockEnd && packet.successor0 != packet.pc + 4)) {
                throw std::invalid_argument(
                    "Interpreter logical shifted-register packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::LogicalImmediate) {
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            if (operation[1] > 31 || operation[2] > 127 ||
                (operation[5] != 32 && operation[5] != 64) ||
                (operation[5] == 32 && operation[4] != 0) ||
                packet.successorCount != (blockEnd ? 1U : 0U) ||
                (blockEnd && packet.successor0 != packet.pc + 4)) {
                throw std::invalid_argument(
                    "Interpreter logical-immediate packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::LogicalVariableShiftRegister) {
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            if (operation[1] > 31 || operation[2] > 31 || operation[3] > 31 ||
                operation[4] > 3 ||
                (operation[5] != 32 && operation[5] != 64) ||
                packet.successorCount != (blockEnd ? 1U : 0U) ||
                (blockEnd && packet.successor0 != packet.pc + 4)) {
                throw std::invalid_argument(
                    "Interpreter logical variable-shift packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::Adr) {
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            if (operation[1] > 31 || operation[2] > 1 ||
                operation[3] > 0x1fffffU || operation[4] != 0 ||
                operation[5] != 64 ||
                packet.successorCount != (blockEnd ? 1U : 0U) ||
                (blockEnd && packet.successor0 != packet.pc + 4)) {
                throw std::invalid_argument("Interpreter ADR/ADRP packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::LoadUnsignedImmediate ||
                   opcode == translate_core::Opcode::StoreUnsignedImmediate) {
            hasMemoryAccess = true;
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            const auto width = operation[5];
            const auto expectedShift = width == 8 ? 0U : (width == 16 ? 1U :
                                      (width == 32 ? 2U : 3U));
            if (operation[1] > 31 || operation[2] > 31 || operation[3] > 0xfff ||
                (width != 8 && width != 16 && width != 32 && width != 64) ||
                operation[4] != expectedShift ||
                packet.successorCount != (blockEnd ? 1U : 0U) ||
                (blockEnd && packet.successor0 != packet.pc + 4)) {
                throw std::invalid_argument("Interpreter memory packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::LoadPair ||
                   opcode == translate_core::Opcode::StorePair) {
            hasMemoryAccess = true;
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            const auto width = operation[5];
            const auto elementBytes = width / 8U;
            const auto pairOffset = static_cast<std::int32_t>(operation[3]);
            const auto mode = operation[4] >> 5U;
            const auto second = operation[4] & 31U;
            const bool load = opcode == translate_core::Opcode::LoadPair;
            if (operation[1] > 31 || operation[2] > 31 ||
                (width != 32 && width != 64) ||
                (operation[4] & ~0x7fU) != 0 || mode < 1 || mode > 3 ||
                pairOffset < -64 * static_cast<std::int32_t>(elementBytes) ||
                pairOffset > 63 * static_cast<std::int32_t>(elementBytes) ||
                pairOffset % static_cast<std::int32_t>(elementBytes) != 0 ||
                (load && operation[1] == second) ||
                (operation[2] != 31 && mode != 2 &&
                 (operation[2] == operation[1] || operation[2] == second)) ||
                packet.successorCount != (blockEnd ? 1U : 0U) ||
                (blockEnd && packet.successor0 != packet.pc + 4)) {
                throw std::invalid_argument("Interpreter pair-memory packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::LoadUnscaledWriteback ||
                   opcode == translate_core::Opcode::StoreUnscaledWriteback) {
            hasMemoryAccess = true;
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            const auto mode = operation[4];
            const auto offset = static_cast<std::int32_t>(operation[3]);
            if (operation[1] > 31 || operation[2] > 31 ||
                (operation[5] != 8 && operation[5] != 16 &&
                 operation[5] != 32 && operation[5] != 64) ||
                mode > 3 || mode == 2 || offset < -256 || offset > 255 ||
                (mode != 0 && operation[2] != 31 &&
                 operation[2] == operation[1]) ||
                packet.successorCount != (blockEnd ? 1U : 0U) ||
                (blockEnd && packet.successor0 != packet.pc + 4)) {
                throw std::invalid_argument(
                    "Interpreter scalar indexed-memory packet metadata is invalid");
            }
        } else if (opcode == translate_core::Opcode::LoadSignedScalar) {
            hasMemoryAccess = true;
            const bool blockEnd = (packet.flags & packetBlockEnd) != 0;
            const auto mode = operation[4] & 3U;
            const bool targetIsX = (operation[4] & 4U) != 0;
            const auto offset = static_cast<std::int32_t>(operation[3]);
            const auto width = operation[5];
            const auto bytes = width / 8U;
            const bool validWidth = width == 8 || width == 16 || width == 32;
            const bool validOffset = validWidth && (mode == 0
                ? operation[3] <= 0xfffU * bytes && operation[3] % bytes == 0
                : offset >= -256 && offset <= 255);
            if (operation[1] > 31 || operation[2] > 31 ||
                !validWidth ||
                (operation[4] & ~7U) != 0 || (width == 32 && !targetIsX) ||
                !validOffset || (mode >= 2 && operation[2] != 31 &&
                 operation[2] == operation[1]) ||
                packet.successorCount != (blockEnd ? 1U : 0U) ||
                (blockEnd && packet.successor0 != packet.pc + 4)) {
                throw std::invalid_argument(
                    "Interpreter signed scalar-memory packet metadata is invalid");
            }
        } else {
            throw std::invalid_argument("Interpreter packet contains an unsupported operation");
        }
    }
    if (!hasInitialPc) {
        throw std::invalid_argument("Interpreter process PC is absent from the packet table");
    }
    const auto& initialPacket = packets[initialPacketIndex];
    const auto initialOpcode = static_cast<translate_core::Opcode>(
        initialPacket.operation[0]);
    const bool memoryRetry =
        (initialOpcode == translate_core::Opcode::LoadUnsignedImmediate ||
         initialOpcode == translate_core::Opcode::StoreUnsignedImmediate ||
         initialOpcode == translate_core::Opcode::LoadPair ||
         initialOpcode == translate_core::Opcode::StorePair ||
         initialOpcode == translate_core::Opcode::LoadUnscaledWriteback ||
         initialOpcode == translate_core::Opcode::StoreUnscaledWriteback ||
         initialOpcode == translate_core::Opcode::LoadSignedScalar) &&
        initialState[35] == (initialState[programCounterIndex] | cacheRetryPcMarker);
    if ((initialPacket.flags & packetBlockStart) == 0 && !memoryRetry) {
        throw std::invalid_argument(
            "Interpreter mid-block entry requires a host cache-retry marker");
    }
    if (hasMemoryAccess && (memoryAccess == nullptr ||
        memoryAccess->guestMemoryBuffer == nullptr || memoryAccess->guestSizeBuffer == nullptr ||
        memoryAccess->writableBuffer == nullptr || memoryAccess->residentBuffer == nullptr ||
        memoryAccess->memoryConfigBuffer == nullptr ||
        memoryAccess->guestMemoryBuffer->data() == nullptr ||
        memoryAccess->guestSizeBuffer->data() == nullptr ||
        memoryAccess->writableBuffer->data() == nullptr ||
        memoryAccess->residentBuffer->data() == nullptr ||
        memoryAccess->memoryConfigBuffer->data() == nullptr ||
        processState.size() < 878 * sizeof(std::uint64_t))) {
        throw std::invalid_argument("Interpreter memory operation requires process memory mappings");
    }

    auto packetBuffer = backend.makeSharedBuffer(packets.size() * sizeof(InstructionPacket));
    auto countBuffer = backend.makeSharedBuffer(sizeof(std::uint32_t));
    if (packetBuffer.data() == nullptr || countBuffer.data() == nullptr) {
        throw std::runtime_error("Metal interpreter packet allocation failed");
    }
    std::memcpy(packetBuffer.data(), packets.data(),
                packets.size() * sizeof(InstructionPacket));
    const auto count = static_cast<std::uint32_t>(packets.size());
    std::memcpy(countBuffer.data(), &count, sizeof(count));

    auto* guestMemory = hasMemoryAccess ? memoryAccess->guestMemoryBuffer : &processState;
    auto* guestSize = hasMemoryAccess ? memoryAccess->guestSizeBuffer : &processState;
    auto* writable = hasMemoryAccess ? memoryAccess->writableBuffer : &processState;
    auto* resident = hasMemoryAccess ? memoryAccess->residentBuffer : &processState;
    auto* memoryConfig = hasMemoryAccess ? memoryAccess->memoryConfigBuffer : &processState;
    const std::vector<BufferBinding> bindings{
        {&processState, 0, 0}, {&packetBuffer, 1, 0}, {&countBuffer, 2, 0},
        {guestMemory, 3, 0}, {guestSize, 4, 0}, {writable, 5, 0},
        {resident, 6, 0}, {memoryConfig, 7, 0}};
    const auto timing = backend.dispatch(kernel_, bindings, {1, 1, 1}, {1, 1, 1});
    Result result;
    std::copy_n(static_cast<const std::uint64_t*>(processState.data()),
                processRegisterCount, result.state.begin());
    result.timing = timing;
    return result;
}

}  // namespace arm64metal::gpu_interpreter
