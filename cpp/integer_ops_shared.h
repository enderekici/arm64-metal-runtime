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

#ifdef __METAL_VERSION__
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
#undef ARM64METAL_MOVE_WIDE_VALUE
#undef ARM64METAL_ADD_SUB_IMM_VALUE
#undef ARM64METAL_ADD_SUB_VALUE
#undef ARM64METAL_ADD_SUB_IMM_NZCV

#endif  // ARM64METAL_INTEGER_OPS_SHARED_H
