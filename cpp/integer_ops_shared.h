#ifndef ARM64METAL_INTEGER_OPS_SHARED_H
#define ARM64METAL_INTEGER_OPS_SHARED_H

#ifndef __METAL_VERSION__
#include <cstdint>
#endif

#define ARM64METAL_MOVE_WIDE_VALUE(opcode, previous, immediate, shift)             \
  ((opcode) == 1u ? (ulong(immediate) << (shift))                                 \
   : (opcode) == 3u ? ~(ulong(immediate) << (shift))                              \
                    : (((previous) & ~(0xfffful << (shift))) |                    \
                       (ulong(immediate) << (shift))))

#define ARM64METAL_ADD_SUB_IMM_VALUE(subtract, lhs, immediate, shift, width)       \
  ((width) == 32u                                                               \
       ? ulong(uint((subtract) != 0u                                             \
                       ? ulong(uint(lhs)) - (ulong(immediate) << (shift))         \
                       : ulong(uint(lhs)) + (ulong(immediate) << (shift))))       \
       : ((subtract) != 0u                                                      \
              ? (lhs) - (ulong(immediate) << (shift))                             \
              : (lhs) + (ulong(immediate) << (shift))))

#define ARM64METAL_ADD_SUB_VALUE(subtract, lhs, rhs, width)                       \
  ((width) == 32u                                                               \
       ? ulong(uint((subtract) != 0u ? uint(lhs) - uint(rhs)                      \
                                     : uint(lhs) + uint(rhs)))                     \
       : ((subtract) != 0u ? (lhs) - (rhs) : (lhs) + (rhs)))

#define ARM64METAL_ADD_SUB_IMM_NZCV(subtract, lhs, rhs, value, width, signBit)    \
  (uint((value) == 0ul) |                                                        \
   (uint(((value) & (signBit)) != 0ul) << 1u) |                                  \
   (uint((subtract) != 0u                                                       \
             ? ((((lhs) ^ (rhs)) & ((lhs) ^ (value)) & (signBit)) != 0ul)        \
             : ((~((lhs) ^ (rhs)) & ((lhs) ^ (value)) & (signBit)) != 0ul))      \
    << 2u) |                                                                      \
   (uint((subtract) != 0u                                                       \
             ? ((lhs) >= (rhs))                                                 \
             : ((width) == 32u ? ((lhs) + (rhs)) > 0xfffffffful                  \
                               : (value) < (lhs)))                              \
    << 3u))

#define ARM64METAL_CONDITION_HOLDS(condition, flags)                             \
  ((condition) >= 14u                                                          \
       ? true                                                                   \
       : ((((condition) >> 1u) == 0u ? (((flags) & 1u) != 0u)                  \
           : ((condition) >> 1u) == 1u ? (((flags) & 8u) != 0u)                \
           : ((condition) >> 1u) == 2u ? (((flags) & 2u) != 0u)                \
           : ((condition) >> 1u) == 3u ? (((flags) & 4u) != 0u)                \
           : ((condition) >> 1u) == 4u ? ((((flags) & 8u) != 0u) &&            \
                                          (((flags) & 1u) == 0u))                \
           : ((condition) >> 1u) == 5u ? ((((flags) & 2u) != 0u) ==             \
                                          (((flags) & 4u) != 0u))                \
           : ((((flags) & 1u) == 0u) &&                                       \
              ((((flags) & 2u) != 0u) == (((flags) & 4u) != 0u))))              \
          != (((condition) & 1u) != 0u)))

#define ARM64METAL_STRINGIFY_INNER(value) #value
#define ARM64METAL_STRINGIFY(value) ARM64METAL_STRINGIFY_INNER(value)

// Guest NZCV is stored internally as C,V,N,Z (bits 3..0).
#define ARM64METAL_SYSTEM_FLAGS \
inline uint system_flags_apply(uint operation, uint prior) { \
  if (operation == 0u) return prior ^ 8u; \
  const bool z = (prior & 1u) != 0u; \
  const bool c = (prior & 8u) != 0u; \
  if (operation == 1u) { \
    return uint(z && c) | (uint(!c && !z) << 1u) | \
           (uint(z && !c) << 2u) | (uint(c || z) << 3u); \
  } \
  const bool v = (prior & 4u) != 0u; \
  return uint(z || v) | (uint(c && !v) << 3u); \
}

#define ARM64METAL_FLAG_MANIPULATION \
inline uint flag_manipulation_apply(ulong source, uint mask, uint shift, uint width, uint prior) { \
  if (width == 64u) { \
    const ulong rotated = (source >> shift) | (source << ((64u - shift) & 63u)); \
    const uint value = uint(rotated & 15ul); \
    const uint packedValue = ((value & 3u) << 2u) | (value >> 2u); \
    const uint packedMask = ((mask & 3u) << 2u) | (mask >> 2u); \
    return (prior & ~packedMask) | (packedValue & packedMask); \
  } \
  const uint bits = uint(source); \
  const uint negative = (bits >> (width - 1u)) & 1u; \
  const uint zero = uint((bits & ((1u << width) - 1u)) == 0u); \
  const uint overflow = ((bits >> width) & 1u) ^ negative; \
  return (prior & 8u) | zero | (negative << 1u) | (overflow << 2u); \
}

