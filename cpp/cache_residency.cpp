#include "cache_residency.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace arm64metal {
namespace {

[[noreturn]] void residencyError(const std::string& message) {
    throw std::runtime_error(message);
}

bool contains(const CacheResidencyPlanner::Span& span, std::int64_t address,
              std::size_t minimumBytes) {
    return address >= span.logical_start && address < span.logical_end &&
           minimumBytes <= static_cast<std::uint64_t>(span.logical_end - address);
}

bool overlaps(std::int64_t lower, std::int64_t upper,
              std::int64_t otherLower, std::int64_t otherUpper) {
    return lower < otherUpper && otherLower < upper;
}

// A page window of a read-only, executable cache mapping that holds no
// deferred segment of a loaded image.
std::optional<CacheResidencyPlanner::DeferredSegment> textWindow(
    const SharedCache& cache, std::int64_t address, std::size_t minimumBytes,
    std::size_t pageSize, const std::vector<CacheResidencyPlanner::Span>& resident) {
    if (pageSize == 0 || (pageSize & (pageSize - 1)) != 0) return std::nullopt;
    const auto requestEnd = address + static_cast<std::int64_t>(minimumBytes);
    for (const auto& region : cache.cacheRegions()) {
        if ((region.protection & 7) != 5 || (region.maximumProtection & 2) != 0 ||
            region.start < SharedCache::guestBase || region.start >= SharedCache::guestLimit ||
            region.size > static_cast<std::uint64_t>(SharedCache::guestLimit - region.start)) continue;
        const auto regionEnd = region.start + static_cast<std::int64_t>(region.size);
        if (address < region.start || requestEnd > regionEnd) continue;
        const auto mask = static_cast<std::int64_t>(pageSize - 1);
        auto lower = std::max(address & ~mask, region.start);
        auto upper = std::min(std::max((address & ~mask) + static_cast<std::int64_t>(pageSize),
                                       requestEnd), regionEnd);
        for (const auto& span : resident) {
            if (!overlaps(lower, upper, span.logical_start, span.logical_end)) continue;
            if (span.logical_start <= address && address < span.logical_end) return std::nullopt;
            if (span.logical_end <= address) lower = std::max(lower, span.logical_end);
            if (span.logical_start > address) upper = std::min(upper, span.logical_start);
        }
        if (requestEnd > upper) return std::nullopt;
        const auto length = static_cast<std::uint64_t>(upper - lower);
        return CacheResidencyPlanner::DeferredSegment{
            lower, upper,
            SharedCache::Segment{"__TEXT", cache.originalAddress(lower), length, 0, length, false}};
    }
    return std::nullopt;
}

}  // namespace

