#include "guest_state.hpp"
#include "process_host.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>

namespace arm64metal {
namespace {

bool is_power_of_two(std::int64_t value) noexcept {
    return value > 0 && (value & (value - 1)) == 0;
}

std::uint32_t lowest_bit(std::uint32_t bits) noexcept {
    return bits & (~bits + 1U);
}

}  // namespace

std::optional<std::uint64_t> decode_guest_ia0_callback(
    const std::uint64_t* state, std::size_t word_count,
    std::uint64_t pointer) noexcept {
    constexpr std::uint64_t tag = 0xa64e000000000000ULL;
    constexpr std::uint64_t low_mask = 0xffffffffULL;
    if (state == nullptr || word_count < guestTokenStart ||
        (pointer & ~low_mask) != tag) return std::nullopt;
    const auto count = state[guestTokenCount];
    const auto capacity = state[guestTokenCapacity];
    const auto index = (pointer & low_mask) >> 3U;
    if (count > capacity || capacity > (word_count - guestTokenStart) / 3 ||
        index == 0 || index > count) return std::nullopt;
    const auto slot = guestTokenStart + 3 * static_cast<std::size_t>(index - 1);
    const auto target = state[slot];
    if (state[slot + 1] != 0 || state[slot + 2] != 0 || target == 0 ||
        target >= guestCodeLimit || (target & 3U) != 0) return std::nullopt;
    const auto encoded = tag | (index << 3U) | (target & 7U);
    if (pointer != encoded) return std::nullopt;
    return target;
}

GuestHeap::GuestHeap(GuestRange range) : free_blocks_{range} {
    if (range.begin < 0 || range.end < range.begin) {
        throw std::invalid_argument("Invalid guest heap range");
    }
}

void GuestHeap::extend(GuestRange range) {
    if (range.begin < 0 || range.end < range.begin) {
        throw std::invalid_argument("Invalid guest heap range");
    }
    if (!free_blocks_.empty() && free_blocks_.back().end == range.begin) {
        free_blocks_.back().end = range.end;
    } else {
        free_blocks_.push_back(range);
    }
}

std::optional<std::int64_t> GuestHeap::allocate(std::uint64_t size, std::int64_t alignment) {
    return allocate_avoiding(size, alignment, {});
}

std::optional<std::int64_t> GuestHeap::allocate_avoiding(
    std::uint64_t size, std::int64_t alignment,
    const std::vector<GuestRange>& excluded) {
    if (!is_power_of_two(alignment) || size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    const auto count = std::max<std::int64_t>(1, static_cast<std::int64_t>(size));
    if (count > std::numeric_limits<std::int64_t>::max() - 15) return std::nullopt;
    const auto aligned = (count + 15) & ~std::int64_t{15};

    for (std::size_t index = 0; index < free_blocks_.size(); ++index) {
        const auto block = free_blocks_[index];
        if (block.size() < aligned) continue;
        auto address = block.begin + ((-block.begin) & (alignment - 1));
        while (address >= block.begin && address <= block.end - aligned) {
            const auto end = address + aligned;
            auto next = address;
            for (const auto& range : excluded) {
                if (range.begin >= range.end || address >= range.end || range.begin >= end) continue;
                if (range.end > std::numeric_limits<std::int64_t>::max() - (alignment - 1)) {
                    next = block.end;
                    break;
                }
                next = std::max(next,
                    (range.end + alignment - 1) & ~(alignment - 1));
            }
            if (next == address) break;
            address = next;
        }
        if (address < block.begin || address > block.end - aligned) continue;
        const auto end = address + aligned;
        std::vector<GuestRange> leftovers;
        if (block.begin < address) leftovers.push_back({block.begin, address});
        if (end < block.end) leftovers.push_back({end, block.end});
        free_blocks_.erase(free_blocks_.begin() + static_cast<std::ptrdiff_t>(index));
        free_blocks_.insert(free_blocks_.begin() + static_cast<std::ptrdiff_t>(index),
                            leftovers.begin(), leftovers.end());
        allocations_[address] = aligned;
        return address;
    }
    return std::nullopt;
}

std::optional<std::int64_t> GuestHeap::allocate_at(
    std::int64_t address, std::uint64_t size, std::int64_t alignment) {
    if (address < 0 || !is_power_of_two(alignment) || address % alignment != 0 ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    const auto count = std::max<std::int64_t>(1, static_cast<std::int64_t>(size));
    if (count > std::numeric_limits<std::int64_t>::max() - 15) return std::nullopt;
    const auto aligned = (count + 15) & ~std::int64_t{15};
    if (address > std::numeric_limits<std::int64_t>::max() - aligned) return std::nullopt;
    const auto end = address + aligned;

    for (std::size_t index = 0; index < free_blocks_.size(); ++index) {
        const auto block = free_blocks_[index];
        if (address < block.begin || end > block.end) continue;
        std::vector<GuestRange> leftovers;
        if (block.begin < address) leftovers.push_back({block.begin, address});
        if (end < block.end) leftovers.push_back({end, block.end});
        free_blocks_.erase(free_blocks_.begin() + static_cast<std::ptrdiff_t>(index));
        free_blocks_.insert(free_blocks_.begin() + static_cast<std::ptrdiff_t>(index),
                            leftovers.begin(), leftovers.end());
        allocations_.emplace(address, aligned);
        return address;
    }
    return std::nullopt;
}

std::optional<std::int64_t> GuestHeap::size_of(std::int64_t address) const {
    const auto found = allocations_.find(address);
    if (found == allocations_.end()) return std::nullopt;
    return found->second;
}

void GuestHeap::release(std::int64_t address) {
    if (address == 0) return;
    const auto found = allocations_.find(address);
    if (found == allocations_.end()) throw std::invalid_argument("Invalid guest heap pointer");
    const auto count = found->second;
    allocations_.erase(found);

    const auto position = std::lower_bound(
        free_blocks_.begin(), free_blocks_.end(), address,
        [](const GuestRange& block, std::int64_t value) { return block.begin < value; });
    auto index = static_cast<std::size_t>(position - free_blocks_.begin());
    free_blocks_.insert(position, {address, address + count});
    if (index + 1 < free_blocks_.size() && free_blocks_[index].end == free_blocks_[index + 1].begin) {
        free_blocks_[index].end = free_blocks_[index + 1].end;
        free_blocks_.erase(free_blocks_.begin() + static_cast<std::ptrdiff_t>(index + 1));
    }
    if (index > 0 && free_blocks_[index - 1].end == free_blocks_[index].begin) {
        free_blocks_[index - 1].end = free_blocks_[index].end;
        free_blocks_.erase(free_blocks_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

std::uint64_t guestMachThreadName(std::uint64_t id) noexcept {
    return 0x10003U + (id << 8U);
}

GuestMutex::GuestMutex(std::int32_t kind) : kind_(kind) {}

GuestMutex::LockResult GuestMutex::lock(std::uint64_t thread, bool tryOnly) {
    if (!owner_) {
        owner_ = thread;
        depth_ = 1;
        return {LockStatus::Acquired, 0};
    }
    if (owner_ == thread && kind_ == PTHREAD_MUTEX_RECURSIVE) {
        if (depth_ >= std::numeric_limits<std::int32_t>::max()) return {LockStatus::Error, EAGAIN};
        ++depth_;
        return {LockStatus::Acquired, 0};
    }
    if (tryOnly) return {LockStatus::Error, EBUSY};
    if (owner_ == thread && kind_ == PTHREAD_MUTEX_ERRORCHECK) return {LockStatus::Error, EDEADLK};
    waiters_.push_back(thread);
    return {LockStatus::Wait, 0};
}

GuestMutexUnlock GuestMutex::unlock(std::uint64_t thread) {
    if (owner_ != thread) return {EPERM, std::nullopt};
    --depth_;
    if (depth_ != 0) return {0, std::nullopt};
    if (waiters_.empty()) {
        owner_.reset();
        depth_ = 0;
        return {0, std::nullopt};
    }
    owner_ = waiters_.front();
    waiters_.erase(waiters_.begin());
    depth_ = 1;
    return {0, owner_};
}

std::uint32_t GuestSignals::bit(std::int32_t signal) {
    if (signal < 1 || signal > 32) throw std::out_of_range("Invalid guest signal bit");
    return std::uint32_t{1} << static_cast<std::uint32_t>(signal - 1);
}

bool GuestSignals::valid(std::int32_t signal) noexcept { return signal >= 1 && signal <= 31; }

GuestSignals::Action GuestSignals::action(std::int32_t signal) const {
    const auto found = actions_.find(signal);
    return found == actions_.end() ? Action{} : found->second;
}

void GuestSignals::set_action(Action value, std::int32_t signal) {
    actions_[signal] = value;
    if (value.handler == 1) pending_ &= ~bit(signal);
}

void GuestSignals::set_mask_value(std::uint32_t value) noexcept {
    mask_ = value & ~(bit(SIGKILL) | bit(SIGSTOP));
}

bool GuestSignals::set_mask(std::int32_t how, std::uint32_t value) {
    switch (how) {
        case SIG_BLOCK: set_mask_value(mask_ | value); return true;
        case SIG_UNBLOCK: set_mask_value(mask_ & ~value); return true;
        case SIG_SETMASK: set_mask_value(value); return true;
        default: return false;
    }
}

void GuestSignals::raise(std::int32_t signal) {
    if (action(signal).handler != 1) pending_ |= bit(signal);
}

void GuestSignals::ingest(std::uint32_t incoming) {
    while (incoming != 0) {
        const auto bitValue = lowest_bit(incoming);
        incoming &= incoming - 1U;
        const auto number = static_cast<std::int32_t>(__builtin_ctz(bitValue) + 1);
        if (valid(number)) raise(number);
    }
}

std::optional<std::pair<std::int32_t, GuestSignals::Action>> GuestSignals::next(
    std::uint32_t& threadPending) {
    const auto ready = (pending_ | threadPending) & ~mask_;
    if (ready == 0) return std::nullopt;
    const auto bitValue = lowest_bit(ready);
    const auto signal = static_cast<std::int32_t>(__builtin_ctz(bitValue) + 1);
    if ((threadPending & bitValue) != 0) threadPending &= ~bitValue;
    else pending_ &= ~bitValue;
    const auto value = action(signal);
    if ((value.flags & SA_RESETHAND) != 0) actions_[signal] = Action{};
    return std::make_pair(signal, value);
}

void GuestSignals::enter(std::int32_t signal, const Action& value) {
    mask_ |= value.mask;
    if ((value.flags & SA_NODEFER) == 0) mask_ |= bit(signal);
    set_mask_value(mask_);
}

}  // namespace arm64metal
