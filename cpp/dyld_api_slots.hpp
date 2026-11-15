#pragma once

// Every dyld4::APIs vtable slot the cached libdyld can call, the extra slots the
// runtime models beyond the dyld_api_abi.hpp core, and a named diagnostic for a
// guest call through a slot that is still unmodeled (it reads a null pointer).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dyld_api_abi.hpp"
#include "image_loader.hpp"
#include "shared_cache.hpp"

namespace arm64metal {

// libdyld exports whose APIs method the runtime serves through a vtable slot.
// Exports named in host_service_registry.hpp are intercepted at their entry
// and never reach the vtable, so they are not listed here.
struct DyldApiExtraSlot {
    const char* exportSymbol;
    const char* serviceSymbol;
};

inline constexpr DyldApiExtraSlot kDyldApiExtraSlots[]{
    {"__dyld_get_shared_cache_uuid", "__guest_dyld_get_shared_cache_uuid"},
    {"__dyld_register_func_for_remove_image", "__guest_dyld_register_func_for_remove_image"},
    {"_dyld_image_header_containing_address", "__guest_dyld_image_header_containing_address"},
    {"_dyld_image_path_containing_address", "__guest_dyld_image_path_containing_address"},
};

struct DyldApiSlot {
    std::uint64_t offset = 0;
    std::uint64_t discriminator = 0;
    std::vector<std::string> exports;   // libdyld exports whose first APIs call uses the slot
    std::vector<std::string> laterCalls;  // exports reaching it through a later APIs call
};

// Scans every libdyld export for its first APIs virtual call with the derived
// vptr discriminator. Ordered by slot offset.
inline std::vector<DyldApiSlot> enumerate_dyld_api_slots(const SharedCache& cache,
                                                         std::uint64_t vptrDiscriminator) {
    static constexpr const char* libdyld = "/usr/lib/system/libdyld.dylib";
    constexpr std::uint64_t scanBytes = 0x600;
    std::map<std::uint64_t, DyldApiSlot> slots;
    const auto image = cache.guestImage(libdyld);
    if (!image) return {};
    std::vector<std::pair<std::string, std::int64_t>> exports(image->exports.begin(),
                                                              image->exports.end());
    std::sort(exports.begin(), exports.end());
    for (const auto& entry : exports) {
        const auto& name = entry.first;
        const auto address = entry.second;
        if (address <= 0) continue;
        const auto original = cache.originalAddress(address);
        const bool inText = std::any_of(image->text.begin(), image->text.end(),
            [original](const auto& range) {
                return original >= range.first && original < range.second;
            });
        if (!inText) continue;
        const auto calls = dyld_api_abi_detail::scan_virtual_calls(
            cache.bytes(cache.originalAddress(address), scanBytes));
        bool first = true;
        for (const auto& call : calls) {
            if (call.vptrDiscriminator != vptrDiscriminator || call.call.offset % 8 != 0 ||
                call.call.offset > 0x10000) continue;
            auto& slot = slots[call.call.offset];
            slot.offset = call.call.offset;
            if (first || (slot.exports.empty() && slot.laterCalls.empty()))
                slot.discriminator = call.call.discriminator;
            auto& names = first ? slot.exports : slot.laterCalls;
            if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
            first = false;
        }
    }
    std::vector<DyldApiSlot> ordered;
    for (auto& entry : slots) ordered.push_back(std::move(entry.second));
    if (const char* trace = std::getenv("ARM64_METAL_TRACE_DYLD_API_ABI");
        trace != nullptr && trace[0] == '1') {
        for (const auto& slot : ordered) {
            std::fprintf(stderr, "arm64-metal dyld-api-slot 0x%llx disc=0x%llx",
                         static_cast<unsigned long long>(slot.offset),
                         static_cast<unsigned long long>(slot.discriminator));
            for (const auto& name : slot.exports) std::fprintf(stderr, " %s", name.c_str());
            for (const auto& name : slot.laterCalls) std::fprintf(stderr, " +%s", name.c_str());
            std::fputc('\n', stderr);
        }
    }
    return ordered;
}

struct DyldImageAtAddress {
    std::uint64_t header = 0;
    std::string path;
};

// Process-wide view of the published APIs façade: slot names for diagnostics,
// remove-image registrations, and the shared-cache image index used by the
// containing-address methods.
class DyldApiSlotState {
public:
    void publish(std::uint64_t vtable, std::uint64_t vtableBytes,
                 std::vector<DyldApiSlot> slots) {
        vtable_ = vtable;
        vtableBytes_ = vtableBytes;
        slots_ = std::move(slots);
        removeImageCallbacks_.clear();
    }

