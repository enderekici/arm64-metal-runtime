#ifndef ARM64METAL_GUEST_NEON_HPP
#define ARM64METAL_GUEST_NEON_HPP

// Integer AdvSIMD lowering: three-same, three-different, two-register misc,
// permute, shift-by-immediate and by-element groups, plus FCVTL/FCVTN.
// Header-only so no build rule changes. Lane helpers lane_read/lane_add and the
// scalar FP helpers must precede neon_metal_source() in the generated source.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "translate_core.hpp"

namespace arm64metal {

inline std::string_view neon_metal_source() noexcept {
  static constexpr std::string_view source = R"METAL(
long neon_sx(ulong v, uint w) { return w == 64u ? long(v) : (long(v << (64u - w)) >> (64u - w)); }
void neon_qc(device ulong *state) { state[@GUEST_FPSR@] |= 134217728ul; }
// Shift a w-bit lane right (logical or arithmetic); count may exceed w.
ulong neon_shr(ulong value, uint count, uint w, bool is_signed) {
    ulong mask = lane_mask(w);
    if (is_signed) {
        long s = neon_sx(value, w);
        return ulong(count >= 63u ? (s < 0 ? -1l : 0l) : (s >> count)) & mask;
    }
    return count >= 64u ? 0ul : ((value >> count) & mask);
}
ulong neon_shr_round(ulong value, uint count, uint w, bool is_signed) {
    ulong result = neon_shr(value, count, w, is_signed);
    ulong round_bit = neon_shr(value, count - 1u, w, is_signed) & 1ul;
    return (result + round_bit) & lane_mask(w);
}
ulong neon_add_sat(ulong a, ulong b, uint w, bool is_signed, device ulong *state) {
    ulong mask = lane_mask(w);
    ulong sum = (a + b) & mask;
    if (!is_signed) {
        bool carry = w == 64u ? sum < a : (a + b) > mask;
        if (carry) { neon_qc(state); return mask; }
        return sum;
    }
    ulong sign = 1ul << (w - 1u);
    if (((a ^ sum) & (b ^ sum) & sign) != 0) {
        neon_qc(state);
        return (a & sign) != 0 ? sign : (sign - 1ul);
    }
    return sum;
}
ulong neon_sub_sat(ulong a, ulong b, uint w, bool is_signed, device ulong *state) {
    ulong mask = lane_mask(w);
    ulong diff = (a - b) & mask;
    if (!is_signed) {
        if (a < b) { neon_qc(state); return 0ul; }
        return diff;
    }
    ulong sign = 1ul << (w - 1u);
    if (((a ^ b) & (a ^ diff) & sign) != 0) {
        neon_qc(state);
        return (a & sign) != 0 ? sign : (sign - 1ul);
    }
    return diff;
}
// Saturate a signed 64-bit value to a w-bit signed lane (w < 64).
ulong neon_clamp_signed(long value, uint w, device ulong *state) {
    long maximum = (1l << (w - 1u)) - 1l;
    long minimum = -maximum - 1l;
    if (value > maximum) { neon_qc(state); return ulong(maximum) & lane_mask(w); }
    if (value < minimum) { neon_qc(state); return ulong(minimum) & lane_mask(w); }
    return ulong(value) & lane_mask(w);
}
// SSHL/USHL/SRSHL/URSHL/SQSHL/UQSHL/SQRSHL/UQRSHL register shifts on one lane.
ulong neon_shift_lane(ulong a, ulong control, uint w, bool is_signed, bool round, bool saturate,
                      device ulong *state) {
    int shift = int(char(control & 0xfful));
    ulong mask = lane_mask(w);
    if (shift < 0) {
        uint count = uint(-shift);
        return round ? neon_shr_round(a, count, w, is_signed) : neon_shr(a, count, w, is_signed);
    }
    uint count = uint(shift);
    if (!saturate) return count >= w ? 0ul : ((a << count) & mask);
    if (is_signed) {
        long s = neon_sx(a, w);
        bool overflow;
        long shifted = 0;
        if (count >= w) overflow = s != 0;
        else {
            shifted = s << count;
            overflow = (shifted >> count) != s;
            if (!overflow && w < 64u) {
                long maximum = (1l << (w - 1u)) - 1l;
                overflow = shifted > maximum || shifted < -maximum - 1l;
            }
        }
        if (overflow) {
            neon_qc(state);
            ulong sign = 1ul << (w - 1u);
            return s < 0 ? sign : (sign - 1ul);
        }
        return ulong(shifted) & mask;
    }
    bool overflow;
    ulong shifted = 0;
    if (count >= w) overflow = a != 0;
    else {
        shifted = a << count;
        overflow = count >= 64u ? a != 0 : ((shifted >> count) != a || shifted > mask);
    }
    if (overflow) { neon_qc(state); return mask; }
    return shifted;
}
ulong neon_sqdmulh_lane(ulong a, ulong b, uint w, bool round, device ulong *state) {
    long sa = neon_sx(a, w), sb = neon_sx(b, w);
    long minimum = -(1l << (w - 1u));
    if (sa == minimum && sb == minimum) { neon_qc(state); return (1ul << (w - 1u)) - 1ul; }
    long product = (sa * sb) * 2l;
    if (round) product += 1l << (w - 1u);
    return ulong(product >> w) & lane_mask(w);
}
ulong neon_pmul8(ulong a, ulong b) {
    ulong r = 0ul;
    for (uint bit = 0; bit < 8u; ++bit) if (((b >> bit) & 1ul) != 0) r ^= a << bit;
    return r & 0xfful;
}
ulong2 neon_three_same(uint opcode, bool u, uint size, uint q, ulong2 acc, ulong2 left,
                       ulong2 right, device ulong *state) {
    uint w = 8u << size;
    uint lanes = (q != 0 ? 128u : 64u) / w;
    ulong mask = lane_mask(w);
    ulong sign = 1ul << (w - 1u);
    bool sgn = !u;
    ulong key = sgn ? sign : 0ul;
    bool pairwise = opcode == 0x14u || opcode == 0x15u || opcode == 0x17u;
    ulong2 target = ulong2(0);
    for (uint lane = 0; lane < lanes; ++lane) {
        ulong a, b;
        if (pairwise) {
            uint half_lanes = lanes / 2u;
            ulong2 src = lane < half_lanes ? left : right;
            uint index = (lane % half_lanes) * 2u;
            a = lane_read(src, index, w);
            b = lane_read(src, index + 1u, w);
        } else {
            a = lane_read(left, lane, w);
            b = lane_read(right, lane, w);
        }
        ulong d = lane_read(acc, lane, w);
        ulong r = 0ul;
        long sa = neon_sx(a, w), sb = neon_sx(b, w);
        ulong abs_diff = (a ^ key) >= (b ^ key) ? ((a - b) & mask) : ((b - a) & mask);
        switch (opcode) {
        case 0x00u:
            r = sgn ? ulong((sa >> 1) + (sb >> 1) + long(a & b & 1ul)) & mask
                    : ((a >> 1) + (b >> 1) + (a & b & 1ul));
            break;
        case 0x01u: r = neon_add_sat(a, b, w, sgn, state); break;
        case 0x02u:
            r = sgn ? ulong((sa >> 1) + (sb >> 1) + long((a | b) & 1ul)) & mask
                    : ((a >> 1) + (b >> 1) + ((a | b) & 1ul));
            break;
        case 0x04u:
            r = sgn ? ulong((sa >> 1) - (sb >> 1) - long((~a & b) & 1ul)) & mask
                    : (((a >> 1) - (b >> 1) - ((~a & b) & 1ul)) & mask);
            break;
        case 0x05u: r = neon_sub_sat(a, b, w, sgn, state); break;
        case 0x06u: r = (a ^ key) > (b ^ key) ? mask : 0ul; break;
        case 0x07u: r = (a ^ key) >= (b ^ key) ? mask : 0ul; break;
        case 0x08u: r = neon_shift_lane(a, b, w, sgn, false, false, state); break;
        case 0x09u: r = neon_shift_lane(a, b, w, sgn, false, true, state); break;
        case 0x0au: r = neon_shift_lane(a, b, w, sgn, true, false, state); break;
        case 0x0bu: r = neon_shift_lane(a, b, w, sgn, true, true, state); break;
        case 0x0cu: case 0x14u: r = (a ^ key) >= (b ^ key) ? a : b; break;
        case 0x0du: case 0x15u: r = (a ^ key) <= (b ^ key) ? a : b; break;
        case 0x0eu: r = abs_diff; break;
        case 0x0fu: r = (d + abs_diff) & mask; break;
        case 0x10u: r = (u ? a - b : a + b) & mask; break;
        case 0x11u: r = u ? (a == b ? mask : 0ul) : ((a & b) != 0 ? mask : 0ul); break;
        case 0x12u: r = (u ? d - a * b : d + a * b) & mask; break;
        case 0x13u: r = u ? neon_pmul8(a, b) : ((a * b) & mask); break;
        case 0x16u: r = neon_sqdmulh_lane(a, b, w, u, state); break;
        case 0x17u: r = (a + b) & mask; break;
        default: break;
        }
        lane_add(target, lane, w, r);
    }
    return target;
}
ulong neon_count_sign_bits(ulong value, uint w) {
    ulong sign = (value >> (w - 1u)) & 1ul;
    uint count = 0;
    for (int bit = int(w) - 2; bit >= 0; --bit) {
        if (((value >> uint(bit)) & 1ul) != sign) break;
        ++count;
    }
    return count;
}
// Narrow one 2w-bit lane to w bits. kind: 0 truncate, 1 signed->signed, 2 unsigned->unsigned,
// 3 signed->unsigned.
ulong neon_narrow_lane(ulong value, uint wide, uint kind, device ulong *state) {
    uint narrow = wide / 2u;
    ulong mask = lane_mask(narrow);
    if (kind == 0u) return value & mask;
    if (kind == 2u) {
        if (value > mask) { neon_qc(state); return mask; }
        return value;
    }
    long s = neon_sx(value, wide);
    if (kind == 1u) return neon_clamp_signed(s, narrow, state);
    if (s < 0) { neon_qc(state); return 0ul; }
    if (ulong(s) > mask) { neon_qc(state); return mask; }
    return ulong(s);
}
ulong2 neon_misc(uint opcode, bool u, uint size, uint q, ulong2 acc, ulong2 src,
                 device ulong *state) {
    bool reverse_bits = opcode == 0x05u && u && size == 1u;  // RBIT is encoded with size 1
    if (reverse_bits) size = 0u;
    uint w = 8u << size;
    ulong mask = lane_mask(w);
    ulong sign = 1ul << (w - 1u);
    bool sgn = !u;
    ulong2 target = ulong2(0);
    uint bits = q != 0 ? 128u : 64u;
    if (opcode == 0x05u && u && !reverse_bits) {
        target = ~src;
        return q != 0 ? target : ulong2(target[0], 0ul);
    }
    if (opcode == 0x00u || opcode == 0x01u) {
        uint group = opcode == 0x01u ? 16u : (u ? 32u : 64u);
        for (uint lane = 0; lane < bits / w; ++lane)
            lane_add(target, lane, w, lane_read(src, lane ^ (group / w - 1u), w));
        return target;
    }
    if (opcode == 0x02u || opcode == 0x06u) {
        uint wide = 2u * w;
        for (uint lane = 0; lane < bits / wide; ++lane) {
            ulong a = lane_read(src, 2u * lane, w), b = lane_read(src, 2u * lane + 1u, w);
            ulong value = (sgn ? ulong(neon_sx(a, w)) + ulong(neon_sx(b, w)) : a + b);
            if (opcode == 0x06u) value += lane_read(acc, lane, wide);
            lane_add(target, lane, wide, value);
        }
        return target;
    }
    if (opcode == 0x12u || opcode == 0x14u) {
        // Narrowing: size is the destination element size; q selects the upper half.
        uint wide = 2u * w;
        uint kind = opcode == 0x12u ? (u ? 3u : 0u) : (u ? 2u : 1u);
        uint count = 64u / w;
        target = q != 0 ? ulong2(acc[0], 0ul) : ulong2(0);
        for (uint lane = 0; lane < count; ++lane)
            lane_add(target, lane + (q != 0 ? count : 0u), w,
                     neon_narrow_lane(lane_read(src, lane, wide), wide, kind, state));
        return target;
    }
    if (opcode == 0x13u) {
        uint count = 64u / w;
        for (uint lane = 0; lane < count; ++lane)
            lane_add(target, lane, 2u * w, lane_read(src, lane + (q != 0 ? count : 0u), w) << w);
        return target;
    }
    for (uint lane = 0; lane < bits / w; ++lane) {
        ulong a = lane_read(src, lane, w);
        long sa = neon_sx(a, w);
        ulong r = 0ul;
        switch (opcode) {
        case 0x03u: {
            ulong accumulator = lane_read(acc, lane, w);
            if (!u) {
                long s = neon_sx(accumulator, w);
                if (w < 64u) {
                    r = neon_clamp_signed(s + long(a), w, state);
                } else {
                    ulong low = ulong(s) + a;
                    ulong high = (s < 0 ? ~0ul : 0ul) + (low < a ? 1ul : 0ul);
                    if (high == ~0ul) {
                        if (low < 0x8000000000000000ul) { neon_qc(state); r = 0x8000000000000000ul; }
                        else r = low;
                    } else if (high == 0ul && low <= 0x7ffffffffffffffful) {
                        r = low;
                    } else {
                        neon_qc(state);
                        r = 0x7ffffffffffffffful;
                    }
                }
            } else if (w < 64u) {
                long t = long(accumulator) + sa;
                if (t < 0) { neon_qc(state); r = 0ul; }
                else if (ulong(t) > mask) { neon_qc(state); r = mask; }
                else r = ulong(t);
            } else if (sa >= 0) {
                ulong low = accumulator + ulong(sa);
                if (low < accumulator) { neon_qc(state); r = ~0ul; }
                else r = low;
            } else {
                ulong magnitude = 0ul - ulong(sa);
                if (accumulator < magnitude) { neon_qc(state); r = 0ul; }
                else r = accumulator - magnitude;
            }
            break;
        }
        case 0x04u:
            r = u ? (a == 0 ? w : (w - 1u - (63u - clz(a)))) : neon_count_sign_bits(a, w);
            break;
        case 0x05u:
            if (u) {
                for (uint bit = 0; bit < 8u; ++bit) r |= ((a >> bit) & 1ul) << (7u - bit);
            } else {
                r = popcount(uint(a));
            }
            break;
        case 0x07u:
            if (u) {
                r = a == sign ? (neon_qc(state), sign - 1ul) : ((0ul - a) & mask);
            } else {
                r = a == sign ? (neon_qc(state), sign - 1ul) : (sa < 0 ? ((0ul - a) & mask) : a);
            }
            break;
        case 0x08u: r = (u ? sa >= 0 : sa > 0) ? mask : 0ul; break;
        case 0x09u: r = (u ? sa <= 0 : a == 0) ? mask : 0ul; break;
        case 0x0au: r = sa < 0 ? mask : 0ul; break;
        case 0x0bu: r = u ? ((0ul - a) & mask) : (sa < 0 ? ((0ul - a) & mask) : a); break;
        default: break;
        }
        lane_add(target, lane, w, r);
    }
    return target;
}
ulong2 neon_permute(uint opcode, uint size, uint q, ulong2 left, ulong2 right) {
    uint w = 8u << size;
    uint lanes = (q != 0 ? 128u : 64u) / w;
    uint half_lanes = lanes / 2u;
    ulong2 target = ulong2(0);
    for (uint lane = 0; lane < lanes; ++lane) {
        ulong r = 0ul;
        uint pair = lane / 2u;
        bool odd = (lane & 1u) != 0;
        switch (opcode) {
        case 1u: case 5u: {
            uint index = 2u * (lane % half_lanes) + (opcode == 5u ? 1u : 0u);
            r = lane_read(lane < half_lanes ? left : right, index, w);
            break;
        }
        case 2u: case 6u: {
            uint index = 2u * pair + (opcode == 6u ? 1u : 0u);
            r = lane_read(odd ? right : left, index, w);
            break;
        }
        default: {
            uint index = pair + (opcode == 7u ? half_lanes : 0u);
            r = lane_read(odd ? right : left, index, w);
            break;
        }
        }
        lane_add(target, lane, w, r);
    }
    return target;
}
// Widening, wide and narrowing three-different groups.
ulong2 neon_three_diff(uint opcode, bool u, uint size, uint q, ulong2 acc, ulong2 left,
                       ulong2 right, device ulong *state) {
    uint w = 8u << size;
    uint wide = 2u * w;
    uint count = 64u / w;
    uint offset = q != 0 ? count : 0u;
    ulong wmask = lane_mask(wide);
    bool sgn = !u && opcode != 14u;
    ulong2 target = ulong2(0);
    if (opcode == 4u || opcode == 6u) {
        // ADDHN/SUBHN (U selects rounding); sources are full-width lanes.
        target = q != 0 ? ulong2(acc[0], 0ul) : ulong2(0);
        for (uint lane = 0; lane < count; ++lane) {
            ulong a = lane_read(left, lane, wide), b = lane_read(right, lane, wide);
            ulong sum = opcode == 4u ? a + b : a - b;
            if (u) sum += 1ul << (w - 1u);
            lane_add(target, lane + offset, w, (sum & wmask) >> w);
        }
        return target;
    }
    bool wide_left = opcode == 1u || opcode == 3u;
    for (uint lane = 0; lane < count; ++lane) {
        ulong a = wide_left ? lane_read(left, lane, wide) : lane_read(left, lane + offset, w);
        ulong b = lane_read(right, lane + offset, w);
        if (!wide_left && sgn) a = ulong(neon_sx(a, w)) & wmask;
        if (sgn) b = ulong(neon_sx(b, w)) & wmask;
        ulong d = lane_read(acc, lane, wide);
        ulong r = 0ul;
        switch (opcode) {
        case 0u: case 1u: r = (a + b) & wmask; break;
        case 2u: case 3u: r = (a - b) & wmask; break;
        case 5u: case 7u: {
            ulong key = sgn ? 1ul << (wide - 1u) : 0ul;
            ulong diff = (a ^ key) >= (b ^ key) ? a - b : b - a;
            r = (opcode == 5u ? d + diff : diff) & wmask;
            break;
        }
        case 8u: r = (d + a * b) & wmask; break;
        case 10u: r = (d - a * b) & wmask; break;
        case 12u: r = (a * b) & wmask; break;
        case 9u: case 11u: case 13u: {
            long sa = neon_sx(a, wide), sb = neon_sx(b, wide);
            ulong product;
            bool overflow = false;
            if (wide == 64u) {
                overflow = sa == -2147483648l && sb == -2147483648l;
                product = overflow ? 0x7ffffffffffffffful : ulong((sa * sb) * 2l);
            } else {
                long p = (sa * sb) * 2l;
                long maximum = (1l << (wide - 1u)) - 1l;
                overflow = p > maximum;
                product = overflow ? ulong(maximum) : ulong(p);
            }
            if (overflow) neon_qc(state);
            product &= wmask;
            if (opcode == 13u) r = product;
            else r = opcode == 9u ? neon_add_sat(d, product, wide, true, state)
                                  : neon_sub_sat(d, product, wide, true, state);
            break;
        }
        case 14u: {
            ulong poly = 0ul;
            for (uint bit = 0; bit < 8u; ++bit) if (((b >> bit) & 1ul) != 0) poly ^= a << bit;
            r = poly & wmask;
            break;
        }
        default: break;
        }
        lane_add(target, lane, wide, r);
    }
    return target;
}
// Shift by immediate. kind: 0 SHR, 1 SRA, 2 SHL, 3 SRI, 4 SLI, 5 SQSHL, 6 SQSHLU.
ulong2 neon_shift_imm(uint kind, bool round, bool is_signed, uint w, uint q, uint shift, ulong2 acc,
                      ulong2 src, device ulong *state) {
    uint lanes = (q != 0 ? 128u : 64u) / w;
    ulong mask = lane_mask(w);
    ulong2 target = ulong2(0);
    for (uint lane = 0; lane < lanes; ++lane) {
        ulong a = lane_read(src, lane, w);
        ulong d = lane_read(acc, lane, w);
        ulong r = 0ul;
        switch (kind) {
        case 0u: case 1u: {
            ulong shifted = round ? neon_shr_round(a, shift, w, is_signed) : neon_shr(a, shift, w, is_signed);
            r = kind == 1u ? (d + shifted) & mask : shifted;
            break;
        }
        case 2u: r = (a << shift) & mask; break;
        case 3u: {
            ulong keep = shift >= w ? mask : (~(mask >> shift) & mask);
            r = (d & keep) | (shift >= w ? 0ul : (a >> shift));
            break;
        }
        case 4u: r = (d & ((1ul << shift) - 1ul)) | ((a << shift) & mask); break;
        case 5u: r = neon_shift_lane(a, shift, w, is_signed, false, true, state); break;
        default: {
            long s = neon_sx(a, w);
            if (s < 0) { r = 0ul; if (a != 0) neon_qc(state); break; }
            ulong v = ulong(s);
            bool overflow = shift >= 64u ? v != 0 : ((v << shift) >> shift) != v || (v << shift) > mask;
            if (overflow) { neon_qc(state); r = mask; }
            else r = (v << shift) & mask;
            break;
        }
        }
        lane_add(target, lane, w, r);
    }
    return target;
}
// Shift right and narrow. kind: 0 SHRN, 1 SQSHRN/UQSHRN, 2 SQSHRUN. w is the destination width.
ulong2 neon_shift_narrow(uint kind, bool round, bool is_signed, uint w, uint q, uint shift,
                         ulong2 acc, ulong2 src, device ulong *state) {
    uint wide = 2u * w;
    uint count = 64u / w;
    ulong2 target = q != 0 ? ulong2(acc[0], 0ul) : ulong2(0);
    for (uint lane = 0; lane < count; ++lane) {
        ulong a = lane_read(src, lane, wide);
        ulong shifted = round ? neon_shr_round(a, shift, wide, is_signed) : neon_shr(a, shift, wide, is_signed);
        ulong r = neon_narrow_lane(shifted, wide, kind == 0u ? 0u : (kind == 2u ? 3u : (is_signed ? 1u : 2u)), state);
        lane_add(target, lane + (q != 0 ? count : 0u), w, r);
    }
    return target;
}
ulong2 neon_fcvtl(ulong2 src, uint q, bool double_result, device ulong *state) {
    uint count = double_result ? 2u : 4u;
    uint source_width = double_result ? 32u : 16u;
    uint destination_width = double_result ? 64u : 32u;
    ulong2 target = ulong2(0);
    for (uint lane = 0; lane < count; ++lane)
        lane_add(target, lane, destination_width,
                 fp_convert(lane_read(src, lane + (q != 0 ? count : 0u), source_width),
                            double_result ? 0u : 2u, double_result ? 1u : 0u, state));
    return target;
}
ulong2 neon_fcvtn(ulong2 acc, ulong2 src, uint q, bool double_source, device ulong *state) {
    uint count = double_source ? 2u : 4u;
    uint source_width = double_source ? 64u : 32u;
    uint destination_width = double_source ? 32u : 16u;
    ulong2 target = q != 0 ? ulong2(acc[0], 0ul) : ulong2(0);
    for (uint lane = 0; lane < count; ++lane)
        lane_add(target, lane + (q != 0 ? count : 0u), destination_width,
                 fp_convert(lane_read(src, lane, source_width), double_source ? 1u : 0u,
                            double_source ? 0u : 2u, state));
    return target;
}
)METAL";
  return source;
}

