#ifndef ARM64METAL_GUEST_CODE_POLICY_HPP
#define ARM64METAL_GUEST_CODE_POLICY_HPP

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace arm64metal {

enum class GuestCodeValidation { unavailable, invalid, valid };

// Security.framework evidence for the exact selected Mach-O slice the loader
// parsed. This is descriptive metadata only; it grants no host VM permissions.
struct GuestCodePolicy {
  GuestCodeValidation validation = GuestCodeValidation::unavailable;
  std::uint32_t signing_flags = 0;
  std::string identity;
  std::array<std::uint8_t, 32> image_sha256{};
  bool hardened_runtime = false;
  bool ad_hoc = false;
  bool allow_jit = false;
  bool trusted_signer = false;
  bool map_jit_eligible = false;
  std::int32_t security_status = 0;
};

// Validate Security.framework metadata against `selected_image`, which must be
// the immutable ARM64 slice retained by the image loader. The function creates
// a private code-object snapshot and substitutes that exact slice before
// validation, avoiding a source-file TOCTOU between parsing and policy lookup.
[[nodiscard]] GuestCodePolicy guest_code_policy(
    const std::filesystem::path& executable,
    const std::vector<std::uint8_t>& selected_image);

}  // namespace arm64metal

#endif  // ARM64METAL_GUEST_CODE_POLICY_HPP
