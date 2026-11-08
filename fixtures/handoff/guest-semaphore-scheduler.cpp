#include "guest_scheduler.hpp"

#include "darwin_bridge.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace arm64metal {
std::uint32_t guest_signal_take() noexcept { return 0; }
} // namespace arm64metal

using namespace arm64metal;

static GuestThread make_main() {
    std::vector<std::uint64_t> registers(guestSchedulerRegisterCount, 0);
    return GuestScheduler::make_main_thread({0x1000, 0x2000}, 0, 0,
                                             std::move(registers));
}

static GuestThread make_worker() {
    return GuestScheduler::make_thread_frame({0x2000, 0x3000}, std::nullopt,
                                              0, 0, 0x4000, 0, 0, 0, 0);
}

int main() {
    GuestScheduler scheduler(make_main());
    const auto worker = scheduler.add_thread(make_worker());
    assert(scheduler.create_semaphore(100, 0, 0) == 0);

    auto live = scheduler.current().registers;
    std::uint32_t pending = 0;
    std::uint32_t mask = 0;
    assert(scheduler.schedule(true, live, pending, mask));
    assert(scheduler.current_id() == worker);
    for (std::size_t index = 806; index < live.size(); ++index) {
        live[index] = 0x53454d4100000000ULL + index;
    }
    live[37] = 0x5151;
    assert(scheduler.wait_semaphore(100).waiting);
    live[0] = 0;
    assert(scheduler.schedule(false, live, pending, mask));
    assert(scheduler.current_id() == 1);
    assert(scheduler.signal_semaphore(100) == 0);
    assert(scheduler.schedule(false, live, pending, mask));
    assert(scheduler.current_id() == worker);
    assert(live[0] == 0);
    assert(live[37] == 0x5151);
    for (std::size_t index = 806; index < live.size(); ++index) {
        assert(live[index] == 0x53454d4100000000ULL + index);
    }

    assert(scheduler.create_semaphore(101, 1, 0) == 4);
    assert(scheduler.create_semaphore(102, 3, 0) == 4);

    GuestScheduler timeout(make_main());
    assert(timeout.create_semaphore(103, 2, 0) == 0);
    assert(timeout.wait_semaphore(103, 500).waiting);
    assert(timeout.expire_waiters(500) == 1);
    assert(timeout.current().registers[0] == 49);

    GuestScheduler expiredBeforeDestroy(make_main());
    assert(expiredBeforeDestroy.create_semaphore(104, 2, 0) == 0);
    assert(expiredBeforeDestroy.wait_semaphore(104, 700).waiting);
    assert(expiredBeforeDestroy.expire_waiters(700) == 1);
    assert(expiredBeforeDestroy.destroy_semaphore(104) == 0);
    assert(expiredBeforeDestroy.current().registers[0] == 49);

    GuestScheduler destroyed(make_main());
    assert(destroyed.create_semaphore(105, 2, 0) == 0);
    assert(destroyed.wait_semaphore(105).waiting);
    assert(destroyed.destroy_semaphore(105) == 0);
    assert(destroyed.current().registers[0] == 37);

    std::cout << "blocked-worker-state=preserved timeout=49 expired-before-destroy=49 "
                 "destroy=37 invalid-policies=4,4\n";
    return 0;
}
