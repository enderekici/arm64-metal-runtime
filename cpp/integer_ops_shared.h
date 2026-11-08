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

#define ARM64METAL_STRINGIFY_INNER(value) #value
#define ARM64METAL_STRINGIFY(value) ARM64METAL_STRINGIFY_INNER(value)

#ifdef __METAL_VERSION__
inline ulong move_wide_apply(uint opcode, ulong previous, uint immediate,
                             uint shift, uint width) {
    const ulong value = ARM64METAL_MOVE_WIDE_VALUE(
        opcode, previous, immediate, shift);
    return width == 32u ? ulong(uint(value)) : value;
}
inline ulong add_sub_immediate_apply(uint subtract, ulong lhs,
                                     uint immediate, uint shift, uint width) {
    return ARM64METAL_ADD_SUB_IMM_VALUE(subtract, lhs, immediate, shift, width);
}
#else
namespace arm64metal {
inline constexpr char typed_integer_msl_source[] =
    "inline ulong move_wide_apply(uint opcode, ulong previous, uint immediate, "
    "uint shift, uint width) { ulong value = "
    ARM64METAL_STRINGIFY(ARM64METAL_MOVE_WIDE_VALUE(
        opcode, previous, immediate, shift))
    "; return width == 32u ? ulong(uint(value)) : value; }\n"
    "inline ulong add_sub_immediate_apply(uint subtract, ulong lhs, "
    "uint immediate, uint shift, uint width) { return "
    ARM64METAL_STRINGIFY(ARM64METAL_ADD_SUB_IMM_VALUE(
        subtract, lhs, immediate, shift, width))
    "; }\n";
}  // namespace arm64metal
#endif

#undef ARM64METAL_STRINGIFY
#undef ARM64METAL_STRINGIFY_INNER
#undef ARM64METAL_MOVE_WIDE_VALUE
#undef ARM64METAL_ADD_SUB_IMM_VALUE

#endif  // ARM64METAL_INTEGER_OPS_SHARED_H
