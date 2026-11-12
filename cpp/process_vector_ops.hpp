#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "translate_core.hpp"

namespace arm64metal::vector_ops {

namespace detail {

inline std::uint32_t field(std::uint32_t bits, int start, int width) {
  return (bits >> start) & ((1u << width) - 1u);
}
inline std::string num(std::int64_t value) { return std::to_string(value); }
inline std::string slot(std::uint32_t reg, int half) { return "x[" + num(806 + 2 * int64_t(reg) + half) + "]"; }
inline std::string vreg(std::uint32_t reg) {
  return "ulong2(" + slot(reg, 0) + ", " + slot(reg, 1) + ")";
}
inline std::string assign(std::uint32_t rd, const std::string& expression) {
  return "{ ulong2 value = " + expression + "; " + slot(rd, 0) + " = value[0]; " +
         slot(rd, 1) + " = value[1]; }";
}
inline std::string boolean(bool value) { return value ? "true" : "false"; }

// Emits the bounds check and defines `ulong p` as the backing offset of `address`.
inline std::vector<std::string> access(bool cached, std::int64_t pc, std::int64_t bytes,
                                       bool write) {
  const std::string fault = "{ x[874] = address; x[33] = " + num(pc) + "ul; x[34] = 2; return; }";
  const std::string width = num(bytes) + "ul";
  if (cached) {
    return {"ulong p = guest_offset(address, " + width + ", size, writable, " + boolean(write) +
                ", x, resident, config);",
            "if (p == ~0ul) " + fault};
  }
  return {"if (address < 16384ul || address > size - " + width +
              (write ? " || !guest_write(address, " + width + ", writable)" : std::string()) +
              ") " + fault,
          "ulong p = address;"};
}

inline translate_core::Instruction make(std::vector<std::string> code, std::int64_t pc) {
  return translate_core::Instruction{std::move(code), {pc + 4}};
}

inline std::optional<translate_core::Instruction> floating_point(std::uint32_t bits,
                                                                  std::int64_t pc) {
  const auto rd = field(bits, 0, 5);
  const auto rn = field(bits, 5, 5);
  const auto rm = field(bits, 16, 5);
  const bool vector_form = field(bits, 24, 5) == 0x0e;
  const bool scalar_form = field(bits, 24, 5) == 0x1e && field(bits, 30, 1) == 1;
  const bool by_element = field(bits, 24, 5) == 0x0f || (field(bits, 24, 5) == 0x1f && field(bits, 30, 1) == 1);
  const bool u = field(bits, 29, 1) != 0;
  const bool q = field(bits, 30, 1) != 0;
  const bool a = field(bits, 23, 1) != 0;
  const bool wide = field(bits, 22, 1) != 0;
  const bool scalar = field(bits, 28, 1) == 1;
  const int lane_bits = wide ? 64 : 32;
  const int total = scalar ? lane_bits : (q ? 128 : 64);
  const auto n = vreg(rn);
  const auto m = vreg(rm);
  const auto d = vreg(rd);
  const auto w = boolean(wide);
  const auto bitcount = num(total);
  const auto bitsarg = w + ", " + bitcount + ", x";
  if ((vector_form || scalar_form) && (bits & 0x00200400U) == 0x00200400U) {
    const auto opcode = field(bits, 11, 5);
    if (opcode < 0x18) return std::nullopt;
    if (!scalar && wide && !q) return std::nullopt;
    if (scalar) {
      const bool allowed = (u && a && opcode == 0x1a) || (u && (opcode == 0x1c || opcode == 0x1d)) ||
                           (!u && !a && opcode == 0x1c);
      if (!allowed) return std::nullopt;
    }
    std::string expression;
    if (!u) {
      switch (opcode) {
        case 0x18: expression = "vop_fp_binary(" + num(a ? 7 : 6) + "u, " + n + ", " + m + ", " + bitsarg + ")"; break;
        case 0x19: expression = "vop_fp_fused(" + d + ", " + n + ", " + m + ", " + boolean(a) + ", " + bitsarg + ")"; break;
        case 0x1a: expression = "vop_fp_binary(" + num(a ? 3 : 2) + "u, " + n + ", " + m + ", " + bitsarg + ")"; break;
        case 0x1c: if (a) return std::nullopt; expression = "vop_fp_compare(0u, " + n + ", " + m + ", " + bitsarg + ")"; break;
        case 0x1e: expression = "vop_fp_binary(" + num(a ? 5 : 4) + "u, " + n + ", " + m + ", " + bitsarg + ")"; break;
        default: return std::nullopt;
      }
    } else {
      switch (opcode) {
        case 0x18: expression = "vop_fp_pairwise(" + num(a ? 7 : 6) + "u, " + n + ", " + m + ", " + bitsarg + ")"; break;
        case 0x1a:
          expression = a ? "vop_fp_binary(10u, " + n + ", " + m + ", " + bitsarg + ")"
                         : "vop_fp_pairwise(2u, " + n + ", " + m + ", " + bitsarg + ")";
          break;
        case 0x1b: if (a) return std::nullopt; expression = "vop_fp_binary(0u, " + n + ", " + m + ", " + bitsarg + ")"; break;
        case 0x1c: expression = "vop_fp_compare(" + num(a ? 2 : 1) + "u, " + n + ", " + m + ", " + bitsarg + ")"; break;
        case 0x1d: expression = "vop_fp_compare(" + num(a ? 4 : 3) + "u, " + n + ", " + m + ", " + bitsarg + ")"; break;
        case 0x1e: expression = "vop_fp_pairwise(" + num(a ? 5 : 4) + "u, " + n + ", " + m + ", " + bitsarg + ")"; break;
        case 0x1f: if (a) return std::nullopt; expression = "vop_fp_binary(9u, " + n + ", " + m + ", " + bitsarg + ")"; break;
        default: return std::nullopt;
      }
    }
    return make({assign(rd, expression)}, pc);
  }
  // Scalar pairwise: FADDP, FMAXNMP, FMINNMP, FMAXP, FMINP.
  if ((bits & 0xff3e0c00U) == 0x7e300800U) {
    const auto opcode = field(bits, 12, 5);
    std::string op;
    switch (opcode) {
      case 0x0c: op = a ? "7u" : "6u"; break;
      case 0x0d: if (a) return std::nullopt; op = "2u"; break;
      case 0x0f: op = a ? "5u" : "4u"; break;
      default: return std::nullopt;
    }
    return make({"{ ulong2 value = vop_fp_pairwise(" + op + ", " + n + ", " + n + ", " + w + ", " +
                 num(2 * lane_bits) + ", x); value = ulong2(value[0] & " +
                 (wide ? "~0ul" : "0xfffffffful") + ", 0ul); " + slot(rd, 0) + " = value[0]; " +
                 slot(rd, 1) + " = 0ul; }"},
                pc);
  }
  // Two-register miscellaneous floating-point.
  if ((vector_form || scalar_form) && (bits & 0x00300c00U) == 0x00200800U && (bits & 0x000e0000U) == 0) {
    const auto opcode = field(bits, 12, 5);
    if (!scalar && wide && !q) return std::nullopt;
    std::string expression;
    const auto unary = [&](int op, int mode) {
      return "vop_fp_unary(" + num(op) + "u, " + num(mode) + "u, " + n + ", " + bitsarg + ")";
    };
    const auto compare = [&](int kind) {
      return "vop_fp_compare(" + num(kind) + "u, " + n + ", ulong2(0), " + bitsarg + ")";
    };
    if (!u && !a) {
      switch (opcode) {
        case 0x18: expression = unary(3, 0); break;
        case 0x19: expression = unary(3, 2); break;
        case 0x1a: expression = unary(4, 0); break;
        case 0x1b: expression = unary(4, 2); break;
        case 0x1c: expression = unary(4, 4); break;
        case 0x1d: expression = unary(6, 0); break;
        default: return std::nullopt;
      }
    } else if (!u && a) {
      switch (opcode) {
        case 0x0c: expression = compare(2); break;
        case 0x0d: expression = compare(0); break;
        case 0x0e: expression = compare(6); break;
        case 0x0f: expression = unary(0, 0); break;
        case 0x18: expression = unary(3, 1); break;
        case 0x19: expression = unary(3, 3); break;
        case 0x1a: expression = unary(4, 1); break;
        case 0x1b: expression = unary(4, 3); break;
        default: return std::nullopt;
      }
    } else if (u && !a) {
      switch (opcode) {
        case 0x18: expression = unary(3, 4); break;
        case 0x19: expression = unary(3, 6); break;
        case 0x1a: expression = unary(5, 0); break;
        case 0x1b: expression = unary(5, 2); break;
        case 0x1c: expression = unary(5, 4); break;
        case 0x1d: expression = unary(7, 0); break;
        default: return std::nullopt;
      }
    } else {
      switch (opcode) {
        case 0x0c: expression = compare(1); break;
        case 0x0d: expression = compare(5); break;
        case 0x0f: expression = unary(1, 0); break;
        case 0x19: expression = unary(3, 5); break;
        case 0x1a: expression = unary(5, 1); break;
        case 0x1b: expression = unary(5, 3); break;
        case 0x1f: expression = unary(2, 0); break;
        default: return std::nullopt;
      }
    }
    return make({assign(rd, expression)}, pc);
  }
  // FMLA, FMLS and FMUL by element.
  if (by_element && (bits & 0x00800400U) == 0x00800000U && !u) {
    const auto opcode = field(bits, 12, 4);
    if (opcode != 1 && opcode != 5 && opcode != 9) return std::nullopt;
    if (!scalar && wide && !q) return std::nullopt;
    const auto h = field(bits, 11, 1);
    const auto l = field(bits, 21, 1);
    const auto mbit = field(bits, 20, 1);
    if (wide && l) return std::nullopt;
    const auto index = wide ? h : (h << 1) | l;
    const auto element = vreg(mbit << 4 | field(bits, 16, 4));
    const auto duplicate = "vector_dup(lane_read(" + element + ", " + num(index) + ", " + num(lane_bits) +
                           "), " + num(lane_bits) + ", " + num(total) + ")";
    std::string expression = opcode == 9
        ? "vop_fp_binary(0u, " + n + ", " + duplicate + ", " + bitsarg + ")"
        : "vop_fp_fused(" + d + ", " + n + ", " + duplicate + ", " + boolean(opcode == 5) + ", " + bitsarg + ")";
    return make({assign(rd, expression)}, pc);
  }
  (void)rm;
  return std::nullopt;
}

// Scalar fixed-point FCVTZS/FCVTZU and SCVTF/UCVTF.
inline std::optional<translate_core::Instruction> fixed_point(std::uint32_t bits, std::int64_t pc) {
  if ((bits & 0x7f200000U) != 0x1e000000U) return std::nullopt;
  const bool sf = field(bits, 31, 1) != 0;
  const auto type = field(bits, 22, 2);
  const auto rmode = field(bits, 19, 2);
  const auto opcode = field(bits, 16, 3);
  const auto scale = field(bits, 10, 6);
  const auto rn = field(bits, 5, 5);
  const auto rd = field(bits, 0, 5);
  if (type > 1 || (!sf && scale < 32)) return std::nullopt;
  const auto fbits = 64 - scale;
  const bool wide = type == 1;
  const auto w = boolean(wide);
  if (rmode == 3 && opcode <= 1) {
    const auto source = slot(rn, 0);
    const auto masked = wide ? source : "(" + source + " & 0xfffffffful)";
    const auto converted = "fp_to_integer(vop_fp_scale_up(" + masked + ", " + w + ", " + num(fbits) +
                           "u, x), " + w + ", " + boolean(sf) + ", " + boolean(opcode == 1) + ", 3u, x)";
    return make({rd == 31 ? "{ ulong ignored = " + converted + "; }"
                          : "x[" + num(rd) + "] = " + converted + ";"}, pc);
  }
  if (rmode == 0 && (opcode == 2 || opcode == 3)) {
    const bool is_signed = opcode == 2;
    const auto source = rn == 31 ? std::string("0ul") : "x[" + num(rn) + "]";
    const auto raw = sf ? source
                        : is_signed ? "ulong(long(int(" + source + ")))" : "ulong(uint(" + source + "))";
    const auto negative = is_signed ? "(long(" + raw + ") < 0)" : std::string("false");
    const auto converted = "vop_fp_scale_down(integer_to_float(" + raw + ", " + negative + ", " +
                           (wide ? "52u" : "23u") + ", x), " + w + ", " + num(fbits) + "u)";
    return make({assign(rd, "ulong2(" + converted + ", 0ul)")}, pc);
  }
  return std::nullopt;
}

inline std::optional<translate_core::Instruction> lower(std::uint32_t bits, std::int64_t pc,
                                                         bool cached) {
  (void)cached;
  if (auto result = floating_point(bits, pc)) return result;
  if (auto result = fixed_point(bits, pc)) return result;
  return std::nullopt;
}

}  // namespace detail

inline std::optional<translate_core::Instruction> lower(std::uint32_t bits, std::int64_t pc,
                                                         bool cached) {
  return detail::lower(bits, pc, cached);
}


}  // namespace arm64metal::vector_ops
