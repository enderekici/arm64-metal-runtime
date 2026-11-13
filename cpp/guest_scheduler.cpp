#include "guest_scheduler.hpp"

#include <mach/kern_return.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <utility>

namespace arm64metal {

GuestScheduler::GuestScheduler(GuestThread mainThread, std::uint64_t firstThreadId)
    : currentId_(mainThread.id), nextThreadId_(firstThreadId) {
    if (currentId_ == 0 || firstThreadId == 0 || firstThreadId <= currentId_) {
        throw std::invalid_argument("Invalid initial guest thread ids");
    }
    threads_.emplace(currentId_, std::move(mainThread));
}

GuestThread GuestScheduler::make_main_thread(
    GuestRange stack, std::int64_t errnoAddress, std::int64_t tsdAddress,
    std::vector<std::uint64_t> registers, std::uint32_t signalMask) {
    if (stack.begin < 0 || stack.end <= stack.begin || errnoAddress < 0 ||
        tsdAddress < 0 || registers.size() < guestSchedulerRegisterCount) {
        throw std::invalid_argument("Invalid guest main thread image");
    }
    GuestThread thread;
    thread.id = 1;
    thread.stack = stack;
    thread.errnoAddress = errnoAddress;
    thread.tsdAddress = tsdAddress;
    thread.registers = std::move(registers);
    thread.signalMask = signalMask;
    return thread;
}

GuestThread GuestScheduler::make_thread_frame(
    GuestRange stack, std::optional<std::int64_t> allocation,
    std::int64_t errnoAddress, std::int64_t tsdAddress,
    std::uint64_t entry, std::uint64_t argument,
    std::uint64_t fpcr, std::uint64_t fpsr, std::uint32_t signalMask) {
    if (stack.begin < 0 || stack.end <= stack.begin || errnoAddress < 0 || tsdAddress < 0 ||
        (allocation.has_value() && *allocation < 0)) {
        throw std::invalid_argument("Invalid guest thread frame addresses");
    }
    GuestThread thread;
    thread.stack = stack;
    thread.allocation = allocation;
    thread.errnoAddress = errnoAddress;
    thread.tsdAddress = tsdAddress;
    thread.registers.assign(guestSchedulerRegisterCount, 0);
    thread.registers[0] = argument;
    thread.registers[30] = guestReturnSentinel;
    thread.registers[31] = static_cast<std::uint64_t>(stack.end);
    thread.registers[33] = entry;
    thread.registers[guestSchedulerFPCR] = fpcr;
    thread.registers[guestSchedulerFPSR] = fpsr;
    thread.signalMask = signalMask;
    return thread;
}

std::uint64_t GuestScheduler::add_thread(GuestThread thread) {
    if (thread.id == 0) {
        if (nextThreadId_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Guest thread id space exhausted");
        }
        thread.id = nextThreadId_++;
    } else {
        if (thread.id == currentId_ || threads_.find(thread.id) != threads_.end()) {
            throw std::invalid_argument("Duplicate guest thread id");
        }
        if (thread.id >= nextThreadId_) {
            if (thread.id == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("Guest thread id space exhausted");
            }
            nextThreadId_ = thread.id + 1;
        }
    }
    const auto id = thread.id;
    if (!threads_.emplace(id, std::move(thread)).second) {
        throw std::invalid_argument("Duplicate guest thread id");
    }
    enqueue_ready(id);
    return id;
}

GuestThread& GuestScheduler::current() {
    const auto found = threads_.find(currentId_);
    if (found == threads_.end()) throw std::logic_error("No current guest thread");
    return found->second;
}

const GuestThread& GuestScheduler::current() const {
    const auto found = threads_.find(currentId_);
    if (found == threads_.end()) throw std::logic_error("No current guest thread");
    return found->second;
}

GuestThread* GuestScheduler::find(std::uint64_t id) noexcept {
    const auto found = threads_.find(id);
    return found == threads_.end() ? nullptr : &found->second;
}

const GuestThread* GuestScheduler::find(std::uint64_t id) const noexcept {
    const auto found = threads_.find(id);
    return found == threads_.end() ? nullptr : &found->second;
}

void GuestScheduler::enqueue_ready(std::uint64_t id) {
    for (const auto queued : ready_) {
        if (queued == id) throw std::logic_error("Guest thread is already ready");
    }
    ready_.push_back(id);
}

bool GuestScheduler::schedule(bool requeue,
                              std::vector<std::uint64_t>& liveRegisters,
                              std::uint32_t& activeThreadPending,
                              std::uint32_t& activeSignalMask) {
    if (requeue && ready_.empty()) return false;
    auto currentIt = threads_.find(currentId_);
    if (currentIt != threads_.end()) {
        auto& running = currentIt->second;
        if (requeue || !running.result.has_value()) {
            running.registers = liveRegisters;
            running.pendingSignals = activeThreadPending;
            running.signalMask = activeSignalMask;
        }
        if (requeue) enqueue_ready(currentId_);
    }

    if (retireCurrent_ && *retireCurrent_ == currentId_ && !requeue) {
        threads_.erase(currentId_);
        retireCurrent_.reset();
    }
    if (ready_.empty()) return false;
    const auto next = ready_.front();
    ready_.pop_front();
    const auto selected = threads_.find(next);
    if (selected == threads_.end()) throw std::logic_error("Ready guest thread was removed");
    if (selected->second.registers.empty()) {
        throw std::logic_error("Ready guest thread has no saved register image");
    }
    currentId_ = next;
    liveRegisters = selected->second.registers;
    activeThreadPending = selected->second.pendingSignals;
    activeSignalMask = selected->second.signalMask;
    return true;
}

GuestJoinResult GuestScheduler::join(std::uint64_t target,
                                     std::uint64_t outputAddress) {
    if (target == currentId_) return {GuestJoinResult::Status::Error, EDEADLK, 0, std::nullopt};
    auto targetIt = threads_.find(target);
    if (targetIt == threads_.end()) return {GuestJoinResult::Status::Error, ESRCH, 0, std::nullopt};
    if (targetIt->second.detached) {
        return {GuestJoinResult::Status::Error, EINVAL, 0, std::nullopt};
    }
    for (const auto& entry : threads_) {
        if (entry.first == currentId_) continue;
        if (entry.second.wait.has_value()) {
            if (const auto* wait = std::get_if<GuestJoinWait>(&*entry.second.wait);
                wait != nullptr && wait->thread == target) {
                return {GuestJoinResult::Status::Error, EINVAL, 0, std::nullopt};
            }
        }
    }
    if (targetIt->second.result.has_value()) {
        const auto value = *targetIt->second.result;
        auto removed = remove_thread(target);
        return {GuestJoinResult::Status::Completed, 0, value, std::move(removed)};
    }
    current().wait = GuestJoinWait{target, outputAddress};
    return {GuestJoinResult::Status::Waiting, 0, 0, std::nullopt};
}

GuestDetachResult GuestScheduler::detach(std::uint64_t target) {
    const auto found = threads_.find(target);
    if (found == threads_.end()) return {ESRCH, std::nullopt};
    if (found->second.detached) return {EINVAL, std::nullopt};
    for (const auto& entry : threads_) {
        if (entry.first == target || !entry.second.wait.has_value()) continue;
        const auto* wait = std::get_if<GuestJoinWait>(&*entry.second.wait);
        if (wait != nullptr && wait->thread == target) return {EINVAL, std::nullopt};
    }
    found->second.detached = true;
    if (!found->second.result.has_value()) return {0, std::nullopt};
    return {0, remove_thread(target)};
}

void GuestScheduler::wake(std::uint64_t id, std::int32_t wakeError) {
    auto found = threads_.find(id);
    if (found == threads_.end() || !found->second.wait.has_value()) {
        throw std::invalid_argument("Invalid guest wakeup");
    }
    auto& thread = found->second;
    thread.wait.reset();
    thread.wakeError = 0;
    if (thread.registers.empty()) throw std::logic_error("Woken guest thread has no saved registers");
    thread.registers[0] = static_cast<std::uint32_t>(wakeError);
    enqueue_ready(id);
}

GuestThreadCompletion GuestScheduler::complete_current(std::uint64_t result) {
    auto& completed = current();
    completed.result = result;
    GuestThreadCompletion completion;
    completion.thread = currentId_;

    if (completed.detached) {
        completion.reapedThread = completed;
        retireCurrent_ = currentId_;
    }

    for (auto& entry : threads_) {
        if (completion.reapedThread.has_value()) break;
        if (entry.first == currentId_ || !entry.second.wait.has_value()) continue;
        const auto* joinWait = std::get_if<GuestJoinWait>(&*entry.second.wait);
        if (joinWait == nullptr || joinWait->thread != currentId_) continue;
        completion.joined = GuestJoinNotification{entry.first, joinWait->output, result};
        wake(entry.first, 0);
        completion.reapedThread = completed;
        retireCurrent_ = currentId_;
        break;
    }

    completion.allComplete = true;
    for (const auto& entry : threads_) {
        if (retireCurrent_ && entry.first == *retireCurrent_) continue;
        if (!entry.second.result.has_value()) {
            completion.allComplete = false;
            break;
        }
    }
    return completion;
}

std::optional<GuestThread> GuestScheduler::remove_thread(std::uint64_t id) {
    if (id == currentId_) throw std::invalid_argument("Cannot reap the current guest thread");
    const auto found = threads_.find(id);
    if (found == threads_.end()) return std::nullopt;
    for (auto ready = ready_.begin(); ready != ready_.end();) {
        if (*ready == id) ready = ready_.erase(ready);
        else ++ready;
    }
    GuestThread removed = std::move(found->second);
    threads_.erase(found);
    return removed;
}

std::optional<GuestThread> GuestScheduler::reap(std::uint64_t id) {
    const auto found = threads_.find(id);
    if (found == threads_.end()) return std::nullopt;
    if (!found->second.result.has_value()) {
        throw std::invalid_argument("Cannot reap a live guest thread");
    }
    return remove_thread(id);
}

std::int32_t GuestScheduler::initialize_mutex(std::uint64_t address,
                                              std::int32_t kind) {
    if (address == 0 || (kind != PTHREAD_MUTEX_NORMAL &&
                         kind != PTHREAD_MUTEX_ERRORCHECK &&
                         kind != PTHREAD_MUTEX_RECURSIVE)) {
        return EINVAL;
    }
    mutexes_.insert_or_assign(address, GuestMutex(kind));
    return 0;
}

std::int32_t GuestScheduler::destroy_mutex(std::uint64_t address) {
    const auto found = mutexes_.find(address);
    if (found == mutexes_.end()) return EINVAL;
    if (found->second.owner().has_value() || !found->second.waiters().empty()) return EBUSY;
    mutexes_.erase(found);
    return 0;
}

bool GuestScheduler::has_mutex(std::uint64_t address) const noexcept {
    return mutexes_.find(address) != mutexes_.end();
}

GuestMutex::LockResult GuestScheduler::lock_mutex(std::uint64_t address,
                                                 std::uint64_t threadId,
                                                 bool tryOnly) {
    auto* thread = find(threadId);
    const auto found = mutexes_.find(address);
    if (thread == nullptr || found == mutexes_.end()) {
        return {GuestMutex::LockStatus::Error, EINVAL};
    }
    if (thread->wait.has_value()) {
        return {GuestMutex::LockStatus::Error, EINVAL};
    }
    auto result = found->second.lock(threadId, tryOnly);
    if (result.status == GuestMutex::LockStatus::Wait) {
        if (thread->wait.has_value()) throw std::logic_error("Guest thread is already waiting");
        thread->wait = GuestMutexWait{address};
    }
    return result;
}

std::int32_t GuestScheduler::unlock_mutex(std::uint64_t address,
                                         std::uint64_t threadId) {
    const auto found = mutexes_.find(address);
    if (found == mutexes_.end()) return EINVAL;
    const auto outcome = found->second.unlock(threadId);
    if (outcome.error != 0) return outcome.error;
    if (outcome.nextOwner.has_value()) {
        auto* next = find(*outcome.nextOwner);
        if (next == nullptr) throw std::logic_error("Mutex waiter thread disappeared");
        const auto result = next->wakeError;
        wake(*outcome.nextOwner, result);
    }
    return 0;
}

std::int32_t GuestScheduler::initialize_condition(std::uint64_t address,
                                                  bool attributesSupported) {
    if (address == 0 || attributesSupported) return EINVAL;
    conditionWaiters_[address] = {};
    return 0;
}

std::int32_t GuestScheduler::destroy_condition(std::uint64_t address) {
    const auto found = conditionWaiters_.find(address);
    if (found == conditionWaiters_.end()) return EINVAL;
    if (!found->second.empty()) return EBUSY;
    conditionWaiters_.erase(found);
    return 0;
}

bool GuestScheduler::has_condition(std::uint64_t address) const noexcept {
    return conditionWaiters_.find(address) != conditionWaiters_.end();
}

void GuestScheduler::reacquire_mutex(GuestThread& thread,
                                    std::uint64_t lockAddress,
                                    std::int32_t result) {
    const auto found = mutexes_.find(lockAddress);
    if (found == mutexes_.end()) {
        throw std::runtime_error("Invalid guest condition mutex");
    }
    thread.wakeError = result;
    const auto outcome = found->second.lock(thread.id, false);
    switch (outcome.status) {
        case GuestMutex::LockStatus::Acquired:
            wake(thread.id, result);
            break;
        case GuestMutex::LockStatus::Wait:
            thread.wait = GuestMutexWait{lockAddress};
            break;
        case GuestMutex::LockStatus::Error:
            throw std::runtime_error("Guest condition could not reacquire its mutex");
    }
}

GuestConditionWaitResult GuestScheduler::wait_condition(
    std::uint64_t condition, std::uint64_t mutex,
    std::optional<std::uint64_t> deadline) {
    auto waiters = conditionWaiters_.find(condition);
    const auto lock = mutexes_.find(mutex);
    if (waiters == conditionWaiters_.end()) return {EINVAL, false};
    if (lock == mutexes_.end()) return {EINVAL, false};
    if (lock->second.owner() != currentId_ || lock->second.depth() != 1) {
        return {EPERM, false};
    }
    auto& thread = current();
    if (thread.wait.has_value()) throw std::logic_error("Current guest thread is already waiting");
    waiters->second.push_back(currentId_);
    thread.wait = GuestConditionWait{condition, mutex, deadline};
    const auto unlockError = unlock_mutex(mutex, currentId_);
    if (unlockError != 0) {
        waiters->second.pop_back();
        thread.wait.reset();
        return {unlockError, false};
    }
    return {0, true};
}

std::int32_t GuestScheduler::signal_condition(std::uint64_t condition,
                                             bool broadcast,
                                             std::size_t* transferred) {
    const auto found = conditionWaiters_.find(condition);
    if (found == conditionWaiters_.end()) return EINVAL;
    auto& waiters = found->second;
    const std::size_t count = broadcast ? waiters.size() : std::min<std::size_t>(1, waiters.size());
    std::vector<std::uint64_t> selected(waiters.begin(), waiters.begin() + static_cast<std::ptrdiff_t>(count));
    for (const auto id : selected) {
        auto* thread = find(id);
        if (thread == nullptr || !thread->wait.has_value()) {
            throw std::logic_error("Invalid guest condition waiter");
        }
        const auto* wait = std::get_if<GuestConditionWait>(&*thread->wait);
        if (wait == nullptr || wait->address != condition) {
            throw std::logic_error("Guest condition waiter state does not match queue");
        }
        const auto lockAddress = wait->mutexAddress;
        reacquire_mutex(*thread, lockAddress, 0);
    }
    waiters.erase(waiters.begin(), waiters.begin() + static_cast<std::ptrdiff_t>(count));
    if (transferred != nullptr) *transferred = count;
    return 0;
}

std::int32_t GuestScheduler::create_semaphore(std::uint64_t name,
                                              std::int32_t policy,
                                              std::int32_t value) {
    if (name == 0 || value < 0 || (policy != 0 && policy != 2)) {
        return KERN_INVALID_ARGUMENT;
    }
    if (!semaphores_.emplace(name, SemaphoreState{
            policy, static_cast<std::uint64_t>(value), {}}).second) {
        return KERN_INVALID_ARGUMENT;
    }
    return KERN_SUCCESS;
}

std::int32_t GuestScheduler::destroy_semaphore(std::uint64_t name) {
    const auto found = semaphores_.find(name);
    if (found == semaphores_.end()) return KERN_INVALID_CAPABILITY;
    const auto waiters = found->second.waiters;
    semaphores_.erase(found);
    for (const auto id : waiters) {
        const auto* thread = find(id);
        if (thread == nullptr || !thread->wait.has_value()) {
            throw std::logic_error("Invalid guest semaphore waiter");
        }
        const auto* wait = std::get_if<GuestSemaphoreWait>(&*thread->wait);
        if (wait == nullptr || wait->semaphore != name) {
            throw std::logic_error("Guest semaphore waiter state does not match queue");
        }
        wake(id, KERN_TERMINATED);
    }
    return KERN_SUCCESS;
}

GuestSemaphoreWaitResult GuestScheduler::wait_semaphore(
    std::uint64_t name, std::optional<std::uint64_t> deadline, bool nonblocking,
    bool bsd) {
    const auto found = semaphores_.find(name);
    if (found == semaphores_.end()) return {KERN_INVALID_CAPABILITY, false};
    if (found->second.value > 0) {
        --found->second.value;
        return {KERN_SUCCESS, false};
    }
    if (nonblocking) return {KERN_OPERATION_TIMED_OUT, false};
    auto& thread = current();
    if (thread.wait.has_value()) return {KERN_ALREADY_WAITING, false};
    found->second.waiters.push_back(currentId_);
    thread.wait = GuestSemaphoreWait{name, deadline, bsd};
    return {KERN_SUCCESS, true};
}

std::int32_t GuestScheduler::signal_semaphore(std::uint64_t name) {
    const auto found = semaphores_.find(name);
    if (found == semaphores_.end()) return KERN_INVALID_CAPABILITY;
    auto& semaphore = found->second;
    if (!semaphore.waiters.empty()) {
        const auto index = (semaphore.policy & 2) != 0
            ? semaphore.waiters.size() - 1 : 0;
        const auto id = semaphore.waiters[index];
        auto* thread = find(id);
        if (thread == nullptr || !thread->wait.has_value()) {
            throw std::logic_error("Invalid guest semaphore waiter");
        }
        const auto* wait = std::get_if<GuestSemaphoreWait>(&*thread->wait);
        if (wait == nullptr || wait->semaphore != name) {
            throw std::logic_error("Guest semaphore waiter state does not match queue");
        }
        semaphore.waiters.erase(semaphore.waiters.begin() +
                                 static_cast<std::ptrdiff_t>(index));
        wake(id, KERN_SUCCESS);
        return KERN_SUCCESS;
    }
    if (semaphore.value == static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max())) {
        return KERN_RESOURCE_SHORTAGE;
    }
    ++semaphore.value;
    return KERN_SUCCESS;
}

