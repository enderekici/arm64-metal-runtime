#ifndef ARM64METAL_MACHO_HPP
#define ARM64METAL_MACHO_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace arm64metal {

// Bounds-checked little- or big-endian integer access for Mach-O byte buffers.
std::uint64_t read_integer(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                          std::size_t width, bool big_endian = false);

struct SymbolRegion {
  std::size_t entry;
  std::size_t limit;
  std::size_t text_offset;
};

struct MachSymbol {
  std::string name;
  // Unslid VM address from n_value in the selected ARM64 slice.
  std::uint64_t address;
  // 1-based ordinal used by n_sect, plus the corresponding section identity.
  std::uint8_t section_index;
  // True for symbols sourced from LC_DYSYMTAB's external-defined range.
  bool external;
  std::string segment_name;
  std::string section_name;
};

// ARM64 Mach-O reader. Universal files select ARM64/ARM64e according to the flag;
// thin files are validated by the same policy. `symbol` returns offsets relative
// to the __text section and the selected function's inferred upper bound.
class MachO {
 public:
  explicit MachO(const std::filesystem::path& path, bool allow_arm64e = false);
  explicit MachO(std::vector<std::uint8_t> bytes, bool allow_arm64e = false);

  const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
  std::uint32_t word(std::size_t offset) const;
  SymbolRegion symbol(const std::string& name) const;
  // Return raw named N_SECT symbols from LC_DYSYMTAB's external-defined then
  // local ranges, including private static symbols. Results are stably sorted
  // by unslid n_value; equal-address external-range entries precede locals,
  // preserving nlist order within each range. A nearest-preceding lookup that
  // selects the last equal-address entry therefore selects the local symbol,
  // matching native dladdr on the colliding-symbol fixture. Addresses belong to
  // the selected slice, not guest VM. Table ranges and string indices validate.
  std::vector<MachSymbol> defined_section_symbols() const;

 private:
  std::vector<std::uint8_t> bytes_;
};

enum class ExportKind { address, reexport };

struct MachExport {
  ExportKind kind;
  std::uint64_t address = 0;
  std::uint64_t ordinal = 0;
  std::string imported_name;
};

// Parse a Mach-O export trie. Addresses are image-relative and validated against
// `memory_size`; re-exports retain their dependency ordinal and imported name.
std::map<std::string, MachExport> mach_exports(const std::vector<std::uint8_t>& trie,
                                               std::size_t memory_size);

struct ChainedImport {
  std::string name;
  std::int64_t ordinal;
  bool weak;
  std::int64_t addend;
};

struct ChainedAuthentication {
  std::uint64_t key;
  std::uint64_t modifier;
};

struct ChainedFixups {
  std::map<std::size_t, ChainedImport> bindings;
  std::map<std::size_t, ChainedAuthentication> authentication;
};

// Decode dyld chained fixups in-place. `placement` is the guest placement base;
// mapped ranges are offsets into `memory` and every written pointer must fit one.
ChainedFixups apply_chained_fixups(const std::vector<std::uint8_t>& payload,
                                   std::uint64_t image_base, std::size_t placement,
                                   std::vector<std::uint8_t>& memory,
                                   const std::vector<std::pair<std::size_t, std::size_t>>& mapped_ranges);

struct LegacyRebaseSegment {
  std::uint64_t vm_address;
  std::uint64_t vm_size;
};

struct LegacyRebase {
  // Byte offset of the pointer slot in the image backing.
  std::size_t location;
  // Relocated guest VM target to write into the slot.
  std::uint64_t target;
};

struct LegacyBind {
  // Byte offset of the pointer slot in the image backing.
  std::size_t location;
  ChainedImport imported;
  // Preserved separately because it is not the weak-import bit.
  bool non_weak_definition;
  // Lazy-stream records are independently decoded between DONE opcodes.
  bool lazy;
  std::size_t lazy_record;
};

struct LegacyWeakBind {
  // A null location records a strong-definition marker, which has no slot.
  std::optional<std::size_t> location;
  ChainedImport imported;
  bool non_weak_definition;
};

// Decode Apple's legacy dyld rebase opcode stream for 64-bit pointer rebases.
// Segment vector order must exactly match LC_SEGMENT_64 load-command order,
// including unmapped segment entries; mapped ranges are image-backing offsets.
// This planner is read-only and returns no operations if any opcode/slot fails
// validation. Legacy bind streams are decoded separately below.
std::vector<LegacyRebase> decode_legacy_rebase_opcodes(
    const std::vector<std::uint8_t>& payload,
    std::uint64_t image_base,
    std::uint64_t placement,
    const std::vector<LegacyRebaseSegment>& segments,
    const std::vector<std::uint8_t>& memory,
    const std::vector<std::pair<std::size_t, std::size_t>>& mapped_ranges);

// Decode a legacy dyld bind stream without invoking the host dyld binder.
// Segment order and mapped-range coordinates follow the rebase API above.
// For lazy streams, each DONE terminates a record and resets per-entry state;
// `lazy_record` records the zero-based record containing each slot. This models
// dyld's per-entry fast lazy binder; its sequential validation pass retains
// state across DONE, so each record here must be independently decodable.
// Regular streams may end at the payload boundary without explicit DONE.
std::vector<LegacyBind> decode_legacy_bind_opcodes(
    const std::vector<std::uint8_t>& payload,
    std::uint64_t image_base,
    const std::vector<LegacyRebaseSegment>& segments,
    const std::vector<std::uint8_t>& memory,
    const std::vector<std::pair<std::size_t, std::size_t>>& mapped_ranges,
    bool lazy_stream = false);

// Decode Apple's weak coalescing stream only. This records strong-definition
// markers and weak-lookup slot binds without resolving either on the host.
std::vector<LegacyWeakBind> decode_legacy_weak_bind_opcodes(
    const std::vector<std::uint8_t>& payload,
    std::uint64_t image_base,
    const std::vector<LegacyRebaseSegment>& segments,
    const std::vector<std::uint8_t>& memory,
    const std::vector<std::pair<std::size_t, std::size_t>>& mapped_ranges);

}  // namespace arm64metal

#endif  // ARM64METAL_MACHO_HPP
