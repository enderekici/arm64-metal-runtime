#include "guest_state.hpp"

#include <cstdio>

int main() {
    arm64metal::GuestHeap heap({0, 16384});
    const auto first = heap.allocate_at(4096, 4096, 4096);
    if (!first || *first != 4096 || heap.size_of(*first) != 4096 ||
        heap.allocate_at(4096, 4096, 4096) ||
        heap.allocate_at(4097, 4096, 4096) ||
        heap.allocate_at(8192, 8193, 4096)) return 1;

    heap.release(*first);
    const auto replacement = heap.allocate_at(4096, 4096, 4096);
    const auto finalBlock = heap.allocate_at(8192, 8192, 4096);
    if (!replacement || *replacement != 4096 || !finalBlock || *finalBlock != 8192 ||
        heap.size_of(*replacement) != 4096 || heap.size_of(*finalBlock) != 8192) return 1;

    arm64metal::GuestHeap excludedHeap({0, 65536});
    const auto skipped = excludedHeap.allocate_avoiding(16384, 16384, {{0, 24576}});
    if (!skipped || *skipped != 32768 || excludedHeap.size_of(*skipped) != 16384) return 1;

    arm64metal::GuestHeap extended({0, 256});
    const auto low = extended.allocate_at(0, 128, 128);
    const auto blocker = extended.allocate_at(128, 128, 128);
    if (!low || !blocker || *low != 0 || *blocker != 128) return 1;
    extended.release(*low);
    extended.extend({512, 1024});
    const auto large = extended.allocate(512, 256);
    if (!large || *large != 512 || extended.size_of(*large) != 512) return 1;
    extended.release(*large);
    const auto reused = extended.allocate_at(512, 512, 256);
    if (!reused || *reused != 512 || extended.size_of(*reused) != 512) return 1;

    std::puts("guest heap allocate_at exact=ok collision=reject release=ok alignment=ok avoid=ok extend=ok");
    return 0;
}