std::int32_t GuestScheduler::signal_all_semaphore(std::uint64_t name) {
    const auto found = semaphores_.find(name);
    if (found == semaphores_.end()) return KERN_INVALID_CAPABILITY;
    const auto waiters = std::move(found->second.waiters);
    found->second.waiters.clear();
    for (const auto id : waiters) {
        const auto* thread = find(id);
        if (thread == nullptr || !thread->wait.has_value()) {
            throw std::logic_error("Invalid guest semaphore waiter");
        }
        const auto* wait = std::get_if<GuestSemaphoreWait>(&*thread->wait);
        if (wait == nullptr || wait->semaphore != name) {
            throw std::logic_error("Guest semaphore waiter state does not match queue");
        }
        wake(id, KERN_SUCCESS);
    }
    return KERN_SUCCESS;
}

std::int32_t GuestScheduler::signal_semaphore_thread(
    std::uint64_t name, std::optional<std::uint64_t> target) {
    constexpr std::int32_t kernNotWaiting = 48;
    const auto found = semaphores_.find(name);
    if (found == semaphores_.end()) return KERN_INVALID_CAPABILITY;
    auto& waiters = found->second.waiters;
    if (waiters.empty()) return kernNotWaiting;
    auto chosen = waiters.end();
    if (target.has_value()) {
        chosen = std::find(waiters.begin(), waiters.end(), *target);
        if (chosen == waiters.end()) return kernNotWaiting;
    } else {
        chosen = (found->second.policy & 2) != 0 ? waiters.end() - 1 : waiters.begin();
    }
    const auto id = *chosen;
    waiters.erase(chosen);
    wake(id, KERN_SUCCESS);
    return KERN_SUCCESS;
}

