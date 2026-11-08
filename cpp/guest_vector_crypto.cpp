#include "guest_vector_crypto.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace arm64metal {
namespace {

constexpr std::string_view kMetalSource = R"METAL(uint aes_xtime(uint value) {
    return ((value << 1) & 0xffu) ^ (0x1bu & (0u - (value >> 7)));
}
uint aes_multiply(uint left, uint right) {
    uint result = 0;
    for (uint bit = 0; bit < 8; ++bit) {
        result ^= left & (0u - (right & 1u));
        left = aes_xtime(left);
        right >>= 1;
    }
    return result;
}
uint aes_inverse(uint value) {
    // In GF(2^8), the multiplicative inverse is value^254.
    uint power = value;
    uint result = 1;
    for (uint bit = 1; bit < 8; ++bit) {
        power = aes_multiply(power, power);
        result = aes_multiply(result, power);
    }
    return result;
}
uint aes_rotate_byte(uint value, uint shift) {
    return ((value << shift) | (value >> (8 - shift))) & 0xffu;
}
ulong2 vector_aes_round(ulong2 state, ulong2 key, bool decrypt) {
    ulong2 input = state ^ key;
    ulong2 output = ulong2(0);
    for (uint lane = 0; lane < 16; ++lane) {
        uint row = lane & 3u;
        uint column = ((lane >> 2) + (decrypt ? 4u - row : row)) & 3u;
        uint value = uint(lane_read(input, 4u * column + row, 8));
        if (decrypt) {
            value = aes_inverse(aes_rotate_byte(value, 1) ^ aes_rotate_byte(value, 3) ^
                                aes_rotate_byte(value, 6) ^ 0x05u);
        } else {
            value = aes_inverse(value);
            value ^= aes_rotate_byte(value, 1) ^ aes_rotate_byte(value, 2) ^
                     aes_rotate_byte(value, 3) ^ aes_rotate_byte(value, 4) ^ 0x63u;
        }
        lane_add(output, lane, 8, value);
    }
    return output;
}
ulong2 vector_aes_mix(ulong2 input, bool inverse) {
    ulong2 output = ulong2(0);
    for (uint column = 0; column < 16; column += 4) {
        uint4 bytes = uint4(lane_read(input, column, 8), lane_read(input, column + 1, 8),
                            lane_read(input, column + 2, 8), lane_read(input, column + 3, 8));
        if (inverse) {
            uint even = aes_xtime(aes_xtime(bytes[0] ^ bytes[2]));
            uint odd = aes_xtime(aes_xtime(bytes[1] ^ bytes[3]));
            bytes ^= uint4(even, odd, even, odd);
        }
        uint parity = bytes[0] ^ bytes[1] ^ bytes[2] ^ bytes[3];
        for (uint row = 0; row < 4; ++row) {
            uint value = bytes[row] ^ parity ^ aes_xtime(bytes[row] ^ bytes[(row + 1) & 3]);
            lane_add(output, column + row, 8, value);
        }
    }
    return output;
}
ulong2 vector_pmull(ulong left, ulong right) {
    ulong2 product = ulong2(0ul, 0ul);
    for (uint bit = 0; bit < 64; ++bit) {
        if (((right >> bit) & 1ul) != 0) {
            product[0] ^= left << bit;
            if (bit != 0) product[1] ^= left >> (64 - bit);
        }
    }
    return product;
}
uint sha256_rotr(uint value, uint amount) {
    return (value >> amount) | (value << (32u - amount));
}
uint sha256_sigma0(uint value) {
    return sha256_rotr(value, 2u) ^ sha256_rotr(value, 13u) ^ sha256_rotr(value, 22u);
}
uint sha256_sigma1(uint value) {
    return sha256_rotr(value, 6u) ^ sha256_rotr(value, 11u) ^ sha256_rotr(value, 25u);
}
uint sha256_small_sigma0(uint value) {
    return sha256_rotr(value, 7u) ^ sha256_rotr(value, 18u) ^ (value >> 3u);
}
uint sha256_small_sigma1(uint value) {
    return sha256_rotr(value, 17u) ^ sha256_rotr(value, 19u) ^ (value >> 10u);
}
uint4 vector_sha256_hash_words(uint4 abcd, uint4 efgh, uint4 schedule, bool first) {
    for (uint round = 0; round < 4; ++round) {
        uint a = abcd.x, b = abcd.y, c = abcd.z, d = abcd.w;
        uint e = efgh.x, f = efgh.y, g = efgh.z, h = efgh.w;
        uint choose = (e & f) ^ (~e & g);
        uint majority = (a & b) ^ (a & c) ^ (b & c);
        uint t1 = h + sha256_sigma1(e) + choose + schedule[round];
        uint t2 = sha256_sigma0(a) + majority;
        abcd = uint4(t1 + t2, a, b, c);
        efgh = uint4(d + t1, e, f, g);
    }
    return first ? abcd : efgh;
}
ulong2 vector_sha256_hash(ulong2 abcd_bits, ulong2 efgh_bits,
                          ulong2 schedule_bits, bool first) {
    uint4 abcd = as_type<uint4>(abcd_bits);
    uint4 efgh = as_type<uint4>(efgh_bits);
    uint4 schedule = as_type<uint4>(schedule_bits);
    return as_type<ulong2>(vector_sha256_hash_words(abcd, efgh, schedule, first));
}
ulong2 vector_sha256_su0(ulong2 destination_bits, ulong2 source_bits) {
    uint4 destination = as_type<uint4>(destination_bits);
    uint4 source = as_type<uint4>(source_bits);
    uint4 schedule = uint4(destination.y, destination.z, destination.w, source.x);
    uint4 result = uint4(
        destination.x + sha256_small_sigma0(schedule.x),
        destination.y + sha256_small_sigma0(schedule.y),
        destination.z + sha256_small_sigma0(schedule.z),
        destination.w + sha256_small_sigma0(schedule.w));
    return as_type<ulong2>(result);
}
ulong2 vector_sha256_su1(ulong2 destination_bits, ulong2 source1_bits,
                         ulong2 source2_bits) {
    uint4 destination = as_type<uint4>(destination_bits);
    uint4 source1 = as_type<uint4>(source1_bits);
    uint4 source2 = as_type<uint4>(source2_bits);
    uint4 schedule = uint4(source1.y, source1.z, source1.w, source2.x);
    uint4 result;
    result.x = destination.x + schedule.x + sha256_small_sigma1(source2.z);
    result.y = destination.y + schedule.y + sha256_small_sigma1(source2.w);
    result.z = destination.z + schedule.z + sha256_small_sigma1(result.x);
    result.w = destination.w + schedule.w + sha256_small_sigma1(result.y);
    return as_type<ulong2>(result);
})METAL";

}  // namespace

