#include "macho.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace arm64metal {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

bool supported_arm64_subtype(std::uint64_t subtype, bool allow_arm64e) {
  switch (subtype & 0x00ffffffU) {
    case 0:
    case 1: return true;
    case 2: return allow_arm64e;
    default: return false;
  }
}

std::string nul_terminated(const std::vector<std::uint8_t>& bytes, std::size_t start,
                           std::size_t end, const char* error) {
  if (start > end || end > bytes.size()) fail(error);
  auto nul = std::find(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                       bytes.begin() + static_cast<std::ptrdiff_t>(end), 0);
  if (nul == bytes.begin() + static_cast<std::ptrdiff_t>(end)) fail(error);
  return std::string(bytes.begin() + static_cast<std::ptrdiff_t>(start), nul);
}

std::uint64_t read_uleb(const std::vector<std::uint8_t>& bytes, std::size_t& cursor,
                        std::size_t end) {
  std::uint64_t result = 0;
  for (unsigned shift = 0; shift <= 63; shift += 7) {
    if (cursor >= end || end > bytes.size()) fail("Truncated Mach-O export ULEB");
    const std::uint8_t byte = bytes[cursor++];
    if (shift == 63 && byte > 1) fail("Overflowing Mach-O export ULEB");
    result |= static_cast<std::uint64_t>(byte & 127U) << shift;
    if ((byte & 128U) == 0) return result;
  }
  fail("Unterminated Mach-O export ULEB");
}

std::size_t checked_add(std::size_t a, std::size_t b, const char* message) {
  if (b > std::numeric_limits<std::size_t>::max() - a) fail(message);
  return a + b;
}

struct MachSection {
  std::uint64_t address;
  std::uint64_t size;
  std::uint32_t ordinal;
  std::string segment_name;
  std::string section_name;
};

struct NListTable {
  std::size_t offset;
  std::size_t count;
  std::size_t strings;
  std::size_t string_size;
};

struct DynamicSymbolTable {
  std::size_t local_start;
  std::size_t local_count;
  std::size_t external_start;
  std::size_t external_count;
  std::size_t undefined_start;
  std::size_t undefined_count;
};

struct MachSymbolMetadata {
  std::vector<MachSection> sections;
  std::vector<std::size_t> text_section_ordinals;
  std::uint64_t text_address = 0;
  std::uint64_t text_size = 0;
  std::size_t text_offset = 0;
  bool has_text = false;
  NListTable symbols{};
  bool has_symbols = false;
  DynamicSymbolTable dynamic_symbols{};
  bool has_dynamic_symbols = false;
};

MachSymbolMetadata parse_symbol_metadata(const std::vector<std::uint8_t>& bytes) {
  const auto word = [&bytes](std::size_t offset) {
    return static_cast<std::uint32_t>(read_integer(bytes, offset, 4));
  };
  const std::size_t command_count = word(16);
  const std::size_t command_bytes = word(20);
  if (command_bytes > bytes.size() || 32 > bytes.size() - command_bytes) {
    fail("Invalid load commands");
  }
  const std::size_t command_end = 32 + command_bytes;
  if (command_count > (command_end - 32) / 8) fail("Invalid load commands");
  MachSymbolMetadata metadata;
  std::size_t offset = 32;
  for (std::size_t command_index = 0; command_index < command_count; ++command_index) {
    if (offset > command_end || command_end - offset < 8) fail("Truncated load command");
    const auto command = word(offset);
    const std::size_t length = word(offset + 4);
    if (length < 8 || length > command_end - offset) fail("Invalid load command");
    if (command == 0x19) {
      if (length < 72) fail("Truncated segment");
      const auto segment_name = nul_terminated(bytes, offset + 8, offset + 24, "Invalid segment name");
      const std::size_t section_count = word(offset + 64);
      if (section_count > (length - 72) / 80) fail("Truncated section table");
      for (std::size_t index = 0; index < section_count; ++index) {
        const std::size_t start = offset + 72 + index * 80;
        const auto section_name = nul_terminated(bytes, start, start + 16, "Invalid section name");
        const auto owning_segment = nul_terminated(bytes, start + 16, start + 32, "Invalid segment name");
        const auto address = read_integer(bytes, start + 32, 8);
        const auto size = read_integer(bytes, start + 40, 8);
        if (metadata.sections.size() >= std::numeric_limits<std::uint8_t>::max()) {
          fail("Mach-O section ordinal exceeds n_sect range");
        }
        const auto ordinal = static_cast<std::uint32_t>(metadata.sections.size() + 1);
        metadata.sections.push_back({address, size, ordinal, owning_segment, section_name});
        if (section_name == "__text" && owning_segment == "__TEXT") {
          metadata.has_text = true;
          metadata.text_address = address;
          metadata.text_size = size;
          metadata.text_offset = word(start + 48);
          metadata.text_section_ordinals.push_back(ordinal);
        }
      }
    } else if (command == 2) {
      if (length < 24) fail("Truncated symbol command");
      metadata.symbols = {word(offset + 8), word(offset + 12), word(offset + 16), word(offset + 20)};
      metadata.has_symbols = true;
    } else if (command == 0xb) {
      if (length < 80) fail("Truncated dynamic symbol command");
      metadata.dynamic_symbols = {word(offset + 8), word(offset + 12), word(offset + 16),
                                  word(offset + 20), word(offset + 24), word(offset + 28)};
      metadata.has_dynamic_symbols = true;
    }
    offset += length;
  }
  if (metadata.has_symbols) {
    const auto& table = metadata.symbols;
    if (table.offset > bytes.size() || table.count > (bytes.size() - table.offset) / 16 ||
        table.strings > bytes.size() || table.string_size > bytes.size() - table.strings) {
      fail("Missing or invalid symbol tables");
    }
  }
  if (metadata.has_dynamic_symbols) {
    if (!metadata.has_symbols) fail("Dynamic symbol table has no symbol table");
    const auto count = metadata.symbols.count;
    const auto validate_range = [count](std::size_t start, std::size_t range_count) {
      if (start > count || range_count > count - start) fail("Invalid dynamic symbol range");
    };
    validate_range(metadata.dynamic_symbols.local_start, metadata.dynamic_symbols.local_count);
    validate_range(metadata.dynamic_symbols.external_start, metadata.dynamic_symbols.external_count);
    validate_range(metadata.dynamic_symbols.undefined_start, metadata.dynamic_symbols.undefined_count);
    const auto overlaps = [](std::size_t left_start, std::size_t left_count,
                             std::size_t right_start, std::size_t right_count) {
      return left_count != 0 && right_count != 0 && left_start < right_start + right_count &&
             right_start < left_start + left_count;
    };
    if (overlaps(metadata.dynamic_symbols.local_start, metadata.dynamic_symbols.local_count,
                 metadata.dynamic_symbols.external_start, metadata.dynamic_symbols.external_count) ||
        overlaps(metadata.dynamic_symbols.local_start, metadata.dynamic_symbols.local_count,
                 metadata.dynamic_symbols.undefined_start, metadata.dynamic_symbols.undefined_count) ||
        overlaps(metadata.dynamic_symbols.external_start, metadata.dynamic_symbols.external_count,
                 metadata.dynamic_symbols.undefined_start, metadata.dynamic_symbols.undefined_count)) {
      fail("Overlapping dynamic symbol ranges");
    }
  }
  return metadata;
}

}  // namespace