bool GuestScheduler::sleep_current(std::uint64_t deadline,
                                   std::optional<std::uint64_t> remainder) {
    auto& thread = current();
    if (thread.wait.has_value()) return false;
    thread.wait = GuestSleepWait{deadline, remainder};
    return true;
}

bool GuestScheduler::wait_ulock(std::uint64_t address,
                                std::optional<std::uint64_t> deadline, bool noErrno) {
    auto& thread = current();
    if (thread.wait.has_value()) return false;
    thread.wait = GuestUlockWait{address, deadline, noErrno};
    ulockWaiters_[address].push_back(currentId_);
    return true;
}

std::size_t GuestScheduler::wake_ulock(std::uint64_t address, bool all) {
    const auto found = ulockWaiters_.find(address);
    if (found == ulockWaiters_.end()) return 0;
    std::size_t woken = 0;
    while (!found->second.empty() && (all || woken == 0)) {
        const auto id = found->second.front();
        found->second.pop_front();
        wake(id, 0);
        ++woken;
    }
    if (found->second.empty()) ulockWaiters_.erase(found);
    return woken;
}

bool GuestScheduler::wait_mach_receive(std::uint32_t port,
                                       std::optional<std::uint64_t> deadline) {
    auto& thread = current();
    if (thread.wait.has_value()) return false;
    thread.wait = GuestMachReceiveWait{port, deadline};
    return true;
}