std::optional<translate_core::Instruction> lower_vector_crypto(
    std::uint32_t bits, std::int64_t pc) {
  const std::uint32_t opcode = bits & 0xfffffc00u;
  constexpr std::array<std::uint32_t, 4> aes_opcodes = {
      0x4e284800u, 0x4e285800u, 0x4e286800u, 0x4e287800u};
  if (std::find(aes_opcodes.begin(), aes_opcodes.end(), opcode) !=
      aes_opcodes.end()) {
    const int destination = 806 + 2 * static_cast<int>(bits & 0x1fu);
    const int source = 806 + 2 * static_cast<int>((bits >> 5) & 0x1fu);
    const std::string input = "ulong2(x[" + std::to_string(source) + "], x[" +
                              std::to_string(source + 1) + "])";
    const bool inverse = (bits & 0x1000u) != 0;
    const std::string expression =
        (bits & 0x2000u) == 0
            ? "vector_aes_round(ulong2(x[" + std::to_string(destination) +
                  "], x[" + std::to_string(destination + 1) + "]), " + input +
                  ", " + (inverse ? "true" : "false") + ")"
            : "vector_aes_mix(" + input + ", " +
                  (inverse ? "true" : "false") + ")";
    return translate_core::Instruction{{"{ ulong2 value = " + expression + "; x[" +
                        std::to_string(destination) + "] = value[0]; x[" +
                        std::to_string(destination + 1) + "] = value[1]; }"},
                       {pc + 4}};
  }

  const std::uint32_t sha256_three_register_opcode = bits & 0xffe0fc00u;
  constexpr std::uint32_t sha256h_opcode = 0x5e004000u;
  constexpr std::uint32_t sha256h2_opcode = 0x5e005000u;
  constexpr std::uint32_t sha256su1_opcode = 0x5e006000u;
  const std::uint32_t sha_destination_register = bits & 0x1fu;
  const std::uint32_t sha_source_register = (bits >> 5) & 0x1fu;
  const std::uint32_t sha_third_register = (bits >> 16) & 0x1fu;
  const auto vector_value = [](std::uint32_t reg) {
    const int slot = 806 + 2 * static_cast<int>(reg);
    return "ulong2(x[" + std::to_string(slot) + "], x[" +
           std::to_string(slot + 1) + "])";
  };
  const int sha_destination_slot = 806 + 2 * static_cast<int>(sha_destination_register);
  std::string sha256_expression;
  if (sha256_three_register_opcode == sha256h_opcode) {
    sha256_expression = "vector_sha256_hash(" + vector_value(sha_destination_register) +
        ", " + vector_value(sha_source_register) + ", " +
        vector_value(sha_third_register) + ", true)";
  } else if (sha256_three_register_opcode == sha256h2_opcode) {
    sha256_expression = "vector_sha256_hash(" + vector_value(sha_source_register) +
        ", " + vector_value(sha_destination_register) + ", " +
        vector_value(sha_third_register) + ", false)";
  } else if (sha256_three_register_opcode == sha256su1_opcode) {
    sha256_expression = "vector_sha256_su1(" + vector_value(sha_destination_register) +
        ", " + vector_value(sha_source_register) + ", " +
        vector_value(sha_third_register) + ")";
  } else if ((bits & 0xfffffc00u) == 0x5e282800u) {
    sha256_expression = "vector_sha256_su0(" + vector_value(sha_destination_register) +
        ", " + vector_value(sha_source_register) + ")";
  }
  if (!sha256_expression.empty()) {
    return translate_core::Instruction{{"{ ulong2 value = " + sha256_expression +
                        "; x[" + std::to_string(sha_destination_slot) +
                        "] = value[0]; x[" + std::to_string(sha_destination_slot + 1) +
                        "] = value[1]; }"},
                       {pc + 4}};
  }

  // This 64-bit lane form has fixed size/Q bits; reject byte/halfword forms.
  if ((bits & 0xbfe0fc00u) != 0x0ee0e000u) {
    return std::nullopt;
  }
  const int destination_register = static_cast<int>(bits & 0x1fu);
  const int left_register = static_cast<int>((bits >> 5) & 0x1fu);
  const int right_register = static_cast<int>((bits >> 16) & 0x1fu);
  const bool top = ((bits >> 30) & 1u) != 0;
  const int left_slot = 806 + 2 * left_register + (top ? 1 : 0);
  const int right_slot = 806 + 2 * right_register + (top ? 1 : 0);
  const int destination_slot = 806 + 2 * destination_register;
  return translate_core::Instruction{{"{ ulong2 product = vector_pmull(x[" +
                      std::to_string(left_slot) + "], x[" +
                      std::to_string(right_slot) + "]); x[" +
                      std::to_string(destination_slot) +
                      "] = product[0]; x[" +
                      std::to_string(destination_slot + 1) +
                      "] = product[1]; }"},
                     {pc + 4}};
}

std::string_view vector_crypto_metal_source() noexcept { return kMetalSource; }

}  // namespace arm64metal