std::uint64_t read_integer(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                           std::size_t width, bool big_endian) {
  if (width > sizeof(std::uint64_t) || offset > bytes.size() || width > bytes.size() - offset) {
    fail("Truncated Mach-O");
  }
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < width; ++index) {
    const std::size_t shift_index = big_endian ? width - 1 - index : index;
    result |= static_cast<std::uint64_t>(bytes[offset + index]) << (shift_index * 8);
  }
  return result;
}

MachO::MachO(const std::filesystem::path& path, bool allow_arm64e) {
  std::ifstream input(path, std::ios::binary);
  if (!input) fail("Unable to read Mach-O file: " + path.string());
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0) fail("Unable to size Mach-O file: " + path.string());
  bytes_.resize(static_cast<std::size_t>(length));
  input.seekg(0, std::ios::beg);
  if (!bytes_.empty() && !input.read(reinterpret_cast<char*>(bytes_.data()), length)) {
    fail("Unable to read Mach-O file: " + path.string());
  }
  // Delegate validation and universal-slice selection to the buffer constructor.
  MachO parsed(std::move(bytes_), allow_arm64e);
  bytes_ = std::move(parsed.bytes_);
}

MachO::MachO(std::vector<std::uint8_t> bytes, bool allow_arm64e) : bytes_(std::move(bytes)) {
  const auto magic = read_integer(bytes_, 0, 4, true);
  if (magic == 0xcafebabeU || magic == 0xcafebabfU) {
    const bool wide = magic == 0xcafebabfU;
    const std::size_t record_size = wide ? 32 : 20;
    const auto count64 = read_integer(bytes_, 4, 4, true);
    if (count64 > (bytes_.size() - 8) / record_size) fail("Invalid universal header");
    const std::size_t count = static_cast<std::size_t>(count64);
    std::vector<std::uint8_t> slice;
    bool found = false;
    for (std::size_t index = 0; index < count; ++index) {
      const std::size_t start = 8 + index * record_size;
      if (read_integer(bytes_, start, 4, true) != 0x0100000cU) continue;
      const auto subtype = read_integer(bytes_, start + 4, 4, true);
      if (!supported_arm64_subtype(subtype, allow_arm64e)) continue;
      const bool arm64e = (subtype & 0x00ffffffU) == 2;
      if (arm64e && found) continue;
      const std::size_t width = wide ? 8 : 4;
      const auto offset = read_integer(bytes_, start + 8, width, true);
      const auto size = read_integer(bytes_, start + 8 + width, width, true);
      if (offset > bytes_.size() || size > bytes_.size() - static_cast<std::size_t>(offset)) {
        fail("Invalid ARM64 slice bounds");
      }
      slice.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                   bytes_.begin() + static_cast<std::ptrdiff_t>(offset + size));
      found = true;
      if (!arm64e) break;
    }
    if (!found) fail("No supported ARM64 slice");
    bytes_ = std::move(slice);
  }
  if (word(0) != 0xfeedfacfU || word(4) != 0x0100000cU ||
      !supported_arm64_subtype(word(8), allow_arm64e)) {
    fail("Expected a little-endian ARM64 Mach-O");
  }
}

std::uint32_t MachO::word(std::size_t offset) const {
  return static_cast<std::uint32_t>(read_integer(bytes_, offset, 4));
}

SymbolRegion MachO::symbol(const std::string& name) const {
  const auto metadata = parse_symbol_metadata(bytes_);
  if (!metadata.has_text || !metadata.has_symbols || metadata.text_offset > bytes_.size() ||
      metadata.text_size > bytes_.size() - metadata.text_offset ||
      metadata.text_size > std::numeric_limits<std::uint32_t>::max()) {
    fail("Missing or invalid __text and symbol tables");
  }

  const auto& table = metadata.symbols;
  const std::string symbol_name = "_" + name;
  bool found_entry = false;
  std::size_t entry = 0;
  std::vector<std::size_t> symbol_offsets;
  for (std::size_t index = 0; index < table.count; ++index) {
    const std::size_t start = table.offset + index * 16;
    const auto type = bytes_[start + 4];
    if ((type & 0xeeU) != 0x0eU ||
        std::find(metadata.text_section_ordinals.begin(), metadata.text_section_ordinals.end(),
                  bytes_[start + 5]) == metadata.text_section_ordinals.end()) continue;
    const auto address = read_integer(bytes_, start + 8, 8);
    if (address < metadata.text_address || address - metadata.text_address >= metadata.text_size) continue;
    const std::size_t relative = static_cast<std::size_t>(address - metadata.text_address);
    symbol_offsets.push_back(relative);
    const std::size_t name_offset = word(start);
    if (name_offset >= table.string_size || symbol_name.size() >= table.string_size - name_offset) continue;
    const std::size_t first = table.strings + name_offset;
    const std::size_t last = first + symbol_name.size();
    if (bytes_[last] == 0 && std::equal(symbol_name.begin(), symbol_name.end(),
                                       bytes_.begin() + static_cast<std::ptrdiff_t>(first))) {
      entry = relative;
      found_entry = true;
    }
  }
  if (!found_entry || entry % 4 != 0) fail("ARM64 text symbol not found: " + name);
  std::size_t limit = static_cast<std::size_t>(metadata.text_size);
  for (const auto candidate : symbol_offsets) {
    if (candidate > entry) limit = std::min(limit, candidate);
  }
  return {entry, limit, metadata.text_offset};
}

