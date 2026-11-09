#include "guest_signal_runtime.hpp"
#include "process_host.hpp"

#include <cassert>
#include <csignal>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace arm64metal {
std::uint32_t guest_signal_take() noexcept { return 0; }

GuestMemory::GuestMemory(std::vector<std::uint8_t> bytes,
                         std::vector<GuestMemoryRange> readOnlyRanges,
                         std::vector<GuestMemoryMapping> mappings)
    : bytes_(std::move(bytes)), data_(bytes_.data()), size_(bytes_.size()),
      readOnlyRanges_(std::move(readOnlyRanges)), mappings_(std::move(mappings)) {}

GuestMemory::GuestMemory(std::uint8_t* bytes, std::size_t size,
                         std::vector<GuestMemoryRange> readOnlyRanges,
                         std::vector<GuestMemoryMapping> mappings)
    : data_(bytes), size_(size), external_(true),
      readOnlyRanges_(std::move(readOnlyRanges)), mappings_(std::move(mappings)) {}

std::uint8_t* GuestMemory::buffer(std::uint64_t address, std::uint64_t length,
                                 bool writable) {
    if (address > size_ || length > size_ - address)
        throw std::out_of_range("test guest memory interval out of range");
    if (writable) {
        for (const auto& range : readOnlyRanges_) {
            if (address < static_cast<std::uint64_t>(range.end) &&
                static_cast<std::uint64_t>(range.begin) < address + length)
                throw std::out_of_range("test guest memory is read-only");
        }
    }
    return data_ + address;
}

const std::uint8_t* GuestMemory::buffer(std::uint64_t address,
                                       std::uint64_t length) const {
    if (address > size_ || length > size_ - address)
        throw std::out_of_range("test guest memory interval out of range");
    return data_ + address;
}
} // namespace arm64metal

