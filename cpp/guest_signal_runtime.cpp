#include "guest_signal_runtime.hpp"

#include "darwin_bridge.hpp"

#include <signal.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace arm64metal {
namespace {

constexpr std::uint64_t kSignalReturnSentinel = ~std::uint64_t{0};

bool ignored_by_default(std::int32_t signal) noexcept {
    return signal == SIGCHLD || signal == SIGURG || signal == SIGWINCH || signal == SIGINFO;
}

bool unsupported_default_stop(std::int32_t signal) noexcept {
    return signal == SIGSTOP || signal == SIGTSTP || signal == SIGTTIN ||
           signal == SIGTTOU || signal == SIGCONT;
}

void validate_registers(const std::vector<std::uint64_t>& registers) {
    if (registers.size() < guestSignalVectorEnd) {
        throw std::invalid_argument("Guest signal register file is incomplete");
    }
}

} // namespace

std::uint32_t GuestSignalRuntime::ingest_host_signals(
    GuestSignals& signals, std::uint32_t incoming) {
    signals.ingest(incoming);
    std::uint32_t processPending = 0;
    std::uint32_t remaining = incoming;
    while (remaining != 0) {
        const auto bitValue = remaining & (~remaining + 1U);
        remaining &= remaining - 1U;
        const auto signal = static_cast<std::int32_t>(__builtin_ctz(bitValue) + 1);
        if (GuestSignals::valid(signal) && signals.action(signal).handler != 1) {
            processPending |= bitValue;
        }
    }
    return processPending;
}

std::uint32_t GuestSignalRuntime::poll_host_signal_queue(GuestSignals& signals) {
    const auto incoming = guest_signal_take();
    return ingest_host_signals(signals, incoming);
}

GuestSignalDelivery GuestSignalRuntime::deliver(
    GuestSignals& signals, std::uint32_t& processPending,
    std::uint32_t& threadPending, std::uint32_t& threadMask,
    std::vector<std::uint64_t>& registers,
    std::uint64_t authenticationDepth, std::uint64_t executionContext) {
    validate_registers(registers);
    signals.set_mask_value(threadMask);
    while (true) {
        auto next = signals.next(threadPending);
        if (!next.has_value()) {
            threadMask = signals.mask();
            return {};
        }
        const auto signal = next->first;
        const auto action = next->second;
        processPending &= ~GuestSignals::bit(signal);
        if (action.handler == 1) continue;
        if (action.handler == 0) {
            if (ignored_by_default(signal)) continue;
            if (unsupported_default_stop(signal)) {
                return {GuestSignalOutcome::UnsupportedStop, signal, 0, 0};
            }
            threadMask = signals.mask();
            return {GuestSignalOutcome::ProcessExit, signal, 128 + signal, 0};
        }
        if (frames_.size() >= guestSignalFrameLimit) {
            throw std::runtime_error("Guest signal nesting limit exceeded");
        }

        Frame frame;
        frame.registers.assign(registers.begin(), registers.begin() + guestSignalGPRCount);
        frame.vectors.assign(registers.begin() + guestSignalVectorStart,
                             registers.begin() + guestSignalVectorEnd);
        frame.signalMask = signals.mask();
        frame.authenticationDepth = authenticationDepth;
        frame.executionContext = executionContext;
        frames_.push_back(std::move(frame));

        signals.enter(signal, action);
        threadMask = signals.mask();
        registers[0] = static_cast<std::uint64_t>(signal);
        registers[guestSignalLR] = kSignalReturnSentinel;
        registers[guestSignalPC] = action.handler;
        return {GuestSignalOutcome::HandlerEntered, signal, 0, action.handler};
    }
}

std::optional<GuestSignalResume> GuestSignalRuntime::resume(
    GuestSignals& signals, std::uint32_t& threadMask,
    std::vector<std::uint64_t>& registers,
    std::uint64_t authenticationDepth,
    const std::function<std::optional<std::uint64_t>(std::uint64_t)>&
        restartableRecovery) {
    if (frames_.empty()) return std::nullopt;
    validate_registers(registers);
    Frame frame = std::move(frames_.back());
    frames_.pop_back();
    if (authenticationDepth != frame.authenticationDepth) {
        throw std::runtime_error("Guest signal handler left its authentication stack unbalanced");
    }
    std::copy(frame.registers.begin(), frame.registers.end(), registers.begin());
    std::copy(frame.vectors.begin(), frame.vectors.end(),
              registers.begin() + guestSignalVectorStart);
    signals.set_mask_value(frame.signalMask);
    threadMask = signals.mask();
    bool recompile = false;
    if (restartableRecovery) {
        const auto recovery = restartableRecovery(registers[guestSignalPC]);
        if (recovery.has_value()) {
            registers[guestSignalPC] = *recovery;
            recompile = true;
        }
    }
    return GuestSignalResume{registers[guestSignalPC], frame.executionContext, recompile};
}

} // namespace arm64metal
