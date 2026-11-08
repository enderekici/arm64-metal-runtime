#include "guest_signal_runtime.hpp"

#include "darwin_bridge.hpp"
#include "process_host.hpp"

#include <signal.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <sys/ucontext.h>
#include <utility>

namespace arm64metal {
namespace {

constexpr std::uint64_t kSignalReturnSentinel = ~std::uint64_t{0};
constexpr std::size_t kGuestSiginfoSize = 104;
constexpr std::size_t kGuestUcontextSize = 56;
constexpr std::size_t kGuestMcontextSize = 816;
constexpr std::size_t kGuestMcontextThreadState = 16;
constexpr std::size_t kGuestMcontextNeonState = 288;
constexpr std::size_t kGuestMcontextExceptionSyndrome = 8;
constexpr std::size_t kGuestSiginfoAddress = 24;
constexpr std::size_t kGuestUcontextSignalMask = 4;
constexpr std::size_t kGuestUcontextMcontextPointer = 48;

static_assert(sizeof(void*) == 8);
static_assert(sizeof(siginfo_t) == kGuestSiginfoSize);
static_assert(offsetof(siginfo_t, si_addr) == kGuestSiginfoAddress);
static_assert(sizeof(ucontext_t) == kGuestUcontextSize);
static_assert(offsetof(ucontext_t, uc_sigmask) == kGuestUcontextSignalMask);
static_assert(offsetof(ucontext_t, uc_mcontext) == kGuestUcontextMcontextPointer);
static_assert(sizeof(*static_cast<ucontext_t*>(nullptr)->uc_mcontext) == kGuestMcontextSize);
static_assert(offsetof(_STRUCT_MCONTEXT, __ss.__pc) ==
              kGuestMcontextThreadState + 256);
static_assert(offsetof(_STRUCT_MCONTEXT, __ss.__cpsr) ==
              kGuestMcontextThreadState + 264);
static_assert(offsetof(_STRUCT_MCONTEXT, __es.__esr) ==
              kGuestMcontextExceptionSyndrome);
static_assert(offsetof(_STRUCT_MCONTEXT, __ns) == kGuestMcontextNeonState);

std::uint64_t aligned_down(std::uint64_t value, std::uint64_t alignment) {
    return value & ~(alignment - 1U);
}

void write_context_integer(std::uint8_t* bytes, std::size_t offset,
                           const void* value, std::size_t size) {
    std::memcpy(bytes + offset, value, size);
}

std::uint32_t cpsr_from_guest_flags(std::uint64_t flags) noexcept {
    return static_cast<std::uint32_t>(((flags >> 1U) & 1U) << 31U |
                                      (flags & 1U) << 30U |
                                      ((flags >> 3U) & 1U) << 29U |
                                      ((flags >> 2U) & 1U) << 28U);
}

std::uint64_t guest_flags_from_cpsr(std::uint32_t cpsr) noexcept {
    return ((cpsr >> 30U) & 1U) | (((cpsr >> 31U) & 1U) << 1U) |
           (((cpsr >> 28U) & 1U) << 2U) | (((cpsr >> 29U) & 1U) << 3U);
}

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
    std::uint64_t authenticationDepth, std::uint64_t executionContext,
    std::uint64_t threadId, GuestMemory* memory) {
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
        if ((action.flags & SA_SIGINFO) != 0) {
            if (memory == nullptr)
                throw std::runtime_error("Guest SA_SIGINFO delivery requires guest memory");
            return deliver_synchronous(signals, threadMask, registers, *memory,
                GuestSynchronousFault{signal, SI_USER, 0, registers[guestSignalPC]},
                authenticationDepth, executionContext, threadId, &action);
        }
        auto& frames = framesByThread_[threadId];
        if (frames.size() >= guestSignalFrameLimit) {
            throw std::runtime_error("Guest signal nesting limit exceeded");
        }

        Frame frame;
        frame.registers.assign(registers.begin(), registers.begin() + guestSignalGPRCount);
        frame.vectors.assign(registers.begin() + guestSignalVectorStart,
                             registers.begin() + guestSignalVectorEnd);
        frame.signalMask = signals.mask();
        frame.authenticationDepth = authenticationDepth;
        frame.executionContext = executionContext;
        frames.push_back(std::move(frame));

        signals.enter(signal, action);
        threadMask = signals.mask();
        registers[0] = static_cast<std::uint64_t>(signal);
        registers[guestSignalLR] = kSignalReturnSentinel;
        registers[guestSignalPC] = action.handler;
        return {GuestSignalOutcome::HandlerEntered, signal, 0, action.handler};
    }
}

