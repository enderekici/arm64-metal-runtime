#pragma once

#include "shared_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace arm64metal {

// Pure demand-cache fault planner. The caller owns guest memory, resident tables,
// authentication-token encoding, and publication of the planned mapping.
class CacheResidencyPlanner {
public:
    struct DeferredSegment {
        std::int64_t logical_start;
        std::int64_t logical_end;
        SharedCache::Segment segment;
    };

    struct Span {
        std::int64_t logical_start;
        std::int64_t logical_end;
    };

    struct RebasePatch {
        std::size_t offset;
        std::uint64_t guest_pointer;
        std::optional<SharedCache::Authentication> authentication;
        std::optional<std::string> deferred_binding;
    };

    struct Plan {
        std::int64_t logical_start;
        std::int64_t logical_end;
        std::size_t backing;
        bool writable;
        bool readable = true;
        std::size_t next_cursor;
        std::vector<std::uint8_t> bytes;
        std::vector<RebasePatch> rebases;
        // The runner must reserve this many auth tokens before applying patches.
        std::size_t authentication_count;
        std::string segment_name;
    };

    struct Capacity {
        std::size_t cache_cursor;
        std::size_t cache_backing_end;
        std::size_t resident_count;
        std::size_t max_resident_entries;
    };

    // Returns no plan when the address is already resident, outside a deferred
    // image, or is not backed by an eligible constant-rebase slot or validated
    // read-only, non-executable cache DATA_CONST mapping.
    static std::optional<Plan> plan(
        const SharedCache& cache,
        const std::vector<DeferredSegment>& deferred,
        std::int64_t address,
        std::size_t minimum_bytes,
        std::size_t page_size,
        const std::vector<Span>& resident,
        const std::set<std::pair<std::int64_t, std::int64_t>>& cached_read_windows,
        const Capacity& capacity,
        const std::map<std::int64_t, std::string>& deferred_bindings = {});
};

}  // namespace arm64metal
