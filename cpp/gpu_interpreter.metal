#include <metal_stdlib>
#include "move_wide_shared.h"
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
        if (opcode == 1u || opcode == 3u || opcode == 4u) {
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
            if (rd < 31u) {
                state[rd] = move_wide_apply(opcode, state[rd], immediate,
                                            shift, width);
            }
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
