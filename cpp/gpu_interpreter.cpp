#include "gpu_interpreter.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <stdexcept>
#include <string_view>

namespace arm64metal::gpu_interpreter {

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
    return Interpreter(backend.compileCompute(source, "run_interpreter"));
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
                             SharedBuffer& processState) const {
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
    std::uint64_t previousPc = 0;
    for (std::size_t index = 0; index < packets.size(); ++index) {
        const auto& packet = packets[index];
        if (packet.pc >= guestCodeLimit || (packet.pc & 3U) != 0 ||
            (index != 0 && packet.pc <= previousPc)) {
            throw std::invalid_argument("Interpreter packets must have ordered aligned PCs");
        }
        hasInitialPc = hasInitialPc || packet.pc == initialState[programCounterIndex];
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
        } else {
            throw std::invalid_argument("Interpreter packet contains an unsupported operation");
        }
    }
    if (!hasInitialPc) {
        throw std::invalid_argument("Interpreter process PC is absent from the packet table");
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

    const std::vector<BufferBinding> bindings{
        {&processState, 0, 0}, {&packetBuffer, 1, 0}, {&countBuffer, 2, 0}};
    const auto timing = backend.dispatch(kernel_, bindings, {1, 1, 1}, {1, 1, 1});
    Result result;
    std::copy_n(static_cast<const std::uint64_t*>(processState.data()),
                processRegisterCount, result.state.begin());
    result.timing = timing;
    return result;
}

}  // namespace arm64metal::gpu_interpreter
