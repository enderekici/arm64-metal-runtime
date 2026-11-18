#include "process_sme_ops.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main() {
  using arm64metal::lower_sme_instruction;
  const auto signed_op = lower_sme_instruction(0xa08da8e3U, 0x1000);
  const auto unsigned_op = lower_sme_instruction(0xa1b37b82U, 0x2000);
  if (!signed_op || signed_op->next != std::vector<std::int64_t>{0x1004} ||
      signed_op->code.size() != 2 ||
      signed_op->code[0].find("(x[881] & 3ul) != 3ul") == std::string::npos ||
      signed_op->code[1] != "sme_smopa8(x, 3u, 2u, 5u, 7u, 13u, true);") {
    std::cerr << "signed SMOPA decoder fields mismatch\n";
    return 1;
  }
  if (!unsigned_op ||
      unsigned_op->code[1] != "sme_smopa8(x, 2u, 6u, 3u, 28u, 19u, false);") {
    std::cerr << "unsigned UMOPA decoder fields mismatch\n";
    return 1;
  }
  const auto ptrue = lower_sme_instruction(0x2598e3e0U, 0x3000);
  const auto mov_imm = lower_sme_instruction(0x2538c120U, 0x3004);
  const auto mov_zero = lower_sme_instruction(0x25b8c000U, 0x3008);
  const auto mova = lower_sme_instruction(0xc0820000U, 0x300c);
  const auto mova_fields = lower_sme_instruction(0xc0824d71U, 0x300d);
  const auto str_z = lower_sme_instruction(0xe5804000U, 0x3010);
  const auto str_z_offset = lower_sme_instruction(0xe5804691U, 0x3011);
  const auto str_z_negative_sp = lower_sme_instruction(0xe5bf5fe2U, 0x3012);
  const auto ldr_z = lower_sme_instruction(0x85804000U, 0x3014);
  const auto ldr_z_offset = lower_sme_instruction(0x85804bffU, 0x3015);
  const auto cntw = lower_sme_instruction(0x04a0e3e8U, 0x3018);
  const auto cntw_zr = lower_sme_instruction(0x04a0e3ffU, 0x3019);
  if (!ptrue || ptrue->code[1] != "sme_ptrue_s(x, 0u);" ||
      !mov_imm || mov_imm->code[1] != "sme_mov_imm_b(x, 0u, 9u);" ||
      !mov_zero || mov_zero->code[1] != "sme_mov_zero_s(x, 0u);" ||
      !mova || mova->code[1] != "sme_mova_s(x, 0u, 0u, 0u, 0u, 0u, false);" ||
      !mova_fields ||
      mova_fields->code[1] != "sme_mova_s(x, 17u, 3u, 2u, 2u, 3u, false);" ||
      !str_z || str_z->code[1].find("memory[offset + i] = sme_z_byte") ==
                    std::string::npos ||
      !str_z_offset ||
      str_z_offset->code[1].find("x[20] + (64ul)") == std::string::npos ||
      str_z_offset->code[1].find("x[33] = 12305ul") == std::string::npos ||
      str_z_offset->code[1].find("uint(z) | (uint(n) << 1)") ==
          std::string::npos ||
      !str_z_negative_sp ||
      str_z_negative_sp->code[1].find("x[31] + (-64ul)") ==
          std::string::npos ||
      !ldr_z || ldr_z->code[1].find("sme_set_z_byte(x, 0u, i, memory") ==
                    std::string::npos ||
      !ldr_z_offset ||
      ldr_z_offset->code[1].find("x[31] + (128ul)") == std::string::npos ||
      !cntw || cntw->code[1] != "x[8] = x[883] / 4ul;") {
    std::cerr << "original ACLE path instruction decoding mismatch\n";
    return 1;
  }
  if (!cntw_zr || !cntw_zr->code[1].empty()) {
    std::cerr << "CNTW XZR incorrectly writes the stack pointer\n";
    return 1;
  }
  if (lower_sme_instruction(0xa0c00000U, 0x3000)) {
    std::cerr << "unexpectedly accepted an unsupported SME opcode\n";
    return 1;
  }
  std::cout << "streaming SME setup, matrix, move, memory, and CNTW forms decode\n";
  return 0;
}
