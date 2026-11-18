#include "translate_core.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace arm64metal::translate_core {
namespace {

std::uint32_t field(std::uint32_t bits, unsigned start, unsigned count) {
  return (bits >> start) & ((std::uint32_t{1} << count) - 1);
}

std::int64_t signed_field(std::uint32_t value, unsigned width) {
  const auto sign = std::uint32_t{1} << (width - 1);
  return static_cast<std::int64_t>(value ^ sign) - static_cast<std::int64_t>(sign);
}

std::string reg(std::uint32_t index, bool wide) {
  if (index == 31) return wide ? "0ul" : "0u";
  return wide ? "x[" + std::to_string(index) + "]"
              : "uint(x[" + std::to_string(index) + "])";
}

std::string pc_literal(std::int64_t pc, const std::string& suffix) {
  return std::to_string(pc) + suffix;
}

}  // namespace

OperationPacket serialize_operation(const Operation& operation) {
  return {static_cast<std::uint32_t>(operation.opcode),
          operation.destination, operation.source, operation.immediate,
          operation.shift, operation.width};
}

std::optional<std::uint64_t> decode_logical_immediate_mask(
    std::uint32_t n, std::uint32_t immr, std::uint32_t imms,
    std::uint32_t width) {
  if (n > 1U || immr > 63U || imms > 63U ||
      (width != 32U && width != 64U)) {
    return std::nullopt;
  }
  const auto marker = (static_cast<std::uint64_t>(n) << 6U) |
                       (~static_cast<std::uint64_t>(imms) & 63U);
  if (marker < 2U) return std::nullopt;
  const auto element = std::uint32_t{1} << (63U - __builtin_clzll(marker));
  const auto length = (imms & (element - 1U)) + 1U;
  if (element > width || length >= element) return std::nullopt;
  const auto rotation = immr & (element - 1U);
  const auto ones = (std::uint64_t{1} << length) - 1U;
  const auto elementMask = element == 64U
      ? UINT64_MAX : ((std::uint64_t{1} << element) - 1U);
  const auto pattern = rotation == 0U
      ? ones
      : ((ones >> rotation) | (ones << (element - rotation))) & elementMask;
  std::uint64_t mask = 0;
  for (std::uint32_t offset = 0; offset < width; offset += element) {
    mask |= pattern << offset;
  }
  return mask;
}