std::vector<MachSymbol> MachO::defined_section_symbols() const {
  const auto metadata = parse_symbol_metadata(bytes_);
  if (!metadata.has_symbols || !metadata.has_dynamic_symbols) return {};
  const auto& table = metadata.symbols;
  std::vector<MachSymbol> result;
  const auto append_range = [&](std::size_t first, std::size_t count, bool expect_external) {
    for (std::size_t index = first; index < first + count; ++index) {
      const std::size_t start = table.offset + index * 16;
      const std::uint32_t string_index = word(start);
      const std::uint8_t type = bytes_[start + 4];
      const std::uint8_t section_index = bytes_[start + 5];
      if ((type & 0xe0U) != 0 || (type & 0x0eU) != 0x0eU) continue;
      if (section_index == 0 || section_index > metadata.sections.size()) {
        fail("Invalid Mach-O section symbol ordinal");
      }
      if (string_index >= table.string_size) fail("Invalid Mach-O symbol string index");
      if (string_index == 0) continue;
      const std::size_t string_start = table.strings + string_index;
      const std::size_t string_end = table.strings + table.string_size;
      auto nul = std::find(bytes_.begin() + static_cast<std::ptrdiff_t>(string_start),
                           bytes_.begin() + static_cast<std::ptrdiff_t>(string_end), 0);
      if (nul == bytes_.begin() + static_cast<std::ptrdiff_t>(string_end)) {
        fail("Unterminated Mach-O symbol name");
      }
      std::string symbol_name(bytes_.begin() + static_cast<std::ptrdiff_t>(string_start), nul);
      if (symbol_name.empty()) continue;
      const auto& section = metadata.sections[section_index - 1];
      const auto address = read_integer(bytes_, start + 8, 8);
      result.push_back({std::move(symbol_name), address, section_index, expect_external,
                        section.segment_name, section.section_name});
    }
  };
  append_range(metadata.dynamic_symbols.external_start, metadata.dynamic_symbols.external_count, true);
  append_range(metadata.dynamic_symbols.local_start, metadata.dynamic_symbols.local_count, false);
  std::stable_sort(result.begin(), result.end(), [](const MachSymbol& left, const MachSymbol& right) {
    if (left.address != right.address) return left.address < right.address;
    return left.external && !right.external;
  });
  return result;
}

std::map<std::string, MachExport> mach_exports(const std::vector<std::uint8_t>& trie,
                                               std::size_t memory_size) {
  std::map<std::string, MachExport> exports;
  std::vector<std::pair<std::size_t, std::string>> pending{{0, {}}};
  std::set<std::size_t> visited;
  while (!pending.empty()) {
    auto node = std::move(pending.back());
    pending.pop_back();
    const std::size_t offset = node.first;
    if (offset >= trie.size() || !visited.insert(offset).second || visited.size() > 100000) {
      fail("Invalid or excessive Mach-O export nodes");
    }
    std::size_t cursor = offset;
    const auto terminal = read_uleb(trie, cursor, trie.size());
    if (cursor > trie.size() || terminal >= trie.size() - cursor) fail("Invalid Mach-O export terminal");
    const std::size_t terminal_end = cursor + static_cast<std::size_t>(terminal);
    if (terminal != 0) {
      const auto flags = read_uleb(trie, cursor, terminal_end);
      if ((flags & 8U) != 0) {
        const auto ordinal = read_uleb(trie, cursor, terminal_end);
        if (node.second.empty() || ordinal == 0 || cursor > terminal_end) fail("Invalid Mach-O re-export");
        auto nul = std::find(trie.begin() + static_cast<std::ptrdiff_t>(cursor),
                             trie.begin() + static_cast<std::ptrdiff_t>(terminal_end), 0);
        if (nul == trie.begin() + static_cast<std::ptrdiff_t>(terminal_end) ||
            nul + 1 != trie.begin() + static_cast<std::ptrdiff_t>(terminal_end)) {
          fail("Invalid Mach-O re-export");
        }
        std::string imported;
        if (nul == trie.begin() + static_cast<std::ptrdiff_t>(cursor)) imported = node.second;
        else imported.assign(trie.begin() + static_cast<std::ptrdiff_t>(cursor), nul);
        exports[node.second] = {ExportKind::reexport, 0, ordinal, std::move(imported)};
      } else if ((flags & 0x1bU) == 0) {
        const auto address = read_uleb(trie, cursor, terminal_end);
        if (address >= memory_size || node.second.empty()) fail("Mach-O export outside guest image");
        exports[node.second] = {ExportKind::address, address, 0, {}};
      }
    }
    cursor = terminal_end;
    if (cursor >= trie.size()) fail("Invalid Mach-O export edge");
    const std::size_t children = trie[cursor++];
    for (std::size_t index = 0; index < children; ++index) {
      if (cursor >= trie.size()) fail("Invalid Mach-O export edge");
      const auto nul = std::find(trie.begin() + static_cast<std::ptrdiff_t>(cursor), trie.end(), 0);
      if (nul == trie.end() || nul == trie.begin() + static_cast<std::ptrdiff_t>(cursor)) {
        fail("Invalid Mach-O export edge");
      }
      const std::size_t edge_end = static_cast<std::size_t>(nul - trie.begin());
      if (edge_end - cursor > 4096 || node.second.size() > 4096 - (edge_end - cursor)) {
        fail("Invalid Mach-O export edge");
      }
      std::string name = node.second;
      name.append(trie.begin() + static_cast<std::ptrdiff_t>(cursor), nul);
      cursor = edge_end + 1;
      const auto child = read_uleb(trie, cursor, trie.size());
      if (child >= trie.size()) fail("Invalid Mach-O export child");
      pending.emplace_back(static_cast<std::size_t>(child), std::move(name));
    }
  }
  return exports;
}

