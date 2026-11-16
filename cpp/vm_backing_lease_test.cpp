#include "guest_state.hpp"
#include "vm_backing_lease.hpp"

#include <cstdio>

int main() {
  int frees = 0;
  arm64metal::GuestHeap heap({0x1000, 0x5000});
  {
    const auto backing = heap.allocate(4096, 4096);
    if (!backing) return 1;
    auto first = arm64metal::VmBackingLease(*backing, 4096, [&] {
      ++frees;
      heap.release(*backing);
    });
    auto second = first;
    first = {};
    if (frees != 0 || !heap.size_of(*backing)) return 2;
    second = {};
    if (frees != 1 || heap.size_of(*backing)) return 3;
  }
  {
    const auto backing = heap.allocate(4096, 4096);
    if (!backing) return 4;
    auto first = arm64metal::VmBackingLease(*backing, 4096, [&] {
      ++frees;
      heap.release(*backing);
    });
    auto second = first;
    second = {};
    if (frees != 1 || !heap.size_of(*backing)) return 5;
    first = {};
    if (frees != 2 || heap.size_of(*backing)) return 6;
  }
  {
    const auto backing = heap.allocate(4096, 4096);
    if (!backing) return 7;
    auto first = arm64metal::VmBackingLease(*backing, 4096, [&] {
      ++frees;
      heap.release(*backing);
    });
    auto second = first;
    first.release_now();
    second.release_now();
    first = {};
    second = {};
    if (frees != 3) {
      std::fprintf(stderr, "unexpected final frees=%d\n", frees);
      return 8;
    }
    if (heap.size_of(*backing)) return 9;
  }
  {
    const auto backing = heap.allocate(4096, 4096);
    if (!backing) return 10;
    auto first = arm64metal::VmBackingLease(*backing, 4096, [&] {
      ++frees;
      heap.release(*backing);
    });
    auto second = first;
    first.release_now();
    if (frees != 4 || heap.size_of(*backing)) return 11;
    if (!heap.allocate_at(*backing, 4096, 4096)) return 12;
    first.rearm_release([&] {
      ++frees;
      heap.release(*backing);
    });
    first = {};
    second = {};
    if (frees != 5 || heap.size_of(*backing)) return 13;
  }
  std::puts("vm backing lease: both owner orders retained backing; rollback rearmed; final frees=5");
  return 0;
}