bool Instruction::ends_block_at(std::int64_t pc) const {
  const auto straight_line = next.size() == 1 && next.front() == pc + 4;
  const auto ends_with = [](const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  const auto terminal = !code.empty() &&
                        (ends_with(code.back(), "break;") || ends_with(code.back(), "return;"));
  return !straight_line || terminal;
}

std::optional<Operation> decode_operation(std::uint32_t bits) {
  if (const auto vectorMemory = decode_vector_memory_operation(bits)) {
    return vectorMemory;
  }
  // FEAT_LRCPC2 scalar LDAPUR*/STLUR* uses an unscaled signed imm9. Reuse
  // the interpreter's scalar unscaled memory packet variants; ordering is
  // governed by the existing single-active-guest-thread dispatch model.
  if ((bits & 0x3f200c00U) == 0x19000000U) {
    const auto size = field(bits, 30, 2);
    const auto opc = field(bits, 22, 2);
    const auto width = 8U << size;
    const auto target = field(bits, 0, 5);
    const auto base = field(bits, 5, 5);
    const auto displacement = signed_field(field(bits, 12, 9), 9);
    const auto encodedDisplacement = static_cast<std::uint32_t>(displacement);
    if (opc == 0U) {
      return Operation{Opcode::StoreUnscaledWriteback, target, base,
                       encodedDisplacement, 0, width};
    }
    if (opc == 1U) {
      return Operation{Opcode::LoadUnscaledWriteback, target, base,
                       encodedDisplacement, 0, width};
    }
    if ((opc == 2U && size <= 2U) || (opc == 3U && size <= 1U)) {
      const auto targetIsX = opc == 2U;
      return Operation{Opcode::LoadSignedScalar, target, base,
                       encodedDisplacement, 1U | (targetIsX ? 4U : 0U), width};
    }
    return std::nullopt;
  }
  // Typed unordered LDUMIN/LDUMAX/LDSMIN/LDSMAX. Keep acquire/release forms
  // on the existing translator path until their memory-order contract is represented.
  const auto atomicMinMaxClass = bits & 0x3f20ec00U;
  if ((atomicMinMaxClass == 0x38206000U || atomicMinMaxClass == 0x38204000U) &&
      (bits & 0x00c00000U) == 0U) {
    const auto width = 8U << field(bits, 30, 2);
    if (width != 32U && width != 64U) return std::nullopt;
    const auto variant = (atomicMinMaxClass == 0x38204000U ? 2U : 0U) |
                         ((bits & 0x1000U) != 0U ? 0U : 1U);
    return Operation{Opcode::LoadUnsignedMinMax, field(bits, 0, 5),
                     field(bits, 5, 5), field(bits, 16, 5), variant, width};
  }
  if ((bits & 0xffe07c10U) == 0xba000400U) {  // RMIF Xn, #shift, #mask
    return Operation{Opcode::FlagManipulation, 0, field(bits, 5, 5),
                     field(bits, 0, 4), field(bits, 15, 6), 64};
  }
  if ((bits & 0xffffbc1fU) == 0x3a00080dU) {  // SETF8/SETF16 Wn
    return Operation{Opcode::FlagManipulation, 0, field(bits, 5, 5), 0, 0,
                     field(bits, 14, 1) ? 16U : 8U};
  }
  if (bits == 0xd500401fU || bits == 0xd500403fU || bits == 0xd500405fU) {
    return Operation{Opcode::SystemFlags, 0, 0, field(bits, 5, 3), 0, 0};
  }
  if (bits == 0xd65f03c0) {
    return Operation{Opcode::Return, 0, 30, 0, 0, 64};
  }

  if ((bits & 0x3f000000U) == 0x18000000U) {
    const auto kind = field(bits, 30, 2);
    if (kind == 3U) return std::nullopt;  // PRFM is intentionally not typed.
    const auto displacement = signed_field(field(bits, 5, 19), 19) * 4;
    const auto resultWidth = kind == 0U ? 32U : 64U;
    return Operation{Opcode::LoadLiteral, field(bits, 0, 5), 0,
                     static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement)),
                     kind, resultWidth};
  }

  if ((bits & 0x7e000000) == 0x34000000) {
    const auto width = field(bits, 31, 1) == 1 ? 64u : 32u;
    const auto displacement = signed_field(field(bits, 5, 19), 19) * 4;
    return Operation{Opcode::BranchZero, 0, field(bits, 0, 5), field(bits, 24, 1),
                     static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement)),
                     width};
  }

  if ((bits & 0x7e000000U) == 0x36000000U) {
    const auto width = field(bits, 31, 1) == 1 ? 64u : 32u;
    const auto bit = field(bits, 19, 5) | (field(bits, 31, 1) << 5U);
    const auto displacement = signed_field(field(bits, 5, 14), 14) * 4;
    return Operation{Opcode::TestBitBranch, bit, field(bits, 0, 5),
                     field(bits, 24, 1),
                     static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement)),
                     width};
  }

  if ((bits & 0xff000010U) == 0x54000000U) {
    const auto displacement = signed_field(field(bits, 5, 19), 19) * 4;
    return Operation{Opcode::BranchCondition, 0, field(bits, 0, 4),
                     static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement)),
                     0, 64};
  }

  const auto addressEncoding = bits & 0x9f000000U;
  if (addressEncoding == 0x10000000U || addressEncoding == 0x90000000U) {
    const auto immediate = (field(bits, 5, 19) << 2U) | field(bits, 29, 2);
    const auto mode = addressEncoding == 0x90000000U ? 1U : 0U;
    return Operation{Opcode::Adr, field(bits, 0, 5), mode, immediate, 0, 64};
  }

  if ((bits & 0x1f800000U) == 0x12000000U) {
    const auto width = field(bits, 31, 1) == 1 ? 64U : 32U;
    const auto n = field(bits, 22, 1);
    const auto immr = field(bits, 16, 6);
    const auto imms = field(bits, 10, 6);
    const auto mask = decode_logical_immediate_mask(n, immr, imms, width);
    if (!mask) return std::nullopt;
    const auto opc = field(bits, 29, 2);
    const auto packedSource = (opc << 5U) | field(bits, 5, 5);
    return Operation{Opcode::LogicalImmediate, field(bits, 0, 5), packedSource,
                     static_cast<std::uint32_t>(*mask),
                     static_cast<std::uint32_t>(*mask >> 32U), width};
  }

  const auto selectEncoding = bits & 0x7fe00c00U;
  std::uint32_t selectVariant;
  if (selectEncoding == 0x1a800000U) {
    selectVariant = 0;
  } else if (selectEncoding == 0x1a800400U) {
    selectVariant = 1;
  } else if (selectEncoding == 0x5a800000U) {
    selectVariant = 2;
  } else if (selectEncoding == 0x5a800400U) {
    selectVariant = 3;
  } else {
    selectVariant = 0;
  }
  if (selectEncoding == 0x1a800000U || selectEncoding == 0x1a800400U ||
      selectEncoding == 0x5a800000U || selectEncoding == 0x5a800400U) {
    return Operation{Opcode::ConditionalSelect, field(bits, 0, 5), field(bits, 5, 5),
                     field(bits, 16, 5), (selectVariant << 4U) | field(bits, 12, 4),
                     field(bits, 31, 1) == 1 ? 64U : 32U};
  }

  const auto immediateBranch = bits & 0xfc000000U;
  if (immediateBranch == 0x94000000U) {
    const auto displacement = signed_field(field(bits, 0, 26), 26) * 4;
    return Operation{Opcode::BranchLink, 0, 0,
                     static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement)),
                     0, 64};
  }
  if (immediateBranch == 0x14000000U) {
    const auto displacement = signed_field(field(bits, 0, 26), 26) * 4;
    return Operation{Opcode::Branch, 0, 0,
                     static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement)),
                     0, 64};
  }

  const auto registerBranch = bits & 0xfffffc1fU;
  if (registerBranch == 0xd61f0000U || registerBranch == 0xd63f0000U) {
    return Operation{Opcode::BranchRegister, 0, field(bits, 5, 5),
                     registerBranch == 0xd63f0000U ? 1U : 0U, 0, 64};
  }

  const auto unsignedMemory = bits & 0xffc00000U;
  const auto signedUnsignedMemory = unsignedMemory;
  const auto signedUnsignedLoads =
      std::array<std::pair<std::uint32_t, std::uint32_t>, 5>{
          std::pair{0x39c00000U, 8U}, std::pair{0x39800000U, 8U},
          std::pair{0x79c00000U, 16U}, std::pair{0x79800000U, 16U},
          std::pair{0xb9800000U, 32U}};
  for (const auto& [encoding, width] : signedUnsignedLoads) {
    if (signedUnsignedMemory == encoding) {
      const auto targetIsX = field(bits, 22, 1) == 0;
      const auto bytes = width / 8U;
      return Operation{Opcode::LoadSignedScalar, field(bits, 0, 5),
                       field(bits, 5, 5),
                       field(bits, 10, 12) * bytes,
                       (targetIsX ? 4U : 0U), width};
    }
  }
  const auto memoryWidths = std::array<std::pair<std::uint32_t, std::uint32_t>, 8>{
      std::pair{0x39400000U, 8U}, std::pair{0x39000000U, 8U},
      std::pair{0x79400000U, 16U}, std::pair{0x79000000U, 16U},
      std::pair{0xb9400000U, 32U}, std::pair{0xb9000000U, 32U},
      std::pair{0xf9400000U, 64U}, std::pair{0xf9000000U, 64U}};
  for (const auto& [encoding, width] : memoryWidths) {
    if (unsignedMemory == encoding) {
      const auto opcode = (encoding & 0x00400000U) != 0
          ? Opcode::LoadUnsignedImmediate : Opcode::StoreUnsignedImmediate;
      return Operation{opcode, field(bits, 0, 5), field(bits, 5, 5),
                       field(bits, 10, 12),
                       width == 64 ? 3U : (width == 32 ? 2U : (width == 16 ? 1U : 0U)),
                       width};
    }
  }
  const auto scalarIndexedMemory = bits & 0xffe00c00U;
  const auto signedIndexedWidths =
      std::array<std::pair<std::uint32_t, std::uint32_t>, 5>{
          std::pair{0x38c00000U, 8U}, std::pair{0x38800000U, 8U},
          std::pair{0x78c00000U, 16U}, std::pair{0x78800000U, 16U},
          std::pair{0xb8800000U, 32U}};
  for (const auto& [encoding, width] : signedIndexedWidths) {
    for (const auto mode : {0U, 1U, 3U}) {
      if (scalarIndexedMemory == (encoding | (mode << 10U))) {
        const auto target = field(bits, 0, 5);
        const auto base = field(bits, 5, 5);
        if (mode != 0 && base != 31 && target == base) {
          return std::nullopt;
        }
        const auto targetIsX = field(bits, 22, 1) == 0;
        const auto displacement = signed_field(field(bits, 12, 9), 9);
        const auto operationMode = mode == 0 ? 1U : (mode == 1 ? 2U : 3U);
        return Operation{Opcode::LoadSignedScalar, target, base,
                         static_cast<std::uint32_t>(
                             static_cast<std::int32_t>(displacement)),
                         (targetIsX ? 4U : 0U) | operationMode, width};
      }
    }
  }
  const auto indexedMemoryWidths =
      std::array<std::pair<std::uint32_t, std::uint32_t>, 8>{
          std::pair{0x38400000U, 8U}, std::pair{0x38000000U, 8U},
          std::pair{0x78400000U, 16U}, std::pair{0x78000000U, 16U},
          std::pair{0xb8400000U, 32U}, std::pair{0xb8000000U, 32U},
          std::pair{0xf8400000U, 64U}, std::pair{0xf8000000U, 64U}};
  for (const auto& [encoding, width] : indexedMemoryWidths) {
    for (const auto mode : {0U, 1U, 3U}) {
      if (scalarIndexedMemory == (encoding | (mode << 10U))) {
        const auto target = field(bits, 0, 5);
        const auto base = field(bits, 5, 5);
        if (mode != 0 && base != 31 && target == base) {
          return std::nullopt;
        }
        const bool load = (encoding & 0x00400000U) != 0;
        return Operation{load ? Opcode::LoadUnscaledWriteback
                              : Opcode::StoreUnscaledWriteback,
                         target, base,
                         static_cast<std::uint32_t>(
                             static_cast<std::int32_t>(
                                 signed_field(field(bits, 12, 9), 9))),
                         mode, width};
      }
    }
  }
  const auto pairEncoding = (bits & 0xffc00000U) & 0x7fffffffU;
  const bool integerPair =
      (pairEncoding == 0x29000000U || pairEncoding == 0x29400000U ||
       pairEncoding == 0x29800000U || pairEncoding == 0x29c00000U ||
       pairEncoding == 0x28800000U || pairEncoding == 0x28c00000U) &&
      field(bits, 30, 1) == 0;
  if (integerPair) {
    const auto width = field(bits, 31, 1) == 1 ? 64U : 32U;
    const auto elementBytes = width / 8U;
    const auto first = field(bits, 0, 5);
    const auto second = field(bits, 10, 5);
    const auto base = field(bits, 5, 5);
    const auto mode = field(bits, 23, 2);
    const bool load = field(bits, 22, 1) == 1;
    if ((load && first == second) ||
        (base != 31 && mode != 2 && (base == first || base == second))) {
      return std::nullopt;
    }
    const auto displacement = signed_field(field(bits, 15, 7), 7) * elementBytes;
    return Operation{load ? Opcode::LoadPair : Opcode::StorePair, first, base,
                     static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement)),
                     (mode << 5U) | second, width};
  }
  if ((bits & 0x1f000000) == 0x11000000) {
    const auto destination = field(bits, 0, 5);
    const auto source = field(bits, 5, 5);
    const auto setFlags = field(bits, 29, 1) != 0;
    const auto width = field(bits, 31, 1) == 1 ? 64u : 32u;
    const auto shiftField = field(bits, 22, 1);
    if (field(bits, 23, 1) != 0) {
      return std::nullopt;
    }
    const auto subtract = field(bits, 30, 1) != 0;
    const auto opcode = setFlags
        ? (subtract ? Opcode::SubImmediateSetFlags : Opcode::AddImmediateSetFlags)
        : (subtract ? Opcode::SubImmediate : Opcode::AddImmediate);
    return Operation{opcode,
                     destination, source, field(bits, 10, 12),
                     shiftField * 12u, width};
  }
  if ((bits & 0x1f200000U) == 0x0b000000U) {
    const auto width = field(bits, 31, 1) == 1 ? 64U : 32U;
    const auto shiftType = field(bits, 22, 2);
    const auto amount = field(bits, 10, 6);
    if (shiftType == 3U || amount >= width) return std::nullopt;
    const auto subtract = field(bits, 30, 1);
    const auto setFlags = field(bits, 29, 1);
    const auto packed = (setFlags << 9U) | (subtract << 8U) |
                        (shiftType << 6U) | amount;
    return Operation{Opcode::AddSubShiftedRegister, field(bits, 0, 5),
                     field(bits, 5, 5), field(bits, 16, 5), packed, width};
  }
  if ((bits & 0x1fe0fc00U) == 0x1a000000U) {
    const auto width = field(bits, 31, 1) == 1 ? 64U : 32U;
    const auto subtract = field(bits, 30, 1);
    const auto setFlags = field(bits, 29, 1);
    return Operation{Opcode::AddSubCarryRegister, field(bits, 0, 5),
                     field(bits, 5, 5), field(bits, 16, 5),
                     (setFlags << 1U) | subtract, width};
  }
  if ((bits & 0x1f200000U) == 0x0b200000U) {
    const auto width = field(bits, 31, 1) == 1 ? 64U : 32U;
    const auto option = field(bits, 13, 3);
    const auto amount = field(bits, 10, 3);
    if (amount > 4U) return std::nullopt;
    const auto subtract = field(bits, 30, 1);
    const auto setFlags = field(bits, 29, 1);
    const auto packed = (setFlags << 7U) | (subtract << 6U) |
                        (option << 3U) | amount;
    return Operation{Opcode::AddSubExtendedRegister, field(bits, 0, 5),
                     field(bits, 5, 5), field(bits, 16, 5), packed, width};
  }
  if ((bits & 0x1f000000U) == 0x0a000000U) {
    const auto width = field(bits, 31, 1) == 1 ? 64U : 32U;
    const auto shiftType = field(bits, 22, 2);
    const auto amount = field(bits, 10, 6);
    if (amount >= width) return std::nullopt;
    const auto opc = field(bits, 29, 2);
    const auto invert = field(bits, 21, 1);
    const auto packed = (amount << 5U) | (shiftType << 3U) |
                        (invert << 2U) | opc;
    return Operation{Opcode::LogicalShiftedRegister, field(bits, 0, 5),
                     field(bits, 5, 5), field(bits, 16, 5), packed, width};
  }
  if ((bits & 0x7fe0f000U) == 0x1ac02000U) {
    const auto width = field(bits, 31, 1) == 1 ? 64U : 32U;
    const auto shiftType = field(bits, 10, 2);
    return Operation{Opcode::LogicalVariableShiftRegister, field(bits, 0, 5),
                     field(bits, 5, 5), field(bits, 16, 5), shiftType, width};
  }
  if ((bits & 0x7fe00000U) == 0x1b000000U) {
    const auto width = field(bits, 31, 1) == 1 ? 64U : 32U;
    const auto subtract = field(bits, 15, 1);
    const auto accumulator = field(bits, 10, 5);
    return Operation{Opcode::IntegerMultiplyAccumulate, field(bits, 0, 5),
                     field(bits, 5, 5), field(bits, 16, 5),
                     (subtract << 5U) | accumulator, width};
  }
  if ((bits & 0x7ffffc00U) == 0x5ac00000U) {
    const auto width = field(bits, 31, 1) == 1U ? 64U : 32U;
    return Operation{Opcode::IntegerBitOperation, field(bits, 0, 5),
                     field(bits, 5, 5), 0U, 0U, width};
  }
  const auto reverseEncoding = bits & 0xfffffc00U;
  if (reverseEncoding == 0x5ac00800U || reverseEncoding == 0xdac00c00U) {
    const auto width = reverseEncoding == 0xdac00c00U ? 64U : 32U;
    return Operation{Opcode::IntegerBitOperation, field(bits, 0, 5),
                     field(bits, 5, 5), 1U, 0U, width};
  }
  if ((bits & 0x7ffff800U) == 0x5ac01000U) {
    const auto width = field(bits, 31, 1) == 1U ? 64U : 32U;
    const auto variant = field(bits, 10, 1) == 0U ? 2U : 3U;
    return Operation{Opcode::IntegerBitOperation, field(bits, 0, 5),
                     field(bits, 5, 5), variant, 0U, width};
  }
  const auto divideEncoding = bits & 0x7fe0fc00U;
  if (divideEncoding == 0x1ac00800U || divideEncoding == 0x1ac00c00U) {
    const auto width = field(bits, 31, 1) == 1U ? 64U : 32U;
    const auto isSigned = divideEncoding == 0x1ac00c00U ? 1U : 0U;
    return Operation{Opcode::IntegerDivide, field(bits, 0, 5),
                     field(bits, 5, 5), field(bits, 16, 5), isSigned, width};
  }
  const auto bitfieldEncoding = bits & 0x7f800000U;
  if (bitfieldEncoding == 0x53000000U || bitfieldEncoding == 0x33000000U ||
      bitfieldEncoding == 0x13000000U) {
    const auto width = field(bits, 31, 1) == 1U ? 64U : 32U;
    const auto rotation = field(bits, 16, 6);
    const auto end = field(bits, 10, 6);
    if (field(bits, 22, 1) != field(bits, 31, 1) ||
        rotation >= width || end >= width) {
      return std::nullopt;
    }
    const auto variant = bitfieldEncoding == 0x53000000U ? 0U
        : bitfieldEncoding == 0x33000000U ? 1U : 2U;
    return Operation{Opcode::Bitfield, field(bits, 0, 5), field(bits, 5, 5),
                     rotation, (variant << 6U) | end, width};
  }
  if ((bits & 0xbf3ffc00U) == 0x0e209800U) {
    const auto elementWidth = 8U << field(bits, 22, 2);
    const auto vectorWidth = field(bits, 30, 1) != 0U ? 128U : 64U;
    if (elementWidth == 64U && vectorWidth != 128U) return std::nullopt;
    return Operation{Opcode::VectorCompareEqualZero, field(bits, 0, 5),
                     field(bits, 5, 5), elementWidth, 0U, vectorWidth};
  }
  if ((bits & 0x9f3efc00U) == 0x0e30a800U) {
    const auto elementWidth = 8U << field(bits, 22, 2);
    const auto vectorWidth = field(bits, 30, 1) != 0U ? 128U : 64U;
    if (elementWidth >= 64U || (elementWidth == 32U && vectorWidth != 128U)) {
      return std::nullopt;
    }
    const auto minimum = field(bits, 16, 1);
    const auto signedLanes = field(bits, 29, 1) == 0U ? 1U : 0U;
    return Operation{Opcode::VectorReduceExtreme, field(bits, 0, 5),
                     field(bits, 5, 5), elementWidth,
                     (signedLanes << 1U) | minimum, vectorWidth};
  }
  if ((bits & 0x9f3ffc00U) == 0x0e31b800U) {
    const auto elementWidth = 8U << field(bits, 22, 2);
    const auto vectorWidth = field(bits, 30, 1) != 0U ? 128U : 64U;
    if (elementWidth >= 64U ||
        (elementWidth == 32U && vectorWidth != 128U)) {
      return std::nullopt;
    }
    return Operation{Opcode::VectorReduceSum, field(bits, 0, 5),
                     field(bits, 5, 5), elementWidth, 0U, vectorWidth};
  }
  const auto vectorReverseEncoding = bits & 0xbf3ffc00U;
  const auto reverseGroupWidth = vectorReverseEncoding == 0x0e201800U ? 16U
      : vectorReverseEncoding == 0x2e200800U ? 32U
      : vectorReverseEncoding == 0x0e200800U ? 64U : 0U;
  if (reverseGroupWidth != 0U) {
    const auto elementWidth = 8U << field(bits, 22, 2);
    const auto vectorWidth = field(bits, 30, 1) != 0U ? 128U : 64U;
    if (elementWidth >= reverseGroupWidth ||
        reverseGroupWidth % elementWidth != 0U) return std::nullopt;
    return Operation{Opcode::VectorReverse, field(bits, 0, 5),
                     field(bits, 5, 5), elementWidth, reverseGroupWidth,
                     vectorWidth};
  }
  if ((bits & 0xfffffc00U) == 0x1e604000U) {
    return Operation{Opcode::ScalarFloatingPoint, field(bits, 0, 5),
                     field(bits, 5, 5), 0U, 20U, 64U};
  }
  if ((bits & 0x7f3efc00U) == 0x1e220000U) {
    if (field(bits, 22, 2) > 1U) return std::nullopt;
    const auto floatWidth = field(bits, 22, 1) != 0 ? 64U : 32U;
    const auto sourceWidth = field(bits, 31, 1) != 0 ? 64U : 32U;
    const auto signedSource = field(bits, 16, 1) == 0 ? 1U : 0U;
    return Operation{Opcode::ScalarFloatingPoint, field(bits, 0, 5),
                     field(bits, 5, 5), 0,
                     ((sourceWidth == 64U ? 1U : 0U) << 1U) | signedSource,
                     floatWidth};
  }
  if ((bits & 0xff201fe0U) == 0x1e201000U) {
    const auto format = field(bits, 22, 2);
    if (format > 1U) return std::nullopt;
    const auto floatWidth = format == 1U ? 64U : 32U;
    return Operation{Opcode::ScalarFloatingPoint, field(bits, 0, 5), 0,
                     field(bits, 13, 8), 4U, floatWidth};
  }
  if ((bits & 0xff3ffc00U) == 0x1e24c000U) {
    const auto format = field(bits, 22, 2);
    if (format > 1U) return std::nullopt;
    const auto floatWidth = format == 1U ? 64U : 32U;
    return Operation{Opcode::ScalarFloatingPoint, field(bits, 0, 5),
                     field(bits, 5, 5), 0, 8U, floatWidth};
  }
  if ((bits & 0xff3ffc00U) == 0x1e214000U) {
    const auto format = field(bits, 22, 2);
    if (format > 1U) return std::nullopt;
    return Operation{Opcode::ScalarFloatingPoint, field(bits, 0, 5),
                     field(bits, 5, 5), 0, 12U,
                     format == 1U ? 64U : 32U};
  }
  if ((bits & 0xff20fc00U) == 0x1e202000U && (bits & 7U) == 0U) {
    const auto format = field(bits, 22, 2);
    if (format > 1U) return std::nullopt;
    const bool compareZero = field(bits, 3, 1) != 0U;
    const auto signalAllNans = field(bits, 4, 1);
    return Operation{Opcode::ScalarFloatingPoint, 0U, field(bits, 5, 5),
                     compareZero ? 0U : field(bits, 16, 5),
                     16U | signalAllNans | (compareZero ? 2U : 0U),
                     format == 1U ? 64U : 32U};
  }
  const auto moveWide = bits & 0x7f800000;
  Opcode opcode;
  if (moveWide == 0x52800000) {
    opcode = Opcode::MoveWideZero;
  } else if (moveWide == 0x12800000) {
    opcode = Opcode::MoveWideNot;
  } else if (moveWide == 0x72800000) {
    opcode = Opcode::MoveWideKeep;
  } else {
    return std::nullopt;
  }

  const auto width = field(bits, 31, 1) == 1 ? 64u : 32u;
  const auto shift = field(bits, 21, 2) * 16u;
  if (shift >= width) {
    throw std::runtime_error("Invalid MOVZ shift for destination width");
  }
  return Operation{opcode, field(bits, 0, 5), 0,
                   field(bits, 5, 16), shift, width};
}

