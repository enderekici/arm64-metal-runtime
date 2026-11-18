#pragma once

#include "process_decoder.hpp"
#include "process_host.hpp"

namespace arm64metal {

// Lower the SME instructions used by the compiler's ZA save/restore sequence.
// Keeping this separate from the main process decoder lets the SME control and
// scheduler work land independently.
inline std::optional<Instruction> decode_sme_lazy(uint32_t bits, int64_t pc,
                                                    bool cached) {
  (void)cached;
  const auto next = pc + 4;
  const auto signExtend = [](uint32_t value, unsigned width) -> int64_t {
    const auto sign = uint32_t{1} << (width - 1);
    return static_cast<int64_t>(value ^ sign) - static_cast<int64_t>(sign);
  };
  const auto emit = [&](std::vector<std::string> code) {
    return std::optional<Instruction>(Instruction{std::move(code), {next}});
  };
  const auto reg = [](uint32_t r, bool sp) {
    if (r == 31 && !sp) return std::string("0ul");
    return std::string("x[") + std::to_string(r) + "]";
  };

  // STR/LDR ZA[Wv, #imm], [Xn|SP, #imm, MUL VL]. Wv selects one of the
  // architectural W12-W15 registers; SVL.B is the ZA array-vector count.
  constexpr uint32_t zaMask = 0xffff9c10u;
  if ((bits & zaMask) == 0xe1200000u || (bits & zaMask) == 0xe1000000u) {
    const bool load = (bits & 0x00200000u) == 0;
    const auto wv = 12u + ((bits >> 13) & 3u);
    const auto rn = (bits >> 5) & 31u;
    const auto imm = bits & 15u;
    const std::string base = reg(rn, true);
    const std::string address = "(" + base + " + " + std::to_string(imm) +
                                "ul * x[" + std::to_string(guestSMESVLBytes) + "])";
    const auto status = load ? guestReadAlignmentFaultStatus
                             : guestWriteAlignmentFaultStatus;
    const auto access = load ? "false" : "true";
    const std::string spAlignment = rn == 31
        ? "if ((x[31] & 15ul) != 0ul) { x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3); x[" +
              std::to_string(guestFaultAddress) + "] = x[31]; x[33] = " +
              std::to_string(pc) + "ul; x[34] = " +
              std::to_string(status) + "; return; } "
        : "";
    const std::string code =
        "{ if ((x[" + std::to_string(guestSMESVCR) + "] & 2ul) == 0ul) { x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3); x[33] = " + std::to_string(pc) +
        "ul; x[34] = 14; return; } "
        "ulong address = " + address + "; "
        + spAlignment +
        "ulong offset = guest_offset(address, x[" + std::to_string(guestSMESVLBytes) + "], size, writable, " +
        access + ", x, resident, config); "
        "if (offset == ~0ul) { x[" + std::to_string(guestFaultAddress) + "] = address; x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3); x[33] = " +
        std::to_string(pc) + "ul; x[34] = 2; return; } "
        "uint vector = (uint(x[" + std::to_string(wv) + "]) + " +
        std::to_string(imm) + "u) % uint(x[" + std::to_string(guestSMESVLBytes) + "]); "
        "for (uint q = 0; q < 8; ++q) { ulong lane = 0ul; " +
        (load
             ? "for (uint b = 0; b < 8; ++b) lane |= ulong(memory[offset + 8ul * q + b]) << (8u * b); x[" + std::to_string(guestSMEZAStart) + "ul + 8ul * ulong(vector) + q] = lane;"
             : "lane = x[" + std::to_string(guestSMEZAStart) + "ul + 8ul * ulong(vector) + q]; for (uint b = 0; b < 8; ++b) memory[offset + 8ul * q + b] = uchar(lane >> (8u * b));") +
        " } }";
    return emit({code});
  }

  // ZERO {ZA}: preserve all other architectural SME state.
  if (bits == 0xc00800ffu) {
    return emit({"if ((x[" + std::to_string(guestSMESVCR) + "] & 2ul) == 0ul) { x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3); x[33] = " +
                 std::to_string(pc) + "ul; x[34] = 14; return; }",
                 "for (ulong i = 0; i < 512ul; ++i) x[" +
                 std::to_string(guestSMEZAStart) + "ul + i] = 0ul;"});
  }

  // ADDSVL Xd|SP, Xn|SP, #imm and RDSVL Xd, #imm.
  if ((bits & 0xffe0f800u) == 0x04205800u) {
    const auto rd = bits & 31u;
    const auto rn = (bits >> 16) & 31u;
    const auto imm = signExtend((bits >> 5) & 63u, 6);
    const std::string rhs = reg(rn, true) + " + (" + std::to_string(imm) +
                            "l * x[" + std::to_string(guestSMESVLBytes) + "])";
    if (rd == 31)
      return emit({"x[31] = " + rhs + ";"});
    return emit({"x[" + std::to_string(rd) + "] = " + rhs + ";"});
  }
  if ((bits & 0xfffff800u) == 0x04bf5800u) {
    const auto rd = bits & 31u;
    const auto imm = signExtend((bits >> 5) & 63u, 6);
    if (rd == 31) return emit({});
    return emit({"x[" + std::to_string(rd) + "] = ulong(" +
                 std::to_string(imm) + "l * x[" + std::to_string(guestSMESVLBytes) + "]);"});
  }
  return std::nullopt;
}

}  // namespace arm64metal