int main() {
    using namespace arm64metal;
    GuestSignalRuntime runtime;
    GuestSignals signals;
    GuestSignals::Action action;
    action.handler = 0x4000;
    signals.set_action(action, SIGUSR1);

    {
        GuestSignalRuntime fastRuntime;
        GuestSignalRuntime directRuntime;
        GuestSignals fastSignals;
        fastSignals.set_action(action, SIGUSR1);
        fastSignals.raise(SIGUSR1);
        auto directSignals = fastSignals;
        std::vector<std::uint64_t> fastRegisters(guestSignalVectorEnd, 0);
        auto directRegisters = fastRegisters;
        auto processPending = GuestSignals::bit(SIGUSR1);
        auto directProcessPending = processPending;
        std::uint32_t threadPending = 0;
        auto directThreadPending = threadPending;
        auto threadMask = GuestSignals::bit(SIGUSR1) |
                          GuestSignals::bit(SIGKILL) | GuestSignals::bit(SIGSTOP);
        auto directThreadMask = threadMask;

        assert(!fastRuntime.has_pending_delivery(fastSignals, threadPending,
            threadMask, fastRegisters.size()));
        const auto directResult = directRuntime.deliver(directSignals,
            directProcessPending, directThreadPending, directThreadMask,
            directRegisters, 0, 0, 1);
        assert(directResult.outcome == GuestSignalOutcome::None);
        assert(threadMask == directThreadMask && threadMask == GuestSignals::bit(SIGUSR1));
        assert(processPending == directProcessPending &&
               threadPending == directThreadPending && fastRegisters == directRegisters);

        threadMask = 0;
        directThreadMask = 0;
        assert(fastRuntime.has_pending_delivery(fastSignals, threadPending,
            threadMask, fastRegisters.size()));
        const auto fastResult = fastRuntime.deliver(fastSignals, processPending,
            threadPending, threadMask, fastRegisters, 0, 0, 1);
        const auto unmaskedDirectResult = directRuntime.deliver(directSignals,
            directProcessPending, directThreadPending, directThreadMask,
            directRegisters, 0, 0, 1);
        assert(fastResult.outcome == unmaskedDirectResult.outcome &&
               fastResult.outcome == GuestSignalOutcome::HandlerEntered);
        assert(threadMask == directThreadMask && processPending == directProcessPending &&
               threadPending == directThreadPending && fastRegisters == directRegisters);
    }

    std::vector<std::uint64_t> first(guestSignalVectorEnd, 0);
    std::vector<std::uint64_t> second(guestSignalVectorEnd, 0);
    first[guestSignalPC] = 0x1110;
    second[guestSignalPC] = 0x2220;
    first[37] = 3;
    second[37] = 5;
    std::uint32_t processPending = 0;
    std::uint32_t firstPending = GuestSignals::bit(SIGUSR1);
    std::uint32_t secondPending = GuestSignals::bit(SIGUSR1);
    std::uint32_t firstMask = 0;
    std::uint32_t secondMask = 0;

    assert(runtime.deliver(signals, processPending, firstPending, firstMask,
        first, 3, 0xa1, 1).outcome == GuestSignalOutcome::HandlerEntered);
    assert(runtime.deliver(signals, processPending, secondPending, secondMask,
        second, 5, 0xb2, 2).outcome == GuestSignalOutcome::HandlerEntered);
    assert(runtime.frame_depth(1) == 1 && runtime.frame_depth(2) == 1);

    const auto secondResume = runtime.resume(signals, secondMask, second, 5, 2);
    assert(secondResume && secondResume->programCounter == 0x2220 &&
           secondResume->executionContext == 0xb2);
    assert(runtime.has_frames(1) && !runtime.has_frames(2));
    const auto firstResume = runtime.resume(signals, firstMask, first, 3, 1);
    assert(firstResume && firstResume->programCounter == 0x1110 &&
           firstResume->executionContext == 0xa1);
    assert(!runtime.has_frames(1) && !runtime.has_frames(2));

    GuestSignalRuntime synchronousRuntime;
    GuestSignals synchronousSignals;
    GuestSignals::Action infoAction;
    infoAction.handler = 0x5000;
    infoAction.flags = SA_SIGINFO;
    GuestMemory memory(std::vector<std::uint8_t>(16U * 1024U));
    first.assign(guestSignalVectorEnd, 0);
    second.assign(guestSignalVectorEnd, 0);
    first[guestSignalPC] = 0x3330;
    second[guestSignalPC] = 0x4440;
    first[31] = 4096;
    second[31] = 8192;
    first[37] = 7;
    second[37] = 9;
    firstMask = 0;
    secondMask = 0;
    const GuestSynchronousFault firstFault{SIGSEGV, 1, 0xabc, 0x3330};
    const GuestSynchronousFault secondFault{SIGBUS, 2, 0xdef, 0x4440};
    assert(synchronousRuntime.deliver_synchronous(synchronousSignals, firstMask,
        first, memory, firstFault, 7, 0xc1, 1, &infoAction).outcome ==
        GuestSignalOutcome::HandlerEntered);
    assert(synchronousRuntime.deliver_synchronous(synchronousSignals, secondMask,
        second, memory, secondFault, 9, 0xd2, 2, &infoAction).outcome ==
        GuestSignalOutcome::HandlerEntered);
    assert(synchronousRuntime.has_frames(1) && synchronousRuntime.has_frames(2));
    const auto syncSecond = synchronousRuntime.resume(synchronousSignals,
        secondMask, second, 9, 2, {}, &memory);
    const auto syncFirst = synchronousRuntime.resume(synchronousSignals,
        firstMask, first, 7, 1, {}, &memory);
    assert(syncSecond && syncSecond->programCounter == 0x4440 &&
           syncSecond->executionContext == 0xd2);
    assert(syncFirst && syncFirst->programCounter == 0x3330 &&
           syncFirst->executionContext == 0xc1);
    assert(!synchronousRuntime.has_frames(1) && !synchronousRuntime.has_frames(2));

    firstPending = GuestSignals::bit(SIGUSR1);
    assert(runtime.deliver(signals, processPending, firstPending, firstMask,
        first, 3, 0xe3, 1).outcome == GuestSignalOutcome::HandlerEntered);
    runtime.discard_thread(1);
    assert(!runtime.has_frames(1) && runtime.frame_depth(1) == 0);
    return 0;
}
