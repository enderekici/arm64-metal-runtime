#include "guest_state.hpp"
#include "cache_backing_leases.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using arm64metal::CacheBackingLeases;
using arm64metal::GuestBackingSpan;
using arm64metal::GuestHeap;
using arm64metal::GuestRange;

int main() {
    GuestHeap heap({0x1000, 0x5000});
    CacheBackingLeases leases;
    std::vector<GuestBackingSpan> resident;
    std::int64_t current = 0;
    for (std::uint64_t step = 0; step < 128; ++step) {
        const auto size = 512 + step * 16;
        const auto allocation = heap.allocate(size);
        assert(allocation);
        leases.track(*allocation, size);
        resident = {{*allocation, size}};
        const auto retired = leases.collect_unreferenced(resident, {});
        for (const auto backing : retired) heap.release(backing);
        if (current != 0) assert(!heap.size_of(current));
        current = *allocation;
        assert(heap.size_of(current));
    }

    const auto aliased = heap.allocate(256);
    const auto replacement = heap.allocate(512);
    const auto selector = heap.allocate(128);
    assert(aliased && replacement && selector);
    leases.track(*aliased, 256);
    leases.track(*replacement, 512);
    resident = {{*replacement, 512}, {*aliased + 32, 16}, {*selector, 128}};
    auto retired = leases.collect_unreferenced(resident, {});
    assert(std::find(retired.begin(), retired.end(), *aliased) == retired.end());
    for (const auto backing : retired) heap.release(backing);
    assert(heap.size_of(*aliased));

    resident = {{*replacement, 512}, {*selector, 128}};
    retired = leases.collect_unreferenced(resident, {{*aliased, 256}});
    assert(retired.empty());
    assert(heap.size_of(*aliased));
    retired = leases.collect_unreferenced(resident, {});
    assert(retired.size() == 1 && retired.front() == *aliased);
    heap.release(retired.front());
    assert(!heap.size_of(*aliased));
    assert(heap.size_of(*replacement));
    assert(heap.size_of(*selector));
    std::cout << "cache backing leases repeated=128 alias=retained-until-unmapped "
                 "active-vm=retained selector=untouched\n";
}
