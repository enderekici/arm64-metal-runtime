#include "process_decoder.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

struct alignas(16) VectorPair {
  std::uint64_t lane0;
  std::uint64_t lane1;
};

static VectorPair native_addp(VectorPair source) {
  VectorPair result{};
  asm volatile("ldr q0, [%1]\n\taddp d0, v0.2d\n\tstr q0, [%0]"
               :
               : "r"(&result), "r"(&source)
               : "v0", "memory");
  return result;
}

int main() {
  const std::array<VectorPair, 4> values{{
      {0xffffffffffffffffULL, 1U},
      {0xffffffffffffffffULL, 0xffffffffffffffffULL},
      {0x0123456789abcdefULL, 0xfedcba9876543210ULL},
      {0U, 0U},
  }};
  for (const auto& source : values) {
    const auto result = native_addp(source);
    assert(result.lane0 == source.lane0 + source.lane1);
    assert(result.lane1 == 0U);
  }

  const auto instruction =
      arm64metal::decode_process(0x5ef1b821U, 0x1000, {}, {}, false);
  assert(instruction.next == std::vector<std::int64_t>{0x1004});
  std::string code;
  for (const auto& line : instruction.code) code += line + "\n";
  assert(code.find("ulong2 source = ulong2(x[808], x[809])") !=
         std::string::npos);
  assert(code.find("x[808] = source[0] + source[1]") != std::string::npos);
  assert(code.find("x[809] = 0ul") != std::string::npos);
  return 0;
}
