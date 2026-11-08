#pragma once

#include "guest_state.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace arm64metal {

inline constexpr std::size_t guestSignalFrameLimit = 32;
inline constexpr std::size_t guestSignalPC = 33;
inline constexpr std::size_t guestSignalLR = 30;
inline constexpr std::size_t guestSignalGPRCount = 36;
inline constexpr std::size_t guestSignalVectorStart = 806;
inline constexpr std::size_t guestSignalVectorEnd = 872;

enum class GuestSignalOutcome {
    None,
    HandlerEntered,
    ProcessExit,
    UnsupportedStop
};

struct GuestSignalDelivery {
    GuestSignalOutcome outcome = GuestSignalOutcome::None;
    std::int32_t signal = 0;
    std::int32_t exitStatus = 0;
    std::uint64_t handler = 0;
};

struct GuestSignalResume {
    std::uint64_t programCounter = 0;
    std::uint64_t executionContext = 0;
    bool recompile = false;
};

// Owns signal frames only. GuestSignals owns process actions/pending state;
// the scheduler owns thread masks, Metal execution, and executable regions.
class GuestSignalRuntime final {
public:
    // Merge host-raised bits into GuestSignals and return bits that the caller
    // should also publish to its process-wakeup mask. The caller obtains bits
    // from guest_signal_take() in the Darwin bridge.
    [[nodiscard]] static std::uint32_t ingest_host_signals(
        GuestSignals& signals, std::uint32_t incoming);

    // Alternative queue consumer for standalone callers. Drain the async-safe
    // Darwin bridge queue, merge bits into process pending state, and return
    // new wakeup bits. Do not call alongside another guest_signal_take owner.
    [[nodiscard]] static std::uint32_t poll_host_signal_queue(GuestSignals& signals);

    [[nodiscard]] GuestSignalDelivery deliver(
        GuestSignals& signals, std::uint32_t& processPending,
        std::uint32_t& threadPending, std::uint32_t& threadMask,
        std::vector<std::uint64_t>& registers,
        std::uint64_t authenticationDepth, std::uint64_t executionContext);

    // Call when the Metal runner reports the handler's sentinel return.
    // Returns the saved PC/context for the scheduler to resume or recompile.
    [[nodiscard]] std::optional<GuestSignalResume> resume(
        GuestSignals& signals, std::uint32_t& threadMask,
        std::vector<std::uint64_t>& registers,
        std::uint64_t authenticationDepth,
        const std::function<std::optional<std::uint64_t>(std::uint64_t)>&
            restartableRecovery = {});

    [[nodiscard]] bool has_frames() const noexcept { return !frames_.empty(); }
    [[nodiscard]] std::size_t frame_depth() const noexcept { return frames_.size(); }

private:
    struct Frame {
        std::vector<std::uint64_t> registers;
        std::vector<std::uint64_t> vectors;
        std::uint32_t signalMask = 0;
        std::uint64_t authenticationDepth = 0;
        std::uint64_t executionContext = 0;
    };
    std::vector<Frame> frames_;
};

} // namespace arm64metal
