#pragma once

#include <string_view>

namespace arm64metal {

inline constexpr std::string_view processSmeOpsMslSource = R"SME_OPS_MSL(
// SME storage shares low Z lanes with the existing V register bank. The
// process buffer keeps each Z register's upper six words separately.
uchar sme_z_byte(device ulong *state, uint reg, uint byte_index) {
    uint word_index = byte_index >> 3;
    ulong word = word_index < 2u
        ? state[806u + 2u * reg + word_index]
        : state[884u + 6u * reg + word_index - 2u];
    return uchar(word >> (8u * (byte_index & 7u)));
}

void sme_set_z_byte(device ulong *state, uint reg, uint byte_index, uchar value) {
    uint word_index = byte_index >> 3;
    device ulong *word = word_index < 2u
        ? state + 806u + 2u * reg + word_index
        : state + 884u + 6u * reg + word_index - 2u;
    uint shift = 8u * (byte_index & 7u);
    ulong mask = 255ul << shift;
    *word = (*word & ~mask) | (ulong(value) << shift);
}

uint sme_decode_pred_count(uint vl_elements, uint pattern) {
    if (pattern == 0u) {
        uint count = 1u;
        while ((count << 1u) <= vl_elements) count <<= 1u;
        return count;
    }
    if (pattern <= 8u) return pattern;
    if (pattern <= 13u) return 1u << (pattern - 5u);
    if (pattern == 29u) return (vl_elements / 4u) * 4u;
    if (pattern == 30u) return (vl_elements / 3u) * 3u;
    if (pattern == 31u) return vl_elements;
    return 0u;
}

void sme_ptrue(device ulong *state, uint pred, uint pattern, uint element_bytes) {
    ulong bits = 0ul;
    const uint lanes = uint(state[883]) / element_bytes;
    const uint count = min(sme_decode_pred_count(lanes, pattern), lanes);
    for (uint lane = 0u; lane < count; ++lane)
        bits |= 1ul << (lane * element_bytes);
    state[1076u + pred] = bits;
}

void sme_mov_imm_b(device ulong *state, uint reg, uint imm) {
    uchar value = uchar(imm < 128u ? imm : imm - 256u);
    for (uint lane = 0; lane < uint(state[883]); ++lane)
        sme_set_z_byte(state, reg, lane, value);
}

void sme_mov_zero_s(device ulong *state, uint reg) {
    for (uint lane = 0; lane < uint(state[883]); ++lane)
        sme_set_z_byte(state, reg, lane, uchar(0));
}

int sme_s8(uchar value) {
    return value < 128u ? int(value) : int(value) - 256;
}

// SMOPA/UMOPA ZA*.S, byte sources, SVL=64. ZA tiles interleave physical rows:
// logical .S slice r in tile t is physical row 4*r+t, eight u64 words wide.
void sme_smopa8(device ulong *state, uint tile, uint pn, uint pm,
                uint zn, uint zm, bool is_signed) {
    device ulong *za = state + 1092u;
    ulong left_pred = state[1076u + pn];
    ulong right_pred = state[1076u + pm];
    for (uint row = 0; row < 16u; ++row) {
        uint physical_row = 4u * row + tile;
        for (uint column = 0; column < 16u; ++column) {
            uint word_index = physical_row * 8u + column / 2u;
            uint shift = (column & 1u) * 32u;
            uint accumulator = uint(za[word_index] >> shift);
            for (uint k = 0; k < 4u; ++k) {
                uint left_lane = row * 4u + k;
                uint right_lane = column * 4u + k;
                if (((left_pred >> left_lane) & 1ul) == 0ul ||
                    ((right_pred >> right_lane) & 1ul) == 0ul) continue;
                uchar left = sme_z_byte(state, zn, left_lane);
                uchar right = sme_z_byte(state, zm, right_lane);
                int a = is_signed ? sme_s8(left) : int(left);
                int b = is_signed ? sme_s8(right) : int(right);
                accumulator += uint(a * b);
            }
            ulong mask = 0xfffffffful << shift;
            za[word_index] = (za[word_index] & ~mask) |
                             (ulong(accumulator) << shift);
        }
    }
}

void sme_mova_s(device ulong *state, uint zd, uint pred_reg, uint tile,
                uint slice_reg, uint offset, bool vertical) {
    uint slice = (uint(state[12u + slice_reg]) + offset) % 16u;
    ulong pred = state[1076u + pred_reg];
    for (uint lane = 0; lane < 16u; ++lane) {
        if (((pred >> (lane * 4u)) & 1ul) == 0ul) continue;
        uint physical_row = vertical ? 4u * lane + tile : 4u * slice + tile;
        uint column = vertical ? slice : lane;
        device ulong *za = state + 1092u + physical_row * 8u;
        uint value = uint(za[column / 2u] >> ((column & 1u) * 32u));
        for (uint byte = 0; byte < 4u; ++byte)
            sme_set_z_byte(state, zd, lane * 4u + byte,
                           uchar(value >> (byte * 8u)));
    }
}
)SME_OPS_MSL";

}  // namespace arm64metal