std::size_t GuestScheduler::expire_waiters(std::uint64_t nowNanoseconds) {
    std::size_t expired = 0;
    for (auto& entry : threads_) {
        auto& thread = entry.second;
        if (!thread.wait.has_value()) continue;
        if (const auto* sleep = std::get_if<GuestSleepWait>(&*thread.wait);
            sleep != nullptr && sleep->deadline <= nowNanoseconds) {
            wake(thread.id, 0);
            ++expired;
            continue;
        }
        constexpr std::int32_t machReceiveTimedOut = 0x10004003;
        const auto* machReceive = std::get_if<GuestMachReceiveWait>(&*thread.wait);
        if (machReceive != nullptr && machReceive->deadline.has_value() &&
            *machReceive->deadline <= nowNanoseconds) {
            wake(thread.id, machReceiveTimedOut);
            ++expired;
            continue;
        }
        if (const auto* ulock = std::get_if<GuestUlockWait>(&*thread.wait)) {
            if (!ulock->deadline.has_value() || *ulock->deadline > nowNanoseconds) continue;
            const auto queue = ulockWaiters_.find(ulock->address);
            if (queue != ulockWaiters_.end()) {
                auto& waiters = queue->second;
                waiters.erase(std::remove(waiters.begin(), waiters.end(), thread.id),
                              waiters.end());
                if (waiters.empty()) ulockWaiters_.erase(queue);
            }
            const bool noErrno = ulock->noErrno;
            wake(thread.id, noErrno ? -ETIMEDOUT : ETIMEDOUT);
            if (noErrno) {
                thread.registers[0] = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(-ETIMEDOUT));
            } else {
                thread.registers[32] |= 8U;
            }
            ++expired;
            continue;
        }
        const auto* condition = std::get_if<GuestConditionWait>(&*thread.wait);
        if (condition == nullptr) {
            const auto* semaphore = std::get_if<GuestSemaphoreWait>(&*thread.wait);
            if (semaphore == nullptr || !semaphore->deadline.has_value() ||
                *semaphore->deadline > nowNanoseconds) continue;
            const auto found = semaphores_.find(semaphore->semaphore);
            if (found != semaphores_.end()) {
                auto& waiters = found->second.waiters;
                waiters.erase(std::remove(waiters.begin(), waiters.end(), thread.id),
                              waiters.end());
            }
            if (semaphore->bsd) {
                wake(thread.id, ETIMEDOUT);
                thread.registers[32] |= 8U;
            } else {
                wake(thread.id, KERN_OPERATION_TIMED_OUT);
            }
            ++expired;
            continue;
        }
        if (!condition->deadline.has_value() || *condition->deadline > nowNanoseconds) continue;
        const auto address = condition->address;
        const auto lockAddress = condition->mutexAddress;
        const auto queue = conditionWaiters_.find(address);
        if (queue != conditionWaiters_.end()) {
            auto& waiters = queue->second;
            waiters.erase(std::remove(waiters.begin(), waiters.end(), thread.id), waiters.end());
        }
        reacquire_mutex(thread, lockAddress, ETIMEDOUT);
        ++expired;
    }
    return expired;
}

