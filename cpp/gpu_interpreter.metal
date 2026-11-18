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

inline bool is_memory_retry_entry(device const InstructionPacket *packet,
                                  device const ulong *state, ulong pc) {
    const uint opcode = packet->operation[0];
    return (opcode == 6u || opcode == 7u ||
            opcode == 13u || opcode == 14u ||
            opcode == 17u || opcode == 18u ||
            opcode == 20u || opcode == 37u || opcode == 38u) &&
           state[35] == (pc | 0x8000000000000000ul);
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
        const int firstIndex = find_packet(packets, packetCount, pc);
        if (firstIndex < 0) {
            state[33] = pc;
            state[34] = 7;
            return;
        }
        const bool memoryRetry = is_memory_retry_entry(
            packets + firstIndex, state, pc);
        if ((packets[firstIndex].flags & 1u) == 0u && !memoryRetry) {
            state[33] = pc;
            state[34] = 7;
            return;
        }
        if (state[36] == 0) {
            state[33] = pc;
            state[34] = 1;
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
            if (opcode == 45u || opcode == 46u) {  // CFINV/XAFLAG/AXFLAG, RMIF, SETF8/16
                const uint rn = instruction.operation[2];
                const uint immediate = instruction.operation[3];
                const uint shift = instruction.operation[4];
                const uint width = instruction.operation[5];
                const bool valid = opcode == 45u ? (!rn && immediate <= 2u && !shift && !width) :
                    rn <= 31u && (width == 64u ? immediate <= 15u && shift <= 63u :
                        (width == 8u || width == 16u) && !immediate && !shift);
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (instruction.operation[1] || !valid ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                if (opcode == 45u) state[32] = system_flags_apply(immediate, uint(state[32]));
                else state[32] = flag_manipulation_apply(rn == 31u ? 0ul : state[rn],
                                                         immediate, shift, width, uint(state[32]));
                pc += 4ul;
                if (blockEnd) {
                    leftBlock = true;
                    break;
                }
                if (index + 1 >= packetCount || packets[index + 1].pc != pc ||
                    (packets[index + 1].flags & 1u) != 0u) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                continue;
            }
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
            if (opcode == 22u) {
                const uint rd = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const uint rm = instruction.operation[3];
                const uint packed = instruction.operation[4];
                const uint amount = packed & 7u;
                const uint option = (packed >> 3u) & 7u;
                const bool subtract = ((packed >> 6u) & 1u) != 0u;
                const bool setFlags = ((packed >> 7u) & 1u) != 0u;
                const uint width = instruction.operation[5];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rd > 31u || rn > 31u || rm > 31u || amount > 4u ||
                    (width != 32u && width != 64u) || (packed & ~0xffu) != 0u ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong lhs = state[rn];
                const ulong rhs = rm == 31u ? 0ul : state[rm];
                const AddSubImmediateResult result = add_sub_extended_apply(
                    uint(subtract), lhs, rhs, option, amount, width,
                    uint(setFlags), uint(state[32]));
                if (!setFlags || rd != 31u) state[rd] = result.value;
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
            if (opcode == 26u) {
                const uint rd = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const uint rm = instruction.operation[3];
                const uint packed = instruction.operation[4];
                const bool subtract = (packed & 1u) != 0u;
                const bool setFlags = (packed & 2u) != 0u;
                const uint width = instruction.operation[5];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rd > 31u || rn > 31u || rm > 31u || (packed & ~3u) != 0u ||
                    (width != 32u && width != 64u) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong lhs = rn == 31u ? 0ul : state[rn];
                const ulong rhs = rm == 31u ? 0ul : state[rm];
                const AddSubImmediateResult result = add_sub_carry_apply(
                    uint(subtract), lhs, rhs, width, uint(setFlags), uint(state[32]));
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
            if (opcode == 23u) {
                const uint rd = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const uint rm = instruction.operation[3];
                const uint packed = instruction.operation[4];
                const uint opc = packed & 3u;
                const uint invert = (packed >> 2u) & 1u;
                const uint shiftType = (packed >> 3u) & 3u;
                const uint amount = (packed >> 5u) & 63u;
                const uint width = instruction.operation[5];
                const bool setFlags = opc == 3u;
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rd > 31u || rn > 31u || rm > 31u ||
                    (width != 32u && width != 64u) || amount >= width ||
                    (packed & ~0x7ffu) != 0u ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong lhs = rn == 31u ? 0ul : state[rn];
                const ulong rhs = rm == 31u ? 0ul : state[rm];
                const LogicalShiftedResult result = logical_shifted_apply(
                    opc, invert, lhs, rhs, shiftType, amount, width,
                    uint(setFlags), uint(state[32]));
                if (!setFlags || rd != 31u) state[rd] = result.value;
                if (setFlags) state[32] = result.flags;
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
            if (opcode == 24u) {
                const uint rd = instruction.operation[1];
                const uint packedSource = instruction.operation[2];
                const uint opc = packedSource >> 5u;
                const uint rn = packedSource & 31u;
                const uint width = instruction.operation[5];
                const ulong immediate = (ulong(instruction.operation[4]) << 32u) |
                                        ulong(instruction.operation[3]);
                const bool setFlags = opc == 3u;
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rd > 31u || packedSource > 127u || opc > 3u ||
                    (width != 32u && width != 64u) ||
                    (width == 32u && instruction.operation[4] != 0u) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong lhs = rn == 31u ? 0ul : state[rn];
                const LogicalShiftedResult result = logical_immediate_apply(
                    opc, lhs, immediate, width, uint(setFlags), uint(state[32]));
                if (rd != 31u) state[rd] = result.value;
                if (setFlags) state[32] = result.flags;
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
            if (opcode == 29u) {
                const uint rd = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const uint rm = instruction.operation[3];
                const uint shiftType = instruction.operation[4];
                const uint width = instruction.operation[5];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rd > 31u || rn > 31u || rm > 31u || shiftType > 3u ||
                    (width != 32u && width != 64u) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong value = rn == 31u ? 0ul : state[rn];
                const ulong amount = rm == 31u ? 0ul : state[rm];
                const LogicalShiftedResult result = logical_variable_shift_apply(
                    shiftType, value, amount, width, uint(state[32]));
                if (rd != 31u) state[rd] = result.value;
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
            if (opcode == 31u) {
                const uint rd = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const uint rm = instruction.operation[3];
                const uint packed = instruction.operation[4];
                const uint accumulatorRegister = packed & 31u;
                const uint subtract = packed >> 5u;
                const uint width = instruction.operation[5];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rd > 31u || rn > 31u || rm > 31u || packed > 63u ||
                    (width != 32u && width != 64u) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong lhs = rn == 31u ? 0ul : state[rn];
                const ulong rhs = rm == 31u ? 0ul : state[rm];
                const ulong accumulator = accumulatorRegister == 31u
                    ? 0ul : state[accumulatorRegister];
                const ulong result = multiply_accumulate_apply(
                    subtract, lhs, rhs, accumulator, width);
                if (rd != 31u) state[rd] = result;
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
            if (opcode == 33u || opcode == 32u) {
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                if (opcode == 33u) {
                    const uint rd = instruction.operation[1];
                    const uint rn = instruction.operation[2];
                    const uint rotation = instruction.operation[3];
                    const uint packed = instruction.operation[4];
                    const uint variant = packed >> 6u;
                    const uint end = packed & 63u;
                    const uint width = instruction.operation[5];
                    if (rd > 31u || rn > 31u ||
                        (width != 32u && width != 64u) || rotation >= width ||
                        end >= width || variant > 2u || (packed & ~0xffu) != 0u) {
                        state[33] = pc;
                        state[34] = 7;
                        return;
                    }
                    const ulong destination = rd == 31u ? 0ul : state[rd];
                    const ulong source = rn == 31u ? 0ul : state[rn];
                    const ulong result = bitfield_apply(
                        variant, destination, source, rotation, end, width);
                    if (rd != 31u) state[rd] = result;
                } else {
                    const uint rd = instruction.operation[1];
                    const uint rn = instruction.operation[2];
                    const uint immediate = instruction.operation[3];
                    const uint packed = instruction.operation[4];
                    const uint width = instruction.operation[5];
                    const uint kind = packed >> 2u;
                    const uint detail = packed & 3u;
                    const uint maxImmediate = kind == 1u ? 255u : kind == 4u ? 31u : 0u;
                    if (rd > 31u || rn > 31u || immediate > maxImmediate ||
                        (width != 32u && width != 64u) ||
                        (kind == 0u ? packed > 3u :
                         kind == 1u ? packed != 4u :
                         kind == 2u ? packed != 8u :
                         kind == 3u ? packed != 12u :
                         kind == 4u ? (packed < 16u || packed > 19u) :
                         kind == 5u ? (packed != 20u || width != 64u) : true) ||
                        ((kind != 1u && kind != 4u) && immediate != 0u) ||
                        (kind == 4u && (detail & 2u) != 0u && immediate != 0u)) {
                        state[33] = pc;
                        state[34] = 7;
                        return;
                    }
                    ScalarFPResult result = ScalarFPResult{0ul, 0u};
                    if (kind == 0u) {
                        const uint sourceWidth = (packed & 2u) != 0u ? 64u : 32u;
                        const uint signedInput = packed & 1u;
                        const ulong raw = rn == 31u ? 0ul : state[rn];
                        result = scalar_fp_integer_to_float(
                            raw, sourceWidth, signedInput, width, uint(state[870]));
                    } else if (kind == 1u) {
                        result = ScalarFPResult{
                            scalar_fp_expand_immediate(immediate, width), 0u};
                    } else if (kind == 2u) {
                        result = scalar_fp_round_positive(
                            state[806u + 2u * rn], width, uint(state[870]));
                    } else if (kind == 3u) {
                        result = scalar_fp_negate(state[806u + 2u * rn], width);
                    } else if (kind == 5u) {
                        result = ScalarFPResult{state[806u + 2u * rn], 0u};
                    } else {
                        const ulong right = (detail & 2u) != 0u
                            ? 0ul : state[806u + 2u * immediate];
                        result = scalar_fp_compare(
                            state[806u + 2u * rn], right, width, detail & 1u,
                            uint(state[870]));
                    }
                    if (kind == 4u) {
                        state[32] = (state[32] & ~0xful) | result.value;
                    } else {
                        state[806u + 2u * rd] = result.value;
                        state[807u + 2u * rd] = 0ul;
                    }
                    state[871] |= result.fpsrFlags;
                }
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
            if (opcode == 34u) {
                const uint rd = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const uint variant = instruction.operation[3];
                const uint shift = instruction.operation[4];
                const uint width = instruction.operation[5];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rd > 31u || rn > 31u || variant > 3u || shift != 0u ||
                    (width != 32u && width != 64u) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong source = rn == 31u ? 0ul : state[rn];
                const ulong result = integer_bit_operation_apply(variant, source, width);
                if (rd != 31u) state[rd] = result;
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
            if (opcode == 35u) {
                const uint rd = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const uint rm = instruction.operation[3];
                const uint isSigned = instruction.operation[4];
                const uint width = instruction.operation[5];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rd > 31u || rn > 31u || rm > 31u || isSigned > 1u ||
                    (width != 32u && width != 64u) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong lhs = rn == 31u ? 0ul : state[rn];
                const ulong rhs = rm == 31u ? 0ul : state[rm];
                const ulong result = integer_divide_apply(isSigned, lhs, rhs, width);
                if (rd != 31u) state[rd] = result;
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
            if (opcode == 28u) {
                const long displacement = long(int(instruction.operation[3]));
                const ulong target = instruction.pc + ulong(displacement);
                if ((instruction.flags & 2u) == 0u ||
                    instruction.successorCount != 2u ||
                    instruction.operation[1] != 0u ||
                    instruction.operation[2] != 0u ||
                    (instruction.operation[3] & 3u) != 0u ||
                    displacement < -134217728l || displacement > 134217724l ||
                    instruction.operation[4] != 0u ||
                    instruction.operation[5] != 64u ||
                    target != instruction.successor0 ||
                    instruction.successor1 != instruction.pc + 4ul) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                for (ulong authIndex = state[37]; authIndex > 0ul; --authIndex) {
                    const ulong authSlot = 38ul + 3ul * (authIndex - 1ul);
                    if (state[authSlot + 2] >= 0xffffffff00000000ul) {
                        --state[authSlot + 2];
                        break;
                    }
                }
                state[30] = instruction.successor1;
                pc = instruction.successor0;
                leftBlock = true;
                break;
            }
            if (opcode == 30u) {
                const uint rn = instruction.operation[2];
                const uint link = instruction.operation[3];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (!blockEnd || instruction.successorCount != 0u ||
                    instruction.operation[1] != 0u || rn > 31u || link > 1u ||
                    instruction.operation[4] != 0u ||
                    instruction.operation[5] != 64u) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong target = rn == 31u ? 0ul : state[rn];
                if (link != 0u) {
                    for (ulong authIndex = state[37]; authIndex > 0ul; --authIndex) {
                        const ulong authSlot = 38ul + 3ul * (authIndex - 1ul);
                        if (state[authSlot + 2] >= 0xffffffff00000000ul) {
                            --state[authSlot + 2];
                            break;
                        }
                    }
                    state[30] = instruction.pc + 4ul;
                }
                if (target == 0ul || target >= (1ul << 48) ||
                    (target & 3ul) != 0ul) {
                    state[874] = target;
                    state[33] = pc;
                    state[34] = 3;
                    return;
                }
                state[33] = target;
                state[34] = 7;
                return;
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
                const uint expectedShift = width == 8u ? 0u :
                    (width == 16u ? 1u : (width == 32u ? 2u : 3u));
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rt > 31u || rn > 31u || immediate > 0xfffu ||
                    (width != 8u && width != 16u &&
                     width != 32u && width != 64u) || shift != expectedShift ||
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
                const bool ordered = instruction.operation[4] == 8u;
                const uint mode = ordered ? 0u : instruction.operation[4];
                const uint width = instruction.operation[5];
                const uint bytes = width / 8u;
                const bool load = opcode == 17u;
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rt > 31u || rn > 31u ||
                    (width != 8u && width != 16u &&
                     width != 32u && width != 64u) ||
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
                if (ordered && rcpc2_alignment_fault(address, bytes, !load, pc, state)) return;
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
            if (opcode == 20u) {
                const uint rt = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const int displacement = as_type<int>(instruction.operation[3]);
                const uint modeAndWidth = instruction.operation[4];
                const uint mode = modeAndWidth & 3u;
                const bool targetIsX = (modeAndWidth & 4u) != 0u;
                const uint width = instruction.operation[5];
                const uint bytes = width / 8u;
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                const bool validWidth = width == 8u || width == 16u || width == 32u;
                const bool validOffset = validWidth && (mode == 0u
                    ? uint(instruction.operation[3]) <= 0xfffu * bytes &&
                      uint(instruction.operation[3]) % bytes == 0u
                    : displacement >= -256 && displacement <= 255);
                if (rt > 31u || rn > 31u || !validWidth ||
                    (modeAndWidth & ~15u) != 0u || ((modeAndWidth & 8u) != 0 && mode != 1u) ||
                    (width == 32u && !targetIsX) ||
                    !validOffset || (mode >= 2u && rn != 31u && rn == rt) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong base = state[rn];
                const ulong address = mode == 2u
                    ? base : base + ulong(displacement);
                if ((modeAndWidth & 8u) != 0 && rcpc2_alignment_fault(address, bytes, false, pc, state))
                    return;
                const ulong offset = guest_offset(address, bytes, size, writable,
                                                   false, state, resident, config);
                if (offset == ~0ul) {
                    state[874] = address;
                    state[33] = pc;
                    state[34] = 2;
                    return;
                }
                const ulong value = load_guest_word(memory + offset, bytes);
                ulong signedValue = 0ul;
                if (width == 8u) {
                    signedValue = targetIsX
                        ? ulong(long(char(value)))
                        : ulong(uint(int(char(value))));
                } else if (width == 16u) {
                    signedValue = targetIsX
                        ? ulong(long(short(value)))
                        : ulong(uint(int(short(value))));
                } else {
                    signedValue = ulong(long(int(value)));
                }
                if (rt < 31u) state[rt] = signedValue;
                if (mode >= 2u) state[rn] = base + ulong(displacement);
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
            if (opcode == 37u) {
                const uint rt = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const uint rs = instruction.operation[3];
                const uint isMax = instruction.operation[4];
                const uint width = instruction.operation[5];
                const uint bytes = width / 8u;
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rt > 31u || rn > 31u || rs > 31u || isMax > 3u ||
                    (width != 8u && width != 16u &&
                     width != 32u && width != 64u) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong address = state[rn];
                if (rcpc2_alignment_fault(address, bytes, true, instruction.pc, state))
                    return;
                const ulong offset = guest_offset(address, bytes, size, writable,
                                                   true, state, resident, config);
                if (offset == ~0ul) {
                    state[874] = address;
                    state[33] = pc;
                    state[34] = 2;
                    return;
                }
                const ulong oldValue = load_guest_word(memory + offset, bytes);
                const ulong laneMask = width == 64u
                    ? ~0ul : ((1ul << width) - 1ul);
                const ulong sourceValue =
                    (rs == 31u ? 0ul : state[rs]) & laneMask;
                const ulong oldWidthValue = oldValue & laneMask;
                const bool signedCompare = isMax >= 2u;
                const bool maximum = (isMax & 1u) != 0u;
                const uint signShift = 64u - width;
                const long oldSigned =
                    as_type<long>(oldWidthValue << signShift) >> signShift;
                const long sourceSigned =
                    as_type<long>(sourceValue << signShift) >> signShift;
                const bool oldLess = signedCompare ? oldSigned < sourceSigned
                                                   : oldWidthValue < sourceValue;
                const ulong newValue = maximum
                    ? (oldLess ? sourceValue : oldWidthValue)
                    : (oldLess ? oldWidthValue : sourceValue);
                store_guest_word(memory + offset, newValue, bytes);
                if (rt != 31u) state[rt] = oldWidthValue;
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
            if (opcode == 38u) {
                const uint rt = instruction.operation[1];
                const uint kind = instruction.operation[4];
                const uint resultWidth = instruction.operation[5];
                const int displacement = as_type<int>(instruction.operation[3]);
                const uint bytes = kind == 1u ? 8u : 4u;
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                const bool validKind = kind <= 2u &&
                    (kind == 0u ? resultWidth == 32u : resultWidth == 64u);
                if (rt > 31u || instruction.operation[2] != 0u || !validKind ||
                    displacement < -(1 << 20) || displacement > ((1 << 20) - 4) ||
                    (uint(displacement) & 3u) != 0u ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                if ((displacement < 0 && instruction.pc < ulong(-displacement)) ||
                    (displacement >= 0 &&
                     instruction.pc > (~0ul - ulong(displacement)))) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong address = displacement < 0
                    ? instruction.pc - ulong(-displacement)
                    : instruction.pc + ulong(displacement);
                const ulong guestCodeLimit = 1ul << 48;
                if (address >= guestCodeLimit || bytes > guestCodeLimit - address) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const ulong offset = guest_offset(address, bytes, size, writable,
                                                   false, state, resident, config);
                if (offset == ~0ul) {
                    state[874] = address;
                    state[33] = pc;
                    state[34] = 2;
                    return;
                }
                const ulong loaded = load_guest_word(memory + offset, bytes);
                if (rt < 31u) {
                    state[rt] = kind == 2u
                        ? ulong(long(int(loaded)))
                        : (resultWidth == 32u ? ulong(uint(loaded)) : loaded);
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
            if (opcode == 43u || opcode == 44u) {
                const uint vt = instruction.operation[1];
                const uint rn = instruction.operation[2];
                const uint immediate = instruction.operation[3];
                const uint packedMode = instruction.operation[4];
                const uint mode = packedMode & 7u;
                const uint width = instruction.operation[5];
                const uint bytes = width / 8u;
                const int displacement = as_type<int>(immediate);
                const uint rm = (packedMode >> 3u) & 31u;
                const uint option = (packedMode >> 8u) & 7u;
                const uint scaled = (packedMode >> 11u) & 1u;
                const bool load = opcode == 43u;
                const bool literal = mode == 5u;
                const bool registerOffset = mode == 4u;
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                const bool validWidth = width == 32u || width == 64u ||
                    width == 128u || (registerOffset &&
                    (width == 8u || width == 16u));
                const bool validRegisterOffset = !registerOffset ||
                    ((packedMode & ~0xfffu) == 0u &&
                     (option == 2u || option == 3u || option == 6u ||
                      option == 7u) && scaled <= 1u && immediate == 0u);
                const bool validOtherMode = registerOffset || literal ||
                    ((packedMode & ~7u) == 0u &&
                     (mode != 0u || (width >= 32u && displacement >= 0 &&
                      uint(displacement) <= 0xfffu * bytes &&
                      uint(displacement) % bytes == 0u)) &&
                     ((mode < 1u || mode > 3u) ||
                      (width >= 32u && displacement >= -256 &&
                       displacement <= 255)));
                if (vt > 31u || rn > 31u || !validWidth || mode > 5u ||
                    !validRegisterOffset || !validOtherMode ||
                    (literal && (!load || rn != 0u || width < 32u ||
                     bytes > 16u || displacement < -(1 << 20) ||
                     displacement > ((1 << 20) - 4) ||
                     (uint(displacement) & 3u) != 0u)) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }

                ulong base = 0ul;
                ulong address = 0ul;
                if (literal) {
                    if ((displacement < 0 &&
                         instruction.pc < ulong(-displacement)) ||
                        (displacement >= 0 && instruction.pc >
                         (~0ul - ulong(displacement)))) {
                        state[33] = pc;
                        state[34] = 7;
                        return;
                    }
                    address = displacement < 0
                        ? instruction.pc - ulong(-displacement)
                        : instruction.pc + ulong(displacement);
                    const ulong guestCodeLimit = 1ul << 48;
                    if (address >= guestCodeLimit ||
                        bytes > guestCodeLimit - address) {
                        state[33] = pc;
                        state[34] = 7;
                        return;
                    }
                } else {
                    base = state[rn];
                    ulong offset = ulong(displacement);
                    if (registerOffset) {
                        const ulong indexValue = rm == 31u ? 0ul : state[rm];
                        if (option == 2u) {
                            offset = ulong(uint(indexValue));
                        } else if (option == 6u) {
                            offset = ulong(long(int(uint(indexValue))));
                        } else {
                            offset = indexValue;
                        }
                        if (scaled != 0u) {
                            const uint shift = bytes == 16u ? 4u :
                                (bytes == 8u ? 3u : (bytes == 4u ? 2u :
                                 (bytes == 2u ? 1u : 0u)));
                            offset <<= shift;
                        }
                    }
                    address = mode == 2u ? base : base + offset;
                }
                const ulong offset = guest_offset(address, bytes, size, writable,
                                                   !load, state, resident, config);
                if (offset == ~0ul) {
                    state[874] = address;
                    state[33] = pc;
                    state[34] = 2;
                    return;
                }
                const uint slot = 806u + 2u * vt;
                if (load) {
                    state[slot] = load_guest_word(memory + offset,
                                                  min(bytes, 8u));
                    state[slot + 1u] = bytes == 16u
                        ? load_guest_word(memory + offset + 8ul, 8u) : 0ul;
                } else {
                    store_guest_word(memory + offset, state[slot], min(bytes, 8u));
                    if (bytes == 16u) {
                        store_guest_word(memory + offset + 8ul,
                                         state[slot + 1u], 8u);
                    }
                }
                if (!literal && (mode == 2u || mode == 3u)) {
                    state[rn] = base + ulong(displacement);
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
            if (opcode == 39u) {
                const uint vd = instruction.operation[1];
                const uint vn = instruction.operation[2];
                const uint elementWidth = instruction.operation[3];
                const uint vectorWidth = instruction.operation[5];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (vd > 31u || vn > 31u ||
                    (elementWidth != 8u && elementWidth != 16u &&
                     elementWidth != 32u && elementWidth != 64u) ||
                    instruction.operation[4] != 0u ||
                    (vectorWidth != 64u && vectorWidth != 128u) ||
                    (elementWidth == 64u && vectorWidth != 128u) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const uint sourceSlot = 806u + 2u * vn;
                const uint destinationSlot = 806u + 2u * vd;
                const ulong2 source = ulong2(state[sourceSlot], state[sourceSlot + 1u]);
                const ulong2 result = vector_compare_equal_zero_apply(
                    source, elementWidth, vectorWidth);
                state[destinationSlot] = result[0];
                state[destinationSlot + 1u] = result[1];
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
            if (opcode == 40u) {
                const uint vd = instruction.operation[1];
                const uint vn = instruction.operation[2];
                const uint elementWidth = instruction.operation[3];
                const uint variant = instruction.operation[4];
                const uint vectorWidth = instruction.operation[5];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (vd > 31u || vn > 31u ||
                    (elementWidth != 8u && elementWidth != 16u &&
                     elementWidth != 32u) || variant > 3u ||
                    (vectorWidth != 64u && vectorWidth != 128u) ||
                    (elementWidth == 32u && vectorWidth != 128u) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const uint sourceSlot = 806u + 2u * vn;
                const uint destinationSlot = 806u + 2u * vd;
                const ulong2 source = ulong2(state[sourceSlot], state[sourceSlot + 1u]);
                const ulong2 result = vector_reduce_extreme_apply(
                    source, elementWidth, vectorWidth, variant & 1u,
                    (variant >> 1u) & 1u);
                state[destinationSlot] = result[0];
                state[destinationSlot + 1u] = result[1];
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
            if (opcode == 42u) {
                const uint vd = instruction.operation[1];
                const uint vn = instruction.operation[2];
                const uint elementWidth = instruction.operation[3];
                const uint vectorWidth = instruction.operation[5];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (vd > 31u || vn > 31u ||
                    (elementWidth != 8u && elementWidth != 16u &&
                     elementWidth != 32u) || instruction.operation[4] != 0u ||
                    (vectorWidth != 64u && vectorWidth != 128u) ||
                    (elementWidth == 32u && vectorWidth != 128u) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const uint sourceSlot = 806u + 2u * vn;
                const uint destinationSlot = 806u + 2u * vd;
                const ulong2 source = ulong2(state[sourceSlot], state[sourceSlot + 1u]);
                const ulong2 result = vector_reduce_sum_apply(
                    source, elementWidth, vectorWidth);
                state[destinationSlot] = result[0];
                state[destinationSlot + 1u] = result[1];
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
            if (opcode == 47u) {
                const uint vd = instruction.operation[1];
                const uint vn = instruction.operation[2];
                const uint vm = instruction.operation[3];
                const uint dotFlags = instruction.operation[4];
                const uint indexed = (dotFlags >> 1u) & 1u;
                const uint lane = (dotFlags >> 2u) & 3u;
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (vd > 31u || vn > 31u || vm > 31u || dotFlags > 15u ||
                    (indexed == 0u && lane != 0u) ||
                    (instruction.operation[5] != 64u && instruction.operation[5] != 128u) ||
                    (instruction.operation[5] == 64u && indexed != 0u && lane > 1u) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const uint destinationSlot = 806u + 2u * vd;
                const uint leftSlot = 806u + 2u * vn;
                const uint rightSlot = 806u + 2u * vm;
                const ulong2 accumulator = ulong2(state[destinationSlot],
                                                  state[destinationSlot + 1u]);
                const ulong2 left = ulong2(state[leftSlot], state[leftSlot + 1u]);
                const ulong2 right = ulong2(state[rightSlot], state[rightSlot + 1u]);
                const ulong2 result = vector_dot_apply(
                    accumulator, left, right, dotFlags & 1u, indexed, lane,
                    instruction.operation[5]);
                state[destinationSlot] = result[0];
                state[destinationSlot + 1u] = result[1];
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
            if (opcode == 41u) {
                const uint vd = instruction.operation[1];
                const uint vn = instruction.operation[2];
                const uint elementWidth = instruction.operation[3];
                const uint groupWidth = instruction.operation[4];
                const uint vectorWidth = instruction.operation[5];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (vd > 31u || vn > 31u ||
                    (elementWidth != 8u && elementWidth != 16u &&
                     elementWidth != 32u) ||
                    (groupWidth != 16u && groupWidth != 32u &&
                     groupWidth != 64u) || elementWidth >= groupWidth ||
                    groupWidth % elementWidth != 0u ||
                    (vectorWidth != 64u && vectorWidth != 128u) ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                const uint sourceSlot = 806u + 2u * vn;
                const uint destinationSlot = 806u + 2u * vd;
                const ulong2 source = ulong2(state[sourceSlot], state[sourceSlot + 1u]);
                const ulong2 result = vector_reverse_apply(
                    source, elementWidth, vectorWidth, groupWidth);
                state[destinationSlot] = result[0];
                state[destinationSlot + 1u] = result[1];
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
            if (opcode == 25u) {
                const uint bit = instruction.operation[1];
                const uint source = instruction.operation[2];
                const uint nonzero = instruction.operation[3];
                const uint width = instruction.operation[5];
                if ((instruction.flags & 2u) == 0u ||
                    instruction.successorCount != 2u || bit >= width || source > 31u ||
                    nonzero > 1u || (width != 32u && width != 64u)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                ulong value = source == 31u ? 0ul : state[source];
                if (width == 32u) value = uint(value);
                const bool bitSet = ((value >> bit) & 1ul) != 0ul;
                const bool takeTarget = nonzero == 0u ? !bitSet : bitSet;
                pc = takeTarget ? instruction.successor0 : instruction.successor1;
                leftBlock = true;
                break;
            }
            if (opcode == 27u) {
                const uint rd = instruction.operation[1];
                const uint mode = instruction.operation[2];
                const uint immediate = instruction.operation[3];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (rd > 31u || mode > 1u || immediate > 0x1fffffu ||
                    instruction.operation[4] != 0u ||
                    instruction.operation[5] != 64u ||
                    instruction.successorCount != (blockEnd ? 1u : 0u) ||
                    (blockEnd && instruction.successor0 != instruction.pc + 4ul)) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                if (rd < 31u) state[rd] = adr_apply(mode, instruction.pc, immediate);
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
            if (opcode == 2u) {
                const uint source = instruction.operation[2];
                if (source != 30u) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                for (ulong authIndex = state[37]; authIndex > 0ul; --authIndex) {
                    const ulong authSlot = 38ul + 3ul * (authIndex - 1ul);
                    if (state[authSlot + 2] >= 0xffffffff00000000ul) {
                        if (state[authSlot + 2] != ~0ul) {
                            ++state[authSlot + 2];
                        } else {
                            if (authIndex != state[37]) {
                                state[33] = pc;
                                state[34] = 6;
                                return;
                            }
                            --state[37];
                            state[33] = pc;
                            state[34] = 16;
                            return;
                        }
                        break;
                    }
                }
                const ulong target = state[source];
                if (target == (1ul << 48)) {
                    state[34] = 0;
                    return;
                }
                if (target >= (1ul << 48)) {
                    state[34] = 3;
                    return;
                }
                // Return to a typed caller continuation in this packet set
                // without a host round trip. The next outer-loop iteration
                // charges the normal block budget before executing it.
                const int targetIndex = find_packet(packets, packetCount, target);
                if (targetIndex >= 0 && (packets[targetIndex].flags & 1u) != 0u) {
                    pc = target;
                    leftBlock = true;
                    break;
                }
                state[33] = target;
                state[34] = 7;
                return;
            }
            if (opcode == 36u) {
                const uint step = instruction.operation[1];
                const bool blockEnd = (instruction.flags & 2u) != 0u;
                if (step > 2u || instruction.operation[2] != 0u ||
                    instruction.operation[3] != 0u ||
                    instruction.operation[4] != 0u ||
                    instruction.operation[5] != 0u) {
                    state[33] = pc;
                    state[34] = 7;
                    return;
                }
                if (step == 0u) {
                    if (state[37] >= 256ul) {
                        state[34] = 6;
                        return;
                    }
                    const ulong slot = 38ul + 3ul * state[37]++;
                    state[slot] = state[30];
                    state[slot + 1] = state[31];
                    state[slot + 2] = pc;
                } else {
                    if (state[37] == 0ul || state[37] > 256ul) {
                        state[34] = 6;
                        return;
                    }
                    const ulong slot = 38ul + 3ul * (state[37] - 1ul);
                    if (step == 2u && state[slot + 2] >= 0xffffffff00000000ul) {
                        --state[37];
                        state[33] = pc;
                        state[34] = 16;
                        return;
                    }
                    if (state[30] != state[slot] || state[31] != state[slot + 1]) {
                        if (step == 1u) {
                            state[slot + 2] = ~0ul;
                        } else {
                            --state[37];
                            state[33] = pc;
                            state[34] = 16;
                            return;
                        }
                    } else {
                        --state[37];
                        if (step == 2u) {
                            for (ulong authIndex = state[37]; authIndex > 0ul; --authIndex) {
                                const ulong authSlot = 38ul + 3ul * (authIndex - 1ul);
                                if (state[authSlot + 2] >= 0xffffffff00000000ul) {
                                    if (state[authSlot + 2] != ~0ul) {
                                        ++state[authSlot + 2];
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
                if (step == 2u) {
                    const ulong target = state[30];
                    if (target == (1ul << 48)) {
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
    const int continuationIndex = find_packet(packets, packetCount, pc);
    if (continuationIndex < 0) {
        state[34] = 7;
        return;
    }
    const bool memoryRetry = is_memory_retry_entry(
        packets + continuationIndex, state, pc);
    if ((packets[continuationIndex].flags & 1u) == 0u && !memoryRetry) {
        state[34] = 7;
        return;
    }
    state[34] = state[36] == 0 ? 1 : 8;
}
