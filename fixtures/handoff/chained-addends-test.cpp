#include "macho.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void put(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value,
        std::size_t width) {
  for (std::size_t index = 0; index < width; ++index) {
    bytes.at(offset + index) = static_cast<std::uint8_t>(value >> (index * 8));
  }
}

std::vector<std::uint8_t> payload(std::uint16_t pointerFormat, std::uint32_t importFormat,
                                  std::int64_t importAddend, std::uint32_t ordinal = 0) {
  const std::size_t importSize = importFormat == 1 ? 4 : importFormat == 2 ? 8 : 16;
  const std::size_t importsOffset = 60;
  const std::size_t symbolsOffset = importsOffset + importSize;
  std::vector<std::uint8_t> bytes(symbolsOffset + 4, 0);

  put(bytes, 4, 28, 4);
  put(bytes, 8, importsOffset, 4);
  put(bytes, 12, symbolsOffset, 4);
  put(bytes, 16, 1, 4);
  put(bytes, 20, importFormat, 4);
  put(bytes, 28, 1, 4);
  put(bytes, 32, 8, 4);
  put(bytes, 36, 24, 4);
  put(bytes, 40, 4096, 2);
  put(bytes, 42, pointerFormat, 2);
  put(bytes, 56, 1, 2);
  put(bytes, 58, 0, 2);

  const std::uint64_t importRecord = importFormat == 3
      ? (static_cast<std::uint64_t>(ordinal) | (std::uint64_t{0} << 32))
      : ordinal;
  put(bytes, importsOffset, importRecord, importFormat == 3 ? 8 : 4);
  if (importFormat == 2) put(bytes, importsOffset + 4, static_cast<std::uint32_t>(importAddend), 4);
  if (importFormat == 3) put(bytes, importsOffset + 8, static_cast<std::uint64_t>(importAddend), 8);
  bytes[symbolsOffset] = 'x';
  bytes[symbolsOffset + 1] = 0;
  put(bytes, symbolsOffset + 2, 0, 2);

  return bytes;
}

std::int64_t decoded_addend(std::uint16_t pointerFormat, std::uint32_t importFormat,
                            std::int64_t importAddend, std::uint64_t pointer) {
  auto bytes = payload(pointerFormat, importFormat, importAddend);
  std::vector<std::uint8_t> memory(4096, 0);
  put(memory, 0, pointer, 8);
  const auto result = arm64metal::apply_chained_fixups(bytes, 0, 0, memory, {{0, 4096}});
  return result.bindings.at(0).addend;
}

