#include "image_loader.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace arm64metal {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

std::optional<bool> arm64e_from_subtype(std::uint32_t subtype) {
  switch (subtype & 0x00ffffffU) {
    case 0:
    case 1: return false;
    case 2: return true;
    default: return std::nullopt;
  }
}

bool known_data_segment(const std::string& name) {
  return name == "__DATA" || name.rfind("__DATA_", 0) == 0 ||
         name == "__AUTH" || name.rfind("__AUTH_", 0) == 0;
}

std::string expand_loader_path(std::string name, const std::filesystem::path& loader,
                               const std::filesystem::path& executable_root) {
  const auto replace = [&name](const std::string& prefix, const std::filesystem::path& dir) {
    if (name == prefix) name = dir.string();
    else if (name.rfind(prefix + "/", 0) == 0)
      name = (dir / name.substr(prefix.size() + 1)).string();
  };
  replace("@loader_path", loader.parent_path());
  replace("@executable_path", executable_root.parent_path());
  return name;
}

std::optional<std::filesystem::path> resolve_local_dependency_path(
    const std::string& name, const std::filesystem::path& loader,
    const std::filesystem::path& executable_root,
    const std::vector<std::string>& inherited_rpaths) {
  std::vector<std::filesystem::path> candidates;
  if (name.rfind("@rpath/", 0) == 0) {
    const auto leaf = name.substr(7);
    for (const auto& rpath : inherited_rpaths)
      candidates.emplace_back(std::filesystem::path(rpath) / leaf);
  } else {
    candidates.emplace_back(expand_loader_path(name, loader, executable_root));
  }
  for (const auto& candidate : candidates) {
    if (candidate.string().empty() || candidate.string().front() == '@' ||
        !std::filesystem::exists(candidate)) continue;
    return std::filesystem::canonical(candidate);
  }
  return {};
}

std::string fixed_name(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                       std::size_t width) {
  if (offset > bytes.size() || width > bytes.size() - offset) fail("Truncated Mach-O name");
  auto end = std::find(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset + width), 0);
  return std::string(bytes.begin() + static_cast<std::ptrdiff_t>(offset), end);
}

std::string bounded_string(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                           std::size_t end, const char* error) {
  if (offset >= end || end > bytes.size()) fail(error);
  auto terminator = std::find(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                              bytes.begin() + static_cast<std::ptrdiff_t>(end), 0);
  if (terminator == bytes.begin() + static_cast<std::ptrdiff_t>(end)) fail(error);
  return std::string(bytes.begin() + static_cast<std::ptrdiff_t>(offset), terminator);
}

struct SymbolTable {
  std::size_t offset;
  std::size_t count;
  std::size_t strings;
  std::size_t string_size;
};

struct IndirectTable {
  std::size_t offset;
  std::size_t count;
};

struct Segment {
  std::string name;
  std::uint64_t address;
  std::uint64_t size;
  std::uint64_t file_offset;
  std::uint64_t file_size;
  bool writable;
  bool executable;
};

struct StubSection {
  std::uint64_t address;
  std::uint64_t size;
  std::size_t indirect_index;
  std::size_t stride;
};

std::pair<std::uint64_t, std::uint64_t> section_named(const std::vector<std::uint8_t>& bytes,
                                                      std::size_t offset) {
  return {read_integer(bytes, offset + 32, 8), read_integer(bytes, offset + 40, 8)};
}

}  // namespace

std::uint64_t GuestBuildVersion::token(bool query_sdk) const noexcept {
  return (static_cast<std::uint64_t>(query_sdk ? sdk : minimum) << 32) | platform;
}

