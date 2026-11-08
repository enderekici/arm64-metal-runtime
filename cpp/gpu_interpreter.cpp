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
        throw std::invalid_argument("Interpreter basic block packet count is outside [1, 8192]");
    }
    if (processState.data() == nullptr ||
        processState.size() < processRegisterCount * sizeof(std::uint64_t)) {
        throw std::invalid_argument("Interpreter process state buffer is too small");
    }
    const auto* initialState = static_cast<const std::uint64_t*>(processState.data());
    if (initialState[programCounterIndex] != packets.front().pc) {
        throw std::invalid_argument("Interpreter process PC must match the first packet");
    }
    constexpr std::uint64_t guestCodeLimit = std::uint64_t{1} << 48;
    if (packets.front().pc >= guestCodeLimit ||
        packets.size() > (guestCodeLimit - packets.front().pc) / 4U) {
        throw std::invalid_argument("Interpreter packet range exceeds guest code space");
    }
    for (std::size_t index = 0; index < packets.size(); ++index) {
        const auto expectedPc = packets.front().pc + index * 4U;
        if ((packets[index].pc & 3U) != 0 || packets[index].pc != expectedPc) {
            throw std::invalid_argument("Interpreter packets must be contiguous aligned instructions");
        }
        const auto& operation = packets[index].operation;
        const auto opcode = static_cast<translate_core::Opcode>(operation[0]);
        if (index + 1 == packets.size()) {
            if (opcode != translate_core::Opcode::Return || operation[1] != 0 ||
                operation[2] != 30 || operation[3] != 0 || operation[4] != 0 ||
                operation[5] != 64) {
                throw std::invalid_argument("Interpreter basic block must end with plain RET X30");
            }
        } else if (opcode != translate_core::Opcode::MoveWideZero ||
                   operation[1] > 31 || operation[2] != 0 ||
                   operation[3] > 0xffff || (operation[5] != 32 && operation[5] != 64) ||
                   operation[4] >= operation[5] || (operation[4] & 15U) != 0 ||
                   (operation[5] == 32 && operation[4] > 16)) {
            throw std::invalid_argument("Interpreter basic block contains an unsupported operation");
        }
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
