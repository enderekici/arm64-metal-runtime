#ifndef ARM64METAL_INTEGER_OPS_SHARED_H
#define ARM64METAL_INTEGER_OPS_SHARED_H

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

#define ARM64METAL_STRINGIFY_INNER(value) #value
#define ARM64METAL_STRINGIFY(value) ARM64METAL_STRINGIFY_INNER(value)

#ifdef __METAL_VERSION__
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
    const ulong value = ARM64METAL_ADD_SUB_IMM_VALUE(
        subtract, left, immediate, shift, width);
    const ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul;
    const uint flags = setFlags != 0u
        ? ARM64METAL_ADD_SUB_IMM_NZCV(subtract, left, right, value, width, signBit)
        : previousFlags;
    return AddSubImmediateResult{value, flags};
}
#else
namespace arm64metal {
inline constexpr char typed_integer_msl_source[] =
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
    ARM64METAL_STRINGIFY(ARM64METAL_ADD_SUB_IMM_VALUE(
        subtract, left, immediate, shift, width))
    "; ulong signBit = width == 32u ? 0x80000000ul : 0x8000000000000000ul; "
    "uint flags = setFlags != 0u ? "
    ARM64METAL_STRINGIFY(ARM64METAL_ADD_SUB_IMM_NZCV(
        subtract, left, right, value, width, signBit))
    " : previousFlags; return AddSubImmediateResult{value, flags}; }\n";
}  // namespace arm64metal
#endif

#undef ARM64METAL_STRINGIFY
#undef ARM64METAL_STRINGIFY_INNER
#undef ARM64METAL_MOVE_WIDE_VALUE
#undef ARM64METAL_ADD_SUB_IMM_VALUE
#undef ARM64METAL_ADD_SUB_IMM_NZCV

#endif  // ARM64METAL_INTEGER_OPS_SHARED_H
