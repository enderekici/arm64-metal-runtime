#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace arm64metal {

struct GuestBackingSpan {
    std::int64_t backing = 0;
    std::uint64_t size = 0;
};

// Tracks only backing allocations explicitly owned by cache metadata mappings.
// Other mapping sources must keep their own lifetime records.
class CacheBackingLeases {
public:
    void track(std::int64_t backing, std::uint64_t size) {
        if (backing < 0 || size == 0 ||
            size > static_cast<std::uint64_t>(INT64_MAX - backing)) {
            throw std::invalid_argument("Invalid cache backing lease");
        }
        for (const auto& lease : leases_) {
            if (lease.backing == backing) {
                throw std::invalid_argument("Duplicate cache backing lease");
            }
        }
        leases_.push_back({backing, size});
    }

    // Retires allocations only when no resident mapping or active VM lease
    // refers to any byte in their backing interval.
    [[nodiscard]] std::vector<std::int64_t> collect_unreferenced(
        const std::vector<GuestBackingSpan>& resident,
        const std::vector<GuestBackingSpan>& activeLeases) {
        std::vector<std::int64_t> retired;
        auto lease = leases_.begin();
        while (lease != leases_.end()) {
            const auto overlaps = [&lease](const GuestBackingSpan& span) {
                if (span.backing < 0 || span.size == 0) return false;
                const auto left = static_cast<std::uint64_t>(lease->backing);
                const auto right = left + lease->size;
                const auto spanLeft = static_cast<std::uint64_t>(span.backing);
                if (span.size > static_cast<std::uint64_t>(INT64_MAX - span.backing)) {
                    return true;
                }
                const auto spanRight = spanLeft + span.size;
                return left < spanRight && spanLeft < right;
            };
            const bool referenced =
                std::any_of(resident.begin(), resident.end(), overlaps) ||
                std::any_of(activeLeases.begin(), activeLeases.end(), overlaps);
            if (referenced) {
                ++lease;
            } else {
                retired.push_back(lease->backing);
                lease = leases_.erase(lease);
            }
        }
        return retired;
    }

    void swap(CacheBackingLeases& other) noexcept { leases_.swap(other.leases_); }

private:
    struct Lease {
        std::int64_t backing;
        std::uint64_t size;
    };
    std::vector<Lease> leases_;
};

}  // namespace arm64metal