namespace neon_detail {

inline std::uint32_t bit_field(std::uint32_t bits, int start, int width) {
  return (bits >> start) & ((1u << width) - 1u);
}
inline std::string vreg(std::uint32_t index) {
  return "ulong2(x[" + std::to_string(806 + 2 * index) + "], x[" +
         std::to_string(807 + 2 * index) + "])";
}
inline translate_core::Instruction assign(std::uint32_t rd, const std::string &expression,
                                          std::int64_t pc) {
  return translate_core::Instruction(
      {"{ ulong2 value = " + expression + "; x[" + std::to_string(806 + 2 * rd) +
       "] = value[0]; x[" + std::to_string(807 + 2 * rd) + "] = value[1]; }"},
      {pc + 4});
}
inline std::string u32(std::uint32_t v) { return std::to_string(v) + "u"; }
inline std::string flag(bool v) { return v ? "true" : "false"; }

}  // namespace neon_detail

// Returns the lowered instruction or nullopt when the encoding is not in this subset.
inline std::optional<translate_core::Instruction> lower_neon(std::uint32_t bits, std::int64_t pc) {
  using namespace neon_detail;
  const auto field = [&](int start, int width) { return bit_field(bits, start, width); };
  const std::uint32_t rd = field(0, 5), rn = field(5, 5), rm = field(16, 5);
  const std::uint32_t q = field(30, 1), size = field(22, 2);
  const bool u = field(29, 1) != 0;
  const std::string d = vreg(rd), n = vreg(rn), m = vreg(rm);

  // Three same (integer opcodes only; 0x03 is the bitwise group).
  if ((bits & 0x9f200400) == 0x0e200400) {
    const auto opcode = field(11, 5);
    if (opcode == 0x03 || opcode >= 0x18) return std::nullopt;
    const bool no64 = opcode == 0x00 || opcode == 0x02 || opcode == 0x04 ||
                      (opcode >= 0x0c && opcode <= 0x10 - 1) || opcode == 0x12 || opcode == 0x13 ||
                      opcode == 0x14 || opcode == 0x15 || opcode == 0x16;
    if (size == 3 && (q == 0 || no64)) return std::nullopt;
    if (opcode == 0x13 && u && size != 0) return std::nullopt;
    if (opcode == 0x16 && size == 0) return std::nullopt;
    if (opcode == 0x17 && u) return std::nullopt;
    return assign(rd, "neon_three_same(" + u32(opcode) + ", " + flag(u) + ", " + u32(size) + ", " +
                          u32(q) + ", " + d + ", " + n + ", " + m + ", x)", pc);
  }
  // Three different.
  if ((bits & 0x9f200c00) == 0x0e200000) {
    const auto opcode = field(12, 4);
    if (size == 3 || opcode == 15) return std::nullopt;
    if ((opcode == 9 || opcode == 11 || opcode == 13) && (u || size == 0)) return std::nullopt;
    if (opcode == 14 && (u || size != 0)) return std::nullopt;
    return assign(rd, "neon_three_diff(" + u32(opcode) + ", " + flag(u) + ", " + u32(size) + ", " +
                          u32(q) + ", " + d + ", " + n + ", " + m + ", x)", pc);
  }
  // Two-register miscellaneous.
  if ((bits & 0x9f3e0c00) == 0x0e200800) {
    const auto opcode = field(12, 5);
    if (opcode == 0x17 && !u && (size & 2) == 0) {
      return assign(rd, "neon_fcvtl(" + n + ", " + u32(q) + ", " + flag(size & 1) + ", x)", pc);
    }
    if (opcode == 0x16 && !u && (size & 2) == 0) {
      return assign(rd, "neon_fcvtn(" + d + ", " + n + ", " + u32(q) + ", " + flag(size & 1) + ", x)", pc);
    }
    switch (opcode) {
    case 0x00: if (size + (u ? 1 : 0) * 0 >= 3 || (u ? size >= 2 : size >= 3)) return std::nullopt; break;
    case 0x01: if (u || size != 0) return std::nullopt; break;
    case 0x02: case 0x06: if (size == 3 && q == 0) return std::nullopt; if (size == 3) return std::nullopt; break;
    case 0x03: if (size == 3 && q == 0) return std::nullopt; break;
    case 0x04: if (size == 3) return std::nullopt; break;
    case 0x05: if (!(size == 0 || (u && size == 1))) return std::nullopt; break;
    case 0x07: case 0x0b: if (size == 3 && q == 0) return std::nullopt; break;
    case 0x08: case 0x09: if (size == 3 && q == 0) return std::nullopt; break;
    case 0x0a: if (u || (size == 3 && q == 0)) return std::nullopt; break;
    case 0x12: case 0x14: if (size == 3) return std::nullopt; break;
    case 0x13: if (!u || size == 3) return std::nullopt; break;
    default: return std::nullopt;
    }
    return assign(rd, "neon_misc(" + u32(opcode) + ", " + flag(u) + ", " + u32(size) + ", " +
                          u32(q) + ", " + d + ", " + n + ", x)", pc);
  }
  // Shift by immediate (vector).
  if ((bits & 0x9f800400) == 0x0f000400 && field(19, 4) != 0) {
    const auto opcode = field(11, 5);
    const auto immh = field(19, 4);
    const auto immhb = field(16, 7);
    const std::uint32_t esize = immh >= 8 ? 64 : immh >= 4 ? 32 : immh >= 2 ? 16 : 8;
    if (esize == 64 && q == 0) return std::nullopt;
    const auto right = 2 * esize - immhb;
    const auto left = immhb - esize;
    const std::string vq = u32(q);
    const std::string w = u32(esize);
    const std::string zero = "ulong2(0)";
    switch (opcode) {
    case 0x00: case 0x02: case 0x04: case 0x06: {
      const bool accumulate = opcode == 0x02 || opcode == 0x06;
      const bool round = opcode >= 0x04;
      return assign(rd, "neon_shift_imm(" + u32(accumulate ? 1 : 0) + ", " + flag(round) + ", " +
                            flag(!u) + ", " + w + ", " + vq + ", " + u32(right) + ", " +
                            (accumulate ? d : zero) + ", " + n + ", x)", pc);
    }
    case 0x08:
      if (!u) return std::nullopt;
      return assign(rd, "neon_shift_imm(3u, false, false, " + w + ", " + vq + ", " + u32(right) +
                            ", " + d + ", " + n + ", x)", pc);
    case 0x0a:
      return assign(rd, "neon_shift_imm(" + u32(u ? 4 : 2) + ", false, false, " + w + ", " + vq +
                            ", " + u32(left) + ", " + d + ", " + n + ", x)", pc);
    case 0x0c:
      if (!u) return std::nullopt;
      return assign(rd, "neon_shift_imm(6u, false, false, " + w + ", " + vq + ", " + u32(left) +
                            ", " + zero + ", " + n + ", x)", pc);
    case 0x0e:
      return assign(rd, "neon_shift_imm(5u, false, " + flag(!u) + ", " + w + ", " + vq + ", " +
                            u32(left) + ", " + zero + ", " + n + ", x)", pc);
    case 0x10: case 0x11: case 0x12: case 0x13: {
      if (esize == 64) return std::nullopt;
      const bool round = (opcode & 1) != 0;
      const std::uint32_t kind = (opcode <= 0x11) ? (u ? 2 : 0) : 1;
      return assign(rd, "neon_shift_narrow(" + u32(kind) + ", " + flag(round) + ", " +
                            flag(opcode <= 0x11 || !u) + ", " + w + ", " + vq + ", " +
                            u32(right) + ", " + d + ", " + n + ", x)", pc);
    }
    default:
      return std::nullopt;
    }
  }
  // Vector by element (integer).
  if ((bits & 0x9f000400) == 0x0f000000 && (size == 1 || size == 2)) {
    const auto opcode = field(12, 4);
    const std::uint32_t index = size == 1 ? (field(11, 1) << 2) | (field(21, 1) << 1) | field(20, 1)
                                          : (field(11, 1) << 1) | field(21, 1);
    const std::uint32_t reg = size == 1 ? field(16, 4) : field(16, 5);
    const std::uint32_t w = 8u << size;
    const std::string element = "vector_dup(lane_read(" + vreg(reg) + ", " + u32(index) + ", " +
                                u32(w) + "), " + u32(w) + ", 128)";
    std::uint32_t same = 0xff, diff = 0xff;
    bool flagU = u;
    if (!u) {
      switch (opcode) {
      case 0x2: diff = 8; break;
      case 0x3: diff = 9; break;
      case 0x6: diff = 10; break;
      case 0x7: diff = 11; break;
      case 0x8: same = 0x13; break;
      case 0xa: diff = 12; break;
      case 0xb: diff = 13; break;
      case 0xc: same = 0x16; flagU = false; break;
      case 0xd: same = 0x16; flagU = true; break;
      default: return std::nullopt;
      }
    } else {
      switch (opcode) {
      case 0x0: same = 0x12; flagU = false; break;
      case 0x2: diff = 8; break;
      case 0x4: same = 0x12; flagU = true; break;
      case 0x6: diff = 10; break;
      case 0xa: diff = 12; break;
      default: return std::nullopt;
      }
    }
    if (same != 0xff) {
      return assign(rd, "neon_three_same(" + u32(same) + ", " + flag(flagU) + ", " + u32(size) +
                            ", " + u32(q) + ", " + d + ", " + n + ", " + element + ", x)", pc);
    }
    return assign(rd, "neon_three_diff(" + u32(diff) + ", " + flag(u) + ", " + u32(size) + ", " +
                          u32(q) + ", " + d + ", " + n + ", " + element + ", x)", pc);
  }
  // Permute.
  if ((bits & 0xbf208c00) == 0x0e000800) {
    const auto opcode = field(12, 3);
    if (opcode == 0 || opcode == 4 || (size == 3 && q == 0)) return std::nullopt;
    return assign(rd, "neon_permute(" + u32(opcode) + ", " + u32(size) + ", " + u32(q) + ", " + n +
                          ", " + m + ")", pc);
  }
  return std::nullopt;
}

}  // namespace arm64metal

#endif  // ARM64METAL_GUEST_NEON_HPP
