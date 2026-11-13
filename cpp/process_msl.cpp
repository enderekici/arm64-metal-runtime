#include "process_msl.hpp"
#include "guest_neon.hpp"
#include "guest_memory_msl.hpp"

#include "guest_vector_crypto.hpp"
#include "integer_ops_shared.h"
#include "process_host.hpp"
#include "process_vector_ops_msl.hpp"

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace arm64metal::process_msl {
namespace {

inline constexpr std::uint64_t guestTokenTag = 0xa64e000000000000ULL;
inline constexpr std::uint64_t guestDataAddressMask = (std::uint64_t{1} << 47) - 1;
inline constexpr std::uint64_t guestDataTokenBuckets = 255;
inline constexpr std::uint64_t guestTokenIndexShift = 3;

constexpr std::string_view kProcessMetalTemplate = R"PROCESS_MSL(
    #include <metal_stdlib>
    using namespace metal;
    @TYPED_INTEGER_MSL_SOURCE@
    // Build IEEE-754 scalar bits with integers, including binary64 on Metal.
    ulong integer_to_float(ulong raw, bool negative, uint fraction, device ulong *state) {
        ulong magnitude = negative ? 0ul - raw : raw;
        if (magnitude == 0) return 0ul;
        uint exponent = 63u - clz(magnitude);
        ulong significand;
        if (exponent <= fraction) {
            significand = magnitude << (fraction - exponent);
        } else {
            uint shift = exponent - fraction;
            significand = magnitude >> shift;
            ulong remainder = magnitude & ((1ul << shift) - 1ul);
            if (remainder != 0) {
                state[@GUEST_FPSR@] |= 16ul;
                ulong halfway = 1ul << (shift - 1);
                uint mode = uint(state[@GUEST_FPCR@] >> 22) & 3u;
                bool up = (mode == 1u && !negative) || (mode == 2u && negative)
                       || (mode == 0u && (remainder > halfway || (remainder == halfway && (significand & 1ul))));
                significand += ulong(up);
            }
        }
        uint bias = fraction == 52u ? 1023u : 127u;
        uint sign = fraction == 52u ? 63u : 31u;
        return (ulong(negative) << sign) | ((ulong(exponent + bias - 1u) << fraction) + significand);
    }
    // FCVT between binary32 and binary64 with integer arithmetic.
    ulong fp_convert_precision(ulong source, bool to_double, device ulong *state) {
        ulong fpcr = state[@GUEST_FPCR@];
        if (to_double) {
            ulong sign = (source >> 31) & 1ul;
            ulong exponent = (source >> 23) & 0xfful;
            ulong fraction = source & 0x7ffffful;
            if (exponent == 0xfful) {
                if (fraction == 0) return (sign << 63) | (0x7fful << 52);
                if ((fraction & 0x400000ul) == 0) state[@GUEST_FPSR@] |= 1ul;
                if ((fpcr & (1ul << 25)) != 0) return 0x7ff8000000000000ul;
                return (sign << 63) | (0x7fful << 52) | ((fraction | 0x400000ul) << 29);
            }
            if (exponent == 0) {
                if (fraction == 0) return sign << 63;
                if ((fpcr & (1ul << 24)) != 0) {
                    state[@GUEST_FPSR@] |= 128ul;
                    return sign << 63;
                }
                uint top = 31u - clz(uint(fraction));
                ulong result_fraction = (fraction << (52u - top)) & ((1ul << 52) - 1ul);
                return (sign << 63) | (ulong(top + 874u) << 52) | result_fraction;
            }
            return (sign << 63) | ((exponent + 896ul) << 52) | (fraction << 29);
        }
        ulong sign = source >> 63;
        ulong exponent = (source >> 52) & 0x7fful;
        ulong fraction = source & ((1ul << 52) - 1ul);
        ulong sign_bit = sign << 31;
        if (exponent == 0x7fful) {
            if (fraction == 0) return sign_bit | 0x7f800000ul;
            if ((fraction & (1ul << 51)) == 0) state[@GUEST_FPSR@] |= 1ul;
            if ((fpcr & (1ul << 25)) != 0) return 0x7fc00000ul;
            return sign_bit | 0x7f800000ul | ((fraction | (1ul << 51)) >> 29);
        }
        if (exponent == 0 && fraction == 0) return sign_bit;
        if (exponent == 0 && (fpcr & (1ul << 24)) != 0) {
            state[@GUEST_FPSR@] |= 128ul;
            return sign_bit;
        }
        long unbiased = exponent == 0 ? -1022l : long(exponent) - 1023l;
        ulong significand = exponent == 0 ? fraction : (fraction | (1ul << 52));
        uint mode = uint(fpcr >> 22) & 3u;
        bool tiny = unbiased < -126l;
        if (tiny && (fpcr & (1ul << 24)) != 0) {
            state[@GUEST_FPSR@] |= 8ul;
            return sign_bit;
        }
        bool overflow = unbiased >= 128l;
        ulong kept = 0;
        if (!overflow) {
            ulong drop = tiny ? 29ul + ulong(-126l - unbiased) : 29ul;
            ulong remainder_nonzero = 0;
            bool up = false;
            if (drop > 60ul) {
                remainder_nonzero = 1ul;
                up = (mode == 1u && sign == 0) || (mode == 2u && sign != 0);
            } else {
                kept = significand >> drop;
                ulong remainder = significand & ((1ul << drop) - 1ul);
                ulong halfway = 1ul << (drop - 1ul);
                remainder_nonzero = remainder != 0 ? 1ul : 0ul;
                up = (mode == 0u && (remainder > halfway || (remainder == halfway && (kept & 1ul) != 0)))
                  || (mode == 1u && sign == 0 && remainder != 0)
                  || (mode == 2u && sign != 0 && remainder != 0);
            }
            if (remainder_nonzero != 0) {
                state[@GUEST_FPSR@] |= 16ul;
                if (tiny) state[@GUEST_FPSR@] |= 8ul;
            }
            kept += up ? 1ul : 0ul;
            ulong bits = tiny ? kept : ((ulong(unbiased + 127l) << 23) + (kept - (1ul << 23)));
            if (bits < 0x7f800000ul) return sign_bit | bits;
        }
        state[@GUEST_FPSR@] |= 20ul;
        bool max_finite = mode == 3u || (mode == 1u && sign != 0) || (mode == 2u && sign == 0);
        return sign_bit | (max_finite ? 0x7f7ffffful : 0x7f800000ul);
    }
    // Scalar FP arithmetic on integers (Metal has no binary64). Value of an
    // operand is significand/2^63 * 2^exponent with bit 63 set.
    struct FpOperand { ulong sign; int exponent; ulong significand; uint kind; };
    ulong lane_mask_bits(uint width) { return ~0ul >> (64u - width); }
    // Format codes: 0 binary32, 1 binary64, 2 binary16.
    uint fp_fbits(uint f) { return f == 1u ? 52u : (f == 0u ? 23u : 10u); }
    uint fp_ebits(uint f) { return f == 1u ? 11u : (f == 0u ? 8u : 5u); }
    int fp_bias(uint f) { return f == 1u ? 1023 : (f == 0u ? 127 : 15); }
    uint fp_sbit(uint f) { return fp_fbits(f) + fp_ebits(f); }
    ulong fp_vmask(uint f) { return f == 1u ? ~0ul : (f == 0u ? 0xfffffffful : 0xfffful); }
    ulong fp_infinity_bits(uint f) { return ((1ul << fp_ebits(f)) - 1ul) << fp_fbits(f); }
    ulong fp_default_nan_bits(uint f) { return fp_infinity_bits(f) | (1ul << (fp_fbits(f) - 1u)); }
    bool fp_flush(uint f, ulong fpcr) { return (fpcr & (1ul << (f == 2u ? 19u : 24u))) != 0; }
    uint fp_kind(ulong bits, uint wide) {
        uint fraction_bits = fp_fbits(wide);
        uint exponent_bits = fp_ebits(wide);
        ulong exponent = (bits >> fraction_bits) & ((1ul << exponent_bits) - 1ul);
        ulong fraction = bits & ((1ul << fraction_bits) - 1ul);
        if (exponent != (1ul << exponent_bits) - 1ul) return 1u;
        if (fraction == 0) return 2u;
        return ((fraction >> (fraction_bits - 1u)) & 1ul) != 0 ? 3u : 4u;
    }
    FpOperand fp_unpack(ulong bits, uint wide, device ulong *state) {
        uint fraction_bits = fp_fbits(wide);
        uint exponent_bits = fp_ebits(wide);
        int bias = fp_bias(wide);
        bits &= fp_vmask(wide);
        FpOperand result;
        result.sign = (bits >> (fraction_bits + exponent_bits)) & 1ul;
        ulong exponent = (bits >> fraction_bits) & ((1ul << exponent_bits) - 1ul);
        ulong fraction = bits & ((1ul << fraction_bits) - 1ul);
        result.exponent = 0;
        result.significand = fraction;
        if (exponent == (1ul << exponent_bits) - 1ul) {
            result.kind = fraction == 0 ? 2u :
                (((fraction >> (fraction_bits - 1u)) & 1ul) != 0 ? 3u : 4u);
            return result;
        }
        if (exponent == 0) {
            result.significand = 0;
            result.kind = 0u;
            if (fraction == 0) return result;
            if (fp_flush(wide, state[@GUEST_FPCR@])) {
                state[@GUEST_FPSR@] |= 128ul;
                return result;
            }
            uint top = 63u - clz(fraction);
            result.kind = 1u;
            result.exponent = int(top) + (1 - bias - int(fraction_bits));
            result.significand = fraction << (63u - top);
            return result;
        }
        result.kind = 1u;
        result.exponent = int(exponent) - bias;
        result.significand = (fraction | (1ul << fraction_bits)) << (63u - fraction_bits);
        return result;
    }
    ulong fp_sign_bit(ulong sign, uint wide) { return sign << (fp_sbit(wide)); }
    ulong fp_default_nan(uint wide) { return fp_default_nan_bits(wide); }
    ulong fp_infinity(ulong sign, uint wide) {
        return fp_sign_bit(sign, wide) | (fp_infinity_bits(wide));
    }
    ulong fp_quiet(ulong bits, uint wide, device ulong *state) {
        if ((state[@GUEST_FPCR@] & (1ul << 25)) != 0) return fp_default_nan(wide);
        return bits | (1ul << (fp_fbits(wide) - 1u));
    }
    // ARM FPProcessNaNs: signaling first, then quiet, operand order preserved.
    bool fp_process_nans(ulong left, ulong right, uint wide, device ulong *state,
                         thread ulong &result) {
        ulong mask = fp_vmask(wide);
        left &= mask;
        right &= mask;
        uint left_kind = fp_kind(left, wide);
        uint right_kind = fp_kind(right, wide);
        if (left_kind < 3u && right_kind < 3u) return false;
        if (left_kind == 4u || right_kind == 4u) state[@GUEST_FPSR@] |= 1ul;
        if ((state[@GUEST_FPCR@] & (1ul << 24)) != 0) {
            uint fraction_bits = wide ? 52u : 23u;
            ulong exponent_field = (1ul << (wide ? 11u : 8u)) - 1ul;
            ulong fraction_field = (1ul << fraction_bits) - 1ul;
            if ((((left >> fraction_bits) & exponent_field) == 0 && (left & fraction_field) != 0) ||
                (((right >> fraction_bits) & exponent_field) == 0 && (right & fraction_field) != 0))
                state[@GUEST_FPSR@] |= 128ul;
        }
        ulong source = left_kind == 4u ? left : right_kind == 4u ? right :
                       left_kind == 3u ? left : right;
        result = fp_quiet(source, wide, state);
        return true;
    }
    // Round value = significand/2^63 * 2^exponent (bit 63 set, bit 0 sticky).
    ulong fp_pack(ulong sign, int exponent, ulong significand, uint wide, device ulong *state) {
        uint precision = fp_fbits(wide) + 1u;
        int minimum = 1 - fp_bias(wide);
        uint fraction_bits = fp_fbits(wide);
        int bias = fp_bias(wide);
        ulong fpcr = state[@GUEST_FPCR@];
        uint mode = uint(fpcr >> 22) & 3u;
        ulong sign_bit = fp_sign_bit(sign, wide);
        bool tiny = exponent < minimum;
        if (tiny && fp_flush(wide, fpcr)) {
            state[@GUEST_FPSR@] |= 8ul;
            return sign_bit;
        }
        uint shift = 64u - precision;
        if (tiny) {
            ulong extra = ulong(minimum - exponent);
            shift += extra >= 64ul ? 64u : uint(extra);
        }
        ulong kept = 0;
        ulong remainder;
        ulong halfway;
        if (shift > 64u) {
            remainder = 1ul;
            halfway = 2ul;
        } else if (shift == 64u) {
            remainder = significand;
            halfway = 1ul << 63;
        } else {
            kept = significand >> shift;
            remainder = significand & ((1ul << shift) - 1ul);
            halfway = 1ul << (shift - 1u);
        }
        bool up = (mode == 0u && (remainder > halfway || (remainder == halfway && (kept & 1ul) != 0)))
               || (mode == 1u && sign == 0 && remainder != 0)
               || (mode == 2u && sign != 0 && remainder != 0);
        if (remainder != 0) {
            state[@GUEST_FPSR@] |= 16ul;
            if (tiny) state[@GUEST_FPSR@] |= 8ul;
        }
        kept += up ? 1ul : 0ul;
        ulong infinity = fp_infinity_bits(wide);
        ulong bits;
        if (tiny) {
            bits = kept;
        } else {
            bits = (ulong(exponent + bias) << fraction_bits) + (kept - (1ul << (precision - 1u)));
            if (bits >= infinity) {
                state[@GUEST_FPSR@] |= 20ul;
                bool max_finite = mode == 3u || (mode == 1u && sign != 0) || (mode == 2u && sign == 0);
                return sign_bit | (max_finite ? infinity - 1ul : infinity);
            }
        }
        return sign_bit | bits;
    }
    ulong fp_add_operands(FpOperand a, FpOperand b, uint wide, device ulong *state) {
        uint mode = uint(state[@GUEST_FPCR@] >> 22) & 3u;
        if (a.kind == 2u || b.kind == 2u) {
            if (a.kind == 2u && b.kind == 2u && a.sign != b.sign) {
                state[@GUEST_FPSR@] |= 1ul;
                return fp_default_nan(wide);
            }
            return fp_infinity(a.kind == 2u ? a.sign : b.sign, wide);
        }
        if (a.kind == 0u && b.kind == 0u)
            return fp_sign_bit(a.sign == b.sign ? a.sign : (mode == 2u ? 1ul : 0ul), wide);
        if (a.kind == 0u) return fp_pack(b.sign, b.exponent, b.significand, wide, state);
        if (b.kind == 0u) return fp_pack(a.sign, a.exponent, a.significand, wide, state);
        if (a.exponent < b.exponent ||
            (a.exponent == b.exponent && a.significand < b.significand)) {
            FpOperand swap = a;
            a = b;
            b = swap;
        }
        uint distance = uint(a.exponent - b.exponent);
        ulong larger = a.significand >> 1;
        ulong smaller = b.significand >> 1;
        ulong aligned = distance >= 63u ? (smaller != 0 ? 1ul : 0ul)
            : ((smaller >> distance) | (((smaller & ((1ul << distance) - 1ul)) != 0) ? 1ul : 0ul));
        ulong sum;
        if (a.sign == b.sign) {
            sum = larger + aligned;
        } else {
            sum = larger - aligned;
            if (sum == 0) return fp_sign_bit(mode == 2u ? 1ul : 0ul, wide);
        }
        uint zeros = clz(sum);
        return fp_pack(a.sign, a.exponent + 1 - int(zeros), sum << zeros, wide, state);
    }
    ulong fp_multiply_operands(FpOperand a, FpOperand b, uint wide, device ulong *state) {
        ulong sign = a.sign ^ b.sign;
        if ((a.kind == 2u && b.kind == 0u) || (a.kind == 0u && b.kind == 2u)) {
            state[@GUEST_FPSR@] |= 1ul;
            return fp_default_nan(wide);
        }
        if (a.kind == 2u || b.kind == 2u) return fp_infinity(sign, wide);
        if (a.kind == 0u || b.kind == 0u) return fp_sign_bit(sign, wide);
        ulong high = mulhi(a.significand, b.significand);
        ulong low = a.significand * b.significand;
        int exponent = a.exponent + b.exponent + 1;
        if ((high >> 63) == 0) {
            high = (high << 1) | (low >> 63);
            low <<= 1;
            exponent -= 1;
        }
        return fp_pack(sign, exponent, high | (low != 0 ? 1ul : 0ul), wide, state);
    }
    // 0 FMUL, 2 FADD, 3 FSUB, 8 FNMUL
    ulong fp_arithmetic(uint operation, ulong left, ulong right, uint wide, device ulong *state) {
        ulong nan_result;
        if (fp_process_nans(left, right, wide, state, nan_result))
            return operation == 8u ? nan_result ^ fp_sign_bit(1ul, wide) : nan_result;
        FpOperand a = fp_unpack(left, wide, state);
        FpOperand b = fp_unpack(right, wide, state);
        if (operation == 2u) return fp_add_operands(a, b, wide, state);
        if (operation == 3u) {
            b.sign ^= 1ul;
            return fp_add_operands(a, b, wide, state);
        }
        ulong product = fp_multiply_operands(a, b, wide, state);
        return operation == 8u ? product ^ fp_sign_bit(1ul, wide) : product;
    }
    ulong fp_flushed(ulong bits, uint wide, device ulong *state) {
        FpOperand value = fp_unpack(bits, wide, state);
        if (value.kind == 0u) return fp_sign_bit(value.sign, wide);
        return bits & (fp_vmask(wide));
    }
    ulong fp_order_key(ulong bits, uint wide) {
        ulong sign = (1ul << fp_sbit(wide));
        return (bits & sign) != 0 ? ~bits & (fp_vmask(wide)) : bits | sign;
    }
    // 4 FMAX, 5 FMIN, 6 FMAXNM, 7 FMINNM
    ulong fp_extreme(uint operation, ulong left, ulong right, uint wide, device ulong *state) {
        ulong mask = fp_vmask(wide);
        left &= mask;
        right &= mask;
        if (operation >= 6u) {
            uint left_kind = fp_kind(left, wide);
            uint right_kind = fp_kind(right, wide);
            if (left_kind == 3u && right_kind < 3u) return fp_flushed(right, wide, state);
            if (right_kind == 3u && left_kind < 3u) return fp_flushed(left, wide, state);
        }
        ulong nan_result;
        if (fp_process_nans(left, right, wide, state, nan_result)) return nan_result;
        left = fp_flushed(left, wide, state);
        right = fp_flushed(right, wide, state);
        bool pick_left = (operation & 1u) == 0u
            ? fp_order_key(left, wide) >= fp_order_key(right, wide)
            : fp_order_key(left, wide) <= fp_order_key(right, wide);
        return pick_left ? left : right;
    }
    ulong fp_square_root(ulong bits, uint wide, device ulong *state) {
        ulong mask = fp_vmask(wide);
        bits &= mask;
        uint kind = fp_kind(bits, wide);
        if (kind >= 3u) {
            if (kind == 4u) state[@GUEST_FPSR@] |= 1ul;
            return fp_quiet(bits, wide, state);
        }
        FpOperand value = fp_unpack(bits, wide, state);
        if (value.kind == 0u) return fp_sign_bit(value.sign, wide);
        if (value.sign != 0) {
            state[@GUEST_FPSR@] |= 1ul;
            return fp_default_nan(wide);
        }
        if (value.kind == 2u) return fp_infinity(0ul, wide);
        ulong high;
        ulong low;
        if ((value.exponent & 1) == 0) {
            high = value.significand >> 1;
            low = value.significand << 63;
        } else {
            high = value.significand;
            low = 0ul;
        }
        ulong root = 0ul;
        for (int bit = 63; bit >= 0; --bit) {
            ulong trial = root | (1ul << uint(bit));
            ulong trial_high = mulhi(trial, trial);
            ulong trial_low = trial * trial;
            if (trial_high < high || (trial_high == high && trial_low <= low)) root = trial;
        }
        bool inexact = mulhi(root, root) != high || root * root != low;
        return fp_pack(0ul, value.exponent >> 1, root | (inexact ? 1ul : 0ul), wide, state);
    }
    // Fused multiply-add: result = (negate_addend ? -a : a) + (negate_product ? -(n*m) : n*m)
    // with one rounding, using 128-bit significands.
    ulong fp_fused(ulong a_bits, ulong n_bits, ulong m_bits, bool negate_product,
                   bool negate_addend, uint wide, device ulong *state) {
        if (negate_addend) a_bits ^= fp_sign_bit(1ul, wide);
        if (negate_product) n_bits ^= fp_sign_bit(1ul, wide);
        FpOperand a = fp_unpack(a_bits, wide, state);
        FpOperand n = fp_unpack(n_bits, wide, state);
        FpOperand m = fp_unpack(m_bits, wide, state);
        bool invalid_product = (n.kind == 2u && m.kind == 0u) || (n.kind == 0u && m.kind == 2u);
        if (a.kind >= 3u || n.kind >= 3u || m.kind >= 3u) {
            if (a.kind == 3u && invalid_product) {
                state[@GUEST_FPSR@] |= 1ul;
                return fp_default_nan(wide);
            }
            if (a.kind == 4u || n.kind == 4u || m.kind == 4u) state[@GUEST_FPSR@] |= 1ul;
            ulong source = a.kind == 4u ? a_bits : n.kind == 4u ? n_bits : m.kind == 4u ? m_bits :
                           a.kind == 3u ? a_bits : n.kind == 3u ? n_bits : m_bits;
            return fp_quiet(source & (fp_vmask(wide)), wide, state);
        }
        if (invalid_product) {
            state[@GUEST_FPSR@] |= 1ul;
            return fp_default_nan(wide);
        }
        uint mode = uint(state[@GUEST_FPCR@] >> 22) & 3u;
        ulong product_sign = n.sign ^ m.sign;
        if (n.kind == 2u || m.kind == 2u) {
            if (a.kind == 2u && a.sign != product_sign) {
                state[@GUEST_FPSR@] |= 1ul;
                return fp_default_nan(wide);
            }
            return fp_infinity(product_sign, wide);
        }
        if (a.kind == 2u) return fp_infinity(a.sign, wide);
        bool product_zero = n.kind == 0u || m.kind == 0u;
        if (product_zero) {
            if (a.kind == 0u)
                return fp_sign_bit(a.sign == product_sign ? a.sign : (mode == 2u ? 1ul : 0ul), wide);
            return fp_pack(a.sign, a.exponent, a.significand, wide, state);
        }
        ulong p_high = mulhi(n.significand, m.significand);
        ulong p_low = n.significand * m.significand;
        int p_exponent = n.exponent + m.exponent + 1;
        if ((p_high >> 63) == 0) {
            p_high = (p_high << 1) | (p_low >> 63);
            p_low <<= 1;
            p_exponent -= 1;
        }
        if (a.kind == 0u) {
            return fp_pack(product_sign, p_exponent, p_high | (p_low != 0 ? 1ul : 0ul), wide, state);
        }
        ulong a_high = a.significand;
        ulong a_low = 0ul;
        int a_exponent = a.exponent;
        ulong sign_big = product_sign;
        ulong big_high = p_high, big_low = p_low, small_high = a_high, small_low = a_low;
        int big_exponent = p_exponent, small_exponent = a_exponent;
        ulong small_sign = a.sign;
        if (a_exponent > p_exponent || (a_exponent == p_exponent &&
            (a_high > p_high || (a_high == p_high && a_low > p_low)))) {
            sign_big = a.sign;
            big_high = a_high; big_low = a_low; big_exponent = a_exponent;
            small_high = p_high; small_low = p_low; small_exponent = p_exponent;
            small_sign = product_sign;
        }
        big_low = (big_low >> 1) | (big_high << 63);
        big_high >>= 1;
        small_low = (small_low >> 1) | (small_high << 63);
        small_high >>= 1;
        uint distance = uint(big_exponent - small_exponent);
        ulong sticky = 0ul;
        if (distance >= 128u) {
            sticky = (small_high | small_low) != 0 ? 1ul : 0ul;
            small_high = 0ul;
            small_low = 0ul;
        } else if (distance >= 64u) {
            uint extra = distance - 64u;
            sticky = (small_low != 0 || (small_high & ((1ul << extra) - 1ul)) != 0) ? 1ul : 0ul;
            small_low = small_high >> extra;
            small_high = 0ul;
        } else if (distance > 0u) {
            sticky = (small_low & ((1ul << distance) - 1ul)) != 0 ? 1ul : 0ul;
            small_low = (small_low >> distance) | (small_high << (64u - distance));
            small_high >>= distance;
        }
        small_low |= sticky;
        ulong high;
        ulong low;
        if (sign_big == small_sign) {
            low = big_low + small_low;
            high = big_high + small_high + (low < big_low ? 1ul : 0ul);
        } else {
            low = big_low - small_low;
            high = big_high - small_high - (big_low < small_low ? 1ul : 0ul);
            if (high == 0 && low == 0) return fp_sign_bit(mode == 2u ? 1ul : 0ul, wide);
        }
        uint zeros = high != 0 ? clz(high) : 64u + clz(low);
        if (zeros >= 64u) {
            high = low << (zeros - 64u);
            low = 0ul;
        } else if (zeros > 0u) {
            high = (high << zeros) | (low >> (64u - zeros));
            low <<= zeros;
        }
        return fp_pack(sign_big, big_exponent + 1 - int(zeros),
                       high | (low != 0 ? 1ul : 0ul), wide, state);
    }
    // FCVT between any two of binary16/32/64 (format codes as fp_fbits).
    ulong fp_convert(ulong bits, uint source, uint destination, device ulong *state) {
        bits &= fp_vmask(source);
        ulong fpcr = state[@GUEST_FPCR@];
        uint kind = fp_kind(bits, source);
        ulong sign = (bits >> fp_sbit(source)) & 1ul;
        if (kind >= 3u) {
            if (kind == 4u) state[@GUEST_FPSR@] |= 1ul;
            if ((fpcr & (1ul << 25)) != 0) return fp_default_nan(destination);
            uint source_fraction = fp_fbits(source);
            uint destination_fraction = fp_fbits(destination);
            ulong fraction = bits & ((1ul << (source_fraction - 1u)) - 1ul);
            fraction = destination_fraction >= source_fraction
                ? fraction << (destination_fraction - source_fraction)
                : fraction >> (source_fraction - destination_fraction);
            return fp_sign_bit(sign, destination) | fp_infinity_bits(destination) |
                   (1ul << (destination_fraction - 1u)) | fraction;
        }
        FpOperand value = fp_unpack(bits, source, state);
        if (value.kind == 0u) return fp_sign_bit(value.sign, destination);
        if (value.kind == 2u) return fp_infinity(value.sign, destination);
        return fp_pack(value.sign, value.exponent, value.significand, destination, state);
    }
    // SCVTF/UCVTF: (negative ? -magnitude : magnitude) / 2^fixed_bits.
    ulong fp_from_integer(ulong magnitude, ulong sign, uint fixed_bits, uint format, device ulong *state) {
        if (magnitude == 0) return 0ul;
        uint zeros = clz(magnitude);
        return fp_pack(sign, 63 - int(zeros) - int(fixed_bits), magnitude << zeros, format, state);
    }
    // Round a finite nonzero value to an integer magnitude. Rounding modes:
    // 0 ties-even, 1 +inf, 2 -inf, 3 zero, 4 ties-away. 'huge' when >= 2^64.
    ulong fp_round_magnitude(FpOperand value, uint mode, thread bool &inexact, thread bool &huge) {
        inexact = false;
        huge = false;
        if (value.exponent >= 64) {
            huge = true;
            return 0ul;
        }
        if (value.exponent == 63) return value.significand;
        uint shift = uint(63 - value.exponent);
        ulong kept = 0;
        ulong remainder;
        ulong halfway;
        if (shift > 64u) {
            remainder = 1ul;
            halfway = 2ul;
        } else if (shift == 64u) {
            remainder = value.significand;
            halfway = 1ul << 63;
        } else {
            kept = value.significand >> shift;
            remainder = shift == 0u ? 0ul : value.significand & ((1ul << shift) - 1ul);
            halfway = shift == 0u ? 0ul : 1ul << (shift - 1u);
        }
        inexact = remainder != 0;
        bool up = (mode == 0u && (remainder > halfway || (remainder == halfway && (kept & 1ul) != 0)))
               || (mode == 1u && value.sign == 0 && remainder != 0)
               || (mode == 2u && value.sign != 0 && remainder != 0)
               || (mode == 4u && remainder != 0 && remainder >= halfway);
        return kept + (up ? 1ul : 0ul);
    }
    // FRINT*: mode 0..4 as above, 5 = current FPCR mode; exact flag only for FRINTX.
    ulong fp_round_to_integral(ulong bits, uint mode, bool signal_inexact, uint wide, device ulong *state) {
        bits &= fp_vmask(wide);
        uint kind = fp_kind(bits, wide);
        if (kind >= 3u) {
            if (kind == 4u) state[@GUEST_FPSR@] |= 1ul;
            return fp_quiet(bits, wide, state);
        }
        FpOperand value = fp_unpack(bits, wide, state);
        if (value.kind != 1u) return value.kind == 2u ? bits : fp_sign_bit(value.sign, wide);
        if (mode == 5u) mode = uint(state[@GUEST_FPCR@] >> 22) & 3u;
        bool inexact;
        bool huge;
        ulong integer = fp_round_magnitude(value, mode, inexact, huge);
        if (huge) return bits;
        if (inexact && signal_inexact) state[@GUEST_FPSR@] |= 16ul;
        if (integer == 0) return fp_sign_bit(value.sign, wide);
        uint zeros = clz(integer);
        return fp_pack(value.sign, 63 - int(zeros), integer << zeros, wide, state);
    }
    // FCVT{N,P,M,Z,A}{S,U} to a W or X destination (value returned zero-extended).
    ulong fp_to_integer_fixed(ulong bits, uint wide_source, uint destination_bits, bool is_unsigned,
                              uint mode, uint fixed_bits, device ulong *state) {
        bits &= fp_vmask(wide_source);
        uint kind = fp_kind(bits, wide_source);
        ulong width_mask = lane_mask_bits(destination_bits);
        ulong signed_limit = 1ul << (destination_bits - 1u);
        if (kind >= 3u) {
            state[@GUEST_FPSR@] |= 1ul;
            return 0ul;
        }
        FpOperand value = fp_unpack(bits, wide_source, state);
        bool negative = value.sign != 0;
        if (value.kind == 0u) return 0ul;
        if (value.kind == 1u) value.exponent += int(fixed_bits);
        ulong magnitude = 0ul;
        bool inexact = false;
        bool overflow = false;
        if (value.kind == 2u) {
            overflow = true;
        } else {
            magnitude = fp_round_magnitude(value, mode, inexact, overflow);
        }
        if (!overflow) {
            if (is_unsigned) {
                if (negative && magnitude != 0) overflow = true;
                else if (magnitude > width_mask) overflow = true;
            } else if (negative ? magnitude > signed_limit : magnitude > signed_limit - 1ul) {
                overflow = true;
            }
        }
        if (overflow) {
            state[@GUEST_FPSR@] |= 1ul;
            if (is_unsigned) return negative ? 0ul : width_mask;
            return negative ? signed_limit : signed_limit - 1ul;
        }
        if (inexact) state[@GUEST_FPSR@] |= 16ul;
        return (negative ? 0ul - magnitude : magnitude) & width_mask;
    }
    ulong fp_to_integer(ulong bits, uint wide_source, uint wide_destination, bool is_unsigned,
                        uint mode, device ulong *state) {
        return fp_to_integer_fixed(bits, wide_source, wide_destination != 0u ? 64u : 32u,
                                   is_unsigned, mode, 0u, state);
    }
    // IEEE-754 scalar divide using integer significands. Metal lacks binary64;
    // this also gives binary32 the guest FPCR rounding mode and FPSR flags.
    ulong fp_divide(ulong left, ulong right, uint wide, device ulong *state) {
        uint fraction = fp_fbits(wide);
        uint exponent_bits = fp_ebits(wide);
        uint bias = uint(fp_bias(wide));
        int minimum_exponent = 1 - fp_bias(wide);
        int maximum_exponent = fp_bias(wide);
        uint sign_bit = fraction + exponent_bits;
        ulong sign_mask = 1ul << sign_bit;
        ulong fraction_mask = (1ul << fraction) - 1ul;
        ulong exponent_limit = (1ul << exponent_bits) - 1ul;
        ulong exponent_mask = exponent_limit << fraction;
        ulong value_mask = fp_vmask(wide);
        left &= value_mask;
        right &= value_mask;
        ulong sign = (left ^ right) & sign_mask;
        ulong left_exponent = (left >> fraction) & exponent_limit;
        ulong right_exponent = (right >> fraction) & exponent_limit;
        ulong left_fraction = left & fraction_mask;
        ulong right_fraction = right & fraction_mask;
        bool flush_left = left_exponent == 0 && left_fraction != 0 &&
                          fp_flush(wide, state[@GUEST_FPCR@]);
        bool flush_right = right_exponent == 0 && right_fraction != 0 &&
                           fp_flush(wide, state[@GUEST_FPCR@]);
        if (flush_left || flush_right) state[@GUEST_FPSR@] |= 128ul;
        if (flush_left) {
            left &= sign_mask;
            left_exponent = 0;
            left_fraction = 0;
        }
        if (flush_right) {
            right &= sign_mask;
            right_exponent = 0;
            right_fraction = 0;
        }
        bool left_nan = left_exponent == exponent_limit && left_fraction != 0;
        bool right_nan = right_exponent == exponent_limit && right_fraction != 0;
        if (left_nan || right_nan) {
            ulong quiet_bit = 1ul << (fraction - 1u);
            bool left_signaling = left_nan && (left_fraction & quiet_bit) == 0;
            bool right_signaling = right_nan && (right_fraction & quiet_bit) == 0;
            if (left_signaling || right_signaling) state[@GUEST_FPSR@] |= 1ul;
            ulong source = left_signaling ? left :
                           right_signaling ? right : left_nan ? left : right;
            if ((state[@GUEST_FPCR@] & (1ul << 25)) != 0)
                return (exponent_limit << fraction) | quiet_bit;
            return (source | quiet_bit) & value_mask;
        }
        bool left_infinity = left_exponent == exponent_limit;
        bool right_infinity = right_exponent == exponent_limit;
        bool left_zero = left_exponent == 0 && left_fraction == 0;
        bool right_zero = right_exponent == 0 && right_fraction == 0;
        ulong infinity = sign | exponent_mask;
        if ((left_infinity && right_infinity) || (left_zero && right_zero)) {
            state[@GUEST_FPSR@] |= 1ul;
            return (exponent_limit << fraction) | (1ul << (fraction - 1u));
        }
        if (left_infinity) return infinity;
        if (right_infinity || left_zero) return sign;
        if (right_zero) {
            state[@GUEST_FPSR@] |= 2ul;
            return infinity;
        }
        ulong hidden = 1ul << fraction;
        ulong left_significand = left_fraction;
        ulong right_significand = right_fraction;
        int left_power = minimum_exponent;
        int right_power = minimum_exponent;
        if (left_exponent != 0) {
            left_significand |= hidden;
            left_power = int(left_exponent) - int(bias);
        } else {
            while ((left_significand & hidden) == 0) {
                left_significand <<= 1;
                --left_power;
            }
        }
        if (right_exponent != 0) {
            right_significand |= hidden;
            right_power = int(right_exponent) - int(bias);
        } else {
            while ((right_significand & hidden) == 0) {
                right_significand <<= 1;
                --right_power;
            }
        }
        int result_power = left_power - right_power;
        if (left_significand < right_significand) {
            left_significand <<= 1;
            --result_power;
        }
        ulong remainder = left_significand - right_significand;
        ulong significand = hidden;
        for (int bit = int(fraction) - 1; bit >= 0; --bit) {
            remainder <<= 1;
            if (remainder >= right_significand) {
                remainder -= right_significand;
                significand |= 1ul << uint(bit);
            }
        }
        remainder <<= 1;
        bool guard = remainder >= right_significand;
        if (guard) remainder -= right_significand;
        bool sticky = remainder != 0;
        uint mode = uint(state[@GUEST_FPCR@] >> 22) & 3u;
        if (result_power < minimum_exponent) {
            if (fp_flush(wide, state[@GUEST_FPCR@])) {
                state[@GUEST_FPSR@] |= 8ul;
                return sign;
            }
            uint shift = uint(minimum_exponent - result_power);
            ulong rounded = shift <= fraction + 1u ? significand >> shift : 0ul;
            bool subnormal_guard = shift <= fraction + 1u &&
                                   ((significand >> (shift - 1u)) & 1ul) != 0;
            ulong lower_mask = shift <= 1u ? 0ul :
                (shift - 1u >= 64u ? ~0ul : (1ul << (shift - 1u)) - 1ul);
            bool subnormal_sticky = (significand & lower_mask) != 0 || guard || sticky;
            bool inexact = subnormal_guard || subnormal_sticky;
            bool round_up = (mode == 1u && sign == 0 && inexact) ||
                            (mode == 2u && sign != 0 && inexact) ||
                            (mode == 0u && (subnormal_guard &&
                                (subnormal_sticky || (rounded & 1ul) != 0)));
            rounded += ulong(round_up);
            if (inexact) state[@GUEST_FPSR@] |= 24ul;
            if (rounded >= hidden) return sign | (1ul << fraction);
            return sign | rounded;
        }
        bool inexact = guard || sticky;
        bool round_up = (mode == 1u && sign == 0 && inexact) ||
                        (mode == 2u && sign != 0 && inexact) ||
                        (mode == 0u && guard && (sticky || (significand & 1ul) != 0));
        significand += ulong(round_up);
        if (significand == (hidden << 1)) {
            significand >>= 1;
            ++result_power;
        }
        if (result_power > maximum_exponent) {
            state[@GUEST_FPSR@] |= 20ul;
            bool to_infinity = mode == 0u || (mode == 1u && sign == 0) ||
                               (mode == 2u && sign != 0);
            return to_infinity ? infinity
                               : sign | ((exponent_limit - 1ul) << fraction) | fraction_mask;
        }
        if (inexact) state[@GUEST_FPSR@] |= 16ul;
        ulong encoded_exponent = ulong(result_power + int(bias));
        return sign | (encoded_exponent << fraction) | (significand & fraction_mask);
    }
    // Round toward +infinity to an integral value without using Metal floating
    // types, retaining NaN payloads and the guest's FZ/IOC behavior.
    ulong fp_round_positive(ulong value, uint wide, device ulong *state) {
        uint fraction = fp_fbits(wide);
        uint exponent_bits = fp_ebits(wide);
        uint sign_bit = fraction + exponent_bits;
        ulong sign_mask = 1ul << sign_bit;
        ulong fraction_mask = (1ul << fraction) - 1ul;
        ulong exponent_limit = (1ul << exponent_bits) - 1ul;
        ulong value_mask = fp_vmask(wide);
        value &= value_mask;
        ulong sign = value & sign_mask;
        ulong exponent = (value >> fraction) & exponent_limit;
        ulong significand = value & fraction_mask;
        if (exponent == exponent_limit) {
            if (significand == 0) return value;
            ulong quiet_bit = 1ul << (fraction - 1u);
            if ((significand & quiet_bit) == 0) state[@GUEST_FPSR@] |= 1ul;
            if ((state[@GUEST_FPCR@] & (1ul << 25)) != 0)
                return (exponent_limit << fraction) | quiet_bit;
            return (value | quiet_bit) & value_mask;
        }
        if (exponent == 0 && significand != 0 &&
            fp_flush(wide, state[@GUEST_FPCR@])) {
            state[@GUEST_FPSR@] |= 128ul;
            return sign;
        }
        if (exponent == 0 && significand == 0) return sign;
        int unbiased = exponent == 0
                           ? (1 - fp_bias(wide))
                           : int(exponent) - (fp_bias(wide));
        if (unbiased >= int(fraction)) return value;
        if (unbiased < 0) {
            if (sign != 0) return sign;
            return ((ulong(fp_bias(wide)) << fp_fbits(wide)));
        }
        uint fractional_bits = fraction - uint(unbiased);
        ulong discarded_mask = (1ul << fractional_bits) - 1ul;
        ulong discarded = value & discarded_mask;
        if (discarded == 0) return value;
        ulong rounded = value & ~discarded_mask;
        if (sign == 0) rounded += 1ul << fractional_bits;
        return rounded & value_mask;
    }

    // Compare scalar IEEE values by their integer encodings, preserving the
    // guest's FZ input behavior and FCMP/FCMPE signaling distinction.
    uint fp_compare(ulong left, ulong right, uint wide, bool signal_all_nans,
                    device ulong *state) {
        uint fraction = fp_fbits(wide);
        uint exponent_bits = fp_ebits(wide);
        uint sign_bit = fraction + exponent_bits;
        ulong sign_mask = 1ul << sign_bit;
        ulong fraction_mask = (1ul << fraction) - 1ul;
        ulong exponent_limit = (1ul << exponent_bits) - 1ul;
        ulong value_mask = fp_vmask(wide);
        left &= value_mask;
        right &= value_mask;
        bool left_subnormal = ((left >> fraction) & exponent_limit) == 0 &&
                              (left & fraction_mask) != 0;
        bool right_subnormal = ((right >> fraction) & exponent_limit) == 0 &&
                               (right & fraction_mask) != 0;
        if (fp_flush(wide, state[@GUEST_FPCR@])) {
            if (left_subnormal) {
                left &= sign_mask;
                state[@GUEST_FPSR@] |= 128ul;
            }
            if (right_subnormal) {
                right &= sign_mask;
                state[@GUEST_FPSR@] |= 128ul;
            }
        }
        ulong left_exponent = (left >> fraction) & exponent_limit;
        ulong right_exponent = (right >> fraction) & exponent_limit;
        ulong left_fraction = left & fraction_mask;
        ulong right_fraction = right & fraction_mask;
        bool left_nan = left_exponent == exponent_limit && left_fraction != 0;
        bool right_nan = right_exponent == exponent_limit && right_fraction != 0;
        if (left_nan || right_nan) {
            ulong quiet_bit = 1ul << (fraction - 1u);
            bool signaling = (left_nan && (left_fraction & quiet_bit) == 0) ||
                             (right_nan && (right_fraction & quiet_bit) == 0);
            if (signaling || signal_all_nans) state[@GUEST_FPSR@] |= 1ul;
            return 3u; // Unordered: N=0, Z=0, C=1, V=1.
        }
        ulong left_magnitude = left & ~sign_mask;
        ulong right_magnitude = right & ~sign_mask;
        if (left_magnitude == 0 && right_magnitude == 0) return 6u;
        bool left_negative = (left & sign_mask) != 0;
        bool right_negative = (right & sign_mask) != 0;
        bool less = left_negative != right_negative
                        ? left_negative
                        : (left_negative ? left_magnitude > right_magnitude
                                         : left_magnitude < right_magnitude);
        bool equal = left == right;
        return equal ? 6u : (less ? 8u : 2u);
    }

    ulong lane_mask(uint width) { return ~0ul >> (64 - width); }
    ulong lane_read(ulong2 source, uint index, uint width) {
        uint bit = index * width;
        return (source[bit >> 6] >> (bit & 63)) & lane_mask(width);
    }
    void lane_add(thread ulong2 &target, uint index, uint width, ulong value) {
        uint bit = index * width;
        target[bit >> 6] |= (value & lane_mask(width)) << (bit & 63);
    }
    ulong2 vector_dup(ulong source, uint width, uint bits) {
        ulong2 target = ulong2(0);
        for (uint lane = 0; lane < bits / width; ++lane) lane_add(target, lane, width, source);
        return target;
    }
    ulong2 vector_add(ulong2 left, ulong2 right, uint width, uint bits, bool subtract) {
        ulong2 target = ulong2(0);
        for (uint lane = 0; lane < bits / width; ++lane) {
            ulong a = lane_read(left, lane, width), b = lane_read(right, lane, width);
            lane_add(target, lane, width, subtract ? a - b : a + b);
        }
        return target;
    }
    ulong2 vector_mla(ulong2 accumulator, ulong2 left, ulong2 right, uint width, uint bits, bool subtract) {
        ulong2 target = ulong2(0);
        for (uint lane = 0; lane < bits / width; ++lane) {
            ulong product = lane_read(left, lane, width) * lane_read(right, lane, width);
            ulong prior = lane_read(accumulator, lane, width);
            lane_add(target, lane, width, subtract ? prior - product : prior + product);
        }
        return target;
    }
    ulong2 vector_long(ulong2 accumulator, ulong2 left, ulong2 right, uint width, uint bits, bool accumulate, bool top, bool subtract) {
        ulong2 target = ulong2(0);
        uint count = 128 / (2 * width);
        uint source_offset = top ? bits / width - count : 0;
        for (uint lane = 0; lane < count; ++lane) {
            ulong a = lane_read(left, lane + source_offset, width);
            ulong b = lane_read(right, lane + source_offset, width);
            ulong value = (accumulate ? lane_read(accumulator, lane, 2 * width) : 0) + (subtract ? 0ul - a * b : a * b);
            lane_add(target, lane, 2 * width, value);
        }
        return target;
    }
    @GUEST_VECTOR_CRYPTO_SOURCE@
    @PROCESS_VECTOR_OPS_MSL@
    @GUEST_NEON_SOURCE@
    ulong2 vector_reduce_long(ulong2 source, uint source_width, uint destination_width, uint bits) {
        ulong sum = 0;
        for (uint lane = 0; lane < bits / source_width; ++lane) sum += lane_read(source, lane, source_width);
        ulong2 target = ulong2(0);
        lane_add(target, 0, destination_width, sum);
        return target;
    }
    ulong2 vector_add_long(ulong2 left, ulong2 right, uint width, uint bits, bool top) {
        ulong2 target = ulong2(0);
        uint count = 128 / (2 * width);
        for (uint lane = 0; lane < count; ++lane)
            lane_add(target, lane, 2 * width, lane_read(left, lane + (top ? count : 0), width) + lane_read(right, lane + (top ? count : 0), width));
        return target;
    }
    ulong2 vector_sum(ulong2 source, uint width, uint bits) {
        ulong sum = 0;
        for (uint lane = 0; lane < bits / width; ++lane) sum += lane_read(source, lane, width);
        return ulong2(sum & lane_mask(width), 0ul);
    }
    ulong2 vector_compare(ulong2 left, ulong2 right, uint width, uint bits, bool inclusive, bool signed_lanes, bool equal_only) {
        ulong2 target = ulong2(0);
        ulong sign = signed_lanes ? (1ul << (width - 1)) : 0ul;
        for (uint lane = 0; lane < bits / width; ++lane) {
            ulong a = lane_read(left, lane, width) ^ sign;
            ulong b = lane_read(right, lane, width) ^ sign;
            bool matched = equal_only ? a == b : (inclusive ? a >= b : a > b);
            if (matched) lane_add(target, lane, width, lane_mask(width));
        }
        return target;
    }
    ulong2 vector_extreme(ulong2 source, uint width, uint bits, bool minimum, bool signed_lanes) {
        ulong best = lane_read(source, 0, width);
        ulong sign = signed_lanes ? (1ul << (width - 1)) : 0ul;
        for (uint lane = 1; lane < bits / width; ++lane) {
            ulong value = lane_read(source, lane, width);
            if (minimum ? (value ^ sign) < (best ^ sign) : (value ^ sign) > (best ^ sign)) best = value;
        }
        return ulong2(best, 0ul);
    }
    ulong2 vector_shift(ulong2 accumulator, ulong2 source, uint width, uint bits, uint shift, bool left) {
        ulong2 target = ulong2(0);
        for (uint lane = 0; lane < bits / width; ++lane) {
            ulong value = lane_read(source, lane, width);
            ulong shifted = shift >= width ? 0 : left ? value << shift : value >> shift;
            lane_add(target, lane, width, lane_read(accumulator, lane, width) + shifted);
        }
        return target;
    }
    ulong2 vector_ushl(ulong2 source, ulong2 shifts, uint width, uint bits) {
        ulong2 target = ulong2(0);
        for (uint lane = 0; lane < bits / width; ++lane) {
            int shift = int(char(lane_read(shifts, lane, width)));
            ulong value = lane_read(source, lane, width);
            if (shift > -int(width) && shift < int(width))
                lane_add(target, lane, width, shift < 0 ? value >> -shift : value << shift);
        }
        return target;
    }
    ulong2 vector_uzp1(ulong2 left, ulong2 right, uint width, uint bits) {
        ulong2 target = ulong2(0);
        uint half_count = bits / width / 2;
        for (uint lane = 0; lane < half_count; ++lane) {
            lane_add(target, lane, width, lane_read(left, 2 * lane, width));
            lane_add(target, lane + half_count, width, lane_read(right, 2 * lane, width));
        }
        return target;
    }
    ulong2 vector_zip1(ulong2 left, ulong2 right, uint width, uint bits) {
        ulong2 target = ulong2(0);
        uint half_count = bits / width / 2;
        for (uint lane = 0; lane < half_count; ++lane) {
            lane_add(target, 2 * lane, width, lane_read(left, lane, width));
            lane_add(target, 2 * lane + 1, width, lane_read(right, lane, width));
        }
        return target;
    }
    uint guest_crc32(uint seed, ulong value, uint width, uint polynomial) {
        for (uint byte = 0; byte < width; ++byte) {
            seed ^= uchar(value >> (8 * byte));
            for (uint bit = 0; bit < 8; ++bit) seed = (seed >> 1) ^ ((0u - (seed & 1)) & polynomial);
        }
        return seed;
    }
    ulong multiply_high(ulong left, ulong right) {
        ulong lo_left = uint(left), lo_right = uint(right);
        ulong hi_left = left >> 32, hi_right = right >> 32;
        ulong carry = ((lo_left * lo_right) >> 32) + hi_left * lo_right;
        ulong middle = uint(carry) + lo_left * hi_right;
        return hi_left * hi_right + (carry >> 32) + (middle >> 32);
    }
    @GUEST_MEMORY_MSL_SOURCE@
    void guest_copy(device uchar *dst, device const uchar *src, ulong count) {
        for (ulong i = 0; i < count; ++i) dst[i] = src[i];
    }
    void guest_move(device uchar *dst, device const uchar *src, ulong count) {
        if (dst > src && ulong(dst - src) < count) {
            for (ulong i = count; i > 0; --i) dst[i - 1] = src[i - 1];
        } else {
            for (ulong i = 0; i < count; ++i) dst[i] = src[i];
        }
    }
    void guest_fill(device uchar *dst, uchar value, ulong count) {
        for (ulong i = 0; i < count; ++i) dst[i] = value;
    }
    void guest_pattern(device uchar *dst, device const uchar *src, ulong count, uint width) {
        uchar pattern[16];
        for (uint i = 0; i < width; ++i) pattern[i] = src[i];
        for (ulong i = 0; i < count; ++i) dst[i] = pattern[i & (width - 1)];
    }
    ulong encode_guest_pointer(ulong address, ulong key, ulong index) {
        if (key == 2 && address <= @GUEST_DATA_ADDRESS_MASK@ul) {
            ulong code = index % @GUEST_DATA_TOKEN_BUCKETS@ul + 1;
            return (code << 47) | address;
        }
        return @GUEST_TOKEN_TAG@ul | ((index + 1) << @GUEST_TOKEN_INDEX_SHIFT@) | (address & 7ul);
    }
    ulong pacga_rotate(ulong value, uint shift) {
        return (value << shift) | (value >> (64 - shift));
    }
    void pacga_round(thread ulong4 &v) {
        v.x += v.y; v.y = pacga_rotate(v.y, 13) ^ v.x; v.x = pacga_rotate(v.x, 32);
        v.z += v.w; v.w = pacga_rotate(v.w, 16) ^ v.z;
        v.x += v.w; v.w = pacga_rotate(v.w, 21) ^ v.x;
        v.z += v.y; v.y = pacga_rotate(v.y, 17) ^ v.z; v.z = pacga_rotate(v.z, 32);
    }
    // SipHash-2-4 supplies a process-local code for the fixed 16-byte input.
    ulong guest_pacga(device const ulong *state, ulong value, ulong modifier) {
        ulong k0 = state[@GUEST_PACGA_KEY@], k1 = state[@GUEST_PACGA_KEY_PLUS_ONE@];
        ulong4 v = ulong4(k0 ^ 0x736f6d6570736575ul, k1 ^ 0x646f72616e646f6dul,
                          k0 ^ 0x6c7967656e657261ul, k1 ^ 0x7465646279746573ul);
        ulong3 words = ulong3(value, modifier, 16ul << 56);
        for (uint i = 0; i < 3; ++i) {
            v.w ^= words[i];
            pacga_round(v); pacga_round(v);
            v.x ^= words[i];
        }
        v.z ^= 0xfful;
        for (uint i = 0; i < 4; ++i) pacga_round(v);
        return (v.x ^ v.y ^ v.z ^ v.w) & 0xffffffff00000000ul;
    }
    ulong sign_guest_pointer(device ulong *state, ulong address, ulong modifier, ulong key) {
        ulong count = state[@GUEST_TOKEN_COUNT@];
        for (ulong index = 0; index < count; ++index) {
            device ulong *token = state + @GUEST_TOKEN_START@ + index * 3;
            if (token[0] == address && token[1] == modifier && token[2] == key)
                return encode_guest_pointer(address, key, index);
        }
        if (count >= state[@GUEST_TOKEN_CAPACITY@]) return 0;
        device ulong *token = state + @GUEST_TOKEN_START@ + count * 3;
        token[0] = address; token[1] = modifier; token[2] = key;
        state[@GUEST_TOKEN_COUNT@] = count + 1;
        return encode_guest_pointer(address, key, count);
    }
    device ulong *guest_pointer_token(device ulong *state, ulong pointer) {
        ulong tag = pointer & ~0xfffffffful;
        ulong index = (pointer & 0xfffffffful) >> @GUEST_TOKEN_INDEX_SHIFT@;
        if (tag != @GUEST_TOKEN_TAG@ul ||
            index == 0 || index > state[@GUEST_TOKEN_COUNT@])
            return nullptr;
        device ulong *token = state + @GUEST_TOKEN_START@ + (index - 1) * 3;
        return encode_guest_pointer(token[0], token[2], index - 1) == pointer ? token : nullptr;
    }
    device ulong *guest_data_token(device ulong *state, ulong pointer, ulong modifier, bool authenticate) {
        ulong bucket = (pointer >> 47) & 0xfful;
        ulong address = pointer & @GUEST_DATA_ADDRESS_MASK@ul;
        ulong upper = pointer >> 55;
        if (!bucket || upper > 1ul) return nullptr;
        for (ulong index = bucket - 1; index < state[@GUEST_TOKEN_COUNT@]; index += @GUEST_DATA_TOKEN_BUCKETS@ul) {
            device ulong *token = state + @GUEST_TOKEN_START@ + index * 3;
            if (token[2] == 2 && token[0] == address &&
                (!upper || encode_guest_pointer(address, 2, index) == pointer) &&
                (!authenticate || token[1] == modifier)) return token;
        }
        return nullptr;
    }
    ulong strip_guest_pointer(device ulong *state, ulong pointer) {
        device ulong *token = guest_pointer_token(state, pointer);
        if (!token) token = guest_data_token(state, pointer, 0ul, false);
        return token ? token[0] : pointer;
    }
    ulong authenticate_guest_pointer(device ulong *state, ulong pointer, ulong modifier, ulong key) {
        device ulong *token = guest_pointer_token(state, pointer);
        if (key == 2 && (!token || token[2] != key)) token = guest_data_token(state, pointer, modifier, true);
        if (token && token[1] == modifier && token[2] == key) return token[0];
        if (@GUEST_PLAIN_ARM64_MAIN@ && !token && pointer <= @GUEST_DATA_ADDRESS_MASK@ul) return pointer;
        return ~0ul;
    }
    kernel void run_process(device uchar *memory [[buffer(0)]],
                            device ulong *x [[buffer(1)]],
                            constant ulong &size [[buffer(2)]],
                            device const ulong *writable [[buffer(3)]],
                            device const ulong *resident [[buffer(4)]],
                            device const ulong *config [[buffer(5)]]) {
        // Preserve the guest dispatch variable across switch cases on Metal.
        volatile ulong pc = x[33];
        bool z = (x[32] & 1ul) != 0;
        bool n = (x[32] & 2ul) != 0;
        bool v = (x[32] & 4ul) != 0;
        bool c = (x[32] & 8ul) != 0;
        for (uint blocks = 0; x[36] > 0 && blocks < 8192; ++blocks) {
            --x[36];
            switch (pc) {
    @RENDERED_CASES@
            default:
                x[33] = pc;
                x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3);
                x[34] = 7;
                return;
            }
        }
        x[33] = pc;
        x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3);
        x[34] = x[36] == 0 ? 1 : 8;
    }
)PROCESS_MSL";