GuestSignalDelivery GuestSignalRuntime::deliver_synchronous(
    GuestSignals& signals, std::uint32_t& threadMask,
    std::vector<std::uint64_t>& registers, GuestMemory& memory,
    const GuestSynchronousFault& fault, std::uint64_t authenticationDepth,
    std::uint64_t executionContext, std::uint64_t threadId,
    const GuestSignals::Action* selectedAction) {
    validate_registers(registers);
    if (!GuestSignals::valid(fault.signal))
        throw std::invalid_argument("Invalid synchronous guest signal");
    signals.set_mask_value(threadMask);
    const auto action = selectedAction == nullptr
        ? signals.action(fault.signal) : *selectedAction;
    // Synchronous faults cannot be ignored: resuming at the same instruction
    // would immediately fault again.
    if (action.handler <= 1) {
        return {GuestSignalOutcome::ProcessExit, fault.signal,
                128 + fault.signal, 0};
    }
    if ((action.flags & SA_SIGINFO) == 0) {
        signals.raise(fault.signal);
        std::uint32_t processPending = 0;
        std::uint32_t threadPending = 0;
        auto delivered = deliver(signals, processPending, threadPending, threadMask,
                                 registers, authenticationDepth, executionContext,
                                 threadId);
        return delivered;
    }
    auto& frames = framesByThread_[threadId];
    if (frames.size() >= guestSignalFrameLimit)
        throw std::runtime_error("Guest signal nesting limit exceeded");

    constexpr auto faultStateSize = kGuestSiginfoSize + kGuestUcontextSize +
                                    kGuestMcontextSize;
    const auto sp = registers[31];
    if (sp < faultStateSize + 16U)
        throw std::runtime_error("Guest stack cannot hold synchronous signal context");
    const auto siginfoAddress = aligned_down(sp - kGuestSiginfoSize, 16);
    const auto ucontextAddress = aligned_down(siginfoAddress - kGuestUcontextSize, 16);
    const auto mcontextAddress = aligned_down(ucontextAddress - kGuestMcontextSize, 16);

    auto* siginfo = memory.buffer(siginfoAddress, kGuestSiginfoSize, true);
    auto* ucontext = memory.buffer(ucontextAddress, kGuestUcontextSize, true);
    auto* mcontext = memory.buffer(mcontextAddress, kGuestMcontextSize, true);
    std::memset(siginfo, 0, kGuestSiginfoSize);
    std::memset(ucontext, 0, kGuestUcontextSize);
    std::memset(mcontext, 0, kGuestMcontextSize);
    write_context_integer(siginfo, 0, &fault.signal, sizeof(fault.signal));
    write_context_integer(siginfo, 8, &fault.code, sizeof(fault.code));
    write_context_integer(siginfo, kGuestSiginfoAddress, &fault.address,
                          sizeof(fault.address));
    write_context_integer(ucontext, kGuestUcontextSignalMask, &threadMask,
                          sizeof(threadMask));
    const std::uint64_t machineContextSize = kGuestMcontextSize;
    write_context_integer(ucontext, 40, &machineContextSize, sizeof(machineContextSize));
    write_context_integer(ucontext, kGuestUcontextMcontextPointer, &mcontextAddress,
                          sizeof(mcontextAddress));
    write_context_integer(mcontext, kGuestMcontextExceptionSyndrome,
                          &fault.exceptionSyndrome, sizeof(fault.exceptionSyndrome));
    for (std::size_t index = 0; index < 29; ++index) {
        write_context_integer(mcontext, kGuestMcontextThreadState + index * 8,
                              &registers[index], sizeof(std::uint64_t));
    }
    for (std::size_t index = 29; index <= 31; ++index) {
        const auto stateOffset = kGuestMcontextThreadState + 232 + (index - 29) * 8;
        write_context_integer(mcontext, stateOffset, &registers[index],
                              sizeof(std::uint64_t));
    }
    write_context_integer(mcontext, kGuestMcontextThreadState + 256,
                          &registers[guestSignalPC], sizeof(std::uint64_t));
    const auto cpsr = cpsr_from_guest_flags(registers[32]);
    write_context_integer(mcontext, kGuestMcontextThreadState + 264,
                          &cpsr, sizeof(cpsr));
    std::memcpy(mcontext + kGuestMcontextNeonState,
                registers.data() + guestSignalVectorStart,
                (guestSignalVectorEnd - guestSignalVectorStart) * sizeof(std::uint64_t));

    Frame frame;
    frame.registers.assign(registers.begin(), registers.begin() + guestSignalGPRCount);
    frame.vectors.assign(registers.begin() + guestSignalVectorStart,
                         registers.begin() + guestSignalVectorEnd);
    frame.signalMask = signals.mask();
    frame.authenticationDepth = authenticationDepth;
    frame.executionContext = executionContext;
    frame.synchronousContext = ucontextAddress;
    frames.push_back(std::move(frame));

    if ((action.flags & SA_RESETHAND) != 0) {
        signals.set_action(GuestSignals::Action{}, fault.signal);
    }
    signals.enter(fault.signal, action);
    threadMask = signals.mask();
    registers[0] = static_cast<std::uint64_t>(fault.signal);
    registers[1] = siginfoAddress;
    registers[2] = ucontextAddress;
    registers[31] = mcontextAddress;
    registers[guestSignalLR] = kSignalReturnSentinel;
    registers[guestSignalPC] = action.handler;
    return {GuestSignalOutcome::HandlerEntered, fault.signal, 0, action.handler};
}