void verify_linker_fixture(const std::filesystem::path& path) {
  const arm64metal::MachO file(path);
  const auto& bytes = file.bytes();
  const auto word = [&file](std::size_t offset) { return file.word(offset); };
  const std::size_t commandCount = word(16);
  const std::size_t commandEnd = 32 + word(20);
  std::size_t cursor = 32;
  std::uint64_t preferredBase = 0;
  bool foundText = false;
  std::size_t fixupOffset = 0;
  std::size_t fixupSize = 0;
  std::vector<std::pair<std::size_t, std::size_t>> mappedRanges;
  std::vector<std::uint8_t> memory;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> segments;

  for (std::size_t index = 0; index < commandCount; ++index) {
    const auto command = word(cursor);
    const std::size_t length = word(cursor + 4);
    assert(length >= 8 && length <= commandEnd - cursor);
    if (command == 0x19) {
      const auto nameEnd = std::find(bytes.begin() + static_cast<std::ptrdiff_t>(cursor + 8),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(cursor + 24), 0);
      const std::string name(bytes.begin() + static_cast<std::ptrdiff_t>(cursor + 8), nameEnd);
      const auto address = arm64metal::read_integer(bytes, cursor + 24, 8);
      const auto size = arm64metal::read_integer(bytes, cursor + 32, 8);
      const auto fileOffset = arm64metal::read_integer(bytes, cursor + 40, 8);
      const auto fileSize = arm64metal::read_integer(bytes, cursor + 48, 8);
      if (name == "__TEXT") {
        preferredBase = address;
        foundText = true;
      }
      segments.emplace_back(address, size);
      assert(fileOffset <= bytes.size() && fileSize <= bytes.size() - fileOffset);
    } else if (command == 0x80000034) {
      fixupOffset = word(cursor + 8);
      fixupSize = word(cursor + 12);
    }
    cursor += length;
  }

  assert(foundText && fixupSize > 0 && fixupOffset <= bytes.size() &&
         fixupSize <= bytes.size() - fixupOffset);
  std::uint64_t imageEnd = preferredBase;
  for (const auto& segment : segments) imageEnd = std::max(imageEnd, segment.first + segment.second);
  memory.resize(static_cast<std::size_t>(imageEnd - preferredBase), 0);
  cursor = 32;
  for (std::size_t index = 0; index < commandCount; ++index) {
    const auto command = word(cursor);
    const std::size_t length = word(cursor + 4);
    if (command == 0x19) {
      const auto address = arm64metal::read_integer(bytes, cursor + 24, 8);
      const auto size = arm64metal::read_integer(bytes, cursor + 32, 8);
      const auto fileOffset = arm64metal::read_integer(bytes, cursor + 40, 8);
      const auto fileSize = arm64metal::read_integer(bytes, cursor + 48, 8);
      const auto start = static_cast<std::size_t>(address - preferredBase);
      mappedRanges.emplace_back(start, start + static_cast<std::size_t>(size));
      std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(fileOffset),
                  static_cast<std::size_t>(fileSize),
                  memory.begin() + static_cast<std::ptrdiff_t>(start));
    }
    cursor += length;
  }
  const std::vector<std::uint8_t> payloadBytes(
      bytes.begin() + static_cast<std::ptrdiff_t>(fixupOffset),
      bytes.begin() + static_cast<std::ptrdiff_t>(fixupOffset + fixupSize));
  const auto result = arm64metal::apply_chained_fixups(
      payloadBytes, preferredBase, 0, memory, mappedRanges);
  const auto binding = std::find_if(result.bindings.begin(), result.bindings.end(),
                                    [](const auto& entry) {
                                      return entry.second.name == "_chained_addend_target";
                                    });
  assert(binding != result.bindings.end());
  assert(binding->second.addend == 12);
}

}  // namespace

int main(int argc, char** argv) {
  constexpr std::uint64_t bind64 = std::uint64_t{1} << 63;
  constexpr std::uint64_t bindArm64e = std::uint64_t{1} << 62;
  constexpr std::uint64_t auth = std::uint64_t{1} << 63;

  const std::uint64_t generic64 = bind64 | (std::uint64_t{200} << 24);
  assert(decoded_addend(2, 2, -12, generic64) == 188);

  const std::uint64_t arm64e = bindArm64e | (std::uint64_t{0x7fffb} << 32);
  assert(decoded_addend(9, 3, 12, arm64e) == 7);
  assert(decoded_addend(9, 1, 0,
                        bindArm64e | (std::uint64_t{0x3ffff} << 32)) == 262143);
  assert(decoded_addend(9, 1, 0,
                        bindArm64e | (std::uint64_t{0x40000} << 32)) == -262144);

  const std::uint64_t authBind = bindArm64e | auth | (std::uint64_t{2} << 49);
  assert(decoded_addend(9, 3, -12, authBind) == -12);
  assert(decoded_addend(9, 3, std::numeric_limits<std::int64_t>::min(), authBind) ==
         std::numeric_limits<std::int64_t>::min());
  auto authBytes = payload(9, 3, -12);
  std::vector<std::uint8_t> authMemory(4096, 0);
  put(authMemory, 0, authBind, 8);
  const auto authResult = arm64metal::apply_chained_fixups(
      authBytes, 0, 0, authMemory, {{0, 4096}});
  assert(authResult.authentication.at(0).key == 2);
  assert(authResult.authentication.at(0).modifier == 0);

  auto overflow = payload(9, 3, std::numeric_limits<std::int64_t>::max());
  std::vector<std::uint8_t> memory(4096, 0);
  put(memory, 0, bindArm64e | (std::uint64_t{1} << 32), 8);
  bool rejected = false;
  try {
    (void)arm64metal::apply_chained_fixups(overflow, 0, 0, memory, {{0, 4096}});
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  auto underflow = payload(9, 3, std::numeric_limits<std::int64_t>::min());
  put(memory, 0, bindArm64e | (std::uint64_t{0x40000} << 32), 8);
  rejected = false;
  try {
    (void)arm64metal::apply_chained_fixups(underflow, 0, 0, memory, {{0, 4096}});
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  if (argc == 2) verify_linker_fixture(argv[1]);
  else assert(argc == 1);
}
