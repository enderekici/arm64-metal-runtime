#include "process_decoder.hpp"

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

static VectorPair native_xtn_2s(VectorPair source) {
  VectorPair result{};
  asm volatile("ldr q1, [%1]\n\txtn v0.2s, v1.2d\n\tstr q0, [%0]"
               :
               : "r"(&result), "r"(&source)
               : "v0", "v1", "memory");
  return result;
}

static VectorPair native_xtn2_alias_4s(VectorPair source) {
  asm volatile("ldr q0, [%0]\n\txtn2 v0.4s, v0.2d\n\tstr q0, [%0]"
               :
               : "r"(&source)
               : "v0", "memory");
  return source;
}

static VectorPair native_xtn_4h(VectorPair source) {
  VectorPair result{};
  asm volatile("ldr q1, [%1]\n\txtn v0.4h, v1.4s\n\tstr q0, [%0]"
               :
               : "r"(&result), "r"(&source)
               : "v0", "v1", "memory");
  return result;
}

static VectorPair native_xtn2_8h(VectorPair source) {
  VectorPair result{0x0123456789abcdefULL, 0xffffffffffffffffULL};
  asm volatile("ldr q0, [%0]\n\tldr q1, [%1]\n\txtn2 v0.8h, v1.4s\n\tstr q0, [%0]"
               :
               : "r"(&result), "r"(&source)
               : "v0", "v1", "memory");
  return result;
}

static VectorPair native_xtn_8b(VectorPair source) {
  VectorPair result{};
  asm volatile("ldr q1, [%1]\n\txtn v0.8b, v1.8h\n\tstr q0, [%0]"
               :
               : "r"(&result), "r"(&source)
               : "v0", "v1", "memory");
  return result;
}

static VectorPair native_xtn2_16b(VectorPair source) {
  VectorPair result{0xaabbccddeeff0011ULL, 0x2233445566778899ULL};
  asm volatile("ldr q0, [%0]\n\tldr q1, [%1]\n\txtn2 v0.16b, v1.8h\n\tstr q0, [%0]"
               :
               : "r"(&result), "r"(&source)
               : "v0", "v1", "memory");
  return result;
}

static std::string generated(const std::uint32_t bits) {
  const auto instruction =
      arm64metal::decode_process(bits, 0x1000, {}, {}, false);
  assert(instruction.next == std::vector<std::int64_t>{0x1004});
  std::string code;
  for (const auto& line : instruction.code) code += line + "\n";
  return code;
}

int main(int argc, char** argv) {
  const VectorPair source{0x1122334455667788ULL, 0x99aabbccddeeff00ULL};
  const auto low32 = native_xtn_2s(source);
  assert(low32.lane0 == 0xddeeff0055667788ULL && low32.lane1 == 0);

  const auto high32 = native_xtn2_alias_4s(source);
  assert(high32.lane0 == source.lane0 && high32.lane1 == 0xddeeff0055667788ULL);

  const auto low16 = native_xtn_4h(source);
  assert(low16.lane0 == 0xbbccff0033447788ULL && low16.lane1 == 0);

  const auto high16 = native_xtn2_8h(source);
  assert(high16.lane0 == 0x0123456789abcdefULL);
  assert(high16.lane1 == 0xbbccff0033447788ULL);

  const auto low8 = native_xtn_8b(source);
  assert(low8.lane0 == 0xaaccee0022446688ULL && low8.lane1 == 0);

  const auto high8 = native_xtn2_16b(source);
  assert(high8.lane0 == 0xaabbccddeeff0011ULL);
  assert(high8.lane1 == 0xaaccee0022446688ULL);

  const auto xtn = generated(0x0ea12800U);
  assert(xtn.find("lane_add(value, lane + 0, 32, lane_read(source, lane, 64))") !=
         std::string::npos);
  assert(xtn.find("value = ulong2(0)") != std::string::npos);

  const auto xtn2Alias = generated(0x4ea12800U);
  assert(xtn2Alias.find("lane_add(value, lane + 2, 32, lane_read(source, lane, 64))") !=
         std::string::npos);
  assert(xtn2Alias.find("ulong2(x[806], x[807])[0], 0ul") != std::string::npos);

  const auto byteXtn = generated(0x0e212820U);
  assert(byteXtn.find("lane_add(value, lane + 0, 8, lane_read(source, lane, 16))") !=
         std::string::npos);
  const auto byteXtn2 = generated(0x4e2128e6U);
  assert(byteXtn2.find("lane_add(value, lane + 8, 8, lane_read(source, lane, 16))") !=
         std::string::npos);

  bool rejected = false;
  try {
    (void)generated(0x0ee12800U);
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
kernel void xtn_compile(device ulong* x [[buffer(0)]]) {
)metal";
    for (const auto bits : std::array<std::uint32_t, 6>{
             0x0ea12800U, 0x4ea12800U, 0x0e612800U,
             0x4e612800U, 0x0e212800U, 0x4e212800U}) {
      metal << generated(bits) << '\n';
    }
    metal << "}\n";
    assert(metal.good());
  }
  return 0;
}
