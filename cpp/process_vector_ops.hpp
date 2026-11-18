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

// LD2-LD4 and ST2-ST4 (multiple structures), LDn/STn (single structure) and LDnR.
inline std::optional<translate_core::Instruction> structure_memory(std::uint32_t bits,
                                                                    std::int64_t pc,
                                                                    bool cached) {
  const bool multiple = (bits & 0xbf200000U) == 0x0c000000U;
  const bool single = (bits & 0xbf000000U) == 0x0d000000U;
  if (!multiple && !single) return std::nullopt;
  const bool post = field(bits, 23, 1) != 0;
  const auto rm = field(bits, 16, 5);
  const auto rn = field(bits, 5, 5);
  const auto rt = field(bits, 0, 5);
  const bool q = field(bits, 30, 1) != 0;
  const bool load = field(bits, 22, 1) != 0;
  if (!post && rm != 0) return std::nullopt;
  std::int64_t selem = 0;
  std::int64_t element = 0;  // bytes per element
  std::int64_t index = 0;
  bool replicate = false;
  if (multiple) {
    const auto opcode = field(bits, 12, 4);
    selem = opcode == 8 ? 2 : opcode == 4 ? 3 : opcode == 0 ? 4 : 0;
    if (selem == 0) return std::nullopt;
    const auto size = field(bits, 10, 2);
    if (size == 3 && !q) return std::nullopt;
    element = std::int64_t(1) << size;
  } else {
    const auto opcode = field(bits, 13, 3);
    const auto s = field(bits, 12, 1);
    const auto size = field(bits, 10, 2);
    selem = ((opcode & 1) << 1 | field(bits, 21, 1)) + 1;
    switch (opcode >> 1) {
      case 0:
        element = 1;
        index = (q << 3) | (s << 2) | size;
        break;
      case 1:
        if (size & 1) return std::nullopt;
        element = 2;
        index = (q << 2) | (s << 1) | (size >> 1);
        break;
      case 2:
        if (size == 0) {
          element = 4;
          index = (q << 1) | s;
        } else if (size == 1 && s == 0) {
          element = 8;
          index = q;
        } else {
          return std::nullopt;
        }
        break;
      default:
        if (!load || s != 0) return std::nullopt;
        replicate = true;
        element = std::int64_t(1) << size;
        break;
    }
  }
  const std::int64_t lanes_per_register = (q ? 16 : 8) / element;
  const std::int64_t bytes = multiple ? selem * lanes_per_register * element : selem * element;
  const auto ebits = num(element * 8);
  std::vector<std::string> code{"{ ulong address = x[" + num(rn) + "];"};
  for (auto& line : access(cached, pc, bytes, !load)) code.push_back(line);
  if (multiple) {
    for (std::int64_t r = 0; r < selem; ++r) {
      const auto reg = (rt + r) & 31U;
      if (load) {
        code.push_back("{ ulong2 value = ulong2(0);");
        code.push_back("for (uint lane = 0; lane < " + num(lanes_per_register) + "; ++lane) lane_add(value, lane, " +
                       ebits + ", load_guest_word(memory + p + (lane * " + num(selem) + " + " +
                       num(r) + ") * " + num(element) + ", " + num(element) + "));");
        code.push_back(slot(reg, 0) + " = value[0]; " + slot(reg, 1) + " = value[1]; }");
      } else {
        code.push_back("for (uint lane = 0; lane < " + num(lanes_per_register) +
                       "; ++lane) store_guest_word(memory + p + (lane * " + num(selem) + " + " + num(r) +
                       ") * " + num(element) + ", lane_read(" + vreg(reg) + ", lane, " + ebits + "), " +
                       num(element) + ");");
      }
    }
  } else {
    for (std::int64_t r = 0; r < selem; ++r) {
      const auto reg = (rt + r) & 31U;
      const auto address = "memory + p + " + num(r * element);
      if (replicate) {
        code.push_back(assign(reg, "vector_dup(load_guest_word(" + address + ", " + num(element) +
                                       "), " + ebits + ", " + (q ? "128" : "64") + ")"));
      } else if (load) {
        code.push_back(assign(reg, "vop_lane_set(" + vreg(reg) + ", " + num(index) + ", " + ebits +
                                       ", load_guest_word(" + address + ", " + num(element) + "))"));
      } else {
        code.push_back("store_guest_word(" + address + ", lane_read(" + vreg(reg) + ", " + num(index) +
                       ", " + ebits + "), " + num(element) + ");");
      }
    }
  }
  if (post) {
    code.push_back("x[" + num(rn) + "] += " + (rm == 31 ? num(bytes) + "ul" : "x[" + num(rm) + "]") + ";");
  }
  code.push_back("}");
  return make(std::move(code), pc);
}

