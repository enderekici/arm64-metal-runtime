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

// Per-image memory metadata used to build shared process/interpreter buffers.
struct Config {
  std::uint64_t minimum_image_address = 16'384;
  std::uint64_t shared_guest_base = 0x1'0000'0000ULL;
  std::uint64_t linear_image_end = 0;
  std::uint64_t mapping_backing_end = 0;
  std::vector<WritableRange> writable_regions;
};

// Render the shared process Metal source with caller-rendered switch cases and
// the guest vector crypto helpers. This emits shader source only; it does not
// run guest instructions on the CPU. A plain arm64 main executable runs with
// pointer authentication disabled on hardware, so authenticating an unsigned
// canonical pointer succeeds when plain_arm64_main is set.
[[nodiscard]] std::string render_source(std::string_view rendered_cases,
                                        bool plain_arm64_main = false);

}  // namespace arm64metal::process_msl