ChainedFixups apply_chained_fixups(
    const std::vector<std::uint8_t>& payload, std::uint64_t image_base,
    std::size_t placement, std::vector<std::uint8_t>& memory,
    const std::vector<std::pair<std::size_t, std::size_t>>& mapped_ranges) {
  auto read = [&payload](std::size_t offset, std::size_t size) {
    return read_integer(payload, offset, size);
  };
  if (read(0, 4) != 0 || read(24, 4) != 0) fail("Unsupported dyld chained fixup header");
  const std::size_t starts = static_cast<std::size_t>(read(4, 4));
  const std::size_t import_offset = static_cast<std::size_t>(read(8, 4));
  const std::size_t strings = static_cast<std::size_t>(read(12, 4));
  const std::size_t import_count = static_cast<std::size_t>(read(16, 4));
  const auto format = read(20, 4);
  const std::size_t record_size = format == 1 ? 4 : format == 2 ? 8 : format == 3 ? 16 : 0;
  if (record_size == 0 || strings > payload.size() || import_offset > payload.size() ||
      import_count > (payload.size() - import_offset) / record_size) {
    fail("Invalid dyld chained imports");
  }
  std::vector<ChainedImport> imports;
  imports.reserve(import_count);
  for (std::size_t index = 0; index < import_count; ++index) {
    const std::size_t offset = import_offset + index * record_size;
    const auto record = read(offset, format == 3 ? 8 : 4);
    const std::size_t name_offset = static_cast<std::size_t>(record >> (format == 3 ? 32 : 9));
    if (name_offset >= payload.size() - strings) fail("Invalid dyld import name offset");
    const std::size_t name_start = strings + name_offset;
    auto nul = std::find(payload.begin() + static_cast<std::ptrdiff_t>(name_start), payload.end(), 0);
    if (nul == payload.end() || nul == payload.begin() + static_cast<std::ptrdiff_t>(name_start)) {
      fail("Unterminated dyld import name");
    }
    const auto raw_ordinal = static_cast<std::int64_t>(record & (format == 3 ? 0xffffU : 0xffU));
    const std::int64_t sign_threshold = format == 3 ? 0xfff1 : 0xf1;
    const std::int64_t modulus = format == 3 ? 65536 : 256;
    const std::int64_t ordinal = raw_ordinal >= sign_threshold ? raw_ordinal - modulus : raw_ordinal;
    const std::int64_t addend = format == 3 ? static_cast<std::int64_t>(read(offset + 8, 8))
        : format == 2 ? static_cast<std::int32_t>(read(offset + 4, 4)) : 0;
    imports.push_back({std::string(payload.begin() + static_cast<std::ptrdiff_t>(name_start), nul), ordinal,
                       (record & (format == 3 ? 0x10000U : 0x100U)) != 0, addend});
  }

  const std::size_t segment_count = static_cast<std::size_t>(read(starts, 4));
  if (starts > payload.size() || payload.size() - starts < 4 ||
      segment_count > (payload.size() - starts - 4) / 4) fail("Invalid dyld segment starts");
  ChainedFixups result;
  std::set<std::size_t> visited;
  for (std::size_t index = 0; index < segment_count; ++index) {
    const std::size_t delta = static_cast<std::size_t>(read(starts + 4 + index * 4, 4));
    if (delta == 0) continue;
    // Each seg_info_offset is relative to dyld_chained_starts_in_image itself.
    const std::size_t segment = checked_add(starts, delta, "Invalid dyld page starts");
    const std::size_t size = static_cast<std::size_t>(read(segment, 4));
    const std::size_t page_size = static_cast<std::size_t>(read(segment + 4, 2));
    const auto pointer_format = read(segment + 6, 2);
    const auto segment_offset = read(segment + 8, 8);
    const std::size_t pages = static_cast<std::size_t>(read(segment + 20, 2));
    const bool arm64e = pointer_format == 1 || pointer_format == 9 || pointer_format == 12;
    if (!arm64e && pointer_format != 2 && pointer_format != 6) {
      fail("Unsupported dyld pointer format " + std::to_string(pointer_format) +
           " (pages=" + std::to_string(pages) + ", offset=" + std::to_string(segment_offset) + ")");
    }
    if (size < 22 || segment > payload.size() || size > payload.size() - segment ||
        pages > (size - 22) / 2 || (page_size != 4096 && page_size != 16384) ||
        segment_offset > memory.size()) fail("Invalid dyld page starts");
    const std::size_t stride = arm64e ? 8 : 4;
    for (std::size_t page = 0; page < pages; ++page) {
      const std::size_t first = static_cast<std::size_t>(read(segment + 22 + page * 2, 2));
      if (first == 0xffff) continue;
      if ((first & 0x8000U) != 0) fail("Multiple starts per dyld fixup page unsupported");
      if (page > (std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(segment_offset)) / page_size) {
        fail("Invalid dyld fixup address");
      }
      const std::size_t page_address = static_cast<std::size_t>(segment_offset) + page * page_size;
      if (page_address > std::numeric_limits<std::size_t>::max() - page_size) fail("Invalid dyld fixup address");
      const std::size_t page_end = page_address + page_size;
      std::size_t address = checked_add(page_address, first, "Invalid dyld fixup address");
      while (true) {
        if (address % stride != 0 || address > page_end || page_end - address < 8 ||
            address > memory.size() || memory.size() - address < 8 || !visited.insert(address).second) {
          fail("Invalid dyld fixup address");
        }
        bool mapped = false;
        for (const auto& range : mapped_ranges) {
          if (address >= range.first && address <= range.second && range.second - address >= 8) {
            mapped = true;
            break;
          }
        }
        if (!mapped) fail("Invalid dyld fixup address");
        const auto raw = read_integer(memory, address, 8);
        const bool authenticated = arm64e && (raw >> 63) != 0;
        if (authenticated) {
          const auto diversity = (raw >> 32) & 0xffffU;
          const bool address_diversity = (raw & (std::uint64_t{1} << 48)) != 0;
          const auto modifier = address_diversity
              ? static_cast<std::uint64_t>(placement + address) | (diversity << 48) : diversity;
          result.authentication[address] = {(raw >> 49) & 3U, modifier};
        }
        const bool bind = (raw & (std::uint64_t{1} << (arm64e ? 62 : 63))) != 0;
        const std::size_t next = static_cast<std::size_t>((raw >> 51) & (arm64e ? 0x7ffU : 0xfffU));
        std::uint64_t replacement = 0;
        if (bind) {
          const std::size_t ordinal = static_cast<std::size_t>(raw &
              (arm64e ? (pointer_format == 12 ? 0xffffffU : 0xffffU) : 0xfffffU));
          if (ordinal >= imports.size()) fail("Invalid dyld bind ordinal");
          const auto& imported = imports[ordinal];
          const std::int64_t embedded = authenticated ? 0 : arm64e
              ? static_cast<std::int64_t>((raw >> 32) & 0x7ffffU)
              : static_cast<std::int64_t>((raw >> 20) & 0xffU);
          if (embedded != 0 || imported.addend != 0) fail("Nonzero dyld bind addends unsupported");
          result.bindings[address] = imported;
          replacement = imported.weak ? 0 : std::numeric_limits<std::uint64_t>::max();
        } else {
          const unsigned low_bits = authenticated ? 32 : arm64e ? 43 : 36;
          const std::uint64_t target = raw & ((std::uint64_t{1} << low_bits) - 1);
          const bool absolute = !authenticated && (pointer_format == 1 || pointer_format == 2);
          if (absolute && target < image_base) fail("Invalid dyld rebase target");
          replacement = absolute ? target - image_base : target;
          if (replacement >= memory.size()) fail("Dyld rebase target outside guest image");
          if (placement > std::numeric_limits<std::uint64_t>::max() - replacement) fail("Dyld rebase target outside guest image");
          replacement += placement;
          if (!authenticated) replacement |= ((raw >> low_bits) & 0xffU) << 56;
        }
        for (std::size_t byte = 0; byte < 8; ++byte) memory[address + byte] = static_cast<std::uint8_t>(replacement >> (byte * 8));
        if (next == 0) break;
        if (next > std::numeric_limits<std::size_t>::max() / stride || next * stride < 8 ||
            address > std::numeric_limits<std::size_t>::max() - next * stride) fail("Overlapping dyld fixups");
        address += next * stride;
      }
    }
  }
  return result;
}