std::optional<CacheResidencyPlanner::Plan> CacheResidencyPlanner::plan(
    const SharedCache& cache,
    const std::vector<DeferredSegment>& deferred,
    std::int64_t address,
    std::size_t minimumBytes,
    std::size_t pageSize,
    const std::vector<Span>& resident,
    const std::set<std::pair<std::int64_t, std::int64_t>>& cachedReadWindows,
    const Capacity& capacity,
    const std::map<std::int64_t, std::string>& deferredBindings) {
    minimumBytes = std::max<std::size_t>(minimumBytes, 1);
    if (address < SharedCache::guestBase || address >= SharedCache::guestLimit ||
        minimumBytes > static_cast<std::uint64_t>(SharedCache::guestLimit - address)) return std::nullopt;
    for (const auto& span : resident) if (contains(span, address, minimumBytes)) return std::nullopt;

    const DeferredSegment* selected = nullptr;
    for (const auto& candidate : deferred) {
        if (candidate.logical_start <= address && address < candidate.logical_end) {
            selected = &candidate;
            break;
        }
    }

    DeferredSegment pending{};
    if (selected != nullptr) {
        if (selected->logical_end <= selected->logical_start ||
            static_cast<std::uint64_t>(selected->logical_end - selected->logical_start) != selected->segment.size ||
            selected->segment.fileSize > selected->segment.size) {
            residencyError("Invalid deferred cache segment");
        }
        if (minimumBytes > static_cast<std::uint64_t>(selected->logical_end - address)) {
            residencyError("Cached read crosses a segment boundary");
        }
        const bool text = selected->segment.name == "__TEXT" && !selected->segment.writable;
        if (text || selected->segment.name == "__OBJC_RO") {
            std::int64_t windowLower = 0;
            std::int64_t windowUpper = 0;
            if (text) {
                if (pageSize == 0 || (pageSize & (pageSize - 1)) != 0) residencyError("Invalid host page size");
                const auto mask = static_cast<std::int64_t>(pageSize - 1);
                windowLower = address & ~mask;
                const auto pageEnd = windowLower + static_cast<std::int64_t>(pageSize);
                const auto requestEnd = address + static_cast<std::int64_t>(minimumBytes);
                windowUpper = std::max(pageEnd, requestEnd);
            } else {
                const auto window = cache.selectorWindow(address, minimumBytes);
                windowLower = window.first;
                windowUpper = window.second;
            }
            auto lower = std::max(windowLower, selected->logical_start);
            auto upper = std::min(windowUpper, selected->logical_end);
            for (const auto& region : resident) {
                if (!overlaps(region.logical_start, region.logical_end, windowLower, windowUpper) ||
                    cachedReadWindows.find({region.logical_start, region.logical_end}) != cachedReadWindows.end()) continue;
                if (region.logical_start <= address && address < region.logical_end) return std::nullopt;
                if (region.logical_end <= address) lower = std::max(lower, region.logical_end);
                if (region.logical_start > address) upper = std::min(upper, region.logical_start);
            }
            if (address < lower || address >= upper || minimumBytes > static_cast<std::uint64_t>(upper - address)) {
                residencyError("Cached read crosses a segment boundary");
            }
            const auto delta = static_cast<std::uint64_t>(lower - selected->logical_start);
            const auto length = static_cast<std::uint64_t>(upper - lower);
            if (delta > std::numeric_limits<std::uint64_t>::max() - selected->segment.address ||
                delta > std::numeric_limits<std::uint64_t>::max() - selected->segment.fileOffset) {
                residencyError("Deferred cache segment offset overflow");
            }
            const auto fileBytes = delta < selected->segment.fileSize
                ? std::min(length, selected->segment.fileSize - delta) : 0;
            pending = DeferredSegment{
                lower, upper,
                SharedCache::Segment{selected->segment.name, selected->segment.address + delta, length,
                                     selected->segment.fileOffset + delta, fileBytes, false}};
        } else {
            // A part of this segment can already be resident (an earlier window,
            // constant slot or staged image); map only the gap around the request
            // so no logical address gets two backings.
            auto lower = selected->logical_start;
            auto upper = selected->logical_end;
            for (const auto& region : resident) {
                if (!overlaps(lower, upper, region.logical_start, region.logical_end)) continue;
                if (region.logical_start <= address && address < region.logical_end) return std::nullopt;
                if (region.logical_end <= address) lower = std::max(lower, region.logical_end);
                if (region.logical_start > address) upper = std::min(upper, region.logical_start);
            }
            if (minimumBytes > static_cast<std::uint64_t>(upper - address)) {
                residencyError("Cached read crosses a resident mapping boundary");
            }
            if (lower == selected->logical_start && upper == selected->logical_end) {
                pending = *selected;
            } else {
                const auto delta = static_cast<std::uint64_t>(lower - selected->logical_start);
                const auto length = static_cast<std::uint64_t>(upper - lower);
                const auto fileBytes = delta < selected->segment.fileSize
                    ? std::min(length, selected->segment.fileSize - delta) : 0;
                pending = DeferredSegment{
                    lower, upper,
                    SharedCache::Segment{selected->segment.name, selected->segment.address + delta,
                                         length, selected->segment.fileOffset + delta, fileBytes,
                                         selected->segment.writable}};
            }
        }
    } else {
        const auto slot = cache.constantRebaseSlot(address);
        if (slot) {
            const auto lower = address & ~std::int64_t{7};
            if (slot->size != 8 || address < lower || minimumBytes > static_cast<std::uint64_t>(lower + 8 - address)) {
                return std::nullopt;
            }
            for (const auto& region : resident) {
                if (region.logical_start <= address && address < region.logical_end) return std::nullopt;
            }
            const auto logicalStart = cache.guestAddress(slot->address);
            pending = DeferredSegment{logicalStart, logicalStart + 8, *slot};
        } else if (const auto text = textWindow(cache, address, minimumBytes, pageSize, resident)) {
            // The real process maps the whole cache, so libobjc may read read-only
            // text (e.g. relative method lists) of images that were never loaded.
            pending = *text;
        } else {
            // Coalesced cache constants can lie outside all image segments and
            // include scalar data that has no slide/rebase entry.
            const auto requestEnd = address + static_cast<std::int64_t>(minimumBytes);
            const SharedCache::CacheRegion* owner = nullptr;
            for (const auto& region : cache.cacheRegions()) {
                if ((region.flags & 4) == 0 || (region.protection & 7) != 1 ||
                    (region.maximumProtection & 4) != 0 ||
                    region.start < SharedCache::guestBase || region.start >= SharedCache::guestLimit ||
                    region.size > static_cast<std::uint64_t>(SharedCache::guestLimit - region.start)) continue;
                const auto end = region.start + static_cast<std::int64_t>(region.size);
                if (address >= region.start && address < end && requestEnd <= end) {
                    owner = &region;
                    break;
                }
            }
            if (!owner || !cache.isCacheDataConst(address, requestEnd)) return std::nullopt;
            // Include complete pointer slots for the common rebase pass, but do
            // not eagerly materialize unrelated constants on the same page.
            auto lower = std::max(address & ~std::int64_t{7}, owner->start);
            const auto ownerEnd = owner->start + static_cast<std::int64_t>(owner->size);
            auto upper = std::min((requestEnd + 7) & ~std::int64_t{7}, ownerEnd);
            // Never replace an existing resident mapping or publish overlapping aliases.
            for (const auto& region : resident) {
                if (!overlaps(lower, upper, region.logical_start, region.logical_end)) continue;
                if (region.logical_start <= address && address < region.logical_end) return std::nullopt;
                if (region.logical_end <= address) lower = std::max(lower, region.logical_end);
                if (region.logical_start > address) upper = std::min(upper, region.logical_start);
            }
            if (address < lower || requestEnd > upper || (lower & 7) != 0 || (upper & 7) != 0) {
                return std::nullopt;
            }
            const auto length = static_cast<std::uint64_t>(upper - lower);
            pending = DeferredSegment{
                lower, upper,
                SharedCache::Segment{"cache const data", cache.originalAddress(lower), length,
                                     0, length, false}};
        }
    }

    if (capacity.resident_count >= capacity.max_resident_entries) {
        residencyError("Guest resident map capacity exceeded");
    }
    if (pending.logical_end <= pending.logical_start) residencyError("Invalid planned cache region");
    const auto logicalSize = static_cast<std::uint64_t>(pending.logical_end - pending.logical_start);
    if (logicalSize > std::numeric_limits<std::size_t>::max()) residencyError("Planned cache region is too large");
    if (capacity.cache_cursor > std::numeric_limits<std::size_t>::max() - 15) residencyError("Cache backing offset overflow");
    const auto aligned = (capacity.cache_cursor + 15) & ~std::size_t{15};
    const auto lowBits = static_cast<std::size_t>(pending.logical_start & 15);
    if (aligned > std::numeric_limits<std::size_t>::max() - lowBits) residencyError("Cache backing offset overflow");
    const auto backing = aligned + lowBits;
    if (backing > capacity.cache_backing_end || logicalSize > capacity.cache_backing_end - backing) {
        residencyError("Guest cache resident backing capacity exceeded");
    }
    const auto segment = pending.segment;
    if (segment.fileSize > segment.size || segment.fileSize > logicalSize ||
        segment.fileSize > std::numeric_limits<std::size_t>::max()) residencyError("Invalid deferred cache file range");
    std::vector<std::uint8_t> mappedBytes(static_cast<std::size_t>(logicalSize), 0);
    const auto fileBytes = cache.bytes(segment.address, segment.fileSize);
    if (!fileBytes.empty()) std::copy(fileBytes.begin(), fileBytes.end(), mappedBytes.begin());

    std::vector<RebasePatch> patches;
    const auto endAddress = segment.address + segment.size;
    if (endAddress < segment.address) residencyError("Cache rebase range overflow");
    const auto rebases = cache.rebases(segment.address, endAddress);
    std::size_t authenticationCount = 0;
    patches.reserve(rebases.size());
    for (const auto& rebase : rebases) {
        const auto logical = cache.guestAddress(rebase.location);
        if (logical < pending.logical_start || logical > pending.logical_end - 8) {
            residencyError("Cached rebase lies outside planned region");
        }
        const auto offset = static_cast<std::size_t>(logical - pending.logical_start);
        auto pointer = static_cast<std::uint64_t>(cache.guestAddress(rebase.target)) |
                       (static_cast<std::uint64_t>(rebase.highByte) << 56);
        std::optional<std::string> binding;
        const auto bindingIt = deferredBindings.find(logical);
        if (bindingIt != deferredBindings.end()) binding = bindingIt->second;
        if (rebase.authentication) ++authenticationCount;
        patches.push_back(RebasePatch{offset, pointer, rebase.authentication, std::move(binding)});
    }
    if (backing > std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(logicalSize)) {
        residencyError("Cache backing cursor overflow");
    }
    return Plan{pending.logical_start, pending.logical_end, backing, segment.writable, true,
                backing + static_cast<std::size_t>(logicalSize), std::move(mappedBytes),
                std::move(patches), authenticationCount, segment.name};
}

}  // namespace arm64metal
