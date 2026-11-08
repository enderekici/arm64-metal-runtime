#ifndef ARM64METAL_MOVE_WIDE_SHARED_H
#define ARM64METAL_MOVE_WIDE_SHARED_H

#define ARM64METAL_MOVE_WIDE_VALUE(opcode, previous, immediate, shift)             \
  ((opcode) == 1u ? (ulong(immediate) << (shift))                                 \
   : (opcode) == 3u ? ~(ulong(immediate) << (shift))                              \
                    : (((previous) & ~(0xfffful << (shift))) |                    \
                       (ulong(immediate) << (shift))))

#define ARM64METAL_STRINGIFY_INNER(value) #value
#define ARM64METAL_STRINGIFY(value) ARM64METAL_STRINGIFY_INNER(value)

#ifdef __METAL_VERSION__
inline ulong move_wide_apply(uint opcode, ulong previous, uint immediate,
                             uint shift, uint width) {
    const ulong value = ARM64METAL_MOVE_WIDE_VALUE(
        opcode, previous, immediate, shift);
    return width == 32u ? ulong(uint(value)) : value;
}
#else
namespace arm64metal {
inline constexpr char move_wide_msl_source[] =
    "inline ulong move_wide_apply(uint opcode, ulong previous, uint immediate, "
    "uint shift, uint width) { ulong value = "
    ARM64METAL_STRINGIFY(ARM64METAL_MOVE_WIDE_VALUE(
        opcode, previous, immediate, shift))
    "; return width == 32u ? ulong(uint(value)) : value; }\n";
}  // namespace arm64metal
#endif

#undef ARM64METAL_STRINGIFY
#undef ARM64METAL_STRINGIFY_INNER
#undef ARM64METAL_MOVE_WIDE_VALUE

#endif  // ARM64METAL_MOVE_WIDE_SHARED_H
