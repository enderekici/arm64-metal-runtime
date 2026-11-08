import Foundation

let guestVectorCryptoMetalSource = """
uint aes_xtime(uint value) {
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
"""

/// Lower AES operations and the 64-bit lane forms of PMULL.
func guestVectorCrypto(_ bits: UInt32, pc: Int) -> Instruction? {
    let opcode = bits & 0xffff_fc00
    if [0x4e28_4800, 0x4e28_5800, 0x4e28_6800, 0x4e28_7800].contains(opcode) {
        let destination = 806 + 2 * Int(bits & 0x1f)
        let source = 806 + 2 * Int((bits >> 5) & 0x1f)
        let input = "ulong2(x[\(source)], x[\(source + 1)])"
        let inverse = bits & 0x1000 != 0
        let expression = bits & 0x2000 == 0
            ? "vector_aes_round(ulong2(x[\(destination)], x[\(destination + 1)]), \(input), \(inverse))"
            : "vector_aes_mix(\(input), \(inverse))"
        return Instruction(code: [
            "{ ulong2 value = \(expression); x[\(destination)] = value[0]; x[\(destination + 1)] = value[1]; }",
        ], next: [pc + 4])
    }
    // This 64-bit lane form has fixed size/Q bits; reject byte/halfword forms.
    guard bits & 0xbfe0_fc00 == 0x0ee0_e000 else { return nil }
    let rd = Int(bits & 0x1f)
    let rn = Int((bits >> 5) & 0x1f)
    let rm = Int((bits >> 16) & 0x1f)
    let top = (bits >> 30) & 1 != 0
    let left = 806 + 2 * rn + (top ? 1 : 0)
    let right = 806 + 2 * rm + (top ? 1 : 0)
    let destination = 806 + 2 * rd
    return Instruction(code: [
        "{ ulong2 product = vector_pmull(x[\(left)], x[\(right)]); x[\(destination)] = product[0]; x[\(destination + 1)] = product[1]; }",
    ], next: [pc + 4])
}