// LDR/STR of B and H registers (unsigned offset, unscaled, pre- and post-index).
inline std::optional<translate_core::Instruction> narrow_scalar_memory(std::uint32_t bits,
                                                                        std::int64_t pc,
                                                                        bool cached) {
  const auto top = bits & 0xffc00000U;
  const auto low = bits & 0xffe00000U;
  const bool scaled = top == 0x3d400000U || top == 0x3d000000U || top == 0x7d400000U ||
                      top == 0x7d000000U;
  const bool unscaled = (low == 0x3c400000U || low == 0x3c000000U || low == 0x7c400000U ||
                         low == 0x7c000000U) && field(bits, 10, 2) != 2;
  if (!scaled && !unscaled) return std::nullopt;
  const std::int64_t element = field(bits, 30, 1) ? 2 : 1;
  const bool load = field(bits, 22, 1) != 0;
  const auto rn = field(bits, 5, 5);
  const auto rt = field(bits, 0, 5);
  std::string address;
  std::string writeback;
  if (scaled) {
    address = "x[" + num(rn) + "] + " + num(std::int64_t(field(bits, 10, 12)) * element) + "ul";
  } else {
    const std::int64_t immediate = std::int64_t(field(bits, 12, 9)) - (field(bits, 20, 1) ? 512 : 0);
    const auto mode = field(bits, 10, 2);
    address = mode == 1 ? "x[" + num(rn) + "]" : "x[" + num(rn) + "] + ulong(" + num(immediate) + ")";
    if (mode != 0) writeback = "x[" + num(rn) + "] += ulong(" + num(immediate) + ");";
  }
  std::vector<std::string> code{"{ ulong address = " + address + ";"};
  for (auto& line : access(cached, pc, element, !load)) code.push_back(line);
  if (load) {
    code.push_back(slot(rt, 0) + " = load_guest_word(memory + p, " + num(element) + "); " + slot(rt, 1) + " = 0ul;");
  } else {
    code.push_back("store_guest_word(memory + p, " + slot(rt, 0) + ", " + num(element) + ");");
  }
  if (!writeback.empty()) code.push_back(writeback);
  code.push_back("}");
  return make(std::move(code), pc);
}

