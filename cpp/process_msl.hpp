#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace arm64metal::process_msl {

struct WritableRange {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;  // exclusive
};

// Per-image values interpolated into the process Metal kernel. State-slot and
// pointer-token constants remain the fixed values used by ProcessTranslation.swift.
struct Config {
  std::uint64_t minimum_image_address = 16'384;
  std::uint64_t shared_guest_base = 0x1'0000'0000ULL;
  std::uint64_t linear_image_end = 0;
  std::uint64_t mapping_backing_end = 0;
  std::vector<WritableRange> writable_regions;
};

// Render the shared process Metal source with caller-rendered switch cases and
// the guest vector crypto helpers. This emits shader source only; it does not
// run guest instructions on the CPU.
[[nodiscard]] std::string render_source(std::string_view rendered_cases,
                                        const Config& config);

}  // namespace arm64metal::process_msl