MachImage::MachImage(const std::filesystem::path& path, bool executable, std::size_t placement) {
  MachO file(path, true);
  if (executable) code_policy = guest_code_policy(path, file.bytes());
  arm64e_code_abi = arm64e_from_subtype(file.word(8));
  const auto& bytes = file.bytes();
  if (file.word(12) != (executable ? 2U : 6U)) {
    fail(executable ? "Launch requires an MH_EXECUTE Mach-O"
                    : "Expected a guest Mach-O dylib at " + path.string());
  }
  const std::size_t command_count = file.word(16);
  const std::size_t command_bytes = file.word(20);
  if (command_bytes > bytes.size() || 32 > bytes.size() - command_bytes ||
      command_count > command_bytes / 8) fail("Invalid executable load commands");
  const std::size_t command_end = 32 + command_bytes;

  struct InitializerSection { std::uint64_t address, size; bool pointers; };
  struct TlvSection { std::uint64_t address, size; std::uint32_t alignment, type; };
  std::vector<Segment> segments;
  std::vector<LegacyRebaseSegment> legacy_rebase_segments;
  std::optional<std::uint64_t> base;
  std::optional<std::uint64_t> entry_offset;
  uuid.assign(16, 0);
  std::optional<std::pair<std::uint64_t, std::uint64_t>> text_section;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> message_stubs;
  std::optional<StubSection> stubs;
  std::vector<StubSection> pointer_sections;
  std::optional<SymbolTable> symbols;
  std::optional<IndirectTable> indirect;
  std::optional<std::pair<std::size_t, std::size_t>> fixups;
  std::optional<std::pair<std::size_t, std::size_t>> legacy_rebase_opcodes;
  std::optional<std::pair<std::size_t, std::size_t>> legacy_bind_opcodes;
  std::optional<std::pair<std::size_t, std::size_t>> legacy_weak_bind_opcodes;
  std::optional<std::pair<std::size_t, std::size_t>> legacy_lazy_bind_opcodes;
  bool sawChainedFixupsCommand = false;
  bool sawLegacyDyldInfoCommand = false;
  std::optional<std::pair<std::size_t, std::size_t>> export_trie;
  std::vector<InitializerSection> initializer_sections;
  std::vector<TlvSection> tlv_thunk_sections;
  std::vector<TlvSection> tlv_template_sections;
  std::vector<TlvSection> tlv_init_sections;
  std::uint64_t cursor = 32;

  for (std::size_t command_index = 0; command_index < command_count; ++command_index) {
    if (cursor > command_end - 8) fail("Truncated executable load command");
    const auto command = file.word(cursor);
    const auto length = static_cast<std::size_t>(file.word(cursor + 4));
    if (length < 8 || length > command_end - cursor) fail("Invalid executable load command");
    const auto quad = [&bytes](std::size_t at) { return read_integer(bytes, at, 8); };
    switch (command) {
      case 0x19: {
        if (length < 72) fail("Truncated executable segment");
        const auto name = fixed_name(bytes, cursor + 8, 16);
        const auto address = quad(cursor + 24);
        if (name == "__TEXT") base = address;
        const auto segment_size = quad(cursor + 32);
        const auto file_offset = quad(cursor + 40);
        const auto file_size = quad(cursor + 48);
        legacy_rebase_segments.push_back({address, segment_size});
        if (name != "__PAGEZERO" && name != "__LINKEDIT") {
          const auto protections = file.word(cursor + 60);
          segments.push_back({name, address, segment_size, file_offset, file_size,
                              (protections & 2U) != 0, (protections & 4U) != 0});
        }
        const auto section_count = static_cast<std::size_t>(file.word(cursor + 64));
        if (section_count > (length - 72) / 80) fail("Invalid executable sections");
        for (std::size_t section_index = 0; section_index < section_count; ++section_index) {
          const std::size_t section = cursor + 72 + section_index * 80;
          const auto section_name = fixed_name(bytes, section, 16);
          if (name == "__RESTRICT" && section_name == "__restrict") restricted = true;
          if (name == "__TEXT" && section_name == "__text") text_section = section_named(bytes, section);
          if (name == "__TEXT" && section_name == "__objc_stubs") {
            const auto [start, size] = section_named(bytes, section);
            if ((file.word(cursor + 60) & 4U) == 0 || start < address ||
                start - address > file_size || size > file_size - (start - address) ||
                (start & 3U) != 0 || (size & 3U) != 0) {
              fail("Invalid Objective-C message stub section");
            }
            if (size != 0) message_stubs.emplace_back(start, size);
          }
          const auto type = file.word(section + 64) & 0xffU;
          if (type == 10) fail("Guest image finalizers are unsupported");
          if (type == 9 || type == 0x16) {
            const auto [address_value, size] = section_named(bytes, section);
            initializer_sections.push_back({address_value, size, type == 9});
          }
          if (type == 0x13 || type == 0x15 || type == 0x11 || type == 0x12) {
            const auto address_value = quad(section + 32);
            const auto size = quad(section + 40);
            const auto alignment = file.word(section + 52);
            if (alignment >= 63) fail("Invalid thread-local section alignment");
            const TlvSection tlv{address_value, size, alignment, type};
            if (type == 0x13) tlv_thunk_sections.push_back(tlv);
            else if (type == 0x15) tlv_init_sections.push_back(tlv);
            else tlv_template_sections.push_back(tlv);
          }
          if (type == 6) {
            pointer_sections.push_back({quad(section + 32), quad(section + 40),
                                        file.word(section + 68), 8});
          }
          if (type == 8) {
            if (stubs) fail("Multiple import stub sections are unsupported");
            stubs = StubSection{quad(section + 32), quad(section + 40), file.word(section + 68),
                                file.word(section + 72)};
          }
        }
        break;
      }
      case 0xc:
      case 0x80000018:
      case 0x8000001f:
      case 0x20:
      case 0x80000023: {
        if (length < 24) fail("Truncated dylib load command");
        const auto name_offset = static_cast<std::size_t>(file.word(cursor + 8));
        const bool uses_dylib_use_flags = file.word(cursor + 12) == 0x1a741800U;
        std::uint32_t dylib_use_flags = 0;
        if (uses_dylib_use_flags) {
          if ((command != 0xc && command != 0x80000018) || length < 28 || name_offset != 28)
            fail("Invalid dylib use command");
          dylib_use_flags = file.word(cursor + 24);
          if ((dylib_use_flags & ~0x0fU) != 0) fail("Unsupported dylib use flags");
        }
        if (name_offset < (uses_dylib_use_flags ? 28U : 24U) || name_offset >= length)
          fail("Invalid dylib install name");
        dependencies.push_back({bounded_string(bytes, cursor + name_offset, cursor + length,
                                                  "Invalid dylib install name"),
                                command == 0x80000018, command == 0x8000001f,
                                file.word(cursor + 16), command == 0x80000023,
                                command == 0x20, (dylib_use_flags & 0x8U) != 0});
        break;
      }
      case 0x1b:
        if (executable) {
          if (length < 24) fail("Truncated executable UUID");
          uuid.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor + 8),
                      bytes.begin() + static_cast<std::ptrdiff_t>(cursor + 24));
        }
        break;
      case 0x32:
        if (executable) {
          if (length < 24) fail("Truncated guest build version");
          const auto minimum = file.word(cursor + 12);
          auto sdk = file.word(cursor + 16);
          if (sdk == 0) sdk = minimum;
          build_version = GuestBuildVersion{file.word(cursor + 8), minimum, sdk};
        }
        break;
      case 0x24:
        if (executable && !build_version) {
          if (length < 16) fail("Truncated guest minimum version");
          const auto minimum = file.word(cursor + 8);
          auto sdk = file.word(cursor + 12);
          if (sdk == 0) sdk = minimum;
          build_version = GuestBuildVersion{1, minimum, sdk};
        }
        break;
      case 0x8000001c: {
        if (length < 12) fail("Truncated rpath load command");
        const auto offset = static_cast<std::size_t>(file.word(cursor + 8));
        if (offset < 12 || offset >= length) fail("Invalid Mach-O rpath");
        rpaths.push_back(bounded_string(bytes, cursor + offset, cursor + length, "Invalid Mach-O rpath"));
        break;
      }
      case 0x80000033:
      case 0x22:
      case 0x80000022: {
        if (command == 0x22 || command == 0x80000022) {
          if (sawLegacyDyldInfoCommand)
            fail("Multiple legacy dyld info commands are unsupported");
          sawLegacyDyldInfoCommand = true;
          if (length < 48) fail("Truncated dyld info command");
          const auto rebase_offset = static_cast<std::size_t>(file.word(cursor + 8));
          const auto rebase_size = static_cast<std::size_t>(file.word(cursor + 12));
          if (rebase_offset > bytes.size() || rebase_size > bytes.size() - rebase_offset)
            fail("Invalid legacy dyld rebase opcode range");
          if (rebase_size != 0) {
            if (legacy_rebase_opcodes)
              fail("Multiple legacy dyld rebase streams are unsupported");
            legacy_rebase_opcodes = std::make_pair(rebase_offset, rebase_size);
          }
          const auto read_bind_range = [&](std::size_t offset_field, std::size_t size_field,
                                           std::optional<std::pair<std::size_t, std::size_t>>& range,
                                           const char* error) {
            const auto start = static_cast<std::size_t>(file.word(cursor + offset_field));
            const auto size = static_cast<std::size_t>(file.word(cursor + size_field));
            if (start > bytes.size() || size > bytes.size() - start) fail(error);
            if (size != 0) range = std::make_pair(start, size);
          };
          read_bind_range(16, 20, legacy_bind_opcodes, "Invalid legacy dyld bind opcode range");
          read_bind_range(24, 28, legacy_weak_bind_opcodes,
                          "Invalid legacy dyld weak-bind opcode range");
          read_bind_range(32, 36, legacy_lazy_bind_opcodes,
                          "Invalid legacy dyld lazy-bind opcode range");
        }
        const auto min_length = command == 0x80000033 ? 16U : 48U;
        if (length >= min_length) {
          const auto delta = command == 0x80000033 ? 8U : 40U;
          const auto start = static_cast<std::size_t>(file.word(cursor + delta));
          const auto size = static_cast<std::size_t>(file.word(cursor + delta + 4));
          if (start > bytes.size() || size > bytes.size() - start) fail("Invalid Mach-O exports trie range");
          if (size != 0) export_trie = std::make_pair(start, size);
        }
        break;
      }
      case 0x80000034:
        if (sawChainedFixupsCommand)
          fail("Multiple chained fixup commands are unsupported");
        sawChainedFixupsCommand = true;
        if (length < 16) fail("Truncated dyld chained fixup command");
        {
          const auto start = static_cast<std::size_t>(file.word(cursor + 8));
          const auto size = static_cast<std::size_t>(file.word(cursor + 12));
          if (start > bytes.size() || size > bytes.size() - start)
            fail("Invalid dyld chained fixup payload");
          fixups = std::make_pair(start, size);
        }
        break;
      case 0x80000028:
        if (length >= 24) entry_offset = quad(cursor + 8);
        break;
      case 2:
        if (length >= 24) {
          symbols = SymbolTable{file.word(cursor + 8), file.word(cursor + 12),
                                file.word(cursor + 16), file.word(cursor + 20)};
        }
        break;
      case 0xb:
        if (length >= 80) indirect = IndirectTable{file.word(cursor + 56), file.word(cursor + 60)};
        break;
      default: break;
    }
    cursor += length;
  }

  if (fixups && (legacy_rebase_opcodes || legacy_bind_opcodes ||
                 legacy_weak_bind_opcodes || legacy_lazy_bind_opcodes))
    fail("Legacy and chained dyld fixups cannot both be present");
  if (legacy_weak_bind_opcodes)
    fail("Legacy weak-bind coalescing semantics are unsupported");

  if (!base || (*base & 0xfffU) != 0 || !text_section) {
    fail("Missing executable entry point or text segment");
  }
  preferred_base = *base;
  constexpr std::uint64_t image_cap = 32 * 1024 * 1024;
  const auto region = [&](std::uint64_t address, std::uint64_t size) {
    if (address < preferred_base || address - preferred_base > image_cap ||
        size > image_cap - (address - preferred_base)) fail("Executable exceeds the guest image limit");
    return std::make_pair(static_cast<std::size_t>(address - preferred_base),
                          static_cast<std::size_t>(address - preferred_base + size));
  };
  const auto text = region(text_section->first, text_section->second);
  if (executable && (!entry_offset || *entry_offset > image_cap ||
                     *entry_offset < text.first || *entry_offset >= text.second ||
                     (*entry_offset & 3U) != 0)) {
    fail("LC_MAIN entry lies outside executable text");
  }
  std::vector<std::pair<std::size_t, std::size_t>> mapped_regions;
  for (const auto& segment : segments) {
    const auto mapped = region(segment.address, segment.size);
    mapped_regions.push_back(mapped);
    mapped_segments.push_back({segment.name, mapped.first, mapped.second - mapped.first,
                               segment.executable, known_data_segment(segment.name)});
    if (segment.writable) writable_regions.push_back(mapped);
    if (segment.file_offset > bytes.size() || segment.file_size > bytes.size() - segment.file_offset ||
        segment.file_size > segment.size) fail("Invalid executable segment bounds");
    if (memory.size() < mapped.second) memory.resize(mapped.second, 0);
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(segment.file_offset),
                static_cast<std::size_t>(segment.file_size),
                memory.begin() + static_cast<std::ptrdiff_t>(mapped.first));
  }
  if (text.second > memory.size()) fail("Missing executable text bytes");

  if (legacy_rebase_opcodes) {
    std::vector<std::uint8_t> payload(
        bytes.begin() + static_cast<std::ptrdiff_t>(legacy_rebase_opcodes->first),
        bytes.begin() + static_cast<std::ptrdiff_t>(legacy_rebase_opcodes->first +
                                                    legacy_rebase_opcodes->second));
    const auto rebases = decode_legacy_rebase_opcodes(
        payload, preferred_base, placement, legacy_rebase_segments, memory, mapped_regions);
    for (const auto& rebase : rebases) {
      if (rebase.location > memory.size() || 8 > memory.size() - rebase.location)
        fail("Validated legacy rebase location exceeds guest image");
    }
    for (const auto& rebase : rebases) {
      for (unsigned byte = 0; byte < 8; ++byte) {
        memory[rebase.location + byte] =
            static_cast<std::uint8_t>(rebase.target >> (byte * 8U));
      }
    }
  }

  const auto merge_legacy_binds = [&](const auto& range, bool lazy) {
    if (!range) return;
    const auto [start, size] = *range;
    std::vector<std::uint8_t> payload(
        bytes.begin() + static_cast<std::ptrdiff_t>(start),
        bytes.begin() + static_cast<std::ptrdiff_t>(start + size));
    const auto decoded = decode_legacy_bind_opcodes(
        payload, preferred_base, legacy_rebase_segments, memory, mapped_regions, lazy);
    for (const auto& bind : decoded) {
      if (bind.lazy != lazy) fail("Legacy dyld bind decoder returned an inconsistent stream kind");
      if (bind.non_weak_definition)
        fail("Legacy dyld non-weak-definition binding semantics are unsupported");
      if (bind.lazy) lazy_bind_locations.insert(bind.location);
      const auto inserted = bindings.emplace(bind.location, bind.imported);
      if (!inserted.second) fail("Multiple dyld binding records target the same slot");
    }
  };
  merge_legacy_binds(legacy_bind_opcodes, false);
  merge_legacy_binds(legacy_lazy_bind_opcodes, true);

  const auto import_names = [&](const StubSection& section) {
    std::map<std::size_t, ChainedImport> result;
    const auto mapped = region(section.address, section.size);
    if (!symbols || !indirect || section.stride == 0 ||
        (mapped.second - mapped.first) % section.stride != 0 ||
        symbols->offset > bytes.size() || symbols->count > (bytes.size() - symbols->offset) / 16 ||
        symbols->strings > bytes.size() || symbols->string_size > bytes.size() - symbols->strings ||
        indirect->offset > bytes.size() || indirect->count > (bytes.size() - indirect->offset) / 4 ||
        section.indirect_index > indirect->count ||
        (mapped.second - mapped.first) / section.stride > indirect->count - section.indirect_index) {
      fail("Invalid executable import tables");
    }
    for (std::size_t index = 0, address = mapped.first; address < mapped.second;
         ++index, address += section.stride) {
      const auto symbol_index = file.word(indirect->offset + (section.indirect_index + index) * 4);
      if (symbol_index >= symbols->count) continue;
      const auto record = symbols->offset + symbol_index * 16;
      const auto string_offset = file.word(record);
      if (string_offset >= symbols->string_size) fail("Invalid import name");
      const auto name = bounded_string(bytes, symbols->strings + string_offset,
                                       symbols->strings + symbols->string_size, "Invalid import name");
      const auto symbolDescription = read_integer(bytes, record + 6, 2);
      const bool weakReference = (symbolDescription & 0x0040U) != 0;
      result.emplace(address, ChainedImport{name,
                                            static_cast<std::int8_t>(bytes[record + 7]),
                                            weakReference, 0});
    }
    return result;
  };
  if (stubs) this->stubs = import_names(*stubs);
  for (const auto& section : pointer_sections) {
    auto names = import_names(section);
    bindings.insert(names.begin(), names.end());
    for (const auto& entry : names) bindings[entry.first] = entry.second;
  }
  if (fixups) {
    std::vector<std::uint8_t> payload(bytes.begin() + static_cast<std::ptrdiff_t>(fixups->first),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(fixups->first + fixups->second));
    auto chained = apply_chained_fixups(payload, preferred_base, placement, memory, mapped_regions);
    authentication = std::move(chained.authentication);
    for (auto& binding : chained.bindings) bindings[binding.first] = std::move(binding.second);
  }

  if (export_trie) {
    std::vector<std::uint8_t> trie(bytes.begin() + static_cast<std::ptrdiff_t>(export_trie->first),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(export_trie->first + export_trie->second));
    exports = mach_exports(trie, memory.size());
  } else if (symbols) {
    if (symbols->offset > bytes.size() || symbols->count > (bytes.size() - symbols->offset) / 16 ||
        symbols->strings > bytes.size() || symbols->string_size > bytes.size() - symbols->strings) {
      fail("Invalid guest export symbol table");
    }
    for (std::size_t index = 0; index < symbols->count; ++index) {
      const auto offset = symbols->offset + index * 16;
      if ((bytes[offset + 4] & 0xffU) != 0x0f) continue;
      const auto address = read_integer(bytes, offset + 8, 8);
      if (address < preferred_base || address - preferred_base >= memory.size()) continue;
      const auto name_offset = file.word(offset);
      if (name_offset >= symbols->string_size) fail("Invalid guest export name");
      auto name = bounded_string(bytes, symbols->strings + name_offset,
                                 symbols->strings + symbols->string_size, "Invalid guest export name");
      exports.emplace(std::move(name), MachExport{ExportKind::address, address - preferred_base, 0, {}});
    }
  }

  for (const auto& section : initializer_sections) {
    const auto mapped = region(section.address, section.size);
    const std::size_t width = section.pointers ? 8 : 4;
    if ((mapped.second - mapped.first) % width != 0 || mapped.second > memory.size()) {
      fail("Invalid guest initializer section");
    }
    for (std::size_t offset = mapped.first; offset < mapped.second; offset += width) {
      const auto raw = read_integer(memory, offset, width);
      std::uint64_t address = raw;
      if (section.pointers) {
        if (raw < placement) fail("Guest initializer pointer is not relocated to its placement");
        address = raw - placement;
      } else if (raw > std::numeric_limits<std::uint32_t>::max()) {
        fail("Unsupported guest initializer offset");
      }
      if (address < text.first || address >= text.second || (address & 3) != 0) {
        fail("Guest initializer outside its image text");
      }
      initializers.push_back(static_cast<std::size_t>(address));
    }
  }
  if (!tlv_thunk_sections.empty() || !tlv_template_sections.empty() || !tlv_init_sections.empty()) {
    if (tlv_thunk_sections.empty() || tlv_template_sections.empty())
      fail("Incomplete guest thread-local metadata");
    ThreadLocalImageMetadata metadata;
    std::uint64_t template_start = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t template_end = 0;
    for (const auto& section : tlv_template_sections) {
      const auto mapped = region(section.address, section.size);
      if (mapped.second > memory.size()) fail("Thread-local template is outside its image");
      metadata.template_ranges.emplace_back(mapped.first, mapped.second - mapped.first);
      template_start = std::min(template_start, static_cast<std::uint64_t>(mapped.first));
      template_end = std::max(template_end, static_cast<std::uint64_t>(mapped.second));
      metadata.template_alignment = std::max(metadata.template_alignment,
                                             std::uint64_t{1} << section.alignment);
      if ((section.address & ((std::uint64_t{1} << section.alignment) - 1)) != 0)
        fail("Misaligned guest thread-local template");
      if (section.type == 0x11) metadata.all_zero_fill = false;
    }
    metadata.template_address = template_start;
    metadata.template_size = template_end - template_start;
    for (const auto& section : tlv_thunk_sections) {
      const auto mapped = region(section.address, section.size);
      if (mapped.second > memory.size() || (mapped.second - mapped.first) % 24 != 0)
        fail("Invalid guest thread-local thunk section");
      metadata.thunk_ranges.emplace_back(mapped.first, mapped.second - mapped.first);
      for (std::size_t offset = mapped.first; offset < mapped.second; offset += 24) {
        const auto target = read_integer(memory, offset + 16, 8);
        if (target > metadata.template_size)
          fail("Guest thread-local descriptor offset exceeds its template");
      }
    }
    for (const auto& section : tlv_init_sections) {
      const auto mapped = region(section.address, section.size);
      if ((mapped.second - mapped.first) % 8 != 0 || mapped.second > memory.size())
        fail("Invalid guest thread-local init callback section");
      metadata.init_callback_ranges.emplace_back(mapped.first, mapped.second - mapped.first);
      for (std::size_t offset = mapped.first; offset < mapped.second; offset += 8) {
        const auto raw = read_integer(memory, offset, 8);
        if (raw < placement || raw - placement < text.first || raw - placement >= text.second ||
            ((raw - placement) & 3U) != 0)
          fail("Guest thread-local init callback outside its image text");
        metadata.init_callbacks.push_back(raw - placement);
      }
    }
    thread_locals = std::move(metadata);
  }
  preferred_base = *base;
  if (executable) entry = static_cast<std::size_t>(*entry_offset);
  code.push_back(text);
  for (const auto& stub : message_stubs) code.push_back(region(stub.first, stub.second));
  for (const auto& mapped : mapped_regions) {
    if (std::find(writable_regions.begin(), writable_regions.end(), mapped) == writable_regions.end()) {
      immutable_regions.push_back(mapped);
    }
  }
}

