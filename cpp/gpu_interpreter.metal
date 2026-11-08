#include <metal_stdlib>
#include "integer_ops_shared.h"
using namespace metal;

struct InstructionPacket {
    uint operation[6];
    ulong pc;
    ulong successor0;
    ulong successor1;
    uint successorCount;
    uint flags;
};
static_assert(sizeof(InstructionPacket) == 56, "host/device packet size mismatch");

@GUEST_MEMORY_MSL_SOURCE@

inline int find_packet(device const InstructionPacket *packets,
                       uint packetCount, ulong pc) {
    uint low = 0;
    uint high = packetCount;
    while (low < high) {
        const uint middle = low + (high - low) / 2;
        if (packets[middle].pc < pc) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < packetCount && packets[low].pc == pc ? int(low) : -1;
}

kernel void run_interpreter(device ulong *state [[buffer(0)]],
                            device const InstructionPacket *packets [[buffer(1)]],
                            constant uint &packetCount [[buffer(2)]],
                            device uchar *memory [[buffer(3)]],
                            constant ulong &size [[buffer(4)]],
                            device const ulong *writable [[buffer(5)]],
                            device const ulong *resident [[buffer(6)]],
                            device const ulong *config [[buffer(7)]]) {
    ulong pc = state[33];
    for (uint block = 0; block < 8192; ++block) {
        if (state[36] == 0) {
            state[33] = pc;
            state[34] = 1;
            return;
        }
        const int firstIndex = find_packet(packets, packetCount, pc);
        if (firstIndex < 0) {
            state[33] = pc;
            state[34] = 7;
            return;
        }
        const uint firstOpcode = packets[firstIndex].operation[0];
        const bool memoryRetry =
            (firstOpcode == 6u || firstOpcode == 7u ||
             firstOpcode == 13u || firstOpcode == 14u ||
             firstOpcode == 17u || firstOpcode == 18u) &&
            state[35] == (pc | 0x8000000000000000ul);
        if ((packets[firstIndex].flags & 1u) == 0u && !memoryRetry) {
            state[33] = pc;
            state[34] = 7;
            return;
        }
        if (memoryRetry) state[35] = 0ul;
        --state[36];

        bool leftBlock = false;
        for (uint index = uint(firstIndex); index < packetCount; ++index) {
            const InstructionPacket instruction = packets[index];
            if (instruction.pc != pc) {
                state[33] = pc;
                state[34] = 7;
                return;
            }
            const uint opcode = instruction.operation[0];
            if (opcode >= 9u && opcode <= 12u) {
                const uint rd = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const uint immediate = instruction.operation[3];
                const uint shift = instruction.operation[4];
                const uint width = instruction.operation[5];
                if (rd > 31u || rn > 31u || immediate > 0xfffu ||
                    (shift != 0u && shift != 12u) ||
                    (width != 32u && width != 64u)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const bool setFlags = opcode >= 11u;
                const AddSubImmediateResult result = add_sub_immediate_apply(
                    uint(opcode == 10u || opcode == 12u), state[rn], immediate,
                    shift, width, uint(setFlags), uint(state[32]));
                if (!setFlags || rd != 31u) state[rd] = result.value;
                state[32] = result.flags;
                if ((instruction.flags & 2u) != 0u) {
                    if (instruction.successorCount != 1u) {
                        state[33] = pc;
                        state[34] = 7;
                        return;
                    }
                    pc = instruction.successor0;
                    leftBlock = true;
                    break;
                }
                pc = instruction.pc + 4ul;
                if (index + 1 >= packetCount || packets[index + 1].pc != pc) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                continue;
            }
            if (opcode == 19u) {
                const uint rd = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const uint rm = instruction.operation[3];
                const uint packed = instruction.operation[4];
                const uint amount = packed & 63u;
                const uint shiftType = (packed >> 6u) & 3u;
                const uint width = instruction.operation[5];
                const bool setFlags = ((packed >> 9u) & 1u) != 0u;
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rd > 31u || rn > 31u || rm > 31u ||
                    (width != 32u && width != 64u) || shiftType > 2u ||
                    amount >= width || (packed & ~0x3ffu) != 0u ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong lhs = rn == 31u ? 0ul : state[rn];
                const ulong rhs = rm == 31u ? 0ul : state[rm];
                const AddSubImmediateResult result = add_sub_shifted_apply(
                    (packed >> 8u) & 1u, lhs, rhs, shiftType, amount, width,
                    uint(setFlags), uint(state[32]));
                if (rd != 31u) state[rd] = result.value;
                state[32] = result.flags;
                if (blockEnd) {
                    pc = instruction.successor0;
                    leftBlock = true;
                    break;
                }
                pc = instruction.pc + 4ul;
                if (index + 1 >= packetCount || packets[index + 1].pc != pc) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                continue;
            }
            if (opcode == 1u || opcode == 3u || opcode == 4u) {
                const uint rd = instruction.operation[1];
                const uint immediate = instruction.operation[3];
                const uint shift = instruction.operation[4];
                const uint width = instruction.operation[5];
                if ((width != 32u && width != 64u) || rd > 31u ||
                    shift >= width || (shift & 15u) != 0u || immediate > 0xffffu) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                if (rd < 31u) {
                    state[rd] = move_wide_apply(opcode, state[rd], immediate,
                                                shift, width);
                }
                if ((instruction.flags & 2u) != 0u) {
                    if (instruction.successorCount != 1u) {
                        state[33] = pc;
                        state[34] = 7;
                        return;
                    }
                    pc = instruction.successor0;
                    leftBlock = true;
                    break;
                }
                pc = instruction.pc + 4ul;
                if (index + 1 >= packetCount || packets[index + 1].pc != pc) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                continue;
            }
            if (opcode == 5u) {
                if ((instruction.flags & 2u) == 0u ||
                    instruction.successorCount != 1u) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                pc = instruction.successor0;
                leftBlock = true;
                break;
            }
            if (opcode == 15u) {
                if ((instruction.flags & 2u) == 0u ||
                    instruction.successorCount != 2u) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const uint condition = instruction.operation[2];
                const uint flags = uint(state[32]);
                if (condition > 15u) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                pc = arm64_condition_holds(condition, flags)
                    ? instruction.successor0 : instruction.successor1;
                leftBlock = true;
                break;
            }
            if (opcode == 16u) {
                const uint rd = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const uint rm = instruction.operation[3];
                const uint packed = instruction.operation[4];
                const uint variant = packed >> 4u;
                const uint condition = packed & 15u;
                const uint width = instruction.operation[5];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rd > 31u || rn > 31u || rm > 31u || variant > 3u ||
                    (width != 32u && width != 64u) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong lhs = rn == 31u ? 0ul : state[rn];
                const ulong rhs = rm == 31u ? 0ul : state[rm];
                const ulong value = conditional_select_apply(
                    variant, lhs, rhs, condition, uint(state[32]), width);
                if (rd < 31u) state[rd] = value;
                if (blockEnd) {
                    if (instruction.successorCount != 1u) {
                        state[33] = pc;
                        state[34] = 7;
                        return;
                    }
                    pc = instruction.successor0;
                    leftBlock = true;
                    break;
                }
                pc = instruction.pc + 4ul;
                if (index + 1 >= packetCount || packets[index + 1].pc != pc) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                continue;
            }
            if (opcode == 6u || opcode == 7u) {
                const uint rt = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const uint immediate = instruction.operation[3];
                const uint shift = instruction.operation[4];
                const uint width = instruction.operation[5];
                const uint expectedShift = width == 32u ? 2u : 3u;
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rt > 31u || rn > 31u || immediate > 0xfffu ||
                    (width != 32u && width != 64u) || shift != expectedShift ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong address = state[rn] + (ulong(immediate) << shift);
                const uint bytes = width / 8u;
                const ulong offset = guest_offset(address, bytes, size, writable,
                                                   opcode == 7u, state, resident, config);
                if (offset == ~0ul) {
                    state[874] = address;
                    state[33] = pc;
                    state[34] = 2;
                    return;
                }
                if (opcode == 6u) {
                    const ulong value = load_guest_word(memory + offset, bytes);
                    if (rt < 31u) state[rt] = width == 32u ? ulong(uint(value)) : value;
                } else {
                    const ulong value = rt < 31u ? state[rt] : 0ul;
                    store_guest_word(memory + offset, value, bytes);
                }
                if (blockEnd) {
                    pc = instruction.successor0;
                    leftBlock = true;
                    break;
                }
                pc = instruction.pc + 4ul;
                if (index + 1 >= packetCount || packets[index + 1].pc != pc ||
                    (packets[index + 1].flags & 1u) != 0u) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                continue;
            }
            if (opcode == 13u || opcode == 14u) {
                const uint first = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const int displacement = as_type<int>(instruction.operation[3]);
                const uint modeAndSecond = instruction.operation[4];
                const uint second = modeAndSecond & 31u;
                const uint mode = modeAndSecond >> 5u;
                const uint width = instruction.operation[5];
                const uint bytes = width / 8u;
                const bool load = opcode == 13u;
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (first > 31u || rn > 31u || (width != 32u && width != 64u) ||
                    (modeAndSecond & ~0x7fu) != 0u || mode < 1u || mode > 3u ||
                    displacement < -64 * int(bytes) || displacement > 63 * int(bytes) ||
                    displacement % int(bytes) != 0 || (load && first == second) ||
                    (rn != 31u && mode != 2u && (rn == first || rn == second)) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong base = state[rn];
                const ulong address = mode == 1u ? base : base + ulong(displacement);
                const ulong accessBytes = ulong(bytes) * 2ul;
                const ulong offset = guest_offset(address, accessBytes, size, writable,
                                                   !load, state, resident, config);
                if (offset == ~0ul) {
                    state[874] = address;
                    state[33] = pc;
                    state[34] = 2;
                    return;
                }
                if (load) {
                    const ulong firstValue = load_guest_word(memory + offset, bytes);
                    const ulong secondValue =
                        load_guest_word(memory + offset + bytes, bytes);
                    if (first < 31u) {
                        state[first] = width == 32u ? ulong(uint(firstValue)) : firstValue;
                    }
                    if (second < 31u) {
                        state[second] = width == 32u ? ulong(uint(secondValue)) : secondValue;
                    }
                } else {
                    const ulong firstValue = first < 31u
                        ? (width == 32u ? ulong(uint(state[first])) : state[first]) : 0ul;
                    const ulong secondValue = second < 31u
                        ? (width == 32u ? ulong(uint(state[second])) : state[second]) : 0ul;
                    store_guest_word(memory + offset, firstValue, bytes);
                    store_guest_word(memory + offset + bytes, secondValue, bytes);
                }
                if (mode != 2u) state[rn] = base + ulong(displacement);
                if (blockEnd) {
                    pc = instruction.successor0;
                    leftBlock = true;
                    break;
                }
                pc = instruction.pc + 4ul;
                if (index + 1 >= packetCount || packets[index + 1].pc != pc ||
                    (packets[index + 1].flags & 1u) != 0u) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                continue;
            }
            if (opcode == 17u || opcode == 18u) {
                const uint rt = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const int displacement = as_type<int>(instruction.operation[3]);
                const uint mode = instruction.operation[4];
                const uint width = instruction.operation[5];
                const uint bytes = width / 8u;
                const bool load = opcode == 17u;
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rt > 31u || rn > 31u ||
                    (width != 32u && width != 64u) ||
                    mode > 3u || mode == 2u ||
                    displacement < -256 || displacement > 255 ||
                    (mode != 0u && rn != 31u && rn == rt) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong base = state[rn];
                const ulong address = mode == 1u
                    ? base : base + ulong(displacement);
                const ulong offset = guest_offset(address, bytes, size, writable,
                                                   !load, state, resident, config);
                if (offset == ~0ul) {
                    state[874] = address;
                    state[33] = pc;
                    state[34] = 2;
                    return;
                }
                if (load) {
                    const ulong value = load_guest_word(memory + offset, bytes);
                    if (rt < 31u) {
                        state[rt] = width == 32u ? ulong(uint(value)) : value;
                    }
                } else {
                    const ulong value = rt < 31u
                        ? (width == 32u ? ulong(uint(state[rt])) : state[rt])
                        : 0ul;
                    store_guest_word(memory + offset, value, bytes);
                }
                if (mode != 0u) state[rn] = base + ulong(displacement);
                if (blockEnd) {
                    pc = instruction.successor0;
                    leftBlock = true;
                    break;
                }
                pc = instruction.pc + 4ul;
                if (index + 1 >= packetCount || packets[index + 1].pc != pc ||
                    (packets[index + 1].flags & 1u) != 0u) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                continue;
            }
            if (opcode == 8u) {
                const uint source = instruction.operation[2];
                const uint nonzero = instruction.operation[3];
                const uint width = instruction.operation[5];
                if ((instruction.flags & 2u) == 0u ||
                    instruction.successorCount != 2u || source > 31u ||
                    nonzero > 1u || (width != 32u && width != 64u)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                ulong value = source == 31u ? 0ul : state[source];
                if (width == 32u) value = uint(value);
                const bool isZero = value == 0ul;
                const bool takeTarget = nonzero == 0u ? isZero : !isZero;
                pc = takeTarget ? instruction.successor0 : instruction.successor1;
                leftBlock = true;
                break;
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
        if (!leftBlock) {
            state[33] = pc;
            state[34] = 7;
            return;
        }
    }
    state[33] = pc;
    state[34] = state[36] == 0 ? 1 : 8;
}
