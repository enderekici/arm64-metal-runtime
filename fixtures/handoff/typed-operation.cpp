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
         std::vector<std::string>{"x[0] = ulong(305397760ul);"});

  const auto zeroRegister = decode_operation(0xd2c2469f);
  assert(zeroRegister.has_value());
  const auto discarded = make_operation_instruction(
      *zeroRegister, {0x1004}, LoweringMode::Process,
      std::uint64_t{1} << 48);
  assert(discarded.operation->destination == 31);
  assert(discarded.code.empty());

  const auto returnOperation = decode_operation(0xd65f03c0);
  assert(returnOperation.has_value());
  assert(returnOperation->opcode == Opcode::Return);
  assert(returnOperation->source == 30);
  const auto returned = make_operation_instruction(
      *returnOperation, {}, LoweringMode::Process, std::uint64_t{1} << 48);
  assert(returned.ends_block_at(0x1000));
  assert(returned.code.size() == 5);

  assert(!decode_operation(0xf2a24680).has_value());

  bool invalidShiftRejected = false;
  try {
    (void)decode_operation(0x52c24680);
  } catch (const std::runtime_error&) {
    invalidShiftRejected = true;
  }
  assert(invalidShiftRejected);
  return 0;
}
