#pragma once

#include "guest_state.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace arm64metal {

inline constexpr std::size_t guestSchedulerRegisterCount = 872;
inline constexpr std::size_t guestSchedulerFPCR = 870;
inline constexpr std::size_t guestSchedulerFPSR = 871;

struct GuestJoinResult {
    enum class Status { Completed, Waiting, Error };
    Status status = Status::Error;
    std::int32_t error = 0;
    std::uint64_t value = 0;
    // A completed target is removed from the scheduler and returned here so
    // the process owner can release its guest stack/TSD allocation.
    std::optional<GuestThread> reapedThread;
};

struct GuestJoinNotification {
    std::uint64_t waiter = 0;
    std::uint64_t outputAddress = 0;
    std::uint64_t result = 0;
};

struct GuestDetachResult {
    std::int32_t error = 0;
    std::optional<GuestThread> reapedThread;
};

struct GuestThreadCompletion {
    std::uint64_t thread = 0;
    bool allComplete = false;
    std::optional<GuestJoinNotification> joined;
    std::optional<GuestThread> reapedThread;
};

struct GuestConditionWaitResult {
    std::int32_t error = 0;
    bool waiting = false;
};

struct GuestSleepInterruption {
    std::uint64_t thread = 0;
    std::uint64_t errnoAddress = 0;
    std::optional<std::uint64_t> remainderAddress;
    std::uint64_t remainingNanoseconds = 0;
    std::int32_t signal = 0;
};

// Cooperative guest thread lifecycle and FIFO scheduling state. This class
// never executes guest instructions; the process runner owns Metal dispatch.
class GuestScheduler final {
public:
    explicit GuestScheduler(GuestThread mainThread, std::uint64_t firstThreadId = 2);

    // Wrap the process bootstrap image as main guest thread 1 without
    // discarding runner-specific register tokens beyond the standard image.
    [[nodiscard]] static GuestThread make_main_thread(
        GuestRange stack, std::int64_t errnoAddress, std::int64_t tsdAddress,
        std::vector<std::uint64_t> registers, std::uint32_t signalMask = 0);

    // Build the ARM64 entry image used by pthread_create. The process runner
    // validates `entry` as guest code and supplies allocated stack/TSD ranges.
    [[nodiscard]] static GuestThread make_thread_frame(
        GuestRange stack, std::optional<std::int64_t> allocation,
        std::int64_t errnoAddress, std::int64_t tsdAddress,
        std::uint64_t entry, std::uint64_t argument,
        std::uint64_t fpcr, std::uint64_t fpsr, std::uint32_t signalMask);

    [[nodiscard]] std::uint64_t add_thread(GuestThread thread);
    [[nodiscard]] GuestThread& current();
    [[nodiscard]] const GuestThread& current() const;
    [[nodiscard]] GuestThread* find(std::uint64_t id) noexcept;
    [[nodiscard]] const GuestThread* find(std::uint64_t id) const noexcept;
    [[nodiscard]] std::uint64_t current_id() const noexcept { return currentId_; }
    [[nodiscard]] std::uint64_t next_thread_id() const noexcept { return nextThreadId_; }
    [[nodiscard]] std::size_t thread_count() const noexcept { return threads_.size(); }
    [[nodiscard]] const std::deque<std::uint64_t>& ready_queue() const noexcept { return ready_; }

    // Persist the current guest image and optionally append it to the ready
    // FIFO. Returns true only when a different ready thread was selected.
    [[nodiscard]] bool schedule(bool requeue,
                                std::vector<std::uint64_t>& liveRegisters,
                                std::uint32_t& activeThreadPending,
                                std::uint32_t& activeSignalMask);

    [[nodiscard]] GuestJoinResult join(std::uint64_t target,
                                       std::uint64_t outputAddress);
    [[nodiscard]] GuestDetachResult detach(std::uint64_t target);
    void wake(std::uint64_t id, std::int32_t wakeError = 0);
    [[nodiscard]] GuestThreadCompletion complete_current(std::uint64_t result);
    [[nodiscard]] std::optional<GuestThread> reap(std::uint64_t id);

    // Mutex/condition state is cooperative and guest-owned. The process host
    // validates guest memory and writes these return codes into x0.
    [[nodiscard]] std::int32_t initialize_mutex(std::uint64_t address,
                                                std::int32_t kind);
    [[nodiscard]] std::int32_t destroy_mutex(std::uint64_t address);
    [[nodiscard]] bool has_mutex(std::uint64_t address) const noexcept;
    [[nodiscard]] GuestMutex::LockResult lock_mutex(std::uint64_t address,
                                                   std::uint64_t thread,
                                                   bool tryOnly);
    [[nodiscard]] std::int32_t unlock_mutex(std::uint64_t address,
                                           std::uint64_t thread);

    [[nodiscard]] std::int32_t initialize_condition(std::uint64_t address,
                                                    bool attributesSupported = false);
    [[nodiscard]] std::int32_t destroy_condition(std::uint64_t address);
    [[nodiscard]] bool has_condition(std::uint64_t address) const noexcept;
    [[nodiscard]] GuestConditionWaitResult wait_condition(
        std::uint64_t condition, std::uint64_t mutex,
        std::optional<std::uint64_t> deadline = std::nullopt);
    [[nodiscard]] std::int32_t signal_condition(std::uint64_t condition,
                                               bool broadcast,
                                               std::size_t* transferred = nullptr);
    [[nodiscard]] bool sleep_current(std::uint64_t deadline,
                                    std::optional<std::uint64_t> remainder = std::nullopt);
    [[nodiscard]] std::size_t expire_waiters(std::uint64_t nowNanoseconds);
    [[nodiscard]] std::optional<std::uint64_t> earliest_deadline() const noexcept;
    [[nodiscard]] std::optional<GuestSleepInterruption> interrupt_sleeping_thread(
        std::uint32_t incomingSignals, const GuestSignals& signals,
        std::uint64_t nowNanoseconds) const noexcept;

private:
    [[nodiscard]] std::optional<GuestThread> remove_thread(std::uint64_t id);
    void enqueue_ready(std::uint64_t id);
    void reacquire_mutex(GuestThread& thread, std::uint64_t lockAddress,
                         std::int32_t result);

    std::map<std::uint64_t, GuestThread> threads_;
    std::deque<std::uint64_t> ready_;
    std::uint64_t currentId_ = 0;
    std::uint64_t nextThreadId_ = 2;
    std::optional<std::uint64_t> retireCurrent_;
    std::map<std::uint64_t, GuestMutex> mutexes_;
    std::map<std::uint64_t, std::deque<std::uint64_t>> conditionWaiters_;
};

} // namespace arm64metal
