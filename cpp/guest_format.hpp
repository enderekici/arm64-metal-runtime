#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace arm64metal {

// Thrown for the same unsupported/incomplete guest printf format cases as the
// Swift implementation. Callers may translate this into their runtime error.
class GuestFormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using GuestArgumentReader = std::function<std::uint64_t()>;
using GuestStringReader = std::function<std::vector<std::uint8_t>(std::uint64_t)>;

// Format the supported integer, character, string, and %% conversions. The
// callbacks supply Darwin ARM64 stack arguments and guest-memory strings.
std::vector<std::uint8_t> format_guest_output(
    const std::vector<std::uint8_t>& format,
    const GuestArgumentReader& argument,
    const GuestStringReader& string);

}  // namespace arm64metal
