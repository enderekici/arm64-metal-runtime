#include "process_decoder.hpp"
#include "translate_core.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>

int main() {
  struct Case {
    std::uint32_t bits;
    std::uint32_t destination;
    std::uint32_t source;
    std::uint32_t right;
    std::uint32_t packed;
  };
  constexpr Case cases[] = {
      {0x9a020023U, 3, 1, 2, 0},       // ADC x3, x1, x2
      {0xda010024U, 4, 1, 1, 1},       // SBC x4, x1, x1
      {0xfa090129U, 9, 9, 9, 3},       // SBCS x9, x9, x9
      {0xba0e01b0U, 16, 13, 14, 2},    // ADCS x16, x13, x14
  };
  std::int64_t pc = 0x1000;
  for (const auto& test : cases) {
    const auto instruction = arm64metal::decode_process(
        test.bits, pc, std::map<std::int64_t, std::string>{},
        std::map<std::int64_t, std::int64_t>{}, false);
    assert(instruction.operation.has_value());
    const auto& operation = *instruction.operation;
    assert(operation.opcode == arm64metal::translate_core::Opcode::AddSubCarryRegister);
    assert(operation.destination == test.destination && operation.source == test.source &&
           operation.immediate == test.right && operation.shift == test.packed &&
           operation.width == 64);
    assert(!instruction.code.empty() &&
           instruction.code[1].find("add_sub_carry_apply(") != std::string::npos);
    std::cout << "interpreter-path=process-typed pc=0x" << std::hex << pc
              << " opcode=26 bits=0x" << test.bits << "\n";
    pc += 4;
  }
}
