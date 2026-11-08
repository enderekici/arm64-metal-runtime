#include "translate_core.hpp"
#include "integer_ops_shared.h"
#include "process_decoder.hpp"

#include <cassert>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using arm64metal::translate_core::LoweringMode;
using arm64metal::translate_core::Operation;
using arm64metal::translate_core::Opcode;
using arm64metal::translate_core::AuthenticatedReturnStep;
using arm64metal::translate_core::decode_operation;
using arm64metal::translate_core::decode_authenticated_return_operation;
using arm64metal::translate_core::make_operation_instruction;
using arm64metal::translate_core::serialize_operation;

static std::uint64_t native_ubfm_wrap(std::uint64_t source) {
  std::uint64_t result;
  asm volatile("ubfm %0, %1, #60, #7" : "=r"(result) : "r"(source));
  return result;
}

static std::uint64_t native_integer_bit_operation(std::uint32_t variant,
                                                  std::uint32_t width,
                                                  std::uint64_t source) {
  std::uint64_t result;
  if (variant == 0U && width == 64U) {
    asm volatile("rbit %0, %1" : "=r"(result) : "r"(source));
  } else if (variant == 0U) {
    asm volatile("rbit %w0, %w1" : "=r"(result) : "r"(source));
  } else if (variant == 1U && width == 64U) {
    asm volatile("rev %0, %1" : "=r"(result) : "r"(source));
  } else if (variant == 1U) {
    asm volatile("rev %w0, %w1" : "=r"(result) : "r"(source));
  } else if (variant == 2U && width == 64U) {
    asm volatile("clz %0, %1" : "=r"(result) : "r"(source));
  } else if (variant == 2U) {
    asm volatile("clz %w0, %w1" : "=r"(result) : "r"(source));
  } else if (width == 64U) {
    asm volatile("cls %0, %1" : "=r"(result) : "r"(source));
  } else {
    asm volatile("cls %w0, %w1" : "=r"(result) : "r"(source));
  }
  return width == 32U ? std::uint32_t(result) : result;
}

static std::uint64_t native_integer_divide(bool isSigned, std::uint32_t width,
                                          std::uint64_t lhs,
                                          std::uint64_t rhs) {
  std::uint64_t result;
  if (isSigned && width == 64U) {
    asm volatile("sdiv %0, %1, %2" : "=r"(result) : "r"(lhs), "r"(rhs));
  } else if (isSigned) {
    asm volatile("sdiv %w0, %w1, %w2" : "=r"(result) : "r"(lhs), "r"(rhs));
  } else if (width == 64U) {
    asm volatile("udiv %0, %1, %2" : "=r"(result) : "r"(lhs), "r"(rhs));
  } else {
    asm volatile("udiv %w0, %w1, %w2" : "=r"(result) : "r"(lhs), "r"(rhs));
  }
  return width == 32U ? std::uint32_t(result) : result;
}

static std::uint64_t native_sbfm_wrap(std::uint64_t source) {
  std::uint64_t result;
  asm volatile("sbfm %0, %1, #60, #7" : "=r"(result) : "r"(source));
  return result;
}

static std::uint64_t native_bfm_wrap(std::uint64_t destination,
                                     std::uint64_t source) {
  asm volatile("bfm %0, %1, #60, #7" : "+r"(destination) : "r"(source));
  return destination;
}

static std::uint64_t native_ubfm_word(std::uint32_t source) {
  std::uint64_t result;
  asm volatile("ubfm %w0, %w1, #28, #7" : "=r"(result) : "r"(source));
  return result;
}

static std::pair<std::uint64_t, std::uint32_t> native_add_sub_carry(
    bool subtract, bool setFlags, std::uint64_t lhs, std::uint64_t rhs,
    std::uint32_t flags, std::uint32_t width) {
  const auto nativeFlags =
      (std::uint64_t(flags & 2U) << 30U) |
      (std::uint64_t(flags & 1U) << 30U) |
      (std::uint64_t(flags & 8U) << 26U) |
      (std::uint64_t(flags & 4U) << 26U);
  std::uint64_t result;
  std::uint64_t observedFlags;
  if (width == 64U && subtract && setFlags) {
    asm volatile("msr nzcv, %4\n\tsbcs %0, %2, %3\n\tmrs %1, nzcv"
                 : "=&r"(result), "=&r"(observedFlags)
                 : "r"(lhs), "r"(rhs), "r"(nativeFlags) : "cc");
  } else if (width == 64U && !subtract && setFlags) {
    asm volatile("msr nzcv, %4\n\tadcs %0, %2, %3\n\tmrs %1, nzcv"
                 : "=&r"(result), "=&r"(observedFlags)
                 : "r"(lhs), "r"(rhs), "r"(nativeFlags) : "cc");
  } else if (width == 32U && subtract && setFlags) {
    asm volatile("msr nzcv, %4\n\tsbcs %w0, %w2, %w3\n\tmrs %1, nzcv"
                 : "=&r"(result), "=&r"(observedFlags)
                 : "r"(lhs), "r"(rhs), "r"(nativeFlags) : "cc");
  } else if (width == 32U && !subtract && setFlags) {
    asm volatile("msr nzcv, %4\n\tadcs %w0, %w2, %w3\n\tmrs %1, nzcv"
                 : "=&r"(result), "=&r"(observedFlags)
                 : "r"(lhs), "r"(rhs), "r"(nativeFlags) : "cc");
  } else if (width == 64U && subtract) {
    asm volatile("msr nzcv, %4\n\tsbc %0, %2, %3\n\tmrs %1, nzcv"
                 : "=&r"(result), "=&r"(observedFlags)
                 : "r"(lhs), "r"(rhs), "r"(nativeFlags) : "cc");
  } else if (width == 64U) {
    asm volatile("msr nzcv, %4\n\tadc %0, %2, %3\n\tmrs %1, nzcv"
                 : "=&r"(result), "=&r"(observedFlags)
                 : "r"(lhs), "r"(rhs), "r"(nativeFlags) : "cc");
  } else if (subtract) {
    asm volatile("msr nzcv, %4\n\tsbc %w0, %w2, %w3\n\tmrs %1, nzcv"
                 : "=&r"(result), "=&r"(observedFlags)
                 : "r"(lhs), "r"(rhs), "r"(nativeFlags) : "cc");
  } else {
    asm volatile("msr nzcv, %4\n\tadc %w0, %w2, %w3\n\tmrs %1, nzcv"
                 : "=&r"(result), "=&r"(observedFlags)
                 : "r"(lhs), "r"(rhs), "r"(nativeFlags) : "cc");
  }
  const auto observed = static_cast<std::uint32_t>(observedFlags >> 28U);
  const auto internalFlags = ((observed >> 2U) & 2U) |
                             ((observed >> 2U) & 1U) |
                             ((observed << 2U) & 8U) |
                             ((observed << 2U) & 4U);
  return {width == 32U ? std::uint64_t(std::uint32_t(result)) : result,
          internalFlags};
}

static std::pair<std::uint64_t, std::uint32_t> native_alias_add_sub_carry(
    bool subtract, std::uint64_t value, std::uint32_t flags) {
  const auto nativeFlags =
      (std::uint64_t(flags & 2U) << 30U) |
      (std::uint64_t(flags & 1U) << 30U) |
      (std::uint64_t(flags & 8U) << 26U) |
      (std::uint64_t(flags & 4U) << 26U);
  std::uint64_t observedFlags;
  if (subtract) {
    asm volatile("msr nzcv, %2\n\tsbcs %0, %0, %0\n\tmrs %1, nzcv"
                 : "+&r"(value), "=&r"(observedFlags)
                 : "r"(nativeFlags) : "cc");
  } else {
    asm volatile("msr nzcv, %2\n\tadcs %0, %0, %0\n\tmrs %1, nzcv"
                 : "+&r"(value), "=&r"(observedFlags)
                 : "r"(nativeFlags) : "cc");
  }
  const auto observed = static_cast<std::uint32_t>(observedFlags >> 28U);
  const auto internalFlags = ((observed >> 2U) & 2U) |
                             ((observed >> 2U) & 1U) |
                             ((observed << 2U) & 8U) |
                             ((observed << 2U) & 4U);
  return {value, internalFlags};
}

struct NativeScalarFPCompare {
  std::uint32_t flags;
  std::uint32_t fpsr;
};

static NativeScalarFPCompare native_scalar_fp_compare(
    std::uint64_t left, std::uint64_t right, std::uint32_t width,
    bool signalAllNans, bool compareZero, std::uint32_t fpcr) {
  std::uint64_t savedFpcr;
  std::uint64_t observedNzcv;
  std::uint64_t observedFpsr;
  asm volatile("mrs %0, fpcr" : "=r"(savedFpcr));
  if (width == 64U) {
    if (signalAllNans && compareZero) {
      asm volatile("msr fpcr, %3\n\tmsr fpsr, xzr\n\tfmov d0, %4\n\tfcmpe d0, #0.0\n\tmrs %0, nzcv\n\tmrs %1, fpsr\n\tmsr fpcr, %2"
                   : "=&r"(observedNzcv), "=&r"(observedFpsr)
                   : "r"(savedFpcr), "r"(std::uint64_t{fpcr}),
                     "r"(left) : "v0", "cc", "memory");
    } else if (signalAllNans) {
      asm volatile("msr fpcr, %3\n\tmsr fpsr, xzr\n\tfmov d0, %4\n\tfmov d1, %5\n\tfcmpe d0, d1\n\tmrs %0, nzcv\n\tmrs %1, fpsr\n\tmsr fpcr, %2"
                   : "=&r"(observedNzcv), "=&r"(observedFpsr)
                   : "r"(savedFpcr), "r"(std::uint64_t{fpcr}),
                     "r"(left), "r"(right)
                   : "v0", "v1", "cc", "memory");
    } else if (compareZero) {
      asm volatile("msr fpcr, %3\n\tmsr fpsr, xzr\n\tfmov d0, %4\n\tfcmp d0, #0.0\n\tmrs %0, nzcv\n\tmrs %1, fpsr\n\tmsr fpcr, %2"
                   : "=&r"(observedNzcv), "=&r"(observedFpsr)
                   : "r"(savedFpcr), "r"(std::uint64_t{fpcr}),
                     "r"(left) : "v0", "cc", "memory");
    } else {
      asm volatile("msr fpcr, %3\n\tmsr fpsr, xzr\n\tfmov d0, %4\n\tfmov d1, %5\n\tfcmp d0, d1\n\tmrs %0, nzcv\n\tmrs %1, fpsr\n\tmsr fpcr, %2"
                   : "=&r"(observedNzcv), "=&r"(observedFpsr)
                   : "r"(savedFpcr), "r"(std::uint64_t{fpcr}),
                     "r"(left), "r"(right)
                   : "v0", "v1", "cc", "memory");
    }
  } else {
    if (signalAllNans && compareZero) {
      asm volatile("msr fpcr, %3\n\tmsr fpsr, xzr\n\tfmov s0, %w4\n\tfcmpe s0, #0.0\n\tmrs %0, nzcv\n\tmrs %1, fpsr\n\tmsr fpcr, %2"
                   : "=&r"(observedNzcv), "=&r"(observedFpsr)
                   : "r"(savedFpcr), "r"(std::uint64_t{fpcr}),
                     "r"(left) : "v0", "cc", "memory");
    } else if (signalAllNans) {
      asm volatile("msr fpcr, %3\n\tmsr fpsr, xzr\n\tfmov s0, %w4\n\tfmov s1, %w5\n\tfcmpe s0, s1\n\tmrs %0, nzcv\n\tmrs %1, fpsr\n\tmsr fpcr, %2"
                   : "=&r"(observedNzcv), "=&r"(observedFpsr)
                   : "r"(savedFpcr), "r"(std::uint64_t{fpcr}),
                     "r"(left), "r"(right)
                   : "v0", "v1", "cc", "memory");
    } else if (compareZero) {
      asm volatile("msr fpcr, %3\n\tmsr fpsr, xzr\n\tfmov s0, %w4\n\tfcmp s0, #0.0\n\tmrs %0, nzcv\n\tmrs %1, fpsr\n\tmsr fpcr, %2"
                   : "=&r"(observedNzcv), "=&r"(observedFpsr)
                   : "r"(savedFpcr), "r"(std::uint64_t{fpcr}),
                     "r"(left) : "v0", "cc", "memory");
    } else {
      asm volatile("msr fpcr, %3\n\tmsr fpsr, xzr\n\tfmov s0, %w4\n\tfmov s1, %w5\n\tfcmp s0, s1\n\tmrs %0, nzcv\n\tmrs %1, fpsr\n\tmsr fpcr, %2"
                   : "=&r"(observedNzcv), "=&r"(observedFpsr)
                   : "r"(savedFpcr), "r"(std::uint64_t{fpcr}),
                     "r"(left), "r"(right)
                   : "v0", "v1", "cc", "memory");
    }
  }
  const auto architectural = static_cast<std::uint32_t>(observedNzcv >> 28U);
  const auto internal = ((architectural >> 2U) & 2U) |
                        ((architectural >> 2U) & 1U) |
                        ((architectural << 2U) & 8U) |
                        ((architectural << 2U) & 4U);
  return {internal, static_cast<std::uint32_t>(observedFpsr)};
}