    // Appended to a branch-authentication failure. A façade slot the runtime
    // does not serve holds null, so the failing modifier names the slot.
    std::string describe_unmodeled_slot(std::uint64_t pointer, std::uint64_t modifier,
                                        std::uint64_t linkRegister) const {
        constexpr std::uint64_t addressMask = 0x0000ffffffffffffULL;
        const auto slotAddress = modifier & addressMask;
        if (pointer != 0 || vtable_ == 0 || slotAddress < vtable_ ||
            slotAddress >= vtable_ + vtableBytes_) return {};
        const auto offset = slotAddress - vtable_;
        std::ostringstream out;
        out << std::hex << "; unmodeled dyld APIs vtable slot 0x" << offset << std::dec
            << " (index " << offset / 8 << ")";
        const auto slot = std::find_if(slots_.begin(), slots_.end(),
            [offset](const DyldApiSlot& item) { return item.offset == offset; });
        if (slot != slots_.end()) {
            out << " reached from libdyld";
            for (const auto& name : slot->exports) out << ' ' << name;
            // Later calls can come from scanning past a tail call into the
            // next function, so they are named only when nothing calls it first.
            if (slot->exports.empty()) {
                for (const auto& name : slot->laterCalls) out << " (later call in " << name << ')';
            }
        } else {
            out << " (no libdyld export uses it)";
        }
        out << std::hex << ", guest lr 0x" << linkRegister;
        return out.str();
    }

    // A null slot reached by braa/blraa resumes at PC 0. clang's arm64e virtual
    // call keeps the slot modifier in x16 (tail call) or x17 (call).
    std::string describe_null_branch(std::uint64_t pc, std::uint64_t x16, std::uint64_t x17,
                                     std::uint64_t linkRegister) const {
        auto note = describe_unmodeled_slot(pc, x16, linkRegister);
        return note.empty() ? describe_unmodeled_slot(pc, x17, linkRegister) : note;
    }

    void register_remove_image(std::uint64_t callback) {
        removeImageCallbacks_.push_back(callback);
    }

    // dyld delivers remove-image callbacks before unmapping; the runtime does
    // not, so an unload that dyld would report stops with a named error.
    void require_unload_without_remove_callbacks(std::uint64_t header) const {
        if (removeImageCallbacks_.empty()) return;
        std::ostringstream out;
        out << std::hex << "Guest image 0x" << header << " unload needs "
            << std::dec << removeImageCallbacks_.size()
            << " _dyld_register_func_for_remove_image callback(s); not modeled";
        throw std::runtime_error(out.str());
    }

    // dyld answers for every image in the shared cache, loaded or not.
    std::optional<DyldImageAtAddress> image_containing(
        std::uint64_t address, const std::vector<MappedImageSegment>& segments,
        const SharedCache* cache) {
        if (address == 0) return std::nullopt;
        for (const auto& segment : segments) {
            if (segment.start >= 0 && segment.end > segment.start &&
                address >= static_cast<std::uint64_t>(segment.start) &&
                address < static_cast<std::uint64_t>(segment.end)) {
                return DyldImageAtAddress{segment.guest_header, segment.image_path};
            }
        }
        if (cache == nullptr) return std::nullopt;
        if (cacheHeaders_.empty()) {
            std::map<std::int64_t, std::string> byHeader;
            for (const auto& [path, original] : cache->images()) {
                const auto guest = cache->guestAddress(original);
                auto found = byHeader.find(guest);
                if (found == byHeader.end() || path < found->second) byHeader[guest] = path;
            }
            cacheHeaders_.assign(byHeader.begin(), byHeader.end());
        }
        const auto next = std::upper_bound(
            cacheHeaders_.begin(), cacheHeaders_.end(), address,
            [](std::uint64_t value, const auto& entry) {
                return entry.first >= 0 && value < static_cast<std::uint64_t>(entry.first);
            });
        if (next == cacheHeaders_.begin()) return std::nullopt;
        const auto& candidate = *(next - 1);
        auto ranges = cacheSegments_.find(candidate.first);
        if (ranges == cacheSegments_.end()) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> parsedRanges;
            if (const auto parsed = cache->guestImage(candidate.second)) {
                for (const auto& segment : parsed->segments) {
                    parsedRanges.emplace_back(
                        static_cast<std::uint64_t>(cache->guestAddress(segment.address)),
                        segment.size);
                }
            }
            ranges = cacheSegments_.emplace(candidate.first, std::move(parsedRanges)).first;
        }
        for (const auto& [start, size] : ranges->second) {
            if (address >= start && address - start < size) {
                return DyldImageAtAddress{static_cast<std::uint64_t>(candidate.first),
                    cache->canonicalImagePath(candidate.second).value_or(candidate.second)};
            }
        }
        return std::nullopt;
    }

private:
    std::uint64_t vtable_ = 0;
    std::uint64_t vtableBytes_ = 0;
    std::vector<DyldApiSlot> slots_;
    std::vector<std::uint64_t> removeImageCallbacks_;
    std::vector<std::pair<std::int64_t, std::string>> cacheHeaders_;
    std::map<std::int64_t, std::vector<std::pair<std::uint64_t, std::uint64_t>>> cacheSegments_;
};

inline DyldApiSlotState& dyld_api_slot_state() {
    static DyldApiSlotState state;
    return state;
}

}  // namespace arm64metal
