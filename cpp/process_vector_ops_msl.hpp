#pragma once

#include <string_view>

namespace arm64metal {

// Per-lane SIMD helpers for the process kernel. They rely on lane_read,
// lane_add, vector_dup and the scalar fp_* helpers being defined first.
inline constexpr std::string_view processVectorOpsMslSource = R"VECTOR_OPS_MSL(
ulong vop_sext(ulong value, uint width) {
    return width == 64 ? value : (value ^ (1ul << (width - 1))) - (1ul << (width - 1));
}
ulong vop_ext(ulong value, uint width, bool is_signed) {
    return is_signed ? vop_sext(value, width) : value;
}
bool vop_less(ulong left, ulong right, bool is_signed) {
    return is_signed ? long(left) < long(right) : left < right;
}
// 0 MAX, 1 MIN, 2 CMTST
ulong2 vop_int_binary(uint op, ulong2 left, ulong2 right, uint width, uint bits, bool is_signed) {
    ulong2 target = ulong2(0);
    for (uint lane = 0; lane < bits / width; ++lane) {
        ulong a = lane_read(left, lane, width), b = lane_read(right, lane, width);
        ulong value;
        if (op == 2u) {
            value = (a & b) != 0 ? lane_mask(width) : 0ul;
        } else {
            bool a_less = vop_less(vop_ext(a, width, is_signed), vop_ext(b, width, is_signed), is_signed);
            value = (op == 0u) == a_less ? b : a;
        }
        lane_add(target, lane, width, value);
    }
    return target;
}
// 0 ADDP, 1 MAX, 2 MIN
ulong2 vop_int_pairwise(uint op, ulong2 left, ulong2 right, uint width, uint bits, bool is_signed) {
    ulong2 target = ulong2(0);
    uint half_count = bits / width / 2;
    for (uint lane = 0; lane < half_count * 2; ++lane) {
        ulong2 source = lane < half_count ? left : right;
        uint pair = (lane % half_count) * 2;
        ulong a = lane_read(source, pair, width), b = lane_read(source, pair + 1, width);
        ulong value;
        if (op == 0u) {
            value = a + b;
        } else {
            bool a_less = vop_less(vop_ext(a, width, is_signed), vop_ext(b, width, is_signed), is_signed);
            value = (op == 1u) == a_less ? b : a;
        }
        lane_add(target, lane, width, value);
    }
    return target;
}
// 0 CMGT #0, 1 CMGE #0, 2 CMLE #0, 3 CMLT #0, 4 ABS, 5 NEG
ulong2 vop_int_unary(uint op, ulong2 source, uint width, uint bits) {
    ulong2 target = ulong2(0);
    for (uint lane = 0; lane < bits / width; ++lane) {
        long value = long(vop_sext(lane_read(source, lane, width), width));
        ulong result;
        if (op == 0u) result = value > 0 ? ~0ul : 0ul;
        else if (op == 1u) result = value >= 0 ? ~0ul : 0ul;
        else if (op == 2u) result = value <= 0 ? ~0ul : 0ul;
        else if (op == 3u) result = value < 0 ? ~0ul : 0ul;
        else if (op == 4u) result = ulong(value < 0 ? -value : value);
        else result = 0ul - ulong(value);
        lane_add(target, lane, width, result);
    }
    return target;
}
// SADDLP/UADDLP/SADALP/UADALP.
ulong2 vop_addlp(ulong2 accumulator, ulong2 source, uint width, uint bits, bool is_signed, bool accumulate) {
    ulong2 target = ulong2(0);
    for (uint lane = 0; lane < bits / width / 2; ++lane) {
        ulong sum = vop_ext(lane_read(source, 2 * lane, width), width, is_signed) +
                    vop_ext(lane_read(source, 2 * lane + 1, width), width, is_signed);
        if (accumulate) sum += lane_read(accumulator, lane, 2 * width);
        lane_add(target, lane, 2 * width, sum);
    }
    return target;
}
// Three-different ops. 0 add, 1 sub, 2 abs diff, 3 mul, 4 multiply-add,
// 5 multiply-subtract, 6 absolute-difference-accumulate.
ulong2 vop_long(uint op, ulong2 accumulator, ulong2 left, ulong2 right, uint width,
                bool is_signed, bool upper, bool wide_first) {
    ulong2 target = ulong2(0);
    uint count = 64 / width;
    uint offset = upper ? count : 0;
    for (uint lane = 0; lane < count; ++lane) {
        ulong a = wide_first ? lane_read(left, lane, 2 * width)
                             : vop_ext(lane_read(left, lane + offset, width), width, is_signed);
        ulong b = vop_ext(lane_read(right, lane + offset, width), width, is_signed);
        ulong prior = lane_read(accumulator, lane, 2 * width);
        ulong difference = vop_less(a, b, is_signed) ? b - a : a - b;
        ulong value;
        if (op == 0u) value = a + b;
        else if (op == 1u) value = a - b;
        else if (op == 2u) value = difference;
        else if (op == 3u) value = a * b;
        else if (op == 4u) value = prior + a * b;
        else if (op == 5u) value = prior - a * b;
        else value = prior + difference;
        lane_add(target, lane, 2 * width, value);
    }
    return target;
}
// 0 FMUL, 2 FADD, 3 FSUB, 4..7 FMAX/FMIN/FMAXNM/FMINNM, 9 FDIV, 10 FABD
ulong2 vop_fp_binary(uint op, ulong2 left, ulong2 right, bool wide, uint bits, device ulong *state) {
    uint width = wide ? 64u : 32u;
    ulong2 target = ulong2(0);
    for (uint lane = 0; lane < bits / width; ++lane) {
        ulong a = lane_read(left, lane, width), b = lane_read(right, lane, width);
        ulong value;
        if (op == 0u || op == 2u || op == 3u) value = fp_arithmetic(op, a, b, wide, state);
        else if (op >= 4u && op <= 7u) value = fp_extreme(op, a, b, wide, state);
        else if (op == 9u) value = fp_divide(a, b, wide, state);
        else value = fp_arithmetic(3u, a, b, wide, state) & (wide ? 0x7ffffffffffffffful : 0x7ffffffful);
        lane_add(target, lane, width, value);
    }
    return target;
}
// FMLA (negate false) and FMLS (negate true).
ulong2 vop_fp_fused(ulong2 accumulator, ulong2 left, ulong2 right, bool negate, bool wide,
                    uint bits, device ulong *state) {
    uint width = wide ? 64u : 32u;
    ulong2 target = ulong2(0);
    for (uint lane = 0; lane < bits / width; ++lane) {
        ulong value = fp_fused(lane_read(accumulator, lane, width), lane_read(left, lane, width),
                               lane_read(right, lane, width), negate, false, wide, state);
        lane_add(target, lane, width, value);
    }
    return target;
}
// 2 FADDP, 4..7 FMAXP/FMINP/FMAXNMP/FMINNMP
ulong2 vop_fp_pairwise(uint op, ulong2 left, ulong2 right, bool wide, uint bits, device ulong *state) {
    uint width = wide ? 64u : 32u;
    ulong2 target = ulong2(0);
    uint half_count = bits / width / 2;
    for (uint lane = 0; lane < half_count * 2; ++lane) {
        ulong2 source = lane < half_count ? left : right;
        uint pair = (lane % half_count) * 2;
        ulong a = lane_read(source, pair, width), b = lane_read(source, pair + 1, width);
        ulong value = op == 2u ? fp_arithmetic(2u, a, b, wide, state) : fp_extreme(op, a, b, wide, state);
        lane_add(target, lane, width, value);
    }
    return target;
}
// 0 FCMEQ, 1 FCMGE, 2 FCMGT, 3 FACGE, 4 FACGT, 5 FCMLE #0, 6 FCMLT #0
ulong2 vop_fp_compare(uint kind, ulong2 left, ulong2 right, bool wide, uint bits, device ulong *state) {
    uint width = wide ? 64u : 32u;
    ulong sign = 1ul << (width - 1);
    ulong2 target = ulong2(0);
    for (uint lane = 0; lane < bits / width; ++lane) {
        ulong a = lane_read(left, lane, width), b = lane_read(right, lane, width);
        if (kind == 3u || kind == 4u) {
            a &= ~sign;
            b &= ~sign;
        }
        uint flags = kind >= 5u ? fp_compare(b, a, wide, true, state)
                                : fp_compare(a, b, wide, kind != 0u, state);
        bool result = kind == 0u ? flags == 6u
                    : (kind == 2u || kind == 4u || kind == 6u) ? flags == 2u
                    : (flags == 6u || flags == 2u);
        if (result) lane_add(target, lane, width, lane_mask(width));
    }
    return target;
}
// 0 FABS, 1 FNEG, 2 FSQRT, 3 FRINT (mode 0..4, 5 FRINTI, 6 FRINTX), 4 FCVT signed,
// 5 FCVT unsigned (mode as fp_to_integer), 6 SCVTF, 7 UCVTF
ulong2 vop_fp_unary(uint op, uint mode, ulong2 source, bool wide, uint bits, device ulong *state) {
    uint width = wide ? 64u : 32u;
    ulong sign = 1ul << (width - 1);
    ulong2 target = ulong2(0);
    for (uint lane = 0; lane < bits / width; ++lane) {
        ulong a = lane_read(source, lane, width);
        ulong value;
        if (op == 0u) value = a & ~sign;
        else if (op == 1u) value = a ^ sign;
        else if (op == 2u) value = fp_square_root(a, wide, state);
        else if (op == 3u) value = fp_round_to_integral(a, mode >= 5u ? 5u : mode, mode == 6u, wide, state);
        else if (op == 4u || op == 5u) value = fp_to_integer(a, wide, wide, op == 5u, mode, state);
        else {
            ulong raw = op == 6u ? vop_sext(a, width) : a;
            value = integer_to_float(raw, op == 6u && long(raw) < 0, wide ? 52u : 23u, state);
        }
        lane_add(target, lane, width, value);
    }
    return target;
}
// Scale a floating-point value by 2^fbits for fixed-point conversion to integer.
ulong vop_fp_scale_up(ulong bits, bool wide, uint fbits, device ulong *state) {
    uint fraction = wide ? 52u : 23u;
    ulong exponent_limit = wide ? 0x7fful : 0xfful;
    ulong exponent = (bits >> fraction) & exponent_limit;
    if (exponent == 0ul && (bits & ((1ul << fraction) - 1ul)) != 0ul)
        return fp_arithmetic(0u, bits, ((wide ? 1023ul : 127ul) + fbits) << fraction, wide, state);
    if (exponent == 0ul || exponent == exponent_limit) return bits;
    exponent += fbits;
    if (exponent >= exponent_limit) {
        ulong sign = bits & (1ul << (fraction + (wide ? 11u : 8u)));
        return sign | (exponent_limit << fraction);
    }
    return (bits & ~(exponent_limit << fraction)) | (exponent << fraction);
}
ulong vop_fp_scale_down(ulong bits, bool wide, uint fbits) {
    if (bits == 0ul) return 0ul;
    return bits - (ulong(fbits) << (wide ? 52u : 23u));
}
ulong2 vop_lane_set(ulong2 vector, uint index, uint width, ulong value) {
    uint bit = index * width;
    vector[bit >> 6] = (vector[bit >> 6] & ~(lane_mask(width) << (bit & 63))) |
                       ((value & lane_mask(width)) << (bit & 63));
    return vector;
}
)VECTOR_OPS_MSL";

}  // namespace arm64metal
