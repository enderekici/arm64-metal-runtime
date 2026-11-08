#pragma once

// C++17 data model for guest process state. This header does not execute guest
// instructions or own host resources; callers provide addresses and memory IO.
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace arm64metal {

// Authenticate an IA-key, zero-modifier guest callback token against the full
// current process state. No unsigned-pointer fallback. The returned target is
// aligned and within the guest code address range; the caller must additionally
// verify it belongs to a currently executable guest image.
[[nodiscard]] std::optional<std::uint64_t> decode_guest_ia0_callback(
    const std::uint64_t* state, std::size_t word_count,
    std::uint64_t pointer) noexcept;

struct GuestRange {
    std::int64_t begin = 0;
    std::int64_t end = 0;  // exclusive

    [[nodiscard]] std::int64_t size() const noexcept { return end - begin; }
};

// First-fit guest heap. Allocations are rounded to 16 bytes, and the returned
// start is aligned to `alignment` (a positive power of two). `release(0)` is a
// no-op; releasing any other unknown address throws std::invalid_argument.
class GuestHeap {
public:
    explicit GuestHeap(GuestRange range);

    void extend(GuestRange range);
    [[nodiscard]] std::optional<std::int64_t> allocate(
        std::uint64_t size, std::int64_t alignment = 16);
    [[nodiscard]] std::optional<std::int64_t> allocate_avoiding(
        std::uint64_t size, std::int64_t alignment,
        const std::vector<GuestRange>& excluded);
    [[nodiscard]] std::optional<std::int64_t> allocate_at(
        std::int64_t address, std::uint64_t size, std::int64_t alignment = 16);
    [[nodiscard]] std::optional<std::int64_t> size_of(std::int64_t address) const;
    void release(std::int64_t address);

private:
    std::vector<GuestRange> free_blocks_;
    std::map<std::int64_t, std::int64_t> allocations_;
};

inline constexpr std::uint64_t guestTSDSize = 1024U * sizeof(std::uint64_t);
inline constexpr std::uint64_t guestMachThreadSlotOffset = 3U * sizeof(std::uint64_t);

// Encodes a guest Mach thread name in a namespace separate from the task port.
[[nodiscard]] std::uint64_t guestMachThreadName(std::uint64_t id) noexcept;

struct GuestJoinWait {
    std::uint64_t thread = 0;
    std::uint64_t output = 0;
};
struct GuestMutexWait {
    std::uint64_t address = 0;
};
struct GuestConditionWait {
    std::uint64_t address = 0;
    std::uint64_t mutexAddress = 0;
    std::optional<std::uint64_t> deadline;
};
struct GuestSleepWait {
    std::uint64_t deadline = 0;
    std::optional<std::uint64_t> remainder;
};
struct GuestSemaphoreWait {
    std::uint64_t semaphore = 0;
    std::optional<std::uint64_t> deadline;
};
struct GuestMachReceiveWait {
    std::uint32_t port = 0;
    std::optional<std::uint64_t> deadline;
};
struct GuestUnixSocketReceiveWait {
    std::int32_t descriptor = -1;
};
using GuestWait = std::variant<GuestJoinWait, GuestMutexWait, GuestConditionWait,
                               GuestSleepWait, GuestSemaphoreWait, GuestMachReceiveWait,
                               GuestUnixSocketReceiveWait>;

// Guest thread bookkeeping only. `stack` and addresses are guest virtual
// offsets; the runtime owns register save/restore and backing memory access.
struct GuestThread {
    std::uint64_t id = 0;
    GuestRange stack;
    std::optional<std::int64_t> allocation;
    std::int64_t errnoAddress = 0;
    std::int64_t tsdAddress = 0;
    std::vector<std::uint64_t> registers;
    std::uint32_t signalMask = 0;
    std::uint32_t pendingSignals = 0;
    // Darwin's JIT write-protection mode is per guest thread and starts enabled.
    bool jitWriteProtect = true;
    std::optional<GuestWait> wait;
    std::int32_t wakeError = 0;
    std::optional<std::uint64_t> result;
    bool detached = false;
};

struct GuestMutexUnlock {
    std::int32_t error = 0;
    std::optional<std::uint64_t> nextOwner;
};

class GuestMutex {
public:
    explicit GuestMutex(std::int32_t kind);

    enum class LockStatus { Acquired, Wait, Error };
    struct LockResult {
        LockStatus status = LockStatus::Acquired;
        std::int32_t error = 0;
    };

    [[nodiscard]] LockResult lock(std::uint64_t thread, bool tryOnly);
    [[nodiscard]] GuestMutexUnlock unlock(std::uint64_t thread);

    [[nodiscard]] std::int32_t kind() const noexcept { return kind_; }
    [[nodiscard]] std::optional<std::uint64_t> owner() const noexcept { return owner_; }
    [[nodiscard]] std::int32_t depth() const noexcept { return depth_; }
    [[nodiscard]] const std::vector<std::uint64_t>& waiters() const noexcept { return waiters_; }

private:
    std::int32_t kind_;
    std::optional<std::uint64_t> owner_;
    std::int32_t depth_ = 0;
    std::vector<std::uint64_t> waiters_;
};

class GuestSignals {
public:
    struct Action {
        std::uint64_t handler = 0;
        std::uint32_t mask = 0;
        std::int32_t flags = 0;
    };

    [[nodiscard]] static std::uint32_t bit(std::int32_t signal);
    [[nodiscard]] static bool valid(std::int32_t signal) noexcept;
    [[nodiscard]] Action action(std::int32_t signal) const;
    void set_action(Action action, std::int32_t signal);
    [[nodiscard]] std::uint32_t mask() const noexcept { return mask_; }
    void set_mask_value(std::uint32_t mask) noexcept;
    [[nodiscard]] bool set_mask(std::int32_t how, std::uint32_t value);
    void raise(std::int32_t signal);
    void ingest(std::uint32_t incoming);
    [[nodiscard]] std::optional<std::pair<std::int32_t, Action>> next(
        std::uint32_t& threadPending);
    void enter(std::int32_t signal, const Action& action);

private:
    std::map<std::int32_t, Action> actions_;
    std::uint32_t pending_ = 0;
    std::uint32_t mask_ = 0;
};

}  // namespace arm64metal
