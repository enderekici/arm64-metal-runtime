#include "guest_vector_crypto.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

bool contains(const arm64metal::translate_core::Instruction& instruction,
              std::string_view text) {
  return !instruction.code.empty() && instruction.code.front().find(text) !=
      std::string::npos;
}

void emit_msl() {
  std::cout << "#include <metal_stdlib>\nusing namespace metal;\n"
               "ulong lane_mask(uint width) { return ~0ul >> (64u - width); }\n"
               "ulong lane_read(ulong2 source, uint index, uint width) {\n"
               "  uint bit = index * width; return (source[bit >> 6u] >> (bit & 63u)) & lane_mask(width);\n"
               "}\n"
               "void lane_add(thread ulong2 &target, uint index, uint width, ulong value) {\n"
               "  uint bit = index * width; target[bit >> 6u] |= (value & lane_mask(width)) << (bit & 63u);\n"
               "}\n";
  std::cout << arm64metal::vector_crypto_metal_source() << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--emit-metal") {
    emit_msl();
    return 0;
  }
  using arm64metal::lower_vector_crypto;
  const auto sha256h = lower_vector_crypto(0x5e024020u, 0x1000);
  assert(sha256h && contains(*sha256h, "vector_sha256_hash(ulong2(x[806], x[807]), ulong2(x[808], x[809]), ulong2(x[810], x[811]), true)"));
  const auto sha256h2 = lower_vector_crypto(0x5e025020u, 0x1004);
  assert(sha256h2 && contains(*sha256h2, "vector_sha256_hash(ulong2(x[808], x[809]), ulong2(x[806], x[807]), ulong2(x[810], x[811]), false)"));
  const auto sha256su0 = lower_vector_crypto(0x5e282820u, 0x1008);
  assert(sha256su0 && contains(*sha256su0, "vector_sha256_su0(ulong2(x[806], x[807]), ulong2(x[808], x[809]))"));
  const auto sha256su1 = lower_vector_crypto(0x5e026020u, 0x100c);
  assert(sha256su1 && contains(*sha256su1, "vector_sha256_su1(ulong2(x[806], x[807]), ulong2(x[808], x[809]), ulong2(x[810], x[811]))"));

  const auto h_alias = lower_vector_crypto(0x5e024000u, 0x1010);
  assert(h_alias && contains(*h_alias, "vector_sha256_hash(ulong2(x[806], x[807]), ulong2(x[806], x[807])"));
  const auto su0_alias = lower_vector_crypto(0x5e282800u, 0x1014);
  assert(su0_alias && contains(*su0_alias, "vector_sha256_su0(ulong2(x[806], x[807]), ulong2(x[806], x[807]))"));
  const auto su1_alias = lower_vector_crypto(0x5e006020u, 0x1018);
  assert(su1_alias && contains(*su1_alias, "vector_sha256_su1(ulong2(x[806], x[807]), ulong2(x[808], x[809]), ulong2(x[806], x[807]))"));
  assert(!lower_vector_crypto(0x5e282c20u, 0x101c));

  const auto& source = arm64metal::vector_crypto_metal_source();
  for (const auto helper : {"vector_sha256_hash_words", "vector_sha256_su0",
                            "vector_sha256_su1"}) {
    assert(source.find(helper) != std::string_view::npos);
  }
  return 0;
}
