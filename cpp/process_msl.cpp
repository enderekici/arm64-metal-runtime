#include "process_msl.hpp"
#include "guest_memory_msl.hpp"

#include "guest_vector_crypto.hpp"
#include "integer_ops_shared.h"
#include "process_host.hpp"

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
    // IEEE-754 scalar divide using integer significands. Metal lacks binary64;
    // this also gives binary32 the guest FPCR rounding mode and FPSR flags.
    ulong fp_divide(ulong left, ulong right, bool wide, device ulong *state) {
        uint fraction = wide ? 52u : 23u;
        uint exponent_bits = wide ? 11u : 8u;
        uint bias = wide ? 1023u : 127u;
        int minimum_exponent = wide ? -1022 : -126;
        int maximum_exponent = wide ? 1023 : 127;
        uint sign_bit = fraction + exponent_bits;
        ulong sign_mask = 1ul << sign_bit;
        ulong fraction_mask = (1ul << fraction) - 1ul;
        ulong exponent_limit = (1ul << exponent_bits) - 1ul;
        ulong exponent_mask = exponent_limit << fraction;
        ulong value_mask = wide ? ~0ul : 0xfffffffful;
        left &= value_mask;
        right &= value_mask;
        ulong sign = (left ^ right) & sign_mask;
        ulong left_exponent = (left >> fraction) & exponent_limit;
        ulong right_exponent = (right >> fraction) & exponent_limit;
        ulong left_fraction = left & fraction_mask;
        ulong right_fraction = right & fraction_mask;
        bool flush_left = left_exponent == 0 && left_fraction != 0 &&
                          (state[@GUEST_FPCR@] & (1ul << 24)) != 0;
        bool flush_right = right_exponent == 0 && right_fraction != 0 &&
                           (state[@GUEST_FPCR@] & (1ul << 24)) != 0;
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
            if ((state[@GUEST_FPCR@] & (1ul << 24)) != 0) {
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
    ulong fp_round_positive(ulong value, bool wide, device ulong *state) {
        uint fraction = wide ? 52u : 23u;
        uint exponent_bits = wide ? 11u : 8u;
        uint sign_bit = fraction + exponent_bits;
        ulong sign_mask = 1ul << sign_bit;
        ulong fraction_mask = (1ul << fraction) - 1ul;
        ulong exponent_limit = (1ul << exponent_bits) - 1ul;
        ulong value_mask = wide ? ~0ul : 0xfffffffful;
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
            (state[@GUEST_FPCR@] & (1ul << 24)) != 0) {
            state[@GUEST_FPSR@] |= 128ul;
            return sign;
        }
        if (exponent == 0 && significand == 0) return sign;
        int unbiased = exponent == 0
                           ? (wide ? -1022 : -126)
                           : int(exponent) - (wide ? 1023 : 127);
        if (unbiased >= int(fraction)) return value;
        if (unbiased < 0) {
            if (sign != 0) return sign;
            return (wide ? 0x3ff0000000000000ul : 0x3f800000ul);
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
    uint fp_compare(ulong left, ulong right, bool wide, bool signal_all_nans,
                    device ulong *state) {
        uint fraction = wide ? 52u : 23u;
        uint exponent_bits = wide ? 11u : 8u;
        uint sign_bit = fraction + exponent_bits;
        ulong sign_mask = 1ul << sign_bit;
        ulong fraction_mask = (1ul << fraction) - 1ul;
        ulong exponent_limit = (1ul << exponent_bits) - 1ul;
        ulong value_mask = wide ? ~0ul : 0xfffffffful;
        left &= value_mask;
        right &= value_mask;
        bool left_subnormal = ((left >> fraction) & exponent_limit) == 0 &&
                              (left & fraction_mask) != 0;
        bool right_subnormal = ((right >> fraction) & exponent_limit) == 0 &&
                               (right & fraction_mask) != 0;
        if ((state[@GUEST_FPCR@] & (1ul << 24)) != 0) {
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
        return token && token[1] == modifier && token[2] == key ? token[0] : ~0ul;
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

std::string render_source(std::string_view rendered_cases) {
  std::string source(kProcessMetalTemplate);
  remove_swift_multiline_indentation(source);
  const auto decimal = [](std::uint64_t value) { return std::to_string(value); };
  const std::array<std::pair<std::string_view, std::string>, 15> values = {{
      {"@RENDERED_CASES@", std::string(rendered_cases)},
      {"@GUEST_MEMORY_MSL_SOURCE@", std::string(guestMemoryMslSource)},
      {"@GUEST_DATA_ADDRESS_MASK@", decimal(guestDataAddressMask)},
      {"@GUEST_DATA_TOKEN_BUCKETS@", decimal(guestDataTokenBuckets)},
      {"@GUEST_FPCR@", decimal(guestFPCR)},
      {"@GUEST_FPSR@", decimal(guestFPSR)},
      {"@GUEST_PACGA_KEY@", decimal(guestPACGAKey)},
      {"@GUEST_PACGA_KEY_PLUS_ONE@", decimal(guestPACGAKey + 1)},
      {"@GUEST_TOKEN_CAPACITY@", decimal(guestTokenCapacity)},
      {"@GUEST_TOKEN_COUNT@", decimal(guestTokenCount)},
      {"@GUEST_TOKEN_INDEX_SHIFT@", decimal(guestTokenIndexShift)},
      {"@GUEST_TOKEN_START@", decimal(guestTokenStart)},
      {"@GUEST_TOKEN_TAG@", decimal(guestTokenTag)},
      {"@GUEST_VECTOR_CRYPTO_SOURCE@", std::string(vector_crypto_metal_source())},
      {"@TYPED_INTEGER_MSL_SOURCE@", std::string(typed_integer_msl_source)},
  }};
  for (const auto& [key, value] : values) replace_all(source, key, value);
  return source;
}

}  // namespace arm64metal::process_msl