std::optional<Operation> decode_vector_memory_operation(std::uint32_t bits) {
  using Mode = VectorMemoryMode;
  const auto destination = field(bits, 0, 5);
  const auto base = field(bits, 5, 5);
  const bool load = field(bits, 22, 1) != 0U;
  const auto opcode = load ? Opcode::LoadVectorMemory
                           : Opcode::StoreVectorMemory;
  const auto make = [destination, base, opcode](std::uint32_t bytes,
                                                std::uint32_t displacement,
                                                std::uint32_t packedMode) {
    return Operation{opcode, destination, base, displacement, packedMode,
                     bytes * 8U};
  };

  // SIMD literal LDR (S, D, or Q); PRFM and reserved 256-bit forms stay out.
  if ((bits & 0x3f000000U) == 0x1c000000U) {
    const auto bytes = 4U << field(bits, 30, 2);
    if (bytes > 16U) return std::nullopt;
    const auto displacement = signed_field(field(bits, 5, 19), 19) * 4;
    return Operation{Opcode::LoadVectorMemory, destination, 0,
                     static_cast<std::uint32_t>(
                         static_cast<std::int32_t>(displacement)),
                     static_cast<std::uint32_t>(Mode::Literal), bytes * 8U};
  }

  const auto vectorMemory = bits & 0xff800000U;
  const auto scaledBytes = vectorMemory == 0x3d800000U ? 16U
      : vectorMemory == 0xfd000000U ? 8U
      : vectorMemory == 0xbd000000U ? 4U : 0U;
  if (scaledBytes != 0U) {
    const auto displacement = field(bits, 10, 12) * scaledBytes;
    return make(scaledBytes, displacement,
                static_cast<std::uint32_t>(Mode::ScaledImmediate));
  }

  const auto addressingMode = field(bits, 10, 2);
  const auto unscaledEncoding = bits & 0xffa00000U;
  const auto unscaledBytes = unscaledEncoding == 0x3c800000U ? 16U
      : unscaledEncoding == 0xfc000000U ? 8U
      : unscaledEncoding == 0xbc000000U ? 4U : 0U;
  if (unscaledBytes != 0U && addressingMode != 2U) {
    const auto displacement = signed_field(field(bits, 12, 9), 9);
    const auto mode = addressingMode == 0U ? Mode::Unscaled
        : addressingMode == 1U ? Mode::PostIndex : Mode::PreIndex;
    return make(unscaledBytes,
                static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(displacement)),
                static_cast<std::uint32_t>(mode));
  }

  const auto extension = field(bits, 13, 3);
  if (extension != 2U && extension != 3U && extension != 6U &&
      extension != 7U) {
    return std::nullopt;
  }
  const auto indexedEncoding = bits & 0xffa00c00U;
  const auto indexedBytes = indexedEncoding == 0x3ca00800U ? 16U
      : indexedEncoding == 0xfc200800U ? 8U
      : indexedEncoding == 0xbc200800U ? 4U
      : indexedEncoding == 0x7c200800U ? 2U
      : indexedEncoding == 0x3c200800U ? 1U : 0U;
  if (indexedBytes == 0U) return std::nullopt;
  const auto packedMode = static_cast<std::uint32_t>(Mode::RegisterOffset) |
      (field(bits, 16, 5) << 3U) | (extension << 8U) |
      (field(bits, 12, 1) << 11U);
  return make(indexedBytes, 0U, packedMode);
}