std::vector<LegacyRebase> decode_legacy_rebase_opcodes(
    const std::vector<std::uint8_t>& payload,
    std::uint64_t image_base,
    std::uint64_t placement,
    const std::vector<LegacyRebaseSegment>& segments,
    const std::vector<std::uint8_t>& memory,
    const std::vector<std::pair<std::size_t, std::size_t>>& mapped_ranges) {
  constexpr std::uint8_t kOpcodeMask = 0xf0;
  constexpr std::uint8_t kImmediateMask = 0x0f;
  constexpr std::uint8_t kRebaseDone = 0x00;
  constexpr std::uint8_t kSetType = 0x10;
  constexpr std::uint8_t kSetSegmentAndOffset = 0x20;
  constexpr std::uint8_t kAddAddressULEB = 0x30;
  constexpr std::uint8_t kAddAddressImmediateScaled = 0x40;
  constexpr std::uint8_t kDoRebaseImmediateTimes = 0x50;
  constexpr std::uint8_t kDoRebaseULEBTimes = 0x60;
  constexpr std::uint8_t kDoRebaseAddAddressULEB = 0x70;
  constexpr std::uint8_t kDoRebaseULEBTimesSkippingULEB = 0x80;
  constexpr std::uint8_t kRebaseTypePointer = 1;
  constexpr std::uint64_t kPointerSize = 8;

  for (const auto& segment : segments) {
    if (segment.vm_size > std::numeric_limits<std::uint64_t>::max() - segment.vm_address) {
      fail("Invalid legacy rebase segment range");
    }
  }
  for (const auto& range : mapped_ranges) {
    if (range.first > range.second || range.second > memory.size()) {
      fail("Invalid legacy rebase mapped range");
    }
  }

  auto read_stream_uleb = [&payload](std::size_t& cursor) {
    std::uint64_t result = 0;
    for (unsigned shift = 0; shift <= 63; shift += 7) {
      if (cursor >= payload.size()) fail("Truncated legacy rebase ULEB");
      const auto byte = payload[cursor++];
      if (shift == 63 && byte > 1) fail("Overflowing legacy rebase ULEB");
      result |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
      if ((byte & 0x80U) == 0) return result;
    }
    fail("Unterminated legacy rebase ULEB");
  };

  std::vector<LegacyRebase> planned;
  std::set<std::size_t> visited;
  std::optional<std::size_t> segment_index;
  std::uint64_t segment_offset = 0;
  std::uint8_t rebase_type = kRebaseTypePointer;
  bool finished = false;

  const auto selected_segment = [&]() -> const LegacyRebaseSegment& {
    if (!segment_index || *segment_index >= segments.size()) {
      fail("Legacy rebase opcode has no valid segment");
    }
    return segments[*segment_index];
  };
  const auto add_segment_offset = [&segment_offset](std::uint64_t amount) {
    if (amount > std::numeric_limits<std::uint64_t>::max() - segment_offset) {
      fail("Legacy rebase offset overflow");
    }
    segment_offset += amount;
  };
  const auto ensure_repeat_count = [&](std::uint64_t count) {
    const auto& segment = selected_segment();
    if (segment_offset > segment.vm_size || count > (segment.vm_size - segment_offset) / kPointerSize) {
      fail("Legacy rebase repeat exceeds segment bounds");
    }
  };
  const auto emit_rebase = [&]() {
    if (rebase_type != kRebaseTypePointer) {
      fail("Unsupported legacy rebase type " + std::to_string(rebase_type));
    }
    const auto& segment = selected_segment();
    if (segment_offset > segment.vm_size || kPointerSize > segment.vm_size - segment_offset ||
        segment_offset > std::numeric_limits<std::uint64_t>::max() - segment.vm_address) {
      fail("Legacy rebase pointer exceeds segment bounds");
    }
    const std::uint64_t vm_slot = segment.vm_address + segment_offset;
    if (vm_slot < image_base || vm_slot - image_base > std::numeric_limits<std::size_t>::max()) {
      fail("Legacy rebase slot is outside image backing");
    }
    const auto location = static_cast<std::size_t>(vm_slot - image_base);
    if (location > memory.size() || kPointerSize > memory.size() - location) {
      fail("Legacy rebase pointer exceeds image backing");
    }
    bool mapped = false;
    for (const auto& range : mapped_ranges) {
      if (location >= range.first && location <= range.second &&
          kPointerSize <= range.second - location) {
        mapped = true;
        break;
      }
    }
    if (!mapped || !visited.insert(location).second) {
      fail("Invalid or duplicate legacy rebase slot");
    }
    const auto unslid_target = read_integer(memory, location, kPointerSize);
    if (unslid_target < image_base || unslid_target - image_base >= memory.size()) {
      std::ostringstream message;
      message << "Legacy rebase target 0x" << std::hex << unslid_target
              << " at slot 0x" << location << " is outside image backing (base 0x"
              << image_base << ", size 0x" << memory.size() << ')';
      fail(message.str());
    }
    const auto target_offset = unslid_target - image_base;
    if (target_offset > std::numeric_limits<std::uint64_t>::max() - placement) {
      fail("Legacy rebase target overflows guest address");
    }
    planned.push_back({location, placement + target_offset});
  };

  std::size_t cursor = 0;
  while (cursor < payload.size()) {
    const auto instruction = payload[cursor++];
    const auto opcode = instruction & kOpcodeMask;
    const auto immediate = instruction & kImmediateMask;
    switch (opcode) {
      case kRebaseDone:
        if (immediate != 0) fail("Invalid legacy rebase DONE opcode");
        finished = true;
        cursor = payload.size();
        break;
      case kSetType:
        if (immediate != kRebaseTypePointer) {
          fail("Unsupported legacy rebase type " + std::to_string(immediate));
        }
        rebase_type = immediate;
        break;
      case kSetSegmentAndOffset:
        if (static_cast<std::size_t>(immediate) >= segments.size()) {
          fail("Invalid legacy rebase segment ordinal");
        }
        segment_index = static_cast<std::size_t>(immediate);
        segment_offset = read_stream_uleb(cursor);
        if (segment_offset > selected_segment().vm_size) {
          fail("Legacy rebase starting offset exceeds segment bounds");
        }
        break;
      case kAddAddressULEB:
        (void)selected_segment();
        add_segment_offset(read_stream_uleb(cursor));
        break;
      case kAddAddressImmediateScaled:
        (void)selected_segment();
        add_segment_offset(static_cast<std::uint64_t>(immediate) * kPointerSize);
        break;
      case kDoRebaseImmediateTimes:
        ensure_repeat_count(immediate);
        for (std::uint8_t index = 0; index < immediate; ++index) {
          emit_rebase();
          add_segment_offset(kPointerSize);
        }
        break;
      case kDoRebaseULEBTimes: {
        const auto count = read_stream_uleb(cursor);
        ensure_repeat_count(count);
        for (std::uint64_t index = 0; index < count; ++index) {
          emit_rebase();
          add_segment_offset(kPointerSize);
        }
        break;
      }
      case kDoRebaseAddAddressULEB: {
        const auto skip = read_stream_uleb(cursor);
        ensure_repeat_count(1);
        emit_rebase();
        if (skip > std::numeric_limits<std::uint64_t>::max() - kPointerSize) {
          fail("Legacy rebase skip overflow");
        }
        add_segment_offset(kPointerSize + skip);
        break;
      }
      case kDoRebaseULEBTimesSkippingULEB: {
        const auto count = read_stream_uleb(cursor);
        const auto skip = read_stream_uleb(cursor);
        ensure_repeat_count(count);
        for (std::uint64_t index = 0; index < count; ++index) {
          emit_rebase();
          if (skip > std::numeric_limits<std::uint64_t>::max() - kPointerSize) {
            fail("Legacy rebase skip overflow");
          }
          add_segment_offset(kPointerSize + skip);
        }
        break;
      }
      default:
        fail("Unsupported legacy rebase opcode 0x" + [&]() {
          std::ostringstream formatted;
          formatted << std::hex << static_cast<unsigned>(opcode);
          return formatted.str();
        }());
    }
  }
  if (!finished) fail("Legacy rebase stream has no DONE opcode");
  return planned;
}

