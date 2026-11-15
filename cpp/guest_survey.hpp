#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Durable diagnostics for the guest's kernel boundary.
//
// ARM64_METAL_SURVEY=1 turns unhandled-service fatals into logged errors so one
// run reveals every missing service. Lines go to ARM64_METAL_SURVEY_LOG (append)
// or stderr, once per (kind, num, sub).
//
// ARM64_METAL_BOUNDARY_TRACE=<path> appends every syscall, Mach trap and Mach
// message id the guest issues, with its key arguments.
namespace arm64metal::guest_survey {

[[nodiscard]] bool enabled() noexcept;
[[nodiscard]] bool boundary_enabled() noexcept;

// No-op unless enabled(). `detail` is appended to the line but is not part of
// the deduplication key.
void note(std::string_view kind, std::int64_t num, std::string_view sub, std::uint64_t pc,
          std::uint64_t lr, std::uint64_t thread, std::string_view detail = {});

// No-op unless boundary_enabled().
void boundary(std::string_view kind, std::int64_t num, std::string_view sub,
              const std::uint64_t* args, std::size_t count, std::uint64_t pc,
              std::uint64_t lr, std::uint64_t thread);

}  // namespace arm64metal::guest_survey
