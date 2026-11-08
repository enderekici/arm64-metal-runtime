#include "gpu_interpreter.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace arm64metal::gpu_interpreter {
namespace {

constexpr std::string_view kInterpreterMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct InstructionPacket {
    uint operation[6];
    ulong pc;
};

kernel void run_interpreter(device ulong *state [[buffer(0)]],
                            device const InstructionPacket *packets [[buffer(1)]],
                            constant uint &packetCount [[buffer(2)]]) {
    if (state[36] == 0) {
        state[34] = 1;
        return;
    }
    --state[36];
    ulong pc = state[33];
    for (uint step = 0; step < packetCount; ++step) {
        const InstructionPacket instruction = packets[step];
        if (instruction.pc != pc) {
            state[33] = pc;
            state[34] = 7;
            return;
        }
        const uint opcode = instruction.operation[0];
        if (opcode == 1u) {
            const uint rd = instruction.operation[1];
            const uint immediate = instruction.operation[3];
            const uint shift = instruction.operation[4];
            const uint width = instruction.operation[5];
            if ((width != 32u && width != 64u) || rd > 31u ||
                shift >= width || (shift & 15u) != 0u ||
                instruction.operation[3] > 0xffffu) {
                state[33] = pc;
                state[34] = 7;
                return;
            }
            ulong value = ulong(immediate) << shift;
            if (width == 32u) value = uint(value);
            if (rd < 31u) state[rd] = value;
            pc = instruction.pc + 4ul;
            continue;
        }
        if (opcode == 2u) {
            const uint source = instruction.operation[2];
            if (source != 30u) {
                state[33] = pc;
                state[34] = 7;
                return;
            }
            const ulong target = state[source];
            if (target == ~0ul) {
                state[34] = 0;
                return;
            }
            if (target >= (1ul << 48)) {
                state[34] = 3;
                return;
            }
            state[33] = target;
            state[34] = 7;
            return;
        }
        state[33] = pc;
        state[34] = 7;
        return;
    }
    state[33] = pc;
    state[34] = 7;
}
)MSL";

}  // namespace

Interpreter::Interpreter(const Backend& backend)
    : kernel_(backend.compileCompute(kInterpreterMsl, "run_interpreter")) {}

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