std::optional<GuestSignalResume> GuestSignalRuntime::resume(
    GuestSignals& signals, std::uint32_t& threadMask,
    std::vector<std::uint64_t>& registers,
    std::uint64_t authenticationDepth,
    std::uint64_t threadId,
    const std::function<std::optional<std::uint64_t>(std::uint64_t)>&
        restartableRecovery, GuestMemory* memory) {
    const auto stack = framesByThread_.find(threadId);
    if (stack == framesByThread_.end() || stack->second.empty()) return std::nullopt;
    validate_registers(registers);
    Frame frame = std::move(stack->second.back());
    stack->second.pop_back();
    if (stack->second.empty()) framesByThread_.erase(stack);
    if (authenticationDepth != frame.authenticationDepth) {
        throw std::runtime_error("Guest signal handler left its authentication stack unbalanced");
    }
    std::copy(frame.registers.begin(), frame.registers.end(), registers.begin());
    std::copy(frame.vectors.begin(), frame.vectors.end(),
              registers.begin() + guestSignalVectorStart);
    bool recompile = false;
    if (frame.synchronousContext != 0 && memory != nullptr) {
        const auto* ucontext = memory->buffer(frame.synchronousContext, kGuestUcontextSize);
        std::uint64_t mcontextAddress = 0;
        std::memcpy(&mcontextAddress, ucontext + kGuestUcontextMcontextPointer,
                    sizeof(mcontextAddress));
        const auto* mcontext = memory->buffer(mcontextAddress, kGuestMcontextSize);
        for (std::size_t index = 0; index < 29; ++index) {
            std::memcpy(&registers[index], mcontext + kGuestMcontextThreadState + index * 8,
                        sizeof(std::uint64_t));
        }
        for (std::size_t index = 29; index <= 31; ++index) {
            const auto stateOffset = kGuestMcontextThreadState + 232 + (index - 29) * 8;
            std::memcpy(&registers[index], mcontext + stateOffset, sizeof(std::uint64_t));
        }
        std::memcpy(&registers[guestSignalPC],
                    mcontext + kGuestMcontextThreadState + 256, sizeof(std::uint64_t));
        std::uint32_t cpsr = 0;
        std::memcpy(&cpsr, mcontext + kGuestMcontextThreadState + 264, sizeof(cpsr));
        registers[32] = guest_flags_from_cpsr(cpsr);
        recompile = registers[guestSignalPC] != frame.registers[guestSignalPC];
        std::memcpy(registers.data() + guestSignalVectorStart,
                    mcontext + kGuestMcontextNeonState,
                    (guestSignalVectorEnd - guestSignalVectorStart) * sizeof(std::uint64_t));
        std::memcpy(&frame.signalMask, ucontext + kGuestUcontextSignalMask,
                    sizeof(frame.signalMask));
    }
    signals.set_mask_value(frame.signalMask);
    threadMask = signals.mask();
    if (restartableRecovery) {
        const auto recovery = restartableRecovery(registers[guestSignalPC]);
        if (recovery.has_value()) {
            registers[guestSignalPC] = *recovery;
            recompile = true;
        }
    }
    return GuestSignalResume{registers[guestSignalPC], frame.executionContext, recompile};
}

bool GuestSignalRuntime::has_frames(std::uint64_t threadId) const noexcept {
    const auto found = framesByThread_.find(threadId);
    return found != framesByThread_.end() && !found->second.empty();
}

std::size_t GuestSignalRuntime::frame_depth(std::uint64_t threadId) const noexcept {
    const auto found = framesByThread_.find(threadId);
    return found == framesByThread_.end() ? 0 : found->second.size();
}

void GuestSignalRuntime::discard_thread(std::uint64_t threadId) noexcept {
    framesByThread_.erase(threadId);
}

} // namespace arm64metal
