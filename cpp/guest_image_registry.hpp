#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace arm64metal {

struct GuestLoadedImage {
  std::uint64_t header = 0;
  std::string name;
  bool cache_backed = false;
};

// Ordered snapshot of images published to the guest dyld interface. A
// transaction makes a staged dlopen visible only after its image publication
// has completed.
class GuestImageRegistry {
 public:
  class Transaction {
   public:
    explicit Transaction(GuestImageRegistry& owner) : owner_(owner) {}
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void add(std::uint64_t header, std::string name, bool cacheBacked = false) {
      if (header == 0 || name.empty()) {
        throw std::invalid_argument("Guest image registry entry is incomplete");
      }
      if (owner_.indices_.find(header) != owner_.indices_.end() ||
          !stagedHeaders_.insert(header).second) return;
      staged_.push_back({header, std::move(name), cacheBacked});
    }

    void commit() {
      if (committed_) return;
      auto entries = owner_.entries_;
      auto indices = owner_.indices_;
      entries.reserve(entries.size() + staged_.size());
      indices.reserve(indices.size() + staged_.size());
      for (const auto& entry : staged_) {
        if (indices.emplace(entry.header, entries.size()).second) entries.push_back(entry);
      }
      owner_.entries_.swap(entries);
      owner_.indices_.swap(indices);
      committed_ = true;
    }

   private:
    GuestImageRegistry& owner_;
    std::vector<GuestLoadedImage> staged_;
    std::unordered_set<std::uint64_t> stagedHeaders_;
    bool committed_ = false;
  };

  GuestImageRegistry() = default;
  explicit GuestImageRegistry(const std::vector<GuestLoadedImage>& initial) {
    Transaction transaction(*this);
    for (const auto& entry : initial)
      transaction.add(entry.header, entry.name, entry.cache_backed);
    transaction.commit();
  }

  [[nodiscard]] Transaction begin_transaction() { return Transaction(*this); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  [[nodiscard]] std::optional<std::uint64_t> header_at(std::uint64_t index) const noexcept {
    if (index >= entries_.size()) return std::nullopt;
    return entries_[static_cast<std::size_t>(index)].header;
  }

  [[nodiscard]] std::optional<std::string_view> name_at(std::uint64_t index) const noexcept {
    if (index >= entries_.size()) return std::nullopt;
    return entries_[static_cast<std::size_t>(index)].name;
  }

  [[nodiscard]] bool cache_backed_at(std::uint64_t index) const noexcept {
    return index < entries_.size() && entries_[static_cast<std::size_t>(index)].cache_backed;
  }

  [[nodiscard]] bool contains(std::uint64_t header) const noexcept {
    return indices_.find(header) != indices_.end();
  }

 private:
  std::vector<GuestLoadedImage> entries_;
  std::unordered_map<std::uint64_t, std::size_t> indices_;
};

inline std::uint64_t guest_image_vmaddr_slide(
    const GuestImageRegistry& registry, std::uint64_t index,
    const std::unordered_map<std::uint64_t, std::uint64_t>& imageSlides) {
  const auto header = registry.header_at(index);
  if (!header) return 0;
  const auto slide = imageSlides.find(*header);
  if (slide == imageSlides.end()) {
    throw std::logic_error("Published guest image has no slide metadata");
  }
  return slide->second;
}

}  // namespace arm64metal
