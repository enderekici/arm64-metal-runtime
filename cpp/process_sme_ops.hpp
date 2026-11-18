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
  constexpr std::uint32_t mask = 0xffc0e000U;
  if ((bits & mask) != base) return std::nullopt;
  const auto zn = bits & 31U;
  const auto rn = (bits >> 5) & 31U;
  const auto encoded_offset = (((bits >> 16) & 63U) << 3) |
                              ((bits >> 10) & 7U);
  const auto signed_offset = encoded_offset & 256U
                                 ? static_cast<std::int32_t>(encoded_offset) - 512
                                 : static_cast<std::int32_t>(encoded_offset);
  const std::string address = "x[" + std::to_string(rn) + "] + (" +
                              std::to_string(signed_offset * 64) + "ul)";
  std::string offset;
  std::string check;
  if (cached) {
    offset = "guest_offset(address, 64ul, size, writable, " +
             std::string(load ? "false" : "true") + ", x, resident, config)";
    check = "offset == ~0ul";
  } else {
    offset = "address";
    check = "size < 64ul || address < 16384ul || address > size - 64ul";
    if (!load) check += " || !guest_write(address, 64ul, writable)";
  }
  const std::string transfer =
      "{ ulong address = " + address + "; ulong offset = " + offset +
      "; if (" + check + ") { x[874] = address; x[33] = " +
      std::to_string(pc) +
      "ul; x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3); "
      "x[34] = 2; return; } "
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

  // PTRUE Pn.<T>, supporting the architectural predicate-count patterns.
  constexpr std::uint32_t ptrue_mask = 0xff3ffc10U;
  constexpr std::uint32_t ptrue_base = 0x2598e3e0U & ptrue_mask;
  if ((bits & ptrue_mask) == ptrue_base) {
    const auto element_bytes = 1U << ((bits >> 22) & 3U);
    return one(pc, "sme_ptrue(x, " + std::to_string(bits & 15U) + "u, " +
                   std::to_string((bits >> 5) & 31U) + "u, " +
                   std::to_string(element_bytes) + "u);", false);
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

  // MOV Zd.S, Pg/M, ZAn{H,V}.S[Ws,offset].
  constexpr std::uint32_t mova_fixed_mask = 0xffff0200U;
  if ((bits & mova_fixed_mask) == 0xc0820000U) {
    const auto zd = bits & 31U;
    const auto offset = (bits >> 5) & 3U;
    const auto tile = (bits >> 7) & 3U;
    const auto pred = (bits >> 10) & 7U;
    const auto slice_reg = (bits >> 13) & 3U;
    const bool vertical = ((bits >> 15) & 1U) != 0;
    return one(pc, "sme_mova_s(x, " + std::to_string(zd) + "u, " +
                        std::to_string(pred) + "u, " + std::to_string(tile) +
                        "u, " + std::to_string(slice_reg) + "u, " +
                        std::to_string(offset) + "u, " +
                        (vertical ? "true" : "false") + ");", true);
  }

  // STR/LDR Zt,[Xn, #imm, MUL VL] with signed vector-length offsets.
  if (auto lowered = sme_ops_detail::memory(bits, pc, cached, false))
    return lowered;
  if (auto lowered = sme_ops_detail::memory(bits, pc, cached, true))
    return lowered;

  // CNTB/H/W/D Xd with the architectural predicate-count patterns.
  if ((bits & 0xff20fc00U) == 0x0420e000U) {
    const auto rd = bits & 31U;
    const auto element_bytes = 1U << ((bits >> 22) & 3U);
    const auto pattern = (bits >> 5) & 31U;
    const auto multiplier = ((bits >> 16) & 15U) + 1U;
    const std::string value = "sme_decode_pred_count(uint(x[883]) / " +
        std::to_string(element_bytes) + "u, " + std::to_string(pattern) +
        "u) * " + std::to_string(multiplier) + "u";
    return one(pc, rd == 31U ? std::string{} :
               "x[" + std::to_string(rd) + "] = " + value + ";", false);
  }
  return std::nullopt;
}

}  // namespace arm64metal