ExecutableImage::ExecutableImage(const std::filesystem::path& binary, bool shared_cache,
                                 bool demand_cache,
                                 const std::unordered_map<std::string, int>& host_service_ids,
                                 const std::set<std::string>& guest_memory_imports,
                                 bool collect_startup_initializer_metadata) {
  host_service_ids_ = host_service_ids;
  guest_memory_imports_ = guest_memory_imports;
  struct Loaded {
    MachImage image;
    std::size_t base;
    std::string path;
    std::vector<std::optional<std::size_t>> deps;
    std::vector<std::string> rpaths;
  };
  const auto root = std::filesystem::canonical(binary);
  std::vector<Loaded> loaded;
  std::map<std::string, std::size_t> known;
  std::vector<std::size_t> init_order;
  constexpr std::size_t max_image_size = 64 * 1024 * 1024;
  std::function<std::size_t(std::filesystem::path, bool, std::vector<std::string>)> load;
  load = [&](std::filesystem::path path, bool is_root, std::vector<std::string> inherited) -> std::size_t {
    path = std::filesystem::canonical(path);
    if (auto found = known.find(path.string()); found != known.end()) return found->second;
    const std::size_t base = std::max(minimum_image_address, (memory.size() + 16'383) & ~std::size_t(16'383));
    MachImage image(path, is_root, base);
    if (loaded.size() >= 256 || image.memory.size() > max_image_size - base) fail("Guest image graph exceeds loader limits");
    memory.resize(base, 0);
    memory.insert(memory.end(), image.memory.begin(), image.memory.end());
    const auto index = loaded.size();
    known.emplace(path.string(), index);
    loaded.push_back({std::move(image), base, path.string(), {}, {}});
    std::vector<std::string> rpaths;
    for (const auto& rp : loaded[index].image.rpaths)
      rpaths.push_back(expand_loader_path(rp, path, root));
    rpaths.insert(rpaths.end(), inherited.begin(), inherited.end());
    loaded[index].rpaths = rpaths;
    std::vector<std::optional<std::size_t>> dependencies;
    for (const auto& dependency : loaded[index].image.dependencies) {
      const auto& name = dependency.name;
      if (name == "/usr/lib/libSystem.B.dylib" || name.rfind("/usr/lib/", 0) == 0 ||
          name.rfind("/System/Library/", 0) == 0) { dependencies.emplace_back(); continue; }
      const auto resolved = resolve_local_dependency_path(name, path, root, rpaths);
      if (!resolved) {
        if (dependency.weak) dependencies.emplace_back();
        else fail("Unable to resolve guest dylib " + name);
      } else dependencies.emplace_back(load(*resolved, false, rpaths));
    }
    loaded[index].deps = std::move(dependencies);
    init_order.push_back(index);
    return index;
  };
  load(root, true, {});
  loaded_image_graph.reserve(loaded.size());
  for (const auto& item : loaded) {
    LoadedImageGraphRecord record;
    record.guest_header = item.base;
    record.canonical_path = item.path;
    record.rpaths = item.rpaths;
    record.exports = item.image.exports;
    record.dependencies.reserve(item.image.dependencies.size());
    for (std::size_t index = 0; index < item.image.dependencies.size(); ++index) {
      LoadedImageDependency dependency{item.image.dependencies[index], {}};
      if (item.deps[index]) dependency.resolved_header = loaded[*item.deps[index]].base;
      record.dependencies.push_back(std::move(dependency));
    }
    loaded_image_graph.push_back(std::move(record));
  }
  linear_image_end = memory.size();
  if (!loaded.front().image.entry) fail("Missing executable entry");
  entry = loaded.front().base + *loaded.front().image.entry;
  main_header = static_cast<std::int64_t>(loaded.front().base);
  main_uuid = loaded.front().image.uuid;
  main_restricted = loaded.front().image.restricted;
  main_code_policy = loaded.front().image.code_policy;
  build_version = loaded.front().image.build_version;
  linked_libraries_ = loaded.front().image.dependencies;
  for (const auto& image : loaded) {
    image_slides.emplace(image.base, image.base - image.image.preferred_base);
    image_paths.emplace(image.base, image.path);
    for (const auto& segment : image.image.mapped_segments) {
      if (segment.size == 0) continue;
      if (segment.offset > image.image.memory.size() ||
          segment.size > image.image.memory.size() - segment.offset ||
          segment.offset > std::numeric_limits<std::uint64_t>::max() - image.base ||
          segment.size > std::numeric_limits<std::uint64_t>::max() -
                             (static_cast<std::uint64_t>(image.base) + segment.offset))
        fail("Invalid mapped local image segment");
      const auto start = static_cast<std::uint64_t>(image.base) + segment.offset;
      const auto end = start + segment.size;
      if (end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        fail("Mapped local image segment exceeds guest address space");
      mapped_image_segments.push_back({static_cast<std::uint64_t>(image.base),
                                       static_cast<std::int64_t>(start),
                                       static_cast<std::int64_t>(end), image.path,
                                       segment.name, segment.executable,
                                       segment.data_segment});
    }
    if (image.image.thread_locals) {
      auto metadata = *image.image.thread_locals;
      if (metadata.template_address > std::numeric_limits<std::uint64_t>::max() - image.base)
        fail("Guest thread-local template address overflows");
      metadata.template_address += image.base;
      for (auto& range : metadata.thunk_ranges) {
        if (range.first > std::numeric_limits<std::uint64_t>::max() - image.base ||
            range.first > image.image.memory.size() ||
            range.second > image.image.memory.size() - range.first)
          fail("Guest thread-local thunk range is invalid");
        range.first += image.base;
      }
      for (auto& range : metadata.template_ranges) {
        if (range.first > std::numeric_limits<std::uint64_t>::max() - image.base ||
            range.first > image.image.memory.size() ||
            range.second > image.image.memory.size() - range.first)
          fail("Guest thread-local template range is invalid");
        range.first += image.base;
      }
      for (auto& range : metadata.init_callback_ranges) {
        if (range.first > std::numeric_limits<std::uint64_t>::max() - image.base ||
            range.first > image.image.memory.size() ||
            range.second > image.image.memory.size() - range.first)
          fail("Guest thread-local callback range is invalid");
        range.first += image.base;
      }
      for (auto& callback : metadata.init_callbacks) {
        if (callback > std::numeric_limits<std::uint64_t>::max() - image.base)
          fail("Guest thread-local init callback address overflows");
        callback += image.base;
      }
      thread_local_images.emplace(image.base, std::move(metadata));
    }
    for (const auto& r : image.image.code) {
      const auto start = static_cast<std::int64_t>(image.base + r.first);
      const auto end = static_cast<std::int64_t>(image.base + r.second);
      text_regions.emplace_back(start, end);
      code_abi_regions.push_back({start, end, image.image.arm64e_code_abi});
    }
    for (const auto& r : image.image.writable_regions) writable_regions.emplace_back(image.base + r.first, image.base + r.second);
    for (const auto& r : image.image.immutable_regions) immutable_regions.emplace_back(image.base + r.first, image.base + r.second);
    for (const auto& auth : image.image.authentication) authentication.emplace(image.base + auth.first, auth.second);
  }
  std::unordered_map<std::string, SharedCache::GuestImage> cached;
  std::unordered_map<std::string, std::size_t> cached_headers;
  std::unordered_map<std::string, std::unordered_map<std::string, std::int64_t>> cache_exports;
  std::set<std::string> modeledDataExportNames;
  if (shared_cache) {
    auto cache = SharedCache();
    std::set<std::string> pending;
    const auto prespecializedServiceRequested = std::any_of(
        loaded.begin(), loaded.end(), [](const auto& item) {
          return std::any_of(item.image.stubs.begin(), item.image.stubs.end(),
                             [](const auto& stub) {
            return stub.second.name == "__dyld_get_swift_prespecialized_data";
          });
        });
    if (prespecializedServiceRequested) {
      swift_prespecialized_data = cache.swiftPrespecializedAddress();
      if (swift_prespecialized_data) pending.insert(SharedCache::swiftPrespecializedLibrary);
    }
    std::unordered_map<std::string, std::pair<std::string, std::int64_t>> demand_libsystem_exports;
    std::map<std::string, SharedCache::GuestImage> demand_reexport_metadata;
    for (const auto& item : loaded) for (const auto& dependency : item.image.dependencies) {
      if (dependency.name != "/usr/lib/libSystem.B.dylib" &&
          (dependency.name.rfind("/usr/lib/", 0) == 0 || dependency.name.rfind("/System/Library/", 0) == 0))
        pending.insert(dependency.name);
    }
    if (demand_cache) {
      constexpr const char* lib_system = "/usr/lib/libSystem.B.dylib";
      const auto umbrella = cache.guestImage(lib_system);
      if (umbrella) {
        demand_reexport_metadata.emplace(lib_system, *umbrella);
        std::vector<std::string> reexports;
        for (const auto& dependency : umbrella->dependencies)
          if (dependency.reexported) reexports.push_back(dependency.name);
        for (std::size_t cursor = 0; cursor < reexports.size(); ++cursor) {
          const auto path = reexports[cursor];
          if (demand_reexport_metadata.count(path) != 0 || demand_reexport_metadata.size() >= 1024) continue;
          auto image = cache.guestImage(path);
          if (!image) continue;
          for (const auto& dependency : image->dependencies)
            if (dependency.reexported) reexports.push_back(dependency.name);
          demand_reexport_metadata.emplace(path, std::move(*image));
        }
        const auto find_provider = [&cache, &demand_reexport_metadata](const std::string& symbol,
                                                                       std::int64_t target) {
          const auto original = cache.originalAddress(target);
          for (const auto& candidate : demand_reexport_metadata) {
            const auto exported = candidate.second.exports.find(symbol);
            if (exported == candidate.second.exports.end() || exported->second != target) continue;
            const auto owns_address = std::any_of(candidate.second.segments.begin(), candidate.second.segments.end(),
                                                  [original](const auto& segment) {
              return original >= segment.address && original - segment.address < segment.size;
            });
            if (owns_address) return candidate.first;
          }
          return std::string{};
        };
        const auto consider = [&](const ChainedImport& import, const std::vector<ImageDependency>& dependencies) {
          if (import.ordinal <= 0 || static_cast<std::size_t>(import.ordinal) > dependencies.size() ||
              dependencies[static_cast<std::size_t>(import.ordinal - 1)].name != lib_system ||
              host_service_ids.count(import.name) != 0 || guest_memory_imports.count(import.name) != 0) return;
          const auto exported = umbrella->exports.find(import.name);
          if (exported == umbrella->exports.end()) return;
          const auto provider = find_provider(import.name, exported->second);
          if (!provider.empty()) {
            pending.insert(provider);
            demand_libsystem_exports[import.name] = {provider, exported->second};
          }
        };
        for (const auto& item : loaded) {
          for (const auto& stub : item.image.stubs) consider(stub.second, item.image.dependencies);
          for (const auto& binding : item.image.bindings) consider(binding.second, item.image.dependencies);
        }
      }
    }
    std::size_t traversedPreboundLazyLoads = 0;
    constexpr std::size_t maximumPreboundLazyLoads = 4096;
    while (!pending.empty()) {
      const auto name = *pending.begin(); pending.erase(pending.begin());
      if (cached.count(name) || cached.size() >= (demand_cache ? 1024U : 256U)) continue;
      auto metadata = demand_reexport_metadata.find(name);
      auto image = metadata == demand_reexport_metadata.end()
          ? cache.guestImage(name) : std::optional<SharedCache::GuestImage>(std::move(metadata->second));
      if (metadata != demand_reexport_metadata.end()) demand_reexport_metadata.erase(metadata);
      if (!image) continue;
      if (name == "/usr/lib/swift/libswiftCore.dylib" && !swift_prespecialized_data) {
        swift_prespecialized_data = cache.swiftPrespecializedAddress();
        if (swift_prespecialized_data) pending.insert(SharedCache::swiftPrespecializedLibrary);
      }
      for (const auto& dependency : image->dependencies) {
        if (dependency.name != "/usr/lib/libSystem.B.dylib" &&
            (dependency.name.rfind("/usr/lib/", 0) == 0 || dependency.name.rfind("/System/Library/", 0) == 0))
          pending.insert(dependency.name);
      }
      for (const auto& [flag, dependency] : cache.preboundLazyDependencies(name)) {
        (void)flag;
        if (++traversedPreboundLazyLoads > maximumPreboundLazyLoads) {
          fail("Cached lazy-load graph exceeds 4096 prebound records");
        }
        if (cache.images().find(dependency) != cache.images().end()) {
          pending.insert(dependency);
        }
      }
      auto& cachedDependencies = cached_image_dependencies[name];
      cachedDependencies.reserve(image->dependencies.size());
      for (const auto& dependency : image->dependencies) {
        cachedDependencies.push_back({dependency.name, dependency.weak,
                                      dependency.reexported, 0, dependency.upward,
                                      false, dependency.delayed_init});
      }
      cached.emplace(name, std::move(*image));
    }
    if (!cached.empty()) cache_reader.emplace(std::move(cache));
    else named_load_cache_reader_.emplace(std::move(cache));
    if (cache_reader) for (const auto& pair : cached) {
      const auto& name = pair.first;
      const auto& img = pair.second;
      std::optional<std::size_t> header;
      for (const auto& segment : img.segments) if (segment.name == "__TEXT") {
        header = static_cast<std::size_t>(cache_reader->guestAddress(segment.address));
        image_slides[*header] = *header - segment.address;
        const auto existingPath = image_paths.find(*header);
        if (existingPath == image_paths.end() || name < existingPath->second)
          image_paths[*header] = name;
        cached_headers[name] = *header;
        break;
      }
      // guestImage() resolves exports to guest virtual addresses already.
      for (const auto& exp : img.exports) cache_exports[name][exp.first] = exp.second;
      for (const auto& segment : img.segments) {
        const auto address = cache_reader->guestAddress(segment.address);
        const auto length = static_cast<std::size_t>(segment.size);
        if (header && segment.name != "__PAGEZERO" && length != 0) {
          if (address < 0 ||
              length > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
              address > std::numeric_limits<std::int64_t>::max() -
                            static_cast<std::int64_t>(length))
            fail("Mapped cached image segment exceeds guest address space");
          const bool dataSegment = known_data_segment(segment.name);
          mapped_image_segments.push_back({static_cast<std::uint64_t>(*header), address,
                                           address + static_cast<std::int64_t>(length), name,
                                           segment.name, !dataSegment, dataSegment});
        }
        if (header && (segment.name.rfind("__DATA", 0) == 0 || segment.name.rfind("__AUTH", 0) == 0))
          cached_image_ranges.push_back({address, address + static_cast<std::int64_t>(length), static_cast<std::int64_t>(*header)});
        const auto logical_end = address + static_cast<std::int64_t>(length);
        if (demand_cache) {
          deferred_cache.push_back({address, logical_end, segment});
        } else {
          const auto backing = (memory.size() + 15U) & ~std::size_t(15U);
          if (length > 64 * 1024 * 1024 - backing) fail("Cached guest image graph exceeds 64 MiB backing limit at " + name + ": " + std::to_string(backing) + " bytes before segment " + segment.name + " (" + std::to_string(length) + " bytes)");
          if (logical_end > SharedCache::guestLimit) fail("Deferred cache segment exceeds guest window");
          if (std::any_of(cache_regions.begin(), cache_regions.end(), [address, logical_end](const auto& region) {
                return region.logical_start < logical_end && address < region.logical_end;
              })) fail("Overlapping cached guest segments");
          memory.resize(backing + length, 0);
          const auto data = cache_reader->bytes(segment.address, segment.fileSize);
          std::copy(data.begin(), data.end(), memory.begin() + static_cast<std::ptrdiff_t>(backing));
          cache_regions.push_back({address, logical_end, backing, segment.writable, true});
        }
      }
      for (const auto& range : img.text) {
        const auto start = cache_reader->guestAddress(range.first);
        const auto end = cache_reader->guestAddress(range.second);
        text_regions.emplace_back(start, end);
        std::optional<bool> arm64e;
        try {
          const auto headerBytes = cache_reader->bytes(cache_reader->images().at(name), 12);
          if (read_integer(headerBytes, 0, 4) == 0xfeedfacfU &&
              read_integer(headerBytes, 4, 4) == 0x0100000cU) {
            arm64e = arm64e_from_subtype(static_cast<std::uint32_t>(
                read_integer(headerBytes, 8, 4)));
          }
        } catch (const std::exception&) {
          // Cached code ABI remains unknown when its selected Mach-O header is unavailable.
        }
        code_abi_regions.push_back({start, end, arm64e});
      }
      if (!demand_cache) for (const auto& rebase : cache_reader->rebases(name)) {
        const auto address = cache_reader->guestAddress(rebase.location);
        auto region = std::find_if(cache_regions.begin(), cache_regions.end(), [address](const auto& r) {
          return r.logical_start <= address && address <= r.logical_end - 8;
        });
        if (region == cache_regions.end()) continue;
        const auto at = region->backing + static_cast<std::size_t>(address - region->logical_start);
        const auto target = static_cast<std::uint64_t>(cache_reader->guestAddress(rebase.target)) |
                            (static_cast<std::uint64_t>(rebase.highByte) << 56);
        for (std::size_t b = 0; b < 8; ++b) memory[at + b] = static_cast<std::uint8_t>(target >> (b * 8));
        if (rebase.authentication) {
          const auto& a = *rebase.authentication;
          const auto modifier = a.addressDiversity ? static_cast<std::uint64_t>(address) |
              (static_cast<std::uint64_t>(a.diversity) << 48) : a.diversity;
          authentication[at] = {a.dataKey ? 2U : 0U, modifier};
        }
      }
      if (header) {
        if (auto cachedTlv = cache_reader->tlvMetadata(name)) {
          const auto rangeBelongsToImage = [this, &img](
              const std::pair<std::uint64_t, std::uint64_t>& range) {
            for (const auto& segment : img.segments) {
              const auto start = static_cast<std::uint64_t>(cache_reader->guestAddress(segment.address));
              if (segment.size > std::numeric_limits<std::uint64_t>::max() - start ||
                  range.first < start || range.first - start > segment.size ||
                  range.second > segment.size - (range.first - start)) continue;
              return true;
            }
            return false;
          };
          ThreadLocalImageMetadata metadata;
          metadata.thunk_ranges = cachedTlv->thunk_ranges;
          metadata.template_ranges = cachedTlv->template_ranges;
          metadata.init_callback_ranges = cachedTlv->init_callback_ranges;
          metadata.template_address = cachedTlv->template_address;
          metadata.template_size = cachedTlv->template_size;
          metadata.template_alignment = cachedTlv->template_alignment;
          metadata.all_zero_fill = cachedTlv->all_zero_fill;
          metadata.from_shared_cache = true;
          metadata.thunk_encoding_verified = false;
          for (const auto& range : metadata.thunk_ranges) {
            if (range.second % 24 != 0 || !rangeBelongsToImage(range))
              fail("Invalid cached thread-local thunk range for " + name);
          }
          for (const auto& range : metadata.template_ranges) {
            if (!rangeBelongsToImage(range))
              fail("Invalid cached thread-local template range for " + name);
          }
          for (const auto& range : metadata.init_callback_ranges) {
            if (range.second % 8 != 0 || !rangeBelongsToImage(range))
              fail("Invalid cached thread-local callback range for " + name);
          }
          if (!metadata.thunk_ranges.empty() && metadata.template_ranges.empty())
            fail("Cached thread-local thunks have no template ranges for " + name);
          if (metadata.template_size > std::numeric_limits<std::uint64_t>::max() -
                                           metadata.template_address)
            fail("Invalid cached thread-local template span for " + name);
          thread_local_images.emplace(static_cast<std::uint64_t>(*header), std::move(metadata));
        }
      }
    }
    for (const auto& exp : demand_libsystem_exports) {
      if (cached.count(exp.second.first) != 0)
        cache_exports["/usr/lib/libSystem.B.dylib"][exp.first] = exp.second.second;
    }
    if (cache_reader) {
      const std::unordered_set<std::string> guest_import_set(guest_memory_imports.begin(), guest_memory_imports.end());
      const auto host = cache_reader->hostExportAddresses(host_service_ids, guest_import_set);
      for (const auto& item : host.functions) imports.emplace(item.first, item.second);
      for (const auto& item : host.data) {
        modeledDataExportNames.insert(item.second);
      }
      for (const auto& item : host.data) if (item.second == "_mach_task_self_") {
        cached_task_port = cache_reader->guestAddress(item.first);
        break;
      }
      std::unordered_map<std::uint64_t, std::string> target_names;
      for (const auto& item : host.data) target_names.emplace(item.first, item.second);
      for (const auto& item : cache_reader->mergedDataBindings(target_names)) {
        const auto address = cache_reader->guestAddress(item.rebase.location);
        auto region = std::find_if(cache_regions.begin(), cache_regions.end(), [address](const auto& r) {
          return r.logical_start <= address && address < r.logical_end;
        });
        if (demand_cache && std::any_of(deferred_cache.begin(), deferred_cache.end(), [address](const auto& segment) {
              return segment.logical_start <= address && address + 8 <= segment.logical_end;
            })) {
          deferred_bindings[static_cast<std::size_t>(address)] = item.name;
        } else if (region != cache_regions.end()) {
          if (address + 8 > region->logical_end) fail("Cached global binding crosses a segment boundary");
          bindings[region->backing + static_cast<std::size_t>(address - region->logical_start)] = item.name;
        } else {
          const auto backing = (memory.size() + 7U) & ~std::size_t(7U);
          if (backing > 64 * 1024 * 1024 - 8) fail("Cached global bindings exceed loader limits");
          memory.resize(backing + 8, 0);
          cache_regions.push_back({address, address + 8, backing, false, true});
          bindings[backing] = item.name;
        }
      }
    }
  }
  struct Visit { std::size_t image; std::string symbol; bool operator<(const Visit& o) const { return std::tie(image, symbol) < std::tie(o.image, o.symbol); } };
  std::function<std::optional<std::size_t>(const std::string&, std::size_t, std::set<Visit>&)> resolve;
  resolve = [&](const std::string& symbol, std::size_t owner, std::set<Visit>& seen) -> std::optional<std::size_t> {
    if (!seen.insert({owner, symbol}).second) return {};
    const auto& item = loaded[owner];
    auto exp = item.image.exports.find(symbol);
    if (exp != item.image.exports.end()) {
      if (exp->second.kind == ExportKind::address) return item.base + exp->second.address;
      const auto ordinal = exp->second.ordinal;
      if (ordinal > 0 && ordinal <= item.deps.size() && item.deps[ordinal - 1])
        return resolve(exp->second.imported_name.empty() ? symbol : exp->second.imported_name,
                       *item.deps[ordinal - 1], seen);
      return {};
    }
    for (std::size_t i = 0; i < item.image.dependencies.size(); ++i) {
      if (item.image.dependencies[i].reexported && item.deps[i]) {
        if (auto target = resolve(symbol, *item.deps[i], seen)) return target;
      }
    }
    return {};
  };
  std::unordered_map<std::string, std::int64_t> synthetic;
  for (std::size_t i = 0; i < loaded.size(); ++i) {
    const auto& item = loaded[i];
    const auto resolve_import = [&](const ChainedImport& ref) -> std::pair<std::optional<std::size_t>, std::string> {
      std::set<Visit> seen;
      if (ref.ordinal == 0) return {resolve(ref.name, i, seen), "self:" + ref.name};
      if (ref.ordinal < 0 || static_cast<std::size_t>(ref.ordinal) > item.deps.size()) return {{}, "ordinal " + std::to_string(ref.ordinal) + ":" + ref.name};
      const auto dep_index = static_cast<std::size_t>(ref.ordinal - 1);
      const auto lib = item.image.dependencies[dep_index].name;
      if (item.deps[dep_index]) return {resolve(ref.name, *item.deps[dep_index], seen), lib + ":" + ref.name};
      auto exp = cache_exports[lib].find(ref.name);
      if (exp != cache_exports[lib].end()) {
        const auto resolved_name = lib == "/usr/lib/libSystem.B.dylib" || ref.ordinal == -2
            ? ref.name : lib + ":" + ref.name;
        return {static_cast<std::size_t>(exp->second), resolved_name};
      }
      return {{}, (lib == "/usr/lib/libSystem.B.dylib" || ref.ordinal == -2)
                       ? ref.name : lib + ":" + ref.name};
    };
    for (const auto& stub : item.image.stubs) {
      const auto addr = static_cast<std::int64_t>(item.base + stub.first);
      auto [target, name] = resolve_import(stub.second);
      if (target) guest_stubs.emplace(addr, *target); else imports.emplace(addr, std::move(name));
    }
    for (const auto& binding : item.image.bindings) {
      const auto address = item.base + binding.first;
      auto [target, name] = resolve_import(binding.second);
      bindings[address] = name;
      if (target) function_bindings[address] = static_cast<std::int64_t>(*target);
      else if (!binding.second.weak || host_service_ids.count(name) || guest_memory_imports.count(name)) {
        auto it = synthetic.find(name);
        const auto stub = it == synthetic.end()
            ? static_cast<std::int64_t>(0x80000000ULL + 4 * synthetic.size()) : it->second;
        synthetic[name] = stub;
        imports[stub] = name;
        function_bindings[address] = stub;
      }
    }
  }
  if (cache_reader && !modeledDataExportNames.empty()) {
    std::set<std::string> referencedModeledSymbols;
    const auto considerBinding = [&referencedModeledSymbols, &modeledDataExportNames](
        const std::string& binding) {
      if (modeledDataExportNames.find(binding) != modeledDataExportNames.end())
        referencedModeledSymbols.insert(binding);
    };
    for (const auto& binding : bindings) considerBinding(binding.second);
    for (const auto& binding : deferred_bindings) considerBinding(binding.second);
    for (const auto& symbol : referencedModeledSymbols) {
      std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> providers;
      for (const auto& [path, exports] : cache_exports) {
        const auto cachedHeader = cached_headers.find(path);
        const auto exported = exports.find(symbol);
        if (cachedHeader == cached_headers.end() || exported == exports.end() ||
            exported->second < 0) continue;
        const auto address = static_cast<std::uint64_t>(exported->second);
        const auto owner = std::find_if(
            mapped_image_segments.begin(), mapped_image_segments.end(),
            [header = static_cast<std::uint64_t>(cachedHeader->second), address](
                const MappedImageSegment& segment) {
              return segment.guest_header == header && segment.data_segment &&
                     !segment.executable && segment.start >= 0 &&
                     address >= static_cast<std::uint64_t>(segment.start) &&
                     address < static_cast<std::uint64_t>(segment.end);
            });
        if (owner == mapped_image_segments.end()) continue;
        const auto canonical = image_paths.find(owner->guest_header);
        if (canonical == image_paths.end()) continue;
        providers.emplace(std::make_pair(owner->guest_header, address), canonical->second);
      }
      if (providers.size() != 1) continue;
      const auto& provider = *providers.begin();
      modeled_global_export_provenance.emplace(
          symbol, ModeledGlobalExportProvenance{provider.first.first,
                                                 provider.first.second,
                                                 provider.second});
    }
  }
  for (auto index : init_order) for (auto offset : loaded[index].image.initializers) initializers.push_back(loaded[index].base + offset);
  imports[objc_mutation_invoke] = "__guest_objc_make_image_mutable";
  if (const auto objc = cache_exports["/usr/lib/libobjc.A.dylib"].find("__objc_init"); objc != cache_exports["/usr/lib/libobjc.A.dylib"].end())
    objc_bootstrap = objc->second;
  if (const auto dispatch = cache_exports["/usr/lib/system/libdispatch.dylib"].find("__dispatch_main_q"); dispatch != cache_exports["/usr/lib/system/libdispatch.dylib"].end())
    cached_dispatch_main_queue = dispatch->second;

  if (collect_startup_initializer_metadata) {
    startup_initializer_metadata_collected = true;
    startup_initializer_upward_links_supported = true;
    constexpr std::size_t maximumStartupImages = 2048;
    constexpr const char* libSystemPath = "/usr/lib/libSystem.B.dylib";
    struct StartupEdge {
      std::uint64_t header;
      bool upward;
      bool delayed;
    };
    std::map<std::uint64_t, std::size_t> localByHeader;
    std::map<std::uint64_t, std::string> cachedByHeader;
    for (std::size_t index = 0; index < loaded.size(); ++index)
      localByHeader.emplace(loaded[index].base, index);
    for (const auto& [path, header] : cached_headers)
      cachedByHeader.emplace(static_cast<std::uint64_t>(header), path);
    const auto edges = [&](std::uint64_t header) {
      std::vector<StartupEdge> result;
      if (const auto local = localByHeader.find(header); local != localByHeader.end()) {
        const auto& item = loaded[local->second];
        for (std::size_t index = 0; index < item.image.dependencies.size(); ++index) {
          const auto& dependency = item.image.dependencies[index];
          if (index < item.deps.size() && item.deps[index]) {
            result.push_back({loaded[*item.deps[index]].base, dependency.upward,
                              dependency.lazy_load || dependency.delayed_init});
          } else if (const auto child = cached_headers.find(dependency.name);
                     child != cached_headers.end()) {
            result.push_back({static_cast<std::uint64_t>(child->second), dependency.upward,
                              dependency.lazy_load || dependency.delayed_init});
          }
        }
      } else if (const auto cachedPath = cachedByHeader.find(header);
                 cachedPath != cachedByHeader.end()) {
        const auto source = cached.find(cachedPath->second);
        if (source != cached.end()) {
          for (const auto& dependency : source->second.dependencies) {
            if (const auto child = cached_headers.find(dependency.name);
                child != cached_headers.end()) {
              result.push_back({static_cast<std::uint64_t>(child->second), dependency.upward,
                                dependency.delayed_init});
            }
          }
        }
      }
      return result;
    };
    // Delay is an image state, not a per-edge traversal exclusion: any eager
    // path activates the image, including an upward edge from an eager image.
    std::set<std::uint64_t> eager;
    std::function<void(std::uint64_t)> markEager = [&](std::uint64_t header) {
      if (!eager.insert(header).second) return;
      if (eager.size() > maximumStartupImages)
        fail("Guest startup initializer graph exceeds 2048 images");
      for (const auto& edge : edges(header))
        if (!edge.delayed) markEager(edge.header);
    };
    markEager(loaded.front().base);
    std::vector<StartupEdge> systemEdges;
    if (cache_reader) {
      // libSystem itself is intentionally absent from the guest image graph.
      // Read its linkage metadata without mapping or running its initializer.
      if (const auto system = cache_reader->guestImage(libSystemPath)) {
        for (const auto& dependency : system->dependencies) {
          const auto child = cached_headers.find(dependency.name);
          if (child == cached_headers.end()) continue;
          systemEdges.push_back({static_cast<std::uint64_t>(child->second),
                                 dependency.upward, dependency.delayed_init});
          if (!dependency.delayed_init) markEager(child->second);
        }
      }
    }
    std::set<std::uint64_t> visited;
    std::set<std::uint64_t> queuedUpward;
    std::vector<std::uint64_t> upward;
    std::function<void(std::uint64_t)> visit = [&](std::uint64_t header) {
      if (!visited.insert(header).second) return;
      if (visited.size() > maximumStartupImages)
        fail("Guest startup initializer graph exceeds 2048 images");
      for (const auto& edge : edges(header)) {
        if (edge.upward) {
          if (queuedUpward.insert(edge.header).second) upward.push_back(edge.header);
        } else {
          visit(edge.header);
        }
      }
      if (eager.find(header) == eager.end()) return;
      StartupInitializerImage record;
      record.header = header;
      if (const auto local = localByHeader.find(header); local != localByHeader.end()) {
        const auto& item = loaded[local->second];
        if (item.image.initializers.empty()) return;
        record.path = item.path;
        record.from_shared_cache = false;
        for (const auto offset : item.image.initializers)
          record.local_targets.push_back(item.base + offset);
      } else {
        const auto path = cachedByHeader.find(header);
        if (path == cachedByHeader.end() || path->second == libSystemPath) return;
        auto targets = cache_reader->initializers(path->second);
        if (targets.empty()) return;
        record.path = path->second;
        record.from_shared_cache = true;
        record.cached_targets = std::move(targets);
      }
      startup_initializer_images.push_back(std::move(record));
    };
    // Match dyld's first and second deferred-upward passes. It does not drain
    // arbitrarily deep upward chains as additional startup roots.
    const auto visitUpwards = [&]() {
      for (unsigned pass = 0; pass != 2; ++pass) {
        auto pendingUpward = std::move(upward);
        upward.clear();
        queuedUpward.clear();
        for (const auto header : pendingUpward) visit(header);
      }
      upward.clear();
      queuedUpward.clear();
    };
    // dyld completes libSystem initialization before other startup roots. Its
    // own initializer is modeled, but its dependency constructors still run.
    for (const auto& edge : systemEdges) {
      if (edge.upward) {
        if (queuedUpward.insert(edge.header).second) upward.push_back(edge.header);
      } else {
        visit(edge.header);
      }
    }
    visitUpwards();
    visit(loaded.front().base);
    visitUpwards();
  }

  std::vector<std::uint64_t> scopeQueue{static_cast<std::uint64_t>(loaded.front().base)};
  std::set<std::uint64_t> queuedHeaders{scopeQueue.front()};
  const auto enqueueHeader = [&](std::uint64_t header) {
    if (queuedHeaders.insert(header).second) scopeQueue.push_back(header);
  };
  for (std::size_t cursor = 0; cursor < scopeQueue.size(); ++cursor) {
    const auto header = scopeQueue[cursor];
    const auto pathEntry = image_paths.find(header);
    if (pathEntry == image_paths.end()) continue;
    global_scope_headers.push_back(header);
    const auto local = std::find_if(loaded.begin(), loaded.end(), [header](const auto& item) {
      return item.base == header;
    });
    if (local != loaded.end()) {
      for (std::size_t index = 0; index < local->image.dependencies.size(); ++index) {
        if (index < local->deps.size() && local->deps[index]) {
          enqueueHeader(static_cast<std::uint64_t>(loaded[*local->deps[index]].base));
          continue;
        }
        const auto cachedHeader = cached_headers.find(local->image.dependencies[index].name);
        if (cachedHeader != cached_headers.end()) {
          enqueueHeader(static_cast<std::uint64_t>(cachedHeader->second));
        }
      }
      continue;
    }
    const auto cachedImage = cached.find(pathEntry->second);
    if (cachedImage == cached.end()) continue;
    for (const auto& dependency : cachedImage->second.dependencies) {
      const auto cachedHeader = cached_headers.find(dependency.name);
      if (cachedHeader != cached_headers.end()) {
        enqueueHeader(static_cast<std::uint64_t>(cachedHeader->second));
      }
    }
  }
  std::unordered_map<std::uint64_t, const LoadedImageGraphRecord*> localGraphByHeader;
  for (const auto& record : loaded_image_graph)
    localGraphByHeader.emplace(record.guest_header, &record);
  std::unordered_map<std::uint64_t, std::string> cachedPathByHeader;
  for (const auto& [path, header] : cached_headers)
    cachedPathByHeader.emplace(static_cast<std::uint64_t>(header), path);
  std::vector<std::uint64_t> eagerQueue{static_cast<std::uint64_t>(loaded.front().base)};
  startup_eager_headers.insert(eagerQueue.front());
  const auto enqueueEager = [this, &eagerQueue](std::uint64_t header) {
    if (startup_eager_headers.insert(header).second) eagerQueue.push_back(header);
  };
  if (cache_reader) {
    if (const auto system = cache_reader->guestImage("/usr/lib/libSystem.B.dylib")) {
      for (const auto& dependency : system->dependencies) {
        if (dependency.delayed_init) continue;
        if (const auto cachedHeader = cached_headers.find(dependency.name);
            cachedHeader != cached_headers.end())
          enqueueEager(static_cast<std::uint64_t>(cachedHeader->second));
      }
    }
  }
  for (std::size_t cursor = 0; cursor < eagerQueue.size(); ++cursor) {
    if (eagerQueue.size() > 2048) fail("Guest eager startup graph exceeds 2048 images");
    const auto header = eagerQueue[cursor];
    if (const auto local = localGraphByHeader.find(header); local != localGraphByHeader.end()) {
      for (const auto& dependency : local->second->dependencies) {
        if (dependency.declaration.delayed_init || dependency.declaration.lazy_load) continue;
        if (dependency.resolved_header) enqueueEager(*dependency.resolved_header);
        else if (const auto cachedHeader = cached_headers.find(dependency.declaration.name);
                 cachedHeader != cached_headers.end())
          enqueueEager(static_cast<std::uint64_t>(cachedHeader->second));
      }
      continue;
    }
    if (const auto path = cachedPathByHeader.find(header); path != cachedPathByHeader.end()) {
      const auto cachedImage = cached.find(path->second);
      if (cachedImage == cached.end()) continue;
      for (const auto& dependency : cachedImage->second.dependencies) {
        if (dependency.delayed_init) continue;
        if (const auto cachedHeader = cached_headers.find(dependency.name);
            cachedHeader != cached_headers.end())
          enqueueEager(static_cast<std::uint64_t>(cachedHeader->second));
      }
    }
  }
  std::sort(mapped_image_segments.begin(), mapped_image_segments.end(),
            [](const MappedImageSegment& left, const MappedImageSegment& right) {
    return std::tie(left.guest_header, left.start, left.end, left.segment_name, left.image_path) <
           std::tie(right.guest_header, right.start, right.end, right.segment_name, right.image_path);
  });
  mapped_image_segments.erase(
      std::unique(mapped_image_segments.begin(), mapped_image_segments.end(),
                  [](const MappedImageSegment& left, const MappedImageSegment& right) {
        return left.guest_header == right.guest_header && left.start == right.start &&
               left.end == right.end && left.segment_name == right.segment_name;
      }), mapped_image_segments.end());
  cached_export_addresses = std::move(cache_exports);
}

StagedNamedLoad ExecutableImage::stage_named_load(const std::string& requested_path,
                                                  std::uint64_t caller_header,
                                                  std::uint64_t logical_placement,
                                                  bool now) const {
  if (loaded_image_graph.empty()) fail("No startup image graph is available for named-load staging");
  const auto caller = std::find_if(loaded_image_graph.begin(), loaded_image_graph.end(),
                                   [caller_header](const auto& image) {
    return image.guest_header == caller_header;
  });
  if (caller == loaded_image_graph.end()) fail("Named-load caller is not in the startup image graph");
  const SharedCache* namedCache = cache_reader ? &*cache_reader
      : named_load_cache_reader_ ? &*named_load_cache_reader_ : nullptr;
  const std::filesystem::path executable_root(loaded_image_graph.front().canonical_path);
  const std::filesystem::path caller_path(caller->canonical_path);
  std::vector<std::filesystem::path> candidates;
  if (requested_path.rfind("@rpath/", 0) == 0) {
    const auto leaf = requested_path.substr(7);
    for (const auto& rpath : caller->rpaths)
      candidates.emplace_back(std::filesystem::path(rpath) / leaf);
  } else {
    candidates.emplace_back(expand_loader_path(requested_path, caller_path, executable_root));
  }
  std::optional<std::filesystem::path> requested;
  std::optional<std::string> requestedCachePath;
  for (const auto& candidate : candidates) {
    const auto candidateName = candidate.lexically_normal().string();
    if (candidateName.empty() || candidateName.front() == '@') continue;
    if (std::filesystem::exists(candidate)) {
      requested = std::filesystem::canonical(candidate);
      break;
    }
    if (namedCache && namedCache->images().find(candidateName) != namedCache->images().end()) {
      requestedCachePath = candidateName;
      break;
    }
  }
  if (!requested && !requestedCachePath)
    fail("Unable to resolve named guest image " + requested_path);

  StagedNamedLoad result;
  result.canonical_path = requested ? requested->string() : *requestedCachePath;
  if (requested) {
    const auto existing = std::find_if(loaded_image_graph.begin(), loaded_image_graph.end(),
                                       [&result](const auto& image) {
      return image.canonical_path == result.canonical_path;
    });
    if (existing != loaded_image_graph.end()) {
      result.requested_header = existing->guest_header;
      return result;
    }
  } else {
    const auto rootImage = namedCache->guestImage(*requestedCachePath);
    if (!rootImage) fail("Unable to parse named cached image " + *requestedCachePath);
    for (const auto& segment : rootImage->segments) {
      if (segment.name != "__TEXT") continue;
      const auto guest = namedCache->guestAddress(segment.address);
      if (guest < 0) fail("Named cached image header is outside guest space");
      result.requested_header = static_cast<std::uint64_t>(guest);
      break;
    }
    if (result.requested_header == 0)
      fail("Named cached image has no __TEXT header: " + *requestedCachePath);
  }

  constexpr std::uint64_t maximumImageGraphBytes = 64 * 1024 * 1024;
  constexpr std::size_t maximumStagedImages = 256;
  constexpr std::uint64_t imageAlignment = 16'384;
  const auto aligned = [](std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - (imageAlignment - 1))
      fail("Named guest image placement overflows");
    return (value + imageAlignment - 1) & ~(imageAlignment - 1);
  };
  std::uint64_t nextPlacement = aligned(logical_placement);
  if (nextPlacement > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    fail("Named guest image placement exceeds the guest address range");
  std::uint64_t stagedBytes = 0;
  std::vector<StagedNamedImage> discovered;
  std::map<std::string, std::size_t> stagedByPath;
  std::map<std::string, std::uint64_t> loadedByPath;
  for (const auto& image : loaded_image_graph) loadedByPath.emplace(image.canonical_path, image.guest_header);
  std::function<std::size_t(const std::filesystem::path&, std::vector<std::string>, bool)> stage_path;
  stage_path = [&](const std::filesystem::path& requestedImage,
                   std::vector<std::string> inheritedRpaths,
                   bool delayedInitialization) -> std::size_t {
    const auto canonical = std::filesystem::canonical(requestedImage);
    const auto path = canonical.string();
    if (const auto alreadyLoaded = loadedByPath.find(path); alreadyLoaded != loadedByPath.end())
      fail("Internal named-load staging attempted to stage an existing image");
    if (const auto known = stagedByPath.find(path); known != stagedByPath.end()) {
      if (!delayedInitialization) discovered[known->second].delayed_initialization = false;
      return known->second;
    }
    if (discovered.size() >= maximumStagedImages) fail("Named guest image graph exceeds 256 images");
    const auto base = static_cast<std::size_t>(nextPlacement);
    MachImage mach(canonical, false, base);
    if (nextPlacement < mach.preferred_base)
      fail("Named guest image placement precedes its preferred base");
    if (mach.memory.size() > maximumImageGraphBytes - stagedBytes ||
        mach.memory.size() > std::numeric_limits<std::uint64_t>::max() - nextPlacement)
      fail("Named guest image graph exceeds 64 MiB");
    const auto imageEnd = nextPlacement + mach.memory.size();
    if (imageEnd > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      fail("Named guest image placement exceeds the guest address range");
    for (const auto& segment : mapped_image_segments) {
      if (segment.start < 0 || segment.end < segment.start) continue;
      const auto start = static_cast<std::uint64_t>(segment.start);
      const auto end = static_cast<std::uint64_t>(segment.end);
      if (start < imageEnd && nextPlacement < end)
        fail("Named guest image placement overlaps an existing mapped image");
    }
    for (const auto& image : discovered) {
      const auto otherStart = image.guest_header;
      if (image.image.memory.size() > std::numeric_limits<std::uint64_t>::max() - otherStart)
        fail("Staged guest image range overflows");
      const auto otherEnd = otherStart + image.image.memory.size();
      if (otherStart < imageEnd && nextPlacement < otherEnd)
        fail("Named guest image placement overlaps another staged image");
    }
    stagedBytes += mach.memory.size();
    nextPlacement = aligned(imageEnd);
    std::vector<std::string> rpaths;
    for (const auto& rpath : mach.rpaths)
      rpaths.push_back(expand_loader_path(rpath, canonical, executable_root));
    rpaths.insert(rpaths.end(), inheritedRpaths.begin(), inheritedRpaths.end());
    StagedNamedImage staged(std::move(mach), static_cast<std::uint64_t>(base), imageEnd,
                            path, rpaths, delayedInitialization);
    const auto index = discovered.size();
    stagedByPath.emplace(path, index);
    discovered.push_back(std::move(staged));

    const auto declarations = discovered[index].image.dependencies;
    std::vector<StagedNamedDependency> resolvedDependencies;
    resolvedDependencies.reserve(declarations.size());
    for (const auto& dependency : declarations) {
      if (dependency.upward || dependency.lazy_load)
        fail("Upward or lazy-load dependency ordering is unsupported for named-load staging");
      StagedNamedDependency resolved{dependency, {}, {}};
      const auto& name = dependency.name;
      if (name == "/usr/lib/libSystem.B.dylib" || name.rfind("/usr/lib/", 0) == 0 ||
          name.rfind("/System/Library/", 0) == 0) {
        resolvedDependencies.push_back(std::move(resolved));
        continue;
      }
      const auto target = resolve_local_dependency_path(name, canonical, executable_root, rpaths);
      if (!target) {
        if (!dependency.weak) fail("Unable to resolve guest dylib " + name);
        resolvedDependencies.push_back(std::move(resolved));
        continue;
      }
      const auto targetPath = target->string();
      if (const auto alreadyLoaded = loadedByPath.find(targetPath); alreadyLoaded != loadedByPath.end()) {
        resolved.existing_header = alreadyLoaded->second;
      } else {
        resolved.staged_image_index = stage_path(*target, rpaths,
                                                  dependency.delayed_init || dependency.lazy_load);
      }
      resolvedDependencies.push_back(std::move(resolved));
    }
    discovered[index].dependencies = std::move(resolvedDependencies);
    return index;
  };

  std::optional<std::size_t> requestedIndex;
  if (requested) requestedIndex = stage_path(*requested, caller->rpaths, false);
  std::unordered_map<std::uint64_t, std::size_t> stagedByHeader;
  for (std::size_t index = 0; index < discovered.size(); ++index)
    stagedByHeader.emplace(discovered[index].guest_header, index);
  using ResolutionVisit = std::pair<std::uint64_t, std::string>;
  std::function<std::optional<std::uint64_t>(const std::string&, std::uint64_t,
                                              std::set<ResolutionVisit>&)> resolve_header;
  std::function<std::optional<std::uint64_t>(const StagedNamedDependency&, const std::string&,
                                              std::set<ResolutionVisit>&)> resolve_staged_dependency;
  std::function<std::optional<std::uint64_t>(const LoadedImageDependency&, const std::string&,
                                              std::set<ResolutionVisit>&)> resolve_loaded_dependency;
  std::unordered_map<std::uint64_t, const LoadedImageGraphRecord*> loadedByHeader;
  for (const auto& image : loaded_image_graph) loadedByHeader.emplace(image.guest_header, &image);
  std::function<std::optional<std::uint64_t>(const std::string&, const std::string&,
                                              std::set<std::string>&)> resolve_cached;
  std::map<std::uint64_t, SharedCache::ResolvedExport> resolved_cache_owners;
  resolve_cached = [this, &resolve_cached, &resolved_cache_owners, &namedCache](
                                            const std::string& path, const std::string& symbol,
                                            std::set<std::string>& seen)
      -> std::optional<std::uint64_t> {
    if (!seen.insert(path).second) return {};
    if (namedCache) {
      const auto resolved = namedCache->resolveExport(path, symbol);
      if (resolved && resolved->guest_address >= 0) {
        resolved_cache_owners.emplace(static_cast<std::uint64_t>(resolved->guest_address),
                                      *resolved);
        return static_cast<std::uint64_t>(resolved->guest_address);
      }
    }
    const auto image = cached_export_addresses.find(path);
    if (image != cached_export_addresses.end()) {
      const auto exported = image->second.find(symbol);
      if (exported != image->second.end() && exported->second >= 0)
        return static_cast<std::uint64_t>(exported->second);
    }
    const auto dependencies = cached_image_dependencies.find(path);
    if (dependencies == cached_image_dependencies.end()) return {};
    for (const auto& dependency : dependencies->second) {
      if (!dependency.reexported) continue;
      if (auto target = resolve_cached(dependency.name, symbol, seen)) return target;
    }
    return {};
  };
  const auto cached_target = [&resolve_cached](const std::string& path, const std::string& symbol)
      -> std::optional<std::uint64_t> {
    std::set<std::string> seen;
    return resolve_cached(path, symbol, seen);
  };
  resolve_staged_dependency = [&](const StagedNamedDependency& dependency,
                                  const std::string& symbol,
                                  std::set<ResolutionVisit>& seen)
      -> std::optional<std::uint64_t> {
    if (dependency.staged_image_index && *dependency.staged_image_index < discovered.size())
      return resolve_header(symbol, discovered[*dependency.staged_image_index].guest_header, seen);
    if (dependency.existing_header)
      return resolve_header(symbol, *dependency.existing_header, seen);
    return cached_target(dependency.declaration.name, symbol);
  };
  resolve_loaded_dependency = [&](const LoadedImageDependency& dependency,
                                  const std::string& symbol,
                                  std::set<ResolutionVisit>& seen)
      -> std::optional<std::uint64_t> {
    if (dependency.resolved_header)
      return resolve_header(symbol, *dependency.resolved_header, seen);
    return cached_target(dependency.declaration.name, symbol);
  };
  resolve_header = [&](const std::string& symbol, std::uint64_t header,
                       std::set<ResolutionVisit>& seen) -> std::optional<std::uint64_t> {
    if (!seen.emplace(header, symbol).second) return {};
    if (const auto staged = stagedByHeader.find(header); staged != stagedByHeader.end()) {
      const auto& image = discovered[staged->second];
      const auto exported = image.image.exports.find(symbol);
      if (exported != image.image.exports.end()) {
        if (exported->second.kind == ExportKind::address) {
          if (exported->second.address > std::numeric_limits<std::uint64_t>::max() - header)
            fail("Staged export address overflows");
          return header + exported->second.address;
        }
        const auto ordinal = exported->second.ordinal;
        if (ordinal > 0 && ordinal <= image.dependencies.size()) {
          const auto& dependency = image.dependencies[static_cast<std::size_t>(ordinal - 1)];
          if (auto target = resolve_staged_dependency(
                  dependency, exported->second.imported_name.empty() ? symbol
                                                                      : exported->second.imported_name,
                  seen)) return target;
        }
      }
      for (const auto& dependency : image.dependencies) {
        if (!dependency.declaration.reexported) continue;
        if (auto target = resolve_staged_dependency(dependency, symbol, seen)) return target;
      }
      return {};
    }
    const auto loadedImage = loadedByHeader.find(header);
    if (loadedImage != loadedByHeader.end()) {
      const auto& record = *loadedImage->second;
      const auto exported = record.exports.find(symbol);
      if (exported != record.exports.end()) {
        if (exported->second.kind == ExportKind::address) {
          if (exported->second.address > std::numeric_limits<std::uint64_t>::max() - header)
            fail("Loaded export address overflows");
          return header + exported->second.address;
        }
        const auto ordinal = exported->second.ordinal;
        if (ordinal > 0 && ordinal <= record.dependencies.size()) {
          const auto& dependency = record.dependencies[static_cast<std::size_t>(ordinal - 1)];
          if (auto target = resolve_loaded_dependency(
                  dependency, exported->second.imported_name.empty() ? symbol
                                                                      : exported->second.imported_name,
                  seen)) return target;
        }
      }
      for (const auto& dependency : record.dependencies) {
        if (!dependency.declaration.reexported) continue;
        if (auto target = resolve_loaded_dependency(dependency, symbol, seen)) return target;
      }
      return {};
    }
    const auto path = image_paths.find(header);
    if (path != image_paths.end()) return cached_target(path->second, symbol);
    return {};
  };
  const auto resolve_import = [&](const StagedNamedImage& image,
                                  const ChainedImport& imported) {
    std::optional<std::uint64_t> providerHeader;
    std::optional<std::uint64_t> target;
    bool systemDependency = false;
    std::set<ResolutionVisit> seen;
    if (imported.ordinal == 0) {
      providerHeader = image.guest_header;
      target = resolve_header(imported.name, image.guest_header, seen);
    } else if (imported.ordinal > 0) {
      if (static_cast<std::size_t>(imported.ordinal) > image.dependencies.size())
        fail("Staged import dependency ordinal is out of range");
      const auto& dependency = image.dependencies[static_cast<std::size_t>(imported.ordinal - 1)];
      systemDependency = dependency.declaration.name == "/usr/lib/libSystem.B.dylib";
      providerHeader = dependency.staged_image_index
          ? std::optional<std::uint64_t>(discovered[*dependency.staged_image_index].guest_header)
          : dependency.existing_header;
      target = resolve_staged_dependency(dependency, imported.name, seen);
    } else if (imported.ordinal == -1) {
      if (!loaded_image_graph.empty()) {
        providerHeader = loaded_image_graph.front().guest_header;
        target = resolve_header(imported.name, *providerHeader, seen);
      }
    } else if (imported.ordinal == -2 || imported.ordinal == -3) {
      for (const auto header : global_scope_headers) {
        if (auto value = resolve_header(imported.name, header, seen)) {
          target = value;
          providerHeader = header;
          break;
        }
      }
      if (!target) for (const auto& candidate : discovered) {
        if (auto value = resolve_header(imported.name, candidate.guest_header, seen)) {
          target = value;
          providerHeader = candidate.guest_header;
          break;
        }
      }
    } else {
      fail("Unsupported staged import ordinal " + std::to_string(imported.ordinal));
    }
    std::string importName = imported.name;
    if (!target && imported.ordinal > 0) {
      const auto& dependency = image.dependencies[static_cast<std::size_t>(imported.ordinal - 1)];
      if (!systemDependency)
        importName = dependency.declaration.name + ":" + importName;
    }
    return std::make_tuple(target, providerHeader, importName, systemDependency);
  };
  std::map<std::string, std::uint8_t> cacheActivationState;
  std::map<std::string, std::size_t> cacheActivationIndex;
  std::function<void(const std::string&, bool)> stage_cache_image;
  stage_cache_image = [&](const std::string& path, bool delayedInitialization) {
    if (!namedCache || namedCache->images().find(path) == namedCache->images().end())
      fail("Resolved cached provider is absent from the shared cache: " + path);
    if (cacheActivationState[path] != 0) {
      if (!delayedInitialization) {
        const auto existing = cacheActivationIndex.find(path);
        if (existing != cacheActivationIndex.end())
          result.cache_images[existing->second].delayed_initialization = false;
      }
      return;
    }
    if (cacheActivationState.size() >= 1024)
      fail("Staged cached dependency graph exceeds 1024 images");
    cacheActivationState[path] = 1;
    auto cachedImage = namedCache->guestImage(path);
    if (!cachedImage) fail("Unable to parse staged cached image " + path);
    for (const auto& dependency : cachedImage->dependencies) {
      if (namedCache->images().find(dependency.name) != namedCache->images().end())
        stage_cache_image(dependency.name, dependency.delayed_init);
    }
    std::optional<std::uint64_t> header;
    std::optional<std::uint64_t> imageSlide;
    for (const auto& segment : cachedImage->segments) {
      if (segment.name != "__TEXT") continue;
      const auto guest = namedCache->guestAddress(segment.address);
      if (guest < 0) fail("Staged cached image header is outside guest space");
      header = static_cast<std::uint64_t>(guest);
      if (*header >= segment.address) {
        const auto difference = *header - segment.address;
        if (difference > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
          fail("Staged cached image slide exceeds signed range");
        imageSlide = static_cast<std::int64_t>(difference);
      } else {
        const auto difference = segment.address - *header;
        if (difference > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
          fail("Staged cached image slide exceeds signed range");
        imageSlide = -static_cast<std::int64_t>(difference);
      }
      break;
    }
    if (!header) fail("Staged cached image has no __TEXT header: " + path);
    const bool alreadyMapped = std::any_of(
        mapped_image_segments.begin(), mapped_image_segments.end(),
        [header](const auto& segment) { return segment.guest_header == *header; });
    StagedCacheImage staged;
    staged.install_name = path;
    staged.guest_header = *header;
    staged.slide = *imageSlide;
    staged.image = std::move(*cachedImage);
    for (const auto& segment : staged.image.segments) {
      const auto logicalStart = namedCache->guestAddress(segment.address);
      if (logicalStart < 0 ||
          segment.size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
          logicalStart > std::numeric_limits<std::int64_t>::max() -
                             static_cast<std::int64_t>(segment.size))
        fail("Staged cached segment exceeds guest address range");
      const auto logicalEnd = logicalStart + static_cast<std::int64_t>(segment.size);
      staged.segments.push_back({logicalStart, logicalEnd, segment});
      if (segment.size > std::numeric_limits<std::uint64_t>::max() - staged.segment_bytes ||
          (!alreadyMapped && segment.size > std::numeric_limits<std::uint64_t>::max() -
                               result.total_cache_segment_bytes))
        fail("Staged cached segment byte count overflows");
      staged.segment_bytes += segment.size;
      if (!alreadyMapped) result.total_cache_segment_bytes += segment.size;
    }
    const auto initializers = namedCache->initializers(path);
    staged.initializers = initializers;
    staged.initializer_targets.reserve(initializers.size());
    for (const auto& initializer : initializers)
      staged.initializer_targets.push_back(initializer.target_address);
    staged.already_mapped = alreadyMapped;
    staged.startup_active = startup_eager_headers.count(*header) != 0;
    staged.delayed_initialization = delayedInitialization;
    cacheActivationIndex.emplace(path, result.cache_images.size());
    result.cache_images.push_back(std::move(staged));
    cacheActivationState[path] = 2;
  };
  if (requestedCachePath) stage_cache_image(*requestedCachePath, false);
  for (auto& image : discovered) {
    for (const auto offset : image.image.initializers) {
      if (offset > std::numeric_limits<std::uint64_t>::max() - image.guest_header)
        fail("Staged initializer target overflows");
      image.initializer_targets.push_back(image.guest_header + offset);
    }
    const auto append_binds = [&](const auto& imports, bool isStub) {
      for (const auto& entry : imports) {
        if (entry.first > std::numeric_limits<std::uint64_t>::max() - image.guest_header)
          fail("Staged binding slot overflows");
        StagedNamedBind binding(image.guest_header + entry.first, entry.second, isStub);
        binding.lazy = image.image.lazy_bind_locations.count(entry.first) != 0;
        if (const auto auth = image.image.authentication.find(entry.first);
            auth != image.image.authentication.end()) binding.authentication = auth->second;
        auto [resolvedTarget, providerHeader, importedName, systemDependency] =
            resolve_import(image, entry.second);
        binding.dependency_header = providerHeader;
        binding.resolved_name = importedName;
        const auto service = systemDependency ? host_service_ids_.find(entry.second.name)
                                              : host_service_ids_.end();
        const bool guestMemory = systemDependency &&
            guest_memory_imports_.count(entry.second.name) != 0;
        if (service != host_service_ids_.end()) {
          binding.host_service_id = service->second;
          binding.resolved_name = service->first;
        } else if (guestMemory) {
          binding.guest_memory_import = true;
          binding.resolved_name = entry.second.name;
        } else if (resolvedTarget) {
          const auto cacheOwner = resolved_cache_owners.find(*resolvedTarget);
          if (cacheOwner != resolved_cache_owners.end()) {
            const auto header = static_cast<std::uint64_t>(cacheOwner->second.guest_header);
            const bool mapped = std::any_of(
                mapped_image_segments.begin(), mapped_image_segments.end(),
                [header](const auto& segment) { return segment.guest_header == header; });
            if (!mapped) {
              binding.deferred_target = *resolvedTarget;
              binding.required_cache_image = cacheOwner->second.install_name;
              binding.dependency_header = header;
              stage_cache_image(cacheOwner->second.install_name, false);
            } else {
              binding.resolved_target = *resolvedTarget;
              binding.dependency_header = header;
            }
          } else {
            binding.resolved_target = *resolvedTarget;
          }
        }
        if (!binding.resolved_target && !binding.deferred_target &&
            !binding.host_service_id && !binding.guest_memory_import) {
          auto hostService = host_service_ids_.find(binding.resolved_name);
          if (hostService != host_service_ids_.end()) {
            binding.host_service_id = hostService->second;
            binding.resolved_name = hostService->first;
          } else if (guest_memory_imports_.count(binding.resolved_name) != 0) {
            binding.guest_memory_import = true;
          } else if (entry.second.weak) {
            binding.weak_null = true;
            binding.resolved_target = 0;
          } else if (now &&
                     (entry.second.name == "dyld_stub_binder" ||
                      entry.second.name == "_dyld_stub_binder") &&
                     !image.image.lazy_bind_locations.empty()) {
            binding.ignored_eager_lazy_binder = true;
          } else if (now) {
            fail("Unresolved strong guest import " + binding.resolved_name);
          } else {
            binding.unresolved = true;
          }
        }
        image.binds.push_back(std::move(binding));
      }
    };
    append_binds(image.image.stubs, true);
    append_binds(image.image.bindings, false);
  }

  std::vector<std::size_t> order;
  std::vector<std::uint8_t> state(discovered.size(), 0);
  std::function<void(std::size_t)> visit = [&](std::size_t index) {
    if (state[index] != 0) return;
    state[index] = 1;
    for (const auto& dependency : discovered[index].dependencies)
      if (dependency.staged_image_index && state[*dependency.staged_image_index] == 0)
        visit(*dependency.staged_image_index);
    state[index] = 2;
    order.push_back(index);
  };
  if (requestedIndex) visit(*requestedIndex);
  std::vector<std::size_t> remap(discovered.size());
  result.images.reserve(order.size());
  for (std::size_t index = 0; index < order.size(); ++index) {
    remap[order[index]] = index;
    result.images.push_back(std::move(discovered[order[index]]));
    if (result.images.back().image_bytes >
        std::numeric_limits<std::size_t>::max() - result.total_image_bytes)
      fail("Staged named-load backing size overflows");
    result.total_image_bytes += result.images.back().image_bytes;
  }
  for (auto& image : result.images) for (auto& dependency : image.dependencies)
    if (dependency.staged_image_index) dependency.staged_image_index = remap[*dependency.staged_image_index];
  if (requestedIndex)
    result.requested_header = result.images[remap[*requestedIndex]].guest_header;
  return result;
}

bool ExecutableImage::cache_enabled() const noexcept {
  return cache_reader.has_value() || named_load_cache_reader_.has_value() ||
         !cache_regions.empty() || !deferred_cache.empty();
}
void ExecutableImage::activate_named_cache_reader() {
  if (cache_reader || !named_load_cache_reader_) return;
  cache_reader.emplace(std::move(*named_load_cache_reader_));
  named_load_cache_reader_.reset();
}
std::size_t ExecutableImage::cache_backing_start() const noexcept { return (memory.size() + 15U) & ~std::size_t(15U); }
std::size_t ExecutableImage::cache_backing_end() const noexcept {
  const bool hasCache = cache_reader.has_value() || named_load_cache_reader_.has_value() ||
                        !deferred_cache.empty();
  return cache_backing_start() + (hasCache ? 64 * 1024 * 1024 : 0);
}
std::size_t ExecutableImage::mapping_backing_end() const noexcept { return cache_backing_end() + 16 * 1024 * 1024; }

std::uint32_t ExecutableImage::link_time_version(const std::string& name) const noexcept {
  for (auto iter = linked_libraries_.rbegin(); iter != linked_libraries_.rend(); ++iter) {
    const auto slash = iter->name.find_last_of('/');
    const auto leaf = iter->name.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto prefix = "lib" + name + ".";
    if (leaf == name || (leaf.rfind(prefix, 0) == 0 && leaf.size() > prefix.size() &&
                         leaf.compare(leaf.size() - 6, 6, ".dylib") == 0)) {
      return iter->current_version;
    }
  }
  return std::numeric_limits<std::uint32_t>::max();
}

bool ExecutableImage::is_code(std::int64_t address) const noexcept {
  if (address < 0 || (address & 3) != 0) return false;
  return std::any_of(text_regions.begin(), text_regions.end(), [address](const auto& range) {
    return range.first <= address && address <= range.second - 4;
  });
}

std::optional<bool> ExecutableImage::is_arm64e_code(std::int64_t pc) const noexcept {
  if (!is_code(pc)) return std::nullopt;
  bool found = false;
  std::optional<bool> result;
  for (const auto& region : code_abi_regions) {
    if (region.start > pc || pc > region.end - 4) continue;
    if (!region.arm64e) return std::nullopt;
    if (found && result != region.arm64e) return std::nullopt;
    found = true;
    result = region.arm64e;
  }
  return found ? result : std::nullopt;
}

std::optional<std::pair<std::int64_t, std::uint64_t>> ExecutableImage::lookup_guest_section(
    std::int64_t header, std::size_t kind) const {
  static const std::array<const char*, 21> names = {
      "__swift5_protos", "__swift5_proto", "__swift5_types", "__swift5_replace",
      "__swift5_replac2", "__swift5_acfuncs", "__objc_imageinfo", "__objc_selrefs",
      "__objc_msgrefs", "__objc_classrefs", "__objc_superrefs", "__objc_protorefs",
      "__objc_classlist", "__objc_nlclslist", "__objc_stublist", "__objc_catlist",
      "__objc_catlist2", "__objc_nlcatlist", "__objc_protolist", "__objc_fork_ok",
      "__objc_rawisa"};
  if (kind >= names.size()) throw std::out_of_range("Invalid dyld section kind");
  if (header < 0) throw std::out_of_range("Negative guest Mach-O header");
  const auto read_guest = [this](std::uint64_t address, std::size_t size) {
    for (const auto& image : runtime_local_images) {
      if (address < image.guest_header) continue;
      const auto offset = address - image.guest_header;
      if (offset > image.bytes.size() || size > image.bytes.size() - offset) continue;
      return std::vector<std::uint8_t>(
          image.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
          image.bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
    }
    if (cache_reader && address >= static_cast<std::uint64_t>(SharedCache::guestBase)) {
      const auto original = cache_reader->originalAddress(static_cast<std::int64_t>(address));
      return cache_reader->bytes(original, size);
    }
    for (const auto& region : cache_regions) {
      if (region.logical_start < 0 || address < static_cast<std::uint64_t>(region.logical_start) ||
          address - static_cast<std::uint64_t>(region.logical_start) >
              static_cast<std::uint64_t>(region.logical_end - region.logical_start)) continue;
      const auto delta = static_cast<std::size_t>(address - static_cast<std::uint64_t>(region.logical_start));
      if (size > static_cast<std::uint64_t>(region.logical_end - region.logical_start) - delta) continue;
      const auto offset = region.backing + delta;
      if (offset <= memory.size() && size <= memory.size() - offset) {
        return std::vector<std::uint8_t>(memory.begin() + static_cast<std::ptrdiff_t>(offset),
                                         memory.begin() + static_cast<std::ptrdiff_t>(offset + size));
      }
    }
    if (address > memory.size() || size > memory.size() - static_cast<std::size_t>(address))
      throw std::out_of_range("Guest Mach-O command bytes are unmapped");
    return std::vector<std::uint8_t>(memory.begin() + static_cast<std::ptrdiff_t>(address),
                                     memory.begin() + static_cast<std::ptrdiff_t>(address + size));
  };
  const auto mach_header = read_guest(static_cast<std::uint64_t>(header), 32);
  const auto command_count = static_cast<std::size_t>(read_integer(mach_header, 16, 4));
  const auto command_size = static_cast<std::size_t>(read_integer(mach_header, 20, 4));
  if (command_count > command_size / 8 || command_size > 16 * 1024 * 1024 ||
      static_cast<std::uint64_t>(header) > std::numeric_limits<std::uint64_t>::max() - 32 - command_size)
    throw std::runtime_error("Invalid guest Mach-O command table");
  std::uint64_t cursor = static_cast<std::uint64_t>(header) + 32;
  const std::uint64_t end = cursor + command_size;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> section_result;
  for (std::size_t index = 0; index < command_count; ++index) {
    if (cursor > end || end - cursor < 8) throw std::runtime_error("Truncated guest Mach-O command");
    const auto command_header = read_guest(cursor, 8);
    const auto command = read_integer(command_header, 0, 4);
    const auto length = static_cast<std::size_t>(read_integer(command_header, 4, 4));
    if (length < 8 || length > end - cursor) throw std::runtime_error("Invalid guest Mach-O command");
    if (command == 0x19 && length >= 72) {
      const auto bytes = read_guest(cursor, length);
      const auto segment = fixed_name(bytes, 8, 16);
      if ((kind < 6 && segment == "__TEXT") ||
          (kind >= 6 && segment.rfind("__DATA", 0) == 0)) {
        const auto section_count = static_cast<std::size_t>(read_integer(bytes, 64, 4));
        if (section_count > (length - 72) / 80)
          throw std::runtime_error("Invalid guest Mach-O section table");
        for (std::size_t section = 0; section < section_count; ++section) {
          const auto offset = 72 + section * 80;
          if (fixed_name(bytes, offset, 16) == names[kind]) {
            section_result = {read_integer(bytes, offset + 32, 8),
                              read_integer(bytes, offset + 40, 8)};
          }
        }
      }
    }
    cursor += length;
  }
  if (!section_result) return std::nullopt;
  std::uint64_t address = section_result->first;
  const auto slide = image_slides.find(static_cast<std::uint64_t>(header));
  if (slide != image_slides.end()) address += slide->second;
  else if (cache_reader && static_cast<std::uint64_t>(header) >=
                               static_cast<std::uint64_t>(SharedCache::guestBase))
    address = static_cast<std::uint64_t>(cache_reader->guestAddress(address));
  else throw std::out_of_range("Guest Mach-O header has no image slide");
  if (address > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    throw std::out_of_range("Guest section address exceeds signed guest space");
  return std::make_pair(static_cast<std::int64_t>(address), section_result->second);
}

std::uint32_t ExecutableImage::instruction(std::int64_t address) const {
  for (const auto& image : runtime_local_images) {
    if (address < 0 || static_cast<std::uint64_t>(address) < image.guest_header) continue;
    const auto offset = static_cast<std::uint64_t>(address) - image.guest_header;
    if (offset > image.bytes.size() || 4 > image.bytes.size() - offset) continue;
    const auto executable = std::any_of(
        image.executable_ranges.begin(), image.executable_ranges.end(),
        [address](const auto& range) { return range.first <= static_cast<std::uint64_t>(address) &&
                                             static_cast<std::uint64_t>(address) + 4 <= range.second; });
    if (executable)
      return static_cast<std::uint32_t>(read_integer(image.bytes, static_cast<std::size_t>(offset), 4));
  }
  if (cache_reader && address >= SharedCache::guestBase && !deferred_cache.empty()) {
    const auto raw = cache_reader->bytes(cache_reader->originalAddress(address), 4);
    return static_cast<std::uint32_t>(read_integer(raw, 0, 4));
  }
  std::size_t backing = static_cast<std::size_t>(address);
  for (const auto& region : cache_regions) {
    if (region.logical_start <= address && address < region.logical_end) {
      backing = region.backing + static_cast<std::size_t>(address - region.logical_start);
      break;
    }
  }
  return static_cast<std::uint32_t>(read_integer(memory, backing, 4));
}

}  // namespace arm64metal
