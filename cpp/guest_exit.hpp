#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <vector>

namespace arm64metal {

enum class GuestExitHandlerKind { Standard, Cxa };

struct GuestExitHandler {
    std::uint64_t target = 0;
    std::uint64_t argument = 0;
    // The token passed to __cxa_finalize remains opaque and is not necessarily
    // the Mach-O header used by dyld image ownership.
    std::uint64_t dso = 0;
    std::uint64_t owner_image = 0;
    GuestExitHandlerKind kind = GuestExitHandlerKind::Standard;
};

// Process-owned bookkeeping only. The caller authenticates and validates the
// target before registration and owns callback execution and exit policy.
// Access must be serialized by the guest scheduler.
class GuestExitRegistry {
public:
    static constexpr std::size_t maximum_handlers = 4096;

    [[nodiscard]] bool register_handler(GuestExitHandler handler) {
        if (handler.target == 0 || handlers_.size() == maximum_handlers)
            return false;
        try {
            // Reserving a fixed maximum prevents vector growth from exceeding
            // the pending-handler bound. Failed allocation leaves state intact.
            if (handlers_.capacity() < maximum_handlers) handlers_.reserve(maximum_handlers);
            handlers_.push_back(handler);
        } catch (const std::bad_alloc&) {
            return false;
        }
        return true;
    }

    // Zero selects all DSOs, matching __cxa_finalize(nullptr). Remove before
    // returning so recursive finalization cannot invoke this handler twice.
    // Query again after every callback: it may register newer handlers.
    [[nodiscard]] std::optional<GuestExitHandler> take_next(std::uint64_t dso = 0) {
        for (std::size_t i = handlers_.size(); i != 0; --i) {
            if (dso != 0 && handlers_[i - 1].dso != dso) continue;
            const auto handler = handlers_[i - 1];
            handlers_.erase(handlers_.begin() + static_cast<std::ptrdiff_t>(i - 1));
            return handler;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<GuestExitHandler> take_next_for_image(
        std::uint64_t imageHeader) {
        if (imageHeader == 0) return std::nullopt;
        for (std::size_t i = handlers_.size(); i != 0; --i) {
            if (handlers_[i - 1].owner_image != imageHeader) continue;
            const auto handler = handlers_[i - 1];
            handlers_.erase(handlers_.begin() + static_cast<std::ptrdiff_t>(i - 1));
            return handler;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t size() const noexcept { return handlers_.size(); }
    [[nodiscard]] bool empty() const noexcept { return handlers_.empty(); }

private:
    std::vector<GuestExitHandler> handlers_;
};

}  // namespace arm64metal
