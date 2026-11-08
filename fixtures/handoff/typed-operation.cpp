#include "translate_core.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>

using arm64metal::translate_core::LoweringMode;
using arm64metal::translate_core::Opcode;
using arm64metal::translate_core::decode_operation;
using arm64metal::translate_core::make_operation_instruction;
using arm64metal::translate_core::serialize_operation;

int main() {
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
  assert(addWideLowered.code == std::vector<std::string>{
      "x[0] = add_sub_immediate_apply(0u, x[0], 291u, 12u, 64u);"});

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
  assert(!decode_operation(0x910003ff).has_value());
  assert(!decode_operation(0x910003e0).has_value());
  assert(!decode_operation(0x9100001f).has_value());
  assert(!decode_operation(0xb1000400).has_value());
  assert(!decode_operation(0xf100003f).has_value());

  bool invalidShiftRejected = false;
  try {
    (void)decode_operation(0x52c24680);
  } catch (const std::runtime_error&) {
    invalidShiftRejected = true;
  }
  assert(invalidShiftRejected);
  return 0;
}