std::vector<LegacyBind> decode_legacy_bind_opcodes(
    const std::vector<std::uint8_t>& payload,
    std::uint64_t image_base,
    const std::vector<LegacyRebaseSegment>& segments,
    const std::vector<std::uint8_t>& memory,
    const std::vector<std::pair<std::size_t, std::size_t>>& mapped_ranges,
    bool lazy_stream) {
  constexpr std::uint8_t kOpcodeMask = 0xf0;
  constexpr std::uint8_t kImmediateMask = 0x0f;
  constexpr std::uint8_t kDone = 0x00;
  constexpr std::uint8_t kSetOrdinalImmediate = 0x10;
  constexpr std::uint8_t kSetOrdinalULEB = 0x20;
  constexpr std::uint8_t kSetSpecialOrdinalImmediate = 0x30;
  constexpr std::uint8_t kSetSymbolTrailingFlagsImmediate = 0x40;
  constexpr std::uint8_t kSetTypeImmediate = 0x50;
  constexpr std::uint8_t kSetAddendSLEB = 0x60;
  constexpr std::uint8_t kSetSegmentAndOffsetULEB = 0x70;
  constexpr std::uint8_t kAddAddressULEB = 0x80;
  constexpr std::uint8_t kDoBind = 0x90;
  constexpr std::uint8_t kDoBindAddAddressULEB = 0xa0;
  constexpr std::uint8_t kDoBindAddAddressImmediateScaled = 0xb0;
  constexpr std::uint8_t kDoBindULEBTimesSkippingULEB = 0xc0;
  constexpr std::uint8_t kBindTypePointer = 1;
  constexpr std::uint64_t kPointerSize = 8;
  constexpr std::uint8_t kWeakImportFlag = 0x1;
  constexpr std::uint8_t kNonWeakDefinitionFlag = 0x8;

  for (const auto& segment : segments) {
    if (segment.vm_size > std::numeric_limits<std::uint64_t>::max() - segment.vm_address) {
      fail("Invalid legacy bind segment range");
    }
  }
  for (const auto& range : mapped_ranges) {
    if (range.first > range.second || range.second > memory.size()) {
      fail("Invalid legacy bind mapped range");
    }
  }

  std::vector<LegacyBind> planned;
  std::set<std::size_t> visited;
  std::optional<std::size_t> segment_index;
  std::uint64_t segment_offset = 0;
  std::int64_t ordinal = 0;
  bool ordinal_set = false;
  std::string symbol;
  std::uint8_t symbol_flags = 0;
  std::uint8_t bind_type = kBindTypePointer;
  std::int64_t addend = 0;
  std::size_t lazy_record = 0;
  std::size_t cursor = 0;
  bool lazy_last_was_done = false;

  const auto reset_record = [&]() {
    segment_index.reset();
    segment_offset = 0;
    ordinal = 0;
    ordinal_set = false;
    symbol.clear();
    symbol_flags = 0;
    bind_type = kBindTypePointer;
    addend = 0;
  };
  const auto selected_segment = [&]() -> const LegacyRebaseSegment& {
    if (!segment_index || *segment_index >= segments.size()) {
      fail("Legacy bind opcode has no valid segment");
    }
    return segments[*segment_index];
  };
  const auto add_segment_offset = [&segment_offset](std::uint64_t amount) {
    // dyld's ULEB cursor arithmetic is modulo 64 bits; some streams encode a
    // small backwards adjustment as a large unsigned ULEB. Slot bounds remain
    // strict in emit_bind after the cursor has been adjusted.
    segment_offset += amount;
  };
  const auto emit_bind = [&]() {
    if (bind_type != kBindTypePointer) {
      fail("Unsupported legacy bind type " + std::to_string(bind_type));
    }
    if (symbol.empty()) fail("Legacy bind has no symbol name");
    if (!ordinal_set) fail("Legacy bind has no dylib ordinal");
    const auto& segment = selected_segment();
    if (segment_offset > segment.vm_size || kPointerSize > segment.vm_size - segment_offset ||
        segment_offset > std::numeric_limits<std::uint64_t>::max() - segment.vm_address) {
      fail("Legacy bind pointer exceeds segment bounds");
    }
    const auto vm_slot = segment.vm_address + segment_offset;
    if (vm_slot < image_base || vm_slot - image_base > std::numeric_limits<std::size_t>::max()) {
      fail("Legacy bind slot is outside image backing");
    }
    const auto location = static_cast<std::size_t>(vm_slot - image_base);
    if (location > memory.size() || kPointerSize > memory.size() - location) {
      fail("Legacy bind pointer exceeds image backing");
    }
    bool mapped = false;
    for (const auto& range : mapped_ranges) {
      if (location >= range.first && location <= range.second &&
          kPointerSize <= range.second - location) {
        mapped = true;
        break;
      }
    }
    if (!mapped || !visited.insert(location).second) fail("Invalid or duplicate legacy bind slot");
    planned.push_back({location,
                       ChainedImport{symbol, ordinal, (symbol_flags & kWeakImportFlag) != 0, addend},
                       (symbol_flags & kNonWeakDefinitionFlag) != 0, lazy_stream, lazy_record});
  };
  const auto read_sleb = [&payload](std::size_t& cursor) {
    std::uint64_t result = 0;
    unsigned shift = 0;
    std::uint8_t byte = 0;
    do {
      if (cursor >= payload.size()) fail("Truncated legacy bind SLEB");
      byte = payload[cursor++];
      if (shift == 63 && byte != 0x00 && byte != 0x7f) {
        fail("Overflowing legacy bind SLEB");
      }
      result |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
      shift += 7;
      if (shift > 63 && (byte & 0x80U) != 0) fail("Overflowing legacy bind SLEB");
    } while ((byte & 0x80U) != 0);
    if (shift < 64 && (byte & 0x40U) != 0) result |= (~std::uint64_t{0}) << shift;
    return static_cast<std::int64_t>(result);
  };

  while (cursor < payload.size()) {
    const auto instruction = payload[cursor++];
    const auto opcode = instruction & kOpcodeMask;
    const auto immediate = instruction & kImmediateMask;
    if (lazy_stream) lazy_last_was_done = opcode == kDone;
    switch (opcode) {
      case kDone:
        if (immediate != 0) fail("Invalid legacy bind DONE opcode");
        if (!lazy_stream) {
          cursor = payload.size();
        } else {
          ++lazy_record;
          reset_record();
        }
        break;
      case kSetOrdinalImmediate:
        ordinal = immediate;
        ordinal_set = true;
        break;
      case kSetOrdinalULEB: {
        const auto value = read_uleb(payload, cursor, payload.size());
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          fail("Legacy bind ordinal exceeds signed range");
        }
        ordinal = static_cast<std::int64_t>(value);
        ordinal_set = true;
        break;
      }
      case kSetSpecialOrdinalImmediate:
        ordinal = immediate == 0 ? 0 : static_cast<std::int8_t>(immediate | 0xf0U);
        ordinal_set = true;
        break;
      case kSetSymbolTrailingFlagsImmediate: {
        if ((immediate & ~(kWeakImportFlag | kNonWeakDefinitionFlag)) != 0) {
          fail("Unsupported legacy bind symbol flags");
        }
        symbol_flags = immediate;
        symbol = nul_terminated(payload, cursor, payload.size(), "Unterminated legacy bind symbol");
        cursor += symbol.size() + 1;
        break;
      }
      case kSetTypeImmediate:
        if (immediate != kBindTypePointer) {
          fail("Unsupported legacy bind type " + std::to_string(immediate));
        }
        bind_type = immediate;
        break;
      case kSetAddendSLEB:
        addend = read_sleb(cursor);
        break;
      case kSetSegmentAndOffsetULEB:
        if (static_cast<std::size_t>(immediate) >= segments.size()) {
          fail("Invalid legacy bind segment ordinal");
        }
        segment_index = static_cast<std::size_t>(immediate);
        segment_offset = read_uleb(payload, cursor, payload.size());
        if (segment_offset > selected_segment().vm_size) {
          fail("Legacy bind starting offset exceeds segment bounds");
        }
        break;
      case kAddAddressULEB:
        (void)selected_segment();
        add_segment_offset(read_uleb(payload, cursor, payload.size()));
        break;
      case kDoBind:
        emit_bind();
        add_segment_offset(kPointerSize);
        break;
      case kDoBindAddAddressULEB: {
        emit_bind();
        const auto skip = read_uleb(payload, cursor, payload.size());
        add_segment_offset(kPointerSize + skip);
        break;
      }
      case kDoBindAddAddressImmediateScaled:
        emit_bind();
        add_segment_offset(kPointerSize + static_cast<std::uint64_t>(immediate) * kPointerSize);
        break;
      case kDoBindULEBTimesSkippingULEB: {
        const auto count = read_uleb(payload, cursor, payload.size());
        const auto skip = read_uleb(payload, cursor, payload.size());
        const auto& segment = selected_segment();
        if (count > segment.vm_size / kPointerSize || count > memory.size() / kPointerSize) {
          fail("Legacy bind repeat count exceeds backing bounds");
        }
        const auto stride = kPointerSize + skip;
        for (std::uint64_t index = 0; index < count; ++index) {
          emit_bind();
          add_segment_offset(stride);
        }
        break;
      }
      default:
        fail("Unsupported legacy bind opcode 0x" + [&]() {
          std::ostringstream formatted;
          formatted << std::hex << static_cast<unsigned>(opcode);
          return formatted.str();
        }());
    }
  }
  if (lazy_stream && (payload.empty() || lazy_record == 0 || !lazy_last_was_done)) {
    fail("Legacy lazy bind stream has no DONE-delimited records");
  }
  return planned;
}