std::optional<Operation> decode_authenticated_return_operation(
    std::uint32_t bits) {
  using Step = AuthenticatedReturnStep;
  if (bits == 0xd503237f) {
    return Operation{Opcode::AuthenticatedReturn,
                     static_cast<std::uint32_t>(Step::SignLinkRegister),
                     0, 0, 0, 0};
  }
  if (bits == 0xd50323ff) {
    return Operation{Opcode::AuthenticatedReturn,
                     static_cast<std::uint32_t>(Step::AuthenticateLinkRegister),
                     0, 0, 0, 0};
  }
  if (bits == 0xd65f0fff) {
    return Operation{Opcode::AuthenticatedReturn,
                     static_cast<std::uint32_t>(Step::Return), 0, 0, 0, 0};
  }
  return std::nullopt;
}

Instruction make_operation_instruction(Operation operation,
                                       std::vector<std::int64_t> next,
                                       LoweringMode mode,
                                       std::uint64_t guest_code_limit,
                                       std::uint64_t guest_pc) {
  std::vector<std::string> code;
  if (operation.opcode == Opcode::SystemFlags || operation.opcode == Opcode::FlagManipulation) {
    std::string expression;
    if (operation.opcode == Opcode::SystemFlags) {
      if (operation.destination || operation.source || operation.immediate > 2 ||
          operation.shift || operation.width || next.size() != 1) {
        throw std::invalid_argument("Invalid typed system flag operation");
      }
      expression = "system_flags_apply(" + std::to_string(operation.immediate) + "u, prior)";
    } else {
      if (operation.destination || next.size() != 1 ||
          !valid_flag_manipulation(operation.source, operation.immediate,
                                   operation.shift, operation.width)) {
        throw std::invalid_argument("Invalid typed RMIF/SETF operation");
      }
      expression = "flag_manipulation_apply(" + reg(operation.source, true) + ", " +
          std::to_string(operation.immediate) + "u, " + std::to_string(operation.shift) +
          "u, " + std::to_string(operation.width) + "u, prior)";
    }
    code = {"{",
            "const uint prior = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3);",
            "const uint flags = " + expression + ";",
            "z = (flags & 1u) != 0u; n = (flags & 2u) != 0u;",
            "v = (flags & 4u) != 0u; c = (flags & 8u) != 0u;"};
    if (mode == LoweringMode::Process) code.push_back("x[32] = flags;");
    code.push_back("}");
  } else if (operation.opcode == Opcode::AuthenticatedReturn) {
    if (mode != LoweringMode::Process || operation.destination > 2 ||
        operation.source != 0 || operation.immediate != 0 ||
        operation.shift != 0 || operation.width != 0) {
      throw std::invalid_argument("Invalid typed authenticated-return operation");
    }
    const auto step = static_cast<AuthenticatedReturnStep>(operation.destination);
    if (step == AuthenticatedReturnStep::SignLinkRegister) {
      if (next.size() != 1) {
        throw std::invalid_argument("Typed PACIBSP needs a fallthrough PC");
      }
      code = {
          "if (x[37] >= 256ul) { x[34] = 6; return; }",
          "{ ulong slot = 38ul + 3ul * x[37]++;",
          "x[slot] = x[30]; x[slot + 1] = x[31]; x[slot + 2] = " +
              std::to_string(guest_pc) + "ul; }"};
    } else if (step == AuthenticatedReturnStep::AuthenticateLinkRegister) {
      if (next.size() != 1) {
        throw std::invalid_argument("Typed AUTIBSP needs a fallthrough PC");
      }
      code = {
          "if (x[37] == 0) { x[34] = 6; return; }",
          "{ ulong slot = 38ul + 3ul * (x[37] - 1ul);",
          "if (x[30] != x[slot] || x[31] != x[slot + 1]) { x[slot + 2] = ~0ul; }",
          "else { --x[37]; } }"};
    } else {
      if (!next.empty() || guest_code_limit == 0) {
        throw std::invalid_argument("Typed RETAB cannot have successors");
      }
      code = {
          "if (x[37] == 0) { x[34] = 6; return; }",
          "{ ulong slot = 38ul + 3ul * --x[37];",
          "if (x[slot + 2] >= 0xffffffff00000000ul) { x[33] = " +
              std::to_string(guest_pc) + "ul; x[34] = 16; return; }",
          "if (x[30] != x[slot] || x[31] != x[slot + 1]) { x[33] = " +
              std::to_string(guest_pc) + "ul; x[34] = 16; return; } }",
          "for (ulong auth_index = x[37]; auth_index > 0ul; --auth_index) { "
          "ulong auth_slot = 38ul + 3ul * (auth_index - 1ul); "
          "if (x[auth_slot + 2] >= 0xffffffff00000000ul) { "
          "if (x[auth_slot + 2] != ~0ul) { ++x[auth_slot + 2]; } break; } }",
          "if (x[30] == " + std::to_string(guest_code_limit) +
              "ul) { x[34] = 0; return; }",
          "if (x[30] >= " + std::to_string(guest_code_limit) +
              "ul) { x[34] = 3; return; }",
          "x[33] = x[30];",
          "x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3);",
          "x[34] = 7; return;"};
    }
  } else if (operation.opcode == Opcode::VectorCompareEqualZero) {
    if (operation.destination > 31 || operation.source > 31 ||
        (operation.immediate != 8U && operation.immediate != 16U &&
         operation.immediate != 32U && operation.immediate != 64U) ||
        operation.shift != 0U ||
        (operation.width != 64U && operation.width != 128U) ||
        (operation.immediate == 64U && operation.width != 128U) ||
        next.size() != 1U) {
      throw std::invalid_argument("Typed vector CMEQ operands are invalid");
    }
    const auto destination = 806U + 2U * operation.destination;
    const auto source = 806U + 2U * operation.source;
    code.push_back("{ ulong2 value = vector_compare_equal_zero_apply(ulong2(x[" +
                   std::to_string(source) + "], x[" +
                   std::to_string(source + 1U) + "]), " +
                   std::to_string(operation.immediate) + "u, " +
                   std::to_string(operation.width) +
                   "u); x[" + std::to_string(destination) +
                   "] = value[0]; x[" + std::to_string(destination + 1U) +
                   "] = value[1]; }");
  } else if (operation.opcode == Opcode::VectorReduceSum) {
    if (operation.destination > 31 || operation.source > 31 ||
        (operation.immediate != 8U && operation.immediate != 16U &&
         operation.immediate != 32U) || operation.shift != 0U ||
        (operation.width != 64U && operation.width != 128U) ||
        (operation.immediate == 32U && operation.width != 128U) ||
        next.size() != 1U) {
      throw std::invalid_argument("Typed vector sum reduction operands are invalid");
    }
    const auto destination = 806U + 2U * operation.destination;
    const auto source = 806U + 2U * operation.source;
    code.push_back("{ ulong2 value = vector_reduce_sum_apply(ulong2(x[" +
                   std::to_string(source) + "], x[" +
                   std::to_string(source + 1U) + "]), " +
                   std::to_string(operation.immediate) + "u, " +
                   std::to_string(operation.width) +
                   "u); x[" + std::to_string(destination) +
                   "] = value[0]; x[" + std::to_string(destination + 1U) +
                   "] = value[1]; }");
  } else if (operation.opcode == Opcode::VectorReduceExtreme) {
    const auto minimum = operation.shift & 1U;
    const auto signedLanes = (operation.shift >> 1U) & 1U;
    if (operation.destination > 31 || operation.source > 31 ||
        (operation.immediate != 8U && operation.immediate != 16U &&
         operation.immediate != 32U) || operation.shift > 3U ||
        (operation.width != 64U && operation.width != 128U) ||
        (operation.immediate == 32U && operation.width != 128U) ||
        next.size() != 1U) {
      throw std::invalid_argument("Typed vector min/max reduction operands are invalid");
    }
    const auto destination = 806U + 2U * operation.destination;
    const auto source = 806U + 2U * operation.source;
    code.push_back("{ ulong2 value = vector_reduce_extreme_apply(ulong2(x[" +
                   std::to_string(source) + "], x[" +
                   std::to_string(source + 1U) + "]), " +
                   std::to_string(operation.immediate) + "u, " +
                   std::to_string(operation.width) + "u, " +
                   std::to_string(minimum) + "u, " +
                   std::to_string(signedLanes) + "u); x[" +
                   std::to_string(destination) + "] = value[0]; x[" +
                   std::to_string(destination + 1U) + "] = value[1]; }");
  } else if (operation.opcode == Opcode::VectorReverse) {
    if (operation.destination > 31 || operation.source > 31 ||
        (operation.immediate != 8U && operation.immediate != 16U &&
         operation.immediate != 32U) ||
        (operation.shift != 16U && operation.shift != 32U &&
         operation.shift != 64U) || operation.immediate >= operation.shift ||
        operation.shift % operation.immediate != 0U ||
        (operation.width != 64U && operation.width != 128U) ||
        next.size() != 1U) {
      throw std::invalid_argument("Typed vector reverse operands are invalid");
    }
    const auto destination = 806U + 2U * operation.destination;
    const auto source = 806U + 2U * operation.source;
    code.push_back("{ ulong2 value = vector_reverse_apply(ulong2(x[" +
                   std::to_string(source) + "], x[" +
                   std::to_string(source + 1U) + "]), " +
                   std::to_string(operation.immediate) + "u, " +
                   std::to_string(operation.width) + "u, " +
                   std::to_string(operation.shift) + "u); x[" +
                   std::to_string(destination) + "] = value[0]; x[" +
                   std::to_string(destination + 1U) + "] = value[1]; }");
  } else if (operation.opcode == Opcode::LoadLiteral) {
    const auto displacement = static_cast<std::int32_t>(operation.immediate);
    const auto bytes = operation.shift == 1U ? 8U : 4U;
    if (operation.destination > 31 || operation.source != 0 || operation.shift > 2U ||
        (operation.shift == 0U && operation.width != 32U) ||
        (operation.shift != 0U && operation.width != 64U) ||
        displacement < -(1 << 20) || displacement > ((1 << 20) - 4) ||
        (displacement & 3) != 0 ||
        guest_pc > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw std::invalid_argument("Typed scalar literal-load operands are invalid");
    }
    const auto base = static_cast<std::int64_t>(guest_pc);
    if ((displacement < 0 && base < -static_cast<std::int64_t>(displacement)) ||
        (displacement > 0 && base >
            std::numeric_limits<std::int64_t>::max() - displacement)) {
      throw std::invalid_argument("Typed scalar literal-load address is invalid");
    }
    const auto address = base + displacement;
    if (address < 0 || (guest_code_limit != 0 &&
        (static_cast<std::uint64_t>(address) >= guest_code_limit ||
         bytes > guest_code_limit - static_cast<std::uint64_t>(address)))) {
      throw std::invalid_argument("Typed scalar literal-load address is invalid");
    }
    const auto literalAddress = std::to_string(address) + "ul";
    const auto load = "load_guest_word(memory + " + literalAddress + ", " +
                      std::to_string(bytes) + "ul)";
    code.push_back("if (" + literalAddress + " > size || " +
                   std::to_string(bytes) + "ul > size - " + literalAddress +
                   ") { x[33] = " + std::to_string(guest_pc) +
                   "ul; x[874] = " + literalAddress +
                   "; x[34] = 2; return; }");
    if (operation.destination != 31) {
      const auto value = operation.shift == 2U
          ? "ulong(long(int(" + load + ")))" : load;
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = " + value + ";");
    }
  } else if (operation.opcode == Opcode::IntegerBitOperation) {
    if (operation.destination > 31 || operation.source > 31 ||
        operation.immediate > 3U || operation.shift != 0 ||
        (operation.width != 32 && operation.width != 64)) {
      throw std::invalid_argument("Typed integer bit-operation operands are invalid");
    }
    if (operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = integer_bit_operation_apply(" +
                     std::to_string(operation.immediate) + "u, " +
                     reg(operation.source, operation.width == 64) + ", " +
                     std::to_string(operation.width) + "u);");
    }
  } else if (operation.opcode == Opcode::IntegerDivide) {
    if (operation.destination > 31 || operation.source > 31 ||
        operation.immediate > 31U || operation.shift > 1U ||
        (operation.width != 32 && operation.width != 64)) {
      throw std::invalid_argument("Typed integer divide operands are invalid");
    }
    if (operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = integer_divide_apply(" +
                     std::to_string(operation.shift) + "u, " +
                     reg(operation.source, operation.width == 64) + ", " +
                     reg(operation.immediate, operation.width == 64) + ", " +
                     std::to_string(operation.width) + "u);");
    }
  } else if (operation.opcode == Opcode::Bitfield) {
    const auto variant = operation.shift >> 6U;
    const auto end = operation.shift & 63U;
    if (operation.destination > 31 || operation.source > 31 ||
        (operation.width != 32 && operation.width != 64) ||
        operation.immediate >= operation.width || end >= operation.width ||
        variant > 2U || (operation.shift & ~0xffU) != 0) {
      throw std::invalid_argument("Typed bitfield operands are invalid");
    }
    if (operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = bitfield_apply(" + std::to_string(variant) +
                     "u, " + reg(operation.destination, operation.width == 64) +
                     ", " + reg(operation.source, operation.width == 64) +
                     ", " + std::to_string(operation.immediate) + "u, " +
                     std::to_string(end) + "u, " +
                     std::to_string(operation.width) + "u);");
    }
  } else if (operation.opcode == Opcode::Adr) {
    if (operation.destination > 31 || operation.source > 1 ||
        operation.immediate > 0x1fffffU || operation.shift != 0 ||
        operation.width != 64) {
      throw std::invalid_argument("Typed ADR/ADRP operands are invalid");
    }
    if (operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = adr_apply(" + std::to_string(operation.source) +
                     "u, " + std::to_string(guest_pc) +
                     "ul, " + std::to_string(operation.immediate) + "u);");
    }
  } else if (operation.opcode == Opcode::MoveWideZero ||
      operation.opcode == Opcode::MoveWideNot ||
      operation.opcode == Opcode::MoveWideKeep) {
    if (operation.destination != 31) {
      const auto destination = "x[" + std::to_string(operation.destination) + "]";
      code.push_back(destination + " = move_wide_apply(" +
                     std::to_string(static_cast<std::uint32_t>(operation.opcode)) +
                     "u, " + destination + ", " +
                     std::to_string(operation.immediate) + "u, " +
                     std::to_string(operation.shift) + "u, " +
                     std::to_string(operation.width) + "u);");
    }
  } else if (operation.opcode == Opcode::AddImmediate ||
             operation.opcode == Opcode::SubImmediate ||
             operation.opcode == Opcode::AddImmediateSetFlags ||
             operation.opcode == Opcode::SubImmediateSetFlags) {
    const bool setFlags = operation.opcode == Opcode::AddImmediateSetFlags ||
                          operation.opcode == Opcode::SubImmediateSetFlags;
    const auto subtract = operation.opcode == Opcode::SubImmediate ||
                          operation.opcode == Opcode::SubImmediateSetFlags;
    const auto lhs = "x[" + std::to_string(operation.source) + "]";
    code.push_back("{");
    code.push_back("const uint previous_flags = uint(z) | (uint(n) << 1u) | "
                   "(uint(v) << 2u) | (uint(c) << 3u);");
    code.push_back("const AddSubImmediateResult add_sub_result = "
                   "add_sub_immediate_apply(" + std::string(subtract ? "1u" : "0u") +
                   ", " + lhs + ", " + std::to_string(operation.immediate) +
                   "u, " + std::to_string(operation.shift) + "u, " +
                   std::to_string(operation.width) + "u, " +
                   std::string(setFlags ? "1u" : "0u") + ", previous_flags);");
    if (!setFlags || operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = add_sub_result.value;");
    }
    code.push_back("z = (add_sub_result.flags & 1u) != 0u;");
    code.push_back("n = (add_sub_result.flags & 2u) != 0u;");
    code.push_back("v = (add_sub_result.flags & 4u) != 0u;");
    code.push_back("c = (add_sub_result.flags & 8u) != 0u;");
    if (mode == LoweringMode::Process) {
      code.push_back("x[32] = add_sub_result.flags;");
    }
    code.push_back("}");
  } else if (operation.opcode == Opcode::AddSubShiftedRegister) {
    const bool subtract = (operation.shift & 0x100U) != 0;
    const bool setFlags = (operation.shift & 0x200U) != 0;
    const auto shiftType = (operation.shift >> 6U) & 3U;
    const auto amount = operation.shift & 63U;
    if (operation.destination > 31 || operation.source > 31 ||
        operation.immediate > 31 || shiftType > 2 || amount >= operation.width ||
        (operation.width != 32 && operation.width != 64) ||
        (operation.shift & ~0x3ffU) != 0) {
      throw std::invalid_argument("Typed ADD/SUB shifted-register operands are invalid");
    }
    const auto flags = "(uint(z) | (uint(n) << 1u) | (uint(v) << 2u) | "
                       "(uint(c) << 3u))";
    code.push_back("{");
    code.push_back("const AddSubImmediateResult add_sub_result = "
                   "add_sub_shifted_apply(" + std::string(subtract ? "1u" : "0u") +
                   ", " + reg(operation.source, operation.width == 64) + ", " +
                   reg(operation.immediate, operation.width == 64) + ", " +
                   std::to_string(shiftType) + "u, " + std::to_string(amount) +
                   "u, " + std::to_string(operation.width) + "u, " +
                   std::string(setFlags ? "1u" : "0u") + ", " + flags + ");");
    if (operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = add_sub_result.value;");
    }
    code.push_back("z = (add_sub_result.flags & 1u) != 0u;");
    code.push_back("n = (add_sub_result.flags & 2u) != 0u;");
    code.push_back("v = (add_sub_result.flags & 4u) != 0u;");
    code.push_back("c = (add_sub_result.flags & 8u) != 0u;");
    if (mode == LoweringMode::Process) {
      code.push_back("x[32] = add_sub_result.flags;");
    }
    code.push_back("}");
  } else if (operation.opcode == Opcode::AddSubExtendedRegister) {
    const bool subtract = (operation.shift & 0x40U) != 0;
    const bool setFlags = (operation.shift & 0x80U) != 0;
    const auto option = (operation.shift >> 3U) & 7U;
    const auto amount = operation.shift & 7U;
    if (operation.destination > 31 || operation.source > 31 ||
        operation.immediate > 31 || amount > 4 ||
        (operation.width != 32 && operation.width != 64) ||
        (operation.shift & ~0xffU) != 0) {
      throw std::invalid_argument("Typed ADD/SUB extended-register operands are invalid");
    }
    const auto flags = "(uint(z) | (uint(n) << 1u) | (uint(v) << 2u) | "
                       "(uint(c) << 3u))";
    code.push_back("{");
    code.push_back("const AddSubImmediateResult add_sub_result = "
                   "add_sub_extended_apply(" + std::string(subtract ? "1u" : "0u") +
                   ", x[" + std::to_string(operation.source) + "], " +
                   reg(operation.immediate, operation.width == 64) + ", " +
                   std::to_string(option) + "u, " + std::to_string(amount) +
                   "u, " + std::to_string(operation.width) + "u, " +
                   std::string(setFlags ? "1u" : "0u") + ", " + flags + ");");
    if (!setFlags || operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = add_sub_result.value;");
    }
    code.push_back("z = (add_sub_result.flags & 1u) != 0u;");
    code.push_back("n = (add_sub_result.flags & 2u) != 0u;");
    code.push_back("v = (add_sub_result.flags & 4u) != 0u;");
    code.push_back("c = (add_sub_result.flags & 8u) != 0u;");
    if (mode == LoweringMode::Process) {
      code.push_back("x[32] = add_sub_result.flags;");
    }
    code.push_back("}");
  } else if (operation.opcode == Opcode::AddSubCarryRegister) {
    const bool subtract = (operation.shift & 1U) != 0;
    const bool setFlags = (operation.shift & 2U) != 0;
    if (operation.destination > 31 || operation.source > 31 ||
        operation.immediate > 31 || (operation.width != 32 && operation.width != 64) ||
        (operation.shift & ~3U) != 0) {
      throw std::invalid_argument("Typed ADC/SBC register operands are invalid");
    }
    const auto flags = "(uint(z) | (uint(n) << 1u) | (uint(v) << 2u) | "
                       "(uint(c) << 3u))";
    code.push_back("{");
    code.push_back("const AddSubImmediateResult add_sub_result = "
                   "add_sub_carry_apply(" + std::string(subtract ? "1u" : "0u") +
                   ", " + reg(operation.source, operation.width == 64) + ", " +
                   reg(operation.immediate, operation.width == 64) + ", " +
                   std::to_string(operation.width) + "u, " +
                   std::string(setFlags ? "1u" : "0u") + ", " + flags + ");");
    if (operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = add_sub_result.value;");
    }
    code.push_back("z = (add_sub_result.flags & 1u) != 0u;");
    code.push_back("n = (add_sub_result.flags & 2u) != 0u;");
    code.push_back("v = (add_sub_result.flags & 4u) != 0u;");
    code.push_back("c = (add_sub_result.flags & 8u) != 0u;");
    if (mode == LoweringMode::Process) {
      code.push_back("x[32] = add_sub_result.flags;");
    }
    code.push_back("}");
  } else if (operation.opcode == Opcode::LogicalImmediate) {
    const auto opc = operation.source >> 5U;
    const auto rn = operation.source & 31U;
    const auto mask = (static_cast<std::uint64_t>(operation.shift) << 32U) |
                      operation.immediate;
    if (operation.destination > 31 || operation.source > 127 || opc > 3U ||
        (operation.width != 32 && operation.width != 64) ||
        (operation.width == 32 && operation.shift != 0)) {
      throw std::invalid_argument("Typed logical immediate operands are invalid");
    }
    const bool setFlags = opc == 3U;
    const auto flags = "(uint(z) | (uint(n) << 1u) | (uint(v) << 2u) | "
                       "(uint(c) << 3u))";
    code.push_back("{");
    code.push_back("const LogicalShiftedResult logical_result = "
                   "logical_immediate_apply(" + std::to_string(opc) + "u, " +
                   reg(rn, operation.width == 64) + ", " +
                   std::to_string(mask) + "ul, " +
                   std::to_string(operation.width) + "u, " +
                   std::string(setFlags ? "1u" : "0u") + ", " + flags + ");");
    if (operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = logical_result.value;");
    }
    if (setFlags) {
      code.push_back("z = (logical_result.flags & 1u) != 0u;");
      code.push_back("n = (logical_result.flags & 2u) != 0u;");
      code.push_back("v = (logical_result.flags & 4u) != 0u;");
      code.push_back("c = (logical_result.flags & 8u) != 0u;");
      if (mode == LoweringMode::Process) {
        code.push_back("x[32] = logical_result.flags;");
      }
    }
    code.push_back("}");
  } else if (operation.opcode == Opcode::LogicalShiftedRegister) {
    const auto opc = operation.shift & 3U;
    const auto invert = (operation.shift >> 2U) & 1U;
    const auto shiftType = (operation.shift >> 3U) & 3U;
    const auto amount = (operation.shift >> 5U) & 63U;
    if (operation.destination > 31 || operation.source > 31 ||
        operation.immediate > 31 || opc > 3 || shiftType > 3 ||
        amount >= operation.width ||
        (operation.width != 32 && operation.width != 64) ||
        (operation.shift & ~0x7ffU) != 0) {
      throw std::invalid_argument("Typed logical shifted-register operands are invalid");
    }
    const bool setFlags = opc == 3U;
    const auto flags = "(uint(z) | (uint(n) << 1u) | (uint(v) << 2u) | "
                       "(uint(c) << 3u))";
    code.push_back("{");
    code.push_back("const LogicalShiftedResult logical_result = "
                   "logical_shifted_apply(" + std::to_string(opc) + "u, " +
                   std::to_string(invert) + "u, " +
                   reg(operation.source, operation.width == 64) + ", " +
                   reg(operation.immediate, operation.width == 64) + ", " +
                   std::to_string(shiftType) + "u, " +
                   std::to_string(amount) + "u, " +
                   std::to_string(operation.width) + "u, " +
                   std::string(setFlags ? "1u" : "0u") + ", " + flags + ");");
    if (!setFlags || operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = logical_result.value;");
    }
    if (setFlags) {
      code.push_back("z = (logical_result.flags & 1u) != 0u;");
      code.push_back("n = (logical_result.flags & 2u) != 0u;");
      code.push_back("v = (logical_result.flags & 4u) != 0u;");
      code.push_back("c = (logical_result.flags & 8u) != 0u;");
      if (mode == LoweringMode::Process) {
        code.push_back("x[32] = logical_result.flags;");
      }
    }
    code.push_back("}");
  } else if (operation.opcode == Opcode::LogicalVariableShiftRegister) {
    if (operation.destination > 31 || operation.source > 31 ||
        operation.immediate > 31 || operation.shift > 3 ||
        (operation.width != 32 && operation.width != 64)) {
      throw std::invalid_argument("Typed logical variable-shift operands are invalid");
    }
    const auto flags = "(uint(z) | (uint(n) << 1u) | (uint(v) << 2u) | "
                       "(uint(c) << 3u))";
    code.push_back("{");
    code.push_back("const LogicalShiftedResult logical_result = "
                   "logical_variable_shift_apply(" +
                   std::to_string(operation.shift) + "u, " +
                   reg(operation.source, operation.width == 64) + ", " +
                   reg(operation.immediate, operation.width == 64) + ", " +
                   std::to_string(operation.width) + "u, " + flags + ");");
    if (operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = logical_result.value;");
    }
    code.push_back("}");
  } else if (operation.opcode == Opcode::IntegerMultiplyAccumulate) {
    if (operation.destination > 31 || operation.source > 31 ||
        operation.immediate > 31 || operation.shift > 63 ||
        (operation.width != 32 && operation.width != 64)) {
      throw std::invalid_argument("Typed multiply-accumulate operands are invalid");
    }
    const auto subtract = (operation.shift >> 5U) & 1U;
    const auto accumulator = operation.shift & 31U;
    if (operation.destination != 31) {
      code.push_back("x[" + std::to_string(operation.destination) + "] = " +
                     "multiply_accumulate_apply(" + std::to_string(subtract) +
                     "u, " + reg(operation.source, operation.width == 64) +
                     ", " + reg(operation.immediate, operation.width == 64) +
                     ", " + reg(accumulator, operation.width == 64) + ", " +
                     std::to_string(operation.width) + "u);");
    }
  } else if (operation.opcode == Opcode::ScalarFloatingPoint) {
    const auto kind = operation.shift >> 2U;
    const auto detail = operation.shift & 3U;
    const auto maxImmediate = kind == 1U ? 255U : kind == 4U ? 31U : 0U;
    const auto scalarRegister = [](std::uint32_t index) {
      return std::to_string(806U + 2U * index);
    };
    if (operation.destination > 31 || operation.source > 31 ||
        operation.immediate > maxImmediate ||
        (operation.width != 32 && operation.width != 64) ||
        (kind == 0U && operation.shift > 3U) ||
        (kind == 1U && operation.shift != 4U) ||
        (kind == 2U && operation.shift != 8U) ||
        (kind == 3U && operation.shift != 12U) ||
        (kind == 4U && operation.shift < 16U) ||
        (kind == 5U && (operation.shift != 20U || operation.width != 64U)) ||
        kind > 5U ||
        ((kind != 1U && kind != 4U) && operation.immediate != 0U) ||
        (kind == 4U && (detail & 2U) != 0U && operation.immediate != 0U)) {
      throw std::invalid_argument("Typed scalar FP operands are invalid");
    }
    const auto destination = scalarRegister(operation.destination);
    std::string result;
    if (kind == 0U) {
      const auto sourceWidth = (operation.shift & 2U) != 0 ? 64U : 32U;
      const auto signedSource = operation.shift & 1U;
      result = "scalar_fp_integer_to_float(" +
          reg(operation.source, sourceWidth == 64U) + ", " +
          std::to_string(sourceWidth) + "u, " +
          std::to_string(signedSource) + "u, " +
          std::to_string(operation.width) + "u, uint(x[870]))";
      code.push_back("{ const ScalarFPResult fp_result = " + result + "; " +
                     "x[" + destination + "] = fp_result.value; x[" +
                     std::to_string(807U + 2U * operation.destination) +
                     "] = 0ul; x[871] |= fp_result.fpsrFlags; }");
    } else if (kind == 1U) {
      code.push_back("{ x[" + destination + "] = scalar_fp_expand_immediate(" +
                     std::to_string(operation.immediate) + "u, " +
                     std::to_string(operation.width) + "u); x[" +
                     std::to_string(807U + 2U * operation.destination) +
                     "] = 0ul; }");
    } else if (kind == 2U) {
      result = "scalar_fp_round_positive(x[" +
          scalarRegister(operation.source) + "], " +
          std::to_string(operation.width) + "u, uint(x[870]))";
      code.push_back("{ const ScalarFPResult fp_result = " + result + "; " +
                     "x[" + destination + "] = fp_result.value; x[" +
                     std::to_string(807U + 2U * operation.destination) +
                     "] = 0ul; x[871] |= fp_result.fpsrFlags; }");
    } else if (kind == 3U) {
      code.push_back("{ const ScalarFPResult fp_result = scalar_fp_negate(x[" +
                     scalarRegister(operation.source) + "], " +
                     std::to_string(operation.width) + "u); x[" + destination +
                     "] = fp_result.value; x[" +
                     std::to_string(807U + 2U * operation.destination) +
                     "] = 0ul; x[871] |= fp_result.fpsrFlags; }");
    } else if (kind == 5U) {
      code.push_back("x[" + destination + "] = x[" +
                     scalarRegister(operation.source) + "]; x[" +
                     std::to_string(807U + 2U * operation.destination) +
                     "] = 0ul;");
    } else {
      const auto right = (detail & 2U) != 0U
          ? std::string("0ul")
          : "x[" + scalarRegister(operation.immediate) + "]";
      code.push_back("{ const ScalarFPResult fp_result = scalar_fp_compare(x[" +
                     scalarRegister(operation.source) + "], " + right + ", " +
                     std::to_string(operation.width) + "u, " +
                     std::to_string(detail & 1U) + "u, uint(x[870])); " +
                     "z = (fp_result.value & 1u) != 0u; " +
                     "n = (fp_result.value & 2u) != 0u; " +
                     "v = (fp_result.value & 4u) != 0u; " +
                     "c = (fp_result.value & 8u) != 0u; " +
                     (mode == LoweringMode::Process
                          ? "x[32] = (x[32] & ~0xful) | fp_result.value; "
                          : "") +
                     "x[871] |= fp_result.fpsrFlags; }");
    }
  } else if (operation.opcode == Opcode::Return) {
    if (mode == LoweringMode::Function) {
      code = {"returns[id] = uint(x[0]); status[id] = 0; return;"};
    } else {
      if (guest_code_limit == 0) {
        throw std::invalid_argument("Process return lowering needs a code limit");
      }
      code = {
          "for (ulong auth_index = x[37]; auth_index > 0ul; --auth_index) { "
          "ulong auth_slot = 38ul + 3ul * (auth_index - 1ul); "
          "if (x[auth_slot + 2] >= 0xffffffff00000000ul) { "
          "if (x[auth_slot + 2] != ~0ul) { ++x[auth_slot + 2]; } else { "
          "if (auth_index != x[37]) { x[34] = 6; return; } --x[37]; "
          "x[33] = " + std::to_string(guest_pc) + "ul; x[34] = 16; return; } break; } }",
          "if (x[30] == " + std::to_string(guest_code_limit) +
              "ul) { x[34] = 0; return; }",
          "if (x[30] >= " + std::to_string(guest_code_limit) +
              "ul) { x[34] = 3; return; }",
          "x[33] = x[30];",
          "x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3);",
          "x[34] = 7; return;"};
    }
  } else if (operation.opcode == Opcode::Branch) {
    if (next.size() != 1) {
      throw std::invalid_argument("Typed B needs one resolved guest target");
    }
    code.push_back("pc = " + std::to_string(next.front()) + "ul; break;");
  } else if (operation.opcode == Opcode::BranchLink) {
    const auto displacement = static_cast<std::int32_t>(operation.immediate);
    if (mode != LoweringMode::Process || operation.destination != 0 ||
        operation.source != 0 || operation.shift != 0 || operation.width != 64 ||
        next.size() != 2 || guest_code_limit == 0 || guest_pc >= guest_code_limit ||
        (operation.immediate & 3U) != 0 || displacement < -(1 << 27) ||
        displacement > ((1 << 27) - 4) || next[0] < 0 || next[0] >=
            static_cast<std::int64_t>(guest_code_limit) || next[1] < 0 ||
        next[1] >= static_cast<std::int64_t>(guest_code_limit) ||
        next[0] != static_cast<std::int64_t>(guest_pc) +
            displacement ||
        next[1] != static_cast<std::int64_t>(guest_pc) + 4) {
      throw std::invalid_argument("Typed BL needs a valid target and return PC");
    }
    code.push_back(
        "for (ulong auth_index = x[37]; auth_index > 0ul; --auth_index) { "
        "ulong auth_slot = 38ul + 3ul * (auth_index - 1ul); "
        "if (x[auth_slot + 2] >= 0xffffffff00000000ul) { --x[auth_slot + 2]; break; } }");
    code.push_back("x[30] = " + std::to_string(next[1]) + "ul;");
    code.push_back("pc = " + std::to_string(next[0]) + "ul; break;");
  } else if (operation.opcode == Opcode::BranchRegister) {
    if (mode != LoweringMode::Process || operation.destination != 0 ||
        operation.source > 31 || operation.immediate > 1 ||
        operation.shift != 0 || operation.width != 64 || !next.empty() ||
        guest_code_limit == 0 || guest_pc >= guest_code_limit) {
      throw std::invalid_argument("Typed BR/BLR operands are invalid");
    }
    code.push_back("x[33] = " + reg(operation.source, true) + ";");
    if (operation.immediate == 1) {
      code.push_back(
          "for (ulong auth_index = x[37]; auth_index > 0ul; --auth_index) { "
          "ulong auth_slot = 38ul + 3ul * (auth_index - 1ul); "
          "if (x[auth_slot + 2] >= 0xffffffff00000000ul) { --x[auth_slot + 2]; break; } }");
      code.push_back("x[30] = " + std::to_string(guest_pc + 4) + "ul;");
    }
    code.push_back("if (x[33] == 0ul || x[33] >= " +
                   std::to_string(guest_code_limit) +
                   "ul || (x[33] & 3ul) != 0ul) { x[874] = x[33]; x[33] = " +
                   std::to_string(guest_pc) + "ul; x[34] = 3; return; }");
    code.push_back("x[34] = 7; return;");
  } else if (operation.opcode == Opcode::BranchZero) {
    if (operation.source > 31 || operation.immediate > 1 ||
        (operation.width != 32 && operation.width != 64) || next.size() != 2) {
      throw std::invalid_argument("Typed CBZ/CBNZ needs two resolved guest edges");
    }
    const auto condition = reg(operation.source, operation.width == 64) +
                           (operation.immediate == 0 ? " == 0" : " != 0");
    code.push_back("pc = (" + condition + ") ? " +
                   pc_literal(next[0], "ul") + " : " +
                   pc_literal(next[1], "ul") + "; break;");
  } else if (operation.opcode == Opcode::TestBitBranch) {
    if (operation.destination >= operation.width || operation.source > 31 ||
        operation.immediate > 1 || (operation.width != 32 && operation.width != 64) ||
        next.size() != 2) {
      throw std::invalid_argument("Typed TBZ/TBNZ needs a valid bit and two resolved guest edges");
    }
    const auto value = reg(operation.source, operation.width == 64);
    const auto bitSet = "((" + value + " >> " +
                        std::to_string(operation.destination) + ") & 1ul) != 0";
    const auto takeTarget = operation.immediate == 0 ? "!(" + bitSet + ")" : bitSet;
    code.push_back("pc = (" + takeTarget + ") ? " +
                   pc_literal(next[0], "ul") + " : " +
                   pc_literal(next[1], "ul") + "; break;");
  } else if (operation.opcode == Opcode::BranchCondition) {
    if (operation.source > 15 || next.size() != 2 || operation.width != 64) {
      throw std::invalid_argument("Typed B.cond needs a condition and two resolved guest edges");
    }
    const auto flags = "(uint(z) | (uint(n) << 1u) | (uint(v) << 2u) | "
                       "(uint(c) << 3u))";
    code.push_back("pc = arm64_condition_holds(" +
                   std::to_string(operation.source) + "u, " + flags + ") ? " +
                   pc_literal(next[0], "ul") + " : " +
                   pc_literal(next[1], "ul") + "; break;");
  } else if (operation.opcode == Opcode::ConditionalSelect) {
    if (operation.destination > 31 || operation.source > 31 ||
        operation.immediate > 31 || operation.shift > 63 ||
        (operation.width != 32 && operation.width != 64)) {
      throw std::invalid_argument("Typed conditional select operands are invalid");
    }
    if (operation.destination != 31) {
      const auto flags = "(uint(z) | (uint(n) << 1u) | (uint(v) << 2u) | "
                         "(uint(c) << 3u))";
      code.push_back("x[" + std::to_string(operation.destination) +
                     "] = conditional_select_apply(" +
                     std::to_string(operation.shift >> 4U) + "u, " +
                     reg(operation.source, operation.width == 64) + ", " +
                     reg(operation.immediate, operation.width == 64) + ", " +
                     std::to_string(operation.shift & 15U) + "u, " + flags + ", " +
                     std::to_string(operation.width) + "u);");
    }
  } else {
    throw std::invalid_argument("Unknown typed ARM64 operation");
  }
  return Instruction{std::move(code), std::move(next), operation};
}