std::optional<std::uint64_t> GuestScheduler::earliest_deadline() const noexcept {
    std::optional<std::uint64_t> earliest;
    for (const auto& entry : threads_) {
        if (!entry.second.wait.has_value()) continue;
        std::optional<std::uint64_t> deadline;
        if (const auto* sleep = std::get_if<GuestSleepWait>(&*entry.second.wait)) {
            deadline = sleep->deadline;
        } else if (const auto* condition = std::get_if<GuestConditionWait>(&*entry.second.wait)) {
            deadline = condition->deadline;
        } else if (const auto* semaphore = std::get_if<GuestSemaphoreWait>(&*entry.second.wait)) {
            deadline = semaphore->deadline;
        } else if (const auto* ulock = std::get_if<GuestUlockWait>(&*entry.second.wait)) {
            deadline = ulock->deadline;
        } else if (const auto* machReceive =
                       std::get_if<GuestMachReceiveWait>(&*entry.second.wait)) {
            deadline = machReceive->deadline;
        } else if (const auto* hostPoll =
                       std::get_if<GuestHostPollWait>(&*entry.second.wait)) {
            deadline = hostPoll->nextPoll;
        }
        if (deadline.has_value() && (!earliest.has_value() || *deadline < *earliest)) {
            earliest = deadline;
        }
    }
    return earliest;
}