std::vector<LegacyWeakBind> decode_legacy_weak_bind_opcodes(
    const std::vector<std::uint8_t>& payload,
    std::uint64_t image_base,
    const std::vector<LegacyRebaseSegment>& segments,
    const std::vector<std::uint8_t>& memory,
    const std::vector<std::pair<std::size_t, std::size_t>>& mapped_ranges) {
  constexpr std::uint8_t kOpcodeMask = 0xf0;
  constexpr std::uint8_t kImmediateMask = 0x0f;
  constexpr std::uint8_t kDone = 0x00;
  constexpr std::uint8_t kSetOrdinalImmediate = 0x10;
  constexpr std::uint8_t kSetOrdinalULEB = 0x20;
  constexpr std::uint8_t kSetSpecialOrdinalImmediate = 0x30;
  constexpr std::uint8_t kSetSymbolTrailingFlagsImmediate = 0x40;
  constexpr std::uint8_t kSetTypeImmediate = 0x50;
  constexpr std::uint8_t kSetAddendSLEB = 0x60;
  constexpr std::uint8_t kSetSegmentAndOffsetULEB = 0x70;
  constexpr std::uint8_t kAddAddressULEB = 0x80;
  constexpr std::uint8_t kDoBind = 0x90;
  constexpr std::uint8_t kDoBindAddAddressULEB = 0xa0;
  constexpr std::uint8_t kDoBindAddAddressImmediateScaled = 0xb0;
  constexpr std::uint8_t kDoBindULEBTimesSkippingULEB = 0xc0;
  constexpr std::uint8_t kBindTypePointer = 1;
  constexpr std::uint64_t kPointerSize = 8;
  constexpr std::uint8_t kWeakImportFlag = 0x1;
  constexpr std::uint8_t kNonWeakDefinitionFlag = 0x8;

  for (const auto& segment : segments) {
    if (segment.vm_size > std::numeric_limits<std::uint64_t>::max() - segment.vm_address) {
      fail("Invalid legacy weak-bind segment range");
    }
  }
  for (const auto& range : mapped_ranges) {
    if (range.first > range.second || range.second > memory.size()) {
      fail("Invalid legacy weak-bind mapped range");
    }
  }

  std::vector<LegacyWeakBind> planned;
  std::set<std::size_t> visited;
  std::optional<std::size_t> segment_index;
  std::uint64_t segment_offset = 0;
  std::int64_t ordinal = -3;
  bool ordinal_set = true;
  std::string symbol;
  std::uint8_t symbol_flags = 0;
  std::uint8_t bind_type = 0;
  std::int64_t addend = 0;
  bool finished = false;

  const auto selected_segment = [&]() -> const LegacyRebaseSegment& {
    if (!segment_index || *segment_index >= segments.size()) {
      fail("Legacy weak bind opcode has no valid segment");
    }
    return segments[*segment_index];
  };
  const auto add_segment_offset = [&segment_offset](std::uint64_t amount) {
    // Match dyld's unsigned cursor arithmetic; emitted slots are strictly checked.
    segment_offset += amount;
  };
  const auto emit_bind = [&]() {
    if (bind_type != kBindTypePointer) {
      fail("Unsupported legacy weak-bind type " + std::to_string(bind_type));
    }
    if (symbol.empty()) fail("Legacy weak bind has no symbol name");
    if (!ordinal_set) fail("Legacy weak bind has no dylib ordinal");
    const auto& segment = selected_segment();
    if (segment_offset > segment.vm_size || kPointerSize > segment.vm_size - segment_offset ||
        segment_offset > std::numeric_limits<std::uint64_t>::max() - segment.vm_address) {
      fail("Legacy weak-bind pointer exceeds segment bounds");
    }
    const auto vm_slot = segment.vm_address + segment_offset;
    if (vm_slot < image_base || vm_slot - image_base > std::numeric_limits<std::size_t>::max()) {
      fail("Legacy weak-bind slot is outside image backing");
    }
    const auto location = static_cast<std::size_t>(vm_slot - image_base);
    if (location > memory.size() || kPointerSize > memory.size() - location) {
      fail("Legacy weak-bind pointer exceeds image backing");
    }
    bool mapped = false;
    for (const auto& range : mapped_ranges) {
      if (location >= range.first && location <= range.second &&
          kPointerSize <= range.second - location) {
        mapped = true;
        break;
      }
    }
    if (!mapped || !visited.insert(location).second) {
      fail("Invalid or duplicate legacy weak-bind slot");
    }
    planned.push_back({location,
                       ChainedImport{symbol, ordinal, (symbol_flags & kWeakImportFlag) != 0, addend},
                       (symbol_flags & kNonWeakDefinitionFlag) != 0});
  };
  const auto read_sleb = [&payload](std::size_t& cursor) {
    std::uint64_t result = 0;
    unsigned shift = 0;
    std::uint8_t byte = 0;
    do {
      if (cursor >= payload.size()) fail("Truncated legacy weak-bind SLEB");
      byte = payload[cursor++];
      if (shift == 63 && byte != 0x00 && byte != 0x7f) {
        fail("Overflowing legacy weak-bind SLEB");
      }
      result |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
      shift += 7;
      if (shift > 63 && (byte & 0x80U) != 0) fail("Overflowing legacy weak-bind SLEB");
    } while ((byte & 0x80U) != 0);
    if (shift < 64 && (byte & 0x40U) != 0) result |= (~std::uint64_t{0}) << shift;
    return static_cast<std::int64_t>(result);
  };

  std::size_t cursor = 0;
  while (cursor < payload.size()) {
    const auto instruction = payload[cursor++];
    const auto opcode = instruction & kOpcodeMask;
    const auto immediate = instruction & kImmediateMask;
    switch (opcode) {
      case kDone:
        if (immediate != 0) fail("Invalid legacy weak-bind DONE opcode");
        finished = true;
        cursor = payload.size();
        break;
      case kSetOrdinalImmediate:
        ordinal = immediate;
        ordinal_set = true;
        break;
      case kSetOrdinalULEB: {
        const auto value = read_uleb(payload, cursor, payload.size());
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          fail("Legacy weak-bind ordinal exceeds signed range");
        }
        ordinal = static_cast<std::int64_t>(value);
        ordinal_set = true;
        break;
      }
      case kSetSpecialOrdinalImmediate:
        ordinal = immediate == 0 ? 0 : static_cast<std::int8_t>(immediate | 0xf0U);
        ordinal_set = true;
        break;
      case kSetSymbolTrailingFlagsImmediate: {
        if ((immediate & ~(kWeakImportFlag | kNonWeakDefinitionFlag)) != 0) {
          fail("Unsupported legacy weak-bind symbol flags");
        }
        symbol_flags = immediate;
        symbol = nul_terminated(payload, cursor, payload.size(), "Unterminated legacy weak-bind symbol");
        cursor += symbol.size() + 1;
        if ((symbol_flags & kNonWeakDefinitionFlag) != 0) {
          planned.push_back({std::nullopt,
                             ChainedImport{symbol, ordinal, (symbol_flags & kWeakImportFlag) != 0,
                                           addend},
                             true});
        }
        break;
      }
      case kSetTypeImmediate:
        if (immediate != kBindTypePointer) {
          fail("Unsupported legacy weak-bind type " + std::to_string(immediate));
        }
        bind_type = immediate;
        break;
      case kSetAddendSLEB:
        addend = read_sleb(cursor);
        break;
      case kSetSegmentAndOffsetULEB:
        if (static_cast<std::size_t>(immediate) >= segments.size()) {
          fail("Invalid legacy weak-bind segment ordinal");
        }
        segment_index = static_cast<std::size_t>(immediate);
        segment_offset = read_uleb(payload, cursor, payload.size());
        if (segment_offset > selected_segment().vm_size) {
          fail("Legacy weak-bind starting offset exceeds segment bounds");
        }
        break;
      case kAddAddressULEB:
        (void)selected_segment();
        add_segment_offset(read_uleb(payload, cursor, payload.size()));
        break;
      case kDoBind:
        emit_bind();
        add_segment_offset(kPointerSize);
        break;
      case kDoBindAddAddressULEB: {
        emit_bind();
        const auto skip = read_uleb(payload, cursor, payload.size());
        add_segment_offset(kPointerSize + skip);
        break;
      }
      case kDoBindAddAddressImmediateScaled:
        emit_bind();
        add_segment_offset(kPointerSize + static_cast<std::uint64_t>(immediate) * kPointerSize);
        break;
      case kDoBindULEBTimesSkippingULEB: {
        const auto count = read_uleb(payload, cursor, payload.size());
        const auto skip = read_uleb(payload, cursor, payload.size());
        const auto& segment = selected_segment();
        if (count > segment.vm_size / kPointerSize || count > memory.size() / kPointerSize) {
          fail("Legacy weak-bind repeat count exceeds backing bounds");
        }
        const auto stride = kPointerSize + skip;
        for (std::uint64_t index = 0; index < count; ++index) {
          emit_bind();
          add_segment_offset(stride);
        }
        break;
      }
      default:
        fail("Unsupported legacy weak-bind opcode 0x" + [&]() {
          std::ostringstream formatted;
          formatted << std::hex << static_cast<unsigned>(opcode);
          return formatted.str();
        }());
    }
  }
  if (!finished) fail("Legacy weak-bind stream has no DONE opcode");
  return planned;
}

}  // namespace arm64metal