Instruction decode(std::uint32_t bits, std::int64_t pc,
                   const std::string& address_suffix) {
  const auto rd = field(bits, 0, 5);
  const auto rn = field(bits, 5, 5);
  const auto rm = field(bits, 16, 5);
  const bool wide = field(bits, 31, 1) == 1;
  const std::string cast = wide ? "ulong" : "uint";
  const auto following = pc + 4;

  const auto assign = [&](const std::string& value) {
    return rd == 31 ? std::string{} :
                      "x[" + std::to_string(rd) + "] = " + cast + "(" + value + ");";
  };
  const auto advance = [following](std::vector<std::string> code) {
    return Instruction{std::move(code), {following}};
  };

  if (const auto operation = decode_operation(bits);
      operation && operation->opcode != Opcode::LoadUnsignedImmediate &&
      operation->opcode != Opcode::StoreUnsignedImmediate &&
      operation->opcode != Opcode::LoadPair &&
      operation->opcode != Opcode::StorePair &&
      operation->opcode != Opcode::LoadUnscaledWriteback &&
      operation->opcode != Opcode::StoreUnscaledWriteback &&
      operation->opcode != Opcode::LoadSignedScalar &&
      operation->opcode != Opcode::BranchLink &&
      operation->opcode != Opcode::BranchRegister) {
    std::vector<std::int64_t> successors;
    if (operation->opcode == Opcode::Branch) {
      successors.push_back(pc + static_cast<std::int32_t>(operation->immediate));
    } else if (operation->opcode == Opcode::BranchZero) {
      successors.push_back(pc + static_cast<std::int32_t>(operation->shift));
      successors.push_back(following);
    } else if (operation->opcode == Opcode::TestBitBranch) {
      successors.push_back(pc + static_cast<std::int32_t>(operation->shift));
      successors.push_back(following);
    } else if (operation->opcode == Opcode::BranchCondition) {
      successors.push_back(pc + static_cast<std::int32_t>(operation->immediate));
      successors.push_back(following);
    } else if (operation->opcode != Opcode::Return) {
      successors.push_back(following);
    }
    return make_operation_instruction(*operation, std::move(successors),
                                      LoweringMode::Function, 0,
                                      static_cast<std::uint64_t>(pc));
  }
  const auto branch = [&](const std::string& condition, std::int64_t target) {
    return Instruction{{"pc = " + condition + " ? " + pc_literal(target, address_suffix) +
                        " : " + pc_literal(following, address_suffix) + "; break;"},
                       {target, following}};
  };

  if ((bits & 0xff00001e) == 0x54000000) {
    return branch(field(bits, 0, 1) == 0 ? "z" : "!z",
                  pc + signed_field(field(bits, 5, 19), 19) * 4);
  }
  if ((bits & 0x7f200000) == 0x0b000000 ||
      (bits & 0x7f200000) == 0x4a000000) {
    const auto shift = field(bits, 10, 6);
    if (field(bits, 22, 2) == 0 && shift < (wide ? 64u : 32u)) {
      const auto op = (bits & 0x7f000000) == 0x0b000000 ? "+" : "^";
      return advance({assign(reg(rn, wide) + " " + op + " (" + reg(rm, wide) +
                            " << " + std::to_string(shift) + ")")});
    }
  }
  if ((bits & 0x7f800000) == 0x71000000 && rn != 31) {
    const auto immediate = field(bits, 10, 12) << (field(bits, 22, 1) * 12);
    return advance({"z = (" + reg(rn, wide) + " == " + cast + "(" +
                        std::to_string(immediate) + "));",
                    assign(reg(rn, wide) + " - " + cast + "(" +
                           std::to_string(immediate) + ")")});
  }
  if ((bits & 0x7fe0ffe0) == 0x2a0003e0) return advance({assign(reg(rm, wide))});
  if ((bits & 0xffc0fc00) == 0x53007c00 && field(bits, 16, 6) < 32) {
    return advance({assign(reg(rn, false) + " >> " + std::to_string(field(bits, 16, 6)))});
  }
  // ANDS to WZR with the single-bit immediate 1.
  if ((bits & 0xfffffc1f) == 0x7200001f) {
    return advance({"z = ((" + reg(rn, false) + " & 1u) == 0);"});
  }
  if ((bits & 0x7fe0ec00) == 0x1a800400) {
    const auto condition = field(bits, 12, 1) == 0 ? "z" : "!z";
    return advance({assign(std::string(condition) + " ? " + reg(rn, wide) + " : " +
                           reg(rm, wide) + " + " + cast + "(1)")});
  }

  const auto word_memory = bits & 0xffc00000;
  const auto post_memory = bits & 0xffe00c00;
  const bool supported_memory = rn != 31 &&
      (word_memory == 0xb9400000 || word_memory == 0xb9000000 ||
       ((post_memory == 0xb8400400 || post_memory == 0xb8000400) && rn != rd));
  if (supported_memory) {
    const bool post = post_memory == 0xb8400400 || post_memory == 0xb8000400;
    const bool load = word_memory == 0xb9400000 || post_memory == 0xb8400400;
    const auto displacement = post ? 0u : field(bits, 10, 12) * 4;
    const auto base = reg(rn, true);
    const auto address = "(" + base + " + " + std::to_string(displacement) + "ul)";
    const auto index = "(" + address + " >> 2)";
    const auto bounds = "if ((" + address + " & 3ul) || " + index +
                        " >= count) { status[id] = 2; return; }";
    const auto access = load ? assign("words[" + index + "]")
                             : "words[" + index + "] = " + reg(rd, false) + ";";
    if (post) {
      return advance({bounds, access,
                      base + " += ulong(" +
                          std::to_string(signed_field(field(bits, 12, 9), 9)) + ");"});
    }
    return advance({bounds, access});
  }

  std::ostringstream message;
  message << "Unsupported ARM64 instruction 0x" << std::hex << std::nouppercase
          << std::setw(8) << std::setfill('0') << bits << " at __text+0x"
          << pc;
  throw std::runtime_error(message.str());
}

