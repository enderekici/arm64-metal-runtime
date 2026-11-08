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
