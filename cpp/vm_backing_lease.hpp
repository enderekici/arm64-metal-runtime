#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace arm64metal {

class VmBackingLease {
 public:
  std::int64_t backing = 0;
  std::uint64_t size = 0;

  VmBackingLease() = default;

  VmBackingLease(std::int64_t backing, std::uint64_t size,
                 std::function<void()> release)
      : backing(backing),
        size(size),
        state_(std::make_shared<State>(std::move(release))) {}

  void release_now() const {
    if (state_) state_->release_now();
  }

  long reference_count() const { return state_ ? state_.use_count() : 0; }

  void rearm_release(std::function<void()> release) const {
    if (!state_ || state_->release) {
      throw std::logic_error("Guest VM backing lease is not released");
    }
    state_->release = std::move(release);
  }

 private:
  struct State {
    explicit State(std::function<void()> release) : release(std::move(release)) {}

    ~State() { release_now(); }

    void release_now() {
      if (!release) return;
      auto callback = std::exchange(release, {});
      if (callback) callback();
    }

    std::function<void()> release;
  };

  std::shared_ptr<State> state_;
};

}  // namespace arm64metal