static std::pair<std::uint64_t, std::uint32_t> native_scalar_fp_negate(
    std::uint64_t value, std::uint32_t width) {
  std::uint64_t savedFpsr;
  std::uint64_t result;
  std::uint64_t observedFpsr;
  asm volatile("mrs %0, fpsr" : "=r"(savedFpsr));
  if (width == 64U) {
    asm volatile("msr fpsr, xzr\n\tfmov d0, %3\n\tfneg d0, d0\n\tfmov %0, d0\n\tmrs %1, fpsr\n\tmsr fpsr, %2"
                 : "=&r"(result), "=&r"(observedFpsr)
                 : "r"(savedFpsr), "r"(value) : "v0", "memory");
  } else {
    asm volatile("msr fpsr, xzr\n\tfmov s0, %w3\n\tfneg s0, s0\n\tfmov %w0, s0\n\tmrs %1, fpsr\n\tmsr fpsr, %2"
                 : "=&r"(result), "=&r"(observedFpsr)
                 : "r"(savedFpsr), "r"(value) : "v0", "memory");
  }
  return {width == 32U ? std::uint64_t{static_cast<std::uint32_t>(result)}
                       : result,
          static_cast<std::uint32_t>(observedFpsr)};
}

int main() {
  for (const std::uint32_t width : {32U, 64U}) {
    for (std::uint32_t variant = 0; variant < 3; ++variant) {
      for (const auto [rotation, end] :
           {std::pair{0U, 0U}, std::pair{0U, width - 1U},
            std::pair{width - 1U, 0U}, std::pair{4U, 11U},
            std::pair{width - 4U, 7U}}) {
        const auto base = variant == 0 ? 0x53000000U
            : variant == 1 ? 0x33000000U : 0x13000000U;
        const auto bits = base | (width == 64 ? 0x80400000U : 0U) |
                          (rotation << 16U) | (end << 10U) | (3U << 5U) | 2U;
        const auto operation = decode_operation(bits);
        const auto packed = (variant << 6U) | end;
        assert(operation.has_value() && operation->opcode == Opcode::Bitfield &&
               operation->destination == 2 && operation->source == 3 &&
               operation->immediate == rotation && operation->shift == packed &&
               operation->width == width);
        assert((serialize_operation(*operation) ==
                arm64metal::translate_core::OperationPacket{
                    33, 2, 3, rotation, packed, width}));
        const auto lowered = make_operation_instruction(
            *operation, {0x1004}, LoweringMode::Process,
            std::uint64_t{1} << 48);
        assert(lowered.code.size() == 1 &&
               lowered.code.front().find("bitfield_apply(") !=
                   std::string::npos);
      }
    }
  }
  assert(!decode_operation(0x13401c20U).has_value());  // W N mismatch.
  assert(!decode_operation(0x53201c20U).has_value());  // W rotation out of range.
  assert(!decode_operation(0x53008020U).has_value());  // W end out of range.
  const std::uint64_t bitfieldSource = 0x8123456789abcdefULL;
  const auto nativeUbfm = native_ubfm_wrap(bitfieldSource);
  const auto nativeSbfm = native_sbfm_wrap(bitfieldSource);
  const auto nativeBfm = native_bfm_wrap(0x123, bitfieldSource);
  assert(nativeUbfm == arm64metal::bitfield_apply(
                           0, 0, bitfieldSource, 60, 7, 64));
  assert(nativeSbfm == arm64metal::bitfield_apply(
                           2, 0, bitfieldSource, 60, 7, 64));
  assert(nativeBfm == arm64metal::bitfield_apply(
                          1, 0x123, bitfieldSource, 60, 7, 64));
  assert(nativeSbfm == 0xfffffffffffffef0ULL);
  assert(native_ubfm_word(0x89abcdefU) ==
         arm64metal::bitfield_apply(0, 0, 0x89abcdefU, 28, 7, 32));
  assert(native_ubfm_word(0x89abcdefU) == 0xef0U);

  for (const std::uint32_t width : {32U, 64U}) {
    for (std::uint32_t subtract = 0; subtract < 2; ++subtract) {
      for (std::uint32_t setFlags = 0; setFlags < 2; ++setFlags) {
        const auto bits = 0x1a000000U | (width == 64U ? 0x80000000U : 0U) |
                          (subtract << 30U) | (setFlags << 29U) |
                          (2U << 16U) | (1U << 5U) | 3U;
        const auto operation = decode_operation(bits);
        const auto packed = (setFlags << 1U) | subtract;
        assert(operation.has_value() &&
               operation->opcode == Opcode::AddSubCarryRegister &&
               operation->destination == 3 && operation->source == 1 &&
               operation->immediate == 2 && operation->shift == packed &&
               operation->width == width);
        assert((serialize_operation(*operation) ==
                arm64metal::translate_core::OperationPacket{
                    26, 3, 1, 2, packed, width}));
        const auto lowered = make_operation_instruction(
            *operation, {0x1004}, LoweringMode::Process,
            std::uint64_t{1} << 48);
        assert(lowered.code[1].find("add_sub_carry_apply(") !=
                   std::string::npos &&
               lowered.code[7] == "x[32] = add_sub_result.flags;");
      }
    }
  }
  const auto fixtureSbcs = decode_operation(0xfa090129U);
  assert(fixtureSbcs.has_value() &&
         fixtureSbcs->opcode == Opcode::AddSubCarryRegister &&
         fixtureSbcs->destination == 9 && fixtureSbcs->source == 9 &&
         fixtureSbcs->immediate == 9 && fixtureSbcs->shift == 3 &&
         fixtureSbcs->width == 64);
  const std::array<std::uint64_t, 6> carryValues{
      0, 1, 0xffffffffU, 0x7fffffffU, 0x8000000000000000ULL,
      0xffffffffffffffffULL};
  for (std::uint32_t subtract = 0; subtract < 2; ++subtract) {
    for (std::uint32_t flags = 0; flags < 16; ++flags) {
      for (const auto value : carryValues) {
        const auto expected = arm64metal::add_sub_carry_apply(
            subtract, value, value, 64, 1, flags);
        const auto actual = native_alias_add_sub_carry(
            subtract != 0, value, flags);
        assert(actual.first == expected.value && actual.second == expected.flags);
      }
    }
  }
  for (const std::uint32_t width : {32U, 64U}) {
    for (std::uint32_t subtract = 0; subtract < 2; ++subtract) {
      for (std::uint32_t setFlags = 0; setFlags < 2; ++setFlags) {
        for (std::uint32_t flags = 0; flags < 16; ++flags) {
          for (const auto lhs : carryValues) {
            for (const auto rhs : carryValues) {
              const auto expected = arm64metal::add_sub_carry_apply(
                  subtract, lhs, rhs, width, setFlags, flags);
              const auto actual = native_add_sub_carry(
                  subtract != 0, setFlags != 0, lhs, rhs, flags, width);
              assert(actual.first == expected.value &&
                     actual.second == expected.flags);
            }
          }
        }
      }
    }
  }
  for (const std::uint32_t width : {32U, 64U}) {
    for (std::uint32_t subtract = 0; subtract < 2; ++subtract) {
      for (std::uint32_t setFlags = 0; setFlags < 2; ++setFlags) {
        for (std::uint32_t shiftType = 0; shiftType < 3; ++shiftType) {
          const auto amount = width == 32 ? 31U : 63U;
          const auto bits = 0x0b000000U | (width == 64 ? 0x80000000U : 0U) |
                            (subtract << 30U) | (setFlags << 29U) |
                            (shiftType << 22U) | (2U << 16U) |
                            (amount << 10U) | (1U << 5U) | 3U;
          const auto shifted = decode_operation(bits);
          const auto packed = (setFlags << 9U) | (subtract << 8U) |
                              (shiftType << 6U) | amount;
          assert(shifted.has_value() &&
                 shifted->opcode == Opcode::AddSubShiftedRegister &&
                 shifted->destination == 3 && shifted->source == 1 &&
                 shifted->immediate == 2 && shifted->shift == packed &&
                 shifted->width == width);
          assert((serialize_operation(*shifted) ==
                  arm64metal::translate_core::OperationPacket{
                      19, 3, 1, 2, packed, width}));
          const auto lowered = make_operation_instruction(
              *shifted, {0x1004}, LoweringMode::Process,
              std::uint64_t{1} << 48);
          assert(lowered.code.size() == 9 &&
                 lowered.code[1].find("add_sub_shifted_apply(") !=
                     std::string::npos &&
                 lowered.code[7] == "x[32] = add_sub_result.flags;");
        }
      }
    }
  }
  const auto shiftedCmp = decode_operation(0xeb02003fU);
  assert(shiftedCmp.has_value() && shiftedCmp->destination == 31 &&
         shiftedCmp->shift == 0x300U);
  assert(make_operation_instruction(*shiftedCmp, {0x1004}, LoweringMode::Process,
                                    std::uint64_t{1} << 48)
             .code[1]
             .find("x[31]") == std::string::npos);
  assert(!decode_operation(0x8bc20c20U).has_value());  // ROR is reserved.
  assert(!decode_operation(0x0b028020U).has_value());  // W shift amount 32.
  const auto shiftedDiscard = decode_operation(0x8b02003fU);
  assert(shiftedDiscard.has_value() && shiftedDiscard->destination == 31);
  assert(make_operation_instruction(*shiftedDiscard, {0x1004},
                                    LoweringMode::Process,
                                    std::uint64_t{1} << 48)
             .code.size() == 8);
  assert(std::string_view(arm64metal::typed_integer_msl_source).find(
             "add_sub_shifted_apply") != std::string_view::npos);

  for (const std::uint32_t width : {32U, 64U}) {
    for (std::uint32_t subtract = 0; subtract < 2; ++subtract) {
      for (std::uint32_t setFlags = 0; setFlags < 2; ++setFlags) {
        for (std::uint32_t option = 0; option < 8; ++option) {
          for (std::uint32_t amount = 0; amount <= 4; ++amount) {
            const auto bits = 0x0b200000U | (width == 64 ? 0x80000000U : 0U) |
                              (subtract << 30U) | (setFlags << 29U) |
                              (option << 13U) | (2U << 16U) | (amount << 10U) |
                              (1U << 5U) | 3U;
            const auto extended = decode_operation(bits);
            const auto packed = (setFlags << 7U) | (subtract << 6U) |
                                (option << 3U) | amount;
            assert(extended.has_value() &&
                   extended->opcode == Opcode::AddSubExtendedRegister &&
                   extended->destination == 3 && extended->source == 1 &&
                   extended->immediate == 2 && extended->shift == packed &&
                   extended->width == width);
            assert((serialize_operation(*extended) ==
                    arm64metal::translate_core::OperationPacket{
                        22, 3, 1, 2, packed, width}));
            const auto lowered = make_operation_instruction(
                *extended, {0x1004}, LoweringMode::Process,
                std::uint64_t{1} << 48);
            assert(lowered.code.size() == 9 &&
                   lowered.code[1].find("add_sub_extended_apply(") !=
                       std::string::npos &&
                   lowered.code[7] == "x[32] = add_sub_result.flags;");
          }
        }
      }
    }
  }
  const auto extendedCmp = decode_operation(0xeb22003fU);
  assert(extendedCmp.has_value() && extendedCmp->destination == 31 &&
         extendedCmp->shift == 0xc0U);
  const auto extendedCmpLowered = make_operation_instruction(
      *extendedCmp, {0x1004}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(extendedCmpLowered.code.size() == 8);
  const auto extendedSp = decode_operation(0x0b22003fU);
  assert(extendedSp.has_value() && extendedSp->destination == 31);
  assert(make_operation_instruction(*extendedSp, {0x1004},
                                    LoweringMode::Process,
                                    std::uint64_t{1} << 48)
             .code[2] == "x[31] = add_sub_result.value;");
  const auto extendedSpSource = decode_operation(0x0b3f03e0U);
  assert(extendedSpSource.has_value() && extendedSpSource->source == 31 &&
         extendedSpSource->immediate == 31);
  assert(make_operation_instruction(*extendedSpSource, {0x1004},
                                    LoweringMode::Process,
                                    std::uint64_t{1} << 48)
             .code[1]
             .find(", x[31], 0u,") != std::string::npos);
  assert(!decode_operation(0x0b22143fU).has_value());  // Shift amount exceeds 4.
  assert(std::string_view(arm64metal::typed_integer_msl_source).find(
             "add_sub_extended_apply") != std::string_view::npos);

  for (const std::uint32_t width : {32U, 64U}) {
    for (std::uint32_t opc = 0; opc < 4; ++opc) {
      for (std::uint32_t invert = 0; invert < 2; ++invert) {
        for (std::uint32_t shiftType = 0; shiftType < 4; ++shiftType) {
          for (const std::uint32_t amount : {0U, 1U, width - 1U}) {
            const auto bits = 0x0a000000U | (width == 64 ? 0x80000000U : 0U) |
                              (opc << 29U) | (shiftType << 22U) |
                              (invert << 21U) | (2U << 16U) |
                              (amount << 10U) | (1U << 5U) | 3U;
            const auto logical = decode_operation(bits);
            const auto packed = (amount << 5U) | (shiftType << 3U) |
                                (invert << 2U) | opc;
            assert(logical.has_value() &&
                   logical->opcode == Opcode::LogicalShiftedRegister &&
                   logical->destination == 3 && logical->source == 1 &&
                   logical->immediate == 2 && logical->shift == packed &&
                   logical->width == width);
            assert((serialize_operation(*logical) ==
                    arm64metal::translate_core::OperationPacket{
                        23, 3, 1, 2, packed, width}));
            const auto lowered = make_operation_instruction(
                *logical, {0x1004}, LoweringMode::Process,
                std::uint64_t{1} << 48);
            assert(lowered.code.front() == "{");
            assert(lowered.code[1].find("logical_shifted_apply(") !=
                   std::string::npos);
            assert(lowered.code.back() == "}");
            assert(lowered.code.size() == (opc == 3U ? 9U : 4U));
          }
        }
      }
    }
  }
  constexpr std::uint32_t logicalImmediateW = 0x72000000U |
      (4U << 16U) | (39U << 10U) | (2U << 5U) | 31U;
  const auto logicalImmediate = decode_operation(logicalImmediateW);
  assert(logicalImmediate.has_value() &&
         logicalImmediate->opcode == Opcode::LogicalImmediate &&
         logicalImmediate->destination == 31 && logicalImmediate->source == 98 &&
         logicalImmediate->immediate == 0xf00ff00fU &&
         logicalImmediate->shift == 0 && logicalImmediate->width == 32);
  assert((serialize_operation(*logicalImmediate) ==
          arm64metal::translate_core::OperationPacket{
              24, 31, 98, 0xf00ff00fU, 0, 32}));
  const auto logicalImmediateLowered = make_operation_instruction(
      *logicalImmediate, {0x1004}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(logicalImmediateLowered.code[1].find("logical_immediate_apply(") !=
         std::string::npos);
  assert(std::string_view(arm64metal::typed_integer_msl_source).find(
             "logical_immediate_apply") != std::string_view::npos);
  assert(logicalImmediateLowered.code.size() == 8);
  const auto logicalImmediateDiscard = make_operation_instruction(
      Operation{Opcode::LogicalImmediate, 31, 0, 0xffU, 0, 64},
      {0x1004}, LoweringMode::Process, std::uint64_t{1} << 48);
  for (const auto& line : logicalImmediateDiscard.code) {
    assert(line.find("x[31] =") == std::string::npos);
  }
  const auto logicalImmediateX = decode_operation(
      0xd2000000U | (1U << 22U) | (1U << 16U) | (1U << 10U) |
      (31U << 5U) | 3U);
  assert(logicalImmediateX.has_value() && logicalImmediateX->source == 95 &&
         logicalImmediateX->immediate == 0x1U &&
         logicalImmediateX->shift == 0x80000000U &&
         logicalImmediateX->width == 64);
  const auto logicalMaskW =
      arm64metal::translate_core::decode_logical_immediate_mask(0, 4, 39, 32);
  const auto logicalMaskX =
      arm64metal::translate_core::decode_logical_immediate_mask(1, 1, 1, 64);
  const auto rotatedPattern =
      arm64metal::translate_core::decode_logical_immediate_mask(0, 8, 39, 64);
  assert(logicalMaskW.has_value() && *logicalMaskW == 0xf00ff00fU);
  assert(logicalMaskX.has_value() && *logicalMaskX == 0x8000000000000001ULL);
  assert(rotatedPattern.has_value() && *rotatedPattern == 0xff00ff00ff00ff00ULL);
  assert(!arm64metal::translate_core::decode_logical_immediate_mask(1, 0, 0, 32));
  assert(!arm64metal::translate_core::decode_logical_immediate_mask(1, 0, 63, 64));
  assert(!arm64metal::translate_core::decode_logical_immediate_mask(0, 0, 63, 64));
  assert(!decode_operation(0x72000000U | (1U << 22U) | (1U << 10U)));
  assert(!decode_operation(0x92000000U | (1U << 22U) | (63U << 10U)));
  Operation invalidLogicalImmediate{Opcode::LogicalImmediate, 0, 0, 1, 1, 32};
  bool invalidLogicalImmediateRejected = false;
  try {
    (void)make_operation_instruction(invalidLogicalImmediate, {0x1004},
                                     LoweringMode::Process,
                                     std::uint64_t{1} << 48);
  } catch (const std::invalid_argument&) {
    invalidLogicalImmediateRejected = true;
  }
  assert(invalidLogicalImmediateRejected);
  assert(!decode_operation(0x0a008020U).has_value());  // W shift amount 32 is invalid.
  const auto logicalCmp = decode_operation(0xea02003fU);
  assert(logicalCmp.has_value() && logicalCmp->destination == 31 &&
         logicalCmp->shift == 3U);
  assert(make_operation_instruction(*logicalCmp, {0x1004}, LoweringMode::Process,
                                    std::uint64_t{1} << 48)
             .code.size() == 8);
  assert(std::string_view(arm64metal::typed_integer_msl_source).find(
             "logical_shifted_apply") != std::string_view::npos);
  const auto logicalMsl =
      std::string_view(arm64metal::typed_integer_msl_source);
  assert(logicalMsl.find("};\ninline LogicalShiftedResult") !=
         std::string_view::npos);
  const auto carryMsl = logicalMsl.find("inline AddSubImmediateResult add_sub_carry_apply");
  assert(carryMsl != std::string_view::npos &&
         logicalMsl.find("}\nstruct LogicalShiftedResult", carryMsl) !=
             std::string_view::npos);
  assert(logicalMsl.find("width, 0u, previousFlags); }\n"
                         "inline ulong multiply_accumulate_apply") !=
         std::string_view::npos);
  assert(logicalMsl.find("inline ulong bitfield_apply(uint variant") !=
         std::string_view::npos);
  assert(logicalMsl.find("inline bool arm64_condition_holds") !=
         std::string_view::npos);
  assert(logicalMsl.find("return extracted & valueMask; }\n"
                         "struct ScalarFPResult") !=
         std::string_view::npos);
  assert(logicalMsl.find("};\\ninline LogicalShiftedResult") ==
         std::string_view::npos);

  for (const std::uint32_t width : {32U, 64U}) {
    for (std::uint32_t shiftType = 0; shiftType < 4; ++shiftType) {
      const auto bits = 0x1ac02000U | (width == 64 ? 0x80000000U : 0U) |
                        (shiftType << 10U) | (2U << 16U) |
                        (1U << 5U) | 3U;
      const auto variable = decode_operation(bits);
      assert(variable.has_value() &&
             variable->opcode == Opcode::LogicalVariableShiftRegister &&
             variable->destination == 3 && variable->source == 1 &&
             variable->immediate == 2 && variable->shift == shiftType &&
             variable->width == width);
      assert((serialize_operation(*variable) ==
              arm64metal::translate_core::OperationPacket{
                  29, 3, 1, 2, shiftType, width}));
      const auto lowered = make_operation_instruction(
          *variable, {0x1004}, LoweringMode::Process,
          std::uint64_t{1} << 48);
      assert(lowered.code.size() == 4 &&
             lowered.code[1].find("logical_variable_shift_apply(") !=
                 std::string::npos &&
             lowered.code[2] == "x[3] = logical_result.value;");
    }
  }
  assert(!decode_operation(0x1ac02000U | (5U << 10U)).has_value());
  assert(std::string_view(arm64metal::typed_integer_msl_source).find(
             "logical_variable_shift_apply") != std::string_view::npos);

  using arm64metal::logical_shifted_apply;
  using arm64metal::logical_variable_shift_apply;
  using arm64metal::logical_immediate_apply;
  assert(logical_immediate_apply(0, 0x1234, 0xff, 64, 0, 0xf).value == 0x34);
  assert(logical_immediate_apply(1, 0, 0x8000000000000001ULL, 64, 0, 0xf)
             .value == 0x8000000000000001ULL);
  assert(logical_immediate_apply(2, 0x8000000000000001ULL,
                                 0x8000000000000001ULL, 64, 0, 0xf)
             .value == 0);
  assert(logical_immediate_apply(3, 0x8000000000000000ULL,
                                0x8000000000000000ULL, 64, 1, 0xf)
             .flags == 2);
  assert(logical_immediate_apply(3, 0, 0xff, 32, 1, 0xf).flags == 1);
  assert(logical_immediate_apply(0, 0, 0xff00000000000000ULL, 32, 0, 0xf)
             .value == 0);
  assert(logical_immediate_apply(1, 0, 0x00ff00ffULL, 32, 0, 0xf).value ==
         0x00ff00ffU);
  assert(logical_immediate_apply(0, 0x12, 0xff, 64, 0, 0xf).flags == 0xf);
  assert(logical_shifted_apply(0, 0, 0xf0, 0x0f, 0, 0, 64, 0, 0xf)
             .value == 0);
  assert(logical_shifted_apply(0, 1, 0xf0, 0x0f, 0, 0, 64, 0, 0xf)
             .value == 0xf0);
  assert(logical_shifted_apply(1, 0, 0xf0, 0x0f, 0, 0, 64, 0, 0xf)
             .value == 0xff);
  assert(logical_shifted_apply(1, 1, 0xf0, 0x0f, 0, 0, 64, 0, 0xf)
             .value == 0xfffffffffffffff0ULL);
  assert(logical_shifted_apply(2, 0, 0xf0, 0x0f, 0, 0, 64, 0, 0xf)
             .value == 0xff);
  assert(logical_shifted_apply(2, 1, 0xf0, 0x0f, 0, 0, 64, 0, 0xf)
             .value == 0xffffffffffffff00ULL);
  assert(logical_shifted_apply(3, 0, 0x8000000000000000ULL,
                               0x8000000000000000ULL, 1, 0, 64, 1, 0xf)
             .flags == 2);
  assert(logical_shifted_apply(3, 1, 0, 0, 0, 0, 32, 1, 0xf).flags == 1);
  assert(logical_shifted_apply(0, 0, 0, 0, 0, 0, 32, 0, 0xf).flags == 0xf);
  assert(logical_shifted_apply(1, 0, 0, 0x80000005U, 0, 2, 32, 0, 0)
             .value == 0x14U);
  assert(logical_shifted_apply(1, 0, 0, 0x80000005U, 1, 2, 32, 0, 0)
             .value == 0x20000001U);
  assert(logical_shifted_apply(1, 0, 0, 0x80000005U, 2, 2, 32, 0, 0)
             .value == 0xe0000001U);
  assert(logical_shifted_apply(1, 0, 0, 0x80000005U, 3, 2, 32, 0, 0)
             .value == 0x60000001U);
  const auto variableValue = 0x8000000000000004ULL;
  assert(logical_variable_shift_apply(0, variableValue, 1, 64, 0xf).value ==
         0x8ULL);
  assert(logical_variable_shift_apply(1, variableValue, 1, 64, 0xf).value ==
         0x4000000000000002ULL);
  assert(logical_variable_shift_apply(2, variableValue, 1, 64, 0xf).value ==
         0xc000000000000002ULL);
  assert(logical_variable_shift_apply(3, variableValue, 1, 64, 0xf).value ==
         0x4000000000000002ULL);
  assert(logical_variable_shift_apply(0, 0x80000000U, 0x10000001U, 32, 0xf)
             .value == 0U);
  assert(logical_variable_shift_apply(1, 0x80000000U, 31, 32, 0xf).value == 1U);
  assert(logical_variable_shift_apply(2, 0x80000000U, 31, 32, 0xf).value ==
         0xffffffffU);
  assert(logical_variable_shift_apply(3, 0x80000000U, 31, 32, 0xf).value == 1U);
  assert(logical_variable_shift_apply(0, 0x8000000000000004ULL, 0x1001, 64, 0xf)
             .value == 0x8ULL);
  assert(logical_variable_shift_apply(2, 0x1234, 0, 64, 0xf).value == 0x1234);
  assert(logical_variable_shift_apply(1, 0, 0, 32, 0xf).flags == 0xf);

  using arm64metal::multiply_accumulate_apply;
  for (const std::uint32_t width : {32U, 64U}) {
    for (std::uint32_t subtract = 0; subtract < 2; ++subtract) {
      const auto bits = 0x1b000000U | (width == 64 ? 0x80000000U : 0U) |
                        (subtract << 15U) | (4U << 10U) | (1U << 5U) |
                        (2U << 16U) | 3U;
      const auto operation = decode_operation(bits);
      const auto packed = (subtract << 5U) | 4U;
      assert(operation.has_value() &&
             operation->opcode == Opcode::IntegerMultiplyAccumulate &&
             operation->destination == 3 && operation->source == 1 &&
             operation->immediate == 2 && operation->shift == packed &&
             operation->width == width);
      assert((serialize_operation(*operation) ==
              arm64metal::translate_core::OperationPacket{
                  31, 3, 1, 2, packed, width}));
      const auto lowered = make_operation_instruction(
          *operation, {0x1004}, LoweringMode::Process,
          std::uint64_t{1} << 48);
      assert(lowered.code.size() == 1 &&
             lowered.code[0].find("multiply_accumulate_apply(") !=
                 std::string::npos);
    }
  }
  const auto mulAlias = decode_operation(0x9b027c23U);
  const auto mnegAlias = decode_operation(0x9b02fc23U);
  assert(mulAlias.has_value() && mulAlias->opcode == Opcode::IntegerMultiplyAccumulate &&
         (mulAlias->shift & 31U) == 31U && (mulAlias->shift >> 5U) == 0U);
  assert(mnegAlias.has_value() && mnegAlias->opcode == Opcode::IntegerMultiplyAccumulate &&
         (mnegAlias->shift & 31U) == 31U && (mnegAlias->shift >> 5U) == 1U);
  const auto zeroDestination = decode_operation(0x9b020fffU);
  assert(zeroDestination.has_value() && zeroDestination->destination == 31);
  assert(make_operation_instruction(*zeroDestination, {0x1004},
                                    LoweringMode::Process,
                                    std::uint64_t{1} << 48)
             .code.empty());
  assert(multiply_accumulate_apply(0, 5, 7, 11, 64) == 46);
  assert(multiply_accumulate_apply(1, 5, 7, 11, 64) == 0xffffffffffffffe8ULL);
  assert(multiply_accumulate_apply(0, 0xffffffffffffffffULL, 2, 5, 64) == 3);
  assert(multiply_accumulate_apply(0, 0xffffffffU, 2, 3, 32) == 1U);
  assert(multiply_accumulate_apply(1, 0x80000000U, 2, 1, 32) == 1U);
  assert(multiply_accumulate_apply(0, 0, 0x1234, 0, 64) == 0);
  assert(multiply_accumulate_apply(1, 0, 0x1234, 0, 64) == 0);
  assert(std::string_view(arm64metal::typed_integer_msl_source).find(
             "multiply_accumulate_apply") != std::string_view::npos);

  using arm64metal::scalar_fp_expand_immediate;
  using arm64metal::scalar_fp_compare;
  using arm64metal::scalar_fp_integer_to_float;
  using arm64metal::scalar_fp_negate;
  using arm64metal::scalar_fp_round_positive;
  const auto ucvtf = decode_operation(0x9e630100U);  // ucvtf d0, x8
  assert(ucvtf.has_value() && ucvtf->opcode == Opcode::ScalarFloatingPoint &&
         ucvtf->destination == 0 && ucvtf->source == 8 && ucvtf->shift == 2 &&
         ucvtf->width == 64);
  assert((serialize_operation(*ucvtf) ==
          arm64metal::translate_core::OperationPacket{32, 0, 8, 0, 2, 64}));
  const auto scvtf = decode_operation(0x1e2200a3U);  // scvtf s3, w5
  assert(scvtf.has_value() && scvtf->opcode == Opcode::ScalarFloatingPoint &&
         scvtf->destination == 3 && scvtf->source == 5 && scvtf->shift == 1 &&
         scvtf->width == 32);
  const auto fmovImmediate = decode_operation(0x1e6d1001U);  // fmov d1, #0.75
  assert(fmovImmediate.has_value() &&
         fmovImmediate->opcode == Opcode::ScalarFloatingPoint &&
         fmovImmediate->destination == 1 && fmovImmediate->immediate == 0x68 &&
         fmovImmediate->shift == 4 && fmovImmediate->width == 64);
  const auto frintp = decode_operation(0x1e64c000U);  // frintp d0, d0
  assert(frintp.has_value() && frintp->opcode == Opcode::ScalarFloatingPoint &&
         frintp->destination == 0 && frintp->source == 0 &&
         frintp->shift == 8 && frintp->width == 64);
  const auto fnegD = decode_operation(0x1e6140a3U);  // fneg d3, d5
  const auto fnegS = decode_operation(0x1e2140c7U);  // fneg s7, s6
  assert(fnegD.has_value() && fnegD->opcode == Opcode::ScalarFloatingPoint &&
         fnegD->destination == 3 && fnegD->source == 5 && fnegD->shift == 12 &&
         fnegD->width == 64);
  assert(fnegS.has_value() && fnegS->destination == 7 && fnegS->source == 6 &&
         fnegS->width == 32);
  const auto fcmpD = decode_operation(0x1e622020U);  // fcmp d1, d2
  const auto fcmpeS = decode_operation(0x1e242070U);  // fcmpe s3, s4
  const auto fcmpZero = decode_operation(0x1e6020a8U);  // fcmp d5, #0.0
  const auto fcmpeZero = decode_operation(0x1e2020f8U);  // fcmpe s7, #0.0
  assert(fcmpD.has_value() && fcmpD->opcode == Opcode::ScalarFloatingPoint &&
         fcmpD->destination == 0 && fcmpD->source == 1 &&
         fcmpD->immediate == 2 && fcmpD->shift == 16 && fcmpD->width == 64);
  assert(fcmpeS.has_value() && fcmpeS->source == 3 && fcmpeS->immediate == 4 &&
         fcmpeS->shift == 17 && fcmpeS->width == 32);
  assert(fcmpZero.has_value() && fcmpZero->source == 5 &&
         fcmpZero->immediate == 0 && fcmpZero->shift == 18 &&
         fcmpZero->width == 64);
  assert(fcmpeZero.has_value() && fcmpeZero->source == 7 &&
         fcmpeZero->shift == 19 && fcmpeZero->width == 32);
  assert(make_operation_instruction(*ucvtf, {0x1000}, LoweringMode::Process,
                                    std::uint64_t{1} << 48)
             .code[0].find("scalar_fp_integer_to_float(") != std::string::npos);
  assert(make_operation_instruction(*fnegD, {0x1000}, LoweringMode::Process,
                                    std::uint64_t{1} << 48)
             .code[0].find("scalar_fp_negate(") != std::string::npos);
  const auto loweredFcmp = make_operation_instruction(
      *fcmpeS, {0x1000}, LoweringMode::Process, std::uint64_t{1} << 48);
  assert(loweredFcmp.code[0].find("scalar_fp_compare(") != std::string::npos &&
         loweredFcmp.code[0].find("z = (fp_result.value & 1u)") !=
             std::string::npos &&
         loweredFcmp.code[0].find("x[32] = (x[32] & ~0xful)") !=
             std::string::npos);
  const auto loweredFcmpFunction = make_operation_instruction(
      *fcmpeS, {0x1000}, LoweringMode::Function, std::uint64_t{1} << 48);
  assert(loweredFcmpFunction.code[0].find("c = (fp_result.value & 8u)") !=
             std::string::npos &&
         loweredFcmpFunction.code[0].find("x[32] =") == std::string::npos);
  assert(scalar_fp_integer_to_float(0x0020000000000001ULL, 64, 0, 64, 0).value ==
         0x4340000000000000ULL);
  assert(scalar_fp_integer_to_float(0x0020000000000001ULL, 64, 0, 64, 0)
             .fpsrFlags == 16U);
  assert(scalar_fp_integer_to_float(0x0020000000000001ULL, 64, 0, 64,
                                    1U << 22U).value ==
         0x4340000000000001ULL);
  assert(scalar_fp_integer_to_float(0xffdfffffffffffffULL, 64, 1, 64,
                                    2U << 22U).value ==
         0xc340000000000001ULL);
  assert(scalar_fp_integer_to_float(0xffffffffU, 32, 1, 32, 0).value ==
         0xbf800000U);
  assert(scalar_fp_integer_to_float(0xffffffffU, 32, 0, 32, 0).value ==
         0x4f800000U);
  assert(scalar_fp_integer_to_float(0x8000000000000000ULL, 64, 1, 64, 0).value ==
         0xc3e0000000000000ULL);
  assert(scalar_fp_expand_immediate(0x68, 64) == 0x3fe8000000000000ULL);
  assert(scalar_fp_expand_immediate(0xe0, 32) == 0xbf000000U);
  assert(scalar_fp_round_positive(0x3fe8000000000000ULL, 64, 0).value ==
         0x3ff0000000000000ULL);
  assert(scalar_fp_round_positive(0x3fe8000000000000ULL, 64, 0).fpsrFlags == 0U);
  assert(scalar_fp_round_positive(0xbfd0000000000000ULL, 64, 0).value ==
         0x8000000000000000ULL);
  assert(scalar_fp_round_positive(0x8000000000000000ULL, 64, 0).value ==
         0x8000000000000000ULL);
  assert(scalar_fp_round_positive(0x7ff8000000001234ULL, 64, 0).value ==
         0x7ff8000000001234ULL);
  assert(scalar_fp_round_positive(0x7ff0000000001234ULL, 64, 0).value ==
         0x7ff8000000001234ULL);
  assert(scalar_fp_round_positive(0x0000000000000001ULL, 64, 1U << 24U).value == 0);
  assert(scalar_fp_round_positive(0x0000000000000001ULL, 64, 1U << 24U)
             .fpsrFlags == 128U);
  const auto checkCompare = [](std::uint64_t left, std::uint64_t right,
                               std::uint32_t width, bool signalAllNans,
                               bool compareZero, std::uint32_t fpcr) {
    const auto native = native_scalar_fp_compare(
        left, right, width, signalAllNans, compareZero, fpcr);
    const auto typed = scalar_fp_compare(
        left, compareZero ? 0U : right, width,
        static_cast<std::uint32_t>(signalAllNans), fpcr);
    assert(typed.value == native.flags);
    assert(typed.fpsrFlags == native.fpsr);
  };
  checkCompare(0x3ff0000000000000ULL, 0x4000000000000000ULL,
               64U, false, false, 0U);
  checkCompare(0x4000000000000000ULL, 0x3ff0000000000000ULL,
               64U, false, false, 0U);
  checkCompare(0x8000000000000000ULL, 0U, 64U, false, false, 0U);
  checkCompare(0x7ff0000000001234ULL, 0x3ff0000000000000ULL,
               64U, false, false, 0U);
  checkCompare(0x7ff8000000001234ULL, 0x3ff0000000000000ULL,
               64U, false, false, 0U);
  checkCompare(0x7ff8000000001234ULL, 0x3ff0000000000000ULL,
               64U, true, false, 0U);
  checkCompare(0x7ff8000000001234ULL, 0U, 64U, true, true, 1U << 25U);
  checkCompare(1U, 0U, 64U, false, true, 0U);
  checkCompare(1U, 0U, 64U, false, true, 1U << 24U);
  checkCompare(0xbf800000U, 0x3f800000U, 32U, false, false, 0U);
  checkCompare(0x7f800123U, 0x3f800000U, 32U, true, false, 0U);
  checkCompare(0x7fc00123U, 0U, 32U, true, true, 0U);
  for (const std::uint64_t bits : {
           0x7ff0000000001234ULL, 0x7ff8000000001234ULL,
           0x8000000000000000ULL, 0x3f800000ULL}) {
    const auto native = native_scalar_fp_negate(bits, 64U);
    const auto typed = scalar_fp_negate(bits, 64U);
    assert(typed.value == native.first && typed.fpsrFlags == native.second);
  }
  for (const std::uint64_t bits : {0x7f800123U, 0x80000000U, 0x3f800000U}) {
    const auto native = native_scalar_fp_negate(bits, 32U);
    const auto typed = scalar_fp_negate(bits, 32U);
    assert(typed.value == native.first && typed.fpsrFlags == native.second);
  }
  assert(std::string_view(arm64metal::typed_integer_msl_source).find(
             "scalar_fp_round_positive") != std::string_view::npos);
  assert(std::string_view(arm64metal::typed_integer_msl_source).find(
             "scalar_fp_compare") != std::string_view::npos);
  assert(std::string_view(arm64metal::typed_integer_msl_source).find(
             "\\ninline ScalarFPResult scalar_fp_compare") ==
         std::string_view::npos);

  constexpr std::uint32_t selectBases[] = {
      0x1a800000U, 0x1a800400U, 0x5a800000U, 0x5a800400U};
  for (std::uint32_t variant = 0; variant < 4; ++variant) {
    for (const std::uint32_t width : {32U, 64U}) {
      for (std::uint32_t condition = 0; condition < 16; ++condition) {
        const auto bits = selectBases[variant] | (width == 64 ? 0x80000000U : 0U) |
                          (2U << 16U) | (1U << 5U) | (condition << 12U) | 3U;
        const auto operation = decode_operation(bits);
        assert(operation.has_value() && operation->opcode == Opcode::ConditionalSelect);
        assert(operation->destination == 3 && operation->source == 1 &&
               operation->immediate == 2 &&
               operation->shift == (variant << 4U) + condition &&
               operation->width == width);
        assert((serialize_operation(*operation) ==
                arm64metal::translate_core::OperationPacket{
                    16, 3, 1, 2, (variant << 4U) + condition, width}));
      }
    }
  }
  const auto cselWide = decode_operation(0x9a820020U);
  assert(cselWide.has_value());
  const auto cselWideLowered = make_operation_instruction(
      *cselWide, {0x1004}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(cselWideLowered.code == std::vector<std::string>{
      "x[0] = conditional_select_apply(0u, x[1], x[2], 0u, "
      "(uint(z) | (uint(n) << 1u) | (uint(v) << 2u) | (uint(c) << 3u)), 64u);"});
  const auto csincWord = decode_operation(0x1a851483U);
  assert(csincWord.has_value() && csincWord->shift == 17 &&
         csincWord->width == 32);
  const auto csincWordLowered = make_operation_instruction(
      *csincWord, {0x1004}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(csincWordLowered.code[0].find("conditional_select_apply(1u,") !=
         std::string::npos);
  const auto cselZero = decode_operation(0x9a9f03ffU);
  assert(cselZero.has_value() && cselZero->destination == 31 &&
         cselZero->source == 31 && cselZero->immediate == 31);
  assert(make_operation_instruction(*cselZero, {0x1004}, LoweringMode::Process,
                                    std::uint64_t{1} << 48).code.empty());
  for (std::uint32_t flags = 0; flags < 16; ++flags) {
    for (std::uint32_t condition = 0; condition < 16; ++condition) {
      const bool takeLeft = arm64metal::arm64_condition_holds(condition, flags);
      for (std::uint32_t variant = 0; variant < 4; ++variant) {
        const std::uint64_t lhs = 0x123456789abcdef0ULL;
        const std::uint64_t rhs = 0xfedcba9876543210ULL;
        const std::uint64_t fallback =
            variant == 1 ? rhs + 1U : variant == 2 ? ~rhs
                                         : variant == 3 ? 0U - rhs : rhs;
        assert(arm64metal::conditional_select_apply(
                   variant, lhs, rhs, condition, flags, 64) ==
               (takeLeft ? lhs : fallback));
      }
    }
  }
  assert(arm64metal::conditional_select_apply(1, 7, 0xffffffffU, 1, 1, 32) == 0);

  const auto branchEqual = decode_operation(0x54000040);
  assert(branchEqual.has_value());
  assert(branchEqual->opcode == Opcode::BranchCondition);
  assert(branchEqual->source == 0 && branchEqual->immediate == 8);
  assert((serialize_operation(*branchEqual) ==
          arm64metal::translate_core::OperationPacket{15, 0, 0, 8, 0, 64}));
  const auto branchEqualLowered = make_operation_instruction(
      *branchEqual, {0x1008, 0x1004}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(branchEqualLowered.code == std::vector<std::string>{
      "pc = arm64_condition_holds(0u, (uint(z) | (uint(n) << 1u) | "
      "(uint(v) << 2u) | (uint(c) << 3u))) ? 4104ul : 4100ul; break;"});
  const auto tbzW31 = decode_operation(0x36f80041U);
  assert(tbzW31.has_value() && tbzW31->opcode == Opcode::TestBitBranch &&
         tbzW31->destination == 31 && tbzW31->source == 1 &&
         tbzW31->immediate == 0 && tbzW31->shift == 8 && tbzW31->width == 32);
  assert((serialize_operation(*tbzW31) ==
          arm64metal::translate_core::OperationPacket{25, 31, 1, 0, 8, 32}));
  const auto tbzX48 = decode_operation(0xb6800041U);
  assert(tbzX48.has_value() && tbzX48->opcode == Opcode::TestBitBranch &&
         tbzX48->destination == 48 && tbzX48->source == 1 &&
         tbzX48->immediate == 0 && tbzX48->shift == 8 && tbzX48->width == 64);
  const auto tbnzX48 = decode_operation(0xb7800041U);
  assert(tbnzX48.has_value() && tbnzX48->destination == 48 &&
         tbnzX48->immediate == 1 && tbnzX48->width == 64);
  const auto tbnzXNegative = decode_operation(0xb7ffffffU);
  assert(tbnzXNegative.has_value() && tbnzXNegative->destination == 63 &&
         tbnzXNegative->source == 31 && tbnzXNegative->immediate == 1 &&
         static_cast<std::int32_t>(tbnzXNegative->shift) == -4 &&
         tbnzXNegative->width == 64);
  const auto tbzWLowered = make_operation_instruction(
      *tbzW31, {0x120000008LL, 0x120000004LL}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert((tbzWLowered.next == std::vector<std::int64_t>{0x120000008LL,
                                                        0x120000004LL}));
  assert(tbzWLowered.code.size() == 1 &&
         tbzWLowered.code[0].find("4831838216ul : 4831838212ul") !=
             std::string::npos);
  const auto tbzGuestEdges = arm64metal::translate_core::decode(
      0x36f80041U, 0x120000000LL, "ul");
  assert((tbzGuestEdges.next == std::vector<std::int64_t>{0x120000008LL,
                                                          0x120000004LL}));
  const auto expectedCondition = [](std::uint32_t condition, std::uint32_t flags) {
    const bool z = (flags & 1U) != 0;
    const bool n = (flags & 2U) != 0;
    const bool v = (flags & 4U) != 0;
    const bool c = (flags & 8U) != 0;
    switch (condition) {
      case 0: return z;
      case 1: return !z;
      case 2: return c;
      case 3: return !c;
      case 4: return n;
      case 5: return !n;
      case 6: return v;
      case 7: return !v;
      case 8: return c && !z;
      case 9: return !c || z;
      case 10: return n == v;
      case 11: return n != v;
      case 12: return !z && n == v;
      case 13: return z || n != v;
      default: return true;
    }
  };
  for (std::uint32_t flags = 0; flags < 16; ++flags) {
    for (std::uint32_t condition = 0; condition < 16; ++condition) {
      assert(arm64metal::arm64_condition_holds(condition, flags) ==
             expectedCondition(condition, flags));
    }
  }

  const auto wide = decode_operation(0xd2a24680);
  assert(wide.has_value());
  assert(wide->opcode == Opcode::MoveWideZero);
  assert(wide->destination == 0);
  assert(wide->immediate == 0x1234);
  assert(wide->shift == 16);
  assert(wide->width == 64);

  const auto words = serialize_operation(*wide);
  assert((words == arm64metal::translate_core::OperationPacket{
                       1, 0, 0, 0x1234, 16, 64}));

  const auto storePairXOffset = decode_operation(0xa90107e0);
  assert(storePairXOffset.has_value());
  assert(storePairXOffset->opcode == Opcode::StorePair);
  assert(storePairXOffset->destination == 0);
  assert(storePairXOffset->source == 31);
  assert(storePairXOffset->immediate == 16);
  assert(storePairXOffset->shift == ((2U << 5U) | 1U));
  assert(storePairXOffset->width == 64);
  assert((serialize_operation(*storePairXOffset) ==
          arm64metal::translate_core::OperationPacket{14, 0, 31, 16, 65, 64}));

  const auto loadPairXPost = decode_operation(0xa8c117e4);
  assert(loadPairXPost.has_value());
  assert(loadPairXPost->opcode == Opcode::LoadPair);
  assert(loadPairXPost->destination == 4);
  assert(loadPairXPost->source == 31);
  assert(loadPairXPost->immediate == 16);
  assert(loadPairXPost->shift == ((1U << 5U) | 5U));
  assert(loadPairXPost->width == 64);

  const auto storePairWPre = decode_operation(0x29be27e8);
  assert(storePairWPre.has_value());
  assert(storePairWPre->opcode == Opcode::StorePair);
  assert(static_cast<std::int32_t>(storePairWPre->immediate) == -16);
  assert(storePairWPre->shift == ((3U << 5U) | 9U));
  assert(storePairWPre->width == 32);

  const auto loadPairWOffset = decode_operation(0x294127e8);
  assert(loadPairWOffset.has_value());
  assert(loadPairWOffset->opcode == Opcode::LoadPair);
  assert(loadPairWOffset->immediate == 8);
  assert(loadPairWOffset->shift == ((2U << 5U) | 9U));
  assert(loadPairWOffset->width == 32);

  assert(!decode_operation(0xa94103e0).has_value());
  assert(!decode_operation(0xa9c10821).has_value());

  const auto loadXPre = decode_operation(0xf85f0c20);
  assert(loadXPre.has_value());
  assert(loadXPre->opcode == Opcode::LoadUnscaledWriteback);
  assert(loadXPre->destination == 0 && loadXPre->source == 1);
  assert(static_cast<std::int32_t>(loadXPre->immediate) == -16);
  assert(loadXPre->shift == 3 && loadXPre->width == 64);
  assert((serialize_operation(*loadXPre) ==
          arm64metal::translate_core::OperationPacket{17, 0, 1, 0xfffffff0U, 3, 64}));

  const auto loadXPreZero = decode_operation(0xf8400c20);
  assert(loadXPreZero.has_value());
  assert(loadXPreZero->opcode == Opcode::LoadUnscaledWriteback);
  assert(loadXPreZero->destination == 0 && loadXPreZero->source == 1);
  assert(loadXPreZero->immediate == 0 && loadXPreZero->shift == 3);
  assert(loadXPreZero->width == 64);

  const auto storeWPost = decode_operation(0xb80047e2);
  assert(storeWPost.has_value());
  assert(storeWPost->opcode == Opcode::StoreUnscaledWriteback);
  assert(storeWPost->destination == 2 && storeWPost->source == 31);
  assert(storeWPost->immediate == 4 && storeWPost->shift == 1);
  assert(storeWPost->width == 32);

  const auto loadWUnscaled = decode_operation(0xb85fc3e3);
  assert(loadWUnscaled.has_value());
  assert(loadWUnscaled->opcode == Opcode::LoadUnscaledWriteback);
  assert(loadWUnscaled->destination == 3 && loadWUnscaled->source == 31);
  assert(static_cast<std::int32_t>(loadWUnscaled->immediate) == -4);
  assert(loadWUnscaled->shift == 0 && loadWUnscaled->width == 32);

  const auto storeXzrUnscaled = decode_operation(0xf81f83ff);
  assert(storeXzrUnscaled.has_value());
  assert(storeXzrUnscaled->opcode == Opcode::StoreUnscaledWriteback);
  assert(storeXzrUnscaled->destination == 31 && storeXzrUnscaled->source == 31);
  assert(static_cast<std::int32_t>(storeXzrUnscaled->immediate) == -8);
  assert(storeXzrUnscaled->shift == 0 && storeXzrUnscaled->width == 64);

  assert(!decode_operation(0xf8408c00).has_value());

  struct NarrowMemoryCase {
    std::uint32_t encoding;
    Opcode opcode;
    std::uint32_t target;
    std::uint32_t base;
    std::int32_t offset;
    std::uint32_t mode;
    std::uint32_t width;
  };
  const std::array<NarrowMemoryCase, 12> narrowMemoryCases{{
      {0x390004c2, Opcode::StoreUnsignedImmediate, 2, 6, 1, 0, 8},
      {0x394004c3, Opcode::LoadUnsignedImmediate, 3, 6, 1, 0, 8},
      {0x790004c2, Opcode::StoreUnsignedImmediate, 2, 6, 1, 1, 16},
      {0x794004c3, Opcode::LoadUnsignedImmediate, 3, 6, 1, 1, 16},
      {0x381ff0c2, Opcode::StoreUnscaledWriteback, 2, 6, -1, 0, 8},
      {0x385ff0c3, Opcode::LoadUnscaledWriteback, 3, 6, -1, 0, 8},
      {0x781fe0c2, Opcode::StoreUnscaledWriteback, 2, 6, -2, 0, 16},
      {0x785fe0c3, Opcode::LoadUnscaledWriteback, 3, 6, -2, 0, 16},
      {0x38001cc2, Opcode::StoreUnscaledWriteback, 2, 6, 1, 3, 8},
      {0x385ff4c3, Opcode::LoadUnscaledWriteback, 3, 6, -1, 1, 8},
      {0x780024c2, Opcode::StoreUnscaledWriteback, 2, 6, 2, 1, 16},
      {0x785fecc3, Opcode::LoadUnscaledWriteback, 3, 6, -2, 3, 16},
  }};
  for (const auto& memoryCase : narrowMemoryCases) {
    const auto decoded = decode_operation(memoryCase.encoding);
    assert(decoded.has_value());
    assert(decoded->opcode == memoryCase.opcode);
    assert(decoded->destination == memoryCase.target);
    assert(decoded->source == memoryCase.base);
    assert(static_cast<std::int32_t>(decoded->immediate) == memoryCase.offset);
    assert(decoded->shift == memoryCase.mode);
    assert(decoded->width == memoryCase.width);
  }
  assert(!decode_operation(0x38400c00).has_value());

  struct SignedScalarMemoryCase {
    std::uint32_t encoding;
    std::uint32_t target;
    std::uint32_t base;
    std::int32_t offset;
    std::uint32_t modeAndTargetWidth;
    std::uint32_t accessWidth;
  };
  const std::array<SignedScalarMemoryCase, 12> signedMemoryCases{{
      {0x39c00cc9, 9, 6, 3, 0, 8},
      {0x389fc0c9, 9, 6, -4, 5, 8},
      {0x38c05cc9, 9, 6, 5, 3, 8},
      {0x38dfb4c9, 9, 6, -5, 2, 8},
      {0x79c00cc9, 9, 6, 6, 0, 16},
      {0x789fe0c9, 9, 6, -2, 5, 16},
      {0x78808cc9, 9, 6, 8, 7, 16},
      {0x789f84c9, 9, 6, -8, 6, 16},
      {0xb9800cc9, 9, 6, 12, 4, 32},
      {0xb89f80c9, 9, 6, -8, 5, 32},
      {0xb8810cc9, 9, 6, 16, 7, 32},
      {0xb89f04c9, 9, 6, -16, 6, 32},
  }};
  for (const auto& memoryCase : signedMemoryCases) {
    const auto decoded = decode_operation(memoryCase.encoding);
    assert(decoded.has_value());
    assert(decoded->opcode == Opcode::LoadSignedScalar);
    assert(decoded->destination == memoryCase.target);
    assert(decoded->source == memoryCase.base);
    assert(static_cast<std::int32_t>(decoded->immediate) == memoryCase.offset);
    assert(decoded->shift == memoryCase.modeAndTargetWidth);
    assert(decoded->width == memoryCase.accessWidth);
  }
  assert(!decode_operation(0x38800c00).has_value());
  assert(!decode_operation(0xb9c00000).has_value());
  assert(!decode_operation(0xb8c00000).has_value());

  const auto lowered = make_operation_instruction(
      *wide, {0x1004}, LoweringMode::Process, std::uint64_t{1} << 48);
  assert(lowered.operation.has_value());
  assert(lowered.next == std::vector<std::int64_t>{0x1004});
  assert(lowered.code ==
         std::vector<std::string>{
             "x[0] = move_wide_apply(1u, x[0], 4660u, 16u, 64u);"});

  const auto zeroRegister = decode_operation(0xd2c2469f);
  assert(zeroRegister.has_value());
  const auto discarded = make_operation_instruction(
      *zeroRegister, {0x1004}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(discarded.operation->destination == 31);
  assert(discarded.code.empty());

  const auto moveNot = decode_operation(0x92a24680);
  assert(moveNot.has_value());
  assert(moveNot->opcode == Opcode::MoveWideNot);
  assert(moveNot->width == 64);
  assert(moveNot->shift == 16);
  const auto moveNotLowered = make_operation_instruction(
      *moveNot, {0x1004}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(moveNotLowered.code ==
         std::vector<std::string>{
             "x[0] = move_wide_apply(3u, x[0], 4660u, 16u, 64u);"});
  assert((serialize_operation(*moveNot) ==
          arm64metal::translate_core::OperationPacket{
              3, 0, 0, 0x1234, 16, 64}));

  const auto moveKeepWide = decode_operation(0xf2d579a0);
  assert(moveKeepWide.has_value());
  assert(moveKeepWide->opcode == Opcode::MoveWideKeep);
  const auto moveKeepWideLowered = make_operation_instruction(
      *moveKeepWide, {0x1004}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(moveKeepWideLowered.code == std::vector<std::string>{
      "x[0] = move_wide_apply(4u, x[0], 43981u, 32u, 64u);"});

  const auto moveKeepWord = decode_operation(0x72b579a0);
  assert(moveKeepWord.has_value());
  assert(moveKeepWord->width == 32);
  const auto moveKeepWordLowered = make_operation_instruction(
      *moveKeepWord, {0x1004}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(moveKeepWordLowered.code == std::vector<std::string>{
      "x[0] = move_wide_apply(4u, x[0], 43981u, 16u, 32u);"});

  const auto discardedMoveKeep = decode_operation(0xf2d579bf);
  assert(discardedMoveKeep.has_value());
  assert(make_operation_instruction(
             *discardedMoveKeep, {0x1004}, LoweringMode::Process,
             std::uint64_t{1} << 48)
             .code.empty());

  const auto returnOperation = decode_operation(0xd65f03c0);
  assert(returnOperation.has_value());
  assert(returnOperation->opcode == Opcode::Return);
  assert(returnOperation->source == 30);
  const auto returned = make_operation_instruction(
      *returnOperation, {}, LoweringMode::Process, std::uint64_t{1} << 48);
  assert(returned.ends_block_at(0x1000));
  assert(returned.code.size() == 5);

  const auto adrEncoding = [](std::uint32_t base, std::uint32_t imm21,
                              std::uint32_t rd) {
    return base | ((imm21 & 3U) << 29U) | ((imm21 >> 2U) << 5U) | rd;
  };
  const auto adrForward = decode_operation(adrEncoding(0x10000000U, 8U, 4U));
  assert(adrForward && adrForward->opcode == Opcode::Adr);
  assert(adrForward->destination == 4 && adrForward->source == 0);
  assert(adrForward->immediate == 8 && adrForward->width == 64);
  const auto adrLowered = make_operation_instruction(
      *adrForward, {0x2003}, LoweringMode::Process, std::uint64_t{1} << 48,
      0x1fffU);
  assert(adrLowered.code == std::vector<std::string>{
      "x[4] = adr_apply(0u, 8191ul, 8u);"});
  assert(arm64metal::adr_apply(0, 0x1fffU, 8U) == 0x2007U);
  const auto adrTranslated = arm64metal::translate_core::decode(
      adrEncoding(0x10000000U, 8U, 4U), 0x1fff, "ul");
  assert(adrTranslated.operation && adrTranslated.operation->opcode == Opcode::Adr);
  assert(adrTranslated.code == adrLowered.code);
  assert(arm64metal::adr_apply(1, 0x2000U, 0x1fffffU) == 0x1000U);
  const auto adrpForward = decode_operation(adrEncoding(0x90000000U, 1U, 5U));
  assert(adrpForward && adrpForward->opcode == Opcode::Adr);
  assert(adrpForward->source == 1 && adrpForward->immediate == 1);
  assert(arm64metal::adr_apply(1, 0x1fffU, 1U) == 0x2000U);
  const auto adrDiscard = decode_operation(
      adrEncoding(0x10000000U, 0x1fffffU, 31U));
  assert(adrDiscard && adrDiscard->destination == 31);
  assert(make_operation_instruction(*adrDiscard, {0x2004},
                                    LoweringMode::Process).code.empty());
  auto invalidAdr = *adrForward;
  invalidAdr.source = 2;
  bool invalidAdrRejected = false;
  try {
    (void)make_operation_instruction(invalidAdr, {0x2004},
                                     LoweringMode::Process);
  } catch (const std::invalid_argument&) {
    invalidAdrRejected = true;
  }
  assert(invalidAdrRejected);
  invalidAdr = *adrForward;
  invalidAdr.width = 32;
  invalidAdrRejected = false;
  try {
    (void)make_operation_instruction(invalidAdr, {0x2004},
                                     LoweringMode::Process);
  } catch (const std::invalid_argument&) {
    invalidAdrRejected = true;
  }
  assert(invalidAdrRejected);
  assert(std::string_view(arm64metal::typed_integer_msl_source).find(
             "inline ulong adr_apply") != std::string_view::npos);

  const auto forwardBranch = decode_operation(0x14000002);
  assert(forwardBranch.has_value());
  assert(forwardBranch->opcode == Opcode::Branch);
  assert(static_cast<std::int32_t>(forwardBranch->immediate) == 8);
  const auto branch = make_operation_instruction(
      *forwardBranch, {0x500c}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(branch.next == std::vector<std::int64_t>{0x500c});
  assert(branch.ends_block_at(0x5004));
  assert(branch.code == std::vector<std::string>{"pc = 20492ul; break;"});

  const auto pacibsp = decode_authenticated_return_operation(0xd503237f);
  assert(pacibsp.has_value());
  assert(pacibsp->opcode == Opcode::AuthenticatedReturn);
  assert(pacibsp->destination == static_cast<std::uint32_t>(
                                    AuthenticatedReturnStep::SignLinkRegister));
  assert((serialize_operation(*pacibsp) ==
          arm64metal::translate_core::OperationPacket{36, 0, 0, 0, 0, 0}));
  const auto pacibspLowered = make_operation_instruction(
      *pacibsp, {0x1004}, LoweringMode::Process, std::uint64_t{1} << 48);
  assert(pacibspLowered.code.size() == 3);
  assert(pacibspLowered.code[0].find("x[37] >= 256ul") != std::string::npos);
  assert(pacibspLowered.code[1].find("38ul + 3ul * x[37]++") != std::string::npos);

  const auto autibsp = decode_authenticated_return_operation(0xd50323ff);
  assert(autibsp.has_value());
  assert(autibsp->destination == static_cast<std::uint32_t>(
                                    AuthenticatedReturnStep::AuthenticateLinkRegister));
  const auto autibspLowered = make_operation_instruction(
      *autibsp, {0x1008}, LoweringMode::Process, std::uint64_t{1} << 48);
  assert(autibspLowered.code.size() == 3);
  assert(autibspLowered.code[2].find("x[30] != x[slot]") != std::string::npos);
  assert(autibspLowered.code[2].find("x[31] != x[slot + 1]") != std::string::npos);

  const auto retab = decode_authenticated_return_operation(0xd65f0fff);
  assert(retab.has_value());
  assert(retab->destination ==
         static_cast<std::uint32_t>(AuthenticatedReturnStep::Return));
  const auto retabLowered = make_operation_instruction(
      *retab, {}, LoweringMode::Process, std::uint64_t{1} << 48);
  assert(retabLowered.ends_block_at(0x1008));
  assert(retabLowered.code.size() == 8);
  assert(retabLowered.code[2].find("x[30] != x[slot]") != std::string::npos);
  assert(retabLowered.code[7] == "x[34] = 7; return;");
  assert(!decode_authenticated_return_operation(0xd503233f).has_value());
  assert(!decode_authenticated_return_operation(0xd50323bf).has_value());
  assert(!decode_authenticated_return_operation(0xd65f0bff).has_value());
  assert(!decode_operation(0xd503237f).has_value());

  const auto backwardBranch = decode_operation(0x17ffffff);
  assert(backwardBranch.has_value());
  assert(static_cast<std::int32_t>(backwardBranch->immediate) == -4);

  const auto branchLinkForward = decode_operation(0x94000002U);
  assert(branchLinkForward && branchLinkForward->opcode == Opcode::BranchLink);
  assert(static_cast<std::int32_t>(branchLinkForward->immediate) == 8);
  assert(branchLinkForward->destination == 0 && branchLinkForward->source == 0 &&
         branchLinkForward->shift == 0 && branchLinkForward->width == 64);
  const auto branchLinkLowered = make_operation_instruction(
      *branchLinkForward, {0x1008, 0x1004}, LoweringMode::Process, 0x10000,
      0x1000);
  assert((branchLinkLowered.code == std::vector<std::string>{
      "x[30] = 4100ul;", "pc = 4104ul; break;"}));
  assert((branchLinkLowered.next == std::vector<std::int64_t>{0x1008, 0x1004}));
  assert(serialize_operation(*branchLinkForward)[0] == 28);
  const auto branchLinkBackward = decode_operation(0x97ffffffU);
  assert(branchLinkBackward && branchLinkBackward->opcode == Opcode::BranchLink);
  assert(static_cast<std::int32_t>(branchLinkBackward->immediate) == -4);
  const auto backwardCall = make_operation_instruction(
      *branchLinkBackward, {0x1000, 0x1008}, LoweringMode::Process, 0x10000,
      0x1004);
  assert(backwardCall.code[0] == "x[30] = 4104ul;");
  assert(backwardCall.code[1] == "pc = 4096ul; break;");
  bool branchLinkRejected = false;
  try {
    (void)make_operation_instruction(*branchLinkForward, {0x1008},
                                     LoweringMode::Process, 0x10000, 0x1000);
  } catch (const std::invalid_argument&) {
    branchLinkRejected = true;
  }
  assert(branchLinkRejected);
  branchLinkRejected = false;
  try {
    (void)make_operation_instruction(*branchLinkForward, {0x1008, 0x1004},
                                     LoweringMode::Function, 0x10000, 0x1000);
  } catch (const std::invalid_argument&) {
    branchLinkRejected = true;
  }
  assert(branchLinkRejected);
  auto invalidBranchLink = *branchLinkForward;
  invalidBranchLink.immediate = 9;
  branchLinkRejected = false;
  try {
    (void)make_operation_instruction(invalidBranchLink, {0x1009, 0x1004},
                                     LoweringMode::Process, 0x10000, 0x1000);
  } catch (const std::invalid_argument&) {
    branchLinkRejected = true;
  }
  assert(branchLinkRejected);
  invalidBranchLink = *branchLinkForward;
  invalidBranchLink.width = 32;
  branchLinkRejected = false;
  try {
    (void)make_operation_instruction(invalidBranchLink, {0x1008, 0x1004},
                                     LoweringMode::Process, 0x10000, 0x1000);
  } catch (const std::invalid_argument&) {
    branchLinkRejected = true;
  }
  assert(branchLinkRejected);
  const auto importedBranchLink = arm64metal::decode_process(
      0x94000002U, 0x1000, {{0x1008, "_exit"}}, {}, false);
  assert(!importedBranchLink.operation);
  assert(importedBranchLink.next.empty());
  assert(importedBranchLink.code.back().find("x[35] = 169ul") !=
         std::string::npos);
  const auto stubBranchLink = arm64metal::decode_process(
      0x94000002U, 0x1000, {}, {{0x1008, 0x2000}}, false);
  assert(!stubBranchLink.operation);
  assert((stubBranchLink.next == std::vector<std::int64_t>{0x2000, 0x1004}));
  assert(stubBranchLink.code[0] == "x[30] = 4100ul;");
  assert(stubBranchLink.code[1] == "pc = 8192ul; break;");

  const auto branchRegister = decode_operation(0xd61f0120U);  // BR X9.
  assert(branchRegister && branchRegister->opcode == Opcode::BranchRegister &&
         branchRegister->destination == 0 && branchRegister->source == 9 &&
         branchRegister->immediate == 0 && branchRegister->shift == 0 &&
         branchRegister->width == 64);
  assert((serialize_operation(*branchRegister) ==
          arm64metal::translate_core::OperationPacket{30, 0, 9, 0, 0, 64}));
  const auto branchRegisterLowered = make_operation_instruction(
      *branchRegister, {}, LoweringMode::Process, 0x10000, 0x2000);
  assert(branchRegisterLowered.next.empty() &&
         branchRegisterLowered.ends_block_at(0x2000));
  assert((branchRegisterLowered.code == std::vector<std::string>{
      "x[33] = x[9];",
      "if (x[33] == 0ul || x[33] >= 65536ul || (x[33] & 3ul) != 0ul) { "
      "x[874] = x[33]; x[33] = 8192ul; x[34] = 3; return; }",
      "x[34] = 7; return;"}));

  const auto branchLinkRegister = decode_operation(0xd63f03c0U);  // BLR X30.
  assert(branchLinkRegister && branchLinkRegister->opcode == Opcode::BranchRegister &&
         branchLinkRegister->source == 30 && branchLinkRegister->immediate == 1 &&
         branchLinkRegister->width == 64);
  const auto branchLinkRegisterLowered = make_operation_instruction(
      *branchLinkRegister, {}, LoweringMode::Process, 0x10000, 0x2000);
  assert((branchLinkRegisterLowered.code == std::vector<std::string>{
      "x[33] = x[30];",
      "x[30] = 8196ul;",
      "if (x[33] == 0ul || x[33] >= 65536ul || (x[33] & 3ul) != 0ul) { "
      "x[874] = x[33]; x[33] = 8192ul; x[34] = 3; return; }",
      "x[34] = 7; return;"}));
  assert((serialize_operation(*branchLinkRegister) ==
          arm64metal::translate_core::OperationPacket{30, 0, 30, 1, 0, 64}));

  const auto branchZeroRegister = decode_operation(0xd61f03e0U);  // BR XZR.
  const auto branchLinkZeroRegister = decode_operation(0xd63f03e0U);  // BLR XZR.
  assert(branchZeroRegister && branchZeroRegister->source == 31 &&
         branchZeroRegister->immediate == 0);
  assert(branchLinkZeroRegister && branchLinkZeroRegister->source == 31 &&
         branchLinkZeroRegister->immediate == 1);
  assert(make_operation_instruction(*branchZeroRegister, {},
                                    LoweringMode::Process, 0x10000, 0x2000)
             .code[0] == "x[33] = 0ul;");
  assert(!decode_operation(0xd61f0800U).has_value());  // Reserved fixed bit.
  const auto indirectWithMap = arm64metal::decode_process(
      0xd63f0120U, 0x2000, {{0x9000, "_exit"}}, {{0x8000, 0x7000}}, false);
  assert(indirectWithMap.operation &&
         indirectWithMap.operation->opcode == Opcode::BranchRegister &&
         indirectWithMap.next.empty() &&
         indirectWithMap.code[0] == "x[33] = x[9];");
  auto invalidBranchRegister = *branchRegister;
  invalidBranchRegister.source = 32;
  bool branchRegisterRejected = false;
  try {
    (void)make_operation_instruction(invalidBranchRegister, {},
                                     LoweringMode::Process, 0x10000, 0x2000);
  } catch (const std::invalid_argument&) {
    branchRegisterRejected = true;
  }
  assert(branchRegisterRejected);
  invalidBranchRegister = *branchRegister;
  invalidBranchRegister.width = 32;
  branchRegisterRejected = false;
  try {
    (void)make_operation_instruction(invalidBranchRegister, {},
                                     LoweringMode::Process, 0x10000, 0x2000);
  } catch (const std::invalid_argument&) {
    branchRegisterRejected = true;
  }
  assert(branchRegisterRejected);
  branchRegisterRejected = false;
  try {
    (void)make_operation_instruction(*branchRegister, {0x2004},
                                     LoweringMode::Process, 0x10000, 0x2000);
  } catch (const std::invalid_argument&) {
    branchRegisterRejected = true;
  }
  assert(branchRegisterRejected);

  const auto loadWord = decode_operation(0xb9400403);
  assert(loadWord.has_value());
  assert(loadWord->opcode == Opcode::LoadUnsignedImmediate);
  assert(loadWord->destination == 3);
  assert(loadWord->source == 0);
  assert(loadWord->immediate == 1);
  assert(loadWord->shift == 2);
  assert(loadWord->width == 32);
  assert((serialize_operation(*loadWord) ==
          arm64metal::translate_core::OperationPacket{6, 3, 0, 1, 2, 32}));

  const auto storeWord = decode_operation(0xb9000401);
  assert(storeWord.has_value());
  assert(storeWord->opcode == Opcode::StoreUnsignedImmediate);
  assert(storeWord->destination == 1);
  assert(storeWord->source == 0);
  assert(storeWord->immediate == 1);
  assert(storeWord->shift == 2);
  assert(storeWord->width == 32);

  const auto loadDouble = decode_operation(0xf9400804);
  assert(loadDouble.has_value());
  assert(loadDouble->opcode == Opcode::LoadUnsignedImmediate);
  assert(loadDouble->destination == 4);
  assert(loadDouble->source == 0);
  assert(loadDouble->immediate == 2);
  assert(loadDouble->shift == 3);
  assert(loadDouble->width == 64);
  assert((serialize_operation(*loadDouble) ==
          arm64metal::translate_core::OperationPacket{6, 4, 0, 2, 3, 64}));

  const auto storeDouble = decode_operation(0xf9000802);
  assert(storeDouble.has_value());
  assert(storeDouble->opcode == Opcode::StoreUnsignedImmediate);
  assert(storeDouble->destination == 2);
  assert(storeDouble->source == 0);
  assert(storeDouble->immediate == 2);
  assert(storeDouble->shift == 3);
  assert(storeDouble->width == 64);

  const auto stackLoad = decode_operation(0xf94003e1);
  assert(stackLoad.has_value());
  assert(stackLoad->opcode == Opcode::LoadUnsignedImmediate);
  assert(stackLoad->source == 31);
  assert(stackLoad->destination == 1);
  const auto cbzX = decode_operation(0xb4000080);
  assert(cbzX.has_value());
  assert(cbzX->opcode == Opcode::BranchZero);
  assert(cbzX->source == 0);
  assert(cbzX->immediate == 0);
  assert(static_cast<std::int32_t>(cbzX->shift) == 16);
  assert(cbzX->width == 64);
  assert((serialize_operation(*cbzX) ==
          arm64metal::translate_core::OperationPacket{8, 0, 0, 0, 16, 64}));
  const auto cbzLowered = make_operation_instruction(
      *cbzX, {0x5014, 0x5008}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(cbzLowered.code == std::vector<std::string>{
      "pc = (x[0] == 0) ? 20500ul : 20488ul; break;"});
  const auto decodedCbz = arm64metal::translate_core::decode(
      0xb4000080, 0x100004000, "ul");
  assert(decodedCbz.next ==
         (std::vector<std::int64_t>{0x100004010, 0x100004004}));

  const auto cbnzW = decode_operation(0x35000080);
  assert(cbnzW.has_value());
  assert(cbnzW->opcode == Opcode::BranchZero);
  assert(cbnzW->source == 0);
  assert(cbnzW->immediate == 1);
  assert(cbnzW->width == 32);
  const auto cbnzLowered = make_operation_instruction(
      *cbnzW, {0x5014, 0x5008}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(cbnzLowered.code == std::vector<std::string>{
      "pc = (uint(x[0]) != 0) ? 20500ul : 20488ul; break;"});
  const auto backwardCbz = decode_operation(0xb4ffffe0);
  assert(backwardCbz.has_value());
  assert(static_cast<std::int32_t>(backwardCbz->shift) == -4);

  const auto addWide = decode_operation(0x91448c00);
  assert(addWide.has_value());
  assert(addWide->opcode == Opcode::AddImmediate);
  assert(addWide->destination == 0 && addWide->source == 0);
  assert(addWide->immediate == 0x123 && addWide->shift == 12);
  assert(addWide->width == 64);
  assert((serialize_operation(*addWide) ==
          arm64metal::translate_core::OperationPacket{9, 0, 0, 0x123, 12, 64}));
  const auto addWideLowered = make_operation_instruction(
      *addWide, {0x1004}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(addWideLowered.code[2] ==
         "const AddSubImmediateResult add_sub_result = "
         "add_sub_immediate_apply(0u, x[0], 291u, 12u, 64u, 0u, previous_flags);");

  const auto subWide = decode_operation(0xd1448c00);
  assert(subWide.has_value() && subWide->opcode == Opcode::SubImmediate);
  assert(subWide->immediate == 0x123 && subWide->shift == 12 &&
         subWide->width == 64);
  const auto subWord = decode_operation(0x51448c00);
  assert(subWord.has_value() && subWord->opcode == Opcode::SubImmediate);
  assert(subWord->width == 32 && subWord->immediate == 0x123 &&
         subWord->shift == 12);
  const auto addWord = decode_operation(0x11000400);
  assert(addWord.has_value() && addWord->opcode == Opcode::AddImmediate);
  assert(addWord->width == 32 && addWord->immediate == 1 &&
         addWord->shift == 0);
  const auto addSpSp = decode_operation(0x910003ff);
  assert(addSpSp.has_value() && addSpSp->opcode == Opcode::AddImmediate);
  assert(addSpSp->destination == 31 && addSpSp->source == 31);
  const auto addFromSp = decode_operation(0x91400be0);
  assert(addFromSp.has_value() && addFromSp->destination == 0 &&
         addFromSp->source == 31 && addFromSp->shift == 12);
  const auto addToSp = decode_operation(0x9100003f);
  assert(addToSp.has_value() && addToSp->destination == 31 &&
         addToSp->source == 1);
  const auto addWsp = decode_operation(0x1100015f);
  assert(addWsp.has_value() && addWsp->opcode == Opcode::AddImmediate);
  assert(addWsp->destination == 31 && addWsp->source == 10 &&
         addWsp->width == 32);
  const auto addFromWsp = decode_operation(0x910007e0);
  assert(addFromWsp.has_value() &&
         addFromWsp->opcode == Opcode::AddImmediate);
  assert(addFromWsp->destination == 0 && addFromWsp->source == 31 &&
         addFromWsp->immediate == 1 && addFromWsp->shift == 0 &&
         addFromWsp->width == 64);
  const auto cmn = decode_operation(0xb100043f);
  assert(cmn.has_value() && cmn->opcode == Opcode::AddImmediateSetFlags);
  assert(cmn->destination == 31 && cmn->source == 1);
  assert((serialize_operation(*cmn) ==
          arm64metal::translate_core::OperationPacket{11, 31, 1, 1, 0, 64}));
  const auto cmp = decode_operation(0xf100043f);
  assert(cmp.has_value() && cmp->opcode == Opcode::SubImmediateSetFlags);
  assert(cmp->destination == 31 && cmp->source == 1);
  const auto cmpLowered = make_operation_instruction(
      *cmp, {0x1004}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(cmpLowered.code.size() == 9);
  assert(cmpLowered.code[2] ==
         "const AddSubImmediateResult add_sub_result = "
         "add_sub_immediate_apply(1u, x[1], 1u, 0u, 64u, 1u, previous_flags);");
  assert(cmpLowered.code[3] == "z = (add_sub_result.flags & 1u) != 0u;");
  const auto functionCmp = arm64metal::translate_core::decode(0xf100043f, 0x1000);
  assert(functionCmp.operation.has_value() &&
         functionCmp.operation->opcode == Opcode::SubImmediateSetFlags);
  assert(functionCmp.code.size() == 8);
  assert(functionCmp.code[2] == cmpLowered.code[2]);
  assert(functionCmp.code[3] == cmpLowered.code[3]);
  assert(functionCmp.code.back() == "}");
  const auto functionAddSp = arm64metal::translate_core::decode(0x910003ff, 0x2000);
  assert(functionAddSp.operation.has_value() &&
         functionAddSp.operation->destination == 31 &&
         functionAddSp.operation->source == 31);
  assert(functionAddSp.code[3] == "x[31] = add_sub_result.value;");
  const auto cmpSp = decode_operation(0xf10003ff);
  assert(cmpSp.has_value() && cmpSp->opcode == Opcode::SubImmediateSetFlags);
  assert(cmpSp->destination == 31 && cmpSp->source == 31);
  const auto subSpShifted = decode_operation(0xd14007ff);
  assert(subSpShifted.has_value() &&
         subSpShifted->opcode == Opcode::SubImmediate);
  assert(subSpShifted->destination == 31 && subSpShifted->source == 31 &&
         subSpShifted->immediate == 1 && subSpShifted->shift == 12 &&
         subSpShifted->width == 64);
  const auto subSpUnshifted = decode_operation(0xd10043ff);
  assert(subSpUnshifted.has_value() &&
         subSpUnshifted->opcode == Opcode::SubImmediate);
  assert(subSpUnshifted->destination == 31 &&
         subSpUnshifted->source == 31 &&
         subSpUnshifted->immediate == 16 && subSpUnshifted->shift == 0);
  const auto cmnShifted = decode_operation(0xb140043f);
  assert(cmnShifted.has_value() &&
         cmnShifted->opcode == Opcode::AddImmediateSetFlags &&
         cmnShifted->destination == 31 && cmnShifted->source == 1 &&
         cmnShifted->shift == 12);
  const auto addsWord = decode_operation(0x31000420);
  assert(addsWord.has_value() &&
         addsWord->opcode == Opcode::AddImmediateSetFlags &&
         addsWord->width == 32 && addsWord->destination == 0);
  assert(!decode_operation(0x91800000).has_value());

  bool invalidShiftRejected = false;
  try {
    (void)decode_operation(0x52c24680);
  } catch (const std::runtime_error&) {
    invalidShiftRejected = true;
  }
  assert(invalidShiftRejected);
  for (const auto width : {32U, 64U}) {
    const std::array<std::uint64_t, 6> values{
        0U, 1U, 0x80000000ULL, 0x123456789abcdef0ULL,
        0xffffffffffffffffULL, 0x55555555aaaaaaaaULL};
    for (std::uint32_t variant = 0; variant < 4U; ++variant) {
      for (const auto value : values) {
        const auto native = native_integer_bit_operation(variant, width, value);
        const auto shared = arm64metal::integer_bit_operation_apply(
            variant, value, width);
        assert(native == shared);
      }
    }
  }
  const auto rbit = decode_operation(0xdac00000U);
  assert(rbit.has_value() && rbit->opcode == Opcode::IntegerBitOperation &&
         rbit->destination == 0 && rbit->source == 0 && rbit->immediate == 0 &&
         rbit->width == 64);
  assert((serialize_operation(*rbit) ==
          arm64metal::translate_core::OperationPacket{34, 0, 0, 0, 0, 64}));
  const auto rbitLowered = make_operation_instruction(
      *rbit, {0x1004}, LoweringMode::Process, std::uint64_t{1} << 48);
  assert(rbitLowered.code[0] ==
         "x[0] = integer_bit_operation_apply(0u, x[0], 64u);");
  const auto rbitXzr = decode_operation(0xdac0003fU);
  assert(rbitXzr.has_value() && rbitXzr->destination == 31U &&
         rbitXzr->source == 1U);
  assert(make_operation_instruction(
             *rbitXzr, {0x1004}, LoweringMode::Process,
             std::uint64_t{1} << 48).code.empty());
  const auto revWzr = decode_operation(0x5ac0083fU);
  assert(revWzr.has_value() && revWzr->destination == 31U &&
         revWzr->source == 1U && revWzr->width == 32U);
  assert(make_operation_instruction(
             *revWzr, {0x1004}, LoweringMode::Process,
             std::uint64_t{1} << 48).code.empty());
  assert(decode_operation(0x5ac00800U)->immediate == 1U);
  assert(decode_operation(0xdac00c00U)->immediate == 1U &&
         decode_operation(0xdac00c00U)->width == 64U);
  assert(decode_operation(0x5ac01000U)->immediate == 2U);
  assert(decode_operation(0xdac01400U)->immediate == 3U);
  assert(!decode_operation(0xdac01800U).has_value());
  for (const auto width : {32U, 64U}) {
    const std::array<std::uint64_t, 8> values{
        0U, 1U, 3U, 7U, 0x80000000ULL, 0xffffffffU,
        0x8000000000000000ULL,
        0xffffffffffffffffULL};
    for (const auto isSigned : {false, true}) {
      for (const auto lhs : values) {
        for (const auto rhs : values) {
          const auto native = native_integer_divide(isSigned, width, lhs, rhs);
          const auto shared = arm64metal::integer_divide_apply(
              isSigned ? 1U : 0U, lhs, rhs, width);
          assert(native == shared);
        }
      }
    }
  }
  const auto udivX = decode_operation(0x9ac00800U);
  assert(udivX.has_value() && udivX->opcode == Opcode::IntegerDivide &&
         udivX->destination == 0U && udivX->source == 0U &&
         udivX->immediate == 0U && udivX->shift == 0U && udivX->width == 64U);
  assert((serialize_operation(*udivX) ==
          arm64metal::translate_core::OperationPacket{35, 0, 0, 0, 0, 64}));
  const auto sdivW = decode_operation(0x1ac00c00U);
  assert(sdivW.has_value() && sdivW->opcode == Opcode::IntegerDivide &&
         sdivW->shift == 1U && sdivW->width == 32U);
  const auto sdivLowered = make_operation_instruction(
      *sdivW, {0x1004}, LoweringMode::Process, std::uint64_t{1} << 48);
  assert(sdivLowered.code[0] ==
         "x[0] = integer_divide_apply(1u, uint(x[0]), uint(x[0]), 32u);");
  assert(decode_operation(0x1ac00800U)->shift == 0U);
  assert(decode_operation(0x9ac00c00U)->width == 64U);
  const auto sdivX = decode_operation(0x9ac20c24U);
  assert(sdivX.has_value() && sdivX->destination == 4U &&
         sdivX->source == 1U && sdivX->immediate == 2U &&
         sdivX->shift == 1U && sdivX->width == 64U);
  assert(!decode_operation(0x9ac01000U).has_value());
  assert(arm64metal::integer_divide_apply(0U, 0x1234U, 0U, 64U) == 0U);
  assert(arm64metal::integer_divide_apply(
             1U, 0x8000000000000000ULL, 0xffffffffffffffffULL, 64U) ==
         0x8000000000000000ULL);
  assert(arm64metal::integer_divide_apply(
             1U, 0x80000000ULL, 0xffffffffULL, 32U) == 0x80000000ULL);
  assert(std::string_view(arm64metal::typed_integer_msl_source).find(
             "integer_divide_apply") != std::string_view::npos);
  const auto literalW = decode_operation(0x18000281U);
  assert(literalW && literalW->opcode == Opcode::LoadLiteral &&
         literalW->destination == 1U && literalW->source == 0U &&
         static_cast<std::int32_t>(literalW->immediate) == 80 &&
         literalW->shift == 0U && literalW->width == 32U);
  assert((serialize_operation(*literalW) ==
          arm64metal::translate_core::OperationPacket{38, 1, 0, 80, 0, 32}));
  const auto literalX = decode_operation(0x58000221U);
  assert(literalX && literalX->opcode == Opcode::LoadLiteral &&
         literalX->destination == 1U &&
         static_cast<std::int32_t>(literalX->immediate) == 68 &&
         literalX->shift == 1U && literalX->width == 64U);
  const auto literalSigned = decode_operation(0x98000181U);
  assert(literalSigned && literalSigned->opcode == Opcode::LoadLiteral &&
         literalSigned->destination == 1U &&
         static_cast<std::int32_t>(literalSigned->immediate) == 48 &&
         literalSigned->shift == 2U && literalSigned->width == 64U);
  const auto literalBackward = decode_operation(0x18ffffe1U);
  assert(literalBackward &&
         static_cast<std::int32_t>(literalBackward->immediate) == -4);
  assert(!decode_operation(0xd8000000U).has_value());  // PRFM.
  assert(!decode_operation(0x1c000000U).has_value());  // SIMD literal load.
  const auto literalLowered = make_operation_instruction(
      *literalSigned, {0x1004}, LoweringMode::Process,
      std::uint64_t{1} << 48, 0x1000);
  assert(literalLowered.operation &&
         literalLowered.operation->opcode == Opcode::LoadLiteral &&
         literalLowered.code[1].find("load_guest_word(memory + 4144ul, 4ul)") !=
             std::string::npos);
  bool invalidLiteralAddressRejected = false;
  try {
    (void)make_operation_instruction(
        *literalBackward, {4}, LoweringMode::Process,
        std::uint64_t{1} << 48, 0);
  } catch (const std::invalid_argument&) {
    invalidLiteralAddressRejected = true;
  }
  assert(invalidLiteralAddressRejected);
  const auto processLiteral = arm64metal::decode_process(
      0x18000281U, 0x1000, {}, {}, false);
  assert(processLiteral.operation &&
         processLiteral.operation->opcode == Opcode::LoadLiteral &&
         processLiteral.next == std::vector<std::int64_t>{0x1004} &&
         processLiteral.code[1].find("load_guest_word(memory + 4176ul, 4)") !=
             std::string::npos);
  bool processLiteralAddressRejected = false;
  try {
    (void)arm64metal::decode_process(0x18ffffe1U, 0, {}, {}, false);
  } catch (const std::runtime_error&) {
    processLiteralAddressRejected = true;
  }
  assert(processLiteralAddressRejected);
  return 0;
}