std::set<std::int64_t> block_entries(
    const std::map<std::int64_t, Instruction>& instructions,
    std::int64_t entry) {
  std::set<std::int64_t> leaders{entry};
  for (const auto& [pc, instruction] : instructions) {
    if (!instruction.ends_block_at(pc)) continue;
    leaders.insert(instruction.next.begin(), instruction.next.end());
  }
  for (auto it = leaders.begin(); it != leaders.end();) {
    if (instructions.find(*it) == instructions.end()) it = leaders.erase(it);
    else ++it;
  }
  return leaders;
}

std::string render_blocks(
    const std::map<std::int64_t, Instruction>& instructions,
    std::int64_t entry, const std::string& address_suffix) {
  const auto leaders = block_entries(instructions, entry);
  std::vector<std::string> blocks;
  for (const auto block_entry : leaders) {
    std::vector<std::string> lines{"case " + pc_literal(block_entry, address_suffix) + ":"};
    auto pc = block_entry;
    while (true) {
      const auto found = instructions.find(pc);
      if (found == instructions.end()) break;
      const auto& instruction = found->second;
      for (const auto& statement : instruction.code) {
        if (!statement.empty()) lines.push_back("    " + statement);
      }
      if (instruction.ends_block_at(pc)) break;
      pc += 4;
      if (leaders.find(pc) != leaders.end() || instructions.find(pc) == instructions.end()) {
        lines.push_back("    pc = " + pc_literal(pc, address_suffix) + "; break;");
        break;
      }
    }
    std::ostringstream block;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (i != 0) block << '\n';
      block << lines[i];
    }
    blocks.push_back(block.str());
  }
  std::ostringstream rendered;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    if (i != 0) rendered << '\n';
    rendered << blocks[i];
  }
  return rendered.str();
}

}  // namespace arm64metal::translate_core
