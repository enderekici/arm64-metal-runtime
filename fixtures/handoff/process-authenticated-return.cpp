#include "process_decoder.hpp"
#include "gpu_interpreter.hpp"
#include "macho.hpp"

#include <cassert>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <stdexcept>
#include <vector>

using arm64metal::decode_process;
using arm64metal::translate_core::AuthenticatedReturnStep;
using arm64metal::translate_core::Opcode;
using arm64metal::translate_core::decode_operation;

int main(int argc, char** argv) {
  using arm64metal::gpu_interpreter::authShadowFrameCapacity;
  using arm64metal::gpu_interpreter::authShadowStateWords;
  using arm64metal::gpu_interpreter::authenticated_return_state_valid;
  assert(authenticated_return_state_valid(authShadowStateWords * sizeof(std::uint64_t), 0));
  assert(authenticated_return_state_valid(
      authShadowStateWords * sizeof(std::uint64_t), authShadowFrameCapacity));
  assert(!authenticated_return_state_valid(
      authShadowStateWords * sizeof(std::uint64_t) - 1, 0));
  assert(!authenticated_return_state_valid(
      authShadowStateWords * sizeof(std::uint64_t), authShadowFrameCapacity + 1));

  if (argc == 3) {
    for (int index = 1; index < argc; ++index) {
      arm64metal::MachO arm64eExecutable(argv[index], true);
      assert((arm64eExecutable.word(8) & 0x00ffffffU) == 2U);
      bool defaultPolicyRejects = false;
      try {
        (void)arm64metal::MachO(argv[index]);
      } catch (const std::runtime_error&) {
        defaultPolicyRejects = true;
      }
      assert(defaultPolicyRejects);
    }
  } else {
    assert(argc == 1);
  }

  const std::map<std::int64_t, std::string> imports;
  const std::map<std::int64_t, std::int64_t> stubs;
  const auto pacibsp = decode_process(0xd503237f, 0x1000, imports, stubs, false);
  assert(pacibsp.operation.has_value());
  assert(pacibsp.operation->opcode == Opcode::AuthenticatedReturn);
  assert(pacibsp.operation->destination == static_cast<std::uint32_t>(
                                             AuthenticatedReturnStep::SignLinkRegister));
  assert(pacibsp.next == std::vector<std::int64_t>{0x1004});
  assert(pacibsp.code[2].find("x[slot + 2] = 4096ul") != std::string::npos);

  const auto autibsp = decode_process(0xd50323ff, 0x1004, imports, stubs, false);
  assert(autibsp.operation.has_value());
  assert(autibsp.operation->destination == static_cast<std::uint32_t>(
                                              AuthenticatedReturnStep::AuthenticateLinkRegister));
  assert(autibsp.next == std::vector<std::int64_t>{0x1008});

  const auto retab = decode_process(0xd65f0fff, 0x1008, imports, stubs, false);
  assert(retab.operation.has_value());
  assert(retab.operation->destination ==
         static_cast<std::uint32_t>(AuthenticatedReturnStep::Return));
  assert(retab.next.empty());
  assert(!decode_operation(0xd503237f).has_value());

  bool keyAStillRejected = false;
  try {
    (void)decode_process(0xd503233f, 0x1000, imports, stubs, false);
  } catch (const std::runtime_error&) {
    keyAStillRejected = true;
  }
  assert(keyAStillRejected);

  const auto checkTypedPcSequence = [&imports, &stubs](
      std::int64_t base, const auto& words, const auto& expectedSteps) {
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto instruction = decode_process(
          words[index], base + static_cast<std::int64_t>(index * 4), imports,
          stubs, false);
      assert(instruction.operation.has_value());
      assert(instruction.operation->opcode == Opcode::AuthenticatedReturn ||
             instruction.operation->opcode == Opcode::MoveWideZero ||
             instruction.operation->opcode == Opcode::Return ||
             instruction.operation->opcode == Opcode::AddImmediate ||
             instruction.operation->opcode == Opcode::SubImmediate ||
             instruction.operation->opcode == Opcode::LogicalShiftedRegister);
      assert(static_cast<std::uint32_t>(instruction.operation->opcode) ==
             expectedSteps[index]);
      if (index + 1 < words.size()) {
        assert(instruction.next == std::vector<std::int64_t>{base +
            static_cast<std::int64_t>((index + 1) * 4)});
      } else {
        assert(instruction.next.empty());
      }
    }
  };
  const std::array<std::uint32_t, 4> authenticatedThenReturn{
      0xd503237f, 0xd50323ff, 0x52800000, 0xd65f03c0};
  const std::array<std::uint32_t, 4> authenticatedThenReturnOps{
      36, 36, 1, 2};
  checkTypedPcSequence(0x2000, authenticatedThenReturn,
                       authenticatedThenReturnOps);
  const std::array<std::uint32_t, 3> authenticatedRetab{
      0xd503237f, 0x52800000, 0xd65f0fff};
  const std::array<std::uint32_t, 3> authenticatedRetabOps{36, 1, 36};
  checkTypedPcSequence(0x3000, authenticatedRetab, authenticatedRetabOps);
  const std::array<std::uint32_t, 5> badLinkRegister{
      0xd503237f, 0xaa1f03fe, 0xd50323ff, 0x52800000, 0xd65f03c0};
  const std::array<std::uint32_t, 5> badLinkRegisterOps{36, 23, 36, 1, 2};
  checkTypedPcSequence(0x4000, badLinkRegister, badLinkRegisterOps);
  const std::array<std::uint32_t, 6> badStackPointer{
      0xd503237f, 0xd10043ff, 0xd50323ff, 0x910043ff, 0x52800000,
      0xd65f03c0};
  const std::array<std::uint32_t, 6> badStackPointerOps{36, 10, 36, 9, 1, 2};
  checkTypedPcSequence(0x5000, badStackPointer, badStackPointerOps);
  const std::array<std::uint32_t, 3> badLinkRegisterRetab{
      0xd503237f, 0xaa1f03fe, 0xd65f0fff};
  const std::array<std::uint32_t, 3> badLinkRegisterRetabOps{36, 23, 36};
  checkTypedPcSequence(0x6000, badLinkRegisterRetab,
                       badLinkRegisterRetabOps);
  const std::array<std::uint32_t, 3> badStackPointerRetab{
      0xd503237f, 0xd10043ff, 0xd65f0fff};
  const std::array<std::uint32_t, 3> badStackPointerRetabOps{36, 10, 36};
  checkTypedPcSequence(0x7000, badStackPointerRetab,
                       badStackPointerRetabOps);
  return 0;
}
