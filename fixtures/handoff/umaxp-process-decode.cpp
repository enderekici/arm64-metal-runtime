#include "process_decoder.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

struct alignas(16) VectorPair {
  std::uint64_t lane0;
  std::uint64_t lane1;
};

#define DEFINE_UMAXP(name, arrangement)                                      \
  static VectorPair name(VectorPair left, VectorPair right) {                \
    VectorPair result{};                                                     \
    asm volatile("ldr q1, [%1]\n\tldr q2, [%2]\n\tumaxp v0." #arrangement \
                 ", v1." #arrangement ", v2." #arrangement                 \
                 "\n\tstr q0, [%0]"                                        \
                 :                                                           \
                 : "r"(&result), "r"(&left), "r"(&right)                   \
                 : "v0", "v1", "v2", "memory");                            \
    return result;                                                           \
  }

DEFINE_UMAXP(native_umaxp_8b, 8b)
DEFINE_UMAXP(native_umaxp_16b, 16b)
DEFINE_UMAXP(native_umaxp_4h, 4h)
DEFINE_UMAXP(native_umaxp_8h, 8h)
DEFINE_UMAXP(native_umaxp_2s, 2s)
DEFINE_UMAXP(native_umaxp_4s, 4s)

static VectorPair native_umaxp_alias_2s(VectorPair source) {
  asm volatile("ldr q0, [%0]\n\tumaxp v0.2s, v0.2s, v0.2s\n\tstr q0, [%0]"
               :
               : "r"(&source)
               : "v0", "memory");
  return source;
}

static std::uint64_t lane(const VectorPair& value, unsigned index,
                          unsigned width) {
  const std::uint64_t mask = (std::uint64_t{1} << width) - 1;
  const auto bit = index * width;
  const auto word = bit < 64 ? value.lane0 : value.lane1;
  return (word >> (bit & 63)) & mask;
}

static VectorPair expected_umaxp(const VectorPair& left, const VectorPair& right,
                                 unsigned width, unsigned bits) {
  VectorPair result{};
  const auto lanes = bits / width;
  for (unsigned output = 0; output < lanes; ++output) {
    const auto& source = output < lanes / 2 ? left : right;
    const auto pair = (output % (lanes / 2)) * 2;
    const auto value = std::max(lane(source, pair, width),
                                lane(source, pair + 1, width));
    const auto bit = output * width;
    if (bit < 64) {
      result.lane0 |= value << bit;
    } else {
      result.lane1 |= value << (bit - 64);
    }
  }
  return result;
}

static void check(VectorPair actual, VectorPair expected) {
  assert(actual.lane0 == expected.lane0);
  assert(actual.lane1 == expected.lane1);
}

static std::string generated(std::uint32_t bits) {
  const auto instruction =
      arm64metal::decode_process(bits, 0x1000, {}, {}, false);
  assert(instruction.next == std::vector<std::int64_t>{0x1004});
  std::string code;
  for (const auto& line : instruction.code) code += line + "\n";
  return code;
}

int main(int argc, char** argv) {
  const VectorPair left{0x8000000100000002ULL, 0x7fffffffffffffffULL};
  const VectorPair right{0xffff000000000003ULL, 0x0000000500000004ULL};
  check(native_umaxp_8b(left, right), expected_umaxp(left, right, 8, 64));
  check(native_umaxp_16b(left, right), expected_umaxp(left, right, 8, 128));
  check(native_umaxp_4h(left, right), expected_umaxp(left, right, 16, 64));
  check(native_umaxp_8h(left, right), expected_umaxp(left, right, 16, 128));
  check(native_umaxp_2s(left, right), expected_umaxp(left, right, 32, 64));
  check(native_umaxp_4s(left, right), expected_umaxp(left, right, 32, 128));

  const auto aliasExpected = expected_umaxp(left, left, 32, 64);
  check(native_umaxp_alias_2s(left), aliasExpected);

  const std::array<std::uint32_t, 6> encodings{
      0x2e22a420U, 0x6e22a420U, 0x2e62a420U,
      0x6e62a420U, 0x2ea2a420U, 0x6ea2a420U};
  const std::array<unsigned, 6> widths{8, 8, 16, 16, 32, 32};
  const std::array<unsigned, 6> bits{64, 128, 64, 128, 64, 128};
  for (std::size_t i = 0; i < encodings.size(); ++i) {
    const auto code = generated(encodings[i]);
    assert(code.find("ulong2 left = ulong2(x[808], x[809])") !=
           std::string::npos);
    assert(code.find("right = ulong2(x[810], x[811])") != std::string::npos);
    assert(code.find("lane < " + std::to_string(bits[i] / widths[i])) !=
           std::string::npos);
    assert(code.find("lane_add(value, lane, " + std::to_string(widths[i])) !=
           std::string::npos);
  }
  const auto aliasCode = generated(0x2ea0a400U);
  assert(aliasCode.find("left = ulong2(x[806], x[807])") !=
         std::string::npos);
  assert(aliasCode.find("right = ulong2(x[806], x[807])") !=
         std::string::npos);

  bool rejected = false;
  try {
    (void)generated(0x2ee2a420U);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  if (argc == 2) {
    std::ofstream metal(argv[1]);
    assert(metal.good());
    metal << R"metal(#include <metal_stdlib>
using namespace metal;
ulong lane_mask(uint width) { return ~0ul >> (64 - width); }
ulong lane_read(ulong2 source, uint index, uint width) {
  uint bit = index * width;
  return (source[bit >> 6] >> (bit & 63)) & lane_mask(width);
}
void lane_add(thread ulong2 &target, uint index, uint width, ulong value) {
  uint bit = index * width;
  target[bit >> 6] |= (value & lane_mask(width)) << (bit & 63);
}
kernel void umaxp_compile(device ulong* x [[buffer(0)]]) {
)metal";
    for (const auto encoding : encodings) metal << generated(encoding) << '\n';
    metal << "}\n";
    assert(metal.good());
  }
  return 0;
}