std::optional<GuestSleepInterruption> GuestScheduler::interrupt_sleeping_thread(
    std::uint32_t incomingSignals, const GuestSignals& signals,
    std::uint64_t nowNanoseconds) const noexcept {
    constexpr std::int32_t ignoredByDefault[] = {SIGCHLD, SIGURG, SIGWINCH, SIGINFO};
    for (const auto& entry : threads_) {
        const auto& thread = entry.second;
        const auto* sleep = thread.wait.has_value()
            ? std::get_if<GuestSleepWait>(&*thread.wait) : nullptr;
        if (sleep == nullptr) continue;
        const auto deliverable = incomingSignals & ~thread.signalMask;
        if (deliverable == 0) continue;
        const auto signalBit = deliverable & (~deliverable + 1U);
        const auto signal = static_cast<std::int32_t>(__builtin_ctz(signalBit) + 1);
        if (!GuestSignals::valid(signal)) continue;
        const auto action = signals.action(signal);
        if (action.handler == 1) continue;
        if (action.handler == 0) {
            bool ignored = false;
            for (const auto ignoredSignal : ignoredByDefault) {
                if (signal == ignoredSignal) {
                    ignored = true;
                    break;
                }
            }
            if (ignored) continue;
        }
        const auto remaining = sleep->deadline > nowNanoseconds
            ? sleep->deadline - nowNanoseconds : 0;
        return GuestSleepInterruption{thread.id,
                                      static_cast<std::uint64_t>(thread.errnoAddress),
                                      sleep->remainder, remaining, signal};
    }
    return std::nullopt;
}

} // namespace arm64metal
