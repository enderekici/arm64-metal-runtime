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
}
ulong sha512_ror(ulong value, ulong amount) {
    return (value >> amount) | (value << (64ul - amount));
}
ulong sha512_upper_sigma1(ulong value) {
    return sha512_ror(value, 14ul) ^ sha512_ror(value, 18ul) ^ sha512_ror(value, 41ul);
}
ulong sha512_upper_sigma0(ulong value) {
    return sha512_ror(value, 28ul) ^ sha512_ror(value, 34ul) ^ sha512_ror(value, 39ul);
}
// Operands follow the ARM pseudocode: w = Vd, x = Vn, y = Vm; [0] is bits 63:0.
ulong2 vector_sha512h(ulong2 w, ulong2 x, ulong2 y) {
    ulong high = ((y[1] & x[0]) ^ (~y[1] & x[1])) + sha512_upper_sigma1(y[1]) + w[1];
    ulong t = high + y[0];
    ulong low = ((t & y[1]) ^ (~t & x[0])) + sha512_upper_sigma1(t) + w[0];
    return ulong2(low, high);
}
ulong2 vector_sha512h2(ulong2 w, ulong2 x, ulong2 y) {
    ulong high = ((x[0] & y[1]) ^ (x[0] & y[0]) ^ (y[1] & y[0])) +
                 sha512_upper_sigma0(y[0]) + w[1];
    ulong low = ((high & y[0]) ^ (high & y[1]) ^ (y[1] & y[0])) +
                sha512_upper_sigma0(high) + w[0];
    return ulong2(low, high);
}
ulong2 vector_sha512su0(ulong2 w, ulong2 x) {
    ulong low = w[0] + (sha512_ror(w[1], 1ul) ^ sha512_ror(w[1], 8ul) ^ (w[1] >> 7));
    ulong high = w[1] + (sha512_ror(x[0], 1ul) ^ sha512_ror(x[0], 8ul) ^ (x[0] >> 7));
    return ulong2(low, high);
}
ulong2 vector_sha512su1(ulong2 w, ulong2 x, ulong2 y) {
    ulong high = w[1] + (sha512_ror(x[1], 19ul) ^ sha512_ror(x[1], 61ul) ^ (x[1] >> 6)) + y[1];
    ulong low = w[0] + (sha512_ror(x[0], 19ul) ^ sha512_ror(x[0], 61ul) ^ (x[0] >> 6)) + y[0];
    return ulong2(low, high);
}
uint sha1_rol(uint value, uint amount) {
    return (value << amount) | (value >> (32u - amount));
}
// SHA1C/SHA1P/SHA1M: kind 0 choose, 1 parity, 2 majority. x = Vd, y = Sn, w = Vm.
ulong2 vector_sha1_hash(ulong2 x_bits, ulong y_bits, ulong2 w_bits, uint kind) {
    uint4 x = as_type<uint4>(x_bits);
    uint y = uint(y_bits);
    uint4 w = as_type<uint4>(w_bits);
    for (uint e = 0; e < 4; ++e) {
        uint t = kind == 0u ? (((x.z ^ x.w) & x.y) ^ x.w)
               : kind == 1u ? (x.y ^ x.z ^ x.w)
                            : ((x.y & x.z) | ((x.y | x.z) & x.w));
        y = y + sha1_rol(x.x, 5u) + t + w[e];
        x.y = sha1_rol(x.y, 30u);
        uint top = x.w;
        x = uint4(y, x.x, x.y, x.z);
        y = top;
    }
    return as_type<ulong2>(x);
}
ulong2 vector_sha1_su1(ulong2 d_bits, ulong2 n_bits) {
    uint4 n = as_type<uint4>(n_bits);
    uint4 t = as_type<uint4>(d_bits) ^ uint4(n.y, n.z, n.w, 0u);
    uint4 result = uint4(sha1_rol(t.x, 1u), sha1_rol(t.y, 1u), sha1_rol(t.z, 1u),
                         sha1_rol(t.w, 1u) ^ sha1_rol(t.x, 2u));
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
  } else if (sha256_three_register_opcode == 0x5e000000u ||
             sha256_three_register_opcode == 0x5e001000u ||
             sha256_three_register_opcode == 0x5e002000u) {
    // SHA1C, SHA1P, SHA1M: Qd, Sn, Vm.4S.
    const auto kind = std::to_string((bits >> 12) & 3u);
    sha256_expression = "vector_sha1_hash(" + vector_value(sha_destination_register) +
        ", x[" + std::to_string(806 + 2 * static_cast<int>(sha_source_register)) + "], " +
        vector_value(sha_third_register) + ", " + kind + "u)";
  } else if (sha256_three_register_opcode == 0x5e003000u) {
    // SHA1SU0: (Vn<63:0> : Vd<127:64>) ^ Vd ^ Vm.
    const auto d = vector_value(sha_destination_register);
    sha256_expression = "(ulong2(" + d + "[1], " + vector_value(sha_source_register) +
        "[0]) ^ " + d + " ^ " + vector_value(sha_third_register) + ")";
  } else if ((bits & 0xfffffc00u) == 0x5e280800u) {
    // SHA1H Sd, Sn: ROL(Sn, 30), upper bits zero.
    sha256_expression = "ulong2(ulong((uint(x[" +
        std::to_string(806 + 2 * static_cast<int>(sha_source_register)) +
        "]) << 30u) | (uint(x[" +
        std::to_string(806 + 2 * static_cast<int>(sha_source_register)) +
        "]) >> 2u)), 0ul)";
  } else if ((bits & 0xfffffc00u) == 0x5e281800u) {
    sha256_expression = "vector_sha1_su1(" + vector_value(sha_destination_register) +
        ", " + vector_value(sha_source_register) + ")";
  }
  if (!sha256_expression.empty()) {
    return translate_core::Instruction{{"{ ulong2 value = " + sha256_expression +
                        "; x[" + std::to_string(sha_destination_slot) +
                        "] = value[0]; x[" + std::to_string(sha_destination_slot + 1) +
                        "] = value[1]; }"},
                       {pc + 4}};
  }

  // FEAT_SHA3: EOR3, BCAX (four-register), RAX1, XAR; FEAT_SHA512: SHA512H,
  // SHA512H2, SHA512SU0, SHA512SU1. All operate on 64-bit lanes.
  {
    const auto d = sha_destination_register, n = sha_source_register,
               m = sha_third_register, a = (bits >> 10) & 0x1fu;
    std::string expression;
    if ((bits & 0xffe08000u) == 0xce000000u) {
      expression = vector_value(n) + " ^ " + vector_value(m) + " ^ " + vector_value(a);
    } else if ((bits & 0xffe08000u) == 0xce200000u) {
      expression = vector_value(n) + " ^ (" + vector_value(m) + " & ~" + vector_value(a) + ")";
    } else if ((bits & 0xffe0fc00u) == 0xce608c00u) {
      expression =vector_value(n) + " ^ rotate(" + vector_value(m) + ", ulong2(1ul))";
    } else if ((bits & 0xffe0fc00u) == 0xce608000u) {
      expression = "vector_sha512h(" + vector_value(d) + ", " + vector_value(n) + ", " +
                   vector_value(m) + ")";
    } else if ((bits & 0xffe0fc00u) == 0xce608400u) {
      expression = "vector_sha512h2(" + vector_value(d) + ", " + vector_value(n) + ", " +
                   vector_value(m) + ")";
    } else if ((bits & 0xffe0fc00u) == 0xce608800u) {
      expression = "vector_sha512su1(" + vector_value(d) + ", " + vector_value(n) + ", " +
                   vector_value(m) + ")";
    } else if ((bits & 0xfffffc00u) == 0xcec08000u) {
      expression = "vector_sha512su0(" + vector_value(d) + ", " + vector_value(n) + ")";
    } else if ((bits & 0xffe00000u) == 0xce800000u) {
      const auto shift = std::to_string((bits >> 10) & 0x3fu);
      expression = "rotate(" + vector_value(n) + " ^ " + vector_value(m) + ", ulong2((64ul - " +
                   shift + "ul) & 63ul))";
    }
    if (!expression.empty()) {
      const int slot = 806 + 2 * static_cast<int>(d);
      return translate_core::Instruction{{"{ ulong2 value = " + expression + "; x[" +
                                          std::to_string(slot) + "] = value[0]; x[" +
                                          std::to_string(slot + 1) + "] = value[1]; }"},
                                         {pc + 4}};
    }
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