void replace_all(std::string& text, std::string_view needle, std::string_view value) {
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos) {
    text.replace(position, needle.size(), value);
    position += value.size();
  }
}

void remove_swift_multiline_indentation(std::string& text) {
  if (!text.empty() && text.front() == '\n') text.erase(text.begin());
  std::size_t line_start = 0;
  while (line_start < text.size()) {
    const auto line_end = text.find('\n', line_start);
    const auto width = line_end == std::string::npos ? text.size() - line_start
                                                     : line_end - line_start;
    if (width >= 4 && text.compare(line_start, 4, "    ") == 0) {
      text.erase(line_start, 4);
      if (line_end != std::string::npos) line_start = line_end - 4 + 1;
      else line_start = text.size();
    } else if (line_end == std::string::npos) {
      line_start = text.size();
    } else {
      line_start = line_end + 1;
    }
  }
  if (!text.empty() && text.back() == '\n') text.pop_back();
}

}  // namespace

std::string render_source(std::string_view rendered_cases, bool plain_arm64_main) {
  std::string source(kProcessMetalTemplate);
  remove_swift_multiline_indentation(source);
  const auto decimal = [](std::uint64_t value) { return std::to_string(value); };
  const std::array<std::pair<std::string_view, std::string>, 18> values = {{
      {"@RENDERED_CASES@", std::string(rendered_cases)},
      {"@GUEST_NEON_SOURCE@", std::string(neon_metal_source())},
      {"@GUEST_MEMORY_MSL_SOURCE@", std::string(guestMemoryMslSource)},
      {"@GUEST_DATA_ADDRESS_MASK@", decimal(guestDataAddressMask)},
      {"@GUEST_DATA_TOKEN_BUCKETS@", decimal(guestDataTokenBuckets)},
      {"@GUEST_FPCR@", decimal(guestFPCR)},
      {"@GUEST_PLAIN_ARM64_MAIN@", plain_arm64_main ? "true" : "false"},
      {"@GUEST_FPSR@", decimal(guestFPSR)},
      {"@GUEST_PACGA_KEY@", decimal(guestPACGAKey)},
      {"@GUEST_PACGA_KEY_PLUS_ONE@", decimal(guestPACGAKey + 1)},
      {"@GUEST_TOKEN_CAPACITY@", decimal(guestTokenCapacity)},
      {"@GUEST_TOKEN_COUNT@", decimal(guestTokenCount)},
      {"@GUEST_TOKEN_INDEX_SHIFT@", decimal(guestTokenIndexShift)},
      {"@GUEST_TOKEN_START@", decimal(guestTokenStart)},
      {"@GUEST_TOKEN_TAG@", decimal(guestTokenTag)},
      {"@GUEST_VECTOR_CRYPTO_SOURCE@", std::string(vector_crypto_metal_source())},
      {"@PROCESS_VECTOR_OPS_MSL@", std::string(processVectorOpsMslSource)},
      {"@TYPED_INTEGER_MSL_SOURCE@", std::string(typed_integer_msl_source)},
  }};
  for (const auto& [key, value] : values) replace_all(source, key, value);
  return source;
}

}  // namespace arm64metal::process_msl
