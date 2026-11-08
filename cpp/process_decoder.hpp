#pragma once
#include "translate_core.hpp"
#include <map>
#include <optional>
#include <string>
namespace arm64metal {
using Instruction = translate_core::Instruction;
// Lower a process import to GPU code or a host-service boundary.
Instruction hostCall(const std::string &name,
                     std::optional<int64_t> returnAddress, bool cached);
// Decode one process ARM64 instruction into Metal code and CFG successors.
// Guest arithmetic, memory, SIMD and authentication execute only in Metal.
// Throws std::runtime_error for unsupported or reserved encodings.
Instruction decode_process(uint32_t bits, int64_t pc,
                           const std::map<int64_t, std::string> &imports,
                           const std::map<int64_t, int64_t> &guestStubs,
                           bool cached);
} // namespace arm64metal
