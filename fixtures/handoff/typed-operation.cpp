#include "translate_core.hpp"
#include "integer_ops_shared.h"

#include <cassert>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using arm64metal::translate_core::LoweringMode;
using arm64metal::translate_core::Opcode;
using arm64metal::translate_core::decode_operation;
using arm64metal::translate_core::make_operation_instruction;
using arm64metal::translate_core::serialize_operation;

int main() {
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

  assert(!decode_operation(0x10000000).has_value());
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

  const auto backwardBranch = decode_operation(0x17ffffff);
  assert(backwardBranch.has_value());
  assert(static_cast<std::int32_t>(backwardBranch->immediate) == -4);

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
  return 0;
}