// SIMD modified immediate (MOVI, MVNI, ORR, BIC) for every cmode except FMOV.
inline std::optional<translate_core::Instruction> modified_immediate(std::uint32_t bits,
                                                                      std::int64_t pc) {
  if ((bits & 0x9ff80400U) != 0x0f000400U) return std::nullopt;
  const auto cmode = field(bits, 12, 4);
  const bool op = field(bits, 29, 1) != 0;
  const bool q = field(bits, 30, 1) != 0;
  const auto rd = field(bits, 0, 5);
  const std::uint64_t imm8 = (field(bits, 16, 3) << 5) | field(bits, 5, 5);
  std::uint64_t value = 0;
  bool logical = false;
  int width = 32;
  if (cmode < 8) {
    value = imm8 << (8 * (cmode >> 1));
    logical = (cmode & 1) != 0;
  } else if (cmode < 12) {
    value = imm8 << (8 * ((cmode >> 1) & 1));
    logical = (cmode & 1) != 0;
    width = 16;
  } else if (cmode < 14) {
    value = (imm8 << (8 * (1 + (cmode & 1)))) | ((std::uint64_t(1) << (8 * (1 + (cmode & 1)))) - 1);
  } else if (cmode == 14) {
    width = 8;
    if (!op) {
      value = imm8;
    } else {
      width = 64;
      for (int byte = 0; byte < 8; ++byte)
        if (imm8 & (1U << byte)) value |= std::uint64_t(0xff) << (8 * byte);
    }
  } else {
    return std::nullopt;
  }
  if (cmode != 14 && !logical) value = op ? ~value : value;
  // Replicate across the lane then across the vector.
  const auto lane_mask = width == 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << width) - 1;
  value &= lane_mask;
  std::uint64_t pattern = value;
  for (int filled = width; filled < 64; filled *= 2) pattern |= pattern << filled;
  const std::string constant = "ulong2(" + std::to_string(pattern) + "ul, " + (q ? std::to_string(pattern) : "0") + "ul)";
  std::string expression;
  if (logical) {
    expression = op ? "(" + vreg(rd) + " & ~" + constant + ")" : "(" + vreg(rd) + " | " + constant + ")";
    if (!q) expression = "(" + expression + " & ulong2(~0ul, 0ul))";
  } else {
    expression = constant;
  }
  return make({assign(rd, expression)}, pc);
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

// Three-register different-width integer ops (SADDL, UADDW, SMULL, UMLAL and friends).
inline std::optional<translate_core::Instruction> widening(std::uint32_t bits, std::int64_t pc) {
  if (field(bits, 24, 5) != 0x0e) return std::nullopt;
  const auto rd = field(bits, 0, 5);
  const auto rn = field(bits, 5, 5);
  const auto rm = field(bits, 16, 5);
  const bool u = field(bits, 29, 1) != 0;
  const bool q = field(bits, 30, 1) != 0;
  const auto size = field(bits, 22, 2);
  const auto width_text = num(8 << size);
  const auto is_signed = boolean(!u);
  const auto n = vreg(rn);
  const auto m = vreg(rm);
  if ((bits & 0x00200c00U) == 0x00200000U) {
    const auto opcode = field(bits, 12, 4);
    if (size == 3) return std::nullopt;
    int op = -1;
    bool wide_first = false;
    switch (opcode) {
      case 0: op = 0; break;
      case 1: op = 0; wide_first = true; break;
      case 2: op = 1; break;
      case 3: op = 1; wide_first = true; break;
      case 5: op = 6; break;
      case 7: op = 2; break;
      case 8: op = 4; break;
      case 10: op = 5; break;
      case 12: op = 3; break;
      default: return std::nullopt;
    }
    return make({assign(rd, "vop_long(" + num(op) + "u, " + vreg(rd) + ", " + n + ", " + m + ", " + width_text + ", " + is_signed + ", " + boolean(q) + ", " + boolean(wide_first) + ")")}, pc);
  }
  return std::nullopt;
}

