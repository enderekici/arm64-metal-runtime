#include "guest_format.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> bytes(const std::string& text) {
  return {text.begin(), text.end()};
}

std::string text(const std::vector<std::uint8_t>& value) {
  return {value.begin(), value.end()};
}

}  // namespace

int main() {
  // Darwin ARM64 variadic arguments occupy 8-byte stack slots. The first
  // seven values cover the narrow unsigned int in its slot; the 64-bit values
  // follow in consecutive stack slots.
  const std::array<std::uint64_t, 11> stack_slots{
      1, 2, 3, 4, 5, 6, 0x10000000aULL, 0x0123456789abcdefULL,
      0x100000005ULL, 0, static_cast<std::uint32_t>(-42)};
  std::size_t next_slot = 0;
  const auto result = arm64metal::format_guest_output(
      bytes("%d %d %d %d %d %d|%02x|%#llx|%zu|%#x|%05d\n"), [&]() {
        assert(next_slot < stack_slots.size());
        return stack_slots[next_slot++];
      }, [](std::uint64_t) -> std::vector<std::uint8_t> {
        assert(false && "unexpected guest string argument");
        return {};
      });
  assert(text(result) == "1 2 3 4 5 6|0a|0x123456789abcdef|4294967301|0|-0042\n");
  assert(next_slot == stack_slots.size());

  const std::array<std::uint64_t, 1> narrow_slot{0x10000000aULL};
  std::size_t narrow_index = 0;
  const auto narrow = arm64metal::format_guest_output(
      bytes("%02x"), [&]() { return narrow_slot[narrow_index++]; },
      [](std::uint64_t) { return std::vector<std::uint8_t>{}; });
  assert(text(narrow) == "0a");

  const auto padded = arm64metal::format_guest_output(
      bytes("%#08x"), []() { return std::uint64_t{0x2a}; },
      [](std::uint64_t) { return std::vector<std::uint8_t>{}; });
  assert(text(padded) == "0x00002a");

  const auto alternate_zero = arm64metal::format_guest_output(
      bytes("%#x"), []() { return std::uint64_t{0}; },
      [](std::uint64_t) { return std::vector<std::uint8_t>{}; });
  assert(text(alternate_zero) == "0");

  const auto signed_zero_padding = arm64metal::format_guest_output(
      bytes("%05d"), []() {
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(-42));
      }, [](std::uint64_t) { return std::vector<std::uint8_t>{}; });
  assert(text(signed_zero_padding) == "-0042");

  const std::array<std::uint64_t, 2> existing_arguments{42, 0x1000};
  std::size_t existing_index = 0;
  const auto preserved = arm64metal::format_guest_output(
      bytes("value=%d text=%s %%"), [&]() {
        assert(existing_index < existing_arguments.size());
        return existing_arguments[existing_index++];
      }, [](std::uint64_t) { return bytes("ok"); });
  assert(text(preserved) == "value=42 text=ok %");
  assert(existing_index == existing_arguments.size());

  const auto expect_rejected = [](const std::string& format) {
    try {
      (void)arm64metal::format_guest_output(bytes(format),
          []() { return std::uint64_t{1}; },
          [](std::uint64_t) { return std::vector<std::uint8_t>{}; });
    } catch (const arm64metal::GuestFormatError&) {
      return;
    }
    assert(false && "guest format must be rejected");
  };
  expect_rejected("%");
  expect_rejected("%q");
  expect_rejected("%#");
  expect_rejected("%4097x");
  bool argument_read = false;
  try {
    (void)arm64metal::format_guest_output(bytes("%999999999999999999999999x"),
        [&]() {
          argument_read = true;
          return std::uint64_t{1};
        }, [](std::uint64_t) { return std::vector<std::uint8_t>{}; });
  } catch (const arm64metal::GuestFormatError&) {
  }
  assert(!argument_read);
  return 0;
}
