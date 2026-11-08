#ifndef ARM64METAL_GUEST_VECTOR_CRYPTO_HPP
#define ARM64METAL_GUEST_VECTOR_CRYPTO_HPP

#include <cstdint>
#include <optional>
#include <string_view>

#include "translate_core.hpp"

namespace arm64metal {

// Lowers AES and 64-bit-lane PMULL/PMULL2 instructions, or returns nullopt when
// the instruction is outside this subset. `pc` is the guest instruction address.
std::optional<translate_core::Instruction> lower_vector_crypto(
    std::uint32_t bits, std::int64_t pc);

// Metal helper definitions required by the emitted instructions. The caller is
// responsible for providing lane_read and lane_add in the generated source.
std::string_view vector_crypto_metal_source() noexcept;

}  // namespace arm64metal

#endif  // ARM64METAL_GUEST_VECTOR_CRYPTO_HPP