inline std::optional<translate_core::Instruction> integer(std::uint32_t bits, std::int64_t pc) {
  if (field(bits, 24, 5) != 0x0e) return std::nullopt;
  const auto rd = field(bits, 0, 5);
  const auto rn = field(bits, 5, 5);
  const auto rm = field(bits, 16, 5);
  const bool u = field(bits, 29, 1) != 0;
  const bool q = field(bits, 30, 1) != 0;
  const auto size = field(bits, 22, 2);
  const int width = 8 << size;
  const auto total = num(q ? 128 : 64);
  const auto n = vreg(rn);
  const auto m = vreg(rm);
  const auto width_text = num(width);
  const auto is_signed = boolean(!u);
  const auto matrix = bits & 0xffe0fc00U;
  if (matrix == 0x4e80a400U || matrix == 0x6e80a400U || matrix == 0x4e80ac00U) {
    const bool signedLeft = matrix == 0x4e80a400U;    // SMMLA
    const bool signedRight = matrix != 0x6e80a400U;  // SMMLA or USMMLA
    return make({assign(rd, "vop_i8mm(" + vreg(rd) + ", " + n + ", " + m + ", " +
                        boolean(signedLeft) + ", " + boolean(signedRight) + ")")}, pc);
  }
  if ((bits & 0x00200400U) == 0x00200400U) {
    const auto opcode = field(bits, 11, 5);
    if (size == 3 && !q) return std::nullopt;
    std::string expression;
    switch (opcode) {
      case 0x0c: case 0x0d:
        if (size == 3) return std::nullopt;
        expression = "vop_int_binary(" + num(opcode == 0x0c ? 0 : 1) + "u, " + n + ", " + m + ", " + width_text + ", " + total + ", " + is_signed + ")";
        break;
      case 0x11:
        if (u) return std::nullopt;
        expression = "vop_int_binary(2u, " + n + ", " + m + ", " + width_text + ", " + total + ", false)";
        break;
      case 0x14: case 0x15:
        if (size == 3) return std::nullopt;
        expression = "vop_int_pairwise(" + num(opcode == 0x14 ? 1 : 2) + "u, " + n + ", " + m + ", " + width_text + ", " + total + ", " + is_signed + ")";
        break;
      case 0x17:
        if (u) return std::nullopt;
        expression = "vop_int_pairwise(0u, " + n + ", " + m + ", " + width_text + ", " + total + ", false)";
        break;
      default:
        return std::nullopt;
    }
    return make({assign(rd, expression)}, pc);
  }
  if ((bits & 0x00300c00U) == 0x00200800U && (bits & 0x000e0000U) == 0) {
    const auto opcode = field(bits, 12, 5);
    if (size == 3 && !q && opcode != 0x02 && opcode != 0x06) return std::nullopt;
    std::string expression;
    int unary = -1;
    if (!u && opcode == 0x08) unary = 0;
    else if (u && opcode == 0x08) unary = 1;
    else if (u && opcode == 0x09) unary = 2;
    else if (!u && opcode == 0x0a) unary = 3;
    else if (!u && opcode == 0x0b) unary = 4;
    else if (u && opcode == 0x0b) unary = 5;
    if (unary >= 0) {
      expression = "vop_int_unary(" + num(unary) + "u, " + n + ", " + width_text + ", " + total + ")";
    } else if (opcode == 0x02 || opcode == 0x06) {
      if (size == 3) return std::nullopt;
      expression = "vop_addlp(" + vreg(rd) + ", " + n + ", " + width_text + ", " + total + ", " + is_signed + ", " + boolean(opcode == 6) + ")";
    } else {
      return std::nullopt;
    }
    return make({assign(rd, expression)}, pc);
  }
  return widening(bits, pc);
}

inline std::optional<translate_core::Instruction> system_hints(std::uint32_t bits, std::int64_t pc,
                                                                bool) {
  if (bits == 0xd503203fU) return make({}, pc);
  if (bits == 0xd50330ffU) return make({"threadgroup_barrier(mem_flags::mem_device);"}, pc);
  // DCZID_EL0: report DC ZVA as prohibited so guests use ordinary stores.
  if ((bits & 0xffffffe0U) == 0xd53b00e0U) {
    const auto rt = field(bits, 0, 5);
    if (rt == 31) return make({}, pc);
    return make({"x[" + num(rt) + "] = 16ul;"}, pc);
  }
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
  if (auto result = system_hints(bits, pc, cached)) return result;
  if (auto result = structure_memory(bits, pc, cached)) return result;
  if (auto result = narrow_scalar_memory(bits, pc, cached)) return result;
  if (auto result = modified_immediate(bits, pc)) return result;
  if (auto result = fixed_point(bits, pc)) return result;
  if (auto result = floating_point(bits, pc)) return result;
  if (auto result = integer(bits, pc)) return result;
  return std::nullopt;
}

}  // namespace detail

inline std::optional<translate_core::Instruction> lower(std::uint32_t bits, std::int64_t pc,
                                                         bool cached) {
  return detail::lower(bits, pc, cached);
}

inline std::optional<translate_core::Instruction> lower_widening(std::uint32_t bits,
                                                                  std::int64_t pc) {
  return detail::widening(bits, pc);
}

}  // namespace arm64metal::vector_ops
