#pragma once

#include "process_decoder.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace arm64metal {
namespace sme_ops_detail {

inline std::optional<Instruction> one(std::int64_t pc, std::string statement,
                                      bool needs_za) {
  const auto mode = needs_za ? "3ul" : "1ul";
  std::vector<std::string> code{
      "if ((x[881] & " + std::string(mode) + ") != " + mode +
          ") { x[33] = " + std::to_string(pc) +
          "ul; x[34] = 14; return; }",
      std::move(statement)};
  return Instruction{std::move(code), {pc + 4}};
}

inline std::optional<Instruction> memory(std::uint32_t bits, std::int64_t pc,
                                         bool cached, bool load) {
  const auto base = load ? 0x85804000U : 0xe5804000U;
  const auto mask = ~((31U << 5) | 31U);
  if ((bits & mask) != base) return std::nullopt;
  const auto zn = bits & 31U;
  const auto rn = (bits >> 5) & 31U;
  if (rn == 31U) return std::nullopt;
  const std::string address = "x[" + std::to_string(rn) + "]";
  std::string offset;
  std::string check;
  if (cached) {
    offset = "guest_offset(address, 64ul, size, writable, " +
             std::string(load ? "false" : "true") + ", x, resident, config)";
    check = "offset == ~0ul";
  } else {
    offset = "address";
    check = "address < 16384ul || address > size - 64ul";
    if (!load) check += " || !guest_write(address, 64ul, writable)";
  }
  const std::string transfer =
      "{ ulong address = " + address + "; ulong offset = " + offset +
      "; if (" + check + ") { x[874] = address; x[34] = 2; return; } "
      "for (uint i = 0; i < 64u; ++i) " +
      std::string(load ? "sme_set_z_byte(x, " + std::to_string(zn) +
                            "u, i, memory[offset + i]);"
                       : "memory[offset + i] = sme_z_byte(x, " +
                            std::to_string(zn) + "u, i);") +
      " }";
  return one(pc, transfer, false);
}

}  // namespace sme_ops_detail

// Lower the first streaming integer-matrix instruction slice plus the SVE
// setup/readback operations used by the original ACLE path. Unsupported SME
// and SVE encodings remain the generic decoder's responsibility.
inline std::optional<Instruction> lower_sme_instruction(std::uint32_t bits,
                                                        std::int64_t pc,
                                                        bool cached = true) {
  using sme_ops_detail::one;

  constexpr std::uint32_t mopa_fields = (3U << 0) | (31U << 5) | (7U << 10) |
                                        (7U << 13) | (31U << 16);
  const std::uint32_t mopa_fixed = bits & ~mopa_fields;
  if (mopa_fixed == 0xa0800000U || mopa_fixed == 0xa1a00000U) {
    const bool is_signed = mopa_fixed == 0xa0800000U;
    const auto tile = bits & 3U;
    const auto zn = (bits >> 5) & 31U;
    const auto pn = (bits >> 10) & 7U;
    const auto pm = (bits >> 13) & 7U;
    const auto zm = (bits >> 16) & 31U;
    return one(pc, "sme_smopa8(x, " + std::to_string(tile) + "u, " +
                        std::to_string(pn) + "u, " + std::to_string(pm) +
                        "u, " + std::to_string(zn) + "u, " +
                        std::to_string(zm) + "u, " +
                        (is_signed ? "true" : "false") + ");", true);
  }

  // PTRUE Pn.S, all lanes, the form emitted by the original ACLE probe.
  if ((bits & 0xfffffff0U) == 0x2598e3e0U) {
    return one(pc, "sme_ptrue_s(x, " + std::to_string(bits & 15U) + "u);",
               false);
  }

  // MOV Zb,#imm8 and the MOV Zs,#0 canonical zeroing form.
  constexpr std::uint32_t movb_fields = (31U << 0) | (255U << 5);
  if ((bits & ~movb_fields) == 0x2538c000U) {
    return one(pc, "sme_mov_imm_b(x, " + std::to_string(bits & 31U) +
                        "u, " + std::to_string((bits >> 5) & 255U) + "u);",
               false);
  }
  if ((bits & ~31U) == 0x25b8c000U) {
    return one(pc, "sme_mov_zero_s(x, " + std::to_string(bits & 31U) + "u);",
               false);
  }

  // MOV Zd.S, P0/M, ZA0H.S[W12,0], the original horizontal ZA read.
  if (bits == 0xc0820000U) {
    return one(pc, "sme_mova_za0h_s_w12(x);", true);
  }

  // STR/LDR Zt,[Xn], no vector-length-scaled offset.
  if (auto lowered = sme_ops_detail::memory(bits, pc, cached, false))
    return lowered;
  if (auto lowered = sme_ops_detail::memory(bits, pc, cached, true))
    return lowered;

  // CNTW Xd, the all-lanes, multiplier-one form.
  if ((bits & ~31U) == 0x04a0e3e0U) {
    return one(pc, "x[" + std::to_string(bits & 31U) + "] = x[883] / 4ul;",
               false);
  }
  return std::nullopt;
}

}  // namespace arm64metal
