#ifndef ARM64METAL_TRANSLATE_CORE_HPP
#define ARM64METAL_TRANSLATE_CORE_HPP

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace arm64metal::translate_core {

// One lowered ARM64 instruction. `code` contains generated Metal statements;
// `next` lists guest PCs which may execute next.
struct Instruction {
  std::vector<std::string> code;
  std::vector<std::int64_t> next;

  // An instruction ends its basic block if control flow is not straight-line.
  bool ends_block_at(std::int64_t pc) const;
};

// Decode one supported integer ARM64 instruction to the runtime's Metal form.
// Throws std::runtime_error for unsupported encodings. `address_suffix` is
// appended to generated PC literals (normally "u" or "ul").
Instruction decode(std::uint32_t bits, std::int64_t pc,
                   const std::string& address_suffix = "u");

// Return reachable instruction addresses that begin emitted switch cases.
std::set<std::int64_t> block_entries(
    const std::map<std::int64_t, Instruction>& instructions,
    std::int64_t entry);

// Render basic blocks as Metal switch cases, matching Translate.swift's
// ordering, indentation, fallthrough handling, and PC literal suffix.
std::string render_blocks(
    const std::map<std::int64_t, Instruction>& instructions,
    std::int64_t entry, const std::string& address_suffix = "u");

}  // namespace arm64metal::translate_core

#endif  // ARM64METAL_TRANSLATE_CORE_HPP