#ifdef __METAL_VERSION__
ARM64METAL_SYSTEM_FLAGS
ARM64METAL_FLAG_MANIPULATION
inline ulong adr_apply(uint mode, ulong pc, uint immediate) {
    const long displacement = (immediate & 0x100000u) != 0u
        ? long(immediate) - 0x200000l : long(immediate);
    const ulong base = mode == 0u ? pc : (pc & ~0xffful);
    const long scale = mode == 0u ? 1l : 4096l;
    return base + ulong(displacement * scale);
}
inline ulong move_wide_apply(uint opcode, ulong previous, uint immediate,
                             uint shift, uint width) {
    const ulong value = ARM64METAL_MOVE_WIDE_VALUE(
        opcode, previous, immediate, shift);
    return width == 32u ? ulong(uint(value)) : value;
}
inline ulong2 vector_compare_equal_zero_apply(
    ulong2 source, uint elementWidth, uint vectorWidth) {
    ulong2 result = ulong2(0ul);
    const ulong laneMask = ~0ul >> (64u - elementWidth);
    const uint laneCount = vectorWidth / elementWidth;
    for (uint lane = 0u; lane < laneCount; ++lane) {
        const uint bit = lane * elementWidth;
        const ulong value = (source[bit >> 6u] >> (bit & 63u)) & laneMask;
        const ulong compared = value == 0ul ? laneMask : 0ul;
        result[bit >> 6u] |= compared << (bit & 63u);
    }
    return result;
}
inline ulong2 vector_reduce_extreme_apply(
    ulong2 source, uint elementWidth, uint vectorWidth,
    uint minimum, uint signedLanes) {
    const ulong laneMask = ~0ul >> (64u - elementWidth);
    const ulong signBit = signedLanes != 0u ? 1ul << (elementWidth - 1u) : 0ul;
    ulong best = source[0] & laneMask;
    const uint laneCount = vectorWidth / elementWidth;
    for (uint lane = 1u; lane < laneCount; ++lane) {
        const uint bit = lane * elementWidth;
        const ulong value = (source[bit >> 6u] >> (bit & 63u)) & laneMask;
        if ((minimum != 0u && (value ^ signBit) < (best ^ signBit)) ||
            (minimum == 0u && (value ^ signBit) > (best ^ signBit))) {
            best = value;
        }
    }
    return ulong2(best, 0ul);
}
inline ulong2 vector_reduce_sum_apply(
    ulong2 source, uint elementWidth, uint vectorWidth) {
    const ulong laneMask = ~0ul >> (64u - elementWidth);
    ulong sum = 0ul;
    const uint laneCount = vectorWidth / elementWidth;
    for (uint lane = 0u; lane < laneCount; ++lane) {
        const uint bit = lane * elementWidth;
        sum += (source[bit >> 6u] >> (bit & 63u)) & laneMask;
    }
    return ulong2(sum & laneMask, 0ul);
}
inline ulong2 vector_reverse_apply(
    ulong2 source, uint elementWidth, uint vectorWidth, uint groupWidth) {
    ulong2 result = ulong2(0ul);
    const ulong laneMask = ~0ul >> (64u - elementWidth);
    const uint laneCount = vectorWidth / elementWidth;
    const uint lanesPerGroup = groupWidth / elementWidth;
    for (uint lane = 0u; lane < laneCount; ++lane) {
        const uint groupStart = (lane / lanesPerGroup) * lanesPerGroup;
        const uint sourceLane = groupStart + lanesPerGroup - 1u -
            (lane % lanesPerGroup);
        const uint sourceBit = sourceLane * elementWidth;
        const uint destinationBit = lane * elementWidth;
        const ulong value = (source[sourceBit >> 6u] >> (sourceBit & 63u)) &
            laneMask;
        result[destinationBit >> 6u] |= value << (destinationBit & 63u);
    }
    return result;
}
struct AddSubImmediateResult {
    ulong value;
    uint flags;
};
inline AddSubImmediateResult add_sub_immediate_apply(
    uint subtract, ulong lhs, uint immediate, uint shift, uint width,
    uint setFlags, uint previousFlags) {
    const ulong left = width == 32u ? ulong(uint(lhs)) : lhs;
    const ulong right = ulong(immediate) << shift;
    const ulong value = ARM64METAL_ADD_SUB_VALUE(subtract, left, right, width);
    const ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul;
    const uint flags = setFlags != 0u
        ? ARM64METAL_ADD_SUB_IMM_NZCV(subtract, left, right, value, width, signBit)
        : previousFlags;
    return AddSubImmediateResult{value, flags};
}
inline AddSubImmediateResult add_sub_shifted_apply(
    uint subtract, ulong lhs, ulong rhs, uint shiftType, uint amount,
    uint width, uint setFlags, uint previousFlags) {
    const ulong left = width == 32u ? ulong(uint(lhs)) : lhs;
    const ulong source = width == 32u ? ulong(uint(rhs)) : rhs;
    const ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul;
    ulong shifted = source;
    if (shiftType == 0u) shifted = source << amount;
    else if (shiftType == 1u) shifted = source >> amount;
    else if (shiftType == 2u && amount != 0u) {
        shifted = source >> amount;
        if ((source & signBit) != 0ul) shifted |= (~0ul << (width - amount));
    }
    if (width == 32u) shifted = ulong(uint(shifted));
    const ulong value = ARM64METAL_ADD_SUB_VALUE(subtract, left, shifted, width);
    const uint flags = setFlags != 0u
        ? ARM64METAL_ADD_SUB_IMM_NZCV(subtract, left, shifted, value, width, signBit)
        : previousFlags;
    return AddSubImmediateResult{value, flags};
}
inline AddSubImmediateResult add_sub_carry_apply(
    uint subtract, ulong lhs, ulong rhs, uint width, uint setFlags,
    uint previousFlags) {
    const ulong left = width == 32u ? ulong(uint(lhs)) : lhs;
    const ulong right = width == 32u ? ulong(uint(rhs)) : rhs;
    const ulong carryIn = ulong((previousFlags >> 3u) & 1u);
    const ulong borrowIn = 1ul - carryIn;
    const ulong value = width == 32u
        ? ulong(uint(subtract != 0u ? uint(left) - uint(right) - uint(borrowIn)
                                    : uint(left) + uint(right) + uint(carryIn)))
        : (subtract != 0u ? left - right - borrowIn : left + right + carryIn);
    const ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul;
    const bool carryOut = subtract != 0u
        ? (left > right || (carryIn != 0ul && left == right))
        : (value < left || (carryIn != 0ul && value == left));
    const uint flags = setFlags != 0u
        ? uint(value == 0ul) | (uint((value & signBit) != 0ul) << 1u) |
              (uint(subtract != 0u
                        ? (((left ^ right) & (left ^ value) & signBit) != 0ul)
                        : ((~(left ^ right) & (left ^ value) & signBit) != 0ul))
               << 2u) | (uint(carryOut) << 3u)
        : previousFlags;
    return AddSubImmediateResult{value, flags};
}
inline ulong add_sub_extend_register(ulong source, uint option) {
    if (option == 0u) return source & 0xfful;
    if (option == 1u) return source & 0xfffful;
    if (option == 2u) return source & 0xfffffffful;
    if (option == 4u) {
        const ulong value = source & 0xfful;
        return (value & 0x80ul) != 0ul ? value | ~0xfful : value;
    }
    if (option == 5u) {
        const ulong value = source & 0xfffful;
        return (value & 0x8000ul) != 0ul ? value | ~0xfffful : value;
    }
    if (option == 6u) {
        const ulong value = source & 0xfffffffful;
        return (value & 0x80000000ul) != 0ul
            ? value | ~0xfffffffful : value;
    }
    return source;
}
inline AddSubImmediateResult add_sub_extended_apply(
    uint subtract, ulong lhs, ulong rhs, uint option, uint amount,
    uint width, uint setFlags, uint previousFlags) {
    const ulong left = width == 32u ? ulong(uint(lhs)) : lhs;
    const ulong source = width == 32u ? ulong(uint(rhs)) : rhs;
    ulong right = add_sub_extend_register(source, option) << amount;
    if (width == 32u) right = ulong(uint(right));
    const ulong value = ARM64METAL_ADD_SUB_VALUE(subtract, left, right, width);
    const ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul;
    const uint flags = setFlags != 0u
        ? ARM64METAL_ADD_SUB_IMM_NZCV(subtract, left, right, value, width, signBit)
        : previousFlags;
    return AddSubImmediateResult{value, flags};
}
struct LogicalShiftedResult {
    ulong value;
    uint flags;
};
inline LogicalShiftedResult logical_immediate_apply(
    uint opc, ulong lhs, ulong immediate, uint width, uint setFlags,
    uint previousFlags) {
    const ulong mask = width == 32u ? 0xfffffffful : ~0ul;
    const ulong left = lhs & mask;
    const ulong right = immediate & mask;
    ulong value = left & right;
    if (opc == 1u) value = left | right;
    else if (opc == 2u) value = left ^ right;
    value &= mask;
    uint flags = previousFlags;
    if (setFlags != 0u) {
        const ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul;
        flags = uint(value == 0ul) | (uint((value & signBit) != 0ul) << 1u);
    }
    return LogicalShiftedResult{value, flags};
}
inline LogicalShiftedResult logical_shifted_apply(
    uint opc, uint invert, ulong lhs, ulong rhs, uint shiftType, uint amount,
    uint width, uint setFlags, uint previousFlags) {
    const ulong mask = width == 32u ? 0xfffffffful : ~0ul;
    const ulong source = rhs & mask;
    ulong shifted = source;
    if (shiftType == 0u) shifted = (source << amount) & mask;
    else if (shiftType == 1u) shifted = source >> amount;
    else if (shiftType == 2u && amount != 0u) {
        const ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul;
        shifted = source >> amount;
        if ((source & signBit) != 0ul) shifted |= ~0ul << (width - amount);
        shifted &= mask;
    } else if (shiftType == 3u && amount != 0u) {
        shifted = ((source >> amount) | (source << (width - amount))) & mask;
    }
    if (invert != 0u) shifted = (~shifted) & mask;
    const ulong left = lhs & mask;
    ulong value = left & shifted;
    if (opc == 1u) value = left | shifted;
    else if (opc == 2u) value = left ^ shifted;
    value &= mask;
    uint flags = previousFlags;
    if (setFlags != 0u) {
        const ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul;
        flags = uint(value == 0ul) | (uint((value & signBit) != 0ul) << 1u);
    }
    return LogicalShiftedResult{value, flags};
}
inline LogicalShiftedResult logical_variable_shift_apply(
    uint shiftType, ulong value, ulong amountValue, uint width,
    uint previousFlags) {
    const uint amount = uint(amountValue) & (width - 1u);
    return logical_shifted_apply(1u, 0u, 0ul, value, shiftType, amount,
                                 width, 0u, previousFlags);
}
inline ulong multiply_accumulate_apply(
    uint subtract, ulong lhs, ulong rhs, ulong accumulator, uint width) {
    const ulong mask = width == 32u ? 0xfffffffful : ~0ul;
    const ulong product = (lhs & mask) * (rhs & mask);
    const ulong result = subtract != 0u
        ? (accumulator & mask) - product
        : (accumulator & mask) + product;
    return result & mask;
}
inline ulong bitfield_apply(uint variant, ulong destination, ulong source,
                            uint rotation, uint end, uint width) {
    const ulong valueMask = width == 32u ? 0xfffffffful : ~0ul;
    const uint count = end >= rotation ? end - rotation + 1u : end + 1u;
    const uint position = end >= rotation ? 0u : width - rotation;
    const ulong lowMask = count == 64u ? ~0ul : ((1ul << count) - 1ul);
    ulong extracted = end >= rotation
        ? ((source >> rotation) & lowMask)
        : ((source & lowMask) << position);
    const ulong fieldMask = (lowMask << position) & valueMask;
    if (variant == 2u &&
        ((extracted >> (position + count - 1u)) & 1ul) != 0ul) {
        const uint fieldEnd = position + count;
        const ulong extensionMask = fieldEnd == 64u ? 0ul
            : valueMask & ~((1ul << fieldEnd) - 1ul);
        extracted |= extensionMask;
    }
    if (variant == 1u) {
        return ((destination & ~fieldMask) | extracted) & valueMask;
    }
    return extracted & valueMask;
}
struct ScalarFPResult { ulong value; uint fpsrFlags; };
inline ScalarFPResult scalar_fp_integer_to_float(
    ulong raw, uint sourceWidth, uint signedInput, uint floatWidth, uint fpcr) {
    const ulong sourceMask = sourceWidth == 32u ? 0xfffffffful : ~0ul;
    raw &= sourceMask;
    const bool negative = signedInput != 0u &&
        ((raw & (sourceWidth == 32u ? 0x80000000ul : 0x8000000000000000ul)) != 0ul);
    const ulong magnitude = negative
        ? (sourceWidth == 32u ? ulong(0u - uint(raw)) : 0ul - raw)
        : raw;
    if (magnitude == 0ul) return ScalarFPResult{0ul, 0u};
    const uint fraction = floatWidth == 64u ? 52u : 23u;
    uint exponent = 63u - metal::clz(magnitude);
    ulong significand;
    uint fpsrFlags = 0u;
    if (exponent <= fraction) {
        significand = magnitude << (fraction - exponent);
    } else {
        const uint shift = exponent - fraction;
        significand = magnitude >> shift;
        const ulong remainder = magnitude & ((1ul << shift) - 1ul);
        if (remainder != 0ul) {
            fpsrFlags |= 16u;
            const ulong halfway = 1ul << (shift - 1u);
            const uint mode = (fpcr >> 22u) & 3u;
            const bool roundUp = (mode == 1u && !negative) ||
                (mode == 2u && negative) ||
                (mode == 0u && (remainder > halfway ||
                    (remainder == halfway && (significand & 1ul) != 0ul)));
            significand += ulong(roundUp);
        }
    }
    const uint bias = floatWidth == 64u ? 1023u : 127u;
    const uint signPosition = floatWidth == 64u ? 63u : 31u;
    const ulong value = (ulong(negative) << signPosition) |
        ((ulong(exponent + bias - 1u) << fraction) + significand);
    return ScalarFPResult{value, fpsrFlags};
}
inline ulong scalar_fp_expand_immediate(uint immediate, uint floatWidth) {
    const uint fraction = floatWidth == 64u ? 52u : 23u;
    const uint exponentBits = floatWidth == 64u ? 11u : 8u;
    const uint repeated = (immediate >> 6u) & 1u;
    const ulong exponent = (ulong(repeated ^ 1u) << (exponentBits - 1u)) |
        (repeated != 0u ? ((1ul << (exponentBits - 1u)) - 4ul) : 0ul) |
        ulong((immediate >> 4u) & 3u);
    return (ulong((immediate >> 7u) & 1u) << (fraction + exponentBits)) |
        (exponent << fraction) |
        (ulong(immediate & 15u) << (fraction - 4u));
}
inline ScalarFPResult scalar_fp_round_positive(
    ulong value, uint floatWidth, uint fpcr) {
    const uint fraction = floatWidth == 64u ? 52u : 23u;
    const uint exponentBits = floatWidth == 64u ? 11u : 8u;
    const uint signPosition = fraction + exponentBits;
    const ulong signMask = 1ul << signPosition;
    const ulong fractionMask = (1ul << fraction) - 1ul;
    const ulong exponentLimit = (1ul << exponentBits) - 1ul;
    const ulong valueMask = floatWidth == 64u ? ~0ul : 0xfffffffful;
    value &= valueMask;
    const ulong sign = value & signMask;
    const ulong exponent = (value >> fraction) & exponentLimit;
    const ulong significand = value & fractionMask;
    if (exponent == exponentLimit) {
        if (significand == 0ul) return ScalarFPResult{value, 0u};
        const ulong quietBit = 1ul << (fraction - 1u);
        const uint flags = (significand & quietBit) == 0ul ? 1u : 0u;
        if ((fpcr & (1u << 25u)) != 0u)
            return ScalarFPResult{(exponentLimit << fraction) | quietBit, flags};
        return ScalarFPResult{(value | quietBit) & valueMask, flags};
    }
    if (exponent == 0ul && significand != 0ul && (fpcr & (1u << 24u)) != 0u)
        return ScalarFPResult{sign, 128u};
    if (exponent == 0ul && significand == 0ul) return ScalarFPResult{sign, 0u};
    const int unbiased = exponent == 0ul
        ? (floatWidth == 64u ? -1022 : -126)
        : int(exponent) - (floatWidth == 64u ? 1023 : 127);
    if (unbiased >= int(fraction)) return ScalarFPResult{value, 0u};
    if (unbiased < 0) {
        if (sign != 0ul) return ScalarFPResult{sign, 0u};
        return ScalarFPResult{floatWidth == 64u ? 0x3ff0000000000000ul : 0x3f800000ul, 0u};
    }
    const uint fractionalBits = fraction - uint(unbiased);
    const ulong discardedMask = (1ul << fractionalBits) - 1ul;
    const ulong discarded = value & discardedMask;
    if (discarded == 0ul) return ScalarFPResult{value, 0u};
    ulong rounded = value & ~discardedMask;
    if (sign == 0ul) rounded += 1ul << fractionalBits;
    return ScalarFPResult{rounded & valueMask, 0u};
}
inline ulong integer_bit_operation_apply(uint variant, ulong source,
                                         uint width) {
    const ulong valueMask = width == 32u ? 0xfffffffful : ~0ul;
    const ulong value = source & valueMask;
    if (variant == 0u) {
        ulong result = 0ul;
        for (uint bit = 0u; bit < width; ++bit) {
            result = (result << 1u) | ((value >> bit) & 1ul);
        }
        return result & valueMask;
    }
    if (variant == 1u) {
        ulong result = 0ul;
        for (uint byte = 0u; byte < width / 8u; ++byte) {
            result = (result << 8u) | ((value >> (byte * 8u)) & 0xfful);
        }
        return result & valueMask;
    }
    if (variant == 2u) {
        if (value == 0ul) return width;
        return width == 32u ? metal::clz(uint(value)) : metal::clz(value);
    }
    const ulong signBit = 1ul << (width - 1u);
    const ulong normalized = (value & signBit) != 0ul
        ? value ^ valueMask : value;
    if (normalized == 0ul) return width - 1u;
    const uint leadingZeros = width == 32u
        ? metal::clz(uint(normalized)) : metal::clz(normalized);
    return leadingZeros - 1u;
}
inline ulong integer_divide_apply(uint isSigned, ulong lhs, ulong rhs,
                                  uint width) {
    const ulong valueMask = width == 32u ? 0xfffffffful : ~0ul;
    const ulong left = lhs & valueMask;
    const ulong right = rhs & valueMask;
    if (right == 0ul) return 0ul;
    if (isSigned == 0u) return left / right;
    const ulong signBit = 1ul << (width - 1u);
    const bool leftNegative = (left & signBit) != 0ul;
    const bool rightNegative = (right & signBit) != 0ul;
    const ulong leftMagnitude = leftNegative ? (0ul - left) & valueMask : left;
    const ulong rightMagnitude = rightNegative ? (0ul - right) & valueMask : right;
    const ulong quotient = leftMagnitude / rightMagnitude;
    return ((leftNegative != rightNegative) ? 0ul - quotient : quotient) & valueMask;
}
inline ScalarFPResult scalar_fp_negate(ulong value, uint floatWidth) {
    const ulong signMask = floatWidth == 64u
        ? 0x8000000000000000ul : 0x80000000ul;
    const ulong valueMask = floatWidth == 64u ? ~0ul : 0xfffffffful;
    return ScalarFPResult{(value ^ signMask) & valueMask, 0u};
}
inline ScalarFPResult scalar_fp_compare(
    ulong left, ulong right, uint floatWidth, uint signalAllNans, uint fpcr) {
    const uint fraction = floatWidth == 64u ? 52u : 23u;
    const uint exponentBits = floatWidth == 64u ? 11u : 8u;
    const uint signPosition = fraction + exponentBits;
    const ulong signMask = 1ul << signPosition;
    const ulong fractionMask = (1ul << fraction) - 1ul;
    const ulong exponentLimit = (1ul << exponentBits) - 1ul;
    const ulong valueMask = floatWidth == 64u ? ~0ul : 0xfffffffful;
    left &= valueMask;
    right &= valueMask;
    uint fpsrFlags = 0u;
    const bool leftSubnormal = ((left >> fraction) & exponentLimit) == 0ul &&
                               (left & fractionMask) != 0ul;
    const bool rightSubnormal = ((right >> fraction) & exponentLimit) == 0ul &&
                                (right & fractionMask) != 0ul;
    if ((fpcr & (1u << 24u)) != 0u) {
        if (leftSubnormal) { left &= signMask; fpsrFlags |= 128u; }
        if (rightSubnormal) { right &= signMask; fpsrFlags |= 128u; }
    }
    const ulong leftExponent = (left >> fraction) & exponentLimit;
    const ulong rightExponent = (right >> fraction) & exponentLimit;
    const ulong leftFraction = left & fractionMask;
    const ulong rightFraction = right & fractionMask;
    const bool leftNan = leftExponent == exponentLimit && leftFraction != 0ul;
    const bool rightNan = rightExponent == exponentLimit && rightFraction != 0ul;
    if (leftNan || rightNan) {
        const ulong quietBit = 1ul << (fraction - 1u);
        const bool signaling =
            (leftNan && (leftFraction & quietBit) == 0ul) ||
            (rightNan && (rightFraction & quietBit) == 0ul);
        if (signaling || signalAllNans != 0u) fpsrFlags |= 1u;
        return ScalarFPResult{12u, fpsrFlags}; // Internal Z,N,V,C = 0,0,1,1.
    }
    const ulong leftMagnitude = left & ~signMask;
    const ulong rightMagnitude = right & ~signMask;
    if (leftMagnitude == 0ul && rightMagnitude == 0ul)
        return ScalarFPResult{9u, fpsrFlags}; // Equal: Z=1, C=1.
    const bool leftNegative = (left & signMask) != 0ul;
    const bool rightNegative = (right & signMask) != 0ul;
    const bool less = leftNegative != rightNegative
        ? leftNegative
        : (leftNegative ? leftMagnitude > rightMagnitude
                        : leftMagnitude < rightMagnitude);
    const bool equal = left == right;
    return ScalarFPResult{equal ? 9u : (less ? 2u : 8u), fpsrFlags};
}
inline bool arm64_condition_holds(uint condition, uint flags) {
    return ARM64METAL_CONDITION_HOLDS(condition, flags);
}
inline ulong conditional_select_apply(uint variant, ulong lhs, ulong rhs,
                                      uint condition, uint flags, uint width) {
    ulong selected = lhs;
    if (!arm64_condition_holds(condition, flags)) {
        if (variant == 1u) selected = rhs + 1ul;
        else if (variant == 2u) selected = ~rhs;
        else if (variant == 3u) selected = 0ul - rhs;
        else selected = rhs;
    }
    return width == 32u ? ulong(uint(selected)) : selected;
}
#else
namespace arm64metal {
inline constexpr std::uint64_t adr_apply(std::uint32_t mode, std::uint64_t pc,
                                         std::uint32_t immediate) {
  const auto displacement = (immediate & 0x100000U) != 0U
      ? static_cast<std::int64_t>(immediate) - 0x200000LL
      : static_cast<std::int64_t>(immediate);
  const auto base = mode == 0U ? pc : (pc & ~std::uint64_t{0xfffU});
  const auto scale = mode == 0U ? 1LL : 4096LL;
  return base + static_cast<std::uint64_t>(displacement * scale);
}
struct AddSubCarryResult {
  std::uint64_t value;
  std::uint32_t flags;
};
inline AddSubCarryResult add_sub_carry_apply(
    std::uint32_t subtract, std::uint64_t lhs, std::uint64_t rhs,
    std::uint32_t width, std::uint32_t setFlags,
    std::uint32_t previousFlags) {
  const auto mask = width == 32U ? std::uint64_t{0xffffffffU} : ~std::uint64_t{0};
  const auto left = lhs & mask;
  const auto right = rhs & mask;
  const auto carryIn = (previousFlags >> 3U) & 1U;
  const auto borrowIn = 1U - carryIn;
  std::uint64_t value;
  bool carryOut;
  if (subtract != 0U) {
    value = (left - right - borrowIn) & mask;
    carryOut = left > right || (carryIn != 0U && left == right);
  } else if (width == 32U) {
    const auto sum = left + right + carryIn;
    value = sum & mask;
    carryOut = (sum >> 32U) != 0U;
  } else {
    value = left + right + carryIn;
    carryOut = value < left || (carryIn != 0U && value == left);
  }
  const auto signBit = width == 32U ? std::uint64_t{0x80000000U}
                                    : std::uint64_t{0x8000000000000000ULL};
  const auto overflow = subtract != 0U
      ? (((left ^ right) & (left ^ value) & signBit) != 0U)
      : ((~(left ^ right) & (left ^ value) & signBit) != 0U);
  const auto flags = setFlags != 0U
      ? std::uint32_t(value == 0U) |
            (std::uint32_t((value & signBit) != 0U) << 1U) |
            (std::uint32_t(overflow) << 2U) | (std::uint32_t(carryOut) << 3U)
      : previousFlags;
  return {value, flags};
}
struct LogicalShiftedResult {
  std::uint64_t value;
  std::uint32_t flags;
};
inline constexpr LogicalShiftedResult logical_immediate_apply(
    std::uint32_t opc, std::uint64_t lhs, std::uint64_t immediate,
    std::uint32_t width, std::uint32_t setFlags,
    std::uint32_t previousFlags) {
  const auto mask = width == 32U ? std::uint64_t{0xffffffffU} : ~std::uint64_t{0};
  const auto left = lhs & mask;
  const auto right = immediate & mask;
  auto value = left & right;
  if (opc == 1U) value = left | right;
  else if (opc == 2U) value = left ^ right;
  value &= mask;
  auto flags = previousFlags;
  if (setFlags != 0U) {
    const auto signBit = width == 32U ? 0x80000000ULL : 0x8000000000000000ULL;
    flags = static_cast<std::uint32_t>(value == 0U) |
            (static_cast<std::uint32_t>((value & signBit) != 0U) << 1U);
  }
  return {value, flags};
}
inline constexpr LogicalShiftedResult logical_shifted_apply(
    std::uint32_t opc, std::uint32_t invert, std::uint64_t lhs,
    std::uint64_t rhs, std::uint32_t shiftType, std::uint32_t amount,
    std::uint32_t width, std::uint32_t setFlags,
    std::uint32_t previousFlags) {
  const auto mask = width == 32U ? std::uint64_t{0xffffffffU} : ~std::uint64_t{0};
  const auto source = rhs & mask;
  std::uint64_t shifted = source;
  if (shiftType == 0U) shifted = (source << amount) & mask;
  else if (shiftType == 1U) shifted = source >> amount;
  else if (shiftType == 2U && amount != 0U) {
    const auto signBit = width == 32U ? 0x80000000ULL : 0x8000000000000000ULL;
    shifted = source >> amount;
    if ((source & signBit) != 0U) shifted |= ~std::uint64_t{0} << (width - amount);
    shifted &= mask;
  } else if (shiftType == 3U && amount != 0U) {
    shifted = ((source >> amount) | (source << (width - amount))) & mask;
  }
  if (invert != 0U) shifted = (~shifted) & mask;
  const auto left = lhs & mask;
  auto value = left & shifted;
  if (opc == 1U) value = left | shifted;
  else if (opc == 2U) value = left ^ shifted;
  value &= mask;
  auto flags = previousFlags;
  if (setFlags != 0U) {
    const auto signBit = width == 32U ? 0x80000000ULL : 0x8000000000000000ULL;
    flags = static_cast<std::uint32_t>(value == 0U) |
            (static_cast<std::uint32_t>((value & signBit) != 0U) << 1U);
  }
  return {value, flags};
}
inline constexpr LogicalShiftedResult logical_variable_shift_apply(
    std::uint32_t shiftType, std::uint64_t value,
    std::uint64_t amountValue, std::uint32_t width,
    std::uint32_t previousFlags) {
  const auto amount = static_cast<std::uint32_t>(amountValue) & (width - 1U);
  return logical_shifted_apply(1U, 0U, 0U, value, shiftType, amount,
                               width, 0U, previousFlags);
}
inline constexpr std::uint64_t multiply_accumulate_apply(
    std::uint32_t subtract, std::uint64_t lhs, std::uint64_t rhs,
    std::uint64_t accumulator, std::uint32_t width) {
  const auto mask = width == 32U ? std::uint64_t{0xffffffffU}
                                 : ~std::uint64_t{0};
  const auto product = (lhs & mask) * (rhs & mask);
  const auto result = subtract != 0U ? (accumulator & mask) - product
                                     : (accumulator & mask) + product;
  return result & mask;
}
inline constexpr std::uint64_t bitfield_apply(
    std::uint32_t variant, std::uint64_t destination, std::uint64_t source,
    std::uint32_t rotation, std::uint32_t end, std::uint32_t width) {
  const auto valueMask = width == 32U ? std::uint64_t{0xffffffffU}
                                      : ~std::uint64_t{0};
  const auto count = end >= rotation ? end - rotation + 1U : end + 1U;
  const auto position = end >= rotation ? 0U : width - rotation;
  const auto lowMask = count == 64U ? ~std::uint64_t{0}
                                    : ((std::uint64_t{1} << count) - 1U);
  auto extracted = end >= rotation
      ? ((source >> rotation) & lowMask)
      : ((source & lowMask) << position);
  const auto fieldMask = (lowMask << position) & valueMask;
  if (variant == 2U &&
      ((extracted >> (position + count - 1U)) & 1U) != 0U) {
    const auto fieldEnd = position + count;
    const auto extensionMask = fieldEnd == 64U ? 0U
        : valueMask & ~((std::uint64_t{1} << fieldEnd) - 1U);
    extracted |= extensionMask;
  }
  if (variant == 1U) {
    return ((destination & ~fieldMask) | extracted) & valueMask;
  }
  return extracted & valueMask;
}
struct ScalarFPResult {
  std::uint64_t value;
  std::uint32_t fpsrFlags;
};
inline constexpr ScalarFPResult scalar_fp_integer_to_float(
    std::uint64_t raw, std::uint32_t sourceWidth, std::uint32_t signedInput,
    std::uint32_t floatWidth, std::uint32_t fpcr) {
  const auto sourceMask = sourceWidth == 32U
      ? std::uint64_t{0xffffffffU} : ~std::uint64_t{0};
  raw &= sourceMask;
  const bool negative = signedInput != 0U &&
      ((raw & (sourceWidth == 32U ? 0x80000000ULL : 0x8000000000000000ULL)) != 0U);
  const auto magnitude = negative
      ? (sourceWidth == 32U
             ? static_cast<std::uint64_t>(std::uint32_t{0U} -
                                          static_cast<std::uint32_t>(raw))
             : std::uint64_t{0U} - raw)
      : raw;
  if (magnitude == 0U) return {0U, 0U};
  const std::uint32_t fraction = floatWidth == 64U ? 52U : 23U;
  std::uint32_t exponent = 0U;
  for (auto remaining = magnitude; remaining > 1U; remaining >>= 1U) ++exponent;
  std::uint64_t significand = 0;
  std::uint32_t fpsrFlags = 0U;
  if (exponent <= fraction) {
    significand = magnitude << (fraction - exponent);
  } else {
    const auto shift = exponent - fraction;
    significand = magnitude >> shift;
    const auto remainder = magnitude & ((std::uint64_t{1} << shift) - 1U);
    if (remainder != 0U) {
      fpsrFlags |= 16U;
      const auto halfway = std::uint64_t{1} << (shift - 1U);
      const auto mode = (fpcr >> 22U) & 3U;
      const bool roundUp = (mode == 1U && !negative) ||
          (mode == 2U && negative) ||
          (mode == 0U && (remainder > halfway ||
              (remainder == halfway && (significand & 1U) != 0U)));
      significand += static_cast<std::uint64_t>(roundUp);
    }
  }
  const std::uint32_t bias = floatWidth == 64U ? 1023U : 127U;
  const std::uint32_t signPosition = floatWidth == 64U ? 63U : 31U;
  const auto value = (std::uint64_t{negative} << signPosition) |
      ((std::uint64_t{exponent + bias - 1U} << fraction) + significand);
  return {value, fpsrFlags};
}
inline constexpr std::uint64_t scalar_fp_expand_immediate(
    std::uint32_t immediate, std::uint32_t floatWidth) {
  const std::uint32_t fraction = floatWidth == 64U ? 52U : 23U;
  const std::uint32_t exponentBits = floatWidth == 64U ? 11U : 8U;
  const auto repeated = (immediate >> 6U) & 1U;
  const auto exponent = (std::uint64_t{repeated ^ 1U} << (exponentBits - 1U)) |
      (repeated != 0U ? ((std::uint64_t{1} << (exponentBits - 1U)) - 4U) : 0U) |
      ((immediate >> 4U) & 3U);
  return (std::uint64_t{(immediate >> 7U) & 1U} << (fraction + exponentBits)) |
      (exponent << fraction) |
      (std::uint64_t{immediate & 15U} << (fraction - 4U));
}
inline constexpr ScalarFPResult scalar_fp_round_positive(
    std::uint64_t value, std::uint32_t floatWidth, std::uint32_t fpcr) {
  const std::uint32_t fraction = floatWidth == 64U ? 52U : 23U;
  const std::uint32_t exponentBits = floatWidth == 64U ? 11U : 8U;
  const std::uint32_t signPosition = fraction + exponentBits;
  const auto signMask = std::uint64_t{1} << signPosition;
  const auto fractionMask = (std::uint64_t{1} << fraction) - 1U;
  const auto exponentLimit = (std::uint64_t{1} << exponentBits) - 1U;
  const auto valueMask = floatWidth == 64U ? ~std::uint64_t{0} : std::uint64_t{0xffffffffU};
  value &= valueMask;
  const auto sign = value & signMask;
  const auto exponent = (value >> fraction) & exponentLimit;
  const auto significand = value & fractionMask;
  if (exponent == exponentLimit) {
    if (significand == 0U) return {value, 0U};
    const auto quietBit = std::uint64_t{1} << (fraction - 1U);
    const auto flags = (significand & quietBit) == 0U ? 1U : 0U;
    if ((fpcr & (1U << 25U)) != 0U)
      return {(exponentLimit << fraction) | quietBit, flags};
    return {(value | quietBit) & valueMask, flags};
  }
  if (exponent == 0U && significand != 0U && (fpcr & (1U << 24U)) != 0U)
    return {sign, 128U};
  if (exponent == 0U && significand == 0U) return {sign, 0U};
  const int unbiased = exponent == 0U
      ? (floatWidth == 64U ? -1022 : -126)
      : static_cast<int>(exponent) - (floatWidth == 64U ? 1023 : 127);
  if (unbiased >= static_cast<int>(fraction)) return {value, 0U};
  if (unbiased < 0) {
    if (sign != 0U) return {sign, 0U};
    return {floatWidth == 64U ? 0x3ff0000000000000ULL : 0x3f800000ULL, 0U};
  }
  const auto fractionalBits = fraction - static_cast<std::uint32_t>(unbiased);
  const auto discardedMask = (std::uint64_t{1} << fractionalBits) - 1U;
  const auto discarded = value & discardedMask;
  if (discarded == 0U) return {value, 0U};
  auto rounded = value & ~discardedMask;
  if (sign == 0U) rounded += std::uint64_t{1} << fractionalBits;
  return {rounded & valueMask, 0U};
}
inline std::uint64_t integer_bit_operation_apply(
    std::uint32_t variant, std::uint64_t source, std::uint32_t width) {
  const auto valueMask = width == 32U ? std::uint64_t{0xffffffffU}
                                      : ~std::uint64_t{0};
  const auto value = source & valueMask;
  if (variant == 0U) {
    std::uint64_t result = 0U;
    for (std::uint32_t bit = 0; bit < width; ++bit) {
      result = (result << 1U) | ((value >> bit) & 1U);
    }
    return result & valueMask;
  }
  if (variant == 1U) {
    std::uint64_t result = 0U;
    for (std::uint32_t byte = 0; byte < width / 8U; ++byte) {
      result = (result << 8U) | ((value >> (byte * 8U)) & 0xffU);
    }
    return result & valueMask;
  }
  if (variant == 2U) {
    if (value == 0U) return width;
    return width == 32U ? __builtin_clz(static_cast<std::uint32_t>(value))
                        : __builtin_clzll(value);
  }
  const auto signBit = std::uint64_t{1} << (width - 1U);
  const auto normalized = (value & signBit) != 0U ? value ^ valueMask : value;
  if (normalized == 0U) return width - 1U;
  const auto leadingZeros = width == 32U
      ? __builtin_clz(static_cast<std::uint32_t>(normalized))
      : __builtin_clzll(normalized);
  return leadingZeros - 1U;
}
inline std::uint64_t integer_divide_apply(std::uint32_t isSigned,
                                          std::uint64_t lhs,
                                          std::uint64_t rhs,
                                          std::uint32_t width) {
  const auto valueMask = width == 32U ? std::uint64_t{0xffffffffU}
                                      : ~std::uint64_t{0};
  const auto left = lhs & valueMask;
  const auto right = rhs & valueMask;
  if (right == 0U) return 0U;
  if (isSigned == 0U) return left / right;
  const auto signBit = std::uint64_t{1} << (width - 1U);
  const bool leftNegative = (left & signBit) != 0U;
  const bool rightNegative = (right & signBit) != 0U;
  const auto leftMagnitude = leftNegative ? (0U - left) & valueMask : left;
  const auto rightMagnitude = rightNegative ? (0U - right) & valueMask : right;
  const auto quotient = leftMagnitude / rightMagnitude;
  return ((leftNegative != rightNegative) ? 0U - quotient : quotient) & valueMask;
}
inline constexpr ScalarFPResult scalar_fp_negate(
    std::uint64_t value, std::uint32_t floatWidth) {
  const auto signMask = floatWidth == 64U ? 0x8000000000000000ULL : 0x80000000ULL;
  const auto valueMask = floatWidth == 64U ? ~std::uint64_t{0} : std::uint64_t{0xffffffffU};
  return {(value ^ signMask) & valueMask, 0U};
}
inline constexpr ScalarFPResult scalar_fp_compare(
    std::uint64_t left, std::uint64_t right, std::uint32_t floatWidth,
    std::uint32_t signalAllNans, std::uint32_t fpcr) {
  const std::uint32_t fraction = floatWidth == 64U ? 52U : 23U;
  const std::uint32_t exponentBits = floatWidth == 64U ? 11U : 8U;
  const std::uint32_t signPosition = fraction + exponentBits;
  const auto signMask = std::uint64_t{1} << signPosition;
  const auto fractionMask = (std::uint64_t{1} << fraction) - 1U;
  const auto exponentLimit = (std::uint64_t{1} << exponentBits) - 1U;
  const auto valueMask = floatWidth == 64U ? ~std::uint64_t{0} : std::uint64_t{0xffffffffU};
  left &= valueMask;
  right &= valueMask;
  std::uint32_t fpsrFlags = 0U;
  const bool leftSubnormal = ((left >> fraction) & exponentLimit) == 0U &&
                             (left & fractionMask) != 0U;
  const bool rightSubnormal = ((right >> fraction) & exponentLimit) == 0U &&
                              (right & fractionMask) != 0U;
  if ((fpcr & (1U << 24U)) != 0U) {
    if (leftSubnormal) { left &= signMask; fpsrFlags |= 128U; }
    if (rightSubnormal) { right &= signMask; fpsrFlags |= 128U; }
  }
  const auto leftExponent = (left >> fraction) & exponentLimit;
  const auto rightExponent = (right >> fraction) & exponentLimit;
  const auto leftFraction = left & fractionMask;
  const auto rightFraction = right & fractionMask;
  const bool leftNan = leftExponent == exponentLimit && leftFraction != 0U;
  const bool rightNan = rightExponent == exponentLimit && rightFraction != 0U;
  if (leftNan || rightNan) {
    const auto quietBit = std::uint64_t{1} << (fraction - 1U);
    const bool signaling =
        (leftNan && (leftFraction & quietBit) == 0U) ||
        (rightNan && (rightFraction & quietBit) == 0U);
    if (signaling || signalAllNans != 0U) fpsrFlags |= 1U;
    return {12U, fpsrFlags};
  }
  const auto leftMagnitude = left & ~signMask;
  const auto rightMagnitude = right & ~signMask;
  if (leftMagnitude == 0U && rightMagnitude == 0U) return {9U, fpsrFlags};
  const bool leftNegative = (left & signMask) != 0U;
  const bool rightNegative = (right & signMask) != 0U;
  const bool less = leftNegative != rightNegative
      ? leftNegative
      : (leftNegative ? leftMagnitude > rightMagnitude
                      : leftMagnitude < rightMagnitude);
  const bool equal = left == right;
  return {equal ? 9U : (less ? 2U : 8U), fpsrFlags};
}
inline constexpr bool arm64_condition_holds(std::uint32_t condition,
                                            std::uint32_t flags) {
  return ARM64METAL_CONDITION_HOLDS(condition, flags);
}
inline constexpr std::uint64_t conditional_select_apply(
    std::uint32_t variant, std::uint64_t lhs, std::uint64_t rhs,
    std::uint32_t condition, std::uint32_t flags, std::uint32_t width) {
  const auto selected = arm64_condition_holds(condition, flags)
      ? lhs
      : variant == 1U ? rhs + 1U
      : variant == 2U ? ~rhs
      : variant == 3U ? 0U - rhs
                      : rhs;
  return width == 32U ? static_cast<std::uint32_t>(selected) : selected;
}
inline constexpr char typed_integer_msl_source[] =
    ARM64METAL_STRINGIFY(ARM64METAL_SYSTEM_FLAGS) "\n"
    ARM64METAL_STRINGIFY(ARM64METAL_FLAG_MANIPULATION) "\n"
    "inline ulong adr_apply(uint mode, ulong pc, uint immediate) { "
    "long displacement = (immediate & 0x100000u) != 0u "
    "? long(immediate) - 0x200000l : long(immediate); "
    "ulong base = mode == 0u ? pc : (pc & ~0xffful); "
    "long scale = mode == 0u ? 1l : 4096l; "
    "return base + ulong(displacement * scale); }\n"
    "inline ulong move_wide_apply(uint opcode, ulong previous, uint immediate, "
    "uint shift, uint width) { ulong value = "
    ARM64METAL_STRINGIFY(ARM64METAL_MOVE_WIDE_VALUE(
        opcode, previous, immediate, shift))
    "; return width == 32u ? ulong(uint(value)) : value; }\n"
    "inline ulong2 vector_compare_equal_zero_apply(ulong2 source, "
    "uint elementWidth, uint vectorWidth) { ulong2 result = ulong2(0ul); "
    "ulong laneMask = ~0ul >> (64u - elementWidth); "
    "uint laneCount = vectorWidth / elementWidth; "
    "for (uint lane = 0u; lane < laneCount; ++lane) { "
    "uint bit = lane * elementWidth; "
    "ulong value = (source[bit >> 6u] >> (bit & 63u)) & laneMask; "
    "ulong compared = value == 0ul ? laneMask : 0ul; "
    "result[bit >> 6u] |= compared << (bit & 63u); } return result; }\n"
    "inline ulong2 vector_reduce_extreme_apply(ulong2 source, "
    "uint elementWidth, uint vectorWidth, uint minimum, uint signedLanes) { "
    "ulong laneMask = ~0ul >> (64u - elementWidth); "
    "ulong signBit = signedLanes != 0u ? 1ul << (elementWidth - 1u) : 0ul; "
    "ulong best = source[0] & laneMask; uint laneCount = vectorWidth / elementWidth; "
    "for (uint lane = 1u; lane < laneCount; ++lane) { "
    "uint bit = lane * elementWidth; "
    "ulong value = (source[bit >> 6u] >> (bit & 63u)) & laneMask; "
    "if ((minimum != 0u && (value ^ signBit) < (best ^ signBit)) || "
    "(minimum == 0u && (value ^ signBit) > (best ^ signBit))) best = value; } "
    "return ulong2(best, 0ul); }\n"
    "inline ulong2 vector_reduce_sum_apply(ulong2 source, uint elementWidth, "
    "uint vectorWidth) { ulong laneMask = ~0ul >> (64u - elementWidth); "
    "ulong sum = 0ul; uint laneCount = vectorWidth / elementWidth; "
    "for (uint lane = 0u; lane < laneCount; ++lane) { "
    "uint bit = lane * elementWidth; "
    "sum += (source[bit >> 6u] >> (bit & 63u)) & laneMask; } "
    "return ulong2(sum & laneMask, 0ul); }\n"
    "inline ulong2 vector_reverse_apply(ulong2 source, uint elementWidth, "
    "uint vectorWidth, uint groupWidth) { ulong2 result = ulong2(0ul); "
    "ulong laneMask = ~0ul >> (64u - elementWidth); "
    "uint laneCount = vectorWidth / elementWidth; "
    "uint lanesPerGroup = groupWidth / elementWidth; "
    "for (uint lane = 0u; lane < laneCount; ++lane) { "
    "uint groupStart = (lane / lanesPerGroup) * lanesPerGroup; "
    "uint sourceLane = groupStart + lanesPerGroup - 1u - "
    "(lane % lanesPerGroup); uint sourceBit = sourceLane * elementWidth; "
    "uint destinationBit = lane * elementWidth; "
    "ulong value = (source[sourceBit >> 6u] >> (sourceBit & 63u)) & laneMask; "
    "result[destinationBit >> 6u] |= value << (destinationBit & 63u); } "
    "return result; }\n"
    "struct AddSubImmediateResult { ulong value; uint flags; };\n"
    "inline AddSubImmediateResult add_sub_immediate_apply(uint subtract, ulong lhs, "
    "uint immediate, uint shift, uint width, uint setFlags, uint previousFlags) { "
    "ulong left = width == 32u ? ulong(uint(lhs)) : lhs; "
    "ulong right = ulong(immediate) << shift; "
    "ulong value = "
    ARM64METAL_STRINGIFY(ARM64METAL_ADD_SUB_VALUE(subtract, left, right, width))
    "; ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul; "
    "uint flags = setFlags != 0u ? "
    ARM64METAL_STRINGIFY(ARM64METAL_ADD_SUB_IMM_NZCV(
        subtract, left, right, value, width, signBit))
    " : previousFlags; return AddSubImmediateResult{value, flags}; }\n"
    "inline ulong add_sub_extend_register(ulong source, uint option) { "
    "if (option == 0u) return source & 0xfful; "
    "if (option == 1u) return source & 0xfffful; "
    "if (option == 2u) return source & 0xfffffffful; "
    "if (option == 4u) { ulong value = source & 0xfful; "
    "return (value & 0x80ul) != 0ul ? value | ~0xfful : value; } "
    "if (option == 5u) { ulong value = source & 0xfffful; "
    "return (value & 0x8000ul) != 0ul ? value | ~0xfffful : value; } "
    "if (option == 6u) { ulong value = source & 0xfffffffful; "
    "return (value & 0x80000000ul) != 0ul ? value | ~0xfffffffful : value; } "
    "return source; }\n"
    "inline AddSubImmediateResult add_sub_extended_apply(uint subtract, ulong lhs, "
    "ulong rhs, uint option, uint amount, uint width, uint setFlags, "
    "uint previousFlags) { ulong left = width == 32u ? ulong(uint(lhs)) : lhs; "
    "ulong source = width == 32u ? ulong(uint(rhs)) : rhs; "
    "ulong right = add_sub_extend_register(source, option) << amount; "
    "if (width == 32u) right = ulong(uint(right)); "
    "ulong value = "
    ARM64METAL_STRINGIFY(ARM64METAL_ADD_SUB_VALUE(subtract, left, right, width))
    "; ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul; "
    "uint flags = setFlags != 0u ? "
    ARM64METAL_STRINGIFY(ARM64METAL_ADD_SUB_IMM_NZCV(
        subtract, left, right, value, width, signBit))
    " : previousFlags; return AddSubImmediateResult{value, flags}; }\n"
    "inline AddSubImmediateResult add_sub_shifted_apply(uint subtract, ulong lhs, "
    "ulong rhs, uint shiftType, uint amount, uint width, uint setFlags, "
    "uint previousFlags) { ulong left = width == 32u ? ulong(uint(lhs)) : lhs; "
    "ulong source = width == 32u ? ulong(uint(rhs)) : rhs; "
    "ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul; "
    "ulong shifted = source; if (shiftType == 0u) shifted = source << amount; "
    "else if (shiftType == 1u) shifted = source >> amount; "
    "else if (shiftType == 2u && amount != 0u) { shifted = source >> amount; "
    "if ((source & signBit) != 0ul) shifted |= (~0ul << (width - amount)); } "
    "if (width == 32u) shifted = ulong(uint(shifted)); ulong value = "
    ARM64METAL_STRINGIFY(ARM64METAL_ADD_SUB_VALUE(subtract, left, shifted, width))
    "; uint flags = setFlags != 0u ? "
    ARM64METAL_STRINGIFY(ARM64METAL_ADD_SUB_IMM_NZCV(
        subtract, left, shifted, value, width, signBit))
    " : previousFlags; return AddSubImmediateResult{value, flags}; }\n"
    "inline AddSubImmediateResult add_sub_carry_apply(uint subtract, ulong lhs, "
    "ulong rhs, uint width, uint setFlags, uint previousFlags) { "
    "ulong left = width == 32u ? ulong(uint(lhs)) : lhs; "
    "ulong right = width == 32u ? ulong(uint(rhs)) : rhs; "
    "ulong carryIn = ulong((previousFlags >> 3u) & 1u); "
    "ulong borrowIn = 1ul - carryIn; ulong value = width == 32u "
    "? ulong(uint(subtract != 0u ? uint(left) - uint(right) - uint(borrowIn) "
    ": uint(left) + uint(right) + uint(carryIn))) "
    ": (subtract != 0u ? left - right - borrowIn : left + right + carryIn); "
    "ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul; "
    "bool carryOut = subtract != 0u ? (left > right || (carryIn != 0ul && left == right)) "
    ": (value < left || (carryIn != 0ul && value == left)); "
    "uint flags = setFlags != 0u ? uint(value == 0ul) | "
    "(uint((value & signBit) != 0ul) << 1u) | "
    "(uint(subtract != 0u ? (((left ^ right) & (left ^ value) & signBit) != 0ul) "
    ": ((~(left ^ right) & (left ^ value) & signBit) != 0ul)) << 2u) | "
    "(uint(carryOut) << 3u) : previousFlags; "
    "return AddSubImmediateResult{value, flags}; }\n"
    "struct LogicalShiftedResult { ulong value; uint flags; };\n"
    "inline LogicalShiftedResult logical_immediate_apply(uint opc, ulong lhs, "
    "ulong immediate, uint width, uint setFlags, uint previousFlags) { "
    "ulong mask = width == 32u ? 0xfffffffful : ~0ul; "
    "ulong left = lhs & mask; ulong right = immediate & mask; "
    "ulong value = left & right; if (opc == 1u) value = left | right; "
    "else if (opc == 2u) value = left ^ right; value &= mask; "
    "uint flags = previousFlags; if (setFlags != 0u) { "
    "ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul; "
    "flags = uint(value == 0ul) | (uint((value & signBit) != 0ul) << 1u); } "
    "return LogicalShiftedResult{value, flags}; }\n"
    "inline LogicalShiftedResult logical_shifted_apply(uint opc, uint invert, "
    "ulong lhs, ulong rhs, uint shiftType, uint amount, uint width, "
    "uint setFlags, uint previousFlags) { ulong mask = width == 32u ? "
    "0xfffffffful : ~0ul; ulong source = rhs & mask; ulong shifted = source; "
    "if (shiftType == 0u) shifted = (source << amount) & mask; "
    "else if (shiftType == 1u) shifted = source >> amount; "
    "else if (shiftType == 2u && amount != 0u) { ulong signBit = width == 32u "
    "? 0x80000000ul : 0x8000000000000000ul; shifted = source >> amount; "
    "if ((source & signBit) != 0ul) shifted |= ~0ul << (width - amount); "
    "shifted &= mask; } else if (shiftType == 3u && amount != 0u) "
    "shifted = ((source >> amount) | (source << (width - amount))) & mask; "
    "if (invert != 0u) shifted = (~shifted) & mask; ulong left = lhs & mask; "
    "ulong value = left & shifted; if (opc == 1u) value = left | shifted; "
    "else if (opc == 2u) value = left ^ shifted; value &= mask; "
    "uint flags = previousFlags; if (setFlags != 0u) { ulong signBit = "
    "width == 32u ? 0x80000000ul : 0x8000000000000000ul; "
    "flags = uint(value == 0ul) | (uint((value & signBit) != 0ul) << 1u); } "
    "return LogicalShiftedResult{value, flags}; }\n"
    "inline LogicalShiftedResult logical_variable_shift_apply(uint shiftType, "
    "ulong value, ulong amountValue, uint width, uint previousFlags) { "
    "uint amount = uint(amountValue) & (width - 1u); "
    "return logical_shifted_apply(1u, 0u, 0ul, value, shiftType, amount, "
    "width, 0u, previousFlags); }\n"
    "inline ulong multiply_accumulate_apply(uint subtract, ulong lhs, "
    "ulong rhs, ulong accumulator, uint width) { ulong mask = width == 32u "
    "? 0xfffffffful : ~0ul; ulong product = (lhs & mask) * (rhs & mask); "
    "ulong base = accumulator & mask; return (subtract != 0u ? "
    "base - product : base + product) & mask; }\n"
    "inline ulong bitfield_apply(uint variant, ulong destination, ulong source, "
    "uint rotation, uint end, uint width) { ulong valueMask = width == 32u "
    "? 0xfffffffful : ~0ul; uint count = end >= rotation ? end - rotation + 1u "
    ": end + 1u; uint position = end >= rotation ? 0u : width - rotation; "
    "ulong lowMask = count == 64u ? ~0ul : ((1ul << count) - 1ul); "
    "ulong extracted = end >= rotation ? ((source >> rotation) & lowMask) "
    ": ((source & lowMask) << position); ulong fieldMask = (lowMask << position) "
    "& valueMask; if (variant == 2u && ((extracted >> (position + count - 1u)) "
    "& 1ul) != 0ul) { uint fieldEnd = position + count; ulong extensionMask "
    "= fieldEnd == 64u ? 0ul : valueMask & ~((1ul << fieldEnd) - 1ul); "
    "extracted |= extensionMask; } if (variant == 1u) "
    "return ((destination & ~fieldMask) | extracted) & valueMask; "
    "return extracted & valueMask; }\n"
    "struct ScalarFPResult { ulong value; uint fpsrFlags; };\n"
    "inline ScalarFPResult scalar_fp_integer_to_float(ulong raw, "
    "uint sourceWidth, uint signedInput, uint floatWidth, uint fpcr) { "
    "ulong sourceMask = sourceWidth == 32u ? 0xfffffffful : ~0ul; "
    "raw &= sourceMask; bool negative = signedInput != 0u && "
    "(raw & (sourceWidth == 32u ? 0x80000000ul : "
    "0x8000000000000000ul)) != 0ul; "
    "ulong magnitude = negative ? (sourceWidth == 32u ? "
    "ulong(0u - uint(raw)) : 0ul - raw) : raw; "
    "if (magnitude == 0ul) return ScalarFPResult{0ul, 0u}; "
    "uint fraction = floatWidth == 64u ? 52u : 23u; "
    "uint exponent = 63u - metal::clz(magnitude); ulong significand; uint flags = 0u; "
    "if (exponent <= fraction) significand = magnitude << (fraction - exponent); "
    "else { uint shift = exponent - fraction; significand = magnitude >> shift; "
    "ulong remainder = magnitude & ((1ul << shift) - 1ul); "
    "if (remainder != 0ul) { flags |= 16u; ulong halfway = 1ul << (shift - 1u); "
    "uint mode = (fpcr >> 22u) & 3u; bool up = (mode == 1u && !negative) || "
    "(mode == 2u && negative) || (mode == 0u && (remainder > halfway || "
    "(remainder == halfway && (significand & 1ul) != 0ul))); "
    "significand += ulong(up); } } uint bias = floatWidth == 64u ? 1023u : 127u; "
    "uint signPosition = floatWidth == 64u ? 63u : 31u; "
    "ulong value = (ulong(negative) << signPosition) | "
    "((ulong(exponent + bias - 1u) << fraction) + significand); "
    "return ScalarFPResult{value, flags}; }\n"
    "inline ulong scalar_fp_expand_immediate(uint immediate, uint floatWidth) { "
    "uint fraction = floatWidth == 64u ? 52u : 23u; "
    "uint exponentBits = floatWidth == 64u ? 11u : 8u; "
    "uint repeated = (immediate >> 6u) & 1u; "
    "ulong exponent = (ulong(repeated ^ 1u) << (exponentBits - 1u)) | "
    "(repeated != 0u ? ((1ul << (exponentBits - 1u)) - 4ul) : 0ul) | "
    "ulong((immediate >> 4u) & 3u); "
    "return (ulong((immediate >> 7u) & 1u) << (fraction + exponentBits)) | "
    "(exponent << fraction) | (ulong(immediate & 15u) << (fraction - 4u)); }\n"
    "inline ScalarFPResult scalar_fp_round_positive(ulong value, "
    "uint floatWidth, uint fpcr) { uint fraction = floatWidth == 64u ? 52u : 23u; "
    "uint exponentBits = floatWidth == 64u ? 11u : 8u; "
    "uint signPosition = fraction + exponentBits; ulong signMask = 1ul << signPosition; "
    "ulong fractionMask = (1ul << fraction) - 1ul; "
    "ulong exponentLimit = (1ul << exponentBits) - 1ul; "
    "ulong valueMask = floatWidth == 64u ? ~0ul : 0xfffffffful; value &= valueMask; "
    "ulong sign = value & signMask; ulong exponent = (value >> fraction) & exponentLimit; "
    "ulong significand = value & fractionMask; "
    "if (exponent == exponentLimit) { if (significand == 0ul) "
    "return ScalarFPResult{value, 0u}; ulong quietBit = 1ul << (fraction - 1u); "
    "uint flags = (significand & quietBit) == 0ul ? 1u : 0u; "
    "if ((fpcr & (1u << 25u)) != 0u) return ScalarFPResult{"
    "(exponentLimit << fraction) | quietBit, flags}; "
    "return ScalarFPResult{(value | quietBit) & valueMask, flags}; } "
    "if (exponent == 0ul && significand != 0ul && (fpcr & (1u << 24u)) != 0u) "
    "return ScalarFPResult{sign, 128u}; "
    "if (exponent == 0ul && significand == 0ul) return ScalarFPResult{sign, 0u}; "
    "int unbiased = exponent == 0ul ? (floatWidth == 64u ? -1022 : -126) : "
    "int(exponent) - (floatWidth == 64u ? 1023 : 127); "
    "if (unbiased >= int(fraction)) return ScalarFPResult{value, 0u}; "
    "if (unbiased < 0) { if (sign != 0ul) return ScalarFPResult{sign, 0u}; "
    "return ScalarFPResult{floatWidth == 64u ? 0x3ff0000000000000ul : "
    "0x3f800000ul, 0u}; } uint fractionalBits = fraction - uint(unbiased); "
    "ulong discardedMask = (1ul << fractionalBits) - 1ul; "
    "ulong discarded = value & discardedMask; "
    "if (discarded == 0ul) return ScalarFPResult{value, 0u}; "
    "ulong rounded = value & ~discardedMask; "
    "if (sign == 0ul) rounded += 1ul << fractionalBits; "
    "return ScalarFPResult{rounded & valueMask, 0u}; }\n"
    "inline ulong integer_bit_operation_apply(uint variant, ulong source, "
    "uint width) { ulong valueMask = width == 32u ? 0xfffffffful : ~0ul; "
    "ulong value = source & valueMask; if (variant == 0u) { ulong result = 0ul; "
    "for (uint bit = 0u; bit < width; ++bit) result = (result << 1u) | "
    "((value >> bit) & 1ul); return result & valueMask; } "
    "if (variant == 1u) { ulong result = 0ul; for (uint byte = 0u; "
    "byte < width / 8u; ++byte) result = (result << 8u) | "
    "((value >> (byte * 8u)) & 0xfful); return result & valueMask; } "
    "if (variant == 2u) return value == 0ul ? width : "
    "(width == 32u ? metal::clz(uint(value)) : metal::clz(value)); "
    "ulong signBit = 1ul << (width - 1u); ulong normalized = "
    "(value & signBit) != 0ul ? value ^ valueMask : value; "
    "if (normalized == 0ul) return width - 1u; "
    "uint leadingZeros = width == 32u ? metal::clz(uint(normalized)) : metal::clz(normalized); "
    "return leadingZeros - 1u; }\n"
    "inline ulong integer_divide_apply(uint isSigned, ulong lhs, ulong rhs, uint width) { "
    "ulong valueMask = width == 32u ? 0xfffffffful : ~0ul; ulong left = lhs & valueMask; "
    "ulong right = rhs & valueMask; if (right == 0ul) return 0ul; "
    "if (isSigned == 0u) return left / right; ulong signBit = 1ul << (width - 1u); "
    "bool leftNegative = (left & signBit) != 0ul; bool rightNegative = (right & signBit) != 0ul; "
    "ulong leftMagnitude = leftNegative ? (0ul - left) & valueMask : left; "
    "ulong rightMagnitude = rightNegative ? (0ul - right) & valueMask : right; "
    "ulong quotient = leftMagnitude / rightMagnitude; "
    "return ((leftNegative != rightNegative) ? 0ul - quotient : quotient) & valueMask; }\n"
    "inline ScalarFPResult scalar_fp_negate(ulong value, uint floatWidth) { "
    "ulong signMask = floatWidth == 64u ? 0x8000000000000000ul : 0x80000000ul; "
    "ulong valueMask = floatWidth == 64u ? ~0ul : 0xfffffffful; "
    "return ScalarFPResult{(value ^ signMask) & valueMask, 0u}; }\n"
    "inline ScalarFPResult scalar_fp_compare(ulong left, ulong right, "
    "uint floatWidth, uint signalAllNans, uint fpcr) { "
    "uint fraction = floatWidth == 64u ? 52u : 23u; "
    "uint exponentBits = floatWidth == 64u ? 11u : 8u; "
    "uint signPosition = fraction + exponentBits; ulong signMask = 1ul << signPosition; "
    "ulong fractionMask = (1ul << fraction) - 1ul; "
    "ulong exponentLimit = (1ul << exponentBits) - 1ul; "
    "ulong valueMask = floatWidth == 64u ? ~0ul : 0xfffffffful; "
    "left &= valueMask; right &= valueMask; uint flags = 0u; "
    "bool leftSubnormal = ((left >> fraction) & exponentLimit) == 0ul && "
    "(left & fractionMask) != 0ul; bool rightSubnormal = "
    "((right >> fraction) & exponentLimit) == 0ul && (right & fractionMask) != 0ul; "
    "if ((fpcr & (1u << 24u)) != 0u) { if (leftSubnormal) { left &= signMask; flags |= 128u; } "
    "if (rightSubnormal) { right &= signMask; flags |= 128u; } } "
    "ulong leftExponent = (left >> fraction) & exponentLimit; "
    "ulong rightExponent = (right >> fraction) & exponentLimit; "
    "ulong leftFraction = left & fractionMask; ulong rightFraction = right & fractionMask; "
    "bool leftNan = leftExponent == exponentLimit && leftFraction != 0ul; "
    "bool rightNan = rightExponent == exponentLimit && rightFraction != 0ul; "
    "if (leftNan || rightNan) { ulong quietBit = 1ul << (fraction - 1u); "
    "bool signaling = (leftNan && (leftFraction & quietBit) == 0ul) || "
    "(rightNan && (rightFraction & quietBit) == 0ul); "
    "if (signaling || signalAllNans != 0u) flags |= 1u; "
    "return ScalarFPResult{12u, flags}; } "
    "ulong leftMagnitude = left & ~signMask; ulong rightMagnitude = right & ~signMask; "
    "if (leftMagnitude == 0ul && rightMagnitude == 0ul) return ScalarFPResult{9u, flags}; "
    "bool leftNegative = (left & signMask) != 0ul; "
    "bool rightNegative = (right & signMask) != 0ul; "
    "bool less = leftNegative != rightNegative ? leftNegative : "
    "(leftNegative ? leftMagnitude > rightMagnitude : leftMagnitude < rightMagnitude); "
    "bool equal = left == right; return ScalarFPResult{equal ? 9u : (less ? 2u : 8u), flags}; }\n"
    "inline bool arm64_condition_holds(uint condition, uint flags) { return "
    ARM64METAL_STRINGIFY(ARM64METAL_CONDITION_HOLDS(condition, flags))
    "; }\n"
    "inline ulong conditional_select_apply(uint variant, ulong lhs, ulong rhs, "
    "uint condition, uint flags, uint width) { ulong selected = lhs; "
    "if (!arm64_condition_holds(condition, flags)) { "
    "if (variant == 1u) selected = rhs + 1ul; "
    "else if (variant == 2u) selected = ~rhs; "
    "else if (variant == 3u) selected = 0ul - rhs; "
    "else selected = rhs; } "
    "return width == 32u ? ulong(uint(selected)) : selected; }\n";
}  // namespace arm64metal
#endif

#undef ARM64METAL_STRINGIFY
#undef ARM64METAL_STRINGIFY_INNER
#undef ARM64METAL_SYSTEM_FLAGS
#undef ARM64METAL_FLAG_MANIPULATION
#undef ARM64METAL_MOVE_WIDE_VALUE
#undef ARM64METAL_ADD_SUB_IMM_VALUE
#undef ARM64METAL_ADD_SUB_VALUE
#undef ARM64METAL_ADD_SUB_IMM_NZCV

#endif  // ARM64METAL_INTEGER_OPS_SHARED_H
